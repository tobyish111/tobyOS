/* vdso.c -- stage, publish, and map the vDSO (2026-08-23).
 *
 * Why this exists: real programs read the clock CONSTANTLY (Chromium does
 * tens of thousands of clock_gettime calls a second), and until now every
 * one was a full trap + BKL round-trip. The vDSO makes them a userspace
 * TSC read against constants the kernel publishes.
 *
 * ONE set of physical pages serves every process: the blob is immutable
 * (zero relocations by construction -- see tools/vdso/vdso.c) and the
 * data page is kernel-written/user-read, so all mappings alias the same
 * frames. Mapping cost per exec is a handful of PTEs.
 *
 * The blob is a multi-PT_LOAD shared object; we honour the segments the
 * linker produced (copy filesz, zero-fill memsz) instead of assuming a
 * single-LOAD layout -- lld's segment splitting has changed across
 * releases and a loader that guesses is a loader that breaks on a
 * toolchain update. Everything maps R+X: nothing ever writes the image
 * (glibc only READS .dynamic; there are no relocations to apply). */

#include <tobyos/vdso.h>
#include <tobyos/proc.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/perf.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/page_fault.h>   /* page_ref_*: the shared-frame discipline */

/* Embedded by src/vdso_blob.S (.incbin of build/vdso_image.so). */
extern const uint8_t vdso_blob_start[];
extern const uint8_t vdso_blob_end[];

#define VDSO_MAX_PAGES 16

static uint64_t g_vdso_phys[VDSO_MAX_PAGES];
static uint32_t g_vdso_pages;
static uint64_t g_vvar_phys;
static volatile struct vdso_data *g_vvar;   /* kernel-side (HHDM) view */

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
#define PT_LOAD 1

void vdso_init(void) {
    size_t blob_sz = (size_t)(vdso_blob_end - vdso_blob_start);
    if (blob_sz < sizeof(struct elf64_ehdr)) {
        kprintf("[vdso] no embedded image (%lu bytes) -- disabled\n",
                (unsigned long)blob_sz);
        return;
    }
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)vdso_blob_start;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        kprintf("[vdso] embedded image is not ELF -- disabled\n");
        return;
    }
    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(vdso_blob_start + eh->e_phoff);

    uint64_t span = 0, min_vaddr = ~0ull;
    for (uint16_t i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr + ph[i].p_memsz > span)
                span = ph[i].p_vaddr + ph[i].p_memsz;
            if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
        }
    /* The image MUST be linked at a NONZERO base (--image-base=0x1000).
     * glibc's setup_vdso computes l_addr with `if (!l->l_addr) l_addr =
     * p_vaddr` -- a first PT_LOAD at vaddr 0 leaves l_addr unset, the
     * SECOND load's vaddr gets taken instead, and every table address
     * resolves ~0x1450 low: found live as a startup SIGSEGV in every
     * glibc process. The vaddr-0 page slot is deliberately empty; the
     * vvar occupies that virtual page instead (see vdso_map_root). */
    if (min_vaddr == 0 || min_vaddr >= 0x2000) {
        kprintf("[vdso] first PT_LOAD vaddr 0x%lx (want 0x1000) -- disabled\n",
                (unsigned long)min_vaddr);
        return;
    }
    uint32_t pages = (uint32_t)((span + 4095) / 4096);
    if (pages < 2 || pages > VDSO_MAX_PAGES) {
        kprintf("[vdso] image span %lu pages out of range -- disabled\n",
                (unsigned long)pages);
        return;
    }

    for (uint32_t i = 0; i < pages; i++) {
        g_vdso_phys[i] = pmm_alloc_page();
        if (!g_vdso_phys[i]) { g_vdso_pages = 0; return; }
        memset(pmm_phys_to_virt(g_vdso_phys[i]), 0, 4096);
        /* Permanent pin. Address-space teardown (free_subtree) frees any
         * present leaf whose refcount is <=1 -- with these frames at 0,
         * THE FIRST PROCESS EXIT handed the live global vDSO back to the
         * PMM, the heap recycled it, and every later teardown freed the
         * same now-heap frame again (found live: kmalloc GP on a smashed
         * freelist at the second boot spawn). The pin plus one reference
         * per mapping (vdso_map_root) keeps refs > 1 for every teardown
         * that can see the page. */
        page_ref_inc(g_vdso_phys[i]);
    }
    /* Copy each LOAD honouring vaddr/offset; memsz tail stays zero. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        for (uint64_t off = 0; off < ph[i].p_filesz; off++) {
            uint64_t va  = ph[i].p_vaddr + off;
            uint8_t *dst = (uint8_t *)pmm_phys_to_virt(g_vdso_phys[va / 4096]);
            dst[va % 4096] = vdso_blob_start[ph[i].p_offset + off];
        }
    }

    g_vvar_phys = pmm_alloc_page();
    if (!g_vvar_phys) { g_vdso_pages = 0; return; }
    memset(pmm_phys_to_virt(g_vvar_phys), 0, 4096);
    page_ref_inc(g_vvar_phys);          /* same pin as the blob frames */
    g_vvar = (volatile struct vdso_data *)pmm_phys_to_virt(g_vvar_phys);

    g_vdso_pages = pages;
    vdso_refresh();
    kprintf("[vdso] staged: %u pages + data page (blob %lu bytes, "
            "tsc=%u kHz)\n", pages, (unsigned long)blob_sz,
            (unsigned)perf_tsc_khz());
}

/* Publish {boot_tsc, tsc_khz, epoch offset} under the seqlock iff they
 * changed. boot_tsc moves on PM-timer recalibration; the epoch offset
 * latches once when the RTC is first read. perf.c exposes the pair
 * through perf_tsc_khz()/perf_boot_tsc(); the realtime offset comes from
 * the same converter the futex deadline path trusts. */
void vdso_refresh(void) {
    if (!g_vvar) return;
    extern uint64_t perf_boot_tsc(void);
    extern uint64_t lx_realtime_offset_ns(void);
    uint32_t khz = perf_tsc_khz();
    uint64_t bt  = perf_boot_tsc();
    uint64_t ep  = lx_realtime_offset_ns();
    if (g_vvar->tsc_khz == khz && g_vvar->boot_tsc == bt &&
        g_vvar->epoch_base_ns == ep)
        return;
    g_vvar->seq++;                                   /* odd: writer active */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_vvar->tsc_khz       = khz;
    g_vvar->boot_tsc      = bt;
    g_vvar->epoch_base_ns = ep;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_vvar->seq++;                                   /* even: stable */
}

uint64_t vdso_map_root(uint64_t cr3) {
    if (!cr3 || g_vdso_pages == 0) return 0;
    uint64_t saved = vmm_set_editor_root(cr3);
    /* VMM_SHARED: fork must never CoW or eager-copy these -- a privately
     * copied vvar page would freeze that child's clock constants at
     * fork time, silently drifting after the next TSC recalibration.
     *
     * Address plan: link vaddr V maps at (VDSO_BASE_VA - 0x1000) + V,
     * so the EHDR (vaddr 0x1000) sits exactly at VDSO_BASE_VA -- the
     * AT_SYSINFO_EHDR value -- and the image's empty vaddr-0 slot IS
     * the vvar page one page below it, which is exactly where the
     * blob's `__ehdr_start - 4096` computation looks. Staged page 0
     * (vaddr 0..0xfff, no LOAD content) is never mapped. */
    bool ok = vmm_map(VDSO_VVAR_VA, g_vvar_phys, 4096,
                      VMM_PRESENT | VMM_USER | VMM_NX | VMM_SHARED);
    for (uint32_t i = 1; ok && i < g_vdso_pages; i++)
        ok = vmm_map(VDSO_VVAR_VA + (uint64_t)i * 4096, g_vdso_phys[i],
                     4096, VMM_PRESENT | VMM_USER | VMM_SHARED); /* R+X */
    vmm_set_editor_root(saved);
    if (!ok) {
        kprintf("[vdso] map failed (cr3=0x%lx)\n", (unsigned long)cr3);
        return 0;
    }
    /* Each mapping takes one reference per frame; the owning address
     * space's teardown drops it (free_subtree's refs>1 arm). The
     * normalize-then-reference idiom is page_fault.c's own (fork's
     * PTE_SHARED arm): refcount 0 means "never tracked", so establish
     * the permanent pin first -- this ALSO self-heals if vdso_init ever
     * again runs before the refcount array exists. */
    if (page_ref_get(g_vvar_phys) == 0) page_ref_inc(g_vvar_phys);
    page_ref_inc(g_vvar_phys);
    for (uint32_t i = 1; i < g_vdso_pages; i++) {   /* page 0 never maps */
        if (page_ref_get(g_vdso_phys[i]) == 0) page_ref_inc(g_vdso_phys[i]);
        page_ref_inc(g_vdso_phys[i]);
    }
    return VDSO_BASE_VA;
}
