#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
    DRD_ENCODING_MODE_RAW = 0,
    DRD_ENCODING_MODE_RFX,
    DRD_ENCODING_MODE_H264,
    DRD_ENCODING_MODE_AUTO
} DrdEncodingMode;

static inline const gchar *
drd_encoding_mode_to_string(DrdEncodingMode mode)
{
    switch (mode)
    {
        case DRD_ENCODING_MODE_RAW:
            return "raw";
        case DRD_ENCODING_MODE_RFX:
            return "rfx";
        case DRD_ENCODING_MODE_H264:
            return "h264";
        case DRD_ENCODING_MODE_AUTO:
            return "auto";
        default:
            return "unknown";
    }
}

typedef struct
{
    guint width;
    guint height;
    DrdEncodingMode mode;
    gboolean enable_frame_diff;
} DrdEncodingOptions;

G_END_DECLS
