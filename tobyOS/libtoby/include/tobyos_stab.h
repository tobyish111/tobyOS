/* tobyos_stab.h -- libtoby userland wrapper for the M28G stability
 * self-test syscall (SYS_STAB_SELFTEST).
 *
 * The kernel runs a fixed list of subsystem probes (boot, log, panic,
 * watchdog, filesystem, services, gui, terminal, network, input,
 * safe_mode, display) and reports a result_mask of OK bits plus a
 * short human-readable detail string.
 *
 * Convention: returns 0 on success (= every requested probe passed),
 * a positive number = how many probes failed, or -1 with errno set on
 * a transport-level failure (bad pointer, etc).
 *
 * Used by `stabilitytest` and the M28G boot harness in kernel.c. */

#ifndef LIBTOBY_TOBYOS_STAB_H
#define LIBTOBY_TOBYOS_STAB_H

#include <stddef.h>
#include <stdint.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run the kernel-side self-test. `mask` is an OR of ABI_STAB_OK_*
 * bits (or 0 = ABI_STAB_OK_ALL). On success `out` is filled in.
 * Returns:
 *   0  -- every requested probe passed
 *   N  -- N probes failed (out->fail_count)
 *  -1  -- syscall failed (errno set; usually EFAULT) */
int tobystab_run(struct abi_stab_report *out, uint32_t mask);

/* Pure helper: lookup the human-readable name for a single bit of
 * ABI_STAB_OK_*. Returns "?" for unknown bits. */
const char *tobystab_bit_name(uint32_t bit);

/* Pure helper: render the result_mask as a comma-separated list of
 * probe names into `dst`. Always NUL-terminates. */
void tobystab_format_mask(char *dst, size_t cap, uint32_t mask);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_STAB_H */
