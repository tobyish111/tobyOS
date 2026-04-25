/* bootdiag.h -- Milestone 29C: per-boot diagnostics recorder.
 *
 * The boot path peppers calls to bootdiag_stage(), bootdiag_driver(),
 * bootdiag_service(), and bootdiag_error() through the existing
 * subsystem inits. Each call appends to an in-kernel ring + bumps
 * counters in `g_diag` (an `abi_boot_diag`).
 *
 * After the per-boot harnesses settle the kernel calls
 * bootdiag_finalise() which:
 *
 *   - stamps `time_to_prompt_ms` from the PIT
 *   - copies the snapshot to `/data/etc/lastboot.report` (text) and
 *     `/data/last_boot.diag` (binary, ABI form) for post-mortem
 *   - emits the M29C_BD: sentinel block on serial so test scripts
 *     can grep for it
 *
 * Userland reads the live snapshot via SYS_BOOT_DIAG (sees the
 * fields as of the moment of the call) or the persisted twin via
 * /bin/bootreport.
 */

#ifndef TOBYOS_BOOTDIAG_H
#define TOBYOS_BOOTDIAG_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Boot-stage outcome codes. */
enum bootdiag_status {
    BOOTDIAG_OK   = 0,
    BOOTDIAG_WARN = 1,
    BOOTDIAG_FAIL = 2,
    BOOTDIAG_SKIP = 3,
};

/* Bring the recorder up. Called as early as slog_init runs. */
void bootdiag_init(void);

/* Has the recorder been initialised? */
bool bootdiag_ready(void);

/* Verbose flag (parsed from /etc/verbose_now during init). When true
 * the kernel lowers slog console-level filtering and emits BD_TRACE
 * lines for every recorded event. */
bool bootdiag_verbose(void);

/* Append a stage event. Safe to call before bootdiag_init -- pre-init
 * events are silently dropped. */
void bootdiag_stage(const char *name, enum bootdiag_status st);

/* Driver bind/probe outcome. `dev_id` is "VVVV:DDDD" or any short
 * locator; may be NULL. */
void bootdiag_driver(const char *driver, enum bootdiag_status st,
                     const char *dev_id);

/* Service start/stop outcome. */
void bootdiag_service(const char *service, enum bootdiag_status st);

/* Soft error/warning that doesn't fit the others. */
void bootdiag_error(const char *what);

/* Snapshot the current diag struct. Returns 0 on success or
 * -ABI_EFAULT for a NULL pointer. */
int  bootdiag_snapshot(struct abi_boot_diag *out);

/* Finalise: stamp time_to_prompt + persist text/binary reports.
 * Idempotent; only the first call writes. */
void bootdiag_finalise(void);

/* For shell + tests. */
void bootdiag_dump_kprintf(void);

#endif /* TOBYOS_BOOTDIAG_H */
