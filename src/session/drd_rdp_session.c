#include "session/drd_rdp_session.h"

#include <freerdp/freerdp.h>
#include <freerdp/update.h>
#include <freerdp/codec/bitmap.h>
#include <freerdp/constants.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/redirection.h>
#include <freerdp/crypto/crypto.h>
#include <freerdp/crypto/certificate.h>

#include <gio/gio.h>
#include <string.h>

#include <winpr/synch.h>
#include <winpr/wtypes.h>
#include <winpr/crypto.h>
#include <winpr/stream.h>
#include <winpr/string.h>

#include "core/drd_server_runtime.h"
#include "utils/drd_log.h"
#include "session/drd_rdp_graphics_pipeline.h"
#include "security/drd_local_session.h"

#define ELEMENT_TYPE_CERTIFICATE 32

G_DEFINE_AUTOPTR_CLEANUP_FUNC(rdpCertificate, freerdp_certificate_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(rdpRedirection, redirection_free)

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
    GThread *render_thread;
    gint render_running;
    DrdRdpSessionClosedFunc closed_cb;
    gpointer closed_cb_data;
    gint closed_cb_invoked;
    DrdLocalSession *local_session;
    gboolean passive_mode;
    DrdRdpSessionError last_error;
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
static gboolean drd_rdp_session_enforce_peer_desktop_size(DrdRdpSession *self);
static gboolean drd_rdp_session_wait_for_graphics_capacity(DrdRdpSession *self,
                                                          gint64 timeout_us);
static gboolean drd_rdp_session_start_render_thread(DrdRdpSession *self);
static void drd_rdp_session_stop_render_thread(DrdRdpSession *self);
static gpointer drd_rdp_session_render_thread(gpointer user_data);
static void drd_rdp_session_notify_closed(DrdRdpSession *self);
gboolean drd_rdp_session_client_is_mstsc(DrdRdpSession *self);
static WCHAR *drd_rdp_session_get_utf16_string(const char *str, size_t *size);
static WCHAR *drd_rdp_session_generate_redirection_guid(size_t *size);
static BYTE *drd_rdp_session_get_certificate_container(const char *certificate, size_t *size);

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
    g_clear_pointer(&self->local_session, drd_local_session_close);

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
    self->render_thread = NULL;
    g_atomic_int_set(&self->render_running, 0);
    self->closed_cb = NULL;
    self->closed_cb_data = NULL;
    g_atomic_int_set(&self->closed_cb_invoked, 0);
    self->local_session = NULL;
    self->passive_mode = FALSE;
    self->last_error = DRD_RDP_SESSION_ERROR_NONE;
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

void
drd_rdp_session_set_closed_callback(DrdRdpSession *self,
                                     DrdRdpSessionClosedFunc callback,
                                     gpointer user_data)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    self->closed_cb = callback;
    self->closed_cb_data = user_data;

    if (callback == NULL)
    {
        g_atomic_int_set(&self->closed_cb_invoked, 0);
        return;
    }

    if (g_atomic_int_get(&self->connection_alive) == 0)
    {
        drd_rdp_session_notify_closed(self);
    }
}

void
drd_rdp_session_set_passive_mode(DrdRdpSession *self, gboolean passive)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    self->passive_mode = passive;
}

void
drd_rdp_session_attach_local_session(DrdRdpSession *self, DrdLocalSession *session)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (session == NULL)
    {
        return;
    }

    g_clear_pointer(&self->local_session, drd_local_session_close);
    self->local_session = session;
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

    if (self->passive_mode)
    {
        drd_rdp_session_set_peer_state(self, "activated-passive");
        self->is_activated = TRUE;
        DRD_LOG_MESSAGE("Session %s running in passive system mode, skipping transport", self->peer_address);
        return TRUE;
    }

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
    if (!drd_rdp_session_start_render_thread(self))
    {
        DRD_LOG_WARNING("Session %s failed to start renderer thread", self->peer_address);
    }
    // drd_rdp_session_start_event_thread(self);
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

    if (self->vcm != NULL && self->vcm != INVALID_HANDLE_VALUE && self->vcm_thread == NULL)
    {
        self->vcm_thread = g_thread_new("drd-rdp-vcm", drd_rdp_session_vcm_thread, self);
    }

    return TRUE;
}

static gboolean
drd_rdp_session_start_render_thread(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);

    if (self->render_thread != NULL)
    {
        return TRUE;
    }

    if (self->runtime == NULL)
    {
        return FALSE;
    }

    g_atomic_int_set(&self->render_running, 1);
    self->render_thread =
        g_thread_new("drd-render-thread", drd_rdp_session_render_thread, g_object_ref(self));
    if (self->render_thread == NULL)
    {
        g_atomic_int_set(&self->render_running, 0);
        return FALSE;
    }
    return TRUE;
}

static void
drd_rdp_session_stop_render_thread(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->render_thread == NULL)
    {
        return;
    }

    g_atomic_int_set(&self->render_running, 0);
    g_thread_join(self->render_thread);
    self->render_thread = NULL;
}

void
drd_rdp_session_stop_event_thread(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    drd_rdp_session_stop_render_thread(self);

    g_atomic_int_set(&self->connection_alive, 0);

    if (self->stop_event != NULL)
    {
        SetEvent(self->stop_event);
    }

    if (self->event_thread != NULL)
    {
        g_thread_join(self->event_thread);
        self->event_thread = NULL;
        DRD_LOG_MESSAGE("Session %s stopped event thread", self->peer_address);
    }

    if (self->vcm_thread != NULL)
    {
        g_thread_join(self->vcm_thread);
        self->vcm_thread = NULL;
    }

    if (self->stop_event != NULL)
    {
        CloseHandle(self->stop_event);
        self->stop_event = NULL;
    }

    drd_rdp_session_notify_closed(self);
}

static void
drd_rdp_session_notify_closed(DrdRdpSession *self)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));

    if (self->closed_cb == NULL)
    {
        return;
    }

    if (!g_atomic_int_compare_and_exchange(&self->closed_cb_invoked, 0, 1))
    {
        return;
    }

    self->closed_cb(self, self->closed_cb_data);
}

gboolean
drd_rdp_session_client_is_mstsc(DrdRdpSession *self)
{
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    if (self->peer == NULL || self->peer->context == NULL || self->peer->context->settings == NULL)
    {
        return FALSE;
    }

    rdpSettings *settings = self->peer->context->settings;
    const guint32 os_major = freerdp_settings_get_uint32(settings, FreeRDP_OsMajorType);
    const guint32 os_minor = freerdp_settings_get_uint32(settings, FreeRDP_OsMinorType);
    return os_major == OSMAJORTYPE_WINDOWS && os_minor == OSMINORTYPE_WINDOWS_NT;
}

static WCHAR *
drd_rdp_session_get_utf16_string(const char *str, size_t *size)
{
    g_return_val_if_fail(str != NULL, NULL);
    g_return_val_if_fail(size != NULL, NULL);

    *size = 0;
    WCHAR *utf16 = ConvertUtf8ToWCharAlloc(str, size);
    if (utf16 == NULL)
    {
        return NULL;
    }

    *size = (*size + 1) * sizeof(WCHAR);
    return utf16;
}

static WCHAR *
drd_rdp_session_generate_redirection_guid(size_t *size)
{
    BYTE guid_bytes[16] = {0};
    g_autofree gchar *guid_base64 = NULL;

    if (winpr_RAND(guid_bytes, sizeof(guid_bytes)) == -1)
    {
        return NULL;
    }

    guid_base64 = crypto_base64_encode(guid_bytes, sizeof(guid_bytes));
    if (guid_base64 == NULL)
    {
        return NULL;
    }

    return drd_rdp_session_get_utf16_string(guid_base64, size);
}

static BYTE *
drd_rdp_session_get_certificate_container(const char *certificate,
                                          size_t *size)
{
    g_return_val_if_fail(certificate != NULL, NULL);
    g_return_val_if_fail(size != NULL, NULL);

    g_autoptr(rdpCertificate) rdp_cert = freerdp_certificate_new_from_pem(certificate);
    if (rdp_cert == NULL)
    {
        return NULL;
    }

    size_t der_length = 0;
    g_autofree BYTE *der_cert = freerdp_certificate_get_der(rdp_cert, &der_length);
    if (der_cert == NULL)
    {
        return NULL;
    }

    wStream *stream = Stream_New(NULL, 2048);
    if (stream == NULL)
    {
        return NULL;
    }

    if (!Stream_EnsureRemainingCapacity(stream, 12))
    {
        Stream_Free(stream, TRUE);
        return NULL;
    }

    Stream_Write_UINT32(stream, ELEMENT_TYPE_CERTIFICATE);
    Stream_Write_UINT32(stream, ENCODING_TYPE_ASN1_DER);
    const gint64 der_len_signed = (gint64)der_length;
    if (der_len_signed < 0 || der_len_signed > G_MAXUINT32)
    {
        Stream_Free(stream, TRUE);
        return NULL;
    }
    Stream_Write_UINT32(stream, der_len_signed);

    if (!Stream_EnsureRemainingCapacity(stream, der_length))
    {
        Stream_Free(stream, TRUE);
        return NULL;
    }

    Stream_Write(stream, der_cert, der_length);

    *size = Stream_GetPosition(stream);
    BYTE *container = Stream_Buffer(stream);
    Stream_Free(stream, FALSE);
    return container;
}

void
drd_rdp_session_notify_error(DrdRdpSession *self, DrdRdpSessionError error)
{
    g_return_if_fail(DRD_IS_RDP_SESSION(self));
    if (error == DRD_RDP_SESSION_ERROR_NONE)
    {
        return;
    }

    self->last_error = error;
    const gchar *reason = NULL;
    switch (error)
    {
        case DRD_RDP_SESSION_ERROR_BAD_CAPS:
            reason = "client reported invalid capabilities";
            break;
        case DRD_RDP_SESSION_ERROR_BAD_MONITOR_DATA:
            reason = "client monitor layout invalid";
            break;
        case DRD_RDP_SESSION_ERROR_CLOSE_STACK_ON_DRIVER_FAILURE:
            reason = "graphics driver requested close";
            break;
        case DRD_RDP_SESSION_ERROR_GRAPHICS_SUBSYSTEM_FAILED:
            reason = "graphics subsystem failed";
            break;
        case DRD_RDP_SESSION_ERROR_SERVER_REDIRECTION:
            reason = "server redirection requested";
            break;
        case DRD_RDP_SESSION_ERROR_NONE:
        default:
            reason = "unknown";
            break;
    }

    DRD_LOG_WARNING("Session %s reported error: %s", self->peer_address, reason);

    if (error == DRD_RDP_SESSION_ERROR_SERVER_REDIRECTION)
    {
        drd_rdp_session_disconnect(self, reason);
    }
}

gboolean
drd_rdp_session_send_server_redirection(DrdRdpSession *self,
                                        const gchar *routing_token,
                                        const gchar *username,
                                        const gchar *password,
                                        const gchar *certificate)
{
    DRD_LOG_MESSAGE("drd_rdp_session_send_server_redirection");
    g_return_val_if_fail(DRD_IS_RDP_SESSION(self), FALSE);
    g_return_val_if_fail(self->peer != NULL, FALSE);
    g_return_val_if_fail(routing_token != NULL, FALSE);
    g_return_val_if_fail(username != NULL, FALSE);
    g_return_val_if_fail(password != NULL, FALSE);
    g_return_val_if_fail(certificate != NULL, FALSE);

    rdpSettings *settings = self->peer->context != NULL ? self->peer->context->settings : NULL;
    if (settings == NULL)
    {
        DRD_LOG_WARNING("Session %s missing settings, cannot send redirection", self->peer_address);
        return FALSE;
    }

    g_autoptr(rdpRedirection) redirection = redirection_new();
    if (redirection == NULL)
    {
        DRD_LOG_MESSAGE("redirection null");
        return FALSE;
    }

    guint32 redirection_flags = 0;
    guint32 incorrect_flags = 0;
    size_t size = 0;

    redirection_flags |= LB_LOAD_BALANCE_INFO;
    redirection_set_byte_option(redirection,
                                LB_LOAD_BALANCE_INFO,
                                (const BYTE *)routing_token,
                                strlen(routing_token));

    redirection_flags |= LB_USERNAME;
    redirection_set_string_option(redirection, LB_USERNAME, username);

    redirection_flags |= LB_PASSWORD;
    guint32 os_major = freerdp_settings_get_uint32(settings, FreeRDP_OsMajorType);
    if (os_major != OSMAJORTYPE_IOS && os_major != OSMAJORTYPE_ANDROID)
    {
        redirection_flags |= LB_PASSWORD_IS_PK_ENCRYPTED;
    }

    g_autofree WCHAR *utf16_password = drd_rdp_session_get_utf16_string(password, &size);
    if (utf16_password == NULL)
    {
        return FALSE;
    }
    redirection_set_byte_option(redirection, LB_PASSWORD, (const BYTE *)utf16_password, size);

    redirection_flags |= LB_REDIRECTION_GUID;
    g_autofree WCHAR *encoded_guid = drd_rdp_session_generate_redirection_guid(&size);
    if (encoded_guid == NULL)
    {
        return FALSE;
    }
    redirection_set_byte_option(redirection, LB_REDIRECTION_GUID, (const BYTE *)encoded_guid, size);

    redirection_flags |= LB_TARGET_CERTIFICATE;
    g_autofree BYTE *certificate_container =
        drd_rdp_session_get_certificate_container(certificate, &size);
    if (certificate_container == NULL)
    {
        return FALSE;
    }
    redirection_set_byte_option(redirection, LB_TARGET_CERTIFICATE, certificate_container, size);

    redirection_set_flags(redirection, redirection_flags);

    if (!redirection_settings_are_valid(redirection, &incorrect_flags))
    {
        DRD_LOG_WARNING("Session %s invalid redirection flags 0x%08x",
                        self->peer_address,
                        incorrect_flags);
        return FALSE;
    }

    DRD_LOG_MESSAGE("Session %s sending server redirection", self->peer_address);
    if (!self->peer->SendServerRedirection(self->peer, redirection))
    {
        DRD_LOG_WARNING("Session %s failed to send server redirection", self->peer_address);
        return FALSE;
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
    g_clear_pointer(&self->local_session, drd_local_session_close);

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
drd_rdp_session_vcm_thread(gpointer user_data)
{
    DrdRdpSession *self = g_object_ref(DRD_RDP_SESSION(user_data));
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
        HANDLE events[32];
        uint32_t peer_events_handles = 0;
        DWORD n_events = 0;

        if (self->stop_event != NULL)
        {
            events[n_events++] = self->stop_event;
        }
        if (channel_event != NULL)
        {
            events[n_events++] = channel_event;
        }

        peer_events_handles = peer->GetEventHandles(peer, &events[n_events], G_N_ELEMENTS(events) - n_events);
        if (!peer_events_handles)
        {
            g_message ("[RDP] peer_events_handles 0, stopping session");
            g_atomic_int_set(&self->connection_alive, 0);
            break;
        }
        n_events += peer_events_handles;
        DWORD status = WAIT_TIMEOUT;
        if (n_events > 0)
        {
            status = WaitForMultipleObjects(n_events, events, FALSE, INFINITE);
        }

        if (status == WAIT_FAILED)
        {
            break;
        }

        if (!peer->CheckFileDescriptor (peer))
        {
            g_message ("[RDP] CheckFileDescriptor error, stopping session");
            g_atomic_int_set(&self->connection_alive, 0);
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

        switch ( WTSVirtualChannelManagerGetDrdynvcState(vcm))
        {
        case DRDYNVC_STATE_NONE:
            SetEvent(channel_event);
            break;
        case DRDYNVC_STATE_READY:
            if (self->graphics_pipeline && g_atomic_int_get(&self->connection_alive))
            {
                drd_rdp_graphics_pipeline_maybe_init(self->graphics_pipeline);
            }
            break;
        }
        if (!g_atomic_int_get(&self->connection_alive))
        {
            break;
        }
        if (channel_event != NULL &&
            WaitForSingleObject(channel_event, 0) == WAIT_OBJECT_0)
        {
            if (!WTSVirtualChannelManagerCheckFileDescriptor(vcm))
            {
                DRD_LOG_MESSAGE("Session %s failed to check VCM descriptor", self->peer_address);
                g_atomic_int_set(&self->connection_alive, 0);
                break;
            }
        }
    }

    g_atomic_int_set(&self->render_running, 0);
    drd_rdp_session_notify_closed(self);
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

static gpointer
drd_rdp_session_render_thread(gpointer user_data)
{
    DrdRdpSession *self = DRD_RDP_SESSION(user_data);

    while (g_atomic_int_get(&self->render_running))
    {
        if (!g_atomic_int_get(&self->connection_alive))
        {
            break;
        }

        if (!self->is_activated || self->runtime == NULL)
        {
            g_usleep(1000);
            continue;
        }

        if (self->graphics_pipeline != NULL && self->graphics_pipeline_ready)
        {
            /*
             * Rdpgfx 背压：若 outstanding_frames >= max_outstanding_frames，则
             * drd_rdp_graphics_pipeline_wait_for_capacity() 会在 capacity_cond 上等待，
             * 直到客户端发送 RDPGFX_FRAME_ACKNOWLEDGE 释放槽位，保证不会编码无限多
             * Progressive 帧。
             */
            if (!drd_rdp_session_wait_for_graphics_capacity(self, -1))
            {
                drd_rdp_session_disable_graphics_pipeline(self, "Rdpgfx capacity wait aborted");
            }
        }

        DrdEncodedFrame *encoded = NULL;
        g_autoptr(GError) error = NULL;
        if (!drd_server_runtime_pull_encoded_frame(self->runtime,
                                                    16 * 1000,
                                                    &encoded,
                                                    &error))
        {
            if (error != NULL && error->domain == G_IO_ERROR && error->code == G_IO_ERROR_TIMED_OUT)
            {
                g_clear_error(&error);
                continue;
            }

            if (error != NULL)
            {
                DRD_LOG_WARNING("Session %s failed to pull encoded frame: %s",
                                self->peer_address,
                                error->message);
            }
            continue;
        }

        g_autoptr(DrdEncodedFrame) owned_frame = encoded;

        gboolean sent_via_graphics = drd_rdp_session_try_submit_graphics(self, owned_frame);
        if (!sent_via_graphics)
        {
            guint32 negotiated_max_payload = 0;
            if (self->peer != NULL && self->peer->context != NULL &&
                self->peer->context->settings != NULL)
            {
                negotiated_max_payload =
                    freerdp_settings_get_uint32(self->peer->context->settings,
                                                FreeRDP_MultifragMaxRequestSize);
            }
            DRD_LOG_MESSAGE("try to send surface bit");
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
    }

    g_object_unref(self);
    return NULL;
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
drd_rdp_session_wait_for_graphics_capacity(DrdRdpSession *self,
                                           gint64 timeout_us)
{
    if (self->graphics_pipeline == NULL || !self->graphics_pipeline_ready)
    {
        return FALSE;
    }

    return drd_rdp_graphics_pipeline_wait_for_capacity(self->graphics_pipeline,
                                                       timeout_us);
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
        if (!drd_rdp_session_wait_for_graphics_capacity(self, -1) ||
            !drd_rdp_graphics_pipeline_can_submit(self->graphics_pipeline))
        {
            DRD_LOG_WARNING("Session %s Rdpgfx congestion persists, disabling graphics pipeline",
                            self->peer_address);
            drd_rdp_session_disable_graphics_pipeline(self, "Rdpgfx congestion");
            return TRUE;
        }
    }

    /*
     * Progressive 关键帧 = 覆盖完整画面且包含 SYNC/CONTEXT 等头部的帧，客户端无需
     * 依赖旧数据即可渲染。只有在关键帧成功提交后，后续增量帧才安全，因此我们要
     * 在编码线程覆盖 payload 前抢先读取 is_keyframe，并传给管线判断。
     */
    gboolean frame_is_keyframe = drd_encoded_frame_is_keyframe(frame);
    g_autoptr(GError) gfx_error = NULL;
    if (!drd_rdp_graphics_pipeline_submit_frame(self->graphics_pipeline,
                                                frame,
                                                frame_is_keyframe,
                                                &gfx_error))
    {
        if (gfx_error != NULL &&
            g_error_matches(gfx_error,
                            DRD_RDP_GRAPHICS_PIPELINE_ERROR,
                            DRD_RDP_GRAPHICS_PIPELINE_ERROR_NEEDS_KEYFRAME))
        {
            DRD_LOG_MESSAGE("Session %s Rdpgfx requires progressive keyframe, requesting re-encode",
                            self->peer_address);
            drd_server_runtime_request_keyframe(self->runtime);
            return TRUE;
        }

        if (gfx_error != NULL)
        {
            DRD_LOG_WARNING("Session %s Rdpgfx submission failed: %s",
                            self->peer_address,
                            gfx_error->message);
        }
        drd_rdp_session_disable_graphics_pipeline(self, "Rdpgfx submission failure");
        return TRUE;
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
