#pragma once

#include <glib-object.h>

typedef struct _GrdcServerRuntime GrdcServerRuntime;

G_BEGIN_DECLS

#define GRDC_TYPE_RDP_LISTENER (grdc_rdp_listener_get_type())
G_DECLARE_FINAL_TYPE(GrdcRdpListener, grdc_rdp_listener, GRDC, RDP_LISTENER, GObject)

GrdcRdpListener *grdc_rdp_listener_new(const gchar *bind_address,
                                       guint16 port,
                                       GrdcServerRuntime *runtime,
                                       const gchar *nla_username,
                                       const gchar *nla_password);
gboolean grdc_rdp_listener_start(GrdcRdpListener *self, GError **error);
void grdc_rdp_listener_stop(GrdcRdpListener *self);
GrdcServerRuntime *grdc_rdp_listener_get_runtime(GrdcRdpListener *self);

G_END_DECLS
