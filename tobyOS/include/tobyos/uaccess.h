/* uaccess.h -- SMAP user-memory access helpers.
 *
 * When CR4.SMAP is enabled, any supervisor-mode (ring 0) access to a
 * user-accessible page faults (#PF) unless RFLAGS.AC is set. The kernel
 * toggles AC with the SMAP instructions stac (set) / clac (clear).
 *
 * Model used by tobyOS (a pragmatic middle ground -- not Linux's per-copy
 * accessors, but far better than SMAP fully off):
 *
 *   - The SYSCALL trampoline (syscall_entry.S) opens a uaccess window with
 *     stac for the whole syscall body and closes it with clac before
 *     SYSRETQ. So syscall handlers may dereference user pointers directly,
 *     as they always have. The window survives blocking context switches
 *     because proc_context_switch saves/restores RFLAGS (pushfq/popfq).
 *
 *   - Code that touches user memory OUTSIDE a syscall -- the ELF loader,
 *     argv/envp stack setup, the copy-on-write page copier -- must bracket
 *     the access with uaccess_begin()/uaccess_end(). These SAVE and RESTORE
 *     the prior AC state, so they nest correctly when the same code runs
 *     inside an already-open window (e.g. the loader invoked from execve).
 *
 * CRITICAL: stac/clac #UD on CPUs that don't implement SMAP. The default
 * QEMU CPU has no SMAP, so we gate every stac/clac on g_smap_on (set by
 * hardening_init only when CR4.SMAP was actually enabled). When SMAP is off
 * these helpers are no-ops and the kernel behaves exactly as before. */

#ifndef TOBYOS_UACCESS_H
#define TOBYOS_UACCESS_H

#include <tobyos/types.h>

/* 1 once CR4.SMAP is live; 0 otherwise. Defined in hardening.c. Also read
 * from syscall_entry.S, so keep it a plain byte. */
extern volatile uint8_t g_smap_on;

static inline void uaccess_stac(void) {
    if (g_smap_on) __asm__ volatile("stac" ::: "cc", "memory");
}
static inline void uaccess_clac(void) {
    if (g_smap_on) __asm__ volatile("clac" ::: "cc", "memory");
}

/* Open a uaccess window, returning the prior RFLAGS for uaccess_end().
 * pushfq/popfq are valid on every CPU; only the stac is SMAP-gated. */
static inline unsigned long uaccess_begin(void) {
    unsigned long flags;
    __asm__ volatile("pushfq\n\t"
                     "popq %0"
                     : "=r"(flags) :: "memory");
    uaccess_stac();
    return flags;
}

/* Restore the RFLAGS saved by uaccess_begin (clearing AC iff it was clear),
 * which correctly closes a nested window without disturbing an outer one. */
static inline void uaccess_end(unsigned long flags) {
    __asm__ volatile("pushq %0\n\t"
                     "popfq"
                     :: "r"(flags) : "cc", "memory");
}

#endif /* TOBYOS_UACCESS_H */
