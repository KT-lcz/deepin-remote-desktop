#include "core/grdc_server_runtime.h"

#include <gio/gio.h>

#define ENCODED_QUEUE_STOP ((gpointer)GINT_TO_POINTER(0x1))

static void grdc_server_runtime_start_encoder(GrdcServerRuntime *self);
static void grdc_server_runtime_stop_encoder(GrdcServerRuntime *self);
static gpointer grdc_server_runtime_encoder_thread(gpointer user_data);
static void grdc_server_runtime_flush_encoded_queue(GrdcServerRuntime *self);
static GrdcEncodedFrame *grdc_server_runtime_wait_encoded(GrdcServerRuntime *self,
                                                          gint64 timeout_us);

struct _GrdcServerRuntime
{
    GObject parent_instance;

    GrdcCaptureManager *capture;
    GrdcEncodingManager *encoder;
    GrdcInputDispatcher *input;
    GrdcTlsCredentials *tls;
    GrdcEncodingOptions encoding_options;
    gboolean has_encoding_options;
    GThread *encoder_thread;
    GAsyncQueue *encoded_queue;
    gint encoder_running;
};

G_DEFINE_TYPE(GrdcServerRuntime, grdc_server_runtime, G_TYPE_OBJECT)

static void
grdc_server_runtime_dispose(GObject *object)
{
    GrdcServerRuntime *self = GRDC_SERVER_RUNTIME(object);
    grdc_server_runtime_stop(self);
    g_clear_object(&self->capture);
    g_clear_object(&self->encoder);
    g_clear_object(&self->input);
    g_clear_object(&self->tls);
    g_clear_pointer(&self->encoded_queue, g_async_queue_unref);

    G_OBJECT_CLASS(grdc_server_runtime_parent_class)->dispose(object);
}

static void
grdc_server_runtime_class_init(GrdcServerRuntimeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = grdc_server_runtime_dispose;
}

static void
grdc_server_runtime_init(GrdcServerRuntime *self)
{
    self->capture = grdc_capture_manager_new();
    self->encoder = grdc_encoding_manager_new();
    self->input = grdc_input_dispatcher_new();
    self->tls = NULL;
    self->has_encoding_options = FALSE;
    self->encoder_thread = NULL;
    self->encoded_queue = g_async_queue_new();
    g_atomic_int_set(&self->encoder_running, 0);
}

GrdcServerRuntime *
grdc_server_runtime_new(void)
{
    return g_object_new(GRDC_TYPE_SERVER_RUNTIME, NULL);
}

GrdcCaptureManager *
grdc_server_runtime_get_capture(GrdcServerRuntime *self)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), NULL);
    return self->capture;
}

GrdcEncodingManager *
grdc_server_runtime_get_encoder(GrdcServerRuntime *self)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), NULL);
    return self->encoder;
}

GrdcInputDispatcher *
grdc_server_runtime_get_input(GrdcServerRuntime *self)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), NULL);
    return self->input;
}

gboolean
grdc_server_runtime_prepare_stream(GrdcServerRuntime *self,
                                   const GrdcEncodingOptions *encoding_options,
                                   GError **error)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(encoding_options != NULL, FALSE);

    self->encoding_options = *encoding_options;
    self->has_encoding_options = TRUE;

    if (!grdc_encoding_manager_prepare(self->encoder, encoding_options, error))
    {
        return FALSE;
    }

    if (!grdc_input_dispatcher_start(self->input,
                                     encoding_options->width,
                                     encoding_options->height,
                                     error))
    {
        grdc_encoding_manager_reset(self->encoder);
        return FALSE;
    }

    if (!grdc_capture_manager_start(self->capture,
                                    encoding_options->width,
                                    encoding_options->height,
                                    error))
    {
        grdc_input_dispatcher_stop(self->input);
        grdc_encoding_manager_reset(self->encoder);
        return FALSE;
    }

    grdc_server_runtime_start_encoder(self);

    g_message("Server runtime prepared stream with geometry %ux%u",
              encoding_options->width,
              encoding_options->height);
    return TRUE;
}

void
grdc_server_runtime_stop(GrdcServerRuntime *self)
{
    g_return_if_fail(GRDC_IS_SERVER_RUNTIME(self));

    grdc_capture_manager_stop(self->capture);
    grdc_server_runtime_stop_encoder(self);
    grdc_encoding_manager_reset(self->encoder);
    grdc_input_dispatcher_flush(self->input);
    grdc_input_dispatcher_stop(self->input);
    self->has_encoding_options = FALSE;
}

gboolean
grdc_server_runtime_pull_encoded_frame(GrdcServerRuntime *self,
                                       gint64 timeout_us,
                                       GrdcEncodedFrame **out_frame,
                                       GError **error)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);
    g_return_val_if_fail(self->encoded_queue != NULL, FALSE);

    GrdcEncodedFrame *encoded = grdc_server_runtime_wait_encoded(self, timeout_us);
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

GrdcFrameCodec
grdc_server_runtime_get_codec(GrdcServerRuntime *self)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), GRDC_FRAME_CODEC_RAW);
    return grdc_encoding_manager_get_codec(self->encoder);
}

gboolean
grdc_server_runtime_get_encoding_options(GrdcServerRuntime *self,
                                         GrdcEncodingOptions *out_options)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), FALSE);
    g_return_val_if_fail(out_options != NULL, FALSE);

    if (!self->has_encoding_options)
    {
        return FALSE;
    }

    *out_options = self->encoding_options;
    return TRUE;
}

void
grdc_server_runtime_set_tls_credentials(GrdcServerRuntime *self, GrdcTlsCredentials *credentials)
{
    g_return_if_fail(GRDC_IS_SERVER_RUNTIME(self));
    if (credentials != NULL)
    {
        g_object_ref(credentials);
    }
    g_clear_object(&self->tls);
    self->tls = credentials;
}

GrdcTlsCredentials *
grdc_server_runtime_get_tls_credentials(GrdcServerRuntime *self)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self), NULL);
    return self->tls;
}

static void
grdc_server_runtime_start_encoder(GrdcServerRuntime *self)
{
    g_return_if_fail(GRDC_IS_SERVER_RUNTIME(self));

    if (self->encoder_thread != NULL)
    {
        return;
    }

    grdc_server_runtime_flush_encoded_queue(self);
    grdc_encoding_manager_force_keyframe(self->encoder);
    g_atomic_int_set(&self->encoder_running, 1);
    self->encoder_thread = g_thread_new("grdc-encoder",
                                        grdc_server_runtime_encoder_thread,
                                        g_object_ref(self));
}

static void
grdc_server_runtime_stop_encoder(GrdcServerRuntime *self)
{
    g_return_if_fail(GRDC_IS_SERVER_RUNTIME(self));

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
    grdc_server_runtime_flush_encoded_queue(self);
}

static gpointer
grdc_server_runtime_encoder_thread(gpointer user_data)
{
    GrdcServerRuntime *self = GRDC_SERVER_RUNTIME(user_data);

    while (g_atomic_int_get(&self->encoder_running))
    {
        g_autoptr(GError) error = NULL;
        GrdcFrame *frame = NULL;
        if (!grdc_capture_manager_wait_frame(self->capture,
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
                g_warning("Encoder thread failed to obtain frame: %s", error->message);
            }
            if (!g_atomic_int_get(&self->encoder_running))
            {
                break;
            }
            continue;
        }

        GrdcEncodedFrame *encoded = NULL;
        if (!grdc_encoding_manager_encode(self->encoder, frame, 0, &encoded, &error))
        {
            if (error != NULL)
            {
                g_warning("Encoder thread failed to encode frame: %s", error->message);
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
grdc_server_runtime_flush_encoded_queue(GrdcServerRuntime *self)
{
    g_return_if_fail(GRDC_IS_SERVER_RUNTIME(self));

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
        GrdcEncodedFrame *frame = GRDC_ENCODED_FRAME(item);
        if (GRDC_IS_ENCODED_FRAME(frame))
        {
            g_object_unref(frame);
        }
    }
}

void
grdc_server_runtime_request_keyframe(GrdcServerRuntime *self)
{
    g_return_if_fail(GRDC_IS_SERVER_RUNTIME(self));
    grdc_server_runtime_flush_encoded_queue(self);
    grdc_encoding_manager_force_keyframe(self->encoder);
}
static gpointer
grdc_server_runtime_pop_queue(GrdcServerRuntime *self, gint64 timeout_us)
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

static GrdcEncodedFrame *
grdc_server_runtime_wait_encoded(GrdcServerRuntime *self, gint64 timeout_us)
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

        gpointer item = grdc_server_runtime_pop_queue(self, timeout_us <= 0 ? timeout_us : remaining);
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

        GrdcEncodedFrame *encoded = GRDC_ENCODED_FRAME(item);
        if (GRDC_IS_ENCODED_FRAME(encoded))
        {
            return encoded;
        }
    }
}
