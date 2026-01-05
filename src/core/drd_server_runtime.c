#include "core/drd_server_runtime.h"

#include <freerdp/settings.h>
#include <gio/gio.h>

#include "utils/drd_log.h"

struct _DrdServerRuntime
{
    GObject parent_instance;

    DrdCaptureManager *capture;
    DrdEncodingManager *encoder;
    DrdInputDispatcher *input;
    DrdTlsCredentials *tls;
    DrdEncodingOptions encoding_options;
    gboolean has_encoding_options;
    gboolean stream_running;
    DrdFrameTransport transport_mode;
};

G_DEFINE_TYPE(DrdServerRuntime, drd_server_runtime, G_TYPE_OBJECT)

/*
 * 功能：释放运行时持有的模块资源。
 * 逻辑：调用 stop 停止流后，依次释放 capture/encoder/input/TLS 对象，再交给父类 dispose。
 * 参数：object 基类指针，期望为 DrdServerRuntime。
 * 外部接口：drd_server_runtime_stop 关闭模块；GLib g_clear_object；GObjectClass::dispose。
 */
static void
drd_server_runtime_dispose(GObject *object)
{
    DrdServerRuntime *self = DRD_SERVER_RUNTIME(object);
    drd_server_runtime_stop(self);
    g_clear_object(&self->capture);
    g_clear_object(&self->encoder);
    g_clear_object(&self->input);
    g_clear_object(&self->tls);

    G_OBJECT_CLASS(drd_server_runtime_parent_class)->dispose(object);
}

/*
 * 功能：绑定类级别的析构回调。
 * 逻辑：将自定义 dispose 挂载到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void
drd_server_runtime_class_init(DrdServerRuntimeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_server_runtime_dispose;
}

/*
 * 功能：初始化运行时对象的成员。
 * 逻辑：创建捕获/编码/输入子模块，初始化标志位与默认传输模式。
 * 参数：self 运行时实例。
 * 外部接口：drd_capture_manager_new、drd_encoding_manager_new、drd_input_dispatcher_new 创建子组件；GLib g_atomic_int_set 设置原子值。
 */
static void
drd_server_runtime_init(DrdServerRuntime *self)
{
    self->capture = drd_capture_manager_new();
    self->encoder = drd_encoding_manager_new();
    self->input = drd_input_dispatcher_new();
    self->tls = NULL;
    self->has_encoding_options = FALSE;
    self->stream_running = FALSE;
    g_atomic_int_set(&self->transport_mode, DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE);
}

/*
 * 功能：构造新的运行时实例。
 * 逻辑：调用 g_object_new 创建对象。
 * 参数：无。
 * 外部接口：GLib g_object_new。
 */
DrdServerRuntime *
drd_server_runtime_new(void)
{
    return g_object_new(DRD_TYPE_SERVER_RUNTIME, NULL);
}

/*
 * 功能：获取捕获管理器。
 * 逻辑：类型检查后返回内部捕获指针。
 * 参数：self 运行时实例。
 * 外部接口：无额外外部库。
 */
DrdCaptureManager *
drd_server_runtime_get_capture(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->capture;
}

/*
 * 功能：获取编码管理器。
 * 逻辑：类型检查后返回编码器指针。
 * 参数：self 运行时实例。
 * 外部接口：无额外外部库。
 */
DrdEncodingManager *
drd_server_runtime_get_encoder(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->encoder;
}

/*
 * 功能：获取输入分发器。
 * 逻辑：类型检查后返回输入组件指针。
 * 参数：self 运行时实例。
 * 外部接口：无额外外部库。
 */
DrdInputDispatcher *
drd_server_runtime_get_input(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->input;
}

/*
 * 功能：准备捕获/编码/输入流水线并启动捕获线程。
 * 逻辑：若已运行则直接返回；缓存编码配置并设置默认传输模式；依次准备编码器、输入分发器与捕获管理器，任一失败则回滚已启动的模块；成功后标记 stream_running。
 * 参数：self 运行时实例；encoding_options 编码选项；error 错误输出。
 * 外部接口：drd_encoding_manager_prepare/reset、drd_input_dispatcher_start/stop、drd_capture_manager_start；日志 DRD_LOG_MESSAGE。
 */
gboolean
drd_server_runtime_prepare_stream(DrdServerRuntime *self,
                                  const DrdEncodingOptions *encoding_options,
                                  GError **error)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(encoding_options != NULL, FALSE);
    if (self->stream_running)
    {
        DRD_LOG_MESSAGE("Server runtime stream already running, skipping prepare");
        return TRUE;
    }

    self->encoding_options = *encoding_options;
    self->has_encoding_options = TRUE;
    g_atomic_int_set(&self->transport_mode, DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE);

    if (!drd_encoding_manager_prepare(self->encoder, encoding_options, error))
    {
        return FALSE;
    }

    if (!drd_input_dispatcher_start(self->input,
                                    encoding_options->width,
                                    encoding_options->height,
                                    error))
    {
        drd_encoding_manager_reset(self->encoder);
        return FALSE;
    }

    if (!drd_capture_manager_start(self->capture,
                                   encoding_options->width,
                                   encoding_options->height,
                                   error))
    {
        drd_input_dispatcher_stop(self->input);
        drd_encoding_manager_reset(self->encoder);
        return FALSE;
    }

    self->stream_running = TRUE;
    DRD_LOG_MESSAGE("Server runtime prepared stream with geometry %ux%u",
                    encoding_options->width,
                    encoding_options->height);
    return TRUE;
}

/*
 * 功能：停止正在运行的捕获/编码流水线。
 * 逻辑：若未运行则返回；清除运行标志后停止捕获、重置编码器并刷新/停止输入分发器。
 * 参数：self 运行时实例。
 * 外部接口：drd_capture_manager_stop、drd_encoding_manager_reset、drd_input_dispatcher_flush/stop；日志 DRD_LOG_MESSAGE。
 */
void
drd_server_runtime_stop(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));

    if (!self->stream_running)
    {
        return;
    }

    self->stream_running = FALSE;
    drd_capture_manager_stop(self->capture);
    drd_encoding_manager_reset(self->encoder);
    drd_input_dispatcher_flush(self->input);
    drd_input_dispatcher_stop(self->input);
    DRD_LOG_MESSAGE("Server runtime stopped and released capture/encoding resources");
}

gboolean drd_server_runtime_pull_encoded_frame_surface_gfx(DrdServerRuntime *self,
                                                           rdpSettings *settings,
                                                           RdpgfxServerContext *context,
                                                           guint16 surface_id,
                                                           gint64 timeout_us,
                                                           guint32 frame_id,
                                                           gboolean *h264,
                                                           GError **error)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(self->capture != NULL, FALSE);
    g_return_val_if_fail(settings != NULL, FALSE);

    const gboolean auto_switch = self->has_encoding_options &&
                                 self->encoding_options.mode == DRD_ENCODING_MODE_AUTO;

    g_autoptr(DrdFrame) frame = NULL;
    g_autoptr(GError) capture_error = NULL;
    if (!drd_capture_manager_wait_frame(self->capture, timeout_us, &frame, &capture_error))
    {
        const gboolean refresh_due = drd_encoding_manager_refresh_interval_reached(self->encoder);

        if (capture_error != NULL && capture_error->domain == G_IO_ERROR &&
            capture_error->code == G_IO_ERROR_TIMED_OUT && refresh_due)
        {
            g_clear_error(&capture_error);
            return drd_encoding_manager_encode_cached_frame_gfx(self->encoder,
                                                                settings,
                                                                context,
                                                                surface_id,
                                                                frame_id,
                                                                h264,
                                                                auto_switch,
                                                                error);
        }

        if (error != NULL)
        {
            *error = capture_error;
            capture_error = NULL;
        }
        return FALSE;
    }

    return drd_encoding_manager_encode_surface_gfx(self->encoder,
                                                   settings,
                                                   context,
                                                   surface_id,
                                                   frame,
                                                   frame_id,
                                                   h264,
                                                   auto_switch,
                                                   error);
}

gboolean drd_server_runtime_send_cached_frame_surface_gfx(DrdServerRuntime *self,
                                                          rdpSettings *settings,
                                                          RdpgfxServerContext *context,
                                                          guint16 surface_id,
                                                          guint32 frame_id,
                                                          gboolean *h264,
                                                          GError **error)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(self->encoder != NULL, FALSE);
    g_return_val_if_fail(settings != NULL, FALSE);
    g_return_val_if_fail(context != NULL, FALSE);

    const gboolean auto_switch = self->has_encoding_options &&
                                 self->encoding_options.mode == DRD_ENCODING_MODE_AUTO;

    return drd_encoding_manager_encode_cached_frame_gfx(self->encoder,
                                                        settings,
                                                        context,
                                                        surface_id,
                                                        frame_id,
                                                        h264,
                                                        auto_switch,
                                                        error);
}

gboolean drd_server_runtime_pull_encoded_frame_surface_bit(DrdServerRuntime *self,
                                                           rdpContext *context,
                                                           guint32 frame_id,
                                                           gsize max_payload,
                                                           gint64 timeout_us,
                                                           GError **error)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(self->capture != NULL, FALSE);
    g_return_val_if_fail(context != NULL, FALSE);

    g_autoptr(DrdFrame) frame = NULL;
    if (!drd_capture_manager_wait_frame(self->capture, timeout_us, &frame, error))
    {
        return FALSE;
    }

    return drd_encoding_manager_encode_surface_bit(self->encoder,
                                                   context,
                                                   frame,
                                                   frame_id,
                                                   max_payload,
                                                   error);
}

/*
 * 功能：切换帧传输模式并在变更时请求关键帧。
 * 逻辑：使用原子 CAS 更新 transport_mode；仅当实际发生切换时触发编码器关键帧，避免互斥锁竞争。
 * 参数：self 运行时实例；transport 目标传输模式。
 * 外部接口：GLib g_atomic_int_get/g_atomic_int_compare_and_exchange；调用 drd_encoding_manager_force_keyframe。
 */
void
drd_server_runtime_set_transport(DrdServerRuntime *self, DrdFrameTransport transport)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));

    const gint desired = (gint) transport;
    while (TRUE)
    {
        const gint current = g_atomic_int_get(&self->transport_mode);
        if (current == desired)
        {
            return;
        }
        /*
         * CAS 确保只有实际完成模式切换的线程触发关键帧，
         * 避免原来的互斥锁带来的不必要竞争。
         */
        if (g_atomic_int_compare_and_exchange(&self->transport_mode, current, desired))
        {
            drd_encoding_manager_force_keyframe(self->encoder);
            return;
        }
    }
}

/*
 * 功能：读取当前传输模式。
 * 逻辑：使用原子读返回枚举值。
 * 参数：self 运行时实例。
 * 外部接口：GLib g_atomic_int_get。
 */
DrdFrameTransport
drd_server_runtime_get_transport(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), DRD_FRAME_TRANSPORT_SURFACE_BITS);
    return (DrdFrameTransport) g_atomic_int_get(&self->transport_mode);
}

/*
 * 功能：获取已缓存的编码参数。
 * 逻辑：若未设置编码选项则返回 FALSE；否则将结构体复制到输出参数。
 * 参数：self 运行时实例；out_options 输出编码选项。
 * 外部接口：无额外外部库。
 */
gboolean
drd_server_runtime_get_encoding_options(DrdServerRuntime *self,
                                        DrdEncodingOptions *out_options)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(out_options != NULL, FALSE);

    if (!self->has_encoding_options)
    {
        return FALSE;
    }

    *out_options = self->encoding_options;
    return TRUE;
}

/*
 * 功能：写入编码参数并检测几何变化。
 * 逻辑：缓存新配置并标记已设置；若几何或模式变化且流已运行则提示需要重启。
 * 参数：self 运行时实例；encoding_options 新编码配置。
 * 外部接口：日志 DRD_LOG_WARNING。
 */
void
drd_server_runtime_set_encoding_options(DrdServerRuntime *self,
                                        const DrdEncodingOptions *encoding_options)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));
    g_return_if_fail(encoding_options != NULL);

    const gboolean had_options = self->has_encoding_options;
    const gboolean options_changed = had_options &&
                                     (self->encoding_options.width != encoding_options->width ||
                                      self->encoding_options.height != encoding_options->height ||
                                      self->encoding_options.mode != encoding_options->mode ||
                                      self->encoding_options.enable_frame_diff != encoding_options->enable_frame_diff ||
                                      self->encoding_options.h264_bitrate != encoding_options->h264_bitrate ||
                                      self->encoding_options.h264_framerate != encoding_options->h264_framerate ||
                                      self->encoding_options.h264_qp != encoding_options->h264_qp ||
                                      self->encoding_options.gfx_large_change_threshold !=
                                              encoding_options->gfx_large_change_threshold ||
                                      self->encoding_options.gfx_progressive_refresh_interval !=
                                              encoding_options->gfx_progressive_refresh_interval ||
                                      self->encoding_options.gfx_progressive_refresh_timeout_ms !=
                                              encoding_options->gfx_progressive_refresh_timeout_ms);

    self->encoding_options = *encoding_options;
    self->has_encoding_options = TRUE;

    if (options_changed && self->stream_running)
    {
        DRD_LOG_WARNING("Server runtime encoding options changed while stream active, restart required");
    }
}

/*
 * 功能：查询流是否正在运行。
 * 逻辑：类型检查后返回 stream_running 标志。
 * 参数：self 运行时实例。
 * 外部接口：无额外外部库。
 */
gboolean
drd_server_runtime_is_stream_running(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    return self->stream_running;
}

/*
 * 功能：设置 TLS 凭据。
 * 逻辑：引用计数新凭据并替换旧值。
 * 参数：self 运行时实例；credentials TLS 凭据。
 * 外部接口：GLib g_object_ref/g_clear_object。
 */
void
drd_server_runtime_set_tls_credentials(DrdServerRuntime *self, DrdTlsCredentials *credentials)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));
    if (credentials != NULL)
    {
        g_object_ref(credentials);
    }
    g_clear_object(&self->tls);
    self->tls = credentials;
}

/*
 * 功能：获取 TLS 凭据。
 * 逻辑：类型检查后返回内部指针。
 * 参数：self 运行时实例。
 * 外部接口：无额外外部库。
 */
DrdTlsCredentials *
drd_server_runtime_get_tls_credentials(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->tls;
}

/*
 * 功能：请求编码器生成关键帧。
 * 逻辑：直接调用编码管理器的强制关键帧接口。
 * 参数：self 运行时实例。
 * 外部接口：drd_encoding_manager_force_keyframe。
 */
void
drd_server_runtime_request_keyframe(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));
    drd_encoding_manager_force_keyframe(self->encoder);
}
gboolean drd_runtime_encoder_prepare(DrdServerRuntime *self, guint32 codecs, rdpSettings *settings)
{
    return drd_encoder_prepare(self->encoder, codecs, settings);
}
