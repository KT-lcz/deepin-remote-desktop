#include "security/drd_local_session.h"

#include <gio/gio.h>
#include <security/pam_appl.h>
#include <string.h>
#include <stdlib.h>

#include "utils/drd_log.h"

struct _DrdLocalSession
{
    pam_handle_t *handle;
    gchar *username;
};

typedef struct
{
    const gchar *password;
} DrdPamConversationData;

static int
drd_local_session_pam_conv(int num_msg, const struct pam_message **msg, struct pam_response **resp, void *user_data)
{
    DrdPamConversationData *conv = (DrdPamConversationData *)user_data;
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

static void
drd_local_session_scrub_string(gchar **value)
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

static void
drd_local_session_cleanup_handle(pam_handle_t *handle)
{
    if (handle == NULL)
    {
        return;
    }
    pam_close_session(handle, PAM_SILENT);
    pam_setcred(handle, PAM_DELETE_CRED | PAM_SILENT);
    pam_end(handle, PAM_SUCCESS);
}

DrdLocalSession *
drd_local_session_new(const gchar *pam_service,
                      const gchar *username,
                      const gchar *domain,
                      const gchar *password,
                      const gchar *remote_host,
                      GError **error)
{
    g_return_val_if_fail(pam_service != NULL && *pam_service != '\0', NULL);
    g_return_val_if_fail(username != NULL && *username != '\0', NULL);
    g_return_val_if_fail(password != NULL && *password != '\0', NULL);

    DrdPamConversationData conv_data = {
        .password = password,
    };
    struct pam_conv conv = {
        .conv = drd_local_session_pam_conv,
        .appdata_ptr = &conv_data,
    };

    pam_handle_t *handle = NULL;
    int status = pam_start(pam_service, username, &conv, &handle);
    if (status != PAM_SUCCESS)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "pam_start(%s) failed: %s",
                    pam_service,
                    pam_strerror(handle, status));
        return NULL;
    }

    if (remote_host != NULL)
    {
        pam_set_item(handle, PAM_RHOST, remote_host);
    }
    if (domain != NULL && *domain != '\0')
    {
#ifdef PAM_USER_HOST
        pam_set_item(handle, PAM_USER_HOST, domain);
#endif
    }

    status = pam_authenticate(handle, PAM_SILENT);
    if (status != PAM_SUCCESS)
    {
        pam_end(handle, status);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    "PAM authentication failed for %s: %s",
                    username,
                    pam_strerror(NULL, status));
        return NULL;
    }

    status = pam_acct_mgmt(handle, PAM_SILENT);
    if (status != PAM_SUCCESS && status != PAM_NEW_AUTHTOK_REQD)
    {
        pam_end(handle, status);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    "PAM account check failed for %s: %s",
                    username,
                    pam_strerror(NULL, status));
        return NULL;
    }

    status = pam_setcred(handle, PAM_ESTABLISH_CRED | PAM_SILENT);
    if (status != PAM_SUCCESS && status != PAM_CRED_UNAVAIL)
    {
        pam_end(handle, status);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Failed to establish PAM credentials: %s",
                    pam_strerror(NULL, status));
        return NULL;
    }

    status = pam_open_session(handle, PAM_SILENT);
    if (status != PAM_SUCCESS)
    {
        pam_setcred(handle, PAM_DELETE_CRED | PAM_SILENT);
        pam_end(handle, status);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Failed to open PAM session for %s: %s",
                    username,
                    pam_strerror(NULL, status));
        return NULL;
    }

    DrdLocalSession *session = g_new0(DrdLocalSession, 1);
    session->handle = handle;
    session->username = g_strdup(username);
    DRD_LOG_MESSAGE("Opened PAM session for %s via service %s", username, pam_service);
    return session;
}

void
drd_local_session_close(DrdLocalSession *session)
{
    if (session == NULL)
    {
        return;
    }

    if (session->handle != NULL)
    {
        drd_local_session_cleanup_handle(session->handle);
        session->handle = NULL;
    }

    drd_local_session_scrub_string(&session->username);
    g_free(session);
}
