#include "transport/grdc_rdp_listener.h"

#include <gio/gio.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/settings.h>
#include <freerdp/input.h>
#include <winpr/wtypes.h>

#include "core/grdc_server_runtime.h"
#include "input/grdc_input_dispatcher.h"
#include "session/grdc_rdp_session.h"
#include "security/grdc_tls_credentials.h"

typedef struct
{
    rdpContext context;
    GrdcRdpSession *session;
    GrdcServerRuntime *runtime;
} GrdcRdpPeerContext;

static BOOL grdc_rdp_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code);
static BOOL grdc_rdp_peer_unicode_event(rdpInput *input, UINT16 flags, UINT16 code);
static BOOL grdc_rdp_peer_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);

struct _GrdcRdpListener
{
    GObject parent_instance;

    gchar *bind_address;
    guint16 port;
    freerdp_listener *listener;
    guint tick_id;
    GPtrArray *sessions;
    GrdcServerRuntime *runtime;
};

G_DEFINE_TYPE(GrdcRdpListener, grdc_rdp_listener, G_TYPE_OBJECT)

static void grdc_rdp_listener_stop_internal(GrdcRdpListener *self);

static void
grdc_rdp_listener_dispose(GObject *object)
{
    GrdcRdpListener *self = GRDC_RDP_LISTENER(object);
    grdc_rdp_listener_stop_internal(self);
    g_clear_object(&self->runtime);
    G_OBJECT_CLASS(grdc_rdp_listener_parent_class)->dispose(object);
}

static void
grdc_rdp_listener_finalize(GObject *object)
{
    GrdcRdpListener *self = GRDC_RDP_LISTENER(object);
    g_clear_pointer(&self->bind_address, g_free);
    g_clear_pointer(&self->sessions, g_ptr_array_unref);
    G_OBJECT_CLASS(grdc_rdp_listener_parent_class)->finalize(object);
}

static void
grdc_rdp_listener_class_init(GrdcRdpListenerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = grdc_rdp_listener_dispose;
    object_class->finalize = grdc_rdp_listener_finalize;
}

static void
grdc_rdp_listener_init(GrdcRdpListener *self)
{
    self->sessions = g_ptr_array_new_with_free_func(g_object_unref);
}

GrdcRdpListener *
grdc_rdp_listener_new(const gchar *bind_address, guint16 port, GrdcServerRuntime *runtime)
{
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(runtime), NULL);

    GrdcRdpListener *self = g_object_new(GRDC_TYPE_RDP_LISTENER, NULL);
    self->bind_address = g_strdup(bind_address != NULL ? bind_address : "0.0.0.0");
    self->port = port;
    self->runtime = g_object_ref(runtime);
    return self;
}

GrdcServerRuntime *
grdc_rdp_listener_get_runtime(GrdcRdpListener *self)
{
    g_return_val_if_fail(GRDC_IS_RDP_LISTENER(self), NULL);
    return self->runtime;
}

static BOOL
grdc_peer_context_new(freerdp_peer *client, rdpContext *context)
{
    GrdcRdpPeerContext *ctx = (GrdcRdpPeerContext *)context;
    ctx->session = grdc_rdp_session_new(client);
    ctx->runtime = NULL;
    return ctx->session != NULL;
}

static void
grdc_peer_context_free(freerdp_peer *client G_GNUC_UNUSED, rdpContext *context)
{
    GrdcRdpPeerContext *ctx = (GrdcRdpPeerContext *)context;
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
}

static BOOL
grdc_peer_post_connect(freerdp_peer *client)
{
    GrdcRdpPeerContext *ctx = (GrdcRdpPeerContext *)client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return FALSE;
    }
    return grdc_rdp_session_post_connect(ctx->session);
}

static BOOL
grdc_peer_activate(freerdp_peer *client)
{
    GrdcRdpPeerContext *ctx = (GrdcRdpPeerContext *)client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        return FALSE;
    }
    return grdc_rdp_session_activate(ctx->session);
}

static void
grdc_peer_disconnected(freerdp_peer *client)
{
    GrdcRdpPeerContext *ctx = (GrdcRdpPeerContext *)client->context;
    if (ctx != NULL && ctx->session != NULL)
    {
        grdc_rdp_session_set_peer_state(ctx->session, "disconnected");
    }
}

static GrdcInputDispatcher *
grdc_peer_get_dispatcher(rdpInput *input)
{
    if (input == NULL || input->context == NULL)
    {
        return NULL;
    }

    GrdcRdpPeerContext *ctx = (GrdcRdpPeerContext *)input->context;
    if (ctx == NULL || ctx->runtime == NULL)
    {
        return NULL;
    }

    return grdc_server_runtime_get_input(ctx->runtime);
}

static BOOL
grdc_rdp_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code)
{
    GrdcInputDispatcher *dispatcher = grdc_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!grdc_input_dispatcher_handle_keyboard(dispatcher, flags, code, &error) && error != NULL)
    {
        g_warning("Keyboard injection failed: %s", error->message);
    }
    return TRUE;
}

static BOOL
grdc_rdp_peer_unicode_event(rdpInput *input, UINT16 flags, UINT16 code)
{
    GrdcInputDispatcher *dispatcher = grdc_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!grdc_input_dispatcher_handle_unicode(dispatcher, flags, code, &error) && error != NULL)
    {
        g_debug("Unicode injection not supported: %s", error->message);
    }
    return TRUE;
}

static BOOL
grdc_rdp_peer_pointer_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    GrdcInputDispatcher *dispatcher = grdc_peer_get_dispatcher(input);
    if (dispatcher == NULL)
    {
        return TRUE;
    }

    g_autoptr(GError) error = NULL;
    if (!grdc_input_dispatcher_handle_pointer(dispatcher, flags, x, y, &error) && error != NULL)
    {
        g_warning("Pointer injection failed: %s", error->message);
    }
    return TRUE;
}

static BOOL
grdc_configure_peer_settings(GrdcRdpListener *self, freerdp_peer *client, GError **error)
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

    GrdcTlsCredentials *tls = grdc_server_runtime_get_tls_credentials(self->runtime);
    if (tls == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "TLS credentials not configured");
        return FALSE;
    }

    if (!grdc_tls_credentials_apply(tls, settings, error))
    {
        return FALSE;
    }

    GrdcEncodingOptions encoding_opts;
    if (!grdc_server_runtime_get_encoding_options(self->runtime, &encoding_opts))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Server runtime does not expose encoding geometry");
        return FALSE;
    }

    const guint32 width = encoding_opts.width;
    const guint32 height = encoding_opts.height;

    if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
        !freerdp_settings_set_bool(settings, FreeRDP_ServerMode, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_DesktopResize, TRUE) ||
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
        !freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE) ||
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

    if (!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
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
grdc_listener_peer_accepted(freerdp_listener *listener, freerdp_peer *client)
{
    GrdcRdpListener *self = (GrdcRdpListener *)listener->param1;
    if (self == NULL)
    {
        return FALSE;
    }

    client->ContextSize = sizeof(GrdcRdpPeerContext);
    client->ContextNew = grdc_peer_context_new;
    client->ContextFree = grdc_peer_context_free;

    if (!freerdp_peer_context_new(client))
    {
        g_warning("Failed to allocate peer %s context", client->hostname);
        return FALSE;
    }

    if (self->sessions->len > 0)
    {
        g_warning("Rejecting connection from %s: session already active", client->hostname);
        return FALSE;
    }

    g_autoptr(GError) settings_error = NULL;
    if (!grdc_configure_peer_settings(self, client, &settings_error))
    {
        if (settings_error != NULL)
        {
            g_warning("Failed to configure peer %s settings: %s", client->hostname, settings_error->message);
        }
        else
        {
            g_warning("Failed to configure peer %s settings", client->hostname);
        }
        return FALSE;
    }

    client->PostConnect = grdc_peer_post_connect;
    client->Activate = grdc_peer_activate;
    client->Disconnect = grdc_peer_disconnected;

    if (client->Initialize == NULL || !client->Initialize(client))
    {
        g_warning("Failed to initialize peer %s", client->hostname);
        return FALSE;
    }

    GrdcRdpPeerContext *ctx = (GrdcRdpPeerContext *)client->context;
    if (ctx == NULL || ctx->session == NULL)
    {
        g_warning("Peer %s context did not expose a session", client->hostname);
        return FALSE;
    }

    ctx->runtime = g_object_ref(self->runtime);
    grdc_rdp_session_set_runtime(ctx->session, self->runtime);

    if (!grdc_rdp_session_start_event_thread(ctx->session))
    {
        g_warning("Failed to start event thread for peer %s", client->hostname);
        return FALSE;
    }

    grdc_rdp_session_set_peer_state(ctx->session, "initialized");
    g_ptr_array_add(self->sessions, g_object_ref(ctx->session));

    if (client->context != NULL && client->context->input != NULL)
    {
        rdpInput *input = client->context->input;
        input->context = client->context;
        input->KeyboardEvent = grdc_rdp_peer_keyboard_event;
        input->UnicodeKeyboardEvent = grdc_rdp_peer_unicode_event;
        input->MouseEvent = grdc_rdp_peer_pointer_event;
        input->ExtendedMouseEvent = grdc_rdp_peer_pointer_event;
    }

    g_message("Accepted connection from %s", client->hostname);
    return TRUE;
}

static gboolean
grdc_rdp_listener_iterate(gpointer user_data)
{
    GrdcRdpListener *self = user_data;
    if (self->listener == NULL)
    {
        return G_SOURCE_REMOVE;
    }

    if (self->listener->CheckFileDescriptor != NULL)
    {
        if (!self->listener->CheckFileDescriptor(self->listener))
        {
            g_warning("Listener CheckFileDescriptor failed");
        }
    }

    guint index = 0;
    while (index < self->sessions->len)
    {
        GrdcRdpSession *session = g_ptr_array_index(self->sessions, index);
        if (!grdc_rdp_session_pump(session))
        {
            grdc_rdp_session_set_peer_state(session, "closed");
            g_ptr_array_remove_index(self->sessions, index);
            continue;
        }
        index++;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
grdc_rdp_listener_open(GrdcRdpListener *self, GError **error)
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
    self->listener->PeerAccepted = grdc_listener_peer_accepted;

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

    self->tick_id = g_timeout_add(16, grdc_rdp_listener_iterate, self);
    if (self->tick_id == 0)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to create listener tick source");
        return FALSE;
    }

    return TRUE;
}

static void
grdc_rdp_listener_stop_internal(GrdcRdpListener *self)
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
        grdc_server_runtime_stop(self->runtime);
    }
}

gboolean
grdc_rdp_listener_start(GrdcRdpListener *self, GError **error)
{
    g_return_val_if_fail(GRDC_IS_RDP_LISTENER(self), FALSE);

    if (self->listener != NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_EXISTS,
                            "Listener already running");
        return FALSE;
    }

    if (!grdc_rdp_listener_open(self, error))
    {
        grdc_rdp_listener_stop_internal(self);
        return FALSE;
    }

    return TRUE;
}

void
grdc_rdp_listener_stop(GrdcRdpListener *self)
{
    g_return_if_fail(GRDC_IS_RDP_LISTENER(self));
    grdc_rdp_listener_stop_internal(self);
}
