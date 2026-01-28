#include "system/drd_system_daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/drd_dbus_constants.h"
#include "drd-dbus-lightdm.h"
#include "drd-dbus-logind.h"
#include "drd-dbus-remote-desktop1.h"
#include "drd_build_config.h"
#include "security/drd_pam_auth.h"
#include "session/drd_rdp_session.h"
#include "transport/drd_rdp_listener.h"
#include "transport/drd_rdp_routing_token.h"
#include "utils/drd_dbus_auth_token.h"
#include "utils/drd_log.h"

#ifndef DRD_PROJECT_VERSION
#define DRD_PROJECT_VERSION "unknown"
#endif

typedef struct _DrdSystemDaemon DrdSystemDaemon;

#define DRD_SYSTEM_MAX_PENDING_CLIENTS 32
#define DRD_SYSTEM_CLIENT_STALE_TIMEOUT_US (30 * G_USEC_PER_SEC)

typedef struct _DrdRemoteClient
{
    DrdSystemDaemon *daemon;
    gchar *handover_dbus_path;
    DrdRoutingTokenInfo *routing;
    GSocketConnection *connection;
    DrdRdpSession *session;
    DrdDBusRemoteDesktop1RemoteDesktop1HandoverSession *handover_iface;
    GDBusObjectSkeleton *object_skeleton;
    gboolean assigned;
    gboolean use_system_credentials;
    guint handover_count;
    gint64 last_activity_us;
    guint32 client_width;
    guint32 client_height;

    gchar *lightdm_session_path;
    DrdDBusLightdmRemoteDisplayFactorySession *lightdm_session_proxy;
} DrdRemoteClient; // 实际应该作为 handover 对象的抽象

typedef struct
{
    DrdDBusRemoteDesktop1RemoteDesktop1 *common_iface;
    DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *remote_login_iface;
    GDBusObjectManagerServer *object_manager;
    GDBusObjectSkeleton *root_object;
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

static gchar *get_dbus_path_from_routing_token(guint32 routing_token)
{
    /*
     * 功能：根据 routing token 构造 handover 对象路径。
     * 逻辑：校验 token 非零后拼接 handover 基础路径与数值。
     * 参数：routing_token 路由 token 数值。
     * 外部接口：GLib g_strdup_printf。
     */
    g_return_val_if_fail(routing_token != 0, NULL);

    return g_strdup_printf("%s%u", DRD_REMOTE_DESKTOP_HANDOVER_SESSION_PATH_PREFIX, routing_token);
}

static gchar *drd_system_daemon_dup_peer_ip(GSocketConnection *connection)
{
    if (!G_IS_SOCKET_CONNECTION(connection))
    {
        return g_strdup("");
    }

    g_autoptr(GSocketAddress) addr = g_socket_connection_get_remote_address(connection, NULL);
    if (!G_IS_INET_SOCKET_ADDRESS(addr))
    {
        return g_strdup("");
    }

    GInetAddress *inet_addr = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(addr));
    if (inet_addr == NULL)
    {
        return g_strdup("");
    }

    g_autofree gchar *ip = g_inet_address_to_string(inet_addr);
    return ip != NULL ? g_strdup(ip) : g_strdup("");
}

static gboolean drd_system_daemon_is_graphical_session_type(const gchar *type)
{
    return g_strcmp0(type, "x11") == 0 || g_strcmp0(type, "wayland") == 0 || g_strcmp0(type, "mir") == 0;
}

static gboolean drd_system_daemon_collect_local_graphical_sessions(DrdSystemDaemon *self, const gchar *username,
                                                                   DrdDBusLogindManager **manager_out,
                                                                   GPtrArray **sessions_out, GError **error)
{
    /*
     * 功能：收集指定用户的本地图形会话 ID 列表。
     * 逻辑：通过 logind 列举会话，筛选 Remote=false 且 Type=x11/wayland/mir 的本地会话，返回会话 ID 列表。
     * 参数：self system 守护实例；username 目标用户名；manager_out 输出 logind manager；
     *       sessions_out 输出会话 ID 列表；error 错误输出。
     * 外部接口：org.freedesktop.login1.Manager/Session D-Bus 调用。
     */
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);
    g_return_val_if_fail(username != NULL && *username != '\0', FALSE);
    g_return_val_if_fail(sessions_out != NULL, FALSE);

    g_autoptr(GError) local_error = NULL;
    g_autoptr(DrdDBusLogindManager) manager = drd_dbus_logind_manager_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, DRD_LOGIND_BUS_NAME,
            DRD_LOGIND_MANAGER_OBJECT_PATH, NULL, &local_error);
    if (manager == NULL)
    {
        if (error != NULL)
        {
            g_propagate_error(error, g_steal_pointer(&local_error));
        }
        else
        {
            g_clear_error(&local_error);
        }
        return FALSE;
    }

    g_autoptr(GVariant) sessions = NULL;
    g_clear_error(&local_error);
    if (!drd_dbus_logind_manager_call_list_sessions_sync(manager, &sessions, NULL, &local_error))
    {
        if (error != NULL)
        {
            g_propagate_error(error, g_steal_pointer(&local_error));
        }
        else
        {
            g_clear_error(&local_error);
        }
        return FALSE;
    }
    GVariantIter iter;
    g_variant_iter_init(&iter, sessions);

    g_autoptr(GPtrArray) session_ids = g_ptr_array_new_with_free_func(g_free);
    const gchar *session_id = NULL;
    const gchar *user = NULL;
    const gchar *seat = NULL;
    const gchar *path = NULL;
    guint32 uid = 0;

    while (g_variant_iter_loop(&iter, "(susso)", &session_id, &uid, &user, &seat, &path))
    {
        (void) uid;
        (void) seat;

        if (g_strcmp0(user, username) != 0)
        {
            continue;
        }

        g_clear_error(&local_error);
        g_autoptr(DrdDBusLogindSession) session_proxy = drd_dbus_logind_session_proxy_new_for_bus_sync(
                G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, DRD_LOGIND_BUS_NAME, path, NULL, &local_error);
        if (session_proxy == NULL)
        {
            if (error != NULL)
            {
                g_propagate_error(error, g_steal_pointer(&local_error));
            }
            else
            {
                g_clear_error(&local_error);
            }
            return FALSE;
        }
        const gchar *type = drd_dbus_logind_session_get_type_(session_proxy);
        if (type == NULL)
        {
            if (error != NULL)
            {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Session %s type unavailable", session_id);
            }
            return FALSE;
        }
        DRD_LOG_MESSAGE("session handover_dbus_path is %s", session_id);
        gboolean remote = drd_dbus_logind_session_get_remote(session_proxy);
        if (remote || !drd_system_daemon_is_graphical_session_type(type))
        {
            continue;
        }

        g_ptr_array_add(session_ids, g_strdup(session_id));
    }

    if (manager_out != NULL)
    {
        *manager_out = g_object_ref(manager);
    }
    *sessions_out = g_steal_pointer(&session_ids);
    return TRUE;
}

static gboolean drd_system_daemon_terminate_local_graphical_sessions(DrdDBusLogindManager *manager,
                                                                     GPtrArray *session_ids, const gchar *username,
                                                                     GError **error)
{
    /*
     * 功能：终止指定会话 ID 列表中的本地图形会话。
     * 逻辑：遍历列表依次调用 logind 终止会话，失败时返回错误。
     * 参数：manager logind 管理器；session_ids 会话 ID 列表；username 目标用户名；error 错误输出。
     * 外部接口：org.freedesktop.login1.Manager/TerminateSession D-Bus 调用。
     */
    g_return_val_if_fail(manager != NULL, FALSE);
    g_return_val_if_fail(session_ids != NULL, FALSE);
    g_return_val_if_fail(username != NULL && *username != '\0', FALSE);

    for (guint i = 0; i < session_ids->len; i++)
    {
        const gchar *session_id = g_ptr_array_index(session_ids, i);
        g_autoptr(GError) local_error = NULL;
        if (!drd_dbus_logind_manager_call_terminate_session_sync(manager, session_id, NULL, &local_error))
        {
            if (error != NULL)
            {
                g_propagate_error(error, g_steal_pointer(&local_error));
            }
            else
            {
                g_clear_error(&local_error);
            }
            return FALSE;
        }
        DRD_LOG_MESSAGE("terminated local session %s for user %s", session_id, username);
    }

    return TRUE;
}

/*
 * 功能：从 handover 对象路径提取 routing token。
 * 逻辑：校验路径是否带 handover 前缀，缺失 token 或前缀不符时记录警告并返回 NULL，否则返回 token 字符串副本。
 * 参数：handover_dbus_path handover 对象路径。
 * 外部接口：GLib g_str_has_prefix/g_strdup；日志 DRD_LOG_WARNING。
 */
static gchar *get_routing_token_from_dbus_path(const gchar *id)
{
    const gchar *prefix = DRD_REMOTE_DESKTOP_HANDOVER_SESSION_PATH_PREFIX;
    gsize prefix_len;

    g_return_val_if_fail(id != NULL, NULL);

    prefix_len = strlen(prefix);
    if (!g_str_has_prefix(id, prefix))
    {
        DRD_LOG_WARNING("remote handover_dbus_path %s missing handover prefix %s", id, prefix);
        return NULL;
    }

    if (id[prefix_len] == '\0')
    {
        DRD_LOG_WARNING("remote handover_dbus_path %s missing routing token segment", id);
        return NULL;
    }

    return g_strdup(id + prefix_len);
}

/*
 * 功能：生成唯一的 handover 对象路径与 routing token。
 * 逻辑：循环随机产生非零 token，转换成 remote_id 并检查哈希表中是否已存在；成功后派生对应的 routing_token
 * 字符串并输出。 参数：self system 守护实例；remote_id_out 输出 handover 对象路径；routing_token_out 输出 token
 * 字符串。 外部接口：GLib g_random_int/g_hash_table_contains/g_strdup_printf。
 */
static gboolean drd_system_daemon_generate_remote_identity(DrdSystemDaemon *self, gchar **remote_id_out,
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
        remote_id = get_dbus_path_from_routing_token(routing_token_value);
        if (remote_id == NULL)
        {
            continue;
        }

        if (!g_hash_table_contains(self->remote_clients, remote_id)) // TODO
        {
            break;
        }
    }

    routing_token = get_routing_token_from_dbus_path(remote_id);
    if (routing_token == NULL)
    {
        return FALSE;
    }

    *remote_id_out = g_steal_pointer(&remote_id);
    *routing_token_out = routing_token;
    return TRUE;
}

static void drd_system_daemon_remove_client(DrdSystemDaemon *self, DrdRemoteClient *client);

static void drd_system_daemon_update_session_list(DrdSystemDaemon *self);

static gboolean drd_system_daemon_queue_client(DrdSystemDaemon *self, DrdRemoteClient *client);

static void drd_system_daemon_unqueue_client(DrdSystemDaemon *self, DrdRemoteClient *client);

static gboolean drd_system_daemon_register_client(DrdSystemDaemon *self, GSocketConnection *connection,
                                                  DrdRoutingTokenInfo *info);

static gboolean drd_system_daemon_delegate(DrdRdpListener *listener, GSocketConnection *connection, gpointer user_data,
                                           GError **error);

static gboolean drd_system_daemon_on_session_ready(DrdRdpListener *listener, DrdRdpSession *session,
                                                   gpointer user_data);

static DrdRemoteClient *drd_system_daemon_find_client_by_token(DrdSystemDaemon *self, const gchar *routing_token);

static gboolean drd_system_daemon_on_start_handover(DrdDBusRemoteDesktop1RemoteDesktop1HandoverSession *interface,
                                                    GDBusMethodInvocation *invocation, const gchar *one_time_auth_token,
                                                    gpointer user_data);

static gboolean drd_system_daemon_on_take_client(DrdDBusRemoteDesktop1RemoteDesktop1HandoverSession *interface,
                                                 GDBusMethodInvocation *invocation, GUnixFDList *fd_list,
                                                 gpointer user_data);

static gboolean
drd_system_daemon_on_get_system_credentials(DrdDBusRemoteDesktop1RemoteDesktop1HandoverSession *interface,
                                            GDBusMethodInvocation *invocation, gpointer user_data);

static void drd_system_daemon_on_bus_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data);

static void drd_system_daemon_on_bus_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data);

G_DEFINE_TYPE(DrdSystemDaemon, drd_system_daemon, G_TYPE_OBJECT)

/*
 * 功能：获取当前单调时钟值（微秒）。
 * 逻辑：直接封装 g_get_monotonic_time。
 * 参数：无。
 * 外部接口：GLib g_get_monotonic_time。
 */
static gint64 drd_system_daemon_now_us(void) { return g_get_monotonic_time(); }

/*
 * 功能：刷新远程客户端的最近活动时间。
 * 逻辑：客户端非空时写入当前微秒时间戳。
 * 参数：client 远程客户端。
 * 外部接口：内部 drd_system_daemon_now_us。
 */
static void drd_system_daemon_touch_client(DrdRemoteClient *client)
{
    if (client == NULL)
    {
        return;
    }

    client->last_activity_us = drd_system_daemon_now_us();
}

/*
 * 功能：移除长时间未被领取的待处理 handover 客户端。
 * 逻辑：遍历 pending 队列，若未分配且空闲时间超过 DRD_SYSTEM_CLIENT_STALE_TIMEOUT_US，则记录日志并调用 remove_client
 * 清理。 参数：self system 守护实例；now_us 当前时间戳（微秒）。 外部接口：日志 DRD_LOG_WARNING；内部
 * drd_system_daemon_remove_client。
 */
static void drd_system_daemon_prune_stale_pending_clients(DrdSystemDaemon *self, gint64 now_us)
{
    if (self->pending_clients == NULL || self->pending_clients->length == 0)
    {
        return;
    }

    // GList *link = self->pending_clients->head;
    // while (link != NULL)
    // {
    //     GList *next = link->next;
    //     DrdRemoteClient *candidate = link->data;
    //     if (candidate != NULL && !candidate->assigned && candidate->last_activity_us > 0)
    //     {
    //         gint64 idle_us = now_us - candidate->last_activity_us;
    //         if (idle_us >= DRD_SYSTEM_CLIENT_STALE_TIMEOUT_US)
    //         {
    //             DRD_LOG_WARNING("Expiring stale handover %s after %.0f seconds in queue",
    //                             candidate->handover_dbus_path != NULL ? candidate->handover_dbus_path : "unknown",
    //                             idle_us / (gdouble) G_USEC_PER_SEC);
    //             drd_system_daemon_remove_client(self, candidate);
    //         }
    //     }
    //     link = next;
    // }
}

/*
 * 功能：销毁远程客户端结构体并清理关联资源。
 * 逻辑：释放 DBus 骨架/接口，清空连接上存储的数据并释放连接/会话，释放 routing 信息和标识字符串，最后释放结构体内存。
 * 参数：client 远程客户端。
 * 外部接口：GLib g_clear_object/g_clear_pointer/g_object_set_data；内部 drd_routing_token_info_free。
 */
static void drd_remote_client_free(DrdRemoteClient *client)
{
    DRD_LOG_MESSAGE("free client");
    if (client == NULL)
    {
        return;
    }

    if (client->handover_iface != NULL)
    {
        g_signal_handlers_disconnect_by_data(client->handover_iface, client);
    }

    g_clear_object(&client->object_skeleton);
    g_clear_object(&client->handover_iface);

    if (client->lightdm_session_proxy != NULL)
    {
        g_signal_handlers_disconnect_by_data(client->lightdm_session_proxy, client);
        g_clear_object(&client->lightdm_session_proxy);
    }
    g_clear_pointer(&client->lightdm_session_path, g_free);
    if (client->connection != NULL)
    {
        g_object_set_data(G_OBJECT(client->connection), "drd-system-client", NULL);
        g_object_set_data(G_OBJECT(client->connection), "drd-system-keep-open", NULL);
        g_clear_object(&client->connection);
    }
    g_clear_object(&client->session);
    drd_routing_token_info_free(client->routing);
    g_clear_pointer(&client->handover_dbus_path, g_free);
    g_free(client);
}

static void drd_system_daemon_on_lightdm_session_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                                                    const gchar *const *invalidated_properties,
                                                                    gpointer user_data)
{
    (void) proxy;
    (void) invalidated_properties;

    DrdRemoteClient *watch_client = user_data;
    if (watch_client == NULL || watch_client->daemon == NULL)
    {
        return;
    }

    DrdSystemDaemon *self = watch_client->daemon;
    if (!DRD_IS_SYSTEM_DAEMON(self) || changed_properties == NULL)
    {
        return;
    }

    g_autofree gchar *new_client_id = NULL;
    if (!g_variant_lookup(changed_properties, "client_id", "s", &new_client_id))
    {
        return;
    }

    DRD_LOG_MESSAGE("LightDM session %s client_id changed %s -> %s",
                    watch_client->lightdm_session_path != NULL ? watch_client->lightdm_session_path : "unknown",
                    watch_client->routing->routing_token, new_client_id);

    if (watch_client->handover_iface == NULL)
    {
        return;
    }

    if (new_client_id == NULL || watch_client->routing == NULL || watch_client->routing->routing_token == NULL)
    {
        return;
    }
    if (g_strcmp0(watch_client->routing->routing_token, new_client_id) == 0)
    {
        DRD_LOG_MESSAGE("routing token == new client id");
        return;
    }
    // 如果发生了display session的client_id属性改变信号，那么证明该有新的client接入了display session。
    // 此时需要把新的client相关信息同步到已经被handover进程监控的旧client对象中。并释放新的client。
    // update old client info
    DrdRemoteClient *new_client = drd_system_daemon_find_client_by_token(watch_client->daemon, new_client_id);
    if (!new_client)
    {
        DRD_LOG_ERROR("not found client by %s", new_client_id);
        return;
    }
    if (watch_client->routing != NULL)
    {
        g_clear_pointer(&watch_client->routing->routing_token, g_free);
        watch_client->routing->routing_token = g_strdup(new_client->routing->routing_token);
        watch_client->routing->requested_rdstls = new_client->routing->requested_rdstls;
    }

    if (watch_client->connection != NULL)
    {
        g_object_set_data(G_OBJECT(watch_client->connection), "drd-system-client", NULL);
        g_object_set_data(G_OBJECT(watch_client->connection), "drd-system-keep-open", NULL);
    }
    g_clear_object(&watch_client->connection);
    watch_client->connection = g_steal_pointer(&new_client->connection);
    if (watch_client->connection != NULL)
    {
        g_object_set_data(G_OBJECT(watch_client->connection), "drd-system-client", watch_client);
        g_object_set_data(G_OBJECT(watch_client->connection), "drd-system-keep-open", GINT_TO_POINTER(1));
    }
    g_clear_object(&watch_client->session);
    watch_client->session = g_steal_pointer(&new_client->session);
    if (watch_client->session != NULL)
    {
        drd_rdp_session_set_system_client(watch_client->session, watch_client);
    }
    watch_client->client_width = new_client->client_width;
    watch_client->client_height = new_client->client_height;
    drd_system_daemon_touch_client(watch_client);
    drd_dbus_remote_desktop1_remote_desktop1_handover_session_emit_restart_handover(watch_client->handover_iface);
    // free new client

    if (new_client->lightdm_session_proxy != NULL)
    {
        g_signal_handlers_disconnect_by_data(new_client->lightdm_session_proxy, new_client);
    }
    drd_system_daemon_remove_client(watch_client->daemon, new_client);
}

static gboolean drd_system_daemon_watch_display_session(DrdRemoteClient *client, const gchar *session_path)
{
    g_return_val_if_fail(client != NULL, FALSE);
    g_return_val_if_fail(client->daemon != NULL, FALSE);
    g_return_val_if_fail(session_path != NULL && *session_path != '\0', FALSE);
    DRD_LOG_MESSAGE("session path is %s", session_path);
    // 判断是否有client持有了该session_path
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, client->daemon->remote_clients);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        DrdRemoteClient *candidate = value;
        DRD_LOG_MESSAGE("candidate session path is %s", candidate->lightdm_session_path);
        if (candidate != NULL && g_strcmp0(session_path, candidate->lightdm_session_path) == 0)
        {
            DRD_LOG_MESSAGE("find session path handover");
            return TRUE;
        }
    }
    g_autoptr(GError) error = NULL;
    client->lightdm_session_proxy = drd_dbus_lightdm_remote_display_factory_session_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, DRD_LIGHTDM_REMOTE_FACTORY_BUS_NAME, session_path,
            NULL, &error);
    if (client->lightdm_session_proxy == NULL)
    {
        DRD_LOG_WARNING("Failed to create LightDM session proxy for %s: %s", session_path,
                        error != NULL ? error->message : "unknown");
        return FALSE;
    }
    client->lightdm_session_path = g_strdup(session_path);
    g_signal_connect(client->lightdm_session_proxy, "g-properties-changed",
                     G_CALLBACK(drd_system_daemon_on_lightdm_session_properties_changed), client);
    return TRUE;
}

/*
 * 功能：通过 routing token 定位远程客户端。
 * 逻辑：将字符串 token 转为整数生成 remote_id，再在哈希表中查找匹配的客户端，非法 token 会记录警告。
 * 参数：self system 守护实例；routing_token 路由 token 字符串。
 * 外部接口：GLib g_ascii_string_to_unsigned/g_hash_table_lookup；日志 DRD_LOG_WARNING。
 */
static DrdRemoteClient *drd_system_daemon_find_client_by_token(DrdSystemDaemon *self, const gchar *routing_token)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), NULL);
    g_return_val_if_fail(routing_token != NULL, NULL);

    guint64 parsed_token = 0;
    gboolean success = FALSE;
    success = g_ascii_string_to_unsigned(routing_token, 10, 1, G_MAXUINT32, &parsed_token, NULL);
    if (!success)
    {
        DRD_LOG_WARNING("Invalid routing token string %s", routing_token);
        return NULL;
    }

    (void) parsed_token;
    if (self->remote_clients == NULL)
    {
        return NULL;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->remote_clients);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        DrdRemoteClient *candidate = value;
        if (candidate != NULL && candidate->routing != NULL && candidate->routing->routing_token != NULL &&
            g_strcmp0(candidate->routing->routing_token, routing_token) == 0)
        {
            return candidate;
        }
    }

    return NULL;
}

/*
 * 功能：将客户端放入等待队列。
 * 逻辑：若已分配则直接返回；先清理超时队列，再检查队列上限，合格则记录活跃时间并入队。
 * 参数：self system 守护实例；client 远程客户端。
 * 外部接口：内部 drd_system_daemon_prune_stale_pending_clients；GLib g_queue_push_tail。
 */
static gboolean drd_system_daemon_queue_client(DrdSystemDaemon *self, DrdRemoteClient *client)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);
    g_return_val_if_fail(client != NULL, FALSE);

    if (client->assigned)
    {
        return TRUE;
    }

    gint64 now_us = drd_system_daemon_now_us();
    drd_system_daemon_prune_stale_pending_clients(self, now_us);

    guint pending = drd_system_daemon_get_pending_client_count(self);
    if (pending >= DRD_SYSTEM_MAX_PENDING_CLIENTS)
    {
        DRD_LOG_WARNING("Pending handover queue full (%u >= %u), cannot enqueue %s", pending,
                        DRD_SYSTEM_MAX_PENDING_CLIENTS,
                        client->routing->routing_token != NULL ? client->routing->routing_token : "unknown");
        return FALSE;
    }

    client->last_activity_us = now_us;
    g_queue_push_tail(self->pending_clients, client);
    return TRUE;
}

/*
 * 功能：从等待队列中移除指定客户端。
 * 逻辑：在队列中查找对应节点并删除。
 * 参数：self system 守护实例；client 远程客户端。
 * 外部接口：GLib g_queue_find/g_queue_delete_link。
 */
static void drd_system_daemon_unqueue_client(DrdSystemDaemon *self, DrdRemoteClient *client)
{
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));
    g_return_if_fail(client != NULL);

    GList *link = g_queue_find(self->pending_clients, client);
    if (link != NULL)
    {
        g_queue_delete_link(self->pending_clients, link);
    }
}

/*
 * 功能：完全移除并注销一个远程客户端。
 * 逻辑：先从等待队列移除；若 handover manager 存在则取消导出对象；清理连接上存储的标记；释放会话并从哈希表删除。
 * 参数：self system 守护实例；client 远程客户端。
 * 外部接口：GDBus g_dbus_object_manager_server_unexport；GLib g_object_set_data/g_hash_table_remove。
 */
static void drd_system_daemon_remove_client(DrdSystemDaemon *self, DrdRemoteClient *client)
{
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));
    g_return_if_fail(client != NULL);

    DRD_LOG_MESSAGE("remove client");
    drd_system_daemon_unqueue_client(self, client);

    if (client->handover_iface != NULL)
    {
        g_signal_handlers_disconnect_by_data(client->handover_iface, client);
    }

    if (self->bus.object_manager != NULL && client->handover_dbus_path != NULL)
    {
        g_dbus_object_manager_server_unexport(self->bus.object_manager, client->handover_dbus_path);
    }

    if (client->connection != NULL)
    {
        g_object_set_data(G_OBJECT(client->connection), "drd-system-client", NULL);
        g_object_set_data(G_OBJECT(client->connection), "drd-system-keep-open", NULL);
    }
    g_clear_object(&client->session);

    g_hash_table_remove(self->remote_clients, client->handover_dbus_path);
    drd_system_daemon_update_session_list(self);
}

static void drd_system_daemon_update_session_list(DrdSystemDaemon *self)
{
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));

    if (self->bus.remote_login_iface == NULL)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    g_autoptr(GPtrArray) list = g_ptr_array_new_with_free_func(g_free);

    g_hash_table_iter_init(&iter, self->remote_clients);
    while (g_hash_table_iter_next(&iter, &key, NULL))
    {
        if (key != NULL)
        {
            g_ptr_array_add(list, g_strdup((const gchar *) key));
        }
    }
    g_ptr_array_add(list, NULL);

    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_session_list(self->bus.remote_login_iface,
                                                                           (const gchar *const *) list->pdata);
}

/*
 * 功能：将新连接注册为 handover 客户端并导出 DBus 接口。
 * 逻辑：生成或复用 routing token/remote_id，构建 handover skeleton 并导出到 handover
 * manager；在连接上写入元数据；入队等待。 参数：self system 守护实例；connection 新连接；info peek 到的 routing token
 * 信息。 外部接口：GLib g_ascii_string_to_unsigned/g_hash_table_contains/g_object_set_data；GDBus
 * drd_dbus_remote_desktop_rdp_handover_skeleton_new/g_dbus_object_skeleton_add_interface/export。
 */
static gboolean drd_system_daemon_register_client(DrdSystemDaemon *self, GSocketConnection *connection,
                                                  DrdRoutingTokenInfo *info)
{
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), FALSE);
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);
    g_return_val_if_fail(info != NULL, FALSE);

    DrdRemoteClient *client = g_new0(DrdRemoteClient, 1);
    g_autofree gchar *handover_dbus_path = NULL;
    g_autofree gchar *routing_token = NULL;
    guint64 parsed_token_value = 0;

    client->daemon = self;
    client->connection = g_object_ref(connection);
    client->routing = drd_routing_token_info_new();
    client->routing->requested_rdstls = info->requested_rdstls;

    if (info->routing_token != NULL)
    {
        if (g_ascii_string_to_unsigned(info->routing_token, 10, 1, G_MAXUINT32, &parsed_token_value, NULL))
        {
            handover_dbus_path = get_dbus_path_from_routing_token((guint32) parsed_token_value);
            routing_token = g_strdup(info->routing_token);
            if (handover_dbus_path != NULL && g_hash_table_contains(self->remote_clients, handover_dbus_path))
            {
                DRD_LOG_WARNING("Routing token %s already tracked, generating a new one", info->routing_token);
                g_clear_pointer(&handover_dbus_path, g_free);
                g_clear_pointer(&routing_token, g_free);
            }
        }
        else
        {
            DRD_LOG_WARNING("Ignoring invalid routing token %s from peek", info->routing_token);
        }
    }

    if (handover_dbus_path == NULL || routing_token == NULL)
    {
        if (!drd_system_daemon_generate_remote_identity(self, &handover_dbus_path, &routing_token))
        {
            DRD_LOG_WARNING("Unable to allocate remote identity for new handover client");
            drd_routing_token_info_free(client->routing);
            g_clear_object(&client->connection);
            g_free(client);
            return FALSE;
        }
    }

    client->handover_dbus_path = g_steal_pointer(&handover_dbus_path);
    client->routing->routing_token = g_steal_pointer(&routing_token);
    client->use_system_credentials = FALSE;
    client->handover_count = 0;
    client->handover_iface = drd_dbus_remote_desktop1_remote_desktop1_handover_session_skeleton_new();
    g_autofree gchar *peer_ip = drd_system_daemon_dup_peer_ip(connection);
    drd_dbus_remote_desktop1_remote_desktop1_handover_session_set_ip(client->handover_iface,
                                                                     peer_ip != NULL ? peer_ip : "");
    g_signal_connect(client->handover_iface, "handle-start-handover", G_CALLBACK(drd_system_daemon_on_start_handover),
                     client);
    g_signal_connect(client->handover_iface, "handle-take-client", G_CALLBACK(drd_system_daemon_on_take_client),
                     client);
    g_signal_connect(client->handover_iface, "handle-get-system-credentials",
                     G_CALLBACK(drd_system_daemon_on_get_system_credentials), client);

    client->object_skeleton = g_dbus_object_skeleton_new(client->handover_dbus_path);
    g_dbus_object_skeleton_add_interface(client->object_skeleton, G_DBUS_INTERFACE_SKELETON(client->handover_iface));

    if (self->bus.object_manager != NULL)
    {
        g_dbus_object_manager_server_export(self->bus.object_manager, client->object_skeleton);
    }

    g_object_set_data(G_OBJECT(connection), "drd-system-client", client);
    g_object_set_data(G_OBJECT(connection), "drd-system-keep-open", GINT_TO_POINTER(1));

    g_hash_table_replace(self->remote_clients, g_strdup(client->handover_dbus_path), client);
    drd_system_daemon_update_session_list(self);
    if (!drd_system_daemon_queue_client(self, client))
    {
        DRD_LOG_WARNING("Rejecting handover client %s because pending queue is full", client->handover_dbus_path);
        drd_system_daemon_remove_client(self, client);
        return FALSE;
    }

    const gchar *token_preview = client->routing != NULL && client->routing->routing_token != NULL
                                         ? client->routing->routing_token
                                         : "unknown";
    DRD_LOG_MESSAGE("Registered handover client %s (token=%s)", client->handover_dbus_path, token_preview);

    return TRUE;
}

// return FALSE 时，需要继续处理这个connection;return TRUE时，代表已经处理过，需要让handover进程来处理；
/*
 * 功能：监听器委派回调，用于在 system 模式下注册/续接 handover 客户端。
 * 逻辑：peek 路由 token；若 token 已存在且未绑定 session，则更新连接并触发
 * TakeClientReady；否则注册为新客户端并决定是否继续交由默认监听器处理。 参数：listener RDP 监听器；connection
 * 新连接；user_data system 守护实例；error 错误输出。 外部接口：drd_routing_token_peek 读取 token；GIO
 * g_socket_connection_factory_create_connection/g_object_set_data；drd_system_daemon_register_client；GDBus 信号
 * drd_dbus_remote_desktop_rdp_handover_emit_take_client_ready。
 */
static gboolean drd_system_daemon_delegate(DrdRdpListener *listener, GSocketConnection *connection, gpointer user_data,
                                           GError **error)
{
    (void) listener;
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
        // 发redirect PDU后重连
        DrdRemoteClient *existing = drd_system_daemon_find_client_by_token(self, info->routing_token);
        if (existing != NULL && existing->session == NULL)
        {
            g_clear_object(&existing->connection);
            existing->connection = g_object_ref(connection);
            g_object_set_data(G_OBJECT(connection), "drd-system-client", existing);
            g_object_set_data(G_OBJECT(connection), "drd-system-keep-open", GINT_TO_POINTER(1));
            drd_system_daemon_touch_client(existing);
            DRD_LOG_MESSAGE("drd_system_daemon emit take client ready");
            drd_dbus_remote_desktop1_remote_desktop1_handover_session_emit_take_client_ready(
                    existing->handover_iface, existing->use_system_credentials);
            g_object_unref(connection);
            return TRUE;
        }
    }

    if (!drd_system_daemon_register_client(self, connection, info))
    {
        g_clear_pointer(&info, drd_routing_token_info_free);
        g_object_unref(connection);
        return TRUE;
    }
    DRD_LOG_MESSAGE("Registered new handover client (total=%u, pending=%u)",
                    drd_system_daemon_get_remote_client_count(self), drd_system_daemon_get_pending_client_count(self));

    /* Allow the default listener to accept the connection so FreeRDP can build a session and send redirection. */
    g_object_unref(connection);
    return FALSE;
}

/*
 * 功能：监听器回调，记录连接对应的会话对象。
 * 逻辑：从会话 metadata 获取客户端结构；替换 session 引用并根据客户端请求的能力决定是否使用系统凭据；创建 LightDM
 * 远程显示；刷新活跃时间。 参数：listener 监听器；session 新会话；user_data system 守护实例。 外部接口：GLib
 * g_clear_object/g_object_ref；drd_rdp_session_client_is_mstsc； LightDM proxy
 * drd_dbus_lightdm_remote_display_factory_proxy_new_for_bus_sync 与
 *           drd_dbus_lightdm_remote_display_factory_call_create_remote_greeter_display_sync。
 */
static gboolean drd_system_daemon_on_session_ready(DrdRdpListener *listener, DrdRdpSession *session, gpointer user_data)
{
    DRD_LOG_MESSAGE("on session ready");
    (void) listener;
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    if (!DRD_IS_SYSTEM_DAEMON(self))
    {
        return FALSE;
    }

    DrdRemoteClient *client = drd_rdp_session_get_system_client(session);
    // new client
    if (client == NULL)
    {
        return FALSE;
    }

    if (client->session != NULL)
    {
        g_clear_object(&client->session);
    }
    client->session = g_object_ref(session);
    if (client->routing != NULL)
    {
        client->use_system_credentials = drd_rdp_session_client_is_mstsc(session) && !client->routing->requested_rdstls;
    }

    if (!self->remote_display_factory)
    {
        self->remote_display_factory = drd_dbus_lightdm_remote_display_factory_proxy_new_for_bus_sync(
                G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, DRD_LIGHTDM_REMOTE_FACTORY_BUS_NAME,
                DRD_LIGHTDM_REMOTE_FACTORY_OBJECT_PATH, NULL, NULL);
    }
    g_autofree gchar *session_path = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *peer_address = drd_rdp_session_get_peer_address(session);
    guint32 client_width = 0;
    guint32 client_height = 0;
    const DrdEncodingOptions *encoding_opts = drd_config_get_encoding_options(self->config);
    if (encoding_opts == NULL)
    {
        DRD_LOG_WARNING("Encoding options unavailable for system session resolution");
        return FALSE;
    }

    guint32 target_width = encoding_opts->width;
    guint32 target_height = encoding_opts->height;
    if (drd_rdp_session_get_peer_resolution(session, &client_width, &client_height) && client_width > 0 &&
        client_height > 0)
    {
        client->client_width = client_width;
        client->client_height = client_height;
        target_width = client_width;
        target_height = client_height;
    }

    DrdEncodingOptions runtime_opts = *encoding_opts;
    runtime_opts.width = target_width;
    runtime_opts.height = target_height;
    drd_server_runtime_set_encoding_options(self->runtime, &runtime_opts);
    if (drd_rdp_listener_is_single_login(self->listener))
    {
        g_autoptr(GVariant) fd_variant = NULL;
        g_autoptr(GUnixFDList) fd_list = NULL;
        g_autoptr(GUnixFDList) out_fd_list = NULL;
        g_autoptr(GError) fd_error = NULL;
        DrdPamAuth *pam_auth = drd_rdp_session_get_pam_auth(session);
        const gchar *auth_username = drd_pam_auth_get_username(pam_auth);
        const gchar *auth_password = drd_pam_auth_get_password(pam_auth);
        g_autofree gchar *auth_payload = NULL;
        gsize payload_len = 0;
        g_autofree gchar *shm_name = g_strdup_printf("/drd-auth-%d-%u", (int) getpid(), g_random_int());
        int auth_fd = -1;
        gboolean single_login_ok = FALSE;

        if (pam_auth == NULL)
        {
            DRD_LOG_WARNING("single logon auth payload missing PAM auth");
            drd_system_daemon_touch_client(client);
            return FALSE;
        }
        if (auth_username == NULL || *auth_username == '\0' || auth_password == NULL || *auth_password == '\0')
        {
            DRD_LOG_WARNING("single logon auth payload missing username/password");
            drd_system_daemon_touch_client(client);
            return FALSE;
        }

        g_autoptr(DrdDBusLogindManager) logind_manager = NULL;
        g_autoptr(GPtrArray) local_sessions = NULL;
        g_autoptr(GError) local_session_error = NULL;
        if (!drd_system_daemon_collect_local_graphical_sessions(self, auth_username, &logind_manager, &local_sessions,
                                                                &local_session_error))
        {
            DRD_LOG_WARNING("collect local sessions failed for user %s: %s", auth_username,
                            local_session_error != NULL ? local_session_error->message : "unknown");
            drd_system_daemon_touch_client(client);
            return FALSE;
        }

        // if (local_sessions->len > 0 && !drd_config_should_logout_local_session_on_single_login(self->config))
        // {
        //     DRD_LOG_WARNING("local graphical session exists for user %s, single login rejected", auth_username);
        //     drd_system_daemon_touch_client(client);
        //     return FALSE;
        // }
        //
        // if (local_sessions->len > 0)
        // {
        //     DRD_LOG_MESSAGE("need terminate local graphics session");
        //     g_clear_error(&local_session_error);
        //     if (!drd_system_daemon_terminate_local_graphical_sessions(logind_manager, local_sessions, auth_username,
        //                                                               &local_session_error))
        //     {
        //         DRD_LOG_WARNING("terminate local session failed for user %s: %s", auth_username,
        //                         local_session_error != NULL ? local_session_error->message : "unknown");
        //         drd_system_daemon_touch_client(client);
        //         return FALSE;
        //     }
        // }
        // else
        // {
        //     DRD_LOG_MESSAGE("no local graphical session for user %s", auth_username);
        // }

        auth_payload = g_strdup_printf("%s\n%s", auth_username, auth_password);
        if (auth_payload == NULL)
        {
            DRD_LOG_WARNING("create single logon auth payload failed");
            goto single_login_cleanup;
        }
        payload_len = strlen(auth_payload) + 1;
        auth_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (auth_fd < 0)
        {
            DRD_LOG_WARNING("create auth shm failed: %s", g_strerror(errno));
            goto single_login_cleanup;
        }
        if (shm_unlink(shm_name) != 0)
        {
            DRD_LOG_WARNING("unlink auth shm failed: %s", g_strerror(errno));
        }
        if (ftruncate(auth_fd, (off_t) payload_len) != 0 ||
            write(auth_fd, auth_payload, payload_len) != (ssize_t) payload_len || lseek(auth_fd, 0, SEEK_SET) < 0)
        {
            DRD_LOG_WARNING("prepare auth shm failed: %s", g_strerror(errno));
            goto single_login_cleanup;
        }
        fd_list = g_unix_fd_list_new();
        gint fd_index = g_unix_fd_list_append(fd_list, auth_fd, &fd_error);
        if (auth_fd >= 0)
        {
            close(auth_fd);
            auth_fd = -1;
        }
        if (fd_index < 0)
        {
            DRD_LOG_WARNING("append auth fd failed: %s", fd_error != NULL ? fd_error->message : "unknown");
            goto single_login_cleanup;
        }
        fd_variant = g_variant_new_handle(fd_index);
        if (!drd_dbus_lightdm_remote_display_factory_call_create_single_logon_session_sync(
                    self->remote_display_factory, client->routing->routing_token, target_width, target_height,
                    fd_variant, peer_address, fd_list, &session_path, &out_fd_list, NULL, &error))
        {
            DRD_LOG_WARNING("create single logon session failed %s", error != NULL ? error->message : "unknown");
            goto single_login_cleanup;
        }
        single_login_ok = TRUE;

    single_login_cleanup:
        if (auth_payload != NULL && payload_len > 0)
        {
            memset(auth_payload, 0, payload_len);
        }
        if (pam_auth != NULL)
        {
            drd_pam_auth_clear_password(pam_auth);
        }
        if (auth_fd >= 0)
        {
            close(auth_fd);
        }
        if (!single_login_ok)
        {
            drd_system_daemon_touch_client(client);
            return FALSE;
        }
        if (session_path && strlen(session_path) > 0)
        {
            DRD_LOG_MESSAGE("session_path=%s", session_path);
        }
        drd_system_daemon_watch_display_session(client, session_path);
    }
    else
    {
        if (!drd_dbus_lightdm_remote_display_factory_call_create_remote_greeter_display_sync(
                    self->remote_display_factory, client->routing->routing_token, target_width, target_height,
                    peer_address, &session_path, NULL, &error))
        {
            DRD_LOG_WARNING("create remote display failed %s", error != NULL ? error->message : "unknown");
            drd_system_daemon_touch_client(client);
            return FALSE;
        }
    }


    drd_system_daemon_touch_client(client);
    return TRUE;
}

static gboolean drd_system_daemon_load_tls_material(DrdSystemDaemon *self, gchar **certificate, gchar **key,
                                                    GError **error)
{
    /*
     * 功能：读取缓存的 TLS 证书与私钥文本。
     * 逻辑：确保凭据存在后调用 drd_tls_credentials_read_material 复制 PEM；缺失时返回错误。
     * 参数：self system 守护实例；certificate/key 输出 PEM；error 错误输出。
     * 外部接口：drd_tls_credentials_read_material；GLib g_set_error_literal。
     */
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

static gboolean drd_system_daemon_handle_request_handover(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                          GDBusMethodInvocation *invocation, gpointer user_data)
{
    /*
     * 功能：处理 dispatcher 的 RequestHandover 调用。
     * 逻辑：清理过期客户端后从 pending 队列取出一个等待对象；若为空返回 NOT_FOUND 错误；否则标记
     * assigned、刷新活跃时间并返回 handover 对象路径。 参数：interface dispatcher 接口；invocation DBus
     * 调用上下文；user_data system 守护实例。 外部接口：GDBus
     * drd_dbus_remote_desktop_rdp_dispatcher_complete_request_handover/g_dbus_method_invocation_return_error；日志
     * DRD_LOG_MESSAGE。
     */
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    drd_system_daemon_prune_stale_pending_clients(self, drd_system_daemon_now_us());
    DrdRemoteClient *client = g_queue_pop_head(self->pending_clients);
    if (client == NULL)
    {
        g_dbus_method_invocation_return_error(invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                              "No pending RDP handover requests");
        DRD_LOG_MESSAGE("request handover error");
        return TRUE;
    }

    client->assigned = TRUE;
    drd_system_daemon_touch_client(client);
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_complete_request_handover(interface, invocation,
                                                                                    client->handover_dbus_path);
    DRD_LOG_MESSAGE("Dispatching handover client %s", client->handover_dbus_path);
    return TRUE;
}

static gboolean drd_system_daemon_method_not_supported(GDBusMethodInvocation *invocation, const gchar *method_name)
{
    g_return_val_if_fail(invocation != NULL, TRUE);

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "%s not implemented",
                                          method_name != NULL ? method_name : "Method");
    return TRUE;
}

static gboolean drd_system_daemon_handle_request_port(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                      GDBusMethodInvocation *invocation, gpointer user_data)
{
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), TRUE);

    const gint port = (gint) drd_config_get_port(self->config);
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_complete_request_port(interface, invocation, port);
    return TRUE;
}

static gboolean drd_system_daemon_handle_enable_remote_login(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                             GDBusMethodInvocation *invocation, gboolean enable,
                                                             gpointer user_data)
{
    (void) interface;
    (void) enable;
    (void) user_data;
    return drd_system_daemon_method_not_supported(invocation, "EnableRemoteLogin");
}

static gboolean drd_system_daemon_handle_get_credentials(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                         GDBusMethodInvocation *invocation, GUnixFDList *fd_list,
                                                         gpointer user_data)
{
    (void) interface;
    (void) fd_list;
    (void) user_data;
    return drd_system_daemon_method_not_supported(invocation, "GetCredentials");
}

static gboolean drd_system_daemon_handle_set_credentials(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                         GDBusMethodInvocation *invocation, GUnixFDList *fd_list,
                                                         GVariant *credentials_fd, gpointer user_data)
{
    (void) interface;
    (void) fd_list;
    (void) credentials_fd;
    (void) user_data;
    return drd_system_daemon_method_not_supported(invocation, "SetCredentials");
}

static gboolean drd_system_daemon_handle_enable_nla_auth(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                         GDBusMethodInvocation *invocation, gboolean enable,
                                                         gpointer user_data)
{
    (void) interface;
    (void) enable;
    (void) user_data;
    return drd_system_daemon_method_not_supported(invocation, "EnableNlaAuth");
}

static gboolean
drd_system_daemon_handle_enable_auto_logout_rdp_disconnect(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                           GDBusMethodInvocation *invocation, gboolean enable,
                                                           gpointer user_data)
{
    (void) interface;
    (void) enable;
    (void) user_data;
    return drd_system_daemon_method_not_supported(invocation, "EnableAutoLogoutRdpDisconnect");
}

static gboolean drd_system_daemon_handle_gen_nla_credential(DrdDBusRemoteDesktop1RemoteDesktop1RemoteLogin *interface,
                                                            GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void) interface;
    (void) user_data;
    return drd_system_daemon_method_not_supported(invocation, "GenNlaCredential");
}

static void drd_system_daemon_reset_bus_context(DrdSystemDaemon *self)
{
    /*
     * 功能：撤销 DBus 相关导出与总线占用。
     * 逻辑：取消 handover manager 连接，unexport dispatcher，释放总线名称并清理连接引用。
     * 参数：self system 守护实例。
     * 外部接口：GDBus g_dbus_object_manager_server_set_connection/unexport、g_bus_unown_name。
     */
    if (self->bus.object_manager != NULL)
    {
        g_dbus_object_manager_server_set_connection(self->bus.object_manager, NULL);
        g_clear_object(&self->bus.object_manager);
    }

    g_clear_object(&self->bus.common_iface);
    g_clear_object(&self->bus.remote_login_iface);
    g_clear_object(&self->bus.root_object);

    if (self->bus.bus_name_owner_id != 0)
    {
        g_bus_unown_name(self->bus.bus_name_owner_id);
        self->bus.bus_name_owner_id = 0;
    }

    g_clear_object(&self->bus.connection);
}

static void drd_system_daemon_stop_listener(DrdSystemDaemon *self)
{
    /*
     * 功能：停止并释放系统模式监听器。
     * 逻辑：若监听器存在则调用 stop 并释放引用。
     * 参数：self system 守护实例。
     * 外部接口：drd_rdp_listener_stop；GLib g_clear_object。
     */
    if (self->listener != NULL)
    {
        drd_rdp_listener_stop(self->listener);
        g_clear_object(&self->listener);
    }
}

static void drd_system_daemon_on_bus_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    /*
     * 功能：总线名称获取回调。
     * 逻辑：记录成功占用 bus name 的日志。
     * 参数：connection DBus 连接；name 名称；user_data system 守护实例。
     * 外部接口：日志 DRD_LOG_MESSAGE。
     */
    (void) connection;
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    if (self == NULL)
    {
        return;
    }
    DRD_LOG_MESSAGE("System daemon acquired bus name %s", name);
}

static void drd_system_daemon_on_bus_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    /*
     * 功能：总线名称丢失回调。
     * 逻辑：记录警告并请求主循环退出，交由 systemd 重启。
     * 参数：connection DBus 连接；name 名称；user_data system 守护实例。
     * 外部接口：日志 DRD_LOG_WARNING；内部 drd_system_daemon_request_shutdown。
     */
    (void) connection;
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    if (self == NULL)
    {
        return;
    }
    DRD_LOG_WARNING("System daemon lost bus name %s, requesting shutdown", name);
    /*
     * 丢失总线名称通常意味着总线重启或权限问题，直接触发主循环退出
     * 让 systemd 重新拉起服务，确保状态一致。
     */
    drd_system_daemon_request_shutdown(self);
}

static void drd_system_daemon_request_shutdown(DrdSystemDaemon *self)
{
    /*
     * 功能：请求退出主循环。
     * 逻辑：若主循环正在运行则记录日志并退出。
     * 参数：self system 守护实例。
     * 外部接口：GLib g_main_loop_is_running/g_main_loop_quit；日志 DRD_LOG_MESSAGE。
     */
    if (self->main_loop != NULL && g_main_loop_is_running(self->main_loop))
    {
        DRD_LOG_MESSAGE("System daemon shutting down main loop");
        g_main_loop_quit(self->main_loop);
    }
}

void drd_system_daemon_stop(DrdSystemDaemon *self)
{
    /*
     * 功能：停止 system 守护的对外服务与监听。
     * 逻辑：重置 DBus 上下文、停止监听器、清空客户端哈希表与队列，并请求主循环退出。
     * 参数：self system 守护实例。
     * 外部接口：内部
     * drd_system_daemon_reset_bus_context/drd_system_daemon_stop_listener/drd_system_daemon_request_shutdown；GLib
     * g_hash_table_remove_all/g_queue_clear。
     */
    g_return_if_fail(DRD_IS_SYSTEM_DAEMON(self));

    drd_system_daemon_reset_bus_context(self);
    drd_system_daemon_stop_listener(self);
    g_clear_object(&self->remote_display_factory);
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

static void drd_system_daemon_dispose(GObject *object)
{
    /*
     * 功能：释放 system 守护持有的资源。
     * 逻辑：调用 stop 清理运行态，再释放 TLS/运行时/配置与主循环引用，销毁队列和哈希表，最后交由父类 dispose。
     * 参数：object 基类指针，期望为 DrdSystemDaemon。
     * 外部接口：GLib g_clear_object/g_clear_pointer/g_queue_free/g_hash_table_destroy；内部 drd_system_daemon_stop。
     */
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

static void drd_system_daemon_class_init(DrdSystemDaemonClass *klass)
{
    /*
     * 功能：绑定类级别析构回调。
     * 逻辑：将自定义 dispose 挂载到 GObjectClass。
     * 参数：klass 类结构。
     * 外部接口：GLib 类型系统。
     */
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_system_daemon_dispose;
}

static void drd_system_daemon_init(DrdSystemDaemon *self)
{
    /*
     * 功能：初始化 system 守护实例字段。
     * 逻辑：清空总线上下文、创建客户端哈希表与队列，主循环置空。
     * 参数：self system 守护实例。
     * 外部接口：GLib g_hash_table_new_full/g_queue_new。
     */
    self->bus.common_iface = NULL;
    self->bus.remote_login_iface = NULL;
    self->bus.object_manager = NULL;
    self->bus.root_object = NULL;
    self->bus.bus_name_owner_id = 0;
    self->bus.connection = NULL;
    self->remote_clients =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) drd_remote_client_free);
    self->pending_clients = g_queue_new();
    self->main_loop = NULL;
}

DrdSystemDaemon *drd_system_daemon_new(DrdConfig *config, DrdServerRuntime *runtime, DrdTlsCredentials *tls_credentials)
{
    /*
     * 功能：创建 system 守护实例并缓存依赖。
     * 逻辑：校验配置与运行时，创建对象并持有引用，TLS 凭据存在则增加引用。
     * 参数：config 配置；runtime 运行时；tls_credentials 可选 TLS 凭据。
     * 外部接口：GLib g_object_new/g_object_ref。
     */
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

gboolean drd_system_daemon_set_main_loop(DrdSystemDaemon *self, GMainLoop *loop)
{
    /*
     * 功能：设置主循环引用。
     * 逻辑：释放旧引用后为新循环增加引用，允许传入 NULL。
     * 参数：self system 守护实例；loop 主循环。
     * 外部接口：GLib g_main_loop_ref/g_main_loop_unref。
     */
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

guint drd_system_daemon_get_pending_client_count(DrdSystemDaemon *self)
{
    /*
     * 功能：查询待分配客户端数量。
     * 逻辑：返回 pending 队列长度。
     * 参数：self system 守护实例。
     * 外部接口：GLib GQueue。
     */
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), 0);
    return self->pending_clients != NULL ? self->pending_clients->length : 0;
}

guint drd_system_daemon_get_remote_client_count(DrdSystemDaemon *self)
{
    /*
     * 功能：查询已注册客户端数量。
     * 逻辑：返回哈希表元素数量。
     * 参数：self system 守护实例。
     * 外部接口：GLib g_hash_table_size。
     */
    g_return_val_if_fail(DRD_IS_SYSTEM_DAEMON(self), 0);
    return self->remote_clients != NULL ? g_hash_table_size(self->remote_clients) : 0;
}

static gboolean drd_system_daemon_start_listener(DrdSystemDaemon *self, GError **error)
{
    /*
     * 功能：启动 system 模式的 RDP 监听器。
     * 逻辑：若已存在则直接返回；从配置读取编码与认证参数创建监听器，启动监听并设置委派与 session 回调。
     * 参数：self system 守护实例；error 错误输出。
     * 外部接口：drd_config_get_encoding_options
     * 等配置接口；drd_rdp_listener_new/start/set_delegate/set_session_callback；日志 DRD_LOG_MESSAGE。
     */
    if (self->listener != NULL)
    {
        return TRUE;
    }

    const DrdEncodingOptions *encoding_opts = drd_config_get_encoding_options(self->config);
    if (encoding_opts == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Encoding options unavailable when starting system daemon listener");
        return FALSE;
    }

    self->listener =
            drd_rdp_listener_new(drd_config_get_bind_address(self->config), drd_config_get_port(self->config),
                                 self->runtime, encoding_opts, drd_config_is_nla_enabled(self->config),
                                 drd_config_get_nla_username(self->config), drd_config_get_nla_password(self->config),
                                 drd_config_get_pam_service(self->config), DRD_RUNTIME_MODE_SYSTEM);
    if (self->listener == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create system-mode RDP listener");
        return FALSE;
    }

    if (!drd_rdp_listener_start(self->listener, error))
    {
        g_clear_object(&self->listener);
        return FALSE;
    }
    drd_rdp_listener_set_delegate(self->listener, drd_system_daemon_delegate, self);
    drd_rdp_listener_set_session_callback(self->listener, drd_system_daemon_on_session_ready, self);

    DRD_LOG_MESSAGE("System daemon listening on %s:%u", drd_config_get_bind_address(self->config),
                    drd_config_get_port(self->config));
    return TRUE;
}

static gboolean drd_system_daemon_start_bus(DrdSystemDaemon *self, GError **error)
{
    /*
     * 功能：启动 DBus 端点并占用服务名。
     * 逻辑：获取 system bus 连接并占用 RemoteDesktop 名称；创建 dispatcher skeleton 并导出；创建 handover manager
     * 并绑定到连接。 参数：self system 守护实例；error 错误输出。 外部接口：GDBus
     * g_bus_get_sync/g_bus_own_name_on_connection/g_dbus_interface_skeleton_export/g_dbus_object_manager_server_set_connection；日志
     * DRD_LOG_MESSAGE。
     */
    g_assert(self->bus.connection == NULL);

    self->bus.connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    if (self->bus.connection == NULL)
    {
        return FALSE;
    }

    g_object_ref(self);
    self->bus.bus_name_owner_id = g_bus_own_name_on_connection(
            self->bus.connection, DRD_REMOTE_DESKTOP_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_REPLACE,
            drd_system_daemon_on_bus_name_acquired, drd_system_daemon_on_bus_name_lost, self, g_object_unref);

    if (self->bus.bus_name_owner_id == 0)
    {
        g_object_unref(self);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to own org.deepin.RemoteDesktop1 bus name");
        return FALSE;
    }

    self->bus.object_manager = g_dbus_object_manager_server_new(DRD_REMOTE_DESKTOP_OBJECT_PATH);
    g_dbus_object_manager_server_set_connection(self->bus.object_manager, self->bus.connection);

    self->bus.root_object = g_dbus_object_skeleton_new(DRD_REMOTE_DESKTOP_OBJECT_PATH);

    self->bus.common_iface = drd_dbus_remote_desktop1_remote_desktop1_skeleton_new();
    drd_dbus_remote_desktop1_remote_desktop1_set_runtime_mode(self->bus.common_iface, "system");
    drd_dbus_remote_desktop1_remote_desktop1_set_version(self->bus.common_iface, DRD_PROJECT_VERSION);

    self->bus.remote_login_iface = drd_dbus_remote_desktop1_remote_desktop1_remote_login_skeleton_new();

    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_enabled(self->bus.remote_login_iface, TRUE);
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_port(self->bus.remote_login_iface,
                                                                   (gint) drd_config_get_port(self->config));
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_tls_cert(self->bus.remote_login_iface,
                                                                       drd_config_get_certificate_path(self->config));
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_tls_key(self->bus.remote_login_iface,
                                                                      drd_config_get_private_key_path(self->config));
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_tls_fingerprint(self->bus.remote_login_iface, "");
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_nla_auth_enabled(self->bus.remote_login_iface,
                                                                               drd_config_is_nla_enabled(self->config));
    drd_dbus_remote_desktop1_remote_desktop1_remote_login_set_auto_logout_on_disconnect(
            self->bus.remote_login_iface, drd_config_should_logout_local_session_on_single_login(self->config));

    g_signal_connect(self->bus.remote_login_iface, "handle-request-handover",
                     G_CALLBACK(drd_system_daemon_handle_request_handover), self);
    g_signal_connect(self->bus.remote_login_iface, "handle-request-port",
                     G_CALLBACK(drd_system_daemon_handle_request_port), self);
    g_signal_connect(self->bus.remote_login_iface, "handle-enable-remote-login",
                     G_CALLBACK(drd_system_daemon_handle_enable_remote_login), self);
    g_signal_connect(self->bus.remote_login_iface, "handle-get-credentials",
                     G_CALLBACK(drd_system_daemon_handle_get_credentials), self);
    g_signal_connect(self->bus.remote_login_iface, "handle-set-credentials",
                     G_CALLBACK(drd_system_daemon_handle_set_credentials), self);
    g_signal_connect(self->bus.remote_login_iface, "handle-enable-nla-auth",
                     G_CALLBACK(drd_system_daemon_handle_enable_nla_auth), self);
    g_signal_connect(self->bus.remote_login_iface, "handle-enable-auto-logout-rdp-disconnect",
                     G_CALLBACK(drd_system_daemon_handle_enable_auto_logout_rdp_disconnect), self);
    g_signal_connect(self->bus.remote_login_iface, "handle-gen-nla-credential",
                     G_CALLBACK(drd_system_daemon_handle_gen_nla_credential), self);

    g_dbus_object_skeleton_add_interface(self->bus.root_object, G_DBUS_INTERFACE_SKELETON(self->bus.common_iface));
    g_dbus_object_skeleton_add_interface(self->bus.root_object,
                                         G_DBUS_INTERFACE_SKELETON(self->bus.remote_login_iface));
    g_dbus_object_manager_server_export(self->bus.object_manager, self->bus.root_object);
    drd_system_daemon_update_session_list(self);

    DRD_LOG_MESSAGE("System daemon exported %s at %s", DRD_REMOTE_DESKTOP_BUS_NAME, DRD_REMOTE_DESKTOP_OBJECT_PATH);
    return TRUE;
}

gboolean drd_system_daemon_start(DrdSystemDaemon *self, GError **error)
{
    /*
     * 功能：启动 system 守护整体服务。
     * 逻辑：先启动监听器；若总线尚未初始化则启动 DBus 服务，失败时回滚监听器与总线。
     * 参数：self system 守护实例；error 错误输出。
     * 外部接口：内部
     * drd_system_daemon_start_listener/drd_system_daemon_start_bus/drd_system_daemon_stop_listener/drd_system_daemon_reset_bus_context。
     */
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

static gboolean drd_system_daemon_on_start_handover(DrdDBusRemoteDesktop1RemoteDesktop1HandoverSession *interface,
                                                    GDBusMethodInvocation *invocation, const gchar *one_time_auth_token,
                                                    gpointer user_data)
{
    /*
     * 功能：处理 handover 对象的 StartHandover 调用。
     * 逻辑：读取 TLS 物料；若已有本地 session 则发送 Server Redirection 并清理会话；否则通过 DBus 发出
     * RedirectClient；最后返回 TLS PEM 并根据重定向结果更新状态。 参数：interface handover 接口；invocation
     * 调用上下文；username/password 目标凭据；user_data 远程客户端。
     * 外部接口：drd_tls_credentials_read_material、drd_rdp_session_send_server_redirection/drd_rdp_session_notify_error；GDBus
     * handover emit/complete；日志 DRD_LOG_MESSAGE/DRD_LOG_WARNING。
     */
    DrdRemoteClient *client = user_data;
    DrdSystemDaemon *self = client->daemon;
    g_autofree gchar *username = NULL;
    g_autofree gchar *password = NULL;
    g_autofree gchar *certificate = NULL;
    g_autofree gchar *key = NULL;
    gboolean redirected_locally = FALSE;
    const gboolean has_routing_token = client->routing != NULL && client->routing->routing_token != NULL;
    drd_system_daemon_touch_client(client);

    g_autoptr(GError) io_error = NULL;
    if (!drd_dbus_auth_token_parse(one_time_auth_token, &username, &password, &io_error))
    {
        g_dbus_method_invocation_return_gerror(invocation, io_error);
        return TRUE;
    }

    g_clear_error(&io_error);
    if (!drd_system_daemon_load_tls_material(self, &certificate, &key, &io_error))
    {
        g_dbus_method_invocation_return_gerror(invocation, io_error);
        return TRUE;
    }

    if (client->session != NULL)
    {
        // gchar *gen_routing_token=g_strdup (client->handover_dbus_path + strlen (REMOTE_DESKTOP_CLIENT_OBJECT_PATH
        // "/"));
        DRD_LOG_MESSAGE("client session not NULL,routing token is %s", client->routing->routing_token);
        if (!drd_rdp_session_send_server_redirection(client->session, client->routing->routing_token, username,
                                                     password, certificate))
        {
            g_dbus_method_invocation_return_error(invocation, G_IO_ERROR, G_IO_ERROR_FAILED,
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
            drd_dbus_remote_desktop1_remote_desktop1_handover_session_emit_redirect_client(
                    interface, client->routing->routing_token, one_time_auth_token);
        }
        else
        {
            DRD_LOG_WARNING("StartHandover for %s missing routing token; skipping RedirectClient signal",
                            client->handover_dbus_path);
        }
    }

    drd_dbus_remote_desktop1_remote_desktop1_handover_session_complete_start_handover(interface, invocation,
                                                                                      certificate, key);

    if (redirected_locally)
    {
        client->assigned = TRUE;
    }

    DRD_LOG_MESSAGE("StartHandover acknowledged for %s", client->handover_dbus_path);
    return TRUE;
}

static gboolean drd_system_daemon_on_take_client(DrdDBusRemoteDesktop1RemoteDesktop1HandoverSession *interface,
                                                 GDBusMethodInvocation *invocation, GUnixFDList *fd_list,
                                                 gpointer user_data)
{
    /*
     * 功能：处理 TakeClient 调用，将现有连接 FD 交给 handover 进程。
     * 逻辑：获取连接 socket FD，封装到 GUnixFDList 返回；关闭本地流并根据 handover 次数决定重新入队或移除。
     * 参数：interface handover 接口；invocation 调用上下文；fd_list 未使用；user_data 远程客户端。
     * 外部接口：GLib g_socket_connection_get_socket/g_unix_fd_list_append/g_io_stream_close；GDBus
     * complete_take_client；日志 DRD_LOG_MESSAGE/DRD_LOG_WARNING。
     */
    (void) fd_list;
    DrdRemoteClient *client = user_data;
    DrdSystemDaemon *self = client->daemon;
    drd_system_daemon_touch_client(client);
    GSocket *socket = g_socket_connection_get_socket(client->connection);
    DRD_LOG_MESSAGE("client take client connection established");
    if (!G_IS_SOCKET(socket))
    {
        g_dbus_method_invocation_return_error(invocation, G_IO_ERROR, G_IO_ERROR_FAILED,
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
    drd_dbus_remote_desktop1_remote_desktop1_handover_session_complete_take_client(interface, invocation, out_list,
                                                                                   handle);

    g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
    g_clear_object(&client->connection);
    g_clear_object(&client->session);

    client->handover_count++;
    // if (client->handover_count >= 2)
    // {
    //     DRD_LOG_MESSAGE("TODO remove client %s", client->handover_dbus_path);
    //     // drd_system_daemon_remove_client(self, client);
    // }
    // else
    // {
    client->assigned = FALSE;
    if (!drd_system_daemon_queue_client(self, client))
    {
        DRD_LOG_WARNING("Failed to requeue handover client %s, removing entry", client->handover_dbus_path);
        drd_system_daemon_remove_client(self, client);
    }
    else
    {
        DRD_LOG_MESSAGE("Client %s ready for next handover stage", client->handover_dbus_path);
    }
    // }

    return TRUE;
}

static gboolean
drd_system_daemon_on_get_system_credentials(DrdDBusRemoteDesktop1RemoteDesktop1HandoverSession *interface,
                                            GDBusMethodInvocation *invocation, gpointer user_data)
{
    /*
     * 功能：处理 GetSystemCredentials 调用。
     * 逻辑：当前未实现，直接返回 NOT_SUPPORTED 错误。
     * 参数：interface handover 接口；invocation 调用上下文；user_data 未用。
     * 外部接口：GDBus g_dbus_method_invocation_return_error。
     */
    (void) interface;
    (void) user_data;
    return drd_system_daemon_method_not_supported(invocation, "GetSystemCredentials");
}
