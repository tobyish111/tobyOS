/* win-msg41 -- Track C, W7j: a program's code the system calls with a
 * message, as Windows 11 calls it. CreateWindowEx sends WM_GETMINMAXINFO,
 * WM_NCCREATE, WM_NCCALCSIZE and WM_CREATE before it returns (and a refusal
 * gets WM_DESTROY / WM_NCDESTROY and no window); an exception escaping
 * WM_CREATE ends the process with 0xC000041D whatever the top-level filter
 * says; SendMessage to the program's own window is a call of its window
 * procedure (an exception there goes to SendMessage's caller); a TimerProc --
 * a thread timer's or a window's -- is called by DispatchMessage, its
 * exception swallowed, and a WM_TIMER naming a TimerProc no timer has is
 * not; an atexit function's exception goes through exit() to its caller's
 * handler, and the next exit runs the rest; a dialog an exception left is
 * still a window. Each check what Windows 11 does (measured first: it exits
 * 43 there). Exit 43 iff all pass, else 100 + the first that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern uint64_t f_try(uint64_t (*fn)(void));
static DWORD g_code;
static int g_filtered;
LONG so_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)frame;
    g_code = ep->ExceptionRecord->ExceptionCode;
    g_filtered++;
    return EXCEPTION_EXECUTE_HANDLER;
}
static int g_veh;
static DWORD g_veh_code;
static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    if ((ep->ExceptionRecord->ExceptionCode & 0xFFFF0000u) == 0xE0000000u) {
        g_veh++;
        g_veh_code = ep->ExceptionRecord->ExceptionCode;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

/* ---- the window procedure ---- */
static char g_seen[256];          /* the creation messages, in order */
static void seen(UINT m) {
    char b[8];
    snprintf(b, sizeof b, "%x ", m);
    strcat(g_seen, b);
}
static int g_mode;                /* 1 WM_CREATE -1, 2 WM_NCCREATE 0, 3 WM_CREATE raises */
static int g_cs_ok = 1;
static void *g_param;
static int g_wtimer;
static UINT_PTR g_wtimer_id;
static LPARAM g_wtimer_l;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETMINMAXINFO || m == WM_NCCREATE || m == WM_NCCALCSIZE || m == WM_CREATE ||
        m == WM_DESTROY || m == WM_NCDESTROY)
        seen(m);
    if (m == WM_NCCREATE || m == WM_CREATE) {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)l;
        g_cs_ok &= cs->lpCreateParams == g_param && cs->hInstance == GetModuleHandleA(0) && !cs->hMenu &&
                   !cs->hwndParent && cs->cy == 144 && cs->cx == 233 && cs->y == 22 && cs->x == 11 &&
                   cs->style == WS_OVERLAPPEDWINDOW && !strcmp(cs->lpszName, "msg41") &&
                   !strcmp(cs->lpszClass, "msg41w");
        if (m == WM_NCCREATE && g_mode == 2) return 0;
        if (m == WM_CREATE && g_mode == 1) return -1;
        if (m == WM_CREATE && g_mode == 3) { RaiseException(0xE0000C01, 0, 0, 0); say("back\n"); }
    }
    if (m == WM_USER + 1) return 5;
    if (m == WM_USER + 2) return SendMessageA(h, WM_USER + 1, 0, 0) + 1;
    if (m == WM_USER + 3) { RaiseException(0xE0000D03, 0, 0, 0); return 0; }
    if (m == WM_TIMER) { g_wtimer++; g_wtimer_id = w; g_wtimer_l = l; return 0; }
    return DefWindowProcA(h, m, w, l);
}
static HWND make(void *param) {
    static int registered;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA(0);
        wc.lpszClassName = "msg41w";
        RegisterClassA(&wc);
        registered = 1;
    }
    g_param = param;
    g_seen[0] = 0;
    return CreateWindowExA(0, "msg41w", "msg41", WS_OVERLAPPEDWINDOW, 11, 22, 233, 144, 0, 0,
                           GetModuleHandleA(0), param);
}
static uint64_t c_make(void) { return (uint64_t)(uintptr_t)make((void *)0x1234); }
static HWND s_h;
static uint64_t send3(void) { return (uint64_t)SendMessageA(s_h, WM_USER + 3, 0, 0); }

/* ---- timers ---- */
static int g_tp;
static HWND g_tp_h;
static UINT g_tp_m;
static UINT_PTR g_tp_id;
static DWORD g_tp_t, g_tp_now;
static int g_tp_raise;
static void CALLBACK tp(HWND h, UINT m, UINT_PTR id, DWORD t) {
    g_tp++;
    g_tp_h = h; g_tp_m = m; g_tp_id = id; g_tp_t = t; g_tp_now = GetTickCount();
    if (g_tp_raise) { RaiseException(0xE0000F01, 0, 0, 0); say("tp: back\n"); }
}
/* the next WM_TIMER (others dispatched): 1, its MSG in *out, DispatchMessage's
 * answer in *r; 0 if none came within ~3 s */
static MSG g_tm;
static LRESULT g_tr;
static int next_timer(void) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 3000) {
        MSG m;
        if (GetMessageA(&m, 0, 0, 0) <= 0) return 0;
        if (m.message == WM_TIMER) { g_tm = m; g_tr = DispatchMessageA(&m); return 1; }
        DispatchMessageA(&m);
    }
    return 0;
}
static uint64_t c_next_timer(void) { return (uint64_t)next_timer(); }

/* ---- the dialog: resource 100 (msg41.rc) ---- */
static HWND g_dlg;
static INT_PTR CALLBACK dlgproc(HWND d, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    if (m == WM_INITDIALOG) {
        g_dlg = d;
        if (g_mode == 4) RaiseException(0xE0000E01, 0, 0, 0);
        EndDialog(d, 9);
        return TRUE;
    }
    return FALSE;
}
static uint64_t c_dialog(void) { return (uint64_t)DialogBoxParamA(GetModuleHandleA(0), MAKEINTRESOURCEA(100), 0, dlgproc, 0); }

static DWORD run_self(const char *self, const char *arg, char *out, int cap) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", self, arg);
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return 0xDEAD;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessA(0, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) return 0xDEAD;
    CloseHandle(w);
    int n = 0;
    DWORD got;
    while (n < cap - 1 && ReadFile(r, out + n, (DWORD)(cap - 1 - n), &got, 0) && got) n += (int)got;
    out[n] = 0;
    CloseHandle(r);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD ec = 0xBEEF;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ec;
}

/* ---- children ---- */
static LONG g_top_ret = EXCEPTION_CONTINUE_SEARCH;
static LONG WINAPI top(EXCEPTION_POINTERS *ep) {
    char b[64];
    snprintf(b, sizeof b, "top %lx\n", ep->ExceptionRecord->ExceptionCode);
    say(b);
    return g_top_ret;
}
static void ax1(void) { say("atexit 1\n"); }
static void ax2(void) { say("atexit 2 raises\n"); RaiseException(0xE0000B01, 0, 0, 0); say("atexit 2 back\n"); }
static uint64_t do_exit(void) { exit(3); return 9; }
static int child(const char *m) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (strcmp(m, "createnf")) SetUnhandledExceptionFilter(top);
    if (!strcmp(m, "createx")) g_top_ret = EXCEPTION_EXECUTE_HANDLER;
    if (!strncmp(m, "create", 6)) {
        g_mode = 3;
        uint64_t r = f_try(c_make);
        char b[64];
        snprintf(b, sizeof b, "f_try returned %llx\n", (unsigned long long)r);
        say(b);
        return 4;
    }
    atexit(ax1);
    atexit(ax2);
    if (!strcmp(m, "axtry")) {
        uint64_t r = f_try(do_exit);
        char b[64];
        snprintf(b, sizeof b, "f_try returned %llx\n", (unsigned long long)r);
        say(b);
        return 7;
    }
    return 4;                                /* "axret": main returns */
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1]);
    AddVectoredExceptionHandler(1, veh);
    char out[512];

    /* 1: WM_CREATE's exception -- first: tobyOS's GUI state is one for the
     * system (phase 2's), so a child that registers a class and makes a
     * window prunes the windows of a parent that has any */
    {
        DWORD e1 = run_self(argv[0], "create", out, sizeof out);
        printf("    child create: %lx [%s]\n", e1, out);
        int ok = e1 == 0xC000041D && !strcmp(out, "top e0000c01\n");
        DWORD e2 = run_self(argv[0], "createx", out, sizeof out);
        printf("    child createx: %lx [%s]\n", e2, out);
        ok &= e2 == 0xC000041D && !strcmp(out, "top e0000c01\n");
        DWORD e3 = run_self(argv[0], "createnf", out, sizeof out);
        printf("    child createnf: %lx\n", e3);
        ok &= e3 == 0xC000041D && !strstr(out, "f_try");
        check(1, ok, "an exception escaping WM_CREATE: the top-level filter, then 0xC000041D (filter or none, whatever it says); the caller's handler never");
    }
    /* 2: CreateWindowEx's messages */
    HWND h = make((void *)0x1234);
    printf("    created: %s\n", g_seen);
    check(2, h && !strcmp(g_seen, "24 81 83 1 ") && g_cs_ok,
          "CreateWindowEx sends WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE (the CREATESTRUCT) before it returns");
    /* 3: refusals */
    {
        g_mode = 1;
        HWND h1 = make(0);
        int ok = !h1 && !strcmp(g_seen, "24 81 83 1 2 82 ");
        printf("    WM_CREATE -1: %s\n", g_seen);
        g_mode = 2;
        HWND h2 = make(0);
        ok &= !h2 && !strcmp(g_seen, "24 81 82 ");
        printf("    WM_NCCREATE 0: %s\n", g_seen);
        g_mode = 0;
        check(3, ok, "WM_CREATE's -1: WM_DESTROY, WM_NCDESTROY, no window; WM_NCCREATE's FALSE: WM_NCDESTROY, no window");
    }
    /* 4: SendMessage to the program's own window */
    {
        int ok = SendMessageA(h, WM_USER + 1, 0, 0) == 5 && SendMessageA(h, WM_USER + 2, 0, 0) == 6;
        s_h = h;
        g_filtered = 0;
        uint64_t r = f_try(send3);
        ok &= r == 0xE0000D03 && g_filtered == 1 && SendMessageA(h, WM_USER + 1, 0, 0) == 5;
        check(4, ok, "SendMessage to its own window calls the window procedure (nested too); its exception goes to SendMessage's caller");
    }
    /* 5: a thread timer and its TimerProc */
    {
        UINT_PTR id = SetTimer(0, 77, 1, tp);
        g_tp = 0;
        /* (DispatchMessage's answer is the TimerProc's rax: a TimerProc is void) */
        int ok = id != 0 && next_timer() && !g_tm.hwnd && g_tm.wParam == id && g_tm.lParam == (LPARAM)tp &&
                 g_tp == 1 && !g_tp_h && g_tp_m == WM_TIMER && g_tp_id == id &&
                 (DWORD)(g_tp_now - g_tp_t) < 1000;
        printf("    thread timer: id %llx, WM_TIMER hwnd %p wParam %llx, the TimerProc called %d\n",
               (unsigned long long)id, (void *)g_tm.hwnd, (unsigned long long)g_tm.wParam, g_tp);
        /* 6: its exception swallowed */
        g_tp_raise = 1;
        g_veh = 0;
        g_filtered = 0;
        uint64_t r = f_try(c_next_timer);
        int ok6 = r == 1 && g_veh == 1 && g_veh_code == 0xE0000F01 && g_filtered == 0 && g_tp >= 2;
        g_tp_raise = 0;
        BOOL k1 = KillTimer(0, id);
        SetLastError(0);
        BOOL k2 = KillTimer(0, id);
        DWORD k2e = GetLastError();
        ok &= k1 == TRUE && k2 == FALSE && k2e == 0;
        check(5, ok, "SetTimer(NULL): an id of the system's; WM_TIMER {NULL, id, the TimerProc}; DispatchMessage calls it (NULL, WM_TIMER, id, the time); a second KillTimer: FALSE");
        check(6, ok6, "a TimerProc's exception: the vectored handler sees it, DispatchMessage swallows it");
    }
    /* 7: a window's timers */
    {
        int ok = SetTimer(h, 7, 1, tp) == 7;
        g_tp = 0; g_wtimer = 0;
        ok &= next_timer() && g_tm.hwnd == h && g_tm.wParam == 7 && g_tp == 1 && g_tp_h == h && g_tp_id == 7 && g_wtimer == 0;
        KillTimer(h, 7);
        ok &= SetTimer(h, 8, 1, 0) == 8;
        g_tp = 0; g_wtimer = 0;
        ok &= next_timer() && g_tm.wParam == 8 && g_wtimer == 1 && g_wtimer_id == 8 && g_wtimer_l == 0 && g_tp == 0;
        KillTimer(h, 8);
        MSG fake = { h, WM_TIMER, 9, (LPARAM)tp, 0, { 0, 0 } };
        g_tp = 0; g_wtimer = 0;
        LRESULT r = DispatchMessageA(&fake);
        ok &= r == 0 && g_tp == 0 && g_wtimer == 0;
        check(7, ok, "a window's timer: its TimerProc, or WM_TIMER to the window procedure; a WM_TIMER naming a TimerProc no timer has goes nowhere");
    }
    /* 8: an atexit function's exception */
    {
        DWORD e1 = run_self(argv[0], "axtry", out, sizeof out);
        printf("    child axtry: %lx [%s]\n", e1, out);
        int ok = e1 == 7 && !strcmp(out, "atexit 2 raises\nf_try returned e0000b01\natexit 1\n");
        DWORD e2 = run_self(argv[0], "axret", out, sizeof out);
        printf("    child axret: %lx [%s]\n", e2, out);
        ok &= e2 == 0xE0000B01 && !strcmp(out, "atexit 2 raises\ntop e0000b01\n");
        check(8, ok, "an atexit function's exception goes through exit() to its caller's handler, the next exit runs the rest; unhandled: the filter, the code");
    }
    /* 9: a dialog an exception unwound out of */
    {
        g_mode = 4;
        g_dlg = 0;
        uint64_t r = f_try(c_dialog);
        int ok = r == 0xE0000E01 && g_dlg && IsWindow(g_dlg);
        g_mode = 0;
        HWND first = g_dlg;
        ok &= f_try(c_dialog) == 9 && g_dlg != first;
        check(9, ok, "a dialog an exception unwound out of is still a window (Windows does not end it); the next dialog runs");
    }
    printf("%s\n", first_fail ? "SOME FAILED" : "ALL PASS");
    return first_fail ? 100 + first_fail : 43;
}

