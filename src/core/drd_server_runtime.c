#include "core/drd_server_runtime.h"

#include <gio/gio.h>

#include "utils/drd_log.h"

#define ENCODED_QUEUE_STOP ((gpointer)GINT_TO_POINTER(0x1))

static void drd_server_runtime_start_encoder(DrdServerRuntime *self);
static void drd_server_runtime_stop_encoder(DrdServerRuntime *self);
static gpointer drd_server_runtime_encoder_thread(gpointer user_data);
static void drd_server_runtime_flush_encoded_queue(DrdServerRuntime *self);
static DrdEncodedFrame *drd_server_runtime_wait_encoded(DrdServerRuntime *self,
                                                          gint64 timeout_us);
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
    GThread *encoder_thread;
    GAsyncQueue *encoded_queue;
    gint encoder_running;
    gint transport_mode;
    GMutex transport_mutex;
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
    g_clear_pointer(&self->encoded_queue, g_async_queue_unref);
    g_mutex_clear(&self->transport_mutex);

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
    self->encoder_thread = NULL;
    self->encoded_queue = g_async_queue_new();
    g_atomic_int_set(&self->encoder_running, 0);
    g_atomic_int_set(&self->transport_mode, DRD_FRAME_TRANSPORT_SURFACE_BITS);
    g_mutex_init(&self->transport_mutex);
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

    drd_server_runtime_start_encoder(self);

    DRD_LOG_MESSAGE("Server runtime prepared stream with geometry %ux%u",
              encoding_options->width,
              encoding_options->height);
    return TRUE;
}

void
drd_server_runtime_stop(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));

    const gboolean had_stream = self->has_encoding_options;
    drd_capture_manager_stop(self->capture);
    drd_server_runtime_stop_encoder(self);
    drd_encoding_manager_reset(self->encoder);
    drd_input_dispatcher_flush(self->input);
    drd_input_dispatcher_stop(self->input);
    self->has_encoding_options = FALSE;

    if (had_stream)
    {
        DRD_LOG_MESSAGE("Server runtime stopped and released capture/encoding resources");
    }
}

gboolean
drd_server_runtime_pull_encoded_frame(DrdServerRuntime *self,
                                       gint64 timeout_us,
                                       DrdEncodedFrame **out_frame,
                                       GError **error)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);
    g_return_val_if_fail(self->encoded_queue != NULL, FALSE);

    DrdEncodedFrame *encoded = drd_server_runtime_wait_encoded(self, timeout_us);
    if (encoded == NULL)
    {
        if (error != NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_TIMED_OUT,
                                "No encoded frame available");
        }
        return FALSE;
    }

    *out_frame = encoded;
    return TRUE;
}

void
drd_server_runtime_set_transport(DrdServerRuntime *self, DrdFrameTransport transport)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));

    g_mutex_lock(&self->transport_mutex);
    DrdFrameTransport current =
        (DrdFrameTransport)g_atomic_int_get(&self->transport_mode);
    if (current == transport)
    {
        g_mutex_unlock(&self->transport_mutex);
        return;
    }

    g_atomic_int_set(&self->transport_mode, transport);
    drd_server_runtime_flush_encoded_queue(self);
    drd_encoding_manager_force_keyframe(self->encoder);
    g_mutex_unlock(&self->transport_mutex);
}

DrdFrameTransport
drd_server_runtime_get_transport(DrdServerRuntime *self)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self), DRD_FRAME_TRANSPORT_SURFACE_BITS);
    return (DrdFrameTransport)g_atomic_int_get(&self->transport_mode);
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

static void
drd_server_runtime_start_encoder(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));

    if (self->encoder_thread != NULL)
    {
        return;
    }

    drd_server_runtime_flush_encoded_queue(self);
    drd_encoding_manager_force_keyframe(self->encoder);
    g_atomic_int_set(&self->encoder_running, 1);
    self->encoder_thread = g_thread_new("drd-encoder",
                                        drd_server_runtime_encoder_thread,
                                        g_object_ref(self));
}

static void
drd_server_runtime_stop_encoder(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));

    if (self->encoder_thread == NULL)
    {
        return;
    }

    g_atomic_int_set(&self->encoder_running, 0);
    if (self->encoded_queue != NULL)
    {
        g_async_queue_push(self->encoded_queue, ENCODED_QUEUE_STOP);
    }
    g_thread_join(self->encoder_thread);
    self->encoder_thread = NULL;
    drd_server_runtime_flush_encoded_queue(self);
}

static gpointer
drd_server_runtime_encoder_thread(gpointer user_data)
{
    DrdServerRuntime *self = DRD_SERVER_RUNTIME(user_data);

    while (g_atomic_int_get(&self->encoder_running))
    {
        g_autoptr(GError) error = NULL;
        DrdFrame *frame = NULL;
        if (!drd_capture_manager_wait_frame(self->capture,
                                             G_GINT64_CONSTANT(16000),
                                             &frame,
                                             &error))
        {
            if (error != NULL && error->domain == G_IO_ERROR &&
                error->code == G_IO_ERROR_TIMED_OUT)
            {
                continue;
            }
            if (error != NULL)
            {
                DRD_LOG_WARNING("Encoder thread failed to obtain frame: %s", error->message);
            }
            if (!g_atomic_int_get(&self->encoder_running))
            {
                break;
            }
            continue;
        }

        DrdFrameCodec codec = drd_server_runtime_resolve_codec(self);
        DrdEncodedFrame *encoded = NULL;
        if (!drd_encoding_manager_encode(self->encoder,
                                          frame,
                                          0,
                                          codec,
                                          &encoded,
                                          &error))
        {
            if (error != NULL)
            {
                DRD_LOG_WARNING("Encoder thread failed to encode frame: %s", error->message);
            }
            g_object_unref(frame);
            continue;
        }

        g_async_queue_push(self->encoded_queue, encoded);
        g_object_unref(frame);
    }

    g_async_queue_push(self->encoded_queue, ENCODED_QUEUE_STOP);
    g_object_unref(self);
    return NULL;
}

static void
drd_server_runtime_flush_encoded_queue(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));

    if (self->encoded_queue == NULL)
    {
        return;
    }

    gpointer item = NULL;
    while ((item = g_async_queue_try_pop(self->encoded_queue)) != NULL)
    {
        if (item == ENCODED_QUEUE_STOP)
        {
            continue;
        }
        DrdEncodedFrame *frame = DRD_ENCODED_FRAME(item);
        if (DRD_IS_ENCODED_FRAME(frame))
        {
            g_object_unref(frame);
        }
    }
}

void
drd_server_runtime_request_keyframe(DrdServerRuntime *self)
{
    g_return_if_fail(DRD_IS_SERVER_RUNTIME(self));
    drd_server_runtime_flush_encoded_queue(self);
    drd_encoding_manager_force_keyframe(self->encoder);
}
static gpointer
drd_server_runtime_pop_queue(DrdServerRuntime *self, gint64 timeout_us)
{
    gpointer item = NULL;

    if (timeout_us < 0)
    {
        item = g_async_queue_pop(self->encoded_queue);
    }
    else if (timeout_us == 0)
    {
        item = g_async_queue_try_pop(self->encoded_queue);
    }
    else
    {
        item = g_async_queue_timeout_pop(self->encoded_queue, timeout_us);
    }

    return item;
}

static DrdEncodedFrame *
drd_server_runtime_wait_encoded(DrdServerRuntime *self, gint64 timeout_us)
{
    g_return_val_if_fail(self->encoded_queue != NULL, NULL);

    const gint64 deadline = (timeout_us > 0) ? g_get_monotonic_time() + timeout_us : 0;

    while (TRUE)
    {
        gint64 remaining = 0;
        if (timeout_us > 0)
        {
            remaining = deadline - g_get_monotonic_time();
            if (remaining <= 0)
            {
                return NULL;
            }
        }

        gpointer item = drd_server_runtime_pop_queue(self, timeout_us <= 0 ? timeout_us : remaining);
        if (item == NULL)
        {
            if (timeout_us == 0)
            {
                return NULL;
            }
            continue;
        }

        if (item == ENCODED_QUEUE_STOP)
        {
            return NULL;
        }

        DrdEncodedFrame *encoded = DRD_ENCODED_FRAME(item);
        if (DRD_IS_ENCODED_FRAME(encoded))
        {
            return encoded;
        }
    }
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
        (DrdFrameTransport)g_atomic_int_get(&self->transport_mode);

    if (transport == DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE)
    {
        return DRD_FRAME_CODEC_RFX_PROGRESSIVE;
    }

    return DRD_FRAME_CODEC_RFX;
}
