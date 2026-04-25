/* cap.h -- capability + sandbox profile model (milestone 18).
 *
 * Every process carries a small capability bitmask and an optional
 * "sandbox root" path prefix. The kernel consults the bitmask (and,
 * for anything that touches the filesystem, the root prefix) at the
 * top of every enforcement point:
 *
 *   VFS         vfs_open / opendir / stat / create / unlink /
 *               mkdir / chmod / chown / read_all / write_all
 *               (plus a re-check in vfs_write on the open handle)
 *   Syscall     sys_exec, sys_socket/bind/sendto/recvfrom,
 *               sys_gui_create, sys_term_open,
 *               sys_setting_set / login / logout / chmod / chown
 *   Network     sock_sendto / sock_recvfrom (defence-in-depth)
 *
 * Capabilities are assigned when the PCB is created and are NEVER
 * widened afterwards. A child always inherits its parent's caps; a
 * launcher may further restrict the child by supplying a sandbox
 * profile name -- the profile's mask is AND'd into the inherited
 * mask, never OR'd.
 *
 * ---- bit layout ---------------------------------------------------
 *
 *   CAP_FILE_READ        open/readdir/read files
 *   CAP_FILE_WRITE       create/unlink/mkdir/write files
 *   CAP_EXEC             spawn a child process (sys_exec, launch q)
 *   CAP_NET              create sockets, send/recv packets
 *   CAP_GUI              open a compositor window
 *   CAP_TERM             open a GUI terminal session
 *   CAP_SETTINGS_WRITE   setting_set / login / logout / chmod / chown
 *   CAP_ADMIN            blanket bypass (kernel pid 0 only)
 *
 * ---- sandbox profiles --------------------------------------------
 *
 * A sandbox profile is just a named (cap mask, path root) tuple. The
 * current set is:
 *
 *   unrestricted        all caps + ADMIN (pid 0)
 *   default             FILE_RW | EXEC | NET | GUI | TERM | SETTINGS_WRITE
 *   file-read-only      FILE_READ | GUI | TERM
 *   network-only        NET | FILE_READ
 *   restricted          FILE_READ | GUI   (root = /data/sandbox)
 *
 * The named set is intentionally tiny. Adding a new profile is a
 * one-line change in cap.c.
 */

#ifndef TOBYOS_CAP_H
#define TOBYOS_CAP_H

#include <tobyos/types.h>

#define CAP_FILE_READ         (1u << 0)
#define CAP_FILE_WRITE        (1u << 1)
#define CAP_EXEC              (1u << 2)
#define CAP_NET               (1u << 3)
#define CAP_GUI               (1u << 4)
#define CAP_TERM              (1u << 5)
#define CAP_SETTINGS_WRITE    (1u << 6)
#define CAP_ADMIN             (1u << 31)

/* Convenience groupings -- used to build profiles and to express
 * "give me every non-admin cap" (the default for user apps). */
#define CAP_GROUP_ALL_USER     (CAP_FILE_READ | CAP_FILE_WRITE | \
                                CAP_EXEC | CAP_NET | CAP_GUI | \
                                CAP_TERM | CAP_SETTINGS_WRITE)
#define CAP_GROUP_ALL          (CAP_GROUP_ALL_USER | CAP_ADMIN)

#define CAP_PROFILE_NAME_MAX   24
#define CAP_PATH_MAX           128   /* matches VFS_PATH_MAX headroom */

struct cap_profile {
    char      name[CAP_PROFILE_NAME_MAX];
    uint32_t  caps;
    char      root[CAP_PATH_MAX];     /* "" = no path jail */
};

struct proc;                          /* forward */

/* ---- queries ---------------------------------------------------- */

/* True iff `p` holds every bit in `need`. NULL `p` is treated as
 * "caller is the kernel" -- i.e. everything allowed. */
bool cap_has(const struct proc *p, uint32_t need);

/* Return true if `p` holds every bit in `need`. On denial, logs a
 * one-line reason through kprintf and returns false. NULL is
 * kernel-implicit and always passes. `what` is a short label used
 * only for logging ("vfs_open", "sys_socket", etc.). */
bool cap_check(const struct proc *p, uint32_t need, const char *what);

/* Path-sandbox check: if `p` has a non-empty sandbox_root, `path`
 * must begin with that prefix (and either end there or continue with
 * a '/'). Always returns true for NULL p or empty root. Does NOT
 * consult capability bits -- use in conjunction with cap_check. */
bool cap_path_allowed(const struct proc *p, const char *path);

/* Combined: path-sandbox check + capability check in one call. Logs
 * on denial. Returns true = allow, false = deny. `want_bits` is any
 * OR of CAP_FILE_READ / CAP_FILE_WRITE / CAP_EXEC. */
bool cap_check_path(const struct proc *p, const char *path,
                    uint32_t want_bits, const char *what);

/* Pretty-print `p`'s (name, caps, sandbox_root) to kprintf. Use from
 * shell builtins. Works with NULL p (prints "(kernel)"). */
void cap_dump_proc(const struct proc *p);

/* ---- profiles --------------------------------------------------- */

/* Look up a profile by name. Returns NULL if the name doesn't match
 * any known profile. Never modifies the returned struct. */
const struct cap_profile *cap_profile_lookup(const char *name);

/* Apply `profile_name` to `p`: intersect its caps with the profile's
 * mask (never widens) and copy the profile's sandbox root into p
 * (unless the profile's root is empty, in which case p keeps
 * whatever it already had -- so a stricter parent can't be loosened
 * by a laxer profile). Returns 0 on success, -1 if the name is
 * unknown (p is left untouched). */
int cap_profile_apply(struct proc *p, const char *profile_name);

/* Iterate every known profile, calling cb(user, profile). Used by
 * the `sandbox list` shell builtin. Stops early if cb returns
 * non-zero (that value is propagated back to the caller). */
typedef int (*cap_profile_iter_cb)(void *user, const struct cap_profile *prof);
int cap_profile_foreach(cap_profile_iter_cb cb, void *user);

/* For diagnostics: comma-separated cap list ("FILE_READ,GUI") into
 * `buf`. Always NUL-terminates. Returns number of bytes written
 * (excluding NUL), or 0 if caps == 0. */
size_t cap_mask_to_string(uint32_t caps, char *buf, size_t cap);

/* ---- M34D: declared capabilities ------------------------------- *
 *
 * Packages declare the capabilities their apps NEED via a CAPS line
 * in the .tpkg manifest (e.g. "CAPS FILE_READ,GUI"). The launcher
 * passes that string into proc_spawn via spec.declared_caps; after
 * the sandbox profile narrows the inherited caps, cap_apply_declared
 * narrows them further to (current & declared). This is what makes
 * "least-privilege by default" actually concrete on tobyOS:
 *
 *   inherited  : caps the parent had
 *   profile    : intersected with the SANDBOX profile mask
 *   declared   : intersected with the manifest's CAPS list
 *
 * It is purely a NARROWING operation -- declaring a cap a parent
 * doesn't have can never grant it. CAP_ADMIN is never grantable via
 * a declared list (and the list parser silently drops it for safety).
 *
 * Tokens are case-insensitive ("file_read" == "FILE_READ"), separated
 * by ',' or whitespace, and may have whitespace padding. Unknown
 * tokens are NOT a hard error -- they're counted in *out_unknown
 * (when non-NULL) and logged at apply time, so a typo never silently
 * widens the granted mask. An empty/whitespace-only csv yields mask 0
 * and 0 unknowns. */
int    cap_parse_list(const char *csv, uint32_t *out_mask,
                      int *out_unknown);

/* Apply M34D declared caps to `p`: p->caps &= cap_parse_list(csv).
 * NEVER widens; CAP_ADMIN is never affected. Returns 0 if csv parsed
 * cleanly, -1 if it was NULL or contained any unknown tokens (the
 * mask is still applied even on -1, with the unknown count logged).
 *
 * An empty csv ("" or NULL) is a no-op (returns 0). */
int    cap_apply_declared(struct proc *p, const char *csv);

/* Convenience used by the top-of-boot init paths. Writes ADMIN-level
 * caps into p->caps, clears the sandbox root. */
void cap_grant_admin(struct proc *p);

#endif /* TOBYOS_CAP_H */
