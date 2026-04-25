/* fat32.h -- on-disk format + mount entrypoint for FAT32 (milestone 23B).
 *
 * FAT32 is the second filesystem in tobyOS and the first one that
 * speaks an industry-standard format. The driver consumes a generic
 * `struct blk_dev *` (whole disk, GPT partition, USB LUN -- all the
 * same to it) and presents itself to VFS through the standard vfs_ops
 * table. No FAT-specific code escapes from src/fat32.c.
 *
 * Layout of a FAT32 volume (sector-aligned regions, LBAs are RELATIVE
 * to the start of the partition):
 *
 *     LBA 0
 *     +-------------------------------------------------+
 *     |  Reserved region  (BPB.rsvd_sec_cnt sectors)    |
 *     |    sec 0     boot sector with BPB / EBPB        |
 *     |    sec 1     FSInfo (signature 0x41615252 ...)  |
 *     |    sec 6     backup boot sector (typical)       |
 *     +-------------------------------------------------+
 *     |  FAT region   (NumFATs * fat_sz32 sectors)      |
 *     |    FAT 0       primary, 32-bit cluster table    |
 *     |    FAT 1       backup, kept in lockstep         |
 *     +-------------------------------------------------+
 *     |  Data region  (cluster 2 starts here)           |
 *     |    cluster N  =  data_start + (N-2)*spc sectors |
 *     +-------------------------------------------------+
 *
 * FAT entries (low 28 bits of each 32-bit slot):
 *   0x0000_0000              free
 *   0x0000_0002..0x0FFFFFEF  next cluster in chain
 *   0x0FFFFFF7               BAD
 *   0x0FFFFFF8..0x0FFFFFFF   end-of-chain (EOC)
 *
 * Directory entries are 32 bytes each. Two flavours coexist:
 *   - Short (8.3) entry           attr != 0x0F
 *   - Long File Name (LFN) entry  attr == 0x0F (RO|HID|SYS|VOL)
 * LFN entries precede the matching short entry on disk in REVERSE
 * order: the entry with `ord & 0x40` set comes FIRST physically and
 * carries the TRAILING characters of the long name.
 */

#ifndef TOBYOS_FAT32_H
#define TOBYOS_FAT32_H

#include <tobyos/types.h>

struct blk_dev;

/* ---- BPB / EBPB ---- */

/* Exact on-disk layout of the FAT32 BIOS Parameter Block (offset 0
 * inside the boot sector). Packed so the offsets match the spec
 * byte-for-byte; we read the boot sector raw and cast. */
struct fat32_bpb {
    uint8_t  jmp[3];               /* +0   short JMP + NOP */
    uint8_t  oem[8];               /* +3   OEM name */
    uint16_t bytes_per_sec;        /* +11  always 512 for us */
    uint8_t  sec_per_clus;         /* +13  power of 2, 1..128 */
    uint16_t rsvd_sec_cnt;         /* +14  reserved sectors before FAT */
    uint8_t  num_fats;             /* +16  1 or 2 */
    uint16_t root_ent_cnt;         /* +17  must be 0 for FAT32 */
    uint16_t tot_sec16;            /* +19  must be 0 for FAT32 */
    uint8_t  media;                /* +21  0xF8 fixed / 0xF0 removable */
    uint16_t fat_sz16;             /* +22  must be 0 for FAT32 */
    uint16_t sec_per_trk;          /* +24  legacy */
    uint16_t num_heads;            /* +26  legacy */
    uint32_t hidd_sec;             /* +28  partition LBA offset (informational) */
    uint32_t tot_sec32;            /* +32  total sectors in this volume */
    /* ---- FAT32 EBPB extension begins here (offset 36) ---- */
    uint32_t fat_sz32;             /* +36  sectors per FAT */
    uint16_t ext_flags;            /* +40  bit7 = mirror disabled, bits0-3 = active FAT */
    uint16_t fs_ver;               /* +42  must be 0x0000 */
    uint32_t root_clus;            /* +44  cluster of root dir (typically 2) */
    uint16_t fs_info;              /* +48  FSInfo sector LBA inside reserved region */
    uint16_t bk_boot_sec;          /* +50  backup boot sector LBA */
    uint8_t  reserved[12];         /* +52 */
    uint8_t  drv_num;              /* +64 */
    uint8_t  reserved1;            /* +65 */
    uint8_t  boot_sig;             /* +66  0x29 if vol_id+vol_lab+fs_type valid */
    uint32_t vol_id;               /* +67 */
    uint8_t  vol_lab[11];          /* +71  short label, padded with spaces */
    uint8_t  fs_type[8];           /* +82  "FAT32   " (informational only) */
} __attribute__((packed));

_Static_assert(sizeof(struct fat32_bpb) == 90, "FAT32 BPB layout");

/* FSInfo sector. Lives inside the reserved region (typically LBA 1). */
struct fat32_fsinfo {
    uint32_t lead_sig;             /* +0    0x41615252 ("RRaA") */
    uint8_t  reserved[480];
    uint32_t struct_sig;           /* +484  0x61417272 ("rrAa") */
    uint32_t free_count;           /* +488  free cluster count, or 0xFFFFFFFF */
    uint32_t nxt_free;             /* +492  hint where to start next alloc */
    uint8_t  reserved2[12];
    uint32_t trail_sig;            /* +508  0xAA550000 */
} __attribute__((packed));

_Static_assert(sizeof(struct fat32_fsinfo) == 512, "FAT32 FSInfo layout");

#define FAT32_FSI_LEAD_SIG   0x41615252u
#define FAT32_FSI_STRUCT_SIG 0x61417272u
#define FAT32_FSI_TRAIL_SIG  0xAA550000u

/* Special FAT entry values (28-bit semantics). */
#define FAT32_ENTRY_MASK   0x0FFFFFFFu
#define FAT32_FREE         0x00000000u
#define FAT32_BAD          0x0FFFFFF7u
#define FAT32_EOC_MIN      0x0FFFFFF8u  /* anything >= EOC_MIN is end-of-chain */
#define FAT32_EOC          0x0FFFFFFFu

/* Directory-entry attribute bits. */
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | \
                            FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

/* Directory-entry status sentinels (first byte of name field). */
#define FAT_DIR_FREE_END     0x00   /* slot is free AND no entries follow */
#define FAT_DIR_FREE         0xE5   /* slot is free, scanning continues */
#define FAT_DIR_KANJI_REPL   0x05   /* original byte was 0xE5; remap to 0xE5 */

/* On-disk 32-byte directory entry, short (8.3) flavour. */
struct fat_dirent {
    uint8_t  name[11];             /* +0   8 + 3 padded with ' ' */
    uint8_t  attr;                 /* +11 */
    uint8_t  ntres;                /* +12  case bits for VFAT */
    uint8_t  crt_time_tenth;       /* +13 */
    uint16_t crt_time;             /* +14 */
    uint16_t crt_date;             /* +16 */
    uint16_t lst_acc_date;         /* +18 */
    uint16_t fst_clus_hi;          /* +20  high 16 bits of first cluster */
    uint16_t wrt_time;             /* +22 */
    uint16_t wrt_date;             /* +24 */
    uint16_t fst_clus_lo;          /* +26  low 16 bits of first cluster */
    uint32_t file_size;            /* +28  bytes; 0 for directories */
} __attribute__((packed));

_Static_assert(sizeof(struct fat_dirent) == 32, "FAT short dirent size");

/* On-disk 32-byte LFN entry (attr == 0x0F). */
struct fat_lfn_entry {
    uint8_t  ord;                  /* +0   sequence; bit 0x40 = last (logical first) */
    uint16_t name1[5];             /* +1   chars 1..5  (UTF-16LE) */
    uint8_t  attr;                 /* +11  always 0x0F */
    uint8_t  type;                 /* +12  always 0 */
    uint8_t  checksum;             /* +13  computed from short name 11 bytes */
    uint16_t name2[6];             /* +14  chars 6..11 (UTF-16LE) */
    uint16_t fst_clus_lo;          /* +26  always 0 */
    uint16_t name3[2];             /* +28  chars 12..13 (UTF-16LE) */
} __attribute__((packed));

_Static_assert(sizeof(struct fat_lfn_entry) == 32, "FAT LFN dirent size");

#define FAT_LFN_LAST   0x40   /* bit set in `ord` for the highest-ord entry */
#define FAT_LFN_CHARS  13     /* characters per LFN entry */

/* ---- public API ----
 *
 * Mount a FAT32 volume on `dev` at `mount_point` (e.g. "/fat"). The
 * device's LBA 0 must contain a valid BPB. Returns VFS_OK on success,
 * a VFS_ERR_* on failure. The driver kmalloc's its own state; the
 * caller does NOT need to free anything (we never unmount yet).
 */
int fat32_mount(const char *mount_point, struct blk_dev *dev);

/* Quick sniff: read LBA 0 of `dev` and decide whether it could be a
 * FAT32 volume. Returns 1 if it looks like FAT32 (BPB sane + 0x55AA
 * trailer + FAT32-specific fields populated), 0 otherwise. Used by
 * `mountfs` to auto-pick the right driver. Cheap (one sector read). */
int fat32_probe(struct blk_dev *dev);

/* M26E: introspection used by usb_msc and the unmount machinery to
 * map an opaque vfs_ops mount-data pointer back to the underlying
 * block device. Returns NULL if `mnt` is NULL.
 *
 * Safe to call from any context -- pure pointer chase, no locking. */
struct blk_dev *fat32_blkdev_of(void *mnt);

/* M26E: address-of-vtable identity for FAT32. Compare a mount's
 * `vfs_ops *` against this before calling fat32_blkdev_of() so you
 * never reinterpret a tobyfs / ramfs / ext4 mount-data pointer as
 * a struct fat32. */
struct vfs_ops;
extern const struct vfs_ops fat32_ops;

#endif /* TOBYOS_FAT32_H */
