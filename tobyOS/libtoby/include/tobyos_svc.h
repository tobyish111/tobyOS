/* tobyos_svc.h -- libtoby userland wrapper for the M28F service
 * supervision syscall (SYS_SVC_LIST).
 *
 * Same convention as the rest of libtoby: returns 0 on success, -1
 * with errno set on failure. The kernel ABI is the layout-frozen
 * struct abi_service_info declared in tobyos/abi/abi.h.
 *
 * Used by `services` to render the live registry, and by the
 * stabilitytest harness in M28G. */

#ifndef LIBTOBY_TOBYOS_SVC_H
#define LIBTOBY_TOBYOS_SVC_H

#include <stddef.h>
#include <stdint.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot the registry into `out` (capacity `cap` records). On
 * success returns the number of records written (>=0); on failure
 * returns -1 and sets errno (EFAULT for a bad buffer, EINVAL for
 * cap == 0 or > 64). */
int tobysvc_list(struct abi_service_info *out, uint32_t cap);

/* Lookup helpers -- pure functions over the ABI integer fields. */
const char *tobysvc_state_str(uint32_t state);
const char *tobysvc_kind_str(uint32_t kind);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_SVC_H */
