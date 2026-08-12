/* ptrace(2) -- the subset strace needs, and no more.
 *
 * ===========================================================================
 * WHAT THIS IS FOR
 *
 * The plan scoped this as "ATTACH/PEEK/POKE/CONT, enough for strace", and that
 * framing is the right one: the value here is not ptrace itself, it is that
 * every FUTURE gap in this arc becomes cheaper to diagnose. Most of this
 * session was spent inferring what a program wanted from the outside -- the
 * syscall ring records argument POINTERS, so a missing file reads as
 * `openat a1=29407312 = -2` and names nothing. That cost a dedicated kernel
 * instrument (-DPATHFAIL_TRACE) and several twenty-minute boots. strace would
 * have answered it in one.
 *
 * ===========================================================================
 * HOW A STOP WORKS HERE
 *
 * The tracee parks itself; the tracer never reaches in and suspends it. That
 * is both Linux's model and the only one that is safe on SMP: a tracer that
 * flipped another process's state could catch it mid-syscall with kernel locks
 * held.
 *
 *   tracee  syscall boundary -> ptrace_syscall_stop()
 *                               state = PROC_STOPPED, ptrace_stopped = 1
 *                               wake the tracer if it is in wait4
 *                               sched_yield()            <- parks here
 *   tracer  wait4 sees ptrace_stopped -> WIFSTOPPED, does NOT reap
 *           PEEK/GETREGS...
 *           PTRACE_SYSCALL -> ptrace_stopped = 0, state = READY, enqueue
 *   tracee  ...resumes on the line after sched_yield()
 *
 * PROC_STOPPED is reused rather than a new state: to the scheduler a
 * ptrace-stop and a job-control stop are identical (off every run queue, not
 * requeued on yield), and `ptrace_stopped` is what distinguishes them for
 * wait4. A second stopped state would have meant auditing every `state ==`
 * comparison in the tree for a difference the scheduler does not care about.
 *
 * ===========================================================================
 * WHAT IS DELIBERATELY NOT HERE
 *
 * SINGLESTEP (needs the TF flag threaded through the signal/return path),
 * the PTRACE_EVENT_* stops (fork/clone/exec notifications), GETSIGINFO,
 * GETREGSET, and group-stop semantics. Each is refused with EINVAL rather
 * than accepted and ignored: a tracer that asks to be told about a fork and is
 * silently not told does not get a weaker feature, it gets a wrong answer.
 * ===========================================================================
 */
#include <tobyos/proc.h>
#include <tobyos/signal.h>
#include <tobyos/sched.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/uaccess.h>

/* Linux request numbers. */
#define PT_TRACEME       0
#define PT_PEEKTEXT      1
#define PT_PEEKDATA      2
#define PT_POKETEXT      4
#define PT_POKEDATA      5
#define PT_CONT          7
#define PT_KILL          8
#define PT_SINGLESTEP    9
#define PT_GETREGS      12
#define PT_SETREGS      13
#define PT_ATTACH       16
#define PT_DETACH       17
#define PT_SYSCALL      24
#define PT_SETOPTIONS   0x4200
#define PT_GETREGSET    0x4204
#define PT_SEIZE        0x4206

#define PTRACE_O_TRACESYSGOOD 0x00000001

extern struct proc g_proc[];
struct proc *proc_lookup(int pid);
struct proc *current_proc(void);
void         sched_enqueue(struct proc *p);
void         sched_yield(void);
int          pid_knr(int vpid);
void         proc_wake_waiters(int pid);

/* x86-64 user_regs_struct, in the order Linux defines it. strace reads
 * orig_rax for the syscall number, rdi/rsi/rdx/r10/r8/r9 for the arguments,
 * and rax for the return value -- so those are the fields that must be right,
 * and the test asserts exactly them. */
struct lx_user_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags;
    uint64_t rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};

/* The tracee's saved user registers live at the top of its kernel stack, the
 * same block fork copies to give a child its parent's register state. */
static struct syscall_regs *tracee_regs(struct proc *t)
{
    if (!t || !t->kstack_top) return 0;
    return (struct syscall_regs *)((uint8_t *)t->kstack_top -
                                   sizeof(struct syscall_regs));
}

static bool may_trace(struct proc *me, struct proc *t)
{
    if (!me || !t) return false;
    if (t->pid == me->pid) return false;              /* not yourself */
    if (t->state == PROC_UNUSED || t->state == PROC_EMBRYO) return false;
    /* Same rule process_vm_readv uses (ptrace_may_access): own it, or hold
     * CAP_SYS_ADMIN over it. Without this, tracing is a way to read any
     * process's memory and rewrite its registers. */
    if (me->uid == 0) return true;
    return me->uid == t->uid;
}

/* ---- the stop ----------------------------------------------------------- */

/* There is deliberately no wake_tracer(). wait4's traced-child arm polls --
 * yield or hlt, then re-check -- so setting ptrace_stopped IS the
 * notification. An earlier version enqueued the tracer from the stop path and
 * wedged the machine; see the note in signal.c's stop site. Parking a process
 * and prodding the scheduler from the same code path is how this tree has
 * deadlocked before. */

/* Park the caller in a ptrace-stop until the tracer resumes it. */
static void ptrace_stop(struct proc *p, int sig)
{
    p->ptrace_stopsig = sig;
    p->ptrace_stopped = 1;
    p->state = PROC_STOPPED;
    /* Wake a tracer already parked in proc_wait. Uses proc.c's own wake --
     * the one proc_exit uses for a terminating child -- rather than a
     * hand-rolled enqueue, because a hand-rolled one in the signal path wedged
     * the guest outright once already. */
    proc_wake_waiters(p->pid);
    sched_yield();          /* resumes here when the tracer flips us READY */
    { static int n; if (n < 24) { n++;
        kprintf("[ptdbg] pid=%d RESUMED\n", p->pid); } }
    p->ptrace_stopped = 0;
}

/* Called at both syscall boundaries from linux_syscall. `entering` picks the
 * half; `nr`/`ret` are what GETREGS will report as orig_rax/rax. */
void ptrace_syscall_stop(struct proc *p, bool entering, long nr, long ret)
{
    if (!p || !p->tracer_pid || !p->ptrace_syscall) return;
    if (entering) {
        p->ptrace_orig_rax = nr;
        p->ptrace_retval   = 0;
        p->ptrace_atexit   = 1;
    } else {
        if (!p->ptrace_atexit) return;    /* resumed with CONT mid-syscall */
        p->ptrace_retval = ret;
        p->ptrace_atexit = 0;
    }
    /* PTRACE_O_TRACESYSGOOD sets bit 7 so the tracer can tell a syscall-stop
     * from a real SIGTRAP. strace sets it and relies on the distinction. */
    int sig = (p->ptrace_opts & PTRACE_O_TRACESYSGOOD) ? (SIGTRAP | 0x80)
                                                       : SIGTRAP;
    ptrace_stop(p, sig);
}

/* Is `t` sitting in a ptrace-stop that `me` should be told about? */
bool ptrace_stopped_for(struct proc *me, struct proc *t)
{
    return me && t && t->tracer_pid == me->pid && t->ptrace_stopped &&
           t->state == PROC_STOPPED;
}

int ptrace_stop_signal(struct proc *t) { return t ? t->ptrace_stopsig : 0; }

/* ---- peek / poke -------------------------------------------------------- */

/* One word through the target's address space. Same borrow process_vm_readv
 * uses: point the page-table walker at the tracee while the kernel stays on
 * its own PML4, translate, then go through the HHDM. */
static int peek_word(struct proc *t, uint64_t va, uint64_t *out)
{
    uint64_t saved = vmm_set_editor_root(t->cr3);
    uint64_t phys  = vmm_translate(va & ~(uint64_t)(PAGE_SIZE - 1));
    vmm_set_editor_root(saved);
    if (!phys) return -1;
    /* A word may straddle two pages; the second half needs its own
     * translation, because contiguous virtual does not mean contiguous
     * physical. Handled by reading byte-wise across the boundary. */
    uint8_t *b = (uint8_t *)out;
    for (int i = 0; i < 8; i++) {
        uint64_t a = va + (uint64_t)i;
        uint64_t s2 = vmm_set_editor_root(t->cr3);
        uint64_t ph = vmm_translate(a & ~(uint64_t)(PAGE_SIZE - 1));
        vmm_set_editor_root(s2);
        if (!ph) return -1;
        b[i] = *((uint8_t *)pmm_phys_to_virt(ph) + (a & (PAGE_SIZE - 1)));
    }
    return 0;
}

static int poke_word(struct proc *t, uint64_t va, uint64_t val)
{
    const uint8_t *b = (const uint8_t *)&val;
    for (int i = 0; i < 8; i++) {
        uint64_t a = va + (uint64_t)i;
        uint64_t s2 = vmm_set_editor_root(t->cr3);
        uint64_t ph = vmm_translate(a & ~(uint64_t)(PAGE_SIZE - 1));
        vmm_set_editor_root(s2);
        if (!ph) return -1;
        *((uint8_t *)pmm_phys_to_virt(ph) + (a & (PAGE_SIZE - 1))) = b[i];
    }
    return 0;
}

/* ---- the syscall -------------------------------------------------------- */

long ptrace_do(long req, int vpid, uint64_t addr, uint64_t data)
{
    struct proc *me = current_proc();
    if (!me) return -ABI_EINVAL;

    if (req == PT_TRACEME) {
        if (me->tracer_pid) return -ABI_EPERM;   /* already traced */
        me->tracer_pid   = me->ppid;
        me->ptrace_opts  = 0;
        me->ptrace_syscall = 0;
        kprintf("[ptrace] pid=%d TRACEME (tracer=%d)\n", me->pid, me->ppid);
        return 0;
    }

    int kpid = pid_knr(vpid);
    if (!kpid) return -ABI_ESRCH;
    struct proc *t = proc_lookup(kpid);
    if (!t) return -ABI_ESRCH;

    if (req == PT_ATTACH || req == PT_SEIZE) {
        if (!may_trace(me, t)) return -ABI_EPERM;
        if (t->tracer_pid) return -ABI_EPERM;
        t->tracer_pid = me->pid;
        t->ptrace_opts = 0;
        if (req == PT_ATTACH) {
            /* Linux stops the tracee on ATTACH. SEIZE deliberately does not,
             * which is the whole difference between them. */
            signal_send(t, SIGSTOP);
        }
        kprintf("[ptrace] pid=%d %s pid=%d\n", me->pid,
                req == PT_ATTACH ? "ATTACH" : "SEIZE", t->pid);
        return 0;
    }

    /* Everything below operates on an established tracee. */
    if (t->tracer_pid != me->pid) return -ABI_ESRCH;

    switch (req) {
    case PT_PEEKTEXT:
    case PT_PEEKDATA: {
        uint64_t w = 0;
        if (peek_word(t, addr, &w) != 0) return -ABI_EIO;
        /* glibc's ptrace() passes &result in `data` for the PEEK requests and
         * returns what the kernel stored there. Writing it out is what makes
         * the library wrapper work; a kernel that only returned the value in
         * rax would break every ordinary caller. */
        if (data && copy_to_user((void *)(uintptr_t)data, &w, sizeof w) != 0)
            return -ABI_EFAULT;
        return (long)w;
    }
    case PT_POKETEXT:
    case PT_POKEDATA:
        if (poke_word(t, addr, data) != 0) return -ABI_EIO;
        return 0;

    case PT_GETREGS: {
        if (!t->ptrace_stopped) return -ABI_ESRCH;   /* must be stopped */
        struct syscall_regs *r = tracee_regs(t);
        if (!r) return -ABI_EIO;
        struct lx_user_regs u;
        memset(&u, 0, sizeof u);
        u.r15 = r->r15; u.r14 = r->r14; u.r13 = r->r13; u.r12 = r->r12;
        u.rbp = r->rbp; u.rbx = r->rbx; u.r11 = r->r11; u.r10 = r->r10;
        u.r9  = r->r9;  u.r8  = r->r8;
        u.rdx = r->rdx; u.rsi = r->rsi; u.rdi = r->rdi;
        u.rcx = r->rcx;
        u.rip = r->rcx;              /* SYSRET returns to rcx */
        u.rsp = r->user_rsp;
        u.eflags = r->r11;           /* ...and reloads rflags from r11 */
        u.cs = 0x33; u.ss = 0x2b;
        u.fs_base = t->tls_base;
        /* The two fields strace actually prints: the syscall number at the
         * entry stop, and the return value at the exit stop. struct
         * syscall_regs has no rax slot (the dispatcher's return value goes
         * straight to userspace), which is why these are recorded separately
         * at the stop rather than read back from the frame. */
        u.orig_rax = (uint64_t)t->ptrace_orig_rax;
        u.rax      = (uint64_t)t->ptrace_retval;
        if (copy_to_user((void *)(uintptr_t)data, &u, sizeof u) != 0)
            return -ABI_EFAULT;
        return 0;
    }
    case PT_SETREGS: {
        if (!t->ptrace_stopped) return -ABI_ESRCH;
        struct syscall_regs *r = tracee_regs(t);
        if (!r) return -ABI_EIO;
        struct lx_user_regs u;
        if (copy_from_user(&u, (const void *)(uintptr_t)data, sizeof u) != 0)
            return -ABI_EFAULT;
        r->r15 = u.r15; r->r14 = u.r14; r->r13 = u.r13; r->r12 = u.r12;
        r->rbp = u.rbp; r->rbx = u.rbx; r->r10 = u.r10;
        r->r9  = u.r9;  r->r8  = u.r8;
        r->rdx = u.rdx; r->rsi = u.rsi; r->rdi = u.rdi;
        r->rcx = u.rip;              /* where SYSRET will land */
        r->r11 = u.eflags;
        r->user_rsp = u.rsp;
        return 0;
    }

    case PT_SETOPTIONS:
        /* Only the option that is actually implemented is accepted. The
         * PTRACE_O_TRACE{FORK,CLONE,EXEC,EXIT} bits request EVENT STOPS this
         * kernel does not generate -- accepting them would promise
         * notifications that never arrive, which is worse for a tracer than
         * being told no. */
        if (data & ~(uint64_t)PTRACE_O_TRACESYSGOOD) {
            kprintf("[ptrace] SETOPTIONS 0x%lx: only TRACESYSGOOD is "
                    "implemented -> EINVAL\n", (unsigned long)data);
            return -ABI_EINVAL;
        }
        t->ptrace_opts = (uint32_t)data;
        return 0;

    case PT_CONT:
    case PT_SYSCALL:
        if (!t->ptrace_stopped) return -ABI_ESRCH;
        t->ptrace_syscall = (req == PT_SYSCALL) ? 1 : 0;
        t->ptrace_stopped = 0;
        t->state = PROC_READY;
        sched_enqueue(t);
        return 0;

    case PT_DETACH:
        t->tracer_pid    = 0;
        t->ptrace_syscall = 0;
        t->ptrace_opts   = 0;
        if (t->ptrace_stopped) {
            t->ptrace_stopped = 0;
            t->state = PROC_READY;
            sched_enqueue(t);
        }
        return 0;

    case PT_KILL:
        signal_send(t, SIGKILL);
        if (t->ptrace_stopped) {
            t->ptrace_stopped = 0;
            t->state = PROC_READY;
            sched_enqueue(t);
        }
        return 0;

    case PT_SINGLESTEP:
    case PT_GETREGSET:
    default:
        /* Refused, not faked. See the header: a tracer told "yes" for a stop
         * that never arrives waits forever, which is a worse failure than an
         * honest EINVAL it can fall back from. */
        kprintf("[ptrace] request %ld is not implemented -> EINVAL\n", req);
        return -ABI_EINVAL;
    }
}

/* A tracee whose tracer disappears must not stay parked forever. Called from
 * the exit path for every process. */
void ptrace_detach_all(int tracer_pid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *t = &g_proc[i];
        if (t->state == PROC_UNUSED || t->tracer_pid != tracer_pid) continue;
        t->tracer_pid = 0;
        t->ptrace_syscall = 0;
        if (t->ptrace_stopped) {
            t->ptrace_stopped = 0;
            t->state = PROC_READY;
            sched_enqueue(t);
        }
    }
}
