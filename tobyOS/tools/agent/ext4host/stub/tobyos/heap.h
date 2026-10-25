/* host stub: the kernel heap is the C heap */
#include <stdlib.h>
static inline void *kmalloc(size_t n) { return malloc(n); }
static inline void *kcalloc(size_t n, size_t sz) { return calloc(n, sz); }
static inline void kfree(void *p) { free(p); }
