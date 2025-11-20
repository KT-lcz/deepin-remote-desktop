#pragma once

#include <gio/gio.h>

#include "core/drd_config.h"

typedef struct _DrdServerRuntime DrdServerRuntime;

G_BEGIN_DECLS

#define DRD_TYPE_RDP_LISTENER (drd_rdp_listener_get_type())
G_DECLARE_FINAL_TYPE(DrdRdpListener, drd_rdp_listener, DRD, RDP_LISTENER, GSocketService)

typedef struct _DrdRdpSession DrdRdpSession;

typedef gboolean (*DrdRdpListenerDelegateFunc)(DrdRdpListener *listener,
                                               GSocketConnection *connection,
                                               gpointer user_data,
                                               GError **error);
typedef void (*DrdRdpListenerSessionFunc)(DrdRdpListener *listener,
                                          DrdRdpSession *session,
                                          GSocketConnection *connection,
                                          gpointer user_data);

DrdRdpListener *drd_rdp_listener_new(const gchar *bind_address,
                                     guint16 port,
                                     DrdServerRuntime *runtime,
                                     const DrdEncodingOptions *encoding_options,
                                     gboolean nla_enabled,
                                     const gchar *nla_username,
                                     const gchar *nla_password,
                                     const gchar *pam_service,
                                     gboolean system_mode);
gboolean drd_rdp_listener_start(DrdRdpListener *self, GError **error);
void drd_rdp_listener_stop(DrdRdpListener *self);
DrdServerRuntime *drd_rdp_listener_get_runtime(DrdRdpListener *self);
void drd_rdp_listener_set_delegate(DrdRdpListener *self,
                                   DrdRdpListenerDelegateFunc func,
                                   gpointer user_data);
gboolean drd_rdp_listener_adopt_connection(DrdRdpListener *self,
                                           GSocketConnection *connection,
                                           GError **error);
void drd_rdp_listener_set_session_callback(DrdRdpListener *self,
                                           DrdRdpListenerSessionFunc func,
                                           gpointer user_data);

G_END_DECLS
