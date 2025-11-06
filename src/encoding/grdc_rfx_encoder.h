#pragma once

#include <glib-object.h>

#include "utils/grdc_frame.h"
#include "utils/grdc_encoded_frame.h"

G_BEGIN_DECLS

typedef enum
{
    GRDC_RFX_QUALITY_HIGH,
    GRDC_RFX_QUALITY_MEDIUM,
    GRDC_RFX_QUALITY_LOW
} GrdcRfxQuality;

#define GRDC_TYPE_RFX_ENCODER (grdc_rfx_encoder_get_type())
G_DECLARE_FINAL_TYPE(GrdcRfxEncoder, grdc_rfx_encoder, GRDC, RFX_ENCODER, GObject)

GrdcRfxEncoder *grdc_rfx_encoder_new(void);

gboolean grdc_rfx_encoder_configure(GrdcRfxEncoder *self,
                                    guint width,
                                    guint height,
                                    gboolean enable_diff,
                                    GrdcRfxQuality quality,
                                    GError **error);

void grdc_rfx_encoder_reset(GrdcRfxEncoder *self);

gboolean grdc_rfx_encoder_encode(GrdcRfxEncoder *self,
                                 GrdcFrame *frame,
                                 GrdcEncodedFrame *output,
                                 GError **error);
void grdc_rfx_encoder_force_keyframe(GrdcRfxEncoder *self);

G_END_DECLS
