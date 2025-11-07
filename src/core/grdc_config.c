#include "core/grdc_config.h"

#include <gio/gio.h>

struct _GrdcConfig
{
    GObject parent_instance;

    gchar *bind_address;
    guint16 port;
    gchar *certificate_path;
    gchar *private_key_path;
    gchar *nla_username;
    gchar *nla_password;
    gchar *base_dir;
    GrdcEncodingOptions encoding;
};

G_DEFINE_TYPE(GrdcConfig, grdc_config, G_TYPE_OBJECT)

static gboolean grdc_config_parse_bool(const gchar *value, gboolean *out_value, GError **error);
static gboolean grdc_config_set_mode_from_string(GrdcConfig *self, const gchar *value, GError **error);

/* 释放配置对象中持有的动态字符串。 */
static void
grdc_config_dispose(GObject *object)
{
    GrdcConfig *self = GRDC_CONFIG(object);
    g_clear_pointer(&self->bind_address, g_free);
    g_clear_pointer(&self->certificate_path, g_free);
    g_clear_pointer(&self->private_key_path, g_free);
    g_clear_pointer(&self->nla_username, g_free);
    g_clear_pointer(&self->nla_password, g_free);
    g_clear_pointer(&self->base_dir, g_free);
    G_OBJECT_CLASS(grdc_config_parent_class)->dispose(object);
}

/* 绑定 dispose 钩子。 */
static void
grdc_config_class_init(GrdcConfigClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = grdc_config_dispose;
}

/* 设置配置缺省值，包括监听地址、分辨率等。 */
static void
grdc_config_init(GrdcConfig *self)
{
    self->bind_address = g_strdup("0.0.0.0");
    self->port = 3390;
    self->encoding.width = 1024;
    self->encoding.height = 768;
    self->encoding.mode = GRDC_ENCODING_MODE_RFX;
    self->encoding.enable_frame_diff = TRUE;
    self->base_dir = g_get_current_dir();
    self->nla_username = NULL;
    self->nla_password = NULL;
}

/* 构造新的配置实例。 */
GrdcConfig *
grdc_config_new(void)
{
    return g_object_new(GRDC_TYPE_CONFIG, NULL);
}

/* 将字符串解析为布尔值，用于配置文件处理。 */
static gboolean
grdc_config_parse_bool(const gchar *value, gboolean *out_value, GError **error)
{
    if (value == NULL)
    {
        return FALSE;
    }
    if (g_ascii_strcasecmp(value, "true") == 0 || g_ascii_strcasecmp(value, "yes") == 0 ||
        g_ascii_strcasecmp(value, "1") == 0)
    {
        *out_value = TRUE;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "false") == 0 || g_ascii_strcasecmp(value, "no") == 0 ||
        g_ascii_strcasecmp(value, "0") == 0)
    {
        *out_value = FALSE;
        return TRUE;
    }
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid boolean value '%s'",
                value);
    return FALSE;
}

/* 根据名称切换编码模式。 */
static gboolean
grdc_config_set_mode_from_string(GrdcConfig *self, const gchar *value, GError **error)
{
    if (value == NULL)
    {
        return FALSE;
    }
    if (g_ascii_strcasecmp(value, "raw") == 0)
    {
        self->encoding.mode = GRDC_ENCODING_MODE_RAW;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "rfx") == 0 || g_ascii_strcasecmp(value, "remotefx") == 0)
    {
        self->encoding.mode = GRDC_ENCODING_MODE_RFX;
        return TRUE;
    }
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "Unknown encoder mode '%s' (expected raw or rfx)",
                value);
    return FALSE;
}

/* 把相对路径转换为绝对路径，便于后续加载证书等资源。 */
static gchar *
grdc_config_resolve_path(GrdcConfig *self, const gchar *value)
{
    if (value == NULL)
    {
        return NULL;
    }

    if (g_path_is_absolute(value))
    {
        return g_strdup(value);
    }

    const gchar *base = self->base_dir != NULL ? self->base_dir : g_get_current_dir();
    gchar *combined = g_build_filename(base, value, NULL);
    gchar *canonical = g_canonicalize_filename(combined, NULL);
    g_free(combined);
    return canonical;
}

/* 读取 ini 配置文件的所有段并写入当前实例。 */
static gboolean
grdc_config_load_from_key_file(GrdcConfig *self, GKeyFile *keyfile, GError **error)
{
    if (g_key_file_has_key(keyfile, "server", "bind_address", NULL))
    {
        g_clear_pointer(&self->bind_address, g_free);
        self->bind_address = g_key_file_get_string(keyfile, "server", "bind_address", NULL);
    }

    if (g_key_file_has_key(keyfile, "server", "port", NULL))
    {
        gint64 port = g_key_file_get_integer(keyfile, "server", "port", NULL);
        if (port <= 0 || port > G_MAXUINT16)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid port value %" G_GINT64_FORMAT " in configuration", port);
            return FALSE;
        }
        self->port = (guint16)port;
    }

    g_clear_pointer(&self->certificate_path, g_free);
    g_clear_pointer(&self->private_key_path, g_free);

    if (g_key_file_has_key(keyfile, "tls", "certificate", NULL))
    {
        gchar *value = g_key_file_get_string(keyfile, "tls", "certificate", NULL);
        g_clear_pointer(&self->certificate_path, g_free);
        self->certificate_path = grdc_config_resolve_path(self, value);
        g_free(value);
    }

    if (g_key_file_has_key(keyfile, "tls", "private_key", NULL))
    {
        gchar *value = g_key_file_get_string(keyfile, "tls", "private_key", NULL);
        g_clear_pointer(&self->private_key_path, g_free);
        self->private_key_path = grdc_config_resolve_path(self, value);
        g_free(value);
    }

    if (g_key_file_has_key(keyfile, "capture", "width", NULL))
    {
        gint64 width = g_key_file_get_integer(keyfile, "capture", "width", NULL);
        if (width > 0)
        {
            self->encoding.width = (guint)width;
        }
    }

    if (g_key_file_has_key(keyfile, "capture", "height", NULL))
    {
        gint64 height = g_key_file_get_integer(keyfile, "capture", "height", NULL);
        if (height > 0)
        {
            self->encoding.height = (guint)height;
        }
    }

    if (g_key_file_has_key(keyfile, "encoding", "mode", NULL))
    {
        g_autofree gchar *mode = g_key_file_get_string(keyfile, "encoding", "mode", NULL);
        if (!grdc_config_set_mode_from_string(self, mode, error))
        {
            return FALSE;
        }
    }

    if (g_key_file_has_key(keyfile, "encoding", "enable_diff", NULL))
    {
        g_autofree gchar *diff = g_key_file_get_string(keyfile, "encoding", "enable_diff", NULL);
        gboolean value = TRUE;
        if (!grdc_config_parse_bool(diff, &value, error))
        {
            return FALSE;
        }
        self->encoding.enable_frame_diff = value;
    }

    if (g_key_file_has_key(keyfile, "auth", "username", NULL))
    {
        g_clear_pointer(&self->nla_username, g_free);
        self->nla_username = g_key_file_get_string(keyfile, "auth", "username", NULL);
    }

    if (g_key_file_has_key(keyfile, "auth", "password", NULL))
    {
        g_clear_pointer(&self->nla_password, g_free);
        self->nla_password = g_key_file_get_string(keyfile, "auth", "password", NULL);
    }

    return TRUE;
}

/* 从磁盘加载配置文件并返回解析结果。 */
GrdcConfig *
grdc_config_new_from_file(const gchar *path, GError **error)
{
    g_return_val_if_fail(path != NULL, NULL);

    g_autoptr(GKeyFile) keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, error))
    {
        return NULL;
    }

    GrdcConfig *config = grdc_config_new();
    g_clear_pointer(&config->base_dir, g_free);
    config->base_dir = g_path_get_dirname(path);
    if (config->base_dir == NULL)
    {
        config->base_dir = g_get_current_dir();
    }
    if (!grdc_config_load_from_key_file(config, keyfile, error))
    {
        g_clear_object(&config);
        return NULL;
    }

    return config;
}

/* 用于在 CLI 合并时更新字符串字段。 */
static gboolean
grdc_config_set_string(gchar **target, const gchar *value)
{
    if (value == NULL)
    {
        return FALSE;
    }
    g_clear_pointer(target, g_free);
    *target = g_strdup(value);
    return TRUE;
}

/* 基于配置根目录解析 CLI 提供的路径。 */
static gboolean
grdc_config_set_path(GrdcConfig *self, gchar **target, const gchar *value)
{
    if (value == NULL)
    {
        return FALSE;
    }
    gchar *resolved = grdc_config_resolve_path(self, value);
    if (resolved == NULL)
    {
        return FALSE;
    }
    g_free(*target);
    *target = resolved;
    return TRUE;
}


/* 合并 CLI 选项，优先级高于配置文件。 */
gboolean
grdc_config_merge_cli(GrdcConfig *self,
                      const gchar *bind_address,
                      gint port,
                      const gchar *cert_path,
                      const gchar *key_path,
                      const gchar *nla_username,
                      const gchar *nla_password,
                      gint width,
                      gint height,
                      const gchar *encoder_mode,
                      gint diff_override,
                      GError **error)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), FALSE);

    if (bind_address != NULL)
    {
        grdc_config_set_string(&self->bind_address, bind_address);
    }

    if (port > 0)
    {
        if (port > G_MAXUINT16)
        {
            g_set_error(error,
                        G_OPTION_ERROR,
                        G_OPTION_ERROR_BAD_VALUE,
                        "Port %d exceeds maximum %u", port, G_MAXUINT16);
            return FALSE;
        }
        self->port = (guint16)port;
    }

    if (cert_path != NULL)
    {
        grdc_config_set_path(self, &self->certificate_path, cert_path);
    }

    if (key_path != NULL)
    {
        grdc_config_set_path(self, &self->private_key_path, key_path);
    }

    if (nla_username != NULL)
    {
        grdc_config_set_string(&self->nla_username, nla_username);
    }

    if (nla_password != NULL)
    {
        grdc_config_set_string(&self->nla_password, nla_password);
    }

    if (width > 0)
    {
        self->encoding.width = (guint)width;
    }

    if (height > 0)
    {
        self->encoding.height = (guint)height;
    }

    if (encoder_mode != NULL)
    {
        if (!grdc_config_set_mode_from_string(self, encoder_mode, error))
        {
            return FALSE;
        }
    }

    if (diff_override != 0)
    {
        self->encoding.enable_frame_diff = diff_override > 0;
    }

    if (self->certificate_path == NULL || self->private_key_path == NULL)
    {
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "TLS certificate and private key must be specified via config or CLI");
        return FALSE;
    }

    if (self->nla_username == NULL || *self->nla_username == '\0' ||
        self->nla_password == NULL || *self->nla_password == '\0')
    {
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "NLA username and password must be specified via config or CLI");
        return FALSE;
    }

    return TRUE;
}

/* 读取监听地址。 */
const gchar *
grdc_config_get_bind_address(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), NULL);
    return self->bind_address;
}

/* 读取监听端口。 */
guint16
grdc_config_get_port(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), 0);
    return self->port;
}

/* 读取证书路径。 */
const gchar *
grdc_config_get_certificate_path(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), NULL);
    return self->certificate_path;
}

/* 读取私钥路径。 */
const gchar *
grdc_config_get_private_key_path(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), NULL);
    return self->private_key_path;
}

const gchar *
grdc_config_get_nla_username(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), NULL);
    return self->nla_username;
}

const gchar *
grdc_config_get_nla_password(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), NULL);
    return self->nla_password;
}

/* 获取采集宽度。 */
guint
grdc_config_get_capture_width(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), 0);
    return self->encoding.width;
}

/* 获取采集高度。 */
guint
grdc_config_get_capture_height(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), 0);
    return self->encoding.height;
}

/* 暴露编码选项结构体。 */
const GrdcEncodingOptions *
grdc_config_get_encoding_options(GrdcConfig *self)
{
    g_return_val_if_fail(GRDC_IS_CONFIG(self), NULL);
    return &self->encoding;
}
