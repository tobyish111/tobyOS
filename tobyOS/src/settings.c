/* settings.c -- in-memory cache backed by /data/settings.conf.
 *
 * On boot:
 *   1. settings_init() seeds the cache with built-in defaults.
 *   2. If /data/settings.conf exists, read + parse it: each non-blank,
 *      non-comment line of the form "key=value" is upserted into the
 *      cache. Anything that doesn't parse is ignored (with a kprintf).
 *   3. If the file did NOT exist, write a fresh copy with the
 *      defaults so future boots see something on disk.
 *
 * At runtime:
 *   - settings_get_*() never blocks; it's a linear scan of <=32 slots.
 *   - settings_set_str() upserts; the caller chooses when to call
 *     settings_save() to push the change through to disk. Most users
 *     (the GUI Settings app, session_login persisting user.last) save
 *     immediately.
 *
 * The on-disk format is line-oriented plain text so a human can edit
 * it by hand from the shell or via the gui_files viewer. We keep the
 * parser deliberately permissive but strict about lengths -- anything
 * longer than SETTING_KEY_MAX/SETTING_VAL_MAX is truncated and the
 * truncation is logged.
 */

#include <tobyos/settings.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

struct setting_entry {
    char key[SETTING_KEY_MAX];
    char val[SETTING_VAL_MAX];
    bool used;
};

static struct setting_entry g_entries[SETTING_MAX_ENTRIES];
static bool                 g_initialised;

/* Built-in defaults installed if SETTINGS_PATH is missing on boot. The
 * desktop and login screen both read from this list, so even on a
 * fresh disk the OS comes up with sensible visuals. */
static const struct {
    const char *key;
    const char *val;
} g_defaults[] = {
    { "desktop.bg",      "0x00204060" },   /* same blue the compositor used to hard-code */
    { "desktop.title",   "tobyOS"     },
    { "desktop.greeting", "Welcome to tobyOS" },
    { "user.last",       "toby"       },
    { "user.greeting",   "Hello, friend!" },
    { "session.autostart", ""         },   /* reserved -- comma-separated apps */
    { 0, 0 },
};

/* ---- tiny string helpers (avoid pulling in printf for parsing) ---- */

static void copy_capped(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static struct setting_entry *find(const char *key) {
    if (!key) return 0;
    for (int i = 0; i < SETTING_MAX_ENTRIES; i++) {
        if (g_entries[i].used && strcmp(g_entries[i].key, key) == 0) {
            return &g_entries[i];
        }
    }
    return 0;
}

static struct setting_entry *alloc_slot(void) {
    for (int i = 0; i < SETTING_MAX_ENTRIES; i++) {
        if (!g_entries[i].used) return &g_entries[i];
    }
    return 0;
}

static int set_internal(const char *key, const char *val) {
    if (!key || !key[0]) return -1;
    struct setting_entry *e = find(key);
    if (!e) {
        e = alloc_slot();
        if (!e) {
            kprintf("[settings] ERROR: cache full, dropping '%s'\n", key);
            return -1;
        }
        copy_capped(e->key, key, SETTING_KEY_MAX);
        e->used = true;
    }
    copy_capped(e->val, val ? val : "", SETTING_VAL_MAX);
    return 0;
}

/* ---- defaults --------------------------------------------------- */

static void install_defaults(void) {
    for (int i = 0; g_defaults[i].key; i++) {
        set_internal(g_defaults[i].key, g_defaults[i].val);
    }
}

/* ---- parser ----------------------------------------------------- */

/* Walk `text` (size `n`) line by line and upsert each "key=value" we
 * find. Blank lines and lines whose first non-space char is '#' are
 * skipped. Long keys/values are truncated with a warning. */
static void parse_buffer(const char *text, size_t n) {
    size_t i = 0;
    int line_no = 0;
    while (i < n) {
        line_no++;

        /* Locate end of line. */
        size_t j = i;
        while (j < n && text[j] != '\n' && text[j] != '\r') j++;

        /* Skip leading whitespace. */
        size_t a = i;
        while (a < j && (text[a] == ' ' || text[a] == '\t')) a++;

        bool comment = (a < j && text[a] == '#');
        bool blank   = (a == j);

        if (!comment && !blank) {
            /* Find '='. */
            size_t eq = a;
            while (eq < j && text[eq] != '=') eq++;
            if (eq == j) {
                kprintf("[settings] line %d: no '=' -- ignored\n", line_no);
            } else {
                char k[SETTING_KEY_MAX];
                char v[SETTING_VAL_MAX];
                size_t klen = eq - a;
                size_t vlen = j  - (eq + 1);
                if (klen >= SETTING_KEY_MAX) {
                    kprintf("[settings] line %d: key truncated (%lu bytes)\n",
                            line_no, (unsigned long)klen);
                    klen = SETTING_KEY_MAX - 1;
                }
                if (vlen >= SETTING_VAL_MAX) {
                    kprintf("[settings] line %d: val truncated (%lu bytes)\n",
                            line_no, (unsigned long)vlen);
                    vlen = SETTING_VAL_MAX - 1;
                }
                memcpy(k, &text[a],     klen); k[klen] = 0;
                memcpy(v, &text[eq + 1], vlen); v[vlen] = 0;
                set_internal(k, v);
            }
        }

        /* Eat the newline (handle \r\n too). */
        i = j;
        while (i < n && (text[i] == '\n' || text[i] == '\r')) i++;
    }
}

/* ---- u32 parsing ------------------------------------------------ */

static bool parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t v = 0;
    int i = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
        if (!s[i]) return false;
        for (; s[i]; i++) {
            char c = s[i];
            uint32_t d;
            if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(10 + c - 'a');
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(10 + c - 'A');
            else return false;
            v = v * 16u + d;
        }
    } else {
        for (; s[i]; i++) {
            char c = s[i];
            if (c < '0' || c > '9') return false;
            v = v * 10u + (uint32_t)(c - '0');
        }
    }
    *out = v;
    return true;
}

/* ---- public API ------------------------------------------------- */

size_t settings_get_str(const char *key, char *out, size_t cap, const char *def) {
    if (!out || cap == 0) return 0;
    struct setting_entry *e = find(key);
    const char *src = e ? e->val : (def ? def : "");
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) out[i] = src[i];
    out[i] = '\0';
    return i;
}

uint32_t settings_get_u32(const char *key, uint32_t def) {
    struct setting_entry *e = find(key);
    if (!e) return def;
    uint32_t v;
    if (!parse_u32(e->val, &v)) return def;
    return v;
}

int settings_set_str(const char *key, const char *val) {
    return set_internal(key, val);
}

/* Build the on-disk image and write it via vfs_write_all. We assemble
 * into a heap buffer first so the file write is one contiguous shot
 * (avoids partially-updated state if the FS is interrupted -- our
 * tobyfs is not journaled, so this is best-effort). */
int settings_save(void) {
    /* Worst case: every slot used, max-length key + max-length val
     * + '=' + '\n' + a small header. */
    size_t cap = 64 + (size_t)SETTING_MAX_ENTRIES *
                       (SETTING_KEY_MAX + SETTING_VAL_MAX + 4);
    char *buf = (char *)kmalloc(cap);
    if (!buf) return -1;

    size_t n = 0;
    const char *header =
        "# tobyOS settings (milestone 14) -- key=value, '#' starts a comment\n";
    size_t hl = strlen(header);
    if (n + hl < cap) { memcpy(&buf[n], header, hl); n += hl; }

    for (int i = 0; i < SETTING_MAX_ENTRIES; i++) {
        if (!g_entries[i].used) continue;
        size_t kl = strlen(g_entries[i].key);
        size_t vl = strlen(g_entries[i].val);
        if (n + kl + 1 + vl + 1 >= cap) break;
        memcpy(&buf[n], g_entries[i].key, kl); n += kl;
        buf[n++] = '=';
        memcpy(&buf[n], g_entries[i].val, vl); n += vl;
        buf[n++] = '\n';
    }

    int rc = vfs_write_all(SETTINGS_PATH, buf, n);
    kfree(buf);
    if (rc != VFS_OK) {
        kprintf("[settings] save failed: %s\n", vfs_strerror(rc));
        return -1;
    }
    kprintf("[settings] saved %d entries to %s (%lu bytes)\n",
            (int)n /* cosmetic */, SETTINGS_PATH, (unsigned long)n);
    return 0;
}

void settings_dump(void) {
    kprintf("[settings] cache:\n");
    int n = 0;
    for (int i = 0; i < SETTING_MAX_ENTRIES; i++) {
        if (!g_entries[i].used) continue;
        kprintf("  %s=%s\n", g_entries[i].key, g_entries[i].val);
        n++;
    }
    if (n == 0) kprintf("  (empty)\n");
}

void settings_init(void) {
    if (g_initialised) return;
    g_initialised = true;

    memset(g_entries, 0, sizeof(g_entries));
    install_defaults();

    /* Try to load the persisted file. If absent, write defaults out so
     * the next boot picks them up unchanged. */
    void *buf = 0; size_t sz = 0;
    int rc = vfs_read_all(SETTINGS_PATH, &buf, &sz);
    if (rc == VFS_OK && buf) {
        kprintf("[settings] loading %s (%lu bytes)\n",
                SETTINGS_PATH, (unsigned long)sz);
        parse_buffer((const char *)buf, sz);
        kfree(buf);
    } else {
        kprintf("[settings] %s not found (%s) -- writing defaults\n",
                SETTINGS_PATH, vfs_strerror(rc));
        (void)settings_save();
    }
    settings_dump();
}
