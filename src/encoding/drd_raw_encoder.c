#include "encoding/drd_raw_encoder.h"

#include <gio/gio.h>
#include <string.h>

struct _DrdRawEncoder
{
    GObject parent_instance;

    guint width;
    guint height;
    gboolean ready;
};

G_DEFINE_TYPE(DrdRawEncoder, drd_raw_encoder, G_TYPE_OBJECT)

static void
drd_raw_encoder_class_init(DrdRawEncoderClass *klass)
{
    (void)klass;
}

static void
drd_raw_encoder_init(DrdRawEncoder *self)
{
    self->width = 0;
    self->height = 0;
    self->ready = FALSE;
}

DrdRawEncoder *
drd_raw_encoder_new(void)
{
    return g_object_new(DRD_TYPE_RAW_ENCODER, NULL);
}

gboolean
drd_raw_encoder_configure(DrdRawEncoder *self, guint width, guint height, GError **error)
{
    g_return_val_if_fail(DRD_IS_RAW_ENCODER(self), FALSE);

    if (width == 0 || height == 0)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "Raw encoder requires non-zero width/height (width=%u height=%u)",
                    width,
                    height);
        return FALSE;
    }

    self->width = width;
    self->height = height;
    self->ready = TRUE;
    return TRUE;
}

void
drd_raw_encoder_reset(DrdRawEncoder *self)
{
    g_return_if_fail(DRD_IS_RAW_ENCODER(self));
    self->ready = FALSE;
    self->width = 0;
    self->height = 0;
}

gboolean
drd_raw_encoder_encode(DrdRawEncoder *self,
                         DrdFrame *input,
                         DrdEncodedFrame *output,
                         GError **error)
{
    g_return_val_if_fail(DRD_IS_RAW_ENCODER(self), FALSE);
    g_return_val_if_fail(DRD_IS_FRAME(input), FALSE);
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(output), FALSE);

    if (!self->ready)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Raw encoder not configured");
        return FALSE;
    }

    if (drd_frame_get_width(input) != self->width || drd_frame_get_height(input) != self->height)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Frame dimensions mismatch encoder configuration (%ux%u vs %ux%u)",
                    drd_frame_get_width(input),
                    drd_frame_get_height(input),
                    self->width,
                    self->height);
        return FALSE;
    }

    const guint32 expected_stride = self->width * 4u;
    const gsize output_size = (gsize)expected_stride * (gsize)self->height;

    guint8 *payload = drd_encoded_frame_ensure_capacity(output, output_size);
    if (payload == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to allocate payload buffer");
        return FALSE;
    }

    const guint8 *src = drd_frame_get_data(input, NULL);
    const guint stride_in = drd_frame_get_stride(input);

    for (guint y = 0; y < self->height; y++)
    {
        const guint8 *src_row = src + (gsize)stride_in * (self->height - 1 - y);
        guint8 *dst_row = payload + (gsize)expected_stride * y;
        memcpy(dst_row, src_row, expected_stride);
    }

    drd_encoded_frame_configure(output,
                                 self->width,
                                 self->height,
                                 expected_stride,
                                 TRUE,
                                 drd_frame_get_timestamp(input),
                                 DRD_FRAME_CODEC_RAW);
    drd_encoded_frame_set_quality(output, 100, 0, TRUE);
    return TRUE;
}
