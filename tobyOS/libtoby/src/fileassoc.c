/* fileassoc.c -- file association registry for tobyOS userland.
 *
 * Maps file extensions to application paths. Stores associations
 * in-memory with defaults for common file types. A real implementation
 * would persist to /system/fileassoc.conf.
 */

#include <toby/fileassoc.h>
#include <string.h>

#define FA_MAX 32

static struct file_assoc g_assocs[FA_MAX];
static int               g_count;
static int               g_init_done;

static void ensure_init(void)
{
    if (g_init_done) return;
    g_init_done = 1;
    g_count = 0;

    struct {
        const char *ext;
        const char *app;
        const char *desc;
    } defaults[] = {
        { ".txt",  "/bin/gui_edit",   "Text Document"   },
        { ".c",    "/bin/gui_edit",   "C Source File"    },
        { ".h",    "/bin/gui_edit",   "C Header File"    },
        { ".md",   "/bin/gui_edit",   "Markdown File"    },
        { ".png",  "/bin/gui_viewer", "PNG Image"        },
        { ".jpg",  "/bin/gui_viewer", "JPEG Image"       },
        { ".mp3",  "/bin/mediaplayer","MP3 Audio"        },
        { ".mp4",  "/bin/mediaplayer","MP4 Video"        },
    };

    for (int i = 0; i < (int)(sizeof(defaults)/sizeof(defaults[0])); i++) {
        struct file_assoc *a = &g_assocs[g_count];
        memset(a, 0, sizeof(*a));
        strncpy(a->extension, defaults[i].ext, sizeof(a->extension) - 1);
        strncpy(a->app_path, defaults[i].app, sizeof(a->app_path) - 1);
        strncpy(a->description, defaults[i].desc, sizeof(a->description) - 1);
        g_count++;
    }
}

static int find_ext(const char *ext)
{
    ensure_init();
    if (!ext) return -1;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_assocs[i].extension, ext) == 0) return i;
    return -1;
}

int fileassoc_register(const char *ext, const char *app_path,
                       const char *description)
{
    ensure_init();
    if (!ext || !app_path) return -1;

    int idx = find_ext(ext);
    if (idx >= 0) {
        strncpy(g_assocs[idx].app_path, app_path, sizeof(g_assocs[idx].app_path) - 1);
        if (description)
            strncpy(g_assocs[idx].description, description, sizeof(g_assocs[idx].description) - 1);
        return 0;
    }

    if (g_count >= FA_MAX) return -1;

    struct file_assoc *a = &g_assocs[g_count];
    memset(a, 0, sizeof(*a));
    strncpy(a->extension, ext, sizeof(a->extension) - 1);
    strncpy(a->app_path, app_path, sizeof(a->app_path) - 1);
    if (description)
        strncpy(a->description, description, sizeof(a->description) - 1);
    g_count++;
    return 0;
}

int fileassoc_unregister(const char *ext)
{
    ensure_init();
    int idx = find_ext(ext);
    if (idx < 0) return -1;

    for (int i = idx; i < g_count - 1; i++)
        g_assocs[i] = g_assocs[i + 1];
    g_count--;
    return 0;
}

const char *fileassoc_get_app(const char *ext)
{
    int idx = find_ext(ext);
    if (idx < 0) return (const char *)0;
    return g_assocs[idx].app_path;
}

int fileassoc_get_all(struct file_assoc *out, int max)
{
    ensure_init();
    if (!out || max <= 0) return 0;
    int count = g_count;
    if (count > max) count = max;
    memcpy(out, g_assocs, (size_t)count * sizeof(struct file_assoc));
    return count;
}

int fileassoc_set_default(const char *ext, const char *app_path)
{
    return fileassoc_register(ext, app_path, (const char *)0);
}
