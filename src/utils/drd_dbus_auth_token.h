#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * AuthToken/OneTimeAuthToken is a DBus string containing INI payload:
 *
 *   [auth]
 *   username=...
 *   password=...
 */

gboolean drd_dbus_auth_token_parse(const gchar *token, gchar **out_username, gchar **out_password, GError **error);

gchar *drd_dbus_auth_token_build(const gchar *username, const gchar *password);

void drd_dbus_auth_token_secure_free(gchar **inout_secret);

G_END_DECLS
