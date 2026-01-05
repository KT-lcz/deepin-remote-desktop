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

#define DRD_H264_DEFAULT_BITRATE 5000000
#define DRD_H264_DEFAULT_FRAMERATE 60
#define DRD_H264_DEFAULT_QP 15

#define DRD_GFX_DEFAULT_LARGE_CHANGE_THRESHOLD 0.05
#define DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_INTERVAL 6
#define DRD_GFX_DEFAULT_PROGRESSIVE_REFRESH_TIMEOUT_MS 100

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
    guint h264_bitrate;
    guint h264_framerate;
    guint h264_qp;
    gdouble gfx_large_change_threshold;
    guint gfx_progressive_refresh_interval;
    guint gfx_progressive_refresh_timeout_ms;
} DrdEncodingOptions;

G_END_DECLS
