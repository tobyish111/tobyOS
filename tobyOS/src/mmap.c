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
#include <tobyos/apic.h>     /* tlb_shootdown_remote */

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
/* Slice 57: 4096 -> 8192, plus merge-on-pressure compaction (vma_compact_
 * table below). Chrome's watch-page renderer SPLITS VMAs continuously
 * (PartitionAlloc decommit + V8 W^X mprotect churn) and nothing ever merged
 * them back: the table hit the 4096 cap ~29s into a YouTube watch run, after
 * which mmap failed and -- worse -- mprotect SPLITS failed silently, leaving
 * sub-ranges with no VMA; the next touch became a fatal "NOT covered by any
 * mmap-VMA" fault at a perfectly valid address (this was the run-10 "wild
 * jump" class: every such crash printed "(4096 total)"). */
#define VMA_MAX_PER_PROC  8192

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

#ifdef CHROMIUM_BOOT
/* Permission-transition ring (defined above sys_madvise_dontneed). */
void mprot_ring_note(uint64_t addr, uint64_t len, int pid, uint32_t prot);
#endif

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
/* Slice 108: what the HARDWARE sees at a fatal fault, next to what the VMA
 * table claims. The multi-process bootstrap flake kills a freshly forked
 * chrome child on an INSTRUCTION FETCH inside libc (err=0x14: not-present +
 * exec) at an address the VMA table reports as present and executable
 * (prot=0x5). Those two statements cannot both be true, and until now the
 * fatal path printed only the second. This prints the PTE, which
 * discriminates the remaining possibilities in one line:
 *   no mapping        -> the resolver never installed a page
 *   present, NX       -> installed without exec permission
 *   present, !user    -> installed without the user bit
 *   present and fine  -> a stale TLB / wrong address space
 * Read-only; it maps nothing and changes no state. */
void mmap_debug_fault_pte(uint64_t addr) {
    struct proc *p = current_proc();
    if (!p) return;
    uint64_t va = addr & ~0xfffULL;
    uint64_t saved = vmm_set_editor_root(p->cr3);
    uint64_t phys = vmm_translate(va);
    vmm_set_editor_root(saved);
    if (!phys) {
        kprintf("[pfpte] va=0x%lx cr3=0x%lx -> NO MAPPING (resolver never "
                "installed a page, or installed it elsewhere)\n",
                (unsigned long)va, (unsigned long)p->cr3);
        return;
    }
    kprintf("[pfpte] va=0x%lx cr3=0x%lx -> phys=0x%lx PRESENT (so the fault is "
            "a PERMISSION/TLB problem, not a missing page)\n",
            (unsigned long)va, (unsigned long)p->cr3, (unsigned long)phys);
}

void mmap_debug_fault_vma(uint64_t addr) {
    struct proc *p = current_proc();
    if (!p) return;
    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    /* Print EVERY covering entry, not just the first hit: overlapping VMAs
     * (two entries claiming one address, fault handler resolving against
     * whichever it scans first) are one of the candidate bug classes, and a
     * first-match print renders them invisible. */
    int hits = 0;
    for (int i = 0; i < vt->count; i++) {
        struct mmap_vma *v = &vt->entries[i];
        if (addr < v->start || addr >= v->end) continue;
        hits++;
        kprintf("[pf] addr=0x%lx COVERED by mmap-VMA%s [0x%lx,0x%lx) prot=0x%x flags=0x%x\n",
                (unsigned long)addr, hits > 1 ? " (OVERLAP!)" : "",
                (unsigned long)v->start,
                (unsigned long)v->end, v->prot, v->flags);
    }
    if (hits) return;
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

/* Slice 57: merge-on-pressure. When the table is >75% full, sort by start
 * and merge strictly-compatible ANON neighbours (same prot, same flags,
 * fd < 0, exactly adjacent). Chrome's mprotect/decommit churn generates
 * thousands of same-prot fragments; merging them makes the cap a soft
 * ceiling instead of a time bomb. Called ONLY at the top of the three
 * VMA syscalls, BEFORE any entry lookup, because it moves entries --
 * callers deeper in hold entry pointers across vma_alloc (split paths)
 * and must never see the table shuffle under them. Insertion sort: the
 * table is nearly sorted in practice, and this runs only under pressure. */
static void vma_compact_table(struct vma_table *vt, int pid) {
    if (vt->count < (VMA_MAX_PER_PROC * 3) / 4) return;
    int before = vt->count;
    for (int i = 1; i < vt->count; i++) {
        struct mmap_vma key = vt->entries[i];
        int j = i - 1;
        while (j >= 0 && vt->entries[j].start > key.start) {
            vt->entries[j + 1] = vt->entries[j];
            j--;
        }
        vt->entries[j + 1] = key;
    }
    int w = 0;
    for (int i = 0; i < vt->count; i++) {
        struct mmap_vma *cur = &vt->entries[i];
        if (w > 0) {
            struct mmap_vma *prev = &vt->entries[w - 1];
            if (prev->end == cur->start &&
                prev->prot  == cur->prot &&
                prev->flags == cur->flags &&
                prev->fd < 0 && cur->fd < 0) {
                prev->end = cur->end;
                continue;
            }
        }
        if (w != i) vt->entries[w] = *cur;
        w++;
    }
    vt->count = w;
    static int logs;
    if (logs < 24) {
        logs++;
        kprintf("[mmap] VMA compact pid=%d %d -> %d entries\n", pid, before, w);
    }
}

long sys_mmap(uint64_t addr, uint64_t len, uint32_t prot,
              uint32_t flags, int fd, uint64_t offset) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    vma_compact_table(vt, pid);

    if (len == 0) return -22;
    len = page_align_up(len);

    uint64_t base;
    if ((flags & VMA_FLAG_FIXED) && addr) {
        base = page_align_down(addr);
        /* Linux MAP_FIXED atomically REPLACES whatever the range held.
         * Skipping this left TWO VMAs claiming the range, and the demand
         * fault handler resolves against whichever it scans first (the
         * table order is scrambled by vma_remove's swap-with-last) -- a
         * coin-flip between the live mapping's prot/backing and a stale
         * one. ld.so maps every .so's segments MAP_FIXED inside a bigger
         * reservation, so duplicates were the NORM, not a corner case. */
        sys_munmap(base, len);
#ifdef CHROMIUM_BOOT
        /* Record FIXED map-overs in the mprotect ring: PartitionAlloc
         * DECOMMITS slot spans by mmap(MAP_FIXED|PROT_NONE) over them (not
         * mprotect), so without this the ring says "(none recorded)" for a
         * range that was in fact committed and then map-over-decommitted.
         * Tag: prot | 0x100 marks "mmap-fixed" vs plain mprotect. */
        mprot_ring_note(base, len, pid, prot | 0x100u);
#endif
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
        /* Slice 95: chunk the commit. This loop (alloc + 4K memset + map,
         * per page, whole range) was the #1 residual BKL customer after the
         * mprotect fixes -- [lx-hold] mmap=28-48k Mcyc/min with single holds
         * ~1 s (a multi-hundred-MB commit zeroes it all under the BKL).
         * Same discipline as sys_mprotect: bounce the BKL between chunks so
         * the ticket queue drains; the VMA is fully recorded above, so
         * concurrent mappers already see this range as taken. */
        const uint64_t COMMIT_CHUNK = 16ull << 20;       /* 16 MiB */
        uint64_t a = base, cend = base + len;
        while (a < cend) {
            uint64_t seg = a + COMMIT_CHUNK;
            if (seg > cend) seg = cend;
            uint64_t saved_root = vmm_set_editor_root(p->cr3);
            for (; a < seg; a += PAGE_SIZE) {
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
            if (a < cend && bkl_held()) { bkl_exit(); bkl_enter(); }
        }
    }

    return (long)base;
}

/* ---- sys_mmap2 (new demand-paged API from mmap.h) ---- */

long sys_mmap2(uint64_t addr, size_t length, int prot, int flags,
               int fd, uint64_t offset) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];

    if (length == 0) return -22;
    length = page_align_up(length);

    uint64_t base;
    if ((flags & MAP_FIXED) && addr) {
        base = page_align_down(addr);
        sys_munmap(base, length);        /* MAP_FIXED replaces (see sys_mmap) */
#ifdef CHROMIUM_BOOT
        mprot_ring_note(base, length, pid, 0x100u);  /* see sys_mmap note */
#endif
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

    /* NOTE: this used to ALSO register the range in page_fault.c's per-pid
     * vm_space (vma_add) -- a SECOND registry that munmap never trimmed and
     * that is keyed by the calling THREAD's pid, not the tgid. Stale entries
     * there could service a demand fault on a recycled address range with
     * the OLD mapping's flags/backing (its VMA_FILE path maps a zero page
     * without ever reading the file), silently zeroing live data. The
     * g_vma_tables entry above is the single source of truth; the fault
     * path reaches it via mmap_handle_page_fault. */

    /* Demand paging: DON'T allocate pages now. The page fault handler
     * will allocate zero-fill pages on first access. */

    return (long)base;
}

/* ---- deferred-free batch: shoot down TLBs BEFORE returning frames to the PMM ---
 *
 * munmap/madvise clear a PTE then free the physical frame. The frame becomes
 * reusable the instant pmm_free_page() runs, but the OLD virtual->physical
 * translation can still be cached in another CPU's TLB until a shootdown. The
 * demand-fault path calls pmm_alloc_page() BKL-FREE (mmap_try_fault, before the
 * BKL-retry), so while a BKL-holding munmap/madvise walks a multi-page range --
 * frame 1 freed, frames 2..N still being freed, shootdown not yet issued -- a
 * concurrent fault on another CPU can re-hand-out frame 1, zero+refill it (e.g.
 * with JSON/string bytes), and a third CPU reading the OLD virtual address
 * through its stale TLB reads that NEW content. Measured as chrome's YouTube
 * renderer reading ASCII ("{{author", "network.") where a pointer belonged, then
 * #GP/#PF'ing on it (same rip 0x208c13a, watch page + embed).
 *
 * Fix: never return a frame to the PMM until a shootdown has followed its unmap.
 * Collect freed frames here; on a full batch (or at the end) shoot down FIRST,
 * then free. A bounded on-stack batch keeps this O(pages) with amortized
 * shootdowns; a partial batch at the end still forces one shootdown, preserving
 * the old "always shoot down after unmapping" guarantee for refs>1 / no-free
 * pages too. */
#define MMAP_FREE_BATCH 256
struct mmap_free_batch { uint64_t phys[MMAP_FREE_BATCH]; int n; };

/* Quarantine for frames whose shootdown TIMED OUT (slice 50): under WHPX a
 * vCPU can be host-descheduled past the bounded ack wait, so ~8 shootdowns
 * per run return unacked -- and a frame freed then is reissuable while that
 * CPU may still hold the stale translation (the exact corruption the batch
 * exists to prevent, sneaking through the timeout path). Unacked frames park
 * here and only drain after a LATER fully-acked shootdown proves every TLB
 * has turned over.
 *
 * Slice 87: shootdown waits drop the BKL (so peer CPUs can ack), so this
 * ring is no longer BKL-covered -- guard it with its own spinlock. Ring
 * full => deliberately LEAK (a page lost beats a page corrupted). */
#define TLBQ_MAX 8192
static uint64_t   g_tlbq[TLBQ_MAX];
static int        g_tlbq_n;
static uint64_t   g_tlbq_leaked;
static spinlock_t g_tlbq_lock = SPINLOCK_INIT;

static void tlbq_drain_after_acked(void) {      /* call ONLY after acked==true */
    spin_lock(&g_tlbq_lock);
    for (int i = 0; i < g_tlbq_n; i++) pmm_free_page(g_tlbq[i]);
    g_tlbq_n = 0;
    spin_unlock(&g_tlbq_lock);
}
static void tlbq_free_or_park(uint64_t phys, bool acked) {
    if (acked) { pmm_free_page(phys); return; }
    spin_lock(&g_tlbq_lock);
    if (g_tlbq_n < TLBQ_MAX) {
        g_tlbq[g_tlbq_n++] = phys;
        spin_unlock(&g_tlbq_lock);
        return;
    }
    g_tlbq_leaked++;
    uint64_t leaked = g_tlbq_leaked;
    spin_unlock(&g_tlbq_lock);
    if (leaked <= 8)
        kprintf("[tlbq] WARN: quarantine full, leaking frame %p (total %lu)\n",
                (void *)phys, (unsigned long)leaked);
}

static void mmap_free_batch_flush(struct mmap_free_batch *b) {
    bool acked = tlb_shootdown_remote_sync();   /* stale TLBs gone BEFORE reuse */
    if (acked) tlbq_drain_after_acked();
    for (int i = 0; i < b->n; i++) tlbq_free_or_park(b->phys[i], acked);
    b->n = 0;
}
static void mmap_free_batch_push(struct mmap_free_batch *b, uint64_t phys) {
    if (b->n == MMAP_FREE_BATCH) mmap_free_batch_flush(b);
    b->phys[b->n++] = phys;
}

/* ---- munmap ---- */

long sys_munmap(uint64_t addr, uint64_t len) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    vma_compact_table(vt, pid);   /* slice 57: safe here -- no entry ptrs yet
                                   * (incl. the sys_mmap MAP_FIXED caller) */

    if (len == 0) return -22;
    addr = page_align_down(addr);
    len  = page_align_up(len);

    struct mmap_free_batch fb = { .n = 0 };
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
                phys &= PAGE_MASK;
                if (nofree) {
                    /* memfd/shm-cache owns the frame: drop only THIS
                     * mapping's reference (taken at map time). The owner
                     * frees it on last unref. Skipping the dec leaked one
                     * count per munmap'd page forever. */
                    page_ref_dec(phys);
                } else {
                    /* CoW-aware: after a fork this frame may still be
                     * mapped by the other side (refs > 1). Blindly freeing
                     * it here handed live frames back to the PMM -- the
                     * next allocation scribbled over the fork sibling's
                     * memory (slice 38: chrome's PartitionAlloc munmaps
                     * constantly amid renderer fork churn; flaky heap
                     * corruption in whichever process kept the frame).
                     * Freeing is DEFERRED to fb so a TLB shootdown precedes
                     * reuse (see mmap_free_batch above). */
                    int refs = page_ref_get(phys);
                    if (refs > 1) {
                        page_ref_dec(phys);
                    } else {
                        if (refs == 1) page_ref_dec(phys);
                        mmap_free_batch_push(&fb, phys);
                    }
                }
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
                v->end = addr;
            } else {
                /* Slice 61f: table full mid-split. The old code shrank v
                 * ANYWAY, silently dropping VMA coverage of [addr+len, end)
                 * while its PTEs stayed present -- the exact "[pfrej] NO
                 * VMA (present PTE)" wild-access signature of the slice-57
                 * jumps and the 61d renderer-startup flake. Keep v spanning
                 * the hole instead: the hole's frames are already freed, so
                 * a later touch demand-faults a fresh zero page (harmless
                 * for ANON) instead of the process losing a live range.
                 * Loud on purpose -- this only fires under real VMA
                 * pressure and must name itself in the serial log. */
                kprintf("[mmap] WARN: munmap mid-split at FULL table -- "
                        "keeping [0x%lx,0x%lx) spanned (hole 0x%lx+0x%lx)\n",
                        (unsigned long)v->start, (unsigned long)v->end,
                        (unsigned long)addr, (unsigned long)len);
            }
        }
    }

    /* The unmapped translations may be cached on other CPUs (sibling
     * threads); the freed frames get reissued by the PMM, so a stale
     * entry would read/write somebody else's new page. Flush shoots down
     * FIRST, then releases the deferred frames; an empty batch still forces
     * a final shootdown for refs>1 / nofree unmaps. */
    mmap_free_batch_flush(&fb);

    return 0;
}

long sys_munmap2(uint64_t addr, size_t length) {
    return sys_munmap(addr, (uint64_t)length);
}

/* madvise(MADV_DONTNEED) with REAL Linux semantics for private anon memory:
 * drop the pages; the next touch demand-faults a FRESH ZERO page (the VMA
 * stays). This is NOT advisory on Linux and callers depend on the zero-fill
 * guarantee -- chrome's PartitionAlloc periodically discards free slot spans
 * with MADV_DONTNEED and its zero-fill allocation path (calloc through the
 * allocator shim) SKIPS memset for slots on "freshly committed from the OS"
 * system pages. Treating DONTNEED as a success no-op left the old bytes in
 * place, so "zeroed" allocations came back holding whatever died there
 * before -- measured: retired SwiftShader framebuffer pixels
 * (0xff000000ff000000 = two opaque-black ARGB texels) resurfacing inside
 * NSPR's freshly calloc'd lock/cv structures, whose notify walk then #GP'd
 * (slice 38 iters 10/12; the crash moved with allocator timing, which is why
 * an identical binary crashed one boot and not the next).
 *
 * Only ranges inside private ANON VMAs are dropped. Shared/NOFREE (memfd,
 * shm-cache) mappings keep their frames: for those DONTNEED means "reload
 * from backing", and the backing IS the frame. Unknown ranges (brk heap,
 * grow-down stacks) are left alone -- no VMA means the demand-refill
 * contract can't be honored, so dropping would turn the next touch into a
 * SIGSEGV. */
#ifdef CHROMIUM_BOOT
/* Slice 38 iter 19: ring of recent mprotect/madvise permission transitions.
 * The deterministic SwiftShader-JIT crash reads a pointer into a PROT_NONE
 * anon VMA (a hole in PartitionAlloc's reservation). Whether that range was
 * NEVER committed (stale pointer, chrome-side) or was committed AND the
 * transition got lost (kernel-side split/apply bug) is invisible post-mortem
 * without history. Dumped by mprotect_ring_dump(addr) from the fatal-fault
 * path. Sized so a pre-crash mprotect storm can't evict the answer (a 2048
 * ring was flushed in <1s of SwiftShader JIT setup); madvise(DONTNEED)
 * drops are tagged MPROT_MADV in the same timeline. */
struct mprot_ev { uint64_t ms, addr, len; int pid; uint32_t prot; };
#define MPROT_RING 16384
#define MPROT_MADV 0xFFFFu
static struct mprot_ev g_mprot_ring[MPROT_RING];
static uint32_t g_mprot_head;
extern uint64_t klog_ms(void);
void mprot_ring_note(uint64_t addr, uint64_t len, int pid, uint32_t prot) {
    struct mprot_ev *e = &g_mprot_ring[g_mprot_head % MPROT_RING];
    g_mprot_head++;
    e->ms = klog_ms(); e->pid = pid; e->addr = addr; e->len = len;
    e->prot = prot;
}
void mprotect_ring_dump(uint64_t fault_addr) {
    uint64_t oldest = 0, newest = 0;
    uint32_t n = g_mprot_head < MPROT_RING ? g_mprot_head : MPROT_RING;
    if (n) {
        oldest = g_mprot_ring[(g_mprot_head - n) % MPROT_RING].ms;
        newest = g_mprot_ring[(g_mprot_head - 1) % MPROT_RING].ms;
    }
    kprintf("[mprot] history covering %p (ring spans %lums..%lums, %u ev):\n",
            (void *)fault_addr, (unsigned long)oldest, (unsigned long)newest,
            (unsigned)g_mprot_head);
    int shown = 0;
    for (uint32_t k = 0; k < MPROT_RING; k++) {
        struct mprot_ev *e = &g_mprot_ring[(g_mprot_head + k) % MPROT_RING];
        if (!e->len) continue;
        if (fault_addr < e->addr || fault_addr >= e->addr + e->len) continue;
        kprintf("  [mprot] %lums pid=%d %s(%p, 0x%lx, prot=0x%x)\n",
                (unsigned long)e->ms, e->pid,
                e->prot == MPROT_MADV ? "madvise-dontneed" : "mprotect",
                (void *)e->addr, (unsigned long)e->len, e->prot);
        shown++;
    }
    if (!shown) kprintf("  [mprot] (none recorded)\n");
}
#endif

long sys_madvise_dontneed(uint64_t addr, uint64_t len) {
    struct proc *p = current_proc();
    if (!p) return -1;
    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    if (len == 0) return 0;
    addr = page_align_down(addr);
    len  = page_align_up(len);
#ifdef CHROMIUM_BOOT
    mprot_ring_note(addr, len, pid, MPROT_MADV);
#endif

    struct mmap_free_batch fb = { .n = 0 };
    for (int i = 0; i < vt->count; i++) {
        struct mmap_vma *v = &vt->entries[i];
        if (addr >= v->end || (addr + len) <= v->start) continue;
        if (!(v->flags & VMA_FLAG_ANON)) continue;      /* file-backed: keep */
        if (v->flags & (VMA_FLAG_SHARED | VMA_FLAG_NOFREE)) continue;

        uint64_t start = (addr > v->start) ? addr : v->start;
        uint64_t end   = ((addr + len) < v->end) ? (addr + len) : v->end;
        uint64_t saved_root = vmm_set_editor_root(p->cr3);
        for (uint64_t a = start; a < end; a += PAGE_SIZE) {
            uint64_t phys = vmm_translate(a);
            if (!phys) continue;                        /* already absent */
            vmm_unmap(a, PAGE_SIZE);
            phys &= PAGE_MASK;
            int refs = page_ref_get(phys);
            if (refs > 1) {
                page_ref_dec(phys);                     /* fork sibling keeps it */
            } else {
                if (refs == 1) page_ref_dec(phys);
                mmap_free_batch_push(&fb, phys);        /* freed AFTER shootdown */
            }
        }
        vmm_set_editor_root(saved_root);
    }
    /* Same as munmap: dropped translations must leave every CPU's TLB BEFORE the
     * freed frames are reissued (else a reused frame is read through a stale
     * entry -- the YouTube renderer corruption). Flush shoots down then frees. */
    mmap_free_batch_flush(&fb);
    return 0;
}

/* ---- mprotect ---- */

long sys_mprotect(uint64_t addr, uint64_t len, uint32_t prot) {
    struct proc *p = current_proc();
    if (!p) return -1;

    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    vma_compact_table(vt, pid);   /* slice 57: before any entry lookup */

    if (len == 0) return -22;
    addr = page_align_down(addr);
    len  = page_align_up(len);
    uint64_t rstart = addr, rend = addr + len;

#ifdef CHROMIUM_BOOT
    mprot_ring_note(addr, len, pid, prot);
#endif

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
            struct mmap_vma *tail = mid ? vma_alloc(vt) : NULL;
            if (!mid || !tail) {
                /* Slice 61f: roll back a half-done alloc pair. vma_alloc
                 * bumps count and returns an UNINITIALIZED slot (stale
                 * range from an earlier vma_remove swap); abandoning mid
                 * here left that garbage entry LIVE in the table, able to
                 * shadow arbitrary address ranges on later lookups. */
                if (mid && !tail) vt->count--;
                v->prot = prot; continue;
            }
            v = &vt->entries[i];
            mid->start  = rstart; mid->end  = rend; mid->prot  = prot;
            mid->flags  = v->flags; mid->fd = v->fd; mid->offset = v->offset;
            tail->start = rend;  tail->end  = ve;   tail->prot = oldprot;
            tail->flags = v->flags; tail->fd = v->fd; tail->offset = v->offset;
            v->end = rstart;                      /* v keeps oldprot */
        }
    }

    uint32_t vmm_f = prot_to_vmm_flags(prot);
    /* Slice 95: chunk the PTE walk so one giant mprotect cannot convoy the
     * whole kernel. The VMA tables above are already updated, so a fault
     * taken mid-walk resolves against the NEW protection (the "VMA present
     * but PROT_NONE / not writable" path); interleaving with the app's own
     * threads is the POSIX-undefined mprotect race it always was. Between
     * chunks: restore the editor root (it is GLOBAL state -- another
     * thread's vmm op must not edit our cr3), then bounce the BKL so the
     * fair ticket queue hands the kernel to whoever piled up behind us.
     * One shootdown at the end, exactly as before (the stale-TLB window
     * across chunks is the same window the single walk always had). */
    {
        const uint64_t MPROT_CHUNK = 32ull << 20;      /* 32 MiB */
        uint64_t off = 0;
        while (off < len) {
            uint64_t n = len - off;
            if (n > MPROT_CHUNK) n = MPROT_CHUNK;
            uint64_t saved_root = vmm_set_editor_root(p->cr3);
            vmm_protect(addr + off, n, vmm_f);
            vmm_set_editor_root(saved_root);
            off += n;
            if (off < len && bkl_held()) { bkl_exit(); bkl_enter(); }
        }
    }
    /* Permission downgrade must reach every CPU's TLB, not just ours --
     * another thread of this process may be running on another core with
     * the old (more permissive) translation cached. */
    tlb_shootdown_remote();

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

    int pid = proc_mm_pid(p);
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
        struct mmap_free_batch fb = { .n = 0 };
        uint64_t saved_root = vmm_set_editor_root(p->cr3);
        for (uint64_t a = new_brk; a < vt->brk_cur; a += PAGE_SIZE) {
            uint64_t phys = vmm_translate(a);
            if (phys) {
                vmm_unmap(a, PAGE_SIZE);
                phys &= PAGE_MASK;
                /* CoW-aware, same as munmap: a fork sibling may still map
                 * this frame. Freeing is DEFERRED behind a shootdown (see
                 * mmap_free_batch) so a reused frame can't be read via a
                 * stale TLB. */
                int refs = page_ref_get(phys);
                if (refs > 1) {
                    page_ref_dec(phys);
                } else {
                    if (refs == 1) page_ref_dec(phys);
                    mmap_free_batch_push(&fb, phys);
                }
            }
        }
        vmm_set_editor_root(saved_root);
        mmap_free_batch_flush(&fb);
    }

    vt->brk_cur = new_brk;
    return (long)new_brk;
}

/* ---- Page fault handler (original API) ---- */

static bool mmap_try_fault(uint64_t fault_addr, uint64_t error_code) {
    struct proc *p = current_proc();
    if (!p) return false;

    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    struct mmap_vma *v = vma_find_internal(vt, fault_addr);
#ifdef CHROMIUM_BOOT
    /* Slice 38 iter 18: two deterministic fatal faults (browser main thread
     * movups-write to a non-present heap page; JIT read of a cage pointer)
     * are REFUSALS from this function -- but the fatal dump can't tell WHICH
     * return-false fired. Log each rejection with the VMA state. Rate-limited;
     * refusals are normally rare (real SIGSEGVs). */
    #define PF_REJECT(fmt, ...) do { static int _c; if (_c < 24) { _c++; \
        kprintf("[pfrej] pid=%d addr=%p err=0x%lx " fmt "\n", \
                pid, (void *)fault_addr, (unsigned long)error_code, \
                ##__VA_ARGS__); } } while (0)
#else
    #define PF_REJECT(fmt, ...) do { } while (0)
#endif
    if (!v) { PF_REJECT("NO VMA"); return false; }

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
            if (!old_phys) { vmm_set_editor_root(saved_root);
                PF_REJECT("COW VMA [%p,%p) prot=0x%x but page NOT PRESENT",
                          (void *)v->start, (void *)v->end, v->prot);
                return false; }

            uint64_t new_phys = pmm_alloc_page();
            if (!new_phys) { vmm_set_editor_root(saved_root);
                PF_REJECT("COW copy OOM"); return false; }

            memcpy((void *)(new_phys + vmm_hhdm_offset()),
                   (void *)(old_phys + vmm_hhdm_offset()),
                   PAGE_SIZE);

            /* Slice 50: compare-and-remap, NOT unmap+map. This path is
             * BKL-free, so a sibling thread's CoW fault on the SAME page can
             * have already swapped in ITS copy -- blindly remapping ours on
             * top would discard every write made through the sibling's copy
             * since. Only install if the frame is still the one we copied. */
            old_phys &= PAGE_MASK;
            uint32_t vmm_f = prot_to_vmm_flags(v->prot | VMA_PROT_WRITE);
            int ins = vmm_remap_page_if(page_va, old_phys, new_phys, vmm_f);
            vmm_set_editor_root(saved_root);
            if (ins != 1) {
                pmm_free_page(new_phys);        /* our copy was never visible */
                if (ins < 0) { PF_REJECT("COW remap walk failed"); return false; }
#ifdef CHROMIUM_BOOT
                { static int _r; if (_r < 24) { _r++;
                    kprintf("[pfrace] pid=%d COW lost race va=%p\n",
                            pid, (void *)page_va); } }
#endif
                /* Winner already broke the CoW; if permissions still block
                 * this access the hardware re-faults and we resolve then. */
                return true;
            }
            /* old_phys is deliberately NOT freed or shot down here -- same
             * leak the pre-slice-50 unmap+map code had. A first attempt that
             * also dec/freed it (mirroring munmap) plus a per-page remote
             * shootdown regressed boot ~16s and coincided with 3/3 no-paint
             * runs (suspect: refcount protocol mismatch freeing live frames,
             * and per-CoW IPI storms during fork churn). A leaked frame can
             * still be written through a peer's stale TLB entry, but it is
             * never REISSUED, so no other owner's memory can be corrupted --
             * strictly the old behavior. Revisit with measured refcount
             * semantics if the leak ever matters. */

            v->flags &= ~VMA_FLAG_COW;
            v->prot |= VMA_PROT_WRITE;
            return true;
        }
        PF_REJECT("WRITE to non-writable VMA [%p,%p) prot=0x%x flags=0x%x",
                  (void *)v->start, (void *)v->end, v->prot, v->flags);
        return false;
    }

    /* A page that is ALREADY PRESENT must never be replaced by the
     * demand-zero path below -- that clobbers live data with zeros and leaks
     * the old frame. A present-page fault here is a pure PERMISSION fault
     * (e.g. a write to a page mapped read-only while the VMA says RW, after
     * an mprotect raced or a CoW bit was cleared): fix the PTE in place if
     * the VMA allows it, else it's a real fault. (page_fault.c's Case 3 has
     * carried this exact guard; this fallback path lacked it.) */
    uint64_t page_va = page_align_down(fault_addr);
    {
        uint64_t saved_root = vmm_set_editor_root(p->cr3);
        uint64_t cur = vmm_translate(page_va);
        if (cur) {
            bool ok = false;
            if (is_write && (v->prot & VMA_PROT_WRITE)) {
                vmm_protect(page_va, PAGE_SIZE,
                            prot_to_vmm_flags(v->prot) |
                            ((v->flags & VMA_FLAG_SHARED) ? VMM_SHARED : 0));
                ok = true;
            }
            vmm_set_editor_root(saved_root);
            if (!ok)
                PF_REJECT("PRESENT page, unhandled combo: VMA [%p,%p) "
                          "prot=0x%x flags=0x%x",
                          (void *)v->start, (void *)v->end, v->prot, v->flags);
            return ok;
        }
        vmm_set_editor_root(saved_root);
    }

    /* PROT_NONE (V8 cage guards / reservations): a touch is a REAL fault.
     * Without this check the demand path below "serviced" it with a page
     * whose PTE carries no user/rw bits, and the instant re-fault came back
     * as an unhandled present-page combo -- same fatal outcome, wrong clue. */
    if (v->prot == 0) {
        PF_REJECT("touch on PROT_NONE VMA [%p,%p) flags=0x%x",
                  (void *)v->start, (void *)v->end, v->flags);
        return false;
    }

    /* Demand paging: allocate a zero-filled physical page */
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        /* This failure path is a SILENT fatal SIGSEGV for the process --
         * from the outside indistinguishable from a wild pointer. Name it. */
        kprintf("[mmap] demand-page OOM at %p (VMA [%p,%p) prot=0x%x) "
                "free=%lu pages\n",
                (void *)fault_addr, (void *)v->start, (void *)v->end,
                v->prot, (unsigned long)pmm_free_pages());
        return false;
    }

    memset((void *)(phys + vmm_hhdm_offset()), 0, PAGE_SIZE);

    uint32_t vmm_f = prot_to_vmm_flags(v->prot);
    /* A page inside a MAP_SHARED VMA must never be CoW'd at fork: whoever
     * shares the frame at fork time keeps sharing it. (Anon-shared pages the
     * parent faults in AFTER the fork are still per-process -- full anon
     * MAP_SHARED needs backing storage like the shm cache -- but pages that
     * exist at fork time now behave.) */
    if (v->flags & VMA_FLAG_SHARED) vmm_f |= VMM_SHARED;
    uint64_t saved_root = vmm_set_editor_root(p->cr3);
    /* Slice 50: install-if-absent, NOT vmm_map. This path runs BKL-free, so
     * two CPUs can demand-fault the same page: both saw it absent above, both
     * allocated a zero frame. vmm_map's map_4k silently REPLACED the first
     * install with the second (zeroed) frame -- discarding every byte the
     * winning thread had written through it in between. Measured: chrome's
     * renderer read NULL out of a live Vec-of-pointers (rip 0x208c13a) with
     * "[vmm] already mapped" replaces logged 15ms before the fault. The loser
     * now backs off and frees its own never-visible frame; the winner's frame
     * (same VMA => same flags, also zero-filled) is the page. */
    int ins = vmm_map_page_if_absent(page_va, phys, vmm_f);
    vmm_set_editor_root(saved_root);
    if (ins != 1) {
        pmm_free_page(phys);
        if (ins < 0) {
            kprintf("[mmap] demand-page table OOM at %p\n", (void *)fault_addr);
            return false;
        }
#ifdef CHROMIUM_BOOT
        { static int _r; if (_r < 24) { _r++;
            kprintf("[pfrace] pid=%d demand-map lost race va=%p (winner keeps)\n",
                    pid, (void *)page_va); } }
#endif
    }
    return true;
}

/* Public entry: resolve a user page fault against the mmap VMA table.
 *
 * The resolver above runs BKL-FREE on the #PF path (fault concurrency, slice
 * 39). But the VMA table is mutated by mmap/munmap/mprotect, which run UNDER
 * the BKL -- so a fault on one CPU can read a VMA MID-SPLIT while a BKL-holding
 * mprotect on another CPU is halfway through carving it. Slice 46 saw exactly
 * this: a YouTube watch page drives chrome's W^X pattern (mprotect a 2MB span
 * to PROT_NONE, then carve one page RW, ~27k times), and the lockless read
 * caught the pre-split GIANT PROT_NONE cage VMA still covering an address that
 * was really in the freshly-carved RW sub-page -> wrong "WRITE to non-writable
 * VMA" -> SIGSEGV -> the browser died mid-video (EXCEPTION 14, err=0x6).
 *
 * Fix: try lockless first (fast, common path). Only if that REFUSES do we
 * re-verify ONCE under the BKL, which serializes with every VMA mutator, so the
 * table is guaranteed consistent (no split in flight). A genuine bad access
 * still refuses; a transient-race refusal now resolves. IRQs are enabled around
 * bkl_enter so its spin still ACKs the TLB-shootdown IPI that the very mprotect
 * we are waiting on issues under the BKL (otherwise: deadlock). A fault from a
 * syscall's copy_to/from_user already holds the BKL (consistent view), so it
 * skips the retry. The refused first attempt is side-effect-free (every
 * return-false path bails before mutating), so re-running is safe. */
bool mmap_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    /* Slice 88: while a sibling forks with CoW STW, do not CoW-copy under
     * the WP walk. Slice 92: the wait must NOT hold the BKL (see
     * vm_quiesce_park_oncpu -- holding it here was the -smp 4 freeze). */
    vm_quiesce_park_oncpu(current_proc());
    if (mmap_try_fault(fault_addr, error_code)) return true;
    if (bkl_held()) return false;          /* syscall-context fault: already serialized */
    uint64_t rf = read_rflags();
    sti();                                  /* let the spin ACK TLB-shootdown IPIs */
    bkl_enter();
    bool ok = mmap_try_fault(fault_addr, error_code);
    bkl_exit();
    if (!(rf & (1u << 9))) cli();           /* restore the #PF handler's IRQ-off state */
    return ok;
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
#ifdef CHROMIUM_BOOT
            /* Eager-copy fork leaves parent PTEs writable and exclusive.
             * Marking the parent VMA CoW here made later mprotect/fault
             * paths treat still-writable exclusive pages as shared. */
            (void)parent_vt;
#else
            if (i < parent_vt->count) {
                parent_vt->entries[i].flags |= VMA_FLAG_COW;
            }
#endif
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

/* ---- slice 107: SHARED-REGION CENSUS ----------------------------------
 * Handoff §6 candidate 1 ("viz shared-memory frames over Mojo") rests on a
 * single testable claim: that somewhere inside chrome there is a
 * FRAMEBUFFER-SIZED shared region whose contents change once per frame. If
 * there is, we can read pixels straight out of it and skip the JPEG+base64+
 * pipe round trip that slice 68 measured as the binding cost (~21ms/frame,
 * and frame rate tracks PAYLOAD SIZE). If there is not, the candidate is
 * disproven and no amount of plumbing will save it.
 *
 * Two tiers of this arc (2.5, then 3) died from building on a mechanism
 * nobody had verified end to end. So verify this one BEFORE implementing:
 * the census walks every shared region the kernel owns -- shm_cache entries
 * (file-backed MAP_SHARED) and memfds -- samples a hash of each, and reports
 * size alongside how many census rounds saw the contents CHANGE.
 *
 * Reading the verdict: a region of width*height*4 bytes (800x600x4 = 1875
 * KiB) that changes every round IS the composited frame. Small regions that
 * change constantly are metrics/discardable/font caches, not pixels. */
#define MEMFD_REG_MAX 192
static struct memfd *g_memfd_reg[MEMFD_REG_MAX];
static uint32_t      g_memfd_reg_id[MEMFD_REG_MAX];
static int           g_memfd_reg_pid[MEMFD_REG_MAX];
static uint32_t      g_memfd_next_id = 1;

static void memfd_reg_add(struct memfd *mf) {
    for (int i = 0; i < MEMFD_REG_MAX; i++) {
        if (!g_memfd_reg[i]) {
            g_memfd_reg[i]     = mf;
            g_memfd_reg_id[i]  = g_memfd_next_id++;
            struct proc *p     = current_proc();
            g_memfd_reg_pid[i] = p ? p->pid : -1;
            return;
        }
    }
}

static void memfd_reg_del(struct memfd *mf) {
    for (int i = 0; i < MEMFD_REG_MAX; i++)
        if (g_memfd_reg[i] == mf) { g_memfd_reg[i] = 0; return; }
}

struct memfd *memfd_new(void) {
    struct memfd *mf = (struct memfd *)kmalloc(sizeof *mf);
    if (!mf) return 0;
    mf->pages = 0; mf->npages = 0; mf->cap = 0;
    mf->size = 0; mf->refs = 1; mf->seals = 0;
    memfd_reg_add(mf);
    return mf;
}

void memfd_ref(struct memfd *mf) { if (mf) mf->refs++; }

void memfd_unref(struct memfd *mf) {
    if (!mf) return;
    if (--mf->refs > 0) return;
    memfd_reg_del(mf);
    for (size_t i = 0; i < mf->npages; i++)
        if (mf->pages[i]) {
            /* Drop the memfd's OWN reference (memfd_ensure_pages). If a
             * live mapping still holds one -- chrome passes memfd
             * SharedMemoryRegions over sockets, and a mapper can outlive
             * every fd -- that mapper's teardown frees the frame. Freeing
             * unconditionally here (the pre-slice-38 code) recycled frames
             * other processes were still writing through. */
            uint64_t ph = mf->pages[i];
            page_ref_dec(ph);
            if (page_ref_get(ph) == 0)
                pmm_free_page(ph);
        }
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
        /* The memfd itself holds one reference (mirrors shm_cache_ensure).
         * Untracked (refs==0) memfd pages got entangled with fork's
         * unconditional PTE refcounting: fork brought them to 2, the fork
         * participants' teardowns dropped them to 0 and FREED a frame the
         * memfd still listed in pages[] -- and that any OTHER mapper (which
         * took no reference at all, see memfd_map) was still writing
         * through. memfd_unref then freed it a second time. */
        page_ref_inc(phys);
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
    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    if (len == 0) return -22;
    len = page_align_up(len);
    offset = page_align_down(offset);
    size_t page_off = (size_t)(offset / PAGE_SIZE);
    size_t np = (size_t)(len / PAGE_SIZE);
    if (memfd_ensure_pages(mf, page_off + np) < 0) return -12;

    /* MAP_PRIVATE of a memfd is copy-on-write on Linux: each mapping gets its
     * own pages, and writes never reach the file or other mappings.
     * SwiftShader's Reactor JIT depends on that -- it keeps ONE
     * 'swiftshader_jit' memfd and, for EVERY compiled routine, ftruncates it
     * to the routine's size and maps it PRIVATE at offset 0, then writes the
     * code through the new mapping (ExecutableMemory.cpp). Treating those
     * mappings as coherent shared memory aliased them all to file pages 0..n:
     * each newly compiled routine overwrote the previous routines' code in
     * place (slice 38 crash: the DrawCall's vertex slot executed PIXEL
     * routine code -- last one written -- which walked the Vertex array with
     * the Primitive stride 0x8710 off the end into PartitionAlloc's guard).
     * Honor PRIVATE with an EAGER copy into a plain private-anon VMA: exact
     * for any writer, and the only deviation (stores through a later
     * MAP_SHARED view not appearing here) is invisible to the JIT pattern,
     * which touches each mapping alone. */
    if (!(flags & VMA_FLAG_SHARED)) {
        uint64_t base;
        if ((flags & VMA_FLAG_FIXED) && addr) {
            base = page_align_down(addr);
            sys_munmap(base, len);
        } else { base = find_free_region(vt, len); if (!base) return -12; }

        struct mmap_vma *v = vma_alloc(vt);
        if (!v) return -12;
        v->start = base; v->end = base + len; v->prot = prot;
        v->flags = (flags & ~(uint32_t)VMA_FLAG_SHARED)
                 | VMA_FLAG_ANON | VMA_FLAG_PRIVATE;
        v->fd = -1; v->offset = 0;

        uint32_t vmm_f = prot_to_vmm_flags(prot);
        uint64_t saved = vmm_set_editor_root(p->cr3);
        for (size_t i = 0; i < np; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) { vmm_set_editor_root(saved); return -12; }
            memcpy((void *)(phys + vmm_hhdm_offset()),
                   (const void *)(mf->pages[page_off + i] + vmm_hhdm_offset()),
                   PAGE_SIZE);
            vmm_map(base + i * PAGE_SIZE, phys, PAGE_SIZE, vmm_f);
        }
        vmm_set_editor_root(saved);
        return (long)base;
    }

    uint64_t base;
    if ((flags & VMA_FLAG_FIXED) && addr) {
        base = page_align_down(addr);
        sys_munmap(base, len);           /* MAP_FIXED replaces (see sys_mmap) */
    } else { base = find_free_region(vt, len); if (!base) return -12; }

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
    for (size_t i = 0; i < np; i++) {
        /* One reference per address space mapping the page, so this
         * process's teardown/munmap drops only its own (mirrors
         * shm_cache_mmap). Paired with the memfd's own reference from
         * memfd_ensure_pages. */
        page_ref_inc(mf->pages[page_off + i]);
        vmm_map(base + i * PAGE_SIZE, mf->pages[page_off + i], PAGE_SIZE, vmm_f);
    }
    vmm_set_editor_root(saved);
    return (long)base;
}

/* Slice 98 (tier 3 Phase 1b): map a CONTIGUOUS PHYSICAL range into the
 * calling process. /dev/dri BOs are DMA buffers the GPU reads directly,
 * so the guest must write the very same frames -- an anonymous shadow
 * plus a copy (the fbdev model) would be both slower and wrong for
 * Mesa, which expects its mapping to BE the buffer.
 *
 * Marked SHARED+NOFREE for the same reasons the memfd path is: fork must
 * never CoW these pages (every mapper writes the one frame the device
 * sees), and teardown must not hand device memory back to the PMM --
 * the DRM layer owns that lifetime. */
long mmap_map_phys_user(uint64_t phys, uint64_t len) {
    struct proc *p = current_proc();
    if (!p || !phys || len == 0) return -22;
    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    len = page_align_up(len);
    size_t np = (size_t)(len / PAGE_SIZE);

    uint64_t base = find_free_region(vt, len);
    if (!base) return -12;
    struct mmap_vma *v = vma_alloc(vt);
    if (!v) return -12;
    v->start = base; v->end = base + len;
    v->prot  = VMA_PROT_READ | VMA_PROT_WRITE;
    v->flags = VMA_FLAG_SHARED | VMA_FLAG_NOFREE;
    v->fd = -1; v->offset = 0;

    uint32_t vmm_f = prot_to_vmm_flags(v->prot) | VMM_SHARED;
    uint64_t saved = vmm_set_editor_root(p->cr3);
    for (size_t i = 0; i < np; i++)
        vmm_map(base + i * PAGE_SIZE, phys + (uint64_t)i * PAGE_SIZE,
                PAGE_SIZE, vmm_f);
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
    int pid = proc_mm_pid(p);
    struct vma_table *vt = &g_vma_tables[pid];
    if (len == 0) return -22;
    len = page_align_up(len);
    offset = page_align_down(offset);
    size_t page_off = (size_t)(offset / PAGE_SIZE);
    size_t np = (size_t)(len / PAGE_SIZE);
    if (shm_cache_ensure(sc, page_off + np) < 0) return -12;

    uint64_t base;
    if ((flags & VMA_FLAG_FIXED) && addr) {
        base = page_align_down(addr);
        sys_munmap(base, len);           /* MAP_FIXED replaces (see sys_mmap) */
    } else { base = find_free_region(vt, len); if (!base) return -12; }

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

/* ---- slice 107: the census walk (see the header comment on memfd_reg) ----
 *
 * Called from the 3s scheduler heartbeat under CHROMIUM_BOOT. Hashes a
 * SAMPLE of each region (32 bytes from every 8th page) rather than the whole
 * thing: enough to detect "these bytes changed", cheap enough to run while
 * chrome is producing frames. A full hash of every 1.9MB region every 3s
 * would perturb the very workload being measured.
 *
 * Only regions >= CENSUS_MIN_PAGES are printed. Below that they are metrics,
 * font caches and discardable-memory scraps -- never a frame -- and printing
 * them buries the signal. */
#define CENSUS_MIN_PAGES 16          /* 64 KiB */

/* Slice 108 item 3: how many pages the change-detector samples.
 *
 * MEASURED so far: the per-frame COPY is not the cost (removing it entirely
 * moved 48.8 -> 49.2 fps, i.e. nothing), and polling HARDER made things worse
 * (4ms: 38.3 fps). Both point at the work done per poll, and the only work
 * per poll is this hash. At stride 8 a 469-page region costs 59 samples
 * scattered across 1.9 MiB -- 59 cache misses per region per poll, times ~7
 * regions, times ~66 polls/s.
 *
 * VIZ_HASH_STRIDE trades detection certainty for cache traffic. A page whose
 * frame changed anywhere is overwhelmingly likely to differ in ANY sampled
 * page for a full-viewport repaint; a small dirty rect could be missed, which
 * costs a dropped frame, not a wrong one. The census keeps the fine stride --
 * it is diagnostic and runs once every 3s, where cost does not matter. */
#ifndef VIZ_HASH_STRIDE
#define VIZ_HASH_STRIDE 64
#endif

static uint64_t census_hash_pages_stride(uint64_t *pages, size_t npages,
                                         size_t stride) {
    uint64_t h = 1469598103934665603ull;             /* FNV-1a offset basis */
    for (size_t i = 0; i < npages; i += stride) {
        uint64_t ph = pages[i];
        if (!ph) { h ^= 0x9e3779b9ull; h *= 1099511628211ull; continue; }
        const unsigned char *p = (const unsigned char *)(ph + vmm_hhdm_offset());
        for (int b = 0; b < 32; b++) { h ^= p[b]; h *= 1099511628211ull; }
    }
    return h;
}

static uint64_t census_hash_pages(uint64_t *pages, size_t npages) {
    uint64_t h = 1469598103934665603ull;             /* FNV-1a offset basis */
    for (size_t i = 0; i < npages; i += 8) {
        uint64_t ph = pages[i];
        if (!ph) { h ^= 0x9e3779b9ull; h *= 1099511628211ull; continue; }
        const unsigned char *p = (const unsigned char *)(ph + vmm_hhdm_offset());
        for (int b = 0; b < 32; b++) { h ^= p[b]; h *= 1099511628211ull; }
    }
    return h;
}

/* Per-slot previous hash + change tally, kept parallel to the registries so
 * neither struct grows for an instrument. */
static uint64_t g_cs_shm_hash[SHMCACHE_MAX];
static uint32_t g_cs_shm_chg[SHMCACHE_MAX];
static uint64_t g_cs_mfd_hash[MEMFD_REG_MAX];
static uint32_t g_cs_mfd_chg[MEMFD_REG_MAX];

void shm_census_dump(void) {
    static uint32_t round;
    round++;
    int shown = 0;
    for (int i = 0; i < SHMCACHE_MAX; i++) {
        struct shm_cache *sc = &g_shm[i];
        if (!sc->used || sc->npages < CENSUS_MIN_PAGES || !sc->pages) continue;
        uint64_t h = census_hash_pages(sc->pages, sc->npages);
        int changed = (round > 1 && h != g_cs_shm_hash[i]);
        if (changed) g_cs_shm_chg[i]++;
        g_cs_shm_hash[i] = h;
        kprintf("[census] r%u shm#%d ino=%lu pages=%lu (%luKiB) chg=%u %s\n",
                round, i, (unsigned long)sc->ino, (unsigned long)sc->npages,
                (unsigned long)(sc->npages * 4), g_cs_shm_chg[i],
                changed ? "CHANGED" : "same");
        shown++;
    }
    for (int i = 0; i < MEMFD_REG_MAX; i++) {
        struct memfd *mf = g_memfd_reg[i];
        if (!mf || mf->npages < CENSUS_MIN_PAGES || !mf->pages) continue;
        uint64_t h = census_hash_pages(mf->pages, mf->npages);
        int changed = (round > 1 && h != g_cs_mfd_hash[i]);
        if (changed) g_cs_mfd_chg[i]++;
        g_cs_mfd_hash[i] = h;
        kprintf("[census] r%u memfd#%u pid=%d pages=%lu (%luKiB) chg=%u %s\n",
                round, g_memfd_reg_id[i], g_memfd_reg_pid[i],
                (unsigned long)mf->npages, (unsigned long)(mf->npages * 4),
                g_cs_mfd_chg[i], changed ? "CHANGED" : "same");
        shown++;
    }
    if (!shown)
        kprintf("[census] r%u NO shared region >= %d pages exists at all\n",
                round, CENSUS_MIN_PAGES);
}

/* ---- slice 107: VIZ SHARED-MEMORY FRAME (handoff §6 candidate 1) --------
 *
 * The census proved the premise: under MULTI-PROCESS chrome a pool of shared
 * regions of exactly viewport size is rewritten continuously while frames are
 * produced. Those are software compositing's shared bitmaps -- the pixels
 * chrome would otherwise JPEG-encode and base64 down the CDP pipe for us to
 * decode, which slice 68 measured as the thing frame rate is bound by.
 *
 * This picks the viewport-sized region that changed MOST RECENTLY and copies
 * it out. "Most recently changed" is a heuristic, and an honest one: with a
 * rotating pool the buffer just written is the newest frame. It can tear --
 * nothing here synchronises against chrome's compositor, so a region may be
 * sampled mid-write. That is a known, measurable cost of this approach, not
 * an oversight; see the ledger.
 *
 * Returns 0 and leaves gen unchanged when nothing changed since the last
 * poll, so the caller can cheaply detect "no new frame".
 */
static uint64_t g_viz_hash[SHMCACHE_MAX];
static int      g_viz_have[SHMCACHE_MAX];
static uint32_t g_viz_gen;
static int      g_viz_last = -1;

#ifdef VIZPROD_DIAG
/* Slice 110: PRODUCER-rate diagnostic (handoff §5 item 2). The shipped poll
 * stops at the first changed region, so it can only ever observe <=1 change
 * per poll -- it measures DELIVERY, not production. This mode hashes EVERY
 * candidate region each poll (the 49.2fps-era cost, acceptable for a
 * diagnostic) and counts all changes, which at 66 polls/s over a rotating
 * pool of 4-7 regions is an unaliased count of chrome's commits: a given
 * region is rewritten every ~100ms+, far above the 15ms sampling period.
 * Delivery semantics stay IDENTICAL (first changed region in scan order
 * wins), so [cwviz] frame accounting remains comparable to viz mode. */
static uint64_t g_vp_polls, g_vp_hit, g_vp_changes, g_vp_multi;
static uint64_t g_vp_polls_l, g_vp_hit_l, g_vp_changes_l, g_vp_multi_l;
#endif

long vizframe_poll(uint32_t *w, uint32_t *h, uint32_t *stride, uint32_t *gen,
                   void *dst, size_t cap) {
    uint32_t vw = w ? *w : 0, vh = h ? *h : 0;
    if (!vw || !vh) return -22;                       /* -EINVAL: caller must say */
    uint64_t want_bytes = (uint64_t)vw * vh * 4ull;
    size_t   want_pages = (size_t)((want_bytes + PAGE_SIZE - 1) / PAGE_SIZE);

    /* Slice 108 item 3: sample coarsely, and STOP at the first region that
     * changed. The old loop hashed every candidate at stride 8 on every poll
     * even after it had found a fresh frame. With a rotating buffer pool any
     * changed region IS a fresh frame, so scanning the rest only burns cache.
     * Regions after the winner keep their stale stored hash, which is correct:
     * they will register as changed on a later poll, when they are the fresh
     * one. */
    int newest = -1;
#ifdef VIZPROD_DIAG
    int vp_chg = 0;
    for (int i = 0; i < SHMCACHE_MAX; i++) {
        struct shm_cache *sc = &g_shm[i];
        if (!sc->used || !sc->pages || sc->npages != want_pages) continue;
        uint64_t hv = census_hash_pages_stride(sc->pages, sc->npages,
                                               VIZ_HASH_STRIDE);
        int had = g_viz_have[i];
        uint64_t prev = g_viz_hash[i];
        g_viz_hash[i] = hv;
        g_viz_have[i] = 1;
        if (had && hv != prev) {
            vp_chg++;
            if (newest < 0) newest = i;   /* same winner as the shipped path */
        }
    }
    g_vp_polls++;
    if (vp_chg >= 1) g_vp_hit++;
    if (vp_chg >= 2) g_vp_multi++;
    g_vp_changes += (uint64_t)vp_chg;
    if ((g_vp_polls % 200u) == 0) {      /* ~every 3s at 66 polls/s */
        kprintf("[vizprod] polls=%lu hit=%lu chg=%lu multi=%lu | "
                "interval polls=%lu hit=%lu chg=%lu multi=%lu\n",
                (unsigned long)g_vp_polls, (unsigned long)g_vp_hit,
                (unsigned long)g_vp_changes, (unsigned long)g_vp_multi,
                (unsigned long)(g_vp_polls - g_vp_polls_l),
                (unsigned long)(g_vp_hit - g_vp_hit_l),
                (unsigned long)(g_vp_changes - g_vp_changes_l),
                (unsigned long)(g_vp_multi - g_vp_multi_l));
        g_vp_polls_l = g_vp_polls; g_vp_hit_l = g_vp_hit;
        g_vp_changes_l = g_vp_changes; g_vp_multi_l = g_vp_multi;
    }
#else
    for (int i = 0; i < SHMCACHE_MAX; i++) {
        struct shm_cache *sc = &g_shm[i];
        if (!sc->used || !sc->pages || sc->npages != want_pages) continue;
        uint64_t hv = census_hash_pages_stride(sc->pages, sc->npages,
                                               VIZ_HASH_STRIDE);
        int had = g_viz_have[i];
        uint64_t prev = g_viz_hash[i];
        g_viz_hash[i] = hv;
        g_viz_have[i] = 1;
        if (had && hv != prev) { newest = i; break; }        /* fresh frame */
    }
#endif
    /* Nothing changed this poll: keep showing the last good buffer, and do
     * NOT bump gen, so the caller knows there is no new frame. */
    if (newest < 0) newest = g_viz_last;
    else { g_viz_last = newest; g_viz_gen++; }
    if (newest < 0) return -61;                        /* -ENODATA: none yet */

    /* Slice 108: the caller needs to know WHICH region is current, so it can
     * blit from a read-only mapping instead of taking a copy. The index is
     * returned rather than added to struct abi_xframe, which tier 2.5's
     * xframe path shares -- widening that struct would change an ABI two
     * producers depend on, to carry a field only one of them uses. */
    int viz_idx = 0;
    for (int i = 0; i < SHMCACHE_MAX && i < newest; i++) {
        struct shm_cache *c = &g_shm[i];
        if (c->used && c->pages && c->npages == want_pages) viz_idx++;
    }

    struct shm_cache *sc = &g_shm[newest];
    if (!sc->used || !sc->pages) { g_viz_last = -1; return -61; }

    if (w)      *w      = vw;
    if (h)      *h      = vh;
    if (stride) *stride = vw * 4u;
    if (gen)    *gen    = g_viz_gen;

    if (dst && cap) {
        uint64_t n = want_bytes < cap ? want_bytes : cap;
        uint64_t done = 0;
        char *out = (char *)dst;
        while (done < n) {
            size_t pi = (size_t)(done / PAGE_SIZE);
            size_t po = (size_t)(done % PAGE_SIZE);
            uint64_t chunk = PAGE_SIZE - po;
            if (chunk > n - done) chunk = n - done;
            uint64_t ph = (pi < sc->npages) ? sc->pages[pi] : 0;
            if (ph) memcpy(out + done,
                           (char *)(ph + vmm_hhdm_offset()) + po, (size_t)chunk);
            else    memset(out + done, 0, (size_t)chunk);
            done += chunk;
        }
    }
    return viz_idx;          /* index into the MAP list, not the slot number */
}

/* Slice 108: install the viewport-sized regions read-only in the caller.
 *
 * These are the same physical pages chrome composites into -- shm_cache_mmap
 * maps them, it does not copy them, which is the whole point. READ-ONLY: a
 * stray write from us would land in a live compositor buffer.
 *
 * Enumerated in the SAME ORDER vizframe_poll counts, so the index it returns
 * selects addr[] here. Both walk g_shm ascending and skip non-matching slots;
 * keep them in step if either changes. */
long vizframe_map(uint32_t w, uint32_t h, uint32_t *stride,
                  uint64_t *addrs, uint32_t max_addrs) {
    if (!w || !h || !addrs || !max_addrs) return -22;
    uint64_t want_bytes = (uint64_t)w * h * 4ull;
    size_t   want_pages = (size_t)((want_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t n = 0;
    for (int i = 0; i < SHMCACHE_MAX && n < max_addrs; i++) {
        struct shm_cache *sc = &g_shm[i];
        if (!sc->used || !sc->pages || sc->npages != want_pages) continue;
        long base = shm_cache_mmap(sc, 0, want_bytes, 0x1u /* PROT_READ */,
                                   0, 0);
        if (base < 0) continue;              /* skip; a partial map is usable */
        addrs[n++] = (uint64_t)base;
    }
    if (stride) *stride = w * 4u;
    return (long)n;
}
