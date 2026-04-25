/* serial.h -- early debug output over COM1 + QEMU's 0xE9 debugcon.
 *
 * Both sinks are written from a single function so debug.log and
 * serial.log always stay in sync.
 */

#ifndef TOBYOS_SERIAL_H
#define TOBYOS_SERIAL_H

#include <tobyos/types.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
void serial_puts(const char *s);

#endif /* TOBYOS_SERIAL_H */
