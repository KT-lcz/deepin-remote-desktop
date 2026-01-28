
#include "core/drd_user_dbus_service.h"

#include <gio/gio.h>

#include "core/drd_dbus_constants.h"
#include "drd-dbus-remote-desktop1.h"
#include "drd_build_config.h"

#ifndef DRD_PROJECT_VERSION
#define DRD_PROJECT_VERSION "unknown"
#endif

struct _DrdUserDbusService
{
    GObject parent_instance;

    DrdConfig *config;

    GDBusConnection *connection;
    guint bus_name_owner_id;

    DrdDBusRemoteDesktop1RemoteDesktop1 *common_iface;
    DrdDBusRemoteDesktop1RemoteDesktop1Shadow *shadow_iface;
};

G_DEFINE_TYPE(DrdUserDbusService, drd_user_dbus_service, G_TYPE_OBJECT)

static gboolean drd_user_dbus_method_not_supported(GDBusMethodInvocation *invocation, const gchar *method_name)
{
    g_return_val_if_fail(invocation != NULL, TRUE);

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "%s not implemented",
                                          method_name != NULL ? method_name : "Method");
    return TRUE;
}

static void drd_user_dbus_service_reset_bus_context(DrdUserDbusService *self)
{
    if (self->common_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(self->common_iface));
        g_clear_object(&self->common_iface);
    }

    if (self->shadow_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(self->shadow_iface));
        g_clear_object(&self->shadow_iface);
    }

    if (self->bus_name_owner_id != 0)
    {
        g_bus_unown_name(self->bus_name_owner_id);
        self->bus_name_owner_id = 0;
    }
    g_clear_object(&self->connection);
}

static void drd_user_dbus_service_dispose(GObject *object)
{
    DrdUserDbusService *self = DRD_USER_DBUS_SERVICE(object);
    drd_user_dbus_service_reset_bus_context(self);
    g_clear_object(&self->config);
    G_OBJECT_CLASS(drd_user_dbus_service_parent_class)->dispose(object);
}

static void drd_user_dbus_service_class_init(DrdUserDbusServiceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_user_dbus_service_dispose;
}

static void drd_user_dbus_service_init(DrdUserDbusService *self)
{
    self->connection = NULL;
    self->bus_name_owner_id = 0;
    self->common_iface = NULL;
    self->shadow_iface = NULL;
}

DrdUserDbusService *drd_user_dbus_service_new(DrdConfig *config)
{
    g_return_val_if_fail(DRD_IS_CONFIG(config), NULL);

    DrdUserDbusService *self = g_object_new(DRD_TYPE_USER_DBUS_SERVICE, NULL);
    self->config = g_object_ref(config);
    return self;
}

static gboolean drd_user_dbus_shadow_handle_stub(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                 GDBusMethodInvocation *invocation, gpointer user_data,
                                                 const gchar *method_name)
{
    (void) interface;
    (void) user_data;
    return drd_user_dbus_method_not_supported(invocation, method_name);
}

static gboolean drd_user_dbus_shadow_handle_enable_shadow(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                          GDBusMethodInvocation *invocation, gboolean enable,
                                                          gpointer user_data)
{
    (void) enable;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "EnableShadow");
}

static gboolean drd_user_dbus_shadow_handle_get_credentials(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                            GDBusMethodInvocation *invocation, GUnixFDList *fd_list,
                                                            gpointer user_data)
{
    (void) fd_list;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "GetCredentials");
}

static gboolean drd_user_dbus_shadow_handle_set_credentials(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                            GDBusMethodInvocation *invocation, GUnixFDList *fd_list,
                                                            GVariant *credentials_fd, gpointer user_data)
{
    (void) fd_list;
    (void) credentials_fd;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "SetCredentials");
}

static gboolean
drd_user_dbus_shadow_handle_enable_allow_client_take_control(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                             GDBusMethodInvocation *invocation, gboolean enable,
                                                             gpointer user_data)
{
    (void) enable;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "EnableAllowClientTakeControl");
}

static gboolean
drd_user_dbus_shadow_handle_enable_local_control_first(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                       GDBusMethodInvocation *invocation, gboolean enable,
                                                       gpointer user_data)
{
    (void) enable;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "EnableLocalControlFirst");
}

static gboolean
drd_user_dbus_shadow_handle_enable_auto_lock_on_connect(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                        GDBusMethodInvocation *invocation, gboolean enable,
                                                        gpointer user_data)
{
    (void) enable;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "EnableAutoLockOnConnect");
}

static gboolean
drd_user_dbus_shadow_handle_enable_lock_on_disconnect(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                      GDBusMethodInvocation *invocation, gboolean enable,
                                                      gpointer user_data)
{
    (void) enable;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "EnableLockOnDisconnect");
}

static gboolean
drd_user_dbus_shadow_handle_enable_connect_with_credentials(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                            GDBusMethodInvocation *invocation,
                                                            gboolean need_credentials, gpointer user_data)
{
    (void) need_credentials;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "EnableConnectWithCredentials");
}

static gboolean
drd_user_dbus_shadow_handle_set_nla_update_interval(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                    GDBusMethodInvocation *invocation, gint interval,
                                                    gpointer user_data)
{
    (void) interval;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "SetNlaUpdateInterval");
}

static gboolean
drd_user_dbus_shadow_handle_switch_connection_state(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                    GDBusMethodInvocation *invocation, gint state, gpointer user_data)
{
    (void) state;
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "SwitchConnectionState");
}

static gboolean drd_user_dbus_shadow_handle_gen_nla_credential(DrdDBusRemoteDesktop1RemoteDesktop1Shadow *interface,
                                                               GDBusMethodInvocation *invocation, gpointer user_data)
{
    return drd_user_dbus_shadow_handle_stub(interface, invocation, user_data, "GenNlaCredential");
}

gboolean drd_user_dbus_service_start(DrdUserDbusService *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_USER_DBUS_SERVICE(self), FALSE);
    g_assert(self->connection == NULL);

    self->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (self->connection == NULL)
    {
        return FALSE;
    }

    g_object_ref(self);
    self->bus_name_owner_id =
            g_bus_own_name_on_connection(self->connection, DRD_REMOTE_DESKTOP_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                         NULL, NULL, self, g_object_unref);
    if (self->bus_name_owner_id == 0)
    {
        g_object_unref(self);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to own org.deepin.RemoteDesktop1 on session bus");
        return FALSE;
    }

    self->common_iface = drd_dbus_remote_desktop1_remote_desktop1_skeleton_new();
    drd_dbus_remote_desktop1_remote_desktop1_set_runtime_mode(self->common_iface, "user");
    drd_dbus_remote_desktop1_remote_desktop1_set_version(self->common_iface, DRD_PROJECT_VERSION);

    self->shadow_iface = drd_dbus_remote_desktop1_remote_desktop1_shadow_skeleton_new();

    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_enabled(self->shadow_iface, TRUE);
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_port(self->shadow_iface,
                                                             (gint) drd_config_get_port(self->config));
    {
        const gchar *const empty_iplist[] = {NULL};
        drd_dbus_remote_desktop1_remote_desktop1_shadow_set_iplist(self->shadow_iface, empty_iplist);
    }
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_tls_cert(self->shadow_iface,
                                                                 drd_config_get_certificate_path(self->config));
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_tls_key(self->shadow_iface,
                                                                drd_config_get_private_key_path(self->config));
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_tls_fingerprint(self->shadow_iface, "");
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_nla_auth_enabled(self->shadow_iface,
                                                                         drd_config_is_nla_enabled(self->config));

    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_local_control_first(self->shadow_iface, FALSE);
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_allow_client_take_control(self->shadow_iface, FALSE);
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_auto_lock_on_connect(self->shadow_iface, FALSE);
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_lock_on_disconnect(self->shadow_iface, FALSE);
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_nla_update_interval(self->shadow_iface, 0);
    drd_dbus_remote_desktop1_remote_desktop1_shadow_set_connection_state(self->shadow_iface, 0);

    g_signal_connect(self->shadow_iface, "handle-enable-shadow", G_CALLBACK(drd_user_dbus_shadow_handle_enable_shadow),
                     self);
    g_signal_connect(self->shadow_iface, "handle-enable-allow-client-take-control",
                     G_CALLBACK(drd_user_dbus_shadow_handle_enable_allow_client_take_control), self);
    g_signal_connect(self->shadow_iface, "handle-enable-local-control-first",
                     G_CALLBACK(drd_user_dbus_shadow_handle_enable_local_control_first), self);
    g_signal_connect(self->shadow_iface, "handle-enable-auto-lock-on-connect",
                     G_CALLBACK(drd_user_dbus_shadow_handle_enable_auto_lock_on_connect), self);
    g_signal_connect(self->shadow_iface, "handle-enable-lock-on-disconnect",
                     G_CALLBACK(drd_user_dbus_shadow_handle_enable_lock_on_disconnect), self);
    g_signal_connect(self->shadow_iface, "handle-get-credentials",
                     G_CALLBACK(drd_user_dbus_shadow_handle_get_credentials), self);
    g_signal_connect(self->shadow_iface, "handle-set-credentials",
                     G_CALLBACK(drd_user_dbus_shadow_handle_set_credentials), self);
    g_signal_connect(self->shadow_iface, "handle-enable-connect-with-credentials",
                     G_CALLBACK(drd_user_dbus_shadow_handle_enable_connect_with_credentials), self);
    g_signal_connect(self->shadow_iface, "handle-set-nla-update-interval",
                     G_CALLBACK(drd_user_dbus_shadow_handle_set_nla_update_interval), self);
    g_signal_connect(self->shadow_iface, "handle-switch-connection-state",
                     G_CALLBACK(drd_user_dbus_shadow_handle_switch_connection_state), self);
    g_signal_connect(self->shadow_iface, "handle-gen-nla-credential",
                     G_CALLBACK(drd_user_dbus_shadow_handle_gen_nla_credential), self);

    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(self->common_iface), self->connection,
                                          DRD_REMOTE_DESKTOP_OBJECT_PATH, error))
    {
        drd_user_dbus_service_reset_bus_context(self);
        return FALSE;
    }

    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(self->shadow_iface), self->connection,
                                          DRD_REMOTE_DESKTOP_SHADOW_OBJECT_PATH, error))
    {
        drd_user_dbus_service_reset_bus_context(self);
        return FALSE;
    }
    return TRUE;
}

void drd_user_dbus_service_stop(DrdUserDbusService *self)
{
    g_return_if_fail(DRD_IS_USER_DBUS_SERVICE(self));
    drd_user_dbus_service_reset_bus_context(self);
}
