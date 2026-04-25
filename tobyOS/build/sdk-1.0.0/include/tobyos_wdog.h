/* tobyos_wdog.h -- libtoby userland wrapper for the M28C watchdog
 * status syscall (SYS_WDOG_STATUS).
 *
 * Same convention as the rest of libtoby: returns 0 on success, -1
 * with errno set on failure. The kernel ABI is the layout-frozen
 * struct abi_wdog_status declared in tobyos/abi/abi.h. */

#ifndef LIBTOBY_TOBYOS_WDOG_H
#define LIBTOBY_TOBYOS_WDOG_H

#include <stddef.h>
#include <stdint.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot the kernel's watchdog state into `out`. */
int tobywdog_status(struct abi_wdog_status *out);

/* Translate a watchdog event-kind code to a printable name (e.g.
 * "sched_stall"). Never returns NULL; unknown kinds get "?". */
const char *tobywdog_kind_str(uint32_t kind);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_WDOG_H */
