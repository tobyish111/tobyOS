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
    /* Flush the DEVICE's volatile write cache to stable media. blk_write
     * returning 0 only means the drive accepted the data -- on real SSDs
     * and HDDs it may still sit in the drive's RAM cache, where an abrupt
     * power cut loses it. Durability barriers (tobyfs journal commit,
     * bcache_sync) call blk_flush() after their writes. Optional: NULL
     * means the device has no volatile cache to flush (RAM disks) or the
     * driver predates the op; blk_flush() treats NULL as success. */
    int (*flush)(struct blk_dev *dev);
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

    /* Human-readable model string from the drive's IDENTIFY / INQUIRY
     * data (ATA words 27..46, NVMe controller MN, SCSI vendor+product).
     * Filled by the disk drivers at probe time; empty for partitions,
     * wrappers, and drivers that predate the field. Shown to the user
     * by the disk-manager confirm flow so "which physical drive am I
     * about to format" is never a guess. */
    char                model[41];
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

/* Decode the model string out of a raw ATA IDENTIFY block (words
 * 27..46, chars byte-swapped within each word) into dev->model,
 * trimming the space padding. Shared by blk_ata + blk_ahci. */
void blk_model_from_ata_id(struct blk_dev *dev, const uint16_t id[256]);

/* Driver registration entry points called from kernel.c during boot.
 * Each one inserts a struct pci_driver into the bus registry. The
 * actual disk doesn't appear in blk_get_first() until pci_bind_drivers
 * has run AND the probe has succeeded -- both are no-ops on machines
 * that don't have the corresponding controller. */
void blk_ata_register(void);   /* PIIX3 IDE / compatibility-mode IDE */
void blk_ahci_register(void);  /* AHCI 1.0 SATA (q35 ICH9 / real PCH) */
void blk_nvme_register(void);  /* NVMe 1.x (PCIe SSDs, QEMU -device nvme) */

/* Opt-in NVMe self-test (called from kernel.c under -DNVME4K_BOOT):
 * round-trips aligned + sub-device-sector I/O against the first
 * registered NVMe namespace to validate the 512<->native-LBA
 * translation. Non-destructive (saves+restores the window it uses);
 * no-op SKIP if no NVMe namespace is present. */
void blk_nvme_selftest(void);

/* Opt-in AHCI NCQ self-test (called from kernel.c under -DAHCIQ_BOOT):
 * sequential rw + batched concurrent rw against the first registered
 * AHCI disk, proving multiple NCQ tags in flight with correct per-tag
 * completion. Non-destructive; SKIPs if no AHCI disk is present. */
void blk_ahci_selftest(void);
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
/* Slice 16 (cgroup io): declared locally rather than by including cgroup.h/proc.h.
 * blk.h is included by nearly every driver, and pulling proc.h in here would create
 * an include cycle for no gain -- two prototypes are the whole dependency. */
struct proc;
struct proc *current_proc(void);
void cgroup_io_charge(struct proc *p, bool write, uint64_t bytes);

/* Slice 16 (cgroup io): these two inlines are the ONE place every block operation
 * in the kernel passes through, so io.stat is charged here. A driver cannot bypass
 * the accounting without bypassing the API itself -- which is the property that
 * makes the numbers trustworthy rather than merely plausible.
 *
 * Cost: one call per block request, against an operation that already costs
 * thousands of cycles and a DMA round trip. There is no measurable hot path here,
 * which is why io accounting needed none of the care the memory charge did.
 *
 * Charged only on SUCCESS -- a failed read moved no bytes, and counting it would
 * make io.stat disagree with the device's own counters after an unplug. */
static inline int blk_read(struct blk_dev *d, uint64_t lba, uint32_t n, void *b) {
    if (!d || d->gone) return -1;
    int r = d->ops->read(d, lba, n, b);
    if (r >= 0) cgroup_io_charge(current_proc(), false, (uint64_t)n * BLK_SECTOR_SIZE);
    return r;
}
static inline int blk_write(struct blk_dev *d, uint64_t lba, uint32_t n, const void *b) {
    if (!d || d->gone) return -1;
    if (!d->ops->write) return -1;
    int r = d->ops->write(d, lba, n, b);
    if (r >= 0) cgroup_io_charge(current_proc(), true, (uint64_t)n * BLK_SECTOR_SIZE);
    return r;
}
static inline int blk_flush(struct blk_dev *d) {
    if (!d || d->gone) return -1;
    if (!d->ops->flush) return 0;   /* no volatile cache -- trivially durable */
    return d->ops->flush(d);
}

/* ---- async block I/O (command queuing) ---------------------------
 *
 * Drivers that keep multiple commands in flight (NVMe multi-CID, AHCI
 * NCQ) split each blk_read/blk_write into one or more `struct blk_io`
 * hardware operations (each <= the driver's per-op cap), submit them all
 * to the hardware queue, then wait for each. The public blk_read/write
 * signatures stay synchronous; the queuing happens underneath.
 *
 * Completion is delivered two ways, and the wait tolerates both:
 *   - the device completion IRQ (MSI) calls blk_io_complete(); or
 *   - the caller's `poll` reaper drains finished tags (used when MSI is
 *     unavailable, or when interrupts are masked at the wait point).
 *
 * `blk_io_wait` cooperatively yields between polls *when it is safe to*
 * (scheduler up, interrupts enabled, a current proc exists) so that other
 * processes -- and their own I/O -- make progress while this op is in
 * flight; that overlap is what keeps the hardware queue full. When it
 * cannot yield (early boot, or a spinlock is held, e.g. the bcache flush
 * paths) it busy-polls the hardware exactly as the old synchronous path
 * did, so it is never worse than single-outstanding-command. */

#define BLK_IO_PENDING  1     /* status while a blk_io is in flight */

struct blk_io {
    uint64_t        lba;        /* device/native LBA of this op        */
    uint32_t        count;      /* sectors in this op                  */
    void           *buf;        /* kernel buffer                       */
    bool            is_write;
    volatile int    status;     /* BLK_IO_PENDING -> 0 (ok) / <0 (err) */
    volatile bool   done;       /* set by blk_io_complete              */
    void           *drv;        /* driver-private cookie (tag, ctrl)   */
    struct blk_io  *next;       /* driver free-list / chaining          */
};

static inline void blk_io_prep(struct blk_io *io, uint64_t lba, uint32_t count,
                               void *buf, bool is_write) {
    io->lba = lba; io->count = count; io->buf = buf; io->is_write = is_write;
    io->status = BLK_IO_PENDING; io->done = false; io->drv = 0; io->next = 0;
}

/* Mark `io` finished with `status` (0 ok, <0 error). Safe from IRQ
 * context: a plain release store, no locks, no scheduler calls. */
void blk_io_complete(struct blk_io *io, int status);

/* Wait until `io->done`. `poll` (may be NULL) is the driver's completion
 * reaper, invoked with `poll_ctx` each iteration to harvest finished tags
 * on the no-IRQ / IRQs-masked path. Returns io->status. */
int  blk_io_wait(struct blk_io *io, void (*poll)(void *), void *poll_ctx);

/* Called once by the kernel after the scheduler + drivers are fully up,
 * enabling blk_io_wait's cooperative-yield path. Before this, all waits
 * busy-poll (early-boot I/O is single-threaded anyway). */
void blk_set_yield_ready(void);

#endif /* TOBYOS_BLK_H */
