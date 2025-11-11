#pragma once

#include <glib-object.h>

#include "utils/drd_frame.h"
#include "utils/drd_encoded_frame.h"
#include "core/drd_encoding_options.h"
#include "encoding/drd_rfx_encoder.h"

G_BEGIN_DECLS

#define DRD_TYPE_ENCODING_MANAGER (drd_encoding_manager_get_type())
G_DECLARE_FINAL_TYPE(DrdEncodingManager, drd_encoding_manager, DRD, ENCODING_MANAGER, GObject)

DrdEncodingManager *drd_encoding_manager_new(void);
gboolean drd_encoding_manager_prepare(DrdEncodingManager *self,
                                       const DrdEncodingOptions *options,
                                       GError **error);
void drd_encoding_manager_reset(DrdEncodingManager *self);
gboolean drd_encoding_manager_is_ready(DrdEncodingManager *self);
gboolean drd_encoding_manager_encode(DrdEncodingManager *self,
                                      DrdFrame *input,
                                      gsize max_payload,
                                      DrdFrameCodec desired_codec,
                                      DrdEncodedFrame **out_frame,
                                      GError **error);

DrdFrameCodec drd_encoding_manager_get_codec(DrdEncodingManager *self);
void drd_encoding_manager_force_keyframe(DrdEncodingManager *self);

G_END_DECLS
