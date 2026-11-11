/* win-life30b -- Track C, W5: the main thread's ExitThread, and
 * FreeLibraryAndExitThread. It imports life30a.dll (the log, life30.h).
 * From Microsoft's ExitThread page ("if the thread is the last thread in the
 * process when this function is called, the thread's process is also
 * terminated") and FreeLibraryAndExitThread's, and measured on Windows 11
 * (it exits 31 there):
 *   1. the main thread calls ExitThread while another thread runs: it gets
 *      DLL_THREAD_DETACH, and the process goes on;
 *   2. that other thread, now the last, loads life30s.dll and calls
 *      FreeLibraryAndExitThread on it: life30s gets DLL_PROCESS_DETACH
 *      (lpReserved NULL), and then -- the process's last thread ending --
 *      no DLL_THREAD_DETACH but the process's DLL_PROCESS_DETACH, on that
 *      thread (lpReserved non-NULL), the .exe's TLS callback last. That
 *      callback checks the exit's calls and ends the process with 31 when
 *      they are right, else 150 + the failing check.
 * A failure before the exit ends the process with 100 + its number. */
#include <stdio.h>
#include "life30.h"

static volatile LONG g_armed;
static void NTAPI exe_tls_cb(PVOID h, DWORD reason, PVOID res) {
    (void)h;
    a_log('X', reason, res, 0);
    if (reason == DLL_PROCESS_DETACH && g_armed) {
        int bad = a_exit_check();
        TerminateProcess(GetCurrentProcess(), bad ? 150 + bad : 31);
    }
}
__attribute__((section(".CRT$XLB"), used)) const PIMAGE_TLS_CALLBACK p_exe_tls_cb = exe_tls_cb;

static DWORD g_main;
static volatile LONG g_quiet;      /* a counter no thread touches */

/* life30a's DllMain has had the main thread's DLL_THREAD_DETACH */
static int main_detached(void) {
    for (int i = 0; i < a_log_n(); i++) {
        struct l30_ev ev;
        a_log_get(i, &ev);
        if (ev.who == 'A' && ev.reason == DLL_THREAD_DETACH && ev.tid == g_main) return 1;
    }
    return 0;
}

static DWORD WINAPI w_fn(LPVOID p) {
    (void)p;
    for (int i = 0; i < 5000 && !main_detached(); i++) Sleep(1);
    if (!main_detached()) {
        printf("FAIL  1 the main thread's ExitThread: no DLL_THREAD_DETACH within 5 s\n");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 101);
    }
    printf("ok    1 the main thread's ExitThread: DLL_THREAD_DETACH on it, and the process goes on\n");
    HMODULE s = LoadLibraryA("life30s.dll");
    if (!s) {
        printf("FAIL  2 LoadLibrary(life30s.dll) on the last thread\n");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 102);
    }
    struct l30_exit e = { "S0n a0 A0r X0", GetCurrentThreadId(), &g_quiet, 0 };
    printf("win-life30b: FreeLibraryAndExitThread(life30s, 7) on the last thread -- the .exe's "
           "TLS callback at DLL_PROCESS_DETACH ends the process with 31 if the exit is right\n");
    fflush(stdout);
    a_arm_exit(&e);
    g_armed = 1;
    FreeLibraryAndExitThread(s, 7);
    printf("FAIL  3 FreeLibraryAndExitThread returned\n");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 103);
    return 0;
}

int main(void) {
    g_main = GetCurrentThreadId();
    printf("win-life30b: main thread %lu\n", (unsigned long)g_main);
    fflush(stdout);
    DWORD wid = 0;
    HANDLE w = CreateThread(NULL, 0, w_fn, NULL, 0, &wid);
    if (!w) return 104;
    ExitThread(5);
    printf("FAIL  4 ExitThread returned\n");
    return 105;
}
