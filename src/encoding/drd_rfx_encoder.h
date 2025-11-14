#pragma once

#include <glib-object.h>

#include "utils/drd_frame.h"
#include "utils/drd_encoded_frame.h"

G_BEGIN_DECLS

#define DRD_TYPE_RFX_ENCODER (drd_rfx_encoder_get_type())
G_DECLARE_FINAL_TYPE(DrdRfxEncoder, drd_rfx_encoder, DRD, RFX_ENCODER, GObject)

typedef enum
{
    DRD_RFX_ENCODER_KIND_SURFACE_BITS = 0,
    DRD_RFX_ENCODER_KIND_PROGRESSIVE
} DrdRfxEncoderKind;

DrdRfxEncoder *drd_rfx_encoder_new(void);

gboolean drd_rfx_encoder_configure(DrdRfxEncoder *self,
                                    guint width,
                                    guint height,
                                    gboolean enable_diff,
                                    GError **error);

void drd_rfx_encoder_reset(DrdRfxEncoder *self);

gboolean drd_rfx_encoder_encode(DrdRfxEncoder *self,
                                 DrdFrame *frame,
                                 DrdEncodedFrame *output,
                                 DrdRfxEncoderKind kind,
                                 GError **error);
void drd_rfx_encoder_force_keyframe(DrdRfxEncoder *self);

G_END_DECLS
