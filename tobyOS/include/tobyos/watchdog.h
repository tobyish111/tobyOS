/* watchdog.h -- Milestone 28C kernel watchdog + hang detector.
 *
 * Three different "I'm alive" heartbeats feed the watchdog:
 *
 *   1. wdog_kick_kernel()    -- bumped from the PIT IRQ handler.
 *      Tells us "interrupts still fire". If THIS counter stops
 *      advancing the box is wedged at the IDT/PIC level (e.g.
 *      cli'd kernel busy-loop) and only an NMI/HW watchdog could
 *      catch it -- we just observe.
 *
 *   2. wdog_kick_sched()     -- bumped from sched_yield / sched_tick.
 *      Tells us "the scheduler is making progress". If kernel
 *      ticks advance but sched ticks don't for > timeout_ms,
 *      something is hogging the CPU in kernel context (a busy loop
 *      with IRQs enabled but no yield).
 *
 *   3. wdog_kick_proc(pid)   -- bumped from syscall_dispatch.
 *      Tells us "this user process is doing things". If a process
 *      runs for > timeout_ms without making any syscall and isn't
 *      sleeping, we treat it as hung. The watchdog handler sends
 *      it SIGKILL (signal 9 in our ABI) instead of panicking the
 *      whole kernel -- a single hung user proc shouldn't take down
 *      the box.
 *
 * The check itself runs from the PIT IRQ at low frequency (once per
 * second). It must NOT touch the heap, the VFS, or any spinlock that
 * could be held by interrupted code; it only reads counters and emits
 * via slog (which is itself IRQ-safe). */

#ifndef TOBYOS_WATCHDOG_H
#define TOBYOS_WATCHDOG_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Default timeout: 10 seconds. wdog_init() can override. The boot
 * harness uses a much shorter value (e.g. 2000 ms) so the test can
 * fire the bite quickly. */
#define WDOG_DEFAULT_TIMEOUT_MS 10000u

void wdog_init(uint32_t timeout_ms);
bool wdog_ready(void);
uint32_t wdog_timeout_ms(void);
void wdog_set_timeout_ms(uint32_t timeout_ms);

/* Heartbeat sources. Each one is a single load+store, IRQ-safe. */
void wdog_kick_kernel(void);
void wdog_kick_sched(void);
void wdog_kick_proc(int pid);

/* Periodic check: scan counters, fire bite events as needed. Safe to
 * call from PIT IRQ. Throttles itself to ~1 Hz internally. */
void wdog_check(void);

/* Snapshot for userland / boot harness. */
void wdog_status(struct abi_wdog_status *out);

/* M28C boot-test trigger. Spins in kernel context with IRQs enabled
 * (hlt-loop) for `ms` milliseconds so the watchdog detects a sched
 * stall. Returns when the synthetic stall is over. Used by the M28C
 * harness to validate the bite path without crashing the system. */
void wdog_simulate_kernel_stall(uint32_t ms);

/* Force-fire a bite event from outside the periodic check. Used by
 * userland `wdogtest --bite` and by the boot harness. */
void wdog_record_event(uint32_t kind, int pid, const char *reason);

#endif /* TOBYOS_WATCHDOG_H */
