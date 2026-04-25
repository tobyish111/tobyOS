/* libtoby/src/dirent.c -- POSIX-shape directory enumeration.
 *
 * Strategy: each opendir() allocates a DIR plus a buffer of
 * vfs_dirent_user[] records. readdir() serves entries from that
 * buffer; when exhausted it re-issues SYS_FS_READDIR with the
 * current offset to refill. closedir() frees the DIR.
 *
 * The buffer window is intentionally small (16 entries) so DIR
 * stays cheap to allocate in restricted heaps (init/initrd-only
 * workflows). 16 also makes the typical /bin enumeration finish
 * in 2-3 syscalls.
 *
 * Caveats:
 *   - We do not deduplicate or sort. Order is whatever the kernel
 *     VFS produces.
 *   - rewinddir() resets the offset and forces a refill.
 *   - The dirent returned by readdir() points into the DIR's buffer
 *     and is invalidated by the next readdir() / closedir(). This
 *     matches POSIX. */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "libtoby_internal.h"

#define DIR_WINDOW  16

struct __libtoby_DIR {
    char                  path[ABI_PATH_MAX];
    int                   next_kernel_off;   /* abs offset to ask kernel */
    int                   have;              /* entries currently buffered */
    int                   pos;               /* next entry to serve */
    struct dirent         view;              /* most recently returned */
    struct abi_dirent     buf[DIR_WINDOW];
};

static int refill(DIR *d) {
    long r = toby_sc4(ABI_SYS_FS_READDIR,
                      (long)(uintptr_t)d->path,
                      (long)(uintptr_t)d->buf,
                      (long)DIR_WINDOW,
                      (long)d->next_kernel_off);
    if (r < 0) {
        /* SYS_FS_READDIR returns -1 on any error (legacy). Best
         * guess at errno -- we can't tell apart NOENT vs IO from
         * the kernel today. */
        errno = ENOENT;
        return -1;
    }
    d->have = (int)r;
    d->pos  = 0;
    d->next_kernel_off += d->have;
    return 0;
}

DIR *opendir(const char *path) {
    if (!path) { errno = EFAULT; return 0; }
    size_t plen = strnlen(path, ABI_PATH_MAX);
    if (plen >= ABI_PATH_MAX) { errno = ENAMETOOLONG; return 0; }

    DIR *d = (DIR *)malloc(sizeof(*d));
    if (!d) { errno = ENOMEM; return 0; }
    memcpy(d->path, path, plen);
    d->path[plen]       = '\0';
    d->next_kernel_off  = 0;
    d->have             = 0;
    d->pos              = 0;

    /* Eagerly load the first window. This catches non-existent or
     * non-directory paths at opendir() time, matching POSIX
     * (which only lets readdir() report EBADF). */
    if (refill(d) != 0) {
        int saved = errno;
        free(d);
        errno = saved;
        return 0;
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return 0; }

    if (d->pos >= d->have) {
        if (d->have < DIR_WINDOW) return 0;       /* end-of-stream */
        if (refill(d) != 0)       return 0;
        if (d->have == 0)         return 0;
    }

    struct abi_dirent *src = &d->buf[d->pos++];
    d->view.d_ino  = 0;                            /* tobyfs has no inode # */
    d->view.d_type = (src->type == ABI_DT_DIR) ? DT_DIR : DT_REG;
    size_t i = 0;
    for (; i < NAME_MAX && src->name[i]; i++) d->view.d_name[i] = src->name[i];
    d->view.d_name[i] = '\0';
    return &d->view;
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    free(d);
    return 0;
}

void rewinddir(DIR *d) {
    if (!d) return;
    d->next_kernel_off = 0;
    d->have            = 0;
    d->pos             = 0;
    (void)refill(d);
}
