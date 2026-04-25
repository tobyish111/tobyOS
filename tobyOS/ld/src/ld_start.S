/* SPDX-License-Identifier: MIT
 *
 * tobyOS dynamic linker entry point.
 *
 * The kernel hands control here with:
 *   RSP -> argc, then argv[], envp[], auxv[]  (SysV layout)
 *
 * We must:
 *   1) capture the original stack pointer (we'll restore it before
 *      jumping into the program -- the program's crt0 expects the
 *      kernel-built layout intact),
 *   2) self-relocate (we are an ET_DYN that the kernel just mapped
 *      at AT_BASE, so all our internal pointers are still link-time
 *      offsets),
 *   3) call ld_main() to load DT_NEEDED libs and apply the program's
 *      relocations,
 *   4) jump to the program's entry point (returned in RAX).
 *
 * No globals may be accessed before ld_self_relocate() returns.
 * RIP-relative `lea` to hidden symbols (_DYNAMIC, __ehdr_start) is
 * the only addressing mode we trust pre-relocation.
 */

.intel_syntax noprefix
.code64

.section .text
.globl _start
.type _start, @function
_start:
    /* Save the kernel-provided stack pointer. RBX is callee-saved
     * across the C calls below, so it survives. */
    mov rbx, rsp

    /* Establish 16-byte alignment for the upcoming `call`. SysV
     * requires (rsp + 8) % 16 == 0 at function entry; the call
     * instruction will push the 8-byte return address. */
    and rsp, -16
    sub rsp, 8

    /* RIP-relative address of our own ELF header == self load base. */
    lea rdi, [rip + __ehdr_start]
    call ld_self_relocate

    /* All internal pointers are now usable. Run the C linker. */
    mov rdi, rbx                    /* original SP for auxv walk */
    lea rsi, [rip + __ehdr_start]   /* self base */
    call ld_main

    /* ld_main returns the program entry point in RAX. Restore the
     * kernel-built stack so the program sees argc/argv/envp/auxv,
     * then tail-jump. */
    mov rsp, rbx
    /* Clear rdx so the program's _start sees a NULL "atexit handler"
     * pointer in rdx (some crt0s check this). */
    xor rdx, rdx
    jmp rax

.size _start, . - _start

/* Mark stack as non-executable. */
.section .note.GNU-stack, "", @progbits
