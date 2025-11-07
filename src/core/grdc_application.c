#include "core/grdc_application.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <winpr/ssl.h>

#include "transport/grdc_rdp_listener.h"
#include "security/grdc_tls_credentials.h"
#include "core/grdc_config.h"
#include "core/grdc_server_runtime.h"

struct _GrdcApplication
{
    GObject parent_instance;

    GrdcConfig *config;
    GMainLoop *loop;
    GrdcRdpListener *listener;
    guint sigint_id;
    guint sigterm_id;
    GrdcServerRuntime *runtime;
    GrdcTlsCredentials *tls_credentials;
};

G_DEFINE_TYPE(GrdcApplication, grdc_application, G_TYPE_OBJECT)

/* 将编码模式转换成便于日志输出的字符串。 */
static const gchar *
grdc_application_mode_to_string(GrdcEncodingMode mode)
{
    switch (mode)
    {
        case GRDC_ENCODING_MODE_RAW:
            return "raw";
        case GRDC_ENCODING_MODE_RFX:
            return "rfx";
        default:
            return "unknown";
    }
}

/* 记录当前合并后的核心运行参数，帮助排查配置生效情况。 */
static void
grdc_application_log_effective_config(GrdcApplication *self)
{
    if (self->config == NULL)
    {
        return;
    }

    const GrdcEncodingOptions *encoding_opts = grdc_config_get_encoding_options(self->config);
    if (encoding_opts == NULL)
    {
        return;
    }

    g_message("Effective capture geometry %ux%u, encoder=%s, frame diff %s",
              encoding_opts->width,
              encoding_opts->height,
              grdc_application_mode_to_string(encoding_opts->mode),
              encoding_opts->enable_frame_diff ? "enabled" : "disabled");
}

/* 释放主循环、监听器等运行期资源，确保干净退出。 */
static void
grdc_application_dispose(GObject *object)
{
    GrdcApplication *self = GRDC_APPLICATION(object);

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
        grdc_rdp_listener_stop(self->listener);
        g_clear_object(&self->listener);
    }

    if (self->runtime != NULL)
    {
        grdc_server_runtime_stop(self->runtime);
        g_clear_object(&self->runtime);
    }

    g_clear_object(&self->tls_credentials);
    g_clear_object(&self->config);

    g_clear_pointer(&self->loop, g_main_loop_unref);

    G_OBJECT_CLASS(grdc_application_parent_class)->dispose(object);
}

/* 最终清理阶段，释放剩余引用。 */
static void
grdc_application_finalize(GObject *object)
{
    GrdcApplication *self = GRDC_APPLICATION(object);
    g_clear_object(&self->config);
    G_OBJECT_CLASS(grdc_application_parent_class)->finalize(object);
}

/* 信号回调：收到终止信号时请求主循环退出。 */
static gboolean
grdc_application_on_signal(gpointer user_data)
{
    GrdcApplication *self = GRDC_APPLICATION(user_data);

    if (self->loop != NULL && g_main_loop_is_running(self->loop))
    {
        g_message("Termination signal received, shutting down main loop");
        g_main_loop_quit(self->loop);
    }

    return G_SOURCE_CONTINUE;
}

/* 启动 RDP 监听器，并将 TLS、编码运行时串联起来。 */
static gboolean
grdc_application_start_listener(GrdcApplication *self, GError **error)
{
    g_assert(self->listener == NULL);
    g_return_val_if_fail(GRDC_IS_SERVER_RUNTIME(self->runtime), FALSE);

    if (self->config == NULL)
    {
        self->config = grdc_config_new();
    }

    const gchar *cert_path = grdc_config_get_certificate_path(self->config);
    const gchar *key_path = grdc_config_get_private_key_path(self->config);
    if (cert_path == NULL || key_path == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "TLS certificate or key path missing after config merge");
        return FALSE;
    }

    const gchar *nla_username = grdc_config_get_nla_username(self->config);
    const gchar *nla_password = grdc_config_get_nla_password(self->config);
    if (nla_username == NULL || nla_password == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "NLA username/password missing after config merge");
        return FALSE;
    }

    if (self->tls_credentials == NULL)
    {
        self->tls_credentials = grdc_tls_credentials_new(cert_path, key_path, error);
        if (self->tls_credentials == NULL)
        {
            return FALSE;
        }
        grdc_server_runtime_set_tls_credentials(self->runtime, self->tls_credentials);
    }

    const GrdcEncodingOptions *encoding_opts = grdc_config_get_encoding_options(self->config);
    if (!grdc_server_runtime_prepare_stream(self->runtime, encoding_opts, error))
    {
        return FALSE;
    }

    self->listener = grdc_rdp_listener_new(grdc_config_get_bind_address(self->config),
                                           grdc_config_get_port(self->config),
                                           self->runtime,
                                           nla_username,
                                           nla_password);
    if (self->listener == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to instantiate RDP listener");
        grdc_server_runtime_stop(self->runtime);
        return FALSE;
    }

    if (!grdc_rdp_listener_start(self->listener, error))
    {
        g_clear_object(&self->listener);
        grdc_server_runtime_stop(self->runtime);
        return FALSE;
    }

    return TRUE;
}

/* 解析 CLI 选项，并与配置文件合并。 */
static gboolean
grdc_application_parse_options(GrdcApplication *self, gint *argc, gchar ***argv, GError **error)
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

    GOptionEntry entries[] = {
        {"bind-address", 'b', 0, G_OPTION_ARG_STRING, &bind_address, "Bind address (default 0.0.0.0)", "ADDR"},
        {"port", 'p', 0, G_OPTION_ARG_INT, &port, "Bind port (default 3390 unless config overrides)", "PORT"},
        {"cert", 0, 0, G_OPTION_ARG_STRING, &cert_path, "TLS certificate PEM path", "FILE"},
        {"key", 0, 0, G_OPTION_ARG_STRING, &key_path, "TLS private key PEM path", "FILE"},
        {"config", 'c', 0, G_OPTION_ARG_STRING, &config_path, "Configuration file path (ini)", "FILE"},
        {"width", 0, 0, G_OPTION_ARG_INT, &capture_width, "Capture width override", "PX"},
        {"height", 0, 0, G_OPTION_ARG_INT, &capture_height, "Capture height override", "PX"},
        {"encoder", 0, 0, G_OPTION_ARG_STRING, &encoder_mode, "Encoder mode (raw|rfx)", "MODE"},
        {"nla-username", 0, 0, G_OPTION_ARG_STRING, &nla_username, "NLA username", "USER"},
        {"nla-password", 0, 0, G_OPTION_ARG_STRING, &nla_password, "NLA password", "PASS"},
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
        return FALSE;
    }

    if (config_path != NULL)
    {
        g_clear_object(&self->config);
        self->config = grdc_config_new_from_file(config_path, error);
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
        g_message("Configuration loaded from %s", config_path);
    }
    else if (self->config == NULL)
    {
        self->config = grdc_config_new();
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

    if (!grdc_config_merge_cli(self->config,
                               bind_address,
                               port,
                               cert_path,
                               key_path,
                               nla_username,
                               nla_password,
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
        return FALSE;
    }

    g_clear_pointer(&bind_address, g_free);
    g_clear_pointer(&cert_path, g_free);
    g_clear_pointer(&key_path, g_free);
    g_clear_pointer(&config_path, g_free);
    g_clear_pointer(&encoder_mode, g_free);
    g_clear_pointer(&nla_username, g_free);
    g_clear_pointer(&nla_password, g_free);

    return TRUE;
}

/* 初始化对象默认值。 */
static void
grdc_application_init(GrdcApplication *self)
{
    self->config = grdc_config_new();
    self->runtime = grdc_server_runtime_new();
    self->tls_credentials = NULL;

    if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
    {
        g_warning("Failed to initialize WinPR SSL context, NTLM may be unavailable");
    }
}

/* 绑定类虚函数。 */
static void
grdc_application_class_init(GrdcApplicationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = grdc_application_dispose;
    object_class->finalize = grdc_application_finalize;
}

/* 对外构造函数。 */
GrdcApplication *
grdc_application_new(void)
{
    return g_object_new(GRDC_TYPE_APPLICATION, NULL);
}

/* 应用入口：解析参数、启动监听、运行主循环。 */
int
grdc_application_run(GrdcApplication *self, int argc, char **argv, GError **error)
{
    g_return_val_if_fail(GRDC_IS_APPLICATION(self), EXIT_FAILURE);

    if (!grdc_application_parse_options(self, &argc, &argv, error))
    {
        return EXIT_FAILURE;
    }

    grdc_application_log_effective_config(self);

    self->loop = g_main_loop_new(NULL, FALSE);
    if (self->loop == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to create GMainLoop");
        return EXIT_FAILURE;
    }

    self->sigint_id = g_unix_signal_add(SIGINT, grdc_application_on_signal, self);
    self->sigterm_id = g_unix_signal_add(SIGTERM, grdc_application_on_signal, self);

    if (!grdc_application_start_listener(self, error))
    {
        return EXIT_FAILURE;
    }

    g_message("RDP service listening on %s:%u",
              grdc_config_get_bind_address(self->config),
              grdc_config_get_port(self->config));
    g_message("Loaded TLS credentials (cert=%s, key=%s)",
              grdc_config_get_certificate_path(self->config),
              grdc_config_get_private_key_path(self->config));
    g_main_loop_run(self->loop);

    g_message("Main loop terminated");
    return EXIT_SUCCESS;
}
