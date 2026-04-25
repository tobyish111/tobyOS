/* programs/c_dynhello/main.c -- the dynamic-linking smoke test.
 *
 * Functionally identical to c_hello, but built as ET_DYN with
 * PT_INTERP=/lib/ld-toby.so and DT_NEEDED=libtoby.so. If this
 * program runs and prints the expected lines, the dynamic loader
 * is correctly:
 *   - mapping libtoby.so into our address space,
 *   - applying R_X86_64_GLOB_DAT / R_X86_64_JUMP_SLOT relocations
 *     for the printf/exit references in this file,
 *   - applying R_X86_64_RELATIVE relocations inside libtoby.so,
 *   - and handing control to our crt0 with auxv intact.
 *
 * Keep this trivial. The point isn't to test libc -- c_hello does
 * that already -- it's to prove that THE SAME libc, exposed via
 * .so + dynamic linker, still works. */

#include <stdio.h>

int main(int argc, char **argv) {
    (void)argv;
    printf("[c_dynhello] hello from DYNAMIC libtoby (argc=%d)\n", argc);
    printf("[c_dynhello] ALL OK\n");
    return 0;
}
