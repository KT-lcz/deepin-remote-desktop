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

/*
 * 功能：Raw 编码器类初始化，当前无额外类级行为。
 * 逻辑：仅占位，保持 GObject 默认实现。
 * 参数：klass Raw 编码器类指针。
 * 外部接口：无外部库调用。
 */
static void
drd_raw_encoder_class_init(DrdRawEncoderClass *klass)
{
    (void) klass;
}

/*
 * 功能：初始化 Raw 编码器实例字段。
 * 逻辑：清零宽高并标记未准备。
 * 参数：self Raw 编码器实例。
 * 外部接口：无。
 */
static void
drd_raw_encoder_init(DrdRawEncoder *self)
{
    self->width = 0;
    self->height = 0;
    self->ready = FALSE;
}

/*
 * 功能：创建 Raw 编码器实例。
 * 逻辑：调用 g_object_new 分配对象。
 * 参数：无。
 * 外部接口：使用 GLib GObject 工厂。
 */
DrdRawEncoder *
drd_raw_encoder_new(void)
{
    return g_object_new(DRD_TYPE_RAW_ENCODER, NULL);
}

/*
 * 功能：配置 Raw 编码器的输出分辨率。
 * 逻辑：校验宽高非零，否则设置错误；保存宽高并标记 ready。
 * 参数：self Raw 编码器；width/height 目标分辨率；error 输出错误。
 * 外部接口：GLib g_set_error 报告参数异常。
 */
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

/*
 * 功能：重置 Raw 编码器配置。
 * 逻辑：清除 ready 标记并将宽高置零。
 * 参数：self Raw 编码器实例。
 * 外部接口：无。
 */
void
drd_raw_encoder_reset(DrdRawEncoder *self)
{
    g_return_if_fail(DRD_IS_RAW_ENCODER(self));
    self->ready = FALSE;
    self->width = 0;
    self->height = 0;
}

typedef struct
{
    const guint8 *src;
    guint stride_in;
    guint expected_stride;
    guint height;
} DrdRawEncoderCopyContext;

/*
 * 功能：将输入帧逐行翻转复制到底朝上的目标缓冲。
 * 逻辑：按 height 自底向上遍历，利用 expected_stride 写入目标，保持 BGRA32 行对齐。
 * 参数：dest 目标缓冲；size 目标大小（需等于 expected_stride*height）；user_data 复制上下文。
 * 外部接口：C 标准库 memcpy。
 */
static gboolean
drd_raw_encoder_copy_bottom_up(guint8 *dest, gsize size, gpointer user_data)
{
    DrdRawEncoderCopyContext *ctx = (DrdRawEncoderCopyContext *) user_data;
    g_return_val_if_fail(ctx != NULL, FALSE);
    g_return_val_if_fail(size == (gsize) ctx->expected_stride * ctx->height, FALSE);

    for (guint y = 0; y < ctx->height; y++)
    {
        const guint8 *src_row = ctx->src + (gsize) ctx->stride_in * (ctx->height - 1 - y);
        guint8 *dst_row = dest + (gsize) ctx->expected_stride * y;
        memcpy(dst_row, src_row, ctx->expected_stride);
    }

    return TRUE;
}

/*
 * 功能：将原始帧转为底层 SurfaceBits 所需的底朝上的 BGRA 缓冲。
 * 逻辑：校验准备状态与尺寸一致性 -> 分配输出 payload -> 按行翻转复制输入数据 ->
 *       配置编码帧元数据与质量。
 * 参数：self Raw 编码器；input 输入帧；output 编码结果；error 输出错误。
 * 外部接口：使用 GLib g_set_error 报告错误，依赖 C 标准库 memcpy 拷贝行数据，
 *           调用 drd_encoded_frame_* API 写入帧元数据。
 */
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
    const gsize output_size = (gsize) expected_stride * (gsize) self->height;

    const guint8 *src = drd_frame_get_data(input, NULL);
    const guint stride_in = drd_frame_get_stride(input);
    DrdRawEncoderCopyContext ctx = {
        .src = src,
        .stride_in = stride_in,
        .expected_stride = expected_stride,
        .height = self->height,
    };

    /* RAW 需在写入时完成 bottom-up 转换与 stride 对齐，因此走 fill_payload 回调写入。 */
    if (!drd_encoded_frame_fill_payload(output,
                                        output_size,
                                        drd_raw_encoder_copy_bottom_up,
                                        &ctx))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to write raw payload");
        return FALSE;
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
