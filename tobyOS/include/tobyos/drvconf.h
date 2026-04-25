/* drvconf.h -- Milestone 35A: driver override configuration.
 *
 * Loads /etc/drvmatch.conf at boot (after the initrd is mounted) and
 * exposes the parsed rules to the rest of the kernel:
 *
 *   - blacklist <driver-name>
 *
 *       Ask pci_bind_drivers() to skip this driver. Implemented by
 *       calling drvmatch_disable_pci(), which also unbinds anything
 *       the driver currently owns and re-runs the bind pass so
 *       fallback drivers (or "no driver at all") get a fair shot.
 *
 *   - force <vid>:<did> <driver-name>
 *
 *       During bind, that specific PCI VID:DID is offered to the
 *       named driver FIRST. If its probe succeeds, the device is
 *       stamped EXACT with reason "forced via drvmatch.conf". If the
 *       named driver isn't registered, or its probe declines, the
 *       device falls back to the normal match loop -- a force rule
 *       can never make a device LESS bindable than it was without it.
 *
 * The parser is whitespace-tolerant, accepts '#' line comments, and
 * silently skips anything it doesn't understand (with a kprintf
 * warning). Missing config file is the common case and is fine; we
 * just leave the rule tables empty.
 *
 * The override surface is small on purpose. Operators get exactly
 * the two levers that real-world bring-up needs:
 *   "this in-tree driver is buggy on my box, leave it off"
 *   "two drivers match this device, pick this one"
 *
 * Anything richer (per-bus quirks, parameter strings, runtime
 * reload, ...) is intentionally OUT of scope for M35A and would
 * land in a later milestone if needed.
 */

#ifndef TOBYOS_DRVCONF_H
#define TOBYOS_DRVCONF_H

#include <tobyos/types.h>

#define DRVCONF_PATH              "/etc/drvmatch.conf"
#define DRVCONF_MAX_BLACKLIST     8
#define DRVCONF_MAX_FORCE         8
#define DRVCONF_NAME_MAX          16   /* mirrors ABI_DRVMATCH_DRIVER_MAX */

struct drvconf_force_rule {
    uint16_t vendor;
    uint16_t device;
    char     driver[DRVCONF_NAME_MAX];
};

/* Parse /etc/drvmatch.conf into the in-kernel rule tables. Idempotent
 * (calling twice just re-reads the file). Safe to call when the file
 * is missing -- silently leaves the tables empty.
 *
 * Returns the number of rules accepted (>=0) or -1 on parse error
 * with no rules registered. */
int drvconf_load(void);

/* Like drvconf_load but reads from an arbitrary path. Used by the
 * M35A selftest to drive a known-good fixture (/etc/drvmatch.conf.test)
 * without depending on what the production config happens to contain. */
int drvconf_load_path(const char *path);

/* Apply the blacklist + force rules to the live PCI driver registry.
 * Must be called AFTER drvconf_load() AND after pci_bind_drivers()
 * has run at least once -- this routine drives drvmatch_disable_pci
 * (which re-runs bind) and the per-device force probes. Idempotent:
 * second call is a no-op for already-applied rules. */
void drvconf_apply(void);

/* Predicate: is `driver_name` on the blacklist? */
bool drvconf_is_blacklisted(const char *driver_name);

/* Returns the forced driver name for (vendor, device), or NULL if
 * no force rule covers that ID. The returned string is owned by the
 * drvconf module and valid until the next drvconf_load. */
const char *drvconf_force_driver(uint16_t vendor, uint16_t device);

/* Counters for diagnostics + selftests. */
size_t drvconf_blacklist_count(void);
size_t drvconf_force_count(void);
const char              *drvconf_blacklist_at(size_t idx);
const struct drvconf_force_rule *drvconf_force_at(size_t idx);

/* Dump the parsed rules to the kernel console (used by the boot
 * banner so operators can see what overrides took effect). */
void drvconf_dump_kprintf(void);

#endif /* TOBYOS_DRVCONF_H */
