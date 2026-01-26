#include "capture/drd_x11_capture.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include "utils/drd_capture_metrics.h"
#include "utils/drd_frame.h"
#include "utils/drd_log.h"

typedef struct
{
    XShmSegmentInfo info;
} DrdX11ShmArea;

struct _DrdX11Capture
{
    GObject parent_instance;

    GMutex state_mutex;
    DrdFrameQueue *queue;
    GThread *thread;

    gboolean running;
    gchar *display_name;

    Display *display;
    int screen;
    Window root;
    XImage *image;
    DrdX11ShmArea shm;
    gboolean attached;
    Damage damage;
    int damage_event_base;

    guint width;
    guint height;
    int wakeup_pipe[2];
};

G_DEFINE_TYPE(DrdX11Capture, drd_x11_capture, G_TYPE_OBJECT)

static gpointer drd_x11_capture_thread(gpointer user_data);

static void drd_x11_capture_cleanup_locked(DrdX11Capture *self);

static gboolean drd_x11_capture_setup_wakeup_pipe(DrdX11Capture *self, GError **error);

static void drd_x11_capture_close_wakeup_pipe(DrdX11Capture *self);

static void drd_x11_capture_drain_wakeup_pipe(int fd);

/*
 * 功能：释放 X11 捕获实例持有的资源。
 * 逻辑：调用 stop 确保线程退出；清理 display 名称与帧队列引用，最后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdX11Capture。
 * 外部接口：GLib g_clear_pointer/g_clear_object 释放资源，最终调用 GObjectClass::dispose。
 */
static void drd_x11_capture_dispose(GObject *object)
{
    DrdX11Capture *self = DRD_X11_CAPTURE(object);

    drd_x11_capture_stop(self);

    g_clear_pointer(&self->display_name, g_free);
    g_clear_object(&self->queue);

    G_OBJECT_CLASS(drd_x11_capture_parent_class)->dispose(object);
}

/*
 * 功能：清理互斥锁等基础资源。
 * 逻辑：销毁 state_mutex，然后调用父类 finalize 完成剩余释放。
 * 参数：object 基类指针。
 * 外部接口：GLib g_mutex_clear。
 */
static void drd_x11_capture_finalize(GObject *object)
{
    DrdX11Capture *self = DRD_X11_CAPTURE(object);
    g_mutex_clear(&self->state_mutex);
    G_OBJECT_CLASS(drd_x11_capture_parent_class)->finalize(object);
}

/*
 * 功能：初始化类回调，挂载 dispose/finalize。
 * 逻辑：将自定义释放函数设置到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：依赖 GLib 类型系统完成类初始化。
 */
static void drd_x11_capture_class_init(DrdX11CaptureClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_x11_capture_dispose;
    object_class->finalize = drd_x11_capture_finalize;
}

/*
 * 功能：初始化实例字段。
 * 逻辑：初始化互斥锁与共享内存标记，置运行状态与唤醒管道为未激活。
 * 参数：self 捕获实例。
 * 外部接口：GLib g_mutex_init、C 库 memset。
 */
static void drd_x11_capture_init(DrdX11Capture *self)
{
    g_mutex_init(&self->state_mutex);
    memset(&self->shm.info, 0, sizeof(self->shm.info));
    self->shm.info.shmid = -1;
    self->running = FALSE;
    self->wakeup_pipe[0] = -1;
    self->wakeup_pipe[1] = -1;
}

/*
 * 功能：创建 X11 捕获对象并绑定输出队列。
 * 逻辑：校验队列类型后创建对象并持有队列引用。
 * 参数：queue 捕获帧输出队列。
 * 外部接口：GLib g_object_new/g_object_ref。
 */
DrdX11Capture *drd_x11_capture_new(DrdFrameQueue *queue)
{
    g_return_val_if_fail(DRD_IS_FRAME_QUEUE(queue), NULL);

    DrdX11Capture *self = g_object_new(DRD_TYPE_X11_CAPTURE, NULL);
    self->queue = g_object_ref(queue);
    return self;
}

/*
 * 功能：读取当前显示的实际分辨率。
 * 逻辑：打开 X11 Display，读取屏幕宽高后关闭连接。
 * 参数：self 捕获实例；display_name 指定显示名（NULL 使用默认）；out_width/out_height 输出值；error 错误输出。
 * 外部接口：X11 XOpenDisplay/DisplayWidth/DisplayHeight/XCloseDisplay。
 */
gboolean drd_x11_capture_get_display_size(DrdX11Capture *self, const gchar *display_name, guint *out_width, guint *out_height, GError **error)
{
    g_return_val_if_fail(DRD_IS_X11_CAPTURE(self), FALSE);
    g_return_val_if_fail(out_width != NULL, FALSE);
    g_return_val_if_fail(out_height != NULL, FALSE);

    Display *display = XOpenDisplay(display_name);
    if (display == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open X11 display for resolution query");
        return FALSE;
    }

    const int screen = DefaultScreen(display);
    *out_width = (guint) DisplayWidth(display, screen);
    *out_height = (guint) DisplayHeight(display, screen);
    XCloseDisplay(display);

    if (*out_width == 0 || *out_height == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid display size %ux%u", *out_width, *out_height);
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：打开 X11 连接并准备共享内存截图资源。
 * 逻辑：依次打开 Display，检测 XShm/XDamage 扩展；获取屏幕/root 窗口与目标尺寸；创建 XShm 图像与共享内存段并附加；创建 Damage 句柄。
 * 参数：self 捕获实例；display_name 显示名称；requested_width/height 期望尺寸；error 错误输出。
 * 外部接口：X11/XShm/XDamage 相关 API：XOpenDisplay 打开连接；XShmQueryExtension/XDamageQueryExtension 检查扩展；XShmCreateImage 创建共享内存图像；shmget/shmat 创建并附加 SysV 共享内存；XShmAttach 绑定共享内存到 X 服务器；XDamageCreate
 * 注册屏幕损坏事件；XSync 刷新事件队列。
 */
static gboolean drd_x11_capture_prepare_display(DrdX11Capture *self, const gchar *display_name, guint requested_width, guint requested_height, GError **error)
{
    self->display = XOpenDisplay(display_name);
    if (self->display == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open X11 display");
        return FALSE;
    }

    if (!XShmQueryExtension(self->display))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "XShm extension not available on X server");
        return FALSE;
    }

    int damage_event = 0;
    int damage_error = 0;
    if (!XDamageQueryExtension(self->display, &damage_event, &damage_error))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "XDamage extension not available on X server");
        return FALSE;
    }
    self->damage_event_base = damage_event;

    self->screen = DefaultScreen(self->display);
    self->root = RootWindow(self->display, self->screen);

    self->width = (requested_width > 0) ? requested_width : (guint) DisplayWidth(self->display, self->screen);
    self->height = (requested_height > 0) ? requested_height : (guint) DisplayHeight(self->display, self->screen);

    self->image = XShmCreateImage(self->display, DefaultVisual(self->display, self->screen), DefaultDepth(self->display, self->screen), ZPixmap, NULL, &self->shm.info, (int) self->width, (int) self->height);
    if (self->image == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create XShm image");
        return FALSE;
    }

    const size_t image_size = (size_t) self->image->bytes_per_line * (size_t) self->image->height;
    self->shm.info.shmid = shmget(IPC_PRIVATE, image_size, IPC_CREAT | 0600);
    if (self->shm.info.shmid < 0)
    {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "shmget failed: %s", g_strerror(errno));
        return FALSE;
    }

    self->shm.info.shmaddr = (char *) shmat(self->shm.info.shmid, NULL, 0);
    if (self->shm.info.shmaddr == (char *) (-1))
    {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "shmat failed: %s", g_strerror(errno));
        return FALSE;
    }

    self->shm.info.readOnly = False;
    self->image->data = self->shm.info.shmaddr;

    if (!XShmAttach(self->display, &self->shm.info))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "XShmAttach failed");
        return FALSE;
    }
    self->attached = TRUE;

    self->damage = XDamageCreate(self->display, self->root, XDamageReportNonEmpty);
    if (self->damage == 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create XDamage handle");
        return FALSE;
    }

    XSync(self->display, False);
    return TRUE;
}

/*
 * 功能：启动 X11 捕获线程并准备资源。
 * 逻辑：持锁检查运行状态；记录 display 名称；创建唤醒管道并准备显示资源；成功后标记 running 并启动线程。
 * 参数：self 捕获实例；display_name 目标显示；requested_width/height 期望尺寸；error 错误输出。
 * 外部接口：GLib g_mutex_lock/unlock、g_thread_new 创建线程；内部调用 drd_x11_capture_setup_wakeup_pipe 与 drd_x11_capture_prepare_display，日志通过 DRD_LOG_MESSAGE。
 */
gboolean drd_x11_capture_start(DrdX11Capture *self, const gchar *display_name, guint requested_width, guint requested_height, GError **error)
{
    g_return_val_if_fail(DRD_IS_X11_CAPTURE(self), FALSE);

    g_mutex_lock(&self->state_mutex);
    if (self->running)
    {
        g_mutex_unlock(&self->state_mutex);
        return TRUE;
    }

    g_clear_pointer(&self->display_name, g_free);
    if (display_name != NULL)
    {
        self->display_name = g_strdup(display_name);
    }

    if (!drd_x11_capture_setup_wakeup_pipe(self, error))
    {
        g_mutex_unlock(&self->state_mutex);
        return FALSE;
    }

    if (!drd_x11_capture_prepare_display(self, self->display_name, requested_width, requested_height, error))
    {
        drd_x11_capture_cleanup_locked(self);
        drd_x11_capture_close_wakeup_pipe(self);
        g_mutex_unlock(&self->state_mutex);
        return FALSE;
    }

    self->running = TRUE;
    self->thread = g_thread_new("drd-x11-capture", drd_x11_capture_thread, g_object_ref(self));

    g_mutex_unlock(&self->state_mutex);
    DRD_LOG_MESSAGE("X11 capture started at %ux%u", self->width, self->height);
    return TRUE;
}

/*
 * 功能：清理 X11 捕获持有的底层资源（需持锁调用）。
 * 逻辑：销毁 Damage 句柄；卸载共享内存与图像；分离并回收 SysV 共享内存；关闭 X Display。
 * 参数：self 捕获实例。
 * 外部接口：XDamageDestroy/XShmDetach/XDestroyImage/shmdt/shmctl/XCloseDisplay 等 X11 与 SysV 共享内存 API。
 */
static void drd_x11_capture_cleanup_locked(DrdX11Capture *self)
{
    if (self->damage != 0 && self->display != NULL)
    {
        XDamageDestroy(self->display, self->damage);
        self->damage = 0;
    }

    if (self->attached && self->display != NULL)
    {
        XShmDetach(self->display, &self->shm.info);
        self->attached = FALSE;
    }

    if (self->image != NULL)
    {
        self->image->data = NULL;
        XDestroyImage(self->image);
        self->image = NULL;
    }

    if (self->shm.info.shmaddr != NULL)
    {
        shmdt(self->shm.info.shmaddr);
        self->shm.info.shmaddr = NULL;
    }

    if (self->shm.info.shmid >= 0)
    {
        shmctl(self->shm.info.shmid, IPC_RMID, NULL);
        self->shm.info.shmid = -1;
        self->shm.info.shmseg = 0;
    }

    if (self->display != NULL)
    {
        XCloseDisplay(self->display);
        self->display = NULL;
    }
}

/*
 * 功能：停止捕获线程并回收资源。
 * 逻辑：持锁检查 running；清除运行标志，先尝试唤醒阻塞线程，再 join 线程；随后持锁调用 cleanup 与关闭唤醒管道，最后记录日志。
 * 参数：self 捕获实例。
 * 外部接口：XSync 同步 X 连接；write 唤醒管道；GLib g_thread_join/g_mutex；日志 DRD_LOG_MESSAGE。
 */
void drd_x11_capture_stop(DrdX11Capture *self)
{
    g_return_if_fail(DRD_IS_X11_CAPTURE(self));

    g_mutex_lock(&self->state_mutex);
    if (!self->running)
    {
        g_mutex_unlock(&self->state_mutex);
        return;
    }

    self->running = FALSE;
    Display *display = self->display;
    g_mutex_unlock(&self->state_mutex);

    if (display != NULL)
    {
        XSync(display, False);
    }

    if (self->wakeup_pipe[1] >= 0)
    {
        const gchar signal_byte = 'x';
        if (write(self->wakeup_pipe[1], &signal_byte, 1) < 0)
        {
            (void) signal_byte;
        }
    }

    if (self->thread != NULL)
    {
        g_thread_join(self->thread);
        self->thread = NULL;
    }

    g_mutex_lock(&self->state_mutex);
    drd_x11_capture_cleanup_locked(self);
    drd_x11_capture_close_wakeup_pipe(self);
    g_mutex_unlock(&self->state_mutex);

    DRD_LOG_MESSAGE("X11 capture stopped");
}

/*
 * 功能：查询捕获线程是否运行。
 * 逻辑：持锁读取 running 标志并返回。
 * 参数：self 捕获实例。
 * 外部接口：GLib g_mutex_lock/unlock。
 */
gboolean drd_x11_capture_is_running(DrdX11Capture *self)
{
    g_return_val_if_fail(DRD_IS_X11_CAPTURE(self), FALSE);

    g_mutex_lock(&self->state_mutex);
    gboolean running = self->running;
    g_mutex_unlock(&self->state_mutex);
    return running;
}

/*
 * 功能：捕获线程主循环，从 X11 拉帧并写入队列。
 * 逻辑：循环读取运行状态与资源；按 target_interval 驱动一次事件消费与抓帧，期间用 g_poll 监听 X 连接和唤醒管道；每个间隔都会触发一次抓帧，XDamage 事件仅用于清理队列与统计，避免被合成器合并后的事件频率限制帧率。
 * 参数：user_data 线程参数，DrdX11Capture 实例。
 * 外部接口：XPending/XNextEvent/XDamageSubtract 处理 Damage 事件；g_poll 监听文件描述符；XShmGetImage 抓帧；glib 时间函数 g_get_monotonic_time；DrdFrame API drd_frame_new/configure/ensure_capacity 与 drd_frame_queue_push；日志
 * DRD_LOG_MESSAGE/DRD_LOG_WARNING。
 */
static gpointer drd_x11_capture_thread(gpointer user_data)
{
    DrdX11Capture *self = DRD_X11_CAPTURE(user_data);

    const guint target_fps = drd_capture_metrics_get_target_fps();
    const gint64 target_interval = drd_capture_metrics_get_target_interval_us();
    const gint64 stats_interval = drd_capture_metrics_get_stats_interval_us();
    gint64 stats_window_start = 0;
    guint stats_frames = 0;
    gint64 next_capture_deadline = 0;
    gint64 now = 0;

    while (TRUE)
    {
        Display *display = NULL;
        XImage *image = NULL;
        Window root;
        int damage_event_base = 0;
        guint width = 0;
        guint height = 0;
        gboolean running;
        int wake_fd = -1;
        gboolean damage = FALSE;

        g_mutex_lock(&self->state_mutex);
        running = self->running;
        display = self->display;
        image = self->image;
        root = self->root;
        damage_event_base = self->damage_event_base;
        width = self->width;
        height = self->height;
        wake_fd = self->wakeup_pipe[0];
        g_mutex_unlock(&self->state_mutex);

        if (!running || display == NULL || image == NULL)
        {
            DRD_LOG_MESSAGE("break x11 capture thread");
            break;
        }

        if (next_capture_deadline == 0)
        {
            next_capture_deadline = g_get_monotonic_time();
        }
        GPollFD pfds[2];
        nfds_t poll_count = 0;
        int wake_index = -1;
        const int connection_fd = XConnectionNumber(display);
        pfds[poll_count].fd = connection_fd;
        pfds[poll_count].events = G_IO_IN;
        pfds[poll_count].revents = 0;
        poll_count++;
        if (wake_fd >= 0)
        {
            wake_index = poll_count;
            pfds[poll_count].fd = wake_fd;
            pfds[poll_count].events = G_IO_IN;
            pfds[poll_count].revents = 0;
            poll_count++;
        }

        gint poll_result = g_poll(pfds, poll_count, target_interval / 1000);
        if (poll_result < 0)
        {
            continue;
        }
        if (poll_result > 0 && wake_index >= 0 && (pfds[wake_index].revents & G_IO_IN))
        {
            drd_x11_capture_drain_wakeup_pipe(wake_fd);
        }

        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == damage_event_base + XDamageNotify)
            {
                XDamageSubtract(display, self->damage, None, None);
                damage = TRUE;
            }
        }
        if (!damage)
            continue;
        now = g_get_monotonic_time();
        if (now < next_capture_deadline)
        {
            continue;
        }

        if (!XShmGetImage(display, root, image, 0, 0, AllPlanes))
        {
            DRD_LOG_WARNING("XShmGetImage failed, retrying");
            next_capture_deadline = now + target_interval;
            continue;
        }
        stats_frames++;
        g_autoptr(DrdFrame) frame = drd_frame_new();
        now = g_get_monotonic_time();
        drd_frame_configure(frame, width, height, (guint) image->bytes_per_line, (guint64) now);

        const gsize frame_size = (gsize) image->bytes_per_line * (gsize) image->height;
        guint8 *buffer = drd_frame_ensure_capacity(frame, frame_size);
        if (buffer != NULL)
        {
            memcpy(buffer, image->data, frame_size);
            drd_frame_queue_push(self->queue, frame);
        }

        if (stats_window_start == 0)
        {
            stats_window_start = now;
        }
        else
        {
            const gint64 stats_elapsed = now - stats_window_start;
            if (stats_elapsed >= stats_interval)
            {
                const gdouble actual_fps = (gdouble) stats_frames * (gdouble) G_USEC_PER_SEC / (gdouble) stats_elapsed;
                const gboolean reached_target = actual_fps >= (gdouble) target_fps;
                DRD_LOG_MESSAGE("X11 capture fps=%.2f (target=%u): %s", actual_fps, target_fps, reached_target ? "reached target" : "below target");
                stats_frames = 0;
                stats_window_start = now;
            }
        }

        next_capture_deadline += target_interval;
        if (next_capture_deadline < now)
        {
            next_capture_deadline = now + target_interval;
        }
    }

    g_object_unref(self);
    return NULL;
}

/*
 * 功能：创建唤醒管道供线程退出时使用。
 * 逻辑：若已有管道直接返回；否则通过 g_unix_open_pipe 创建带 CLOEXEC 标志的管道并缓存 fd。
 * 参数：self 捕获实例；error 错误输出。
 * 外部接口：glib-unix g_unix_open_pipe 创建管道。
 */
static gboolean drd_x11_capture_setup_wakeup_pipe(DrdX11Capture *self, GError **error)
{
    if (self->wakeup_pipe[0] >= 0 && self->wakeup_pipe[1] >= 0)
    {
        return TRUE;
    }

    int fds[2] = {-1, -1};
    if (!g_unix_open_pipe(fds, O_CLOEXEC, error))
    {
        return FALSE;
    }

    if (!g_unix_set_fd_nonblocking(fds[0], TRUE, error) || !g_unix_set_fd_nonblocking(fds[1], TRUE, error))
    {
        close(fds[0]);
        close(fds[1]);
        return FALSE;
    }

    self->wakeup_pipe[0] = fds[0];
    self->wakeup_pipe[1] = fds[1];
    return TRUE;
}

/*
 * 功能：关闭并清理唤醒管道 fd。
 * 逻辑：遍历两个 fd，若有效则 close 并置为 -1。
 * 参数：self 捕获实例。
 * 外部接口：POSIX close。
 */
static void drd_x11_capture_close_wakeup_pipe(DrdX11Capture *self)
{
    for (int i = 0; i < 2; ++i)
    {
        if (self->wakeup_pipe[i] >= 0)
        {
            close(self->wakeup_pipe[i]);
            self->wakeup_pipe[i] = -1;
        }
    }
}

/*
 * 功能：清空唤醒管道中的残留数据。
 * 逻辑：若 fd 有效则循环读取直到无数据。
 * 参数：fd 管道读端。
 * 外部接口：POSIX read。
 */
static void drd_x11_capture_drain_wakeup_pipe(int fd)
{
    if (fd < 0)
    {
        return;
    }

    char buffer[64];
    for (;;)
    {
        const ssize_t ret = read(fd, buffer, sizeof(buffer));
        if (ret > 0)
        {
            continue;
        }
        if (ret == 0)
        {
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        break;
    }
}
