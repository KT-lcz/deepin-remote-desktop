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

static void
drd_encoded_frame_dispose(GObject *object)
{
    DrdEncodedFrame *self = DRD_ENCODED_FRAME(object);
    g_clear_pointer(&self->payload, g_byte_array_unref);
    G_OBJECT_CLASS(drd_encoded_frame_parent_class)->dispose(object);
}

static void
drd_encoded_frame_class_init(DrdEncodedFrameClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_encoded_frame_dispose;
}

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

DrdEncodedFrame *
drd_encoded_frame_new(void)
{
    return g_object_new(DRD_TYPE_ENCODED_FRAME, NULL);
}

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

void
drd_encoded_frame_set_quality(DrdEncodedFrame *self, guint8 quality, guint8 qp, gboolean is_keyframe)
{
    g_return_if_fail(DRD_IS_ENCODED_FRAME(self));

    self->quality = quality;
    self->qp = qp;
    self->is_keyframe = is_keyframe;
}

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

guint
drd_encoded_frame_get_width(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->width;
}

guint
drd_encoded_frame_get_height(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->height;
}

guint
drd_encoded_frame_get_stride(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->stride;
}

gboolean
drd_encoded_frame_get_is_bottom_up(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), FALSE);
    return self->is_bottom_up;
}

guint64
drd_encoded_frame_get_timestamp(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->timestamp;
}

DrdFrameCodec
drd_encoded_frame_get_codec(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), DRD_FRAME_CODEC_RAW);
    return self->codec;
}

gboolean
drd_encoded_frame_is_keyframe(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), FALSE);
    return self->is_keyframe;
}

guint8
drd_encoded_frame_get_quality(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->quality;
}

guint8
drd_encoded_frame_get_qp(DrdEncodedFrame *self)
{
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(self), 0);
    return self->qp;
}
