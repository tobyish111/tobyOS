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

/* Concurrent open handles tracked per mount. Sized for "plenty";
 * exceeding it is reported, never silently ignored. */
#define EXT4_MAX_OPEN_INODES 64

/* ---- the block cache (finding 278) ----
 * The driver read every block from the device, every time: one create in
 * a directory of 3000 names was 56 reads, a readdir of it 3049 -- on the
 * box's USB stick, seconds each. Blocks read or written are kept here, up
 * to E4C_BYTES a mount, the least recently used going first.
 *
 * WRITE-THROUGH: a write still reaches the device before the call
 * returns, and the cache takes the copy after it has. So a crash leaves
 * exactly what it left with no cache -- the volume marked not clean while
 * mounted read-write, every change already on the device -- and nothing
 * the cache holds is newer than the device. A write that goes around the
 * driver (the disk written as a file: /dev/sdb1, or /dev/sdb) drops what
 * the cache held of it (blk_notify_write -> e4c_watch). Dropped and freed
 * at the umount. */
#define E4C_BYTES (8u << 20)

struct e4c_ent {
    uint64_t        blk;
    struct e4c_ent *hnext;              /* the hash chain */
    struct e4c_ent *prev, *next;        /* the LRU list, or the free list */
    uint8_t        *data;
};

struct e4cache {
    struct e4c_ent  *ents;              /* max of them */
    struct e4c_ent **hash;              /* hmask + 1 chains */
    struct e4c_ent  *head, *tail;       /* in use, most recent first */
    struct e4c_ent  *free;              /* not in use */
    uint32_t         max, hmask;
    uint64_t         hits, misses;
};

struct ext4 {
    struct blk_dev *dev;
    /* the disk DEV lies on and where (the cache's watch, finding 278) */
    struct blk_dev *disk;
    uint64_t        disk_off;
    struct e4cache  bc;
    uint64_t        dev_reads, dev_writes;   /* blocks, for the umount's line */

    uint32_t block_size;          /* 1024, 2048, 4096 */
    uint32_t sectors_per_block;   /* block_size / 512 */
    uint64_t total_blocks;
    uint32_t inode_size;          /* 128 or 256 */
    uint16_t want_extra_isize;    /* a new inode's i_extra_isize: 0 for 128 */
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_ino;           /* usually 11 for ext4 */
    uint32_t feature_incompat;
    uint16_t desc_size;           /* 32 or 64 */
    uint32_t first_data_block;    /* 0 for >= 2 KiB blocks, 1 for 1 KiB */
    bool     desc_64bit;          /* read the high half of group desc */

    uint32_t total_inodes;        /* s_inodes_count (write: free bookkeeping) */
    bool     feat_64bit;          /* INCOMPAT_64BIT -> 64-bit free counts */

    /* the metadata's sums (finding 271): metadata_csum's CRC-32C, or
     * uninit_bg's CRC-16 on the group descriptors alone */
    bool     csum;                /* RO_COMPAT_METADATA_CSUM */
    bool     gdt_csum;            /* RO_COMPAT_GDT_CSUM (without the above) */
    uint32_t csum_seed;           /* s_checksum_seed, or the UUID's CRC-32C */
    uint8_t  uuid[16];

    /* Features this driver reads but cannot keep when it writes (quota,
     * bigalloc, ...): such a volume mounts read-only or not at all, as
     * Linux's ext4 refuses a read-write mount of a feature it lacks. */
    uint32_t incompat_bad;
    uint32_t ro_compat_bad;
    bool     rdonly;              /* mounted read-only */
    bool     state_cleared;       /* marked not clean while mounted (no journal) */
    uint16_t state_at_mount;

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

    /* Open handles per inode, so unlink() can defer the release to the
     * last close the way POSIX requires. See oref_get() below. */
    struct ext4_openref {
        uint32_t ino;             /* 0 = free slot */
        int      refs;
        bool     orphan;          /* the last NAME went away while open */
    } openrefs[EXT4_MAX_OPEN_INODES];
};

/* Per-handle state for an open file. The inode is snapshotted at open
 * time so reads don't chase the GDT/inode table on every byte.
 *
 * The original note here said "the driver is RO so the inode contents
 * can't change underneath us" -- that stopped being true the moment the
 * driver could write, and the snapshot then went stale whenever anyone
 * else touched the file. fp_refresh() below re-reads it; this struct is
 * a cache, not a source of truth. */
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

/* ---- the block cache (finding 278; see struct e4cache) ---- */

static uint32_t e4c_slot(const struct e4cache *c, uint64_t blk) {
    return (uint32_t)((blk * 0x9E3779B97F4A7C15ull) >> 40) & c->hmask;
}

static bool e4c_init(struct ext4 *fs) {
    struct e4cache *c = &fs->bc;
    c->max = E4C_BYTES / fs->block_size;
    uint32_t h = 1;
    while (h < c->max) h <<= 1;
    c->hmask = h - 1;
    c->ents = kcalloc(c->max, sizeof(*c->ents));
    c->hash = kcalloc(h, sizeof(*c->hash));
    if (!c->ents || !c->hash) return false;
    for (uint32_t i = 0; i < c->max; i++) {
        c->ents[i].next = c->free;
        c->free = &c->ents[i];
    }
    return true;
}

static void e4c_unlink(struct e4cache *c, struct e4c_ent *e) {
    if (e->prev) e->prev->next = e->next; else c->head = e->next;
    if (e->next) e->next->prev = e->prev; else c->tail = e->prev;
    e->prev = e->next = NULL;
}

static void e4c_front(struct e4cache *c, struct e4c_ent *e) {
    e->prev = NULL;
    e->next = c->head;
    if (c->head) c->head->prev = e; else c->tail = e;
    c->head = e;
}

static void e4c_unhash(struct e4cache *c, struct e4c_ent *e) {
    struct e4c_ent **pp = &c->hash[e4c_slot(c, e->blk)];
    while (*pp && *pp != e) pp = &(*pp)->hnext;
    if (*pp) *pp = e->hnext;
    e->hnext = NULL;
}

static struct e4c_ent *e4c_find(struct ext4 *fs, uint64_t blk) {
    struct e4cache *c = &fs->bc;
    if (!c->hash) return NULL;
    for (struct e4c_ent *e = c->hash[e4c_slot(c, blk)]; e; e = e->hnext)
        if (e->blk == blk) return e;
    return NULL;
}

/* BLK now holds DATA on the device: keep a copy. Out of memory, the copy
 * is not kept -- the device has it. */
static void e4c_put(struct ext4 *fs, uint64_t blk, const void *data) {
    struct e4cache *c = &fs->bc;
    if (!c->hash) return;
    struct e4c_ent *e = e4c_find(fs, blk);
    if (e) {
        e4c_unlink(c, e);
    } else {
        if (c->free) {
            e = c->free;
            c->free = e->next;
            e->next = NULL;
        } else {
            e = c->tail;                    /* the least recently used */
            if (!e) return;
            e4c_unlink(c, e);
            e4c_unhash(c, e);
        }
        if (!e->data) e->data = kmalloc(fs->block_size);
        if (!e->data) { e->next = c->free; c->free = e; return; }
        e->blk = blk;
        uint32_t s = e4c_slot(c, blk);
        e->hnext = c->hash[s];
        c->hash[s] = e;
    }
    memcpy(e->data, data, fs->block_size);
    e4c_front(c, e);
}

/* BLK is no longer known: what the device holds is the truth */
static void e4c_drop(struct ext4 *fs, uint64_t blk) {
    struct e4cache *c = &fs->bc;
    struct e4c_ent *e = e4c_find(fs, blk);
    if (!e) return;
    e4c_unlink(c, e);
    e4c_unhash(c, e);
    e->next = c->free;
    c->free = e;
}

static void e4c_free(struct ext4 *fs) {
    struct e4cache *c = &fs->bc;
    if (c->ents)
        for (uint32_t i = 0; i < c->max; i++)
            if (c->ents[i].data) kfree(c->ents[i].data);
    if (c->ents) kfree(c->ents);
    if (c->hash) kfree(c->hash);
    memset(c, 0, sizeof(*c));
}

/* A write that went around the driver, on DISK: drop each block of the
 * volume it touched */
static void e4c_watch(void *ctx, struct blk_dev *disk, uint64_t lba, uint64_t count) {
    struct ext4 *fs = (struct ext4 *)ctx;
    if (disk != fs->disk) return;
    uint64_t lo = fs->disk_off, hi = lo + fs->total_blocks * fs->sectors_per_block;
    uint64_t a = lba > lo ? lba : lo, b = lba + count < hi ? lba + count : hi;
    if (a >= b) return;
    for (uint64_t blk = (a - lo) / fs->sectors_per_block;
         blk * fs->sectors_per_block < b - lo; blk++)
        e4c_drop(fs, blk);
}

static int read_block(struct ext4 *fs, uint64_t blk, void *buf) {
    if (blk >= fs->total_blocks) return VFS_ERR_INVAL;
    /* Read-your-writes: a block staged in the open transaction reflects
     * the not-yet-committed contents. */
    struct ext4_txn_ent *e = txn_find(fs, blk);
    if (e) { memcpy(buf, e->data, fs->block_size); return VFS_OK; }
    struct e4c_ent *ce = e4c_find(fs, blk);
    if (ce) {
        memcpy(buf, ce->data, fs->block_size);
        e4c_unlink(&fs->bc, ce);
        e4c_front(&fs->bc, ce);
        fs->bc.hits++;
        return VFS_OK;
    }
    uint64_t lba = blk * fs->sectors_per_block;
    fs->dev_reads++;
    if (blk_read(fs->dev, lba, fs->sectors_per_block, buf) != 0) {
        return VFS_ERR_IO;
    }
    fs->bc.misses++;
    e4c_put(fs, blk, buf);
    return VFS_OK;
}

/* BLK written to the device with DATA, around write_block (the journal's
 * checkpoint and replay): the cache takes the copy, or drops its own if
 * the write failed */
static int dev_write_block(struct ext4 *fs, uint64_t blk, const void *data) {
    fs->dev_writes++;
    if (blk_write(fs->dev, blk * fs->sectors_per_block, fs->sectors_per_block, data) != 0) {
        e4c_drop(fs, blk);
        return VFS_ERR_IO;
    }
    e4c_put(fs, blk, data);
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


/* ---- checksums (finding 271) ----
 * A volume with metadata_csum -- mkfs.ext4's default -- keeps a CRC-32C of
 * its superblock, of every group descriptor, of each group's two bitmaps,
 * of every inode, and of every directory, extent-tree, htree and attribute
 * block; one with uninit_bg alone keeps a CRC-16 of each group descriptor.
 * Linux will not mount a volume whose superblock sum is wrong, and e2fsck
 * reports each other one. The CRC-32C here is the bare register, seeded by
 * the caller and never inverted at the end: that is how ext4 chains a sum
 * over several pieces. The layouts are the ones Linux writes, and each sum
 * below is checked against Linux's own volumes (tools/agent/ext4host,
 * `verify`). */
static uint32_t g_crc32c_tab[256];
static uint16_t g_crc16_tab[256];
static bool     g_crc_tabs;

static void crc_tabs_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        uint16_t h = (uint16_t)i;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
            h = (uint16_t)((h >> 1) ^ (0xA001u & (0u - (h & 1u))));
        }
        g_crc32c_tab[i] = c;
        g_crc16_tab[i]  = h;
    }
    g_crc_tabs = true;
}

static uint32_t crc32c(uint32_t crc, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n--) crc = g_crc32c_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static uint16_t crc16(uint16_t crc, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n--) crc = (uint16_t)(g_crc16_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8));
    return crc;
}

static uint32_t crc32c_u32(uint32_t crc, uint32_t v) { return crc32c(crc, &v, 4); }

static const uint8_t g_zero4[4];

/* the superblock's: every byte before s_checksum */
static uint32_t sb_csum(const void *sb) {
    return crc32c(~0u, sb, __builtin_offsetof(struct ext4_super_block, s_checksum));
}

/* group G's descriptor, its bg_checksum taken as zero */
#define EXT4_BG_CSUM_OFF 0x1Eu
static uint16_t gd_csum(const struct ext4 *fs, uint32_t g, const uint8_t *desc) {
    uint32_t rest = fs->desc_size > EXT4_BG_CSUM_OFF + 2 ?
                    fs->desc_size - EXT4_BG_CSUM_OFF - 2 : 0;
    if (fs->csum) {
        uint32_t c = crc32c_u32(fs->csum_seed, g);
        c = crc32c(c, desc, EXT4_BG_CSUM_OFF);
        c = crc32c(c, g_zero4, 2);
        c = crc32c(c, desc + EXT4_BG_CSUM_OFF + 2, rest);
        return (uint16_t)c;
    }
    uint16_t h = crc16(0xFFFFu, fs->uuid, 16);
    h = crc16(h, &g, 4);
    h = crc16(h, desc, EXT4_BG_CSUM_OFF);
    return crc16(h, desc + EXT4_BG_CSUM_OFF + 2, rest);
}

/* a bitmap's: its first BITS bits (a group's blocks or inodes) */
static uint32_t bitmap_csum(const struct ext4 *fs, const uint8_t *bmp, uint32_t bits) {
    return crc32c(fs->csum_seed, bmp, bits / 8);
}

/* An inode's own seed -- its number and generation -- which the sums of
 * the blocks it owns start from too. */
static uint32_t inode_seed(const struct ext4 *fs, uint32_t ino, uint32_t gen) {
    return crc32c_u32(crc32c_u32(fs->csum_seed, ino), gen);
}

/* an inode's: the whole of its on-disk slot, i_checksum_lo and (where
 * i_extra_isize covers it) i_checksum_hi taken as zero */
#define EXT4_I_GEN_OFF     0x64u
#define EXT4_I_CSUM_LO_OFF 0x7Cu
#define EXT4_I_CSUM_HI_OFF 0x82u
static uint32_t inode_csum(const struct ext4 *fs, uint32_t ino, const uint8_t *raw) {
    uint32_t gen;
    memcpy(&gen, raw + EXT4_I_GEN_OFF, 4);
    uint32_t c = inode_seed(fs, ino, gen);
    c = crc32c(c, raw, EXT4_I_CSUM_LO_OFF);
    c = crc32c(c, g_zero4, 2);
    c = crc32c(c, raw + EXT4_I_CSUM_LO_OFF + 2,
               EXT4_GOOD_OLD_INODE_SIZE - EXT4_I_CSUM_LO_OFF - 2);
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        uint16_t extra;
        memcpy(&extra, raw + EXT4_GOOD_OLD_INODE_SIZE, 2);
        uint32_t off = EXT4_I_CSUM_HI_OFF;
        c = crc32c(c, raw + EXT4_GOOD_OLD_INODE_SIZE, off - EXT4_GOOD_OLD_INODE_SIZE);
        if (EXT4_GOOD_OLD_INODE_SIZE + (uint32_t)extra >= EXT4_I_CSUM_HI_OFF + 2) {
            c = crc32c(c, g_zero4, 2);
            off += 2;
        }
        c = crc32c(c, raw + off, fs->inode_size - off);
    }
    return c;
}

static bool inode_has_csum_hi(const struct ext4 *fs, const uint8_t *raw) {
    if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE) return false;
    uint16_t extra;
    memcpy(&extra, raw + EXT4_GOOD_OLD_INODE_SIZE, 2);
    return EXT4_GOOD_OLD_INODE_SIZE + (uint32_t)extra >= EXT4_I_CSUM_HI_OFF + 2;
}

/* A directory block ends, on a volume with sums, in a 12-byte entry of no
 * inode and no name (file type 0xDE) whose last word is the block's sum. */
#define EXT4_DIR_TAIL_LEN   12u
#define EXT4_DIR_TAIL_FT    0xDEu
static bool dir_tail_at(const struct ext4 *fs, const uint8_t *blk) {
    const struct ext4_dir_entry_2 *t =
        (const struct ext4_dir_entry_2 *)(blk + fs->block_size - EXT4_DIR_TAIL_LEN);
    return t->inode == 0 && t->rec_len == EXT4_DIR_TAIL_LEN &&
           t->name_len == 0 && t->file_type == EXT4_DIR_TAIL_FT;
}
static uint32_t dir_leaf_csum(const struct ext4 *fs, uint32_t iseed, const uint8_t *blk) {
    return crc32c(iseed, blk, fs->block_size - EXT4_DIR_TAIL_LEN);
}

/* An extent-tree block's sum is the word past its eh_max entries. */
static uint32_t extent_block_csum_off(const uint8_t *blk) {
    const struct ext4_extent_header *eh = (const struct ext4_extent_header *)blk;
    return (uint32_t)sizeof(*eh) + (uint32_t)eh->eh_max * 12u;
}
static uint32_t extent_block_csum(uint32_t iseed, const uint8_t *blk) {
    return crc32c(iseed, blk, extent_block_csum_off(blk));
}

/* An htree block: the count and limit at COUNT_OFF (0x20 in the root, 8 in
 * a node), the entries, and past LIMIT of them an 8-byte tail whose second
 * word is the sum -- of the header and the entries in use, then the tail's
 * first word and a zero for its second. */
static uint32_t dx_csum(uint32_t iseed, const uint8_t *blk, uint32_t count_off) {
    uint16_t limit, count;
    memcpy(&limit, blk + count_off, 2);
    memcpy(&count, blk + count_off + 2, 2);
    const uint8_t *tail = blk + count_off + (uint32_t)limit * 8u;
    uint32_t c = crc32c(iseed, blk, count_off + (uint32_t)count * 8u);
    c = crc32c(c, tail, 4);
    return crc32c(c, g_zero4, 4);
}

/* An extended-attribute block: its number, then the block with h_checksum
 * taken as zero. */
#define EXT4_XATTR_MAGIC    0xEA020000u
#define EXT4_XATTR_CSUM_OFF 0x10u
static uint32_t xattr_block_csum(const struct ext4 *fs, uint64_t blkno, const uint8_t *blk) {
    uint32_t c = crc32c(fs->csum_seed, &blkno, 8);
    c = crc32c(c, blk, EXT4_XATTR_CSUM_OFF);
    c = crc32c(c, g_zero4, 4);
    return crc32c(c, blk + EXT4_XATTR_CSUM_OFF + 4, fs->block_size - EXT4_XATTR_CSUM_OFF - 4);
}

/* How many of the large inode's bytes past the 128-byte core an inode has
 * and this driver keeps: its i_extra_isize, within the inode's room and
 * the fields struct ext4_inode knows. One that claims more room than the
 * inode has is taken to have none. */
static uint32_t ext4_inode_extra_len(const struct ext4 *fs,
                                     const struct ext4_inode *in) {
    if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE) return 0;
    uint32_t n = in->i_extra_isize;
    uint32_t known = (uint32_t)sizeof(*in) - EXT4_GOOD_OLD_INODE_SIZE;
    if (n > fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE) return 0;
    return n < known ? n : known;
}

/* ---- times (finding 255) ----
 * A time is the low 32 bits of its seconds in the core and, where the
 * inode has the field, an _extra word: the nanoseconds above two bits that
 * are the seconds' bits 32 and 33. Without the word the 32 bits are signed
 * (1901 to 2038); with it they run on to 2446. A time outside what the
 * inode can hold is held at the nearer end, with no nanoseconds. */
struct ext4_time { int64_t s; uint32_t ns; };

static struct ext4_time ext4_now(void) {
    extern uint64_t file_time_ns(void);     /* Linux's coarse clock */
    uint64_t n = file_time_ns();
    struct ext4_time t = { (int64_t)(n / 1000000000ull),
                           (uint32_t)(n % 1000000000ull) };
    return t;
}

/* whether the inode has FIELD, one of the 4-byte words past the core */
#define EXT4_HAS_EXTRA(in, field)                                         \
    ((in)->i_extra_isize >= __builtin_offsetof(struct ext4_inode, field) + 4u \
                            - EXT4_GOOD_OLD_INODE_SIZE)

static void ext4_time_put(uint32_t *lo, uint32_t *extra, bool has,
                          struct ext4_time t) {
    int64_t min = INT32_MIN;
    int64_t max = has ? (int64_t)INT32_MAX + (3ll << 32) : INT32_MAX;
    if (t.s <= min) { t.s = min; t.ns = 0; }
    if (t.s >= max) { t.s = max; t.ns = 0; }
    if (t.ns >= 1000000000u) t.ns = 0;
    *lo = (uint32_t)t.s;
    if (has)
        *extra = (t.ns << 2) |
                 ((uint32_t)((t.s - (int64_t)(int32_t)(uint32_t)t.s) >> 32) & 3u);
}

static struct ext4_time ext4_time_get(uint32_t lo, uint32_t extra, bool has) {
    struct ext4_time t = { (int32_t)lo, 0 };
    if (has) {
        t.s += (int64_t)(extra & 3u) << 32;
        t.ns = extra >> 2;
        if (t.ns >= 1000000000u) t.ns = 0;
    }
    return t;
}

enum { E4_ATIME = 1, E4_MTIME = 2, E4_CTIME = 4, E4_CRTIME = 8 };

static void ext4_stamp(struct ext4_inode *in, int which, struct ext4_time t) {
    if (which & E4_ATIME)
        ext4_time_put(&in->i_atime, &in->i_atime_extra, EXT4_HAS_EXTRA(in, i_atime_extra), t);
    if (which & E4_MTIME)
        ext4_time_put(&in->i_mtime, &in->i_mtime_extra, EXT4_HAS_EXTRA(in, i_mtime_extra), t);
    if (which & E4_CTIME)
        ext4_time_put(&in->i_ctime, &in->i_ctime_extra, EXT4_HAS_EXTRA(in, i_ctime_extra), t);
    if ((which & E4_CRTIME) && EXT4_HAS_EXTRA(in, i_crtime))
        ext4_time_put(&in->i_crtime, &in->i_crtime_extra,
                      EXT4_HAS_EXTRA(in, i_crtime_extra), t);
}

static int read_inode(struct ext4 *fs, uint32_t ino, struct ext4_inode *out) {
    if (ino == 0) return VFS_ERR_INVAL;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->group_count) return VFS_ERR_INVAL;

    uint64_t itab = group_inode_table(fs, group);
    uint64_t byte_off_in_group = (uint64_t)idx * fs->inode_size;
    uint64_t blk = itab + byte_off_in_group / fs->block_size;
    uint32_t off = (uint32_t)(byte_off_in_group % fs->block_size);

    /* The on-disk inode is `inode_size` bytes: the core, then the fields
     * its i_extra_isize covers (ext4_inode_extra_len), then attributes,
     * which stay on the disk. A field it does not cover reads as 0. */
    memset(out, 0, sizeof(*out));
    uint32_t n = fs->inode_size < sizeof(*out) ? fs->inode_size : (uint32_t)sizeof(*out);
    int rc = read_block_partial(fs, blk, off, n, out);
    if (rc != VFS_OK) return rc;
    uint32_t keep = ext4_inode_extra_len(fs, out);
    memset((uint8_t *)out + EXT4_GOOD_OLD_INODE_SIZE + keep, 0,
           sizeof(*out) - EXT4_GOOD_OLD_INODE_SIZE - keep);
    return VFS_OK;
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
    return dev_write_block(fs, blk, buf);       /* through, then the cache's */
}

/* Write back the cached 1024-byte superblock (free counts, state). The
 * SB always lives at byte offset 1024 = LBA 2 (512-byte sectors).
 *
 * Inside a transaction we can't do that sub-block write directly: the SB
 * shares an fs block with the boot area (and, for 1 KiB blocks, sits at a
 * block boundary). So we read-modify-write the whole containing block and
 * stage *that*, keeping the journal at fs-block granularity. */
static void sb_csum_set(struct ext4 *fs) {
    struct ext4_super_block *sb = (struct ext4_super_block *)fs->sb_buf;
    if (fs->csum) sb->s_checksum = sb_csum(sb);
}

static int flush_superblock(struct ext4 *fs) {
    if (!fs->sb_buf) return VFS_OK;
    sb_csum_set(fs);
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
    e4c_drop(fs, 1024u / fs->block_size);    /* the block it sits in */
    if (blk_write(fs->dev, 2, 2, fs->sb_buf) != 0) return VFS_ERR_IO;
    return VFS_OK;
}

/* Write the cached SB straight to disk, bypassing the transaction. Used
 * by the journal machinery itself (marking the log dirty/clean) where the
 * SB change must NOT be part of the staged set. */
static int flush_superblock_direct(struct ext4 *fs) {
    if (!fs->sb_buf) return VFS_OK;
    sb_csum_set(fs);
    e4c_drop(fs, 1024u / fs->block_size);
    if (blk_write(fs->dev, 2, 2, fs->sb_buf) != 0) return VFS_ERR_IO;
    return VFS_OK;
}

/* Group G's descriptor sum, in the cached GDT */
static void gd_csum_set(struct ext4 *fs, uint32_t g) {
    if (!fs->csum && !fs->gdt_csum) return;
    uint8_t *d = fs->gdt_buf + (uint64_t)g * fs->desc_size;
    uint16_t c = gd_csum(fs, g, d);
    memcpy(d + EXT4_BG_CSUM_OFF, &c, 2);
}

/* Write back the GDT block that holds group G's descriptor, its sum made
 * first. (The whole table was written after every allocation: on a large
 * volume that is a hundred blocks for each one allocated.) */
static int flush_group(struct ext4 *fs, uint32_t g) {
    gd_csum_set(fs, g);
    uint32_t i = (uint32_t)(((uint64_t)g * fs->desc_size) / fs->block_size);
    return write_block(fs, fs->first_data_block + 1 + i,
                       fs->gdt_buf + (uint64_t)i * fs->block_size);
}

/* Write inode `ino` back: read-modify-write its slot -- the core and the
 * extra fields its i_extra_isize covers; what lies past them (in-inode
 * attributes) is preserved. FRESH is a new inode: its whole slot is
 * zeroed first, so nothing of the slot's last tenant (its attributes) is
 * taken to be the new file's. */
static int write_inode_slot(struct ext4 *fs, uint32_t ino,
                            const struct ext4_inode *in, bool fresh) {
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
    uint8_t *raw = fs->blk_buf + off;
    if (fresh) memset(raw, 0, fs->inode_size);
    memcpy(raw, in, EXT4_GOOD_OLD_INODE_SIZE + ext4_inode_extra_len(fs, in));
    if (fs->csum) {                 /* the whole slot's sum (finding 271) */
        uint32_t c = inode_csum(fs, ino, raw);
        uint16_t lo = (uint16_t)c, hi = (uint16_t)(c >> 16);
        memcpy(raw + EXT4_I_CSUM_LO_OFF, &lo, 2);
        if (inode_has_csum_hi(fs, raw)) memcpy(raw + EXT4_I_CSUM_HI_OFF, &hi, 2);
    }
    return write_block(fs, blk, fs->blk_buf);
}

static int write_inode(struct ext4 *fs, uint32_t ino,
                       const struct ext4_inode *in) {
    return write_inode_slot(fs, ino, in, false);
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

/* ---- block + inode bitmaps (finding 271) ----
 * On a volume with sums or uninit_bg, mkfs leaves most groups flagged as
 * never having had a bitmap written: BLOCK_UNINIT, INODE_UNINIT. What lies
 * on the disk there is not the bitmap. Linux takes such a block bitmap to
 * be the group's own metadata -- a superblock backup, the GDT and its
 * reserved blocks after it, those of the group's bitmaps and inode table
 * that lie in it -- and such an inode bitmap to be clear; so does this
 * driver, and the first allocation in a group writes its bitmap out and
 * clears the flag, as Linux's does. Past a group's last block or inode a
 * bitmap's bits are set. A bitmap written carries its sum in the group's
 * descriptor. */

static bool fs_uninit_bg(const struct ext4 *fs) { return fs->csum || fs->gdt_csum; }

static struct ext4_group_desc_64 *gd64(struct ext4 *fs, uint32_t g) {
    return (struct ext4_group_desc_64 *)(fs->gdt_buf + (uint64_t)g * fs->desc_size);
}

static uint16_t group_flags(struct ext4 *fs, uint32_t g) {
    return fs_uninit_bg(fs) ? gd64(fs, g)->bg_flags : 0;
}

static uint32_t group_free_blocks(struct ext4 *fs, uint32_t g) {
    struct ext4_group_desc_64 *d = gd64(fs, g);
    return d->bg_free_blocks_count_lo |
           (fs->desc_size >= 64 ? (uint32_t)d->bg_free_blocks_count_hi << 16 : 0);
}

static uint32_t group_free_inodes(struct ext4 *fs, uint32_t g) {
    struct ext4_group_desc_64 *d = gd64(fs, g);
    return d->bg_free_inodes_count_lo |
           (fs->desc_size >= 64 ? (uint32_t)d->bg_free_inodes_count_hi << 16 : 0);
}

static uint32_t group_itable_unused(struct ext4 *fs, uint32_t g) {
    struct ext4_group_desc_64 *d = gd64(fs, g);
    return d->bg_itable_unused_lo |
           (fs->desc_size >= 64 ? (uint32_t)d->bg_itable_unused_hi << 16 : 0);
}

static void group_itable_unused_set(struct ext4 *fs, uint32_t g, uint32_t v) {
    struct ext4_group_desc_64 *d = gd64(fs, g);
    d->bg_itable_unused_lo = (uint16_t)v;
    if (fs->desc_size >= 64) d->bg_itable_unused_hi = (uint16_t)(v >> 16);
}

static bool is_power_of(uint32_t n, uint32_t b) {
    while (n > 1 && n % b == 0) n /= b;
    return n == 1;
}

/* whether group G holds a copy of the superblock and the GDT */
static bool group_has_super(const struct ext4 *fs, uint32_t g) {
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    if (g == 0) return true;
    if (sb->s_feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2)
        return g == sb->s_backup_bgs[0] || g == sb->s_backup_bgs[1];
    if (!(sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER)) return true;
    return g == 1 || is_power_of(g, 3) || is_power_of(g, 5) || is_power_of(g, 7);
}

static uint64_t group_first_block(const struct ext4 *fs, uint32_t g) {
    return (uint64_t)g * fs->blocks_per_group + fs->first_data_block;
}

static uint32_t group_block_count(const struct ext4 *fs, uint32_t g) {
    uint64_t n = fs->total_blocks - group_first_block(fs, g);
    return n < fs->blocks_per_group ? (uint32_t)n : fs->blocks_per_group;
}

static void bit_set(uint8_t *bmp, uint32_t bit) { bmp[bit / 8] |= (uint8_t)(1u << (bit % 8)); }
static void bit_clear(uint8_t *bmp, uint32_t bit) { bmp[bit / 8] &= (uint8_t)~(1u << (bit % 8)); }
static bool bit_test(const uint8_t *bmp, uint32_t bit) { return (bmp[bit / 8] >> (bit % 8)) & 1u; }

/* the bitmap a group flagged BLOCK_UNINIT stands for */
static void block_bitmap_synth(struct ext4 *fs, uint32_t g, uint8_t *bmp) {
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    memset(bmp, 0, fs->block_size);
    uint64_t first = group_first_block(fs, g);
    uint32_t count = group_block_count(fs, g);
    if (group_has_super(fs, g)) {
        uint32_t n = 1 + (fs->gdt_bytes + fs->block_size - 1) / fs->block_size;
        if (sb->s_feature_compat & EXT4_FEATURE_COMPAT_RESIZE_INODE)
            n += sb->s_reserved_gdt_blocks;
        for (uint32_t b = 0; b < n && b < count; b++) bit_set(bmp, b);
    }
    uint32_t itab = (uint32_t)(((uint64_t)fs->inodes_per_group * fs->inode_size +
                                fs->block_size - 1) / fs->block_size);
    const uint64_t at[3]  = { group_block_bitmap(fs, g), group_inode_bitmap(fs, g),
                              group_inode_table(fs, g) };
    const uint32_t len[3] = { 1, 1, itab };
    for (int k = 0; k < 3; k++)
        for (uint32_t j = 0; j < len[k]; j++)
            if (at[k] + j >= first && at[k] + j < first + count)
                bit_set(bmp, (uint32_t)(at[k] + j - first));
    for (uint32_t b = count; b < fs->block_size * 8; b++) bit_set(bmp, b);
}

static int read_block_bitmap(struct ext4 *fs, uint32_t g, uint8_t *bmp) {
    if (group_flags(fs, g) & EXT4_BG_BLOCK_UNINIT) {
        block_bitmap_synth(fs, g, bmp);
        return VFS_OK;
    }
    uint64_t b = group_block_bitmap(fs, g);
    return b ? read_block(fs, b, bmp) : VFS_ERR_IO;
}

static int read_inode_bitmap(struct ext4 *fs, uint32_t g, uint8_t *bmp) {
    if (group_flags(fs, g) & EXT4_BG_INODE_UNINIT) {
        memset(bmp, 0, fs->block_size);
        for (uint32_t b = fs->inodes_per_group; b < fs->block_size * 8; b++) bit_set(bmp, b);
        return VFS_OK;
    }
    uint64_t b = group_inode_bitmap(fs, g);
    return b ? read_block(fs, b, bmp) : VFS_ERR_IO;
}

/* Write group G's block bitmap back with its sum; the group is no longer
 * uninitialised. The caller flushes the group. */
static int write_block_bitmap(struct ext4 *fs, uint32_t g, const uint8_t *bmp) {
    struct ext4_group_desc_64 *d = gd64(fs, g);
    if (fs_uninit_bg(fs)) d->bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
    if (fs->csum) {
        uint32_t c = bitmap_csum(fs, bmp, fs->blocks_per_group);
        d->bg_block_bitmap_csum_lo = (uint16_t)c;
        if (fs->desc_size >= 64) d->bg_block_bitmap_csum_hi = (uint16_t)(c >> 16);
    }
    return write_block(fs, group_block_bitmap(fs, g), bmp);
}

static int write_inode_bitmap(struct ext4 *fs, uint32_t g, const uint8_t *bmp) {
    struct ext4_group_desc_64 *d = gd64(fs, g);
    if (fs_uninit_bg(fs)) d->bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
    if (fs->csum) {
        uint32_t c = bitmap_csum(fs, bmp, fs->inodes_per_group);
        d->bg_inode_bitmap_csum_lo = (uint16_t)c;
        if (fs->desc_size >= 64) d->bg_inode_bitmap_csum_hi = (uint16_t)(c >> 16);
    }
    return write_block(fs, group_inode_bitmap(fs, g), bmp);
}

/* block BIT of group G, whose bitmap BMP holds it free, taken */
static int take_block(struct ext4 *fs, uint32_t g, uint8_t *bmp, uint32_t bit,
                      uint32_t *out_blk) {
    bit_set(bmp, bit);
    int rc = write_block_bitmap(fs, g, bmp);
    if (rc != VFS_OK) return rc;
    adj_free_blocks(fs, g, -1);
    flush_group(fs, g);
    flush_superblock(fs);
    *out_blk = (uint32_t)(group_first_block(fs, g) + bit);
    return VFS_OK;
}

/* Allocate one block near GOAL: the goal itself if it is free (so a file
 * written in order stays one run, as Linux keeps it after its last block);
 * else the first block of the first run of RUN free blocks from there on --
 * what is still to be written fits one run, as Linux's allocator looks for
 * room the size of the request (finding 271: a 3000-block file put into a
 * Linux volume's one-block holes came out in 2520 extents); else the first
 * free block. In the goal's group, then the groups after it, around to the
 * goal again. Sets the bit, writes the bitmap, and takes the block from the
 * group's and the superblock's free counts. */
static int ext4_alloc_block_near(struct ext4 *fs, uint64_t goal, uint32_t run,
                                 uint32_t *out_blk) {
    if (goal < fs->first_data_block || goal >= fs->total_blocks)
        goal = fs->first_data_block;
    if (run == 0) run = 1;
    uint32_t g0 = (uint32_t)((goal - fs->first_data_block) / fs->blocks_per_group);
    uint32_t b0 = (uint32_t)((goal - fs->first_data_block) % fs->blocks_per_group);
    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;
    int rc;

    if (group_free_blocks(fs, g0) && read_block_bitmap(fs, g0, bmp) == VFS_OK &&
        b0 < group_block_count(fs, g0) && !bit_test(bmp, b0)) {
        rc = take_block(fs, g0, bmp, b0, out_blk);
        kfree(bmp);
        return rc;
    }
    for (uint32_t pass = run > 1 ? 0 : 1; pass < 2; pass++) {
        uint32_t want = pass == 0 ? run : 1;
        for (uint32_t gi = 0; gi <= fs->group_count; gi++) {
            uint32_t g = (g0 + gi) % fs->group_count;
            if (group_free_blocks(fs, g) < want) continue;
            if (read_block_bitmap(fs, g, bmp) != VFS_OK) continue;
            uint32_t from = gi == 0 ? b0 : 0;
            uint32_t to   = gi == fs->group_count ? b0 : group_block_count(fs, g);
            uint32_t len = 0;
            for (uint32_t bit = from; bit < to; bit++) {
                len = bit_test(bmp, bit) ? 0 : len + 1;
                if (len < want) continue;
                rc = take_block(fs, g, bmp, bit + 1 - want, out_blk);
                kfree(bmp);
                return rc;
            }
        }
    }
    kfree(bmp);
    return VFS_ERR_NOSPC;
}

static int ext4_alloc_block(struct ext4 *fs, uint32_t pref_group, uint32_t *out_blk) {
    return ext4_alloc_block_near(fs, group_first_block(fs, pref_group % fs->group_count),
                                 1, out_blk);
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
    int rc = read_block_bitmap(fs, g, bmp);
    if (rc != VFS_OK) { kfree(bmp); return rc; }
    bit_clear(bmp, bit);
    rc = write_block_bitmap(fs, g, bmp);
    kfree(bmp);
    if (rc != VFS_OK) return rc;
    adj_free_blocks(fs, g, +1);
    flush_group(fs, g);
    return flush_superblock(fs);
}

static int ext4_alloc_inode(struct ext4 *fs, uint32_t pref_group,
                            uint32_t *out_ino) {
    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;

    for (uint32_t gi = 0; gi < fs->group_count; gi++) {
        uint32_t g = (pref_group + gi) % fs->group_count;
        if (group_free_inodes(fs, g) == 0) continue;
        if (read_inode_bitmap(fs, g, bmp) != VFS_OK) continue;

        for (uint32_t bit = 0; bit < fs->inodes_per_group; bit++) {
            if (bit_test(bmp, bit)) continue;
            uint32_t ino = g * fs->inodes_per_group + bit + 1;
            if (ino < fs->first_ino && ino != EXT4_ROOT_INODE) continue;

            bit_set(bmp, bit);
            int rc;
            /* a group's first inode brings its block bitmap out of
             * BLOCK_UNINIT too, as Linux's allocator does */
            if (group_flags(fs, g) & EXT4_BG_BLOCK_UNINIT) {
                uint8_t *bb = kmalloc(fs->block_size);
                if (!bb) { kfree(bmp); return VFS_ERR_NOMEM; }
                block_bitmap_synth(fs, g, bb);
                rc = write_block_bitmap(fs, g, bb);
                kfree(bb);
                if (rc != VFS_OK) { kfree(bmp); return rc; }
            }
            rc = write_inode_bitmap(fs, g, bmp);
            if (rc != VFS_OK) { kfree(bmp); return rc; }
            /* the inode table's unused tail no longer reaches this one */
            if (fs_uninit_bg(fs) &&
                bit >= fs->inodes_per_group - group_itable_unused(fs, g))
                group_itable_unused_set(fs, g, fs->inodes_per_group - bit - 1);
            adj_free_inodes(fs, g, -1);
            flush_group(fs, g);
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
    int rc = read_inode_bitmap(fs, g, bmp);
    if (rc != VFS_OK) { kfree(bmp); return rc; }
    bit_clear(bmp, bit);
    rc = write_inode_bitmap(fs, g, bmp);
    kfree(bmp);
    if (rc != VFS_OK) return rc;
    adj_free_inodes(fs, g, +1);
    flush_group(fs, g);
    return flush_superblock(fs);
}

/* ---- block resolution: extents OR legacy pointers ----
 *
 * Finding 271: this was a depth-0 tree of at most four extents for writing
 * and freeing -- a file on a Linux volume whose tree has index blocks could
 * be read but not grown, and deleting it freed none of its blocks; and an
 * unwritten (preallocated) extent was read as whatever its blocks held, and
 * written into while staying unwritten, so Linux read zeros there. Now a
 * tree of any depth is found, grown, split and trimmed as Linux's is: a
 * leaf or index node that fills is split, a root that fills moves down a
 * level, every block's sum is kept, and each index's key is its child's
 * first. Unwritten blocks read as zeros, and writing one makes it written. */

static uint32_t extent_len_of(const struct ext4_extent *e) {
    uint16_t l = e->ee_len;
    return (l > 32768) ? (uint32_t)(l - 32768) : (uint32_t)l;
}
static void extent_len_set(struct ext4_extent *e, uint32_t len) {
    bool uninit = e->ee_len > 32768;
    e->ee_len = (uint16_t)(uninit ? len + 32768 : len);
}
static bool extent_unwritten(const struct ext4_extent *e) { return e->ee_len > 32768; }
static uint64_t extent_start(const struct ext4_extent *e) {
    return ((uint64_t)e->ee_start_hi << 32) | e->ee_start_lo;
}
static void extent_set(struct ext4_extent *e, uint32_t lblk, uint32_t len,
                       uint64_t phys, bool unwritten) {
    e->ee_block    = lblk;
    e->ee_len      = (uint16_t)(unwritten ? len + 32768 : len);
    e->ee_start_lo = (uint32_t)phys;
    e->ee_start_hi = (uint16_t)(phys >> 32);
}
static uint64_t idx_child(const struct ext4_extent_idx *ix) {
    return ((uint64_t)ix->ei_leaf_hi << 32) | ix->ei_leaf_lo;
}

#define EXT_MAX_LEVELS 6

/* From the inode's root down to a leaf: each level's node (the inode's
 * i_block, then blocks), its block number (0 for the root), and the entry
 * followed -- in a leaf the last extent at or below the block looked for,
 * or -1. */
struct ext_level {
    uint8_t *node;
    uint64_t blk;
    int      at;
};
struct ext_path {
    int depth;
    struct ext_level lv[EXT_MAX_LEVELS];
    uint8_t *bufs;
};

static struct ext4_extent_header *ext_hdr(const struct ext_level *l) {
    return (struct ext4_extent_header *)l->node;
}

static void ext_path_free(struct ext_path *p) {
    if (p->bufs) kfree(p->bufs);
    p->bufs = NULL;
}

static int ext_find(struct ext4 *fs, struct ext4_inode *in, uint32_t lblk,
                    struct ext_path *p) {
    struct ext4_extent_header *root = (struct ext4_extent_header *)in->i_block;
    p->bufs = NULL;
    if (root->eh_magic != EXT4_EXT_MAGIC || root->eh_depth >= EXT_MAX_LEVELS)
        return VFS_ERR_IO;
    p->depth = root->eh_depth;
    if (p->depth) {
        p->bufs = kmalloc((size_t)fs->block_size * p->depth);
        if (!p->bufs) return VFS_ERR_NOMEM;
    }
    p->lv[0].node = (uint8_t *)in->i_block;
    p->lv[0].blk  = 0;
    for (int k = 0; ; k++) {
        struct ext4_extent_header *h = ext_hdr(&p->lv[k]);
        if (h->eh_magic != EXT4_EXT_MAGIC || h->eh_entries > h->eh_max ||
            h->eh_depth != p->depth - k) {
            ext_path_free(p);
            return VFS_ERR_IO;
        }
        if (k == p->depth) {
            const struct ext4_extent *e = (const struct ext4_extent *)(h + 1);
            int at = -1;
            for (int i = 0; i < h->eh_entries && e[i].ee_block <= lblk; i++) at = i;
            p->lv[k].at = at;
            return VFS_OK;
        }
        const struct ext4_extent_idx *ix = (const struct ext4_extent_idx *)(h + 1);
        if (h->eh_entries == 0) { ext_path_free(p); return VFS_ERR_IO; }
        int at = 0;
        for (int i = 1; i < h->eh_entries && ix[i].ei_block <= lblk; i++) at = i;
        p->lv[k].at = at;
        p->lv[k + 1].node = p->bufs + (size_t)k * fs->block_size;
        p->lv[k + 1].blk  = idx_child(&ix[at]);
        int rc = read_block(fs, p->lv[k + 1].blk, p->lv[k + 1].node);
        if (rc != VFS_OK) { ext_path_free(p); return rc; }
    }
}

/* A node written back: a block with its sum, or the root, which goes to
 * the disk with the inode (the caller writes that). */
static int ext_level_write(struct ext4 *fs, uint32_t ino, const struct ext4_inode *in,
                           struct ext_level *l) {
    if (l->blk == 0) return VFS_OK;
    if (fs->csum) {
        uint32_t c = extent_block_csum(inode_seed(fs, ino, in->i_generation), l->node);
        memcpy(l->node + extent_block_csum_off(l->node), &c, 4);
    }
    return write_block(fs, l->blk, l->node);
}

static uint32_t ext_first_key(const struct ext_level *l) {
    const struct ext4_extent_header *h = ext_hdr(l);
    return h->eh_depth ? ((const struct ext4_extent_idx *)(h + 1))[0].ei_block
                       : ((const struct ext4_extent *)(h + 1))[0].ee_block;
}

/* Level K's node has a new first key: the index entries above it that
 * lead there take it, up while each is its node's first. */
static int ext_fix_keys(struct ext4 *fs, uint32_t ino, const struct ext4_inode *in,
                        struct ext_path *p, int k) {
    for (; k > 0; k--) {
        if (ext_hdr(&p->lv[k])->eh_entries == 0) return VFS_OK;
        uint32_t first = ext_first_key(&p->lv[k]);
        struct ext_level *pl = &p->lv[k - 1];
        struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(ext_hdr(pl) + 1);
        if (ix[pl->at].ei_block == first) return VFS_OK;
        ix[pl->at].ei_block = first;
        int rc = ext_level_write(fs, ino, in, pl);
        if (rc != VFS_OK) return rc;
        if (pl->at != 0) return VFS_OK;
    }
    return VFS_OK;
}

static void inode_blocks_add(const struct ext4 *fs, struct ext4_inode *in, int64_t n) {
    uint64_t units = (in->i_flags & EXT4_HUGE_FILE_FL) ? 1 : fs->block_size / 512;
    uint64_t v = in->i_blocks_lo | ((uint64_t)osd2_half(in, 0) << 32);
    v = (uint64_t)((int64_t)v + n * (int64_t)units);
    in->i_blocks_lo = (uint32_t)v;
    osd2_set_half(in, 0, (uint32_t)(v >> 32));
}

/* Where a file's next block should be: after the extent nearest LBLK, as
 * Linux's goal is; else the start of the inode's flex group (a regular
 * file's: the group after it), as Linux's. */
static uint64_t ext_goal(struct ext4 *fs, uint32_t ino, const struct ext4_inode *in,
                         const struct ext_path *p, uint32_t lblk) {
    if (p) {
        const struct ext_level *l = &p->lv[p->depth];
        const struct ext4_extent *e = (const struct ext4_extent *)(ext_hdr(l) + 1);
        if (l->at >= 0) {
            const struct ext4_extent *x = &e[l->at];
            if (lblk >= x->ee_block) return extent_start(x) + (lblk - x->ee_block);
            uint64_t back = x->ee_block - lblk;
            return extent_start(x) > back ? extent_start(x) - back : extent_start(x);
        }
        if (l->blk) return l->blk;
    }
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    uint32_t g = (ino - 1) / fs->inodes_per_group;
    if ((fs->feature_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) &&
        sb->s_log_groups_per_flex >= 2 && sb->s_log_groups_per_flex < 31) {
        g &= ~((1u << sb->s_log_groups_per_flex) - 1u);
        if ((in->i_mode & EXT4_S_IFMT) == EXT4_S_IFREG && g + 1 < fs->group_count) g++;
    }
    return group_first_block(fs, g);
}

/* a new node block for the tree, near the file */
static int ext_new_block(struct ext4 *fs, uint32_t ino, struct ext4_inode *in,
                         const struct ext_path *p, uint32_t lblk, uint64_t *out) {
    uint32_t b;
    int rc = ext4_alloc_block_near(fs, ext_goal(fs, ino, in, p, lblk), 1, &b);
    if (rc != VFS_OK) return rc;
    inode_blocks_add(fs, in, 1);
    *out = b;
    return VFS_OK;
}

/* The full node at level K gets room: the root moves down a level into a
 * new block (a node that is then split in turn), or a block is split in
 * two -- its upper half, or, when the file is growing past its end,
 * nothing, into a new block indexed after it -- once its parent has room
 * for that index. The caller looks its path up again. */
static int ext_make_room(struct ext4 *fs, uint32_t ino, struct ext4_inode *in,
                         struct ext_path *p, int k, uint32_t lblk) {
    struct ext4_extent_header *h = ext_hdr(&p->lv[k]);
    if (k == 0) {
        if (p->depth + 1 >= EXT_MAX_LEVELS) return VFS_ERR_NOSPC;
        uint64_t nb;
        int rc = ext_new_block(fs, ino, in, p, lblk, &nb);
        if (rc != VFS_OK) return rc;
        uint8_t *buf = kmalloc(fs->block_size);
        if (!buf) return VFS_ERR_NOMEM;
        memset(buf, 0, fs->block_size);
        struct ext4_extent_header *nh = (struct ext4_extent_header *)buf;
        *nh = *h;
        nh->eh_max = (uint16_t)((fs->block_size - sizeof(*nh)) / 12);
        memcpy(nh + 1, h + 1, (size_t)h->eh_entries * 12);
        struct ext_level nl = { buf, nb, 0 };
        uint32_t first = ext_first_key(&nl);
        rc = ext_level_write(fs, ino, in, &nl);
        kfree(buf);
        if (rc != VFS_OK) return rc;
        struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(h + 1);
        h->eh_depth++;
        h->eh_entries = 1;
        memset(ix, 0, sizeof(in->i_block) - sizeof(*h));
        ix[0].ei_block   = first;
        ix[0].ei_leaf_lo = (uint32_t)nb;
        ix[0].ei_leaf_hi = (uint16_t)(nb >> 32);
        return VFS_OK;
    }
    struct ext4_extent_header *ph = ext_hdr(&p->lv[k - 1]);
    if (ph->eh_entries >= ph->eh_max) return ext_make_room(fs, ino, in, p, k - 1, lblk);

    /* a leaf splits where the extent goes in, as Linux's: what is after
     * that point moves (nothing, when the file grows past its end); an
     * index node, in half */
    uint32_t n = h->eh_entries, split = n / 2;
    if (k == p->depth && p->lv[k].at >= 0) split = (uint32_t)(p->lv[k].at + 1);
    uint64_t nb;
    int rc = ext_new_block(fs, ino, in, p, lblk, &nb);
    if (rc != VFS_OK) return rc;
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return VFS_ERR_NOMEM;
    memset(buf, 0, fs->block_size);
    struct ext4_extent_header *nh = (struct ext4_extent_header *)buf;
    nh->eh_magic   = EXT4_EXT_MAGIC;
    nh->eh_max     = (uint16_t)((fs->block_size - sizeof(*nh)) / 12);
    nh->eh_depth   = h->eh_depth;
    nh->eh_entries = (uint16_t)(n - split);
    memcpy(nh + 1, (uint8_t *)(h + 1) + (size_t)split * 12, (size_t)(n - split) * 12);
    h->eh_entries = (uint16_t)split;
    struct ext_level nl = { buf, nb, 0 };
    uint32_t key = n - split ? ext_first_key(&nl) : lblk;
    rc = ext_level_write(fs, ino, in, &nl);
    if (rc == VFS_OK) rc = ext_level_write(fs, ino, in, &p->lv[k]);
    kfree(buf);
    if (rc != VFS_OK) return rc;
    /* its index, after the parent's entry for the node split */
    struct ext_level *pl = &p->lv[k - 1];
    struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(ph + 1);
    int pos = pl->at + 1;
    memmove(&ix[pos + 1], &ix[pos], (size_t)(ph->eh_entries - pos) * 12);
    memset(&ix[pos], 0, sizeof(ix[pos]));
    ix[pos].ei_block   = key;
    ix[pos].ei_leaf_lo = (uint32_t)nb;
    ix[pos].ei_leaf_hi = (uint16_t)(nb >> 32);
    ph->eh_entries++;
    return ext_level_write(fs, ino, in, pl);
}

/* two extents, A then B, that make one: both written, one run on the disk
 * as in the file, no longer than an extent may be */
static bool ext_mergeable(const struct ext4_extent *a, const struct ext4_extent *b) {
    return !extent_unwritten(a) && !extent_unwritten(b) &&
           a->ee_block + extent_len_of(a) == b->ee_block &&
           extent_start(a) + extent_len_of(a) == extent_start(b) &&
           extent_len_of(a) + extent_len_of(b) <= 32768;
}

/* A tree of one leaf under a root of one index, whose extents would fit in
 * the root: they move up into it and the leaf block is freed -- what
 * Linux's ext4 does after it puts an extent in. */
static int ext_try_merge_up(struct ext4 *fs, struct ext4_inode *in) {
    struct ext4_extent_header *h = (struct ext4_extent_header *)in->i_block;
    if (h->eh_depth != 1 || h->eh_entries != 1) return VFS_OK;
    uint16_t root_max = (uint16_t)((sizeof(in->i_block) - sizeof(*h)) / 12);
    uint64_t leaf = idx_child((struct ext4_extent_idx *)(h + 1));
    uint8_t *b = kmalloc(fs->block_size);
    if (!b) return VFS_ERR_NOMEM;
    int rc = read_block(fs, leaf, b);
    const struct ext4_extent_header *lh = (const struct ext4_extent_header *)b;
    if (rc == VFS_OK && lh->eh_magic == EXT4_EXT_MAGIC && lh->eh_depth == 0 &&
        lh->eh_entries <= root_max) {
        memset(h + 1, 0, sizeof(in->i_block) - sizeof(*h));
        memcpy(h + 1, lh + 1, (size_t)lh->eh_entries * 12);
        h->eh_entries = lh->eh_entries;
        h->eh_depth   = 0;
        h->eh_max     = root_max;
        ext4_free_block(fs, (uint32_t)leaf);
        inode_blocks_add(fs, in, -1);
    }
    kfree(b);
    return rc;
}

/* Extent AT of leaf L and those after it in the leaf, while each continues
 * the one before: made one, as Linux's merge to the right does -- a hole
 * filled between two extents leaves one, not two that touch. */
static void ext_merge_right(struct ext_level *l, int at) {
    struct ext4_extent_header *h = ext_hdr(l);
    struct ext4_extent *e = (struct ext4_extent *)(h + 1);
    while (at >= 0 && at + 1 < h->eh_entries && ext_mergeable(&e[at], &e[at + 1])) {
        extent_len_set(&e[at], extent_len_of(&e[at]) + extent_len_of(&e[at + 1]));
        memmove(&e[at + 1], &e[at + 2], (size_t)(h->eh_entries - at - 2) * 12);
        h->eh_entries--;
        memset(&e[h->eh_entries], 0, sizeof(e[0]));
    }
}

/* Put extent X, which overlaps none, into the tree: grown onto its
 * neighbour where it continues one (and that onto the next, where the
 * two then touch), else in its place. The caller writes the inode. */
static int ext_insert(struct ext4 *fs, uint32_t ino, struct ext4_inode *in,
                      const struct ext4_extent *x) {
    for (int tries = 0; tries < 2 * EXT_MAX_LEVELS + 2; tries++) {
        struct ext_path p;
        int rc = ext_find(fs, in, x->ee_block, &p);
        if (rc != VFS_OK) return rc;
        struct ext_level *l = &p.lv[p.depth];
        struct ext4_extent_header *h = ext_hdr(l);
        struct ext4_extent *e = (struct ext4_extent *)(h + 1);
        int at = l->at;
        if (at >= 0 && ext_mergeable(&e[at], x)) {
            extent_len_set(&e[at], extent_len_of(&e[at]) + extent_len_of(x));
            ext_merge_right(l, at);
            rc = ext_level_write(fs, ino, in, l);
        } else if (at + 1 < h->eh_entries && ext_mergeable(x, &e[at + 1])) {
            uint32_t len = extent_len_of(x) + extent_len_of(&e[at + 1]);
            extent_set(&e[at + 1], x->ee_block, len, extent_start(x), false);
            rc = ext_level_write(fs, ino, in, l);
            if (rc == VFS_OK && at + 1 == 0) rc = ext_fix_keys(fs, ino, in, &p, p.depth);
        } else if (h->eh_entries < h->eh_max) {
            memmove(&e[at + 2], &e[at + 1], (size_t)(h->eh_entries - at - 1) * 12);
            e[at + 1] = *x;
            h->eh_entries++;
            rc = ext_level_write(fs, ino, in, l);
            if (rc == VFS_OK && at + 1 == 0) rc = ext_fix_keys(fs, ino, in, &p, p.depth);
        } else {
            rc = ext_make_room(fs, ino, in, &p, p.depth, x->ee_block);
            ext_path_free(&p);
            if (rc != VFS_OK) return rc;
            continue;
        }
        ext_path_free(&p);
        if (rc == VFS_OK) rc = ext_try_merge_up(fs, in);
        return rc;
    }
    return VFS_ERR_NOSPC;
}

/* Block LBLK of the unwritten extent holding it is written now: the
 * extent keeps what is before it and after it unwritten, and the block
 * becomes (or joins) a written extent. */
static int ext_mark_written(struct ext4 *fs, uint32_t ino, struct ext4_inode *in,
                            uint32_t lblk) {
    struct ext_path p;
    int rc = ext_find(fs, in, lblk, &p);
    if (rc != VFS_OK) return rc;
    struct ext_level *l = &p.lv[p.depth];
    struct ext4_extent *e = (struct ext4_extent *)(ext_hdr(l) + 1);
    if (l->at < 0) { ext_path_free(&p); return VFS_ERR_IO; }
    struct ext4_extent *x = &e[l->at];
    uint32_t b = x->ee_block, len = extent_len_of(x);
    uint64_t s = extent_start(x);
    if (!extent_unwritten(x) || lblk < b || lblk >= b + len) {
        ext_path_free(&p);
        return VFS_ERR_IO;
    }
    uint32_t left = lblk - b, right = len - left - 1;
    struct ext4_extent w, u;
    extent_set(&w, lblk, 1, s + left, false);
    extent_set(&u, lblk + 1, right, s + left + 1, true);
    bool insert_w = true, insert_u = false;
    if (left > 0) {
        extent_len_set(x, left);
        insert_u = right > 0;
    } else if (right > 0) {
        *x = u;                           /* what is left is after it */
    } else {
        extent_set(x, lblk, 1, s, false); /* the whole extent, now written, */
        insert_w = false;                 /* one with the written ones it touches */
        if (l->at > 0) ext_merge_right(l, l->at - 1);
        ext_merge_right(l, l->at);
    }
    rc = ext_level_write(fs, ino, in, l);
    if (rc == VFS_OK && l->at == 0) rc = ext_fix_keys(fs, ino, in, &p, p.depth);
    ext_path_free(&p);
    if (rc == VFS_OK && insert_w) rc = ext_insert(fs, ino, in, &w);
    if (rc == VFS_OK && insert_u) rc = ext_insert(fs, ino, in, &u);
    return rc;
}

/* ---- legacy block pointers: 12 direct, then single, double and triple
 * indirect blocks (Linux's ext2/ext3 files) ---- */

/* the levels of indirection to LBLK, and the slot in each */
static int legacy_route(const struct ext4 *fs, uint32_t lblk, uint32_t *root, uint32_t slot[3]) {
    uint32_t per = fs->block_size / 4;
    if (lblk < 12) { *root = lblk; return 0; }
    lblk -= 12;
    if (lblk < per) { *root = 12; slot[0] = lblk; return 1; }
    lblk -= per;
    if ((uint64_t)lblk < (uint64_t)per * per) {
        *root = 13; slot[0] = lblk / per; slot[1] = lblk % per; return 2;
    }
    lblk -= per * per;
    *root = 14;
    slot[0] = lblk / (per * per); slot[1] = (lblk / per) % per; slot[2] = lblk % per;
    return slot[0] < per ? 3 : -1;
}

/* LBLK's block; with ALLOC, one made where there is none (and indirect
 * blocks on the way, zeroed). */
static int legacy_map(struct ext4 *fs, struct ext4_inode *in, uint32_t ino, uint32_t lblk,
                      bool alloc, uint64_t *out_phys, bool *fresh) {
    (void)ino;
    uint32_t root, slot[3];
    int levels = legacy_route(fs, lblk, &root, slot);
    *out_phys = 0;
    if (fresh) *fresh = false;
    if (levels < 0) return VFS_ERR_INVAL;
    uint32_t goal_g = (ino - 1) / fs->inodes_per_group;
    uint32_t *ptr = &in->i_block[root];
    uint8_t *buf = levels ? kmalloc(fs->block_size) : NULL;
    if (levels && !buf) return VFS_ERR_NOMEM;
    uint64_t holder = 0;                       /* the indirect block PTR is in */
    int rc = VFS_OK;
    for (int lv = 0; ; lv++) {
        if (*ptr == 0) {
            if (!alloc) break;
            uint32_t b;
            rc = ext4_alloc_block(fs, goal_g, &b);
            if (rc != VFS_OK) break;
            inode_blocks_add(fs, in, 1);
            if (lv < levels) {                 /* an indirect block: zeroed */
                uint8_t *z = kcalloc(1, fs->block_size);
                if (!z) { rc = VFS_ERR_NOMEM; break; }
                rc = write_block(fs, b, z);
                kfree(z);
                if (rc != VFS_OK) break;
            } else if (fresh) {
                *fresh = true;
            }
            *ptr = b;
            if (holder) {
                rc = write_block(fs, holder, buf);
                if (rc != VFS_OK) break;
            }
        }
        if (lv == levels) { *out_phys = *ptr; break; }
        holder = *ptr;
        rc = read_block(fs, holder, buf);
        if (rc != VFS_OK) break;
        ptr = (uint32_t *)buf + slot[lv];
    }
    if (buf) kfree(buf);
    return rc;
}

/* Map logical file block `lblk` to a physical disk block. *out_phys is 0
 * for a hole -- and for an unwritten block, which reads as zeros. */
static int inode_block_map(struct ext4 *fs, const struct ext4_inode *in,
                           uint32_t lblk, uint64_t *out_phys) {
    *out_phys = 0;
    if (!(in->i_flags & EXT4_EXTENTS_FL))
        return legacy_map(fs, (struct ext4_inode *)in, 1, lblk, false, out_phys, NULL);
    struct ext_path p;
    int rc = ext_find(fs, (struct ext4_inode *)in, lblk, &p);
    if (rc != VFS_OK) return rc;
    const struct ext_level *l = &p.lv[p.depth];
    if (l->at >= 0) {
        const struct ext4_extent *x = (const struct ext4_extent *)(ext_hdr(l) + 1) + l->at;
        if (lblk < x->ee_block + extent_len_of(x) && !extent_unwritten(x))
            *out_phys = extent_start(x) + (lblk - x->ee_block);
    }
    ext_path_free(&p);
    return VFS_OK;
}

/* Map `lblk`, allocating a block if it has none and making an unwritten
 * one written; RUN is how many blocks the caller is about to write from
 * here, which a new block is put where there is room for. *FRESH: the
 * block holds nothing of the file yet, so a partial write must start from
 * zeros, not from what the disk holds there. The caller writes the inode. */
static int inode_block_map_alloc(struct ext4 *fs, uint32_t ino, struct ext4_inode *in,
                                 uint32_t lblk, uint32_t run, uint64_t *out_phys,
                                 bool *fresh) {
    *fresh = false;
    if (!(in->i_flags & EXT4_EXTENTS_FL))
        return legacy_map(fs, in, ino, lblk, true, out_phys, fresh);
    struct ext_path p;
    int rc = ext_find(fs, in, lblk, &p);
    if (rc != VFS_OK) return rc;
    const struct ext_level *l = &p.lv[p.depth];
    if (l->at >= 0) {
        const struct ext4_extent *x = (const struct ext4_extent *)(ext_hdr(l) + 1) + l->at;
        if (lblk < x->ee_block + extent_len_of(x)) {
            *out_phys = extent_start(x) + (lblk - x->ee_block);
            bool unwritten = extent_unwritten(x);
            ext_path_free(&p);
            if (!unwritten) return VFS_OK;
            *fresh = true;
            return ext_mark_written(fs, ino, in, lblk);
        }
    }
    uint64_t goal = ext_goal(fs, ino, in, &p, lblk);
    ext_path_free(&p);
    uint32_t b;
    rc = ext4_alloc_block_near(fs, goal, run, &b);
    if (rc != VFS_OK) return rc;
    struct ext4_extent x;
    extent_set(&x, lblk, 1, b, false);
    rc = ext_insert(fs, ino, in, &x);
    if (rc != VFS_OK) { ext4_free_block(fs, b); return rc; }
    inode_blocks_add(fs, in, 1);
    *out_phys = b;
    *fresh = true;
    return VFS_OK;
}

/* ---- trimming: every block from FROM on given back ---- */

static void free_run(struct ext4 *fs, uint64_t start, uint32_t len) {
    for (uint32_t j = 0; j < len; j++) ext4_free_block(fs, (uint32_t)(start + j));
}

/* The subtree under node NODE (a block's bytes, or the root): every block
 * of each extent from FROM on freed, extents shortened or dropped, and
 * nodes left empty freed with their index entries. Returns how many entries
 * NODE keeps; *DIRTY when it changed. */
static int ext_trim_node(struct ext4 *fs, uint32_t ino, const struct ext4_inode *in,
                         uint8_t *node, uint32_t from, int levels_left, bool *dirty) {
    struct ext4_extent_header *h = (struct ext4_extent_header *)node;
    if (h->eh_magic != EXT4_EXT_MAGIC || levels_left < 0) return -1;
    if (h->eh_depth == 0) {
        struct ext4_extent *e = (struct ext4_extent *)(h + 1);
        uint16_t keep = 0;
        for (uint16_t i = 0; i < h->eh_entries; i++) {
            uint32_t len = extent_len_of(&e[i]), first = e[i].ee_block;
            uint64_t start = extent_start(&e[i]);
            if (len == 0) { *dirty = true; continue; }
            if (first >= from) { free_run(fs, start, len); *dirty = true; continue; }
            if (first + len > from) {
                uint32_t k = from - first;
                free_run(fs, start + k, len - k);
                extent_len_set(&e[i], k);
                *dirty = true;
            }
            if (keep != i) e[keep] = e[i];
            keep++;
        }
        h->eh_entries = keep;
        return keep;
    }
    struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(h + 1);
    uint8_t *child = kmalloc(fs->block_size);
    if (!child) return -1;
    uint16_t keep = 0;
    for (uint16_t i = 0; i < h->eh_entries; i++) {
        uint32_t next = i + 1 < h->eh_entries ? ix[i + 1].ei_block : 0xFFFFFFFFu;
        uint64_t cb = idx_child(&ix[i]);
        if (next <= from) {                          /* wholly before the cut */
            if (keep != i) ix[keep] = ix[i];
            keep++;
            continue;
        }
        if (read_block(fs, cb, child) != VFS_OK) { kfree(child); return -1; }
        bool cd = false;
        int left = ext_trim_node(fs, ino, in, child, from, levels_left - 1, &cd);
        if (left < 0) { kfree(child); return -1; }
        if (left == 0) {                             /* the child is gone */
            ext4_free_block(fs, (uint32_t)cb);
            *dirty = true;
            continue;
        }
        if (cd) {
            struct ext_level cl = { child, cb, 0 };
            if (ext_level_write(fs, ino, in, &cl) != VFS_OK) { kfree(child); return -1; }
        }
        if (keep != i) { ix[keep] = ix[i]; *dirty = true; }
        keep++;
    }
    kfree(child);
    h->eh_entries = keep;
    return keep;
}

/* legacy: the indirect block BLK (LEVEL 1 holds data blocks) covering file
 * blocks from BASE, trimmed from FROM; returns whether it still holds any */
static int legacy_trim_ind(struct ext4 *fs, uint32_t blk, int level, uint64_t base,
                           uint64_t span, uint32_t from) {
    uint32_t per = fs->block_size / 4;
    uint32_t *t = kmalloc(fs->block_size);
    if (!t) return -1;
    if (read_block(fs, blk, t) != VFS_OK) { kfree(t); return -1; }
    bool dirty = false, any = false;
    for (uint32_t i = 0; i < per; i++) {
        if (!t[i]) continue;
        uint64_t lo = base + (uint64_t)i * span;
        if (lo + span <= from) { any = true; continue; }
        if (level == 1) {
            ext4_free_block(fs, t[i]);
            t[i] = 0;
            dirty = true;
            continue;
        }
        int still = legacy_trim_ind(fs, t[i], level - 1, lo, span / per, from);
        if (still < 0) { kfree(t); return -1; }
        if (!still) { ext4_free_block(fs, t[i]); t[i] = 0; dirty = true; }
        else any = true;
    }
    if (dirty && any) (void)write_block(fs, blk, t);
    kfree(t);
    return any;
}

/* how many blocks the inode holds: its data, its tree or indirect blocks,
 * its attribute block -- i_blocks, as Linux counts it */
static uint64_t ext_count_node(struct ext4 *fs, const uint8_t *node, int levels_left) {
    const struct ext4_extent_header *h = (const struct ext4_extent_header *)node;
    if (h->eh_magic != EXT4_EXT_MAGIC || levels_left < 0) return 0;
    uint64_t n = 0;
    if (h->eh_depth == 0) {
        const struct ext4_extent *e = (const struct ext4_extent *)(h + 1);
        for (uint16_t i = 0; i < h->eh_entries; i++) n += extent_len_of(&e[i]);
        return n;
    }
    const struct ext4_extent_idx *ix = (const struct ext4_extent_idx *)(h + 1);
    uint8_t *child = kmalloc(fs->block_size);
    if (!child) return 0;
    for (uint16_t i = 0; i < h->eh_entries; i++)
        if (read_block(fs, idx_child(&ix[i]), child) == VFS_OK)
            n += 1 + ext_count_node(fs, child, levels_left - 1);
    kfree(child);
    return n;
}

static uint64_t legacy_count_ind(struct ext4 *fs, uint32_t blk, int level) {
    uint32_t per = fs->block_size / 4;
    uint32_t *t = kmalloc(fs->block_size);
    if (!t) return 0;
    uint64_t n = 1;
    if (read_block(fs, blk, t) == VFS_OK)
        for (uint32_t i = 0; i < per; i++)
            if (t[i]) n += level == 1 ? 1 : legacy_count_ind(fs, t[i], level - 1);
    kfree(t);
    return n;
}

static void inode_blocks_recount(struct ext4 *fs, struct ext4_inode *in) {
    uint64_t n = 0;
    if (in->i_flags & EXT4_EXTENTS_FL) {
        n = ext_count_node(fs, (const uint8_t *)in->i_block, EXT_MAX_LEVELS);
    } else {
        for (int i = 0; i < 12; i++) if (in->i_block[i]) n++;
        for (int i = 12; i < 15; i++)
            if (in->i_block[i]) n += legacy_count_ind(fs, in->i_block[i], i - 11);
    }
    if (in->i_file_acl_lo | osd2_half(in, 2)) n++;
    in->i_blocks_lo = 0;
    osd2_set_half(in, 0, 0);
    inode_blocks_add(fs, in, (int64_t)n);
}

/* the extents under NODE, into V (at most MAX; -1 if there are more), and
 * the tree's blocks below it into B */
static int ext_gather(struct ext4 *fs, const uint8_t *node, int levels_left,
                      struct ext4_extent *v, int max, uint64_t *b, int *nb) {
    const struct ext4_extent_header *h = (const struct ext4_extent_header *)node;
    if (h->eh_magic != EXT4_EXT_MAGIC || levels_left < 0) return -1;
    if (h->eh_depth == 0) {
        if (h->eh_entries > max) return -1;
        memcpy(v, h + 1, (size_t)h->eh_entries * 12);
        return h->eh_entries;
    }
    const struct ext4_extent_idx *ix = (const struct ext4_extent_idx *)(h + 1);
    uint8_t *child = kmalloc(fs->block_size);
    if (!child) return -1;
    int n = 0;
    for (uint16_t i = 0; i < h->eh_entries; i++) {
        if (*nb >= 16 || read_block(fs, idx_child(&ix[i]), child) != VFS_OK) { n = -1; break; }
        b[(*nb)++] = idx_child(&ix[i]);
        int k = ext_gather(fs, child, levels_left - 1, v + n, max - n, b, nb);
        if (k < 0) { n = -1; break; }
        n += k;
    }
    kfree(child);
    return n;
}

/* A tree cut so small that the inode holds every extent left: they move
 * into it and the tree's blocks are freed -- Linux's merge up, here after
 * a truncate too, so a file cut short keeps no levels it has no need of
 * (e2fsck offers to make such a tree shorter). */
static void ext_shrink(struct ext4 *fs, struct ext4_inode *in) {
    struct ext4_extent_header *h = (struct ext4_extent_header *)in->i_block;
    uint16_t root_max = (uint16_t)((sizeof(in->i_block) - sizeof(*h)) / 12);
    /* a root of one index whose child the root would hold: the child's
     * place taken by its entries, a level fewer */
    while (h->eh_depth > 1 && h->eh_entries == 1) {
        uint64_t cb = idx_child((struct ext4_extent_idx *)(h + 1));
        uint8_t *c = kmalloc(fs->block_size);
        if (!c) return;
        const struct ext4_extent_header *ch = (const struct ext4_extent_header *)c;
        bool pulled = read_block(fs, cb, c) == VFS_OK && ch->eh_magic == EXT4_EXT_MAGIC &&
                      ch->eh_depth + 1 == h->eh_depth && ch->eh_entries <= root_max;
        if (pulled) {
            memset(h + 1, 0, sizeof(in->i_block) - sizeof(*h));
            memcpy(h + 1, ch + 1, (size_t)ch->eh_entries * 12);
            h->eh_entries = ch->eh_entries;
            h->eh_depth   = ch->eh_depth;
            ext4_free_block(fs, (uint32_t)cb);
        }
        kfree(c);
        if (!pulled) break;
    }
    if (h->eh_depth == 0) return;
    struct ext4_extent v[4];
    uint64_t blocks[16];
    int nb = 0;
    int n = ext_gather(fs, (const uint8_t *)in->i_block, EXT_MAX_LEVELS, v, root_max, blocks, &nb);
    if (n < 0) return;
    memset(h + 1, 0, sizeof(in->i_block) - sizeof(*h));
    memcpy(h + 1, v, (size_t)n * 12);
    h->eh_entries = (uint16_t)n;
    h->eh_depth   = 0;
    h->eh_max     = root_max;
    for (int i = 0; i < nb; i++) ext4_free_block(fs, (uint32_t)blocks[i]);
}

/* Release every block from logical block `from` onward, keeping everything
 * below it; `from == 0` frees all the file's data (and its tree). */
static int inode_trim_from(struct ext4 *fs, uint32_t ino, struct ext4_inode *in,
                           uint32_t from) {
    if (in->i_flags & EXT4_EXTENTS_FL) {
        struct ext4_extent_header *h = (struct ext4_extent_header *)in->i_block;
        bool dirty = false;
        int left = ext_trim_node(fs, ino, in, (uint8_t *)in->i_block, from,
                                 EXT_MAX_LEVELS, &dirty);
        if (left < 0) return VFS_ERR_IO;
        if (left == 0) {           /* nothing left: back to an empty leaf, as Linux */
            h->eh_depth = 0;
            h->eh_max = (sizeof(in->i_block) - sizeof(*h)) / 12;
        } else {
            ext_shrink(fs, in);
        }
    } else {
        uint32_t per = fs->block_size / 4;
        for (uint32_t i = 0; i < 12; i++)
            if (in->i_block[i] && i >= from) { ext4_free_block(fs, in->i_block[i]); in->i_block[i] = 0; }
        uint64_t base = 12, span = 1;
        for (int r = 12; r < 15; r++) {
            uint64_t cover = span * per;     /* file blocks under this root */
            if (in->i_block[r] && base + cover > from) {
                int still = legacy_trim_ind(fs, in->i_block[r], r - 11, base, span, from);
                if (still < 0) return VFS_ERR_IO;
                if (!still) { ext4_free_block(fs, in->i_block[r]); in->i_block[r] = 0; }
            }
            base += cover;
            span *= per;
        }
    }
    inode_blocks_recount(fs, in);
    return VFS_OK;
}

/* The inode's share of its extended-attribute block given back: the
 * block's count of owners drops, and the block is freed at zero, as
 * Linux's. (Unlinking left it allocated, owned by nothing.) */
static void xattr_block_release(struct ext4 *fs, struct ext4_inode *in) {
    uint64_t acl = in->i_file_acl_lo | ((uint64_t)osd2_half(in, 2) << 32);
    if (!acl) return;
    in->i_file_acl_lo = 0;
    osd2_set_half(in, 2, 0);
    if (acl >= fs->total_blocks) return;
    uint8_t *b = kmalloc(fs->block_size);
    if (!b) return;
    uint32_t magic, refs;
    if (read_block(fs, acl, b) == VFS_OK && (memcpy(&magic, b, 4), magic == EXT4_XATTR_MAGIC)) {
        memcpy(&refs, b + 4, 4);
        if (refs > 1) {
            refs--;
            memcpy(b + 4, &refs, 4);
            if (fs->csum) {
                uint32_t c = xattr_block_csum(fs, acl, b);
                memcpy(b + EXT4_XATTR_CSUM_OFF, &c, 4);
            }
            (void)write_block(fs, acl, b);
        } else {
            ext4_free_block(fs, (uint32_t)acl);
        }
    }
    kfree(b);
}

/* Free everything an inode holds: its data, its tree or indirect blocks,
 * its share of an attribute block. */
static void inode_free_data(struct ext4 *fs, uint32_t ino, struct ext4_inode *in) {
    (void)inode_trim_from(fs, ino, in, 0);
    xattr_block_release(fs, in);
    inode_blocks_recount(fs, in);
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

/* ---- directory leaf blocks and their tails (finding 271) ----
 * On a volume with sums a leaf block's last 12 bytes are the tail entry
 * holding its sum: no entry may take that room, and a leaf block made is
 * made with one. */
static uint32_t dir_leaf_end(const struct ext4 *fs, const uint8_t *blk) {
    return fs->block_size - (dir_tail_at(fs, blk) ? EXT4_DIR_TAIL_LEN : 0);
}

static void dir_leaf_csum_set(const struct ext4 *fs, uint32_t dir_ino,
                              const struct ext4_inode *dir, uint8_t *blk) {
    if (!fs->csum || !dir_tail_at(fs, blk)) return;
    uint32_t c = dir_leaf_csum(fs, inode_seed(fs, dir_ino, dir->i_generation), blk);
    memcpy(blk + fs->block_size - 4, &c, 4);
}

/* an empty leaf: one unused entry over the room, then the tail */
static void dir_leaf_init(const struct ext4 *fs, uint8_t *blk) {
    memset(blk, 0, fs->block_size);
    uint32_t room = fs->block_size - (fs->csum ? EXT4_DIR_TAIL_LEN : 0);
    ((struct ext4_dir_entry_2 *)blk)->rec_len = (uint16_t)room;
    if (fs->csum) {
        struct ext4_dir_entry_2 *t = (struct ext4_dir_entry_2 *)(blk + room);
        t->rec_len   = EXT4_DIR_TAIL_LEN;
        t->file_type = EXT4_DIR_TAIL_FT;
    }
}

static int dir_leaf_write(struct ext4 *fs, uint32_t dir_ino,
                          const struct ext4_inode *dir, uint64_t phys, uint8_t *blk) {
    dir_leaf_csum_set(fs, dir_ino, dir, blk);
    return write_block(fs, phys, blk);
}

static int dx_add_entry(struct ext4 *fs, uint32_t dir_ino, struct ext4_inode *dir,
                        const char *name, uint32_t child_ino, uint8_t file_type);
static int dx_make_indexed(struct ext4 *fs, uint32_t dir_ino, struct ext4_inode *dir,
                           const char *name, uint32_t child_ino, uint8_t file_type);
static bool dir_is_dx(const struct ext4 *fs, const struct ext4_inode *dir);
static bool dir_may_index(const struct ext4 *fs, const struct ext4_inode *dir);

/* Insert (name -> child_ino) into directory `dir_ino`. Walks existing
 * dir blocks for a slot with enough slack (splitting it), else grows the
 * directory by one block via the unified extent/legacy allocator. An htree
 * directory takes the entry where its index says (dx_add_entry); a
 * one-block directory that is full becomes one, as Linux makes it. */
static int dir_add_entry(struct ext4 *fs, uint32_t dir_ino,
                         const char *name, uint32_t child_ino,
                         uint8_t file_type) {
    struct ext4_inode dir;
    int rc = read_inode(fs, dir_ino, &dir);
    if (rc != VFS_OK) return rc;
    if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    if (dir_is_dx(fs, &dir))
        return dx_add_entry(fs, dir_ino, &dir, name, child_ino, file_type);

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

        uint32_t off = 0, end = dir_leaf_end(fs, blk);
        while (off + 8 <= end) {
            struct ext4_dir_entry_2 *de =
                (struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > end) break;

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
                rc = dir_leaf_write(fs, dir_ino, &dir, phys, blk);
                kfree(blk);
                /* an entry added changes the directory: its mtime and
                 * ctime, as Linux's (finding 255) */
                if (rc == VFS_OK) {
                    ext4_stamp(&dir, E4_MTIME | E4_CTIME, ext4_now());
                    rc = write_inode(fs, dir_ino, &dir);
                }
                return rc;
            }
            off += rec_len;
        }
    }

    /* No room. A one-block directory on a volume with htree directories
     * becomes one, as Linux's grows it (its index, two leaves). */
    if (total_blocks == 1 && dir_may_index(fs, &dir)) {
        kfree(blk);
        return dx_make_indexed(fs, dir_ino, &dir, name, child_ino, file_type);
    }

    /* Otherwise grow the directory by one block. */
    uint64_t new_phys = 0;
    bool fresh;
    rc = inode_block_map_alloc(fs, dir_ino, &dir, total_blocks, 1, &new_phys, &fresh);
    if (rc != VFS_OK) { kfree(blk); return rc; }

    dir_leaf_init(fs, blk);
    struct ext4_dir_entry_2 *ne = (struct ext4_dir_entry_2 *)blk;
    ne->inode     = child_ino;
    ne->name_len  = nlen;
    ne->file_type = file_type;
    memcpy((char *)ne + 8, name, nlen);
    rc = dir_leaf_write(fs, dir_ino, &dir, new_phys, blk);
    kfree(blk);
    if (rc != VFS_OK) return rc;

    dir.i_size_lo += fs->block_size;       /* i_blocks already bumped by alloc */
    ext4_stamp(&dir, E4_MTIME | E4_CTIME, ext4_now());
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

        uint32_t off = 0, end = dir_leaf_end(fs, blk);
        struct ext4_dir_entry_2 *prev = NULL;
        while (off + 8 <= end) {
            struct ext4_dir_entry_2 *de =
                (struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > end) break;

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
                    rc = dir_leaf_write(fs, dir_ino, &dir, phys, blk);
                    kfree(blk);
                    if (rc == VFS_OK) {     /* as dir_add_entry's */
                        ext4_stamp(&dir, E4_MTIME | E4_CTIME, ext4_now());
                        rc = write_inode(fs, dir_ino, &dir);
                    }
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

/* ================================================================
 * htree directories (finding 271).
 *
 * Linux finds a name in an htree directory by its hash alone: the index in
 * block 0 (and, below it, index nodes) maps hash ranges to leaf blocks, and
 * only the leaf the name's hash falls in is searched. An entry put into any
 * other leaf -- as a linear insert does -- is there for this driver's linear
 * walk and missing for Linux. So an entry goes where the index says, a full
 * leaf is split by hash with its upper half in a new block, and the index
 * gains an entry for it; a full index node is split, and a full root gains
 * a level, up to Linux's two (three with largedir). The hash is the
 * directory's own: legacy, half-MD4 or TEA over the volume's seed, its
 * characters signed or unsigned as the superblock says. A one-block
 * directory that fills becomes an htree one, as Linux's does. Removing an
 * entry is the linear walk's: a leaf keeps its range when it empties.
 * ================================================================ */

#define DX_HASH_LEGACY   0
#define DX_HASH_HALF_MD4 1
#define DX_HASH_TEA      2
#define DX_ROOT_COUNT_OFF 0x20u
#define DX_NODE_COUNT_OFF 8u
#define DX_TAIL_LEN       8u

static uint32_t rol32(uint32_t v, unsigned s) { return (v << s) | (v >> (32 - s)); }

/* LEN bytes of NAME as NUM words, the way the htree hashes read a name:
 * each word four characters (signed or not) shifted in, the rest the
 * length repeated */
static void dx_str2words(const char *name, int len, uint32_t *w, int num, bool sign) {
    uint32_t pad = (uint32_t)len | ((uint32_t)len << 8);
    pad |= pad << 16;
    uint32_t val = pad;
    if (len > num * 4) len = num * 4;
    for (int i = 0; i < len; i++) {
        int c = sign ? (int)(signed char)name[i] : (int)(unsigned char)name[i];
        val = (uint32_t)c + (val << 8);
        if (i % 4 == 3) { *w++ = val; val = pad; num--; }
    }
    if (--num >= 0) *w++ = val;
    while (--num >= 0) *w++ = pad;
}

static void dx_half_md4(uint32_t buf[4], const uint32_t in[8]) {
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
#define F_(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G_(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H_(x, y, z) ((x) ^ (y) ^ (z))
#define R_(f, a, b, c, d, x, s) (a = rol32(a + f(b, c, d) + (x), s))
    const uint32_t k2 = 0x5A827999u, k3 = 0x6ED9EBA1u;
    R_(F_, a, b, c, d, in[0], 3);  R_(F_, d, a, b, c, in[1], 7);
    R_(F_, c, d, a, b, in[2], 11); R_(F_, b, c, d, a, in[3], 19);
    R_(F_, a, b, c, d, in[4], 3);  R_(F_, d, a, b, c, in[5], 7);
    R_(F_, c, d, a, b, in[6], 11); R_(F_, b, c, d, a, in[7], 19);
    R_(G_, a, b, c, d, in[1] + k2, 3);  R_(G_, d, a, b, c, in[3] + k2, 5);
    R_(G_, c, d, a, b, in[5] + k2, 9);  R_(G_, b, c, d, a, in[7] + k2, 13);
    R_(G_, a, b, c, d, in[0] + k2, 3);  R_(G_, d, a, b, c, in[2] + k2, 5);
    R_(G_, c, d, a, b, in[4] + k2, 9);  R_(G_, b, c, d, a, in[6] + k2, 13);
    R_(H_, a, b, c, d, in[3] + k3, 3);  R_(H_, d, a, b, c, in[7] + k3, 9);
    R_(H_, c, d, a, b, in[2] + k3, 11); R_(H_, b, c, d, a, in[6] + k3, 15);
    R_(H_, a, b, c, d, in[1] + k3, 3);  R_(H_, d, a, b, c, in[5] + k3, 9);
    R_(H_, c, d, a, b, in[0] + k3, 11); R_(H_, b, c, d, a, in[4] + k3, 15);
#undef F_
#undef G_
#undef H_
#undef R_
    buf[0] += a; buf[1] += b; buf[2] += c; buf[3] += d;
}

static void dx_tea(uint32_t buf[4], const uint32_t in[4]) {
    uint32_t sum = 0, b0 = buf[0], b1 = buf[1];
    for (int n = 0; n < 16; n++) {
        sum += 0x9E3779B9u;
        b0 += ((b1 << 4) + in[0]) ^ (b1 + sum) ^ ((b1 >> 5) + in[1]);
        b1 += ((b0 << 4) + in[2]) ^ (b0 + sum) ^ ((b0 >> 5) + in[3]);
    }
    buf[0] += b0; buf[1] += b1;
}

/* NAME's hash under VERSION (the root's), as Linux computes it */
static uint32_t dx_hash(const struct ext4 *fs, const char *name, int len, uint8_t version) {
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    bool sign = !(sb->s_flags & EXT4_FLAGS_UNSIGNED_HASH);
    uint32_t buf[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };
    if (sb->s_hash_seed[0] | sb->s_hash_seed[1] | sb->s_hash_seed[2] | sb->s_hash_seed[3])
        memcpy(buf, sb->s_hash_seed, 16);
    uint32_t hash = 0, in[8];
    switch (version) {
    case DX_HASH_LEGACY: {
        uint32_t h0 = 0x12A3FE2Du, h1 = 0x37ABE8F9u;
        for (int i = 0; i < len; i++) {
            int c = sign ? (int)(signed char)name[i] : (int)(unsigned char)name[i];
            uint32_t h = h1 + (h0 ^ (uint32_t)(c * 7152373));
            if (h & 0x80000000u) h -= 0x7FFFFFFFu;
            h1 = h0;
            h0 = h;
        }
        hash = h0 << 1;
        break;
    }
    case DX_HASH_HALF_MD4:
        for (const char *p = name; len > 0; len -= 32, p += 32) {
            dx_str2words(p, len, in, 8, sign);
            dx_half_md4(buf, in);
        }
        hash = buf[1];
        break;
    case DX_HASH_TEA:
        for (const char *p = name; len > 0; len -= 16, p += 16) {
            dx_str2words(p, len, in, 4, sign);
            dx_tea(buf, in);
        }
        hash = buf[0];
        break;
    default:
        return 0;
    }
    hash &= ~1u;
    if (hash == 0xFFFFFFFEu) hash = 0xFFFFFFFCu;      /* never the end-of-dir mark */
    return hash;
}

static bool dir_is_dx(const struct ext4 *fs, const struct ext4_inode *dir) {
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    return (sb->s_feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX) &&
           (dir->i_flags & EXT4_INDEX_FL);
}

static bool dir_may_index(const struct ext4 *fs, const struct ext4_inode *dir) {
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    return (sb->s_feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX) &&
           !(dir->i_flags & EXT4_INDEX_FL) && (dir->i_flags & EXT4_EXTENTS_FL);
}

static uint16_t dx_get16(const uint8_t *b, uint32_t off) { uint16_t v; memcpy(&v, b + off, 2); return v; }
static uint32_t dx_get32(const uint8_t *b, uint32_t off) { uint32_t v; memcpy(&v, b + off, 4); return v; }
static void dx_put16(uint8_t *b, uint32_t off, uint16_t v) { memcpy(b + off, &v, 2); }
static void dx_put32(uint8_t *b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }

/* entry I of the index at COFF: its hash (entry 0's is implicitly 0, its
 * room holding the count and limit) and its leaf's logical block */
static uint32_t dx_hash_of(const uint8_t *b, uint32_t coff, uint32_t i) {
    return i ? dx_get32(b, coff + 8 * i) : 0;
}
static uint32_t dx_block_of(const uint8_t *b, uint32_t coff, uint32_t i) {
    return dx_get32(b, coff + 8 * i + 4);
}

static uint16_t dx_limit_for(const struct ext4 *fs, bool root) {
    uint32_t room = fs->block_size - (root ? DX_ROOT_COUNT_OFF : DX_NODE_COUNT_OFF) -
                    (fs->csum ? DX_TAIL_LEN : 0);
    return (uint16_t)(room / 8);
}

struct dx_frame {
    uint32_t lblk;
    uint64_t phys;
    uint8_t *buf;
    uint32_t coff;        /* where the count and limit are */
    uint32_t at;          /* the entry followed down */
};

static int dx_frame_write(struct ext4 *fs, uint32_t dir_ino, const struct ext4_inode *dir,
                          struct dx_frame *f) {
    if (fs->csum) {
        uint16_t limit = dx_get16(f->buf, f->coff);
        uint32_t t = f->coff + (uint32_t)limit * 8u;
        if (t + DX_TAIL_LEN <= fs->block_size) {
            dx_put32(f->buf, t, 0);
            dx_put32(f->buf, t + 4,
                     dx_csum(inode_seed(fs, dir_ino, dir->i_generation), f->buf, f->coff));
        }
    }
    return write_block(fs, f->phys, f->buf);
}

/* the leaf block for HASH: the root's and each node's last entry whose
 * hash is at or below it. FR gets the path, FR[0] the root; *DEPTH the
 * frames used. */
static int dx_probe(struct ext4 *fs, const struct ext4_inode *dir, uint32_t hash,
                    struct dx_frame *fr, int *depth, uint8_t *version) {
    uint8_t *root = fr[0].buf;
    uint64_t phys = 0;
    int rc = inode_block_map(fs, dir, 0, &phys);
    if (rc != VFS_OK || !phys) return VFS_ERR_IO;
    rc = read_block(fs, phys, root);
    if (rc != VFS_OK) return rc;
    uint8_t info_len = root[0x1D], levels = root[0x1E];
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    int max = (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_LARGEDIR) ? 3 : 2;
    if (dx_get32(root, 0x18) != 0 || info_len != 8 || levels >= max ||
        root[0x1C] > DX_HASH_TEA) return VFS_ERR_IO;
    *version = root[0x1C];
    fr[0].lblk = 0;
    fr[0].phys = phys;
    fr[0].coff = DX_ROOT_COUNT_OFF;
    for (int lv = 0; ; lv++) {
        struct dx_frame *f = &fr[lv];
        uint16_t count = dx_get16(f->buf, f->coff + 2);
        uint16_t limit = dx_get16(f->buf, f->coff);
        if (count == 0 || count > limit) return VFS_ERR_IO;
        uint32_t at = 0;
        for (uint32_t i = 1; i < count; i++) {
            if (dx_hash_of(f->buf, f->coff, i) > hash) break;
            at = i;
        }
        f->at = at;
        if (lv == levels) { *depth = lv + 1; return VFS_OK; }
        struct dx_frame *n = &fr[lv + 1];
        n->lblk = dx_block_of(f->buf, f->coff, at);
        rc = inode_block_map(fs, dir, n->lblk, &n->phys);
        if (rc != VFS_OK || !n->phys) return VFS_ERR_IO;
        rc = read_block(fs, n->phys, n->buf);
        if (rc != VFS_OK) return rc;
        n->coff = DX_NODE_COUNT_OFF;
    }
}

/* a new block at the directory's end, for a leaf or an index node */
static int dir_append_block(struct ext4 *fs, uint32_t dir_ino, struct ext4_inode *dir,
                            uint32_t *lblk, uint64_t *phys) {
    uint64_t size = ((uint64_t)dir->i_size_hi << 32) | dir->i_size_lo;
    *lblk = (uint32_t)(size / fs->block_size);
    bool fresh;
    int rc = inode_block_map_alloc(fs, dir_ino, dir, *lblk, 1, phys, &fresh);
    if (rc != VFS_OK) return rc;
    size += fs->block_size;
    dir->i_size_lo = (uint32_t)size;
    dir->i_size_hi = (uint32_t)(size >> 32);
    return VFS_OK;
}

/* Put (HASH, LBLK) into index frame F after its entry F->at. It has room. */
static void dx_insert_at(struct dx_frame *f, uint32_t hash, uint32_t lblk) {
    uint16_t count = dx_get16(f->buf, f->coff + 2);
    uint32_t pos = f->coff + 8 * (f->at + 1);
    memmove(f->buf + pos + 8, f->buf + pos, 8u * (count - f->at - 1));
    dx_put32(f->buf, pos, hash);
    dx_put32(f->buf, pos + 4, lblk);
    dx_put16(f->buf, f->coff + 2, (uint16_t)(count + 1));
}

/* A leaf's live entries: where each is, its length, its hash. */
struct dx_ent { uint32_t hash; uint16_t off; uint16_t len; };

static int dx_ent_cmp(const struct dx_ent *a, const struct dx_ent *b) {
    return a->hash < b->hash ? -1 : a->hash > b->hash ? 1 : (int)a->off - (int)b->off;
}

static uint32_t dx_leaf_entries(const struct ext4 *fs, const uint8_t *blk, uint8_t version,
                                struct dx_ent *v) {
    uint32_t n = 0, off = 0, end = dir_leaf_end(fs, blk);
    while (off + 8 <= end) {
        const struct ext4_dir_entry_2 *de = (const struct ext4_dir_entry_2 *)(blk + off);
        if (de->rec_len < 8 || off + de->rec_len > end) break;
        if (de->inode && de->name_len) {
            v[n].hash = dx_hash(fs, de->name, de->name_len, version);
            v[n].off  = (uint16_t)off;
            v[n].len  = (uint16_t)((8 + de->name_len + 3) & ~3u);
            n++;
        }
        off += de->rec_len;
    }
    /* by hash (an insertion sort: a leaf holds a few hundred at most) */
    for (uint32_t i = 1; i < n; i++) {
        struct dx_ent t = v[i];
        uint32_t j = i;
        while (j > 0 && dx_ent_cmp(&v[j - 1], &t) > 0) { v[j] = v[j - 1]; j--; }
        v[j] = t;
    }
    return n;
}

/* DST: an empty leaf holding SRC's entries V[0..N), in that order */
static void dx_leaf_build(const struct ext4 *fs, uint8_t *dst, const uint8_t *src,
                          const struct dx_ent *v, uint32_t n) {
    dir_leaf_init(fs, dst);
    uint32_t room = ((struct ext4_dir_entry_2 *)dst)->rec_len, off = 0;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(dst + off, src + v[i].off, v[i].len);
        struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(dst + off);
        de->rec_len = (uint16_t)(i + 1 == n ? room - off : v[i].len);
        off += v[i].len;
    }
}

/* Put the entry into leaf BLK if it fits; false if it does not. */
static bool dx_leaf_add(const struct ext4 *fs, uint8_t *blk, const char *name,
                        uint32_t child, uint8_t ft) {
    uint8_t nlen = (uint8_t)strlen(name);
    uint16_t need = (uint16_t)((8 + nlen + 3) & ~3u);
    uint32_t off = 0, end = dir_leaf_end(fs, blk);
    while (off + 8 <= end) {
        struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(blk + off);
        uint16_t rec = de->rec_len;
        if (rec < 8 || off + rec > end) return false;
        uint16_t used = de->inode ? (uint16_t)((8 + de->name_len + 3) & ~3u) : 0;
        if (rec - used >= need) {
            if (de->inode) { de->rec_len = used; off += used; }
            struct ext4_dir_entry_2 *ne = (struct ext4_dir_entry_2 *)(blk + off);
            ne->inode = child;
            ne->rec_len = (uint16_t)(rec - used);
            ne->name_len = nlen;
            ne->file_type = ft;
            memcpy(ne->name, name, nlen);
            return true;
        }
        off += rec;
    }
    return false;
}

/* Make room in the deepest index frame of FR[0..*DEPTH) for one entry:
 * split a full node into two (its parent has room or is made to), or give
 * a full root a level below it, as Linux's. */
static int dx_make_room(struct ext4 *fs, uint32_t dir_ino, struct ext4_inode *dir,
                        struct dx_frame *fr, int *depth, uint8_t *spare) {
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    int max = (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_LARGEDIR) ? 3 : 2;
    int lv = *depth - 1;
    struct dx_frame *f = &fr[lv];
    if (dx_get16(f->buf, f->coff + 2) < dx_get16(f->buf, f->coff)) return VFS_OK;

    if (lv == 0) {
        /* the root is full: its entries go down to a new node */
        if (*depth != 1 || fr[0].buf[0x1E] + 1 >= max)
            return VFS_ERR_NOSPC;                       /* Linux's "index full" */
        uint32_t nl; uint64_t np;
        int rc = dir_append_block(fs, dir_ino, dir, &nl, &np);
        if (rc != VFS_OK) return rc;
        uint16_t count = dx_get16(f->buf, f->coff + 2);
        struct dx_frame *n = &fr[1];
        n->buf = spare;
        memset(n->buf, 0, fs->block_size);
        ((struct ext4_dir_entry_2 *)n->buf)->rec_len = (uint16_t)fs->block_size;
        n->coff = DX_NODE_COUNT_OFF;
        memcpy(n->buf + n->coff, f->buf + f->coff, 8u * count);
        dx_put16(n->buf, n->coff, dx_limit_for(fs, false));
        n->lblk = nl;
        n->phys = np;
        n->at = f->at;
        dx_put16(f->buf, f->coff + 2, 1);
        dx_put32(f->buf, f->coff + 4, nl);
        f->at = 0;
        f->buf[0x1E]++;
        (*depth)++;
        rc = dx_frame_write(fs, dir_ino, dir, n);
        if (rc == VFS_OK) rc = dx_frame_write(fs, dir_ino, dir, f);
        return rc;
    }

    /* a full node: its parent needs room for one more entry first */
    int rc = dx_make_room(fs, dir_ino, dir, fr, &lv, spare);
    if (rc != VFS_OK) return rc;
    if (lv + 1 != *depth) { *depth = lv + 1; f = &fr[lv]; }     /* a level was added */
    struct dx_frame *p = &fr[lv - 1];
    uint16_t count = dx_get16(f->buf, f->coff + 2);
    uint16_t c1 = count / 2, c2 = (uint16_t)(count - c1);
    uint32_t hash2 = dx_hash_of(f->buf, f->coff, c1);
    uint32_t nl; uint64_t np;
    rc = dir_append_block(fs, dir_ino, dir, &nl, &np);
    if (rc != VFS_OK) return rc;
    uint8_t *nb = kmalloc(fs->block_size);
    if (!nb) return VFS_ERR_NOMEM;
    memset(nb, 0, fs->block_size);
    ((struct ext4_dir_entry_2 *)nb)->rec_len = (uint16_t)fs->block_size;
    memcpy(nb + DX_NODE_COUNT_OFF, f->buf + f->coff + 8u * c1, 8u * c2);
    dx_put16(nb, DX_NODE_COUNT_OFF, dx_limit_for(fs, false));
    dx_put16(nb, DX_NODE_COUNT_OFF + 2, c2);
    dx_put16(f->buf, f->coff + 2, c1);
    struct dx_frame nf = { nl, np, nb, DX_NODE_COUNT_OFF, 0 };
    dx_insert_at(p, hash2, nl);
    rc = dx_frame_write(fs, dir_ino, dir, &nf);
    if (rc == VFS_OK) rc = dx_frame_write(fs, dir_ino, dir, f);
    if (rc == VFS_OK) rc = dx_frame_write(fs, dir_ino, dir, p);
    if (rc == VFS_OK && f->at >= c1) {        /* the path goes through the new node */
        memcpy(f->buf, nb, fs->block_size);
        f->lblk = nl;
        f->phys = np;
        f->at -= c1;
        p->at++;
    }
    kfree(nb);
    return rc;
}

/* Split LEAF (FR's deepest entry's block, at PHYS) by hash: its upper half
 * to a new block, indexed after it. Then the new entry goes into the half
 * its hash falls in. */
static int dx_split_add(struct ext4 *fs, uint32_t dir_ino, struct ext4_inode *dir,
                        struct dx_frame *fr, int depth, uint8_t version, uint8_t *leaf,
                        uint64_t phys, const char *name, uint32_t hash,
                        uint32_t child, uint8_t ft) {
    struct dx_ent *v = kmalloc(sizeof(*v) * (fs->block_size / 12 + 1));
    uint8_t *a = kmalloc(fs->block_size), *b = kmalloc(fs->block_size);
    int rc = VFS_ERR_NOMEM;
    if (!v || !a || !b) goto out;
    uint32_t n = dx_leaf_entries(fs, leaf, version, v);
    if (n < 2) { rc = VFS_ERR_NOSPC; goto out; }      /* one name fills it */
    /* Linux's point: from the top, while an entry's middle is in the
     * block's upper half */
    uint32_t size = 0, move = 0;
    for (uint32_t i = n; i-- > 0; ) {
        if (size + v[i].len / 2 > fs->block_size / 2) break;
        size += v[i].len;
        move++;
    }
    if (move == 0) move = 1;
    if (move >= n) move = n - 1;
    uint32_t split = n - move;
    uint32_t hash2 = v[split].hash;
    uint32_t cont = hash2 == v[split - 1].hash;     /* the range goes on from the leaf before */
    uint32_t nl; uint64_t np;
    rc = dir_append_block(fs, dir_ino, dir, &nl, &np);
    if (rc != VFS_OK) goto out;
    dx_leaf_build(fs, a, leaf, v, split);
    dx_leaf_build(fs, b, leaf, v + split, n - split);
    uint8_t *dst = hash >= hash2 ? b : a;
    if (!dx_leaf_add(fs, dst, name, child, ft)) { rc = VFS_ERR_NOSPC; goto out; }
    struct dx_frame *p = &fr[depth - 1];
    dx_insert_at(p, hash2 + cont, nl);
    rc = dir_leaf_write(fs, dir_ino, dir, phys, a);
    if (rc == VFS_OK) rc = dir_leaf_write(fs, dir_ino, dir, np, b);
    if (rc == VFS_OK) rc = dx_frame_write(fs, dir_ino, dir, p);
out:
    if (v) kfree(v);
    if (a) kfree(a);
    if (b) kfree(b);
    return rc;
}

static int dx_add_entry(struct ext4 *fs, uint32_t dir_ino, struct ext4_inode *dir,
                        const char *name, uint32_t child_ino, uint8_t file_type) {
    struct dx_frame fr[4];
    uint8_t *bufs = kmalloc((size_t)fs->block_size * 6);
    if (!bufs) return VFS_ERR_NOMEM;
    for (int i = 0; i < 4; i++) fr[i].buf = bufs + (size_t)i * fs->block_size;
    uint8_t *leaf = bufs + (size_t)4 * fs->block_size;
    uint8_t *spare = bufs + (size_t)5 * fs->block_size;
    int depth = 0;
    uint8_t version = 0;
    int nlen = (int)strlen(name);
    uint32_t hash = 0;
    int rc = dx_probe(fs, dir, 0, fr, &depth, &version);
    if (rc == VFS_OK) {
        hash = dx_hash(fs, name, nlen, version);
        rc = dx_probe(fs, dir, hash, fr, &depth, &version);
    }
    uint64_t phys = 0;
    if (rc == VFS_OK) {
        rc = inode_block_map(fs, dir, dx_block_of(fr[depth - 1].buf, fr[depth - 1].coff,
                                                  fr[depth - 1].at), &phys);
        if (rc == VFS_OK && !phys) rc = VFS_ERR_IO;
    }
    if (rc == VFS_OK) rc = read_block(fs, phys, leaf);
    if (rc == VFS_OK) {
        if (dx_leaf_add(fs, leaf, name, child_ino, file_type)) {
            rc = dir_leaf_write(fs, dir_ino, dir, phys, leaf);
        } else {
            rc = dx_make_room(fs, dir_ino, dir, fr, &depth, spare);
            if (rc == VFS_OK)
                rc = dx_split_add(fs, dir_ino, dir, fr, depth, version, leaf, phys,
                                  name, hash, child_ino, file_type);
        }
    }
    kfree(bufs);
    if (rc == VFS_OK) {
        ext4_stamp(dir, E4_MTIME | E4_CTIME, ext4_now());
        rc = write_inode(fs, dir_ino, dir);
    }
    return rc;
}

/* A one-block directory that is full becomes an htree one: its entries
 * but "." and ".." move to a new leaf, block 0 becomes the index, and the
 * new leaf is split as a full one is -- Linux's way. */
static int dx_make_indexed(struct ext4 *fs, uint32_t dir_ino, struct ext4_inode *dir,
                           const char *name, uint32_t child_ino, uint8_t file_type) {
    const struct ext4_super_block *sb = (const struct ext4_super_block *)fs->sb_buf;
    uint8_t version = sb->s_def_hash_version <= DX_HASH_TEA ? sb->s_def_hash_version
                                                            : DX_HASH_HALF_MD4;
    uint8_t *root = kmalloc(fs->block_size), *leaf = kmalloc(fs->block_size);
    struct dx_ent *v = kmalloc(sizeof(*v) * (fs->block_size / 12 + 1));
    int rc = VFS_ERR_NOMEM;
    uint64_t rphys = 0, lphys = 0;
    uint32_t llblk = 0;
    if (!root || !leaf || !v) goto out;
    rc = inode_block_map(fs, dir, 0, &rphys);
    if (rc == VFS_OK && !rphys) rc = VFS_ERR_IO;
    if (rc == VFS_OK) rc = read_block(fs, rphys, root);
    if (rc != VFS_OK) goto out;
    /* "." and ".." must be the block's first two entries */
    struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)root;
    if (dot->rec_len != 12 || dot->name_len != 1) { rc = VFS_ERR_IO; goto out; }
    struct ext4_dir_entry_2 *dd = (struct ext4_dir_entry_2 *)(root + 12);
    if (dd->name_len != 2 || dd->rec_len < 12) { rc = VFS_ERR_IO; goto out; }
    /* the entries after them (past ".."'s record) move whole into the new leaf */
    uint32_t n = 0, walk = 12u + dd->rec_len, end = dir_leaf_end(fs, root);
    while (walk + 8 <= end) {
        struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(root + walk);
        if (de->rec_len < 8 || walk + de->rec_len > end) break;
        if (de->inode && de->name_len) {
            v[n].hash = dx_hash(fs, de->name, de->name_len, version);
            v[n].off  = (uint16_t)walk;
            v[n].len  = (uint16_t)((8 + de->name_len + 3) & ~3u);
            n++;
        }
        walk += de->rec_len;
    }
    rc = dir_append_block(fs, dir_ino, dir, &llblk, &lphys);
    if (rc != VFS_OK) goto out;
    dx_leaf_build(fs, leaf, root, v, n);           /* in their order, as Linux */
    dir->i_flags |= EXT4_INDEX_FL;
    /* block 0: "." and ".." over the block, the index after them */
    memset(root + 24, 0, fs->block_size - 24);
    dd->rec_len = (uint16_t)(fs->block_size - 12);
    root[0x1C] = version;
    root[0x1D] = 8;                                /* info_length */
    root[0x1E] = 0;                                /* indirect levels */
    dx_put16(root, DX_ROOT_COUNT_OFF, dx_limit_for(fs, true));
    dx_put16(root, DX_ROOT_COUNT_OFF + 2, 1);
    dx_put32(root, DX_ROOT_COUNT_OFF + 4, llblk);
    struct dx_frame fr[1] = { { 0, rphys, root, DX_ROOT_COUNT_OFF, 0 } };
    rc = dx_frame_write(fs, dir_ino, dir, &fr[0]);
    /* the leaf is split as a full one is, and the entry goes into its half */
    if (rc == VFS_OK)
        rc = dx_split_add(fs, dir_ino, dir, fr, 1, version, leaf, lphys, name,
                          dx_hash(fs, name, (int)strlen(name), version),
                          child_ino, file_type);
    if (rc == VFS_OK) {
        ext4_stamp(dir, E4_MTIME | E4_CTIME, ext4_now());
        rc = write_inode(fs, dir_ino, dir);
    }
out:
    if (root) kfree(root);
    if (leaf) kfree(leaf);
    if (v) kfree(v);
    return rc;
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
    e4c_drop(fs, fs->j_phys_first + rel);
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
        if (dev_write_block(fs, t->ents[i].blk, t->ents[i].data) != VFS_OK)
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
        for (int i = 0; i < t->n && rc == VFS_OK; i++)
            rc = dev_write_block(fs, t->ents[i].blk, t->ents[i].data);
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
                if (home < fs->total_blocks &&
                    dev_write_block(fs, home, dblk) == VFS_OK)
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

/* ---- open-handle references, so unlink can defer the release --------
 *
 * POSIX: unlink() removes the NAME. The file itself survives until the
 * last descriptor on it closes, and THAT is when the blocks come back.
 * This driver freed the blocks and the inode the moment i_links_count
 * hit zero, so an open handle was left reading blocks that had been
 * returned to the allocator -- and once the inode number was reissued,
 * reading a different file entirely.
 *
 * It is not a corner case: mkstemp() and tmpfile() both open-then-unlink
 * to get a private temp file, and so does every bash here-document.
 *
 * A fixed table rather than a list: mounts are few, this costs under a
 * kilobyte, and running out is reported rather than silently degrading
 * back to the behaviour above.
 */
static struct ext4_openref *oref_find(struct ext4 *fs, uint32_t ino) {
    for (size_t i = 0; i < EXT4_MAX_OPEN_INODES; i++)
        if (fs->openrefs[i].ino == ino && fs->openrefs[i].refs > 0)
            return &fs->openrefs[i];
    return 0;
}

static int oref_get(struct ext4 *fs, uint32_t ino) {
    struct ext4_openref *r = oref_find(fs, ino);
    if (r) { r->refs++; return VFS_OK; }
    for (size_t i = 0; i < EXT4_MAX_OPEN_INODES; i++) {
        if (fs->openrefs[i].refs == 0) {
            fs->openrefs[i].ino    = ino;
            fs->openrefs[i].refs   = 1;
            fs->openrefs[i].orphan = false;
            return VFS_OK;
        }
    }
    /* Refuse the open rather than proceed untracked: an untracked handle
     * is exactly the bug this table exists to prevent, and it would come
     * back only under load, where it is hardest to see. */
    kprintf("[ext4] open-inode table full (%d) -- refusing to open ino %u "
            "untracked\n", EXT4_MAX_OPEN_INODES, (unsigned)ino);
    return VFS_ERR_NOMEM;
}

/* Release the inode for real: blocks, then the inode itself. */
static void ext4_release_inode(struct ext4 *fs, uint32_t ino) {
    struct ext4_inode in;
    if (read_inode(fs, ino, &in) != VFS_OK) return;
    if (in.i_links_count != 0) return;         /* re-linked in the meantime */
    txn_begin(fs);
    inode_free_data(fs, ino, &in);
    in.i_size_lo = 0;
    in.i_size_hi = 0;
    in.i_dtime   = (uint32_t)ext4_now().s;
    int rc = write_inode(fs, ino, &in);
    if (rc == VFS_OK) rc = ext4_free_inode(fs, ino);
    (void)txn_finish(fs, rc);
}

static void oref_put(struct ext4 *fs, uint32_t ino) {
    struct ext4_openref *r = oref_find(fs, ino);
    if (!r) return;
    if (--r->refs > 0) return;
    bool orphan = r->orphan;
    r->ino    = 0;
    r->orphan = false;
    /* Last close of a name-less inode: NOW the space comes back. */
    if (orphan) ext4_release_inode(fs, ino);
}

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
    /* Count this handle BEFORE anyone can unlink the name out from under
     * it -- that count is what makes the deferred release possible. */
    int orc = oref_get(fs, ino);
    if (orc != VFS_OK) { kfree(fp); return orc; }
    fp->inode_no  = ino;
    fp->in        = in;
    fp->file_size = ((uint64_t)in.i_size_hi << 32) | in.i_size_lo;

    out->priv = fp;
    out->size = (size_t)fp->file_size;
    out->pos  = 0;
    out->ino  = ino;          /* the file's own inode number (finding 273) */
    out->mode = in.i_mode & VFS_MODE_PERMS;
    if (in.i_mode != 0) out->mode |= VFS_MODE_VALID;
    out->uid  = ext4_inode_uid(&in);
    out->gid  = ext4_inode_gid(&in);
    return VFS_OK;
}

static int ext4_close(struct vfs_file *f) {
    if (f && f->priv) {
        struct ext4 *fs = (struct ext4 *)f->mnt;
        struct ext4_filepriv *fp = (struct ext4_filepriv *)f->priv;
        uint32_t ino = fp->inode_no;
        kfree(fp);
        f->priv = NULL;
        /* Last close of an already-unlinked file is where its blocks
         * actually go back. */
        if (fs) oref_put(fs, ino);
    }
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
    if (fs->rdonly) return VFS_ERR_ROFS;

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
            bool fresh = false;
            /* room for what this write still needs, or as much as the
             * file already has (a file written a block at a time grows
             * into runs its own size), 8 to 64 blocks -- as Linux sizes a
             * request by the file */
            uint64_t want = (off + n + fs->block_size - 1) / fs->block_size;
            uint64_t have = (fp->file_size + fs->block_size - 1) / fs->block_size;
            if (want < have) want = have;
            if (want < 8) want = 8;
            if (want > 64) want = 64;
            rc = inode_block_map_alloc(fs, fp->inode_no, &fp->in, lblk, (uint32_t)want,
                                       &phys, &fresh);
            if (rc != VFS_OK || phys == 0) {
                if (rc == VFS_OK) rc = VFS_ERR_IO;
                break;
            }
            /* RMW for a partial block; full-block writes skip the read. A
             * block new to the file (or unwritten until now) starts from
             * zeros -- what the disk holds there is some other file's old
             * bytes (finding 271) */
            uint8_t *tmp = kmalloc(fs->block_size);
            if (!tmp) { rc = VFS_ERR_NOMEM; break; }
            if (take < fs->block_size) {
                if (fresh) memset(tmp, 0, fs->block_size);
                else rc = read_block(fs, phys, tmp);
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
            ext4_stamp(&fp->in, E4_MTIME | E4_CTIME, ext4_now());
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
    /* Born now, not at the epoch -- to the nanosecond where the inode has
     * room for the large fields, as Linux gives a new inode them */
    in.i_extra_isize = fs->want_extra_isize;
    ext4_stamp(&in, E4_ATIME | E4_MTIME | E4_CTIME | E4_CRTIME, ext4_now());
    in.i_links_count = 1;
    in.i_flags = EXT4_EXTENTS_FL;
    init_extent_header(&in);

    rc = write_inode_slot(fs, new_ino, &in, true);
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }
    rc = dir_add_entry(fs, parent_ino, leaf, new_ino, EXT4_FT_REG_FILE);
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }
    return VFS_OK;
}

static int ext4_create(void *mnt, const char *path,
                       uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (fs->rdonly) return VFS_ERR_ROFS;
    txn_begin(fs);
    return txn_finish(fs, do_create(fs, path, uid, gid, mode));
}

/* An entry of a directory other than "." and "..": the directory is not
 * empty, and the walk stops. */
static int dir_any_entry(void *user, uint32_t ino, uint8_t ft,
                         const char *name, uint8_t len) {
    (void)ft;
    if (ino == 0) return VFS_OK;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return VFS_OK;
    *(bool *)user = true;
    return VFS_ERR_NOENT;
}

/* rmdir: an empty directory's entry goes from its parent, the parent loses
 * the link its ".." was, and the directory's blocks and inode are freed --
 * mkdir undone. A directory with entries is VFS_ERR_INVAL, which rmdir(2)
 * reports as ENOTEMPTY. The VFS's unlink comes here for both (sys_unlink
 * refuses a directory before it; rmdir_path a file); the driver said
 * EISDIR, so no directory on an ext4 volume could be removed (`rm -rf`
 * left them all; finding 255's probe). */
static int do_rmdir(struct ext4 *fs, uint32_t parent_ino, const char *leaf,
                    uint32_t ino) {
    struct ext4_inode in;
    int rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;
    bool full = false;
    rc = dir_walk(fs, &in, dir_any_entry, &full);
    if (rc != VFS_OK) return rc;
    if (full) return VFS_ERR_INVAL;

    rc = dir_remove_entry(fs, parent_ino, leaf);
    if (rc != VFS_OK) return rc;
    struct ext4_inode parent;
    if (read_inode(fs, parent_ino, &parent) == VFS_OK) {
        if (parent.i_links_count > 2) parent.i_links_count--;
        write_inode(fs, parent_ino, &parent);
    }
    inode_free_data(fs, ino, &in);
    in.i_links_count = 0;
    in.i_size_lo = 0;
    in.i_size_hi = 0;
    struct ext4_time now = ext4_now();
    ext4_stamp(&in, E4_CTIME, now);
    in.i_dtime = (uint32_t)now.s;
    rc = write_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    adj_used_dirs(fs, (ino - 1) / fs->inodes_per_group, -1);
    return ext4_free_inode(fs, ino);      /* flushes the GDT and superblock */
}

static int do_unlink(struct ext4 *fs, const char *path) {
    uint32_t parent_ino;
    char leaf[VFS_NAME_MAX];
    int rc = path_parent(fs, path, &parent_ino, leaf, sizeof(leaf));
    if (rc != VFS_OK) return rc;

    uint32_t ino; uint8_t ftype;
    rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    if (ftype == EXT4_FT_DIR) return do_rmdir(fs, parent_ino, leaf, ino);

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    rc = dir_remove_entry(fs, parent_ino, leaf);
    if (rc != VFS_OK) return rc;

    if (in.i_links_count > 0) in.i_links_count--;
    ext4_stamp(&in, E4_CTIME, ext4_now());      /* a link gone, as Linux's */
    if (in.i_links_count == 0) {
        struct ext4_openref *r = oref_find(fs, ino);
        if (r) {
            /* Somebody still has it open. The NAME is gone -- that part
             * already happened above -- but the bytes belong to those
             * handles until the last of them closes. Persist the zero
             * link count and leave the data alone; oref_put() finishes
             * the job. */
            r->orphan = true;
            return write_inode(fs, ino, &in);
        }
        inode_free_data(fs, ino, &in);
        in.i_size_lo = 0;
        in.i_size_hi = 0;
        in.i_dtime   = (uint32_t)ext4_now().s;
        write_inode(fs, ino, &in);
        ext4_free_inode(fs, ino);
    } else {
        write_inode(fs, ino, &in);
    }
    return VFS_OK;
}

static int ext4_unlink(void *mnt, const char *path) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (fs->rdonly) return VFS_ERR_ROFS;
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
    ext4_stamp(&in, E4_CTIME, ext4_now());
    return write_inode(fs, ino, &in);
}

static int ext4_link(void *mnt, const char *oldpath, const char *newpath) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (fs->rdonly) return VFS_ERR_ROFS;
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
    rc = dir_remove_entry(fs, op_ino, op_leaf);
    if (rc != VFS_OK) return rc;
    /* the inode moved is changed too: its ctime, as Linux's (finding 255) */
    struct ext4_inode in;
    rc = read_inode(fs, src_ino, &in);
    if (rc != VFS_OK) return rc;
    ext4_stamp(&in, E4_CTIME, ext4_now());
    return write_inode(fs, src_ino, &in);
}

static int ext4_rename(void *mnt, const char *oldpath, const char *newpath) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (fs->rdonly) return VFS_ERR_ROFS;
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
    /* Born now, not at the epoch (see do_create) */
    in.i_extra_isize = fs->want_extra_isize;
    ext4_stamp(&in, E4_ATIME | E4_MTIME | E4_CTIME | E4_CRTIME, ext4_now());
    in.i_links_count = 2;            /* self + "." */
    in.i_flags = EXT4_EXTENTS_FL;
    init_extent_header(&in);

    uint64_t dir_phys = 0;
    bool fresh;
    rc = inode_block_map_alloc(fs, new_ino, &in, 0, 1, &dir_phys, &fresh);   /* extent block 0 */
    if (rc != VFS_OK) { ext4_free_inode(fs, new_ino); return rc; }
    in.i_size_lo = fs->block_size;

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) rc = VFS_ERR_NOMEM;
    if (rc == VFS_OK) {
        /* "." and ".." over the block, the tail after them (finding 271) */
        dir_leaf_init(fs, blk);
        uint32_t room = ((struct ext4_dir_entry_2 *)blk)->rec_len;
        struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)blk;
        dot->inode = new_ino; dot->rec_len = 12; dot->name_len = 1;
        dot->file_type = EXT4_FT_DIR; memcpy((char *)dot + 8, ".", 1);
        struct ext4_dir_entry_2 *dd = (struct ext4_dir_entry_2 *)(blk + 12);
        dd->inode = parent_ino; dd->rec_len = (uint16_t)(room - 12);
        dd->name_len = 2; dd->file_type = EXT4_FT_DIR; memcpy((char *)dd + 8, "..", 2);
        rc = dir_leaf_write(fs, new_ino, &in, dir_phys, blk);
        kfree(blk);
    }
    if (rc == VFS_OK) rc = write_inode_slot(fs, new_ino, &in, true);
    if (rc == VFS_OK) rc = dir_add_entry(fs, parent_ino, leaf, new_ino, EXT4_FT_DIR);
    if (rc != VFS_OK) {
        /* undone: nothing of it left allocated on a volume written in place */
        ext4_free_block(fs, (uint32_t)dir_phys);
        ext4_free_inode(fs, new_ino);
        return rc;
    }

    /* Parent gains a link (the new dir's ".."), and the group's used-dirs
     * count rises. An htree directory past Linux's 65000 links counts one
     * (dir_nlink), as Linux's. */
    struct ext4_inode parent;
    if (read_inode(fs, parent_ino, &parent) == VFS_OK) {
        if (parent.i_links_count != 1) parent.i_links_count++;
        if (parent.i_links_count > 65000 && dir_is_dx(fs, &parent))
            parent.i_links_count = 1;
        write_inode(fs, parent_ino, &parent);
    }
    uint32_t g = (new_ino - 1) / fs->inodes_per_group;
    adj_used_dirs(fs, g, +1);
    flush_group(fs, g);
    return VFS_OK;
}

static int ext4_mkdir(void *mnt, const char *path,
                      uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (fs->rdonly) return VFS_ERR_ROFS;
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



/* The shared core of truncate(2) and ftruncate(2). `in` is updated in
 * place so an open handle's cached copy stays true. */
static int ext4_do_truncate(struct ext4 *fs, uint32_t ino,
                            struct ext4_inode *in, uint64_t len) {
    if ((in->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;
    if (fs->rdonly) return VFS_ERR_ROFS;
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
            rc = inode_trim_from(fs, ino, in, first_free);
        }
    }
    /* Growing allocates nothing. An unmapped block is a hole and
     * inode_read() already returns zeroes for one, so a larger i_size is
     * the entire job -- the file just becomes sparse, as it would on
     * Linux. */

    if (rc == VFS_OK) {
        in->i_size_lo = (uint32_t)(len & 0xFFFFFFFFu);
        in->i_size_hi = (uint32_t)(len >> 32);
        ext4_stamp(in, E4_MTIME | E4_CTIME, ext4_now());
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
    if (fs->rdonly) return VFS_ERR_ROFS;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    apply(&in, a, b);
    if (stamp_ctime) ext4_stamp(&in, E4_CTIME, ext4_now());

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

static int ext4_chmod(void *mnt, const char *path, uint32_t mode) {
    return ext4_meta_update((struct ext4 *)mnt, path, apply_chmod,
                            mode, 0, true);
}
static int ext4_chown(void *mnt, const char *path, uint32_t uid,
                      uint32_t gid) {
    return ext4_meta_update((struct ext4 *)mnt, path, apply_chown,
                            uid, gid, true);
}
static int ext4_utimens(void *mnt, const char *path, uint64_t mtime,
                        uint32_t mtime_ns, uint64_t atime, uint32_t atime_ns) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (fs->rdonly) return VFS_ERR_ROFS;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    struct ext4_time m = { (int64_t)mtime, mtime_ns };
    struct ext4_time a = { (int64_t)atime, atime_ns };
    ext4_stamp(&in, E4_MTIME, m);
    ext4_stamp(&in, E4_ATIME, a);
    /* setting a time is a change: ctime to now, as Linux's (finding 255;
     * it was left alone) */
    ext4_stamp(&in, E4_CTIME, ext4_now());
    txn_begin(fs);
    rc = write_inode(fs, ino, &in);
    return txn_finish(fs, rc);
}
static int ext4_utimes(void *mnt, const char *path, uint64_t mtime,
                       uint64_t atime) {
    return ext4_utimens(mnt, path, mtime, 0, atime, 0);
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
    struct ext4_time t;
    t = ext4_time_get(in.i_mtime, in.i_mtime_extra, EXT4_HAS_EXTRA(&in, i_mtime_extra));
    out->mtime = (uint64_t)t.s; out->mtime_ns = t.ns;
    t = ext4_time_get(in.i_atime, in.i_atime_extra, EXT4_HAS_EXTRA(&in, i_atime_extra));
    out->atime = (uint64_t)t.s; out->atime_ns = t.ns;
    /* ...and the change time, which was never reported at all (%Z said
     * the epoch for every file) */
    t = ext4_time_get(in.i_ctime, in.i_ctime_extra, EXT4_HAS_EXTRA(&in, i_ctime_extra));
    out->ctime = (uint64_t)t.s; out->ctime_ns = t.ns;
    out->nlink = in.i_links_count;
    /* every file's own number, as Linux's st_ino: with none, two files on
     * one volume were one to every tool that compares them -- `cat a >> b`
     * said "input file is output file" (finding 273) */
    out->ino = ino;
    out->mode = in.i_mode & VFS_MODE_PERMS;
    if (in.i_mode != 0) out->mode |= VFS_MODE_VALID;
    return VFS_OK;
}

/* fsync(2): the block cache is WRITE-BACK, so a successful write is not
 * yet on the medium. Without this, fsync() returned 0 while the bytes sat
 * in RAM -- which is the difference between "saved" and "saved until you
 * unplug it". */
static int ext4_sync(void *mnt) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (!fs || !fs->dev) return VFS_OK;
    extern void bcache_sync(struct blk_dev *dev);
    bcache_sync(fs->dev);
    return blk_flush(fs->dev) == 0 ? VFS_OK : VFS_ERR_IO;
}

static int ext4_umount(void *mnt);

static const struct vfs_ops ext4_ops = {
    .fsname   = "ext4",          /* batch 31: /proc/mounts */
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
    .utimens   = ext4_utimens,  /* finding 255 */
    .truncate  = ext4_truncate,
    .ftruncate = ext4_ftruncate,
    .link      = ext4_link,
    .statfs    = ext4_statfs,   /* Phase H */
    .sync      = ext4_sync,
    .umount    = ext4_umount,   /* finding 271 */
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
    blk_watch_remove(e4c_watch, fs);
    e4c_free(fs);
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
    /* a new inode's large fields: what the volume asks for, as Linux's
     * ext4 gives (32 bytes when it says nothing), within the inode's room */
    if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
        uint32_t w = sb->s_want_extra_isize ? sb->s_want_extra_isize : 32u;
        uint32_t room = fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE;
        uint32_t known = (uint32_t)sizeof(struct ext4_inode) - EXT4_GOOD_OLD_INODE_SIZE;
        if (w > room) w = room;
        if (w > known) w = known;
        fs->want_extra_isize = (uint16_t)w;
    }
    fs->feature_incompat = sb->s_feature_incompat;
    if (!g_crc_tabs) crc_tabs_init();
    fs->csum     = (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
    fs->gdt_csum = !fs->csum &&
                   (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) != 0;
    memcpy(fs->uuid, sb->s_uuid, 16);
    fs->csum_seed = (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) ?
                    sb->s_checksum_seed : crc32c(~0u, sb->s_uuid, 16);
    /* Linux refuses a volume whose superblock sum is wrong, or summed some
     * other way (finding 271) */
    if (fs->csum && sb->s_checksum_type != 1) {
        kprintf("[ext4] VFS: Found ext4 filesystem with unknown checksum algorithm.\n");
        kfree(sb_raw); kfree(fs);
        return NULL;
    }
    if (fs->csum && sb->s_checksum != sb_csum(sb)) {
        kprintf("[ext4] VFS: Found ext4 filesystem with invalid superblock checksum.  Run e2fsck?\n");
        kfree(sb_raw); kfree(fs);
        return NULL;
    }
    /* group descriptors past the first meta group are not where this
     * driver reads them */
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) {
        kprintf("[ext4] refuse mount: META_BG not supported\n");
        kfree(sb_raw); kfree(fs);
        return NULL;
    }
    fs->incompat_bad = sb->s_feature_incompat &
        ~(EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_RECOVER |
          EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |
          EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED);
    fs->ro_compat_bad = sb->s_feature_ro_compat &
        ~(EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
          EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM |
          EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
          EXT4_FEATURE_RO_COMPAT_METADATA_CSUM);
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
    if (!fs->blk_buf || !fs->blk_buf2 || !fs->gdt_buf || !fs->sb_buf ||
        !e4c_init(fs)) {
        kfree(sb_raw);
        ext4_free(fs);
        return NULL;
    }
    /* a write to the disk as a file drops what the cache held of it */
    fs->disk = blk_disk_of(dev, &fs->disk_off);
    (void)blk_watch_add(e4c_watch, fs);
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

#define EXT4_VALID_FS 0x0001u   /* s_state: unmounted cleanly */

int ext4_mount_ro(const char *mount_point, struct blk_dev *dev, bool rdonly) {
    if (!mount_point || !dev) return VFS_ERR_INVAL;

    struct ext4 *fs = ext4_setup(dev);
    if (!fs) return VFS_ERR_INVAL;

    /* a feature this driver cannot keep: read-only, or not at all -- in
     * Linux's words (finding 271) */
    if (!rdonly && (fs->ro_compat_bad || fs->incompat_bad)) {
        kprintf("[ext4] couldn't mount RDWR because of unsupported optional features (%x)\n",
                fs->ro_compat_bad ? fs->ro_compat_bad : fs->incompat_bad);
        ext4_free(fs);
        return VFS_ERR_INVAL;
    }
    fs->rdonly = rdonly;
    if (!rdonly) {
        /* Mounted read-write: counted, dated -- and, with no journal this
         * driver drives, marked not clean until the umount, so a crash
         * in between leaves Linux the sign to check it, as Linux's own
         * no-journal mounts do. */
        struct ext4_super_block *sb = (struct ext4_super_block *)fs->sb_buf;
        fs->state_at_mount = sb->s_state;
        if (!(sb->s_state & EXT4_VALID_FS))
            kprintf("[ext4] warning: mounting unchecked fs, running e2fsck is recommended\n");
        sb->s_mnt_count++;
        sb->s_mtime = (uint32_t)ext4_now().s;
        if (!fs->has_journal) {
            sb->s_state &= (uint16_t)~EXT4_VALID_FS;
            fs->state_cleared = true;
        }
        (void)flush_superblock_direct(fs);
    }

    int rc = vfs_mount_flags(mount_point, &ext4_ops, fs, rdonly ? VFS_MNT_RDONLY : 0);
    if (rc == VFS_OK && dev->name) {
        char sp[48];                   /* its /dev node's path (finding 198) */
        extern bool devfs_blk_path(const struct blk_dev *d, char *out, size_t cap);
        vfs_mount_set_source(mount_point, devfs_blk_path(dev, sp, sizeof sp) ? sp : dev->name);
    }
    if (rc != VFS_OK) {
        kprintf("[ext4] vfs_mount('%s') failed: %d\n", mount_point, rc);
        ext4_free(fs);
        return rc;
    }

    kprintf("[ext4] mounted '%s' on %s: %u blocks x %u B "
            "(%u KiB total, %u groups, inode_size=%u, first_ino=%u, %s, r%s%s)\n",
            mount_point, dev->name ? dev->name : "(anon)",
            (unsigned)fs->total_blocks, fs->block_size,
            (unsigned)((fs->total_blocks * fs->block_size) / 1024u),
            fs->group_count, fs->inode_size, fs->first_ino,
            (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) ?
                "extents" : "legacy-ptrs",
            rdonly ? "o" : "w", fs->has_journal ? "+jbd2" : "");
    return VFS_OK;
}

int ext4_mount(const char *mount_point, struct blk_dev *dev) {
    return ext4_mount_ro(mount_point, dev, false);
}

/* umount: the volume marked clean again (as it was at the mount), the
 * cache written out, and the mount's memory freed -- unless a file of it
 * is still open (a lazy unmount), which keeps both: those handles still
 * use it, and a volume still being written is not clean. */
static int ext4_umount(void *mnt) {
    struct ext4 *fs = (struct ext4 *)mnt;
    if (!fs) return VFS_OK;
    for (size_t i = 0; i < EXT4_MAX_OPEN_INODES; i++)
        if (fs->openrefs[i].refs > 0) return VFS_OK;
    if (fs->state_cleared) {
        struct ext4_super_block *sb = (struct ext4_super_block *)fs->sb_buf;
        sb->s_state = fs->state_at_mount;
        (void)flush_superblock_direct(fs);
    }
    (void)ext4_sync(fs);
    kprintf("[ext4] umount %s: cache %lu hits, %lu misses; device %lu block reads, "
            "%lu block writes\n", fs->dev && fs->dev->name ? fs->dev->name : "(anon)",
            (unsigned long)fs->bc.hits, (unsigned long)fs->bc.misses,
            (unsigned long)fs->dev_reads, (unsigned long)fs->dev_writes);
    ext4_free(fs);
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
    sb->s_min_extra_isize     = 32;     /* the large inode's fields (255) */
    sb->s_want_extra_isize    = 32;
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
        /* born when the volume is, to the nanosecond, as mke2fs's root
         * (it had no times: 1970) */
        struct ext4_time born = ext4_now();
        root->i_extra_isize = 32;
        ext4_stamp(root, E4_ATIME | E4_MTIME | E4_CTIME | E4_CRTIME, born);
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
            ji->i_extra_isize = 32;
            ext4_stamp(ji, E4_ATIME | E4_MTIME | E4_CTIME | E4_CRTIME, born);
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
