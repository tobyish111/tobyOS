/* sysprot.h -- system file protection (milestone 34E).
 *
 * tobyOS designates a small set of paths as PROTECTED. Writing to,
 * unlinking from, or modifying the metadata of anything under one of
 * those prefixes is denied at the VFS layer unless the caller is in a
 * scoped privileged mode (sysprot_priv_begin/end) or is the kernel
 * itself (pid 0). The intent is two-fold:
 *
 *   - protect tobyOS install state from accidental damage by
 *     userland writes (a buggy app, a stray `pkg remove ../../system`)
 *   - record EVERY denied attempt through SLOG_SUB_SYSPROT so the
 *     M34F audit log shows exactly who tried what
 *
 * Protected prefixes (matched on '/' boundaries):
 *
 *   /system            kernel, system binaries, trust DB
 *   /system/keys       trust store (subset of /system)
 *   /boot              bootloader, kernel image
 *   /data/packages     install records + .bak rollback blobs
 *   /data/users        user database
 *
 * Non-protected paths flow through unchanged. Reads are NEVER
 * restricted by sysprot -- ordinary file modes still gate them and
 * sandbox capabilities still gate them, but a process that can read
 * /system/keys/trust.db today can still read it after M34E.
 *
 * ---- privileged scopes ------------------------------------------
 *
 * The package manager, updater, and a handful of boot-time helpers
 * legitimately need to write protected paths. They wrap the write
 * region in a paired begin/end:
 *
 *     struct sysprot_priv_scope s;
 *     sysprot_priv_begin(&s);
 *     // ... vfs_write_all, vfs_unlink, ... succeed even on /system
 *     sysprot_priv_end(&s);
 *
 * Scopes nest correctly via a per-process counter; an outer scope
 * stays open after an inner scope closes. The counter lives in
 * struct proc so it follows the kernel scheduler naturally.
 *
 * ---- test override ----------------------------------------------
 *
 * `sysprot_set_test_strict(true)` forces sysprot_check_write to
 * evaluate as if every caller had pid != 0, regardless of who
 * actually called. The privilege counter is still honoured. The
 * M34E boot self-test uses this to drive the deny path from the
 * kernel context (pid 0) without having to spawn a sandboxed app.
 * Always false in production. */

#ifndef TOBYOS_SYSPROT_H
#define TOBYOS_SYSPROT_H

#include <tobyos/types.h>

struct proc;

/* Scope cookie used by callers to pair begin/end. The struct exists
 * so a future implementation can stash extra state without changing
 * every caller; today it just remembers which proc we incremented. */
struct sysprot_priv_scope {
    struct proc *p;       /* proc whose counter we bumped (NULL = no-op) */
    bool         active;  /* true between begin and end */
};

/* True iff `path` falls under one of the protected prefixes. NULL
 * path returns false. */
bool sysprot_is_protected(const char *path);

/* Decide whether `p` may MUTATE `path`. Returns:
 *   VFS_OK         path is not protected, OR caller is privileged
 *   VFS_ERR_PERM   path is protected and caller is not privileged
 *
 * On VFS_ERR_PERM also emits an SLOG_SUB_SYSPROT audit line through
 * slog so the auditlog tool sees every blocked attempt. `what` is a
 * short label used only in the log line ("vfs_create" / "vfs_unlink"
 * / etc.). */
int  sysprot_check_write(const struct proc *p, const char *path,
                         const char *what);

/* Open / close a privileged scope on the current process. Nests
 * correctly (counter-based). The scope is per-proc, so a syscall
 * that returns to userland with sysprot_priv > 0 is a kernel bug --
 * every begin must have a matching end on every return path. */
void sysprot_priv_begin(struct sysprot_priv_scope *s);
void sysprot_priv_end  (struct sysprot_priv_scope *s);

/* Pure introspection: depth of the active scope on `p`. */
uint32_t sysprot_priv_depth(const struct proc *p);

/* Test hook: when true, sysprot_check_write IGNORES the pid-0 implicit
 * bypass. Privilege scopes still work. Used only by the M34E self-
 * test. Default false. */
void sysprot_set_test_strict(bool on);
bool sysprot_get_test_strict(void);

/* Boot init: registers the protected-prefix table. Currently a no-op
 * (the table is static), but call it from kmain so a future runtime
 * config file can be loaded here. */
void sysprot_init(void);

#endif /* TOBYOS_SYSPROT_H */
