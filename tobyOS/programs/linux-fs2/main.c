/* linux-fs2 -- writable root + hard links (Phase G, 2026-08-22).
 *
 *   bit0  the root (initrd ramfs) accepts NEW files: create, write, read
 *         back, and unlink-while-open keeps the bytes readable
 *   bit1  mkdir on /, a file inside it, then rmdir -- all real
 *   bit2  link(2) on /data: both names read the same bytes, writes through
 *         one are seen through the other, st_nlink == 2 on both
 *   bit3  unlink removes a NAME: the other name survives with its data,
 *         st_nlink drops back to 1
 *   bit4  honesty of refusals: cross-mount link is EXDEV; link on tmpfs
 *         (no inode indirection) is EPERM
 *   bit5  linkat(2) resolves both names against directory fds
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* /data persists between runs (the known disk.img trap): sweep every
     * name this test mints so a crashed or buggy earlier run can't turn
     * a fresh one red with EEXIST. */
    unlink("/data/fs2-a");  unlink("/data/fs2-b");
    unlink("/data/fs2-x");  unlink("/data/fs2-la");
    unlink("/data/fs2-lb");

    /* ---- bit0: new files on the root, unlink-while-open ---- */
    {
        int ok = 0;
        int fd = open("/fs2-root.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd >= 0 && write(fd, "rooted", 6) == 6) {
            unlink("/fs2-root.txt");             /* while still open */
            struct stat st;
            int gone = (stat("/fs2-root.txt", &st) < 0 && errno == ENOENT);
            char b[8] = {0};
            lseek(fd, 0, SEEK_SET);
            int still = (read(fd, b, 6) == 6 && memcmp(b, "rooted", 6) == 0);
            ok = gone && still;
        }
        if (fd >= 0) close(fd);
        printf("fs2: root-create=%d\n", ok);
        if (ok) bits |= 1;
    }

    /* ---- bit1: mkdir on /, file inside, rmdir ---- */
    {
        int ok = 0;
        if (mkdir("/fs2-dir", 0755) == 0) {
            int fd = open("/fs2-dir/f", O_CREAT | O_RDWR, 0644);
            char b[4] = {0};
            int inner = 0;
            if (fd >= 0 && write(fd, "in", 2) == 2) {
                lseek(fd, 0, SEEK_SET);
                inner = (read(fd, b, 2) == 2 && memcmp(b, "in", 2) == 0);
                close(fd);
            }
            int busy = (rmdir("/fs2-dir") < 0);   /* non-empty must refuse */
            unlink("/fs2-dir/f");
            int rr = rmdir("/fs2-dir");
            struct stat st;
            int gone = (stat("/fs2-dir", &st) < 0 && errno == ENOENT);
            ok = inner && busy && rr == 0 && gone;
        }
        printf("fs2: root-mkdir=%d\n", ok);
        if (ok) bits |= 2;
    }

    /* ---- bit2: hard link shares the inode ---- */
    {
        int ok = 0;
        int fd = open("/data/fs2-a", O_CREAT | O_RDWR | O_TRUNC, 0644);
        (void)!write(fd, "AAAA", 4);
        if (link("/data/fs2-a", "/data/fs2-b") == 0) {
            char b[8] = {0};
            int bfd = open("/data/fs2-b", O_RDONLY);
            int same = (bfd >= 0 && read(bfd, b, 4) == 4 &&
                        memcmp(b, "AAAA", 4) == 0);
            if (bfd >= 0) close(bfd);
            lseek(fd, 0, SEEK_SET);
            (void)!write(fd, "BBBB", 4);         /* through the first name */
            memset(b, 0, sizeof b);
            bfd = open("/data/fs2-b", O_RDONLY);
            int seen = (bfd >= 0 && read(bfd, b, 4) == 4 &&
                        memcmp(b, "BBBB", 4) == 0);
            if (bfd >= 0) close(bfd);
            struct stat sa, sb;
            int nl = (stat("/data/fs2-a", &sa) == 0 &&
                      stat("/data/fs2-b", &sb) == 0 &&
                      sa.st_nlink == 2 && sb.st_nlink == 2);
            printf("fs2: link same=%d write-seen=%d nlink=%d/%d\n",
                   same, seen, (int)sa.st_nlink, (int)sb.st_nlink);
            ok = same && seen && nl;
        } else {
            printf("fs2: link failed errno=%d\n", errno);
        }
        if (fd >= 0) close(fd);
        if (ok) bits |= 4;
    }

    /* ---- bit3: unlink one name; the other keeps the data ---- */
    {
        int ok = 0;
        if (unlink("/data/fs2-a") == 0) {
            char b[8] = {0};
            int bfd = open("/data/fs2-b", O_RDONLY);
            int alive = (bfd >= 0 && read(bfd, b, 4) == 4 &&
                         memcmp(b, "BBBB", 4) == 0);
            if (bfd >= 0) close(bfd);
            struct stat sb;
            int nl = (stat("/data/fs2-b", &sb) == 0 && sb.st_nlink == 1);
            struct stat sa;
            int gone = (stat("/data/fs2-a", &sa) < 0 && errno == ENOENT);
            printf("fs2: survivor alive=%d nlink=%d gone=%d\n",
                   alive, nl, gone);
            ok = alive && nl && gone;
        }
        unlink("/data/fs2-b");
        if (ok) bits |= 8;
    }

    /* ---- bit4: EXDEV across mounts; EPERM where links can't exist ---- */
    {
        int fd = open("/data/fs2-x", O_CREAT | O_RDWR, 0644);
        if (fd >= 0) close(fd);
        errno = 0;
        int xr = link("/data/fs2-x", "/tmp/fs2-y");
        int xdev = (xr < 0 && errno == EXDEV);
        int tf = open("/tmp/fs2-t", O_CREAT | O_RDWR, 0644);
        if (tf >= 0) close(tf);
        errno = 0;
        int pr = link("/tmp/fs2-t", "/tmp/fs2-u");
        int eperm = (pr < 0 && errno == EPERM);
        printf("fs2: xdev-errno=%d eperm-errno=%d\n",
               xdev ? EXDEV : errno, eperm ? EPERM : errno);
        unlink("/data/fs2-x");
        unlink("/tmp/fs2-t");
        if (xdev && eperm) bits |= 16;
    }

    /* ---- bit5: linkat against directory fds ---- */
    {
        int ok = 0;
        int dfd = open("/data", O_RDONLY | O_DIRECTORY);
        int fd = openat(dfd, "fs2-la", O_CREAT | O_RDWR, 0644);
        if (dfd >= 0 && fd >= 0 && write(fd, "at", 2) == 2) {
            if (linkat(dfd, "fs2-la", dfd, "fs2-lb", 0) == 0) {
                char b[4] = {0};
                int bfd = openat(dfd, "fs2-lb", O_RDONLY);
                ok = (bfd >= 0 && read(bfd, b, 2) == 2 &&
                      memcmp(b, "at", 2) == 0);
                if (bfd >= 0) close(bfd);
            }
        }
        if (fd >= 0) close(fd);
        unlink("/data/fs2-la");
        unlink("/data/fs2-lb");
        if (dfd >= 0) close(dfd);
        printf("fs2: linkat=%d\n", ok);
        if (ok) bits |= 32;
    }

    printf("LXFS2: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
