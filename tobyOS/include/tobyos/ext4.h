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
#define EXT4_FEATURE_INCOMPAT_FILETYPE   0x0002u   /* ok */
#define EXT4_FEATURE_INCOMPAT_RECOVER    0x0004u   /* journal needs replay */
#define EXT4_FEATURE_INCOMPAT_EXTENTS    0x0040u   /* ok, we walk these */
#define EXT4_FEATURE_INCOMPAT_64BIT      0x0080u   /* ok if no high blocks */
#define EXT4_FEATURE_INCOMPAT_FLEX_BG    0x0200u   /* ok */
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000u  /* refuse */

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

/* Inode i_flags bit we care about. */
#define EXT4_EXTENTS_FL      0x00080000u

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

    /* The real Linux ext4 SB has dozens of additional fields after this
     * point (s_flags, s_mmp_*, s_first_error_*, s_mount_opts, ...). We
     * never read them in this RO driver, so we collapse the rest into a
     * single padding array sized so the struct is exactly 1024 bytes.
     * Used fields above end at offset 352, so pad = 1024 - 352 = 672. */
    uint8_t  s_padding[672];
};
_Static_assert(sizeof(struct ext4_super_block) == 1024,
               "ext4 superblock must be exactly 1024 bytes");

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
/* We only read the first 128 bytes -- the extra ext4 fields beyond
 * that (i_file_acl_high, i_extra_isize, ...) aren't needed for RO
 * traversal. */

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
};
_Static_assert(sizeof(struct ext4_inode) == 128,
               "ext4 inode core must be 128 bytes");

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
 * VFS_OK on success; VFS_ERR_* on any failure. The mount is
 * read-only -- attempts to write through the VFS layer return
 * VFS_ERR_ROFS. */
int ext4_mount(const char *mount_point, struct blk_dev *dev);

#endif /* TOBYOS_EXT4_H */
