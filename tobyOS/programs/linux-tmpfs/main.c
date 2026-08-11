/* linux-tmpfs -- the writable in-memory filesystem.
 *
 * ===========================================================================
 * WHAT MAKES THIS TEST MEAN SOMETHING
 *
 * `mount -t tmpfs` returning 0 proves nothing at all: this kernel answered
 * ENODEV for tmpfs on purpose for a long time, precisely because a mount that
 * succeeds and then behaves like something else is the failure mode this arc
 * keeps finding (VFS_MNT_RDONLY stored and unenforced, PR_SET_NO_NEW_PRIVS
 * accepted and ignored, io.max absent rather than faked).
 *
 * So every bit below asserts a VALUE or a REFUSAL:
 *
 * bit0 mount succeeds AND the mount point is a directory that lists empty --
 *      an empty listing is the two-sided half: if tmpfs were shadowing the
 *      underlying directory rather than covering it, the old contents would
 *      still be there.
 * bit1 a file written to it READS BACK BYTE FOR BYTE. Not "write returned n".
 * bit2 the data really lives in the tmpfs and not in whatever was underneath:
 *      the same path must be ABSENT after umount, and present again on a
 *      fresh mount only as an empty directory.
 * bit3 directories, nesting and readdir: mkdir a/b, create a file in it, and
 *      require readdir to return exactly the names created -- a flat table
 *      keyed by path (which is what this driver is) gets child-listing wrong
 *      in obvious ways if tchild_of is off by one.
 * bit4 unlink and rmdir actually remove, and a non-empty directory REFUSES to
 *      be removed rather than silently orphaning its contents.
 * bit5 THE SIZE CAP BITES. Write past `size=` and require ENOSPC. An in-memory
 *      filesystem with no limit lets any process take the machine down, so an
 *      unenforced cap is not a missing feature, it is a hazard. This is the
 *      bit that carries the test.
 * bit6 the cap is the one that was ASKED FOR: mounting with size=64k must fail
 *      near 64 KiB, not at the 16 MiB default. Reading a number back from a
 *      config file proves the file remembers it; this proves the mount honours
 *      it.
 * bit7 rename works, including moving a directory with a child in it -- the
 *      case a path-keyed table gets wrong by leaving the child stranded.
 *
 * exit status is the bitmask; 255 = all eight.
 * ===========================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>

#define MP  "/data/tfs"
#define MP2 "/data/tfs2"

static int g_bits;
__attribute__((format(printf, 2, 3)))
static void ok(int bit, const char *fmt, ...)
{
    va_list ap; g_bits |= bit;
    printf("tmpfs:   ok   ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}
__attribute__((format(printf, 1, 2)))
static void no(const char *fmt, ...)
{
    va_list ap;
    printf("tmpfs:   FAIL ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static int count_entries(const char *dir, char *first, size_t cap)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == 0 && first && cap) {
            size_t l = strlen(e->d_name);
            if (l >= cap) l = cap - 1;
            memcpy(first, e->d_name, l); first[l] = 0;
        }
        n++;
    }
    closedir(d);
    return n;
}

static int write_file(const char *path, const void *buf, size_t n, int *err)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { *err = errno; return -1; }
    ssize_t w = write(fd, buf, n);
    *err = errno;
    close(fd);
    return (w == (ssize_t)n) ? 0 : -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("tmpfs: start uid=%u\n", (unsigned)getuid());

    mkdir(MP, 0755);
    /* Leave a marker UNDERNEATH the mount point. bit2 uses it to prove the
     * mount covered the directory rather than merging with it. */
    { int e; (void)write_file(MP "/underneath", "x", 1, &e); }

    if (mount("tmpfs", MP, "tmpfs", 0, NULL) != 0) {
        no("mount(tmpfs, " MP ") errno=%d", errno);
        printf("tmpfs: BITS=0x%x (want 0xff)\n", g_bits);
        return g_bits;
    }
    {
        struct stat st;
        int n = count_entries(MP, NULL, 0);
        if (stat(MP, &st) != 0 || !S_ISDIR(st.st_mode))
            no("mount point is not a directory after mounting");
        else if (n != 0)
            no("a fresh tmpfs lists %d entries, want 0 -- it did not COVER "
               "the directory underneath", n);
        else
            ok(1, "mounted, and a fresh tmpfs is empty (it covers what was "
                  "underneath rather than merging with it)");
    }

    /* ---- bit1: bytes round-trip ---------------------------------------- */
    {
        static char pat[4096];
        for (size_t i = 0; i < sizeof pat; i++) pat[i] = (char)(i * 31 + 7);
        int e = 0;
        if (write_file(MP "/f", pat, sizeof pat, &e) != 0) {
            no("write to tmpfs failed errno=%d", e);
        } else {
            static char back[4096];
            int fd = open(MP "/f", O_RDONLY);
            ssize_t r = (fd >= 0) ? read(fd, back, sizeof back) : -1;
            if (fd >= 0) close(fd);
            if (r != (ssize_t)sizeof pat)
                no("read back %ld bytes, wrote %lu", (long)r,
                   (unsigned long)sizeof pat);
            else if (memcmp(pat, back, sizeof pat) != 0)
                no("read back the right LENGTH but the wrong BYTES");
            else
                ok(2, "%lu bytes round-trip byte for byte",
                   (unsigned long)sizeof pat);
        }
    }

    /* ---- bit3: directories, nesting, readdir --------------------------- */
    {
        int good = 1;
        if (mkdir(MP "/d", 0755) != 0) { no("mkdir errno=%d", errno); good = 0; }
        if (good && mkdir(MP "/d/e", 0755) != 0) {
            no("nested mkdir errno=%d", errno); good = 0;
        }
        int e = 0;
        if (good && write_file(MP "/d/e/deep", "hi", 2, &e) != 0) {
            no("write in a nested dir errno=%d", e); good = 0;
        }
        char first[64] = {0};
        if (good) {
            int n = count_entries(MP "/d/e", first, sizeof first);
            if (n != 1 || strcmp(first, "deep") != 0) {
                no("readdir(%s) returned %d entries, first='%s' -- want 1 "
                   "and 'deep' (a path-keyed table gets child listing wrong "
                   "here if the prefix match is off)", MP "/d/e", n, first);
                good = 0;
            }
        }
        if (good) {
            /* And the PARENT must list only 'e', not 'e/deep'. */
            int n = count_entries(MP "/d", first, sizeof first);
            if (n != 1 || strcmp(first, "e") != 0) {
                no("readdir(%s) returned %d entries, first='%s' -- want 1 and "
                   "'e'; a grandchild must not appear as a direct child",
                   MP "/d", n, first);
                good = 0;
            }
        }
        if (good) ok(8, "mkdir/nesting/readdir: children list, grandchildren "
                        "do not");
    }

    /* ---- bit4: removal, and the refusal ------------------------------- */
    {
        int good = 1;
        if (rmdir(MP "/d") == 0) {
            no("rmdir removed a NON-EMPTY directory -- its contents would be "
               "unreachable and still charged");
            good = 0;
        }
        if (good && unlink(MP "/d/e/deep") != 0) {
            no("unlink errno=%d", errno); good = 0;
        }
        if (good && access(MP "/d/e/deep", F_OK) == 0) {
            no("unlink returned 0 but the file is still there"); good = 0;
        }
        if (good && rmdir(MP "/d/e") != 0) {
            no("rmdir of an emptied directory errno=%d", errno); good = 0;
        }
        if (good) ok(16, "unlink/rmdir remove for real, and a non-empty "
                         "directory is refused");
    }

    /* ---- bit7: rename, including a directory with a child -------------- */
    {
        int good = 1, e = 0;
        mkdir(MP "/r", 0755);
        if (write_file(MP "/r/inside", "abc", 3, &e) != 0) {
            no("rename setup write errno=%d", e); good = 0;
        }
        if (good && rename(MP "/r", MP "/r2") != 0) {
            no("rename(dir) errno=%d", errno); good = 0;
        }
        if (good) {
            int fd = open(MP "/r2/inside", O_RDONLY);
            char b[8] = {0};
            ssize_t n = (fd >= 0) ? read(fd, b, sizeof b) : -1;
            if (fd >= 0) close(fd);
            if (n != 3 || memcmp(b, "abc", 3) != 0) {
                no("after renaming a directory its child is unreachable "
                   "(read %ld) -- the subtree did not move with it", (long)n);
                good = 0;
            }
        }
        if (good) ok(128, "rename moves a directory AND its children");
    }

    /* ---- bit5: the size cap bites -------------------------------------- */
    {
        static char blk[65536];
        memset(blk, 'A', sizeof blk);
        int fd = open(MP "/big", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            no("cap: could not create the probe file errno=%d", errno);
        } else {
            long total = 0; int hit = 0, e = 0;
            for (int i = 0; i < 4096; i++) {          /* up to 256 MiB */
                ssize_t w = write(fd, blk, sizeof blk);
                if (w < 0) { e = errno; hit = 1; break; }
                total += w;
                if (w < (ssize_t)sizeof blk) { e = ENOSPC; hit = 1; break; }
            }
            close(fd);
            if (!hit)
                no("cap: wrote %ld bytes without ever hitting the limit -- an "
                   "unbounded tmpfs can take the machine down", total);
            else if (e != ENOSPC)
                no("cap: writing stopped with errno=%d after %ld bytes, want "
                   "ENOSPC (%d)", e, total, ENOSPC);
            else
                ok(32, "size cap ENFORCED: ENOSPC after %ld bytes on the "
                       "default mount", total);
            unlink(MP "/big");
        }
    }

    /* ---- bit6: the cap is the one that was asked for -------------------- */
    {
        mkdir(MP2, 0755);
        if (mount("tmpfs", MP2, "tmpfs", 0, "size=64k") != 0) {
            no("mount with size=64k errno=%d", errno);
        } else {
            static char blk[4096];
            memset(blk, 'B', sizeof blk);
            int fd = open(MP2 "/x", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            long total = 0; int hit = 0;
            if (fd >= 0) {
                for (int i = 0; i < 4096; i++) {
                    ssize_t w = write(fd, blk, sizeof blk);
                    if (w < 0) { hit = (errno == ENOSPC); break; }
                    total += w;
                }
                close(fd);
            }
            if (!hit)
                no("size=64k: wrote %ld bytes without ENOSPC -- the option was "
                   "accepted and ignored", total);
            else if (total > 256 * 1024)
                no("size=64k: took %ld bytes to hit the limit, so the mount "
                   "used the DEFAULT size rather than the requested one",
                   total);
            else
                ok(64, "size=64k honoured: ENOSPC after %ld bytes, nowhere "
                       "near the 16 MiB default", total);
            umount(MP2);
        }
    }

    /* ---- bit2: it really was the tmpfs holding the data ----------------- */
    {
        if (umount(MP) != 0) {
            no("umount errno=%d", errno);
        } else if (access(MP "/f", F_OK) == 0) {
            no("after umount the tmpfs file is STILL visible -- the data was "
               "never in the tmpfs");
        } else if (access(MP "/underneath", F_OK) != 0) {
            no("after umount the marker that was underneath is gone -- the "
               "mount did not cover the directory, it replaced it");
        } else {
            ok(4, "umount reveals what was underneath and takes the tmpfs "
                  "contents with it");
        }
    }

    printf("tmpfs: BITS=0x%x (want 0xff)\n", g_bits);
    return g_bits;
}
