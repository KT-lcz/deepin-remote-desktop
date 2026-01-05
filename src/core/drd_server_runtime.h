#pragma once

#include <glib-object.h>

#include "capture/drd_capture_manager.h"
#include "core/drd_encoding_options.h"
#include "encoding/drd_encoding_manager.h"
#include "input/drd_input_dispatcher.h"
#include "security/drd_tls_credentials.h"

G_BEGIN_DECLS

#define DRD_TYPE_SERVER_RUNTIME (drd_server_runtime_get_type())
G_DECLARE_FINAL_TYPE(DrdServerRuntime, drd_server_runtime, DRD, SERVER_RUNTIME, GObject)

typedef enum
{
    DRD_FRAME_TRANSPORT_SURFACE_BITS = 0,
    DRD_FRAME_TRANSPORT_GRAPHICS_PIPELINE
} DrdFrameTransport;

DrdServerRuntime *drd_server_runtime_new(void);

DrdCaptureManager *drd_server_runtime_get_capture(DrdServerRuntime *self);
DrdEncodingManager *drd_server_runtime_get_encoder(DrdServerRuntime *self);
DrdInputDispatcher *drd_server_runtime_get_input(DrdServerRuntime *self);

gboolean drd_server_runtime_prepare_stream(DrdServerRuntime *self, const DrdEncodingOptions *encoding_options,
                                           GError **error);
void drd_server_runtime_stop(DrdServerRuntime *self);

gboolean drd_server_runtime_pull_encoded_frame_surface_gfx(DrdServerRuntime *self,
                                                           rdpSettings *settings,
                                                           RdpgfxServerContext *context,
                                                           guint16 surface_id,
                                                           gint64 timeout_us,
                                                           guint32 frame_id,
                                                           gboolean *h264,
                                                           GError **error);
gboolean drd_server_runtime_send_cached_frame_surface_gfx(DrdServerRuntime *self,
                                                          rdpSettings *settings,
                                                          RdpgfxServerContext *context,
                                                          guint16 surface_id,
                                                          guint32 frame_id,
                                                          gboolean *h264,
                                                          GError **error);
gboolean drd_server_runtime_pull_encoded_frame_surface_bit(DrdServerRuntime *self,
                                                           rdpContext *context,
                                                           guint32 frame_id,
                                                           gsize max_payload,
                                                           gint64 timeout_us,
                                                           GError **error);

void drd_server_runtime_set_transport(DrdServerRuntime *self, DrdFrameTransport transport);
DrdFrameTransport drd_server_runtime_get_transport(DrdServerRuntime *self);
gboolean drd_server_runtime_get_encoding_options(DrdServerRuntime *self, DrdEncodingOptions *out_options);
void drd_server_runtime_set_encoding_options(DrdServerRuntime *self, const DrdEncodingOptions *encoding_options);
gboolean drd_server_runtime_is_stream_running(DrdServerRuntime *self);
void drd_server_runtime_set_tls_credentials(DrdServerRuntime *self, DrdTlsCredentials *credentials);
DrdTlsCredentials *drd_server_runtime_get_tls_credentials(DrdServerRuntime *self);
void drd_server_runtime_request_keyframe(DrdServerRuntime *self);

gboolean drd_runtime_encoder_prepare(DrdServerRuntime *self, guint32 codecs, rdpSettings *settings);

G_END_DECLS
