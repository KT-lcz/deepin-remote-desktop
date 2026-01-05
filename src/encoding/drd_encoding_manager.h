#pragma once

#include <glib-object.h>

#include <freerdp/freerdp.h>
#include <freerdp/server/rdpgfx.h>

#include "core/drd_encoding_options.h"
#include "utils/drd_frame.h"

G_BEGIN_DECLS

#define DRD_TYPE_ENCODING_MANAGER (drd_encoding_manager_get_type())
G_DECLARE_FINAL_TYPE(DrdEncodingManager, drd_encoding_manager, DRD, ENCODING_MANAGER, GObject)

typedef enum
{
    DRD_ENCODING_CODEC_CLASS_UNKNOWN = 0,
    DRD_ENCODING_CODEC_CLASS_AVC,
    DRD_ENCODING_CODEC_CLASS_NON_AVC
} DrdEncodingCodecClass;

DrdEncodingManager *drd_encoding_manager_new(void);
gboolean drd_encoding_manager_prepare(DrdEncodingManager *self,
                                       const DrdEncodingOptions *options,
                                       GError **error);
void drd_encoding_manager_reset(DrdEncodingManager *self);
gboolean drd_encoding_manager_refresh_interval_reached( DrdEncodingManager *self);
gboolean drd_encoding_manager_has_avc_to_non_avc_transition( DrdEncodingManager *self);
guint drd_encoding_manager_get_refresh_timeout_ms( DrdEncodingManager *self);

gboolean drd_encoding_manager_encode_surface_gfx(DrdEncodingManager *self,
                                                 rdpSettings *settings,
                                                 RdpgfxServerContext *context,
                                                 guint16 surface_id,
                                                 DrdFrame *input,
                                                 guint32 frame_id,
                                                 gboolean *h264,
                                                 gboolean auto_switch,
                                                 GError **error);
gboolean drd_encoding_manager_encode_cached_frame_gfx(DrdEncodingManager *self,
                                                     rdpSettings *settings,
                                                     RdpgfxServerContext *context,
                                                     guint16 surface_id,
                                                     guint32 frame_id,
                                                     gboolean *h264,
                                                     gboolean auto_switch,
                                                     GError **error);
gboolean drd_encoding_manager_encode_surface_bit(DrdEncodingManager *self,
                                                 rdpContext *context,
                                                 DrdFrame *input,
                                                 guint32 frame_id,
                                                 gsize max_payload,
                                                 GError **error);


void drd_encoding_manager_force_keyframe(DrdEncodingManager *self);
void drd_encoding_manager_register_codec_result(DrdEncodingManager *self,
                                                DrdEncodingCodecClass codec_class,
                                                gboolean keyframe_encode);
gboolean drd_encoder_prepare(DrdEncodingManager *encoder, guint32 codecs, rdpSettings *settings);

G_END_DECLS
