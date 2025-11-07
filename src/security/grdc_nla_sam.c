#include "security/grdc_nla_sam.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <winpr/ntlm.h>

struct _GrdcNlaSamFile
{
    gchar *path;
};

/* 简单的内存清零帮助函数，避免编译器优化。 */
static void
grdc_nla_memzero(gchar *buffer, gsize len)
{
    if (buffer == NULL || len == 0)
    {
        return;
    }

    volatile gchar *ptr = buffer;
    while (len-- > 0)
    {
        ptr[len] = 0;
    }
}

/* 根据用户名和 NT 哈希生成 SAM 条目文本（username:::NTLM_HASH:::）。 */
static gchar *
grdc_nla_sam_format_entry(const gchar *username, const gchar *nt_hash_hex)
{
    const gsize username_len = strlen(username);
    const gsize hash_len = strlen(nt_hash_hex);
    const gsize buffer_len = username_len + 3 + hash_len + 4;
    gchar *line = g_malloc0(buffer_len);

    g_stpcpy(line, username);
    g_stpcpy(line + strlen(line), ":::");
    g_stpcpy(line + strlen(line), nt_hash_hex);
    g_stpcpy(line + strlen(line), ":::\n");
    return line;
}

/* 将内容写入 SAM 文件，确保完整落盘。 */
static gboolean
grdc_nla_sam_write_entry(int fd, const gchar *username, const gchar *nt_hash_hex, GError **error)
{
    g_autofree gchar *entry = grdc_nla_sam_format_entry(username, nt_hash_hex);
    const gsize total = strlen(entry);
    gsize written = 0;

    while (written < total)
    {
        ssize_t ret = write(fd, entry + written, total - written);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "Failed to write SAM file: %s",
                        g_strerror(errno));
            return FALSE;
        }
        written += (gsize)ret;
    }

    if (fsync(fd) != 0)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "Failed to flush SAM file: %s",
                    g_strerror(errno));
        return FALSE;
    }

    grdc_nla_memzero(entry, total);

    return TRUE;
}

/* 选择 SAM 文件所在目录（优先 XDG runtime，回退 /tmp）。 */
static gchar *
grdc_nla_sam_default_dir(void)
{
    const gchar *runtime_dir = g_get_user_runtime_dir();
    if (runtime_dir != NULL)
    {
        return g_build_filename(runtime_dir, "grdc", NULL);
    }
    return g_build_filename(g_get_tmp_dir(), "grdc", NULL);
}

GrdcNlaSamFile *
grdc_nla_sam_file_new(const gchar *username, const gchar *nt_hash_hex, GError **error)
{
    g_return_val_if_fail(username != NULL && *username != '\0', NULL);
    g_return_val_if_fail(nt_hash_hex != NULL && *nt_hash_hex != '\0', NULL);

    g_autofree gchar *base_dir = grdc_nla_sam_default_dir();
    if (g_mkdir_with_parents(base_dir, 0700) != 0 && errno != EEXIST)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "Failed to create SAM directory '%s': %s",
                    base_dir,
                    g_strerror(errno));
        return NULL;
    }

    g_autofree gchar *template_path = g_build_filename(base_dir, "nla-sam-XXXXXX", NULL);
    int fd = g_mkstemp_full(template_path, O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "Failed to create SAM file: %s",
                    g_strerror(errno));
        return NULL;
    }

    if (!grdc_nla_sam_write_entry(fd, username, nt_hash_hex, error))
    {
        close(fd);
        g_unlink(template_path);
        return NULL;
    }

    close(fd);

    GrdcNlaSamFile *sam_file = g_new0(GrdcNlaSamFile, 1);
    sam_file->path = g_strdup(template_path);
    return sam_file;
}

const gchar *
grdc_nla_sam_file_get_path(GrdcNlaSamFile *sam_file)
{
    g_return_val_if_fail(sam_file != NULL, NULL);
    return sam_file->path;
}

void
grdc_nla_sam_file_free(GrdcNlaSamFile *sam_file)
{
    if (sam_file == NULL)
    {
        return;
    }

    if (sam_file->path != NULL)
    {
        g_unlink(sam_file->path);
    }

    g_free(sam_file->path);
    g_free(sam_file);
}

gchar *
grdc_nla_sam_hash_password(const gchar *password)
{
    g_return_val_if_fail(password != NULL && *password != '\0', NULL);

    uint8_t hash[16];
    if (!NTOWFv1A((LPSTR)password, strlen(password), hash))
    {
        g_warning("Failed to initialize NT hash for NLA credentials");
        return NULL;
    }

    gchar *hex = g_malloc0(33);
    for (gsize i = 0; i < G_N_ELEMENTS(hash); i++)
    {
        g_snprintf(hex + (i * 2), 3, "%02" PRIx8, hash[i]);
    }
    return hex;
}
