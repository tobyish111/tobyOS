/* programs/linux-fbgui/crt.c
 *
 * Standard musl-style C runtime startup (a faithful reproduction of musl's
 * arch/x86_64 crt1). The in-tree clang has no musl sysroot, so we cannot link
 * the real Scrt1.o; instead we provide the identical entry sequence and hand
 * control to musl's exported __libc_start_main, which (under ld-musl) sets up
 * TLS/errno/atexit/stdio and then calls main(argc, argv, envp).
 *
 * _start receives the kernel-provided initial stack in %rsp:
 *     [rsp+0] = argc, [rsp+8...] = argv[], NULL, envp[], NULL, auxv...
 */

extern int  main(int, char **, char **);
extern void __libc_start_main(int (*)(int, char **, char **), int, char **);

__attribute__((used, noreturn))
void _start_c(long *sp) {
    int    argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    __libc_start_main(main, argc, argv);
    __builtin_unreachable();
}

__asm__(
    ".text\n"
    ".global _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "    xor %ebp, %ebp\n"        /* mark outermost frame */
    "    mov %rsp, %rdi\n"        /* arg = pointer to {argc, argv, envp, auxv} */
    "    and $-16, %rsp\n"        /* 16-byte align the stack */
    "    call _start_c\n"
    "    hlt\n"
);
