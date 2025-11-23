#include "security/drd_tls_credentials.h"

#include <gio/gio.h>

#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/settings.h>

struct _DrdTlsCredentials
{
    GObject parent_instance;

    gchar *certificate_path;
    gchar *private_key_path;
    gchar *certificate_pem;
    gchar *private_key_pem;
    rdpCertificate *certificate;
    rdpPrivateKey *private_key;
};

G_DEFINE_TYPE(DrdTlsCredentials, drd_tls_credentials, G_TYPE_OBJECT)

static void
drd_tls_credentials_dispose(GObject *object)
{
    DrdTlsCredentials *self = DRD_TLS_CREDENTIALS(object);

    if (self->certificate != NULL)
    {
        freerdp_certificate_free(self->certificate);
        self->certificate = NULL;
    }

    if (self->private_key != NULL)
    {
        freerdp_key_free(self->private_key);
        self->private_key = NULL;
    }

    g_clear_pointer(&self->certificate_path, g_free);
    g_clear_pointer(&self->private_key_path, g_free);
    g_clear_pointer(&self->certificate_pem, g_free);
    g_clear_pointer(&self->private_key_pem, g_free);

    G_OBJECT_CLASS(drd_tls_credentials_parent_class)->dispose(object);
}

static void
drd_tls_credentials_class_init(DrdTlsCredentialsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_tls_credentials_dispose;
}

static void
drd_tls_credentials_init(DrdTlsCredentials *self)
{
    self->certificate_path = NULL;
    self->private_key_path = NULL;
    self->certificate_pem = NULL;
    self->private_key_pem = NULL;
    self->certificate = NULL;
    self->private_key = NULL;
}

static gboolean
drd_tls_credentials_apply_pem(DrdTlsCredentials *self,
                               const gchar *certificate_pem,
                               const gchar *private_key_pem,
                               GError **error)
{
    g_return_val_if_fail(certificate_pem != NULL, FALSE);
    g_return_val_if_fail(private_key_pem != NULL, FALSE);

    rdpCertificate *certificate = freerdp_certificate_new_from_pem(certificate_pem);
    if (certificate == NULL)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to parse TLS certificate material");
        return FALSE;
    }

    rdpPrivateKey *key = freerdp_key_new_from_pem(private_key_pem);
    if (key == NULL)
    {
        freerdp_certificate_free(certificate);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to parse TLS private key material");
        return FALSE;
    }

    if (self->certificate != NULL)
    {
        freerdp_certificate_free(self->certificate);
    }
    if (self->private_key != NULL)
    {
        freerdp_key_free(self->private_key);
    }
    g_free(self->certificate_pem);
    g_free(self->private_key_pem);

    self->certificate = certificate;
    self->private_key = key;
    self->certificate_pem = g_strdup(certificate_pem);
    self->private_key_pem = g_strdup(private_key_pem);
    return TRUE;
}

/* 读取 PEM 证书与私钥，失败时通过 GError 传递细节，方便上层统一记录。 */
static gboolean
drd_tls_credentials_load(DrdTlsCredentials *self, const gchar *certificate_path, const gchar *private_key_path, GError **error)
{
    g_autofree gchar *cert_data = NULL;
    if (!g_file_get_contents(certificate_path, &cert_data, NULL, error))
    {
        return FALSE;
    }

    g_autofree gchar *key_data = NULL;
    if (!g_file_get_contents(private_key_path, &key_data, NULL, error))
    {
        return FALSE;
    }

    if (!drd_tls_credentials_apply_pem(self, cert_data, key_data, error))
    {
        g_prefix_error(error, "Failed to load TLS material from files: ");
        return FALSE;
    }

    return TRUE;
}

DrdTlsCredentials *
drd_tls_credentials_new(const gchar *certificate_path, const gchar *private_key_path, GError **error)
{
    g_return_val_if_fail(certificate_path != NULL, NULL);
    g_return_val_if_fail(private_key_path != NULL, NULL);

    DrdTlsCredentials *self = g_object_new(DRD_TYPE_TLS_CREDENTIALS, NULL);
    self->certificate_path = g_strdup(certificate_path);
    self->private_key_path = g_strdup(private_key_path);

    if (!drd_tls_credentials_load(self, certificate_path, private_key_path, error))
    {
        g_clear_object(&self);
        return NULL;
    }

    return self;
}

const gchar *
drd_tls_credentials_get_certificate_path(DrdTlsCredentials *self)
{
    g_return_val_if_fail(DRD_IS_TLS_CREDENTIALS(self), NULL);
    return self->certificate_path;
}

const gchar *
drd_tls_credentials_get_private_key_path(DrdTlsCredentials *self)
{
    g_return_val_if_fail(DRD_IS_TLS_CREDENTIALS(self), NULL);
    return self->private_key_path;
}

rdpCertificate *
drd_tls_credentials_get_certificate(DrdTlsCredentials *self)
{
    g_return_val_if_fail(DRD_IS_TLS_CREDENTIALS(self), NULL);
    return self->certificate;
}

rdpPrivateKey *
drd_tls_credentials_get_private_key(DrdTlsCredentials *self)
{
    g_return_val_if_fail(DRD_IS_TLS_CREDENTIALS(self), NULL);
    return self->private_key;
}

gboolean
drd_tls_credentials_read_material(DrdTlsCredentials *self,
                                  gchar **certificate,
                                  gchar **key,
                                  GError **error)
{
    g_return_val_if_fail(DRD_IS_TLS_CREDENTIALS(self), FALSE);

    if (certificate != NULL)
    {
        if (self->certificate_pem == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "TLS certificate material unavailable");
            return FALSE;
        }
        *certificate = g_strdup(self->certificate_pem);
    }

    if (key != NULL)
    {
        if (self->private_key_pem == NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "TLS private key material unavailable");
            if (certificate != NULL)
            {
                g_free(*certificate);
                *certificate = NULL;
            }
            return FALSE;
        }
        *key = g_strdup(self->private_key_pem);
    }

    return TRUE;
}

gboolean
drd_tls_credentials_reload_from_pem(DrdTlsCredentials *self,
                                    const gchar *certificate_pem,
                                    const gchar *key_pem,
                                    GError **error)
{
    g_return_val_if_fail(DRD_IS_TLS_CREDENTIALS(self), FALSE);

    if (!drd_tls_credentials_apply_pem(self, certificate_pem, key_pem, error))
    {
        return FALSE;
    }

    return TRUE;
}

gboolean
drd_tls_credentials_apply(DrdTlsCredentials *self, rdpSettings *settings, GError **error)
{
    g_return_val_if_fail(DRD_IS_TLS_CREDENTIALS(self), FALSE);
    g_return_val_if_fail(settings != NULL, FALSE);

    if (!freerdp_settings_set_pointer(settings, FreeRDP_RdpServerCertificate, self->certificate))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to assign server certificate to settings");
        return FALSE;
    }

    if (!freerdp_settings_set_pointer(settings, FreeRDP_RdpServerRsaKey, self->private_key))
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to assign private key to settings");
        return FALSE;
    }

    return TRUE;
}
