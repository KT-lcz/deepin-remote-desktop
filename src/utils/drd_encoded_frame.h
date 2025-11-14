#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum
{
    DRD_FRAME_CODEC_RAW = 0,
    DRD_FRAME_CODEC_RFX,
    DRD_FRAME_CODEC_RFX_PROGRESSIVE,
    DRD_FRAME_CODEC_H264
} DrdFrameCodec;

#define DRD_TYPE_ENCODED_FRAME (drd_encoded_frame_get_type())
G_DECLARE_FINAL_TYPE(DrdEncodedFrame, drd_encoded_frame, DRD, ENCODED_FRAME, GObject)

DrdEncodedFrame *drd_encoded_frame_new(void);

void drd_encoded_frame_configure(DrdEncodedFrame *self,
                                  guint width,
                                  guint height,
                                  guint stride,
                                  gboolean is_bottom_up,
                                  guint64 timestamp,
                                  DrdFrameCodec codec);

void drd_encoded_frame_set_quality(DrdEncodedFrame *self, guint8 quality, guint8 qp, gboolean is_keyframe);

guint8 *drd_encoded_frame_ensure_capacity(DrdEncodedFrame *self, gsize size);
const guint8 *drd_encoded_frame_get_data(DrdEncodedFrame *self, gsize *size);

guint drd_encoded_frame_get_width(DrdEncodedFrame *self);
guint drd_encoded_frame_get_height(DrdEncodedFrame *self);
guint drd_encoded_frame_get_stride(DrdEncodedFrame *self);
gboolean drd_encoded_frame_get_is_bottom_up(DrdEncodedFrame *self);
guint64 drd_encoded_frame_get_timestamp(DrdEncodedFrame *self);
DrdFrameCodec drd_encoded_frame_get_codec(DrdEncodedFrame *self);
gboolean drd_encoded_frame_is_keyframe(DrdEncodedFrame *self);
guint8 drd_encoded_frame_get_quality(DrdEncodedFrame *self);
guint8 drd_encoded_frame_get_qp(DrdEncodedFrame *self);

G_END_DECLS
