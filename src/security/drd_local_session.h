#pragma once

#include <glib.h>

typedef struct _DrdLocalSession DrdLocalSession;

DrdLocalSession *drd_local_session_new(const gchar *pam_service,
                                       const gchar *username,
                                       const gchar *domain,
                                       const gchar *password,
                                       const gchar *remote_host);
void drd_local_session_auth(DrdLocalSession *self, GError **error);
const gchar *drd_local_session_get_username(DrdLocalSession *self);
const gchar *drd_local_session_get_password(DrdLocalSession *self);
void drd_local_session_clear_password(DrdLocalSession *self);
void drd_local_session_close(DrdLocalSession *session);
