/* tobyfs.h -- on-disk format + mount entrypoint for tobyOS's FS.
 *
 * Layout (4 KiB blocks, 4 MiB image = 1024 blocks):
 *   block  0       superblock
 *   block  1       inode bitmap   (1 bit / inode, 256 inodes)
 *   block  2       data  bitmap   (1 bit / data block)
 *   blocks 3..10   inode table    (32 inodes / block * 8 = 256 inodes)
 *   blocks 11..1023 data blocks   (1013 user blocks)
 *
 * Inode 0 is reserved (means "invalid"); inode 1 is always the root.
 * Files use direct block pointers only -- 16 of them, max file size
 * 64 KiB. No indirect blocks, no symlinks, no permissions. The
 * format is shared verbatim with tools/mkfs_tobyfs.c.
 */

#ifndef TOBYOS_TOBYFS_H
#define TOBYOS_TOBYFS_H

#include <tobyos/types.h>

#define TFS_MAGIC          0x546F627946535331ULL  /* "TobyFSS1" */
#define TFS_BLOCK_SIZE     4096u
#define TFS_SECTORS_PER_BLOCK (TFS_BLOCK_SIZE / 512u)
#define TFS_TOTAL_BLOCKS   1024u                  /* 4 MiB */
#define TFS_INODE_COUNT    256u
#define TFS_INODE_SIZE     128u                   /* sizeof(tfs_inode_disk) */
#define TFS_INODES_PER_BLOCK (TFS_BLOCK_SIZE / TFS_INODE_SIZE)
#define TFS_INODE_BLOCKS   (TFS_INODE_COUNT / TFS_INODES_PER_BLOCK)
#define TFS_NDIRECT        16u                    /* 16 * 4 KiB = 64 KiB max file */
#define TFS_NAME_MAX       55u                    /* fits in 56-byte slot incl. NUL */
#define TFS_DIRENT_SIZE    64u
#define TFS_DIRENTS_PER_BLOCK (TFS_BLOCK_SIZE / TFS_DIRENT_SIZE)

/* Fixed region offsets (in block units). Match these in mkfs. */
#define TFS_INODE_BITMAP_BLK  1u
#define TFS_DATA_BITMAP_BLK   2u
#define TFS_INODE_TABLE_BLK   3u
#define TFS_DATA_BLK_START    (TFS_INODE_TABLE_BLK + TFS_INODE_BLOCKS)  /* 11 */

#define TFS_TYPE_FREE 0
#define TFS_TYPE_FILE 1
#define TFS_TYPE_DIR  2

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
#define TFS_MODE_PERMS    00777u
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
    uint8_t  pad[40];
};

struct tfs_dirent_disk {
    uint32_t ino;        /* 0 == empty slot */
    uint32_t reserved;
    char     name[56];   /* NUL-terminated, max 55 chars + NUL */
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
 * direct-block pointers of every allocated inode. Allocates ~12 KiB
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

#endif /* TOBYOS_TOBYFS_H */
