#include "session/drd_rdp_session.h"

#include <freerdp/freerdp.h>
#include <freerdp/update.h>
#include <freerdp/codec/bitmap.h>
#include <freerdp/constants.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/wtsvc.h>

#include <gio/gio.h>
#include <string.h>

#include <winpr/synch.h>
#include <winpr/wtypes.h>

#include "core/drd_server_runtime.h"
#include "utils/drd_log.h"
#include "session/drd_rdp_graphics_pipeline.h"

struct _DrdRdpSession
{
    GObject parent_instance;

    freerdp_peer *peer;
    gchar *peer_address;
    gchar *state;
    DrdServerRuntime *runtime;
    HANDLE vcm;
    GThread *vcm_thread;
    DrdRdpGraphicsPipeline *graphics_pipeline;
    gboolean graphics_pipeline_ready;
    guint32 frame_sequence;
    gboolean is_activated;
    GThread *event_thread;
    HANDLE stop_event;
    gint connection_alive;
};

G_DEFINE_TYPE(DrdRdpSession, drd_rdp_session, G_TYPE_OBJECT)

static gboolean drd_rdp_session_send_surface_bits(DrdRdpSession *self,
                                                   DrdEncodedFrame *frame,
                                                   guint32 frame_id,
                                                   UINT32 negotiated_max_payload,
                                                   GError **error);
static gpointer drd_rdp_session_vcm_thread(gpointer user_data);
static gboolean drd_rdp_session_try_submit_graphics(DrdRdpSession *self,
                                                    DrdEncodedFrame *frame);
static void drd_rdp_session_maybe_init_graphics(DrdRdpSession *self);
static void drd_rdp_session_disable_graphics_pipeline(DrdRdpSession *self,
                                                      const gchar *reason);
static gpointer drd_rdp_session_event_thread(gpointer user_data);
static gboolean drd_rdp_session_enforce_peer_desktop_size(DrdRdpSession *self);

static void
drd_rdp_session_dispose(GObject *object)
{
    DrdRdpSession *self = DRD_RDP_SESSION(object);

    drd_rdp_session_stop_event_thread(self);
    drd_rdp_session_disable_graphics_pipeline(self, NULL);
    if (self->vcm_thread != NULL)
    {
        g_thread_join(self->vcm_thread);
        self->vcm_thread = NULL;
    }

    if (self->peer != NULL && self->peer->context != NULL)
    {
        /* Let FreeRDP manage the context lifecycle */
        self->peer = NULL;
    }

    g_clear_object(&self->runtime);

    G_OBJECT_CLASS(drd_rdp_session_parent_class)->dispose(object);
}

static void
drd_rdp_session_finalize(GObject *object)
{
    DrdRdpSession *self = DRD_RDP_SESSION(object);
    drd_rdp_session_stop_event_thread(self);
    g_clear_pointer(&self->peer_address, g_free);
    g_clear_pointer(&self->state, g_free);
    g_clear_object(&self->graphics_pipeline);
    G_OBJECT_CLASS(drd_rdp_session_parent_class)->finalize(object);
}

static void
drd_rdp_session_class_init(DrdRdpSessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_rdp_session_dispose;
    object_class->finalize = drd_rdp_session_finalize;
}

static void
drd_rdp_session_init(DrdRdpSession *self)
{
    self->peer = NULL;
    self->peer_address = g_strdup("unknown");
    self->state = g_strdup("created");
    self->runtime = NULL;
    self->vcm_thread = NULL;
    self->vcm = INVALID_HANDLE_VALUE;
    self->graphics_pipeline = NULL;
    self->graphics_pipeline_ready = FALSE;
    self->frame_sequence = 1;
    self->is_activated = FALSE;
    self->event_thread = NULL;
    self->stop_event = NULL;
    g_atomic_int_set(&self->connection_alive, 1);
}

DrdRdpSession *
drd_rdp_session_new(freerdp_peer *peer)
{
    g_return_val_if_fail(peer != NULL, NULL);

    DrdRdpSession *self = g_object_new(DRD_TYPE_RDP_SESSION, NULL);
    self->peer = peer;
    g_clear_pointer(&self->peer_address, g_free);
    self->peer_address = g_strdup(peer->hostname != NULL ? peer->hostname : "unknown");
    return self;
}

void
drd_rdp_session_set_peer_state(DrdRdpSession *self, const gchar *state)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    g_clear_pointer(&self->state, g_free);
    self->state = g_strdup(state != NULL ? state : "unknown");

    DRD_LOG_MESSAGE("Session %s transitioned to state %s", self->peer_address, self->state);
}

void
drd_rdp_session_set_runtime(DrdRdpSession *self, DrdServerRuntime *runtime)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    g_return_if_fail(runtime == NULL || DRD_IS_SERVER_RUNTIME(runtime));

    if (self->runtime == runtime)
    {
        return;
    }

    if (runtime != NULL)
    {
        g_object_ref(runtime);
    }

    g_clear_object(&self->runtime);
    self->runtime = runtime;

    drd_rdp_session_maybe_init_graphics(self);
}

void
drd_rdp_session_set_virtual_channel_manager(DrdRdpSession *self, HANDLE vcm)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    self->vcm = vcm;
    drd_rdp_session_maybe_init_graphics(self);
}

BOOL
drd_rdp_session_post_connect(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    drd_rdp_session_set_peer_state(self, "post-connect");
    return TRUE;
}

BOOL
drd_rdp_session_activate(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);

    if (!drd_rdp_session_enforce_peer_desktop_size(self))
    {
        drd_rdp_session_set_peer_state(self, "desktop-resize-blocked");
        drd_rdp_session_disconnect(self, "client does not support desktop resize");
        return FALSE;
    }

    drd_rdp_session_set_peer_state(self, "activated");
    self->is_activated = TRUE;
    if (self->runtime != NULL)
    {
        drd_server_runtime_request_keyframe(self->runtime);
    }
    drd_rdp_session_start_event_thread(self);
    return TRUE;
}

gboolean
drd_rdp_session_start_event_thread(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);

    if (self->peer == NULL)
    {
        return FALSE;
    }

    if (self->event_thread != NULL)
    {
        return TRUE;
    }
    if (self->stop_event == NULL)
    {
        self->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (self->stop_event == NULL)
        {
            DRD_LOG_WARNING("Session %s failed to create stop event", self->peer_address);
            return FALSE;
        }
    }

    g_atomic_int_set(&self->connection_alive, 1);
    self->event_thread = g_thread_new("drd-rdp-io", drd_rdp_session_event_thread, g_object_ref(self));
    if (self->event_thread != NULL)
    {
        DRD_LOG_MESSAGE("Session %s started event thread", self->peer_address);
    }

    if (self->vcm != NULL && self->vcm != INVALID_HANDLE_VALUE && self->vcm_thread == NULL)
    {
        self->vcm_thread = g_thread_new("drd-rdp-vcm", drd_rdp_session_vcm_thread, g_object_ref(self));
    }

    return TRUE;
}

void
drd_rdp_session_stop_event_thread(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->event_thread != NULL)
    {
        if (self->stop_event != NULL)
        {
            SetEvent(self->stop_event);
        }
        g_thread_join(self->event_thread);
        self->event_thread = NULL;
        DRD_LOG_MESSAGE("Session %s stopped event thread", self->peer_address);
    }

    if (self->stop_event != NULL)
    {
        CloseHandle(self->stop_event);
        self->stop_event = NULL;
    }

    g_atomic_int_set(&self->connection_alive, 0);

    if (self->vcm_thread != NULL)
    {
        g_thread_join(self->vcm_thread);
        self->vcm_thread = NULL;
    }
}

BOOL
drd_rdp_session_pump(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    if (self->peer == NULL)
    {
        return FALSE;
    }

    if (!g_atomic_int_get(&self->connection_alive))
    {
        DRD_LOG_MESSAGE("Session %s connection closed", self->peer_address);
        return FALSE;
    }

    if (self->runtime == NULL)
    {
        return TRUE;
    }

    if (!self->is_activated)
    {
        return TRUE;
    }

    UINT32 negotiated_max_payload = 0;
    if (self->peer->context != NULL && self->peer->context->settings != NULL)
    {
        negotiated_max_payload = freerdp_settings_get_uint32(self->peer->context->settings,
                                                             FreeRDP_MultifragMaxRequestSize);
    }

    DrdEncodedFrame *encoded = NULL;
    g_autoptr(GError) error = NULL;
    if (!drd_server_runtime_pull_encoded_frame(self->runtime,
                                                16 * 1000, /* 16ms */
                                                &encoded,
                                                &error))
    {
        if (error != NULL && error->domain == G_IO_ERROR && error->code == G_IO_ERROR_TIMED_OUT)
        {
            g_clear_error(&error);
            return TRUE;
        }

        if (error != NULL)
        {
            DRD_LOG_WARNING("Session %s failed to pull encoded frame: %s", self->peer_address, error->message);
        }
        return TRUE;
    }

    g_autoptr(DrdEncodedFrame) owned_frame = encoded;

    gboolean sent_via_graphics = drd_rdp_session_try_submit_graphics(self, owned_frame);
    if (!sent_via_graphics)
    {
        g_autoptr(GError) send_error = NULL;
        if (!drd_rdp_session_send_surface_bits(self,
                                                owned_frame,
                                                self->frame_sequence,
                                                negotiated_max_payload,
                                                &send_error))
        {
            if (send_error != NULL)
            {
                DRD_LOG_WARNING("Session %s failed to send frame: %s",
                                self->peer_address,
                                send_error->message);
            }
        }
    }

    self->frame_sequence++;
    if (self->frame_sequence == 0)
    {
        self->frame_sequence = 1;
    }

    return TRUE;
}

void
drd_rdp_session_disconnect(DrdRdpSession *self, const gchar *reason)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (reason != NULL)
    {
        DRD_LOG_MESSAGE("Disconnecting session %s: %s", self->peer_address, reason);
    }

    drd_rdp_session_stop_event_thread(self);
    drd_rdp_session_disable_graphics_pipeline(self, NULL);

    if (self->peer != NULL && self->peer->Disconnect != NULL)
    {
        self->peer->Disconnect(self->peer);
        self->peer = NULL;
    }

    self->is_activated = FALSE;
}

static gboolean
drd_rdp_session_send_surface_bits(DrdRdpSession *self,
                                   DrdEncodedFrame *frame,
                                   guint32 frame_id,
                                   UINT32 negotiated_max_payload,
                                   GError **error);

static gpointer
drd_rdp_session_event_thread(gpointer user_data)
{
    DrdRdpSession *self = DRD_RDP_SESSION(user_data);
    freerdp_peer *peer = self->peer;
    HANDLE vcm = self->vcm;
    HANDLE vcm_event = NULL;

    if (vcm != NULL && vcm != INVALID_HANDLE_VALUE)
    {
        vcm_event = WTSVirtualChannelManagerGetEventHandle(vcm);
    }

    if (peer == NULL)
    {
        g_atomic_int_set(&self->connection_alive, 0);
        g_object_unref(self);
        return NULL;
    }

    /* 循环等待 stop 事件与 FreeRDP I/O 句柄，确保编码/输入线程协同推进。 */
    while (TRUE)
    {
        HANDLE handles[64];
        DWORD count = 0;

        if (self->stop_event != NULL)
        {
            handles[count++] = self->stop_event;
        }

        DWORD obtained = 0;

        if (vcm_event != NULL)
        {
            handles[count++] = vcm_event;
        }

        if (peer->GetEventHandles != NULL)
        {
            obtained = peer->GetEventHandles(peer, &handles[count], G_N_ELEMENTS(handles) - count);
        }

        if (obtained == 0)
        {
            g_atomic_int_set(&self->connection_alive, 0);
            break;
        }

        count += obtained;

        DWORD status = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
        if (status == WAIT_FAILED)
        {
            g_atomic_int_set(&self->connection_alive, 0);
            break;
        }

        if (self->stop_event != NULL && status == WAIT_OBJECT_0)
        {
            break;
        }

        if (!peer->CheckFileDescriptor(peer))
        {
            DRD_LOG_MESSAGE("Session %s CheckFileDescriptor failed in event thread", self->peer_address);
            g_atomic_int_set(&self->connection_alive, 0);
            break;
        }
    }

    g_object_unref(self);
    return NULL;
}

static gpointer
drd_rdp_session_vcm_thread(gpointer user_data)
{
    DrdRdpSession *self = DRD_RDP_SESSION(user_data);
    freerdp_peer *peer = self->peer;
    HANDLE vcm = self->vcm;
    HANDLE channel_event = NULL;

    if (vcm == NULL || vcm == INVALID_HANDLE_VALUE || peer == NULL)
    {
        g_object_unref(self);
        return NULL;
    }

    channel_event = WTSVirtualChannelManagerGetEventHandle(vcm);

    while (g_atomic_int_get(&self->connection_alive))
    {
        HANDLE handles[2];
        DWORD count = 0;

        if (self->stop_event != NULL)
        {
            handles[count++] = self->stop_event;
        }
        if (channel_event != NULL)
        {
            handles[count++] = channel_event;
        }

        DWORD status = WAIT_TIMEOUT;
        if (count > 0)
        {
            status = WaitForMultipleObjects(count, handles, FALSE, 100);
        }

        if (status == WAIT_FAILED)
        {
            break;
        }

        if (self->stop_event != NULL && status == WAIT_OBJECT_0)
        {
            break;
        }



        if (!peer->connected)
        {
            continue;
        }

        if (!WTSVirtualChannelManagerIsChannelJoined(vcm, DRDYNVC_SVC_CHANNEL_NAME))
        {
            continue;
        }

        BYTE drdynvc_state = WTSVirtualChannelManagerGetDrdynvcState(vcm);
        if (drdynvc_state == DRDYNVC_STATE_NONE)
        {
            if (channel_event != NULL)
            {
                SetEvent(channel_event);
            }
            continue;
        }

        if (drdynvc_state == DRDYNVC_STATE_READY &&
            self->graphics_pipeline != NULL && !self->graphics_pipeline_ready)
        {
            drd_rdp_graphics_pipeline_maybe_init(self->graphics_pipeline);
        }

        if (channel_event != NULL &&
            WaitForSingleObject(channel_event, 0) == WAIT_OBJECT_0)
        {
            if (!WTSVirtualChannelManagerCheckFileDescriptor(vcm))
            {
                DRD_LOG_MESSAGE("Session %s failed to check VCM descriptor", self->peer_address);
                break;
            }
        }
    }

    g_object_unref(self);
    return NULL;
}

static gboolean
drd_rdp_session_enforce_peer_desktop_size(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), TRUE);

    if (self->peer == NULL || self->peer->context == NULL || self->runtime == NULL)
    {
        return TRUE;
    }

    DrdEncodingOptions encoding_opts;
    if (!drd_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        return TRUE;
    }

    rdpContext *context = self->peer->context;
    rdpSettings *settings = context->settings;
    if (settings == NULL)
    {
        return TRUE;
    }

    const guint32 desired_width = encoding_opts.width;
    const guint32 desired_height = encoding_opts.height;
    const guint32 client_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const guint32 client_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    const gboolean client_allows_resize = freerdp_settings_get_bool(settings, FreeRDP_DesktopResize);

    DRD_LOG_MESSAGE("Session %s peer geometry %ux%u, server requires %ux%u",
              self->peer_address,
              client_width,
              client_height,
              desired_width,
              desired_height);

    if (!client_allows_resize && (client_width != desired_width || client_height != desired_height))
    {
        DRD_LOG_WARNING("Session %s client did not advertise DesktopResize, cannot override %ux%u with %ux%u",
                  self->peer_address,
                  client_width,
                  client_height,
                  desired_width,
                  desired_height);
        return FALSE;
    }

    gboolean updated = FALSE;

    if (client_width != desired_width)
    {
        if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, desired_width))
        {
            DRD_LOG_WARNING("Session %s could not update DesktopWidth to %u",
                      self->peer_address,
                      desired_width);
            return FALSE;
        }
        updated = TRUE;
    }

    if (client_height != desired_height)
    {
        if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, desired_height))
        {
            DRD_LOG_WARNING("Session %s could not update DesktopHeight to %u",
                      self->peer_address,
                      desired_height);
            return FALSE;
        }
        updated = TRUE;
    }

    if (!updated)
    {
        return TRUE;
    }

    rdpUpdate *update = context->update;
    if (update == NULL || update->DesktopResize == NULL)
    {
        DRD_LOG_WARNING("Session %s missing DesktopResize callback, cannot synchronize geometry", self->peer_address);
        return FALSE;
    }

    if (!update->DesktopResize(context))
    {
        DRD_LOG_WARNING("Session %s failed to notify DesktopResize", self->peer_address);
        return FALSE;
    }

    DRD_LOG_MESSAGE("Session %s enforced desktop resolution to %ux%u",
              self->peer_address,
              desired_width,
              desired_height);
    return TRUE;
}

static void
drd_rdp_session_maybe_init_graphics(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->graphics_pipeline != NULL || self->peer == NULL ||
        self->peer->context == NULL || self->runtime == NULL ||
        self->vcm == NULL || self->vcm == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DrdEncodingOptions encoding_opts;
    if (!drd_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        return;
    }

    if (encoding_opts.mode != DRD_ENCODING_MODE_RFX)
    {
        return;
    }

    DrdRdpGraphicsPipeline *pipeline =
        drd_rdp_graphics_pipeline_new(self->peer,
                                      self->vcm,
                                      (guint16)encoding_opts.width,
                                      (guint16)encoding_opts.height);
    if (pipeline == NULL)
    {
        DRD_LOG_WARNING("Session %s failed to allocate graphics pipeline", self->peer_address);
        return;
    }

    self->graphics_pipeline = pipeline;
    self->graphics_pipeline_ready = FALSE;
    DRD_LOG_MESSAGE("Session %s graphics pipeline created", self->peer_address);
}

static void
drd_rdp_session_disable_graphics_pipeline(DrdRdpSession *self, const gchar *reason)
{
    if (self->graphics_pipeline == NULL)
    {
        return;
    }

    if (reason != NULL)
    {
        DRD_LOG_WARNING("Session %s disabling graphics pipeline: %s",
                        self->peer_address,
                        reason);
    }

    if (self->runtime != NULL)
    {
        drd_server_runtime_set_transport(self->runtime, DRD_FRAME_TRANSPORT_SURFACE_BITS);
    }

    self->graphics_pipeline_ready = FALSE;
    g_clear_object(&self->graphics_pipeline);
}

static gboolean
drd_rdp_session_try_submit_graphics(DrdRdpSession *self, DrdEncodedFrame *frame)
{
    if (self->runtime == NULL)
    {
        return FALSE;
    }

    drd_rdp_session_maybe_init_graphics(self);

    if (self->graphics_pipeline == NULL)
    {
        return FALSE;
    }

    // drd_rdp_graphics_pipeline_maybe_init(self->graphics_pipeline);

    if (!self->graphics_pipeline_ready &&
        drd_rdp_graphics_pipeline_is_ready(self->graphics_pipeline))
    {
        drd_server_runtime_set_transport(self->runtime,
                                         DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE);
        self->graphics_pipeline_ready = TRUE;
        DRD_LOG_MESSAGE("Session %s graphics pipeline ready, switching to RFX progressive",
                        self->peer_address);
    }

    if (!self->graphics_pipeline_ready)
    {
        return FALSE;
    }

    if (drd_encoded_frame_get_codec(frame) != DRD_FRAME_CODEC_RFX_PROGRESSIVE)
    {
        return FALSE;
    }

    if (!drd_rdp_graphics_pipeline_can_submit(self->graphics_pipeline))
    {
        return FALSE;
    }

    g_autoptr(GError) gfx_error = NULL;
    if (!drd_rdp_graphics_pipeline_submit_frame(self->graphics_pipeline, frame, &gfx_error))
    {
        drd_rdp_session_disable_graphics_pipeline(self,
                                                  gfx_error != NULL ? gfx_error->message
                                                                    : "submission failure");
        return FALSE;
    }

    return TRUE;
}

static gboolean
drd_rdp_session_send_surface_bits(DrdRdpSession *self,
                                   DrdEncodedFrame *frame,
                                   guint32 frame_id,
                                   UINT32 negotiated_max_payload,
                                   GError **error)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    g_return_val_if_fail(DRD_IS_ENCODED_FRAME(frame), FALSE);

    if (self->peer == NULL || self->peer->context == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Peer context not available");
        return FALSE;
    }

    rdpContext *context = self->peer->context;
    rdpUpdate *update = context->update;
    if (update == NULL || update->SurfaceBits == NULL || update->SurfaceFrameMarker == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "Surface update callbacks are not available");
        return FALSE;
    }

    guint width = drd_encoded_frame_get_width(frame);
    guint height = drd_encoded_frame_get_height(frame);
    gsize data_size = 0;
    const guint8 *data = drd_encoded_frame_get_data(frame, &data_size);
    if (data == NULL || data_size == 0)
    {
        SURFACE_FRAME_MARKER marker_begin = {SURFACECMD_FRAMEACTION_BEGIN, frame_id};
        SURFACE_FRAME_MARKER marker_end = {SURFACECMD_FRAMEACTION_END, frame_id};

        if (!update->SurfaceFrameMarker(context, &marker_begin))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "SurfaceFrameMarker (begin) failed");
            return FALSE;
        }

        if (!update->SurfaceFrameMarker(context, &marker_end))
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "SurfaceFrameMarker (end) failed");
            return FALSE;
        }

        return TRUE;
    }

    const gsize stride = drd_encoded_frame_get_stride(frame);
    const gboolean bottom_up = drd_encoded_frame_get_is_bottom_up(frame);
    DrdFrameCodec codec = drd_encoded_frame_get_codec(frame);

    gsize payload_limit = (gsize)negotiated_max_payload;
    if (payload_limit > 0 && payload_limit < stride)
    {
        payload_limit = stride;
    }

    SURFACE_FRAME_MARKER marker_begin = {SURFACECMD_FRAMEACTION_BEGIN, frame_id};
    SURFACE_FRAME_MARKER marker_end = {SURFACECMD_FRAMEACTION_END, frame_id};

    gboolean frame_started = update->SurfaceFrameMarker(context, &marker_begin);
    if (!frame_started)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "SurfaceFrameMarker (begin) failed");
        return FALSE;
    }

    gboolean success = FALSE;

    if (codec == DRD_FRAME_CODEC_RFX)
    {
        if (payload_limit > 0 && data_size > payload_limit)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Encoded RFX frame exceeds negotiated payload limit (%zu > %u)",
                        data_size,
                        negotiated_max_payload);
            goto end_frame;
        }

        SURFACE_BITS_COMMAND cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
        cmd.destLeft = 0;
        cmd.destTop = 0;
        cmd.destRight = width;
        cmd.destBottom = height;
        cmd.skipCompression = FALSE;

        cmd.bmp.codecID = RDP_CODEC_ID_REMOTEFX;
        cmd.bmp.bpp = 32;
        cmd.bmp.flags = 0;
        cmd.bmp.width = (UINT16)width;
        cmd.bmp.height = (UINT16)height;
        cmd.bmp.bitmapData = (BYTE *)data;
        cmd.bmp.bitmapDataLength = (UINT32)data_size;

        success = update->SurfaceBits(context, &cmd);
    }
    else
    {
        /* 对 raw 帧执行按行分片，避免超出通道带宽的巨块推送。 */
        const gsize chunk_budget = (payload_limit > 0) ? payload_limit : (512 * 1024);
        guint rows_per_chunk = (guint)MAX((gsize)1, chunk_budget / stride);
        if (rows_per_chunk == 0)
        {
            rows_per_chunk = 1;
        }

        success = TRUE;
        for (guint top = 0; top < height; top += rows_per_chunk)
        {
            guint chunk_height = MIN(rows_per_chunk, height - top);
            gsize offset = bottom_up ? (gsize)stride * (height - top - chunk_height)
                                     : (gsize)stride * top;

            SURFACE_BITS_COMMAND cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
            cmd.destLeft = 0;
            cmd.destTop = top;
            cmd.destRight = width;
            cmd.destBottom = top + chunk_height;
            cmd.skipCompression = TRUE;

            cmd.bmp.bitmapData = (BYTE *)(data + offset);
            cmd.bmp.bitmapDataLength = (UINT32)(stride * chunk_height);
            cmd.bmp.bpp = 32;
            cmd.bmp.flags = 0;
            cmd.bmp.codecID = 0;
            cmd.bmp.width = (UINT16)width;
            cmd.bmp.height = (UINT16)chunk_height;

            if (!update->SurfaceBits(context, &cmd))
            {
                success = FALSE;
                break;
            }
        }
    }

end_frame:
    if (!success)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "SurfaceBits command failed");
        if (frame_started)
        {
            update->SurfaceFrameMarker(context, &marker_end);
        }
        return FALSE;
    }

    if (!update->SurfaceFrameMarker(context, &marker_end))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "SurfaceFrameMarker (end) failed");
        return FALSE;
    }

    return TRUE;
}
