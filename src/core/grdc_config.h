#pragma once

#include <glib-object.h>

#include "core/grdc_encoding_options.h"

G_BEGIN_DECLS

#define GRDC_TYPE_CONFIG (grdc_config_get_type())
G_DECLARE_FINAL_TYPE(GrdcConfig, grdc_config, GRDC, CONFIG, GObject)

GrdcConfig *grdc_config_new(void);
GrdcConfig *grdc_config_new_from_file(const gchar *path, GError **error);

gboolean grdc_config_merge_cli(GrdcConfig *self,
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
                               GError **error);

const gchar *grdc_config_get_bind_address(GrdcConfig *self);
guint16 grdc_config_get_port(GrdcConfig *self);
const gchar *grdc_config_get_certificate_path(GrdcConfig *self);
const gchar *grdc_config_get_private_key_path(GrdcConfig *self);
const gchar *grdc_config_get_nla_username(GrdcConfig *self);
const gchar *grdc_config_get_nla_password(GrdcConfig *self);
guint grdc_config_get_capture_width(GrdcConfig *self);
guint grdc_config_get_capture_height(GrdcConfig *self);
const GrdcEncodingOptions *grdc_config_get_encoding_options(GrdcConfig *self);

G_END_DECLS
