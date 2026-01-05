#include "session/drd_rdp_graphics_pipeline.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/color.h>

#include <gio/gio.h>

#include "core/drd_server_runtime.h"
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

    DrdServerRuntime *runtime;
    gboolean last_frame_h264;
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

static UINT drd_rdpgfx_frame_ack(RdpgfxServerContext *context,
                                 const RDPGFX_FRAME_ACKNOWLEDGE_PDU *ack);

static UINT shadow_client_rdpgfx_caps_advertise(RdpgfxServerContext* context,
                                                const RDPGFX_CAPS_ADVERTISE_PDU* capsAdvertise);

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
    map.reserved = 0;
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
    self->last_frame_h264 = FALSE;
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
                              DrdServerRuntime *runtime,
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
    self->runtime = runtime;

    rdpgfx_context->rdpcontext = peer->context;
    rdpgfx_context->custom = self;
    rdpgfx_context->ChannelIdAssigned = drd_rdpgfx_channel_assigned;
    rdpgfx_context->CapsAdvertise = shadow_client_rdpgfx_caps_advertise;
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
    drd_server_runtime_set_transport(self->runtime,DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE);
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
                   self->outstanding_frames < (gint) self->max_outstanding_frames ||
                   self->last_frame_h264);
    g_mutex_unlock(&self->lock);
    return ok;
}

guint16
drd_rdp_graphics_pipeline_get_surface_id(DrdRdpGraphicsPipeline *self)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), 0);

    return self->surface_id;
}

void drd_rdp_graphics_pipeline_out_frame_change(DrdRdpGraphicsPipeline *self, gboolean add)
{
    g_mutex_lock(&self->lock);
    if (add)
    {
        if (!self->frame_acks_suspended)
        {
            self->outstanding_frames++;
        }
    }
    else
    {
        if (!self->frame_acks_suspended && self->outstanding_frames > 0)
        {
            self->outstanding_frames--;
        }
        g_cond_broadcast(&self->capacity_cond);
    }
    g_mutex_unlock(&self->lock);
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
    if (self->last_frame_h264)
    {
        g_mutex_unlock(&self->lock);
        return TRUE;
    }
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

static BOOL shadow_are_caps_filtered(const rdpSettings* settings, UINT32 caps)
{
    const UINT32 capList[] = { RDPGFX_CAPVERSION_8,   RDPGFX_CAPVERSION_81,
                               RDPGFX_CAPVERSION_10,  RDPGFX_CAPVERSION_101,
                               RDPGFX_CAPVERSION_102, RDPGFX_CAPVERSION_103,
                               RDPGFX_CAPVERSION_104, RDPGFX_CAPVERSION_105,
                               RDPGFX_CAPVERSION_106, RDPGFX_CAPVERSION_106_ERR,
                               RDPGFX_CAPVERSION_107 };

    WINPR_ASSERT(settings);
    const UINT32 filter = freerdp_settings_get_uint32(settings, FreeRDP_GfxCapsFilter);

    for (UINT32 x = 0; x < ARRAYSIZE(capList); x++)
    {
        if (caps == capList[x])
            return (filter & (1 << x)) != 0;
    }

    return TRUE;
}

static UINT shadow_client_send_caps_confirm(DrdRdpGraphicsPipeline *self,RdpgfxServerContext* context,
                                            const RDPGFX_CAPS_CONFIRM_PDU* pdu)
{
    WINPR_ASSERT(context);
    WINPR_ASSERT(self);
    WINPR_ASSERT(pdu);

    WINPR_ASSERT(context->CapsConfirm);
    UINT rc = context->CapsConfirm(context, pdu);
    g_mutex_lock(&self->lock);
    self->caps_confirmed = TRUE;
    g_mutex_unlock(&self->lock);
    return rc;
}


static BOOL shadow_client_caps_test_version(DrdRdpGraphicsPipeline *self,RdpgfxServerContext* context,
                                            BOOL h264, const RDPGFX_CAPSET* capsSets,
                                            UINT32 capsSetCount, UINT32 capsVersion, UINT* rc)
{
	const rdpSettings* srvSettings = NULL;
	rdpSettings* clientSettings = NULL;

	WINPR_ASSERT(context);
	WINPR_ASSERT(self);
	WINPR_ASSERT(capsSets || (capsSetCount == 0));
	WINPR_ASSERT(rc);

	WINPR_ASSERT(context->rdpcontext);
	srvSettings = context->rdpcontext->settings;
	WINPR_ASSERT(srvSettings);

	clientSettings = self->peer->context->settings; // TODO
	WINPR_ASSERT(clientSettings);

	if (shadow_are_caps_filtered(srvSettings, capsVersion))
		return FALSE;

	for (UINT32 index = 0; index < capsSetCount; index++)
	{
		const RDPGFX_CAPSET* currentCaps = &capsSets[index];

		if (currentCaps->version == capsVersion)
		{
			UINT32 flags = 0;
			BOOL rfx = FALSE;
			BOOL avc444v2 = FALSE;
			BOOL avc444 = FALSE;
			BOOL avc420 = FALSE;
			BOOL progressive = FALSE;
			RDPGFX_CAPSET caps = *currentCaps;
			RDPGFX_CAPS_CONFIRM_PDU pdu = { 0 };
			pdu.capsSet = &caps;

			flags = pdu.capsSet->flags;

			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxSmallCache,
			                               (flags & RDPGFX_CAPS_FLAG_SMALL_CACHE) ? TRUE : FALSE))
				return FALSE;

			avc444v2 = avc444 = !(flags & RDPGFX_CAPS_FLAG_AVC_DISABLED);
            avc420 = avc444;
		    gboolean avc444v2_setting = freerdp_settings_get_bool(srvSettings, FreeRDP_GfxAVC444v2);
		    gboolean avc444_setting = freerdp_settings_get_bool(srvSettings, FreeRDP_GfxAVC444);
		    gboolean h264_setting = freerdp_settings_get_bool(srvSettings, FreeRDP_GfxH264);
			if (!avc444v2_setting || !h264)
				avc444v2 = FALSE;
			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxAVC444v2, avc444v2))
				return FALSE;
			if (!avc444_setting || !h264)
				avc444 = FALSE;
			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxAVC444, avc444))
				return FALSE;
            if (!h264_setting || !h264)
            {
                avc420 = FALSE;
            }

			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxH264, avc420))
				return FALSE;


			progressive = freerdp_settings_get_bool(srvSettings, FreeRDP_GfxProgressive);
			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxProgressive, progressive))
				return FALSE;
			progressive = freerdp_settings_get_bool(srvSettings, FreeRDP_GfxProgressiveV2);
			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxProgressiveV2, progressive))
				return FALSE;

			rfx = freerdp_settings_get_bool(srvSettings, FreeRDP_RemoteFxCodec);
			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_RemoteFxCodec, rfx))
				return FALSE;

			// planar = freerdp_settings_get_bool(srvSettings, FreeRDP_GfxPlanar);
			if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxPlanar, FALSE))
				return FALSE;

			if (!avc444v2 && !avc444 && !avc420)
				pdu.capsSet->flags |= RDPGFX_CAPS_FLAG_AVC_DISABLED;

			*rc = shadow_client_send_caps_confirm(self, context, &pdu);
			return TRUE;
		}
	}

	return FALSE;
}

static UINT shadow_client_rdpgfx_caps_advertise(RdpgfxServerContext* context,
                                                const RDPGFX_CAPS_ADVERTISE_PDU* capsAdvertise)
{
	UINT rc = ERROR_INTERNAL_ERROR;
	const rdpSettings* srvSettings = NULL;
	rdpSettings* clientSettings = NULL;
	BOOL h264 = FALSE;

	UINT32 flags = 0;

	WINPR_ASSERT(context);
	WINPR_ASSERT(capsAdvertise);

    DrdRdpGraphicsPipeline *self = context->custom;

    if (self == NULL ||capsAdvertise->capsSetCount == 0)
    {
        return CHANNEL_RC_OK;
    }

    rdpSettings *settings = (context->rdpcontext != NULL) ? context->rdpcontext->settings : NULL;
    if (settings == NULL)
    {
        return CHANNEL_RC_OK;
    }

	WINPR_ASSERT(self);
	WINPR_ASSERT(context->rdpcontext);

	srvSettings = context->rdpcontext->settings;
	WINPR_ASSERT(srvSettings);

	clientSettings = context->rdpcontext->settings;
	WINPR_ASSERT(clientSettings);

    h264 = drd_runtime_encoder_prepare(self->runtime, FREERDP_CODEC_AVC420 | FREERDP_CODEC_AVC444, clientSettings);
    DRD_LOG_MESSAGE("h264 support: %d", h264);
    drd_server_runtime_request_keyframe(self->runtime);

	if (shadow_client_caps_test_version(self,context,h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_107, &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_106, &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_106_ERR,
	                                    &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_105, &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_104, &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_103, &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_102, &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_101, &rc))
		return rc;

	if (shadow_client_caps_test_version(self,context, h264, capsAdvertise->capsSets,
	                                    capsAdvertise->capsSetCount, RDPGFX_CAPVERSION_10, &rc))
		return rc;

	if (!shadow_are_caps_filtered(srvSettings, RDPGFX_CAPVERSION_81))
	{
		for (UINT32 index = 0; index < capsAdvertise->capsSetCount; index++)
		{
			const RDPGFX_CAPSET* currentCaps = &capsAdvertise->capsSets[index];

			if (currentCaps->version == RDPGFX_CAPVERSION_81)
			{
				RDPGFX_CAPSET caps = *currentCaps;
				RDPGFX_CAPS_CONFIRM_PDU pdu;
				pdu.capsSet = &caps;

				flags = pdu.capsSet->flags;

				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxAVC444v2, FALSE))
					return rc;
				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxAVC444, FALSE))
					return rc;

				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxThinClient,
				                               (flags & RDPGFX_CAPS_FLAG_THINCLIENT) ? TRUE
				                                                                     : FALSE))
					return rc;
				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxSmallCache,
				                               (flags & RDPGFX_CAPS_FLAG_SMALL_CACHE) ? TRUE
				                                                                      : FALSE))
					return rc;



				if (h264)
				{
					if (!freerdp_settings_set_bool(
					        clientSettings, FreeRDP_GfxH264,
					        (flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) ? TRUE : FALSE))
						return rc;
				}
				else
				{
					if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxH264, FALSE))
						return rc;
				}

				return shadow_client_send_caps_confirm(self,context, &pdu);
			}
		}
	}

	if (!shadow_are_caps_filtered(srvSettings, RDPGFX_CAPVERSION_8))
	{
		for (UINT32 index = 0; index < capsAdvertise->capsSetCount; index++)
		{
			const RDPGFX_CAPSET* currentCaps = &capsAdvertise->capsSets[index];

			if (currentCaps->version == RDPGFX_CAPVERSION_8)
			{
				RDPGFX_CAPSET caps = *currentCaps;
				RDPGFX_CAPS_CONFIRM_PDU pdu;
				pdu.capsSet = &caps;
				flags = pdu.capsSet->flags;

				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxAVC444v2, FALSE))
					return rc;
				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxAVC444, FALSE))
					return rc;
				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxH264, FALSE))
					return rc;

				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxThinClient,
				                               (flags & RDPGFX_CAPS_FLAG_THINCLIENT) ? TRUE
				                                                                     : FALSE))
					return rc;
				if (!freerdp_settings_set_bool(clientSettings, FreeRDP_GfxSmallCache,
				                               (flags & RDPGFX_CAPS_FLAG_SMALL_CACHE) ? TRUE
				                                                                      : FALSE))
					return rc;

				return shadow_client_send_caps_confirm(self,context, &pdu);
			}
		}
	}

	return CHANNEL_RC_UNSUPPORTED_VERSION;
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
        if (self->last_frame_h264)
            self->outstanding_frames = 0;
        else
            self->outstanding_frames--;
    }
    g_cond_broadcast(&self->capacity_cond);
    g_mutex_unlock(&self->lock);

    return CHANNEL_RC_OK;
}

RdpgfxServerContext* drd_rdpgfx_get_context(DrdRdpGraphicsPipeline *self)
{
    return self->rdpgfx_context;
}
void drd_rdp_graphics_pipeline_set_last_frame_mode(DrdRdpGraphicsPipeline *self, gboolean h264)
{
    g_mutex_lock(&self->lock);
    self->last_frame_h264 = h264;
    g_mutex_unlock(&self->lock);
}
