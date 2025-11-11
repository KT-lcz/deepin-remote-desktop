#pragma once

#include <sys/types.h>
#include <stdio.h>
#include <freerdp/listener.h>
#include <winpr/wtypes.h>
#include <glib-object.h>

typedef struct _DrdServerRuntime DrdServerRuntime;

#include "utils/drd_encoded_frame.h"

G_BEGIN_DECLS

#define DRD_TYPE_RDP_SESSION (drd_rdp_session_get_type())
G_DECLARE_FINAL_TYPE(DrdRdpSession, drd_rdp_session, DRD, RDP_SESSION, GObject)

DrdRdpSession *drd_rdp_session_new(freerdp_peer *peer);
void drd_rdp_session_set_peer_state(DrdRdpSession *self, const gchar *state);
void drd_rdp_session_set_runtime(DrdRdpSession *self, DrdServerRuntime *runtime);
void drd_rdp_session_set_virtual_channel_manager(DrdRdpSession *self, HANDLE vcm);
BOOL drd_rdp_session_post_connect(DrdRdpSession *self);
BOOL drd_rdp_session_activate(DrdRdpSession *self);
BOOL drd_rdp_session_pump(DrdRdpSession *self);
void drd_rdp_session_disconnect(DrdRdpSession *self, const gchar *reason);
gboolean drd_rdp_session_start_event_thread(DrdRdpSession *self);
void drd_rdp_session_stop_event_thread(DrdRdpSession *self);

G_END_DECLS
