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

/* 1 once CR4.SMAP is actually live. Read by the uaccess helpers
 * (include/tobyos/uaccess.h) and syscall_entry.S to gate stac/clac, which
 * #UD on CPUs without SMAP. Plain byte so the .S side can `cmpb` it. */
volatile uint8_t g_smap_on = 0;

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

    /* SMAP (#PF on any supervisor-mode access to a user page unless the AC
     * flag is set) is now ENABLED behind a uaccess window.
     *
     * The kernel touches user memory in two contexts:
     *   - inside a syscall: the SYSCALL trampoline (syscall_entry.S) brackets
     *     the whole syscall body in stac/clac, so handlers keep dereferencing
     *     user pointers directly. The window survives blocking switches
     *     (proc_context_switch saves/restores RFLAGS).
     *   - outside a syscall: the ELF loader, argv/envp stack setup, and the
     *     COW page copier use uaccess_begin()/uaccess_end() (see uaccess.h).
     *
     * stac/clac #UD on CPUs without SMAP, so they are all gated on g_smap_on,
     * which we only set after CR4.SMAP is confirmed live below. On QEMU's
     * default (no-SMAP) CPU this stays 0 and every stac/clac is skipped, so
     * the default boot path is byte-for-byte unchanged. */
    if (g_smap_available) {
        cr4 |= CR4_SMAP;
        kprintf("[hardening] SMAP enabled (uaccess window active)\n");
    } else {
        kprintf("[hardening] SMAP not available\n");
    }

    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    /* Only now that CR4.SMAP is actually set is it safe to let stac/clac
     * execute. Order matters: the mov-to-cr4 above must complete first. */
    if (g_smap_available) g_smap_on = 1;

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
