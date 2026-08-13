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
#include <tobyos/panic.h>
#include <tobyos/klibc.h>
#include <tobyos/swap.h>
#include <tobyos/cgroup.h>   /* swap-in must re-charge residency */
#include <tobyos/mmap.h>     /* reclaim_stat_* live alongside mem_reclaim_pages */
#include <tobyos/apic.h>     /* tlb_shootdown_remote */
#include <tobyos/cgroup.h>   /* slice 16: the user-page charge funnel */

/* One refcount slot per MANAGED physical frame, sized from the PMM at
 * init. This used to be a fixed 256K-slot static array (1 GiB of
 * coverage) whose indexer MASKED the frame number with (max-1): on the
 * 4 GiB chromium guest every frame above 1 GiB silently SHARED a slot
 * with three other frames. Fork and teardown then entangled unrelated
 * frames' counts: an exiting process whose frame aliased a live CoW
 * frame's slot stole that slot's references on teardown, so a LATER
 * teardown of the CoW frame's first owner saw refs==1 and freed a
 * frame the second owner still mapped. The PMM recycled it into the
 * next allocation, which scribbled over live memory of an unrelated
 * process -- flaky, load-dependent corruption that only chromium's
 * multi-GiB fork-churn footprint ever triggered (slice 38: ICU locale
 * strings appearing inside the browser's vulkan-loader heap). */
static uint16_t *g_page_refcounts;

/* Pages brought back from swap. Paired with reclaim_stat_evicted(): the
 * reclaim harness requires BOTH to be non-zero, so a run that evicted nothing
 * (or evicted but never faulted anything back) cannot report a pass. */
static uint64_t g_swapin_count;
uint64_t reclaim_stat_faulted_back(void) { return g_swapin_count; }
static size_t    g_page_ref_slots;

/* Per-process vm_space table, indexed by pid */
static struct vm_space g_vm_spaces[PROC_MAX];

/* ---- Page reference counting ---- */

void page_ref_init(void) {
    size_t frames = pmm_total_pages();
    size_t bytes  = frames * sizeof(uint16_t);
    size_t pages  = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys)
        kpanic("page_ref_init: no memory for page refcount array");
    g_page_refcounts = (uint16_t *)pmm_phys_to_virt(phys);
    memset(g_page_refcounts, 0, pages * PAGE_SIZE);
    g_page_ref_slots = frames;
    kprintf("[cow] page refcounts: %lu frames tracked (%lu KiB array)\n",
            (unsigned long)frames,
            (unsigned long)((pages * PAGE_SIZE) / 1024));
}

void page_ref_inc(uint64_t phys) {
    size_t idx = phys >> 12;
    if (g_page_refcounts && idx < g_page_ref_slots &&
        g_page_refcounts[idx] < 0xFFFF)
        g_page_refcounts[idx]++;
}

void page_ref_dec(uint64_t phys) {
    size_t idx = phys >> 12;
    if (g_page_refcounts && idx < g_page_ref_slots &&
        g_page_refcounts[idx] > 0)
        g_page_refcounts[idx]--;
}

int page_ref_get(uint64_t phys) {
    size_t idx = phys >> 12;
    if (!g_page_refcounts || idx >= g_page_ref_slots) return 0;
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
    /* Slice 16: NOT charged, and that is the correct answer, not an omission.
     *
     * Breaking CoW is charge-NEUTRAL for this process: it already paid for this
     * page (the fork paths bulk-charge the child for every inherited page), and
     * here it exchanges its share of `old_phys` for a private `new_phys`. Charging
     * again counts the same page twice.
     *
     * Charging it here was tried, and bit7 of linux-cgroup2 caught it as a steady
     * 2-pages-per-fork-cycle drift in memory.current -- because the old page is
     * released below via page_ref_dec/pmm_free_page, a path with no uncharge. The
     * leak was invisible in every other bit. */
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

    /* Slice 88: serialize with CoW-fork STW -- do not promote/copy pages
     * while a sibling is mid vmm_cow_fork WP walk. Slice 92: the wait MUST
     * NOT hold the BKL (a kernel #PF from a syscall's user copy arrives
     * with it held; keeping it deadlocked the forker's mmap_cow_clone
     * bkl_enter = the -smp 4 freeze). vm_quiesce_park_oncpu drops it. */
    vm_quiesce_park_oncpu(p);

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
#ifdef CHROMIUM_BOOT
            { extern void pgj_note(uint8_t, uint64_t, uint64_t, uint64_t);
              pgj_note(5, page_va, old_phys, 0); }
#endif
            return true;
        }

        /* Multiple references: must copy */
        if (!cow_copy_page(old_phys, pte))
            return false; /* OOM during COW */

        invlpg(page_va);
        /* Frame CHANGE: any other CPU with the old translation cached would
         * keep reading/writing the pre-copy frame (now owned by the fork
         * sibling, or freed). One of this process's ~30 sibling threads can
         * be running that VA on another core right now. */
        tlb_shootdown_remote();
#ifdef CHROMIUM_BOOT
        { extern void pgj_note(uint8_t, uint64_t, uint64_t, uint64_t);
          pgj_note(6, page_va, *pte & PTE_ADDR_MASK, old_phys); }
#endif
        return true;
    }

    /* Case 2: Demand-zero fault -- PTE marked DEMAND but not present */
    if (pte && !(*pte & PTE_PRESENT) && (*pte & PTE_DEMAND)) {
        /* Slice 16: charged -- this is THE demand-fault path for a PTE_DEMAND
         * mapping, and it is where a growing process actually gets its memory. */
        uint64_t new_phys = mm_user_page_alloc(p);
        if (!new_phys) return false;

        uint64_t hhdm = vmm_hhdm_offset();
        memset((void *)(new_phys + hhdm), 0, PAGE_SIZE);

        *pte = new_phys | PTE_PRESENT | PTE_USER | PTE_WRITABLE;
        page_ref_inc(new_phys);
        invlpg(page_va);
#ifdef CHROMIUM_BOOT
        { extern void pgj_note(uint8_t, uint64_t, uint64_t, uint64_t);
          pgj_note(7, page_va, new_phys, 0); }
#endif
        return true;
    }

    /* Case 2.5: Swap-in -- page was swapped out (PTE has PTE_SWAPPED bit) */
    if (pte && !(*pte & PTE_PRESENT) && (*pte & PTE_SWAPPED)) {
        int slot = swap_decode_pte(*pte);
        if (slot >= 0) {
            /* Charge BEFORE bringing the page back. Reclaim uncharged this
             * page when it evicted it (mmap.c's free batch), so residency has
             * to be paid for again here -- otherwise every evict/fault-back
             * cycle ratchets the process's counter down and memory.max stops
             * meaning anything. Refusal is a fault failure, which is the same
             * way memory.max is enforced on every other allocating fault. */
            if (!mm_user_page_charge_existing(p))
                return false;
            uint64_t new_phys;
            if (swap_in(slot, &new_phys) == 0) {
                *pte = new_phys | PTE_PRESENT | PTE_USER | PTE_WRITABLE;
                page_ref_inc(new_phys);
                invlpg(page_va);
                g_swapin_count++;
                return true;
            }
            /* Page did not come back -- do not keep the charge. */
            mm_user_page_uncharge_existing(p);
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
#ifdef CHROMIUM_BOOT
                { extern void pgj_note(uint8_t, uint64_t, uint64_t, uint64_t);
                  pgj_note(8, page_va, *pte & PTE_ADDR_MASK,
                           page_ref_get(*pte & PTE_ADDR_MASK)); }
#endif
                return true;
            }
            return false;   /* present + not a grantable write = real fault */
        }

        /* File-backed mmap: read page from disk */
        if (v->flags & VMA_FILE) {
            uint64_t new_phys = mm_user_page_alloc(p);   /* slice 16: charged */
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
        uint64_t new_phys = mm_user_page_alloc(p);       /* slice 16: charged */
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

    /* Case N: grow-down stack expansion.
     *
     * execve maps only USER_STACK_PAGES (8 pages = 32 KiB) eagerly and nothing
     * grew it, so any process needing a deeper stack simply ran off the bottom
     * into unmapped space and took a fatal #PF. Real Linux maps the stack
     * grow-down and expands it on a fault below the current base, up to
     * RLIMIT_STACK (8 MiB by default). Chrome's child processes blow well past
     * 32 KiB -- they faulted at exactly stack_top-0x7f48 with "NOT covered by
     * any mmap-VMA" -- so every one of them died.
     *
     * Expand lazily instead of eagerly reserving 8 MiB per process: chrome runs
     * many processes and most touch only a few pages. Guard tightly -- the
     * address must sit below the current stack base, within the growth limit,
     * and be a USER access -- so this can never resurrect an unrelated wild
     * pointer as "stack". */
    if ((error_code & PF_ERR_USER) && p->user_stack_base &&
        page_va <  p->user_stack_base &&
        page_va >= USER_STACK_FLOOR_VA) {

        /* Map EVERY page from the fault up to the current base -- like
         * Linux's expand_stack, which extends the VMA to COVER the fault,
         * not just the one page. Mapping only page_va and dropping base to
         * it left HOLES: a function prologue that drops RSP by more than a
         * page (chrome's renderer has multi-KiB frames) faults on its
         * LOWEST touch first, base moved below the untouched pages in
         * between, and the very next write to one of them was "above base"
         * -- unresolvable -> SIGSEGV at stack_top-0xDFF0 with 8 mapped
         * pages above it (slice 34, first post-shm-fix renderer crash). */
        uint64_t saved = vmm_set_editor_root(p->cr3);
        bool ok = true;
        for (uint64_t va = page_va; va < p->user_stack_base; va += PAGE_SIZE) {
            /* Defensive: skip anything already present (shouldn't happen --
             * everything below base is by construction unmapped). */
            uint64_t *epte = get_pte(p->cr3, va);
            if (epte && (*epte & PTE_PRESENT)) continue;

            uint64_t new_phys = mm_user_page_alloc(p);   /* slice 16: charged */
            if (!new_phys) { ok = false; break; }
            memset((void *)(new_phys + vmm_hhdm_offset()), 0, PAGE_SIZE);
            if (!vmm_map(va, new_phys, PAGE_SIZE,
                         VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_USER)) {
                mm_user_page_free(p, new_phys);          /* slice 16 */
                ok = false;
                break;
            }
            page_ref_inc(new_phys);
            p->user_stack_pages++;
            invlpg(va);
        }
        vmm_set_editor_root(saved);
        if (!ok) return false;

        /* The stack now reaches this far down, with no holes above. */
        p->user_stack_base = page_va;
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
/* Read-side twin of uaccess_prepare_write. copy_from_user used to do only a
 * RANGE check and rely on faults inside the stac window being resolved by the
 * demand/COW handler -- which is true for a lazily-backed page, but NOT for an
 * address covered by no VMA at all. There is no exception-fixup table, so such
 * a fault is an unhandled kernel #PF: an instant PANIC that any user process
 * can trigger with a bad pointer. (Found via isr.c's own user-stack dump, which
 * get_user_u64's its way along a faulting thread's rsp -- when that rsp was
 * unmapped, the crash DIAGNOSTIC killed the kernel.) Probe first and let the
 * caller return -EFAULT instead, which is what Linux does. */
/* Pure residency probe: is every page of [addr,addr+len) ALREADY present?
 * Unlike uaccess_prepare_read this never calls the fault handlers, so it is
 * safe in contexts where resolving a fault would be re-entrant or fatal --
 * above all inside exception handling itself, where isr.c dumps a faulting
 * thread's user stack. (Resolving from there re-enters the fault machinery
 * during a fault; that is how the crash DIAGNOSTIC came to panic the kernel.)
 * This is Linux's copy_from_user_nofault distinction. */
int uaccess_probe_resident(uint64_t addr, uint64_t len) {
    struct proc *p = current_proc();
    if (!p || len == 0) return 0;
    uint64_t va  = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = addr + len;
    for (; va < end; va += PAGE_SIZE) {
        uint64_t *pte = get_pte(p->cr3, va);
        if (!pte || !(*pte & PTE_PRESENT)) return -1;
    }
    return 0;
}

/* Pure residency+writability probe: is every page of [addr,addr+len) ALREADY
 * present AND writable? Like uaccess_probe_resident this NEVER calls the fault
 * handlers and NEVER mutates the address space, so it is safe to call without
 * the BKL and concurrently across CPUs -- it only reads page-table entries.
 * Used by the lock-free time-syscall fast path (clock_gettime storm under
 * WHPX): if the tiny result buffer is already writable we can copy it without
 * taking the BKL at all; if not, the caller falls back to the normal, BKL-held
 * copy_to_user path that is allowed to fault/allocate. */
int uaccess_probe_writable(uint64_t addr, uint64_t len) {
    struct proc *p = current_proc();
    if (!p || len == 0) return 0;
    uint64_t va  = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = addr + len;
    for (; va < end; va += PAGE_SIZE) {
        uint64_t *pte = get_pte(p->cr3, va);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_WRITABLE)) return -1;
    }
    return 0;
}

int uaccess_prepare_read(uint64_t addr, uint64_t len) {
    struct proc *p = current_proc();
    if (!p || len == 0) return 0;
    uint64_t va  = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = addr + len;
    for (; va < end; va += PAGE_SIZE) {
        uint64_t *pte = get_pte(p->cr3, va);
        if (pte && (*pte & PTE_PRESENT)) continue;      /* readable already */
        uint64_t ec = PF_ERR_USER;                      /* read, not-present */
        if (page_fault_handler(va, ec, p))   continue;  /* demand/COW resolved */
        if (mmap_handle_page_fault(va, ec))  continue;
        return -1;                                      /* unmapped -> EFAULT */
    }
    return 0;
}

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
        {   /* Diagnostic: a syscall aimed copy_to_user at a read-only/unmapped
             * user page. NOTE (slice 19): the recurring hit here on chrome is
             * BENIGN and correct -- Chromium's base::ProtectedMemory verifies its
             * function-pointer table is read-only by issuing a SYSCALL write
             * (prlimit64 old_limit=&protected) and expecting EFAULT (a CPU write
             * would SIGSEGV; a syscall write cleanly returns -EFAULT). Returning
             * -EFAULT here is the correct Linux behaviour, not a bug to fix.
             * Dump the raw PTE to distinguish present-read-only from unmapped. */
            static int logged = 0;
            if (logged < 8) {
                logged++;
                uint64_t praw = pte ? *pte : 0;
                kprintf("[uaccess] copy_to_user -> EFAULT: addr=0x%lx page=0x%lx "
                        "pte=0x%lx [%s%s%s]\n",
                        (unsigned long)addr, (unsigned long)va,
                        (unsigned long)praw,
                        (praw & PTE_PRESENT)  ? "P" : "-",
                        (praw & PTE_WRITABLE) ? "W" : "r",
                        (praw & PTE_USER)     ? "U" : "k");
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

#ifdef CHROMIUM_BOOT
                    uint64_t fork_va = ((uint64_t)i4 << 39) |
                                       ((uint64_t)i3 << 30) |
                                       ((uint64_t)i2 << 21) |
                                       ((uint64_t)i1 << 12);
#endif
                    if (pte_val & PTE_SHARED) {
                        /* MAP_SHARED page (shm cache / memfd): NEVER CoW.
                         * POSIX: fork children share these mappings for
                         * real -- parent and child keep writing the SAME
                         * frame and see each other's writes. Write-
                         * protecting it here made the parent's FIRST
                         * post-fork write CoW-copy the page (refs>1 is
                         * guaranteed -- the shm cache itself holds a ref),
                         * silently diverging the browser's live ipcz
                         * NodeLinkMemory from every child: parcels the
                         * browser "sent" after the first fork landed in a
                         * private copy nobody else could see, children
                         * decoded stale fragment bytes, and Mojo killed
                         * the connection with ILLEGAL_MEMORY_RANGE (the
                         * slice-33/34 DOM blocker). Keep it writable in
                         * BOTH parent and child. */
                        child_pt[i1] = pte_val;
                        if (page_ref_get(phys) == 0)
                            page_ref_inc(phys);
                        page_ref_inc(phys);
                    } else {
#ifdef CHROMIUM_BOOT
                        /* Tier 2.5: EAGER private copy for EVERY non-SHARED
                         * present page (writable AND read-only). Sharing RO
                         * text/RELRO still couples parent refs to the
                         * crashpad child's teardown; a refcount slip there
                         * frees frames still mapped in chrome and shows up
                         * as GPU-thread jumps into .text padding. Parent
                         * PTEs stay as-is (no WP / no PTE_COW) — no stale
                         * WRITABLE-TLB window. RAM cost is brief: crashpad
                         * child execs/exits within milliseconds. */
                        uint64_t new_phys = pmm_alloc_page();
                        if (!new_phys) return -1;
                        memcpy((void *)(new_phys + hhdm),
                               (void *)(phys + hhdm), PAGE_SIZE);
                        child_pt[i1] = (pte_val & ~PTE_ADDR_MASK) | new_phys;
                        if (page_ref_get(new_phys) == 0)
                            page_ref_inc(new_phys);
                        { extern void pgj_note(uint8_t, uint64_t, uint64_t,
                                               uint64_t);
                          pgj_note(13, fork_va, new_phys, 1); }
#else
                        if (pte_val & PTE_WRITABLE) {
                            /* Clear writable, set COW in parent */
                            parent_pt[i1] = (pte_val & ~PTE_WRITABLE) | PTE_COW;
                            /* Child gets same: not writable, COW set */
                            child_pt[i1] = (pte_val & ~PTE_WRITABLE) | PTE_COW;
                            if (page_ref_get(phys) == 0)
                                page_ref_inc(phys);
                            page_ref_inc(phys);
                        } else {
                            /* Read-only page: shared + COW-marked. */
                            parent_pt[i1] = pte_val | PTE_COW;
                            child_pt[i1]  = pte_val | PTE_COW;
                            if (page_ref_get(phys) == 0)
                                page_ref_inc(phys);
                            page_ref_inc(phys);
                        }
#endif
                    }
                }
            }
        }
    }

#ifdef CHROMIUM_BOOT
    /* Eager full-copy leaves every parent PTE untouched — no TLB invalidate
     * required for the parent. Child installs a fresh CR3 on first run. */
#else
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

    /* The comment above ("the parent runs on exactly one CPU") is FALSE for
     * a multithreaded parent: chrome's browser process has ~30 threads, and
     * any of them may be running on another CPU with WRITABLE translations
     * to the pages we just write-protected hot in that CPU's TLB. Those
     * threads then keep writing straight into frames shared with the child
     * -- and worse, after a later CoW copy-out moves the PTE to a new frame,
     * a still-stale CPU keeps hitting the GHOST frame: chrome's SwiftShader
     * JIT thread lost 4 stack spills that way (slice 38 iter 24 page-journal
     * autopsy) and read back pre-fork bytes. Flush everyone.
     *
     * Tier 2.5: use BROADCAST sync (not cr3-filtered fire-and-forget).
     * Filtered shootdown skips CPUs whose current is idle/other even when
     * they still hold stale WRITABLE chrome translations from a just-parked
     * sibling — exactly the STW window. Broadcast + sync closes that hole. */
    if (!tlb_shootdown_remote_sync())
        tlb_shootdown_remote(); /* retry filtered as fallback */
#endif

    return 0;
}

/* ---- Initialization ---- */

void page_fault_init(void) {
    page_ref_init();
    memset(g_vm_spaces, 0, sizeof(g_vm_spaces));
    kprintf("page_fault: initialized (COW + demand paging ready)\n");
}
