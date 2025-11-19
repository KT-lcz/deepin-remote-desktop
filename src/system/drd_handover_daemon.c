#include "system/drd_handover_daemon.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "core/drd_dbus_constants.h"
#include "drd-dbus-remote-desktop.h"
#include "transport/drd_rdp_listener.h"
#include "utils/drd_log.h"

static gboolean drd_handover_daemon_bind_handover(DrdHandoverDaemon *self, GError **error);
static gboolean drd_handover_daemon_start_session(DrdHandoverDaemon *self, GError **error);
static gboolean drd_handover_daemon_take_client(DrdHandoverDaemon *self, GError **error);
static void drd_handover_daemon_on_redirect_client(DrdDBusRemoteDesktopRdpHandover *interface,
                                                   const gchar *routing_token,
                                                   const gchar *username,
                                                   const gchar *password,
                                                   gpointer user_data);
static void drd_handover_daemon_on_take_client_ready(DrdDBusRemoteDesktopRdpHandover *interface,
                                                     gboolean use_system_credentials,
                                                     gpointer user_data);
static void drd_handover_daemon_on_restart_handover(DrdDBusRemoteDesktopRdpHandover *interface,
                                                    gpointer user_data);

struct _DrdHandoverDaemon
{
    GObject parent_instance;

    DrdConfig *config;
    DrdServerRuntime *runtime;
    DrdTlsCredentials *tls_credentials;

    DrdDBusRemoteDesktopRdpDispatcher *dispatcher_proxy;
    DrdDBusRemoteDesktopRdpHandover *handover_proxy;
    gchar *handover_object_path;
    DrdRdpListener *listener;
    GSocketConnection *active_connection;
};

G_DEFINE_TYPE(DrdHandoverDaemon, drd_handover_daemon, G_TYPE_OBJECT)

static void
drd_handover_daemon_dispose(GObject *object)
{
    DrdHandoverDaemon *self = DRD_HANDOVER_DAEMON(object);

    g_clear_object(&self->dispatcher_proxy);
    g_clear_object(&self->handover_proxy);
    g_clear_pointer(&self->handover_object_path, g_free);
    g_clear_object(&self->listener);
    g_clear_object(&self->active_connection);
    g_clear_object(&self->tls_credentials);
    g_clear_object(&self->runtime);
    g_clear_object(&self->config);

    G_OBJECT_CLASS(drd_handover_daemon_parent_class)->dispose(object);
}

static void
drd_handover_daemon_class_init(DrdHandoverDaemonClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_handover_daemon_dispose;
}

static void
drd_handover_daemon_init(DrdHandoverDaemon *self)
{
    self->handover_proxy = NULL;
    self->handover_object_path = NULL;
    self->listener = NULL;
    self->active_connection = NULL;
}

DrdHandoverDaemon *
drd_handover_daemon_new(DrdConfig *config,
                        DrdServerRuntime *runtime,
                        DrdTlsCredentials *tls_credentials)
{
    g_return_val_if_fail(DRD_IS_CONFIG(config), NULL);
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(runtime), NULL);

    DrdHandoverDaemon *self = g_object_new(DRD_TYPE_HANDOVER_DAEMON, NULL);
    self->config = g_object_ref(config);
    self->runtime = g_object_ref(runtime);
    if (tls_credentials != NULL)
    {
        self->tls_credentials = g_object_ref(tls_credentials);
    }
    return self;
}

static gboolean
drd_handover_daemon_bind_handover(DrdHandoverDaemon *self, GError **error)
{
    gchar *object_path = NULL;
    if (!drd_dbus_remote_desktop_rdp_dispatcher_call_request_handover_sync(
            self->dispatcher_proxy,
            &object_path,
            NULL,
            error))
    {
        return FALSE;
    }

    self->handover_object_path = object_path;
    self->handover_proxy =
        drd_dbus_remote_desktop_rdp_handover_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM,
            G_DBUS_PROXY_FLAGS_NONE,
            DRD_REMOTE_DESKTOP_BUS_NAME,
            object_path,
            NULL,
            error);
    if (self->handover_proxy == NULL)
    {
        return FALSE;
    }

    g_signal_connect(self->handover_proxy,
                     "redirect-client",
                     G_CALLBACK(drd_handover_daemon_on_redirect_client),
                     self);
    g_signal_connect(self->handover_proxy,
                     "take-client-ready",
                     G_CALLBACK(drd_handover_daemon_on_take_client_ready),
                     self);
    g_signal_connect(self->handover_proxy,
                     "restart-handover",
                     G_CALLBACK(drd_handover_daemon_on_restart_handover),
                     self);

    DRD_LOG_MESSAGE("Bound to handover object %s", self->handover_object_path);
    return TRUE;
}

static gboolean
drd_handover_daemon_start_session(DrdHandoverDaemon *self, GError **error)
{
    const gchar *username = drd_config_get_nla_username(self->config);
    const gchar *password = drd_config_get_nla_password(self->config);
    if (username == NULL)
    {
        username = "handover";
    }
    if (password == NULL)
    {
        password = "handover";
    }

    g_autofree gchar *certificate = NULL;
    g_autofree gchar *key = NULL;
    if (!drd_dbus_remote_desktop_rdp_handover_call_start_handover_sync(self->handover_proxy,
                                                                       username,
                                                                       password,
                                                                       &certificate,
                                                                       &key,
                                                                       NULL,
                                                                       error))
    {
        return FALSE;
    }

    DRD_LOG_MESSAGE("StartHandover negotiated TLS (%zu bytes cert)",
                    certificate != NULL ? strlen(certificate) : 0);
    return TRUE;
}

static gboolean
drd_handover_daemon_take_client(DrdHandoverDaemon *self, GError **error)
{
    g_autoptr(GVariant) fd_variant = NULL;
    g_autoptr(GUnixFDList) fd_list = NULL;
    if (!drd_dbus_remote_desktop_rdp_handover_call_take_client_sync(self->handover_proxy,
                                                                    NULL,
                                                                    &fd_variant,
                                                                    &fd_list,
                                                                    NULL,
                                                                    error))
    {
        return FALSE;
    }

    gint fd_idx = g_variant_get_handle(fd_variant);
    gint fd = g_unix_fd_list_get(fd_list, fd_idx, error);
    if (fd == -1)
    {
        return FALSE;
    }

    g_autoptr(GSocket) socket = g_socket_new_from_fd(fd, error);
    if (!G_IS_SOCKET(socket))
    {
        return FALSE;
    }

    g_autoptr(GSocketConnection) connection =
        g_socket_connection_factory_create_connection(socket);
    if (!drd_rdp_listener_adopt_connection(self->listener, connection, error))
    {
        return FALSE;
    }

    DRD_LOG_MESSAGE("Adopted redirected client");
    return TRUE;
}

static void
drd_handover_daemon_on_redirect_client(DrdDBusRemoteDesktopRdpHandover *interface,
                                       const gchar *routing_token,
                                       const gchar *username,
                                       const gchar *password,
                                       gpointer user_data)
{
    (void)interface;
    (void)username;
    (void)password;
    DrdHandoverDaemon *self = user_data;
    DRD_LOG_MESSAGE("RedirectClient received (token=%s) for %s",
                    routing_token != NULL ? routing_token : "unknown",
                    self->handover_object_path);
}

static void
drd_handover_daemon_on_take_client_ready(DrdDBusRemoteDesktopRdpHandover *interface,
                                         gboolean use_system_credentials,
                                         gpointer user_data)
{
    (void)interface;
    (void)use_system_credentials;
    DrdHandoverDaemon *self = user_data;

    g_autoptr(GError) error = NULL;
    if (!drd_handover_daemon_take_client(self, &error) && error != NULL)
    {
        DRD_LOG_WARNING("Failed to take client: %s", error->message);
    }
}

static void
drd_handover_daemon_on_restart_handover(DrdDBusRemoteDesktopRdpHandover *interface,
                                        gpointer user_data)
{
    (void)interface;
    DrdHandoverDaemon *self = user_data;
    DRD_LOG_MESSAGE("RestartHandover received for %s", self->handover_object_path);
}

void
drd_handover_daemon_stop(DrdHandoverDaemon *self)
{
    g_return_if_fail(DRD_IS_HANDOVER_DAEMON(self));

    g_clear_object(&self->dispatcher_proxy);
    g_clear_object(&self->handover_proxy);
    g_clear_pointer(&self->handover_object_path, g_free);
    g_clear_object(&self->listener);
}

gboolean
drd_handover_daemon_start(DrdHandoverDaemon *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_HANDOVER_DAEMON(self), FALSE);

    if (self->dispatcher_proxy == NULL)
    {
        self->dispatcher_proxy =
            drd_dbus_remote_desktop_rdp_dispatcher_proxy_new_for_bus_sync(
                G_BUS_TYPE_SYSTEM,
                G_DBUS_PROXY_FLAGS_NONE,
                DRD_REMOTE_DESKTOP_BUS_NAME,
                DRD_REMOTE_DESKTOP_DISPATCHER_OBJECT_PATH,
                NULL,
                error);
        if (self->dispatcher_proxy == NULL)
        {
            return FALSE;
        }
    }

    if (self->listener == NULL)
    {
        const DrdEncodingOptions *encoding_opts = drd_config_get_encoding_options(self->config);
        if (encoding_opts == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Encoding options unavailable for handover listener");
            return FALSE;
        }

        self->listener =
            drd_rdp_listener_new(drd_config_get_bind_address(self->config),
                                 drd_config_get_port(self->config),
                                 self->runtime,
                                 encoding_opts,
                                 drd_config_is_nla_enabled(self->config),
                                 drd_config_get_nla_username(self->config),
                                 drd_config_get_nla_password(self->config),
                                 drd_config_get_pam_service(self->config),
                                 FALSE);
        if (self->listener == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to create handover listener");
            return FALSE;
        }
    }

    if (!drd_handover_daemon_bind_handover(self, error))
    {
        return FALSE;
    }

    if (!drd_handover_daemon_start_session(self, error))
    {
        return FALSE;
    }

    DRD_LOG_MESSAGE("Handover daemon connected to dispatcher %s",
              DRD_REMOTE_DESKTOP_DISPATCHER_OBJECT_PATH);
    return TRUE;
}
