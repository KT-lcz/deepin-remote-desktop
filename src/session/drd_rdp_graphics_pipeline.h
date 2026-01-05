#pragma once

#include <glib-object.h>

#include <freerdp/freerdp.h>
#include <freerdp/server/rdpgfx.h>
#include <winpr/wtypes.h>

#include "core/drd_server_runtime.h"

#define DRD_RDP_GRAPHICS_PIPELINE_ERROR (drd_rdp_graphics_pipeline_error_quark())

typedef enum
{
    DRD_RDP_GRAPHICS_PIPELINE_ERROR_FAILED
} DrdRdpGraphicsPipelineError;

GQuark drd_rdp_graphics_pipeline_error_quark(void);

G_BEGIN_DECLS

#define DRD_TYPE_RDP_GRAPHICS_PIPELINE (drd_rdp_graphics_pipeline_get_type())
G_DECLARE_FINAL_TYPE(DrdRdpGraphicsPipeline,
                      drd_rdp_graphics_pipeline,
                     DRD,
                     RDP_GRAPHICS_PIPELINE,
                     GObject)

DrdRdpGraphicsPipeline *drd_rdp_graphics_pipeline_new(freerdp_peer *peer,
                                                       HANDLE vcm,
                                                       DrdServerRuntime *runtime,
                                                       guint16 surface_width,
                                                       guint16 surface_height);

gboolean drd_rdp_graphics_pipeline_maybe_init(DrdRdpGraphicsPipeline *self);
gboolean drd_rdp_graphics_pipeline_is_ready(DrdRdpGraphicsPipeline *self);
gboolean drd_rdp_graphics_pipeline_can_submit(DrdRdpGraphicsPipeline *self);
gboolean drd_rdp_graphics_pipeline_wait_for_capacity(DrdRdpGraphicsPipeline *self,
                                                     gint64 timeout_us);
guint16 drd_rdp_graphics_pipeline_get_surface_id(DrdRdpGraphicsPipeline *self);

void drd_rdp_graphics_pipeline_out_frame_change(DrdRdpGraphicsPipeline *self,gboolean add);

RdpgfxServerContext* drd_rdpgfx_get_context(DrdRdpGraphicsPipeline *self);
void drd_rdp_graphics_pipeline_set_last_frame_mode(DrdRdpGraphicsPipeline *self,gboolean h264);
G_END_DECLS
