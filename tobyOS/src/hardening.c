/* hardening.c -- Kernel hardening: SMEP, SMAP, NX enforcement (Phase 7).
 *
 * SMEP (Supervisor Mode Execution Prevention): prevents kernel from
 * executing code in user-space pages. Set via CR4 bit 20.
 *
 * SMAP (Supervisor Mode Access Prevention): prevents kernel from
 * reading/writing user-space pages unless explicitly enabled. CR4 bit 21.
 *
 * NX/DEP (No-Execute / Data Execution Prevention): enforced per-page via
 * the NX bit (bit 63) in page table entries. The kernel already sets NX
 * on data pages; this module ensures IA32_EFER.NXE is enabled.
 */

#include <tobyos/printk.h>
#include <tobyos/cpu.h>
#include <tobyos/types.h>

#define CPUID_SMEP_BIT    (1U << 7)   /* EBX from leaf 7, sub 0 */
#define CPUID_SMAP_BIT    (1U << 20)  /* EBX from leaf 7, sub 0 */
#define CPUID_NX_BIT      (1U << 20)  /* EDX from ext leaf 0x80000001 */

#define CR4_SMEP          (1UL << 20)
#define CR4_SMAP          (1UL << 21)
#define MSR_IA32_EFER     0xC0000080UL
#define EFER_NXE          (1UL << 11)

static int g_smep_available;
static int g_smap_available;
static int g_nx_available;

static inline void cpuid_query(uint32_t leaf, uint32_t sub,
                               uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(sub));
}

void hardening_init(void) {
    uint32_t a, b, c, d;

    /* Check structured extended features (leaf 7) */
    cpuid_query(7, 0, &a, &b, &c, &d);
    g_smep_available = !!(b & CPUID_SMEP_BIT);
    g_smap_available = !!(b & CPUID_SMAP_BIT);

    /* Check NX support (extended leaf 0x80000001) */
    cpuid_query(0x80000001, 0, &a, &b, &c, &d);
    g_nx_available = !!(d & CPUID_NX_BIT);

    uint64_t cr4 = read_cr4();

    if (g_smep_available) {
        cr4 |= CR4_SMEP;
        kprintf("[hardening] SMEP enabled\n");
    } else {
        kprintf("[hardening] SMEP not available\n");
    }

    if (g_smap_available) {
        cr4 |= CR4_SMAP;
        kprintf("[hardening] SMAP enabled\n");
    } else {
        kprintf("[hardening] SMAP not available\n");
    }

    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    if (g_nx_available) {
        uint64_t efer = rdmsr(MSR_IA32_EFER);
        efer |= EFER_NXE;
        wrmsr(MSR_IA32_EFER, efer);
        kprintf("[hardening] NX/DEP enabled (IA32_EFER.NXE)\n");
    } else {
        kprintf("[hardening] NX/DEP not available\n");
    }
}

int hardening_smep_enabled(void) { return g_smep_available; }
int hardening_smap_enabled(void) { return g_smap_available; }
int hardening_nx_enabled(void)   { return g_nx_available; }
