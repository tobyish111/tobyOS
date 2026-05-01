/* drvdb.c -- Milestone 35A: driver knowledge base, static catalogue.
 *
 * Pure data + lookup helpers. No allocation, no global state, no
 * locking required. Adding a new device is a one-line table change
 * here -- pci.c / xhci.c remain untouched unless we also gain a
 * driver for it.
 *
 * The catalogue is intentionally narrow: every entry corresponds to
 * something tobyOS either already drives, would like to drive next,
 * or has consciously decided to skip. We don't try to mirror Linux's
 * full pci.ids -- the goal is operator-readable diagnostics, not
 * complete coverage.
 */

#include <tobyos/drvdb.h>
#include <tobyos/klibc.h>

/* ============================================================
 * PCI catalogue
 * ============================================================ */

static const struct drvdb_pci_entry pci_table[] = {
    /* QEMU virtio family (the workhorse for every VM-only test).
     * Modern (1.x) IDs are 0x1040..0x107F; legacy IDs are 0x1000..0x103F.
     * Where we ship both, we list both so the entries appear in
     * hwcompat regardless of which QEMU machine type is in use. */
    { 0x1AF4, 0x1000, "virtio-net (legacy)",  "virtio-net", DRVDB_SUPPORTED },
    { 0x1AF4, 0x1041, "virtio-net (modern)",  "virtio-net", DRVDB_SUPPORTED },
    { 0x1AF4, 0x1001, "virtio-blk (legacy)",  "virtio-blk", DRVDB_SUPPORTED },
    { 0x1AF4, 0x1042, "virtio-blk (modern)",  "virtio-blk", DRVDB_SUPPORTED },
    { 0x1AF4, 0x1004, "virtio-scsi (legacy)", NULL,          DRVDB_UNSUPPORTED },
    { 0x1AF4, 0x1048, "virtio-scsi (modern)", NULL,          DRVDB_UNSUPPORTED },
    { 0x1AF4, 0x1050, "virtio-gpu",           "virtio-gpu", DRVDB_PARTIAL   },
    { 0x1AF4, 0x1052, "virtio-input (modern)", NULL,        DRVDB_UNSUPPORTED },

    /* Intel Ethernet (e1000 / e1000e generations). */
    { 0x8086, 0x100E, "Intel 82540EM Gigabit",     "e1000",  DRVDB_SUPPORTED },
    { 0x8086, 0x10D3, "Intel 82574L Gigabit",      "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x10EA, "Intel 82577LM Gigabit",     "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x10EB, "Intel 82577LC Gigabit",     "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x1502, "Intel 82579LM Gigabit",     "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x1503, "Intel 82579V  Gigabit",     "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x153A, "Intel I217-LM",             "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x153B, "Intel I217-V",              "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x1559, "Intel I218-V",              "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x155A, "Intel I218-LM",             "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x156F, "Intel I219-LM",             "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x1570, "Intel I219-V",              "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15B7, "Intel I219-V (Kaby PCH)",   "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15B8, "Intel I219-LM (Kaby PCH)",  "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15B9, "Intel I219-LM (Lewisburg)", "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15A0, "Intel I218-LM",             "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15A1, "Intel I218-V",              "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15A2, "Intel I218-LM (Wildcat)",   "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15A3, "Intel I218-V (Wildcat)",    "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15D7, "Intel I219-LM (CNP)",       "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15D8, "Intel I219-V (CNP)",        "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15E3, "Intel I219-LM (CNP)",       "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15D6, "Intel I219-V (CNP)",        "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15BD, "Intel I219-LM (400 PCH)",   "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15BE, "Intel I219-V (400 PCH)",    "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15FB, "Intel I219-LM (TGP)",       "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x15FC, "Intel I219-V (TGP)",        "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x1A1E, "Intel I219-LM (ADP)",       "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x1A1F, "Intel I219-V (ADP)",        "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x0D4E, "Intel I219-LM (CMP)",       "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x550A, "Intel I219-LM (MTP)",       "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x10F6, "Intel 82574LA",             "e1000e", DRVDB_SUPPORTED },
    { 0x8086, 0x150C, "Intel 82583V",              "e1000e", DRVDB_SUPPORTED },
    /* Intel 2.5 GbE (Linux igc) — different MAC from e1000e; no driver yet. */
    { 0x8086, 0x15F2, "Intel I225-LM (2.5GbE)",    NULL,      DRVDB_UNSUPPORTED },
    { 0x8086, 0x15F3, "Intel I225-V (2.5GbE)",     NULL,      DRVDB_UNSUPPORTED },
    { 0x8086, 0x125B, "Intel I226-LM (2.5GbE)",    NULL,      DRVDB_UNSUPPORTED },
    { 0x8086, 0x125C, "Intel I226-V (2.5GbE)",     NULL,      DRVDB_UNSUPPORTED },

    /* Realtek family. */
    { 0x10EC, 0x8168, "Realtek RTL8168",           "rtl8169", DRVDB_SUPPORTED },
    { 0x10EC, 0x8169, "Realtek RTL8169",           "rtl8169", DRVDB_SUPPORTED },
    { 0x10EC, 0x8161, "Realtek RTL8161",           "rtl8169", DRVDB_SUPPORTED },
    { 0x10EC, 0x8167, "Realtek RTL8167",           "rtl8169", DRVDB_SUPPORTED },
    { 0x10EC, 0x8139, "Realtek RTL8139",           NULL,      DRVDB_UNSUPPORTED },

    /* Wireless PCI devices (not on the Ethernet stack — informational). */
    { 0x8086, 0x2723, "Intel Wi-Fi 6 AX200",     NULL,      DRVDB_UNSUPPORTED },
    { 0x8086, 0x2725, "Intel Wi-Fi 6E AX210",    NULL,      DRVDB_UNSUPPORTED },
    { 0x10EC, 0x8852, "Realtek RTL8852BE Wi-Fi", NULL,      DRVDB_UNSUPPORTED },
    { 0x10EC, 0xC852, "Realtek RTL8822CE Wi-Fi", NULL,      DRVDB_UNSUPPORTED },

    /* QEMU PCI host / chipset bits we want named in hwcompat output. */
    { 0x8086, 0x1237, "Intel 440FX (PMC)",         NULL,      DRVDB_PARTIAL },
    { 0x8086, 0x7000, "Intel PIIX3 ISA bridge",    NULL,      DRVDB_PARTIAL },
    { 0x8086, 0x7010, "Intel PIIX3 IDE",           "blk_ata", DRVDB_SUPPORTED },
    { 0x8086, 0x7113, "Intel PIIX4 PM",            NULL,      DRVDB_PARTIAL },
    { 0x8086, 0x29C0, "Intel Q35 host bridge",     NULL,      DRVDB_PARTIAL },
    { 0x8086, 0x2918, "Intel ICH9 LPC",            NULL,      DRVDB_PARTIAL },
    { 0x8086, 0x2922, "Intel ICH9 AHCI",           "blk_ahci", DRVDB_SUPPORTED },
    { 0x8086, 0x2934, "Intel ICH9 USB UHCI",       NULL,      DRVDB_UNSUPPORTED },

    /* QEMU bespoke PCI IDs. */
    { 0x1B36, 0x000D, "QEMU XHCI controller",      "xhci",    DRVDB_SUPPORTED },
    { 0x1B36, 0x0010, "QEMU NVMe controller",      "blk_nvme", DRVDB_SUPPORTED },
    { 0x1B36, 0x0008, "QEMU PCIe host bridge",     NULL,      DRVDB_PARTIAL },

    /* HD Audio: M26F brings the controller up but doesn't ship a
     * full codec/path implementation, so this is PARTIAL. */
    { 0x8086, 0x2668, "Intel ICH6 HDA",            "audio_hda", DRVDB_PARTIAL },
    { 0x8086, 0x293E, "Intel ICH9 HDA",            "audio_hda", DRVDB_PARTIAL },
};

/* ============================================================
 * USB catalogue
 * ============================================================ */

static const struct drvdb_usb_entry usb_table[] = {
    /* HID -- boot subclass, the only one xhci_finalize binds today. */
    { 0x03, 0x01, 0x01, "USB HID Keyboard (Boot)", "usb_hid", DRVDB_SUPPORTED },
    { 0x03, 0x01, 0x02, "USB HID Mouse (Boot)",    "usb_hid", DRVDB_SUPPORTED },
    /* HID -- non-boot subclass. We log + leave unbound for now. */
    { 0x03, 0x00, 0xFF, "USB HID (Report-only)",   NULL,      DRVDB_PARTIAL },

    /* Mass storage. We support BBB/SCSI; the rest is logged. */
    { 0x08, 0x06, 0x50, "USB Mass Storage (BBB/SCSI)", "usb_msc", DRVDB_SUPPORTED },
    { 0x08, 0x05, 0x50, "USB Mass Storage (BBB/ATAPI)", NULL,     DRVDB_UNSUPPORTED },
    { 0x08, 0x06, 0x00, "USB Mass Storage (CBI/SCSI)",  NULL,     DRVDB_UNSUPPORTED },
    { 0x08, 0xFF, 0xFF, "USB Mass Storage (variant)",   NULL,     DRVDB_UNSUPPORTED },

    /* Hubs. */
    { 0x09, 0xFF, 0xFF, "USB Hub", "usb_hub", DRVDB_SUPPORTED },

    /* Other top-level classes we recognise but explicitly do NOT
     * drive yet. Listed so hwcompat can label them clearly. */
    { 0x01, 0xFF, 0xFF, "USB Audio",              NULL, DRVDB_UNSUPPORTED },
    { 0x02, 0xFF, 0xFF, "USB Communications/CDC", NULL, DRVDB_UNSUPPORTED },
    { 0x05, 0xFF, 0xFF, "USB Physical Interface", NULL, DRVDB_UNSUPPORTED },
    { 0x06, 0xFF, 0xFF, "USB Image (PTP)",        NULL, DRVDB_UNSUPPORTED },
    { 0x07, 0xFF, 0xFF, "USB Printer",            NULL, DRVDB_UNSUPPORTED },
    { 0x0A, 0xFF, 0xFF, "USB CDC-Data",           NULL, DRVDB_UNSUPPORTED },
    { 0x0B, 0xFF, 0xFF, "USB Smart Card",         NULL, DRVDB_UNSUPPORTED },
    { 0x0E, 0xFF, 0xFF, "USB Video",              NULL, DRVDB_UNSUPPORTED },
    { 0xE0, 0xFF, 0xFF, "USB Wireless Controller", NULL, DRVDB_UNSUPPORTED },
    { 0xEF, 0xFF, 0xFF, "USB Miscellaneous",      NULL, DRVDB_UNSUPPORTED },
    { 0xFE, 0xFF, 0xFF, "USB Application-Specific", NULL, DRVDB_UNSUPPORTED },
    { 0xFF, 0xFF, 0xFF, "USB Vendor-Specific",    NULL, DRVDB_UNSUPPORTED },
};

#define PCI_TABLE_LEN  (sizeof(pci_table) / sizeof(pci_table[0]))
#define USB_TABLE_LEN  (sizeof(usb_table) / sizeof(usb_table[0]))

/* ============================================================
 * PCI lookup
 * ============================================================ */

const struct drvdb_pci_entry *drvdb_pci_lookup(uint16_t vendor,
                                               uint16_t device) {
    for (size_t i = 0; i < PCI_TABLE_LEN; i++) {
        if (pci_table[i].vendor == vendor &&
            pci_table[i].device == device) {
            return &pci_table[i];
        }
    }
    return NULL;
}

const char *drvdb_pci_name(uint16_t vendor, uint16_t device) {
    const struct drvdb_pci_entry *e = drvdb_pci_lookup(vendor, device);
    return e ? e->friendly : "unknown";
}

const char *drvdb_pci_driver_hint(uint16_t vendor, uint16_t device) {
    const struct drvdb_pci_entry *e = drvdb_pci_lookup(vendor, device);
    return (e && e->driver) ? e->driver : NULL;
}

/* ============================================================
 * USB lookup -- prefer most-specific match
 * ============================================================ */

const struct drvdb_usb_entry *drvdb_usb_lookup(uint8_t class_code,
                                               uint8_t subclass,
                                               uint8_t protocol) {
    const struct drvdb_usb_entry *best = NULL;
    int best_score = -1;
    for (size_t i = 0; i < USB_TABLE_LEN; i++) {
        const struct drvdb_usb_entry *e = &usb_table[i];
        if (e->class_code != class_code) continue;

        int score = 0;
        if (e->subclass == 0xFF) {
            /* class-only wildcard */
        } else if (e->subclass == subclass) {
            score += 2;
        } else {
            continue;
        }
        if (e->protocol == 0xFF) {
            /* subclass-only */
        } else if (e->protocol == protocol) {
            score += 1;
        } else {
            continue;
        }

        if (score > best_score) {
            best = e;
            best_score = score;
        }
    }
    return best;
}

const char *drvdb_usb_name(uint8_t class_code,
                           uint8_t subclass,
                           uint8_t protocol) {
    const struct drvdb_usb_entry *e =
        drvdb_usb_lookup(class_code, subclass, protocol);
    return e ? e->friendly : "unknown USB device";
}

const char *drvdb_usb_driver_hint(uint8_t class_code,
                                  uint8_t subclass,
                                  uint8_t protocol) {
    const struct drvdb_usb_entry *e =
        drvdb_usb_lookup(class_code, subclass, protocol);
    return (e && e->driver) ? e->driver : NULL;
}

/* ============================================================
 * Tier name + iteration
 * ============================================================ */

const char *drvdb_tier_name(uint32_t tier) {
    switch (tier) {
    case DRVDB_SUPPORTED:   return "supported";
    case DRVDB_PARTIAL:     return "partial";
    case DRVDB_UNSUPPORTED: return "unsupported";
    default:                return "unknown";
    }
}

size_t drvdb_pci_count(void) { return PCI_TABLE_LEN; }
size_t drvdb_usb_count(void) { return USB_TABLE_LEN; }

const struct drvdb_pci_entry *drvdb_pci_at(size_t idx) {
    return (idx < PCI_TABLE_LEN) ? &pci_table[idx] : NULL;
}

const struct drvdb_usb_entry *drvdb_usb_at(size_t idx) {
    return (idx < USB_TABLE_LEN) ? &usb_table[idx] : NULL;
}
