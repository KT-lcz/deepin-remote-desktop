#include "utils/drd_log.h"

#include <errno.h>
#include <unistd.h>

static const gchar *
drd_log_level_to_string(GLogLevelFlags level)
{
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

static const gchar *
drd_log_lookup_field(const GLogField *fields, gsize n_fields, const gchar *name)
{
    for (gsize i = 0; i < n_fields; i++)
    {
        if (g_strcmp0(fields[i].key, name) == 0 && fields[i].value != NULL)
        {
            return fields[i].value;
        }
    }
    return NULL;
}

// static void
// drd_log_write_stderr(const gchar *buffer, gsize length)
// {
//     while (length > 0)
//     {
//         const ssize_t written = write(STDERR_FILENO, buffer, length);
//         if (written < 0)
//         {
//             if (errno == EINTR)
//             {
//                 continue;
//             }
//             break;
//         }
//         buffer += written;
//         length -= written;
//     }
// }

static GLogWriterOutput
drd_log_writer(GLogLevelFlags log_level, const GLogField *fields, gsize n_fields, gpointer user_data G_GNUC_UNUSED)
{
    const gchar *domain = drd_log_lookup_field(fields, n_fields, "GLIB_DOMAIN");
    const gchar *message = drd_log_lookup_field(fields, n_fields, "MESSAGE");
    const gchar *file = drd_log_lookup_field(fields, n_fields, "CODE_FILE");
    const gchar *line = drd_log_lookup_field(fields, n_fields, "CODE_LINE");
    const gchar *func = drd_log_lookup_field(fields, n_fields, "CODE_FUNC");

    if (message == NULL)
    {
        message = "(null)";
    }
    if (file == NULL)
    {
        file = "unknown";
    }
    if (line == NULL)
    {
        line = "0";
    }
    if (func == NULL)
    {
        func = "unknown";
    }

    const gchar *level_str = drd_log_level_to_string(log_level);
    if (domain == NULL)
    {
        domain = "drd";
    }

    // GString *formatted = g_string_new(NULL);
    // g_string_append_printf(
    //     formatted, "%s-%s [%s:%s %s]: %s\n", domain, level_str, file, line, func, message);
    // drd_log_write_stderr(formatted->str, formatted->len);
    // g_string_free(formatted, TRUE);
    g_printerr("%s-%s [%s:%s %s]: %s\n", domain, level_str, file, line, func, message);

    return G_LOG_WRITER_HANDLED;
}

void
drd_log_init(void)
{
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized))
    {
        g_log_set_writer_func(drd_log_writer, NULL, NULL);
        g_once_init_leave(&initialized, 1);
    }
}
