/* src/file_assoc.c -- File extension to application mapping.
 *
 * Maintains a table of file-type associations used by the desktop to
 * open files with their preferred application. The file manager and
 * drag-and-drop subsystem use file_assoc_open() to launch the correct
 * app for a given file path.
 */

#include <tobyos/file_assoc.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>

/* ---- Association table ---- */

static struct file_assoc g_assocs[ASSOC_MAX];
static int g_assoc_count;

/* ---- Helpers ---- */

static void fa_strcpy(char *dst, const char *src, int maxlen) {
    int i = 0;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] && i < maxlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int fa_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static const char *get_extension(const char *filename) {
    if (!filename) return NULL;
    const char *dot = NULL;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') dot = p + 1;
        if (*p == '/') dot = NULL;
    }
    return dot;
}

static struct file_assoc *find_assoc(const char *ext) {
    if (!ext) return NULL;
    for (int i = 0; i < g_assoc_count; i++) {
        if (fa_stricmp(g_assocs[i].extension, ext) == 0)
            return &g_assocs[i];
    }
    return NULL;
}

/* ---- Public API ---- */

void file_assoc_init(void) {
    memset(g_assocs, 0, sizeof(g_assocs));
    g_assoc_count = 0;

    /* Default associations */
    file_assoc_register("txt",  "/bin/gui_viewer", "text_file",  "text/plain");
    file_assoc_register("c",    "/bin/gui_viewer", "text_file",  "text/x-c");
    file_assoc_register("h",    "/bin/gui_viewer", "text_file",  "text/x-c");
    file_assoc_register("conf", "/bin/gui_viewer", "text_file",  "text/plain");
    file_assoc_register("log",  "/bin/gui_viewer", "text_file",  "text/plain");
    file_assoc_register("png",  "/bin/gui_viewer", "image_file", "image/png");
    file_assoc_register("jpg",  "/bin/gui_viewer", "image_file", "image/jpeg");
    file_assoc_register("bmp",  "/bin/gui_viewer", "image_file", "image/bmp");
    file_assoc_register("gif",  "/bin/gui_viewer", "image_file", "image/gif");
    file_assoc_register("wav",  "/bin/mediaplayer","audio_file", "audio/wav");
    file_assoc_register("mp3",  "/bin/mediaplayer","audio_file", "audio/mpeg");
    file_assoc_register("ogg",  "/bin/mediaplayer","audio_file", "audio/ogg");
    file_assoc_register("html", "/bin/gui_browser","web_file",   "text/html");
    file_assoc_register("pdf",  "/bin/gui_viewer", "pdf_file",   "application/pdf");
    file_assoc_register("elf",  NULL,              "exec_file",  "application/x-elf");

    kprintf("[file_assoc] initialized with %d default associations\n",
            g_assoc_count);
}

int file_assoc_register(const char *ext, const char *app,
                        const char *icon, const char *mime) {
    if (!ext) return -1;

    /* Update existing entry if extension already registered */
    struct file_assoc *existing = find_assoc(ext);
    if (existing) {
        fa_strcpy(existing->app_path, app, FA_PATH_MAX);
        fa_strcpy(existing->icon_name, icon, 32);
        fa_strcpy(existing->mime_type, mime, 64);
        return 0;
    }

    if (g_assoc_count >= ASSOC_MAX) return -1;

    struct file_assoc *a = &g_assocs[g_assoc_count++];
    fa_strcpy(a->extension, ext, FA_EXT_MAX);
    fa_strcpy(a->app_path, app, FA_PATH_MAX);
    fa_strcpy(a->icon_name, icon, 32);
    fa_strcpy(a->mime_type, mime, 64);
    return 0;
}

const char *file_assoc_get_app(const char *filename) {
    const char *ext = get_extension(filename);
    struct file_assoc *a = find_assoc(ext);
    if (a && a->app_path[0]) return a->app_path;
    return NULL;
}

const char *file_assoc_get_icon(const char *filename) {
    const char *ext = get_extension(filename);
    struct file_assoc *a = find_assoc(ext);
    if (a) return a->icon_name;
    return "unknown_file";
}

const char *file_assoc_get_mime(const char *filename) {
    const char *ext = get_extension(filename);
    struct file_assoc *a = find_assoc(ext);
    if (a) return a->mime_type;
    return "application/octet-stream";
}

int file_assoc_open(const char *filepath) {
    if (!filepath) return -1;

    const char *ext = get_extension(filepath);
    struct file_assoc *a = find_assoc(ext);

    if (!a) {
        kprintf("[file_assoc] no association for '%s'\n", filepath);
        return -1;
    }

    /* ELF files are directly executable */
    if (fa_stricmp(a->extension, "elf") == 0) {
        char *argv[] = { (char *)filepath, NULL };
        struct proc_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.path = filepath;
        spec.argc = 1;
        spec.argv = argv;
        int pid = proc_spawn(&spec);
        if (pid < 0) {
            kprintf("[file_assoc] failed to exec '%s'\n", filepath);
            return -1;
        }
        return pid;
    }

    if (!a->app_path[0]) {
        kprintf("[file_assoc] no app for extension '%s'\n", ext);
        return -1;
    }

    char *argv[] = { (char *)a->app_path, (char *)filepath, NULL };
    struct proc_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.path = a->app_path;
    spec.argc = 2;
    spec.argv = argv;
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("[file_assoc] failed to open '%s' with '%s'\n",
                filepath, a->app_path);
        return -1;
    }

    kprintf("[file_assoc] opened '%s' with '%s' (pid %d)\n",
            filepath, a->app_path, pid);
    return pid;
}

int file_assoc_list(struct file_assoc *out, int max) {
    if (!out || max <= 0) return 0;
    int count = g_assoc_count < max ? g_assoc_count : max;
    memcpy(out, g_assocs, (size_t)count * sizeof(struct file_assoc));
    return count;
}
