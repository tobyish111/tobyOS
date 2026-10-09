/* tmpfshost.c -- src/tmpfs.c on the HOST, past the old 512-node table
 * (finding 55). Includes the driver whole, with stub headers for the kernel
 * heap, printk, klibc and proc, and drives its ops directly.
 *
 *   gcc -O1 -g -fsanitize=address -Itools/agent/tmpfshost/stub -Iinclude \
 *       tools/agent/tmpfshost/tmpfshost.c -o /c/t/tmpfshost && /c/t/tmpfshost
 *
 * Prints one line per check and "tmpfshost: N/N" at the end; exit 0 only
 * when every check passed. */
#include "../../../src/tmpfs.c"
#include <stdio.h>

uint64_t lx_realtime_ns(uint64_t mono_ns) { return mono_ns; }
uint64_t perf_now_ns(void) { return 0; }

static void *g_last_mount;
int vfs_mount(const char *mp, const struct vfs_ops *ops, void *d)
{
    (void)mp; (void)ops;
    g_last_mount = d;
    return VFS_OK;
}

static int g_pass, g_total;
static void check(int ok, const char *what)
{
    g_total++;
    if (ok) g_pass++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static void *mount_with(const char *opts, int *rc)
{
    g_last_mount = 0;
    *rc = tmpfs_mount_at("/x", opts);
    return *rc == VFS_OK ? g_last_mount : 0;
}

static int fill(void *m, const char *pfx, int upto)
{
    char p[64];
    int n = 0;
    for (; n < upto; n++) {
        snprintf(p, sizeof p, "/%s%d", pfx, n);
        if (tmpfs_create(m, p, 0, 0, 0644) != VFS_OK) break;
    }
    return n;
}

int main(void)
{
    int rc;
    char p[64], buf[16];

    /* The default mount: 16 MiB, so 16384 nodes -- far past the old 512. */
    void *m = mount_with(0, &rc);
    struct tmpfs_mount *tm = m;
    check(m && tm->max_nodes == 16384, "default mount allows 16384 nodes");
    check(tm->nnodes == 0, "no chunk is allocated before the first file");
    int made = fill(m, "f", 5000);
    check(made == 5000, "5000 files created (the old table stopped at 512)");
    check(tm->nnodes == 5000 + (TMPFS_CHUNK_NODES - 5000 % TMPFS_CHUNK_NODES),
          "the table grew a chunk at a time");

    struct vfs_statfs sf;
    tmpfs_statfs(m, &sf);
    check(sf.files == 16384 && sf.ffree == 16384 - 5000,
          "statfs counts the limit and the free nodes");

    /* Every node's inode number is its own, and none is the root's. */
    struct vfs_stat st, root;
    tmpfs_stat(m, "/", &root);
    static unsigned char seen[16384 + 2];
    int dup = 0, bad = 0;
    for (int i = 0; i < 5000; i++) {
        snprintf(p, sizeof p, "/f%d", i);
        if (tmpfs_stat(m, p, &st) != VFS_OK) { bad++; continue; }
        if (st.ino == root.ino || st.ino == 0 || st.ino > 16384) bad++;
        else if (seen[st.ino]++) dup++;
    }
    check(!dup && !bad && root.ino == 16385, "5000 distinct inode numbers, root past them");

    /* readdir walks every chunk. */
    struct vfs_dir d;
    struct vfs_dirent de;
    int listed = 0;
    tmpfs_opendir(m, "/", &d);
    while (tmpfs_readdir(&d, &de) == VFS_OK) listed++;
    tmpfs_closedir(&d);
    check(listed == 5000, "readdir lists all 5000");

    /* A hard link to a node in a late chunk, made when talloc must grow:
     * the owner's index survives the growth. */
    made = fill(m, "g", 120);                 /* 5120: 40 chunks, all full */
    check(made == 120 && tm->nnodes == 5120, "120 more files fill the last chunk");
    struct vfs_stat a, b;
    tmpfs_stat(m, "/f4999", &a);
    rc = tmpfs_link(m, "/f4999", "/l4999");   /* its talloc must grow */
    tmpfs_stat(m, "/l4999", &b);
    check(rc == VFS_OK && tm->nnodes == 5248 && a.ino == b.ino &&
          b.nlink == 2, "a hard link made by growing the table shares the inode");

    /* An open, unlinked file in a late chunk: write, ftruncate and read
     * find its mount by address. */
    struct vfs_file f;
    memset(&f, 0, sizeof f);
    rc = tmpfs_open(m, "/f4321", &f);
    long w = tmpfs_write(&f, "abcdef", 6);
    int tr = tmpfs_ftruncate(&f, 3);
    tmpfs_unlink(m, "/f4321");
    f.pos = 0;
    long r = tmpfs_read(&f, buf, sizeof buf);
    check(rc == VFS_OK && w == 6 && tr == VFS_OK && r == 3 &&
          memcmp(buf, "abc", 3) == 0,
          "write/ftruncate/read through a handle in chunk 33, after unlink");
    size_t before = tm->bytes;
    tmpfs_close(&f);
    check(tm->bytes < before && tmpfs_stat(m, "/f4321", &st) == VFS_ERR_NOENT,
          "the last close frees it and gives the bytes back");

    /* A directory with children across chunks: rmdir refuses, rename moves
     * them all. */
    tmpfs_mkdir(m, "/d", 0, 0, 0755);
    int kids = 0;
    for (int i = 0; i < 300; i++) {
        snprintf(p, sizeof p, "/d/x%d", i);
        if (tmpfs_create(m, p, 0, 0, 0644) == VFS_OK) kids++;
    }
    check(kids == 300, "300 children in /d");
    check(tmpfs_unlink(m, "/d") == VFS_ERR_EXIST, "a non-empty directory is not removed");
    rc = tmpfs_rename(m, "/d", "/e");
    check(rc == VFS_OK && tmpfs_stat(m, "/e/x299", &st) == VFS_OK &&
          tmpfs_stat(m, "/d/x0", &st) == VFS_ERR_NOENT,
          "rename takes every child along");

    /* Freed slots are reused before the table grows again. */
    size_t grown = tm->nnodes;
    for (int i = 0; i < 1000; i++) {
        snprintf(p, sizeof p, "/f%d", i);
        tmpfs_unlink(m, p);
    }
    made = fill(m, "r", 1000);
    check(made == 1000 && tm->nnodes == grown, "1000 freed slots reused, no growth");

    /* The limit is exact: nr_inodes=600 makes 600, not a chunk's multiple. */
    m = mount_with("nr_inodes=600", &rc);
    made = m ? fill(m, "n", 1000) : -1;
    check(made == 600, "nr_inodes=600 gives exactly 600 nodes");
    check(m && tmpfs_create(m, "/one-more", 0, 0, 0644) == VFS_ERR_NOSPC,
          "the 601st is ENOSPC");
    tmpfs_statfs(m, &sf);
    check(sf.files == 600 && sf.ffree == 0, "statfs: 600 files, 0 free");

    /* A small size= keeps at least the old 512. */
    m = mount_with("size=64k", &rc);
    check(m && ((struct tmpfs_mount *)m)->max_nodes == 512, "size=64k: 512 nodes");
    m = mount_with("size=1m,nr_inodes=2k", &rc);
    check(m && ((struct tmpfs_mount *)m)->max_nodes == 2048 &&
          ((struct tmpfs_mount *)m)->max_bytes == 1024 * 1024,
          "size=1m,nr_inodes=2k: both honoured");

    /* Refused, not quietly changed. */
    mount_with("nr_inodes=abc", &rc);
    check(rc == VFS_ERR_INVAL, "nr_inodes=abc refused");
    mount_with("nr_inodes=99999999", &rc);
    check(rc == VFS_ERR_INVAL, "nr_inodes past the 16M limit refused");

    printf("tmpfshost: %d/%d\n", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
