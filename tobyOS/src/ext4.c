/* ext4.c -- read-only ext2/ext3/ext4 driver (milestone 23D).
 *
 * Mounts an ext4 partition and exposes it through the same `vfs_ops`
 * table that ramfs / tobyfs / fat32 use. The driver only knows about
 * `struct blk_dev *`, so it works on whole disks AND GPT partitions
 * AND USB-MSC LUNs without any glue code at the filesystem layer.
 *
 * What we read from the on-disk format
 * ------------------------------------
 *
 *   - Superblock at byte offset 1024 (sector 2 if 512-byte sectors).
 *     Validates magic, supported INCOMPAT bits, and a sane block size.
 *
 *   - Group Descriptor Table starting at block (s_first_data_block + 1).
 *     We read the entire GDT into memory at mount time. For our small
 *     test images that's at most a few KiB; for huge filesystems we
 *     would lazy-cache it, but RO mounts don't churn the GDT so a
 *     single read on mount is fine.
 *
 *   - Inode table per-group via the descriptor's inode_table_lo (+_hi if
 *     the s_desc_size field signals 64-byte descriptors).
 *
 *   - Directory entries via the linear ext4_dir_entry_2 walk -- htree
 *     fast-path is intentionally NOT implemented because a linear scan
 *     always finds the entry (htree just speeds it up).
 *
 *   - File data via either:
 *       * the extent tree (header + entries; depth 0 leaf or depth 1
 *         index pointing to leaves), OR
 *       * legacy block pointers (12 direct + single-indirect).
 *     Double + triple indirect aren't implemented (test files are
 *     small enough to never need them).
 *
 * What we DELIBERATELY skip
 * -------------------------
 *
 *   - Writes. All of vfs_create / vfs_unlink / vfs_write return
 *     VFS_ERR_ROFS.
 *   - Journal replay. We refuse to mount if EXT4_FEATURE_INCOMPAT_RECOVER
 *     is set (a needs-recovery filesystem can't be safely read).
 *   - Inline data + extended attributes + encryption.
 *   - Symlinks (the inode parses fine, but we don't follow them).
 *   - Hashed-tree directory index (htree). We linear-scan instead,
 *     which the spec mandates as the fallback path anyway.
 */

#include <tobyos/ext4.h>
#include <tobyos/jbd2.h>
#include <tobyos/blk.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* JBD2 stores every multi-byte field big-endian; we run little-endian. */
static inline uint16_t be16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t be32(uint32_t x) { return __builtin_bswap32(x); }

/* ---- write-ahead journal (JBD2) transaction staging ---- */

/* One staged block write inside the active transaction: its home block
 * number plus a full-block snapshot of the bytes to commit there. */
struct ext4_txn_ent { uint64_t blk; uint8_t *data; };

/* An in-progress atomic operation. Every write_block() during the op is
 * captured here instead of hitting the disk; on txn_commit() the whole
 * set is logged to the journal, committed, then checkpointed to the home
 * locations. The entries array grows on demand. */
struct ext4_txn {
    struct ext4_txn_ent *ents;
    int    n;
    int    cap;
    bool   nomem;        /* a staging allocation failed -> abort the op */
};

/* Crash-injection knob for the journal self-test. 0 == disabled; when
 * set, txn_commit() stops at the chosen phase WITHOUT clearing on-disk
 * state, modelling a power loss so the next mount must recover. */
enum {
    JBD2_CRASH_NONE = 0,
    JBD2_CRASH_BEFORE_COMMIT,   /* descriptor+data logged, no commit block */
    JBD2_CRASH_AFTER_COMMIT,    /* committed, nothing checkpointed */
    JBD2_CRASH_MID_CHECKPOINT,  /* committed, only half the homes written */
};
static int g_jbd_crash_phase = JBD2_CRASH_NONE;

/* ---- in-memory state ---- */

struct ext4 {
    struct blk_dev *dev;

    uint32_t block_size;          /* 1024, 2048, 4096 */
    uint32_t sectors_per_block;   /* block_size / 512 */
    uint64_t total_blocks;
    uint32_t inode_size;          /* 128 or 256 */
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_ino;           /* usually 11 for ext4 */
    uint32_t feature_incompat;
    uint16_t desc_size;           /* 32 or 64 */
    uint32_t first_data_block;    /* 0 for >= 2 KiB blocks, 1 for 1 KiB */
    bool     desc_64bit;          /* read the high half of group desc */

    uint32_t total_inodes;        /* s_inodes_count (write: free bookkeeping) */
    bool     feat_64bit;          /* INCOMPAT_64BIT -> 64-bit free counts */

    /* Cached scratch buffers (allocated at mount). */
    uint8_t *blk_buf;             /* one block */
    uint8_t *blk_buf2;            /* spare for indirect / extent walks */
    uint8_t *gdt_buf;             /* full GDT image */
    uint8_t *sb_buf;              /* cached 1024-byte superblock (write-back) */
    uint32_t gdt_bytes;
    uint32_t group_count;

    /* ---- JBD2 write-ahead journal ---- */
    bool     has_journal;         /* journal present AND we can drive it */
    uint64_t j_phys_first;        /* physical block of journal block 0 (j SB) */
    uint32_t j_blocks;            /* journal length in fs blocks (s_maxlen) */
    uint32_t j_first;             /* first usable log block (s_first, usually 1) */
    uint32_t j_sequence;          /* sequence to stamp on the next transaction */
    struct ext4_txn *txn;         /* active transaction, or NULL (writes direct) */
};

/* Per-handle state for an open file. We snapshot the inode at open
 * time so subsequent reads don't have to chase the GDT/inode table
 * again on every byte. The driver is RO so the inode contents can't
 * change underneath us. */
struct ext4_filepriv {
    uint32_t          inode_no;
    struct ext4_inode in;
    uint64_t          file_size;
};

/* Per-handle state for an open directory. Fully materialised on
 * opendir, mirroring the tobyfs / fat32 pattern. */
struct ext4_diriter {
    struct vfs_dirent *ents;
    size_t             count;
};

/* ---- low-level helpers ---- */

/* Find a block already staged in the active transaction (NULL if none). */
static struct ext4_txn_ent *txn_find(struct ext4 *fs, uint64_t blk) {
    if (!fs->txn) return NULL;
    for (int i = 0; i < fs->txn->n; i++)
        if (fs->txn->ents[i].blk == blk) return &fs->txn->ents[i];
    return NULL;
}

static int read_block(struct ext4 *fs, uint64_t blk, void *buf) {
    if (blk >= fs->total_blocks) return VFS_ERR_INVAL;
    /* Read-your-writes: a block staged in the open transaction reflects
     * the not-yet-committed contents. */
    struct ext4_txn_ent *e = txn_find(fs, blk);
    if (e) { memcpy(buf, e->data, fs->block_size); return VFS_OK; }
    uint64_t lba = blk * fs->sectors_per_block;
    if (blk_read(fs->dev, lba, fs->sectors_per_block, buf) != 0) {
        return VFS_ERR_IO;
    }
    return VFS_OK;
}

/* Read `len` bytes starting at byte offset `byte_off` inside block `blk`.
 * `len` MUST fit inside the block (caller chunks larger reads). */
static int read_block_partial(struct ext4 *fs, uint64_t blk,
                              uint32_t byte_off, uint32_t len, void *out) {
    if (byte_off + len > fs->block_size) return VFS_ERR_INVAL;
    int rc = read_block(fs, blk, fs->blk_buf);
    if (rc != VFS_OK) return rc;
    memcpy(out, fs->blk_buf + byte_off, len);
    return VFS_OK;
}

/* ---- group descriptor + inode lookup ---- */

/* Pull the 64-bit "physical block number of inode table for group g"
 * out of our cached GDT, honouring 64-byte descriptors when present. */
static uint64_t group_inode_table(const struct ext4 *fs, uint32_t g) {
    const uint8_t *p = fs->gdt_buf + (uint64_t)g * fs->desc_size;
    const struct ext4_group_desc_32 *d32 =
        (const struct ext4_group_desc_32 *)p;
    uint64_t lo = d32->bg_inode_table_lo;
    if (fs->desc_64bit && fs->desc_size >= 64) {
        const struct ext4_group_desc_64 *d64 =
            (const struct ext4_group_desc_64 *)p;
        lo |= ((uint64_t)d64->bg_inode_table_hi) << 32;
    }
    return lo;
}

/* Read inode number `ino` (1-based) into `out`. Reads exactly the
 * first 128 bytes -- the extra ext4 fields beyond that aren't needed
 * for read-only operation. */
/* uid/gid are split: the low 16 bits sit in the inode proper, the high 16
 * in the Linux osd2 area. Reading only the low half silently aliases every
 * uid above 65535 onto a different user -- and user namespaces hand out
 * exactly those (the standard 100000+ ranges). */
#define EXT4_OSD2_UID_HIGH 4
#define EXT4_OSD2_GID_HIGH 6

static uint32_t osd2_half(const struct ext4_inode *in, int off) {
    return (uint32_t)in->i_osd2[off] | ((uint32_t)in->i_osd2[off + 1] << 8);
}
static void osd2_set_half(struct ext4_inode *in, int off, uint32_t v) {
    in->i_osd2[off]     = (uint8_t)(v & 0xFF);
    in->i_osd2[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}
static uint32_t ext4_inode_uid(const struct ext4_inode *in) {
    return (uint32_t)in->i_uid | (osd2_half(in, EXT4_OSD2_UID_HIGH) << 16);
}
static uint32_t ext4_inode_gid(const struct ext4_inode *in) {
    return (uint32_t)in->i_gid | (osd2_half(in, EXT4_OSD2_GID_HIGH) << 16);
}
static void ext4_inode_set_uid(struct ext4_inode *in, uint32_t uid) {
    in->i_uid = (uint16_t)(uid & 0xFFFF);
    osd2_set_half(in, EXT4_OSD2_UID_HIGH, uid >> 16);
}
static void ext4_inode_set_gid(struct ext4_inode *in, uint32_t gid) {
    in->i_gid = (uint16_t)(gid & 0xFFFF);
    osd2_set_half(in, EXT4_OSD2_GID_HIGH, gid >> 16);
}

static uint64_t ext4_now_secs(void);

static int read_inode(struct ext4 *fs, uint32_t ino, struct ext4_inode *out) {
    if (ino == 0) return VFS_ERR_INVAL;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->group_count) return VFS_ERR_INVAL;

    uint64_t itab = group_inode_table(fs, group);
    uint64_t byte_off_in_group = (uint64_t)idx * fs->inode_size;
    uint64_t blk = itab + byte_off_in_group / fs->block_size;
    uint32_t off = (uint32_t)(byte_off_in_group % fs->block_size);

    /* The on-disk inode is `inode_size` bytes; we only ever consume
     * the first sizeof(struct ext4_inode) = 128 bytes. */
    return read_block_partial(fs, blk, off, sizeof(*out), out);
}

/* ================================================================
 * WRITE SUPPORT (milestone: ext4 rw)
 *
 * Adds in-place overwrite, file growth (extent + legacy), and
 * create/unlink/mkdir on a *clean* (no-journal-replay) ext4/ext2.
 * No journaling: writes are not crash-atomic, which is fine for the
 * clean-FS-only mount gate. The bitmap / free-count / dir-entry logic
 * mirrors the proven read-write ext2 driver (src/ext2.c); the
 * ext4-specific part is depth-0 extent-tree growth.
 * ================================================================ */

/* Stage `buf` for home block `blk` into the active transaction, replacing
 * any earlier staged copy of the same block. */
static int txn_stage(struct ext4 *fs, uint64_t blk, const void *buf) {
    struct ext4_txn *t = fs->txn;
    struct ext4_txn_ent *e = txn_find(fs, blk);
    if (!e) {
        if (t->n == t->cap) {
            int ncap = t->cap ? t->cap * 2 : 16;
            struct ext4_txn_ent *ne =
                kmalloc((size_t)ncap * sizeof(*ne));
            if (!ne) { t->nomem = true; return VFS_ERR_NOMEM; }
            if (t->n) memcpy(ne, t->ents, (size_t)t->n * sizeof(*ne));
            if (t->ents) kfree(t->ents);
            t->ents = ne; t->cap = ncap;
        }
        e = &t->ents[t->n];
        e->blk = blk;
        e->data = kmalloc(fs->block_size);
        if (!e->data) { t->nomem = true; return VFS_ERR_NOMEM; }
        t->n++;
    }
    memcpy(e->data, buf, fs->block_size);
    return VFS_OK;
}

static int write_block(struct ext4 *fs, uint64_t blk, const void *buf) {
    if (blk >= fs->total_blocks) return VFS_ERR_INVAL;
    if (fs->txn) return txn_stage(fs, blk, buf);
    uint64_t lba = blk * fs->sectors_per_block;
    if (blk_write(fs->dev, lba, fs->sectors_per_block, buf) != 0)
        return VFS_ERR_IO;
    return VFS_OK;
}

/* Write back the cached 1024-byte superblock (free counts, state). The
 * SB always lives at byte offset 1024 = LBA 2 (512-byte sectors).
 *
 * Inside a transaction we can't do that sub-block write directly: the SB
 * shares an fs block with the boot area (and, for 1 KiB blocks, sits at a
 * block boundary). So we read-modify-write the whole containing block and
 * stage *that*, keeping the journal at fs-block granularity. */
static int flush_superblock(struct ext4 *fs) {
    if (!fs->sb_buf) return VFS_OK;
    if (fs->txn) {
        uint64_t sbblk = 1024u / fs->block_size;
        uint32_t sboff = 1024u % fs->block_size;
        uint8_t *tmp = kmalloc(fs->block_size);
        if (!tmp) return VFS_ERR_NOMEM;
        int rc = read_block(fs, sbblk, tmp);
        if (rc != VFS_OK) { kfree(tmp); return rc; }
        memcpy(tmp + sboff, fs->sb_buf, 1024);
        rc = write_block(fs, sbblk, tmp);
        kfree(tmp);
        return rc;
    }
    if (blk_write(fs->dev, 2, 2, fs->sb_buf) != 0) return VFS_ERR_IO;
    return VFS_OK;
}

/* Write the cached SB straight to disk, bypassing the transaction. Used
 * by the journal machinery itself (marking the log dirty/clean) where the
 * SB change must NOT be part of the staged set. */
static int flush_superblock_direct(struct ext4 *fs) {
    if (!fs->sb_buf) return VFS_OK;
    if (blk_write(fs->dev, 2, 2, fs->sb_buf) != 0) return VFS_ERR_IO;
    return VFS_OK;
}

/* Write the cached GDT image back. */
static int flush_gdt(struct ext4 *fs) {
    uint32_t gdt_blocks = (fs->gdt_bytes + fs->block_size - 1) / fs->block_size;
    uint32_t gdt_first = fs->first_data_block + 1;
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        int rc = write_block(fs, gdt_first + i,
                             fs->gdt_buf + i * fs->block_size);
        if (rc != VFS_OK) return rc;
    }
    return VFS_OK;
}

/* Write inode `ino` back: read-modify-write its slot so the ext4 extra
 * fields beyond the 128-byte core are preserved. */
static int write_inode(struct ext4 *fs, uint32_t ino,
                       const struct ext4_inode *in) {
    if (ino == 0) return VFS_ERR_INVAL;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->group_count) return VFS_ERR_INVAL;

    uint64_t itab = group_inode_table(fs, group);
    uint64_t byte_off = (uint64_t)idx * fs->inode_size;
    uint64_t blk = itab + byte_off / fs->block_size;
    uint32_t off = (uint32_t)(byte_off % fs->block_size);

    int rc = read_block(fs, blk, fs->blk_buf);
    if (rc != VFS_OK) return rc;
    memcpy(fs->blk_buf + off, in, sizeof(*in));
    return write_block(fs, blk, fs->blk_buf);
}

/* ---- group descriptor bitmap accessors ---- */

static uint64_t group_block_bitmap(const struct ext4 *fs, uint32_t g) {
    const struct ext4_group_desc_32 *d =
        (const struct ext4_group_desc_32 *)(fs->gdt_buf +
                                            (uint64_t)g * fs->desc_size);
    uint64_t lo = d->bg_block_bitmap_lo;
    if (fs->desc_64bit && fs->desc_size >= 64)
        lo |= ((uint64_t)((const struct ext4_group_desc_64 *)d)
                   ->bg_block_bitmap_hi) << 32;
    return lo;
}

static uint64_t group_inode_bitmap(const struct ext4 *fs, uint32_t g) {
    const struct ext4_group_desc_32 *d =
        (const struct ext4_group_desc_32 *)(fs->gdt_buf +
                                            (uint64_t)g * fs->desc_size);
    uint64_t lo = d->bg_inode_bitmap_lo;
    if (fs->desc_64bit && fs->desc_size >= 64)
        lo |= ((uint64_t)((const struct ext4_group_desc_64 *)d)
                   ->bg_inode_bitmap_hi) << 32;
    return lo;
}

static struct ext4_group_desc_32 *group_desc_mut(struct ext4 *fs, uint32_t g) {
    return (struct ext4_group_desc_32 *)(fs->gdt_buf +
                                         (uint64_t)g * fs->desc_size);
}

/* Adjust a group's free-block count by `delta` (+/-1), honouring the
 * 64-byte-descriptor high half, and mirror it into the superblock's
 * free-block count. Caller flushes GDT + SB. */
static void adj_free_blocks(struct ext4 *fs, uint32_t g, int delta) {
    struct ext4_group_desc_32 *gd = group_desc_mut(fs, g);
    uint32_t v = gd->bg_free_blocks_count_lo;
    if (fs->desc_64bit && fs->desc_size >= 64)
        v |= (uint32_t)((struct ext4_group_desc_64 *)gd)
                 ->bg_free_blocks_count_hi << 16;
    v = (uint32_t)((int)v + delta);
    gd->bg_free_blocks_count_lo = (uint16_t)(v & 0xFFFF);
    if (fs->desc_64bit && fs->desc_size >= 64)
        ((struct ext4_group_desc_64 *)gd)->bg_free_blocks_count_hi =
            (uint16_t)(v >> 16);

    if (fs->sb_buf) {
        struct ext4_super_block *sb = (struct ext4_super_block *)fs->sb_buf;
        uint64_t fb = sb->s_free_blocks_count_lo;
        if (fs->feat_64bit) fb |= ((uint64_t)sb->s_free_blocks_count_hi) << 32;
        fb = (uint64_t)((int64_t)fb + delta);
        sb->s_free_blocks_count_lo = (uint32_t)(fb & 0xFFFFFFFFu);
        if (fs->feat_64bit) sb->s_free_blocks_count_hi = (uint32_t)(fb >> 32);
    }
}

static void adj_free_inodes(struct ext4 *fs, uint32_t g, int delta) {
    struct ext4_group_desc_32 *gd = group_desc_mut(fs, g);
    uint32_t v = gd->bg_free_inodes_count_lo;
    if (fs->desc_64bit && fs->desc_size >= 64)
        v |= (uint32_t)((struct ext4_group_desc_64 *)gd)
                 ->bg_free_inodes_count_hi << 16;
    v = (uint32_t)((int)v + delta);
    gd->bg_free_inodes_count_lo = (uint16_t)(v & 0xFFFF);
    if (fs->desc_64bit && fs->desc_size >= 64)
        ((struct ext4_group_desc_64 *)gd)->bg_free_inodes_count_hi =
            (uint16_t)(v >> 16);

    if (fs->sb_buf) {
        struct ext4_super_block *sb = (struct ext4_super_block *)fs->sb_buf;
        sb->s_free_inodes_count = (uint32_t)((int)sb->s_free_inodes_count + delta);
    }
}

static void adj_used_dirs(struct ext4 *fs, uint32_t g, int delta) {
    struct ext4_group_desc_32 *gd = group_desc_mut(fs, g);
    gd->bg_used_dirs_count_lo = (uint16_t)((int)gd->bg_used_dirs_count_lo + delta);
}

/* ---- block + inode bitmap allocation ---- */

/* Allocate one data block, preferring group `pref_group`. Sets the bit,
 * writes the bitmap, decrements the group + superblock free counts.
 * Sequential calls on a fresh FS return consecutive blocks, so a
 * sequential writer's extent stays a single contiguous run. */
static int ext4_alloc_block(struct ext4 *fs, uint32_t pref_group,
                            uint32_t *out_blk) {
    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;

    for (uint32_t gi = 0; gi < fs->group_count; gi++) {
        uint32_t g = (pref_group + gi) % fs->group_count;
        uint64_t bmp_blk = group_block_bitmap(fs, g);
        if (bmp_blk == 0) continue;
        if (read_block(fs, bmp_blk, bmp) != VFS_OK) continue;

        for (uint32_t bit = 0; bit < fs->blocks_per_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint8_t  mask = (uint8_t)(1u << (bit & 7));
            if (byte_idx >= fs->block_size) break;
            if (bmp[byte_idx] & mask) continue;

            uint32_t abs_blk = g * fs->blocks_per_group +
                               fs->first_data_block + bit;
            if (abs_blk >= fs->total_blocks) continue;

            bmp[byte_idx] |= mask;
            int rc = write_block(fs, bmp_blk, bmp);
            if (rc != VFS_OK) { kfree(bmp); return rc; }
            adj_free_blocks(fs, g, -1);
            flush_gdt(fs);
            flush_superblock(fs);
            kfree(bmp);
            *out_blk = abs_blk;
            return VFS_OK;
        }
    }
    kfree(bmp);
    return VFS_ERR_NOSPC;
}

static int ext4_free_block(struct ext4 *fs, uint32_t blk_no) {
    if (blk_no < fs->first_data_block || blk_no >= fs->total_blocks)
        return VFS_ERR_INVAL;
    uint32_t rel = blk_no - fs->first_data_block;
    uint32_t g = rel / fs->blocks_per_group;
    uint32_t bit = rel % fs->blocks_per_group;
    if (g >= fs->group_count) return VFS_ERR_INVAL;

    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;
    uint64_t bmp_blk = group_block_bitmap(fs, g);
    int rc = read_block(fs, bmp_blk, bmp);
    if (rc != VFS_OK) { kfree(bmp); return rc; }
    bmp[bit / 8] &= (uint8_t)~(1u << (bit & 7));
    rc = write_block(fs, bmp_blk, bmp);
    kfree(bmp);
    if (rc != VFS_OK) return rc;
    adj_free_blocks(fs, g, +1);
    flush_gdt(fs);
    return flush_superblock(fs);
}

static int ext4_alloc_inode(struct ext4 *fs, uint32_t pref_group,
                            uint32_t *out_ino) {
    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;

    for (uint32_t gi = 0; gi < fs->group_count; gi++) {
        uint32_t g = (pref_group + gi) % fs->group_count;
        uint64_t bmp_blk = group_inode_bitmap(fs, g);
        if (bmp_blk == 0) continue;
        if (read_block(fs, bmp_blk, bmp) != VFS_OK) continue;

        for (uint32_t bit = 0; bit < fs->inodes_per_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint8_t  mask = (uint8_t)(1u << (bit & 7));
            if (byte_idx >= fs->block_size) break;
            if (bmp[byte_idx] & mask) continue;

            uint32_t ino = g * fs->inodes_per_group + bit + 1;
            if (ino < fs->first_ino && ino != EXT4_ROOT_INODE) continue;

            bmp[byte_idx] |= mask;
            int rc = write_block(fs, bmp_blk, bmp);
            if (rc != VFS_OK) { kfree(bmp); return rc; }
            adj_free_inodes(fs, g, -1);
            flush_gdt(fs);
            flush_superblock(fs);
            kfree(bmp);
            *out_ino = ino;
            return VFS_OK;
        }
    }
    kfree(bmp);
    return VFS_ERR_NOSPC;
}

static int ext4_free_inode(struct ext4 *fs, uint32_t ino) {
    if (ino == 0 || ino > fs->total_inodes) return VFS_ERR_INVAL;
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;
    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;
    uint64_t bmp_blk = group_inode_bitmap(fs, g);
    int rc = read_block(fs, bmp_blk, bmp);
    if (rc != VFS_OK) { kfree(bmp); return rc; }
    bmp[bit / 8] &= (uint8_t)~(1u << (bit & 7));
    rc = write_block(fs, bmp_blk, bmp);
    kfree(bmp);
    if (rc != VFS_OK) return rc;
    adj_free_inodes(fs, g, +1);
    flush_gdt(fs);
    return flush_superblock(fs);
}

/* ---- block resolution: extents OR legacy pointers ---- */

/* Walk one extent tree level. `node` points to a buffer that begins
 * with an ext4_extent_header. `logical_blk` is the file-relative
 * block we want. On success `*out_phys` gets the physical block
 * number. Recurses up to `EXT4_MAX_EXTENT_DEPTH` (5 is enough for any
 * real ext4; we cap at 4 here defensively). */
#define EXT4_MAX_EXTENT_DEPTH 4

static int extent_lookup(struct ext4 *fs, const uint8_t *node,
                         uint32_t logical_blk, uint64_t *out_phys,
                         int depth_left) {
    if (depth_left < 0) return VFS_ERR_INVAL;

    const struct ext4_extent_header *eh =
        (const struct ext4_extent_header *)node;
    if (eh->eh_magic != EXT4_EXT_MAGIC) return VFS_ERR_INVAL;
    if (eh->eh_entries == 0) return VFS_ERR_NOENT;

    if (eh->eh_depth == 0) {
        /* Leaf -- entries are ext4_extent. */
        const struct ext4_extent *e =
            (const struct ext4_extent *)(node + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            uint16_t len = e[i].ee_len;
            /* Uninitialized extent: high bit of ee_len means "zeros".
             * For RO we still return the block; callers either get a
             * real block (initialized) or zeros if the FS pre-allocated
             * but never wrote -- typical for sparse files we don't yet
             * support. Treat both the same way. */
            if (len > 32768) len = (uint16_t)(len - 32768);
            if (logical_blk >= e[i].ee_block &&
                logical_blk <  e[i].ee_block + len) {
                uint64_t base = ((uint64_t)e[i].ee_start_hi << 32) |
                                e[i].ee_start_lo;
                *out_phys = base + (logical_blk - e[i].ee_block);
                return VFS_OK;
            }
        }
        return VFS_ERR_NOENT;
    }

    /* Index node -- entries are ext4_extent_idx. Find the LARGEST
     * index whose ei_block <= logical_blk, then recurse. */
    const struct ext4_extent_idx *ei =
        (const struct ext4_extent_idx *)(node + sizeof(*eh));
    int chosen = -1;
    for (uint16_t i = 0; i < eh->eh_entries; i++) {
        if (ei[i].ei_block <= logical_blk) chosen = i;
        else break;
    }
    if (chosen < 0) return VFS_ERR_NOENT;

    uint64_t leaf_blk = ((uint64_t)ei[chosen].ei_leaf_hi << 32) |
                        ei[chosen].ei_leaf_lo;
    int rc = read_block(fs, leaf_blk, fs->blk_buf2);
    if (rc != VFS_OK) return rc;
    return extent_lookup(fs, fs->blk_buf2, logical_blk,
                         out_phys, depth_left - 1);
}

/* Map logical file block `lblk` to a physical disk block using
 * either the extent tree or the legacy block-pointer chain in the
 * inode's i_block[15] array. Sets *out_phys to 0 to indicate a hole
 * (caller should treat as zeros). */
static int inode_block_map(struct ext4 *fs, const struct ext4_inode *in,
                           uint32_t lblk, uint64_t *out_phys) {
    *out_phys = 0;

    if (in->i_flags & EXT4_EXTENTS_FL) {
        /* i_block IS the extent header + entries (60 bytes total). */
        return extent_lookup(fs, (const uint8_t *)&in->i_block[0],
                             lblk, out_phys, EXT4_MAX_EXTENT_DEPTH);
    }

    /* Legacy pointers: 12 direct, 1 single-indirect (slot 12), then
     * double + triple. We support up to single-indirect. */
    if (lblk < 12) {
        *out_phys = in->i_block[lblk];
        return VFS_OK;
    }

    uint32_t per_block = fs->block_size / 4;  /* 32-bit pointers */
    if (lblk < 12u + per_block) {
        uint32_t ind = in->i_block[12];
        if (ind == 0) return VFS_OK;          /* hole */
        int rc = read_block(fs, ind, fs->blk_buf2);
        if (rc != VFS_OK) return rc;
        const uint32_t *table = (const uint32_t *)fs->blk_buf2;
        *out_phys = table[lblk - 12];
        return VFS_OK;
    }

    /* Double / triple indirect not implemented -- our test files are
     * tiny. Return INVAL so the caller surfaces a clean error rather
     * than silently truncating. */
    return VFS_ERR_INVAL;
}

/* ---- block allocation into an inode (write path) ---- */

/* Append a freshly-allocated physical block for file block `lblk` to an
 * EXTENT inode's depth-0 tree (header + leaves live in the 60-byte inode
 * body). The inode is updated in memory; the caller writes it back.
 *
 * Sequential growth keeps a single contiguous extent (just bumps
 * ee_len); a non-contiguous block starts a new leaf. The inode-embedded
 * header holds (60-12)/12 = 4 leaves, so up to 4 discontiguous runs are
 * supported. A deeper (index) tree, or overflow past 4 leaves, returns
 * NOSPC -- a documented limit, never silent corruption. */
static int extent_append_block(struct ext4 *fs, struct ext4_inode *in,
                               uint32_t lblk, uint64_t *out_phys) {
    struct ext4_extent_header *eh =
        (struct ext4_extent_header *)&in->i_block[0];
    if (eh->eh_magic != EXT4_EXT_MAGIC) return VFS_ERR_INVAL;
    if (eh->eh_depth != 0) return VFS_ERR_NOSPC;   /* index tree: unsupported */

    struct ext4_extent *e = (struct ext4_extent *)(eh + 1);
    uint32_t group = 0;        /* allocation goal: first group is fine */

    /* Try to extend the last extent if `lblk` continues it AND a
     * contiguous physical block is what the allocator hands back. */
    if (eh->eh_entries > 0) {
        struct ext4_extent *last = &e[eh->eh_entries - 1];
        uint16_t llen = last->ee_len;
        if (llen > 32768) llen = (uint16_t)(llen - 32768);   /* uninit */
        uint64_t last_start = ((uint64_t)last->ee_start_hi << 32) |
                              last->ee_start_lo;
        if (lblk == (uint32_t)(last->ee_block + llen) && llen < 32768) {
            uint32_t blk;
            int rc = ext4_alloc_block(fs, group, &blk);
            if (rc != VFS_OK) return rc;
            if ((uint64_t)blk == last_start + llen) {
                last->ee_len = (uint16_t)(llen + 1);   /* contiguous: grow */
                *out_phys = blk;
                in->i_blocks_lo += fs->block_size / 512;
                return VFS_OK;
            }
            /* Not contiguous -- fall through to add a new leaf using the
             * block we already allocated. */
            if (eh->eh_entries >= eh->eh_max) { ext4_free_block(fs, blk); return VFS_ERR_NOSPC; }
            struct ext4_extent *ne = &e[eh->eh_entries];
            ne->ee_block    = lblk;
            ne->ee_len      = 1;
            ne->ee_start_hi = 0;       /* 32-bit block numbers: hi always 0 */
            ne->ee_start_lo = blk;
            eh->eh_entries++;
            *out_phys = blk;
            in->i_blocks_lo += fs->block_size / 512;
            return VFS_OK;
        }
    }

    /* First extent, or a non-adjacent logical block: new leaf. */
    if (eh->eh_entries >= eh->eh_max) return VFS_ERR_NOSPC;
    uint32_t blk;
    int rc = ext4_alloc_block(fs, group, &blk);
    if (rc != VFS_OK) return rc;
    struct ext4_extent *ne = &e[eh->eh_entries];
    ne->ee_block    = lblk;
    ne->ee_len      = 1;
    ne->ee_start_hi = 0;               /* 32-bit block numbers: hi always 0 */
    ne->ee_start_lo = blk;
    eh->eh_entries++;
    *out_phys = blk;
    in->i_blocks_lo += fs->block_size / 512;
    return VFS_OK;
}

/* Set the legacy block pointer for file block `lblk` (direct or single-
 * indirect), allocating the indirect block on demand. Double/triple
 * indirect unsupported (NOSPC). Mirrors ext2.c inode_set_block. */
static int legacy_set_block(struct ext4 *fs, struct ext4_inode *in,
                            uint32_t lblk, uint32_t phys_blk) {
    uint32_t per_block = fs->block_size / 4;
    if (lblk < 12) { in->i_block[lblk] = phys_blk; return VFS_OK; }

    if (lblk < 12u + per_block) {
        if (in->i_block[12] == 0) {
            uint32_t ib;
            int rc = ext4_alloc_block(fs, 0, &ib);
            if (rc != VFS_OK) return rc;
            uint8_t *zero = kcalloc(1, fs->block_size);
            if (!zero) return VFS_ERR_NOMEM;
            write_block(fs, ib, zero);
            kfree(zero);
            in->i_block[12] = ib;
            in->i_blocks_lo += fs->block_size / 512;
        }
        uint8_t *ind = kmalloc(fs->block_size);
        if (!ind) return VFS_ERR_NOMEM;
        int rc = read_block(fs, in->i_block[12], ind);
        if (rc != VFS_OK) { kfree(ind); return rc; }
        ((uint32_t *)ind)[lblk - 12] = phys_blk;
        rc = write_block(fs, in->i_block[12], ind);
        kfree(ind);
        return rc;
    }
    return VFS_ERR_NOSPC;
}

/* Map `lblk` to a physical block, allocating one if it isn't backed yet.
 * Unifies the extent + legacy paths so the write code is path-agnostic. */
static int inode_block_map_alloc(struct ext4 *fs, struct ext4_inode *in,
                                 uint32_t lblk, uint64_t *out_phys) {
    uint64_t phys = 0;
    int rc = inode_block_map(fs, in, lblk, &phys);
    if (rc == VFS_OK && phys != 0) { *out_phys = phys; return VFS_OK; }
    if (rc != VFS_OK && rc != VFS_ERR_NOENT) return rc;

    if (in->i_flags & EXT4_EXTENTS_FL)
        return extent_append_block(fs, in, lblk, out_phys);

    /* Legacy: allocate a data block and wire the pointer. */
    uint32_t blk;
    rc = ext4_alloc_block(fs, 0, &blk);
    if (rc != VFS_OK) return rc;
    rc = legacy_set_block(fs, in, lblk, blk);
    if (rc != VFS_OK) { ext4_free_block(fs, blk); return rc; }
    in->i_blocks_lo += fs->block_size / 512;
    *out_phys = blk;
    return VFS_OK;
}

/* ---- read bytes from an inode ---- */

static long inode_read(struct ext4 *fs, const struct ext4_inode *in,
                       uint64_t file_size, uint64_t pos,
                       void *buf, size_t n) {
    if (pos >= file_size) return 0;
    uint64_t avail = file_size - pos;
    if (n > avail) n = (size_t)avail;
    if (n == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    size_t   total = 0;
    while (n > 0) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t off  = (uint32_t)(pos % fs->block_size);
        uint32_t take = fs->block_size - off;
        if (take > n) take = (uint32_t)n;

        uint64_t phys = 0;
        int rc = inode_block_map(fs, in, lblk, &phys);
        if (rc != VFS_OK) return rc;
        if (phys == 0) {
            memset(out, 0, take);            /* hole reads as zero */
        } else {
            rc = read_block(fs, phys, fs->blk_buf);
            if (rc != VFS_OK) return rc;
            memcpy(out, fs->blk_buf + off, take);
        }
        out   += take;
        pos   += take;
        n     -= take;
        total += take;
    }
    return (long)total;
}

/* ---- directory iteration ---- */

/* Run `cb` against every dir entry in inode `in` (which must be a
 * directory). The callback receives (inode_no, file_type, name).
 * Returning VFS_ERR_NOENT from the callback stops the walk and is
 * treated as success (the "found-and-done" pattern matches fat32.c).
 * Any other negative return aborts. */
typedef int (*dir_cb_t)(void *user, uint32_t ino, uint8_t file_type,
                        const char *name, uint8_t name_len);

static int dir_walk(struct ext4 *fs, const struct ext4_inode *dir_inode,
                    dir_cb_t cb, void *user) {
    uint64_t dir_size = ((uint64_t)dir_inode->i_size_hi << 32) |
                        dir_inode->i_size_lo;
    uint32_t total_blocks = (uint32_t)((dir_size + fs->block_size - 1) /
                                       fs->block_size);

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;

    int rc = VFS_OK;
    for (uint32_t lblk = 0; lblk < total_blocks; lblk++) {
        uint64_t phys = 0;
        rc = inode_block_map(fs, dir_inode, lblk, &phys);
        if (rc != VFS_OK || phys == 0) {
            if (rc == VFS_OK) continue;       /* hole -- skip */
            goto out;
        }
        rc = read_block(fs, phys, blk);
        if (rc != VFS_OK) goto out;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            const struct ext4_dir_entry_2 *de =
                (const struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > fs->block_size) {
                rc = VFS_ERR_IO;
                goto out;
            }
            if (de->inode != 0 && de->name_len > 0) {
                /* Stack-copy + NUL-terminate so callbacks can treat
                 * the name as a normal C string. */
                char name_buf[VFS_NAME_MAX];
                size_t cap = sizeof(name_buf) - 1;
                size_t cp  = de->name_len < cap ? de->name_len : cap;
                memcpy(name_buf, de->name, cp);
                name_buf[cp] = 0;
                int crc = cb(user, de->inode, de->file_type, name_buf,
                             (uint8_t)cp);
                if (crc == VFS_ERR_NOENT) { rc = VFS_OK; goto out; }
                if (crc != VFS_OK)        { rc = crc;    goto out; }
            }
            off += rec_len;
        }
    }
out:
    kfree(blk);
    return rc;
}

/* ---- path resolution ---- */

struct lookup_one_ctx {
    const char *want;
    uint32_t    out_ino;
    uint8_t     out_type;
    bool        found;
};

static int lookup_one_cb(void *user, uint32_t ino, uint8_t ftype,
                         const char *name, uint8_t nlen) {
    (void)nlen;
    struct lookup_one_ctx *ctx = (struct lookup_one_ctx *)user;
    if (strcmp(name, ctx->want) == 0) {
        ctx->out_ino  = ino;
        ctx->out_type = ftype;
        ctx->found    = true;
        return VFS_ERR_NOENT;        /* stop walking */
    }
    return VFS_OK;
}

/* Find the inode number for `path` (absolute, '/'-separated). On
 * success returns VFS_OK and fills *out_ino / *out_type with the
 * EXT4_FT_* file_type of the final component. */
static int path_to_inode(struct ext4 *fs, const char *path,
                         uint32_t *out_ino, uint8_t *out_type) {
    if (!path) return VFS_ERR_INVAL;
    while (*path == '/') path++;

    uint32_t cur = EXT4_ROOT_INODE;
    uint8_t  type = EXT4_FT_DIR;

    char comp[VFS_NAME_MAX];
    while (*path) {
        size_t i = 0;
        while (path[i] && path[i] != '/') {
            if (i + 1 >= sizeof(comp)) return VFS_ERR_NAMETOOLONG;
            comp[i] = path[i];
            i++;
        }
        comp[i] = 0;

        if (i == 0) break;            /* trailing '/' */

        struct ext4_inode dir;
        int rc = read_inode(fs, cur, &dir);
        if (rc != VFS_OK) return rc;
        if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
            return VFS_ERR_NOTDIR;
        }
        struct lookup_one_ctx ctx = { .want = comp, .out_ino = 0,
                                       .out_type = 0, .found = false };
        rc = dir_walk(fs, &dir, lookup_one_cb, &ctx);
        if (rc != VFS_OK) return rc;
        if (!ctx.found) return VFS_ERR_NOENT;

        cur  = ctx.out_ino;
        type = ctx.out_type;
        path += i;
        while (*path == '/') path++;
    }

    *out_ino  = cur;
    *out_type = type;
    return VFS_OK;
}

/* Resolve `path` to its parent directory inode + leaf component name.
 * Mirrors ext2.c path_parent. */
static int path_parent(struct ext4 *fs, const char *path,
                       uint32_t *parent_ino, char *leaf, size_t leaf_sz) {
    if (!path || path[0] != '/') return VFS_ERR_INVAL;
    const char *last_slash = path;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    size_t dir_len = (size_t)(last_slash - path);
    if (dir_len == 0) dir_len = 1;
    char dir_path[VFS_PATH_MAX];
    if (dir_len >= sizeof(dir_path)) return VFS_ERR_NAMETOOLONG;
    memcpy(dir_path, path, dir_len);
    dir_path[dir_len] = 0;
    if (dir_path[0] != '/') { dir_path[0] = '/'; dir_path[1] = 0; }

    const char *name = last_slash + 1;
    while (*name == '/') name++;
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen >= leaf_sz) return VFS_ERR_INVAL;
    memcpy(leaf, name, nlen + 1);

    uint32_t pino; uint8_t ptype;
    int rc = path_to_inode(fs, dir_path, &pino, &ptype);
    if (rc != VFS_OK) return rc;
    if (ptype != EXT4_FT_DIR) return VFS_ERR_NOTDIR;
    *parent_ino = pino;
    return VFS_OK;
}

/* ---- directory entry management (write) ---- */

static void init_extent_header(struct ext4_inode *in) {
    memset(&in->i_block[0], 0, sizeof(in->i_block));
    struct ext4_extent_header *eh =
        (struct ext4_extent_header *)&in->i_block[0];
    eh->eh_magic   = EXT4_EXT_MAGIC;
    eh->eh_entries = 0;
    eh->eh_max     = (sizeof(in->i_block) - sizeof(*eh)) / sizeof(struct ext4_extent);
    eh->eh_depth   = 0;
}

/* Insert (name -> child_ino) into directory `dir_ino`. Walks existing
 * dir blocks for a slot with enough slack (splitting it), else grows the
 * directory by one block via the unified extent/legacy allocator. */
static int dir_add_entry(struct ext4 *fs, uint32_t dir_ino,
                         const char *name, uint32_t child_ino,
                         uint8_t file_type) {
    struct ext4_inode dir;
    int rc = read_inode(fs, dir_ino, &dir);
    if (rc != VFS_OK) return rc;
    if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    uint8_t nlen = (uint8_t)strlen(name);
    uint16_t need_rec = (uint16_t)((8 + nlen + 3) & ~3u);
    uint64_t dir_size = ((uint64_t)dir.i_size_hi << 32) | dir.i_size_lo;
    uint32_t total_blocks = (uint32_t)((dir_size + fs->block_size - 1) /
                                       fs->block_size);

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;

    for (uint32_t lblk = 0; lblk < total_blocks; lblk++) {
        uint64_t phys = 0;
        rc = inode_block_map(fs, &dir, lblk, &phys);
        if (rc != VFS_OK || phys == 0) continue;
        if (read_block(fs, phys, blk) != VFS_OK) continue;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            struct ext4_dir_entry_2 *de =
                (struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > fs->block_size) break;

            uint16_t used = (de->inode == 0) ? 0 :
                            (uint16_t)((8 + de->name_len + 3) & ~3u);
            uint16_t avail = (uint16_t)(rec_len - used);
            if (avail >= need_rec) {
                if (de->inode != 0) { de->rec_len = used; off += used; }
                struct ext4_dir_entry_2 *ne =
                    (struct ext4_dir_entry_2 *)(blk + off);
                ne->inode     = child_ino;
                ne->rec_len   = (de->inode == 0 && off == 0) ? rec_len
                                : (uint16_t)(rec_len - used);
                ne->name_len  = nlen;
                ne->file_type = file_type;
                memcpy((char *)ne + 8, name, nlen);
                rc = write_block(fs, phys, blk);
                kfree(blk);
                return rc;
            }
            off += rec_len;
        }
    }

    /* No room: grow the directory by one block. */
    uint64_t new_phys = 0;
    rc = inode_block_map_alloc(fs, &dir, total_blocks, &new_phys);
    if (rc != VFS_OK) { kfree(blk); return rc; }

    memset(blk, 0, fs->block_size);
    struct ext4_dir_entry_2 *ne = (struct ext4_dir_entry_2 *)blk;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)fs->block_size;
    ne->name_len  = nlen;
    ne->file_type = file_type;
    memcpy((char *)ne + 8, name, nlen);
    rc = write_block(fs, new_phys, blk);
    kfree(blk);
    if (rc != VFS_OK) return rc;

    dir.i_size_lo += fs->block_size;       /* i_blocks already bumped by alloc */
    return write_inode(fs, dir_ino, &dir);
}

/* Remove `name` from directory `dir_ino` (merge its rec_len into the
 * previous entry, or zero the inode for the first slot). */
static int dir_remove_entry(struct ext4 *fs, uint32_t dir_ino,
                            const char *name) {
    struct ext4_inode dir;
    int rc = read_inode(fs, dir_ino, &dir);
    if (rc != VFS_OK) return rc;

    uint64_t dir_size = ((uint64_t)dir.i_size_hi << 32) | dir.i_size_lo;
    uint32_t total_blocks = (uint32_t)((dir_size + fs->block_size - 1) /
                                       fs->block_size);
    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;
    uint8_t want_len = (uint8_t)strlen(name);

    for (uint32_t lblk = 0; lblk < total_blocks; lblk++) {
        uint64_t phys = 0;
        rc = inode_block_map(fs, &dir, lblk, &phys);
        if (rc != VFS_OK || phys == 0) continue;
        if (read_block(fs, phys, blk) != VFS_OK) continue;

        uint32_t off = 0;
        struct ext4_dir_entry_2 *prev = NULL;
        while (off + 8 <= fs->block_size) {
            struct ext4_dir_entry_2 *de =
                (struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > fs->block_size) break;

            if (de->inode != 0 && de->name_len == want_len) {
                char nbuf[VFS_NAME_MAX];
                size_t cp = de->name_len < sizeof(nbuf) - 1 ?
                            de->name_len : sizeof(nbuf) - 1;
                memcpy(nbuf, (const char *)de + 8, cp);
                nbuf[cp] = 0;
                if (strcmp(nbuf, name) == 0) {
                    if (prev)
                        prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                    else
                        de->inode = 0;
                    rc = write_block(fs, phys, blk);
                    kfree(blk);
                    return rc;
                }
            }
            prev = de;
            off += rec_len;
        }
    }
    kfree(blk);
    return VFS_ERR_NOENT;
}

/* Free all data blocks an inode references (extent leaves or legacy
 * direct + single-indirect). */
static void inode_free_data(struct ext4 *fs, struct ext4_inode *in) {
    if (in->i_flags & EXT4_EXTENTS_FL) {
        struct ext4_extent_header *eh =
            (struct ext4_extent_header *)&in->i_block[0];
        if (eh->eh_magic == EXT4_EXT_MAGIC && eh->eh_depth == 0) {
            struct ext4_extent *e = (struct ext4_extent *)(eh + 1);
            for (uint16_t i = 0; i < eh->eh_entries; i++) {
                uint16_t len = e[i].ee_len;
                if (len > 32768) len = (uint16_t)(len - 32768);
                uint64_t start = ((uint64_t)e[i].ee_start_hi << 32) |
                                 e[i].ee_start_lo;
                for (uint16_t j = 0; j < len; j++)
                    ext4_free_block(fs, (uint32_t)(start + j));
            }
            eh->eh_entries = 0;
        }
        in->i_blocks_lo = 0;
        return;
    }
    uint32_t per_block = fs->block_size / 4;
    for (int i = 0; i < 12; i++)
        if (in->i_block[i]) { ext4_free_block(fs, in->i_block[i]); in->i_block[i] = 0; }
    if (in->i_block[12]) {
        uint8_t *ind = kmalloc(fs->block_size);
        if (ind && read_block(fs, in->i_block[12], ind) == VFS_OK) {
            const uint32_t *p = (const uint32_t *)ind;
            for (uint32_t i = 0; i < per_block; i++)
                if (p[i]) ext4_free_block(fs, p[i]);
        }
        if (ind) kfree(ind);
        ext4_free_block(fs, in->i_block[12]);
        in->i_block[12] = 0;
    }
    in->i_blocks_lo = 0;
}

/* ================================================================
 * JBD2 write-ahead journalling.
 *
 * Each atomic filesystem operation opens a transaction (txn_begin),
 * stages every block it touches, then commits (txn_commit):
 *
 *   1. mark the on-disk journal dirty (superblock s_start = first log
 *      block, s_sequence = this txn's id; set ext4 NEEDS_RECOVERY).
 *   2. write the log: [descriptor][data..][commit].
 *   3. checkpoint: copy each staged block to its home location.
 *   4. mark the journal clean (s_start = 0; clear NEEDS_RECOVERY).
 *
 * A crash anywhere is healed on the next mount by jbd2_recover():
 *   - crash before the commit block -> no valid commit -> discard
 *     (the operation atomically did not happen);
 *   - crash after the commit block (or mid-checkpoint) -> replay the
 *     whole logged set to its homes (the operation atomically happened).
 * Replay is idempotent, so replaying an already-checkpointed txn is a
 * no-op. The journal is reset to empty after every checkpoint, so at most
 * one transaction is ever outstanding -- transactions always start at the
 * first log block, which removes all the circular-buffer bookkeeping.
 * ================================================================ */

/* Raw journal-block I/O: rel 0 is the journal superblock, rel >= j_first
 * are log blocks. These bypass transaction staging (they ARE the commit
 * machinery) and go straight to the device. */
static int jraw_read(struct ext4 *fs, uint32_t rel, void *buf) {
    uint64_t lba = (fs->j_phys_first + rel) * fs->sectors_per_block;
    return blk_read(fs->dev, lba, fs->sectors_per_block, buf) == 0
               ? VFS_OK : VFS_ERR_IO;
}
static int jraw_write(struct ext4 *fs, uint32_t rel, const void *buf) {
    uint64_t lba = (fs->j_phys_first + rel) * fs->sectors_per_block;
    return blk_write(fs->dev, lba, fs->sectors_per_block, buf) == 0
               ? VFS_OK : VFS_ERR_IO;
}

/* Open a transaction. With no (drivable) journal this is a no-op and
 * writes fall through to the disk as before -- zero behaviour change on
 * non-journalled images. If the txn allocation fails we also fall back to
 * direct writes (safe, just not atomic). */
static void txn_begin(struct ext4 *fs) {
    if (!fs->has_journal || fs->txn) return;
    fs->txn = kcalloc(1, sizeof(struct ext4_txn));
}

static void txn_free(struct ext4_txn *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++)
        if (t->ents[i].data) kfree(t->ents[i].data);
    if (t->ents) kfree(t->ents);
    kfree(t);
}

/* Throw away a transaction without touching the disk (operation failed
 * before commit -> filesystem is exactly as it was). */
static void txn_abort(struct ext4 *fs) {
    struct ext4_txn *t = fs->txn;
    fs->txn = NULL;
    txn_free(t);
}

/* Set the on-disk journal state. start_rel == 0 marks the log clean;
 * start_rel == j_first marks it dirty (a transaction may need recovery).
 * Also mirror the state into the ext4 NEEDS_RECOVERY incompat bit and
 * push the superblock straight to disk. */
static int jbd2_set_state(struct ext4 *fs, uint32_t start_rel,
                          uint32_t seq, bool recover_bit) {
    uint8_t *jsb = kmalloc(fs->block_size);
    if (!jsb) return VFS_ERR_NOMEM;
    int rc = jraw_read(fs, 0, jsb);
    if (rc != VFS_OK) { kfree(jsb); return rc; }
    jbd2_superblock_t *s = (jbd2_superblock_t *)jsb;
    s->s_start    = be32(start_rel);
    s->s_sequence = be32(seq);
    rc = jraw_write(fs, 0, jsb);
    kfree(jsb);
    if (rc != VFS_OK) return rc;

    struct ext4_super_block *sb = (struct ext4_super_block *)fs->sb_buf;
    if (sb) {
        if (recover_bit) sb->s_feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
        else             sb->s_feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
        fs->feature_incompat = sb->s_feature_incompat;
        rc = flush_superblock_direct(fs);
    }
    return rc;
}

/* Count the block tags in a descriptor block (mirrors the write layout:
 * 8-byte tag, plus a 16-byte UUID after any tag without SAME_UUID). */
static uint32_t jbd2_count_tags(struct ext4 *fs, const uint8_t *desc) {
    uint32_t off = sizeof(jbd2_header_t), n = 0;
    while (off + sizeof(jbd2_block_tag_t) <= fs->block_size) {
        const jbd2_block_tag_t *tag = (const jbd2_block_tag_t *)(desc + off);
        uint16_t flags = be16(tag->t_flags);
        off += sizeof(*tag);
        if (!(flags & JBD2_FLAG_SAME_UUID)) off += 16;
        n++;
        if (flags & JBD2_FLAG_LAST_TAG) break;
    }
    return n;
}

/* Write the log records for transaction `t`: one descriptor block listing
 * every home block, the data blocks themselves, then a commit block. */
static int jbd2_write_log(struct ext4 *fs, struct ext4_txn *t, uint32_t seq) {
    uint32_t n = (uint32_t)t->n;
    if ((uint64_t)fs->j_first + n + 1 >= fs->j_blocks) return VFS_ERR_NOSPC;

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;
    int rc = VFS_OK;

    /* --- descriptor block at j_first --- */
    memset(blk, 0, fs->block_size);
    jbd2_header_t *dh = (jbd2_header_t *)blk;
    dh->h_magic     = be32(JBD2_MAGIC_NUMBER);
    dh->h_blocktype = be32(JBD2_DESCRIPTOR_BLOCK);
    dh->h_sequence  = be32(seq);
    uint32_t off = sizeof(jbd2_header_t);
    for (uint32_t i = 0; i < n; i++) {
        if (off + sizeof(jbd2_block_tag_t) + 16 > fs->block_size) {
            rc = VFS_ERR_NOSPC; goto out;     /* too many tags for one block */
        }
        jbd2_block_tag_t *tag = (jbd2_block_tag_t *)(blk + off);
        uint16_t flags = 0;
        if (i + 1 == n) flags |= JBD2_FLAG_LAST_TAG;
        if (i > 0)      flags |= JBD2_FLAG_SAME_UUID;
        if (be32(*(const uint32_t *)t->ents[i].data) == JBD2_MAGIC_NUMBER)
            flags |= JBD2_FLAG_ESCAPE;        /* first word collides with magic */
        tag->t_blocknr  = be32((uint32_t)t->ents[i].blk);
        tag->t_checksum = 0;
        tag->t_flags    = be16(flags);
        off += sizeof(*tag);
        if (i == 0) { memset(blk + off, 0, 16); off += 16; }   /* journal UUID */
    }
    rc = jraw_write(fs, fs->j_first, blk);
    if (rc != VFS_OK) goto out;

    /* --- data blocks at j_first+1 .. j_first+n --- */
    for (uint32_t i = 0; i < n; i++) {
        memcpy(blk, t->ents[i].data, fs->block_size);
        if (be32(*(uint32_t *)blk) == JBD2_MAGIC_NUMBER)
            *(uint32_t *)blk = 0;             /* escape: zero the magic word */
        rc = jraw_write(fs, fs->j_first + 1 + i, blk);
        if (rc != VFS_OK) goto out;
    }

    if (g_jbd_crash_phase == JBD2_CRASH_BEFORE_COMMIT) goto out;  /* no commit */

    /* --- commit block at j_first+n+1 --- */
    memset(blk, 0, fs->block_size);
    jbd2_commit_header_t *ch = (jbd2_commit_header_t *)blk;
    ch->h.h_magic     = be32(JBD2_MAGIC_NUMBER);
    ch->h.h_blocktype = be32(JBD2_COMMIT_BLOCK);
    ch->h.h_sequence  = be32(seq);
    rc = jraw_write(fs, fs->j_first + 1 + n, blk);

out:
    kfree(blk);
    return rc;
}

/* Copy every staged block to its home location. */
static int jbd2_checkpoint(struct ext4 *fs, struct ext4_txn *t) {
    int half = (t->n + 1) / 2;
    for (int i = 0; i < t->n; i++) {
        if (g_jbd_crash_phase == JBD2_CRASH_MID_CHECKPOINT && i >= half)
            return VFS_OK;                    /* model a crash partway */
        uint64_t lba = t->ents[i].blk * fs->sectors_per_block;
        if (blk_write(fs->dev, lba, fs->sectors_per_block,
                      t->ents[i].data) != 0)
            return VFS_ERR_IO;
    }
    return VFS_OK;
}

/* Commit the active transaction (see the section header for ordering). */
static int txn_commit(struct ext4 *fs) {
    struct ext4_txn *t = fs->txn;
    if (!t) return VFS_OK;
    fs->txn = NULL;                  /* journal + checkpoint writes go direct */
    int rc;

    if (t->nomem) { rc = VFS_ERR_NOMEM; goto done; }
    if (t->n == 0) { rc = VFS_OK; goto done; }

    if (!fs->has_journal) {
        /* No journal: flush staged blocks straight home (legacy, non-
         * atomic). We never stage without a journal, but stay correct. */
        rc = VFS_OK;
        for (int i = 0; i < t->n && rc == VFS_OK; i++) {
            uint64_t lba = t->ents[i].blk * fs->sectors_per_block;
            if (blk_write(fs->dev, lba, fs->sectors_per_block,
                          t->ents[i].data) != 0)
                rc = VFS_ERR_IO;
        }
        goto done;
    }

    uint32_t seq = fs->j_sequence;
    rc = jbd2_set_state(fs, fs->j_first, seq, true);     /* mark dirty */
    if (rc != VFS_OK) goto done;
    rc = jbd2_write_log(fs, t, seq);
    if (rc != VFS_OK) goto done;
    if (g_jbd_crash_phase == JBD2_CRASH_BEFORE_COMMIT ||
        g_jbd_crash_phase == JBD2_CRASH_AFTER_COMMIT) { rc = VFS_OK; goto done; }
    rc = jbd2_checkpoint(fs, t);
    if (rc != VFS_OK) goto done;
    if (g_jbd_crash_phase == JBD2_CRASH_MID_CHECKPOINT) { rc = VFS_OK; goto done; }
    rc = jbd2_set_state(fs, 0, seq + 1, false);          /* mark clean */
    if (rc == VFS_OK) fs->j_sequence = seq + 1;

done:
    txn_free(t);
    return rc;
}

/* Close a transaction opened by txn_begin: commit on success, discard on
 * failure. If no journal was active, just return the operation's rc. */
static int txn_finish(struct ext4 *fs, int rc) {
    if (!fs->txn) return rc;
    if (rc != VFS_OK) { txn_abort(fs); return rc; }
    return txn_commit(fs);
}

/* Scan the log from the journal superblock's s_start and replay every
 * transaction that has a matching commit block. Idempotent: safe to run
 * on a partially-checkpointed or already-recovered journal. */
static int jbd2_recover(struct ext4 *fs) {
    uint8_t *jsb = kmalloc(fs->block_size);
    if (!jsb) return VFS_ERR_NOMEM;
    int rc = jraw_read(fs, 0, jsb);
    if (rc != VFS_OK) { kfree(jsb); return rc; }
    jbd2_superblock_t *s = (jbd2_superblock_t *)jsb;
    uint32_t start = be32(s->s_start);
    uint32_t seq   = be32(s->s_sequence);
    kfree(jsb);
    if (start == 0) { fs->j_sequence = seq; return VFS_OK; }   /* clean */

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;

    /* Pass 1: find the highest sequence whose commit block is present. */
    uint32_t rel = start, expect = seq, last_committed = seq - 1;
    while (rel < fs->j_blocks) {
        if (jraw_read(fs, rel, blk) != VFS_OK) break;
        jbd2_header_t *h = (jbd2_header_t *)blk;
        if (be32(h->h_magic) != JBD2_MAGIC_NUMBER) break;
        if (be32(h->h_blocktype) != JBD2_DESCRIPTOR_BLOCK) break;
        if (be32(h->h_sequence) != expect) break;
        uint32_t m = jbd2_count_tags(fs, blk);
        uint32_t commit_rel = rel + 1 + m;
        if (commit_rel >= fs->j_blocks) break;
        if (jraw_read(fs, commit_rel, blk) != VFS_OK) break;
        jbd2_header_t *c = (jbd2_header_t *)blk;
        if (be32(c->h_magic) == JBD2_MAGIC_NUMBER &&
            be32(c->h_blocktype) == JBD2_COMMIT_BLOCK &&
            be32(c->h_sequence) == expect) {
            last_committed = expect;
            rel = commit_rel + 1;
            expect++;
        } else {
            break;                        /* incomplete transaction: stop */
        }
    }

    /* Pass 2: replay each committed transaction's data blocks to home.
     * (No revoke pass: our writer never emits revoke records.) */
    uint32_t replayed = 0;
    rel = start; expect = seq;
    uint8_t *dblk = kmalloc(fs->block_size);
    if (!dblk) { kfree(blk); return VFS_ERR_NOMEM; }
    while (expect <= last_committed && rel < fs->j_blocks) {
        if (jraw_read(fs, rel, blk) != VFS_OK) break;
        jbd2_header_t *h = (jbd2_header_t *)blk;
        if (be32(h->h_magic) != JBD2_MAGIC_NUMBER ||
            be32(h->h_blocktype) != JBD2_DESCRIPTOR_BLOCK ||
            be32(h->h_sequence) != expect) break;
        uint32_t off = sizeof(jbd2_header_t), idx = 0;
        bool last = false;
        while (!last && off + sizeof(jbd2_block_tag_t) <= fs->block_size) {
            jbd2_block_tag_t *tag = (jbd2_block_tag_t *)(blk + off);
            uint16_t flags = be16(tag->t_flags);
            uint32_t home  = be32(tag->t_blocknr);
            off += sizeof(*tag);
            if (!(flags & JBD2_FLAG_SAME_UUID)) off += 16;
            last = (flags & JBD2_FLAG_LAST_TAG) != 0;
            if (jraw_read(fs, rel + 1 + idx, dblk) == VFS_OK) {
                if (flags & JBD2_FLAG_ESCAPE)
                    *(uint32_t *)dblk = be32(JBD2_MAGIC_NUMBER);
                uint64_t lba = (uint64_t)home * fs->sectors_per_block;
                if (blk_write(fs->dev, lba, fs->sectors_per_block, dblk) == 0)
                    replayed++;
            }
            idx++;
        }
        rel = rel + 1 + idx + 1;          /* descriptor + data + commit */
        expect++;
    }
    kfree(dblk);
    kfree(blk);

    /* Recovery may have rewritten the ext4 SB / GDT; refresh our caches
     * from disk before we clear the recovery flags (which write the SB). */
    if (blk_read(fs->dev, 2, 2, fs->sb_buf) != 0) return VFS_ERR_IO;
    uint32_t gdt_blocks = (fs->gdt_bytes + fs->block_size - 1) / fs->block_size;
    uint32_t gdt_first = fs->first_data_block + 1;
    for (uint32_t i = 0; i < gdt_blocks; i++)
        if (read_block(fs, gdt_first + i,
                       fs->gdt_buf + i * fs->block_size) != VFS_OK)
            return VFS_ERR_IO;

    fs->j_sequence = last_committed + 1;
    rc = jbd2_set_state(fs, 0, fs->j_sequence, false);     /* journal clean */
    kprintf("[ext4] journal recovery: replayed %u block(s), seq %u..%u\n",
            replayed, (unsigned)seq, (unsigned)last_committed);
    return rc;
}

/* Locate the journal inode's (single, contiguous, depth-0) extent so we
 * can address log blocks physically. Returns false if the journal isn't
 * laid out the simple way we can drive. */
static bool journal_locate(struct ext4 *fs, uint32_t jinum) {
    struct ext4_inode ji;
    if (read_inode(fs, jinum, &ji) != VFS_OK) return false;
    if (!(ji.i_flags & EXT4_EXTENTS_FL)) return false;
    struct ext4_extent_header *eh = (struct ext4_extent_header *)&ji.i_block[0];
    if (eh->eh_magic != EXT4_EXT_MAGIC || eh->eh_depth != 0 ||
        eh->eh_entries != 1) return false;
    struct ext4_extent *e = (struct ext4_extent *)(eh + 1);
    if (e->ee_block != 0) return false;
    uint16_t len = e->ee_len;
    if (len > 32768) len = (uint16_t)(len - 32768);
    fs->j_phys_first = ((uint64_t)e->ee_start_hi << 32) | e->ee_start_lo;
    fs->j_blocks     = len;
    return fs->j_blocks >= 4;     /* superblock + at least desc/data/commit */
}

/* ---- vfs_ops ---- */

static int ext4_open(void *mnt, const char *path, struct vfs_file *out) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG) return VFS_ERR_NOENT;

    struct ext4_filepriv *fp = kcalloc(1, sizeof(*fp));
    if (!fp) return VFS_ERR_NOMEM;
    fp->inode_no  = ino;
    fp->in        = in;
    fp->file_size = ((uint64_t)in.i_size_hi << 32) | in.i_size_lo;

    out->priv = fp;
    out->size = (size_t)fp->file_size;
    out->pos  = 0;
    out->mode = in.i_mode & VFS_MODE_PERMS;
    if (in.i_mode != 0) out->mode |= VFS_MODE_VALID;
    out->uid  = ext4_inode_uid(&in);
    out->gid  = ext4_inode_gid(&in);
    return VFS_OK;
}

static int ext4_close(struct vfs_file *f) {
    if (f && f->priv) { kfree(f->priv); f->priv = NULL; }
    return VFS_OK;
}

/* An open handle keeps its OWN copy of the inode, and every mutation --
 * this handle's, another handle's, a truncate by path -- writes the inode
 * straight to disk. So disk is the shared truth and the cached copy goes
 * stale the moment anyone else touches the file.
 *
 * Re-read before acting. Without this, two handles on one file each wrote
 * their own stale copy back and whichever finished last silently discarded
 * the other's growth, leaking every block it had allocated; and `> file`
 * behind an already-open handle was undone by that handle's next write.
 *
 * Cheap: the inode table goes through the block cache, so this is a memcpy
 * in the common case. */
static int fp_refresh(struct ext4 *fs, struct ext4_filepriv *fp) {
    struct ext4_inode in;
    int rc = read_inode(fs, fp->inode_no, &in);
    if (rc != VFS_OK) return rc;
    fp->in        = in;
    fp->file_size = ((uint64_t)in.i_size_hi << 32) | in.i_size_lo;
    return VFS_OK;
}

static long ext4_read(struct vfs_file *f, void *buf, size_t n) {
    struct ext4 *fs = (struct ext4 *)f->mnt;
    struct ext4_filepriv *fp = (struct ext4_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    int frc = fp_refresh(fs, fp);            /* another handle may have grown it */
    if (frc != VFS_OK) return frc;
    f->size = (size_t)fp->file_size;
    long got = inode_read(fs, &fp->in, fp->file_size, f->pos, buf, n);
    if (got > 0) f->pos += (size_t)got;
    return got;
}

/* Cap on file-data blocks journalled per transaction. A write longer
 * than this is split into several atomic chunks (the inode is persisted
 * at each chunk boundary, so every commit leaves a consistent file). */
#define EXT4_WRITE_CHUNK_BLOCKS 32

static long ext4_write(struct vfs_file *f, const void *buf, size_t n) {
    struct ext4 *fs = (struct ext4 *)f->mnt;
    struct ext4_filepriv *fp = (struct ext4_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;

    /* Once, before the loop: the chunks below are this call's own and are
     * authoritative as they go. */
    int frc = fp_refresh(fs, fp);
    if (frc != VFS_OK) return frc;
    f->size = (size_t)fp->file_size;

    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;

    while (n > 0) {
        txn_begin(fs);                       /* one transaction per chunk */
        int rc = VFS_OK;
        int blocks = 0;
        while (n > 0 && blocks < EXT4_WRITE_CHUNK_BLOCKS) {
            uint32_t lblk = (uint32_t)(f->pos / fs->block_size);
            uint32_t off  = (uint32_t)(f->pos % fs->block_size);
            uint32_t take = fs->block_size - off;
            if (take > n) take = (uint32_t)n;

            uint64_t phys = 0;
            rc = inode_block_map_alloc(fs, &fp->in, lblk, &phys);
            if (rc != VFS_OK || phys == 0) {
                if (rc == VFS_OK) rc = VFS_ERR_IO;
                break;
            }
            /* RMW for a partial block; full-block writes skip the read. */
            uint8_t *tmp = kmalloc(fs->block_size);
            if (!tmp) { rc = VFS_ERR_NOMEM; break; }
            if (take < fs->block_size) {
                rc = read_block(fs, phys, tmp);
                if (rc != VFS_OK) { kfree(tmp); break; }
            }
            memcpy(tmp + off, src, take);
            rc = write_block(fs, phys, tmp);
            kfree(tmp);
            if (rc != VFS_OK) break;

            src     += take;
            f->pos  += take;
            n       -= take;
            written += take;
            blocks++;
        }

        /* Persist the inode (size + extent growth) INSIDE this txn so the
         * commit is self-consistent, then commit (or discard on error). */
        if (rc == VFS_OK) {
            if (f->pos > fp->file_size) {
                fp->file_size    = f->pos;
                fp->in.i_size_lo = (uint32_t)(fp->file_size & 0xFFFFFFFFu);
                fp->in.i_size_hi = (uint32_t)(fp->file_size >> 32);
                f->size          = (size_t)fp->file_size;
            }
            /* A write is a modification, and this driver never said so:
             * every file on an ext4 volume kept mtime 0 no matter how
             * often it changed. */
            uint32_t now = (uint32_t)ext4_now_secs();
            fp->in.i_mtime = now;
            fp->in.i_ctime = now;
            rc = write_inode(fs, fp->inode_no, &fp->in);
        }
        rc = txn_finish(fs, rc);
        if (rc != VFS_OK) return written > 0 ? (long)written : rc;
    }
    return (long)written;
}

static int do_create(struct ext4 *fs, const char *path,
                     uint32_t uid, uint32_t gid, uint32_t mode) {
    uint32_t parent_ino;
    char leaf[VFS_NAME_MAX];
    int rc = path_parent(fs, path, &parent_ino, leaf, sizeof(leaf));
    if (rc != VFS_OK) return rc;

    uint32_t ino; uint8_t ftype;
    if (path_to_inode(fs, path, &ino, &ftype) == VFS_OK) return VFS_ERR_EXIST;

    uint32_t pgroup = (parent_ino - 1) / fs->inodes_per_group;
    uint32_t new_ino;
    rc = ext4_alloc_inode(fs, pgroup, &new_ino);
    if (rc != VFS_OK) return rc;

    /* New regular file: extent-based with an empty depth-0 tree, so the
     * first write grows it through the extent path. */
    struct ext4_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode  = EXT4_S_IFREG | (uint16_t)(mode & 0xFFF);
    ext4_inode_set_uid(&in, uid);
    ext4_inode_set_gid(&in, gid);
    /* Born now, not at the epoch. */
    in.i_atime = in.i_ctime = in.i_mtime =
        (uint32_t)ext4_now_secs();
    in.i_links_count = 1;
    in.i_flags = EXT4_EXTENTS_FL;
    init_extent_header(&in);

    rc = write_inode(fs, new_ino, &in);
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }
    rc = dir_add_entry(fs, parent_ino, leaf, new_ino, EXT4_FT_REG_FILE);
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }
    return VFS_OK;
}

static int ext4_create(void *mnt, const char *path,
                       uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ext4 *fs = (struct ext4 *)mnt;
    txn_begin(fs);
    return txn_finish(fs, do_create(fs, path, uid, gid, mode));
}

static int do_unlink(struct ext4 *fs, const char *path) {
    uint32_t parent_ino;
    char leaf[VFS_NAME_MAX];
    int rc = path_parent(fs, path, &parent_ino, leaf, sizeof(leaf));
    if (rc != VFS_OK) return rc;

    uint32_t ino; uint8_t ftype;
    rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    if (ftype == EXT4_FT_DIR) return VFS_ERR_ISDIR;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    rc = dir_remove_entry(fs, parent_ino, leaf);
    if (rc != VFS_OK) return rc;

    if (in.i_links_count > 0) in.i_links_count--;
    if (in.i_links_count == 0) {
        inode_free_data(fs, &in);
        in.i_size_lo = 0;
        in.i_size_hi = 0;
        in.i_dtime   = 1;
        write_inode(fs, ino, &in);
        ext4_free_inode(fs, ino);
    } else {
        write_inode(fs, ino, &in);
    }
    return VFS_OK;
}

static int ext4_unlink(void *mnt, const char *path) {
    struct ext4 *fs = (struct ext4 *)mnt;
    txn_begin(fs);
    return txn_finish(fs, do_unlink(fs, path));
}

/* Hard links. do_unlink() above already did the hard half -- it decrements
 * i_links_count and only releases the blocks when the count reaches zero --
 * so the inode side of this has been correct all along and only the
 * second-directory-entry half was missing. */
static int do_link(struct ext4 *fs, const char *oldpath,
                   const char *newpath) {
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, oldpath, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    /* POSIX forbids hard links to directories: they make the tree cyclic
     * and fsck has to clean up after every walker that trusted it. */
    if (ftype == EXT4_FT_DIR ||
        (in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;

    uint32_t np_ino;
    char np_leaf[VFS_NAME_MAX];
    rc = path_parent(fs, newpath, &np_ino, np_leaf, sizeof np_leaf);
    if (rc != VFS_OK) return rc;

    uint32_t clash; uint8_t clash_ft;
    if (path_to_inode(fs, newpath, &clash, &clash_ft) == VFS_OK)
        return VFS_ERR_EXIST;

    rc = dir_add_entry(fs, np_ino, np_leaf, ino, ftype);
    if (rc != VFS_OK) return rc;

    in.i_links_count++;
    in.i_ctime = (uint32_t)ext4_now_secs();
    return write_inode(fs, ino, &in);
}

static int ext4_link(void *mnt, const char *oldpath, const char *newpath) {
    struct ext4 *fs = (struct ext4 *)mnt;
    txn_begin(fs);
    return txn_finish(fs, do_link(fs, oldpath, newpath));
}

/* Phase H: rename, journalled like every other mutation here. Same
 * stated scope as ext2's: directories rename only within their parent
 * (a cross-directory move leaves the child's ".." stale, and silently
 * corrupting a foreign filesystem is worse than refusing); files move
 * freely, clobbering a destination file the way rename(2) requires. */
static int do_rename(struct ext4 *fs, const char *oldpath,
                     const char *newpath) {
    uint32_t src_ino; uint8_t src_ft;
    int rc = path_to_inode(fs, oldpath, &src_ino, &src_ft);
    if (rc != VFS_OK) return rc;
    uint32_t op_ino, np_ino;
    char op_leaf[VFS_NAME_MAX], np_leaf[VFS_NAME_MAX];
    rc = path_parent(fs, oldpath, &op_ino, op_leaf, sizeof op_leaf);
    if (rc != VFS_OK) return rc;
    rc = path_parent(fs, newpath, &np_ino, np_leaf, sizeof np_leaf);
    if (rc != VFS_OK) return rc;
    if (src_ft == EXT4_FT_DIR && op_ino != np_ino) return VFS_ERR_INVAL;

    uint32_t dst_ino; uint8_t dst_ft;
    int drc = path_to_inode(fs, newpath, &dst_ino, &dst_ft);
    if (drc == VFS_OK) {
        if (dst_ino == src_ino) return VFS_OK;
        if (dst_ft == EXT4_FT_DIR) return VFS_ERR_INVAL;
        rc = do_unlink(fs, newpath);
        if (rc != VFS_OK) return rc;
    } else if (drc != VFS_ERR_NOENT) {
        return drc;
    }
    rc = dir_add_entry(fs, np_ino, np_leaf, src_ino, src_ft);
    if (rc != VFS_OK) return rc;
    return dir_remove_entry(fs, op_ino, op_leaf);
}

static int ext4_rename(void *mnt, const char *oldpath, const char *newpath) {
    struct ext4 *fs = (struct ext4 *)mnt;
    txn_begin(fs);
    return txn_finish(fs, do_rename(fs, oldpath, newpath));
}

/* Phase H: real statistics -- sum the group descriptors' free counters
 * (the same numbers the allocators maintain). */
static int ext4_statfs(void *mnt, struct vfs_statfs *out) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint64_t bfree = 0, ifree = 0;
    for (uint32_t g = 0; g < fs->group_count; g++) {
        struct ext4_group_desc_32 *gd = group_desc_mut(fs, g);
        uint64_t bf = gd->bg_free_blocks_count_lo;
        uint64_t inf = gd->bg_free_inodes_count_lo;
        if (fs->desc_64bit && fs->desc_size >= 64) {
            bf  |= ((uint64_t)((struct ext4_group_desc_64 *)gd)
                        ->bg_free_blocks_count_hi) << 16;
            inf |= ((uint64_t)((struct ext4_group_desc_64 *)gd)
                        ->bg_free_inodes_count_hi) << 16;
        }
        bfree += bf;
        ifree += inf;
    }
    out->bsize      = fs->block_size;
    out->blocks     = fs->total_blocks;
    out->bfree      = bfree;
    out->files      = fs->total_inodes;
    out->ffree      = ifree;
    out->type_magic = 0xEF53;               /* EXT4_SUPER_MAGIC */
    out->namelen    = 255;
    return VFS_OK;
}

static int do_mkdir(struct ext4 *fs, const char *path,
                    uint32_t uid, uint32_t gid, uint32_t mode) {
    uint32_t parent_ino;
    char leaf[VFS_NAME_MAX];
    int rc = path_parent(fs, path, &parent_ino, leaf, sizeof(leaf));
    if (rc != VFS_OK) return rc;

    uint32_t ino; uint8_t ftype;
    if (path_to_inode(fs, path, &ino, &ftype) == VFS_OK) return VFS_ERR_EXIST;

    uint32_t pgroup = (parent_ino - 1) / fs->inodes_per_group;
    uint32_t new_ino;
    rc = ext4_alloc_inode(fs, pgroup, &new_ino);
    if (rc != VFS_OK) return rc;

    /* New directory: extent-based, one data block holding "." + "..". */
    struct ext4_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode  = EXT4_S_IFDIR | (uint16_t)(mode & 0xFFF);
    ext4_inode_set_uid(&in, uid);
    ext4_inode_set_gid(&in, gid);
    /* Born now, not at the epoch. */
    in.i_atime = in.i_ctime = in.i_mtime =
        (uint32_t)ext4_now_secs();
    in.i_links_count = 2;            /* self + "." */
    in.i_flags = EXT4_EXTENTS_FL;
    init_extent_header(&in);

    uint64_t dir_phys = 0;
    rc = inode_block_map_alloc(fs, &in, 0, &dir_phys);   /* extent block 0 */
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }
    in.i_size_lo = fs->block_size;

    uint8_t *blk = kcalloc(1, fs->block_size);
    if (!blk) { ext4_free_inode(fs, new_ino); return VFS_ERR_NOMEM; }
    struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)blk;
    dot->inode = new_ino; dot->rec_len = 12; dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR; memcpy((char *)dot + 8, ".", 1);
    struct ext4_dir_entry_2 *dd = (struct ext4_dir_entry_2 *)(blk + 12);
    dd->inode = parent_ino; dd->rec_len = (uint16_t)(fs->block_size - 12);
    dd->name_len = 2; dd->file_type = EXT4_FT_DIR; memcpy((char *)dd + 8, "..", 2);
    rc = write_block(fs, dir_phys, blk);
    kfree(blk);
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }

    rc = write_inode(fs, new_ino, &in);
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }
    rc = dir_add_entry(fs, parent_ino, leaf, new_ino, EXT4_FT_DIR);
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }

    /* Parent gains a link (the new dir's ".."), and the group's
     * used-dirs count rises. */
    struct ext4_inode parent;
    if (read_inode(fs, parent_ino, &parent) == VFS_OK) {
        parent.i_links_count++;
        write_inode(fs, parent_ino, &parent);
    }
    adj_used_dirs(fs, (new_ino - 1) / fs->inodes_per_group, +1);
    flush_gdt(fs);
    return VFS_OK;
}

static int ext4_mkdir(void *mnt, const char *path,
                      uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ext4 *fs = (struct ext4 *)mnt;
    txn_begin(fs);
    return txn_finish(fs, do_mkdir(fs, path, uid, gid, mode));
}

/* ---- opendir / readdir / closedir ---- */

struct collect_ctx {
    struct ext4       *fs;
    struct vfs_dirent *out;
    size_t             cap;
    size_t             count;
    int                err;
};

static int collect_cb(void *user, uint32_t ino, uint8_t ftype,
                      const char *name, uint8_t nlen) {
    struct collect_ctx *ctx = (struct collect_ctx *)user;

    /* Skip "." and "..". The VFS contract is to expose only real
     * children; the ramfs / fat32 / tobyfs drivers all behave the
     * same way. */
    if (nlen == 1 && name[0] == '.') return VFS_OK;
    if (nlen == 2 && name[0] == '.' && name[1] == '.') return VFS_OK;

    if (ctx->count >= ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 16;
        struct vfs_dirent *grown =
            kmalloc(new_cap * sizeof(struct vfs_dirent));
        if (!grown) { ctx->err = VFS_ERR_NOMEM; return VFS_ERR_NOMEM; }
        if (ctx->count) memcpy(grown, ctx->out,
                                ctx->count * sizeof(struct vfs_dirent));
        if (ctx->out) kfree(ctx->out);
        ctx->out = grown;
        ctx->cap = new_cap;
    }

    /* Look up the child inode so we can fill in size + mode. Cheap
     * enough at opendir time -- our test directories have a handful
     * of entries each. */
    struct ext4_inode child;
    int rc = read_inode(ctx->fs, ino, &child);
    if (rc != VFS_OK) { ctx->err = rc; return rc; }

    struct vfs_dirent *e = &ctx->out[ctx->count++];
    memset(e, 0, sizeof(*e));
    size_t cp = nlen < VFS_NAME_MAX - 1 ? nlen : VFS_NAME_MAX - 1;
    memcpy(e->name, name, cp);
    e->name[cp] = 0;

    if (ftype == EXT4_FT_DIR ||
        (child.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        e->type = VFS_TYPE_DIR;
        e->size = 0;
    } else {
        e->type = VFS_TYPE_FILE;
        e->size = (size_t)(((uint64_t)child.i_size_hi << 32) |
                           child.i_size_lo);
    }
    e->uid  = child.i_uid;
    e->gid  = child.i_gid;
    e->mode = child.i_mode & VFS_MODE_PERMS;
    if (child.i_mode != 0) e->mode |= VFS_MODE_VALID;
    return VFS_OK;
}

static int ext4_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode dir;
    rc = read_inode(fs, ino, &dir);
    if (rc != VFS_OK) return rc;
    if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    struct collect_ctx ctx = { .fs = fs, .out = NULL, .cap = 0,
                                .count = 0, .err = VFS_OK };
    rc = dir_walk(fs, &dir, collect_cb, &ctx);
    if (rc != VFS_OK || ctx.err != VFS_OK) {
        if (ctx.out) kfree(ctx.out);
        return rc != VFS_OK ? rc : ctx.err;
    }

    struct ext4_diriter *it = kcalloc(1, sizeof(*it));
    if (!it) {
        if (ctx.out) kfree(ctx.out);
        return VFS_ERR_NOMEM;
    }
    it->ents  = ctx.out;
    it->count = ctx.count;

    out->priv  = it;
    out->index = 0;
    return VFS_OK;
}

static int ext4_closedir(struct vfs_dir *d) {
    if (d && d->priv) {
        struct ext4_diriter *it = (struct ext4_diriter *)d->priv;
        if (it->ents) kfree(it->ents);
        kfree(it);
        d->priv = NULL;
    }
    return VFS_OK;
}

static int ext4_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct ext4_diriter *it = (struct ext4_diriter *)d->priv;
    if (!it || d->index >= it->count) return VFS_ERR_NOENT;
    *out = it->ents[d->index++];
    return VFS_OK;
}

/* ================================================================
 * Metadata + truncate.
 *
 * These five ops were NULL, and a NULL vfs op is not a stub: vfs.c turns
 * it into VFS_ERR_ROFS. So every chmod, chown, utimes and truncate on an
 * ext4 volume failed with "read-only filesystem" on a volume that was
 * plainly writable, and nothing in the tree tested it. `make` compares
 * mtimes, `tar -p` restores modes, `cp -p` does both -- all of them
 * quietly misbehave against a filesystem that drops the metadata.
 * ================================================================ */

static uint64_t ext4_now_secs(void) {
    extern uint64_t lx_realtime_ns(uint64_t mono_ns);
    extern uint64_t perf_now_ns(void);
    return lx_realtime_ns(perf_now_ns()) / 1000000000ull;
}


/* ee_len carries an "uninitialised" flag in its top bit; the length is the
 * remainder. Written once here so the +/-32768 dance is not repeated. */
static uint32_t extent_len_of(const struct ext4_extent *e) {
    uint16_t l = e->ee_len;
    return (l > 32768) ? (uint32_t)(l - 32768) : (uint32_t)l;
}
static void extent_len_set(struct ext4_extent *e, uint32_t len) {
    bool uninit = e->ee_len > 32768;
    e->ee_len = (uint16_t)(uninit ? len + 32768 : len);
}

/* Release every block from logical block `from` onward, keeping everything
 * below it. inode_free_data() is the `from == 0` case of this and stays as
 * it is -- unlink has no partial extents to think about. */
static int inode_trim_from(struct ext4 *fs, struct ext4_inode *in,
                           uint32_t from) {
    uint64_t kept = 0;                        /* data blocks still in use */

    if (in->i_flags & EXT4_EXTENTS_FL) {
        struct ext4_extent_header *eh =
            (struct ext4_extent_header *)&in->i_block[0];
        if (eh->eh_magic != EXT4_EXT_MAGIC) return VFS_ERR_INVAL;
        /* An index tree would need the leaf blocks walked and possibly
         * freed. We never build one (extent_append_block refuses), so
         * refuse here too rather than trim a tree we cannot navigate. */
        if (eh->eh_depth != 0) return VFS_ERR_NOSPC;

        struct ext4_extent *e = (struct ext4_extent *)(eh + 1);
        uint16_t keep = 0;
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            uint32_t len   = extent_len_of(&e[i]);
            uint32_t first = e[i].ee_block;
            uint64_t start = ((uint64_t)e[i].ee_start_hi << 32) |
                             e[i].ee_start_lo;
            if (len == 0) continue;

            if (first >= from) {              /* wholly past the cut */
                for (uint32_t j = 0; j < len; j++)
                    ext4_free_block(fs, (uint32_t)(start + j));
                continue;                     /* entry disappears */
            }
            if (first + len > from) {         /* straddles it */
                uint32_t keep_len = from - first;
                for (uint32_t j = keep_len; j < len; j++)
                    ext4_free_block(fs, (uint32_t)(start + j));
                extent_len_set(&e[i], keep_len);
                len = keep_len;
            }
            if (keep != i) e[keep] = e[i];    /* compact in place */
            keep++;
            kept += len;
        }
        eh->eh_entries = keep;
    } else {
        for (uint32_t i = 0; i < 12; i++) {
            if (!in->i_block[i]) continue;
            if (i >= from) {
                ext4_free_block(fs, in->i_block[i]);
                in->i_block[i] = 0;
            } else {
                kept++;
            }
        }
        uint32_t per_block = fs->block_size / 4;
        if (in->i_block[12]) {
            uint8_t *ind = kmalloc(fs->block_size);
            if (!ind) return VFS_ERR_NOMEM;
            if (read_block(fs, in->i_block[12], ind) == VFS_OK) {
                uint32_t *tbl = (uint32_t *)ind;
                uint32_t still = 0;
                bool dirty = false;
                for (uint32_t i = 0; i < per_block; i++) {
                    if (!tbl[i]) continue;
                    if (12u + i >= from) {
                        ext4_free_block(fs, tbl[i]);
                        tbl[i] = 0;
                        dirty  = true;
                    } else {
                        still++;
                    }
                }
                if (still == 0) {             /* the table itself is dead */
                    ext4_free_block(fs, in->i_block[12]);
                    in->i_block[12] = 0;
                } else {
                    kept += still + 1;        /* +1: the indirect block */
                    if (dirty) (void)write_block(fs, in->i_block[12], ind);
                }
            }
            kfree(ind);
        }
    }

    in->i_blocks_lo = (uint32_t)(kept * (fs->block_size / 512));
    return VFS_OK;
}

/* The shared core of truncate(2) and ftruncate(2). `in` is updated in
 * place so an open handle's cached copy stays true. */
static int ext4_do_truncate(struct ext4 *fs, uint32_t ino,
                            struct ext4_inode *in, uint64_t len) {
    if ((in->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;
    uint64_t old = ((uint64_t)in->i_size_hi << 32) | in->i_size_lo;

    txn_begin(fs);
    int rc = VFS_OK;

    if (len < old) {
        /* Zero the tail of the last SURVIVING block before releasing the
         * rest. Without this the bytes past `len` stay on disk inside a
         * block we keep, and truncating back UP later would hand them
         * straight back to the reader -- the file would grow with its own
         * old contents where POSIX promises zeroes. */
        uint32_t off = (uint32_t)(len % fs->block_size);
        if (off) {
            uint64_t phys = 0;
            rc = inode_block_map(fs, in,
                                 (uint32_t)(len / fs->block_size), &phys);
            if (rc == VFS_OK && phys) {
                uint8_t *tmp = kmalloc(fs->block_size);
                if (!tmp) {
                    rc = VFS_ERR_NOMEM;
                } else {
                    rc = read_block(fs, phys, tmp);
                    if (rc == VFS_OK) {
                        memset(tmp + off, 0, fs->block_size - off);
                        rc = write_block(fs, phys, tmp);
                    }
                    kfree(tmp);
                }
            }
        }
        if (rc == VFS_OK) {
            uint32_t first_free =
                (uint32_t)((len + fs->block_size - 1) / fs->block_size);
            rc = inode_trim_from(fs, in, first_free);
        }
    }
    /* Growing allocates nothing. An unmapped block is a hole and
     * inode_read() already returns zeroes for one, so a larger i_size is
     * the entire job -- the file just becomes sparse, as it would on
     * Linux. */

    if (rc == VFS_OK) {
        in->i_size_lo = (uint32_t)(len & 0xFFFFFFFFu);
        in->i_size_hi = (uint32_t)(len >> 32);
        uint32_t now  = (uint32_t)ext4_now_secs();
        in->i_mtime = now;
        in->i_ctime = now;
        rc = write_inode(fs, ino, in);
    }
    return txn_finish(fs, rc);
}

static int ext4_truncate(void *mnt, const char *path, uint64_t length) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    return ext4_do_truncate(fs, ino, &in, length);
}

static int ext4_ftruncate(struct vfs_file *f, uint64_t length) {
    struct ext4 *fs = (struct ext4 *)f->mnt;
    struct ext4_filepriv *fp = (struct ext4_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    /* Critical here, not merely tidy: trimming with a stale extent tree
     * would free blocks the file no longer owns. */
    int rc = fp_refresh(fs, fp);
    if (rc != VFS_OK) return rc;
    rc = ext4_do_truncate(fs, fp->inode_no, &fp->in, length);
    if (rc == VFS_OK) {
        fp->file_size = length;
        f->size       = (size_t)length;
        /* The offset deliberately does NOT move: POSIX says ftruncate
         * leaves it alone, and a read past EOF already returns 0. */
    }
    return rc;
}

/* A small shared prologue: fetch the inode, apply the change, stamp ctime
 * where POSIX says to, and write it back inside one transaction. */
typedef void (*ext4_meta_fn)(struct ext4_inode *, uint64_t, uint64_t);

static int ext4_meta_update(struct ext4 *fs, const char *path,
                            ext4_meta_fn apply, uint64_t a, uint64_t b,
                            bool stamp_ctime) {
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    apply(&in, a, b);
    if (stamp_ctime) in.i_ctime = (uint32_t)ext4_now_secs();

    txn_begin(fs);
    rc = write_inode(fs, ino, &in);
    return txn_finish(fs, rc);
}

static void apply_chmod(struct ext4_inode *in, uint64_t mode, uint64_t un) {
    (void)un;
    /* Keep the format bits (S_IFREG/S_IFDIR); replace only the 12
     * permission bits, setuid/setgid/sticky included. */
    in->i_mode = (uint16_t)((in->i_mode & EXT4_S_IFMT) | (mode & 07777u));
}
static void apply_chown(struct ext4_inode *in, uint64_t uid, uint64_t gid) {
    if ((uint32_t)uid != (uint32_t)-1) ext4_inode_set_uid(in, (uint32_t)uid);
    if ((uint32_t)gid != (uint32_t)-1) ext4_inode_set_gid(in, (uint32_t)gid);
}
static void apply_utimes(struct ext4_inode *in, uint64_t mtime,
                         uint64_t atime) {
    in->i_mtime = (uint32_t)mtime;
    in->i_atime = (uint32_t)atime;
}

static int ext4_chmod(void *mnt, const char *path, uint32_t mode) {
    return ext4_meta_update((struct ext4 *)mnt, path, apply_chmod,
                            mode, 0, true);
}
static int ext4_chown(void *mnt, const char *path, uint32_t uid,
                      uint32_t gid) {
    return ext4_meta_update((struct ext4 *)mnt, path, apply_chown,
                            uid, gid, true);
}
static int ext4_utimes(void *mnt, const char *path, uint64_t mtime,
                       uint64_t atime) {
    /* utimes sets the two times the caller named; ctime is not one of
     * them, so it is deliberately left alone. */
    return ext4_meta_update((struct ext4 *)mnt, path, apply_utimes,
                            mtime, atime, false);
}

static int ext4_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    memset(out, 0, sizeof(*out));
    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
    } else {
        out->type = VFS_TYPE_FILE;
        out->size = (size_t)(((uint64_t)in.i_size_hi << 32) | in.i_size_lo);
    }
    out->uid  = ext4_inode_uid(&in);
    out->gid  = ext4_inode_gid(&in);
    /* The timestamps were never reported, so every file on an ext4 volume
     * looked like it was last modified at the epoch -- which makes `make`
     * rebuild everything, every time, and `find -newer` useless. */
    out->mtime = in.i_mtime;
    out->atime = in.i_atime;
    out->nlink = in.i_links_count;
    out->mode = in.i_mode & VFS_MODE_PERMS;
    if (in.i_mode != 0) out->mode |= VFS_MODE_VALID;
    return VFS_OK;
}

static const struct vfs_ops ext4_ops = {
    .open     = ext4_open,
    .close    = ext4_close,
    .read     = ext4_read,
    .write    = ext4_write,
    .create   = ext4_create,
    .unlink   = ext4_unlink,
    .rename   = ext4_rename,   /* Phase H */
    .mkdir    = ext4_mkdir,
    .opendir  = ext4_opendir,
    .closedir = ext4_closedir,
    .readdir  = ext4_readdir,
    .stat      = ext4_stat,
    .chmod     = ext4_chmod,
    .chown     = ext4_chown,
    .utimes    = ext4_utimes,
    .truncate  = ext4_truncate,
    .ftruncate = ext4_ftruncate,
    .link      = ext4_link,
    .statfs    = ext4_statfs,   /* Phase H */
};

/* ---- probe + mount entry points ---- */

/* Read just the SB; check magic + that the block size is sane and
 * the incompat features don't include anything we refuse. */
int ext4_probe(struct blk_dev *dev) {
    if (!dev) return 0;
    uint8_t buf[1024];
    /* Superblock starts at byte offset 1024 -> sector 2 (assuming
     * 512-byte sectors, which is invariant in our blk layer). */
    if (blk_read(dev, 2, 2, buf) != 0) return 0;
    const struct ext4_super_block *sb = (const struct ext4_super_block *)buf;
    if (sb->s_magic != EXT4_SUPER_MAGIC) return 0;
    if (sb->s_log_block_size > 6) return 0;          /* > 64 KiB? bogus */

    uint32_t bs = 1024u << sb->s_log_block_size;
    if (bs != 1024 && bs != 2048 && bs != 4096) return 0;

    /* INLINE_DATA we can't read at all; refuse. A NEEDS_RECOVERY image is
     * now allowed through -- mount() recovers it if the journal is one we
     * can drive, and otherwise refuses cleanly. */
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) return 0;
    return 1;
}

/* Free a mounted fs's heap state (buffers + struct). Used by failure
 * paths and the self-test teardown (a live VFS mount must NOT be freed
 * this way -- only an fs that was never vfs_mount'd, or after unmount). */
static void ext4_free(struct ext4 *fs) {
    if (!fs) return;
    if (fs->blk_buf)  kfree(fs->blk_buf);
    if (fs->blk_buf2) kfree(fs->blk_buf2);
    if (fs->gdt_buf)  kfree(fs->gdt_buf);
    if (fs->sb_buf)   kfree(fs->sb_buf);
    kfree(fs);
}

/* Build an in-memory `struct ext4` from `dev`: read + validate the
 * superblock, cache the GDT + SB, and confirm the root inode decodes.
 * Returns the fs (caller owns it) or NULL on any failure. Shared by
 * ext4_mount (which then vfs_mounts it) and the self-test (which drives
 * the ext4_ops directly without a global mount). */
static struct ext4 *ext4_setup(struct blk_dev *dev) {
    if (!dev) return NULL;

    /* Heap, not stack: this can be reached deep in kmain's call chain
     * (boot stack is only ~16 KiB), and a 1 KiB stack buffer there risks
     * overflowing into adjacent memory. */
    uint8_t *sb_raw = kmalloc(1024);
    if (!sb_raw) return NULL;
    if (blk_read(dev, 2, 2, sb_raw) != 0) {
        kprintf("[ext4] superblock read failed\n");
        kfree(sb_raw);
        return NULL;
    }
    const struct ext4_super_block *sb = (const struct ext4_super_block *)sb_raw;
    if (sb->s_magic != EXT4_SUPER_MAGIC) {
        kprintf("[ext4] bad magic 0x%x (want 0xEF53)\n", (unsigned)sb->s_magic);
        kfree(sb_raw);
        return NULL;
    }
    /* NEEDS_RECOVERY is handled below (after we locate the journal): we
     * replay if the journal is drivable, else refuse. INLINE_DATA we can
     * never read. */
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) {
        kprintf("[ext4] refuse mount: INLINE_DATA not supported\n");
        kfree(sb_raw);
        return NULL;
    }

    struct ext4 *fs = kcalloc(1, sizeof(*fs));
    if (!fs) { kfree(sb_raw); return NULL; }
    fs->dev = dev;
    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->sectors_per_block= fs->block_size / BLK_SECTOR_SIZE;
    fs->total_blocks     = sb->s_blocks_count_lo;
    fs->feat_64bit       = (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
    if (fs->feat_64bit)
        fs->total_blocks |= ((uint64_t)sb->s_blocks_count_hi) << 32;
    fs->total_inodes     = sb->s_inodes_count;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->first_ino        = (sb->s_rev_level >= 1) ? sb->s_first_ino : 11;
    fs->inode_size       = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->feature_incompat = sb->s_feature_incompat;
    fs->desc_size        = sb->s_desc_size;
    if (fs->desc_size < 32) fs->desc_size = 32;
    fs->desc_64bit = fs->feat_64bit && fs->desc_size >= 64;
    fs->first_data_block = (fs->block_size == 1024) ? 1 : 0;

    if (fs->inodes_per_group == 0 || fs->blocks_per_group == 0 ||
        fs->inode_size < 128 || fs->total_blocks == 0) {
        kprintf("[ext4] superblock fields out of range\n");
        kfree(sb_raw);
        kfree(fs);
        return NULL;
    }

    fs->group_count = (uint32_t)((fs->total_blocks + fs->blocks_per_group - 1) /
                                  fs->blocks_per_group);
    fs->gdt_bytes = fs->group_count * fs->desc_size;
    uint32_t gdt_blocks = (fs->gdt_bytes + fs->block_size - 1) / fs->block_size;

    fs->blk_buf  = kmalloc(fs->block_size);
    fs->blk_buf2 = kmalloc(fs->block_size);
    fs->gdt_buf  = kmalloc(gdt_blocks * fs->block_size);
    fs->sb_buf   = kmalloc(1024);
    if (!fs->blk_buf || !fs->blk_buf2 || !fs->gdt_buf || !fs->sb_buf) {
        kfree(sb_raw);
        ext4_free(fs);
        return NULL;
    }
    memcpy(fs->sb_buf, sb_raw, 1024);      /* cache SB for write-back */
    kfree(sb_raw);                          /* done with the temp SB read buffer */

    uint32_t gdt_first_blk = fs->first_data_block + 1;
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        if (read_block(fs, gdt_first_blk + i,
                       fs->gdt_buf + i * fs->block_size) != VFS_OK) {
            kprintf("[ext4] GDT read failed at block %u\n", gdt_first_blk + i);
            ext4_free(fs);
            return NULL;
        }
    }

    struct ext4_inode root;
    if (read_inode(fs, EXT4_ROOT_INODE, &root) != VFS_OK ||
        (root.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
        kprintf("[ext4] root inode (#2) is not a directory (mode=0x%x)\n",
                (unsigned)root.i_mode);
        ext4_free(fs);
        return NULL;
    }

    /* ---- JBD2 journal: detect, validate, recover ---- */
    struct ext4_super_block *sbc = (struct ext4_super_block *)fs->sb_buf;
    if (sbc->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
        uint32_t jinum = sbc->s_journal_inum ? sbc->s_journal_inum
                                             : EXT4_JOURNAL_INODE;
        uint8_t *jsb = NULL;
        if (journal_locate(fs, jinum) &&
            (jsb = kmalloc(fs->block_size)) != NULL &&
            jraw_read(fs, 0, jsb) == VFS_OK) {
            jbd2_superblock_t *js = (jbd2_superblock_t *)jsb;
            uint32_t jmagic    = be32(js->s_header.h_magic);
            uint32_t jtype     = be32(js->s_header.h_blocktype);
            uint32_t jincompat = be32(js->s_feature_incompat);
            uint32_t jbsize    = be32(js->s_blocksize);
            uint32_t jstart    = be32(js->s_start);
            bool drivable =
                jmagic == JBD2_MAGIC_NUMBER &&
                (jtype == JBD2_SUPERBLOCK_V2 || jtype == JBD2_SUPERBLOCK_V1) &&
                jbsize == fs->block_size &&
                (jincompat & ~JBD2_INCOMPAT_SUPPORTED) == 0;
            if (drivable) {
                fs->j_first    = be32(js->s_first);
                if (fs->j_first == 0) fs->j_first = 1;
                fs->j_sequence = be32(js->s_sequence);
                fs->has_journal = true;
                kfree(jsb); jsb = NULL;
                if (jstart != 0) {
                    kprintf("[ext4] journal needs recovery (s_start=%u, "
                            "seq=%u)\n", jstart, fs->j_sequence);
                    if (jbd2_recover(fs) != VFS_OK) {
                        kprintf("[ext4] journal recovery FAILED\n");
                        ext4_free(fs);
                        return NULL;
                    }
                }
            } else {
                kprintf("[ext4] journal present but not drivable "
                        "(magic=0x%x incompat=0x%x bs=%u); in-place writes\n",
                        jmagic, jincompat, jbsize);
            }
        }
        if (jsb) kfree(jsb);
    }
    /* NEEDS_RECOVERY but we couldn't drive its journal -> refuse rather
     * than corrupt it with in-place writes. */
    if ((fs->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) &&
        !fs->has_journal) {
        kprintf("[ext4] refuse mount: NEEDS_RECOVERY, journal not drivable\n");
        ext4_free(fs);
        return NULL;
    }
    return fs;
}

int ext4_mount(const char *mount_point, struct blk_dev *dev) {
    if (!mount_point || !dev) return VFS_ERR_INVAL;

    struct ext4 *fs = ext4_setup(dev);
    if (!fs) return VFS_ERR_INVAL;

    int rc = vfs_mount(mount_point, &ext4_ops, fs);
    if (rc != VFS_OK) {
        kprintf("[ext4] vfs_mount('%s') failed: %d\n", mount_point, rc);
        ext4_free(fs);
        return rc;
    }

    kprintf("[ext4] mounted '%s' on %s: %u blocks x %u B "
            "(%u KiB total, %u groups, inode_size=%u, first_ino=%u, %s, rw%s)\n",
            mount_point, dev->name ? dev->name : "(anon)",
            (unsigned)fs->total_blocks, fs->block_size,
            (unsigned)((fs->total_blocks * fs->block_size) / 1024u),
            fs->group_count, fs->inode_size, fs->first_ino,
            (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) ?
                "extents" : "legacy-ptrs",
            fs->has_journal ? "+jbd2" : "");
    return VFS_OK;
}

/* ================================================================
 * In-kernel minimal ext4 formatter (for the self-test; no host mke2fs)
 *
 * Produces a single-block-group, 4 KiB-block, extents ext4 that the
 * read+write paths fully understand. Layout:
 *   block 0      : boot area + superblock (at byte offset 1024)
 *   block 1      : group descriptor table (one 32-byte descriptor)
 *   block 2      : block bitmap
 *   block 3      : inode bitmap
 *   blocks 4..   : inode table (EXT4FMT_IPG inodes x 256 B)
 *   root dir     : root directory block ("." / "..")
 *   [journal]    : with_journal -> EXT4FMT_JBLOCKS-block JBD2 journal
 *                  (inode 8); block 0 of it is the journal superblock.
 * Reserved inodes 1..10 are marked used; inode 2 is the root dir and
 * inode 8 (when journalled) is the journal.
 * ================================================================ */

#define EXT4FMT_BS       4096u
#define EXT4FMT_SPB      (EXT4FMT_BS / BLK_SECTOR_SIZE)   /* 8 */
#define EXT4FMT_ISIZE    256u
#define EXT4FMT_IPG      512u                              /* inodes/group */
#define EXT4FMT_BPG      (EXT4FMT_BS * 8u)                 /* 32768 blocks/group */
#define EXT4FMT_JBLOCKS  256u                              /* 1 MiB journal */

static int fmt_write_block(struct blk_dev *dev, uint64_t blk, const void *buf) {
    if (blk_write(dev, blk * EXT4FMT_SPB, EXT4FMT_SPB, buf) != 0)
        return VFS_ERR_IO;
    return VFS_OK;
}

int ext4_format_ex(struct blk_dev *dev, int with_journal) {
    if (!dev) return VFS_ERR_INVAL;
    uint64_t total_blocks = dev->sector_count / EXT4FMT_SPB;
    if (total_blocks < 64 || total_blocks > EXT4FMT_BPG) {
        kprintf("[ext4] format: device size %lu blocks out of range "
                "(need 64..%u, single group)\n",
                (unsigned long)total_blocks, EXT4FMT_BPG);
        return VFS_ERR_INVAL;
    }

    const uint32_t itable_blocks = (EXT4FMT_IPG * EXT4FMT_ISIZE + EXT4FMT_BS - 1)
                                   / EXT4FMT_BS;                  /* 32 */
    const uint32_t sb_block       = 0;
    const uint32_t gdt_block       = 1;
    const uint32_t bbitmap_block   = 2;
    const uint32_t ibitmap_block   = 3;
    const uint32_t itable_block     = 4;
    const uint32_t first_data       = itable_block + itable_blocks;   /* 36 */
    const uint32_t root_dir_block   = first_data;
    const uint32_t journal_blocks   = with_journal ? EXT4FMT_JBLOCKS : 0;
    const uint32_t journal_start    = root_dir_block + 1;
    const uint32_t used_blocks      = journal_start + journal_blocks;

    if (with_journal && total_blocks < used_blocks + 8) {
        kprintf("[ext4] format: device too small for a %u-block journal "
                "(have %lu blocks, need >= %u)\n", journal_blocks,
                (unsigned long)total_blocks, used_blocks + 8);
        return VFS_ERR_INVAL;
    }

    uint8_t *blk = kcalloc(1, EXT4FMT_BS);
    if (!blk) return VFS_ERR_NOMEM;
    int rc = VFS_OK;

    /* --- block 0: superblock at offset 1024 --- */
    memset(blk, 0, EXT4FMT_BS);
    struct ext4_super_block *sb = (struct ext4_super_block *)(blk + 1024);
    sb->s_inodes_count        = EXT4FMT_IPG;
    sb->s_blocks_count_lo     = (uint32_t)total_blocks;
    sb->s_free_blocks_count_lo = (uint32_t)(total_blocks - used_blocks);
    sb->s_free_inodes_count   = EXT4FMT_IPG - 10;     /* inodes 1..10 reserved */
    sb->s_first_data_block    = 0;
    sb->s_log_block_size      = 2;                    /* 4096 */
    sb->s_log_cluster_size    = 2;
    sb->s_blocks_per_group    = EXT4FMT_BPG;
    sb->s_clusters_per_group  = EXT4FMT_BPG;
    sb->s_inodes_per_group    = EXT4FMT_IPG;
    sb->s_magic               = EXT4_SUPER_MAGIC;
    sb->s_state               = 1;                    /* clean */
    sb->s_errors              = 1;
    sb->s_rev_level           = 1;
    sb->s_first_ino           = 11;
    sb->s_inode_size          = EXT4FMT_ISIZE;
    sb->s_feature_incompat    = EXT4_FEATURE_INCOMPAT_FILETYPE |
                                EXT4_FEATURE_INCOMPAT_EXTENTS;
    if (with_journal) {
        sb->s_feature_compat |= EXT4_FEATURE_COMPAT_HAS_JOURNAL;
        sb->s_journal_inum    = EXT4_JOURNAL_INODE;
    }
    /* No ro_compat checksums, 32-byte descs. */
    rc = fmt_write_block(dev, sb_block, blk);
    if (rc != VFS_OK) goto out;

    /* --- block 1: group descriptor table (one descriptor) --- */
    memset(blk, 0, EXT4FMT_BS);
    struct ext4_group_desc_32 *gd = (struct ext4_group_desc_32 *)blk;
    gd->bg_block_bitmap_lo      = bbitmap_block;
    gd->bg_inode_bitmap_lo      = ibitmap_block;
    gd->bg_inode_table_lo       = itable_block;
    gd->bg_free_blocks_count_lo = (uint16_t)(total_blocks - used_blocks);
    gd->bg_free_inodes_count_lo = (uint16_t)(EXT4FMT_IPG - 10);
    gd->bg_used_dirs_count_lo   = 1;                  /* root */
    rc = fmt_write_block(dev, gdt_block, blk);
    if (rc != VFS_OK) goto out;

    /* --- block 2: block bitmap (bit N = block N) --- */
    memset(blk, 0, EXT4FMT_BS);
    for (uint32_t b = 0; b < used_blocks; b++)         /* metadata + root + journal */
        blk[b / 8] |= (uint8_t)(1u << (b & 7));
    for (uint32_t b = (uint32_t)total_blocks; b < EXT4FMT_BPG; b++) /* nonexistent */
        blk[b / 8] |= (uint8_t)(1u << (b & 7));
    rc = fmt_write_block(dev, bbitmap_block, blk);
    if (rc != VFS_OK) goto out;

    /* --- block 3: inode bitmap (bit i = inode i+1) --- */
    memset(blk, 0, EXT4FMT_BS);
    for (uint32_t i = 0; i < 10; i++)                  /* inodes 1..10 reserved */
        blk[i / 8] |= (uint8_t)(1u << (i & 7));
    rc = fmt_write_block(dev, ibitmap_block, blk);
    if (rc != VFS_OK) goto out;

    /* --- inode table: zero all blocks, then write inodes 2 + 8 (both
     *     land in the first itable block, block 4) --- */
    memset(blk, 0, EXT4FMT_BS);
    for (uint32_t i = 0; i < itable_blocks; i++) {
        rc = fmt_write_block(dev, itable_block + i, blk);
        if (rc != VFS_OK) goto out;
    }
    {
        memset(blk, 0, EXT4FMT_BS);          /* block 4 holds inodes 1..16 */
        /* root inode #2 at byte offset (2-1)*256 = 256 */
        struct ext4_inode *root =
            (struct ext4_inode *)(blk + (EXT4_ROOT_INODE - 1) * EXT4FMT_ISIZE);
        root->i_mode        = EXT4_S_IFDIR | 0755u;
        root->i_links_count = 2;             /* "." + parent's entry */
        root->i_size_lo     = EXT4FMT_BS;
        root->i_blocks_lo   = EXT4FMT_BS / 512;
        root->i_flags       = EXT4_EXTENTS_FL;
        struct ext4_extent_header *reh =
            (struct ext4_extent_header *)&root->i_block[0];
        reh->eh_magic   = EXT4_EXT_MAGIC;
        reh->eh_entries = 1;
        reh->eh_max     = (sizeof(root->i_block) - sizeof(*reh)) /
                          sizeof(struct ext4_extent);
        reh->eh_depth   = 0;
        struct ext4_extent *re = (struct ext4_extent *)(reh + 1);
        re->ee_block    = 0;
        re->ee_len      = 1;
        re->ee_start_hi = 0;
        re->ee_start_lo = root_dir_block;

        if (with_journal) {
            /* journal inode #8 at byte offset (8-1)*256 = 1792, one
             * contiguous extent covering the whole journal region. */
            struct ext4_inode *ji =
                (struct ext4_inode *)(blk + (EXT4_JOURNAL_INODE - 1) * EXT4FMT_ISIZE);
            ji->i_mode        = EXT4_S_IFREG | 0600u;
            ji->i_links_count = 1;
            ji->i_size_lo     = journal_blocks * EXT4FMT_BS;
            ji->i_blocks_lo   = journal_blocks * (EXT4FMT_BS / 512);
            ji->i_flags       = EXT4_EXTENTS_FL;
            struct ext4_extent_header *jeh =
                (struct ext4_extent_header *)&ji->i_block[0];
            jeh->eh_magic   = EXT4_EXT_MAGIC;
            jeh->eh_entries = 1;
            jeh->eh_max     = (sizeof(ji->i_block) - sizeof(*jeh)) /
                              sizeof(struct ext4_extent);
            jeh->eh_depth   = 0;
            struct ext4_extent *je = (struct ext4_extent *)(jeh + 1);
            je->ee_block    = 0;
            je->ee_len      = (uint16_t)journal_blocks;
            je->ee_start_hi = 0;
            je->ee_start_lo = journal_start;
        }
        rc = fmt_write_block(dev, itable_block, blk);
        if (rc != VFS_OK) goto out;
    }

    /* --- root directory block: "." and ".." --- */
    memset(blk, 0, EXT4FMT_BS);
    {
        struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)blk;
        dot->inode = EXT4_ROOT_INODE; dot->rec_len = 12; dot->name_len = 1;
        dot->file_type = EXT4_FT_DIR; memcpy((char *)dot + 8, ".", 1);
        struct ext4_dir_entry_2 *dd = (struct ext4_dir_entry_2 *)(blk + 12);
        dd->inode = EXT4_ROOT_INODE; dd->rec_len = (uint16_t)(EXT4FMT_BS - 12);
        dd->name_len = 2; dd->file_type = EXT4_FT_DIR; memcpy((char *)dd + 8, "..", 2);
        rc = fmt_write_block(dev, root_dir_block, blk);
        if (rc != VFS_OK) goto out;
    }

    /* --- journal superblock (journal block 0), clean (s_start = 0) --- */
    if (with_journal) {
        memset(blk, 0, EXT4FMT_BS);
        jbd2_superblock_t *js = (jbd2_superblock_t *)blk;
        js->s_header.h_magic     = be32(JBD2_MAGIC_NUMBER);
        js->s_header.h_blocktype = be32(JBD2_SUPERBLOCK_V2);
        js->s_blocksize          = be32(EXT4FMT_BS);
        js->s_maxlen             = be32(journal_blocks);
        js->s_first              = be32(1);
        js->s_sequence           = be32(1);     /* first commit id */
        js->s_start              = be32(0);      /* clean */
        js->s_nr_users           = be32(1);
        rc = fmt_write_block(dev, journal_start, blk);
        if (rc != VFS_OK) goto out;
    }

out:
    kfree(blk);
    if (rc == VFS_OK)
        kprintf("[ext4] format: %lu blocks (4K), %u inodes, itable=%u blk, "
                "root=%u%s\n", (unsigned long)total_blocks, EXT4FMT_IPG,
                itable_blocks, root_dir_block,
                with_journal ? ", +jbd2 journal (inode 8)" : "");
    return rc;
}

int ext4_format(struct blk_dev *dev) {
    return ext4_format_ex(dev, 0);
}

/* ================================================================
 * Self-test: format a ramdev, mount via ext4_setup, exercise the write
 * paths (create / grow-via-extent write / mkdir / unlink) and verify,
 * including a re-mount to confirm on-disk persistence. Self-contained --
 * no host mke2fs, no QEMU disk. Emits greppable [EXT4ST] markers.
 * ================================================================ */

struct ext4_ramdev { uint8_t *buf; uint64_t bytes; };

static int e4ram_read(struct blk_dev *dev, uint64_t lba, uint32_t count, void *buf) {
    struct ext4_ramdev *r = (struct ext4_ramdev *)dev->priv;
    uint64_t off = lba * (uint64_t)BLK_SECTOR_SIZE;
    uint64_t span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(buf, r->buf + off, span);
    return 0;
}
static int e4ram_write(struct blk_dev *dev, uint64_t lba, uint32_t count, const void *buf) {
    struct ext4_ramdev *r = (struct ext4_ramdev *)dev->priv;
    uint64_t off = lba * (uint64_t)BLK_SECTOR_SIZE;
    uint64_t span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(r->buf + off, buf, span);
    return 0;
}
static const struct blk_ops e4ram_ops = { .read = e4ram_read, .write = e4ram_write };

static void e4st_fill(uint8_t *b, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)(seed * 131u + i * 17u + (i >> 8) * 7u);
}

/* Create `path`, then open + write `n` bytes of pattern `seed`. */
static int e4st_create_write(struct ext4 *fs, const char *path,
                             uint8_t *scratch, size_t n, uint32_t seed) {
    int rc = ext4_create(fs, path, 0, 0, 0644);
    if (rc != VFS_OK) return rc;
    struct vfs_file f; memset(&f, 0, sizeof(f));
    rc = ext4_open(fs, path, &f);
    if (rc != VFS_OK) return rc;
    f.mnt = fs; f.pos = 0;
    e4st_fill(scratch, n, seed);
    long w = ext4_write(&f, scratch, n);
    ext4_close(&f);
    if (w != (long)n) return w < 0 ? (int)w : VFS_ERR_IO;
    return VFS_OK;
}

/* Open `path`, read all `n` bytes, verify they match pattern `seed`. */
static int e4st_read_verify(struct ext4 *fs, const char *path,
                            uint8_t *scratch, size_t n, uint32_t seed) {
    struct vfs_file f; memset(&f, 0, sizeof(f));
    int rc = ext4_open(fs, path, &f);
    if (rc != VFS_OK) return rc;
    f.mnt = fs; f.pos = 0;
    if (f.size != n) { ext4_close(&f); return VFS_ERR_IO; }
    size_t got = 0;
    while (got < n) {
        long r = ext4_read(&f, scratch + got, n - got);
        if (r <= 0) { ext4_close(&f); return r < 0 ? (int)r : VFS_ERR_IO; }
        got += (size_t)r;
    }
    ext4_close(&f);
    uint8_t *exp = kmalloc(n);
    if (!exp) return VFS_ERR_NOMEM;
    e4st_fill(exp, n, seed);
    int ok = memcmp(scratch, exp, n) == 0;
    kfree(exp);
    return ok ? VFS_OK : VFS_ERR_IO;
}

int ext4_self_test(void) {
    const uint64_t bytes = 1u * 1024u * 1024u;        /* 1 MiB ramdev (256 blk) */
    const size_t   fsize = 10000;                     /* spans 3 x 4K blocks */
    int fails = 0;

    uint8_t *image = kmalloc((size_t)bytes);
    uint8_t *scratch = kmalloc(fsize);
    if (!image || !scratch) {
        kprintf("[EXT4ST] SKIP -- kmalloc failed\n");
        if (image) kfree(image); if (scratch) kfree(scratch);
        return -1;
    }
    memset(image, 0, (size_t)bytes);

    struct ext4_ramdev r = { .buf = image, .bytes = bytes };
    struct blk_dev dev; memset(&dev, 0, sizeof(dev));
    dev.name = "ext4-selftest"; dev.ops = &e4ram_ops;
    dev.sector_count = bytes / BLK_SECTOR_SIZE; dev.priv = &r;
    dev.class = BLK_CLASS_DISK;

    /* 0. format + mount */
    int rc = ext4_format(&dev);
    bool ok0 = (rc == VFS_OK);
    struct ext4 *fs = ok0 ? ext4_setup(&dev) : NULL;
    ok0 = ok0 && fs != NULL;
    if (!ok0) fails++;
    kprintf("[EXT4ST] step0 format+mount: %s\n", ok0 ? "PASS" : "FAIL");
    if (!fs) { kfree(image); kfree(scratch); return -1; }

    /* 1. create + multi-block write + read-back verify (extent growth) */
    rc = e4st_create_write(fs, "/foo", scratch, fsize, 0xA1);
    bool ok1 = (rc == VFS_OK) &&
               (e4st_read_verify(fs, "/foo", scratch, fsize, 0xA1) == VFS_OK);
    if (!ok1) fails++;
    kprintf("[EXT4ST] step1 create+grow-write+verify (%u B): %s\n",
            (unsigned)fsize, ok1 ? "PASS" : "FAIL");

    /* 2. in-place overwrite of an existing block, verify only it changed */
    {
        struct vfs_file f; memset(&f, 0, sizeof(f));
        rc = ext4_open(fs, "/foo", &f);
        if (rc == VFS_OK) {
            f.mnt = fs; f.pos = 4096;            /* start of block 1 */
            e4st_fill(scratch, 512, 0xB2);
            long w = ext4_write(&f, scratch, 512);
            ext4_close(&f);
            rc = (w == 512) ? VFS_OK : VFS_ERR_IO;
        }
        /* verify: byte at 4096..4607 = pattern B2; size unchanged. Read +
         * expected both go in the heap `scratch` buffer (no stack arrays
         * -- the boot stack is tight). */
        bool ok2 = (rc == VFS_OK);
        if (ok2) {
            struct vfs_file f2; memset(&f2, 0, sizeof(f2));
            if (ext4_open(fs, "/foo", &f2) == VFS_OK) {
                ok2 = (f2.size == fsize);
                f2.mnt = fs; f2.pos = 4096;
                long rr = ext4_read(&f2, scratch, 512);          /* read */
                ext4_close(&f2);
                e4st_fill(scratch + 512, 512, 0xB2);             /* expected */
                ok2 = ok2 && rr == 512 &&
                      memcmp(scratch, scratch + 512, 512) == 0;
            } else ok2 = false;
        }
        if (!ok2) fails++;
        kprintf("[EXT4ST] step2 in-place overwrite: %s\n", ok2 ? "PASS" : "FAIL");
    }

    /* 3. mkdir + nested create/write/verify */
    rc = ext4_mkdir(fs, "/d", 0, 0, 0755);
    bool ok3 = (rc == VFS_OK) &&
               (e4st_create_write(fs, "/d/bar", scratch, fsize, 0xC3) == VFS_OK) &&
               (e4st_read_verify(fs, "/d/bar", scratch, fsize, 0xC3) == VFS_OK);
    if (!ok3) fails++;
    kprintf("[EXT4ST] step3 mkdir + nested file: %s\n", ok3 ? "PASS" : "FAIL");

    /* 4. unlink /foo, confirm it's gone but /d/bar survives */
    rc = ext4_unlink(fs, "/foo");
    bool ok4 = (rc == VFS_OK);
    {
        struct vfs_file f; memset(&f, 0, sizeof(f));
        ok4 = ok4 && (ext4_open(fs, "/foo", &f) == VFS_ERR_NOENT);
    }
    if (!ok4) fails++;
    kprintf("[EXT4ST] step4 unlink: %s\n", ok4 ? "PASS" : "FAIL");

    /* 5. re-mount (fresh ext4_setup on the same image) -> persistence */
    ext4_free(fs);
    fs = ext4_setup(&dev);
    bool ok5 = (fs != NULL) &&
               (e4st_read_verify(fs, "/d/bar", scratch, fsize, 0xC3) == VFS_OK);
    {
        struct vfs_file f; memset(&f, 0, sizeof(f));
        ok5 = ok5 && fs && (ext4_open(fs, "/foo", &f) == VFS_ERR_NOENT);
    }
    if (!ok5) fails++;
    kprintf("[EXT4ST] step5 remount + persistence: %s\n", ok5 ? "PASS" : "FAIL");

    if (fs) ext4_free(fs);
    kfree(image);
    kfree(scratch);
    kprintf("[EXT4ST] %s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : -1;
}

/* ================================================================
 * JBD2 crash-consistency self-test.
 *
 * Formats a journalled ramdev, then for each phase of a transaction
 * (before the commit block / after the commit block / midway through the
 * checkpoint) injects a "power loss": the in-flight commit stops, the
 * in-memory fs is thrown away, and the image is remounted -- which runs
 * jbd2_recover(). We then assert that the operation was atomic:
 *   - crash before commit  -> the change vanished (rolled back);
 *   - crash after commit    -> the change is present (replayed);
 *   - crash mid-checkpoint  -> the half-written homes were healed.
 * Both metadata (create) and data (overwrite) transactions are tested.
 * Self-contained -- no host mke2fs, no QEMU disk. [EXT4JT] markers.
 * ================================================================ */

static bool e4jt_exists(struct ext4 *fs, const char *path) {
    uint32_t ino; uint8_t ftype;
    return path_to_inode(fs, path, &ino, &ftype) == VFS_OK;
}

/* Create `path` with crash `phase` armed, then model a power loss:
 * discard the in-memory fs and remount (which recovers). */
static struct ext4 *e4jt_crash_create(struct blk_dev *dev, struct ext4 *fs,
                                      const char *path, int phase) {
    g_jbd_crash_phase = phase;
    (void)ext4_create(fs, path, 0, 0, 0644);    /* commit halts at `phase` */
    g_jbd_crash_phase = JBD2_CRASH_NONE;
    ext4_free(fs);
    return ext4_setup(dev);                      /* remount -> jbd2_recover */
}

/* Overwrite all of /keep with pattern `seed`, crash at `phase`, remount. */
static struct ext4 *e4jt_crash_rewrite(struct blk_dev *dev, struct ext4 *fs,
                                       uint8_t *scratch, size_t n,
                                       uint32_t seed, int phase) {
    struct vfs_file f; memset(&f, 0, sizeof(f));
    if (ext4_open(fs, "/keep", &f) == VFS_OK) {
        f.mnt = fs; f.pos = 0;
        e4st_fill(scratch, n, seed);
        g_jbd_crash_phase = phase;
        ext4_write(&f, scratch, n);
        g_jbd_crash_phase = JBD2_CRASH_NONE;
        ext4_close(&f);
    }
    ext4_free(fs);
    return ext4_setup(dev);
}

int ext4_journal_self_test(void) {
    const uint64_t bytes = 4u * 1024u * 1024u;        /* 4 MiB ramdev (1024 blk) */
    const size_t   ksize = 8192;                      /* /keep: 2 x 4K blocks */
    int fails = 0;

    uint8_t *image = kmalloc((size_t)bytes);
    uint8_t *scratch = kmalloc(ksize);
    if (!image || !scratch) {
        kprintf("[EXT4JT] SKIP -- kmalloc failed\n");
        if (image) kfree(image); if (scratch) kfree(scratch);
        return -1;
    }
    memset(image, 0, (size_t)bytes);

    struct ext4_ramdev r = { .buf = image, .bytes = bytes };
    struct blk_dev dev; memset(&dev, 0, sizeof(dev));
    dev.name = "ext4-jbd-selftest"; dev.ops = &e4ram_ops;
    dev.sector_count = bytes / BLK_SECTOR_SIZE; dev.priv = &r;
    dev.class = BLK_CLASS_DISK;

    /* 0. format WITH journal + mount; confirm the journal is being driven. */
    int rc = ext4_format_ex(&dev, 1);
    struct ext4 *fs = (rc == VFS_OK) ? ext4_setup(&dev) : NULL;
    bool ok0 = fs && fs->has_journal;
    if (!ok0) fails++;
    kprintf("[EXT4JT] step0 format+mount (journalled): %s\n",
            ok0 ? "PASS" : "FAIL");
    if (!fs) { kfree(image); kfree(scratch); return -1; }

    /* 1. clean journalled create + multi-block write + verify (end-to-end). */
    rc = e4st_create_write(fs, "/keep", scratch, ksize, 0x5A);
    bool ok1 = (rc == VFS_OK) &&
               (e4st_read_verify(fs, "/keep", scratch, ksize, 0x5A) == VFS_OK);
    if (!ok1) fails++;
    kprintf("[EXT4JT] step1 clean journalled write+verify: %s\n",
            ok1 ? "PASS" : "FAIL");

    /* 2. crash BEFORE the commit block -> create atomically rolled back. */
    fs = e4jt_crash_create(&dev, fs, "/a", JBD2_CRASH_BEFORE_COMMIT);
    bool ok2 = fs && !e4jt_exists(fs, "/a") && e4jt_exists(fs, "/keep");
    if (!ok2) fails++;
    kprintf("[EXT4JT] step2 crash<commit rollback (/a absent): %s\n",
            ok2 ? "PASS" : "FAIL");
    if (!fs) { kfree(image); kfree(scratch); return -1; }

    /* 3. crash AFTER the commit block -> create atomically replayed. */
    fs = e4jt_crash_create(&dev, fs, "/b", JBD2_CRASH_AFTER_COMMIT);
    bool ok3 = fs && e4jt_exists(fs, "/b") && e4jt_exists(fs, "/keep");
    if (!ok3) fails++;
    kprintf("[EXT4JT] step3 crash>commit replay (/b present): %s\n",
            ok3 ? "PASS" : "FAIL");
    if (!fs) { kfree(image); kfree(scratch); return -1; }

    /* 4. crash mid-checkpoint -> half-written homes healed by full replay. */
    fs = e4jt_crash_create(&dev, fs, "/c", JBD2_CRASH_MID_CHECKPOINT);
    bool ok4 = fs && e4jt_exists(fs, "/c") &&
               e4jt_exists(fs, "/b") && e4jt_exists(fs, "/keep");
    if (!ok4) fails++;
    kprintf("[EXT4JT] step4 crash mid-checkpoint heal (/c present): %s\n",
            ok4 ? "PASS" : "FAIL");
    if (!fs) { kfree(image); kfree(scratch); return -1; }

    /* 5. data overwrite, crash BEFORE commit -> original data survives. */
    fs = e4jt_crash_rewrite(&dev, fs, scratch, ksize, 0x77,
                            JBD2_CRASH_BEFORE_COMMIT);
    bool ok5 = fs &&
               (e4st_read_verify(fs, "/keep", scratch, ksize, 0x5A) == VFS_OK);
    if (!ok5) fails++;
    kprintf("[EXT4JT] step5 overwrite crash<commit rollback (0x5A kept): %s\n",
            ok5 ? "PASS" : "FAIL");
    if (!fs) { kfree(image); kfree(scratch); return -1; }

    /* 6. data overwrite, crash AFTER commit -> new data replayed. */
    fs = e4jt_crash_rewrite(&dev, fs, scratch, ksize, 0x77,
                            JBD2_CRASH_AFTER_COMMIT);
    bool ok6 = fs &&
               (e4st_read_verify(fs, "/keep", scratch, ksize, 0x77) == VFS_OK);
    if (!ok6) fails++;
    kprintf("[EXT4JT] step6 overwrite crash>commit replay (0x77 applied): %s\n",
            ok6 ? "PASS" : "FAIL");

    /* 7. filesystem still fully usable after all that recovery. */
    bool ok7 = fs &&
               (e4st_create_write(fs, "/after", scratch, ksize, 0x33) == VFS_OK) &&
               (e4st_read_verify(fs, "/after", scratch, ksize, 0x33) == VFS_OK);
    if (!ok7) fails++;
    kprintf("[EXT4JT] step7 fs usable post-recovery: %s\n",
            ok7 ? "PASS" : "FAIL");

    if (fs) ext4_free(fs);
    kfree(image);
    kfree(scratch);
    kprintf("[EXT4JT] %s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : -1;
}
