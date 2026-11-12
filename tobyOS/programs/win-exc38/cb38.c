/* win-cb38 -- Track C, W7g: an exception raised in a program's code that
 * the system called. A window procedure under DispatchMessage, and a dialog
 * procedure under DialogBoxParam: the exception goes on up to the caller's
 * handlers, through the system's frames. DllMain: the loader takes what it
 * raises under LoadLibrary (which fails, ERROR_DLL_INIT_FAILED, with no
 * DLL_PROCESS_DETACH) and FreeLibrary (which succeeds); at process start the
 * process fails (STATUS_DLL_INIT_FAILED); at a thread's start the process
 * ends with the code, the top-level filter not asked; at process exit and a
 * thread's end it is an unhandled exception. Each check what Windows 11 does
 * (measured first: it exits 40 there). Exit 40 iff all pass, else 100 + the
 * first that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern uint64_t f_try(uint64_t (*fn)(void *), void *arg);
extern uint64_t f_fin(uint64_t (*fn)(void *), void *arg);
#define NOTHING_RAISED (1ull << 62)

static char g_log[256];
static void lg(const char *s) { strcat(g_log, s); }
static void hex8(char *b, uint64_t v) {
    for (int i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15];
    b[8] = 0;
}
static void lgx(const char *p, uint64_t v) {
    char b[16];
    hex8(b, v);
    lg(p);
    lg(b);
    lg(" ");
}
LONG cb_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)frame;
    lgx("filter:", ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}
void cb_finally(BOOLEAN ab, void *frame) { (void)frame; lg(ab ? "fin(ab) " : "fin(n) "); }
static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    lgx("veh:", ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
    if (!ok) printf("        log: %s\n", g_log);
}

/* 1, 2: a window procedure */
static HWND g_hwnd;
static uint64_t dispatch(void *msg) { return (uint64_t)DispatchMessageA((MSG *)msg); }
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER) { lg("wm_user "); RaiseException(0xE0000D01, 0, 0, 0); lg("back "); return 0; }
    if (m == WM_USER + 1) return 5;
    if (m == WM_USER + 2) {                   /* the system again, from inside: two of its frames */
        MSG in;
        memset(&in, 0, sizeof in);
        in.hwnd = h;
        in.message = WM_USER;
        lg("wm_user2 ");
        f_fin(dispatch, &in);
        lg("back2 ");
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static uint64_t post(UINT m) {
    MSG msg;
    memset(&msg, 0, sizeof msg);
    msg.hwnd = g_hwnd;
    msg.message = m;
    return f_try(dispatch, &msg);
}

/* 3: a dialog procedure */
static int g_dlg_mode;
static INT_PTR CALLBACK dlgproc(HWND d, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    if (m == WM_INITDIALOG) {
        lg("initdialog ");
        if (g_dlg_mode == 0) { RaiseException(0xE0000E02, 0, 0, 0); lg("back "); }
        EndDialog(d, 7);
        return TRUE;
    }
    return FALSE;
}
static uint64_t dialog(void *a) {
    (void)a;
    return (uint64_t)DialogBoxParamA(GetModuleHandleA(0), MAKEINTRESOURCEA(100), 0, dlgproc, 0);
}

/* 4, 5: LoadLibrary and FreeLibrary */
static uint64_t load(void *a) { return (uint64_t)(uintptr_t)LoadLibraryA((const char *)a); }
static uint64_t unload(void *a) { return (uint64_t)FreeLibrary((HMODULE)a); }
static void mode(const char *m) {
    SetEnvironmentVariableA("D38_MODE", m);
    SetEnvironmentVariableA("D38_LOG", "");
}
static const char *dlog(void) {
    static char b[256];
    b[0] = 0;
    GetEnvironmentVariableA("D38_LOG", b, sizeof b);
    return b;
}

/* 6: the children */
static int run_child(const char *exe, const char *args, char *out, int cap, DWORD *code) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    char cmd[400];
    strcpy(cmd, "\"");
    strcat(cmd, exe);
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
    if (!CreateProcessA(exe, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return -1; }
    CloseHandle(wr);
    int n = 0;
    for (;;) {
        DWORD got = 0;
        if (n >= cap - 1 || !ReadFile(rd, out + n, (DWORD)(cap - 1 - n), &got, 0) || !got) break;
        n += (int)got;
    }
    out[n] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return n;
}

int main(void) {
    printf("win-cb38: exceptions in code the system calls\n");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    AddVectoredExceptionHandler(1, veh);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "cb38";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(0, "cb38", "cb38", WS_OVERLAPPEDWINDOW, 0, 0, 160, 100, 0, 0, wc.hInstance, 0);

    g_log[0] = 0;
    uint64_t r1 = post(WM_USER);
    uint64_t r1b = post(WM_USER + 1);
    check(1, g_hwnd && r1 == 0xE0000D01 && r1b == (NOTHING_RAISED | 5) &&
             !strcmp(g_log, "wm_user veh:e0000d01 filter:e0000d01 fin(ab) "),
          "a window procedure's exception, under DispatchMessage: the caller's __except takes it; the window goes on");

    g_log[0] = 0;
    uint64_t r2 = post(WM_USER + 2);
    check(2, r2 == 0xE0000D01 &&
             !strcmp(g_log, "wm_user2 wm_user veh:e0000d01 filter:e0000d01 fin(ab) fin(ab) "),
          "through two DispatchMessage frames and a __finally between them");

    g_log[0] = 0;
    g_dlg_mode = 0;
    uint64_t r3 = f_try(dialog, 0);
    g_dlg_mode = 1;
    uint64_t r3b = f_try(dialog, 0);
    check(3, r3 == 0xE0000E02 && r3b == (NOTHING_RAISED | 7) &&
             !strcmp(g_log, "initdialog veh:e0000e02 filter:e0000e02 fin(ab) initdialog "),
          "a dialog procedure's, under DialogBoxParam: the caller's __except takes it; another dialog after works");

    g_log[0] = 0;
    mode("attach");
    uint64_t r4 = f_try(load, (void *)"dll38.dll");
    DWORD e4 = GetLastError();
    HMODULE m4 = GetModuleHandleA("dll38.dll");
    check(4, r4 == NOTHING_RAISED && e4 == ERROR_DLL_INIT_FAILED && !m4 && !strcmp(dlog(), "attach ") &&
             !strcmp(g_log, "veh:e0000a01 "),
          "DllMain's under LoadLibrary: the loader takes it -- NULL, ERROR_DLL_INIT_FAILED, no DLL_PROCESS_DETACH");
    if (r4 != NOTHING_RAISED || e4 != ERROR_DLL_INIT_FAILED || m4) printf("        r %llx error %lu module %p dll: %s\n", (unsigned long long)r4, e4, (void *)m4, dlog());

    g_log[0] = 0;
    mode("detach");
    HMODULE m5 = LoadLibraryA("dll38.dll");
    uint64_t r5 = m5 ? f_try(unload, m5) : 0;
    HMODULE m5b = GetModuleHandleA("dll38.dll");
    check(5, m5 && r5 == (NOTHING_RAISED | 1) && !m5b && !strcmp(dlog(), "attach detach ") &&
             !strcmp(g_log, "veh:e0000a02 "),
          "DllMain's under FreeLibrary: the loader takes it -- TRUE, the DLL gone");
    if (!m5 || r5 != (NOTHING_RAISED | 1) || m5b) printf("        r %llx module after %p dll: %s\n", (unsigned long long)r5, (void *)m5b, dlog());

    char self[MAX_PATH], child[MAX_PATH + 16];
    GetModuleFileNameA(0, self, sizeof self);
    strcpy(child, self);
    char *slash = strrchr(child, '\\');
    strcpy(slash ? slash + 1 : child, "win-cb38s.exe");
    static const struct { const char *mode, *args, *out; DWORD code; } ch[] = {
        { "none", "", "main\nmain returns\n", 5 },
        { "attach", "", "", 0xC0000142u },
        { "detach", "", "main\nmain returns\nveh e0000a02\ntop e0000a02\n", 0xE0000A02u },
        { "thread", "thread", "main\nveh e0000a03\n", 0xE0000A03u },
        { "tdetach", "thread", "main\nthread\nveh e0000a04\ntop e0000a04\n", 0xE0000A04u },
    };
    int ok6 = 1;
    for (unsigned i = 0; i < sizeof ch / sizeof ch[0]; i++) {
        char out[256];
        DWORD code = 0x12345;
        mode(ch[i].mode);
        int n = run_child(child, ch[i].args, out, sizeof out, &code);
        int ok = n >= 0 && !strcmp(out, ch[i].out) && code == ch[i].code;
        printf("     child %-7s exit 0x%08lx %s\n", ch[i].mode, code, ok ? "" : "(not as on Windows)");
        if (!ok) { ok6 = 0; printf("%s", out); }
    }
    check(6, ok6, "DllMain's at process start (0xC0000142), at exit and a thread's end (unhandled), "
                  "at a thread's start (the code, no top-level filter)");

    printf("win-cb38: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 40;
}
