#include "core/drd_config.h"

#include <gio/gio.h>

#define DRD_PAM_SERVICE_DEFAULT "deepin-remote-desktop"
#define DRD_PAM_SERVICE_SYSTEM "deepin-remote-desktop-system"

struct _DrdConfig
{
    GObject parent_instance;

    gchar *bind_address;
    guint16 port;
    gchar *certificate_path;
    gchar *private_key_path;
    gchar *nla_username;
    gchar *nla_password;
    gchar *base_dir;
    gboolean nla_enabled;
    DrdRuntimeMode runtime_mode;
    gchar *pam_service;
    gboolean pam_service_overridden;
    DrdEncodingOptions encoding;
    guint capture_target_fps;
    guint capture_stats_interval_sec;
};

G_DEFINE_TYPE(DrdConfig, drd_config, G_TYPE_OBJECT)

static gboolean drd_config_parse_bool(const gchar *value, gboolean *out_value, GError **error);

static gboolean drd_config_set_mode_from_string(DrdConfig *self, const gchar *value, GError **error);

static gboolean drd_config_parse_runtime_mode(const gchar *value,
                                              DrdRuntimeMode *out_mode,
                                              GError **error);

static void drd_config_set_runtime_mode_internal(DrdConfig *self, DrdRuntimeMode mode);

static void drd_config_refresh_pam_service(DrdConfig * self);

/*
 * 功能：释放配置实例中持有的动态字符串资源。
 * 逻辑：依次清理绑定地址、证书路径、NLA 凭据、基目录与 PAM 服务名，最后交由父类 dispose。
 * 参数：object 基类指针，期望为 DrdConfig。
 * 外部接口：GLib g_clear_pointer/g_free 释放字符串，GObjectClass::dispose。
 */
static void
drd_config_dispose(GObject *object)
{
    DrdConfig *self = DRD_CONFIG(object);
    g_clear_pointer(&self->bind_address, g_free);
    g_clear_pointer(&self->certificate_path, g_free);
    g_clear_pointer(&self->private_key_path, g_free);
    g_clear_pointer(&self->nla_username, g_free);
    g_clear_pointer(&self->nla_password, g_free);
    g_clear_pointer(&self->base_dir, g_free);
    g_clear_pointer(&self->pam_service, g_free);
    G_OBJECT_CLASS(drd_config_parent_class)->dispose(object);
}

/*
 * 功能：绑定类级别的析构回调。
 * 逻辑：将自定义 dispose 赋给 GObjectClass。
 * 参数：klass 类结构。
 * 外部接口：GLib 类型系统。
 */
static void
drd_config_class_init(DrdConfigClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_config_dispose;
}

/*
 * 功能：初始化配置对象的默认值。
 * 逻辑：设置默认监听地址/端口、编码分辨率与模式、捕获帧率观测默认值、NLA 与运行模式默认值，初始化 pam_service 并刷新 PAM 服务名。
 * 参数：self 配置实例。
 * 外部接口：GLib g_strdup/g_get_current_dir 处理字符串与默认路径。
 */
static void
drd_config_init(DrdConfig *self)
{
    self->bind_address = g_strdup("0.0.0.0");
    self->port = 3390;
    self->encoding.width = 1024;
    self->encoding.height = 768;
    self->encoding.mode = DRD_ENCODING_MODE_RFX;
    self->encoding.enable_frame_diff = TRUE;
    self->encoding.h264_bitrate = DRD_H264_DEFAULT_BITRATE;
    self->encoding.h264_framerate = DRD_H264_DEFAULT_FRAMERATE;
    self->encoding.h264_qp = DRD_H264_DEFAULT_QP;
    self->encoding.gfx_large_change_threshold = DRD_GFX_DEFAULT_LARGE_CHANGE_THRESHOLD;
    self->encoding.gfx_progressive_refresh_interval = DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_INTERVAL;
    self->encoding.gfx_progressive_refresh_timeout_ms = DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_TIMEOUT_MS;
    self->base_dir = g_get_current_dir();
    self->nla_username = NULL;
    self->nla_password = NULL;
    self->nla_enabled = TRUE;
    self->runtime_mode = DRD_RUNTIME_MODE_USER;
    self->pam_service_overridden = FALSE;
    self->pam_service = NULL;
    self->capture_target_fps = 60;
    self->capture_stats_interval_sec = 5;
    drd_config_refresh_pam_service(self);
}

/*
 * 功能：创建新的配置对象。
 * 逻辑：调用 g_object_new 分配并初始化。
 * 参数：无。
 * 外部接口：GLib g_object_new。
 */
DrdConfig *
drd_config_new(void)
{
    return g_object_new(DRD_TYPE_CONFIG, NULL);
}

/*
 * 功能：解析布尔字符串。
 * 逻辑：匹配 true/false 的多种大小写与数字表示，解析失败写入错误。
 * 参数：value 输入字符串；out_value 输出布尔值；error 错误输出。
 * 外部接口：GLib g_ascii_strcasecmp/g_set_error。
 */
static gboolean
drd_config_parse_bool(const gchar *value, gboolean *out_value, GError **error)
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

/*
 * 功能：解析运行模式字符串。
 * 逻辑：匹配 user/system/handover，写入枚举值，其他值报错。
 * 参数：value 字符串；out_mode 输出枚举；error 错误输出。
 * 外部接口：GLib g_ascii_strcasecmp/g_set_error。
 */
static gboolean
drd_config_parse_runtime_mode(const gchar *value,
                              DrdRuntimeMode *out_mode,
                              GError **error)
{
    if (value == NULL)
    {
        return FALSE;
    }

    if (g_ascii_strcasecmp(value, "user") == 0)
    {
        *out_mode = DRD_RUNTIME_MODE_USER;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "system") == 0)
    {
        *out_mode = DRD_RUNTIME_MODE_SYSTEM;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "handover") == 0)
    {
        *out_mode = DRD_RUNTIME_MODE_HANDOVER;
        return TRUE;
    }

    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid runtime mode '%s' (expected user, system or handover)",
                value);
    return FALSE;
}

/*
 * 功能：设置运行模式并刷新相关配置。
 * 逻辑：在模式变更时更新内部枚举并调用 PAM 服务刷新。
 * 参数：self 配置实例；mode 新模式。
 * 外部接口：调用 drd_config_refresh_pam_service，GLib g_return_if_fail。
 */
static void
drd_config_set_runtime_mode_internal(DrdConfig *self, DrdRuntimeMode mode)
{
    g_return_if_fail(DRD_IS_CONFIG(self));

    if (self->runtime_mode == mode)
    {
        return;
    }

    self->runtime_mode = mode;
    drd_config_refresh_pam_service(self);
}

/*
 * 功能：根据字符串设置编码模式。
 * 逻辑：接受 h264/rfx/remotefx/auto 并写入对应枚举，非法值时报错。
 * 参数：self 配置实例；value 模式名称；error 错误输出。
 * 外部接口：GLib g_ascii_strcasecmp/g_set_error。
 */
static gboolean
drd_config_set_mode_from_string(DrdConfig *self, const gchar *value, GError **error)
{
    if (value == NULL)
    {
        return FALSE;
    }
    if (g_ascii_strcasecmp(value, "h264") == 0)
    {
        self->encoding.mode = DRD_ENCODING_MODE_H264;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "rfx") == 0)
    {
        self->encoding.mode = DRD_ENCODING_MODE_RFX;
        return TRUE;
    }
    if (g_ascii_strcasecmp(value, "auto") == 0)
    {
        self->encoding.mode = DRD_ENCODING_MODE_AUTO;
        return TRUE;
    }
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "Unknown encoder mode '%s' (expected h264, rfx or auto)",
                value);
    return FALSE;
}

/*
 * 功能：根据当前运行模式刷新 PAM 服务名。
 * 逻辑：若未被 CLI/配置覆盖则为 system 模式设置 system 服务名，否则使用默认服务名。
 * 参数：self 配置实例。
 * 外部接口：GLib g_clear_pointer/g_strdup。
 */
static void
drd_config_refresh_pam_service(DrdConfig *self)
{
    g_return_if_fail(DRD_IS_CONFIG(self));

    if (self->pam_service_overridden)
    {
        return;
    }

    g_clear_pointer(&self->pam_service, g_free);
    if (self->runtime_mode == DRD_RUNTIME_MODE_SYSTEM)
    {
        self->pam_service = g_strdup(DRD_PAM_SERVICE_SYSTEM);
    }
    else
    {
        self->pam_service = g_strdup(DRD_PAM_SERVICE_DEFAULT);
    }
}

/*
 * 功能：覆盖 PAM 服务名。
 * 逻辑：若给定值非空则替换 pam_service 并标记已覆盖。
 * 参数：self 配置实例；value 新服务名。
 * 外部接口：GLib g_clear_pointer/g_strdup。
 */
static void
drd_config_override_pam_service(DrdConfig *self, const gchar *value)
{
    g_return_if_fail(DRD_IS_CONFIG(self));

    if (value == NULL || *value == '\0')
    {
        return;
    }
    g_clear_pointer(&self->pam_service, g_free);
    self->pam_service = g_strdup(value);
    self->pam_service_overridden = TRUE;
}

/*
 * 功能：解析路径为绝对路径。
 * 逻辑：若路径已绝对直接复制，否则基于 base_dir/current_dir 组合并规范化返回。
 * 参数：self 配置实例；value 原始路径。
 * 外部接口：GLib g_path_is_absolute/g_build_filename/g_canonicalize_filename。
 */
static gchar *
drd_config_resolve_path(DrdConfig *self, const gchar *value)
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

/*
 * 功能：从 GKeyFile 读取配置段并写入实例。
 * 逻辑：解析 server/tls/capture/encoding/auth/service 等段，处理布尔与枚举校验，必要时转换路径或刷新 PAM 服务。
 * 参数：self 配置实例；keyfile 解析后的 GKeyFile；error 错误输出。
 * 外部接口：GLib GKeyFile API（g_key_file_get_*）、g_set_error；调用 drd_config_parse_bool/drd_config_set_mode_from_string/drd_config_parse_runtime_mode 等内部解析函数。
 */
static gboolean
drd_config_load_from_key_file(DrdConfig *self, GKeyFile *keyfile, GError **error)
{
    gboolean nla_auth_override = FALSE;

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
        self->port = (guint16) port;
    }

    g_clear_pointer(&self->certificate_path, g_free);
    g_clear_pointer(&self->private_key_path, g_free);

    if (g_key_file_has_key(keyfile, "tls", "certificate", NULL))
    {
        gchar *value = g_key_file_get_string(keyfile, "tls", "certificate", NULL);
        g_clear_pointer(&self->certificate_path, g_free);
        self->certificate_path = drd_config_resolve_path(self, value);
        g_free(value);
    }

    if (g_key_file_has_key(keyfile, "tls", "private_key", NULL))
    {
        gchar *value = g_key_file_get_string(keyfile, "tls", "private_key", NULL);
        g_clear_pointer(&self->private_key_path, g_free);
        self->private_key_path = drd_config_resolve_path(self, value);
        g_free(value);
    }

    if (g_key_file_has_key(keyfile, "capture", "width", NULL))
    {
        gint64 width = g_key_file_get_integer(keyfile, "capture", "width", NULL);
        if (width > 0)
        {
            self->encoding.width = (guint) width;
        }
    }

    if (g_key_file_has_key(keyfile, "capture", "height", NULL))
    {
        gint64 height = g_key_file_get_integer(keyfile, "capture", "height", NULL);
        if (height > 0)
        {
            self->encoding.height = (guint) height;
        }
    }

    if (g_key_file_has_key(keyfile, "capture", "target_fps", NULL))
    {
        gint64 target_fps = g_key_file_get_integer(keyfile, "capture", "target_fps", NULL);
        if (target_fps > 0)
        {
            self->capture_target_fps = (guint) target_fps;
        }
    }

    if (g_key_file_has_key(keyfile, "capture", "stats_interval_sec", NULL))
    {
        gint64 stats_interval = g_key_file_get_integer(keyfile, "capture", "stats_interval_sec", NULL);
        if (stats_interval > 0)
        {
            self->capture_stats_interval_sec = (guint) stats_interval;
        }
    }

    if (g_key_file_has_key(keyfile, "encoding", "mode", NULL))
    {
        g_autofree gchar *mode = g_key_file_get_string(keyfile, "encoding", "mode", NULL);
        if (!drd_config_set_mode_from_string(self, mode, error))
        {
            return FALSE;
        }
    }

    if (g_key_file_has_key(keyfile, "encoding", "enable_diff", NULL))
    {
        g_autofree gchar *diff = g_key_file_get_string(keyfile, "encoding", "enable_diff", NULL);
        gboolean value = TRUE;
        if (!drd_config_parse_bool(diff, &value, error))
        {
            return FALSE;
        }
        self->encoding.enable_frame_diff = value;
    }

    if (g_key_file_has_key(keyfile, "encoding", "h264_bitrate", NULL))
    {
        gint64 bitrate = g_key_file_get_integer(keyfile, "encoding", "h264_bitrate", NULL);
        if (bitrate <= 0)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid h264_bitrate %" G_GINT64_FORMAT " (must be >0)",
                        bitrate);
            return FALSE;
        }
        self->encoding.h264_bitrate = (guint) bitrate;
    }

    if (g_key_file_has_key(keyfile, "encoding", "h264_framerate", NULL))
    {
        gint64 framerate = g_key_file_get_integer(keyfile, "encoding", "h264_framerate", NULL);
        if (framerate <= 0)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid h264_framerate %" G_GINT64_FORMAT " (must be >0)",
                        framerate);
            return FALSE;
        }
        self->encoding.h264_framerate = (guint) framerate;
    }

    if (g_key_file_has_key(keyfile, "encoding", "h264_qp", NULL))
    {
        gint64 qp = g_key_file_get_integer(keyfile, "encoding", "h264_qp", NULL);
        if (qp <= 0)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid h264_qp %" G_GINT64_FORMAT " (must be >0)",
                        qp);
            return FALSE;
        }
        self->encoding.h264_qp = (guint) qp;
    }

    if (g_key_file_has_key(keyfile, "encoding", "gfx_large_change_threshold", NULL))
    {
        gdouble threshold = g_key_file_get_double(keyfile, "encoding", "gfx_large_change_threshold", NULL);
        if (threshold < 0.0)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid gfx_large_change_threshold %f (must be >=0)",
                        threshold);
            return FALSE;
        }
        self->encoding.gfx_large_change_threshold = threshold;
    }

    if (g_key_file_has_key(keyfile, "encoding", "gfx_progressive_refresh_interval", NULL))
    {
        gint64 interval = g_key_file_get_integer(keyfile, "encoding", "gfx_progressive_refresh_interval", NULL);
        if (interval < 0)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid gfx_progressive_refresh_interval %" G_GINT64_FORMAT " (must be >=0)",
                        interval);
            return FALSE;
        }
        self->encoding.gfx_progressive_refresh_interval = (guint) interval;
    }

    if (g_key_file_has_key(keyfile, "encoding", "gfx_progressive_refresh_timeout_ms", NULL))
    {
        gint64 timeout_ms = g_key_file_get_integer(keyfile, "encoding", "gfx_progressive_refresh_timeout_ms", NULL);
        if (timeout_ms < 0)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid gfx_progressive_refresh_timeout_ms %" G_GINT64_FORMAT " (must be >=0)",
                        timeout_ms);
            return FALSE;
        }
        self->encoding.gfx_progressive_refresh_timeout_ms = (guint) timeout_ms;
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

    if (g_key_file_has_key(keyfile, "auth", "mode", NULL))
    {
        g_autofree gchar *mode = g_key_file_get_string(keyfile, "auth", "mode", NULL);
        if (mode != NULL && g_ascii_strcasecmp(mode, "static") == 0)
        {
            self->nla_enabled = TRUE;
        }
        else
        {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "NLA delegate mode has been removed; disable NLA via [auth] enable_nla=false");
            return FALSE;
        }
    }

    if (g_key_file_has_key(keyfile, "auth", "enable_nla", NULL))
    {
        g_autofree gchar *nla_value = g_key_file_get_string(keyfile, "auth", "enable_nla", NULL);
        gboolean enable_nla = TRUE;
        if (!drd_config_parse_bool(nla_value, &enable_nla, error))
        {
            return FALSE;
        }
        self->nla_enabled = enable_nla;
        nla_auth_override = TRUE;
    }
    else if (g_key_file_has_key(keyfile, "auth", "nla", NULL))
    {
        g_autofree gchar *nla_value = g_key_file_get_string(keyfile, "auth", "nla", NULL);
        gboolean enable_nla = TRUE;
        if (!drd_config_parse_bool(nla_value, &enable_nla, error))
        {
            return FALSE;
        }
        self->nla_enabled = enable_nla;
        nla_auth_override = TRUE;
    }

    if (g_key_file_has_key(keyfile, "auth", "pam_service", NULL))
    {
        g_autofree gchar *pam_service = g_key_file_get_string(keyfile, "auth", "pam_service", NULL);
        drd_config_override_pam_service(self, pam_service);
    }

    if (g_key_file_has_key(keyfile, "service", "runtime_mode", NULL))
    {
        g_autofree gchar *runtime_mode =
                g_key_file_get_string(keyfile, "service", "runtime_mode", NULL);
        DrdRuntimeMode parsed_mode = DRD_RUNTIME_MODE_USER;
        if (!drd_config_parse_runtime_mode(runtime_mode, &parsed_mode, error))
        {
            return FALSE;
        }
        drd_config_set_runtime_mode_internal(self, parsed_mode);
    }
    else if (g_key_file_has_key(keyfile, "service", "system", NULL))
    {
        g_autofree gchar *system_value = g_key_file_get_string(keyfile, "service", "system", NULL);
        gboolean system_mode = FALSE;
        if (!drd_config_parse_bool(system_value, &system_mode, error))
        {
            return FALSE;
        }
        drd_config_set_runtime_mode_internal(self,
                                             system_mode
                                                 ? DRD_RUNTIME_MODE_SYSTEM
                                                 : DRD_RUNTIME_MODE_USER);
    }

    if (!nla_auth_override && g_key_file_has_key(keyfile, "service", "rdp_sso", NULL))
    {
        g_autofree gchar *rdp_sso_str = g_key_file_get_string(keyfile, "service", "rdp_sso", NULL);
        gboolean rdp_sso = FALSE;
        if (!drd_config_parse_bool(rdp_sso_str, &rdp_sso, error))
        {
            return FALSE;
        }
        self->nla_enabled = !rdp_sso;
    }

    return TRUE;
}

/*
 * 功能：从磁盘加载 ini 配置并返回配置对象。
 * 逻辑：读取文件为 GKeyFile，构造默认配置并设置 base_dir，随后调用 load_from_key_file 填充字段，失败则释放对象。
 * 参数：path 配置文件路径；error 错误输出。
 * 外部接口：GLib g_key_file_load_from_file/g_path_get_dirname；内部 drd_config_load_from_key_file。
 */
DrdConfig *
drd_config_new_from_file(const gchar *path, GError **error)
{
    g_return_val_if_fail(path != NULL, NULL);

    g_autoptr(GKeyFile)
    keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, error))
    {
        return NULL;
    }

    DrdConfig *config = drd_config_new();
    g_clear_pointer(&config->base_dir, g_free);
    config->base_dir = g_path_get_dirname(path);
    if (config->base_dir == NULL)
    {
        config->base_dir = g_get_current_dir();
    }
    if (!drd_config_load_from_key_file(config, keyfile, error))
    {
        g_clear_object(&config);
        return NULL;
    }

    return config;
}

/*
 * 功能：在 CLI 合并时更新字符串字段。
 * 逻辑：若值非空则释放旧值并复制新值。
 * 参数：target 目标字段指针；value 新字符串。
 * 外部接口：GLib g_clear_pointer/g_strdup。
 */
static gboolean
drd_config_set_string(gchar **target, const gchar *value)
{
    if (value == NULL)
    {
        return FALSE;
    }
    g_clear_pointer(target, g_free);
    *target = g_strdup(value);
    return TRUE;
}

/*
 * 功能：合并 CLI 路径字段。
 * 逻辑：解析给定路径为绝对路径并替换目标字段。
 * 参数：self 配置实例；target 目标指针；value CLI 值。
 * 外部接口：内部 drd_config_resolve_path，GLib g_free。
 */
static gboolean
drd_config_set_path(DrdConfig *self, gchar **target, const gchar *value)
{
    if (value == NULL)
    {
        return FALSE;
    }
    gchar *resolved = drd_config_resolve_path(self, value);
    if (resolved == NULL)
    {
        return FALSE;
    }
    g_free(*target);
    *target = resolved;
    return TRUE;
}


/*
 * 功能：将命令行参数合并到配置实例中。
 * 逻辑：逐项覆盖监听地址/端口、TLS 路径、NLA 凭据、运行模式、分辨率、编码模式与差分开关；校验互斥条件与必填项；NLA/PAM 约束失败则报错。
 * 参数：self 配置实例；bind_address/port 等 CLI 参数；error 错误输出。
 * 外部接口：GLib g_set_error_literal/g_set_error；调用 drd_config_set_path/drd_config_set_mode_from_string/drd_config_parse_runtime_mode 等内部方法。
 */
gboolean
drd_config_merge_cli(DrdConfig *self,
                     const gchar *bind_address,
                     gint port,
                     const gchar *cert_path,
                     const gchar *key_path,
                     const gchar *nla_username,
                     const gchar *nla_password,
                     gboolean cli_enable_nla,
                     gboolean cli_disable_nla,
                     const gchar *runtime_mode_name,
                     gint width,
                     gint height,
                     const gchar *encoder_mode,
                     gint diff_override,
                     gint capture_target_fps,
                     gint capture_stats_interval_sec,
                     GError **error)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), FALSE);

    if (bind_address != NULL)
    {
        drd_config_set_string(&self->bind_address, bind_address);
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
        self->port = (guint16) port;
    }

    if (cert_path != NULL)
    {
        drd_config_set_path(self, &self->certificate_path, cert_path);
    }

    if (key_path != NULL)
    {
        drd_config_set_path(self, &self->private_key_path, key_path);
    }

    if (nla_username != NULL)
    {
        drd_config_set_string(&self->nla_username, nla_username);
    }

    if (nla_password != NULL)
    {
        drd_config_set_string(&self->nla_password, nla_password);
    }

    if (cli_enable_nla && cli_disable_nla)
    {
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "Cannot enable and disable NLA at the same time");
        return FALSE;
    }
    if (cli_enable_nla)
    {
        self->nla_enabled = TRUE;
    }
    else if (cli_disable_nla)
    {
        self->nla_enabled = FALSE;
    }

    if (runtime_mode_name != NULL)
    {
        DrdRuntimeMode cli_mode = self->runtime_mode;
        if (runtime_mode_name != NULL)
        {
            if (!drd_config_parse_runtime_mode(runtime_mode_name, &cli_mode, error))
            {
                return FALSE;
            }
        }
        drd_config_set_runtime_mode_internal(self, cli_mode);
    }

    if (width > 0)
    {
        self->encoding.width = (guint) width;
    }

    if (height > 0)
    {
        self->encoding.height = (guint) height;
    }

    if (encoder_mode != NULL)
    {
        if (!drd_config_set_mode_from_string(self, encoder_mode, error))
        {
            return FALSE;
        }
    }

    if (diff_override != 0)
    {
        self->encoding.enable_frame_diff = diff_override > 0;
    }

    if (capture_target_fps > 0)
    {
        self->capture_target_fps = (guint) capture_target_fps;
    }

    if (capture_stats_interval_sec > 0)
    {
        self->capture_stats_interval_sec = (guint) capture_stats_interval_sec;
    }

    if (self->runtime_mode != DRD_RUNTIME_MODE_HANDOVER &&
        (self->certificate_path == NULL || self->private_key_path == NULL))
    {
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "TLS certificate and private key must be specified via config or CLI");
        return FALSE;
    }

    if (!self->nla_enabled && self->runtime_mode != DRD_RUNTIME_MODE_SYSTEM)
    {
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "Disabling NLA requires --system");
        return FALSE;
    }

    if (self->nla_enabled && self->runtime_mode != DRD_RUNTIME_MODE_HANDOVER)
    {
        if (self->nla_username == NULL || *self->nla_username == '\0' ||
            self->nla_password == NULL || *self->nla_password == '\0')
        {
            g_set_error_literal(error,
                                G_OPTION_ERROR,
                                G_OPTION_ERROR_BAD_VALUE,
                                "NLA username and password must be specified via config or CLI");
            return FALSE;
        }
    }

    if (self->pam_service == NULL || *self->pam_service == '\0')
    {
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "PAM service name is not configured");
        return FALSE;
    }

    return TRUE;
}

/*
 * 功能：获取监听地址。
 * 逻辑：类型检查后返回存储的字符串。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
const gchar *
drd_config_get_bind_address(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), NULL);
    return self->bind_address;
}

/*
 * 功能：获取监听端口。
 * 逻辑：类型检查后返回端口号。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
guint16
drd_config_get_port(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), 0);
    return self->port;
}

/*
 * 功能：获取证书路径。
 * 逻辑：类型检查后返回证书路径字符串。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
const gchar *
drd_config_get_certificate_path(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), NULL);
    return self->certificate_path;
}

/*
 * 功能：获取私钥路径。
 * 逻辑：类型检查后返回私钥路径。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
const gchar *
drd_config_get_private_key_path(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), NULL);
    return self->private_key_path;
}

/*
 * 功能：获取 NLA 用户名。
 * 逻辑：类型检查后返回用户名字符串。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
const gchar *
drd_config_get_nla_username(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), NULL);
    return self->nla_username;
}

/*
 * 功能：获取 NLA 密码。
 * 逻辑：类型检查后返回密码字符串。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
const gchar *
drd_config_get_nla_password(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), NULL);
    return self->nla_password;
}

/*
 * 功能：查询是否启用 NLA。
 * 逻辑：类型检查后返回标志。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
gboolean
drd_config_is_nla_enabled(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), TRUE);
    return self->nla_enabled;
}

/*
 * 功能：获取运行模式。
 * 逻辑：类型检查后返回枚举。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
DrdRuntimeMode
drd_config_get_runtime_mode(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), DRD_RUNTIME_MODE_USER);
    return self->runtime_mode;
}

/*
 * 功能：获取 PAM 服务名。
 * 逻辑：类型检查后返回服务名。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
const gchar *
drd_config_get_pam_service(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), NULL);
    return self->pam_service;
}

/*
 * 功能：获取采集宽度。
 * 逻辑：类型检查后返回 width。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
guint
drd_config_get_capture_width(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), 0);
    return self->encoding.width;
}

/*
 * 功能：获取采集高度。
 * 逻辑：类型检查后返回 height。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
guint
drd_config_get_capture_height(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), 0);
    return self->encoding.height;
}

/*
 * 功能：获取捕获目标帧率。
 * 逻辑：类型检查后返回配置中的 target_fps。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
guint
drd_config_get_capture_target_fps(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), 60);
    return self->capture_target_fps;
}

/*
 * 功能：获取帧率统计窗口秒数。
 * 逻辑：类型检查后返回 stats_interval_sec。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
guint
drd_config_get_capture_stats_interval_sec(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), 5);
    return self->capture_stats_interval_sec;
}

/*
 * 功能：获取编码选项结构体。
 * 逻辑：类型检查后返回内部 encoding 指针。
 * 参数：self 配置实例。
 * 外部接口：无额外外部库。
 */
const DrdEncodingOptions *
drd_config_get_encoding_options(DrdConfig *self)
{
    g_return_val_if_fail(DRD_IS_CONFIG(self), NULL);
    return &self->encoding;
}
