/* life30a.dll -- Track C, W5: imported by win-life30.exe (and by life30c,
 * life30d and life30s). It keeps the log (life30.h), has a thread-local
 * variable whose initial value is 0xA1, its own TLS callback ('a') besides
 * its DllMain ('A'), and the checks of the process's exit (a_exit_check),
 * which the .exe's TLS callback makes at DLL_PROCESS_DETACH, the exit's last
 * call. */
#define LIFE30A_BUILD
#include <stdio.h>
#include "life30.h"

static _Thread_local int a_tls = 0xA1;

#define LOGMAX 512
static struct l30_ev g_log[LOGMAX];
static volatile LONG g_n;
static volatile LONG g_on = 1;
static volatile LONG g_count[128][4];
static struct l30_exit g_exit;
static volatile LONG g_armed;
static int g_arm_at;
static volatile LONG g_param;

void a_log(char who, DWORD reason, void *res, int tls) {
    InterlockedIncrement(&g_count[(unsigned char)who & 127][reason & 3]);
    if (!g_on) return;
    LONG i = InterlockedIncrement(&g_n) - 1;
    if (i >= LOGMAX) return;
    g_log[i].who = who;
    g_log[i].reason = (unsigned char)reason;
    g_log[i].res = res != NULL;
    g_log[i].tid = GetCurrentThreadId();
    g_log[i].tls = tls;
}
int a_log_n(void) { return g_n < LOGMAX ? (int)g_n : LOGMAX; }
int a_log_get(int i, struct l30_ev *out) {
    if (i < 0 || i >= a_log_n()) return 0;
    *out = g_log[i];
    return 1;
}
void a_log_reset(void) { g_n = 0; }
void a_logging(int on) { g_on = on; }
LONG a_count(char who, DWORD reason) { return g_count[(unsigned char)who & 127][reason & 3]; }
int a_get_tls(void) { return a_tls; }
void a_set_param(int ms) { g_param = ms; }
int a_get_param(void) { return g_param; }
void a_set_tls(int v) { a_tls = v; }
extern ULONG _tls_index;
DWORD a_tls_index(void) { return _tls_index; }
void a_arm_exit(const struct l30_exit *e) {
    g_exit = *e;
    g_arm_at = a_log_n();
    g_armed = 1;
}

/* The exit checks. Returns 0, or the number of the first that fails. */
static int exit_checks(void) {
    /* 1: the thread that kept counting was ended before the DLLs were told */
    LONG s0 = *g_exit.spin;
    Sleep(60);
    if (*g_exit.spin != s0) return 1;
    /* 2..: the events since the .exe armed us, against the expected sequence */
    const char *p = g_exit.seq;
    int k = g_arm_at, step = 2;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char who = p[0];
        DWORD reason = (DWORD)(p[1] - '0');
        p += 2;
        int res = -1;                 /* r: lpReserved non-NULL, n: NULL */
        if (*p == 'r') { res = 1; p++; } else if (*p == 'n') { res = 0; p++; }
        struct l30_ev ev;
        if (!a_log_get(k++, &ev)) return step;
        if (ev.who != who || ev.reason != reason) return step;
        if (res >= 0 && ev.res != res) return step;
        if (ev.tid != g_exit.tid) return step;   /* on the exiting thread */
        step++;
    }
    /* nothing else -- above all no DLL_THREAD_DETACH for the counting thread */
    for (int i = g_arm_at; i < a_log_n(); i++) {
        struct l30_ev ev;
        a_log_get(i, &ev);
        if (ev.tid == g_exit.spin_tid) return 40;
    }
    if (a_log_n() != k) return 41;
    return 0;
}

static void NTAPI a_tls_cb(PVOID h, DWORD reason, PVOID res) {
    (void)h;
    a_log('a', reason, res, a_tls);
}
__attribute__((section(".CRT$XLB"), used)) const PIMAGE_TLS_CALLBACK p_a_tls_cb = a_tls_cb;

/* The exit checks, called by the .exe's TLS callback at DLL_PROCESS_DETACH
 * -- the last call of the exit. Prints the events since the .exe armed them;
 * returns 0, or the number of the first check that failed. */
int a_exit_check(void) {
    if (!g_armed) return -1;
    int bad = exit_checks();
    int n = a_log_n();
    printf("life30a: exit log (%d events since armed):", n - g_arm_at);
    for (int i = g_arm_at; i < n; i++) {
        struct l30_ev ev;
        a_log_get(i, &ev);
        printf(" %c%u%s", ev.who, ev.reason, ev.res ? "r" : "");
    }
    printf("\nlife30a: exit checks: %s %d\n", bad ? "FAIL at" : "ok", bad);
    fflush(stdout);
    return bad;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID res) {
    (void)hinst;
    a_log('A', reason, res, a_tls);
    return TRUE;
}
