#include "system/drd_system_daemon.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <string.h>

#include "core/drd_dbus_constants.h"
#include "transport/drd_rdp_listener.h"
#include "transport/drd_rdp_routing_token.h"
#include "session/drd_rdp_session.h"
#include "drd-dbus-remote-desktop.h"
#include "drd-dbus-lightdm.h"
#include "utils/drd_log.h"

typedef struct _DrdSystemDaemon DrdSystemDaemon;

typedef struct _DrdRemoteClient
{
    DrdSystemDaemon *daemon;
    gchar *id;
    DrdRoutingTokenInfo *routing;
    GSocketConnection *connection;
    DrdRdpSession *session;
    DrdDBusRemoteDesktopRdpHandover *handover_iface;
    GDBusObjectSkeleton *object_skeleton;
    gboolean assigned;
    gboolean use_system_credentials;
    guint handover_count;
} DrdRemoteClient;

typedef struct
{
    DrdDBusRemoteDesktopRdpDispatcher *dispatcher;
    GDBusObjectManagerServer *handover_manager;
    guint bus_name_owner_id;
    GDBusConnection *connection;
} DrdSystemDaemonBusContext;

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

    DrdDBusLightdmRemoteDisplayFactory *remote_display_factory;
    GMainLoop *main_loop;
};

static void drd_system_daemon_request_shutdown(DrdSystemDaemon *self);

static gchar *
get_id_from_routing_token(guint32 routing_token)
{
    g_return_val_if_fail(routing_token != 0, NULL);

    return g_strdup_printf("%s/%u", DRD_REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH, routing_token);
}

static gchar *
get_routing_token_from_id(const gchar *id)
{
    const gchar *prefix = DRD_REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH
    "/";
    gsize prefix_len;

    g_return_val_if_fail(id != NULL, NULL);

    prefix_len = strlen(prefix);
    if (!g_str_has_prefix(id, prefix))
    {
        DRD_LOG_WARNING("remote id %s missing handover prefix %s", id, prefix);
        return NULL;
    }

    if (id[prefix_len] == '\0')
    {
        DRD_LOG_WARNING("remote id %s missing routing token segment", id);
        return NULL;
    }

    return g_strdup(id + prefix_len);
}

static gboolean
drd_system_daemon_generate_remote_identity(DrdSystemDaemon *self,
                                           gchar **remote_id_out,
                                           gchar **routing_token_out)
{
    g_autofree gchar *remote_id = NULL;
    gchar *routing_token = NULL;

    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);
    g_return_val_if_fail(remote_id_out != NULL, FALSE);
    g_return_val_if_fail(routing_token_out != NULL, FALSE);

    while (TRUE)
    {
        guint32 routing_token_value = g_random_int();

        if (routing_token_value == 0)
        {
            continue;
        }

        g_clear_pointer(&remote_id, g_free);
        remote_id = get_id_from_routing_token(routing_token_value);
        if (remote_id == NULL)
        {
            continue;
        }

        if (!g_hash_table_contains(self->remote_clients, remote_id))
        {
            break;
        }
    }

    routing_token = get_routing_token_from_id(remote_id);
    if (routing_token == NULL)
    {
        return FALSE;
    }

    *remote_id_out = g_steal_pointer(&remote_id);
    *routing_token_out = routing_token;
    return TRUE;
}

static void drd_system_daemon_remove_client(DrdSystemDaemon *self, DrdRemoteClient *client);

static void drd_system_daemon_queue_client(DrdSystemDaemon *self, DrdRemoteClient *client);

static void drd_system_daemon_unqueue_client(DrdSystemDaemon *self, DrdRemoteClient *client);

static gboolean drd_system_daemon_register_client(DrdSystemDaemon * self,
                                                  GSocketConnection * connection,
                                                  DrdRoutingTokenInfo * info);

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
    g_clear_pointer(&client->id, g_free);
    g_free(client);
}

static DrdRemoteClient *
drd_system_daemon_find_client_by_token(DrdSystemDaemon *self, const gchar *routing_token)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), NULL);
    g_return_val_if_fail(routing_token != NULL, NULL);

    g_autofree gchar *remote_id = NULL;
    guint64 parsed_token = 0;
    gboolean success = FALSE;

    success = g_ascii_string_to_unsigned(routing_token, 10, 1, G_MAXUINT32, &parsed_token, NULL);
    if (!success)
    {
        DRD_LOG_WARNING("Invalid routing token string %s", routing_token);
        return NULL;
    }

    remote_id = get_id_from_routing_token((guint32) parsed_token);
    if (remote_id == NULL)
    {
        return NULL;
    }

    return g_hash_table_lookup(self->remote_clients, remote_id);
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

    if (self->bus.handover_manager != NULL && client->id != NULL)
    {
        g_dbus_object_manager_server_unexport(self->bus.handover_manager, client->id);
    }

    if (client->connection != NULL)
    {
        g_object_set_data(G_OBJECT(client->connection), "drd-system-client", NULL);
        g_object_set_data(G_OBJECT(client->connection), "drd-system-keep-open", NULL);
    }
    g_clear_object(&client->session);

    g_hash_table_remove(self->remote_clients, client->id);
}

static gboolean
drd_system_daemon_register_client(DrdSystemDaemon *self,
                                  GSocketConnection *connection,
                                  DrdRoutingTokenInfo *info)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);
    g_return_val_if_fail(info != NULL, FALSE);

    DrdRemoteClient *client = g_new0(DrdRemoteClient, 1);
    g_autofree gchar *remote_id = NULL;
    g_autofree gchar *routing_token = NULL;
    guint64 parsed_token_value = 0;

    client->daemon = self;
    client->connection = g_object_ref(connection);
    client->routing = drd_routing_token_info_new();
    client->routing->requested_rdstls = info->requested_rdstls;

    if (info->routing_token != NULL)
    {
        if (g_ascii_string_to_unsigned(info->routing_token,
                                       10,
                                       1,
                                       G_MAXUINT32,
                                       &parsed_token_value,
                                       NULL))
        {
            remote_id = get_id_from_routing_token((guint32) parsed_token_value);
            routing_token = g_strdup(info->routing_token);
            if (remote_id != NULL && g_hash_table_contains(self->remote_clients, remote_id))
            {
                DRD_LOG_WARNING("Routing token %s already tracked, generating a new one",
                                info->routing_token);
                g_clear_pointer(&remote_id, g_free);
                g_clear_pointer(&routing_token, g_free);
            }
        }
        else
        {
            DRD_LOG_WARNING("Ignoring invalid routing token %s from peek", info->routing_token);
        }
    }

    if (remote_id == NULL || routing_token == NULL)
    {
        if (!drd_system_daemon_generate_remote_identity(self, &remote_id, &routing_token))
        {
            DRD_LOG_WARNING("Unable to allocate remote identity for new handover client");
            drd_routing_token_info_free(client->routing);
            g_clear_object(&client->connection);
            g_free(client);
            return FALSE;
        }
    }

    client->id = g_steal_pointer(&remote_id);
    client->routing->routing_token = g_steal_pointer(&routing_token);

    client->use_system_credentials = FALSE;
    client->handover_count = 0;
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

    client->object_skeleton = g_dbus_object_skeleton_new(client->id);
    g_dbus_object_skeleton_add_interface(client->object_skeleton,
                                         G_DBUS_INTERFACE_SKELETON(client->handover_iface));

    if (self->bus.handover_manager != NULL)
    {
        g_dbus_object_manager_server_export(self->bus.handover_manager,
                                            client->object_skeleton);
    }

    g_object_set_data(G_OBJECT(connection), "drd-system-client", client);
    g_object_set_data(G_OBJECT(connection), "drd-system-keep-open", GINT_TO_POINTER(1));

    g_hash_table_replace(self->remote_clients, g_strdup(client->id), client);
    DRD_LOG_MESSAGE("drd_system_daemon_queue_client run");
    drd_system_daemon_queue_client(self, client);

    const gchar *token_preview = client->routing != NULL && client->routing->routing_token != NULL
                                     ? client->routing->routing_token
                                     : "unknown";
    DRD_LOG_MESSAGE("Registered handover client %s (token=%s)",
                    client->id,
                    token_preview);

    // call lightdm create remote display
    if (!self->remote_display_factory)
        self->remote_display_factory = drd_dbus_lightdm_remote_display_factory_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
            DRD_LIGHTDM_REMOTE_FACTORY_BUS_NAME,
            DRD_LIGHTDM_REMOTE_FACTORY_OBJECT_PATH,
            NULL,
            NULL);
    g_autofree gchar*session_path = NULL;
    g_autofree GError *error = NULL;
    if (!drd_dbus_lightdm_remote_display_factory_call_create_remote_greeter_display_sync(self->remote_display_factory,
        g_random_int_range(0, 128), 1920, 1080, "0.0.0.0", &session_path, NULL, &error))
    {
        DRD_LOG_WARNING("create remote display failed %s", error->message);
        return TRUE; // Debug mode
    }
    DRD_LOG_MESSAGE("session_path=%s", session_path);

    return TRUE;
}

// return FALSE 时，需要继续处理这个connection;return TRUE时，代表已经处理过，需要让handover进程来处理；
static gboolean
drd_system_daemon_delegate(DrdRdpListener *listener,
                           GSocketConnection *connection,
                           gpointer user_data,
                           GError **error)
{
    (void) listener;
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), TRUE);

    g_autoptr(DrdRoutingTokenInfo)
    info = drd_routing_token_info_new();
    g_object_ref(connection);
    DRD_LOG_MESSAGE("drd_routing_token_peek run");
    g_autoptr(GCancellable)
    cancellable = g_cancellable_new();
    if (!drd_routing_token_peek(connection, cancellable, info, error))
    {
        g_object_unref(connection);
        return TRUE;
    }

    if (info->routing_token != NULL)
    {
        // 第二次进
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

    DRD_LOG_MESSAGE("drd_system_daemon_register_client run");
    if (!drd_system_daemon_register_client(self, connection, info))
    {
        g_clear_pointer(&info, drd_routing_token_info_free);
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
    (void) listener;
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

    if (self->tls_credentials == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "TLS credentials unavailable");
        return FALSE;
    }

    return drd_tls_credentials_read_material(self->tls_credentials, certificate, key, error);
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
                                                                     client->id);
    DRD_LOG_MESSAGE("Dispatching handover client %s", client->id);
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

static void
drd_system_daemon_request_shutdown(DrdSystemDaemon *self)
{
    if (self->main_loop != NULL && g_main_loop_is_running(self->main_loop))
    {
        DRD_LOG_MESSAGE("System daemon shutting down main loop");
        g_main_loop_quit(self->main_loop);
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

    drd_system_daemon_request_shutdown(self);
}

static void
drd_system_daemon_dispose(GObject *object)
{
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(object);

    drd_system_daemon_stop(self);

    g_clear_object(&self->tls_credentials);
    g_clear_object(&self->runtime);
    g_clear_object(&self->config);
    g_clear_pointer(&self->main_loop, g_main_loop_unref);
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
                                                 (GDestroyNotify) drd_remote_client_free);
    self->pending_clients = g_queue_new();
    self->main_loop = NULL;
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

gboolean
drd_system_daemon_set_main_loop(DrdSystemDaemon *self, GMainLoop *loop)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);

    if (self->main_loop != NULL)
    {
        g_main_loop_unref(self->main_loop);
        self->main_loop = NULL;
    }

    if (loop != NULL)
    {
        self->main_loop = g_main_loop_ref(loop);
    }

    return TRUE;
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
                                          DRD_RUNTIME_MODE_SYSTEM);
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
    g_bus_own_name(G_BUS_TYPE_SYSTEM,
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
    const gboolean has_routing_token =
            client->routing != NULL && client->routing->routing_token != NULL;

    g_autoptr(GError)
    io_error = NULL;
    if (!drd_system_daemon_load_tls_material(self, &certificate, &key, &io_error))
    {
        g_dbus_method_invocation_return_gerror(invocation, io_error);
        return TRUE;
    }

    if (client->session != NULL)
    {
        // gchar *gen_routing_token=g_strdup (client->id + strlen (REMOTE_DESKTOP_CLIENT_OBJECT_PATH "/"));
        DRD_LOG_MESSAGE("client session not NULL,routing token is %s", client->routing->routing_token);
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
            //            g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
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
                            client->id);
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

    DRD_LOG_MESSAGE("StartHandover acknowledged for %s", client->id);
    return TRUE;
}

static gboolean
drd_system_daemon_on_take_client(DrdDBusRemoteDesktopRdpHandover *interface,
                                 GDBusMethodInvocation *invocation,
                                 GUnixFDList *fd_list,
                                 gpointer user_data)
{
    (void) fd_list;
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

    g_autoptr(GUnixFDList)
    out_list = g_unix_fd_list_new();
    g_autoptr(GError)
    local_error = NULL;
    gint idx = g_unix_fd_list_append(out_list, g_socket_get_fd(socket), &local_error);
    if (idx == -1)
    {
        g_dbus_method_invocation_return_gerror(invocation, local_error);
        return TRUE;
    }

    g_autoptr(GVariant)
    handle = g_variant_new_handle(idx);
    drd_dbus_remote_desktop_rdp_handover_complete_take_client(interface,
                                                              invocation,
                                                              out_list,
                                                              handle);

    g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
    g_clear_object(&client->connection);
    g_clear_object(&client->session);

    client->handover_count++;
    if (client->handover_count >= 2)
    {
        DRD_LOG_MESSAGE("TODO remove client %s", client->id);
        // drd_system_daemon_remove_client(self, client);
    }
    else
    {
        client->assigned = FALSE;
        drd_system_daemon_queue_client(self, client);
        DRD_LOG_MESSAGE("Client %s ready for next handover stage", client->id);
    }

    return TRUE;
}

static gboolean
drd_system_daemon_on_get_system_credentials(DrdDBusRemoteDesktopRdpHandover *interface,
                                            GDBusMethodInvocation *invocation,
                                            gpointer user_data)
{
    (void) interface;
    (void) user_data;
    g_dbus_method_invocation_return_error(invocation,
                                          G_IO_ERROR,
                                          G_IO_ERROR_NOT_SUPPORTED,
                                          "System credentials not available");
    return TRUE;
}
