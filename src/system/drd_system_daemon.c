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

#define DRD_SYSTEM_MAX_PENDING_CLIENTS 32
#define DRD_SYSTEM_CLIENT_STALE_TIMEOUT_US (30 * G_USEC_PER_SEC)

typedef struct _DrdRemoteClient
{
    DrdSystemDaemon *daemon;
    gchar *id;
    guint32 remote_id;
    DrdRoutingTokenInfo *routing;
    GSocketConnection *connection;
    DrdRdpSession *session;
    DrdDBusRemoteDesktopRdpHandover *handover_iface;
    GDBusObjectSkeleton *object_skeleton;
    gboolean assigned;
    gboolean use_system_credentials;
    guint handover_count;
    gint64 last_activity_us;
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
    /*
     * 功能：根据 routing token 构造 handover 对象路径。
     * 逻辑：校验 token 非零后拼接 handover 基础路径与数值。
     * 参数：routing_token 路由 token 数值。
     * 外部接口：GLib g_strdup_printf。
     */
    g_return_val_if_fail(routing_token != 0, NULL);

    return g_strdup_printf("%s/%u", DRD_REMOTE_DESKTOP_HANDOVERS_OBJECT_PATH, routing_token);
}

/*
 * 功能：从 handover 对象路径提取 routing token。
 * 逻辑：校验路径是否带 handover 前缀，缺失 token 或前缀不符时记录警告并返回 NULL，否则返回 token 字符串副本。
 * 参数：id handover 对象路径。
 * 外部接口：GLib g_str_has_prefix/g_strdup；日志 DRD_LOG_WARNING。
 */
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

/*
 * 功能：生成唯一的 handover 对象路径与 routing token。
 * 逻辑：循环随机产生非零 token，转换成 remote_id 并检查哈希表中是否已存在；成功后派生对应的 routing_token 字符串并输出。
 * 参数：self system 守护实例；remote_id_out 输出 handover 对象路径；routing_token_out 输出 token 字符串。
 * 外部接口：GLib g_random_int/g_hash_table_contains/g_strdup_printf。
 */
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

static gboolean drd_system_daemon_queue_client(DrdSystemDaemon *self, DrdRemoteClient *client);

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

static void drd_system_daemon_on_bus_name_acquired(GDBusConnection *connection,
                                                   const gchar *name,
                                                   gpointer user_data);

static void drd_system_daemon_on_bus_name_lost(GDBusConnection *connection,
                                               const gchar *name,
                                               gpointer user_data);

G_DEFINE_TYPE(DrdSystemDaemon, drd_system_daemon, G_TYPE_OBJECT)

/*
 * 功能：获取当前单调时钟值（微秒）。
 * 逻辑：直接封装 g_get_monotonic_time。
 * 参数：无。
 * 外部接口：GLib g_get_monotonic_time。
 */
static gint64
drd_system_daemon_now_us(void)
{
    return g_get_monotonic_time();
}

/*
 * 功能：刷新远程客户端的最近活动时间。
 * 逻辑：客户端非空时写入当前微秒时间戳。
 * 参数：client 远程客户端。
 * 外部接口：内部 drd_system_daemon_now_us。
 */
static void
drd_system_daemon_touch_client(DrdRemoteClient *client)
{
    if (client == NULL)
    {
        return;
    }

    client->last_activity_us = drd_system_daemon_now_us();
}

/*
 * 功能：移除长时间未被领取的待处理 handover 客户端。
 * 逻辑：遍历 pending 队列，若未分配且空闲时间超过 DRD_SYSTEM_CLIENT_STALE_TIMEOUT_US，则记录日志并调用 remove_client 清理。
 * 参数：self system 守护实例；now_us 当前时间戳（微秒）。
 * 外部接口：日志 DRD_LOG_WARNING；内部 drd_system_daemon_remove_client。
 */
static void
drd_system_daemon_prune_stale_pending_clients(DrdSystemDaemon *self, gint64 now_us)
{
    if (self->pending_clients == NULL || self->pending_clients->length == 0)
    {
        return;
    }

    GList *link = self->pending_clients->head;
    while (link != NULL)
    {
        GList *next = link->next;
        DrdRemoteClient *candidate = link->data;
        if (candidate != NULL && !candidate->assigned && candidate->last_activity_us > 0)
        {
            gint64 idle_us = now_us - candidate->last_activity_us;
            if (idle_us >= DRD_SYSTEM_CLIENT_STALE_TIMEOUT_US)
            {
                DRD_LOG_WARNING("Expiring stale handover %s after %.0f seconds in queue",
                                candidate->id != NULL ? candidate->id : "unknown",
                                idle_us / (gdouble) G_USEC_PER_SEC);
                drd_system_daemon_remove_client(self, candidate);
            }
        }
        link = next;
    }
}

/*
 * 功能：销毁远程客户端结构体并清理关联资源。
 * 逻辑：释放 DBus 骨架/接口，清空连接上存储的数据并释放连接/会话，释放 routing 信息和标识字符串，最后释放结构体内存。
 * 参数：client 远程客户端。
 * 外部接口：GLib g_clear_object/g_clear_pointer/g_object_set_data；内部 drd_routing_token_info_free。
 */
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

/*
 * 功能：通过 routing token 定位远程客户端。
 * 逻辑：将字符串 token 转为整数生成 remote_id，再在哈希表中查找匹配的客户端，非法 token 会记录警告。
 * 参数：self system 守护实例；routing_token 路由 token 字符串。
 * 外部接口：GLib g_ascii_string_to_unsigned/g_hash_table_lookup；日志 DRD_LOG_WARNING。
 */
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

/*
 * 功能：将客户端放入等待队列。
 * 逻辑：若已分配则直接返回；先清理超时队列，再检查队列上限，合格则记录活跃时间并入队。
 * 参数：self system 守护实例；client 远程客户端。
 * 外部接口：内部 drd_system_daemon_prune_stale_pending_clients；GLib g_queue_push_tail。
 */
static gboolean
drd_system_daemon_queue_client(DrdSystemDaemon *self, DrdRemoteClient *client)
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
        DRD_LOG_WARNING("Pending handover queue full (%u >= %u), cannot enqueue %s",
                        pending,
                        DRD_SYSTEM_MAX_PENDING_CLIENTS,
                        client->id != NULL ? client->id : "unknown");
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

/*
 * 功能：完全移除并注销一个远程客户端。
 * 逻辑：先从等待队列移除；若 handover manager 存在则取消导出对象；清理连接上存储的标记；释放会话并从哈希表删除。
 * 参数：self system 守护实例；client 远程客户端。
 * 外部接口：GDBus g_dbus_object_manager_server_unexport；GLib g_object_set_data/g_hash_table_remove。
 */
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

/*
 * 功能：将新连接注册为 handover 客户端并导出 DBus 接口。
 * 逻辑：生成或复用 routing token/remote_id，构建 handover skeleton 并导出到 handover manager；在连接上写入元数据；入队等待。
 * 参数：self system 守护实例；connection 新连接；info peek 到的 routing token 信息。
 * 外部接口：GLib g_ascii_string_to_unsigned/g_hash_table_contains/g_object_set_data；GDBus drd_dbus_remote_desktop_rdp_handover_skeleton_new/g_dbus_object_skeleton_add_interface/export。
 */
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
    client->remote_id = 0;
    if (parsed_token_value > 0)
    {
        client->remote_id = (guint32) parsed_token_value;
    }

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
    if (!drd_system_daemon_queue_client(self, client))
    {
        DRD_LOG_WARNING("Rejecting handover client %s because pending queue is full", client->id);
        drd_system_daemon_remove_client(self, client);
        return FALSE;
    }

    const gchar *token_preview = client->routing != NULL && client->routing->routing_token != NULL
                                     ? client->routing->routing_token
                                     : "unknown";
    DRD_LOG_MESSAGE("Registered handover client %s (token=%s)",
                    client->id,
                    token_preview);

    return TRUE;
}

// return FALSE 时，需要继续处理这个connection;return TRUE时，代表已经处理过，需要让handover进程来处理；
/*
 * 功能：监听器委派回调，用于在 system 模式下注册/续接 handover 客户端。
 * 逻辑：peek 路由 token；若 token 已存在且未绑定 session，则更新连接并触发 TakeClientReady；否则注册为新客户端并决定是否继续交由默认监听器处理。
 * 参数：listener RDP 监听器；connection 新连接；user_data system 守护实例；error 错误输出。
 * 外部接口：drd_routing_token_peek 读取 token；GIO g_socket_connection_factory_create_connection/g_object_set_data；drd_system_daemon_register_client；GDBus 信号 drd_dbus_remote_desktop_rdp_handover_emit_take_client_ready。
 */
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
            drd_system_daemon_touch_client(existing);

            drd_dbus_remote_desktop_rdp_handover_emit_take_client_ready(
                existing->handover_iface,
                existing->use_system_credentials);
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
                    drd_system_daemon_get_remote_client_count(self),
                    drd_system_daemon_get_pending_client_count(self));

    /* Allow the default listener to accept the connection so FreeRDP can build a session and send redirection. */
    g_object_unref(connection);
    return FALSE;
}

/*
 * 功能：监听器回调，记录连接对应的会话对象。
 * 逻辑：从连接 metadata 获取客户端结构；替换 session 引用并根据客户端请求的能力决定是否使用系统凭据；创建 LightDM 远程显示；刷新活跃时间。
 * 参数：listener 监听器；session 新会话；connection 底层连接；user_data system 守护实例。
 * 外部接口：GLib g_object_get_data/g_clear_object/g_object_ref；drd_rdp_session_client_is_mstsc；
 *           LightDM proxy drd_dbus_lightdm_remote_display_factory_proxy_new_for_bus_sync 与
 *           drd_dbus_lightdm_remote_display_factory_call_create_remote_greeter_display_sync。
 */
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

    // call lightdm create remote display
    if (client->remote_id == 0)
    {
        DRD_LOG_WARNING("Client %s missing remote id, skip remote display",
                        client->id != NULL ? client->id : "unknown");
        drd_system_daemon_touch_client(client);
        return;
    }
    if (!self->remote_display_factory)
    {
        self->remote_display_factory =
                drd_dbus_lightdm_remote_display_factory_proxy_new_for_bus_sync(
                        G_BUS_TYPE_SYSTEM,
                        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                        DRD_LIGHTDM_REMOTE_FACTORY_BUS_NAME,
                        DRD_LIGHTDM_REMOTE_FACTORY_OBJECT_PATH,
                        NULL,
                        NULL);
    }
    g_autofree gchar *session_path = NULL;
    g_autofree GError *error = NULL;
    const gchar *peer_address = drd_rdp_session_get_peer_address(session);
    if (!drd_dbus_lightdm_remote_display_factory_call_create_remote_greeter_display_sync(
                self->remote_display_factory,
                client->remote_id,
                1920,
                1080,
                peer_address,
                &session_path,
                NULL,
                &error))
    {
        DRD_LOG_WARNING("create remote display failed %s", error->message);
        drd_system_daemon_touch_client(client);
        return;
    }
    DRD_LOG_MESSAGE("session_path=%s", session_path);

    drd_system_daemon_touch_client(client);
}

static gboolean
drd_system_daemon_load_tls_material(DrdSystemDaemon *self,
                                    gchar **certificate,
                                    gchar **key,
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

static gboolean
drd_system_daemon_handle_request_handover(DrdDBusRemoteDesktopRdpDispatcher *interface,
                                          GDBusMethodInvocation *invocation,
                                          gpointer user_data)
{
    /*
     * 功能：处理 dispatcher 的 RequestHandover 调用。
     * 逻辑：清理过期客户端后从 pending 队列取出一个等待对象；若为空返回 NOT_FOUND 错误；否则标记 assigned、刷新活跃时间并返回 handover 对象路径。
     * 参数：interface dispatcher 接口；invocation DBus 调用上下文；user_data system 守护实例。
     * 外部接口：GDBus drd_dbus_remote_desktop_rdp_dispatcher_complete_request_handover/g_dbus_method_invocation_return_error；日志 DRD_LOG_MESSAGE。
     */
    DrdSystemDaemon *self = DRD_SYSTEM_DAEMON(user_data);
    drd_system_daemon_prune_stale_pending_clients(self, drd_system_daemon_now_us());
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
    drd_system_daemon_touch_client(client);
    drd_dbus_remote_desktop_rdp_dispatcher_complete_request_handover(interface,
                                                                     invocation,
                                                                     client->id);
    DRD_LOG_MESSAGE("Dispatching handover client %s", client->id);
    return TRUE;
}

static void
drd_system_daemon_reset_bus_context(DrdSystemDaemon *self)
{
    /*
     * 功能：撤销 DBus 相关导出与总线占用。
     * 逻辑：取消 handover manager 连接，unexport dispatcher，释放总线名称并清理连接引用。
     * 参数：self system 守护实例。
     * 外部接口：GDBus g_dbus_object_manager_server_set_connection/unexport、g_bus_unown_name。
     */
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

static void
drd_system_daemon_on_bus_name_acquired(GDBusConnection *connection,
                                       const gchar *name,
                                       gpointer user_data)
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

static void
drd_system_daemon_on_bus_name_lost(GDBusConnection *connection,
                                   const gchar *name,
                                   gpointer user_data)
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

static void
drd_system_daemon_request_shutdown(DrdSystemDaemon *self)
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

void
drd_system_daemon_stop(DrdSystemDaemon *self)
{
    /*
     * 功能：停止 system 守护的对外服务与监听。
     * 逻辑：重置 DBus 上下文、停止监听器、清空客户端哈希表与队列，并请求主循环退出。
     * 参数：self system 守护实例。
     * 外部接口：内部 drd_system_daemon_reset_bus_context/drd_system_daemon_stop_listener/drd_system_daemon_request_shutdown；GLib g_hash_table_remove_all/g_queue_clear。
     */
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

static void
drd_system_daemon_class_init(DrdSystemDaemonClass *klass)
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

static void
drd_system_daemon_init(DrdSystemDaemon *self)
{
    /*
     * 功能：初始化 system 守护实例字段。
     * 逻辑：清空总线上下文、创建客户端哈希表与队列，主循环置空。
     * 参数：self system 守护实例。
     * 外部接口：GLib g_hash_table_new_full/g_queue_new。
     */
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

gboolean
drd_system_daemon_set_main_loop(DrdSystemDaemon *self, GMainLoop *loop)
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

guint
drd_system_daemon_get_pending_client_count(DrdSystemDaemon *self)
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

guint
drd_system_daemon_get_remote_client_count(DrdSystemDaemon *self)
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

static gboolean
drd_system_daemon_start_listener(DrdSystemDaemon *self, GError **error)
{
    /*
     * 功能：启动 system 模式的 RDP 监听器。
     * 逻辑：若已存在则直接返回；从配置读取编码与认证参数创建监听器，启动监听并设置委派与 session 回调。
     * 参数：self system 守护实例；error 错误输出。
     * 外部接口：drd_config_get_encoding_options 等配置接口；drd_rdp_listener_new/start/set_delegate/set_session_callback；日志 DRD_LOG_MESSAGE。
     */
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
    /*
     * 功能：启动 DBus 端点并占用服务名。
     * 逻辑：获取 system bus 连接并占用 RemoteDesktop 名称；创建 dispatcher skeleton 并导出；创建 handover manager 并绑定到连接。
     * 参数：self system 守护实例；error 错误输出。
     * 外部接口：GDBus g_bus_get_sync/g_bus_own_name_on_connection/g_dbus_interface_skeleton_export/g_dbus_object_manager_server_set_connection；日志 DRD_LOG_MESSAGE。
     */
    g_assert(self->bus.connection == NULL);

    self->bus.connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    if (self->bus.connection == NULL)
    {
        return FALSE;
    }

    g_object_ref(self);
    self->bus.bus_name_owner_id =
            g_bus_own_name_on_connection(self->bus.connection,
                                         DRD_REMOTE_DESKTOP_BUS_NAME,
                                         G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                         drd_system_daemon_on_bus_name_acquired,
                                         drd_system_daemon_on_bus_name_lost,
                                         self,
                                         g_object_unref);

    if (self->bus.bus_name_owner_id == 0)
    {
        g_object_unref(self);
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
    /*
     * 功能：启动 system 守护整体服务。
     * 逻辑：先启动监听器；若总线尚未初始化则启动 DBus 服务，失败时回滚监听器与总线。
     * 参数：self system 守护实例；error 错误输出。
     * 外部接口：内部 drd_system_daemon_start_listener/drd_system_daemon_start_bus/drd_system_daemon_stop_listener/drd_system_daemon_reset_bus_context。
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

static gboolean
drd_system_daemon_on_start_handover(DrdDBusRemoteDesktopRdpHandover *interface,
                                    GDBusMethodInvocation *invocation,
                                    const gchar *username,
                                    const gchar *password,
                                    gpointer user_data)
{
    /*
     * 功能：处理 handover 对象的 StartHandover 调用。
     * 逻辑：读取 TLS 物料；若已有本地 session 则发送 Server Redirection 并清理会话；否则通过 DBus 发出 RedirectClient；最后返回 TLS PEM 并根据重定向结果更新状态。
     * 参数：interface handover 接口；invocation 调用上下文；username/password 目标凭据；user_data 远程客户端。
     * 外部接口：drd_tls_credentials_read_material、drd_rdp_session_send_server_redirection/drd_rdp_session_notify_error；GDBus handover emit/complete；日志 DRD_LOG_MESSAGE/DRD_LOG_WARNING。
     */
    DrdRemoteClient *client = user_data;
    DrdSystemDaemon *self = client->daemon;
    g_autofree gchar *certificate = NULL;
    g_autofree gchar *key = NULL;
    gboolean redirected_locally = FALSE;
    const gboolean has_routing_token =
            client->routing != NULL && client->routing->routing_token != NULL;
    drd_system_daemon_touch_client(client);

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
    /*
     * 功能：处理 TakeClient 调用，将现有连接 FD 交给 handover 进程。
     * 逻辑：获取连接 socket FD，封装到 GUnixFDList 返回；关闭本地流并根据 handover 次数决定重新入队或移除。
     * 参数：interface handover 接口；invocation 调用上下文；fd_list 未使用；user_data 远程客户端。
     * 外部接口：GLib g_socket_connection_get_socket/g_unix_fd_list_append/g_io_stream_close；GDBus complete_take_client；日志 DRD_LOG_MESSAGE/DRD_LOG_WARNING。
     */
    (void) fd_list;
    DrdRemoteClient *client = user_data;
    DrdSystemDaemon *self = client->daemon;
    drd_system_daemon_touch_client(client);
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
        if (!drd_system_daemon_queue_client(self, client))
        {
            DRD_LOG_WARNING("Failed to requeue handover client %s, removing entry", client->id);
            drd_system_daemon_remove_client(self, client);
        }
        else
        {
            DRD_LOG_MESSAGE("Client %s ready for next handover stage", client->id);
        }
    }

    return TRUE;
}

static gboolean
drd_system_daemon_on_get_system_credentials(DrdDBusRemoteDesktopRdpHandover *interface,
                                            GDBusMethodInvocation *invocation,
                                            gpointer user_data)
{
    /*
     * 功能：处理 GetSystemCredentials 调用。
     * 逻辑：当前未实现，直接返回 NOT_SUPPORTED 错误。
     * 参数：interface handover 接口；invocation 调用上下文；user_data 未用。
     * 外部接口：GDBus g_dbus_method_invocation_return_error。
     */
    (void) interface;
    (void) user_data;
    g_dbus_method_invocation_return_error(invocation,
                                          G_IO_ERROR,
                                          G_IO_ERROR_NOT_SUPPORTED,
                                          "System credentials not available");
    return TRUE;
}
