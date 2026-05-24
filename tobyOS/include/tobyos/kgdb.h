/* kgdb.h -- GDB remote serial protocol stub (Phase 8). */

#ifndef TOBYOS_KGDB_H
#define TOBYOS_KGDB_H

#include <tobyos/types.h>

/* Initialise the GDB stub on a serial port. */
void kgdb_init(void);

/* Trigger a breakpoint (enter the debugger). */
void kgdb_breakpoint(void);

/* Called from the trap/exception handler when an exception occurs.
 * `signal` is the GDB signal number, `regs` points to the saved
 * register file (16 x uint64_t: rax..r15, rip, rflags, cs, ss). */
void kgdb_handle_exception(int signal, uint64_t *regs);

/* Check if KGDB is connected/active. */
int kgdb_active(void);

#endif /* TOBYOS_KGDB_H */
