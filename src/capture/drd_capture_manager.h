#pragma once

#include <glib-object.h>

#include "utils/drd_frame_queue.h"
#include "utils/drd_frame.h"

G_BEGIN_DECLS

#define DRD_TYPE_CAPTURE_MANAGER (drd_capture_manager_get_type())
G_DECLARE_FINAL_TYPE(DrdCaptureManager, drd_capture_manager, DRD, CAPTURE_MANAGER, GObject)

DrdCaptureManager *drd_capture_manager_new(void);
gboolean drd_capture_manager_start(DrdCaptureManager *self, guint width,
                                   guint height, GError **error);
void drd_capture_manager_stop(DrdCaptureManager *self);
gboolean drd_capture_manager_is_running(DrdCaptureManager *self);
gboolean drd_capture_manager_get_display_size(DrdCaptureManager *self,
                                              guint *out_width,
                                              guint *out_height,
                                              GError **error);
DrdFrameQueue *drd_capture_manager_get_queue(DrdCaptureManager *self);
gboolean drd_capture_manager_wait_frame(DrdCaptureManager *self,
                                        gint64 timeout_us, DrdFrame **out_frame,
                                        GError **error);

G_END_DECLS
