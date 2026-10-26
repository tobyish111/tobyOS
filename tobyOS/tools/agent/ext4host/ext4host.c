/* ext4host.c -- src/ext4.c on the HOST, against an image file (finding
 * 271). Includes the driver whole, with stub headers for the kernel heap,
 * printk and klibc, a block device that is the image, and drives the
 * driver's vfs_ops the way the VFS does, from a command language on stdin.
 * What it leaves is judged by Linux's own tools: e2fsck -fn, a Linux mount,
 * and the manifest of each (logs/b34/ext4stress.py).
 *
 *   gcc -O1 -g -fsanitize=address,undefined -Itools/agent/ext4host/stub \
 *       -Iinclude tools/agent/ext4host/ext4host.c -o /tmp/ext4host
 *   /tmp/ext4host IMAGE [ro] < SCRIPT      (ro: mounted read-only)
 *
 * One command a line; paths have no blanks:
 *   create P MODE        mkdir P MODE        unlink P (a directory too)
 *   write P OFF LEN SEED (bytes pat(SEED, offset), the file opened, written
 *                         at OFF and closed)
 *   trunc P LEN          rename A B          link A B
 *   chmod P MODE         chown P UID GID     utime P MTIME ATIME
 *   wseq P N BS          (P opened once, N writes of BS bytes from where
 *                         it ends, closed: as dd bs=BS appends)
 *   ls P                 (P's entries counted, as a readdir gives them)
 *   io                   (the device reads and writes since the last
 *                         `io`, and their sectors; then zeroed)
 *   manifest             (every name under /, sorted: path, type, mode,
 *                         uid, gid, links, size, the contents' CRC-32)
 *   verify               (every checksum on the volume, and every htree
 *                         name's hash, recomputed by the driver's own
 *                         functions against what is stored)
 *   umount
 * Each command prints "> LINE rc=N"; the driver's messages go to stderr. */
#include "../../../src/ext4.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- the kernel around the driver ---- */

static int g_fd;
static unsigned long long g_rd, g_rd_sec, g_wr, g_wr_sec;   /* `io` (finding 278) */

static int img_read(struct blk_dev *d, uint64_t lba, uint32_t n, void *b)
{
    (void)d;
    g_rd++; g_rd_sec += n;
    ssize_t want = (ssize_t)n * BLK_SECTOR_SIZE;
    return pread(g_fd, b, (size_t)want, (off_t)(lba * BLK_SECTOR_SIZE)) == want ? 0 : -1;
}

static int img_write(struct blk_dev *d, uint64_t lba, uint32_t n, const void *b)
{
    (void)d;
    g_wr++; g_wr_sec += n;
    ssize_t want = (ssize_t)n * BLK_SECTOR_SIZE;
    return pwrite(g_fd, b, (size_t)want, (off_t)(lba * BLK_SECTOR_SIZE)) == want ? 0 : -1;
}

static const struct blk_ops g_img_ops = { .read = img_read, .write = img_write };
static struct blk_dev g_dev = { .name = "img", .ops = &g_img_ops };

/* the block layer's write watch (finding 278): one image, no other writer */
bool blk_watch_add(blk_write_watch_fn fn, void *ctx) { (void)fn; (void)ctx; return true; }
void blk_watch_remove(blk_write_watch_fn fn, void *ctx) { (void)fn; (void)ctx; }
struct blk_dev *blk_disk_of(struct blk_dev *dev, uint64_t *off)
{
    if (off) *off = 0;
    return dev;
}

struct proc *current_proc(void) { return 0; }
void cgroup_io_charge(struct proc *p, bool w, uint64_t n) { (void)p; (void)w; (void)n; }
void bcache_sync(struct blk_dev *d) { (void)d; }
bool devfs_blk_path(const struct blk_dev *d, char *out, size_t cap)
{
    (void)d; (void)out; (void)cap;
    return false;
}

/* the wall clock as of the last 4 ms tick, as the kernel's file_time_ns */
uint64_t file_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t n = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return n - n % 4000000ull;
}

static void *g_mnt;
int vfs_mount(const char *mp, const struct vfs_ops *ops, void *d)
{
    (void)mp; (void)ops;
    g_mnt = d;
    return VFS_OK;
}
static uint32_t g_mount_flags;
int vfs_mount_flags(const char *mp, const struct vfs_ops *ops, void *d, uint32_t flags)
{
    g_mount_flags = flags;
    return vfs_mount(mp, ops, d);
}
int vfs_mount_set_source(const char *mp, const char *src)
{
    (void)mp; (void)src;
    return VFS_OK;
}

/* ---- the harness ---- */

static uint8_t pat(uint32_t seed, uint64_t pos)
{
    return (uint8_t)(seed * 167u + pos * 31u + (pos >> 8) * 7u + (pos >> 16));
}

static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int do_write(const char *p, uint64_t off, uint64_t len, uint32_t seed)
{
    struct vfs_file f;
    memset(&f, 0, sizeof f);
    f.ops = &ext4_ops;
    f.mnt = g_mnt;
    int rc = ext4_open(g_mnt, p, &f);
    if (rc != VFS_OK) return rc;
    uint8_t *buf = malloc(len ? len : 1);
    for (uint64_t i = 0; i < len; i++) buf[i] = pat(seed, off + i);
    f.pos = (size_t)off;
    long got = ext4_write(&f, buf, (size_t)len);
    free(buf);
    ext4_close(&f);
    if (got < 0) return (int)got;
    return got == (long)len ? VFS_OK : VFS_ERR_IO;
}

/* P opened once and N writes of BS bytes from its end, as `dd bs=BS`
 * appends */
static int do_wseq(const char *p, uint64_t n, uint64_t bs)
{
    struct vfs_file f;
    memset(&f, 0, sizeof f);
    f.ops = &ext4_ops;
    f.mnt = g_mnt;
    int rc = ext4_open(g_mnt, p, &f);
    if (rc != VFS_OK) return rc;
    uint8_t *buf = malloc(bs ? bs : 1);
    f.pos = f.size;
    for (uint64_t k = 0; k < n && rc == VFS_OK; k++) {
        for (uint64_t i = 0; i < bs; i++) buf[i] = pat(7, f.pos + i);
        long got = ext4_write(&f, buf, (size_t)bs);
        if (got != (long)bs) rc = got < 0 ? (int)got : VFS_ERR_IO;
    }
    free(buf);
    ext4_close(&f);
    return rc;
}

static int do_ls(const char *p, unsigned long long *count)
{
    struct vfs_dir d;
    memset(&d, 0, sizeof d);
    d.mnt = g_mnt;
    int rc = ext4_opendir(g_mnt, p, &d);
    if (rc != VFS_OK) return rc;
    struct vfs_dirent e;
    *count = 0;
    while (ext4_readdir(&d, &e) == VFS_OK) (*count)++;
    ext4_closedir(&d);
    return VFS_OK;
}

struct name { char s[VFS_NAME_MAX]; };
static int name_cmp(const void *a, const void *b)
{
    return strcmp(((const struct name *)a)->s, ((const struct name *)b)->s);
}

static void manifest(const char *dir)
{
    struct vfs_dir d;
    memset(&d, 0, sizeof d);
    d.mnt = g_mnt;
    if (ext4_opendir(g_mnt, dir, &d) != VFS_OK) {
        printf("%s opendir-failed\n", dir);
        return;
    }
    struct name *v = 0;
    size_t n = 0, cap = 0;
    struct vfs_dirent e;
    while (ext4_readdir(&d, &e) == VFS_OK) {
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, cap * sizeof *v); }
        snprintf(v[n++].s, VFS_NAME_MAX, "%s", e.name);
    }
    ext4_closedir(&d);
    qsort(v, n, sizeof *v, name_cmp);
    for (size_t i = 0; i < n; i++) {
        char p[VFS_PATH_MAX];
        snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", v[i].s);
        struct vfs_stat st;
        if (ext4_stat(g_mnt, p, &st) != VFS_OK) { printf("%s stat-failed\n", p); continue; }
        if (st.type == VFS_TYPE_DIR) {
            printf("%s d %o %u %u %u - -\n", p, st.mode & 07777, st.uid, st.gid, st.nlink);
            manifest(p);
            continue;
        }
        struct vfs_file f;
        memset(&f, 0, sizeof f);
        f.ops = &ext4_ops;
        f.mnt = g_mnt;
        if (ext4_open(g_mnt, p, &f) != VFS_OK) {      /* a link, a device, ... */
            printf("%s o %o %u %u %u - -\n", p, st.mode & 07777, st.uid, st.gid, st.nlink);
            continue;
        }
        uint32_t crc = 0;
        uint64_t total = 0;
        static uint8_t buf[65536];
        long got;
        while ((got = ext4_read(&f, buf, sizeof buf)) > 0) {
            crc = crc32_upd(crc, buf, (size_t)got);
            total += (uint64_t)got;
        }
        ext4_close(&f);
        printf("%s f %o %u %u %u %llu %08x%s\n", p, st.mode & 07777, st.uid, st.gid,
               st.nlink, (unsigned long long)total, crc,
               (uint64_t)st.size == total ? "" : " size-differs");
    }
    free(v);
}

/* ---- verify: every sum on the volume against the driver's own ---- */

struct tally { const char *what; unsigned ok, bad, shown; };
static void tally(struct tally *t, int good, const char *fmt, unsigned long long a,
                  unsigned long long b, unsigned long long c)
{
    if (good) { t->ok++; return; }
    t->bad++;
    if (t->shown++ < 5) {
        printf("  %s MISMATCH: ", t->what);
        printf(fmt, a, b, c);
        printf("\n");
    }
}

static int raw_inode(struct ext4 *fs, uint32_t ino, uint8_t *raw)
{
    uint32_t g = (ino - 1) / fs->inodes_per_group, idx = (ino - 1) % fs->inodes_per_group;
    uint64_t off = (uint64_t)idx * fs->inode_size;
    uint64_t blk = group_inode_table(fs, g) + off / fs->block_size;
    if (read_block(fs, blk, fs->blk_buf) != VFS_OK) return -1;
    memcpy(raw, fs->blk_buf + off % fs->block_size, fs->inode_size);
    return 0;
}

static struct tally t_sb = {"superblock"}, t_gd = {"group desc"}, t_bb = {"block bitmap"},
                    t_ib = {"inode bitmap"}, t_in = {"inode"}, t_dl = {"dir leaf"},
                    t_dn = {"dir no-tail"}, t_dx = {"htree"}, t_ex = {"extent block"},
                    t_xa = {"xattr block"};

static void verify_extents(struct ext4 *fs, uint32_t ino, uint32_t iseed, const uint8_t *node, int depth)
{
    const struct ext4_extent_header *eh = (const struct ext4_extent_header *)node;
    if (eh->eh_magic != EXT4_EXT_MAGIC || eh->eh_depth == 0 || depth > 5) return;
    const struct ext4_extent_idx *ix = (const struct ext4_extent_idx *)(eh + 1);
    for (unsigned i = 0; i < eh->eh_entries; i++) {
        uint64_t b = ((uint64_t)ix[i].ei_leaf_hi << 32) | ix[i].ei_leaf_lo;
        uint8_t *blk = malloc(fs->block_size);
        if (read_block(fs, b, blk) == VFS_OK) {
            uint32_t want, have = extent_block_csum(iseed, blk);
            memcpy(&want, blk + extent_block_csum_off(blk), 4);
            tally(&t_ex, want == have, "ino %llu block %llu stored %llx", ino, b, want);
            verify_extents(fs, ino, iseed, blk, depth + 1);
        }
        free(blk);
    }
}

static struct tally t_hash = {"name hash"};

/* every name in each leaf an index entry points at hashes into that entry's
 * range [its hash, the next one's) -- the driver's dx_hash against Linux's */
static void verify_dx_range(struct ext4 *fs, const struct ext4_inode *in, uint32_t ino,
                            uint8_t version, const uint8_t *node, uint32_t coff, int levels,
                            uint32_t lo_bound, uint32_t hi_bound, int hi_cont)
{
    uint16_t count = dx_get16(node, coff + 2);
    uint8_t *blk = malloc(fs->block_size);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t lo = i ? dx_hash_of(node, coff, i) & ~1u : lo_bound;
        uint32_t next = i + 1 < count ? dx_hash_of(node, coff, i + 1) : 0;
        uint32_t hi = i + 1 < count ? next & ~1u : hi_bound;
        int cont = i + 1 < count ? (int)(next & 1u) : hi_cont;
        uint64_t phys = 0;
        if (inode_block_map(fs, in, dx_block_of(node, coff, i), &phys) != VFS_OK || !phys) continue;
        if (read_block(fs, phys, blk) != VFS_OK) continue;
        if (levels > 0) {
            verify_dx_range(fs, in, ino, version, blk, DX_NODE_COUNT_OFF, levels - 1, lo, hi, cont);
            continue;
        }
        uint32_t off = 0, end = dir_leaf_end(fs, blk);
        while (off + 8 <= end) {
            const struct ext4_dir_entry_2 *de = (const struct ext4_dir_entry_2 *)(blk + off);
            if (de->rec_len < 8) break;
            if (de->inode && de->name_len) {
                uint32_t h = dx_hash(fs, de->name, de->name_len, version);
                /* in [lo, hi); at hi only when the next range is marked as
                 * going on from this leaf */
                tally(&t_hash, h >= lo && (h < hi || (h == hi && cont)),
                      "ino %llu hash %llx range lo %llx", ino, h, lo);
            }
            off += de->rec_len;
        }
    }
    free(blk);
}

static void verify_dir(struct ext4 *fs, uint32_t ino, uint32_t iseed, const struct ext4_inode *in)
{
    if (in->i_flags & EXT4_INDEX_FL) {
        uint64_t phys = 0;
        uint8_t *root = malloc(fs->block_size);
        if (inode_block_map(fs, in, 0, &phys) == VFS_OK && phys && read_block(fs, phys, root) == VFS_OK)
            verify_dx_range(fs, in, ino, root[0x1C], root, DX_ROOT_COUNT_OFF, root[0x1E], 0,
                            0xFFFFFFFFu, 0);
        free(root);
    }
    uint64_t size = ((uint64_t)in->i_size_hi << 32) | in->i_size_lo;
    uint8_t *blk = malloc(fs->block_size);
    for (uint32_t l = 0; l < size / fs->block_size; l++) {
        uint64_t phys = 0;
        if (inode_block_map(fs, in, l, &phys) != VFS_OK || !phys) continue;
        if (read_block(fs, phys, blk) != VFS_OK) continue;
        const struct ext4_dir_entry_2 *de = (const struct ext4_dir_entry_2 *)blk;
        uint32_t want;
        if ((in->i_flags & EXT4_INDEX_FL) && l == 0) {
            uint32_t coff = 0x18u + blk[0x1D];          /* dx_root_info's length */
            uint16_t limit; memcpy(&limit, blk + coff, 2);
            memcpy(&want, blk + coff + limit * 8u + 4, 4);
            tally(&t_dx, want == dx_csum(iseed, blk, coff), "ino %llu root, block %llu stored %llx", ino, phys, want);
        } else if ((in->i_flags & EXT4_INDEX_FL) && de->inode == 0 && de->rec_len == fs->block_size) {
            uint16_t limit; memcpy(&limit, blk + 8, 2);
            memcpy(&want, blk + 8 + limit * 8u + 4, 4);
            tally(&t_dx, want == dx_csum(iseed, blk, 8), "ino %llu node, block %llu stored %llx", ino, phys, want);
        } else if (dir_tail_at(fs, blk)) {
            memcpy(&want, blk + fs->block_size - 4, 4);
            tally(&t_dl, want == dir_leaf_csum(fs, iseed, blk), "ino %llu block %llu stored %llx", ino, phys, want);
        } else {
            tally(&t_dn, 0, "ino %llu lblk %llu block %llu", ino, l, phys);
        }
    }
    free(blk);
}

static void verify(struct ext4 *fs)
{
    struct ext4_super_block *sb = (struct ext4_super_block *)fs->sb_buf;
    printf("verify: csum=%d gdt_csum=%d seed=%08x groups=%u desc=%u inode=%u\n", fs->csum,
           fs->gdt_csum, fs->csum_seed, fs->group_count, fs->desc_size, fs->inode_size);
    if (fs->csum) tally(&t_sb, sb->s_checksum == sb_csum(sb), "stored %llx want %llx (%llu)",
                        sb->s_checksum, sb_csum(sb), 0);
    uint8_t *bmp = malloc(fs->block_size), *raw = malloc(fs->inode_size);
    for (uint32_t g = 0; g < fs->group_count; g++) {
        struct ext4_group_desc_64 *gd = (struct ext4_group_desc_64 *)group_desc_mut(fs, g);
        if (fs->csum || fs->gdt_csum)
            tally(&t_gd, gd->bg_checksum == gd_csum(fs, g, (const uint8_t *)gd),
                  "group %llu stored %llx want %llx", g, gd->bg_checksum, gd_csum(fs, g, (const uint8_t *)gd));
        if (!fs->csum) continue;
        bool big = fs->desc_size >= 64;
        if (!(gd->bg_flags & EXT4_BG_BLOCK_UNINIT) && read_block(fs, group_block_bitmap(fs, g), bmp) == VFS_OK) {
            uint32_t c = bitmap_csum(fs, bmp, fs->blocks_per_group);
            uint32_t s = gd->bg_block_bitmap_csum_lo | (big ? (uint32_t)gd->bg_block_bitmap_csum_hi << 16 : 0);
            tally(&t_bb, s == (big ? c : (c & 0xFFFF)), "group %llu stored %llx computed %llx", g, s, c);
        }
        if (gd->bg_flags & EXT4_BG_INODE_UNINIT) continue;
        if (read_block(fs, group_inode_bitmap(fs, g), bmp) != VFS_OK) continue;
        uint32_t c = bitmap_csum(fs, bmp, fs->inodes_per_group);
        uint32_t s = gd->bg_inode_bitmap_csum_lo | (big ? (uint32_t)gd->bg_inode_bitmap_csum_hi << 16 : 0);
        tally(&t_ib, s == (big ? c : (c & 0xFFFF)), "group %llu stored %llx computed %llx", g, s, c);
        uint32_t used = fs->inodes_per_group - (gd->bg_itable_unused_lo |
                        (big ? (uint32_t)gd->bg_itable_unused_hi << 16 : 0));
        for (uint32_t i = 0; i < used; i++) {
            if (!(bmp[i / 8] & (1u << (i % 8)))) continue;
            uint32_t ino = g * fs->inodes_per_group + i + 1;
            if (raw_inode(fs, ino, raw)) continue;
            uint32_t want = inode_csum(fs, ino, raw);
            uint16_t lo, hi = 0;
            memcpy(&lo, raw + EXT4_I_CSUM_LO_OFF, 2);
            bool has_hi = inode_has_csum_hi(fs, raw);
            if (has_hi) memcpy(&hi, raw + EXT4_I_CSUM_HI_OFF, 2);
            uint32_t s = lo | ((uint32_t)hi << 16);
            tally(&t_in, s == (has_hi ? want : (want & 0xFFFF)), "ino %llu stored %llx computed %llx", ino, s, want);
            struct ext4_inode in;
            if (read_inode(fs, ino, &in) != VFS_OK) continue;
            uint32_t iseed = inode_seed(fs, ino, in.i_generation);
            if (in.i_flags & EXT4_EXTENTS_FL) verify_extents(fs, ino, iseed, (const uint8_t *)in.i_block, 0);
            if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) verify_dir(fs, ino, iseed, &in);
            uint64_t acl = in.i_file_acl_lo | ((uint64_t)osd2_half(&in, 2) << 32);
            if (acl && read_block(fs, acl, bmp) == VFS_OK) {
                uint32_t xs; memcpy(&xs, bmp + EXT4_XATTR_CSUM_OFF, 4);
                tally(&t_xa, xs == xattr_block_csum(fs, acl, bmp), "ino %llu block %llu stored %llx", ino, acl, xs);
            }
        }
    }
    free(bmp); free(raw);
    struct tally *all[] = {&t_sb, &t_gd, &t_bb, &t_ib, &t_in, &t_dl, &t_dn, &t_dx, &t_ex, &t_xa,
                           &t_hash};
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++)
        printf("  %-13s %u ok, %u bad\n", all[i]->what, all[i]->ok, all[i]->bad);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "ro"))) {
        fprintf(stderr, "usage: ext4host IMAGE [ro] < SCRIPT\n");
        return 2;
    }
    bool ro = argc == 3;
    setvbuf(stdout, NULL, _IOLBF, 0);    /* every line out, whatever ends the run */
    g_fd = open(argv[1], O_RDWR);
    if (g_fd < 0) { perror(argv[1]); return 2; }
    struct stat sb;
    fstat(g_fd, &sb);
    g_dev.sector_count = (uint64_t)sb.st_size / BLK_SECTOR_SIZE;
    int rc = ext4_mount_ro("/x", &g_dev, ro);
    printf("> mount%s rc=%d flags=%u\n", ro ? " ro" : "", rc, g_mount_flags);
    if (rc != VFS_OK) return 1;

    char line[4 * VFS_PATH_MAX];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char op[32], a[VFS_PATH_MAX] = "", b[VFS_PATH_MAX] = "";
        unsigned long long x = 0, y = 0, z = 0;
        int k = sscanf(line, "%31s %1023s", op, a);
        if (k < 1) continue;
        rc = VFS_OK;
        if (!strcmp(op, "create")) {
            sscanf(line, "%*s %*s %llo", &x);
            rc = ext4_create(g_mnt, a, 0, 0, (uint32_t)x);
        } else if (!strcmp(op, "mkdir")) {
            sscanf(line, "%*s %*s %llo", &x);
            rc = ext4_mkdir(g_mnt, a, 0, 0, (uint32_t)x);
        } else if (!strcmp(op, "write")) {
            sscanf(line, "%*s %*s %llu %llu %llu", &x, &y, &z);
            rc = do_write(a, x, y, (uint32_t)z);
        } else if (!strcmp(op, "trunc")) {
            sscanf(line, "%*s %*s %llu", &x);
            rc = ext4_truncate(g_mnt, a, x);
        } else if (!strcmp(op, "unlink")) {
            rc = ext4_unlink(g_mnt, a);
        } else if (!strcmp(op, "rename")) {
            sscanf(line, "%*s %*s %1023s", b);
            rc = ext4_rename(g_mnt, a, b);
        } else if (!strcmp(op, "link")) {
            sscanf(line, "%*s %*s %1023s", b);
            rc = ext4_link(g_mnt, a, b);
        } else if (!strcmp(op, "chmod")) {
            sscanf(line, "%*s %*s %llo", &x);
            rc = ext4_chmod(g_mnt, a, (uint32_t)x);
        } else if (!strcmp(op, "chown")) {
            sscanf(line, "%*s %*s %llu %llu", &x, &y);
            rc = ext4_chown(g_mnt, a, (uint32_t)x, (uint32_t)y);
        } else if (!strcmp(op, "utime")) {
            sscanf(line, "%*s %*s %llu %llu", &x, &y);
            rc = ext4_utimens(g_mnt, a, x, 0, y, 0);
        } else if (!strcmp(op, "wseq")) {
            sscanf(line, "%*s %*s %llu %llu", &x, &y);
            rc = do_wseq(a, x, y);
        } else if (!strcmp(op, "ls")) {
            rc = do_ls(a, &x);
            printf("> %s rc=%d entries=%llu\n", line, rc, x);
            continue;
        } else if (!strcmp(op, "io")) {
            printf("> io reads=%llu (%llu sectors) writes=%llu (%llu sectors)\n",
                   g_rd, g_rd_sec, g_wr, g_wr_sec);
            g_rd = g_rd_sec = g_wr = g_wr_sec = 0;
            continue;
        } else if (!strcmp(op, "manifest")) {
            manifest("/");
            continue;
        } else if (!strcmp(op, "verify")) {
            verify((struct ext4 *)g_mnt);
            continue;
        } else if (!strcmp(op, "umount")) {
            rc = ext4_ops.umount ? ext4_ops.umount(g_mnt) : VFS_OK;
            g_mnt = 0;
            printf("> %s rc=%d\n", line, rc);
            break;
        } else {
            printf("> %s unknown\n", line);
            continue;
        }
        printf("> %s rc=%d\n", line, rc);
    }
    fsync(g_fd);
    close(g_fd);
    return 0;
}
