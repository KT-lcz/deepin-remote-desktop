#include "input/drd_input_dispatcher.h"

#include <gio/gio.h>

#include "input/drd_x11_input.h"

struct _DrdInputDispatcher
{
    GObject parent_instance;

    DrdX11Input *backend;
    gboolean active;
};

G_DEFINE_TYPE(DrdInputDispatcher, drd_input_dispatcher, G_TYPE_OBJECT)

/*
 * 功能：释放输入分发器持有的后端资源。
 * 逻辑：调用 stop 确保停止后端，再释放 backend 引用，最后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdInputDispatcher。
 * 外部接口：GLib g_clear_object；调用 drd_input_dispatcher_stop 以释放 X11 后端。
 */
static void
drd_input_dispatcher_dispose(GObject *object)
{
    DrdInputDispatcher *self = DRD_INPUT_DISPATCHER(object);
    drd_input_dispatcher_stop(self);
    g_clear_object(&self->backend);

    G_OBJECT_CLASS(drd_input_dispatcher_parent_class)->dispose(object);
}

/*
 * 功能：绑定类级别析构回调。
 * 逻辑：将自定义 dispose 挂载到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void
drd_input_dispatcher_class_init(DrdInputDispatcherClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_input_dispatcher_dispose;
}

/*
 * 功能：初始化输入分发器实例。
 * 逻辑：创建 X11 输入后端并设置默认 inactive 状态。
 * 参数：self 分发器实例。
 * 外部接口：drd_x11_input_new 创建后端。
 */
static void
drd_input_dispatcher_init(DrdInputDispatcher *self)
{
    self->backend = drd_x11_input_new();
    self->active = FALSE;
}

/*
 * 功能：创建输入分发器对象。
 * 逻辑：使用 g_object_new 分配实例。
 * 参数：无。
 * 外部接口：GLib g_object_new。
 */
DrdInputDispatcher *
drd_input_dispatcher_new(void)
{
    return g_object_new(DRD_TYPE_INPUT_DISPATCHER, NULL);
}

/*
 * 功能：启动输入后端并应用桌面尺寸。
 * 逻辑：调用 X11 后端启动；启动成功后更新桌面尺寸并置 active 标志。
 * 参数：self 分发器；width/height 编码流宽高；error 输出错误。
 * 外部接口：drd_x11_input_start/drd_x11_input_update_desktop_size；错误通过 GLib GError。
 */
gboolean
drd_input_dispatcher_start(DrdInputDispatcher *self, guint width, guint height, GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);

    if (!drd_x11_input_start(self->backend, error))
    {
        return FALSE;
    }

    drd_x11_input_update_desktop_size(self->backend, width, height);
    self->active = TRUE;
    return TRUE;
}

/*
 * 功能：停止输入后端。
 * 逻辑：校验实例后调用 X11 后端 stop，并清理 active 标志。
 * 参数：self 分发器。
 * 外部接口：drd_x11_input_stop。
 */
void
drd_input_dispatcher_stop(DrdInputDispatcher *self)
{
    g_return_if_fail(DRD_IS_INPUT_DISPATCHER(self));
    drd_x11_input_stop(self->backend);
    self->active = FALSE;
}

/*
 * 功能：更新桌面尺寸，供指针缩放使用。
 * 逻辑：将宽高写入 X11 后端以刷新缩放比。
 * 参数：self 分发器；width/height 新尺寸。
 * 外部接口：drd_x11_input_update_desktop_size。
 */
void
drd_input_dispatcher_update_desktop_size(DrdInputDispatcher *self, guint width, guint height)
{
    g_return_if_fail(DRD_IS_INPUT_DISPATCHER(self));
    drd_x11_input_update_desktop_size(self->backend, width, height);
}

/*
 * 功能：分发键盘扫描码事件。
 * 逻辑：直接调用 X11 后端注入键盘事件。
 * 参数：self 分发器；flags RDP 键盘标志；scancode 扫描码；error 错误输出。
 * 外部接口：drd_x11_input_inject_keyboard（基于 X11 XTest/FreeRDP 键盘映射）。
 */
gboolean
drd_input_dispatcher_handle_keyboard(DrdInputDispatcher *self,
                                     guint16 flags,
                                     guint8 scancode,
                                     GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);
    return drd_x11_input_inject_keyboard(self->backend, flags, scancode, error);
}

/*
 * 功能：分发 Unicode 键盘事件。
 * 逻辑：调用 X11 后端将 Unicode 注入为键盘事件。
 * 参数：self 分发器；flags RDP 标志；codepoint Unicode 码点；error 错误输出。
 * 外部接口：drd_x11_input_inject_unicode（使用 XKeysymToKeycode/XTest）。
 */
gboolean
drd_input_dispatcher_handle_unicode(DrdInputDispatcher *self,
                                    guint16 flags,
                                    guint16 codepoint,
                                    GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);
    return drd_x11_input_inject_unicode(self->backend, flags, codepoint, error);
}

/*
 * 功能：分发指针事件。
 * 逻辑：调用 X11 后端注入移动/按键/滚轮事件。
 * 参数：self 分发器；flags RDP 指针标志；x/y 流坐标；error 错误输出。
 * 外部接口：drd_x11_input_inject_pointer（XTestFakeMotion/ButtonEvent 等）。
 */
gboolean
drd_input_dispatcher_handle_pointer(DrdInputDispatcher *self,
                                    guint16 flags,
                                    guint16 x,
                                    guint16 y,
                                    GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);
    return drd_x11_input_inject_pointer(self->backend, flags, x, y, error);
}

/*
 * 功能：刷新输入缓冲占位接口。
 * 逻辑：当前无缓冲行为，保持接口对称性。
 * 参数：self 分发器实例。
 * 外部接口：无。
 */
void
drd_input_dispatcher_flush(DrdInputDispatcher *self)
{
    (void) self;
    /* No buffered events at the moment; maintained for API symmetry. */
}
