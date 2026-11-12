/* win-gui45 -- Track C, W7n: what a process keeps of its own. Its window
 * classes, windows, menus, bitmaps, timers, TLS indexes and environment are
 * its own: another program registering a class of the same name, making and
 * destroying windows, starting its own menus and timers, changing its
 * environment, leaves this one's as they were; two such programs at once do
 * too. A child's environment is its parent's (or the block CreateProcess was
 * given), and a change in it stays the child's. A class registered before
 * another, with no window yet, is still there; a class nobody registered
 * makes no window. Each check is what Windows 11 does (measured first: this
 * program exits 47 there). Exit 47 iff all pass, else 100 + the first check
 * that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}

/* ---- the window procedures ---- */
static int g_parent_user, g_parent_timer;
static LRESULT CALLBACK parent_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER) { g_parent_user++; return 0x4E5; }
    if (m == WM_TIMER && w == 7) { g_parent_timer++; return 0; }
    return DefWindowProcA(h, m, w, l);
}
static int g_child_timer;
static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER) return 0xC;
    if (m == WM_TIMER) { g_child_timer++; return 0; }
    return DefWindowProcA(h, m, w, l);
}
static LRESULT CALLBACK k1_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER) return 1;
    return DefWindowProcA(h, m, w, l);
}
static LRESULT CALLBACK k2_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER) return 2;
    return DefWindowProcA(h, m, w, l);
}
static ATOM reg(const char *name, WNDPROC p) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = p;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = name;
    return RegisterClassA(&wc);
}
static HWND make(const char *cls, const char *title) {
    return CreateWindowExA(0, cls, title, WS_OVERLAPPEDWINDOW, 10, 10, 200, 120, 0, 0, GetModuleHandleA(0), 0);
}
/* the messages for `ms` milliseconds, dispatched */
static void pump(DWORD ms) {
    DWORD t0 = GetTickCount();
    MSG msg;
    while (GetTickCount() - t0 < ms) {
        if (!GetMessageA(&msg, 0, 0, 0)) break;
        DispatchMessageA(&msg);
    }
}

static char g_self[MAX_PATH];
/* start this program as a child with `arg` (and env, if not NULL), its
 * stdout a pipe of ours; the pipe's read end in *rd */
static HANDLE start(const char *arg, const char *env, HANDLE *rd) {
    char cmd[MAX_PATH + 40];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", g_self, arg);
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return 0;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(g_self, cmd, 0, 0, TRUE, 0, (LPVOID)env, 0, &si, &pi)) {
        CloseHandle(r); CloseHandle(w);
        return 0;
    }
    CloseHandle(w);
    CloseHandle(pi.hThread);
    *rd = r;
    return pi.hProcess;
}
static DWORD finish(HANDLE hp, HANDLE r, char *out, int cap) {
    int n = 0;
    DWORD got;
    while (n < cap - 1 && ReadFile(r, out + n, (DWORD)(cap - 1 - n), &got, 0) && got) n += (int)got;
    out[n] = 0;
    CloseHandle(r);
    WaitForSingleObject(hp, 30000);
    DWORD ec = 0xBEEF;
    GetExitCodeProcess(hp, &ec);
    CloseHandle(hp);
    return ec;
}
static DWORD run(const char *arg, const char *env, char *out, int cap) {
    HANDLE r;
    HANDLE hp = start(arg, env, &r);
    if (!hp) { out[0] = 0; return 0xDEAD; }
    return finish(hp, r, out, cap);
}

/* ---- the children ---- */
static int child(const char *m) {
    char b[200];
    if (!strcmp(m, "gui")) {                  /* the class's name, windows, a menu, a timer, its env */
        if (!reg("W7nClass", child_proc)) return 30;
        HWND c1 = make("W7nClass", "child 1"), c2 = make("W7nClass", "child 2");
        if (!c1 || !c2) return 31;
        LRESULT r = SendMessageA(c1, WM_USER, 0, 0);
        char v[32] = "";
        GetEnvironmentVariableA("W7N", v, sizeof v);
        SetEnvironmentVariableA("W7N", "child");
        DWORD idx = TlsAlloc();
        TlsSetValue(idx, (LPVOID)0x99);
        HMENU mn = CreateMenu();
        AppendMenuA(mn, MF_STRING, 1, "Only");
        SetMenu(c1, mn);
        SetTimer(c1, 7, 10, 0);
        pump(150);
        KillTimer(c1, 7);
        DestroyWindow(c1);
        DestroyWindow(c2);
        snprintf(b, sizeof b, "env=%s timers=%s\n", v, g_child_timer > 0 ? "yes" : "no");
        say(b);
        return r == 0xC ? 21 : 22;
    }
    if (!strcmp(m, "spin")) {                 /* two at once: each its class and windows */
        if (!reg("W7nClass", child_proc)) return 30;
        HWND c = make("W7nClass", "spin");
        if (!c) return 31;
        SetTimer(c, 3, 10, 0);
        pump(300);
        LRESULT r = SendMessageA(c, WM_USER, 0, 0);
        DestroyWindow(c);
        return r == 0xC && g_child_timer > 0 ? 24 : 25;
    }
    if (!strcmp(m, "env")) {                  /* what the environment is */
        char v[32] = "", x[32] = "";
        DWORD nv = GetEnvironmentVariableA("W7N", v, sizeof v);
        DWORD nx = GetEnvironmentVariableA("X", x, sizeof x);
        snprintf(b, sizeof b, "W7N=%s(%lu) X=%s(%lu)\n", v, (unsigned long)nv, x, (unsigned long)nx);
        say(b);
        return 23;
    }
    if (!strcmp(m, "classes")) {              /* two classes before a window; one nobody registered */
        if (!reg("K1", k1_proc) || !reg("K2", k2_proc)) return 30;
        HWND w1 = make("K1", "k1"), w2 = make("K2", "k2");
        SetLastError(0);
        HWND w3 = make("NoSuchClass", "none");
        DWORD e3 = GetLastError();
        LRESULT r1 = w1 ? SendMessageA(w1, WM_USER, 0, 0) : -1, r2 = w2 ? SendMessageA(w2, WM_USER, 0, 0) : -1;
        snprintf(b, sizeof b, "k1=%ld k2=%ld none=%d err=%lu\n", (long)r1, (long)r2, w3 != 0, (unsigned long)e3);
        say(b);
        return 26;
    }
    return 1;
}

int main(int argc, char **argv) {
    GetModuleFileNameA(0, g_self, sizeof g_self);
    if (argc > 1) return child(argv[1]);
    char out[400], out2[400];

    /* 1: this process's own -- a class, a window, a menu, a bitmap, a TLS
     *    index, an environment variable */
    int ok1 = reg("W7nClass", parent_proc) != 0;
    HWND a = make("W7nClass", "parent");
    LRESULT r = a ? SendMessageA(a, WM_USER, 0, 0) : 0;
    SetEnvironmentVariableA("W7N", "parent");
    DWORD idx = TlsAlloc();
    TlsSetValue(idx, (LPVOID)0x1234);
    HMENU menu = CreateMenu();
    AppendMenuA(menu, MF_STRING, 1, "One");
    AppendMenuA(menu, MF_STRING, 2, "Two");
    AppendMenuA(menu, MF_STRING, 3, "Three");
    SetMenu(a, menu);
    HBITMAP bmp = CreateBitmap(17, 9, 1, 32, 0);
    BITMAP bm;
    memset(&bm, 0, sizeof bm);
    int gob = GetObjectA(bmp, sizeof bm, &bm);
    printf("     window %d, its procedure %lx, TLS index %lu, the menu's items %d, the bitmap %ldx%ld (%d)\n",
           a != 0, (unsigned long)r, (unsigned long)idx, GetMenuItemCount(menu), (long)bm.bmWidth, (long)bm.bmHeight, gob);
    check(1, ok1 && a && r == 0x4E5 && idx != TLS_OUT_OF_INDEXES && GetMenuItemCount(menu) == 3 &&
             gob == (int)sizeof bm && bm.bmWidth == 17 && bm.bmHeight == 9,
          "the parent's class and window, a menu of 3, a 17x9 bitmap, a TLS index, W7N=parent");

    /* 2: a child with a class of the same name, two windows, a menu, a timer,
     *    a changed environment and a TLS index -- and then the parent's own */
    DWORD rc = run("gui", 0, out, sizeof out);
    int before = g_parent_user;
    LRESULT r2 = SendMessageA(a, WM_USER, 0, 0);
    char v[32] = "";
    GetEnvironmentVariableA("W7N", v, sizeof v);
    DWORD idx2 = TlsAlloc();
    LPVOID tv = TlsGetValue(idx);
    memset(&bm, 0, sizeof bm);
    GetObjectA(bmp, sizeof bm, &bm);
    printf("     child: %lu \"%.*s\"; then: procedure %lx (calls %d->%d), window %d, menu %d of %d items, W7N=%s, TLS %p, next index %lu, bitmap %ldx%ld\n",
           (unsigned long)rc, (int)strcspn(out, "\r\n"), out, (unsigned long)r2, before, g_parent_user, IsWindow(a) != 0,
           GetMenu(a) == menu, GetMenuItemCount(menu), v, tv, (unsigned long)idx2, (long)bm.bmWidth, (long)bm.bmHeight);
    check(2, rc == 21 && !strncmp(out, "env=parent timers=yes", 21) && r2 == 0x4E5 && g_parent_user == before + 1 &&
             IsWindow(a) && GetMenu(a) == menu && GetMenuItemCount(menu) == 3 && !strcmp(v, "parent") &&
             tv == (LPVOID)0x1234 && idx2 != idx && idx2 != TLS_OUT_OF_INDEXES && bm.bmWidth == 17,
          "after a child's class of the same name, its windows, menu, timer, environment and TLS: the parent's are as they were");

    /* 3: the parent's timer goes on while a child starts its windows */
    HANDLE pr;
    HANDLE hp = start("gui", 0, &pr);
    g_parent_timer = 0;
    SetTimer(a, 7, 20, 0);
    pump(400);
    rc = finish(hp, pr, out, sizeof out);
    int t1 = g_parent_timer;
    pump(200);
    KillTimer(a, 7);
    printf("     the parent's WM_TIMERs: %d while the child ran (it exited %lu), %d in all\n", t1, (unsigned long)rc, g_parent_timer);
    check(3, rc == 21 && t1 > 0 && g_parent_timer > t1,
          "the parent's timer fires while a child registers its class and makes windows, and after");

    /* 4: two children at once, each with its class and a window */
    HANDLE r1, r2h;
    HANDLE h1 = start("spin", 0, &r1), h2 = start("spin", 0, &r2h);
    DWORD c1 = finish(h1, r1, out, sizeof out), c2 = finish(h2, r2h, out2, sizeof out2);
    LRESULT r4 = SendMessageA(a, WM_USER, 0, 0);
    printf("     two at once: %lu %lu; the parent's procedure %lx\n", (unsigned long)c1, (unsigned long)c2, (unsigned long)r4);
    check(4, c1 == 24 && c2 == 24 && r4 == 0x4E5 && IsWindow(a),
          "two children at once, each with its own class, window and timer; the parent's window as it was");

    /* 5: a child's environment: the parent's, or the block it was given */
    rc = run("env", 0, out, sizeof out);
    static const char block[] = "W7N=given\0X=1\0";
    DWORD rcb = run("env", block, out2, sizeof out2);
    printf("     inherited: %lu \"%.*s\"; given: %lu \"%.*s\"\n", (unsigned long)rc, (int)strcspn(out, "\r\n"), out,
           (unsigned long)rcb, (int)strcspn(out2, "\r\n"), out2);
    check(5, rc == 23 && !strncmp(out, "W7N=parent(6) X=(0)", 19) && rcb == 23 && !strncmp(out2, "W7N=given(5) X=1(1)", 19),
          "a child's environment is the parent's, or the block CreateProcess was given");

    /* 6: two classes registered before any window; a class nobody registered */
    rc = run("classes", 0, out, sizeof out);
    printf("     %lu \"%.*s\"\n", (unsigned long)rc, (int)strcspn(out, "\r\n"), out);
    check(6, rc == 26 && !strncmp(out, "k1=1 k2=2 none=0 err=1407", 25),
          "two classes before a window keep their procedures; CreateWindowEx of no class: NULL, ERROR_CANNOT_FIND_WND_CLASS");

    DestroyWindow(a);
    printf(first_fail ? "SOME FAILED (first %d)\n" : "ALL PASS\n", first_fail);
    return first_fail ? 100 + first_fail : 47;
}
