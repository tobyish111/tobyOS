/* host stub: the kernel heap is the C heap */
#include <stdlib.h>
static inline void *kmalloc(size_t n) { return malloc(n); }
static inline void kfree(void *p) { free(p); }
