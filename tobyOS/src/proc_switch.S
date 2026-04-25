/* proc_switch.S -- cooperative context switch + first-time iretq.
 *
 * Two entry points:
 *
 *   proc_context_switch(uint64_t *save_old_rsp,
 *                       uint64_t  new_rsp,
 *                       uint64_t  new_cr3)
 *
 *     Saves callee-saved registers (per SysV: rbx, rbp, r12-r15) onto
 *     the *current* kernel stack, stashes the resulting RSP through
 *     `save_old_rsp`, switches RSP to `new_rsp`, conditionally
 *     reloads CR3, restores callee-saved regs from the new stack, and
 *     `ret`s. The `ret` pops the next 8 bytes off the new stack as
 *     the resume RIP.
 *
 *     We only write CR3 if it actually changed. Writing CR3 flushes
 *     all non-global TLB entries; for switches between two threads
 *     that share an address space (or for proc 0 yielding to itself)
 *     skipping the write is a noticeable win.
 *
 *   proc_enter_user_asm(uint64_t user_rip, uint64_t user_rsp)
 *
 *     Builds an IRETQ frame for ring 3 (CS=USER_CS|3, SS=USER_DS|3,
 *     RFLAGS=IF|reserved) and iretqs. Does NOT save any kernel
 *     resume context -- in milestone 5 the way out of user mode is
 *     proc_exit (which sched_yields to a different thread), not a
 *     longjmp back into a saved kernel frame.
 */

.intel_syntax noprefix
.code64

.section .text

/* ============================================================== */
/* proc_context_switch                                             */
/* ============================================================== */
.global proc_context_switch
.type   proc_context_switch, @function
proc_context_switch:
    /* rdi = uint64_t *save_old_rsp
     * rsi = uint64_t  new_rsp
     * rdx = uint64_t  new_cr3
     *
     * Save callee-saved regs on the current kernel stack. The order
     * here MUST match the fake initial frame proc_create installs --
     * push rbp, push rbx, push r12, push r13, push r14, push r15.
     *
     * Also save RFLAGS so the resumed context has the IF / DF state
     * it expected when it was last switched out. Without this, a
     * process that yields from the SYSCALL path (where the CPU has
     * already cleared RFLAGS.IF via FMASK) would resume the PEER
     * process with IRQs disabled -- which is catastrophic if the peer
     * was about to `hlt()` in idle_loop (deadlock: hlt with IF=0 never
     * wakes). `pushfq` doesn't change any flags, and popfq restores
     * them wholesale on the way out. */
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    pushfq

    /* Hand off our kernel SP to the caller's slot, then jump onto the
     * new process's kernel stack. */
    mov [rdi], rsp
    mov rsp, rsi

    /* Conditional CR3 write. Same address space -> skip the TLB
     * flush. */
    mov rax, cr3
    cmp rax, rdx
    je 1f
    mov cr3, rdx
1:
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

/* ============================================================== */
/* proc_enter_user_asm -- iretq into ring 3                        */
/* ============================================================== */
.global proc_enter_user_asm
.type   proc_enter_user_asm, @function
/* __attribute__((noreturn)) void proc_enter_user_asm(
 *     uint64_t user_rip,   // rdi
 *     uint64_t user_rsp);  // rsi
 */
proc_enter_user_asm:
    /* Build the IRETQ frame on the current kernel stack.
     * Stack layout the CPU expects (top -> bottom; CPU pops top-first):
     *   RIP, CS, RFLAGS, RSP, SS
     * RFLAGS = 0x202 = IF (bit 9) | mandatory reserved bit 1.
     * IF=1 ensures the user is preemptible by IRQs (PIT, keyboard). */
    mov rax, 0x1B               /* USER_DS | RPL 3 */
    push rax                    /* SS */
    push rsi                    /* RSP */
    mov rax, 0x202
    push rax                    /* RFLAGS */
    mov rax, 0x23               /* USER_CS | RPL 3 */
    push rax                    /* CS */
    push rdi                    /* RIP */

    /* Reload data segments to user selectors -- mostly cosmetic in
     * long mode, but keeps the descriptor cache tidy. */
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    iretq                       /* drop to CPL 3 */

.section .note.GNU-stack, "", @progbits
