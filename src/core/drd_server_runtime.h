#pragma once

#include <glib-object.h>

#include "capture/drd_capture_manager.h"
#include "encoding/drd_encoding_manager.h"
#include "input/drd_input_dispatcher.h"
#include "security/drd_tls_credentials.h"
#include "utils/drd_encoded_frame.h"
#include "utils/drd_frame.h"
#include "core/drd_encoding_options.h"

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

gboolean drd_server_runtime_prepare_stream(DrdServerRuntime *self,
                                            const DrdEncodingOptions *encoding_options,
                                            GError **error);
void drd_server_runtime_stop(DrdServerRuntime *self);
gboolean drd_server_runtime_pull_encoded_frame(DrdServerRuntime *self,
                                                gint64 timeout_us,
                                                DrdEncodedFrame **out_frame,
                                                GError **error);

void drd_server_runtime_set_transport(DrdServerRuntime *self, DrdFrameTransport transport);
DrdFrameTransport drd_server_runtime_get_transport(DrdServerRuntime *self);
DrdFrameCodec drd_server_runtime_get_codec(DrdServerRuntime *self);
gboolean drd_server_runtime_get_encoding_options(DrdServerRuntime *self,
                                                  DrdEncodingOptions *out_options);
void drd_server_runtime_set_encoding_options(DrdServerRuntime *self,
                                              const DrdEncodingOptions *encoding_options);
gboolean drd_server_runtime_is_stream_running(DrdServerRuntime *self);
void drd_server_runtime_set_tls_credentials(DrdServerRuntime *self, DrdTlsCredentials *credentials);
DrdTlsCredentials *drd_server_runtime_get_tls_credentials(DrdServerRuntime *self);
void drd_server_runtime_request_keyframe(DrdServerRuntime *self);

G_END_DECLS
