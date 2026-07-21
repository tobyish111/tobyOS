/* serial.h -- early debug output over COM1 + QEMU's 0xE9 debugcon.
 *
 * Both sinks are written from serial_putc(). COM1 is initialised on the
 * first write (and serial_init() is idempotent) so the entire boot path
 * from _start onward lands on the host serial backend when QEMU is
 * started with -serial.
 */

#ifndef TOBYOS_SERIAL_H
#define TOBYOS_SERIAL_H

#include <tobyos/types.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
void serial_puts(const char *s);

/* Arm non-blocking async TX (ring buffer + COM1 THRE IRQ). Call once after
 * the IRQ facade is up (post irq_switch_to_ioapic). Until then, and after
 * serial_set_sync_mode(), serial_putc() uses a bounded spin. */
void serial_enable_tx_irq(void);
/* Log bytes silently discarded because the TX ring was full. Non-zero means the
 * serial log is INCOMPLETE -- in particular, "the log stops here" is then NOT
 * evidence that the system stopped there. */
uint64_t serial_dropped(void);

/* Drain the TX ring into the UART FIFO without spinning. Pump from the
 * pid-0 idle loop so output flows even if the THRE IRQ never routes. */
void serial_tx_pump(void);

/* Force synchronous bounded-spin output and flush the ring in order. For
 * the panic path (IRQs off, scheduler dead) so the full message reaches
 * the wire without depending on the THRE IRQ or the idle-loop pump. */
void serial_set_sync_mode(void);

/* Configured COM1 line rate (bits/s). The capture terminal on the other
 * end of the cable must match this (8 data bits, no parity, 1 stop). */
unsigned serial_baud(void);

#endif /* TOBYOS_SERIAL_H */
