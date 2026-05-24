/* sys/mman.h -- libtoby's memory mapping interface. */

#ifndef LIBTOBY_SYS_MMAN_H
#define LIBTOBY_SYS_MMAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x04
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FIXED     0x08

#define MAP_FAILED  ((void *)-1)

#define MS_ASYNC      1
#define MS_SYNC       2
#define MS_INVALIDATE 4

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);
int   msync(void *addr, size_t length, int flags);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_SYS_MMAN_H */
