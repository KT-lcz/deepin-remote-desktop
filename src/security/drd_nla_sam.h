#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _DrdNlaSamFile DrdNlaSamFile;

DrdNlaSamFile *drd_nla_sam_file_new(const gchar *username,
                                      const gchar *nt_hash_hex,
                                      GError **error);
const gchar *drd_nla_sam_file_get_path(DrdNlaSamFile *sam_file);
void drd_nla_sam_file_free(DrdNlaSamFile *sam_file);
gchar *drd_nla_sam_hash_password(const gchar *password);

G_END_DECLS
