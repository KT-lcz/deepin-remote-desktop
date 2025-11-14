#include "input/drd_input_dispatcher.h"

#include <gio/gio.h>

#include "input/drd_x11_input.h"

struct _DrdInputDispatcher
{
    GObject parent_instance;

    DrdX11Input *backend;
    gboolean active;
};

G_DEFINE_TYPE(DrdInputDispatcher, drd_input_dispatcher, G_TYPE_OBJECT)

static void
drd_input_dispatcher_dispose(GObject *object)
{
    DrdInputDispatcher *self = DRD_INPUT_DISPATCHER(object);
    drd_input_dispatcher_stop(self);
    g_clear_object(&self->backend);

    G_OBJECT_CLASS(drd_input_dispatcher_parent_class)->dispose(object);
}

static void
drd_input_dispatcher_class_init(DrdInputDispatcherClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = drd_input_dispatcher_dispose;
}

static void
drd_input_dispatcher_init(DrdInputDispatcher *self)
{
    self->backend = drd_x11_input_new();
    self->active = FALSE;
}

DrdInputDispatcher *
drd_input_dispatcher_new(void)
{
    return g_object_new(DRD_TYPE_INPUT_DISPATCHER, NULL);
}

gboolean
drd_input_dispatcher_start(DrdInputDispatcher *self, guint width, guint height, GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);

    if (!drd_x11_input_start(self->backend, error))
    {
        return FALSE;
    }

    drd_x11_input_update_desktop_size(self->backend, width, height);
    self->active = TRUE;
    return TRUE;
}

void
drd_input_dispatcher_stop(DrdInputDispatcher *self)
{
    g_return_if_fail(DRD_IS_INPUT_DISPATCHER(self));
    drd_x11_input_stop(self->backend);
    self->active = FALSE;
}

void
drd_input_dispatcher_update_desktop_size(DrdInputDispatcher *self, guint width, guint height)
{
    g_return_if_fail(DRD_IS_INPUT_DISPATCHER(self));
    drd_x11_input_update_desktop_size(self->backend, width, height);
}

gboolean
drd_input_dispatcher_handle_keyboard(DrdInputDispatcher *self,
                                      guint16 flags,
                                      guint8 scancode,
                                      GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);
    return drd_x11_input_inject_keyboard(self->backend, flags, scancode, error);
}

gboolean
drd_input_dispatcher_handle_unicode(DrdInputDispatcher *self,
                                     guint16 flags,
                                     guint16 codepoint,
                                     GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);
    return drd_x11_input_inject_unicode(self->backend, flags, codepoint, error);
}

gboolean
drd_input_dispatcher_handle_pointer(DrdInputDispatcher *self,
                                     guint16 flags,
                                     guint16 x,
                                     guint16 y,
                                     GError **error)
{
    g_return_val_if_fail(DRD_IS_INPUT_DISPATCHER(self), FALSE);
    return drd_x11_input_inject_pointer(self->backend, flags, x, y, error);
}

void
drd_input_dispatcher_flush(DrdInputDispatcher *self)
{
    (void)self;
    /* No buffered events at the moment; maintained for API symmetry. */
}
