/* drvmatch.c -- Milestone 29B: driver matching + fallback registry.
 *
 * Stays as thin as possible: it never owns devices, it only reads
 * the existing PCI/USB tables, classifies HOW each binding happened,
 * and exposes that picture through SYS_DRVMATCH.
 *
 * The classification rules:
 *
 *   pci_dev->driver == NULL                       -> ABI_DRVMATCH_NONE
 *                                                    (reason: "no matching driver")
 *   pci_dev->match_strategy == FORCED_OFF         -> ABI_DRVMATCH_FORCED_OFF
 *                                                    (reason: "disabled by drvmatch_disable_pci")
 *   pci_dev->driver != NULL and strategy != 0     -> EXACT/CLASS/GENERIC
 *                                                    set by pci_bind_drivers()
 *
 * For USB devices we don't have a separate "driver" pointer per slot;
 * the class drivers (usb_hid, usb_msc, usb_hub) attach by setting
 * fields on usb_device. We synthesise the strategy from those fields:
 *
 *   is_hub                 -> usb_hub  (CLASS, "hub class match")
 *   hid_state != NULL      -> usb_hid  (CLASS, "HID class match")
 *   bulk_in_dci != 0       -> usb_msc  (CLASS, "MSC class match")
 *   otherwise              -> NONE     ("no class driver matched")
 */

#include <tobyos/drvmatch.h>
#include <tobyos/pci.h>
#include <tobyos/xhci.h>
#include <tobyos/usb.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>
#include <tobyos/abi/abi.h>
#include <tobyos/drvdb.h>

#define DRVMATCH_TAG "drvmatch"

/* ============================================================
 * helpers
 * ============================================================ */

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Forward: drvmatch_init() prints a snapshot of the table after the
 * boot bind pass. We reuse it from drvmatch_disable_pci to confirm
 * the post-rebind state, so it's static-callable. */
static void log_state(const char *tag);

/* ============================================================
 * PCI: fill drvmatch_info from pci_dev
 * ============================================================ */

static void fill_pci(struct abi_drvmatch_info *out, struct pci_dev *p) {
    if (!out || !p) return;
    memset(out, 0, sizeof(*out));
    out->bus        = ABI_DEVT_BUS_PCI;
    out->vendor     = p->vendor;
    out->device     = p->device;
    out->class_code = p->class_code;
    out->subclass   = p->subclass;
    out->prog_if    = p->prog_if;
    out->bound      = (p->driver != 0) ? 1u : 0u;

    if (p->match_strategy == ABI_DRVMATCH_FORCED_OFF) {
        out->strategy = ABI_DRVMATCH_FORCED_OFF;
        copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, "(disabled)");
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                 "driver disabled by drvmatch_disable_pci");
        return;
    }

    if (p->driver) {
        out->strategy = (p->match_strategy != 0)
                            ? p->match_strategy
                            : ABI_DRVMATCH_EXACT;
        copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, p->driver->name);
        switch (out->strategy) {
        case ABI_DRVMATCH_EXACT:
            copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                     "vendor:device matched"); break;
        case ABI_DRVMATCH_CLASS:
            copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                     "class/subclass matched"); break;
        case ABI_DRVMATCH_GENERIC:
            copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                     "generic fallback driver"); break;
        default:
            copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                     "bound, strategy unknown"); break;
        }
        return;
    }

    /* M35A: enrich the unbound case using the drvdb knowledge base.
     * Three outcomes:
     *   - drvdb knows the chip and tags it UNSUPPORTED
     *       -> strategy=UNSUPPORTED, reason="<friendly>: not supported"
     *   - drvdb knows the chip but tags it SUPPORTED/PARTIAL
     *       -> strategy=NONE, reason="<friendly>: driver missing/declined"
     *       (this happens when the driver was blacklisted or its
     *       probe declined.)
     *   - drvdb has no record -> strategy=NONE, generic reason. */
    const struct drvdb_pci_entry *e = drvdb_pci_lookup(p->vendor, p->device);
    if (e && e->tier == DRVDB_UNSUPPORTED) {
        out->strategy = ABI_DRVMATCH_UNSUPPORTED;
        copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, "(unsupported)");
        char tmp[ABI_DRVMATCH_REASON_MAX];
        ksnprintf(tmp, sizeof(tmp), "%s: known unsupported", e->friendly);
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX, tmp);
        return;
    }
    out->strategy = ABI_DRVMATCH_NONE;
    copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, "(none)");
    if (e) {
        char tmp[ABI_DRVMATCH_REASON_MAX];
        ksnprintf(tmp, sizeof(tmp),
                  "%s: no driver bound", e->friendly);
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX, tmp);
    } else {
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                 "no matching driver registered");
    }
}

/* ============================================================
 * USB: classify by which class driver attached
 * ============================================================ */

static void classify_usb(struct abi_drvmatch_info *out,
                         const struct abi_dev_info *u) {
    if (!out || !u) return;
    memset(out, 0, sizeof(*out));
    out->bus        = ABI_DEVT_BUS_USB;
    out->vendor     = u->vendor;
    out->device     = u->device;
    out->class_code = u->class_code;
    out->subclass   = u->subclass;
    out->prog_if    = u->prog_if;
    out->bound      = (u->driver[0] && u->driver[0] != '?') ? 1u : 0u;

    if (out->bound) {
        out->strategy = ABI_DRVMATCH_CLASS;
        copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, u->driver);
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                 "USB class driver matched");
        return;
    }
    /* M35A: enrich the unbound USB case using drvdb. */
    const struct drvdb_usb_entry *e =
        drvdb_usb_lookup((uint8_t)u->class_code,
                         (uint8_t)u->subclass,
                         (uint8_t)u->prog_if);
    if (e && e->tier == DRVDB_UNSUPPORTED) {
        out->strategy = ABI_DRVMATCH_UNSUPPORTED;
        copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, "(unsupported)");
        char tmp[ABI_DRVMATCH_REASON_MAX];
        ksnprintf(tmp, sizeof(tmp), "%s: known unsupported", e->friendly);
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX, tmp);
        return;
    }
    out->strategy = ABI_DRVMATCH_NONE;
    copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, "(none)");
    if (e) {
        char tmp[ABI_DRVMATCH_REASON_MAX];
        ksnprintf(tmp, sizeof(tmp), "%s: no driver bound", e->friendly);
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX, tmp);
    } else {
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                 "no USB class driver matched");
    }
}

/* ============================================================
 * public query
 * ============================================================ */

long drvmatch_query(uint32_t bus, uint32_t vendor, uint32_t device,
                    struct abi_drvmatch_info *out) {
    if (!out) return -ABI_EFAULT;
    memset(out, 0, sizeof(*out));

    if (bus == ABI_DEVT_BUS_PCI) {
        size_t total = pci_device_count();
        for (size_t i = 0; i < total; i++) {
            struct pci_dev *p = pci_device_at(i);
            if (!p) continue;
            if (p->vendor == (uint16_t)vendor &&
                p->device == (uint16_t)device) {
                fill_pci(out, p);
                return 0;
            }
        }
        /* Not found: synthesise an UNSUPPORTED record so userland
         * can display "we have no record of this device" cleanly. */
        out->bus     = ABI_DEVT_BUS_PCI;
        out->vendor  = vendor;
        out->device  = device;
        out->strategy = ABI_DRVMATCH_NONE;
        copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, "(none)");
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                 "device not present in PCI inventory");
        return -ABI_ENOENT;
    }

    if (bus == ABI_DEVT_BUS_USB) {
        int total = xhci_introspect_count();
        for (int i = 0; i < total; i++) {
            struct abi_dev_info rec;
            memset(&rec, 0, sizeof(rec));
            if (xhci_introspect_at(i, &rec) <= 0) continue;
            if (rec.vendor == vendor && rec.device == device) {
                classify_usb(out, &rec);
                return 0;
            }
        }
        out->bus     = ABI_DEVT_BUS_USB;
        out->vendor  = vendor;
        out->device  = device;
        out->strategy = ABI_DRVMATCH_NONE;
        copy_str(out->driver, ABI_DRVMATCH_DRIVER_MAX, "(none)");
        copy_str(out->reason, ABI_DRVMATCH_REASON_MAX,
                 "device not present in USB inventory");
        return -ABI_ENOENT;
    }

    return -ABI_EINVAL;
}

/* ============================================================
 * counters
 * ============================================================ */

void drvmatch_count(uint32_t *out_total, uint32_t *out_bound,
                    uint32_t *out_unbound, uint32_t *out_forced) {
    uint32_t total = 0, bound = 0, unbound = 0, forced = 0;
    size_t pci_n = pci_device_count();
    for (size_t i = 0; i < pci_n; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (!p) continue;
        total++;
        if (p->match_strategy == ABI_DRVMATCH_FORCED_OFF) forced++;
        else if (p->driver) bound++;
        else unbound++;
    }
    int usb_n = xhci_introspect_count();
    for (int i = 0; i < usb_n; i++) {
        struct abi_dev_info rec;
        if (xhci_introspect_at(i, &rec) <= 0) continue;
        total++;
        if (rec.driver[0] && rec.driver[0] != '?') bound++;
        else unbound++;
    }
    if (out_total)   *out_total   = total;
    if (out_bound)   *out_bound   = bound;
    if (out_unbound) *out_unbound = unbound;
    if (out_forced)  *out_forced  = forced;
}

/* ============================================================
 * dump / boot summary
 * ============================================================ */

static const char *strat_name(uint32_t s) {
    switch (s) {
    case ABI_DRVMATCH_NONE:        return "NONE";
    case ABI_DRVMATCH_EXACT:       return "EXACT";
    case ABI_DRVMATCH_CLASS:       return "CLASS";
    case ABI_DRVMATCH_GENERIC:     return "GENERIC";
    case ABI_DRVMATCH_UNSUPPORTED: return "UNSUPPORTED";
    case ABI_DRVMATCH_FORCED_OFF:  return "FORCED_OFF";
    default:                       return "?";
    }
}

void drvmatch_dump_kprintf(void) {
    uint32_t total = 0, bound = 0, unbound = 0, forced = 0;
    drvmatch_count(&total, &bound, &unbound, &forced);
    kprintf("[drvmatch] === driver match table ===\n");
    kprintf("[drvmatch] total=%u bound=%u unbound=%u forced_off=%u\n",
            (unsigned)total, (unsigned)bound,
            (unsigned)unbound, (unsigned)forced);

    size_t pci_n = pci_device_count();
    for (size_t i = 0; i < pci_n; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (!p) continue;
        struct abi_drvmatch_info r;
        fill_pci(&r, p);
        kprintf("[drvmatch] PCI %02x:%02x.%x %04x:%04x cls=%02x.%02x "
                "drv=%s strat=%s\n",
                (unsigned)p->bus, (unsigned)p->slot, (unsigned)p->fn,
                (unsigned)p->vendor, (unsigned)p->device,
                (unsigned)p->class_code, (unsigned)p->subclass,
                r.driver, strat_name(r.strategy));
    }

    int usb_n = xhci_introspect_count();
    for (int i = 0; i < usb_n; i++) {
        struct abi_dev_info rec;
        if (xhci_introspect_at(i, &rec) <= 0) continue;
        struct abi_drvmatch_info r;
        classify_usb(&r, &rec);
        kprintf("[drvmatch] USB slot=%u %04x:%04x cls=%02x.%02x "
                "drv=%s strat=%s\n",
                (unsigned)rec.index,
                (unsigned)rec.vendor, (unsigned)rec.device,
                (unsigned)rec.class_code, (unsigned)rec.subclass,
                r.driver, strat_name(r.strategy));
    }
}

static void log_state(const char *tag) {
    uint32_t total = 0, bound = 0, unbound = 0, forced = 0;
    drvmatch_count(&total, &bound, &unbound, &forced);
    kprintf("[drvmatch] %s: total=%u bound=%u unbound=%u forced_off=%u\n",
            tag, (unsigned)total, (unsigned)bound,
            (unsigned)unbound, (unsigned)forced);
    slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
              "drvmatch %s: total=%u bound=%u unbound=%u forced=%u",
              tag, (unsigned)total, (unsigned)bound,
              (unsigned)unbound, (unsigned)forced);
}

void drvmatch_init(void) {
    log_state("init");
}

/* ============================================================
 * test-only: disable / re-enable a PCI driver
 * ============================================================ */

/* Walk the PCI driver registry until we find one with the given name.
 * This pokes at internals (drv->_next) so it lives here, not in pci.c. */
struct pci_driver *pci_drv_find(const char *name);   /* defined below */

/* We don't have a public iterator over the driver list, so we add a
 * tiny helper here that the registry's sole owner (pci.c) exports
 * through the same compilation unit. To keep the patch surface
 * small we use a weak-ish forward + an inline copy: walk the public
 * pci_device_at side-effect of probe-bind. Since we DON'T need to
 * touch the list head from outside, we instead expose an iteration
 * helper inside pci.c. */
extern struct pci_driver *pci_driver_iter(struct pci_driver *prev);

long drvmatch_disable_pci(const char *name) {
    if (!name || !*name) return -ABI_EINVAL;
    struct pci_driver *drv = 0;
    for (struct pci_driver *it = pci_driver_iter(0); it;
         it = pci_driver_iter(it)) {
        if (it->name && !strcmp(it->name, name)) { drv = it; break; }
    }
    if (!drv) {
        kprintf("[drvmatch] disable: no driver named '%s'\n", name);
        return -ABI_ENOENT;
    }
    long unbound = 0;
    size_t pci_n = pci_device_count();
    for (size_t i = 0; i < pci_n; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (!p || p->driver != drv) continue;
        if (drv->remove) drv->remove(p);
        p->driver = 0;
        p->driver_data = 0;
        p->match_strategy = ABI_DRVMATCH_FORCED_OFF;
        unbound++;
        kprintf("[drvmatch] disable: unbound %02x:%02x.%x from %s\n",
                (unsigned)p->bus, (unsigned)p->slot, (unsigned)p->fn,
                drv->name);
    }
    drv->_disabled = 1;
    log_state("post-disable");
    /* Re-run the bind pass so any fallback driver in the registry can
     * try to claim the device. */
    pci_bind_drivers();
    log_state("post-rebind");
    slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
              "drvmatch disabled '%s' (unbound %ld dev)", name, unbound);
    return unbound;
}

long drvmatch_reenable_pci(const char *name) {
    if (!name || !*name) return -ABI_EINVAL;
    struct pci_driver *drv = 0;
    for (struct pci_driver *it = pci_driver_iter(0); it;
         it = pci_driver_iter(it)) {
        if (it->name && !strcmp(it->name, name)) { drv = it; break; }
    }
    if (!drv) return -ABI_ENOENT;
    drv->_disabled = 0;
    /* Clear FORCED_OFF marks so future binds start from a clean
     * slate; pci_bind_drivers() leaves bound devices alone, so any
     * device currently owned by a fallback driver stays owned --
     * the test harness calls re-enable to leave the registry in a
     * consistent state, not to hand the device back. */
    size_t pci_n = pci_device_count();
    long restored = 0;
    for (size_t i = 0; i < pci_n; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (!p) continue;
        if (p->match_strategy == ABI_DRVMATCH_FORCED_OFF && !p->driver) {
            p->match_strategy = ABI_DRVMATCH_NONE;
        }
    }
    pci_bind_drivers();
    /* Count how many devices the disabled driver re-claimed. */
    for (size_t i = 0; i < pci_n; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (p && p->driver == drv) restored++;
    }
    log_state("post-reenable");
    slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_HW,
              "drvmatch re-enabled '%s' (rebound %ld dev)", name, restored);
    return restored;
}
