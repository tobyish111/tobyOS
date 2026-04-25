/* serial.c -- COM1 driver + QEMU debugcon mux.
 *
 * Every byte goes to BOTH 0xE9 (debugcon, no init required, captured by
 * `-debugcon file:debug.log`) and COM1 once initialised. That way even
 * a one-character write before serial_init() shows up in debug.log.
 */

#include <tobyos/serial.h>
#include <tobyos/cpu.h>

#define COM1            0x3F8
#define COM1_DATA       (COM1 + 0)
#define COM1_INT_EN     (COM1 + 1)
#define COM1_FIFO_CTRL  (COM1 + 2)
#define COM1_LINE_CTRL  (COM1 + 3)
#define COM1_MODEM_CTRL (COM1 + 4)
#define COM1_LINE_STAT  (COM1 + 5)

#define LINE_STAT_THR_EMPTY 0x20

#define DBGCON_PORT 0xE9

static bool s_serial_ready = false;

static inline void dbgcon_putc(char c) {
    outb(DBGCON_PORT, (uint8_t)c);
}

void serial_init(void) {
    outb(COM1_INT_EN,     0x00);  /* mask all UART interrupts */
    outb(COM1_LINE_CTRL,  0x80);  /* DLAB on -> baud divisor */
    outb(COM1_DATA,       0x03);  /* divisor low  (38400 baud) */
    outb(COM1_INT_EN,     0x00);  /* divisor high */
    outb(COM1_LINE_CTRL,  0x03);  /* 8N1, DLAB off */
    outb(COM1_FIFO_CTRL,  0xC7);  /* enable + clear FIFO, 14-byte threshold */
    outb(COM1_MODEM_CTRL, 0x0B);  /* DTR/RTS/OUT2 -- IRQs gated by OUT2 */
    s_serial_ready = true;
}

void serial_putc(char c) {
    /* debugcon always on -- never blocks, never fails */
    dbgcon_putc(c);

    if (!s_serial_ready) return;
    while (!(inb(COM1_LINE_STAT) & LINE_STAT_THR_EMPTY)) { /* spin */ }
    outb(COM1_DATA, (uint8_t)c);
}

void serial_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) serial_putc(s[i]);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}
