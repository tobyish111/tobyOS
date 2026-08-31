/* fileassoc.c -- file types and their handlers, for userland.
 *
 * The table itself lives in <tobyos/file_types.h> and is shared with the
 * kernel; this file is the userland half that expands it once and answers
 * questions about it. Before the consolidation this file carried its OWN
 * twelve-entry list and had no callers at all.
 *
 * Overrides are kept separate from the built-in table rather than mutating a
 * copy of it: the built-ins are const data in .rodata, and a user changing
 * "open .txt with X" should not be able to lose the shipped default.
 */

#include <toby/fileassoc.h>
#include <string.h>

static const struct file_type g_types[] = {
#define FT_ENTRY(e, l, h, g, ed, a) { e, l, h, (unsigned char)(g), \
                                      (unsigned char)(ed), (unsigned char)(a) },
    FILE_TYPE_TABLE(FT_ENTRY)
#undef FT_ENTRY
};
#define N_TYPES ((int)(sizeof(g_types) / sizeof(g_types[0])))

/* User overrides, consulted before the built-ins. */
#define FA_OVERRIDE_MAX 32
static struct file_assoc g_over[FA_OVERRIDE_MAX];
static int               g_over_count;

/* ---- extension extraction --------------------------------------------
 * One implementation, because the three previous copies disagreed: a dot in
 * a DIRECTORY name ("/a.b/notes") is not an extension, so the scan restarts
 * at every separator. A name that is all extension (".gitignore") keeps it --
 * that is how those files are named and "Ignore rules" is the right answer. */
static const char *fa_ext(const char *name) {
    const char *dot = 0;
    if (!name) return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') dot = 0;
        else if (*p == '.') dot = p + 1;
    }
    if (!dot || !*dot) return 0;
    return dot;
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

const struct file_type *toby_file_type(const char *name) {
    const char *ext = fa_ext(name);
    if (!ext) return 0;
    for (int i = 0; i < N_TYPES; i++)
        if (fa_ieq(g_types[i].ext, ext)) return &g_types[i];
    return 0;
}

const char *toby_file_type_label(const char *name) {
    const struct file_type *t = toby_file_type(name);
    return t ? t->label : "File";
}

const char *toby_file_type_handler(const char *name) {
    const char *ext = fa_ext(name);
    if (ext) {
        for (int i = 0; i < g_over_count; i++)
            if (fa_ieq(g_over[i].extension, ext))
                return g_over[i].app_path[0] ? g_over[i].app_path : 0;
    }
    const struct file_type *t = toby_file_type(name);
    return t ? t->handler : 0;
}

int toby_file_type_action(const char *name) {
    const struct file_type *t = toby_file_type(name);
    if (!t) return FTA_NONE;
    return t->action;
}

int toby_file_type_group(const char *name) {
    const struct file_type *t = toby_file_type(name);
    return t ? (int)t->group : (int)FTG_OTHER;
}

int toby_file_type_editable(const char *name) {
    const struct file_type *t = toby_file_type(name);
    /* No extension at all -> assume text. /etc/motd, /etc/hosts, README and
     * every dotfile land here, and they are precisely what a person opens in
     * an editor. A KNOWN binary type answers 0; only the unknown gets the
     * benefit of the doubt. */
    if (!t) return 1;
    return t->editable ? 1 : 0;
}

const char *toby_file_group_name(int group) {
    switch (group) {
    case FTG_TEXT:    return "Text";
    case FTG_CODE:    return "Code";
    case FTG_DATA:    return "Data";
    case FTG_WEB:     return "Web";
    case FTG_IMAGE:   return "Image";
    case FTG_AUDIO:   return "Audio";
    case FTG_VIDEO:   return "Video";
    case FTG_ARCHIVE: return "Archive";
    case FTG_DISK:    return "Disk image";
    case FTG_EXEC:    return "Program";
    case FTG_FONT:    return "Font";
    case FTG_DOC:     return "Document";
    default:          return "File";
    }
}

int toby_file_type_count(void) { return N_TYPES; }

/* ---- legacy API ------------------------------------------------------- */

static void fa_cpy(char *d, const char *s, int cap) {
    int i = 0;
    if (!s) { d[0] = '\0'; return; }
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

/* Accepts "txt" or ".txt": callers wrote both against the old copies. */
static const char *fa_bare(const char *ext) {
    if (!ext) return 0;
    return (*ext == '.') ? ext + 1 : ext;
}

int fileassoc_register(const char *ext, const char *app_path,
                       const char *description) {
    const char *b = fa_bare(ext);
    if (!b || !*b) return -1;
    for (int i = 0; i < g_over_count; i++) {
        if (fa_ieq(g_over[i].extension, b)) {
            fa_cpy(g_over[i].app_path, app_path, 128);
            fa_cpy(g_over[i].description, description, 64);
            return 0;
        }
    }
    if (g_over_count >= FA_OVERRIDE_MAX) return -1;
    struct file_assoc *a = &g_over[g_over_count++];
    fa_cpy(a->extension, b, 8);
    fa_cpy(a->app_path, app_path, 128);
    fa_cpy(a->description, description, 64);
    a->icon[0] = '\0';
    return 0;
}

int fileassoc_unregister(const char *ext) {
    const char *b = fa_bare(ext);
    if (!b) return -1;
    for (int i = 0; i < g_over_count; i++) {
        if (fa_ieq(g_over[i].extension, b)) {
            for (int j = i; j < g_over_count - 1; j++) g_over[j] = g_over[j + 1];
            g_over_count--;
            return 0;   /* the built-in default is what shows through now */
        }
    }
    return -1;
}

/* Takes a bare extension, unlike toby_file_type_handler which takes a name.
 * Build a dotted name so both go through one lookup. */
const char *fileassoc_get_app(const char *ext) {
    const char *b = fa_bare(ext);
    if (!b || !*b) return 0;
    char name[72];
    name[0] = 'x'; name[1] = '.';
    int i = 0;
    while (b[i] && i < (int)sizeof(name) - 3) { name[2 + i] = b[i]; i++; }
    name[2 + i] = '\0';
    return toby_file_type_handler(name);
}

int fileassoc_get_all(struct file_assoc *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < N_TYPES && n < max; i++, n++) {
        fa_cpy(out[n].extension,   g_types[i].ext,     8);
        fa_cpy(out[n].app_path,    g_types[i].handler, 128);
        fa_cpy(out[n].description, g_types[i].label,   64);
        fa_cpy(out[n].icon, toby_file_group_name((int)g_types[i].group), 32);
    }
    return n;
}

int fileassoc_set_default(const char *ext, const char *app_path) {
    return fileassoc_register(ext, app_path, 0);
}
