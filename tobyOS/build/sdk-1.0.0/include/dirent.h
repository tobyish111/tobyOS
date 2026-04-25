/* dirent.h -- libtoby's directory enumeration.
 *
 * The kernel's SYS_FS_READDIR is batch-oriented: pass a
 * struct vfs_dirent_user[] of known capacity plus an `offset`
 * to skip-to, and it fills as many entries as fit. We hide that
 * shape behind classic POSIX opendir/readdir/closedir by buffering
 * the directory into a private DIR struct and serving entries from
 * that buffer.
 *
 * Expected use is identical to glibc/musl. The caller never owns or
 * frees the returned struct dirent; closedir() reclaims everything.
 *
 * Limits:
 *   - NAME_MAX is 63 (kernel's SYS_FS_NAME_MAX is 64 incl. NUL).
 *   - Re-entrancy: DIR has internal state; not thread-safe (we
 *     have no threads anyway).
 *   - readdir() refills its window when exhausted, so directories
 *     larger than the window are streamed transparently. */

#ifndef LIBTOBY_DIRENT_H
#define LIBTOBY_DIRENT_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAME_MAX  63

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

struct dirent {
    ino_t          d_ino;
    unsigned char  d_type;
    char           d_name[NAME_MAX + 1];
};

typedef struct __libtoby_DIR DIR;

DIR           *opendir (const char *path);
struct dirent *readdir (DIR *d);
int            closedir(DIR *d);
void           rewinddir(DIR *d);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_DIRENT_H */
