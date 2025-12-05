#include "encoding/drd_encoding_manager.h"

#include <gio/gio.h>

#include "encoding/drd_raw_encoder.h"
#include "encoding/drd_rfx_encoder.h"
#include "utils/drd_log.h"

struct _DrdEncodingManager
{
    GObject parent_instance;

    guint frame_width;
    guint frame_height;
    gboolean ready;
    DrdEncodingMode mode;
    DrdRawEncoder *raw_encoder;
    DrdRfxEncoder *rfx_encoder;
    DrdEncodedFrame *scratch_frame;
    gboolean enable_diff;
    guint rfx_fallback_grace;
    gsize last_fallback_payload;
    guint rfx_fallback_count;
};

#define DRD_RFX_FALLBACK_GRACE_FRAMES 30

G_DEFINE_TYPE(DrdEncodingManager, drd_encoding_manager, G_TYPE_OBJECT)

/*
 * 功能：释放编码管理器持有的编码器及缓冲区，避免悬挂引用。
 * 逻辑：先调用 drd_encoding_manager_reset 清空运行时状态，再释放 raw_encoder 和 scratch_frame，
 *       最后交给父类 dispose 做剩余清理。
 * 参数：object GObject 指针，期望为 DrdEncodingManager 实例。
 * 外部接口：依赖 GLib 的 g_clear_object 处理引用计数，最终调用父类 GObjectClass::dispose。
 */
static void
drd_encoding_manager_dispose(GObject *object)
{
    DrdEncodingManager *self = DRD_ENCODING_MANAGER(object);
    drd_encoding_manager_reset(self);
    g_clear_object(&self->raw_encoder);
    g_clear_object(&self->scratch_frame);
    G_OBJECT_CLASS(drd_encoding_manager_parent_class)->dispose(object);
}

/*
 * 功能：初始化编码管理器的类回调。
 * 逻辑：注册自定义 dispose 以释放内部 encoder；其他生命周期保持默认。
 * 参数：klass 类结构指针。
 * 外部接口：使用 GLib 类型系统，将 dispose 挂载到 GObjectClass。
 */
static void
drd_encoding_manager_class_init(DrdEncodingManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_encoding_manager_dispose;
}

/*
 * 功能：初始化编码管理器的实例字段。
 * 逻辑：设置默认分辨率/编码模式/差分开关，创建 Raw/RFX 编码器与暂存帧，并清零回退统计。
 * 参数：self 编码管理器实例。
 * 外部接口：调用内部工厂 drd_raw_encoder_new、drd_rfx_encoder_new、drd_encoded_frame_new。
 */
static void
drd_encoding_manager_init(DrdEncodingManager *self)
{
    self->frame_width = 0;
    self->frame_height = 0;
    self->ready = FALSE;
    self->mode = DRD_ENCODING_MODE_RAW;
    self->enable_diff = TRUE;
    self->raw_encoder = drd_raw_encoder_new();
    self->rfx_encoder = drd_rfx_encoder_new();
    self->scratch_frame = drd_encoded_frame_new();
    self->rfx_fallback_grace = 0;
    self->last_fallback_payload = 0;
    self->rfx_fallback_count = 0;
}

/*
 * 功能：创建新的编码管理器实例。
 * 逻辑：委托 g_object_new 分配并初始化 GObject。
 * 参数：无。
 * 外部接口：使用 GLib 的 g_object_new 完成对象创建。
 */
DrdEncodingManager *
drd_encoding_manager_new(void)
{
    return g_object_new(DRD_TYPE_ENCODING_MANAGER, NULL);
}

/*
 * 功能：按给定编码参数准备 Raw/RFX 编码器。
 * 逻辑：校验分辨率非零 -> 记录分辨率/模式/差分标记 -> 先配置 Raw 编码器以便回退；
 *       当目标模式为 RFX 时继续配置 RFX 编码器；若任一步失败则重置状态并返回错误。
 * 参数：self 管理器；options 编码选项（分辨率、模式、差分开关等）；error 输出错误。
 * 外部接口：GLib g_set_error 报告参数/配置错误；调用内部 drd_raw_encoder_configure、
 *           drd_rfx_encoder_configure 完成具体编码器初始化；日志使用 DRD_LOG_MESSAGE。
 */
gboolean
drd_encoding_manager_prepare(DrdEncodingManager *self,
                             const DrdEncodingOptions *options,
                             GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);

    if (options->width == 0 || options->height == 0)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "Encoder resolution must be non-zero (width=%u height=%u)",
                    options->width,
                    options->height);
        return FALSE;
    }

    self->frame_width = options->width;
    self->frame_height = options->height;
    self->mode = options->mode;
    self->enable_diff = options->enable_frame_diff;
    self->rfx_fallback_grace = 0;
    self->last_fallback_payload = 0;
    self->rfx_fallback_count = 0;

    gboolean ok = FALSE;
    gboolean raw_ok = drd_raw_encoder_configure(self->raw_encoder,
                                                options->width,
                                                options->height,
                                                (options->mode == DRD_ENCODING_MODE_RAW) ? error : NULL);
    if (!raw_ok)
    {
        if (options->mode != DRD_ENCODING_MODE_RAW && error != NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to configure raw encoder fallback");
        }
        drd_encoding_manager_reset(self);
        return FALSE;
    }

    if (options->mode == DRD_ENCODING_MODE_RAW)
    {
        ok = TRUE;
    }
    else
    {
        ok = drd_rfx_encoder_configure(self->rfx_encoder,
                                       options->width,
                                       options->height,
                                       options->enable_frame_diff,
                                       error);
    }

    if (!ok)
    {
        drd_encoding_manager_reset(self);
        return FALSE;
    }

    self->ready = TRUE;

    DRD_LOG_MESSAGE("Encoding manager configured for %ux%u stream (mode=%s diff=%s)",
                    options->width,
                    options->height,
                    options->mode == DRD_ENCODING_MODE_RAW ? "raw" : "rfx",
                    options->enable_frame_diff ? "on" : "off");
    return TRUE;
}

/*
 * 功能：重置编码管理器状态，释放底层编码器状态。
 * 逻辑：若未准备好直接返回；清零分辨率/模式/回退统计，调用 Raw/RFX 编码器的 reset，
 *       置 ready 为 FALSE。
 * 参数：self 管理器实例。
 * 外部接口：调用 drd_raw_encoder_reset、drd_rfx_encoder_reset 复位内部 encoder，使用 DRD_LOG_MESSAGE 记录。
 */
void
drd_encoding_manager_reset(DrdEncodingManager *self)
{
    g_return_if_fail(DRD_IS_ENCODING_MANAGER(self));

    if (!self->ready)
    {
        return;
    }

    DRD_LOG_MESSAGE("Encoding manager reset");
    self->frame_width = 0;
    self->frame_height = 0;
    self->mode = DRD_ENCODING_MODE_RAW;
    self->enable_diff = TRUE;
    if (self->raw_encoder != NULL)
    {
        drd_raw_encoder_reset(self->raw_encoder);
    }
    if (self->rfx_encoder != NULL)
    {
        drd_rfx_encoder_reset(self->rfx_encoder);
    }
    self->ready = FALSE;
    self->rfx_fallback_grace = 0;
    self->last_fallback_payload = 0;
    self->rfx_fallback_count = 0;
}

/*
 * 功能：查询编码管理器是否已完成配置。
 * 逻辑：校验类型后返回 ready 标志。
 * 参数：self 管理器实例。
 * 外部接口：无额外外部库调用，仅依赖 GLib 类型检查宏。
 */
gboolean
drd_encoding_manager_is_ready(DrdEncodingManager *self)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);
    return self->ready;
}

/*
 * 功能：根据期望编码格式输出编码帧，并处理 RFX->RAW 回退。
 * 逻辑：检查准备状态与模式匹配 -> 根据 max_payload 与回退计数决定选择 RAW 还是 RFX；
 *       对 RFX 编码后若 payload 超限则回退 RAW 并设置宽限帧计数；成功时返回共享 scratch_frame。
 * 参数：self 管理器；input 原始帧；max_payload 对端可接受的最大包；desired_codec 期望编码器；
 *       out_frame 返回编码结果；error 输出错误。
 * 外部接口：调用 drd_raw_encoder_encode、drd_rfx_encoder_encode 完成实际编码；
 *           使用 DRD_LOG_MESSAGE/DRD_LOG_WARNING/DRD_LOG_DEBUG 记录流程。
 */
gboolean
drd_encoding_manager_encode(DrdEncodingManager *self,
                            DrdFrame *input,
                            gsize max_payload,
                            DrdFrameCodec desired_codec,
                            DrdEncodedFrame **out_frame,
                            GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);
    g_return_val_if_fail(DRD_IS_FRAME(input), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);

    if (!self->ready)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoding manager not prepared");
        return FALSE;
    }

    if (self->mode == DRD_ENCODING_MODE_RAW && desired_codec != DRD_FRAME_CODEC_RAW)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoding manager configured for RAW output only");
        return FALSE;
    }

    if (max_payload > 0 && self->last_fallback_payload > 0 &&
        max_payload > self->last_fallback_payload)
    {
        DRD_LOG_MESSAGE("RFX raw fallback cleared, peer payload limit increased to %zu", max_payload);
        self->rfx_fallback_grace = 0;
        self->last_fallback_payload = 0;
    }

    gboolean ok = FALSE;
    const gboolean prefer_raw =
            (desired_codec == DRD_FRAME_CODEC_RFX && max_payload > 0 && self->rfx_fallback_grace > 0);

    switch (desired_codec)
    {
        case DRD_FRAME_CODEC_RAW:
            ok = drd_raw_encoder_encode(self->raw_encoder, input, self->scratch_frame, error);
            break;
        case DRD_FRAME_CODEC_RFX:
            if (prefer_raw)
            {
                DRD_LOG_DEBUG("RFX raw grace active (%u frame(s) remaining, limit=%zu)",
                              self->rfx_fallback_grace,
                              self->last_fallback_payload);
                if (self->rfx_fallback_grace > 0)
                {
                    self->rfx_fallback_grace--;
                    if (self->rfx_fallback_grace == 0)
                    {
                        self->last_fallback_payload = 0;
                    }
                }
                ok = drd_raw_encoder_encode(self->raw_encoder, input, self->scratch_frame, error);
                break;
            }
            ok = drd_rfx_encoder_encode(self->rfx_encoder,
                                        input,
                                        self->scratch_frame,
                                        DRD_RFX_ENCODER_KIND_SURFACE_BITS,
                                        error);
            if (ok && max_payload > 0)
            {
                gsize payload_len = 0;
                drd_encoded_frame_get_data(self->scratch_frame, &payload_len);
                if (payload_len > max_payload)
                {
                    self->rfx_fallback_count++;
                    self->rfx_fallback_grace = DRD_RFX_FALLBACK_GRACE_FRAMES;
                    self->last_fallback_payload = max_payload;
                    DRD_LOG_WARNING("RFX payload %zu exceeds peer limit %zu, falling back to raw frame (count=%u, grace=%u)",
                                    payload_len,
                                    max_payload,
                                    self->rfx_fallback_count,
                                    self->rfx_fallback_grace);
                    ok = drd_raw_encoder_encode(self->raw_encoder,
                                                input,
                                                self->scratch_frame,
                                                error);
                }
            }
            break;
        case DRD_FRAME_CODEC_RFX_PROGRESSIVE:
            ok = drd_rfx_encoder_encode(self->rfx_encoder,
                                        input,
                                        self->scratch_frame,
                                        DRD_RFX_ENCODER_KIND_PROGRESSIVE,
                                        error);
            break;
        default:
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Unsupported codec requested");
            ok = FALSE;
            break;
    }

    if (!ok)
    {
        return FALSE;
    }

    *out_frame = g_object_ref(self->scratch_frame);
    return TRUE;
}

/*
 * 功能：返回当前编码器实际使用的帧编码格式。
 * 逻辑：根据 mode 选择 RAW 或 RFX 常量。
 * 参数：self 管理器实例。
 * 外部接口：无额外外部库调用。
 */
DrdFrameCodec
drd_encoding_manager_get_codec(DrdEncodingManager *self)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), DRD_FRAME_CODEC_RAW);
    return (self->mode == DRD_ENCODING_MODE_RAW) ? DRD_FRAME_CODEC_RAW : DRD_FRAME_CODEC_RFX;
}

/*
 * 功能：请求下一个 RFX 编码产生关键帧。
 * 逻辑：确认模式为 RFX 后调用 RFX 编码器的强制关键帧接口。
 * 参数：self 管理器实例。
 * 外部接口：调用 drd_rfx_encoder_force_keyframe 触发 RFX 编码器内部标记。
 */
void
drd_encoding_manager_force_keyframe(DrdEncodingManager *self)
{
    g_return_if_fail(DRD_IS_ENCODING_MANAGER(self));
    if (self->mode == DRD_ENCODING_MODE_RFX && self->rfx_encoder != NULL)
    {
        drd_rfx_encoder_force_keyframe(self->rfx_encoder);
    }
}
