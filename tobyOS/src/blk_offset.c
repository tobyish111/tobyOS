/* blk_offset.c -- offset wrapper block device (M20) + partition wrapper (M23A).
 *
 * Thin shim around an underlying blk_dev: translates every read/write
 * LBA by a fixed offset and clamps the visible sector range to a
 * caller-provided window. Lets a filesystem driver -- which always
 * believes block 0 of its device is the superblock -- sit at an
 * arbitrary offset on a raw disk.
 *
 * Two entry points:
 *
 *   blk_offset_wrap()    -- legacy installer compat (M20). Produces a
 *                           BLK_CLASS_WRAPPER device with no parent
 *                           link / partition metadata. Used to mount a
 *                           "data partition" that sits at a hard-coded
 *                           offset on a raw disk that has no real
 *                           partition table.
 *
 *   blk_partition_wrap() -- partition layer (M23A). Produces a
 *                           BLK_CLASS_PARTITION device with parent +
 *                           index + type-guid + ASCII label. The
 *                           transport is identical (offset translate);
 *                           only the metadata differs.
 *
 * The transport is the same in both cases -- partitions and wrappers
 * are byte-equivalent at the read/write layer. Filesystem drivers see
 * `struct blk_dev *` and never look at class.
 */

#include <tobyos/blk.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

struct blk_offset_priv {
    struct blk_dev *parent;
    uint64_t        offset_lba;
};

static int off_read(struct blk_dev *d, uint64_t lba, uint32_t count,
                    void *buf) {
    struct blk_offset_priv *p = (struct blk_offset_priv *)d->priv;
    if (lba + count > d->sector_count) return -1;
    return p->parent->ops->read(p->parent, p->offset_lba + lba, count, buf);
}

static int off_write(struct blk_dev *d, uint64_t lba, uint32_t count,
                     const void *buf) {
    struct blk_offset_priv *p = (struct blk_offset_priv *)d->priv;
    if (lba + count > d->sector_count) return -1;
    return p->parent->ops->write(p->parent, p->offset_lba + lba, count, buf);
}

static int off_flush(struct blk_dev *d) {
    struct blk_offset_priv *p = (struct blk_offset_priv *)d->priv;
    return blk_flush(p->parent);
}

static const struct blk_ops g_off_ops = {
    .read  = off_read,
    .write = off_write,
    .flush = off_flush,
};

/* Internal: do the heap allocation + plumbing. Public wrappers below
 * just decorate the result with class-specific metadata. */
static struct blk_dev *wrap_internal(struct blk_dev *parent,
                                     uint64_t offset_lba,
                                     uint64_t size_lba,
                                     const char *name,
                                     enum blk_dev_class class) {
    if (!parent) return 0;
    if (offset_lba >= parent->sector_count) {
        kprintf("[blk_offset] refusing: offset %lu >= parent size %lu\n",
                (unsigned long)offset_lba,
                (unsigned long)parent->sector_count);
        return 0;
    }
    uint64_t avail = parent->sector_count - offset_lba;
    if (size_lba == 0 || size_lba > avail) size_lba = avail;

    struct blk_offset_priv *priv = kcalloc(1, sizeof(*priv));
    struct blk_dev         *dev  = kcalloc(1, sizeof(*dev));
    if (!priv || !dev) {
        kfree(priv);
        kfree(dev);
        return 0;
    }
    priv->parent     = parent;
    priv->offset_lba = offset_lba;

    dev->name         = name ? name : "blk.part";
    dev->ops          = &g_off_ops;
    dev->sector_count = size_lba;
    dev->priv         = priv;
    dev->class        = class;
    /* Both classes record their parent + offset now. Wrappers used to
     * leave parent NULL ("anonymous"), but the provisioning guard must
     * be able to see that a legacy installer wrapper mount lives on a
     * given disk -- consumers that only want real partitions already
     * filter on class, not on parent presence. */
    dev->parent     = parent;
    dev->offset_lba = offset_lba;
    return dev;
}

struct blk_dev *blk_offset_wrap(struct blk_dev *parent,
                                uint64_t offset_lba,
                                uint64_t size_lba,
                                const char *name) {
    return wrap_internal(parent, offset_lba, size_lba, name, BLK_CLASS_WRAPPER);
}

/* A partition entry that went with its disk, given the geometry of the
 * table the disk has now (a stick written elsewhere and plugged back):
 * where it starts, how long, its number, type and label. Its parent is
 * the same disk entry, revived. The caller clears `gone` after. */
int blk_partition_rewrap(struct blk_dev *d, uint64_t offset_lba,
                         uint64_t size_lba, uint32_t partition_index,
                         const uint8_t type_guid[BLK_GUID_BYTES],
                         const char *label) {
    if (!d || d->class != BLK_CLASS_PARTITION || !d->parent) return -1;
    struct blk_offset_priv *p = (struct blk_offset_priv *)d->priv;
    struct blk_dev *parent = d->parent;
    if (!p || offset_lba >= parent->sector_count) return -1;
    uint64_t avail = parent->sector_count - offset_lba;
    if (size_lba == 0 || size_lba > avail) size_lba = avail;
    p->offset_lba = offset_lba;
    d->offset_lba = offset_lba;
    d->sector_count = size_lba;
    d->partition_index = partition_index;
    for (size_t i = 0; i < BLK_GUID_BYTES; i++)
        d->type_guid[i] = type_guid ? type_guid[i] : 0;
    size_t i = 0;
    if (label)
        for (; label[i] && i < BLK_PART_LABEL_MAX - 1; i++)
            d->partition_label[i] = label[i];
    d->partition_label[i] = 0;
    return 0;
}

struct blk_dev *blk_partition_wrap(struct blk_dev *parent,
                                   uint64_t offset_lba,
                                   uint64_t size_lba,
                                   const char *name,
                                   uint32_t partition_index,
                                   const uint8_t type_guid[BLK_GUID_BYTES],
                                   const char *label) {
    struct blk_dev *d = wrap_internal(parent, offset_lba, size_lba, name,
                                      BLK_CLASS_PARTITION);
    if (!d) return 0;
    d->partition_index = partition_index;
    if (type_guid) {
        for (size_t i = 0; i < BLK_GUID_BYTES; i++) d->type_guid[i] = type_guid[i];
    }
    if (label) {
        size_t i = 0;
        while (label[i] && i < BLK_PART_LABEL_MAX - 1) {
            d->partition_label[i] = label[i];
            i++;
        }
        d->partition_label[i] = 0;
    }
    return d;
}
