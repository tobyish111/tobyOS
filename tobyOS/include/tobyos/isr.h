/* isr.h -- interrupt service routine plumbing.
 *
 * `struct regs` mirrors EXACTLY what the assembly stub leaves on the
 * stack before calling the C dispatcher. Field order is in increasing
 * memory address: first member = lowest address = LAST thing pushed by
 * the stub, last member = highest address = FIRST thing pushed by CPU.
 *
 * If you change this struct, you MUST keep src/isr_stubs.S in sync.
 */

#ifndef TOBYOS_ISR_H
#define TOBYOS_ISR_H

#include <tobyos/types.h>

struct regs {
    /* Pushed by our common stub, in reverse order so they land here: */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    /* Pushed by the per-vector stub: */
    uint64_t vector;
    uint64_t error_code;

    /* Pushed by the CPU automatically on interrupt entry: */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;     /* user RSP (or kernel RSP if no privilege change) */
    uint64_t ss;
};

/* Per-vector callback. NULL = use the default exception handler.
 * Vector range is 0..255. */
typedef void (*isr_handler_fn)(struct regs *r);

void isr_register(uint8_t vector, isr_handler_fn fn);

/* Called from the asm common stub. Public only so isr_stubs.S can reference it. */
void isr_dispatch(struct regs *r);

#endif /* TOBYOS_ISR_H */
