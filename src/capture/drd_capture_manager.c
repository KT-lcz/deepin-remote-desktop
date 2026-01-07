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

/*
 * 功能：释放捕获管理器持有的资源并处理运行中状态。
 * 逻辑：若仍在运行则先调用 stop；随后清理队列与 X11 捕获实例，最后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdCaptureManager 实例。
 * 外部接口：GLib 的 g_clear_object 负责引用计数释放，最终调用 GObjectClass::dispose。
 */
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

/*
 * 功能：初始化捕获管理器的类方法表。
 * 逻辑：将自定义 dispose 覆盖到 GObjectClass，以便释放内部对象。
 * 参数：klass 类对象。
 * 外部接口：依赖 GLib 类型系统进行类初始化。
 */
static void
drd_capture_manager_class_init(DrdCaptureManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_capture_manager_dispose;
}

/*
 * 功能：初始化捕获管理器实例字段。
 * 逻辑：默认置 running 为 FALSE，创建帧队列并实例化 X11 捕获对象。
 * 参数：self 捕获管理器实例。
 * 外部接口：调用 drd_frame_queue_new、drd_x11_capture_new 创建内部组件。
 */
static void
drd_capture_manager_init(DrdCaptureManager *self)
{
    self->running = FALSE;
    self->queue = drd_frame_queue_new();
    self->x11_capture = drd_x11_capture_new(self->queue);
}

/*
 * 功能：创建新的捕获管理器对象。
 * 逻辑：使用 g_object_new 分配并初始化实例。
 * 参数：无。
 * 外部接口：GLib 的 g_object_new。
 */
DrdCaptureManager *
drd_capture_manager_new(void)
{
    return g_object_new(DRD_TYPE_CAPTURE_MANAGER, NULL);
}

/*
 * 功能：启动 X11 捕获线程并准备帧队列。
 * 逻辑：若已运行直接返回；重置队列后启动 X11 捕获，失败则停止队列并返回错误；成功时更新 running 标志。
 * 参数：self 管理器；width/height 期望分辨率；error 输出错误信息。
 * 外部接口：调用 drd_frame_queue_reset / drd_frame_queue_stop 控制队列，drd_x11_capture_start 启动捕获；日志通过 DRD_LOG_MESSAGE。
 */
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

/*
 * 功能：停止捕获线程并清理队列。
 * 逻辑：若未运行直接返回；先停止 X11 捕获与队列，再输出丢帧统计并清除 running 标志。
 * 参数：self 管理器实例。
 * 外部接口：调用 drd_x11_capture_stop、drd_frame_queue_stop、drd_frame_queue_get_dropped_frames，日志使用 DRD_LOG_WARNING/DRD_LOG_MESSAGE。
 */
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

/*
 * 功能：查询捕获管理器是否处于运行状态。
 * 逻辑：类型校验后返回 running 标志。
 * 参数：self 管理器实例。
 * 外部接口：无额外外部库依赖。
 */
gboolean
drd_capture_manager_is_running(DrdCaptureManager *self)
{
    g_return_val_if_fail(DRD_IS_CAPTURE_MANAGER(self), FALSE);
    return self->running;
}

/*
 * 功能：获取当前显示的实际分辨率。
 * 逻辑：委托 X11 捕获模块读取 Display 宽高。
 * 参数：self 捕获管理器；out_width/out_height 输出值；error 错误输出。
 * 外部接口：drd_x11_capture_get_display_size。
 */
gboolean
drd_capture_manager_get_display_size(DrdCaptureManager *self,
                                     guint *out_width,
                                     guint *out_height,
                                     GError **error)
{
    g_return_val_if_fail(DRD_IS_CAPTURE_MANAGER(self), FALSE);
    g_return_val_if_fail(out_width != NULL, FALSE);
    g_return_val_if_fail(out_height != NULL, FALSE);

    return drd_x11_capture_get_display_size(self->x11_capture, NULL, out_width, out_height, error);
}

/*
 * 功能：获取内部帧队列。
 * 逻辑：类型校验后返回持有的队列指针。
 * 参数：self 管理器实例。
 * 外部接口：无额外外部库依赖。
 */
DrdFrameQueue *
drd_capture_manager_get_queue(DrdCaptureManager *self)
{
    g_return_val_if_fail(DRD_IS_CAPTURE_MANAGER(self), NULL);
    return self->queue;
}

/*
 * 功能：在运行状态下等待捕获帧输出。
 * 逻辑：若未运行则报错；调用帧队列等待接口获取帧，超时或失败返回错误；成功时返回帧对象。
 * 参数：self 管理器；timeout_us 超时时间（微秒）；out_frame 输出帧；error 错误输出。
 * 外部接口：依赖 drd_frame_queue_wait 阻塞等待，错误通过 GLib g_set_error_literal 设置。
 */
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
