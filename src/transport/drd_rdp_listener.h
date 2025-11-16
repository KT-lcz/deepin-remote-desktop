#pragma once

#include <glib-object.h>

#include "core/drd_config.h"

typedef struct _DrdServerRuntime DrdServerRuntime;

G_BEGIN_DECLS

#define DRD_TYPE_RDP_LISTENER (drd_rdp_listener_get_type())
G_DECLARE_FINAL_TYPE(DrdRdpListener, drd_rdp_listener, DRD, RDP_LISTENER, GObject)

DrdRdpListener *drd_rdp_listener_new(const gchar *bind_address,
                                     guint16 port,
                                     DrdServerRuntime *runtime,
                                     const DrdEncodingOptions *encoding_options,
                                     DrdNlaMode nla_mode,
                                     const gchar *nla_username,
                                     const gchar *nla_password,
                                     const gchar *pam_service,
                                     gboolean system_mode,
                                     gboolean rdp_sso_enabled);
gboolean drd_rdp_listener_start(DrdRdpListener *self, GError **error);
void drd_rdp_listener_stop(DrdRdpListener *self);
DrdServerRuntime *drd_rdp_listener_get_runtime(DrdRdpListener *self);

G_END_DECLS
