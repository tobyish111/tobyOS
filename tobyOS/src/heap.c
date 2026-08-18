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

/* At or above this, an allocation gets a private arena that is released the
 * moment it is freed. Chosen well above anything the kernel allocates
 * routinely and well below a process image, so the general pool keeps
 * servicing everyday traffic and only the big transients are segregated. */
#define KHEAP_LARGE       (128u * 1024u)

/* arena_t::flags */
#define ARENA_HUGE        0x1u   /* 2 MiB-page backed (was the old _pad==1) */
#define ARENA_DEDICATED   0x2u   /* holds exactly one allocation; freed with it */
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
    uint32_t      flags;        /* ARENA_* -- was _pad; keeps the header 32
                                 * bytes, which is what makes payloads
                                 * 16-aligned. Do not grow this struct. */
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

/* 2 MiB huge page, and a running count of 4 KiB-equivalent pages we've
 * backed with huge leaves (stat / self-test). */
#define KHEAP_2M (2ULL * 1024 * 1024)
static size_t g_huge_arena_pages;

size_t heap_huge_pages(void) { return g_huge_arena_pages; }

/* Undo the first `done_2m` huge mappings of a partially-grown huge arena. */
static void rollback_huge(uint64_t base, size_t done_2m) {
    for (size_t j = 0; j < done_2m; j++) {
        uint64_t v = base + (uint64_t)j * KHEAP_2M;
        uint64_t p = vmm_translate(v) & ~(KHEAP_2M - 1);
        vmm_unmap(v, KHEAP_2M);
        if (p) pmm_free_2m(p);
    }
}

/* Grow the heap with 2 MiB huge pages: one PD leaf per 2 MiB instead of
 * 512 PTEs + a PT page, which cuts page-table memory and TLB pressure for
 * large allocations. Best-effort -- returns 0 (caller falls back to the
 * 4 KiB path) if the 2 MiB-aligned physical runs aren't available. The
 * brk is rounded up to a 2 MiB boundary first (the skipped virtual
 * address space is free; physical memory is untouched). */
static arena_t *grow_huge(size_t bytes) {
    uint64_t base = (g_kheap_brk + KHEAP_2M - 1) & ~(KHEAP_2M - 1);
    size_t n2m = (bytes + KHEAP_2M - 1) / KHEAP_2M;
    if (base + (uint64_t)n2m * KHEAP_2M > KHEAP_VIRT_END) return 0;

    for (size_t k = 0; k < n2m; k++) {
        uint64_t phys = pmm_alloc_2m();
        if (phys == 0) { rollback_huge(base, k); return 0; }
        if (!vmm_map(base + (uint64_t)k * KHEAP_2M, phys, KHEAP_2M,
                     VMM_HUGE_2M | VMM_WRITE | VMM_NX)) {
            pmm_free_2m(phys);
            rollback_huge(base, k);
            return 0;
        }
    }
    g_kheap_brk = base + (uint64_t)n2m * KHEAP_2M;

    arena_t *a    = (arena_t *)base;
    a->next       = g_arenas;
    a->pages      = n2m * (KHEAP_2M / PAGE_SIZE);
    a->total_size = (size_t)n2m * KHEAP_2M - sizeof(arena_t);
    a->magic      = KHEAP_MAGIC;
    a->flags      = ARENA_HUGE;
    g_arenas      = a;

    block_hdr_t *b = first_block(a);
    b->size  = a->total_size;
    b->magic = KHEAP_MAGIC;

    g_total_bytes      += a->total_size;
    g_huge_arena_pages += a->pages;

    static bool announced;
    if (!announced) {
        announced = true;
        kprintf("[heap] first 2 MiB huge arena: %lu x 2 MiB at %p\n",
                (unsigned long)n2m, (void *)base);
    }
    return a;
}

static arena_t *grow(size_t need_bytes) {
    /* Pages needed to fit one block of `need_bytes` plus the arena
     * header. Round up to KHEAP_GROW_PAGES so we don't churn one page
     * at a time. */
    size_t bytes = align_up(need_bytes + sizeof(arena_t), PAGE_SIZE);
    size_t pages = bytes / PAGE_SIZE;
    if (pages < KHEAP_GROW_PAGES) pages = KHEAP_GROW_PAGES;

    /* Large arenas get 2 MiB huge-page backing when possible; on
     * fragmentation we transparently fall back to the 4 KiB path below. */
    if (bytes >= KHEAP_2M) {
        arena_t *huge = grow_huge(bytes);
        if (huge) return huge;
    }

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
    a->flags     = 0;                       /* 4 KiB-backed arena */
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

    /* Big transients never share an arena with anything else -- see the note
     * at the top of this file. Skipping the search is not just an
     * optimisation: allocating a 1 MiB image inside a general arena is what
     * lets a later 64-byte object pin a megabyte for the life of the system. */
    if (need >= KHEAP_LARGE) {
        arena_t *ded = grow(need);
        if (!ded) {
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return 0;
        }
        ded->flags |= ARENA_DEDICATED;
        void *p = try_alloc_in(ded, need);
        if (p) g_alloc_count++;
        spin_unlock_irqrestore(&g_heap_lock, flags);
        return p;
    }

    for (arena_t *a = g_arenas; a; a = a->next) {
        if (a->flags & ARENA_DEDICATED) continue;   /* not a shared pool */
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
    if (p) {
        block_hdr_t *hdr = (block_hdr_t *)((uint8_t *)p - sizeof(block_hdr_t));
        size_t payload = (hdr->size & SIZE_MASK) - sizeof(block_hdr_t);
        memset(p, 0, payload);
    }
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

    /* Find which arena owns this block, then coalesce that arena. `prev` is
     * tracked so a dedicated arena can be unlinked below. */
    arena_t *prev = 0;
    for (arena_t *a = g_arenas; a; prev = a, a = a->next) {
        if ((uint8_t *)b < (uint8_t *)first_block(a) ||
            (uint8_t *)b >= (uint8_t *)arena_end(a)) continue;

        coalesce_arena(a);

        /* A dedicated arena holds exactly one allocation, so freeing it makes
         * the arena wholly free and it can go back to the PMM. Verified
         * rather than assumed: the single block must be free and span the
         * whole arena. */
        if (a->flags & ARENA_DEDICATED) {
            block_hdr_t *only = first_block(a);
            if (!(only->size & INUSE_BIT) &&
                (only->size & SIZE_MASK) == a->total_size) {
                if (prev) prev->next = a->next;
                else      g_arenas   = a->next;
                g_total_bytes -= a->total_size;

                uint64_t base  = (uint64_t)a;
                size_t   pages = a->pages;
                bool     huge  = (a->flags & ARENA_HUGE) != 0;
                /* Unmap AFTER unlinking: nothing can reach the arena now. The
                 * virtual range is not recycled (g_kheap_brk is a bump cursor
                 * over a 1 TiB window); the physical frames are, and those are
                 * what ran out. */
                spin_unlock_irqrestore(&g_heap_lock, flags);
                if (huge) rollback_huge(base, pages / (KHEAP_2M / PAGE_SIZE));
                else      rollback_pages(base, pages);
                return;
            }
        }
        spin_unlock_irqrestore(&g_heap_lock, flags);
        return;
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

/* ================================================================
 * 2 MiB large-page self-test (-DHUGEPAGE_SELFTEST). Proves the PMM huge-
 * frame allocator + VMM huge map/translate/unmap, and that a large
 * kmalloc is transparently backed by 2 MiB leaves. [HUGEPT] markers.
 * ================================================================ */
int hugepage_self_test(void) {
    int fails = 0;
    kprintf("[HUGEPT] 2 MiB large-page self-test\n");

    /* ---- Test 1: direct PMM 2M alloc + VMM huge map/RW/translate/unmap ---- */
    {
        size_t   free0 = pmm_free_pages();
        uint64_t phys  = pmm_alloc_2m();
        bool aligned = (phys != 0) && ((phys & (KHEAP_2M - 1)) == 0);
        bool drop512 = aligned && (pmm_free_pages() == free0 - 512);

        /* An unused, 2 MiB-aligned VA near the top of the heap window. */
        uint64_t tva = KHEAP_VIRT_END - 4 * KHEAP_2M;
        bool mapped = aligned &&
            vmm_map(tva, phys, KHEAP_2M, VMM_HUGE_2M | VMM_WRITE | VMM_NX);
        bool huge  = mapped && (vmm_leaf_size(tva) == KHEAP_2M);
        bool xlate = mapped && vmm_translate(tva) == phys &&
                     vmm_translate(tva + 0x100000) == phys + 0x100000;

        bool rw = mapped;
        if (mapped) {
            volatile uint32_t *p = (volatile uint32_t *)tva;
            for (size_t off = 0; off < KHEAP_2M; off += PAGE_SIZE)
                p[off / 4] = (uint32_t)(off ^ 0xABCD1234u);
            for (size_t off = 0; off < KHEAP_2M; off += PAGE_SIZE)
                if (p[off / 4] != (uint32_t)(off ^ 0xABCD1234u)) { rw = false; break; }
        }

        bool unmapped = mapped && vmm_unmap(tva, KHEAP_2M) &&
                        vmm_leaf_size(tva) == 0 && vmm_translate(tva) == 0;
        /* Isolate the huge-frame return from the VMM intermediate tables
         * (PDPT/PD) that vmm_map allocated and unmap intentionally keeps:
         * the free count must rise by exactly 512 across pmm_free_2m. */
        size_t before_free = pmm_free_pages();
        if (phys) pmm_free_2m(phys);
        bool gain512 = (pmm_free_pages() == before_free + 512);

        bool pass = drop512 && huge && xlate && rw && unmapped && gain512;
        if (!pass) fails++;
        kprintf("[HUGEPT] t1 direct 2M: %s (aligned=%d -512f=%d huge_leaf=%d "
                "xlate=%d rw=%d unmap=%d +512f=%d)\n", pass ? "PASS" : "FAIL",
                aligned, drop512, huge, xlate, rw, unmapped, gain512);
    }

    /* ---- Test 2: a large kmalloc is huge-page backed ---- */
    {
        size_t hp0 = heap_huge_pages();
        const size_t big = 4u * 1024 * 1024;        /* 4 MiB */
        uint8_t *p = kmalloc(big);
        bool ok   = (p != NULL);
        bool huge = ok && (vmm_leaf_size((uint64_t)p) == KHEAP_2M);
        bool grew = (heap_huge_pages() > hp0);

        bool rw = ok;
        if (ok) {
            for (size_t i = 0; i < big; i += PAGE_SIZE)
                p[i] = (uint8_t)((i * 2654435761u) >> 24);
            for (size_t i = 0; i < big; i += PAGE_SIZE)
                if (p[i] != (uint8_t)((i * 2654435761u) >> 24)) { rw = false; break; }
        }
        if (p) kfree(p);

        size_t n2m = (big + KHEAP_2M - 1) / KHEAP_2M;  /* PT pages saved vs 4K */
        /* A >= 2 MiB allocation always lands in a huge-backed arena (every
         * 4K arena is < 2 MiB), so huge_leaf is the robust assertion; grew
         * is informational (it's false if a prior huge arena had room). */
        bool pass = ok && rw && huge;
        if (!pass) fails++;
        kprintf("[HUGEPT] t2 heap kmalloc(4MiB): %s (huge_leaf=%d grew=%d rw=%d "
                "PT_pages_saved=%lu)\n", pass ? "PASS" : "FAIL",
                huge, grew, rw, (unsigned long)n2m);
    }

    kprintf("[HUGEPT] %s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : -1;
}
