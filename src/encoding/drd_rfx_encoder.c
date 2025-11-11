#include "encoding/drd_rfx_encoder.h"

#include <freerdp/codec/rfx.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/progressive.h>
#include <winpr/stream.h>

#include <gio/gio.h>
#include <string.h>

struct _DrdRfxEncoder
{
    GObject parent_instance;

    RFX_CONTEXT *context;
    PROGRESSIVE_CONTEXT *progressive;
    guint width;
    guint height;
    gboolean enable_diff;
    GByteArray *bottom_up_frame;
    GByteArray *previous_frame;
    GArray *tile_hashes;
    guint tiles_x;
    guint tiles_y;
    gboolean force_keyframe;
};

G_DEFINE_TYPE(DrdRfxEncoder, drd_rfx_encoder, G_TYPE_OBJECT)

static guint64
hash_tile(const guint8 *data,
          guint stride,
          guint32 x,
          guint32 y,
          guint32 width,
          guint32 height)
{
    const guint64 fnv_offset = 1469598103934665603ULL;
    const guint64 fnv_prime = 1099511628211ULL;
    guint64 hash = fnv_offset;

    for (guint row = 0; row < height; ++row)
    {
        const guint8 *ptr = data + ((gsize)(y + row) * stride) + (gsize)x * 4;
        for (guint col = 0; col < width * 4; ++col)
        {
            hash ^= ptr[col];
            hash *= fnv_prime;
        }
    }

    return hash;
}

static void
drd_rfx_encoder_dispose(GObject *object)
{
    DrdRfxEncoder *self = DRD_RFX_ENCODER(object);

    if (self->context != NULL)
    {
        rfx_context_free(self->context);
        self->context = NULL;
    }
    if (self->progressive != NULL)
    {
        progressive_context_reset(self->progressive);
    }
    if (self->progressive != NULL)
    {
        progressive_context_free(self->progressive);
        self->progressive = NULL;
    }

    g_clear_pointer(&self->bottom_up_frame, g_byte_array_unref);
    g_clear_pointer(&self->previous_frame, g_byte_array_unref);
    g_clear_pointer(&self->tile_hashes, g_array_unref);

    G_OBJECT_CLASS(drd_rfx_encoder_parent_class)->dispose(object);
}

static void
drd_rfx_encoder_class_init(DrdRfxEncoderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_rfx_encoder_dispose;
}

static void
drd_rfx_encoder_init(DrdRfxEncoder *self)
{
    self->context = NULL;
    self->width = 0;
    self->height = 0;
    self->enable_diff = FALSE;
    self->bottom_up_frame = g_byte_array_new();
    self->previous_frame = g_byte_array_new();
    self->tile_hashes = g_array_new(FALSE, TRUE, sizeof(guint64));
    self->tiles_x = 0;
    self->tiles_y = 0;
}

DrdRfxEncoder *
drd_rfx_encoder_new(void)
{
    return g_object_new(DRD_TYPE_RFX_ENCODER, NULL);
}

gboolean
drd_rfx_encoder_configure(DrdRfxEncoder *self,
                           guint width,
                           guint height,
                           gboolean enable_diff,
                           GError **error)
{
    g_return_val_if_fail(DRD_IS_RFX_ENCODER(self), FALSE);

    if (width == 0 || height == 0)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "RemoteFX encoder requires non-zero width/height");
        return FALSE;
    }

    if (self->context != NULL)
    {
        rfx_context_free(self->context);
        self->context = NULL;
    }

    self->context = rfx_context_new(TRUE);
    if (self->context == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to create RFX context");
        return FALSE;
    }

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    rfx_context_set_pixel_format(self->context, PIXEL_FORMAT_BGRX32);
#else
    rfx_context_set_pixel_format(self->context, PIXEL_FORMAT_XRGB32);
#endif

    if (!rfx_context_reset(self->context, width, height))
    {
        rfx_context_free(self->context);
        self->context = NULL;
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to reset RFX context");
        return FALSE;
    }

    rfx_context_set_mode(self->context, RLGR3);

    self->width = width;
    self->height = height;
    self->enable_diff = enable_diff;
    self->force_keyframe = TRUE;

    g_byte_array_set_size(self->bottom_up_frame, (gsize)width * height * 4u);
    memset(self->bottom_up_frame->data, 0, self->bottom_up_frame->len);

    g_byte_array_set_size(self->previous_frame, (gsize)width * height * 4u);
    memset(self->previous_frame->data, 0, self->previous_frame->len);

    self->tiles_x = (width + 63) / 64;
    self->tiles_y = (height + 63) / 64;
    g_array_set_size(self->tile_hashes, self->tiles_x * self->tiles_y);
    memset(self->tile_hashes->data, 0, self->tile_hashes->len * sizeof(guint64));

    return TRUE;
}

void
drd_rfx_encoder_reset(DrdRfxEncoder *self)
{
    g_return_if_fail(DRD_IS_RFX_ENCODER(self));

    if (self->context != NULL)
    {
        rfx_context_free(self->context);
        self->context = NULL;
    }

    g_byte_array_set_size(self->bottom_up_frame, 0);
    g_byte_array_set_size(self->previous_frame, 0);
    g_array_set_size(self->tile_hashes, 0);

    self->width = 0;
    self->height = 0;
    self->enable_diff = FALSE;
    self->tiles_x = 0;
    self->tiles_y = 0;
    self->force_keyframe = TRUE;
}

static const guint8 *
copy_frame_linear(DrdFrame *frame, GByteArray *buffer)
{
    const guint stride = drd_frame_get_stride(frame);
    const guint width = drd_frame_get_width(frame);
    const guint height = drd_frame_get_height(frame);
    const guint bytes_per_row = width * 4u;

    g_byte_array_set_size(buffer, (gsize)bytes_per_row * height);
    guint8 *dst = buffer->data;
    const guint8 *src = drd_frame_get_data(frame, NULL);

    for (guint row = 0; row < height; ++row)
    {
        const guint8 *src_row = src + (gsize)row * stride;
        memcpy(dst + (gsize)row * bytes_per_row, src_row, bytes_per_row);
    }

    return dst;
}

static gboolean
drd_rfx_encoder_ensure_progressive(DrdRfxEncoder *self, GError **error)
{
    if (self->progressive == NULL)
    {
        self->progressive = progressive_context_new(TRUE);
        if (self->progressive == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to allocate Progressive RFX context");
            return FALSE;
        }
    }

    if (!progressive_context_reset(self->progressive))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to reset Progressive RFX context");
        return FALSE;
    }

    return TRUE;
}

static gboolean
drd_rfx_encoder_write_stream(DrdRfxEncoder *self,
                              DrdRfxEncoderKind kind,
                              wStream *stream,
                              RFX_MESSAGE *message,
                              GError **error)
{
    Stream_SetPosition(stream, 0);

    switch (kind)
    {
        case DRD_RFX_ENCODER_KIND_SURFACE_BITS:
            if (!rfx_write_message(self->context, stream, message))
            {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_FAILED,
                                    "Failed to write RFX SurfaceBits message");
                return FALSE;
            }
            return TRUE;
        case DRD_RFX_ENCODER_KIND_PROGRESSIVE:
            if (!drd_rfx_encoder_ensure_progressive(self, error))
            {
                return FALSE;
            }
            if (!progressive_rfx_write_message_progressive_simple(self->progressive,
                                                                  stream,
                                                                  message))
            {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_FAILED,
                                    "Failed to write Progressive RFX message");
                return FALSE;
            }
            return TRUE;
        default:
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Unsupported RFX encoder kind");
            return FALSE;
    }
}

static gboolean
collect_dirty_rects(DrdRfxEncoder *self,
                    const guint8 *data,
                    const guint8 *previous,
                    guint stride,
                    GArray *rects)
{
    if (self->tiles_x == 0 || self->tiles_y == 0)
    {
        return FALSE;
    }

    gboolean has_dirty = FALSE;

    for (guint y = 0; y < self->height; y += 64)
    {
        const guint tile_h = MIN(64u, self->height - y);
        for (guint x = 0; x < self->width; x += 64)
        {
            const guint tile_w = MIN(64u, self->width - x);
            const guint index = (y / 64) * self->tiles_x + (x / 64);
            guint64 hash = hash_tile(data, stride, x, y, tile_w, tile_h);
            guint64 *stored = &g_array_index(self->tile_hashes, guint64, index);
            if (*stored != hash)
            {
                gboolean different = TRUE;
                if (previous != NULL)
                {
                    different = FALSE;
                    for (guint row = 0; row < tile_h; ++row)
                    {
                        const guint offset = ((y + row) * stride) + x * 4;
                        if (memcmp(previous + offset, data + offset, tile_w * 4) != 0)
                        {
                            different = TRUE;
                            break;
                        }
                    }
                }

                if (different)
                {
                    RFX_RECT rect = {(UINT16)x, (UINT16)y, (UINT16)tile_w, (UINT16)tile_h};
                    g_array_append_val(rects, rect);
                    has_dirty = TRUE;
                }

                *stored = hash;
            }
        }
    }

    return has_dirty;
}

gboolean
drd_rfx_encoder_encode(DrdRfxEncoder *self,
                        DrdFrame *frame,
                        DrdEncodedFrame *output,
                        DrdRfxEncoderKind kind,
                        GError **error)
{
    g_return_val_if_fail(DRD_IS_RFX_ENCODER(self), FALSE);
    g_return_val_if_fail(DRD_IS_FRAME(frame), FALSE);
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(output), FALSE);

    if (self->context == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "RFX context not initialized");
        return FALSE;
    }

    if (drd_frame_get_width(frame) != self->width ||
        drd_frame_get_height(frame) != self->height)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Frame dimensions mismatch RFX configuration (%ux%u vs %ux%u)",
                    drd_frame_get_width(frame),
                    drd_frame_get_height(frame),
                    self->width,
                    self->height);
        return FALSE;
    }

    guint64 timestamp = drd_frame_get_timestamp(frame);
    DrdFrameCodec frame_codec =
        (kind == DRD_RFX_ENCODER_KIND_PROGRESSIVE) ? DRD_FRAME_CODEC_RFX_PROGRESSIVE
                                                   : DRD_FRAME_CODEC_RFX;
    if (!rfx_context_reset(self->context, self->width, self->height))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to reset RFX context");
        return FALSE;
    }

    const guint8 *linear_frame = copy_frame_linear(frame, self->bottom_up_frame);
    const guint expected_stride = self->width * 4u;

    g_autoptr(GArray) rects = g_array_sized_new(FALSE, FALSE, sizeof(RFX_RECT),
                                                self->tiles_x * self->tiles_y);

    const guint8 *previous_linear = (self->previous_frame->len ==
                                     self->bottom_up_frame->len)
                                        ? self->previous_frame->data
                                        : NULL;

    if (self->force_keyframe || !self->enable_diff)
    {
        for (guint idx = 0; idx < self->tile_hashes->len; ++idx)
        {
            g_array_index(self->tile_hashes, guint64, idx) = 0;
        }
        RFX_RECT full = {0, 0, (UINT16)self->width, (UINT16)self->height};
        g_array_append_val(rects, full);
    }
    else if (!collect_dirty_rects(self, linear_frame, previous_linear, expected_stride, rects))
    {
        drd_encoded_frame_configure(output,
                                     self->width,
                                     self->height,
                                     drd_frame_get_stride(frame),
                                     FALSE,
                                     timestamp,
                                     frame_codec);
        drd_encoded_frame_set_quality(output, 0, 0, FALSE);
        return TRUE;
    }

    RFX_MESSAGE *message = rfx_encode_message(self->context,
                                              (RFX_RECT *)rects->data,
                                              rects->len,
                                              linear_frame,
                                              self->width,
                                              self->height,
                                              expected_stride);
    if (message == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to encode RFX message");
        return FALSE;
    }

    wStream *stream = Stream_New(NULL, (gsize)self->width * self->height * 4u);
    if (stream == NULL)
    {
        rfx_message_free(self->context, message);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to allocate RFX stream");
        return FALSE;
    }

    gboolean ok = drd_rfx_encoder_write_stream(self, kind, stream, message, error);
    rfx_message_free(self->context, message);

    if (!ok)
    {
        Stream_Free(stream, TRUE);
        return FALSE;
    }

    gsize payload_size = Stream_GetPosition(stream);
    const guint8 *payload_data = Stream_Buffer(stream);

    drd_encoded_frame_configure(output,
                                 self->width,
                                 self->height,
                                 expected_stride,
                                 FALSE,
                                 timestamp,
                                 frame_codec);
    guint8 *payload = drd_encoded_frame_ensure_capacity(output, payload_size);
    memcpy(payload, payload_data, payload_size);
    drd_encoded_frame_set_quality(output, 0, 0, TRUE);

    Stream_Free(stream, TRUE);

    if (self->previous_frame->len != self->bottom_up_frame->len)
    {
        g_byte_array_set_size(self->previous_frame, self->bottom_up_frame->len);
    }
    memcpy(self->previous_frame->data, linear_frame, self->previous_frame->len);
    self->force_keyframe = FALSE;

    return TRUE;
}

void
drd_rfx_encoder_force_keyframe(DrdRfxEncoder *self)
{
    g_return_if_fail(DRD_IS_RFX_ENCODER(self));
    self->force_keyframe = TRUE;
}
