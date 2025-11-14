#pragma once

#include <glib-object.h>

#include "utils/drd_frame.h"

G_BEGIN_DECLS

#define DRD_TYPE_X11_INPUT (drd_x11_input_get_type())
G_DECLARE_FINAL_TYPE(DrdX11Input, drd_x11_input, DRD, X11_INPUT, GObject)

DrdX11Input *drd_x11_input_new(void);

gboolean drd_x11_input_start(DrdX11Input *self, GError **error);
void drd_x11_input_stop(DrdX11Input *self);

void drd_x11_input_update_desktop_size(DrdX11Input *self, guint width, guint height);

gboolean drd_x11_input_inject_keyboard(DrdX11Input *self, guint16 flags, guint8 scancode, GError **error);
gboolean drd_x11_input_inject_unicode(DrdX11Input *self, guint16 flags, guint16 codepoint, GError **error);
gboolean drd_x11_input_inject_pointer(DrdX11Input *self,
                                       guint16 flags,
                                       guint16 x,
                                       guint16 y,
                                       GError **error);

G_END_DECLS
