#include "utils/drd_frame_queue.h"

struct _DrdFrameQueue
{
    GObject parent_instance;

    GMutex mutex;
    GCond cond;
    DrdFrame *frame;
    gboolean has_frame;
    gboolean running;
};

G_DEFINE_TYPE(DrdFrameQueue, drd_frame_queue, G_TYPE_OBJECT)

static void
drd_frame_queue_dispose(GObject *object)
{
    DrdFrameQueue *self = DRD_FRAME_QUEUE(object);

    g_mutex_lock(&self->mutex);
    g_clear_object(&self->frame);
    self->has_frame = FALSE;
    g_mutex_unlock(&self->mutex);

    G_OBJECT_CLASS(drd_frame_queue_parent_class)->dispose(object);
}

static void
drd_frame_queue_finalize(GObject *object)
{
    DrdFrameQueue *self = DRD_FRAME_QUEUE(object);
    g_mutex_clear(&self->mutex);
    g_cond_clear(&self->cond);
    G_OBJECT_CLASS(drd_frame_queue_parent_class)->finalize(object);
}

static void
drd_frame_queue_class_init(DrdFrameQueueClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_frame_queue_dispose;
    object_class->finalize = drd_frame_queue_finalize;
}

static void
drd_frame_queue_init(DrdFrameQueue *self)
{
    g_mutex_init(&self->mutex);
    g_cond_init(&self->cond);
    self->frame = NULL;
    self->has_frame = FALSE;
    self->running = TRUE;
}

DrdFrameQueue *
drd_frame_queue_new(void)
{
    return g_object_new(DRD_TYPE_FRAME_QUEUE, NULL);
}

void
drd_frame_queue_reset(DrdFrameQueue *self)
{
    g_return_if_fail(DRD_IS_FRAME_QUEUE(self));

    g_mutex_lock(&self->mutex);
    self->running = TRUE;
    self->has_frame = FALSE;
    g_clear_object(&self->frame);
    g_mutex_unlock(&self->mutex);
}

void
drd_frame_queue_push(DrdFrameQueue *self, DrdFrame *frame)
{
    g_return_if_fail(DRD_IS_FRAME_QUEUE(self));
    g_return_if_fail(DRD_IS_FRAME(frame));

    g_mutex_lock(&self->mutex);
    if (!self->running)
    {
        g_mutex_unlock(&self->mutex);
        return;
    }

    g_clear_object(&self->frame);
    self->frame = g_object_ref(frame);
    self->has_frame = TRUE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->mutex);
}

gboolean
drd_frame_queue_wait(DrdFrameQueue *self, gint64 timeout_us, DrdFrame **out_frame)
{
    g_return_val_if_fail(DRD_IS_FRAME_QUEUE(self), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);

    gboolean result = FALSE;

    g_mutex_lock(&self->mutex);
    if (!self->running)
    {
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    gint64 deadline = 0;
    if (timeout_us > 0)
    {
        deadline = g_get_monotonic_time() + timeout_us;
    }

    while (self->running && !self->has_frame)
    {
        if (timeout_us == 0)
        {
            g_mutex_unlock(&self->mutex);
            return FALSE;
        }

        if (timeout_us > 0)
        {
            if (!g_cond_wait_until(&self->cond, &self->mutex, deadline))
            {
                break;
            }
        }
        else
        {
            g_cond_wait(&self->cond, &self->mutex);
        }
    }

    if (self->running && self->has_frame && self->frame != NULL)
    {
        *out_frame = g_object_ref(self->frame);
        self->has_frame = FALSE;
        result = TRUE;
    }

    g_mutex_unlock(&self->mutex);
    return result;
}

void
drd_frame_queue_stop(DrdFrameQueue *self)
{
    g_return_if_fail(DRD_IS_FRAME_QUEUE(self));

    g_mutex_lock(&self->mutex);
    self->running = FALSE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->mutex);
}
