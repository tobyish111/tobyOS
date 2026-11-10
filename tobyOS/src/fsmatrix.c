/* fsmatrix.c -- the CRUD contract, asked of EVERY filesystem driver.
 *
 * WHY THIS EXISTS. logs/fsprobe.sh runs a full create/read/update/rename/
 * truncate/chmod/delete matrix from userspace -- but only over `/`, `/etc`,
 * `/tmp` and `/data`, which are ramfs, tmpfs and tobyfs. NOTHING exercised
 * ext4 or FAT metadata, and the capability table showed why that mattered:
 *
 *     driver   write create unlink rename mkdir trunc chmod utimes statfs link
 *     tobyfs     Y     Y      Y      Y      Y     Y     Y     Y      Y     Y
 *     tmpfs      Y     Y      Y      Y      Y     Y     Y     Y      Y     Y
 *     ramfs      Y     Y      Y      Y      Y     Y     Y     Y      Y     Y
 *     ext2       Y     Y      Y      Y      Y     Y     Y     Y      Y     Y
 *     ext4       Y     Y      Y      Y      Y     Y     Y     Y      Y     Y
 *     fat32      Y     Y      Y      Y      Y     Y     -     Y      Y     -
 *
 * (2026-08-25: every `-` in the trunc/chmod/utimes/statfs/link columns
 * above used to be a gap. The two that remain are real properties of FAT,
 * not omissions: it has no permission bits and no inode for a second name
 * to point at.)
 *
 * A NULL op is not a stub: the VFS turns it straight into VFS_ERR_ROFS, so
 * every gap above is silent until something needs it. `make`, `tar` and
 * `cp -p` all care about timestamps; a build on an ext4 volume would
 * quietly misbehave and nothing in the tree would have said so.
 *
 * This runs the matrix IN THE KERNEL against a freshly formatted RAM-backed
 * volume per driver, so it needs no disk, no fixture, and no userspace. It
 * asserts VALUES -- byte counts, sizes, modes, timestamps read back -- not
 * exit codes.
 *
 * fat32 is absent by necessity, not oversight: there is no in-kernel FAT
 * formatter, so a FAT volume cannot be conjured here. Said out loud at the
 * end rather than left as a silent hole in the coverage.
 */

#include <tobyos/blk.h>
#include <tobyos/vfs.h>
#include <tobyos/tobyfs.h>
#include <tobyos/ext4.h>
#include <tobyos/fat32.h>
#include <tobyos/bcache.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

static int g_fsm_pass, g_fsm_fail;

static void fsm_ok(const char *fs, const char *what, const char *detail) {
    g_fsm_pass++;
    kprintf("[FSMATRIX]   ok   %-7s %-28s %s\n", fs, what, detail ? detail : "");
}
static void fsm_bad(const char *fs, const char *what, const char *detail) {
    g_fsm_fail++;
    kprintf("[FSMATRIX]  FAIL  %-7s %-28s %s\n", fs, what, detail ? detail : "");
}
static void fsm_chk(int cond, const char *fs, const char *what,
                    const char *detail) {
    if (cond) fsm_ok(fs, what, detail); else fsm_bad(fs, what, detail);
}

/* ---- a RAM-backed disk to format ------------------------------------- */

struct fsm_ram { uint8_t *buf; uint64_t bytes; };

static int fsm_read(struct blk_dev *d, uint64_t lba, uint32_t n, void *buf) {
    struct fsm_ram *r = (struct fsm_ram *)d->priv;
    uint64_t off = lba * BLK_SECTOR_SIZE, len = (uint64_t)n * BLK_SECTOR_SIZE;
    if (off + len > r->bytes) return -1;
    memcpy(buf, r->buf + off, (size_t)len);
    return 0;
}
static int fsm_write(struct blk_dev *d, uint64_t lba, uint32_t n,
                     const void *buf) {
    struct fsm_ram *r = (struct fsm_ram *)d->priv;
    uint64_t off = lba * BLK_SECTOR_SIZE, len = (uint64_t)n * BLK_SECTOR_SIZE;
    if (off + len > r->bytes) return -1;
    memcpy(r->buf + off, buf, (size_t)len);
    return 0;
}
static const struct blk_ops g_fsm_ops = { .read = fsm_read, .write = fsm_write };

/* Static: a mounted volume's blk_dev must outlive this function. */
static struct blk_dev g_fsm_dev;
static struct fsm_ram g_fsm_ram;

/* `fill` is the byte the raw device is primed with BEFORE the formatter
 * runs. Zero models a pristine device; 0xFF models the honest case -- a
 * stick that already held something. That distinction is not cosmetic:
 * tobyfs_format deliberately does NOT zero the bulk of the inode table
 * (lazy_itable_init), so a fixture that hands the formatter an all-zero
 * device cannot tell "format cleared the table" from "format skipped the
 * table and the bitmap is doing its job". Only a dirty fixture can. */
static struct blk_dev *fsm_make_disk_filled(const char *name, uint64_t sectors,
                                            uint8_t fill) {
    /* One fixture at a time: give the previous volume's memory back
     * before minting the next, or the second driver tested doubles the
     * peak. Callers unmount first. */
    if (g_fsm_ram.buf) { kfree(g_fsm_ram.buf); g_fsm_ram.buf = 0; }
    g_fsm_ram.bytes = sectors * BLK_SECTOR_SIZE;
    g_fsm_ram.buf   = (uint8_t *)kmalloc((size_t)g_fsm_ram.bytes);
    if (!g_fsm_ram.buf) return 0;
    memset(g_fsm_ram.buf, fill, (size_t)g_fsm_ram.bytes);
    memset(&g_fsm_dev, 0, sizeof g_fsm_dev);
    g_fsm_dev.name         = name;
    g_fsm_dev.ops          = &g_fsm_ops;
    g_fsm_dev.sector_count = sectors;
    g_fsm_dev.priv         = &g_fsm_ram;
    g_fsm_dev.class        = BLK_CLASS_DISK;
    return &g_fsm_dev;
}

static struct blk_dev *fsm_make_disk(const char *name, uint64_t sectors) {
    return fsm_make_disk_filled(name, sectors, 0x00);
}

/* ---- a SPARSE RAM disk, for volumes far larger than RAM ---------------
 *
 * The dense fixture above allocates every byte, which caps it at a few tens
 * of MiB -- and a volume that small has exactly ONE data-bitmap block, so it
 * cannot exercise the paged bitmap at all. Everything interesting about
 * paging (skipping a full bitmap block via the summary, the allocation hint,
 * crossing from one bitmap block to the next) only exists above 32768
 * blocks, and the case that matters most -- a volume whose METADATA alone
 * fills bitmap block 0, so every allocation must skip it -- needs 16 GiB.
 *
 * Chunks are materialised on first write; unwritten regions read as zeroes,
 * exactly like a thin-provisioned device. Formatting a 16 GiB tobyfs touches
 * the superblock, 32 inode-bitmap blocks, 128 data-bitmap blocks, one inode
 * table block and the journal -- about 4 MiB of real memory for a volume
 * that reports 16 GiB. */
#define FSM_SPARSE_CHUNK_BLOCKS 256u                      /* 1 MiB per chunk */
#define FSM_SPARSE_CHUNK_BYTES  (FSM_SPARSE_CHUNK_BLOCKS * 4096u)

struct fsm_sparse {
    uint8_t **chunk;
    uint64_t  nchunks;
    uint64_t  bytes;
};
static struct fsm_sparse g_fsm_sp;
static struct blk_dev    g_fsm_sp_dev;

static int fsm_sp_rw(struct fsm_sparse *sp, uint64_t off, uint64_t len,
                     void *buf, bool write) {
    uint8_t *p = (uint8_t *)buf;
    if (off + len > sp->bytes) return -1;
    while (len) {
        uint64_t ci  = off / FSM_SPARSE_CHUNK_BYTES;
        uint64_t co  = off % FSM_SPARSE_CHUNK_BYTES;
        uint64_t run = FSM_SPARSE_CHUNK_BYTES - co;
        if (run > len) run = len;
        if (ci >= sp->nchunks) return -1;
        if (!sp->chunk[ci]) {
            if (!write) { memset(p, 0, (size_t)run); goto next; }
            sp->chunk[ci] = (uint8_t *)kmalloc(FSM_SPARSE_CHUNK_BYTES);
            if (!sp->chunk[ci]) return -1;
            memset(sp->chunk[ci], 0, FSM_SPARSE_CHUNK_BYTES);
        }
        if (write) memcpy(sp->chunk[ci] + co, p, (size_t)run);
        else       memcpy(p, sp->chunk[ci] + co, (size_t)run);
    next:
        p += run; off += run; len -= run;
    }
    return 0;
}

static int fsm_sp_read(struct blk_dev *d, uint64_t lba, uint32_t n, void *buf) {
    return fsm_sp_rw((struct fsm_sparse *)d->priv, lba * BLK_SECTOR_SIZE,
                     (uint64_t)n * BLK_SECTOR_SIZE, buf, false);
}
static int fsm_sp_write(struct blk_dev *d, uint64_t lba, uint32_t n,
                        const void *buf) {
    return fsm_sp_rw((struct fsm_sparse *)d->priv, lba * BLK_SECTOR_SIZE,
                     (uint64_t)n * BLK_SECTOR_SIZE, (void *)buf, true);
}
static const struct blk_ops g_fsm_sp_ops = { .read = fsm_sp_read,
                                             .write = fsm_sp_write };

static struct blk_dev *fsm_make_sparse(const char *name, uint64_t bytes) {
    g_fsm_sp.bytes   = bytes;
    g_fsm_sp.nchunks = (bytes + FSM_SPARSE_CHUNK_BYTES - 1) / FSM_SPARSE_CHUNK_BYTES;
    g_fsm_sp.chunk   = (uint8_t **)kmalloc((size_t)g_fsm_sp.nchunks * sizeof(uint8_t *));
    if (!g_fsm_sp.chunk) return 0;
    memset(g_fsm_sp.chunk, 0, (size_t)g_fsm_sp.nchunks * sizeof(uint8_t *));
    memset(&g_fsm_sp_dev, 0, sizeof g_fsm_sp_dev);
    g_fsm_sp_dev.name         = name;
    g_fsm_sp_dev.ops          = &g_fsm_sp_ops;
    g_fsm_sp_dev.sector_count = bytes / BLK_SECTOR_SIZE;
    g_fsm_sp_dev.priv         = &g_fsm_sp;
    g_fsm_sp_dev.class        = BLK_CLASS_DISK;
    return &g_fsm_sp_dev;
}

static uint64_t fsm_sparse_resident(void) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < g_fsm_sp.nchunks; i++) if (g_fsm_sp.chunk[i]) n++;
    return n * FSM_SPARSE_CHUNK_BYTES;
}

static void fsm_free_sparse(void) {
    if (!g_fsm_sp.chunk) return;
    for (uint64_t i = 0; i < g_fsm_sp.nchunks; i++)
        if (g_fsm_sp.chunk[i]) kfree(g_fsm_sp.chunk[i]);
    kfree(g_fsm_sp.chunk);
    g_fsm_sp.chunk = 0; g_fsm_sp.nchunks = 0; g_fsm_sp.bytes = 0;
}

/* ---- the matrix ------------------------------------------------------- */

static long fsm_size(const char *path) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != VFS_OK) return -1;
    return (long)st.size;
}

/* `mp` is a mount point that already holds a live filesystem. */
static void fsm_run(const char *fs, const char *mp, bool expect_links,
                    bool has_perms) {
    char f[128], f2[128], sub[128], ln[128], msg[192];
    ksnprintf(f,   sizeof f,   "%s/crud.txt",  mp);
    ksnprintf(f2,  sizeof f2,  "%s/crud2.txt", mp);
    ksnprintf(sub, sizeof sub, "%s/crudddir",  mp);
    ksnprintf(ln,  sizeof ln,  "%s/crudlink",  mp);

    /* C -- create + write */
    int rc = vfs_write_all(f, "hello\n", 6);
    ksnprintf(msg, sizeof msg, "rc=%d", rc);
    fsm_chk(rc == VFS_OK, fs, "create+write", msg);
    if (rc != VFS_OK) return;               /* the rest is meaningless */

    /* R -- read the exact bytes back */
    {
        void *rb = 0; size_t rn = 0;
        int r = vfs_read_all(f, &rb, &rn);
        int good = (r == VFS_OK && rn == 6 && rb &&
                    memcmp(rb, "hello\n", 6) == 0);
        ksnprintf(msg, sizeof msg, "rc=%d n=%lu", r, (unsigned long)rn);
        fsm_chk(good, fs, "read-back", msg);
        if (rb) kfree(rb);
    }

    /* U -- truncate by PATH, and the size must really change */
    {
        int t = vfs_truncate(f, 3);
        long sz = fsm_size(f);
        ksnprintf(msg, sizeof msg, "rc=%d size=%ld want 3", t, sz);
        fsm_chk(t == VFS_OK && sz == 3, fs, "truncate(path) shrinks", msg);
    }
    /* ...and GROWING must zero-fill, not expose old bytes. */
    {
        int t = vfs_truncate(f, 10);
        long sz = fsm_size(f);
        void *rb = 0; size_t rn = 0;
        int zeros = 0;
        if (vfs_read_all(f, &rb, &rn) == VFS_OK && rb && rn >= 10) {
            const uint8_t *p = (const uint8_t *)rb;
            zeros = (p[3] == 0 && p[4] == 0 && p[9] == 0);
        }
        if (rb) kfree(rb);
        ksnprintf(msg, sizeof msg, "rc=%d size=%ld want 10, hole-zeroed=%d",
                  t, sz, zeros);
        fsm_chk(t == VFS_OK && sz == 10 && zeros, fs,
                "truncate(path) grows w/ zeros", msg);
    }

    /* U -- ftruncate on an OPEN handle */
    {
        struct vfs_file h;
        int t = -1;
        if (vfs_open(f, &h) == VFS_OK) {
            t = vfs_file_truncate(&h, 2);
            vfs_close(&h);
        }
        long sz = fsm_size(f);
        ksnprintf(msg, sizeof msg, "rc=%d size=%ld want 2", t, sz);
        fsm_chk(t == VFS_OK && sz == 2, fs, "ftruncate(fd)", msg);
    }

    /* U -- chmod, read back through stat. FAT has no permission bits at
     * all, so there the right answer is a clean refusal, not a stored
     * mode -- asserting the exact code keeps "unsupported" from drifting
     * into "silently ignored". */
    if (has_perms) {
        int c = vfs_chmod(f, 00640u);
        struct vfs_stat st;
        int s = vfs_stat(f, &st);
        ksnprintf(msg, sizeof msg, "rc=%d mode=%04o want 0640",
                  c, (unsigned)(st.mode & 07777));
        fsm_chk(c == VFS_OK && s == VFS_OK && (st.mode & 07777) == 00640u,
                fs, "chmod", msg);

        int c2 = vfs_chown(f, 1234, 5678);
        struct vfs_stat st2;
        int s2 = vfs_stat(f, &st2);
        ksnprintf(msg, sizeof msg, "rc=%d uid=%u gid=%u want 1234/5678",
                  c2, (unsigned)st2.uid, (unsigned)st2.gid);
        fsm_chk(c2 == VFS_OK && s2 == VFS_OK && st2.uid == 1234 &&
                st2.gid == 5678, fs, "chown", msg);
    } else {
        int c  = vfs_chmod(f, 00640u);
        int c2 = vfs_chown(f, 1234, 5678);
        ksnprintf(msg, sizeof msg, "chmod=%d chown=%d both want %d "
                  "(format has no perm bits)", c, c2, VFS_ERR_ROFS);
        fsm_chk(c == VFS_ERR_ROFS && c2 == VFS_ERR_ROFS, fs,
                "no perms, reported cleanly", msg);
    }

    /* U -- utimes. THE ONE `make` AND `cp -p` CARE ABOUT: a filesystem
     * that accepts a timestamp and drops it makes every dependency check
     * wrong, silently. */
    {
        int u = vfs_utimes(f, 1700000000ull, 1700000001ull);
        struct vfs_stat st;
        int s = vfs_stat(f, &st);
        ksnprintf(msg, sizeof msg, "rc=%d mtime=%lu want 1700000000",
                  u, (unsigned long)st.mtime);
        fsm_chk(u == VFS_OK && s == VFS_OK && st.mtime == 1700000000ull,
                fs, "utimes stores mtime", msg);
    }

    /* A write must MOVE the modification time. `make` compares mtimes and
     * nothing else; a filesystem that keeps returning the same one makes
     * every dependency check wrong in the quiet direction. */
    {
        struct vfs_stat before, after;
        int b = vfs_stat(f, &before);
        /* Park it at a time every filesystem here can actually store.
         * Parking at 100 looked stricter and was weaker: FAT cannot
         * represent anything before 1980, so it clamps, and the check
         * would have passed without the write doing anything. */
        (void)vfs_utimes(f, 1700000000ull, 1700000000ull);
        int w = vfs_write_all(f, "zz", 2);
        int a = vfs_stat(f, &after);
        ksnprintf(msg, sizeof msg, "wrote rc=%d mtime %lu -> %lu (parked at "
                  "1700000000)", w, (unsigned long)before.mtime,
                  (unsigned long)after.mtime);
        fsm_chk(b == VFS_OK && a == VFS_OK && w == VFS_OK &&
                after.mtime > 1700000000ull, fs, "write moves mtime", msg);
    }

    /* U -- rename: old gone, new present */
    {
        int r = vfs_rename(f, f2);
        long o = fsm_size(f), n = fsm_size(f2);
        ksnprintf(msg, sizeof msg, "rc=%d old=%ld want -1 new=%ld want 2",
                  r, o, n);
        fsm_chk(r == VFS_OK && o < 0 && n == 2, fs, "rename", msg);
    }

    /* Hard links -- a second NAME for one set of bytes. The interesting
     * assertion is not that link() succeeds but that removing the FIRST
     * name leaves the bytes reachable through the second: that is the
     * whole difference between a link and a copy. */
    if (expect_links) {
        (void)vfs_unlink(ln);
        int l = vfs_link(f2, ln);
        struct vfs_stat st;
        int s1 = vfs_stat(f2, &st);
        uint32_t nl = (s1 == VFS_OK) ? st.nlink : 0;
        long lsz = fsm_size(ln);
        ksnprintf(msg, sizeof msg, "rc=%d nlink=%u want 2 linksize=%ld want 2",
                  l, (unsigned)nl, lsz);
        fsm_chk(l == VFS_OK && nl == 2 && lsz == 2, fs, "link makes a 2nd name",
                msg);

        int u = vfs_unlink(f2);
        long gone = fsm_size(f2), still = fsm_size(ln);
        ksnprintf(msg, sizeof msg,
                  "rc=%d first-name=%ld want -1 bytes-via-2nd=%ld want 2",
                  u, gone, still);
        fsm_chk(u == VFS_OK && gone < 0 && still == 2, fs,
                "bytes outlive 1st name", msg);

        /* Put the surviving name back so the delete test below still has
         * something to delete. */
        (void)vfs_rename(ln, f2);
    } else {
        /* A driver with no .link must say so cleanly. Asserting the exact
         * code keeps this from passing for the wrong reason later. */
        int l = vfs_link(f2, ln);
        ksnprintf(msg, sizeof msg, "rc=%d want %d (no .link op)",
                  l, VFS_ERR_ROFS);
        fsm_chk(l == VFS_ERR_ROFS, fs, "link absent, reported cleanly", msg);
    }

    /* directories */
    {
        int m = vfs_mkdir(sub);
        struct vfs_stat st;
        int s = vfs_stat(sub, &st);
        ksnprintf(msg, sizeof msg, "rc=%d isdir=%d", m,
                  (s == VFS_OK && st.type == VFS_TYPE_DIR) ? 1 : 0);
        fsm_chk(m == VFS_OK && s == VFS_OK && st.type == VFS_TYPE_DIR,
                fs, "mkdir", msg);
    }

    /* D -- delete */
    {
        int u = vfs_unlink(f2);
        long sz = fsm_size(f2);
        ksnprintf(msg, sizeof msg, "rc=%d size-after=%ld want -1", u, sz);
        fsm_chk(u == VFS_OK && sz < 0, fs, "unlink", msg);
    }
    (void)vfs_unlink(sub);

    /* statfs must report the volume, not a fabricated constant. */
    {
        struct vfs_statfs sf;
        memset(&sf, 0, sizeof sf);
        int s = vfs_statfs(mp, &sf);
        ksnprintf(msg, sizeof msg, "rc=%d bsize=%lu blocks=%lu", s,
                  (unsigned long)sf.bsize, (unsigned long)sf.blocks);
        fsm_chk(s == VFS_OK && sf.bsize > 0 && sf.blocks > 0,
                fs, "statfs reports the volume", msg);
    }
}



/* ---- one file, two handles ------------------------------------------
 *
 * ext4, ext2 and fat32 each cache the file's metadata INSIDE the open
 * handle (struct ext4_filepriv holds a whole struct ext4_inode; fat32
 * holds the first cluster). Two handles on one file therefore hold two
 * copies of the truth, and whichever writes last wins -- silently
 * discarding the other's growth and leaking the blocks it allocated.
 *
 * tmpfs and ramfs point their handles straight at the shared node, so
 * they are coherent by construction. They run these same checks as the
 * control: if the assertions were wrong, they would fail there too.
 */
static void fsm_coherence(const char *fs, const char *mp) {
    char f[128], msg[192];
    ksnprintf(f, sizeof f, "%s/coher.txt", mp);

    /* --- a second handle must not lose the first handle's growth --- */
    {
        (void)vfs_unlink(f);
        int cr = vfs_create(f);
        uint8_t *big = kmalloc(8192);
        if (!big || cr != VFS_OK) {
            if (big) kfree(big);
            fsm_bad(fs, "2nd handle keeps 1st's growth", "fixture failed");
        } else {
            memset(big, 'A', 8192);
            struct vfs_file a, b;
            int oa = vfs_open(f, &a);
            int ob = vfs_open(f, &b);
            long wa = -1, wb = -1;
            if (oa == VFS_OK) wa = vfs_write(&a, big, 8192);
            /* B was opened when the file was empty. One byte at offset 0
             * must not roll the file back to one byte long. */
            if (ob == VFS_OK) wb = vfs_write(&b, "B", 1);
            if (oa == VFS_OK) vfs_close(&a);
            if (ob == VFS_OK) vfs_close(&b);
            long sz = fsm_size(f);
            kfree(big);
            ksnprintf(msg, sizeof msg,
                      "wrote %ld then %ld, size=%ld want 8192", wa, wb, sz);
            fsm_chk(oa == VFS_OK && ob == VFS_OK && wa == 8192 && wb == 1 &&
                    sz == 8192, fs, "2nd handle keeps 1st's growth", msg);
        }
    }

    /* --- a truncate by PATH must be visible to an already-open handle ---
     * This is the one `> file` used to hit: O_TRUNC truncates by path,
     * and a handle opened first would write its stale size straight back
     * over the result. */
    {
        (void)vfs_unlink(f);
        int cr = vfs_write_all(f, "hello\n", 6);
        struct vfs_file h;
        int oh = vfs_open(f, &h);
        int tr = vfs_truncate(f, 0);
        long mid = fsm_size(f);
        long w = -1;
        if (oh == VFS_OK) {
            w = vfs_write(&h, "Z", 1);
            vfs_close(&h);
        }
        long sz = fsm_size(f);
        ksnprintf(msg, sizeof msg,
                  "trunc rc=%d size-after-trunc=%ld want 0, then wrote %ld -> "
                  "size=%ld want 1", tr, mid, w, sz);
        fsm_chk(cr == VFS_OK && oh == VFS_OK && tr == VFS_OK && mid == 0 &&
                w == 1 && sz == 1, fs, "open handle sees a path truncate", msg);
    }
    (void)vfs_unlink(f);
}


static void fsm_note(const char *fs, const char *what, const char *detail) {
    kprintf("[FSMATRIX]  note  %-7s %-28s %s\n", fs, what, detail ? detail : "");
}

/* ---- unlink removes the NAME, not the file --------------------------
 *
 * POSIX: the bytes stay readable through every handle already open, and
 * the space comes back at the LAST close. This is not a corner case --
 * it is how mkstemp() and tmpfile() make a private temp file, and how
 * every bash here-document works: open, unlink immediately, keep using
 * the fd.
 *
 * The first version of this check PASSED on fat32 while fat32 was still
 * freeing the chain inside unlink(): the clusters had been returned to
 * the allocator but nothing had overwritten them yet, so the old bytes
 * were still lying there to be read. So the check now allocates a second
 * file of the same size in between -- if the space really was released,
 * that file takes it and the handle starts reading somebody else's
 * bytes. Reading 'U' back afterwards is then a result rather than luck.
 */
static void fsm_unlink_open(const char *fs, const char *mp) {
    char f[128], g[128], msg[192];
    ksnprintf(f, sizeof f, "%s/unlopen.dat", mp);
    ksnprintf(g, sizeof g, "%s/unlgrab.dat", mp);
    (void)vfs_unlink(f);
    (void)vfs_unlink(g);

    const size_t BIG = 64u * 1024u;
    uint8_t *buf = kmalloc(BIG);
    if (!buf) { fsm_bad(fs, "unlink-while-open", "out of memory"); return; }

    struct vfs_statfs before, after;
    memset(&before, 0, sizeof before);
    (void)vfs_statfs(mp, &before);

    memset(buf, 'U', BIG);
    int cr = vfs_write_all(f, buf, BIG);

    struct vfs_file h;
    int oh = vfs_open(f, &h);
    int un = vfs_unlink(f);
    long named = fsm_size(f);            /* the NAME must be gone */

    /* Take the space, if it was in fact released. */
    memset(buf, 'X', BIG);
    int gr = vfs_write_all(g, buf, BIG);

    long got = -1;
    int  intact = 0;
    if (oh == VFS_OK) {
        uint8_t probe[64];
        got = vfs_read(&h, probe, sizeof probe);
        intact = (got == (long)sizeof probe);
        for (size_t i = 0; intact && i < sizeof probe; i++)
            if (probe[i] != 'U') intact = 0;      /* 'X' here = freed early */
    }
    if (oh == VFS_OK) vfs_close(&h);
    (void)vfs_unlink(g);

    memset(&after, 0, sizeof after);
    (void)vfs_statfs(mp, &after);
    kfree(buf);

    ksnprintf(msg, sizeof msg,
              "unlink rc=%d name=%ld want -1, reused rc=%d, read %ld bytes "
              "still-ours=%d", un, named, gr, got, intact);
    fsm_chk(cr == VFS_OK && oh == VFS_OK && un == VFS_OK && named < 0 &&
            gr == VFS_OK && intact, fs,
            "bytes survive unlink-while-open", msg);

    /* ...and the space must come back at the last close, or the deferred
     * release is just a leak wearing a POSIX costume. */
    if (before.bfree == 0 && after.bfree == 0) {
        /* ramfs reports bfree = 0 by design (the tar never grows), so
         * this would be a comparison of 0 with 0 -- which passes without
         * meaning anything. Said out loud instead of counted. */
        fsm_note(fs, "leak check N/A", "driver does not report free space");
    } else {
        ksnprintf(msg, sizeof msg, "bfree before=%lu after=%lu",
                  (unsigned long)before.bfree, (unsigned long)after.bfree);
        fsm_chk(after.bfree == before.bfree, fs,
                "space fully returned, no leak", msg);
    }
    (void)vfs_unlink(f);
    (void)vfs_unlink(g);
}


/* ---- fsync must put the bytes on the DEVICE -------------------------
 *
 * Writes a payload nothing else would produce, then scans the RAM disk's
 * backing buffer for it -- that buffer IS the "platter", so finding the
 * bytes there means they really reached the device.
 *
 * READ THE before-fsync COLUMN. It is 1 on both drivers, and that is a
 * finding rather than a formality: ext4, ext2 and fat32 all write
 * straight through blk_write(), and tobyfs is the only driver that
 * defers into the write-back cache (and it flushes on every journal
 * commit). So no data was ever at risk here, and this check does NOT
 * currently discriminate -- it would pass with fsync stubbed out.
 *
 * It is kept because it stops being vacuous the moment any driver starts
 * deferring, which is exactly when nobody would think to add it. Printing
 * both samples is what keeps a green line from claiming more than it
 * proves.
 */
static bool fsm_raw_contains(const uint8_t *needle, size_t n) {
    if (!g_fsm_ram.buf || g_fsm_ram.bytes < n) return false;
    for (uint64_t i = 0; i + n <= g_fsm_ram.bytes; i++)
        if (g_fsm_ram.buf[i] == needle[0] &&
            memcmp(g_fsm_ram.buf + i, needle, n) == 0) return true;
    return false;
}

static void fsm_fsync_check(const char *fs, const char *mp) {
    char f[128], msg[192];
    ksnprintf(f, sizeof f, "%s/fsyncme.txt", mp);
    (void)vfs_unlink(f);

    static const char magic[] = "FSYNC-MAGIC-8A3C1F09-ON-THE-PLATTER";
    const size_t mlen = sizeof magic - 1;

    int cr = vfs_create(f);
    struct vfs_file h;
    int oh = (cr == VFS_OK) ? vfs_open(f, &h) : -1;
    long w  = (oh == VFS_OK) ? vfs_write(&h, magic, mlen) : -1;

    bool before = fsm_raw_contains((const uint8_t *)magic, mlen);
    int  sy     = (oh == VFS_OK) ? vfs_file_sync(&h) : -1;
    bool after  = fsm_raw_contains((const uint8_t *)magic, mlen);

    if (oh == VFS_OK) vfs_close(&h);
    (void)vfs_unlink(f);

    ksnprintf(msg, sizeof msg,
              "wrote %ld rc=%d, on-device before-fsync=%d after-fsync=%d",
              w, sy, before ? 1 : 0, after ? 1 : 0);
    fsm_chk(cr == VFS_OK && oh == VFS_OK && w == (long)mlen &&
            sy == VFS_OK && after, fs, "fsync puts bytes on the device", msg);
}

/* ---- formatting a REAL device, for host-side verification ------------
 *
 * fsm_run above proves the driver can read what the formatter wrote --
 * which is exactly as circular as it sounds. The claim that actually
 * matters for a USB stick is that OTHER systems can read it, and no
 * amount of self-consistency tests that.
 *
 * So: format an attached scratch disk and leave real content on it, so
 * the image file can be handed to an independent FAT implementation on
 * the host. Guarded twice over -- opt-in at compile time, and it refuses
 * any device whose first sectors are not already blank, because "format
 * the second disk" is one typo away from formatting somebody's data.
 */
static bool fsm_dev_is_blank(struct blk_dev *d) {
    uint8_t buf[BLK_SECTOR_SIZE];
    /* The boot sector, the GPT header and a data sector. A disk with any
     * of the three non-zero is not a scratch disk and we do not touch it. */
    const uint64_t probe[] = { 0, 1, 64 };
    for (size_t i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        if (probe[i] >= d->sector_count) continue;
        if (blk_read(d, probe[i], 1, buf) != 0) return false;
        for (size_t j = 0; j < sizeof buf; j++)
            if (buf[j] != 0) return false;
    }
    return true;
}

void fsmatrix_format_scratch(void) {
    kprintf("[FATDISK] ==== format a real device for host verification ====\n");

    struct blk_dev *target = 0;
    size_t it = 0;
    struct blk_dev *d;
    while ((d = blk_iter_next(&it, BLK_CLASS_DISK)) != NULL) {
        bool blank = fsm_dev_is_blank(d);
        kprintf("[FATDISK] candidate %s sectors=%lu blank=%d\n",
                d->name ? d->name : "?", (unsigned long)d->sector_count,
                blank ? 1 : 0);
        /* First blank disk big enough to hold a legal FAT32 volume. */
        if (blank && d->sector_count >= 70000 && !target) target = d;
    }
    if (!target) {
        kprintf("[FATDISK] no blank scratch disk attached -- nothing done\n");
        return;
    }

    kprintf("[FATDISK] formatting %s (%lu sectors)\n",
            target->name ? target->name : "?",
            (unsigned long)target->sector_count);
    int rc = fat32_format(target);
    if (rc != VFS_OK) { kprintf("[FATDISK] format FAILED rc=%d\n", rc); return; }
    bcache_invalidate(target);

    rc = fat32_mount("/fatscratch", target);
    if (rc != VFS_OK) { kprintf("[FATDISK] mount FAILED rc=%d\n", rc); return; }

    /* Content an independent reader can check byte for byte. */
    static const char payload[] =
        "tobyOS wrote this to a FAT32 volume it formatted itself.\n";
    int w  = vfs_write_all("/fatscratch/HELLO.TXT", payload,
                           sizeof payload - 1);
    int md = vfs_mkdir("/fatscratch/SUBDIR");
    int w2 = vfs_write_all("/fatscratch/SUBDIR/NESTED.TXT", "nested\n", 7);
    kprintf("[FATDISK] wrote HELLO.TXT rc=%d (%lu bytes), mkdir rc=%d, "
            "nested rc=%d\n", w, (unsigned long)(sizeof payload - 1), md, w2);

    (void)vfs_unmount("/fatscratch");
    blk_flush(target);
    kprintf("[FATDISK] VERDICT: %s -- image is on the host disk now\n",
            (w == VFS_OK && md == VFS_OK && w2 == VFS_OK) ? "WROTE" : "INCOMPLETE");
}

/* ---- entry point ------------------------------------------------------ */

/* tobyfs on a RAM volume, run twice: once over a ZEROED device and once
 * over a 0xFF one.
 *
 * The pair is the point. tobyfs_format writes the superblock, both bitmaps,
 * the FIRST inode-table block (the root) and the journal -- and deliberately
 * leaves the remaining inode-table blocks holding whatever the device held
 * before, because zeroing them made format O(volume): a 30 GB stick meant
 * ~59000 single-block writes. Format has never zeroed DATA blocks either.
 *
 * Safety therefore rests on the inode BITMAP being the sole authority on
 * which slots exist, and on every allocator zeroing what it hands out. An
 * all-zero fixture cannot test either claim -- stale bytes and correct bytes
 * are the same bytes. 0xFF is the hostile case: untouched inode slots read as
 * type=0xFFFFFFFF, nlink=0xFFFF, size=4 GiB, direct[]=OUT OF RANGE, and an
 * unzeroed data block reads as 0xFF instead of NUL.
 *
 * Running BOTH means the log itself separates the two diagnoses: a check that
 * fails only under tobyfsX is a stale-data bug, one that fails under both is
 * a plain tobyfs bug that simply had no fixture before. */
static void fsm_tobyfs_case(const char *label, uint8_t fill) {
    struct blk_dev *d = fsm_make_disk_filled("fsmatrixtfs", 65536, fill);
    if (!d) {
        fsm_bad(label, "fixture", "out of memory for the RAM volume");
        return;
    }
    char msg[192];
    int fr = tobyfs_format(d);
    ksnprintf(msg, sizeof msg, "tobyfs_format rc=%d over 0x%02X", fr, fill);
    fsm_chk(fr == VFS_OK, label, "format over the device", msg);
    if (fr != VFS_OK) return;
    bcache_invalidate(d);

    /* 1. Structural check WITHOUT mounting. If anything consulted the stale
     *    inode table this reports FATAL, naming the offending inode. */
    struct tobyfs_check chk;
    int cr = tobyfs_check_dev(d, &chk);
    ksnprintf(msg, sizeof msg, "rc=%d sev=%d errs=%u: %s", cr, chk.severity,
              chk.errors, chk.detail[0] ? chk.detail : "(none)");
    fsm_chk(cr == 0 && chk.severity == TFS_CHECK_OK, label,
            "fscheck clean after format", msg);

    /* 2. Exactly one inode allocated -- the root, and nothing the previous
     *    contents left behind. */
    ksnprintf(msg, sizeof msg, "inodes_used=%u (want 1) of %u",
              chk.inodes_used, chk.inodes_total);
    fsm_chk(chk.inodes_used == 1, label, "only the root inode is allocated",
            msg);

    int m = tobyfs_mount("/fsmtfs", d);
    ksnprintf(msg, sizeof msg, "tobyfs_mount rc=%d", m);
    fsm_chk(m == VFS_OK, label, "format then mount", msg);
    if (m != VFS_OK) return;

    /* 3. A freshly allocated inode carries the values its creator wrote, not
     *    the slot's previous bytes. A stale 0xFF slot shows up as size
     *    4294967295 rather than 0. */
    const char *f = "/fsmtfs/stale.txt";
    (void)vfs_unlink(f);
    int c = vfs_create(f);
    long sz = fsm_size(f);
    ksnprintf(msg, sizeof msg, "create rc=%d size=%ld (want 0)", c, sz);
    fsm_chk(c == VFS_OK && sz == 0, label,
            "new inode is clean, not the slot's old bytes", msg);
    (void)vfs_unlink(f);

    /* 3b. Reach PAST the first inode-table block.
     *
     * This is the assertion the rest of the case cannot make. Inodes are 32
     * to a 4 KiB block, and format writes table block 0 in full (it carries
     * the root), so every check above lands on inodes 1..~10 -- bytes that
     * format zeroed either way. The blocks lazy_itable_init actually leaves
     * alone are 1..63, and nothing had yet read one.
     *
     * 40 files pushes allocation to inode ~41, which is in table block 1 --
     * still 0xFF on the hostile fixture. Each file gets a distinct byte and
     * is read back, so a slot that kept any of its previous contents shows
     * up as a wrong size or a wrong byte rather than as nothing at all. */
    {
        enum { NFILE = 40 };
        char path[64];
        int made = 0, verified = 0;
        for (int i = 0; i < NFILE; i++) {
            ksnprintf(path, sizeof path, "/fsmtfs/i%d.bin", i);
            if (vfs_create(path) != VFS_OK) continue;
            made++;
            /* size must be 0 the instant the inode exists -- a stale 0xFF
             * slot reads back as 4294967295 here. */
            if (fsm_size(path) != 0) continue;
            uint8_t b = (uint8_t)('a' + (i % 26));
            if (vfs_write_all(path, &b, 1) != VFS_OK) continue;
            void *rb = 0; size_t rn = 0;
            if (vfs_read_all(path, &rb, &rn) == VFS_OK && rb && rn == 1 &&
                ((const uint8_t *)rb)[0] == b) verified++;
            if (rb) kfree(rb);
        }
        for (int i = 0; i < NFILE; i++) {
            ksnprintf(path, sizeof path, "/fsmtfs/i%d.bin", i);
            (void)vfs_unlink(path);
        }
        ksnprintf(msg, sizeof msg,
                  "created %d/%d, byte-exact %d/%d (spans inode-table block 1)",
                  made, (int)NFILE, verified, (int)NFILE);
        fsm_chk(made == NFILE && verified == NFILE, label,
                "inodes past table block 0 are clean", msg);
    }

    /* 4. And the whole CRUD contract, same as every other driver. */
    fsm_run(label, "/fsmtfs", true, true);
    fsm_coherence(label, "/fsmtfs");
    fsm_unlink_open(label, "/fsmtfs");
    fsm_fsync_check(label, "/fsmtfs");
    (void)vfs_unmount("/fsmtfs");
}

void fsmatrix_selftest(void) {
    g_fsm_pass = g_fsm_fail = 0;
    kprintf("[FSMATRIX] ==== the CRUD contract, per driver ====\n");

    /* tmpfs and the initrd root are already mounted and need no fixture. */
    fsm_run("tmpfs", "/tmp", true, true);
    fsm_coherence("tmpfs", "/tmp");
    fsm_unlink_open("tmpfs", "/tmp");

    /* ramfs IS the root every session starts in, and nothing had ever run
     * the matrix against it. Work inside a subdirectory so a failed run
     * leaves its litter in one place. */
    {
        const char *dir = "/fsmramfs";
        (void)vfs_unlink(dir);
        int mk = vfs_mkdir(dir);
        if (mk != VFS_OK) {
            char msg[96];
            ksnprintf(msg, sizeof msg, "mkdir %s rc=%d", dir, mk);
            fsm_bad("ramfs", "fixture", msg);
        } else {
            fsm_run("ramfs", dir, true, true);
            fsm_coherence("ramfs", dir);
            fsm_unlink_open("ramfs", dir);
            (void)vfs_unlink(dir);
        }
    }

    /* A 16 GiB volume on the sparse device -- the paged data bitmap.
     *
     * Size chosen for one specific property: at 16 GiB the inode table alone
     * is 32768 blocks, so data_blk_start lands at 32929, PAST the 32768 bits
     * that data-bitmap block 0 covers. Block 0 is therefore entirely
     * metadata and permanently full, which means every single allocation on
     * this volume has to consult the free-count summary, skip block 0
     * without reading it, and find space in a later one. On the 32 MiB
     * fixtures above there is exactly one bitmap block and none of that code
     * ever runs.
     *
     * The old whole-bitmap cache would have wanted 512 KiB here and 512 MiB
     * at the format's ceiling; the window plus summary want 4 KiB + 256 B. */
    {
        struct blk_dev *d = fsm_make_sparse("fsmbig", 16ULL * 1024 * 1024 * 1024);
        char msg[192];
        if (!d) {
            fsm_bad("tobyfsB", "fixture", "out of memory for the sparse volume");
        } else {
            int fr = tobyfs_format(d);
            ksnprintf(msg, sizeof msg, "tobyfs_format rc=%d on 16 GiB", fr);
            fsm_chk(fr == VFS_OK, "tobyfsB", "format a 16 GiB volume", msg);
            if (fr == VFS_OK) {
                bcache_invalidate(d);

                struct tobyfs_check chk;
                int cr = tobyfs_check_dev(d, &chk);
                ksnprintf(msg, sizeof msg, "rc=%d sev=%d errs=%u: %s", cr,
                          chk.severity, chk.errors,
                          chk.detail[0] ? chk.detail : "(none)");
                fsm_chk(cr == 0 && chk.severity == TFS_CHECK_OK, "tobyfsB",
                        "fscheck clean at 16 GiB", msg);

                int m = tobyfs_mount("/fsmbig", d);
                ksnprintf(msg, sizeof msg, "tobyfs_mount rc=%d", m);
                fsm_chk(m == VFS_OK, "tobyfsB", "mount a 16 GiB volume", msg);
                if (m == VFS_OK) {
                    /* Prove the shape this case exists to create: more than
                     * one bitmap block, and a first one the allocator can
                     * never use. If a future geometry change quietly undoes
                     * that, the rest of this case stops testing paging and
                     * would still pass -- so assert it out loud. */
                    struct vfs_statfs st;
                    int sr = vfs_statfs("/fsmbig", &st);
                    ksnprintf(msg, sizeof msg,
                              "statfs rc=%d blocks=%lu bfree=%lu (>32768 bits/bitmap block)",
                              sr, (unsigned long)st.blocks,
                              (unsigned long)st.bfree);
                    fsm_chk(sr == VFS_OK && st.blocks > 32768 &&
                            st.bfree > 32768 && st.bfree <= st.blocks,
                            "tobyfsB", "statfs spans many bitmap blocks", msg);

                    /* Every allocation below therefore starts by skipping a
                     * full bitmap block via the summary. */
                    fsm_run("tobyfsB", "/fsmbig", true, true);
                    fsm_coherence("tobyfsB", "/fsmbig");
                    fsm_unlink_open("tobyfsB", "/fsmbig");

                    /* And the free count must survive all of that.
                     *
                     * WARM UP FIRST. The obvious version of this check --
                     * compare bfree from just after mount against bfree at
                     * the end -- fails by exactly one block, and the block
                     * is real: the first create in an empty directory
                     * allocates the root's first dirent block, which is a
                     * one-time cost that no later delete gives back. That is
                     * directory growth, not drift, and a check that cannot
                     * tell them apart is measuring the wrong thing.
                     *
                     * So grow the directory first, then cycle. statfs is now
                     * derived from the paged summary rather than counted bit
                     * by bit, so a summary that lost or double-counted a bit
                     * would surface here and almost nowhere else. */
                    {
                        const char *cf = "/fsmbig/cycle.bin";
                        uint8_t *big = kmalloc(8192);
                        (void)vfs_unlink(cf);
                        (void)vfs_create(cf);           /* warm-up: grows root */
                        (void)vfs_unlink(cf);

                        struct vfs_statfs base, now;
                        int br = vfs_statfs("/fsmbig", &base);
                        int cycles = 0;
                        if (big) {
                            memset(big, 'C', 8192);
                            for (int c = 0; c < 8; c++) {
                                if (vfs_create(cf) != VFS_OK) break;
                                if (vfs_write_all(cf, big, 8192) != VFS_OK) break;
                                if (vfs_unlink(cf) != VFS_OK) break;
                                cycles++;
                            }
                            kfree(big);
                        }
                        int nr = vfs_statfs("/fsmbig", &now);
                        ksnprintf(msg, sizeof msg,
                                  "%d/8 cycles of 8 KiB, bfree %lu -> %lu (want equal)",
                                  cycles, (unsigned long)base.bfree,
                                  (unsigned long)now.bfree);
                        fsm_chk(br == VFS_OK && nr == VFS_OK && cycles == 8 &&
                                now.bfree == base.bfree, "tobyfsB",
                                "summary free-count does not drift", msg);
                        (void)vfs_unlink(cf);
                    }
                    (void)vfs_unmount("/fsmbig");
                }
                ksnprintf(msg, sizeof msg,
                          "%lu MiB resident for a 16384 MiB volume",
                          (unsigned long)(fsm_sparse_resident() >> 20));
                fsm_note("tobyfsB", "sparse fixture cost", msg);
            }
            fsm_free_sparse();
        }
    }

    /* The control first, then the hostile one. Same code, same volume size,
     * only the pre-existing bytes differ -- so any check that passes as
     * `tobyfs` and fails as `tobyfsX` is naming a stale-data leak, and one
     * that fails as both is naming a bug the gate simply never had a tobyfs
     * fixture to find. */
    fsm_tobyfs_case("tobyfs", 0x00);
    fsm_tobyfs_case("tobyfsX", 0xFF);
    fsm_note("tobyfsX", "what the X means",
             "same volume, but the raw device was 0xFF before format");

    /* ext4 on a freshly formatted RAM volume. 32 MiB: comfortably above
     * the formatter's minimum and small enough to allocate. */
    {
        struct blk_dev *d = fsm_make_disk("fsmatrix0", 65536);   /* 32 MiB */
        if (!d) {
            fsm_bad("ext4", "fixture", "out of memory for the RAM volume");
        } else if (ext4_format(d) != VFS_OK) {
            fsm_bad("ext4", "fixture", "ext4_format failed");
        } else {
            bcache_invalidate(d);
            int m = ext4_mount("/fsm4", d);
            if (m != VFS_OK) {
                char msg[96];
                ksnprintf(msg, sizeof msg, "ext4_mount rc=%d", m);
                fsm_bad("ext4", "fixture", msg);
            } else {
                fsm_run("ext4", "/fsm4", true, true);
                fsm_coherence("ext4", "/fsm4");
                fsm_unlink_open("ext4", "/fsm4");
                fsm_fsync_check("ext4", "/fsm4");
                (void)vfs_unmount("/fsm4");
            }
        }
    }

    /* fat32 on a freshly formatted RAM volume. FAT32 is DEFINED as more
     * than 65524 clusters, so the fixture has to clear that bar: 70000
     * single-sector clusters is the smallest round number that does,
     * at ~34 MiB. */
    {
        struct blk_dev *d = fsm_make_disk("fsmatrix1", 70000);
        if (!d) {
            fsm_bad("fat32", "fixture", "out of memory for the RAM volume");
        } else {
            int fr = fat32_format(d);
            if (fr != VFS_OK) {
                char msg[96];
                ksnprintf(msg, sizeof msg, "fat32_format rc=%d", fr);
                fsm_bad("fat32", "fixture", msg);
            } else {
                bcache_invalidate(d);
                /* Mounting what we just wrote is itself the first
                 * assertion: the driver has to accept the volume its
                 * own formatter produced. */
                int m = fat32_mount("/fsmfat", d);
                char msg[96];
                ksnprintf(msg, sizeof msg, "format+mount rc=%d", m);
                fsm_chk(m == VFS_OK, "fat32", "format then mount", msg);
                if (m == VFS_OK) {
                    /* No perms and no hard links -- both are real
                     * properties of FAT, not gaps in the driver. */
                    fsm_run("fat32", "/fsmfat", false, false);
                    fsm_coherence("fat32", "/fsmfat");
                    fsm_unlink_open("fat32", "/fsmfat");
                    fsm_fsync_check("fat32", "/fsmfat");
                    (void)vfs_unmount("/fsmfat");
                }
            }
        }
    }

    kprintf("[FSMATRIX] NOTE: FAT cannot express hard links at all -- no inode to point a second name at\n");

    kprintf("[FSMATRIX] VERDICT: %s pass=%d fail=%d\n",
            g_fsm_fail ? "FAIL" : "PASS", g_fsm_pass, g_fsm_fail);
}
