/* seccomp.c -- seccomp-bpf: a classic-BPF verifier + interpreter (slice 13).
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS SMALL, AND WHY THAT IS THE POINT
 *
 * seccomp deliberately uses CLASSIC BPF, not eBPF. Classic BPF has fixed 8-byte
 * instructions, exactly two registers (A and X), 16 scratch words, NO backward
 * jumps and NO loops. So:
 *
 *   - every program TERMINATES by construction (jumps only go forward, and the
 *     interpreter is additionally bounded by the instruction count);
 *   - the verifier is a single forward pass -- there is no control-flow graph to
 *     analyse, because the CFG is a DAG by definition;
 *   - the interpreter cannot touch anything but its own A/X/scratch and the
 *     read-only seccomp_data buffer.
 *
 * That is the whole security argument for running attacker-supplied filter
 * programs in ring 0, and it is why this file is ~200 lines rather than a JIT.
 * ---------------------------------------------------------------------------
 *
 * WHAT IS DELIBERATELY NOT HERE
 *
 * SECCOMP_RET_TRACE and SECCOMP_RET_USER_NOTIF both mean "hand this syscall to
 * a supervisor". There is no ptrace and no notifier fd in this kernel, so both
 * return -ENOSYS -- WHICH IS EXACTLY WHAT LINUX DOES when no tracer/notifier is
 * attached. Reporting them as unavailable via SECCOMP_GET_ACTION_AVAIL is the
 * honest half of that; silently treating them as ALLOW would hand a caller a
 * filter that fails open, which is the worst possible failure mode for a sandbox.
 */

#include <tobyos/seccomp.h>
#include <tobyos/proc.h>
#include <tobyos/signal.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/uaccess.h>
#include <tobyos/abi/abi.h>

/* ---- classic BPF opcode fields (uapi/linux/bpf_common.h) ---- */
#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ST    0x02
#define BPF_STX   0x03
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_RET   0x06
#define BPF_MISC  0x07

#define BPF_SIZE(c) ((c) & 0x18)
#define BPF_W     0x00
#define BPF_H     0x08
#define BPF_B     0x10

#define BPF_MODE(c) ((c) & 0xe0)
#define BPF_IMM   0x00
#define BPF_ABS   0x20
#define BPF_IND   0x40
#define BPF_MEM   0x60
#define BPF_LEN   0x80
#define BPF_MSH   0xa0

#define BPF_OP(c) ((c) & 0xf0)
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xa0

#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40

#define BPF_SRC(c) ((c) & 0x08)
#define BPF_K     0x00
#define BPF_X     0x08

#define BPF_TAX   0x00
#define BPF_TXA   0x80

#define BPF_MEMWORDS 16

/* A refcounted filter. Filters CHAIN: `prev` is the filter that was installed
 * before this one, and all of them run on every syscall. */
struct seccomp_filter {
    int                     refs;
    uint16_t                len;
    struct seccomp_filter  *prev;
    struct sock_filter      insns[];   /* len entries */
};

/* struct sock_fprog as userspace passes it (x86-64: u16 + 6 pad + ptr). */
struct sock_fprog_user {
    uint16_t len;
    uint16_t _pad[3];
    uint64_t filter;
};

/* ===================================================================
 * verifier -- one forward pass, no CFG needed
 * =================================================================== */

static bool seccomp_verify(const struct sock_filter *f, uint16_t len) {
    if (len == 0 || len > BPF_MAXINSNS) return false;
    for (uint16_t pc = 0; pc < len; pc++) {
        uint16_t code = f[pc].code;
        switch (BPF_CLASS(code)) {
        case BPF_LD:
        case BPF_LDX:
            switch (BPF_MODE(code)) {
            case BPF_IMM: case BPF_LEN: break;
            case BPF_ABS:
                /* Only the seccomp_data buffer is addressable, word-aligned.
                 * This is the check that keeps a filter from reading kernel
                 * memory: there IS no packet here, just the 64-byte struct. */
                if (BPF_SIZE(code) != BPF_W) return false;
                if (f[pc].k >= sizeof(struct seccomp_data)) return false;
                if (f[pc].k & 3) return false;
                break;
            case BPF_MEM:
                if (f[pc].k >= BPF_MEMWORDS) return false;
                break;
            default:
                /* BPF_IND and BPF_MSH are packet-relative addressing modes with
                 * no meaning here, and Linux rejects them for seccomp too. */
                return false;
            }
            break;
        case BPF_ST:
        case BPF_STX:
            if (f[pc].k >= BPF_MEMWORDS) return false;
            break;
        case BPF_ALU:
            if (BPF_OP(code) == BPF_NEG) break;
            if (BPF_OP(code) > BPF_XOR) return false;
            /* Division and modulo by a literal zero are rejected at load time
             * rather than trapped at run time -- there is no sane runtime answer
             * and a #DE in ring 0 would be a kernel bug, not a filter bug. */
            if ((BPF_OP(code) == BPF_DIV || BPF_OP(code) == BPF_MOD) &&
                BPF_SRC(code) == BPF_K && f[pc].k == 0) return false;
            break;
        case BPF_JMP: {
            if (BPF_OP(code) > BPF_JSET) return false;
            /* FORWARD JUMPS ONLY, and they must land inside the program. This
             * single check is what makes every filter terminate. */
            uint32_t t = (BPF_OP(code) == BPF_JA) ? f[pc].k : f[pc].jt;
            uint32_t fa = (BPF_OP(code) == BPF_JA) ? 0u : f[pc].jf;
            if ((uint64_t)pc + 1 + t >= len) return false;
            if ((uint64_t)pc + 1 + fa >= len) return false;
            break;
        }
        case BPF_RET:
            if (BPF_MODE(code) != 0 && BPF_SRC(code) != BPF_K &&
                BPF_SRC(code) != BPF_X) return false;
            break;
        case BPF_MISC:
            if ((code & 0xf8) != BPF_TAX && (code & 0xf8) != BPF_TXA)
                return false;
            break;
        default:
            return false;
        }
    }
    /* Linux additionally requires the last instruction to be a RET, because
     * falling off the end has no defined value. */
    if (BPF_CLASS(f[len - 1].code) != BPF_RET) return false;
    return true;
}

/* ===================================================================
 * interpreter
 * =================================================================== */

static uint32_t seccomp_run(const struct seccomp_filter *prog,
                            const struct seccomp_data *d) {
    uint32_t A = 0, X = 0, mem[BPF_MEMWORDS];
    memset(mem, 0, sizeof mem);
    const struct sock_filter *f = prog->insns;
    const uint8_t *buf = (const uint8_t *)d;

    for (uint32_t pc = 0; pc < prog->len; pc++) {
        uint16_t code = f[pc].code;
        uint32_t k = f[pc].k;
        switch (BPF_CLASS(code)) {
        case BPF_LD:
            if (BPF_MODE(code) == BPF_IMM)      A = k;
            else if (BPF_MODE(code) == BPF_LEN) A = (uint32_t)sizeof(*d);
            else if (BPF_MODE(code) == BPF_MEM) A = mem[k];
            else /* BPF_ABS, verified in range + aligned */
                memcpy(&A, buf + k, 4);
            break;
        case BPF_LDX:
            if (BPF_MODE(code) == BPF_IMM)      X = k;
            else if (BPF_MODE(code) == BPF_LEN) X = (uint32_t)sizeof(*d);
            else if (BPF_MODE(code) == BPF_MEM) X = mem[k];
            else memcpy(&X, buf + k, 4);
            break;
        case BPF_ST:  mem[k] = A; break;
        case BPF_STX: mem[k] = X; break;
        case BPF_ALU: {
            uint32_t v = (BPF_SRC(code) == BPF_X) ? X : k;
            switch (BPF_OP(code)) {
            case BPF_ADD: A += v; break;
            case BPF_SUB: A -= v; break;
            case BPF_MUL: A *= v; break;
            case BPF_DIV: if (!v) return SECCOMP_RET_KILL_PROCESS; A /= v; break;
            case BPF_MOD: if (!v) return SECCOMP_RET_KILL_PROCESS; A %= v; break;
            case BPF_OR:  A |= v; break;
            case BPF_AND: A &= v; break;
            /* Shifts >= 32 are undefined in C, so clamp rather than emit a
             * shift the compiler is free to turn into anything. */
            case BPF_LSH: A = (v >= 32) ? 0 : (A << v); break;
            case BPF_RSH: A = (v >= 32) ? 0 : (A >> v); break;
            case BPF_NEG: A = (uint32_t)(-(int32_t)A); break;
            case BPF_XOR: A ^= v; break;
            default: return SECCOMP_RET_KILL_PROCESS;
            }
            break;
        }
        case BPF_JMP: {
            if (BPF_OP(code) == BPF_JA) { pc += k; break; }
            uint32_t v = (BPF_SRC(code) == BPF_X) ? X : k;
            bool t;
            switch (BPF_OP(code)) {
            case BPF_JEQ:  t = (A == v); break;
            case BPF_JGT:  t = (A >  v); break;
            case BPF_JGE:  t = (A >= v); break;
            case BPF_JSET: t = (A &  v) != 0; break;
            default: return SECCOMP_RET_KILL_PROCESS;
            }
            pc += t ? f[pc].jt : f[pc].jf;
            break;
        }
        case BPF_RET:
            return (BPF_SRC(code) == BPF_X) ? X : k;
        case BPF_MISC:
            if ((code & 0xf8) == BPF_TAX) X = A; else A = X;
            break;
        default:
            return SECCOMP_RET_KILL_PROCESS;
        }
    }
    /* Unreachable: the verifier requires a trailing RET. Fail CLOSED anyway. */
    return SECCOMP_RET_KILL_PROCESS;
}

/* ===================================================================
 * filter lifecycle
 * =================================================================== */

static void filter_put(struct seccomp_filter *fl) {
    while (fl) {
        if (--fl->refs > 0) return;
        struct seccomp_filter *p = fl->prev;
        kfree(fl);
        fl = p;
    }
}

void seccomp_fork_inherit(struct proc *child) {
    if (!child) return;
    struct seccomp_filter *fl = (struct seccomp_filter *)child->seccomp_filter;
    if (fl) fl->refs++;          /* the chain below is reached through ->prev */
}

void seccomp_release(struct proc *p) {
    if (!p) return;
    struct seccomp_filter *fl = (struct seccomp_filter *)p->seccomp_filter;
    p->seccomp_filter = 0;
    p->seccomp_mode = SECCOMP_MODE_DISABLED;
    filter_put(fl);
}

long seccomp_set_mode_filter(uint64_t uprog, uint32_t flags) {
    struct proc *p = current_proc();
    if (!p) return -ABI_EINVAL;
    /* Unknown flags must be refused, not ignored: a caller passing a flag we do
     * not implement is asking for a guarantee we would not be providing. */
    if (flags & ~(uint32_t)(SECCOMP_FILTER_FLAG_TSYNC |
                            SECCOMP_FILTER_FLAG_LOG   |
                            SECCOMP_FILTER_FLAG_SPEC_ALLOW))
        return -ABI_EINVAL;

    /* Linux: installing a filter needs either CAP_SYS_ADMIN or no_new_privs
     * set. The rule exists because a filter can make a setuid binary behave
     * unexpectedly (by failing syscalls it does not expect to fail), so an
     * unprivileged installer must first promise not to gain privilege on exec. */
    if (!p->no_new_privs && p->uid != 0) return -ABI_EACCES;

    struct sock_fprog_user fp;
    if (copy_from_user(&fp, (const void *)(uintptr_t)uprog, sizeof fp) != 0)
        return -ABI_EFAULT;
    if (fp.len == 0 || fp.len > BPF_MAXINSNS) return -ABI_EINVAL;

    size_t bytes = (size_t)fp.len * sizeof(struct sock_filter);
    struct seccomp_filter *fl = kmalloc(sizeof(*fl) + bytes);
    if (!fl) return -ABI_ENOMEM;
    if (copy_from_user(fl->insns, (const void *)(uintptr_t)fp.filter, bytes) != 0) {
        kfree(fl);
        return -ABI_EFAULT;
    }
    if (!seccomp_verify(fl->insns, fp.len)) {
        kprintf("[seccomp] pid=%d rejected a %u-insn filter (verifier)\n",
                p->pid, fp.len);
        kfree(fl);
        return -ABI_EINVAL;
    }
    fl->refs = 1;
    fl->len  = fp.len;
    fl->prev = (struct seccomp_filter *)p->seccomp_filter;  /* CHAIN, not replace */
    p->seccomp_filter = fl;
    p->seccomp_mode   = SECCOMP_MODE_FILTER;
    kprintf("[seccomp] pid=%d installed a %u-insn filter (mode=FILTER)\n",
            p->pid, fp.len);
    return 0;
}

long seccomp_set_mode_strict(void) {
    struct proc *p = current_proc();
    if (!p) return -ABI_EINVAL;
    p->seccomp_mode = SECCOMP_MODE_STRICT;
    kprintf("[seccomp] pid=%d entered STRICT mode "
            "(read/write/_exit/sigreturn only)\n", p->pid);
    return 0;
}

long seccomp_get_action_avail(uint64_t uaction) {
    uint32_t act = 0;
    if (copy_from_user(&act, (const void *)(uintptr_t)uaction, sizeof act) != 0)
        return -ABI_EFAULT;
    switch (act & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_KILL_PROCESS:
    case SECCOMP_RET_KILL_THREAD:
    case SECCOMP_RET_TRAP:
    case SECCOMP_RET_ERRNO:
    case SECCOMP_RET_LOG:
    case SECCOMP_RET_ALLOW:
        return 0;
    default:
        /* TRACE and USER_NOTIF need a supervisor this kernel does not have.
         * Answering "not available" is the honest answer and lets a caller
         * choose a different action rather than get a filter that fails open.
         * Linux uses EOPNOTSUPP here; this ABI has no such constant, and
         * EOPNOTSUPP == ENOTSUP == 95 on Linux, so the literal is used with the
         * name recorded rather than inventing a new ABI_* define for one site. */
        return -95;   /* EOPNOTSUPP */
    }
}

long seccomp_prctl_set(long which, uint64_t arg) {
    struct proc *p = current_proc();
    if (!p) return -ABI_EINVAL;
    if (which == SECCOMP_MODE_STRICT) return seccomp_set_mode_strict();
    if (which == SECCOMP_MODE_FILTER) return seccomp_set_mode_filter(arg, 0);
    return -ABI_EINVAL;
}

/* ===================================================================
 * THE HOOK
 * =================================================================== */

/* STRICT mode allows exactly these four (Linux: read, write, _exit,
 * rt_sigreturn) and kills on anything else. */
static bool strict_allowed(long nr) {
    return nr == 0 /*read*/ || nr == 1 /*write*/ ||
           nr == 60 /*exit*/ || nr == 15 /*rt_sigreturn*/ ||
           nr == 231 /*exit_group -- glibc's _exit uses it*/;
}

long seccomp_check(struct proc *p, long nr, long a1, long a2, long a3,
                   long a4, long a5, long a6) {
    if (!p || p->seccomp_mode == SECCOMP_MODE_DISABLED) return 0;

    if (p->seccomp_mode == SECCOMP_MODE_STRICT) {
        if (strict_allowed(nr)) return 0;
        kprintf("[seccomp] pid=%d STRICT killed on syscall %ld\n", p->pid, nr);
        proc_exit(128 + 31 /* SIGSYS */);
        return -ABI_EPERM;                     /* not reached */
    }

    struct seccomp_data d;
    memset(&d, 0, sizeof d);
    d.nr      = (int32_t)nr;
    d.arch    = AUDIT_ARCH_X86_64;
    d.args[0] = (uint64_t)a1; d.args[1] = (uint64_t)a2;
    d.args[2] = (uint64_t)a3; d.args[3] = (uint64_t)a4;
    d.args[4] = (uint64_t)a5; d.args[5] = (uint64_t)a6;

    /* EVERY filter in the chain runs and the MOST RESTRICTIVE result wins, so
     * adding a filter can only tighten a policy, never loosen it.
     *
     * THE COMPARISON MUST BE SIGNED. The action constants are ordered so that
     * "least permissive" sorts lowest:
     *
     *   KILL_PROCESS 0x80000000  KILL_THREAD 0x00000000  TRAP 0x00030000
     *   ERRNO 0x00050000  USER_NOTIF 0x7fc00000  TRACE 0x7ff00000
     *   LOG 0x7ffc0000  ALLOW 0x7fff0000
     *
     * but that ordering only holds when they are read as INT32:
     * KILL_PROCESS is 0x80000000, which as unsigned is 2147483648 -- LARGER
     * than ALLOW's 0x7fff0000 -- so an unsigned min() silently selects ALLOW and
     * SECCOMP_RET_KILL_PROCESS can never win. As signed it is INT32_MIN and
     * sorts first, which is what makes the constant set a total order.
     *
     * I got this wrong first time and the acceptance test's bit6 caught it: the
     * filtered child ran to completion and exited normally instead of dying.
     * A sandbox whose KILL action silently degrades to ALLOW is the exact
     * fail-open outcome this slice must not have. */
    int32_t ret = (int32_t)SECCOMP_RET_ALLOW;
    for (struct seccomp_filter *fl = (struct seccomp_filter *)p->seccomp_filter;
         fl; fl = fl->prev) {
        int32_t r = (int32_t)seccomp_run(fl, &d);
        if ((r & (int32_t)SECCOMP_RET_ACTION_FULL) <
            (ret & (int32_t)SECCOMP_RET_ACTION_FULL)) ret = r;
    }

    switch ((uint32_t)ret & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_ALLOW:
        return 0;
    case SECCOMP_RET_LOG:
        kprintf("[seccomp] pid=%d LOG syscall %ld (allowed)\n", p->pid, nr);
        return 0;
    case SECCOMP_RET_ERRNO: {
        long e = (long)(ret & SECCOMP_RET_DATA);
        if (e > 4095) e = 4095;                /* Linux clamps to MAX_ERRNO */
        return -e;
    }
    case SECCOMP_RET_TRAP:
        /* SIGSYS with si_syscall == nr. We deliver the signal; a handler that
         * returns resumes WITHOUT running the syscall, which is the contract. */
        signal_send(p, 31 /* SIGSYS */);
        return -ABI_EPERM;
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_USER_NOTIF:
        /* No tracer and no notifier exists, and Linux's documented behaviour in
         * exactly that case is ENOSYS. Reported unavailable by
         * SECCOMP_GET_ACTION_AVAIL so a caller can pick something else. */
        return -ABI_ENOSYS;
    case SECCOMP_RET_KILL_THREAD:
    case SECCOMP_RET_KILL_PROCESS:
    default:
        kprintf("[seccomp] pid=%d KILLED by filter on syscall %ld "
                "(action 0x%x)\n", p->pid, nr,
                (uint32_t)ret & SECCOMP_RET_ACTION_FULL);
        proc_exit(128 + 31);
        return -ABI_EPERM;                     /* not reached */
    }
}
