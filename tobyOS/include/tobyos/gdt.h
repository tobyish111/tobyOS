/* gdt.h -- 64-bit Global Descriptor Table.
 *
 * In long mode the segmentation hardware is mostly inert (base = 0,
 * limit ignored), but the CPU still requires a valid GDT before we can
 * install an IDT, run a TSS, or enter user mode.
 *
 * Slot order is forced by SYSCALL/SYSRET (milestone 3D):
 *
 *   index 0  selector 0x00  null
 *   index 1  selector 0x08  kernel code (L=1, DPL=0)   STAR[47:32]
 *   index 2  selector 0x10  kernel data (DPL=0)        = STAR[47:32]+8
 *   index 3  selector 0x1B  user   data (DPL=3)        STAR[63:48]+8 | 3
 *   index 4  selector 0x23  user   code (L=1, DPL=3)   STAR[63:48]+16| 3
 *   index 5  selector 0x28  TSS lo (16 bytes total: takes slots 5+6)
 *   index 6  ----           TSS hi  (upper 8 bytes of the system desc)
 *
 * The user-data-then-user-code ordering is NOT optional. SYSRETQ derives
 * both selectors from STAR[63:48] with fixed +8 / +16 offsets, so user
 * data MUST land at the lower selector value. Don't reorder these.
 *
 * tss.c calls gdt_install_tss() to write the 16-byte TSS descriptor into
 * slots 5+6 once the TSS struct's address is known.
 */

#ifndef TOBYOS_GDT_H
#define TOBYOS_GDT_H

#include <tobyos/types.h>

#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_DS   0x1B
#define GDT_USER_CS   0x23
#define GDT_TSS_SEL   0x28

void gdt_init(void);

/* Patch the 16-byte TSS descriptor at slots 5+6 to point at `tss_base`
 * with the given byte limit. Called by tss_init after the TSS struct is
 * laid out. */
void gdt_install_tss(uint64_t tss_base, uint32_t tss_limit);

/* Read the current CS / DS selectors -- handy for verifying the reload. */
uint16_t cpu_read_cs(void);
uint16_t cpu_read_ds(void);

/* Address of the kernel's GDT pseudo-descriptor (10 bytes: limit:2 +
 * base:8). Used by smp.c to copy into the AP trampoline so each AP
 * lgdt's the same GDT the BSP runs on. */
const void *gdt_pseudo_descriptor(void);

#endif /* TOBYOS_GDT_H */
