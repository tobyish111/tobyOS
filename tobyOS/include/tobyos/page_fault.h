/* page_fault.h -- demand paging + copy-on-write page fault handler.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * The page fault handler resolves:
 *   - COW (copy-on-write) faults: write to a shared page triggers a copy
 *   - Demand-zero faults: first access to a lazily-allocated page
 *   - VMA-backed faults: address in a valid VMA but not yet mapped
 *   - File-backed faults: mmap'd file page read from disk on first access
 *   - Everything else: segfault -> kill the process
 */

#ifndef TOBYOS_PAGE_FAULT_H
#define TOBYOS_PAGE_FAULT_H

#include <tobyos/types.h>

struct proc;

/* Page table entry flags (architectural + software-defined) */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_COW       (1ULL << 9)   /* software: copy-on-write */
#define PTE_DEMAND    (1ULL << 10)  /* software: demand-zero page */
#define PTE_SWAPPED   (1ULL << 11)  /* software: page is in swap */
#define PTE_SHARED    (1ULL << 52)  /* software: MAP_SHARED page -- NEVER CoW.
                                     * Bits 52..62 are hardware-ignored on
                                     * present PTEs (we don't use protection
                                     * keys); bits 9..11 are all taken above.
                                     * Set at map time by vmm_map(VMM_SHARED)
                                     * for shm-cache/memfd pages; vmm_cow_fork
                                     * copies these PTEs as-is (still writable)
                                     * so parent+child keep writing the SAME
                                     * frame -- POSIX MAP_SHARED across fork.
                                     * (Slice 34: fork used to write-protect
                                     * them; the browser's first post-fork
                                     * parcel write then CoW-diverged its ipcz
                                     * NodeLinkMemory -> children decoded stale
                                     * fragments -> ILLEGAL_MEMORY_RANGE.) */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* x86_64 error code bits pushed by CPU on #PF */
#define PF_ERR_PRESENT  (1ULL << 0)  /* fault caused by page-level protection */
#define PF_ERR_WRITE    (1ULL << 1)  /* write access caused the fault */
#define PF_ERR_USER     (1ULL << 2)  /* fault occurred in user mode */
#define PF_ERR_RSVD     (1ULL << 3)  /* reserved bit set in PTE */
#define PF_ERR_IFETCH   (1ULL << 4)  /* instruction fetch */

/* VMA (virtual memory area) tracking per process */
#define VMA_MAX 64
#define VMA_READ    (1 << 0)
#define VMA_WRITE   (1 << 1)
#define VMA_EXEC    (1 << 2)
#define VMA_SHARED  (1 << 3)
#define VMA_FILE    (1 << 4)
#define VMA_COW     (1 << 5)
#define VMA_ANON    (1 << 6)

struct vma {
    uint64_t start;
    uint64_t end;           /* exclusive */
    uint32_t flags;
    int      file_fd;       /* for mmap'd files, -1 if anonymous */
    uint64_t file_offset;
};

struct vm_space {
    struct vma areas[VMA_MAX];
    int        area_count;
    uint64_t   brk;         /* program break for sbrk/brk */
    uint64_t   mmap_base;   /* next available mmap address */
};

/* Main page fault entry point. Returns true if fault was resolved,
 * false if the process should be killed (segfault). */
bool page_fault_handler(uint64_t fault_addr, uint64_t error_code,
                        struct proc *p);

/* Walk page tables to find the PTE pointer for a virtual address.
 * Returns NULL if intermediate table is not present. */
uint64_t *get_pte(uint64_t cr3, uint64_t virt_addr);

/* Allocate a new page, copy 4KB from old_phys, update *new_pte. */
bool cow_copy_page(uint64_t old_phys, uint64_t *pte);

/* Get the vm_space for a given process. */
struct vm_space *vm_space_for_proc(struct proc *p);

/* Find the VMA containing `addr`, or NULL. */
struct vma *vma_find(struct vm_space *vms, uint64_t addr);

/* Add a new VMA to the vm_space. Returns 0 on success, -1 if full. */
int vma_add(struct vm_space *vms, uint64_t start, uint64_t end,
            uint32_t flags);

/* Per-page reference counting for COW. */
void page_ref_init(void);
void page_ref_inc(uint64_t phys);
void page_ref_dec(uint64_t phys);
int  page_ref_get(uint64_t phys);

/* COW fork: mark parent+child page tables for copy-on-write.
 * Walks user-space entries in parent_cr3, clears writable + sets COW
 * in both parent and child page tables. Returns 0 on success. */
int vmm_cow_fork(uint64_t parent_cr3, uint64_t child_cr3);

/* Initialize page fault subsystem (call once during boot). */
void page_fault_init(void);

#endif /* TOBYOS_PAGE_FAULT_H */
