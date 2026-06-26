/* win-xfilter -- Track X / X1 cross-personality pipeline FILTER (middle stage).
 *
 * A genuine Windows x86-64 console PE (x86_64-w64-mingw32, -nostdlib). It
 * reads all of stdin via kernel32!ReadFile, then writes a transformed copy to
 * stdout via WriteFile: the literal prefix "WIN-SAW:" followed by the input
 * bytes. Both stdin and stdout are kernel pipes.
 *
 * In the X1 proof it is the MIDDLE stage of a busybox `sh` pipeline:
 *
 *     busybox echo XPIPE_TOKEN_OK | /bin/win-xfilter.exe |
 *         busybox grep -q WIN-SAW:XPIPE_TOKEN_OK
 *
 * This is the showcase: data flows Linux(echo) -> Windows(this PE) -> Linux
 * (grep), i.e. TWO personality flips inside one pipeline. The Windows filter
 * both receives (ReadFile) and emits (WriteFile) over kernel pipes, and the
 * asserting Linux grep only matches if the Windows transform actually ran on
 * the Linux producer's bytes.
 */

__declspec(dllimport) void * __stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) int    __stdcall ReadFile(void *hFile, void *buf,
                                                unsigned long n,
                                                unsigned long *read_,
                                                void *overlapped);
__declspec(dllimport) int    __stdcall WriteFile(void *hFile, const void *buf,
                                                 unsigned long n,
                                                 unsigned long *written,
                                                 void *overlapped);
__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

#define STD_INPUT_HANDLE  ((unsigned long)-10)
#define STD_OUTPUT_HANDLE ((unsigned long)-11)

static int write_all(void *h, const char *p, unsigned long n) {
    unsigned long done = 0;
    while (done < n) {
        unsigned long w = 0;
        if (!WriteFile(h, p + done, n - done, &w, 0) || w == 0) return 0;
        done += w;
    }
    return 1;
}

void __attribute__((force_align_arg_pointer)) xfilter_entry(void) {
    void *hin  = GetStdHandle(STD_INPUT_HANDLE);
    void *hout = GetStdHandle(STD_OUTPUT_HANDLE);

    static char buf[4096];
    unsigned long total = 0;
    for (;;) {
        unsigned long got = 0;
        int ok = ReadFile(hin, buf + total, (unsigned long)(sizeof(buf) - total),
                          &got, 0);
        if (!ok || got == 0) break;
        total += got;
        if (total >= sizeof(buf)) break;
    }

    static const char pfx[] = "WIN-SAW:";
    if (!write_all(hout, pfx, (unsigned long)(sizeof(pfx) - 1))) ExitProcess(2u);
    if (!write_all(hout, buf, total)) ExitProcess(2u);
    ExitProcess(0u);
}
