#include "encoding/drd_encoding_manager.h"

#include <gio/gio.h>

#include "encoding/drd_raw_encoder.h"
#include "encoding/drd_rfx_encoder.h"
#include "utils/drd_log.h"

struct _DrdEncodingManager
{
    GObject parent_instance;

    guint frame_width;
    guint frame_height;
    gboolean ready;
    DrdEncodingMode mode;
    DrdRawEncoder *raw_encoder;
    DrdRfxEncoder *rfx_encoder;
    DrdEncodedFrame *scratch_frame;
    gboolean enable_diff;
};

G_DEFINE_TYPE(DrdEncodingManager, drd_encoding_manager, G_TYPE_OBJECT)

static void
drd_encoding_manager_dispose(GObject *object)
{
    DrdEncodingManager *self = DRD_ENCODING_MANAGER(object);
    drd_encoding_manager_reset(self);
    g_clear_object(&self->raw_encoder);
    g_clear_object(&self->scratch_frame);
    G_OBJECT_CLASS(drd_encoding_manager_parent_class)->dispose(object);
}

static void
drd_encoding_manager_class_init(DrdEncodingManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_encoding_manager_dispose;
}

static void
drd_encoding_manager_init(DrdEncodingManager *self)
{
    self->frame_width = 0;
    self->frame_height = 0;
    self->ready = FALSE;
    self->mode = DRD_ENCODING_MODE_RAW;
    self->enable_diff = TRUE;
    self->raw_encoder = drd_raw_encoder_new();
    self->rfx_encoder = drd_rfx_encoder_new();
    self->scratch_frame = drd_encoded_frame_new();
}

DrdEncodingManager *
drd_encoding_manager_new(void)
{
    return g_object_new(DRD_TYPE_ENCODING_MANAGER, NULL);
}

gboolean
drd_encoding_manager_prepare(DrdEncodingManager *self,
                              const DrdEncodingOptions *options,
                              GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);

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
    gboolean raw_ok = drd_raw_encoder_configure(self->raw_encoder,
                                                 options->width,
                                                 options->height,
                                                 (options->mode == DRD_ENCODING_MODE_RAW) ? error : NULL);
    if (!raw_ok)
    {
        if (options->mode != DRD_ENCODING_MODE_RAW && error != NULL)
        {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Failed to configure raw encoder fallback");
        }
        drd_encoding_manager_reset(self);
        return FALSE;
    }

    if (options->mode == DRD_ENCODING_MODE_RAW)
    {
        ok = TRUE;
    }
    else
    {
        ok = drd_rfx_encoder_configure(self->rfx_encoder,
                                        options->width,
                                        options->height,
                                        options->enable_frame_diff,
                                        error);
    }

    if (!ok)
    {
        drd_encoding_manager_reset(self);
        return FALSE;
    }

    self->ready = TRUE;

    DRD_LOG_MESSAGE("Encoding manager configured for %ux%u stream (mode=%s diff=%s)",
              options->width,
              options->height,
              options->mode == DRD_ENCODING_MODE_RAW ? "raw" : "rfx",
              options->enable_frame_diff ? "on" : "off");
    return TRUE;
}

void
drd_encoding_manager_reset(DrdEncodingManager *self)
{
    g_return_if_fail(DRD_IS_ENCODING_MANAGER(self));

    if (!self->ready)
    {
        return;
    }

    DRD_LOG_MESSAGE("Encoding manager reset");
    self->frame_width = 0;
    self->frame_height = 0;
    self->mode = DRD_ENCODING_MODE_RAW;
    self->enable_diff = TRUE;
    if (self->raw_encoder != NULL)
    {
        drd_raw_encoder_reset(self->raw_encoder);
    }
    if (self->rfx_encoder != NULL)
    {
        drd_rfx_encoder_reset(self->rfx_encoder);
    }
    self->ready = FALSE;
}

gboolean
drd_encoding_manager_is_ready(DrdEncodingManager *self)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);
    return self->ready;
}

gboolean
drd_encoding_manager_encode(DrdEncodingManager *self,
                              DrdFrame *input,
                              gsize max_payload,
                              DrdFrameCodec desired_codec,
                              DrdEncodedFrame **out_frame,
                              GError **error)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), FALSE);
    g_return_val_if_fail(DRD_IS_FRAME(input), FALSE);
    g_return_val_if_fail(out_frame != NULL, FALSE);

    if (!self->ready)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoding manager not prepared");
        return FALSE;
    }

    if (self->mode == DRD_ENCODING_MODE_RAW && desired_codec != DRD_FRAME_CODEC_RAW)
    {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Encoding manager configured for RAW output only");
        return FALSE;
    }

    gboolean ok = FALSE;

    switch (desired_codec)
    {
        case DRD_FRAME_CODEC_RAW:
            ok = drd_raw_encoder_encode(self->raw_encoder, input, self->scratch_frame, error);
            break;
        case DRD_FRAME_CODEC_RFX:
            ok = drd_rfx_encoder_encode(self->rfx_encoder,
                                        input,
                                        self->scratch_frame,
                                        DRD_RFX_ENCODER_KIND_SURFACE_BITS,
                                        error);
            if (ok && max_payload > 0)
            {
                gsize payload_len = 0;
                drd_encoded_frame_get_data(self->scratch_frame, &payload_len);
                if (payload_len > max_payload)
                {
                    DRD_LOG_MESSAGE("RFX payload %zu exceeds peer limit %zu, falling back to raw frame",
                              payload_len,
                              max_payload);
                    ok = drd_raw_encoder_encode(self->raw_encoder,
                                                input,
                                                self->scratch_frame,
                                                error);
                }
            }
            break;
        case DRD_FRAME_CODEC_RFX_PROGRESSIVE:
            ok = drd_rfx_encoder_encode(self->rfx_encoder,
                                        input,
                                        self->scratch_frame,
                                        DRD_RFX_ENCODER_KIND_PROGRESSIVE,
                                        error);
            break;
        default:
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Unsupported codec requested");
            ok = FALSE;
            break;
    }

    if (!ok)
    {
        return FALSE;
    }

    *out_frame = g_object_ref(self->scratch_frame);
    return TRUE;
}

DrdFrameCodec
drd_encoding_manager_get_codec(DrdEncodingManager *self)
{
    g_return_val_if_fail(DRD_IS_ENCODING_MANAGER(self), DRD_FRAME_CODEC_RAW);
    return (self->mode == DRD_ENCODING_MODE_RAW) ? DRD_FRAME_CODEC_RAW : DRD_FRAME_CODEC_RFX;
}

void
drd_encoding_manager_force_keyframe(DrdEncodingManager *self)
{
    g_return_if_fail(DRD_IS_ENCODING_MANAGER(self));
    if (self->mode == DRD_ENCODING_MODE_RFX && self->rfx_encoder != NULL)
    {
        drd_rfx_encoder_force_keyframe(self->rfx_encoder);
    }
}
