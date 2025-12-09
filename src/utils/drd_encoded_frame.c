#include "utils/drd_encoded_frame.h"

struct _DrdEncodedFrame
{
    GObject parent_instance;

    GByteArray *payload;
    guint width;
    guint height;
    guint stride;
    gboolean is_bottom_up;
    guint64 timestamp;
    DrdFrameCodec codec;
    guint8 quality;
    guint8 qp;
    gboolean is_keyframe;
};

G_DEFINE_TYPE(DrdEncodedFrame, drd_encoded_frame, G_TYPE_OBJECT)

/*
 * 功能：释放编码帧持有的 payload。
 * 逻辑：清理 GByteArray 后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdEncodedFrame。
 * 外部接口：GLib g_clear_pointer/g_byte_array_unref。
 */
static void
drd_encoded_frame_dispose(GObject *object)
{
    DrdEncodedFrame *self = DRD_ENCODED_FRAME(object);
    g_clear_pointer(&self->payload, g_byte_array_unref);
    G_OBJECT_CLASS(drd_encoded_frame_parent_class)->dispose(object);
}

/*
 * 功能：绑定类级别析构回调。
 * 逻辑：将自定义 dispose 安装到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void
drd_encoded_frame_class_init(DrdEncodedFrameClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_encoded_frame_dispose;
}

/*
 * 功能：初始化编码帧默认值。
 * 逻辑：创建 payload 数组，设置默认质量/编码器/关键帧标记。
 * 参数：self 编码帧实例。
 * 外部接口：GLib g_byte_array_new。
 */
static void
drd_encoded_frame_init(DrdEncodedFrame *self)
{
    self->payload = g_byte_array_new();
    self->quality = 100;
    self->qp = 0;
    self->codec = DRD_FRAME_CODEC_RAW;
    self->is_bottom_up = FALSE;
    self->is_keyframe = TRUE;
}

/*
 * 功能：创建编码帧对象。
 * 逻辑：调用 g_object_new 分配实例。
 * 参数：无。
 * 外部接口：GLib g_object_new。
 */
DrdEncodedFrame *
drd_encoded_frame_new(void)
{
    return g_object_new(DRD_TYPE_ENCODED_FRAME, NULL);
}

/*
 * 功能：配置编码帧的几何与元数据。
 * 逻辑：写入宽、高、stride、方向、时间戳与编码器类型。
 * 参数：self 编码帧；width/height/stride 几何；is_bottom_up 是否倒置；timestamp 时间戳；codec 编码格式。
 * 外部接口：无。
 */
void
drd_encoded_frame_configure(DrdEncodedFrame *self,
                            guint width,
                            guint height,
                            guint stride,
                            gboolean is_bottom_up,
                            guint64 timestamp,
                            DrdFrameCodec codec)
{
    g_return_if_fail(DRD_IS_ENCODED_FRAME(self));

    self->width = width;
    self->height = height;
    self->stride = stride;
    self->is_bottom_up = is_bottom_up;
    self->timestamp = timestamp;
    self->codec = codec;
}

/*
 * 功能：设置质量参数与关键帧标记。
 * 逻辑：直接写入 quality、qp 与 is_keyframe。
 * 参数：self 编码帧；quality 质量值；qp 量化参数；is_keyframe 是否关键帧。
 * 外部接口：无。
 */
void
drd_encoded_frame_set_quality(DrdEncodedFrame *self, guint8 quality, guint8 qp, gboolean is_keyframe)
{
    g_return_if_fail(DRD_IS_ENCODED_FRAME(self));

    self->quality = quality;
    self->qp = qp;
    self->is_keyframe = is_keyframe;
}

/*
 * 功能：确保 payload 有足够容量并返回数据指针。
 * 逻辑：若长度不同则调整 GByteArray 大小，返回内部数据指针。
 * 参数：self 编码帧；size 期望字节数。
 * 外部接口：GLib g_byte_array_set_size。
 */
guint8 *
drd_encoded_frame_ensure_capacity(DrdEncodedFrame *self, gsize size)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), NULL);

    if (self->payload->len != size)
    {
        g_byte_array_set_size(self->payload, size);
    }

    return self->payload->data;
}

/*
 * 功能：读取 payload 数据指针与长度。
 * 逻辑：可选写入长度后返回内部数据常量指针。
 * 参数：self 编码帧；size 输出长度可选。
 * 外部接口：无。
 */
const guint8 *
drd_encoded_frame_get_data(DrdEncodedFrame *self, gsize *size)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), NULL);

    if (size != NULL)
    {
        *size = self->payload->len;
    }

    return self->payload->data;
}

/*
 * 功能：获取编码帧宽度。
 * 逻辑：类型检查后返回 width。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
guint
drd_encoded_frame_get_width(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->width;
}

/*
 * 功能：获取编码帧高度。
 * 逻辑：类型检查后返回 height。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
guint
drd_encoded_frame_get_height(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->height;
}

/*
 * 功能：获取 stride。
 * 逻辑：类型检查后返回 stride。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
guint
drd_encoded_frame_get_stride(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->stride;
}

/*
 * 功能：判断图像是否倒置存储。
 * 逻辑：类型检查后返回 is_bottom_up。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
gboolean
drd_encoded_frame_get_is_bottom_up(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), FALSE);
    return self->is_bottom_up;
}

/*
 * 功能：获取时间戳。
 * 逻辑：类型检查后返回 timestamp。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
guint64
drd_encoded_frame_get_timestamp(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->timestamp;
}

/*
 * 功能：获取编码格式。
 * 逻辑：类型检查后返回 codec。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
DrdFrameCodec
drd_encoded_frame_get_codec(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), DRD_FRAME_CODEC_RAW);
    return self->codec;
}

/*
 * 功能：查询是否关键帧。
 * 逻辑：类型检查后返回 is_keyframe。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
gboolean
drd_encoded_frame_is_keyframe(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), FALSE);
    return self->is_keyframe;
}

/*
 * 功能：获取质量值。
 * 逻辑：类型检查后返回 quality。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
guint8
drd_encoded_frame_get_quality(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->quality;
}

/*
 * 功能：获取量化参数。
 * 逻辑：类型检查后返回 qp。
 * 参数：self 编码帧。
 * 外部接口：无。
 */
guint8
drd_encoded_frame_get_qp(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->qp;
}
