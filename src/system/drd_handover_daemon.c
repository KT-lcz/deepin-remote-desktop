#include "system/drd_handover_daemon.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <string.h>

#include "core/drd_dbus_constants.h"
#include "drd-dbus-remote-desktop.h"
#include "transport/drd_rdp_listener.h"
#include "session/drd_rdp_session.h"
#include "utils/drd_log.h"

static gboolean drd_handover_daemon_bind_handover(DrdHandoverDaemon * self, GError * *error);
static gboolean drd_handover_daemon_start_session(DrdHandoverDaemon * self, GError * *error);
static gboolean drd_handover_daemon_take_client(DrdHandoverDaemon * self, GError * *error);

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

static void drd_handover_daemon_on_session_ready(DrdRdpListener *listener,
                                                 DrdRdpSession *session,
                                                 gpointer user_data);

static gboolean drd_handover_daemon_redirect_active_client(DrdHandoverDaemon *self,
                                                           const gchar *routing_token,
                                                           const gchar *username,
                                                           const gchar *password);

static void drd_handover_daemon_request_shutdown(DrdHandoverDaemon * self);
static gboolean drd_handover_daemon_refresh_nla_credentials(DrdHandoverDaemon * self, GError * *error);
static void drd_handover_daemon_clear_nla_credentials(DrdHandoverDaemon * self);

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
    DrdRdpSession *active_session;
    GMainLoop *main_loop;

    gchar *nla_username;
    gchar *nla_password;
};

G_DEFINE_TYPE(DrdHandoverDaemon, drd_handover_daemon, G_TYPE_OBJECT)

/*
 * 功能：释放 handover 守护持有的资源。
 * 逻辑：清理 DBus 代理、监听器、会话、NLA 凭据、TLS/配置/运行时及主循环引用，最后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdHandoverDaemon。
 * 外部接口：GLib g_clear_object/g_clear_pointer/g_main_loop_unref；内部 drd_handover_daemon_clear_nla_credentials。
 */
static void
drd_handover_daemon_dispose(GObject *object)
{
    DrdHandoverDaemon *self = DRD_HANDOVER_DAEMON(object);

    g_clear_object(&self->dispatcher_proxy);
    g_clear_object(&self->handover_proxy);
    g_clear_pointer(&self->handover_object_path, g_free);
    g_clear_object(&self->listener);
    g_clear_object(&self->active_session);
    drd_handover_daemon_clear_nla_credentials(self);
    g_clear_object(&self->tls_credentials);
    g_clear_object(&self->runtime);
    g_clear_object(&self->config);
    g_clear_pointer(&self->main_loop, g_main_loop_unref);

    G_OBJECT_CLASS(drd_handover_daemon_parent_class)->dispose(object);
}

/*
 * 功能：绑定类级别析构回调。
 * 逻辑：将自定义 dispose 挂载到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void
drd_handover_daemon_class_init(DrdHandoverDaemonClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_handover_daemon_dispose;
}

/*
 * 功能：初始化 handover 守护实例默认值。
 * 逻辑：清空 DBus 代理、监听器、会话、主循环与 NLA 字段。
 * 参数：self 守护实例。
 * 外部接口：无额外外部库。
 */
static void
drd_handover_daemon_init(DrdHandoverDaemon *self)
{
    self->handover_proxy = NULL;
    self->handover_object_path = NULL;
    self->listener = NULL;
    self->active_session = NULL;
    self->main_loop = NULL;

    self->nla_username = NULL;
    self->nla_password = NULL;
}

/*
 * 功能：创建 handover 守护实例并缓存外部依赖。
 * 逻辑：校验 config/runtime，创建对象并持有引用，TLS 凭据存在则增加引用。
 * 参数：config 配置；runtime 运行时；tls_credentials 可选 TLS 凭据。
 * 外部接口：GLib g_object_new/g_object_ref。
 */
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

/*
 * 功能：生成带前缀的随机 token。
 * 逻辑：生成指定字节数的随机值，转换为 hex 字符串并附加前缀。
 * 参数：prefix 可选字符串前缀；random_bytes 随机字节数。
 * 外部接口：GLib g_random_int_range/g_string_sized_new/g_string_append_printf。
 */
static gchar *
drd_handover_daemon_generate_token(const gchar *prefix, gsize random_bytes)
{
    g_autofree guint8 *bytes = g_new(guint8, random_bytes);
    if (bytes == NULL)
    {
        return NULL;
    }

    for (gsize i = 0; i < random_bytes; i++)
    {
        bytes[i] = (guint8) g_random_int_range(0, 256);
    }

    g_autoptr(GString)
    token = g_string_sized_new((prefix != NULL ? strlen(prefix) : 0) +
                               random_bytes * 2);
    if (token == NULL)
    {
        return NULL;
    }

    if (prefix != NULL)
    {
        g_string_append(token, prefix);
    }
    for (gsize i = 0; i < random_bytes; i++)
    {
        g_string_append_printf(token, "%02x", bytes[i]);
    }

    return g_string_free(g_steal_pointer(&token), FALSE);
}

/*
 * 功能：清理 handover 使用的临时 NLA 凭据。
 * 逻辑：释放用户名；密码存在则先覆盖为 0 再释放，最后置空。
 * 参数：self 守护实例。
 * 外部接口：C 库 memset/strlen；GLib g_clear_pointer/g_free。
 */
static void
drd_handover_daemon_clear_nla_credentials(DrdHandoverDaemon *self)
{
    g_return_if_fail(DRD_IS_HANDOVER_DAEMON(self));

    g_clear_pointer(&self->nla_username, g_free);
    if (self->nla_password != NULL)
    {
        memset(self->nla_password, 0, strlen(self->nla_password));
        g_clear_pointer(&self->nla_password, g_free);
    }
}

/*
 * 功能：生成新的随机 NLA 用户名/密码。
 * 逻辑：先清理旧凭据，再生成短 token 作为用户名与更长 token 作为密码，任一失败则报错。
 * 参数：self 守护实例；error 错误输出。
 * 外部接口：内部 drd_handover_daemon_generate_token、drd_handover_daemon_clear_nla_credentials；GLib g_set_error_literal。
 */
static gboolean
drd_handover_daemon_refresh_nla_credentials(DrdHandoverDaemon *self, GError **error)
{
    g_return_val_if_fail(DRD_IS_HANDOVER_DAEMON(self), FALSE);

    drd_handover_daemon_clear_nla_credentials(self);

    self->nla_username = drd_handover_daemon_generate_token("handover-", 8);
    self->nla_password = drd_handover_daemon_generate_token("handover-", 16);
    if (self->nla_username == NULL || self->nla_password == NULL)
    {
        drd_handover_daemon_clear_nla_credentials(self);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to generate NLA credentials for handover");
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：向 dispatcher 申请 handover 对象并绑定信号。
 * 逻辑：调用 DBus RequestHandover 获取 object path，创建 handover 代理并连接 RedirectClient/TakeClientReady/RestartHandover 信号。
 * 参数：self 守护实例；error 错误输出。
 * 外部接口：GDBus 生成的 drd_dbus_remote_desktop_rdp_dispatcher_call_request_handover_sync、drd_dbus_remote_desktop_rdp_handover_proxy_new_for_bus_sync；GLib g_signal_connect；日志 DRD_LOG_MESSAGE。
 */
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

/*
 * 功能：向 dispatcher 发起 StartHandover 以获取一次性 TLS 与 NLA 凭据。
 * 逻辑：要求已有随机 NLA 凭据；调用 StartHandover 获取 TLS PEM；若未初始化凭据对象则创建空对象并加载 PEM；日志记录证书长度。
 * 参数：self 守护实例；error 错误输出。
 * 外部接口：GDBus drd_dbus_remote_desktop_rdp_handover_call_start_handover_sync；TLS 处理 drd_tls_credentials_new_empty/drd_tls_credentials_reload_from_pem；日志 DRD_LOG_MESSAGE。
 */
static gboolean
drd_handover_daemon_start_session(DrdHandoverDaemon *self, GError **error)
{
    g_return_val_if_fail(self->nla_username != NULL, FALSE);
    g_return_val_if_fail(self->nla_password != NULL, FALSE);

    g_autofree gchar *certificate = NULL;
    g_autofree gchar *key = NULL;
    if (!drd_dbus_remote_desktop_rdp_handover_call_start_handover_sync(self->handover_proxy,
                                                                       self->nla_username,
                                                                       self->nla_password,
                                                                       &certificate,
                                                                       &key,
                                                                       NULL,
                                                                       error))
    {
        return FALSE;
    }

    if (self->tls_credentials == NULL)
    {
        self->tls_credentials = drd_tls_credentials_new_empty();
    }
    if (self->tls_credentials == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "TLS credentials unavailable for handover listener");
        return FALSE;
    }
    if (certificate == NULL || key == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Dispatcher did not provide TLS material");
        return FALSE;
    }
    if (!drd_tls_credentials_reload_from_pem(self->tls_credentials, certificate, key, error))
    {
        return FALSE;
    }

    DRD_LOG_MESSAGE("StartHandover negotiated TLS (%zu bytes cert)",
                    certificate != NULL ? strlen(certificate) : 0);
    return TRUE;
}

/*
 * 功能：接管 dispatcher 已经建立的客户端连接。
 * 逻辑：调用 TakeClient 获取 Unix FD，基于 fd 构造 GSocket/GSocketConnection 并交给 RDP 监听器接管。
 * 参数：self 守护实例；error 错误输出。
 * 外部接口：GDBus drd_dbus_remote_desktop_rdp_handover_call_take_client_sync；GLib GUnixFDList/g_socket_new_from_fd/g_socket_connection_factory_create_connection；drd_rdp_listener_adopt_connection。
 */
static gboolean
drd_handover_daemon_take_client(DrdHandoverDaemon *self, GError **error)
{
    g_autoptr(GVariant)
    fd_variant = NULL;
    g_autoptr(GUnixFDList)
    fd_list = NULL;
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

    g_autoptr(GSocket)
    socket = g_socket_new_from_fd(fd, error);
    if (!G_IS_SOCKET(socket))
    {
        return FALSE;
    }

    g_autoptr(GSocketConnection)
    connection =
            g_socket_connection_factory_create_connection(socket);
    if (!drd_rdp_listener_adopt_connection(self->listener,
                                           g_steal_pointer(&connection),
                                           error))
    {
        return FALSE;
    }

    DRD_LOG_MESSAGE("Adopted redirected client");
    return TRUE;
}

/*
 * 功能：处理 dispatcher 的 RedirectClient 信号，将活跃会话重定向到新路由。
 * 逻辑：记录日志后调用 redirect_active_client 发送 Server Redirection；失败则输出警告；成功后停止守护并请求退出。
 * 参数：interface DBus handover 接口；routing_token/username/password 路由与凭据；user_data handover 守护实例。
 * 外部接口：日志 DRD_LOG_MESSAGE/DRD_LOG_WARNING；内部 drd_handover_daemon_redirect_active_client/drd_handover_daemon_stop/drd_handover_daemon_request_shutdown。
 */
static void
drd_handover_daemon_on_redirect_client(DrdDBusRemoteDesktopRdpHandover *interface,
                                       const gchar *routing_token,
                                       const gchar *username,
                                       const gchar *password,
                                       gpointer user_data)
{
    (void) interface;
    DrdHandoverDaemon *self = user_data;
    DRD_LOG_MESSAGE("RedirectClient received (token=%s) for %s",
                    routing_token != NULL ? routing_token : "unknown",
                    self->handover_object_path);

    if (!drd_handover_daemon_redirect_active_client(self, routing_token, username, password))
    {
        DRD_LOG_WARNING("Failed to redirect current client for %s", self->handover_object_path);
        return;
    }

    drd_handover_daemon_stop(self);
    drd_handover_daemon_request_shutdown(self);
}

/*
 * 功能：处理 TakeClientReady 信号，领取客户端连接。
 * 逻辑：尝试调用 take_client，失败时记录警告。
 * 参数：interface handover 接口；use_system_credentials 标志（当前未用）；user_data handover 守护实例。
 * 外部接口：内部 drd_handover_daemon_take_client；日志 DRD_LOG_WARNING。
 */
static void
drd_handover_daemon_on_take_client_ready(DrdDBusRemoteDesktopRdpHandover *interface,
                                         gboolean use_system_credentials,
                                         gpointer user_data)
{
    (void) interface;
    (void) use_system_credentials;
    DrdHandoverDaemon *self = user_data;

    g_autoptr(GError)
    error = NULL;
    if (!drd_handover_daemon_take_client(self, &error) && error != NULL)
    {
        DRD_LOG_WARNING("Failed to take client: %s", error->message);
    }
}

/*
 * 功能：处理 RestartHandover 信号。
 * 逻辑：当前仅记录收到事件，未来可用于重新协商。
 * 参数：interface handover 接口；user_data handover 守护实例。
 * 外部接口：日志 DRD_LOG_MESSAGE。
 */
static void
drd_handover_daemon_on_restart_handover(DrdDBusRemoteDesktopRdpHandover *interface,
                                        gpointer user_data)
{
    (void) interface;
    DrdHandoverDaemon *self = user_data;
    DRD_LOG_MESSAGE("RestartHandover received for %s", self->handover_object_path);
}

/*
 * 功能：监听器回调，记录当前活跃 RDP 会话。
 * 逻辑：替换 active_session 引用，确保为 handover 模式的最新会话。
 * 参数：listener 监听器；session 新会话；user_data handover 守护实例。
 * 外部接口：GLib g_clear_object/g_object_ref；日志 DRD_LOG_MESSAGE。
 */
static void
drd_handover_daemon_on_session_ready(DrdRdpListener *listener,
                                     DrdRdpSession *session,
                                     gpointer user_data)
{
    (void) listener;
    DrdHandoverDaemon *self = DRD_HANDOVER_DAEMON(user_data);
    if (!DRD_IS_HANDOVER_DAEMON(self))
    {
        return;
    }
    DRD_LOG_MESSAGE("handover on session ready");
    g_clear_object(&self->active_session);
    if (DRD_IS_RDP_SESSION(session))
    {
        self->active_session = g_object_ref(session);
    }
}

/*
 * 功能：在活跃会话上发送 Server Redirection。
 * 逻辑：校验参数与 TLS 凭据；读取证书 PEM；调用会话接口发送重定向并发出错误通知，然后清理 active_session。
 * 参数：self 守护实例；routing_token 路由 token；username/password 目标凭据。
 * 外部接口：drd_tls_credentials_read_material、drd_rdp_session_send_server_redirection、drd_rdp_session_notify_error；日志 DRD_LOG_WARNING。
 */
static gboolean
drd_handover_daemon_redirect_active_client(DrdHandoverDaemon *self,
                                           const gchar *routing_token,
                                           const gchar *username,
                                           const gchar *password)
{
    if (!DRD_IS_HANDOVER_DAEMON(self) || self->active_session == NULL)
    {
        return FALSE;
    }

    if (routing_token == NULL || username == NULL || password == NULL)
    {
        DRD_LOG_WARNING("RedirectClient missing routing data, ignoring");
        return FALSE;
    }

    if (self->tls_credentials == NULL)
    {
        DRD_LOG_WARNING("TLS credentials unavailable, cannot send server redirection");
        return FALSE;
    }

    g_autofree gchar *certificate = NULL;
    g_autoptr(GError)
    tls_error = NULL;
    if (!drd_tls_credentials_read_material(self->tls_credentials, &certificate, NULL, &tls_error))
    {
        if (tls_error != NULL)
        {
            DRD_LOG_WARNING("Failed to read TLS credential for redirect: %s", tls_error->message);
        }
        return FALSE;
    }

    if (!drd_rdp_session_send_server_redirection(self->active_session,
                                                 routing_token,
                                                 username,
                                                 password,
                                                 certificate))
    {
        DRD_LOG_WARNING("Active session failed to send server redirection");
        return FALSE;
    }

    drd_rdp_session_notify_error(self->active_session, DRD_RDP_SESSION_ERROR_SERVER_REDIRECTION);
    g_clear_object(&self->active_session);

    /* 会话线程会在 notify_error 之后关闭底层连接，此处无需再操作 socket。 */
    return TRUE;
}

/*
 * 功能：请求主循环退出。
 * 逻辑：若主循环运行中则记录日志并退出。
 * 参数：self 守护实例。
 * 外部接口：GLib g_main_loop_is_running/g_main_loop_quit；日志 DRD_LOG_MESSAGE。
 */
static void
drd_handover_daemon_request_shutdown(DrdHandoverDaemon *self)
{
    g_return_if_fail(DRD_IS_HANDOVER_DAEMON(self));

    if (self->main_loop != NULL && g_main_loop_is_running(self->main_loop))
    {
        DRD_LOG_MESSAGE("Handover daemon exiting after redirect");
        g_main_loop_quit(self->main_loop);
    }
}

/*
 * 功能：设置/替换 handover 守护使用的主循环引用。
 * 逻辑：释放旧引用后为新主循环增加引用，允许传入 NULL。
 * 参数：self 守护实例；loop 主循环。
 * 外部接口：GLib g_main_loop_ref/g_main_loop_unref。
 */
gboolean
drd_handover_daemon_set_main_loop(DrdHandoverDaemon *self, GMainLoop *loop)
{
    g_return_val_if_fail(DRD_IS_HANDOVER_DAEMON(self), FALSE);

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

/*
 * 功能：停止 handover 守护的 DBus/监听器资源。
 * 逻辑：记录日志后清理 dispatcher/handover 代理、object path 及监听器。
 * 参数：self 守护实例。
 * 外部接口：GLib g_clear_object/g_clear_pointer；日志 DRD_LOG_MESSAGE。
 */
void
drd_handover_daemon_stop(DrdHandoverDaemon *self)
{
    DRD_LOG_MESSAGE("drd_handover_daemon_stop");
    g_return_if_fail(DRD_IS_HANDOVER_DAEMON(self));

    g_clear_object(&self->dispatcher_proxy);
    g_clear_object(&self->handover_proxy);
    g_clear_pointer(&self->handover_object_path, g_free);
    g_clear_object(&self->listener);
}

/*
 * 功能：启动 handover 守护，连接 dispatcher 并准备监听器。
 * 逻辑：创建 dispatcher 代理；若无 NLA 凭据则生成；创建 handover 监听器并设置 session 回调；绑定 handover 对象并与 dispatcher 交换 TLS/NLA；成功后记录日志。
 * 参数：self 守护实例；error 错误输出。
 * 外部接口：GDBus 代理工厂 drd_dbus_remote_desktop_rdp_dispatcher_proxy_new_for_bus_sync；drd_config_get_encoding_options 等配置接口；drd_rdp_listener_new/set_session_callback；内部 drd_handover_daemon_bind_handover/drd_handover_daemon_start_session；日志 DRD_LOG_MESSAGE。
 */
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

    if (self->nla_username == NULL || self->nla_password == NULL)
    {
        if (!drd_handover_daemon_refresh_nla_credentials(self, error))
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
                                     self->nla_username,
                                     self->nla_password,
                                     drd_config_get_pam_service(self->config),
                                     DRD_RUNTIME_MODE_HANDOVER);
        if (self->listener == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to create handover listener");
            return FALSE;
        }

        drd_rdp_listener_set_session_callback(self->listener,
                                              drd_handover_daemon_on_session_ready,
                                              self);
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
