/* pkg.c -- minimal tobyOS package manager (milestone 16).
 *
 * Three operations drive everything:
 *   install  -- parse a .tpkg, extract every FILE, register every APP
 *   remove   -- walk the install record, unlink every file, forget the pkg
 *   list     -- enumerate /data/packages/
 *
 * Design notes:
 *
 * - Package bodies are loaded via vfs_read_all into one kmalloc'd
 *   buffer. .tpkg files are small (a few KiB to a few dozen KiB) and
 *   the heap can comfortably hold them. If a package ever grew past
 *   the per-file tobyfs ceiling (64 KiB) install would fail cleanly
 *   on the write side.
 *
 * - All FILE destinations MUST start with "/data/". We refuse to
 *   write anywhere else -- the ramfs root is read-only, settings/
 *   users/session files live under /data/ already, and forcing the
 *   prefix lets the user safely `pkg remove` a package later knowing
 *   nothing outside /data has been touched.
 *
 * - install collides LOUDLY if a package with the same name is already
 *   installed or if a FILE destination exists but isn't owned by this
 *   package. No automatic upgrade, no partial writes. The user is
 *   expected to `pkg remove` first.
 *
 * - remove is deliberately tolerant: missing files (already gone) are
 *   warnings, not errors. Unlinking stops on the FIRST real error so
 *   we don't leak a half-deleted install.
 *
 * - After every successful install/remove we call pkg_refresh_launcher()
 *   which clears the dynamic slice of the launcher menu and re-scans
 *   the .app descriptors in /data/apps. This keeps the desktop UI in
 *   sync without needing any explicit "you must reboot" step.
 */

#include <tobyos/pkg.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/http.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/gui.h>
#include <tobyos/proc.h>
#include <tobyos/cap.h>
#include <tobyos/sec.h>
#include <tobyos/sysprot.h>
#include <tobyos/slog.h>

#define PKG_MAX_FILES       32          /* per package */
#define PKG_MAX_APPS        8           /* per package */
#define PKG_HEADER_LINE_MAX 256         /* one header line */

/* ---- privilege scope --------------------------------------------- *
 *
 * `pkg install/remove/upgrade/rollback` all need to write under
 * /data/apps/, /data/packages/, etc. The VFS gates writes by
 * current_proc()->uid, so a non-root caller -- e.g. /bin/gui_term
 * running inside a logged-in user's session -- gets VFS_ERR_PERM.
 *
 * In tobyOS the package operations are intentionally not
 * authenticated: there is no password system, and any logged-in user
 * is allowed to install apps from /repo onto the local box. We
 * therefore borrow the kernel's root identity for the duration of
 * each public pkg_* mutating call and restore the caller's identity
 * on exit. The save/restore is a no-op when the caller is already
 * root (boot init, kernel selftests, the serial-console shell).
 *
 * NOTE: this does NOT bypass cap_check_path -- the VFS still applies
 * the path-sandbox capability gate independently of uid. pkg.c is
 * only invoked from the kernel shell and the GUI terminal builtin,
 * both of which already have full caps; if you ever wire pkg into a
 * sandboxed app you'll need to widen its sandbox or add a privileged
 * service in front of it.
 */
struct pkg_priv {
    int                       saved_uid;
    int                       saved_gid;
    bool                      uid_swapped;     /* uid/gid were touched */
    /* Milestone 34E: every privileged pkg op also enters a sysprot
     * scope so its writes under /system, /data/packages, /data/users,
     * etc. reach the disk. The scope is opened unconditionally (even
     * when uid was already 0) so a kernel-context call still gets the
     * paired begin/end pattern -- helps the audit log line up. */
    struct sysprot_priv_scope sysprot;
};

static void pkg_priv_begin(struct pkg_priv *s) {
    s->saved_uid   = 0;
    s->saved_gid   = 0;
    s->uid_swapped = false;
    sysprot_priv_begin(&s->sysprot);
    struct proc *p = current_proc();
    if (!p) return;                              /* boot context: no proc */
    if (p->uid == 0 && p->gid == 0) return;      /* already root          */
    s->saved_uid   = p->uid;
    s->saved_gid   = p->gid;
    p->uid         = 0;
    p->gid         = 0;
    s->uid_swapped = true;
}

static void pkg_priv_end(struct pkg_priv *s) {
    if (s->uid_swapped) {
        struct proc *p = current_proc();
        if (p) {
            p->uid = s->saved_uid;
            p->gid = s->saved_gid;
        }
        s->uid_swapped = false;
    }
    sysprot_priv_end(&s->sysprot);
}

/* ---- tiny string helpers ------------------------------------------ */

static void copy_capped(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static bool starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static bool ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lt = strlen(suffix);
    if (lt > ls) return false;
    return strcmp(s + (ls - lt), suffix) == 0;
}

/* Decimal parse with an explicit end pointer. Returns true on success;
 * *out holds the parsed value, *end points at the first non-digit. */
static bool parse_uint(const char *s, const char *limit,
                       size_t *out, const char **end) {
    if (s >= limit || *s < '0' || *s > '9') return false;
    size_t v = 0;
    while (s < limit && *s >= '0' && *s <= '9') {
        v = v * 10 + (size_t)(*s - '0');
        s++;
    }
    *out = v;
    if (end) *end = s;
    return true;
}

/* Locate the leaf basename inside a path. Returns a pointer into the
 * original string (never NULL). "/a/b/c" -> "c", "c" -> "c". */
static const char *path_basename(const char *p) {
    const char *last = p;
    for (const char *c = p; *c; c++) if (*c == '/') last = c + 1;
    return last;
}

/* ---- mkdir -p: create every missing ancestor of `path` ----------- *
 *
 * Walks a path "/a/b/c/leaf" and mkdirs "/a", "/a/b", "/a/b/c". Uses
 * a local buffer (no dynamic alloc). Existing dirs aren't errors.
 * Returns VFS_OK on full success; bails on the first real error.
 *
 * We stat each prefix first: if it already exists as a directory we
 * skip mkdir for it (this matters at mount-point boundaries like
 * "/data", where vfs_mkdir would refuse with INVAL because the mount
 * root can't be re-created). Anything still missing is created with
 * vfs_mkdir; we tolerate EXIST in case of races. */
static int mkdir_parents(const char *full_path) {
    if (!full_path || full_path[0] != '/') return VFS_ERR_INVAL;
    char buf[VFS_PATH_MAX];
    size_t len = strlen(full_path);
    if (len + 1 > sizeof(buf)) return VFS_ERR_NAMETOOLONG;
    memcpy(buf, full_path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';

        struct vfs_stat st;
        int srt = vfs_stat(buf, &st);
        if (srt == VFS_OK) {
            if (st.type != VFS_TYPE_DIR) {
                /* A non-directory component is in the way -- give up. */
                buf[i] = '/';
                return VFS_ERR_NOTDIR;
            }
            /* Already a directory (or a mount-point root). Skip. */
            buf[i] = '/';
            continue;
        }
        int rc = vfs_mkdir(buf);
        if (rc != VFS_OK && rc != VFS_ERR_EXIST) {
            /* Real problem: ROFS / OOM / NOSPC / INVAL on a path
             * we couldn't even stat. Propagate. */
            buf[i] = '/';
            return rc;
        }
        buf[i] = '/';
    }
    return VFS_OK;
}

/* ---- header parser ------------------------------------------------ */

/* One parsed FILE declaration. `size` is only populated from the tpkg
 * header; install records store files-only (no sizes). */
struct pkg_file_decl {
    char   dest[PKG_PATH_MAX];
    size_t size;                    /* 0 for install-record entries */
    /* M34A: per-file SHA-256 digest, hex-encoded (no NUL).
     * `has_hash` is true iff a matching FHASH line was present. */
    char   hash_hex[PKG_HASH_HEX_LEN + 1];
    bool   has_hash;
};

struct pkg_app_decl {
    char label[PKG_LABEL_MAX];
    char exec[PKG_PATH_MAX];
};

struct pkg_manifest {
    char                 name   [PKG_NAME_MAX];
    char                 version[PKG_VERSION_MAX];
    char                 desc   [PKG_DESC_MAX];
    /* Milestone 18: sandbox profile name that apps declared by this
     * package run under when launched via the desktop. Empty string
     * means "inherit" -- the launcher (or drain_launch_queue) will
     * substitute "default" so user apps don't accidentally run with
     * ADMIN inherited from pid 0. Unknown names are accepted at
     * parse time and logged + ignored at spawn time. */
    char                 sandbox[PKG_SANDBOX_MAX];

    /* M34A integrity metadata. Whole-package body SHA-256 (hex). */
    char                 publisher[PKG_PUBLISHER_MAX];
    char                 hash_hex [PKG_HASH_HEX_LEN + 1];
    bool                 has_hash;

    /* M34D capability declarations (raw comma-separated, normalised
     * to upper-case during parse). The kernel cap subsystem
     * interprets it at app launch time (see cap.c). */
    char                 caps[PKG_CAPS_LIST_MAX];

    /* M34C signature: "<key-id> <hex-tag>". Empty when unsigned. */
    char                 sig_key[PKG_KEYID_MAX];
    char                 sig_hex[PKG_HASH_HEX_LEN + 1];
    bool                 has_sig;

    struct pkg_file_decl files[PKG_MAX_FILES];
    int                  file_count;

    struct pkg_app_decl  apps[PKG_MAX_APPS];
    int                  app_count;

    /* Offset (in the original .tpkg buffer) of the first byte of the
     * BODY section. 0 when parsing an install record (no body). */
    size_t               body_offset;
};

/* Returns pointer to byte AFTER the '\n' (one past the line). If the
 * line has no terminator, treats end-of-buffer as the end. Writes the
 * (inclusive) line start/length into *out_line, *out_len. */
static const char *next_line(const char *p, const char *end,
                             const char **out_line, size_t *out_len) {
    const char *start = p;
    while (p < end && *p != '\n') p++;
    *out_line = start;
    *out_len  = (size_t)(p - start);
    /* Strip a trailing \r so Windows-authored headers still parse. */
    if (*out_len > 0 && start[*out_len - 1] == '\r') (*out_len)--;
    if (p < end) p++;
    return p;
}

/* Parse "APP <label>|<exec>" payload (everything after "APP "). */
static int parse_app_line(const char *p, size_t n, struct pkg_app_decl *out) {
    const char *bar = 0;
    for (size_t i = 0; i < n; i++) if (p[i] == '|') { bar = p + i; break; }
    if (!bar) {
        kprintf("[pkg] malformed APP line (no '|')\n");
        return -1;
    }
    size_t llen = (size_t)(bar - p);
    size_t elen = n - llen - 1;
    if (llen >= sizeof(out->label) || elen >= sizeof(out->exec)) {
        kprintf("[pkg] APP label/exec too long\n");
        return -1;
    }
    memcpy(out->label, p, llen);   out->label[llen] = '\0';
    memcpy(out->exec,  bar + 1, elen); out->exec[elen] = '\0';
    if (out->exec[0] != '/') {
        kprintf("[pkg] APP exec must be absolute: '%s'\n", out->exec);
        return -1;
    }
    return 0;
}

/* Parse "FILE <path> <size>". install records skip the <size>. */
static int parse_file_line(const char *p, size_t n,
                           struct pkg_file_decl *out, bool expect_size) {
    memset(out, 0, sizeof(*out));
    /* Locate space separator between path and size. */
    const char *sp = 0;
    for (size_t i = 0; i < n; i++) if (p[i] == ' ') { sp = p + i; break; }

    const char *path_end = sp ? sp : (p + n);
    size_t plen = (size_t)(path_end - p);
    if (plen == 0 || plen >= sizeof(out->dest)) {
        kprintf("[pkg] FILE path missing or too long\n");
        return -1;
    }
    memcpy(out->dest, p, plen); out->dest[plen] = '\0';
    if (out->dest[0] != '/') {
        kprintf("[pkg] FILE path must be absolute: '%s'\n", out->dest);
        return -1;
    }

    if (!expect_size) {
        out->size = 0;
        return 0;
    }
    if (!sp) {
        kprintf("[pkg] FILE line missing size: '%s'\n", out->dest);
        return -1;
    }
    const char *num_start = sp + 1;
    const char *num_end   = 0;
    size_t sz;
    if (!parse_uint(num_start, p + n, &sz, &num_end)) {
        kprintf("[pkg] FILE bad size for '%s'\n", out->dest);
        return -1;
    }
    out->size = sz;
    return 0;
}

/* Parse "FHASH <abs-path> <64-hex>". Records the hash on the matching
 * FILE entry; we accept FHASH lines in any order relative to FILE
 * lines (a small linear scan to find the match, M is small). */
static int parse_fhash_line(const char *p, size_t n,
                            struct pkg_manifest *out) {
    /* Find the LAST space: hex is the trailing token, path may have
     * embedded spaces in theory (we still cap at PKG_PATH_MAX). */
    const char *sp = 0;
    for (size_t i = 0; i < n; i++) if (p[i] == ' ') sp = p + i;
    if (!sp) {
        kprintf("[pkg] FHASH missing hash field\n");
        return -1;
    }
    size_t plen = (size_t)(sp - p);
    if (plen == 0 || plen >= PKG_PATH_MAX) {
        kprintf("[pkg] FHASH path missing or too long\n");
        return -1;
    }
    char path[PKG_PATH_MAX];
    memcpy(path, p, plen); path[plen] = '\0';
    if (path[0] != '/') {
        kprintf("[pkg] FHASH path must be absolute: '%s'\n", path);
        return -1;
    }
    size_t hlen = (size_t)((p + n) - (sp + 1));
    if (hlen != PKG_HASH_HEX_LEN) {
        kprintf("[pkg] FHASH '%s' wrong hash length (got %lu, need %d)\n",
                path, (unsigned long)hlen, PKG_HASH_HEX_LEN);
        return -1;
    }
    /* Find matching FILE entry. */
    for (int i = 0; i < out->file_count; i++) {
        if (strcmp(out->files[i].dest, path) == 0) {
            memcpy(out->files[i].hash_hex, sp + 1, PKG_HASH_HEX_LEN);
            out->files[i].hash_hex[PKG_HASH_HEX_LEN] = '\0';
            out->files[i].has_hash = true;
            return 0;
        }
    }
    /* FHASH for an unknown FILE entry. We store nothing -- but log it
     * loudly so a malformed package author can debug. Don't fail
     * outright (FHASH might appear before its FILE in a future
     * generator). Caller will sweep again after parse completes. */
    kprintf("[pkg] FHASH for '%s' has no matching FILE yet -- "
            "second-pass match will retry\n", path);
    /* We stash it in a side buffer? Simpler: require FILE-before-FHASH.
     * Our generator (mkpkg) emits FILE lines first then FHASH lines, so
     * this branch should only fire on hand-crafted malformed pkgs. */
    return -1;
}

/* Core parser shared by .tpkg and install records.
 *
 * buf/n = the full file contents. expect_body = true for .tpkg (we
 * look for "BODY" and record body_offset), false for install records
 * (headers only).
 *
 * Returns 0 on success, -1 on any malformed line. `out` is fully
 * populated; on failure, already-parsed fields are left for the caller
 * to inspect (install aborts immediately so this is harmless). */
static int parse_manifest(const char *buf, size_t n,
                          struct pkg_manifest *out, bool expect_body) {
    memset(out, 0, sizeof(*out));

    const char *p   = buf;
    const char *end = buf + n;
    bool got_magic = false;

    while (p < end) {
        const char *line;
        size_t      len;
        const char *next = next_line(p, end, &line, &len);

        /* Blank line / comment -- skip silently. */
        if (len == 0 || line[0] == '#') { p = next; continue; }

        if (!got_magic) {
            if (len < 4 || strncmp(line, "TPKG", 4) != 0) {
                /* Install records start with "NAME" -- accept that too. */
                if (!(len >= 4 && strncmp(line, "NAME", 4) == 0)) {
                    kprintf("[pkg] missing TPKG magic / NAME header\n");
                    return -1;
                }
            } else {
                /* "TPKG 1" -- could validate version. Accept v1. */
                got_magic = true;
                p = next;
                continue;
            }
            got_magic = true;
            /* fall through to the directive dispatcher with this line */
        }

        /* Dispatch by directive. `len` is the line length. */
        if (len >= 5 && strncmp(line, "NAME ", 5) == 0) {
            size_t vl = len - 5;
            if (vl >= sizeof(out->name)) {
                kprintf("[pkg] NAME too long\n"); return -1;
            }
            memcpy(out->name, line + 5, vl); out->name[vl] = '\0';
        } else if (len >= 8 && strncmp(line, "VERSION ", 8) == 0) {
            size_t vl = len - 8;
            if (vl >= sizeof(out->version)) {
                kprintf("[pkg] VERSION too long\n"); return -1;
            }
            memcpy(out->version, line + 8, vl); out->version[vl] = '\0';
        } else if (len >= 5 && strncmp(line, "DESC ", 5) == 0) {
            size_t vl = len - 5;
            if (vl >= sizeof(out->desc)) vl = sizeof(out->desc) - 1;
            memcpy(out->desc, line + 5, vl); out->desc[vl] = '\0';
        } else if (len >= 8 && strncmp(line, "SANDBOX ", 8) == 0) {
            size_t vl = len - 8;
            if (vl >= sizeof(out->sandbox)) vl = sizeof(out->sandbox) - 1;
            memcpy(out->sandbox, line + 8, vl); out->sandbox[vl] = '\0';
        } else if (len >= 10 && strncmp(line, "PUBLISHER ", 10) == 0) {
            size_t vl = len - 10;
            if (vl >= sizeof(out->publisher)) vl = sizeof(out->publisher) - 1;
            memcpy(out->publisher, line + 10, vl); out->publisher[vl] = '\0';
        } else if (len >= 5 && strncmp(line, "CAPS ", 5) == 0) {
            size_t vl = len - 5;
            if (vl >= sizeof(out->caps)) vl = sizeof(out->caps) - 1;
            memcpy(out->caps, line + 5, vl); out->caps[vl] = '\0';
            /* Normalise to upper-case so the spawn-time cap_apply_declared
             * can do byte-exact name matches without re-canonicalising. */
            for (size_t i = 0; i < vl; i++) {
                char c = out->caps[i];
                if (c >= 'a' && c <= 'z') out->caps[i] = (char)(c - 'a' + 'A');
            }
        } else if (len >= 5 && strncmp(line, "HASH ", 5) == 0) {
            size_t vl = len - 5;
            if (vl != PKG_HASH_HEX_LEN) {
                kprintf("[pkg] HASH wrong length (got %lu, need %d)\n",
                        (unsigned long)vl, PKG_HASH_HEX_LEN);
                return -1;
            }
            memcpy(out->hash_hex, line + 5, PKG_HASH_HEX_LEN);
            out->hash_hex[PKG_HASH_HEX_LEN] = '\0';
            out->has_hash = true;
        } else if (len >= 6 && strncmp(line, "FHASH ", 6) == 0) {
            if (parse_fhash_line(line + 6, len - 6, out) != 0) return -1;
        } else if (len >= 4 && strncmp(line, "SIG ", 4) == 0) {
            /* Format: "SIG <key-id> <hex-tag>" */
            const char *q   = line + 4;
            const char *qe  = line + len;
            const char *sp1 = 0;
            for (const char *c = q; c < qe; c++)
                if (*c == ' ') { sp1 = c; break; }
            if (!sp1) {
                kprintf("[pkg] SIG malformed (need '<id> <hex>')\n");
                return -1;
            }
            size_t idl = (size_t)(sp1 - q);
            size_t hxl = (size_t)(qe - (sp1 + 1));
            if (idl == 0 || idl >= sizeof(out->sig_key)) {
                kprintf("[pkg] SIG key id missing or too long\n");
                return -1;
            }
            if (hxl != PKG_HASH_HEX_LEN) {
                kprintf("[pkg] SIG hex wrong length (got %lu, need %d)\n",
                        (unsigned long)hxl, PKG_HASH_HEX_LEN);
                return -1;
            }
            memcpy(out->sig_key, q, idl); out->sig_key[idl] = '\0';
            memcpy(out->sig_hex, sp1 + 1, PKG_HASH_HEX_LEN);
            out->sig_hex[PKG_HASH_HEX_LEN] = '\0';
            out->has_sig = true;
        } else if (len >= 4 && strncmp(line, "APP ", 4) == 0) {
            if (out->app_count >= PKG_MAX_APPS) {
                kprintf("[pkg] too many APP entries (max %d)\n", PKG_MAX_APPS);
                return -1;
            }
            if (parse_app_line(line + 4, len - 4,
                               &out->apps[out->app_count]) != 0) return -1;
            out->app_count++;
        } else if (len >= 5 && strncmp(line, "FILE ", 5) == 0) {
            if (out->file_count >= PKG_MAX_FILES) {
                kprintf("[pkg] too many FILE entries (max %d)\n", PKG_MAX_FILES);
                return -1;
            }
            if (parse_file_line(line + 5, len - 5,
                                &out->files[out->file_count],
                                expect_body) != 0) return -1;
            out->file_count++;
        } else if (len == 4 && strncmp(line, "BODY", 4) == 0) {
            if (!expect_body) {
                kprintf("[pkg] unexpected BODY in install record\n");
                return -1;
            }
            out->body_offset = (size_t)(next - buf);
            return 0;        /* header done */
        } else if (len >= 4 && strncmp(line, "TPKG", 4) == 0) {
            /* Tolerate repeated magic (unlikely but harmless). */
        } else {
            /* Unknown directive -- warn but don't fail. Forward
             * compatibility for future optional fields. */
            char tag[32]; size_t tl = len < sizeof(tag) - 1 ? len : sizeof(tag) - 1;
            memcpy(tag, line, tl); tag[tl] = '\0';
            kprintf("[pkg] ignoring unknown directive: '%s'\n", tag);
        }
        p = next;
    }

    if (expect_body) {
        kprintf("[pkg] missing BODY marker\n");
        return -1;
    }
    return 0;
}

/* ---- install-record read/write ------------------------------------ */

/* Build /data/packages/<name>.pkg into out (size >= VFS_PATH_MAX). */
static void db_path(const char *name, char *out) {
    size_t i = 0;
    const char *d = PKG_DB_DIR "/";
    while (*d && i + 1 < VFS_PATH_MAX) out[i++] = *d++;
    while (*name && i + 1 < VFS_PATH_MAX - 4) out[i++] = *name++;
    const char *ext = ".pkg";
    while (*ext && i + 1 < VFS_PATH_MAX) out[i++] = *ext++;
    out[i] = '\0';
}

/* Serialize a manifest back to disk as a plain-text install record.
 * Writes /data/packages/<name>.pkg. NOT reversible into a .tpkg
 * (no body, no file sizes) -- this is purely for removal. */
static int write_install_record(const struct pkg_manifest *m) {
    size_t cap = 1024 + (size_t)m->file_count * PKG_PATH_MAX
                      + (size_t)m->app_count  * (PKG_LABEL_MAX + PKG_PATH_MAX + 8);
    char *buf = (char *)kmalloc(cap);
    if (!buf) return -1;

    size_t n = 0;
    #define APPEND(s) do { \
            const char *_s = (s); size_t _l = strlen(_s); \
            if (n + _l + 1 >= cap) { kfree(buf); return -1; } \
            memcpy(buf + n, _s, _l); n += _l; \
        } while (0)

    APPEND("NAME ");    APPEND(m->name);    APPEND("\n");
    APPEND("VERSION "); APPEND(m->version); APPEND("\n");
    if (m->desc[0])    { APPEND("DESC ");    APPEND(m->desc);    APPEND("\n"); }
    if (m->sandbox[0]) { APPEND("SANDBOX "); APPEND(m->sandbox); APPEND("\n"); }
    /* M34D: persist declared capabilities. `pkg info` prints them and
     * upgrade_internal preserves them across an upgrade if the new
     * manifest has none of its own. */
    if (m->caps[0])    { APPEND("CAPS ");    APPEND(m->caps);    APPEND("\n"); }
    for (int i = 0; i < m->app_count; i++) {
        APPEND("APP "); APPEND(m->apps[i].label);
        APPEND("|");    APPEND(m->apps[i].exec);  APPEND("\n");
    }
    for (int i = 0; i < m->file_count; i++) {
        APPEND("FILE "); APPEND(m->files[i].dest); APPEND("\n");
    }
    #undef APPEND

    char path[VFS_PATH_MAX];
    db_path(m->name, path);
    int rc = vfs_write_all(path, buf, n);
    kfree(buf);
    return (rc == VFS_OK) ? 0 : -1;
}

/* Reads + parses an install record. Returns 0 on success. */
static int read_install_record(const char *name, struct pkg_manifest *out) {
    char path[VFS_PATH_MAX];
    db_path(name, path);

    void *buf = 0; size_t sz = 0;
    int rc = vfs_read_all(path, &buf, &sz);
    if (rc != VFS_OK) return rc;

    int prc = parse_manifest((const char *)buf, sz, out, /*expect_body=*/false);
    kfree(buf);
    return prc;
}

/* ---- .app descriptor emit / scan --------------------------------- *
 *
 * An .app file is the desktop launcher's "installed app" descriptor:
 *
 *     label=<menu label>
 *     exec=<absolute ELF path>
 *
 * The package installer emits one per APP entry (numbered so a package
 * with two GUI apps gets helloapp-0.app + helloapp-1.app). On boot
 * pkg_init() scans the .app descriptors in /data/apps and registers
 * each into the launcher via gui_launcher_register(). */

static int emit_app_descriptor(const char *pkg_name, int idx,
                               const struct pkg_app_decl *a,
                               const char *sandbox,
                               const char *caps,
                               char *out_path) {
    size_t i = 0;
    const char *d = PKG_APPS_DIR "/";
    while (*d && i + 1 < VFS_PATH_MAX) out_path[i++] = *d++;
    while (*pkg_name && i + 1 < VFS_PATH_MAX) out_path[i++] = *pkg_name++;
    /* "-<idx>.app" */
    if (i + 3 < VFS_PATH_MAX) { out_path[i++] = '-'; }
    /* idx is 0..PKG_MAX_APPS-1, always single-digit */
    if (i + 1 < VFS_PATH_MAX) out_path[i++] = (char)('0' + (idx % 10));
    const char *ext = ".app";
    while (*ext && i + 1 < VFS_PATH_MAX) out_path[i++] = *ext++;
    out_path[i] = '\0';

    char body[PKG_LABEL_MAX + PKG_PATH_MAX + PKG_SANDBOX_MAX +
              PKG_CAPS_LIST_MAX + 64];
    size_t bn = 0;
    const char *p;
    p = "label="; while (*p) body[bn++] = *p++;
    p = a->label; while (*p && bn + 1 < sizeof(body)) body[bn++] = *p++;
    body[bn++] = '\n';
    p = "exec=";  while (*p && bn + 1 < sizeof(body)) body[bn++] = *p++;
    p = a->exec;  while (*p && bn + 1 < sizeof(body)) body[bn++] = *p++;
    body[bn++] = '\n';
    /* Milestone 18: pin the sandbox profile into the .app descriptor so
     * the desktop launcher can apply it directly without consulting the
     * install record. Empty = inherit default. */
    if (sandbox && sandbox[0]) {
        p = "sandbox="; while (*p && bn + 1 < sizeof(body)) body[bn++] = *p++;
        p = sandbox;    while (*p && bn + 1 < sizeof(body)) body[bn++] = *p++;
        body[bn++] = '\n';
    }
    /* M34D: pin the declared capability list. The launcher passes this
     * to proc_spawn via spec.declared_caps so the spawned proc gets
     * narrowed past the profile to (parent & profile & declared). */
    if (caps && caps[0]) {
        p = "caps="; while (*p && bn + 1 < sizeof(body)) body[bn++] = *p++;
        p = caps;    while (*p && bn + 1 < sizeof(body)) body[bn++] = *p++;
        body[bn++] = '\n';
    }

    int rc = vfs_write_all(out_path, body, bn);
    return (rc == VFS_OK) ? 0 : -1;
}

/* Parse one .app file's "label=..\nexec=..\n[sandbox=..\n][caps=..\n]"
 * body into the provided output buffers. Missing keys leave the
 * corresponding buffer empty. Any of the out_* pointers may be NULL
 * (with matching cap=0) -- callers that don't care about a field skip
 * it that way. */
static void parse_app_file(const char *buf, size_t n,
                           char *out_label,   size_t lcap,
                           char *out_exec,    size_t ecap,
                           char *out_sandbox, size_t scap,
                           char *out_caps,    size_t ccap) {
    out_label  [0] = '\0';
    out_exec   [0] = '\0';
    if (out_sandbox) out_sandbox[0] = '\0';
    if (out_caps)    out_caps   [0] = '\0';

    const char *p   = buf;
    const char *end = buf + n;
    while (p < end) {
        const char *line;
        size_t      len;
        const char *next = next_line(p, end, &line, &len);
        if (len >= 6 && strncmp(line, "label=", 6) == 0) {
            size_t vl = len - 6;
            if (vl >= lcap) vl = lcap - 1;
            memcpy(out_label, line + 6, vl); out_label[vl] = '\0';
        } else if (len >= 5 && strncmp(line, "exec=", 5) == 0) {
            size_t vl = len - 5;
            if (vl >= ecap) vl = ecap - 1;
            memcpy(out_exec, line + 5, vl); out_exec[vl] = '\0';
        } else if (out_sandbox && scap &&
                   len >= 8 && strncmp(line, "sandbox=", 8) == 0) {
            size_t vl = len - 8;
            if (vl >= scap) vl = scap - 1;
            memcpy(out_sandbox, line + 8, vl); out_sandbox[vl] = '\0';
        } else if (out_caps && ccap &&
                   len >= 5 && strncmp(line, "caps=", 5) == 0) {
            size_t vl = len - 5;
            if (vl >= ccap) vl = ccap - 1;
            memcpy(out_caps, line + 5, vl); out_caps[vl] = '\0';
        }
        p = next;
    }
}

/* ---- pkg_refresh_launcher: clear + re-scan ----------------------- */

void pkg_refresh_launcher(void) {
    gui_launcher_reset_user();

    struct vfs_dir d;
    if (vfs_opendir(PKG_APPS_DIR, &d) != VFS_OK) return;

    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        if (ent.type != VFS_TYPE_FILE) continue;
        if (!ends_with(ent.name, ".app")) continue;

        char full[VFS_PATH_MAX];
        size_t i = 0;
        const char *dir = PKG_APPS_DIR "/";
        while (*dir && i + 1 < sizeof(full)) full[i++] = *dir++;
        for (const char *c = ent.name; *c && i + 1 < sizeof(full); c++) full[i++] = *c;
        full[i] = '\0';

        void *buf = 0; size_t sz = 0;
        if (vfs_read_all(full, &buf, &sz) != VFS_OK) continue;

        char label  [PKG_LABEL_MAX];
        char exec   [PKG_PATH_MAX];
        char sandbox[PKG_SANDBOX_MAX];
        char caps   [PKG_CAPS_LIST_MAX];
        parse_app_file((const char *)buf, sz,
                       label,   sizeof(label),
                       exec,    sizeof(exec),
                       sandbox, sizeof(sandbox),
                       caps,    sizeof(caps));
        kfree(buf);

        if (label[0] && exec[0]) {
            (void)gui_launcher_register_with_profile_caps(label, exec,
                                                          sandbox, caps);
            kprintf("[pkg] registered launcher entry: '%s' -> %s%s%s%s%s\n",
                    label, exec,
                    sandbox[0] ? " sandbox=" : "",
                    sandbox[0] ? sandbox     : "",
                    caps[0]    ? " caps="    : "",
                    caps[0]    ? caps        : "");
        }
    }
    vfs_closedir(&d);
}

/* ---- public API: pkg_init ---------------------------------------- */

static void ensure_dir(const char *path) {
    int rc = vfs_mkdir(path);
    if (rc != VFS_OK && rc != VFS_ERR_EXIST) {
        kprintf("[pkg] mkdir('%s') failed: %s\n", path, vfs_strerror(rc));
    }
}

void pkg_init(void) {
    ensure_dir(PKG_APPS_DIR);
    ensure_dir(PKG_DB_DIR);
    ensure_dir(PKG_REPO_DATA_DIR);
    pkg_refresh_launcher();
    kprintf("[pkg] package manager ready (apps=%s db=%s repo=%s,%s)\n",
            PKG_APPS_DIR, PKG_DB_DIR,
            PKG_REPO_DATA_DIR, PKG_REPO_RAMFS_DIR);
}

/* ---- public API: list / info / repo ------------------------------ */

void pkg_list(void) {
    struct vfs_dir d;
    int rc = vfs_opendir(PKG_DB_DIR, &d);
    if (rc != VFS_OK) {
        kprintf("pkg: no package database yet (%s: %s)\n",
                PKG_DB_DIR, vfs_strerror(rc));
        return;
    }

    int shown = 0;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        if (ent.type != VFS_TYPE_FILE) continue;
        if (!ends_with(ent.name, ".pkg")) continue;

        /* Strip the ".pkg" extension for display. */
        char nm[PKG_NAME_MAX];
        size_t nl = strlen(ent.name);
        if (nl < 4) continue;
        nl -= 4;
        if (nl >= sizeof(nm)) nl = sizeof(nm) - 1;
        memcpy(nm, ent.name, nl); nm[nl] = '\0';

        struct pkg_manifest m;
        if (read_install_record(nm, &m) == 0) {
            kprintf("  %-20s  %-10s  %s\n",
                    m.name, m.version,
                    m.desc[0] ? m.desc : "");
        } else {
            kprintf("  %-20s  (record unreadable)\n", nm);
        }
        shown++;
    }
    vfs_closedir(&d);

    if (shown == 0) kprintf("(no packages installed)\n");
}

int pkg_info(const char *name) {
    if (!name || !*name) { kprintf("pkg info: missing <name>\n"); return -1; }

    struct pkg_manifest m;
    int rc = read_install_record(name, &m);
    if (rc != 0) {
        kprintf("pkg info: '%s' is not installed (%s)\n",
                name, rc < 0 ? vfs_strerror(rc) : "?");
        return -1;
    }

    kprintf("name    : %s\n", m.name);
    kprintf("version : %s\n", m.version);
    if (m.desc[0])    kprintf("desc    : %s\n", m.desc);
    if (m.sandbox[0]) kprintf("sandbox : %s\n", m.sandbox);
    if (m.caps[0])    kprintf("caps    : %s\n", m.caps);
    if (m.app_count) {
        kprintf("apps    :\n");
        for (int i = 0; i < m.app_count; i++) {
            kprintf("  %-24s  %s\n", m.apps[i].label, m.apps[i].exec);
        }
    }
    if (m.file_count) {
        kprintf("files   :\n");
        for (int i = 0; i < m.file_count; i++) {
            kprintf("  %s\n", m.files[i].dest);
        }
    }
    return 0;
}

static void dump_repo_dir(const char *dir, int *total) {
    struct vfs_dir d;
    if (vfs_opendir(dir, &d) != VFS_OK) return;

    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        if (ent.type != VFS_TYPE_FILE) continue;
        if (!ends_with(ent.name, ".tpkg")) continue;
        kprintf("  %-28s  %6lu B  %s\n",
                ent.name, (unsigned long)ent.size, dir);
        (*total)++;
    }
    vfs_closedir(&d);
}

void pkg_repo_dump(void) {
    int total = 0;
    kprintf("available packages:\n");
    dump_repo_dir(PKG_REPO_DATA_DIR, &total);
    dump_repo_dir(PKG_REPO_RAMFS_DIR, &total);
    if (total == 0) {
        kprintf("  (none -- drop .tpkg files in %s or %s)\n",
                PKG_REPO_DATA_DIR, PKG_REPO_RAMFS_DIR);
    }
}

/* ---- public API: install ----------------------------------------- */

/* Verify a FILE destination is safe to write:
 *   - must start with "/data/"
 *   - must not collide with a file owned by any OTHER installed pkg
 *   - if it already exists on disk but is owned by no one, REFUSE
 *     (protects random /data/ files from being clobbered).
 *
 * installing_name = the package doing the install; used to allow a
 * file to match an own-package ghost record (shouldn't happen with
 * install-then-record ordering, but harmless). */
static int validate_dest(const char *dest, const char *installing_name) {
    if (!starts_with(dest, "/data/")) {
        kprintf("pkg: refusing '%s': must live under /data/\n", dest);
        return -1;
    }

    struct vfs_stat st;
    if (vfs_stat(dest, &st) == VFS_OK) {
        /* Something is there. Scan every install record to see if any
         * OTHER package already owns it. */
        struct vfs_dir d;
        if (vfs_opendir(PKG_DB_DIR, &d) == VFS_OK) {
            struct vfs_dirent ent;
            while (vfs_readdir(&d, &ent) == VFS_OK) {
                if (ent.type != VFS_TYPE_FILE) continue;
                if (!ends_with(ent.name, ".pkg")) continue;

                char nm[PKG_NAME_MAX];
                size_t nl = strlen(ent.name);
                if (nl < 4) continue;
                nl -= 4;
                if (nl >= sizeof(nm)) nl = sizeof(nm) - 1;
                memcpy(nm, ent.name, nl); nm[nl] = '\0';

                if (strcmp(nm, installing_name) == 0) continue;

                struct pkg_manifest mo;
                if (read_install_record(nm, &mo) != 0) continue;
                for (int i = 0; i < mo.file_count; i++) {
                    if (strcmp(mo.files[i].dest, dest) == 0) {
                        kprintf("pkg: '%s' is owned by package '%s' -- "
                                "cannot overwrite\n", dest, nm);
                        vfs_closedir(&d);
                        return -1;
                    }
                }
            }
            vfs_closedir(&d);
        }
        /* Path exists but no package owns it. Refuse -- we don't
         * want `pkg install` clobbering a file the user put there. */
        kprintf("pkg: '%s' already exists and is unmanaged -- "
                "remove it before installing\n", dest);
        return -1;
    }
    return 0;
}

/* ---- M34A/B/C: integrity + signature verification ---------------- *
 *
 * Run AFTER manifest parse and BEFORE we touch the filesystem with
 * any FILE payload. Hashes are computed over the body bytes:
 *
 *     body = raw + body_offset .. raw + body_offset + total_payload
 *
 * Per-file hashes are computed over each file's slice of the body.
 *
 * Failure modes (all fatal, fail-closed):
 *
 *   - HASH declared, body sha256 mismatch
 *   - any FHASH declared, payload sha256 mismatch
 *   - SIG declared, key not in trust store / wrong algo / tag mismatch
 *   - sig policy = REQUIRED and SIG missing
 *
 * The slog SUB_SEC tag emits exactly one record per verification
 * outcome so the M34F audit log captures both the success and the
 * failure paths from a single source. */

/* Module-wide signature policy. Default: WARN (back-compat with
 * pre-M34 packages). Boot self-test or future userland tooling can
 * flip to REQUIRED via pkg_set_sig_policy(). */
static int g_pkg_sig_policy = PKG_SIG_POLICY_WARN;

void pkg_set_sig_policy(int policy) {
    if (policy != PKG_SIG_POLICY_WARN && policy != PKG_SIG_POLICY_REQUIRED)
        return;
    g_pkg_sig_policy = policy;
    SLOG_INFO(SLOG_SUB_SEC, "package sig policy now %s",
              policy == PKG_SIG_POLICY_REQUIRED ? "REQUIRED" : "WARN");
}

int pkg_get_sig_policy(void) { return g_pkg_sig_policy; }

/* M34G accessor -- used by the securitytest harness to verify that
 * upgrade/rollback actually moves VERSION on disk, without exposing
 * the (intentionally static) install-record reader. */
int pkg_get_installed_version(const char *name, char *out_buf, size_t cap) {
    if (!name || !out_buf || cap == 0) return -1;
    out_buf[0] = '\0';
    struct pkg_manifest m;
    if (read_install_record(name, &m) != 0) return -1;
    copy_capped(out_buf, m.version, cap);
    return 0;
}

/* Returns 0 on pass, -1 on any verification failure. Caller must
 * have already validated that body_offset + sum(file sizes) <= raw_sz. */
static int verify_integrity(const struct pkg_manifest *m,
                            const uint8_t *raw, size_t raw_sz) {
    /* Total declared body length: catch a too-short package early. */
    size_t total = 0;
    for (int i = 0; i < m->file_count; i++) total += m->files[i].size;
    if (m->body_offset + total > raw_sz) {
        kprintf("[pkg] integrity failure: body truncated "
                "(need %lu bytes, have %lu)\n",
                (unsigned long)total,
                (unsigned long)(raw_sz - m->body_offset));
        SLOG_WARN(SLOG_SUB_SEC,
                  "verify(%s): truncated body need=%lu have=%lu",
                  m->name, (unsigned long)total,
                  (unsigned long)(raw_sz - m->body_offset));
        return -1;
    }

    /* M34A: package-wide HASH (over the body bytes). */
    if (m->has_hash) {
        uint8_t digest[SHA256_DIGEST_LEN];
        sha256_buf(raw + m->body_offset, total, digest);
        char have[SHA256_HEX_LEN + 1];
        sec_to_hex(digest, SHA256_DIGEST_LEN, have);
        if (strcmp(have, m->hash_hex) != 0) {
            kprintf("[pkg] integrity failure: package hash mismatch\n");
            kprintf("       declared : %s\n", m->hash_hex);
            kprintf("       computed : %s\n", have);
            /* kvprintf doesn't support %.16s precision -- copy a 16-char
             * prefix to a small NUL-terminated buffer instead. */
            char dpfx[17], hpfx[17];
            memcpy(dpfx, m->hash_hex, 16); dpfx[16] = '\0';
            memcpy(hpfx, have,         16); hpfx[16] = '\0';
            SLOG_WARN(SLOG_SUB_SEC,
                      "verify(%s): pkg HASH mismatch decl=%s.. got=%s..",
                      m->name, dpfx, hpfx);
            return -1;
        }
    }

    /* M34A: per-file FHASH. */
    size_t cursor = 0;
    for (int i = 0; i < m->file_count; i++) {
        size_t sz = m->files[i].size;
        if (m->files[i].has_hash) {
            uint8_t d[SHA256_DIGEST_LEN];
            sha256_buf(raw + m->body_offset + cursor, sz, d);
            char have[SHA256_HEX_LEN + 1];
            sec_to_hex(d, SHA256_DIGEST_LEN, have);
            if (strcmp(have, m->files[i].hash_hex) != 0) {
                kprintf("[pkg] integrity failure: file hash mismatch for '%s'\n",
                        m->files[i].dest);
                kprintf("       declared : %s\n", m->files[i].hash_hex);
                kprintf("       computed : %s\n", have);
                char dpfx[17], hpfx[17];
                memcpy(dpfx, m->files[i].hash_hex, 16); dpfx[16] = '\0';
                memcpy(hpfx, have,                 16); hpfx[16] = '\0';
                SLOG_WARN(SLOG_SUB_SEC,
                          "verify(%s): FHASH mismatch %s decl=%s.. got=%s..",
                          m->name, m->files[i].dest, dpfx, hpfx);
                return -1;
            }
        }
        cursor += sz;
    }

    /* M34C: SIG (covers the same body bytes as HASH). */
    if (m->has_sig) {
        int rv = sig_verify_hmac(m->sig_key, m->sig_hex,
                                 raw + m->body_offset, total);
        if (rv == 1) {
            kprintf("[pkg] signature OK (key='%s')\n", m->sig_key);
            SLOG_INFO(SLOG_SUB_SEC,
                      "verify(%s): SIG OK key=%s", m->name, m->sig_key);
        } else {
            const char *why = "?";
            switch (rv) {
            case  0: why = "tag mismatch (tampered or wrong key)"; break;
            case -1: why = "unknown key id (not in trust store)";  break;
            case -2: why = "key algorithm mismatch";               break;
            case -3: why = "malformed signature";                  break;
            }
            kprintf("[pkg] integrity failure: signature rejected for '%s' "
                    "(key='%s'): %s\n", m->name, m->sig_key, why);
            SLOG_WARN(SLOG_SUB_SEC,
                      "verify(%s): SIG REJECT key=%s rv=%d (%s)",
                      m->name, m->sig_key, rv, why);
            return -1;
        }
    } else if (g_pkg_sig_policy == PKG_SIG_POLICY_REQUIRED) {
        kprintf("[pkg] integrity failure: '%s' is unsigned and policy "
                "requires SIG\n", m->name);
        SLOG_WARN(SLOG_SUB_SEC,
                  "verify(%s): UNSIGNED rejected by REQUIRED policy",
                  m->name);
        return -1;
    } else if (m->has_hash) {
        /* Hashed but not signed: integrity protected against accidental
         * corruption, just not against a malicious build. */
        SLOG_INFO(SLOG_SUB_SEC,
                  "verify(%s): unsigned, HASH OK (policy=warn)", m->name);
    } else {
        /* Pre-M34 package, no integrity metadata at all. */
        SLOG_INFO(SLOG_SUB_SEC,
                  "verify(%s): no HASH/SIG (legacy package)", m->name);
    }

    return 0;
}

static int pkg_install_path_inner(const char *tpkg_path) {
    if (!tpkg_path) return -1;

    kprintf("[pkg] installing from %s\n", tpkg_path);

    void *raw = 0; size_t raw_sz = 0;
    int rc = vfs_read_all(tpkg_path, &raw, &raw_sz);
    if (rc != VFS_OK) {
        kprintf("pkg: cannot read '%s': %s\n", tpkg_path, vfs_strerror(rc));
        return -1;
    }

    struct pkg_manifest m;
    if (parse_manifest((const char *)raw, raw_sz, &m, /*expect_body=*/true) != 0) {
        kfree(raw);
        return -1;
    }
    if (!m.name[0] || !m.version[0]) {
        kprintf("pkg: missing NAME or VERSION\n");
        kfree(raw);
        return -1;
    }

    /* Refuse to install on top of an existing install. */
    char db[VFS_PATH_MAX];
    db_path(m.name, db);
    struct vfs_stat st_db;
    if (vfs_stat(db, &st_db) == VFS_OK) {
        kprintf("pkg: '%s' is already installed -- "
                "'pkg remove %s' first\n", m.name, m.name);
        kfree(raw);
        return -1;
    }

    /* Validate every FILE destination up front, before writing any. */
    for (int i = 0; i < m.file_count; i++) {
        if (validate_dest(m.files[i].dest, m.name) != 0) {
            kfree(raw);
            return -1;
        }
    }

    /* M34A/B/C: verify integrity + signature BEFORE any FS write so a
     * corrupted or tampered package never lands on disk. Also catches
     * truncated bodies (the previous bespoke check is now folded into
     * verify_integrity). */
    if (verify_integrity(&m, (const uint8_t *)raw, raw_sz) != 0) {
        kprintf("pkg: refusing to install '%s' -- integrity check failed\n",
                m.name);
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "pkg install REJECT %s reason=integrity src=%s",
                  m.name, tpkg_path);
        kfree(raw);
        return -1;
    }

    /* (Body size already validated by verify_integrity above.) */

    /* Extract every FILE. On first write failure, roll back by
     * unlinking whatever we've already laid down. */
    const uint8_t *body = (const uint8_t *)raw + m.body_offset;
    size_t cursor = 0;
    int written = 0;
    for (int i = 0; i < m.file_count; i++) {
        const char *dest = m.files[i].dest;
        size_t      sz   = m.files[i].size;

        /* Ensure the destination dir exists. */
        int mrc = mkdir_parents(dest);
        if (mrc != VFS_OK) {
            kprintf("pkg: mkdir for '%s' failed: %s\n",
                    dest, vfs_strerror(mrc));
            goto rollback;
        }

        int wrc = vfs_write_all(dest, body + cursor, sz);
        if (wrc != VFS_OK) {
            kprintf("pkg: writing '%s' failed: %s\n",
                    dest, vfs_strerror(wrc));
            goto rollback;
        }
        kprintf("  installed %s (%lu bytes)\n", dest, (unsigned long)sz);
        cursor += sz;
        written++;
    }

    /* Emit APP descriptors and append them to the file list so `pkg
     * remove` wipes them too. We stop if the file list fills up, but
     * install still succeeds -- user just loses tracking of the
     * descriptor. */
    for (int i = 0; i < m.app_count; i++) {
        if (m.file_count >= PKG_MAX_FILES) {
            kprintf("pkg: warning: file list full, cannot track .app descriptor\n");
            break;
        }
        char app_path[VFS_PATH_MAX];
        if (emit_app_descriptor(m.name, i, &m.apps[i], m.sandbox, m.caps,
                                app_path) != 0) {
            kprintf("pkg: failed to emit .app descriptor for '%s'\n",
                    m.apps[i].label);
            /* non-fatal -- the binary is already installed; skip */
            continue;
        }
        kprintf("  registered app: %s -> %s%s%s%s%s\n",
                m.apps[i].label, m.apps[i].exec,
                m.sandbox[0] ? " sandbox=" : "",
                m.sandbox[0] ? m.sandbox   : "",
                m.caps[0]    ? " caps="    : "",
                m.caps[0]    ? m.caps      : "");
        copy_capped(m.files[m.file_count].dest, app_path, PKG_PATH_MAX);
        m.file_count++;
    }

    /* Persist the install record LAST. If the box crashes before this
     * point the half-installed files are orphans -- the user can
     * re-install (after removing them manually) or we can add a
     * `pkg gc` command later. */
    if (write_install_record(&m) != 0) {
        kprintf("pkg: failed to write install record -- rolling back\n");
        goto rollback;
    }

    kfree(raw);
    pkg_refresh_launcher();
    kprintf("[pkg] installed %s %s (%d file%s, %d app%s)\n",
            m.name, m.version,
            m.file_count, m.file_count == 1 ? "" : "s",
            m.app_count,  m.app_count  == 1 ? "" : "s");
    SLOG_INFO(SLOG_SUB_AUDIT,
              "pkg install OK name=%s ver=%s pub=%s sig=%s",
              m.name, m.version,
              m.publisher[0] ? m.publisher : "-",
              m.has_sig ? m.sig_key : "-");
    return 0;

rollback:
    for (int i = 0; i < written; i++) {
        (void)vfs_unlink(m.files[i].dest);
    }
    kfree(raw);
    return -1;
}

int pkg_install_path(const char *tpkg_path) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_install_path_inner(tpkg_path);
    pkg_priv_end(&pv);
    return rc;
}

static int pkg_install_name_inner(const char *name) {
    if (!name || !*name) return -1;

    char path[VFS_PATH_MAX];
    for (int i = 0; i < 2; i++) {
        const char *dir = (i == 0) ? PKG_REPO_DATA_DIR : PKG_REPO_RAMFS_DIR;
        size_t k = 0;
        const char *d = dir;
        while (*d && k + 1 < sizeof(path)) path[k++] = *d++;
        if (k + 1 < sizeof(path)) path[k++] = '/';
        const char *n = name;
        while (*n && k + 1 < sizeof(path)) path[k++] = *n++;
        const char *ext = ".tpkg";
        while (*ext && k + 1 < sizeof(path)) path[k++] = *ext++;
        path[k] = '\0';

        struct vfs_stat st;
        if (vfs_stat(path, &st) == VFS_OK && st.type == VFS_TYPE_FILE) {
            return pkg_install_path(path);
        }
    }
    kprintf("pkg: '%s' not found in %s or %s\n",
            name, PKG_REPO_DATA_DIR, PKG_REPO_RAMFS_DIR);
    return -1;
}

int pkg_install_name(const char *name) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_install_name_inner(name);
    pkg_priv_end(&pv);
    return rc;
}

/* ---- public API: install over HTTP (milestone 24D) -------------- */

static int pkg_install_url_inner(const char *url) {
    if (!url || !*url) return -1;

    /* Cap downloads at 4 MiB. .tpkg files in the initrd are tens of
     * KiB, so this is roomy without letting a misbehaving server
     * exhaust the heap. */
    const size_t max_body = 4u * 1024u * 1024u;

    struct http_response resp;
    int rc = http_get(url, max_body, /*timeout_ms=*/0, &resp);
    if (rc != 0) {
        kprintf("pkg: download failed: %s (%d)\n", http_strerror(rc), rc);
        return -1;
    }
    if (resp.status != 200) {
        kprintf("pkg: HTTP %d %s -- not installing\n",
                resp.status, resp.reason);
        http_free(&resp);
        return -1;
    }

    /* Spool to /data/packages/.dl/<basename(path)>. We re-parse the
     * URL to recover the path so we can pick a sensible filename. */
    struct http_url u;
    if (http_parse_url(url, &u) != 0) {
        http_free(&resp);
        return -1;
    }
    const char *base = path_basename(u.path);
    if (!*base) base = "download.tpkg";

    char scratch[VFS_PATH_MAX];
    /* /data/packages/.dl/<base>  (PKG_DB_DIR == "/data/packages") */
    {
        size_t k = 0;
        const char *prefix = PKG_DB_DIR "/.dl/";
        while (*prefix && k + 1 < sizeof(scratch)) scratch[k++] = *prefix++;
        const char *s = base;
        while (*s && k + 1 < sizeof(scratch)) scratch[k++] = *s++;
        scratch[k] = 0;
    }

    int mrc = mkdir_parents(scratch);
    if (mrc != VFS_OK) {
        kprintf("pkg: cannot create scratch dir for '%s': %s\n",
                scratch, vfs_strerror(mrc));
        http_free(&resp);
        return -1;
    }
    int wrc = vfs_write_all(scratch, resp.body, resp.body_len);
    if (wrc != VFS_OK) {
        kprintf("pkg: cannot stage download to '%s': %s\n",
                scratch, vfs_strerror(wrc));
        http_free(&resp);
        return -1;
    }
    http_free(&resp);

    kprintf("[pkg] downloaded %s -> %s\n", url, scratch);

    int irc = pkg_install_path(scratch);
    if (irc != 0) {
        /* Leave the scratch in place for forensics. */
        kprintf("pkg: install from '%s' failed; download retained for inspection\n",
                scratch);
        return -1;
    }
    /* Clean up on success; ignore errors (worst case the user can
     * delete it manually). */
    (void)vfs_unlink(scratch);
    return 0;
}

int pkg_install_url(const char *url) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_install_url_inner(url);
    pkg_priv_end(&pv);
    return rc;
}

/* ---- public API: remove ------------------------------------------ */

static int pkg_remove_inner(const char *name) {
    if (!name || !*name) return -1;

    struct pkg_manifest m;
    int rc = read_install_record(name, &m);
    if (rc != 0) {
        kprintf("pkg: '%s' is not installed\n", name);
        return -1;
    }

    kprintf("[pkg] removing %s %s (%d file%s)\n",
            m.name, m.version, m.file_count, m.file_count == 1 ? "" : "s");

    for (int i = 0; i < m.file_count; i++) {
        int urc = vfs_unlink(m.files[i].dest);
        if (urc != VFS_OK && urc != VFS_ERR_NOENT) {
            /* Non-fatal: warn and keep going so we don't leave a
             * partially-uninstalled package behind. */
            kprintf("  warn: unlink '%s' failed: %s\n",
                    m.files[i].dest, vfs_strerror(urc));
        } else if (urc == VFS_ERR_NOENT) {
            kprintf("  skip: %s (already gone)\n", m.files[i].dest);
        } else {
            kprintf("  removed %s\n", m.files[i].dest);
        }
    }

    char db[VFS_PATH_MAX];
    db_path(m.name, db);
    int urc = vfs_unlink(db);
    if (urc != VFS_OK) {
        kprintf("pkg: could not remove install record '%s': %s\n",
                db, vfs_strerror(urc));
        return -1;
    }

    pkg_refresh_launcher();
    kprintf("[pkg] '%s' removed\n", m.name);
    SLOG_INFO(SLOG_SUB_AUDIT,
              "pkg remove OK name=%s ver=%s", m.name, m.version);
    (void)path_basename;        /* kept for future use (diagnostics) */
    return 0;
}

int pkg_remove(const char *name) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_remove_inner(name);
    pkg_priv_end(&pv);
    return rc;
}

/* ===================================================================
 * Milestone 17: versioning, repository scan, upgrade, rollback.
 * ===================================================================
 *
 * Everything below builds on the parser + install/remove primitives
 * above. The design notes:
 *
 *   - Versions are dotted-decimal ("1.0.0"). Compare component-wise.
 *   - "Available version" of a package = highest VERSION header found
 *     in any .tpkg file under /repo or /data/repo, regardless of the
 *     file's on-disk name. Filename-based install (`pkg install
 *     <name>`) still uses literal "<name>.tpkg" for backwards compat
 *     with milestone-16 tests; only upgrade/update use the by-NAME
 *     scan.
 *   - Upgrade is "backup, replace, rewrite record, auto-rollback on
 *     failure". The backup is itself a valid .tpkg, so rollback is the
 *     same code path as install with a different source file.
 *   - The .bak file is left on disk after a successful upgrade so the
 *     user can rollback later. A subsequent successful upgrade
 *     overwrites it (single-version history, by design).
 */

/* ---- version parse + compare ------------------------------------- */

int pkg_version_parse(const char *s, struct pkg_version *out) {
    memset(out, 0, sizeof(*out));
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s && out->count < PKG_VERSION_PARTS) {
        int v = 0;
        bool any = false;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
            any = true;
        }
        if (!any) {
            /* Non-numeric component (e.g. "1.0a") -- treat as 0 and
             * scan past the garbage so the next dot still parses. */
            while (*s && *s != '.') s++;
        }
        out->parts[out->count++] = v;
        if (*s == '.') s++;
        else break;
    }
    return out->count;
}

int pkg_version_compare(const struct pkg_version *a,
                        const struct pkg_version *b) {
    for (int i = 0; i < PKG_VERSION_PARTS; i++) {
        int av = (i < a->count) ? a->parts[i] : 0;
        int bv = (i < b->count) ? b->parts[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return  1;
    }
    return 0;
}

int pkg_version_cmp_str(const char *a, const char *b) {
    struct pkg_version va, vb;
    pkg_version_parse(a, &va);
    pkg_version_parse(b, &vb);
    return pkg_version_compare(&va, &vb);
}

/* ---- repo scan: find latest available version of <name> ---------- */

struct pkg_avail {
    char name   [PKG_NAME_MAX];
    char version[PKG_VERSION_MAX];
    char path   [VFS_PATH_MAX];   /* absolute path to .tpkg */
    bool found;
};

/* Slurp a .tpkg and parse just the header (NAME + VERSION + ...).
 * We use vfs_read_all so we always see the BODY marker no matter how
 * big the body is -- our packages cap at ~64 KiB anyway and this only
 * runs for the brief repo-scan path. Returns 0 on success. */
static int read_tpkg_header(const char *path, struct pkg_manifest *out) {
    void *buf = 0; size_t sz = 0;
    int rc = vfs_read_all(path, &buf, &sz);
    if (rc != VFS_OK) return rc;
    int prc = parse_manifest((const char *)buf, sz, out, /*expect_body=*/true);
    kfree(buf);
    return prc;
}

/* Scan one repo directory; for each .tpkg whose NAME matches `want`,
 * compare its VERSION against the running best and keep the higher.
 * `out->found` is set true the first time anything matches. */
static void scan_repo_dir(const char *dir, const char *want,
                          struct pkg_avail *out) {
    struct vfs_dir d;
    if (vfs_opendir(dir, &d) != VFS_OK) return;

    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        if (ent.type != VFS_TYPE_FILE) continue;
        if (!ends_with(ent.name, ".tpkg")) continue;

        char full[VFS_PATH_MAX];
        size_t i = 0;
        while (dir[i] && i + 1 < sizeof(full)) { full[i] = dir[i]; i++; }
        if (i + 1 < sizeof(full)) full[i++] = '/';
        for (const char *c = ent.name; *c && i + 1 < sizeof(full); c++)
            full[i++] = *c;
        full[i] = '\0';

        struct pkg_manifest m;
        if (read_tpkg_header(full, &m) != 0) continue;
        if (!m.name[0] || strcmp(m.name, want) != 0) continue;
        if (!m.version[0]) continue;

        if (!out->found ||
            pkg_version_cmp_str(m.version, out->version) > 0) {
            copy_capped(out->name,    m.name,    sizeof(out->name));
            copy_capped(out->version, m.version, sizeof(out->version));
            copy_capped(out->path,    full,      sizeof(out->path));
            out->found = true;
        }
    }
    vfs_closedir(&d);
}

static int repo_find_latest(const char *name, struct pkg_avail *out) {
    memset(out, 0, sizeof(*out));
    scan_repo_dir(PKG_REPO_DATA_DIR,  name, out);
    scan_repo_dir(PKG_REPO_RAMFS_DIR, name, out);
    return out->found ? 0 : -1;
}

/* ---- backup blob: serialize the currently-installed package ------
 *
 * Produces a self-describing .tpkg-format buffer in memory containing:
 *   TPKG 1
 *   NAME / VERSION / DESC / APP* / FILE <path> <size> ...
 *   BODY
 *   <concatenated old file payloads>
 *
 * Files that no longer exist on disk are silently skipped (a previous
 * partial uninstall must not block backup creation). Returns 0 and
 * fills out_buf/out_sz on success; the caller frees out_buf. */
static int build_backup_blob(const struct pkg_manifest *m,
                             void **out_buf, size_t *out_sz) {
    *out_buf = 0; *out_sz = 0;

    /* First pass: stat every file to know total body size + which
     * files are actually present right now. */
    struct file_info {
        const char *path;
        const char *label;        /* matching APP label, or NULL */
        const char *exec;
        size_t      size;
        bool        present;
    };
    struct file_info *fi =
        (struct file_info *)kmalloc(sizeof(*fi) * (size_t)m->file_count);
    if (!fi) return -1;
    memset(fi, 0, sizeof(*fi) * (size_t)m->file_count);

    size_t body_total = 0;
    int    present_n  = 0;
    for (int i = 0; i < m->file_count; i++) {
        fi[i].path = m->files[i].dest;
        struct vfs_stat st;
        if (vfs_stat(fi[i].path, &st) == VFS_OK && st.type == VFS_TYPE_FILE) {
            fi[i].size    = st.size;
            fi[i].present = true;
            body_total += st.size;
            present_n++;
        } else {
            kprintf("[pkg] backup: '%s' missing on disk -- skipping\n",
                    fi[i].path);
        }
    }

    /* Header capacity estimate. */
    size_t header_cap = 256
                     + (size_t)m->file_count * (PKG_PATH_MAX + 16)
                     + (size_t)m->app_count  * (PKG_LABEL_MAX + PKG_PATH_MAX + 8);
    size_t total_cap  = header_cap + body_total;
    char  *buf = (char *)kmalloc(total_cap);
    if (!buf) { kfree(fi); return -1; }

    size_t n = 0;
    #define APPEND_S(s) do { \
            const char *_s = (s); size_t _l = strlen(_s); \
            if (n + _l > total_cap) goto fail; \
            memcpy(buf + n, _s, _l); n += _l; \
        } while (0)
    #define APPEND_C(c) do { \
            if (n + 1 > total_cap) goto fail; \
            buf[n++] = (char)(c); \
        } while (0)

    APPEND_S("TPKG 1\n");
    APPEND_S("NAME ");    APPEND_S(m->name);    APPEND_C('\n');
    APPEND_S("VERSION "); APPEND_S(m->version); APPEND_C('\n');
    if (m->desc[0])    { APPEND_S("DESC ");    APPEND_S(m->desc);    APPEND_C('\n'); }
    if (m->sandbox[0]) { APPEND_S("SANDBOX "); APPEND_S(m->sandbox); APPEND_C('\n'); }
    if (m->caps[0])    { APPEND_S("CAPS ");    APPEND_S(m->caps);    APPEND_C('\n'); }
    for (int i = 0; i < m->app_count; i++) {
        APPEND_S("APP "); APPEND_S(m->apps[i].label);
        APPEND_C('|');    APPEND_S(m->apps[i].exec);  APPEND_C('\n');
    }
    /* Decimal-print the per-file size as part of the FILE line. */
    for (int i = 0; i < m->file_count; i++) {
        if (!fi[i].present) continue;
        APPEND_S("FILE "); APPEND_S(fi[i].path); APPEND_C(' ');
        char num[16]; size_t nn = 0;
        size_t v = fi[i].size;
        if (v == 0) num[nn++] = '0';
        char rev[16]; size_t rn = 0;
        while (v) { rev[rn++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (rn) num[nn++] = rev[--rn];
        if (n + nn > total_cap) goto fail;
        memcpy(buf + n, num, nn); n += nn;
        APPEND_C('\n');
    }
    APPEND_S("BODY\n");

    /* Body: read each present file straight in. */
    for (int i = 0; i < m->file_count; i++) {
        if (!fi[i].present) continue;
        if (n + fi[i].size > total_cap) goto fail;
        struct vfs_file f;
        if (vfs_open(fi[i].path, &f) != VFS_OK) goto fail;
        size_t got = 0;
        while (got < fi[i].size) {
            long r = vfs_read(&f, buf + n + got, fi[i].size - got);
            if (r <= 0) { vfs_close(&f); goto fail; }
            got += (size_t)r;
        }
        vfs_close(&f);
        n += fi[i].size;
    }

    #undef APPEND_S
    #undef APPEND_C

    kfree(fi);
    *out_buf = buf;
    *out_sz  = n;
    (void)present_n;
    return 0;

fail:
    kfree(buf);
    kfree(fi);
    return -1;
}

/* /data/packages/<name>.bak path. */
static void bak_path(const char *name, char *out) {
    size_t i = 0;
    const char *d = PKG_DB_DIR "/";
    while (*d && i + 1 < VFS_PATH_MAX) out[i++] = *d++;
    while (*name && i + 1 < VFS_PATH_MAX - 4) out[i++] = *name++;
    const char *ext = ".bak";
    while (*ext && i + 1 < VFS_PATH_MAX) out[i++] = *ext++;
    out[i] = '\0';
}

/* ---- core "apply a parsed package over an installed one" --------- *
 *
 * Used by both upgrade and rollback. Assumes:
 *   - new_buf / new_sz contain the FULL .tpkg bytes
 *   - the package is currently installed (m_old is its install record)
 *
 * Steps:
 *   1. Parse new manifest, validate body bytes available
 *   2. Validate every NEW dest is under /data/, not owned by another pkg
 *   3. For each OLD file not in NEW: unlink it
 *   4. For each NEW file: unlink existing dest (ignore NOENT) then
 *      vfs_write_all the new bytes
 *   5. Emit new APP descriptors (track them in m_new->files so the
 *      install record knows to reap them later)
 *   6. Write the new install record
 *
 * Returns 0 on success. Caller is responsible for backup/rollback. */
static int apply_replace(const char *new_buf, size_t new_sz,
                         const struct pkg_manifest *m_old) {
    struct pkg_manifest m_new;
    if (parse_manifest(new_buf, new_sz, &m_new, /*expect_body=*/true) != 0)
        return -1;
    if (!m_new.name[0] || !m_new.version[0]) {
        kprintf("pkg: new package missing NAME or VERSION\n");
        return -1;
    }
    if (strcmp(m_new.name, m_old->name) != 0) {
        kprintf("pkg: refusing replace -- new pkg is '%s' not '%s'\n",
                m_new.name, m_old->name);
        return -1;
    }

    /* Validate destinations + total body size. */
    for (int i = 0; i < m_new.file_count; i++) {
        const char *dest = m_new.files[i].dest;
        if (!starts_with(dest, "/data/")) {
            kprintf("pkg: refusing '%s': must live under /data/\n", dest);
            return -1;
        }
    }

    /* M34B: integrity + signature verification on the upgrade source.
     * Same gate as install -- if the new package is corrupted or
     * tampered, the live install is left untouched and the caller
     * (upgrade_internal) preserves its rollback semantics. */
    if (verify_integrity(&m_new, (const uint8_t *)new_buf, new_sz) != 0) {
        kprintf("pkg upgrade: refusing replace -- integrity check failed\n");
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "pkg upgrade REJECT %s reason=integrity",
                  m_new.name);
        return -1;
    }

    size_t total = 0;
    for (int i = 0; i < m_new.file_count; i++) total += m_new.files[i].size;
    (void)total;    /* silence -Wunused; size already verified above */

    /* (3) Remove old files that won't be in the new install. The new
     * file list is what we build the install record from; old .app
     * descriptors live in m_old->files too, so they get cleaned up
     * here automatically when the new manifest doesn't list them. */
    for (int i = 0; i < m_old->file_count; i++) {
        const char *p = m_old->files[i].dest;
        bool keep = false;
        for (int j = 0; j < m_new.file_count; j++) {
            if (strcmp(p, m_new.files[j].dest) == 0) { keep = true; break; }
        }
        if (!keep) {
            int u = vfs_unlink(p);
            if (u != VFS_OK && u != VFS_ERR_NOENT) {
                kprintf("  warn: could not unlink old file '%s': %s\n",
                        p, vfs_strerror(u));
            } else {
                kprintf("  removed (no longer in new pkg): %s\n", p);
            }
        }
    }

    /* (4) Lay down each new file. We unlink-then-write rather than
     * overwriting in place so a smaller new file doesn't leave stale
     * tail bytes (tobyfs's write doesn't truncate). */
    const uint8_t *body = (const uint8_t *)new_buf + m_new.body_offset;
    size_t cursor = 0;
    for (int i = 0; i < m_new.file_count; i++) {
        const char *dest = m_new.files[i].dest;
        size_t      sz   = m_new.files[i].size;

        int mrc = mkdir_parents(dest);
        if (mrc != VFS_OK) {
            kprintf("pkg: mkdir for '%s' failed: %s\n",
                    dest, vfs_strerror(mrc));
            return -1;
        }
        int urc = vfs_unlink(dest);
        if (urc != VFS_OK && urc != VFS_ERR_NOENT) {
            kprintf("pkg: cannot unlink existing '%s': %s\n",
                    dest, vfs_strerror(urc));
            return -1;
        }
        int wrc = vfs_write_all(dest, body + cursor, sz);
        if (wrc != VFS_OK) {
            kprintf("pkg: writing '%s' failed: %s\n",
                    dest, vfs_strerror(wrc));
            return -1;
        }
        kprintf("  wrote %s (%lu bytes)\n", dest, (unsigned long)sz);
        cursor += sz;
    }

    /* (5) Emit fresh .app descriptors. Same numbering as install: they
     * land at /data/apps/<name>-<idx>.app and get tracked in the file
     * list so future remove/upgrade reap them. */
    for (int i = 0; i < m_new.app_count; i++) {
        if (m_new.file_count >= PKG_MAX_FILES) {
            kprintf("pkg: warning: file list full, cannot track .app descriptor\n");
            break;
        }
        char app_path[VFS_PATH_MAX];
        if (emit_app_descriptor(m_new.name, i, &m_new.apps[i],
                                m_new.sandbox, m_new.caps,
                                app_path) != 0) {
            kprintf("pkg: failed to emit .app descriptor for '%s'\n",
                    m_new.apps[i].label);
            continue;
        }
        kprintf("  registered app: %s -> %s%s%s%s%s\n",
                m_new.apps[i].label, m_new.apps[i].exec,
                m_new.sandbox[0] ? " sandbox=" : "",
                m_new.sandbox[0] ? m_new.sandbox   : "",
                m_new.caps[0]    ? " caps="    : "",
                m_new.caps[0]    ? m_new.caps      : "");
        copy_capped(m_new.files[m_new.file_count].dest,
                    app_path, PKG_PATH_MAX);
        m_new.file_count++;
    }

    /* (6) Persist the new install record (overwrites the old). */
    if (write_install_record(&m_new) != 0) {
        kprintf("pkg: failed to write install record\n");
        return -1;
    }
    return 0;
}

/* Restore from a backup blob in memory. Treats the blob as if it were
 * a fresh .tpkg and runs apply_replace against the *current* installed
 * record. */
static int restore_from_blob(const char *name,
                             const void *bak_buf, size_t bak_sz) {
    struct pkg_manifest m_now;
    if (read_install_record(name, &m_now) != 0) {
        /* Install record itself was lost. Synthesize a minimal one
         * carrying just the name so apply_replace doesn't refuse. */
        memset(&m_now, 0, sizeof(m_now));
        copy_capped(m_now.name, name, sizeof(m_now.name));
    }
    return apply_replace((const char *)bak_buf, bak_sz, &m_now);
}

/* ---- pkg_update ------------------------------------------------- */

void pkg_update(void) {
    struct vfs_dir d;
    int rc = vfs_opendir(PKG_DB_DIR, &d);
    if (rc != VFS_OK) {
        kprintf("pkg update: no install database (%s)\n",
                vfs_strerror(rc));
        return;
    }

    int total = 0, upgradable = 0;
    struct vfs_dirent ent;
    kprintf("%-20s  %-12s  %-12s  %s\n",
            "package", "installed", "available", "status");
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        if (ent.type != VFS_TYPE_FILE) continue;
        if (!ends_with(ent.name, ".pkg")) continue;
        /* Skip backup files (.bak doesn't end in .pkg, so they're
         * naturally excluded; this is just defensive). */
        if (ends_with(ent.name, ".bak")) continue;

        char nm[PKG_NAME_MAX];
        size_t nl = strlen(ent.name);
        if (nl < 4) continue;
        nl -= 4;
        if (nl >= sizeof(nm)) nl = sizeof(nm) - 1;
        memcpy(nm, ent.name, nl); nm[nl] = '\0';

        struct pkg_manifest m;
        if (read_install_record(nm, &m) != 0) continue;

        struct pkg_avail av;
        const char *avail_str = "-";
        const char *status    = "no repo entry";
        if (repo_find_latest(m.name, &av) == 0) {
            avail_str = av.version;
            int c = pkg_version_cmp_str(av.version, m.version);
            if      (c > 0) { status = "UPGRADE";       upgradable++; }
            else if (c < 0) { status = "newer installed"; }
            else            { status = "up-to-date"; }
        }
        kprintf("%-20s  %-12s  %-12s  %s\n",
                m.name, m.version, avail_str, status);
        total++;
    }
    vfs_closedir(&d);

    if (total == 0) {
        kprintf("(no packages installed)\n");
    } else if (upgradable == 0) {
        kprintf("\nall %d package%s up-to-date\n",
                total, total == 1 ? "" : "s");
    } else {
        kprintf("\n%d package%s can be upgraded; run 'pkg upgrade'\n",
                upgradable, upgradable == 1 ? "" : "s");
    }
}

/* ---- pkg_upgrade_one -------------------------------------------- */

/* Internal worker. If `forced_src` is non-NULL, use that absolute path
 * as the new package source instead of consulting the repo (used by
 * rollback to feed the .bak in). If `make_backup` is false we skip
 * writing the .bak (also used by rollback). */
static int upgrade_internal(const char *name,
                            const char *forced_src,
                            bool make_backup) {
    struct pkg_manifest m_old;
    int rc = read_install_record(name, &m_old);
    if (rc != 0) {
        kprintf("pkg upgrade: '%s' is not installed\n", name);
        return -1;
    }

    char src_path_buf[VFS_PATH_MAX];
    const char *src_path = forced_src;
    char new_version_buf[PKG_VERSION_MAX];
    new_version_buf[0] = '\0';

    if (!src_path) {
        struct pkg_avail av;
        if (repo_find_latest(name, &av) != 0) {
            kprintf("pkg upgrade: '%s' has no .tpkg in repo\n", name);
            return -1;
        }
        int c = pkg_version_cmp_str(av.version, m_old.version);
        if (c == 0) {
            kprintf("pkg upgrade: '%s' is already up-to-date (%s)\n",
                    name, m_old.version);
            return 0;
        }
        if (c < 0) {
            kprintf("pkg upgrade: '%s' installed=%s but repo has older %s "
                    "-- refusing to downgrade\n",
                    name, m_old.version, av.version);
            return -1;
        }
        copy_capped(src_path_buf, av.path, sizeof(src_path_buf));
        src_path = src_path_buf;
        copy_capped(new_version_buf, av.version, sizeof(new_version_buf));
        kprintf("[pkg] upgrade '%s': %s -> %s (from %s)\n",
                name, m_old.version, av.version, av.path);
    } else {
        kprintf("[pkg] applying %s to package '%s' (current=%s)\n",
                src_path, name, m_old.version);
    }

    /* Slurp the new package up-front so we know it parses + fits before
     * we touch the live install. */
    void *new_buf = 0; size_t new_sz = 0;
    rc = vfs_read_all(src_path, &new_buf, &new_sz);
    if (rc != VFS_OK) {
        kprintf("pkg upgrade: cannot read '%s': %s\n",
                src_path, vfs_strerror(rc));
        return -1;
    }

    /* Build + persist the backup blob so we can roll back. The blob is
     * itself a valid .tpkg, so rollback is just apply_replace(.bak). */
    void *bak_buf = 0; size_t bak_sz = 0;
    char  bak[VFS_PATH_MAX];
    bak[0] = '\0';
    if (make_backup) {
        if (build_backup_blob(&m_old, &bak_buf, &bak_sz) != 0) {
            kprintf("pkg upgrade: failed to build backup blob\n");
            kfree(new_buf);
            return -1;
        }
        bak_path(name, bak);
        if (vfs_write_all(bak, bak_buf, bak_sz) != VFS_OK) {
            kprintf("pkg upgrade: failed to write backup '%s'\n", bak);
            kfree(new_buf); kfree(bak_buf);
            return -1;
        }
        kprintf("  backup -> %s (%lu bytes)\n",
                bak, (unsigned long)bak_sz);
    }

    /* Apply the new package. On failure, auto-rollback from the
     * in-memory backup blob (we haven't freed it yet). */
    rc = apply_replace((const char *)new_buf, new_sz, &m_old);
    kfree(new_buf);
    if (rc != 0) {
        kprintf("pkg upgrade: apply failed -- rolling back to %s\n",
                m_old.version);
        if (make_backup && bak_buf) {
            int rrc = restore_from_blob(name, bak_buf, bak_sz);
            if (rrc != 0) {
                kprintf("pkg upgrade: ROLLBACK FAILED -- system in mixed state\n");
                kprintf("            backup preserved at %s; "
                        "try 'pkg rollback %s'\n", bak, name);
            } else {
                kprintf("pkg upgrade: rollback succeeded; "
                        "kept %s for manual inspection\n", bak);
            }
        }
        if (bak_buf) kfree(bak_buf);
        pkg_refresh_launcher();
        return -1;
    }
    if (bak_buf) kfree(bak_buf);

    pkg_refresh_launcher();
    if (new_version_buf[0]) {
        kprintf("[pkg] upgraded %s: %s -> %s\n",
                name, m_old.version, new_version_buf);
        SLOG_INFO(SLOG_SUB_AUDIT,
                  "pkg upgrade OK name=%s old=%s new=%s",
                  name, m_old.version, new_version_buf);
    } else {
        kprintf("[pkg] upgraded %s\n", name);
        SLOG_INFO(SLOG_SUB_AUDIT,
                  "pkg upgrade OK name=%s old=%s",
                  name, m_old.version);
    }
    return 0;
}

static int pkg_upgrade_one_inner(const char *name) {
    if (!name || !*name) { kprintf("pkg upgrade: missing <name>\n"); return -1; }
    return upgrade_internal(name, NULL, /*make_backup=*/true);
}

int pkg_upgrade_one(const char *name) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_upgrade_one_inner(name);
    pkg_priv_end(&pv);
    return rc;
}

static int pkg_upgrade_all_inner(void) {
    struct vfs_dir d;
    int rc = vfs_opendir(PKG_DB_DIR, &d);
    if (rc != VFS_OK) {
        kprintf("pkg upgrade: no install database (%s)\n",
                vfs_strerror(rc));
        return -1;
    }

    /* Snapshot the list of installed package names first; we'll
     * mutate the directory by re-writing install records during the
     * loop and don't want readdir to skip or repeat. */
    char  names[16][PKG_NAME_MAX];
    int   count = 0;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == VFS_OK && count < 16) {
        if (ent.type != VFS_TYPE_FILE) continue;
        if (!ends_with(ent.name, ".pkg")) continue;
        size_t nl = strlen(ent.name);
        if (nl < 4) continue;
        nl -= 4;
        if (nl >= PKG_NAME_MAX) nl = PKG_NAME_MAX - 1;
        memcpy(names[count], ent.name, nl);
        names[count][nl] = '\0';
        count++;
    }
    vfs_closedir(&d);

    int upgraded = 0, skipped = 0, failed = 0;
    for (int i = 0; i < count; i++) {
        struct pkg_manifest mi;
        if (read_install_record(names[i], &mi) != 0) {
            kprintf("  %-20s  unreadable record -- skipping\n", names[i]);
            failed++;
            continue;
        }
        struct pkg_avail av;
        if (repo_find_latest(names[i], &av) != 0) {
            kprintf("  %-20s  installed=%s  no repo entry -- skipping\n",
                    names[i], mi.version);
            skipped++;
            continue;
        }
        int c = pkg_version_cmp_str(av.version, mi.version);
        if (c <= 0) {
            kprintf("  %-20s  %s (already %s)\n", names[i], mi.version,
                    c == 0 ? "current" : "newer than repo");
            skipped++;
            continue;
        }
        if (upgrade_internal(names[i], NULL, /*make_backup=*/true) == 0) {
            upgraded++;
        } else {
            failed++;
        }
    }

    kprintf("\npkg upgrade: %d upgraded, %d skipped, %d failed\n",
            upgraded, skipped, failed);
    return failed == 0 ? 0 : -1;
}

int pkg_upgrade_all(void) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_upgrade_all_inner();
    pkg_priv_end(&pv);
    return rc;
}

/* ---- pkg_upgrade_path (M34B) ------------------------------------ *
 *
 * Apply a SPECIFIC .tpkg as an upgrade. Reads the source's NAME field
 * and dispatches to upgrade_internal with that name. Same gates as
 * pkg_upgrade_one (verify_integrity, auto-rollback on failure) -- this
 * is purely a different way to choose the SOURCE.
 *
 * On failure the existing install is preserved (apply_replace bails
 * before any FS write when verify_integrity fails; if a write fails
 * mid-replace the in-memory backup is restored). Returns 0/-1. */
static int pkg_upgrade_path_inner(const char *src_tpkg) {
    if (!src_tpkg || !*src_tpkg) {
        kprintf("pkg upgrade-path: missing source path\n");
        return -1;
    }

    /* Peek at the source to learn its NAME so upgrade_internal can
     * read the matching install record. We deliberately re-read the
     * file inside upgrade_internal -- the cost is one extra slurp on
     * a tiny .tpkg, in exchange for keeping upgrade_internal's
     * lifetime unchanged. */
    void *buf = 0; size_t sz = 0;
    int rc = vfs_read_all(src_tpkg, &buf, &sz);
    if (rc != VFS_OK) {
        kprintf("pkg upgrade-path: cannot read '%s': %s\n",
                src_tpkg, vfs_strerror(rc));
        return -1;
    }
    struct pkg_manifest peek;
    int prc = parse_manifest((const char *)buf, sz, &peek,
                             /*expect_body=*/true);
    kfree(buf);
    if (prc != 0) {
        kprintf("pkg upgrade-path: malformed package '%s'\n", src_tpkg);
        return -1;
    }
    if (!peek.name[0]) {
        kprintf("pkg upgrade-path: package '%s' has no NAME\n", src_tpkg);
        return -1;
    }
    return upgrade_internal(peek.name, src_tpkg, /*make_backup=*/true);
}

int pkg_upgrade_path(const char *src_tpkg) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_upgrade_path_inner(src_tpkg);
    pkg_priv_end(&pv);
    return rc;
}

/* ---- pkg_rollback ----------------------------------------------- */

static int pkg_rollback_inner(const char *name) {
    if (!name || !*name) { kprintf("pkg rollback: missing <name>\n"); return -1; }

    char bak[VFS_PATH_MAX];
    bak_path(name, bak);
    struct vfs_stat st;
    if (vfs_stat(bak, &st) != VFS_OK) {
        kprintf("pkg rollback: no backup at %s\n", bak);
        return -1;
    }

    struct pkg_manifest m_old;
    int rc = read_install_record(name, &m_old);
    if (rc != 0) {
        kprintf("pkg rollback: '%s' is not currently installed -- "
                "nothing to roll back from\n", name);
        return -1;
    }

    kprintf("[pkg] rolling back '%s' (current=%s) from %s\n",
            name, m_old.version, bak);
    /* Don't make a NEW backup -- that would overwrite the very file
     * we're reading from. */
    int r = upgrade_internal(name, bak, /*make_backup=*/false);
    if (r == 0) {
        kprintf("[pkg] rollback complete\n");
        /* M34F: upgrade_internal already emitted an "upgrade OK" audit
         * line for the underlying replace, but the operator-visible
         * intent here is a rollback. Tag explicitly so audit grep
         * shows the lifecycle phase, not just the implementation
         * detail. */
        SLOG_INFO(SLOG_SUB_AUDIT,
                  "pkg rollback OK name=%s from=%s",
                  name, bak);
    } else {
        SLOG_WARN(SLOG_SUB_AUDIT,
                  "pkg rollback FAIL name=%s from=%s", name, bak);
    }
    return r;
}

int pkg_rollback(const char *name) {
    struct pkg_priv pv;
    pkg_priv_begin(&pv);
    int rc = pkg_rollback_inner(name);
    pkg_priv_end(&pv);
    return rc;
}

/* ---- milestone 17 boot self-test -------------------------------- *
 *
 * Drives the entire upgrade + rollback flow against the helloapp
 * packages shipped in initrd /repo/. The test isn't a unit test
 * harness; it's a "happy path" demo that prints clearly-marked lines
 * to serial so an external script can grep for them. Output begins
 * with "[m17-selftest] step N: ..." so each phase is recognisable.
 *
 * Compiled only when PKG_M17_SELFTEST is defined; otherwise a stub. */

#ifdef PKG_M17_SELFTEST

static const char *m17_status(int rc) { return rc == 0 ? "OK" : "FAIL"; }

void pkg_m17_selftest(void) {
    kprintf("\n========================================\n");
    kprintf("[m17-selftest] milestone 17 begin\n");
    kprintf("========================================\n");

    /* Make sure no leftover install from a previous run interferes.
     * pkg_remove returns -1 if not installed -- harmless. */
    (void)pkg_remove("helloapp");

    kprintf("[m17-selftest] step 1: pkg_install_name(\"helloapp\")\n");
    int rc = pkg_install_name("helloapp");
    kprintf("[m17-selftest] step 1: %s\n", m17_status(rc));
    if (rc != 0) goto out;

    kprintf("[m17-selftest] step 2: pkg_update -- expect UPGRADE row\n");
    pkg_update();

    kprintf("[m17-selftest] step 3: pkg_info BEFORE upgrade\n");
    (void)pkg_info("helloapp");

    kprintf("[m17-selftest] step 4: pkg_upgrade_one(\"helloapp\")\n");
    rc = pkg_upgrade_one("helloapp");
    kprintf("[m17-selftest] step 4: %s\n", m17_status(rc));
    if (rc != 0) goto out;

    kprintf("[m17-selftest] step 5: pkg_info AFTER upgrade\n");
    (void)pkg_info("helloapp");

    kprintf("[m17-selftest] step 6: pkg_upgrade_one again -- expect up-to-date\n");
    rc = pkg_upgrade_one("helloapp");
    kprintf("[m17-selftest] step 6: %s\n", m17_status(rc));

    kprintf("[m17-selftest] step 7: pkg_rollback(\"helloapp\")\n");
    rc = pkg_rollback("helloapp");
    kprintf("[m17-selftest] step 7: %s\n", m17_status(rc));
    if (rc != 0) goto out;

    kprintf("[m17-selftest] step 8: pkg_info AFTER rollback\n");
    (void)pkg_info("helloapp");

    kprintf("[m17-selftest] step 9: pkg_upgrade_all\n");
    rc = pkg_upgrade_all();
    kprintf("[m17-selftest] step 9: %s\n", m17_status(rc));

    kprintf("[m17-selftest] step 10: cleanup pkg_remove\n");
    rc = pkg_remove("helloapp");
    kprintf("[m17-selftest] step 10: %s\n", m17_status(rc));

    /* Wipe the .bak so a subsequent normal boot doesn't show stale
     * rollback artifacts in `ls /data/packages`. */
    char bak[VFS_PATH_MAX];
    bak_path("helloapp", bak);
    (void)vfs_unlink(bak);

out:
    kprintf("========================================\n");
    kprintf("[m17-selftest] milestone 17 end (rc=%d)\n", rc);
    kprintf("========================================\n\n");
}

#else  /* !PKG_M17_SELFTEST */

void pkg_m17_selftest(void) { /* not built in */ }

#endif

/* ===================================================================
 * Milestone 34 boot self-test.
 *
 * Drives the full security pipeline in one place:
 *
 *   M34A  package integrity (HASH/FHASH)
 *   M34B  upgrade verification (corrupt update is rejected)
 *   M34C  signature trust-store (signed/unsigned/tampered/unknown-key)
 *   M34D  capability enforcement (declared CAPS narrow the profile)
 *   M34E  protected-path writes (sysprot_priv lets pkg manager through)
 *
 * Compiled only when -DPKG_M34_SELFTEST is passed (via `make m34test`).
 * The harness uses initrd-shipped fixtures:
 *
 *   /repo/helloapp.tpkg          -- valid v1.0.0
 *   /repo/helloapp_v2.tpkg       -- valid v1.1.0
 *   /repo/helloapp_corrupt.tpkg  -- v1.0.0 with one body byte flipped
 *   /repo/helloapp_signed.tpkg   -- valid + SIG by 'default' key
 *   /repo/helloapp_badsig.tpkg   -- valid hash, tampered SIG
 *   /repo/helloapp_unknown.tpkg  -- valid + SIG by unknown key id
 *
 * All of these are produced by the Makefile's mkpkg invocations.
 * Each step prints "[m34*-selftest] ... PASS|FAIL" so a PowerShell
 * test driver can grep verdicts off serial. Fail-closed: any FAIL
 * stops further sub-tests in that phase.
 *
 * The harness is INTENTIONALLY idempotent: it removes all installs
 * it created so a regular boot afterwards is identical to a fresh
 * boot. */

#ifdef PKG_M34_SELFTEST

static const char *m34_status(int rc) { return rc == 0 ? "PASS" : "FAIL"; }

/* M34A: package integrity. Drives every fixture in /repo/helloapp_*.
 * Each step is independent: a single failure does not abort the rest,
 * so the operator sees the full pass/fail matrix at once. */
static void m34a_selftest(int *failures) {
    kprintf("[m34a-selftest] ----- M34A: package integrity -----\n");

    /* Cleanup any leftover from a previous run. */
    (void)pkg_remove("helloapp");

    char db[VFS_PATH_MAX];
    db_path("helloapp", db);
    struct vfs_stat st;

    /* Step 1: valid package installs cleanly. */
    kprintf("[m34a-selftest] step 1: install valid /repo/helloapp.tpkg\n");
    int rc = pkg_install_path("/repo/helloapp.tpkg");
    int step1 = (rc == 0) ? 0 : -1;
    kprintf("[m34a-selftest] step 1: %s (rc=%d)\n", m34_status(step1), rc);
    if (step1 != 0) (*failures)++;

    /* Step 2: remove so the next install starts clean. Sequence of
     * steps 1 + 2 also exercises the audit-log "pkg install OK" /
     * "pkg remove OK" emissions. */
    kprintf("[m34a-selftest] step 2: pkg_remove helloapp\n");
    rc = pkg_remove("helloapp");
    int step2 = (rc == 0) ? 0 : -1;
    kprintf("[m34a-selftest] step 2: %s (rc=%d)\n", m34_status(step2), rc);
    if (step2 != 0) (*failures)++;

    /* Step 3: body-corrupted package MUST be rejected (HASH path). */
    kprintf("[m34a-selftest] step 3: install corrupt /repo/helloapp_corrupt.tpkg "
            "(expect REJECT)\n");
    rc = pkg_install_path("/repo/helloapp_corrupt.tpkg");
    int step3 = (rc != 0) ? 0 : -1;
    kprintf("[m34a-selftest] step 3: %s (install rc=%d)\n",
            m34_status(step3), rc);
    if (step3 != 0) (*failures)++;

    /* Step 4: confirm step-3 reject left NO trace on disk
     * (fail-closed). */
    int step4 = (vfs_stat(db, &st) == VFS_OK) ? -1 : 0;
    kprintf("[m34a-selftest] step 4: %s (no install record after reject)\n",
            m34_status(step4));
    if (step4 != 0) {
        (*failures)++;
        /* Recover -- otherwise step 6 would refuse to install on top
         * of the leftover. */
        (void)pkg_remove("helloapp");
    }

    /* Step 5: package with WRONG declared HASH (body intact, header
     * lies). Different code path from --corrupt-body but same outcome
     * expected. */
    kprintf("[m34a-selftest] step 5: install /repo/helloapp_badhash.tpkg "
            "(expect REJECT)\n");
    rc = pkg_install_path("/repo/helloapp_badhash.tpkg");
    int step5 = (rc != 0) ? 0 : -1;
    kprintf("[m34a-selftest] step 5: %s (install rc=%d)\n",
            m34_status(step5), rc);
    if (step5 != 0) (*failures)++;

    /* Step 6: package with wrong per-file FHASH for one file but a
     * correct package-wide HASH wouldn't match -- mkpkg's --bad-fhash
     * only mangles the declared FHASH for the named file, leaving the
     * package HASH consistent with the body. The verifier therefore
     * rejects on the FHASH path. */
    kprintf("[m34a-selftest] step 6: install /repo/helloapp_badfhash.tpkg "
            "(expect REJECT)\n");
    rc = pkg_install_path("/repo/helloapp_badfhash.tpkg");
    int step6 = (rc != 0) ? 0 : -1;
    kprintf("[m34a-selftest] step 6: %s (install rc=%d)\n",
            m34_status(step6), rc);
    if (step6 != 0) (*failures)++;

    /* Step 7: pre-M34 unhashed package MUST install under the default
     * WARN policy (back-compat). The audit + sec slog will emit
     * "no HASH/SIG (legacy package)" so an operator can flag the
     * sysadmin. */
    (void)pkg_remove("helloapp");
    kprintf("[m34a-selftest] step 7: install /repo/helloapp_nohash.tpkg "
            "(legacy, expect ACCEPT under WARN policy)\n");
    rc = pkg_install_path("/repo/helloapp_nohash.tpkg");
    int step7 = (rc == 0) ? 0 : -1;
    kprintf("[m34a-selftest] step 7: %s (rc=%d, policy=%s)\n",
            m34_status(step7), rc,
            pkg_get_sig_policy() == PKG_SIG_POLICY_REQUIRED
                ? "REQUIRED" : "WARN");
    if (step7 != 0) (*failures)++;

    /* Cleanup the legacy install before any later sub-test runs. */
    (void)pkg_remove("helloapp");

    kprintf("[m34a-selftest] M34A complete\n");
}

/* M34B: update verification. Drives the upgrade pipeline through both
 * a corrupted source (must reject + auto-rollback so the live install
 * survives) and a clean v1.1 source (must succeed), then exercises
 * pkg_rollback to confirm the .bak path still works after a verified
 * upgrade. Each step prints its own PASS/FAIL line. */
static void m34b_selftest(int *failures) {
    kprintf("[m34b-selftest] ----- M34B: update verification -----\n");

    (void)pkg_remove("helloapp");
    char bak[VFS_PATH_MAX];
    bak_path("helloapp", bak);
    (void)vfs_unlink(bak);

    /* Step 1: install baseline v1.0.0. */
    kprintf("[m34b-selftest] step 1: install /repo/helloapp.tpkg (v1.0.0)\n");
    int rc = pkg_install_path("/repo/helloapp.tpkg");
    int s1 = (rc == 0) ? 0 : -1;
    kprintf("[m34b-selftest] step 1: %s (rc=%d)\n", m34_status(s1), rc);
    if (s1 != 0) (*failures)++;

    /* Step 2: forced upgrade from corrupted v2 fixture must REJECT.
     * The integrity gate runs inside apply_replace BEFORE any FS
     * write, so the live install is never touched. The auto-rollback
     * branch is still exercised (upgrade_internal calls
     * restore_from_blob) which is itself an idempotent re-write of
     * the v1.0.0 files from the .bak. */
    kprintf("[m34b-selftest] step 2: pkg_upgrade_path "
            "/repo/helloapp_v2_corrupt.tpkg (expect REJECT)\n");
    rc = pkg_upgrade_path("/repo/helloapp_v2_corrupt.tpkg");
    int s2 = (rc != 0) ? 0 : -1;
    kprintf("[m34b-selftest] step 2: %s (upgrade rc=%d)\n",
            m34_status(s2), rc);
    if (s2 != 0) (*failures)++;

    /* Step 3: install record must STILL be v1.0.0 (rollback preserved
     * the live install). */
    struct pkg_manifest m_after;
    int s3 = -1;
    if (read_install_record("helloapp", &m_after) == 0 &&
        strcmp(m_after.version, "1.0.0") == 0) {
        s3 = 0;
    }
    kprintf("[m34b-selftest] step 3: %s (post-reject version=%s, expect=1.0.0)\n",
            m34_status(s3),
            (read_install_record("helloapp", &m_after) == 0)
                ? m_after.version : "<gone>");
    if (s3 != 0) (*failures)++;

    /* Step 4: clean upgrade to v1.1.0 via the same forced-path API.
     * Verifies the success path through pkg_upgrade_path AND that
     * verify_integrity accepts the legitimate signed-or-hashed v2. */
    kprintf("[m34b-selftest] step 4: pkg_upgrade_path "
            "/repo/helloapp_v2.tpkg (expect ACCEPT)\n");
    rc = pkg_upgrade_path("/repo/helloapp_v2.tpkg");
    int s4 = (rc == 0) ? 0 : -1;
    kprintf("[m34b-selftest] step 4: %s (upgrade rc=%d)\n",
            m34_status(s4), rc);
    if (s4 != 0) (*failures)++;

    /* Step 5: install record must now read 1.1.0. */
    int s5 = -1;
    char ver_after[PKG_VERSION_MAX] = "<unread>";
    if (read_install_record("helloapp", &m_after) == 0) {
        copy_capped(ver_after, m_after.version, sizeof(ver_after));
        if (strcmp(m_after.version, "1.1.0") == 0) s5 = 0;
    }
    kprintf("[m34b-selftest] step 5: %s (post-upgrade version=%s, expect=1.1.0)\n",
            m34_status(s5), ver_after);
    if (s5 != 0) (*failures)++;

    /* Step 6: rollback must succeed and bring the install back to
     * v1.0.0. This re-uses the .bak that pkg_upgrade_path wrote in
     * step 4. */
    kprintf("[m34b-selftest] step 6: pkg_rollback helloapp\n");
    rc = pkg_rollback("helloapp");
    int s6 = (rc == 0) ? 0 : -1;
    kprintf("[m34b-selftest] step 6: %s (rollback rc=%d)\n",
            m34_status(s6), rc);
    if (s6 != 0) (*failures)++;

    /* Step 7: install record must read 1.0.0 again. */
    int s7 = -1;
    char ver_rb[PKG_VERSION_MAX] = "<unread>";
    if (read_install_record("helloapp", &m_after) == 0) {
        copy_capped(ver_rb, m_after.version, sizeof(ver_rb));
        if (strcmp(m_after.version, "1.0.0") == 0) s7 = 0;
    }
    kprintf("[m34b-selftest] step 7: %s (post-rollback version=%s, expect=1.0.0)\n",
            m34_status(s7), ver_rb);
    if (s7 != 0) (*failures)++;

    /* Cleanup. */
    (void)pkg_remove("helloapp");
    (void)vfs_unlink(bak);

    kprintf("[m34b-selftest] M34B complete\n");
}

/* M34C: package signing groundwork. Drives the verify pipeline
 * through every signed-package outcome we care about today:
 *
 *   - signed by a TRUSTED key id          -> install
 *   - signed by an UNKNOWN key id         -> reject (no key in store)
 *   - signed but body TAMPERED post-MAC   -> reject (tag mismatch)
 *   - UNSIGNED under PKG_SIG_POLICY_REQUIRED -> reject
 *   - UNSIGNED under PKG_SIG_POLICY_WARN     -> install with warn
 *
 * The trust store is shipped read-only inside the initrd at
 * /system/keys/trust.db (loaded by sig_trust_store_init() in kmain),
 * so by the time we get here the test key id "tobyOS-test" must be
 * resolvable via sig_trust_store_find. If it isn't, every later step
 * would fail with the same root cause -- we surface that as step 0
 * so the failure summary points at the real problem. */
static void m34c_selftest(int *failures) {
    kprintf("[m34c-selftest] ----- M34C: package signing -----\n");

    /* Make sure the policy is at its M34C baseline (WARN) -- prior
     * sub-tests are expected to leave it that way, but be defensive. */
    int saved_policy = pkg_get_sig_policy();
    pkg_set_sig_policy(PKG_SIG_POLICY_WARN);

    /* Make sure no helloapp install or .bak is around so each install
     * step is observed cleanly. */
    (void)pkg_remove("helloapp");
    char bak[VFS_PATH_MAX];
    bak_path("helloapp", bak);
    (void)vfs_unlink(bak);

    /* Step 0: trust store sanity check -- the test key id must be
     * present, else the rest of M34C is effectively a no-op. */
    int n_keys = sig_trust_store_count();
    int s0 = (sig_trust_store_find("tobyOS-test") != 0) ? 0 : -1;
    kprintf("[m34c-selftest] step 0: %s (trust store keys=%d, "
            "tobyOS-test=%s)\n",
            m34_status(s0), n_keys, s0 == 0 ? "present" : "MISSING");
    if (s0 != 0) (*failures)++;

    /* Step 1: signed-by-trusted-key package installs cleanly. This
     * exercises the SIG OK path inside verify_integrity. */
    kprintf("[m34c-selftest] step 1: install /repo/helloapp_signed.tpkg "
            "(expect ACCEPT)\n");
    int rc = pkg_install_path("/repo/helloapp_signed.tpkg");
    int s1 = (rc == 0) ? 0 : -1;
    kprintf("[m34c-selftest] step 1: %s (rc=%d)\n", m34_status(s1), rc);
    if (s1 != 0) (*failures)++;
    (void)pkg_remove("helloapp");

    /* Step 2: signed package with an UNKNOWN key id is rejected before
     * the MAC is even computed (sig_verify_hmac returns -1). */
    kprintf("[m34c-selftest] step 2: install /repo/helloapp_signed_unknown.tpkg "
            "(expect REJECT)\n");
    rc = pkg_install_path("/repo/helloapp_signed_unknown.tpkg");
    int s2 = (rc != 0) ? 0 : -1;
    kprintf("[m34c-selftest] step 2: %s (rc=%d)\n", m34_status(s2), rc);
    if (s2 != 0) (*failures)++;

    /* Step 3: signed-and-tampered (body flipped after MAC). The
     * fixture is built with --no-hash so HASH/FHASH can't catch the
     * tamper first; the SIG check is the gate that fires. */
    kprintf("[m34c-selftest] step 3: install /repo/helloapp_signed_tampered.tpkg "
            "(expect REJECT via SIG)\n");
    rc = pkg_install_path("/repo/helloapp_signed_tampered.tpkg");
    int s3 = (rc != 0) ? 0 : -1;
    kprintf("[m34c-selftest] step 3: %s (rc=%d)\n", m34_status(s3), rc);
    if (s3 != 0) (*failures)++;

    /* Step 4: under REQUIRED policy, an UNSIGNED package is rejected
     * even though its HASH/FHASH are perfectly valid. This is the
     * fail-closed path for sites that mandate signed-only repos. */
    kprintf("[m34c-selftest] step 4: install /repo/helloapp.tpkg under REQUIRED "
            "(expect REJECT)\n");
    pkg_set_sig_policy(PKG_SIG_POLICY_REQUIRED);
    rc = pkg_install_path("/repo/helloapp.tpkg");
    int s4 = (rc != 0) ? 0 : -1;
    kprintf("[m34c-selftest] step 4: %s (rc=%d, policy=REQUIRED)\n",
            m34_status(s4), rc);
    if (s4 != 0) (*failures)++;
    pkg_set_sig_policy(PKG_SIG_POLICY_WARN);

    /* Step 5: under WARN (default), the same UNSIGNED package installs
     * cleanly -- the warn path must NOT regress to a hard fail. */
    kprintf("[m34c-selftest] step 5: install /repo/helloapp.tpkg under WARN "
            "(expect ACCEPT)\n");
    rc = pkg_install_path("/repo/helloapp.tpkg");
    int s5 = (rc == 0) ? 0 : -1;
    kprintf("[m34c-selftest] step 5: %s (rc=%d, policy=WARN)\n",
            m34_status(s5), rc);
    if (s5 != 0) (*failures)++;

    /* Cleanup. */
    (void)pkg_remove("helloapp");
    (void)vfs_unlink(bak);
    pkg_set_sig_policy(saved_policy);

    kprintf("[m34c-selftest] M34C complete\n");
}

/* M34D: stronger sandbox defaults.
 *
 * The plumbing under test is the chain
 *
 *     manifest CAPS line --> pkg_manifest.caps (parsed)
 *                        --> install record (persisted)
 *                        --> .app descriptor caps= field
 *                        --> gui_launcher_register_with_profile_caps
 *                        --> proc_spec.declared_caps
 *                        --> cap_apply_declared at proc_spawn
 *
 * We don't actually launch a process here -- the GUI/login pipeline
 * isn't up yet during the m34 selftest, and spawning helloapp would
 * require a working compositor. Instead we drive the same primitives
 * directly with a stack-allocated synthetic struct proc, which is
 * exactly what the runtime path does after profile narrowing. That
 * gives us byte-exact end-to-end coverage of the narrowing math and
 * the parser without depending on the compositor.
 *
 * Steps:
 *   0  install /repo/helloapp_caps_ro.tpkg (CAPS "FILE_READ,GUI")
 *   1  read install record back, confirm m.caps == "FILE_READ,GUI"
 *   2  synthetic proc starts with default-profile caps; apply declared
 *      list; verify caps == (CAP_FILE_READ | CAP_GUI) exactly
 *   3  cap_check denies FILE_WRITE / NET / EXEC under the narrowed proc
 *   4  cap_check allows FILE_READ / GUI under the narrowed proc
 *   5  cap_apply_declared with an unknown token returns -1 BUT still
 *      narrows the known bits (no fail-open)
 *   6  cap_apply_declared NEVER grants ADMIN even if "ADMIN" is in csv
 *   7  empty / NULL csv is a no-op (returns 0, caps unchanged)
 */
static void m34d_selftest(int *failures) {
    kprintf("[m34d-selftest] ----- M34D: sandbox defaults -----\n");

    (void)pkg_remove("helloapp");

    /* Step 0: install the read-only-caps fixture. */
    kprintf("[m34d-selftest] step 0: install "
            "/repo/helloapp_caps_ro.tpkg\n");
    int rc = pkg_install_path("/repo/helloapp_caps_ro.tpkg");
    int s0 = (rc == 0) ? 0 : -1;
    kprintf("[m34d-selftest] step 0: %s (rc=%d)\n", m34_status(s0), rc);
    if (s0 != 0) {
        (*failures)++;
        kprintf("[m34d-selftest] aborting M34D (no install record to read)\n");
        return;
    }

    /* Step 1: read the install record back; CAPS must round-trip
     * verbatim through emit_install_record + the parser. */
    struct pkg_manifest m;
    int s1 = -1;
    if (read_install_record("helloapp", &m) == 0 &&
        strcmp(m.caps, "FILE_READ,GUI") == 0) {
        s1 = 0;
    }
    kprintf("[m34d-selftest] step 1: %s (record caps='%s', expect "
            "'FILE_READ,GUI')\n",
            m34_status(s1),
            (read_install_record("helloapp", &m) == 0) ? m.caps : "<unread>");
    if (s1 != 0) (*failures)++;

    /* Build a synthetic proc that mirrors what proc_spawn would
     * produce after cap_profile_apply("default") -- i.e. all user-mode
     * caps, no ADMIN, no path jail. cap_check only ever touches caps,
     * pid, name; the rest is fine left zeroed. */
    struct proc fake = {0};
    fake.pid    = 9990;
    {
        const char *nm = "m34d-fake";
        size_t i = 0;
        while (nm[i] && i + 1 < sizeof(fake.name)) {
            fake.name[i] = nm[i];
            i++;
        }
        fake.name[i] = '\0';
    }
    fake.caps = CAP_GROUP_ALL_USER;

    /* Step 2: declared narrowing yields exactly FILE_READ | GUI. */
    int rc2 = cap_apply_declared(&fake, m.caps);
    uint32_t expect_mask = CAP_FILE_READ | CAP_GUI;
    int s2 = (rc2 == 0 && fake.caps == expect_mask) ? 0 : -1;
    {
        char got_s[96];
        cap_mask_to_string(fake.caps, got_s, sizeof(got_s));
        kprintf("[m34d-selftest] step 2: %s (apply rc=%d, caps='%s', "
                "expect 'FILE_READ,GUI')\n",
                m34_status(s2), rc2, got_s);
    }
    if (s2 != 0) (*failures)++;

    /* Step 3: denied operations actually fail. cap_check returns false
     * AND emits a "[cap] deny pid=... 'm34d-fake' in <what>" line per
     * call -- those lines double as M34F audit-style proof of the
     * negative path. */
    bool deny_w = !cap_check(&fake, CAP_FILE_WRITE,    "m34d.write");
    bool deny_n = !cap_check(&fake, CAP_NET,           "m34d.net");
    bool deny_e = !cap_check(&fake, CAP_EXEC,          "m34d.exec");
    bool deny_s = !cap_check(&fake, CAP_SETTINGS_WRITE,"m34d.settings");
    int s3 = (deny_w && deny_n && deny_e && deny_s) ? 0 : -1;
    kprintf("[m34d-selftest] step 3: %s "
            "(deny: write=%d net=%d exec=%d settings=%d)\n",
            m34_status(s3),
            (int)deny_w, (int)deny_n, (int)deny_e, (int)deny_s);
    if (s3 != 0) (*failures)++;

    /* Step 4: granted operations succeed. */
    bool ok_r = cap_check(&fake, CAP_FILE_READ, "m34d.read");
    bool ok_g = cap_check(&fake, CAP_GUI,       "m34d.gui");
    int s4 = (ok_r && ok_g) ? 0 : -1;
    kprintf("[m34d-selftest] step 4: %s (allow: read=%d gui=%d)\n",
            m34_status(s4), (int)ok_r, (int)ok_g);
    if (s4 != 0) (*failures)++;

    /* Step 5: unknown token in csv. parse returns -1, but the known
     * tokens still narrow -- the unknown one is dropped, never
     * silently widened. */
    struct proc fake2 = {0};
    fake2.pid  = 9991;
    {
        const char *nm = "m34d-fake2";
        size_t i = 0;
        while (nm[i] && i + 1 < sizeof(fake2.name)) {
            fake2.name[i] = nm[i];
            i++;
        }
        fake2.name[i] = '\0';
    }
    fake2.caps = CAP_GROUP_ALL_USER;
    int rc5 = cap_apply_declared(&fake2, "FILE_READ,BOGUS_BIT,NET");
    int s5 = (rc5 != 0 &&
              fake2.caps == (CAP_FILE_READ | CAP_NET)) ? 0 : -1;
    {
        char got_s[96];
        cap_mask_to_string(fake2.caps, got_s, sizeof(got_s));
        kprintf("[m34d-selftest] step 5: %s (rc=%d, caps='%s', expect "
                "'FILE_READ,NET')\n",
                m34_status(s5), rc5, got_s);
    }
    if (s5 != 0) (*failures)++;

    /* Step 6: ADMIN can NEVER be granted via a declared list, even
     * if the parent already had ADMIN, the child must not get it
     * back through this path. We start with non-ADMIN caps to make
     * the assertion sharp. */
    struct proc fake3 = {0};
    fake3.pid  = 9992;
    {
        const char *nm = "m34d-fake3";
        size_t i = 0;
        while (nm[i] && i + 1 < sizeof(fake3.name)) {
            fake3.name[i] = nm[i];
            i++;
        }
        fake3.name[i] = '\0';
    }
    fake3.caps = CAP_GROUP_ALL_USER;
    (void)cap_apply_declared(&fake3, "FILE_READ,ADMIN,GUI");
    int s6 = ((fake3.caps & CAP_ADMIN) == 0 &&
              fake3.caps == (CAP_FILE_READ | CAP_GUI)) ? 0 : -1;
    {
        char got_s[96];
        cap_mask_to_string(fake3.caps, got_s, sizeof(got_s));
        kprintf("[m34d-selftest] step 6: %s (caps='%s', expect "
                "'FILE_READ,GUI', ADMIN must NOT be set)\n",
                m34_status(s6), got_s);
    }
    if (s6 != 0) (*failures)++;

    /* Step 7: empty/NULL csv leaves caps untouched (so a legacy app
     * with no CAPS line keeps the profile's caps). */
    struct proc fake4 = {0};
    fake4.pid  = 9993;
    {
        const char *nm = "m34d-fake4";
        size_t i = 0;
        while (nm[i] && i + 1 < sizeof(fake4.name)) {
            fake4.name[i] = nm[i];
            i++;
        }
        fake4.name[i] = '\0';
    }
    fake4.caps = CAP_GROUP_ALL_USER;
    int rc7a = cap_apply_declared(&fake4, "");
    int rc7b = cap_apply_declared(&fake4, 0);
    int s7   = (rc7a == 0 && rc7b == 0 &&
                fake4.caps == CAP_GROUP_ALL_USER) ? 0 : -1;
    kprintf("[m34d-selftest] step 7: %s (rc empty=%d null=%d, caps "
            "preserved=%d)\n",
            m34_status(s7), rc7a, rc7b,
            (int)(fake4.caps == CAP_GROUP_ALL_USER));
    if (s7 != 0) (*failures)++;

    /* Cleanup. */
    (void)pkg_remove("helloapp");

    kprintf("[m34d-selftest] M34D complete\n");
}

/* M34E: system file protection.
 *
 * The plumbing under test is the chain
 *
 *     sysprot_check_write   -- pure decision function
 *     vfs_create / vfs_unlink / vfs_mkdir / vfs_chmod / vfs_chown
 *                           -- called from src/vfs.c on every write op
 *     vfs_open + vfs_write  -- handle re-check via f->sysprot
 *     pkg_priv_begin/end    -- the only legitimate way to write
 *                              under /system, /data/packages, etc.
 *                              from a non-pid-0 caller
 *
 * The boot self-test runs in pid 0 context with sysprot's "pid 0
 * implicit bypass" enabled by default. To exercise the deny path we
 * flip sysprot_set_test_strict(true) -- that suppresses the bypass
 * but still honours the priv counter, so pkg_priv-bracketed writes
 * succeed and bare writes are denied + audited.
 *
 * Steps:
 *   0  table sanity: every prefix in the spec is recognised by
 *      sysprot_is_protected; non-protected paths return false
 *   1  in strict mode, vfs_write_all on /system/keys/m34e_probe.txt
 *      is rejected with VFS_ERR_PERM and emits a sysprot audit line
 *   2  same write inside pkg_priv_begin/end succeeds
 *   3  vfs_unlink in strict mode under /data/packages is denied
 *   4  the same unlink inside pkg_priv_begin succeeds (cleanup of
 *      the file written in step 2)
 *   5  reads under /system are NEVER blocked by sysprot (vfs_open
 *      + vfs_read on the trust DB still works in strict mode)
 *   6  non-protected paths are unaffected (writing /data/m34e_probe2
 *      succeeds in strict mode)
 *   7  end-to-end via pkg_install: installing helloapp under strict
 *      mode succeeds because pkg_priv brackets the writes; the
 *      audit log shows the install events but no sysprot-deny
 *      events for the install itself
 */
static void m34e_selftest(int *failures) {
    kprintf("[m34e-selftest] ----- M34E: system file protection -----\n");

    /* Step 0: prefix table sanity. */
    bool s0_ok =
        sysprot_is_protected("/system")           &&
        sysprot_is_protected("/system/keys")      &&
        sysprot_is_protected("/system/keys/trust.db") &&
        sysprot_is_protected("/boot")             &&
        sysprot_is_protected("/data/packages")    &&
        sysprot_is_protected("/data/packages/helloapp.bak") &&
        sysprot_is_protected("/data/users")       &&
        !sysprot_is_protected("/data")            &&
        !sysprot_is_protected("/data/foo")        &&
        !sysprot_is_protected("/system-evil")     &&
        !sysprot_is_protected("");
    int s0 = s0_ok ? 0 : -1;
    kprintf("[m34e-selftest] step 0: %s (prefix table sanity)\n",
            m34_status(s0));
    if (s0 != 0) (*failures)++;

    /* Engage strict mode for the rest of M34E so pid 0 doesn't sail
     * through the bypass. EVERY exit path below MUST disengage it.
     *
     * NOTE: we test writes against /data/packages (a WRITABLE
     * protected mount point), not /system. /system is mounted from
     * the read-only initrd ramfs, so any write there returns
     * VFS_ERR_ROFS regardless of sysprot -- that's a fine final-
     * state property (the kernel image and trust DB really ARE
     * read-only at runtime) but it doesn't exercise the sysprot
     * gate. /data/packages is writable on the persistent disk and
     * is the realistic target for the M34E rule. */
    sysprot_set_test_strict(true);

    /* Step 1: bare write to /data/packages/* MUST be denied. */
    const char *probe1 = "/data/packages/m34e_probe.txt";
    int rc1 = vfs_write_all(probe1, "deny me", 7);
    int s1 = (rc1 == VFS_ERR_PERM) ? 0 : -1;
    kprintf("[m34e-selftest] step 1: %s (write %s rc=%d, expect "
            "VFS_ERR_PERM=%d)\n",
            m34_status(s1), probe1, rc1, VFS_ERR_PERM);
    if (s1 != 0) (*failures)++;

    /* Step 2: same write inside a privileged scope MUST succeed.
     * Use a real pkg_priv pair so this also exercises the pkg_priv
     * -> sysprot integration. */
    int s2 = -1;
    {
        struct pkg_priv pp;
        pkg_priv_begin(&pp);
        int rc2 = vfs_write_all(probe1, "allow me", 8);
        pkg_priv_end(&pp);
        s2 = (rc2 == VFS_OK) ? 0 : -1;
        kprintf("[m34e-selftest] step 2: %s (priv write %s rc=%d)\n",
                m34_status(s2), probe1, rc2);
    }
    if (s2 != 0) (*failures)++;

    /* Step 3: vfs_unlink of the file we just created MUST be denied
     * outside a privileged scope. */
    int rc3 = vfs_unlink(probe1);
    int s3 = (rc3 == VFS_ERR_PERM) ? 0 : -1;
    kprintf("[m34e-selftest] step 3: %s (unlink %s rc=%d, expect "
            "VFS_ERR_PERM=%d)\n",
            m34_status(s3), probe1, rc3, VFS_ERR_PERM);
    if (s3 != 0) (*failures)++;

    /* Step 4: the same unlink inside a priv scope succeeds (and also
     * cleans up steps 2 + 3). */
    int s4 = -1;
    {
        struct pkg_priv pp;
        pkg_priv_begin(&pp);
        int rc4 = vfs_unlink(probe1);
        pkg_priv_end(&pp);
        s4 = (rc4 == VFS_OK) ? 0 : -1;
        kprintf("[m34e-selftest] step 4: %s (priv unlink rc=%d)\n",
                m34_status(s4), rc4);
    }
    if (s4 != 0) (*failures)++;

    /* Step 5: reads of /system MUST still work even in strict mode.
     * The trust DB is shipped in initrd, so it's always readable. */
    void  *buf  = 0;
    size_t size = 0;
    int rc5 = vfs_read_all("/system/keys/trust.db", &buf, &size);
    int s5 = (rc5 == VFS_OK && size > 0) ? 0 : -1;
    kprintf("[m34e-selftest] step 5: %s (read trust.db rc=%d size=%lu)\n",
            m34_status(s5), rc5, (unsigned long)size);
    if (buf) kfree(buf);
    if (s5 != 0) (*failures)++;

    /* Step 6: non-protected paths are unaffected. /data/ itself is
     * NOT protected (only /data/packages, /data/users) so writes
     * sail through. We use a probe that pkg_init has already
     * created (/data/) -- writing a child file there in strict
     * mode must succeed without any priv scope. */
    const char *probe6 = "/data/m34e_probe2.txt";
    int rc6 = vfs_write_all(probe6, "ok", 2);
    int s6 = (rc6 == VFS_OK) ? 0 : -1;
    kprintf("[m34e-selftest] step 6: %s (non-protected write rc=%d)\n",
            m34_status(s6), rc6);
    /* Cleanup -- non-protected so plain unlink works. */
    (void)vfs_unlink(probe6);
    if (s6 != 0) (*failures)++;

    /* Step 7: end-to-end. With strict mode still ON, install a
     * package -- pkg_install_path opens its own pkg_priv so this
     * MUST succeed even though strict mode is suppressing pid 0's
     * bypass. Then remove cleanly. */
    (void)pkg_remove("helloapp");
    int rc7a = pkg_install_path("/repo/helloapp.tpkg");
    int rc7b = pkg_remove("helloapp");
    int s7 = (rc7a == 0 && rc7b == 0) ? 0 : -1;
    kprintf("[m34e-selftest] step 7: %s (priv install rc=%d, remove "
            "rc=%d under strict mode)\n",
            m34_status(s7), rc7a, rc7b);
    if (s7 != 0) (*failures)++;

    /* Disengage strict mode. */
    sysprot_set_test_strict(false);

    kprintf("[m34e-selftest] M34E complete\n");
}

/* ---- M34F: audit logging ---------------------------------------- *
 *
 * Verifies that the SLOG_SUB_AUDIT subsystem actually receives entries
 * for the events M34F promises:
 *
 *   - failed permission / sysprot writes (via cap.c + sysprot.c)
 *   - package install OK
 *   - package install REJECT on integrity failure
 *   - package upgrade OK + REJECT on integrity failure
 *   - package remove OK
 *   - signature/hash verification failures (via sec.c -> SLOG_SUB_SEC)
 *
 * Strategy: snapshot slog seq before each step, run the operation,
 * then drain the ring and look for an entry on the AUDIT or SEC tag
 * whose msg contains a known substring. We do NOT depend on console
 * fan-out; the ring is the source of truth.
 *
 * Notes:
 *   - We deliberately don't read /data/system.log (that path is
 *     M28A territory and may be flushed asynchronously). The ring
 *     has the freshest copy.
 *   - The shell builtin `auditlog` shares this exact ring, so a green
 *     selftest also implies the operator-facing tool will see the
 *     same events. */
/* Tiny strstr -- klibc.h doesn't currently expose one, and pulling
 * one in just for the M34F selftest isn't worth the surface area. */
static const char *m34f_strstr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    if (!*needle) return hay;
    for (const char *p = hay; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return p;
    }
    return NULL;
}

static bool audit_ring_has(uint64_t since_seq,
                           const char *sub_want,
                           const char *needle)
{
    static struct abi_slog_record snap[ABI_SLOG_RING_DEPTH];
    uint32_t got = slog_drain(snap, ABI_SLOG_RING_DEPTH, since_seq);
    for (uint32_t i = 0; i < got; i++) {
        if (sub_want && strcmp(snap[i].sub, sub_want) != 0) continue;
        if (needle && !m34f_strstr(snap[i].msg, needle)) continue;
        return true;
    }
    return false;
}

static uint64_t audit_now_seq(void)
{
    struct abi_slog_stats s;
    slog_stats(&s);
    return s.total_emitted;
}

static void m34f_selftest(int *failures) {
    kprintf("[m34f-selftest] ----- M34F: audit logging -----\n");

    /* Step 0: an explicit emit is observable on the AUDIT tag. This
     * is the smoke test -- if this fails, all the other steps below
     * are testing against a broken ring. */
    uint64_t before0 = audit_now_seq();
    SLOG_INFO(SLOG_SUB_AUDIT, "m34f selftest probe %llu",
              (unsigned long long)before0);
    bool s0_hit = audit_ring_has(before0, SLOG_SUB_AUDIT,
                                 "m34f selftest probe");
    int s0 = s0_hit ? 0 : -1;
    kprintf("[m34f-selftest] step 0: %s (audit ring observable)\n",
            m34_status(s0));
    if (s0 != 0) (*failures)++;

    /* Step 1: package install OK -> AUDIT line. We make sure helloapp
     * is gone, then install it from the standard test fixture. */
    (void)pkg_remove("helloapp");
    uint64_t before1 = audit_now_seq();
    int rc1 = pkg_install_path("/repo/helloapp.tpkg");
    bool s1_hit = audit_ring_has(before1, SLOG_SUB_AUDIT,
                                 "pkg install OK name=helloapp");
    int s1 = (rc1 == 0 && s1_hit) ? 0 : -1;
    kprintf("[m34f-selftest] step 1: %s (install rc=%d, audit hit=%d)\n",
            m34_status(s1), rc1, (int)s1_hit);
    if (s1 != 0) (*failures)++;

    /* Step 2: package remove OK -> AUDIT line. */
    uint64_t before2 = audit_now_seq();
    int rc2 = pkg_remove("helloapp");
    bool s2_hit = audit_ring_has(before2, SLOG_SUB_AUDIT,
                                 "pkg remove OK name=helloapp");
    int s2 = (rc2 == 0 && s2_hit) ? 0 : -1;
    kprintf("[m34f-selftest] step 2: %s (remove rc=%d, audit hit=%d)\n",
            m34_status(s2), rc2, (int)s2_hit);
    if (s2 != 0) (*failures)++;

    /* Step 3: install REJECT on integrity failure -> AUDIT line +
     * SEC verification line. We use the corrupted fixture that M34A
     * already validates. */
    uint64_t before3 = audit_now_seq();
    int rc3 = pkg_install_path("/repo/helloapp_badhash.tpkg");
    bool s3_audit = audit_ring_has(before3, SLOG_SUB_AUDIT,
                                   "pkg install REJECT");
    bool s3_sec   = audit_ring_has(before3, SLOG_SUB_SEC, NULL);
    int s3 = (rc3 != 0 && s3_audit && s3_sec) ? 0 : -1;
    kprintf("[m34f-selftest] step 3: %s (install rc=%d, audit=%d sec=%d)\n",
            m34_status(s3), rc3, (int)s3_audit, (int)s3_sec);
    if (s3 != 0) (*failures)++;

    /* Step 4: sysprot denial (write to a protected path with no
     * pkg_priv scope) emits a SYSPROT WARN line. We use the same
     * /data/packages probe that m34e_selftest already validates as
     * denied so we're testing only the AUDIT plumbing here, not the
     * underlying enforcement. */
    sysprot_set_test_strict(true);
    uint64_t before4 = audit_now_seq();
    const char *probe4 = "/data/packages/m34f_probe.txt";
    int rc4 = vfs_write_all(probe4, "audit me", 8);
    bool s4_hit = audit_ring_has(before4, SLOG_SUB_SYSPROT, "deny");
    sysprot_set_test_strict(false);
    int s4 = (rc4 == VFS_ERR_PERM && s4_hit) ? 0 : -1;
    kprintf("[m34f-selftest] step 4: %s (write rc=%d, audit hit=%d)\n",
            m34_status(s4), rc4, (int)s4_hit);
    if (s4 != 0) (*failures)++;

    /* Step 5: upgrade REJECT on integrity failure -> AUDIT line. We
     * need a live install for upgrade_internal to even get to the
     * verify step. Install good helloapp first, then try to upgrade
     * it from the bad-fhash fixture. */
    int rc_pre  = pkg_install_path("/repo/helloapp.tpkg");
    uint64_t before5 = audit_now_seq();
    int rc5 = pkg_upgrade_path("/repo/helloapp_v2_corrupt.tpkg");
    bool s5_hit = audit_ring_has(before5, SLOG_SUB_AUDIT,
                                 "pkg upgrade REJECT");
    int s5 = (rc_pre == 0 && rc5 != 0 && s5_hit) ? 0 : -1;
    kprintf("[m34f-selftest] step 5: %s (pre rc=%d, upgrade rc=%d, "
            "audit hit=%d)\n",
            m34_status(s5), rc_pre, rc5, (int)s5_hit);
    if (s5 != 0) (*failures)++;

    /* Cleanup so subsequent selftests start from a known state. */
    (void)pkg_remove("helloapp");

    kprintf("[m34f-selftest] M34F complete\n");
}

void pkg_m34_selftest(void) {
    kprintf("\n========================================\n");
    kprintf("[m34-selftest] milestone 34 begin\n");
    kprintf("========================================\n");

    int failures = 0;
    m34a_selftest(&failures);
    m34b_selftest(&failures);
    m34c_selftest(&failures);
    m34d_selftest(&failures);
    m34e_selftest(&failures);
    m34f_selftest(&failures);

    /* Cleanup at the end so a normal boot has no leftovers. */
    (void)pkg_remove("helloapp");
    char bak[VFS_PATH_MAX];
    bak_path("helloapp", bak);
    (void)vfs_unlink(bak);

    kprintf("========================================\n");
    kprintf("[m34-selftest] milestone 34 end (failures=%d) -- %s\n",
            failures, failures == 0 ? "PASS" : "FAIL");
    kprintf("========================================\n\n");
}

#else  /* !PKG_M34_SELFTEST */

void pkg_m34_selftest(void) { /* not built in */ }

#endif
