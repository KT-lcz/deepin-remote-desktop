#include "core/drd_application.h"

#include <freerdp/channels/channels.h>
#include <winpr/ssl.h>
#include <winpr/wtsapi.h>
#include <freerdp/primitives.h>
static gboolean
drd_initialize_winpr(void)
{
    if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
    {
        g_printerr("WinPR SSL init failed\n");
        return FALSE;
    }

    const WtsApiFunctionTable *table = FreeRDP_InitWtsApi();
    if (table == NULL || !WTSRegisterWtsApiFunctionTable(table))
    {
        g_printerr("register WinPR WTS API failed\n");
        return FALSE;
    }

    return TRUE;
}

int
main(int argc, char **argv)
{
    if (!drd_initialize_winpr())
    {
        return 1;
    }
    primitives_get ();
    g_autoptr(DrdApplication) app = drd_application_new();
    g_autoptr(GError) error = NULL;

    int status = drd_application_run(app, argc, argv, &error);
    if (status != 0 && error != NULL)
    {
        g_printerr("run error：%s\n", error->message);
    }

    return status;
}
