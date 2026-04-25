/* pic.c -- 8259A Programmable Interrupt Controller (legacy PIC pair).
 *
 * On a PC there are two cascaded PICs. The master sits at I/O ports
 * 0x20/0x21 (command/data), the slave at 0xA0/0xA1, with the slave's
 * INT line wired into the master's IRQ2 pin.
 *
 * The chip is reprogrammed via four sequential writes to the data port
 * after sending ICW1 to the command port:
 *
 *   ICW1 (cmd)  : 0x11 = init + ICW4-needed + edge-triggered
 *   ICW2 (data) : vector offset (must be a multiple of 8)
 *   ICW3 (data) : master = bitmask of slave pins; slave = pin id
 *   ICW4 (data) : 0x01 = 8086/88 mode (vs. 8080)
 *
 * After remapping, IRQs land at the chosen vectors and we leave all
 * lines masked. The Interrupt Mask Register (IMR) lives at the data
 * port: bit N set = IRQ N masked.
 */

#include <tobyos/pic.h>
#include <tobyos/cpu.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01

void pic_init(void) {
    /* Start the initialisation sequence on both chips (in cascade). */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offsets. */
    outb(PIC1_DATA, PIC_IRQ_BASE);  io_wait();
    outb(PIC2_DATA, PIC_IRQ_SLAVE); io_wait();

    /* ICW3: tell master that the slave is on IRQ2 (bit 2 set);
     *       tell slave its cascade identity (= 2). */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: 8086/88 mode. */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask everything; callers will unmask the lines they own. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

void pic_unmask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) & ~(1u << bit)));
}

void pic_send_eoi(uint8_t irq) {
    /* For slave IRQs, EOI both chips so the master's IRQ2 line releases. */
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask_all(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
