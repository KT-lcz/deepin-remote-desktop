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

static void
drd_frame_dispose(GObject *object)
{
    DrdFrame *self = DRD_FRAME(object);
    g_clear_pointer(&self->pixels, g_byte_array_unref);
    G_OBJECT_CLASS(drd_frame_parent_class)->dispose(object);
}

static void
drd_frame_class_init(DrdFrameClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_frame_dispose;
}

static void
drd_frame_init(DrdFrame *self)
{
    self->pixels = g_byte_array_new();
}

DrdFrame *
drd_frame_new(void)
{
    return g_object_new(DRD_TYPE_FRAME, NULL);
}

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

guint
drd_frame_get_width(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->width;
}

guint
drd_frame_get_height(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->height;
}

guint
drd_frame_get_stride(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->stride;
}

guint64
drd_frame_get_timestamp(DrdFrame *self)
{
    g_return_val_if_fail(DRD_IS_FRAME(self), 0);
    return self->timestamp;
}

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
