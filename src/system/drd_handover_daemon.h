#pragma once

#include <glib-object.h>

#include "core/drd_config.h"
#include "core/drd_server_runtime.h"
#include "security/drd_tls_credentials.h"

G_BEGIN_DECLS

#define DRD_TYPE_HANDOVER_DAEMON (drd_handover_daemon_get_type())
G_DECLARE_FINAL_TYPE(DrdHandoverDaemon, drd_handover_daemon, DRD, HANDOVER_DAEMON, GObject)

DrdHandoverDaemon *drd_handover_daemon_new(DrdConfig *config,
                                           DrdServerRuntime *runtime,
                                           DrdTlsCredentials *tls_credentials);

gboolean drd_handover_daemon_start(DrdHandoverDaemon *self, GError **error);
void drd_handover_daemon_stop(DrdHandoverDaemon *self);

G_END_DECLS
