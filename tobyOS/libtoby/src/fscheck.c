/* libtoby/src/fscheck.c -- Milestone 28E userland wrapper for the
 * filesystem-check syscall. Mirrors libtoby/src/wdog.c's shape. */

#include <tobyos_fscheck.h>
#include <errno.h>

#include "libtoby_internal.h"

int tobyfscheck(const char *mount_point, struct abi_fscheck_report *out) {
    if (!mount_point || !out) { errno = EFAULT; return -1; }
    long rv = toby_sc2(ABI_SYS_FS_CHECK,
                       (long)(uintptr_t)mount_point,
                       (long)(uintptr_t)out);
    return (int)__toby_check(rv);
}

const char *tobyfscheck_status_str(uint32_t status) {
    /* The status field is a bitmap. Pick the most useful single label
     * for the common combinations; callers wanting full detail can
     * test individual ABI_FSCHECK_* bits themselves. */
    if (status & ABI_FSCHECK_UNMOUNTED) return "UNMOUNTED";
    if (status & ABI_FSCHECK_CORRUPT)   return "CORRUPT";
    if (status & ABI_FSCHECK_REPAIRED)  return "WARN";
    if (status & ABI_FSCHECK_OK)        return "OK";
    return "?";
}
