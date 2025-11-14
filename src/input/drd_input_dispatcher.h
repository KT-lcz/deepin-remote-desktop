#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define DRD_TYPE_INPUT_DISPATCHER (drd_input_dispatcher_get_type())
G_DECLARE_FINAL_TYPE(DrdInputDispatcher, drd_input_dispatcher, DRD, INPUT_DISPATCHER, GObject)

DrdInputDispatcher *drd_input_dispatcher_new(void);

gboolean drd_input_dispatcher_start(DrdInputDispatcher *self, guint width, guint height, GError **error);
void drd_input_dispatcher_stop(DrdInputDispatcher *self);
void drd_input_dispatcher_update_desktop_size(DrdInputDispatcher *self, guint width, guint height);

gboolean drd_input_dispatcher_handle_keyboard(DrdInputDispatcher *self,
                                               guint16 flags,
                                               guint8 scancode,
                                               GError **error);
gboolean drd_input_dispatcher_handle_unicode(DrdInputDispatcher *self,
                                              guint16 flags,
                                              guint16 codepoint,
                                              GError **error);
gboolean drd_input_dispatcher_handle_pointer(DrdInputDispatcher *self,
                                              guint16 flags,
                                              guint16 x,
                                              guint16 y,
                                              GError **error);

void drd_input_dispatcher_flush(DrdInputDispatcher *self);

G_END_DECLS
