/* win-modal52 -- Track C, W7u: waiting for messages and objects at once, and
 * modal loops that run what other threads send. MsgWaitForMultipleObjects
 * returns WAIT_OBJECT_0 + n for input of the kinds asked for that arrived
 * since the thread last looked at its queue (PeekMessage, GetQueueStatus:
 * input it has seen does not wake it; MWMO_INPUTAVAILABLE asks for any) --
 * before the objects -- WAIT_OBJECT_0 + i for a signalled object,
 * WAIT_TIMEOUT, WAIT_IO_COMPLETION once the thread's queued APCs have run
 * (MWMO_ALERTABLE; SleepEx and WaitForSingleObjectEx alertable the same);
 * a message another thread sends wakes it for QS_SENDMESSAGE. While a
 * MessageBox or a DialogBox is open the thread's modal loop runs messages
 * other threads send to its windows and dispatches messages posted to them;
 * the box is a "#32770" window another thread can find and close
 * (WM_COMMAND). Each check is what Windows 11 does (measured first: this
 * program exits 54 there). Exit 54 iff all pass, else 100 + the first check
 * that failed. */
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
static char g_s[4096];
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
/* how long a call took, coarsely */
static const char *took(ULONGLONG ms, ULONGLONG lo, ULONGLONG hi) { return ms < lo ? "early" : ms < hi ? "on time" : "late"; }

#define WM_PING  (WM_APP + 1)     /* sent by the helper: wParam*10 back */
#define WM_POSTD (WM_APP + 2)     /* posted by the helper to the main window */
#define WM_ENDIT (WM_APP + 3)     /* posted by the helper to the dialog */

static HWND g_main;
static DWORD g_main_tid;
static volatile LONG g_ping_ran, g_ping_tid_ok, g_posted_ran, g_in_modal;
static volatile LONG g_ping_in_modal, g_posted_in_modal;
static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PING:
        g_ping_ran++;
        g_ping_tid_ok = GetCurrentThreadId() == g_main_tid;
        g_ping_in_modal = g_in_modal;
        return (LRESULT)(w * 10);
    case WM_POSTD:
        g_posted_ran++;
        g_posted_in_modal = g_in_modal;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ---- the helper thread's jobs ---- */
static volatile LONG g_job;
static LRESULT g_send_r = -1;
static HWND g_found;
static char g_found_cls[64], g_found_title[64];
static DWORD WINAPI helper(LPVOID p) {
    (void)p;
    Sleep(300);
    switch (g_job) {
    case 1:                                       /* a send while the main thread waits in MsgWait */
        g_send_r = SendMessageA(g_main, WM_PING, 4, 0);
        break;
    case 2:                                       /* the message box: send, post, find it, close it */
    case 3: {                                     /* the dialog: the same, then end it by a posted message */
        g_send_r = SendMessageA(g_main, WM_PING, 5, 0);
        PostMessageA(g_main, WM_POSTD, 0, 0);
        Sleep(200);
        HWND b = FindWindowA("#32770", g_job == 2 ? "W52 box" : "W52 dialog");
        g_found = b;
        if (b) {
            GetClassNameA(b, g_found_cls, sizeof g_found_cls);
            GetWindowTextA(b, g_found_title, sizeof g_found_title);
            if (g_job == 2) PostMessageA(b, WM_COMMAND, IDCANCEL, 0);
            else PostMessageA(b, WM_ENDIT, 0, 0);
        }
        break;
    }
    }
    return 0;
}
static HANDLE start_helper(int job) {
    g_job = job;
    g_ping_ran = g_ping_tid_ok = g_posted_ran = 0;
    g_ping_in_modal = g_posted_in_modal = 0;
    g_send_r = -1; g_found = 0; g_found_cls[0] = g_found_title[0] = 0;
    return CreateThread(0, 0, helper, 0, 0, 0);
}

static void CALLBACK apc(ULONG_PTR p) { *(volatile LONG *)p = 1; }
static void drain(void) { MSG m; while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m); }

/* the dialog's procedure */
static INT_PTR CALLBACK dlg_proc(HWND d, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    switch (m) {
    case WM_INITDIALOG: return TRUE;
    case WM_ENDIT: EndDialog(d, 77); return TRUE;
    case WM_COMMAND: if (LOWORD(w) == IDCANCEL) { EndDialog(d, 2); return TRUE; } break;
    }
    return FALSE;
}

int main(void) {
    HINSTANCE inst = GetModuleHandleA(0);
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = main_proc; wc.hInstance = inst; wc.lpszClassName = "W52Main";
    RegisterClassA(&wc);
    g_main = CreateWindowExA(0, "W52Main", "w52 main", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, 0, 0, inst, 0);
    g_main_tid = GetCurrentThreadId();
    drain();

    /* 1: nothing to wake it */
    s_reset();
    {
        ULONGLONG t0 = GetTickCount64();
        DWORD r = MsgWaitForMultipleObjects(0, NULL, FALSE, 150, QS_ALLINPUT);
        s_add(" %lu %s", (unsigned long)r, took(GetTickCount64() - t0, 120, 1000));
        HANDLE ev = CreateEventA(0, TRUE, FALSE, 0);
        t0 = GetTickCount64();
        r = MsgWaitForMultipleObjects(1, &ev, FALSE, 150, QS_ALLINPUT);
        s_add(" %lu %s", (unsigned long)r, took(GetTickCount64() - t0, 120, 1000));
        r = MsgWaitForMultipleObjects(1, &ev, FALSE, 0, QS_ALLINPUT);
        s_add(" %lu", (unsigned long)r);
        CloseHandle(ev);
    }
    check(1, seen(1,
        " 258 on time 258 on time 258"), "MsgWaitForMultipleObjects with nothing to wake it: WAIT_TIMEOUT when the time is up (0: at once)");

    /* 2: a posted message is new input; once looked at, it is not */
    s_reset();
    {
        PostMessageA(g_main, WM_USER, 0, 0);
        DWORD r1 = MsgWaitForMultipleObjects(0, NULL, FALSE, 1000, QS_POSTMESSAGE);
        DWORD r2 = MsgWaitForMultipleObjects(0, NULL, FALSE, 0, QS_POSTMESSAGE);
        MSG m; BOOL pk = PeekMessageA(&m, 0, 0, 0, PM_NOREMOVE);
        ULONGLONG t0 = GetTickCount64();
        DWORD r3 = MsgWaitForMultipleObjects(0, NULL, FALSE, 150, QS_POSTMESSAGE);
        const char *t3 = took(GetTickCount64() - t0, 120, 1000);
        DWORD r4 = MsgWaitForMultipleObjectsEx(0, NULL, 150, QS_POSTMESSAGE, MWMO_INPUTAVAILABLE);
        DWORD r5 = MsgWaitForMultipleObjects(0, NULL, FALSE, 0, QS_ALLINPUT);
        DWORD st = GetQueueStatus(QS_POSTMESSAGE);
        s_add(" %lu %lu %d %lu %s %lu %lu %08lx", (unsigned long)r1, (unsigned long)r2, pk, (unsigned long)r3, t3,
              (unsigned long)r4, (unsigned long)r5, (unsigned long)st);
        drain();
        st = GetQueueStatus(QS_POSTMESSAGE);
        PostMessageA(g_main, WM_USER, 0, 0);
        DWORD st2 = GetQueueStatus(QS_POSTMESSAGE);
        DWORD st3 = GetQueueStatus(QS_POSTMESSAGE);
        s_add(" | %08lx %08lx %08lx", (unsigned long)st, (unsigned long)st2, (unsigned long)st3);
        DWORD r6 = MsgWaitForMultipleObjects(0, NULL, FALSE, 0, QS_POSTMESSAGE);
        s_add(" %lu", (unsigned long)r6);
        drain();
    }
    check(2, seen(2,
        " 0 0 1 258 on time 0 258 00080000 | 00000000 00080008 00080000 258"), "a posted message wakes it (WAIT_OBJECT_0 + 0); once PeekMessage or GetQueueStatus saw it, only MWMO_INPUTAVAILABLE does");

    /* 3: the kinds asked for */
    s_reset();
    {
        PostMessageA(g_main, WM_USER, 0, 0);
        DWORD r1 = MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_TIMER | QS_KEY | QS_MOUSE | QS_PAINT);
        DWORD r2 = MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_SENDMESSAGE);
        DWORD r3 = MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLPOSTMESSAGE);
        drain();
        SetTimer(g_main, 9, 60, NULL);
        ULONGLONG t0 = GetTickCount64();
        DWORD r4 = MsgWaitForMultipleObjects(0, NULL, FALSE, 2000, QS_TIMER);
        const char *t4 = took(GetTickCount64() - t0, 30, 1000);
        KillTimer(g_main, 9);
        drain();
        s_add(" %lu %lu %lu %lu %s", (unsigned long)r1, (unsigned long)r2, (unsigned long)r3, (unsigned long)r4, t4);
    }
    check(3, seen(3,
        " 258 258 0 0 on time"), "only the kinds of input asked for wake it: a posted message not for QS_TIMER or QS_SENDMESSAGE; a timer for QS_TIMER");

    /* 4: objects and input together */
    s_reset();
    {
        HANDLE ev[2];
        ev[0] = CreateEventA(0, TRUE, FALSE, 0);
        ev[1] = CreateEventA(0, TRUE, TRUE, 0);
        PostMessageA(g_main, WM_USER, 0, 0);
        DWORD r1 = MsgWaitForMultipleObjects(2, ev, FALSE, 1000, QS_POSTMESSAGE);
        drain();
        PostMessageA(g_main, WM_USER, 0, 0);
        ResetEvent(ev[1]);
        DWORD r2 = MsgWaitForMultipleObjects(2, ev, FALSE, 1000, QS_POSTMESSAGE);
        drain();
        SetEvent(ev[1]);
        DWORD r3 = MsgWaitForMultipleObjects(2, ev, TRUE, 100, QS_POSTMESSAGE);
        SetEvent(ev[0]);
        DWORD r4 = MsgWaitForMultipleObjects(2, ev, TRUE, 100, QS_POSTMESSAGE);
        PostMessageA(g_main, WM_USER, 0, 0);
        DWORD r5 = MsgWaitForMultipleObjects(2, ev, TRUE, 100, QS_POSTMESSAGE);
        drain();
        SetLastError(0);
        DWORD r6 = MsgWaitForMultipleObjects(1, NULL, FALSE, 0, QS_ALLINPUT);
        DWORD e6 = GetLastError();
        CloseHandle(ev[0]); CloseHandle(ev[1]);
        s_add(" %lu %lu %lu %lu %lu %lx/%lu", (unsigned long)r1, (unsigned long)r2, (unsigned long)r3, (unsigned long)r4,
              (unsigned long)r5, (unsigned long)r6, (unsigned long)e6);
    }
    check(4, seen(4,
        " 2 2 258 258 0 ffffffff/87"), "new input first (WAIT_OBJECT_0 + n), else a signalled object (+ i); bWaitAll: every object and new input (WAIT_OBJECT_0); NULL handles ERROR_INVALID_PARAMETER");

    /* 5: an APC (MWMO_ALERTABLE) */
    s_reset();
    {
        volatile LONG ran = 0;
        QueueUserAPC(apc, GetCurrentThread(), (ULONG_PTR)&ran);
        DWORD r1 = MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
        LONG ran1 = ran;
        DWORD r2 = MsgWaitForMultipleObjectsEx(0, NULL, 1000, QS_ALLINPUT, MWMO_ALERTABLE);
        s_add(" %lu %ld %lx %ld", (unsigned long)r1, (long)ran1, (unsigned long)r2, (long)ran);
        volatile LONG ran2 = 0, ran3 = 0;
        DWORD q = QueueUserAPC(apc, GetCurrentThread(), (ULONG_PTR)&ran2);
        DWORD s1 = SleepEx(30, FALSE);
        LONG a1 = ran2;
        DWORD s2 = SleepEx(1000, TRUE);
        LONG a2 = ran2;
        HANDLE ev = CreateEventA(0, TRUE, FALSE, 0);
        QueueUserAPC(apc, GetCurrentThread(), (ULONG_PTR)&ran3);
        ULONGLONG t0 = GetTickCount64();
        DWORD w1 = WaitForSingleObjectEx(ev, 1000, TRUE);
        const char *t1 = took(GetTickCount64() - t0, 0, 500);
        LONG a3 = ran3;
        CloseHandle(ev);
        s_add(" | %lu %lu %ld %lx %ld %lx %s %ld", (unsigned long)q, (unsigned long)s1, (long)a1, (unsigned long)s2, (long)a2,
              (unsigned long)w1, t1, (long)a3);
    }
    check(5, seen(5,
        " 258 0 c0 1 | 1 0 0 c0 1 c0 on time 1"), "MWMO_ALERTABLE runs a queued APC: WAIT_IO_COMPLETION; without it the APC waits; SleepEx and WaitForSingleObjectEx alertable the same");

    /* 6: a message another thread sends */
    s_reset();
    {
        HANDLE th = start_helper(1);
        ULONGLONG t0 = GetTickCount64();
        DWORD r1 = MsgWaitForMultipleObjects(0, NULL, FALSE, 3000, QS_SENDMESSAGE);
        const char *t1 = took(GetTickCount64() - t0, 200, 2500);
        LONG ran1 = g_ping_ran;
        MSG m; BOOL pk = PeekMessageA(&m, 0, 0, 0, PM_REMOVE);
        LONG ran2 = g_ping_ran;
        WaitForSingleObject(th, 3000); CloseHandle(th);
        s_add(" %lu %s %ld %d %ld %ld %ld", (unsigned long)r1, t1, (long)ran1, pk, (long)ran2, (long)g_ping_tid_ok, (long)g_send_r);
        th = start_helper(1);
        t0 = GetTickCount64();
        DWORD r2 = MsgWaitForMultipleObjects(0, NULL, FALSE, 1000, QS_POSTMESSAGE);
        const char *t2 = took(GetTickCount64() - t0, 900, 2500);
        LONG ran3 = g_ping_ran;
        drain();
        WaitForSingleObject(th, 3000); CloseHandle(th);
        s_add(" | %lu %s %ld %ld %ld", (unsigned long)r2, t2, (long)ran3, (long)g_ping_ran, (long)g_send_r);
    }
    check(6, seen(6,
        " 0 on time 0 0 1 1 40 | 258 on time 0 1 40"), "a message another thread sends wakes it for QS_SENDMESSAGE (it runs when the thread looks at its queue), not for QS_POSTMESSAGE");

    /* 7: a message box: another thread's message runs, a posted one is dispatched; it is found and closed */
    s_reset();
    {
        HANDLE th = start_helper(2);
        g_in_modal = 1;
        int r = MessageBoxA(g_main, "W52 text", "W52 box", MB_OKCANCEL);
        g_in_modal = 0;
        WaitForSingleObject(th, 3000); CloseHandle(th);
        s_add(" %d | %ld %ld %ld %ld | %ld %ld | %d %s %s", r, (long)g_ping_ran, (long)g_ping_tid_ok, (long)g_ping_in_modal,
              (long)g_send_r, (long)g_posted_ran, (long)g_posted_in_modal, g_found != 0, g_found_cls, g_found_title);
        drain();
    }
    check(7, seen(7,
        " 2 | 1 1 1 50 | 1 1 | 1 #32770 W52 box"), "while a MessageBox is open its thread runs what another thread sends and dispatches what is posted; the box is a #32770 window WM_COMMAND closes");

    /* 8: the same while a dialog is open; a message posted to the dialog reaches its procedure */
    s_reset();
    {
        HANDLE th = start_helper(3);
        g_in_modal = 1;
        INT_PTR r = DialogBoxParamA(inst, MAKEINTRESOURCEA(52), g_main, dlg_proc, 0);
        g_in_modal = 0;
        WaitForSingleObject(th, 3000); CloseHandle(th);
        s_add(" %ld | %ld %ld %ld %ld | %ld %ld | %d %s %s", (long)r, (long)g_ping_ran, (long)g_ping_tid_ok, (long)g_ping_in_modal,
              (long)g_send_r, (long)g_posted_ran, (long)g_posted_in_modal, g_found != 0, g_found_cls, g_found_title);
        drain();
    }
    check(8, seen(8,
        " 77 | 1 1 1 50 | 1 1 | 1 #32770 W52 dialog"), "while a DialogBox is open the same; a message posted to the dialog reaches its procedure (EndDialog's result back)");

    DestroyWindow(g_main);
    drain();
    if (first_fail) { printf("FAILED (first: %d)\n", first_fail); return 100 + first_fail; }
    printf("ALL PASS\n");
    return 54;
}
