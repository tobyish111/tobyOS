/* drvdb.h -- Milestone 35A: driver knowledge base.
 *
 * A small, static catalogue of PCI vendor:device IDs and USB
 * class/subclass/protocol triples that tobyOS *recognises*. This is
 * deliberately separate from the per-driver pci_match tables -- the
 * goal here is to be able to say "we know what this chip is, even if
 * we don't have a driver for it" so the operator gets a meaningful
 * line in `hwinfo`/`hwcompat` instead of "0x1234:0x5678 unknown".
 *
 * Each entry carries:
 *   - a friendly name ("Intel 82574L Gigabit Ethernet")
 *   - the recommended in-tree driver name, or NULL if none
 *   - a support tier:
 *
 *       DRVDB_SUPPORTED   the in-tree driver is expected to bind and
 *                         work for this device
 *       DRVDB_PARTIAL     a driver exists but only covers a subset of
 *                         the device's capabilities (e.g. virtio-net
 *                         without MRG_RXBUF/multiqueue)
 *       DRVDB_UNSUPPORTED we know about the device but don't claim it
 *                         today; binding will be skipped with a clear
 *                         "[drvmatch] UNSUPPORTED ..." log line
 *
 * The database is read-only; querying never allocates and never
 * blocks. It's safe to call from any kernel context.
 *
 * Used by:
 *   - drvmatch.c   -- to populate the human-readable `reason` field
 *                     and to mark explicitly-unsupported devices
 *   - hwcompat.c   -- (M35D) to label every detected device with a
 *                     stable status tier in the compatibility DB
 *   - kernel.c     -- to print friendly chip names in boot messages
 */

#ifndef TOBYOS_DRVDB_H
#define TOBYOS_DRVDB_H

#include <tobyos/types.h>

/* Support tiers. Stable; new tiers go on the end. */
#define DRVDB_UNKNOWN      0u   /* no record at all (default for any
                                 * VID:DID we've never heard of) */
#define DRVDB_SUPPORTED    1u   /* in-tree driver expected to work */
#define DRVDB_PARTIAL      2u   /* in-tree driver covers a subset only */
#define DRVDB_UNSUPPORTED  3u   /* we know about it; intentionally not
                                 * claimed by any driver at present */

struct drvdb_pci_entry {
    uint16_t    vendor;
    uint16_t    device;
    const char *friendly;     /* NUL-terminated, never NULL */
    const char *driver;       /* in-tree driver name, may be NULL */
    uint32_t    tier;         /* one of DRVDB_* */
};

struct drvdb_usb_entry {
    uint8_t     class_code;
    uint8_t     subclass;     /* 0xFF = wildcard inside a class */
    uint8_t     protocol;     /* 0xFF = wildcard inside subclass */
    const char *friendly;
    const char *driver;
    uint32_t    tier;
};

/* PCI lookup. Returns the matching entry or NULL. The returned
 * pointer is valid for the lifetime of the kernel image. */
const struct drvdb_pci_entry *drvdb_pci_lookup(uint16_t vendor,
                                               uint16_t device);

/* Convenience: friendly name for a PCI ID, or "unknown" if there's
 * no record. Always returns a non-NULL, NUL-terminated string. */
const char *drvdb_pci_name(uint16_t vendor, uint16_t device);

/* Recommended driver name for a PCI ID, or NULL if either:
 *   - we have no record, OR
 *   - we recognise the device but don't ship a driver for it. */
const char *drvdb_pci_driver_hint(uint16_t vendor, uint16_t device);

/* USB lookup. Match preference: exact (class, subclass, protocol) >
 * (class, subclass, ANY) > (class, ANY, ANY). */
const struct drvdb_usb_entry *drvdb_usb_lookup(uint8_t class_code,
                                               uint8_t subclass,
                                               uint8_t protocol);

const char *drvdb_usb_name(uint8_t class_code,
                           uint8_t subclass,
                           uint8_t protocol);

const char *drvdb_usb_driver_hint(uint8_t class_code,
                                  uint8_t subclass,
                                  uint8_t protocol);

/* Stable string for a tier (for log lines and userland tools). */
const char *drvdb_tier_name(uint32_t tier);

/* Diagnostic dumps. */
size_t drvdb_pci_count(void);
size_t drvdb_usb_count(void);
const struct drvdb_pci_entry *drvdb_pci_at(size_t idx);
const struct drvdb_usb_entry *drvdb_usb_at(size_t idx);

#endif /* TOBYOS_DRVDB_H */
