#include "system/drd_system_daemon.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "core/drd_dbus_constants.h"
#include "transport/drd_rdp_listener.h"
#include "transport/drd_rdp_routing_token.h"
#include "session/drd_rdp_session.h"
#include "drd-dbus-remote-desktop.h"
#include "utils/drd_log.h"

typedef struct _DrdSystemDaemon DrdSystemDaemon;

typedef struct _DrdRemoteClient
{
    DrdSystemDaemon *daemon;
    gchar *id;
    gchar *object_path;
    DrdRoutingTokenInfo *routing;
    GSocketConnection *connection;
    DrdRdpSession *session;
    DrdDBusRemoteDesktopRdpHandover *handover_iface;
    GDBusObjectSkeleton *object_skeleton;
    gboolean assigned;
    gboolean use_system_credentials;
} DrdRemoteClient;

typedef struct
{
    DrdDBusRemoteDesktopRdpDispatcher *dispatcher;
    GDBusObjectManagerServer *handover_manager;
    guint bus_name_owner_id;
    GDBusConnection *connection;
} DrdSystemDaemonBusContext;

static void drd_system_daemon_remove_client(DrdSystemDaemon *self, DrdRemoteClient *client);
static void drd_system_daemon_queue_client(DrdSystemDaemon *self, DrdRemoteClient *client);
static void drd_system_daemon_unqueue_client(DrdSystemDaemon *self, DrdRemoteClient *client);
static gboolean drd_system_daemon_register_client(DrdSystemDaemon *self,
                                                  GSocketConnection *connection,
                                                  DrdRoutingTokenInfo *info);
static gboolean drd_system_daemon_delegate(DrdRdpListener *listener,
                                           GSocketConnection *connection,
                                           gpointer user_data,
                                           GError **error);
static void drd_system_daemon_on_session_ready(DrdRdpListener *listener,
                                               DrdRdpSession *session,
                                               GSocketConnection *connection,
                                               gpointer user_data);
static DrdRemoteClient *drd_system_daemon_find_client_by_token(DrdSystemDaemon *self,
                                                               const gchar *routing_token);
static gboolean drd_system_daemon_on_start_handover(DrdDBusRemoteDesktopRdpHandover *interface,
                                                   GDBusMethodInvocation *invocation,
                                                   const gchar *username,
                                                   const gchar *password,
                                                   gpointer user_data);
static gboolean drd_system_daemon_on_take_client(DrdDBusRemoteDesktopRdpHandover *interface,
                                                 GDBusMethodInvocation *invocation,
                                                 GUnixFDList *fd_list,
                                                 gpointer user_data);
static gboolean drd_system_daemon_on_get_system_credentials(DrdDBusRemoteDesktopRdpHandover *interface,
                                                           GDBusMethodInvocation *invocation,
                                                           gpointer user_data);
static void drd_system_daemon_ensure_routing_token(DrdSystemDaemon *self, DrdRemoteClient *client);

struct _DrdSystemDaemon
{
    GObject parent_instance;

    DrdConfig *config;
    DrdServerRuntime *runtime;
    DrdTlsCredentials *tls_credentials;

    DrdRdpListener *listener;
    DrdSystemDaemonBusContext bus;
    GHashTable *remote_clients;
    GQueue *pending_clients;
    guint client_counter;
};

G_DEFINE_TYPE(DrdSystemDaemon, drd_system_daemon, G_TYPE_OBJECT)

static void
drd_remote_client_free(DrdRemoteClient *client)
{
    if (client == NULL)
    {
        return;
    }

    g_clear_object(&client->object_skeleton);
    g_clear_object(&client->handover_iface);
    if (client->connection != NULL)
    {
        g_object_set_data(G_OBJECT(client->connection), "drd-system-client", NULL);
        g_object_set_data(G_OBJECT(client->connection), "drd-system-keep-open", NULL);
        g_clear_object(&client->connection);
    }
    g_clear_object(&client->session);
    drd_routing_token_info_free(client->routing);
    g_clear_pointer(&client->object_path, g_free);
    g_clear_pointer(&client->id, g_free);
    g_free(client);
}

static DrdRemoteClient *
drd_system_daemon_find_client_by_token(DrdSystemDaemon *self, const gchar *routing_token)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), NULL);
    g_return_val_if_fail(routing_token != NULL, NULL);

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->remote_clients);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        DrdRemoteClient *client = value;
        if (client->routing != NULL &&
            client->routing->routing_token != NULL &&
            g_strcmp0(client->routing->routing_token, routing_token) == 0)
        {
            return client;
        }
    }

    return NULL;
}

static gchar *
drd_system_daemon_generate_routing_token(DrdSystemDaemon *self)
{
    gchar *token = NULL;
    do
    {
        guint32 value = g_random_int();
        if (value == 0)
        {
            continue;
        }
        g_free(token);
        token = g_strdup_printf("%u", value);
    } while (token == NULL || drd_system_daemon_find_client_by_token(self, token) != NULL);
    return token;
}

static void
drd_system_daemon_ensure_routing_token(DrdSystemDaemon *self, DrdRemoteClient *client)
{
    if (client->routing == NULL)
    {
        client->routing = drd_routing_token_info_new();
    }

    if (client->routing->routing_token == NULL)
    {
        client->routing->routing_token = drd_system_daemon_generate_routing_token(self);
    }
}

static void
drd_system_daemon_queue_client(DrdSystemDaemon *self, DrdRemoteClient *client)
{
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));
    g_return_if_fail(client != NULL);

    if (!client->assigned)
    {
        DRD_LOG_MESSAGE("g_queue_push_tail client run");
        g_queue_push_tail(self->pending_clients, client);
    }
}

static void
drd_system_daemon_unqueue_client(DrdSystemDaemon *self, DrdRemoteClient *client)
{
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));
    g_return_if_fail(client != NULL);

    GList *link = g_queue_find(self->pending_clients, client);
    if (link != NULL)
    {
        g_queue_delete_link(self->pending_clients, link);
    }
}

static void
drd_system_daemon_remove_client(DrdSystemDaemon *self, DrdRemoteClient *client)
{
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));
    g_return_if_fail(client != NULL);

    drd_system_daemon_unqueue_client(self, client);

    if (self->bus.handover_manager != NULL && client->object_path != NULL)
    {
        g_dbus_object_manager_server_unexport(self->bus.handover_manager, client->object_path);
    }

    if (client->connection != NULL)
    {
        g_object_set_data(G_OBJECT(client->connection), "drd-system-client", NULL);
        g_object_set_data(G_OBJECT(client->connection), "drd-system-keep-open", NULL);
    }
    g_clear_object(&client->session);

    g_hash_table_remove(self->remote_clients, client->object_path);
}

static gboolean
drd_system_daemon_register_client(DrdSystemDaemon *self,
                                  GSocketConnection *connection,
                                  DrdRoutingTokenInfo *info)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);

    DrdRemoteClient *client = g_new0(DrdRemoteClient, 1);
    client->daemon = self;
    client->routing = info;
    client->connection = g_object_ref(connection);
    client->id = g_strdup_printf("session%u", ++self->client_counter);
    client->object_path = g_strdup_printf("%s/%s",
                                          DRD_REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH,
                                          client->id);

    client->session = NULL;
    client->use_system_credentials = FALSE;
    client->handover_iface = drd_dbus_remote_desktop_rdp_handover_skeleton_new();
    g_signal_connect(client->handover_iface,
                     "handle-start-handover",
                     G_CALLBACK(drd_system_daemon_on_start_handover),
                     client);
    g_signal_connect(client->handover_iface,
                     "handle-take-client",
                     G_CALLBACK(drd_system_daemon_on_take_client),
                     client);
    g_signal_connect(client->handover_iface,
                     "handle-get-system-credentials",
                     G_CALLBACK(drd_system_daemon_on_get_system_credentials),
                     client);

    client->object_skeleton = g_dbus_object_skeleton_new(client->object_path);
    g_dbus_object_skeleton_add_interface(client->object_skeleton,
                                         G_DBUS_INTERFACE_SKELETON(client->handover_iface));

    if (self->bus.handover_manager != NULL)
    {
        g_dbus_object_manager_server_export(self->bus.handover_manager,
                                            client->object_skeleton);
    }

    g_object_set_data(G_OBJECT(connection), "drd-system-client", client);
    g_object_set_data(G_OBJECT(connection), "drd-system-keep-open", GINT_TO_POINTER(1));

    drd_system_daemon_ensure_routing_token(self, client);

    g_hash_table_replace(self->remote_clients, g_strdup(client->object_path), client);
    DRD_LOG_MESSAGE("drd_system_daemon_queue_client run");
    drd_system_daemon_queue_client(self, client);

    const gchar *token_preview = client->routing != NULL && client->routing->routing_token != NULL
                                     ? client->routing->routing_token
                                     : "unknown";
    DRD_LOG_MESSAGE("Registered handover client %s (token=%s)",
                    client->object_path,
                    token_preview);
    return TRUE;
}
// return FALSE 时，需要继续处理这个connection;return TRUE时，代表已经处理过，需要让handover进程来处理；
static gboolean
drd_system_daemon_delegate(DrdRdpListener *listener,
                           GSocketConnection *connection,
                           gpointer user_data,
                           GError **error)
{
    (void)listener;
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), TRUE);

    g_autoptr(DrdRoutingTokenInfo) info = drd_routing_token_info_new();
    g_object_ref(connection);
    DRD_LOG_MESSAGE("drd_routing_token_peek run");
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    if (!drd_routing_token_peek(connection, cancellable, info, error))
    {
        g_object_unref(connection);
        return TRUE;
    }

    if (info->routing_token != NULL)
    {
        DrdRemoteClient *existing =
            drd_system_daemon_find_client_by_token(self, info->routing_token);
        if (existing != NULL && existing->session == NULL)
        {
            g_clear_object(&existing->connection);
            existing->connection = g_object_ref(connection);
            g_object_set_data(G_OBJECT(connection), "drd-system-client", existing);
            g_object_set_data(G_OBJECT(connection), "drd-system-keep-open", GINT_TO_POINTER(1));

            drd_dbus_remote_desktop_rdp_handover_emit_take_client_ready(
                existing->handover_iface,
                existing->use_system_credentials);
            g_object_unref(connection);
            return TRUE;
        }
    }

    DrdRoutingTokenInfo *owned_info = info;
    info = NULL;
    DRD_LOG_MESSAGE("drd_system_daemon_register_client run");
    if (!drd_system_daemon_register_client(self, connection, owned_info))
    {
        drd_routing_token_info_free(owned_info);
        g_object_unref(connection);
        return TRUE;
    }

    /* Allow the default listener to accept the connection so FreeRDP can build a session and send redirection. */
    g_object_unref(connection);
    return FALSE;
}

static void
drd_system_daemon_on_session_ready(DrdRdpListener *listener,
                                   DrdRdpSession *session,
                                   GSocketConnection *connection,
                                   gpointer user_data)
{
    (void)listener;
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    if (!DRD_IS_SYSTEM_DAEMON(self))
    {
        return;
    }

    DrdRemoteClient *client = g_object_get_data(G_OBJECT(connection), "drd-system-client");
    if (client == NULL)
    {
        return;
    }

    if (client->session != NULL)
    {
        g_clear_object(&client->session);
    }
    client->session = g_object_ref(session);
    if (client->routing != NULL)
    {
        client->use_system_credentials =
            drd_rdp_session_client_is_mstsc(session) && !client->routing->requested_rdstls;
    }
}

static gboolean
drd_system_daemon_load_tls_material(DrdSystemDaemon *self,
                                    gchar **certificate,
                                    gchar **key,
                                    GError **error)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);
    g_return_val_if_fail(certificate != NULL, FALSE);
    g_return_val_if_fail(key != NULL, FALSE);

    const gchar *cert_path = drd_tls_credentials_get_certificate_path(self->tls_credentials);
    const gchar *key_path = drd_tls_credentials_get_private_key_path(self->tls_credentials);
    if (cert_path == NULL || key_path == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "TLS credential paths unavailable");
        return FALSE;
    }

    if (!g_file_get_contents(cert_path, certificate, NULL, error))
    {
        return FALSE;
    }
    if (!g_file_get_contents(key_path, key, NULL, error))
    {
        g_clear_pointer(certificate, g_free);
        return FALSE;
    }
    return TRUE;
}

static gboolean
drd_system_daemon_handle_request_handover(DrdDBusRemoteDesktopRdpDispatcher *interface,
                                          GDBusMethodInvocation *invocation,
                                          gpointer user_data)
{
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    DrdRemoteClient *client = g_queue_pop_head(self->pending_clients);
    if (client == NULL)
    {
        g_dbus_method_invocation_return_error(invocation,
                                              G_IO_ERROR,
                                              G_IO_ERROR_NOT_FOUND,
                                              "No pending RDP handover requests");
        DRD_LOG_MESSAGE("request handover error");
        return TRUE;
    }

    client->assigned = TRUE;
    drd_dbus_remote_desktop_rdp_dispatcher_complete_request_handover(interface,
                                                                     invocation,
                                                                     client->object_path);
    DRD_LOG_MESSAGE("Dispatching handover client %s", client->object_path);
    return TRUE;
}

static void
drd_system_daemon_reset_bus_context(DrdSystemDaemon *self)
{
    if (self->bus.handover_manager != NULL)
    {
        g_dbus_object_manager_server_set_connection(self->bus.handover_manager, NULL);
        g_clear_object(&self->bus.handover_manager);
    }

    if (self->bus.dispatcher != NULL)
    {
        g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(self->bus.dispatcher));
        g_clear_object(&self->bus.dispatcher);
    }

    if (self->bus.bus_name_owner_id != 0)
    {
        g_bus_unown_name(self->bus.bus_name_owner_id);
        self->bus.bus_name_owner_id = 0;
    }

    g_clear_object(&self->bus.connection);
}

static void
drd_system_daemon_stop_listener(DrdSystemDaemon *self)
{
    if (self->listener != NULL)
    {
        drd_rdp_listener_stop(self->listener);
        g_clear_object(&self->listener);
    }
}

void
drd_system_daemon_stop(DrdSystemDaemon *self)
{
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));

    drd_system_daemon_reset_bus_context(self);
    drd_system_daemon_stop_listener(self);
    if (self->remote_clients != NULL)
    {
        g_hash_table_remove_all(self->remote_clients);
    }
    if (self->pending_clients != NULL)
    {
        g_queue_clear(self->pending_clients);
    }
}

static void
drd_system_daemon_dispose(GObject *object)
{
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(object);

    drd_system_daemon_stop(self);

    g_clear_object(&self->tls_credentials);
    g_clear_object(&self->runtime);
    g_clear_object(&self->config);
    if (self->pending_clients != NULL)
    {
        g_queue_free(self->pending_clients);
        self->pending_clients = NULL;
    }
    if (self->remote_clients != NULL)
    {
        g_hash_table_destroy(self->remote_clients);
        self->remote_clients = NULL;
    }

    G_OBJECT_CLASS(drd_system_daemon_parent_class)->dispose(object);
}

static void
drd_system_daemon_class_init(DrdSystemDaemonClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_system_daemon_dispose;
}

static void
drd_system_daemon_init(DrdSystemDaemon *self)
{
    self->bus.dispatcher = NULL;
    self->bus.handover_manager = NULL;
    self->bus.bus_name_owner_id = 0;
    self->bus.connection = NULL;
    self->remote_clients = g_hash_table_new_full(g_str_hash,
                                                 g_str_equal,
                                                 g_free,
                                                 (GDestroyNotify)drd_remote_client_free);
    self->pending_clients = g_queue_new();
    self->client_counter = 0;
}

DrdSystemDaemon *
drd_system_daemon_new(DrdConfig *config,
                      DrdServerRuntime *runtime,
                      DrdTlsCredentials *tls_credentials)
{
    g_return_val_if_fail(DRD_IS_CONFIG(config), NULL);
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(runtime), NULL);

    DrdSystemDaemon *self = g_object_new(DRD_TYPE_SYSTEM_DAEMON, NULL);
    self->config = g_object_ref(config);
    self->runtime = g_object_ref(runtime);
    if (tls_credentials != NULL)
    {
        self->tls_credentials = g_object_ref(tls_credentials);
    }
    return self;
}

static gboolean
drd_system_daemon_start_listener(DrdSystemDaemon *self, GError **error)
{
    if (self->listener != NULL)
    {
        return TRUE;
    }

    const DrdEncodingOptions *encoding_opts = drd_config_get_encoding_options(self->config);
    if (encoding_opts == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoding options unavailable when starting system daemon listener");
        return FALSE;
    }

    self->listener = drd_rdp_listener_new(drd_config_get_bind_address(self->config),
                                          drd_config_get_port(self->config),
                                          self->runtime,
                                          encoding_opts,
                                          drd_config_is_nla_enabled(self->config),
                                          drd_config_get_nla_username(self->config),
                                          drd_config_get_nla_password(self->config),
                                          drd_config_get_pam_service(self->config),
                                          TRUE);
    if (self->listener == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to create system-mode RDP listener");
        return FALSE;
    }

    if (!drd_rdp_listener_start(self->listener, error))
    {
        g_clear_object(&self->listener);
        return FALSE;
    }
    drd_rdp_listener_set_delegate(self->listener, drd_system_daemon_delegate, self);
    drd_rdp_listener_set_session_callback(self->listener,
                                          drd_system_daemon_on_session_ready,
                                          self);

    DRD_LOG_MESSAGE("System daemon listening on %s:%u",
              drd_config_get_bind_address(self->config),
              drd_config_get_port(self->config));
    return TRUE;
}

static gboolean
drd_system_daemon_start_bus(DrdSystemDaemon *self, GError **error)
{
    g_assert(self->bus.connection == NULL);

    self->bus.connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    if (self->bus.connection == NULL)
    {
        return FALSE;
    }

    self->bus.bus_name_owner_id =
        g_bus_own_name_on_connection(self->bus.connection,
                                     DRD_REMOTE_DESKTOP_BUS_NAME,
                                     G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL);

    if (self->bus.bus_name_owner_id == 0)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to own org.deepin.RemoteDesktop bus name");
        return FALSE;
    }

    self->bus.dispatcher = drd_dbus_remote_desktop_rdp_dispatcher_skeleton_new();
    g_signal_connect(self->bus.dispatcher,
                     "handle-request-handover",
                     G_CALLBACK(drd_system_daemon_handle_request_handover),
                     self);

    // TODO
    g_bus_own_name (G_BUS_TYPE_SYSTEM,
                   "org.deepin.RemoteDesktop",
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   NULL,
                   NULL,
                   NULL,
                   self, NULL);

    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(self->bus.dispatcher),
                                          self->bus.connection,
                                          DRD_REMOTE_DESKTOP_DISPATCHER_OBJECT_PATH,
                                          error))
    {
        return FALSE;
    }

    self->bus.handover_manager =
        g_dbus_object_manager_server_new(DRD_REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH);
    g_dbus_object_manager_server_set_connection(self->bus.handover_manager,
                                                self->bus.connection);

    DRD_LOG_MESSAGE("System daemon exported dispatcher at %s",
              DRD_REMOTE_DESKTOP_DISPATCHER_OBJECT_PATH);
    return TRUE;
}

gboolean
drd_system_daemon_start(DrdSystemDaemon *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);

    if (!drd_system_daemon_start_listener(self, error))
    {
        return FALSE;
    }

    if (self->bus.connection != NULL)
    {
        return TRUE;
    }

    if (!drd_system_daemon_start_bus(self, error))
    {
        drd_system_daemon_stop_listener(self);
        drd_system_daemon_reset_bus_context(self);
        return FALSE;
    }

    return TRUE;
}

static gboolean
drd_system_daemon_on_start_handover(DrdDBusRemoteDesktopRdpHandover *interface,
                                    GDBusMethodInvocation *invocation,
                                    const gchar *username,
                                    const gchar *password,
                                    gpointer user_data)
{
    DrdRemoteClient *client = user_data;
    DrdSystemDaemon *self = client->daemon;
    g_autofree gchar *certificate = NULL;
    g_autofree gchar *key = NULL;
    gboolean redirected_locally = FALSE;
    drd_system_daemon_ensure_routing_token(self, client);
    const gboolean has_routing_token =
        client->routing != NULL && client->routing->routing_token != NULL;

    g_autoptr(GError) io_error = NULL;
    if (!drd_system_daemon_load_tls_material(self, &certificate, &key, &io_error))
    {
        g_dbus_method_invocation_return_gerror(invocation, io_error);
        return TRUE;
    }

    if (client->session != NULL)
    {
        DRD_LOG_MESSAGE("client session not NULL");
        if (!drd_rdp_session_send_server_redirection(client->session,
                                                     client->routing->routing_token,
                                                     username,
                                                     password,
                                                     certificate))
        {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_IO_ERROR,
                                                  G_IO_ERROR_FAILED,
                                                  "Failed to redirect client session");
            return TRUE;
        }
        drd_rdp_session_notify_error(client->session, DRD_RDP_SESSION_ERROR_SERVER_REDIRECTION);
        g_clear_object(&client->session);
        if (client->connection != NULL)
        {
            g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
            g_clear_object(&client->connection);
        }
        redirected_locally = TRUE;
    }
    else
    {
        DRD_LOG_MESSAGE("client session is NULL");
        if (has_routing_token)
        {
            drd_dbus_remote_desktop_rdp_handover_emit_redirect_client(interface,
                                                                      client->routing->routing_token,
                                                                      username,
                                                                      password);
        }
        else
        {
            DRD_LOG_WARNING("StartHandover for %s missing routing token; skipping RedirectClient signal",
                            client->object_path);
        }
    }

    drd_dbus_remote_desktop_rdp_handover_complete_start_handover(interface,
                                                                 invocation,
                                                                 certificate,
                                                                 key);

    if (redirected_locally)
    {
        client->assigned = TRUE;
    }

    DRD_LOG_MESSAGE("StartHandover acknowledged for %s", client->object_path);
    return TRUE;
}

static gboolean
drd_system_daemon_on_take_client(DrdDBusRemoteDesktopRdpHandover *interface,
                                 GDBusMethodInvocation *invocation,
                                 GUnixFDList *fd_list,
                                 gpointer user_data)
{
    (void)fd_list;
    DrdRemoteClient *client = user_data;
    DrdSystemDaemon *self = client->daemon;
    GSocket *socket = g_socket_connection_get_socket(client->connection);
    if (!G_IS_SOCKET(socket))
    {
        g_dbus_method_invocation_return_error(invocation,
                                              G_IO_ERROR,
                                              G_IO_ERROR_FAILED,
                                              "Socket unavailable for client");
        return TRUE;
    }

    g_autoptr(GUnixFDList) out_list = g_unix_fd_list_new();
    g_autoptr(GError) local_error = NULL;
    gint idx = g_unix_fd_list_append(out_list, g_socket_get_fd(socket), &local_error);
    if (idx == -1)
    {
        g_dbus_method_invocation_return_gerror(invocation, local_error);
        return TRUE;
    }

    g_autoptr(GVariant) handle = g_variant_new_handle(idx);
    drd_dbus_remote_desktop_rdp_handover_complete_take_client(interface,
                                                              invocation,
                                                              out_list,
                                                              handle);

    g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
    g_clear_object(&client->session);
    drd_system_daemon_remove_client(self, client);
    DRD_LOG_MESSAGE("Socket handed over for client %s", client->object_path);
    return TRUE;
}

static gboolean
drd_system_daemon_on_get_system_credentials(DrdDBusRemoteDesktopRdpHandover *interface,
                                            GDBusMethodInvocation *invocation,
                                            gpointer user_data)
{
    (void)interface;
    (void)user_data;
    g_dbus_method_invocation_return_error(invocation,
                                          G_IO_ERROR,
                                          G_IO_ERROR_NOT_SUPPORTED,
                                          "System credentials not available");
    return TRUE;
}
