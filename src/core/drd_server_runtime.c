#include "core/drd_server_runtime.h"

#include <gio/gio.h>

#include "utils/drd_log.h"

static DrdFrameCodec drd_server_runtime_resolve_codec(DrdServerRuntime *self);

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
    gint transport_mode;
};

G_DEFINE_TYPE(DrdServerRuntime, drd_server_runtime, G_TYPE_OBJECT)

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

static void
drd_server_runtime_class_init(DrdServerRuntimeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_server_runtime_dispose;
}

static void
drd_server_runtime_init(DrdServerRuntime *self)
{
    self->capture = drd_capture_manager_new();
    self->encoder = drd_encoding_manager_new();
    self->input = drd_input_dispatcher_new();
    self->tls = NULL;
    self->has_encoding_options = FALSE;
    self->stream_running = FALSE;
    g_atomic_int_set(&self->transport_mode, DRD_FRAME_TRANSPORT_SURFACE_BITS);
}

DrdServerRuntime *
drd_server_runtime_new(void)
{
    return g_object_new(DRD_TYPE_SERVER_RUNTIME, NULL);
}

DrdCaptureManager *
drd_server_runtime_get_capture(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->capture;
}

DrdEncodingManager *
drd_server_runtime_get_encoder(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->encoder;
}

DrdInputDispatcher *
drd_server_runtime_get_input(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->input;
}

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
    g_atomic_int_set(&self->transport_mode, DRD_FRAME_TRANSPORT_SURFACE_BITS);

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

gboolean
drd_server_runtime_pull_encoded_frame(DrdServerRuntime *self,
                                      gint64 timeout_us,
                                      DrdEncodedFrame **out_frame,
                                      GError **error)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);
    g_return_val_if_fail(self->capture != NULL, FALSE);

    g_autoptr(DrdFrame) frame = NULL;
    if (!drd_capture_manager_wait_frame(self->capture, timeout_us, &frame, error))
    {
        return FALSE;
    }

    DrdFrameCodec codec = drd_server_runtime_resolve_codec(self);
    return drd_encoding_manager_encode(self->encoder,
                                       frame,
                                       0,
                                       codec,
                                       out_frame,
                                       error);
}

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

DrdFrameTransport
drd_server_runtime_get_transport(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), DRD_FRAME_TRANSPORT_SURFACE_BITS);
    return (DrdFrameTransport) g_atomic_int_get(&self->transport_mode);
}

DrdFrameCodec
drd_server_runtime_get_codec(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), DRD_FRAME_CODEC_RAW);
    return drd_server_runtime_resolve_codec(self);
}

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

void
drd_server_runtime_set_encoding_options(DrdServerRuntime *self,
                                        const DrdEncodingOptions *encoding_options)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));
    g_return_if_fail(encoding_options != NULL);

    const gboolean had_options = self->has_encoding_options;
    const gboolean geometry_changed =
            had_options && (self->encoding_options.width != encoding_options->width ||
                            self->encoding_options.height != encoding_options->height ||
                            self->encoding_options.mode != encoding_options->mode ||
                            self->encoding_options.enable_frame_diff != encoding_options->enable_frame_diff);

    self->encoding_options = *encoding_options;
    self->has_encoding_options = TRUE;

    if (geometry_changed && self->stream_running)
    {
        DRD_LOG_WARNING("Server runtime encoding options changed while stream active, restart required");
    }
}

gboolean
drd_server_runtime_is_stream_running(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    return self->stream_running;
}

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

DrdTlsCredentials *
drd_server_runtime_get_tls_credentials(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), NULL);
    return self->tls;
}

void
drd_server_runtime_request_keyframe(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));
    drd_encoding_manager_force_keyframe(self->encoder);
}

static DrdFrameCodec
drd_server_runtime_resolve_codec(DrdServerRuntime *self)
{
    if (!self->has_encoding_options)
    {
        return DRD_FRAME_CODEC_RAW;
    }

    if (self->encoding_options.mode == DRD_ENCODING_MODE_RAW)
    {
        return DRD_FRAME_CODEC_RAW;
    }

    DrdFrameTransport transport =
            (DrdFrameTransport) g_atomic_int_get(&self->transport_mode);

    if (transport == DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE)
    {
        return DRD_FRAME_CODEC_RFX_PROGRESSIVE;
    }

    return DRD_FRAME_CODEC_RFX;
}
