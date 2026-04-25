/* tobyos_devtest.h -- libtoby wrappers for the M26A peripheral
 * test harness syscalls (SYS_DEV_LIST, SYS_DEV_TEST).
 *
 * These wrappers convert the kernel ABI (negative ABI_E* on failure,
 * non-negative on success / SKIP) into a POSIX-shape "errno + -1 on
 * fail" interface that user programs are used to.
 *
 * The struct itself (`struct abi_dev_info`) is the kernel-frozen
 * record from <tobyos/abi/abi.h>. Userland walks an array of these.
 *
 * Why this lives here and not in the kernel headers: programs want
 * <stdio.h>+<errno.h> shape, not the freestanding kernel slice. */

#ifndef LIBTOBY_TOBYOS_DEVTEST_H
#define LIBTOBY_TOBYOS_DEVTEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>     /* for FILE * */
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* List devices into `out`. `cap` is the capacity in records. `mask`
 * is the OR of ABI_DEVT_BUS_* bits to filter, or 0 for "all".
 *
 * Returns the number of records written (>= 0) on success. On
 * failure returns -1 and sets errno to one of:
 *   EFAULT  -- bad pointer
 *   EINVAL  -- cap == 0
 * (Other errors map directly through __toby_check.) */
int tobydev_list(struct abi_dev_info *out, size_t cap, uint32_t mask);

/* Run a registered driver self-test by name. Fills `msg_out` with a
 * one-line diagnostic (NUL-terminated, capped at ABI_DEVT_MSG_MAX-1).
 *
 * Return values mirror the kernel:
 *    0  PASS
 *   +1  SKIP (hardware not present)  -- == ABI_DEVT_SKIP
 *   -1  FAIL (errno set, msg_out has a reason)
 *
 * Note: errno on FAIL reflects whatever the kernel returned (e.g.
 * EIO for a register sanity-check failure, ENOENT for "no such
 * test"). */
int tobydev_test(const char *name, char *msg_out, size_t msg_cap);

/* Bus-tag -> short string. Useful for tabular output. Never NULL. */
const char *tobydev_bus_str(uint8_t bus);

/* Pretty-print a single record to FILE *fp using a stable column
 * layout. Header is printed by tobydev_print_header(). Implemented
 * in libtoby's devtest.c so userland tools share one renderer. */
void tobydev_print_header(FILE *fp);
void tobydev_print_record(FILE *fp, const struct abi_dev_info *r);

/* M26C: drain at most `cap` hot-plug events into `out`. Returns:
 *    >= 0  number of events copied
 *    -1    failure, errno set
 *
 * The kernel ring is lossy on overflow. The first returned event's
 * ev->dropped field carries the count of events that were silently
 * discarded since the last drain (one big bump rather than per-event
 * bookkeeping). cap may be 0 to "peek" without consuming, in which
 * case the call returns 0 with no events copied. */
int tobydev_hot_drain(struct abi_hot_event *out, int cap);

/* Pretty-print a single hot-plug event to fp. Format:
 *   <time_ms>ms seq=<n> [+a/-d/!err] bus=<b> slot=<s> depth=<d> port=<p> info="..."
 */
void tobydev_print_hot_event(FILE *fp, const struct abi_hot_event *ev);

/* ============================================================
 *  M27A: display introspection
 * ============================================================ */

/* Enumerate display outputs into `out`. Returns:
 *   >= 0  number of records written (capped to `cap`)
 *    -1   failure, errno set
 *
 * Pass cap == 0 to "peek" the wire shape (always returns 0). */
int tobydisp_list(struct abi_display_info *out, size_t cap);

/* Convert a backend id (ABI_DISPLAY_BACKEND_*) to a short name
 * suitable for a column. Never returns NULL. */
const char *tobydisp_backend_str(uint8_t backend_id);

/* Convert a pixel format (ABI_DISPLAY_FMT_*) to a short string. */
const char *tobydisp_format_str(uint8_t fmt);

/* Pretty-print a single display record to fp. Stable column shape so
 * `displayinfo` and `drvtest` print identically when they touch the
 * same record. */
void tobydisp_print_header(FILE *fp);
void tobydisp_print_record(FILE *fp, const struct abi_display_info *r);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_DEVTEST_H */
