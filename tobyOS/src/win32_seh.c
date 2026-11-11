/* src/win32_seh.c -- W7d (docs/roadmap-2026-11.md): a CPU fault in a Win32
 * program becomes an SEH exception, dispatched in user mode by tobyrt.dll.
 *
 * On Windows the kernel turns a fault into an EXCEPTION_RECORD and a CONTEXT
 * on the faulting thread's stack and continues at ntdll's
 * KiUserExceptionDispatcher, which dispatches it as RaiseException's are
 * (vectored handlers, the frames' handlers, the top-level filter). Here the
 * same: default_exception (src/isr.c) offers a ring-3 fault of a Win32
 * process to win32_fault_deliver before the POSIX signal path; this builds
 *
 *   S + 0x000  home space (0x20)
 *   S + 0x020  CONTEXT (0x4D0) -- every integer register, Rip, Rsp, EFlags,
 *              the segment selectors, MXCSR and the FXSAVE image (XMM0-15)
 *   S + 0x4F0  EXCEPTION_RECORD (0x98)
 *   S + 0x590  a machine frame: Rip, CS, RFLAGS, Rsp, SS of the fault
 *
 * below the faulting Rsp, and the trap returns to tobyrt.dll's
 * KiUserExceptionDispatcher (its address is in the module table,
 * WIN32_MT_KIUSER, put there when the loader maps the runtime) with Rsp = S.
 * The entry's unwind information describes exactly this frame (the machine
 * frame, the CONTEXT's nonvolatile registers), so a walk goes through it back
 * into the faulting function, as on Windows.
 *
 * The codes and parameters are Windows 11's, measured (programs/win-exc35,
 * logs/w32/w7d-host-run.txt): an access violation 0xC0000005 {0 read / 1
 * write / 8 execute, the address} -- {0, all ones} for a #GP from a
 * non-canonical address; divide by zero 0xC0000094 and divide overflow
 * 0xC0000095 told apart by the divisor; ud2 0xC000001D; a privileged
 * instruction 0xC0000096; int3 0x80000003 {0} with Rip back AT the int3; a
 * single step 0x80000004; an SSE exception 0xC000008E.. {0, MXCSR}.
 *
 * Linux and native processes never come here (the caller checks the
 * personality); a Win32 process without the runtime, or whose stack cannot
 * take the frame, falls through to the signal path as before. */
#include <tobyos/types.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/isr.h>
#include <tobyos/proc.h>
#include <tobyos/uaccess.h>
#include <tobyos/cpu.h>
#include <tobyos/pe.h>
#include <tobyos/abi/abi.h>

#define W32X_ACCESS_VIOLATION       0xC0000005u
#define W32X_INT_DIVIDE_BY_ZERO     0xC0000094u
#define W32X_INT_OVERFLOW           0xC0000095u
#define W32X_ILLEGAL_INSTRUCTION    0xC000001Du
#define W32X_PRIV_INSTRUCTION       0xC0000096u
#define W32X_BREAKPOINT             0x80000003u
#define W32X_SINGLE_STEP            0x80000004u
#define W32X_DATATYPE_MISALIGNMENT  0x80000002u
#define W32X_FLT_DENORMAL_OPERAND   0xC000008Du
#define W32X_FLT_DIVIDE_BY_ZERO     0xC000008Eu
#define W32X_FLT_INEXACT_RESULT     0xC000008Fu
#define W32X_FLT_INVALID_OPERATION  0xC0000090u
#define W32X_FLT_OVERFLOW           0xC0000091u
#define W32X_FLT_STACK_CHECK        0xC0000092u
#define W32X_FLT_UNDERFLOW          0xC0000093u
#define W32X_FLT_MULTIPLE_TRAPS     0xC00002B5u

/* The x64 CONTEXT's offsets (winnt.h) and its size. */
#define CTX_FLAGS    0x30
#define CTX_MXCSR    0x34
#define CTX_SEGCS    0x38
#define CTX_SEGDS    0x3A
#define CTX_SEGES    0x3C
#define CTX_SEGFS    0x3E
#define CTX_SEGGS    0x40
#define CTX_SEGSS    0x42
#define CTX_EFLAGS   0x44
#define CTX_RAX      0x78
#define CTX_RIP      0xF8
#define CTX_FLTSAVE  0x100
#define CTX_SIZE     0x4D0
#define REC_SIZE     0x98

#define FRAME_CTX    0x20
#define FRAME_REC    0x4F0
#define FRAME_MACH   0x590
#define FRAME_SIZE   0x5C0

/* An EXCEPTION_RECORD (x64). */
struct w32x_record {
    uint32_t code, flags;
    uint64_t chain, address;
    uint32_t nparams, pad;
    uint64_t info[15];
};
_Static_assert(sizeof(struct w32x_record) == REC_SIZE, "EXCEPTION_RECORD is 0x98 bytes");
_Static_assert(ABI_PERS_WIN32 == 2, "src/isr.c tests the Win32 personality as 2");

/* ---- reading the faulting instruction ---- */

struct w32x_insn {
    uint8_t b[16];
    int     n;                      /* bytes read */
    int     i;                      /* cursor */
    uint8_t rex;
    bool    opsize, addrsize;
};

static void w32x_fetch(struct w32x_insn *in, uint64_t rip) {
    memset(in, 0, sizeof *in);
    /* the instruction may end at a page end: read what is resident */
    for (int k = 16; k > 0; k--) {
        if (copy_from_user_nofault(in->b, (const void *)(uintptr_t)rip, (size_t)k) == 0) {
            in->n = k;
            break;
        }
    }
}

/* Skip legacy prefixes and REX; the opcode's first byte is at in->i. */
static void w32x_prefixes(struct w32x_insn *in) {
    while (in->i < in->n) {
        uint8_t c = in->b[in->i];
        if (c == 0x66) in->opsize = true;
        else if (c == 0x67) in->addrsize = true;
        else if (c == 0xF0 || c == 0xF2 || c == 0xF3 || c == 0x2E || c == 0x36 || c == 0x3E ||
                 c == 0x26 || c == 0x64 || c == 0x65) { /* lock, rep, segment */ }
        else break;
        in->i++;
    }
    if (in->i < in->n && (in->b[in->i] & 0xF0) == 0x40) in->rex = in->b[in->i++];
}

/* A #GP's instruction is privileged at CPL 3 (what Windows reports as
 * STATUS_PRIVILEGED_INSTRUCTION, rather than an access violation). */
static bool w32x_privileged(const struct w32x_insn *in0) {
    struct w32x_insn in = *in0;
    w32x_prefixes(&in);
    if (in.i >= in.n) return false;
    uint8_t op = in.b[in.i];
    switch (op) {
    case 0xF4:                                  /* hlt */
    case 0xFA: case 0xFB:                       /* cli, sti */
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: /* in/out imm8 */
    case 0xEC: case 0xED: case 0xEE: case 0xEF: /* in/out dx */
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: /* ins, outs */
        return true;
    case 0x0F:
        if (in.i + 1 >= in.n) return false;
        switch (in.b[in.i + 1]) {
        case 0x06: case 0x08: case 0x09:        /* clts, invd, wbinvd */
        case 0x20: case 0x21: case 0x22: case 0x23: /* mov cr/dr */
        case 0x30: case 0x32: case 0x33:        /* wrmsr, rdmsr, rdpmc */
        case 0x07: case 0x35:                   /* sysret, sysexit */
            return true;
        case 0x00: {                            /* lldt, ltr */
            if (in.i + 2 >= in.n) return false;
            int reg = (in.b[in.i + 2] >> 3) & 7;
            return reg == 2 || reg == 3;
        }
        case 0x01: {                            /* lgdt, lidt, lmsw, invlpg, swapgs */
            if (in.i + 2 >= in.n) return false;
            uint8_t m = in.b[in.i + 2];
            int reg = (m >> 3) & 7;
            if (m == 0xF8) return true;         /* swapgs */
            if ((m >> 6) == 3) return false;    /* the register forms are other instructions */
            return reg == 2 || reg == 3 || reg == 6 || reg == 7;
        }
        default:
            return false;
        }
    default:
        return false;
    }
}

/* A general register of the trap frame by its number (rax 0 .. r15 15). */
static uint64_t w32x_reg(const struct regs *r, int n) {
    switch (n) {
    case 0: return r->rax;  case 1: return r->rcx;  case 2: return r->rdx;  case 3: return r->rbx;
    case 4: return r->rsp;  case 5: return r->rbp;  case 6: return r->rsi;  case 7: return r->rdi;
    case 8: return r->r8;   case 9: return r->r9;   case 10: return r->r10; case 11: return r->r11;
    case 12: return r->r12; case 13: return r->r13; case 14: return r->r14; default: return r->r15;
    }
}

/* A #DE's divisor (div/idiv: F6 /6 /7, F7 /6 /7): 0 means divide by zero,
 * anything else the quotient overflowed. *ok false when it cannot be read. */
static uint64_t w32x_divisor(const struct regs *r, const struct w32x_insn *in0, bool *ok) {
    struct w32x_insn in = *in0;
    *ok = false;
    w32x_prefixes(&in);
    if (in.i + 1 >= in.n) return 0;
    uint8_t op = in.b[in.i];
    if (op != 0xF6 && op != 0xF7) return 0;
    int size = op == 0xF6 ? 1 : (in.rex & 8) ? 8 : in.opsize ? 2 : 4;
    uint8_t m = in.b[in.i + 1];
    int mod = m >> 6, rm = m & 7;
    int k = in.i + 2;
    uint64_t v;
    if (mod == 3) {
        int reg = rm | ((in.rex & 1) << 3);
        if (size == 1 && !in.rex && rm >= 4) v = w32x_reg(r, rm - 4) >> 8;   /* ah ch dh bh */
        else v = w32x_reg(r, reg);
    } else {
        uint64_t ea = 0;
        if (rm == 4) {                                 /* SIB */
            if (k >= in.n) return 0;
            uint8_t sib = in.b[k++];
            int base = (sib & 7) | ((in.rex & 1) << 3);
            int index = ((sib >> 3) & 7) | ((in.rex & 2) << 2);
            int scale = sib >> 6;
            if (index != 4) ea += w32x_reg(r, index) << scale;
            if ((sib & 7) == 5 && mod == 0) {
                if (k + 4 > in.n) return 0;
                int32_t d;
                memcpy(&d, in.b + k, 4);
                k += 4;
                ea += (uint64_t)(int64_t)d;
            } else {
                ea += w32x_reg(r, base);
            }
        } else if (rm == 5 && mod == 0) {              /* rip + disp32 */
            if (k + 4 > in.n) return 0;
            int32_t d;
            memcpy(&d, in.b + k, 4);
            k += 4;
            ea = r->rip + (uint64_t)k + (uint64_t)(int64_t)d;
        } else {
            ea = w32x_reg(r, rm | ((in.rex & 1) << 3));
        }
        if (mod == 1) {
            if (k >= in.n) return 0;
            ea += (uint64_t)(int64_t)(int8_t)in.b[k];
        } else if (mod == 2) {
            if (k + 4 > in.n) return 0;
            int32_t d;
            memcpy(&d, in.b + k, 4);
            ea += (uint64_t)(int64_t)d;
        }
        if (in.addrsize) ea &= 0xFFFFFFFFull;
        v = 0;
        if (copy_from_user_nofault(&v, (const void *)(uintptr_t)ea, (size_t)size) != 0) return 0;
    }
    if (size < 8) v &= (1ull << (size * 8)) - 1;
    *ok = true;
    return v;
}

/* An SSE exception's code from MXCSR: the raised flags its mask bits let
 * through -- one of them, or STATUS_FLOAT_MULTIPLE_TRAPS. */
static uint32_t w32x_simd_code(uint32_t mxcsr) {
    uint32_t live = mxcsr & ~(mxcsr >> 7) & 0x3F;
    static const struct { uint32_t bit, code; } t[] = {
        { 0x01, W32X_FLT_INVALID_OPERATION }, { 0x02, W32X_FLT_DENORMAL_OPERAND },
        { 0x04, W32X_FLT_DIVIDE_BY_ZERO },    { 0x08, W32X_FLT_OVERFLOW },
        { 0x10, W32X_FLT_UNDERFLOW },         { 0x20, W32X_FLT_INEXACT_RESULT },
    };
    uint32_t code = 0;
    int n = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (live & t[i].bit) { code = t[i].code; n++; }
    return n == 1 ? code : n ? W32X_FLT_MULTIPLE_TRAPS : W32X_FLT_INVALID_OPERATION;
}

/* An x87 exception's code from the status word (the stack fault first). */
static uint32_t w32x_x87_code(uint16_t fsw, uint16_t fcw) {
    uint16_t live = fsw & ~fcw & 0x3F;
    if ((fsw & 0x40) && (live & 0x01)) return W32X_FLT_STACK_CHECK;
    if (live & 0x01) return W32X_FLT_INVALID_OPERATION;
    if (live & 0x04) return W32X_FLT_DIVIDE_BY_ZERO;
    if (live & 0x08) return W32X_FLT_OVERFLOW;
    if (live & 0x10) return W32X_FLT_UNDERFLOW;
    if (live & 0x02) return W32X_FLT_DENORMAL_OPERAND;
    return W32X_FLT_INEXACT_RESULT;
}

/* ---- the delivery ---- */

bool win32_fault_deliver(struct regs *r) {
    struct proc *p = current_proc();
    if (!p || p->personality != ABI_PERS_WIN32) return false;
    uint64_t kiuser = 0;
    if (copy_from_user_nofault(&kiuser, (const void *)(uintptr_t)(WIN32_MODTAB_BASE + WIN32_MT_KIUSER), 8) != 0 ||
        !kiuser)
        return false;

    uint8_t fx[512] __attribute__((aligned(16)));
    memset(fx, 0, sizeof fx);
    fpu_save(fx);
    uint32_t mxcsr;
    memcpy(&mxcsr, fx + 24, 4);

    struct w32x_record rec;
    memset(&rec, 0, sizeof rec);
    uint64_t rip = r->rip, rflags = r->rflags;
    struct w32x_insn in;
    switch (r->vector) {
    case 0: {                                           /* #DE */
        w32x_fetch(&in, rip);
        bool ok;
        uint64_t d = w32x_divisor(r, &in, &ok);
        rec.code = (ok && d != 0) ? W32X_INT_OVERFLOW : W32X_INT_DIVIDE_BY_ZERO;
        break;
    }
    case 1:                                             /* #DB: a single step */
        rec.code = W32X_SINGLE_STEP;
        rflags &= ~0x100ull;
        break;
    case 3:                                             /* #BP: Rip back at the int3 */
        rip -= 1;
        rec.code = W32X_BREAKPOINT;
        rec.nparams = 1;
        break;
    case 6:                                             /* #UD */
        rec.code = W32X_ILLEGAL_INSTRUCTION;
        break;
    case 13:                                            /* #GP */
        w32x_fetch(&in, rip);
        if (w32x_privileged(&in)) {
            rec.code = W32X_PRIV_INSTRUCTION;
        } else {
            rec.code = W32X_ACCESS_VIOLATION;
            rec.nparams = 2;
            rec.info[0] = 0;
            rec.info[1] = ~0ull;
        }
        break;
    case 14: {                                          /* #PF */
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        rec.code = W32X_ACCESS_VIOLATION;
        rec.nparams = 2;
        rec.info[0] = (r->error_code & 0x10) ? 8 : (r->error_code & 0x02) ? 1 : 0;
        rec.info[1] = cr2;
        break;
    }
    case 16: {                                          /* #MF: x87 */
        uint16_t fcw, fsw;
        memcpy(&fcw, fx + 0, 2);
        memcpy(&fsw, fx + 2, 2);
        rec.code = w32x_x87_code(fsw, fcw);
        rec.nparams = 1;                                /* {0}, as Windows 11 gives */
        break;
    }
    case 17:                                            /* #AC */
        rec.code = W32X_DATATYPE_MISALIGNMENT;
        break;
    case 19:                                            /* #XM: SSE */
        rec.code = w32x_simd_code(mxcsr);
        rec.nparams = 2;
        rec.info[0] = 0;
        rec.info[1] = mxcsr;
        break;
    default:
        return false;
    }
    rec.address = rip;

    /* the CONTEXT (the kernel stacks are 128 KiB: two KiB of frame is fine) */
    uint8_t cbuf[CTX_SIZE] __attribute__((aligned(16)));
    memset(cbuf, 0, sizeof cbuf);
    uint32_t cflags = 0x10001Fu;                        /* AMD64 | CONTROL INTEGER SEGMENTS FLOATING_POINT */
    memcpy(cbuf + CTX_FLAGS, &cflags, 4);
    memcpy(cbuf + CTX_MXCSR, &mxcsr, 4);
    uint16_t cs = (uint16_t)r->cs, ss = (uint16_t)r->ss;
    memcpy(cbuf + CTX_SEGCS, &cs, 2);
    memcpy(cbuf + CTX_SEGDS, &ss, 2);
    memcpy(cbuf + CTX_SEGES, &ss, 2);
    memcpy(cbuf + CTX_SEGSS, &ss, 2);
    uint32_t ef = (uint32_t)rflags;
    memcpy(cbuf + CTX_EFLAGS, &ef, 4);
    uint64_t g[16] = { r->rax, r->rcx, r->rdx, r->rbx, r->rsp, r->rbp, r->rsi, r->rdi,
                       r->r8, r->r9, r->r10, r->r11, r->r12, r->r13, r->r14, r->r15 };
    memcpy(cbuf + CTX_RAX, g, sizeof g);
    memcpy(cbuf + CTX_RIP, &rip, 8);
    memcpy(cbuf + CTX_FLTSAVE, fx, sizeof fx);

    uint64_t mach[5] = { rip, r->cs, rflags, r->rsp, r->ss };

    uint64_t s = (r->rsp - FRAME_SIZE) & ~0xFull;
    if (copy_to_user((void *)(uintptr_t)(s + FRAME_CTX), cbuf, CTX_SIZE) != 0 ||
        copy_to_user((void *)(uintptr_t)(s + FRAME_REC), &rec, REC_SIZE) != 0 ||
        copy_to_user((void *)(uintptr_t)(s + FRAME_MACH), mach, sizeof mach) != 0) {
        kprintf("[win32] fault 0x%x at %p: no room on the stack (rsp %p) for the dispatch\n",
                rec.code, (void *)rip, (void *)r->rsp);
        return false;
    }
    r->rip = kiuser;
    r->rsp = s;
    r->rflags &= ~(0x100ull | 0x400ull | 0x40000ull);  /* TF DF AC */
    return true;
}
