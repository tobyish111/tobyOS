/* pic.h -- legacy 8259 PIC driver.
 *
 * After pic_init():
 *   master IRQs 0..7  -> IDT vectors 0x20..0x27
 *   slave  IRQs 8..15 -> IDT vectors 0x28..0x2F
 *
 * All IRQs start masked. Use pic_unmask(irq) to enable them one at a
 * time. EOI must be sent at the END of every IRQ handler -- forgetting
 * this leaves the line latched and you'll never see another interrupt.
 */

#ifndef TOBYOS_PIC_H
#define TOBYOS_PIC_H

#include <tobyos/types.h>

#define PIC_IRQ_BASE  0x20  /* vector for IRQ 0  (master) */
#define PIC_IRQ_SLAVE 0x28  /* vector for IRQ 8  (slave)  */

void pic_init(void);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_send_eoi(uint8_t irq);

/* Park the legacy 8259A pair (mask all 16 IRQs at both IMRs). Called
 * by the IO APIC bring-up so a stray ISA-IRQ assert can't slip past
 * the new routing. The chips stay remapped to 0x20..0x2F so any
 * spurious-IRQ7 / IRQ15 still lands on a vector we own (not on a
 * CPU exception slot). */
void pic_mask_all(void);

#endif /* TOBYOS_PIC_H */
