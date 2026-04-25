/* pit.h -- 8254 Programmable Interval Timer (channel 0).
 *
 * Channel 0 is the system tick: it fires IRQ0 at the configured rate.
 * Real frequency = 1193182 Hz / divisor, so e.g. 100 Hz uses divisor
 * 11932 (which yields ~99.99 Hz; close enough).
 *
 * pit_init() registers an IRQ0 handler that increments a global tick
 * counter and EOIs the PIC. Don't forget to call pic_unmask(0) and
 * `sti` afterwards.
 */

#ifndef TOBYOS_PIT_H
#define TOBYOS_PIT_H

#include <tobyos/types.h>

#define PIT_BASE_FREQ_HZ 1193182u

void     pit_init(uint32_t hz);
uint64_t pit_ticks(void);
uint32_t pit_hz(void);

/* Busy-wait (with hlt between checks) until at least `ms` milliseconds
 * have passed. Requires interrupts to be enabled. */
void pit_sleep_ms(uint64_t ms);

#endif /* TOBYOS_PIT_H */
