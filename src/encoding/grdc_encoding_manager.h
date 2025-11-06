#pragma once

#include <glib-object.h>

#include "utils/grdc_frame.h"
#include "utils/grdc_encoded_frame.h"
#include "core/grdc_encoding_options.h"
#include "encoding/grdc_rfx_encoder.h"

G_BEGIN_DECLS

#define GRDC_TYPE_ENCODING_MANAGER (grdc_encoding_manager_get_type())
G_DECLARE_FINAL_TYPE(GrdcEncodingManager, grdc_encoding_manager, GRDC, ENCODING_MANAGER, GObject)

GrdcEncodingManager *grdc_encoding_manager_new(void);
gboolean grdc_encoding_manager_prepare(GrdcEncodingManager *self,
                                       const GrdcEncodingOptions *options,
                                       GError **error);
void grdc_encoding_manager_reset(GrdcEncodingManager *self);
gboolean grdc_encoding_manager_is_ready(GrdcEncodingManager *self);
gboolean grdc_encoding_manager_encode(GrdcEncodingManager *self,
                                      GrdcFrame *input,
                                      gsize max_payload,
                                      GrdcEncodedFrame **out_frame,
                                      GError **error);

GrdcFrameCodec grdc_encoding_manager_get_codec(GrdcEncodingManager *self);
void grdc_encoding_manager_force_keyframe(GrdcEncodingManager *self);

G_END_DECLS
