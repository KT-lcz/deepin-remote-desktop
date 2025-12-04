#pragma once

#include <glib-object.h>

#include "core/drd_encoding_options.h"

G_BEGIN_DECLS

#define DRD_TYPE_CONFIG (drd_config_get_type())
G_DECLARE_FINAL_TYPE(DrdConfig, drd_config, DRD, CONFIG, GObject)

typedef enum
{
    DRD_RUNTIME_MODE_USER = 0,
    DRD_RUNTIME_MODE_SYSTEM,
    DRD_RUNTIME_MODE_HANDOVER,
} DrdRuntimeMode;

DrdConfig *drd_config_new(void);
DrdConfig *drd_config_new_from_file(const gchar *path, GError **error);

gboolean drd_config_merge_cli(DrdConfig *self,
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
                               GError **error);

const gchar *drd_config_get_bind_address(DrdConfig *self);
guint16 drd_config_get_port(DrdConfig *self);
const gchar *drd_config_get_certificate_path(DrdConfig *self);
const gchar *drd_config_get_private_key_path(DrdConfig *self);
const gchar *drd_config_get_nla_username(DrdConfig *self);
const gchar *drd_config_get_nla_password(DrdConfig *self);
gboolean drd_config_is_nla_enabled(DrdConfig *self);
DrdRuntimeMode drd_config_get_runtime_mode(DrdConfig *self);
const gchar *drd_config_get_pam_service(DrdConfig *self);
guint drd_config_get_capture_width(DrdConfig *self);
guint drd_config_get_capture_height(DrdConfig *self);
const DrdEncodingOptions *drd_config_get_encoding_options(DrdConfig *self);

G_END_DECLS
