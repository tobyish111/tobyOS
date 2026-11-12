/* win-lj37 -- Track C, W7f: longjmp that unwinds, and the consolidation
 * unwind. mingw's setjmp is _setjmp(buf, its frame address); longjmp then
 * unwinds to that frame with RtlUnwindEx (STATUS_LONGJUMP, the jump buffer
 * its parameter) -- so a __finally between runs, even one in the setjmp
 * caller's own frame -- and the registers come back from the jump buffer. A
 * frame of 0 restores without an unwind; a frame that is no frame's
 * establisher ends the process with STATUS_BAD_STACK (one above every frame,
 * STATUS_BAD_FUNCTION_TABLE), after the unwind's __finally blocks -- which
 * the unhandled exception's unwind then runs again. RtlUnwindEx with
 * STATUS_UNWIND_CONSOLIDATE unwinds to its target, then calls the record's
 * callback below it, which says where to go on. Each check what Windows 11
 * does (measured first: it exits 38 there). Exit 38 iff all pass, else 100 +
 * the first that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>

extern void f_fin(void (*fn)(void));
extern int f_jmp_owner(jmp_buf jb, void (*fn)(void), int frame_mode);
extern int f_nv(jmp_buf jb, const uint64_t *in, uint64_t *out, void (*fn)(void));
extern void f_clobber_jump(void);
extern uint64_t c_outer(void);
extern char c_outer_landing[];

jmp_buf g_nv_jb;
int g_nv_mode;
ULONG_PTR g_high_frame;
uint64_t g_landing_rbx, g_landing_xmm6;

static int g_child;
static char g_log[256];
static void lg(const char *s) { strcat(g_log, s); }
static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}
static void hex(char *b, uint64_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15];
    b[digits] = 0;
}

void lj_finally(BOOLEAN ab, void *frame) {
    (void)frame;
    if (g_child) say(ab ? "finally(abnormal)\n" : "finally(normal)\n");
    else lg(ab ? "fin(ab) " : "fin(n) ");
}

static int g_vcalls;
static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    g_vcalls++;
    if (g_child) {
        char b[64];
        strcpy(b, "veh ");
        hex(b + 4, ep->ExceptionRecord->ExceptionCode, 8);
        strcat(b, " flags ");
        hex(b + strlen(b), ep->ExceptionRecord->ExceptionFlags, 2);
        strcat(b, " np ");
        hex(b + strlen(b), ep->ExceptionRecord->NumberParameters, 2);
        strcat(b, "\n");
        say(b);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

/* 1: the plain case */
static jmp_buf g_jb;
static int g_val, g_zero_seen;
static __attribute__((noinline)) void jump_val(void) { lg("deep "); longjmp(g_jb, g_val); }
static __attribute__((noinline)) int plain(int v, void **fa) {
    *fa = __builtin_frame_address(0);
    g_val = v;
    int r = setjmp(g_jb);
    if (r == 0) { g_zero_seen++; jump_val(); lg("not "); }
    return r;
}

/* 2, 3, 5: through a __finally */
static __attribute__((noinline)) void jump_7(void) { lg("deep "); longjmp(g_jb, 7); }
static __attribute__((noinline)) void fin_jump_7(void) { f_fin(jump_7); lg("not "); }
static __attribute__((noinline)) int through_fin(void) {
    int r = setjmp(g_jb);
    if (r == 0) { f_fin(jump_7); lg("not "); }
    return r;
}
static __attribute__((noinline)) int frame0(void) {
    int r = _setjmp(g_jb, 0);
    if (r == 0) { f_fin(jump_7); lg("not "); }
    return r;
}
static __attribute__((noinline)) int frame0_val(int v) {
    g_val = v;
    int r = _setjmp(g_jb, 0);
    if (r == 0) { jump_val(); lg("not "); }
    return r;
}

/* 4: what comes back */
static int nv_round(int mode, char *why) {
    uint64_t in[29], out[29];
    for (int i = 0; i < 28; i++) in[i] = 0x1000000000000001ull * (uint64_t)(i + 1) ^ 0x0123456789ABCDEFull;
    in[28] = 0x5F80ull | (0x0A7Full << 32);           /* round up; x87 round up, 53-bit */
    memset(out, 0, sizeof out);
    g_nv_mode = mode;
    int r = f_nv(g_nv_jb, in, out, f_clobber_jump);
    unsigned csr = 0x1F80;
    unsigned short cw = 0x027F;
    __asm__ volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(csr), "m"(cw));
    int bad = -1;
    for (int i = 0; i < 28 && bad < 0; i++) if (in[i] != out[i]) bad = i;
    unsigned got_csr = (unsigned)out[28] & 0xFFFF, got_cw = (unsigned)(out[28] >> 32) & 0xFFFF;
    strcpy(why, "mxcsr ");
    hex(why + 6, got_csr, 4);
    strcat(why, " fpcw ");
    hex(why + strlen(why), got_cw, 4);
    if (bad >= 0) { strcat(why, " first wrong word "); hex(why + strlen(why), (uint64_t)bad, 2); }
    return r == 1 && bad < 0 && got_csr == 0x5F80 && got_cw == 0x0A7F;
}

/* 6, 7: the consolidation unwind */
static EXCEPTION_RECORD g_crec;
static ULONG64 g_outer_est, g_cb_rsp;
static int g_cb_rec_same, g_raise_in_cb;
static DWORD g_cb_code, g_cb_np;
static ULONG_PTR g_cb_i1, g_cb_i2, g_cb_i3;
static CONTEXT g_uctx;

EXCEPTION_DISPOSITION h_mid(EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
    (void)frame; (void)ctx; (void)dc;
    lg(rec->ExceptionFlags & EXCEPTION_UNWIND ? "mu " : "md ");
    if (rec->ExceptionCode == 0xE0000007) lg("[m7] ");
    return ExceptionContinueSearch;
}
static PVOID cons_cb(EXCEPTION_RECORD *rec) {
    ULONG64 here;
    __asm__ volatile("mov %%rsp, %0" : "=r"(here));
    g_cb_rsp = here;
    g_cb_rec_same = rec == &g_crec;
    g_cb_code = rec->ExceptionCode;
    g_cb_np = rec->NumberParameters;
    g_cb_i1 = rec->ExceptionInformation[1];
    g_cb_i2 = rec->ExceptionInformation[2];
    g_cb_i3 = rec->ExceptionInformation[3];
    lg("cb ");
    if (g_raise_in_cb) { lg("raise7 "); RaiseException(0xE0000007, 0, 0, 0); lg("back "); }
    return c_outer_landing;
}
EXCEPTION_DISPOSITION h_outer(EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
    (void)ctx;
    if (rec->ExceptionCode == 0xE0000007) {
        lg(rec->ExceptionFlags & EXCEPTION_UNWIND ? "o7u " : "o7d ");
        return rec->ExceptionFlags & EXCEPTION_UNWIND ? ExceptionContinueSearch : ExceptionContinueExecution;
    }
    if (rec->ExceptionFlags & EXCEPTION_UNWIND) {
        lg(rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND ? "ot " : "ou ");
        if (rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE) lg("[consolidate] ");
        return ExceptionContinueSearch;
    }
    lg("od ");
    g_outer_est = frame;
    memset(&g_crec, 0, sizeof g_crec);
    g_crec.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
    g_crec.NumberParameters = 4;
    g_crec.ExceptionInformation[0] = (ULONG_PTR)cons_cb;
    g_crec.ExceptionInformation[1] = 0x1111;
    g_crec.ExceptionInformation[2] = 0x2222;
    g_crec.ExceptionInformation[3] = frame;
    RtlUnwindEx((PVOID)frame, (PVOID)0x12345, &g_crec, (PVOID)0x4321, &g_uctx, dc->HistoryTable);
    return ExceptionContinueSearch;
}
__attribute__((noinline)) uint64_t c_raiser(void) {
    RaiseException(0xE0000005, 0, 0, 0);
    return 0x7777;
}
static int consolidate(int raise_in_cb, const char *want_log) {
    g_log[0] = 0;
    g_raise_in_cb = raise_in_cb;
    g_landing_rbx = g_landing_xmm6 = 0;
    g_cb_rsp = g_outer_est = 0;
    uint64_t r = c_outer();
    int ok = r == 0x4321 && strcmp(g_log, want_log) == 0 && g_cb_rec_same && g_cb_code == STATUS_UNWIND_CONSOLIDATE &&
             g_cb_np == 4 && g_cb_i1 == 0x1111 && g_cb_i2 == 0x2222 && g_cb_i3 == g_outer_est &&
             g_cb_rsp < g_outer_est && g_landing_rbx == 0xB0B0B0B0B0B0ull && g_landing_xmm6 == 0xB0B0B0B0B0B0ull;
    if (!ok) {
        char b[20];
        hex(b, r, 16);
        printf("        returned %s log: %s\n", b, g_log);
        printf("        callback: same record %d code %lx np %lu i1 %llx i2 %llx i3==frame %d below %d\n", g_cb_rec_same,
               g_cb_code, g_cb_np, (unsigned long long)g_cb_i1, (unsigned long long)g_cb_i2, g_cb_i3 == g_outer_est,
               g_cb_rsp < g_outer_est);
        printf("        landing rbx %llx xmm6 %llx\n", (unsigned long long)g_landing_rbx,
               (unsigned long long)g_landing_xmm6);
    }
    return ok;
}

/* 8: the children */
static __attribute__((noinline)) void jump_child(void) { say("deep\n"); longjmp(g_jb, 3); }
static __attribute__((noinline)) void fin_jump_child(void) { f_fin(jump_child); say("not\n"); }
static int child(const char *m) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    g_child = 1;
    AddVectoredExceptionHandler(1, veh);
    say("child\n");
    if (!strcmp(m, "bad") || !strcmp(m, "high")) {
        ULONG_PTR lo, hi;
        GetCurrentThreadStackLimits(&lo, &hi);
        g_high_frame = hi - 0x40;
        int r = f_jmp_owner(g_jb, fin_jump_child, m[0] == 'b' ? 1 : 2);
        say(r ? "came back\n" : "returned\n");
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
    printf("win-lj37: longjmp that unwinds, and the consolidation unwind\n");
    AddVectoredExceptionHandler(1, veh);

    void *fa5 = 0, *fa0 = 0;
    g_log[0] = 0;
    int r5 = plain(5, &fa5);
    void *jf5 = (void *)((_JUMP_BUFFER *)g_jb)->Frame;
    int r0 = plain(0, &fa0);
    check(1, r5 == 5 && r0 == 1 && g_zero_seen == 2 && jf5 == fa5 && !strcmp(g_log, "deep deep ") && g_vcalls == 0,
          "setjmp: 0, its frame in the buffer; longjmp(5) comes back 5, longjmp(0) 1; no exception dispatched");

    g_log[0] = 0;
    int r7 = through_fin();
    check(2, r7 == 7 && !strcmp(g_log, "deep fin(ab) ") && g_vcalls == 0,
          "a longjmp out of a __try runs its __finally (abnormal termination), nothing dispatched");
    if (r7 != 7 || strcmp(g_log, "deep fin(ab) ")) printf("        came back %d; log: %s\n", r7, g_log);

    g_log[0] = 0;
    int ro = f_jmp_owner(g_jb, fin_jump_7, 0);
    check(3, ro == 1 && !strcmp(g_log, "deep fin(ab) fin(ab) ") && g_vcalls == 0,
          "and one in the setjmp caller's own frame, the unwind's target");
    if (ro != 1 || strcmp(g_log, "deep fin(ab) fin(ab) ")) printf("        came back %d; log: %s\n", ro, g_log);

    char why[96];
    int nv = nv_round(0, why);
    check(4, nv && g_vcalls == 0,
          "rbx rsi rdi rbp r12-r15, xmm6-15, MXCSR and the x87 control word come back as setjmp saw them");
    if (!nv) printf("        %s\n", why);

    g_log[0] = 0;
    int rf = frame0();
    int rf0 = frame0_val(0);
    char why0[96];
    int nv0 = nv_round(1, why0);
    check(5, rf == 7 && rf0 == 1 && !strcmp(g_log, "deep deep ") && nv0 && g_vcalls == 0,
          "_setjmp with a frame of 0: no unwind (the __finally does not run), longjmp(0) 1, the registers all the same");
    if (rf != 7 || rf0 != 1 || strcmp(g_log, "deep deep ") || !nv0)
        printf("        came back %d, %d; log: %s; %s\n", rf, rf0, g_log, why0);

    check(6, consolidate(0, "md od mu ot [consolidate] cb ") && g_vcalls == 1,
          "STATUS_UNWIND_CONSOLIDATE: the unwind to its target, then the callback below it; on where it says, rax the value");
    check(7, consolidate(1, "md od mu ot [consolidate] cb raise7 o7d back ") && g_vcalls == 3,
          "an exception raised in the callback: only the target frame's handler sees it; continued, back in the callback");

    char self[MAX_PATH];
    GetModuleFileNameA(0, self, sizeof self);
    static const struct { const char *args, *out; DWORD code; } ch[] = {
        /* the unwind runs both __finally blocks, then finds the next frame
         * above its target: STATUS_BAD_STACK, raised (noncontinuable) where
         * the unwind stands; nobody handles it, so the unhandled exception's
         * unwind runs them again, and the process ends with its code */
        { "bad", "child\ndeep\nfinally(abnormal)\nfinally(abnormal)\nveh c0000028 flags 81 np 00\n"
                 "finally(abnormal)\nfinally(abnormal)\n", 0xC0000028u },
        /* the same, but the unwind runs off the top of the stack: the
         * status is STATUS_BAD_FUNCTION_TABLE */
        { "high", "child\ndeep\nfinally(abnormal)\nfinally(abnormal)\nveh c00000ff flags 81 np 00\n"
                  "finally(abnormal)\nfinally(abnormal)\n", 0xC00000FFu },
    };
    int ok8 = 1;
    for (unsigned i = 0; i < sizeof ch / sizeof ch[0]; i++) {
        char out[512];
        DWORD code = 0x12345;
        int n = run_child(self, ch[i].args, out, sizeof out, &code);
        int ok = n >= 0 && strcmp(out, ch[i].out) == 0 && code == ch[i].code;
        printf("     child %-3s exit 0x%08lx %s\n", ch[i].args, code, ok ? "" : "(not as on Windows)");
        if (!ok) { ok8 = 0; printf("%s", out); }
    }
    check(8, ok8, "a longjmp to a frame that is no frame's establisher: STATUS_BAD_STACK ends the process; "
                  "to one above every frame, STATUS_BAD_FUNCTION_TABLE");

    printf("win-lj37: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 38;
}
