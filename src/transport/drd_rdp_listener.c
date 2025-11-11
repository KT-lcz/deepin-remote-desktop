#include "transport/drd_rdp_listener.h"

#include <gio/gio.h>
#include <string.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/settings.h>
#include <freerdp/input.h>
#include <winpr/wtypes.h>
#include <winpr/wtsapi.h>

#include "core/drd_server_runtime.h"
#include "input/drd_input_dispatcher.h"
#include "session/drd_rdp_session.h"
#include "security/drd_tls_credentials.h"
#include "security/drd_nla_sam.h"
#include "utils/drd_log.h"

typedef struct
{
    rdpContext context;
    DrdRdpSession *session;
    DrdServerRuntime *runtime;
    DrdNlaSamFile *nla_sam;
    HANDLE vcm;
} DrdRdpPeerContext;

static BOOL drd_rdp_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code);
static BOOL drd_rdp_peer_unicode_event(rdpInput *input, UINT16 flags, UINT16 code);
static BOOL drd_rdp_peer_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);
static BOOL drd_peer_capabilities(freerdp_peer *client);

struct _DrdRdpListener
{
    GObject parent_instance;

    gchar *bind_address;
    guint16 port;
    freerdp_listener *listener;
    guint tick_id;
    GPtrArray *sessions;
    DrdServerRuntime *runtime;
    gchar *nla_username;
    gchar *nla_password;
    gchar *nla_hash;
};

G_DEFINE_TYPE(DrdRdpListener, drd_rdp_listener, G_TYPE_OBJECT)

static void drd_rdp_listener_stop_internal(DrdRdpListener *self);

static gboolean drd_rdp_listener_ensure_nla_hash(DrdRdpListener *self, GError **error)
{
    if (self->nla_hash != NULL)
    {
        return TRUE;
    }

    if (self->nla_password == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "NLA password is not available");
        return FALSE;
    }

    self->nla_hash = drd_nla_sam_hash_password(self->nla_password);
    if (self->nla_hash == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to derive NT hash for NLA user");
        return FALSE;
    }

    memset(self->nla_password, 0, strlen(self->nla_password));
    g_clear_pointer(&self->nla_password, g_free);
    return TRUE;
}

static void
drd_rdp_listener_dispose(GObject *object)
{
    DrdRdpListener *self = DRD_RDP_LISTENER(object);
    drd_rdp_listener_stop_internal(self);
    g_clear_object(&self->runtime);
    G_OBJECT_CLASS(drd_rdp_listener_parent_class)->dispose(object);
}

static void
drd_rdp_listener_finalize(GObject *object)
{
    DrdRdpListener *self = DRD_RDP_LISTENER(object);
    g_clear_pointer(&self->bind_address, g_free);
    g_clear_pointer(&self->sessions, g_ptr_array_unref);
    g_clear_pointer(&self->nla_username, g_free);
    if (self->nla_password != NULL)
    {
        memset(self->nla_password, 0, strlen(self->nla_password));
        g_clear_pointer(&self->nla_password, g_free);
    }
    if (self->nla_hash != NULL)
    {
        memset(self->nla_hash, 0, strlen(self->nla_hash));
        g_clear_pointer(&self->nla_hash, g_free);
    }
    G_OBJECT_CLASS(drd_rdp_listener_parent_class)->finalize(object);
}

static void
drd_rdp_listener_class_init(DrdRdpListenerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_rdp_listener_dispose;
    object_class->finalize = drd_rdp_listener_finalize;
}

static void
drd_rdp_listener_init(DrdRdpListener *self)
{
    self->sessions = g_ptr_array_new_with_free_func(g_object_unref);
}

DrdRdpListener *
drd_rdp_listener_new(const gchar *bind_address,
                      guint16 port,
                      DrdServerRuntime *runtime,
                      const gchar *nla_username,
                      const gchar *nla_password)
{
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(runtime), NULL);
    g_return_val_if_fail(nla_username != NULL && *nla_username != '\0', NULL);
    g_return_val_if_fail(nla_password != NULL && *nla_password != '\0', NULL);

    DrdRdpListener *self = g_object_new(DRD_TYPE_RDP_LISTENER, NULL);
    self->bind_address = g_strdup(bind_address != NULL ? bind_address : "0.0.0.0");
    self->port = port;
    self->runtime = g_object_ref(runtime);
    self->nla_username = g_strdup(nla_username);
    self->nla_password = g_strdup(nla_password);
    self->nla_hash = NULL;
    return self;
}

DrdServerRuntime *
drd_rdp_listener_get_runtime(DrdRdpListener *self)
{
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), NULL);
    return self->runtime;
}

static BOOL
drd_peer_context_new(freerdp_peer *client, rdpContext *context)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)context;
    ctx->session = drd_rdp_session_new(client);
    ctx->runtime = NULL;
    ctx->nla_sam = NULL;
    ctx->vcm = INVALID_HANDLE_VALUE;
    return ctx->session != NULL;
}

static void
drd_peer_context_free(freerdp_peer *client G_GNUC_UNUSED, rdpContext *context)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)context;
    if (ctx->session != NULL)
    {
        g_object_unref(ctx->session);
        ctx->session = NULL;
    }

    if (ctx->runtime != NULL)
    {
        g_object_unref(ctx->runtime);
        ctx->runtime = NULL;
    }

    g_clear_pointer(&ctx->nla_sam, drd_nla_sam_file_free);
    if (ctx->vcm != NULL && ctx->vcm != INVALID_HANDLE_VALUE)
    {
        WTSCloseServer(ctx->vcm);
        ctx->vcm = INVALID_HANDLE_VALUE;
    }
}

static BOOL
drd_peer_post_connect(freerdp_peer *client)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return FALSE;
    }
    BOOL result = drd_rdp_session_post_connect(ctx->session);
    g_clear_pointer(&ctx->nla_sam, drd_nla_sam_file_free);
    return result;
}

static BOOL
drd_peer_activate(freerdp_peer *client)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return FALSE;
    }
    return drd_rdp_session_activate(ctx->session);
}

static void
drd_peer_disconnected(freerdp_peer *client)
{
    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)client->context;
    if (ctx != NULL && ctx->session != NULL)
    {
        drd_rdp_session_set_peer_state(ctx->session, "disconnected");
    }
}

static BOOL
drd_peer_capabilities(freerdp_peer *client)
{
    if (client == NULL || client->context == NULL)
    {
        return FALSE;
    }

    rdpContext *context = client->context;
    rdpSettings *settings = context->settings;
    if (settings == NULL)
    {
        return FALSE;
    }

    const guint32 client_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const guint32 client_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    const gboolean desktop_resize = freerdp_settings_get_bool(settings, FreeRDP_DesktopResize);

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)client->context;
    if (!desktop_resize)
    {
        if (ctx != NULL && ctx->session != NULL)
        {
            drd_rdp_session_set_peer_state(ctx->session, "desktop-resize-unsupported");
        }
        DRD_LOG_WARNING("Peer %s disabled DesktopResize capability (client %ux%u), rejecting connection",
                         client->hostname,
                         client_width,
                         client_height);
        return FALSE;
    }

    DRD_LOG_MESSAGE("Peer %s capabilities accepted with DesktopResize enabled (%ux%u requested)",
                     client->hostname,
                     client_width,
                     client_height);
    return TRUE;
}

static DrdInputDispatcher *
drd_peer_get_dispatcher(rdpInput *input)
{
    if (input == NULL || input->context == NULL)
    {
        return NULL;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)input->context;
    if (ctx == NULL || ctx->runtime == NULL)
    {
        return NULL;
    }

    return drd_server_runtime_get_input(ctx->runtime);
}

static BOOL
drd_rdp_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code)
{
    DrdInputDispatcher *dispatcher = drd_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!drd_input_dispatcher_handle_keyboard(dispatcher, flags, code, &error) && error != NULL)
    {
        DRD_LOG_WARNING("Keyboard injection failed: %s", error->message);
    }
    return TRUE;
}

static BOOL
drd_rdp_peer_unicode_event(rdpInput *input, UINT16 flags, UINT16 code)
{
    DrdInputDispatcher *dispatcher = drd_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!drd_input_dispatcher_handle_unicode(dispatcher, flags, code, &error) && error != NULL)
    {
        DRD_LOG_DEBUG("Unicode injection not supported: %s", error->message);
    }
    return TRUE;
}

static BOOL
drd_rdp_peer_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    DrdInputDispatcher *dispatcher = drd_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!drd_input_dispatcher_handle_pointer(dispatcher, flags, x, y, &error) && error != NULL)
    {
        DRD_LOG_WARNING("Pointer injection failed: %s", error->message);
    }
    return TRUE;
}

static BOOL
drd_configure_peer_settings(DrdRdpListener *self, freerdp_peer *client, GError **error)
{
    if (client->context == NULL)
    {
        return FALSE;
    }

    rdpSettings *settings = client->context->settings;
    if (settings == NULL)
    {
        return FALSE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)client->context;
    if (ctx == NULL)
    {
        return FALSE;
    }

    DrdTlsCredentials *tls = drd_server_runtime_get_tls_credentials(self->runtime);
    if (tls == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "TLS credentials not configured");
        return FALSE;
    }

    if (!drd_tls_credentials_apply(tls, settings, error))
    {
        return FALSE;
    }

    if (self->nla_username == NULL ||
        !drd_rdp_listener_ensure_nla_hash(self, error))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "NLA credentials not configured");
        return FALSE;
    }

    g_clear_pointer(&ctx->nla_sam, drd_nla_sam_file_free);
    ctx->nla_sam = drd_nla_sam_file_new(self->nla_username, self->nla_hash, error);
    if (ctx->nla_sam == NULL)
    {
        return FALSE;
    }

    if (!freerdp_settings_set_string(settings,
                                     FreeRDP_NtlmSamFile,
                                     drd_nla_sam_file_get_path(ctx->nla_sam)))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to configure SAM database for NLA");
        return FALSE;
    }

    DrdEncodingOptions encoding_opts;
    if (!drd_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Server runtime does not expose encoding geometry");
        return FALSE;
    }

    const guint32 width = encoding_opts.width;
    const guint32 height = encoding_opts.height;
    const gboolean enable_graphics_pipeline = (encoding_opts.mode == DRD_ENCODING_MODE_RFX);

    if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
        !freerdp_settings_set_bool(settings, FreeRDP_ServerMode, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxImageCodec, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE) ||
        !freerdp_settings_set_bool(settings,
                                   FreeRDP_SupportGraphicsPipeline,
                                   enable_graphics_pipeline) ||
        !freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, TRUE) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_EncryptionLevel, ENCRYPTION_LEVEL_CLIENT_COMPATIBLE) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_VCFlags, VCCAPS_COMPR_SC) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_VCChunkSize, 16256) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_PointerCacheSize, 100) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 0))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to configure peer settings");
        return FALSE;
    }

    if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to configure peer security flags");
        return FALSE;
    }

    return TRUE;
}

static BOOL
drd_listener_peer_accepted(freerdp_listener *listener, freerdp_peer *client)
{
    DrdRdpListener *self = (DrdRdpListener *)listener->param1;
    if (self == NULL)
    {
        return FALSE;
    }

    client->ContextSize = sizeof(DrdRdpPeerContext);
    client->ContextNew = drd_peer_context_new;
    client->ContextFree = drd_peer_context_free;

    if (!freerdp_peer_context_new(client))
    {
        DRD_LOG_WARNING("Failed to allocate peer %s context", client->hostname);
        return FALSE;
    }

    if (self->sessions->len > 0)
    {
        DRD_LOG_WARNING("Rejecting connection from %s: session already active", client->hostname);
        return FALSE;
    }

    g_autoptr(GError) settings_error = NULL;
    if (!drd_configure_peer_settings(self, client, &settings_error))
    {
        if (settings_error != NULL)
        {
            DRD_LOG_WARNING("Failed to configure peer %s settings: %s", client->hostname, settings_error->message);
        }
        else
        {
            DRD_LOG_WARNING("Failed to configure peer %s settings", client->hostname);
        }
        return FALSE;
    }

    client->PostConnect = drd_peer_post_connect;
    client->Activate = drd_peer_activate;
    client->Disconnect = drd_peer_disconnected;
    client->Capabilities = drd_peer_capabilities;

    if (client->Initialize == NULL || !client->Initialize(client))
    {
        DRD_LOG_WARNING("Failed to initialize peer %s", client->hostname);
        return FALSE;
    }

    DrdRdpPeerContext *ctx = (DrdRdpPeerContext *)client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        DRD_LOG_WARNING("Peer %s context did not expose a session", client->hostname);
        return FALSE;
    }

    ctx->vcm = WTSOpenServerA((LPSTR)client->context);
    if (ctx->vcm == NULL || ctx->vcm == INVALID_HANDLE_VALUE)
    {
        DRD_LOG_WARNING("Peer %s failed to create virtual channel manager", client->hostname);
        return FALSE;
    }

    drd_rdp_session_set_virtual_channel_manager(ctx->session, ctx->vcm);

    ctx->runtime = g_object_ref(self->runtime);
    drd_rdp_session_set_runtime(ctx->session, self->runtime);

    if (!drd_rdp_session_start_event_thread(ctx->session))
    {
        DRD_LOG_WARNING("Failed to start event thread for peer %s", client->hostname);
        return FALSE;
    }

    drd_rdp_session_set_peer_state(ctx->session, "initialized");
    g_ptr_array_add(self->sessions, g_object_ref(ctx->session));

    if (client->context != NULL && client->context->input != NULL)
    {
        rdpInput *input = client->context->input;
        input->context = client->context;
        input->KeyboardEvent = drd_rdp_peer_keyboard_event;
        input->UnicodeKeyboardEvent = drd_rdp_peer_unicode_event;
        input->MouseEvent = drd_rdp_peer_pointer_event;
        input->ExtendedMouseEvent = drd_rdp_peer_pointer_event;
    }

    DRD_LOG_MESSAGE("Accepted connection from %s", client->hostname);
    return TRUE;
}

static gboolean
drd_rdp_listener_iterate(gpointer user_data)
{
    DrdRdpListener *self = user_data;
    if (self->listener == NULL)
    {
        return G_SOURCE_REMOVE;
    }

    if (self->listener->CheckFileDescriptor != NULL)
    {
        if (!self->listener->CheckFileDescriptor(self->listener))
        {
            DRD_LOG_WARNING("Listener CheckFileDescriptor failed");
        }
    }

    guint index = 0;
    while (index < self->sessions->len)
    {
        DrdRdpSession *session = g_ptr_array_index(self->sessions, index);
        if (!drd_rdp_session_pump(session))
        {
            drd_rdp_session_set_peer_state(session, "closed");
            g_ptr_array_remove_index(self->sessions, index);
            continue;
        }
        index++;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
drd_rdp_listener_open(DrdRdpListener *self, GError **error)
{
    self->listener = freerdp_listener_new();
    if (self->listener == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "freerdp_listener_new returned null");
        return FALSE;
    }

    self->listener->param1 = self;
    self->listener->PeerAccepted = drd_listener_peer_accepted;

    if (self->listener->Open == NULL ||
        !self->listener->Open(self->listener, self->bind_address, self->port))
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Failed to open listening socket on %s:%u",
                    self->bind_address,
                    self->port);
        return FALSE;
    }

    self->tick_id = g_timeout_add(16, drd_rdp_listener_iterate, self);
    if (self->tick_id == 0)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to create listener tick source");
        return FALSE;
    }

    DRD_LOG_MESSAGE("Listener event loop armed for %s:%u (tick=16ms)",
              self->bind_address,
              self->port);

    return TRUE;
}

static void
drd_rdp_listener_stop_internal(DrdRdpListener *self)
{
    if (self->tick_id != 0)
    {
        g_source_remove(self->tick_id);
        self->tick_id = 0;
    }

    if (self->listener != NULL)
    {
        if (self->listener->Close != NULL)
        {
            self->listener->Close(self->listener);
        }
        freerdp_listener_free(self->listener);
        self->listener = NULL;
    }

    g_ptr_array_set_size(self->sessions, 0);

    if (self->runtime != NULL)
    {
        drd_server_runtime_stop(self->runtime);
    }
}

gboolean
drd_rdp_listener_start(DrdRdpListener *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_RDP_LISTENER(self), FALSE);

    if (self->listener != NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_EXISTS,
                            "Listener already running");
        return FALSE;
    }

    if (!drd_rdp_listener_open(self, error))
    {
        drd_rdp_listener_stop_internal(self);
        return FALSE;
    }

    return TRUE;
}

void
drd_rdp_listener_stop(DrdRdpListener *self)
{
    g_return_if_fail(DRD_IS_RDP_LISTENER(self));
    drd_rdp_listener_stop_internal(self);
}
