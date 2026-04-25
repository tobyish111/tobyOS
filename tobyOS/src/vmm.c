/* vmm.c -- x86_64 4-level paging for the kernel half.
 *
 * Address translation we build:
 *
 *   virt[47:39]  -> PML4 entry  (top level, one 4 KiB page)
 *   virt[38:30]  -> PDPT entry
 *   virt[29:21]  -> PD   entry  (PS=1 here -> 2 MiB leaf)
 *   virt[20:12]  -> PT   entry  (4 KiB leaf)
 *   virt[11: 0]  -> page offset
 *
 * Page-table pages are themselves PMM pages, accessed via the HHDM
 * (so editing a table is just a normal kernel pointer write).
 *
 * vmm_init() builds a fresh PML4 with two regions populated:
 *   1. HHDM mirror at hhdm_offset .. hhdm_offset + highest_phys, in
 *      2 MiB pages. Covers everything Limine handed us via the memmap,
 *      so the framebuffer, PMM bitmap, heap arenas, and (critically)
 *      the bootloader-provided stack all keep resolving after CR3.
 *   2. Kernel image at virtual_base .. __kernel_end mapped to
 *      physical_base.., in 4 KiB pages. Same per-page layout the
 *      bootloader gave us; only the table is ours.
 *
 * Then we load CR3 and we're running on our own tables.
 *
 * Notes:
 *   - We enable EFER.NXE before any NX-bearing entry is created, so
 *     setting bit 63 doesn't #GP.
 *   - We never free intermediate tables on unmap. With a tiny set of
 *     mappings this just leaks one or two PT pages at most; once we
 *     have churn we'll add ref-counting.
 *   - All map / unmap touch only the PT / PD level; we never collapse
 *     or split mixed-granularity entries on the fly. If a caller asks
 *     for a 4 KiB unmap inside a 2 MiB leaf, we refuse rather than
 *     silently shattering the page.
 */

#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/limine.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/spinlock.h>

/* ---- architectural PTE bits ---- */

#define PTE_P    (1ULL << 0)
#define PTE_RW   (1ULL << 1)
#define PTE_US   (1ULL << 2)
#define PTE_PWT  (1ULL << 3)
#define PTE_PCD  (1ULL << 4)
#define PTE_A    (1ULL << 5)
#define PTE_D    (1ULL << 6)
#define PTE_PS   (1ULL << 7)
#define PTE_G    (1ULL << 8)
#define PTE_NX   (1ULL << 63)

/* Bits 12..51 hold the physical frame number << 12. Top 12 bits of a
 * canonical phys are zero on x86_64, so this mask grabs the address
 * cleanly without touching the flag bits. */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PAGE_2M        (2ULL * 1024 * 1024)
#define PAGE_2M_MASK   (PAGE_2M - 1)

/* ---- linker symbols (see linker.ld) ---- */

extern char __kernel_start[];
extern char __kernel_end[];
extern char __text_start[],   __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[],   __data_end[];

/* ---- Limine request: where the kernel was loaded physically ---- */

extern volatile struct limine_executable_address_request exec_addr_req;

/* ---- module state ---- */

typedef uint64_t pte_t;

/* The "active editing root": HHDM-virt pointer + phys of the PML4 that
 * vmm_map / vmm_unmap / vmm_protect / vmm_translate / vmm_dump walk.
 * Defaults to the kernel PML4 (set by vmm_init); milestone-5 process
 * setup temporarily swaps it via vmm_set_active_root. CR3 is unrelated;
 * see vmm_set_active_root for the rationale. */
static pte_t   *g_pml4;
static uint64_t g_pml4_phys;
/* Ground-truth kernel PML4 phys -- captured once at vmm_init and never
 * touched again. vmm_set_active_root(0) restores from this. */
static uint64_t g_kernel_pml4_phys;
static uint64_t g_hhdm;

/* Milestone 22 step 5: serialise page-table edits. The PML4/PDPT/PD/PT
 * tree is shared across CPUs (the kernel half is mirrored into every
 * user PML4 we ever build), so two CPUs concurrently extending the same
 * PDPT while heap arenas grow on one CPU and a driver maps DMA on
 * another would race on alloc_table() and the PTE writes. We grab this
 * lock around the body of every external map / unmap / protect call,
 * and around vmm_set_active_root since it also pokes g_pml4 / g_pml4_phys.
 *
 * vmm_translate is RW-ish but we keep it lock-free because (a) its
 * callers (page-fault handler, kfree arena lookup) need it during
 * IRQ context where we don't want to block, and (b) torn 64-bit reads
 * of a PTE are impossible on x86_64. Worst case a translate races a
 * concurrent unmap and we see a stale entry -- but the page-fault
 * path will just retry, and kfree's arena-lookup walk is only called
 * on a virt that the caller is trying to free, which is owned by the
 * caller. */
static spinlock_t g_vmm_lock = SPINLOCK_INIT;

/* ---- helpers ---- */

static inline pte_t *phys_to_table(uint64_t phys) {
    return (pte_t *)(phys + g_hhdm);
}

static uint64_t alloc_table(void) {
    uint64_t p = pmm_alloc_page();
    if (p == 0) {
        kpanic("vmm: PMM out of memory while allocating a page-table page");
    }
    memset(phys_to_table(p), 0, PAGE_SIZE);
    return p;
}

/* Translate the public flag bits into architectural PTE bits. PRESENT
 * is added unconditionally on every leaf. For non-leaf entries we
 * always set RW|P|(US if requested) so the leaf bits are the source of
 * truth -- this matches Intel's "most permissive at every level wins
 * for ANDed bits" rule. */
static uint64_t flags_to_leaf(uint32_t f) {
    uint64_t v = PTE_P;
    if (f & VMM_WRITE)   v |= PTE_RW;
    if (f & VMM_USER)    v |= PTE_US;
    if (f & VMM_NOCACHE) v |= PTE_PCD | PTE_PWT;
    if (f & VMM_NX)      v |= PTE_NX;
    return v;
}

static uint64_t flags_to_intermediate(uint32_t f) {
    uint64_t v = PTE_P | PTE_RW;            /* leaf decides actual writability */
    if (f & VMM_USER) v |= PTE_US;          /* leaf decides actual US */
    return v;
}

/* Walk one level. Returns the child table's HHDM-virt pointer, or NULL
 * if the entry was missing and `create` is false. */
static pte_t *next_table(pte_t *parent, size_t idx, uint32_t flags, bool create) {
    pte_t e = parent[idx];
    if (e & PTE_P) {
        if (e & PTE_PS) return 0;            /* huge leaf -- not a table */
        return phys_to_table(e & PTE_ADDR_MASK);
    }
    if (!create) return 0;
    uint64_t phys = alloc_table();
    parent[idx] = phys | flags_to_intermediate(flags);
    return phys_to_table(phys);
}

static inline size_t pml4_idx(uint64_t v) { return (v >> 39) & 0x1ff; }
static inline size_t pdpt_idx(uint64_t v) { return (v >> 30) & 0x1ff; }
static inline size_t pd_idx  (uint64_t v) { return (v >> 21) & 0x1ff; }
static inline size_t pt_idx  (uint64_t v) { return (v >> 12) & 0x1ff; }

/* ---- map paths ---- */

static bool map_4k(uint64_t virt, uint64_t phys, uint32_t flags) {
    pte_t *pdpt = next_table(g_pml4, pml4_idx(virt), flags, true);
    if (!pdpt) return false;
    pte_t *pd   = next_table(pdpt,  pdpt_idx(virt), flags, true);
    if (!pd)   return false;
    pte_t *pt   = next_table(pd,    pd_idx(virt),   flags, true);
    if (!pt)   return false;

    size_t i = pt_idx(virt);
    if (pt[i] & PTE_P) {
        kprintf("[vmm] WARN: map_4k: virt %p already mapped (entry=0x%lx)\n",
                (void *)virt, pt[i]);
    }
    pt[i] = (phys & PTE_ADDR_MASK) | flags_to_leaf(flags);
    invlpg(virt);
    return true;
}

static bool map_2m(uint64_t virt, uint64_t phys, uint32_t flags) {
    pte_t *pdpt = next_table(g_pml4, pml4_idx(virt), flags, true);
    if (!pdpt) return false;
    pte_t *pd   = next_table(pdpt,  pdpt_idx(virt), flags, true);
    if (!pd)   return false;

    size_t i = pd_idx(virt);
    if (pd[i] & PTE_P) {
        kprintf("[vmm] WARN: map_2m: virt %p already mapped (entry=0x%lx)\n",
                (void *)virt, pd[i]);
    }
    pd[i] = (phys & ~PAGE_2M_MASK) | flags_to_leaf(flags) | PTE_PS;
    invlpg(virt);
    return true;
}

bool vmm_map(uint64_t virt, uint64_t phys, size_t bytes, uint32_t flags) {
    if (bytes == 0) return true;

    uint64_t saved = spin_lock_irqsave(&g_vmm_lock);
    bool ok = true;
    if (flags & VMM_HUGE_2M) {
        if ((virt | phys | bytes) & PAGE_2M_MASK) {
            spin_unlock_irqrestore(&g_vmm_lock, saved);
            kprintf("[vmm] vmm_map: 2M alignment violation "
                    "virt=%p phys=%p bytes=0x%lx\n",
                    (void *)virt, (void *)phys, (unsigned long)bytes);
            return false;
        }
        for (size_t off = 0; off < bytes; off += PAGE_2M) {
            if (!map_2m(virt + off, phys + off, flags)) { ok = false; break; }
        }
        spin_unlock_irqrestore(&g_vmm_lock, saved);
        return ok;
    }

    if ((virt | phys | bytes) & (PAGE_SIZE - 1)) {
        spin_unlock_irqrestore(&g_vmm_lock, saved);
        kprintf("[vmm] vmm_map: 4K alignment violation "
                "virt=%p phys=%p bytes=0x%lx\n",
                (void *)virt, (void *)phys, (unsigned long)bytes);
        return false;
    }
    for (size_t off = 0; off < bytes; off += PAGE_SIZE) {
        if (!map_4k(virt + off, phys + off, flags)) { ok = false; break; }
    }
    spin_unlock_irqrestore(&g_vmm_lock, saved);
    return ok;
}

/* ---- unmap path ---- */

static bool unmap_one(uint64_t virt) {
    pte_t *pdpt = next_table(g_pml4, pml4_idx(virt), 0, false);
    if (!pdpt) return false;
    pte_t *pd   = next_table(pdpt,  pdpt_idx(virt), 0, false);
    if (!pd)   return false;

    pte_t pde = pd[pd_idx(virt)];
    if (!(pde & PTE_P)) return false;

    if (pde & PTE_PS) {
        /* 2 MiB leaf -- only allow unmap if the request is aligned to
         * the leaf. Refuse to silently shatter a huge page. */
        if (virt & PAGE_2M_MASK) {
            kprintf("[vmm] WARN: unmap of 4K virt %p inside 2M leaf -- skipped\n",
                    (void *)virt);
            return false;
        }
        pd[pd_idx(virt)] = 0;
        invlpg(virt);
        return true;
    }

    pte_t *pt = phys_to_table(pde & PTE_ADDR_MASK);
    size_t i  = pt_idx(virt);
    if (!(pt[i] & PTE_P)) return false;
    pt[i] = 0;
    invlpg(virt);
    return true;
}

bool vmm_unmap(uint64_t virt, size_t bytes) {
    if (bytes == 0) return true;
    if ((virt | bytes) & (PAGE_SIZE - 1)) {
        kprintf("[vmm] vmm_unmap: 4K alignment violation virt=%p bytes=0x%lx\n",
                (void *)virt, (unsigned long)bytes);
        return false;
    }
    uint64_t saved = spin_lock_irqsave(&g_vmm_lock);
    bool ok = true;
    for (size_t off = 0; off < bytes; off += PAGE_SIZE) {
        if (!unmap_one(virt + off)) ok = false;
    }
    spin_unlock_irqrestore(&g_vmm_lock, saved);
    return ok;
}

/* ---- protect ---- */

bool vmm_protect(uint64_t virt, size_t bytes, uint32_t flags) {
    if (bytes == 0) return true;
    if ((virt | bytes) & (PAGE_SIZE - 1)) {
        kprintf("[vmm] vmm_protect: 4K alignment violation virt=%p bytes=0x%lx\n",
                (void *)virt, (unsigned long)bytes);
        return false;
    }

    uint64_t leaf_flags = flags_to_leaf(flags);
    uint64_t saved = spin_lock_irqsave(&g_vmm_lock);
    bool ok = true;
    for (size_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t v = virt + off;
        pte_t *pdpt = next_table(g_pml4, pml4_idx(v), 0, false);
        if (!pdpt) { ok = false; continue; }
        pte_t *pd   = next_table(pdpt,  pdpt_idx(v), 0, false);
        if (!pd)   { ok = false; continue; }

        pte_t pde = pd[pd_idx(v)];
        if (!(pde & PTE_P)) { ok = false; continue; }
        if (pde & PTE_PS) {
            spin_unlock_irqrestore(&g_vmm_lock, saved);
            kpanic("vmm_protect: 2 MiB leaf at %p inside requested range "
                   "-- caller would have to shatter it", (void *)v);
        }

        pte_t *pt = phys_to_table(pde & PTE_ADDR_MASK);
        size_t i  = pt_idx(v);
        if (!(pt[i] & PTE_P)) { ok = false; continue; }
        pt[i] = (pt[i] & PTE_ADDR_MASK) | leaf_flags;
        invlpg(v);
    }
    spin_unlock_irqrestore(&g_vmm_lock, saved);
    return ok;
}

/* ---- translate / dump ---- */

uint64_t vmm_translate(uint64_t virt) {
    pte_t *pdpt = next_table(g_pml4, pml4_idx(virt), 0, false);
    if (!pdpt) return 0;
    pte_t *pd   = next_table(pdpt,  pdpt_idx(virt), 0, false);
    if (!pd)   return 0;
    pte_t pde = pd[pd_idx(virt)];
    if (!(pde & PTE_P)) return 0;
    if (pde & PTE_PS) {
        return (pde & ~PAGE_2M_MASK & PTE_ADDR_MASK) | (virt & PAGE_2M_MASK);
    }
    pte_t *pt = phys_to_table(pde & PTE_ADDR_MASK);
    pte_t pte = pt[pt_idx(virt)];
    if (!(pte & PTE_P)) return 0;
    return (pte & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

static void dump_entry(const char *level, size_t idx, pte_t e) {
    kprintf("  %-5s [%3lu] = 0x%016lx  %c%c%c%c%c%c\n",
            level, (unsigned long)idx, e,
            (e & PTE_P)  ? 'P' : '-',
            (e & PTE_RW) ? 'W' : '-',
            (e & PTE_US) ? 'U' : '-',
            (e & PTE_PS) ? 'H' : '-',
            (e & PTE_PCD)? 'C' : '-',
            (e & PTE_NX) ? 'X' : '-');
}

void vmm_dump(uint64_t virt) {
    kprintf("[vmm] walk virt=%p (pml4=%lu pdpt=%lu pd=%lu pt=%lu off=0x%lx)\n",
            (void *)virt,
            (unsigned long)pml4_idx(virt),
            (unsigned long)pdpt_idx(virt),
            (unsigned long)pd_idx(virt),
            (unsigned long)pt_idx(virt),
            (unsigned long)(virt & (PAGE_SIZE - 1)));

    pte_t e4 = g_pml4[pml4_idx(virt)];
    dump_entry("PML4", pml4_idx(virt), e4);
    if (!(e4 & PTE_P)) { kprintf("  -> not present\n"); return; }

    pte_t *pdpt = phys_to_table(e4 & PTE_ADDR_MASK);
    pte_t e3 = pdpt[pdpt_idx(virt)];
    dump_entry("PDPT", pdpt_idx(virt), e3);
    if (!(e3 & PTE_P)) { kprintf("  -> not present\n"); return; }
    if (e3 & PTE_PS)   { kprintf("  -> 1 GiB leaf\n");  return; }

    pte_t *pd = phys_to_table(e3 & PTE_ADDR_MASK);
    pte_t e2 = pd[pd_idx(virt)];
    dump_entry("PD",   pd_idx(virt), e2);
    if (!(e2 & PTE_P)) { kprintf("  -> not present\n"); return; }
    if (e2 & PTE_PS) {
        uint64_t phys = (e2 & ~PAGE_2M_MASK & PTE_ADDR_MASK) | (virt & PAGE_2M_MASK);
        kprintf("  -> 2 MiB leaf, phys=%p\n", (void *)phys);
        return;
    }

    pte_t *pt = phys_to_table(e2 & PTE_ADDR_MASK);
    pte_t e1 = pt[pt_idx(virt)];
    dump_entry("PT",   pt_idx(virt), e1);
    if (!(e1 & PTE_P)) { kprintf("  -> not present\n"); return; }
    uint64_t phys = (e1 & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
    kprintf("  -> 4 KiB leaf, phys=%p\n", (void *)phys);
}

uint64_t vmm_kernel_pml4_phys(void) { return g_kernel_pml4_phys; }

/* ---- per-process address space management (milestone 5) ----
 *
 * The "active" PML4 here is the one our editing code (vmm_map,
 * vmm_unmap, vmm_protect, ...) walks via HHDM. By default it's the
 * kernel PML4 (g_pml4 set by vmm_init). Process setup temporarily
 * swaps it to a freshly-allocated PML4 to install user-half mappings,
 * then swaps back. CR3 is unaffected -- only context-switch flips CR3.
 *
 * Sharing model: a user PML4 carries a copy of the kernel's top-half
 * entries (indices 256..511). Each entry points at the SAME PDPT page
 * the kernel PML4 uses, so any new kernel-half mapping the heap or
 * vmm_map paths install via the kernel PML4 also appears in every
 * live process's PML4 at the same time -- as long as no NEW top-level
 * (PML4) entry needs to be created. We don't allocate top-level
 * entries past init, so this invariant holds.
 *
 * Destruction walks ONLY entries 0..255 (user half). The shared
 * kernel-half PDPT pages are intentionally left alive. */

uint64_t vmm_create_user_pml4(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        kprintf("[vmm] create_user_pml4: PMM OOM\n");
        return 0;
    }
    pte_t *new_pml4 = phys_to_table(phys);
    memset(new_pml4, 0, PAGE_SIZE);

    /* Mirror the kernel half by entry-copy. Entries 256..511 reference
     * shared PDPT pages; their address+flags are identical between
     * the kernel PML4 and every per-process PML4. */
    for (size_t i = 256; i < 512; i++) {
        new_pml4[i] = g_pml4[i];
    }
    return phys;
}

uint64_t vmm_set_active_root(uint64_t pml4_phys) {
    uint64_t old = g_pml4_phys;
    if (pml4_phys == 0) pml4_phys = g_kernel_pml4_phys;

    /* Swap BOTH the editor pointer and CR3. The editor pointer is what
     * vmm_map / vmm_protect / etc. walk via HHDM; CR3 is what the CPU
     * uses to resolve the very next memory access. They must agree, or
     * the memcpy that elf_load_user does to populate a fresh user-half
     * page (just mapped into the new PML4) would walk the kernel PML4
     * and fault on a "missing" page.
     *
     * Writing CR3 flushes the non-global TLB. That's the price of
     * correctness for the brief window we use this call (process
     * creation only). Both source and destination PML4s share their
     * kernel-half (entries 256..511 reference the same PDPT pages),
     * so kernel code keeps executing across the swap. */
    g_pml4_phys = pml4_phys;
    g_pml4      = phys_to_table(pml4_phys);
    if (read_cr3() != pml4_phys) write_cr3(pml4_phys);
    return old;
}

/* Milestone 25A companion: editor-only swap. Used by syscall handlers
 * (proc_brk, future mmap, etc.) that need to install user-half mappings
 * into the calling process's PML4. On the SYSCALL entry path CR3 is
 * already that PML4 -- the CPU never changes CR3 across `syscall` and
 * we don't either -- so the *only* thing we need to do is point the
 * editor at the right PML4 long enough to walk/extend its tables. We
 * MUST NOT touch CR3, because:
 *
 *   - Restoring `old` (which is typically the kernel PML4 left over
 *     from spawn_internal's last swap) would clobber the user CR3
 *     and the next user-mode instruction fetch on `sysret` would
 *     fault (the kernel PML4's user half is empty).
 *
 *   - Even forward-direction we don't *need* to write CR3, since it
 *     already matches.
 *
 * Returns the previous editor root, just like vmm_set_active_root.
 * Pass 0 to mean "the kernel PML4". */
uint64_t vmm_set_editor_root(uint64_t pml4_phys) {
    uint64_t old = g_pml4_phys;
    if (pml4_phys == 0) pml4_phys = g_kernel_pml4_phys;
    g_pml4_phys = pml4_phys;
    g_pml4      = phys_to_table(pml4_phys);
    return old;
}

/* Recursive walk: free a non-leaf table (PDPT/PD/PT) along with all
 * its leaves and any subtables. `level` counts toward the leaf:
 *   level 3 -> we're freeing a PDPT (entries point at PDs)
 *   level 2 -> a PD (entries point at PTs *or* are PS=1 leaves)
 *   level 1 -> a PT (entries are leaves)
 *
 * `range_user` guards us against accidentally walking into kernel-half
 * tables -- only used at the very top, but we pass it down for
 * defensive symmetry. */
static void free_subtree(uint64_t table_phys, int level) {
    pte_t *t = phys_to_table(table_phys);
    for (size_t i = 0; i < 512; i++) {
        pte_t e = t[i];
        if (!(e & PTE_P)) continue;

        if (level == 1) {
            /* PT entry -- leaf 4 KiB page, free the data frame. */
            pmm_free_page(e & PTE_ADDR_MASK);
            continue;
        }
        if (e & PTE_PS) {
            /* PS=1 inside a PD = 2 MiB leaf -- shouldn't happen for
             * user-half mappings (we only mint 4 KiB user pages), but
             * if it does, free the leaf's 2 MiB region as 512 frames. */
            uint64_t base = e & ~PAGE_2M_MASK & PTE_ADDR_MASK;
            for (uint64_t off = 0; off < PAGE_2M; off += PAGE_SIZE) {
                pmm_free_page(base + off);
            }
            continue;
        }
        /* Intermediate table: recurse, then free the table page. */
        free_subtree(e & PTE_ADDR_MASK, level - 1);
        pmm_free_page(e & PTE_ADDR_MASK);
    }
}

void vmm_destroy_user_pml4(uint64_t pml4_phys) {
    if (pml4_phys == 0) return;
    pte_t *root = phys_to_table(pml4_phys);

    /* User half only: entries 0..255. Anything in 256..511 is shared
     * with the kernel PML4 and other processes; we must not touch it. */
    for (size_t i = 0; i < 256; i++) {
        pte_t e = root[i];
        if (!(e & PTE_P)) continue;
        free_subtree(e & PTE_ADDR_MASK, 3);   /* PML4 entry -> PDPT */
        pmm_free_page(e & PTE_ADDR_MASK);
        root[i] = 0;
    }
    /* Finally, the PML4 page itself. */
    pmm_free_page(pml4_phys);
}

/* ---- init ---- */

#define IA32_EFER         0xC0000080u
#define IA32_EFER_NXE     (1ULL << 11)

static void enable_nxe(void) {
    uint64_t efer = rdmsr(IA32_EFER);
    if (!(efer & IA32_EFER_NXE)) {
        wrmsr(IA32_EFER, efer | IA32_EFER_NXE);
    }
}

/* Decide whether a memmap entry should be mirrored into HHDM.
 * RESERVED is intentionally skipped: it usually points at MMIO holes or
 * very-high sparse regions (we saw a 12 GiB RESERVED entry at
 * 0xfd00000000 from QEMU). MMIO that the kernel actually wants will get
 * its own vmm_map call later; everything else is uninteresting and
 * mapping it would just waste PD pages. */
static bool memmap_entry_in_hhdm(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_NVS:
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
    case LIMINE_MEMMAP_FRAMEBUFFER:
        return true;
    default:
        return false;
    }
}

void vmm_init(struct limine_memmap_response *memmap) {
    if (!exec_addr_req.response) {
        kpanic("vmm_init: Limine executable_address request returned no response");
    }
    if (!memmap || memmap->entry_count == 0) {
        kpanic("vmm_init: empty memmap");
    }

    g_hhdm = pmm_hhdm_offset();

    enable_nxe();

    g_pml4_phys        = alloc_table();
    g_kernel_pml4_phys = g_pml4_phys;
    g_pml4             = phys_to_table(g_pml4_phys);

    /* ---- 1. mirror HHDM in 2 MiB pages ----
     *
     * Walk the memmap and mirror each interesting entry. Round each
     * region's base DOWN to a 2 MiB boundary and its end UP, then call
     * the 2 MiB map path. Overlap from rounding is harmless: vmm_map
     * will warn but the entries are identical (same phys -> same
     * canonical HHDM virt), so the second write is a no-op in effect.
     *
     * To keep the spam down we suppress the duplicate-mapping warnings
     * during this initial sweep by tracking what we've already covered
     * across entries with a simple high-water mark. */
    uint64_t hhdm_high = 0;
    uint64_t hhdm_bytes_total = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (!memmap_entry_in_hhdm(e->type)) continue;

        uint64_t base = e->base & ~PAGE_2M_MASK;
        uint64_t end  = (e->base + e->length + PAGE_2M - 1) & ~PAGE_2M_MASK;

        if (end <= hhdm_high) continue;            /* already covered */
        if (base < hhdm_high) base = hhdm_high;    /* skip the overlap */

        size_t bytes = (size_t)(end - base);
        if (!vmm_map(g_hhdm + base, base, bytes,
                     VMM_WRITE | VMM_NX | VMM_HUGE_2M)) {
            kpanic("vmm_init: HHDM mirror failed for phys %p..%p (type=%lu)",
                   (void *)base, (void *)end, (unsigned long)e->type);
        }
        hhdm_high = end;
        hhdm_bytes_total += bytes;
    }

    /* ---- 2. map the kernel image (4 KiB pages) ----
     *
     * Use the linker-provided virt range (4 KiB-aligned ends) and
     * Limine's reported physical base. We map the whole image RW; per-
     * section permissions (.text RX, .rodata R, .data RW NX) come in
     * milestone 3B. We deliberately leave NX off here because .text is
     * inside this range. */
    uint64_t kvirt_start = (uint64_t)__kernel_start;
    uint64_t kvirt_end   = (uint64_t)__kernel_end;
    uint64_t kphys_start = exec_addr_req.response->physical_base;
    uint64_t kvirt_base  = exec_addr_req.response->virtual_base;

    if (kvirt_base != kvirt_start) {
        kprintf("[vmm] WARN: linker __kernel_start=%p but Limine virtual_base=%p\n",
                (void *)kvirt_start, (void *)kvirt_base);
    }

    size_t image_size = (size_t)(kvirt_end - kvirt_start);
    if (!vmm_map(kvirt_start, kphys_start, image_size, VMM_WRITE)) {
        kpanic("vmm_init: failed to map kernel image (%lu bytes)",
               (unsigned long)image_size);
    }

    kprintf("[vmm] kernel image: virt %p..%p -> phys %p (%lu KiB)\n",
            (void *)kvirt_start, (void *)kvirt_end, (void *)kphys_start,
            (unsigned long)(image_size / 1024));
    kprintf("[vmm] hhdm mirror:  %lu MiB across memmap, top phys %p, 2M pages\n",
            (unsigned long)(hhdm_bytes_total / (1024 * 1024)),
            (void *)hhdm_high);
    kprintf("[vmm] new PML4 at phys %p, switching CR3...\n", (void *)g_pml4_phys);

    write_cr3(g_pml4_phys);

    kprintf("[vmm] CR3 switched. Running on tobyOS page tables.\n");
}

void vmm_init_and_test(struct limine_memmap_response *memmap) {
    vmm_init(memmap);

    /* ---- self-translate kernel symbols ---- */
    uint64_t v_text  = (uint64_t)__text_start;
    uint64_t v_data  = (uint64_t)__data_start;
    uint64_t p_text  = vmm_translate(v_text);
    uint64_t p_data  = vmm_translate(v_data);
    if (p_text == 0 || p_data == 0) {
        kpanic("vmm test: kernel symbol failed to translate "
               "(text=%p->%p data=%p->%p)",
               (void *)v_text, (void *)p_text, (void *)v_data, (void *)p_data);
    }
    kprintf("[vmm] test: __text_start %p -> phys %p\n", (void *)v_text, (void *)p_text);
    kprintf("[vmm] test: __data_start %p -> phys %p\n", (void *)v_data, (void *)p_data);

    /* ---- HHDM round-trip: pick a known PMM page, prove HHDM works ---- */
    uint64_t scratch_phys = pmm_alloc_page();
    if (scratch_phys == 0) kpanic("vmm test: pmm OOM");
    volatile uint64_t *hhdm_ptr =
        (volatile uint64_t *)pmm_phys_to_virt(scratch_phys);
    *hhdm_ptr = 0x5A5AA5A5DEADBEEFULL;
    if (*hhdm_ptr != 0x5A5AA5A5DEADBEEFULL) {
        kpanic("vmm test: HHDM read-back failed at virt %p", (void *)hhdm_ptr);
    }
    kprintf("[vmm] test: HHDM r/w through new PML4 at phys %p OK\n",
            (void *)scratch_phys);

    /* ---- mint a fresh mapping at a chosen test virt ----
     *
     * Pick a virt high enough to be obviously kernel-half but well
     * clear of both HHDM and the kernel image. 0xffff'c000_0000_0000
     * sits in the empty stretch between HHDM (..0xffff_8800_..) and
     * the kernel (0xffff_ffff_8000_0000). */
    const uint64_t test_va = 0xFFFFC00000000000ULL;
    if (vmm_translate(test_va) != 0) {
        kpanic("vmm test: %p was unexpectedly already mapped", (void *)test_va);
    }
    if (!vmm_map(test_va, scratch_phys, PAGE_SIZE, VMM_WRITE | VMM_NX)) {
        kpanic("vmm test: vmm_map(test_va) failed");
    }
    uint64_t resolved = vmm_translate(test_va);
    if (resolved != scratch_phys) {
        kpanic("vmm test: translate after map: got %p expected %p",
               (void *)resolved, (void *)scratch_phys);
    }

    /* The page we mapped at test_va is the same physical page we wrote
     * via HHDM above; reading at test_va should see the same magic. */
    volatile uint64_t *fresh = (volatile uint64_t *)test_va;
    if (*fresh != 0x5A5AA5A5DEADBEEFULL) {
        kpanic("vmm test: read at test_va got 0x%lx, expected 0x5A5AA5A5DEADBEEF",
               *fresh);
    }
    *fresh = 0x1122334455667788ULL;
    if (*hhdm_ptr != 0x1122334455667788ULL) {
        kpanic("vmm test: write at test_va not visible via HHDM (got 0x%lx)",
               *hhdm_ptr);
    }
    kprintf("[vmm] test: new mapping at %p <-> phys %p verified\n",
            (void *)test_va, (void *)scratch_phys);

    /* ---- unmap and confirm it really went away ---- */
    if (!vmm_unmap(test_va, PAGE_SIZE)) {
        kpanic("vmm test: vmm_unmap(test_va) failed");
    }
    if (vmm_translate(test_va) != 0) {
        kpanic("vmm test: translate after unmap returned non-zero");
    }
    pmm_free_page(scratch_phys);
    kprintf("[vmm] test: unmap clears translation, scratch page freed\n");
    kprintf("[vmm] test: ok\n");
}
