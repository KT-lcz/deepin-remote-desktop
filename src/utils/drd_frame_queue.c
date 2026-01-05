#include "utils/drd_frame_queue.h"

struct _DrdFrameQueue
{
    GObject parent_instance;

    GMutex mutex;
    GCond cond;
    DrdFrame *frames[DRD_FRAME_QUEUE_MAX_FRAMES];
    guint head;
    guint tail;
    guint size;
    gboolean running;
    guint64 dropped_frames;
};

G_DEFINE_TYPE(DrdFrameQueue, drd_frame_queue, G_TYPE_OBJECT)

/*
 * 功能：释放帧队列中的帧对象。
 * 逻辑：持锁清理环形缓冲的帧引用并重置计数，随后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdFrameQueue。
 * 外部接口：GLib g_clear_object；互斥锁保护。
 */
static void
drd_frame_queue_dispose(GObject *object)
{
    DrdFrameQueue *self = DRD_FRAME_QUEUE(object);

    g_mutex_lock(&self->mutex);
    for (guint i = 0; i < DRD_FRAME_QUEUE_MAX_FRAMES; ++i)
    {
        g_clear_object(&self->frames[i]);
    }
    self->size = 0;
    g_mutex_unlock(&self->mutex);

    G_OBJECT_CLASS(drd_frame_queue_parent_class)->dispose(object);
}

/*
 * 功能：释放互斥量与条件变量。
 * 逻辑：清理锁与条件后交由父类 finalize。
 * 参数：object 基类指针。
 * 外部接口：GLib g_mutex_clear/g_cond_clear。
 */
static void
drd_frame_queue_finalize(GObject *object)
{
    DrdFrameQueue *self = DRD_FRAME_QUEUE(object);
    g_mutex_clear(&self->mutex);
    g_cond_clear(&self->cond);
    G_OBJECT_CLASS(drd_frame_queue_parent_class)->finalize(object);
}

/*
 * 功能：绑定类级别析构回调。
 * 逻辑：将自定义 dispose/finalize 挂载到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void
drd_frame_queue_class_init(DrdFrameQueueClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_frame_queue_dispose;
    object_class->finalize = drd_frame_queue_finalize;
}

/*
 * 功能：初始化帧队列的锁、条件和缓冲。
 * 逻辑：初始化互斥锁/条件变量，清空环形缓冲并设置运行标志与计数。
 * 参数：self 队列实例。
 * 外部接口：GLib g_mutex_init/g_cond_init。
 */
static void
drd_frame_queue_init(DrdFrameQueue *self)
{
    g_mutex_init(&self->mutex);
    g_cond_init(&self->cond);
    for (guint i = 0; i < DRD_FRAME_QUEUE_MAX_FRAMES; ++i)
    {
        self->frames[i] = NULL;
    }
    self->head = 0;
    self->tail = 0;
    self->size = 0;
    self->running = TRUE;
    self->dropped_frames = 0;
}

/*
 * 功能：创建帧队列对象。
 * 逻辑：调用 g_object_new 分配实例。
 * 参数：无。
 * 外部接口：GLib g_object_new。
 */
DrdFrameQueue *
drd_frame_queue_new(void)
{
    return g_object_new(DRD_TYPE_FRAME_QUEUE, NULL);
}

/*
 * 功能：重置队列状态并清空缓冲。
 * 逻辑：持锁恢复 running，清理所有帧引用，重置头尾指针与统计，并广播条件唤醒等待线程。
 * 参数：self 队列实例。
 * 外部接口：GLib g_clear_object；互斥锁保护。
 */
void
drd_frame_queue_reset(DrdFrameQueue *self)
{
    g_return_if_fail(DRD_IS_FRAME_QUEUE(self));

    g_mutex_lock(&self->mutex);
    self->running = TRUE;
    for (guint i = 0; i < DRD_FRAME_QUEUE_MAX_FRAMES; ++i)
    {
        g_clear_object(&self->frames[i]);
        self->frames[i] = NULL;
    }
    self->head = 0;
    self->tail = 0;
    self->size = 0;
    self->dropped_frames = 0;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->mutex);
}

/*
 * 功能：向队列推入一帧，满容量时丢弃最旧帧。
 * 逻辑：持锁检查运行状态；满队列时移除头部帧并累加丢弃计数；将新帧写入尾部并广播条件。
 * 参数：self 队列实例；frame 待推入帧。
 * 外部接口：GLib g_clear_object/g_cond_broadcast；互斥锁保护。
 */
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

    if (self->size == DRD_FRAME_QUEUE_MAX_FRAMES)
    {
        g_clear_object(&self->frames[self->head]);
        self->frames[self->head] = NULL;
        self->head = (self->head + 1) % DRD_FRAME_QUEUE_MAX_FRAMES;
        self->size--;
        self->dropped_frames++;
    }

    self->frames[self->tail] = g_object_ref(frame);
    self->tail = (self->tail + 1) % DRD_FRAME_QUEUE_MAX_FRAMES;
    self->size++;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->mutex);
}

/*
 * 功能：阻塞等待一帧输出，可选超时。
 * 逻辑：持锁检查运行状态；根据超时策略等待条件；取出头部帧返回并缩减大小。
 * 参数：self 队列实例；timeout_us 超时时间（微秒，0 为立即返回，<0 为无限等待）；out_frame 输出帧。
 * 外部接口：GLib g_cond_wait/g_cond_wait_until/g_get_monotonic_time；互斥锁保护。
 */
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

    while (self->running && self->size == 0)
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

    if (self->running && self->size > 0)
    {
        DrdFrame *frame = self->frames[self->head];
        self->frames[self->head] = NULL;
        self->head = (self->head + 1) % DRD_FRAME_QUEUE_MAX_FRAMES;
        self->size--;
        if (frame != NULL)
        {
            *out_frame = g_object_ref(frame);
            g_clear_object(&frame);
        }
        else
        {
            *out_frame = NULL;
        }
        result = TRUE;
    }

    g_mutex_unlock(&self->mutex);
    return result;
}

/*
 * 功能：停止队列，唤醒所有等待者。
 * 逻辑：持锁将 running 置 FALSE 并广播条件。
 * 参数：self 队列实例。
 * 外部接口：GLib g_cond_broadcast；互斥锁保护。
 */
void
drd_frame_queue_stop(DrdFrameQueue *self)
{
    g_return_if_fail(DRD_IS_FRAME_QUEUE(self));

    g_mutex_lock(&self->mutex);
    self->running = FALSE;
    g_cond_broadcast(&self->cond);
    g_mutex_unlock(&self->mutex);
}

/*
 * 功能：获取队列累计丢帧数。
 * 逻辑：持锁读取 dropped_frames 并返回。
 * 参数：self 队列实例。
 * 外部接口：互斥锁保护。
 */
guint64
drd_frame_queue_get_dropped_frames(DrdFrameQueue *self)
{
    g_return_val_if_fail(DRD_IS_FRAME_QUEUE(self), 0);

    g_mutex_lock(&self->mutex);
    guint64 dropped = self->dropped_frames;
    g_mutex_unlock(&self->mutex);
    return dropped;
}
