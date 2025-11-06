#pragma once

#include <glib-object.h>

#include "capture/grdc_capture_manager.h"
#include "encoding/grdc_encoding_manager.h"
#include "input/grdc_input_dispatcher.h"
#include "security/grdc_tls_credentials.h"
#include "utils/grdc_encoded_frame.h"
#include "utils/grdc_frame.h"
#include "core/grdc_encoding_options.h"

G_BEGIN_DECLS

#define GRDC_TYPE_SERVER_RUNTIME (grdc_server_runtime_get_type())
G_DECLARE_FINAL_TYPE(GrdcServerRuntime, grdc_server_runtime, GRDC, SERVER_RUNTIME, GObject)

GrdcServerRuntime *grdc_server_runtime_new(void);

GrdcCaptureManager *grdc_server_runtime_get_capture(GrdcServerRuntime *self);
GrdcEncodingManager *grdc_server_runtime_get_encoder(GrdcServerRuntime *self);
GrdcInputDispatcher *grdc_server_runtime_get_input(GrdcServerRuntime *self);

gboolean grdc_server_runtime_prepare_stream(GrdcServerRuntime *self,
                                            const GrdcEncodingOptions *encoding_options,
                                            GError **error);
void grdc_server_runtime_stop(GrdcServerRuntime *self);
gboolean grdc_server_runtime_pull_encoded_frame(GrdcServerRuntime *self,
                                                gint64 timeout_us,
                                                GrdcEncodedFrame **out_frame,
                                                GError **error);

GrdcFrameCodec grdc_server_runtime_get_codec(GrdcServerRuntime *self);
gboolean grdc_server_runtime_get_encoding_options(GrdcServerRuntime *self,
                                                  GrdcEncodingOptions *out_options);
void grdc_server_runtime_set_tls_credentials(GrdcServerRuntime *self, GrdcTlsCredentials *credentials);
GrdcTlsCredentials *grdc_server_runtime_get_tls_credentials(GrdcServerRuntime *self);
void grdc_server_runtime_request_keyframe(GrdcServerRuntime *self);

G_END_DECLS
