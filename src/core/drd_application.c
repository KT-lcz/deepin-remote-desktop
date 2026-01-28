#include "core/drd_application.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>
#include <winpr/ssl.h>

#include "core/drd_config.h"
#include "core/drd_server_runtime.h"
#include "core/drd_user_dbus_service.h"
#include "security/drd_tls_credentials.h"
#include "system/drd_handover_daemon.h"
#include "system/drd_system_daemon.h"
#include "transport/drd_rdp_listener.h"
#include "utils/drd_capture_metrics.h"
#include "utils/drd_log.h"

struct _DrdApplication
{
    GObject parent_instance;

    DrdConfig *config;
    GMainLoop *loop;
    DrdRdpListener *listener;
    guint sigint_id;
    guint sigterm_id;
    DrdServerRuntime *runtime;
    DrdTlsCredentials *tls_credentials;
    GObject *mode_controller;
    gboolean is_handover;
};

G_DEFINE_TYPE(DrdApplication, drd_application, G_TYPE_OBJECT)

/*
 * 功能：将运行模式转为字符串。
 * 逻辑：根据枚举返回 system/handover/user 文本，默认 user。
 * 参数：mode 运行模式。
 * 外部接口：无额外外部库调用。
 */
static const gchar *drd_application_runtime_mode_to_string(DrdRuntimeMode mode)
{
    switch (mode)
    {
        case DRD_RUNTIME_MODE_SYSTEM:
            return "system";
        case DRD_RUNTIME_MODE_HANDOVER:
            return "handover";
        case DRD_RUNTIME_MODE_USER:
        default:
            return "user";
    }
}

typedef struct
{
    DrdEncodingOptions encoding_opts;
    gboolean nla_enabled;
    const gchar *nla_username;
    const gchar *nla_password;
    const gchar *pam_service;
    DrdRuntimeMode runtime_mode;
} DrdRuntimeContextSnapshot;

/*
 * 功能：输出当前生效的编码与运行模式配置。
 * 逻辑：读取配置中的编码选项与运行模式，分别记录分辨率、编码模式、差分开关、NLA 与 PAM 服务名。
 * 参数：self 应用实例。
 * 外部接口：依赖 drd_config_* 获取配置，日志通过 DRD_LOG_MESSAGE。
 */
static void drd_application_log_effective_config(DrdApplication *self)
{
    if (self->config == NULL)
    {
        return;
    }

    const DrdEncodingOptions *encoding_opts = drd_config_get_encoding_options(self->config);
    if (encoding_opts == NULL)
    {
        return;
    }

    DRD_LOG_MESSAGE("Effective capture geometry %ux%u, encoder=%s, frame diff %s", encoding_opts->width,
                    encoding_opts->height, drd_encoding_mode_to_string(encoding_opts->mode),
                    encoding_opts->enable_frame_diff ? "enabled" : "disabled");

    const DrdRuntimeMode runtime_mode = drd_config_get_runtime_mode(self->config);
    DRD_LOG_MESSAGE("Effective NLA %s, runtime=%s, PAM service=%s",
                    drd_config_is_nla_enabled(self->config) ? "enabled" : "disabled",
                    drd_application_runtime_mode_to_string(runtime_mode), drd_config_get_pam_service(self->config));
}

/*
 * 功能：释放应用持有的运行期资源。
 * 逻辑：移除信号源；停止监听器与运行时；释放模式控制器、TLS 凭据与配置对象；释放主循环引用；最后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdApplication。
 * 外部接口：GLib g_source_remove/g_clear_object/g_clear_pointer，调用 drd_rdp_listener_stop、drd_server_runtime_stop
 * 关闭子模块。
 */
static void drd_application_dispose(GObject *object)
{
    DrdApplication *self = DRD_APPLICATION(object);

    if (self->sigint_id != 0)
    {
        g_source_remove(self->sigint_id);
        self->sigint_id = 0;
    }

    if (self->sigterm_id != 0)
    {
        g_source_remove(self->sigterm_id);
        self->sigterm_id = 0;
    }

    if (self->listener != NULL)
    {
        drd_rdp_listener_stop(self->listener);
        g_clear_object(&self->listener);
    }

    if (self->runtime != NULL)
    {
        drd_server_runtime_stop(self->runtime);
        g_clear_object(&self->runtime);
    }

    if (self->mode_controller != NULL)
    {
        g_clear_object(&self->mode_controller);
    }

    g_clear_object(&self->tls_credentials);
    g_clear_object(&self->config);

    g_clear_pointer(&self->loop, g_main_loop_unref);

    G_OBJECT_CLASS(drd_application_parent_class)->dispose(object);
}

/*
 * 功能：最终清理阶段释放残余引用。
 * 逻辑：清理配置对象引用后调用父类 finalize。
 * 参数：object 基类指针。
 * 外部接口：GLib g_clear_object，GObjectClass::finalize。
 */
static void drd_application_finalize(GObject *object)
{
    DrdApplication *self = DRD_APPLICATION(object);
    g_clear_object(&self->config);
    G_OBJECT_CLASS(drd_application_parent_class)->finalize(object);
}

/*
 * 功能：异步退出主循环的辅助函数。
 * 逻辑：调用 g_main_loop_quit 并移除超时源。
 * 参数：user_data 主循环指针。
 * 外部接口：GLib g_main_loop_quit。
 */
static gboolean quit_loop(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

/*
 * 功能：Unix 信号回调，触发主循环退出。
 * 逻辑：若主循环运行中且非 handover 模式则立刻退出；handover 模式下延迟 5 秒退出并记录日志。
 * 参数：user_data 应用实例。
 * 外部接口：GLib g_main_loop_is_running/g_timeout_add，DRD_LOG_MESSAGE；依赖 g_unix_signal_add 注册的回调。
 */
static gboolean drd_application_on_signal(gpointer user_data)
{
    DrdApplication *self = DRD_APPLICATION(user_data);
    if (self->loop != NULL && g_main_loop_is_running(self->loop))
    {
        if (!self->is_handover || getuid() >= 1000)
        {
            g_main_loop_quit(self->loop);
            return G_SOURCE_CONTINUE;
        }
        DRD_LOG_MESSAGE("Termination signal received, shutting down main loop");
        g_timeout_add(5 * 1000, quit_loop, self->loop);
    }

    return G_SOURCE_CONTINUE;
}

/*
 * 功能：准备运行时依赖（配置、TLS、编码选项）并可选记录快照。
 * 逻辑：加载/创建配置；校验 TLS 路径与 NLA/PAM 账户；按模式创建或加载 TLS 凭据并注入 runtime；提取编码选项写入
 * runtime；填充 snapshot 供后续创建监听器或守护进程。 参数：self 应用实例；snapshot 可选输出的上下文快照；error
 * 错误输出。 外部接口：调用 drd_config_* 读取配置、drd_tls_credentials_new/drd_tls_credentials_new_empty 加载
 * TLS，drd_server_runtime_set_tls_credentials/set_encoding_options 注入运行时；错误通过 GLib g_set_error_literal。
 */
static gboolean drd_application_prepare_runtime(DrdApplication *self, DrdRuntimeContextSnapshot *snapshot,
                                                GError **error)
{
    g_return_val_if_fail(DRD_IS_APPLICATION(self), FALSE);

    if (self->config == NULL)
    {
        self->config = drd_config_new();
    }

    const gboolean nla_enabled = drd_config_is_nla_enabled(self->config);
    const gchar *nla_username = drd_config_get_nla_username(self->config);
    const gchar *nla_password = drd_config_get_nla_password(self->config);
    const gchar *pam_service = drd_config_get_pam_service(self->config);
    const DrdRuntimeMode runtime_mode = drd_config_get_runtime_mode(self->config);

    const gboolean require_tls_paths = runtime_mode != DRD_RUNTIME_MODE_HANDOVER;
    const gchar *cert_path = NULL;
    const gchar *key_path = NULL;
    if (require_tls_paths)
    {
        cert_path = drd_config_get_certificate_path(self->config);
        key_path = drd_config_get_private_key_path(self->config);
        if (cert_path == NULL || key_path == NULL)
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "TLS certificate or key path missing after config merge");
            return FALSE;
        }
    }

    if (nla_enabled && runtime_mode != DRD_RUNTIME_MODE_HANDOVER && (nla_username == NULL || nla_password == NULL))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "NLA username/password missing after config merge");
        return FALSE;
    }

    if (!nla_enabled && pam_service == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "PAM service missing for TLS authentication");
        return FALSE;
    }

    if (self->tls_credentials == NULL)
    {
        if (runtime_mode == DRD_RUNTIME_MODE_HANDOVER)
        {
            self->tls_credentials = drd_tls_credentials_new_empty();
        }
        else
        {
            self->tls_credentials = drd_tls_credentials_new(cert_path, key_path, error);
        }
        if (self->tls_credentials == NULL)
        {
            if (error != NULL && *error == NULL && require_tls_paths)
            {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to load TLS credentials");
            }
            return FALSE;
        }
        drd_server_runtime_set_tls_credentials(self->runtime, self->tls_credentials);
    }

    const DrdEncodingOptions *config_encoding_opts = drd_config_get_encoding_options(self->config);
    if (config_encoding_opts == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Encoding options unavailable after config merge");
        return FALSE;
    }

    DrdEncodingOptions encoding_opts = *config_encoding_opts;
    if (runtime_mode != DRD_RUNTIME_MODE_SYSTEM)
    {
        guint display_width = 0;
        guint display_height = 0;
        DrdCaptureManager *capture = drd_server_runtime_get_capture(self->runtime);
        if (capture == NULL || !drd_capture_manager_get_display_size(capture, &display_width, &display_height, error))
        {
            if (error != NULL && *error == NULL)
            {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to query current display resolution");
            }
            return FALSE;
        }
        encoding_opts.width = display_width;
        encoding_opts.height = display_height;
    }

    drd_server_runtime_set_encoding_options(self->runtime, &encoding_opts);

    DRD_LOG_MESSAGE("Runtime initialized without capture/encoding setup "
                    "(runtime mode=%s, awaiting session activation)",
                    drd_application_runtime_mode_to_string(runtime_mode));

    if (snapshot != NULL)
    {
        snapshot->encoding_opts = encoding_opts;
        snapshot->nla_enabled = nla_enabled;
        snapshot->nla_username = nla_username;
        snapshot->nla_password = nla_password;
        snapshot->pam_service = pam_service;
        snapshot->runtime_mode = runtime_mode;
    }

    return TRUE;
}

/*
 * 功能：在 user 模式下启动 RDP 监听器。
 * 逻辑：准备运行时上下文；创建监听器并绑定编码/NLA/TLS 参数；启动监听失败则清理 runtime。
 * 参数：self 应用实例；error 错误输出。
 * 外部接口：drd_application_prepare_runtime、drd_rdp_listener_new/drd_rdp_listener_start 进行 FreeRDP
 * 监听，drd_server_runtime_stop 关闭运行时。
 */
static gboolean drd_application_start_listener(DrdApplication *self, GError **error)
{
    g_assert(self->listener == NULL);
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self->runtime), FALSE);

    DrdRuntimeContextSnapshot snapshot = {0};
    if (!drd_application_prepare_runtime(self, &snapshot, error))
    {
        return FALSE;
    }

    self->listener =
            drd_rdp_listener_new(drd_config_get_bind_address(self->config), drd_config_get_port(self->config),
                                 self->runtime, &snapshot.encoding_opts, snapshot.nla_enabled, snapshot.nla_username,
                                 snapshot.nla_password, snapshot.pam_service, snapshot.runtime_mode);
    if (self->listener == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to instantiate RDP listener");
        drd_server_runtime_stop(self->runtime);
        return FALSE;
    }

    if (!drd_rdp_listener_start(self->listener, error))
    {
        g_clear_object(&self->listener);
        drd_server_runtime_stop(self->runtime);
        return FALSE;
    }

    g_clear_object(&self->mode_controller);
    self->mode_controller = G_OBJECT(drd_user_dbus_service_new(self->config));
    if (self->mode_controller == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate user DBus service");
        g_clear_object(&self->listener);
        drd_server_runtime_stop(self->runtime);
        return FALSE;
    }

    if (!drd_user_dbus_service_start(DRD_USER_DBUS_SERVICE(self->mode_controller), error))
    {
        g_clear_object(&self->mode_controller);
        g_clear_object(&self->listener);
        drd_server_runtime_stop(self->runtime);
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：在 system 模式下启动守护进程控制器。
 * 逻辑：准备运行时上下文；创建 system 守护并绑定主循环；启动失败则清理控制器。
 * 参数：self 应用实例；error 错误输出。
 * 外部接口：drd_system_daemon_new/drd_system_daemon_set_main_loop/drd_system_daemon_start，GLib
 * g_clear_object；错误通过 g_set_error_literal。
 */
static gboolean drd_application_start_system_daemon(DrdApplication *self, GError **error)
{
    DrdRuntimeContextSnapshot snapshot = {0};
    if (!drd_application_prepare_runtime(self, &snapshot, error))
    {
        return FALSE;
    }

    g_clear_object(&self->mode_controller);
    self->mode_controller = G_OBJECT(drd_system_daemon_new(self->config, self->runtime, self->tls_credentials));
    if (self->mode_controller == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate system daemon controller");
        return FALSE;
    }

    DrdSystemDaemon *system_daemon = DRD_SYSTEM_DAEMON(self->mode_controller);
    if (!drd_system_daemon_set_main_loop(system_daemon, self->loop))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to attach main loop to system daemon");
        g_clear_object(&self->mode_controller);
        return FALSE;
    }

    if (!drd_system_daemon_start(system_daemon, error))
    {
        g_clear_object(&self->mode_controller);
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：在 handover 模式下启动交接守护进程。
 * 逻辑：准备运行时上下文；创建 handover 守护并挂接主循环；启动失败则释放控制器。
 * 参数：self 应用实例；error 错误输出。
 * 外部接口：drd_handover_daemon_new/drd_handover_daemon_set_main_loop/drd_handover_daemon_start，GLib
 * g_clear_object；错误通过 g_set_error_literal。
 */
static gboolean drd_application_start_handover_daemon(DrdApplication *self, GError **error)
{
    DrdRuntimeContextSnapshot snapshot = {0};
    if (!drd_application_prepare_runtime(self, &snapshot, error))
    {
        return FALSE;
    }

    g_clear_object(&self->mode_controller);
    self->mode_controller = G_OBJECT(drd_handover_daemon_new(self->config, self->runtime, self->tls_credentials));
    if (self->mode_controller == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate handover daemon controller");
        return FALSE;
    }

    DrdHandoverDaemon *handover_daemon = DRD_HANDOVER_DAEMON(self->mode_controller);
    if (!drd_handover_daemon_set_main_loop(handover_daemon, self->loop))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to attach main loop to handover daemon");
        g_clear_object(&self->mode_controller);
        return FALSE;
    }

    if (!drd_handover_daemon_start(handover_daemon, error))
    {
        g_clear_object(&self->mode_controller);
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：解析命令行参数并与配置文件合并。
 * 逻辑：构建 GOptionEntry 解析 CLI；防止互斥选项组合；加载配置文件或使用默认配置；按 CLI 覆盖配置项并校验权限（system
 * 模式需 root）。 参数：self 应用实例；argc/argv 命令行参数；error 错误输出。 外部接口：GLib GOptionContext
 * 解析选项；drd_config_new_from_file/drd_config_merge_cli 读取与合并；geteuid 校验权限；日志 DRD_LOG_MESSAGE。
 */
static gboolean drd_application_parse_options(DrdApplication *self, gint *argc, gchar ***argv, GError **error)
{
    gchar *bind_address = NULL;
    gint port = 0;
    gchar *cert_path = NULL;
    gchar *key_path = NULL;
    gchar *config_path = NULL;
    gint capture_width = 0;
    gint capture_height = 0;
    gint capture_target_fps = 0;
    gint capture_stats_interval_sec = 0;
    gchar *encoder_mode = NULL;
    gboolean enable_diff_flag = FALSE;
    gboolean disable_diff_flag = FALSE;
    gchar *nla_username = NULL;
    gchar *nla_password = NULL;
    gboolean enable_nla_flag = FALSE;
    gboolean disable_nla_flag = FALSE;
    gchar *runtime_mode_name = NULL;

    GOptionEntry entries[] = {
            {"bind-address", 'b', 0, G_OPTION_ARG_STRING, &bind_address, "Bind address (default 0.0.0.0)", "ADDR"},
            {"port", 'p', 0, G_OPTION_ARG_INT, &port, "Bind port (default 3390 unless config overrides)", "PORT"},
            {"cert", 0, 0, G_OPTION_ARG_STRING, &cert_path, "TLS certificate PEM path", "FILE"},
            {"key", 0, 0, G_OPTION_ARG_STRING, &key_path, "TLS private key PEM path", "FILE"},
            {"config", 'c', 0, G_OPTION_ARG_STRING, &config_path, "Configuration file path (ini)", "FILE"},
            {"width", 0, 0, G_OPTION_ARG_INT, &capture_width, "Capture width override", "PX"},
            {"height", 0, 0, G_OPTION_ARG_INT, &capture_height, "Capture height override", "PX"},
            {"capture-fps", 0, 0, G_OPTION_ARG_INT, &capture_target_fps, "Capture target fps", "FPS"},
            {"capture-stats-sec", 0, 0, G_OPTION_ARG_INT, &capture_stats_interval_sec,
             "Capture/render fps stats window seconds", "SEC"},
            {"encoder", 0, 0, G_OPTION_ARG_STRING, &encoder_mode, "Encoder mode (h264|rfx|auto)", "MODE"},
            {"nla-username", 0, 0, G_OPTION_ARG_STRING, &nla_username, "NLA username for static mode", "USER"},
            {"nla-password", 0, 0, G_OPTION_ARG_STRING, &nla_password, "NLA password for static mode", "PASS"},
            {"enable-nla", 0, 0, G_OPTION_ARG_NONE, &enable_nla_flag, "Force enable NLA regardless of config", NULL},
            {"disable-nla", 0, 0, G_OPTION_ARG_NONE, &disable_nla_flag,
             "Disable NLA and use TLS+PAM single sign-on (system mode only)", NULL},
            {"mode", 0, 0, G_OPTION_ARG_STRING, &runtime_mode_name, "Runtime mode (user|system|handover)", "MODE"},
            {"enable-diff", 0, 0, G_OPTION_ARG_NONE, &enable_diff_flag,
             "Enable frame difference even if disabled in config", NULL},
            {"disable-diff", 0, 0, G_OPTION_ARG_NONE, &disable_diff_flag,
             "Disable frame difference regardless of config", NULL},
            {NULL, 0, 0, 0, NULL, NULL, NULL}};

    g_autoptr(GOptionContext) context = g_option_context_new("- GLib FreeRDP minimal server skeleton");
    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse(context, argc, argv, error))
    {
        g_clear_pointer(&bind_address, g_free);
        g_clear_pointer(&cert_path, g_free);
        g_clear_pointer(&key_path, g_free);
        g_clear_pointer(&config_path, g_free);
        g_clear_pointer(&encoder_mode, g_free);
        g_clear_pointer(&runtime_mode_name, g_free);
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        return FALSE;
    }

    if (enable_diff_flag && disable_diff_flag)
    {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                            "--enable-diff and --disable-diff cannot be used together");
        g_clear_pointer(&bind_address, g_free);
        g_clear_pointer(&cert_path, g_free);
        g_clear_pointer(&key_path, g_free);
        g_clear_pointer(&config_path, g_free);
        g_clear_pointer(&encoder_mode, g_free);
        g_clear_pointer(&runtime_mode_name, g_free);
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        return FALSE;
    }

    if (enable_nla_flag && disable_nla_flag)
    {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                            "--enable-nla and --disable-nla cannot be used together");
        g_clear_pointer(&bind_address, g_free);
        g_clear_pointer(&cert_path, g_free);
        g_clear_pointer(&key_path, g_free);
        g_clear_pointer(&config_path, g_free);
        g_clear_pointer(&encoder_mode, g_free);
        g_clear_pointer(&runtime_mode_name, g_free);
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        return FALSE;
    }

    if (config_path != NULL)
    {
        g_clear_object(&self->config);
        self->config = drd_config_new_from_file(config_path, error);
        if (self->config == NULL)
        {
            g_clear_pointer(&bind_address, g_free);
            g_clear_pointer(&cert_path, g_free);
            g_clear_pointer(&key_path, g_free);
            g_clear_pointer(&encoder_mode, g_free);
            g_clear_pointer(&runtime_mode_name, g_free);
            g_clear_pointer(&nla_username, g_free);
            g_clear_pointer(&nla_password, g_free);
            return FALSE;
        }
        DRD_LOG_MESSAGE("Configuration loaded from %s", config_path);
    }
    else if (self->config == NULL)
    {
        self->config = drd_config_new();
    }

    gint diff_override = 0;
    if (enable_diff_flag)
    {
        diff_override = 1;
    }
    else if (disable_diff_flag)
    {
        diff_override = -1;
    }

    if (!drd_config_merge_cli(self->config, bind_address, port, cert_path, key_path, nla_username, nla_password,
                              enable_nla_flag, disable_nla_flag, runtime_mode_name, capture_width, capture_height,
                              encoder_mode, diff_override, capture_target_fps, capture_stats_interval_sec, error))
    {
        g_clear_pointer(&bind_address, g_free);
        g_clear_pointer(&cert_path, g_free);
        g_clear_pointer(&key_path, g_free);
        g_clear_pointer(&config_path, g_free);
        g_clear_pointer(&encoder_mode, g_free);
        g_clear_pointer(&runtime_mode_name, g_free);
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        return FALSE;
    }

    if (drd_config_get_runtime_mode(self->config) == DRD_RUNTIME_MODE_SYSTEM && geteuid() != 0)
    {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--system requires root privileges");
        g_clear_pointer(&bind_address, g_free);
        g_clear_pointer(&cert_path, g_free);
        g_clear_pointer(&key_path, g_free);
        g_clear_pointer(&config_path, g_free);
        g_clear_pointer(&encoder_mode, g_free);
        g_clear_pointer(&runtime_mode_name, g_free);
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        return FALSE;
    }

    drd_capture_metrics_apply_config(drd_config_get_capture_target_fps(self->config),
                                     drd_config_get_capture_stats_interval_sec(self->config));

    g_clear_pointer(&bind_address, g_free);
    g_clear_pointer(&cert_path, g_free);
    g_clear_pointer(&key_path, g_free);
    g_clear_pointer(&config_path, g_free);
    g_clear_pointer(&encoder_mode, g_free);
    g_clear_pointer(&runtime_mode_name, g_free);
    g_clear_pointer(&nla_username, g_free);
    g_clear_pointer(&nla_password, g_free);

    return TRUE;
}

/*
 * 功能：初始化应用实例默认状态。
 * 逻辑：创建配置与运行时对象，初始化日志，调用 WinPR 初始化 SSL 支持。
 * 参数：self 应用实例。
 * 外部接口：drd_config_new、drd_server_runtime_new、drd_log_init；WinPR winpr_InitializeSSL 初始化 SSL 堆栈。
 */
static void drd_application_init(DrdApplication *self)
{
    self->config = drd_config_new();
    self->runtime = drd_server_runtime_new();
    self->tls_credentials = NULL;
    self->is_handover = FALSE;
    drd_log_init();

    if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
    {
        DRD_LOG_WARNING("Failed to initialize WinPR SSL context, NTLM may be unavailable");
    }
}

/*
 * 功能：绑定类级别的虚函数。
 * 逻辑：将自定义 dispose/finalize 挂载到 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void drd_application_class_init(DrdApplicationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_application_dispose;
    object_class->finalize = drd_application_finalize;
}

/*
 * 功能：创建应用实例。
 * 逻辑：委托 g_object_new 分配并初始化对象。
 * 参数：无。
 * 外部接口：GLib g_object_new。
 */
DrdApplication *drd_application_new(void) { return g_object_new(DRD_TYPE_APPLICATION, NULL); }

/*
 * 功能：应用入口，负责解析参数、启动相应模式并运行主循环。
 * 逻辑：先解析 CLI；输出生效配置；创建主循环并注册 SIGINT/SIGTERM；按运行模式启动监听器或守护；运行主循环并返回退出码。
 * 参数：self 应用实例；argc/argv 命令行参数；error 错误输出。
 * 外部接口：g_option_context、g_main_loop_new/run、g_unix_signal_add
 * 注册信号；drd_application_start_listener/drd_application_start_system_daemon/drd_application_start_handover_daemon
 * 启动子模块；日志 DRD_LOG_MESSAGE。
 */
int drd_application_run(DrdApplication *self, int argc, char **argv, GError **error)
{
    g_return_val_if_fail(DRD_IS_APPLICATION(self), EXIT_FAILURE);

    if (!drd_application_parse_options(self, &argc, &argv, error))
    {
        return EXIT_FAILURE;
    }

    drd_application_log_effective_config(self);

    self->loop = g_main_loop_new(NULL, FALSE);
    if (self->loop == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create GMainLoop");
        return EXIT_FAILURE;
    }

    self->sigint_id = g_unix_signal_add(SIGINT, drd_application_on_signal, self);
    self->sigterm_id = g_unix_signal_add(SIGTERM, drd_application_on_signal, self);

    const DrdRuntimeMode runtime_mode = drd_config_get_runtime_mode(self->config);
    gboolean started = FALSE;
    switch (runtime_mode)
    {
        case DRD_RUNTIME_MODE_SYSTEM:
            started = drd_application_start_system_daemon(self, error);
            if (started)
            {
                DRD_LOG_MESSAGE("System daemon exposing DBus dispatcher (%s)",
                                drd_application_runtime_mode_to_string(runtime_mode));
            }
            break;
        case DRD_RUNTIME_MODE_HANDOVER:
            self->is_handover = TRUE;
            started = drd_application_start_handover_daemon(self, error);
            if (started)
            {
                DRD_LOG_MESSAGE("Handover daemon initialized (mode=%s)",
                                drd_application_runtime_mode_to_string(runtime_mode));
            }
            break;
        case DRD_RUNTIME_MODE_USER:
        default:
            started = drd_application_start_listener(self, error);
            if (started)
            {
                DRD_LOG_MESSAGE("RDP service listening on %s:%u", drd_config_get_bind_address(self->config),
                                drd_config_get_port(self->config));
                DRD_LOG_MESSAGE("Loaded TLS credentials (cert=%s, key=%s)",
                                drd_config_get_certificate_path(self->config),
                                drd_config_get_private_key_path(self->config));
            }
            break;
    }

    if (!started)
    {
        return EXIT_FAILURE;
    }

    g_main_loop_run(self->loop);

    DRD_LOG_MESSAGE("Main loop terminated");
    return EXIT_SUCCESS;
}
