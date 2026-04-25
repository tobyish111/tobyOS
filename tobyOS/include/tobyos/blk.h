/* blk.h -- block-device interface (milestone 6 + 21 + 23A).
 *
 * Layered model
 * -------------
 *
 *   Filesystem driver  (tobyfs / fat32 / ext4 / ...) -- M23B+
 *           |
 *           v
 *   struct blk_dev      <-- THIS LAYER. Uniform LBA read/write API.
 *           |
 *           +-- BLK_CLASS_DISK       (whole disk -- ide0:master, ahci0:p0,
 *           |                         nvme0:n1, usb0:lun0, ...)
 *           +-- BLK_CLASS_PARTITION  (region inside a parent disk --
 *           |                         "ide0:master.p1", "nvme0:n1.p2", ...
 *           |                         created by the partition layer
 *           |                         from a GPT entry)
 *           +-- BLK_CLASS_WRAPPER    (anonymous offset wrapper -- legacy
 *                                     installer compat layer)
 *
 * Sector size is fixed at BLK_SECTOR_SIZE = 512. tobyfs uses 4 KiB
 * blocks built from 8 sectors.
 *
 * Filesystem drivers consume `struct blk_dev *` and never look at the
 * class field -- a partition is byte-for-byte equivalent to a smaller
 * disk that happens to start at LBA 0. This is the key M23 invariant
 * that lets us drop FAT32 / ext4 onto either whole disks or GPT
 * partitions with no driver-side changes.
 */

#ifndef TOBYOS_BLK_H
#define TOBYOS_BLK_H

#include <tobyos/types.h>

#define BLK_SECTOR_SIZE 512u

/* Maximum length of a partition label decoded from GPT (UTF-16LE name
 * field is 36 chars; ASCII transcode fits in <= 36 bytes + NUL). */
#define BLK_PART_LABEL_MAX 37
#define BLK_GUID_BYTES     16

struct blk_dev;

struct blk_ops {
    /* Read `count` sectors starting at LBA `lba` into `buf`.
     * Returns 0 on success, negative errno-ish on failure. */
    int (*read) (struct blk_dev *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blk_dev *dev, uint64_t lba, uint32_t count,
                 const void *buf);
};

/* Device class -- set by the registering layer. Disks come from PCI
 * driver probes (blk_ata / blk_ahci / blk_nvme / future blk_usb_msc).
 * Partitions are created by the partition layer (src/partition.c)
 * after parsing a GPT off a disk. Wrappers are used by the milestone-20
 * installer and are NOT linked to a parent disk in the partition
 * registry. */
enum blk_dev_class {
    BLK_CLASS_DISK      = 1,
    BLK_CLASS_PARTITION = 2,
    BLK_CLASS_WRAPPER   = 3,
};

struct blk_dev {
    const char         *name;
    const struct blk_ops *ops;
    uint64_t            sector_count;   /* total LBAs available */
    void               *priv;

    /* ---- M23A additions: device-class metadata ----
     *
     * These fields are zero-initialised by the existing register sites
     * (which use `static struct blk_dev g_dev = { .name = ... };`).
     * blk_register() upgrades a zero `class` to BLK_CLASS_DISK so older
     * driver code keeps registering disks correctly without source
     * changes. The partition layer fills these in explicitly when it
     * creates a partition slice. */
    enum blk_dev_class  class;
    struct blk_dev     *parent;          /* DISK for a partition; 0 for a disk */
    uint64_t            offset_lba;      /* offset into parent (partition only) */
    uint32_t            partition_index; /* 1-based GPT slot index (partition only) */
    uint8_t             type_guid[BLK_GUID_BYTES];   /* GPT type GUID */
    char                partition_label[BLK_PART_LABEL_MAX];

    /* ---- M26E additions: removable-storage lifecycle ----
     *
     * `gone` is flipped to true by the owning driver (today: usb_msc on
     * detach) before it nulls its back-pointers. blk_read/blk_write
     * short-circuit to -1 (EIO-shape) on any device flagged gone, so
     * a still-mounted FS that loops doing reads after the cable was
     * yanked never touches a freed driver state. The blk_dev itself
     * stays in the registry until the FS unmounts: nothing in the
     * tobyOS heap path supports unregister yet, and leaving the dead
     * record visible lets `devlist blk` and `usbtest storage` report
     * "removed" instead of silently disappearing. */
    bool                gone;
};

/* ---- block-device registry (milestone 21, expanded in 23A) -------
 *
 * Drivers (blk_ata, blk_ahci, blk_nvme, ...) call blk_register() from
 * inside their PCI probe; the partition layer calls it for every
 * partition it carves out of a disk. Disks and partitions both live in
 * the same flat registry; consumers filter by class via blk_get_class()
 * if they care.
 *
 * The registry stores raw pointers -- registered devices must have
 * static OR heap lifetime that outlives the registry (in practice the
 * heap allocations live forever -- we never unregister).
 */

#define BLK_MAX_DEVICES 16   /* M23A: up from 8 -- partitions can quickly
                              *       outnumber the underlying disks */

void   blk_register(struct blk_dev *dev);
struct blk_dev *blk_get_first(void);
struct blk_dev *blk_find(const char *name);
size_t blk_count(void);
struct blk_dev *blk_get(size_t idx);

/* M23A helpers -- iterate by class. Pass class=0 to walk all entries.
 * Returns NULL when `cookie` reaches the end of the registry. Caller
 * passes a pointer-to-size_t that we mutate to track position. Usage:
 *
 *     size_t it = 0;
 *     struct blk_dev *d;
 *     while ((d = blk_iter_next(&it, BLK_CLASS_DISK)) != NULL) {
 *         partition_scan_disk(d);
 *     } */
struct blk_dev *blk_iter_next(size_t *cookie, enum blk_dev_class class);

/* Convenience: first DISK / first PARTITION -- preferred over
 * blk_get_first() once partitions exist, because blk_get_first()
 * still returns whatever was registered first (could be a partition
 * carved from a disk that shows up earlier in the list). */
struct blk_dev *blk_first_disk(void);
struct blk_dev *blk_first_partition(void);

/* Diagnostic: one-line-per-device summary printed to serial. The M23A
 * version groups partitions under their parent disk for readability. */
void blk_dump(void);

/* Driver registration entry points called from kernel.c during boot.
 * Each one inserts a struct pci_driver into the bus registry. The
 * actual disk doesn't appear in blk_get_first() until pci_bind_drivers
 * has run AND the probe has succeeded -- both are no-ops on machines
 * that don't have the corresponding controller. */
void blk_ata_register(void);   /* PIIX3 IDE / compatibility-mode IDE */
void blk_ahci_register(void);  /* AHCI 1.0 SATA (q35 ICH9 / real PCH) */
void blk_nvme_register(void);  /* NVMe 1.x (PCIe SSDs, QEMU -device nvme) */
void virtio_blk_register(void); /* M35B: modern virtio-blk-pci         */

/* M35B: lightweight introspection (NULL/0/false if no virtio-blk bound). */
const char *virtio_blk_name(void);
uint64_t    virtio_blk_capacity_lba(void);
bool        virtio_blk_present(void);

/* Milestone 20: offset wrapper. Creates a "virtual" block device whose
 * LBA 0 maps to LBA `offset_lba` on `parent`, and whose reported size
 * is `size_lba` sectors (clamped to what the parent actually has).
 *
 * Used by:
 *   - the installer to carve a raw disk into:
 *       sectors 0..BOOT_RESERVED-1   -> Limine-bootable image
 *       sectors BOOT_RESERVED..end   -> tobyfs "/data" partition
 *     without a real partition table (legacy path; class=WRAPPER).
 *   - the partition layer to expose every GPT entry as a block device
 *     (modern path; class=PARTITION, partition fields populated).
 *
 * The returned device + priv buffer are kmalloc'd; callers own them.
 * In practice they live forever on the kernel heap -- there is no
 * blk_unregister yet. */
struct blk_dev *blk_offset_wrap(struct blk_dev *parent,
                                uint64_t offset_lba,
                                uint64_t size_lba,
                                const char *name);

/* M23A: like blk_offset_wrap, but tags the result as a real partition
 * with the supplied metadata (1-based index, type-guid copy, ASCII
 * label). Caller still owns the result; partition layer registers it
 * with blk_register() after this returns. */
struct blk_dev *blk_partition_wrap(struct blk_dev *parent,
                                   uint64_t offset_lba,
                                   uint64_t size_lba,
                                   const char *name,
                                   uint32_t partition_index,
                                   const uint8_t type_guid[BLK_GUID_BYTES],
                                   const char *label);

/* M26E: mark a disk + every partition that lives on it as no longer
 * backed by hardware. After this returns, blk_read/blk_write on any of
 * those records short-circuit to -1 (EIO shape) without touching the
 * driver. The blk_dev structures themselves stay registered so the
 * partition layer's parent pointers and any in-flight FS handles do
 * not dangle. Safe to call from a teardown path -- never blocks. */
void blk_mark_gone(struct blk_dev *disk);

/* Convenience wrappers -- keep call sites short.
 *
 * M26E: both inline wrappers fail-fast with -1 once `gone` flips, so
 * a stale FAT32 mount that survives an unplug returns clean EIO codes
 * instead of dereferencing a freed driver state. The check is two
 * loads + one branch; harmless on the steady-state path. */
static inline int blk_read(struct blk_dev *d, uint64_t lba, uint32_t n, void *b) {
    if (!d || d->gone) return -1;
    return d->ops->read(d, lba, n, b);
}
static inline int blk_write(struct blk_dev *d, uint64_t lba, uint32_t n, const void *b) {
    if (!d || d->gone) return -1;
    if (!d->ops->write) return -1;
    return d->ops->write(d, lba, n, b);
}

#endif /* TOBYOS_BLK_H */
