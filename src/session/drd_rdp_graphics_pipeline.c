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

    gint outstanding_frames;
    guint max_outstanding_frames;
    guint32 channel_id;

    GMutex lock;
};

G_DEFINE_TYPE(DrdRdpGraphicsPipeline, drd_rdp_graphics_pipeline, G_TYPE_OBJECT)

static BOOL drd_rdpgfx_channel_assigned(RdpgfxServerContext *context, UINT32 channel_id);
static UINT drd_rdpgfx_caps_advertise(RdpgfxServerContext *context,
                                       const RDPGFX_CAPS_ADVERTISE_PDU *caps);
static UINT drd_rdpgfx_frame_ack(RdpgfxServerContext *context,
                                  const RDPGFX_FRAME_ACKNOWLEDGE_PDU *ack);

static guint32
drd_rdp_graphics_pipeline_build_timestamp(void)
{
    guint32 timestamp = 0;
    GDateTime *now = g_date_time_new_now_local();

    if (now != NULL)
    {
        timestamp = ((guint32)g_date_time_get_hour(now) << 22) |
                    ((guint32)g_date_time_get_minute(now) << 16) |
                    ((guint32)g_date_time_get_second(now) << 10) |
                    ((guint32)(g_date_time_get_microsecond(now) / 1000));
        g_date_time_unref(now);
    }

    return timestamp;
}

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
    return TRUE;
}

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
        }

        if (self->channel_opened && self->rdpgfx_context->Close)
        {
            self->rdpgfx_context->Close(self->rdpgfx_context);
            self->channel_opened = FALSE;
        }
    }

    G_OBJECT_CLASS(drd_rdp_graphics_pipeline_parent_class)->dispose(object);
}

static void
drd_rdp_graphics_pipeline_finalize(GObject *object)
{
    DrdRdpGraphicsPipeline *self = DRD_RDP_GRAPHICS_PIPELINE(object);

    g_mutex_clear(&self->lock);
    g_clear_pointer(&self->rdpgfx_context, rdpgfx_server_context_free);

    G_OBJECT_CLASS(drd_rdp_graphics_pipeline_parent_class)->finalize(object);
}

static void
drd_rdp_graphics_pipeline_init(DrdRdpGraphicsPipeline *self)
{
    g_mutex_init(&self->lock);
    self->surface_id = 1;
    self->codec_context_id = 1;
    self->next_frame_id = 1;
    self->max_outstanding_frames = 3;
}

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

gboolean
drd_rdp_graphics_pipeline_is_ready(DrdRdpGraphicsPipeline *self)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), FALSE);

    g_mutex_lock(&self->lock);
    gboolean ready = self->surface_ready;
    g_mutex_unlock(&self->lock);
    return ready;
}

gboolean
drd_rdp_graphics_pipeline_can_submit(DrdRdpGraphicsPipeline *self)
{
    g_return_val_if_fail(DRD_IS_RDP_GRAPHICS_PIPELINE(self), FALSE);

    g_mutex_lock(&self->lock);
    gboolean ok = self->surface_ready &&
                  self->outstanding_frames < (gint)self->max_outstanding_frames;
    g_mutex_unlock(&self->lock);
    return ok;
}

gboolean
drd_rdp_graphics_pipeline_submit_frame(DrdRdpGraphicsPipeline *self,
                                        DrdEncodedFrame *frame,
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

    if (self->outstanding_frames >= (gint)self->max_outstanding_frames)
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
    self->outstanding_frames++;
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
    cmd.length = (UINT32)payload_size;
    cmd.data = (BYTE *)payload;

    if (!self->rdpgfx_context->StartFrame ||
        self->rdpgfx_context->StartFrame(self->rdpgfx_context, &start) != CHANNEL_RC_OK ||
        !self->rdpgfx_context->SurfaceCommand ||
        self->rdpgfx_context->SurfaceCommand(self->rdpgfx_context, &cmd) != CHANNEL_RC_OK ||
        !self->rdpgfx_context->EndFrame ||
        self->rdpgfx_context->EndFrame(self->rdpgfx_context, &end) != CHANNEL_RC_OK)
    {
        g_mutex_lock(&self->lock);
        if (self->outstanding_frames > 0)
        {
            self->outstanding_frames--;
        }
        g_mutex_unlock(&self->lock);

        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to submit frame over Rdpgfx");
        return FALSE;
    }

    return TRUE;
}

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
    if (self->outstanding_frames > 0)
    {
        self->outstanding_frames--;
    }
    g_mutex_unlock(&self->lock);

    return CHANNEL_RC_OK;
}
