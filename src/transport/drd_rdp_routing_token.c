#include "transport/drd_rdp_routing_token.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include <freerdp/crypto/crypto.h>
#include <freerdp/constants.h>
#include <winpr/stream.h>

#ifndef PROTOCOL_RDSTLS
#define PROTOCOL_RDSTLS 0x00000004
#endif

#define DRD_ROUTING_TOKEN_PREFIX "Cookie: msts="
#define DRD_ROUTING_TOKEN_PEEK_TIMEOUT_MS (2 * 1000)

struct _DrdRoutingTokenInfo
{
    gboolean requested_rdstls;
    gchar *routing_token;
};

DrdRoutingTokenInfo *
drd_routing_token_info_new(void)
{
    return g_new0(DrdRoutingTokenInfo, 1);
}

void
 drd_routing_token_info_free(DrdRoutingTokenInfo *info)
{
    if (info == NULL)
    {
        return;
    }

    g_clear_pointer(&info->routing_token, g_free);
    g_free(info);
}

typedef struct
{
    GSocket *socket;
    GCancellable *cancellable;
} DrdRoutingTokenPeekContext;

static void
wstream_free_full(wStream *s);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(wStream, wstream_free_full)

static void
wstream_free_full(wStream *s)
{
    Stream_Free(s, TRUE);
}

static gboolean
drd_routing_token_peek_bytes(DrdRoutingTokenPeekContext *ctx,
                              guint8 *buffer,
                              gsize length,
                              GError **error)
{
    GPollFD poll_fds[2] = {0};
    gint n_fds = 0;

    poll_fds[n_fds].fd = g_socket_get_fd(ctx->socket);
    poll_fds[n_fds].events = G_IO_IN;
    n_fds++;

    gboolean has_cancellable_fd = FALSE;
    if (ctx->cancellable != NULL)
    {
        if (!g_cancellable_make_pollfd(ctx->cancellable, &poll_fds[n_fds]))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to prepare cancellable FD");
            return FALSE;
        }
        has_cancellable_fd = TRUE;
        n_fds++;
    }

    while (TRUE)
    {
        gint ret;
        do
        {
            ret = g_poll(poll_fds, n_fds, DRD_ROUTING_TOKEN_PEEK_TIMEOUT_MS);
        } while (ret == -1 && errno == EINTR);

        if (ret == -1)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "Poll failed: %s",
                        g_strerror(errno));
            break;
        }

        if (has_cancellable_fd && g_cancellable_is_cancelled(ctx->cancellable))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled while peeking token");
            break;
        }

        do
        {
            ret = recv(poll_fds[0].fd, buffer, length, MSG_PEEK);
        } while (ret == -1 && errno == EINTR);

        if (ret == (gint)length)
        {
            if (has_cancellable_fd)
            {
                g_cancellable_release_fd(ctx->cancellable);
            }
            return TRUE;
        }

        if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            continue;
        }

        if (ret == -1)
        {
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "recv failed: %s",
                        g_strerror(errno));
        }
        else
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unexpected EOF while peeking token");
        }
        break;
    }

    if (has_cancellable_fd)
    {
        g_cancellable_release_fd(ctx->cancellable);
    }
    return FALSE;
}

static gboolean
drd_routing_token_read_stream(DrdRoutingTokenPeekContext *ctx,
                               wStream *stream,
                               guint expected,
                               GError **error)
{
    if (!drd_routing_token_peek_bytes(ctx, Stream_Buffer(stream), expected, error))
    {
        return FALSE;
    }
    Stream_SetPosition(stream, expected);
    return TRUE;
}

static gchar *
drd_routing_token_extract_cookie(const gchar *buffer, gsize length)
{
    const gsize prefix_len = strlen(DRD_ROUTING_TOKEN_PREFIX);
    if (length <= prefix_len)
    {
        return NULL;
    }

    if (g_str_has_prefix(buffer, DRD_ROUTING_TOKEN_PREFIX))
    {
        const gchar *end = strstr(buffer, "\r\n");
        if (end == NULL)
        {
            return NULL;
        }
        return g_strndup(buffer + prefix_len, end - buffer - prefix_len);
    }

    return NULL;
}

static gboolean
drd_routing_token_parse_neg_req(wStream *stream, gboolean *requested_rdstls)
{
    /* rdpNegReq fields after cookie */
    UINT8 type = 0;
    UINT8 flags = 0;
    UINT16 length = 0;
    UINT32 requested_protocols = 0;

    if (Stream_GetRemainingLength(stream) < 8)
    {
        return FALSE;
    }

    Stream_Read_UINT8(stream, type);
    Stream_Read_UINT8(stream, flags);
    Stream_Read_UINT16(stream, length);
    (void)flags;
    (void)length;

    if (type != 0x01) /* TYPE_RDP_NEG_REQ */
    {
        return FALSE;
    }

    Stream_Read_UINT32(stream, requested_protocols);
    *requested_rdstls = (requested_protocols & PROTOCOL_RDSTLS) != 0;
    return TRUE;
}

gboolean
drd_routing_token_peek(GSocketConnection *connection,
                                GCancellable *cancellable,
                                DrdRoutingTokenInfo *info,
                                GError **error)
{
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);
    g_return_val_if_fail(info != NULL, FALSE);

    g_autoptr(GSocket) socket = g_socket_connection_get_socket(connection);
    if (!G_IS_SOCKET(socket))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Socket unavailable for routing token peek");
        return FALSE;
    }

    DrdRoutingTokenPeekContext ctx = {socket, cancellable};
    g_autoptr(wStream) stream = Stream_New(NULL, 512);
    if (stream == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate stream");
        return FALSE;
    }

    /* Peek TPKT header (4 bytes) */
    if (!drd_routing_token_read_stream(&ctx, stream, 4, error))
    {
        return FALSE;
    }

    Stream_Rewind(stream, 4);
    UINT8 version = 0;
    UINT16 tpkt_length = 0;
    Stream_Read_UINT8(stream, version);
    Stream_Seek_UINT8(stream); /* reserved */
    Stream_Read_UINT16(stream, tpkt_length);

    if (version != 3 || tpkt_length < 11)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid TPKT header when peeking token");
        return FALSE;
    }

    /* x224 CR TPDU (7 bytes) */
    Stream_SetPosition(stream, 0);
    if (!drd_routing_token_read_stream(&ctx, stream, 11, error))
    {
        return FALSE;
    }

    Stream_SetPosition(stream, 7);
    UINT8 rdp_neg_type = 0;
    Stream_Read_UINT8(stream, rdp_neg_type);
    Stream_SetPosition(stream, 11);

    gchar *cookie = drd_routing_token_extract_cookie((const gchar *)Stream_Buffer(stream), Stream_GetPosition(stream));
    if (cookie != NULL)
    {
        g_clear_pointer(&info->routing_token, g_free);
        info->routing_token = cookie;
    }

    if (rdp_neg_type == 0x01)
    {
        Stream_SetPosition(stream, 11);
        if (!drd_routing_token_parse_neg_req(stream, &info->requested_rdstls))
        {
            info->requested_rdstls = FALSE;
        }
    }

    return TRUE;
}
