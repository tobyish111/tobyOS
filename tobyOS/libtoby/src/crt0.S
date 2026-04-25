/* libtoby/src/crt0.S -- libc entry trampoline.
 *
 * This is the libtoby-flavoured replacement for programs/common/start.S.
 * The shape of the user stack at entry is the same (the kernel does not
 * know whether the program is libc-linked):
 *
 *   high addr  argv strings + envp strings
 *              envp[envc] = NULL
 *              envp[envc-1] ... envp[0]
 *              argv[argc] = NULL
 *              argv[argc-1] ... argv[0]
 *   user_rsp   argc                <-- this trampoline reads here
 *
 * What this version adds over the bare trampoline:
 *   - __libtoby_init(argc, argv, envp) is called before main, so the
 *     allocator, stdio FILEs, and the ABI-equivalence asserts can run
 *     before user code touches them.
 *   - On main's return we go through exit(), not raw SYS_EXIT, so any
 *     handlers registered via atexit() fire (e.g. fflush(stdout)).
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

    /* Persist for libtoby internals (getenv walks __toby_envp,
     * sample programs read __toby_argc/argv directly, etc.). */
    lea rax, [rip + __toby_argc]
    mov [rax], rdi
    lea rax, [rip + __toby_argv]
    mov [rax], rsi
    lea rax, [rip + __toby_envp]
    mov [rax], rdx

    /* __libtoby_init(argc, argv, envp). The args are already in
     * rdi/rsi/rdx -- the SysV ABI is set up perfectly. */
    call __libtoby_init

    /* Reload (callee-saved regs survived the call but we explicitly
     * pulled args from globals to keep this simple). */
    mov rdi, [rip + __toby_argc]
    mov rsi, [rip + __toby_argv]
    mov rdx, [rip + __toby_envp]

    call main

    /* main's return -> exit code. Go through exit() so atexit handlers
     * fire (notably fflush(stdout)). exit() never returns; the loop
     * below is purely belt-and-braces. */
    mov edi, eax
    call exit
1:  jmp 1b

.section .data
.globl __toby_argc
.globl __toby_argv
.globl __toby_envp
.align 8
__toby_argc: .quad 0
__toby_argv: .quad 0
__toby_envp: .quad 0

.section .note.GNU-stack, "", @progbits
