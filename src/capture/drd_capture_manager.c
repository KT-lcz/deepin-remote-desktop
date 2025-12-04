#include "capture/drd_capture_manager.h"

#include <gio/gio.h>

#include "capture/drd_x11_capture.h"
#include "utils/drd_log.h"

struct _DrdCaptureManager
{
    GObject parent_instance;
    gboolean running;
    DrdFrameQueue *queue;
    DrdX11Capture *x11_capture;
};

G_DEFINE_TYPE(DrdCaptureManager, drd_capture_manager, G_TYPE_OBJECT)

static void
drd_capture_manager_dispose(GObject *object)
{
    DrdCaptureManager *self = DRD_CAPTURE_MANAGER(object);
    if (self->running)
    {
        drd_capture_manager_stop(self);
    }

    g_clear_object(&self->queue);
    g_clear_object(&self->x11_capture);

    G_OBJECT_CLASS(drd_capture_manager_parent_class)->dispose(object);
}

static void
drd_capture_manager_class_init(DrdCaptureManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_capture_manager_dispose;
}

static void
drd_capture_manager_init(DrdCaptureManager *self)
{
    self->running = FALSE;
    self->queue = drd_frame_queue_new();
    self->x11_capture = drd_x11_capture_new(self->queue);
}

DrdCaptureManager *
drd_capture_manager_new(void)
{
    return g_object_new(DRD_TYPE_CAPTURE_MANAGER, NULL);
}

gboolean
drd_capture_manager_start(DrdCaptureManager *self, guint width, guint height, GError **error)
{
    g_return_val_if_fail(DRD_IS_CAPTURE_MANAGER(self), FALSE);

    (void) error;

    if (self->running)
    {
        return TRUE;
    }

    drd_frame_queue_reset(self->queue);

    if (!drd_x11_capture_start(self->x11_capture, NULL, width, height, error))
    {
        drd_frame_queue_stop(self->queue);
        return FALSE;
    }

    DRD_LOG_MESSAGE("Capture manager entering running state");
    self->running = TRUE;
    return TRUE;
}

void
drd_capture_manager_stop(DrdCaptureManager *self)
{
    g_return_if_fail(DRD_IS_CAPTURE_MANAGER(self));

    if (!self->running)
    {
        return;
    }

    drd_x11_capture_stop(self->x11_capture);
    drd_frame_queue_stop(self->queue);

    const guint64 dropped = drd_frame_queue_get_dropped_frames(self->queue);
    if (dropped > 0)
    {
        DRD_LOG_WARNING("Capture manager dropped %" G_GUINT64_FORMAT " frame(s) due to backpressure",
                        dropped);
    }

    DRD_LOG_MESSAGE("Capture manager leaving running state");
    self->running = FALSE;
}

gboolean
drd_capture_manager_is_running(DrdCaptureManager *self)
{
    g_return_val_if_fail(DRD_IS_CAPTURE_MANAGER(self), FALSE);
    return self->running;
}

DrdFrameQueue *
drd_capture_manager_get_queue(DrdCaptureManager *self)
{
    g_return_val_if_fail(DRD_IS_CAPTURE_MANAGER(self), NULL);
    return self->queue;
}

gboolean
drd_capture_manager_wait_frame(DrdCaptureManager *self,
                               gint64 timeout_us,
                               DrdFrame **out_frame,
                               GError **error)
{
    g_return_val_if_fail(DRD_IS_CAPTURE_MANAGER(self), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);

    if (!self->running)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Capture manager is not running");
        return FALSE;
    }

    if (!drd_frame_queue_wait(self->queue, timeout_us, out_frame))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_TIMED_OUT,
                            "Timed out waiting for capture frame");
        return FALSE;
    }

    return TRUE;
}
