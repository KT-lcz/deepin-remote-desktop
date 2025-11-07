#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GrdcNlaSamFile GrdcNlaSamFile;

GrdcNlaSamFile *grdc_nla_sam_file_new(const gchar *username,
                                      const gchar *nt_hash_hex,
                                      GError **error);
const gchar *grdc_nla_sam_file_get_path(GrdcNlaSamFile *sam_file);
void grdc_nla_sam_file_free(GrdcNlaSamFile *sam_file);
gchar *grdc_nla_sam_hash_password(const gchar *password);

G_END_DECLS
