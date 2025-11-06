#include "session/grdc_rdp_session.h"

#include <freerdp/freerdp.h>
#include <freerdp/update.h>
#include <freerdp/codec/bitmap.h>
#include <freerdp/constants.h>

#include <gio/gio.h>
#include <string.h>

#include <winpr/synch.h>

#include "core/grdc_server_runtime.h"

struct _GrdcRdpSession
{
    GObject parent_instance;

    freerdp_peer *peer;
    gchar *peer_address;
    gchar *state;
    GrdcServerRuntime *runtime;
    guint32 frame_sequence;
    gboolean is_activated;
    GThread *event_thread;
    HANDLE stop_event;
    gint connection_alive;
};

G_DEFINE_TYPE(GrdcRdpSession, grdc_rdp_session, G_TYPE_OBJECT)

static gboolean grdc_rdp_session_send_surface_bits(GrdcRdpSession *self,
                                                   GrdcEncodedFrame *frame,
                                                   guint32 frame_id,
                                                   UINT32 negotiated_max_payload,
                                                   GError **error);
static gpointer grdc_rdp_session_event_thread(gpointer user_data);
static void grdc_rdp_session_enforce_peer_desktop_size(GrdcRdpSession *self);

static void
grdc_rdp_session_dispose(GObject *object)
{
    GrdcRdpSession *self = GRDC_RDP_SESSION(object);

    grdc_rdp_session_stop_event_thread(self);

    if (self->peer != NULL && self->peer->context != NULL)
    {
        /* Let FreeRDP manage the context lifecycle */
        self->peer = NULL;
    }

    g_clear_object(&self->runtime);

    G_OBJECT_CLASS(grdc_rdp_session_parent_class)->dispose(object);
}

static void
grdc_rdp_session_finalize(GObject *object)
{
    GrdcRdpSession *self = GRDC_RDP_SESSION(object);
    grdc_rdp_session_stop_event_thread(self);
    g_clear_pointer(&self->peer_address, g_free);
    g_clear_pointer(&self->state, g_free);
    G_OBJECT_CLASS(grdc_rdp_session_parent_class)->finalize(object);
}

static void
grdc_rdp_session_class_init(GrdcRdpSessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = grdc_rdp_session_dispose;
    object_class->finalize = grdc_rdp_session_finalize;
}

static void
grdc_rdp_session_init(GrdcRdpSession *self)
{
    self->peer = NULL;
    self->peer_address = g_strdup("unknown");
    self->state = g_strdup("created");
    self->runtime = NULL;
    self->frame_sequence = 1;
    self->is_activated = FALSE;
    self->event_thread = NULL;
    self->stop_event = NULL;
    g_atomic_int_set(&self->connection_alive, 1);
}

GrdcRdpSession *
grdc_rdp_session_new(freerdp_peer *peer)
{
    g_return_val_if_fail(peer != NULL, NULL);

    GrdcRdpSession *self = g_object_new(GRDC_TYPE_RDP_SESSION, NULL);
    self->peer = peer;
    g_clear_pointer(&self->peer_address, g_free);
    self->peer_address = g_strdup(peer->hostname != NULL ? peer->hostname : "unknown");
    return self;
}

void
grdc_rdp_session_set_peer_state(GrdcRdpSession *self, const gchar *state)
{
    g_return_if_fail(GRDC_IS_RDP_SESSION(self));

    g_clear_pointer(&self->state, g_free);
    self->state = g_strdup(state != NULL ? state : "unknown");

    g_message("Session %s transitioned to state %s", self->peer_address, self->state);
}

void
grdc_rdp_session_set_runtime(GrdcRdpSession *self, GrdcServerRuntime *runtime)
{
    g_return_if_fail(GRDC_IS_RDP_SESSION(self));
    g_return_if_fail(runtime == NULL || GRDC_IS_SERVER_RUNTIME(runtime));

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
}

BOOL
grdc_rdp_session_post_connect(GrdcRdpSession *self)
{
    g_return_val_if_fail(GRDC_IS_RDP_SESSION(self), FALSE);
    grdc_rdp_session_set_peer_state(self, "post-connect");
    return TRUE;
}

BOOL
grdc_rdp_session_activate(GrdcRdpSession *self)
{
    g_return_val_if_fail(GRDC_IS_RDP_SESSION(self), FALSE);
    grdc_rdp_session_set_peer_state(self, "activated");
    self->is_activated = TRUE;
    grdc_rdp_session_enforce_peer_desktop_size(self);
    if (self->runtime != NULL)
    {
        grdc_server_runtime_request_keyframe(self->runtime);
    }
    grdc_rdp_session_start_event_thread(self);
    return TRUE;
}

gboolean
grdc_rdp_session_start_event_thread(GrdcRdpSession *self)
{
    g_return_val_if_fail(GRDC_IS_RDP_SESSION(self), FALSE);

    if (self->event_thread != NULL)
    {
        return TRUE;
    }

    if (self->peer == NULL)
    {
        return FALSE;
    }

    if (self->stop_event == NULL)
    {
        self->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (self->stop_event == NULL)
        {
            g_warning("Session %s failed to create stop event", self->peer_address);
            return FALSE;
        }
    }

    g_atomic_int_set(&self->connection_alive, 1);
    self->event_thread = g_thread_new("grdc-rdp-io", grdc_rdp_session_event_thread, g_object_ref(self));
    return TRUE;
}

void
grdc_rdp_session_stop_event_thread(GrdcRdpSession *self)
{
    g_return_if_fail(GRDC_IS_RDP_SESSION(self));

    if (self->event_thread != NULL)
    {
        if (self->stop_event != NULL)
        {
            SetEvent(self->stop_event);
        }
        g_thread_join(self->event_thread);
        self->event_thread = NULL;
    }

    if (self->stop_event != NULL)
    {
        CloseHandle(self->stop_event);
        self->stop_event = NULL;
    }

    g_atomic_int_set(&self->connection_alive, 0);
}

BOOL
grdc_rdp_session_pump(GrdcRdpSession *self)
{
    g_return_val_if_fail(GRDC_IS_RDP_SESSION(self), FALSE);
    if (self->peer == NULL)
    {
        return FALSE;
    }

    if (!g_atomic_int_get(&self->connection_alive))
    {
        g_message("Session %s connection closed", self->peer_address);
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

    GrdcEncodedFrame *encoded = NULL;
    g_autoptr(GError) error = NULL;
    if (!grdc_server_runtime_pull_encoded_frame(self->runtime,
                                                16 * 1000, /* 16ms */
                                                (gsize)negotiated_max_payload,
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
            g_warning("Session %s failed to pull encoded frame: %s", self->peer_address, error->message);
        }
        return TRUE;
    }

    g_autoptr(GrdcEncodedFrame) owned_frame = encoded;

    g_autoptr(GError) send_error = NULL;
    if (!grdc_rdp_session_send_surface_bits(self,
                                            owned_frame,
                                            self->frame_sequence,
                                            negotiated_max_payload,
                                            &send_error))
    {
        if (send_error != NULL)
        {
            g_warning("Session %s failed to send frame: %s", self->peer_address, send_error->message);
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
grdc_rdp_session_disconnect(GrdcRdpSession *self, const gchar *reason)
{
    g_return_if_fail(GRDC_IS_RDP_SESSION(self));

    if (reason != NULL)
    {
        g_message("Disconnecting session %s: %s", self->peer_address, reason);
    }

    grdc_rdp_session_stop_event_thread(self);

    if (self->peer != NULL && self->peer->Disconnect != NULL)
    {
        self->peer->Disconnect(self->peer);
        self->peer = NULL;
    }

    self->is_activated = FALSE;
}

static gboolean
grdc_rdp_session_send_surface_bits(GrdcRdpSession *self,
                                   GrdcEncodedFrame *frame,
                                   guint32 frame_id,
                                   UINT32 negotiated_max_payload,
                                   GError **error);

static gpointer
grdc_rdp_session_event_thread(gpointer user_data)
{
    GrdcRdpSession *self = GRDC_RDP_SESSION(user_data);
    freerdp_peer *peer = self->peer;

    if (peer == NULL)
    {
        g_atomic_int_set(&self->connection_alive, 0);
        g_object_unref(self);
        return NULL;
    }

    while (TRUE)
    {
        HANDLE handles[64];
        DWORD count = 0;

        if (self->stop_event != NULL)
        {
            handles[count++] = self->stop_event;
        }

        DWORD obtained = 0;
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
            g_message("Session %s CheckFileDescriptor failed in event thread", self->peer_address);
            g_atomic_int_set(&self->connection_alive, 0);
            break;
        }
    }

    g_object_unref(self);
    return NULL;
}

static void
grdc_rdp_session_enforce_peer_desktop_size(GrdcRdpSession *self)
{
    g_return_if_fail(GRDC_IS_RDP_SESSION(self));

    if (self->peer == NULL || self->peer->context == NULL || self->runtime == NULL)
    {
        return;
    }

    GrdcEncodingOptions encoding_opts;
    if (!grdc_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        return;
    }

    rdpContext *context = self->peer->context;
    rdpSettings *settings = context->settings;
    if (settings == NULL)
    {
        return;
    }

    const guint32 desired_width = encoding_opts.width;
    const guint32 desired_height = encoding_opts.height;
    gboolean updated = FALSE;

    if (freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) != desired_width)
    {
        if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, desired_width))
        {
            g_warning("Session %s could not update DesktopWidth to %u",
                      self->peer_address,
                      desired_width);
            return;
        }
        updated = TRUE;
    }

    if (freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) != desired_height)
    {
        if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, desired_height))
        {
            g_warning("Session %s could not update DesktopHeight to %u",
                      self->peer_address,
                      desired_height);
            return;
        }
        updated = TRUE;
    }

    if (!updated)
    {
        return;
    }

    rdpUpdate *update = context->update;
    if (update == NULL || update->DesktopResize == NULL)
    {
        return;
    }

    if (!update->DesktopResize(context))
    {
        g_warning("Session %s failed to notify DesktopResize", self->peer_address);
        return;
    }

    g_message("Session %s enforced desktop resolution to %ux%u",
              self->peer_address,
              desired_width,
              desired_height);
}

static gboolean
grdc_rdp_session_send_surface_bits(GrdcRdpSession *self,
                                   GrdcEncodedFrame *frame,
                                   guint32 frame_id,
                                   UINT32 negotiated_max_payload,
                                   GError **error)
{
    g_return_val_if_fail(GRDC_IS_RDP_SESSION(self), FALSE);
    g_return_val_if_fail(GRDC_IS_ENCODED_FRAME(frame), FALSE);

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

    guint width = grdc_encoded_frame_get_width(frame);
    guint height = grdc_encoded_frame_get_height(frame);
    gsize data_size = 0;
    const guint8 *data = grdc_encoded_frame_get_data(frame, &data_size);
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

    const gsize stride = grdc_encoded_frame_get_stride(frame);
    const gboolean bottom_up = grdc_encoded_frame_get_is_bottom_up(frame);
    GrdcFrameCodec codec = grdc_encoded_frame_get_codec(frame);

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

    if (codec == GRDC_FRAME_CODEC_RFX)
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
