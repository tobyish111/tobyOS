/* win-so35 -- Track C, W7d (its second part): stack overflow. A thread's
 * stack is a reservation -- the .exe's SizeOfStackReserve for the main
 * thread, CreateThread's size with STACK_SIZE_PARAM_IS_A_RESERVATION for
 * another -- and running off its end is STATUS_STACK_OVERFLOW (0xC00000FD)
 * {1, the address}, raised while there is still room to dispatch it, once:
 * a second overflow (the guard spent) ends the process with 0xC0000005, and
 * one nobody handles with 0xC00000FD. Measured on Windows 11 first (it exits
 * 36 there): the reservation's size from GetCurrentThreadStackLimits on
 * both kinds of thread; an overflow caught by a __except on the main thread
 * and on a 64 KiB thread, each going on after it; a deep recursion that
 * stays inside the reservation; and, in child processes, the unhandled and
 * the second overflow. Exit 36 iff all pass, else 100 + the first that
 * failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern uint64_t f_try(uint64_t (*fn)(void));

static volatile int g_depth;
static DWORD g_code;
static ULONG_PTR g_p0, g_p1;
static int g_calls;

LONG so_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)frame;
    g_calls++;
    g_code = ep->ExceptionRecord->ExceptionCode;
    g_p0 = ep->ExceptionRecord->ExceptionInformation[0];
    g_p1 = ep->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_EXECUTE_HANDLER;
}

/* 512 bytes and a call a level */
__attribute__((noinline)) static int recurse(int n) {
    volatile char pad[512];
    pad[0] = (char)n;
    g_depth = n;
    return recurse(n + 1) + pad[0];
}
static int g_limit;
__attribute__((noinline)) static int recurse_to(int n) {
    volatile char pad[512];
    pad[0] = (char)n;
    if (n >= g_limit) return n;
    return recurse_to(n + 1) + pad[0] - pad[0];
}
static uint64_t go(void) { recurse(0); return 1; }
static uint64_t go_to(void) { return (uint64_t)recurse_to(0); }

static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok) printf("        code %08lx p0 %llx p1 %llx depth %d calls %d\n", g_code,
                    (unsigned long long)g_p0, (unsigned long long)g_p1, g_depth, g_calls);
    if (!ok && !first_fail) first_fail = n;
}

/* the thread: its reservation, a deep but legal recursion, an overflow caught */
static ULONG_PTR g_t_lo, g_t_hi;
static uint64_t g_t_ok_depth, g_t_r;
static int g_t_depth;
static DWORD WINAPI t_main(void *p) {
    (void)p;
    GetCurrentThreadStackLimits(&g_t_lo, &g_t_hi);
    g_limit = 40;                             /* 40 * ~560 bytes: ~22 KiB of 64 */
    g_t_ok_depth = f_try(go_to);
    g_calls = 0;
    g_t_r = f_try(go);
    g_t_depth = g_depth;
    return 5;
}

/* the children */
static DWORD WINAPI t_child(void *p) { (void)p; recurse(0); return 0; }
static int child(const char *m) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    say("child\n");
    if (!strcmp(m, "un")) recurse(0);
    else if (!strcmp(m, "th")) {
        HANDLE h = CreateThread(0, 64 * 1024, t_child, 0, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
        WaitForSingleObject(h, 20000);
        say("main: the thread ended\n");
        return 3;
    } else if (!strcmp(m, "2nd")) {
        if (f_try(go) == STATUS_STACK_OVERFLOW) say("caught the first\n");
        recurse(0);
    }
    say("child: went on\n");
    return 4;
}
static int run_child(const char *self, const char *args, char *out, int cap, DWORD *code) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    char cmd[400];
    strcpy(cmd, "\"");
    strcat(cmd, self);
    strcat(cmd, "\" ");
    strcat(cmd, args);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return -1; }
    CloseHandle(wr);
    int n = 0;
    for (;;) {
        DWORD got = 0;
        if (n >= cap - 1 || !ReadFile(rd, out + n, (DWORD)(cap - 1 - n), &got, 0) || !got) break;
        n += (int)got;
    }
    out[n] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1]);
    printf("win-so35: stack overflow\n");
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(0);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)dos + dos->e_lfanew);
    ULONG64 reserve = nt->OptionalHeader.SizeOfStackReserve;
    ULONG_PTR here = (ULONG_PTR)&lo;
    check(1, hi - lo == reserve && here > lo && here < hi,
          "the main thread's stack is the .exe's SizeOfStackReserve (GetCurrentThreadStackLimits)");

    g_limit = 2000;                           /* ~1.1 MiB of 2 */
    uint64_t okd = f_try(go_to);
    check(2, okd == 2000, "a recursion of ~1.1 MiB on the main thread stays inside it");

    g_calls = 0;
    uint64_t r = f_try(go);
    check(3, r == STATUS_STACK_OVERFLOW && g_calls == 1 && g_code == STATUS_STACK_OVERFLOW && g_p0 == 1 &&
             g_p1 >= lo && g_p1 < lo + 0x8000 && g_depth > 3000,
          "running off the main thread's stack: 0xC00000FD {1, an address within 32 KiB of its end}, caught, and on");

    HANDLE th = CreateThread(0, 64 * 1024, t_main, 0, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
    WaitForSingleObject(th, 20000);
    DWORD tcode = 0;
    GetExitCodeThread(th, &tcode);
    check(4, g_t_hi - g_t_lo == 64 * 1024, "a thread made with a 64 KiB reservation: GetCurrentThreadStackLimits says 64 KiB");
    check(5, g_t_ok_depth == 40 && g_t_r == STATUS_STACK_OVERFLOW && g_code == STATUS_STACK_OVERFLOW &&
             g_p1 >= g_t_lo && g_p1 < g_t_lo + 0x8000 && g_t_depth > 60 && tcode == 5,
          "on that thread: 22 KiB fine, then 0xC00000FD near its end, caught, and the thread ends normally");

    char self[MAX_PATH];
    GetModuleFileNameA(0, self, sizeof self);
    static const struct { const char *args, *out; DWORD code; } ch[] = {
        { "un", "child\n", 0xC00000FDu },
        { "th", "child\n", 0xC00000FDu },
        { "2nd", "child\ncaught the first\n", 0xC0000005u },
    };
    int ok6 = 1;
    for (unsigned i = 0; i < sizeof ch / sizeof ch[0]; i++) {
        char out[256];
        DWORD code = 0x12345;
        int n = run_child(self, ch[i].args, out, sizeof out, &code);
        int ok = n >= 0 && strcmp(out, ch[i].out) == 0 && code == ch[i].code;
        printf("     child %-3s exit 0x%08lx %s\n", ch[i].args, code, ok ? "" : "(not as on Windows)");
        if (!ok) ok6 = 0;
    }
    check(6, ok6, "an overflow nobody handles ends the process with 0xC00000FD (either thread); a second one, 0xC0000005");

    printf("win-so35: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 36;
}
