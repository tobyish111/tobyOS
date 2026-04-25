/* idt.h -- 64-bit Interrupt Descriptor Table.
 *
 * In long mode each gate is 16 bytes. We install an interrupt-gate
 * (type 0xE, DPL 0) for vectors 0..31 pointing at the assembly stubs
 * exported by src/isr.S.
 *
 * Higher vectors (IRQs, syscalls) are filled in by later steps.
 */

#ifndef TOBYOS_IDT_H
#define TOBYOS_IDT_H

#include <tobyos/types.h>

void idt_init(void);

/* Address of the kernel's IDT pseudo-descriptor (10 bytes: limit:2 +
 * base:8). Used by smp.c to make each AP lidt the same IDT we run on. */
const void *idt_pseudo_descriptor(void);

#endif /* TOBYOS_IDT_H */
