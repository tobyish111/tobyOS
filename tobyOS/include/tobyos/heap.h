/* heap.h -- kernel heap (kmalloc / kfree).
 *
 * Backed by single-page PMM allocations stitched into virtually
 * contiguous arenas via vmm_map(). The heap lives in its own kernel
 * virtual region (see KHEAP_VIRT_BASE / heap_virt_base()), so kmalloc
 * pointers are easy to distinguish from raw HHDM accesses in dumps.
 * All returned pointers are 16-byte aligned -- safe for any standard
 * scalar / SSE value.
 */

#ifndef TOBYOS_HEAP_H
#define TOBYOS_HEAP_H

#include <tobyos/types.h>

/* Initialise the heap. Must be called after pmm_init(). Pre-allocates
 * one arena so the first kmalloc is fast. */
void heap_init(void);

/* Allocate `n` bytes (rounded up to 16). Returns 0 on OOM. The result
 * is 16-byte aligned and uninitialised. */
void *kmalloc(size_t n);

/* Allocate and zero-fill `count * size` bytes. */
void *kcalloc(size_t count, size_t size);

/* Free a pointer previously returned by kmalloc/kcalloc. NULL is a
 * no-op. Detects double-free and out-of-bounds via an inline sentinel
 * stamped at allocation time. */
void kfree(void *p);

/* Snapshot of allocator state. Bytes count payload + per-block headers,
 * not arena overhead. */
struct heap_stats {
    size_t arenas;
    size_t total_bytes;       /* size of all arenas (excl. arena headers) */
    size_t used_bytes;        /* bytes currently allocated (incl. block hdrs) */
    size_t free_bytes;        /* total - used */
    size_t alloc_count;       /* lifetime kmalloc calls */
    size_t free_count;        /* lifetime kfree calls */
};

void heap_stats(struct heap_stats *out);
void heap_dump(void);  /* prints arenas + per-block layout (verbose) */

/* Range the heap allocates from. Useful for asserting that a kmalloc
 * pointer really came from the heap (and not, say, an HHDM address). */
uint64_t heap_virt_base(void);
uint64_t heap_virt_brk(void);    /* next free byte after the last arena */
uint64_t heap_virt_end(void);    /* one-past-the-end of the heap window */

#endif /* TOBYOS_HEAP_H */
