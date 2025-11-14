#include "input/drd_x11_input.h"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include <freerdp/input.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/scancode.h>

#include <gio/gio.h>
#include <math.h>
#include <string.h>

struct _DrdX11Input
{
    GObject parent_instance;

    GMutex lock;
    Display *display;
    gint screen;
    guint desktop_width;
    guint desktop_height;
    guint stream_width;
    guint stream_height;
    gboolean running;
    guint32 keyboard_layout;
};

G_DEFINE_TYPE(DrdX11Input, drd_x11_input, G_TYPE_OBJECT)

static gboolean drd_x11_input_open_display(DrdX11Input *self, GError **error);
static void drd_x11_input_close_display(DrdX11Input *self);

/* 对象销毁时停止后台线程并释放互斥锁。 */
static void
drd_x11_input_dispose(GObject *object)
{
    DrdX11Input *self = DRD_X11_INPUT(object);
    drd_x11_input_stop(self);
    g_mutex_clear(&self->lock);

    G_OBJECT_CLASS(drd_x11_input_parent_class)->dispose(object);
}

/* 绑定 dispose 钩子。 */
static void
drd_x11_input_class_init(DrdX11InputClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_x11_input_dispose;
}

/* 初始化默认值，真实尺寸会在启动时根据显示与编码流写入。 */
static void
drd_x11_input_init(DrdX11Input *self)
{
    g_mutex_init(&self->lock);
    self->display = NULL;
    self->screen = 0;
    self->desktop_width = 0;
    self->desktop_height = 0;
    self->stream_width = 0;
    self->stream_height = 0;
    self->running = FALSE;
    self->keyboard_layout = 0;
}

/* 构造输入后端。 */
DrdX11Input *
drd_x11_input_new(void)
{
    return g_object_new(DRD_TYPE_X11_INPUT, NULL);
}

/* 打开 X11 Display，并查询屏幕大小、键盘布局。 */
static gboolean
drd_x11_input_open_display(DrdX11Input *self, GError **error)
{
    if (self->display != NULL)
    {
        return TRUE;
    }

    Display *display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "X11 input injector failed to open default display");
        return FALSE;
    }

    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    if (!XTestQueryExtension(display, &event_base, &error_base, &major, &minor))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "X11 XTest extension not available");
        XCloseDisplay(display);
        return FALSE;
    }

    self->display = display;
    self->screen = DefaultScreen(display);
    self->desktop_width = (guint)DisplayWidth(display, self->screen);
    self->desktop_height = (guint)DisplayHeight(display, self->screen);
    if (self->desktop_width == 0)
    {
        self->desktop_width = 1920;
    }
    if (self->desktop_height == 0)
    {
        self->desktop_height = 1080;
    }
    if (self->stream_width == 0)
    {
        self->stream_width = self->desktop_width;
    }
    if (self->stream_height == 0)
    {
        self->stream_height = self->desktop_height;
    }

    self->keyboard_layout = freerdp_keyboard_init(0);
    if (self->keyboard_layout == 0)
    {
        self->keyboard_layout = freerdp_keyboard_init(KBD_US);
    }

    return TRUE;
}

/* 关闭 X11 Display 连接。 */
static void
drd_x11_input_close_display(DrdX11Input *self)
{
    if (self->display != NULL)
    {
        XCloseDisplay(self->display);
        self->display = NULL;
    }
}

/* 启动输入注入器，确保已连接 X11。 */
gboolean
drd_x11_input_start(DrdX11Input *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_X11_INPUT(self), FALSE);

    g_mutex_lock(&self->lock);
    if (self->running)
    {
        g_mutex_unlock(&self->lock);
        return TRUE;
    }

    gboolean ok = drd_x11_input_open_display(self, error);
    if (ok)
    {
        self->running = TRUE;
    }
    g_mutex_unlock(&self->lock);
    return ok;
}

/* 停止输入注入器并释放 Display。 */
void
drd_x11_input_stop(DrdX11Input *self)
{
    g_return_if_fail(DRD_IS_X11_INPUT(self));

    g_mutex_lock(&self->lock);
    if (!self->running)
    {
        g_mutex_unlock(&self->lock);
        return;
    }

    drd_x11_input_close_display(self);
    self->running = FALSE;
    g_mutex_unlock(&self->lock);
}

/* 更新编码流尺寸，供坐标映射使用。 */
void
drd_x11_input_update_desktop_size(DrdX11Input *self, guint width, guint height)
{
    g_return_if_fail(DRD_IS_X11_INPUT(self));

    g_mutex_lock(&self->lock);
    if (width > 0)
    {
        self->stream_width = width;
    }
    if (height > 0)
    {
        self->stream_height = height;
    }
    g_mutex_unlock(&self->lock);
}

/* 检查注入器运行状态，失败时填充错误信息。 */
static gboolean
drd_x11_input_check_running(DrdX11Input *self, GError **error)
{
    if (!self->running || self->display == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "X11 input injector is not running");
        return FALSE;
    }
    return TRUE;
}

/* 将 RDP 键盘事件转换为 X11 事件并注入。 */
gboolean
drd_x11_input_inject_keyboard(DrdX11Input *self, guint16 flags, guint8 scancode, GError **error)
{
    g_return_val_if_fail(DRD_IS_X11_INPUT(self), FALSE);

    g_mutex_lock(&self->lock);
    if (!drd_x11_input_check_running(self, error))
    {
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    const gboolean release = (flags & KBD_FLAGS_RELEASE) != 0;
    const gboolean extended = (flags & (KBD_FLAGS_EXTENDED | KBD_FLAGS_EXTENDED1)) != 0;
    const UINT32 rdp_scancode = MAKE_RDP_SCANCODE(scancode, extended);
    const UINT32 base_scancode = RDP_SCANCODE_CODE(rdp_scancode);
    const UINT32 x11_keycode = freerdp_keyboard_get_x11_keycode_from_rdp_scancode(
        base_scancode, extended ? TRUE : FALSE);

    if (x11_keycode == 0)
    {
        g_mutex_unlock(&self->lock);
        return TRUE;
    }

    XTestFakeKeyEvent(self->display,
                      (unsigned int)x11_keycode,
                      release ? False : True,
                      CurrentTime);
    XFlush(self->display);

    g_mutex_unlock(&self->lock);
    return TRUE;
}

/* 目前未实现 Unicode 注入，占位返回成功。 */
gboolean
drd_x11_input_inject_unicode(DrdX11Input *self, guint16 flags, guint16 codepoint, GError **error)
{
    g_return_val_if_fail(DRD_IS_X11_INPUT(self), FALSE);
    (void)flags;
    (void)codepoint;
    (void)error;
    /* Unicode injection is not yet implemented; silently ignore for now. */
    return TRUE;
}

/* 将指针标志转换为 XTest 按键 ID。 */
static int
drd_x11_input_pointer_button(guint16 flags, guint16 mask, int button_id)
{
    if ((flags & mask) == 0)
    {
        return 0;
    }

    const gboolean press = (flags & PTR_FLAGS_DOWN) != 0;
    return press ? button_id : -button_id;
}

/* 注入指针移动、按键及滚轮事件，同时处理分辨率缩放。 */
gboolean
drd_x11_input_inject_pointer(DrdX11Input *self,
                              guint16 flags,
                              guint16 x,
                              guint16 y,
                              GError **error)
{
    g_return_val_if_fail(DRD_IS_X11_INPUT(self), FALSE);

    g_mutex_lock(&self->lock);
    if (!drd_x11_input_check_running(self, error))
    {
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    const guint32 stream_width = MAX(self->stream_width, 1u);
    const guint32 stream_height = MAX(self->stream_height, 1u);
    const guint32 desktop_width = MAX(self->desktop_width, 1u);
    const guint32 desktop_height = MAX(self->desktop_height, 1u);

    const guint16 max_stream_x = (guint16)(stream_width > 0 ? stream_width - 1 : 0);
    const guint16 max_stream_y = (guint16)(stream_height > 0 ? stream_height - 1 : 0);
    const guint16 clamped_stream_x = x > max_stream_x ? max_stream_x : x;
    const guint16 clamped_stream_y = y > max_stream_y ? max_stream_y : y;

    guint16 target_x = clamped_stream_x;
    guint16 target_y = clamped_stream_y;
    if (stream_width != desktop_width)
    {
        const gdouble scale_x = (gdouble)desktop_width / (gdouble)stream_width;
        guint scaled = (guint)((gdouble)clamped_stream_x * scale_x + 0.5);
        if (scaled >= desktop_width)
        {
            scaled = desktop_width - 1;
        }
        target_x = (guint16)scaled;
    }
    if (stream_height != desktop_height)
    {
        const gdouble scale_y = (gdouble)desktop_height / (gdouble)stream_height;
        guint scaled = (guint)((gdouble)clamped_stream_y * scale_y + 0.5);
        if (scaled >= desktop_height)
        {
            scaled = desktop_height - 1;
        }
        target_y = (guint16)scaled;
    }

    if (flags & PTR_FLAGS_MOVE)
    {
        XTestFakeMotionEvent(self->display, self->screen, target_x, target_y, CurrentTime);
    }

    struct ButtonMask
    {
        guint16 mask;
        int button_id;
    } button_map[] = {
        {PTR_FLAGS_BUTTON1, 1},
        {PTR_FLAGS_BUTTON3, 2},
        {PTR_FLAGS_BUTTON2, 3},
    };

    for (guint i = 0; i < G_N_ELEMENTS(button_map); ++i)
    {
        int button_event = drd_x11_input_pointer_button(flags, button_map[i].mask, button_map[i].button_id);
        if (button_event > 0)
        {
            XTestFakeButtonEvent(self->display, button_event, True, CurrentTime);
        }
        else if (button_event < 0)
        {
            XTestFakeButtonEvent(self->display, -button_event, False, CurrentTime);
        }
    }

    if (flags & PTR_FLAGS_WHEEL)
    {
        const gboolean negative = (flags & PTR_FLAGS_WHEEL_NEGATIVE) != 0;
        const int button = negative ? 5 : 4;
        XTestFakeButtonEvent(self->display, button, True, CurrentTime);
        XTestFakeButtonEvent(self->display, button, False, CurrentTime);
    }

    if (flags & PTR_FLAGS_HWHEEL)
    {
        const gboolean negative = (flags & PTR_FLAGS_WHEEL_NEGATIVE) != 0;
        const int button = negative ? 7 : 6;
        XTestFakeButtonEvent(self->display, button, True, CurrentTime);
        XTestFakeButtonEvent(self->display, button, False, CurrentTime);
    }

    XFlush(self->display);
    g_mutex_unlock(&self->lock);
    return TRUE;
}
