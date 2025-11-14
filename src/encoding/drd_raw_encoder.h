#pragma once

#include <glib-object.h>

#include "utils/drd_frame.h"
#include "utils/drd_encoded_frame.h"

G_BEGIN_DECLS

#define DRD_TYPE_RAW_ENCODER (drd_raw_encoder_get_type())
G_DECLARE_FINAL_TYPE(DrdRawEncoder, drd_raw_encoder, DRD, RAW_ENCODER, GObject)

DrdRawEncoder *drd_raw_encoder_new(void);

gboolean drd_raw_encoder_configure(DrdRawEncoder *self, guint width, guint height, GError **error);
void drd_raw_encoder_reset(DrdRawEncoder *self);

gboolean drd_raw_encoder_encode(DrdRawEncoder *self,
                                 DrdFrame *input,
                                 DrdEncodedFrame *output,
                                 GError **error);

G_END_DECLS
