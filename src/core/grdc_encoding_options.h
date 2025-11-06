#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
    GRDC_ENCODING_MODE_RAW,
    GRDC_ENCODING_MODE_RFX
} GrdcEncodingMode;

typedef struct
{
    guint width;
    guint height;
    GrdcEncodingMode mode;
    gboolean enable_frame_diff;
} GrdcEncodingOptions;

G_END_DECLS
