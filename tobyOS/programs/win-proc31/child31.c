/* child31.exe -- Track C, W6: the child win-proc31.exe starts with
 * CreateProcess. What it does is its argv[1]:
 *   exit N     ExitProcess(N) (N in C syntax, so 0xC0000005 works)
 *   args       writes "argc=<n>|<argv[0]>|<argv[1]>|...", a newline, then
 *              "cmd=<GetCommandLineA()>" and "cmdw=<GetCommandLineW() as
 *              ASCII>", each with a newline, to its standard output
 *   upper      copies its standard input to its standard output in upper
 *              case until the end, then writes "done" to its standard error
 *   sleep MS   sleeps, then exits 7
 *   cwd        writes GetCurrentDirectoryA()'s answer to standard output
 * All output goes through WriteFile on GetStdHandle's handles, so nothing
 * waits in a C-runtime buffer. Exits 0 unless told otherwise, 100 on an
 * argument it does not know. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_out;

static void put(HANDLE h, const char *s, DWORD n) {
    DWORD w;
    WriteFile(h, s, n, &w, NULL);
}
static void puts_h(HANDLE h, const char *s) { put(h, s, (DWORD)strlen(s)); }

int main(int argc, char **argv) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (argc < 2) return 100;
    const char *mode = argv[1];
    if (!strcmp(mode, "exit") && argc > 2) {
        ExitProcess((UINT)strtoul(argv[2], NULL, 0));
    } else if (!strcmp(mode, "args")) {
        char num[16];
        int n = argc, k = 0;
        char tmp[16];
        do { tmp[k++] = (char)('0' + n % 10); n /= 10; } while (n);
        for (int i = 0; i < k; i++) num[i] = tmp[k - 1 - i];
        num[k] = 0;
        puts_h(g_out, "argc=");
        puts_h(g_out, num);
        for (int i = 0; i < argc; i++) {
            puts_h(g_out, "|");
            puts_h(g_out, argv[i]);
        }
        puts_h(g_out, "\ncmd=");
        puts_h(g_out, GetCommandLineA());
        puts_h(g_out, "\ncmdw=");
        const WCHAR *w = GetCommandLineW();
        char a[600];
        int i = 0;
        for (; w[i] && i < (int)sizeof a - 1; i++) a[i] = (w[i] < 128) ? (char)w[i] : '?';
        a[i] = 0;
        puts_h(g_out, a);
        puts_h(g_out, "\n");
    } else if (!strcmp(mode, "upper")) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        char buf[256];
        DWORD got;
        while (ReadFile(in, buf, sizeof buf, &got, NULL) && got > 0) {
            for (DWORD i = 0; i < got; i++)
                if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);
            put(g_out, buf, got);
        }
        puts_h(GetStdHandle(STD_ERROR_HANDLE), "done\n");
    } else if (!strcmp(mode, "sleep") && argc > 2) {
        Sleep((DWORD)strtoul(argv[2], NULL, 0));
        return 7;
    } else if (!strcmp(mode, "cwd")) {
        char d[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(sizeof d, d);
        put(g_out, d, n);
        puts_h(g_out, "\n");
    } else {
        return 100;
    }
    return 0;
}
