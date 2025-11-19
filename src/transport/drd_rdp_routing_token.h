#pragma once

#include <gio/gio.h>

#include <freerdp/freerdp.h>

G_BEGIN_DECLS

typedef struct
{
    gboolean requested_rdstls;
    gchar *routing_token;
} DrdRoutingTokenInfo;

DrdRoutingTokenInfo *drd_routing_token_info_new(void);
void drd_routing_token_info_free(DrdRoutingTokenInfo *info);

gboolean drd_routing_token_peek(GSocketConnection *connection,
                                GCancellable *cancellable,
                                DrdRoutingTokenInfo *info,
                                GError **error);

#if GLIB_CHECK_VERSION(2, 44, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(DrdRoutingTokenInfo, drd_routing_token_info_free)
#endif

G_END_DECLS
