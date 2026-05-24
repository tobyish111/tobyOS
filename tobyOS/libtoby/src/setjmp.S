/* libtoby/src/setjmp.S -- setjmp/longjmp for x86_64.
 *
 * jmp_buf layout (8 longs):
 *   [0] rbx
 *   [1] rbp
 *   [2] r12
 *   [3] r13
 *   [4] r14
 *   [5] r15
 *   [6] rsp  (caller's, i.e. after setjmp returns)
 *   [7] rip  (return address)
 */

.text
.global setjmp
.type setjmp, @function
setjmp:
    /* rdi = jmp_buf env */
    movq %rbx,    (%rdi)
    movq %rbp,   8(%rdi)
    movq %r12,  16(%rdi)
    movq %r13,  24(%rdi)
    movq %r14,  32(%rdi)
    movq %r15,  40(%rdi)
    /* Save caller's rsp (skip the return address pushed by call) */
    leaq 8(%rsp), %rax
    movq %rax,  48(%rdi)
    /* Save return address */
    movq (%rsp), %rax
    movq %rax,  56(%rdi)
    /* Return 0 */
    xorl %eax, %eax
    ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, @function
longjmp:
    /* rdi = jmp_buf env, esi = val */
    movl %esi, %eax
    /* If val == 0, return 1 (POSIX requirement) */
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:
    movq    (%rdi), %rbx
    movq   8(%rdi), %rbp
    movq  16(%rdi), %r12
    movq  24(%rdi), %r13
    movq  32(%rdi), %r14
    movq  40(%rdi), %r15
    movq  48(%rdi), %rsp
    jmpq *56(%rdi)
.size longjmp, . - longjmp
