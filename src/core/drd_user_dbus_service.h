#pragma once

#include <glib-object.h>

#include "core/drd_config.h"

G_BEGIN_DECLS

#define DRD_TYPE_USER_DBUS_SERVICE (drd_user_dbus_service_get_type())
G_DECLARE_FINAL_TYPE(DrdUserDbusService, drd_user_dbus_service, DRD, USER_DBUS_SERVICE, GObject)

DrdUserDbusService *drd_user_dbus_service_new(DrdConfig *config);

gboolean drd_user_dbus_service_start(DrdUserDbusService *self, GError **error);

void drd_user_dbus_service_stop(DrdUserDbusService *self);

G_END_DECLS
