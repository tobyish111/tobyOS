/* usb_msc.h -- USB Mass Storage Class driver (Bulk-Only Transport,
 *              SCSI subclass).
 *
 * Milestone 23C. Sits between the xHCI host-controller driver
 * (src/xhci.c) and the block-device layer (struct blk_dev), turning a
 * single-LUN USB stick into something the partition + FAT32 layers can
 * mount.
 *
 *      enumerate_port(xHCI)
 *           |
 *           +-- usb_msc_probe(dev, iface, ep_in, ep_out)
 *                  |
 *                  +-- xhci_configure_bulk_endpoints (one Configure-EP
 *                  |                                  TRB for both)
 *                  +-- BBB Reset, Get-Max-LUN
 *                  +-- SCSI INQUIRY, TEST UNIT READY,
 *                      READ CAPACITY (10), [READ/WRITE (10)]
 *                  +-- partition_scan_disk(blk_dev) -- M23A path
 *                      kicks in for any GPT-partitioned stick.
 *
 * Scope (deliberate, BBB + single LUN only):
 *
 *   - exactly one MSC device per probe (LUN 0).
 *   - BBB transport, dCBWDataTransferLength up to one cluster
 *     (FAT32 4 KiB) per CBW. Larger filesystem reads are split by
 *     the FAT32 driver.
 *   - SCSI commands: TEST UNIT READY, INQUIRY, READ CAPACITY (10),
 *     READ (10), WRITE (10). No mode pages, no PREVENT_ALLOW_MEDIUM,
 *     no synchronize cache. QEMU's usb-storage tolerates this.
 *
 * The driver takes ownership of dev->msc_state. xhci_recover_stall()
 * is invoked on STALL during the data phase or CSW-IN read; if both
 * stall recovery and a Bulk-Only Mass Storage Reset fail, the device
 * is given up on for that probe and never registers a blk_dev.
 */

#ifndef TOBYOS_USB_MSC_H
#define TOBYOS_USB_MSC_H

#include <tobyos/types.h>
#include <tobyos/usb.h>

/* Try to claim a Mass-Storage / BBB / SCSI interface. The xHCI
 * descriptor walker calls this after pre-collecting the bulk-IN +
 * bulk-OUT endpoint pair belonging to `iface`. On success we:
 *   - configure both bulk endpoints in the device context,
 *   - issue Get-Max-LUN (defaults to 0 on STALL),
 *   - run INQUIRY + TEST UNIT READY + READ CAPACITY (10) on LUN 0,
 *   - register a `struct blk_dev` named "usbN" and trigger
 *     partition_scan_disk on it from inside the probe.
 *
 * Returns true if and only if the device is now usable as a block
 * device (registered + sector_count known). False is returned for
 * any pre-block-layer failure -- the device is still in Configured
 * state on the bus but does not appear in blk_get_first(). */
bool usb_msc_probe(struct usb_device *dev,
                   const struct usb_iface_desc *iface,
                   const struct usb_endpoint_desc *ep_in,
                   const struct usb_endpoint_desc *ep_out);

/* M26C: drop any MSC state owned by `dev`. The `gone` flag on the
 * underlying `struct blk_dev` is set so future I/O returns -EIO instead
 * of trying to talk to a freed slot. The blk_dev itself stays
 * registered (so any open file descriptors don't dangle); the M26E
 * mass-storage hardening pass adds full unmount-after-detach safety. */
void usb_msc_unbind(struct usb_device *dev);

/* ============================================================== */
/* M26E: introspection + selftest                                   */
/* ============================================================== */

/* Per-device telemetry counters. These are bumped from the read/write
 * fast paths and from the unbind path; userland surfaces them through
 * the BLK bus listing + the `usbtest storage` command. */
struct usb_msc_stats {
    uint64_t reads_ok;        /* successful blk_dev->read() calls */
    uint64_t reads_eio;       /* read() returned -1 (gone or stall) */
    uint64_t writes_ok;       /* successful blk_dev->write() calls */
    uint64_t writes_eio;      /* write() returned -1 (gone or stall) */
    uint64_t bytes_read;      /* total payload bytes returned to caller */
    uint64_t bytes_written;   /* total payload bytes accepted */
    uint64_t unsafe_removals; /* unbind while a FAT32 mount existed */
    uint64_t safe_removals;   /* unbind with no live mounts */
};

/* Snapshot of one bound MSC slot. All pointers belong to the driver;
 * callers must treat them as read-only and not retain across calls. */
struct usb_msc_info {
    const char         *blk_name;     /* "usb0", "usb1", ... */
    uint32_t            slot_id;      /* xHCI slot, 0 if unbound */
    uint32_t            block_size;   /* SCSI block size, bytes */
    uint64_t            block_count;  /* total LBAs */
    bool                bound;        /* udev != NULL */
    bool                gone;         /* underlying blk_dev gone flag */
    bool                mounted;      /* a vfs mount currently uses this */
    char                mount_point[64];   /* "/usb" etc., empty if none */
    struct usb_msc_stats stats;
};

/* Number of MSC slots ever populated this boot. Includes slots whose
 * device has since been detached (so usb_msc_introspect_at can still
 * report their stats). Bounded by USB_MSC_MAX_DEVICES internally. */
size_t usb_msc_count(void);

/* Fill `*out` with a snapshot of slot `idx`. Returns false if `idx` is
 * out of range. Safe to call from any context (no I/O, no allocation). */
bool   usb_msc_introspect_at(size_t idx, struct usb_msc_info *out);

/* Devtest-style selftest. Returns 0 on PASS, ABI_DEVT_SKIP when no
 * USB MSC slot is populated, or -ABI_EIO on the first invariant
 * violation. A short structured summary is written to `msg` (always
 * NUL-terminated) and a verbose per-slot trace is also kprintf'd so
 * the boot log shows the same evidence.
 *
 * Invariants checked (non-destructive):
 *   - every populated slot has a registered blk_dev with a valid name
 *     and class == BLK_CLASS_DISK,
 *   - bound slots (udev != NULL) must have gone == false,
 *   - unbound slots (udev == NULL) must have gone == true (M26C
 *     guarantees this -- see usb_msc_unbind),
 *   - each bound slot accepts a single-sector read at LBA 0. */
int    usb_msc_selftest(char *msg, size_t cap);

#endif /* TOBYOS_USB_MSC_H */
