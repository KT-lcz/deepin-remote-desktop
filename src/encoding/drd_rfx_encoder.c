#include "encoding/drd_rfx_encoder.h"

#include <freerdp/codec/rfx.h>
#include <freerdp/codec/color.h>
#include <winpr/stream.h>

#include <gio/gio.h>
#include <string.h>

struct _DrdRfxEncoder
{
    GObject parent_instance;

    RFX_CONTEXT *context;
    guint width;
    guint height;
    gboolean enable_diff;
    GByteArray *bottom_up_frame;
    GByteArray *previous_frame;
    GArray *tile_hashes;
    guint tiles_x;
    guint tiles_y;
    gboolean force_keyframe;
    gboolean progressive_header_sent;
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
    self->progressive_header_sent = FALSE;
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

    rfx_context_set_mode(self->context, RLGR1);

    self->width = width;
    self->height = height;
    self->enable_diff = enable_diff;
    self->force_keyframe = TRUE;
    self->progressive_header_sent = FALSE;

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
    self->progressive_header_sent = FALSE;
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

static inline void
drd_stream_write_uint32(wStream *stream, UINT32 value)
{
    Stream_Write_UINT32_unchecked(stream, value);
}

static inline void
drd_stream_write_uint16(wStream *stream, UINT16 value)
{
    Stream_Write_UINT16_unchecked(stream, value);
}

static inline void
drd_stream_write_uint8(wStream *stream, UINT8 value)
{
    Stream_Write_UINT8_unchecked(stream, value);
}

static gboolean
drd_rfx_encoder_write_progressive_message(RFX_MESSAGE *rfx_message,
                                          wStream *stream,
                                          gboolean needs_progressive_header,
                                          GError **error)
{
    const RFX_RECT *rfx_rects = NULL;
    UINT16 n_rfx_rects = 0;
    const UINT32 *quant_vals = NULL;
    UINT16 n_quant_vals = 0;
    const RFX_TILE **rfx_tiles = NULL;
    UINT16 n_rfx_tiles = 0;
    UINT32 block_len = 0;
    UINT32 tiles_data_size = 0;
    const UINT32 *qv = NULL;
    const RFX_TILE *rfx_tile = NULL;
    UINT16 i = 0;

    rfx_rects = rfx_message_get_rects(rfx_message, &n_rfx_rects);
    quant_vals = rfx_message_get_quants(rfx_message, &n_quant_vals);
    rfx_tiles = rfx_message_get_tiles(rfx_message, &n_rfx_tiles);

    if (needs_progressive_header)
    {
        block_len = 12;
        if (!Stream_EnsureRemainingCapacity(stream, block_len))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to write RFX_PROGRESSIVE_SYNC block");
            return FALSE;
        }

        drd_stream_write_uint16(stream, 0xCCC0);
        drd_stream_write_uint32(stream, block_len);
        drd_stream_write_uint32(stream, 0xCACCACCA);
        drd_stream_write_uint16(stream, 0x0100);

        block_len = 10;
        if (!Stream_EnsureRemainingCapacity(stream, block_len))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to write RFX_PROGRESSIVE_CONTEXT block");
            return FALSE;
        }

        drd_stream_write_uint16(stream, 0xCCC3);
        drd_stream_write_uint32(stream, block_len);
        drd_stream_write_uint8(stream, 0);   /* ctxId */
        drd_stream_write_uint16(stream, 0x0040);
        drd_stream_write_uint8(stream, 0);   /* flags */
    }

    block_len = 12;
    if (!Stream_EnsureRemainingCapacity(stream, block_len))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to write RFX_PROGRESSIVE_FRAME_BEGIN block");
        return FALSE;
    }

    drd_stream_write_uint16(stream, 0xCCC1);
    drd_stream_write_uint32(stream, block_len);
    drd_stream_write_uint32(stream, rfx_message_get_frame_idx(rfx_message));
    drd_stream_write_uint16(stream, 1); /* regionCount */

    tiles_data_size = n_rfx_tiles * 22;
    for (i = 0; i < n_rfx_tiles; ++i)
    {
        rfx_tile = rfx_tiles[i];
        tiles_data_size += rfx_tile->YLen + rfx_tile->CbLen + rfx_tile->CrLen;
    }

    block_len = 18;
    block_len += n_rfx_rects * 8;
    block_len += n_quant_vals * 5;
    block_len += tiles_data_size;

    if (!Stream_EnsureRemainingCapacity(stream, block_len))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to write RFX_PROGRESSIVE_REGION block");
        return FALSE;
    }

    drd_stream_write_uint16(stream, 0xCCC4);
    drd_stream_write_uint32(stream, block_len);
    drd_stream_write_uint8(stream, 0x40);          /* tileSize */
    drd_stream_write_uint16(stream, n_rfx_rects);
    drd_stream_write_uint8(stream, n_quant_vals);
    drd_stream_write_uint8(stream, 0);             /* numProgQuant */
    drd_stream_write_uint8(stream, 0);             /* flags */
    drd_stream_write_uint16(stream, n_rfx_tiles);
    drd_stream_write_uint32(stream, tiles_data_size);

    for (i = 0; i < n_rfx_rects; ++i)
    {
        drd_stream_write_uint16(stream, rfx_rects[i].x);
        drd_stream_write_uint16(stream, rfx_rects[i].y);
        drd_stream_write_uint16(stream, rfx_rects[i].width);
        drd_stream_write_uint16(stream, rfx_rects[i].height);
    }

    for (i = 0, qv = quant_vals; i < n_quant_vals; ++i, qv += 10)
    {
        drd_stream_write_uint8(stream, qv[0] + (qv[2] << 4));
        drd_stream_write_uint8(stream, qv[1] + (qv[3] << 4));
        drd_stream_write_uint8(stream, qv[5] + (qv[4] << 4));
        drd_stream_write_uint8(stream, qv[6] + (qv[8] << 4));
        drd_stream_write_uint8(stream, qv[7] + (qv[9] << 4));
    }

    for (i = 0; i < n_rfx_tiles; ++i)
    {
        rfx_tile = rfx_tiles[i];
        block_len = 22 + rfx_tile->YLen + rfx_tile->CbLen + rfx_tile->CrLen;
        if (!Stream_EnsureRemainingCapacity(stream, block_len))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to write RFX_PROGRESSIVE_TILE block");
            return FALSE;
        }

        drd_stream_write_uint16(stream, 0xCCC5);
        drd_stream_write_uint32(stream, block_len);
        drd_stream_write_uint8(stream, rfx_tile->quantIdxY);
        drd_stream_write_uint8(stream, rfx_tile->quantIdxCb);
        drd_stream_write_uint8(stream, rfx_tile->quantIdxCr);
        drd_stream_write_uint16(stream, rfx_tile->xIdx);
        drd_stream_write_uint16(stream, rfx_tile->yIdx);
        drd_stream_write_uint8(stream, 0); /* flags */
        drd_stream_write_uint16(stream, rfx_tile->YLen);
        drd_stream_write_uint16(stream, rfx_tile->CbLen);
        drd_stream_write_uint16(stream, rfx_tile->CrLen);
        drd_stream_write_uint16(stream, 0); /* tailLen */
        Stream_Write(stream, rfx_tile->YData, rfx_tile->YLen);
        Stream_Write(stream, rfx_tile->CbData, rfx_tile->CbLen);
        Stream_Write(stream, rfx_tile->CrData, rfx_tile->CrLen);
    }

    block_len = 6;
    if (!Stream_EnsureRemainingCapacity(stream, block_len))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to write RFX_PROGRESSIVE_FRAME_END block");
        return FALSE;
    }

    drd_stream_write_uint16(stream, 0xCCC2);
    drd_stream_write_uint32(stream, block_len);
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
        {
            gboolean include_header = !self->progressive_header_sent;
            if (!drd_rfx_encoder_write_progressive_message(message,
                                                           stream,
                                                           include_header,
                                                           error))
            {
                return FALSE;
            }
            self->progressive_header_sent = TRUE;
            return TRUE;
        }
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

    const gboolean keyframe_encode = self->force_keyframe || !self->enable_diff;

    if (keyframe_encode)
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
    drd_encoded_frame_set_quality(output, 0, 0, keyframe_encode);

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
    self->progressive_header_sent = FALSE;
}
