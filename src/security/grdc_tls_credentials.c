#include "security/grdc_tls_credentials.h"

#include <gio/gio.h>

#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/settings.h>

struct _GrdcTlsCredentials
{
    GObject parent_instance;

    gchar *certificate_path;
    gchar *private_key_path;
    rdpCertificate *certificate;
    rdpPrivateKey *private_key;
};

G_DEFINE_TYPE(GrdcTlsCredentials, grdc_tls_credentials, G_TYPE_OBJECT)

static void
grdc_tls_credentials_dispose(GObject *object)
{
    GrdcTlsCredentials *self = GRDC_TLS_CREDENTIALS(object);

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

    G_OBJECT_CLASS(grdc_tls_credentials_parent_class)->dispose(object);
}

static void
grdc_tls_credentials_class_init(GrdcTlsCredentialsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = grdc_tls_credentials_dispose;
}

static void
grdc_tls_credentials_init(GrdcTlsCredentials *self)
{
    self->certificate_path = NULL;
    self->private_key_path = NULL;
    self->certificate = NULL;
    self->private_key = NULL;
}

/* 读取 PEM 证书与私钥，失败时通过 GError 传递细节，方便上层统一记录。 */
static gboolean
grdc_tls_credentials_load(GrdcTlsCredentials *self, const gchar *certificate_path, const gchar *private_key_path, GError **error)
{
    g_autofree gchar *cert_data = NULL;
    gsize cert_len = 0;
    if (!g_file_get_contents(certificate_path, &cert_data, &cert_len, error))
    {
        return FALSE;
    }

    self->certificate = freerdp_certificate_new_from_pem(cert_data);
    if (self->certificate == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Failed to parse certificate from '%s'",
                    certificate_path);
        return FALSE;
    }

    g_autofree gchar *key_data = NULL;
    gsize key_len = 0;
    if (!g_file_get_contents(private_key_path, &key_data, &key_len, error))
    {
        return FALSE;
    }

    self->private_key = freerdp_key_new_from_pem(key_data);
    if (self->private_key == NULL)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Failed to parse private key from '%s'",
                    private_key_path);
        return FALSE;
    }

    return TRUE;
}

GrdcTlsCredentials *
grdc_tls_credentials_new(const gchar *certificate_path, const gchar *private_key_path, GError **error)
{
    g_return_val_if_fail(certificate_path != NULL, NULL);
    g_return_val_if_fail(private_key_path != NULL, NULL);

    GrdcTlsCredentials *self = g_object_new(GRDC_TYPE_TLS_CREDENTIALS, NULL);
    self->certificate_path = g_strdup(certificate_path);
    self->private_key_path = g_strdup(private_key_path);

    if (!grdc_tls_credentials_load(self, certificate_path, private_key_path, error))
    {
        g_clear_object(&self);
        return NULL;
    }

    return self;
}

const gchar *
grdc_tls_credentials_get_certificate_path(GrdcTlsCredentials *self)
{
    g_return_val_if_fail(GRDC_IS_TLS_CREDENTIALS(self), NULL);
    return self->certificate_path;
}

const gchar *
grdc_tls_credentials_get_private_key_path(GrdcTlsCredentials *self)
{
    g_return_val_if_fail(GRDC_IS_TLS_CREDENTIALS(self), NULL);
    return self->private_key_path;
}

rdpCertificate *
grdc_tls_credentials_get_certificate(GrdcTlsCredentials *self)
{
    g_return_val_if_fail(GRDC_IS_TLS_CREDENTIALS(self), NULL);
    return self->certificate;
}

rdpPrivateKey *
grdc_tls_credentials_get_private_key(GrdcTlsCredentials *self)
{
    g_return_val_if_fail(GRDC_IS_TLS_CREDENTIALS(self), NULL);
    return self->private_key;
}

gboolean
grdc_tls_credentials_apply(GrdcTlsCredentials *self, rdpSettings *settings, GError **error)
{
    g_return_val_if_fail(GRDC_IS_TLS_CREDENTIALS(self), FALSE);
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

    freerdp_settings_set_bool(settings, FreeRDP_ServerMode, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);

    return TRUE;
}
