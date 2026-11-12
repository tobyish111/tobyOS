/* win-post46 -- Track C, W7o: posted messages. PostMessage puts a message on
 * the queue of the thread that made the window (PostThreadMessage on a
 * thread's own, with no window); GetMessage takes them in order, before
 * input, paint and timers, and takes only what its window and its range of
 * message numbers ask for -- waiting when there is nothing (a message
 * posted by another thread 100 ms later wakes it); PeekMessage looks without
 * waiting, and takes only with PM_REMOVE; WM_QUIT comes when the posted
 * messages are gone. Each check is what Windows 11 does (measured first:
 * this program exits 48 there). Exit 48 iff all pass, else 100 + the first
 * check that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

static char g_log[512];
static void logm(const char *tag, UINT m) {
    char b[24];
    snprintf(b, sizeof b, "%s%x ", tag, m);
    strcat(g_log, b);
}
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m >= WM_USER && m < 0xC000) { logm("d", m); return (LRESULT)m; }
    return DefWindowProcA(h, m, w, l);
}
static HWND make(const char *title) {
    return CreateWindowExA(0, "W7oClass", title, WS_OVERLAPPEDWINDOW, 10, 10, 160, 100, 0, 0, GetModuleHandleA(0), 0);
}
/* take every message there is now, without waiting: a PeekMessage loop */
static int drain(HWND h, UINT lo, UINT hi) {
    MSG msg;
    int n = 0;
    while (PeekMessageA(&msg, h, lo, hi, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { logm("q", msg.message); n++; continue; }
        if (msg.message >= WM_USER && msg.message < 0xC000) { logm("g", msg.message); n++; }
        DispatchMessageA(&msg);
    }
    return n;
}

/* ---- another thread ---- */
static HWND g_twin;
static DWORD g_main_tid;
static HANDLE g_ready, g_go, g_done;
static char g_tlog[256];
static volatile DWORD g_posted_at;                 /* GetTickCount just before the post */
static DWORD WINAPI late_poster(LPVOID p) {
    Sleep(150);
    g_posted_at = GetTickCount();
    PostMessageA((HWND)p, WM_USER + 9, 9, 99);
    return 0;
}
static DWORD WINAPI late_thread_poster(LPVOID p) {
    (void)p;
    Sleep(150);
    g_posted_at = GetTickCount();
    PostThreadMessageA(g_main_tid, WM_APP + 9, 0, 0);
    return 0;
}
static DWORD WINAPI owner(LPVOID p) {               /* 6: a thread with a window of its own */
    (void)p;
    g_twin = make("thread's");
    MSG msg;
    PeekMessageA(&msg, 0, 0, 0, PM_NOREMOVE);       /* (the thread's queue made) */
    SetEvent(g_ready);
    WaitForSingleObject(g_go, 5000);
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        if (msg.message >= WM_USER && msg.message < 0xC000) {
            char b[40];
            snprintf(b, sizeof b, "%s%x ", msg.hwnd == g_twin ? "w" : msg.hwnd ? "?" : "t", msg.message);
            strcat(g_tlog, b);
        }
        DispatchMessageA(&msg);
    }
    DestroyWindow(g_twin);
    SetEvent(g_done);
    return 0;
}

int main(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "W7oClass";
    RegisterClassA(&wc);
    HWND a = make("A"), b = make("B");
    MSG msg;
    drain(0, 0, 0);                                  /* what CreateWindowEx left (none of ours) */
    g_log[0] = 0;

    /* 1: in order, before a window's paint and timer, through DispatchMessage */
    BOOL p1 = PostMessageA(a, WM_USER + 1, 11, 111);
    BOOL p2 = PostMessageA(b, WM_USER + 2, 22, 222);
    BOOL p3 = PostMessageA(a, WM_USER + 3, 33, 333);
    InvalidateRect(a, 0, TRUE);
    SetTimer(a, 5, 1, 0);
    Sleep(20);
    int got = 0, other = 0;
    WPARAM w1 = 0;
    LPARAM l1 = 0;
    while (got < 3 && GetMessageA(&msg, 0, 0, 0)) {
        if (msg.message >= WM_USER && msg.message < 0xC000) {
            if (!got) { w1 = msg.wParam; l1 = msg.lParam; }
            got++;
        } else if (!got || got < 3) other++;
        LRESULT r = DispatchMessageA(&msg);
        (void)r;
    }
    KillTimer(a, 5);
    printf("     posted %d%d%d, then [%s] (other messages before them: %d), the first's wParam %lu lParam %ld\n",
           p1, p2, p3, g_log, other, (unsigned long)w1, (long)l1);
    check(1, p1 && p2 && p3 && !strcmp(g_log, "d401 d402 d403 ") && other == 0 && w1 == 11 && l1 == 111,
          "posted messages come in their order, before the window's WM_PAINT and WM_TIMER, and DispatchMessage gives them to it");
    drain(0, 0, 0);
    g_log[0] = 0;

    /* 2: GetMessage's filters -- a window, a range of messages */
    PostMessageA(a, WM_USER + 1, 0, 0);
    PostMessageA(b, WM_USER + 2, 0, 0);
    PostMessageA(a, WM_APP + 1, 0, 0);
    PostMessageA(b, WM_APP + 2, 0, 0);
    BOOL g1 = GetMessageA(&msg, b, 0, 0);
    UINT m1 = msg.message; HWND h1 = msg.hwnd;
    BOOL g2 = GetMessageA(&msg, 0, WM_APP, WM_APP + 0x10);
    UINT m2 = msg.message;
    BOOL g3 = GetMessageA(&msg, b, WM_APP, WM_APP + 0x10);
    UINT m3 = msg.message;
    BOOL g4 = GetMessageA(&msg, 0, WM_USER, WM_USER + 5);
    UINT m4 = msg.message;
    printf("     GetMessage(B) %x, (0, WM_APP..) %x, (B, WM_APP..) %x, (0, WM_USER..) %x\n", m1, m2, m3, m4);
    check(2, g1 && m1 == WM_USER + 2 && h1 == b && g2 && m2 == WM_APP + 1 && g3 && m3 == WM_APP + 2 && g4 && m4 == WM_USER + 1,
          "GetMessage takes only its window's, and only its range of messages, the first of those in order");
    drain(0, 0, 0);
    g_log[0] = 0;

    /* 3: PeekMessage: PM_NOREMOVE leaves it, PM_REMOVE takes it, nothing: FALSE
     *    at once; hWnd -1 is the thread's own (posted with no window) */
    BOOL e0 = PeekMessageA(&msg, 0, WM_USER, 0x7FFF, PM_REMOVE);
    PostMessageA(a, WM_USER + 4, 0, 0);
    PostMessageA(0, WM_USER + 5, 0, 0);
    PostThreadMessageA(GetCurrentThreadId(), WM_USER + 6, 0, 0);
    BOOL k1 = PeekMessageA(&msg, 0, 0, 0, PM_NOREMOVE);
    UINT km1 = msg.message;
    BOOL k2 = PeekMessageA(&msg, 0, 0, 0, PM_NOREMOVE);
    UINT km2 = msg.message;
    BOOL k3 = PeekMessageA(&msg, (HWND)-1, 0, 0, PM_REMOVE);
    UINT km3 = msg.message; HWND kh3 = msg.hwnd;
    BOOL k4 = PeekMessageA(&msg, (HWND)-1, 0, 0, PM_REMOVE);
    UINT km4 = msg.message;
    BOOL k5 = PeekMessageA(&msg, (HWND)-1, 0, 0, PM_REMOVE);
    BOOL k6 = PeekMessageA(&msg, a, WM_USER, WM_USER + 10, PM_REMOVE);
    UINT km6 = msg.message;
    BOOL k7 = PeekMessageA(&msg, a, WM_USER, WM_USER + 10, PM_REMOVE);
    printf("     empty %d; peek %x %x; -1: %x (hwnd %p) %x, then %d; A: %x, then %d\n",
           e0, km1, km2, km3, (void *)kh3, km4, k5, km6, k7);
    check(3, !e0 && k1 && k2 && km1 == WM_USER + 4 && km2 == WM_USER + 4 && k3 && km3 == WM_USER + 5 && !kh3 &&
             k4 && km4 == WM_USER + 6 && !k5 && k6 && km6 == WM_USER + 4 && !k7,
          "PeekMessage: FALSE with nothing, PM_NOREMOVE leaves the message, PM_REMOVE takes it; hWnd -1 the thread's own messages");
    drain(0, 0, 0);

    /* 4: WM_QUIT comes after the posted messages; a range that leaves it out
     *    still gets it; it is taken once */
    PostMessageA(a, WM_USER + 7, 0, 0);
    PostQuitMessage(7);
    PostMessageA(a, WM_USER + 8, 0, 0);
    BOOL q1 = GetMessageA(&msg, 0, 0, 0); UINT qm1 = msg.message;
    BOOL q2 = GetMessageA(&msg, 0, WM_USER + 8, WM_USER + 8); UINT qm2 = msg.message;
    BOOL q3 = GetMessageA(&msg, 0, WM_USER + 100, WM_USER + 100); UINT qm3 = msg.message; WPARAM qw3 = msg.wParam;
    BOOL q4 = PeekMessageA(&msg, 0, 0, 0, PM_REMOVE);
    printf("     %d %x, %d %x, %d %x (exit code %lu), then peek %d\n", q1, qm1, q2, qm2, q3, qm3, (unsigned long)qw3, q4);
    check(4, q1 && qm1 == WM_USER + 7 && q2 && qm2 == WM_USER + 8 && !q3 && qm3 == WM_QUIT && qw3 == 7 && !q4,
          "WM_QUIT after the posted messages, whatever the range asked; GetMessage FALSE for it, and it is taken once");

    /* 5: GetMessage waits: a message another thread posts after a 150 ms
     *    Sleep wakes it -- not before the post (the poster's GetTickCount
     *    just before it, on the same clock as ours) */
    g_posted_at = 0;
    DWORD t0 = GetTickCount();
    HANDLE th = CreateThread(0, 0, late_poster, a, 0, 0);
    BOOL w5 = GetMessageA(&msg, 0, WM_USER + 9, WM_USER + 9);
    DWORD tr = GetTickCount(), at = g_posted_at;
    UINT wm5 = msg.message; WPARAM ww5 = msg.wParam;
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    g_main_tid = GetCurrentThreadId();
    g_posted_at = 0;
    DWORD t1 = GetTickCount();
    th = CreateThread(0, 0, late_thread_poster, 0, 0, 0);
    BOOL w6 = GetMessageA(&msg, 0, 0, 0);
    DWORD tr2 = GetTickCount(), at2 = g_posted_at;
    UINT wm6 = msg.message; HWND wh6 = msg.hwnd;
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    printf("     posted at %lu ms, GetMessage back at %lu ms with %x (wParam %lu); posted at %lu ms, back at %lu ms with %x (hwnd %p)\n",
           (unsigned long)(at - t0), (unsigned long)(tr - t0), wm5, (unsigned long)ww5,
           (unsigned long)(at2 - t1), (unsigned long)(tr2 - t1), wm6, (void *)wh6);
    check(5, w5 && wm5 == WM_USER + 9 && ww5 == 9 && at && at - t0 >= 50 && tr >= at &&
             w6 && wm6 == WM_APP + 9 && !wh6 && at2 && at2 - t1 >= 50 && tr2 >= at2,
          "GetMessage waits for a message another thread posts after a Sleep: to the window, and to the thread");

    /* 6: a window's messages go to the thread that made it */
    g_ready = CreateEventA(0, TRUE, FALSE, 0);
    g_go = CreateEventA(0, TRUE, FALSE, 0);
    g_done = CreateEventA(0, TRUE, FALSE, 0);
    DWORD ttid = 0;
    th = CreateThread(0, 0, owner, 0, 0, &ttid);
    WaitForSingleObject(g_ready, 5000);
    BOOL x1 = PostMessageA(g_twin, WM_USER + 0x10, 0, 0);
    BOOL x2 = PostThreadMessageA(ttid, WM_USER + 0x11, 0, 0);
    BOOL x3 = PeekMessageA(&msg, 0, WM_USER, 0x7FFF, PM_NOREMOVE);
    SetEvent(g_go);
    WaitForSingleObject(g_done, 5000);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    printf("     posted %d %d; seen here %d; the thread's [%s]\n", x1, x2, x3, g_tlog);
    check(6, x1 && x2 && !x3 && !strcmp(g_tlog, "w410 t411 "),
          "a window's posted message and a thread message go to the thread that made the window, not to this one");

    /* 7: the errors */
    SetLastError(0);
    BOOL e1 = PostMessageA((HWND)(INT_PTR)0x7654321, WM_USER, 0, 0);
    DWORD ee1 = GetLastError();
    SetLastError(0);
    BOOL e2 = PostThreadMessageA(0x7FFFFFF0, WM_USER, 0, 0);
    DWORD ee2 = GetLastError();
    printf("     PostMessage(no window) %d %lu, PostThreadMessage(no thread) %d %lu\n", e1, (unsigned long)ee1, e2, (unsigned long)ee2);
    check(7, !e1 && ee1 == ERROR_INVALID_WINDOW_HANDLE && !e2 && ee2 == ERROR_INVALID_THREAD_ID,
          "PostMessage to no window: ERROR_INVALID_WINDOW_HANDLE; PostThreadMessage to no thread: ERROR_INVALID_THREAD_ID");

    /* 8: the wide forms do the same with these messages */
    PostMessageW(a, WM_USER + 0x20, 1, 2);
    PostThreadMessageW(GetCurrentThreadId(), WM_USER + 0x21, 3, 4);
    BOOL v1 = PeekMessageW(&msg, a, 0, 0, PM_REMOVE); UINT vm1 = msg.message;
    BOOL v2 = GetMessageW(&msg, 0, 0, 0); UINT vm2 = msg.message; WPARAM vw2 = msg.wParam;
    printf("     PeekMessageW %x, GetMessageW %x (wParam %lu)\n", vm1, vm2, (unsigned long)vw2);
    check(8, v1 && vm1 == WM_USER + 0x20 && v2 && vm2 == WM_USER + 0x21 && vw2 == 3,
          "PostMessageW, PostThreadMessageW, PeekMessageW and GetMessageW");

    DestroyWindow(a);
    DestroyWindow(b);
    printf(first_fail ? "SOME FAILED (first %d)\n" : "ALL PASS\n", first_fail);
    return first_fail ? 100 + first_fail : 48;
}
