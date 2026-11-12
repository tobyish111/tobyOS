/* tobyrt.dll -- the tobyOS Win32 runtime: x64 structured exception handling
 * (W7b of docs/roadmap-2026-11.md), in user mode, as Windows has it in ntdll.
 *
 * A throw on x64 Windows -- a C++ one through libgcc's SEH unwinder, or
 * RaiseException itself -- is dispatched by the system: it walks the stack
 * from the raise, finds each frame's unwind information in its image's
 * exception directory (RtlLookupFunctionEntry), unwinds the frame virtually
 * (RtlVirtualUnwind) and calls the frame's language handler; a handler that
 * takes the exception unwinds to itself (RtlUnwindEx), which calls every
 * handler between again so their cleanups run, and continues at its target.
 *
 * Written from Microsoft's documentation (x64 exception handling, prolog and
 * epilog, the RtlVirtualUnwind/RtlUnwindEx/RtlLookupFunctionEntry/
 * RaiseException/SetUnhandledExceptionFilter pages) and the public headers'
 * structures; what the pages leave out -- the dispositions, collided unwinds,
 * the flags each phase carries, what DispatcherContext->ContextRecord is,
 * the scope table's semantics, the unhandled path -- measured on Windows 11
 * (programs/win-exc33/seh33.c and logs/w32/w7b-host-run.txt).
 *
 * It is an ordinary DLL with its own unwind information, so a walk passes
 * through its frames as Windows' walks pass through ntdll's: an unwind
 * started inside a handler goes up through the dispatcher's frames, and the
 * two frames handlers are called from carry handlers of their own that tell
 * a nested dispatch or a collided unwind where the first one had got to.
 *
 * The loader (src/pe_loader.c) binds a program's import of each exported
 * name (tobyrt.def) from a built-in DLL to it, and maps it at
 * /lib/win32/tobyrt.dll. It reads two things of the loader's: the module
 * table (include/tobyos/pe.h, WIN32_MODTAB_BASE) to find the image a pc is
 * in, and the shim area's range (pe.h, WIN32_SHIM_BASE..WIN32_MISSING_END_VA)
 * -- where every thread's first frame is (the loader's start stubs), so where
 * a walk ends. */
#include <windows.h>
#include <stdint.h>

#ifndef EXCEPTION_SOFTWARE_ORIGINATE
#define EXCEPTION_SOFTWARE_ORIGINATE 0x80
#endif
#define RT_STATUS_NONCONTINUABLE        0xC0000025u
#define RT_STATUS_INVALID_DISPOSITION   0xC0000026u
#define RT_STATUS_UNWIND                0xC0000027u
#define RT_STATUS_BAD_STACK             0xC0000028u
#define RT_STATUS_INVALID_UNWIND_TARGET 0xC0000029u
#define RT_STATUS_BAD_FUNCTION_TABLE    0xC00000FFu
#define RT_STATUS_LONGJUMP              0x80000026u
#define RT_STATUS_UNWIND_CONSOLIDATE    0x80000029u

/* include/tobyos/pe.h */
#define RT_SHIM_LO        0x0000000030000000ULL   /* WIN32_SHIM_BASE */
#define RT_SHIM_HI        0x0000000030006000ULL   /* WIN32_MISSING_END_VA */
#define RT_STUBTAB        0x0000000030000C00ULL   /* W7g: WIN32_STUBTAB_VA */
#define RT_STUBTAB_MAGIC  0x37425453u             /* WIN32_STUBTAB_MAGIC */
#define RT_MODTAB         0x0000000031008000ULL   /* WIN32_MODTAB_BASE */
#define RT_MT_MOD_COUNT   0x024
#define RT_MT_MODS        0x200
#define RT_MT_MOD_STRIDE  64
#define RT_MTM_NAME       0x18
#define RT_MAX_MODULES    16

/* tobyrt_s.S */
void rt_capture_context(CONTEXT *ctx);
__attribute__((noreturn)) void rt_restore_context(CONTEXT *ctx, EXCEPTION_RECORD *rec);
__attribute__((noreturn)) void rt_consolidate(CONTEXT *ctx, EXCEPTION_RECORD *rec);
struct rt_jump_buffer;
__attribute__((noreturn)) void rt_longjmp_direct(const struct rt_jump_buffer *jb, int value);
EXCEPTION_DISPOSITION rt_call_dispatch_handler(EXCEPTION_RECORD *rec, ULONG64 frame,
                                               CONTEXT *ctx, DISPATCHER_CONTEXT *dc);
EXCEPTION_DISPOSITION rt_call_unwind_handler(EXCEPTION_RECORD *rec, ULONG64 frame,
                                             CONTEXT *ctx, DISPATCHER_CONTEXT *dc);
#define RT_HELPER_DC_SLOT 0x20      /* where those two keep dc, from their establisher frame */

void rt_RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);
void rt_RtlUnwindEx(PVOID target_frame, PVOID target_ip, EXCEPTION_RECORD *rec,
                    PVOID retval, CONTEXT *ctx, PUNWIND_HISTORY_TABLE hist);
void rt_RtlUnwind(PVOID target_frame, PVOID target_ip, EXCEPTION_RECORD *rec, PVOID retval);
__attribute__((noreturn)) void rt_RtlRestoreContext(CONTEXT *ctx, EXCEPTION_RECORD *rec);
LONG rt_UnhandledExceptionFilter(EXCEPTION_POINTERS *ep);

static LPTOP_LEVEL_EXCEPTION_FILTER volatile g_top_filter;
static volatile UINT g_error_mode;          /* SetErrorMode's */

/* ---- vectored handlers (W7c) -------------------------------------------- *
 * Two lists -- exception handlers, called before any frame's handler while
 * dispatching, and continue handlers, called when an exception is about to be
 * continued -- of entries from a fixed pool (a handle is its entry). A spin
 * lock guards the lists only: the handlers are called from a copy taken under
 * it, so one may add or remove handlers, or raise, while being called. The
 * rules (measured on Windows 11, programs/win-exc34): First != 0 puts an entry
 * at the head; a handler's EXCEPTION_CONTINUE_EXECUTION ends its list, any
 * other answer goes on. */
#define RT_VEH_MAX 64
struct rt_veh {
    struct rt_veh *next;
    PVECTORED_EXCEPTION_HANDLER fn;         /* NULL: a free entry */
};
static struct rt_veh g_veh_pool[RT_VEH_MAX];
static struct rt_veh *g_veh_head[2];        /* 0 exception handlers, 1 continue handlers */
static volatile LONG g_veh_lock;

static void rt_veh_lock(void) {
    while (__atomic_exchange_n(&g_veh_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}
static void rt_veh_unlock(void) { __atomic_store_n(&g_veh_lock, 0, __ATOMIC_RELEASE); }

static PVOID rt_veh_add(int list, ULONG first, PVECTORED_EXCEPTION_HANDLER fn) {
    if (!fn) return 0;
    rt_veh_lock();
    struct rt_veh *e = 0;
    for (int i = 0; i < RT_VEH_MAX && !e; i++)
        if (!g_veh_pool[i].fn) e = &g_veh_pool[i];
    if (e) {
        e->fn = fn;
        e->next = 0;
        if (first || !g_veh_head[list]) {
            e->next = g_veh_head[list];
            g_veh_head[list] = e;
        } else {
            struct rt_veh *t = g_veh_head[list];
            while (t->next) t = t->next;
            t->next = e;
        }
    }
    rt_veh_unlock();
    return e;
}

static ULONG rt_veh_remove(int list, PVOID h) {
    ULONG done = 0;
    rt_veh_lock();
    for (struct rt_veh **pp = &g_veh_head[list]; *pp; pp = &(*pp)->next) {
        if (*pp == h) {
            struct rt_veh *e = *pp;
            *pp = e->next;
            e->next = 0;
            e->fn = 0;
            done = 1;
            break;
        }
    }
    rt_veh_unlock();
    return done;
}

/* Call a list's handlers in order; EXCEPTION_CONTINUE_EXECUTION from one ends
 * it and is the answer, else EXCEPTION_CONTINUE_SEARCH. */
static LONG rt_veh_call(int list, EXCEPTION_POINTERS *ep) {
    PVECTORED_EXCEPTION_HANDLER fns[RT_VEH_MAX];
    int n = 0;
    rt_veh_lock();
    for (struct rt_veh *e = g_veh_head[list]; e && n < RT_VEH_MAX; e = e->next) fns[n++] = e->fn;
    rt_veh_unlock();
    for (int i = 0; i < n; i++)
        if (fns[i](ep) == EXCEPTION_CONTINUE_EXECUTION) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

PVOID rt_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn) {
    return rt_veh_add(0, first, fn);
}
ULONG rt_RemoveVectoredExceptionHandler(PVOID h) { return rt_veh_remove(0, h); }
PVOID rt_AddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER fn) {
    return rt_veh_add(1, first, fn);
}
ULONG rt_RemoveVectoredContinueHandler(PVOID h) { return rt_veh_remove(1, h); }

/* ---- where a pc is ------------------------------------------------------ */

static int rt_is_system_pc(ULONG64 pc) { return pc >= RT_SHIM_LO && pc < RT_SHIM_HI; }

static ULONG64 rt_stack_base(void) {
    ULONG64 b = __readgsqword(8);           /* NT_TIB.StackBase */
    return b ? b : ~0ULL;
}

/* The module record whose image holds pc: its base (0 if none). */
static ULONG64 rt_module_of(ULONG64 pc, ULONG64 *size, const char **name) {
    const volatile uint8_t *mt = (const volatile uint8_t *)RT_MODTAB;
    uint32_t n = *(const volatile uint32_t *)(mt + RT_MT_MOD_COUNT);
    if (n > RT_MAX_MODULES) n = RT_MAX_MODULES;
    for (uint32_t i = 0; i < n; i++) {
        const volatile uint8_t *r = mt + RT_MT_MODS + (ULONG64)i * RT_MT_MOD_STRIDE;
        ULONG64 b = *(const volatile ULONG64 *)r, s = *(const volatile ULONG64 *)(r + 8);
        if (b && pc >= b && pc - b < s) {
            if (size) *size = s;
            if (name) *name = (const char *)*(const volatile ULONG64 *)(r + RT_MTM_NAME);
            return b;
        }
    }
    return 0;
}

/* The image's exception directory: its RUNTIME_FUNCTION array. */
static const RUNTIME_FUNCTION *rt_pdata(ULONG64 base, ULONG64 size, DWORD *count) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (ULONG64)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size)
        return 0;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + (ULONG64)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        return 0;
    const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d->VirtualAddress || d->Size < sizeof(RUNTIME_FUNCTION) ||
        (ULONG64)d->VirtualAddress + d->Size > size)
        return 0;
    *count = d->Size / sizeof(RUNTIME_FUNCTION);
    return (const RUNTIME_FUNCTION *)(base + d->VirtualAddress);
}

/* ---- function tables for code made at run time (W7e) ------------------- *
 * RtlAddFunctionTable, RtlInstallFunctionTableCallback and the growable
 * tables: entries from a fixed pool, a spin lock around the list.
 * RtlLookupFunctionEntry asks them for a pc no image holds. The rules
 * (measured on Windows 11, programs/win-exc36): a table's RVAs are relative
 * to the base it is given, which the lookup reports as the image base; a
 * callback table's identifier must have its low two bits set (else FALSE)
 * and is the handle RtlDeleteFunctionTable takes; a growable table answers
 * for its first `count` entries, nothing until RtlGrowFunctionTable; a
 * table deleted twice: FALSE. */
#define RT_DYN_MAX 64
enum { RT_DYN_FREE, RT_DYN_STATIC, RT_DYN_CALLBACK, RT_DYN_GROWABLE };
struct rt_dyn {
    int kind;
    PRUNTIME_FUNCTION table;            /* static, growable */
    volatile DWORD count;               /* static, growable */
    DWORD max;                          /* growable */
    ULONG64 base;                       /* the RVAs' base */
    ULONG64 lo, hi;                     /* the pcs it answers for */
    ULONG64 id;                         /* callback: the identifier */
    PGET_RUNTIME_FUNCTION_CALLBACK callback;
    PVOID context;
};
static struct rt_dyn g_dyn[RT_DYN_MAX];
static volatile LONG g_dyn_lock;
static void rt_dyn_lock(void) {
    while (__atomic_exchange_n(&g_dyn_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}
static void rt_dyn_unlock(void) { __atomic_store_n(&g_dyn_lock, 0, __ATOMIC_RELEASE); }

static struct rt_dyn *rt_dyn_new(void) {
    for (int i = 0; i < RT_DYN_MAX; i++)
        if (g_dyn[i].kind == RT_DYN_FREE) return &g_dyn[i];
    return 0;
}

BOOLEAN rt_RtlAddFunctionTable(PRUNTIME_FUNCTION table, DWORD count, DWORD64 base) {
    if (!table || !count) return FALSE;
    ULONG64 lo = ~0ULL, hi = 0;
    for (DWORD i = 0; i < count; i++) {
        if (base + table[i].BeginAddress < lo) lo = base + table[i].BeginAddress;
        if (base + table[i].EndAddress > hi) hi = base + table[i].EndAddress;
    }
    rt_dyn_lock();
    struct rt_dyn *d = rt_dyn_new();
    if (d) {
        d->table = table; d->count = count; d->max = count; d->base = base;
        d->lo = lo; d->hi = hi; d->id = 0; d->callback = 0; d->context = 0;
        d->kind = RT_DYN_STATIC;
    }
    rt_dyn_unlock();
    return d != 0;
}

BOOLEAN rt_RtlInstallFunctionTableCallback(DWORD64 id, DWORD64 base, DWORD length,
                                           PGET_RUNTIME_FUNCTION_CALLBACK callback,
                                           PVOID context, PCWSTR dll) {
    (void)dll;
    if ((id & 3) != 3 || !callback || !length) return FALSE;
    rt_dyn_lock();
    struct rt_dyn *d = rt_dyn_new();
    if (d) {
        d->table = 0; d->count = 0; d->max = 0; d->base = base;
        d->lo = base; d->hi = base + length; d->id = id; d->callback = callback; d->context = context;
        d->kind = RT_DYN_CALLBACK;
    }
    rt_dyn_unlock();
    return d != 0;
}

BOOLEAN rt_RtlDeleteFunctionTable(PRUNTIME_FUNCTION table) {
    BOOLEAN done = FALSE;
    rt_dyn_lock();
    for (int i = 0; i < RT_DYN_MAX && !done; i++) {
        struct rt_dyn *d = &g_dyn[i];
        if ((d->kind == RT_DYN_STATIC && d->table == table) ||
            (d->kind == RT_DYN_CALLBACK && d->id == (ULONG64)table)) {
            d->kind = RT_DYN_FREE;
            done = TRUE;
        }
    }
    rt_dyn_unlock();
    return done;
}

DWORD rt_RtlAddGrowableFunctionTable(PVOID *handle, PRUNTIME_FUNCTION table, DWORD count,
                                     DWORD max, ULONG_PTR range_base, ULONG_PTR range_end) {
    if (!handle || !table || count > max) return 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
    rt_dyn_lock();
    struct rt_dyn *d = rt_dyn_new();
    if (d) {
        d->table = table; d->count = count; d->max = max; d->base = range_base;
        d->lo = range_base; d->hi = range_end; d->id = 0; d->callback = 0; d->context = 0;
        d->kind = RT_DYN_GROWABLE;
    }
    rt_dyn_unlock();
    if (!d) return 0xC0000017u;                                   /* STATUS_NO_MEMORY */
    *handle = d;
    return 0;
}

void rt_RtlGrowFunctionTable(PVOID handle, DWORD count) {
    struct rt_dyn *d = (struct rt_dyn *)handle;
    if (d && d->kind == RT_DYN_GROWABLE && count <= d->max && count >= d->count) d->count = count;
}

void rt_RtlDeleteGrowableFunctionTable(PVOID handle) {
    struct rt_dyn *d = (struct rt_dyn *)handle;
    rt_dyn_lock();
    if (d >= g_dyn && d < g_dyn + RT_DYN_MAX && d->kind == RT_DYN_GROWABLE) d->kind = RT_DYN_FREE;
    rt_dyn_unlock();
}

/* The entry for pc in the run-time tables, or NULL; *base its RVAs' base. */
static PRUNTIME_FUNCTION rt_dyn_lookup(ULONG64 pc, PULONG64 base) {
    PGET_RUNTIME_FUNCTION_CALLBACK cb = 0;
    PVOID cb_ctx = 0;
    ULONG64 cb_base = 0;
    PRUNTIME_FUNCTION found = 0;
    rt_dyn_lock();
    for (int i = 0; i < RT_DYN_MAX && !found && !cb; i++) {
        struct rt_dyn *d = &g_dyn[i];
        if (d->kind == RT_DYN_FREE || pc < d->lo || pc >= d->hi) continue;
        if (d->kind == RT_DYN_CALLBACK) {
            cb = d->callback; cb_ctx = d->context; cb_base = d->base;
            continue;
        }
        DWORD rva = (DWORD)(pc - d->base);
        for (DWORD k = 0; k < d->count; k++) {
            if (rva >= d->table[k].BeginAddress && rva < d->table[k].EndAddress) {
                found = &d->table[k];
                if (base) *base = d->base;
                break;
            }
        }
    }
    rt_dyn_unlock();
    if (!found && cb) {                       /* called without the lock */
        found = cb(pc, cb_ctx);
        if (found && base) *base = cb_base;
    }
    return found;
}

/* RtlLookupFunctionEntry: the function table entry for pc (a binary search
 * of its image's array, which is sorted by address), NULL for none -- a leaf
 * function; a pc no image holds, the run-time tables' (W7e). The history
 * table is optional and not used. */
/* W7g: a pc in the shim area -- one of the loader's CPL3 stubs: its entry in
 * the stub function table (pe.h, WIN32_STUBTAB_VA), or none (the top of the
 * stack: a thread's first frame). */
static PRUNTIME_FUNCTION rt_stub_lookup(ULONG64 pc) {
    const DWORD *h = (const DWORD *)RT_STUBTAB;
    if (h[1] != RT_STUBTAB_MAGIC || h[0] > 16) return 0;
    RUNTIME_FUNCTION *t = (RUNTIME_FUNCTION *)(RT_STUBTAB + 0x10);
    DWORD rva = (DWORD)(pc - RT_SHIM_LO);
    for (DWORD i = 0; i < h[0]; i++)
        if (rva >= t[i].BeginAddress && rva < t[i].EndAddress) return &t[i];
    return 0;
}

PRUNTIME_FUNCTION rt_RtlLookupFunctionEntry(ULONG64 pc, PULONG64 image_base,
                                            PUNWIND_HISTORY_TABLE hist) {
    (void)hist;
    if (rt_is_system_pc(pc)) {
        PRUNTIME_FUNCTION f = rt_stub_lookup(pc);
        if (image_base) *image_base = f ? RT_SHIM_LO : 0;
        return f;
    }
    ULONG64 size = 0, base = rt_module_of(pc, &size, 0);
    if (image_base) *image_base = base;
    if (!base) {                              /* W7e: code made at run time */
        ULONG64 db = 0;
        PRUNTIME_FUNCTION f = rt_dyn_lookup(pc, &db);
        if (image_base) *image_base = f ? db : 0;
        return f;
    }
    DWORD n = 0;
    const RUNTIME_FUNCTION *t = rt_pdata(base, size, &n);
    if (!t) return 0;
    DWORD rva = (DWORD)(pc - base), lo = 0, hi = n;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        if (rva < t[mid].BeginAddress) hi = mid;
        else if (rva >= t[mid].EndAddress) lo = mid + 1;
        else return (PRUNTIME_FUNCTION)&t[mid];
    }
    return 0;
}

/* ---- the unwinder ------------------------------------------------------- */

#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3
#define UWOP_SAVE_NONVOL     4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG          6          /* version 2: an epilog's location */
#define UWOP_SAVE_XMM128     8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME  10

typedef struct {
    BYTE   VerFlags;                    /* Version:3, Flags:5 */
    BYTE   SizeOfProlog;
    BYTE   CountOfCodes;
    BYTE   FrameRegOff;                 /* FrameRegister:4, FrameOffset:4 */
    USHORT Code[];                      /* CodeOffset:8, UnwindOp:4, OpInfo:4 */
} RT_UNWIND_INFO;

/* The slots an unwind code takes, or -1 for one this unwinder does not know. */
static int rt_slots(int op, int info, int ver) {
    switch (op) {
    case UWOP_PUSH_NONVOL:     return 1;
    case UWOP_ALLOC_LARGE:     return info == 0 ? 2 : info == 1 ? 3 : -1;
    case UWOP_ALLOC_SMALL:     return 1;
    case UWOP_SET_FPREG:       return 1;
    case UWOP_SAVE_NONVOL:     return 2;
    case UWOP_SAVE_NONVOL_FAR: return 3;
    case UWOP_EPILOG:          return ver >= 2 ? 1 : -1;
    case UWOP_SAVE_XMM128:     return 2;
    case UWOP_SAVE_XMM128_FAR: return 3;
    case UWOP_PUSH_MACHFRAME:  return 1;
    default:                   return -1;
    }
}

/* If pc is in an epilog -- the trailing part of `add rsp, n` or `lea rsp,
 * n[frame register]`, then pops, then ret / ret n / rep ret / a jmp out of
 * the function / jmp qword ptr [rip+n] / rex.w jmp reg (the forms the
 * documentation allows) -- carry out the rest of it on ctx and return 1. */
static int rt_epilog(ULONG64 pc, ULONG64 fn_lo, ULONG64 fn_hi, int fpreg, CONTEXT *ctx,
                     KNONVOLATILE_CONTEXT_POINTERS *ptrs) {
    const BYTE *p = (const BYTE *)pc;
    ULONG64 *R = &ctx->Rax;
    int kind = 0;
    LONG64 imm = 0;
    if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xC4) {
        kind = 1; imm = (signed char)p[3]; p += 4;
    } else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xC4) {
        kind = 1; imm = *(const LONG *)(p + 3); p += 7;
    } else if (fpreg && (p[0] == 0x48 || p[0] == 0x49) && p[1] == 0x8D) {
        int modrm = p[2], mod = modrm >> 6, reg = (modrm >> 3) & 7;
        int rm = (modrm & 7) | ((p[0] & 1) << 3);
        if (reg != 4 || rm != fpreg || (modrm & 7) == 4 || mod == 0 || mod == 3) return 0;
        if (mod == 1) { imm = (signed char)p[3]; p += 4; }
        else          { imm = *(const LONG *)(p + 3); p += 7; }
        kind = 2;
    }
    const BYTE *q = p;
    for (;;) {
        if (q[0] >= 0x58 && q[0] <= 0x5F) q += 1;
        else if (q[0] == 0x41 && q[1] >= 0x58 && q[1] <= 0x5F) q += 2;
        else break;
    }
    int term = 0;
    ULONG64 retn = 0;
    if (q[0] == 0xC3) term = 1;
    else if (q[0] == 0xC2) { term = 1; retn = *(const USHORT *)(q + 1); }
    else if (q[0] == 0xF3 && q[1] == 0xC3) term = 1;
    else if (q[0] == 0xE9) {
        ULONG64 t = (ULONG64)(q + 5) + (LONG64)*(const LONG *)(q + 1);
        term = t < fn_lo || t >= fn_hi;
    } else if (q[0] == 0xEB) {
        ULONG64 t = (ULONG64)(q + 2) + (LONG64)(signed char)q[1];
        term = t < fn_lo || t >= fn_hi;
    } else if (q[0] == 0xFF && q[1] == 0x25) term = 1;
    else if ((q[0] == 0x48 || q[0] == 0x49) && q[1] == 0xFF && (q[2] & 0xF8) == 0xE0) term = 1;
    if (!term) return 0;
    if (kind == 1) ctx->Rsp += (ULONG64)imm;
    else if (kind == 2) ctx->Rsp = R[fpreg] + (ULONG64)imm;
    for (const BYTE *s = p; s < q; ) {
        int r;
        if (s[0] == 0x41) { r = 8 + (s[1] - 0x58); s += 2; }
        else              { r = s[0] - 0x58;       s += 1; }
        R[r] = *(const ULONG64 *)ctx->Rsp;
        if (ptrs) ptrs->IntegerContext[r] = (PULONG64)ctx->Rsp;
        ctx->Rsp += 8;
    }
    ctx->Rip = *(const ULONG64 *)ctx->Rsp;
    ctx->Rsp += 8 + retn;
    return 1;
}

/* RtlVirtualUnwind's work: unwind ctx out of the function fe describes (pc in
 * it), and return its handler of `type` (UNW_FLAG_EHANDLER / _UHANDLER) when
 * pc is in its body -- none in the prolog or an epilog. *bad on unwind
 * information this unwinder cannot follow. */
static PEXCEPTION_ROUTINE rt_vunwind(DWORD type, ULONG64 base, ULONG64 pc,
                                     const RUNTIME_FUNCTION *fe, CONTEXT *ctx,
                                     PVOID *hdata, PULONG64 est_out,
                                     KNONVOLATILE_CONTEXT_POINTERS *ptrs, int *bad) {
    ULONG64 *R = &ctx->Rax;
    M128A *X = &ctx->Xmm0;
    const RT_UNWIND_INFO *ui = (const RT_UNWIND_INFO *)(base + fe->UnwindData);
    int ver = ui->VerFlags & 7;
    *hdata = 0;
    *est_out = ctx->Rsp;
    if (ver != 1 && ver != 2) { *bad = 1; return 0; }
    ULONG64 fn = base + fe->BeginAddress, off = pc - fn;
    int fpreg = ui->FrameRegOff & 15;
    ULONG64 fpoff = (ULONG64)(ui->FrameRegOff >> 4) * 16;
    /* "within the prolog if the distance from the function start to the RIP
     * is less than or equal to the prolog size" */
    int in_prolog = off <= ui->SizeOfProlog;

    /* the establisher frame: the base of the fixed allocation -- the frame
     * register less its offset once the prolog has set it, else RSP */
    ULONG64 est = ctx->Rsp;
    if (fpreg) {
        int set = 1;
        if (in_prolog) {
            for (int i = 0; i < ui->CountOfCodes; ) {
                USHORT c = ui->Code[i];
                int op = (c >> 8) & 15, s = rt_slots(op, c >> 12, ver);
                if (s <= 0) break;
                if (op == UWOP_SET_FPREG && (c & 0xFF) > off) { set = 0; break; }
                i += s;
            }
        }
        if (set) est = R[fpreg] - fpoff;
    }
    *est_out = est;

    if (!in_prolog && rt_epilog(pc, fn, base + fe->EndAddress, fpreg, ctx, ptrs)) return 0;

    const RT_UNWIND_INFO *cur = ui;
    int first = 1, mach = 0;
    for (int depth = 0; ; depth++) {
        int n = cur->CountOfCodes, cver = cur->VerFlags & 7;
        for (int i = 0; i < n; ) {
            USHORT c = cur->Code[i];
            int op = (c >> 8) & 15, info = c >> 12, s = rt_slots(op, info, cver);
            if (s <= 0 || i + s > n) { *bad = 1; return 0; }
            /* in the prolog, only what has been done is undone */
            if (first && in_prolog && (ULONG64)(c & 0xFF) > off) { i += s; continue; }
            switch (op) {
            case UWOP_PUSH_NONVOL:
                R[info] = *(const ULONG64 *)ctx->Rsp;
                if (ptrs) ptrs->IntegerContext[info] = (PULONG64)ctx->Rsp;
                ctx->Rsp += 8;
                break;
            case UWOP_ALLOC_LARGE:
                ctx->Rsp += info ? (ULONG64)(cur->Code[i + 1] | ((ULONG)cur->Code[i + 2] << 16))
                                 : (ULONG64)cur->Code[i + 1] * 8;
                break;
            case UWOP_ALLOC_SMALL:
                ctx->Rsp += (ULONG64)info * 8 + 8;
                break;
            case UWOP_SET_FPREG:
                ctx->Rsp = R[cur->FrameRegOff & 15] - (ULONG64)(cur->FrameRegOff >> 4) * 16;
                break;
            case UWOP_SAVE_NONVOL:
            case UWOP_SAVE_NONVOL_FAR: {
                ULONG64 o = op == UWOP_SAVE_NONVOL ? (ULONG64)cur->Code[i + 1] * 8
                          : (ULONG64)(cur->Code[i + 1] | ((ULONG)cur->Code[i + 2] << 16));
                PULONG64 a = (PULONG64)(est + o);
                R[info] = *a;
                if (ptrs) ptrs->IntegerContext[info] = a;
                break;
            }
            case UWOP_SAVE_XMM128:
            case UWOP_SAVE_XMM128_FAR: {
                ULONG64 o = op == UWOP_SAVE_XMM128 ? (ULONG64)cur->Code[i + 1] * 16
                          : (ULONG64)(cur->Code[i + 1] | ((ULONG)cur->Code[i + 2] << 16));
                PM128A a = (PM128A)(est + o);
                X[info] = *a;
                if (ptrs) ptrs->FloatingContext[info] = a;
                break;
            }
            case UWOP_PUSH_MACHFRAME:
                if (info) ctx->Rsp += 8;                    /* the error code */
                ctx->Rip = *(const ULONG64 *)ctx->Rsp;
                ctx->Rsp = *(const ULONG64 *)(ctx->Rsp + 24);
                mach = 1;
                break;
            default:                                        /* UWOP_EPILOG */
                break;
            }
            i += s;
        }
        if (!((cur->VerFlags >> 3) & UNW_FLAG_CHAININFO)) break;
        if (depth >= 32) { *bad = 1; return 0; }
        /* chained: the codes of the primary entry, all of them */
        const RUNTIME_FUNCTION *ch = (const RUNTIME_FUNCTION *)&cur->Code[(n + 1) & ~1];
        cur = (const RT_UNWIND_INFO *)(base + ch->UnwindData);
        first = 0;
    }
    if (!mach) {
        ctx->Rip = *(const ULONG64 *)ctx->Rsp;
        ctx->Rsp += 8;
    }
    int flags = cur->VerFlags >> 3;
    if (in_prolog || !(flags & type & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))) return 0;
    const ULONG *h = (const ULONG *)&cur->Code[(cur->CountOfCodes + 1) & ~1];
    *hdata = (PVOID)(h + 1);
    return (PEXCEPTION_ROUTINE)(base + h[0]);
}

PEXCEPTION_ROUTINE rt_RtlVirtualUnwind(DWORD type, ULONG64 base, ULONG64 pc,
                                       PRUNTIME_FUNCTION fe, PCONTEXT ctx, PVOID *hdata,
                                       PULONG64 est, PKNONVOLATILE_CONTEXT_POINTERS ptrs) {
    PVOID hd;
    ULONG64 e;
    int bad = 0;
    PEXCEPTION_ROUTINE r = rt_vunwind(type, base, pc, fe, ctx, hdata ? hdata : &hd,
                                      est ? est : &e, ptrs, &bad);
    return bad ? 0 : r;
}

/* ---- raising ------------------------------------------------------------ */

static __attribute__((noreturn)) void rt_terminate(DWORD code) {
    for (;;) TerminateProcess(GetCurrentProcess(), code);
}

/* A status the dispatcher or the unwinder raises: noncontinuable, no
 * parameters, no record chained (as Windows 11 does it). */
static __attribute__((noreturn)) void rt_raise_status(DWORD code) {
    rt_RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, 0);
    rt_terminate(code);
}

/* 1 when a handler took the exception (continue at orig), 0 when none did.
 * The vectored handlers first; a vectored handler's CONTINUE_EXECUTION
 * continues even a noncontinuable exception (as Windows 11 does). Whatever
 * continues it, the continue handlers run before. */
__attribute__((noinline)) static int rt_dispatch(EXCEPTION_RECORD *rec, CONTEXT *orig) {
    EXCEPTION_POINTERS ep = { rec, orig };
    if (rt_veh_call(0, &ep) == EXCEPTION_CONTINUE_EXECUTION) {
        (void)rt_veh_call(1, &ep);
        return 1;
    }
    CONTEXT ctx = *orig;
    ULONG64 lo = orig->Rsp, hi = rt_stack_base(), nested = 0;
    for (int depth = 0; depth < 100000; depth++) {
        ULONG64 pc = ctx.Rip;
        if (!pc) return 0;                                  /* the top of the stack */
        if (ctx.Rsp < lo || ctx.Rsp >= hi || (ctx.Rsp & 7)) break;
        ULONG64 base;
        PRUNTIME_FUNCTION fe = rt_RtlLookupFunctionEntry(pc, &base, 0);
        if (!fe && rt_is_system_pc(pc)) return 0;           /* so is a stub with no entry (W7g) */
        if (!fe) {                                          /* a leaf: [rsp] is the return */
            ctx.Rip = *(const ULONG64 *)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        PVOID hdata;
        ULONG64 est;
        int bad = 0;
        PEXCEPTION_ROUTINE h = rt_vunwind(UNW_FLAG_EHANDLER, base, pc, fe, &ctx, &hdata, &est, 0, &bad);
        if (bad || est < lo || est >= hi || (est & 7)) break;
        if (!h) continue;
        /* the handler gets the raise's context; the dispatcher context
         * carries the caller's (the frame unwound) */
        DISPATCHER_CONTEXT dc;
        dc.ControlPc = pc;
        dc.ImageBase = base;
        dc.FunctionEntry = fe;
        dc.EstablisherFrame = est;
        dc.TargetIp = 0;
        dc.ContextRecord = &ctx;
        dc.LanguageHandler = h;
        dc.HandlerData = hdata;
        dc.HistoryTable = 0;
        dc.ScopeIndex = 0;
        dc.Fill0 = 0;
        EXCEPTION_DISPOSITION d = rt_call_dispatch_handler(rec, est, orig, &dc);
        if (nested && est == nested) {
            rec->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
            nested = 0;
        }
        switch ((int)d) {
        case ExceptionContinueExecution:
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) rt_raise_status(RT_STATUS_NONCONTINUABLE);
            (void)rt_veh_call(1, &ep);
            return 1;
        case ExceptionContinueSearch:
            break;
        case ExceptionNestedException:
            rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
            if (dc.EstablisherFrame > nested) nested = dc.EstablisherFrame;
            break;
        default:
            rt_raise_status(RT_STATUS_INVALID_DISPOSITION);
        }
    }
    rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
    return 0;
}

/* The stack-wide unwind of an exception no handler took, or of an exit
 * unwind: every frame's termination handler, to the top. */
__attribute__((noinline)) static void rt_unwind_loop(ULONG64 target, ULONG64 tip,
                                                     EXCEPTION_RECORD *rec, ULONG64 retval,
                                                     CONTEXT *cur, CONTEXT *param,
                                                     PUNWIND_HISTORY_TABLE hist, int to_end);

/* tobyOS's Application Error box: one line on stderr -- none when the process
 * asked for no box (SEM_NOGPFAULTERRORBOX), as on Windows. */
static void rt_put(char *buf, int *k, const char *s) { while (*s && *k < 190) buf[(*k)++] = *s++; }
static void rt_hex(char *buf, int *k, ULONG64 v, int digits) {
    for (int i = digits - 1; i >= 0 && *k < 190; i--) buf[(*k)++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
}
static void rt_report(const EXCEPTION_RECORD *rec) {
    if (g_error_mode & SEM_NOGPFAULTERRORBOX) return;
    char b[200];
    int k = 0;
    ULONG64 at = (ULONG64)rec->ExceptionAddress, size = 0;
    const char *name = 0;
    ULONG64 base = rt_module_of(at, &size, &name);
    rt_put(b, &k, "unhandled exception 0x");
    rt_hex(b, &k, rec->ExceptionCode, 8);
    rt_put(b, &k, " at 0x");
    rt_hex(b, &k, at, 12);
    if (base && name) {
        rt_put(b, &k, " (");
        rt_put(b, &k, name);
        rt_put(b, &k, "+0x");
        rt_hex(b, &k, at - base, 6);
        rt_put(b, &k, ")");
    }
    rt_put(b, &k, "\n");
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, (DWORD)k, &w, 0);
}

/* After every frame's handler has said no: the top-level filter, as
 * RtlUserThreadStart's frame runs it on Windows. CONTINUE_EXECUTION goes on at
 * the raise; anything else unwinds the whole stack (termination handlers
 * run) and ends the process with the exception's code -- without
 * DLL_PROCESS_DETACH, from any thread (measured: logs/w32/w7b-host-run.txt). */
__attribute__((noinline, noreturn)) static void rt_unhandled(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
    if (!(rec->ExceptionFlags & EXCEPTION_STACK_INVALID)) {
        EXCEPTION_POINTERS ep = { rec, ctx };
        LONG r = rt_UnhandledExceptionFilter(&ep);
        if (r == EXCEPTION_CONTINUE_EXECUTION) {
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) rt_raise_status(RT_STATUS_NONCONTINUABLE);
            (void)rt_veh_call(1, &ep);
            rt_restore_context(ctx, 0);
        }
    } else {
        rt_report(rec);
    }
    CONTEXT here;
    rt_capture_context(&here);
    rec->ExceptionFlags = EXCEPTION_UNWINDING;
    rt_unwind_loop(0, 0, rec, 0, &here, 0, 0, 1);
    rt_terminate(rec->ExceptionCode);
}

/* RtlRaiseException: dispatch rec from the caller's context (its
 * ExceptionAddress set to the caller's pc); return there if a handler
 * continues it. */
__attribute__((noinline)) void rt_RtlRaiseException(EXCEPTION_RECORD *rec) {
    CONTEXT ctx;
    rt_capture_context(&ctx);
    ULONG64 base;
    PRUNTIME_FUNCTION fe = rt_RtlLookupFunctionEntry(ctx.Rip, &base, 0);
    PVOID hd;
    ULONG64 est;
    int bad = 0;
    if (!fe) rt_terminate(RT_STATUS_BAD_STACK);
    (void)rt_vunwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &hd, &est, 0, &bad);
    if (bad) rt_terminate(RT_STATUS_BAD_STACK);
    rec->ExceptionAddress = (PVOID)ctx.Rip;
    if (rt_dispatch(rec, &ctx)) rt_restore_context(&ctx, 0);
    rt_unhandled(rec, &ctx);
}

/* W7d: a CPU fault, from rt_KiUserExceptionDispatcher (tobyrt_s.S): the
 * kernel (src/win32_seh.c) put the record and the fault's context on the
 * stack. Dispatched as a raise is; a handler that continues it continues at
 * the context -- the faulting instruction again, unless the handler moved
 * Rip -- and nobody taking it ends the process with its code. */
__attribute__((noreturn)) void rt_dispatch_fault(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
    if (rt_dispatch(rec, ctx)) rt_restore_context(ctx, 0);
    rt_unhandled(rec, ctx);
}

/* RtlRaiseException comes back to its caller only through a restored context
 * (every path in it ends in a noreturn call), so the compiler takes it for
 * noreturn and puts an int3 after a direct call to it -- which a handler's
 * ExceptionContinueExecution then lands on. Called through a volatile pointer
 * (a local: no fixup), its caller keeps the code that returns. */

/* RaiseException: a record of the code, EXCEPTION_NONCONTINUABLE from the
 * flags (with EXCEPTION_SOFTWARE_ORIGINATE, as Windows 11 marks it) and at
 * most EXCEPTION_MAXIMUM_PARAMETERS arguments. */
void rt_RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args) {
    EXCEPTION_RECORD rec;
    __builtin_memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = code;
    rec.ExceptionFlags = (flags & EXCEPTION_NONCONTINUABLE) | EXCEPTION_SOFTWARE_ORIGINATE;
    if (args) {
        if (nargs > EXCEPTION_MAXIMUM_PARAMETERS) nargs = EXCEPTION_MAXIMUM_PARAMETERS;
        for (DWORD i = 0; i < nargs; i++) rec.ExceptionInformation[i] = args[i];
        rec.NumberParameters = nargs;
    }
    void (*volatile raise)(EXCEPTION_RECORD *) = rt_RtlRaiseException;
    raise(&rec);
}

/* rt_call_dispatch_handler's handler: an exception raised inside a handler
 * the dispatcher called. Tell its dispatch where the first one had got to;
 * frames up to there see EXCEPTION_NESTED_CALL. */
EXCEPTION_DISPOSITION rt_dispatch_nested(EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx,
                                         DISPATCHER_CONTEXT *dc) {
    (void)rec; (void)ctx;
    const DISPATCHER_CONTEXT *outer = *(DISPATCHER_CONTEXT *const *)(frame + RT_HELPER_DC_SLOT);
    dc->EstablisherFrame = outer->EstablisherFrame;
    return ExceptionNestedException;
}

/* rt_call_unwind_handler's handler: an unwind that met another one being
 * carried out (RtlUnwindEx from a handler the unwinder called). It goes on
 * from the frame the other one was unwinding, whose handler is called again,
 * flagged EXCEPTION_COLLIDED_UNWIND, with its ScopeIndex as it was. */
EXCEPTION_DISPOSITION rt_unwind_collided(EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx,
                                         DISPATCHER_CONTEXT *dc) {
    (void)rec; (void)ctx;
    const DISPATCHER_CONTEXT *outer = *(DISPATCHER_CONTEXT *const *)(frame + RT_HELPER_DC_SLOT);
    dc->ControlPc = outer->ControlPc;
    dc->ImageBase = outer->ImageBase;
    dc->FunctionEntry = outer->FunctionEntry;
    dc->EstablisherFrame = outer->EstablisherFrame;
    dc->ContextRecord = outer->ContextRecord;
    dc->LanguageHandler = outer->LanguageHandler;
    dc->HandlerData = outer->HandlerData;
    dc->HistoryTable = outer->HistoryTable;
    dc->ScopeIndex = outer->ScopeIndex;
    return ExceptionCollidedUnwind;
}

/* ---- unwinding ---------------------------------------------------------- */

/* Walk from cur (the frame RtlUnwindEx was called from) up to target, calling
 * each termination handler with cur -- the frame's own context, which is
 * also DispatcherContext->ContextRecord -- then continue at tip in the
 * target frame with rax = retval. to_end: no target; return at the top.
 *
 * cur is the unwind's own storage. The caller's ContextRecord (param) is not
 * touched until a collision: it is often the very buffer the unwind being
 * collided with is working in (a handler passes its ContextRecord on -- as
 * libgcc does), so writing it first would destroy the context the collision
 * takes over. At a collision that context is copied into param (nothing to
 * do when it is the same buffer), and the walk goes on in it. */
__attribute__((noinline)) static void rt_unwind_loop(ULONG64 target, ULONG64 tip,
                                                     EXCEPTION_RECORD *rec, ULONG64 retval,
                                                     CONTEXT *cur, CONTEXT *param,
                                                     PUNWIND_HISTORY_TABLE hist, int to_end) {
    CONTEXT next;
    ULONG64 lo = cur->Rsp, hi = rt_stack_base();
    for (int depth = 0; ; depth++) {
        ULONG64 pc = cur->Rip;
        ULONG64 base = 0;
        PRUNTIME_FUNCTION fe = pc ? rt_RtlLookupFunctionEntry(pc, &base, hist) : 0;
        if (!pc || (!fe && rt_is_system_pc(pc)) || depth > 100000) {   /* the top (W7g: a stub with no entry) */
            if (to_end) return;
            rt_raise_status(RT_STATUS_BAD_FUNCTION_TABLE);      /* W7f: as Windows 11 */
        }
        if (cur->Rsp < lo || cur->Rsp >= hi || (cur->Rsp & 7)) rt_raise_status(RT_STATUS_BAD_STACK);
        next = *cur;
        if (!fe) {
            next.Rip = *(const ULONG64 *)next.Rsp;
            next.Rsp += 8;
            *cur = next;
            continue;
        }
        PVOID hdata;
        ULONG64 est;
        int bad = 0;
        PEXCEPTION_ROUTINE h = rt_vunwind(UNW_FLAG_UHANDLER, base, pc, fe, &next, &hdata, &est, 0, &bad);
        if (bad || est < lo || est >= hi || (est & 7)) rt_raise_status(RT_STATUS_BAD_STACK);
        if (!to_end && target && est > target) rt_raise_status(RT_STATUS_BAD_STACK);
        if (h) {
            ULONG scope = 0;
            for (;;) {
                if (!to_end && est == target) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
                DISPATCHER_CONTEXT dc;
                dc.ControlPc = pc;
                dc.ImageBase = base;
                dc.FunctionEntry = fe;
                dc.EstablisherFrame = est;
                dc.TargetIp = tip;
                dc.ContextRecord = cur;
                dc.LanguageHandler = h;
                dc.HandlerData = hdata;
                dc.HistoryTable = hist;
                dc.ScopeIndex = scope;
                dc.Fill0 = 0;
                EXCEPTION_DISPOSITION d = rt_call_unwind_handler(rec, est, cur, &dc);
                rec->ExceptionFlags &= ~(EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND);
                if (d == ExceptionContinueSearch) break;
                if ((int)d != ExceptionCollidedUnwind) rt_raise_status(RT_STATUS_INVALID_DISPOSITION);
                /* collided: take over the other unwind at the frame it was on */
                pc = dc.ControlPc;
                base = dc.ImageBase;
                fe = dc.FunctionEntry;
                est = dc.EstablisherFrame;
                h = dc.LanguageHandler;
                hdata = dc.HandlerData;
                hist = dc.HistoryTable;
                scope = dc.ScopeIndex;
                if (param) cur = param;
                if (cur != dc.ContextRecord) *cur = *dc.ContextRecord;
                next = *cur;
                PVOID hd2;
                ULONG64 est2;
                int bad2 = 0;
                (void)rt_vunwind(UNW_FLAG_NHANDLER, base, pc, fe, &next, &hd2, &est2, 0, &bad2);
                if (bad2) rt_raise_status(RT_STATUS_BAD_STACK);
                rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
            }
        }
        if (!to_end && est == target) break;
        *cur = next;
    }
    /* W7f: a consolidation goes on in the target frame where its callback
     * says, so the context keeps the frame's own pc (a walk from the
     * callback goes through it: programs/win-exc37) */
    cur->Rax = retval;
    if (rec->ExceptionCode != RT_STATUS_UNWIND_CONSOLIDATE) cur->Rip = tip;
    rt_RtlRestoreContext(cur, rec);
}

/* RtlUnwindEx: unwind from the caller up to target_frame, then continue at
 * target_ip with rax = retval. ctx is storage for the unwind's context,
 * used from a collision on (rt_unwind_loop); the record's flags become
 * EXCEPTION_UNWINDING (| EXCEPTION_EXIT_UNWIND with no target), dropping the
 * dispatch's -- as Windows 11 does. */
__attribute__((noinline)) void rt_RtlUnwindEx(PVOID target_frame, PVOID target_ip,
                                              EXCEPTION_RECORD *rec, PVOID retval,
                                              CONTEXT *ctx, PUNWIND_HISTORY_TABLE hist) {
    EXCEPTION_RECORD local;
    CONTEXT own;
    rt_capture_context(&own);
    if (!rec) {
        __builtin_memset(&local, 0, sizeof local);
        local.ExceptionCode = RT_STATUS_UNWIND;
        local.ExceptionAddress = (PVOID)own.Rip;
        rec = &local;
    }
    rec->ExceptionFlags = EXCEPTION_UNWINDING | (target_frame ? 0 : EXCEPTION_EXIT_UNWIND);
    rt_unwind_loop((ULONG64)target_frame, (ULONG64)target_ip, rec, (ULONG64)retval, &own, ctx,
                   hist, 0);
}

/* RtlUnwind: RtlUnwindEx with a context of its own. */
__attribute__((noinline)) void rt_RtlUnwind(PVOID target_frame, PVOID target_ip,
                                            EXCEPTION_RECORD *rec, PVOID retval) {
    CONTEXT ctx;
    rt_RtlUnwindEx(target_frame, target_ip, rec, retval, &ctx, 0);
}

/* ---- RtlRestoreContext, setjmp and longjmp (W7f) ---------------------- */

/* The x64 _JUMP_BUFFER (setjmp.h): what setjmp saves (tobyrt_s.S, rt_setjmp). */
struct rt_jump_buffer {
    ULONG64 Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12, R13, R14, R15, Rip;
    ULONG MxCsr;
    USHORT FpCsr, Spare;
    M128A Xmm6, Xmm7, Xmm8, Xmm9, Xmm10, Xmm11, Xmm12, Xmm13, Xmm14, Xmm15;
};
_Static_assert(__builtin_offsetof(struct rt_jump_buffer, Rip) == 0x50 &&
               __builtin_offsetof(struct rt_jump_buffer, MxCsr) == 0x58 &&
               __builtin_offsetof(struct rt_jump_buffer, FpCsr) == 0x5C &&
               __builtin_offsetof(struct rt_jump_buffer, Xmm6) == 0x60 &&
               sizeof(struct rt_jump_buffer) == 0x100, "the x64 _JUMP_BUFFER's layout");

/* RtlRestoreContext: continue at ctx. Two records change what that means
 * (measured on Windows 11: programs/win-exc37, logs/w32/w7f-host-run.txt):
 *   STATUS_LONGJUMP -- the registers setjmp saved, from the jump buffer its
 *     first parameter points at: the integer nonvolatiles, Rsp, Rip,
 *     XMM6-15, MXCSR and the x87 control word (rax stays: longjmp's value);
 *   STATUS_UNWIND_CONSOLIDATE -- the callback its first parameter is,
 *     called with the record from a frame below the stack pointer that
 *     unwinds to ctx (tobyrt_s.S, rt_consolidate): a walk from inside the
 *     callback goes on in ctx's frame. It returns where to continue in ctx. */
__attribute__((noreturn)) void rt_RtlRestoreContext(CONTEXT *ctx, EXCEPTION_RECORD *rec) {
    if (rec && rec->ExceptionCode == RT_STATUS_LONGJUMP && rec->NumberParameters >= 1) {
        const struct rt_jump_buffer *jb = (const struct rt_jump_buffer *)rec->ExceptionInformation[0];
        if (!(ctx->ContextFlags & 8)) {             /* no x87/SSE state in it: the current one */
            CONTEXT now;
            rt_capture_context(&now);
            ctx->FltSave = now.FltSave;
            ctx->ContextFlags |= CONTEXT_FLOATING_POINT;
        }
        ctx->Rbx = jb->Rbx;
        ctx->Rsp = jb->Rsp;
        ctx->Rbp = jb->Rbp;
        ctx->Rsi = jb->Rsi;
        ctx->Rdi = jb->Rdi;
        ctx->R12 = jb->R12;
        ctx->R13 = jb->R13;
        ctx->R14 = jb->R14;
        ctx->R15 = jb->R15;
        ctx->Rip = jb->Rip;
        ctx->MxCsr = jb->MxCsr;
        ctx->FltSave.MxCsr = jb->MxCsr;
        ctx->FltSave.ControlWord = jb->FpCsr;
        ctx->Xmm6 = jb->Xmm6;
        ctx->Xmm7 = jb->Xmm7;
        ctx->Xmm8 = jb->Xmm8;
        ctx->Xmm9 = jb->Xmm9;
        ctx->Xmm10 = jb->Xmm10;
        ctx->Xmm11 = jb->Xmm11;
        ctx->Xmm12 = jb->Xmm12;
        ctx->Xmm13 = jb->Xmm13;
        ctx->Xmm14 = jb->Xmm14;
        ctx->Xmm15 = jb->Xmm15;
    } else if (rec && rec->ExceptionCode == RT_STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 1) {
        rt_consolidate(ctx, rec);
    }
    rt_restore_context(ctx, rec);
}

/* longjmp: back to where setjmp saved jb, value what it returns there (0
 * comes back 1). A buffer with a frame -- mingw's setjmp is _setjmp(buf, its
 * frame) -- is unwound to: RtlUnwind to that frame with a STATUS_LONGJUMP
 * record whose parameter is the buffer, so every __finally between runs
 * (abnormal termination), the frame's own too (it is the target, and the
 * buffer's Rip is outside its __try), and RtlRestoreContext takes the
 * registers from the buffer. A frame that is no frame's establisher is
 * STATUS_BAD_STACK, raised by the unwind. A frame of 0: straight there, no
 * unwind. (Measured on Windows 11: programs/win-exc37.) */
__attribute__((noreturn)) void rt_longjmp(const struct rt_jump_buffer *jb, int value) {
    if (!value) value = 1;
    if (!jb->Frame) rt_longjmp_direct(jb, value);
    EXCEPTION_RECORD rec;
    __builtin_memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = RT_STATUS_LONGJUMP;
    rec.NumberParameters = 1;
    rec.ExceptionInformation[0] = (ULONG_PTR)jb;
    rt_RtlUnwind((PVOID)jb->Frame, (PVOID)jb->Rip, &rec, (PVOID)(ULONG_PTR)(ULONG)value);
    rt_terminate(RT_STATUS_BAD_STACK);              /* an unwind does not come back */
}

/* ---- the loader's DllMain calls (W7g) ---------------------------------- */

/* The handler of the loader stubs' ldr_run frame -- where every DllMain and
 * TLS callback is called from (pe_loader.c's stub function table names it,
 * through a trampoline reading the module table). What Windows 11 does with
 * an exception such a call lets out (measured: programs/win-exc38,
 * logs/w32/w7g-host-run.txt), by the call's reason and lpReserved (the
 * control word ldr_run keeps in its frame; the handler data says where, and
 * where the two landings are):
 *   DLL_PROCESS_ATTACH -- the loader takes it: LoadLibrary fails
 *     (ERROR_DLL_INIT_FAILED, and that DLL is not called with
 *     DLL_PROCESS_DETACH), or the process's start does (STATUS_DLL_INIT_FAILED):
 *     unwound to ldr_run_raised;
 *   DLL_PROCESS_DETACH from FreeLibrary (lpReserved NULL) -- taken, and on
 *     with the next call (FreeLibrary succeeds): unwound to ldr_run_next;
 *     at the process's exit (lpReserved set) -- not taken: unhandled;
 *   DLL_THREAD_ATTACH -- the process ends with the code, the top-level
 *     filter not asked;
 *   DLL_THREAD_DETACH -- not taken: unhandled. */
EXCEPTION_DISPOSITION rt_ldr_handler(EXCEPTION_RECORD *rec, ULONG64 frame, CONTEXT *ctx,
                                     DISPATCHER_CONTEXT *dc) {
    (void)ctx;
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    const DWORD *hd = (const DWORD *)dc->HandlerData;   /* raised, next (RVAs), the control word's slot */
    ULONG64 ctl = *(const ULONG64 *)(frame + hd[2]);
    unsigned reason = (unsigned)(ctl & 0xFF);
    int reserved = (int)((ctl >> 9) & 1);
    ULONG64 to;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        to = dc->ImageBase + hd[0];
        break;
    case DLL_PROCESS_DETACH:
        if (reserved) return ExceptionContinueSearch;
        to = dc->ImageBase + hd[1];
        break;
    case DLL_THREAD_ATTACH:
        rt_terminate(rec->ExceptionCode);
    default:
        return ExceptionContinueSearch;
    }
    rt_RtlUnwindEx((PVOID)frame, (PVOID)to, rec, 0, dc->ContextRecord, dc->HistoryTable);
    return ExceptionContinueSearch;
}

/* ---- __C_specific_handler ----------------------------------------------- */

typedef LONG (*RT_FILTER)(EXCEPTION_POINTERS *, ULONG64);
typedef void (*RT_TERMINATION)(BOOLEAN, ULONG64);

/* The C language handler: HandlerData is a SCOPE_TABLE_AMD64, the scopes
 * innermost first, each a [BeginAddress, EndAddress) of the function with
 * a filter (HandlerAddress; 1 = EXCEPTION_EXECUTE_HANDLER) and its __except
 * block (JumpTarget), or a __finally (HandlerAddress, JumpTarget 0).
 * Dispatch: each __except scope holding pc has its filter called with the
 * EXCEPTION_POINTERS and the establisher frame; < 0 continues execution, > 0
 * unwinds to the __except block with the exception code in rax, 0 goes on.
 * Unwind: each __finally holding pc runs (abnormal termination TRUE), up to
 * the scope that is the unwind's target; ScopeIndex records how far, so a
 * collided unwind does not run one twice. */
EXCEPTION_DISPOSITION rt_C_specific_handler(EXCEPTION_RECORD *rec, ULONG64 frame,
                                            CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
    const SCOPE_TABLE_AMD64 *st = (const SCOPE_TABLE_AMD64 *)dc->HandlerData;
    ULONG64 base = dc->ImageBase;
    DWORD pc = (DWORD)(dc->ControlPc - base);
    if (!(rec->ExceptionFlags & EXCEPTION_UNWIND)) {
        EXCEPTION_POINTERS ep = { rec, ctx };
        for (DWORD i = dc->ScopeIndex; i < st->Count; i++) {
            const __typeof__(st->ScopeRecord[0]) *s = &st->ScopeRecord[i];
            if (pc < s->BeginAddress || pc >= s->EndAddress || !s->JumpTarget) continue;
            LONG r = s->HandlerAddress == 1 ? EXCEPTION_EXECUTE_HANDLER
                   : ((RT_FILTER)(base + s->HandlerAddress))(&ep, frame);
            if (r < 0) return ExceptionContinueExecution;
            if (r > 0)
                rt_RtlUnwindEx((PVOID)frame, (PVOID)(base + s->JumpTarget), rec,
                               (PVOID)(ULONG_PTR)rec->ExceptionCode, ctx, dc->HistoryTable);
        }
        return ExceptionContinueSearch;
    }
    DWORD tip = (DWORD)(dc->TargetIp - base);
    int target = (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) != 0;
    for (DWORD i = dc->ScopeIndex; i < st->Count; i++) {
        const __typeof__(st->ScopeRecord[0]) *s = &st->ScopeRecord[i];
        if (pc < s->BeginAddress || pc >= s->EndAddress) continue;
        if (target) {
            if (s->JumpTarget && s->JumpTarget == tip) break;      /* the __except being reached */
            if (tip >= s->BeginAddress && tip < s->EndAddress) continue;   /* still inside it */
        }
        if (s->JumpTarget) continue;
        dc->ScopeIndex = i + 1;
        ((RT_TERMINATION)(base + s->HandlerAddress))(TRUE, frame);
    }
    return ExceptionContinueSearch;
}

/* ---- the top-level filter ----------------------------------------------- */

LPTOP_LEVEL_EXCEPTION_FILTER rt_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    return (LPTOP_LEVEL_EXCEPTION_FILTER)__atomic_exchange_n((void *volatile *)&g_top_filter,
                                                             (void *)f, __ATOMIC_SEQ_CST);
}

/* GetCurrentThreadStackLimits (W7d-2): the thread's stack reservation,
 * [lowest address, StackBase) -- the lowest kept in the TEB's slot 0x2F8
 * (include/tobyos/pe.h, WIN32_TEB_STACK_BOTTOM), StackBase at 0x08. */
void rt_GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high) {
    ULONG64 bottom = __readgsqword(0x2F8);
    ULONG64 top = __readgsqword(8);
    if (low) *low = (ULONG_PTR)bottom;
    if (high) *high = (ULONG_PTR)top;
}

/* SetErrorMode / GetErrorMode: the process's error mode. Of its flags only
 * SEM_NOGPFAULTERRORBOX means anything here (rt_report); the rest are kept
 * and given back. */
UINT rt_SetErrorMode(UINT mode) {
    return __atomic_exchange_n(&g_error_mode, mode, __ATOMIC_SEQ_CST);
}
UINT rt_GetErrorMode(void) { return g_error_mode; }

/* The filter SetUnhandledExceptionFilter installed decides; when there is
 * none, or it says EXCEPTION_CONTINUE_SEARCH, the default -- tobyOS's
 * Application Error box, a line on stderr -- and EXCEPTION_EXECUTE_HANDLER. */
LONG rt_UnhandledExceptionFilter(EXCEPTION_POINTERS *ep) {
    LPTOP_LEVEL_EXCEPTION_FILTER f = g_top_filter;
    if (f) {
        LONG r = f(ep);
        if (r != EXCEPTION_CONTINUE_SEARCH) return r;
    }
    if (ep && ep->ExceptionRecord) rt_report(ep->ExceptionRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}
