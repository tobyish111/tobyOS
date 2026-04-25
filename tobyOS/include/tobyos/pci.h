/* pci.h -- PCI bus enumeration + driver-binding registry.
 *
 * Two-layer interface:
 *
 *   1. Low-level config-space accessors (pci_cfg_read{8,16,32}, _write*).
 *      These work the same way as the milestone-9 cut: legacy port
 *      0xCF8 / 0xCFC, no MMCONFIG. Every higher layer is built on top.
 *
 *   2. Enumeration + driver registry. pci_init() walks bus 0 (with
 *      PCI-to-PCI bridge recursion), probes every (bus,slot,fn), and
 *      stores one struct pci_dev per responsive function in a static
 *      array. pci_register_driver() inserts a driver into a singly-
 *      linked list. pci_bind_drivers() then walks every device, finds
 *      the first matching driver, and calls its probe().
 *
 * The match table allows EITHER "vendor:device pair" matches (use
 * PCI_ANY_CLASS for the class triple), OR "class/subclass/prog_if"
 * matches (use PCI_ANY_ID for vendor/device). All three class fields
 * default to wildcard if unset.
 *
 * Only ONE driver binds per device. probe() returning non-zero means
 * "not me, try the next driver", just like Linux. probe() returning 0
 * stamps dev->driver and stops the search for that device.
 *
 * IRQs / DMA contract for drivers: see milestone-21 architecture doc.
 *   - IRQs:  isr_register(PIC_IRQ_BASE + dev->irq_line, fn) +
 *            pic_unmask(dev->irq_line). One path, no parallel APIC
 *            routing in this milestone.
 *   - DMA:   pmm_alloc_page() for buffers + descriptor rings,
 *            pmm_phys_to_virt() for kernel access, the phys address
 *            goes into descriptor `addr` fields verbatim.
 *   - MMIO BAR: pci_map_bar(dev, idx, flags) -- maps the BAR into
 *            HHDM with VMM_NOCACHE if it lies in a Limine RESERVED
 *            region (which is true for QEMU's PCI aperture).
 */

#ifndef TOBYOS_PCI_H
#define TOBYOS_PCI_H

#include <tobyos/types.h>

/* ------------ standard config-space register offsets ------------ */

#define PCI_CFG_VENDOR_ID    0x00
#define PCI_CFG_DEVICE_ID    0x02
#define PCI_CFG_COMMAND      0x04
#define PCI_CFG_STATUS       0x06
#define PCI_CFG_REV_ID       0x08
#define PCI_CFG_PROG_IF      0x09
#define PCI_CFG_SUBCLASS     0x0A
#define PCI_CFG_CLASS        0x0B
#define PCI_CFG_HEADER_TYPE  0x0E
#define PCI_CFG_BAR0         0x10
#define PCI_CFG_BAR1         0x14
#define PCI_CFG_BAR2         0x18
#define PCI_CFG_BAR3         0x1C
#define PCI_CFG_BAR4         0x20
#define PCI_CFG_BAR5         0x24
#define PCI_CFG_INT_LINE     0x3C
#define PCI_CFG_INT_PIN      0x3D
#define PCI_CFG_CAP_PTR      0x34    /* type-0 header: cap list head */

/* PCI-to-PCI bridge config-space offsets (header_type 1). */
#define PCI_CFG_PRIM_BUS     0x18
#define PCI_CFG_SEC_BUS      0x19
#define PCI_CFG_SUB_BUS      0x1A

/* Command-register bits we care about. */
#define PCI_CMD_IO_SPACE     (1u << 0)
#define PCI_CMD_MEM_SPACE    (1u << 1)
#define PCI_CMD_BUS_MASTER   (1u << 2)
#define PCI_CMD_INT_DISABLE  (1u << 10)

/* Status-register bits we care about. */
#define PCI_STATUS_CAP_LIST  (1u << 4)

/* Capability IDs that show up in the linked list at PCI_CFG_CAP_PTR. */
#define PCI_CAP_ID_MSI       0x05
#define PCI_CAP_ID_VENDOR    0x09    /* virtio caps live under this */
#define PCI_CAP_ID_PCIE      0x10
#define PCI_CAP_ID_MSIX      0x11

/* MSI capability layout (offsets relative to the cap-list entry). */
#define PCI_MSI_CTRL          0x02   /* uint16: Message Control            */
#define PCI_MSI_ADDR_LO       0x04   /* uint32: Message Address [31:0]     */
#define PCI_MSI_ADDR_HI       0x08   /* uint32: Message Address [63:32]    */
#define PCI_MSI_DATA_32       0x08   /* uint16: Message Data (32-bit form) */
#define PCI_MSI_DATA_64       0x0C   /* uint16: Message Data (64-bit form) */

#define PCI_MSI_CTRL_ENABLE        (1u << 0)
#define PCI_MSI_CTRL_64BIT         (1u << 7)
/* MMC = bits[3:1], log2(supported vectors); MME = bits[6:4], log2(used). */
#define PCI_MSI_CTRL_MME_SHIFT     4
#define PCI_MSI_CTRL_MME_MASK      (0x7u << PCI_MSI_CTRL_MME_SHIFT)

/* MSI-X capability layout. */
#define PCI_MSIX_CTRL         0x02   /* uint16: Message Control            */
#define PCI_MSIX_TBL_OFF_BIR  0x04   /* uint32: table offset + BIR (bits[2:0]) */
#define PCI_MSIX_PBA_OFF_BIR  0x08   /* uint32: PBA   offset + BIR             */

#define PCI_MSIX_CTRL_ENABLE       (1u << 15)
#define PCI_MSIX_CTRL_FN_MASK      (1u << 14)
#define PCI_MSIX_CTRL_TBL_SIZE_MASK 0x07FFu  /* size MINUS 1 */

#define PCI_MSIX_BIR_MASK          0x7u
#define PCI_MSIX_OFFSET_MASK       0xFFFFFFF8u

/* MSI-X table entry layout (16 bytes per entry, BAR-relative). */
#define PCI_MSIX_TBL_ADDR_LO  0x00
#define PCI_MSIX_TBL_ADDR_HI  0x04
#define PCI_MSIX_TBL_DATA     0x08
#define PCI_MSIX_TBL_VEC_CTRL 0x0C   /* bit 0 = mask */
#define PCI_MSIX_ENTRY_SIZE   16

/* x86 MSI/MSI-X message format (Intel SDM vol 3, fig 11-13). */
#define MSI_ADDR_BASE         0xFEE00000ULL
#define MSI_ADDR_DEST_SHIFT   12      /* target LAPIC ID lives here */
/* Data: just the IDT vector for "fixed delivery, edge, deassert". */

/* PCI base-class codes we care about. (Spec § Appendix D.) */
#define PCI_CLASS_MASS_STORAGE   0x01
#define PCI_CLASS_NETWORK        0x02
#define PCI_CLASS_DISPLAY        0x03
#define PCI_CLASS_BRIDGE         0x06
#define PCI_CLASS_SERIAL_BUS     0x0C

#define PCI_SUBCLASS_IDE         0x01    /* under MASS_STORAGE */
#define PCI_SUBCLASS_AHCI        0x06    /* under MASS_STORAGE */
#define PCI_SUBCLASS_NVME        0x08    /* under MASS_STORAGE */
#define PCI_SUBCLASS_ETHERNET    0x00    /* under NETWORK      */
#define PCI_SUBCLASS_USB         0x03    /* under SERIAL_BUS   */
#define PCI_SUBCLASS_BRIDGE_PCI  0x04    /* PCI-to-PCI bridge  */

#define PCI_PROGIF_XHCI          0x30    /* under SERIAL_BUS / USB */

/* Wildcards in match tables. 0xFFFF for the 16-bit fields and 0xFF
 * for the 8-bit class triple is the convention used throughout. */
#define PCI_ANY_ID     0xFFFFu
#define PCI_ANY_CLASS  0xFFu

/* ------------ enumeration result table ------------ */

#define PCI_BAR_COUNT     6
#define PCI_MAX_DEVICES  64

struct pci_driver;        /* forward; defined below */

struct pci_dev {
    uint8_t  bus, slot, fn;

    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;     /* 0 = endpoint, 1 = bridge, bit 7 = multifn */
    uint8_t  irq_line;        /* CFG_INT_LINE -- legacy IRQ 0..15 (0xFF = none) */
    uint8_t  irq_pin;         /* CFG_INT_PIN  -- 1..4 (A..D), 0 = no INTx */

    /* Fully-decoded BAR base addresses + sizes. For a 64-bit memory BAR
     * the upper-half slot is left zero in `bar` but its bar_is_64 bit is
     * set so iteration code can skip it. I/O BARs are flagged separately
     * because their `bar` value is a port number, not a phys address. */
    uint64_t bar[PCI_BAR_COUNT];
    uint64_t bar_size[PCI_BAR_COUNT];
    uint8_t  bar_is_io[PCI_BAR_COUNT];
    uint8_t  bar_is_64[PCI_BAR_COUNT];

    void                    *driver_data;   /* opaque, set by probe() */
    const struct pci_driver *driver;        /* NULL = unbound         */

    /* M29B drvmatch annotation. Filled by pci_bind_drivers() once the
     * winning driver is known: ABI_DRVMATCH_EXACT if the matching
     * pci_match entry pinned a specific vendor:device pair,
     * ABI_DRVMATCH_CLASS if it relied on a class/subclass wildcard,
     * ABI_DRVMATCH_GENERIC if the bound driver was tagged in the
     * fallback list (blk_ata, fb-limine, ...). Untouched (NONE) for
     * unbound devices. drvmatch.c reads this directly. */
    uint8_t                  match_strategy;
};

/* ------------ driver registry ------------ */

struct pci_match {
    uint16_t vendor;       /* PCI_ANY_ID    = wildcard */
    uint16_t device;       /* PCI_ANY_ID    = wildcard */
    uint8_t  class_code;   /* PCI_ANY_CLASS = wildcard */
    uint8_t  subclass;     /* PCI_ANY_CLASS = wildcard */
    uint8_t  prog_if;      /* PCI_ANY_CLASS = wildcard */
};

/* Match-table terminator: all wildcards is meaningless (every device
 * would match), so we use it as the sentinel. Drivers MUST end their
 * tables with PCI_MATCH_END. */
#define PCI_MATCH_END  { PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_CLASS, \
                         PCI_ANY_CLASS, PCI_ANY_CLASS }

struct pci_driver {
    const char             *name;       /* "blk_ata", "e1000", ... */
    const struct pci_match *matches;    /* PCI_MATCH_END-terminated */
    int  (*probe) (struct pci_dev *dev);   /* 0 = bound, <0 = next driver */
    void (*remove)(struct pci_dev *dev);   /* may be NULL */

    /* M29B fallback hint. Drivers can OR in any of:
     *   PCI_DRIVER_GENERIC -- this is a class-only fallback (e.g.
     *                         blk_ata or a synthetic generic_*).
     *                         pci_bind reports them as
     *                         ABI_DRVMATCH_GENERIC instead of CLASS.
     * Drivers can leave this zero; the default classification is
     * EXACT (vendor+device match) or CLASS (class wildcard match). */
    uint32_t                flags;

    /* M29B test-only: set non-zero by drvmatch_disable_pci() to
     * suppress this driver in subsequent rebind passes. Production
     * boots leave this zero. */
    uint8_t                 _disabled;

    /* Internal: linked-list head walked by pci_bind_drivers. Drivers
     * leave this zero. */
    struct pci_driver      *_next;
};

/* Bit values for pci_driver.flags. */
#define PCI_DRIVER_GENERIC  0x1u   /* class-only fallback driver  */

/* ------------ public API ------------ */

/* Low-level config-space accessors. Same semantics as before. */
uint32_t pci_cfg_read32 (uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off);
uint16_t pci_cfg_read16 (uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off);
uint8_t  pci_cfg_read8  (uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off);
void     pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off,
                         uint32_t val);
void     pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off,
                         uint16_t val);

/* Returns true if any function exists at (bus,slot,fn). */
bool pci_present(uint8_t bus, uint8_t slot, uint8_t fn);

/* OR `bits` into the Command register. Used to flip BME / MEM / IO on. */
void pci_enable(uint8_t bus, uint8_t slot, uint8_t fn, uint16_t bits);

/* Same, addressed through a struct pci_dev. */
void pci_dev_enable(struct pci_dev *dev, uint16_t bits);

/* Resolve a memory BAR to its physical base by reading config space
 * (legacy interface kept for ad-hoc callers; new code uses dev->bar[i]). */
uint64_t pci_bar_addr(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t bar_off);

/* Map an MMIO BAR into kernel virt and return the cooked pointer. The
 * BAR is mapped in the HHDM window with VMM_WRITE | VMM_NX | flags
 * (NOCACHE always implied) so that BARs lying in a Limine RESERVED
 * region (which is the case for QEMU's PCI aperture) are reachable.
 *
 * The map covers `bytes` rounded up to a page; if `bytes` is 0 the
 * whole probed BAR size (dev->bar_size[idx]) is mapped. Returns NULL
 * for I/O-space BARs, empty BARs, or vmm_map failure. */
void *pci_map_bar(struct pci_dev *dev, int idx, size_t bytes);

/* Locate the FIRST device matching (vendor, device); pass PCI_ANY_ID
 * for "don't care". Returns NULL if none. Used by leftover ad-hoc
 * lookups during the milestone-21 transition; new drivers should use
 * a struct pci_driver instead. */
struct pci_dev *pci_find_dev(uint16_t vendor, uint16_t device);

/* PCI capability-list walker. Used by virtio (which keeps its
 * COMMON_CFG / NOTIFY_CFG / ISR_CFG / DEVICE_CFG pointers as a chain
 * of vendor-specific caps), by xHCI (extended caps), and eventually
 * by MSI-X. Returns the cfg-space offset of the first cap, or 0 if
 * the device has no cap list. Iterate via pci_cap_next(). */
uint8_t pci_cap_first(struct pci_dev *dev);
uint8_t pci_cap_next (struct pci_dev *dev, uint8_t prev_off);

/* Find the FIRST capability of `cap_id` in the device's cap list and
 * return its config-space offset, or 0 if not present. Handy for
 * lookups like "where is this device's MSI cap?" without writing the
 * walk loop everywhere. */
uint8_t pci_find_capability(struct pci_dev *dev, uint8_t cap_id);

/* ============================================================== */
/* MSI / MSI-X enablement                                          */
/* ============================================================== */
/*
 * Both helpers assume the caller has:
 *   1. allocated IDT vector(s) via irq_alloc_vector() and registered
 *      handlers there (the device will fire those vectors directly --
 *      no PIC, no IO APIC re-route involved);
 *   2. set bus-mastering on the device (PCI_CMD_BUS_MASTER), since
 *      MSI delivery is a posted memory write upstream from the device.
 *
 * Both helpers also disable INTx (set PCI_CMD_INT_DISABLE) so the
 * device can't fire its legacy INTx pin while MSI is enabled --
 * matches what every other OS does and avoids a class of "double
 * IRQ" surprises during the hand-off.
 */

/* Single-vector MSI on a device that has the legacy MSI capability
 * (cap id 0x05). All `count` messages programmed by the device target
 * the same `vector` on the same `lapic_id` (Multiple Message Enable
 * forced to 0 = 1 vector total).
 *
 * Address-low layout: 0xFEE00000 | (lapic_id << 12)  (RH=0, DM=0)
 * Data layout:        vector | (0 << 8 fixed delivery)
 *                            | (0 << 14 deassert)  | (0 << 15 edge)
 *
 * Returns true if the cap was present and we successfully enabled
 * MSI; false otherwise (caller should fall back to legacy INTx). */
bool pci_msi_enable(struct pci_dev *dev, uint8_t vector, uint8_t lapic_id);

/* Multi-vector MSI-X on a device that has the MSI-X capability
 * (cap id 0x11). Programs entries [0..count-1] of the on-device MSI-X
 * table to fire `vector_base + i` on `lapic_id`, then sets
 * MSI-X Enable (and clears Function Mask). Vectors past `count-1` in
 * the device's own table are LEFT MASKED (Vector Control bit 0 = 1).
 *
 * `count` must be <= the device-reported table size. The MSI-X table
 * BAR is mapped on demand into kernel virt with NOCACHE.
 *
 * Returns true if the cap was present, the table was mapped, and the
 * vectors were programmed; false otherwise. */
bool pci_msix_enable(struct pci_dev *dev,
                     uint8_t vector_base, uint8_t lapic_id,
                     uint32_t count);

/* Bus enumeration + binding. Called once from kernel.c during boot. */
void pci_init(void);                    /* fill the device table        */
void pci_register_driver(struct pci_driver *drv);   /* add to registry  */
void pci_bind_drivers(void);            /* call probe() for each device */

/* Iteration -- mostly for the pci_dump diagnostic + future shell
 * commands. Returns the table size after pci_init. */
size_t           pci_device_count(void);
struct pci_dev  *pci_device_at(size_t idx);

/* One-line-per-device summary printed to serial. Called from pci_init
 * automatically; exposed so the shell can dump on demand. */
void pci_dump(void);

/* Compatibility shim: legacy callers (kept until every driver is
 * migrated to the registry path). Returns the first matching device
 * by vendor:device and writes its B/S/F to *out. PCI_ANY_ID = wildcard. */
bool pci_find(uint16_t vendor, uint16_t device,
              uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_fn);

#endif /* TOBYOS_PCI_H */
