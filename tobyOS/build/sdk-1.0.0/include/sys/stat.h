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

struct stat {
    off_t  st_size;
    mode_t st_mode;
    uid_t  st_uid;
    gid_t  st_gid;
};

int stat (const char *path, struct stat *out);
int fstat(int fd,           struct stat *out);
int mkdir(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_SYS_STAT_H */
