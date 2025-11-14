#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
    DRD_ENCODING_MODE_RAW,
    DRD_ENCODING_MODE_RFX
} DrdEncodingMode;

typedef struct
{
    guint width;
    guint height;
    DrdEncodingMode mode;
    gboolean enable_frame_diff;
} DrdEncodingOptions;

G_END_DECLS
