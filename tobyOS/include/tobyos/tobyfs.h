/* tobyfs.h -- on-disk format + mount entrypoint for tobyOS's FS.
 *
 * Layout (4 KiB blocks, 4 MiB image = 1024 blocks):
 *   block  0       superblock
 *   block  1       inode bitmap   (1 bit / inode, 256 inodes)
 *   block  2       data  bitmap   (1 bit / data block)
 *   blocks 3..10   inode table    (32 inodes / block * 8 = 256 inodes)
 *   blocks 11..991  data blocks   (981 user blocks)
 *   blocks 992..1023 journal      (32 blocks, write-ahead log)
 *
 * Inode 0 is reserved (means "invalid"); inode 1 is always the root.
 * Files use 16 direct block pointers (64 KiB) plus one single-indirect
 * block pointer (~4 MiB max file size). No double-indirect, no
 * symlinks. The format is shared verbatim with tools/mkfs_tobyfs.c.
 */

#ifndef TOBYOS_TOBYFS_H
#define TOBYOS_TOBYFS_H

#include <tobyos/types.h>

#define TFS_MAGIC          0x546F627946535331ULL  /* "TobyFSS1" */
#define TFS_BLOCK_SIZE     4096u
#define TFS_SECTORS_PER_BLOCK (TFS_BLOCK_SIZE / 512u)
/* Default geometry for the smallest (legacy 4 MiB) image. Volumes are now
 * SIZED DYNAMICALLY at format time from the backing device; these are the
 * minimum/fallback values and the geometry an old 1024-block image carries.
 * The real geometry always comes from the superblock at mount time. */
#define TFS_TOTAL_BLOCKS   1024u                  /* 4 MiB minimum */
#define TFS_INODE_COUNT    256u                   /* minimum inode count */
#define TFS_BITS_PER_BLOCK (TFS_BLOCK_SIZE * 8u)  /* 32768 bitmap bits/block */
#define TFS_INODE_SIZE     128u                   /* sizeof(tfs_inode_disk) */
#define TFS_INODES_PER_BLOCK (TFS_BLOCK_SIZE / TFS_INODE_SIZE)
#define TFS_INODE_BLOCKS   (TFS_INODE_COUNT / TFS_INODES_PER_BLOCK)
#define TFS_NDIRECT        16u                    /* 16 * 4 KiB = 64 KiB direct */
#define TFS_INDIRECT_ENTRIES (TFS_BLOCK_SIZE / sizeof(uint32_t))  /* 1024 entries */
/* Double-indirect: one L1 block of 1024 pointers, each -> an L2 block of 1024
 * data-block pointers => 1024*1024 blocks = 4 GiB of addressable data. */
#define TFS_DBL_INDIRECT_ENTRIES (TFS_INDIRECT_ENTRIES * TFS_INDIRECT_ENTRIES)
/* First file-block index reachable only via the double-indirect tree. */
#define TFS_DBL_INDIRECT_BASE (TFS_NDIRECT + TFS_INDIRECT_ENTRIES)
/* Total addressable bytes overflow uint32_t, so compute in 64-bit and clamp
 * to the on-disk `size` field's range (uint32_t => 4 GiB - 1). */
#define TFS_ADDRESSABLE_BYTES \
    (((uint64_t)(TFS_NDIRECT + TFS_INDIRECT_ENTRIES + TFS_DBL_INDIRECT_ENTRIES)) \
     * (uint64_t)TFS_BLOCK_SIZE)
#define TFS_MAX_FILE_SIZE    0xFFFFFFFFu           /* capped by uint32_t size */
#define TFS_NAME_MAX       55u                    /* fits in 56-byte slot incl. NUL */
#define TFS_DIRENT_SIZE    64u
#define TFS_DIRENTS_PER_BLOCK (TFS_BLOCK_SIZE / TFS_DIRENT_SIZE)

/* LEGACY region offsets (block units) for the default 4 MiB image. With
 * dynamic sizing the true offsets are read from the superblock at mount;
 * each region's span is derived from consecutive start deltas (the regions
 * are contiguous), so NO on-disk format change is needed and old 1024-block
 * images still mount with these exact values. format() recomputes them from
 * the device size. */
#define TFS_INODE_BITMAP_BLK  1u
#define TFS_DATA_BITMAP_BLK   2u
#define TFS_INODE_TABLE_BLK   3u
#define TFS_DATA_BLK_START    (TFS_INODE_TABLE_BLK + TFS_INODE_BLOCKS)  /* 11 */

/* Write-ahead journal (last TFS_JOURNAL_BLOCKS of the volume). The journal
 * size is fixed; its START is dynamic (volume end - size) and is recorded in
 * the superblock reserved fields, so it scales with the volume. */
#define TFS_JOURNAL_BLOCKS    32u
#define TFS_JOURNAL_START     (TFS_TOTAL_BLOCKS - TFS_JOURNAL_BLOCKS)  /* 992 */
#define TFS_USABLE_BLOCKS     TFS_JOURNAL_START                        /* 992 */

/* Largest volume we'll format/mount, in 4 KiB blocks. Caps RAM used by the
 * cached bitmaps + bounds the format math. 1 GiB = 262144 blocks => an 8-block
 * data bitmap. Plenty for /data and big-file testing; raise if ever needed. */
#define TFS_MAX_TOTAL_BLOCKS  (256u * 1024u)      /* 1 GiB */

#define TFS_TXN_BEGIN_MAGIC   0x4A4F5552U  /* "JOUR" */
#define TFS_TXN_COMMIT_MAGIC  0x434F4D54U  /* "COMT" */
#define TFS_TXN_MAX_BLOCKS    16u

#define TFS_TYPE_FREE 0
#define TFS_TYPE_FILE 1
#define TFS_TYPE_DIR  2
/* Linux slice 7: on-disk symbolic link. The target string is stored as the
 * inode's DATA (size = strlen), so no new on-disk structure is needed and
 * legacy volumes are unaffected -- they simply contain no type-3 inodes.
 *
 * Before this, symlinks lived only in a fixed 128-entry RAM table in vfs.c:
 * global to the machine, not per-filesystem, and gone on reboot. A real
 * distro rootfs is mostly symlinks (Alpine's minirootfs has 335, nearly all
 * busybox applet links), so that table both overflowed AND would have left
 * every link dangling after a restart. */
#define TFS_TYPE_SYMLINK 3

#define TFS_ROOT_INO  1u

/* Permission/identity bits stored in the inode (milestone 15). The
 * low 9 bits are classic Unix-style rwxrwxrwx for owner/group/other.
 * Bit MODE_VALID distinguishes "the kernel set this on purpose" from
 * "this is a legacy inode" -- old disks (formatted before milestone
 * 15) have mode == 0, which the VFS treats as fully accessible so
 * existing data files keep working without a manual reformat. */
#define TFS_MODE_R_OTHER  00004u
#define TFS_MODE_W_OTHER  00002u
#define TFS_MODE_X_OTHER  00001u
#define TFS_MODE_R_OWNER  00400u
#define TFS_MODE_W_OWNER  00200u
#define TFS_MODE_X_OWNER  00100u
#define TFS_MODE_R_GROUP  00040u
#define TFS_MODE_W_GROUP  00020u
#define TFS_MODE_X_GROUP  00010u
/* Slice 2: 00777 -> 07777, matching VFS_MODE_PERMS, so setuid/setgid/sticky
 * round-trip through the on-disk inode (mode is a uint32; nothing else to do). */
#define TFS_MODE_PERMS    07777u
#define TFS_MODE_VALID    0x10000u   /* "this inode has explicit perms" */

/* Defaults for newly-created inodes. Files are owner-rw + world-r,
 * directories are owner-rwx + world-rx (matches POSIX 0644 / 0755
 * after umask = 0022). Both have MODE_VALID set so the VFS enforces
 * the bits. */
#define TFS_DEFAULT_FILE_MODE (00644u | TFS_MODE_VALID)
#define TFS_DEFAULT_DIR_MODE  (00755u | TFS_MODE_VALID)

struct tfs_superblock {
    uint64_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t inode_bitmap_blk;
    uint32_t data_bitmap_blk;
    uint32_t inode_table_blk;
    uint32_t data_blk_start;
    uint32_t root_ino;
    uint32_t reserved[24];
};

struct tfs_inode_disk {
    uint16_t type;
    uint16_t nlink;
    uint32_t size;
    uint32_t mtime;
    /* Milestone 15: repurpose the three reserved 32-bit slots without
     * changing the on-disk layout. mode==0 (no MODE_VALID bit) means
     * "legacy inode" -- the VFS treats the file as fully accessible so
     * disks formatted before milestone 15 still work. */
    uint32_t mode;                  /* TFS_MODE_* bits */
    uint32_t uid;                   /* owner user id */
    uint32_t gid;                   /* owner group id */
    uint32_t direct[TFS_NDIRECT];   /* 64 bytes */
    uint32_t indirect;               /* single-indirect block pointer */
    /* Double-indirect block pointer. Carved out of the former pad[] WITHOUT
     * changing the on-disk inode size (still TFS_INODE_SIZE), so legacy
     * disks still mount: those bytes were zero, and dbl_indirect==0 means
     * "no double-indirect blocks" -- identical to a freshly-created inode. */
    uint32_t dbl_indirect;           /* double-indirect block pointer */
    uint8_t  pad[32];
};

struct tfs_dirent_disk {
    uint32_t ino;        /* 0 == empty slot */
    uint32_t reserved;
    char     name[56];   /* NUL-terminated, max 55 chars + NUL */
};

/* On-disk journal header (first block of a transaction in the journal). */
struct tfs_txn_header {
    uint32_t magic;                          /* TFS_TXN_BEGIN_MAGIC */
    uint32_t txn_id;
    uint32_t block_count;
    uint32_t dest_blocks[TFS_TXN_MAX_BLOCKS];
};

/* On-disk journal commit marker (follows the data blocks). */
struct tfs_txn_commit {
    uint32_t magic;                          /* TFS_TXN_COMMIT_MAGIC */
    uint32_t txn_id;
};

struct blk_dev;

/* Mount the tobyfs image found on `dev` at the given VFS mount point
 * (e.g. "/data"). Reads the superblock; rejects if magic is wrong or
 * geometry mismatches the build-time constants above. Returns 0 on
 * success or a VFS_ERR_* code. */
int tobyfs_mount(const char *mount_point, struct blk_dev *dev);

/* Milestone 20: in-kernel formatter. Writes a fresh tobyfs image onto
 * `dev` starting at LBA 0 of the device -- identical to what
 * tools/mkfs_tobyfs.c produces on the host. Pair with blk_offset_wrap
 * to format a region on a larger disk without touching the rest.
 *
 * The device must be at least TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK
 * sectors; blocks beyond that are left untouched. Returns 0 on success
 * or VFS_ERR_IO / VFS_ERR_INVAL. */
int tobyfs_format(struct blk_dev *dev);

/* Identification helpers for VFS mount-table walks (see vfs_iter_mounts):
 * tobyfs_ops_addr() returns the static vtable pointer so callers can
 * recognise tobyfs mounts; tobyfs_blkdev_of() maps a recognised mount's
 * opaque mount-data back to its backing block device. */
const void *tobyfs_ops_addr(void);
struct blk_dev *tobyfs_blkdev_of(void *mnt);

/* ============================================================
 *  Milestone 28E: filesystem integrity checker.
 *
 *  Severity ladder (matches `abi_fscheck_report.status`):
 *    TFS_CHECK_OK          - pristine, mountable
 *    TFS_CHECK_WARN        - minor anomalies (orphaned bits in
 *                            data bitmap, allocated-but-empty inodes);
 *                            mount allowed, fscheck reports
 *    TFS_CHECK_FATAL       - structural corruption that would have
 *                            us walk off arrays or trash unrelated
 *                            disk: bad superblock, geometry mismatch,
 *                            root inode missing/wrong type, dirent
 *                            referencing impossible inode index,
 *                            inode pointing into metadata blocks,
 *                            etc. Mount must REFUSE.
 *
 *  The structure lives outside the abi_fscheck_report so the kernel
 *  can store more detail than userland needs. The conversion to the
 *  ABI struct happens in the syscall handler.
 * ============================================================ */
#define TFS_CHECK_OK     0
#define TFS_CHECK_WARN   1
#define TFS_CHECK_FATAL  2

#define TFS_CHECK_DETAIL_MAX 192

struct tobyfs_check {
    int      severity;             /* TFS_CHECK_*                  */
    uint32_t errors;               /* count of issues found         */
    uint32_t repaired;             /* count auto-fixed (always 0 in v1) */
    uint32_t inodes_used;
    uint32_t inodes_total;
    uint32_t data_blocks_used;
    uint32_t data_blocks_total;
    uint64_t bytes_total;
    uint64_t bytes_free;
    char     detail[TFS_CHECK_DETAIL_MAX];
};

/* Run a structural check over a tobyfs image without mounting it.
 * Reads the superblock, both bitmaps, the inode table, and walks the
 * block pointers (direct + indirect) of every allocated inode. Allocates ~12 KiB
 * of scratch on the kernel heap for the duration of the call.
 *
 * Returns 0 on success (regardless of severity -- inspect result.severity).
 * Returns -VFS_ERR_IO on any underlying read error and a -VFS_ERR_NOMEM
 * if the scratch allocation fails. */
int tobyfs_check_dev(struct blk_dev *dev, struct tobyfs_check *out);

/* Run a check against an already-mounted tobyfs whose mount data was
 * returned by tobyfs_mount(). Same return semantics as
 * tobyfs_check_dev. Used by ABI_SYS_FS_CHECK so userland can validate
 * the live /data without unmounting it. */
int tobyfs_check_mounted(void *mount_data, struct tobyfs_check *out);

/* M28E self-test: build a tiny in-RAM tobyfs image (4 MiB on the
 * kernel heap), format it, then exercise tobyfs_check_dev() against
 *
 *   1. the freshly-formatted image  -> expects TFS_CHECK_OK
 *   2. the same image with the superblock magic deliberately stomped
 *      to garbage                   -> expects TFS_CHECK_FATAL
 *
 * Used by the M28E boot harness to prove the corruption-detection
 * path lights up without going anywhere near the live /data disk.
 * Prints `[m28e] ...` diagnostic lines via kprintf along the way.
 *
 * On success returns 0 and `clean_out` / `bad_out` carry the kernel's
 * verdicts. On any allocator / I/O failure returns a negative VFS_ERR_*
 * and the output structs may be partially populated. */
int tobyfs_self_test(struct tobyfs_check *clean_out,
                     struct tobyfs_check *bad_out);

/* Crash-consistency + stress harness. Injects a power loss at each journal
 * phase of an operation and proves remount+recovery is atomic (rollback /
 * replay), then churns the filesystem with hundreds of mixed operations
 * (incl. a double-indirect 4.5 MiB file) verifying every byte and running
 * the integrity checker throughout. Returns 0 on all-pass. Emits [TFST]
 * markers. Self-contained ramdev. Called from kernel.c under
 * -DTOBYFS_STRESS_SELFTEST. */
int tobyfs_crash_stress_test(void);

#endif /* TOBYOS_TOBYFS_H */
