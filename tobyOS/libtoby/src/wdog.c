/* libtoby/src/wdog.c -- Milestone 28C userland wrapper for the
 * watchdog status syscall. Mirrors libtoby/src/slog.c's shape. */

#include <tobyos_wdog.h>
#include <errno.h>

#include "libtoby_internal.h"

int tobywdog_status(struct abi_wdog_status *out) {
    if (!out) { errno = EFAULT; return -1; }
    long rv = toby_sc1(ABI_SYS_WDOG_STATUS,
                       (long)(uintptr_t)out);
    return (int)__toby_check(rv);
}

const char *tobywdog_kind_str(uint32_t kind) {
    switch (kind) {
    case ABI_WDOG_KIND_SCHED_STALL: return "sched_stall";
    case ABI_WDOG_KIND_KERNEL_HANG: return "kernel_hang";
    case ABI_WDOG_KIND_USER_HANG:   return "user_hang";
    case ABI_WDOG_KIND_MANUAL:      return "manual";
    case ABI_WDOG_KIND_NONE:        return "none";
    default:                        return "?";
    }
}
