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
#include <tobyos/rng.h>
#include <monocypher.h>

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
        g_users[g_count].password_hash[0] = '\0';
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
            /* Find colon separators: name:uid:gid[:password_hash] */
            size_t c1 = a;
            while (c1 < j && text[c1] != ':') c1++;
            size_t c2 = (c1 < j) ? c1 + 1 : j;
            while (c2 < j && text[c2] != ':') c2++;

            /* Optional 3rd colon for password hash */
            size_t c3 = (c2 < j) ? c2 + 1 : j;
            while (c3 < j && text[c3] != ':') c3++;

            if (c1 == j || c2 == j) {
                kprintf("[users] line %d: malformed -- ignored\n", line_no);
            } else {
                size_t nlen = c1 - a;
                /* gid ends at c3 (next colon) or j (end of line) */
                size_t gid_end = c3;
                int uid, gid;
                if (nlen == 0 || nlen >= USER_NAME_MAX) {
                    kprintf("[users] line %d: bad name length -- ignored\n",
                            line_no);
                } else if (!parse_int(&text[c1 + 1], c2 - (c1 + 1), &uid) ||
                           !parse_int(&text[c2 + 1], gid_end - (c2 + 1), &gid)) {
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
                        /* Parse optional password_hash (4th field after 3rd colon) */
                        u->password_hash[0] = '\0';
                        if (c3 < j) {
                            size_t hstart = c3 + 1;
                            size_t hlen = j - hstart;
                            if (hlen > USER_PWHASH_MAX - 1) hlen = USER_PWHASH_MAX - 1;
                            if (hlen > 0) {
                                memcpy(u->password_hash, &text[hstart], hlen);
                                u->password_hash[hlen] = '\0';
                            }
                        }
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
    u->password_hash[0] = '\0';
    return 0;
}

void users_visit(users_visit_fn cb, void *ctx) {
    if (!cb) return;
    for (int i = 0; i < g_count; i++) cb(&g_users[i], ctx);
}

int users_save(void) {
    /* Worst case: header + USER_MAX * (name + 3x":" + 2x10-digit + hash + "\n"). */
    size_t cap = 128 + (size_t)USER_MAX * (USER_NAME_MAX + 32 + USER_PWHASH_MAX);
    char *buf = (char *)kmalloc(cap);
    if (!buf) return -1;

    size_t n = 0;
    const char *header =
        "# tobyOS users (milestone 15) -- name:uid:gid\n";
    size_t hl = strlen(header);
    if (n + hl < cap) { memcpy(&buf[n], header, hl); n += hl; }

    for (int i = 0; i < g_count; i++) {
        size_t nl = strlen(g_users[i].name);
        if (n + nl + 32 + USER_PWHASH_MAX >= cap) break;
        memcpy(&buf[n], g_users[i].name, nl); n += nl;
        buf[n++] = ':';
        append_int(buf, &n, cap, g_users[i].uid);
        buf[n++] = ':';
        append_int(buf, &n, cap, g_users[i].gid);
        if (g_users[i].password_hash[0]) {
            buf[n++] = ':';
            size_t hl = strlen(g_users[i].password_hash);
            memcpy(&buf[n], g_users[i].password_hash, hl);
            n += hl;
        }
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

/* ---- password hashing (salted Argon2id) ----
 *
 * Stored credential format (no ':' so it survives the name:uid:gid:hash
 * line parser):
 *
 *     $argon2id$m=<kib>,t=<passes>$<salt_hex>$<hash_hex>
 *
 * A bare 16-hex-char string is the legacy unsalted djb2 digest; it is
 * still accepted on login (backward compat) and transparently rehashed
 * to Argon2id on the next successful login. */

#define PW_ARGON2_KIB     1024u   /* nb_blocks: 1 MiB work area */
#define PW_ARGON2_PASSES  3u
#define PW_SALT_LEN       16u
#define PW_HASH_LEN       32u

static const char pw_hextab[] = "0123456789abcdef";

static void pw_bytes_to_hex(const uint8_t *in, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = pw_hextab[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = pw_hextab[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static int pw_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse exactly `outcap` bytes worth of hex from `in` (2*outcap chars).
 * Returns the number of bytes written, or -1 on a bad/short digit run. */
static int pw_hex_to_bytes(const char *in, uint8_t *out, size_t outcap) {
    for (size_t i = 0; i < outcap; i++) {
        int hi = pw_hexval(in[i * 2]);
        int lo = pw_hexval(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)outcap;
}

/* Argon2id(password, salt) -> out[PW_HASH_LEN]. false if work-area alloc fails. */
static bool pw_argon2(const char *password, const uint8_t *salt, uint32_t salt_len,
                      uint32_t kib, uint32_t passes, uint8_t *out) {
    size_t plen = 0;
    if (password) while (password[plen]) plen++;

    size_t work_sz = (size_t)kib * 1024u;
    void *work = kmalloc(work_sz);
    if (!work) {
        kprintf("[users] argon2: work-area alloc (%u KiB) failed\n", kib);
        return false;
    }
    crypto_argon2_config cfg = { CRYPTO_ARGON2_ID, kib, passes, 1 };
    crypto_argon2_inputs in  = {
        (const uint8_t *)(password ? password : ""), salt,
        (uint32_t)plen, salt_len
    };
    crypto_argon2(out, PW_HASH_LEN, work, cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work, work_sz);
    kfree(work);
    return true;
}

/* Hash `password` with a fresh random salt and write the encoded credential
 * into dst (USER_PWHASH_MAX bytes). Returns 0 on success. */
static int pw_make(char *dst, const char *password) {
    uint8_t salt[PW_SALT_LEN];
    uint8_t hash[PW_HASH_LEN];
    rng_fill(salt, sizeof salt);
    if (!pw_argon2(password, salt, PW_SALT_LEN, PW_ARGON2_KIB, PW_ARGON2_PASSES, hash))
        return -1;

    char salt_hex[PW_SALT_LEN * 2 + 1];
    char hash_hex[PW_HASH_LEN * 2 + 1];
    pw_bytes_to_hex(salt, PW_SALT_LEN, salt_hex);
    pw_bytes_to_hex(hash, PW_HASH_LEN, hash_hex);

    int w = ksnprintf(dst, USER_PWHASH_MAX, "$argon2id$m=%u,t=%u$%s$%s",
                      PW_ARGON2_KIB, PW_ARGON2_PASSES, salt_hex, hash_hex);
    crypto_wipe(hash, sizeof hash);
    if (w < 0 || (size_t)w >= USER_PWHASH_MAX) return -1;
    return 0;
}

/* Verify `password` against an encoded Argon2id credential. */
static bool pw_verify_argon2(const char *stored, const char *password) {
    /* stored == "$argon2id$m=<kib>,t=<passes>$<salt_hex>$<hash_hex>" */
    const char *p = stored;
    const char *pfx = "$argon2id$m=";
    size_t pl = strlen(pfx);
    if (strncmp(p, pfx, pl) != 0) return false;
    p += pl;

    /* kib */
    uint32_t kib = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') { kib = kib * 10 + (uint32_t)(*p - '0'); p++; }
    if (strncmp(p, ",t=", 3) != 0) return false;
    p += 3;
    /* passes */
    uint32_t passes = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') { passes = passes * 10 + (uint32_t)(*p - '0'); p++; }
    if (*p != '$') return false;
    p++;

    /* salt_hex (PW_SALT_LEN*2) then '$' then hash_hex (PW_HASH_LEN*2) */
    const char *salt_hex = p;
    uint8_t salt[PW_SALT_LEN];
    if (pw_hex_to_bytes(salt_hex, salt, PW_SALT_LEN) < 0) return false;
    p += PW_SALT_LEN * 2;
    if (*p != '$') return false;
    p++;
    const char *hash_hex = p;
    uint8_t want[PW_HASH_LEN];
    if (pw_hex_to_bytes(hash_hex, want, PW_HASH_LEN) < 0) return false;
    if (p[PW_HASH_LEN * 2] != '\0') return false;  /* trailing garbage */

    /* Sanity-bound the cost params we'll actually run (don't let a corrupt
     * file ask us to allocate gigabytes). */
    if (kib == 0 || kib > 8192 || passes == 0 || passes > 16) return false;

    uint8_t got[PW_HASH_LEN];
    bool ok = pw_argon2(password, salt, PW_SALT_LEN, kib, passes, got);
    if (!ok) return false;
    int eq = crypto_verify32(got, want);   /* constant-time, 0 == equal */
    crypto_wipe(got, sizeof got);
    crypto_wipe(want, sizeof want);
    return eq == 0;
}

/* Verify against the legacy unsalted djb2 digest (16 lowercase hex chars). */
static bool pw_verify_legacy_djb2(const char *stored, const char *password) {
    uint64_t hash = 5381;
    const char *p = password ? password : "";
    for (; *p; p++)
        hash = ((hash << 5) + hash) + (unsigned char)*p;
    char hex[17];
    for (int i = 15; i >= 0; i--) {
        hex[i] = pw_hextab[hash & 0xF];
        hash >>= 4;
    }
    hex[16] = '\0';
    return strcmp(stored, hex) == 0;
}

bool users_check_password(const char *username, const char *password) {
    const struct user *u = users_lookup_by_name(username);
    if (!u) return false;
    if (u->password_hash[0] == '\0') return true;   /* no password set */

    if (u->password_hash[0] == '$')
        return pw_verify_argon2(u->password_hash, password);

    /* Legacy djb2 digest. Verify, and on success transparently upgrade the
     * stored credential to salted Argon2id so the weak hash goes away. */
    if (pw_verify_legacy_djb2(u->password_hash, password)) {
        struct user *mu = (struct user *)u;   /* points into g_users cache */
        char upgraded[USER_PWHASH_MAX];
        if (pw_make(upgraded, password) == 0) {
            memcpy(mu->password_hash, upgraded, USER_PWHASH_MAX);
            crypto_wipe(upgraded, sizeof upgraded);
            (void)users_save();
            kprintf("[users] upgraded '%s' password from djb2 to argon2id\n",
                    username);
        }
        return true;
    }
    return false;
}

int users_set_password(const char *username, const char *password) {
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_users[i].name, username) == 0) {
            if (!password || !password[0]) {
                g_users[i].password_hash[0] = '\0';
                users_save();
                return 0;
            }
            if (pw_make(g_users[i].password_hash, password) != 0) {
                kprintf("[users] set_password '%s' failed (hash error)\n",
                        username);
                return -1;
            }
            users_save();
            return 0;
        }
    }
    return -1;
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
