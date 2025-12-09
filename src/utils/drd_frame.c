#include "utils/drd_frame.h"

#include <string.h>

struct _DrdFrame
{
    GObject parent_instance;

    GByteArray *pixels;
    guint width;
    guint height;
    guint stride;
    guint64 timestamp;
};

G_DEFINE_TYPE(DrdFrame, drd_frame, G_TYPE_OBJECT)

/*
 * 功能：释放帧对象持有的像素缓冲。
 * 逻辑：清理 GByteArray 引用后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdFrame。
 * 外部接口：GLib g_clear_pointer/g_byte_array_unref。
 */
static void
drd_frame_dispose(GObject *object)
{
    DrdFrame *self = DRD_FRAME(object);
    g_clear_pointer(&self->pixels, g_byte_array_unref);
    G_OBJECT_CLASS(drd_frame_parent_class)->dispose(object);
}

/*
 * 功能：绑定类级别析构回调。
 * 逻辑：将自定义 dispose 安装到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void
drd_frame_class_init(DrdFrameClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_frame_dispose;
}

/*
 * 功能：初始化帧对象。
 * 逻辑：创建像素缓存数组。
 * 参数：self 帧实例。
 * 外部接口：GLib g_byte_array_new。
 */
static void
drd_frame_init(DrdFrame *self)
{
    self->pixels = g_byte_array_new();
}

/*
 * 功能：创建帧对象。
 * 逻辑：调用 g_object_new 分配实例。
 * 参数：无。
 * 外部接口：GLib g_object_new。
 */
DrdFrame *
drd_frame_new(void)
{
    return g_object_new(DRD_TYPE_FRAME, NULL);
}

/*
 * 功能：配置帧的几何信息与时间戳。
 * 逻辑：写入宽、高、stride 与时间戳。
 * 参数：self 帧实例；width/height/stride 几何；timestamp 时间戳。
 * 外部接口：无。
 */
void
drd_frame_configure(DrdFrame *self,
                    guint width,
                    guint height,
                    guint stride,
                    guint64 timestamp)
{
    g_return_if_fail(DRD_IS_FRAME(self));

    self->width = width;
    self->height = height;
    self->stride = stride;
    self->timestamp = timestamp;
}

/*
 * 功能：获取帧宽度。
 * 逻辑：类型检查后返回 width。
 * 参数：self 帧实例。
 * 外部接口：无。
 */
guint
drd_frame_get_width(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->width;
}

/*
 * 功能：获取帧高度。
 * 逻辑：类型检查后返回 height。
 * 参数：self 帧实例。
 * 外部接口：无。
 */
guint
drd_frame_get_height(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->height;
}

/*
 * 功能：获取 stride。
 * 逻辑：类型检查后返回 stride。
 * 参数：self 帧实例。
 * 外部接口：无。
 */
guint
drd_frame_get_stride(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->stride;
}

/*
 * 功能：获取时间戳。
 * 逻辑：类型检查后返回 timestamp。
 * 参数：self 帧实例。
 * 外部接口：无。
 */
guint64
drd_frame_get_timestamp(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->timestamp;
}

/*
 * 功能：确保像素缓冲容量并返回可写指针。
 * 逻辑：若当前长度与期望不同则调整 GByteArray 大小，然后返回内部数据指针。
 * 参数：self 帧实例；size 需要的字节数。
 * 外部接口：GLib g_byte_array_set_size。
 */
guint8 *
drd_frame_ensure_capacity(DrdFrame *self, gsize size)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), NULL);

    if (self->pixels->len != size)
    {
        g_byte_array_set_size(self->pixels, size);
    }

    return self->pixels->data;
}

/*
 * 功能：获取像素数据指针与长度。
 * 逻辑：可选写入长度后返回内部数据常量指针。
 * 参数：self 帧实例；size 输出长度可选。
 * 外部接口：无。
 */
const guint8 *
drd_frame_get_data(DrdFrame *self, gsize *size)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), NULL);

    if (size != NULL)
    {
        *size = self->pixels->len;
    }

    return self->pixels->data;
}
