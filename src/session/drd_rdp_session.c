#include "session/drd_rdp_session.h"

#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/bitmap.h>
#include <freerdp/constants.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/crypto.h>
#include <freerdp/freerdp.h>
#include <freerdp/redirection.h>
#include <freerdp/update.h>

#include <gio/gio.h>
#include <string.h>

#include <winpr/crypto.h>
#include <winpr/stream.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/wtypes.h>

#include "core/drd_server_runtime.h"
#include "security/drd_local_session.h"
#include "session/drd_rdp_graphics_pipeline.h"
#include "utils/drd_capture_metrics.h"
#include "utils/drd_log.h"

#define ELEMENT_TYPE_CERTIFICATE 32

G_DEFINE_AUTOPTR_CLEANUP_FUNC(rdpCertificate, freerdp_certificate_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(rdpRedirection, redirection_free)

struct _DrdRdpSession
{
    GObject parent_instance;

    freerdp_peer *peer;
    gchar *peer_address;
    gchar *state;
    DrdServerRuntime *runtime;
    HANDLE vcm;
    GThread *vcm_thread;
    DrdRdpGraphicsPipeline *graphics_pipeline;
    gboolean graphics_pipeline_ready;
    guint32 frame_sequence;
    gint max_surface_payload;
    gboolean is_activated;
    gpointer system_client;
    gboolean connection_keep_open;
    GThread *event_thread;
    HANDLE stop_event;
    gint connection_alive;
    GThread *render_thread;
    gint render_running;
    DrdRdpSessionClosedFunc closed_cb;
    gpointer closed_cb_data;
    gint closed_cb_invoked;
    DrdLocalSession *local_session;
    gboolean passive_mode;
    DrdRdpSessionError last_error;
    guint64 frame_pull_errors;
    /* 拥塞降级相关的状态管理 */
    guint congestion_recovery_attempts; /* 尝试恢复 Rdpgfx 的次数 */
    gint64 congestion_disable_time; /* 上次禁用 Rdpgfx 的时间戳 */
    gboolean congestion_permanent_disabled; /* 是否永久禁用 */

    guint refresh_timeout_source; /* avc 切换时的全量帧定时器 */
    gint refresh_timeout_due; /* 定时器到期后由渲染线程消费 */
};

G_DEFINE_TYPE(DrdRdpSession, drd_rdp_session, G_TYPE_OBJECT)

static gpointer drd_rdp_session_vcm_thread(gpointer user_data);

static void drd_rdp_session_maybe_init_graphics(DrdRdpSession *self);

static void drd_rdp_session_disable_graphics_pipeline(DrdRdpSession *self, const gchar *reason);

static gboolean drd_rdp_session_enforce_peer_desktop_size(DrdRdpSession *self);

static gboolean drd_rdp_session_wait_for_graphics_capacity(DrdRdpSession *self, gint64 timeout_us);

static gboolean drd_rdp_session_start_render_thread(DrdRdpSession *self);

static void drd_rdp_session_stop_render_thread(DrdRdpSession *self);

static gpointer drd_rdp_session_render_thread(gpointer user_data);

static void drd_rdp_session_notify_closed(DrdRdpSession *self);

gboolean drd_rdp_session_client_is_mstsc(DrdRdpSession *self);

static void drd_rdp_session_refresh_surface_payload_limit(DrdRdpSession *self);

static WCHAR *drd_rdp_session_get_utf16_string(const char *str, size_t *size);

static WCHAR *drd_rdp_session_generate_redirection_guid(size_t *size);

static BYTE *drd_rdp_session_get_certificate_container(const char *certificate, size_t *size);

static gboolean drd_rdp_session_on_refresh_timeout(gpointer user_data);

static void drd_rdp_session_cancel_refresh_timer(DrdRdpSession *self);
static void drd_rdp_session_update_refresh_timer_state(DrdRdpSession *self);

/*
 * 功能：释放会话持有的线程与资源，防止 FreeRDP peer 悬挂。
 * 逻辑：停止事件线程与渲染管线，等待 VCM 线程结束；若 peer context 仍存在则交由 FreeRDP 管理；
 *       释放运行时与本地会话引用，交给父类做剩余清理。
 * 参数：object GObject 指针，预期为 DrdRdpSession。
 * 外部接口：调用 GLib g_thread_join 等线程接口，依赖 drd_local_session_close 关闭本地会话。
 */
static void drd_rdp_session_dispose(GObject *object)
{
    DrdRdpSession *self = DRD_RDP_SESSION(object);

    drd_rdp_session_stop_event_thread(self);
    drd_rdp_session_disable_graphics_pipeline(self, NULL);
    drd_rdp_session_cancel_refresh_timer(self);
    if (self->vcm_thread != NULL)
    {
        g_thread_join(self->vcm_thread);
        self->vcm_thread = NULL;
    }

    if (self->peer != NULL && self->peer->context != NULL)
    {
        /* Let FreeRDP manage the context lifecycle */
        self->peer = NULL;
    }

    g_clear_object(&self->runtime);
    g_clear_pointer(&self->local_session, drd_local_session_close);

    G_OBJECT_CLASS(drd_rdp_session_parent_class)->dispose(object);
}

/*
 * 功能：释放会话中申请的动态字符串与图形管线。
 * 逻辑：停止事件线程，释放 peer_address/state 字符串以及 graphics_pipeline 引用，
 *       最终交给父类 finalize。
 * 参数：object GObject 指针。
 * 外部接口：使用 GLib g_clear_pointer/g_clear_object 处理引用。
 */
static void drd_rdp_session_finalize(GObject *object)
{
    DrdRdpSession *self = DRD_RDP_SESSION(object);
    drd_rdp_session_stop_event_thread(self);
    g_clear_pointer(&self->peer_address, g_free);
    g_clear_pointer(&self->state, g_free);
    g_clear_object(&self->graphics_pipeline);
    G_OBJECT_CLASS(drd_rdp_session_parent_class)->finalize(object);
}

/*
 * 功能：设置会话类的生命周期回调。
 * 逻辑：挂载自定义 dispose/finalize。
 * 参数：klass 类结构指针。
 * 外部接口：GLib GObject 类型系统。
 */
static void drd_rdp_session_class_init(DrdRdpSessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_rdp_session_dispose;
    object_class->finalize = drd_rdp_session_finalize;
}

/*
 * 功能：初始化会话实例字段为安全默认值。
 * 逻辑：填充 peer 地址/状态默认字符串，重置线程/句柄/计数器，初始化原子标志。
 * 参数：self 会话实例。
 * 外部接口：使用 GLib 原子操作 g_atomic_int_set。
 */
static void drd_rdp_session_init(DrdRdpSession *self)
{
    self->peer = NULL;
    self->peer_address = g_strdup("unknown");
    self->state = g_strdup("created");
    self->runtime = NULL;
    self->vcm_thread = NULL;
    self->vcm = INVALID_HANDLE_VALUE;
    self->graphics_pipeline = NULL;
    self->graphics_pipeline_ready = FALSE;
    self->frame_sequence = 1;
    g_atomic_int_set(&self->max_surface_payload, 0);
    self->is_activated = FALSE;
    self->event_thread = NULL;
    self->stop_event = NULL;
    g_atomic_int_set(&self->connection_alive, 1);
    self->render_thread = NULL;
    g_atomic_int_set(&self->render_running, 0);
    self->closed_cb = NULL;
    self->closed_cb_data = NULL;
    g_atomic_int_set(&self->closed_cb_invoked, 0);
    self->local_session = NULL;
    self->passive_mode = FALSE;
    self->last_error = DRD_RDP_SESSION_ERROR_NONE;
    self->frame_pull_errors = 0;
    self->congestion_recovery_attempts = 0;
    self->congestion_disable_time = 0;
    self->congestion_permanent_disabled = FALSE;
    self->refresh_timeout_source = 0;
    g_atomic_int_set(&self->refresh_timeout_due, 0);
}

/*
 * 功能：创建基于 FreeRDP peer 的会话对象。
 * 逻辑：校验 peer 非空，分配对象并记录对端地址。
 * 参数：peer FreeRDP 的连接上下文。
 * 外部接口：依赖 FreeRDP peer->hostname 提供远端信息；使用 GLib g_object_new/g_strdup。
 */
DrdRdpSession *drd_rdp_session_new(freerdp_peer *peer)
{
    g_return_val_if_fail(peer != NULL, NULL);

    DrdRdpSession *self = g_object_new(DRD_TYPE_RDP_SESSION, NULL);
    self->peer = peer;
    g_clear_pointer(&self->peer_address, g_free);
    self->peer_address = g_strdup(peer->hostname != NULL ? peer->hostname : "unknown");
    return self;
}

/*
 * 功能：更新会话的对端地址描述。
 * 逻辑：释放旧地址并复制新的 peer 描述字符串。
 * 参数：self 会话实例；peer_address 对端地址描述。
 * 外部接口：GLib g_strdup/g_clear_pointer。
 */
void drd_rdp_session_set_peer_address(DrdRdpSession *self, const gchar *peer_address)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    g_clear_pointer(&self->peer_address, g_free);
    self->peer_address = g_strdup(peer_address != NULL ? peer_address : "unknown");
}

/*
 * 功能：获取会话的对端地址描述。
 * 逻辑：返回已记录的 peer_address，缺省时返回 unknown。
 * 参数：self 会话实例。
 * 外部接口：无。
 */
const gchar *drd_rdp_session_get_peer_address(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), "unknown");

    return self->peer_address != NULL ? self->peer_address : "unknown";
}

/*
 * 功能：更新会话状态字符串并输出日志。
 * 逻辑：释放旧状态、复制新状态，记录 DRD_LOG_MESSAGE。
 * 参数：self 会话实例；state 状态描述。
 * 外部接口：日志使用 DRD_LOG_MESSAGE。
 */
void drd_rdp_session_set_peer_state(DrdRdpSession *self, const gchar *state)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    g_clear_pointer(&self->state, g_free);
    self->state = g_strdup(state != NULL ? state : "unknown");

    DRD_LOG_MESSAGE("Session %s transitioned to state %s", self->peer_address, self->state);
}

/*
 * 功能：绑定服务器运行时到会话并尝试初始化图形管线。
 * 逻辑：校验参数后替换 runtime 引用，必要时增加引用计数，最后尝试创建 graphics pipeline。
 * 参数：self 会话；runtime 服务器运行时。
 * 外部接口：GLib g_object_ref/g_clear_object 管理引用。
 */
void drd_rdp_session_set_runtime(DrdRdpSession *self, DrdServerRuntime *runtime)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    g_return_if_fail(runtime == NULL || DRD_IS_SERVER_RUNTIME(runtime));

    if (self->runtime == runtime)
    {
        return;
    }

    if (runtime != NULL)
    {
        g_object_ref(runtime);
    }

    g_clear_object(&self->runtime);
    self->runtime = runtime;

    drd_rdp_session_maybe_init_graphics(self);
}

/*
 * 功能：记录虚拟通道管理器句柄并尝试初始化图形管线。
 * 逻辑：保存 vcm，调用 drd_rdp_session_maybe_init_graphics。
 * 参数：self 会话；vcm FreeRDP 虚拟通道管理器句柄。
 * 外部接口：句柄由 FreeRDP/WTS 提供。
 */
void drd_rdp_session_set_virtual_channel_manager(DrdRdpSession *self, HANDLE vcm)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    self->vcm = vcm;
    drd_rdp_session_maybe_init_graphics(self);
}

/*
 * 功能：记录 system 侧客户端指针，不改变其生命周期。
 * 逻辑：仅缓存指针供系统守护进程在认证后取回。
 * 参数：self 会话；system_client system 侧客户端指针。
 * 外部接口：无。
 */
void drd_rdp_session_set_system_client(DrdRdpSession *self, gpointer system_client)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    self->system_client = system_client;
}

gpointer drd_rdp_session_get_system_client(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), NULL);
    return self->system_client;
}

/*
 * 功能：记录是否需要保持底层连接打开。
 * 逻辑：仅保存标志，供认证完成后回调读取。
 * 参数：self 会话；keep_open 是否保持连接打开。
 * 外部接口：无。
 */
void drd_rdp_session_set_connection_keep_open(DrdRdpSession *self, gboolean keep_open)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    self->connection_keep_open = keep_open;
}

gboolean drd_rdp_session_get_connection_keep_open(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    return self->connection_keep_open;
}

/*
 * 功能：设置会话关闭回调，支持在已关闭时立即触发。
 * 逻辑：保存回调与上下文；若回调置空则重置已调用标记；若连接已结束立即调用通知。
 * 参数：self 会话；callback 关闭回调；user_data 回调透传数据。
 * 外部接口：使用 GLib 原子 g_atomic_int_get/compare_and_exchange 检查状态。
 */
void drd_rdp_session_set_closed_callback(DrdRdpSession *self, DrdRdpSessionClosedFunc callback, gpointer user_data)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    self->closed_cb = callback;
    self->closed_cb_data = user_data;

    if (callback == NULL)
    {
        g_atomic_int_set(&self->closed_cb_invoked, 0);
        return;
    }

    if (g_atomic_int_get(&self->connection_alive) == 0)
    {
        drd_rdp_session_notify_closed(self);
    }
}

/*
 * 功能：设置会话是否为被动模式。
 * 逻辑：简单记录标志。
 * 参数：self 会话；passive 是否被动。
 * 外部接口：无。
 */
void drd_rdp_session_set_passive_mode(DrdRdpSession *self, gboolean passive)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    self->passive_mode = passive;
}

/*
 * 功能：将 PAM/NLA 验证后得到的本地会话附加到 RDP 会话。
 * 逻辑：若 session 非空则替换旧的 local_session。
 * 参数：self 会话；session 本地会话对象。
 * 外部接口：调用 drd_local_session_close 关闭旧会话。
 */
void drd_rdp_session_attach_local_session(DrdRdpSession *self, DrdLocalSession *session)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (session == NULL)
    {
        return;
    }

    g_clear_pointer(&self->local_session, drd_local_session_close);
    self->local_session = session;
}

DrdLocalSession *
drd_rdp_session_get_local_session(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), NULL);
    return self->local_session;
}

/*
 * 功能：post-connect 钩子，更新状态。
 * 逻辑：设置状态为 post-connect 后返回 TRUE。
 * 参数：self 会话。
 * 外部接口：无。
 */
BOOL drd_rdp_session_post_connect(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    drd_rdp_session_set_peer_state(self, "post-connect");
    return TRUE;
}

/*
 * 功能：执行 RDP 会话激活，启动编码/渲染流程。
 * 逻辑：被动模式直接标记激活；否则校验客户端桌面尺寸、获取编码配置并准备 runtime 流；
 *       若需要触发关键帧请求，刷新 payload 上限，启动渲染线程并更新状态。
 * 参数：self 会话。
 * 外部接口：调用 drd_server_runtime_* 访问编码流水，使用 FreeRDP settings 更新桌面尺寸，
 *           日志采用 DRD_LOG_MESSAGE/DRD_LOG_WARNING。
 */
BOOL drd_rdp_session_activate(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);

    if (self->passive_mode)
    {
        drd_rdp_session_set_peer_state(self, "activated-passive");
        self->is_activated = TRUE;
        DRD_LOG_MESSAGE("Session %s running in passive system mode, skipping transport", self->peer_address);
        return TRUE;
    }

    if (!drd_rdp_session_enforce_peer_desktop_size(self))
    {
        drd_rdp_session_set_peer_state(self, "desktop-resize-blocked");
        drd_rdp_session_disconnect(self, "client does not support desktop resize");
        return FALSE;
    }

    if (self->runtime == NULL)
    {
        return FALSE;
    }
    const gboolean stream_running = drd_server_runtime_is_stream_running(self->runtime);
    DrdEncodingOptions encoding_opts;
    if (!drd_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        DRD_LOG_WARNING("Session %s missing encoding options before stream start", self->peer_address);
        drd_rdp_session_set_peer_state(self, "encoding-opts-missing");
        drd_rdp_session_disconnect(self, "encoding-opts-missing");
        return FALSE;
    }

    gboolean started_stream = FALSE;
    if (!stream_running)
    {
        g_autoptr(GError) stream_error = NULL;
        if (!drd_server_runtime_prepare_stream(self->runtime, &encoding_opts, &stream_error))
        {
            if (stream_error != NULL)
            {
                DRD_LOG_WARNING("Session %s failed to prepare runtime stream: %s", self->peer_address,
                                stream_error->message);
            }
            else
            {
                DRD_LOG_WARNING("Session %s failed to prepare runtime stream", self->peer_address);
            }
            drd_rdp_session_set_peer_state(self, "stream-prepare-failed");
            drd_rdp_session_disconnect(self, "stream-prepare-failed");
            return FALSE;
        }
        started_stream = TRUE;
    }

    if (started_stream)
    {
        drd_server_runtime_request_keyframe(self->runtime);
    }

    drd_rdp_session_refresh_surface_payload_limit(self);

    drd_rdp_session_set_peer_state(self, "activated");
    self->is_activated = TRUE;
    if (!drd_rdp_session_start_render_thread(self))
    {
        DRD_LOG_WARNING("Session %s failed to start renderer thread", self->peer_address);
    }
    // drd_rdp_session_start_event_thread(self);
    return TRUE;
}

/*
 * 功能：创建事件线程（含 VCM 线程），准备 Stop 事件。
 * 逻辑：校验 peer 有效与未重复创建；初始化 stop_event；重置 connection_alive；
 *       若存在 VCM 句柄则启动 drd_rdp_session_vcm_thread。
 * 参数：self 会话。
 * 外部接口：使用 WinPR CreateEvent/SetEvent/WaitForSingleObject 操作事件，
 *           GLib g_thread_new 创建线程。
 */
gboolean drd_rdp_session_start_event_thread(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);

    if (self->peer == NULL)
    {
        return FALSE;
    }

    if (self->event_thread != NULL)
    {
        return TRUE;
    }
    if (self->stop_event == NULL)
    {
        self->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (self->stop_event == NULL)
        {
            DRD_LOG_WARNING("Session %s failed to create stop event", self->peer_address);
            return FALSE;
        }
    }

    g_atomic_int_set(&self->connection_alive, 1);

    if (self->vcm != NULL && self->vcm != INVALID_HANDLE_VALUE && self->vcm_thread == NULL)
    {
        self->vcm_thread = g_thread_new("drd-rdp-vcm", drd_rdp_session_vcm_thread, self);
    }

    return TRUE;
}

/*
 * 功能：启动渲染线程负责从 runtime 拉取帧。
 * 逻辑：若线程未创建且 runtime 有效，则设置 render_running 并创建渲染线程。
 * 参数：self 会话。
 * 外部接口：GLib g_thread_new 创建线程，g_atomic_int_set 设置标志。
 */
static gboolean drd_rdp_session_start_render_thread(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);

    if (self->render_thread != NULL)
    {
        return TRUE;
    }

    if (self->runtime == NULL)
    {
        return FALSE;
    }

    g_atomic_int_set(&self->render_running, 1);
    self->render_thread = g_thread_new("drd-render-thread", drd_rdp_session_render_thread, g_object_ref(self));
    if (self->render_thread == NULL)
    {
        g_atomic_int_set(&self->render_running, 0);
        return FALSE;
    }
    return TRUE;
}

/*
 * 功能：停止渲染线程并等待退出。
 * 逻辑：若线程存在，清除运行标志并 join 线程。
 * 参数：self 会话。
 * 外部接口：GLib g_thread_join。
 */
static void drd_rdp_session_stop_render_thread(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->render_thread == NULL)
    {
        return;
    }

    g_atomic_int_set(&self->render_running, 0);
    g_thread_join(self->render_thread);
    self->render_thread = NULL;
}

/*
 * 功能：停止事件线程/渲染线程并通知关闭。
 * 逻辑：先停止渲染线程并置 connection_alive=0；触发 stop_event 唤醒等待；
 *       join 事件与 VCM 线程、关闭事件句柄，最后触发关闭回调。
 * 参数：self 会话。
 * 外部接口：WinPR SetEvent/CloseHandle 操作事件；GLib g_thread_join。
 */
void drd_rdp_session_stop_event_thread(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    drd_rdp_session_stop_render_thread(self);

    g_atomic_int_set(&self->connection_alive, 0);

    if (self->stop_event != NULL)
    {
        SetEvent(self->stop_event);
    }

    if (self->event_thread != NULL)
    {
        g_thread_join(self->event_thread);
        self->event_thread = NULL;
        DRD_LOG_MESSAGE("Session %s stopped event thread", self->peer_address);
    }

    if (self->vcm_thread != NULL)
    {
        g_thread_join(self->vcm_thread);
        self->vcm_thread = NULL;
    }

    if (self->stop_event != NULL)
    {
        CloseHandle(self->stop_event);
        self->stop_event = NULL;
    }

    drd_rdp_session_notify_closed(self);
}

/*
 * 功能：触发会话关闭回调（只触发一次）。
 * 逻辑：若存在回调且 closed_cb_invoked 原子从 0->1 成功，则调用回调。
 * 参数：self 会话。
 * 外部接口：GLib g_atomic_int_compare_and_exchange。
 */
static void drd_rdp_session_notify_closed(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->closed_cb == NULL)
    {
        return;
    }

    if (!g_atomic_int_compare_and_exchange(&self->closed_cb_invoked, 0, 1))
    {
        return;
    }

    self->closed_cb(self, self->closed_cb_data);
}

/*
 * 功能：判断客户端是否为 Windows mstsc。
 * 逻辑：从 FreeRDP settings 读取 OsMajor/MinorType 比较 Windows 常量。
 * 参数：self 会话。
 * 外部接口：使用 freerdp_settings_get_uint32 读取设置。
 */
gboolean drd_rdp_session_client_is_mstsc(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    if (self->peer == NULL || self->peer->context == NULL || self->peer->context->settings == NULL)
    {
        return FALSE;
    }

    rdpSettings *settings = self->peer->context->settings;
    const guint32 os_major = freerdp_settings_get_uint32(settings, FreeRDP_OsMajorType);
    const guint32 os_minor = freerdp_settings_get_uint32(settings, FreeRDP_OsMinorType);
    return os_major == OSMAJORTYPE_WINDOWS && os_minor == OSMINORTYPE_WINDOWS_NT;
}

/*
 * 功能：读取客户端上报的桌面分辨率。
 * 逻辑：从 FreeRDP settings 获取 DesktopWidth/Height。
 * 参数：self 会话；out_width/out_height 输出分辨率。
 * 外部接口：freerdp_settings_get_uint32 读取设置。
 */
gboolean
drd_rdp_session_get_peer_resolution(DrdRdpSession *self,
                                    guint32 *out_width,
                                    guint32 *out_height)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    g_return_val_if_fail(out_width != NULL, FALSE);
    g_return_val_if_fail(out_height != NULL, FALSE);

    if (self->peer == NULL || self->peer->context == NULL || self->peer->context->settings == NULL)
    {
        return FALSE;
    }

    rdpSettings *settings = self->peer->context->settings;
    *out_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    *out_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    return TRUE;
}

/*
 * 功能：将 UTF-8 字符串转换为 UTF-16（含终止符）。
 * 逻辑：调用 ConvertUtf8ToWCharAlloc 分配转换后的字符串，返回字节长度。
 * 参数：str 源字符串；size 输出字节长度。
 * 外部接口：WinPR ConvertUtf8ToWCharAlloc 负责编码转换。
 */
static WCHAR *drd_rdp_session_get_utf16_string(const char *str, size_t *size)
{
    g_return_val_if_fail(str != NULL, NULL);
    g_return_val_if_fail(size != NULL, NULL);

    *size = 0;
    WCHAR *utf16 = ConvertUtf8ToWCharAlloc(str, size);
    if (utf16 == NULL)
    {
        return NULL;
    }

    *size = (*size + 1) * sizeof(WCHAR);
    return utf16;
}

/*
 * 功能：生成随机的重定向 GUID 并以 base64 UTF-16 形式返回。
 * 逻辑：通过 winpr_RAND 生成随机 16 字节 -> base64 编码 -> 转为 UTF-16。
 * 参数：size 输出字节长度。
 * 外部接口：WinPR winpr_RAND 生成随机数，FreeRDP crypto_base64_encode 进行 base64。
 */
static WCHAR *drd_rdp_session_generate_redirection_guid(size_t *size)
{
    BYTE guid_bytes[16] = {0};
    g_autofree gchar *guid_base64 = NULL;

    if (winpr_RAND(guid_bytes, sizeof(guid_bytes)) == -1)
    {
        return NULL;
    }

    guid_base64 = crypto_base64_encode(guid_bytes, sizeof(guid_bytes));
    if (guid_base64 == NULL)
    {
        return NULL;
    }

    return drd_rdp_session_get_utf16_string(guid_base64, size);
}

/*
 * 功能：将 PEM 证书封装为 RDP 重定向要求的 container。
 * 逻辑：解析 PEM 为 rdpCertificate -> 获取 DER -> 按协议序列化 ELEMENT_TYPE_CERTIFICATE
 *       头与 DER 数据到 wStream，返回 buffer 指针与长度。
 * 参数：certificate PEM 字符串；size 输出数据长度。
 * 外部接口：FreeRDP freerdp_certificate_new_from_pem/get_der 处理证书，
 *           WinPR Stream_* API 管理序列化。
 */
static BYTE *drd_rdp_session_get_certificate_container(const char *certificate, size_t *size)
{
    g_return_val_if_fail(certificate != NULL, NULL);
    g_return_val_if_fail(size != NULL, NULL);

    g_autoptr(rdpCertificate) rdp_cert = freerdp_certificate_new_from_pem(certificate);
    if (rdp_cert == NULL)
    {
        return NULL;
    }

    size_t der_length = 0;
    g_autofree BYTE *der_cert = freerdp_certificate_get_der(rdp_cert, &der_length);
    if (der_cert == NULL)
    {
        return NULL;
    }

    wStream *stream = Stream_New(NULL, 2048);
    if (stream == NULL)
    {
        return NULL;
    }

    if (!Stream_EnsureRemainingCapacity(stream, 12))
    {
        Stream_Free(stream, TRUE);
        return NULL;
    }

    Stream_Write_UINT32(stream, ELEMENT_TYPE_CERTIFICATE);
    Stream_Write_UINT32(stream, ENCODING_TYPE_ASN1_DER);
    const gint64 der_len_signed = (gint64) der_length;
    if (der_len_signed < 0 || der_len_signed > G_MAXUINT32)
    {
        Stream_Free(stream, TRUE);
        return NULL;
    }
    Stream_Write_UINT32(stream, der_len_signed);

    if (!Stream_EnsureRemainingCapacity(stream, der_length))
    {
        Stream_Free(stream, TRUE);
        return NULL;
    }

    Stream_Write(stream, der_cert, der_length);

    *size = Stream_GetPosition(stream);
    BYTE *container = Stream_Buffer(stream);
    Stream_Free(stream, FALSE);
    return container;
}

/*
 * 功能：记录并处理会话错误，必要时触发断开。
 * 逻辑：保存错误类型并映射为英文原因字符串，输出警告；若为重定向错误则断开连接。
 * 参数：self 会话；error 枚举。
 * 外部接口：DRD_LOG_WARNING 打印日志。
 */
void drd_rdp_session_notify_error(DrdRdpSession *self, DrdRdpSessionError error)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    if (error == DRD_RDP_SESSION_ERROR_NONE)
    {
        return;
    }

    self->last_error = error;
    const gchar *reason = NULL;
    switch (error)
    {
        case DRD_RDP_SESSION_ERROR_BAD_CAPS:
            reason = "client reported invalid capabilities";
            break;
        case DRD_RDP_SESSION_ERROR_BAD_MONITOR_DATA:
            reason = "client monitor layout invalid";
            break;
        case DRD_RDP_SESSION_ERROR_CLOSE_STACK_ON_DRIVER_FAILURE:
            reason = "graphics driver requested close";
            break;
        case DRD_RDP_SESSION_ERROR_GRAPHICS_SUBSYSTEM_FAILED:
            reason = "graphics subsystem failed";
            break;
        case DRD_RDP_SESSION_ERROR_SERVER_REDIRECTION:
            reason = "server redirection requested";
            break;
        case DRD_RDP_SESSION_ERROR_NONE:
        default:
            reason = "unknown";
            break;
    }

    DRD_LOG_WARNING("Session %s reported error: %s", self->peer_address, reason);

    if (error == DRD_RDP_SESSION_ERROR_SERVER_REDIRECTION)
    {
        drd_rdp_session_disconnect(self, reason);
    }
}

/*
 * 功能：向客户端发送服务器重定向信息（路由令牌 + 凭据 + 证书）。
 * 逻辑：构造 redirection 结构，填充负载均衡 token、用户名/密码、随机 GUID、目标证书，
 *       校验标志合法后调用 FreeRDP 的 SendServerRedirection。
 * 参数：self 会话；routing_token/username/password/certificate 重定向数据。
 * 外部接口：使用 FreeRDP redirection_* API 与 freerdp_settings_get_uint32 读取客户端 OS，
 *           调用 peer->SendServerRedirection 发送。
 */
gboolean drd_rdp_session_send_server_redirection(DrdRdpSession *self, const gchar *routing_token, const gchar *username,
                                                 const gchar *password, const gchar *certificate)
{
    DRD_LOG_MESSAGE("drd_rdp_session_send_server_redirection");
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    g_return_val_if_fail(self->peer != NULL, FALSE);
    g_return_val_if_fail(routing_token != NULL, FALSE);
    g_return_val_if_fail(username != NULL, FALSE);
    g_return_val_if_fail(password != NULL, FALSE);
    g_return_val_if_fail(certificate != NULL, FALSE);

    rdpSettings *settings = self->peer->context != NULL ? self->peer->context->settings : NULL;
    if (settings == NULL)
    {
        DRD_LOG_WARNING("Session %s missing settings, cannot send redirection", self->peer_address);
        return FALSE;
    }

    g_autoptr(rdpRedirection) redirection = redirection_new();
    if (redirection == NULL)
    {
        DRD_LOG_MESSAGE("redirection null");
        return FALSE;
    }

    guint32 redirection_flags = 0;
    guint32 incorrect_flags = 0;
    size_t size = 0;

    redirection_flags |= LB_LOAD_BALANCE_INFO;
    redirection_set_byte_option(redirection, LB_LOAD_BALANCE_INFO, (const BYTE *) routing_token, strlen(routing_token));

    redirection_flags |= LB_USERNAME;
    redirection_set_string_option(redirection, LB_USERNAME, username);

    redirection_flags |= LB_PASSWORD;
    guint32 os_major = freerdp_settings_get_uint32(settings, FreeRDP_OsMajorType);
    if (os_major != OSMAJORTYPE_IOS && os_major != OSMAJORTYPE_ANDROID)
    {
        redirection_flags |= LB_PASSWORD_IS_PK_ENCRYPTED;
    }

    g_autofree WCHAR *utf16_password = drd_rdp_session_get_utf16_string(password, &size);
    if (utf16_password == NULL)
    {
        return FALSE;
    }
    redirection_set_byte_option(redirection, LB_PASSWORD, (const BYTE *) utf16_password, size);

    redirection_flags |= LB_REDIRECTION_GUID;
    g_autofree WCHAR *encoded_guid = drd_rdp_session_generate_redirection_guid(&size);
    if (encoded_guid == NULL)
    {
        return FALSE;
    }
    redirection_set_byte_option(redirection, LB_REDIRECTION_GUID, (const BYTE *) encoded_guid, size);

    redirection_flags |= LB_TARGET_CERTIFICATE;
    g_autofree BYTE *certificate_container = drd_rdp_session_get_certificate_container(certificate, &size);
    if (certificate_container == NULL)
    {
        return FALSE;
    }
    redirection_set_byte_option(redirection, LB_TARGET_CERTIFICATE, certificate_container, size);

    redirection_set_flags(redirection, redirection_flags);

    if (!redirection_settings_are_valid(redirection, &incorrect_flags))
    {
        DRD_LOG_WARNING("Session %s invalid redirection flags 0x%08x", self->peer_address, incorrect_flags);
        return FALSE;
    }

    DRD_LOG_MESSAGE("Session %s sending server redirection", self->peer_address);
    if (!self->peer->SendServerRedirection(self->peer, redirection))
    {
        DRD_LOG_WARNING("Session %s failed to send server redirection", self->peer_address);
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：主动断开会话并释放资源。
 * 逻辑：记录原因日志 -> 停止事件线程/图形管线 -> 清理本地会话并重置标志 -> 调用 peer->Disconnect。
 * 参数：self 会话；reason 断开原因。
 * 外部接口：FreeRDP peer->Disconnect 终止连接；DRD_LOG_MESSAGE 输出日志。
 */
void drd_rdp_session_disconnect(DrdRdpSession *self, const gchar *reason)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (reason != NULL)
    {
        DRD_LOG_MESSAGE("Disconnecting session %s: %s", self->peer_address, reason);
    }

    drd_rdp_session_stop_event_thread(self);
    drd_rdp_session_disable_graphics_pipeline(self, NULL);
    g_clear_pointer(&self->local_session, drd_local_session_close);
    g_atomic_int_set(&self->render_running, 0);
    g_atomic_int_set(&self->connection_alive, 0);
    if (self->peer != NULL && self->peer->Disconnect != NULL)
    {
        self->peer->Disconnect(self->peer);
        self->peer = NULL;
    }

    self->is_activated = FALSE;
}

/*
 * 功能：在独立线程处理虚拟通道与 peer 事件，驱动 drdynvc/Rdpgfx 生命周期。
 * 逻辑：获取 VCM 事件句柄，循环等待 stop_event、channel_event 以及 peer 事件；
 *       调用 peer->CheckFileDescriptor 驱动 FreeRDP，监测 drdynvc 状态并触发 graphics 管线初始化，
 *       直到连接终止。
 * 参数：user_data 会话指针。
 * 外部接口：WinPR WaitForMultipleObjects/WaitForSingleObject 等事件 API，
 *           FreeRDP WTSVirtualChannelManager*、peer->GetEventHandles/CheckFileDescriptor。
 */
static gpointer drd_rdp_session_vcm_thread(gpointer user_data)
{
    DrdRdpSession *self = g_object_ref(DRD_RDP_SESSION(user_data));
    freerdp_peer *peer = self->peer;
    HANDLE vcm = self->vcm;
    HANDLE channel_event = NULL;

    if (vcm == NULL || vcm == INVALID_HANDLE_VALUE || peer == NULL)
    {
        g_object_unref(self);
        return NULL;
    }

    channel_event = WTSVirtualChannelManagerGetEventHandle(vcm);

    while (g_atomic_int_get(&self->connection_alive))
    {
        HANDLE events[32];
        uint32_t peer_events_handles = 0;
        DWORD n_events = 0;

        if (self->stop_event != NULL)
        {
            events[n_events++] = self->stop_event;
        }
        if (channel_event != NULL)
        {
            events[n_events++] = channel_event;
        }

        peer_events_handles = peer->GetEventHandles(peer, &events[n_events], G_N_ELEMENTS(events) - n_events);
        if (!peer_events_handles)
        {
            g_message("[RDP] peer_events_handles 0, stopping session");
            g_atomic_int_set(&self->connection_alive, 0);
            break;
        }
        n_events += peer_events_handles;
        DWORD status = WAIT_TIMEOUT;
        if (n_events > 0)
        {
            status = WaitForMultipleObjects(n_events, events, FALSE, INFINITE);
        }

        if (status == WAIT_FAILED)
        {
            break;
        }

        if (!peer->CheckFileDescriptor(peer))
        {
            g_message("[RDP] CheckFileDescriptor error, stopping session");
            g_atomic_int_set(&self->connection_alive, 0);
            break;
        }

        if (!peer->connected)
        {
            continue;
        }

        if (!WTSVirtualChannelManagerIsChannelJoined(vcm, DRDYNVC_SVC_CHANNEL_NAME))
        {
            continue;
        }

        switch (WTSVirtualChannelManagerGetDrdynvcState(vcm))
        {
            case DRDYNVC_STATE_NONE:
                SetEvent(channel_event);
                break;
            case DRDYNVC_STATE_READY:
                if (self->graphics_pipeline && g_atomic_int_get(&self->connection_alive))
                {
                    drd_rdp_graphics_pipeline_maybe_init(self->graphics_pipeline);
                }
                break;
        }
        if (!g_atomic_int_get(&self->connection_alive))
        {
            break;
        }
        if (channel_event != NULL && WaitForSingleObject(channel_event, 0) == WAIT_OBJECT_0)
        {
            if (!WTSVirtualChannelManagerCheckFileDescriptor(vcm))
            {
                DRD_LOG_MESSAGE("Session %s failed to check VCM descriptor", self->peer_address);
                g_atomic_int_set(&self->connection_alive, 0);
                break;
            }
        }
    }

    g_atomic_int_set(&self->render_running, 0);
    drd_rdp_session_notify_closed(self);
    g_object_unref(self);
    return NULL;
}

/*
 * 功能：强制客户端桌面分辨率匹配服务器编码分辨率。
 * 逻辑：读取 runtime 编码宽高与客户端设置；若客户端不支持 DesktopResize 且尺寸不符则拒绝；
 *       否则更新 FreeRDP 设置并调用 DesktopResize 回调同步。
 * 参数：self 会话。
 * 外部接口：freerdp_settings_get_uint32/get_bool 读取设置，freerdp_settings_set_uint32 写入；
 *           调用 rdpUpdate->DesktopResize 通知客户端。
 */
static gboolean drd_rdp_session_enforce_peer_desktop_size(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), TRUE);

    if (self->peer == NULL || self->peer->context == NULL || self->runtime == NULL)
    {
        return TRUE;
    }

    DrdEncodingOptions encoding_opts;
    if (!drd_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        return TRUE;
    }

    rdpContext *context = self->peer->context;
    rdpSettings *settings = context->settings;
    if (settings == NULL)
    {
        return TRUE;
    }

    const guint32 desired_width = encoding_opts.width;
    const guint32 desired_height = encoding_opts.height;
    const guint32 client_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const guint32 client_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    const gboolean client_allows_resize = freerdp_settings_get_bool(settings, FreeRDP_DesktopResize);

    DRD_LOG_MESSAGE("Session %s peer geometry %ux%u, server requires %ux%u", self->peer_address, client_width,
                    client_height, desired_width, desired_height);

    if (!client_allows_resize && (client_width != desired_width || client_height != desired_height))
    {
        DRD_LOG_WARNING("Session %s client did not advertise DesktopResize, cannot override %ux%u with %ux%u",
                        self->peer_address, client_width, client_height, desired_width, desired_height);
        return FALSE;
    }

    gboolean updated = FALSE;

    if (client_width != desired_width)
    {
        if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, desired_width))
        {
            DRD_LOG_WARNING("Session %s could not update DesktopWidth to %u", self->peer_address, desired_width);
            return FALSE;
        }
        updated = TRUE;
    }

    if (client_height != desired_height)
    {
        if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, desired_height))
        {
            DRD_LOG_WARNING("Session %s could not update DesktopHeight to %u", self->peer_address, desired_height);
            return FALSE;
        }
        updated = TRUE;
    }

    if (!updated)
    {
        return TRUE;
    }

    rdpUpdate *update = context->update;
    if (update == NULL || update->DesktopResize == NULL)
    {
        DRD_LOG_WARNING("Session %s missing DesktopResize callback, cannot synchronize geometry", self->peer_address);
        return FALSE;
    }

    if (!update->DesktopResize(context))
    {
        DRD_LOG_WARNING("Session %s failed to notify DesktopResize", self->peer_address);
        return FALSE;
    }

    DRD_LOG_MESSAGE("Session %s enforced desktop resolution to %ux%u", self->peer_address, desired_width,
                    desired_height);
    return TRUE;
}

/*
 * 功能：缓存对端协商的 SurfaceBits 多片段负载上限。
 * 逻辑：从 FreeRDP settings 读取 MultifragMaxRequestSize，存入原子变量供渲染线程快速读取。
 * 参数：self 会话。
 * 外部接口：freerdp_settings_get_uint32 读取设置；GLib g_atomic_int_set 更新缓存。
 */
static void drd_rdp_session_refresh_surface_payload_limit(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    guint32 max_payload = 0;
    if (self->peer != NULL && self->peer->context != NULL && self->peer->context->settings != NULL)
    {
        max_payload = freerdp_settings_get_uint32(self->peer->context->settings, FreeRDP_MultifragMaxRequestSize);
    }
    /* 缓存通道协商出的多片段上限，渲染线程读取时无需再访问 settings。 */
    g_atomic_int_set(&self->max_surface_payload, (gint) max_payload);
}

/*
 * 功能：渲染线程循环，从 runtime 拉取编码帧并选择 Rdpgfx 或 SurfaceBits 发送。
 * 逻辑：在连接/激活有效时，必要时等待图形管线容量；从 runtime 取帧，优先提交 Rdpgfx，
 *       若提交失败或不可用则回退 SurfaceBits 并处理 payload 限制，维护帧序列号。
 * 参数：user_data 会话指针。
 * 外部接口：drd_server_runtime_pull_encoded_frame 获取帧，drd_rdp_graphics_pipeline_* 操作图形通道，
 *           FreeRDP/WinPR 睡眠 g_usleep，日志使用 DRD_LOG_*。
 */
static gpointer drd_rdp_session_render_thread(gpointer user_data)
{
    DrdRdpSession *self = DRD_RDP_SESSION(user_data);

    const guint target_fps = drd_capture_metrics_get_target_fps();
    const gint64 stats_interval = drd_capture_metrics_get_stats_interval_us();
    guint stats_frames = 0;
    gint64 stats_window_start = 0;

    while (g_atomic_int_get(&self->render_running))
    {
        if (!g_atomic_int_get(&self->connection_alive))
        {
            break;
        }

        if (!self->is_activated || self->runtime == NULL)
        {
            g_usleep(1000);
            continue;
        }
        g_autoptr(GError) error = NULL;
        gboolean sent = FALSE;
        DrdFrameTransport transport = drd_server_runtime_get_transport(self->runtime);
        if (transport == DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE)
        {
            /* 尝试恢复 Rdpgfx 管线 */
            if (self->graphics_pipeline == NULL)
            {
                drd_rdp_session_maybe_init_graphics(self);
                drd_rdp_graphics_pipeline_maybe_init(self->graphics_pipeline);
            }

            if (!self->graphics_pipeline_ready && self->graphics_pipeline != NULL &&
                drd_rdp_graphics_pipeline_is_ready(self->graphics_pipeline))
            {
                drd_server_runtime_set_transport(self->runtime, DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE);
                self->graphics_pipeline_ready = TRUE;
                DRD_LOG_MESSAGE("Session %s graphics pipeline ready, switching to GFX", self->peer_address);
            }

            if (self->graphics_pipeline_ready)
            {
                if (!drd_rdp_session_wait_for_graphics_capacity(self, -1) || !drd_rdp_graphics_pipeline_can_submit(self->graphics_pipeline))
                {
                    DRD_LOG_WARNING("Session %s Rdpgfx congestion persists, disabling graphics pipeline",
                                                       self->peer_address);
                    drd_rdp_session_disable_graphics_pipeline(self, "Rdpgfx congestion");
                    continue;
                }
                gboolean h264 = FALSE;
                if (g_atomic_int_compare_and_exchange(&self->refresh_timeout_due, 1, 0))
                {
                    if (drd_server_runtime_send_cached_frame_surface_gfx(self->runtime,
                                                                         self->peer->context->settings,
                                                                         drd_rdpgfx_get_context(self->graphics_pipeline),
                                                                         drd_rdp_graphics_pipeline_get_surface_id(self->graphics_pipeline),
                                                                         self->frame_sequence,
                                                                         &h264,
                                                                         &error))
                    {
                        sent = TRUE;
                        drd_rdp_graphics_pipeline_out_frame_change(self->graphics_pipeline, TRUE);

                        drd_rdp_graphics_pipeline_set_last_frame_mode(self->graphics_pipeline, h264);
                    }
                    else if (error != NULL && error->domain == G_IO_ERROR &&
                             (error->code == G_IO_ERROR_TIMED_OUT || error->code == G_IO_ERROR_PENDING))
                    {
                        g_clear_error(&error);
                    }
                    else if (error != NULL)
                    {
                        self->frame_pull_errors++;
                        DRD_LOG_WARNING("Session %s failed to send cached refresh: %s (errors=%" G_GUINT64_FORMAT ")",
                                        self->peer_address,
                                        error->message,
                                        self->frame_pull_errors);
                        g_clear_error(&error);
                    }
                }
                if (!sent)
                {
                    if (drd_server_runtime_pull_encoded_frame_surface_gfx(
                            self->runtime,
                            self->peer->context->settings,
                            drd_rdpgfx_get_context(self->graphics_pipeline),
                            drd_rdp_graphics_pipeline_get_surface_id(self->graphics_pipeline),
                            16 * 1000,
                            self->frame_sequence,
                            &h264,
                            &error))
                    {
                        sent = TRUE;
                        drd_rdp_graphics_pipeline_out_frame_change(self->graphics_pipeline, TRUE);

                        drd_rdp_graphics_pipeline_set_last_frame_mode(self->graphics_pipeline, h264);

                    }
                    else
                    {
                        if (error != NULL && error->domain == G_IO_ERROR &&
                        (error->code == G_IO_ERROR_TIMED_OUT || error->code == G_IO_ERROR_PENDING))
                        {
                            /* 无新数据情况：不递增 outstanding_frames，无需人工调整 */
                            g_clear_error(&error);
                            continue;
                        }
                        if (error != NULL)
                        {
                            self->frame_pull_errors++;
                            DRD_LOG_WARNING("Session %s failed to pull encoded frame: %s (errors=%" G_GUINT64_FORMAT ")",
                                            self->peer_address, error->message, self->frame_pull_errors);
                        }
                        continue;
                    }
                }
            }
        }
        if (transport == DRD_FRAME_TRANSPORT_SURFACE_BITS)
        {
            const guint32 max_payload = (guint32) g_atomic_int_get(&self->max_surface_payload);
            if (!drd_server_runtime_pull_encoded_frame_surface_bit(
                        self->runtime, self->peer->context, self->frame_sequence, max_payload, 16 * 1000, &error))
            {
                if (error != NULL && error->domain == G_IO_ERROR &&
                    (error->code == G_IO_ERROR_TIMED_OUT || error->code == G_IO_ERROR_PENDING))
                {
                    g_clear_error(&error);
                    continue;
                }
                /* SurfaceBits 未实现时，不应禁用 Rdpgfx，因为它是唯一可用的传输 */
                if (error != NULL && error->domain == G_IO_ERROR && error->code == G_IO_ERROR_NOT_SUPPORTED)
                {
                    DRD_LOG_WARNING("Session %s SurfaceBits not supported, Rdpgfx required", self->peer_address);
                    g_clear_error(&error);
                    /* SurfaceBits 不可用时，尝试恢复 Rdpgfx */
                    if (!self->graphics_pipeline)
                    {
                        drd_rdp_session_maybe_init_graphics(self);
                        drd_rdp_graphics_pipeline_maybe_init(self->graphics_pipeline);
                    }
                    continue;
                }
                if (error != NULL)
                {
                    self->frame_pull_errors++;
                    DRD_LOG_WARNING("Session %s failed to send surface bits: %s (errors=%" G_GUINT64_FORMAT ")",
                                    self->peer_address, error->message, self->frame_pull_errors);
                }
                continue;
            }
            sent = TRUE;
        }
        else
        {
            /* not reached */
        }


        if (sent)
        {
            const gint64 now = g_get_monotonic_time();
            if (stats_window_start == 0)
            {
                stats_window_start = now;
            }
            stats_frames++;

            const gint64 elapsed = now - stats_window_start;
            if (elapsed >= stats_interval)
            {
                const gdouble actual_fps = (gdouble) stats_frames * (gdouble) G_USEC_PER_SEC / (gdouble) elapsed;
                const gboolean reached_target = actual_fps >= (gdouble) target_fps;
                DRD_LOG_MESSAGE("Session %s render fps=%.2f (target=%u): %s", self->peer_address, actual_fps,
                                target_fps, reached_target ? "reached target" : "below target");
                stats_frames = 0;
                stats_window_start = now;
            }
        }

        drd_rdp_session_update_refresh_timer_state(self);

        self->frame_sequence++;
        if (self->frame_sequence == 0)
        {
            self->frame_sequence = 1;
        }
    }

    g_object_unref(self);
    return NULL;
}

/*
 * 功能：按需创建 Rdpgfx 图形管线，前提是编码模式为 RFX 且 VCM/peer/runtime 就绪。
 * 逻辑：若已存在管线或条件不足直接返回；读取编码配置，确认模式为 RFX 后调用
 *       drd_rdp_graphics_pipeline_new 创建管线并记录。
 * 参数：self 会话。
 * 外部接口：drd_server_runtime_get_encoding_options 读取配置，
 *           drd_rdp_graphics_pipeline_new 创建 FreeRDP Rdpgfx 上下文。
 */
static void drd_rdp_session_maybe_init_graphics(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->graphics_pipeline != NULL || self->peer == NULL || self->peer->context == NULL || self->runtime == NULL ||
        self->vcm == NULL || self->vcm == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DrdEncodingOptions encoding_opts;
    if (!drd_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        return;
    }

    if (encoding_opts.mode != DRD_ENCODING_MODE_RFX && encoding_opts.mode != DRD_ENCODING_MODE_AUTO &&
        encoding_opts.mode != DRD_ENCODING_MODE_H264)
    {
        return;
    }

    DrdRdpGraphicsPipeline *pipeline = drd_rdp_graphics_pipeline_new(
            self->peer, self->vcm, self->runtime, (guint16) encoding_opts.width, (guint16) encoding_opts.height);
    if (pipeline == NULL)
    {
        DRD_LOG_WARNING("Session %s failed to allocate graphics pipeline", self->peer_address);
        return;
    }

    self->graphics_pipeline = pipeline;
    self->graphics_pipeline_ready = FALSE;
    DRD_LOG_MESSAGE("Session %s graphics pipeline created", self->peer_address);
}

/*
 * 功能：关闭图形管线并回退到 SurfaceBits 传输。
 * 逻辑：若存在管线则记录原因日志，告知 runtime 使用 SurfaceBits，重置 ready 标志并释放 pipeline。
 * 参数：self 会话；reason 关闭原因，可为空。
 * 外部接口：drd_server_runtime_set_transport 设置传输方式；DRD_LOG_WARNING 记录。
 */
static void drd_rdp_session_disable_graphics_pipeline(DrdRdpSession *self, const gchar *reason)
{
    if (self->graphics_pipeline == NULL)
    {
        return;
    }

    if (reason != NULL)
    {
        DRD_LOG_WARNING("Session %s disabling graphics pipeline: %s", self->peer_address, reason);
    }

    if (self->runtime != NULL)
    {
        drd_server_runtime_set_transport(self->runtime, DRD_FRAME_TRANSPORT_SURFACE_BITS);
    }

    self->graphics_pipeline_ready = FALSE;
    g_clear_object(&self->graphics_pipeline);
}

/*
 * 功能：等待 Rdpgfx 管线释放容量。
 * 逻辑：当 pipeline 就绪时调用 drd_rdp_graphics_pipeline_wait_for_capacity，按超时返回。
 * 参数：self 会话；timeout_us 等待时间，-1 表示阻塞。
 * 外部接口：drd_rdp_graphics_pipeline_wait_for_capacity。
 */
static gboolean drd_rdp_session_wait_for_graphics_capacity(DrdRdpSession *self, gint64 timeout_us)
{
    if (self->graphics_pipeline == NULL || !self->graphics_pipeline_ready)
    {
        return FALSE;
    }

    return drd_rdp_graphics_pipeline_wait_for_capacity(self->graphics_pipeline, timeout_us);
}

static void drd_rdp_session_cancel_refresh_timer(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->refresh_timeout_source != 0)
    {
        g_source_remove(self->refresh_timeout_source);
        self->refresh_timeout_source = 0;
    }

    g_atomic_int_set(&self->refresh_timeout_due, 0);
}

static gboolean drd_rdp_session_on_refresh_timeout(gpointer user_data)
{
    DrdRdpSession *self = DRD_RDP_SESSION(user_data);
    self->refresh_timeout_source = 0;

    if (self->runtime != NULL)
    {
        DrdEncodingManager *encoder = drd_server_runtime_get_encoder(self->runtime);
        if (encoder != NULL && drd_encoding_manager_refresh_interval_reached(encoder))
        {
            g_atomic_int_set(&self->refresh_timeout_due, 1);
        }
    }

    return G_SOURCE_REMOVE;
}

static void drd_rdp_session_update_refresh_timer_state(DrdRdpSession *self)
{
    DrdEncodingManager *encoder = NULL;

    if (self->runtime != NULL)
    {
        encoder = drd_server_runtime_get_encoder(self->runtime);
    }

    if (encoder == NULL)
    {
        drd_rdp_session_cancel_refresh_timer(self);
        return;
    }

    const gboolean tracking = drd_encoding_manager_has_avc_to_non_avc_transition(encoder);

    if (!tracking)
    {
        drd_rdp_session_cancel_refresh_timer(self);
        return;
    }

    if (self->refresh_timeout_source == 0)
    {
        const guint refresh_timeout_ms = drd_encoding_manager_get_refresh_timeout_ms(encoder);
        if (refresh_timeout_ms > 0)
        {
            self->refresh_timeout_source = g_timeout_add_full(G_PRIORITY_DEFAULT,
                                                              refresh_timeout_ms,
                                                              drd_rdp_session_on_refresh_timeout,
                                                              g_object_ref(self),
                                                              g_object_unref);
        }
    }
}

/*
 * 功能：尝试将 Progressive 帧提交到 Rdpgfx 管线，处理容量与关键帧要求。
 * 逻辑：若 runtime 或管线未就绪返回 FALSE；首次就绪时切换传输模式；
 *       非 Progressive 帧直接回退 SurfaceBits；在拥塞时等待容量或禁用管线；
 *       提交失败根据错误请求关键帧或关闭管线。
 * 参数：self 会话；frame 已编码帧；out_sent 输出是否成功提交（可为空）。
 * 外部接口：drd_rdp_graphics_pipeline_* 系列检查能力并提交帧，
 *           drd_server_runtime_request_keyframe 触发关键帧重编。
 */
