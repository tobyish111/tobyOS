/* win-wtext51 -- Track C, W7t: windows' text in UTF-16, and another
 * process's title. A class registered by RegisterClassW makes Unicode
 * windows (IsWindowUnicode), RegisterClassA's ANSI ones, whichever of
 * CreateWindowExA/W made them; a window's title is UTF-16, the A functions
 * see it through the ANSI code page (1252: a character it lacks is '?').
 * The procedure of a window gets WM_NCCREATE's CREATESTRUCT, WM_SETTEXT's
 * text and WM_GETTEXT's buffer in its own kind, converted when the sender
 * is of the other kind (WM_GETTEXTLENGTH across kinds is followed by a
 * WM_GETTEXT, to count the other kind's characters); DefWindowProcA/W keep
 * and give the text. SetWindowTextA/W send WM_SETTEXT to the program's own
 * windows (a procedure that does not pass it on keeps the old title,
 * SetWindowText answers TRUE all the same), GetWindowText(Length)A/W send
 * WM_GETTEXT(LENGTH). Another process's window is not asked: SetWindowText
 * sets its title and GetWindowText reads it without a message (an owner
 * that does not take messages holds neither up). Each check is what Windows 11
 * does (measured first: this program exits 53 there); the program runs
 * itself as the child. Exit 53 iff all pass, else 100 + the first check
 * that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
static char g_s[4096];
static int g_k;
static void s_reset(void) { g_k = 0; g_s[0] = 0; }
static void s_add(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_s + g_k, sizeof g_s - (size_t)g_k, fmt, ap);
    va_end(ap);
    if (n > 0 && g_k + n < (int)sizeof g_s) g_k += n;
}
/* text: ASCII as it is, the rest as \uXXXX (wide) or \xHH (narrow) */
static void s_wt(const wchar_t *w) { for (; *w; w++) { if (*w >= 0x20 && *w < 0x7F) s_add("%c", (char)*w); else s_add("\\u%04X", (unsigned)*w); } }
static void s_nt(const char *b) { for (; *b; b++) { unsigned char c = (unsigned char)*b; if (c >= 0x20 && c < 0x7F) s_add("%c", c); else s_add("\\x%02X", c); } }
static int seen(int n, const char *want) {
    printf("     %d saw:%s\n", n, g_s);
    if (strcmp(g_s, want) != 0) { printf("     %d want:%s\n", n, want); return 0; }
    return 1;
}

/* what a procedure was given: its log */
static char g_log[1024];
static int g_logn;
static void log_add(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_log + g_logn, sizeof g_log - (size_t)g_logn, fmt, ap);
    va_end(ap);
    if (n > 0 && g_logn + n < (int)sizeof g_log) g_logn += n;
}
static void log_w(const wchar_t *w) { if (!w) { log_add("(null)"); return; } for (; *w; w++) { if (*w >= 0x20 && *w < 0x7F) log_add("%c", (char)*w); else log_add("\\u%04X", (unsigned)*w); } }
static void log_n(const char *b) { if (!b) { log_add("(null)"); return; } for (; *b; b++) { unsigned char c = (unsigned char)*b; if (c >= 0x20 && c < 0x7F) log_add("%c", c); else log_add("\\x%02X", c); } }
static void log_take(void) { s_add(" {%s}", g_log); g_logn = 0; g_log[0] = 0; }

#define WM_HELLO  (WM_APP + 1)    /* child -> parent: wParam its window */
#define WM_REPORT (WM_APP + 2)    /* parent -> child, sent: the child's log to the parent by WM_COPYDATA */
#define WM_STALL  (WM_APP + 3)    /* parent -> child, posted: the child sleeps 1.5 s */
#define WM_BYE    (WM_APP + 4)    /* parent -> child, posted: destroy and quit */

/* an ANSI window's procedure */
static LRESULT CALLBACK a_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCREATE: {
        const CREATESTRUCTA *cs = (const CREATESTRUCTA *)l;
        log_add(" A-NCCREATE:"); log_n(cs->lpszName); log_add("/");
        if ((ULONG_PTR)cs->lpszClass < 0x10000) log_add("#%u", (unsigned)(ULONG_PTR)cs->lpszClass); else log_n(cs->lpszClass);
        break;
    }
    case WM_SETTEXT:
        log_add(" A-SETTEXT:"); log_n((const char *)l);
        if (l && !strncmp((const char *)l, "keep", 4)) return FALSE;
        break;
    case WM_GETTEXT: log_add(" A-GETTEXT:%u", (unsigned)w); break;
    case WM_GETTEXTLENGTH: log_add(" A-GETTEXTLENGTH"); break;
    }
    return DefWindowProcA(h, m, w, l);
}
/* a Unicode window's */
static LRESULT CALLBACK w_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCREATE: {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)l;
        log_add(" W-NCCREATE:"); log_w(cs->lpszName); log_add("/");
        if ((ULONG_PTR)cs->lpszClass < 0x10000) log_add("#%u", (unsigned)(ULONG_PTR)cs->lpszClass); else log_w(cs->lpszClass);
        break;
    }
    case WM_SETTEXT:
        log_add(" W-SETTEXT:%s", InSendMessage() ? "(sent)" : ""); log_w((const wchar_t *)l);
        if (l && !wcsncmp((const wchar_t *)l, L"keep", 4)) return FALSE;
        break;
    case WM_GETTEXT: log_add(" W-GETTEXT:%u", (unsigned)w); break;
    case WM_GETTEXTLENGTH: log_add(" W-GETTEXTLENGTH"); break;
    case WM_REPORT: {
        HWND to = (HWND)w;
        COPYDATASTRUCT cd = { 51, (DWORD)(g_logn + 1), g_log };
        LRESULT r = SendMessageW(to, WM_COPYDATA, (WPARAM)h, (LPARAM)&cd);
        g_logn = 0; g_log[0] = 0;
        return r;
    }
    case WM_STALL: Sleep(1500); return 0;
    case WM_BYE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(4); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---- the child: a Unicode window, its procedure's log sent back ---- */
static int child(HWND parent) {
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = w_proc; wc.hInstance = GetModuleHandleW(0); wc.lpszClassName = L"W51Child";
    if (!RegisterClassW(&wc)) return 20;
    HWND h = CreateWindowExW(0, L"W51Child", L"child \u0416", WS_OVERLAPPEDWINDOW, 10, 10, 200, 100, 0, 0, wc.hInstance, 0);
    if (!h) return 21;
    g_logn = 0; g_log[0] = 0;
    if (!PostMessageW(parent, WM_HELLO, (WPARAM)h, 0)) return 22;
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) DispatchMessageW(&msg);
    return (int)msg.wParam;
}

/* ---- the parent's own window, which hears the child ---- */
static HWND g_child;
static int g_hello, g_copied;
static char g_copy[1024];
static LRESULT CALLBACK parent_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HELLO: g_child = (HWND)w; g_hello = 1; return 0;
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)l;
        size_t n = cd->cbData < sizeof g_copy ? cd->cbData : sizeof g_copy - 1;
        memcpy(g_copy, cd->lpData, n); g_copy[n] = 0; g_copied = 1;
        return 7;
    }
    }
    return DefWindowProcA(h, m, w, l);
}
static void pump_until(volatile int *flag, DWORD ms) {
    DWORD t0 = GetTickCount();
    MSG msg;
    while (!*flag && GetTickCount() - t0 < ms) {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (!*flag) Sleep(5);
    }
}
static HWND g_parent;
/* the child's log, by WM_REPORT */
static void child_log(void) {
    g_copied = 0; g_copy[0] = 0;
    LRESULT r = SendMessageW(g_child, WM_REPORT, (WPARAM)g_parent, 0);
    s_add(" {%s}%ld", g_copy, (long)r);
}
static void texts(HWND h) {
    wchar_t wb[64]; char nb[64];
    wmemset(wb, L'#', 64); memset(nb, '#', 64);
    int a = GetWindowTextW(h, wb, 64), b = GetWindowTextA(h, nb, 64);
    int c = GetWindowTextLengthW(h), d = GetWindowTextLengthA(h);
    s_add(" [%d]", a); s_wt(wb); s_add(" [%d]", b); s_nt(nb); s_add(" %d %d", c, d);
}

static unsigned long long parse_u64(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) return child((HWND)(UINT_PTR)parse_u64(argv[2]));
    HINSTANCE inst = GetModuleHandleA(0);

    WNDCLASSA ca; memset(&ca, 0, sizeof ca);
    ca.lpfnWndProc = a_proc; ca.hInstance = inst; ca.lpszClassName = "W51Ansi";
    ATOM aa = RegisterClassA(&ca);
    WNDCLASSW cw; memset(&cw, 0, sizeof cw);
    cw.lpfnWndProc = w_proc; cw.hInstance = inst; cw.lpszClassName = L"W51Wide";
    ATOM aw = RegisterClassW(&cw);
    ca.lpfnWndProc = parent_proc; ca.lpszClassName = "W51Parent";
    RegisterClassA(&ca);

    /* 1: the windows each kind of class makes, by CreateWindowExA and W; what WM_NCCREATE gets */
    s_reset();
    s_add(" %d %d", aa != 0, aw != 0);
    HWND hA = CreateWindowExA(0, "W51Ansi", "Stra\xDF" "e \x80", WS_OVERLAPPEDWINDOW, 0, 0, 100, 60, 0, 0, inst, 0);
    log_take();
    HWND hW = CreateWindowExW(0, L"W51Wide", L"Gr\u00F6\u00DFe \u0416\u20AC", WS_OVERLAPPEDWINDOW, 0, 0, 100, 60, 0, 0, inst, 0);
    log_take();
    HWND hAW = CreateWindowExW(0, L"W51Ansi", L"\u0416a\u00E9", WS_OVERLAPPEDWINDOW, 0, 0, 100, 60, 0, 0, inst, 0);
    log_take();
    HWND hWA = CreateWindowExA(0, "W51Wide", "b\xE9\x80", WS_OVERLAPPEDWINDOW, 0, 0, 100, 60, 0, 0, inst, 0);
    log_take();
    s_add(" | %d %d %d %d | %d %d %d %d", hA != 0, hW != 0, hAW != 0, hWA != 0,
          IsWindowUnicode(hA), IsWindowUnicode(hW), IsWindowUnicode(hAW), IsWindowUnicode(hWA));
    check(1, seen(1,
        " 1 1 { A-NCCREATE:Stra\\xDFe \\x80/W51Ansi} { W-NCCREATE:Gr\\u00F6\\u00DFe \\u0416\\u20AC/W51Wide} { A-NCCREATE:?a\\xE9/W51Ansi} { W-NCCREATE:b\\u00E9\\u20AC/W5"
        "1Wide} | 1 1 1 1 | 0 1 0 1"), "RegisterClassW's windows are Unicode, RegisterClassA's ANSI, by CreateWindowExA or W; WM_NCCREATE's names in the window's kind");

    /* 2: the titles both ways */
    s_reset();
    texts(hA); texts(hW); texts(hAW); texts(hWA);
    log_take();
    check(2, seen(2,
        " [8]Stra\\u00DFe \\u20AC [8]Stra\\xDFe \\x80 8 8 [8]Gr\\u00F6\\u00DFe \\u0416\\u20AC [8]Gr\\xF6\\xDFe ?\\x80 8 8 [3]?a\\u00E9 [3]?a\\xE9 3 3 [3]b\\u00E9\\u20AC "
        "[3]b\\xE9\\x80 3 3 { A-GETTEXT:128 A-GETTEXT:64 A-GETTEXTLENGTH A-GETTEXT:9 A-GETTEXTLENGTH W-GETTEXT:64 W-GETTEXT:64 W-GETTEXTLENGTH W-GETTEXTLENGTH W-GETTEXT:"
        "9 A-GETTEXT:128 A-GETTEXT:64 A-GETTEXTLENGTH A-GETTEXT:4 A-GETTEXTLENGTH W-GETTEXT:64 W-GETTEXT:64 W-GETTEXTLENGTH W-GETTEXTLENGTH W-GETTEXT:4}"), "GetWindowTextA/W and GetWindowTextLengthA/W: a title is UTF-16, the A functions' through code page 1252");

    /* 3: the class's name both ways */
    s_reset();
    {
        wchar_t wb[64]; char nb[64];
        int a = GetClassNameW(hA, wb, 64); s_add(" [%d]", a); s_wt(wb);
        a = GetClassNameA(hW, nb, 64); s_add(" [%d]", a); s_nt(nb);
        a = GetClassNameW(hW, wb, 4); s_add(" [%d]", a); s_wt(wb);
        a = GetClassNameA(hA, nb, 4); s_add(" [%d]", a); s_nt(nb);
        SetLastError(0); a = GetClassNameW((HWND)(ULONG_PTR)0x1234560, wb, 64); s_add(" [%d] %lu", a, GetLastError());
        s_add(" | %d", FindWindowW(L"W51Wide", L"Gr\u00F6\u00DFe \u0416\u20AC") == hW);
        s_add(" %d", FindWindowA("W51Wide", "Gr\xF6\xDF" "e ?\x80") == hW);
        s_add(" %d", FindWindowW(NULL, L"\u0416a\u00E9") == hAW);
    }
    check(3, seen(3,
        " [7]W51Ansi [7]W51Wide [3]W51 [3]W51 [0] 1400 | 1 0 0"), "GetClassNameA/W; FindWindowW by a title past code page 1252, FindWindowA by its ANSI form");

    /* 4: SetWindowTextA/W send WM_SETTEXT in the window's kind */
    s_reset();
    {
        BOOL r1 = SetWindowTextA(hW, "A\xE4\x80");
        log_take(); texts(hW);
        BOOL r2 = SetWindowTextW(hA, L"B\u0416\u00E4\u20AC");
        log_take(); texts(hA);
        BOOL r3 = SetWindowTextW(hW, L"C\u0416");
        log_take(); texts(hW);
        BOOL r4 = SetWindowTextA(hA, NULL);
        log_take(); texts(hA);
        s_add(" | %d %d %d %d", r1, r2, r3, r4);
    }
    check(4, seen(4,
        " { W-SETTEXT:A\\u00E4\\u20AC} [3]A\\u00E4\\u20AC [3]A\\xE4\\x80 3 3 { W-GETTEXT:64 W-GETTEXT:64 W-GETTEXTLENGTH W-GETTEXTLENGTH W-GETTEXT:4 A-SETTEXT:B?\\xE4\\x"
        "80} [4]B?\\u00E4\\u20AC [4]B?\\xE4\\x80 4 4 { A-GETTEXT:128 A-GETTEXT:64 A-GETTEXTLENGTH A-GETTEXT:5 A-GETTEXTLENGTH W-SETTEXT:C\\u0416} [2]C\\u0416 [2]C? 2 2 {"
        " W-GETTEXT:64 W-GETTEXT:64 W-GETTEXTLENGTH W-GETTEXTLENGTH W-GETTEXT:3 A-SETTEXT:(null)} [0] [0] 0 0 | 1 1 1 1"), "SetWindowTextA/W send WM_SETTEXT, its text converted to the window's kind; DefWindowProc keeps it");

    /* 5: a procedure that keeps the old title */
    s_reset();
    {
        SetLastError(0);
        BOOL r1 = SetWindowTextW(hW, L"keep mine"); DWORD e1 = GetLastError();
        log_take(); texts(hW);
        SetLastError(0);
        BOOL r2 = SetWindowTextA(hA, "keep that"); DWORD e2 = GetLastError();
        log_take(); texts(hA);
        s_add(" | %d %lu %d %lu", r1, e1, r2, e2);
    }
    check(5, seen(5,
        " { A-GETTEXT:128 A-GETTEXT:64 A-GETTEXTLENGTH A-GETTEXTLENGTH W-SETTEXT:keep mine} [2]C\\u0416 [2]C? 2 2 { W-GETTEXT:64 W-GETTEXT:64 W-GETTEXTLENGTH W-GETTEXTLE"
        "NGTH W-GETTEXT:3 A-SETTEXT:keep that} [0] [0] 0 0 | 1 0 1 0"), "a procedure that answers WM_SETTEXT itself keeps the old title; SetWindowText answers TRUE all the same");

    /* 6: WM_GETTEXT and WM_GETTEXTLENGTH sent each way */
    s_reset();
    {
        SetWindowTextW(hW, L"D\u00E9\u0416x");
        SetWindowTextW(hA, L"E\u00E9\u20ACy");
        log_take();
        wchar_t wb[16]; char nb[16];
        wmemset(wb, L'#', 16); LRESULT r = SendMessageW(hA, WM_GETTEXT, 16, (LPARAM)wb); s_add(" [%ld]", (long)r); s_wt(wb);
        memset(nb, '#', 16); r = SendMessageA(hW, WM_GETTEXT, 16, (LPARAM)nb); s_add(" [%ld]", (long)r); s_nt(nb);
        wmemset(wb, L'#', 16); r = SendMessageW(hW, WM_GETTEXT, 3, (LPARAM)wb); s_add(" [%ld]", (long)r); s_wt(wb);
        memset(nb, '#', 16); r = SendMessageA(hA, WM_GETTEXT, 3, (LPARAM)nb); s_add(" [%ld]", (long)r); s_nt(nb);
        long l1 = (long)SendMessageW(hA, WM_GETTEXTLENGTH, 0, 0);
        long l2 = (long)SendMessageA(hW, WM_GETTEXTLENGTH, 0, 0);
        long l3 = (long)SendMessageW(hW, WM_GETTEXTLENGTH, 0, 0);
        long l4 = (long)SendMessageA(hA, WM_GETTEXTLENGTH, 0, 0);
        s_add(" %ld %ld %ld %ld", l1, l2, l3, l4);
        wmemset(wb, L'#', 16); int n = GetWindowTextW(hW, wb, 3); s_add(" [%d]", n); s_wt(wb);
        memset(nb, '#', 16); n = GetWindowTextA(hA, nb, 3); s_add(" [%d]", n); s_nt(nb);
        log_take();
    }
    check(6, seen(6,
        " { A-GETTEXT:128 A-GETTEXT:64 A-GETTEXTLENGTH A-GETTEXTLENGTH W-SETTEXT:D\\u00E9\\u0416x A-SETTEXT:E\\xE9\\x80y} [4]E\\u00E9\\u20ACy [4]D\\xE9?x [2]D\\u00E9 [2]"
        "E\\xE9 4 4 4 4 [2]D\\u00E9 [2]E\\xE9 { A-GETTEXT:32 W-GETTEXT:16 W-GETTEXT:3 A-GETTEXT:3 A-GETTEXTLENGTH A-GETTEXT:5 W-GETTEXTLENGTH W-GETTEXT:5 W-GETTEXTLENGTH"
        " A-GETTEXTLENGTH W-GETTEXT:3 A-GETTEXT:3}"), "WM_GETTEXT and WM_GETTEXTLENGTH sent by SendMessageA/W to each kind; a buffer too short; GetWindowText sends WM_GETTEXT");

    /* 7: DefWindowProcA/W themselves */
    s_reset();
    {
        LRESULT r1 = DefWindowProcW(hA, WM_SETTEXT, 0, (LPARAM)L"F\u0416\u00E4");
        texts(hA);
        LRESULT r2 = DefWindowProcA(hW, WM_SETTEXT, 0, (LPARAM)"G\xE4\x80");
        texts(hW);
        wchar_t wb[16]; char nb[16];
        wmemset(wb, L'#', 16); LRESULT r3 = DefWindowProcW(hA, WM_GETTEXT, 16, (LPARAM)wb); s_add(" [%ld]", (long)r3); s_wt(wb);
        memset(nb, '#', 16); LRESULT r4 = DefWindowProcA(hW, WM_GETTEXT, 16, (LPARAM)nb); s_add(" [%ld]", (long)r4); s_nt(nb);
        long l1 = (long)DefWindowProcW(hA, WM_GETTEXTLENGTH, 0, 0);
        long l2 = (long)DefWindowProcA(hW, WM_GETTEXTLENGTH, 0, 0);
        s_add(" %ld %ld | %ld %ld", l1, l2, (long)r1, (long)r2);
        log_take();
    }
    check(7, seen(7,
        " [3]F?\\u00E4 [3]F?\\xE4 3 3 [3]G\\u00E4\\u20AC [3]G\\xE4\\x80 3 3 [3]F\\u0416\\u00E4 [3]G\\xE4\\x80 3 3 | 1 1 { A-GETTEXT:128 A-GETTEXT:64 A-GETTEXTLENGTH A-GE"
        "TTEXT:4 A-GETTEXTLENGTH W-GETTEXT:64 W-GETTEXT:64 W-GETTEXTLENGTH W-GETTEXTLENGTH W-GETTEXT:4}"), "DefWindowProcA/W keep and give a window's text in their own kind, whatever the window's");

    /* the child */
    g_parent = CreateWindowExA(0, "W51Parent", "w51 parent", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, 0, 0, inst, 0);
    char self[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(0, self, sizeof self);
    sprintf(cmd, "\"%s\" child %llu", self, (unsigned long long)(UINT_PTR)g_parent);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) { printf("no child\n"); return 98; }
    pump_until(&g_hello, 5000);
    log_take();

    /* 8: another process's Unicode window, read from here */
    s_reset();
    {
        s_add(" %d %d", g_hello, IsWindowUnicode(g_child));
        texts(g_child);
        wchar_t wb[64]; int a = GetClassNameW(g_child, wb, 64); s_add(" [%d]", a); s_wt(wb);
        s_add(" %d", FindWindowW(L"W51Child", L"child \u0416") == g_child);
        child_log();
    }
    check(8, seen(8,
        " 1 1 [7]child \\u0416 [7]child ? 7 7 [8]W51Child 1 {}7"), "another process's Unicode window: GetWindowTextA/W, its length, GetClassNameW, FindWindowW; GetWindowText sends it nothing");

    /* 9: SetWindowTextW and A on the other process's window */
    s_reset();
    {
        BOOL r1 = SetWindowTextW(g_child, L"renamed \u00E4\u0416");
        texts(g_child);
        child_log();
        BOOL r2 = SetWindowTextA(g_child, "ansi \xE4\x80");
        texts(g_child);
        child_log();
        s_add(" | %d %d %d", r1, r2, FindWindowW(NULL, L"ansi \u00E4\u20AC") == g_child);
    }
    check(9, seen(9,
        " [10]renamed \\u00E4\\u0416 [10]renamed \\xE4? 10 10 {}7 [7]ansi \\u00E4\\u20AC [7]ansi \\xE4\\x80 7 7 {}7 | 1 1 1"), "SetWindowTextW and SetWindowTextA on another process's window set its title without sending it WM_SETTEXT");

    /* 10: its procedure keeps the old one */
    s_reset();
    {
        SetLastError(0);
        BOOL r = SetWindowTextW(g_child, L"keep yours"); DWORD e = GetLastError();
        texts(g_child);
        child_log();
        s_add(" | %d %lu", r, e);
    }
    check(10, seen(10,
        " [10]keep yours [10]keep yours 10 10 {}7 | 1 0"), "another process's procedure is not asked: a title it would keep out is set");

    /* 11: while its thread takes no messages: GetWindowText does not wait, SendMessageTimeout does */
    s_reset();
    {
        PostMessageW(g_child, WM_STALL, 0, 0);
        Sleep(100);
        ULONGLONG t0 = GetTickCount64();
        wchar_t wb[64]; wb[0] = 0;
        int n = GetWindowTextW(g_child, wb, 64);
        ULONGLONG dt = GetTickCount64() - t0;
        s_add(" [%d]", n); s_wt(wb); s_add(" %s", dt < 500 ? "at once" : "waited");
        DWORD_PTR res = 99;
        SetLastError(0);
        t0 = GetTickCount64();
        LRESULT r = SendMessageTimeoutW(g_child, WM_GETTEXTLENGTH, 0, 0, SMTO_NORMAL, 200, &res);
        DWORD e = GetLastError();
        dt = GetTickCount64() - t0;
        s_add(" | %ld %lu %s", (long)r, (unsigned long)e, dt >= 150 && dt < 1000 ? "the time" : "not the time");
        Sleep(1600);
        r = SendMessageTimeoutW(g_child, WM_GETTEXTLENGTH, 0, 0, SMTO_NORMAL, 2000, &res);
        s_add(" | %ld %lu", (long)r, (unsigned long)res);
        child_log();
    }
    check(11, seen(11,
        " [10]keep yours at once | 0 1460 the time | 1 10 { W-GETTEXTLENGTH}7"), "while the other process takes no messages GetWindowText reads its title at once; SendMessageTimeout times out");

    /* 12: once the child has ended */
    s_reset();
    {
        PostMessageW(g_child, WM_BYE, 0, 0);
        DWORD w = WaitForSingleObject(pi.hProcess, 5000);
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        wchar_t wb[8]; wmemset(wb, L'#', 8);
        SetLastError(0);
        int n = GetWindowTextW(g_child, wb, 8); DWORD e = GetLastError();
        s_add(" %lu %lu [%d] %lu %d", (unsigned long)w, (unsigned long)code, n, (unsigned long)e, wb[0] == 0);
        SetLastError(0);
        BOOL r = SetWindowTextW(g_child, L"gone"); e = GetLastError();
        s_add(" %d %lu %d", r, (unsigned long)e, GetWindowTextLengthW(g_child));
    }
    check(12, seen(12,
        " 0 4 [0] 1400 1 1 1400 0"), "the child's window once its process has ended: GetWindowText 0 (ERROR_INVALID_WINDOW_HANDLE), its buffer emptied; SetWindowText TRUE, the error set");

    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DestroyWindow(hA); DestroyWindow(hW); DestroyWindow(hAW); DestroyWindow(hWA); DestroyWindow(g_parent);
    if (first_fail) { printf("FAILED (first: %d)\n", first_fail); return 100 + first_fail; }
    printf("ALL PASS\n");
    return 53;
}
