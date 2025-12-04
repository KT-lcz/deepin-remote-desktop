#pragma once

#include <glib-object.h>

#include "utils/drd_frame.h"

#define DRD_FRAME_QUEUE_MAX_FRAMES 3

G_BEGIN_DECLS

#define DRD_TYPE_FRAME_QUEUE (drd_frame_queue_get_type())
G_DECLARE_FINAL_TYPE(DrdFrameQueue, drd_frame_queue, DRD, FRAME_QUEUE, GObject)

DrdFrameQueue *drd_frame_queue_new(void);

void drd_frame_queue_reset(DrdFrameQueue *self);
void drd_frame_queue_push(DrdFrameQueue *self, DrdFrame *frame);
gboolean drd_frame_queue_wait(DrdFrameQueue *self,
                               gint64 timeout_us,
                               DrdFrame **out_frame);
void drd_frame_queue_stop(DrdFrameQueue *self);
guint64 drd_frame_queue_get_dropped_frames(DrdFrameQueue *self);

G_END_DECLS
