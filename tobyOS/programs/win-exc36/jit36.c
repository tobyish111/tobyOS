/* win-jit36 -- Track C, W7e: code made at run time. VirtualAlloc gives the
 * memory (reserved, committed, decommitted, released -- each state as
 * VirtualQuery reports it, an access to memory that is not committed an
 * access violation), the program writes a function into it -- push rbx; sub
 * rsp,0x20; mov rbx,0xDEAD; call rcx; ... -- and its unwind information, and
 * tells the system with RtlAddFunctionTable, RtlInstallFunctionTableCallback
 * or the growable tables; an exception raised (or a fault) under the
 * generated frame is then caught above it, rbx restored by the unwind
 * through it. And children that end without releasing what they reserved
 * leave nothing behind for the next. Each check what Windows 11 does
 * (measured first: it exits 37 there). Exit 37 iff all pass, else 100 + the
 * first that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern uint64_t f_try_rbx(uint64_t (*fn)(void *), void *arg, uint64_t *rbx_out);

static DWORD g_code;
LONG jit_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)frame;
    g_code = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}
static void raiser(void) { RaiseException(0xE0000002, 0, 0, 0); }
static void faulter(void) { *(volatile int *)0 = 1; }

/* push rbx; sub rsp,0x20; mov rbx,0xDEAD; call rcx; nop; add rsp,0x20; pop rbx; ret */
static const uint8_t g_fn_bytes[] = {
    0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xBB, 0xAD, 0xDE, 0, 0, 0, 0, 0, 0,
    0xFF, 0xD1, 0x90, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
};
/* UNWIND_INFO: version 1, prolog 5 bytes, 2 codes: [5] ALLOC_SMALL 0x20, [1] PUSH_NONVOL rbx */
static const uint8_t g_unwind[] = { 0x01, 0x05, 0x02, 0x00, 0x05, 0x32, 0x01, 0x30 };

static uint8_t *make(uint8_t *buf, RUNTIME_FUNCTION *rf) {
    memcpy(buf + 0x100, g_fn_bytes, sizeof g_fn_bytes);
    memcpy(buf + 0x200, g_unwind, sizeof g_unwind);
    rf->BeginAddress = 0x100;
    rf->EndAddress = 0x100 + sizeof g_fn_bytes;
    rf->UnwindData = 0x200;
    return buf + 0x100;
}
static int through(uint8_t *fn, void (*inner)(void), DWORD want) {
    uint64_t rbx = 0;
    g_code = 0;
    uint64_t r = f_try_rbx((uint64_t (*)(void *))fn, (void *)inner, &rbx);
    return r == want && g_code == want && rbx == 0x5C0FED0B5C0FEDULL;
}

static int g_veh_calls;
static ULONG_PTR g_veh_p0, g_veh_p1;
static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    g_veh_calls++;
    g_veh_p0 = ep->ExceptionRecord->ExceptionInformation[0];
    g_veh_p1 = ep->ExceptionRecord->ExceptionInformation[1];
    ep->ContextRecord->Rip += 3;               /* the 3-byte access in touch() */
    return EXCEPTION_CONTINUE_EXECUTION;
}
extern void touch_read(void *p);
__asm__(".globl touch_read\ntouch_read:\n\tmovq (%rcx), %rax\n\tret\n");

static RUNTIME_FUNCTION g_cb_rf;
static int g_cb_calls;
static PRUNTIME_FUNCTION CALLBACK cb(DWORD64 pc, PVOID ctx) { (void)pc; g_cb_calls++; return (PRUNTIME_FUNCTION)ctx; }

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

/* 10: a child reserves 400 regions of 64 KiB and ends without releasing
 * them; its exit code is how many it got */
static int child_reserve(void) {
    int n = 0;
    for (int i = 0; i < 400; i++)
        if (VirtualAlloc(0, 0x10000, MEM_RESERVE, PAGE_NOACCESS)) n++;
    return n;
}
static DWORD run_child(const char *self, const char *arg) {
    char cmd[400];
    strcpy(cmd, "\"");
    strcat(cmd, self);
    strcat(cmd, "\" ");
    strcat(cmd, arg);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) return 0xFFFFFFFFu;
    DWORD code = 0x12345;
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

int main(int argc, char **argv) {
    if (argc > 1) return child_reserve();
    printf("win-jit36: code made at run time\n");
    MEMORY_BASIC_INFORMATION mbi;

    uint8_t *buf = VirtualAlloc(0, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int zero = 1;
    for (int i = 0; buf && i < 0x10000; i += 512) if (buf[i]) zero = 0;
    SIZE_T q = buf ? VirtualQuery(buf + 0x123, &mbi, sizeof mbi) : 0;
    check(1, buf && ((uintptr_t)buf & 0xFFFF) == 0 && zero && q == sizeof mbi && mbi.BaseAddress == buf &&
             mbi.AllocationBase == buf && mbi.RegionSize == 0x10000 && mbi.State == MEM_COMMIT &&
             mbi.Protect == PAGE_EXECUTE_READWRITE && mbi.AllocationProtect == PAGE_EXECUTE_READWRITE &&
             mbi.Type == MEM_PRIVATE,
          "VirtualAlloc(64 KiB, commit+reserve, RWX): 64 KiB-aligned, zeroed; VirtualQuery says so");

    uint8_t *res = VirtualAlloc(0, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    q = res ? VirtualQuery(res + 0x5000, &mbi, sizeof mbi) : 0;
    /* BaseAddress is the queried address's page; RegionSize runs from there
     * to the end of the pages alike (measured) */
    int ok2 = res && ((uintptr_t)res & 0xFFFF) == 0 && q && mbi.AllocationBase == res &&
              mbi.BaseAddress == res + 0x5000 && mbi.RegionSize == 0x100000 - 0x5000 &&
              mbi.State == MEM_RESERVE && mbi.Protect == 0 &&
              mbi.AllocationProtect == PAGE_NOACCESS && mbi.Type == MEM_PRIVATE;
    uint8_t *com = res ? VirtualAlloc(res + 0x4000, 0x2000, MEM_COMMIT, PAGE_READWRITE) : 0;
    if (com) com[0] = 7, com[0x1FFF] = 9;
    q = com ? VirtualQuery(com + 0x10, &mbi, sizeof mbi) : 0;
    ok2 = ok2 && com == res + 0x4000 && q && mbi.BaseAddress == com && mbi.AllocationBase == res &&
          mbi.RegionSize == 0x2000 && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE &&
          com[0] == 7 && com[0x1FFF] == 9;
    check(2, ok2, "reserve 1 MiB, commit 8 KiB in it: each region's state, base and size as VirtualQuery gives them");

    void *vh = AddVectoredExceptionHandler(1, veh);
    g_veh_calls = 0;
    touch_read(res + 0x8000);
    int ok3 = g_veh_calls == 1 && g_veh_p0 == 0 && g_veh_p1 == (ULONG_PTR)(res + 0x8000);
    BOOL dc = VirtualFree(com, 0x1000, MEM_DECOMMIT);
    q = VirtualQuery(com, &mbi, sizeof mbi);
    ok3 = ok3 && dc && mbi.State == MEM_RESERVE && mbi.BaseAddress == com;
    BOOL rl = VirtualFree(res, 0, MEM_RELEASE);
    q = VirtualQuery(res + 0x4000, &mbi, sizeof mbi);
    ok3 = ok3 && rl && mbi.State == MEM_FREE;
    g_veh_calls = 0;
    touch_read(res + 0x4000);
    ok3 = ok3 && g_veh_calls == 1 && g_veh_p1 == (ULONG_PTR)(res + 0x4000);
    RemoveVectoredExceptionHandler(vh);
    check(3, ok3, "a read of reserved memory faults; decommit leaves it reserved; release frees it (and it faults)");

    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(buf + 0x300);
    uint8_t *fn = make(buf, rf);
    DWORD64 base = 0;
    PRUNTIME_FUNCTION e0 = RtlLookupFunctionEntry((DWORD64)fn + 0x10, &base, 0);
    BOOLEAN added = RtlAddFunctionTable(rf, 1, (DWORD64)buf);
    PRUNTIME_FUNCTION e1 = RtlLookupFunctionEntry((DWORD64)fn + 0x10, &base, 0);
    check(4, !e0 && added && e1 == rf && base == (DWORD64)buf,
          "RtlAddFunctionTable: the generated function's entry, found, with its base");
    check(5, through(fn, raiser, 0xE0000002) && through(fn, faulter, 0xC0000005),
          "a raise and a fault under the generated frame, caught above it, rbx restored through it");
    BOOLEAN d1 = RtlDeleteFunctionTable(rf);
    PRUNTIME_FUNCTION e2 = RtlLookupFunctionEntry((DWORD64)fn + 0x10, &base, 0);
    BOOLEAN d2 = RtlDeleteFunctionTable(rf);
    check(6, d1 && !e2 && !d2, "RtlDeleteFunctionTable: TRUE, the entry gone; again: FALSE");

    uint8_t *buf2 = buf + 0x8000;
    uint8_t *fn2 = make(buf2, &g_cb_rf);
    BOOLEAN bad = RtlInstallFunctionTableCallback((DWORD64)buf2, (DWORD64)buf2, 0x1000, cb, &g_cb_rf, 0);
    BOOLEAN ic = RtlInstallFunctionTableCallback((DWORD64)buf2 | 3, (DWORD64)buf2, 0x1000, cb, &g_cb_rf, 0);
    g_cb_calls = 0;
    PRUNTIME_FUNCTION e3 = RtlLookupFunctionEntry((DWORD64)fn2 + 0x10, &base, 0);
    int ok7 = !bad && ic && e3 == &g_cb_rf && base == (DWORD64)buf2 && g_cb_calls == 1 &&
              through(fn2, raiser, 0xE0000002);
    BOOLEAN dcb = RtlDeleteFunctionTable((PRUNTIME_FUNCTION)((DWORD64)buf2 | 3));
    PRUNTIME_FUNCTION e3b = RtlLookupFunctionEntry((DWORD64)fn2 + 0x10, &base, 0);
    check(7, ok7 && dcb && !e3b,
          "RtlInstallFunctionTableCallback: an id without its low bits refused; the callback answers lookups; deleted by its id");

    uint8_t *buf3 = buf + 0x4000;
    RUNTIME_FUNCTION *rf3 = (RUNTIME_FUNCTION *)(buf3 + 0x300);
    uint8_t *fn3 = make(buf3, rf3);
    PVOID h = 0;
    DWORD st = RtlAddGrowableFunctionTable(&h, rf3, 0, 4, (ULONG_PTR)buf3, (ULONG_PTR)buf3 + 0x1000);
    PRUNTIME_FUNCTION e4 = RtlLookupFunctionEntry((DWORD64)fn3 + 0x10, &base, 0);
    RtlGrowFunctionTable(h, 1);
    PRUNTIME_FUNCTION e5 = RtlLookupFunctionEntry((DWORD64)fn3 + 0x10, &base, 0);
    int ok8 = st == 0 && h && !e4 && e5 == rf3 && base == (DWORD64)buf3 && through(fn3, raiser, 0xE0000002);
    RtlDeleteGrowableFunctionTable(h);
    PRUNTIME_FUNCTION e6 = RtlLookupFunctionEntry((DWORD64)fn3 + 0x10, &base, 0);
    check(8, ok8 && !e6, "the growable tables: nothing until RtlGrowFunctionTable, then found and unwound through; deleted");

    check(9, VirtualFree(buf, 0, MEM_RELEASE) && !VirtualAlloc(0, 0, MEM_COMMIT, PAGE_READWRITE) &&
             GetLastError() == ERROR_INVALID_PARAMETER,
          "VirtualFree(MEM_RELEASE): TRUE; VirtualAlloc of 0 bytes: NULL, ERROR_INVALID_PARAMETER");

    char self[MAX_PATH];
    GetModuleFileNameA(0, self, sizeof self);
    DWORD c1 = run_child(self, "res"), c2 = run_child(self, "res"), c3 = run_child(self, "res");
    check(10, c1 == 400 && c2 == 400 && c3 == 400,
          "three children in turn each reserve 400 regions and end without releasing them: each gets all 400");
    if (c1 != 400 || c2 != 400 || c3 != 400) printf("        children got %lu, %lu, %lu\n", c1, c2, c3);

    printf("win-jit36: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 37;
}
