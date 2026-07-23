/* mmap.c -- Advanced virtual memory: mmap/munmap/mprotect + demand paging.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * Implements per-process virtual memory areas (VMAs) with anonymous and
 * file-backed mappings. Provides:
 *   - sys_mmap / sys_mmap2: map anonymous or file-backed memory
 *   - sys_munmap / sys_munmap2: unmap a region
 *   - sys_mprotect / sys_mprotect2: change permissions
 *   - sys_brk2: extend/shrink heap
 *   - Demand paging via page fault handler (lazy allocation)
 *   - Copy-on-write (COW) support for fork()
 *
 * VMA list is a simple linear array per-process (sufficient for the
 * current workload of <64 mappings per process).
 */

#include <tobyos/proc.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/cpu.h>
#include <tobyos/sched.h>
#include <tobyos/mmap.h>
#include <tobyos/page_fault.h>

/* ---- VMA definitions ---- */

/* Chromium bring-up (slice 8): raised 256 -> 4096. Real chrome maps ~60 shared
 * libraries (each: a whole-file MAP_PRIVATE reservation + several MAP_FIXED
 * segment mappings), plus V8's cage arenas, ~17 thread stacks, and shared-memory
 * regions -- well over 256 live VMAs. At the 256 cap `vma_alloc` failed, mmap
 * returned -ENOMEM, and chrome CHECK-failed -> base::ImmediateCrash mid-startup
 * (the invariant "shared-memory" crash was really this: an mmap failing on a full
 * VMA table). Linux's default vm.max_map_count is 65530. This is a static array
 * per proc slot (~40 B/entry x PROC_MAX), so 4096 = ~42 MiB BSS. A future refactor
 * could make the table per-proc heap-allocated + have MAP_FIXED replace overlapped
 * VMAs (ld.so's segment maps currently each add an entry). */
#define VMA_MAX_PER_PROC  4096

#define VMA_PROT_READ   0x01
#define VMA_PROT_WRITE  0x02
#define VMA_PROT_EXEC   0x04
#define VMA_PROT_NONE   0x00

#define VMA_FLAG_ANON      0x01
#define VMA_FLAG_SHARED    0x02
#define VMA_FLAG_PRIVATE   0x04
#define VMA_FLAG_FIXED     0x08
#define VMA_FLAG_COW       0x10
/* The VMA's physical pages are owned by something else (a memfd) -- munmap must
 * unmap them but MUST NOT pmm_free them (the memfd frees them at close). */
#define VMA_FLAG_NOFREE    0x20

struct mmap_vma {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t flags;
    int      fd;
    uint64_t offset;
};

struct vma_table {
    struct mmap_vma entries[VMA_MAX_PER_PROC];
    int count;
    uint64_t mmap_hint;
    uint64_t brk_base;
    uint64_t brk_cur;
};

static struct vma_table g_vma_tables[PROC_MAX];

/* ---- Helpers ---- */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096ULL
#endif
/* PAGE_SIZE comes from pmm.h as `4096u` (32-bit unsigned), so the naive
 * `~(PAGE_SIZE - 1)` is a 32-bit `0xFFFFF000` that zero-extends to
 * `0x00000000FFFFF000` and TRUNCATES any address >= 4 GiB -- and the mmap
 * region lives at 16 TiB (MMAP_REGION_BASE). Force the mask 64-bit so
 * page_align_down/up keep the high bits (a real bug the Linux dynamic
 * loader's MAP_FIXED segment maps surfaced). */
#define PAGE_MASK (~((uint64_t)PAGE_SIZE - 1))

static inline uint64_t page_align_up(uint64_t v) {
    return (v + PAGE_SIZE - 1) & PAGE_MASK;
}

static inline uint64_t page_align_down(uint64_t v) {
    return v & PAGE_MASK;
}

/* User-half mmap region */
#define MMAP_REGION_BASE  0x0000100000000000ULL
#define MMAP_REGION_END   0x0000700000000000ULL

void mmap_init_proc(int pid) {
    struct vma_table *vt = &g_vma_tables[pid];
    memset(vt, 0, sizeof(*vt));
    vt->mmap_hint = MMAP_REGION_BASE;
}

void mmap2_init_proc(int pid) {
    mmap_init_proc(pid);
}

static struct mmap_vma *vma_find_internal(struct vma_table *vt, uint64_t addr) {
    for (int i = 0; i < vt->count; i++) {
        if (addr >= vt->entries[i].start && addr < vt->entries[i].end)
            return &vt->entries[i];
    }
    return NULL;
}

/* Chromium slice 17: on a fatal user #PF, report whether the mmap VMA table
 * covers the fault address (+ its prot/flags) and, if not, the nearest mappings.
 * Distinguishes "no VMA at all" (wild pointer / cage reservation gap) from
 * "VMA present but PROT_NONE / not writable" (an mprotect/commit not reflected
 * here). Uses the tgid table so a worker thread sees the process's mappings. */
void mmap_debug_fault_vma(uint64_t addr) {
    struct proc *p = current_proc();
    if (!p) return;
    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];
    struct mmap_vma *v = vma_find_internal(vt, addr);
    if (v) {
        kprintf("[pf] addr=0x%lx COVERED by mmap-VMA [0x%lx,0x%lx) prot=0x%x flags=0x%x\n",
                (unsigned long)addr, (unsigned long)v->start,
                (unsigned long)v->end, v->prot, v->flags);
        return;
    }
    kprintf("[pf] addr=0x%lx NOT covered by any mmap-VMA (%d total); nearest within 64GB:\n",
            (unsigned long)addr, vt->count);
    for (int i = 0; i < vt->count; i++) {
        uint64_t s = vt->entries[i].start, e = vt->entries[i].end;
        uint64_t d = (addr > s) ? (addr - s) : (s - addr);
        if (d < 0x1000000000ULL)
            kprintf("   [0x%lx,0x%lx) prot=0x%x flags=0x%x\n",
                    (unsigned long)s, (unsigned long)e,
                    vt->entries[i].prot, vt->entries[i].flags);
    }
}

static struct mmap_vma *vma_alloc(struct vma_table *vt) {
    if (vt->count >= VMA_MAX_PER_PROC) {
        kprintf("[mmap] WARN: VMA table FULL (%d entries) -- mmap will fail\n",
                vt->count);
        return NULL;
    }
    return &vt->entries[vt->count++];
}

static void vma_remove(struct vma_table *vt, int idx) {
    if (idx < 0 || idx >= vt->count) return;
    vt->count--;
    if (idx < vt->count)
        vt->entries[idx] = vt->entries[vt->count];
}

/* Natural alignment for a fresh mapping of `len` bytes. Linux mmap gives no
 * alignment guarantee beyond the page, but chrome's allocators DEMAND more
 * and probe for it: PartitionAlloc requires 2 MiB-aligned super-pages and
 * V8's pointer-compression cage a 4 GiB-aligned ~4 GiB reservation. Both
 * allocate, CHECK the result's alignment, munmap and retry on failure -- and
 * a bump allocator whose results are 4 KiB-aligned with a non-power-of-2
 * stride essentially NEVER passes the check, so the renderer looped
 * alloc/free-4GiB forever (slice 34: each giant munmap walked its pages
 * under the BKL, starving every other process -- observed as a full wedge
 * with the browser's own --timeout never firing). Aligning big requests
 * naturally costs a little VA in a 96 TiB window and makes the first probe
 * succeed. */
static uint64_t align_for_len(uint64_t len) {
    if (len >= 0x100000000ULL) return 0x100000000ULL;   /* >=4G: V8 cage    */
    if (len >= 0x200000ULL)    return 0x200000ULL;      /* >=2M: PA superpage */
    return PAGE_SIZE;
}

static uint64_t find_free_region(struct vma_table *vt, uint64_t len) {
    uint64_t align = align_for_len(len);
    uint64_t addr = vt->mmap_hint;
    if (addr < MMAP_REGION_BASE) addr = MMAP_REGION_BASE;
    addr = (addr + align - 1) & ~(align - 1);

    for (int iter = 0; iter < 1000; iter++) {
        if (addr + len > MMAP_REGION_END) {
            addr = (MMAP_REGION_BASE + align - 1) & ~(align - 1);
            continue;
        }
        bool conflict = false;
        for (int i = 0; i < vt->count; i++) {
            if (addr < vt->entries[i].end && (addr + len) > vt->entries[i].start) {
                addr = (page_align_up(vt->entries[i].end) + align - 1) &
                       ~(align - 1);
                conflict = true;
                break;
            }
        }
        if (!conflict) {
            vt->mmap_hint = addr + len;
            return addr;
        }
    }
    kprintf("[mmap] WARN: find_free_region FAILED len=0x%lx (no hole)\n",
            (unsigned long)len);
    return 0;
}

static uint32_t prot_to_vmm_flags(uint32_t prot) {
    uint32_t f = VMM_USER;
    if (prot & VMA_PROT_WRITE) f |= VMM_WRITE;
    if (!(prot & VMA_PROT_EXEC)) f |= VMM_NX;
    return f;
}

/* ---- sys_mmap (original API from vmm.h) ---- */

long sys_mmap(uint64_t addr, uint64_t len, uint32_t prot,
              uint32_t flags, int fd, uint64_t offset) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];

    if (len == 0) return -22;
    len = page_align_up(len);

    uint64_t base;
    if ((flags & VMA_FLAG_FIXED) && addr) {
        base = page_align_down(addr);
    } else {
        base = find_free_region(vt, len);
        if (!base) return -12;
    }

    struct mmap_vma *v = vma_alloc(vt);
    if (!v) return -12;

    v->start  = base;
    v->end    = base + len;
    v->prot   = prot;
    v->flags  = flags | VMA_FLAG_ANON;
    v->fd     = fd;
    v->offset = offset;

    /* For anonymous: eager-commit every page (compatible with existing code) --
     * EXCEPT a PROT_NONE mapping, which is an address-space RESERVATION, not
     * usable memory. V8's VirtualMemoryCage / PartitionAlloc reserve huge
     * PROT_NONE regions (e.g. 32 GiB) up front and commit sub-ranges later via
     * mprotect; eager-committing those pages instantly OOMs (8.3M pmm_alloc_page
     * for 32 GiB). Leave a PROT_NONE anon region UNCOMMITTED: the VMA is
     * recorded here, and mmap_handle_page_fault() demand-zero-fills on first
     * touch AFTER an mprotect has raised v->prot to a usable permission (a touch
     * while still PROT_NONE correctly can't be satisfied -> SIGSEGV). */
    if ((flags & VMA_FLAG_ANON) && prot != VMA_PROT_NONE) {
        uint32_t vmm_f = prot_to_vmm_flags(prot);
        uint64_t saved_root = vmm_set_editor_root(p->cr3);
        for (uint64_t a = base; a < base + len; a += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                kprintf("[mmap] WARN: pmm_alloc_page FAILED at a=0x%lx "
                        "(base=0x%lx len=0x%lx) -- OOM\n",
                        (unsigned long)a, (unsigned long)base,
                        (unsigned long)len);
                vmm_set_editor_root(saved_root);
                return -12;
            }
            memset((void *)(phys + vmm_hhdm_offset()), 0, PAGE_SIZE);
            vmm_map(a, phys, PAGE_SIZE, vmm_f);
        }
        vmm_set_editor_root(saved_root);
    }

    return (long)base;
}

/* ---- sys_mmap2 (new demand-paged API from mmap.h) ---- */

long sys_mmap2(uint64_t addr, size_t length, int prot, int flags,
               int fd, uint64_t offset) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];

    if (length == 0) return -22;
    length = page_align_up(length);

    uint64_t base;
    if ((flags & MAP_FIXED) && addr) {
        base = page_align_down(addr);
    } else {
        base = find_free_region(vt, length);
        if (!base) return -12;
    }

    struct mmap_vma *v = vma_alloc(vt);
    if (!v) return -12;

    v->start  = base;
    v->end    = base + length;
    v->prot   = 0;
    v->flags  = 0;
    v->fd     = -1;
    v->offset = offset;

    /* Convert PROT_* to internal prot bits */
    if (prot & PROT_READ)  v->prot |= VMA_PROT_READ;
    if (prot & PROT_WRITE) v->prot |= VMA_PROT_WRITE;
    if (prot & PROT_EXEC)  v->prot |= VMA_PROT_EXEC;

    if (flags & MAP_ANONYMOUS) v->flags |= VMA_FLAG_ANON;
    if (flags & MAP_PRIVATE)   v->flags |= VMA_FLAG_PRIVATE;
    if (flags & MAP_SHARED)    v->flags |= VMA_FLAG_SHARED;

    if (!(flags & MAP_ANONYMOUS)) {
        v->fd = fd;
    }

    /* Also register with page_fault vm_space for demand paging */
    struct vm_space *vms = vm_space_for_proc(p);
    if (vms) {
        uint32_t vma_flags = 0;
        if (prot & PROT_READ)  vma_flags |= VMA_READ;
        if (prot & PROT_WRITE) vma_flags |= VMA_WRITE;
        if (prot & PROT_EXEC)  vma_flags |= VMA_EXEC;
        if (flags & MAP_ANONYMOUS) vma_flags |= VMA_ANON;
        if (!(flags & MAP_ANONYMOUS)) vma_flags |= VMA_FILE;
        vma_add(vms, base, base + length, vma_flags);
    }

    /* Demand paging: DON'T allocate pages now. The page fault handler
     * will allocate zero-fill pages on first access. */

    return (long)base;
}

/* ---- munmap ---- */

long sys_munmap(uint64_t addr, uint64_t len) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];

    if (len == 0) return -22;
    addr = page_align_down(addr);
    len  = page_align_up(len);

    for (int i = 0; i < vt->count; i++) {
        struct mmap_vma *v = &vt->entries[i];
        if (addr >= v->end || (addr + len) <= v->start) continue;

        uint64_t saved_root = vmm_set_editor_root(p->cr3);
        uint64_t start = (addr > v->start) ? addr : v->start;
        uint64_t end   = ((addr + len) < v->end) ? (addr + len) : v->end;

        bool nofree = (v->flags & VMA_FLAG_NOFREE) != 0;   /* memfd-backed pages */
        for (uint64_t a = start; a < end; a += PAGE_SIZE) {
            uint64_t phys = vmm_translate(a);
            if (phys) {
                vmm_unmap(a, PAGE_SIZE);
                if (!nofree) pmm_free_page(phys & PAGE_MASK);
            }
        }
        vmm_set_editor_root(saved_root);

        if (addr <= v->start && (addr + len) >= v->end) {
            vma_remove(vt, i);
            i--;
        } else if (addr <= v->start) {
            v->start = addr + len;
        } else if ((addr + len) >= v->end) {
            v->end = addr;
        } else {
            struct mmap_vma *tail = vma_alloc(vt);
            if (tail) {
                tail->start  = addr + len;
                tail->end    = v->end;
                tail->prot   = v->prot;
                tail->flags  = v->flags;
                tail->fd     = v->fd;
                tail->offset = v->offset;
            }
            v->end = addr;
        }
    }

    return 0;
}

long sys_munmap2(uint64_t addr, size_t length) {
    return sys_munmap(addr, (uint64_t)length);
}

/* ---- mprotect ---- */

long sys_mprotect(uint64_t addr, uint64_t len, uint32_t prot) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];

    if (len == 0) return -22;
    addr = page_align_down(addr);
    len  = page_align_up(len);
    uint64_t rstart = addr, rend = addr + len;

    /* Apply `prot` ONLY to the [rstart,rend) sub-range, SPLITTING any VMA it
     * partially covers. The demand-page handler reads the VMA's prot for a
     * not-yet-present page, so without splitting an mprotect of a sub-range
     * (e.g. V8 making part of its 64MB heap read-only for W^X / a read-only
     * space) clobbered the WHOLE VMA's prot -- and a later WRITE to a different,
     * still-RW sub-range then demand-paged read-only and SIGSEGV'd. Iterate only
     * the pre-existing entries (n); the split tails we append keep their own
     * (correct) prot and don't need re-visiting. */
    int n = vt->count;
    for (int i = 0; i < n; i++) {
        struct mmap_vma *v = &vt->entries[i];
        if (rstart >= v->end || rend <= v->start) continue;   /* no overlap */
        uint64_t vs = v->start, ve = v->end;
        uint32_t oldprot = v->prot;

        if (rstart <= vs && rend >= ve) {
            v->prot = prot;                       /* fully covered */
        } else if (rstart <= vs) {                /* covers the left part */
            struct mmap_vma *tail = vma_alloc(vt);
            if (!tail) { v->prot = prot; continue; }   /* table full: degrade */
            v = &vt->entries[i];
            tail->start = rend; tail->end = ve; tail->prot = oldprot;
            tail->flags = v->flags; tail->fd = v->fd; tail->offset = v->offset;
            v->end = rend; v->prot = prot;
        } else if (rend >= ve) {                  /* covers the right part */
            struct mmap_vma *tail = vma_alloc(vt);
            if (!tail) { v->prot = prot; continue; }
            v = &vt->entries[i];
            tail->start = rstart; tail->end = ve; tail->prot = prot;
            tail->flags = v->flags; tail->fd = v->fd; tail->offset = v->offset;
            v->end = rstart;                      /* v keeps oldprot */
        } else {                                  /* covers a middle sub-range */
            struct mmap_vma *mid  = vma_alloc(vt);
            struct mmap_vma *tail = vma_alloc(vt);
            if (!mid || !tail) { v->prot = prot; continue; }
            v = &vt->entries[i];
            mid->start  = rstart; mid->end  = rend; mid->prot  = prot;
            mid->flags  = v->flags; mid->fd = v->fd; mid->offset = v->offset;
            tail->start = rend;  tail->end  = ve;   tail->prot = oldprot;
            tail->flags = v->flags; tail->fd = v->fd; tail->offset = v->offset;
            v->end = rstart;                      /* v keeps oldprot */
        }
    }

    uint32_t vmm_f = prot_to_vmm_flags(prot);
    uint64_t saved_root = vmm_set_editor_root(p->cr3);
    vmm_protect(addr, len, vmm_f);
    vmm_set_editor_root(saved_root);

    return 0;
}

long sys_mprotect2(uint64_t addr, size_t length, int prot) {
    uint32_t internal_prot = 0;
    if (prot & PROT_READ)  internal_prot |= VMA_PROT_READ;
    if (prot & PROT_WRITE) internal_prot |= VMA_PROT_WRITE;
    if (prot & PROT_EXEC)  internal_prot |= VMA_PROT_EXEC;
    return sys_mprotect(addr, (uint64_t)length, internal_prot);
}

/* ---- brk (heap management) ---- */

long sys_brk2(uint64_t new_brk) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];

    /* Query current brk */
    if (new_brk == 0)
        return (long)vt->brk_cur;

    new_brk = page_align_up(new_brk);

    if (new_brk < vt->brk_base)
        return -22;

    /* Grow: map new pages between old brk and new brk */
    if (new_brk > vt->brk_cur) {
        uint64_t saved_root = vmm_set_editor_root(p->cr3);
        for (uint64_t a = vt->brk_cur; a < new_brk; a += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                vmm_set_editor_root(saved_root);
                return -12;
            }
            memset((void *)(phys + vmm_hhdm_offset()), 0, PAGE_SIZE);
            vmm_map(a, phys, PAGE_SIZE, VMM_USER | VMM_WRITE);
        }
        vmm_set_editor_root(saved_root);
    }

    /* Shrink: unmap pages between new brk and old brk */
    if (new_brk < vt->brk_cur) {
        uint64_t saved_root = vmm_set_editor_root(p->cr3);
        for (uint64_t a = new_brk; a < vt->brk_cur; a += PAGE_SIZE) {
            uint64_t phys = vmm_translate(a);
            if (phys) {
                vmm_unmap(a, PAGE_SIZE);
                pmm_free_page(phys & PAGE_MASK);
            }
        }
        vmm_set_editor_root(saved_root);
    }

    vt->brk_cur = new_brk;
    return (long)new_brk;
}

/* ---- Page fault handler (original API) ---- */

bool mmap_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    struct proc *p = current_proc();
    if (!p) return false;

    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];
    struct mmap_vma *v = vma_find_internal(vt, fault_addr);
    if (!v) return false;

    /* vmm_map/vmm_unmap/vmm_translate edit the GLOBAL editor root (g_pml4_phys),
     * which between syscalls is the kernel PML4 -- NOT the faulting process. We
     * MUST retarget it to p->cr3 so the demand page lands in THIS process's
     * address space (page_fault.c's COW/demand path does exactly this). Without
     * it the page maps into the wrong PML4 and the process re-faults forever ->
     * SIGSEGV. Latent until a Linux process first demand-paged through here (V8's
     * PROT_NONE cage: reserve huge, mprotect a slice RW, touch it). */
    bool is_write = (error_code & 0x2) != 0;
    if (is_write && !(v->prot & VMA_PROT_WRITE)) {
        if (v->flags & VMA_FLAG_COW) {
            uint64_t page_va = page_align_down(fault_addr);
            uint64_t saved_root = vmm_set_editor_root(p->cr3);
            uint64_t old_phys = vmm_translate(page_va);
            if (!old_phys) { vmm_set_editor_root(saved_root); return false; }

            uint64_t new_phys = pmm_alloc_page();
            if (!new_phys) { vmm_set_editor_root(saved_root); return false; }

            memcpy((void *)(new_phys + vmm_hhdm_offset()),
                   (void *)(old_phys + vmm_hhdm_offset()),
                   PAGE_SIZE);

            uint32_t vmm_f = prot_to_vmm_flags(v->prot | VMA_PROT_WRITE);
            vmm_unmap(page_va, PAGE_SIZE);
            vmm_map(page_va, new_phys, PAGE_SIZE, vmm_f);
            vmm_set_editor_root(saved_root);

            v->flags &= ~VMA_FLAG_COW;
            v->prot |= VMA_PROT_WRITE;
            return true;
        }
        return false;
    }

    /* Demand paging: allocate a zero-filled physical page */
    uint64_t page_va = page_align_down(fault_addr);
    uint64_t phys = pmm_alloc_page();
    if (!phys) return false;

    memset((void *)(phys + vmm_hhdm_offset()), 0, PAGE_SIZE);

    uint32_t vmm_f = prot_to_vmm_flags(v->prot);
    /* A page inside a MAP_SHARED VMA must never be CoW'd at fork: whoever
     * shares the frame at fork time keeps sharing it. (Anon-shared pages the
     * parent faults in AFTER the fork are still per-process -- full anon
     * MAP_SHARED needs backing storage like the shm cache -- but pages that
     * exist at fork time now behave.) */
    if (v->flags & VMA_FLAG_SHARED) vmm_f |= VMM_SHARED;
    uint64_t saved_root = vmm_set_editor_root(p->cr3);
    vmm_map(page_va, phys, PAGE_SIZE, vmm_f);
    vmm_set_editor_root(saved_root);
    return true;
}

/* ---- COW fork helper (original API) ---- */

int mmap_cow_clone(int parent_pid, int child_pid) {
    struct vma_table *parent_vt = &g_vma_tables[parent_pid];
    struct vma_table *child_vt  = &g_vma_tables[child_pid];

    memcpy(child_vt, parent_vt, sizeof(*child_vt));

    for (int i = 0; i < child_vt->count; i++) {
        struct mmap_vma *v = &child_vt->entries[i];
        /* MAP_SHARED (and NOFREE cache/memfd-backed) regions must NEVER be
         * marked CoW: fork shares them for real -- both sides keep writing
         * the same frames (see PTE_SHARED in vmm_cow_fork). */
        if (v->flags & (VMA_FLAG_SHARED | VMA_FLAG_NOFREE)) continue;
        if (v->prot & VMA_PROT_WRITE) {
            v->flags |= VMA_FLAG_COW;
            if (i < parent_vt->count) {
                parent_vt->entries[i].flags |= VMA_FLAG_COW;
            }
        }
    }

    return 0;
}

int mmap2_cow_clone(int parent_pid, int child_pid) {
    return mmap_cow_clone(parent_pid, child_pid);
}

/* ---- Cleanup ---- */

void mmap_cleanup_proc(int pid) {
    struct vma_table *vt = &g_vma_tables[pid];
    vt->count = 0;
    vt->mmap_hint = MMAP_REGION_BASE;
    vt->brk_base = 0;
    vt->brk_cur = 0;
}

void mmap2_cleanup_proc(int pid) {
    mmap_cleanup_proc(pid);
}

/* ---- Query ---- */

uint64_t mmap2_mapped_bytes(int pid) {
    if (pid < 0 || pid >= PROC_MAX) return 0;
    struct vma_table *vt = &g_vma_tables[pid];
    uint64_t total = 0;
    for (int i = 0; i < vt->count; i++) {
        total += vt->entries[i].end - vt->entries[i].start;
    }
    return total;
}

/* ==================================================================
 * memfd -- anonymous, page-backed, mmap-COHERENT shared memory.
 * The memfd owns a growable list of physical pages; every mmap of the memfd
 * maps those SAME pages (real sharing), and read/write() touch them too. Pages
 * are freed only at the last close (memfd_unref); munmap leaves them
 * (VMA_FLAG_NOFREE). This is what chrome's compositor / *SharedMemoryRegion
 * needs -- file-backed mmap (linux_mmap_file) only copies, breaking sharing.
 * ================================================================== */

struct memfd {
    uint64_t *pages;    /* physical page addrs (PMM); index i == file page i */
    size_t    npages;   /* number of allocated pages (high-water) */
    size_t    cap;      /* capacity of pages[] */
    uint64_t  size;     /* logical size (ftruncate/write) */
    int       refs;     /* fd-level refcount (dup/clone) */
    unsigned  seals;    /* F_ADD_SEALS bits (accepted, informational) */
};

struct memfd *memfd_new(void) {
    struct memfd *mf = (struct memfd *)kmalloc(sizeof *mf);
    if (!mf) return 0;
    mf->pages = 0; mf->npages = 0; mf->cap = 0;
    mf->size = 0; mf->refs = 1; mf->seals = 0;
    return mf;
}

void memfd_ref(struct memfd *mf) { if (mf) mf->refs++; }

void memfd_unref(struct memfd *mf) {
    if (!mf) return;
    if (--mf->refs > 0) return;
    for (size_t i = 0; i < mf->npages; i++)
        if (mf->pages[i]) pmm_free_page(mf->pages[i]);
    if (mf->pages) kfree(mf->pages);
    kfree(mf);
}

/* Ensure the memfd owns at least `want` allocated (zero-filled) physical pages. */
static int memfd_ensure_pages(struct memfd *mf, size_t want) {
    if (want <= mf->npages) return 0;
    if (want > mf->cap) {
        size_t ncap = mf->cap ? mf->cap : 8;
        while (ncap < want) ncap *= 2;
        uint64_t *np = (uint64_t *)kmalloc(ncap * sizeof(uint64_t));
        if (!np) return -1;
        for (size_t i = 0; i < mf->npages; i++) np[i] = mf->pages[i];
        for (size_t i = mf->npages; i < ncap; i++) np[i] = 0;
        if (mf->pages) kfree(mf->pages);
        mf->pages = np; mf->cap = ncap;
    }
    for (size_t i = mf->npages; i < want; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -1;
        memset((char *)(phys + vmm_hhdm_offset()), 0, PAGE_SIZE);
        mf->pages[i] = phys;
    }
    mf->npages = want;
    return 0;
}

/* Linux file-seal bits (fcntl F_ADD_SEALS). */
#define MFD_SEAL_SEAL         0x0001u
#define MFD_SEAL_SHRINK       0x0002u
#define MFD_SEAL_GROW         0x0004u
#define MFD_SEAL_WRITE        0x0008u
#define MFD_SEAL_FUTURE_WRITE 0x0010u

long memfd_ftruncate(struct memfd *mf, uint64_t size) {
    if (!mf) return -1;
    if ((mf->seals & MFD_SEAL_SHRINK) && size < mf->size) return -1;  /* -EPERM */
    if ((mf->seals & MFD_SEAL_GROW)   && size > mf->size) return -1;
    size_t want = (size_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (memfd_ensure_pages(mf, want) < 0) return -12;   /* -ENOMEM */
    mf->size = size;
    return 0;
}

uint64_t memfd_size(struct memfd *mf) { return mf ? mf->size : 0; }

/* fcntl(F_ADD_SEALS): OR in the requested seals. Once F_SEAL_SEAL is set no
 * further seals may be added (-EPERM). */
long memfd_add_seals(struct memfd *mf, unsigned int seals) {
    if (!mf) return -1;
    if (mf->seals & MFD_SEAL_SEAL) return -1;   /* -EPERM: sealed against sealing */
    mf->seals |= seals;
    return 0;
}
long memfd_get_seals(struct memfd *mf) { return mf ? (long)mf->seals : 0; }

long memfd_read(struct memfd *mf, uint64_t pos, void *dst, size_t n) {
    if (!mf || !dst) return -1;
    if (pos >= mf->size) return 0;
    if (pos + n > mf->size) n = (size_t)(mf->size - pos);
    size_t done = 0;
    char *out = (char *)dst;
    while (done < n) {
        size_t pi = (size_t)((pos + done) / PAGE_SIZE);
        size_t po = (size_t)((pos + done) % PAGE_SIZE);
        size_t chunk = PAGE_SIZE - po;
        if (chunk > n - done) chunk = n - done;
        uint64_t phys = (pi < mf->npages) ? mf->pages[pi] : 0;
        if (phys) memcpy(out + done, (char *)(phys + vmm_hhdm_offset()) + po, chunk);
        else      memset(out + done, 0, chunk);     /* sparse hole -> zero */
        done += chunk;
    }
    return (long)done;
}

long memfd_write(struct memfd *mf, uint64_t pos, const void *src, size_t n) {
    if (!mf || !src) return -1;
    if (n == 0) return 0;
    size_t want = (size_t)((pos + n + PAGE_SIZE - 1) / PAGE_SIZE);
    if (memfd_ensure_pages(mf, want) < 0) return -12;
    size_t done = 0;
    const char *in = (const char *)src;
    while (done < n) {
        size_t pi = (size_t)((pos + done) / PAGE_SIZE);
        size_t po = (size_t)((pos + done) % PAGE_SIZE);
        size_t chunk = PAGE_SIZE - po;
        if (chunk > n - done) chunk = n - done;
        memcpy((char *)(mf->pages[pi] + vmm_hhdm_offset()) + po, in + done, chunk);
        done += chunk;
    }
    if (pos + n > mf->size) mf->size = pos + n;
    return (long)done;
}

long memfd_map(uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags,
               struct memfd *mf, uint64_t offset) {
    struct proc *p = current_proc();
    if (!p || !mf) return -1;
    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];
    if (len == 0) return -22;
    len = page_align_up(len);
    offset = page_align_down(offset);
    size_t page_off = (size_t)(offset / PAGE_SIZE);
    size_t np = (size_t)(len / PAGE_SIZE);
    if (memfd_ensure_pages(mf, page_off + np) < 0) return -12;

    uint64_t base;
    if ((flags & VMA_FLAG_FIXED) && addr) base = page_align_down(addr);
    else { base = find_free_region(vt, len); if (!base) return -12; }

    struct mmap_vma *v = vma_alloc(vt);
    if (!v) return -12;
    v->start = base; v->end = base + len; v->prot = prot;
    v->flags = (flags | VMA_FLAG_NOFREE) & ~(uint32_t)VMA_FLAG_ANON;
    v->fd = -1; v->offset = offset;

    /* VMM_SHARED: stamp PTE_SHARED so fork never write-protects these pages
     * for CoW -- every mapper (incl. a post-fork parent) must keep writing
     * the SAME physical frame. */
    uint32_t vmm_f = prot_to_vmm_flags(prot) | VMM_SHARED;
    uint64_t saved = vmm_set_editor_root(p->cr3);
    for (size_t i = 0; i < np; i++)
        vmm_map(base + i * PAGE_SIZE, mf->pages[page_off + i], PAGE_SIZE, vmm_f);
    vmm_set_editor_root(saved);
    return (long)base;
}

/* ==================================================================
 * MAP_SHARED file-backed page cache (slice 22).
 *
 * POSIX MAP_SHARED means every mapping of the same file region sees the SAME
 * memory. linux_mmap_file historically reserved ANONYMOUS pages and filled
 * them by reading the file, so two processes mapping one file each got a
 * PRIVATE COPY -- writes were invisible to the other side. That silently broke
 * every cross-process shared-memory user, and it is what stalled Chromium's
 * Mojo bootstrap: base/memory/platform_shared_memory_region_posix.cc creates
 * its regions as temp FILES (this build has no memfd path for them), so the
 * browser wrote into its copy and the child waited forever on its own.
 *
 * Fix: one page list per INODE, shared by every MAP_SHARED mapping of that
 * file -- structurally the same trick struct memfd already uses, keyed by
 * vfs_file.ino (stable per file, slice 14) instead of by fd. Keying on the
 * inode rather than the path is what makes it survive unlink(), which matters
 * because chrome unlinks its regions immediately after creating them.
 *
 * MAP_PRIVATE is untouched and still copies, which is correct.
 *
 * LIMITATION (deliberate, documented): entries are never reclaimed and there is
 * no writeback to disk -- the cache IS the file's contents for anyone who maps
 * it, but a later read() sees the on-disk bytes. Chrome's regions are anonymous
 * scratch that nobody read()s, so this is sound for the bring-up; a general
 * implementation needs msync/close writeback and eviction.
 * ================================================================== */

/* One entry per SHARED-MAPPED FILE, not per inode NUMBER. Chrome creates ~40
 * regions in a run and each needs its own entry now that they no longer
 * (wrongly) collapse onto a recycled inode number -- see shm_cache_detach_ino. */
#define SHMCACHE_MAX 256

struct shm_cache {
    bool      used;
    uint64_t  ino;          /* inode number, 0 == not keyed (fd-identity only) */
    uint64_t  gen;          /* which INCARNATION of that number (vfs_file.ino_gen) */
    uint64_t *pages;        /* physical page addrs; index i == file page i */
    size_t    npages;
    size_t    cap;
};

static struct shm_cache g_shm[SHMCACHE_MAX];

static struct shm_cache *shm_slot_alloc(uint64_t ino, uint64_t gen) {
    for (int i = 0; i < SHMCACHE_MAX; i++) {
        if (!g_shm[i].used) {
            g_shm[i].used = true;
            g_shm[i].ino  = ino;
            g_shm[i].gen  = gen;
            g_shm[i].pages = 0; g_shm[i].npages = 0; g_shm[i].cap = 0;
            return &g_shm[i];
        }
    }
    return 0;                                    /* table full -> caller copies */
}

/* Look up (or create) the shared page set for one FILE.
 *
 * The key is (ino, gen), never the inode NUMBER alone. tobyfs's alloc_inode
 * hands back the LOWEST free number, so a number is typically reissued on the
 * very next create -- and chrome creates each shared-memory region as a temp
 * file that it unlinks IMMEDIATELY, keeping only the fd. Keying on the bare
 * number therefore aliased six unrelated chrome regions onto ONE set of
 * physical pages (measured: ino 9 allocated 6x in a single boot and mapped at
 * four different sizes). Each new region then came up holding a previous
 * tenant's bytes: chrome's persistent_memory_allocator found a valid cookie
 * with an inconsistent header and logged "Corruption detected in shared-memory
 * segment", and the Mojo/field-trial regions handed children data the browser
 * had never written.
 *
 * gen==0 means the fs has no inode identity (ramfs et al.), so fall back to
 * fd-only identity -- the caller pins the result on the struct file. */
struct shm_cache *shm_cache_for_ino(uint64_t ino, uint64_t gen, bool *created) {
    if (created) *created = false;
    if (ino == 0 || gen == 0) return 0;
    for (int i = 0; i < SHMCACHE_MAX; i++)
        if (g_shm[i].used && g_shm[i].ino == ino && g_shm[i].gen == gen)
            return &g_shm[i];
    if (created) *created = true;
    return shm_slot_alloc(ino, gen);
}

/* NB: a file with no inode identity (gen==0) gets no region at all -- the mmap
 * path falls back to copying. See the call site in linux_mmap_file. */

int shm_cache_ensure(struct shm_cache *sc, size_t want) {
    if (!sc) return -1;
    if (want <= sc->npages) return 0;
    if (want > sc->cap) {
        size_t ncap = sc->cap ? sc->cap : 8;
        while (ncap < want) ncap *= 2;
        uint64_t *np = (uint64_t *)kmalloc(ncap * sizeof(uint64_t));
        if (!np) return -1;
        for (size_t i = 0; i < sc->npages; i++) np[i] = sc->pages[i];
        for (size_t i = sc->npages; i < ncap; i++) np[i] = 0;
        if (sc->pages) kfree(sc->pages);
        sc->pages = np; sc->cap = ncap;
    }
    for (size_t i = sc->npages; i < want; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -1;
        memset((char *)(phys + vmm_hhdm_offset()), 0, PAGE_SIZE);
        /* The CACHE itself holds one reference, so the page survives every
         * mapper exiting. Without this the refcount sat at 0 and
         * free_subtree() (vmm.c) freed the frame on the FIRST address-space
         * teardown -- then again for every other mapper: "[pmm] WARN: double
         * free of page", with the frame handed back out while live processes
         * were still writing through it. */
        page_ref_inc(phys);
        sc->pages[i] = phys;
    }
    sc->npages = want;
    return 0;
}

/* High-water page count, so a caller can tell which pages a given
 * shm_cache_ensure() call NEWLY allocated and populate exactly those. */
size_t shm_cache_npages(struct shm_cache *sc) { return sc ? sc->npages : 0; }

/* Kernel-virtual address of file page `idx`, for populating it from the file. */
void *shm_cache_page_ptr(struct shm_cache *sc, size_t idx) {
    if (!sc || idx >= sc->npages || !sc->pages[idx]) return 0;
    return (void *)(sc->pages[idx] + vmm_hhdm_offset());
}

/* Map [offset,offset+len) of the cache into the caller. Mirrors memfd_mmap:
 * the SAME physical pages are installed in every mapper's address space, which
 * is what makes writes mutually visible. VMA_FLAG_NOFREE so munmap unmaps but
 * never frees pages the cache owns. */
long shm_cache_mmap(struct shm_cache *sc, uint64_t addr, uint64_t len,
                    uint32_t prot, uint32_t flags, uint64_t offset) {
    struct proc *p = current_proc();
    if (!p || !sc) return -1;
    int pid = p->is_thread ? p->tgid : p->pid;
    struct vma_table *vt = &g_vma_tables[pid];
    if (len == 0) return -22;
    len = page_align_up(len);
    offset = page_align_down(offset);
    size_t page_off = (size_t)(offset / PAGE_SIZE);
    size_t np = (size_t)(len / PAGE_SIZE);
    if (shm_cache_ensure(sc, page_off + np) < 0) return -12;

    uint64_t base;
    if ((flags & VMA_FLAG_FIXED) && addr) base = page_align_down(addr);
    else { base = find_free_region(vt, len); if (!base) return -12; }

    struct mmap_vma *v = vma_alloc(vt);
    if (!v) return -12;
    v->start = base; v->end = base + len; v->prot = prot;
    v->flags = (flags | VMA_FLAG_NOFREE) & ~(uint32_t)VMA_FLAG_ANON;
    v->fd = -1; v->offset = offset;

    /* VMM_SHARED: stamp PTE_SHARED so fork never write-protects these pages
     * for CoW. Without it, the FIRST post-fork write through a pre-fork
     * mapping faulted, saw refs>1 (the cache's own ref guarantees that), and
     * silently diverted the writer onto a PRIVATE copy -- chrome's browser
     * process CoW-diverged its live ipcz NodeLinkMemory the moment it forked
     * a child and kept "writing" parcels nobody else could see (slice 34,
     * VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE). */
    uint32_t vmm_f = prot_to_vmm_flags(prot) | VMM_SHARED;
    uint64_t saved = vmm_set_editor_root(p->cr3);
    for (size_t i = 0; i < np; i++) {
        /* One reference per ADDRESS SPACE the page is mapped into, so this
         * process's teardown only drops its own (free_subtree decrements when
         * refs > 1 and frees only at the last). Paired with the cache's own
         * reference from shm_cache_ensure. */
        page_ref_inc(sc->pages[page_off + i]);
        vmm_map(base + i * PAGE_SIZE, sc->pages[page_off + i], PAGE_SIZE, vmm_f);
    }
    vmm_set_editor_root(saved);
    return (long)base;
}
