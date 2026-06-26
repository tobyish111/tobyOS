/* main.c -- Track C / C22: proves formatted/number INPUT works -- the scanf
 * family (sscanf/fscanf) + strtod/atof. Before C22 the kernel's vsscanf was a
 * 0-fields stub and strtod always returned 0.0, so no Windows program could read
 * a number from text. This is the read-side complement to C20 (compute floats)
 * and C21 (print floats), closing the float I/O round-trip.
 *
 * Built like win-crt (clang --target mingw, -D__USE_MINGW_ANSI_STDIO=0) so the
 * scanf/printf family inlines to the ucrt stdio primitives
 * (__stdio_common_vsscanf / __stdio_common_vfscanf / __stdio_common_vfprintf),
 * which tobyOS shims kernel-side -- so the parsing runs in win32_vscanf + the
 * -msse kmath_strtod, NOT libmingwex in-process. The float results are scaled to
 * integers for the sentinel (integer compares only). The kernel re-reads
 * C:\c22.out and matches the exact text; main also self-checks and returns 22
 * only if every field parsed correctly (else 50+i identifies the failing check).
 */

#include <stdio.h>
#include <stdlib.h>

__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

static double myabs(double x) { return x < 0 ? -x : x; }

static int real_main(void) {
    /* (1) sscanf: int, negative int, string, double. */
    int a = 0, b = 0; char word[32] = {0}; double d = 0;
    int n = sscanf("42 -7 hello 3.14159", "%d %d %s %lf", &a, &b, word, &d);

    /* (2) strtod with endptr; (3) atof with leading ws + exponent. */
    char *end = 0;
    double e = strtod("2.71828rest", &end);
    double g = atof("  -1.5e3");

    /* (4) %x into unsigned; (5) %f into a float*. */
    unsigned u = 0; sscanf("ff", "%x", &u);
    float fv = 0; sscanf("0.5", "%f", &fv);

    /* (6) fscanf from a real file (write it first via fprintf). */
    FILE *wf = fopen("C:\\c22in.txt", "w");
    if (wf) { fprintf(wf, "100 2.5"); fclose(wf); }
    int fi = 0; double fd2 = 0; int m = 0;
    FILE *rf = fopen("C:\\c22in.txt", "r");
    if (rf) { m = fscanf(rf, "%d %lf", &fi, &fd2); fclose(rf); }

    /* Scale floats to integers (uses C21's working %d), write the sentinel. */
    FILE *f = fopen("C:\\c22.out", "w");
    if (!f) { puts("C22: fopen failed"); return 99; }
    fprintf(f, "C22|n=%d|a=%d|b=%d|w=%s|d=%d|e=%d|g=%d|u=%u|f=%d|end=%c|m=%d|fi=%d|fd2=%d|\n",
            n, a, b, word,
            (int)(d * 100000 + 0.5), (int)(e * 100000 + 0.5), (int)g, u,
            (int)(fv * 100 + 0.5), end ? *end : '?', m, fi, (int)(fd2 * 10 + 0.5));
    fclose(f);

    /* Self-check: exit 22 only if every conversion was correct. */
    if (n != 4)                       return 51;
    if (a != 42 || b != -7)           return 52;
    if (word[0] != 'h' || word[4] != 'o' || word[5] != 0) return 53;
    if (myabs(d - 3.14159) > 1e-4)    return 54;
    if (myabs(e - 2.71828) > 1e-4)    return 55;
    if (!end || *end != 'r')          return 56;
    if (myabs(g - (-1500.0)) > 1e-3)  return 57;
    if (u != 255)                     return 58;
    if (myabs(fv - 0.5) > 1e-4)       return 59;
    if (m != 2 || fi != 100)          return 60;
    if (myabs(fd2 - 2.5) > 1e-4)      return 61;

    return 22;
}

void __attribute__((force_align_arg_pointer)) c22_entry(void) {
    ExitProcess((unsigned)real_main());
}
