/* win-life30 -- Track C, W5: DLL lifetime. It imports life30a.dll (which keeps
 * the log, life30.h) and life30c.dll; it loads life30n.dll, life30d.dll and
 * life30s.dll while it runs. The .exe, life30a and life30d each have a
 * thread-local variable and a TLS callback of their own. The checks, from
 * Microsoft's DllMain, ExitThread, ExitProcess, DisableThreadLibraryCalls and
 * PE format (TLS callback) documentation, and measured on Windows 11 where
 * those are silent (the expectations below are what it does):
 *   - at process start each DLL's TLS callbacks, then its DllMain, get
 *     DLL_PROCESS_ATTACH (lpReserved non-NULL: a static load), a DLL after
 *     the DLLs it imports, and the .exe's TLS callbacks run last, before main;
 *   - a DLL's thread-local storage has its own slot (_tls_index past the
 *     .exe's), and every thread its own copy -- threads made later, and
 *     (LoadLibrary) threads that were already running;
 *   - a new thread gets DLL_THREAD_ATTACH in that thread before its function
 *     and DLL_THREAD_DETACH after it returns or calls ExitThread, from every
 *     loaded DLL -- each module's TLS callbacks before its DllMain, the
 *     .exe's TLS callbacks last; the order among the DLLs is not documented
 *     (Windows 11 gives neither load nor initialization order), so it is not
 *     checked. A thread that was running before a LoadLibrary gets the new
 *     DLL's DETACH only. DisableThreadLibraryCalls stops them for a DLL with
 *     no TLS directory; for one with a TLS directory -- every DLL the mingw
 *     CRT starts -- it returns TRUE and the calls go on (the documentation
 *     says it fails there; Windows 11 does this);
 *   - two threads loading and freeing at once, and a LoadLibrary while
 *     another thread is inside the DLL's DllMain: it waits for it (the
 *     loader lock);
 *   - ExitProcess: the other threads end first (none of them gets a
 *     DLL_THREAD_DETACH), then DLL_PROCESS_DETACH on the calling thread
 *     (lpReserved non-NULL) in the reverse of the order of initialization,
 *     each module's TLS callbacks before its DllMain, the .exe's last.
 * The last is checked by the .exe's own TLS callback at DLL_PROCESS_DETACH --
 * the exit's last call -- which ends the process with 30 when everything
 * holds. Each check prints ok/FAIL with its number; a failure ends the
 * process with 100 + the first that failed. Every expectation here was
 * measured on Windows 11 first: the test exits 30 there. */
#include <stdio.h>
#include "life30.h"

__declspec(dllimport) int c_dtlc_result(void);
extern ULONG _tls_index;

static _Thread_local int e_tls = 0xE0;

/* At the exit's DLL_PROCESS_DETACH -- the last call of all -- it ends the
 * process with 30 when the exit was right, else 150 + the failing check. */
static volatile LONG g_armed;
static void NTAPI exe_tls_cb(PVOID h, DWORD reason, PVOID res) {
    (void)h;
    a_log('X', reason, res, e_tls);
    if (reason == DLL_PROCESS_DETACH && g_armed) {
        int bad = a_exit_check();
        TerminateProcess(GetCurrentProcess(), bad ? 150 + bad : 30);
    }
}
__attribute__((section(".CRT$XLB"), used)) const PIMAGE_TLS_CALLBACK p_exe_tls_cb = exe_tls_cb;

static int first_fail;
static void check(int n, int cond, const char *what) {
    printf("%s %2d %s\n", cond ? "ok  " : "FAIL", n, what);
    if (!cond && !first_fail) first_fail = n;
}

static void log_dump(int from) {
    for (int i = from; i < a_log_n(); i++) {
        struct l30_ev ev;
        a_log_get(i, &ev);
        printf(" %c%u%s@%lu", ev.who, ev.reason, ev.res ? "r" : "", (unsigned long)ev.tid);
    }
    /* (a conversion, so the compiler does not make it putchar, which no
     * tobyOS shim has) */
    printf(" (%d)\n", a_log_n() - from);
}

/* One token of a spec: who, reason, then r = lpReserved non-NULL, n = NULL,
 * nothing = either. Returns the text after it, or NULL at its end. */
struct tok { char who; DWORD reason; int res; };
static const char *tok_read(const char *p, struct tok *t) {
    if (!p[0] || !p[1] || p[0] == ' ' || p[0] == '|' || p[0] == ';') return NULL;
    t->who = p[0];
    t->reason = (DWORD)(p[1] - '0');
    p += 2;
    t->res = -1;
    if (*p == 'r') { t->res = 1; p++; } else if (*p == 'n') { t->res = 0; p++; }
    return p;
}
static int tok_is(const struct tok *t, int k, DWORD tid) {
    struct l30_ev ev;
    return a_log_get(k, &ev) && ev.who == t->who && ev.reason == t->reason &&
           (t->res < 0 || ev.res == t->res) && (!tid || ev.tid == tid);
}

/* The log from event `from` on is exactly `spec`, every event on thread `tid`
 * (0: any). A spec is phases separated by ';'. A phase is runs separated by
 * spaces, then '|' and a tail: each run (tokens with no space between them,
 * "a2A2") appears once, its tokens in a row, the runs in any order; then the
 * tail's tokens, in order. */
static int log_like(int from, const char *spec, DWORD tid) {
    int k = from, ok = 1;
    const char *p = spec;
    while (ok && *p) {
        const char *runs[16];
        int nruns = 0;
        while (*p == ' ') p++;
        while (*p && *p != '|' && *p != ';') {             /* the runs */
            if (nruns < 16) runs[nruns++] = p;
            while (*p && *p != ' ' && *p != '|' && *p != ';') p++;
            while (*p == ' ') p++;
        }
        int used[16] = { 0 };
        for (int left = nruns; ok && left > 0; left--) {
            int hit = -1;
            for (int r = 0; r < nruns && hit < 0; r++) {
                if (used[r]) continue;
                const char *q = runs[r];
                struct tok t;
                int kk = k, good = 1;
                while (good && (q = tok_read(q, &t)) != NULL) good = tok_is(&t, kk++, tid);
                if (good) { hit = r; k = kk; }
            }
            if (hit < 0) ok = 0; else used[hit] = 1;
        }
        if (*p == '|') {                                     /* the tail */
            p++;
            while (ok && *p && *p != ';') {
                while (*p == ' ') p++;
                struct tok t;
                const char *q = tok_read(p, &t);
                if (!q) break;
                if (!tok_is(&t, k++, tid)) ok = 0;
                p = q;
            }
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    if (a_log_n() != k) ok = 0;
    if (!ok) {
        printf("     want %s (thread %lu)\n     got ", spec, (unsigned long)tid);
        log_dump(from);
    }
    return ok;
}

/* The thread-local value the first `who`/`reason` event since `from` saw. */
static int ev_tls(int from, char who, DWORD reason) {
    for (int i = from; i < a_log_n(); i++) {
        struct l30_ev ev;
        a_log_get(i, &ev);
        if (ev.who == who && ev.reason == reason) return ev.tls;
    }
    return -1;
}

typedef int (*get_fn)(void);
typedef void (*set_fn)(int);
static get_fn p_d_get;
static set_fn p_d_set;

/* a thread that reads its thread-locals and returns */
static volatile int t1_a, t1_e;
static DWORD WINAPI t1_fn(LPVOID p) {
    (void)p;
    t1_a = a_get_tls();
    t1_e = e_tls;
    return 7;
}

/* ExitThread from inside a call */
static volatile int t2_after;
static void t2_deep(void) { ExitThread(5); }
static DWORD WINAPI t2_fn(LPVOID p) {
    (void)p;
    t2_deep();
    t2_after = 1;
    return 9;
}

/* a thread that is running when life30d is loaded */
static volatile int w_state, w_d;
static DWORD WINAPI w_fn(LPVOID p) {
    (void)p;
    w_state = 1;
    while (w_state != 2) Sleep(1);
    w_d = p_d_get();
    w_state = 3;
    while (w_state != 4) Sleep(1);
    return 0;
}

/* a thread made after the load */
static volatile int t3_d;
static DWORD WINAPI t3_fn(LPVOID p) {
    (void)p;
    t3_d = p_d_get();
    return 0;
}

/* a thread that writes life30d's thread-local, sees it unloaded and loaded
 * again, and reads it once more */
static volatile int v_state, v_d, v_d2;
static DWORD WINAPI v_fn(LPVOID p) {
    (void)p;
    v_state = 1;
    while (v_state != 2) Sleep(1);
    p_d_set(0xBAD);
    v_d = p_d_get();
    v_state = 3;
    while (v_state != 4) Sleep(1);
    v_d2 = p_d_get();
    v_state = 5;
    while (v_state != 6) Sleep(1);
    return 0;
}

/* load and free, from two threads at once */
#define ROUNDS 60
static HMODULE g_hd;
static volatile LONG s_fails;
static DWORD WINAPI s_fn(LPVOID p) {
    (void)p;
    for (int i = 0; i < ROUNDS; i++) {
        HMODULE s = LoadLibraryA("life30s.dll");
        HMODULE d = LoadLibraryA("life30d.dll");
        get_fn sv = s ? (get_fn)(void *)GetProcAddress(s, "s_value") : 0;
        if (!s || d != g_hd || !sv || sv() != 0x5A) InterlockedIncrement(&s_fails);
        if (s && !FreeLibrary(s)) InterlockedIncrement(&s_fails);
        if (d && !FreeLibrary(d)) InterlockedIncrement(&s_fails);
    }
    return 0;
}

/* the loader lock: one thread loads life30s, whose DllMain then sleeps; a
 * second calls LoadLibrary while it does, and must get the module only once
 * that DllMain has returned */
static volatile LONG q_state;
static volatile int q_val;
static DWORD WINAPI p_fn(LPVOID p) {
    (void)p;
    HMODULE s = LoadLibraryA("life30s.dll");
    while (q_state != 2) Sleep(1);
    if (s) FreeLibrary(s);
    return 0;
}
static DWORD WINAPI q_fn(LPVOID p) {
    LONG before = (LONG)(LONG_PTR)p;
    int i;
    for (i = 0; i < 5000 && a_count('S', DLL_PROCESS_ATTACH) == before; i++) Sleep(1);
    if (i == 5000) { q_val = -2; q_state = 1; return 0; }
    HMODULE s = LoadLibraryA("life30s.dll");      /* life30s's DllMain is running */
    get_fn sv = s ? (get_fn)(void *)GetProcAddress(s, "s_value") : 0;
    q_val = sv ? sv() : -1;
    if (s) FreeLibrary(s);
    q_state = 1;
    return 0;
}

/* a thread still counting when the process exits */
static volatile LONG z_spin;
static DWORD WINAPI z_fn(LPVOID p) {
    (void)p;
    for (;;) {
        if ((InterlockedIncrement(&z_spin) & 0x3FF) == 0) Sleep(1);
    }
    return 0;
}

static HANDLE start(LPTHREAD_START_ROUTINE fn, DWORD *tid) {
    *tid = 0;
    return CreateThread(NULL, 0, fn, NULL, 0, tid);
}

int main(void) {
    DWORD me = GetCurrentThreadId();
    printf("win-life30: main thread %lu\n", (unsigned long)me);

    /* ---- process start ---- */
    check(1, log_like(0, "| a1 A1r C1r X1", me),
          "process start: life30a's TLS callback, then its DllMain, then life30c's (it imports a), "
          "then the .exe's TLS callback -- before main");
    check(2, a_tls_index() != _tls_index && a_get_tls() == 0xA1 && e_tls == 0xE0 &&
             ev_tls(0, 'A', 1) == 0xA1,
          "life30a's thread-locals: their own slot (_tls_index not the .exe's), initial values");
    printf("     (_tls_index: .exe %lu, life30a %lu)\n", (unsigned long)_tls_index,
           (unsigned long)a_tls_index());

    /* ---- DisableThreadLibraryCalls ---- */
    a_log_reset();
    HMODULE hn = LoadLibraryA("life30n.dll");
    get_fn n_res = hn ? (get_fn)(void *)GetProcAddress(hn, "n_dtlc_result") : 0;
    check(3, hn && n_res && n_res() == 1 && c_dtlc_result() == 1 && log_like(0, "| N1n", me),
          "DisableThreadLibraryCalls in a DllMain: TRUE, for life30n (no TLS directory) "
          "and for life30c (a TLS directory)");
    SetLastError(0);
    BOOL bogus = DisableThreadLibraryCalls((HMODULE)(ULONG_PTR)0x1230000);
    DWORD bogus_err = GetLastError();
    check(4, !bogus && bogus_err != 0, "DisableThreadLibraryCalls of no module: FALSE, an error");
    printf("     (error %lu)\n", (unsigned long)bogus_err);

    /* ---- a thread that returns ---- */
    a_set_tls(0x1111);
    e_tls = 0x2222;
    a_log_reset();
    DWORD t1id;
    HANDLE t1 = start(t1_fn, &t1id);
    if (t1) WaitForSingleObject(t1, INFINITE);
    check(5, t1 && log_like(0, "C2 a2A2 | X2 ; C3 a3A3 | X3", t1id),
          "a new thread: DLL_THREAD_ATTACH in it before its function, DETACH after it returns; "
          "life30c's too, life30n's not");
    check(6, t1_a == 0xA1 && t1_e == 0xE0 && ev_tls(0, 'A', 2) == 0xA1 &&
             a_get_tls() == 0x1111 && e_tls == 0x2222,
          "its own copies of the DLL's and the .exe's thread-locals, there before DllMain's THREAD_ATTACH");

    /* ---- ExitThread ---- */
    a_log_reset();
    DWORD t2id;
    HANDLE t2 = start(t2_fn, &t2id);
    if (t2) WaitForSingleObject(t2, INFINITE);
    check(7, t2 && log_like(0, "C2 a2A2 | X2 ; C3 a3A3 | X3", t2id) && !t2_after,
          "ExitThread inside a call: DLL_THREAD_DETACH, and nothing after it runs");

    /* ---- LoadLibrary while a thread runs ---- */
    DWORD wid;
    HANDLE w = start(w_fn, &wid);
    while (w && w_state != 1) Sleep(1);
    a_log_reset();
    HMODULE hd = LoadLibraryA("life30d.dll");
    g_hd = hd;
    p_d_get = hd ? (get_fn)(void *)GetProcAddress(hd, "d_get_tls") : 0;
    p_d_set = hd ? (set_fn)(void *)GetProcAddress(hd, "d_set_tls") : 0;
    check(8, hd && p_d_get && p_d_set && log_like(0, "| d1 D1n", me),
          "LoadLibrary: life30d's TLS callback, then its DllMain (lpReserved NULL); "
          "no DLL_THREAD_ATTACH to the thread already running");
    if (!hd || !p_d_get || !p_d_set) {
        printf("win-life30: cannot go on\n");
        ExitProcess(100 + (first_fail ? first_fail : 8));
    }
    w_state = 2;
    while (w_state != 3) Sleep(1);
    check(9, p_d_get() == 0xD1 && w_d == 0xD1 && ev_tls(0, 'D', 1) == 0xD1,
          "its thread-locals: on this thread, and on the thread that was already running");
    a_log_reset();
    w_state = 4;
    if (w) WaitForSingleObject(w, INFINITE);
    check(10, w && log_like(0, "C3 a3A3 d3D3 | X3", wid),
          "that thread's exit: DLL_THREAD_DETACH from every DLL loaded now -- life30d too");

    /* ---- a thread made after the load ---- */
    a_log_reset();
    DWORD t3id;
    HANDLE t3 = start(t3_fn, &t3id);
    if (t3) WaitForSingleObject(t3, INFINITE);
    check(11, t3 && t3_d == 0xD1 && log_like(0, "C2 a2A2 d2D2 | X2 ; C3 a3A3 d3D3 | X3", t3id),
          "a thread made after the load: life30d's THREAD_ATTACH and DETACH too, its thread-local 0xD1");

    /* ---- FreeLibrary and a reload while a thread runs ---- */
    DWORD vid;
    HANDLE v = start(v_fn, &vid);
    while (v && v_state != 1) Sleep(1);
    v_state = 2;
    while (v && v_state != 3) Sleep(1);
    a_log_reset();
    BOOL fr = FreeLibrary(hd);
    check(12, fr && v_d == 0xBAD && log_like(0, "| d0 D0n", me) && !GetModuleHandleA("life30d.dll"),
          "FreeLibrary: life30d's TLS callback, then its DllMain, get DLL_PROCESS_DETACH "
          "(lpReserved NULL); unloaded");
    hd = LoadLibraryA("life30d.dll");
    g_hd = hd;
    p_d_get = hd ? (get_fn)(void *)GetProcAddress(hd, "d_get_tls") : 0;
    p_d_set = hd ? (set_fn)(void *)GetProcAddress(hd, "d_set_tls") : 0;
    if (!hd || !p_d_get) {
        printf("win-life30: cannot go on\n");
        ExitProcess(100 + (first_fail ? first_fail : 13));
    }
    v_state = 4;
    while (v_state != 5) Sleep(1);
    check(13, v_d2 == 0xD1,
          "loaded again while the thread ran: its thread-local is the initial value again, "
          "not what it wrote before the unload");
    v_state = 6;
    if (v) WaitForSingleObject(v, INFINITE);

    /* ---- the loader lock ---- */
    a_logging(0);
    a_set_param(1);                       /* life30s's DllMain takes a while */
    LONG s_pa = a_count('S', 1), s_pd = a_count('S', 0), d_pa = a_count('D', 1);
    DWORD s1id, s2id;
    HANDLE s1 = start(s_fn, &s1id), s2 = start(s_fn, &s2id);
    if (s1) WaitForSingleObject(s1, INFINITE);
    if (s2) WaitForSingleObject(s2, INFINITE);
    a_logging(1);
    LONG s_att = a_count('S', 1) - s_pa, s_det = a_count('S', 0) - s_pd;
    check(14, s1 && s2 && s_fails == 0 && !GetModuleHandleA("life30s.dll") &&
              GetModuleHandleA("life30d.dll") == hd && s_att >= 1 && s_att == s_det &&
              a_count('D', 1) == d_pa,
          "two threads loading and freeing at once: every call works, life30s attached as often "
          "as detached and unloaded, life30d (still held) never reloaded");
    printf("     (%d rounds x 2 threads: %ld failures, life30s attached %ld times)\n",
           ROUNDS, (long)s_fails, (long)s_att);

    a_set_param(200);
    LONG s_before = a_count('S', 1);
    DWORD pid_, qid;
    HANDLE hp = start(p_fn, &pid_);
    HANDLE hq = CreateThread(NULL, 0, q_fn, (LPVOID)(LONG_PTR)s_before, 0, &qid);
    if (hq) WaitForSingleObject(hq, INFINITE);
    q_state = 2;
    if (hp) WaitForSingleObject(hp, INFINITE);
    a_set_param(0);
    check(15, hp && hq && q_val == 0x5A && a_count('S', 1) == s_before + 1 &&
              !GetModuleHandleA("life30s.dll"),
          "LoadLibrary while another thread is inside the DLL's DllMain: it waits until that "
          "returns (the loader lock), and gets the same module, ready");
    printf("     (s_value() on the second thread: %d)\n", q_val);

    /* ---- ExitProcess ---- */
    DWORD zid;
    HANDLE z = start(z_fn, &zid);
    while (z && z_spin < 2000) Sleep(1);
    if (first_fail) {
        printf("win-life30: check %d failed\n", first_fail);
        fflush(stdout);
        ExitProcess(100 + first_fail);
    }
    struct l30_exit e = { "d0 D0r N0r C0r a0 A0r X0", me, &z_spin, zid };
    printf("win-life30: checks 1-15 ok; ExitProcess(31) -- the .exe's TLS callback at "
           "DLL_PROCESS_DETACH ends the process with 30 if the exit is right\n");
    fflush(stdout);
    a_arm_exit(&e);
    g_armed = 1;
    ExitProcess(31);
}
