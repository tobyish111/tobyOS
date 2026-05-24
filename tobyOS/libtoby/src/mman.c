/* libtoby/src/mman.c -- memory mapping (mmap/munmap/mprotect/msync).
 *
 * Delegates to the kernel's SYS_MMAP/SYS_MUNMAP. For MAP_ANONYMOUS
 * with no kernel mmap support, falls back to sbrk-based allocation. */

#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "libtoby_internal.h"

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset) {
    if (length == 0) { errno = EINVAL; return MAP_FAILED; }

#ifdef ABI_SYS_MMAP
    long rv = toby_sc6(ABI_SYS_MMAP,
                       (long)(uintptr_t)addr,
                       (long)length, prot, flags, fd, offset);
    if (rv != -ENOSYS) {
        if (rv < 0) { errno = (int)(-rv); return MAP_FAILED; }
        return (void *)(uintptr_t)rv;
    }
#endif

    /* Fallback: for MAP_ANONYMOUS, emulate with sbrk. */
    if (flags & MAP_ANONYMOUS) {
        (void)addr; (void)fd; (void)offset;
        size_t aligned = (length + 4095) & ~(size_t)4095;
        void *p = sbrk((long)aligned);
        if (p == (void *)-1) { errno = ENOMEM; return MAP_FAILED; }
        memset(p, 0, aligned);
        (void)prot;
        return p;
    }

    errno = ENOSYS;
    return MAP_FAILED;
}

int munmap(void *addr, size_t length) {
    if (!addr || length == 0) { errno = EINVAL; return -1; }

#ifdef ABI_SYS_MUNMAP
    long rv = toby_sc2(ABI_SYS_MUNMAP,
                       (long)(uintptr_t)addr, (long)length);
    if (rv != -ENOSYS) {
        if (rv < 0) { errno = (int)(-rv); return -1; }
        return 0;
    }
#endif

    /* Fallback: sbrk-allocated memory can't be individually freed.
     * Just succeed silently. */
    (void)addr; (void)length;
    return 0;
}

int mprotect(void *addr, size_t length, int prot) {
    if (!addr || length == 0) { errno = EINVAL; return -1; }

#ifdef ABI_SYS_MPROTECT
    long rv = toby_sc3(ABI_SYS_MPROTECT,
                       (long)(uintptr_t)addr, (long)length, prot);
    if (rv != -ENOSYS) {
        if (rv < 0) { errno = (int)(-rv); return -1; }
        return 0;
    }
#endif

    (void)prot;
    return 0;
}

int msync(void *addr, size_t length, int flags) {
    (void)addr; (void)length; (void)flags;
    return 0;
}
