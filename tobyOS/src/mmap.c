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

static uint64_t find_free_region(struct vma_table *vt, uint64_t len) {
    uint64_t addr = vt->mmap_hint;
    if (addr < MMAP_REGION_BASE) addr = MMAP_REGION_BASE;

    for (int iter = 0; iter < 1000; iter++) {
        if (addr + len > MMAP_REGION_END) {
            addr = MMAP_REGION_BASE;
            continue;
        }
        bool conflict = false;
        for (int i = 0; i < vt->count; i++) {
            if (addr < vt->entries[i].end && (addr + len) > vt->entries[i].start) {
                addr = page_align_up(vt->entries[i].end);
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

        for (uint64_t a = start; a < end; a += PAGE_SIZE) {
            uint64_t phys = vmm_translate(a);
            if (phys) {
                vmm_unmap(a, PAGE_SIZE);
                pmm_free_page(phys & PAGE_MASK);
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

    for (int i = 0; i < vt->count; i++) {
        struct mmap_vma *v = &vt->entries[i];
        if (addr >= v->end || (addr + len) <= v->start) continue;
        v->prot = prot;
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
