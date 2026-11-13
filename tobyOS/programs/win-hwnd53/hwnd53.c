/* win-hwnd53 -- Track C, W7v: as many windows as a program makes. A process
 * makes a thousand hidden windows and its child six hundred more: each is a
 * window (IsWindow, not visible until shown), every HWND its own and the
 * system's (FindWindow finds one by its title from either process,
 * GetWindowThreadProcessId names its thread, PostMessage and SendMessage
 * reach its procedure, EnumWindows lists them); destroyed, each is none, and
 * the next thousand are new numbers; a shown window is visible. Each check is
 * what Windows 11 does (measured first: this program exits 55 there); the
 * program runs itself as the child. Exit 55 iff all pass, else 100 + the
 * first check that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
static char g_s[2048];
static int g_k;
static void s_reset(void) { g_k = 0; g_s[0] = 0; }
static void s_add(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_s + g_k, sizeof g_s - (size_t)g_k, fmt, ap);
    va_end(ap);
    if (n > 0 && g_k + n < (int)sizeof g_s) g_k += n;
}
static int seen(int n, const char *want) {
    printf("     %d saw:%s\n", n, g_s);
    if (strcmp(g_s, want) != 0) { printf("     %d want:%s\n", n, want); return 0; }
    return 1;
}

#define N   1000
#define NC  600
#define WM_HELLO (WM_APP + 1)     /* child -> parent: wParam its last window, lParam its count */
#define WM_PROBE (WM_APP + 2)     /* to a window: its procedure answers the window's index */
#define WM_ECHO  (WM_APP + 3)     /* child -> parent: wParam the index the child's window had */
#define WM_BYE   (WM_APP + 4)

static HWND g_h[N], g_old[N];
static int g_destroyed, g_probe_hits;
static HWND g_probe_hwnd;
static LRESULT CALLBACK many_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PROBE: {
        g_probe_hits++;
        g_probe_hwnd = h;
        return (LRESULT)GetWindowLongPtrA(h, GWLP_USERDATA);
    }
    case WM_DESTROY: g_destroyed++; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ---- the child ---- */
static HWND g_parent;
static HWND g_c[NC];
static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PROBE:
        PostMessageA(g_parent, WM_ECHO, (WPARAM)GetWindowLongPtrA(h, GWLP_USERDATA), (LPARAM)h);
        return 0;
    case WM_BYE: PostQuitMessage(5); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static int child(HWND parent) {
    g_parent = parent;
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = child_proc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "W53Child";
    if (!RegisterClassA(&wc)) return 20;
    int made = 0;
    for (int i = 0; i < NC; i++) {
        char t[32]; sprintf(t, "w53c #%d", i);
        g_c[i] = CreateWindowExA(0, "W53Child", t, WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, wc.hInstance, 0);
        if (g_c[i]) { SetWindowLongPtrA(g_c[i], GWLP_USERDATA, i); made++; }
    }
    PostMessageA(parent, WM_HELLO, (WPARAM)g_c[NC - 1], made);
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) DispatchMessageA(&msg);
    return (int)msg.wParam;
}

/* ---- the parent's own window ---- */
static HWND g_child_last;
static int g_child_made = -1, g_echo = -1;
static HWND g_echo_hwnd;
static LRESULT CALLBACK parent_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HELLO: g_child_last = (HWND)w; g_child_made = (int)l; return 0;
    case WM_ECHO: g_echo = (int)w; g_echo_hwnd = (HWND)l; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static void pump_until(volatile int *v, int not_val, DWORD ms) {
    DWORD t0 = GetTickCount();
    MSG msg;
    while (*v == not_val && GetTickCount() - t0 < ms) {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (*v == not_val) Sleep(5);
    }
}
static int g_enum_mine, g_enum_child;
static BOOL CALLBACK enum_proc(HWND h, LPARAM l) {
    (void)l;
    char c[64];
    if (GetClassNameA(h, c, sizeof c)) {
        if (!strcmp(c, "W53Many")) g_enum_mine++;
        if (!strcmp(c, "W53Child")) g_enum_child++;
    }
    return TRUE;
}
static unsigned long long parse_u64(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}
static int all_distinct(HWND *h, int n) {
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (h[i] == h[j]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) return child((HWND)(UINT_PTR)parse_u64(argv[2]));
    HINSTANCE inst = GetModuleHandleA(0);
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = many_proc; wc.hInstance = inst; wc.lpszClassName = "W53Many";
    RegisterClassA(&wc);
    wc.lpfnWndProc = parent_proc; wc.lpszClassName = "W53Parent";
    RegisterClassA(&wc);

    /* 1: a thousand hidden windows */
    s_reset();
    {
        int made = 0, isw = 0, vis = 0, nonnull = 0;
        for (int i = 0; i < N; i++) {
            char t[32]; sprintf(t, "w53 #%d", i);
            g_h[i] = CreateWindowExA(0, "W53Many", t, WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, inst, 0);
            if (g_h[i]) { made++; SetWindowLongPtrA(g_h[i], GWLP_USERDATA, i); }
        }
        for (int i = 0; i < N; i++) {
            nonnull += g_h[i] != 0;
            isw += IsWindow(g_h[i]) != 0;
            vis += IsWindowVisible(g_h[i]) != 0;
        }
        s_add(" %d %d %d %d %d", made, nonnull, isw, vis, all_distinct(g_h, N));
    }
    check(1, seen(1,
        " 1000 1000 1000 0 1"), "a thousand hidden windows: each made, each its own HWND, IsWindow, none visible");

    /* 2: found by title, read, its thread; messages reach it */
    s_reset();
    {
        HWND f = FindWindowA("W53Many", "w53 #999"), f2 = FindWindowA(NULL, "w53 #0");
        char t[32] = ""; GetWindowTextA(g_h[500], t, sizeof t);
        DWORD pid = 0, tid = GetWindowThreadProcessId(g_h[777], &pid);
        LRESULT r = SendMessageA(g_h[321], WM_PROBE, 0, 0);
        int hits = g_probe_hits;
        PostMessageA(g_h[999], WM_PROBE, 0, 0);
        MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        s_add(" %d %d %s %d %d %ld %d %d %d", f == g_h[999], f2 == g_h[0], t, tid == GetCurrentThreadId(), pid == GetCurrentProcessId(),
              (long)r, hits, g_probe_hits, g_probe_hwnd == g_h[999]);
    }
    check(2, seen(2,
        " 1 1 w53 #500 1 1 321 1 2 1"), "FindWindow finds one by its title, GetWindowText reads it, GetWindowThreadProcessId names this thread; SendMessage and PostMessage reach it");

    /* 3: EnumWindows lists them */
    s_reset();
    {
        g_enum_mine = g_enum_child = 0;
        EnumWindows(enum_proc, 0);
        s_add(" %d", g_enum_mine);
    }
    check(3, seen(3,
        " 1000"), "EnumWindows lists the thousand");

    /* 4: a child process's six hundred, from here */
    HWND parent = CreateWindowExA(0, "W53Parent", "w53 parent", WS_OVERLAPPED, 0, 0, 100, 60, 0, 0, inst, 0);
    char self[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(0, self, sizeof self);
    sprintf(cmd, "\"%s\" child %llu", self, (unsigned long long)(UINT_PTR)parent);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) { printf("no child\n"); return 98; }
    pump_until(&g_child_made, -1, 20000);
    s_reset();
    {
        char t[32] = ""; GetWindowTextA(g_child_last, t, sizeof t);
        HWND f = FindWindowA("W53Child", "w53c #599");
        DWORD pid = 0; GetWindowThreadProcessId(g_child_last, &pid);
        PostMessageA(g_child_last, WM_PROBE, 0, 0);
        pump_until(&g_echo, -1, 5000);
        g_enum_mine = g_enum_child = 0;
        EnumWindows(enum_proc, 0);
        s_add(" %d %d %s %d %d %d %d %d %d", g_child_made, IsWindow(g_child_last) != 0, t, f == g_child_last, pid == pi.dwProcessId,
              g_echo, g_echo_hwnd == g_child_last, g_enum_child, g_enum_mine);
    }
    check(4, seen(4,
        " 600 1 w53c #599 1 1 599 1 600 1000"), "a child's six hundred more: IsWindow, GetWindowText, FindWindow, GetWindowThreadProcessId, PostMessage from here; EnumWindows lists both processes'");

    /* 5: the child gone, its windows are none */
    s_reset();
    {
        PostMessageA(g_child_last, WM_BYE, 0, 0);
        DWORD w = WaitForSingleObject(pi.hProcess, 10000);
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        s_add(" %lu %lu %d %d", (unsigned long)w, (unsigned long)code, IsWindow(g_child_last) != 0,
              FindWindowA("W53Child", NULL) != 0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    check(5, seen(5,
        " 0 5 0 0"), "the child ended: its windows are none");

    /* 6: destroyed, each is none; the next thousand are new numbers */
    s_reset();
    {
        g_destroyed = 0;
        int ok = 0;
        for (int i = 0; i < N; i++) { g_old[i] = g_h[i]; ok += DestroyWindow(g_h[i]) != 0; }
        int still = 0;
        for (int i = 0; i < N; i++) still += IsWindow(g_old[i]) != 0;
        s_add(" %d %d %d %d", ok, g_destroyed, still, FindWindowA("W53Many", NULL) != 0);
        int made = 0, same = 0;
        for (int i = 0; i < N; i++) {
            g_h[i] = CreateWindowExA(0, "W53Many", "again", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, inst, 0);
            made += g_h[i] != 0;
        }
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) if (g_h[i] == g_old[j]) { same++; break; }
        s_add(" | %d %d %d", made, same, all_distinct(g_h, N));
        for (int i = 0; i < N; i++) DestroyWindow(g_h[i]);
    }
    check(6, seen(6,
        " 1000 1000 0 0 | 1000 0 1"), "DestroyWindow: WM_DESTROY for each, each none; a thousand more made, none an old number");

    /* 7: shown */
    s_reset();
    {
        HWND a = CreateWindowExA(0, "W53Many", "w53 shown", WS_OVERLAPPEDWINDOW, 10, 10, 120, 80, 0, 0, inst, 0);
        int v1 = IsWindowVisible(a) != 0;
        BOOL was = ShowWindow(a, SW_SHOW);
        int v2 = IsWindowVisible(a) != 0;
        HWND b = CreateWindowExA(0, "W53Many", "w53 visible", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 140, 10, 120, 80, 0, 0, inst, 0);
        int v3 = IsWindowVisible(b) != 0;
        BOOL was2 = ShowWindow(b, SW_HIDE);
        int v4 = IsWindowVisible(b) != 0;
        s_add(" %d %d %d %d %d %d", v1, was != 0, v2, v3, was2 != 0, v4);
        DestroyWindow(a); DestroyWindow(b); DestroyWindow(parent);
        MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    }
    check(7, seen(7,
        " 0 0 1 1 1 0"), "a window is visible once shown (ShowWindow's answer: was it), WS_VISIBLE at once; hidden again by SW_HIDE");

    if (first_fail) { printf("FAILED (first: %d)\n", first_fail); return 100 + first_fail; }
    printf("ALL PASS\n");
    return 55;
}
