/* ext4.h -- read-only ext4 / ext2 filesystem driver.
 *
 * Milestone 23D. Mounts an ext4 partition read-only and exposes it
 * through the existing VFS layer. Supports just enough of the on-disk
 * format to walk directories, look up files, and read their contents:
 *
 *   - 1 KiB / 2 KiB / 4 KiB block sizes
 *   - 128-byte and 256-byte inodes
 *   - directory traversal via the linear ext4_dir_entry_2 walk
 *     (no htree fast-path -- fall back to linear is always correct)
 *   - file data via EITHER:
 *       (a) extent tree (header + leaf entries, depth 0 or 1), OR
 *       (b) legacy block pointers: 12 direct + 1 single-indirect
 *           (double + triple indirect not implemented; > ~1 MB
 *           with 1 KiB blocks needs them, our test image fits)
 *   - root inode is always inode 2; first user inode honours
 *     s_first_ino from the superblock.
 *
 * Out of scope (read-only only):
 *   - journal replay
 *   - inline data
 *   - extended attributes / ACLs / encryption
 *   - htree directory hashes (linear scan still finds the entry)
 *   - symlinks, hard links beyond ref-count read
 *   - quota
 *
 * The driver coexists with tobyfs + FAT32 -- each filesystem owns
 * its own struct vfs_ops table and they're registered at independent
 * mount points (/data, /fat, /usb, /ext).
 */

#ifndef TOBYOS_EXT4_H
#define TOBYOS_EXT4_H

#include <tobyos/types.h>

struct blk_dev;

/* ---- magic numbers + feature bits we recognise ---------------- */

#define EXT4_SUPER_MAGIC     0xEF53u
#define EXT4_ROOT_INODE      2u
#define EXT4_EXT_MAGIC       0xF30Au

/* INCOMPAT bits we either understand or know to refuse. */
#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x0001u
#define EXT4_FEATURE_INCOMPAT_FILETYPE   0x0002u   /* ok */
#define EXT4_FEATURE_INCOMPAT_RECOVER    0x0004u   /* journal needs replay */
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008u
#define EXT4_FEATURE_INCOMPAT_META_BG    0x0010u
#define EXT4_FEATURE_INCOMPAT_EXTENTS    0x0040u   /* ok, we walk these */
#define EXT4_FEATURE_INCOMPAT_64BIT      0x0080u   /* ok if no high blocks */
#define EXT4_FEATURE_INCOMPAT_MMP        0x0100u
#define EXT4_FEATURE_INCOMPAT_FLEX_BG    0x0200u   /* ok */
#define EXT4_FEATURE_INCOMPAT_EA_INODE   0x0400u
#define EXT4_FEATURE_INCOMPAT_DIRDATA    0x1000u
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED  0x2000u   /* s_checksum_seed */
#define EXT4_FEATURE_INCOMPAT_LARGEDIR   0x4000u
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000u  /* refuse */
#define EXT4_FEATURE_INCOMPAT_ENCRYPT    0x10000u
#define EXT4_FEATURE_INCOMPAT_CASEFOLD   0x20000u

/* COMPAT bits. HAS_JOURNAL means a JBD2 journal inode is present
 * (s_journal_inum); we drive it for crash-atomic writes + recovery. */
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL  0x0004u
#define EXT4_FEATURE_COMPAT_EXT_ATTR     0x0008u
#define EXT4_FEATURE_COMPAT_RESIZE_INODE 0x0010u   /* reserved GDT blocks */
#define EXT4_FEATURE_COMPAT_DIR_INDEX    0x0020u   /* htree directories */
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200u
#define EXT4_FEATURE_COMPAT_FAST_COMMIT  0x0400u
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE  0x1000u

/* RO_COMPAT bits: a driver may read a volume with one it does not know,
 * and must not write it (finding 271). */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001u
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE    0x0002u
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR     0x0004u
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE     0x0008u
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM      0x0010u   /* uninit_bg: CRC-16 */
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK     0x0020u
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   0x0040u
#define EXT4_FEATURE_RO_COMPAT_QUOTA         0x0100u
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC      0x0200u
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400u   /* CRC-32C */
#define EXT4_FEATURE_RO_COMPAT_READONLY      0x1000u
#define EXT4_FEATURE_RO_COMPAT_PROJECT       0x2000u
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x10000u

/* bg_flags */
#define EXT4_BG_INODE_UNINIT  0x0001u   /* the inode bitmap is all clear */
#define EXT4_BG_BLOCK_UNINIT  0x0002u   /* the block bitmap is the group's metadata */
#define EXT4_BG_INODE_ZEROED  0x0004u   /* the inode table is zeroed */

/* Conventional inode number of the JBD2 journal (s_journal_inum). */
#define EXT4_JOURNAL_INODE   8u

/* Inode mode (i_mode) high nibble: file type. */
#define EXT4_S_IFMT          0xF000u
#define EXT4_S_IFREG         0x8000u
#define EXT4_S_IFDIR         0x4000u
#define EXT4_S_IFLNK         0xA000u

/* ext4_dir_entry_2.file_type. */
#define EXT4_FT_UNKNOWN      0
#define EXT4_FT_REG_FILE     1
#define EXT4_FT_DIR          2
#define EXT4_FT_SYMLINK      7

/* Inode i_flags bits we care about. */
#define EXT4_INDEX_FL        0x00001000u   /* an htree directory */
#define EXT4_HUGE_FILE_FL    0x00040000u   /* i_blocks in blocks, not sectors */
#define EXT4_EXTENTS_FL      0x00080000u
#define EXT4_EA_INODE_FL     0x00200000u
#define EXT4_INLINE_DATA_FL  0x10000000u

/* ---- superblock (1024 bytes, located at byte offset 1024) ----- */

#pragma pack(push, 1)
struct ext4_super_block {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;     /* block_size = 1024 << this */
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              /* EXT4_SUPER_MAGIC */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;

    /* Dynamic-rev fields (s_rev_level >= 1). */
    uint32_t s_first_ino;          /* first non-reserved inode (often 11) */
    uint16_t s_inode_size;         /* size of inode struct (128 or 256) */
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;

    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;          /* group descriptor size (32 or 64) */
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;    /* 64BIT: high 32 bits of total blocks */
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;              /* EXT4_FLAGS_*: the hash's signedness */
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;      /* 1: CRC-32C */
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;
    /* snapshots, the first and last error, mount options, quota inodes */
    uint8_t  s_snapshot_to_overhead[0x24C - 0x180];
    uint32_t s_backup_bgs[2];      /* sparse_super2's two backup groups */
    uint8_t  s_encrypt_to_prj_quota[0x270 - 0x254];
    uint32_t s_checksum_seed;      /* INCOMPAT_CSUM_SEED: the sums' seed */
    uint8_t  s_time_hi_and_errcodes[0x27C - 0x274];
    uint16_t s_encoding;
    uint16_t s_encoding_flags;
    uint32_t s_orphan_file_inum;
    uint32_t s_reserved[94];
    uint32_t s_checksum;           /* CRC-32C of the bytes before it */
};
_Static_assert(sizeof(struct ext4_super_block) == 1024,
               "ext4 superblock must be exactly 1024 bytes");
_Static_assert(__builtin_offsetof(struct ext4_super_block, s_flags) == 0x160, "s_flags");
_Static_assert(__builtin_offsetof(struct ext4_super_block, s_checksum_type) == 0x175, "s_checksum_type");
_Static_assert(__builtin_offsetof(struct ext4_super_block, s_backup_bgs) == 0x24C, "s_backup_bgs");
_Static_assert(__builtin_offsetof(struct ext4_super_block, s_checksum_seed) == 0x270, "s_checksum_seed");
_Static_assert(__builtin_offsetof(struct ext4_super_block, s_orphan_file_inum) == 0x280, "s_orphan_file_inum");
_Static_assert(__builtin_offsetof(struct ext4_super_block, s_checksum) == 0x3FC, "s_checksum");

/* s_flags: whether a name's characters hash as signed or unsigned */
#define EXT4_FLAGS_SIGNED_HASH    0x0001u
#define EXT4_FLAGS_UNSIGNED_HASH  0x0002u

/* ---- group descriptor (32 or 64 bytes; we read both) ---------- */

struct ext4_group_desc_32 {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};
_Static_assert(sizeof(struct ext4_group_desc_32) == 32,
               "ext4 32-byte group descriptor");

struct ext4_group_desc_64 {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    /* 64BIT-only "high half" follows (all 0 for our small images). */
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
};
_Static_assert(sizeof(struct ext4_group_desc_64) == 64,
               "ext4 64-byte group descriptor");

/* ---- inode (128 or 256 bytes) --------------------------------- */
/* The 128-byte core, then -- in an inode larger than that -- the fields
 * i_extra_isize says it has: the nanoseconds of each time (finding 255),
 * a creation time. Past them the inode holds extended attributes, so only
 * what i_extra_isize covers is ever read or written (ext4.c); ext2.c keeps
 * to the core. */
#define EXT4_GOOD_OLD_INODE_SIZE 128u

struct ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;          /* in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];          /* 15 * 4 = 60 bytes: extents OR ptrs */
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_hi;            /* file sz high 32 bits */
    uint32_t i_obso_faddr;
    uint8_t  i_osd2[12];
    /* the large inode's: 0 where the inode is the old 128 bytes, or where
     * its i_extra_isize stops short of them */
    uint16_t i_extra_isize;        /* bytes of these past the core */
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;        /* nanoseconds << 2 | seconds' bits 32-33 */
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;             /* creation time (statx's btime) */
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
};
_Static_assert(__builtin_offsetof(struct ext4_inode, i_extra_isize) == 128,
               "ext4 inode core must be 128 bytes");
_Static_assert(sizeof(struct ext4_inode) == 160,
               "ext4 inode core + 32 bytes of extra fields");

/* ---- extent tree ---------------------------------------------- */

struct ext4_extent_header {
    uint16_t eh_magic;             /* EXT4_EXT_MAGIC */
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;             /* 0 = leaf, > 0 = index */
    uint32_t eh_generation;
};
_Static_assert(sizeof(struct ext4_extent_header) == 12, "extent header 12 B");

struct ext4_extent {                /* leaf (depth == 0) */
    uint32_t ee_block;             /* logical block number */
    uint16_t ee_len;               /* number of physical blocks */
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
};
_Static_assert(sizeof(struct ext4_extent) == 12, "extent leaf 12 B");

struct ext4_extent_idx {            /* index (depth > 0) */
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
};
_Static_assert(sizeof(struct ext4_extent_idx) == 12, "extent idx 12 B");

/* ---- directory entry (variable length; rec_len walks to next) - */

struct ext4_dir_entry_2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];               /* name_len bytes (no NUL) */
};
#pragma pack(pop)

/* ---- public API ----------------------------------------------- */

/* Sniff a block device for an ext4 superblock. Returns true if the
 * magic + a sensible block size + supported incompat bits all line up.
 * Safe to call against any block device (IDE / NVMe / USB MSC partition
 * or whole disk); reads at most one block. */
int ext4_probe(struct blk_dev *dev);

/* Mount the ext4 filesystem on `dev` at `mount_point`. Returns
 * VFS_OK on success; VFS_ERR_* on any failure. The mount is now
 * READ-WRITE: create / write / unlink / mkdir mutate the filesystem
 * (no journaling -- writes are not crash-atomic; a needs-recovery
 * image is still refused at mount). */
int ext4_mount(const char *mount_point, struct blk_dev *dev);
/* The same, read-only where RDONLY: a volume with a feature this driver
 * cannot keep when it writes mounts only that way (VFS_ERR_INVAL for a
 * read-write mount of one, as Linux's ext4 refuses it; finding 271). */
int ext4_mount_ro(const char *mount_point, struct blk_dev *dev, bool rdonly);

/* Format `dev` as a minimal single-block-group, 4 KiB-block, extents
 * ext4. `with_journal` adds a JBD2 journal (inode 8) so writes become
 * crash-atomic. ext4_format() is the no-journal shorthand (back-compat
 * with the original self-test). Returns VFS_OK / VFS_ERR_*. */
int ext4_format_ex(struct blk_dev *dev, int with_journal);
int ext4_format(struct blk_dev *dev);

/* Self-contained write self-test: formats a ramdev, mounts it, and
 * exercises create / extent-grow write / overwrite / mkdir / unlink /
 * remount-persistence. Returns 0 on all-pass. Emits [EXT4ST] markers.
 * Called from kernel.c under -DEXT4_SELFTEST. */
int ext4_self_test(void);

/* Crash-consistency self-test for the JBD2 journal: formats a journalled
 * ramdev, then injects a power loss at each phase of a transaction
 * (before commit / after commit / mid-checkpoint) and verifies that
 * remount+recovery leaves the filesystem atomically all-or-nothing.
 * Returns 0 on all-pass. Emits [EXT4JT] markers. Called from kernel.c
 * under -DEXT4_JOURNAL_SELFTEST. */
int ext4_journal_self_test(void);

/* The mount's vfs_ops and the device under a mount of it, as the other
 * block filesystems give them (provision_dev_mounted: an ext4 mount was
 * not seen, so a mounted ext4 volume read as unmounted). */
struct vfs_ops;
const struct vfs_ops *ext4_ops_addr(void);
struct blk_dev *ext4_blkdev_of(void *mnt);

#endif /* TOBYOS_EXT4_H */
