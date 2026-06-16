/* main.c -- a genuine Windows x86-64 console PE that uses the C RUNTIME
 * (Track C / C2). Unlike C1's freestanding win-hello, this calls the real
 * MinGW/ucrt `printf` and `puts`. Those inline to the ucrt stdio primitives
 * __acrt_iob_func + __stdio_common_vfprintf (whose va_list is a STACK arg),
 * which tobyOS shims kernel-side -- so this proves: (a) the marshalling gate
 * now delivers stack args, and (b) a real printf format string with %s/%d/
 * %x/%c/%%, width, precision, flags, and >4 varargs renders correctly.
 *
 * Built with a custom entry (-nostartfiles, -e c2_entry) so the full ucrt
 * mainCRTStartup (SEH, _initterm, ~9 import DLLs) is bypassed -- that is a
 * later milestone. The import surface stays tiny: kernel32!ExitProcess plus
 * the three api-ms-win-crt-stdio symbols. force_align_arg_pointer realigns
 * RSP to the 16-byte boundary the MS-x64 ABI assumes at entry.
 */

#include <stdio.h>

__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

static int real_main(void) {
    puts("== tobyOS runs a Windows C-runtime program (printf) ==");

    int n = printf("printf: %s #%d hex=0x%x char=%c pct=%%\n",
                   "world", 42, 255, 'Z');
    printf("  (that call returned %d)\n", n);

    /* >4 varargs: forces stack-passed arguments through the gate. */
    printf("six ints: %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6);

    /* width / precision / flags / left-justify / zero-pad / truncated %s.
     * (Floating-point %f is intentionally out of C2 scope -- it needs XMM
     * vararg handling -- so it is not exercised here.) */
    printf("fmt: [%5d] [%-5d] [%05d] [%+d] [%.3s] [%#x] [%8s]\n",
           42, 42, 42, 42, "truncated", 0xABCD, "rt");

    printf("neg=%d  big=%lld  ptr=%p\n", -7, 1234567890123LL, (void *)0x140001000ULL);

    return 7;
}

void __attribute__((force_align_arg_pointer)) c2_entry(void) {
    ExitProcess((unsigned)real_main());
}
