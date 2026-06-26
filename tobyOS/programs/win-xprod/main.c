/* win-xprod -- Track X / X1 cross-personality pipeline PRODUCER.
 *
 * A genuine Windows x86-64 console PE (x86_64-w64-mingw32, -nostdlib). It
 * writes a known token line to stdout via kernel32!WriteFile and exits 0.
 *
 * In the X1 proof it is the FIRST stage of a busybox `sh` pipeline:
 *
 *     /bin/win-xprod.exe | busybox grep -q XPIPE_TOKEN_OK
 *
 * busybox forks, dup2's the pipe's write end onto fd 1, then execve()s this
 * .exe. tobyOS's execve now recognises the PE, loads it as a Win32 process,
 * and runs it -- so its WriteFile(stdout) bytes flow through the kernel pipe
 * into a Linux `grep`. A PASS proves a Windows binary and a Linux binary
 * exchanged data in a single pipeline on one kernel.
 */

__declspec(dllimport) void * __stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) int    __stdcall WriteFile(void *hFile, const void *buf,
                                                 unsigned long n,
                                                 unsigned long *written,
                                                 void *overlapped);
__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

#define STD_OUTPUT_HANDLE ((unsigned long)-11)

void __attribute__((force_align_arg_pointer)) xprod_entry(void) {
    /* Trailing newline so a line-oriented Linux consumer (grep) sees a
     * complete line; the token is unique enough to assert on. */
    static const char msg[] = "XPIPE_TOKEN_OK\n";
    void *h = GetStdHandle(STD_OUTPUT_HANDLE);
    unsigned long written = 0;
    /* sizeof-1 to drop the implicit NUL. */
    WriteFile(h, msg, (unsigned long)(sizeof(msg) - 1), &written, 0);
    ExitProcess(written == (unsigned long)(sizeof(msg) - 1) ? 0u : 3u);
}
