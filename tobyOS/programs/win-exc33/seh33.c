/* win-seh33 -- Track C, W7b: the x64 exception machinery itself, under the
 * C++ runtime's feet. seh33s.S's frames carry language handlers of this
 * file's (h_outer, h_mid) and a __C_specific_handler scope table (c_frame),
 * so every handler call can be logged: which handler, in which phase
 * (dispatch, unwind, target unwind, collided unwind), with what flags,
 * dispatcher context, context record and target. Twelve checks, each
 * measured on Windows 11 first (it exits 35 there): the two phases and
 * their flags; ReturnValue and the nonvolatile registers (rbx, and xmm6
 * restored from UWOP_SAVE_XMM128 through a frame pointer) at the unwind's
 * target; what DispatcherContext->ContextRecord is in each phase; a collided
 * unwind (RtlUnwindEx from a termination handler: the handler is called
 * again, its ScopeIndex kept); ExceptionContinueExecution; continuing a
 * noncontinuable exception (STATUS_NONCONTINUABLE_EXCEPTION); a __try /
 * __finally / __except scope table's filter results; and RaiseException's
 * fifteen-argument limit. Exit 35 iff all pass, else 100 + the first that
 * failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifndef EXCEPTION_SOFTWARE_ORIGINATE
#define EXCEPTION_SOFTWARE_ORIGINATE 0x80
#endif

uint64_t g_landing_rbx, g_landing_xmm6;
void *g_mid_target;

extern uint64_t outer_frame(void), mid_frame(void), c_frame(void);
extern char outer_landing[], mid_landing[];

static int g_scn, g_collided_once;
static DWORD g_code = 0xE0001234, g_flags, g_nargs = 2;
static LONG g_filter_ret;
static CONTEXT g_uctx;

struct ev {
    char who, phase;              /* who: m o f t; phase: d u t c (see note) */
    DWORD code, flags, nparams;
    void *exaddr, *chain;
    ULONG_PTR info0, info14;
    ULONG64 frame, controlpc, targetip, ctx_rip, ctx_rsp, dc_rip, dc_rsp;
    int ctx_is_dc, ctx_is_uctx, have_dc;
    ULONG scope;
    int arg;
};
static struct ev g_ev[40];
static int g_nev;

static struct ev *note(char who, EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx,
                       DISPATCHER_CONTEXT *dc) {
    static struct ev spare;
    struct ev *e = g_nev < 40 ? &g_ev[g_nev++] : &spare;
    memset(e, 0, sizeof *e);
    e->who = who;
    e->phase = 'd';
    e->frame = frame;
    if (rec) {
        e->code = rec->ExceptionCode;
        e->flags = rec->ExceptionFlags;
        if (e->flags & EXCEPTION_COLLIDED_UNWIND) e->phase = 'c';
        else if (e->flags & EXCEPTION_TARGET_UNWIND) e->phase = 't';
        else if (e->flags & EXCEPTION_UNWINDING) e->phase = 'u';
        e->nparams = rec->NumberParameters;
        e->exaddr = rec->ExceptionAddress;
        e->chain = rec->ExceptionRecord;
        e->info0 = rec->ExceptionInformation[0];
        e->info14 = rec->ExceptionInformation[14];
    }
    if (ctx) { e->ctx_rip = ctx->Rip; e->ctx_rsp = ctx->Rsp; e->ctx_is_uctx = ctx == &g_uctx; }
    if (dc) {
        e->have_dc = 1;
        e->controlpc = dc->ControlPc;
        e->targetip = dc->TargetIp;
        e->scope = dc->ScopeIndex;
        e->ctx_is_dc = ctx == dc->ContextRecord;
        if (dc->ContextRecord) { e->dc_rip = dc->ContextRecord->Rip; e->dc_rsp = dc->ContextRecord->Rsp; }
    }
    return e;
}

EXCEPTION_DISPOSITION h_mid(EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx,
                            DISPATCHER_CONTEXT *dc) {
    note('m', rec, frame, ctx, dc);
    int unwinding = (rec->ExceptionFlags & EXCEPTION_UNWIND) != 0;
    switch (g_scn) {
    case 'B':
        if (unwinding && !g_collided_once) {
            g_collided_once = 1;
            dc->ScopeIndex = 7;
            RtlUnwindEx((PVOID)frame, mid_landing, rec, (PVOID)0x5678, &g_uctx, dc->HistoryTable);
        }
        break;
    case 'C':
        if (!unwinding) return ExceptionContinueExecution;
        break;
    case 'D': case 'H':
        if (!unwinding && rec->ExceptionCode == g_code) return ExceptionContinueExecution;
        break;
    }
    return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION h_outer(EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx,
                              DISPATCHER_CONTEXT *dc) {
    note('o', rec, frame, ctx, dc);
    if (!(rec->ExceptionFlags & EXCEPTION_UNWIND))
        RtlUnwindEx((PVOID)frame, outer_landing, rec, (PVOID)0x1234, &g_uctx, dc->HistoryTable);
    return ExceptionContinueSearch;
}

LONG c_filter(EXCEPTION_POINTERS *ep, void *frame) {
    note('f', ep->ExceptionRecord, (ULONG64)frame, ep->ContextRecord, NULL);
    return g_filter_ret;
}

void c_finally(BOOLEAN abnormal, void *frame) {
    struct ev *e = note('t', NULL, (ULONG64)frame, NULL, NULL);
    e->phase = 'f';
    e->arg = abnormal;
}

__attribute__((noinline)) uint64_t raiser(void) {
    ULONG_PTR a[20];
    for (int i = 0; i < 20; i++) a[i] = 0x100 + i;
    RaiseException(g_code, g_flags, g_nargs, a);
    return 0x7777;
}

static uint64_t g_ret;
static char g_seq[80];

static void run(int scn, int top, void *target, DWORD flags, DWORD nargs, LONG filt) {
    g_scn = scn; g_collided_once = 0; g_nev = 0;
    g_flags = flags; g_nargs = nargs; g_filter_ret = filt;
    g_landing_rbx = g_landing_xmm6 = 0;
    g_mid_target = target;
    memset(&g_uctx, 0, sizeof g_uctx);
    g_ret = top ? outer_frame() : c_frame();
    int k = 0;
    for (int i = 0; i < g_nev && k < (int)sizeof g_seq - 4; i++) {
        if (i) g_seq[k++] = ' ';
        g_seq[k++] = g_ev[i].who;
        g_seq[k++] = g_ev[i].phase;
    }
    g_seq[k] = 0;
    printf("  scenario %c: returned %llx, handler calls \"%s\"\n", scn,
           (unsigned long long)g_ret, g_seq);
}

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

int main(void) {
    printf("win-seh33: the x64 exception machinery\n");
    const uint64_t pat = 0xB0B0B0B0B0B0ULL;
    struct ev *m, *o;

    run('A', 1, (void *)raiser, 0, 2, 0);
    check(1, g_ret == 0x1234 && strcmp(g_seq, "md od mu ot") == 0,
          "RtlUnwindEx from a handler: dispatch then unwind, the target last; ReturnValue in rax");
    check(2, g_landing_rbx == pat && g_landing_xmm6 == pat,
          "rbx and xmm6 at the target are its own (restored from the frames between)");
    m = &g_ev[0]; o = &g_ev[1];
    check(3, g_nev == 4 && m->flags == EXCEPTION_SOFTWARE_ORIGINATE && m->nparams == 2 &&
             m->info0 == 0x100 && (ULONG64)m->exaddr == m->ctx_rip && m->targetip == 0 &&
             m->code == 0xE0001234 && !m->chain,
          "dispatch: the record (flags 0x80, 2 parameters, the address = the raise context's rip)");
    check(4, g_nev == 4 && !m->ctx_is_dc && m->dc_rip == o->controlpc && m->dc_rsp == o->frame &&
             m->ctx_rip == o->ctx_rip && m->frame != o->frame,
          "dispatch: DispatcherContext->ContextRecord is the caller's context, not the raise context");
    check(5, g_nev == 4 && g_ev[2].flags == EXCEPTION_UNWINDING &&
             g_ev[3].flags == (EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND) &&
             g_ev[2].targetip == (ULONG64)outer_landing && g_ev[3].targetip == (ULONG64)outer_landing &&
             g_ev[2].ctx_is_dc && g_ev[3].ctx_is_dc &&
             g_ev[2].ctx_rip == g_ev[2].controlpc && g_ev[2].ctx_rsp == g_ev[2].frame &&
             g_ev[3].ctx_rip == g_ev[3].controlpc && g_ev[3].ctx_rsp == g_ev[3].frame &&
             g_ev[2].controlpc == m->controlpc && g_ev[2].frame == m->frame,
          "unwind: flags 0x2 then 0x22, TargetIp, ContextRecord = the frame's own context");

    run('B', 1, (void *)raiser, 0, 2, 0);
    check(6, g_ret == 0x5678 && g_landing_rbx == 0 && strcmp(g_seq, "md od mu mc") == 0 &&
             g_ev[3].flags == (EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND) &&
             g_ev[3].scope == 7 && g_ev[3].targetip == (ULONG64)mid_landing && g_ev[3].ctx_is_uctx &&
             g_ev[3].ctx_is_dc && g_ev[3].frame == g_ev[2].frame && g_ev[3].controlpc == g_ev[2].controlpc,
          "a collided unwind: the handler is called again (flags 0x62), ScopeIndex kept");

    run('C', 1, (void *)raiser, 0, 2, 0);
    check(7, g_ret == 0x7777 && strcmp(g_seq, "md") == 0,
          "ExceptionContinueExecution: RaiseException returns");

    run('D', 1, (void *)raiser, EXCEPTION_NONCONTINUABLE, 2, 0);
    check(8, g_ret == 0x1234 && strcmp(g_seq, "md md od mu ot") == 0 &&
             g_ev[0].flags == (EXCEPTION_NONCONTINUABLE | EXCEPTION_SOFTWARE_ORIGINATE) &&
             g_ev[1].code == STATUS_NONCONTINUABLE_EXCEPTION &&
             g_ev[1].flags == (EXCEPTION_NONCONTINUABLE | EXCEPTION_SOFTWARE_ORIGINATE) &&
             g_ev[1].nparams == 0 && !g_ev[1].chain && g_ev[3].code == STATUS_NONCONTINUABLE_EXCEPTION,
          "continuing a noncontinuable exception raises STATUS_NONCONTINUABLE_EXCEPTION");

    run('E', 0, 0, 0, 2, EXCEPTION_EXECUTE_HANDLER);
    check(9, g_ret == 0xE0001234 && strcmp(g_seq, "fd tf") == 0 && g_ev[1].arg == 1 &&
             g_ev[0].frame == g_ev[1].frame && g_ev[0].flags == EXCEPTION_SOFTWARE_ORIGINATE &&
             (ULONG64)g_ev[0].exaddr == g_ev[0].ctx_rip,
          "__C_specific_handler: filter EXECUTE_HANDLER, the inner __finally (abnormal), then __except with the code");

    run('F', 0, 0, 0, 2, EXCEPTION_CONTINUE_EXECUTION);
    check(10, g_ret == 0 && strcmp(g_seq, "fd") == 0,
          "__C_specific_handler: filter CONTINUE_EXECUTION, RaiseException returns");

    run('G', 1, (void *)c_frame, 0, 2, EXCEPTION_CONTINUE_SEARCH);
    check(11, g_ret == 0x1234 && strcmp(g_seq, "fd md od tf mu ot") == 0 && g_ev[3].arg == 1 &&
             g_landing_rbx == pat && g_landing_xmm6 == pat,
          "__C_specific_handler: filter CONTINUE_SEARCH; its __finally runs in another frame's unwind");

    run('H', 1, (void *)raiser, 0, 20, 0);
    check(12, g_ret == 0x7777 && strcmp(g_seq, "md") == 0 && g_ev[0].nparams == 15 &&
             g_ev[0].info14 == 0x10E,
          "RaiseException with 20 arguments keeps 15");

    printf("win-seh33: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 35;
}
