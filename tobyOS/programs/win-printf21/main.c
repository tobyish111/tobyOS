/* main.c -- Track C / C21: proves the kernel Win32 printf engine now formats
 * DOUBLES (%f / %e / %g and the upper-case variants), the gap C20 left open
 * after it made double-returning libm work but could not yet PRINT a double.
 *
 * Built the same way as win-crt (clang --target mingw, -D__USE_MINGW_ANSI_STDIO=0)
 * so printf/fprintf inline to the ucrt stdio primitive __stdio_common_vfprintf --
 * which tobyOS shims kernel-side and runs through win32_vformat. A double vararg
 * therefore reaches the kernel engine as its 8-byte bit pattern in the va_list,
 * and the engine's new %f/%e/%g cases convert it via the -msse kmath unit. (A
 * mingw libmingwex build would format floats in-process and NOT exercise the
 * kernel engine, so the build flags matter.)
 *
 * The program fprintf()s a battery of conversions -- default + explicit
 * precision, width, flags, %e/%g exponent selection, %g trailing-zero stripping,
 * a negative value, and a 14-significant-digit %g (what a real interpreter uses
 * for tostring(number)) -- into C:\c21.out, then prints the same line to stdout.
 * The kernel re-reads the file and matches the EXACT expected text, so exit 21 +
 * the file match is reachable only if every double rendered correctly. The float
 * inputs are computed/stored through `volatile` so nothing is constant-folded
 * into a precomputed string at compile time. */

#include <stdio.h>

__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

/* volatile: force the values to be real runtime doubles passed as varargs, not
 * folded into a literal result string by the optimizer. */
static volatile double v_pi6   = 3.14159265358979;
static volatile double v_1p5   = 1.5;
static volatile double v_0p5   = 0.5;
static volatile double v_1e5   = 100000.0;
static volatile double v_1e6   = 1000000.0;
static volatile double v_big   = 12345.678;
static volatile double v_small = 0.015625;
static volatile double v_neg   = -2.5;
static volatile double v_num   = 2.0;
static volatile double v_den   = 3.0;
static volatile double v_pi    = 3.14159;

static int real_main(void) {
    double pi6   = v_pi6;
    double third = v_num / v_den;          /* real FP divide -> 0.6666666666666666 */

    FILE *f = fopen("C:\\c21.out", "w");
    if (!f) { puts("C21: fopen failed"); return 99; }

    /* One format string, ten double varargs. The leading run also proves the
     * engine still threads >4 stack varargs (doubles) correctly. */
    fprintf(f, "C21|%.6f|%.2f|%g|%g|%g|%e|%.3e|%.3f|%.14g|%8.2f|\n",
            pi6, v_1p5, v_0p5, v_1e5, v_1e6, v_big, v_small, v_neg, third, v_pi);
    fclose(f);

    /* Echo to stdout for the serial log (also through the kernel engine). */
    printf("C21|%.6f|%.2f|%g|%g|%g|%e|%.3e|%.3f|%.14g|%8.2f|\n",
           pi6, v_1p5, v_0p5, v_1e5, v_1e6, v_big, v_small, v_neg, third, v_pi);

    return 21;
}

void __attribute__((force_align_arg_pointer)) c21_entry(void) {
    ExitProcess((unsigned)real_main());
}
