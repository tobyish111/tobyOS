/* Freestanding <stdlib.h> shim for the vendored brotli decoder. brotli
 * only uses malloc/free in its default allocator (BrotliDefaultAllocFunc);
 * src/brotli.c passes custom kmalloc/kfree allocators, so these are just
 * to satisfy the includes. Map onto the kernel heap. Brotli-include-path
 * only (see the src/brotli.o make rule). */
#ifndef TOBYOS_BROTLI_SHIM_STDLIB_H
#define TOBYOS_BROTLI_SHIM_STDLIB_H
#include <tobyos/types.h>
void *kmalloc(size_t n);
void  kfree(void *p);
static inline void *malloc(size_t n) { return kmalloc(n); }
static inline void  free(void *p)    { kfree(p); }
#endif
