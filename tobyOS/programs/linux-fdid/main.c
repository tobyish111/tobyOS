/* linux-fdid -- an open file is the file its name is (finding 106,
 * 2026-10-09).
 *
 * fstat(2) on a descriptor answered from the descriptor's own copy of the
 * data: a device number no stat(2) ever gave and an inode hashed from the
 * name it was opened by. So the same file had two identities -- a file
 * renamed while open, a hard link, /dev/null, /proc/self/fd/N all disagreed
 * with stat of the same object, and anything that pairs (st_dev, st_ino)
 * from both calls (cp, tar, find -samefile, bash's test -ef on /dev/fd/N,
 * Python's os.path.samestat) saw two files.
 *
 *   bit0  tmpfs (/tmp): fstat == stat; a hard link and a second open of
 *         the same file are the same object
 *   bit1  tobyfs (/data) file and directory: fstat == stat
 *   bit2  the initrd (/etc/passwd), /dev/shm, /proc/self/status and the
 *         devices /dev/null, /dev/full, /dev/random: fstat == stat, the
 *         device numbers too (/dev/random's said /dev/urandom's, /dev/full's
 *         a regular file), and /proc/self/fd/N names the device's node
 *   bit3  renamed while open: fstat == stat of the NEW name; fstat ==
 *         stat("/proc/self/fd/N") == stat("/dev/fd/N")
 *   bit4  the negative: two files on one mount share st_dev and differ in
 *         st_ino; /tmp, /dev/shm and /proc differ in st_dev (and so does
 *         /data) -- a constant answer passes bits 0-3 and fails this
 *   bit5  fstat sees through the descriptor: the type bits are right
 *         (regular, directory, character device) and a write made through
 *         the descriptor shows in fstat's st_size before any close
 *   bit6  a named FIFO: fstat == stat, and it is a FIFO (it said what an
 *         unnamed pipe says, so bash's noclobber refused `echo x > fifo`)
 *   bit7  a symbolic link to /dev/null and to /dev/zero opens the DEVICE:
 *         fstat is a character device, == stat of the link and of the
 *         node, a write is taken and a read is EOF (/dev/null) or zeros
 *         (/dev/zero) -- the open was refused, or opened the link itself
 *
 * Real Linux answers 255. Without /data (an oracle run) bit1 uses /var/tmp.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

static int same(const char *tag, const struct stat *a, const struct stat *b) {
    int ok = a->st_dev == b->st_dev && a->st_ino == b->st_ino;
    printf("fdid: %-14s %s (dev %lx/%lx ino %lu/%lu)\n", tag, ok ? "same" : "DIFFERENT",
           (unsigned long)a->st_dev, (unsigned long)b->st_dev,
           (unsigned long)a->st_ino, (unsigned long)b->st_ino);
    return ok;
}

/* a device opened by NAME: fstat == stat, rdev included, and its
 * /proc/self/fd link names the node */
static int dev(const char *tag, const char *name) {
    struct stat f, s;
    char link[64], lk[64];
    int fd = open(name, O_RDWR);
    if (fd < 0) { printf("fdid: %-14s open %s failed\n", tag, name); return 0; }
    snprintf(lk, sizeof lk, "/proc/self/fd/%d", fd);
    ssize_t ll = readlink(lk, link, sizeof link - 1);
    link[ll > 0 ? ll : 0] = 0;
    int r1 = fstat(fd, &f), r2 = stat(name, &s);
    close(fd);
    if (r1 || r2) { printf("fdid: %-14s fstat=%d stat=%d\n", tag, r1, r2); return 0; }
    int ok = same(tag, &f, &s);
    int rdev = f.st_rdev == s.st_rdev && S_ISCHR(f.st_mode);
    int named = strcmp(link, name) == 0;
    printf("fdid: %-14s rdev %s (%lx/%lx) link %s\n", tag, rdev ? "same" : "DIFFERENT",
           (unsigned long)f.st_rdev, (unsigned long)s.st_rdev, link);
    return ok && rdev && named;
}

/* open PATH, fstat it, stat PATH, compare */
static int one(const char *tag, const char *path, int flags) {
    struct stat f, s;
    int fd = open(path, flags);
    if (fd < 0) { printf("fdid: %-14s open %s failed\n", tag, path); return 0; }
    int r1 = fstat(fd, &f), r2 = stat(path, &s);
    close(fd);
    if (r1 || r2) { printf("fdid: %-14s fstat=%d stat=%d\n", tag, r1, r2); return 0; }
    return same(tag, &f, &s);
}

static void put(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { (void)!write(fd, text, strlen(text)); close(fd); }
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    char t[64], shm[64];
    snprintf(t, sizeof t, "/tmp/fdid.%d", (int)getpid());
    snprintf(shm, sizeof shm, "/dev/shm/fdid.%d", (int)getpid());
    const char *data_root = access("/data", W_OK) == 0 ? "/data" : "/var/tmp";
    int have_data = strcmp(data_root, "/data") == 0;
    char D[64];
    snprintf(D, sizeof D, "%s/fdid.%d", data_root, (int)getpid());
    mkdir(t, 0755);
    mkdir(D, 0755);

    char f[96], g[96], r[96], r2[96], h[96], df[96];
    snprintf(f, sizeof f, "%s/f", t);
    snprintf(g, sizeof g, "%s/g", t);
    snprintf(r, sizeof r, "%s/r", t);
    snprintf(r2, sizeof r2, "%s/r2", t);
    snprintf(h, sizeof h, "%s/h", t);
    snprintf(df, sizeof df, "%s/f", D);
    put(f, "x\n");
    put(r, "r\n");
    put(h, "other\n");
    put(df, "y\n");
    put(shm, "z\n");
    int linked = link(f, g) == 0;

    /* ---- bit0: tmpfs ---- */
    {
        int ok = one("tmpfs", f, O_RDONLY);
        struct stat a, b;
        int fd = open(g, O_RDONLY);
        ok &= linked && fd >= 0 && fstat(fd, &a) == 0 && stat(f, &b) == 0 &&
              same("hard-link", &a, &b);
        if (fd >= 0) close(fd);
        int x = open(f, O_RDONLY), y = open(f, O_RDONLY);
        ok &= x >= 0 && y >= 0 && fstat(x, &a) == 0 && fstat(y, &b) == 0 &&
              same("two-opens", &a, &b);
        if (x >= 0) close(x);
        if (y >= 0) close(y);
        if (ok) bits |= 1;
    }

    /* ---- bit1: tobyfs ---- */
    {
        int ok = one("tobyfs", df, O_RDONLY);
        ok &= one("tobyfs-dir", D, O_RDONLY | O_DIRECTORY);
        if (ok) bits |= 2;
    }

    /* ---- bit2: the initrd, /dev/shm, /proc, /dev ---- */
    {
        int ok = one("initrd", "/etc/passwd", O_RDONLY);
        ok &= one("shm", shm, O_RDONLY);
        ok &= one("proc", "/proc/self/status", O_RDONLY);
        ok &= one("devnull", "/dev/null", O_WRONLY);
        ok &= dev("dev-null", "/dev/null");
        ok &= dev("dev-full", "/dev/full");
        ok &= dev("dev-random", "/dev/random");
        ok &= dev("dev-urandom", "/dev/urandom");
        if (ok) bits |= 4;
    }

    /* ---- bit3: renamed while open; the magic links ---- */
    {
        struct stat a, b, c, d;
        int ok = 0;
        int fd = open(r, O_RDONLY);
        if (fd >= 0 && rename(r, r2) == 0 && fstat(fd, &a) == 0 && stat(r2, &b) == 0) {
            char m1[48], m2[48];
            snprintf(m1, sizeof m1, "/proc/self/fd/%d", fd);
            snprintf(m2, sizeof m2, "/dev/fd/%d", fd);
            ok = same("renamed", &a, &b);
            ok &= stat(m1, &c) == 0 && same("proc-fd", &a, &c);
            ok &= stat(m2, &d) == 0 && same("dev-fd", &a, &d);
        } else {
            printf("fdid: renamed        setup failed\n");
        }
        if (fd >= 0) close(fd);
        if (ok) bits |= 8;
    }

    /* ---- bit4: different files are different ---- */
    {
        struct stat a, b, s, p, dd;
        int ok = stat(f, &a) == 0 && stat(h, &b) == 0 && stat(shm, &s) == 0 &&
                 stat("/proc/self/status", &p) == 0 && stat(df, &dd) == 0;
        if (ok) {
            int one_mount = a.st_dev == b.st_dev && a.st_ino != b.st_ino;
            int mounts = a.st_dev != s.st_dev && a.st_dev != p.st_dev && s.st_dev != p.st_dev;
            int data = !have_data || dd.st_dev != a.st_dev;
            printf("fdid: distinct       one-mount=%d mounts=%d data=%d\n",
                   one_mount, mounts, data);
            ok = one_mount && mounts && data;
        }
        if (ok) bits |= 16;
    }

    /* ---- bit5: fstat reads the object, not a copy ---- */
    {
        struct stat a, b, c, d, e;
        int ok = 0;
        int fd = open(f, O_RDWR | O_APPEND);
        int dfd = open(t, O_RDONLY | O_DIRECTORY);
        int nfd = open("/dev/null", O_WRONLY);
        if (fd >= 0 && dfd >= 0 && nfd >= 0 &&
            fstat(fd, &a) == 0 && write(fd, "more\n", 5) == 5 && fstat(fd, &b) == 0 &&
            fstat(dfd, &c) == 0 && fstat(nfd, &d) == 0 && stat(f, &e) == 0) {
            printf("fdid: types          reg=%d dir=%d chr=%d size %ld->%ld (stat %ld)\n",
                   S_ISREG(a.st_mode), S_ISDIR(c.st_mode), S_ISCHR(d.st_mode),
                   (long)a.st_size, (long)b.st_size, (long)e.st_size);
            ok = S_ISREG(a.st_mode) && S_ISDIR(c.st_mode) && S_ISCHR(d.st_mode) &&
                 b.st_size == a.st_size + 5 && e.st_size == b.st_size;
        }
        if (fd >= 0) close(fd);
        if (dfd >= 0) close(dfd);
        if (nfd >= 0) close(nfd);
        if (ok) bits |= 32;
    }

    /* ---- bit6: a named FIFO ---- */
    char p[96];
    snprintf(p, sizeof p, "%s/p", t);
    {
        struct stat a, b;
        int ok = 0;
        int fd = mkfifo(p, 0600) == 0 ? open(p, O_RDWR) : -1;   /* never waits */
        if (fd >= 0 && fstat(fd, &a) == 0 && stat(p, &b) == 0) {
            ok = same("fifo", &a, &b) && S_ISFIFO(a.st_mode);
            printf("fdid: fifo-type      fifo=%d\n", S_ISFIFO(a.st_mode));
        } else {
            printf("fdid: fifo           setup failed\n");
        }
        if (fd >= 0) close(fd);
        if (ok) bits |= 64;
    }

    /* ---- bit7: a link to a device opens the device ---- */
    char ln[96], lz[96];
    snprintf(ln, sizeof ln, "%s/ln", t);
    snprintf(lz, sizeof lz, "%s/lz", t);
    {
        struct stat a, b, c;
        int ok = symlink("/dev/null", ln) == 0 && symlink("/dev/zero", lz) == 0;
        int w = ok ? open(ln, O_WRONLY) : -1;
        int r = ok ? open(ln, O_RDONLY) : -1;
        int z = ok ? open(lz, O_RDONLY) : -1;
        char buf[8];
        memset(buf, 7, sizeof buf);
        if (w >= 0 && r >= 0 && z >= 0 && fstat(w, &a) == 0 && stat(ln, &b) == 0 &&
            stat("/dev/null", &c) == 0) {
            int chr = S_ISCHR(a.st_mode);
            int wr = (int)write(w, "x", 1);
            int rd = (int)read(r, buf, 1);
            int zr = (int)read(z, buf, sizeof buf);
            int zeros = zr == (int)sizeof buf;
            for (int k = 0; zeros && k < (int)sizeof buf; k++)
                if (buf[k]) zeros = 0;
            printf("fdid: link-to-dev    chr=%d write=%d read-null=%d read-zero=%d zeros=%d\n",
                   chr, wr, rd, zr, zeros);
            ok = chr && same("link-dev", &a, &b) && same("link-node", &a, &c) &&
                 wr == 1 && rd == 0 && zeros;
        } else {
            printf("fdid: link-to-dev    open w=%d r=%d z=%d\n", w, r, z);
            ok = 0;
        }
        if (w >= 0) close(w);
        if (r >= 0) close(r);
        if (z >= 0) close(z);
        if (ok) bits |= 128;
    }

    unlink(f); unlink(g); unlink(r2); unlink(h); unlink(df); unlink(shm);
    unlink(p); unlink(ln); unlink(lz);
    rmdir(t); rmdir(D);
    printf("fdid: bits=%d (want 255)\n", bits);
    return bits;
}
