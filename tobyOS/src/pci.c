/* pci.c -- PCI bus enumeration, BAR decode, driver registry.
 *
 * Layered on the same legacy port-CF8/CFC config-space accessors as
 * before; what's new in milestone 21:
 *
 *   1. pci_init() does a one-shot recursive walk (bus 0 + every PCI-
 *      to-PCI bridge it finds) and stores one struct pci_dev per
 *      responsive function in g_devs[]. BAR base addresses + sizes
 *      are decoded once, so later drivers don't have to know the
 *      probe dance.
 *
 *   2. pci_register_driver() inserts a driver into a singly-linked
 *      list. pci_bind_drivers() walks every (device, registered
 *      driver) pair and calls probe() on the first match. Match
 *      tables can target either vendor:device pairs OR class triples
 *      (PCI_ANY_ID / PCI_ANY_CLASS = wildcard).
 *
 *   3. pci_map_bar() lifts the e1000-style "vmm_map(BAR phys, len,
 *      NOCACHE)" recipe into a one-liner every new driver can use.
 *
 * The legacy pci_find / pci_enable / pci_bar_addr helpers are kept as
 * thin wrappers over the new table so any leftover ad-hoc caller still
 * works during the transition.
 *
 * Address layout written to 0xCF8:
 *
 *    31     30..24      23..16        15..11       10..8        7..2        1..0
 *   +---+-------------+----------+-------------+-----------+-------------+-----+
 *   | E | reserved    | bus #    | device #    | function  | register #  | 00  |
 *   +---+-------------+----------+-------------+-----------+-------------+-----+
 */

#include <tobyos/pci.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/klibc.h>
#include <tobyos/abi/abi.h>

#define PCI_ADDR_PORT  0xCF8
#define PCI_DATA_PORT  0xCFC

/* ============================================================== */
/* low-level config-space access (unchanged from milestone 9)     */
/* ============================================================== */

static inline uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t fn,
                                uint8_t off) {
    return ((uint32_t)1   << 31)
         | ((uint32_t)bus << 16)
         | ((uint32_t)(slot & 0x1F) << 11)
         | ((uint32_t)(fn   & 0x07) <<  8)
         | ((uint32_t)(off & 0xFC));
}

static inline void out32(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port) : "memory");
}
static inline uint32_t in32(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off) {
    out32(PCI_ADDR_PORT, pci_addr(bus, slot, fn, off));
    return in32(PCI_DATA_PORT);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off) {
    uint32_t d = pci_cfg_read32(bus, slot, fn, off);
    return (uint16_t)(d >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off) {
    uint32_t d = pci_cfg_read32(bus, slot, fn, off);
    return (uint8_t)(d >> ((off & 3) * 8));
}

void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off,
                     uint32_t val) {
    out32(PCI_ADDR_PORT, pci_addr(bus, slot, fn, off));
    out32(PCI_DATA_PORT, val);
}

void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off,
                     uint16_t val) {
    uint32_t d = pci_cfg_read32(bus, slot, fn, off);
    unsigned shift = (off & 2) * 8;
    d &= ~((uint32_t)0xFFFF << shift);
    d |= ((uint32_t)val << shift);
    pci_cfg_write32(bus, slot, fn, off, d);
}

bool pci_present(uint8_t bus, uint8_t slot, uint8_t fn) {
    return pci_cfg_read16(bus, slot, fn, PCI_CFG_VENDOR_ID) != 0xFFFF;
}

void pci_enable(uint8_t bus, uint8_t slot, uint8_t fn, uint16_t bits) {
    uint16_t cmd = pci_cfg_read16(bus, slot, fn, PCI_CFG_COMMAND);
    cmd |= bits;
    pci_cfg_write16(bus, slot, fn, PCI_CFG_COMMAND, cmd);
}

uint64_t pci_bar_addr(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t bar_off) {
    uint32_t bar = pci_cfg_read32(bus, slot, fn, bar_off);
    if (bar == 0) return 0;
    if (bar & 1)  return 0;             /* I/O space, not what we want */
    uint8_t type = (uint8_t)((bar >> 1) & 0x3);
    uint64_t base = bar & ~(uint64_t)0xF;
    if (type == 2) {
        uint32_t hi = pci_cfg_read32(bus, slot, fn, bar_off + 4);
        base |= ((uint64_t)hi) << 32;
    }
    return base;
}

/* ============================================================== */
/* enumeration: walk bus 0 + bridges, fill g_devs[]                */
/* ============================================================== */

static struct pci_dev g_devs[PCI_MAX_DEVICES];
static size_t          g_dev_count;

static struct pci_driver *g_drivers;     /* singly-linked list */
static bool               g_initialised; /* pci_init has run    */

void pci_dev_enable(struct pci_dev *dev, uint16_t bits) {
    pci_enable(dev->bus, dev->slot, dev->fn, bits);
}

size_t          pci_device_count(void)        { return g_dev_count; }
struct pci_dev *pci_device_at(size_t idx)     {
    return (idx < g_dev_count) ? &g_devs[idx] : 0;
}

/* Decode all 6 BARs of a function. We do this exactly once per device
 * during enumeration so probes can read dev->bar[i] directly without
 * having to disable/restore the device themselves. The standard sizing
 * recipe is:
 *
 *   1. Save current BAR value.
 *   2. Write 0xFFFFFFFF (or 0xFFFFFFFFFFFFFFFF for 64-bit).
 *   3. Read back. Mask off type bits, NOT the result, add 1 -> size.
 *   4. Restore the saved BAR.
 *
 * Devices that don't decode the size bits (or have only enabled-state
 * BARs) report 0 -- we treat those as "no BAR".
 */
static void decode_bars(struct pci_dev *d) {
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        d->bar[i]       = 0;
        d->bar_size[i]  = 0;
        d->bar_is_io[i] = 0;
        d->bar_is_64[i] = 0;
    }

    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (d->bar_is_64[i]) continue;       /* upper-half slot, skip */

        uint8_t  off  = (uint8_t)(PCI_CFG_BAR0 + i * 4);
        uint32_t orig = pci_cfg_read32(d->bus, d->slot, d->fn, off);
        if (orig == 0) continue;

        bool     is_io  = (orig & 0x1) != 0;
        uint8_t  mtype  = (uint8_t)((orig >> 1) & 0x3);
        bool     is_64  = !is_io && (mtype == 2);

        /* Size probe -- write all-ones, read back, restore. */
        pci_cfg_write32(d->bus, d->slot, d->fn, off, 0xFFFFFFFFu);
        uint32_t lo_mask = pci_cfg_read32(d->bus, d->slot, d->fn, off);
        pci_cfg_write32(d->bus, d->slot, d->fn, off, orig);

        uint32_t hi_orig = 0, hi_mask = 0;
        if (is_64 && i + 1 < PCI_BAR_COUNT) {
            uint8_t off_hi = (uint8_t)(off + 4);
            hi_orig = pci_cfg_read32(d->bus, d->slot, d->fn, off_hi);
            pci_cfg_write32(d->bus, d->slot, d->fn, off_hi, 0xFFFFFFFFu);
            hi_mask = pci_cfg_read32(d->bus, d->slot, d->fn, off_hi);
            pci_cfg_write32(d->bus, d->slot, d->fn, off_hi, hi_orig);
        }

        /* Size computation. KEY POINT: for 32-bit BARs the complement
         * MUST be done in 32 bits. If you widen first and then ~, the
         * upper 32 bits of the all-ones mask become 0xFFFFFFFF, and
         * (~mask)+1 produces a 64-bit value in the exa-byte range
         * instead of the few bytes the device actually decodes. */
        uint64_t size;
        if (is_io) {
            uint32_t m = lo_mask & 0xFFFFFFFCu;
            if (m == 0) continue;
            size = (uint64_t)((~m) + 1u);
        } else if (is_64) {
            uint64_t m = ((uint64_t)hi_mask << 32) |
                         (uint64_t)(lo_mask & 0xFFFFFFF0u);
            if (m == 0) continue;
            size = (~m) + 1ull;
        } else {
            uint32_t m = lo_mask & 0xFFFFFFF0u;
            if (m == 0) continue;
            size = (uint64_t)((~m) + 1u);
        }

        uint64_t base;
        if (is_io) {
            base = (uint64_t)(orig & 0xFFFFFFFCu);
        } else {
            uint64_t lo = (uint64_t)(orig & 0xFFFFFFF0u);
            uint64_t hi = is_64 ? (uint64_t)hi_orig : 0;
            base = lo | (hi << 32);
        }

        d->bar[i]       = base;
        d->bar_size[i]  = size;
        d->bar_is_io[i] = is_io ? 1 : 0;
        if (is_64 && i + 1 < PCI_BAR_COUNT) {
            d->bar_is_64[i + 1] = 1;       /* mark upper-half slot */
        }
    }
}

/* Forward decl for bridge recursion. */
static void scan_bus(uint8_t bus);

static void scan_function(uint8_t bus, uint8_t slot, uint8_t fn) {
    if (!pci_present(bus, slot, fn)) return;
    if (g_dev_count >= PCI_MAX_DEVICES) {
        kprintf("[pci] WARN: device table full (%u entries) -- "
                "ignoring %02x:%02x.%x\n",
                (unsigned)PCI_MAX_DEVICES, bus, slot, fn);
        return;
    }

    struct pci_dev *d = &g_devs[g_dev_count++];
    d->bus  = bus;
    d->slot = slot;
    d->fn   = fn;

    d->vendor      = pci_cfg_read16(bus, slot, fn, PCI_CFG_VENDOR_ID);
    d->device      = pci_cfg_read16(bus, slot, fn, PCI_CFG_DEVICE_ID);
    d->revision    = pci_cfg_read8 (bus, slot, fn, PCI_CFG_REV_ID);
    d->prog_if     = pci_cfg_read8 (bus, slot, fn, PCI_CFG_PROG_IF);
    d->subclass    = pci_cfg_read8 (bus, slot, fn, PCI_CFG_SUBCLASS);
    d->class_code  = pci_cfg_read8 (bus, slot, fn, PCI_CFG_CLASS);
    d->header_type = pci_cfg_read8 (bus, slot, fn, PCI_CFG_HEADER_TYPE);
    d->irq_line    = pci_cfg_read8 (bus, slot, fn, PCI_CFG_INT_LINE);
    d->irq_pin     = pci_cfg_read8 (bus, slot, fn, PCI_CFG_INT_PIN);
    d->driver_data = 0;
    d->driver      = 0;

    /* BAR decode only makes sense on header type 0 (endpoints).
     * Type-1 (PCI bridges) reuse offsets 0x10/0x14 as different
     * registers. We zero everything for bridges. */
    if ((d->header_type & 0x7F) == 0) {
        decode_bars(d);
    } else {
        for (int i = 0; i < PCI_BAR_COUNT; i++) {
            d->bar[i] = 0;
            d->bar_size[i] = 0;
            d->bar_is_io[i] = 0;
            d->bar_is_64[i] = 0;
        }
    }

    /* Bridge: recurse onto the secondary bus number. */
    if ((d->header_type & 0x7F) == 1) {
        uint8_t sec = pci_cfg_read8(bus, slot, fn, PCI_CFG_SEC_BUS);
        if (sec != bus) scan_bus(sec);
    }
}

static void scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        if (!pci_present(bus, slot, 0)) continue;
        scan_function(bus, slot, 0);

        /* Multi-function device? (bit 7 of the function-0 header type).
         * If set we probe functions 1..7; otherwise we skip them. */
        uint8_t hdr = pci_cfg_read8(bus, slot, 0, PCI_CFG_HEADER_TYPE);
        if ((hdr & 0x80) == 0) continue;
        for (uint8_t fn = 1; fn < 8; fn++) {
            scan_function(bus, slot, fn);
        }
    }
}

/* ============================================================== */
/* dump + lookup                                                    */
/* ============================================================== */

static const char *class_label(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case PCI_CLASS_MASS_STORAGE:
        switch (sub) {
        case 0x00: return "storage.scsi";
        case PCI_SUBCLASS_IDE:  return "storage.ide";
        case 0x05: return "storage.ata";
        case PCI_SUBCLASS_AHCI: return "storage.ahci";
        case PCI_SUBCLASS_NVME: return "storage.nvme";
        default:   return "storage";
        }
    case PCI_CLASS_NETWORK:
        switch (sub) {
        case PCI_SUBCLASS_ETHERNET: return "network.ethernet";
        default:                    return "network";
        }
    case PCI_CLASS_DISPLAY:
        switch (sub) {
        case 0x00: return "display.vga";
        case 0x80: return "display.other";
        default:   return "display";
        }
    case PCI_CLASS_BRIDGE:
        switch (sub) {
        case 0x00: return "bridge.host";
        case 0x01: return "bridge.isa";
        case PCI_SUBCLASS_BRIDGE_PCI: return "bridge.pci";
        default:   return "bridge";
        }
    case PCI_CLASS_SERIAL_BUS:
        switch (sub) {
        case PCI_SUBCLASS_USB: return "serial.usb";
        default:               return "serial";
        }
    default: return "device";
    }
}

void pci_dump(void) {
    kprintf("[pci] %lu device(s):\n", (unsigned long)g_dev_count);
    for (size_t i = 0; i < g_dev_count; i++) {
        struct pci_dev *d = &g_devs[i];
        kprintf("[pci] %02x:%02x.%x  %04x:%04x  %-18s",
                d->bus, d->slot, d->fn,
                d->vendor, d->device,
                class_label(d->class_code, d->subclass));
        if (d->irq_pin != 0) kprintf("  IRQ %u", (unsigned)d->irq_line);
        for (int b = 0; b < PCI_BAR_COUNT; b++) {
            if (d->bar_size[b] == 0 || d->bar_is_64[b]) continue;
            uint64_t kib = d->bar_size[b] / 1024;
            if (kib == 0) {
                kprintf("  BAR%d %s %lu",
                        b, d->bar_is_io[b] ? "io" : "mem",
                        (unsigned long)d->bar_size[b]);
            } else {
                kprintf("  BAR%d %s %luK",
                        b, d->bar_is_io[b] ? "io" : "mem",
                        (unsigned long)kib);
            }
        }
        kprintf("\n");
    }
}

struct pci_dev *pci_find_dev(uint16_t vendor, uint16_t device) {
    for (size_t i = 0; i < g_dev_count; i++) {
        struct pci_dev *d = &g_devs[i];
        if ((vendor == PCI_ANY_ID || d->vendor == vendor) &&
            (device == PCI_ANY_ID || d->device == device)) {
            return d;
        }
    }
    return 0;
}

/* Backwards-compat entry point used by code that hasn't migrated to a
 * struct pci_driver yet. Delegates to pci_find_dev so it sees the same
 * enumeration. We keep the (out_bus, out_slot, out_fn) writes optional
 * to match the milestone-9 signature exactly. */
bool pci_find(uint16_t vendor, uint16_t device,
              uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_fn) {
    struct pci_dev *d = pci_find_dev(vendor, device);
    if (!d) return false;
    if (out_bus)  *out_bus  = d->bus;
    if (out_slot) *out_slot = d->slot;
    if (out_fn)   *out_fn   = d->fn;
    return true;
}

/* ============================================================== */
/* capability list walker                                          */
/* ============================================================== */

/* The cap list lives in cfg space. Status bit 4 says it's present;
 * the head is at offset 0x34 (type-0 header). Each cap starts with
 * { uint8_t cap_id; uint8_t cap_next; ... }. The lower 2 bits of any
 * pointer into the list are reserved (per PCI spec) so we mask them
 * off; an offset of 0 terminates the chain. */

uint8_t pci_cap_first(struct pci_dev *dev) {
    uint16_t status = pci_cfg_read16(dev->bus, dev->slot, dev->fn,
                                     PCI_CFG_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;
    uint8_t off = pci_cfg_read8(dev->bus, dev->slot, dev->fn,
                                PCI_CFG_CAP_PTR);
    return (uint8_t)(off & 0xFCu);
}

uint8_t pci_cap_next(struct pci_dev *dev, uint8_t prev_off) {
    if (prev_off == 0) return 0;
    uint8_t off = pci_cfg_read8(dev->bus, dev->slot, dev->fn,
                                (uint8_t)(prev_off + 1u));
    return (uint8_t)(off & 0xFCu);
}

uint8_t pci_find_capability(struct pci_dev *dev, uint8_t cap_id) {
    /* Cap chain entries are at most 256 bytes apart and the chain has
     * at most 48 entries before it must terminate (the spec doesn't
     * actually impose a hard cap, but we bound the loop to be safe in
     * case some buggy device gives us a self-referential next ptr). */
    int guard = 64;
    for (uint8_t off = pci_cap_first(dev);
         off != 0 && guard-- > 0;
         off = pci_cap_next(dev, off)) {
        uint8_t id = pci_cfg_read8(dev->bus, dev->slot, dev->fn, off);
        if (id == cap_id) return off;
    }
    return 0;
}

/* ============================================================== */
/* MSI / MSI-X enablement                                          */
/* ============================================================== */

bool pci_msi_enable(struct pci_dev *dev, uint8_t vector, uint8_t lapic_id) {
    if (!dev) return false;
    uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
    if (cap == 0) return false;

    uint8_t  bus  = dev->bus, slot = dev->slot, fn = dev->fn;
    uint16_t ctrl = pci_cfg_read16(bus, slot, fn, (uint8_t)(cap + PCI_MSI_CTRL));
    bool     wide = (ctrl & PCI_MSI_CTRL_64BIT) != 0;

    /* Force MME to 0 (= 1 vector enabled). Even if the device claims
     * up to 32 vectors via MMC, this is the simple single-shot form
     * we promise in the header. */
    ctrl &= ~PCI_MSI_CTRL_MME_MASK;

    /* Program the message address (lower half always; upper half =
     * 0 when 64-bit-capable, since LAPIC MMIO is in the low 4 GiB). */
    uint32_t addr_lo = (uint32_t)(MSI_ADDR_BASE |
                                  ((uint32_t)lapic_id << MSI_ADDR_DEST_SHIFT));
    pci_cfg_write32(bus, slot, fn,
                    (uint8_t)(cap + PCI_MSI_ADDR_LO), addr_lo);
    if (wide) {
        pci_cfg_write32(bus, slot, fn,
                        (uint8_t)(cap + PCI_MSI_ADDR_HI), 0);
    }

    /* Message Data: just the IDT vector, fixed delivery, edge,
     * deassert (all-zero high bits). */
    uint8_t data_off = (uint8_t)(cap + (wide ? PCI_MSI_DATA_64
                                              : PCI_MSI_DATA_32));
    pci_cfg_write16(bus, slot, fn, data_off, (uint16_t)vector);

    /* Disable INTx (we don't want both paths firing) and enable MSI. */
    pci_dev_enable(dev, PCI_CMD_INT_DISABLE);
    ctrl |= PCI_MSI_CTRL_ENABLE;
    pci_cfg_write16(bus, slot, fn,
                    (uint8_t)(cap + PCI_MSI_CTRL), ctrl);

    kprintf("[pci] %02x:%02x.%x: MSI enabled (cap=0x%02x, %s, "
            "vec=0x%02x lapic=%u)\n",
            bus, slot, fn, cap, wide ? "64-bit" : "32-bit",
            (unsigned)vector, (unsigned)lapic_id);
    return true;
}

bool pci_msix_enable(struct pci_dev *dev,
                     uint8_t vector_base, uint8_t lapic_id,
                     uint32_t count) {
    if (!dev || count == 0) return false;
    uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (cap == 0) return false;

    uint8_t  bus  = dev->bus, slot = dev->slot, fn = dev->fn;
    uint16_t ctrl = pci_cfg_read16(bus, slot, fn,
                                   (uint8_t)(cap + PCI_MSIX_CTRL));
    uint32_t tbl_size = (uint32_t)(ctrl & PCI_MSIX_CTRL_TBL_SIZE_MASK) + 1u;
    if (count > tbl_size) {
        kprintf("[pci] %02x:%02x.%x: MSI-X requested %u vectors but "
                "device exposes only %u\n",
                bus, slot, fn,
                (unsigned)count, (unsigned)tbl_size);
        return false;
    }

    /* Locate + map the MSI-X table BAR. */
    uint32_t off_bir = pci_cfg_read32(bus, slot, fn,
                                      (uint8_t)(cap + PCI_MSIX_TBL_OFF_BIR));
    uint8_t  bir         = (uint8_t)(off_bir & PCI_MSIX_BIR_MASK);
    uint32_t table_off   = off_bir & PCI_MSIX_OFFSET_MASK;
    uint32_t table_bytes = tbl_size * (uint32_t)PCI_MSIX_ENTRY_SIZE;

    if (bir >= PCI_BAR_COUNT) {
        kprintf("[pci] %02x:%02x.%x: MSI-X cap names invalid BAR %u\n",
                bus, slot, fn, (unsigned)bir);
        return false;
    }
    void *bar_v = pci_map_bar(dev, bir, 0);
    if (!bar_v) {
        kprintf("[pci] %02x:%02x.%x: MSI-X BAR%u map failed\n",
                bus, slot, fn, (unsigned)bir);
        return false;
    }

    volatile uint32_t *table = (volatile uint32_t *)((uint8_t *)bar_v
                                                     + table_off);

    /* First mask the function while we program the table -- the
     * device must not deliver a half-programmed entry. Set Function
     * Mask, leave MSI-X Enable however the firmware/BIOS left it; we
     * flip both at the end. */
    pci_cfg_write16(bus, slot, fn,
                    (uint8_t)(cap + PCI_MSIX_CTRL),
                    (uint16_t)(ctrl | PCI_MSIX_CTRL_FN_MASK));

    uint32_t addr_lo = (uint32_t)(MSI_ADDR_BASE |
                                  ((uint32_t)lapic_id << MSI_ADDR_DEST_SHIFT));
    for (uint32_t i = 0; i < tbl_size; i++) {
        volatile uint32_t *e = table + (i * 4);  /* 4 dwords per entry */
        if (i < count) {
            e[0] = addr_lo;                               /* ADDR_LO */
            e[1] = 0;                                     /* ADDR_HI */
            e[2] = (uint32_t)(vector_base + i);           /* DATA    */
            e[3] = 0;                                     /* unmask  */
        } else {
            /* Mask any entry we don't own. (Power-on default IS masked,
             * but be defensive in case firmware touched the table.) */
            e[3] = 1;
        }
    }

    /* Disable INTx, enable MSI-X, clear Function Mask. */
    pci_dev_enable(dev, PCI_CMD_INT_DISABLE);
    ctrl = (uint16_t)((ctrl & ~PCI_MSIX_CTRL_FN_MASK) |
                      PCI_MSIX_CTRL_ENABLE);
    pci_cfg_write16(bus, slot, fn,
                    (uint8_t)(cap + PCI_MSIX_CTRL), ctrl);

    kprintf("[pci] %02x:%02x.%x: MSI-X enabled (cap=0x%02x, "
            "BAR%u@+0x%x, %u/%u vecs, base=0x%02x lapic=%u)\n",
            bus, slot, fn, cap, (unsigned)bir, (unsigned)table_off,
            (unsigned)count, (unsigned)tbl_size,
            (unsigned)vector_base, (unsigned)lapic_id);
    (void)table_bytes;
    return true;
}

/* ============================================================== */
/* BAR mapping helper                                               */
/* ============================================================== */

void *pci_map_bar(struct pci_dev *dev, int idx, size_t bytes) {
    if (idx < 0 || idx >= PCI_BAR_COUNT) return 0;
    if (dev->bar_is_io[idx]) return 0;
    if (dev->bar_is_64[idx]) return 0;     /* upper half -- caller meant idx-1 */
    uint64_t phys = dev->bar[idx];
    uint64_t size = bytes ? bytes : dev->bar_size[idx];
    if (phys == 0 || size == 0) return 0;

    /* Round to page bounds: BAR may not be page-aligned (uncommon, but
     * legal); we map from the page that contains it. */
    uint64_t bar_pg   = phys & ~((uint64_t)PAGE_SIZE - 1u);
    size_t   head     = (size_t)(phys - bar_pg);
    size_t   to_map   = head + size;
    to_map = (to_map + PAGE_SIZE - 1u) & ~((size_t)PAGE_SIZE - 1u);

    uint64_t hhdm = pmm_hhdm_offset();
    uint64_t virt = hhdm + bar_pg;

    /* Idempotent: if the page is already mapped (e.g. covered by the
     * vmm_init HHDM sweep because the firmware reported the aperture
     * as USABLE rather than RESERVED), vmm_translate returns non-zero
     * and we just return the cooked pointer. */
    if (vmm_translate(virt) == 0) {
        if (!vmm_map(virt, bar_pg, to_map,
                     VMM_WRITE | VMM_NX | VMM_NOCACHE)) {
            kprintf("[pci] %02x:%02x.%x BAR%d: vmm_map(%p, %lu) failed\n",
                    dev->bus, dev->slot, dev->fn, idx,
                    (void *)bar_pg, (unsigned long)to_map);
            return 0;
        }
    }
    return (void *)pmm_phys_to_virt(phys);
}

/* ============================================================== */
/* driver registry + bind loop                                      */
/* ============================================================== */

void pci_register_driver(struct pci_driver *drv) {
    if (!drv || !drv->name || !drv->probe) {
        kprintf("[pci] WARN: refused malformed driver registration\n");
        return;
    }
    drv->_next = g_drivers;
    g_drivers  = drv;
}

/* M29B helper: iterate the driver registry. Pass NULL to get the
 * first registered driver, or the previous driver to advance. The
 * iteration walks the singly-linked list of struct pci_driver in
 * registration order. */
struct pci_driver *pci_driver_iter(struct pci_driver *prev) {
    if (prev) return prev->_next;
    return g_drivers;
}

/* No auto-bind happens inside pci_register_driver: the kernel boot
 * sequence is
 *      pci_init();          // scan
 *      <driver>_register(); // x N
 *      pci_bind_drivers();  // probe everything in one pass
 * which keeps the bind log contiguous and the probe order
 * deterministic. */

static bool match_one(const struct pci_match *m, struct pci_dev *d) {
    return (m->vendor     == PCI_ANY_ID    || m->vendor     == d->vendor) &&
           (m->device     == PCI_ANY_ID    || m->device     == d->device) &&
           (m->class_code == PCI_ANY_CLASS || m->class_code == d->class_code) &&
           (m->subclass   == PCI_ANY_CLASS || m->subclass   == d->subclass) &&
           (m->prog_if    == PCI_ANY_CLASS || m->prog_if    == d->prog_if);
}

void pci_bind_drivers(void) {
    if (!g_initialised) {
        kprintf("[pci] WARN: pci_bind_drivers called before pci_init\n");
        return;
    }
    size_t bound = 0;
    for (size_t i = 0; i < g_dev_count; i++) {
        struct pci_dev *d = &g_devs[i];
        if (d->driver) continue;
        for (struct pci_driver *drv = g_drivers; drv; drv = drv->_next) {
            /* M29B: skip drivers the test harness disabled.
             * The disabled bit is set by drvmatch_disable_pci(). */
            if (drv->_disabled) continue;
            const struct pci_match *m = drv->matches;
            const struct pci_match *winning = 0;
            for (; m; m++) {
                /* PCI_MATCH_END = all wildcards -> end of table. */
                if (m->vendor == PCI_ANY_ID && m->device == PCI_ANY_ID &&
                    m->class_code == PCI_ANY_CLASS &&
                    m->subclass   == PCI_ANY_CLASS &&
                    m->prog_if    == PCI_ANY_CLASS) break;
                if (match_one(m, d)) { winning = m; break; }
            }
            if (!winning) continue;
            int rc = drv->probe(d);
            if (rc == 0) {
                d->driver = drv;
                /* M29B: classify how the match was satisfied. */
                if (drv->flags & PCI_DRIVER_GENERIC) {
                    d->match_strategy = ABI_DRVMATCH_GENERIC;
                } else if (winning->vendor != PCI_ANY_ID &&
                           winning->device != PCI_ANY_ID) {
                    d->match_strategy = ABI_DRVMATCH_EXACT;
                } else {
                    d->match_strategy = ABI_DRVMATCH_CLASS;
                }
                kprintf("[pci] driver %-8s bound to %02x:%02x.%x  "
                        "(%04x:%04x) strat=%s\n",
                        drv->name, d->bus, d->slot, d->fn,
                        d->vendor, d->device,
                        d->match_strategy == ABI_DRVMATCH_EXACT ? "exact" :
                        d->match_strategy == ABI_DRVMATCH_CLASS ? "class" :
                        d->match_strategy == ABI_DRVMATCH_GENERIC ? "generic" :
                        "?");
                bound++;
                break;
            } else {
                kprintf("[pci] driver %-8s declined %02x:%02x.%x  "
                        "(probe rc=%d)\n",
                        drv->name, d->bus, d->slot, d->fn, rc);
            }
        }
    }
    /* Report total bound vs total devices, NOT "newly bound this pass":
     * the previous version subtracted only the newly-bound count which
     * misled when re-running bind on a partially-bound table. */
    size_t total_bound = 0;
    for (size_t i = 0; i < g_dev_count; i++) {
        if (g_devs[i].driver) total_bound++;
    }
    kprintf("[pci] bind complete -- %lu of %lu device(s) bound  "
            "(%lu newly this pass)\n",
            (unsigned long)total_bound,
            (unsigned long)g_dev_count,
            (unsigned long)bound);
}

void pci_init(void) {
    if (g_initialised) return;
    g_dev_count = 0;
    kprintf("[pci] enumerating bus 0...\n");
    scan_bus(0);
    g_initialised = true;
    pci_dump();
}
