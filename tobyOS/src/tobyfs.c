/* tobyfs.c -- writable on-disk filesystem (milestone 6).
 *
 * Layout (matches include/tobyos/tobyfs.h verbatim and is shared with
 * tools/mkfs_tobyfs.c):
 *
 *   block 0         superblock
 *   block 1         inode bitmap   (1 bit / inode)
 *   block 2         data  bitmap   (1 bit / data block, includes meta blocks)
 *   blocks 3..10    inode table
 *   blocks 11..991  data blocks
 *   blocks 992..1023 write-ahead journal (32 blocks)
 *
 * Metadata-mutating operations (create, mkdir, unlink, write) are
 * wrapped in write-ahead journal transactions so a crash mid-write
 * can be recovered on the next mount. The journal lives in the last
 * 32 blocks of the image and holds at most one active transaction
 * (up to 16 unique blocks per txn).
 *
 * Path resolution is iterative: split on '/', look up each component
 * in the parent's data blocks (each containing TFS_DIRENTS_PER_BLOCK
 * 64-byte slots). No support for "." or ".." -- callers hand us
 * canonical absolute paths.
 */

#include <tobyos/tobyfs.h>
#include <tobyos/blk.h>
#include <tobyos/bcache.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>
#include <tobyos/slog.h>     /* M28E: emit fscheck verdicts via slog */
#include <tobyos/mmap.h>     /* slice 23: shm_cache_detach_ino on inode reuse */

_Static_assert(sizeof(struct tfs_inode_disk)  == TFS_INODE_SIZE,  "tobyfs inode size");
_Static_assert(sizeof(struct tfs_dirent_disk) == TFS_DIRENT_SIZE, "tobyfs dirent size");

struct tobyfs;
/* Forward declaration: defined further down in the M28E section.
 * Needed because tobyfs_mount() (defined first) calls into it. */
static int check_core(struct tobyfs *fs, struct tobyfs_check *out);

struct tobyfs {
    struct blk_dev        *dev;
    struct tfs_superblock  sb;
    /* Bitmaps are cached in RAM, kept in lockstep with the on-disk copies.
     * They now SPAN MULTIPLE BLOCKS for large volumes (a 4 KiB block holds
     * 32768 bits => 32768 blocks/inodes), so they are heap-allocated and we
     * flush only the single block containing a mutated bit. ibitmap_blocks /
     * dbitmap_blocks are the spans; ibitmap_blk / dbitmap_blk are the on-disk
     * start blocks (mirrors of the superblock fields, cached for speed). */
    uint8_t               *ibitmap;
    uint8_t               *dbitmap;
    uint32_t               ibitmap_blocks;   /* span in 4 KiB blocks */
    uint32_t               dbitmap_blocks;
    uint32_t               ibitmap_blk;      /* on-disk start block */
    uint32_t               dbitmap_blk;
    uint32_t               usable_blocks;    /* journal_start or total_blocks */
    /* Bumped by alloc_inode each time a number is issued; stamped into
     * vfs_file.ino_gen at open so (ino, ino_gen) identifies a FILE rather than
     * a reusable number. In-memory, per-mount. NULL = alloc failed, in which
     * case gen stays 0 and shared mappings fall back to fd-only identity. */
    uint32_t              *ino_gen;

    /* Write-ahead journal state.  journal_start == 0 means the
     * image was formatted without a journal (pre-journal disk). */
    uint32_t               journal_start;
    uint32_t               journal_size;
    uint32_t               next_txn_id;
    bool                   in_transaction;
    int                    txn_count;
    uint32_t               txn_blocks[TFS_TXN_MAX_BLOCKS];
    uint8_t                txn_data[TFS_TXN_MAX_BLOCKS][TFS_BLOCK_SIZE];
};

/* -------- block I/O helpers (block N <-> sectors N*8 .. N*8+7) -------- */

static int read_block(struct tobyfs *fs, uint32_t blk, void *buf) {
    uint64_t lba = (uint64_t)blk * TFS_SECTORS_PER_BLOCK;
    void *cached = bcache_get(fs->dev, lba);
    if (!cached) return VFS_ERR_IO;
    memcpy(buf, cached, TFS_BLOCK_SIZE);
    bcache_release(fs->dev, lba);
    return 0;
}

/* Linux slice 6: wall-clock seconds for inode timestamps.
 *
 * These fields used to be filled with pit_ticks() -- time since BOOT, which
 * resets every reboot and is not a timestamp at all despite looking like one.
 * lx_realtime_ns() is the same anchored realtime clock CLOCK_REALTIME uses,
 * so on-disk times now agree with what userspace sees. */
static uint32_t tfs_now_secs(void) {
    extern uint64_t lx_realtime_ns(uint64_t mono_ns);
    extern uint64_t perf_now_ns(void);
    return (uint32_t)(lx_realtime_ns(perf_now_ns()) / 1000000000ull);
}

static int write_block_raw(struct tobyfs *fs, uint32_t blk, const void *buf) {
    uint64_t lba = (uint64_t)blk * TFS_SECTORS_PER_BLOCK;
    void *cached = bcache_get(fs->dev, lba);
    if (!cached) return VFS_ERR_IO;
    memcpy(cached, buf, TFS_BLOCK_SIZE);
    bcache_mark_dirty(fs->dev, lba);
    bcache_release(fs->dev, lba);
    return 0;
}

/* Buffer a block write into the active transaction.  Deduplicates
 * repeated writes to the same block (e.g. the data bitmap during a
 * multi-block free) so a single txn slot is consumed.
 *
 * CRITICAL: also write the data through to the block cache immediately.
 * Within one transaction, code routinely does read-modify-write on the
 * SAME block more than once -- e.g. tobyfs_create() updates a child inode
 * and then the parent-directory inode, which (for low inode numbers) live
 * in the same inode-table block. read_block() reads from bcache, so if the
 * buffered write weren't reflected there, the second read would see stale
 * pre-transaction data and the re-write would CLOBBER the first change in
 * the dedup slot. (This silently corrupted freshly-created inodes -- the
 * child inode reverted to type=FREE.) The journal copy still provides crash
 * durability; bcache is just the live, in-RAM view both reads and the final
 * checkpoint already use. */
static int journal_write(struct tobyfs *fs, uint32_t blk, const void *data) {
    /* Keep the live cache consistent with the buffered write. */
    (void)write_block_raw(fs, blk, data);

    for (int i = 0; i < fs->txn_count; i++) {
        if (fs->txn_blocks[i] == blk) {
            memcpy(fs->txn_data[i], data, TFS_BLOCK_SIZE);
            return 0;
        }
    }
    if (fs->txn_count >= (int)TFS_TXN_MAX_BLOCKS)
        return VFS_ERR_NOSPC;
    fs->txn_blocks[fs->txn_count] = blk;
    memcpy(fs->txn_data[fs->txn_count], data, TFS_BLOCK_SIZE);
    fs->txn_count++;
    return 0;
}

static int write_block(struct tobyfs *fs, uint32_t blk, const void *buf) {
    if (fs->in_transaction)
        return journal_write(fs, blk, buf);
    return write_block_raw(fs, blk, buf);
}

/* -------- write-ahead journal -------- */

/* Crash-injection knob for the crash-consistency self-test. When set,
 * journal_commit() stops at the chosen phase WITHOUT clearing the on-disk
 * journal header, modelling a power loss so the next mount must recover.
 * The in-RAM checkpoint lives in the (write-back) block cache, so the test
 * models the lost cache by bcache_invalidate()'ing before it remounts. */
enum {
    TFS_CRASH_NONE = 0,
    TFS_CRASH_BEFORE_COMMIT,   /* header+data logged, no commit marker */
    TFS_CRASH_AFTER_COMMIT,    /* committed, nothing checkpointed to disk */
};
static int g_tfs_crash_phase = TFS_CRASH_NONE;

static void journal_begin(struct tobyfs *fs) {
    if (fs->journal_start == 0) return;
    fs->in_transaction = true;
    fs->txn_count = 0;
    fs->next_txn_id++;
}

static void journal_abort(struct tobyfs *fs) {
    fs->in_transaction = false;
    fs->txn_count = 0;
    /* journal_write() writes buffered blocks through to bcache so in-txn
     * reads stay consistent. On abort those changes were never committed to
     * the journal, so drop the cache to force a clean re-read from disk and
     * discard the partial transaction. Also resync the in-RAM bitmaps, which
     * may have been mutated by the aborted allocator calls. */
    bcache_invalidate(fs->dev);
    for (uint32_t s = 0; s < fs->ibitmap_blocks; s++)
        (void)read_block(fs, fs->ibitmap_blk + s,
                         fs->ibitmap + (size_t)s * TFS_BLOCK_SIZE);
    for (uint32_t s = 0; s < fs->dbitmap_blocks; s++)
        (void)read_block(fs, fs->dbitmap_blk + s,
                         fs->dbitmap + (size_t)s * TFS_BLOCK_SIZE);
}

/* Write the buffered transaction to the journal area, then checkpoint
 * (replay) all blocks to their real destinations.  Uses blk_write()
 * for journal I/O so the log is synchronous / durable before the
 * checkpoint begins. */
static int journal_commit(struct tobyfs *fs) {
    if (!fs->in_transaction) return VFS_OK;
    int count = fs->txn_count;
    if (count == 0) { fs->in_transaction = false; return VFS_OK; }

    uint8_t buf[TFS_BLOCK_SIZE];
    uint32_t jbase = fs->journal_start + 1;  /* block after journal super */

    /* 1. Write transaction header. */
    memset(buf, 0, sizeof(buf));
    struct tfs_txn_header *hdr = (struct tfs_txn_header *)buf;
    hdr->magic       = TFS_TXN_BEGIN_MAGIC;
    hdr->txn_id      = fs->next_txn_id;
    hdr->block_count = (uint32_t)count;
    for (int i = 0; i < count; i++)
        hdr->dest_blocks[i] = fs->txn_blocks[i];

    if (blk_write(fs->dev, (uint64_t)jbase * TFS_SECTORS_PER_BLOCK,
                  TFS_SECTORS_PER_BLOCK, buf) != 0) {
        journal_abort(fs);
        return VFS_ERR_IO;
    }

    /* 2. Write data blocks into the journal. */
    for (int i = 0; i < count; i++) {
        uint64_t lba = (uint64_t)(jbase + 1 + (uint32_t)i) * TFS_SECTORS_PER_BLOCK;
        if (blk_write(fs->dev, lba, TFS_SECTORS_PER_BLOCK,
                      fs->txn_data[i]) != 0)
            goto fail_clear;
    }

    /* Crash injection: power loss after the log's data is durable but
     * before the commit marker. Recovery must find no commit -> discard. */
    if (g_tfs_crash_phase == TFS_CRASH_BEFORE_COMMIT) {
        fs->in_transaction = false; fs->txn_count = 0;
        return VFS_ERR_IO;
    }

    /* 3. Write commit marker. */
    memset(buf, 0, sizeof(buf));
    struct tfs_txn_commit *cmt = (struct tfs_txn_commit *)buf;
    cmt->magic  = TFS_TXN_COMMIT_MAGIC;
    cmt->txn_id = fs->next_txn_id;
    {
        uint64_t lba = (uint64_t)(jbase + 1 + (uint32_t)count) * TFS_SECTORS_PER_BLOCK;
        if (blk_write(fs->dev, lba, TFS_SECTORS_PER_BLOCK, buf) != 0)
            goto fail_clear;
    }

    /* Crash injection: power loss after the commit marker is durable but
     * before any block is checkpointed home. Recovery must replay. */
    if (g_tfs_crash_phase == TFS_CRASH_AFTER_COMMIT) {
        fs->in_transaction = false; fs->txn_count = 0;
        return VFS_ERR_IO;
    }

    /* WAL barrier: force the log (header + data + commit marker) out of
     * the DRIVE's volatile cache before any checkpoint write can reach
     * the device. Without this a power cut could land checkpoint blocks
     * on media while the commit marker is still lost in drive cache --
     * partial home writes with a rolled-back journal = corruption. */
    (void)blk_flush(fs->dev);

    /* 4. Checkpoint: write every buffered block to its real location. */
    for (int i = 0; i < count; i++)
        (void)write_block_raw(fs, fs->txn_blocks[i], fs->txn_data[i]);

    /* Durability barrier. write_block_raw goes through the WRITE-BACK block
     * cache, so the checkpointed data currently lives only in RAM. Flush it to
     * the physical device BEFORE clearing the journal below -- otherwise a
     * power loss after the (on-disk) journal is cleared loses the committed
     * data with no log left to replay, which silently defeats the whole
     * journal (observed: a saved file vanishing across a reboot). A crash
     * between this flush and the clear just re-replays the txn (idempotent). */
    bcache_sync(fs->dev);

    /* 5. Clear the journal header (marks txn as fully checkpointed). */
    memset(buf, 0, sizeof(buf));
    (void)blk_write(fs->dev, (uint64_t)jbase * TFS_SECTORS_PER_BLOCK,
                    TFS_SECTORS_PER_BLOCK, buf);

    fs->in_transaction = false;
    fs->txn_count = 0;
    return VFS_OK;

fail_clear:
    memset(buf, 0, sizeof(buf));
    (void)blk_write(fs->dev, (uint64_t)jbase * TFS_SECTORS_PER_BLOCK,
                    TFS_SECTORS_PER_BLOCK, buf);
    journal_abort(fs);
    return VFS_ERR_IO;
}

/* Scan the journal at mount time. If a committed-but-uncheckpointed
 * transaction is found, replay it to the real destinations. */
static void journal_recover(struct tobyfs *fs) {
    if (fs->journal_start == 0) return;

    uint8_t buf[TFS_BLOCK_SIZE];
    uint32_t jbase = fs->journal_start + 1;

    if (read_block(fs, jbase, buf) != 0) return;

    struct tfs_txn_header *hdr = (struct tfs_txn_header *)buf;
    if (hdr->magic != TFS_TXN_BEGIN_MAGIC) return;
    if (hdr->block_count == 0 || hdr->block_count > TFS_TXN_MAX_BLOCKS)
        goto clear;

    /* Check for a matching commit marker. */
    {
        uint32_t cblk = jbase + 1 + hdr->block_count;
        if (cblk >= fs->journal_start + fs->journal_size) goto clear;

        uint8_t cbuf[TFS_BLOCK_SIZE];
        if (read_block(fs, cblk, cbuf) != 0) goto clear;

        struct tfs_txn_commit *cmt = (struct tfs_txn_commit *)cbuf;
        if (cmt->magic != TFS_TXN_COMMIT_MAGIC ||
            cmt->txn_id != hdr->txn_id)
            goto clear;
    }

    kprintf("[tobyfs] journal: replaying txn %u (%u blocks)\n",
            hdr->txn_id, hdr->block_count);
    fs->next_txn_id = hdr->txn_id;

    for (uint32_t i = 0; i < hdr->block_count; i++) {
        uint8_t data[TFS_BLOCK_SIZE];
        if (read_block(fs, jbase + 1 + i, data) != 0) {
            kprintf("[tobyfs] journal: read failed at journal blk %u\n",
                    jbase + 1 + i);
            break;
        }
        if (write_block_raw(fs, hdr->dest_blocks[i], data) != 0) {
            kprintf("[tobyfs] journal: replay write failed at blk %u\n",
                    hdr->dest_blocks[i]);
            break;
        }
    }

clear:
    /* Flush any replayed blocks to the device, THEN clear the on-disk journal
     * durably (blk_write, not the write-back cache) -- same ordering as
     * journal_commit, so a crash mid-recovery re-replays (idempotent) rather
     * than clearing the log while the recovered data is still only in RAM. */
    bcache_sync(fs->dev);
    memset(buf, 0, sizeof(buf));
    (void)blk_write(fs->dev, (uint64_t)jbase * TFS_SECTORS_PER_BLOCK,
                    TFS_SECTORS_PER_BLOCK, buf);
}

/* -------- bitmap helpers (operate on the in-memory copy + flush) -------- */

static bool bit_get(const uint8_t *bm, uint32_t i) {
    return (bm[i >> 3] >> (i & 7)) & 1;
}
static void bit_set(uint8_t *bm, uint32_t i) {
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static void bit_clear(uint8_t *bm, uint32_t i) {
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

/* Flush only the single on-disk bitmap block that holds bit `i`. The cached
 * bitmap now spans multiple 4 KiB blocks, so we can't blindly rewrite "the"
 * bitmap block: locate the sub-block (i / 32768), then write that one block
 * from the matching offset in the cached buffer. `start_blk` is the on-disk
 * start of the bitmap region; `cache` is the in-RAM span. */
static int flush_bitmap_bit(struct tobyfs *fs, uint8_t *cache,
                            uint32_t start_blk, uint32_t i) {
    uint32_t sub = i / TFS_BITS_PER_BLOCK;          /* which bitmap block */
    return write_block(fs, start_blk + sub,
                       cache + (size_t)sub * TFS_BLOCK_SIZE);
}

static int alloc_inode(struct tobyfs *fs, uint32_t *out_ino) {
    /* Inode 0 is reserved (means "invalid" everywhere in the FS). */
    for (uint32_t i = 1; i < fs->sb.inode_count; i++) {
        if (!bit_get(fs->ibitmap, i)) {
            bit_set(fs->ibitmap, i);
            int rc = flush_bitmap_bit(fs, fs->ibitmap, fs->ibitmap_blk, i);
            if (rc != 0) { bit_clear(fs->ibitmap, i); return VFS_ERR_IO; }
            *out_ino = i;
            /* New incarnation of this number: anything still keyed on the
             * previous one (a live shared mapping) must not match it. */
            if (fs->ino_gen) fs->ino_gen[i]++;
#ifdef CHROMIUM_BOOT
            {   static int c = 0;
                if (c < 120) { c++; kprintf("[ino] alloc ino=%u gen=%u\n",
                                            i, fs->ino_gen ? fs->ino_gen[i] : 0); } }
#endif
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPC;
}

static int free_inode(struct tobyfs *fs, uint32_t ino) {
    if (ino == 0 || ino >= fs->sb.inode_count) return VFS_ERR_INVAL;
#ifdef CHROMIUM_BOOT
    {   static int c = 0;
        if (c < 120) { c++; kprintf("[ino] free  ino=%u\n", ino); } }
#endif
    /* No shared-mapping bookkeeping needed here: the number is about to become
     * reusable, but alloc_inode bumps its incarnation counter, so the next file
     * to receive it keys a DIFFERENT shared region than this one's mappers. */
    bit_clear(fs->ibitmap, ino);
    return flush_bitmap_bit(fs, fs->ibitmap, fs->ibitmap_blk, ino)
           == 0 ? VFS_OK : VFS_ERR_IO;
}

static int alloc_data_block(struct tobyfs *fs, uint32_t *out_blk) {
    uint32_t limit = fs->usable_blocks;
    for (uint32_t b = fs->sb.data_blk_start; b < limit; b++) {
        if (!bit_get(fs->dbitmap, b)) {
            bit_set(fs->dbitmap, b);
            int rc = flush_bitmap_bit(fs, fs->dbitmap, fs->dbitmap_blk, b);
            if (rc != 0) { bit_clear(fs->dbitmap, b); return VFS_ERR_IO; }
            /* Zero the freshly-allocated block on disk so stale data
             * never leaks across allocations -- especially important
             * for newly-allocated directory blocks. */
            uint8_t zero[TFS_BLOCK_SIZE] = {0};
            rc = write_block(fs, b, zero);
            if (rc != 0) {
                bit_clear(fs->dbitmap, b);
                (void)flush_bitmap_bit(fs, fs->dbitmap, fs->dbitmap_blk, b);
                return VFS_ERR_IO;
            }
            *out_blk = b;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPC;
}

static int free_data_block(struct tobyfs *fs, uint32_t blk) {
    uint32_t limit = fs->usable_blocks;
    if (blk < fs->sb.data_blk_start || blk >= limit) {
        return VFS_ERR_INVAL;
    }
    bit_clear(fs->dbitmap, blk);
    return flush_bitmap_bit(fs, fs->dbitmap, fs->dbitmap_blk, blk)
           == 0 ? VFS_OK : VFS_ERR_IO;
}

/* -------- inode I/O (read-modify-write whole block) -------- */

static int read_inode(struct tobyfs *fs, uint32_t ino,
                      struct tfs_inode_disk *out) {
    if (ino == 0 || ino >= fs->sb.inode_count) return VFS_ERR_INVAL;
    if (!bit_get(fs->ibitmap, ino)) return VFS_ERR_NOENT;
    uint32_t blk = fs->sb.inode_table_blk + (ino / TFS_INODES_PER_BLOCK);
    uint8_t buf[TFS_BLOCK_SIZE];
    int rc = read_block(fs, blk, buf);
    if (rc != 0) return VFS_ERR_IO;
    memcpy(out, buf + (ino % TFS_INODES_PER_BLOCK) * TFS_INODE_SIZE,
           sizeof(*out));
    return VFS_OK;
}

static int write_inode(struct tobyfs *fs, uint32_t ino,
                       const struct tfs_inode_disk *in) {
    if (ino == 0 || ino >= fs->sb.inode_count) return VFS_ERR_INVAL;
    uint32_t blk = fs->sb.inode_table_blk + (ino / TFS_INODES_PER_BLOCK);
    uint8_t buf[TFS_BLOCK_SIZE];
    int rc = read_block(fs, blk, buf);
    if (rc != 0) return VFS_ERR_IO;
    memcpy(buf + (ino % TFS_INODES_PER_BLOCK) * TFS_INODE_SIZE,
           in, sizeof(*in));
    return write_block(fs, blk, buf) == 0 ? VFS_OK : VFS_ERR_IO;
}

/* -------- path / dirent helpers -------- */

/* Pull the next '/'-delimited component out of *cursor. On entry,
 * *cursor is positioned at either '/' or the first byte of a name.
 * On return, `out` holds the NUL-terminated component, and *cursor
 * is advanced to the next component or the trailing NUL. Returns
 * 0 on success, VFS_ERR_NAMETOOLONG / VFS_ERR_INVAL on failure, and
 * +1 when there are no more components. */
static int next_component(const char **cursor, char *out, size_t out_max) {
    const char *p = *cursor;
    while (*p == '/') p++;
    if (*p == 0) { *cursor = p; return 1; }
    size_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 >= out_max) return VFS_ERR_NAMETOOLONG;
        out[n++] = *p++;
    }
    out[n] = 0;
    *cursor = p;
    return VFS_OK;
}

/* Look up `name` in the directory inode `dir`. Returns its inode
 * number in *out_ino on success. */
static int dir_lookup(struct tobyfs *fs, const struct tfs_inode_disk *dir,
                      const char *name, uint32_t *out_ino) {
    if (dir->type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;
    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return VFS_ERR_IO;
        const struct tfs_dirent_disk *de = (const struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) continue;
            if (strcmp(de[j].name, name) == 0) {
                *out_ino = de[j].ino;
                return VFS_OK;
            }
        }
    }
    return VFS_ERR_NOENT;
}

/* Walk an absolute path to its inode. `consumed_components` lets us
 * also handle "walk to parent" by requesting one fewer component. */
static int path_walk(struct tobyfs *fs, const char *path, uint32_t *out_ino,
                     struct tfs_inode_disk *out_node) {
    uint32_t cur = fs->sb.root_ino;
    struct tfs_inode_disk node;
    int rc = read_inode(fs, cur, &node);
    if (rc != VFS_OK) return rc;
    const char *p = path;
    char comp[TFS_NAME_MAX + 1];
    for (;;) {
        rc = next_component(&p, comp, sizeof(comp));
        if (rc == 1) break;            /* end of path */
        if (rc != VFS_OK) return rc;
        uint32_t child;
        rc = dir_lookup(fs, &node, comp, &child);
        if (rc != VFS_OK) return rc;
        cur = child;
        rc = read_inode(fs, cur, &node);
        if (rc != VFS_OK) return rc;
    }
    *out_ino = cur;
    *out_node = node;
    return VFS_OK;
}

/* Split "/a/b/c" into parent="/a/b" + leaf="c". Both buffers must be
 * at least VFS_PATH_MAX. Empty parent ("" or "/") is represented as
 * "/". Returns VFS_OK on success, VFS_ERR_INVAL on something like "/". */
static int split_parent_leaf(const char *path, char *parent, char *leaf) {
    /* Find the last '/' that precedes a non-empty leaf. */
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;     /* trim trailing slashes */
    if (n == 0) return VFS_ERR_INVAL;            /* root has no leaf */
    size_t leaf_start = n;
    while (leaf_start > 0 && path[leaf_start - 1] != '/') leaf_start--;
    size_t leaf_len = n - leaf_start;
    if (leaf_len == 0 || leaf_len > TFS_NAME_MAX) return VFS_ERR_NAMETOOLONG;
    memcpy(leaf, path + leaf_start, leaf_len);
    leaf[leaf_len] = 0;
    /* Parent is everything before leaf_start, with at least "/". */
    if (leaf_start <= 1) {
        parent[0] = '/'; parent[1] = 0;
    } else {
        size_t plen = leaf_start - 1;     /* drop the slash */
        if (plen >= VFS_PATH_MAX) return VFS_ERR_NAMETOOLONG;
        memcpy(parent, path, plen);
        parent[plen] = 0;
    }
    return VFS_OK;
}

/* Insert (ino, name) into the directory `dir_node`. May allocate a
 * new data block if every existing slot is full. Updates dir_node and
 * writes it back. */
static int dir_insert(struct tobyfs *fs, uint32_t dir_ino,
                      struct tfs_inode_disk *dir_node,
                      uint32_t child_ino, const char *name) {
    uint8_t buf[TFS_BLOCK_SIZE];
    /* Pass 1: find an empty slot in an already-allocated block. */
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir_node->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return VFS_ERR_IO;
        struct tfs_dirent_disk *de = (struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) {
                de[j].ino = child_ino;
                de[j].reserved = 0;
                size_t nlen = strlen(name);
                memcpy(de[j].name, name, nlen);
                de[j].name[nlen] = 0;
                if (write_block(fs, b, buf) != 0) return VFS_ERR_IO;
                dir_node->size += TFS_DIRENT_SIZE;
                return write_inode(fs, dir_ino, dir_node);
            }
        }
    }
    /* Pass 2: every existing block is full -- allocate a new one. */
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        if (dir_node->direct[i] != 0) continue;
        uint32_t newb;
        int rc = alloc_data_block(fs, &newb);
        if (rc != VFS_OK) return rc;
        dir_node->direct[i] = newb;
        memset(buf, 0, sizeof(buf));
        struct tfs_dirent_disk *de = (struct tfs_dirent_disk *)buf;
        de[0].ino = child_ino;
        de[0].reserved = 0;
        size_t nlen = strlen(name);
        memcpy(de[0].name, name, nlen);
        de[0].name[nlen] = 0;
        if (write_block(fs, newb, buf) != 0) {
            (void)free_data_block(fs, newb);
            dir_node->direct[i] = 0;
            return VFS_ERR_IO;
        }
        dir_node->size += TFS_DIRENT_SIZE;
        return write_inode(fs, dir_ino, dir_node);
    }
    return VFS_ERR_NOSPC;
}

/* Remove (name) from the directory. Does not touch the child inode --
 * caller is responsible for that. Returns VFS_ERR_NOENT if not found. */
static int dir_remove(struct tobyfs *fs, uint32_t dir_ino,
                      struct tfs_inode_disk *dir_node, const char *name) {
    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir_node->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return VFS_ERR_IO;
        struct tfs_dirent_disk *de = (struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) continue;
            if (strcmp(de[j].name, name) == 0) {
                de[j].ino = 0;
                de[j].name[0] = 0;
                if (write_block(fs, b, buf) != 0) return VFS_ERR_IO;
                if (dir_node->size >= TFS_DIRENT_SIZE) {
                    dir_node->size -= TFS_DIRENT_SIZE;
                }
                return write_inode(fs, dir_ino, dir_node);
            }
        }
    }
    return VFS_ERR_NOENT;
}

/* True if the directory contains any populated dirent. Used to refuse
 * removing a non-empty directory (we don't support recursive rm). */
static bool dir_is_empty(struct tobyfs *fs,
                         const struct tfs_inode_disk *dir_node) {
    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir_node->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return false;   /* fail-safe */
        const struct tfs_dirent_disk *de = (const struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino != 0) return false;
        }
    }
    return true;
}

/* -------- vfs_ops impl -------- */

struct tobyfs_handle {
    uint32_t                ino;
    struct tfs_inode_disk   node;     /* in-RAM copy; flushed on write */
};

static int tobyfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    /* Slice 7: report symlinks as such -- vfs_follow_link only engages when
     * stat says VFS_TYPE_SYMLINK, so getting this wrong makes every link
     * behave like a regular file containing its own target string. */
    out->type = (node.type == TFS_TYPE_DIR)     ? VFS_TYPE_DIR
              : (node.type == TFS_TYPE_SYMLINK) ? VFS_TYPE_SYMLINK
                                                : VFS_TYPE_FILE;
    out->size = node.size;
    out->uid  = node.uid;
    out->gid  = node.gid;
    out->mode = node.mode;
    out->mtime = node.mtime;      /* slice 6 */
    out->atime = node.mtime;      /* no separate atime on disk */
    out->ctime = node.mtime;
    return VFS_OK;
}

static int tobyfs_open(void *mnt, const char *path, struct vfs_file *out) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    if (node.type != TFS_TYPE_FILE) return VFS_ERR_ISDIR;
    struct tobyfs_handle *h = kmalloc(sizeof(*h));
    if (!h) return VFS_ERR_NOMEM;
    h->ino  = ino;
    h->node = node;
    out->priv = h;
    out->pos  = 0;
    out->size = node.size;
    out->uid  = node.uid;
    out->gid  = node.gid;
    out->mode = node.mode;
    out->ino  = ino;    /* stable per-file inode: two opens of the same path
                         * report the same st_ino (chrome shm inode-match check) */
    /* Both of those opens land on the same incarnation, so they share a
     * MAP_SHARED region; a later file that inherits the number does not. */
    out->ino_gen = (fs->ino_gen && ino < fs->sb.inode_count)
                       ? (uint64_t)fs->ino_gen[ino] : 0;
    return VFS_OK;
}

static int tobyfs_close(struct vfs_file *f) {
    if (f->priv) kfree(f->priv);
    f->priv = 0;
    return VFS_OK;
}

/* -------- indirect-block helpers -------- */

static uint32_t get_block_for_index(struct tobyfs *fs, struct tfs_inode_disk *node,
                                     uint32_t didx) {
    if (didx < TFS_NDIRECT) {
        return node->direct[didx];
    }

    /* Single-indirect range: [NDIRECT, NDIRECT + INDIRECT_ENTRIES). */
    if (didx < TFS_DBL_INDIRECT_BASE) {
        if (node->indirect == 0) return 0;
        uint32_t ioff = didx - TFS_NDIRECT;
        uint8_t ibuf[TFS_BLOCK_SIZE];
        if (read_block(fs, node->indirect, ibuf) != 0) return 0;
        uint32_t *entries = (uint32_t *)ibuf;
        return entries[ioff];
    }

    /* Double-indirect range: didx maps to (L1 index, L2 index) within the
     * 1024 x 1024 tree rooted at node->dbl_indirect. */
    if (node->dbl_indirect == 0) return 0;
    uint32_t doff = didx - TFS_DBL_INDIRECT_BASE;
    if (doff >= TFS_DBL_INDIRECT_ENTRIES) return 0;
    uint32_t l1 = doff / TFS_INDIRECT_ENTRIES;   /* which L2 block */
    uint32_t l2 = doff % TFS_INDIRECT_ENTRIES;   /* slot within L2 */

    uint8_t l1buf[TFS_BLOCK_SIZE];
    if (read_block(fs, node->dbl_indirect, l1buf) != 0) return 0;
    uint32_t l2blk = ((uint32_t *)l1buf)[l1];
    if (l2blk == 0) return 0;

    uint8_t l2buf[TFS_BLOCK_SIZE];
    if (read_block(fs, l2blk, l2buf) != 0) return 0;
    return ((uint32_t *)l2buf)[l2];
}

static int set_block_for_index(struct tobyfs *fs, struct tfs_inode_disk *node,
                                uint32_t didx, uint32_t blk_num) {
    if (didx < TFS_NDIRECT) {
        node->direct[didx] = blk_num;
        return 0;
    }

    /* Single-indirect range. */
    if (didx < TFS_DBL_INDIRECT_BASE) {
        uint32_t ioff = didx - TFS_NDIRECT;

        if (node->indirect == 0) {
            uint32_t iblk;
            int rc = alloc_data_block(fs, &iblk);
            if (rc != VFS_OK) return rc;
            node->indirect = iblk;
        }

        uint8_t ibuf[TFS_BLOCK_SIZE];
        if (read_block(fs, node->indirect, ibuf) != 0) return VFS_ERR_IO;
        uint32_t *entries = (uint32_t *)ibuf;
        entries[ioff] = blk_num;
        if (write_block(fs, node->indirect, ibuf) != 0) return VFS_ERR_IO;
        return 0;
    }

    /* Double-indirect range: lazily allocate the L1 root, then the L2 block,
     * then store the data-block pointer in the L2 slot. alloc_data_block
     * zero-fills new blocks, so freshly-allocated L1/L2 blocks read back as
     * all-zero pointers (== "hole"), which is exactly what we want. */
    uint32_t doff = didx - TFS_DBL_INDIRECT_BASE;
    if (doff >= TFS_DBL_INDIRECT_ENTRIES) return -1;
    uint32_t l1 = doff / TFS_INDIRECT_ENTRIES;
    uint32_t l2 = doff % TFS_INDIRECT_ENTRIES;

    if (node->dbl_indirect == 0) {
        uint32_t blk;
        int rc = alloc_data_block(fs, &blk);
        if (rc != VFS_OK) return rc;
        node->dbl_indirect = blk;
    }

    uint8_t l1buf[TFS_BLOCK_SIZE];
    if (read_block(fs, node->dbl_indirect, l1buf) != 0) return VFS_ERR_IO;
    uint32_t *l1e = (uint32_t *)l1buf;
    uint32_t l2blk = l1e[l1];
    if (l2blk == 0) {
        int rc = alloc_data_block(fs, &l2blk);
        if (rc != VFS_OK) return rc;
        l1e[l1] = l2blk;
        if (write_block(fs, node->dbl_indirect, l1buf) != 0) return VFS_ERR_IO;
    }

    uint8_t l2buf[TFS_BLOCK_SIZE];
    if (read_block(fs, l2blk, l2buf) != 0) return VFS_ERR_IO;
    uint32_t *l2e = (uint32_t *)l2buf;
    l2e[l2] = blk_num;
    if (write_block(fs, l2blk, l2buf) != 0) return VFS_ERR_IO;
    return 0;
}

static long tobyfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct tobyfs *fs = (struct tobyfs *)f->mnt;
    struct tobyfs_handle *h = (struct tobyfs_handle *)f->priv;
    if (!h) return VFS_ERR_INVAL;
    if (f->pos >= h->node.size) return 0;
    size_t avail = h->node.size - f->pos;
    if (n > avail) n = avail;

    uint8_t *out = (uint8_t *)buf;
    size_t copied = 0;
    uint8_t blkbuf[TFS_BLOCK_SIZE];
    while (copied < n) {
        size_t off    = f->pos + copied;
        uint32_t didx = (uint32_t)(off / TFS_BLOCK_SIZE);
        uint32_t bofs = (uint32_t)(off % TFS_BLOCK_SIZE);
        uint32_t blk = get_block_for_index(fs, &h->node, didx);
        if (blk == 0) {
            /* Sparse hole -- shouldn't happen on regular writes, but
             * be defensive: zero-fill. */
            size_t chunk = TFS_BLOCK_SIZE - bofs;
            if (chunk > n - copied) chunk = n - copied;
            memset(out + copied, 0, chunk);
            copied += chunk;
            continue;
        }
        if (read_block(fs, blk, blkbuf) != 0) return VFS_ERR_IO;
        size_t chunk = TFS_BLOCK_SIZE - bofs;
        if (chunk > n - copied) chunk = n - copied;
        memcpy(out + copied, blkbuf + bofs, chunk);
        copied += chunk;
    }
    f->pos += copied;
    return (long)copied;
}

static long tobyfs_write(struct vfs_file *f, const void *buf, size_t n) {
    struct tobyfs *fs = (struct tobyfs *)f->mnt;
    struct tobyfs_handle *h = (struct tobyfs_handle *)f->priv;
    if (!h) return VFS_ERR_INVAL;

    journal_begin(fs);

    const uint8_t *in = (const uint8_t *)buf;
    size_t written = 0;
    uint8_t blkbuf[TFS_BLOCK_SIZE];

    while (written < n) {
        /* ---- Bound the journal transaction --------------------------------
         *
         * A single write() can touch far more blocks than one transaction can
         * hold. TFS_TXN_MAX_BLOCKS is 16, and a 64 KiB write is 16 DATA blocks
         * before you count the bitmap, indirect and inode blocks that go with
         * them -- so journal_write() hit VFS_ERR_NOSPC and the whole write
         * failed. Symptom: `dd bs=64k` transferred ZERO bytes, while 16 KiB
         * and 32 KiB worked. 4 KiB writes were fine, which is why this hid --
         * every in-kernel writer used small buffers.
         *
         * Fix: commit the current transaction and open a fresh one whenever
         * the next iteration might not fit. Each chunk stays crash-atomic
         * (that is what the journal is for); the write as a whole is not one
         * atomic unit, which POSIX does not require of write(2) on a regular
         * file anyway.
         *
         * RESERVE is the worst-case slot count for one iteration: the data
         * block, its bitmap block, an indirect block, a double-indirect block,
         * and the inode -- five, rounded up to six. */
        #define TFS_TXN_RESERVE 6
        if (fs->in_transaction &&
            fs->txn_count + TFS_TXN_RESERVE > (int)TFS_TXN_MAX_BLOCKS) {
            size_t cur_end = f->pos + written;
            if (cur_end > h->node.size) h->node.size = (uint32_t)cur_end;
            h->node.mtime = tfs_now_secs();
            int crc = write_inode(fs, h->ino, &h->node);
            if (crc != VFS_OK) { journal_abort(fs); return crc; }
            crc = journal_commit(fs);
            if (crc != VFS_OK) return crc;
            journal_begin(fs);          /* continue in a new transaction */
        }

        size_t off    = f->pos + written;
        uint32_t didx = (uint32_t)(off / TFS_BLOCK_SIZE);
        uint32_t bofs = (uint32_t)(off % TFS_BLOCK_SIZE);
        /* Direct + single-indirect + double-indirect addressable blocks. */
        uint32_t max_blocks = TFS_NDIRECT + TFS_INDIRECT_ENTRIES +
                              TFS_DBL_INDIRECT_ENTRIES;
        if (didx >= max_blocks) break;
        uint32_t blk = get_block_for_index(fs, &h->node, didx);
        if (blk == 0) {
            int rc = alloc_data_block(fs, &blk);
            if (rc != VFS_OK) {
                if (written > 0) goto flush;
                journal_abort(fs);
                return rc;
            }
            rc = set_block_for_index(fs, &h->node, didx, blk);
            if (rc != 0) {
                (void)free_data_block(fs, blk);
                if (written > 0) goto flush;
                journal_abort(fs);
                return VFS_ERR_IO;
            }
        }
        size_t chunk = TFS_BLOCK_SIZE - bofs;
        if (chunk > n - written) chunk = n - written;
        if (chunk == TFS_BLOCK_SIZE) {
            /* Whole-block overwrite: skip the read. */
            memcpy(blkbuf, in + written, TFS_BLOCK_SIZE);
        } else {
            if (read_block(fs, blk, blkbuf) != 0) {
                if (written > 0) goto flush;
                journal_abort(fs);
                return VFS_ERR_IO;
            }
            memcpy(blkbuf + bofs, in + written, chunk);
        }
        {
            /* Report the REAL failure. This used to flatten everything to
             * VFS_ERR_IO, which turned a full-journal VFS_ERR_NOSPC into "I/O
             * error" -- and since VFS codes leak out as Linux errnos, -7
             * surfaced to userspace as E2BIG, "Argument list too long", on a
             * plain disk write. Losing the code cost more debugging time than
             * the bug it hid. */
            int wrc = write_block(fs, blk, blkbuf);
            if (wrc != 0) {
                if (written > 0) goto flush;
                journal_abort(fs);
                return wrc < 0 ? wrc : VFS_ERR_IO;
            }
        }
        written += chunk;
    }

flush:
    {
        size_t newend = f->pos + written;
        if (newend > h->node.size) h->node.size = (uint32_t)newend;
        h->node.mtime = tfs_now_secs();
        int rc = write_inode(fs, h->ino, &h->node);
        if (rc != VFS_OK) { journal_abort(fs); return rc; }
        rc = journal_commit(fs);
        if (rc != VFS_OK) return rc;
        f->pos += written;
        f->size = h->node.size;
        return (long)written;
    }
}

static void inode_free_blocks(struct tobyfs *fs, struct tfs_inode_disk *node) {
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        if (node->direct[i] != 0) {
            (void)free_data_block(fs, node->direct[i]);
            node->direct[i] = 0;
        }
    }
    if (node->indirect != 0) {
        uint8_t ibuf[TFS_BLOCK_SIZE];
        if (read_block(fs, node->indirect, ibuf) == 0) {
            uint32_t *entries = (uint32_t *)ibuf;
            for (uint32_t i = 0; i < TFS_INDIRECT_ENTRIES; i++) {
                if (entries[i] != 0) {
                    (void)free_data_block(fs, entries[i]);
                }
            }
        }
        (void)free_data_block(fs, node->indirect);
        node->indirect = 0;
    }

    /* Double-indirect tree: free every data block via the L2 blocks, free
     * each L2 block, then the L1 root. Mirrors the single-indirect walk one
     * level deeper. */
    if (node->dbl_indirect != 0) {
        uint8_t l1buf[TFS_BLOCK_SIZE];
        if (read_block(fs, node->dbl_indirect, l1buf) == 0) {
            uint32_t *l1e = (uint32_t *)l1buf;
            for (uint32_t i = 0; i < TFS_INDIRECT_ENTRIES; i++) {
                if (l1e[i] == 0) continue;
                uint8_t l2buf[TFS_BLOCK_SIZE];
                if (read_block(fs, l1e[i], l2buf) == 0) {
                    uint32_t *l2e = (uint32_t *)l2buf;
                    for (uint32_t j = 0; j < TFS_INDIRECT_ENTRIES; j++) {
                        if (l2e[j] != 0) (void)free_data_block(fs, l2e[j]);
                    }
                }
                (void)free_data_block(fs, l1e[i]);
            }
        }
        (void)free_data_block(fs, node->dbl_indirect);
        node->dbl_indirect = 0;
    }
}

static int tobyfs_create(void *mnt, const char *path,
                         uint32_t uid, uint32_t gid, uint32_t mode) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char parent[VFS_PATH_MAX], leaf[TFS_NAME_MAX + 1];
    int rc = split_parent_leaf(path, parent, leaf);
    if (rc != VFS_OK) return rc;

    uint32_t pino;
    struct tfs_inode_disk pnode;
    rc = path_walk(fs, parent, &pino, &pnode);
    if (rc != VFS_OK) return rc;
    if (pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    uint32_t existing;
    rc = dir_lookup(fs, &pnode, leaf, &existing);
    if (rc == VFS_OK) {
        /* For "touch"-style semantics: if it already exists and is a
         * file, that's fine -- report EXIST so vfs_write_all knows to
         * truncate. If it's a directory, that's a real error. */
        struct tfs_inode_disk en;
        if (read_inode(fs, existing, &en) == VFS_OK &&
            en.type == TFS_TYPE_DIR) {
            return VFS_ERR_ISDIR;
        }
        return VFS_ERR_EXIST;
    }
    if (rc != VFS_ERR_NOENT) return rc;

    journal_begin(fs);

    uint32_t ino;
    rc = alloc_inode(fs, &ino);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }

    struct tfs_inode_disk node = {0};
    node.type  = TFS_TYPE_FILE;
    node.nlink = 1;
    node.size  = 0;
    node.mtime = tfs_now_secs();
    node.mode  = mode ? mode : TFS_DEFAULT_FILE_MODE;
    node.uid   = uid;
    node.gid   = gid;
    rc = write_inode(fs, ino, &node);
    if (rc != VFS_OK) { (void)free_inode(fs, ino); journal_abort(fs); return rc; }

    rc = dir_insert(fs, pino, &pnode, ino, leaf);
    if (rc != VFS_OK) {
        (void)free_inode(fs, ino);
        journal_abort(fs);
        return rc;
    }
    return journal_commit(fs);
}

/* Linux slice 7: create an on-disk symbolic link.
 *
 * Modelled on tobyfs_create, with the target string written into the inode's
 * first data block and node.size set to its length -- so readlink is just a
 * short read and no new on-disk structure exists to version. */
static int tobyfs_symlink(void *mnt, const char *path, const char *target) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char parent[VFS_PATH_MAX], leaf[TFS_NAME_MAX + 1];
    size_t tlen = strlen(target);
    if (tlen == 0 || tlen >= TFS_BLOCK_SIZE) return VFS_ERR_NAMETOOLONG;

    int rc = split_parent_leaf(path, parent, leaf);
    if (rc != VFS_OK) return rc;
    uint32_t pino;
    struct tfs_inode_disk pnode;
    rc = path_walk(fs, parent, &pino, &pnode);
    if (rc != VFS_OK) return rc;
    if (pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    uint32_t existing;
    if (dir_lookup(fs, &pnode, leaf, &existing) == VFS_OK) return VFS_ERR_EXIST;

    journal_begin(fs);
    uint32_t ino;
    rc = alloc_inode(fs, &ino);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }

    struct tfs_inode_disk node = {0};
    node.type  = TFS_TYPE_SYMLINK;
    node.nlink = 1;
    node.size  = (uint32_t)tlen;
    node.mtime = tfs_now_secs();
    node.mode  = 00777u | TFS_MODE_VALID;     /* links are always lrwxrwxrwx */

    uint32_t blk;
    rc = alloc_data_block(fs, &blk);
    if (rc != VFS_OK) { (void)free_inode(fs, ino); journal_abort(fs); return rc; }
    rc = set_block_for_index(fs, &node, 0, blk);
    if (rc != VFS_OK) { (void)free_data_block(fs, blk); (void)free_inode(fs, ino);
                        journal_abort(fs); return VFS_ERR_IO; }
    {
        uint8_t blkbuf[TFS_BLOCK_SIZE];
        memset(blkbuf, 0, sizeof blkbuf);
        memcpy(blkbuf, target, tlen);
        if (write_block(fs, blk, blkbuf) != 0) {
            (void)free_data_block(fs, blk); (void)free_inode(fs, ino);
            journal_abort(fs); return VFS_ERR_IO;
        }
    }
    rc = write_inode(fs, ino, &node);
    if (rc != VFS_OK) { (void)free_inode(fs, ino); journal_abort(fs); return rc; }
    rc = dir_insert(fs, pino, &pnode, ino, leaf);
    if (rc != VFS_OK) { (void)free_inode(fs, ino); journal_abort(fs); return rc; }
    return journal_commit(fs);
}

/* Linux slice 7: read an on-disk symbolic link's target. */
static int tobyfs_readlink(void *mnt, const char *path,
                           char *buf, size_t bufsz) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    if (node.type != TFS_TYPE_SYMLINK) return VFS_ERR_INVAL;
    uint32_t blk = get_block_for_index(fs, &node, 0);
    if (blk == 0) return VFS_ERR_IO;
    uint8_t blkbuf[TFS_BLOCK_SIZE];
    if (read_block(fs, blk, blkbuf) != 0) return VFS_ERR_IO;
    size_t n = node.size;
    if (n >= bufsz) n = bufsz - 1;
    memcpy(buf, blkbuf, n);
    buf[n] = '\0';
    /* VFS_OK, NOT the length. vfs_readlink()'s caller tests `rc != VFS_OK`
     * and takes the string's length itself, so returning a byte count here
     * (the obvious readlink(2)-shaped answer) reads as an error code and
     * makes every link look broken -- which is exactly what happened to the
     * 335 links in the extracted Alpine rootfs. procfs_readlink returns
     * VFS_OK for the same reason. */
    return VFS_OK;
}

static int tobyfs_mkdir(void *mnt, const char *path,
                        uint32_t uid, uint32_t gid, uint32_t mode) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char parent[VFS_PATH_MAX], leaf[TFS_NAME_MAX + 1];
    int rc = split_parent_leaf(path, parent, leaf);
    if (rc != VFS_OK) return rc;

    uint32_t pino;
    struct tfs_inode_disk pnode;
    rc = path_walk(fs, parent, &pino, &pnode);
    if (rc != VFS_OK) return rc;
    if (pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    uint32_t existing;
    rc = dir_lookup(fs, &pnode, leaf, &existing);
    if (rc == VFS_OK) return VFS_ERR_EXIST;
    if (rc != VFS_ERR_NOENT) return rc;

    journal_begin(fs);

    uint32_t ino;
    rc = alloc_inode(fs, &ino);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }

    struct tfs_inode_disk node = {0};
    node.type  = TFS_TYPE_DIR;
    node.nlink = 1;
    node.size  = 0;
    node.mtime = tfs_now_secs();
    node.mode  = mode ? mode : TFS_DEFAULT_DIR_MODE;
    node.uid   = uid;
    node.gid   = gid;
    /* Empty dir -- no data blocks until something's inserted. */
    rc = write_inode(fs, ino, &node);
    if (rc != VFS_OK) { (void)free_inode(fs, ino); journal_abort(fs); return rc; }

    rc = dir_insert(fs, pino, &pnode, ino, leaf);
    if (rc != VFS_OK) {
        (void)free_inode(fs, ino);
        journal_abort(fs);
        return rc;
    }
    return journal_commit(fs);
}



static int tobyfs_chmod(void *mnt, const char *path, uint32_t mode) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    /* Preserve any high-bit flags (e.g. MODE_VALID) the caller asked for
     * but mask everything else to perms only. */
    node.mode = (mode & TFS_MODE_PERMS) | TFS_MODE_VALID;
    return write_inode(fs, ino, &node);
}

/* Linux slice 6: persist timestamps. The on-disk inode has ONE time field, so
 * atime is accepted and folded into mtime rather than stored separately --
 * stated plainly because a caller that sets only atime will see mtime move.
 * The alternative (silently ignoring atime) would be worse: `tar -x` and
 * `cp -p` set both, and mtime is the one anything actually compares. */
static int tobyfs_utimes(void *mnt, const char *path,
                         uint64_t mtime, uint64_t atime) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    node.mtime = (uint32_t)(mtime ? mtime : atime);
    return write_inode(fs, ino, &node);
}

/* Linux slice 6: set a file's length.
 *
 * SCOPE, stated plainly:
 *   - truncate to 0 frees every data block (the `> file` case, and the one
 *     where leaking blocks would actually hurt).
 *   - a partial SHRINK updates the size but leaves the now-unreachable tail
 *     blocks allocated until the file is unlinked. Reclaiming them needs a
 *     block-range free that the inode layout does not have a helper for; the
 *     space is recovered on unlink, so this wastes blocks rather than losing
 *     data.
 *   - a GROW is sparse: the size moves and reads past the old end return
 *     zeroes, because get_block_for_index() reports "no block" as zero-fill.
 * All three match what callers observe on Linux; only the middle one differs
 * in on-disk economy. */
static int tobyfs_truncate(void *mnt, const char *path, uint64_t length) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    if (node.type == TFS_TYPE_DIR) return VFS_ERR_ISDIR;
    if (length > 0xFFFFFFFFull) return VFS_ERR_INVAL;

    journal_begin(fs);
    if (length == 0) inode_free_blocks(fs, &node);
    node.size  = (uint32_t)length;
    node.mtime = tfs_now_secs();
    rc = write_inode(fs, ino, &node);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }
    return journal_commit(fs);
}

/* Linux slice 6: truncate through an OPEN handle. The handle already carries
 * the inode number and a cached copy of the inode, so this needs no path walk
 * -- which is the point: busybox's `truncate` and CPython's os.ftruncate act
 * on an fd, and the slice-1 ftruncate only ever touched in-memory open-file
 * state that never reached the disk. Same scope notes as tobyfs_truncate. */
static int tobyfs_ftruncate(struct vfs_file *f, uint64_t length) {
    struct tobyfs *fs = (struct tobyfs *)f->mnt;
    struct tobyfs_handle *h = (struct tobyfs_handle *)f->priv;
    if (!fs || !h) return VFS_ERR_INVAL;
    if (length > 0xFFFFFFFFull) return VFS_ERR_INVAL;
    journal_begin(fs);
    if (length == 0) inode_free_blocks(fs, &h->node);
    h->node.size  = (uint32_t)length;
    h->node.mtime = tfs_now_secs();
    int rc = write_inode(fs, h->ino, &h->node);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }
    return journal_commit(fs);
}

static int tobyfs_chown(void *mnt, const char *path,
                        uint32_t uid, uint32_t gid) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    node.uid = uid;
    node.gid = gid;
    /* Inode now has authoritative ownership info; flip MODE_VALID on so
     * the VFS treats the perm bits as enforceable. If perms were never
     * set (legacy), assume the matching default. */
    if (!(node.mode & TFS_MODE_VALID)) {
        node.mode = (node.type == TFS_TYPE_DIR)
                  ? TFS_DEFAULT_DIR_MODE
                  : TFS_DEFAULT_FILE_MODE;
    }
    return write_inode(fs, ino, &node);
}

static int tobyfs_unlink(void *mnt, const char *path) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char parent[VFS_PATH_MAX], leaf[TFS_NAME_MAX + 1];
    int rc = split_parent_leaf(path, parent, leaf);
    if (rc != VFS_OK) return rc;

    uint32_t pino;
    struct tfs_inode_disk pnode;
    rc = path_walk(fs, parent, &pino, &pnode);
    if (rc != VFS_OK) return rc;
    if (pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    uint32_t cino;
    rc = dir_lookup(fs, &pnode, leaf, &cino);
    if (rc != VFS_OK) return rc;

    struct tfs_inode_disk cnode;
    rc = read_inode(fs, cino, &cnode);
    if (rc != VFS_OK) return rc;

    /* Empty directories are removable; non-empty ones are refused (we
     * don't do recursive rm in this milestone). */
    if (cnode.type == TFS_TYPE_DIR && !dir_is_empty(fs, &cnode)) {
        return VFS_ERR_INVAL;
    }

    journal_begin(fs);

    inode_free_blocks(fs, &cnode);
    cnode.type = TFS_TYPE_FREE;
    cnode.size = 0;
    (void)write_inode(fs, cino, &cnode);
    (void)free_inode(fs, cino);
    rc = dir_remove(fs, pino, &pnode, leaf);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }
    return journal_commit(fs);
}

/* Atomically re-link oldpath -> newpath. Source must exist; an existing dest is
 * replaced (its inode freed). Journalled so a crash leaves either the old or the
 * new name, never both/neither. leveldb/SQLite (chrome's profile stores) rename
 * a temp file over a live one (CURRENT / MANIFEST / journal) constantly. */
static int tobyfs_rename(void *mnt, const char *oldpath, const char *newpath) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char op_par[VFS_PATH_MAX], op_leaf[TFS_NAME_MAX + 1];
    char np_par[VFS_PATH_MAX], np_leaf[TFS_NAME_MAX + 1];
    int rc = split_parent_leaf(oldpath, op_par, op_leaf);
    if (rc != VFS_OK) return rc;
    rc = split_parent_leaf(newpath, np_par, np_leaf);
    if (rc != VFS_OK) return rc;

    uint32_t old_pino, new_pino;
    struct tfs_inode_disk old_pnode, new_pnode;
    rc = path_walk(fs, op_par, &old_pino, &old_pnode);
    if (rc != VFS_OK) return rc;
    if (old_pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;
    rc = path_walk(fs, np_par, &new_pino, &new_pnode);
    if (rc != VFS_OK) return rc;
    if (new_pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    bool same_parent = (old_pino == new_pino);
    /* When both names live in the same directory the in-memory dir node must be
     * a SINGLE copy -- dir_insert/dir_remove each mutate + persist it, so two
     * independent copies would clobber one another. Alias old onto new. */
    struct tfs_inode_disk *opn = same_parent ? &new_pnode : &old_pnode;

    /* Source must exist. */
    uint32_t src_ino;
    rc = dir_lookup(fs, opn, op_leaf, &src_ino);
    if (rc != VFS_OK) return rc;

    /* Does the destination already exist? */
    uint32_t dst_ino = 0;
    int drc = dir_lookup(fs, &new_pnode, np_leaf, &dst_ino);
    if (drc == VFS_OK) {
        if (dst_ino == src_ino) return VFS_OK;   /* same file -- rename is a no-op */
        struct tfs_inode_disk dnode;
        rc = read_inode(fs, dst_ino, &dnode);
        if (rc != VFS_OK) return rc;
        /* Refuse to clobber a non-empty directory (no recursive rm here). */
        if (dnode.type == TFS_TYPE_DIR && !dir_is_empty(fs, &dnode))
            return VFS_ERR_INVAL;
    } else if (drc != VFS_ERR_NOENT) {
        return drc;
    }

    journal_begin(fs);

    /* Replace: free the old destination inode + drop its dirent. */
    if (drc == VFS_OK) {
        struct tfs_inode_disk dnode;
        if (read_inode(fs, dst_ino, &dnode) == VFS_OK) {
            inode_free_blocks(fs, &dnode);
            dnode.type = TFS_TYPE_FREE; dnode.size = 0;
            (void)write_inode(fs, dst_ino, &dnode);
            (void)free_inode(fs, dst_ino);
        }
        rc = dir_remove(fs, new_pino, &new_pnode, np_leaf);
        if (rc != VFS_OK && rc != VFS_ERR_NOENT) { journal_abort(fs); return rc; }
    }

    /* Link the source inode under the new name, then unlink the old name. */
    rc = dir_insert(fs, new_pino, &new_pnode, src_ino, np_leaf);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }

    rc = dir_remove(fs, old_pino, opn, op_leaf);
    if (rc != VFS_OK) { journal_abort(fs); return rc; }

    return journal_commit(fs);
}

/* -------- directory iteration --------
 *
 * opendir() snapshots the directory's dirents into a heap array; readdir
 * just hands them out. Snapshotting is fine because directories are
 * tiny (max ~1024 entries) and the snapshot survives concurrent write
 * traffic without locking. */

struct tobyfs_diriter {
    struct vfs_dirent *ents;
    size_t             count;
};

static int tobyfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    if (node.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    struct tobyfs_diriter *it = kmalloc(sizeof(*it));
    if (!it) return VFS_ERR_NOMEM;
    /* Worst case: every direct block packed full of dirents. */
    size_t cap = TFS_NDIRECT * TFS_DIRENTS_PER_BLOCK;
    it->ents  = kcalloc(cap, sizeof(*it->ents));
    if (!it->ents) { kfree(it); return VFS_ERR_NOMEM; }
    it->count = 0;

    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = node.direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) {
            kfree(it->ents); kfree(it);
            return VFS_ERR_IO;
        }
        const struct tfs_dirent_disk *de = (const struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) continue;
            struct vfs_dirent *e = &it->ents[it->count++];
            size_t nlen = strlen(de[j].name);
            if (nlen >= sizeof(e->name)) nlen = sizeof(e->name) - 1;
            memcpy(e->name, de[j].name, nlen);
            e->name[nlen] = 0;
            struct tfs_inode_disk cn;
            if (read_inode(fs, de[j].ino, &cn) == VFS_OK) {
                e->type = (cn.type == TFS_TYPE_DIR) ? VFS_TYPE_DIR
                                                    : VFS_TYPE_FILE;
                e->size = cn.size;
                e->uid  = cn.uid;
                e->gid  = cn.gid;
                e->mode = cn.mode;
            } else {
                e->type = VFS_TYPE_FILE;
                e->size = 0;
                e->uid  = 0;
                e->gid  = 0;
                e->mode = 0;
            }
        }
    }
    out->priv  = it;
    out->index = 0;
    return VFS_OK;
}

static int tobyfs_closedir(struct vfs_dir *d) {
    struct tobyfs_diriter *it = (struct tobyfs_diriter *)d->priv;
    if (it) {
        if (it->ents) kfree(it->ents);
        kfree(it);
    }
    d->priv = 0;
    return VFS_OK;
}

static int tobyfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct tobyfs_diriter *it = (struct tobyfs_diriter *)d->priv;
    if (!it) return VFS_ERR_INVAL;
    if (d->index >= it->count) return VFS_ERR_NOENT;
    *out = it->ents[d->index++];
    return VFS_OK;
}

static const struct vfs_ops tobyfs_ops = {
    .open     = tobyfs_open,
    .close    = tobyfs_close,
    .read     = tobyfs_read,
    .write    = tobyfs_write,
    .create   = tobyfs_create,
    .unlink   = tobyfs_unlink,
    .rename   = tobyfs_rename,
    .mkdir    = tobyfs_mkdir,
    .opendir  = tobyfs_opendir,
    .closedir = tobyfs_closedir,
    .readdir  = tobyfs_readdir,
    .stat     = tobyfs_stat,
    .chmod    = tobyfs_chmod,
    .chown    = tobyfs_chown,
    .symlink  = tobyfs_symlink,  /* slice 7 */
    .readlink = tobyfs_readlink, /* slice 7 */
    .utimes   = tobyfs_utimes,   /* slice 6 */
    .truncate = tobyfs_truncate, /* slice 6 */
    .ftruncate= tobyfs_ftruncate,/* slice 6 */
};

/* M28E: identification helper used by sys_fs_check() to recognise
 * tobyfs mounts in the VFS table without exposing the static vtable. */
const void *tobyfs_ops_addr(void) { return &tobyfs_ops; }

/* Map an opaque tobyfs mount-data pointer back to its backing block
 * device. Same contract as fat32_blkdev_of / ext2_blkdev_of: caller
 * must have verified the vfs_ops pointer against tobyfs_ops_addr()
 * first. Used by the provisioning guard to refuse formatting a disk
 * that carries a live mount. */
struct blk_dev *tobyfs_blkdev_of(void *mnt) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    return fs ? fs->dev : 0;
}

/* Slice 122c: the filesystem's TRUE on-disk extent in 512-byte sectors, so
 * the swap placer can start past it. TFS_TOTAL_BLOCKS (the compile-time
 * 4 MiB minimum) is NOT the answer for dynamically-sized volumes -- placing
 * swap at that stale constant put it INSIDE the live filesystem of every
 * volume larger than the minimum. */
uint64_t tobyfs_total_sectors(void *mnt) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    if (!fs) return 0;
    return (uint64_t)fs->sb.total_blocks * TFS_SECTORS_PER_BLOCK;
}

/* -------- mount -------- */

int tobyfs_mount(const char *mount_point, struct blk_dev *dev) {
    if (!mount_point || !dev) return VFS_ERR_INVAL;

    struct tobyfs *fs = kcalloc(1, sizeof(*fs));
    if (!fs) return VFS_ERR_NOMEM;
    fs->dev = dev;

    /* Read superblock. */
    uint8_t buf[TFS_BLOCK_SIZE];
    if (read_block(fs, 0, buf) != 0) {
        kprintf("[tobyfs] superblock read failed\n");
        kfree(fs);
        return VFS_ERR_IO;
    }
    memcpy(&fs->sb, buf, sizeof(fs->sb));
    if (fs->sb.magic != TFS_MAGIC) {
        kprintf("[tobyfs] bad magic 0x%lx (expected 0x%lx) -- run mkfs?\n",
                (unsigned long)fs->sb.magic,
                (unsigned long)TFS_MAGIC);
        kfree(fs);
        return VFS_ERR_INVAL;
    }
    /* Validate geometry. We now accept ANY total_blocks/inode_count in the
     * supported range (dynamic sizing); only the block size is fixed and the
     * volume must not exceed our cap (bounds the cached-bitmap RAM). */
    if (fs->sb.block_size  != TFS_BLOCK_SIZE        ||
        fs->sb.total_blocks <  TFS_TOTAL_BLOCKS     ||
        fs->sb.total_blocks >  TFS_MAX_TOTAL_BLOCKS ||
        fs->sb.inode_count  == 0                    ||
        fs->sb.data_blk_start >= fs->sb.total_blocks ||
        fs->sb.inode_bitmap_blk == 0) {
        kprintf("[tobyfs] geometry invalid: bs=%u total=%u inodes=%u dstart=%u\n",
                fs->sb.block_size, fs->sb.total_blocks, fs->sb.inode_count,
                fs->sb.data_blk_start);
        kfree(fs);
        return VFS_ERR_INVAL;
    }

    /* Derive bitmap geometry from the superblock. The on-disk regions are
     * contiguous (sb | ibitmap | dbitmap | inode_table | data | journal), so
     * each bitmap's block span is exactly what it needs to cover its bits,
     * and the start blocks come straight from the superblock. */
    fs->ibitmap_blk    = fs->sb.inode_bitmap_blk;
    fs->dbitmap_blk    = fs->sb.data_bitmap_blk;
    fs->ibitmap_blocks =
        (fs->sb.inode_count  + TFS_BITS_PER_BLOCK - 1) / TFS_BITS_PER_BLOCK;
    fs->dbitmap_blocks =
        (fs->sb.total_blocks + TFS_BITS_PER_BLOCK - 1) / TFS_BITS_PER_BLOCK;

    /* Detect write-ahead journal from superblock reserved fields. Pre-journal
     * images have reserved[0]==0 so journaling is silently disabled. The
     * journal start is now dynamic (recorded in reserved[0]); validate it
     * sits inside the volume before trusting it. */
    if (fs->sb.reserved[0] != 0 &&
        fs->sb.reserved[0] <  fs->sb.total_blocks &&
        fs->sb.reserved[1] == TFS_JOURNAL_BLOCKS) {
        fs->journal_start = fs->sb.reserved[0];
        fs->journal_size  = fs->sb.reserved[1];
    }
    fs->usable_blocks = fs->journal_start ? fs->journal_start
                                          : fs->sb.total_blocks;

    /* Allocate the cached bitmaps (multi-block for large volumes). */
    fs->ibitmap = kmalloc((size_t)fs->ibitmap_blocks * TFS_BLOCK_SIZE);
    fs->dbitmap = kmalloc((size_t)fs->dbitmap_blocks * TFS_BLOCK_SIZE);
    /* Per-inode-number incarnation counters (see vfs_file.ino_gen). In-memory
     * only: they exist to tell successive users of a recycled number apart
     * within one boot, which is exactly the window a shared mapping lives in. */
    fs->ino_gen = (uint32_t *)kmalloc((size_t)fs->sb.inode_count * sizeof(uint32_t));
    if (fs->ino_gen)
        memset(fs->ino_gen, 0, (size_t)fs->sb.inode_count * sizeof(uint32_t));
    if (!fs->ibitmap || !fs->dbitmap) {
        kprintf("[tobyfs] bitmap alloc failed (ibm=%u dbm=%u blocks)\n",
                fs->ibitmap_blocks, fs->dbitmap_blocks);
        if (fs->ibitmap) kfree(fs->ibitmap);
        if (fs->dbitmap) kfree(fs->dbitmap);
        kfree(fs);
        return VFS_ERR_NOMEM;
    }

    if (fs->journal_start) {
        journal_recover(fs);
        kprintf("[tobyfs] journal enabled (blocks %u..%u)\n",
                fs->journal_start,
                fs->journal_start + fs->journal_size - 1);
    }

    /* Cache both (possibly multi-block) bitmaps in RAM for fast scan. AFTER
     * journal recovery, which may have replayed writes to the bitmap blocks. */
    int berr = 0;
    for (uint32_t s = 0; s < fs->ibitmap_blocks && !berr; s++)
        berr |= read_block(fs, fs->ibitmap_blk + s,
                           fs->ibitmap + (size_t)s * TFS_BLOCK_SIZE);
    for (uint32_t s = 0; s < fs->dbitmap_blocks && !berr; s++)
        berr |= read_block(fs, fs->dbitmap_blk + s,
                           fs->dbitmap + (size_t)s * TFS_BLOCK_SIZE);
    if (berr) {
        kprintf("[tobyfs] bitmap read failed\n");
        kfree(fs->ibitmap); kfree(fs->dbitmap); if (fs->ino_gen) kfree(fs->ino_gen); kfree(fs);
        return VFS_ERR_IO;
    }

    /* Sanity: root inode must exist and be a directory. */
    struct tfs_inode_disk root;
    if (read_inode(fs, fs->sb.root_ino, &root) != VFS_OK ||
        root.type != TFS_TYPE_DIR) {
        kprintf("[tobyfs] root inode %u missing or not a directory\n",
                fs->sb.root_ino);
        kfree(fs);
        return VFS_ERR_INVAL;
    }

    /* Milestone 28E: full integrity check BEFORE we expose the mount
     * to the rest of the kernel. We reject FATAL corruption, log a
     * warning for repair-friendly issues, and proceed. */
    {
        struct tobyfs_check chk;
        int crc = check_core(fs, &chk);
        if (crc == 0 && chk.severity == TFS_CHECK_FATAL) {
            kprintf("[tobyfs] REFUSING mount of '%s': fscheck FATAL "
                    "(%u errors): %s\n",
                    mount_point, chk.errors,
                    chk.detail[0] ? chk.detail : "(no detail)");
            if (slog_ready()) {
                SLOG_ERROR(SLOG_SUB_FS,
                           "refusing mount %s: %s",
                           mount_point,
                           chk.detail[0] ? chk.detail : "fscheck fatal");
            }
            kfree(fs->ibitmap); kfree(fs->dbitmap); if (fs->ino_gen) kfree(fs->ino_gen); kfree(fs);
            return VFS_ERR_INVAL;
        }
        if (crc == 0 && chk.severity == TFS_CHECK_WARN) {
            kprintf("[tobyfs] mount of '%s' WARNING: %u issue(s): %s "
                    "(mounting anyway)\n",
                    mount_point, chk.errors,
                    chk.detail[0] ? chk.detail : "");
            if (slog_ready()) {
                SLOG_WARN(SLOG_SUB_FS,
                          "mount %s warn: %s",
                          mount_point,
                          chk.detail[0] ? chk.detail : "fscheck warn");
            }
        } else if (crc == 0 && chk.severity == TFS_CHECK_OK) {
            kprintf("[tobyfs] mount of '%s' clean: inodes=%u/%u "
                    "blocks=%u/%u\n",
                    mount_point, chk.inodes_used, chk.inodes_total,
                    chk.data_blocks_used, chk.data_blocks_total);
            if (slog_ready()) {
                SLOG_INFO(SLOG_SUB_FS,
                          "mount %s clean (i=%u/%u b=%u/%u)",
                          mount_point,
                          chk.inodes_used, chk.inodes_total,
                          chk.data_blocks_used, chk.data_blocks_total);
            }
        } else {
            /* Check failed (likely I/O); log but mount anyway -- the
             * geometry checks above already passed. */
            kprintf("[tobyfs] fscheck for '%s' failed (rc=%d) -- mounting "
                    "without integrity verdict\n", mount_point, crc);
        }
    }

    int rc = vfs_mount(mount_point, &tobyfs_ops, fs);
    if (rc != VFS_OK) {
        kprintf("[tobyfs] vfs_mount('%s') failed: %d\n", mount_point, rc);
        kfree(fs->ibitmap); kfree(fs->dbitmap); if (fs->ino_gen) kfree(fs->ino_gen); kfree(fs);
        return rc;
    }

    /* Quick stats print so the boot log shows the FS came up. */
    uint32_t data_end = fs->journal_start ? fs->journal_start
                                          : fs->sb.total_blocks;
    uint32_t used_inodes = 0, used_blocks = 0;
    for (uint32_t i = 1; i < fs->sb.inode_count; i++) {
        if (bit_get(fs->ibitmap, i)) used_inodes++;
    }
    for (uint32_t b = fs->sb.data_blk_start; b < data_end; b++) {
        if (bit_get(fs->dbitmap, b)) used_blocks++;
    }
    kprintf("[tobyfs] mounted '%s' on %s: %u/%u inodes, %u/%u data blocks "
            "(%u KiB free)\n",
            mount_point, dev->name,
            used_inodes, fs->sb.inode_count - 1,
            used_blocks, data_end - fs->sb.data_blk_start,
            (data_end - fs->sb.data_blk_start - used_blocks) * 4);
    return VFS_OK;
}

/* -------- milestone 20: in-kernel mkfs --------
 *
 * Produces the exact same byte pattern as tools/mkfs_tobyfs.c so an
 * installed disk is interchangeable with a host-formatted one. Used by
 * the installer to stamp a fresh /data region after writing the boot
 * image to the front of the disk. */
/* Compute a TobyFS on-disk geometry sized to `total_blocks`. Layout is fully
 * contiguous so each region's span is recoverable from start deltas at mount:
 *   block 0                       superblock
 *   inode_bitmap_blk .. (ibm_blocks)   inode bitmap   (1 bit / inode)
 *   data_bitmap_blk  .. (dbm_blocks)   data bitmap    (1 bit / block)
 *   inode_table_blk  .. (itbl_blocks)  inode table
 *   data_blk_start   ..                data blocks
 *   journal_start    .. total_blocks   write-ahead journal (fixed size)
 * Returns 0 on success, -1 if the volume can't even hold the metadata. */
struct tfs_geom {
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t inode_bitmap_blk, ibm_blocks;
    uint32_t data_bitmap_blk,  dbm_blocks;
    uint32_t inode_table_blk,  itbl_blocks;
    uint32_t data_blk_start;
    uint32_t journal_start;
};

static int tfs_compute_geom(uint32_t total_blocks, struct tfs_geom *g) {
    if (total_blocks < TFS_TOTAL_BLOCKS) return -1;
    if (total_blocks > TFS_MAX_TOTAL_BLOCKS) total_blocks = TFS_MAX_TOTAL_BLOCKS;

    /* Scale inodes with the volume: ~1 inode per 4 blocks, floor 256, and
     * round UP to a whole inode-table block (TFS_INODES_PER_BLOCK each).
     *
     * WAS 1-per-64 (one inode per 256 KiB), which assumed big files. A browser
     * profile is the opposite workload -- thousands of small cache entries --
     * and it exhausted the inode table long before the space: a 256 MiB /data
     * carried only 1023 inodes, so Chromium's SECOND launch could not create a
     * file and its leveldb reported "OS or hardware error". Inodes are 128 B,
     * i.e. 32 per block, so this density costs 0.8% of the volume (2 MiB on
     * 256 MiB) -- the stingy version was not buying anything worth having.
     * 1-per-4-blocks == one inode per 16 KiB, which is ext4's default
     * bytes-per-inode. Geometry is per-volume and comes from the superblock at
     * mount, so existing images keep their own count and still mount. */
    uint32_t inodes = total_blocks / 4u;
    if (inodes < TFS_INODE_COUNT) inodes = TFS_INODE_COUNT;
    inodes = ((inodes + TFS_INODES_PER_BLOCK - 1) / TFS_INODES_PER_BLOCK)
             * TFS_INODES_PER_BLOCK;

    uint32_t ibm = (inodes + TFS_BITS_PER_BLOCK - 1) / TFS_BITS_PER_BLOCK;
    uint32_t dbm = (total_blocks + TFS_BITS_PER_BLOCK - 1) / TFS_BITS_PER_BLOCK;
    uint32_t itbl = inodes / TFS_INODES_PER_BLOCK;

    g->total_blocks     = total_blocks;
    g->inode_count      = inodes;
    g->inode_bitmap_blk = 1u;
    g->ibm_blocks       = ibm;
    g->data_bitmap_blk  = g->inode_bitmap_blk + ibm;
    g->dbm_blocks       = dbm;
    g->inode_table_blk  = g->data_bitmap_blk + dbm;
    g->itbl_blocks      = itbl;
    g->data_blk_start   = g->inode_table_blk + itbl;
    g->journal_start    = total_blocks - TFS_JOURNAL_BLOCKS;

    /* Need room for metadata + journal + at least a few data blocks. */
    if (g->data_blk_start + 8u >= g->journal_start) return -1;
    return 0;
}

int tobyfs_format(struct blk_dev *dev) {
    if (!dev || !dev->ops || !dev->ops->write) return VFS_ERR_INVAL;
    if (dev->sector_count < (uint64_t)TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK) {
        kprintf("[tobyfs] format: device too small (%lu sectors, need %u)\n",
                (unsigned long)dev->sector_count,
                TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK);
        return VFS_ERR_INVAL;
    }

    /* Size the volume to the device (capped at TFS_MAX_TOTAL_BLOCKS). */
    uint64_t dev_blocks = dev->sector_count / TFS_SECTORS_PER_BLOCK;
    uint32_t total = (dev_blocks > TFS_MAX_TOTAL_BLOCKS)
                     ? TFS_MAX_TOTAL_BLOCKS : (uint32_t)dev_blocks;
    struct tfs_geom g;
    if (tfs_compute_geom(total, &g) != 0) {
        kprintf("[tobyfs] format: cannot lay out geometry for %u blocks\n",
                total);
        return VFS_ERR_INVAL;
    }
    kprintf("[tobyfs] format: %u blocks (%lu MiB), %u inodes, "
            "ibm=%u@%u dbm=%u@%u itbl=%u@%u data@%u journal@%u\n",
            g.total_blocks,
            (unsigned long)((uint64_t)g.total_blocks * TFS_BLOCK_SIZE >> 20),
            g.inode_count, g.ibm_blocks, g.inode_bitmap_blk,
            g.dbm_blocks, g.data_bitmap_blk, g.itbl_blocks, g.inode_table_blk,
            g.data_blk_start, g.journal_start);

    uint8_t blk[TFS_BLOCK_SIZE];

    /* Block 0: superblock. */
    memset(blk, 0, sizeof(blk));
    struct tfs_superblock sb = {
        .magic            = TFS_MAGIC,
        .block_size       = TFS_BLOCK_SIZE,
        .total_blocks     = g.total_blocks,
        .inode_count      = g.inode_count,
        .inode_bitmap_blk = g.inode_bitmap_blk,
        .data_bitmap_blk  = g.data_bitmap_blk,
        .inode_table_blk  = g.inode_table_blk,
        .data_blk_start   = g.data_blk_start,
        .root_ino         = TFS_ROOT_INO,
    };
    sb.reserved[0] = g.journal_start;
    sb.reserved[1] = TFS_JOURNAL_BLOCKS;
    memcpy(blk, &sb, sizeof(sb));
    if (blk_write(dev, 0, TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;

    /* Inode bitmap (ibm_blocks blocks). Bit 0 reserved + bit 1 = root, in the
     * first block; the rest are all-zero (free). */
    for (uint32_t s = 0; s < g.ibm_blocks; s++) {
        memset(blk, 0, sizeof(blk));
        if (s == 0) blk[0] = 0x03;   /* inode 0 reserved, inode 1 = root */
        if (blk_write(dev,
                      (uint64_t)(g.inode_bitmap_blk + s) * TFS_SECTORS_PER_BLOCK,
                      TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;
    }

    /* Data bitmap (dbm_blocks blocks). Mark metadata blocks
     * [0, data_blk_start) and journal blocks [journal_start, total) as used so
     * the allocator skips them; everything else free. Build per-block. */
    for (uint32_t s = 0; s < g.dbm_blocks; s++) {
        memset(blk, 0, sizeof(blk));
        uint32_t base = s * TFS_BITS_PER_BLOCK;   /* first block index in this bitmap block */
        for (uint32_t bit = 0; bit < TFS_BITS_PER_BLOCK; bit++) {
            uint32_t b = base + bit;
            if (b >= g.total_blocks) { bit_set(blk, bit); continue; } /* past EOV */
            if (b < g.data_blk_start || b >= g.journal_start)
                bit_set(blk, bit);
        }
        if (blk_write(dev,
                      (uint64_t)(g.data_bitmap_blk + s) * TFS_SECTORS_PER_BLOCK,
                      TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;
    }

    /* Inode table: first block carries the root dir inode; rest zeroed. */
    memset(blk, 0, sizeof(blk));
    struct tfs_inode_disk *table = (struct tfs_inode_disk *)blk;
    table[TFS_ROOT_INO].type  = TFS_TYPE_DIR;
    table[TFS_ROOT_INO].nlink = 1;
    table[TFS_ROOT_INO].size  = 0;
    table[TFS_ROOT_INO].mtime = 0;
    table[TFS_ROOT_INO].mode  = TFS_DEFAULT_DIR_MODE;
    table[TFS_ROOT_INO].uid   = 0;
    table[TFS_ROOT_INO].gid   = 0;
    if (blk_write(dev,
                  (uint64_t)g.inode_table_blk * TFS_SECTORS_PER_BLOCK,
                  TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;

    memset(blk, 0, sizeof(blk));
    for (uint32_t i = 1; i < g.itbl_blocks; i++) {
        if (blk_write(dev,
                      (uint64_t)(g.inode_table_blk + i) * TFS_SECTORS_PER_BLOCK,
                      TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;
    }

    /* Zero the journal area so recovery never finds stale data. */
    memset(blk, 0, sizeof(blk));
    for (uint32_t b = g.journal_start; b < g.total_blocks; b++) {
        if (blk_write(dev, (uint64_t)b * TFS_SECTORS_PER_BLOCK,
                      TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;
    }

    return VFS_OK;
}

/* ============================================================
 *  Milestone 28E: integrity checker (tobyfs_check_*).
 *
 *  The strategy is intentionally a single-pass scan that NEVER writes
 *  back to disk -- it's safe to call on a mount that the rest of the
 *  kernel is concurrently writing to (worst case the in-flight bitmap
 *  bit gets seen as "unset just before" the inode that just claimed
 *  it; we report that as a "WARN: bitmap drift" rather than corruption).
 *
 *  Steps:
 *    1. Re-read the superblock and validate magic + geometry.
 *    2. Re-read both bitmaps.
 *    3. Walk the inode bitmap: for each allocated inode, read it and:
 *         - check type is TFS_TYPE_FILE or TFS_TYPE_DIR
 *         - check nlink >= 1
 *         - check direct[] entries are either 0 or in the data-block
 *           range AND set in the data bitmap.
 *    4. Sanity-check the root inode (must be type DIR, nlink>=1).
 *    5. Detail string is filled with the FIRST issue encountered so
 *       fscheck's text output is actionable.
 *
 *  Severity ladder:
 *    - bad magic / geometry -> FATAL (return immediately).
 *    - root inode not a directory -> FATAL.
 *    - inode points into a metadata block -> FATAL.
 *    - dirent referencing impossible inode -> FATAL.
 *    - inode references a data block that's NOT marked in dbitmap
 *      -> WARN (race window during a concurrent write).
 *    - allocated inode with type==FREE -> WARN.
 * ============================================================ */

static void check_record(struct tobyfs_check *r, int sev,
                         const char *fmt, ...) {
    if (!r) return;
    if (sev > r->severity) r->severity = sev;
    r->errors++;
    if (r->detail[0] == '\0' && fmt) {
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(r->detail, sizeof(r->detail), fmt, ap);
        va_end(ap);
    }
}

/* Count set bits in a bitmap up to `max_bits`. */
static uint32_t popcount_bitmap(const uint8_t *bm, uint32_t max_bits) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < max_bits; i++) {
        if (bm[i >> 3] & (uint8_t)(1u << (i & 7))) c++;
    }
    return c;
}

static int check_core(struct tobyfs *fs, struct tobyfs_check *out) {
    /* Defaults. */
    memset(out, 0, sizeof(*out));
    out->severity = TFS_CHECK_OK;
    out->inodes_total = fs->sb.inode_count;
    out->data_blocks_total =
        (fs->sb.total_blocks > fs->sb.data_blk_start)
            ? (fs->sb.total_blocks - fs->sb.data_blk_start) : 0;
    out->bytes_total =
        (uint64_t)fs->sb.total_blocks * fs->sb.block_size;

    /* 1. Verify the superblock again from the on-disk copy (the in-mem
     *    copy could have been corrupted by a previous OOPS). */
    uint8_t sbbuf[TFS_BLOCK_SIZE];
    if (read_block(fs, 0, sbbuf) != 0) {
        check_record(out, TFS_CHECK_FATAL, "superblock read failed");
        return -VFS_ERR_IO;
    }
    struct tfs_superblock *fresh = (struct tfs_superblock *)sbbuf;
    if (fresh->magic != TFS_MAGIC) {
        check_record(out, TFS_CHECK_FATAL,
                     "bad superblock magic 0x%lx (expected 0x%lx)",
                     (unsigned long)fresh->magic, (unsigned long)TFS_MAGIC);
        return 0;
    }
    /* Dynamic geometry: accept any in-range volume (matches mount). Only the
     * block size is fixed; total/inode counts vary with the device. Sanity-
     * check the layout is internally consistent rather than equal to fixed
     * constants. */
    if (fresh->block_size  != TFS_BLOCK_SIZE        ||
        fresh->total_blocks <  TFS_TOTAL_BLOCKS     ||
        fresh->total_blocks >  TFS_MAX_TOTAL_BLOCKS ||
        fresh->inode_count  == 0) {
        check_record(out, TFS_CHECK_FATAL,
                     "geometry invalid bs=%u total=%u inodes=%u",
                     fresh->block_size, fresh->total_blocks,
                     fresh->inode_count);
        return 0;
    }
    if (fresh->root_ino != TFS_ROOT_INO ||
        fresh->inode_bitmap_blk == 0 ||
        fresh->data_bitmap_blk <= fresh->inode_bitmap_blk ||
        fresh->inode_table_blk <= fresh->data_bitmap_blk ||
        fresh->data_blk_start  <= fresh->inode_table_blk ||
        fresh->data_blk_start  >= fresh->total_blocks) {
        check_record(out, TFS_CHECK_FATAL,
                     "region offsets inconsistent (root=%u ibm=%u dbm=%u "
                     "itab=%u dstart=%u total=%u)",
                     fresh->root_ino, fresh->inode_bitmap_blk,
                     fresh->data_bitmap_blk, fresh->inode_table_blk,
                     fresh->data_blk_start, fresh->total_blocks);
        return 0;
    }

    /* 2. Re-read both (possibly multi-block) bitmaps into fresh scratch
     *    buffers (rather than using fs->ibitmap/fs->dbitmap directly) so a
     *    check called before mount finishes still works -- and so a
     *    concurrent allocator write doesn't tear our scan. Spans are derived
     *    from the on-disk geometry. */
    uint32_t ibm_blocks =
        (fresh->inode_count  + TFS_BITS_PER_BLOCK - 1) / TFS_BITS_PER_BLOCK;
    uint32_t dbm_blocks =
        (fresh->total_blocks + TFS_BITS_PER_BLOCK - 1) / TFS_BITS_PER_BLOCK;
    uint8_t *ibm = (uint8_t *)kmalloc((size_t)ibm_blocks * TFS_BLOCK_SIZE);
    uint8_t *dbm = (uint8_t *)kmalloc((size_t)dbm_blocks * TFS_BLOCK_SIZE);
    if (!ibm || !dbm) {
        if (ibm) kfree(ibm);
        if (dbm) kfree(dbm);
        check_record(out, TFS_CHECK_FATAL, "no memory for fscheck scratch");
        return -VFS_ERR_NOMEM;
    }
    int rc = 0;
    int berr = 0;
    for (uint32_t s = 0; s < ibm_blocks && !berr; s++)
        berr |= read_block(fs, fresh->inode_bitmap_blk + s,
                           ibm + (size_t)s * TFS_BLOCK_SIZE);
    for (uint32_t s = 0; s < dbm_blocks && !berr; s++)
        berr |= read_block(fs, fresh->data_bitmap_blk + s,
                           dbm + (size_t)s * TFS_BLOCK_SIZE);
    if (berr) {
        check_record(out, TFS_CHECK_FATAL, "bitmap re-read failed");
        kfree(ibm); kfree(dbm);
        return -VFS_ERR_IO;
    }

    /* 3. Inode 0 is the "invalid" sentinel. tobyfs_format() *sets*
     *    bit 0 to 1 so the allocator never hands it out (alloc_inode
     *    starts its scan at i=1 anyway, but the set bit is the
     *    intentional belt-and-braces pattern). Don't flag it here --
     *    that would falsely reject every freshly-formatted image. */

    /* Root inode bit must be set. */
    if (!(ibm[TFS_ROOT_INO >> 3] & (uint8_t)(1u << (TFS_ROOT_INO & 7)))) {
        check_record(out, TFS_CHECK_FATAL,
                     "root inode %u not allocated in bitmap", TFS_ROOT_INO);
    }

    /* 4. Walk allocated inodes. */
    uint32_t inodes_used = 0;
    for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
        if (!(ibm[ino >> 3] & (uint8_t)(1u << (ino & 7)))) continue;
        inodes_used++;

        struct tfs_inode_disk node;
        uint32_t blk = fs->sb.inode_table_blk + (ino / TFS_INODES_PER_BLOCK);
        uint8_t  ibuf[TFS_BLOCK_SIZE];
        if (read_block(fs, blk, ibuf) != 0) {
            check_record(out, TFS_CHECK_FATAL,
                         "inode table read failed at blk=%u for ino=%u",
                         blk, ino);
            rc = -VFS_ERR_IO;
            break;
        }
        memcpy(&node, ibuf + (ino % TFS_INODES_PER_BLOCK) * TFS_INODE_SIZE,
               sizeof(node));

        if (node.type == TFS_TYPE_FREE) {
            check_record(out, TFS_CHECK_WARN,
                         "ino=%u allocated but type=FREE (orphan slot)",
                         ino);
            continue;
        }
        if (node.type != TFS_TYPE_FILE && node.type != TFS_TYPE_DIR) {
            check_record(out, TFS_CHECK_FATAL,
                         "ino=%u has invalid type=%u",
                         ino, (unsigned)node.type);
            continue;
        }
        if (node.nlink == 0) {
            check_record(out, TFS_CHECK_WARN,
                         "ino=%u has nlink=0", ino);
        }

        for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
            uint32_t db = node.direct[i];
            if (db == 0) continue;
            if (db < fs->sb.data_blk_start || db >= fs->sb.total_blocks) {
                check_record(out, TFS_CHECK_FATAL,
                             "ino=%u direct[%u]=%u OUT OF RANGE",
                             ino, i, db);
                continue;
            }
            if (!(dbm[db >> 3] & (uint8_t)(1u << (db & 7)))) {
                check_record(out, TFS_CHECK_WARN,
                             "ino=%u direct[%u]=%u not set in dbitmap",
                             ino, i, db);
            }
        }

        if (node.indirect != 0) {
            if (node.indirect < fs->sb.data_blk_start ||
                node.indirect >= fs->sb.total_blocks) {
                check_record(out, TFS_CHECK_FATAL,
                             "ino=%u indirect=%u OUT OF RANGE",
                             ino, node.indirect);
            } else {
                if (!(dbm[node.indirect >> 3] &
                      (uint8_t)(1u << (node.indirect & 7)))) {
                    check_record(out, TFS_CHECK_WARN,
                                 "ino=%u indirect=%u not set in dbitmap",
                                 ino, node.indirect);
                }
                uint8_t indirect_buf[TFS_BLOCK_SIZE];
                if (read_block(fs, node.indirect, indirect_buf) == 0) {
                    uint32_t *ientries = (uint32_t *)indirect_buf;
                    for (uint32_t ii = 0; ii < TFS_INDIRECT_ENTRIES; ii++) {
                        uint32_t ib = ientries[ii];
                        if (ib == 0) continue;
                        if (ib < fs->sb.data_blk_start ||
                            ib >= fs->sb.total_blocks) {
                            check_record(out, TFS_CHECK_FATAL,
                                         "ino=%u indirect[%u]=%u OUT OF RANGE",
                                         ino, ii, ib);
                        } else if (!(dbm[ib >> 3] &
                                     (uint8_t)(1u << (ib & 7)))) {
                            check_record(out, TFS_CHECK_WARN,
                                         "ino=%u indirect[%u]=%u not set in dbitmap",
                                         ino, ii, ib);
                        }
                    }
                }
            }
        }

        /* Double-indirect tree: validate the L1 root, each L2 block, and the
         * data pointers within. Same range + dbitmap checks as the single-
         * indirect walk, one level deeper. */
        if (node.dbl_indirect != 0) {
            if (node.dbl_indirect < fs->sb.data_blk_start ||
                node.dbl_indirect >= fs->sb.total_blocks) {
                check_record(out, TFS_CHECK_FATAL,
                             "ino=%u dbl_indirect=%u OUT OF RANGE",
                             ino, node.dbl_indirect);
            } else {
                if (!(dbm[node.dbl_indirect >> 3] &
                      (uint8_t)(1u << (node.dbl_indirect & 7)))) {
                    check_record(out, TFS_CHECK_WARN,
                                 "ino=%u dbl_indirect=%u not set in dbitmap",
                                 ino, node.dbl_indirect);
                }
                uint8_t l1buf[TFS_BLOCK_SIZE];
                if (read_block(fs, node.dbl_indirect, l1buf) == 0) {
                    uint32_t *l1e = (uint32_t *)l1buf;
                    for (uint32_t li = 0; li < TFS_INDIRECT_ENTRIES; li++) {
                        uint32_t l2b = l1e[li];
                        if (l2b == 0) continue;
                        if (l2b < fs->sb.data_blk_start ||
                            l2b >= fs->sb.total_blocks) {
                            check_record(out, TFS_CHECK_FATAL,
                                         "ino=%u dbl L1[%u]=%u OUT OF RANGE",
                                         ino, li, l2b);
                            continue;
                        }
                        if (!(dbm[l2b >> 3] & (uint8_t)(1u << (l2b & 7)))) {
                            check_record(out, TFS_CHECK_WARN,
                                         "ino=%u dbl L1[%u]=%u not set in dbitmap",
                                         ino, li, l2b);
                        }
                        uint8_t l2buf[TFS_BLOCK_SIZE];
                        if (read_block(fs, l2b, l2buf) != 0) continue;
                        uint32_t *l2e = (uint32_t *)l2buf;
                        for (uint32_t lj = 0; lj < TFS_INDIRECT_ENTRIES; lj++) {
                            uint32_t db2 = l2e[lj];
                            if (db2 == 0) continue;
                            if (db2 < fs->sb.data_blk_start ||
                                db2 >= fs->sb.total_blocks) {
                                check_record(out, TFS_CHECK_FATAL,
                                             "ino=%u dbl[%u][%u]=%u OUT OF RANGE",
                                             ino, li, lj, db2);
                            } else if (!(dbm[db2 >> 3] &
                                         (uint8_t)(1u << (db2 & 7)))) {
                                check_record(out, TFS_CHECK_WARN,
                                             "ino=%u dbl[%u][%u]=%u not set in dbitmap",
                                             ino, li, lj, db2);
                            }
                        }
                    }
                }
            }
        }

        if (ino == fs->sb.root_ino && node.type != TFS_TYPE_DIR) {
            check_record(out, TFS_CHECK_FATAL,
                         "root inode %u is not a directory (type=%u)",
                         ino, (unsigned)node.type);
        }
    }

    out->inodes_used = inodes_used;
    /* Count data blocks: skip metadata bits 0..data_blk_start-1 which
     * are always set by mkfs. */
    uint32_t dbits = popcount_bitmap(dbm, fs->sb.total_blocks);
    out->data_blocks_used =
        (dbits >= fs->sb.data_blk_start) ? (dbits - fs->sb.data_blk_start) : 0;
    out->bytes_free =
        (uint64_t)(out->data_blocks_total - out->data_blocks_used) *
        fs->sb.block_size;

    kfree(ibm);
    kfree(dbm);
    return rc;
}

int tobyfs_check_dev(struct blk_dev *dev, struct tobyfs_check *out) {
    if (!dev || !out) return -VFS_ERR_INVAL;

    /* Build a temporary tobyfs handle on the heap so we can re-use
     * read_block(). We copy in the superblock first (to populate
     * fs->sb for the rest of check_core); if that fails, mark FATAL. */
    struct tobyfs *fs = (struct tobyfs *)kcalloc(1, sizeof(*fs));
    if (!fs) {
        memset(out, 0, sizeof(*out));
        out->severity = TFS_CHECK_FATAL;
        check_record(out, TFS_CHECK_FATAL, "no memory for fscheck handle");
        return -VFS_ERR_NOMEM;
    }
    fs->dev = dev;

    /* Load the REAL on-disk superblock so check_core's inode walk uses the
     * actual (dynamic) geometry. check_core re-reads + revalidates the
     * superblock itself; this just gives it the data_blk_start/total_blocks
     * the scan relies on. If the read fails or the magic is wrong, leave
     * fs->sb zeroed and let check_core report it. */
    {
        uint8_t sbbuf[TFS_BLOCK_SIZE];
        if (read_block(fs, 0, sbbuf) == 0)
            memcpy(&fs->sb, sbbuf, sizeof(fs->sb));
    }

    int rc = check_core(fs, out);
    kfree(fs);
    return rc;
}

int tobyfs_check_mounted(void *mount_data, struct tobyfs_check *out) {
    if (!mount_data || !out) return -VFS_ERR_INVAL;
    struct tobyfs *fs = (struct tobyfs *)mount_data;
    return check_core(fs, out);
}

/* ============================================================
 *  Milestone 28E: in-kernel ramdisk-backed self-test.
 *
 *  The corruption-detection requirement says "simulate corrupted
 *  filesystem (test image), verify corruption detected, safe error
 *  shown, no kernel crash". We satisfy that without ever touching
 *  the live /data disk by:
 *
 *    1. allocating a 4 MiB heap buffer (one tobyfs image worth),
 *    2. wrapping it as a tiny in-RAM blk_dev,
 *    3. formatting it (real tobyfs_format),
 *    4. running tobyfs_check_dev twice -- once on the clean image
 *       (expect OK), once after stomping the superblock magic
 *       (expect FATAL).
 *
 *  Pure heap; nothing persists; safe to call from the boot harness.
 * ============================================================ */

struct tfs_ramdev {
    uint8_t *buf;
    uint64_t bytes;
};

static int ramdev_read(struct blk_dev *dev, uint64_t lba, uint32_t count,
                       void *buf) {
    struct tfs_ramdev *r = (struct tfs_ramdev *)dev->priv;
    uint64_t off  = lba * (uint64_t)BLK_SECTOR_SIZE;
    uint64_t span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(buf, r->buf + off, span);
    return 0;
}

static int ramdev_write(struct blk_dev *dev, uint64_t lba, uint32_t count,
                        const void *buf) {
    struct tfs_ramdev *r = (struct tfs_ramdev *)dev->priv;
    uint64_t off  = lba * (uint64_t)BLK_SECTOR_SIZE;
    uint64_t span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(r->buf + off, buf, span);
    return 0;
}

static const struct blk_ops ramdev_ops = {
    .read  = ramdev_read,
    .write = ramdev_write,
};

int tobyfs_self_test(struct tobyfs_check *clean_out,
                     struct tobyfs_check *bad_out) {
    if (!clean_out || !bad_out) return -VFS_ERR_INVAL;
    memset(clean_out, 0, sizeof(*clean_out));
    memset(bad_out,   0, sizeof(*bad_out));

    /* Use a 16 MiB volume (4096 blocks) -- past the ~4.06 MiB single-indirect
     * limit (double-indirect begins at file block 16+1024=1040 => ~4.06 MiB),
     * so a 5 MiB file exercises the new double-indirect path AND dynamic
     * geometry, while keeping the heap footprint modest (the image buffer is
     * heap-allocated, so smaller is friendlier to the kernel allocator). */
    const uint32_t test_blocks = 4096u;            /* 16 MiB */
    const uint64_t bytes = (uint64_t)test_blocks * TFS_BLOCK_SIZE;

    uint8_t *image = (uint8_t *)kmalloc((size_t)bytes);
    if (!image) {
        kprintf("[m28e] self-test: kmalloc(%lu) failed\n",
                (unsigned long)bytes);
        return -VFS_ERR_NOMEM;
    }
    memset(image, 0, (size_t)bytes);

    struct tfs_ramdev r = { .buf = image, .bytes = bytes };
    struct blk_dev    dev;
    memset(&dev, 0, sizeof(dev));
    dev.name         = "ramfs-fscheck-test";
    dev.ops          = &ramdev_ops;
    dev.sector_count = bytes / BLK_SECTOR_SIZE;
    dev.priv         = &r;
    dev.class        = BLK_CLASS_DISK;

    /* 1. Format -> expect a clean image (now device-sized). */
    int rc = tobyfs_format(&dev);
    if (rc != VFS_OK) {
        kprintf("[m28e] self-test: tobyfs_format failed rc=%d\n", rc);
        kfree(image);
        return rc;
    }
    kprintf("[m28e] self-test: ramdev formatted (%u blocks, %lu bytes)\n",
            test_blocks, (unsigned long)bytes);

    /* 2. Check clean. */
    rc = tobyfs_check_dev(&dev, clean_out);
    kprintf("[m28e] self-test: clean check rc=%d sev=%d errors=%u "
            "inodes=%u/%u dblocks=%u/%u detail=\"%s\"\n",
            rc, clean_out->severity, clean_out->errors,
            clean_out->inodes_used, clean_out->inodes_total,
            clean_out->data_blocks_used, clean_out->data_blocks_total,
            clean_out->detail);
    if (rc != 0) {
        kprintf("[m28e] self-test: clean check returned non-zero rc=%d\n", rc);
        kfree(image);
        return rc;
    }

    /* 2b. BIG-FILE test: mount the ramdev, write a >4 MiB file (forcing the
     *     double-indirect path), read it back, and verify every byte. This
     *     is the QEMU-verifiable proof that double-indirect + dynamic sizing
     *     work end-to-end. Writes go in 16 KiB chunks (4 blocks) so each
     *     vfs_write stays well under the 16-block journal transaction cap. */
    {
        const char *mp   = "/tfs_selftest";
        const char *path = "/tfs_selftest/big.bin";
        const size_t big_size = 5u * 1024u * 1024u;     /* 5 MiB > 4 MiB cap */
        const size_t chunk    = 16u * 1024u;            /* 4 blocks/write */
        int mrc = tobyfs_mount(mp, &dev);
        if (mrc != VFS_OK) {
            kprintf("[m28e] self-test: BIG-FILE mount failed rc=%d\n", mrc);
            kfree(image);
            return mrc;
        }

        uint8_t *wbuf = (uint8_t *)kmalloc(chunk);
        uint8_t *rbuf = (uint8_t *)kmalloc(chunk);
        int big_ok = (wbuf && rbuf);
        if (!big_ok)
            kprintf("[m28e] self-test: BIG-FILE buf alloc failed "
                    "(wbuf=%p rbuf=%p)\n", (void *)wbuf, (void *)rbuf);

        if (big_ok) {
            int crc2 = vfs_create(path);
            if (crc2 != VFS_OK) {
                kprintf("[m28e] self-test: BIG-FILE create rc=%d\n", crc2);
                big_ok = 0;
            }
        }

        /* Write: byte at offset i is a position-derived pattern so a
         * misdirected block (the classic indirect bug) is caught. */
        if (big_ok) {
            struct vfs_file f;
            int orc = vfs_open(path, &f);
            if (orc != VFS_OK) {
                kprintf("[m28e] self-test: BIG-FILE open(write) rc=%d\n", orc);
                big_ok = 0;
            } else {
                size_t done = 0;
                while (done < big_size && big_ok) {
                    size_t this = big_size - done;
                    if (this > chunk) this = chunk;
                    for (size_t k = 0; k < this; k++)
                        wbuf[k] = (uint8_t)((done + k) * 31u + 7u);
                    long w = vfs_write(&f, wbuf, this);
                    if (w != (long)this) {
                        kprintf("[m28e] self-test: BIG-FILE write short "
                                "at %lu got=%ld\n", (unsigned long)done, w);
                        big_ok = 0;
                    }
                    done += this;
                }
                vfs_close(&f);
            }
        }

        /* Read back + verify every byte. */
        if (big_ok) {
            struct vfs_file f;
            if (vfs_open(path, &f) != VFS_OK) { big_ok = 0; }
            else {
                size_t done = 0;
                while (done < big_size && big_ok) {
                    size_t this = big_size - done;
                    if (this > chunk) this = chunk;
                    long got = vfs_read(&f, rbuf, this);
                    if (got != (long)this) {
                        kprintf("[m28e] self-test: BIG-FILE read short "
                                "at %lu got=%ld\n", (unsigned long)done, got);
                        big_ok = 0;
                        break;
                    }
                    for (size_t k = 0; k < this; k++) {
                        if (rbuf[k] != (uint8_t)((done + k) * 31u + 7u)) {
                            kprintf("[m28e] self-test: BIG-FILE MISMATCH at "
                                    "offset %lu\n", (unsigned long)(done + k));
                            big_ok = 0;
                            break;
                        }
                    }
                    done += this;
                }
                vfs_close(&f);
            }
        }

        if (wbuf) kfree(wbuf);
        if (rbuf) kfree(rbuf);
        (void)vfs_unmount(mp);

        kprintf("[m28e] self-test: BIG-FILE 5 MiB write/read/verify %s "
                "(double-indirect path)\n", big_ok ? "PASS" : "FAIL");
    }

    /* 3. Stomp the superblock magic. The first 8 bytes of block 0 are
     *    the magic field (verified by check_core). Any value != TFS_MAGIC
     *    must trip TFS_CHECK_FATAL. */
    uint64_t bad_magic = 0xDEADBEEFCAFEBABEULL;
    memcpy(image, &bad_magic, sizeof(bad_magic));

    rc = tobyfs_check_dev(&dev, bad_out);
    kprintf("[m28e] self-test: corrupt check rc=%d sev=%d errors=%u "
            "detail=\"%s\"\n",
            rc, bad_out->severity, bad_out->errors, bad_out->detail);

    /* The block cache may still hold (possibly dirty) entries keyed to our
     * on-STACK ramdev `dev`. Once this function returns, that stack memory is
     * reused -- a later bcache_sync()/eviction would then dereference a stale
     * device pointer and fault (seen as a #PF in bcache_get). Drop every
     * cached entry for this device now, while `dev` is still valid, so the
     * cache holds nothing pointing at transient stack storage. This is also
     * why the original read-only self-test never crashed: it never dirtied or
     * cached writes for the stack dev; mounting + writing a file does. */
    bcache_invalidate(&dev);

    kfree(image);
    return 0;
}

/* ============================================================
 *  Crash-consistency + stress harness (-DTOBYFS_STRESS_SELFTEST).
 *
 *  Part 1 injects a power loss at each journal phase of a single
 *  operation and proves remount+recovery is atomic (rollback vs
 *  replay). Part 2 churns the filesystem with hundreds of mixed
 *  operations (incl. a double-indirect 4.5 MiB file), verifying
 *  every byte and running the integrity checker throughout.
 *
 *  The journal's [header][data][commit] log is written with blk_write
 *  (direct to device); the checkpoint + live data sit in the write-back
 *  block cache. So "power loss" = bcache_invalidate() (drop the cache),
 *  leaving only what reached the device -- exactly what recovery must
 *  cope with. [TFST] markers.
 * ============================================================ */

/* 4 blocks/write. This used to be a REQUIREMENT -- tobyfs_write ran the whole
 * call in one journal transaction, so anything over TFS_TXN_MAX_BLOCKS (16)
 * failed outright, and this test chunked to stay under it. That constraint is
 * gone as of 2026-08-07: tobyfs_write now commits and reopens the transaction
 * mid-write, so a write of any size succeeds.
 *
 * The value is kept because it is now a useful property of THIS test rather
 * than a workaround: small writes mean many transactions per file, which is
 * what makes the crash-injection cases (t1/t2) land at interesting points.
 * Note the consequence of the fix, deliberately accepted: a single large
 * write() is no longer atomic across a power loss -- it spans several
 * transactions, exactly as ext4/xfs behave. Each transaction is still
 * complete-or-absent, so metadata stays consistent, which is the guarantee
 * that actually matters. */
#define TFST_CHUNK (16u * 1024u)

/* Write `size` bytes of a seed+offset-derived pattern to `path`. */
static int tfst_write(const char *path, uint32_t seed, size_t size,
                      uint8_t *chunk) {
    if (vfs_create(path) != VFS_OK) return -1;
    struct vfs_file f;
    if (vfs_open(path, &f) != VFS_OK) return -1;
    size_t done = 0;
    int rc = 0;
    while (done < size) {
        size_t this = size - done;
        if (this > TFST_CHUNK) this = TFST_CHUNK;
        for (size_t k = 0; k < this; k++)
            chunk[k] = (uint8_t)(seed * 131u + (done + k) * 17u +
                                 ((done + k) >> 8) * 7u);
        if (vfs_write(&f, chunk, this) != (long)this) { rc = -1; break; }
        done += this;
    }
    vfs_close(&f);
    return rc;
}

/* Read `path` back and verify the pattern. Returns 0 on a perfect match. */
static int tfst_verify(const char *path, uint32_t seed, size_t size,
                       uint8_t *chunk) {
    struct vfs_file f;
    if (vfs_open(path, &f) != VFS_OK) return -1;
    size_t done = 0;
    int rc = 0;
    while (done < size) {
        size_t this = size - done;
        if (this > TFST_CHUNK) this = TFST_CHUNK;
        if (vfs_read(&f, chunk, this) != (long)this) { rc = -1; break; }
        for (size_t k = 0; k < this; k++)
            if (chunk[k] != (uint8_t)(seed * 131u + (done + k) * 17u +
                                      ((done + k) >> 8) * 7u)) { rc = -1; break; }
        if (rc) break;
        done += this;
    }
    vfs_close(&f);
    return rc;
}

static void tfst_path(char *buf, int slot) {
    const char *base = "/tfst/f";
    int i = 0;
    while (base[i]) { buf[i] = base[i]; i++; }
    char num[12]; int j = 0; int n = slot;
    if (n == 0) num[j++] = '0';
    while (n > 0) { num[j++] = (char)('0' + n % 10); n /= 10; }
    while (j > 0) buf[i++] = num[--j];
    buf[i] = 0;
}

/* Create `name` under /tfst with a crash injected at `phase`, then model a
 * power loss (drop the cache) and remount (which recovers). Returns true
 * if the post-recovery filesystem is non-FATAL consistent. */
static bool tfst_crash_create(struct blk_dev *dev, const char *mp,
                              const char *name, int phase) {
    g_tfs_crash_phase = phase;
    (void)vfs_create(name);          /* journal_commit halts at `phase` */
    g_tfs_crash_phase = TFS_CRASH_NONE;
    bcache_discard(dev);             /* power loss: lose the write-back cache */
    (void)vfs_unmount(mp);
    return tobyfs_mount(mp, dev) == VFS_OK;   /* reboot -> journal_recover */
}

int tobyfs_crash_stress_test(void) {
    int fails = 0;
    kprintf("[TFST] tobyfs crash-consistency + stress self-test\n");

    const uint32_t test_blocks = 4096u;             /* 16 MiB */
    const uint64_t bytes = (uint64_t)test_blocks * TFS_BLOCK_SIZE;
    uint8_t *image = (uint8_t *)kmalloc((size_t)bytes);
    uint8_t *chunk = (uint8_t *)kmalloc(TFST_CHUNK);
    if (!image || !chunk) {
        kprintf("[TFST] SKIP -- kmalloc failed\n");
        if (image) kfree(image); if (chunk) kfree(chunk);
        return -1;
    }
    memset(image, 0, (size_t)bytes);

    struct tfs_ramdev r = { .buf = image, .bytes = bytes };
    struct blk_dev dev; memset(&dev, 0, sizeof(dev));
    dev.name = "tfs-crashstress"; dev.ops = &ramdev_ops;
    dev.sector_count = bytes / BLK_SECTOR_SIZE; dev.priv = &r;
    dev.class = BLK_CLASS_DISK;

    const char *mp = "/tfst";
    if (tobyfs_format(&dev) != VFS_OK ||
        tobyfs_mount(mp, &dev) != VFS_OK) {
        kprintf("[TFST] format/mount FAIL\n");
        bcache_invalidate(&dev); kfree(image); kfree(chunk);
        return -1;
    }

    /* baseline file, made durable on the device before any crash. */
    if (tfst_write("/tfst/keep", 0x11, 8192, chunk) != 0) fails++;
    bcache_sync(&dev);

    struct vfs_stat st;
    struct tobyfs_check chk;

    /* ---- Part 1a: crash before the commit marker -> rolled back. ---- */
    {
        bool mounted = tfst_crash_create(&dev, mp, "/tfst/victim",
                                         TFS_CRASH_BEFORE_COMMIT);
        bool victim_absent = mounted && vfs_stat("/tfst/victim", &st) != VFS_OK;
        bool keep_ok = mounted && tfst_verify("/tfst/keep", 0x11, 8192, chunk) == 0;
        tobyfs_check_dev(&dev, &chk);
        bool ok = victim_absent && keep_ok && chk.severity != TFS_CHECK_FATAL;
        if (!ok) fails++;
        kprintf("[TFST] t1 crash<commit rollback: %s "
                "(victim_absent=%d keep_ok=%d chk_sev=%d)\n",
                ok ? "PASS" : "FAIL", victim_absent, keep_ok, chk.severity);
    }

    /* ---- Part 1b: crash after the commit marker -> replayed. ---- */
    {
        bcache_sync(&dev);            /* current consistent state -> device */
        bool mounted = tfst_crash_create(&dev, mp, "/tfst/survivor",
                                         TFS_CRASH_AFTER_COMMIT);
        bool survivor_present = mounted && vfs_stat("/tfst/survivor", &st) == VFS_OK;
        bool keep_ok = mounted && tfst_verify("/tfst/keep", 0x11, 8192, chunk) == 0;
        tobyfs_check_dev(&dev, &chk);
        bool ok = survivor_present && keep_ok && chk.severity != TFS_CHECK_FATAL;
        if (!ok) fails++;
        kprintf("[TFST] t2 crash>commit replay: %s "
                "(survivor_present=%d keep_ok=%d chk_sev=%d)\n",
                ok ? "PASS" : "FAIL", survivor_present, keep_ok, chk.severity);
    }

    /* ---- Part 2a: a 4.5 MiB file exercises the double-indirect path. ---- */
    {
        const size_t big = 4608u * 1024u;          /* 4.5 MiB > 4.06 MiB */
        bool ok = tfst_write("/tfst/big", 0xC3, big, chunk) == 0 &&
                  tfst_verify("/tfst/big", 0xC3, big, chunk) == 0;
        (void)vfs_unlink("/tfst/big");             /* free the space again */
        if (!ok) fails++;
        kprintf("[TFST] t3 double-indirect 4.5 MiB write/verify: %s\n",
                ok ? "PASS" : "FAIL");
    }

    /* ---- Part 2b: random churn with content + integrity verification. ---- */
    {
        enum { NFILES = 24, ITERS = 80 };
        struct { int live; uint32_t seed; size_t size; } f[NFILES];
        for (int i = 0; i < NFILES; i++) f[i].live = 0;
        size_t live_bytes = 0;
        const size_t budget = 9u * 1024 * 1024;    /* keep well under volume */
        uint32_t rng = 0x9e3779b9u;
        int verified = 0, did = 0, bad = 0;

        for (int it = 0; it < ITERS; it++) {
            rng = rng * 1664525u + 1013904223u; uint32_t roll = rng % 100;
            rng = rng * 1664525u + 1013904223u; int slot = (int)(rng % NFILES);
            char path[40]; tfst_path(path, slot);

            if (!f[slot].live && roll < 60) {
                rng = rng * 1664525u + 1013904223u;
                size_t sz = (roll < 45)
                    ? 4096u + (rng % (60u * 1024))            /* small */
                    : 64u * 1024 + (rng % (2u * 1024 * 1024));/* indirect */
                if (live_bytes + sz > budget) continue;
                if (tfst_write(path, (uint32_t)(slot * 7 + it + 1), sz, chunk) == 0 &&
                    tfst_verify(path, (uint32_t)(slot * 7 + it + 1), sz, chunk) == 0) {
                    f[slot].live = 1; f[slot].seed = (uint32_t)(slot * 7 + it + 1);
                    f[slot].size = sz; live_bytes += sz; verified++;
                } else bad++;
                did++;
            } else if (f[slot].live && roll < 80) {
                /* rewrite: unlink + recreate at a new size/seed */
                (void)vfs_unlink(path); live_bytes -= f[slot].size; f[slot].live = 0;
                rng = rng * 1664525u + 1013904223u;
                size_t sz = 4096u + (rng % (128u * 1024));
                if (live_bytes + sz > budget) { did++; continue; }
                if (tfst_write(path, (uint32_t)(slot * 13 + it + 3), sz, chunk) == 0 &&
                    tfst_verify(path, (uint32_t)(slot * 13 + it + 3), sz, chunk) == 0) {
                    f[slot].live = 1; f[slot].seed = (uint32_t)(slot * 13 + it + 3);
                    f[slot].size = sz; live_bytes += sz; verified++;
                } else bad++;
                did++;
            } else if (f[slot].live) {
                (void)vfs_unlink(path); live_bytes -= f[slot].size; f[slot].live = 0;
                did++;
            }

            if ((it & 15) == 15) {
                tobyfs_check_dev(&dev, &chk);
                if (chk.severity == TFS_CHECK_FATAL) { bad++; break; }
            }
        }

        /* Re-verify every surviving file end-to-end. */
        int survivors = 0;
        for (int i = 0; i < NFILES; i++) {
            if (!f[i].live) continue;
            char path[40]; tfst_path(path, i);
            if (tfst_verify(path, f[i].seed, f[i].size, chunk) != 0) bad++;
            else survivors++;
        }
        tobyfs_check_dev(&dev, &chk);
        bool ok = (bad == 0) && chk.severity != TFS_CHECK_FATAL;
        if (!ok) fails++;
        kprintf("[TFST] t4 churn %d ops (%d writes verified, %d survivors): %s "
                "(bad=%d final_chk_sev=%d used=%u/%u blocks)\n",
                did, verified, survivors, ok ? "PASS" : "FAIL", bad, chk.severity,
                chk.data_blocks_used, chk.data_blocks_total);
    }

    (void)vfs_unmount(mp);
    bcache_invalidate(&dev);          /* drop cache keyed to the stack dev */
    kfree(image);
    kfree(chunk);
    kprintf("[TFST] %s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : -1;
}
