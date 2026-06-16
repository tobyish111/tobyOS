/* hello.c -- a genuine Windows x86-64 console PE, the Track C / C1 proof.
 *
 * This is NOT a tobyOS program. It is built with the Windows target triple
 * (x86_64-w64-mingw32) and links against the real mingw kernel32 import
 * library, so the produced win-hello.exe is byte-for-byte a normal Windows
 * PE32+ executable -- it would run unmodified on Windows. It imports ONLY
 * three kernel32 symbols (the smallest useful console surface):
 *
 *     GetStdHandle  WriteFile  ExitProcess
 *
 * tobyOS's PE loader maps the image, binds those three IAT entries to its
 * Win32 shims (kernel-side, reached through a user-mode marshalling gate),
 * and transfers to hello_entry. The program writes a line to stdout via
 * WriteFile and exits 42 -- the boot harness asserts that exit code, and
 * the written line appears in the serial log, proving the whole chain:
 * PE load -> IAT resolution -> MS-x64 shim call -> tobyOS stdout.
 *
 * Built with -nostdlib + a custom entry point so NO C runtime is dragged
 * in (the CRT would balloon the import surface and touch the TEB/GS). The
 * force_align_arg_pointer attribute realigns RSP in case the kernel enters
 * the entry off the 16-byte boundary the MS-x64 ABI assumes.
 */

__declspec(dllimport) void * __stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) int    __stdcall WriteFile(void *hFile, const void *buf,
                                                 unsigned long n,
                                                 unsigned long *written,
                                                 void *overlapped);
__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

#define STD_OUTPUT_HANDLE ((unsigned long)-11)

static unsigned long my_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

void __attribute__((force_align_arg_pointer)) hello_entry(void) {
    const char *msg = "hello from a Windows PE binary running on tobyOS\r\n";
    void *h = GetStdHandle(STD_OUTPUT_HANDLE);
    unsigned long written = 0;
    WriteFile(h, msg, my_strlen(msg), &written, 0);
    ExitProcess(42);
}
