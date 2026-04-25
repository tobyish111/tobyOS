/* devtest.h -- M26A peripheral test harness (kernel side).
 *
 * Why this exists
 * ---------------
 * Milestone 26 adds a lot of new device support (USB hubs, hot-plug,
 * HID polish, mass-storage robustness, HD audio, ACPI battery). Each
 * sub-phase needs to be measurable WITHOUT scrolling the boot log
 * looking for kprintf lines and squinting. Devtest is the uniform
 * answer:
 *
 *   - It defines ONE record type (struct devtest_dev_info) that
 *     every introspectable subsystem fills in. Userland sees the
 *     identical layout via abi_dev_info in <tobyos/abi/abi.h>.
 *
 *   - It hosts a tiny driver self-test registry: subsystems call
 *     devtest_register("xhci", ...) once, and `drvtest` (userland)
 *     can call it back via SYS_DEV_TEST.
 *
 *   - It exposes an event ring for hot-plug attach/detach. Empty
 *     until M26C, but the API exists from M26A so callers don't get
 *     "function not implemented yet".
 *
 *   - It runs a boot-time inventory + self-test sweep that prints a
 *     structured `[boot] M26A: ...` block. The same data flows out
 *     to userland via the two new syscalls so PASS/FAIL results from
 *     CI scripts and from interactive `devlist` / `drvtest` are
 *     literally the same numbers.
 *
 * No hard dependency on the rest of M26: the audio / battery / hub
 * paths are introspected through optional accessor functions in
 * src/audio_hda.c and src/acpi_bat.c. Both files exist as stubs
 * starting in M26A and graduate to real drivers in M26F / M26G. */

#ifndef TOBYOS_DEVTEST_H
#define TOBYOS_DEVTEST_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Internal kernel-side struct mirrors abi_dev_info exactly. We just
 * use abi_dev_info directly so there is zero copy across the syscall
 * boundary -- the user-side struct IS the kernel-side struct. */
typedef struct abi_dev_info devtest_dev_info_t;

/* Event-ring entry: one slot per attach/detach. type_mask is the
 * ABI_DEVT_BUS_* tag of the affected device; `info` is a snapshot
 * of the device record at the moment of the event so userland can
 * react after the device has already been freed. */
struct devtest_event {
    uint64_t            seq;          /* monotonic, starts at 1 */
    uint64_t            timestamp_ms; /* perf_now_ns() / 1e6 */
    uint8_t             kind;         /* DEVT_EV_* below */
    uint8_t             _pad[7];
    devtest_dev_info_t  info;
};

#define DEVT_EV_NONE     0
#define DEVT_EV_ATTACH   1
#define DEVT_EV_DETACH   2

/* One-time init. Called from kernel.c after pci_bind_drivers and
 * partition_scan_all so every subsystem has had a chance to register. */
void devtest_init(void);

/* Register a self-test by short ASCII name (e.g. "xhci", "blk").
 * Names longer than 15 chars are truncated. Re-registering replaces.
 *
 * The callback writes a NUL-terminated diagnostic into `msg` (capped
 * at ABI_DEVT_MSG_MAX) and returns:
 *    0                = PASS
 *   ABI_DEVT_SKIP (1) = SKIP (hardware not present)
 *   negative          = FAIL (and msg explains why) */
typedef int (*devtest_fn)(char *msg, size_t cap);

void devtest_register(const char *name, devtest_fn fn);

/* Look up + run a registered test. Returns the same code as the fn.
 * If `name` doesn't match anything, returns -ABI_ENOENT and writes
 * "no such test: <name>" into msg. */
int  devtest_run(const char *name, char *msg, size_t cap);

/* Walk every registered test, calling cb() with each (name, rc, msg).
 * Returns the number of tests run. Used by drvtest. */
typedef void (*devtest_walk_cb)(const char *name, int rc,
                                const char *msg, void *cookie);
int  devtest_for_each(devtest_walk_cb cb, void *cookie);

/* Enumerate currently-known devices into a flat array. `type_mask`
 * is the OR of ABI_DEVT_BUS_* bits to filter (0 means all). Returns
 * the number of entries written, capped to `cap`. Stable order:
 *   PCI -> USB -> BLK -> INPUT -> AUDIO -> BATTERY -> HUB
 * within each bus: sub-keyed by per-bus index. */
int  devtest_enumerate(devtest_dev_info_t *out, int cap, uint32_t type_mask);

/* Hot-plug event ring (M26C will fill it). Until then attach() and
 * post() are still callable so M26A drivers can register fake events
 * during self-tests. */
void devtest_event_post(uint8_t kind, const devtest_dev_info_t *info);
int  devtest_event_drain(struct devtest_event *out, int cap);

/* Boot-time inventory + self-test sweep. Logs to serial via kprintf
 * in the same `[PASS]/[FAIL]/[INFO]` format the userland tools use,
 * so a single grep finds both layers. Safe to call multiple times. */
void devtest_boot_run(void);

/* Kernel-callable formatter used by both `devtest_boot_run()` and
 * the shell builtin `devlist`. Re-runs enumerate + dumps the table
 * via kprintf using the canonical PASS/INFO format. */
void devtest_dump_kprintf(uint32_t type_mask);

#endif /* TOBYOS_DEVTEST_H */
