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
     * long mode, but keeps the descriptor cache tidy. We deliberately do
     * NOT reload GS: loading a selector rewrites the segment's hidden base
     * from its GDT descriptor (base 0), which would wipe this CPU's per-CPU
     * GS base that the SYSCALL trampoline relies on (gs:[0]=syscall_rsp).
     * tobyOS user code uses FS for TLS, never GS, so leaving GS pointed at
     * per-CPU kernel data is safe (no swapgs scheme needed). */
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax

    /* Zero EVERY general-purpose register before the descent to CPL3.
     * RSP is excluded -- it comes from the IRETQ frame pushed above.
     *
     * This is required by the x86-64 SysV program-entry contract, not just
     * hygiene. The ABI specifies that at process entry %rdx holds "a function
     * pointer that the application should register with atexit", and Linux
     * zeroes all GP registers so that pointer reads as NULL. glibc's _start
     * takes %rdx as `rtld_fini` and __libc_start_main does:
     *
     *     if (rtld_fini != NULL) __cxa_atexit (rtld_fini, NULL, NULL);
     *
     * We were leaving whatever the kernel last had in %rdx, so every STATIC
     * glibc binary registered a garbage kernel value as an atexit handler and
     * jumped to it from __run_exit_handlers -- doing all of its real work
     * correctly and then dying on the way out. That is the LXGLIBC gate's
     * "exit=-1": the observed value was 0x...61000, this process's PML4
     * physical address, deterministic per boot, which is also why the crash
     * looked like a mysterious constant rather than random garbage.
     *
     * Dynamically-linked binaries were immune, which is exactly why this hid
     * for so long: ld.so sets %rdx to a real _dl_fini before jumping to the
     * program entry, overwriting the leak. Only the static path was exposed.
     *
     * It is also a kernel->user INFORMATION LEAK in its own right: ring 3 saw
     * raw kernel register contents (here, a physical page-table address).
     *
     * proc_enter_user_thread_asm already did this; the clearing was simply
     * never applied to the process-entry path. Both are now consistent. */
    xor eax, eax                /* 32-bit xor zero-extends: clears all 64 */
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx                /* <-- the ABI-mandated one (rtld_fini) */
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d,  r8d
    xor r9d,  r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d

    /* SWAPGS: first descent to CPL3. do_switch put this proc's user GS base
     * (TEB for a Win32 PE, else &percpu) in the IA32_KERNEL_GS_BASE shadow;
     * swap it into the active GS base for ring 3. Mirror swaps live in the
     * syscall + ISR entry paths. */
    swapgs
    iretq                       /* drop to CPL 3 */

/* ============================================================== */
/* proc_enter_user_thread_asm -- iretq into ring 3 with arg in RDI */
/* ============================================================== */
.global proc_enter_user_thread_asm
.type   proc_enter_user_thread_asm, @function
/* void proc_enter_user_thread_asm(
 *     uint64_t user_rip,   // rdi
 *     uint64_t user_rsp,   // rsi
 *     uint64_t arg);       // rdx -> becomes RDI in ring 3
 */
proc_enter_user_thread_asm:
    /* Save the thread arg before we clobber registers */
    mov rcx, rdx

    /* Build IRETQ frame */
    mov rax, 0x1B               /* USER_DS | RPL 3 */
    push rax                    /* SS */
    push rsi                    /* RSP */
    mov rax, 0x202
    push rax                    /* RFLAGS */
    mov rax, 0x23               /* USER_CS | RPL 3 */
    push rax                    /* CS */
    push rdi                    /* RIP */

    /* Set data segments. NOT GS -- see proc_enter_user_asm: reloading GS
     * would clobber this CPU's per-CPU GS base used by the SYSCALL path. */
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax

    /* Set RDI = arg for the thread function, and zero everything else.
     * rbx/rbp/r12-r15 were previously left un-cleared here -- the same
     * kernel->user register leak fixed in proc_enter_user_asm above, just
     * narrower (a thread entry has no rtld_fini contract, so it did not
     * crash anything; it was still a leak). */
    mov rdi, rcx
    xor esi, esi
    xor edx, edx
    xor ecx, ecx
    xor eax, eax
    xor ebx, ebx
    xor ebp, ebp
    xor r8d,  r8d
    xor r9d,  r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d

    /* SWAPGS: first descent to CPL3 (see proc_enter_user_asm). */
    swapgs
    iretq

.section .note.GNU-stack, "", @progbits
