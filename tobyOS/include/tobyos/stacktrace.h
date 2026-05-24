/* stacktrace.h -- Kernel stack trace capture (Phase 8). */

#ifndef TOBYOS_STACKTRACE_H
#define TOBYOS_STACKTRACE_H

#include <tobyos/types.h>

#define STACKTRACE_MAX_FRAMES 32

struct stack_frame {
    uint64_t rip;
    uint64_t rbp;
    const char *symbol;  /* NULL if unknown */
};

/* Capture the current call stack into `frames`. Returns the number of
 * frames captured (up to max_frames). */
int stacktrace_capture(struct stack_frame *frames, int max_frames);

/* Print the current stack trace to the kernel console. */
void stacktrace_print(void);

/* Print a stack trace starting from a given RBP/RIP (e.g. from a trap
 * frame or a crashed process context). */
void stacktrace_print_from(uint64_t rbp, uint64_t rip);

/* Register a kernel symbol for address resolution. Called during boot
 * to populate the symbol table (typically from an embedded linker map). */
void stacktrace_add_symbol(uint64_t addr, const char *name);

/* Initialise the stack trace subsystem. */
void stacktrace_init(void);

#endif /* TOBYOS_STACKTRACE_H */
