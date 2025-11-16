#pragma once

#include <glib.h>

typedef struct _DrdLocalSession DrdLocalSession;

DrdLocalSession *drd_local_session_new(const gchar *pam_service,
                                       const gchar *username,
                                       const gchar *domain,
                                       const gchar *password,
                                       const gchar *remote_host,
                                       GError **error);
void drd_local_session_close(DrdLocalSession *session);
