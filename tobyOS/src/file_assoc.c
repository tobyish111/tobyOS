/* src/file_assoc.c -- file types and their handlers, kernel side.
 *
 * The table lives in <tobyos/file_types.h> and is shared with userland's
 * libtoby/src/fileassoc.c; this file expands it once for the kernel.
 *
 * What was here before: a private list of a dozen extensions and a
 * file_assoc_init() that nothing ever called -- not one reference to any
 * symbol in this file existed outside it. It was dead the whole time, and it
 * disagreed with the live code in File Explorer (it sent .txt to the
 * read-only viewer; Explorer sent it to the editor). Both now answer from
 * the same table, so they cannot drift apart again.
 *
 * file_assoc_register() keeps working, but overrides live in their own array
 * instead of mutating a copy of the defaults: the built-ins are const, and a
 * bad override must never be able to destroy the shipped answer.
 */

#include <tobyos/file_assoc.h>
#include <tobyos/file_types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>

static const struct file_type g_types[] = {
#define FT_ENTRY(e, l, h, g, ed, a) { e, l, h, (unsigned char)(g), \
                                      (unsigned char)(ed), (unsigned char)(a) },
    FILE_TYPE_TABLE(FT_ENTRY)
#undef FT_ENTRY
};
#define N_TYPES ((int)(sizeof(g_types) / sizeof(g_types[0])))

static struct file_assoc g_over[ASSOC_MAX];
static int               g_over_count;

/* ---- helpers ---------------------------------------------------------- */

static void fa_strcpy(char *dst, const char *src, int maxlen) {
    int i = 0;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] && i < maxlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int fa_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* A dot in a DIRECTORY name is not an extension, so restart at separators. */
static const char *get_extension(const char *filename) {
    const char *dot = NULL;
    if (!filename) return NULL;
    for (const char *p = filename; *p; p++) {
        if (*p == '/' || *p == '\\') dot = NULL;
        else if (*p == '.') dot = p + 1;
    }
    if (!dot || !*dot) return NULL;
    return dot;
}

static const struct file_type *find_type(const char *filename) {
    const char *ext = get_extension(filename);
    if (!ext) return NULL;
    for (int i = 0; i < N_TYPES; i++)
        if (fa_ieq(g_types[i].ext, ext)) return &g_types[i];
    return NULL;
}

static const struct file_assoc *find_override(const char *filename) {
    const char *ext = get_extension(filename);
    if (!ext) return NULL;
    for (int i = 0; i < g_over_count; i++)
        if (fa_ieq(g_over[i].extension, ext)) return &g_over[i];
    return NULL;
}

/* ---- public API ------------------------------------------------------- */

void file_assoc_init(void) {
    g_over_count = 0;
    kprintf("[file_assoc] %d file types known (shared table)\n", N_TYPES);
}

int file_assoc_register(const char *ext, const char *app,
                        const char *icon, const char *mime) {
    if (!ext) return -1;
    if (*ext == '.') ext++;
    if (!*ext) return -1;

    for (int i = 0; i < g_over_count; i++) {
        if (fa_ieq(g_over[i].extension, ext)) {
            fa_strcpy(g_over[i].app_path,  app,  FA_PATH_MAX);
            fa_strcpy(g_over[i].icon_name, icon, 32);
            fa_strcpy(g_over[i].mime_type, mime, 64);
            return 0;
        }
    }
    if (g_over_count >= ASSOC_MAX) return -1;
    struct file_assoc *a = &g_over[g_over_count++];
    fa_strcpy(a->extension, ext,  FA_EXT_MAX);
    fa_strcpy(a->app_path,  app,  FA_PATH_MAX);
    fa_strcpy(a->icon_name, icon, 32);
    fa_strcpy(a->mime_type, mime, 64);
    return 0;
}

const char *file_assoc_get_app(const char *filename) {
    const struct file_assoc *o = find_override(filename);
    if (o) return o->app_path[0] ? o->app_path : NULL;
    const struct file_type *t = find_type(filename);
    if (t && t->handler && t->handler[0]) return t->handler;
    return NULL;
}

const char *file_assoc_get_icon(const char *filename) {
    const struct file_assoc *o = find_override(filename);
    if (o && o->icon_name[0]) return o->icon_name;
    const struct file_type *t = find_type(filename);
    if (!t) return "unknown_file";
    switch (t->group) {
    case FTG_TEXT:    return "text_file";
    case FTG_CODE:    return "code_file";
    case FTG_DATA:    return "data_file";
    case FTG_WEB:     return "web_file";
    case FTG_IMAGE:   return "image_file";
    case FTG_AUDIO:   return "audio_file";
    case FTG_VIDEO:   return "video_file";
    case FTG_ARCHIVE: return "archive_file";
    case FTG_DISK:    return "disk_file";
    case FTG_EXEC:    return "exec_file";
    case FTG_FONT:    return "font_file";
    case FTG_DOC:     return "doc_file";
    default:          return "unknown_file";
    }
}

/* The label doubles as the MIME answer's human half. There is no MIME string
 * in the shared table on purpose: nothing in tobyOS dispatches on MIME, and a
 * second parallel identifier is exactly how the old copies drifted. Callers
 * that want a type name should use file_assoc_get_label(). */
const char *file_assoc_get_mime(const char *filename) {
    const struct file_assoc *o = find_override(filename);
    if (o && o->mime_type[0]) return o->mime_type;
    const struct file_type *t = find_type(filename);
    if (!t) return "application/octet-stream";
    switch (t->group) {
    case FTG_TEXT:
    case FTG_CODE:
    case FTG_DATA:  return "text/plain";
    case FTG_WEB:   return "text/html";
    case FTG_IMAGE: return "image/*";
    case FTG_AUDIO: return "audio/*";
    case FTG_VIDEO: return "video/*";
    default:        return "application/octet-stream";
    }
}

const char *file_assoc_get_label(const char *filename) {
    const struct file_type *t = find_type(filename);
    return t ? t->label : "File";
}

int file_assoc_get_action(const char *filename) {
    const struct file_type *t = find_type(filename);
    return t ? (int)t->action : (int)FTA_NONE;
}

int file_assoc_open(const char *filepath) {
    if (!filepath || !filepath[0]) return -1;

    const struct file_type *t = find_type(filepath);
    const char *app = file_assoc_get_app(filepath);
    int action = t ? (int)t->action : (int)FTA_NONE;

    /* An override supplies a handler even for a type the table says nothing
     * can open, so honour it -- that is the whole point of an override. */
    if (app && action == FTA_NONE) action = FTA_OPEN;

    if (action == FTA_RUN) {
        char *argv[] = { (char *)filepath, NULL };
        struct proc_spec spec;
        memset(&spec, 0, sizeof(spec));
        spec.path = filepath;
        spec.argc = 1;
        spec.argv = argv;
        int pid = proc_spawn(&spec);
        if (pid < 0) kprintf("[file_assoc] exec failed: %s\n", filepath);
        return pid;
    }

    if (action == FTA_NONE || !app) {
        /* Say WHICH of the two it is. "no association" for a file we named
         * "MPEG-4 video" in the very same UI reads like a bug; "nothing
         * installed opens it" is the truth. */
        kprintf("[file_assoc] %s: %s -- nothing installed opens it\n",
                filepath, t ? t->label : "unknown type");
        return -1;
    }

    char *argv[] = { (char *)app, (char *)filepath, NULL };
    struct proc_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.path = app;
    spec.argc = 2;
    spec.argv = argv;
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("[file_assoc] failed to open '%s' with '%s'\n", filepath, app);
        return -1;
    }
    kprintf("[file_assoc] opened '%s' (%s) with '%s' (pid %d)\n",
            filepath, t ? t->label : "File", app, pid);
    return pid;
}

int file_assoc_list(struct file_assoc *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < N_TYPES && n < max; i++, n++) {
        fa_strcpy(out[n].extension, g_types[i].ext,     FA_EXT_MAX);
        fa_strcpy(out[n].app_path,  g_types[i].handler, FA_PATH_MAX);
        fa_strcpy(out[n].icon_name, file_assoc_get_icon(g_types[i].ext), 32);
        fa_strcpy(out[n].mime_type, g_types[i].label,   64);
    }
    return n;
}

int file_assoc_count(void) { return N_TYPES; }
