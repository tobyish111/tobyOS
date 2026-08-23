/* linux-stat -- statfs tells the truth (Phase H, 2026-08-22).
 *
 * Every mount used to claim the same fabricated 4 GiB tmpfs. Now:
 *
 *   bit0  /data (tobyfs): its own magic, sane counts -- and writing a
 *         64 KiB file makes bfree DROP (the numbers are LIVE, not a
 *         snapshot someone fabricated once)
 *   bit1  /tmp (tmpfs): TMPFS_MAGIC, the mount's real 16 MiB budget
 *   bit2  / (initrd ramfs): RAMFS_MAGIC, image-sized, populated
 *   bit3  fstatfs(fd) agrees with statfs(path) for the same mount
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/vfs.h>

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: tobyfs numbers are real and MOVE ---- */
    {
        struct statfs a, b;
        int r1 = statfs("/data", &a);
        int fd = open("/data/stat-probe", O_CREAT | O_RDWR | O_TRUNC, 0644);
        static char blob[65536];
        memset(blob, 7, sizeof blob);
        (void)!write(fd, blob, sizeof blob);
        if (fd >= 0) close(fd);
        int r2 = statfs("/data", &b);
        unlink("/data/stat-probe");
        printf("stat: data magic=0x%lx blocks=%ld bfree %ld->%ld\n",
               (long)a.f_type, (long)a.f_blocks,
               (long)a.f_bfree, (long)b.f_bfree);
        if (r1 == 0 && r2 == 0 && a.f_type == 0x746f6279 &&
            a.f_blocks > 0 && a.f_bfree > 0 && a.f_bfree <= a.f_blocks &&
            b.f_bfree < a.f_bfree)
            bits |= 1;
    }

    /* ---- bit1: tmpfs budget ---- */
    {
        struct statfs s;
        int r = statfs("/tmp", &s);
        printf("stat: tmp magic=0x%lx blocks=%ld bfree=%ld ffree=%ld\n",
               (long)s.f_type, (long)s.f_blocks,
               (long)s.f_bfree, (long)s.f_ffree);
        if (r == 0 && s.f_type == 0x01021994 && s.f_blocks > 0 &&
            s.f_blocks <= (1 << 16) && s.f_bfree <= s.f_blocks)
            bits |= 2;
    }

    /* ---- bit2: ramfs root ---- */
    {
        struct statfs s;
        int r = statfs("/", &s);
        printf("stat: root magic=0x%lx blocks=%ld files=%ld\n",
               (long)s.f_type, (long)s.f_blocks, (long)s.f_files);
        if (r == 0 && s.f_type == (long)0x858458f6 && s.f_blocks > 0 &&
            s.f_files > 0)
            bits |= 4;
    }

    /* ---- bit3: fstatfs matches statfs ---- */
    {
        int fd = open("/data/stat-fd", O_CREAT | O_RDWR, 0644);
        struct statfs p, f;
        int r1 = statfs("/data", &p);
        int r2 = (fd >= 0) ? fstatfs(fd, &f) : -1;
        if (fd >= 0) close(fd);
        unlink("/data/stat-fd");
        printf("stat: fstatfs magic=0x%lx (path 0x%lx)\n",
               (long)(r2 == 0 ? f.f_type : -1), (long)p.f_type);
        if (r1 == 0 && r2 == 0 && p.f_type == f.f_type &&
            p.f_blocks == f.f_blocks)
            bits |= 8;
    }

    printf("LXSTAT: VERDICT bits=%d (15=all)\n", bits);
    return bits;
}
