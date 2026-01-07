#pragma once

#include <glib-object.h>

#include "utils/drd_frame_queue.h"

G_BEGIN_DECLS

#define DRD_TYPE_X11_CAPTURE (drd_x11_capture_get_type())
G_DECLARE_FINAL_TYPE(DrdX11Capture, drd_x11_capture, DRD, X11_CAPTURE, GObject)

DrdX11Capture *drd_x11_capture_new(DrdFrameQueue *queue);

gboolean drd_x11_capture_start(DrdX11Capture *self, const gchar *display_name,
                               guint requested_width, guint requested_height,
                               GError **error);

void drd_x11_capture_stop(DrdX11Capture *self);
gboolean drd_x11_capture_is_running(DrdX11Capture *self);
gboolean drd_x11_capture_get_display_size(DrdX11Capture *self,
                                          const gchar *display_name,
                                          guint *out_width, guint *out_height,
                                          GError **error);

G_END_DECLS
