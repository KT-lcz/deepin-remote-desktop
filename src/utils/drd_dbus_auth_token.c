
#include "utils/drd_dbus_auth_token.h"

#include <gio/gio.h>
#include <string.h>

static void drd_dbus_memzero(gchar *buffer, gsize len)
{
    if (buffer == NULL || len == 0)
    {
        return;
    }
    volatile gchar *p = buffer;
    while (len--)
    {
        *p++ = 0;
    }
}

void drd_dbus_auth_token_secure_free(gchar **inout_secret)
{
    if (inout_secret == NULL || *inout_secret == NULL)
    {
        return;
    }
    drd_dbus_memzero(*inout_secret, strlen(*inout_secret));
    g_clear_pointer(inout_secret, g_free);
}

gboolean drd_dbus_auth_token_parse(const gchar *token, gchar **out_username, gchar **out_password, GError **error)
{
    g_return_val_if_fail(out_username != NULL, FALSE);
    g_return_val_if_fail(out_password != NULL, FALSE);

    *out_username = NULL;
    *out_password = NULL;

    if (token == NULL || *token == '\0')
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "AuthToken is empty");
        return FALSE;
    }

    g_autoptr(GKeyFile) keyfile = g_key_file_new();
    if (!g_key_file_load_from_data(keyfile, token, (gsize) strlen(token), G_KEY_FILE_NONE, error))
    {
        return FALSE;
    }

    g_autofree gchar *username = g_key_file_get_string(keyfile, "auth", "username", error);
    if (username == NULL || *username == '\0')
    {
        return FALSE;
    }

    g_autofree gchar *password = g_key_file_get_string(keyfile, "auth", "password", error);
    if (password == NULL || *password == '\0')
    {
        return FALSE;
    }

    *out_username = g_steal_pointer(&username);
    *out_password = g_steal_pointer(&password);
    return TRUE;
}

gchar *drd_dbus_auth_token_build(const gchar *username, const gchar *password)
{
    g_return_val_if_fail(username != NULL && *username != '\0', NULL);
    g_return_val_if_fail(password != NULL && *password != '\0', NULL);

    g_autoptr(GKeyFile) keyfile = g_key_file_new();
    g_key_file_set_string(keyfile, "auth", "username", username);
    g_key_file_set_string(keyfile, "auth", "password", password);
    g_key_file_set_integer(keyfile, "meta", "format", 1);

    gsize len = 0;
    return g_key_file_to_data(keyfile, &len, NULL);
}
