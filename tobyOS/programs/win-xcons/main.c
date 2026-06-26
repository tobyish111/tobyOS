/* win-xcons -- Track X / X1 cross-personality pipeline CONSUMER.
 *
 * A genuine Windows x86-64 console PE (x86_64-w64-mingw32, -nostdlib). It
 * drains stdin via kernel32!ReadFile, checks the bytes contain the expected
 * token, and exits 0 (found) or 1 (not found).
 *
 * In the X1 proof it is the LAST stage of a busybox `sh` pipeline:
 *
 *     busybox echo XPIPE_TOKEN_OK | /bin/win-xcons.exe
 *
 * busybox forks, dup2's the pipe's read end onto fd 0, then execve()s this
 * .exe. The Linux `echo`'s bytes flow through the kernel pipe into this
 * Windows PE's ReadFile(stdin). Because a pipeline's exit status is its last
 * stage, this .exe's exit code becomes the shell's -- so exit 0 proves the
 * Linux->Windows direction end to end.
 */

__declspec(dllimport) void * __stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) int    __stdcall ReadFile(void *hFile, void *buf,
                                                unsigned long n,
                                                unsigned long *read_,
                                                void *overlapped);
__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

#define STD_INPUT_HANDLE ((unsigned long)-10)

/* Does haystack[0..hn) contain the NUL-terminated needle? */
static int contains(const char *hay, unsigned long hn, const char *needle) {
    for (unsigned long i = 0; i < hn; i++) {
        unsigned long j = 0;
        while (needle[j] && i + j < hn && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

void __attribute__((force_align_arg_pointer)) xcons_entry(void) {
    void *h = GetStdHandle(STD_INPUT_HANDLE);
    static char buf[4096];
    unsigned long total = 0;
    for (;;) {
        unsigned long got = 0;
        int ok = ReadFile(h, buf + total, (unsigned long)(sizeof(buf) - total),
                          &got, 0);
        if (!ok || got == 0) break;          /* EOF (write end closed) */
        total += got;
        if (total >= sizeof(buf)) break;
    }
    ExitProcess(contains(buf, total, "XPIPE_TOKEN_OK") ? 0u : 1u);
}
