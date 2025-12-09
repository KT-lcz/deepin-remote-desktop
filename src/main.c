#include "core/drd_application.h"

#include <freerdp/channels/channels.h>
#include <winpr/ssl.h>
#include <winpr/wtsapi.h>
#include <freerdp/primitives.h>

static gboolean
drd_initialize_winpr(void)
{
    /*
     * 功能：初始化 WinPR 相关子系统（SSL 与 WTS）。
     * 逻辑：初始化 WinPR SSL；获取 WTS API 表并注册；任一步失败打印错误并返回 FALSE。
     * 参数：无。
     * 外部接口：WinPR winpr_InitializeSSL、FreeRDP_InitWtsApi/WTSRegisterWtsApiFunctionTable；GLib g_printerr 输出错误。
     */
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
    /*
     * 功能：程序入口，初始化 WinPR 并运行应用。
     * 逻辑：先初始化 WinPR；创建并运行 DrdApplication，失败时输出错误；返回运行状态码。
     * 参数：argc/argv 命令行参数。
     * 外部接口：WinPR/FreeRDP 初始化；drd_application_run 执行主逻辑；GLib g_printerr 输出错误。
     */
    if (!drd_initialize_winpr())
    {
        return 1;
    }
    primitives_get();
    g_autoptr(DrdApplication)
    app = drd_application_new();
    g_autoptr(GError)
    error = NULL;

    int status = drd_application_run(app, argc, argv, &error);
    if (status != 0 && error != NULL)
    {
        g_printerr("run error：%s\n", error->message);
    }

    return status;
}
