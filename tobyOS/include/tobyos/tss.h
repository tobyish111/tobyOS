/* tss.h -- 64-bit Task State Segment.
 *
 * In long mode the TSS is mostly vestigial -- task switching is gone --
 * but the CPU still consults it for two things we care about:
 *
 *   1. RSP0 (the stack pointer used when an interrupt or trap raises
 *      CPL from 3 -> 0). Without this, an IRQ that fires while user
 *      code is running would push the trap frame onto the *user* RSP
 *      and the kernel would page-fault inside the IRQ handler.
 *
 *   2. The optional IST stacks (IST1..IST7). We don't use them yet --
 *      double-fault hardening can come later -- but the slots are
 *      reserved in the struct.
 *
 * tss_init() reserves a small kernel "interrupt stack", points
 * TSS.RSP0 at its top, patches the GDT TSS descriptor, and executes
 * `ltr $GDT_TSS_SEL`. After this, both the IRQ path (interrupt entry
 * from CPL=3) and the SYSCALL path (which grabs the same address from
 * a global) share one safe kernel stack.
 *
 * The kernel itself runs on the bootloader-provided stack until that
 * first ring-3 transition. We never *return* to that bootloader stack
 * -- once SYSCALL/IRET kicks in, the IRQ stack becomes the only kernel
 * stack we use.
 */

#ifndef TOBYOS_TSS_H
#define TOBYOS_TSS_H

#include <tobyos/types.h>

void tss_init(void);

/* Top (highest address) of the kernel interrupt stack. SYSCALL entry
 * loads RSP from this on entry from user mode. Same value lives in
 * TSS.RSP0 for the IRQ path. */
uint64_t tss_kernel_rsp_top(void);

/* Update TSS.RSP0 -- the stack the CPU loads when an interrupt or
 * exception raises CPL from 3 -> 0. The scheduler calls this on every
 * context switch so an IRQ that arrives in user mode lands on the
 * *current* process's per-process kernel stack, not on a stale one. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* TOBYOS_TSS_H */
