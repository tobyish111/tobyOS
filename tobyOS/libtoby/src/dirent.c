/* libtoby/src/dirent.c -- POSIX-shape directory enumeration.
 *
 * Strategy: opendir() reads the WHOLE listing (SYS_FS_READDIR, a window at
 * a time) into a buffer the DIR owns; readdir() serves entries from it;
 * rewinddir() reads it again; closedir() frees it.
 *
 * It used to read one 16-entry window and ask for the next at the next
 * OFFSET when that ran out. The kernel's offset is an entry INDEX, so a
 * caller that removes what it reads -- `rm -r`, the commonest one -- had
 * the directory shift under it: after it unlinked entries 0..15 the next
 * window began at index 16 of what was left, sixteen entries were never
 * seen, and the final rmdir found the directory not empty ("rm: cannot
 * remove dir"; findings 56, logs/b14 r2). A listing taken at opendir is what
 * POSIX allows -- whether readdir reports entries added or removed after
 * opendir is unspecified -- and what a remover needs.
 *
 * Caveats:
 *   - We do not deduplicate or sort. Order is whatever the kernel
 *     VFS produces.
 *   - The dirent returned by readdir() is invalidated by the next
 *     readdir() / closedir(). This matches POSIX. */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "libtoby_internal.h"

#define DIR_WINDOW  64

struct __libtoby_DIR {
    char                  path[ABI_PATH_MAX];
    int                   have;              /* entries in the listing */
    int                   pos;               /* next entry to serve */
    int                   cap;
    struct dirent         view;              /* most recently returned */
    struct abi_dirent2   *ents;              /* the listing */
};

/* The whole listing of d->path into d->ents. 0, or -1 with errno. */
static int load(DIR *d) {
    d->have = 0;
    d->pos  = 0;
    for (;;) {
        if (d->cap - d->have < DIR_WINDOW) {
            int ncap = d->cap ? d->cap * 2 : DIR_WINDOW;
            struct abi_dirent2 *ne = (struct abi_dirent2 *)realloc(d->ents,
                                         (size_t)ncap * sizeof *ne);
            if (!ne) { errno = ENOMEM; return -1; }
            d->ents = ne;
            d->cap  = ncap;
        }
        /* READDIR2 (batch 31, finding 187): ".", "..", inodes, types and
         * names whole, as Linux's readdir gives them */
        long r = toby_sc4(ABI_SYS_FS_READDIR2,
                          (long)(uintptr_t)d->path,
                          (long)(uintptr_t)(d->ents + d->have),
                          (long)DIR_WINDOW,
                          (long)d->have);
        if (r < 0) {
            /* SYS_FS_READDIR says why now (EACCES, ENOTDIR, ENOENT); it
             * said -1 for everything, and this called that ENOENT */
            errno = (int)-r;
            return -1;
        }
        d->have += (int)r;
        if (r < DIR_WINDOW) return 0;
    }
}

DIR *opendir(const char *path) {
    if (!path) { errno = EFAULT; return 0; }
    size_t plen = strnlen(path, ABI_PATH_MAX);
    if (plen >= ABI_PATH_MAX) { errno = ENAMETOOLONG; return 0; }

    DIR *d = (DIR *)malloc(sizeof(*d));
    if (!d) { errno = ENOMEM; return 0; }
    memcpy(d->path, path, plen);
    d->path[plen] = '\0';
    d->have = d->pos = d->cap = 0;
    d->ents = 0;

    /* Reading it now also catches non-existent or non-directory paths at
     * opendir() time, matching POSIX (which only lets readdir() report
     * EBADF). */
    if (load(d) != 0) {
        int saved = errno;
        free(d->ents);
        free(d);
        errno = saved;
        return 0;
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return 0; }
    if (d->pos >= d->have) return 0;               /* end-of-stream */

    struct abi_dirent2 *src = &d->ents[d->pos++];
    d->view.d_ino  = (ino_t)src->ino;              /* what lstat would say */
    d->view.d_type = (unsigned char)src->type;     /* Linux's DT_ values */
    size_t i = 0;
    for (; i < NAME_MAX && src->name[i]; i++) d->view.d_name[i] = src->name[i];
    d->view.d_name[i] = '\0';
    return &d->view;
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    free(d->ents);
    free(d);
    return 0;
}

void rewinddir(DIR *d) {
    if (!d) return;
    (void)load(d);
}
