#pragma once

#include <glib.h>
#include <glib-object.h>

#include "core/drd_config.h"
#include "core/drd_server_runtime.h"
#include "security/drd_tls_credentials.h"

G_BEGIN_DECLS

#define DRD_TYPE_SYSTEM_DAEMON (drd_system_daemon_get_type())
G_DECLARE_FINAL_TYPE(DrdSystemDaemon, drd_system_daemon, DRD, SYSTEM_DAEMON, GObject)

DrdSystemDaemon *drd_system_daemon_new(DrdConfig *config,
                                       DrdServerRuntime *runtime,
                                       DrdTlsCredentials *tls_credentials);

gboolean drd_system_daemon_start(DrdSystemDaemon *self, GError **error);
void drd_system_daemon_stop(DrdSystemDaemon *self);
gboolean drd_system_daemon_set_main_loop(DrdSystemDaemon *self, GMainLoop *loop);
guint drd_system_daemon_get_pending_client_count(DrdSystemDaemon *self);
guint drd_system_daemon_get_remote_client_count(DrdSystemDaemon *self);

G_END_DECLS
