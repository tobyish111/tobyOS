/* win-flt35 -- Track C, W7d: CPU faults as SEH exceptions. Each fault of
 * flt35s.S's -- an access violation reading, writing and executing, at a
 * non-canonical address; divide by zero and divide overflow; ud2; hlt, cli
 * and in; int3; a single-step trap; an unmasked SSE divide by zero -- is
 * seen by a vectored handler with the code, parameters, address and
 * context Windows 11 gives (measured first, logs/w32/w7d-host-run.txt: it
 * exits 35 there), which steps over it and continues. Then: every integer
 * register and xmm6-15 in the fault's CONTEXT, changed by the handler and
 * there after it; a fault two frames down caught by a __except, its
 * __finally run and rbx restored by the unwind out of the faulting frame;
 * and, in child processes, faults nobody handles -- the process ends with
 * the code (from a second thread too), the __finally on the way run; last,
 * an unmasked x87 divide by zero, reported at the next x87 instruction and
 * gone once the handler clears it in the context. The SSE check (11) needs a
 * CPU that raises SIMD exceptions: QEMU's TCG raises none, so there it is
 * shown as not run, not counted (real hardware runs it).
 * Exit 35 iff all pass, else 100 + the first that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <xmmintrin.h>

extern void f_null_read(void), f_ro_write(void *p), f_exec(void *p), f_div0(void), f_div0_mem(void),
            f_divovf(void), f_ud2(void), f_hlt(void), f_cli(void), f_in(void), f_int3(void),
            f_noncanon(void), f_tf(void), f_sse_div(void), f_regs(const uint64_t *in, uint64_t *out),
            f_fin(void), f_x87_div(void);
extern uint64_t f_scoped(uint64_t *rbx_at_except);
extern char f_null_read_at[], f_ro_write_at[], f_div0_at[], f_div0_mem_at[], f_divovf_at[], f_ud2_at[],
            f_hlt_at[], f_cli_at[], f_in_at[], f_int3_at[], f_noncanon_at[], f_tf_next[],
            f_sse_div_at[], f_regs_at[], f_x87_wait_at[];

static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}

/* A page of .rdata (read-only) and one of .data (no execute) -- the image's
 * own pages, as its section flags make them. */
__attribute__((aligned(4096))) static const char g_ro_page[4096] = { 1 };
__attribute__((aligned(4096))) static char g_nx_page[4096] = { 1 };

/* ---- the vectored handler ---- */
static EXCEPTION_RECORD g_rec;
static CONTEXT g_ctx;
static int g_calls;
static ULONG64 g_skip;            /* bytes to step over */
static int g_mode;                /* 1: the first call continues unchanged; 2: return to the caller; 3: f_regs */
static uint64_t g_in[36], g_out[36];
static int g_regs_seen_ok;
#define K 0x0101010101010101ULL

static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    CONTEXT *c = ep->ContextRecord;
    if (g_calls++ == 0) { g_rec = *ep->ExceptionRecord; g_ctx = *c; }
    if (g_calls > 3) { c->Rip += g_skip ? g_skip : 1; return EXCEPTION_CONTINUE_EXECUTION; }
    if (g_mode == 1 && g_calls == 1) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_mode == 2) {
        c->Rip = *(ULONG64 *)c->Rsp;
        c->Rsp += 8;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_mode == 4) {                       /* x87: clear the pending exception, mask them */
        c->FltSave.StatusWord &= (WORD)~0x80BF;
        c->FltSave.ControlWord |= 0x3F;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_mode == 3) {
        ULONG64 *r = &c->Rax;
        int ok = 1;
        for (int i = 0; i < 16; i++) {
            if (i == 4) continue;
            if (r[i] != g_in[i]) ok = 0;
            r[i] = g_in[i] ^ K;
        }
        M128A *x = &c->Xmm6;
        for (int i = 0; i < 10; i++) {
            if (x[i].Low != g_in[16 + 2 * i] || (ULONG64)x[i].High != g_in[17 + 2 * i]) ok = 0;
            x[i].Low = g_in[16 + 2 * i] ^ K;
            x[i].High = (LONGLONG)(g_in[17 + 2 * i] ^ K);
        }
        g_regs_seen_ok = ok;
    }
    c->Rip += g_skip;
    c->EFlags &= ~0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int first_fail, g_skipped;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok) {
        printf("        code %08lx flags %lx np %lu p0 %llx p1 %llx addr %p rip %llx calls %d\n",
               g_rec.ExceptionCode, g_rec.ExceptionFlags, g_rec.NumberParameters,
               (unsigned long long)g_rec.ExceptionInformation[0],
               (unsigned long long)g_rec.ExceptionInformation[1], g_rec.ExceptionAddress,
               (unsigned long long)g_ctx.Rip, g_calls);
    }
    if (!ok && !first_fail) first_fail = n;
}
static void run(void (*f)(void), ULONG64 skip) {
    g_calls = 0; g_skip = skip; g_mode = 0;
    memset(&g_rec, 0, sizeof g_rec);
    f();
}
static int at(const void *a) { return g_rec.ExceptionAddress == a && g_ctx.Rip == (ULONG64)a; }
static int av(ULONG_PTR rw, ULONG_PTR addr) {
    return g_rec.ExceptionCode == 0xC0000005 && g_rec.ExceptionFlags == 0 && g_rec.NumberParameters == 2 &&
           g_rec.ExceptionInformation[0] == rw && g_rec.ExceptionInformation[1] == addr;
}
static int plain(DWORD code) {
    return g_rec.ExceptionCode == code && g_rec.ExceptionFlags == 0 && g_rec.NumberParameters == 0;
}

/* ---- the frame-based part ---- */
static char g_slog[64];
LONG s_filter(EXCEPTION_POINTERS *ep, void *frame) {
    (void)frame;
    strcat(g_slog, ep->ExceptionRecord->ExceptionCode == 0xC0000005 ? "f " : "f? ");
    return EXCEPTION_EXECUTE_HANDLER;
}
void s_finally(BOOLEAN abnormal, void *frame) {
    (void)frame;
    strcat(g_slog, abnormal ? "t1 " : "t0 ");
    say(abnormal ? "finally (abnormal)\n" : "finally (normal)\n");
}

/* ---- the children: a fault nobody handles ---- */
static DWORD WINAPI t_av(void *p) { (void)p; f_null_read(); return 0; }
static int child(const char *m) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    say("child\n");
    if (!strcmp(m, "av")) f_null_read();
    else if (!strcmp(m, "dz")) f_div0();
    else if (!strcmp(m, "fin")) f_fin();
    else if (!strcmp(m, "th")) {
        HANDLE h = CreateThread(0, 0, t_av, 0, 0, 0);
        WaitForSingleObject(h, 10000);
        say("main: the thread ended\n");
        return 3;
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
    WaitForSingleObject(pi.hProcess, 20000);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1]);
    printf("win-flt35: CPU faults as SEH exceptions\n");
    void *vh = AddVectoredExceptionHandler(1, veh);
    char *ro = (char *)g_ro_page;
    char *nx = g_nx_page;
    if (!vh) { printf("setup failed\n"); return 99; }
    memset(nx, 0xC3, 4096);

    run(f_null_read, 3);
    check(1, g_calls == 1 && av(0, 0) && at(f_null_read_at),
          "read at 0: 0xC0000005, parameters {0 (read), 0}, at the instruction");
    g_calls = 0; g_skip = 3; g_mode = 0; memset(&g_rec, 0, sizeof g_rec);
    f_ro_write(ro);
    check(2, g_calls == 1 && av(1, (ULONG_PTR)ro) && at(f_ro_write_at),
          "a write to a read-only page: {1 (write), the address}");
    g_calls = 0; g_skip = 0; g_mode = 2; memset(&g_rec, 0, sizeof g_rec);
    f_exec(nx);
    check(3, g_calls == 1 && av(8, (ULONG_PTR)nx) && at(nx),
          "a jump into a page without execute: {8 (execute), the address}, rip there");
    run(f_noncanon, 3);
    check(4, g_calls == 1 && av(0, ~(ULONG_PTR)0) && at(f_noncanon_at),
          "a read at a non-canonical address: {0, all ones}");
    run(f_div0, 2);
    int ok5 = g_calls == 1 && plain(0xC0000094) && at(f_div0_at);
    run(f_div0_mem, 3);
    ok5 = ok5 && g_calls == 1 && plain(0xC0000094) && at(f_div0_mem_at);
    check(5, ok5, "divide by a zero register, by a zero in memory: 0xC0000094");
    run(f_divovf, 3);
    check(6, g_calls == 1 && plain(0xC0000095) && at(f_divovf_at), "INT64_MIN / -1: 0xC0000095");
    run(f_ud2, 2);
    check(7, g_calls == 1 && plain(0xC000001D) && at(f_ud2_at), "ud2: 0xC000001D");
    run(f_hlt, 1);
    int ok8 = g_calls == 1 && plain(0xC0000096) && at(f_hlt_at);
    run(f_cli, 1);
    ok8 = ok8 && g_calls == 1 && plain(0xC0000096) && at(f_cli_at);
    run(f_in, 1);
    ok8 = ok8 && g_calls == 1 && plain(0xC0000096) && at(f_in_at);
    check(8, ok8, "hlt, cli, in al,dx: 0xC0000096");
    run(f_int3, 1);
    int ok9 = g_calls == 1 && g_rec.ExceptionCode == 0x80000003 && g_rec.NumberParameters == 1 &&
              g_rec.ExceptionInformation[0] == 0 && at(f_int3_at);
    g_calls = 0; g_skip = 1; g_mode = 1; memset(&g_rec, 0, sizeof g_rec);
    f_int3();
    ok9 = ok9 && g_calls == 2;
    check(9, ok9, "int3: 0x80000003 {0}, rip AT the int3 -- continued unchanged it traps again");
    run(f_tf, 0);
    check(10, g_calls == 1 && plain(0x80000004) && at(f_tf_next),
          "a single-step trap: 0x80000004 after the stepped instruction");
    unsigned int old = _mm_getcsr();
    _mm_setcsr(old & ~0x0200u);
    run(f_sse_div, 4);
    unsigned int mx = g_ctx.MxCsr, after = _mm_getcsr();
    _mm_setcsr(old);
    if (g_calls == 0 && (after & 0x4) && !(after & 0x200)) {
        /* The division raised ZE with ZM clear and nothing trapped: this CPU
         * raises no SIMD exception at all (QEMU's TCG does not) -- no
         * system could have shown one. Shown, not counted. */
        printf("skip 11 an unmasked SSE divide by zero -- this CPU raised no #XM (MXCSR after: %x)\n", after);
        g_skipped++;
    } else {
        check(11, g_calls == 1 && g_rec.ExceptionCode == 0xC000008E && g_rec.NumberParameters == 2 &&
                  g_rec.ExceptionInformation[0] == 0 && g_rec.ExceptionInformation[1] == mx &&
                  (mx & 0x4) && !(mx & 0x200) && at(f_sse_div_at),
              "an unmasked SSE divide by zero: 0xC000008E {0, MXCSR}, the context's MXCSR with ZE");
    }

    for (int i = 0; i < 36; i++) g_in[i] = 0x1000000000000000ULL * (uint64_t)(i % 15 + 1) + (uint64_t)i * 0x1111;
    g_calls = 0; g_skip = 2; g_mode = 3; g_regs_seen_ok = 0;
    memset(g_out, 0, sizeof g_out);
    f_regs(g_in, g_out);
    int ok12 = g_calls == 1 && plain(0xC000001D) && at(f_regs_at) && g_regs_seen_ok;
    for (int i = 0; i < 36; i++) if (i != 4 && g_out[i] != (g_in[i] ^ K)) ok12 = 0;
    check(12, ok12, "every integer register and xmm6-15 in the context; the handler's changes are there after it");

    RemoveVectoredExceptionHandler(vh);
    uint64_t rbx = 0;
    g_slog[0] = 0;
    uint64_t r = f_scoped(&rbx);
    check(13, r == 0xC0000005 && rbx == 0x5C0FED0B5C0FEDULL && strcmp(g_slog, "f t1 ") == 0,
          "a fault two frames down: the __except's filter, the __finally, rbx restored, the code in rax");

    char self[MAX_PATH];
    GetModuleFileNameA(0, self, sizeof self);
    static const struct { const char *args, *out; DWORD code; } ch[] = {
        { "av", "child\n", 0xC0000005u },
        { "dz", "child\n", 0xC0000094u },
        { "fin", "child\nfinally (abnormal)\n", 0xC0000005u },
        { "th", "child\n", 0xC0000005u },
    };
    int ok14 = 1;
    for (unsigned i = 0; i < sizeof ch / sizeof ch[0]; i++) {
        char out[256];
        DWORD code = 0x12345;
        int n = run_child(self, ch[i].args, out, sizeof out, &code);
        int ok = n >= 0 && strcmp(out, ch[i].out) == 0 && code == ch[i].code;
        printf("     child %-3s exit 0x%08lx %s\n", ch[i].args, code, ok ? "" : "(not as on Windows)");
        if (!ok) ok14 = 0;
    }
    check(14, ok14, "faults nobody handles: the process ends with the code, from a second thread too");

    /* x87: the exception is pending after the fdivrp and comes at the next
     * x87 instruction (the fwait), Rip there; the handler clears it in the
     * context's FltSave (status word) and masks it -- which only takes if
     * continuing restores the x87 state. */
    vh = AddVectoredExceptionHandler(1, veh);
    g_calls = 0; g_skip = 0; g_mode = 4; memset(&g_rec, 0, sizeof g_rec);
    f_x87_div();
    RemoveVectoredExceptionHandler(vh);
    check(15, g_calls == 1 && g_rec.ExceptionCode == 0xC000008E && g_rec.ExceptionFlags == 0 &&
              g_rec.NumberParameters == 1 && g_rec.ExceptionInformation[0] == 0 && at(f_x87_wait_at) &&
              (g_ctx.FltSave.StatusWord & 0x84) == 0x84 && !(g_ctx.FltSave.ControlWord & 0x4),
          "an unmasked x87 divide by zero: 0xC000008E {0} at the next x87 instruction; cleared in the context, gone");

    printf("win-flt35: %s%s\n", first_fail ? "a check failed" : "all checks ok",
           g_skipped ? " (one not run: see above)" : "");
    return first_fail ? 100 + first_fail : 35;
}
