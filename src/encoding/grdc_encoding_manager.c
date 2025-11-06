#include "encoding/grdc_encoding_manager.h"

#include <gio/gio.h>

#include "encoding/grdc_raw_encoder.h"
#include "encoding/grdc_rfx_encoder.h"

struct _GrdcEncodingManager
{
    GObject parent_instance;

    guint frame_width;
    guint frame_height;
    gboolean ready;
    GrdcEncodingMode mode;
    GrdcRawEncoder *raw_encoder;
    GrdcRfxEncoder *rfx_encoder;
    GrdcEncodedFrame *scratch_frame;
    gboolean enable_diff;
};

G_DEFINE_TYPE(GrdcEncodingManager, grdc_encoding_manager, G_TYPE_OBJECT)

static void
grdc_encoding_manager_dispose(GObject *object)
{
    GrdcEncodingManager *self = GRDC_ENCODING_MANAGER(object);
    grdc_encoding_manager_reset(self);
    g_clear_object(&self->raw_encoder);
    g_clear_object(&self->scratch_frame);
    G_OBJECT_CLASS(grdc_encoding_manager_parent_class)->dispose(object);
}

static void
grdc_encoding_manager_class_init(GrdcEncodingManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = grdc_encoding_manager_dispose;
}

static void
grdc_encoding_manager_init(GrdcEncodingManager *self)
{
    self->frame_width = 0;
    self->frame_height = 0;
    self->ready = FALSE;
    self->mode = GRDC_ENCODING_MODE_RAW;
    self->enable_diff = TRUE;
    self->raw_encoder = grdc_raw_encoder_new();
    self->rfx_encoder = grdc_rfx_encoder_new();
    self->scratch_frame = grdc_encoded_frame_new();
}

GrdcEncodingManager *
grdc_encoding_manager_new(void)
{
    return g_object_new(GRDC_TYPE_ENCODING_MANAGER, NULL);
}

gboolean
grdc_encoding_manager_prepare(GrdcEncodingManager *self,
                              const GrdcEncodingOptions *options,
                              GError **error)
{
    g_return_val_if_fail(GRDC_IS_ENCODING_MANAGER(self), FALSE);

    if (options->width == 0 || options->height == 0)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "Encoder resolution must be non-zero (width=%u height=%u)",
                    options->width,
                    options->height);
        return FALSE;
    }

    self->frame_width = options->width;
    self->frame_height = options->height;
    self->mode = options->mode;
    self->enable_diff = options->enable_frame_diff;

    gboolean ok = FALSE;
    gboolean raw_ok = grdc_raw_encoder_configure(self->raw_encoder,
                                                 options->width,
                                                 options->height,
                                                 (options->mode == GRDC_ENCODING_MODE_RAW) ? error : NULL);
    if (!raw_ok)
    {
        if (options->mode != GRDC_ENCODING_MODE_RAW && error != NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to configure raw encoder fallback");
        }
        grdc_encoding_manager_reset(self);
        return FALSE;
    }

    if (options->mode == GRDC_ENCODING_MODE_RAW)
    {
        ok = TRUE;
    }
    else
    {
        ok = grdc_rfx_encoder_configure(self->rfx_encoder,
                                        options->width,
                                        options->height,
                                        options->enable_frame_diff,
                                        error);
    }

    if (!ok)
    {
        grdc_encoding_manager_reset(self);
        return FALSE;
    }

    self->ready = TRUE;

    g_message("Encoding manager configured for %ux%u stream (mode=%s diff=%s)",
              options->width,
              options->height,
              options->mode == GRDC_ENCODING_MODE_RAW ? "raw" : "rfx",
              options->enable_frame_diff ? "on" : "off");
    return TRUE;
}

void
grdc_encoding_manager_reset(GrdcEncodingManager *self)
{
    g_return_if_fail(GRDC_IS_ENCODING_MANAGER(self));

    if (!self->ready)
    {
        return;
    }

    g_message("Encoding manager reset");
    self->frame_width = 0;
    self->frame_height = 0;
    self->mode = GRDC_ENCODING_MODE_RAW;
    self->enable_diff = TRUE;
    if (self->raw_encoder != NULL)
    {
        grdc_raw_encoder_reset(self->raw_encoder);
    }
    if (self->rfx_encoder != NULL)
    {
        grdc_rfx_encoder_reset(self->rfx_encoder);
    }
    self->ready = FALSE;
}

gboolean
grdc_encoding_manager_is_ready(GrdcEncodingManager *self)
{
    g_return_val_if_fail(GRDC_IS_ENCODING_MANAGER(self), FALSE);
    return self->ready;
}

gboolean
grdc_encoding_manager_encode(GrdcEncodingManager *self,
                              GrdcFrame *input,
                              gsize max_payload,
                              GrdcEncodedFrame **out_frame,
                              GError **error)
{
    g_return_val_if_fail(GRDC_IS_ENCODING_MANAGER(self), FALSE);
    g_return_val_if_fail(GRDC_IS_FRAME(input), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);

    if (!self->ready)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoding manager not prepared");
        return FALSE;
    }

    gboolean ok = FALSE;

    if (self->mode == GRDC_ENCODING_MODE_RAW)
    {
        ok = grdc_raw_encoder_encode(self->raw_encoder, input, self->scratch_frame, error);
    }
    else
    {
        ok = grdc_rfx_encoder_encode(self->rfx_encoder, input, self->scratch_frame, error);
        if (ok && max_payload > 0)
        {
            gsize payload_len = 0;
            grdc_encoded_frame_get_data(self->scratch_frame, &payload_len);
            if (payload_len > max_payload)
            {
                g_message("RFX payload %zu exceeds peer limit %zu, falling back to raw frame",
                          payload_len,
                          max_payload);
                ok = grdc_raw_encoder_encode(self->raw_encoder, input, self->scratch_frame, error);
            }
        }
    }

    if (!ok)
    {
        return FALSE;
    }

    *out_frame = g_object_ref(self->scratch_frame);
    return TRUE;
}

GrdcFrameCodec
grdc_encoding_manager_get_codec(GrdcEncodingManager *self)
{
    g_return_val_if_fail(GRDC_IS_ENCODING_MANAGER(self), GRDC_FRAME_CODEC_RAW);
    return (self->mode == GRDC_ENCODING_MODE_RAW) ? GRDC_FRAME_CODEC_RAW : GRDC_FRAME_CODEC_RFX;
}

void
grdc_encoding_manager_force_keyframe(GrdcEncodingManager *self)
{
    g_return_if_fail(GRDC_IS_ENCODING_MANAGER(self));
    if (self->mode == GRDC_ENCODING_MODE_RFX && self->rfx_encoder != NULL)
    {
        grdc_rfx_encoder_force_keyframe(self->rfx_encoder);
    }
}
