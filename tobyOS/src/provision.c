/* provision.c -- user-driven persistent-storage provisioning.
 *
 * Turns a BLANK spare disk (SATA / NVMe / USB / IDE) into a persistent
 * /data volume: writes a GPT whose single partition carries the
 * tobyOS-data type GUID, formats it with tobyfs, and mounts it. The
 * boot-time discovery in kernel.c already prefers exactly this layout
 * (partition_find_by_type(GPT_TYPE_TOBYOS_DATA)), so a provisioned
 * disk is found again on every subsequent boot with no configuration.
 *
 * DATA SAFETY -- the guard (provision_classify) is the contract:
 *
 *   - A disk carrying a recognised FOREIGN filesystem (NTFS / FAT /
 *     exFAT / ext / ISO9660) or a foreign partition table (MBR with
 *     real entries, GPT with any non-tobyOS partition) is NEVER a
 *     valid target. No flag overrides this -- the Windows disk and the
 *     live boot stick simply cannot be written through this path.
 *   - A disk that backs a live VFS mount is never a valid target.
 *   - A disk that already carries tobyOS data (whole-disk tobyfs, a
 *     tobyOS-only GPT, or an empty partition table) additionally
 *     needs ABI_PROV_F_FORCE, which the disk-manager UI only sets
 *     after its typed confirmation.
 *   - Only a fully BLANK disk is provisionable without FORCE (the UI
 *     still runs the typed confirmation for it).
 *
 * The guard runs IN THE KERNEL on every request -- it re-probes the
 * target no matter what the calling app claims to have checked.
 */

#include <tobyos/provision.h>
#include <tobyos/blk.h>
#include <tobyos/partition.h>
#include <tobyos/tobyfs.h>
#include <tobyos/fat32.h>
#include <tobyos/ext2.h>
#include <tobyos/vfs.h>
#include <tobyos/bcache.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>
#include <tobyos/installer.h>   /* t7: the installer is a writer too */

/* ---- tiny helpers ------------------------------------------------ */

static void str_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static bool name_is_ramdata(const struct blk_dev *d) {
    return d && d->name && strcmp(d->name, "ramdata") == 0;
}

/* ---- live-mount detection ---------------------------------------- */

struct prov_mount_walk {
    struct blk_dev *target;
    const char     *mount_point;   /* set on hit (points into vfs table) */
    bool            found;
};

/* Map a mount's opaque data back to its block device for the three
 * block-backed filesystems tobyOS mounts. Unknown vfs_ops (ramfs,
 * procfs, devfs...) have no block device -- return NULL. */
static struct blk_dev *mount_backing_dev(const struct vfs_ops *ops,
                                         void *mount_data) {
    if (ops == tobyfs_ops_addr()) return tobyfs_blkdev_of(mount_data);
    if (ops == &fat32_ops)        return fat32_blkdev_of(mount_data);
    if (ops == &ext2_ops)         return ext2_blkdev_of(mount_data);
    return 0;
}

static bool prov_mount_cb(const char *mount_point,
                          const struct vfs_ops *ops,
                          void *mount_data,
                          void *cookie) {
    struct prov_mount_walk *w = (struct prov_mount_walk *)cookie;
    struct blk_dev *bd = mount_backing_dev(ops, mount_data);
    if (!bd) return true;
    /* Match the device itself, a partition/wrapper carved from the
     * target disk, or the target being a slice of the mounted disk. */
    if (bd == w->target || bd->parent == w->target ||
        (w->target && w->target->parent == bd)) {
        w->mount_point = mount_point;
        w->found = true;
        return false;
    }
    return true;
}

bool provision_dev_mounted(struct blk_dev *dev, const char **mount_point) {
    struct prov_mount_walk w = { .target = dev, .mount_point = 0,
                                 .found = false };
    vfs_iter_mounts(prov_mount_cb, &w);
    if (mount_point) *mount_point = w.mount_point;
    return w.found;
}

/* Find the block device currently backing /data (NULL if unmounted or
 * not a block-backed mount). */
struct prov_data_walk { struct blk_dev *dev; };

static bool prov_data_cb(const char *mount_point, const struct vfs_ops *ops,
                         void *mount_data, void *cookie) {
    struct prov_data_walk *w = (struct prov_data_walk *)cookie;
    if (strcmp(mount_point, "/data") != 0) return true;
    w->dev = mount_backing_dev(ops, mount_data);
    return false;
}

static struct blk_dev *data_backing_dev(void) {
    struct prov_data_walk w = { .dev = 0 };
    vfs_iter_mounts(prov_data_cb, &w);
    return w.dev;
}

/* ---- read-only classification ------------------------------------ */

/* Filesystem-signature probe of one device's own address space (LBA 0
 * relative to the device -- works for whole disks and partitions
 * alike). Returns flags: bit0 tobyfs, and writes the first matched
 * name into fs[] by priority. Read failures leave fs empty. */
static bool probe_fs_signature(struct blk_dev *d, char fs[ABI_BLK_FS_MAX]) {
    uint8_t b0[BLK_SECTOR_SIZE];
    fs[0] = 0;
    if (blk_read(d, 0, 1, b0) != 0) return false;

    uint64_t magic;
    memcpy(&magic, b0, sizeof(magic));
    if (magic == TFS_MAGIC)                      { str_copy(fs, ABI_BLK_FS_MAX, "tobyfs");  return true; }
    if (memcmp(b0 + 3, "NTFS    ", 8) == 0)      { str_copy(fs, ABI_BLK_FS_MAX, "ntfs");    return true; }
    if (memcmp(b0 + 3, "EXFAT   ", 8) == 0)      { str_copy(fs, ABI_BLK_FS_MAX, "exfat");   return true; }
    if (memcmp(b0 + 82, "FAT32   ", 8) == 0 ||
        memcmp(b0 + 54, "FAT1", 4) == 0)         { str_copy(fs, ABI_BLK_FS_MAX, "fat");     return true; }

    /* ext2/3/4: superblock at byte 1024, s_magic (0xEF53) at +56. */
    if (d->sector_count > 2) {
        uint8_t b2[BLK_SECTOR_SIZE];
        if (blk_read(d, 2, 1, b2) == 0) {
            uint16_t em;
            memcpy(&em, b2 + 56, sizeof(em));
            if (em == 0xEF53)                    { str_copy(fs, ABI_BLK_FS_MAX, "ext");     return true; }
        }
    }
    /* ISO9660 (the live boot medium): "CD001" at byte 32769. */
    if (d->sector_count > 64) {
        uint8_t b64[BLK_SECTOR_SIZE];
        if (blk_read(d, 64, 1, b64) == 0 &&
            memcmp(b64 + 1, "CD001", 5) == 0)    { str_copy(fs, ABI_BLK_FS_MAX, "iso9660"); return true; }
    }
    return true;
}

/* Parse the GPT on `d` (if any) and count tobyOS-data vs other
 * partitions. Returns true if a valid-looking GPT header was found.
 * Light validation on purpose: for the SAFETY decision a half-broken
 * GPT still means "someone's data lives here" -- so a bad CRC counts
 * as a table with foreign content, not as a blank disk. */
static bool probe_gpt(struct blk_dev *d, int *tobyos_parts,
                      int *other_parts, bool *malformed) {
    *tobyos_parts = 0;
    *other_parts  = 0;
    *malformed    = false;

    uint8_t hb[BLK_SECTOR_SIZE];
    if (blk_read(d, GPT_HEADER_LBA, 1, hb) != 0) return false;
    struct gpt_header h;
    memcpy(&h, hb, sizeof(h));
    if (h.signature != GPT_HEADER_SIGNATURE) return false;

    if (h.partition_entry_size < sizeof(struct gpt_entry) ||
        h.partition_entry_size > 4096 ||
        h.num_partition_entries == 0 ||
        h.num_partition_entries > 1024) {
        *malformed = true;
        return true;
    }
    uint64_t total_bytes = (uint64_t)h.num_partition_entries *
                           h.partition_entry_size;
    uint64_t total_secs  = (total_bytes + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;
    if (total_secs > 128) { *malformed = true; return true; }

    uint8_t *arr = kmalloc((size_t)(total_secs * BLK_SECTOR_SIZE));
    if (!arr) { *malformed = true; return true; }
    if (blk_read(d, h.partition_entry_lba, (uint32_t)total_secs, arr) != 0) {
        kfree(arr);
        *malformed = true;
        return true;
    }
    for (uint32_t i = 0; i < h.num_partition_entries; i++) {
        struct gpt_entry e;
        memcpy(&e, arr + (size_t)i * h.partition_entry_size, sizeof(e));
        if (partition_guid_cmp(e.type_guid, GPT_TYPE_UNUSED) == 0) continue;
        if (partition_guid_cmp(e.type_guid, GPT_TYPE_TOBYOS_DATA) == 0)
            (*tobyos_parts)++;
        else
            (*other_parts)++;
    }
    kfree(arr);
    return true;
}

uint8_t provision_classify(struct blk_dev *d, uint32_t *flags,
                           char fs[ABI_BLK_FS_MAX]) {
    uint32_t fl = 0;
    fs[0] = 0;

    if (!d) { if (flags) *flags = 0; return ABI_BLKV_UNKNOWN; }
    if (d->class != BLK_CLASS_DISK) {
        probe_fs_signature(d, fs);
        if (provision_dev_mounted(d, 0)) fl |= ABI_BLK_F_MOUNTED;
        if (flags) *flags = fl;
        return ABI_BLKV_NOT_DISK;
    }
    if (d->gone) {
        if (flags) *flags = ABI_BLK_F_GONE;
        return ABI_BLKV_UNKNOWN;
    }
    if (name_is_ramdata(d)) {
        fl |= ABI_BLK_F_RAM;
        if (provision_dev_mounted(d, 0)) fl |= ABI_BLK_F_MOUNTED;
        if (flags) *flags = fl;
        return ABI_BLKV_UNKNOWN;    /* never a provisioning target */
    }

    bool mounted = provision_dev_mounted(d, 0);
    if (mounted) fl |= ABI_BLK_F_MOUNTED;
    if (d->removable) fl |= ABI_BLK_F_REMOVABLE;

    /* MBR / protective-MBR probe. */
    uint8_t b0[BLK_SECTOR_SIZE];
    if (blk_read(d, 0, 1, b0) != 0) {
        if (flags) *flags = fl;
        return ABI_BLKV_UNKNOWN;
    }
    bool mbr_sig = (b0[MBR_SIGNATURE_OFF] == 0x55 &&
                    b0[MBR_SIGNATURE_OFF + 1] == 0xAA);
    bool protective = false;
    int  mbr_real_entries = 0;
    if (mbr_sig) {
        for (int i = 0; i < 4; i++) {
            uint8_t type = b0[446 + i * 16 + 4];
            if (type == 0) continue;
            if (type == MBR_PROTECTIVE_GPT_TYPE) protective = true;
            else mbr_real_entries++;
        }
    }

    /* Whole-disk filesystem signatures. NOTE: a FAT BPB also ends in
     * 55AA -- probe_fs_signature distinguishes it from a plain MBR via
     * the FAT strings, so the two checks don't fight. */
    bool sig_read = probe_fs_signature(d, fs);
    bool tobyfs_whole = (fs[0] && strcmp(fs, "tobyfs") == 0);
    bool foreign_fs   = (fs[0] && !tobyfs_whole);
    if (tobyfs_whole) fl |= ABI_BLK_F_TOBYFS;

    /* GPT probe (independent of the protective flag: some tools write
     * a GPT without a strictly-correct protective MBR). */
    int tobyos_parts = 0, other_parts = 0;
    bool gpt_malformed = false;
    bool has_gpt = probe_gpt(d, &tobyos_parts, &other_parts, &gpt_malformed);
    if (has_gpt) fl |= ABI_BLK_F_GPT;
    if (mbr_sig && !protective && !has_gpt && mbr_real_entries > 0)
        fl |= ABI_BLK_F_MBR;

    if (flags) *flags = fl;

    if (mounted) return ABI_BLKV_MOUNTED;
    if (!sig_read) return ABI_BLKV_UNKNOWN;

    /* Foreign content -- NEVER provisionable, no override:
     *   - a recognised foreign filesystem on the raw disk
     *   - an MBR with real (non-protective) partition entries
     *   - a GPT with any non-tobyOS partition, or a GPT too mangled
     *     to enumerate (assume it protects someone's data) */
    if (foreign_fs || mbr_real_entries > 0 ||
        (has_gpt && (other_parts > 0 || gpt_malformed)))
        return ABI_BLKV_FOREIGN;

    /* tobyOS-owned or user-cleared content -- provisionable with FORCE:
     *   - whole-disk tobyfs
     *   - GPT whose non-empty entries are all tobyOS-data (incl. zero
     *     entries = an empty table the user cleared elsewhere)
     *   - a bare 55AA boot signature with no entries (wiped disk) */
    if (tobyfs_whole || has_gpt || mbr_sig)
        return ABI_BLKV_TOBYOS;

    return ABI_BLKV_BLANK;
}

/* ---- GPT writer ---------------------------------------------------
 *
 * Writes the exact layout the boot-time discovery keys on (and that
 * build/mkdisk_gpt produces on the host): protective MBR, primary
 * header @ LBA 1, 128x128-byte entry array @ LBA 2..33, one
 * tobyOS-data partition, backup array + header in the last 33 LBAs.
 */

#define PROV_ENTRY_COUNT   GPT_DEFAULT_ENTRY_COUNT              /* 128 */
#define PROV_ENTRY_BYTES   GPT_DEFAULT_ENTRY_SIZE               /* 128 */
#define PROV_ARRAY_BYTES   (PROV_ENTRY_COUNT * PROV_ENTRY_BYTES)
#define PROV_ARRAY_SECTORS (PROV_ARRAY_BYTES / BLK_SECTOR_SIZE) /* 32 */

/* Partition start: 1 MiB aligned, the universal SSD/tool convention. */
#define PROV_PART_START_LBA 2048ull

static void utf16_label(uint16_t *dst, size_t chars, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < chars; i++) dst[i] = (uint16_t)src[i];
    for (; i < chars; i++) dst[i] = 0;
}

/* Deterministic-enough unique GUIDs: ticks + disk name hash, with the
 * RFC 4122 version/variant bits patched so tools render them cleanly.
 * These only need to be unique across the machine's disks. */
static void make_unique_guid(uint8_t g[BLK_GUID_BYTES], const char *seed) {
    uint64_t t = pit_ticks() ^ 0x746F62794F534744ull;
    uint32_t h = 2166136261u;
    for (const char *c = seed; c && *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    memcpy(g, &t, 8);
    memcpy(g + 8, &h, 4);
    memcpy(g + 12, &h, 4);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

static void fill_header(struct gpt_header *h, uint64_t my_lba,
                        uint64_t alt_lba, uint64_t arr_lba,
                        uint64_t nsectors, uint32_t arr_crc,
                        const uint8_t disk_guid[BLK_GUID_BYTES]) {
    memset(h, 0, sizeof(*h));
    h->signature             = GPT_HEADER_SIGNATURE;
    h->revision              = GPT_HEADER_REVISION_1_0;
    h->header_size           = GPT_HEADER_SIZE;
    h->my_lba                = my_lba;
    h->alternate_lba         = alt_lba;
    h->first_usable_lba      = GPT_DEFAULT_ENTRY_LBA + PROV_ARRAY_SECTORS;
    h->last_usable_lba       = nsectors - PROV_ARRAY_SECTORS - 2;
    memcpy(h->disk_guid, disk_guid, BLK_GUID_BYTES);
    h->partition_entry_lba   = arr_lba;
    h->num_partition_entries = PROV_ENTRY_COUNT;
    h->partition_entry_size  = PROV_ENTRY_BYTES;
    h->partition_entry_array_crc32 = arr_crc;
    h->header_crc32          = partition_crc32(h, h->header_size);
}

static int write_gpt_single(struct blk_dev *d, uint64_t start, uint64_t end) {
    uint64_t n = d->sector_count;
    uint8_t *arr = kcalloc(1, PROV_ARRAY_BYTES);
    if (!arr) return -1;

    struct gpt_entry e;
    memset(&e, 0, sizeof(e));
    memcpy(e.type_guid, GPT_TYPE_TOBYOS_DATA, BLK_GUID_BYTES);
    make_unique_guid(e.unique_guid, d->name);
    e.starting_lba = start;
    e.ending_lba   = end;
    utf16_label(e.name_utf16, 36, "tobyOS-data");
    memcpy(arr, &e, sizeof(e));
    uint32_t arr_crc = partition_crc32(arr, PROV_ARRAY_BYTES);

    uint8_t disk_guid[BLK_GUID_BYTES];
    make_unique_guid(disk_guid, d->name ? d->name : "disk");
    disk_guid[0] ^= 0xA5;    /* disk GUID != partition GUID */

    /* Protective MBR (mkdisk_gpt layout: one 0xEE entry spanning
     * LBA 1..end, CHS sentinels, 55AA). */
    uint8_t sect[BLK_SECTOR_SIZE];
    memset(sect, 0, sizeof(sect));
    uint8_t *pe = sect + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;
    pe[4] = MBR_PROTECTIVE_GPT_TYPE;
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    uint32_t pstart = 1;
    uint64_t span   = n - 1;
    uint32_t span32 = (span > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)span;
    memcpy(pe + 8, &pstart, 4);
    memcpy(pe + 12, &span32, 4);
    sect[510] = 0x55; sect[511] = 0xAA;
    if (blk_write(d, 0, 1, sect) != 0) goto io_fail;

    /* Primary header + array. */
    struct gpt_header h;
    fill_header(&h, GPT_HEADER_LBA, n - 1, GPT_DEFAULT_ENTRY_LBA, n,
                arr_crc, disk_guid);
    memset(sect, 0, sizeof(sect));
    memcpy(sect, &h, sizeof(h));
    if (blk_write(d, GPT_HEADER_LBA, 1, sect) != 0) goto io_fail;
    if (blk_write(d, GPT_DEFAULT_ENTRY_LBA, PROV_ARRAY_SECTORS, arr) != 0)
        goto io_fail;

    /* Backup array + header (last 33 LBAs). */
    uint64_t backup_arr_lba = n - 1 - PROV_ARRAY_SECTORS;
    fill_header(&h, n - 1, GPT_HEADER_LBA, backup_arr_lba, n,
                arr_crc, disk_guid);
    if (blk_write(d, backup_arr_lba, PROV_ARRAY_SECTORS, arr) != 0)
        goto io_fail;
    memset(sect, 0, sizeof(sect));
    memcpy(sect, &h, sizeof(h));
    if (blk_write(d, n - 1, 1, sect) != 0) goto io_fail;

    /* Zero the first tobyfs block of the new partition so any stale
     * superblock magic in that region can't be mistaken for a live
     * filesystem if the follow-up format is interrupted. */
    memset(sect, 0, sizeof(sect));
    for (uint64_t s = 0; s < 8 && start + s <= end; s++) {
        if (blk_write(d, start + s, 1, sect) != 0) goto io_fail;
    }

    kfree(arr);
    return 0;

io_fail:
    kfree(arr);
    return -2;
}

/* ---- end-to-end create ------------------------------------------- */

long provision_create_data_volume(struct blk_dev *d, uint32_t flags) {
    if (!d) return -ABI_EINVAL;
    bool force = (flags & ABI_PROV_F_FORCE) != 0;
    bool erase = (flags & ABI_PROV_F_ERASE) != 0;

    char fs[ABI_BLK_FS_MAX];
    uint32_t fl = 0;
    uint8_t v = provision_classify(d, &fl, fs);

    switch (v) {
    case ABI_BLKV_BLANK:
        break;
    case ABI_BLKV_TOBYOS:
        if (!force) {
            kprintf("[prov] '%s' already carries tobyOS data / an empty "
                    "table -- FORCE required\n", d->name);
            return -ABI_EEXIST;
        }
        break;
    case ABI_BLKV_MOUNTED: {
        const char *mp = 0;
        (void)provision_dev_mounted(d, &mp);
        kprintf("[prov] REFUSED '%s': backs a live mount (%s)\n",
                d->name, mp ? mp : "?");
        return -ABI_EBUSY;
    }
    case ABI_BLKV_FOREIGN:
        /* THE OWNER MAY REFORMAT THEIR OWN REMOVABLE MEDIA -- and nothing
         * else. Every condition below is re-checked HERE, in the kernel,
         * whatever userspace claims to have confirmed.
         *
         * The blanket refusal this replaces was right about the danger and
         * wrong as an absolute: it also meant a user with a FAT32 stick had
         * no way to hand it to tobyOS, which is all `mkfs` has ever been.
         * What stays refused is everything the owner cannot plausibly mean:
         * a fixed disk (so the drive holding a Windows install is
         * unreachable by ANY flag), the live boot medium we are running
         * from, and anything currently mounted. */
        if (!erase) {
            kprintf("[prov] REFUSED '%s': foreign filesystem/partition table "
                    "(%s) -- ERASE is required to reformat removable media\n",
                    d->name, fs[0] ? fs : "partition table");
            return -ABI_EPERM;
        }
        if (!d->removable) {
            kprintf("[prov] REFUSED '%s': ERASE covers REMOVABLE media only; "
                    "this is a fixed disk carrying %s\n",
                    d->name, fs[0] ? fs : "a partition table");
            return -ABI_EPERM;
        }
        if (fs[0] && strcmp(fs, "iso9660") == 0) {
            kprintf("[prov] REFUSED '%s': iso9660 -- this is the live boot "
                    "medium; erasing it would destroy the running system\n",
                    d->name);
            return -ABI_EPERM;
        }
        if (provision_dev_mounted(d, 0)) {
            kprintf("[prov] REFUSED '%s': backs a live mount\n", d->name);
            return -ABI_EBUSY;
        }
        kprintf("[prov] ERASE '%s' (%s, %lu MiB): destroying %s at the "
                "owner's explicit request\n",
                d->name, d->model[0] ? d->model : "unknown model",
                (unsigned long)(d->sector_count / 2048u),
                fs[0] ? fs : "an existing partition table");
        break;
    default:
        kprintf("[prov] REFUSED '%s': not a provisionable whole disk "
                "(verdict %u)\n", d->name, (unsigned)v);
        return -ABI_EINVAL;
    }

    /* FORMAT_ONLY: a plain whole-disk tobyfs, and nothing else.
     *
     * Placed HERE, after the verdict switch, so it inherits the guard
     * unchanged -- the flag decides what gets written, never who may be
     * written to. No GPT (a transfer stick does not need a partition
     * table), and /data is deliberately left alone: "format this USB" and
     * "make this my system volume" are different requests. */
    if (flags & ABI_PROV_F_FORMAT_ONLY) {
        kprintf("[prov] formatting '%s' (%s, %lu MiB) as whole-disk tobyfs\n",
                d->name, d->model[0] ? d->model : "unknown model",
                (unsigned long)(d->sector_count / 2048u));
        int fmt = tobyfs_format(d);
        if (fmt != VFS_OK) {
            kprintf("[prov] format of '%s' FAILED: %s\n",
                    d->name, vfs_strerror(fmt));
            return -ABI_EIO;
        }
        /* The failed probes above cached this device's pre-format sectors;
         * drop them or a following mount re-reads the old bytes. Exactly
         * the trap the GPT path documents a few lines further down. */
        bcache_invalidate(d);
        kprintf("[prov] '%s' formatted -- mount it with: "
                "mount -t tobyfs %s /mnt/usb\n", d->name, d->name);
        return ABI_PROV_OK_NEXT_BOOT;
    }

    /* Geometry: partition @ 1 MiB .. last usable LBA. tobyfs needs at
     * least TFS_TOTAL_BLOCKS 4-KiB blocks. */
    uint64_t min_part = (uint64_t)TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK;
    if (d->sector_count < PROV_PART_START_LBA + min_part +
                          PROV_ARRAY_SECTORS + 2) {
        kprintf("[prov] '%s' too small (%lu sectors; need >= %lu)\n",
                d->name, (unsigned long)d->sector_count,
                (unsigned long)(PROV_PART_START_LBA + min_part +
                                PROV_ARRAY_SECTORS + 2));
        return -ABI_EINVAL;
    }
    uint64_t start = PROV_PART_START_LBA;
    uint64_t end   = d->sector_count - PROV_ARRAY_SECTORS - 2;

    kprintf("[prov] provisioning '%s' (%s, %lu MiB): tobyOS-data GPT "
            "partition LBA %lu..%lu\n",
            d->name, d->model[0] ? d->model : "unknown model",
            (unsigned long)(d->sector_count / 2048u),
            (unsigned long)start, (unsigned long)end);

    if (write_gpt_single(d, start, end) != 0) {
        kprintf("[prov] GPT write failed on '%s'\n", d->name);
        return -ABI_EIO;
    }
    /* Drop every cached block of this disk: the probes above cached
     * pre-GPT bytes, and any stale FS blocks in the partition range
     * would shadow the freshly formatted ones. */
    bcache_invalidate(d);

    /* Surface the new partition in the registry. Idempotent: on a
     * re-provision the identical "<disk>.p1" record already exists. */
    (void)partition_scan_disk(d);
    char pname[64];
    ksnprintf(pname, sizeof(pname), "%s.p1", d->name);
    struct blk_dev *p = blk_find(pname);
    if (!p || p->class != BLK_CLASS_PARTITION || p->parent != d ||
        p->offset_lba != start || p->sector_count != end - start + 1) {
        kprintf("[prov] partition registration mismatch for '%s'\n", pname);
        return -ABI_EIO;
    }

    if (tobyfs_format(p) != VFS_OK) {
        kprintf("[prov] tobyfs format failed on '%s'\n", pname);
        return -ABI_EIO;
    }
    /* The failed pre-format probes cached this partition's old blocks;
     * drop them or the mount below re-reads pre-format bytes. */
    bcache_invalidate(p);

    /* Everything above went through blk_write / bcache; push it out of
     * the drive's volatile cache before declaring success. */
    (void)blk_flush(d);

    /* Mount policy: take over /data only from the RAM fallback (or
     * nothing). A real persistent /data stays live; the new volume is
     * a partition, which the boot sweep prefers over whole-disk
     * volumes, so it wins on the next boot. */
    struct blk_dev *cur = data_backing_dev();
    if (cur && !name_is_ramdata(cur)) {
        kprintf("[prov] '%s' provisioned; current /data ('%s') stays -- "
                "new volume is picked up on the next boot\n",
                pname, cur->name);
        return ABI_PROV_OK_NEXT_BOOT;
    }
    if (cur) (void)vfs_unmount("/data");
    if (tobyfs_mount("/data", p) != VFS_OK) {
        kprintf("[prov] mount of fresh '%s' at /data FAILED\n", pname);
        /* Try to restore the RAM fallback so the session keeps a
         * writable /data. */
        if (cur && tobyfs_mount("/data", cur) == VFS_OK)
            kprintf("[prov] RAM-backed /data restored\n");
        return -ABI_EIO;
    }
    /* Root-owned 0755 straight out of the formatter; without this the
     * desktop session that just asked for this volume cannot write to
     * the thing it just made. */
    data_volume_relax_perms();
    kprintf("[prov] '%s' is now /data (persistent)\n", pname);
    return ABI_PROV_OK_MOUNTED;
}


/* ---- /data must be writable by the LOGGED-IN USER -------------------
 *
 * A freshly formatted tobyfs root is uid 0 / mode 0755, so on any
 * non-root session every write to /data fails the permission check --
 * no saved files, no settings, no downloads. /data is the desktop's
 * shared scratch+data volume, so it gets /tmp semantics: world-writable.
 *
 * This lives HERE, called from every site that mounts /data, because it
 * used to live only in the boot path. Provisioning a stick mid-session
 * mounts /data a SECOND time, that mount skipped the relaxation, and the
 * volume stayed root-only until the next reboot -- which is exactly the
 * "I formatted my USB, made a file, then could not save edits" report
 * from real hardware on 2026-08-25. One copy, every caller.
 */
void data_volume_relax_perms(void) {
    struct vfs_stat st;
    if (vfs_stat("/data", &st) != VFS_OK) return;
    if (st.mode & 00002u) return;              /* already world-writable */
    unsigned was = (unsigned)(st.mode & 07777u);
    if (vfs_chmod("/data", 00777u) == VFS_OK)
        kprintf("[data] /data was mode 0%04o (root-only) -- relaxed to 0777 "
                "so non-root sessions can use the desktop\n", was);
    else
        kprintf("[data] WARN: /data is mode 0%04o (root-only) and could not "
                "be relaxed -- non-root logins will not be able to save "
                "anything\n", was);
}

/* ---- SYS_BLK_LIST record builder (kernel staging buffer) ---------- */

static int16_t registry_index_of(struct blk_dev *d) {
    if (!d) return -1;
    size_t total = blk_count();
    for (size_t i = 0; i < total; i++)
        if (blk_get(i) == d) return (int16_t)i;
    return -1;
}

/* ---- opt-in boot self-test (-DPROVISION_SELFTEST) ------------------
 *
 * Proves the guard on RAM-backed fake disks: a blank disk classifies
 * BLANK and provisions end-to-end; NTFS / MBR-partitioned / foreign-GPT
 * / ISO9660 disks are refused EVEN WITH FORCE; a mounted volume is
 * refused; re-provisioning tobyOS-owned media needs FORCE. Serial
 * marker: "[PROV]". Compiled unconditionally (a few KiB), called from
 * kernel.c only under the flag -- same pattern as blk_nvme_selftest. */

struct prov_ramdisk { uint8_t *buf; uint64_t bytes; };

static int prd_read(struct blk_dev *d, uint64_t lba, uint32_t n, void *buf) {
    struct prov_ramdisk *r = (struct prov_ramdisk *)d->priv;
    uint64_t off = lba * BLK_SECTOR_SIZE, len = (uint64_t)n * BLK_SECTOR_SIZE;
    if (off + len > r->bytes) return -1;
    memcpy(buf, r->buf + off, (size_t)len);
    return 0;
}
static int prd_write(struct blk_dev *d, uint64_t lba, uint32_t n,
                     const void *buf) {
    struct prov_ramdisk *r = (struct prov_ramdisk *)d->priv;
    uint64_t off = lba * BLK_SECTOR_SIZE, len = (uint64_t)n * BLK_SECTOR_SIZE;
    if (off + len > r->bytes) return -1;
    memcpy(r->buf + off, buf, (size_t)len);
    return 0;
}
static const struct blk_ops g_prd_ops = { .read = prd_read,
                                          .write = prd_write };

/* Static lifetimes: provisioned fake disks become PARENTS of real
 * registry entries, so they must outlive this function. */
#define PROV_ST_DISKS 4
static struct blk_dev      g_prd_devs[PROV_ST_DISKS];
static struct prov_ramdisk g_prd_priv[PROV_ST_DISKS];

static struct blk_dev *prd_make(int slot, const char *name,
                                uint64_t sectors) {
    struct blk_dev      *d = &g_prd_devs[slot];
    struct prov_ramdisk *r = &g_prd_priv[slot];
    r->bytes = sectors * BLK_SECTOR_SIZE;
    r->buf   = kmalloc((size_t)r->bytes);
    if (!r->buf) return 0;
    memset(r->buf, 0, (size_t)r->bytes);
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->ops = &g_prd_ops;
    d->sector_count = sectors;
    d->priv = r;
    d->class = BLK_CLASS_DISK;
    str_copy(d->model, sizeof(d->model), "PROV selftest ramdisk");
    return d;
}

/* Release a fake disk's RAM once its tests are done. Only safe for
 * disks that gained NO registry children (a provisioned disk's
 * partition record holds a parent pointer forever). Drops any cache
 * entries keyed to the dev first so a later bcache flush can't write
 * through a freed buffer. */
static void prd_free(int slot) {
    struct blk_dev      *d = &g_prd_devs[slot];
    struct prov_ramdisk *r = &g_prd_priv[slot];
    if (!r->buf) return;
    bcache_discard(d);
    kfree(r->buf);
    r->buf = 0;
    d->gone = true;
}

/* Retype GPT entry 0 on `d` to the MS Basic Data GUID and re-seal both
 * headers' CRCs -- turns our own layout into a "Windows-like" disk. */
static int prd_retype_foreign(struct blk_dev *d) {
    uint8_t *arr = kmalloc(PROV_ARRAY_BYTES);
    uint8_t  hb[BLK_SECTOR_SIZE];
    if (!arr) return -1;
    if (blk_read(d, GPT_DEFAULT_ENTRY_LBA, PROV_ARRAY_SECTORS, arr) != 0)
        goto fail;
    memcpy(arr, GPT_TYPE_MS_BASIC_DATA, BLK_GUID_BYTES);
    uint32_t crc = partition_crc32(arr, PROV_ARRAY_BYTES);
    uint64_t hdr_lbas[2] = { GPT_HEADER_LBA, d->sector_count - 1 };
    uint64_t arr_lbas[2] = { GPT_DEFAULT_ENTRY_LBA,
                             d->sector_count - 1 - PROV_ARRAY_SECTORS };
    for (int i = 0; i < 2; i++) {
        if (blk_write(d, arr_lbas[i], PROV_ARRAY_SECTORS, arr) != 0)
            goto fail;
        if (blk_read(d, hdr_lbas[i], 1, hb) != 0) goto fail;
        struct gpt_header h;
        memcpy(&h, hb, sizeof(h));
        h.partition_entry_array_crc32 = crc;
        h.header_crc32 = 0;
        h.header_crc32 = partition_crc32(&h, h.header_size);
        memset(hb, 0, sizeof(hb));
        memcpy(hb, &h, sizeof(h));
        if (blk_write(d, hdr_lbas[i], 1, hb) != 0) goto fail;
    }
    kfree(arr);
    bcache_invalidate(d);
    return 0;
fail:
    kfree(arr);
    return -1;
}

static int g_prov_st_fail;

static void prov_expect(const char *what, long got, long want) {
    if (got == want) {
        kprintf("[PROV] PASS %s (rc=%ld)\n", what, got);
    } else {
        kprintf("[PROV] FAIL %s: got %ld want %ld\n", what, got, want);
        g_prov_st_fail++;
    }
}

void provision_selftest(void) {
    kprintf("[PROV] provisioning guard self-test starting\n");
    g_prov_st_fail = 0;
    char fs[ABI_BLK_FS_MAX];
    uint32_t fl;

    /* Remember what backs /data so we can put it back afterwards. */
    struct blk_dev *data_before = data_backing_dev();

    /* t1: blank disk classifies BLANK, provisions without FORCE,
     * ends up as a live /data with a working file round-trip. */
    struct blk_dev *d1 = prd_make(0, "prov0", 12288);   /* 6 MiB */
    if (!d1) { kprintf("[PROV] OOM -- aborting\n"); return; }
    prov_expect("t1 classify blank", provision_classify(d1, &fl, fs),
                ABI_BLKV_BLANK);
    prov_expect("t1 provision blank",
                provision_create_data_volume(d1, 0u),
                ABI_PROV_OK_MOUNTED);
    {
        struct vfs_file f;
        long ok = -1;
        if (vfs_create("/data/prov_selftest.txt") == VFS_OK &&
            vfs_open("/data/prov_selftest.txt", &f) == VFS_OK) {
            if (vfs_write(&f, "prov", 4) == 4) ok = 0;
            vfs_close(&f);
        }
        prov_expect("t1 file round-trip on provisioned /data", ok, 0);
    }

    /* t2: while /data lives on d1.p1 the whole disk classifies MOUNTED
     * and refuses provisioning even with FORCE. Once unmounted it
     * classifies TOBYOS: refused without FORCE, accepted with it. */
    prov_expect("t2 classify while /data mounted",
                provision_classify(d1, &fl, fs), ABI_BLKV_MOUNTED);
    prov_expect("t2 re-provision mounted (FORCE)",
                provision_create_data_volume(d1, ABI_PROV_F_FORCE), -ABI_EBUSY);
    (void)vfs_unmount("/data");
    prov_expect("t2 classify unmounted", provision_classify(d1, &fl, fs),
                ABI_BLKV_TOBYOS);
    prov_expect("t2 re-provision without FORCE",
                provision_create_data_volume(d1, 0u), -ABI_EEXIST);
    prov_expect("t2 re-provision unmounted (FORCE)",
                provision_create_data_volume(d1, ABI_PROV_F_FORCE),
                ABI_PROV_OK_MOUNTED);
    (void)vfs_unmount("/data");

    /* t3: "Windows-like" disk -- our GPT retyped to MS Basic Data --
     * refuses classification AND provisioning even with FORCE. */
    struct blk_dev *d2 = prd_make(1, "prov1", 12288);
    if (d2 && write_gpt_single(d2, PROV_PART_START_LBA,
                               d2->sector_count - PROV_ARRAY_SECTORS - 2) == 0
           && prd_retype_foreign(d2) == 0) {
        prov_expect("t3 classify foreign GPT",
                    provision_classify(d2, &fl, fs), ABI_BLKV_FOREIGN);
        prov_expect("t3 provision foreign GPT (FORCE)",
                    provision_create_data_volume(d2, ABI_PROV_F_FORCE), -ABI_EPERM);
    } else {
        kprintf("[PROV] FAIL t3 setup\n");
        g_prov_st_fail++;
    }
    prd_free(1);

    /* t4: NTFS superblock -> FOREIGN even with FORCE. */
    struct blk_dev *d3 = prd_make(2, "prov2", 2048);    /* 1 MiB */
    if (d3) {
        uint8_t s[BLK_SECTOR_SIZE];
        memset(s, 0, sizeof(s));
        memcpy(s + 3, "NTFS    ", 8);
        s[510] = 0x55; s[511] = 0xAA;
        (void)blk_write(d3, 0, 1, s);
        prov_expect("t4 classify NTFS", provision_classify(d3, &fl, fs),
                    ABI_BLKV_FOREIGN);
        prov_expect("t4 provision NTFS (FORCE)",
                    provision_create_data_volume(d3, ABI_PROV_F_FORCE), -ABI_EPERM);

        /* t5: same disk, rewritten as an MBR with one Linux (0x83)
         * partition entry -> FOREIGN. */
        memset(s, 0, sizeof(s));
        s[446 + 4] = 0x83;
        uint32_t st = 2048, cnt = 8192;
        memcpy(s + 446 + 8, &st, 4);
        memcpy(s + 446 + 12, &cnt, 4);
        s[510] = 0x55; s[511] = 0xAA;
        (void)blk_write(d3, 0, 1, s);
        bcache_invalidate(d3);
        prov_expect("t5 classify MBR table", provision_classify(d3, &fl, fs),
                    ABI_BLKV_FOREIGN);
        prov_expect("t5 provision MBR table (FORCE)",
                    provision_create_data_volume(d3, ABI_PROV_F_FORCE), -ABI_EPERM);
    } else {
        kprintf("[PROV] FAIL t4/t5 setup\n");
        g_prov_st_fail++;
    }
    prd_free(2);

    /* ---- t5b: THE EXPLICIT-ERASE PATH, in all four directions -------
     *
     * ABI_PROV_F_ERASE is the one flag that authorises destroying somebody
     * else's data, so the interesting assertions are the REFUSALS. A
     * permission model is only proven by what it says no to.
     *
     * The disk is rebuilt as FAT for each case; `removable` is toggled by
     * hand, which is the whole point -- an internal disk carrying the same
     * bytes must be unreachable by the same flag. */
    /* 12288 sectors, matching t1/t3: a provisionable volume needs
     * PROV_PART_START_LBA + TFS_TOTAL_BLOCKS*TFS_SECTORS_PER_BLOCK +
     * PROV_ARRAY_SECTORS + 2 = 10274 sectors. At 8192 the ALLOW case
     * returned EINVAL ("too small") and looked like a guard failure. */
    struct blk_dev *d5 = prd_make(2, "prov2e", 12288);  /* 6 MiB */
    if (d5) {
        uint8_t s[BLK_SECTOR_SIZE];

        /* THE REFUSALS RUN FIRST, and the ALLOW case last, because a
         * successful provision MOUNTS the disk as /data -- after which it
         * classifies MOUNTED and every later assertion here would be
         * testing the mounted path wearing another name. The first draft
         * had the allow case in the middle and the iso9660 refusal after
         * it: the refusal "passed" for the wrong reason (EBUSY, not
         * EPERM), which the expected-value check caught. */

        /* iso9660 + removable + ERASE -> REFUSED. The live boot medium;
         * erasing it destroys the system that is running. */
        memset(s, 0, sizeof(s));
        (void)blk_write(d5, 0, 1, s);
        {
            uint8_t iso[BLK_SECTOR_SIZE];
            memset(iso, 0, sizeof(iso));
            memcpy(iso + 1, "CD001", 5);
            (void)blk_write(d5, 64, 1, iso);
        }
        bcache_invalidate(d5);
        d5->removable = true;
        prov_expect("t5b iso9660 + removable + ERASE",
                    provision_create_data_volume(d5, ABI_PROV_F_ERASE),
                    -ABI_EPERM);

        /* Now a FAT32 boot sector: the everyday factory-formatted stick.
         * Sector 64 is cleared too, or the iso9660 probe still matches and
         * the next three cases test the wrong signature. */
        {
            uint8_t zero[BLK_SECTOR_SIZE];
            memset(zero, 0, sizeof(zero));
            (void)blk_write(d5, 64, 1, zero);
        }
        memset(s, 0, sizeof(s));
        memcpy(s + 82, "FAT32   ", 8);
        s[510] = 0x55; s[511] = 0xAA;
        (void)blk_write(d5, 0, 1, s);
        bcache_invalidate(d5);

        /* Fixed disk + ERASE -> refused. The assertion that keeps this
         * flag away from the drive holding someone's Windows. */
        d5->removable = false;
        prov_expect("t5b FAT on FIXED disk + ERASE",
                    provision_create_data_volume(d5, ABI_PROV_F_ERASE),
                    -ABI_EPERM);

        /* Removable, but WITHOUT the flag -> refused. */
        d5->removable = true;
        prov_expect("t5b FAT on removable, no ERASE",
                    provision_create_data_volume(d5, ABI_PROV_F_FORCE),
                    -ABI_EPERM);

        /* Removable + ERASE -> ALLOWED. The stick is the owner's. /data is
         * unmounted by now (t2 left it that way), so the new volume becomes
         * the live /data: OK_MOUNTED, not OK_NEXT_BOOT. */
        prov_expect("t5b FAT on removable + ERASE",
                    provision_create_data_volume(d5, ABI_PROV_F_ERASE),
                    ABI_PROV_OK_MOUNTED);

        /* The volume the caller just asked for must be WRITABLE by the
         * session that asked. A fresh tobyfs root is uid 0 / 0755, and
         * this relaxation used to exist only in the boot path -- so a
         * stick provisioned mid-session stayed root-only until reboot:
         * "I formatted my USB, made a file, then could not save edits",
         * reported from real hardware 2026-08-25. */
        {
            struct vfs_stat dst;
            long wr = (vfs_stat("/data", &dst) == VFS_OK &&
                       (dst.mode & 00002u)) ? 1 : 0;
            prov_expect("t5b-perm fresh /data is world-writable", wr, 1);
        }
        (void)vfs_unmount("/data");
    } else {
        kprintf("[PROV] FAIL t5b setup\n");
        g_prov_st_fail++;
    }
    /* NOT prd_free(2): a provisioned disk owns a partition record that
     * holds a parent pointer forever (see prd_free's own comment), so the
     * slot leaks by design -- exactly as slot 0 does after t1. */

    /* ---- t5c: FORMAT_ONLY -- "format my USB stick" ------------------
     *
     * A different question from provisioning, and the assertions say so:
     * the same guard applies (the flag changes WHAT is written, not WHO
     * may be written to), the result is a real tobyfs, and /data IS LEFT
     * ALONE. That last one is the whole distinction -- a format that
     * quietly stole the system volume would be a worse surprise than the
     * missing feature. */
    {
        struct blk_dev *d7 = prd_make(1, "prov1f", 12288);
        struct blk_dev *data_before_fmt = data_backing_dev();
        if (d7) {
            uint8_t s[BLK_SECTOR_SIZE];
            memset(s, 0, sizeof(s));
            memcpy(s + 82, "FAT32   ", 8);
            s[510] = 0x55; s[511] = 0xAA;
            (void)blk_write(d7, 0, 1, s);
            bcache_invalidate(d7);

            /* Fixed disk -- refused, exactly as for provisioning. */
            d7->removable = false;
            prov_expect("t5c format FIXED disk + ERASE",
                        provision_create_data_volume(
                            d7, ABI_PROV_F_FORMAT_ONLY | ABI_PROV_F_ERASE),
                        -ABI_EPERM);

            /* Removable but no ERASE over a foreign fs -- refused. */
            d7->removable = true;
            prov_expect("t5c format removable, no ERASE",
                        provision_create_data_volume(
                            d7, ABI_PROV_F_FORMAT_ONLY),
                        -ABI_EPERM);

            /* Removable + ERASE -- formatted. */
            prov_expect("t5c format removable + ERASE",
                        provision_create_data_volume(
                            d7, ABI_PROV_F_FORMAT_ONLY | ABI_PROV_F_ERASE),
                        ABI_PROV_OK_NEXT_BOOT);

            /* It is a REAL tobyfs: mountable, and round-trips a file.
             * "returned OK" is not evidence that a filesystem exists. */
            {
                long ok = -1;
                if (tobyfs_mount("/fmttest", d7) == VFS_OK) {
                    struct vfs_file f;
                    if (vfs_create("/fmttest/hello.txt") == VFS_OK &&
                        vfs_open("/fmttest/hello.txt", &f) == VFS_OK) {
                        if (vfs_write(&f, "usb", 3) == 3) ok = 0;
                        vfs_close(&f);
                    }
                    (void)vfs_unmount("/fmttest");
                }
                prov_expect("t5c formatted stick mounts + writes", ok, 0);
            }

            /* AND /data was not touched. */
            prov_expect("t5c format left /data alone",
                        (long)(data_backing_dev() == data_before_fmt), 1L);
        } else {
            kprintf("[PROV] FAIL t5c setup\n");
            g_prov_st_fail++;
        }
        prd_free(1);
    }

    /* t6: a mounted tobyfs volume refuses provisioning (BUSY), then
     * accepts it once unmounted (with FORCE -- it carries tobyfs). */
    struct blk_dev *d4 = prd_make(3, "prov3", 8192);    /* 4 MiB */
    if (d4 && tobyfs_format(d4) == VFS_OK) {
        bcache_invalidate(d4);
        if (tobyfs_mount("/ptest", d4) == VFS_OK) {
            prov_expect("t6 classify mounted",
                        provision_classify(d4, &fl, fs), ABI_BLKV_MOUNTED);
            prov_expect("t6 provision mounted (FORCE)",
                        provision_create_data_volume(d4, ABI_PROV_F_FORCE), -ABI_EBUSY);
            (void)vfs_unmount("/ptest");
            prov_expect("t6 classify unmounted tobyfs",
                        provision_classify(d4, &fl, fs), ABI_BLKV_TOBYOS);
        } else {
            kprintf("[PROV] FAIL t6 mount setup\n");
            g_prov_st_fail++;
        }
    } else {
        kprintf("[PROV] FAIL t6 setup\n");
        g_prov_st_fail++;
    }
    prd_free(3);

    /* Restore whatever backed /data before the test. */
    if (data_backing_dev() != data_before) {
        (void)vfs_unmount("/data");
        if (data_before) {
            if (tobyfs_mount("/data", data_before) == VFS_OK)
                kprintf("[PROV] restored original /data ('%s')\n",
                        data_before->name);
            else
                kprintf("[PROV] WARN: could not restore original /data\n");
        }
    }

    /* ---- t7: THE INSTALLER'S GUARD -------------------------------
     *
     * installer_run() writes a BOOT IMAGE over the front of a disk and
     * until 2026-08-25 had no safety check at all -- its caller picked the
     * target with blk_get_first(), whichever device enumerated first. On a
     * machine whose internal disk came up before its USB sticks that was a
     * boot image over somebody's Windows.
     *
     * Its rule is STRICTER than provisioning's on purpose: there is no
     * ERASE escape hatch, because writing a bootloader over a disk has no
     * everyday case the way handing over a spare USB stick does. So the
     * assertion is that removable + foreign is refused HERE even though the
     * very same disk is erasable through datavol.
     *
     * These run without an install image loaded, so installer_run stops at
     * its -2 ("no install image") check AFTER the guard would have fired --
     * which is why each case asserts the GUARD's specific code and not a
     * generic failure. */
    {
        struct blk_dev *d6 = prd_make(1, "prov1i", 12288);
        if (d6) {
            uint8_t s[BLK_SECTOR_SIZE];
            memset(s, 0, sizeof(s));
            memcpy(s + 3, "NTFS    ", 8);
            s[510] = 0x55; s[511] = 0xAA;
            (void)blk_write(d6, 0, 1, s);
            bcache_invalidate(d6);

            d6->removable = false;
            prov_expect("t7 installer refuses NTFS (fixed)",
                        installer_run(d6), -11);
            /* Removable makes no difference to the INSTALLER. */
            d6->removable = true;
            prov_expect("t7 installer refuses NTFS (removable too)",
                        installer_run(d6), -11);

            /* The live boot medium. */
            memset(s, 0, sizeof(s));
            (void)blk_write(d6, 0, 1, s);
            {
                uint8_t iso[BLK_SECTOR_SIZE];
                memset(iso, 0, sizeof(iso));
                memcpy(iso + 1, "CD001", 5);
                (void)blk_write(d6, 64, 1, iso);
            }
            bcache_invalidate(d6);
            prov_expect("t7 installer refuses iso9660",
                        installer_run(d6), -11);

            /* A partition, not a whole disk. */
            {
                uint8_t zero[BLK_SECTOR_SIZE];
                memset(zero, 0, sizeof(zero));
                (void)blk_write(d6, 64, 1, zero);
                (void)blk_write(d6, 0, 1, zero);
            }
            bcache_invalidate(d6);
            d6->class = BLK_CLASS_PARTITION;
            prov_expect("t7 installer refuses a partition",
                        installer_run(d6), -12);
            d6->class = BLK_CLASS_DISK;

            /* A blank disk PASSES the guard and stops at the missing
             * install image (-2) -- proof the guard let it through rather
             * than the guard being skipped. */
            prov_expect("t7 installer accepts a blank disk",
                        installer_run(d6), -2);
        } else {
            kprintf("[PROV] FAIL t7 setup\n");
            g_prov_st_fail++;
        }
        prd_free(1);
    }

    if (g_prov_st_fail == 0)
        kprintf("[PROV] self-test complete: ALL PASS\n");
    else
        kprintf("[PROV] self-test complete: %d FAILURE(S)\n",
                g_prov_st_fail);
}

long provision_fill_blk_list(struct abi_blk_info *out, uint32_t cap) {
    struct blk_dev *data_dev = data_backing_dev();
    size_t total = blk_count();
    uint32_t n = 0;

    for (size_t i = 0; i < total && n < cap; i++) {
        struct blk_dev *b = blk_get(i);
        if (!b) continue;
        struct abi_blk_info *r = &out[n];
        memset(r, 0, sizeof(*r));

        str_copy(r->name, sizeof(r->name), b->name ? b->name : "blk?");
        str_copy(r->model, sizeof(r->model), b->model);
        str_copy(r->label, sizeof(r->label), b->partition_label);
        r->sector_count    = b->sector_count;
        r->offset_lba      = b->offset_lba;
        r->class           = (uint8_t)b->class;
        r->partition_index = (uint8_t)b->partition_index;
        r->parent          = registry_index_of(b->parent);

        uint32_t fl = 0;
        r->verdict = provision_classify(b, &fl, r->fs);
        if (b->gone) fl |= ABI_BLK_F_GONE;
        if (data_dev &&
            (b == data_dev || data_dev->parent == b)) fl |= ABI_BLK_F_DATA;
        r->flags = fl;
        n++;
    }
    return (long)n;
}
