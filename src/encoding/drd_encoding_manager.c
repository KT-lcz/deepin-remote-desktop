#include "encoding/drd_encoding_manager.h"

#include <gio/gio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <freerdp/codec/color.h>
#include <freerdp/codec/h264.h>
#include <freerdp/codec/progressive.h>
#include <freerdp/codec/rfx.h>
#include <winpr/stream.h>

#include "utils/drd_log.h"

/* SurfaceBits 未实现标志，拒绝切换 */
#define SURFACE_BITS_NOT_IMPLEMENTED

static void drd_vaapi_encoder_release(DrdEncodingManager *self);
static gboolean drd_vaapi_encoder_prepare(DrdEncodingManager *self, GError **error);
static gboolean drd_h264_build_fullframe_metablock(const RECTANGLE_16 *regionRect, RDPGFX_H264_METABLOCK *meta,
                                                   GError **error);
static gboolean drd_vaapi_encode_avc420(DrdEncodingManager *self, const guint8 *data, guint stride,
                                        const RECTANGLE_16 *regionRect, RDPGFX_AVC420_BITMAP_STREAM *avc420,
                                        GByteArray **bitstream_out, GError **error);

struct _DrdEncodingManager
{
    GObject parent_instance;

    guint frame_width;
    guint frame_height;
    gboolean ready;
    gboolean enable_diff;
    guint h264_bitrate;
    guint h264_framerate;
    guint h264_qp;
    gboolean h264_hw_accel;

    AVCodecContext *vaapi_encoder;
    AVBufferRef *vaapi_device;
    AVBufferRef *vaapi_frames;
    AVFrame *vaapi_sw_frame;
    struct SwsContext *vaapi_sws;
    guint vaapi_width;
    guint vaapi_height;

    guint32 codecs;
    H264_CONTEXT *h264;
    RFX_CONTEXT *rfx;
    PROGRESSIVE_CONTEXT *progressive;
    GByteArray *gfx_previous_frame;
    GArray *gfx_tile_hashes;
    GArray *gfx_dirty_rects;
    guint gfx_tiles_x;
    guint gfx_tiles_y;
    guint gfx_diff_width;
    guint gfx_diff_height;
    guint gfx_diff_stride;
    gboolean gfx_force_keyframe;
    guint gfx_progressive_rfx_frames;
    gdouble gfx_large_change_threshold;
    guint gfx_progressive_refresh_interval;
    guint gfx_progressive_refresh_timeout_ms;
    DrdEncodingCodecClass gfx_last_codec;
    gboolean gfx_avc_to_non_avc_transition;
    gint64 gfx_non_avc_switch_timestamp_us;
};

G_DEFINE_TYPE(DrdEncodingManager, drd_encoding_manager, G_TYPE_OBJECT)

/*
 * 功能：释放编码管理器持有的编码器及缓冲区，避免悬挂引用。
 * 逻辑：先调用 drd_encoding_manager_reset 清空运行时状态，再释放 raw_encoder 和 scratch_frame，
 *       最后交给父类 dispose 做剩余清理。
 * 参数：object GObject 指针，期望为 DrdEncodingManager 实例。
 * 外部接口：依赖 GLib 的 g_clear_object 处理引用计数，最终调用父类 GObjectClass::dispose。
 */
static void drd_encoding_manager_dispose(GObject *object)
{
    DrdEncodingManager *self = DRD_ENCODING_MANAGER(object);
    drd_encoding_manager_reset(self);
    g_clear_pointer(&self->h264, h264_context_free);
    g_clear_pointer(&self->rfx, rfx_context_free);
    g_clear_pointer(&self->progressive, progressive_context_free);
    g_clear_pointer(&self->gfx_previous_frame, g_byte_array_unref);
    g_clear_pointer(&self->gfx_tile_hashes, g_array_unref);
    g_clear_pointer(&self->gfx_dirty_rects, g_array_unref);
    G_OBJECT_CLASS(drd_encoding_manager_parent_class)->dispose(object);
}

/*
 * 功能：初始化编码管理器的类回调。
 * 逻辑：注册自定义 dispose 以释放内部 encoder；其他生命周期保持默认。
 * 参数：klass 类结构指针。
 * 外部接口：使用 GLib 类型系统，将 dispose 挂载到 GObjectClass。
 */
static void drd_encoding_manager_class_init(DrdEncodingManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_encoding_manager_dispose;
}

/*
 * 功能：初始化编码管理器的实例字段。
 * 逻辑：设置默认分辨率/编码模式/差分开关，创建暂存帧与编码上下文缓冲。
 * 参数：self 编码管理器实例。
 * 外部接口：调用 drd_encoded_frame_new 生成暂存帧。
 */
static void drd_encoding_manager_init(DrdEncodingManager *self)
{
    self->frame_width = 0;
    self->frame_height = 0;
    self->ready = FALSE;
    self->enable_diff = TRUE;
    self->h264_bitrate = DRD_H264_DEFAULT_BITRATE;
    self->h264_framerate = DRD_H264_DEFAULT_FRAMERATE;
    self->h264_qp = DRD_H264_DEFAULT_QP;
    self->h264_hw_accel = DRD_H264_DEFAULT_HW_ACCEL;
    self->vaapi_encoder = NULL;
    self->vaapi_device = NULL;
    self->vaapi_frames = NULL;
    self->vaapi_sw_frame = NULL;
    self->vaapi_sws = NULL;
    self->vaapi_width = 0;
    self->vaapi_height = 0;
    self->h264 = NULL;
    self->rfx = NULL;
    self->progressive = NULL;
    self->gfx_previous_frame = g_byte_array_new();
    self->gfx_tile_hashes = g_array_new(FALSE, TRUE, sizeof(guint64));
    self->gfx_dirty_rects = g_array_new(FALSE, FALSE, sizeof(RFX_RECT));
    self->gfx_tiles_x = 0;
    self->gfx_tiles_y = 0;
    self->gfx_diff_width = 0;
    self->gfx_diff_height = 0;
    self->gfx_diff_stride = 0;
    self->gfx_force_keyframe = TRUE;
    self->gfx_progressive_rfx_frames = 0;
    self->gfx_large_change_threshold = DRD_GFX_DEFAULT_LARGE_CHANGE_THRESHOLD;
    self->gfx_progressive_refresh_interval = DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_INTERVAL;
    self->gfx_progressive_refresh_timeout_ms = DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_TIMEOUT_MS;
    self->gfx_last_codec = DRD_ENCODING_CODEC_CLASS_UNKNOWN;
    self->gfx_avc_to_non_avc_transition = FALSE;
    self->gfx_non_avc_switch_timestamp_us = 0;
    drd_vaapi_encoder_release(self);
}

/*
 * 功能：释放 VAAPI 编码器相关资源，避免重建或重置时泄漏。
 * 逻辑：依次释放 sws 转换器、软帧、硬件帧池与编码器上下文，同时清零尺寸缓存。
 * 参数：self 编码管理器实例。
 * 外部接口：libswscale 的 sws_freeContext，libavutil 的 av_buffer_unref/av_frame_free，
 *           libavcodec 的 avcodec_free_context。
 */
static void drd_vaapi_encoder_release(DrdEncodingManager *self)
{
    if (self->vaapi_sws != NULL)
    {
        sws_freeContext(self->vaapi_sws);
        self->vaapi_sws = NULL;
    }
    if (self->vaapi_sw_frame != NULL)
    {
        av_frame_free(&self->vaapi_sw_frame);
    }
    if (self->vaapi_frames != NULL)
    {
        av_buffer_unref(&self->vaapi_frames);
    }
    if (self->vaapi_device != NULL)
    {
        av_buffer_unref(&self->vaapi_device);
    }
    if (self->vaapi_encoder != NULL)
    {
        avcodec_free_context(&self->vaapi_encoder);
    }
    self->vaapi_width = 0;
    self->vaapi_height = 0;
}

/*
 * 功能：准备 VAAPI 编码器上下文与 BGRA→NV12 的 swscale 转换器。
 * 逻辑：按当前分辨率初始化 VAAPI 设备、frames 池、编码器上下文和 sws 颜色空间转换，
 *       已准备且尺寸一致时直接复用；失败时释放中间资源并返回错误。
 * 参数：self 编码管理器实例；error GLib 错误返回。
 * 外部接口：libavcodec 的 avcodec_find_encoder_by_name/avcodec_alloc_context3/avcodec_open2，
 *           libavutil 的 av_hwdevice_ctx_create/av_hwframe_ctx_alloc/av_hwframe_ctx_init，
 *           libswscale 的 sws_getContext。
 */
static gboolean drd_vaapi_encoder_prepare(DrdEncodingManager *self, GError **error)
{
    const AVCodec *codec = NULL;
    AVHWFramesContext *frames_ctx = NULL;
    int ret = 0;

    if (self->vaapi_encoder != NULL && self->vaapi_width == self->frame_width &&
        self->vaapi_height == self->frame_height)
    {
        return TRUE;
    }

    drd_vaapi_encoder_release(self);

    codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (codec == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "VAAPI encoder h264_vaapi not available");
        return FALSE;
    }

    ret = av_hwdevice_ctx_create(&self->vaapi_device, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create VAAPI device context");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }

    self->vaapi_frames = av_hwframe_ctx_alloc(self->vaapi_device);
    if (self->vaapi_frames == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate VAAPI frames context");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }
    // TODO 在 sws_scale时，是否可以将服务端的分辨率缩放成客户端的分辨率
    frames_ctx = (AVHWFramesContext *) self->vaapi_frames->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = (int) self->frame_width;
    frames_ctx->height = (int) self->frame_height;
    frames_ctx->initial_pool_size = 4;

    ret = av_hwframe_ctx_init(self->vaapi_frames);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to initialize VAAPI frames context");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }

    self->vaapi_encoder = avcodec_alloc_context3(codec);
    if (self->vaapi_encoder == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate VAAPI encoder context");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }

    self->vaapi_encoder->width = (int) self->frame_width;
    self->vaapi_encoder->height = (int) self->frame_height;
    self->vaapi_encoder->pix_fmt = AV_PIX_FMT_VAAPI;
    self->vaapi_encoder->sw_pix_fmt = AV_PIX_FMT_NV12;
    self->vaapi_encoder->time_base = (AVRational) {1, (int) self->h264_framerate};
    self->vaapi_encoder->framerate = (AVRational) {(int) self->h264_framerate, 1};
    self->vaapi_encoder->bit_rate = (int64_t) self->h264_bitrate;
    self->vaapi_encoder->gop_size = (int) self->h264_framerate;
    self->vaapi_encoder->max_b_frames = 0;
    self->vaapi_encoder->hw_frames_ctx = av_buffer_ref(self->vaapi_frames);
    self->vaapi_encoder->trellis = 2;
    self->vaapi_encoder->qmin = 1;
    self->vaapi_encoder->qmax = 60;
    self->vaapi_encoder->max_qdiff = 5;
    self->vaapi_encoder->rc_max_rate = 5000000;
    self->vaapi_encoder->rc_min_rate = 1000000;
    self->vaapi_encoder->rc_buffer_size = 4000000;
    self->vaapi_encoder->me_cmp = FF_CMP_VSAD;
    self->vaapi_encoder->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
    self->vaapi_encoder->max_b_frames = 0;
    self->vaapi_encoder->level = 41;

    if (self->vaapi_encoder->hw_frames_ctx == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to reference VAAPI frames context");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }

    ret = avcodec_open2(self->vaapi_encoder, codec, NULL);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open VAAPI encoder");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }
    // TODO 用 vaapi 替代 sws_scale完成色彩转换
    self->vaapi_sws = sws_getContext((int) self->frame_width, (int) self->frame_height, AV_PIX_FMT_BGRA,
                                     (int) self->frame_width, (int) self->frame_height, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                     NULL, NULL, NULL);
    if (self->vaapi_sws == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create swscale context");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }

    self->vaapi_sw_frame = av_frame_alloc();
    if (self->vaapi_sw_frame == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate software frame");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }

    self->vaapi_sw_frame->format = AV_PIX_FMT_NV12;
    self->vaapi_sw_frame->width = (int) self->frame_width;
    self->vaapi_sw_frame->height = (int) self->frame_height;

    ret = av_frame_get_buffer(self->vaapi_sw_frame, 32);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate software frame buffer");
        drd_vaapi_encoder_release(self);
        return FALSE;
    }

    self->vaapi_width = self->frame_width;
    self->vaapi_height = self->frame_height;
    return TRUE;
}

/*
 * 功能：构造 AVC420 全帧元数据，供 Rdpgfx H264 元数据发送。
 * 逻辑：填充单区域矩形与量化/质量默认值；分配失败时释放并返回错误。
 * 参数：regionRect 全帧矩形；meta 输出元数据；error GLib 错误。
 * 外部接口：FreeRDP h264 的 free_h264_metablock。
 */
static gboolean drd_h264_build_fullframe_metablock(const RECTANGLE_16 *regionRect, RDPGFX_H264_METABLOCK *meta, GError **error)
{
    WINPR_ASSERT(regionRect != NULL);
    WINPR_ASSERT(meta != NULL);

    memset(meta, 0, sizeof(*meta));
    meta->numRegionRects = 1;
    meta->regionRects = g_malloc0(sizeof(*meta->regionRects));
    meta->quantQualityVals = g_malloc0(sizeof(*meta->quantQualityVals));
    if (meta->regionRects == NULL || meta->quantQualityVals == NULL)
    {
        free_h264_metablock(meta);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate h264 metablock");
        return FALSE;
    }

    meta->regionRects[0] = *regionRect;
    memset(meta->quantQualityVals, 0, sizeof(*meta->quantQualityVals));
    return TRUE;
}

/*
 * 功能：使用 VAAPI 硬件加速编码 BGRA 帧为 AVC420，并填充 Rdpgfx 需要的元数据。
 * 逻辑：通过 swscale 将 BGRA 转 NV12，上传到 VAAPI 硬件帧后编码，收集 H264 packet，
 *       拼接输出到 avc420->data/length，并构造全帧元数据。
 * 参数：self 编码管理器；data 原始 BGRA 像素；stride 行跨度；regionRect 全帧矩形；
 *       avc420 输出结构；bitstream_out 返回缓存；error GLib 错误。
 * 外部接口：libswscale 的 sws_scale，libavcodec 的 avcodec_send_frame/avcodec_receive_packet，
 *           libavutil 的 av_hwframe_get_buffer/av_hwframe_transfer_data。
 */
static gboolean drd_vaapi_encode_avc420(DrdEncodingManager *self, const guint8 *data, guint stride,
                                        const RECTANGLE_16 *regionRect, RDPGFX_AVC420_BITMAP_STREAM *avc420,
                                        GByteArray **bitstream_out, GError **error)
{
    const uint8_t *src_slices[4] = {data, NULL, NULL, NULL};
    int src_strides[4] = {(int) stride, 0, 0, 0};
    GByteArray *bitstream = NULL;
    AVPacket *packet = NULL;
    AVFrame *hw_frame = NULL;
    int ret = 0;

    if (!drd_vaapi_encoder_prepare(self, error))
    {
        return FALSE;
    }

    ret = av_frame_make_writable(self->vaapi_sw_frame);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to make software frame writable");
        return FALSE;
    }

    ret = sws_scale(self->vaapi_sws, src_slices, src_strides, 0, (int) self->frame_height,
                    self->vaapi_sw_frame->data, self->vaapi_sw_frame->linesize);
    if (ret <= 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "sws_scale failed for BGRA to NV12 conversion");
        return FALSE;
    }

    hw_frame = av_frame_alloc();
    if (hw_frame == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate VAAPI frame");
        return FALSE;
    }

    hw_frame->format = AV_PIX_FMT_VAAPI;
    hw_frame->width = (int) self->frame_width;
    hw_frame->height = (int) self->frame_height;

    ret = av_hwframe_get_buffer(self->vaapi_frames, hw_frame, 0);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to get VAAPI frame buffer");
        av_frame_free(&hw_frame);
        return FALSE;
    }

    ret = av_hwframe_transfer_data(hw_frame, self->vaapi_sw_frame, 0);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to upload data to VAAPI frame");
        av_frame_free(&hw_frame);
        return FALSE;
    }

    ret = avcodec_send_frame(self->vaapi_encoder, hw_frame);
    av_frame_free(&hw_frame);
    if (ret < 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to send VAAPI frame to encoder");
        return FALSE;
    }

    bitstream = g_byte_array_new();
    packet = av_packet_alloc();
    if (packet == NULL)
    {
        g_byte_array_unref(bitstream);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate AVPacket");
        return FALSE;
    }

    while ((ret = avcodec_receive_packet(self->vaapi_encoder, packet)) == 0)
    {
        g_byte_array_append(bitstream, packet->data, packet->size);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
    {
        g_byte_array_unref(bitstream);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to receive VAAPI packet");
        return FALSE;
    }

    if (bitstream->len == 0)
    {
        g_byte_array_unref(bitstream);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "no avc420 frame produced by VAAPI");
        return FALSE;
    }

    if (!drd_h264_build_fullframe_metablock(regionRect, &avc420->meta, error))
    {
        g_byte_array_unref(bitstream);
        return FALSE;
    }

    avc420->data = bitstream->data;
    avc420->length = bitstream->len;
    *bitstream_out = bitstream;
    return TRUE;
}

/*
 * 功能：创建新的编码管理器实例。
 * 逻辑：委托 g_object_new 分配并初始化 GObject。
 * 参数：无。
 * 外部接口：使用 GLib 的 g_object_new 完成对象创建。
 */
DrdEncodingManager *drd_encoding_manager_new(void) { return g_object_new(DRD_TYPE_ENCODING_MANAGER, NULL); }

/*
 * 功能：按给定编码参数准备 Raw/RFX 编码器。
 * 逻辑：校验分辨率非零 -> 记录分辨率/模式/差分标记 -> 按模式准备 RAW/RFX 相关上下文；
 *       任一步失败则重置状态并返回错误。
 * 参数：self 管理器；options 编码选项（分辨率、模式、差分开关等）；error 输出错误。
 * 外部接口：GLib g_set_error 报告参数/配置错误；调用内部准备函数初始化编码上下文；
 *           日志使用 DRD_LOG_MESSAGE。
 */
gboolean drd_encoding_manager_prepare(DrdEncodingManager *self, const DrdEncodingOptions *options, GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);

    if (options->width == 0 || options->height == 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Encoder resolution must be non-zero (width=%u height=%u)", options->width, options->height);
        return FALSE;
    }

    const gboolean gfx_enabled = (options->mode == DRD_ENCODING_MODE_RFX || options->mode == DRD_ENCODING_MODE_AUTO || options->mode == DRD_ENCODING_MODE_H264);
    if (!gfx_enabled)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Unknown encoder mode");
        drd_encoding_manager_reset(self);
        return FALSE;
    }
    if (self->ready && (self->frame_width != options->width || self->frame_height != options->height))
    {
        drd_encoding_manager_reset(self);
    }
    self->enable_diff = options->enable_frame_diff;
    self->h264_bitrate = options->h264_bitrate;
    self->h264_framerate = options->h264_framerate;
    self->h264_qp = options->h264_qp;
    self->h264_hw_accel = options->h264_hw_accel;
    self->gfx_force_keyframe = TRUE;
    self->gfx_progressive_rfx_frames = 0;
    self->gfx_large_change_threshold = options->gfx_large_change_threshold;
    self->gfx_progressive_refresh_interval = options->gfx_progressive_refresh_interval;
    self->gfx_progressive_refresh_timeout_ms = options->gfx_progressive_refresh_timeout_ms;
    self->gfx_last_codec = DRD_ENCODING_CODEC_CLASS_UNKNOWN;
    self->gfx_avc_to_non_avc_transition = FALSE;
    self->frame_width = options->width;
    self->frame_height = options->height;
    self->ready = TRUE;

    DRD_LOG_MESSAGE("Encoding manager configured for %ux%u stream (mode=%s diff=%s)", options->width, options->height,
                    drd_encoding_mode_to_string(options->mode), options->enable_frame_diff ? "on" : "off");
    return TRUE;
}

/*
 * 功能：重置编码管理器状态，释放底层编码器状态。
 * 逻辑：若未准备好直接返回；清零分辨率/模式/状态并释放上下文缓存，置 ready 为 FALSE。
 * 参数：self 管理器实例。
 * 外部接口：释放 FreeRDP 上下文与 GLib 缓存，使用 DRD_LOG_MESSAGE 记录。
 */
void drd_encoding_manager_reset(DrdEncodingManager *self)
{
    g_return_if_fail(DRD_IS_ENCODING_MANAGER(self));

    if (!self->ready)
    {
        return;
    }

    DRD_LOG_MESSAGE("Encoding manager reset");
    self->codecs = 0;
    self->frame_width = 0;
    self->frame_height = 0;
    self->enable_diff = TRUE;
    self->ready = FALSE;
    g_clear_pointer(&self->h264, h264_context_free);
    g_clear_pointer(&self->rfx, rfx_context_free);
    g_clear_pointer(&self->progressive, progressive_context_free);
    if (self->gfx_previous_frame != NULL)
    {
        g_byte_array_set_size(self->gfx_previous_frame, 0);
    }
    if (self->gfx_tile_hashes != NULL)
    {
        g_array_set_size(self->gfx_tile_hashes, 0);
    }
    if (self->gfx_dirty_rects != NULL)
    {
        g_array_set_size(self->gfx_dirty_rects, 0);
    }
    self->gfx_tiles_x = 0;
    self->gfx_tiles_y = 0;
    self->gfx_diff_width = 0;
    self->gfx_diff_height = 0;
    self->gfx_diff_stride = 0;
    self->gfx_force_keyframe = TRUE;
    self->gfx_progressive_rfx_frames = 0;
    self->gfx_large_change_threshold = DRD_GFX_DEFAULT_LARGE_CHANGE_THRESHOLD;
    self->gfx_progressive_refresh_interval = DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_INTERVAL;
    self->gfx_progressive_refresh_timeout_ms = DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_TIMEOUT_MS;
    self->gfx_last_codec = DRD_ENCODING_CODEC_CLASS_UNKNOWN;
    self->gfx_avc_to_non_avc_transition = FALSE;
    self->gfx_non_avc_switch_timestamp_us = 0;
}

gboolean drd_encoding_manager_has_avc_to_non_avc_transition( DrdEncodingManager *self)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);

    return self->gfx_avc_to_non_avc_transition;
}

guint drd_encoding_manager_get_refresh_timeout_ms( DrdEncodingManager *self)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), 0);

    return self->gfx_progressive_refresh_timeout_ms;
}

gboolean drd_encoding_manager_refresh_interval_reached( DrdEncodingManager *self)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);

    if (!self->gfx_avc_to_non_avc_transition)
    {
        return FALSE;
    }

    if (self->gfx_non_avc_switch_timestamp_us <= 0)
    {
        return FALSE;
    }

    const gboolean frame_budget_reached = self->gfx_progressive_refresh_interval > 0 &&
                                          self->gfx_progressive_rfx_frames + 1 >=
                                              self->gfx_progressive_refresh_interval;
    const gint64 now_us = g_get_monotonic_time();
    const gint64 elapsed_us = now_us - self->gfx_non_avc_switch_timestamp_us;
    const gboolean timeout_reached = self->gfx_progressive_refresh_timeout_ms > 0 &&
                                     elapsed_us >=
                                         ((gint64) self->gfx_progressive_refresh_timeout_ms) * G_TIME_SPAN_MILLISECOND;

    return frame_budget_reached || timeout_reached;
}

/*
 * 功能：在无新捕获帧时复用上一帧并强制输出 Surface GFX 关键帧。
 * 逻辑：校验缓存帧与差分状态可用，构造临时 DrdFrame 承载上一帧像素，置位关键帧标志后复用 Surface GFX
 *       编码路径发送全量帧。
 * 参数：self 管理器；settings 客户端编码设置；context Rdpgfx 上下文；surface_id 目标 surface；frame_id 帧序号；h264 输出是否
 *       使用 H264；auto_switch 自动切换编码策略；error 错误输出。
 * 外部接口：GLib g_get_monotonic_time/g_set_error；调用 drd_frame_new/drd_frame_configure/drd_frame_ensure_capacity 以及
 *           drd_encoding_manager_encode_surface_gfx 复用现有编码逻辑。
 */
gboolean
drd_encoding_manager_encode_cached_frame_gfx(DrdEncodingManager *self,
                                             rdpSettings *settings,
                                             RdpgfxServerContext *context,
                                             guint16 surface_id,
                                             guint32 frame_id,
                                             gboolean *h264,
                                             gboolean auto_switch,
                                             GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);
    g_return_val_if_fail(settings != NULL, FALSE);
    g_return_val_if_fail(context != NULL, FALSE);

    if (!self->ready)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Encoding manager not prepared");
        return FALSE;
    }

    if (self->gfx_previous_frame->len == 0 || self->gfx_diff_width == 0 || self->gfx_diff_height == 0 ||
        self->gfx_diff_stride == 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "No cached frame available for refresh");
        return FALSE;
    }

    g_autoptr(DrdFrame) cached_frame = drd_frame_new();
    drd_frame_configure(cached_frame,
                        self->gfx_diff_width,
                        self->gfx_diff_height,
                        self->gfx_diff_stride,
                        (guint64) g_get_monotonic_time());

    guint8 *buffer = drd_frame_ensure_capacity(cached_frame, self->gfx_previous_frame->len);
    if (buffer == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate cached frame buffer");
        return FALSE;
    }
    memcpy(buffer, self->gfx_previous_frame->data, self->gfx_previous_frame->len);

    self->gfx_force_keyframe = TRUE;
    DRD_LOG_MESSAGE("encode cached frame");
    return drd_encoding_manager_encode_surface_gfx(
            self, settings, context, surface_id, cached_frame, frame_id, h264, auto_switch, error);
}

void drd_encoding_manager_register_codec_result(DrdEncodingManager *self,
                                                DrdEncodingCodecClass codec_class,
                                                gboolean keyframe_encode)
{
    g_return_if_fail(DRD_IS_ENCODING_MANAGER(self));

    switch (codec_class)
    {
        case DRD_ENCODING_CODEC_CLASS_AVC:
            self->gfx_last_codec = DRD_ENCODING_CODEC_CLASS_AVC;
            self->gfx_avc_to_non_avc_transition = FALSE;
            self->gfx_progressive_rfx_frames = 0;
            self->gfx_non_avc_switch_timestamp_us = 0;
            return;
        case DRD_ENCODING_CODEC_CLASS_NON_AVC:
            break;
        case DRD_ENCODING_CODEC_CLASS_UNKNOWN:
        default:
            self->gfx_last_codec = DRD_ENCODING_CODEC_CLASS_UNKNOWN;
            self->gfx_progressive_rfx_frames = 0;
            self->gfx_avc_to_non_avc_transition = FALSE;
            self->gfx_non_avc_switch_timestamp_us = 0;
            return;
    }

    const gint64 now_us = g_get_monotonic_time();
    const gboolean entering_avc_switch = (self->gfx_last_codec == DRD_ENCODING_CODEC_CLASS_AVC);

    if (entering_avc_switch)
    {
        self->gfx_avc_to_non_avc_transition = TRUE;
        self->gfx_non_avc_switch_timestamp_us = now_us;
        self->gfx_progressive_rfx_frames = 0;
    }

    const gboolean refresh_tracking = self->gfx_avc_to_non_avc_transition;

    if (!refresh_tracking)
    {
        self->gfx_progressive_rfx_frames = 0;
    }
    else if (keyframe_encode || self->gfx_progressive_refresh_interval == 0)
    {
        self->gfx_progressive_rfx_frames = 0;
    }
    else
    {
        self->gfx_progressive_rfx_frames++;
    }

    if (keyframe_encode)
    {
        self->gfx_non_avc_switch_timestamp_us = refresh_tracking ? now_us : 0;
        self->gfx_avc_to_non_avc_transition = refresh_tracking ? FALSE : self->gfx_avc_to_non_avc_transition;
    }

    self->gfx_last_codec = codec_class;
}


// copy from freerdp shadow
// 可能开启硬件加速时，无需初始化 freerdp 的 h264，使用了 ffmpeg 的 h264 编码器
static int drd_encoder_init_h264(DrdEncodingManager *encoder)
{
    if (!encoder->h264)
        encoder->h264 = h264_context_new(TRUE);

    if (!encoder->h264)
        goto fail;

    if (!h264_context_reset(encoder->h264, encoder->frame_width, encoder->frame_height))
        goto fail;

    if (!h264_context_set_option(encoder->h264, H264_CONTEXT_OPTION_RATECONTROL, H264_RATECONTROL_VBR))
        goto fail;
    if (!h264_context_set_option(encoder->h264, H264_CONTEXT_OPTION_BITRATE, encoder->h264_bitrate))
        goto fail;
    if (!h264_context_set_option(encoder->h264, H264_CONTEXT_OPTION_FRAMERATE, encoder->h264_framerate))
        goto fail;
    if (!h264_context_set_option(encoder->h264, H264_CONTEXT_OPTION_QP, encoder->h264_qp))
        goto fail;
    // 不能使用 freerdp 的 HW_ACCEL 配置开启硬件加速，会导致 freerdp 崩溃
    // if (!h264_context_set_option(encoder->h264, H264_CONTEXT_OPTION_HW_ACCEL, encoder->h264_hw_accel))
    //     goto fail;
    // guint hw = h264_context_get_option(encoder->h264, H264_CONTEXT_OPTION_HW_ACCEL);
    // DRD_LOG_MESSAGE("hw is %d",hw);
    encoder->codecs |= FREERDP_CODEC_AVC420 | FREERDP_CODEC_AVC444;
    return 1;
fail:
    g_clear_pointer(&encoder->h264, h264_context_free);
    return -1;
}


static int drd_encoder_init_rfx(DrdEncodingManager *encoder, const rdpSettings *settings)
{
    if (!encoder->rfx)
        encoder->rfx = rfx_context_new_ex(TRUE, freerdp_settings_get_uint32(settings, FreeRDP_ThreadingFlags));
    if (!encoder->rfx)
        goto fail;

    if (!rfx_context_reset(encoder->rfx, encoder->frame_width, encoder->frame_height))
        goto fail;

    rfx_context_set_mode(encoder->rfx, freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxRlgrMode));
    rfx_context_set_pixel_format(encoder->rfx, PIXEL_FORMAT_BGRX32);
    encoder->codecs |= FREERDP_CODEC_REMOTEFX;
    return 1;
fail:
    g_clear_pointer(&encoder->rfx, rfx_context_free);
    return -1;
}

static int drd_encoder_init_progressive(DrdEncodingManager *encoder)
{
    WINPR_ASSERT(encoder);
    if (!encoder->progressive)
        encoder->progressive = progressive_context_new(TRUE);

    if (!encoder->progressive)
        goto fail;

    if (!progressive_context_reset(encoder->progressive))
        goto fail;

    encoder->codecs |= FREERDP_CODEC_PROGRESSIVE;
    return 1;
fail:
    g_clear_pointer(&encoder->progressive, progressive_context_free);
    return -1;
}


gboolean drd_encoder_prepare(DrdEncodingManager *encoder, guint32 codecs, rdpSettings *settings)
{
    int status = 0;

    if ((codecs & FREERDP_CODEC_REMOTEFX) && !(encoder->codecs & FREERDP_CODEC_REMOTEFX))
    {
        WLog_DBG(TAG, "initializing RemoteFX encoder");
        status = drd_encoder_init_rfx(encoder, settings);

        if (status < 0)
            return FALSE;
    }

    if ((codecs & (FREERDP_CODEC_AVC420 | FREERDP_CODEC_AVC444)) &&
        !(encoder->codecs & (FREERDP_CODEC_AVC420 | FREERDP_CODEC_AVC444)))
    {
        WLog_DBG(TAG, "initializing H.264 encoder");
        status = drd_encoder_init_h264(encoder);

        if (status < 0)
            return FALSE;
    }

    if ((codecs & FREERDP_CODEC_PROGRESSIVE) && !(encoder->codecs & FREERDP_CODEC_PROGRESSIVE))
    {
        WLog_DBG(TAG, "initializing progressive encoder");
        status = drd_encoder_init_progressive(encoder);

        if (status < 0)
            return FALSE;
    }

    return TRUE;
}

static INLINE UINT32 rdpgfx_estimate_h264_avc420(RDPGFX_AVC420_BITMAP_STREAM *havc420)
{
    /* H264 metadata + H264 stream. See rdpgfx_write_h264_avc420 */
    WINPR_ASSERT(havc420);
    return sizeof(UINT32) /* numRegionRects */
           + 10ULL /* regionRects + quantQualityVals */
                     * havc420->meta.numRegionRects +
           havc420->length;
}

/*
 * 功能：对 64 位整数执行左循环位移，作为 tile hash 的扰动基础。
 * 逻辑：将值左移指定位数并与右移互补位或运算。
 * 参数：value 原始数；shift 左移位数（0-63）。
 * 外部接口：无。
 */
static inline guint64 drd_gfx_rotl64(guint64 value, guint shift) { return (value << shift) | (value >> (64 - shift)); }

/*
 * 功能：将 64 位块混入 hash，降低 tile 哈希碰撞概率。
 * 逻辑：执行 xor/乘法/移位混合，随后回滚并再扰动。
 * 参数：hash 当前 hash 值；chunk 待混入数据。
 * 外部接口：无。
 */
static inline guint64 drd_gfx_mix_chunk(guint64 hash, guint64 chunk)
{
    chunk ^= chunk >> 30;
    chunk *= G_GUINT64_CONSTANT(0xbf58476d1ce4e5b9);
    chunk ^= chunk >> 27;
    chunk *= G_GUINT64_CONSTANT(0x94d049bb133111eb);
    chunk ^= chunk >> 31;

    hash ^= chunk;
    hash = drd_gfx_rotl64(hash, 29);
    hash *= G_GUINT64_CONSTANT(0x9e3779b185ebca87);
    return hash;
}

/*
 * 功能：计算指定 tile 的哈希，用于 surface gfx 的差分检测。
 * 逻辑：按行遍历 tile，将 16/8 字节块混入 hash，尾部按长度补齐。
 * 参数：data 帧缓冲；stride 行步长；x/y 左上角；width/height tile 尺寸。
 * 外部接口：C 标准库 memcpy。
 */
static guint64 drd_gfx_hash_tile(const guint8 *data, guint stride, guint32 x, guint32 y, guint32 width, guint32 height)
{
    guint64 hash = G_GUINT64_CONSTANT(0xcbf29ce484222325);
    const guint32 bytes_per_row = width * 4u;

    for (guint row = 0; row < height; ++row)
    {
        const guint8 *ptr = data + ((gsize) (y + row) * stride) + (gsize) x * 4;
        guint32 remaining = bytes_per_row;

        while (remaining >= 16)
        {
            guint64 lo, hi;
            memcpy(&lo, ptr, sizeof(lo));
            memcpy(&hi, ptr + 8, sizeof(hi));
            hash = drd_gfx_mix_chunk(hash, lo);
            hash = drd_gfx_mix_chunk(hash, hi);
            ptr += 16;
            remaining -= 16;
        }

        while (remaining >= 8)
        {
            guint64 chunk;
            memcpy(&chunk, ptr, sizeof(chunk));
            hash = drd_gfx_mix_chunk(hash, chunk);
            ptr += 8;
            remaining -= 8;
        }

        if (remaining > 0)
        {
            guint64 tail = 0;
            memcpy(&tail, ptr, remaining);
            tail ^= ((guint64) remaining << 56);
            hash = drd_gfx_mix_chunk(hash, tail);
        }
    }

    return hash;
}

/*
 * 功能：根据帧尺寸与 stride 初始化 surface gfx 差分状态。
 * 逻辑：尺寸变化时重建 tile 哈希与 previous buffer，并强制关键帧。
 * 参数：self 管理器；width/height/stride 当前帧几何。
 * 外部接口：GLib g_byte_array_set_size/g_array_set_size。
 */
static void drd_encoding_manager_prepare_gfx_diff_state(DrdEncodingManager *self, guint width, guint height,
                                                        guint stride)
{
    const gboolean size_changed =
            self->gfx_diff_width != width || self->gfx_diff_height != height || self->gfx_diff_stride != stride;
    const guint tiles_x = (width + 63) / 64;
    const guint tiles_y = (height + 63) / 64;
    const gboolean tiles_changed = self->gfx_tiles_x != tiles_x || self->gfx_tiles_y != tiles_y;

    if (!size_changed && !tiles_changed && self->gfx_previous_frame->len == (gsize) stride * height)
    {
        return;
    }

    self->gfx_diff_width = width;
    self->gfx_diff_height = height;
    self->gfx_diff_stride = stride;
    self->gfx_tiles_x = tiles_x;
    self->gfx_tiles_y = tiles_y;
    g_byte_array_set_size(self->gfx_previous_frame, (gsize) stride * height);
    memset(self->gfx_previous_frame->data, 0, self->gfx_previous_frame->len);
    g_array_set_size(self->gfx_tile_hashes, self->gfx_tiles_x * self->gfx_tiles_y);
    memset(self->gfx_tile_hashes->data, 0, self->gfx_tile_hashes->len * sizeof(guint64));
    self->gfx_force_keyframe = TRUE;
    self->gfx_progressive_rfx_frames = 0;
}

static void drd_encoding_manager_store_previous_frame(DrdEncodingManager *self, const guint8 *data, guint stride,
                                                       guint height)
{
    if (self->gfx_previous_frame->len != (gsize) stride * height)
    {
        g_byte_array_set_size(self->gfx_previous_frame, (gsize) stride * height);
    }
    memcpy(self->gfx_previous_frame->data, data, self->gfx_previous_frame->len);
}

/*
 * 功能：按当前帧预计算 tile hash，便于后续差分复用。
 * 逻辑：遍历 64x64 tile 计算 hash 并写回缓存。
 * 参数：self 管理器；data 当前帧；stride 行步长。
 * 外部接口：无。
 */
static void drd_encoding_manager_update_tile_hashes(DrdEncodingManager *self, const guint8 *data, guint stride)
{
    if (self->gfx_tiles_x == 0 || self->gfx_tiles_y == 0)
    {
        return;
    }

    for (guint y = 0; y < self->gfx_diff_height; y += 64)
    {
        const guint tile_h = MIN(64u, self->gfx_diff_height - y);
        for (guint x = 0; x < self->gfx_diff_width; x += 64)
        {
            const guint tile_w = MIN(64u, self->gfx_diff_width - x);
            const guint index = (y / 64) * self->gfx_tiles_x + (x / 64);
            guint64 hash = drd_gfx_hash_tile(data, stride, x, y, tile_w, tile_h);
            guint64 *stored = &g_array_index(self->gfx_tile_hashes, guint64, index);
            *stored = hash;
        }
    }
}

/*
 * 功能：基于预计算的脏块标记生成 REGION16 或 RFX_RECT，供 Progressive/RemoteFX 共用。
 * 逻辑：按 64x64 tile 读取 dirty_flags，命中时写入矩形或合并 REGION16，避免重复像素比对。
 * 参数：self 管理器；dirty_flags 脏块标记；region/rects 输出容器。
 * 外部接口：WinPR region16_union_rect；GLib g_array_append_val。
 */
static gboolean drd_encoding_manager_collect_dirty_tiles(DrdEncodingManager *self, const GArray *dirty_flags,
                                                         REGION16 *region, GArray *rects)
{
    if (self->gfx_tiles_x == 0 || self->gfx_tiles_y == 0)
    {
        return FALSE;
    }

    const guint total_tiles = self->gfx_tiles_x * self->gfx_tiles_y;
    WINPR_ASSERT(dirty_flags != NULL);
    WINPR_ASSERT(dirty_flags->len == total_tiles);

    gboolean has_dirty = FALSE;

    for (guint y = 0; y < self->gfx_diff_height; y += 64)
    {
        const guint tile_h = MIN(64u, self->gfx_diff_height - y);
        for (guint x = 0; x < self->gfx_diff_width; x += 64)
        {
            const guint tile_w = MIN(64u, self->gfx_diff_width - x);
            const guint index = (y / 64) * self->gfx_tiles_x + (x / 64);
            const gboolean different = g_array_index(dirty_flags, gboolean, index);

            if (different)
            {
                if (rects != NULL)
                {
                    RFX_RECT rect = {(UINT16) x, (UINT16) y, (UINT16) tile_w, (UINT16) tile_h};
                    g_array_append_val(rects, rect);
                }
                if (region != NULL)
                {
                    const guint32 right = x + tile_w;
                    const guint32 bottom = y + tile_h;
                    RECTANGLE_16 region_rect;
                    WINPR_ASSERT(right <= UINT16_MAX);
                    WINPR_ASSERT(bottom <= UINT16_MAX);
                    region_rect.left = (UINT16) x;
                    region_rect.top = (UINT16) y;
                    region_rect.right = (UINT16) right;
                    region_rect.bottom = (UINT16) bottom;
                    region16_union_rect(region, region, &region_rect);
                }
                has_dirty = TRUE;
            }
        }
    }

    return has_dirty;
}

static gboolean drd_encoding_manager_collect_dirty_rects(DrdEncodingManager *self, const GArray *dirty_flags,
                                                         GArray *rects)
{
    return drd_encoding_manager_collect_dirty_tiles(self, dirty_flags, NULL, rects);
}

static gboolean drd_encoding_manager_collect_dirty_region(DrdEncodingManager *self, const GArray *dirty_flags,
                                                          REGION16 *region)
{
    return drd_encoding_manager_collect_dirty_tiles(self, dirty_flags, region, NULL);
}

/*
 * 功能：单次遍历 tile 获取脏块分布并判定是否为大变化。
 * 逻辑：按 64x64 tile 计算 hash，对比历史 hash 后在差异 tile 上执行 memcmp，累计变化比例并写入脏块标记。
 * 参数：self 管理器；data 当前帧；previous 上一帧；stride 行步长；threshold 判定阈值；dirty_flags 脏块标记数组；changed_tiles 输出变化 tile 数。
 * 外部接口：C 标准库 memcmp。
 */
static gboolean drd_encoding_manager_analyze_tiles(DrdEncodingManager *self, const guint8 *data, const guint8 *previous,
                                                   guint stride, gdouble threshold, GArray *dirty_flags,
                                                   guint *changed_tiles)
{
    if (self->gfx_tiles_x == 0 || self->gfx_tiles_y == 0 || self->gfx_diff_width == 0 || self->gfx_diff_height == 0)
    {
        if (changed_tiles != NULL)
        {
            *changed_tiles = 0;
        }
        return TRUE;
    }

    const guint total_tiles = self->gfx_tiles_x * self->gfx_tiles_y;
    if (total_tiles == 0)
    {
        if (changed_tiles != NULL)
        {
            *changed_tiles = 0;
        }
        return TRUE;
    }

    if (dirty_flags != NULL)
    {
        g_array_set_size(dirty_flags, total_tiles);
        memset(dirty_flags->data, 0, dirty_flags->len * sizeof(gboolean));
    }

    guint local_changed_tiles = 0;
    const gboolean force_dirty = previous == NULL;

    for (guint y = 0; y < self->gfx_diff_height; y += 64)
    {
        const guint tile_h = MIN(64u, self->gfx_diff_height - y);
        for (guint x = 0; x < self->gfx_diff_width; x += 64)
        {
            const guint tile_w = MIN(64u, self->gfx_diff_width - x);
            const guint index = (y / 64) * self->gfx_tiles_x + (x / 64);
            const guint64 hash = drd_gfx_hash_tile(data, stride, x, y, tile_w, tile_h);
            const guint64 stored = g_array_index(self->gfx_tile_hashes, guint64, index);
            gboolean different = force_dirty || stored != hash;

            if (different && !force_dirty)
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

            if (dirty_flags != NULL)
            {
                gboolean *flag = &g_array_index(dirty_flags, gboolean, index);
                *flag = different;
            }

            if (different)
            {
                local_changed_tiles++;
            }
        }
    }

    if (changed_tiles != NULL)
    {
        *changed_tiles = local_changed_tiles;
    }

    return ((gdouble) local_changed_tiles / (gdouble) total_tiles) >= threshold;
}

/*
 * 功能：生成符合 Rdpgfx 要求的 32 位时间戳。
 * 逻辑：获取本地时间，按小时/分钟/秒/毫秒编码到 32 位整数。
 * 参数：无。
 * 外部接口：GLib GDateTime API 获取时间。
 */
static guint32 drd_rdp_graphics_pipeline_build_timestamp(void)
{
    guint32 timestamp = 0;
    GDateTime *now = g_date_time_new_now_local();

    if (now != NULL)
    {
        timestamp = ((guint32) g_date_time_get_hour(now) << 22) | ((guint32) g_date_time_get_minute(now) << 16) |
                    ((guint32) g_date_time_get_second(now) << 10) |
                    ((guint32) (g_date_time_get_microsecond(now) / 1000));
        g_date_time_unref(now);
    }

    return timestamp;
}


gboolean drd_encoding_manager_encode_surface_gfx(DrdEncodingManager *self, rdpSettings *settings,
                                                 RdpgfxServerContext *context, guint16 surface_id, DrdFrame *input,
                                                 guint32 frame_id, gboolean *h264, gboolean auto_switch,
                                                 GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);
    g_return_val_if_fail(settings != NULL, FALSE);
    g_return_val_if_fail(context != NULL, FALSE);
    g_return_val_if_fail(DRD_IS_FRAME(input), FALSE);

    if (!self->ready)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Encoding manager not prepared");
        return FALSE;
    }

    const gboolean gfx_avc420 = freerdp_settings_get_bool(settings, FreeRDP_GfxH264);
    const gboolean gfx_avc444 = freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444);
    const gboolean gfx_avc444v2 = freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2);
    const gboolean gfx_remotefx = freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec);
    const gboolean gfx_progressive = freerdp_settings_get_bool(settings, FreeRDP_GfxProgressive);
    const guint32 id = freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId);

    // DRD_LOG_MESSAGE("avc420 %d;avc444 %d;avc444v2 %d;remotefx %d;progressive %d",gfx_avc420,gfx_avc444,gfx_avc444v2,gfx_remotefx,gfx_remotefx);
    RDPGFX_SURFACE_COMMAND cmd;
    RDPGFX_START_FRAME_PDU cmd_start;
    RDPGFX_END_FRAME_PDU cmd_end;


    self->frame_width = drd_frame_get_width(input);
    self->frame_height = drd_frame_get_height(input);


    const guint stride = drd_frame_get_stride(input);
    gsize data_size = 0;
    const guint8 *data = drd_frame_get_data(input, &data_size);
    *h264 = FALSE;

    drd_encoding_manager_prepare_gfx_diff_state(self, self->frame_width, self->frame_height, stride);
    const guint8 *previous_frame =
            (self->gfx_previous_frame->len == (gsize) stride * self->frame_height) ? self->gfx_previous_frame->data : NULL;
    gboolean success = FALSE;
    GArray *dirty_flags = g_array_sized_new(FALSE, TRUE, sizeof(gboolean), self->gfx_tiles_x * self->gfx_tiles_y);
    const gboolean large_change = drd_encoding_manager_analyze_tiles(
            self, data, previous_frame, stride, self->gfx_large_change_threshold, dirty_flags, NULL);
    gboolean use_avc444 = FALSE;
    gboolean use_avc420 = FALSE;
    gboolean use_progressive = FALSE;
    gboolean use_remotefx = FALSE;
    gboolean force_avc420 = FALSE;

    if (auto_switch && self->h264_hw_accel && gfx_avc420)
    {
        g_autoptr(GError) vaapi_error = NULL;

        if (drd_vaapi_encoder_prepare(self, &vaapi_error))
        {
            force_avc420 = TRUE;
        }
        else
        {
            g_clear_error(&vaapi_error);
        }
    }

    if (force_avc420)
    {
        use_avc420 = TRUE;
    }
    else if (auto_switch)
    {
        if (large_change)
        {
            if (gfx_avc444v2 || gfx_avc444)
            {
                use_avc444 = TRUE;
            }
            else if (gfx_avc420)
            {
                use_avc420 = TRUE;
            }
            else if (gfx_progressive)
            {
                use_progressive = TRUE;
            }
            else if (gfx_remotefx && id != 0)
            {
                use_remotefx = TRUE;
            }
        }
        else
        {
            if (gfx_progressive)
            {
                use_progressive = TRUE;
            }
            else if (gfx_remotefx && id != 0)
            {
                use_remotefx = TRUE;
            }
            else if (gfx_avc444v2 || gfx_avc444)
            {
                use_avc444 = TRUE;
            }
            else if (gfx_avc420)
            {
                use_avc420 = TRUE;
            }
        }
    }
    else if (gfx_avc444v2 || gfx_avc444 || gfx_avc420)
    {
        use_avc444 = gfx_avc444v2 || gfx_avc444;
        use_avc420 = !use_avc444 && gfx_avc420;
        if (!use_avc444 && !use_avc420)
        {
            use_progressive = gfx_progressive;
            use_remotefx = gfx_remotefx && id != 0;
        }
    }
    else
    {
        use_progressive = gfx_progressive;
        use_remotefx = gfx_remotefx && id != 0;
    }

    cmd_start.frameId = frame_id;
    cmd_start.timestamp = drd_rdp_graphics_pipeline_build_timestamp();
    cmd_end.frameId = cmd_start.frameId;
    cmd.surfaceId = surface_id;
    cmd.format = PIXEL_FORMAT_BGRX32;
    cmd.left = 0;
    cmd.top = 0;
    cmd.right = cmd.left + self->frame_width;
    cmd.bottom = cmd.top + self->frame_height;
    cmd.width = self->frame_width;
    cmd.height = self->frame_height;
    gint if_error = CHANNEL_RC_OK;

    if (use_avc444)
    {
        DRD_LOG_MESSAGE("avc444 encode");
        // avc444 encode
        gint32 rc = 0;
        RDPGFX_AVC444_BITMAP_STREAM avc444 = {0};
        RECTANGLE_16 regionRect = {0};
        BYTE version = gfx_avc444v2 ? 2 : 1;
        *h264 = TRUE;
        WINPR_ASSERT(cmd.left <= UINT16_MAX);
        WINPR_ASSERT(cmd.top <= UINT16_MAX);
        WINPR_ASSERT(cmd.right <= UINT16_MAX);
        WINPR_ASSERT(cmd.bottom <= UINT16_MAX);
        regionRect.left = (UINT16)cmd.left;
        regionRect.top = (UINT16)cmd.top;
        regionRect.right = (UINT16)cmd.right;
        regionRect.bottom = (UINT16)cmd.bottom;

        if (!drd_encoder_prepare(self, FREERDP_CODEC_AVC444, settings))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "failed to prepare encoder FREERDP_CODEC_AVC444");
            goto out;
        }
        rc = avc444_compress(self->h264, data, cmd.format, stride, self->frame_width, self->frame_height, version, &regionRect,
                             &avc444.LC, &avc444.bitstream[0].data, &avc444.bitstream[0].length,
                             &avc444.bitstream[1].data, &avc444.bitstream[1].length, &avc444.bitstream[0].meta,
                             &avc444.bitstream[1].meta);
        if (rc < 0)
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "avc444_compress failed");
            goto out;
        }
        if (rc == 0)
        {
            free_h264_metablock(&avc444.bitstream[0].meta);
            free_h264_metablock(&avc444.bitstream[1].meta);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "no avc444 frame produced");
            goto out;
        }
        if (rc > 0)
        {
            avc444.cbAvc420EncodedBitstream1 = rdpgfx_estimate_h264_avc420(&avc444.bitstream[0]);
            cmd.codecId = gfx_avc444v2 ? RDPGFX_CODECID_AVC444v2 : RDPGFX_CODECID_AVC444;
            cmd.extra = (void *) &avc444;
            IFCALLRET(context->SurfaceFrameCommand, if_error, context, &cmd, &cmd_start, &cmd_end);
        }
        free_h264_metablock(&avc444.bitstream[0].meta);
        free_h264_metablock(&avc444.bitstream[1].meta);
        if (if_error)
        {
            g_autofree gchar *err_msg = g_strdup_printf("SurfaceFrameCommand failed with error %" PRIu32 "", if_error);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, err_msg);
            goto out;
        }
        if (rc > 0)
        {
            drd_encoding_manager_store_previous_frame(self, data, stride, self->frame_height);
            drd_encoding_manager_update_tile_hashes(self, data, stride);
            drd_encoding_manager_register_codec_result(self, DRD_ENCODING_CODEC_CLASS_AVC, TRUE);
        }
    }
    else if (use_avc420)
    {
        INT32 rc = 0;
        RDPGFX_AVC420_BITMAP_STREAM avc420 = {0};
        RECTANGLE_16 regionRect;
        g_autoptr(GByteArray) vaapi_bitstream = NULL;
        gboolean use_vaapi = FALSE;
        *h264 = TRUE;
        if (!drd_encoder_prepare(self, FREERDP_CODEC_AVC420, settings))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to prepare encoder FREERDP_CODEC_AVC420");
            goto out;
        }
        regionRect.left = (UINT16) cmd.left;
        regionRect.top = (UINT16) cmd.top;
        regionRect.right = (UINT16) cmd.right;
        regionRect.bottom = (UINT16) cmd.bottom;

        if (self->h264_hw_accel)
        {
            if (drd_vaapi_encode_avc420(self, data, stride, &regionRect, &avc420, &vaapi_bitstream, error))
            {
                DRD_LOG_MESSAGE("VAAPI avc420 encode");
                rc = 1;
                use_vaapi = TRUE;
            }
            else
            {
                if (error != NULL && *error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_PENDING))
                {
                    goto out;
                }
                g_clear_error(error);
                g_warning("VAAPI avc420 encode failed, fallback to software");
            }
        }

        if (!use_vaapi)
        {
            rc = avc420_compress(self->h264, data, cmd.format, stride, self->frame_width, self->frame_height, &regionRect,
                                 &avc420.data, &avc420.length, &avc420.meta);
            if (rc < 0)
            {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "avc420_compress failed");
                goto out;
            }
            if (rc == 0)
            {
                free_h264_metablock(&avc420.meta);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "no avc420 frame produced");
                goto out;
            }
        }
        /* rc > 0 means new data */
        if (rc > 0)
        {
            cmd.codecId = RDPGFX_CODECID_AVC420;
            cmd.extra = (void *) &avc420;

            IFCALLRET(context->SurfaceFrameCommand, if_error, context, &cmd, &cmd_start, &cmd_end);
        }
        free_h264_metablock(&avc420.meta);

        if (if_error)
        {
            g_autofree gchar *err_msg = g_strdup_printf("SurfaceFrameCommand failed with error %" PRIu32 "", if_error);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, err_msg);
            goto out;
        }
        if (rc > 0)
        {
            drd_encoding_manager_store_previous_frame(self, data, stride, self->frame_height);
            drd_encoding_manager_update_tile_hashes(self, data, stride);
            drd_encoding_manager_register_codec_result(self, DRD_ENCODING_CODEC_CLASS_AVC, TRUE);
        }

        if (use_vaapi)
        {
            g_clear_pointer(&vaapi_bitstream, g_byte_array_unref);
        }
    }
    else if (use_progressive)
    {
        DRD_LOG_MESSAGE("progressive encode");
        INT32 rc = 0;
        REGION16 region;
        RECTANGLE_16 regionRect;
        if (!drd_encoder_prepare(self, FREERDP_CODEC_PROGRESSIVE, settings))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "failed to prepare encoder FREERDP_CODEC_PROGRESSIVE");
            goto out;
        }

        WINPR_ASSERT(cmd.left <= UINT16_MAX);
        WINPR_ASSERT(cmd.top <= UINT16_MAX);
        WINPR_ASSERT(cmd.right <= UINT16_MAX);
        WINPR_ASSERT(cmd.bottom <= UINT16_MAX);
        WINPR_ASSERT(self->frame_width <= UINT16_MAX);
        WINPR_ASSERT(self->frame_height <= UINT16_MAX);
        const gboolean refresh_interval_reached = drd_encoding_manager_refresh_interval_reached(self);
        const gboolean keyframe_encode = self->gfx_force_keyframe || !self->enable_diff || refresh_interval_reached;

        region16_init(&region);
        if (keyframe_encode)
        {
            DRD_LOG_MESSAGE("frame key refresh");
            memset(self->gfx_tile_hashes->data, 0, self->gfx_tile_hashes->len * sizeof(guint64));
            regionRect.left = (UINT16) cmd.left;
            regionRect.top = (UINT16) cmd.top;
            regionRect.right = (UINT16) cmd.right;
            regionRect.bottom = (UINT16) cmd.bottom;
            region16_union_rect(&region, &region, &regionRect);
        }
        else if (!drd_encoding_manager_collect_dirty_region(self, dirty_flags, &region))
        {
            region16_uninit(&region);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "not exist dirty region");
            goto out;
        }
        rc = progressive_compress(self->progressive, data, stride * self->frame_height, cmd.format, self->frame_width, self->frame_height, stride, &region,
                                  &cmd.data, &cmd.length);
        region16_uninit(&region);
        if (rc < 0)
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "progressive_compress failed");
            goto out;
        }

        /* rc > 0 means new data */
        if (rc > 0)
        {
            cmd.codecId = RDPGFX_CODECID_CAPROGRESSIVE;

            IFCALLRET(context->SurfaceFrameCommand, if_error, context, &cmd, &cmd_start, &cmd_end);
        }

        if (if_error)
        {
            g_autofree gchar *err_msg = g_strdup_printf("SurfaceFrameCommand failed with error %" PRIu32 "", if_error);
            self->gfx_force_keyframe = TRUE;
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, err_msg);
            goto out;
        }
        if (rc > 0)
        {
            drd_encoding_manager_store_previous_frame(self, data, stride, self->frame_height);
            drd_encoding_manager_update_tile_hashes(self, data, stride);
            drd_encoding_manager_register_codec_result(self, DRD_ENCODING_CODEC_CLASS_NON_AVC, keyframe_encode);
            self->gfx_force_keyframe = FALSE;
        }
    }
    else if (use_remotefx)
    {
        DRD_LOG_MESSAGE("remotefx encode");
        BOOL rc = 0;
        wStream *s = NULL;
        GArray *rects = self->gfx_dirty_rects;

        if (!drd_encoder_prepare(self, FREERDP_CODEC_REMOTEFX, settings))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "failed to prepare encoder FREERDP_CODEC_REMOTEFX");
            goto out;
        }

        s = Stream_New(NULL, 1024);
        WINPR_ASSERT(s);

        WINPR_ASSERT(rects != NULL);
        g_array_set_size(rects, 0);
        WINPR_ASSERT(self->frame_width <= UINT16_MAX);
        WINPR_ASSERT(self->frame_height <= UINT16_MAX);
        const gboolean refresh_interval_reached = drd_encoding_manager_refresh_interval_reached(self);
        const gboolean keyframe_encode = self->gfx_force_keyframe || !self->enable_diff || refresh_interval_reached;

        if (keyframe_encode)
        {
            memset(self->gfx_tile_hashes->data, 0, self->gfx_tile_hashes->len * sizeof(guint64));
            RFX_RECT full = {0, 0, (UINT16) self->frame_width, (UINT16) self->frame_height};
            g_array_append_val(rects, full);
        }
        else if (!drd_encoding_manager_collect_dirty_rects(self, dirty_flags, rects))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "not exist dirty region");
            Stream_Free(s, TRUE);
            goto out;
        }

        WINPR_ASSERT(rects->len <= UINT16_MAX);
        rc = rfx_compose_message(self->rfx, s, (RFX_RECT *) rects->data, rects->len, data, self->frame_width, self->frame_height, stride);

        if (!rc)
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "rfx_compose_message failed");
            Stream_Free(s, TRUE);
            goto out;
        }
        /* rc > 0 means new data */
        if (rc > 0)
        {
            const size_t pos = Stream_GetPosition(s);
            WINPR_ASSERT(pos <= UINT32_MAX);

            cmd.codecId = RDPGFX_CODECID_CAVIDEO;
            cmd.data = Stream_Buffer(s);
            cmd.length = (UINT32) pos;

            IFCALLRET(context->SurfaceFrameCommand, if_error, context, &cmd, &cmd_start, &cmd_end);
        }

        Stream_Free(s, TRUE);
        if (if_error)
        {
            g_autofree gchar *err_msg = g_strdup_printf("SurfaceFrameCommand failed with error %" PRIu32 "", if_error);
            self->gfx_force_keyframe = TRUE;
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, err_msg);
            goto out;
        }
        if (rc > 0)
        {
            drd_encoding_manager_store_previous_frame(self, data, stride, self->frame_height);
            drd_encoding_manager_update_tile_hashes(self, data, stride);
            drd_encoding_manager_register_codec_result(self, DRD_ENCODING_CODEC_CLASS_NON_AVC, keyframe_encode);
            self->gfx_force_keyframe = FALSE;
        }
    }
    else
    {
        // not reached:planar and freerdp_image_copy_no_overlap
    }
    success = TRUE;

out:
    if (dirty_flags != NULL)
    {
        g_array_free(dirty_flags, TRUE);
    }

    return success;
}

/*
 * 功能：SurfaceBits 编码实现。
 * 逻辑：SurfaceBits 路径未实现，拒绝切换。
 * 参数：self 管理器；context rdp context；input 原始帧；frame_id 帧序列号；max_payload 负载上限；error 错误输出。
 * 外部接口：返回 FALSE 并设置 G_IO_ERROR_NOT_SUPPORTED 错误。
 */
gboolean drd_encoding_manager_encode_surface_bit(DrdEncodingManager *self, rdpContext *context, DrdFrame *input,
                                                 guint32 frame_id, gsize max_payload, GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);
    g_return_val_if_fail(context != NULL, FALSE);
    g_return_val_if_fail(DRD_IS_FRAME(input), FALSE);

#ifdef SURFACE_BITS_NOT_IMPLEMENTED
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "SurfaceBits encoder not implemented, Rdpgfx preferred");
    return FALSE;
#else
    /* TODO: 实现实际的 SurfaceBits 编码 */
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "SurfaceBits encoder placeholder");
    return FALSE;
#endif
}

/*
 * 功能：请求下一个编码产生关键帧。
 * 逻辑：置关键帧标记，供 RFX/Progressive 编码路径读取。
 * 参数：self 管理器实例。
 * 外部接口：无。
 */
void drd_encoding_manager_force_keyframe(DrdEncodingManager *self)
{
    g_return_if_fail(DRD_IS_ENCODING_MANAGER(self));
    self->gfx_force_keyframe = TRUE;
}
