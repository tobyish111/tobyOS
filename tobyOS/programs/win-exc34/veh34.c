/* win-veh34 -- Track C, W7c: vectored exception and continue handlers,
 * GetExitCodeThread, and a raise on one thread while another loads and
 * frees a DLL. The exceptions are RaiseException's, inside c_frame
 * (programs/win-exc33/seh33s.S: a __try/__finally/__except scope table whose
 * filter answers as each check sets). Every handler call is logged; the
 * checks are what Windows 11 does (measured first: it exits 34 there):
 *   - vectored handlers run before any frame's handler, in the order added
 *     (First puts one at the head), only while dispatching;
 *   - one that answers EXCEPTION_CONTINUE_EXECUTION ends the search -- even
 *     for a noncontinuable exception -- and the continue handlers run, in
 *     order, until one answers EXCEPTION_CONTINUE_EXECUTION; any other
 *     answer (0, 1, 7) goes on searching;
 *   - the continue handlers run too when a frame's filter, or the top-level
 *     filter, continues the exception;
 *   - Remove* answers TRUE once, then FALSE, and FALSE for a handle of the
 *     other list;
 *   - GetExitCodeThread: STILL_ACTIVE (259) while it runs, then the thread's
 *     return value or ExitThread's code; the current thread's pseudo handle
 *     is STILL_ACTIVE; a handle that is none, FALSE and
 *     ERROR_INVALID_HANDLE.
 * Exit 34 iff all pass, else 100 + the first that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

uint64_t g_landing_rbx, g_landing_xmm6;
void *g_mid_target;
extern uint64_t c_frame(void);
EXCEPTION_DISPOSITION h_mid(void *a, void *b, void *c, void *d) {
    (void)a; (void)b; (void)c; (void)d;
    return ExceptionContinueSearch;
}
EXCEPTION_DISPOSITION h_outer(void *a, void *b, void *c, void *d) {
    (void)a; (void)b; (void)c; (void)d;
    return ExceptionContinueSearch;
}

static char g_log[512];
static volatile int g_quiet;               /* the stress check logs nothing */
static LONG g_filter = EXCEPTION_EXECUTE_HANDLER;
static LONG g_ret[4];                      /* what each logging handler answers */
static DWORD g_flags;
static int g_addr_ok = 1;                  /* every vectored call: address == context rip */

static void logc(const char *s, const EXCEPTION_RECORD *r) {
    if (g_quiet) return;
    char h[12];
    int k = 0;
    unsigned long v = r->ExceptionFlags;
    char t[12];
    do { t[k++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    int j = 0;
    while (k) h[j++] = t[--k];
    h[j] = 0;
    strcat(g_log, s);
    strcat(g_log, "(");
    strcat(g_log, h);
    strcat(g_log, ") ");
}
LONG c_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)frame;
    logc("f", ep->ExceptionRecord);
    return g_filter;
}
void c_finally(BOOLEAN ab, void *frame) {
    (void)frame;
    if (!g_quiet) strcat(g_log, ab ? "t1 " : "t0 ");
}
__attribute__((noinline)) uint64_t raiser(void) {
    RaiseException(0xE0000001, g_flags, 0, 0);
    if (!g_quiet) strcat(g_log, "ret ");
    return 0x7777;
}

#define H(n) static LONG WINAPI h##n(EXCEPTION_POINTERS *ep) {                       \
    logc(#n, ep->ExceptionRecord);                                                 \
    if ((ULONG64)ep->ExceptionRecord->ExceptionAddress != ep->ContextRecord->Rip)  \
        g_addr_ok = 0;                                                             \
    return g_ret[n]; }
H(0) H(1) H(2) H(3)
static LONG WINAPI top(EXCEPTION_POINTERS *ep) {
    logc("top", ep->ExceptionRecord);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void reset(void) {
    g_log[0] = 0;
    memset(g_ret, 0, sizeof g_ret);
    g_filter = EXCEPTION_EXECUTE_HANDLER;
    g_flags = 0;
}

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok) printf("        log: %s\n", g_log);
    if (!ok && !first_fail) first_fail = n;
}

static HANDLE g_ev;
static DWORD WINAPI t_wait(void *p) { (void)p; WaitForSingleObject(g_ev, INFINITE); return 0x1234; }
static DWORD WINAPI t_exit(void *p) { (void)p; ExitThread(77); return 5; }

static volatile LONG g_caught, g_loads;
static DWORD WINAPI t_raise(void *p) {
    (void)p;
    for (int i = 0; i < 300; i++)
        if (c_frame() == 0xE0000001) InterlockedIncrement(&g_caught);
    return 0;
}
static DWORD WINAPI t_load(void *p) {
    (void)p;
    for (int i = 0; i < 100; i++) {
        HMODULE m = LoadLibraryA("dll28c.dll");
        if (m && FreeLibrary(m)) InterlockedIncrement(&g_loads);
    }
    return 0;
}

int main(void) {
    printf("win-veh34: vectored handlers, GetExitCodeThread\n");
    SetUnhandledExceptionFilter(top);
    uint64_t r;

    reset();
    void *a = AddVectoredExceptionHandler(0, h0);
    void *b = AddVectoredExceptionHandler(0, h1);
    r = c_frame();
    check(1, a && b && a != b && r == 0xE0000001 && strcmp(g_log, "0(80) 1(80) f(80) t1 ") == 0,
          "two vectored handlers, in the order added, before the frame's filter; none while unwinding");
    RemoveVectoredExceptionHandler(a);
    RemoveVectoredExceptionHandler(b);

    reset();
    a = AddVectoredExceptionHandler(0, h0);
    b = AddVectoredExceptionHandler(1, h1);
    r = c_frame();
    check(2, r == 0xE0000001 && strcmp(g_log, "1(80) 0(80) f(80) t1 ") == 0,
          "First != 0 puts a handler at the head");

    reset();
    void *c1 = AddVectoredContinueHandler(0, h2);
    void *c2 = AddVectoredContinueHandler(0, h3);
    g_ret[1] = EXCEPTION_CONTINUE_EXECUTION;
    r = c_frame();
    check(3, c1 && c2 && r == 0 && strcmp(g_log, "1(80) 2(80) 3(80) ret ") == 0,
          "a vectored handler continues: no frame handler, the continue handlers in order, RaiseException returns");

    reset();
    g_filter = EXCEPTION_CONTINUE_EXECUTION;
    r = c_frame();
    check(4, r == 0 && strcmp(g_log, "1(80) 0(80) f(80) 2(80) 3(80) ret ") == 0,
          "a frame's filter continues: the continue handlers run too");

    reset();
    g_ret[1] = EXCEPTION_CONTINUE_EXECUTION;
    g_flags = EXCEPTION_NONCONTINUABLE;
    r = c_frame();
    check(5, r == 0 && strcmp(g_log, "1(81) 2(81) 3(81) ret ") == 0,
          "a vectored handler continues a noncontinuable exception: it goes on (no 0xC0000025)");

    reset();
    g_ret[1] = EXCEPTION_CONTINUE_EXECUTION;
    g_ret[2] = EXCEPTION_CONTINUE_EXECUTION;
    r = c_frame();
    check(6, r == 0 && strcmp(g_log, "1(80) 2(80) ret ") == 0,
          "a continue handler's CONTINUE_EXECUTION ends the continue handlers");

    reset();
    g_ret[1] = EXCEPTION_EXECUTE_HANDLER;
    r = c_frame();
    int ok7 = r == 0xE0000001 && strcmp(g_log, "1(80) 0(80) f(80) t1 ") == 0;
    reset();
    g_ret[1] = 7;
    r = c_frame();
    ok7 = ok7 && r == 0xE0000001 && strcmp(g_log, "1(80) 0(80) f(80) t1 ") == 0;
    check(7, ok7, "a vectored handler's 1 or 7 goes on searching");

    ULONG rb1 = RemoveVectoredExceptionHandler(b);
    ULONG rb2 = RemoveVectoredExceptionHandler(b);
    ULONG rc1 = RemoveVectoredContinueHandler(c1);
    ULONG rc2 = RemoveVectoredContinueHandler(c1);
    ULONG rx = RemoveVectoredContinueHandler(a);
    reset();
    r = c_frame();
    check(8, rb1 && !rb2 && rc1 && !rc2 && !rx && r == 0xE0000001 &&
             strcmp(g_log, "0(80) f(80) t1 ") == 0,
          "Remove: TRUE, then FALSE; FALSE for the other list's handle; a removed handler is not called");

    reset();
    r = raiser();
    check(9, r == 0x7777 && strcmp(g_log, "0(80) top(80) 3(80) ret ") == 0,
          "no frame takes it, the top-level filter continues: the continue handlers run");
    RemoveVectoredExceptionHandler(a);
    RemoveVectoredContinueHandler(c2);

    check(10, g_addr_ok, "every vectored call: ExceptionAddress == ContextRecord->Rip (the raise's context)");

    g_ev = CreateEventA(0, TRUE, FALSE, 0);
    HANDLE th = CreateThread(0, 0, t_wait, 0, 0, 0);
    DWORD code = 0;
    BOOL got = GetExitCodeThread(th, &code);
    check(11, got && code == STILL_ACTIVE, "GetExitCodeThread of a running thread: STILL_ACTIVE (259)");
    SetEvent(g_ev);
    WaitForSingleObject(th, INFINITE);
    code = 0;
    got = GetExitCodeThread(th, &code);
    check(12, got && code == 0x1234, "after it returns 0x1234 (waited for): 0x1234");
    HANDLE te = CreateThread(0, 0, t_exit, 0, 0, 0);
    WaitForSingleObject(te, 10000);
    code = 0;
    got = GetExitCodeThread(te, &code);
    check(13, got && code == 77, "a thread that called ExitThread(77): 77");
    DWORD cur = 0, bad = 0;
    BOOL gc = GetExitCodeThread(GetCurrentThread(), &cur);
    SetLastError(0);
    BOOL gb = GetExitCodeThread((HANDLE)(ULONG_PTR)0x12345, &bad);
    DWORD err = GetLastError();
    check(14, gc && cur == STILL_ACTIVE && !gb && err == ERROR_INVALID_HANDLE,
          "the current thread: STILL_ACTIVE; not a handle: FALSE, ERROR_INVALID_HANDLE");

    g_quiet = 1;
    void *v = AddVectoredExceptionHandler(0, h0);
    HANDLE ta = CreateThread(0, 0, t_raise, 0, 0, 0);
    HANDLE tl = CreateThread(0, 0, t_load, 0, 0, 0);
    WaitForSingleObject(ta, 60000);
    WaitForSingleObject(tl, 60000);
    RemoveVectoredExceptionHandler(v);
    g_quiet = 0;
    check(15, g_caught == 300 && g_loads == 100,
          "300 raises caught on one thread while another loads and frees a DLL 100 times");

    printf("win-veh34: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 34;
}
