/* tobyos_hwinfo.h -- libtoby wrapper for the M29A SYS_HWINFO syscall.
 *
 * The wrapper is intentionally tiny: it just shells out to the
 * kernel for the canonical struct, then offers a small set of
 * formatting helpers so userland tools (hwinfo, hwreport, hwtest,
 * bringuptest) print the same columns. The struct itself is the
 * kernel-frozen `abi_hwinfo_summary` from <tobyos/abi/abi.h>; we
 * never copy it.
 *
 * Failure path follows the rest of libtoby: returns -1 + errno set
 * (EFAULT, EINVAL); 0 == success.
 */

#ifndef LIBTOBY_TOBYOS_HWINFO_H
#define LIBTOBY_TOBYOS_HWINFO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Refresh + fetch the kernel's hardware summary. Returns 0 on
 * success and writes into *out; returns -1 + errno on failure
 * (EFAULT for a NULL/bad pointer). */
int tobyhw_summary(struct abi_hwinfo_summary *out);

/* Decode the cpu_features bitmap into a space-separated list of
 * lowercase names ("fpu tsc msr sse sse2 lm nx ..."). Always
 * NUL-terminates `dst` if cap > 0. Returns the number of bytes
 * written excluding the NUL. */
size_t tobyhw_format_features(uint32_t feat, char *dst, size_t cap);

/* Single-line CPU summary for compact tools (e.g. `hwtest vm`).
 * Format: "<vendor> <brand> family=N model=N step=N cpus=N feat=...".
 * Truncates cleanly into `dst[cap]`. */
size_t tobyhw_format_cpu_line(const struct abi_hwinfo_summary *s,
                              char *dst, size_t cap);

/* Pretty-print the entire summary to fp using the SAME layout the
 * kernel writes to /data/hwinfo.snap, so a quick `cat` of the file
 * matches `hwinfo` output verbatim. */
void tobyhw_print_summary(FILE *fp,
                          const struct abi_hwinfo_summary *s);

/* Best-effort heuristic mirror of the kernel's profile pick. The
 * kernel's profile_hint field is the canonical answer; this helper
 * is just for tools that want a fallback if the snapshot is stale. */
const char *tobyhw_profile_str(const struct abi_hwinfo_summary *s);

/* M29B: SYS_DRVMATCH wrapper. Look up (bus, vendor, device) -- bus
 * must be ABI_DEVT_BUS_PCI or ABI_DEVT_BUS_USB -- and fill `out`
 * with the corresponding match record. Returns 0 if the device was
 * found and bound to a known driver; -1 + errno otherwise. errno
 * == ENOENT means the device is not present in the inventory; the
 * record is still populated with strategy=NONE so callers can render
 * it cleanly. */
int tobyhw_drvmatch(uint32_t bus, uint32_t vendor, uint32_t device,
                    struct abi_drvmatch_info *out);

/* Stable string for an ABI_DRVMATCH_* strategy code. Returns "?" for
 * unknown values; never returns NULL. */
const char *tobyhw_strategy_str(uint32_t strategy);

/* M35D: SYS_HWCOMPAT_LIST wrapper. Snapshot up to `cap` rows from the
 * kernel's hardware-compatibility database into `out`. Returns the
 * number of rows actually written (>= 0); -1 on a kernel error
 * (errno set: EFAULT for a NULL/bad pointer, EINVAL for non-zero
 * `flags`). `flags` MUST be 0 in this revision; the parameter exists
 * so a future ABI bump can add filters without breaking callers. */
int tobyhw_compat_list(struct abi_hwcompat_entry *out,
                       unsigned int cap,
                       unsigned int flags);

/* Stable string for an ABI_HWCOMPAT_* status. Returns "?" for unknown
 * values; never returns NULL. Mirrors the kernel's hwdb_status_name. */
const char *tobyhw_compat_status_str(uint32_t status);

/* Stable string for an ABI_DEVT_BUS_* tag, kept lowercase. Returns
 * "?" for values outside the documented set. */
const char *tobyhw_compat_bus_str(uint32_t bus);

/* M35F: stable lowercase string for an ABI_BOOT_MODE_* code. The
 * `safe_mode` field on `abi_hwinfo_summary` carries the raw enum
 * since M35F (kept ABI-compatible because old callers only check
 * `!= 0` for "any safe mode"). Returns "?" for unknown values; never
 * returns NULL. */
const char *tobyhw_boot_mode_str(uint32_t mode);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_HWINFO_H */
