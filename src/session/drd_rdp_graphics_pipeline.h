#pragma once

#include <glib-object.h>

#include <freerdp/freerdp.h>
#include <winpr/wtypes.h>

#include "utils/drd_encoded_frame.h"

G_BEGIN_DECLS

#define DRD_TYPE_RDP_GRAPHICS_PIPELINE (drd_rdp_graphics_pipeline_get_type())
G_DECLARE_FINAL_TYPE(DrdRdpGraphicsPipeline,
                     drd_rdp_graphics_pipeline,
                     DRD,
                     RDP_GRAPHICS_PIPELINE,
                     GObject)

DrdRdpGraphicsPipeline *drd_rdp_graphics_pipeline_new(freerdp_peer *peer,
                                                       HANDLE vcm,
                                                       guint16 surface_width,
                                                       guint16 surface_height);

gboolean drd_rdp_graphics_pipeline_maybe_init(DrdRdpGraphicsPipeline *self);
gboolean drd_rdp_graphics_pipeline_is_ready(DrdRdpGraphicsPipeline *self);
gboolean drd_rdp_graphics_pipeline_can_submit(DrdRdpGraphicsPipeline *self);
gboolean drd_rdp_graphics_pipeline_submit_frame(DrdRdpGraphicsPipeline *self,
                                                DrdEncodedFrame *frame,
                                                GError **error);

G_END_DECLS
