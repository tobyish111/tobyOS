/* pmm.h -- physical memory manager.
 *
 * Bitmap allocator over Limine's USABLE memory regions, with one bit
 * per 4 KiB page. Returns physical addresses (suitable for paging /
 * DMA); use HHDM to access the contents from kernel virtual space.
 */

#ifndef TOBYOS_PMM_H
#define TOBYOS_PMM_H

#include <tobyos/types.h>

#define PAGE_SIZE 4096u

struct limine_memmap_response;

/* Initialise the PMM from Limine's memmap + HHDM data. Must be called
 * exactly once during boot. Panics on a bad / empty memmap. */
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/* Allocate one 4 KiB page. Returns the physical address (always page-
 * aligned), or 0 if the system is out of memory. */
uint64_t pmm_alloc_page(void);

/* Allocate N physically-contiguous 4 KiB pages. Returns the physical
 * address of the first page, or 0 if no run of N free pages exists.
 * O(N * total_pages) worst case -- fine for our growth path. */
uint64_t pmm_alloc_pages(size_t n);

/* Free one previously-allocated page. Silently ignores out-of-range or
 * already-free addresses (logs a warning in the latter case). */
void pmm_free_page(uint64_t phys);

/* Free N consecutive pages starting at `phys`. */
void pmm_free_pages_range(uint64_t phys, size_t n);

/* Reserve a SPECIFIC physical page so the allocator never hands it
 * out. Used (e.g.) by smp.c to claim 0x8000 for the AP trampoline at
 * a fixed location. Returns false if the page is already allocated
 * or out of range. */
bool pmm_reserve_page(uint64_t phys);

/* HHDM helpers: turn a physical address into a kernel virtual one and
 * vice versa. Both are O(1) integer adds. */
void *pmm_phys_to_virt(uint64_t phys);
uint64_t pmm_virt_to_phys(void *virt);

/* Raw HHDM offset Limine handed us. vmm_init needs this to build its
 * own copy of the direct map. */
uint64_t pmm_hhdm_offset(void);

/* Highest physical byte covered by the bitmap (i.e. one past the end of
 * the last managed page). vmm_init uses this as the upper bound when
 * mirroring HHDM into its fresh PML4. */
uint64_t pmm_highest_phys(void);

/* Counters in pages. total = managed pages (covered by bitmap). */
size_t pmm_total_pages(void);
size_t pmm_used_pages(void);
size_t pmm_free_pages(void);

#endif /* TOBYOS_PMM_H */
