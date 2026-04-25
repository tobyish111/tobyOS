/* sysprot.c -- system file protection (milestone 34E).
 *
 * See <tobyos/sysprot.h> for the high-level model. The policy is
 * intentionally tiny: a static table of '/'-anchored prefixes, a
 * per-process privileged-scope counter, and a check function the VFS
 * write hooks consult before delegating to the driver.
 *
 * Three places are wired today:
 *
 *   <tobyos/proc.h>::struct proc.sysprot_priv     -- per-proc counter
 *   <tobyos/sysprot.h>                            -- API surface
 *   src/vfs.c::vfs_create / vfs_unlink / vfs_mkdir / vfs_chmod /
 *              vfs_chown / vfs_write_all          -- enforcement
 *
 * The package manager (src/pkg.c) opens a sysprot scope inside
 * pkg_priv_begin so its writes under /data/packages and the
 * .app-descriptor copy under /system reach the disk; an end-user's
 * sandboxed process trying to write /data/packages/foo gets
 * VFS_ERR_PERM and an audit line.
 *
 * Reads are NEVER affected -- a userland tool that today can read
 * /system/keys/trust.db can still read it. M34E is about preventing
 * MUTATION; confidentiality is the cap-system's job.
 */

#include <tobyos/sysprot.h>
#include <tobyos/proc.h>
#include <tobyos/vfs.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

/* ---- protected-prefix table -------------------------------------- *
 *
 * Order matters only insofar as the LONGEST matching prefix wins for
 * intent reporting (the audit line names which prefix matched). For
 * the boolean check itself any match is enough.
 *
 * NOTE: each entry MUST start with '/' and MUST NOT end with '/'.
 * Matching is exact at a path component boundary -- "/system" matches
 * "/system" and "/system/foo" but NOT "/system-evil/foo". */
struct sysprot_prefix {
    const char *path;
    const char *label;     /* short human name for audit logs */
};

static const struct sysprot_prefix g_prefixes[] = {
    { "/system",        "system"        },
    { "/system/keys",   "trust-store"   },
    { "/boot",          "boot"          },
    { "/data/packages", "package-db"    },
    { "/data/users",    "user-db"       },
};

#define PREFIX_COUNT  ((int)(sizeof(g_prefixes) / sizeof(g_prefixes[0])))

/* Test override (see <tobyos/sysprot.h>). */
static bool g_test_strict = false;

/* ---- helpers ---------------------------------------------------- */

/* True iff `path` starts with `prefix` AND the next char is '\0' or
 * '/'. So "/system" matches "/system", "/system/keys" but NOT
 * "/system-evil". */
static bool path_starts_with(const char *path, const char *prefix) {
    size_t pn = strlen(prefix);
    if (strncmp(path, prefix, pn) != 0) return false;
    char next = path[pn];
    return next == '\0' || next == '/';
}

/* Return the longest-matching prefix entry, or NULL. */
static const struct sysprot_prefix *match_prefix(const char *path) {
    if (!path || path[0] != '/') return 0;
    const struct sysprot_prefix *best = 0;
    size_t best_len = 0;
    for (int i = 0; i < PREFIX_COUNT; i++) {
        if (!path_starts_with(path, g_prefixes[i].path)) continue;
        size_t len = strlen(g_prefixes[i].path);
        if (len > best_len) { best = &g_prefixes[i]; best_len = len; }
    }
    return best;
}

/* ---- API -------------------------------------------------------- */

bool sysprot_is_protected(const char *path) {
    return match_prefix(path) != 0;
}

uint32_t sysprot_priv_depth(const struct proc *p) {
    return p ? p->sysprot_priv : 0;
}

/* Return true if `p` is currently allowed to MUTATE protected paths.
 *
 * Bypass conditions:
 *   1. p == NULL                  -- in-kernel boot context (before
 *                                    proc_init or from a deep helper
 *                                    that never set up current_proc)
 *   2. p->pid == 0                -- the kernel main thread; this is
 *                                    where boot-time users_init,
 *                                    pkg_init, settings_init, and the
 *                                    serial-console shell run. Suppressed
 *                                    when sysprot_test_strict is set so
 *                                    the M34E selftest can drive the
 *                                    deny path from kmain.
 *   3. p->sysprot_priv > 0        -- inside a sysprot_priv_begin/end
 *                                    region (pkg_priv, updater, etc.)
 *
 * Anything else: deny. */
static bool writes_allowed(const struct proc *p) {
    if (!p) return true;
    if (p->sysprot_priv > 0) return true;
    if (!g_test_strict && p->pid == 0) return true;
    return false;
}

int sysprot_check_write(const struct proc *p, const char *path,
                        const char *what) {
    const struct sysprot_prefix *m = match_prefix(path);
    if (!m) return VFS_OK;                       /* not protected   */
    if (writes_allowed(p)) return VFS_OK;        /* privileged path */

    /* Denied -- emit a structured audit entry AND a one-line kprintf
     * so a tail of serial.log is also obvious to a human operator. */
    SLOG_WARN(SLOG_SUB_SYSPROT,
              "deny pid=%d uid=%d '%s' path=%s prefix=/%s op=%s",
              p ? p->pid  : -1,
              p ? p->uid  : -1,
              p ? p->name : "(kernel)",
              path ? path : "(null)",
              m->label,
              what ? what : "?");
    kprintf("[sysprot] deny pid=%d '%s' op=%s path=%s prefix=%s\n",
            p ? p->pid  : -1,
            p ? p->name : "(kernel)",
            what ? what : "?",
            path ? path : "(null)",
            m->path);
    return VFS_ERR_PERM;
}

void sysprot_priv_begin(struct sysprot_priv_scope *s) {
    if (!s) return;
    s->p      = current_proc();
    s->active = false;
    if (!s->p) return;                /* boot before proc_init */
    s->p->sysprot_priv++;
    s->active = true;
}

void sysprot_priv_end(struct sysprot_priv_scope *s) {
    if (!s || !s->active || !s->p) return;
    if (s->p->sysprot_priv > 0) s->p->sysprot_priv--;
    s->active = false;
    s->p      = 0;
}

void sysprot_set_test_strict(bool on) { g_test_strict = on; }
bool sysprot_get_test_strict(void)    { return g_test_strict; }

void sysprot_init(void) {
    /* Today the prefix table is static and a no-op suffices, but log
     * the active set so an operator skimming serial.log can see what
     * the kernel currently considers protected. */
    kprintf("[sysprot] init: %d protected prefix(es)\n", PREFIX_COUNT);
    for (int i = 0; i < PREFIX_COUNT; i++) {
        kprintf("[sysprot]   %-15s  %s\n",
                g_prefixes[i].path, g_prefixes[i].label);
    }
}
