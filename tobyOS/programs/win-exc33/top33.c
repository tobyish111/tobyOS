/* win-top33 -- Track C, W7b: an exception no frame handles, the top-level
 * filter (SetUnhandledExceptionFilter) and what follows it. The program runs
 * itself as a child five times, its standard output a pipe; each child
 * installs a top-level filter that answers one way, then raises 0xE0000001
 * inside c_frame (seh33s.S), whose __try/__finally/__except scopes do not
 * take it (c_filter: EXCEPTION_CONTINUE_SEARCH), on the main thread or on
 * one of its own. The parent compares each child's output and exit code
 * with Windows 11's (measured: logs/w32/w7b-host-run.txt):
 *   e    the filter says EXECUTE_HANDLER: the stack is unwound (the
 *        __finally runs, abnormally), then the process ends with the
 *        exception's code -- without DLL_PROCESS_DETACH (the .exe's TLS
 *        callback reports detaches);
 *   s    CONTINUE_SEARCH: the same (no debugger; error box off);
 *   c    CONTINUE_EXECUTION: RaiseException returns, the program goes on
 *        and exits 4 through ExitProcess (its detach runs);
 *   e t  EXECUTE_HANDLER on a second thread: the whole process ends, with
 *        the code;
 *   c t  CONTINUE_EXECUTION there: the thread goes on and ends, main exits 3.
 * Each child sets SEM_NOGPFAULTERRORBOX first, so neither system shows an
 * error box (on tobyOS: the line on stderr) for 's'.
 * Exit 36 iff all five match, else 100 + the first that did not. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

uint64_t g_landing_rbx, g_landing_xmm6;
void *g_mid_target;
extern uint64_t c_frame(void);

static LONG g_ret;

static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}
static void hex(char *b, unsigned long v) {
    char t[9];
    int k = 0;
    do { t[k++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && k < 8);
    while (k) *b++ = t[--k];
    *b = 0;
}

static void NTAPI tls_cb(PVOID h, DWORD reason, PVOID r) {
    (void)h; (void)r;
    if (reason == DLL_PROCESS_DETACH) say("tls: process detach\n");
    if (reason == DLL_THREAD_DETACH) say("tls: thread detach\n");
}
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK p_tls33 = tls_cb;

/* seh33s.S's other frames are not used here */
EXCEPTION_DISPOSITION h_mid(void *a, void *b, void *c, void *d) {
    (void)a; (void)b; (void)c; (void)d;
    return ExceptionContinueSearch;
}
EXCEPTION_DISPOSITION h_outer(void *a, void *b, void *c, void *d) {
    (void)a; (void)b; (void)c; (void)d;
    return ExceptionContinueSearch;
}

LONG c_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)ep; (void)frame;
    say("scope filter: continue search\n");
    return EXCEPTION_CONTINUE_SEARCH;
}
void c_finally(BOOLEAN abnormal, void *frame) {
    (void)frame;
    say(abnormal ? "finally (abnormal)\n" : "finally (normal)\n");
}
__attribute__((noinline)) uint64_t raiser(void) {
    ULONG_PTR a[2] = { 1, 2 };
    RaiseException(0xE0000001, 0, 2, a);
    say("RaiseException returned\n");
    return 0x7777;
}

static LONG WINAPI top(EXCEPTION_POINTERS *ep) {
    char b[64], h[12];
    strcpy(b, "top filter: code ");
    hex(h, ep->ExceptionRecord->ExceptionCode);
    strcat(b, h);
    strcat(b, " flags ");
    hex(h, ep->ExceptionRecord->ExceptionFlags);
    strcat(b, h);
    strcat(b, "\n");
    say(b);
    return g_ret;
}

static DWORD WINAPI thr(void *p) {
    (void)p;
    c_frame();
    say("thread: c_frame returned\n");
    return 5;
}

static int child(const char *mode, int threaded) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    g_ret = mode[0] == 'e' ? EXCEPTION_EXECUTE_HANDLER
          : mode[0] == 'c' ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(top);
    say(prev ? "previous filter set\n" : "previous filter NULL\n");
    if (threaded) {
        HANDLE h = CreateThread(0, 0, thr, 0, 0, 0);
        say(WaitForSingleObject(h, 10000) == WAIT_OBJECT_0 ? "main: the thread ended\n"
                                                           : "main: the thread did not end\n");
        return 3;
    }
    c_frame();
    say("main: c_frame returned\n");
    return 4;
}

struct expect {
    const char *args, *out;
    DWORD code;
};
static const struct expect g_exp[] = {
    { "e",
      "previous filter set\nscope filter: continue search\ntop filter: code e0000001 flags 80\n"
      "finally (abnormal)\n", 0xE0000001u },
    { "s",
      "previous filter set\nscope filter: continue search\ntop filter: code e0000001 flags 80\n"
      "finally (abnormal)\n", 0xE0000001u },
    { "c",
      "previous filter set\nscope filter: continue search\ntop filter: code e0000001 flags 80\n"
      "RaiseException returned\nmain: c_frame returned\ntls: process detach\n", 4 },
    { "e t",
      "previous filter set\nscope filter: continue search\ntop filter: code e0000001 flags 80\n"
      "finally (abnormal)\n", 0xE0000001u },
    { "c t",
      "previous filter set\nscope filter: continue search\ntop filter: code e0000001 flags 80\n"
      "RaiseException returned\nthread: c_frame returned\ntls: thread detach\n"
      "main: the thread ended\ntls: process detach\n", 3 },
};

/* Run "<self> <args>" with stdout a pipe; its output and exit code. */
static int run_child(const char *self, const char *args, char *out, int cap, DWORD *code) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    char cmd[400];
    strcpy(cmd, "\"");
    strcat(cmd, self);
    strcat(cmd, "\" ");
    strcat(cmd, args);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    int n = 0;
    for (;;) {
        DWORD got = 0;
        if (n >= cap - 1 || !ReadFile(rd, out + n, (DWORD)(cap - 1 - n), &got, 0) || !got) break;
        n += (int)got;
    }
    out[n] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 20000);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1], argc > 2 && argv[2][0] == 't');
    say("win-top33: an exception no frame takes, and the top-level filter\n");
    char self[MAX_PATH];
    if (!GetModuleFileNameA(0, self, sizeof self)) return 99;
    int first = 0;
    for (unsigned i = 0; i < sizeof g_exp / sizeof g_exp[0]; i++) {
        char out[1024], line[80], x[12];
        DWORD code = 0x12345;
        int n = run_child(self, g_exp[i].args, out, sizeof out, &code);
        int ok = n >= 0 && strcmp(out, g_exp[i].out) == 0 && code == g_exp[i].code;
        strcpy(line, ok ? "ok   " : "FAIL ");
        strcat(line, "child '");
        strcat(line, g_exp[i].args);
        strcat(line, "': exit 0x");
        hex(x, code);
        strcat(line, x);
        strcat(line, "\n");
        say(line);
        if (!ok) {
            say("  its output:\n");
            say(n >= 0 ? out : "(not run)\n");
            if (!first) first = (int)i + 1;
        }
    }
    say(first ? "win-top33: a child did not match\n" : "win-top33: all five as on Windows\n");
    return first ? 100 + first : 36;
}
