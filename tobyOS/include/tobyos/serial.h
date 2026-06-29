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

/* Configured COM1 line rate (bits/s). The capture terminal on the other
 * end of the cable must match this (8 data bits, no parity, 1 stop). */
unsigned serial_baud(void);

#endif /* TOBYOS_SERIAL_H */
