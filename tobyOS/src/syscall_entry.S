/* syscall_entry.S -- the SYSCALL trampoline.
 *
 * One entry point:
 *
 *   syscall_entry  -- LSTAR target: where the CPU lands when user code
 *                     executes the `syscall` insn.
 *
 * One RIP-relative global plus one momentary scratch coordinate the
 * SYSCALL stack swap:
 *
 *   g_kernel_syscall_rsp  -- top of the kernel IRQ/syscall stack.
 *                            Updated on every context switch by the
 *                            scheduler so it points at the *current*
 *                            process's per-process kernel stack.
 *   g_user_rsp_scratch    -- 8-byte scratch used ONLY for the literal
 *                            `mov [scratch], rsp; mov rsp, kstack;
 *                            push [scratch]` window at entry. The
 *                            saved user RSP is then living on this
 *                            process's per-process kernel stack, so
 *                            it survives any context switch that may
 *                            happen inside the syscall body.
 *
 * Why per-process and not global: if the parent issues a blocking
 * syscall (e.g. waitpid) and the scheduler runs the child, the child's
 * own SYSCALL would overwrite a single global "saved user RSP" slot --
 * and when the parent eventually resumes, it would `mov rsp, [global]`
 * and SYSRETQ to user with the CHILD's RSP, corrupting the parent's
 * stack. Stashing it on the kernel stack avoids that entirely.
 *
 * IF is cleared by the CPU on SYSCALL entry (we set FMASK to do that),
 * so the brief `[scratch]` window is non-preemptible.
 *
 * Stack layout below `g_kernel_syscall_rsp` (highest to lowest, after
 * SYSCALL entry preamble has finished):
 *
 *      kstack_top -> [user RSP]            \  16-byte
 *                    [alignment pad]       /  pair (mod16==0 here)
 *                    [rcx = user RIP]      \
 *                    [r11 = user RFLAGS]    |
 *                    [rdi]                  |
 *                    [rsi]                  |
 *                    [rdx]                  |  14 saved
 *                    [r10]                  |  registers
 *                    [r8]                   |  (112 bytes)
 *                    [r9]                   |
 *                    [rbx]                  |
 *                    [rbp]                  |
 *                    [r12]                  |
 *                    [r13]                  |
 *                    [r14]                  |
 *                    [r15]                 /
 *      rsp here -> ----- (mod16==0, ready for `call`) -----
 *
 * The first-time iretq into ring 3 (was enter_user_mode here) lives in
 * proc_switch.S as proc_enter_user_asm. The unwind out of user mode is
 * now proc_exit() (no longjmp -- the scheduler picks the next thread).
 *
 * We don't use SWAPGS / per-CPU GS_BASE: one CPU runs userspace, and a
 * single global per-process kernel-stack pointer keeps the code simple.
 */

.intel_syntax noprefix
.code64

.section .data
.global g_kernel_syscall_rsp
.global g_user_rsp_scratch
g_kernel_syscall_rsp: .quad 0
g_user_rsp_scratch:   .quad 0

.section .text

/* ============================================================== */
/* SYSCALL entry                                                   */
/* ============================================================== */
.global syscall_entry
.type   syscall_entry, @function
syscall_entry:
    /* Entry state set by the CPU on `syscall`:
     *   rcx = user RIP (clobbers user's rcx)
     *   r11 = user RFLAGS (clobbers user's r11)
     *   CS  = STAR[47:32]            -- our kernel CS
     *   SS  = STAR[47:32] + 8        -- our kernel SS
     *   RFLAGS &= ~FMASK             -- IF/DF/TF cleared
     *   RSP unchanged                -- still pointing into user stack
     */

    /* Stash user RSP via a tiny scratch window, then swap to the
     * per-process kernel syscall stack and immediately push the saved
     * user RSP. From that point on the user RSP lives on a *per-
     * process* kernel stack, so a context switch inside the syscall
     * body (or another process executing SYSCALL while we're blocked
     * in waitpid/etc.) cannot overwrite it.
     *
     * IMPORTANT (SysV stack alignment): the C dispatcher expects RSP
     * to be 16-byte aligned BEFORE the `call` (so the callee sees
     * RSP%16==8 after CALL pushes the 8-byte return address). The
     * 14 saved-reg pushes below leave RSP at mod16==0; the user-RSP
     * push alone would unbalance that to mod16==8 and break MOVAPS
     * etc. inside printk. We therefore push TWO 8-byte slots here:
     * the live user RSP plus an alignment pad. The exit path skips
     * the pad and reloads RSP from the user-RSP slot. */
    mov [rip + g_user_rsp_scratch], rsp
    mov rsp, [rip + g_kernel_syscall_rsp]
    push qword ptr [rip + g_user_rsp_scratch]   /* user RSP -> kstack */
    sub rsp, 8                                  /* 16-B alignment pad */

    /* Save the regs we'll touch in the body. We need to preserve
     *   rcx, r11    -- SYSRETQ reads these to return to user
     *   rbx, rbp, r12-r15 -- callee-saved per SysV (the C dispatcher
     *                        is allowed to clobber them otherwise)
     *   rdi, rsi, rdx, r10, r8, r9 -- user-side syscall ABI says ALL
     *                  GP regs survive the call (matching Linux). The
     *                  compiler treats inline `syscall` asm as preserving
     *                  every reg not in its clobbers list, so a user
     *                  program legitimately keeps live values in those
     *                  registers across many syscalls (e.g. clang holds
     *                  an `fd` in rdi for the whole life of a function).
     *                  Push BEFORE the SysV register shuffle so what we
     *                  pop back are the original user values, not the
     *                  C-clobbered ones. */
    push rcx                /* user RIP */
    push r11                /* user RFLAGS */
    push rdi                /* user-side syscall arg1 (preserve for user) */
    push rsi                /* user-side syscall arg2 */
    push rdx                /* user-side syscall arg3 */
    push r10                /* user-side syscall arg4 */
    push r8                 /* user-side syscall arg5 */
    push r9                 /* user-side syscall arg6 */
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    /* Translate the user-side syscall ABI into the SysV C ABI:
     *
     *   user side  C arg slot   transform
     *   ---------  ----------   ---------
     *   rax (num)  rdi          mov rdi, rax
     *   rdi (a1)   rsi          mov rsi, rdi
     *   rsi (a2)   rdx          mov rdx, rsi
     *   rdx (a3)   rcx          mov rcx, rdx
     *   r10 (a4)   r8           mov r8 , r10
     *   r8  (a5)   r9           mov r9 , r8
     *
     * Crucially: do them from "right to left" so each src is read
     * before any later instruction overwrites it. */
    mov r9,  r8
    mov r8,  r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    cld                     /* SysV: DF must be clear at function entry */
    call syscall_dispatch
    /* rax now holds the return value -- SysV puts it there, and SYSRETQ
     * leaves it untouched, so the user sees it in rax. */

    /* Unwind in reverse push order. */
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r9                  /* restore user r9 */
    pop r8                  /* restore user r8 */
    pop r10                 /* restore user r10 */
    pop rdx                 /* restore user rdx (a3) */
    pop rsi                 /* restore user rsi (a2) */
    pop rdi                 /* restore user rdi (a1) */
    pop r11                 /* user RFLAGS -> SYSRETQ will reload */
    pop rcx                 /* user RIP    -> SYSRETQ will reload */

    /* Switch back to the user stack and return.
     *
     * The remaining 16 bytes on our kernel stack are:
     *   [rsp + 0]  = alignment padding
     *   [rsp + 8]  = saved user RSP
     *
     * Skip the pad, then load RSP from the saved-user-RSP slot. We
     * use `mov rsp, [rsp]` rather than `pop rsp` because the SDM
     * pseudocode for the POP RSP form is ambiguous about the post-
     * pop value of RSP (real silicon stores the popped value, but
     * `mov` is unambiguous). The slot is on THIS process's kstack
     * so it survived any context switches inside the syscall body. */
    add rsp, 8              /* discard alignment pad */
    mov rsp, [rsp]          /* user RSP <- saved slot */

    sysretq                 /* CPL3, RIP <- rcx, RFLAGS <- r11 */

.section .note.GNU-stack, "", @progbits
