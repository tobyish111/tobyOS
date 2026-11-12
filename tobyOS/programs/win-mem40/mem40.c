/* win-mem40 -- Track C, W7i: what is left of VirtualAlloc's memory. Guard
 * pages (PAGE_GUARD: the first touch a STATUS_GUARD_PAGE_VIOLATION, once,
 * and a ReadFile or WriteFile on one failing), write watch (MEM_WRITE_WATCH,
 * GetWriteWatch, ResetWriteWatch), MEM_RESET, VirtualProtect on pages that
 * are not committed, a touched page made PAGE_NOACCESS, more reservations
 * than the 1024 W7e kept, and _resetstkoflw (an overflow caught, the guard
 * re-armed, the next overflow caught again). Each check what Windows 11 does
 * (measured first: it exits 42 there). Exit 42 iff all pass, else 100 + the
 * first that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#define STATUS_GUARD 0x80000001u

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

/* ---- the exceptions a touch raises ---- */
static int g_calls;
static DWORD g_code;
static ULONG_PTR g_p0, g_p1;
static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != STATUS_GUARD && c != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    g_calls++;
    g_code = c;
    g_p0 = ep->ExceptionRecord->ExceptionInformation[0];
    g_p1 = ep->ExceptionRecord->ExceptionInformation[1];
    if (c == 0xC0000005) ep->ContextRecord->Rip += 3;      /* past touch_*'s 3-byte access */
    return EXCEPTION_CONTINUE_EXECUTION;
}
extern uint64_t touch_read(void *p);
extern void touch_write(void *p);
__asm__(".globl touch_read\ntouch_read:\n\tmovq (%rcx), %rax\n\tret\n");
__asm__(".globl touch_write\ntouch_write:\n\tmovq %rdx, (%rcx)\n\tret\n");
static void reset_x(void) { g_calls = 0; g_code = 0; g_p0 = g_p1 = 0x5A5A; }
static int one(DWORD code, ULONG_PTR p0, void *addr) {
    return g_calls == 1 && g_code == code && g_p0 == p0 && g_p1 == (ULONG_PTR)addr;
}

static MEMORY_BASIC_INFORMATION g_m;
static int vq(void *p, SIZE_T size, DWORD state, DWORD prot) {
    memset(&g_m, 0, sizeof g_m);
    return VirtualQuery(p, &g_m, sizeof g_m) == sizeof g_m && g_m.BaseAddress == p &&
           g_m.RegionSize == size && g_m.State == state && g_m.Protect == prot;
}

/* ---- write watch ---- */
static void *g_wa[64];
static ULONG_PTR g_wn;
static UINT ww(DWORD fl, void *b, SIZE_T sz, ULONG_PTR cap) {
    ULONG gran = 0;
    g_wn = cap;
    SetLastError(0);
    UINT u = GetWriteWatch(fl, b, sz, g_wa, &g_wn, &gran);
    if (u == 0 && gran != 4096) return 77;
    return u;
}
/* the pages written, as offsets from b in pages: "1 3 5" */
static const char *pages(void *b) {
    static char s[256];
    s[0] = 0;
    for (ULONG_PTR i = 0; i < g_wn && i < 64; i++) {
        char t[16];
        snprintf(t, sizeof t, "%s%llu", i ? " " : "", (unsigned long long)(((uint8_t *)g_wa[i] - (uint8_t *)b) / 4096));
        strcat(s, t);
    }
    return s;
}

/* ---- stack overflow, caught ---- */
extern uint64_t f_try(uint64_t (*fn)(void));
static volatile int g_depth;
LONG so_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)ep; (void)frame;
    return EXCEPTION_EXECUTE_HANDLER;
}
__attribute__((noinline)) static int recurse(int n) {
    volatile char pad[512];
    pad[0] = (char)n;
    g_depth = n;
    return recurse(n + 1) + pad[0];
}
static uint64_t go(void) { recurse(0); return 1; }
static int g_t_ok;
static DWORD WINAPI t_rounds(void *a) {
    (void)a;
    int d0 = -1, ok = 1;
    for (int i = 0; i < 3; i++) {
        uint64_t r = f_try(go);
        int rs = _resetstkoflw();
        if (i == 0) d0 = g_depth;
        printf("    thread round %d: %llx depth %d reset %d\n", i, (unsigned long long)r, g_depth, rs);
        ok &= r == 0xC00000FD && rs == 1 && g_depth == d0;
    }
    g_t_ok = ok;
    return 0;
}

static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}
static char g_self[MAX_PATH];             /* the .exe's own path, a file to read */
static DWORD run_self(const char *self, const char *arg) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", self, arg);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) return 0xDEAD;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD ec = 0xBEEF;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ec;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "unh")) {             /* a guard page nobody handles */
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        uint8_t *g = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
        say("child: touching\n");
        touch_read(g);
        say("child: went on\n");
        return 5;
    }
    AddVectoredExceptionHandler(1, veh);
    GetModuleFileNameA(0, g_self, sizeof g_self);
    DWORD old;
    BOOL b;

    /* 1: a guard committed by VirtualAlloc: each page's first touch */
    {
        uint8_t *g = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
        int ok = g && vq(g, 0x10000, MEM_COMMIT, 0x104) && g_m.AllocationProtect == 0x104;
        reset_x(); touch_read(g + 8);
        ok &= one(STATUS_GUARD, 0, g + 8);
        ok &= vq(g, 0x1000, MEM_COMMIT, PAGE_READWRITE) && vq(g + 0x1000, 0xF000, MEM_COMMIT, 0x104);
        reset_x(); touch_read(g + 8);
        ok &= g_calls == 0;
        reset_x(); touch_write(g + 0x1010);
        ok &= one(STATUS_GUARD, 1, g + 0x1010) && vq(g + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        check(1, ok, "PAGE_GUARD: a page's first read/write raises 0x80000001 {0/1, the address} once, the page then PAGE_READWRITE");
    }
    /* 2: an execute guard set by VirtualProtect */
    {
        uint8_t *x = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        x[0] = 0xC3;                                       /* ret */
        b = VirtualProtect(x, 0x1000, PAGE_EXECUTE_READ | PAGE_GUARD, &old);
        int ok = b && old == PAGE_EXECUTE_READWRITE && vq(x, 0x1000, MEM_COMMIT, 0x120);
        reset_x();
        ((void (*)(void))x)();
        ok &= one(STATUS_GUARD, 8, x);
        b = VirtualProtect(x, 0x1000, PAGE_EXECUTE_READ | PAGE_GUARD, &old);
        ok &= b && old == PAGE_EXECUTE_READ;
        b = VirtualProtect(x, 0x1000, PAGE_EXECUTE_READ, &old);
        ok &= b && old == 0x120;
        check(2, ok, "an execute guard: {8, the address}; VirtualProtect's old protection with and without PAGE_GUARD");
    }
    /* 3: a page that was touched, then guarded -- and one made PAGE_NOACCESS */
    {
        uint8_t *t = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        memset(t, 0x77, 0x3000);
        int bad = 0;                                       /* which part failed, a bit each */
        b = VirtualProtect(t + 0x1000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
        bad |= !(b && old == PAGE_READWRITE) << 0;
        reset_x();
        uint64_t v = touch_read(t + 0x1000);
        bad |= !(one(STATUS_GUARD, 0, t + 0x1000) && v == 0x7777777777777777ull) << 1;
        b = VirtualProtect(t + 0x2000, 0x1000, PAGE_NOACCESS, &old);
        bad |= !(b && vq(t + 0x2000, 0x1000, MEM_COMMIT, PAGE_NOACCESS)) << 2;
        reset_x(); touch_read(t + 0x2000);
        bad |= !one(0xC0000005, 0, t + 0x2000) << 3;
        reset_x(); touch_write(t + 0x2000);
        bad |= !one(0xC0000005, 1, t + 0x2000) << 4;
        b = VirtualProtect(t + 0x2000, 0x1000, PAGE_READONLY, &old);
        reset_x();
        v = touch_read(t + 0x2000);
        bad |= !(b && old == PAGE_NOACCESS && g_calls == 0 && v == 0x7777777777777777ull) << 5;
        if (bad) printf("    parts failed: %x (calls %d code %lx p0 %llx)\n", bad, g_calls, g_code, (unsigned long long)g_p0);
        check(3, !bad, "a touched page guarded raises on its next read; one made PAGE_NOACCESS faults on read and write, its contents kept");
    }
    /* 4: what PAGE_GUARD may not go with */
    {
        int ok = 1;
        SetLastError(0);
        ok &= !VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS | PAGE_GUARD) && GetLastError() == 87;
        uint8_t *r = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        SetLastError(0);
        ok &= !VirtualProtect(r, 0x1000, PAGE_NOACCESS | PAGE_GUARD, &old) && GetLastError() == 87;
        SetLastError(0);
        ok &= !VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_WRITECOPY) && GetLastError() == 87;
        SetLastError(0);
        uint8_t *wg = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE | PAGE_GUARD);
        ok &= wg && vq(wg, 0x10000, MEM_COMMIT, 0x104);
        uint8_t *gr = VirtualAlloc(0, 0x10000, MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD);
        ok &= gr && vq(gr, 0x10000, MEM_RESERVE, 0) && g_m.AllocationProtect == 0x104;
        void *c = gr ? VirtualAlloc(gr + 0x2000, 0x1000, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD) : 0;
        ok &= c == gr + 0x2000 && vq(gr + 0x2000, 0x1000, MEM_COMMIT, 0x104) && vq(gr, 0x2000, MEM_RESERVE, 0);
        check(4, ok, "PAGE_GUARD with PAGE_NOACCESS, or PAGE_WRITECOPY: 87; a guarded write watch, a guarded reservation, a guard committed in one");
    }
    /* 5: a ReadFile or WriteFile on a guard page */
    {
        uint8_t *g = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
        HANDLE h = CreateFileA(g_self, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        DWORD got = 77;
        reset_x();
        SetLastError(0);
        b = ReadFile(h, g, 64, &got, 0);
        int bad = 0;                                       /* which part failed, a bit each */
        bad |= !(!b && got == 0 && GetLastError() == STATUS_GUARD && g_calls == 0 &&
                 vq(g, 0x1000, MEM_COMMIT, PAGE_READWRITE)) << 0;
        b = ReadFile(h, g, 64, &got, 0);
        bad |= !(b && got == 64 && g[0] == 'M' && g[1] == 'Z') << 1;
        /* page 0 plain, pages 1 and 2 guarded: nothing read, page 1's guard taken */
        uint8_t *s = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        VirtualProtect(s + 0x1000, 0x2000, PAGE_READWRITE | PAGE_GUARD, &old);
        memset(s, 0, 0x1000);
        SetFilePointer(h, 0, 0, FILE_BEGIN);
        got = 77;
        SetLastError(0);
        b = ReadFile(h, s + 0xF00, 0x2000, &got, 0);
        bad |= !(!b && got == 0 && GetLastError() == STATUS_GUARD && s[0xF00] == 0 &&
              vq(s + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE) && vq(s + 0x2000, 0x1000, MEM_COMMIT, 0x104)) << 2;
        CloseHandle(h);
        uint8_t *w = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
        char tmp[MAX_PATH];
        GetTempPathA(sizeof tmp, tmp);
        strcat(tmp, "w-mem40.tmp");
        HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
        DWORD put = 77;
        SetLastError(0);
        b = WriteFile(f, w, 16, &put, 0);
        bad |= !(f != INVALID_HANDLE_VALUE && !b && put == 0 && GetLastError() == 1784 && g_calls == 0 &&
              vq(w, 0x1000, MEM_COMMIT, PAGE_READWRITE)) << 3;
        b = WriteFile(f, w, 16, &put, 0);
        bad |= !(b && put == 16) << 4;
        CloseHandle(f);
        DeleteFileA(tmp);
        if (bad) printf("    parts failed: %x\n", bad);
        check(5, !bad, "ReadFile into a guard page: 0x80000001 as the last error, WriteFile from one 1784; nothing raised, the first guard page taken");
    }
    /* 6: a guard page nobody handles */
    {
        DWORD ec = run_self(argv[0], "unh");
        printf("    child exit %lx\n", ec);
        check(6, ec == STATUS_GUARD, "an unhandled guard page violation ends the process with 0x80000001");
    }
    /* 7: VirtualProtect on pages that are not committed */
    {
        uint8_t *r = VirtualAlloc(0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
        VirtualAlloc(r, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        int ok = 1;
        old = 0x12345678; SetLastError(0);
        ok &= !VirtualProtect(r + 0x1000, 0x1000, PAGE_READWRITE, &old) && GetLastError() == 487 && old == PAGE_NOACCESS;
        old = 0x12345678; SetLastError(0);
        ok &= !VirtualProtect(r, 0x2000, PAGE_READONLY, &old) && GetLastError() == 487 &&
              vq(r, 0x1000, MEM_COMMIT, PAGE_READWRITE) && vq(r + 0x1000, 0xF000, MEM_RESERVE, 0);
        old = 0x12345678; SetLastError(0);
        ok &= !VirtualProtect(r, 0x1000, 0x55, &old) && GetLastError() == 87 && old == 0x12345678;
        uint8_t *c = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        ok &= vq(c, 0x10000, MEM_COMMIT, PAGE_NOACCESS);
        ok &= VirtualProtect(c, 0x1000, PAGE_READWRITE, &old) && old == PAGE_NOACCESS;
        VirtualFree(c + 0x2000, 0x1000, MEM_DECOMMIT);
        SetLastError(0);
        ok &= !VirtualProtect(c + 0x2000, 0x1000, PAGE_READWRITE, &old) && GetLastError() == 487 &&
              vq(c + 0x2000, 0x1000, MEM_RESERVE, 0);
        SetLastError(0);
        ok &= !VirtualProtect((void *)0x7ff000000000ull, 0x1000, PAGE_READWRITE, &old) && GetLastError() == 487;
        /* a commit over committed pages: the new protection, the contents kept */
        uint8_t *k = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        k[0] = 7;
        ok &= VirtualAlloc(k, 0x1000, MEM_COMMIT, PAGE_READONLY) == k && k[0] == 7 && vq(k, 0x1000, MEM_COMMIT, PAGE_READONLY);
        check(7, ok, "VirtualProtect on reserved, decommitted or free pages: 487 (old = PAGE_NOACCESS), nothing changed; committed PAGE_NOACCESS is committed");
    }
    /* 8: MEM_RESET */
    {
        uint8_t *m = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        memset(m, 0x5A, 0x10000);
        int ok = 1;
        void *x;
#define RS(addr, size, type, prot, want, err) \
        (SetLastError(0), x = VirtualAlloc((addr), (size), (type), (prot)), \
         x == (void *)(want) && (x || GetLastError() == (err)))
        ok &= RS(m + 0x1000, 0x2000, MEM_RESET, PAGE_READWRITE, m + 0x1000, 0) && vq(m + 0x1000, 0xF000, MEM_COMMIT, PAGE_READWRITE);
        ok &= RS(m + 0x1000, 0x2000, MEM_RESET, PAGE_NOACCESS, m + 0x1000, 0);
        ok &= RS(m + 0x1000, 0x2000, MEM_RESET, PAGE_READWRITE | PAGE_GUARD, m + 0x1000, 0);
        ok &= RS(m + 0x1000, 0x2000, MEM_RESET, 0, 0, 87);
        ok &= RS(m + 0x1000, 0x2000, MEM_RESET | MEM_COMMIT, PAGE_READWRITE, 0, 87);
        ok &= RS(m, 0x1000, MEM_RESET | MEM_RESET_UNDO, PAGE_READWRITE, 0, 87);
        ok &= RS(m + 0x1000, 0x1800, MEM_RESET, PAGE_READWRITE, m + 0x1000, 0);
        ok &= RS(m + 0x1800, 0x1000, MEM_RESET, PAGE_READWRITE, 0, 487);
        ok &= RS(m + 0x1800, 0x2000, MEM_RESET, PAGE_READWRITE, m + 0x2000, 0);
        ok &= RS(m + 0x1000, 0x10000, MEM_RESET, PAGE_READWRITE, 0, 487);
        ok &= RS(m, 0, MEM_RESET, PAGE_READWRITE, 0, 87);
        ok &= RS(0, 0x1000, MEM_RESET, PAGE_READWRITE, 0, 487);
        ok &= RS(m + 0x1000, 0x2000, MEM_RESET_UNDO, PAGE_READWRITE, m + 0x1000, 0);
        uint8_t *rv = VirtualAlloc(0, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
        ok &= RS(rv, 0x1000, MEM_RESET, PAGE_READWRITE, rv, 0);
        ok &= RS((uint8_t *)GetModuleHandleA(0) + 0x1000, 0x1000, MEM_RESET, PAGE_READWRITE, 0, 1224);
        ok &= RS((void *)0x7ff000000000ull, 0x1000, MEM_RESET, PAGE_READWRITE, 0, 487);
        check(8, ok, "MEM_RESET and MEM_RESET_UNDO: alone, the range rounded in, in private memory (an image: 1224, nothing: 487)");
    }
    /* 9: a write watch -- what counts as written */
    {
        uint8_t *w = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        int ok = w && vq(w, 0x10000, MEM_COMMIT, PAGE_READWRITE);
        ok &= ww(0, w, 0x10000, 64) == 0 && g_wn == 0;
        w[0x1000] = 1; w[0x3008] = 2; (void)*(volatile uint8_t *)(w + 0x5000);
        ok &= ww(0, w, 0x10000, 64) == 0 && !strcmp(pages(w), "1 3 5");
        printf("    written: %s\n", pages(w));
        ok &= ww(0, w, 0x10000, 1) == 0 && !strcmp(pages(w), "1");
        ok &= ww(0, w + 0x2000, 0x4000, 64) == 0 && !strcmp(pages(w), "3 5");
        ok &= ww(0, w + 0x2800, 0x1000, 64) == 0 && !strcmp(pages(w), "3");
        ok &= ResetWriteWatch(w, 0x10000) == 0 && ww(0, w, 0x10000, 64) == 0 && g_wn == 0;
        (void)*(volatile uint8_t *)(w + 0x1000);           /* a read of a page that is there */
        ok &= ww(0, w, 0x10000, 64) == 0 && g_wn == 0;
        HANDLE h = CreateFileA(g_self, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        DWORD got = 0;
        ReadFile(h, w + 0x9000, 64, &got, 0);              /* the system's write */
        CloseHandle(h);
        w[0xA000] = 4;
        VirtualProtect(w + 0xA000, 0x1000, PAGE_READONLY, &old);
        VirtualProtect(w + 0xA000, 0x1000, PAGE_READWRITE, &old);
        ok &= ww(0, w, 0x10000, 64) == 0 && !strcmp(pages(w), "9 10");
        printf("    written: %s\n", pages(w));
        VirtualFree(w + 0xA000, 0x1000, MEM_DECOMMIT);     /* written, then decommitted */
        VirtualFree(w + 0xB000, 0x1000, MEM_DECOMMIT);     /* clean, decommitted */
        ok &= ww(0, w, 0x10000, 64) == 0 && !strcmp(pages(w), "9 10");
        VirtualAlloc(w + 0xB000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        ok &= ww(0, w, 0x10000, 64) == 0 && !strcmp(pages(w), "9 10");
        (void)*(volatile uint8_t *)(w + 0xB000);
        ok &= ww(0, w, 0x10000, 64) == 0 && !strcmp(pages(w), "9 10 11");
        uint8_t *ro = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READONLY);
        (void)*(volatile uint8_t *)(ro + 0x1000);
        ok &= ro && ww(0, ro, 0x10000, 64) == 0 && g_wn == 0;
        uint8_t *w2 = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
        VirtualAlloc(w2, 0x2000, MEM_COMMIT, PAGE_READWRITE);
        w2[0x1000] = 9;
        ok &= w2 && ww(0, w2, 0x10000, 64) == 0 && !strcmp(pages(w2), "1");
        check(9, ok, "write watch: the pages written (by the program or ReadFile, a fresh page's first touch too), kept across VirtualProtect and decommit; reads not");
    }
    /* 10: a write watch -- resets and refusals */
    {
        uint8_t *w = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        w[0x1000] = 1; w[0x2000] = 1; w[0x3000] = 1;
        int ok = ww(WRITE_WATCH_FLAG_RESET, w, 0x10000, 1) == 0 && !strcmp(pages(w), "1");
        ok &= ww(0, w, 0x10000, 64) == 0 && !strcmp(pages(w), "2 3");
        ok &= ww(WRITE_WATCH_FLAG_RESET, w, 0x10000, 64) == 0 && !strcmp(pages(w), "2 3");
        ok &= ww(0, w, 0x10000, 64) == 0 && g_wn == 0;
        w[0x3000] = 2;
        ok &= ww(0, w, 0x10000, 64) == 0 && !strcmp(pages(w), "3");
        w[0x2000] = 2;
        ok &= ResetWriteWatch(w + 0x2800, 0x1000) == 0 && ww(0, w, 0x10000, 64) == 0 && g_wn == 0;
        uint8_t *nw = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        ok &= ww(0, nw, 0x10000, 64) == 0xFFFFFFFFu && GetLastError() == 87;
        SetLastError(0);
        ok &= ResetWriteWatch(nw, 0x10000) == 0xFFFFFFFFu && GetLastError() == 87;
        ok &= ww(0, w, 0x20000, 64) == 0xFFFFFFFFu && GetLastError() == 87;
        ok &= ww(0, w, 0, 64) == 0xFFFFFFFFu && GetLastError() == 87;
        ok &= ww(0, w, 0x10000, 0) == 0xFFFFFFFFu && GetLastError() == 87;
        ULONG_PTR cnt = 8;
        ULONG gran;
        SetLastError(0);
        ok &= GetWriteWatch(0, w, 0x10000, 0, &cnt, &gran) == 0xFFFFFFFFu && GetLastError() == 998;
        SetLastError(0);
        ok &= !VirtualAlloc(0, 0x10000, MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE) && GetLastError() == 87;
        SetLastError(0);
        ok &= !VirtualAlloc(w, 0x1000, MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE) && GetLastError() == 87;
        VirtualFree(w, 0, MEM_RELEASE);
        ok &= ww(0, w, 0x10000, 64) == 0xFFFFFFFFu && GetLastError() == 87;
        check(10, ok, "write watch: WRITE_WATCH_FLAG_RESET resets the pages it returns, ResetWriteWatch a range; not a watch, past its end, no count: 87, no array: 998");
    }
    /* 11: more reservations than W7e's 1024 */
    {
        static void *r[1500];
        int n = 0, freed = 0;
        for (int i = 0; i < 1500; i++) if ((r[i] = VirtualAlloc(0, 0x10000, MEM_RESERVE, PAGE_NOACCESS))) n++;
        for (int i = 0; i < 1500; i++) if (r[i] && VirtualFree(r[i], 0, MEM_RELEASE)) freed++;
        printf("    %d reserved, %d released\n", n, freed);
        check(11, n == 1500 && freed == 1500, "1500 reservations of 64 KiB, all released");
    }
    /* 12: _resetstkoflw on the main thread */
    {
        int ok = _resetstkoflw() == 1, d0 = -1;
        for (int i = 0; i < 3; i++) {
            uint64_t rr = f_try(go);
            int rs = _resetstkoflw();
            if (i == 0) d0 = g_depth;
            printf("    main round %d: %llx depth %d reset %d\n", i, (unsigned long long)rr, g_depth, rs);
            ok &= rr == 0xC00000FD && rs == 1 && g_depth == d0;
        }
        check(12, ok, "_resetstkoflw: 1 with nothing to do; after a caught overflow the next is caught again, at the same depth (main thread)");
    }
    /* 13: on a thread with a 64 KiB reservation */
    {
        HANDLE h = CreateThread(0, 64 * 1024, t_rounds, 0, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
        WaitForSingleObject(h, 60000);
        check(13, g_t_ok, "_resetstkoflw on a thread: three overflows, each caught");
    }
    printf("%s\n", first_fail ? "SOME FAILED" : "ALL PASS");
    return first_fail ? 100 + first_fail : 42;
}
