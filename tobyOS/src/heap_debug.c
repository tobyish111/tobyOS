/* src/heap_debug.c -- Kernel heap debugging extensions (Phase 8).
 *
 * Adds canary checking, free-list poisoning, double-free detection,
 * and allocation tracking to the existing heap allocator.
 */

#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/stacktrace.h>

/* Debug canary values */
#define HEAP_CANARY_HEAD   0xDEADBEEFCAFEBABEULL
#define HEAP_CANARY_TAIL   0xFEEDFACE12345678ULL
#define HEAP_POISON_FREE   0xFEEDFACEFEEDFACEULL
#define HEAP_POISON_BYTE   0xFE

/* Maximum tracked allocations */
#define HEAP_TRACK_MAX     1024

struct heap_debug_entry {
    void    *ptr;
    size_t   size;
    uint64_t caller;       /* return address of caller */
    int      active;
};

static struct heap_debug_entry g_track[HEAP_TRACK_MAX];
static int g_track_count = 0;
static int g_debug_enabled = 0;
static int g_total_allocs = 0;
static int g_total_frees = 0;
static int g_corruption_count = 0;

/* ---- Debug header prepended to each allocation ---- */

struct heap_debug_hdr {
    uint64_t canary_head;
    size_t   user_size;
    uint64_t caller_rip;
    uint64_t magic;
};

#define HEAP_DEBUG_MAGIC  0x48454150444247ULL  /* "HEAPDBG" */

/* ---- Public API ---- */

void heap_debug_enable(void) {
    g_debug_enabled = 1;
    kprintf("[heap_debug] debug mode enabled (canaries + tracking)\n");
}

void heap_debug_disable(void) {
    g_debug_enabled = 0;
}

int heap_debug_is_enabled(void) {
    return g_debug_enabled;
}

/* Wrap around kmalloc to add canaries. If debug is off, this is a no-op
 * wrapper. Call this instead of kmalloc when debugging. */
void *heap_debug_alloc(size_t size) {
    if (!g_debug_enabled) return kmalloc(size);

    size_t total = sizeof(struct heap_debug_hdr) + size + sizeof(uint64_t);
    void *raw = kmalloc(total);
    if (!raw) return 0;

    /* Get caller return address */
    uint64_t caller = 0;
    __asm__ volatile ("mov 8(%%rbp), %0" : "=r"(caller));

    struct heap_debug_hdr *hdr = (struct heap_debug_hdr *)raw;
    hdr->canary_head = HEAP_CANARY_HEAD;
    hdr->user_size = size;
    hdr->caller_rip = caller;
    hdr->magic = HEAP_DEBUG_MAGIC;

    void *user_ptr = (uint8_t *)raw + sizeof(struct heap_debug_hdr);

    /* Tail canary */
    uint64_t *tail = (uint64_t *)((uint8_t *)user_ptr + size);
    *tail = HEAP_CANARY_TAIL;

    /* Track allocation */
    if (g_track_count < HEAP_TRACK_MAX) {
        g_track[g_track_count].ptr = user_ptr;
        g_track[g_track_count].size = size;
        g_track[g_track_count].caller = caller;
        g_track[g_track_count].active = 1;
        g_track_count++;
    }

    g_total_allocs++;
    return user_ptr;
}

/* Validate and free a debug-tracked allocation. */
void heap_debug_free(void *ptr) {
    if (!ptr) return;
    if (!g_debug_enabled) { kfree(ptr); return; }

    struct heap_debug_hdr *hdr =
        (struct heap_debug_hdr *)((uint8_t *)ptr - sizeof(struct heap_debug_hdr));

    /* Check magic to detect double-free or wild pointer */
    if (hdr->magic != HEAP_DEBUG_MAGIC) {
        kprintf("[heap_debug] DOUBLE FREE or CORRUPTION at %p\n", ptr);
        kprintf("  magic: 0x%lx (expected 0x%lx)\n",
                (unsigned long)hdr->magic, (unsigned long)HEAP_DEBUG_MAGIC);
        stacktrace_print();
        g_corruption_count++;
        return;
    }

    /* Verify head canary */
    if (hdr->canary_head != HEAP_CANARY_HEAD) {
        kprintf("[heap_debug] HEAD CANARY CORRUPT at %p (alloc from 0x%lx)\n",
                ptr, hdr->caller_rip);
        kprintf("  canary: 0x%lx (expected 0x%lx)\n",
                (unsigned long)hdr->canary_head, (unsigned long)HEAP_CANARY_HEAD);
        stacktrace_print();
        g_corruption_count++;
    }

    /* Verify tail canary */
    uint64_t *tail = (uint64_t *)((uint8_t *)ptr + hdr->user_size);
    if (*tail != HEAP_CANARY_TAIL) {
        kprintf("[heap_debug] TAIL CANARY CORRUPT at %p+%lu (alloc from 0x%lx)\n",
                ptr, (unsigned long)hdr->user_size, hdr->caller_rip);
        kprintf("  canary: 0x%lx (expected 0x%lx)\n",
                (unsigned long)*tail, (unsigned long)HEAP_CANARY_TAIL);
        stacktrace_print();
        g_corruption_count++;
    }

    /* Mark as freed (invalidate magic) */
    hdr->magic = 0;

    /* Poison the user area */
    memset(ptr, HEAP_POISON_BYTE, hdr->user_size);

    /* Remove from tracking */
    for (int i = 0; i < g_track_count; i++) {
        if (g_track[i].ptr == ptr && g_track[i].active) {
            g_track[i].active = 0;
            break;
        }
    }

    g_total_frees++;
    size_t total = sizeof(struct heap_debug_hdr) + hdr->user_size + sizeof(uint64_t);
    (void)total;
    kfree(hdr);
}

/* Walk all tracked allocations and verify canaries are intact. */
int heap_check(void) {
    if (!g_debug_enabled) return 0;

    int errors = 0;
    for (int i = 0; i < g_track_count; i++) {
        if (!g_track[i].active) continue;

        void *ptr = g_track[i].ptr;
        struct heap_debug_hdr *hdr =
            (struct heap_debug_hdr *)((uint8_t *)ptr - sizeof(struct heap_debug_hdr));

        if (hdr->magic != HEAP_DEBUG_MAGIC) {
            kprintf("[heap_check] slot %d: magic invalid at %p\n", i, ptr);
            errors++;
            continue;
        }
        if (hdr->canary_head != HEAP_CANARY_HEAD) {
            kprintf("[heap_check] slot %d: head canary corrupt at %p\n", i, ptr);
            errors++;
        }
        uint64_t *tail = (uint64_t *)((uint8_t *)ptr + hdr->user_size);
        if (*tail != HEAP_CANARY_TAIL) {
            kprintf("[heap_check] slot %d: tail canary corrupt at %p (size %zu)\n",
                    i, ptr, hdr->user_size);
            errors++;
        }
    }

    if (errors == 0) {
        kprintf("[heap_check] all %d active allocations OK\n",
                g_total_allocs - g_total_frees);
    } else {
        kprintf("[heap_check] %d ERRORS found in %d allocations\n",
                errors, g_total_allocs - g_total_frees);
    }
    return errors;
}

/* Print heap debug statistics. */
void heap_debug_stats(void) {
    kprintf("[heap_debug] stats: allocs=%d frees=%d active=%d corruptions=%d\n",
            g_total_allocs, g_total_frees,
            g_total_allocs - g_total_frees, g_corruption_count);
}

/* Dump all active allocations (for leak detection). */
void heap_debug_dump_leaks(void) {
    int leaks = 0;
    kprintf("[heap_debug] active allocations:\n");
    for (int i = 0; i < g_track_count; i++) {
        if (!g_track[i].active) continue;
        kprintf("  %p  size=%zu  caller=0x%lx\n",
                g_track[i].ptr, g_track[i].size, g_track[i].caller);
        leaks++;
    }
    kprintf("[heap_debug] %d active allocation(s)\n", leaks);
}
