/* hwdb.c -- Milestone 35D: hardware compatibility database.
 *
 * Stays as thin as drvmatch.c: never owns devices, just reads the
 * already-populated PCI / USB / drvdb tables and renders the joined
 * view. No allocation, no global state, no locking required for the
 * read path -- the underlying tables either change rarely (PCI)
 * during boot or are themselves IRQ-safe (usbreg).
 *
 * The status resolution rules (also documented in the ABI header):
 *
 *      drvdb=SUPPORTED   bound=1  -> SUPPORTED
 *      drvdb=SUPPORTED   bound=0  -> PARTIAL    (driver disabled)
 *      drvdb=PARTIAL     *        -> PARTIAL
 *      drvdb=UNSUPPORTED *        -> UNSUPPORTED
 *      drvdb=UNKNOWN     bound=1  -> PARTIAL    (generic driver bound)
 *      drvdb=UNKNOWN     bound=0  -> UNSUPPORTED
 *
 * The same join is exposed three ways:
 *      hwdb_snapshot()   -- syscall-facing flat array
 *      hwdb_counts()     -- aggregate totals (for hwcompat --summary)
 *      hwdb_dump_kprintf -- shell + selftest debug dump
 */

#include <tobyos/hwdb.h>
#include <tobyos/pci.h>
#include <tobyos/usbreg.h>
#include <tobyos/drvdb.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/abi/abi.h>

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

/* Resolve (drvdb tier, bound) into the userland-facing status. The
 * helper is split out so the dump path and the snapshot path agree
 * verbatim. */
static uint8_t resolve_status(uint32_t tier, bool bound) {
    switch (tier) {
    case DRVDB_SUPPORTED:
        return bound ? ABI_HWCOMPAT_SUPPORTED : ABI_HWCOMPAT_PARTIAL;
    case DRVDB_PARTIAL:
        return ABI_HWCOMPAT_PARTIAL;
    case DRVDB_UNSUPPORTED:
        return ABI_HWCOMPAT_UNSUPPORTED;
    default: /* DRVDB_UNKNOWN */
        return bound ? ABI_HWCOMPAT_PARTIAL : ABI_HWCOMPAT_UNSUPPORTED;
    }
}

/* Choose the single-line `reason` string. `friendly` is the drvdb
 * blurb for the device (or "" if drvdb has no record); the format
 * stays stable across releases so userland tools can pattern-match
 * on it for regressions. */
static void render_reason(char *dst, size_t cap,
                          uint8_t status, bool bound,
                          uint32_t tier, const char *friendly,
                          const char *bound_drv) {
    if (!friendly) friendly = "";
    char tmp[ABI_HWCOMPAT_REASON_MAX];

    switch (status) {
    case ABI_HWCOMPAT_SUPPORTED:
        ksnprintf(tmp, sizeof(tmp),
                  "%s: driver %s active",
                  friendly[0] ? friendly : "device",
                  bound_drv && bound_drv[0] ? bound_drv : "(?)");
        break;

    case ABI_HWCOMPAT_PARTIAL:
        if (bound && tier == DRVDB_PARTIAL) {
            ksnprintf(tmp, sizeof(tmp),
                      "%s: %s partial (subset of features)",
                      friendly[0] ? friendly : "device",
                      bound_drv && bound_drv[0] ? bound_drv : "driver");
        } else if (bound && tier == DRVDB_UNKNOWN) {
            ksnprintf(tmp, sizeof(tmp),
                      "%s bound via %s, no drvdb record",
                      bound_drv && bound_drv[0] ? bound_drv : "driver",
                      "class match");
        } else if (!bound && tier == DRVDB_SUPPORTED) {
            ksnprintf(tmp, sizeof(tmp),
                      "%s: driver disabled (blacklisted/declined)",
                      friendly[0] ? friendly : "device");
        } else {
            ksnprintf(tmp, sizeof(tmp),
                      "%s: partial support",
                      friendly[0] ? friendly : "device");
        }
        break;

    case ABI_HWCOMPAT_UNSUPPORTED:
        if (tier == DRVDB_UNSUPPORTED) {
            ksnprintf(tmp, sizeof(tmp),
                      "%s: known unsupported",
                      friendly[0] ? friendly : "device");
        } else {
            ksnprintf(tmp, sizeof(tmp),
                      "unknown device, no driver bound");
        }
        break;

    default:
        ksnprintf(tmp, sizeof(tmp), "(no record)");
        break;
    }
    copy_str(dst, cap, tmp);
}

/* ============================================================
 * PCI -> abi_hwcompat_entry
 * ============================================================ */

static void fill_pci(struct abi_hwcompat_entry *out, struct pci_dev *p) {
    memset(out, 0, sizeof(*out));
    out->bus        = ABI_DEVT_BUS_PCI;
    out->vendor     = p->vendor;
    out->product    = p->device;
    out->class_code = p->class_code;
    out->subclass   = p->subclass;
    out->prog_if    = p->prog_if;
    out->bound      = (p->driver != 0) ? 1u : 0u;

    const struct drvdb_pci_entry *e = drvdb_pci_lookup(p->vendor, p->device);
    uint32_t tier = e ? e->tier : DRVDB_UNKNOWN;
    out->status = resolve_status(tier, out->bound != 0);

    /* friendly name: drvdb wins; otherwise build a "VID:DID" stub so
     * userland tools never have to special-case the empty string. */
    if (e && e->friendly && e->friendly[0]) {
        copy_str(out->friendly, ABI_HWCOMPAT_FRIENDLY_MAX, e->friendly);
    } else {
        char tmp[ABI_HWCOMPAT_FRIENDLY_MAX];
        ksnprintf(tmp, sizeof(tmp),
                  "PCI %04x:%04x (cls %02x.%02x.%02x)",
                  (unsigned)p->vendor, (unsigned)p->device,
                  (unsigned)p->class_code, (unsigned)p->subclass,
                  (unsigned)p->prog_if);
        copy_str(out->friendly, ABI_HWCOMPAT_FRIENDLY_MAX, tmp);
    }

    const char *bound_drv = (p->driver && p->driver->name)
                                ? p->driver->name : "";
    if (out->bound) {
        copy_str(out->driver, ABI_HWCOMPAT_DRIVER_MAX, bound_drv);
    }

    render_reason(out->reason, ABI_HWCOMPAT_REASON_MAX,
                  out->status, out->bound != 0, tier,
                  e ? e->friendly : "", bound_drv);
}

/* ============================================================
 * USB -> abi_hwcompat_entry
 * ============================================================ */

/* Map usbreg's per-row status to the drvdb tier we should consult.
 * usbreg already classifies via drvdb at attach time, so we usually
 * just trust that decision and feed it through resolve_status() with
 * a synthesised tier. */
static uint32_t usbreg_to_drvdb_tier(enum usbreg_status st,
                                     uint32_t fallback_tier) {
    switch (st) {
    case USBREG_STATUS_BOUND:        return DRVDB_SUPPORTED;
    case USBREG_STATUS_UNSUPPORTED:  return DRVDB_UNSUPPORTED;
    case USBREG_STATUS_PROBE_FAILED: return DRVDB_PARTIAL;
    case USBREG_STATUS_UNKNOWN:      return DRVDB_UNKNOWN;
    case USBREG_STATUS_GONE:         return fallback_tier;
    default:                         return fallback_tier;
    }
}

static void fill_usb(struct abi_hwcompat_entry *out,
                     const struct usbreg_entry *u) {
    memset(out, 0, sizeof(*out));
    out->bus        = ABI_DEVT_BUS_USB;
    out->vendor     = u->vendor;
    out->product    = u->product;
    out->class_code = u->dev_class;
    out->subclass   = u->dev_subclass;
    out->prog_if    = u->dev_protocol;
    out->bound      = (u->status == USBREG_STATUS_BOUND) ? 1u : 0u;

    const struct drvdb_usb_entry *e =
        drvdb_usb_lookup(u->dev_class, u->dev_subclass, u->dev_protocol);
    uint32_t fallback_tier = e ? e->tier : DRVDB_UNKNOWN;
    uint32_t tier = usbreg_to_drvdb_tier(u->status, fallback_tier);
    out->status   = resolve_status(tier, out->bound != 0);

    /* friendly: prefer the snapshot the registry already captured,
     * fall back to drvdb, then to a class-triple stub. */
    if (u->friendly[0]) {
        copy_str(out->friendly, ABI_HWCOMPAT_FRIENDLY_MAX, u->friendly);
    } else if (e && e->friendly && e->friendly[0]) {
        copy_str(out->friendly, ABI_HWCOMPAT_FRIENDLY_MAX, e->friendly);
    } else {
        char tmp[ABI_HWCOMPAT_FRIENDLY_MAX];
        ksnprintf(tmp, sizeof(tmp),
                  "USB %02x/%02x/%02x (vid:pid %04x:%04x)",
                  (unsigned)u->dev_class, (unsigned)u->dev_subclass,
                  (unsigned)u->dev_protocol,
                  (unsigned)u->vendor, (unsigned)u->product);
        copy_str(out->friendly, ABI_HWCOMPAT_FRIENDLY_MAX, tmp);
    }

    const char *bound_drv = u->driver[0] ? u->driver : "";
    if (u->status == USBREG_STATUS_BOUND ||
        u->status == USBREG_STATUS_PROBE_FAILED) {
        copy_str(out->driver, ABI_HWCOMPAT_DRIVER_MAX, bound_drv);
    }

    /* Special-case the lifecycle states drvmatch's reason rules
     * don't cover -- we want the operator to know whether a slot is
     * still warm-plugged in or has just been yanked. */
    char tmp[ABI_HWCOMPAT_REASON_MAX];
    switch (u->status) {
    case USBREG_STATUS_PROBE_FAILED:
        ksnprintf(tmp, sizeof(tmp),
                  "%s: probe failed via %s",
                  out->friendly,
                  bound_drv[0] ? bound_drv : "(unknown driver)");
        copy_str(out->reason, ABI_HWCOMPAT_REASON_MAX, tmp);
        return;
    case USBREG_STATUS_GONE:
        ksnprintf(tmp, sizeof(tmp), "%s: detached", out->friendly);
        copy_str(out->reason, ABI_HWCOMPAT_REASON_MAX, tmp);
        return;
    default:
        break;
    }

    render_reason(out->reason, ABI_HWCOMPAT_REASON_MAX,
                  out->status, out->bound != 0, tier,
                  u->friendly[0] ? u->friendly :
                      (e ? e->friendly : ""),
                  bound_drv);
}

/* ============================================================
 * public API
 * ============================================================ */

size_t hwdb_snapshot(struct abi_hwcompat_entry *out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (cap > ABI_HWCOMPAT_MAX_ENTRIES) cap = ABI_HWCOMPAT_MAX_ENTRIES;

    size_t n = 0;

    /* PCI first (stable bus/slot/fn order). */
    size_t pci_n = pci_device_count();
    for (size_t i = 0; i < pci_n && n < cap; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (!p) continue;
        fill_pci(&out[n], p);
        n++;
    }

    /* USB second (attach order from usbreg). Skip GONE entries on
     * the snapshot path -- they're useful in the live dump but the
     * userland tool's "compat status" should reflect what's actually
     * present right now. */
    size_t usb_n = usbreg_count();
    for (size_t i = 0; i < usb_n && n < cap; i++) {
        const struct usbreg_entry *u = usbreg_get(i);
        if (!u) continue;
        if (u->status == USBREG_STATUS_FREE) continue;
        if (u->status == USBREG_STATUS_GONE) continue;
        fill_usb(&out[n], u);
        n++;
    }

    return n;
}

size_t hwdb_counts(size_t *out_supported,
                   size_t *out_partial,
                   size_t *out_unsupported) {
    size_t sup = 0, par = 0, uns = 0;

    /* Mirror hwdb_snapshot()'s walk so the totals always match what
     * a tool would see after listing every row. */
    size_t pci_n = pci_device_count();
    for (size_t i = 0; i < pci_n; i++) {
        struct pci_dev *p = pci_device_at(i);
        if (!p) continue;
        const struct drvdb_pci_entry *e =
            drvdb_pci_lookup(p->vendor, p->device);
        uint32_t tier = e ? e->tier : DRVDB_UNKNOWN;
        uint8_t st = resolve_status(tier, p->driver != 0);
        switch (st) {
        case ABI_HWCOMPAT_SUPPORTED:   sup++; break;
        case ABI_HWCOMPAT_PARTIAL:     par++; break;
        case ABI_HWCOMPAT_UNSUPPORTED: uns++; break;
        default: break;
        }
    }

    size_t usb_n = usbreg_count();
    for (size_t i = 0; i < usb_n; i++) {
        const struct usbreg_entry *u = usbreg_get(i);
        if (!u) continue;
        if (u->status == USBREG_STATUS_FREE) continue;
        if (u->status == USBREG_STATUS_GONE) continue;

        const struct drvdb_usb_entry *e = drvdb_usb_lookup(
            u->dev_class, u->dev_subclass, u->dev_protocol);
        uint32_t fb = e ? e->tier : DRVDB_UNKNOWN;
        uint32_t tier = usbreg_to_drvdb_tier(u->status, fb);
        bool bound = (u->status == USBREG_STATUS_BOUND);
        uint8_t st = resolve_status(tier, bound);
        switch (st) {
        case ABI_HWCOMPAT_SUPPORTED:   sup++; break;
        case ABI_HWCOMPAT_PARTIAL:     par++; break;
        case ABI_HWCOMPAT_UNSUPPORTED: uns++; break;
        default: break;
        }
    }

    if (out_supported)   *out_supported   = sup;
    if (out_partial)     *out_partial     = par;
    if (out_unsupported) *out_unsupported = uns;
    return sup + par + uns;
}

const char *hwdb_status_name(uint32_t status) {
    switch (status) {
    case ABI_HWCOMPAT_SUPPORTED:   return "supported";
    case ABI_HWCOMPAT_PARTIAL:     return "partial";
    case ABI_HWCOMPAT_UNSUPPORTED: return "unsupported";
    case ABI_HWCOMPAT_UNKNOWN:     return "unknown";
    default:                       return "?";
    }
}

void hwdb_dump_kprintf(void) {
    /* Stage on the kernel stack so the printk path doesn't wedge if
     * we ever call this from an IRQ context (the wider buffer also
     * keeps formatting cheap). 16 rows * 144B = 2.3 KiB. */
    struct abi_hwcompat_entry rows[16];
    size_t n = hwdb_snapshot(rows, 16);
    size_t sup = 0, par = 0, uns = 0;
    size_t total = hwdb_counts(&sup, &par, &uns);

    kprintf("[hwdb] compatibility snapshot total=%u "
            "supported=%u partial=%u unsupported=%u\n",
            (unsigned)total,
            (unsigned)sup, (unsigned)par, (unsigned)uns);

    for (size_t i = 0; i < n; i++) {
        const struct abi_hwcompat_entry *r = &rows[i];
        const char *bus = (r->bus == ABI_DEVT_BUS_PCI) ? "pci" :
                          (r->bus == ABI_DEVT_BUS_USB) ? "usb" : "?";
        kprintf("  %s %04x:%04x %02x.%02x.%02x %-11s "
                "drv=%-12s %s\n",
                bus,
                (unsigned)r->vendor, (unsigned)r->product,
                (unsigned)r->class_code, (unsigned)r->subclass,
                (unsigned)r->prog_if,
                hwdb_status_name(r->status),
                r->driver[0] ? r->driver : "(none)",
                r->friendly);
        if (r->reason[0]) {
            kprintf("       reason: %s\n", r->reason);
        }
    }
    if (n == 16 && total > 16) {
        kprintf("  ... %u more rows truncated, use hwcompat for full list\n",
                (unsigned)(total - 16));
    }
}
