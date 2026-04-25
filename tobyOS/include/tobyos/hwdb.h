/* hwdb.h -- Milestone 35D: hardware compatibility database.
 *
 * The hwdb module is the runtime "what works on this machine" view.
 * It joins three already-populated tables:
 *
 *   - PCI inventory  (pci_device_count + pci_device_at)
 *   - USB inventory  (usbreg_count    + usbreg_get      -- M35C)
 *   - Static drvdb   (drvdb_pci_lookup + drvdb_usb_lookup -- M35A)
 *
 * ... with the live drvmatch outcome (which driver actually bound to
 * each device this boot) and emits a flat list of struct
 * abi_hwcompat_entry rows. Tier resolution rules:
 *
 *     drvdb=SUPPORTED   bound=1 -> SUPPORTED
 *     drvdb=SUPPORTED   bound=0 -> PARTIAL    ("driver disabled")
 *     drvdb=PARTIAL     *       -> PARTIAL
 *     drvdb=UNSUPPORTED *       -> UNSUPPORTED
 *     drvdb=UNKNOWN     bound=1 -> PARTIAL    ("generic driver bound")
 *     drvdb=UNKNOWN     bound=0 -> UNSUPPORTED
 *
 * The DB is read-only at the API level -- there's no add/remove path.
 * Whenever the underlying tables change (hot-plug attach/detach, a
 * deferred PCI rescan, etc.) the next snapshot just reflects the new
 * state. That keeps lock contention to zero and lets the syscall
 * implementation be a single pass with no allocation.
 *
 * Used by:
 *   - sys_hwcompat_list()   -- userland tool `hwcompat`
 *   - m35d_selftest()       -- per-phase regression harness
 *   - shell `hwcompat` cmd  -- (M35F follow-up; not wired here)
 */

#ifndef TOBYOS_HWDB_H
#define TOBYOS_HWDB_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Snapshot up-to-`cap` rows into the caller's buffer. Returns the
 * number of rows actually written. A return value equal to `cap`
 * means the snapshot may have been truncated; the kernel does not
 * reserve the last slot for a sentinel. Returns 0 if either pointer
 * is NULL or cap is 0; the call cannot fail otherwise. */
size_t hwdb_snapshot(struct abi_hwcompat_entry *out, size_t cap);

/* Aggregate counters for the most recent snapshot (or a fresh walk
 * if no snapshot has been taken yet). Both pointers are optional --
 * pass NULL for the totals you don't care about. Returns the total
 * row count.
 *
 * The kernel does not cache the snapshot between calls; this just
 * loops the same join the syscall does. Cheap (O(N) over the PCI/
 * USB tables with no allocation), so the cost is dominated by the
 * caller's I/O. */
size_t hwdb_counts(size_t *out_supported,
                   size_t *out_partial,
                   size_t *out_unsupported);

/* Stable string for an ABI_HWCOMPAT_* status. Returns "?" for unknown
 * values; never returns NULL. Mirrors drvdb_tier_name() but uses the
 * userland-facing names ("supported"/"partial"/"unsupported"). */
const char *hwdb_status_name(uint32_t status);

/* Diagnostic dump straight to kprintf. Used by the m35d selftest and
 * by the shell `hwcompat` builtin. Walks the same join, so it stays
 * in sync with hwdb_snapshot() output. */
void hwdb_dump_kprintf(void);

#endif /* TOBYOS_HWDB_H */
