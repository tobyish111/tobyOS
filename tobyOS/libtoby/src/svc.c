/* libtoby/src/svc.c -- Milestone 28F userland wrapper for the
 * service supervision query syscall. Mirrors libtoby/src/wdog.c. */

#include <tobyos_svc.h>
#include <errno.h>

#include "libtoby_internal.h"

int tobysvc_list(struct abi_service_info *out, uint32_t cap) {
    if (!out)        { errno = EFAULT; return -1; }
    if (cap == 0)    { errno = EINVAL; return -1; }
    long rv = toby_sc2(ABI_SYS_SVC_LIST,
                       (long)(uintptr_t)out,
                       (long)cap);
    return (int)__toby_check(rv);
}

const char *tobysvc_state_str(uint32_t state) {
    switch (state) {
    case ABI_SVC_STATE_STOPPED:  return "stopped";
    case ABI_SVC_STATE_RUNNING:  return "running";
    case ABI_SVC_STATE_FAILED:   return "failed";
    case ABI_SVC_STATE_BACKOFF:  return "backoff";
    case ABI_SVC_STATE_DISABLED: return "DISABLED";
    default:                     return "?";
    }
}

const char *tobysvc_kind_str(uint32_t kind) {
    switch (kind) {
    case ABI_SVC_KIND_BUILTIN: return "builtin";
    case ABI_SVC_KIND_PROGRAM: return "program";
    default:                   return "?";
    }
}
