/* main.c -- a STOCK Windows console .exe (Track C / C3). Unlike C1/C2 this
 * uses NO special build flags and NO custom entry: it is built with a plain
 * `clang main.c -o win-hello3.exe`, so it runs through the full ucrt
 * mainCRTStartup (security cookie, _initterm, __getmainargs, the heap, the
 * TEB via gs:[0x30], ...) before reaching main(). That is exactly the
 * "runs actual Windows software off the shelf" milestone: tobyOS provides a
 * TEB (via the SWAPGS scheme), a user-memory heap, an argc/argv/environ
 * region, and ~40 kernel32 + ucrt shims so the unmodified CRT startup
 * reaches main and printf reaches stdout.
 *
 * Returns 3; the boot harness asserts that exit code and the printed lines
 * appear in the serial log.
 */

#include <stdio.h>

int main(int argc, char **argv) {
    printf("argc=%d argv0=%s\n", argc, argv[0]);
    printf("hello from a STOCK clang-built Windows .exe on tobyOS\n");
    return 3;
}
