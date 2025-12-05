#include "transport/drd_rdp_routing_token.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include <freerdp/crypto/crypto.h>
#include <freerdp/constants.h>
#include <winpr/stream.h>

#include "utils/drd_log.h"

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

/*
 * 功能：分配路由令牌信息结构并初始化为空。
 * 逻辑：使用 g_new0 分配零填充结构。
 * 参数：无。
 * 外部接口：GLib g_new0。
 */
DrdRoutingTokenInfo *
drd_routing_token_info_new(void)
{
    return g_new0(DrdRoutingTokenInfo, 1);
}

/*
 * 功能：释放路由令牌信息并清除字符串。
 * 逻辑：释放 routing_token 字符串后释放结构本身。
 * 参数：info 路由令牌信息。
 * 外部接口：GLib g_free。
 */
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
wstream_free_full(wStream * s);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(wStream, wstream_free_full)

/*
 * 功能：释放 WinPR wStream 并释放底层缓冲。
 * 逻辑：调用 Stream_Free，传 TRUE 表示释放内部分配的缓冲。
 * 参数：s 待释放的 wStream。
 * 外部接口：WinPR Stream_Free。
 */
static void
wstream_free_full(wStream *s)
{
    Stream_Free(s, TRUE);
}

#define MAX_PEEK_TIME_MS 2000

/*
 * 功能：在不消费 socket 数据的情况下读取指定字节数（MSG_PEEK）。
 * 逻辑：使用 g_poll 等待可读或取消事件，随后调用 recv(MSG_PEEK) 获取数据，支持 EINTR/EAGAIN 重试。
 * 参数：fd 套接字；buffer 目标缓存；length 读取长度；cancellable 取消源；error 错误输出。
 * 外部接口：POSIX poll/recv，GLib g_cancellable_* 管理取消。
 */
static gboolean
peek_bytes(int fd,
           uint8_t *buffer,
           int length,
           GCancellable *cancellable,
           GError **error)
{
    GPollFD poll_fds[2] = {};
    int n_fds = 0;
    int ret;

    poll_fds[n_fds].fd = fd;
    poll_fds[n_fds].events = G_IO_IN;
    n_fds++;

    if (!g_cancellable_make_pollfd(cancellable, &poll_fds[n_fds]))
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failure preparing the cancellable for pollfd");
        return FALSE;
    }

    n_fds++;

    do
    {
        do
            ret = g_poll(poll_fds, n_fds, MAX_PEEK_TIME_MS);
        while (ret == -1 && errno == EINTR);

        if (ret == -1)
        {
            g_set_error(error, G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "On poll command: %s", strerror(errno));
            g_cancellable_release_fd(cancellable);
            return FALSE;
        }

        if (g_cancellable_is_cancelled(cancellable))
        {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "Cancelled");
            g_cancellable_release_fd(cancellable);
            return FALSE;
        }

        do
            ret = recv(fd, (void *) buffer, (size_t) length, MSG_PEEK);
        while (ret == -1 && errno == EINTR);

        if (ret == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            g_set_error(error, G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "On recv command: %s", strerror(errno));
            g_cancellable_release_fd(cancellable);
            return FALSE;
        }
    }
    while (ret < length);

    g_cancellable_release_fd(cancellable);

    return TRUE;
}

/*
 * 功能：在 buffer 中查找 CRLF 序列的起始位置。
 * 逻辑：线性扫描直到遇到 0x0D 0x0A，返回索引；找不到返回 -1。
 * 参数：buffer 缓冲；length 长度。
 * 外部接口：无。
 */
static int
find_cr_lf(const char *buffer,
           int length)
{
    int i;

    for (i = 0; i < length - 1; ++i)
    {
        if (buffer[i] == 0x0D && buffer[i + 1] == 0x0A)
            return i;
    }

    return -1;
}

/*
 * 功能：从 peek 到的头部提取 routing token（移除前缀）。
 * 逻辑：检查前缀 "Cookie: msts=" 是否匹配；若匹配则截取 CRLF 前的 token。
 * 参数：buffer 数据；buffer_length 长度；routing_token_length 输出 token 总长度（含前缀）。
 * 外部接口：GLib g_strdup/g_strndup。
 */
static char *
get_routing_token_without_prefix(char *buffer,
                                 size_t buffer_length,
                                 int *routing_token_length)
{
    g_autofree
    char *peeked_prefix = NULL;
    g_autofree
    char *prefix = NULL;
    size_t prefix_length;

    prefix = g_strdup("Cookie: msts=");
    prefix_length = strlen(prefix);

    if (buffer_length < prefix_length)
        return NULL;

    peeked_prefix = g_strndup(buffer, prefix_length);
    if (g_strcmp0(peeked_prefix, prefix) != 0)
        return NULL;

    *routing_token_length = find_cr_lf(buffer, buffer_length);
    if (*routing_token_length == -1)
        return NULL;

    return g_strndup(buffer + prefix_length,
                     *routing_token_length - prefix_length);
}


/*
 * 功能：窥探 RDP TPKT/X224 握手头，提取路由令牌及 RDSTLS 请求信息。
 * 逻辑：MSG_PEEK 首 4 字节解析 TPKT 长度；再 peek 全部 PDU 验证 x224 CRQ 与 rdpNegReq，
 *       解析前缀为 Cookie: msts= 的 routing token，并检查是否请求 RDSTLS 协议。
 * 参数：connection GLib socket 连接；cancellable 可取消对象；info 输出路由令牌信息；error 错误输出。
 * 外部接口：WinPR Stream_* 操作缓冲，FreeRDP 常量 PROTOCOL_RDSTLS；GLib g_socket_get_fd 获取 fd。
 */
gboolean
drd_routing_token_peek(GSocketConnection *connection,
                       GCancellable *cancellable,
                       DrdRoutingTokenInfo *info,
                       GError **error)
{
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);
    g_return_val_if_fail(info != NULL, FALSE);

    GSocket *socket = g_socket_connection_get_socket(connection);
    if (!G_IS_SOCKET(socket))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Socket unavailable for routing token peek");
        return FALSE;
    }


    // copy form grd
    g_autoptr(wStream)
    stream = Stream_New(NULL, 4);
    if (stream == NULL)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate stream");
        return FALSE;
    }

    int fd = g_socket_get_fd(socket);
    if (!peek_bytes(fd, Stream_Buffer(stream), 4, cancellable, error))
        return FALSE;

    /* TPKT values */
    uint8_t version;
    uint16_t tpkt_length;
    Stream_Read_UINT8(stream, version);
    Stream_Seek(stream, 1);
    Stream_Read_UINT16_BE(stream, tpkt_length);

    if (version != 3)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "The TPKT Header doesn't have version 3");
        return FALSE;
    }
    if (tpkt_length < 4 + 7)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "The x224Crq TPDU length is too short");
        return FALSE;
    }
    /* Peek full PDU */
    Stream_Free(stream, TRUE);
    stream = Stream_New(NULL, tpkt_length);
    g_assert(stream);

    if (!peek_bytes(fd, Stream_Buffer(stream), tpkt_length, cancellable, error))
        return FALSE;

    Stream_Seek(stream, 4);


    /* Check x224Crq */
    uint8_t length_indicator;
    uint8_t cr_cdt;
    uint16_t dst_ref;
    uint8_t class_opt;
    Stream_Read_UINT8(stream, length_indicator);
    Stream_Read_UINT8(stream, cr_cdt);
    Stream_Read_UINT16(stream, dst_ref);
    Stream_Seek(stream, 2);
    Stream_Read_UINT8(stream, class_opt);
    if (tpkt_length - 5 != length_indicator ||
        cr_cdt != 0xE0 ||
        dst_ref != 0 ||
        (class_opt & 0xFC) != 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Wrong info on x224Crq");
        return FALSE;
    }

    int routing_token_length;
    /* Check routingToken */
    info->routing_token = get_routing_token_without_prefix((char *) Stream_Pointer(stream),
                                                           Stream_GetRemainingLength(stream),
                                                           &routing_token_length);
    if (!info->routing_token)
        return TRUE;

    /* Check rdpNegReq */
    Stream_Seek(stream, routing_token_length + 2);
    if (Stream_GetRemainingLength(stream) < 8)
        return TRUE;

    /* rdpNegReq values */
    uint8_t rdp_neg_type;
    uint16_t rdp_neg_length;
    uint32_t requested_protocols;
    Stream_Read_UINT8(stream, rdp_neg_type);
    Stream_Seek(stream, 1);
    Stream_Read_UINT16(stream, rdp_neg_length);
    Stream_Read_UINT32(stream, requested_protocols);
    if (rdp_neg_type != 0x01 || rdp_neg_length != 8)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Wrong info on rdpNegReq");
        return FALSE;
    }

    info->requested_rdstls = !!(requested_protocols & PROTOCOL_RDSTLS);

    // end grd copy


    return TRUE;
}
