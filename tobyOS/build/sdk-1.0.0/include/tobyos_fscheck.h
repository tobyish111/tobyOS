/* tobyos_fscheck.h -- libtoby userland wrapper for the M28E
 * filesystem-integrity syscall (SYS_FS_CHECK).
 *
 * Same convention as the rest of libtoby: returns 0 on success, -1
 * with errno set on failure. The kernel ABI is the layout-frozen
 * struct abi_fscheck_report declared in tobyos/abi/abi.h.
 *
 * NOTE: a "non-OK" status (corruption found) is reported as -1 with
 * errno = EIO so a CI invocation can simply check the exit code. The
 * report buffer is always populated with the kernel's full verdict
 * (status flags, error counts, detail string) so callers can render
 * a useful diagnostic. */

#ifndef LIBTOBY_TOBYOS_FSCHECK_H
#define LIBTOBY_TOBYOS_FSCHECK_H

#include <stddef.h>
#include <stdint.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the mount at `mount_point` (e.g. "/data"). On clean and
 * warning-only verdicts the call returns 0 and *out is populated.
 * On corruption / I/O failure / unmounted path it returns -1 with
 * errno set; *out is still populated with whatever the kernel was
 * able to determine. */
int tobyfscheck(const char *mount_point, struct abi_fscheck_report *out);

/* Render the status flag bitmap to a static, single-line string for
 * `printf("%s", ...)` use. Never returns NULL. */
const char *tobyfscheck_status_str(uint32_t status);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_FSCHECK_H */
