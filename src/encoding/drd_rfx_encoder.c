#include "encoding/drd_rfx_encoder.h"

#include <freerdp/codec/rfx.h>
#include <freerdp/codec/color.h>
#include <winpr/stream.h>

#include <gio/gio.h>
#include <string.h>

#include "utils/drd_log.h"

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

/*
 * 功能：对 64 位整数执行左循环位移，作为简易 hash 辅助。
 * 逻辑：将值左移指定位数并与右移互补位或运算。
 * 参数：value 原始数；shift 左移位数（0-63）。
 * 外部接口：无外部库调用。
 */
static inline guint64
drd_rotl64(guint64 value, guint shift)
{
    return (value << shift) | (value >> (64 - shift));
}

/*
 * 功能：将 64 位块混入 hash，模仿 splitmix64 的扰动。
 * 逻辑：对 chunk 做数轮 xor/乘法/移位，再与 hash 异或与循环左移，提升熵。
 * 参数：hash 当前 hash 值；chunk 待混入数据。
 * 外部接口：无外部库调用。
 */
static inline guint64
drd_mix_chunk(guint64 hash, guint64 chunk)
{
    chunk ^= chunk >> 30;
    chunk *= G_GUINT64_CONSTANT(0xbf58476d1ce4e5b9);
    chunk ^= chunk >> 27;
    chunk *= G_GUINT64_CONSTANT(0x94d049bb133111eb);
    chunk ^= chunk >> 31;

    hash ^= chunk;
    hash = drd_rotl64(hash, 29);
    hash *= G_GUINT64_CONSTANT(0x9e3779b185ebca87);
    return hash;
}

/* 对每个 tile 采用 16/8 字节块混合，避免逐字节 FNV 乘法带来的 CPU 压力。 */
/*
 * 功能：计算指定 tile 的哈希，用于检测差分编码的 dirty tile。
 * 逻辑：遍历 tile 行，将每行数据按 16/8 字节块混入 hash，处理尾部剩余字节时附加长度，
 *       返回滚动后的 64 位哈希。
 * 参数：data 帧线性缓冲；stride 帧步长；x/y tile 左上角；width/height tile 尺寸。
 * 外部接口：使用 C 标准库 memcpy 拷贝块数据。
 */
static guint64
hash_tile(const guint8 *data,
          guint stride,
          guint32 x,
          guint32 y,
          guint32 width,
          guint32 height)
{
    guint64 hash = G_GUINT64_CONSTANT(0xcbf29ce484222325);
    const guint32 bytes_per_row = width * 4u;

    for (guint row = 0; row < height; ++row)
    {
        const guint8 *ptr = data + ((gsize)(y + row) * stride) + (gsize) x * 4;
        guint32 remaining = bytes_per_row;

        while (remaining >= 16)
        {
            guint64 lo, hi;
            memcpy(&lo, ptr, sizeof(lo));
            memcpy(&hi, ptr + 8, sizeof(hi));
            hash = drd_mix_chunk(hash, lo);
            hash = drd_mix_chunk(hash, hi);
            ptr += 16;
            remaining -= 16;
        }

        while (remaining >= 8)
        {
            guint64 chunk;
            memcpy(&chunk, ptr, sizeof(chunk));
            hash = drd_mix_chunk(hash, chunk);
            ptr += 8;
            remaining -= 8;
        }

        if (remaining > 0)
        {
            guint64 tail = 0;
            memcpy(&tail, ptr, remaining);
            tail ^= ((guint64) remaining << 56);
            hash = drd_mix_chunk(hash, tail);
        }
    }

    return hash;
}

/*
 * 功能：释放 RFX 编码器持有的 FreeRDP 上下文与缓存。
 * 逻辑：若 context 存在则调用 rfx_context_free；释放底朝上帧、前一帧、tile 哈希数组；
 *       最后委托父类 dispose。
 * 参数：object GObject 指针。
 * 外部接口：调用 FreeRDP 的 rfx_context_free 释放 RFX 上下文；使用 GLib g_clear_pointer。
 */
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

/*
 * 功能：设置 RFX 编码器类的 dispose 回调。
 * 逻辑：将自定义 dispose 赋给 GObjectClass。
 * 参数：klass 类结构指针。
 * 外部接口：GLib GObject 类型系统。
 */
static void
drd_rfx_encoder_class_init(DrdRfxEncoderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_rfx_encoder_dispose;
}

/*
 * 功能：初始化 RFX 编码器实例的内部状态。
 * 逻辑：清零尺寸与差分开关，创建底朝上帧/上一帧 buffer、tile hash 数组并设置初始标志位。
 * 参数：self RFX 编码器实例。
 * 外部接口：使用 GLib g_byte_array_new/g_array_new 分配缓存。
 */
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

/*
 * 功能：创建 RFX 编码器实例。
 * 逻辑：调用 g_object_new 分配对象。
 * 参数：无。
 * 外部接口：GLib GObject 工厂。
 */
DrdRfxEncoder *
drd_rfx_encoder_new(void)
{
    return g_object_new(DRD_TYPE_RFX_ENCODER, NULL);
}

/*
 * 功能：根据分辨率和差分开关配置 RFX 编码器上下文。
 * 逻辑：校验宽高 -> 若旧 context 存在则释放 -> 通过 FreeRDP rfx_context_new 创建并设定像素格式、
 *       rfx_context_reset 重置尺寸、rfx_context_set_mode 设置编码模式 -> 初始化缓冲与 tile hash。
 * 参数：self 编码器；width/height 分辨率；enable_diff 是否开启 tile 哈希差分；error 输出错误。
 * 外部接口：调用 FreeRDP 的 rfx_context_new/rfx_context_reset/rfx_context_set_pixel_format/
 *           rfx_context_set_mode；使用 GLib g_set_error 报错，g_byte_array_set_size 初始化缓存。
 */
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

    g_byte_array_set_size(self->bottom_up_frame, (gsize) width * height * 4u);
    memset(self->bottom_up_frame->data, 0, self->bottom_up_frame->len);

    g_byte_array_set_size(self->previous_frame, (gsize) width * height * 4u);
    memset(self->previous_frame->data, 0, self->previous_frame->len);

    self->tiles_x = (width + 63) / 64;
    self->tiles_y = (height + 63) / 64;
    g_array_set_size(self->tile_hashes, self->tiles_x * self->tiles_y);
    memset(self->tile_hashes->data, 0, self->tile_hashes->len * sizeof(guint64));

    return TRUE;
}

/*
 * 功能：重置 RFX 编码器状态，释放上下文与缓存。
 * 逻辑：释放 rfx_context，清空底朝上帧/上一帧/tile hash，重置尺寸、差分、标志位。
 * 参数：self RFX 编码器。
 * 外部接口：FreeRDP rfx_context_free 释放上下文；GLib g_byte_array_set_size/g_array_set_size 清空缓存。
 */
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

/*
 * 功能：将帧数据按行拷贝为无翻转的线性缓冲。
 * 逻辑：按 frame stride 逐行 memcpy 到目标 GByteArray，便于 RFX 编码消费。
 * 参数：frame 输入帧；buffer 目标缓冲。
 * 外部接口：C 标准库 memcpy。
 */
static const guint8 *
copy_frame_linear(DrdFrame *frame, GByteArray *buffer)
{
    const guint stride = drd_frame_get_stride(frame);
    const guint width = drd_frame_get_width(frame);
    const guint height = drd_frame_get_height(frame);
    const guint bytes_per_row = width * 4u;

    g_byte_array_set_size(buffer, (gsize) bytes_per_row * height);
    guint8 *dst = buffer->data;
    const guint8 *src = drd_frame_get_data(frame, NULL);

    for (guint row = 0; row < height; ++row)
    {
        const guint8 *src_row = src + (gsize) row * stride;
        memcpy(dst + (gsize) row * bytes_per_row, src_row, bytes_per_row);
    }

    return dst;
}

/*
 * 功能：向 WinPR wStream 写入 32 位无符号整数。
 * 逻辑：直接调用 Stream_Write_UINT32_unchecked 写入到当前写指针。
 * 参数：stream 目标流；value 写入值。
 * 外部接口：WinPR Stream_Write_UINT32_unchecked。
 */
static inline void
drd_stream_write_uint32(wStream *stream, UINT32 value)
{
    Stream_Write_UINT32_unchecked(stream, value);
}

/*
 * 功能：向 WinPR wStream 写入 16 位无符号整数。
 * 逻辑：调用 Stream_Write_UINT16_unchecked 直接写入。
 * 参数：stream 目标流；value 写入值。
 * 外部接口：WinPR Stream_Write_UINT16_unchecked。
 */
static inline void
drd_stream_write_uint16(wStream *stream, UINT16 value)
{
    Stream_Write_UINT16_unchecked(stream, value);
}

/*
 * 功能：向 WinPR wStream 写入 8 位无符号整数。
 * 逻辑：调用 Stream_Write_UINT8_unchecked。
 * 参数：stream 目标流；value 写入值。
 * 外部接口：WinPR Stream_Write_UINT8_unchecked。
 */
static inline void
drd_stream_write_uint8(wStream *stream, UINT8 value)
{
    Stream_Write_UINT8_unchecked(stream, value);
}

/*
 * 功能：按照 RFX Progressive 协议将编码结果序列化到 wStream。
 * 逻辑：提取 rect/quant/tile 信息 -> 可选写入 SYNC/CONTEXT 头 -> 写 FrameBegin/Region/Tiles/FrameEnd
 *       各段数据，过程中确保容量并填充字段。
 * 参数：rfx_message FreeRDP 编码生成的消息；stream WinPR 流；needs_progressive_header 是否需要头部；
 *       error 输出错误。
 * 外部接口：大量使用 WinPR Stream_EnsureRemainingCapacity/Stream_Write* 写入，读取 RFX_MESSAGE
 *           结构由 FreeRDP rfx_message_get_* 提供。
 */
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
        drd_stream_write_uint8(stream, 0); /* ctxId */
        drd_stream_write_uint16(stream, 0x0040);
        drd_stream_write_uint8(stream, 0); /* flags */
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
    drd_stream_write_uint8(stream, 0x40); /* tileSize */
    drd_stream_write_uint16(stream, n_rfx_rects);
    drd_stream_write_uint8(stream, n_quant_vals);
    drd_stream_write_uint8(stream, 0); /* numProgQuant */
    drd_stream_write_uint8(stream, 0); /* flags */
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

/*
 * 功能：根据编码模式将 RFX 消息写入 wStream。
 * 逻辑：SurfaceBits 模式直接调用 FreeRDP rfx_write_message；Progressive 模式调用自定义
 *       drd_rfx_encoder_write_progressive_message 并标记头部已发送。
 * 参数：self 编码器；kind 模式；stream 目标流；message RFX_MESSAGE；error 输出错误。
 * 外部接口：FreeRDP rfx_write_message 完成 SurfaceBits 序列化。
 */
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

/*
 * 功能：基于 tile 哈希检测脏矩形以支持差分编码。
 * 逻辑：遍历 64x64 tile，计算 hash 与缓存比较；若 hash 变化再按行精细比较以剔除哈希碰撞，
 *       将变化 tile 作为 RFX_RECT 追加到 rects。
 * 参数：self 编码器；data 当前帧线性缓冲；previous 上一帧数据；stride 行步长；rects 输出脏矩形数组。
 * 外部接口：C 标准库 memcmp；GLib g_array_append_val/g_array_index 维护数组。
 */
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
                    RFX_RECT rect = {(UINT16) x, (UINT16) y, (UINT16) tile_w, (UINT16) tile_h};
                    g_array_append_val(rects, rect);
                    has_dirty = TRUE;
                }

                *stored = hash;
            }
        }
    }

    return has_dirty;
}

/*
 * 功能：执行 RFX 或 RFX Progressive 编码，支持关键帧/增量与差分 tile。
 * 逻辑：校验上下文与尺寸 -> 重置 context -> 拷贝帧为线性 buffer -> 通过差分检测决定编码全部或脏块；
 *       调用 FreeRDP rfx_encode_message 生成消息 -> 根据模式写入 wStream -> 填充 DrdEncodedFrame，
 *       更新 previous_frame 并维护关键帧标志。
 * 参数：self 编码器；frame 输入帧；output 编码结果；kind 编码模式；error 输出错误。
 * 外部接口：使用 FreeRDP rfx_encode_message/rfx_message_free 进行编码，
 *           WinPR Stream_New/Stream_GetPosition/Stream_Buffer/Stream_Free 管理缓冲，
 *           通过 DRD_LOG_MESSAGE 输出关键帧日志。
 */
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
            (kind == DRD_RFX_ENCODER_KIND_PROGRESSIVE)
                ? DRD_FRAME_CODEC_RFX_PROGRESSIVE
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

    g_autoptr(GArray)
    rects = g_array_sized_new(FALSE, FALSE, sizeof(RFX_RECT),
                              self->tiles_x * self->tiles_y);

    const guint8 *previous_linear = (self->previous_frame->len ==
                                     self->bottom_up_frame->len)
                                        ? self->previous_frame->data
                                        : NULL;

    const gboolean keyframe_encode = self->force_keyframe || !self->enable_diff;

    if (keyframe_encode)
    {
        DRD_LOG_MESSAGE("key frame encode");
        for (guint idx = 0; idx < self->tile_hashes->len; ++idx)
        {
            g_array_index(self->tile_hashes, guint64, idx) = 0;
        }
        RFX_RECT full = {0, 0, (UINT16) self->width, (UINT16) self->height};
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
                                              (RFX_RECT *) rects->data,
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

    wStream *stream = Stream_New(NULL, (gsize) self->width * self->height * 4u);
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
    /* RFX 已由 FreeRDP 序列化为连续 buffer，无需转换，直接 set_payload 复制。 */
    if (!drd_encoded_frame_set_payload(output, payload_data, payload_size))
    {
        Stream_Free(stream, TRUE);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to persist encoded payload");
        return FALSE;
    }
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

/*
 * 功能：强制下一帧编码为关键帧并重置 progressive 头状态。
 * 逻辑：置 force_keyframe 和 progressive_header_sent 标志，促使后续编码包含完整头。
 * 参数：self RFX 编码器。
 * 外部接口：无额外外部库调用。
 */
void
drd_rfx_encoder_force_keyframe(DrdRfxEncoder *self)
{
    g_return_if_fail(DRD_IS_RFX_ENCODER(self));
    self->force_keyframe = TRUE;
    self->progressive_header_sent = FALSE;
}
