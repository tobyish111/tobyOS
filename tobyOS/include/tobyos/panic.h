/* panic.h -- last-resort failure path.
 *
 * Milestone 28B upgraded `kpanic()` from a one-liner halt into a full
 * crash-reporting handler:
 *
 *   - Captures GP-register snapshot (RAX..R15, RIP/RSP/RBP, RFLAGS,
 *     CR2/CR3) at the panic site.
 *   - Identifies the current process (pid, name, ppid, syscall count).
 *   - Walks the kernel stack with frame-pointer following to print a
 *     short symbolic-ish trace (RIP values; userland will resolve them
 *     against the kernel symbol table when /bin/crashinfo runs).
 *   - Re-paints the framebuffer console as a "panic screen" -- white
 *     text on a red background banner so a person standing in front
 *     of the box knows it died.
 *   - Best-effort writes a serialized crash dump to /data/crash/last
 *     so a sysadmin can grab it on the next boot.
 *   - Dumps the slog ring (last N entries) so the surrounding context
 *     ends up in the serial log alongside the panic banner.
 *
 * Re-entrant panics are still guarded; the inner panic just halts.
 */

#ifndef TOBYOS_PANIC_H
#define TOBYOS_PANIC_H

#include <tobyos/types.h>

__attribute__((noreturn, format(printf, 3, 4)))
void kpanic_at(const char *file, int line, const char *fmt, ...);

#define kpanic(...) kpanic_at(__FILE__, __LINE__, __VA_ARGS__)

#define KASSERT(cond)                                                     \
    do {                                                                  \
        if (!(cond)) {                                                    \
            kpanic_at(__FILE__, __LINE__, "assertion failed: %s", #cond); \
        }                                                                 \
    } while (0)

/* M28B: query whether we are currently inside the panic handler. Used
 * by IRQ paths to skip work that would otherwise re-enter the panic
 * print path (e.g. hot-plug ticking, timer fan-out). */
bool kpanic_in_progress(void);

/* M28B: optional opt-in. Some boots want to provoke a controlled
 * panic to validate the crash plumbing without having to wedge the
 * box for real. The shell-level `crashtest` builtin and the userland
 * `crashtest` ELF call this. The argument is one of:
 *   "assert"   -- triggers KASSERT(false)
 *   "deref"    -- attempts a NULL pointer dereference
 *   "kpanic"   -- direct kpanic() call
 *   "div0"     -- integer divide by zero
 *
 * Returns -1 on unknown trigger; on success the function never
 * returns (panic). Callable from kernel code.  */
__attribute__((noreturn))
void kpanic_self_test(const char *trigger);

/* M28B: arm the boot-time crash test. _start checks this flag right
 * before the M28A harness so a test boot can deliberately trip the
 * panic path. The flag is wired by m28b_boot_request_panic() which
 * is gated behind a build-time KCONFIG_BOOT_PANIC_TEST symbol. */
bool kpanic_boot_request_pending(void);
const char *kpanic_boot_request_trigger(void);
void kpanic_boot_request(const char *trigger);

#endif /* TOBYOS_PANIC_H */
