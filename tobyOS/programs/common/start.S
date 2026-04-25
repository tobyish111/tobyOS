/* programs/common/start.S -- shared user-mode entry trampoline.
 *
 * The kernel packs the following onto the new process's user stack
 * (see proc.c::pack_argv_envp_on_user_stack):
 *
 *   high addr  argv strings + envp strings
 *              envp[envc] = NULL
 *              envp[envc-1] ... envp[0]
 *              argv[argc] = NULL
 *              argv[argc-1] ... argv[0]
 *   user_rsp   argc                <-- this trampoline reads here
 *
 * We:
 *   1. Load argc       = [rsp]                      -> rdi
 *   2. Load argv       = rsp + 8                    -> rsi
 *   3. Compute envp    = argv + (argc + 1) * 8      -> rdx
 *   4. Stash argc/argv/envp in __toby_argc/__toby_argv/__toby_envp
 *      so libc's getenv can find them after main is entered.
 *   5. Call main(argc, argv, envp).
 *      The kernel laid the stack out so that argc_va % 16 == 0; after
 *      the `call` (which pushes 8 bytes), rsp is 16-byte aligned at
 *      main's entry, satisfying the SysV ABI.
 *   6. Use main's return value as the exit code, invoke SYS_EXIT.
 *
 * Programs whose `main` only takes (int, char**) still work: SysV passes
 * the third argument in rdx, which the callee is free to ignore.
 */

.intel_syntax noprefix
.code64

.section .text
.globl _start
.type _start, @function
_start:
    mov rdi, [rsp]              /* argc */
    lea rsi, [rsp + 8]          /* argv */
    /* envp = argv + (argc + 1) * 8 */
    mov rax, rdi
    inc rax
    shl rax, 3
    lea rdx, [rsi + rax]

    /* Persist for libc (getenv, etc.). The symbols are weak so a
     * program that links without libc still resolves them to a
     * locally-defined zero word in this object. */
    lea rax, [rip + __toby_argc]
    mov [rax], rdi
    lea rax, [rip + __toby_argv]
    mov [rax], rsi
    lea rax, [rip + __toby_envp]
    mov [rax], rdx

    call main
    mov rdi, rax                /* main's return -> exit code */
    mov rax, 0                  /* SYS_EXIT */
    syscall
1:  jmp 1b                      /* belt-and-braces: SYS_EXIT never returns */

.section .data
.globl __toby_argc
.globl __toby_argv
.globl __toby_envp
.weak  __toby_argc
.weak  __toby_argv
.weak  __toby_envp
.align 8
__toby_argc: .quad 0
__toby_argv: .quad 0
__toby_envp: .quad 0

.section .note.GNU-stack, "", @progbits
