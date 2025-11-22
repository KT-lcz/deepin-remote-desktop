#include "core/drd_application.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <winpr/ssl.h>
#include <unistd.h>

#include "transport/drd_rdp_listener.h"
#include "security/drd_tls_credentials.h"
#include "core/drd_config.h"
#include "core/drd_server_runtime.h"
#include "system/drd_system_daemon.h"
#include "system/drd_handover_daemon.h"
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
};

G_DEFINE_TYPE(DrdApplication, drd_application, G_TYPE_OBJECT)

/* 将编码模式转换成便于日志输出的字符串。 */
static const gchar *
drd_application_mode_to_string(DrdEncodingMode mode)
{
    switch (mode)
    {
        case DRD_ENCODING_MODE_RAW:
            return "raw";
        case DRD_ENCODING_MODE_RFX:
            return "rfx";
        default:
            return "unknown";
    }
}

static const gchar *
drd_application_runtime_mode_to_string(DrdRuntimeMode mode)
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
    const DrdEncodingOptions *encoding_opts;
    gboolean nla_enabled;
    const gchar *nla_username;
    const gchar *nla_password;
    const gchar *pam_service;
    DrdRuntimeMode runtime_mode;
} DrdRuntimeContextSnapshot;

/* 记录当前合并后的核心运行参数，帮助排查配置生效情况。 */
static void
drd_application_log_effective_config(DrdApplication *self)
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

    DRD_LOG_MESSAGE("Effective capture geometry %ux%u, encoder=%s, frame diff %s",
              encoding_opts->width,
              encoding_opts->height,
              drd_application_mode_to_string(encoding_opts->mode),
              encoding_opts->enable_frame_diff ? "enabled" : "disabled");

    const DrdRuntimeMode runtime_mode = drd_config_get_runtime_mode(self->config);
    DRD_LOG_MESSAGE("Effective NLA %s, runtime=%s, PAM service=%s",
              drd_config_is_nla_enabled(self->config) ? "enabled" : "disabled",
              drd_application_runtime_mode_to_string(runtime_mode),
              drd_config_get_pam_service(self->config));
}

/* 释放主循环、监听器等运行期资源，确保干净退出。 */
static void
drd_application_dispose(GObject *object)
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

/* 最终清理阶段，释放剩余引用。 */
static void
drd_application_finalize(GObject *object)
{
    DrdApplication *self = DRD_APPLICATION(object);
    g_clear_object(&self->config);
    G_OBJECT_CLASS(drd_application_parent_class)->finalize(object);
}

/* 信号回调：收到终止信号时请求主循环退出。 */
static gboolean
drd_application_on_signal(gpointer user_data)
{
    DrdApplication *self = DRD_APPLICATION(user_data);

    if (self->loop != NULL && g_main_loop_is_running(self->loop))
    {
        DRD_LOG_MESSAGE("Termination signal received, shutting down main loop");
        g_main_loop_quit(self->loop);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
drd_application_prepare_runtime(DrdApplication *self,
                                gboolean require_stream,
                                DrdRuntimeContextSnapshot *snapshot,
                                GError **error)
{
    g_return_val_if_fail(DRD_IS_APPLICATION(self), FALSE);

    if (self->config == NULL)
    {
        self->config = drd_config_new();
    }

    const gchar *cert_path = drd_config_get_certificate_path(self->config);
    const gchar *key_path = drd_config_get_private_key_path(self->config);
    if (cert_path == NULL || key_path == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "TLS certificate or key path missing after config merge");
        return FALSE;
    }

    const gboolean nla_enabled = drd_config_is_nla_enabled(self->config);
    const gchar *nla_username = drd_config_get_nla_username(self->config);
    const gchar *nla_password = drd_config_get_nla_password(self->config);
    const gchar *pam_service = drd_config_get_pam_service(self->config);
    const DrdRuntimeMode runtime_mode = drd_config_get_runtime_mode(self->config);

    if (nla_enabled)
    {
        if (nla_username == NULL || nla_password == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "NLA username/password missing after config merge");
            return FALSE;
        }
    }
    else
    {
        if (runtime_mode != DRD_RUNTIME_MODE_SYSTEM)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "Disabling NLA requires --system");
            return FALSE;
        }
        if (pam_service == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "PAM service missing for TLS authentication");
            return FALSE;
        }
    }

    if (self->tls_credentials == NULL)
    {
        self->tls_credentials = drd_tls_credentials_new(cert_path, key_path, error);
        if (self->tls_credentials == NULL)
        {
            return FALSE;
        }
        drd_server_runtime_set_tls_credentials(self->runtime, self->tls_credentials);
    }

    const DrdEncodingOptions *encoding_opts = drd_config_get_encoding_options(self->config);
    if (encoding_opts == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Encoding options unavailable after config merge");
        return FALSE;
    }

    if (require_stream)
    {
        if (!drd_server_runtime_prepare_stream(self->runtime, encoding_opts, error))
        {
            return FALSE;
        }
    }
    else
    {
        DRD_LOG_MESSAGE("Runtime initialized without capture/encoding setup "
                        "(runtime mode=%s)",
                        drd_application_runtime_mode_to_string(runtime_mode));
    }

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

/* 启动 RDP 监听器，并将 TLS、编码运行时串联起来。 */
static gboolean
drd_application_start_listener(DrdApplication *self, GError **error)
{
    g_assert(self->listener == NULL);
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self->runtime), FALSE);

    DrdRuntimeContextSnapshot snapshot = {0};
    if (!drd_application_prepare_runtime(self, TRUE, &snapshot, error))
    {
        return FALSE;
    }

    self->listener = drd_rdp_listener_new(drd_config_get_bind_address(self->config),
                                          drd_config_get_port(self->config),
                                          self->runtime,
                                          snapshot.encoding_opts,
                                          snapshot.nla_enabled,
                                          snapshot.nla_username,
                                          snapshot.nla_password,
                                          snapshot.pam_service,
                                          snapshot.runtime_mode == DRD_RUNTIME_MODE_SYSTEM);
    if (self->listener == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to instantiate RDP listener");
        drd_server_runtime_stop(self->runtime);
        return FALSE;
    }

    if (!drd_rdp_listener_start(self->listener, error))
    {
        g_clear_object(&self->listener);
        drd_server_runtime_stop(self->runtime);
        return FALSE;
    }

    return TRUE;
}

static gboolean
drd_application_start_system_daemon(DrdApplication *self, GError **error)
{
    DrdRuntimeContextSnapshot snapshot = {0};
    if (!drd_application_prepare_runtime(self, FALSE, &snapshot, error))
    {
        return FALSE;
    }

    g_clear_object(&self->mode_controller);
    self->mode_controller = G_OBJECT(drd_system_daemon_new(self->config,
                                                           self->runtime,
                                                           self->tls_credentials));
    if (self->mode_controller == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to allocate system daemon controller");
        return FALSE;
    }

    DrdSystemDaemon *system_daemon = DRD_SYSTEM_DAEMON(self->mode_controller);
    if (!drd_system_daemon_set_main_loop(system_daemon, self->loop))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to attach main loop to system daemon");
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

static gboolean
drd_application_start_handover_daemon(DrdApplication *self, GError **error)
{
    DrdRuntimeContextSnapshot snapshot = {0};
    if (!drd_application_prepare_runtime(self, TRUE, &snapshot, error))
    {
        return FALSE;
    }

    g_clear_object(&self->mode_controller);
    self->mode_controller = G_OBJECT(drd_handover_daemon_new(self->config,
                                                             self->runtime,
                                                             self->tls_credentials));
    if (self->mode_controller == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to allocate handover daemon controller");
        return FALSE;
    }

    DrdHandoverDaemon *handover_daemon = DRD_HANDOVER_DAEMON(self->mode_controller);
    if (!drd_handover_daemon_set_main_loop(handover_daemon, self->loop))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to attach main loop to handover daemon");
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

/* 解析 CLI 选项，并与配置文件合并。 */
static gboolean
drd_application_parse_options(DrdApplication *self, gint *argc, gchar ***argv, GError **error)
{
    gchar *bind_address = NULL;
    gint port = 0;
    gchar *cert_path = NULL;
    gchar *key_path = NULL;
    gchar *config_path = NULL;
    gint capture_width = 0;
    gint capture_height = 0;
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
        {"encoder", 0, 0, G_OPTION_ARG_STRING, &encoder_mode, "Encoder mode (raw|rfx)", "MODE"},
        {"nla-username", 0, 0, G_OPTION_ARG_STRING, &nla_username, "NLA username for static mode", "USER"},
        {"nla-password", 0, 0, G_OPTION_ARG_STRING, &nla_password, "NLA password for static mode", "PASS"},
        {"enable-nla", 0, 0, G_OPTION_ARG_NONE, &enable_nla_flag, "Force enable NLA regardless of config", NULL},
        {"disable-nla", 0, 0, G_OPTION_ARG_NONE, &disable_nla_flag, "Disable NLA and use TLS+PAM single sign-on (system mode only)", NULL},
        {"mode", 0, 0, G_OPTION_ARG_STRING, &runtime_mode_name, "Runtime mode (user|system|handover)", "MODE"},
        {"enable-diff", 0, 0, G_OPTION_ARG_NONE, &enable_diff_flag, "Enable frame difference even if disabled in config", NULL},
        {"disable-diff", 0, 0, G_OPTION_ARG_NONE, &disable_diff_flag, "Disable frame difference regardless of config", NULL},
        {NULL}
    };

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
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
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
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
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

    if (!drd_config_merge_cli(self->config,
                               bind_address,
                               port,
                               cert_path,
                               key_path,
                               nla_username,
                               nla_password,
                               enable_nla_flag,
                               disable_nla_flag,
                               runtime_mode_name,
                               capture_width,
                               capture_height,
                               encoder_mode,
                               diff_override,
                               error))
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
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "--system requires root privileges");
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

/* 初始化对象默认值。 */
static void
drd_application_init(DrdApplication *self)
{
    self->config = drd_config_new();
    self->runtime = drd_server_runtime_new();
    self->tls_credentials = NULL;
    drd_log_init();

    if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
    {
        DRD_LOG_WARNING("Failed to initialize WinPR SSL context, NTLM may be unavailable");
    }
}

/* 绑定类虚函数。 */
static void
drd_application_class_init(DrdApplicationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_application_dispose;
    object_class->finalize = drd_application_finalize;
}

/* 对外构造函数。 */
DrdApplication *
drd_application_new(void)
{
    return g_object_new(DRD_TYPE_APPLICATION, NULL);
}

/* 应用入口：解析参数、启动监听、运行主循环。 */
int
drd_application_run(DrdApplication *self, int argc, char **argv, GError **error)
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
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to create GMainLoop");
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
                DRD_LOG_MESSAGE("RDP service listening on %s:%u",
                                drd_config_get_bind_address(self->config),
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
