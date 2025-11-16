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

    const gchar *nla_mode = drd_config_get_nla_mode(self->config) == DRD_NLA_MODE_DELEGATE ? "delegate"
                                                                                           : "static";
    DRD_LOG_MESSAGE("Effective NLA mode %s, system=%s, PAM service=%s",
              nla_mode,
              drd_config_get_system_mode(self->config) ? "true" : "false",
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

/* 启动 RDP 监听器，并将 TLS、编码运行时串联起来。 */
static gboolean
drd_application_start_listener(DrdApplication *self, GError **error)
{
    g_assert(self->listener == NULL);
    g_return_val_if_fail(DRD_IS_SERVER_RUNTIME(self->runtime), FALSE);

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

    const gchar *nla_username = drd_config_get_nla_username(self->config);
    const gchar *nla_password = drd_config_get_nla_password(self->config);
    const DrdNlaMode nla_mode = drd_config_get_nla_mode(self->config);
    const gchar *pam_service = drd_config_get_pam_service(self->config);
    const gboolean system_mode = drd_config_get_system_mode(self->config);
    const gboolean rdp_sso_enabled = drd_config_get_rdp_sso_enabled(self->config);
    if (!rdp_sso_enabled)
    {
        if (nla_mode == DRD_NLA_MODE_STATIC)
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
            if (!system_mode)
            {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_INVALID_ARGUMENT,
                                    "NLA delegate mode requires --system and root privileges");
                return FALSE;
            }
            if (pam_service == NULL)
            {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_INVALID_ARGUMENT,
                                    "PAM service missing for delegate mode");
                return FALSE;
            }
        }
    }
    else
    {
        if (pam_service == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "PAM service missing for RDP SSO");
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

    if (!system_mode)
    {
        if (!drd_server_runtime_prepare_stream(self->runtime, encoding_opts, error))
        {
            return FALSE;
        }
    }
    else
    {
        DRD_LOG_MESSAGE("System mode enabled, skipping capture/encoding initialization");
    }

    self->listener = drd_rdp_listener_new(drd_config_get_bind_address(self->config),
                                          drd_config_get_port(self->config),
                                          self->runtime,
                                          encoding_opts,
                                          nla_mode,
                                          nla_username,
                                          nla_password,
                                          pam_service,
                                          system_mode,
                                          rdp_sso_enabled);
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
    gchar *nla_mode = NULL;
    gboolean system_mode_flag = FALSE;
    gboolean rdp_sso_flag = FALSE;

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
        {"nla-mode", 0, 0, G_OPTION_ARG_STRING, &nla_mode, "NLA mode (static|delegate)", "MODE"},
        {"system", 0, 0, G_OPTION_ARG_NONE, &system_mode_flag, "Run in system mode (root, PAM delegate)", NULL},
        {"enable-rdp-sso", 0, 0, G_OPTION_ARG_NONE, &rdp_sso_flag, "Enable TLS-only RDP single sign-on (system mode only)", NULL},
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
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        g_clear_pointer(&nla_mode, g_free);
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
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        g_clear_pointer(&nla_mode, g_free);
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
                               nla_mode,
                               system_mode_flag,
                               rdp_sso_flag,
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
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        g_clear_pointer(&nla_mode, g_free);
        return FALSE;
    }

    if (drd_config_get_system_mode(self->config) && geteuid() != 0)
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
        g_clear_pointer(&nla_username, g_free);
        g_clear_pointer(&nla_password, g_free);
        g_clear_pointer(&nla_mode, g_free);
        return FALSE;
    }

    g_clear_pointer(&bind_address, g_free);
    g_clear_pointer(&cert_path, g_free);
    g_clear_pointer(&key_path, g_free);
    g_clear_pointer(&config_path, g_free);
    g_clear_pointer(&encoder_mode, g_free);
    g_clear_pointer(&nla_username, g_free);
    g_clear_pointer(&nla_password, g_free);
    g_clear_pointer(&nla_mode, g_free);

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

    if (!drd_application_start_listener(self, error))
    {
        return EXIT_FAILURE;
    }

    DRD_LOG_MESSAGE("RDP service listening on %s:%u",
              drd_config_get_bind_address(self->config),
              drd_config_get_port(self->config));
    DRD_LOG_MESSAGE("Loaded TLS credentials (cert=%s, key=%s)",
              drd_config_get_certificate_path(self->config),
              drd_config_get_private_key_path(self->config));
    g_main_loop_run(self->loop);

    DRD_LOG_MESSAGE("Main loop terminated");
    return EXIT_SUCCESS;
}
