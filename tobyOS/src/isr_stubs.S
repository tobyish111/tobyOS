/* isr_stubs.S -- per-vector ISR stubs + common dispatcher trampoline.
 *
 * One stub per IDT vector (0..255). Same shape as before but generated
 * via .rept so we cover the whole vector space, not just 0..47. We
 * need that as soon as the IO APIC and MSI/MSI-X allocator hand out
 * vectors above 0x2F (Milestone 22 step 1+).
 *
 * Layered work split:
 *   - per-vector stub : pushes a fake error code (if the CPU didn't),
 *                       pushes the vector number, jumps to common_isr.
 *   - common_isr      : saves all GP registers in the order matching
 *                       `struct regs` (see include/tobyos/isr.h),
 *                       aligns rsp, calls C dispatcher, restores, iretq.
 *
 * Vectors that DO push an error code automatically (per Intel SDM
 * vol 1, table 6-1):
 *     8, 10, 11, 12, 13, 14, 17, 21, 29, 30
 * All others get a synthetic 0 here so the C dispatcher always sees a
 * uniform stack frame.
 *
 * Stack alignment: SysV AMD64 requires rsp % 16 == 0 immediately before
 * a CALL. We snapshot rsp into rbp, mask to 16, call, then restore.
 * (rbp's saved value is on the stack and gets popped back in the unwind.)
 */

.intel_syntax noprefix
.code64

.section .text

/* -------- per-vector stubs -------- */

.macro ISR_NOERR vec
    .global isr\vec
    .type   isr\vec, @function
isr\vec:
    push 0                  /* fake error code for uniformity */
    push \vec
    jmp common_isr
.endm

.macro ISR_ERR vec
    .global isr\vec
    .type   isr\vec, @function
isr\vec:
    /* CPU already pushed the error code */
    push \vec
    jmp common_isr
.endm

/* CPU exceptions 0..31: a handful push a real error code. */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Vectors 32..255: every external interrupt (PIC, IO APIC, LAPIC LVT,
 * MSI / MSI-X, etc.). None of these push a real error code, so they
 * all use ISR_NOERR. We generate them with .rept so we don't have to
 * spell out 224 individual macro invocations.
 *
 * Note: the macro is `\vec`, so `.rept` + an external counter symbol
 * is the pattern. GAS doesn't have a clean for-loop -- we use \i below
 * and increment it inside the .rept body. */
.altmacro
.set vec_iter, 32
.rept (256 - 32)
    ISR_NOERR %vec_iter
    .set vec_iter, vec_iter + 1
.endr

/* -------- common dispatcher trampoline -------- */

.global common_isr
.type   common_isr, @function
common_isr:
    /* On entry, the stack looks like (top -> bottom = low addr -> high):
     *   [rsp+0]   vector
     *   [rsp+8]   error_code
     *   [rsp+16]  rip       (CPU)
     *   [rsp+24]  cs        (CPU)
     *   [rsp+32]  rflags    (CPU)
     *   [rsp+40]  rsp       (CPU)
     *   [rsp+48]  ss        (CPU)
     *
     * We push GP regs in REVERSE order of `struct regs` so that the
     * lowest field (r15) ends up at the lowest address. */
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    /* rdi = pointer to struct regs (= current rsp) */
    mov rdi, rsp

    /* Align stack to 16 bytes for the SysV ABI before CALL.
     * Save current rsp in rbp; rbp's "saved" value is on the stack
     * and will be popped back below, so trashing the live rbp is fine. */
    mov rbp, rsp
    and rsp, -16

    cld                     /* SysV ABI: DF must be clear at function entry */
    call isr_dispatch

    mov rsp, rbp

    /* Restore GP registers in mirror order */
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    /* Drop the vector + error_code we pushed (16 bytes), then return. */
    add rsp, 16
    iretq

/* -------- table of stub addresses, exported to C -------- */

.section .rodata
.global isr_stub_table
.type   isr_stub_table, @object
.align 8
isr_stub_table:
    /* Same trick as the stub generator: spell out the names with .rept.
     * The `%tab_iter` substitution only works as a MACRO ARGUMENT in
     * .altmacro mode, not as a raw directive arg, so wrap the .quad
     * in a one-liner macro. */
.altmacro
.macro EMIT_STUB_PTR n
    .quad isr\n
.endm

    .set tab_iter, 0
    .rept 256
        EMIT_STUB_PTR %tab_iter
        .set tab_iter, tab_iter + 1
    .endr

/* Mark stack as non-executable (silences ld warnings). */
.section .note.GNU-stack, "", @progbits
