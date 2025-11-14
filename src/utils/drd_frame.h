#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define DRD_TYPE_FRAME (drd_frame_get_type())
G_DECLARE_FINAL_TYPE(DrdFrame, drd_frame, DRD, FRAME, GObject)

DrdFrame *drd_frame_new(void);

void drd_frame_configure(DrdFrame *self,
                          guint width,
                          guint height,
                          guint stride,
                          guint64 timestamp);

guint drd_frame_get_width(DrdFrame *self);
guint drd_frame_get_height(DrdFrame *self);
guint drd_frame_get_stride(DrdFrame *self);
guint64 drd_frame_get_timestamp(DrdFrame *self);

/**
 * drd_frame_ensure_capacity:
 * @self: the frame
 * @size: required byte size
 *
 * Ensures the internal storage has exactly @size bytes allocated.
 * Returns a pointer to the writable buffer for direct population.
 */
guint8 *drd_frame_ensure_capacity(DrdFrame *self, gsize size);

const guint8 *drd_frame_get_data(DrdFrame *self, gsize *size);

G_END_DECLS
