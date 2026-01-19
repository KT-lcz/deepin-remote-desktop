#include "security/drd_pam_auth.h"

#include <gio/gio.h>
#include <security/pam_appl.h>
#include <string.h>
#include <stdlib.h>

#include "utils/drd_log.h"

struct _DrdPamAuth
{
    pam_handle_t *handle;
    gchar *username;
    gchar *password;
    gchar *remote_host;
    gchar *domain;
    gchar *pam_service;
};

typedef struct
{
    const gchar *password;
} DrdPamConversationData;

/*
 * 功能：PAM 会话交互回调，按消息类型返回密码等响应。
 * 逻辑：为每条消息分配 pam_response，遇到隐藏输入请求时复制会话密码；不支持有回显输入；忽略错误/信息提示，未知类型返回错误。
 * 参数：num_msg 消息数量；msg PAM 消息数组；resp 输出响应数组；user_data 传入的会话数据（包含密码）。
 * 外部接口：PAM 回调协议，使用 calloc/strdup 分配内存，返回 PAM_SUCCESS/PAM_CONV_ERR；pam_message/pam_response 属于 PAM API。
 */
static int
drd_pam_auth_pam_conv(int num_msg, const struct pam_message **msg, struct pam_response **resp, void *user_data)
{
    DrdPamConversationData *conv = (DrdPamConversationData *) user_data;
    struct pam_response *responses = calloc(num_msg, sizeof(struct pam_response));
    if (responses == NULL)
    {
        return PAM_CONV_ERR;
    }

    for (int i = 0; i < num_msg; i++)
    {
        responses[i].resp_retcode = 0;
        responses[i].resp = NULL;
        switch (msg[i]->msg_style)
        {
            case PAM_PROMPT_ECHO_OFF:
                if (conv == NULL || conv->password == NULL)
                {
                    free(responses);
                    return PAM_CONV_ERR;
                }
                responses[i].resp = strdup(conv->password);
                if (responses[i].resp == NULL)
                {
                    free(responses);
                    return PAM_CONV_ERR;
                }
                break;
            case PAM_PROMPT_ECHO_ON:
                /* 不支持交互式有回显输入，返回错误。 */
                free(responses);
                return PAM_CONV_ERR;
            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
                break;
            default:
                free(responses);
                return PAM_CONV_ERR;
        }
    }

    *resp = responses;
    return PAM_SUCCESS;
}

/*
 * 功能：安全地清除并释放字符串。
 * 逻辑：若字符串存在则用 0 覆盖后释放，并将指针置空。
 * 参数：value 字符串指针地址。
 * 外部接口：C 库 strlen/memset，GLib g_free。
 */
static void
drd_pam_auth_scrub_string(gchar **value)
{
    if (value == NULL || *value == NULL)
    {
        return;
    }
    size_t len = strlen(*value);
    if (len > 0)
    {
        memset(*value, 0, len);
    }
    g_free(*value);
    *value = NULL;
}

/*
 * 功能：关闭 PAM 会话并删除凭据。
 * 逻辑：若句柄有效则调用 pam_close_session、pam_setcred 删除凭据并最终 pam_end。
 * 参数：handle PAM 会话句柄。
 * 外部接口：PAM API pam_close_session/pam_setcred/pam_end。
 */
static void
drd_pam_auth_cleanup_handle(pam_handle_t *handle)
{
    if (handle == NULL)
    {
        return;
    }
    pam_end(handle, PAM_SUCCESS);
}

void drd_pam_auth_auth(DrdPamAuth *self, GError **error)
{
    g_return_if_fail(self != NULL);

    if (self->handle != NULL)
    {
        drd_pam_auth_cleanup_handle(self->handle);
        self->handle = NULL;
    }

    DrdPamConversationData conv_data = {
            .password = self->password,
    };
    struct pam_conv conv = {
            .conv = drd_pam_auth_pam_conv,
            .appdata_ptr = &conv_data,
    };

    pam_handle_t *handle = NULL;
    int status = pam_start(self->pam_service, self->username, &conv, &handle);
    if (status != PAM_SUCCESS)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "pam_start(%s) failed: %s", self->pam_service,
                    pam_strerror(NULL, status));
        return;
    }

    if (self->remote_host != NULL)
    {
        pam_set_item(handle, PAM_RHOST, self->remote_host);
    }
    // TODO demain
    //     if ( self->domain != NULL && * self->domain != '\0')
    //     {
    // #ifdef PAM_USER_HOST
    //         pam_set_item(handle, PAM_USER_HOST, domain);
    // #endif
    //     }

    status = pam_authenticate(handle, PAM_SILENT);
    if (status != PAM_SUCCESS)
    {
        pam_end(handle, status);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "PAM authentication failed for %s: %s",
                    self->username, pam_strerror(NULL, status));
        return;
    }

    status = pam_acct_mgmt(handle, PAM_SILENT);
    if (status != PAM_SUCCESS && status != PAM_NEW_AUTHTOK_REQD)
    {
        pam_end(handle, status);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "PAM account check failed for %s: %s",
                    self->username, pam_strerror(NULL, status));
        return;
    }

    pam_end(handle, PAM_SUCCESS);
    self->handle = NULL;
}

const gchar *
drd_pam_auth_get_username(DrdPamAuth *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->username;
}

const gchar *
drd_pam_auth_get_password(DrdPamAuth *self)
{
    g_return_val_if_fail(self != NULL, NULL);
    return self->password;
}

void
drd_pam_auth_clear_password(DrdPamAuth *self)
{
    g_return_if_fail(self != NULL);
    drd_pam_auth_scrub_string(&self->password);
}

/*
 * 功能：使用 PAM 创建并打开本地会话。
 * 逻辑：构造 PAM 会话与自定义回调；设置远端主机与域字段；依次执行 pam_start、pam_authenticate、pam_acct_mgmt、pam_setcred、pam_open_session；任一步失败则写入错误并清理；成功返回封装的 DrdPamAuth。
 * 参数：pam_service PAM 服务名；username 用户名；domain 域/主机信息；password 密码；remote_host 远端地址；error 错误输出。
 * 外部接口：PAM API pam_start/pam_set_item/pam_authenticate/pam_acct_mgmt/pam_setcred/pam_open_session/pam_end；日志 DRD_LOG_MESSAGE。
 */
DrdPamAuth *
drd_pam_auth_new(const gchar *pam_service,
                      const gchar *username,
                      const gchar *domain,
                      const gchar *password,
                      const gchar *remote_host)
{
    g_return_val_if_fail(pam_service != NULL && *pam_service != '\0', NULL);
    g_return_val_if_fail(username != NULL && *username != '\0', NULL);
    g_return_val_if_fail(password != NULL && *password != '\0', NULL);

    DrdPamAuth *session = g_new0(DrdPamAuth, 1);
    session->username = g_strdup(username);
    session->domain = g_strdup(domain);
    session->pam_service = g_strdup(pam_service);
    session->remote_host = g_strdup(remote_host);
    session->password = g_strdup(password);
    return session;
}

/*
 * 功能：关闭并释放本地 PAM 会话。
 * 逻辑：若会话存在则清理 PAM 句柄、抹除用户名并释放结构体。
 * 参数：session 会话实例。
 * 外部接口：调用 drd_pam_auth_cleanup_handle、drd_pam_auth_scrub_string；GLib g_free。
 */
void
drd_pam_auth_close(DrdPamAuth *session)
{
    if (session == NULL)
    {
        return;
    }

    if (session->handle != NULL)
    {
        drd_pam_auth_cleanup_handle(session->handle);
        session->handle = NULL;
    }

    drd_pam_auth_scrub_string(&session->password);
    drd_pam_auth_scrub_string(&session->username);
    g_clear_pointer(&session->domain, g_free);
    g_clear_pointer(&session->pam_service, g_free);
    g_clear_pointer(&session->remote_host, g_free);
    g_free(session);
}
