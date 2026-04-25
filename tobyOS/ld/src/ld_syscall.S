/* SPDX-License-Identifier: MIT
 *
 * Raw syscall trampolines for ld-toby.so.
 *
 * The dynamic linker cannot link against libtoby (libtoby is the
 * thing it loads), so it carries its own kernel entry stubs. The
 * convention mirrors the libtoby toby_sc* stubs:
 *
 *   long ld_syscallN(long n, long a1, ...)
 *       n    in rdi  -> SYSCALL needs n in rax
 *       a1   in rsi  -> SYSCALL needs a1 in rdi
 *       a2   in rdx  -> a2 in rsi
 *       a3   in rcx  -> a3 in rdx
 *
 * We must avoid clobbering rcx/r11 around the SYSCALL because the
 * CPU itself uses them to record the user RIP/RFLAGS. The kernel
 * convention is "preserve nothing"; we just reload before SYSCALL.
 */

.intel_syntax noprefix
.code64

.section .text
.globl ld_syscall1
.type ld_syscall1, @function
ld_syscall1:
    mov rax, rdi
    mov rdi, rsi
    syscall
    ret
.size ld_syscall1, . - ld_syscall1

.globl ld_syscall3
.type ld_syscall3, @function
ld_syscall3:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    syscall
    ret
.size ld_syscall3, . - ld_syscall3

.section .note.GNU-stack, "", @progbits
