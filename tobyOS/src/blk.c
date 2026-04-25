/* blk.c -- block-device registry (milestone 21, expanded in 23A).
 *
 * Drivers and the partition layer register block devices here. The
 * registry is a fixed-size flat array; consumers look devices up by
 * name (blk_find), by index (blk_get), or by class (blk_iter_next /
 * blk_first_disk / blk_first_partition).
 *
 * M23A changes
 * ------------
 *
 *   - capacity raised from 8 to 16 entries (partitions can quickly
 *     outnumber the disks they came from)
 *   - new `class` field on each registration; default is BLK_CLASS_DISK
 *     so older driver code (which sets `class = 0` because it predates
 *     the field) keeps registering disks correctly
 *   - blk_dump groups partitions under their parent disk for clarity
 *   - blk_iter_next + blk_first_disk + blk_first_partition for
 *     class-aware enumeration
 *
 * The registry stores raw pointers and never copies. Devices have
 * static OR heap lifetime that must outlive the registry -- in practice
 * we never unregister, so heap allocations live forever.
 */

#include <tobyos/blk.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

static struct blk_dev *g_devs[BLK_MAX_DEVICES];
static size_t          g_count;

void blk_register(struct blk_dev *dev) {
    if (!dev || !dev->ops || !dev->ops->read) {
        kprintf("[blk] WARN: refused malformed registration\n");
        return;
    }
    if (g_count >= BLK_MAX_DEVICES) {
        kprintf("[blk] WARN: registry full (%u entries) -- ignoring '%s'\n",
                (unsigned)BLK_MAX_DEVICES, dev->name ? dev->name : "?");
        return;
    }
    /* Tolerate accidental double-register: if the same pointer is
     * already on the list, do nothing rather than racing duplicates. */
    for (size_t i = 0; i < g_count; i++) {
        if (g_devs[i] == dev) return;
    }
    /* M23A: legacy drivers register with class=0 (the field is brand
     * new). Treat an unspecified class as DISK -- which is what every
     * pre-23A registration was. The partition layer + future USB MSC
     * driver explicitly set class so this default is only ever used
     * for "I'm a whole disk on a PCI controller". */
    if (dev->class == 0) dev->class = BLK_CLASS_DISK;

    g_devs[g_count++] = dev;
    const char *cname = "unk";
    switch (dev->class) {
    case BLK_CLASS_DISK:      cname = "disk"; break;
    case BLK_CLASS_PARTITION: cname = "part"; break;
    case BLK_CLASS_WRAPPER:   cname = "wrap"; break;
    }
    kprintf("[blk] registered '%s' [%s] (%lu sectors, %lu KiB)\n",
            dev->name ? dev->name : "(anon)",
            cname,
            (unsigned long)dev->sector_count,
            (unsigned long)(dev->sector_count / 2u));
}

struct blk_dev *blk_get_first(void) {
    return g_count > 0 ? g_devs[0] : 0;
}

struct blk_dev *blk_find(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < g_count; i++) {
        if (g_devs[i]->name && strcmp(g_devs[i]->name, name) == 0) {
            return g_devs[i];
        }
    }
    return 0;
}

size_t blk_count(void) { return g_count; }

struct blk_dev *blk_get(size_t idx) {
    return idx < g_count ? g_devs[idx] : 0;
}

struct blk_dev *blk_iter_next(size_t *cookie, enum blk_dev_class class) {
    if (!cookie) return 0;
    while (*cookie < g_count) {
        struct blk_dev *d = g_devs[*cookie];
        (*cookie)++;
        if (class == 0 || d->class == class) return d;
    }
    return 0;
}

struct blk_dev *blk_first_disk(void) {
    size_t it = 0;
    return blk_iter_next(&it, BLK_CLASS_DISK);
}

struct blk_dev *blk_first_partition(void) {
    size_t it = 0;
    return blk_iter_next(&it, BLK_CLASS_PARTITION);
}

/* Pretty dump grouped by parent disk. Layout:
 *
 *   [blk] 4 device(s) registered:
 *     [0] ide0:master      disk    32768 sectors (16384 KiB)
 *         +-- ide0:master.p1   part      2048 sectors  ( 1024 KiB)  BIOS Boot
 *         +-- ide0:master.p2   part      8192 sectors  ( 4096 KiB)  tobyOS-data
 *     [1] ide0:master.p1   part      2048 sectors  ( 1024 KiB)
 *     ...
 *
 * The "+--" lines are duplicated information for at-a-glance reading;
 * each device is also listed as its own top-level row so blk_get(idx)
 * stays consistent with the displayed indices.
 */
static void short_guid(const uint8_t g[BLK_GUID_BYTES], char *out) {
    /* Mixed-endian per RFC 4122: first three groups are little-endian,
     * last two are byte-wise. We just print the first 4 bytes (group
     * 1) byte-reversed so the operator can grep for known prefixes
     * like c12a7328 (EFI System) / 21686148 (BIOS Boot) / etc. */
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        uint8_t b = g[3 - i];
        out[i * 2 + 0] = hex[(b >> 4) & 0xF];
        out[i * 2 + 1] = hex[b & 0xF];
    }
    out[8] = 0;
}

/* M26E: tear-down marker for removable storage. Walking the registry
 * once is cheap (BLK_MAX_DEVICES==16) and lets us flip every partition
 * that hangs off `disk` in lockstep with the disk itself. Idempotent:
 * calling on an already-gone device just re-confirms the flag. */
void blk_mark_gone(struct blk_dev *disk) {
    if (!disk) return;
    if (!disk->gone) {
        disk->gone = true;
        kprintf("[blk] '%s' marked gone -- future I/O will return -EIO\n",
                disk->name ? disk->name : "(anon)");
    }
    /* Mark every partition slice whose parent is `disk`. blk_register
     * stores raw pointers, so this comparison is exact. */
    for (size_t i = 0; i < g_count; i++) {
        struct blk_dev *p = g_devs[i];
        if (!p || p->class != BLK_CLASS_PARTITION) continue;
        if (p->parent != disk) continue;
        if (p->gone) continue;
        p->gone = true;
        kprintf("[blk]   partition '%s' marked gone (parent='%s')\n",
                p->name ? p->name : "(anon)",
                disk->name ? disk->name : "(anon)");
    }
}

void blk_dump(void) {
    if (g_count == 0) {
        kprintf("[blk] no block devices registered\n");
        return;
    }
    kprintf("[blk] %lu device(s) registered:\n", (unsigned long)g_count);
    for (size_t i = 0; i < g_count; i++) {
        struct blk_dev *d = g_devs[i];
        const char *cname = "?";
        switch (d->class) {
        case BLK_CLASS_DISK:      cname = "disk"; break;
        case BLK_CLASS_PARTITION: cname = "part"; break;
        case BLK_CLASS_WRAPPER:   cname = "wrap"; break;
        }
        const char *gone_tag = d->gone ? " [GONE]" : "";
        if (d->class == BLK_CLASS_PARTITION) {
            char gprefix[9] = {0};
            short_guid(d->type_guid, gprefix);
            kprintf("  [%lu] %-20s %-4s %8lu sectors (%6lu KiB) "
                    "type=%s label='%s'%s\n",
                    (unsigned long)i,
                    d->name ? d->name : "(anon)",
                    cname,
                    (unsigned long)d->sector_count,
                    (unsigned long)(d->sector_count / 2u),
                    gprefix,
                    d->partition_label[0] ? d->partition_label : "",
                    gone_tag);
        } else {
            kprintf("  [%lu] %-20s %-4s %8lu sectors (%6lu KiB)%s\n",
                    (unsigned long)i,
                    d->name ? d->name : "(anon)",
                    cname,
                    (unsigned long)d->sector_count,
                    (unsigned long)(d->sector_count / 2u),
                    gone_tag);
        }
    }
}
