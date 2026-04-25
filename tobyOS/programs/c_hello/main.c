/* programs/c_hello/main.c -- the smallest possible libtoby smoke test.
 *
 * Touches printf (stdio.c), the format engine (do_format), and the
 * exit path (atexit + _exit). If this program runs and prints the
 * expected line, the libc bring-up plumbing is sound. */

#include <stdio.h>

int main(int argc, char **argv) {
    (void)argv;
    printf("[c_hello] hello from libtoby (argc=%d)\n", argc);
    printf("[c_hello] ALL OK\n");
    return 0;
}
