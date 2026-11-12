/* win-cb38s.exe (W7g): imports dll38.dll statically -- the child
 * win-cb38.exe runs for the cases at process start, process exit and a
 * thread's start and end. Says each step on stdout, and the vectored
 * handler and the top-level filter say what they see. Exits 5. */
#include <windows.h>
#include <string.h>

__declspec(dllimport) int d38_value(void);
static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}
static void sayx(const char *p, DWORD v) {
    char b[48];
    strcpy(b, p);
    int k = (int)strlen(b);
    for (int i = 7; i >= 0; i--) b[k++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
    b[k++] = '\n';
    b[k] = 0;
    say(b);
}
static LONG WINAPI veh(EXCEPTION_POINTERS *ep) { sayx("veh ", ep->ExceptionRecord->ExceptionCode); return EXCEPTION_CONTINUE_SEARCH; }
static LONG WINAPI top(EXCEPTION_POINTERS *ep) { sayx("top ", ep->ExceptionRecord->ExceptionCode); return EXCEPTION_CONTINUE_SEARCH; }
static DWORD WINAPI thr(void *p) { (void)p; say("thread\n"); return 3; }

int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(top);
    say("main\n");
    if (argc > 1 && !strcmp(argv[1], "thread")) {
        HANDLE t = CreateThread(0, 0, thr, 0, 0, 0);
        WaitForSingleObject(t, 20000);
        say("joined\n");
    }
    if (d38_value() == 38) say("main returns\n");
    return 5;
}
