#include "session/drd_rdp_graphics_pipeline.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/color.h>

#include <gio/gio.h>

#include "utils/drd_log.h"

struct _DrdRdpGraphicsPipeline
{
    GObject parent_instance;

    freerdp_peer *peer;
    guint16 width;
    guint16 height;

    RdpgfxServerContext *rdpgfx_context;
    gboolean channel_opened;
    gboolean caps_confirmed;
    gboolean surface_ready;

    guint16 surface_id;
    guint32 codec_context_id;
    guint32 next_frame_id;
    /*
     * needs_keyframe: Progressive RFX 在 Reset/提交失败后必须先发送一帧完整关键帧
     * (带 SYNC/CONTEXT/REGION/TILE)，客户端才能正确叠加后续增量帧。
     * 一旦我们检测到关键帧丢失，就置位该标志，禁止额外增量帧写入，直到新的
     * 关键帧成功入队；否则 Windows mstsc 会因没有基线而花屏。
     */
    gboolean needs_keyframe;

    gint outstanding_frames;
    guint max_outstanding_frames;
    guint32 channel_id;
    gboolean frame_acks_suspended; /* Rdpgfx 客户端暂停 ACK 时跳过背压 */
    GMutex lock;
    /*
     * capacity_cond: Rdpgfx 背压的条件变量。当 outstanding_frames 达到上限时，
     * renderer 线程会在 drd_rdp_graphics_pipeline_wait_for_capacity() 内等待该条件;
     * 客户端发送 FrameAcknowledge 或提交失败时唤醒，保证编码/发送速率与客户端 ACK
     * 节奏一致，避免“先编码再丢弃”导致的花屏。
     */
    GCond capacity_cond;
};

G_DEFINE_TYPE(DrdRdpGraphicsPipeline, drd_rdp_graphics_pipeline, G_TYPE_OBJECT)

/*
 * 功能：返回 Rdpgfx 管线的错误域 quark。
 * 逻辑：使用 g_quark_from_static_string 注册静态错误域名。
 * 参数：无。
 * 外部接口：GLib g_quark_from_static_string。
 */
GQuark
drd_rdp_graphics_pipeline_error_quark(void)
{
    return g_quark_from_static_string("drd-rdp-graphics-pipeline-error");
}

static BOOL drd_rdpgfx_channel_assigned(RdpgfxServerContext *context, UINT32 channel_id);

static UINT drd_rdpgfx_caps_advertise(RdpgfxServerContext *context,
                                      const RDPGFX_CAPS_ADVERTISE_PDU *caps);

static UINT drd_rdpgfx_frame_ack(RdpgfxServerContext *context,
                                 const RDPGFX_FRAME_ACKNOWLEDGE_PDU *ack);

/*
 * 功能：生成符合 Rdpgfx 要求的 32 位时间戳。
 * 逻辑：获取本地时间，按小时/分钟/秒/毫秒编码到 32 位整数。
 * 参数：无。
 * 外部接口：GLib GDateTime API 获取时间。
 */
static guint32
drd_rdp_graphics_pipeline_build_timestamp(void)
{
    guint32 timestamp = 0;
    GDateTime *now = g_date_time_new_now_local();

    if (now != NULL)
    {
        timestamp = ((guint32) g_date_time_get_hour(now) << 22) |
                    ((guint32) g_date_time_get_minute(now) << 16) |
                    ((guint32) g_date_time_get_second(now) << 10) |
                    ((guint32)(g_date_time_get_microsecond(now) / 1000));
        g_date_time_unref(now);
    }

    return timestamp;
}

/*
 * 功能：在持有锁的情况下重置 Rdpgfx surface 与上下文。
 * 逻辑：发送 ResetGraphics、CreateSurface、MapSurfaceToOutput 三个 PDU，重置帧计数、背压与标志位。
 * 参数：self 图形管线。
 * 外部接口：调用 RdpgfxServerContext 的 ResetGraphics/CreateSurface/MapSurfaceToOutput 函数，
 *           这些接口由 FreeRDP 提供。
 */
static gboolean
drd_rdp_graphics_pipeline_reset_locked(DrdRdpGraphicsPipeline *self)
{
    g_assert(self->rdpgfx_context != NULL);

    if (self->surface_ready)
    {
        return TRUE;
    }

    RDPGFX_RESET_GRAPHICS_PDU reset = {0};
    reset.width = self->width;
    reset.height = self->height;
    reset.monitorCount = 0;
    reset.monitorDefArray = NULL;

    if (!self->rdpgfx_context->ResetGraphics ||
        self->rdpgfx_context->ResetGraphics(self->rdpgfx_context, &reset) != CHANNEL_RC_OK)
    {
        DRD_LOG_WARNING("Graphics pipeline failed to reset graphics");
        return FALSE;
    }

    RDPGFX_CREATE_SURFACE_PDU create = {0};
    create.surfaceId = self->surface_id;
    create.width = self->width;
    create.height = self->height;
    create.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;

    if (!self->rdpgfx_context->CreateSurface ||
        self->rdpgfx_context->CreateSurface(self->rdpgfx_context, &create) != CHANNEL_RC_OK)
    {
        DRD_LOG_WARNING("Graphics pipeline failed to create surface %u", self->surface_id);
        return FALSE;
    }

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = {0};
    map.surfaceId = self->surface_id;
    map.outputOriginX = 0;
    map.outputOriginY = 0;

    if (!self->rdpgfx_context->MapSurfaceToOutput ||
        self->rdpgfx_context->MapSurfaceToOutput(self->rdpgfx_context, &map) != CHANNEL_RC_OK)
    {
        DRD_LOG_WARNING("Graphics pipeline failed to map surface %u to output",
                        self->surface_id);
        return FALSE;
    }

    self->next_frame_id = 1;
    self->outstanding_frames = 0;
    self->surface_ready = TRUE;
    self->needs_keyframe = TRUE;
    self->frame_acks_suspended = FALSE;
    g_cond_broadcast(&self->capacity_cond);
    return TRUE;
}

/*
 * 功能：释放 Rdpgfx 管线持有的上下文与 surface。
 * 逻辑：若 surface 已创建则发送 DeleteSurface；若通道已打开则调用 Close；最后交给父类 dispose。
 * 参数：object GObject 指针。
 * 外部接口：调用 RdpgfxServerContext->DeleteSurface/Close（FreeRDP）关闭资源。
 */
static void
drd_rdp_graphics_pipeline_dispose(GObject *object)
{
    DrdRdpGraphicsPipeline *self = DRD_RDP_GRAPHICS_PIPELINE(object);

    if (self->rdpgfx_context != NULL)
    {
        if (self->surface_ready && self->rdpgfx_context->DeleteSurface)
        {
            RDPGFX_DELETE_SURFACE_PDU del = {0};
            del.surfaceId = self->surface_id;
            self->rdpgfx_context->DeleteSurface(self->rdpgfx_context, &del);
            self->surface_ready = FALSE;
            g_cond_broadcast(&self->capacity_cond);
        }

        if (self->channel_opened && self->rdpgfx_context->Close)
        {
            self->rdpgfx_context->Close(self->rdpgfx_context);
            self->channel_opened = FALSE;
        }
    }

    G_OBJECT_CLASS(drd_rdp_graphics_pipeline_parent_class)->dispose(object);
}

/*
 * 功能：释放同步原语与 Rdpgfx 上下文。
 * 逻辑：清理条件变量/互斥量，释放 Rdpgfx server context，委托父类 finalize。
 * 参数：object GObject 指针。
 * 外部接口：GLib g_cond_clear/g_mutex_clear；FreeRDP rdpgfx_server_context_free。
 */
static void
drd_rdp_graphics_pipeline_finalize(GObject *object)
{
    DrdRdpGraphicsPipeline *self = DRD_RDP_GRAPHICS_PIPELINE(object);

    g_cond_clear(&self->capacity_cond);
    g_mutex_clear(&self->lock);
    g_clear_pointer(&self->rdpgfx_context, rdpgfx_server_context_free);

    G_OBJECT_CLASS(drd_rdp_graphics_pipeline_parent_class)->finalize(object);
}

/*
 * 功能：初始化图形管线实例的同步与默认参数。
 * 逻辑：初始化互斥与条件变量，设置 surface/codec/frame 计数默认值与标志位。
 * 参数：self 图形管线。
 * 外部接口：GLib g_mutex_init/g_cond_init。
 */
static void
drd_rdp_graphics_pipeline_init(DrdRdpGraphicsPipeline *self)
{
    g_mutex_init(&self->lock);
    g_cond_init(&self->capacity_cond);
    self->surface_id = 1;
    self->codec_context_id = 1;
    self->next_frame_id = 1;
    self->max_outstanding_frames = 3;
    self->needs_keyframe = TRUE;
    self->frame_acks_suspended = FALSE;
}

/*
 * 功能：注册图形管线的生命周期回调。
 * 逻辑：将 dispose 与 finalize 指针挂到 GObjectClass。
 * 参数：klass 类结构指针。
 * 外部接口：GLib GObject 类型系统。
 */
static void
drd_rdp_graphics_pipeline_class_init(DrdRdpGraphicsPipelineClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_rdp_graphics_pipeline_dispose;
    object_class->finalize = drd_rdp_graphics_pipeline_finalize;
}

DrdRdpGraphicsPipeline *
drd_rdp_graphics_pipeline_new(freerdp_peer *peer,
                              HANDLE vcm,
                              guint16 surface_width,
                              guint16 surface_height)
{
    /*
     * 功能：创建绑定指定 peer/VCM 的图形管线实例。
     * 逻辑：校验参数有效后分配 Rdpgfx server context 并设置自定义回调，
     *       保存 surface 尺寸与 peer/context。
     * 参数：peer FreeRDP peer；vcm 虚拟通道管理器句柄；surface_width/height 渲染表面尺寸。
     * 外部接口：FreeRDP rdpgfx_server_context_new 分配上下文，设置 ChannelIdAssigned/CapsAdvertise/FrameAcknowledge 回调。
     */
    g_return_val_if_fail(peer != NULL, NULL);
    g_return_val_if_fail(peer->context != NULL, NULL);
    g_return_val_if_fail(vcm != NULL && vcm != INVALID_HANDLE_VALUE, NULL);

    RdpgfxServerContext *rdpgfx_context = rdpgfx_server_context_new(vcm);
    if (rdpgfx_context == NULL)
    {
        DRD_LOG_WARNING("Failed to allocate Rdpgfx server context");
        return NULL;
    }

    DrdRdpGraphicsPipeline *self = g_object_new(DRD_TYPE_RDP_GRAPHICS_PIPELINE, NULL);
    self->peer = peer;
    self->width = surface_width;
    self->height = surface_height;
    self->rdpgfx_context = rdpgfx_context;

    rdpgfx_context->rdpcontext = peer->context;
    rdpgfx_context->custom = self;
    rdpgfx_context->ChannelIdAssigned = drd_rdpgfx_channel_assigned;
    rdpgfx_context->CapsAdvertise = drd_rdpgfx_caps_advertise;
    rdpgfx_context->FrameAcknowledge = drd_rdpgfx_frame_ack;

    return self;
}

/*
 * 功能：尝试初始化 Rdpgfx 渠道与 surface，确保通道能力准备就绪。
 * 逻辑：持锁检查上下文；若未打开通道则调用 Open；等待 CapsConfirm 完成；最后调用 reset_locked
 *       创建 surface 并复位背压。
 * 参数：self 图形管线。
 * 外部接口：FreeRDP RdpgfxServerContext->Open/CapsConfirm、内部 reset_locked。
 */
gboolean
drd_rdp_graphics_pipeline_maybe_init(DrdRdpGraphicsPipeline *self)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), FALSE);

    g_mutex_lock(&self->lock);

    if (self->rdpgfx_context == NULL)
    {
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    if (!self->channel_opened)
    {
        RdpgfxServerContext *rdpgfx_context = self->rdpgfx_context;

        g_mutex_unlock(&self->lock);

        if (rdpgfx_context == NULL ||
            rdpgfx_context->Open == NULL ||
            !rdpgfx_context->Open(rdpgfx_context))
        {
            DRD_LOG_WARNING("Failed to open Rdpgfx channel");
            return FALSE;
        }
        g_mutex_lock(&self->lock);

        if (self->rdpgfx_context != rdpgfx_context)
        {
            g_mutex_unlock(&self->lock);
            return FALSE;
        }

        self->channel_opened = TRUE;
    }

    if (!self->caps_confirmed)
    {
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    gboolean ok = drd_rdp_graphics_pipeline_reset_locked(self);
    g_mutex_unlock(&self->lock);
    return ok;
}

/*
 * 功能：判断 surface 是否已创建可用。
 * 逻辑：持锁读取 surface_ready 标志。
 * 参数：self 图形管线。
 * 外部接口：无额外外部库调用。
 */
gboolean
drd_rdp_graphics_pipeline_is_ready(DrdRdpGraphicsPipeline *self)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), FALSE);

    g_mutex_lock(&self->lock);
    gboolean ready = self->surface_ready;
    g_mutex_unlock(&self->lock);
    return ready;
}

/*
 * 功能：检查是否允许提交新帧（背压控制）。
 * 逻辑：持锁判断 surface_ready 且 outstanding_frames 未超过上限，或客户端暂停 ACK。
 * 参数：self 图形管线。
 * 外部接口：无额外外部库调用。
 */
gboolean
drd_rdp_graphics_pipeline_can_submit(DrdRdpGraphicsPipeline *self)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), FALSE);

    g_mutex_lock(&self->lock);
    gboolean ok = self->surface_ready &&
                  (self->frame_acks_suspended ||
                   self->outstanding_frames < (gint) self->max_outstanding_frames);
    g_mutex_unlock(&self->lock);
    return ok;
}

/*
 * 功能：等待 Rdpgfx 管线具备提交容量（基于 outstanding_frames）。
 * 逻辑：在 surface_ready 时根据 timeout_us 在条件变量上等待 outstanding_frames 降至上限以下，
 *       支持无限或超时等待。
 * 参数：self 管线；timeout_us 等待时间，-1 表示无限。
 * 外部接口：GLib g_cond_wait/g_cond_wait_until。
 */
gboolean
drd_rdp_graphics_pipeline_wait_for_capacity(DrdRdpGraphicsPipeline *self,
                                            gint64 timeout_us)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), FALSE);
    /*
     * capacity 的定义：未被客户端 RDPGFX_FRAME_ACKNOWLEDGE 确认的帧数
     * (outstanding_frames) 必须始终小于 max_outstanding_frames（默认 3）。
     * 当编码线程调用本函数时，如果 outstanding>=max，就在 capacity_cond 上阻塞，
     * 直到 FrameAcknowledge 或提交失败/Reset 释放槽位。
     */
    gint64 deadline = 0;
    if (timeout_us > 0)
    {
        deadline = g_get_monotonic_time() + timeout_us;
    }

    g_mutex_lock(&self->lock);
    while (self->surface_ready && !self->frame_acks_suspended &&
           self->outstanding_frames >= (gint) self->max_outstanding_frames)
    {
        if (timeout_us < 0)
        {
            g_cond_wait(&self->capacity_cond, &self->lock);
        }
        else if (!g_cond_wait_until(&self->capacity_cond, &self->lock, deadline))
        {
            break;
        }
    }

    gboolean ready = self->surface_ready &&
                     (self->frame_acks_suspended ||
                      self->outstanding_frames < (gint) self->max_outstanding_frames);
    g_mutex_unlock(&self->lock);
    return ready;
}

/*
 * 功能：将 Progressive RFX 帧提交到 Rdpgfx 通道，维护背压与关键帧状态。
 * 逻辑：校验编码类型与 surface 就绪；若需要关键帧而未提供则报错；在背压超限时返回 WOULD_BLOCK；
 *       生成 frame_id，填充 StartFrame/SurfaceCommand/EndFrame PDU 并调用 FreeRDP 回调发送；
 *       失败时回滚 outstanding、标记需要关键帧并唤醒等待者。
 * 参数：self 管线；frame 编码帧；frame_is_keyframe 是否关键帧；error 输出错误。
 * 外部接口：FreeRDP RdpgfxServerContext 的 SurfaceFrameCommand/StartFrame/SurfaceCommand/EndFrame；
 *           GLib g_set_error/g_error_matches，使用 DRD_RDP_GRAPHICS_PIPELINE_ERROR 域。
 */
gboolean
drd_rdp_graphics_pipeline_submit_frame(DrdRdpGraphicsPipeline *self,
                                       DrdEncodedFrame *frame,
                                       gboolean frame_is_keyframe,
                                       GError **error)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), FALSE);
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(frame), FALSE);

    if (drd_encoded_frame_get_codec(frame) != DRD_FRAME_CODEC_RFX_PROGRESSIVE)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoded frame is not RFX progressive");
        return FALSE;
    }

    gsize payload_size = 0;
    const guint8 *payload = drd_encoded_frame_get_data(frame, &payload_size);
    if (payload == NULL || payload_size == 0)
    {
        return TRUE;
    }

    guint32 frame_id = 0;

    g_mutex_lock(&self->lock);
    if (!self->surface_ready)
    {
        g_mutex_unlock(&self->lock);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Graphics pipeline surface not ready");
        return FALSE;
    }

    if (self->needs_keyframe && !frame_is_keyframe)
    {
        g_mutex_unlock(&self->lock);
        g_set_error(error,
                    DRD_RDP_GRAPHICS_PIPELINE_ERROR,
                    DRD_RDP_GRAPHICS_PIPELINE_ERROR_NEEDS_KEYFRAME,
                    "Graphics pipeline requires RFX progressive keyframe");
        return FALSE;
    }

    gboolean track_ack = !self->frame_acks_suspended;

    if (track_ack &&
        self->outstanding_frames >= (gint) self->max_outstanding_frames)
    {
        g_mutex_unlock(&self->lock);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_WOULD_BLOCK,
                            "Graphics pipeline congestion");
        return FALSE;
    }

    frame_id = self->next_frame_id++;
    if (self->next_frame_id == 0)
    {
        self->next_frame_id = 1;
    }
    if (track_ack)
    {
        self->outstanding_frames++;
    }
    if (frame_is_keyframe)
    {
        self->needs_keyframe = FALSE;
    }
    g_mutex_unlock(&self->lock);

    RDPGFX_START_FRAME_PDU start = {0};
    start.timestamp = drd_rdp_graphics_pipeline_build_timestamp();
    start.frameId = frame_id;

    RDPGFX_END_FRAME_PDU end = {0};
    end.frameId = frame_id;

    RDPGFX_SURFACE_COMMAND cmd = {0};
    cmd.surfaceId = self->surface_id;
    cmd.codecId = RDPGFX_CODECID_CAPROGRESSIVE;
    cmd.contextId = self->codec_context_id;
    cmd.format = PIXEL_FORMAT_BGRX32;
    cmd.left = 0;
    cmd.top = 0;
    cmd.right = self->width;
    cmd.bottom = self->height;
    cmd.width = self->width;
    cmd.height = self->height;
    cmd.length = (UINT32) payload_size;
    cmd.data = (BYTE *) payload;

    UINT status = CHANNEL_RC_OK;
    if (self->rdpgfx_context->SurfaceFrameCommand != NULL)
    {
        status = self->rdpgfx_context->SurfaceFrameCommand(self->rdpgfx_context,
                                                           &cmd,
                                                           &start,
                                                           &end);
    }
    else if (!self->rdpgfx_context->StartFrame ||
             self->rdpgfx_context->StartFrame(self->rdpgfx_context, &start) != CHANNEL_RC_OK ||
             !self->rdpgfx_context->SurfaceCommand ||
             self->rdpgfx_context->SurfaceCommand(self->rdpgfx_context, &cmd) != CHANNEL_RC_OK ||
             !self->rdpgfx_context->EndFrame ||
             self->rdpgfx_context->EndFrame(self->rdpgfx_context, &end) != CHANNEL_RC_OK)
    {
        status = CHANNEL_RC_BAD_PROC;
    }

    if (status != CHANNEL_RC_OK)
    {
        g_mutex_lock(&self->lock);
        if (track_ack && self->outstanding_frames > 0)
        {
            self->outstanding_frames--;
        }
        self->needs_keyframe = TRUE;
        g_cond_broadcast(&self->capacity_cond);
        g_mutex_unlock(&self->lock);

        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to submit frame over Rdpgfx");
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：记录 Rdpgfx 通道分配的 ChannelId。
 * 逻辑：从回调获取 channel_id 并写入实例字段。
 * 参数：context Rdpgfx server 上下文；channel_id 分配的通道号。
 * 外部接口：回调由 FreeRDP 在通道分配时调用。
 */
static BOOL
drd_rdpgfx_channel_assigned(RdpgfxServerContext *context, UINT32 channel_id)
{
    DrdRdpGraphicsPipeline *self = context != NULL ? context->custom : NULL;

    if (self == NULL)
    {
        return CHANNEL_RC_OK;
    }

    g_mutex_lock(&self->lock);
    self->channel_id = channel_id;
    g_mutex_unlock(&self->lock);
    return TRUE;
}

/*
 * 功能：处理客户端能力广告并返回确认。
 * 逻辑：读取首个 capsSet，调用 CapsConfirm 回调回复；成功后标记 caps_confirmed。
 * 参数：context Rdpgfx 上下文；caps 客户端能力广告。
 * 外部接口：FreeRDP RdpgfxServerContext->CapsConfirm。
 */
static UINT
drd_rdpgfx_caps_advertise(RdpgfxServerContext *context,
                          const RDPGFX_CAPS_ADVERTISE_PDU *caps)
{
    DrdRdpGraphicsPipeline *self = context != NULL ? context->custom : NULL;

    if (self == NULL || caps == NULL || caps->capsSetCount == 0)
    {
        return CHANNEL_RC_OK;
    }

    RDPGFX_CAPS_CONFIRM_PDU confirm = {0};
    confirm.capsSet = &caps->capsSets[0];
    UINT status = CHANNEL_RC_OK;

    if (context->CapsConfirm)
    {
        status = context->CapsConfirm(context, &confirm);
    }

    if (status == CHANNEL_RC_OK)
    {
        g_mutex_lock(&self->lock);
        self->caps_confirmed = TRUE;
        g_mutex_unlock(&self->lock);
    }

    return status;
}

/*
 * 功能：处理客户端 FrameAcknowledge，维护背压与 ACK 状态。
 * 逻辑：在 SUSPEND_FRAME_ACKNOWLEDGEMENT 时清零 outstanding 并挂起背压；正常情况将 outstanding 减 1，
 *       并唤醒等待容量的线程。
 * 参数：context Rdpgfx 上下文；ack 客户端 ACK PDU。
 * 外部接口：FreeRDP 调用该回调；日志使用 DRD_LOG_MESSAGE。
 */
static UINT
drd_rdpgfx_frame_ack(RdpgfxServerContext *context,
                     const RDPGFX_FRAME_ACKNOWLEDGE_PDU *ack)
{
    DrdRdpGraphicsPipeline *self = context != NULL ? context->custom : NULL;

    if (self == NULL || ack == NULL)
    {
        return CHANNEL_RC_OK;
    }
    g_mutex_lock(&self->lock);

    if (ack->queueDepth == SUSPEND_FRAME_ACKNOWLEDGEMENT)
    {
        if (!self->frame_acks_suspended)
        {
            DRD_LOG_MESSAGE("RDPGFX client suspended frame acknowledgements");
        }
        self->frame_acks_suspended = TRUE;
        self->outstanding_frames = 0;
        g_cond_broadcast(&self->capacity_cond);
        g_mutex_unlock(&self->lock);
        return CHANNEL_RC_OK;
    }

    if (self->frame_acks_suspended)
    {
        DRD_LOG_MESSAGE("RDPGFX client resumed frame acknowledgements");
    }
    self->frame_acks_suspended = FALSE;
    /*
     * 客户端在成功解码/渲染一帧 Progressive 数据后会发送 RDPGFX_FRAME_ACKNOWLEDGE_PDU，
     * 告知服务器 frameId、totalFramesDecoded 以及 queueDepth。我们只需要把
     * outstanding_frames 减 1 并唤醒等待 capacity_cond 的编码线程，保证新的帧
     * 只有在客户端确认后才继续发送。
     */
    if (self->outstanding_frames > 0)
    {
        self->outstanding_frames--;
    }
    g_cond_broadcast(&self->capacity_cond);
    g_mutex_unlock(&self->lock);

    return CHANNEL_RC_OK;
}
