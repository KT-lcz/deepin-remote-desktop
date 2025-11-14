#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define DRD_TYPE_APPLICATION (drd_application_get_type())
G_DECLARE_FINAL_TYPE(DrdApplication, drd_application, DRD, APPLICATION, GObject)

DrdApplication *drd_application_new(void);
int drd_application_run(DrdApplication *self, int argc, char **argv, GError **error);

G_END_DECLS
