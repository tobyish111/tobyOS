/* sys/stat.h -- libtoby's stat / fstat / mkdir surface.
 *
 * The struct stat layout is libtoby's own -- it is NOT byte-equivalent
 * to the kernel's struct abi_stat. The translation happens inside
 * stat()/fstat() implementations (libtoby/src/unistd.c). This decouples
 * userland from kernel struct evolution; the kernel can grow new
 * trailing fields in abi_stat without forcing a libc soname bump. */

#ifndef LIBTOBY_SYS_STAT_H
#define LIBTOBY_SYS_STAT_H

#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S_IFMT      0xF000
#define S_IFREG     0x8000
#define S_IFDIR     0x4000
/* The other POSIX file types. The kernel reports S_IFCHR for the /dev nodes
 * (and passes st_mode through unchanged -- see unistd.c), but with only the
 * two macros below defined, every native tool that asked "what is this?"
 * got neither answer: `ls -l /dev/null` printed `?rw-rw-rw-` and `stat`
 * said "unknown". Values are the POSIX/Linux ones. */
#define S_IFIFO     0x1000
#define S_IFCHR     0x2000
#define S_IFBLK     0x6000
#define S_IFLNK     0xA000
#define S_IFSOCK    0xC000

#define S_IRUSR     0400
#define S_IWUSR     0200
#define S_IXUSR     0100
#define S_IRGRP     0040
#define S_IWGRP     0020
#define S_IXGRP     0010
#define S_IROTH     0004
#define S_IWOTH     0002
#define S_IXOTH     0001

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

struct stat {
    off_t  st_size;
    mode_t st_mode;
    uid_t  st_uid;
    gid_t  st_gid;
    /* 2026-08-24: timestamps, in UNIX EPOCH SECONDS. The kernel had them
     * in struct vfs_stat all along and struct abi_stat had nowhere to put
     * them, so no native program could read a file's date -- which is why
     * `touch -m` had no existing atime to preserve. 0 means the filesystem
     * has no answer (fstat on a pipe, say), not 1970. */
    time_t st_mtime;
    time_t st_atime;
    /* 2026-09-08. st_nlink was absent, so `ls -l` printed a synthetic
     * count; st_ino/st_dev were absent, so `test -ef` compared paths and
     * answered "different" for two hard links to one file. 0 for ino/dev
     * means the filesystem has no answer -- compare paths instead. */
    nlink_t st_nlink;
    ino_t   st_ino;
    dev_t   st_dev;
};

int stat (const char *path, struct stat *out);
int fstat(int fd,           struct stat *out);
int lstat(const char *path, struct stat *out);   /* the link itself */
int mkdir(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_SYS_STAT_H */
