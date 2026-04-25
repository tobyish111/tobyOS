/* heap.c -- per-arena implicit free-list allocator over paged virt memory.
 *
 * Each arena is N pages, *virtually* contiguous in the dedicated kernel
 * heap region (KHEAP_VIRT_BASE..KHEAP_VIRT_END), but its backing pages
 * come from individual pmm_alloc_page() calls and are stitched together
 * with vmm_map(). This decouples heap growth from physical
 * fragmentation: as long as enough free frames exist *anywhere* in
 * RAM, a heap arena of any reasonable size can be created.
 *
 * Layout inside an arena (unchanged from milestone 2):
 *
 *   [ arena_t ] [ block_hdr | payload ] [ block_hdr | payload ] ...
 *
 * Every block stores its total size (header + payload) in `size`. The
 * low bit of `size` carries the "in use" flag -- safe because sizes are
 * always rounded up to KHEAP_ALIGN (16), so the bottom 4 bits of size
 * are otherwise unused.
 *
 * Iterating an arena is just `next = (uint8_t*)blk + (blk->size & ~1)`.
 * No explicit prev/next links are needed -- this is the classic Knuth
 * implicit free-list.
 *
 * Allocation: walk all arenas, first-fit. If splitting would leave at
 * least KHEAP_MIN_SPLIT bytes free, split off the tail.
 *
 * Free: clear the use flag, then sweep the owning arena once to
 * coalesce all adjacent free blocks. O(arena), but arenas are bounded
 * and writes are dense, so it's fine until we feel the pain.
 *
 * Growth: bump the heap brk forward by N pages, allocate each frame
 * separately from the PMM, and vmm_map it into the new virt range with
 * RW + NX. On any failure we roll back what we already mapped + freed.
 * Arenas are never released back to the brk in this milestone.
 */

#include <tobyos/heap.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>

#define KHEAP_ALIGN       16u
#define KHEAP_MIN_SPLIT   32u            /* don't split if remainder < this */
#define KHEAP_GROW_PAGES  16u            /* grow in 64 KiB chunks at minimum */
#define KHEAP_MAGIC       0xC0DEFEEDu

#define INUSE_BIT         1u
#define SIZE_MASK         (~(size_t)(KHEAP_ALIGN - 1))

/* Dedicated kernel heap virtual window. Sits well clear of HHDM
 * (0xffff_8000_..) and the kernel image (0xffff_ffff_8...). 1 TiB of
 * virt is far more than we'd ever consume, but bookkeeping is just a
 * single bump cursor so the cost is zero. */
#define KHEAP_VIRT_BASE   0xFFFFD00000000000ULL
#define KHEAP_VIRT_END    0xFFFFD10000000000ULL

static uint64_t g_kheap_brk = KHEAP_VIRT_BASE;

typedef struct arena {
    struct arena *next;
    size_t        pages;        /* page count for pmm_free_pages_range */
    size_t        total_size;   /* bytes after the arena header */
    uint32_t      magic;
    uint32_t      _pad;
} arena_t;

typedef struct block_hdr {
    size_t   size;              /* header + payload, with INUSE_BIT in bit 0 */
    uint32_t magic;             /* KHEAP_MAGIC, validated on free */
    uint32_t _pad;              /* keeps header 16 bytes -> payload is 16-aligned */
} block_hdr_t;

_Static_assert(sizeof(block_hdr_t) == 16, "block_hdr_t must be 16 bytes");

static arena_t *g_arenas;
static size_t   g_alloc_count;
static size_t   g_free_count;
static size_t   g_used_bytes;
static size_t   g_total_bytes;
/* Milestone 22 step 5: protect the arena list + free-list walk
 * against concurrent kmalloc/kfree. As with the PMM lock, only the
 * BSP touches the heap in v1 (APs don't allocate from sched_idle),
 * but the cost is negligible and any future AP-side path that ends
 * up calling kmalloc would silently corrupt the free-list without
 * this. */
static spinlock_t g_heap_lock = SPINLOCK_INIT;

static inline size_t align_up(size_t x, size_t a) {
    return (x + a - 1) & ~(a - 1);
}

static inline block_hdr_t *first_block(arena_t *a) {
    return (block_hdr_t *)((uint8_t *)a + sizeof(arena_t));
}

static inline block_hdr_t *arena_end(arena_t *a) {
    return (block_hdr_t *)((uint8_t *)first_block(a) + a->total_size);
}

/* Undo the first `mapped` page mappings of a partially-grown arena
 * starting at `arena_virt`. Recovers the phys frame from the page
 * tables (vmm_translate), unmaps it, then returns it to the PMM. */
static void rollback_pages(uint64_t arena_virt, size_t mapped) {
    for (size_t j = 0; j < mapped; j++) {
        uint64_t v = arena_virt + (uint64_t)j * PAGE_SIZE;
        uint64_t p = vmm_translate(v);
        vmm_unmap(v, PAGE_SIZE);
        if (p) pmm_free_page(p);
    }
}

static arena_t *grow(size_t need_bytes) {
    /* Pages needed to fit one block of `need_bytes` plus the arena
     * header. Round up to KHEAP_GROW_PAGES so we don't churn one page
     * at a time. */
    size_t bytes = align_up(need_bytes + sizeof(arena_t), PAGE_SIZE);
    size_t pages = bytes / PAGE_SIZE;
    if (pages < KHEAP_GROW_PAGES) pages = KHEAP_GROW_PAGES;

    if (g_kheap_brk + (uint64_t)pages * PAGE_SIZE > KHEAP_VIRT_END) {
        kprintf("[heap] WARN: out of heap virtual address space "
                "(brk=%p need=%lu pages)\n",
                (void *)g_kheap_brk, (unsigned long)pages);
        return 0;
    }

    /* Map `pages` consecutive virt pages, each backed by a fresh PMM
     * frame. On any failure roll back what we already did and leave
     * g_kheap_brk untouched. */
    uint64_t arena_virt = g_kheap_brk;
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[heap] WARN: pmm_alloc_page() OOM at page %lu/%lu\n",
                    (unsigned long)i, (unsigned long)pages);
            rollback_pages(arena_virt, i);
            return 0;
        }
        if (!vmm_map(arena_virt + (uint64_t)i * PAGE_SIZE, phys, PAGE_SIZE,
                     VMM_WRITE | VMM_NX)) {
            kprintf("[heap] WARN: vmm_map failed at page %lu/%lu\n",
                    (unsigned long)i, (unsigned long)pages);
            pmm_free_page(phys);
            rollback_pages(arena_virt, i);
            return 0;
        }
    }
    g_kheap_brk += (uint64_t)pages * PAGE_SIZE;

    arena_t *a   = (arena_t *)arena_virt;
    a->next      = g_arenas;
    a->pages     = pages;
    a->total_size = pages * PAGE_SIZE - sizeof(arena_t);
    a->magic     = KHEAP_MAGIC;
    g_arenas     = a;

    block_hdr_t *b = first_block(a);
    b->size  = a->total_size;     /* free, INUSE_BIT clear */
    b->magic = KHEAP_MAGIC;

    g_total_bytes += a->total_size;
    return a;
}

void heap_init(void) {
    if (g_arenas != 0) return;
    if (grow(KHEAP_GROW_PAGES * PAGE_SIZE) == 0) {
        kpanic("heap_init: cannot allocate initial arena from PMM");
    }
    kprintf("[heap] up: initial arena %lu pages (%lu KiB)\n",
            (unsigned long)g_arenas->pages,
            (unsigned long)(g_arenas->total_size / 1024));
}

/* Try to allocate from a single arena. Returns NULL if no fit. */
static void *try_alloc_in(arena_t *a, size_t need) {
    block_hdr_t *b    = first_block(a);
    block_hdr_t *end  = arena_end(a);

    while (b < end) {
        if (b->magic != KHEAP_MAGIC) {
            kpanic("heap corruption: bad block magic at %p (got 0x%x)",
                   b, b->magic);
        }
        size_t bsz = b->size & SIZE_MASK;
        bool   used = b->size & INUSE_BIT;

        if (!used && bsz >= need) {
            size_t remain = bsz - need;
            if (remain >= KHEAP_MIN_SPLIT) {
                /* Split: shrink current, create a new free tail. */
                b->size = need | INUSE_BIT;
                block_hdr_t *t = (block_hdr_t *)((uint8_t *)b + need);
                t->size  = remain;        /* free */
                t->magic = KHEAP_MAGIC;
                g_used_bytes += need;
            } else {
                b->size = bsz | INUSE_BIT;
                g_used_bytes += bsz;
            }
            return (uint8_t *)b + sizeof(block_hdr_t);
        }
        b = (block_hdr_t *)((uint8_t *)b + bsz);
    }
    return 0;
}

void *kmalloc(size_t n) {
    if (n == 0) return 0;

    /* Total block size = header + payload, aligned. */
    size_t need = align_up(n + sizeof(block_hdr_t), KHEAP_ALIGN);

    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    for (arena_t *a = g_arenas; a; a = a->next) {
        void *p = try_alloc_in(a, need);
        if (p) {
            g_alloc_count++;
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return p;
        }
    }

    /* No fit -- grow and retry. grow() calls pmm_alloc_page + vmm_map
     * internally, which take their own locks; that's fine because
     * we never call those WHILE holding g_heap_lock from outside the
     * heap allocator -- the lock order is always heap -> pmm/vmm,
     * never the reverse. */
    arena_t *fresh = grow(need);
    if (!fresh) {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        return 0;
    }

    void *p = try_alloc_in(fresh, need);
    if (p) g_alloc_count++;
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return p;
}

void *kcalloc(size_t count, size_t size) {
    /* Naive overflow check (good enough for kernel-internal callers). */
    size_t total = count * size;
    if (size != 0 && total / size != count) return 0;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

/* Coalesce all adjacent free blocks in arena `a`. One linear sweep. */
static void coalesce_arena(arena_t *a) {
    block_hdr_t *b   = first_block(a);
    block_hdr_t *end = arena_end(a);

    while (b < end) {
        size_t bsz = b->size & SIZE_MASK;
        bool   used = b->size & INUSE_BIT;
        if (!used) {
            block_hdr_t *n = (block_hdr_t *)((uint8_t *)b + bsz);
            while (n < end && !(n->size & INUSE_BIT)) {
                bsz += (n->size & SIZE_MASK);
                n = (block_hdr_t *)((uint8_t *)b + bsz);
            }
            b->size = bsz;          /* still free */
        }
        b = (block_hdr_t *)((uint8_t *)b + bsz);
    }
}

void kfree(void *p) {
    if (!p) return;

    block_hdr_t *b = (block_hdr_t *)((uint8_t *)p - sizeof(block_hdr_t));
    if (b->magic != KHEAP_MAGIC) {
        kpanic("kfree(%p): bad block magic 0x%x (corruption or not from kmalloc)",
               p, b->magic);
    }

    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    if (!(b->size & INUSE_BIT)) {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        kpanic("kfree(%p): double free (block already free, size=%lu)",
               p, (unsigned long)(b->size & SIZE_MASK));
    }

    size_t bsz = b->size & SIZE_MASK;
    b->size = bsz;                  /* clear INUSE_BIT */
    g_used_bytes -= bsz;
    g_free_count++;

    /* Find which arena owns this block, then coalesce that arena. */
    for (arena_t *a = g_arenas; a; a = a->next) {
        if ((uint8_t *)b >= (uint8_t *)first_block(a) &&
            (uint8_t *)b <  (uint8_t *)arena_end(a)) {
            coalesce_arena(a);
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return;
        }
    }
    spin_unlock_irqrestore(&g_heap_lock, flags);
    kpanic("kfree(%p): block does not belong to any known arena", p);
}

void heap_stats(struct heap_stats *out) {
    if (!out) return;
    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    size_t n = 0;
    for (arena_t *a = g_arenas; a; a = a->next) n++;
    out->arenas      = n;
    out->total_bytes = g_total_bytes;
    out->used_bytes  = g_used_bytes;
    out->free_bytes  = g_total_bytes - g_used_bytes;
    out->alloc_count = g_alloc_count;
    out->free_count  = g_free_count;
    spin_unlock_irqrestore(&g_heap_lock, flags);
}

uint64_t heap_virt_base(void) { return KHEAP_VIRT_BASE; }
uint64_t heap_virt_brk(void)  { return g_kheap_brk; }
uint64_t heap_virt_end(void)  { return KHEAP_VIRT_END; }

void heap_dump(void) {
    kprintf("[heap] dump: arenas total=%lu used=%lu free=%lu allocs=%lu frees=%lu\n",
            (unsigned long)g_total_bytes, (unsigned long)g_used_bytes,
            (unsigned long)(g_total_bytes - g_used_bytes),
            (unsigned long)g_alloc_count, (unsigned long)g_free_count);
    int aix = 0;
    for (arena_t *a = g_arenas; a; a = a->next, aix++) {
        kprintf("  arena[%d] %p pages=%lu size=%lu\n",
                aix, a, (unsigned long)a->pages,
                (unsigned long)a->total_size);
        block_hdr_t *b   = first_block(a);
        block_hdr_t *end = arena_end(a);
        int bix = 0;
        while (b < end) {
            size_t bsz  = b->size & SIZE_MASK;
            bool   used = b->size & INUSE_BIT;
            kprintf("    blk[%d] %p size=%6lu %s\n",
                    bix, b, (unsigned long)bsz, used ? "USED" : "free");
            b = (block_hdr_t *)((uint8_t *)b + bsz);
            bix++;
        }
    }
}
