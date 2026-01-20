#include "transport/drd_rdp_listener.h"

#include <gio/gio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/settings.h>
#include <freerdp/input.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/wtsvc.h>
#include <winpr/wtypes.h>
#include <winpr/wtsapi.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include "core/drd_server_runtime.h"
#include "input/drd_input_dispatcher.h"
#include "session/drd_rdp_session.h"
#include "security/drd_tls_credentials.h"
#include "security/drd_pam_auth.h"
#include "security/drd_nla_sam.h"
#include "utils/drd_log.h"
#include "utils/drd_system_info.h"

typedef struct
{
    rdpContext context;
    DrdRdpSession *session;
    DrdServerRuntime *runtime;
    DrdNlaSamFile *nla_sam;
    HANDLE vcm;
    DrdRdpListener *listener;
} DrdRdpPeerContext;

static BOOL drd_rdp_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code);

static BOOL drd_rdp_peer_unicode_event(rdpInput *input, UINT16 flags, UINT16 code);

static BOOL drd_rdp_peer_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);

static BOOL drd_rdp_peer_extended_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);

static BOOL drd_rdp_peer_keyboard_event_x11(rdpInput *input, UINT16 flags, UINT8 code);

static BOOL drd_rdp_peer_unicode_event_x11(rdpInput *input, UINT16 flags, UINT16 code);

static BOOL drd_rdp_peer_pointer_event_x11(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);

static BOOL drd_peer_capabilities(freerdp_peer *client);

static gboolean drd_rdp_listener_has_active_session(DrdRdpListener *self);

static gboolean drd_rdp_listener_session_closed(DrdRdpListener *self, DrdRdpSession *session);

static void drd_rdp_listener_on_session_closed(DrdRdpSession *session, gpointer user_data);

static gboolean drd_rdp_listener_authenticate_tls_login(DrdRdpPeerContext *ctx, freerdp_peer *client);

static gboolean drd_rdp_listener_incoming(GSocketService *service,
                                          GSocketConnection *connection,
                                          GObject *source_object);

static gboolean drd_rdp_listener_connection_keep_open(GSocketConnection *connection);

static void drd_rdp_listener_close_connection(GSocketConnection *connection, gboolean keep_open);

static void drd_rdp_listener_cleanup_peer(freerdp_peer *peer,
                                          GSocketConnection *connection,
                                          gboolean keep_open);

struct _DrdRdpListener
{
    GSocketService parent_instance;

    gchar *bind_address;
    guint16 port;
    GPtrArray *sessions;
    DrdServerRuntime *runtime;
    gchar *nla_username;
    gchar *nla_password;
    gchar *nla_hash;
    gboolean nla_enabled;
    gchar *pam_service;
    DrdRuntimeMode runtime_mode;
    DrdEncodingOptions encoding_options;
    gboolean is_bound;
    GCancellable *cancellable;
    DrdRdpListenerDelegateFunc delegate_func;
    gpointer delegate_data;
    DrdRdpListenerSessionFunc session_cb;
    gpointer session_cb_data;
};

G_DEFINE_TYPE(DrdRdpListener, drd_rdp_listener, G_TYPE_SOCKET_SERVICE)

/*
 * 功能：判断监听器是否运行在 system 模式。
 * 逻辑：检查实例非空且 runtime_mode 为 DRD_RUNTIME_MODE_SYSTEM。
 * 参数：self 监听器。
 * 外部接口：无。
 */
static gboolean
drd_rdp_listener_is_system_mode(DrdRdpListener *self)
{
    return self != NULL && self->runtime_mode == DRD_RUNTIME_MODE_SYSTEM;
}

gboolean drd_rdp_listener_is_single_login(DrdRdpListener *self)
{
    return self!=NULL && self->nla_enabled == FALSE;
}

/*
 * 功能：在 system 模式下根据客户端分辨率更新编码配置。
 * 逻辑：以配置中的编码选项为基准，若客户端提供分辨率则覆盖，并写入 runtime。
 * 参数：self 监听器；client_width/client_height 客户端分辨率。
 * 外部接口：drd_server_runtime_set_encoding_options 更新运行时参数。
 */
static void
drd_rdp_listener_update_system_encoding(DrdRdpListener *self,
                                        guint32 client_width,
                                        guint32 client_height)
{
    if (!drd_rdp_listener_is_system_mode(self))
    {
        return;
    }

    if (self->runtime == NULL)
    {
        return;
    }

    DrdEncodingOptions updated = self->encoding_options;
    if (client_width > 0 && client_height > 0)
    {
        updated.width = client_width;
        updated.height = client_height;
    }

    drd_server_runtime_set_encoding_options(self->runtime, &updated);
}

/*
 * 功能：判断监听器是否处于 handover 模式。
 * 逻辑：检查实例非空且 runtime_mode 为 HANDOVER。
 * 参数：self 监听器。
 * 外部接口：无。
 */
gboolean
drd_rdp_listener_is_handover_mode(DrdRdpListener *self)
{
    return self != NULL && self->runtime_mode == DRD_RUNTIME_MODE_HANDOVER;
}

static void drd_rdp_listener_stop_internal(DrdRdpListener *self);

/*
 * 功能：确保 NLA 使用的 NT hash 已就绪。
 * 逻辑：若未启用 NLA 或已有 hash 直接返回；否则用存储的密码生成 hash，清零原始密码存储。
 * 参数：self 监听器；error 输出错误。
 * 外部接口：调用 drd_nla_sam_hash_password 派生 NTLM hash；GLib g_set_error。
 */
static gboolean
drd_rdp_listener_ensure_nla_hash(DrdRdpListener *self, GError **error)
{
    if (!self->nla_enabled)
    {
        return TRUE;
    }

    if (self->nla_hash != NULL)
    {
        return TRUE;
    }

    if (self->nla_password == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "NLA password is not available");
        return FALSE;
    }

    self->nla_hash = drd_nla_sam_hash_password(self->nla_password);
    if (self->nla_hash == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to derive NT hash for NLA user");
        return FALSE;
    }

    memset(self->nla_password, 0, strlen(self->nla_password));
    g_clear_pointer(&self->nla_password, g_free);
    return TRUE;
}

/*
 * 功能：释放监听器持有的运行时资源。
 * 逻辑：调用内部 stop，释放 runtime 引用，交由父类 dispose。
 * 参数：object GObject 指针。
 * 外部接口：GLib g_clear_object，父类 GObject dispose。
 */
static void
drd_rdp_listener_dispose(GObject *object)
{
    DrdRdpListener *self = DRD_RDP_LISTENER(object);
    drd_rdp_listener_stop_internal(self);
    g_clear_object(&self->runtime);
    G_OBJECT_CLASS(drd_rdp_listener_parent_class)->dispose(object);
}

/*
 * 功能：释放监听器中分配的字符串、数组与敏感信息。
 * 逻辑：清理地址、session 数组、NLA 用户名/密码/hash 等，并交由父类 finalize。
 * 参数：object GObject 指针。
 * 外部接口：GLib g_clear_pointer/g_free；对密码/hash 做 memset 清零。
 */
static void
drd_rdp_listener_finalize(GObject *object)
{
    DrdRdpListener *self = DRD_RDP_LISTENER(object);
    g_clear_pointer(&self->bind_address, g_free);
    g_clear_pointer(&self->sessions, g_ptr_array_unref);
    g_clear_pointer(&self->nla_username, g_free);
    if (self->nla_password != NULL)
    {
        memset(self->nla_password, 0, strlen(self->nla_password));
        g_clear_pointer(&self->nla_password, g_free);
    }
    if (self->nla_hash != NULL)
    {
        memset(self->nla_hash, 0, strlen(self->nla_hash));
        g_clear_pointer(&self->nla_hash, g_free);
    }
    g_clear_pointer(&self->pam_service, g_free);
    G_OBJECT_CLASS(drd_rdp_listener_parent_class)->finalize(object);
}

/*
 * 功能：注册监听器的生命周期与 incoming 回调。
 * 逻辑：设置 dispose/finalize，并将 incoming 指针指向 drd_rdp_listener_incoming。
 * 参数：klass 类结构指针。
 * 外部接口：GLib GSocketServiceClass 挂载 incoming 回调。
 */
static void
drd_rdp_listener_class_init(DrdRdpListenerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_rdp_listener_dispose;
    object_class->finalize = drd_rdp_listener_finalize;

    GSocketServiceClass *service_class = G_SOCKET_SERVICE_CLASS(klass);
    service_class->incoming = drd_rdp_listener_incoming;
}

/*
 * 功能：初始化监听器实例的集合与默认标志。
 * 逻辑：创建 session 数组并清零绑定/回调相关状态。
 * 参数：self 监听器。
 * 外部接口：GLib g_ptr_array_new_with_free_func。
 */
static void
drd_rdp_listener_init(DrdRdpListener *self)
{
    self->sessions = g_ptr_array_new_with_free_func(g_object_unref);
    self->is_bound = FALSE;
    self->cancellable = NULL;
    self->delegate_func = NULL;
    self->delegate_data = NULL;
    self->session_cb = NULL;
    self->session_cb_data = NULL;
}

/*
 * 功能：检查是否存在活跃会话。
 * 逻辑：若 sessions 数组为空或未初始化返回 FALSE，否则根据长度判断。
 * 参数：self 监听器。
 * 外部接口：无。
 */
static gboolean
drd_rdp_listener_has_active_session(DrdRdpListener *self)
{
    if (self == NULL || self->sessions == NULL)
    {
        return FALSE;
    }

    return self->sessions->len > 0;
}

/*
 * 功能：从会话列表中移除关闭的会话并在空闲时停止 runtime。
 * 逻辑：从 sessions 数组移除匹配会话并记录日志；若列表为空则调用 runtime 停止。
 * 参数：self 监听器；session 已关闭的会话。
 * 外部接口：drd_server_runtime_stop 停止流。
 */
static gboolean
drd_rdp_listener_session_closed(DrdRdpListener *self, DrdRdpSession *session)
{
    if (self == NULL || self->sessions == NULL || session == NULL)
    {
        return FALSE;
    }

    if (!g_ptr_array_remove_fast(self->sessions, session))
    {
        return FALSE;
    }

    DRD_LOG_MESSAGE("Detached session %p, %u session(s) remaining",
                    (void *)session,
                    self->sessions->len);

    if (self->sessions->len == 0 && self->runtime != NULL)
    {
        drd_server_runtime_stop(self->runtime);
    }
    return TRUE;
}

/*
 * 功能：作为关闭回调，将会话从监听器列表移除。
 * 逻辑：转换 user_data 为监听器并调用 session_closed。
 * 参数：session 关闭的会话；user_data 监听器。
 * 外部接口：无。
 */
static void
drd_rdp_listener_on_session_closed(DrdRdpSession *session, gpointer user_data)
{
    DrdRdpListener *self = DRD_RDP_LISTENER(user_data);
    if (self == NULL)
    {
        return;
    }

    drd_rdp_listener_session_closed(self, session);
}

/*
 * 功能：创建并初始化 RDP 监听器。
 * 逻辑：校验参数（NLA/PAM/模式），复制绑定地址与凭据，保存编码配置与运行时引用。
 * 参数：bind_address 监听地址；port 端口；runtime 运行时；encoding_options 编码配置；
 *       nla_enabled 是否启用 NLA；nla_username/password NLA 凭据；pam_service PAM 服务名；
 *       runtime_mode 运行模式。
 * 外部接口：GLib g_object_new/g_strdup；drd_server_runtime 由调用方提供。
 */
DrdRdpListener *
drd_rdp_listener_new(const gchar *bind_address,
                     guint16 port,
                     DrdServerRuntime *runtime,
                     const DrdEncodingOptions *encoding_options,
                     gboolean nla_enabled,
                     const gchar *nla_username,
                     const gchar *nla_password,
                     const gchar *pam_service,
                     DrdRuntimeMode runtime_mode)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(runtime), NULL);
    g_return_val_if_fail(pam_service != NULL && *pam_service != '\0', NULL);
    g_return_val_if_fail(runtime_mode == DRD_RUNTIME_MODE_USER ||
                         runtime_mode == DRD_RUNTIME_MODE_SYSTEM ||
                         runtime_mode == DRD_RUNTIME_MODE_HANDOVER,
                         NULL);
    if (nla_enabled)
    {
        g_return_val_if_fail(nla_username != NULL && *nla_username != '\0', NULL);
        g_return_val_if_fail(nla_password != NULL && *nla_password != '\0', NULL);
    }

    DrdRdpListener *self = g_object_new(DRD_TYPE_RDP_LISTENER, NULL);
    self->bind_address = g_strdup(bind_address != NULL ? bind_address : "0.0.0.0");
    self->port = port;
    self->runtime = g_object_ref(runtime);
    self->nla_enabled = nla_enabled;
    self->nla_username = nla_enabled ? g_strdup(nla_username) : NULL;
    self->nla_password = nla_enabled ? g_strdup(nla_password) : NULL;
    self->nla_hash = NULL;
    self->pam_service = g_strdup(pam_service);
    self->runtime_mode = runtime_mode;
    if (encoding_options != NULL)
    {
        self->encoding_options = *encoding_options;
    }
    else
    {
        memset(&self->encoding_options, 0, sizeof(self->encoding_options));
    }
    return self;
}

/*
 * 功能：获取监听器绑定的运行时实例。
 * 逻辑：校验类型后返回 runtime。
 * 参数：self 监听器。
 * 外部接口：无。
 */
DrdServerRuntime *
drd_rdp_listener_get_runtime(DrdRdpListener *self)
{
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), NULL);
    return self->runtime;
}

/*
 * 功能：将 socket 连接转换为可读的“IP:端口”字符串。
 * 逻辑：提取远端地址，若为 IPv4/IPv6 则格式化输出，否则返回 unknown。
 * 参数：connection 套接字连接。
 * 外部接口：GLib GSocketConnection/GInetAddress API。
 */
static gchar *
drd_rdp_listener_describe_connection(GSocketConnection *connection)
{
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), g_strdup("unknown"));

    g_autoptr(GSocketAddress) address = g_socket_connection_get_remote_address(connection, NULL);
    if (address == NULL)
    {
        return g_strdup("unknown");
    }

    if (!G_IS_INET_SOCKET_ADDRESS(address))
    {
        return g_strdup("unknown");
    }

    GInetAddress *inet_address = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address));
    if (inet_address == NULL)
    {
        return g_strdup("unknown");
    }

    g_autofree gchar *ip = g_inet_address_to_string(inet_address);
    if (ip == NULL)
    {
        return g_strdup("unknown");
    }

    const guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(address));
    return g_strdup_printf("%s:%u", ip, port);
}

/*
 * 功能：从 GLib socket 连接构造 FreeRDP peer。
 * 逻辑：获取底层 fd，dup 一份并设置 CLOEXEC，传入 freerdp_peer_new 创建 peer，失败时设置错误。
 * 参数：connection 套接字连接；error 输出错误。
 * 外部接口：GLib GSocket API 获取 fd，POSIX dup/fcntl 设置标志，FreeRDP freerdp_peer_new 创建 peer。
 */
static freerdp_peer *
drd_rdp_listener_peer_from_connection(GSocketConnection *connection, GError **error)
{
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), NULL);

    GSocket *socket = g_socket_connection_get_socket(connection);
    if (socket == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Connection did not expose a socket");
        return NULL;
    }

    const int fd = g_socket_get_fd(socket);
    if (fd < 0)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to retrieve socket file descriptor");
        return NULL;
    }

    const int duplicated_fd = dup(fd);
    if (duplicated_fd < 0)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "dup() failed: %s",
                    g_strerror(errno));
        return NULL;
    }
    int flag = fcntl(duplicated_fd,F_GETFD);
    if (flag < 0)
    {
        DRD_LOG_MESSAGE("fcntl(F_GETFD) failed");
    }
    else
    {
        flag |= O_CLOEXEC;
        fcntl(duplicated_fd, F_SETFD, flag);
    }

    freerdp_peer *peer = freerdp_peer_new(duplicated_fd);
    if (peer == NULL)
    {
        close(duplicated_fd);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "freerdp_peer_new returned null");
        return NULL;
    }

    return peer;
}

/*
 * 功能：分配 peer 上下文并创建对应的会话。
 * 逻辑：将 context 转为 DrdRdpPeerContext，初始化 session/runtime/nla/vcm 等字段，
 *       创建 DrdRdpSession 绑定到 peer。
 * 参数：client FreeRDP peer；context FreeRDP 上下文。
 * 外部接口：调用 drd_rdp_session_new 创建会话。
 */
static BOOL
drd_peer_context_new(freerdp_peer *client, rdpContext *context)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) context;
    ctx->session = drd_rdp_session_new(client);
    ctx->runtime = NULL;
    ctx->nla_sam = NULL;
    ctx->vcm = INVALID_HANDLE_VALUE;
    ctx->listener = NULL;
    return ctx->session != NULL;
}

/*
 * 功能：释放 peer 上下文中的资源。
 * 逻辑：在 listener 中移除 session，释放 session/runtime 引用，释放 NLA SAM 文件，关闭 VCM。
 * 参数：client peer（未使用）；context 上下文。
 * 外部接口：WTSCloseServer 关闭虚拟通道管理器，drd_nla_sam_file_free 释放 SAM。
 */
static void
drd_peer_context_free(freerdp_peer *client G_GNUC_UNUSED, rdpContext *context)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) context;
    if (ctx->session != NULL)
    {
        if (ctx->listener != NULL)
        {
            drd_rdp_listener_session_closed(ctx->listener, ctx->session);
        }
        g_object_unref(ctx->session);
        ctx->session = NULL;
    }

    if (ctx->runtime != NULL)
    {
        g_object_unref(ctx->runtime);
        ctx->runtime = NULL;
    }

    g_clear_pointer(&ctx->nla_sam, drd_nla_sam_file_free);
    if (ctx->vcm != NULL && ctx->vcm != INVALID_HANDLE_VALUE)
    {
        WTSCloseServer(ctx->vcm);
        ctx->vcm = INVALID_HANDLE_VALUE;
    }
    ctx->listener = NULL;
}

/*
 * 功能：处理 FreeRDP PostConnect 回调。
 * 逻辑：调用会话 post_connect，释放 NLA SAM 文件；在非 NLA 模式下执行 TLS/PAM 登录校验。
 * 参数：client peer。
 * 外部接口：FreeRDP 回调机制；drd_rdp_listener_authenticate_tls_login 验证凭据。
 */
static BOOL
drd_peer_post_connect(freerdp_peer *client)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return FALSE;
    }
    BOOL result = drd_rdp_session_post_connect(ctx->session);
    g_clear_pointer(&ctx->nla_sam, drd_nla_sam_file_free);
    if (!result)
    {
        return FALSE;
    }

    if (ctx->listener != NULL && !ctx->listener->nla_enabled)
    {
        if (drd_rdp_listener_is_system_mode(ctx->listener))
        {
            if (!drd_rdp_listener_authenticate_tls_login(ctx, client))
            {
                drd_rdp_session_disconnect(ctx->session, "tls-rdp-sso-auth-failed");
                return FALSE;
            }
        }
    }

    if (ctx->listener != NULL && ctx->listener->session_cb != NULL)
    {
        if (!ctx->listener->session_cb(ctx->listener, ctx->session, ctx->listener->session_cb_data))
        {
            drd_rdp_session_disconnect(ctx->session, "session-callback-failed");
            return FALSE;
        }
    }
    return result;
}

/*
 * 功能：处理 FreeRDP Activate 阶段。
 * 逻辑：直接调用 drd_rdp_session_activate。
 * 参数：client peer。
 * 外部接口：无额外外部库调用。
 */
static BOOL
drd_peer_activate(freerdp_peer *client)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return FALSE;
    }
    return drd_rdp_session_activate(ctx->session);
}

/*
 * 功能：处理 peer 断开事件，更新状态并移除会话。
 * 逻辑：记录日志，更新会话状态为 disconnected，调用 session_closed 移除。
 * 参数：client peer。
 * 外部接口：DRD_LOG_MESSAGE 输出日志。
 */
static void
drd_peer_disconnected(freerdp_peer *client)
{
    DRD_LOG_MESSAGE("%s peer disconnected", client->hostname);
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) client->context;
    if (ctx != NULL && ctx->session != NULL)
    {
        drd_rdp_session_set_peer_state(ctx->session, "disconnected");
        if (ctx->listener != NULL)
        {
            drd_rdp_listener_session_closed(ctx->listener, ctx->session);
        }
    }
}

/*
 * 功能：校验客户端能力并确保 DRDYNVC/桌面尺寸配置满足要求。
 * 逻辑：读取客户端桌面尺寸与 DesktopResize 能力；确认 VCM 加入 DRDYNVC；不符合条件则拒绝。
 * 参数：client peer。
 * 外部接口：freerdp_settings_get_uint32/get_bool 读取能力，DRDYNVC/WTS API 检查通道。
 */
static BOOL
drd_peer_capabilities(freerdp_peer *client)
{
    if (client == NULL || client->context == NULL)
    {
        return FALSE;
    }

    rdpContext *context = client->context;
    rdpSettings *settings = context->settings;
    if (settings == NULL)
    {
        return FALSE;
    }

    const guint32 client_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const guint32 client_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    const gboolean desktop_resize = freerdp_settings_get_bool(settings, FreeRDP_DesktopResize);

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) client->context;
    if (ctx == NULL || ctx->vcm == NULL || ctx->vcm == INVALID_HANDLE_VALUE)
    {
        DRD_LOG_WARNING("Peer %s missing virtual channel manager during capability exchange",
                        client->hostname);
        return FALSE;
    }

    if (!WTSVirtualChannelManagerIsChannelJoined(ctx->vcm, DRDYNVC_SVC_CHANNEL_NAME))
    {
        DRD_LOG_WARNING("Peer %s does not support DRDYNVC, rejecting connection", client->hostname);
        return FALSE;
    }

    if (!desktop_resize)
    {
        if (ctx != NULL && ctx->session != NULL)
        {
            drd_rdp_session_set_peer_state(ctx->session, "desktop-resize-unsupported");
        }
        DRD_LOG_WARNING("Peer %s disabled DesktopResize capability (client %ux%u), rejecting connection",
                        client->hostname,
                        client_width,
                        client_height);
        return FALSE;
    }

    if (ctx != NULL && ctx->listener != NULL)
    {
        drd_rdp_listener_update_system_encoding(ctx->listener, client_width, client_height);
    }

    DRD_LOG_MESSAGE("Peer %s capabilities accepted with DesktopResize enabled (%ux%u requested)",
                    client->hostname,
                    client_width,
                    client_height);
    return TRUE;
}

/*
 * 功能：从 FreeRDP input 上下文获取输入分发器。
 * 逻辑：验证 context 与 runtime 存在后返回 runtime 持有的输入调度器。
 * 参数：input FreeRDP 输入接口。
 * 外部接口：drd_server_runtime_get_input 返回 DrdInputDispatcher。
 */
static DrdInputDispatcher *
drd_peer_get_dispatcher(rdpInput *input)
{
    if (input == NULL || input->context == NULL)
    {
        return NULL;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) input->context;
    if (ctx == NULL || ctx->runtime == NULL)
    {
        return NULL;
    }

    return drd_server_runtime_get_input(ctx->runtime);
}

/*
 * 功能：处理客户端键盘事件并转发到输入分发器。
 * 逻辑：获取 dispatcher 后调用 drd_input_dispatcher_handle_keyboard，失败时记录警告。
 * 参数：input FreeRDP 输入接口；flags 按键标志；code 扫码。
 * 外部接口：drd_input_dispatcher_handle_keyboard 负责注入输入；日志 DRD_LOG_WARNING。
 */
static BOOL
drd_rdp_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code)
{
    DrdInputDispatcher *dispatcher = drd_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!drd_input_dispatcher_handle_keyboard(dispatcher, flags, code, &error) && error != NULL)
    {
        DRD_LOG_WARNING("Keyboard injection failed: %s", error->message);
    }
    return TRUE;
}

/*
 * 功能：适配器函数，将 FreeRDP input 回调转换为 x11_shadow 输入函数调用。
 * 逻辑：直接使用 X11 API 模拟键盘事件，绕过 shadow subsystem。
 * 参数：input FreeRDP 输入接口；flags 按键标志；code 扫码。
 * 外部接口：X11 XTest 扩展。
 */
static BOOL
drd_rdp_peer_keyboard_event_x11(rdpInput *input, UINT16 flags, UINT8 code)
{
    if (input == NULL || input->context == NULL)
    {
        return TRUE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) input->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return TRUE;
    }

    // 获取 X11 显示
    Display *display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        return TRUE;
    }

    DWORD vkcode = 0;
    DWORD keycode = 0;
    DWORD scancode = code;
    BOOL extended = FALSE;

    if (flags & KBD_FLAGS_EXTENDED)
        extended = TRUE;

    if (extended)
        scancode |= KBDEXT;

    vkcode = GetVirtualKeyCodeFromVirtualScanCode(scancode, WINPR_KBD_TYPE_IBM_ENHANCED);

    if (extended)
        vkcode |= KBDEXT;

    keycode = GetKeycodeFromVirtualKeyCode(vkcode, WINPR_KEYCODE_TYPE_XKB);

    DRD_LOG_MESSAGE("Keyboard event conversion: code=%u (0x%02x), flags=0x%04x, extended=%d, "
                    "scancode=%u (0x%04x), vkcode=%u (0x%04x), keycode=%u (0x%04x), event_type=%s",
                    code, code, flags, extended,
                    scancode, scancode, vkcode, vkcode, keycode, keycode,
                    (flags & KBD_FLAGS_RELEASE) ? "release" : "press");

    if (keycode != 0)
    {
        XLockDisplay(display);
        XTestGrabControl(display, True);

        if (flags & KBD_FLAGS_RELEASE)
            XTestFakeKeyEvent(display, keycode, False, CurrentTime);
        else
            XTestFakeKeyEvent(display, keycode, True, CurrentTime);

        XTestGrabControl(display, False);
        XFlush(display);
        XUnlockDisplay(display);
    }

    XCloseDisplay(display);
    return TRUE;
}

/*
 * 功能：适配器函数，将 FreeRDP input 回调转换为 x11_shadow 输入函数调用。
 * 逻辑：直接使用 X11 API 模拟 Unicode 键盘事件，绕过 shadow subsystem。
 * 参数：input FreeRDP 输入接口；flags 按键标志；code Unicode 码点。
 * 外部接口：X11 XTest 扩展。
 */
static BOOL
drd_rdp_peer_unicode_event_x11(rdpInput *input, UINT16 flags, UINT16 code)
{
    if (input == NULL || input->context == NULL)
    {
        return TRUE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) input->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return TRUE;
    }

    // Unicode 输入暂不支持
    return TRUE;
}

/*
 * 功能：适配器函数，将 FreeRDP input 回调转换为 x11_shadow 输入函数调用。
 * 逻辑：直接使用 X11 API 模拟鼠标事件，绕过 shadow subsystem。
 * 参数：input FreeRDP 输入接口；flags 鼠标标志；x/y 坐标。
 * 外部接口：X11 XTest 扩展。
 */
static BOOL
drd_rdp_peer_pointer_event_x11(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    if (input == NULL || input->context == NULL)
    {
        return TRUE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) input->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return TRUE;
    }

    // 获取 X11 显示
    Display *display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        return TRUE;
    }

    unsigned int button = 0;
    BOOL down = FALSE;

    XLockDisplay(display);
    XTestGrabControl(display, True);

    if (flags & PTR_FLAGS_WHEEL)
    {
        BOOL negative = FALSE;

        if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
            negative = TRUE;

        button = (negative) ? 5 : 4;
        XTestFakeButtonEvent(display, button, True, (unsigned long)CurrentTime);
        XTestFakeButtonEvent(display, button, False, (unsigned long)CurrentTime);
    }
    else if (flags & PTR_FLAGS_HWHEEL)
    {
        BOOL negative = FALSE;

        if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
            negative = TRUE;

        button = (negative) ? 7 : 6;
        XTestFakeButtonEvent(display, button, True, (unsigned long)CurrentTime);
        XTestFakeButtonEvent(display, button, False, (unsigned long)CurrentTime);
    }
    else
    {
        if (flags & PTR_FLAGS_MOVE)
            XTestFakeMotionEvent(display, 0, x, y, CurrentTime);

        if (flags & PTR_FLAGS_BUTTON1)
            button = 1;
        else if (flags & PTR_FLAGS_BUTTON2)
            button = 3;
        else if (flags & PTR_FLAGS_BUTTON3)
            button = 2;

        if (flags & PTR_FLAGS_DOWN)
            down = TRUE;

        if (button)
            XTestFakeButtonEvent(display, button, down, CurrentTime);
    }

    XTestGrabControl(display, False);
    XFlush(display);
    XUnlockDisplay(display);
    XCloseDisplay(display);

    return TRUE;
}

/*
 * 功能：处理客户端 Unicode 键盘事件。
 * 逻辑：获取 dispatcher，调用 drd_input_dispatcher_handle_unicode，失败时记录调试日志。
 * 参数：input 输入接口；flags 标志；code Unicode 码点。
 * 外部接口：drd_input_dispatcher_handle_unicode；DRD_LOG_DEBUG 记录不支持提示。
 */
static BOOL
drd_rdp_peer_unicode_event(rdpInput *input, UINT16 flags, UINT16 code)
{
    DrdInputDispatcher *dispatcher = drd_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!drd_input_dispatcher_handle_unicode(dispatcher, flags, code, &error) && error != NULL)
    {
        DRD_LOG_DEBUG("Unicode injection not supported: %s", error->message);
    }
    return TRUE;
}

/*
 * 功能：处理鼠标事件并转发给输入分发器。
 * 逻辑：调用 drd_input_dispatcher_handle_pointer 注入鼠标事件，失败记录警告。
 * 参数：input 输入接口；flags 鼠标标志；x/y 坐标。
 * 外部接口：drd_input_dispatcher_handle_pointer；DRD_LOG_WARNING。
 */
static BOOL
drd_rdp_peer_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    DrdInputDispatcher *dispatcher = drd_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!drd_input_dispatcher_handle_pointer(dispatcher, flags, x, y, &error) && error != NULL)
    {
        DRD_LOG_WARNING("Pointer injection failed: %s", error->message);
    }
    return TRUE;
}

/*
 * 功能：适配器函数，将 FreeRDP input 回调转换为 x11_shadow 输入函数调用。
 * 逻辑：直接使用 X11 API 模拟输入事件，绕过 shadow subsystem。
 * 参数：input FreeRDP 输入接口；flags 标志；code 扫码；x/y 坐标。
 * 外部接口：X11 XTest 扩展。
 */
static BOOL
drd_rdp_peer_extended_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    if (input == NULL || input->context == NULL)
    {
        return TRUE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) input->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return TRUE;
    }

    // 获取 X11 显示
    Display *display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        return TRUE;
    }

    XLockDisplay(display);
    XTestGrabControl(display, True);
    XTestFakeMotionEvent(display, 0, x, y, CurrentTime);

    UINT button = 0;
    BOOL down = FALSE;

    if (flags & PTR_XFLAGS_BUTTON1)
        button = 8;
    else if (flags & PTR_XFLAGS_BUTTON2)
        button = 9;

    if (flags & PTR_XFLAGS_DOWN)
        down = TRUE;

    if (button)
        XTestFakeButtonEvent(display, button, down, CurrentTime);

    XTestGrabControl(display, False);
    XFlush(display);
    XUnlockDisplay(display);
    XCloseDisplay(display);

    return TRUE;
}

/*
 * 功能：根据运行时配置初始化 FreeRDP peer 设置（TLS/NLA/编码模式等）。
 * 逻辑：应用 TLS 证书，配置 NLA SAM 文件或 TLS-only 安全模式，设置桌面尺寸/色深/管线能力，
 *       禁用不需要的功能，按照 handover 模式打开 RDSTLS。
 * 参数：self 监听器；client peer；error 错误输出。
 * 外部接口：大量使用 freerdp_settings_set_* API，drd_tls_credentials_apply 应用证书，
 *           drd_nla_sam_file_new 配置 SAM。
 */
static BOOL
drd_configure_peer_settings(DrdRdpListener *self, freerdp_peer *client, GError **error)
{
    if (client->context == NULL)
    {
        return FALSE;
    }

    rdpSettings *settings = client->context->settings;
    if (settings == NULL)
    {
        return FALSE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) client->context;
    if (ctx == NULL)
    {
        return FALSE;
    }

    DrdTlsCredentials *tls = drd_server_runtime_get_tls_credentials(self->runtime);
    if (tls == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "TLS credentials not configured");
        return FALSE;
    }

    if (!drd_tls_credentials_apply(tls, settings, error))
    {
        return FALSE;
    }

    if (self->nla_enabled)
    {
        if (self->nla_username == NULL ||
            !drd_rdp_listener_ensure_nla_hash(self, error))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "NLA credentials not configured");
            return FALSE;
        }

        g_clear_pointer(&ctx->nla_sam, drd_nla_sam_file_free);
        ctx->nla_sam = drd_nla_sam_file_new(self->nla_username, self->nla_hash, error);
        if (ctx->nla_sam == NULL)
        {
            return FALSE;
        }

        if (!freerdp_settings_set_string(settings,
                                         FreeRDP_NtlmSamFile,
                                         drd_nla_sam_file_get_path(ctx->nla_sam)))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to configure SAM database for NLA");
            return FALSE;
        }
    }
    else
    {
        g_clear_pointer(&ctx->nla_sam, drd_nla_sam_file_free);
        if (!freerdp_settings_set_string(settings, FreeRDP_NtlmSamFile, NULL))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to disable SAM database for TLS login");
            return FALSE;
        }
        DRD_LOG_MESSAGE("Peer %s will authenticate via TLS/PAM service %s",
                        client->hostname,
                        self->pam_service != NULL ? self->pam_service : "unknown");
    }

    const guint32 width = self->encoding_options.width;
    const guint32 height = self->encoding_options.height;
    const gboolean is_virtual_machine = drd_system_is_virtual_machine();
    const gboolean h264_vm_support = self->encoding_options.h264_vm_support;
    const gboolean enable_h264 = h264_vm_support || !is_virtual_machine;
    const gboolean enable_graphics_pipeline =
            (self->encoding_options.mode == DRD_ENCODING_MODE_RFX ||
             self->encoding_options.mode == DRD_ENCODING_MODE_AUTO ||
             self->encoding_options.mode != DRD_ENCODING_MODE_H264);
    if (width == 0 || height == 0)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoding geometry is not configured");
        return FALSE;
    }

    if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
        !freerdp_settings_set_bool(settings, FreeRDP_ServerMode, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxImageCodec, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_GfxH264, enable_h264) ||
        !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE) ||
        !freerdp_settings_set_bool(settings,FreeRDP_GfxProgressive,TRUE) ||
        !freerdp_settings_set_bool(settings,FreeRDP_GfxProgressiveV2,TRUE) ||
        !freerdp_settings_set_bool(settings,FreeRDP_SupportGraphicsPipeline,enable_graphics_pipeline) ||
        !freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_HasQoeEvent, FALSE) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_EncryptionLevel, ENCRYPTION_LEVEL_CLIENT_COMPATIBLE) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_VCFlags, VCCAPS_COMPR_SC) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_VCChunkSize, 16256) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_PointerCacheSize, 100) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 0) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType,OSMAJORTYPE_UNIX) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType,OSMINORTYPE_PSEUDO_XSERVER) ||
        !freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to configure peer settings");
        return FALSE;
    }

    if (is_virtual_machine && !h264_vm_support)
    {
        DRD_LOG_MESSAGE("Virtual machine detected, H264 graphics pipeline is disabled by config");
    }

    if (self->encoding_options.mode == DRD_ENCODING_MODE_RFX)
    {
        if (!freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE) ||
            !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE) ||
            !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE))
        {
            g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to configure h264 settings");
        }
    }

    if (!self->nla_enabled)
    {
        if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
            !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
            !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to configure TLS-only security flags");
            return FALSE;
        }
    }
    else
    {
        if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
            !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, TRUE) ||
            !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to configure peer security flags");
            return FALSE;
        }
    }
    if (drd_rdp_listener_is_handover_mode(self))
    {
        if (!freerdp_settings_set_bool(settings, FreeRDP_RdstlsSecurity, TRUE))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to enable RDSTLS security flags");
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * 功能：接受新的 FreeRDP peer，配置上下文与输入回调。
 * 逻辑：为 peer 分配自定义 context，确保无其他会话占用；配置 peer settings、回调与 VCM，
 *       启动会话事件线程并将会话加入列表，设置输入回调（非 system 模式）。
 * 参数：self 监听器；peer FreeRDP peer；peer_name 日志用对端描述。
 * 外部接口：freerdp_peer_context_new/Initialize、WTSOpenServerA 打开 VCM，
 *           会话相关 drd_rdp_session_* 调用。
 */
static gboolean
drd_rdp_listener_accept_peer(DrdRdpListener *self,
                             freerdp_peer *peer,
                             const gchar *peer_name)
{
    DRD_LOG_MESSAGE("listener accept peer");
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), FALSE);
    g_return_val_if_fail(peer != NULL, FALSE);

    peer->ContextSize = sizeof(DrdRdpPeerContext);
    peer->ContextNew = drd_peer_context_new;
    peer->ContextFree = drd_peer_context_free;

    if (!freerdp_peer_context_new(peer))
    {
        DRD_LOG_WARNING("Failed to allocate peer %s context", peer_name);
        return FALSE;
    }

    if (drd_rdp_listener_has_active_session(self))
    {
        DRD_LOG_WARNING("Rejecting connection from %s: session already active", peer_name);
        return FALSE;
    }

    g_autoptr(GError) settings_error = NULL;
    if (!drd_configure_peer_settings(self, peer, &settings_error))
    {
        if (settings_error != NULL)
        {
            DRD_LOG_WARNING("Failed to configure peer %s settings: %s",
                            peer_name,
                            settings_error->message);
        }
        else
        {
            DRD_LOG_WARNING("Failed to configure peer %s settings", peer_name);
        }
        return FALSE;
    }

    peer->PostConnect = drd_peer_post_connect;
    peer->Activate = drd_peer_activate;
    peer->Disconnect = drd_peer_disconnected;
    peer->Capabilities = drd_peer_capabilities;

    if (peer->Initialize == NULL || !peer->Initialize(peer))
    {
        DRD_LOG_WARNING("Failed to initialize peer %s", peer_name);
        return FALSE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) peer->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        DRD_LOG_WARNING("Peer %s context did not expose a session", peer_name);
        return FALSE;
    }

    drd_rdp_session_set_peer_address(ctx->session, peer_name);

    ctx->vcm = WTSOpenServerA((LPSTR) peer->context);
    if (ctx->vcm == NULL || ctx->vcm == INVALID_HANDLE_VALUE)
    {
        DRD_LOG_WARNING("Peer %s failed to create virtual channel manager", peer_name);
        return FALSE;
    }

    drd_rdp_session_set_virtual_channel_manager(ctx->session, ctx->vcm);

    ctx->runtime = g_object_ref(self->runtime);
    drd_rdp_session_set_runtime(ctx->session, self->runtime);
    drd_rdp_session_set_passive_mode(ctx->session, drd_rdp_listener_is_system_mode(self));

    if (!drd_rdp_session_start_event_thread(ctx->session))
    {
        DRD_LOG_WARNING("Failed to start event thread for peer %s", peer_name);
        return FALSE;
    }

    ctx->listener = self;
    drd_rdp_session_set_peer_state(ctx->session, "initialized");
    g_ptr_array_add(self->sessions, g_object_ref(ctx->session));
    drd_rdp_session_set_closed_callback(ctx->session,
                                        drd_rdp_listener_on_session_closed,
                                        self);

    if (peer->context != NULL && peer->context->input != NULL)
    {
        rdpInput *input = peer->context->input;
        input->context = peer->context;
        if (!drd_rdp_listener_is_system_mode(self))
        {
            input->KeyboardEvent = drd_rdp_peer_keyboard_event_x11;
            input->UnicodeKeyboardEvent = drd_rdp_peer_unicode_event_x11;
            input->MouseEvent = drd_rdp_peer_pointer_event_x11;
            input->ExtendedMouseEvent = drd_rdp_peer_extended_pointer_event;
        }
    }

    DRD_LOG_MESSAGE("Accepted connection from %s", peer_name);
    return TRUE;
}

/*
 * 功能：统一关闭或释放 GLib socket 连接。
 * 逻辑：根据 keep_open 决定是否关闭 IO stream，最后释放引用。
 * 参数：connection 套接字连接；keep_open 是否保持连接打开。
 * 外部接口：GLib g_io_stream_close/g_object_unref。
 */
static void
drd_rdp_listener_close_connection(GSocketConnection *connection, gboolean keep_open)
{
    if (connection == NULL || !G_IS_SOCKET_CONNECTION(connection))
    {
        return;
    }

    if (!keep_open)
    {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    }
    g_object_unref(connection);
}

/*
 * 功能：统一回收 peer 与关联连接的失败分支。
 * 逻辑：释放 peer 后复用连接关闭逻辑，减少重复代码。
 * 参数：peer FreeRDP peer；connection 套接字连接；keep_open 是否保持连接打开。
 * 外部接口：freerdp_peer_free。
 */
static void
drd_rdp_listener_cleanup_peer(freerdp_peer *peer,
                              GSocketConnection *connection,
                              gboolean keep_open)
{
    if (peer != NULL)
    {
        freerdp_peer_free(peer);
    }
    drd_rdp_listener_close_connection(connection, keep_open);
}

/*
 * 功能：处理新的 socket 连接，构造 peer 并交由 accept_peer。
 * 逻辑：生成对端描述，按需求保持连接打开，创建 FreeRDP peer；若接受成功记录 system 元数据；
 *       根据 keep_open 决定是否立即关闭 GLib 连接。
 * 参数：self 监听器；connection 套接字连接；error 错误输出。
 * 外部接口：drd_rdp_listener_peer_from_connection 创建 peer，drd_rdp_listener_accept_peer 完成配置。
 */
static gboolean
drd_rdp_listener_handle_connection(DrdRdpListener *self,
                                   GSocketConnection *connection,
                                   GError **error)
{
    DRD_LOG_MESSAGE("listener handle connection");
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), FALSE);
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);

    g_autofree gchar *peer_name = drd_rdp_listener_describe_connection(connection);
    const gboolean keep_open = drd_rdp_listener_connection_keep_open(connection);
    freerdp_peer *peer = drd_rdp_listener_peer_from_connection(connection, error);
    if (peer == NULL)
    {
        drd_rdp_listener_close_connection(connection, FALSE);
        return FALSE;
    }

    if (!drd_rdp_listener_accept_peer(self, peer, peer_name))
    {
        drd_rdp_listener_cleanup_peer(peer, connection, keep_open);
        return FALSE;
    }

    if (peer->context != NULL)
    {
        DrdRdpPeerContext *ctx = (DrdRdpPeerContext *) peer->context;
        if (ctx != NULL && ctx->session != NULL)
        {
            gpointer system_client = g_object_get_data(G_OBJECT(connection), "drd-system-client");
            drd_rdp_session_set_system_client(ctx->session, system_client);
            drd_rdp_session_set_connection_keep_open(ctx->session, keep_open);
        }
    }

    drd_rdp_listener_close_connection(connection, keep_open);

    return TRUE;
}

/*
 * 功能：GSocketService incoming 回调，处理系统模式委托或直接接受连接。
 * 逻辑：若 system 模式且有 delegate 则先交给 delegate 处理；否则调用 handle_connection，
 *       并在失败时记录日志。
 * 参数：service 套接字服务（监听器自身）；connection 新连接；source_object 未使用。
 * 外部接口：GLib GSocketService 回调机制；日志 DRD_LOG_*。
 */
static gboolean
drd_rdp_listener_incoming(GSocketService *service,
                          GSocketConnection *connection,
                          GObject *source_object G_GNUC_UNUSED)
{
    DrdRdpListener *self = DRD_RDP_LISTENER(service);
    g_autoptr(GError) accept_error = NULL;
    DRD_LOG_MESSAGE("drd_rdp_listener_incoming");

    if (drd_rdp_listener_is_system_mode(self) && self->delegate_func != NULL)
    {
        const gboolean handled =
                self->delegate_func(self, connection, self->delegate_data, &accept_error);

        if (handled)
        {
            // 第二次进这里
            DRD_LOG_MESSAGE("delegate_func run handled");
            if (accept_error != NULL)
            {
                DRD_LOG_WARNING("Delegate reported error while handling connection: %s",
                                accept_error->message);
            }
            return TRUE;
        }
        if (accept_error != NULL)
        {
            DRD_LOG_WARNING("Delegate failed to process connection: %s", accept_error->message);
            return TRUE;
        }
    }

    // 第一次进这里
    if (!drd_rdp_listener_handle_connection(self, g_object_ref(connection), &accept_error))
    {
        if (accept_error != NULL)
        {
            DRD_LOG_WARNING("Failed to handle incoming RDP connection: %s", accept_error->message);
        }
        else
        {
            DRD_LOG_WARNING("Failed to handle incoming RDP connection");
        }
    }

    return TRUE;
}

/*
 * 功能：绑定监听地址并准备 socket listener。
 * 逻辑：防止重复绑定；若绑定到任意地址则 add_inet_port，否则构造指定地址并绑定。
 * 参数：self 监听器；error 输出错误。
 * 外部接口：GLib g_socket_listener_add_inet_port/add_address 完成绑定。
 */
static gboolean
drd_rdp_listener_bind(DrdRdpListener *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), FALSE);

    if (self->is_bound)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_EXISTS,
                            "Listener already bound");
        return FALSE;
    }

    GSocketListener *listener = G_SOCKET_LISTENER(self);
    // g_socket_listener_close(listener);

    if (self->bind_address == NULL || *self->bind_address == '\0' ||
        g_str_equal(self->bind_address, "0.0.0.0") ||
        g_str_equal(self->bind_address, "::"))
    {
        if (!g_socket_listener_add_inet_port(listener, self->port, NULL, error))
        {
            return FALSE;
        }
    }
    else
    {
        g_autoptr(GInetAddress) inet_address = g_inet_address_new_from_string(self->bind_address);
        if (inet_address == NULL)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid bind address %s",
                        self->bind_address);
            return FALSE;
        }

        g_autoptr(GSocketAddress) socket_address =
                g_inet_socket_address_new(inet_address, self->port);

        if (!g_socket_listener_add_address(listener,
                                           socket_address,
                                           G_SOCKET_TYPE_STREAM,
                                           G_SOCKET_PROTOCOL_TCP,
                                           NULL,
                                           NULL,
                                           error))
        {
            return FALSE;
        }
    }

    self->is_bound = TRUE;
    return TRUE;
}

/*
 * 功能：内部停止监听器并清理资源。
 * 逻辑：停止 socket service、关闭 listener、清空 session 列表、停止 runtime、取消 cancellable。
 * 参数：self 监听器。
 * 外部接口：GLib g_socket_service_stop/g_socket_listener_close，drd_server_runtime_stop 停止流。
 */
static void
drd_rdp_listener_stop_internal(DrdRdpListener *self)
{
    if (self->is_bound)
    {
        g_socket_service_stop(G_SOCKET_SERVICE(self));
        g_socket_listener_close(G_SOCKET_LISTENER(self));
        self->is_bound = FALSE;
    }

    g_ptr_array_set_size(self->sessions, 0);

    if (self->runtime != NULL)
    {
        drd_server_runtime_stop(self->runtime);
    }

    if (self->cancellable != NULL)
    {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
}

/*
 * 功能：启动监听器，绑定端口并激活 socket service。
 * 逻辑：调用 bind，必要时创建 cancellable（system 模式），最后启动服务并记录日志。
 * 参数：self 监听器；error 输出错误。
 * 外部接口：GLib g_socket_service_start。
 */
gboolean
drd_rdp_listener_start(DrdRdpListener *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), FALSE);

    if (!drd_rdp_listener_bind(self, error))
    {
        return FALSE;
    }

    if (drd_rdp_listener_is_system_mode(self) && self->cancellable == NULL)
    {
        self->cancellable = g_cancellable_new();
    }

    g_socket_service_start(G_SOCKET_SERVICE(self));
    DRD_LOG_MESSAGE("Socket service armed for %s:%u",
                    self->bind_address != NULL ? self->bind_address : "0.0.0.0",
                    self->port);

    return TRUE;
}

/*
 * 功能：停止监听器（公开接口）。
 * 逻辑：调用内部 stop 清理绑定与 runtime。
 * 参数：self 监听器。
 * 外部接口：无。
 */
void
drd_rdp_listener_stop(DrdRdpListener *self)
{
    g_return_if_fail(DRD_IS_RDP_LISTENER(self));
    drd_rdp_listener_stop_internal(self);
}

/*
 * 功能：设置系统模式下的委托回调。
 * 逻辑：保存 delegate 函数与用户数据。
 * 参数：self 监听器；func 委托；user_data 用户数据。
 * 外部接口：无。
 */
void
drd_rdp_listener_set_delegate(DrdRdpListener *self,
                              DrdRdpListenerDelegateFunc func,
                              gpointer user_data)
{
    g_return_if_fail(DRD_IS_RDP_LISTENER(self));
    self->delegate_func = func;
    self->delegate_data = user_data;
}

/*
 * 功能：注册新会话建立时的回调。
 * 逻辑：保存回调函数与上下文。
 * 参数：self 监听器；func 回调；user_data 用户数据。
 * 外部接口：无。
 */
void
drd_rdp_listener_set_session_callback(DrdRdpListener *self,
                                      DrdRdpListenerSessionFunc func,
                                      gpointer user_data)
{
    g_return_if_fail(DRD_IS_RDP_LISTENER(self));
    self->session_cb = func;
    self->session_cb_data = user_data;
}

/*
 * 功能：将外部传入的 socket 连接纳入监听器处理流程。
 * 逻辑：校验参数后调用 handle_connection。
 * 参数：self 监听器；connection 套接字；error 错误输出。
 * 外部接口：无额外外部库调用。
 */
gboolean
drd_rdp_listener_adopt_connection(DrdRdpListener *self,
                                  GSocketConnection *connection,
                                  GError **error)
{
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), FALSE);
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);

    return drd_rdp_listener_handle_connection(self, connection, error);
}

/*
 * 功能：在 TLS-only 模式下使用 PAM 验证客户端凭据并生成本地会话。
 * 逻辑：读取 FreeRDP settings 中的用户名/密码/域，调用 drd_pam_auth_new 进行 PAM 认证，
 *       成功后附加到会话并清空密码；失败时记录警告。
 * 参数：ctx peer 上下文；client FreeRDP peer。
 * 外部接口：drd_pam_auth_new 进行 PAM/NSS 登录；freerdp_settings_get_string 读取凭据。
 */
static gboolean
drd_rdp_listener_authenticate_tls_login(DrdRdpPeerContext *ctx, freerdp_peer *client)
{
    if (ctx == NULL || ctx->session == NULL || ctx->listener == NULL || client == NULL ||
        client->context == NULL || client->context->settings == NULL)
    {
        return FALSE;
    }

    rdpSettings *settings = client->context->settings;
    const char *username = freerdp_settings_get_string(settings, FreeRDP_Username);
    const char *password = freerdp_settings_get_string(settings, FreeRDP_Password);
    const char *domain = freerdp_settings_get_string(settings, FreeRDP_Domain);
    if (username == NULL || *username == '\0' || password == NULL || *password == '\0')
    {
        DRD_LOG_WARNING("Peer %s missing credentials in TLS client info", client->hostname);
        return FALSE;
    }

    g_autoptr(GError) auth_error = NULL;
    DrdPamAuth *pam_auth =
            drd_pam_auth_new(ctx->listener->pam_service,
                             username,
                             domain,
                             password,
                             client->hostname);

    if (pam_auth == NULL)
    {
        DRD_LOG_WARNING("%s Miss arg", client->hostname);
        return FALSE;
    }
    drd_pam_auth_auth(pam_auth, &auth_error);
    if (auth_error != NULL)
    {
        DRD_LOG_WARNING("Peer %s TLS/PAM single sign-on failure for %s: %s",
                        client->hostname,
                        username,
                        auth_error->message);
        drd_pam_auth_close(pam_auth);
        return FALSE;
    }
    freerdp_settings_set_string(settings, FreeRDP_Password, "");
    drd_rdp_session_attach_pam_auth(ctx->session, pam_auth);
    DRD_LOG_MESSAGE("Peer %s TLS/PAM single sign-on accepted for %s", client->hostname, username);
    return TRUE;
}

/*
 * 功能：判断系统模式下的连接是否需要保持打开（交由 delegate 继续使用）。
 * 逻辑：检查连接对象上是否存在标记数据 drd-system-keep-open。
 * 参数：connection 套接字连接。
 * 外部接口：GLib g_object_get_data。
 */
static gboolean
drd_rdp_listener_connection_keep_open(GSocketConnection *connection)
{
    if (!G_IS_SOCKET_CONNECTION(connection))
    {
        return FALSE;
    }
    return g_object_get_data(G_OBJECT(connection), "drd-system-keep-open") != NULL;
}
