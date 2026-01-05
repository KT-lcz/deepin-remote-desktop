#pragma once

#include <glib-object.h>
#include <freerdp/freerdp.h>

G_BEGIN_DECLS

#define DRD_TYPE_TLS_CREDENTIALS (drd_tls_credentials_get_type())
G_DECLARE_FINAL_TYPE(DrdTlsCredentials, drd_tls_credentials, DRD, TLS_CREDENTIALS, GObject)

DrdTlsCredentials *drd_tls_credentials_new(const gchar *certificate_path,
                                             const gchar *private_key_path,
                                             GError **error);

DrdTlsCredentials *drd_tls_credentials_new_empty(void);

const gchar *drd_tls_credentials_get_certificate_path(DrdTlsCredentials *self);
const gchar *drd_tls_credentials_get_private_key_path(DrdTlsCredentials *self);

gboolean drd_tls_credentials_apply(DrdTlsCredentials *self, rdpSettings *settings, GError **error);

rdpCertificate *drd_tls_credentials_get_certificate(DrdTlsCredentials *self);
rdpPrivateKey *drd_tls_credentials_get_private_key(DrdTlsCredentials *self);

gboolean drd_tls_credentials_read_material(DrdTlsCredentials *self,
                                           gchar **certificate,
                                           gchar **key,
                                           GError **error);

gboolean drd_tls_credentials_reload_from_pem(DrdTlsCredentials *self,
                                             const gchar *certificate_pem,
                                             const gchar *key_pem,
                                             GError **error);

G_END_DECLS
