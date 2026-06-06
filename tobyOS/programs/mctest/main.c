/* programs/mctest/main.c -- CPU-bound worker for the multi-core test.
 *
 * Spends a fixed, sizable amount of time computing entirely in user mode
 * (just two syscalls: the final write + exit), so when several copies run
 * at once they parallelize across cores rather than serializing in the
 * kernel. The MCTEST_BOOT harness spawns N of these and times the batch:
 * wall time ~= one worker's time means real multi-core; ~= N x means serial.
 */

#include <stdio.h>

int main(int argc, char **argv) {
    /* Touch argv so the compiler keeps the packed strings live and we
     * actually dereference the user-stack argv pointers (the region the
     * AP-first-run SMEP fault was reported to land in). */
    volatile unsigned long sink = (unsigned long)argc;
    for (int a = 0; a < argc; a++) {
        const char *s = argv[a];
        while (s && *s) { sink += (unsigned char)*s; s++; }
    }
    volatile unsigned long x = sink;
    for (unsigned long i = 0; i < 400000000UL; i++) {
        x += i * 2654435761UL;
        x ^= (x >> 13);
    }
    printf("MCTEST: worker done (checksum=%lu)\n", (unsigned long)x);
    return 0;
}
