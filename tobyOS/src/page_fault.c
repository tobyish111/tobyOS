/* page_fault.c -- demand paging + copy-on-write page fault handler.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * Handles #PF exceptions for user-space processes:
 *   1. COW fault: write to shared page -> copy + remap writable
 *   2. Demand-zero: first touch of lazy-allocated page -> zero-fill
 *   3. VMA miss: address in valid VMA but unmapped -> allocate + map
 *   4. File-backed: mmap'd file region -> read page from disk
 *   5. Swap-in: page was swapped out -> bring it back
 *   6. Otherwise: segfault -> kill process
 */

#include <tobyos/page_fault.h>
#include <tobyos/proc.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/swap.h>

/* Maximum physical pages we track refcounts for (256K pages = 1 GiB) */
#define PAGE_REF_MAX  (256 * 1024)

static uint16_t g_page_refcounts[PAGE_REF_MAX];

/* Per-process vm_space table, indexed by pid */
static struct vm_space g_vm_spaces[PROC_MAX];

/* ---- Page reference counting ---- */

void page_ref_init(void) {
    memset(g_page_refcounts, 0, sizeof(g_page_refcounts));
}

static inline size_t phys_to_refidx(uint64_t phys) {
    return (phys >> 12) & (PAGE_REF_MAX - 1);
}

void page_ref_inc(uint64_t phys) {
    size_t idx = phys_to_refidx(phys);
    if (idx < PAGE_REF_MAX && g_page_refcounts[idx] < 0xFFFF)
        g_page_refcounts[idx]++;
}

void page_ref_dec(uint64_t phys) {
    size_t idx = phys_to_refidx(phys);
    if (idx < PAGE_REF_MAX && g_page_refcounts[idx] > 0)
        g_page_refcounts[idx]--;
}

int page_ref_get(uint64_t phys) {
    size_t idx = phys_to_refidx(phys);
    if (idx >= PAGE_REF_MAX) return 0;
    return g_page_refcounts[idx];
}

/* ---- Page table walker ---- */

uint64_t *get_pte(uint64_t cr3, uint64_t virt_addr) {
    uint64_t hhdm = vmm_hhdm_offset();

    uint64_t *pml4 = (uint64_t *)(cr3 + hhdm);
    unsigned i4 = (virt_addr >> 39) & 0x1FF;
    if (!(pml4[i4] & PTE_PRESENT)) return NULL;

    uint64_t *pdpt = (uint64_t *)((pml4[i4] & PTE_ADDR_MASK) + hhdm);
    unsigned i3 = (virt_addr >> 30) & 0x1FF;
    if (!(pdpt[i3] & PTE_PRESENT)) return NULL;
    if (pdpt[i3] & (1ULL << 7)) return NULL; /* 1GiB page, can't drill further */

    uint64_t *pd = (uint64_t *)((pdpt[i3] & PTE_ADDR_MASK) + hhdm);
    unsigned i2 = (virt_addr >> 21) & 0x1FF;
    if (!(pd[i2] & PTE_PRESENT)) return NULL;
    if (pd[i2] & (1ULL << 7)) return NULL; /* 2MiB page */

    uint64_t *pt = (uint64_t *)((pd[i2] & PTE_ADDR_MASK) + hhdm);
    unsigned i1 = (virt_addr >> 12) & 0x1FF;
    return &pt[i1];
}

/* ---- COW page copy ---- */

bool cow_copy_page(uint64_t old_phys, uint64_t *pte) {
    uint64_t new_phys = pmm_alloc_page();
    if (!new_phys) return false;

    uint64_t hhdm = vmm_hhdm_offset();
    memcpy((void *)(new_phys + hhdm), (void *)(old_phys + hhdm), PAGE_SIZE);

    /* Build new PTE: present + writable + user, clear COW */
    *pte = new_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /* Decrement refcount on old page; free if no more references */
    page_ref_dec(old_phys);
    if (page_ref_get(old_phys) == 0)
        pmm_free_page(old_phys);

    /* New page starts with refcount 1 */
    page_ref_inc(new_phys);

    return true;
}

/* ---- VMA helpers ---- */

struct vm_space *vm_space_for_proc(struct proc *p) {
    if (!p) return NULL;
    int pid = p->pid;
    if (pid < 0 || pid >= PROC_MAX) return NULL;
    return &g_vm_spaces[pid];
}

struct vma *vma_find(struct vm_space *vms, uint64_t addr) {
    if (!vms) return NULL;
    for (int i = 0; i < vms->area_count; i++) {
        if (addr >= vms->areas[i].start && addr < vms->areas[i].end)
            return &vms->areas[i];
    }
    return NULL;
}

int vma_add(struct vm_space *vms, uint64_t start, uint64_t end,
            uint32_t flags) {
    if (!vms || vms->area_count >= VMA_MAX) return -1;
    struct vma *v = &vms->areas[vms->area_count++];
    v->start = start;
    v->end = end;
    v->flags = flags;
    v->file_fd = -1;
    v->file_offset = 0;
    return 0;
}

/* ---- TLB invalidation ---- */

static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ---- Main page fault handler ---- */

bool page_fault_handler(uint64_t fault_addr, uint64_t error_code,
                        struct proc *p) {
    if (!p) return false;

    uint64_t page_va = fault_addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *pte = get_pte(p->cr3, fault_addr);

    /* Case 1: COW fault -- write to a page marked copy-on-write */
    if ((error_code & PF_ERR_WRITE) && pte && (*pte & PTE_PRESENT) &&
        (*pte & PTE_COW)) {
        uint64_t old_phys = *pte & PTE_ADDR_MASK;

        /* If we're the sole owner, just make it writable */
        if (page_ref_get(old_phys) <= 1) {
            *pte = (*pte | PTE_WRITABLE) & ~PTE_COW;
            invlpg(page_va);
            return true;
        }

        /* Multiple references: must copy */
        if (!cow_copy_page(old_phys, pte))
            return false; /* OOM during COW */

        invlpg(page_va);
        return true;
    }

    /* Case 2: Demand-zero fault -- PTE marked DEMAND but not present */
    if (pte && !(*pte & PTE_PRESENT) && (*pte & PTE_DEMAND)) {
        uint64_t new_phys = pmm_alloc_page();
        if (!new_phys) return false;

        uint64_t hhdm = vmm_hhdm_offset();
        memset((void *)(new_phys + hhdm), 0, PAGE_SIZE);

        *pte = new_phys | PTE_PRESENT | PTE_USER | PTE_WRITABLE;
        page_ref_inc(new_phys);
        invlpg(page_va);
        return true;
    }

    /* Case 2.5: Swap-in -- page was swapped out (PTE has PTE_SWAPPED bit) */
    if (pte && !(*pte & PTE_PRESENT) && (*pte & PTE_SWAPPED)) {
        int slot = swap_decode_pte(*pte);
        if (slot >= 0) {
            uint64_t new_phys;
            if (swap_in(slot, &new_phys) == 0) {
                *pte = new_phys | PTE_PRESENT | PTE_USER | PTE_WRITABLE;
                page_ref_inc(new_phys);
                invlpg(page_va);
                return true;
            }
        }
        return false;
    }

    /* Case 3: VMA-backed fault -- address is in a VMA but not mapped */
    struct vm_space *vms = vm_space_for_proc(p);
    struct vma *v = vma_find(vms, fault_addr);
    if (v) {
        /* Check write permission */
        if ((error_code & PF_ERR_WRITE) && !(v->flags & VMA_WRITE))
            return false;

        /* Already-present page in a VMA = a pure PERMISSION fault (e.g. a
         * write to a read-only-but-writable-VMA page). The mapping code
         * below allocates a FRESH zero page and remaps -- which would
         * clobber the live page's contents and leak the old frame, and
         * (if the write bit still didn't take) re-fault forever. Fix it
         * in place: grant write on the existing PTE + invlpg, or refuse
         * (segfault) rather than resolve-without-fixing. This closes an
         * infinite resolved-fault loop avenue and never destroys data. */
        if (pte && (*pte & PTE_PRESENT)) {
            if ((error_code & PF_ERR_WRITE) && (v->flags & VMA_WRITE)) {
                *pte |= PTE_WRITABLE;
                invlpg(page_va);
                return true;
            }
            return false;   /* present + not a grantable write = real fault */
        }

        /* File-backed mmap: read page from disk */
        if (v->flags & VMA_FILE) {
            uint64_t new_phys = pmm_alloc_page();
            if (!new_phys) return false;

            uint64_t hhdm = vmm_hhdm_offset();
            memset((void *)(new_phys + hhdm), 0, PAGE_SIZE);

            /* TODO: read from file v->file_fd at offset
             * v->file_offset + (page_va - v->start) */

            uint32_t vmm_flags = VMM_USER;
            if (v->flags & VMA_WRITE) vmm_flags |= VMM_WRITE;
            if (!(v->flags & VMA_EXEC)) vmm_flags |= VMM_NX;

            uint64_t saved = vmm_set_editor_root(p->cr3);
            vmm_map(page_va, new_phys, PAGE_SIZE, vmm_flags);
            vmm_set_editor_root(saved);

            page_ref_inc(new_phys);
            return true;
        }

        /* Anonymous mapping: allocate zero page */
        uint64_t new_phys = pmm_alloc_page();
        if (!new_phys) return false;

        uint64_t hhdm = vmm_hhdm_offset();
        memset((void *)(new_phys + hhdm), 0, PAGE_SIZE);

        uint32_t vmm_flags = VMM_USER;
        if (v->flags & VMA_WRITE) vmm_flags |= VMM_WRITE;
        if (!(v->flags & VMA_EXEC)) vmm_flags |= VMM_NX;

        uint64_t saved = vmm_set_editor_root(p->cr3);
        vmm_map(page_va, new_phys, PAGE_SIZE, vmm_flags);
        vmm_set_editor_root(saved);

        page_ref_inc(new_phys);
        return true;
    }

    /* No VMA, no valid PTE -- segfault */
    return false;
}

/* Ensure [addr,addr+len) is present + WRITABLE in the current proc before the
 * kernel memcpy's into it (copy_to_user), demand-paging not-present pages and
 * resolving COW via the normal handlers. Returns 0 if the whole range is now
 * writable, -1 if any page is a genuine read-only / unmapped user page. Without
 * this, copy_to_user's memcpy #PF'd on a read-only user page (e.g. a syscall
 * out-pointer landing in chrome's rodata / RELRO) and, being unresolvable inside
 * the SMAP uaccess window, took the fatal kernel-fault path -> KERNEL PANIC.
 * Now copy_to_user returns -EFAULT instead, exactly like Linux's uaccess. */
int uaccess_prepare_write(uint64_t addr, uint64_t len) {
    struct proc *p = current_proc();
    if (!p || len == 0) return 0;
    uint64_t va  = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = addr + len;
    for (; va < end; va += PAGE_SIZE) {
        uint64_t *pte = get_pte(p->cr3, va);
        if (pte && (*pte & PTE_PRESENT) && (*pte & PTE_WRITABLE))
            continue;                                   /* already writable */
        uint64_t ec = PF_ERR_WRITE | PF_ERR_USER |
                      ((pte && (*pte & PTE_PRESENT)) ? PF_ERR_PRESENT : 0);
        if (page_fault_handler(va, ec, p))   continue;  /* demand/COW resolved */
        if (mmap_handle_page_fault(va, ec))  continue;
        {   /* slice 18 diagnostic: which syscall aimed at a read-only user buf */
            static int logged = 0;
            if (logged < 8) {
                logged++;
                kprintf("[uaccess] copy_to_user -> EFAULT: read-only/unmapped "
                        "user addr=0x%lx (page 0x%lx)\n",
                        (unsigned long)addr, (unsigned long)va);
                if (logged == 1) {
                    extern void lx_dump_recent_syscalls(void);
                    lx_dump_recent_syscalls();
                }
            }
        }
        return -1;                                      /* read-only / unmapped */
    }
    return 0;
}

/* ---- COW fork: mark both parent and child as copy-on-write ---- */

#define USER_HALF_END 0x0000800000000000ULL

int vmm_cow_fork(uint64_t parent_cr3, uint64_t child_cr3) {
    uint64_t hhdm = vmm_hhdm_offset();
    uint64_t *parent_pml4 = (uint64_t *)(parent_cr3 + hhdm);
    uint64_t *child_pml4  = (uint64_t *)(child_cr3 + hhdm);

    /* Walk PML4 entries 0..255 (user half) */
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(parent_pml4[i4] & PTE_PRESENT)) continue;

        uint64_t *parent_pdpt = (uint64_t *)(
            (parent_pml4[i4] & PTE_ADDR_MASK) + hhdm);

        /* Allocate child PDPT */
        uint64_t c_pdpt_phys = pmm_alloc_page();
        if (!c_pdpt_phys) return -1;
        uint64_t *child_pdpt = (uint64_t *)(c_pdpt_phys + hhdm);
        memset(child_pdpt, 0, PAGE_SIZE);
        child_pml4[i4] = c_pdpt_phys |
                         (parent_pml4[i4] & ~PTE_ADDR_MASK);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(parent_pdpt[i3] & PTE_PRESENT)) continue;
            if (parent_pdpt[i3] & (1ULL << 7)) continue; /* skip 1GiB pages */

            uint64_t *parent_pd = (uint64_t *)(
                (parent_pdpt[i3] & PTE_ADDR_MASK) + hhdm);

            /* Allocate child PD */
            uint64_t c_pd_phys = pmm_alloc_page();
            if (!c_pd_phys) return -1;
            uint64_t *child_pd = (uint64_t *)(c_pd_phys + hhdm);
            memset(child_pd, 0, PAGE_SIZE);
            child_pdpt[i3] = c_pd_phys |
                             (parent_pdpt[i3] & ~PTE_ADDR_MASK);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(parent_pd[i2] & PTE_PRESENT)) continue;
                if (parent_pd[i2] & (1ULL << 7)) continue; /* skip 2MiB pages */

                uint64_t *parent_pt = (uint64_t *)(
                    (parent_pd[i2] & PTE_ADDR_MASK) + hhdm);

                /* Allocate child PT */
                uint64_t c_pt_phys = pmm_alloc_page();
                if (!c_pt_phys) return -1;
                uint64_t *child_pt = (uint64_t *)(c_pt_phys + hhdm);
                memset(child_pt, 0, PAGE_SIZE);
                child_pd[i2] = c_pt_phys |
                               (parent_pd[i2] & ~PTE_ADDR_MASK);

                for (int i1 = 0; i1 < 512; i1++) {
                    if (!(parent_pt[i1] & PTE_PRESENT)) continue;

                    uint64_t pte_val = parent_pt[i1];
                    uint64_t phys = pte_val & PTE_ADDR_MASK;

                    if (pte_val & PTE_WRITABLE) {
                        /* Clear writable, set COW in parent */
                        parent_pt[i1] = (pte_val & ~PTE_WRITABLE) | PTE_COW;
                        /* Child gets same: not writable, COW set */
                        child_pt[i1] = (pte_val & ~PTE_WRITABLE) | PTE_COW;
                    } else {
                        /* Read-only: copy PTE as-is */
                        child_pt[i1] = pte_val;
                    }

                    /* Account BOTH owners of the now-shared page. Pages
                     * allocated at spawn/exec time predate the refcount
                     * system and sit untracked at 0, so the original
                     * single inc here produced refs == 1 -- the COW fault
                     * handler's sole-owner shortcut then made the SHARED
                     * page writable in place for parent and child alike
                     * (shared writable stacks: the post-fork procs
                     * scribbled over each other and jumped through
                     * corrupted return addresses). Bring an untracked
                     * page to 1 (the parent's own reference) before
                     * adding the child's. */
                    if (page_ref_get(phys) == 0)
                        page_ref_inc(phys);     /* parent's untracked ref */
                    page_ref_inc(phys);         /* child's new ref        */
                }
            }
        }
    }

    /* CRITICAL: flush the TLB. We just stripped PTE_WRITABLE from every
     * writable user PTE of the RUNNING parent, whose translations -- its
     * own stack above all -- are hot in this CPU's TLB with the old write
     * permission cached. Without a flush the parent keeps writing straight
     * through stale TLB entries after fork() returns: no #PF, no CoW copy,
     * and its writes land on the physical pages now shared with the child
     * (observed as the fork child "resuming" with the parent's later stack
     * frames -- its return addresses were overwritten under it). Reloading
     * CR3 flushes all non-global entries; user pages are non-global. Other
     * CPUs are safe without an IPI shootdown: the parent runs on exactly
     * one CPU (this one), and any future migration switches CR3 anyway
     * (proc_context_switch), which flushes that CPU's stale entries before
     * the parent can run there. (Inline asm rather than cpu.h's helpers --
     * this file defines its own invlpg, which collides with cpu.h's.) */
    {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }

    return 0;
}

/* ---- Initialization ---- */

void page_fault_init(void) {
    page_ref_init();
    memset(g_vm_spaces, 0, sizeof(g_vm_spaces));
    kprintf("page_fault: initialized (COW + demand paging ready)\n");
}
