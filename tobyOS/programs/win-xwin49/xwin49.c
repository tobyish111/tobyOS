/* win-xwin49 -- Track C, W7r: windows named across processes and threads.
 * An HWND is the system's: the number one process has for its window is the
 * one another process uses to find it, post to it and send to it. FindWindow
 * finds another process's window by class and title, EnumWindows lists it,
 * GetWindowThreadProcessId names its thread and process, GetWindowText reads
 * its title. PostMessage puts a message on the queue of the window's thread
 * in its own process, PostThreadMessage on another process's thread.
 * SendMessage to a window of another thread or process runs the window's
 * procedure on that thread, while it waits for a message, and waits for the
 * answer -- handling what is sent to it meanwhile; a message sent comes before
 * the posted ones; WM_COPYDATA, WM_GETTEXT and WM_SETTEXT carry their data
 * across; SendMessageTimeout gives up (ERROR_TIMEOUT) on a thread that does
 * not take it. A window whose process ended is no window. Each check is what
 * Windows 11 does (measured first: this program exits 51 there); the program
 * runs itself as the child. Exit 51 iff all pass, else 100 + the first check
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

#define WM_HELLO   (WM_APP + 1)    /* child -> parent: wParam its window, lParam its thread */
#define WM_ASK     (WM_APP + 2)    /* parent -> child, sent: wParam*1000 + lParam (+100 InSendMessage) */
#define WM_BACK    (WM_APP + 3)    /* parent -> child, sent: the child sends WM_ECHO to the parent first */
#define WM_ECHO    (WM_APP + 4)    /* child -> parent, sent while the parent waits */
#define WM_STALL   (WM_APP + 5)    /* parent -> child, posted: the child sleeps 1 s */
#define WM_LATE    (WM_APP + 6)    /* parent -> child, SendMessageTimeout while it sleeps */
#define WM_TMSG    (WM_APP + 7)    /* parent -> child's thread, PostThreadMessage */
#define WM_TGOT    (WM_APP + 8)    /* child -> parent, posted: the thread message's wParam */
#define WM_BYE     (WM_APP + 9)    /* parent -> child, posted: destroy and quit */

static HWND g_parent, g_child;
static DWORD g_child_tid;
static UINT_PTR g_hello_w; static LPARAM g_hello_l;
static int g_hello, g_tgot = -1, g_tgot_seen;
static int g_echo_in_send = -1;
static DWORD g_echo_tid;
static char g_copy[64]; static int g_copy_n = -1; static ULONG_PTR g_copy_tag;

/* ---- the child ---- */
static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ASK:
        return (LRESULT)(w * 1000 + l + (InSendMessage() ? 100 : 0));
    case WM_BACK: {
        LRESULT r = SendMessageA(g_parent, WM_ECHO, 7, 0);
        return r + 1;
    }
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)l;
        unsigned sum = 0;
        for (DWORD i = 0; i < cd->cbData; i++) sum = sum * 31 + ((const unsigned char *)cd->lpData)[i];
        return (LRESULT)(sum ^ (unsigned)cd->dwData ^ (unsigned)(w == (WPARAM)g_parent ? 0x5A : 0));
    }
    case WM_STALL:
        Sleep(1000);
        return 0;
    case WM_BYE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(3);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static int child(HWND parent) {
    g_parent = parent;
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = child_proc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "W7rChild";
    if (!RegisterClassA(&wc)) return 20;
    HWND h = CreateWindowExA(0, "W7rChild", "w7r child", WS_OVERLAPPEDWINDOW, 10, 10, 200, 100, 0, 0, wc.hInstance, 0);
    if (!h) return 21;
    if (!PostMessageA(parent, WM_HELLO, (WPARAM)h, (LPARAM)GetCurrentThreadId())) return 22;
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == WM_TMSG) { PostMessageA(parent, WM_TGOT, msg.wParam, 0); continue; }
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}

/* ---- the parent's window ---- */
static LRESULT CALLBACK parent_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HELLO: g_hello_w = w; g_hello_l = l; g_hello = 1; return 0;
    case WM_TGOT: g_tgot = (int)w; g_tgot_seen = 1; return 0;
    case WM_ECHO:
        g_echo_in_send = InSendMessage() ? 1 : 0;
        g_echo_tid = GetCurrentThreadId();
        return (LRESULT)(w * 10);
    }
    return DefWindowProcA(h, m, w, l);
}
/* pump the parent's messages until *flag or ms pass */
static void pump_until(volatile int *flag, DWORD ms) {
    DWORD t0 = GetTickCount();
    MSG msg;
    while (!*flag && GetTickCount() - t0 < ms) {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (!*flag) Sleep(5);
    }
}

/* ---- 9: another thread's window in this process ---- */
static HWND g_tw;
static DWORD g_tw_tid, g_proc_tid;
static HANDLE g_tw_ready;
static int g_order[8], g_norder;
static LRESULT CALLBACK thread_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m >= WM_APP + 20 && m < WM_APP + 30) {
        if (g_norder < 8) g_order[g_norder++] = (int)(m - WM_APP - 20);
        g_proc_tid = GetCurrentThreadId();
        if (m == WM_APP + 29) { DestroyWindow(h); return 0; }
        return (LRESULT)(m - WM_APP) + (InSendMessage() ? 1000 : 0);
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}
static DWORD WINAPI window_thread(LPVOID p) {
    (void)p;
    g_tw = CreateWindowExA(0, "W7rThread", "w7r thread", WS_OVERLAPPEDWINDOW, 0, 0, 100, 60, 0, 0, GetModuleHandleA(0), 0);
    g_tw_tid = GetCurrentThreadId();
    SetEvent(g_tw_ready);
    Sleep(300);                                   /* not taking messages: one is posted, one sent */
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) DispatchMessageA(&msg);
    return 0;
}

/* ---- 10: EnumWindows ---- */
static int g_saw_parent, g_saw_child, g_enum_n;
static BOOL CALLBACK enum_proc(HWND h, LPARAM l) {
    (void)l;
    g_enum_n++;
    if (h == g_parent) g_saw_parent = 1;
    if (h == g_child) g_saw_child = 1;
    return TRUE;
}

static unsigned long long parse_u64(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) return child((HWND)(UINT_PTR)parse_u64(argv[2]));

    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = parent_proc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "W7rParent";
    RegisterClassA(&wc);
    wc.lpfnWndProc = thread_proc; wc.lpszClassName = "W7rThread";
    RegisterClassA(&wc);
    g_parent = CreateWindowExA(0, "W7rParent", "w7r parent", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, 0, 0, wc.hInstance, 0);
    if (!g_parent) { printf("no window\n"); return 99; }

    /* the child, the parent's window on its command line as a number */
    char self[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(0, self, sizeof self);
    sprintf(cmd, "\"%s\" child %llu", self, (unsigned long long)(UINT_PTR)g_parent);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) { printf("no child\n"); return 98; }

    /* 1: the child's PostMessage to this window, by the number it was given */
    pump_until(&g_hello, 5000);
    g_child = (HWND)g_hello_w;
    g_child_tid = (DWORD)g_hello_l;
    DWORD pid = 0;
    DWORD tid = g_child ? GetWindowThreadProcessId(g_child, &pid) : 0;
    DWORD mpid = 0, mtid = GetWindowThreadProcessId(g_parent, &mpid);
    printf("     hello %d: child window %p thread %lu (CreateProcess: %lu); GetWindowThreadProcessId %lu %lu (pid %lu); own %d %d\n",
           g_hello, (void *)g_child, (unsigned long)g_child_tid, (unsigned long)pi.dwThreadId,
           (unsigned long)tid, (unsigned long)pid, (unsigned long)pi.dwProcessId,
           mtid == GetCurrentThreadId(), mpid == GetCurrentProcessId());
    check(1, g_hello && g_child && g_child_tid == pi.dwThreadId && tid == pi.dwThreadId && pid == pi.dwProcessId &&
             mtid == GetCurrentThreadId() && mpid == GetCurrentProcessId(),
          "a child posts to this window by the HWND on its command line; GetWindowThreadProcessId names the child's thread and process");

    /* 2: FindWindow */
    HWND f1 = FindWindowA("W7rChild", NULL), f2 = FindWindowA(NULL, "w7r child"), f3 = FindWindowA("W7rChild", "w7r child");
    HWND f4 = FindWindowA("w7rchild", NULL), f5 = FindWindowA(NULL, "W7R CHILD"), f6 = FindWindowW(L"W7rChild", L"w7r child");
    HWND f7 = FindWindowA("W7rParent", NULL);
    SetLastError(0); HWND f8 = FindWindowA("W7rNobody", NULL); DWORD e8 = GetLastError();
    SetLastError(0); HWND f9 = FindWindowA("W7rChild", "w7r other"); DWORD e9 = GetLastError();
    printf("     FindWindow: %d %d %d %d %d %d own %d; none %p/%lu %p/%lu\n", f1 == g_child, f2 == g_child, f3 == g_child,
           f4 == g_child, f5 == g_child, f6 == g_child, f7 == g_parent, (void *)f8, (unsigned long)e8, (void *)f9, (unsigned long)e9);
    check(2, f1 == g_child && f2 == g_child && f3 == g_child && f4 == g_child && f5 == g_child && f6 == g_child &&
             f7 == g_parent && !f8 && e8 == 0 && !f9 && e9 == 0,
          "FindWindowA/W find the child's window by class, by title, by both (neither's case counts); the same number it has");

    /* 3: what this process can read of it */
    char t[64] = ""; int tn = GetWindowTextA(g_child, t, sizeof t);
    char c[64] = ""; int cn = GetClassNameA(g_child, c, sizeof c);
    int tl = GetWindowTextLengthA(g_child);
    printf("     IsWindow %d, title \"%s\" %d (length %d), class \"%s\" %d\n", IsWindow(g_child), t, tn, tl, c, cn);
    check(3, IsWindow(g_child) && tn == 9 && !strcmp(t, "w7r child") && tl == 9 && cn == 8 && !strcmp(c, "W7rChild"),
          "IsWindow, GetWindowText, GetWindowTextLength and GetClassName of the child's window");

    /* 4: SendMessage runs the child's procedure in the child */
    LRESULT r4 = SendMessageA(g_child, WM_ASK, 5, 6);
    printf("     SendMessage -> %ld\n", (long)r4);
    check(4, r4 == 5106, "SendMessage to the child's window: its procedure's answer, InSendMessage TRUE there");

    /* 5: WM_COPYDATA */
    static const char data[] = "across the line";
    COPYDATASTRUCT cd = { 77, sizeof data, (PVOID)data };
    unsigned want = 0;
    for (DWORD i = 0; i < sizeof data; i++) want = want * 31 + (unsigned char)data[i];
    LRESULT r5 = SendMessageA(g_child, WM_COPYDATA, (WPARAM)g_parent, (LPARAM)&cd);
    printf("     WM_COPYDATA -> %lx (want %x)\n", (unsigned long)r5, want ^ 77u ^ 0x5Au);
    check(5, (unsigned)r5 == (want ^ 77u ^ 0x5Au), "WM_COPYDATA: the child reads the data this process sent");

    /* 6: WM_GETTEXT, WM_SETTEXT across */
    char g[64]; memset(g, 'x', sizeof g);
    LRESULT r6a = SendMessageA(g_child, WM_GETTEXT, sizeof g, (LPARAM)g);
    LRESULT r6b = SendMessageA(g_child, WM_GETTEXTLENGTH, 0, 0);
    LRESULT r6c = SendMessageA(g_child, WM_SETTEXT, 0, (LPARAM)"w7r renamed");
    char t2[64] = ""; GetWindowTextA(g_child, t2, sizeof t2);
    HWND f6r = FindWindowA(NULL, "w7r renamed");
    char g2[4]; memset(g2, 'x', sizeof g2);
    LRESULT r6d = SendMessageA(g_child, WM_GETTEXT, sizeof g2, (LPARAM)g2);
    printf("     WM_GETTEXT %ld \"%s\", length %ld, WM_SETTEXT %ld -> \"%s\" found %d, short %ld \"%.4s\"\n",
           (long)r6a, g, (long)r6b, (long)r6c, t2, f6r == g_child, (long)r6d, g2);
    check(6, r6a == 9 && !strcmp(g, "w7r child") && r6b == 9 && r6c == 1 && !strcmp(t2, "w7r renamed") && f6r == g_child &&
             r6d == 3 && !memcmp(g2, "w7r", 4),
          "WM_GETTEXT, WM_GETTEXTLENGTH and WM_SETTEXT carry the text across; FindWindow finds the new title");

    /* 7: a send back while this thread waits in SendMessage */
    LRESULT r7 = SendMessageA(g_child, WM_BACK, 0, 0);
    printf("     WM_BACK -> %ld; WM_ECHO here: InSendMessage %d, this thread %d\n", (long)r7, g_echo_in_send,
           g_echo_tid == GetCurrentThreadId());
    check(7, r7 == 71 && g_echo_in_send == 1 && g_echo_tid == GetCurrentThreadId(),
          "the child's procedure sends back to this window while this thread waits in SendMessage: it runs here, then the answer");

    /* 8: SendMessageTimeout to a thread that does not take it */
    PostMessageA(g_child, WM_STALL, 0, 0);
    Sleep(100);
    DWORD_PTR res = 12345;
    SetLastError(0);
    DWORD t0 = GetTickCount();
    LRESULT r8 = SendMessageTimeoutA(g_child, WM_ASK, 1, 2, SMTO_NORMAL, 200, &res);
    DWORD e8b = GetLastError(), dt = GetTickCount() - t0;
    Sleep(1000);
    res = 0;
    LRESULT r8b = SendMessageTimeoutA(g_child, WM_ASK, 3, 4, SMTO_NORMAL, 2000, &res);
    printf("     SendMessageTimeout while it sleeps: %ld, error %lu, after %s; later: %ld result %lu\n",
           (long)r8, (unsigned long)e8b, dt >= 150 && dt < 900 ? "the time" : "not the time", (long)r8b, (unsigned long)res);
    check(8, r8 == 0 && e8b == ERROR_TIMEOUT && dt >= 150 && dt < 900 && r8b != 0 && res == 3104,
          "SendMessageTimeout to a window whose thread does not take messages: 0, ERROR_TIMEOUT; then its answer");

    /* 9: another thread's window in this process: its procedure on its thread, a sent message before the posted */
    g_tw_ready = CreateEventA(0, TRUE, FALSE, 0);
    HANDLE th = CreateThread(0, 0, window_thread, 0, 0, 0);
    WaitForSingleObject(g_tw_ready, 5000);
    PostMessageA(g_tw, WM_APP + 21, 0, 0);
    LRESULT r9 = SendMessageA(g_tw, WM_APP + 22, 0, 0);
    Sleep(100);                                   /* it takes the posted one now */
    LRESULT r9b = SendMessageA(g_tw, WM_APP + 23, 0, 0);
    DWORD p9 = 0, t9 = GetWindowThreadProcessId(g_tw, &p9);
    Sleep(100);
    SendMessageA(g_tw, WM_APP + 29, 0, 0);
    WaitForSingleObject(th, 5000);
    printf("     thread window: %ld %ld, ran on its thread %d, order", (long)r9, (long)r9b, g_proc_tid == g_tw_tid);
    for (int i = 0; i < g_norder; i++) printf(" %d", g_order[i]);
    printf("; GetWindowThreadProcessId %d %d\n", t9 == g_tw_tid, p9 == GetCurrentProcessId());
    check(9, r9 == 1022 && r9b == 1023 && g_proc_tid == g_tw_tid && g_norder >= 4 && g_order[0] == 2 && g_order[1] == 1 &&
             g_order[2] == 3 && g_order[3] == 9 && t9 == g_tw_tid && p9 == GetCurrentProcessId(),
          "SendMessage to another thread's window runs its procedure on that thread, before the message posted first");

    /* 10: EnumWindows; 11: PostThreadMessage to the child's thread */
    EnumWindows(enum_proc, 0);
    BOOL pt = PostThreadMessageA(g_child_tid, WM_TMSG, 42, 0);
    pump_until(&g_tgot_seen, 5000);
    printf("     EnumWindows: %d windows, this one %d, the child's %d; PostThreadMessage %d -> %d\n",
           g_enum_n, g_saw_parent, g_saw_child, pt, g_tgot);
    check(10, g_saw_parent && g_saw_child, "EnumWindows lists this window and the child's");
    check(11, pt && g_tgot == 42, "PostThreadMessage to the child's thread: its GetMessage has it");

    /* 12: the child's window when its process has ended */
    PostMessageA(g_child, WM_BYE, 0, 0);
    DWORD w12 = WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    SetLastError(0);
    BOOL p12 = PostMessageA(g_child, WM_ASK, 0, 0); DWORD e12 = GetLastError();
    LRESULT s12 = SendMessageA(g_child, WM_ASK, 1, 1);
    DWORD q12 = 77, t12 = GetWindowThreadProcessId(g_child, &q12);
    printf("     the child ended %lu (code %lu): IsWindow %d, PostMessage %d/%lu, SendMessage %ld, FindWindow %p, thread %lu pid %lu\n",
           (unsigned long)w12, (unsigned long)code, IsWindow(g_child), p12, (unsigned long)e12, (long)s12,
           (void *)FindWindowA("W7rChild", NULL), (unsigned long)t12, (unsigned long)q12);
    check(12, w12 == WAIT_OBJECT_0 && code == 3 && !IsWindow(g_child) && !p12 && e12 == ERROR_INVALID_WINDOW_HANDLE &&
              s12 == 0 && !FindWindowA("W7rChild", NULL) && t12 == 0,
          "the child's window when its process has ended: no window (PostMessage ERROR_INVALID_WINDOW_HANDLE)");

    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DestroyWindow(g_parent);
    if (first_fail) { printf("FAILED (first: %d)\n", first_fail); return 100 + first_fail; }
    printf("ALL PASS\n");
    return 51;
}
