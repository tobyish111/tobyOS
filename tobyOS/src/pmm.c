/* pmm.c -- bitmap-based physical page allocator.
 *
 * Strategy:
 *   1. Walk Limine's memmap, find the highest USABLE physical address.
 *      Bitmap covers pages [0..highest/4096), one bit per page.
 *   2. Find a USABLE region big enough to hold the bitmap; place the
 *      bitmap at its physical start, accessed via HHDM.
 *   3. Mark every bit as USED (1), then walk USABLE regions and clear
 *      bits for each page they cover (truncating partial pages at both
 *      ends to stay strictly inside the region).
 *   4. Re-mark the bitmap's own pages as USED so we don't hand them out.
 *   5. (Optional sanity) Mark physical page 0 as USED -- we never want
 *      to return a NULL-looking address from pmm_alloc_page.
 *
 * Allocation is a linear scan from a "first free" hint; freeing rewinds
 * the hint if the freed page is earlier. O(N) worst case, but in
 * practice we hit a free page very quickly. Good enough until we have
 * a slab/buddy allocator.
 */

#include <tobyos/pmm.h>
#include <tobyos/limine.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>

static uint8_t *g_bitmap;
static size_t   g_bitmap_bytes;
static size_t   g_total_pages;
static size_t   g_used_pages;
static size_t   g_first_free_hint;
static uint64_t g_hhdm;
/* Milestone 22 step 5: protect the bitmap + free-page hint against
 * concurrent allocators. In v1 only the BSP allocates (APs sit in
 * sched_idle), but this is the foundational allocator and the cost
 * is one xchg per call -- cheap insurance against future regressions
 * the moment any AP path needs a page (LAPIC timer ISR currently
 * doesn't, but a future v2 may want to balloon kernel work). */
static spinlock_t g_pmm_lock = SPINLOCK_INIT;

static inline bool bm_get(size_t i) {
    return (g_bitmap[i / 8] >> (i & 7)) & 1u;
}
static inline void bm_set(size_t i) {
    g_bitmap[i / 8] |= (uint8_t)(1u << (i & 7));
}
static inline void bm_clr(size_t i) {
    g_bitmap[i / 8] &= (uint8_t)~(1u << (i & 7));
}

void *pmm_phys_to_virt(uint64_t phys) {
    return (void *)(phys + g_hhdm);
}

uint64_t pmm_virt_to_phys(void *virt) {
    return (uint64_t)virt - g_hhdm;
}

static const char *type_name(uint64_t t) {
    switch (t) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi-reclaim";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi-nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    default:                                   return "unknown";
    }
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    if (!memmap || memmap->entry_count == 0) {
        kpanic("pmm_init: no memmap from Limine");
    }

    g_hhdm = hhdm_offset;

    /* ----- pass 1: log the memmap and find highest USABLE end ----- */
    uint64_t highest = 0;
    uint64_t total_usable_bytes = 0;

    kprintf("[pmm] Limine memmap (%lu entries):\n", memmap->entry_count);
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        kprintf("  %p..%p  %8lu KiB  %s\n",
                (void *)e->base,
                (void *)(e->base + e->length),
                e->length / 1024,
                type_name(e->type));
        if (e->type == LIMINE_MEMMAP_USABLE) {
            total_usable_bytes += e->length;
            uint64_t end = e->base + e->length;
            if (end > highest) highest = end;
        }
    }

    if (highest == 0) {
        kpanic("pmm_init: no USABLE memory in memmap");
    }

    /* ----- pass 2: size + place the bitmap ----- */
    g_total_pages   = highest / PAGE_SIZE;
    g_bitmap_bytes  = (g_total_pages + 7) / 8;
    size_t bitmap_pages = (g_bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t bitmap_aligned = bitmap_pages * PAGE_SIZE;

    /* Search for a USABLE region big enough for the bitmap. We use a
     * separate `found` flag instead of treating `bitmap_phys == 0` as
     * "not found", because under UEFI the first USABLE region can
     * legitimately start at physical address 0 (SeaBIOS always
     * reserves the IVT/BDA at 0 so this never came up under legacy
     * BIOS, but Limine's UEFI memmap on QEMU+OVMF does include a
     * usable region @ 0x0 covering 540 KiB of low conventional RAM). */
    uint64_t bitmap_phys = 0;
    bool     bitmap_found = false;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        if (e->length >= bitmap_aligned) {
            bitmap_phys = e->base;
            bitmap_found = true;
            break;
        }
    }
    if (!bitmap_found) {
        kpanic("pmm_init: no USABLE region big enough for %lu-byte bitmap",
               (unsigned long)g_bitmap_bytes);
    }

    g_bitmap = (uint8_t *)pmm_phys_to_virt(bitmap_phys);
    memset(g_bitmap, 0xFF, g_bitmap_bytes);
    g_used_pages = g_total_pages;

    /* ----- pass 3: clear bits for USABLE pages (truncating ends) ----- */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t start_page = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t end_page   = (e->base + e->length) / PAGE_SIZE;
        for (uint64_t p = start_page; p < end_page; p++) {
            if (bm_get(p)) {
                bm_clr(p);
                g_used_pages--;
            }
        }
    }

    /* ----- pass 4: re-reserve the bitmap's own pages ----- */
    uint64_t bitmap_first_page = bitmap_phys / PAGE_SIZE;
    for (size_t i = 0; i < bitmap_pages; i++) {
        size_t p = (size_t)bitmap_first_page + i;
        if (!bm_get(p)) {
            bm_set(p);
            g_used_pages++;
        }
    }

    /* Reserve physical page 0 unconditionally so pmm_alloc_page never
     * returns 0 (which would alias our OOM signal). */
    if (g_total_pages > 0 && !bm_get(0)) {
        bm_set(0);
        g_used_pages++;
    }

    g_first_free_hint = 1;

    kprintf("[pmm] managed:  %lu pages (%lu MiB) up to %p\n",
            (unsigned long)g_total_pages,
            (unsigned long)((g_total_pages * PAGE_SIZE) / (1024 * 1024)),
            (void *)highest);
    kprintf("[pmm] usable:   %lu KiB across %lu regions\n",
            (unsigned long)(total_usable_bytes / 1024),
            (unsigned long)memmap->entry_count);
    kprintf("[pmm] bitmap:   %lu bytes at phys %p (virt %p), %lu pages\n",
            (unsigned long)g_bitmap_bytes,
            (void *)bitmap_phys,
            g_bitmap,
            (unsigned long)bitmap_pages);
    kprintf("[pmm] free:     %lu pages (%lu MiB) -- used: %lu pages\n",
            (unsigned long)pmm_free_pages(),
            (unsigned long)((pmm_free_pages() * PAGE_SIZE) / (1024 * 1024)),
            (unsigned long)g_used_pages);
}

uint64_t pmm_alloc_page(void) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t phys = 0;
    for (size_t i = g_first_free_hint; i < g_total_pages; i++) {
        if (!bm_get(i)) {
            bm_set(i);
            g_used_pages++;
            g_first_free_hint = i + 1;
            phys = (uint64_t)i * PAGE_SIZE;
            break;
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return phys;  /* 0 == out of memory */
}

uint64_t pmm_alloc_pages(size_t n) {
    if (n == 0) return 0;
    if (n == 1) return pmm_alloc_page();

    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t result = 0;
    /* Linear scan for a run of N consecutive free pages. */
    size_t run_start = 0;
    size_t run_len   = 0;
    for (size_t i = g_first_free_hint; i < g_total_pages; i++) {
        if (!bm_get(i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len == n) {
                for (size_t k = 0; k < n; k++) bm_set(run_start + k);
                g_used_pages += n;
                /* Don't bump the hint past the run -- earlier pages may
                 * have been freed since we last scanned. */
                result = (uint64_t)run_start * PAGE_SIZE;
                break;
            }
        } else {
            run_len = 0;
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return result;
}

void pmm_free_pages_range(uint64_t phys, size_t n) {
    for (size_t k = 0; k < n; k++) {
        pmm_free_page(phys + (uint64_t)k * PAGE_SIZE);
    }
}

void pmm_free_page(uint64_t phys) {
    if (phys == 0 || (phys & (PAGE_SIZE - 1)) != 0) {
        kprintf("[pmm] WARN: pmm_free_page(%p): not a page-aligned phys\n",
                (void *)phys);
        return;
    }
    size_t i = phys / PAGE_SIZE;
    if (i >= g_total_pages) {
        kprintf("[pmm] WARN: pmm_free_page(%p): out of range\n", (void *)phys);
        return;
    }
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (!bm_get(i)) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        kprintf("[pmm] WARN: double free of page %p\n", (void *)phys);
        return;
    }
    bm_clr(i);
    g_used_pages--;
    if (i < g_first_free_hint) g_first_free_hint = i;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

bool pmm_reserve_page(uint64_t phys) {
    if ((phys & (PAGE_SIZE - 1)) != 0) return false;
    size_t i = phys / PAGE_SIZE;
    if (i >= g_total_pages) return false;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (bm_get(i)) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return false;       /* already used (or non-USABLE) */
    }
    bm_set(i);
    g_used_pages++;
    if (i == g_first_free_hint) g_first_free_hint++;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return true;
}

size_t pmm_total_pages(void) { return g_total_pages; }
size_t pmm_used_pages(void)  { return g_used_pages;  }
size_t pmm_free_pages(void)  { return g_total_pages - g_used_pages; }

uint64_t pmm_hhdm_offset(void)  { return g_hhdm; }
uint64_t pmm_highest_phys(void) { return (uint64_t)g_total_pages * PAGE_SIZE; }
