/* pkg.h -- minimal package manager (milestones 16 + 17).
 *
 * Goal: install, list, remove, and now (milestone 17) UPGRADE small
 * software bundles ("packages") onto tobyOS without rebuilding the
 * kernel. A package is a single file with the ".tpkg" extension
 * carrying metadata + one or more files to drop onto disk.
 *
 * ---- .tpkg file format ---------------------------------------------
 *
 * ASCII header, '\n'-terminated lines, followed by a raw binary body:
 *
 *   TPKG 1
 *   NAME <name>
 *   VERSION <version>
 *   DESC <one-line description>          (optional, 0..1 lines)
 *   PUBLISHER <publisher/source>         (optional, M34A)
 *   SANDBOX <profile-name>               (optional, milestone 18; defaults
 *                                          to "default" -- see <tobyos/cap.h>)
 *   CAPS <comma-sep cap names>           (optional, M34D; e.g. "FILE_READ,GUI")
 *   HASH <64-hex sha256-of-body>         (optional, M34A; verified on install)
 *   FHASH <abs-path> <64-hex sha256>     (optional, M34A; per-file integrity)
 *   SIG <key-id> <64-hex hmac-sha256>    (optional, M34C; verified if present)
 *   APP <label>|<exec_path>              (optional, 0..N lines)
 *   FILE <abs-dest-path> <size-bytes>    (0..N lines; declaration order)
 *   FILE <abs-dest-path> <size-bytes>
 *   BODY
 *   <file_0 bytes><file_1 bytes>...<file_N-1 bytes>
 *
 * ---- M34A integrity verification ------------------------------------
 *
 * If HASH is present, install/upgrade compute SHA-256 of the body
 * bytes (everything after BODY\n) and reject the package if it
 * doesn't match. If FHASH lines are present, the per-file payload is
 * also hashed and compared against its declared digest. A mismatch
 * (or a missing declared HASH at install time when policy is strict)
 * fails the install with a clear "[pkg] integrity failure: ..."
 * message; nothing lands on disk.
 *
 * Verification fails CLOSED: any malformed hash field, hash mismatch,
 * or oversize body short-circuits the whole install. There is no
 * "best effort" install for M34A.
 *
 * ---- M34C signature verification ------------------------------------
 *
 * If SIG is present, the named trust-store key is looked up in
 * /system/keys/trust.db. The signature covers the BODY bytes (same
 * domain as HASH). HMAC-SHA-256 is the only algorithm currently
 * accepted. Unknown key id, bad algorithm, or tag mismatch all reject
 * the package. The default policy treats UNSIGNED packages as
 * accepted (warn-only) so existing repos keep working; M34C tests
 * exercise both policies.
 *
 * - NAME + VERSION are required; everything else is optional.
 * - APP entries cause the installer to emit a "/data/apps/<name>-<idx>.app"
 *   descriptor (label=.., exec=..) AND register the app with the desktop
 *   launcher so it shows up in the Apps menu.
 * - FILE payloads are concatenated in declaration order, with no
 *   separators. The installer reads exactly <size-bytes> bytes per file.
 * - All FILE destinations MUST live under /data/ -- writing to the
 *   read-only ramfs root is rejected at install time.
 *
 * ---- on-disk layout -------------------------------------------------
 *
 *   /data/apps/           installed executables + .app descriptors
 *   /data/packages/       per-package install records (one file per pkg)
 *   /data/packages/<name>.bak    rollback bundle (milestone 17, optional)
 *   /data/repo/           writable local package repository
 *   /repo/                read-only shipped-in-initrd repository
 *
 * ---- install record format (/data/packages/<name>.pkg) -------------
 *
 *   NAME <name>
 *   VERSION <version>
 *   DESC <description>
 *   APP <label>|<exec_path>
 *   FILE <abs-dest-path>
 *   ...
 *
 * Identical to the tpkg header minus the size/body. Readable with
 * `cat` from the shell; used by `pkg remove` to tear an install down.
 *
 * ---- CLI -----------------------------------------------------------
 *
 * Exposed via the shell `pkg` builtin (see shell.c). Available subcmds:
 *
 *   pkg install <name>|<path>   extract + register; idempotent-ish.
 *   pkg remove  <name>          tear down an install.
 *   pkg list                    installed packages.
 *   pkg info    <name>          show one package's install record.
 *   pkg repo                    list packages visible in /data/repo
 *                               and /repo.
 *   pkg update                  show installed packages with newer
 *                               versions available in the repo
 *                               (milestone 17).
 *   pkg upgrade [name]          upgrade one (or all) installed
 *                               package(s) to the latest version
 *                               available; auto-rolls back on failure
 *                               (milestone 17).
 *   pkg rollback <name>         restore the previous version from the
 *                               .bak left by the most recent upgrade
 *                               (milestone 17).
 *
 * ---- versioning model (milestone 17) -------------------------------
 *
 * Versions are dotted-decimal strings up to 4 components, e.g.
 *   "1", "1.2", "1.2.3", "1.0.0.4"
 *
 * Comparison is component-wise; missing trailing components are
 * treated as 0, so "1.0" == "1.0.0" and "1.1" > "1.0.99". Anything
 * non-numeric in a component is treated as 0 (so the comparator can
 * never crash on a malformed VERSION string).
 *
 * ---- API -----------------------------------------------------------
 *
 * pkg_init() creates the on-disk directory skeleton (idempotent) and
 * (re)registers every installed app with the desktop launcher (by
 * scanning the .app descriptors under /data/apps). Called once during
 * boot after /data is mounted AND the GUI is ready. Safe to call more
 * than once.
 *
 * pkg_install_path() / pkg_install_name() / pkg_remove() / pkg_list()
 * are the underlying operations; the shell builtin is a thin wrapper.
 * All routines print diagnostics via kprintf and return 0 on success,
 * a negative int on failure.
 */

#ifndef TOBYOS_PKG_H
#define TOBYOS_PKG_H

#include <tobyos/types.h>

/* Path constants + limits. Kept centralised so the shell builtin can
 * advertise them in help text without guessing. */

#define PKG_APPS_DIR       "/data/apps"
#define PKG_DB_DIR         "/data/packages"
#define PKG_REPO_DATA_DIR  "/data/repo"
#define PKG_REPO_RAMFS_DIR "/repo"

#define PKG_NAME_MAX       32
#define PKG_VERSION_MAX    16
#define PKG_DESC_MAX       96
#define PKG_LABEL_MAX      32
#define PKG_PATH_MAX       128
#define PKG_SANDBOX_MAX    24   /* milestone 18: profile name length cap */
#define PKG_PUBLISHER_MAX  64   /* M34A: free-form publisher/source name */
#define PKG_HASH_HEX_LEN   64   /* M34A: SHA-256 in lowercase hex (no NUL) */
#define PKG_KEYID_MAX      32   /* M34C: trust-store key identifier      */
#define PKG_CAPS_LIST_MAX  128  /* M34D: comma-separated capability list */

/* Bring up the package manager. Creates /data/apps, /data/packages,
 * /data/repo if they don't exist, scans the .app descriptors under
 * /data/apps and registers each installed app with the launcher.
 * Idempotent. */
void pkg_init(void);

/* Install from an absolute VFS path to a .tpkg file. */
int  pkg_install_path(const char *tpkg_path);

/* Install by package name. Searches /data/repo/<name>.tpkg then
 * /repo/<name>.tpkg. */
int  pkg_install_name(const char *name);

/* Milestone 24D: install from an http://... URL. Downloads the .tpkg
 * body via http_get(), spools it to a scratch path under
 * /data/packages/.dl/<basename>, and then dispatches to
 * pkg_install_path(). Returns 0 on success, -1 on any failure. The
 * scratch file is removed on success and left in place on failure
 * (so the user can `pkg install` it manually for diagnostics). */
int  pkg_install_url(const char *url);

/* Remove an installed package by name. */
int  pkg_remove(const char *name);

/* Print the list of installed packages (one per line). */
void pkg_list(void);

/* Print the install record for a single package. */
int  pkg_info(const char *name);

/* Print every .tpkg visible in /data/repo and /repo. */
void pkg_repo_dump(void);

/* Re-scan /data/apps and re-register every .app with the launcher.
 * Called after install/remove. Cheap to call. */
void pkg_refresh_launcher(void);

/* ---- milestone 17: versioning + upgrade ------------------------- */

#define PKG_VERSION_PARTS 4

struct pkg_version {
    int parts[PKG_VERSION_PARTS];
    int count;                 /* how many parts were actually present */
};

/* Parse a dotted-decimal version string into `out`. Always succeeds:
 * malformed components yield 0. Returns the number of parts found. */
int pkg_version_parse(const char *s, struct pkg_version *out);

/* Compare two parsed versions. Trailing missing parts == 0.
 * Returns -1 if a<b, 0 if equal, +1 if a>b. */
int pkg_version_compare(const struct pkg_version *a,
                        const struct pkg_version *b);

/* Convenience: compare two version strings directly. */
int pkg_version_cmp_str(const char *a, const char *b);

/* Print a one-line-per-package "what would upgrade?" report. Walks
 * every installed package, looks up the highest-version .tpkg
 * available in /data/repo + /repo, and shows
 *   name   installed   ->   available   status
 * where status is one of "up-to-date", "UPGRADE", or "no repo entry". */
void pkg_update(void);

/* Upgrade a single installed package to the highest version available
 * in either repo. Returns:
 *    0  on success (including "already up-to-date" -- treated as no-op)
 *   -1  on any failure (after attempting auto-rollback). */
int  pkg_upgrade_one(const char *name);

/* Upgrade every installed package that has a newer version in the
 * repo. Prints a summary line and returns 0 if every attempted upgrade
 * succeeded (or there was nothing to do), -1 if any failed. */
int  pkg_upgrade_all(void);

/* M34B: upgrade an installed package using a SPECIFIC .tpkg path
 * instead of going through repo_find_latest. Useful for two cases:
 *
 *   - applying an upgrade dropped by a downloader / installer to a
 *     known scratch path (skipping the version-bump check)
 *   - the M34B self-test, which forces a known-bad v2 fixture into
 *     the upgrade pipeline to confirm the integrity gate rejects it
 *     and the live install survives untouched
 *
 * Same gates as pkg_upgrade_one: verify_integrity runs on the new
 * package; on failure the .bak is restored and the existing install
 * is preserved. Returns 0 on success, -1 on any failure (after
 * attempting auto-rollback). */
int  pkg_upgrade_path(const char *tpkg_path);

/* Restore <name> from /data/packages/<name>.bak. Returns 0 on success,
 * -1 if no backup exists or the restore fails. */
int  pkg_rollback(const char *name);

/* Boot-time self-test for milestone 17. Runs the full
 * install -> update -> upgrade -> rollback cycle against the
 * helloapp packages shipped in the initrd repo, printing every step
 * to serial so an external CI script can grep success markers out of
 * serial.log without needing to drive the kernel shell over QMP.
 *
 * Only compiled in when PKG_M17_SELFTEST is defined at build time;
 * otherwise this is a no-op stub. Invoked from kmain after pkg_init().
 *
 * Idempotent: it removes the helloapp install at the end so a normal
 * boot afterwards starts in the same state. */
void pkg_m17_selftest(void);

/* ---- milestone 34: package security ---------------------------- */

/* Signature verification policy. Default = WARN: unsigned packages
 * install with a warning, signed-but-bad fails closed. STRICT
 * (sig-required) refuses unsigned packages outright. The setting
 * lives on the package manager and can be flipped by the boot
 * harness or future userland tooling. */
enum pkg_sig_policy {
    PKG_SIG_POLICY_WARN     = 0,   /* unsigned -> install with warn */
    PKG_SIG_POLICY_REQUIRED = 1,   /* unsigned -> reject            */
};

void pkg_set_sig_policy(int policy);
int  pkg_get_sig_policy(void);

/* Read the installed VERSION of `name` into `out_buf` (size = cap).
 * Returns 0 and writes a NUL-terminated string on success; -1 if the
 * package is not installed or the install record is unreadable. Used
 * by the M34G securitytest harness to assert version progression
 * across upgrade + rollback. */
int pkg_get_installed_version(const char *name, char *out_buf, size_t cap);

/* Boot-time self-test that exercises the entire M34 security pipe:
 * package integrity (M34A), update verification (M34B), signing
 * (M34C), capability declarations (M34D), and protected-path writes
 * (M34E). Each sub-test prints "[m34a-selftest] ... PASS|FAIL" so an
 * external script can grep verdicts. Only compiled when
 * PKG_M34_SELFTEST is defined; otherwise a no-op stub. */
void pkg_m34_selftest(void);

#endif /* TOBYOS_PKG_H */
