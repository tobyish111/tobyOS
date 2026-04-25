/* drvmatch.h -- Milestone 29B: driver matching + fallback registry.
 *
 * Thin, query-only layer on top of the existing PCI/USB driver
 * registry. The kernel already binds drivers in pci_bind_drivers()
 * and the USB class drivers attach themselves during xHCI
 * enumeration -- this module just records WHY each binding happened
 * (exact/class/generic/unsupported/forced-off) and exposes that
 * information to userland through SYS_DRVMATCH.
 *
 * It also exposes a small test-only hook (drvmatch_disable_pci) that
 * marks a registered driver as off-limits, unbinds any device
 * currently owned by that driver, and re-runs pci_bind_drivers() so
 * the test suite can prove the fallback path is reachable.
 */
#ifndef TOBYOS_DRVMATCH_H
#define TOBYOS_DRVMATCH_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Boot-time setup. Call once after pci_bind_drivers() and after the
 * xHCI/USB class drivers have had a chance to attach. Walks every
 * device, fills in any drvmatch_record fields we keep on top of the
 * pci_dev / usb_device structs, and prints a one-line summary. */
void drvmatch_init(void);

/* Resolve a (bus, vendor, device) lookup into an abi_drvmatch_info
 * record. `bus` is one of ABI_DEVT_BUS_PCI or ABI_DEVT_BUS_USB.
 * Returns 0 on success, -ABI_ENOENT if no matching device exists,
 * -ABI_EINVAL on a bad bus tag. */
long drvmatch_query(uint32_t bus, uint32_t vendor, uint32_t device,
                    struct abi_drvmatch_info *out);

/* Stat counters. Total = devices considered; bound = devices with a
 * driver owning them; unbound = devices not bound to any driver;
 * forced = devices whose driver was disabled by drvmatch_disable_pci.
 * Any of the out pointers may be NULL. */
void drvmatch_count(uint32_t *out_total,
                    uint32_t *out_bound,
                    uint32_t *out_unbound,
                    uint32_t *out_forced);

/* Boot-time + post-mortem helper: dump the live drvmatch table to
 * the kernel console. Used by the M29B harness in kernel.c. */
void drvmatch_dump_kprintf(void);

/* Test-only: disable the named PCI driver. We:
 *   1. Find the driver in the registry; if missing, return -ABI_ENOENT.
 *   2. For every device currently bound to it, call drv->remove()
 *      (if non-NULL), clear dev->driver, and stamp dev->match_strategy
 *      = ABI_DRVMATCH_FORCED_OFF.
 *   3. Set drv->_disabled = 1 so subsequent rebind passes skip it.
 *   4. Re-run pci_bind_drivers() so any fallback driver still in the
 *      registry gets a chance to claim the device.
 *
 * Return value is the number of devices that were unbound (>=0) or a
 * negative ABI error code. The caller is responsible for re-enabling
 * the driver via drvmatch_reenable_pci() once the test is over. */
long drvmatch_disable_pci(const char *driver_name);

/* Test-only counterpart to drvmatch_disable_pci. Clears _disabled on
 * the named driver and re-runs the bind loop so the original driver
 * can pick up the previously-stranded device(s). Returns the number
 * of devices it (re-)bound, or a negative ABI error. */
long drvmatch_reenable_pci(const char *driver_name);

#endif /* TOBYOS_DRVMATCH_H */
