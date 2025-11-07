#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GrdcNlaSamFile GrdcNlaSamFile;

GrdcNlaSamFile *grdc_nla_sam_file_new(const gchar *username,
                                      const gchar *password,
                                      GError **error);
const gchar *grdc_nla_sam_file_get_path(GrdcNlaSamFile *sam_file);
void grdc_nla_sam_file_free(GrdcNlaSamFile *sam_file);

G_END_DECLS
