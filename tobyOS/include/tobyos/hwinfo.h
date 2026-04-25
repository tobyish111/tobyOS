/* hwinfo.h -- Milestone 29A: hardware discovery & inventory.
 *
 * Why this exists
 * ---------------
 * M29 is the "make it boot on real hardware" milestone. Before any of
 * the harder problems (driver matching, fallback paths, hwreport,
 * regression baseline) we need ONE blessed source of truth for "what
 * hardware did this kernel see?". hwinfo is that source:
 *
 *   - It calls every existing introspection API (CPUID, pmm_total_*,
 *     pci_device_count, devtest_enumerate, ...) and packs the answers
 *     into a single struct abi_hwinfo_summary.
 *
 *   - It exposes that struct to userland via SYS_HWINFO so the new
 *     /bin/hwinfo tool, the M29D installer report, the M29E hwtest
 *     profiles, and the M29F regression guard all agree on the
 *     numbers.
 *
 *   - It writes a textual snapshot to /data/hwinfo.snap on every
 *     boot (best-effort) so post-mortem debugging on a real machine
 *     can read the inventory back even after a crash.
 *
 * No allocations: all snapshots live in static buffers. The CPU
 * brand strings are extracted via CPUID once and cached.
 */

#ifndef TOBYOS_HWINFO_H
#define TOBYOS_HWINFO_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* One-shot init. Reads CPUID, caches the brand strings + feature
 * flags, latches the boot timestamp. Idempotent (subsequent calls
 * are no-ops). Safe to call BEFORE pci/devtest are populated -- the
 * device counts will simply be zero until the next snapshot. */
void hwinfo_init(void);

/* Cheap accessor: refresh the per-bus counters from devtest_enumerate
 * + pmm_*, fold in the cached CPU info, and copy the result into
 * `out`. Bumps the snapshot_epoch counter so userland can detect
 * "has this changed since I last looked" without diffing every
 * field. */
void hwinfo_snapshot(struct abi_hwinfo_summary *out);

/* The static cached copy from the most recent hwinfo_snapshot()
 * call (or hwinfo_init for the very first one). Lifetime: kernel
 * boot. Useful for callers that want a stable pointer (e.g. the
 * persistence path that re-emits the snapshot after every boot). */
const struct abi_hwinfo_summary *hwinfo_current(void);

/* Snapshot a fresh inventory and write a textual rendering to
 * /data/hwinfo.snap. Returns the number of bytes written, 0 if
 * /data was unavailable (silently skipped), or a negative VFS error
 * code on disk failure. Logs to slog regardless. */
long hwinfo_persist(void);

/* Pretty-print the current snapshot to the kernel console using
 * kprintf. Used by the M29A boot-time harness and the `hwinfo`
 * shell builtin. */
void hwinfo_dump_kprintf(void);

/* Format the cached summary into a caller-supplied text buffer
 * exactly as it would appear on disk in /data/hwinfo.snap.
 * Returns the number of bytes written (always NUL-terminated; if
 * the buffer is too small the line gets truncated cleanly).
 *
 * Used by both hwinfo_persist (which then writes to disk) and the
 * hwinfo userland tool's --snapshot mode (which reads the live
 * inventory through SYS_HWINFO and renders identically). */
size_t hwinfo_format_text(char *buf, size_t cap,
                          const struct abi_hwinfo_summary *snap);

/* Best-effort heuristic: pick "vm", "desktop", or "laptop" based on
 * the cached snapshot. Used by hwinfo_init to populate
 * profile_hint and by M29E's hwtest tool to default the profile.
 * Returns a stable string pointer (never NULL). */
const char *hwinfo_profile_hint(void);

#endif /* TOBYOS_HWINFO_H */
