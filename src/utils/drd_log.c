#include "utils/drd_log.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
    const gchar *value;
    gssize length;
} DrdLogFieldView;

static const gchar *drd_log_level_to_string(GLogLevelFlags level)
{
    /*
     * 功能：将 GLib 日志级别转换为字符串。
     * 逻辑：按级别掩码匹配返回常量字符串，未识别时返回 "Log"。
     * 参数：level GLib 日志级别。
     * 外部接口：无。
     */
    switch (level & G_LOG_LEVEL_MASK)
    {
        case G_LOG_LEVEL_ERROR:
            return "Error";
        case G_LOG_LEVEL_CRITICAL:
            return "Critical";
        case G_LOG_LEVEL_WARNING:
            return "Warning";
        case G_LOG_LEVEL_MESSAGE:
            return "Message";
        case G_LOG_LEVEL_INFO:
            return "Info";
        case G_LOG_LEVEL_DEBUG:
            return "Debug";
        default:
            return "Log";
    }
}

static const gchar *drd_log_lookup_field(const GLogField *fields, gsize n_fields, const gchar *name)
{
    /*
     * 功能：在结构化日志字段中查找指定键。
     * 逻辑：遍历字段数组，比对 key，返回找到的值指针。
     * 参数：fields 字段数组；n_fields 数量；name 目标键。
     * 外部接口：GLib g_strcmp0。
     */
    for (gsize i = 0; i < n_fields; i++)
    {
        if (g_strcmp0(fields[i].key, name) == 0 && fields[i].value != NULL)
        {
            return fields[i].value;
        }
    }
    return NULL;
}

static DrdLogFieldView drd_log_lookup_field_view(const GLogField *fields, gsize n_fields, const gchar *name)
{
    for (gsize i = 0; i < n_fields; i++)
    {
        if (g_strcmp0(fields[i].key, name) == 0)
        {
            return (DrdLogFieldView) {
                    .value = fields[i].value,
                    .length = fields[i].length,
            };
        }
    }
    return (DrdLogFieldView) {
            .value = NULL,
            .length = 0,
    };
}

static void drd_log_write_stderr(const gchar *buffer, gsize length)
{
    while (length > 0)
    {
        const ssize_t written = write(STDERR_FILENO, buffer, length);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        buffer += written;
        length -= (gsize) written;
    }
}

static void drd_log_append_hex_byte(GString *out, guint8 byte)
{
    static const char hex[] = "0123456789ABCDEF";
    g_string_append(out, "\\x");
    g_string_append_c(out, hex[(byte >> 4) & 0x0F]);
    g_string_append_c(out, hex[byte & 0x0F]);
}

static void drd_log_append_escaped_field(GString *out, DrdLogFieldView view, const gchar *fallback, gsize max_bytes)
{
    const gchar *ptr = view.value;
    gsize len = 0;

    if (ptr == NULL)
    {
        ptr = fallback;
        if (ptr == NULL)
        {
            ptr = "";
        }
        len = strnlen(ptr, max_bytes);
    }
    else if (view.length < 0)
    {
        /* length == -1 means NUL-terminated string, still cap to max_bytes. */
        len = strnlen(ptr, max_bytes);
    }
    else
    {
        len = MIN((gsize) view.length, max_bytes);
    }

    for (gsize i = 0; i < len; i++)
    {
        const guint8 c = (guint8) ptr[i];
        switch (c)
        {
            case '\n':
                g_string_append(out, "\\n");
                break;
            case '\r':
                g_string_append(out, "\\r");
                break;
            case '\t':
                g_string_append(out, "\\t");
                break;
            case '\\':
                g_string_append(out, "\\\\");
                break;
            default:
                if (c >= 0x20 && c <= 0x7E)
                {
                    g_string_append_c(out, (gchar) c);
                }
                else
                {
                    drd_log_append_hex_byte(out, c);
                }
                break;
        }
    }

    if (view.value != NULL && view.length >= 0 && (gsize) view.length > max_bytes)
    {
        g_string_append(out, "...");
    }
}

static GLogWriterOutput drd_log_writer(GLogLevelFlags log_level, const GLogField *fields, gsize n_fields,
                                       gpointer user_data G_GNUC_UNUSED)
{
    /*
     * 功能：自定义 GLib 日志 writer，格式化输出到 stderr。
     * 逻辑：提取 domain/message/代码位置信息，缺省填补后生成等级字符串，最终写入 stderr。
     * 参数：log_level 日志级别；fields/n_fields 结构化字段；user_data 未使用。
     * 外部接口：GLib 结构化日志回调、write。
     */
    const DrdLogFieldView domain = drd_log_lookup_field_view(fields, n_fields, "GLIB_DOMAIN");
    const DrdLogFieldView message = drd_log_lookup_field_view(fields, n_fields, "MESSAGE");
    const DrdLogFieldView file = drd_log_lookup_field_view(fields, n_fields, "CODE_FILE");
    const DrdLogFieldView line = drd_log_lookup_field_view(fields, n_fields, "CODE_LINE");
    const DrdLogFieldView func = drd_log_lookup_field_view(fields, n_fields, "CODE_FUNC");

    const gchar *level_str = drd_log_level_to_string(log_level);
    GString *formatted = g_string_sized_new(256);

    drd_log_append_escaped_field(formatted, domain, "drd", 64);
    g_string_append_c(formatted, '-');
    g_string_append(formatted, level_str);
    g_string_append(formatted, " [");
    drd_log_append_escaped_field(formatted, file, "unknown", 256);
    g_string_append_c(formatted, ':');
    drd_log_append_escaped_field(formatted, line, "0", 32);
    g_string_append_c(formatted, ' ');
    drd_log_append_escaped_field(formatted, func, "unknown", 128);
    g_string_append(formatted, "]: ");
    drd_log_append_escaped_field(formatted, message, "(null)", 2048);
    g_string_append_c(formatted, '\n');

    drd_log_write_stderr(formatted->str, formatted->len);
    g_string_free(formatted, TRUE);

    return G_LOG_WRITER_HANDLED;
}

void drd_log_init(void)
{
    /*
     * 功能：安装自定义日志 writer（一次性）。
     * 逻辑：使用 g_once_init 确保只初始化一次，并设置 g_log_set_writer_func。
     * 参数：无。
     * 外部接口：GLib g_log_set_writer_func/g_once_init_enter/g_once_init_leave。
     */
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized))
    {
        g_log_set_writer_func(drd_log_writer, NULL, NULL);
        g_once_init_leave(&initialized, 1);
    }
}
