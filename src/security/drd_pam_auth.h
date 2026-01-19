#pragma once

#include <glib.h>

typedef struct _DrdPamAuth DrdPamAuth;

DrdPamAuth *drd_pam_auth_new(const gchar *pam_service,
                                       const gchar *username,
                                       const gchar *domain,
                                       const gchar *password,
                                       const gchar *remote_host);
void drd_pam_auth_auth(DrdPamAuth *self, GError **error);
const gchar *drd_pam_auth_get_username(DrdPamAuth *self);
const gchar *drd_pam_auth_get_password(DrdPamAuth *self);
void drd_pam_auth_clear_password(DrdPamAuth *self);
void drd_pam_auth_close(DrdPamAuth *session);
