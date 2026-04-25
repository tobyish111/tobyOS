/* users.c -- on-disk user account database (milestone 15).
 *
 * In-memory cache of up to USER_MAX entries, persisted to /data/users
 * as plain text "name:uid:gid" per line. Lookup is a linear scan;
 * USER_MAX is small so this is fast enough for everything we need
 * (login, ls -l, whoami).
 *
 * Defaults installed if /data/users is missing on boot:
 *
 *     root:0:0
 *     toby:1000:1000
 *     guest:1001:1001
 *
 * The session manager calls users_lookup_by_name() inside session_login
 * to validate the typed username and learn the user's uid/gid. The
 * shell uses users_visit() / users_lookup_by_uid() for `users` and
 * `whoami`. The VFS uses uid 0 as a "root bypass" sentinel; nothing
 * else here special-cases uid 0.
 */

#include <tobyos/users.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

static struct user g_users[USER_MAX];
static int         g_count;
static bool        g_initialised;

/* ---- tiny helpers ---- */

static void copy_capped(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static bool parse_int(const char *s, size_t n, int *out) {
    if (n == 0) return false;
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

/* Append decimal int to buf at *pos (in-place). Caller-checked space. */
static void append_int(char *buf, size_t *pos, size_t cap, int v) {
    char tmp[16]; int k = 0;
    unsigned u = (unsigned)v;
    if (u == 0) tmp[k++] = '0';
    while (u) { tmp[k++] = (char)('0' + u % 10u); u /= 10u; }
    while (k && *pos + 1 < cap) buf[(*pos)++] = tmp[--k];
}

/* ---- defaults ---- */

static const struct {
    const char *name;
    int         uid;
    int         gid;
} g_defaults[] = {
    { "root",  0,    0    },
    { "toby",  1000, 1000 },
    { "guest", 1001, 1001 },
    { 0, 0, 0 },
};

static void install_defaults(void) {
    g_count = 0;
    for (int i = 0; g_defaults[i].name; i++) {
        if (g_count >= USER_MAX) break;
        copy_capped(g_users[g_count].name, g_defaults[i].name, USER_NAME_MAX);
        g_users[g_count].uid = g_defaults[i].uid;
        g_users[g_count].gid = g_defaults[i].gid;
        g_count++;
    }
}

/* ---- parser ---- */

/* Walk `text` (size `n`) line by line and append each "name:uid:gid"
 * to the cache. Blank lines and lines whose first non-space char is
 * '#' are skipped. Replaces the cache wholesale; caller is expected
 * to have called install_defaults() first if a fallback is desired. */
static void parse_buffer(const char *text, size_t n) {
    g_count = 0;
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
            /* Find two ':' separators. */
            size_t c1 = a;
            while (c1 < j && text[c1] != ':') c1++;
            size_t c2 = (c1 < j) ? c1 + 1 : j;
            while (c2 < j && text[c2] != ':') c2++;

            if (c1 == j || c2 == j) {
                kprintf("[users] line %d: malformed -- ignored\n", line_no);
            } else {
                size_t nlen = c1 - a;
                int uid, gid;
                if (nlen == 0 || nlen >= USER_NAME_MAX) {
                    kprintf("[users] line %d: bad name length -- ignored\n",
                            line_no);
                } else if (!parse_int(&text[c1 + 1], c2 - (c1 + 1), &uid) ||
                           !parse_int(&text[c2 + 1], j  - (c2 + 1), &gid)) {
                    kprintf("[users] line %d: bad uid/gid -- ignored\n",
                            line_no);
                } else if (g_count >= USER_MAX) {
                    kprintf("[users] cache full -- dropping line %d\n",
                            line_no);
                } else {
                    /* Reject duplicates by name (first wins). */
                    bool dup = false;
                    for (int k = 0; k < g_count; k++) {
                        if (strncmp(g_users[k].name, &text[a], nlen) == 0 &&
                            g_users[k].name[nlen] == '\0') {
                            dup = true;
                            break;
                        }
                    }
                    if (dup) {
                        kprintf("[users] line %d: duplicate name -- ignored\n",
                                line_no);
                    } else {
                        struct user *u = &g_users[g_count++];
                        memcpy(u->name, &text[a], nlen);
                        u->name[nlen] = '\0';
                        u->uid = uid;
                        u->gid = gid;
                    }
                }
            }
        }

        i = j;
        while (i < n && (text[i] == '\n' || text[i] == '\r')) i++;
    }
}

/* ---- public API ---- */

const struct user *users_lookup_by_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_users[i].name, name) == 0) return &g_users[i];
    }
    return 0;
}

const struct user *users_lookup_by_uid(int uid) {
    for (int i = 0; i < g_count; i++) {
        if (g_users[i].uid == uid) return &g_users[i];
    }
    return 0;
}

int users_add(const char *name, int uid, int gid) {
    if (!name || !name[0]) return -1;
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen >= USER_NAME_MAX) return -1;
    if (g_count >= USER_MAX) {
        kprintf("[users] cannot add '%s': cache full\n", name);
        return -1;
    }
    if (users_lookup_by_name(name)) {
        kprintf("[users] cannot add '%s': name already taken\n", name);
        return -1;
    }
    if (users_lookup_by_uid(uid)) {
        kprintf("[users] cannot add '%s': uid %d already taken\n", name, uid);
        return -1;
    }
    struct user *u = &g_users[g_count++];
    copy_capped(u->name, name, USER_NAME_MAX);
    u->uid = uid;
    u->gid = gid;
    return 0;
}

void users_visit(users_visit_fn cb, void *ctx) {
    if (!cb) return;
    for (int i = 0; i < g_count; i++) cb(&g_users[i], ctx);
}

int users_save(void) {
    /* Worst case: header + USER_MAX * (name + 2x":" + 2x10-digit + "\n"). */
    size_t cap = 128 + (size_t)USER_MAX * (USER_NAME_MAX + 32);
    char *buf = (char *)kmalloc(cap);
    if (!buf) return -1;

    size_t n = 0;
    const char *header =
        "# tobyOS users (milestone 15) -- name:uid:gid\n";
    size_t hl = strlen(header);
    if (n + hl < cap) { memcpy(&buf[n], header, hl); n += hl; }

    for (int i = 0; i < g_count; i++) {
        size_t nl = strlen(g_users[i].name);
        if (n + nl + 32 >= cap) break;
        memcpy(&buf[n], g_users[i].name, nl); n += nl;
        buf[n++] = ':';
        append_int(buf, &n, cap, g_users[i].uid);
        buf[n++] = ':';
        append_int(buf, &n, cap, g_users[i].gid);
        buf[n++] = '\n';
    }

    int rc = vfs_write_all(USERS_PATH, buf, n);
    kfree(buf);
    if (rc != VFS_OK) {
        kprintf("[users] save failed: %s\n", vfs_strerror(rc));
        return -1;
    }
    kprintf("[users] saved %d entries to %s (%lu bytes)\n",
            g_count, USERS_PATH, (unsigned long)n);
    return 0;
}

void users_dump(void) {
    kprintf("[users] cache (%d entries):\n", g_count);
    for (int i = 0; i < g_count; i++) {
        kprintf("  %-16s uid=%d gid=%d\n",
                g_users[i].name, g_users[i].uid, g_users[i].gid);
    }
    if (g_count == 0) kprintf("  (empty)\n");
}

void users_init(void) {
    if (g_initialised) return;
    g_initialised = true;

    memset(g_users, 0, sizeof(g_users));
    g_count = 0;

    /* Try to load the persisted file. If absent, install defaults and
     * write them out so subsequent boots are deterministic. */
    void *buf = 0; size_t sz = 0;
    int rc = vfs_read_all(USERS_PATH, &buf, &sz);
    if (rc == VFS_OK && buf) {
        kprintf("[users] loading %s (%lu bytes)\n",
                USERS_PATH, (unsigned long)sz);
        parse_buffer((const char *)buf, sz);
        kfree(buf);
        if (g_count == 0) {
            kprintf("[users] %s parsed to zero entries -- "
                    "falling back to defaults\n", USERS_PATH);
            install_defaults();
            (void)users_save();
        }
    } else {
        kprintf("[users] %s not found (%s) -- writing defaults\n",
                USERS_PATH, vfs_strerror(rc));
        install_defaults();
        (void)users_save();
    }
    users_dump();
}
