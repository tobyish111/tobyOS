/* signal.c -- Full POSIX signal subsystem (Phase 1 M1.3).
 *
 * Extends the minimal milestone 8 signal support with:
 *   - User-installable signal handlers via sigaction
 *   - Signal masks (sigprocmask)
 *   - Proper signal delivery via trampoline to user-space
 *   - SIGCHLD delivery on child exit
 *   - SIGKILL/SIGSTOP cannot be caught or blocked
 */

#include <tobyos/signal.h>
#include <tobyos/shell.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/printk.h>
#include <tobyos/uaccess.h>
#include <tobyos/klibc.h>
#include <tobyos/isr.h>
#include <tobyos/nsproxy.h>   /* pid_knr / pid_vnr_in / pid_ns_can_see (slice 10) */
#include <tobyos/abi/abi.h>

static int g_foreground_pid = 0;

/* ---- Linux x86-64 SA_SIGINFO ABI (B15) ----
 *
 * A Linux binary's 3-arg handler `void h(int, siginfo_t*, void*)` reads its
 * siginfo_t / ucontext_t at the EXACT byte offsets the Linux kernel uses, which
 * differ from tobyOS's native layout (signal.h). So when delivering to an
 * ABI_PERS_LINUX process we synthesize the Linux-layout structures by hand.
 * Field offsets below are from the x86-64 Linux uapi (asm/sigcontext.h,
 * bits/types/siginfo_t.h) and are ABI -- they must match byte-for-byte. */
#define LX_SIGINFO_SIZE   128   /* sizeof(siginfo_t) on x86-64 Linux         */
#define LX_UCONTEXT_SIZE  968   /* sizeof(ucontext_t) on x86-64 Linux        */

/* siginfo_t field offsets (common header + the relevant union members). */
#define LXSI_SIGNO   0          /* int  si_signo                             */
#define LXSI_ERRNO   4          /* int  si_errno                             */
#define LXSI_CODE    8          /* int  si_code                              */
#define LXSI_PID    16          /* _kill: pid_t si_pid (kill/raise origin)   */
#define LXSI_UID    20          /* _kill: uid_t si_uid                       */
#define LXSI_ADDR   16          /* _sigfault: void *si_addr (fault address)  */

/* ucontext_t: uc_mcontext.gregs[] starts at offset 40 (after uc_flags,
 * uc_link, and the 24-byte uc_stack). gregs is greg_t[23] in this fixed
 * order; uc_sigmask follows the 256-byte mcontext_t at offset 296. */
#define LXUC_GREGS   40
#define LXUC_SIGMASK 296
enum {
    LXREG_R8=0, LXREG_R9, LXREG_R10, LXREG_R11, LXREG_R12, LXREG_R13,
    LXREG_R14, LXREG_R15, LXREG_RDI, LXREG_RSI, LXREG_RBP, LXREG_RBX,
    LXREG_RDX, LXREG_RAX, LXREG_RCX, LXREG_RSP, LXREG_RIP, LXREG_EFL,
    LXREG_CSGSFS, LXREG_ERR, LXREG_TRAPNO, LXREG_OLDMASK, LXREG_CR2
};
#define LX_USER_CS 0x33         /* ring-3 %cs packed into gregs[CSGSFS]      */

/* Marker stamped into the on-user-stack signal frame; sys_sigreturn refuses
 * to restore a frame without it (a corrupt/forged frame -> SIGSEGV). */
#define SIG_FRAME_MAGIC  0x5347465254423031ULL  /* "SGFRTB01" */

/* struct syscall_regs (the saved syscall trapframe layout) lives in
 * <tobyos/signal.h> now -- fork.c shares it for the child resume frame. */

/* Saved user context pushed onto the user stack for a caught signal and read
 * back by sys_sigreturn. It sits ABOVE the 8-byte handler return address so
 * the handler's own stack growth (downward) never clobbers it. rcx/r11 are
 * intentionally absent: the `syscall` instruction clobbers them by ABI, so
 * the interrupted user code already treats them as dead. */
struct sig_context {
    uint64_t rax;        /* syscall return value at the interruption point */
    uint64_t rdi, rsi, rdx, r10, r8, r9;
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rip;        /* resume RIP    */
    uint64_t rsp;        /* resume RSP    */
    uint64_t rflags;     /* resume RFLAGS */
    uint64_t saved_mask; /* signal mask to restore */
    uint64_t magic;
};

/* Locate the current process's saved syscall register block. Only valid on
 * the SYSCALL return path (where the block was just pushed by the asm
 * trampoline); returns NULL for pid 0 / kernel threads. */
static struct syscall_regs *current_syscall_regs(void) {
    struct proc *p = current_proc();
    if (!p || p->pid == 0 || !p->kstack_top) return 0;
    return (struct syscall_regs *)((uint8_t *)p->kstack_top
                                   - sizeof(struct syscall_regs));
}

void signal_init(void) {
    g_foreground_pid = 0;
    kprintf("[signal] POSIX signal subsystem ready (%d signals)\n", SIG_MAX);
}

void signal_init_proc(struct signal_state *ss) {
    for (int i = 0; i < SIG_MAX; i++) {
        ss->actions[i].sa_handler = SIG_DFL;
        ss->actions[i].sa_mask    = 0;
        ss->actions[i].sa_flags   = 0;
        ss->si_pid[i] = 0;
        ss->si_uid[i] = 0;
    }
    ss->mask     = 0;
    ss->pending  = 0;
    ss->restorer = 0;
}

int signal_get_foreground(void) {
    return g_foreground_pid;
}

void signal_set_foreground(int pid) {
    g_foreground_pid = pid;
}

/* Walk the wait-queue rooted at *head and unlink p. */
static void wait_queue_unlink(struct proc *p) {
    if (!p || !p->wait_head) return;
    struct proc **slot = p->wait_head;
    while (*slot && *slot != p) slot = &(*slot)->next_wait;
    if (*slot == p) *slot = p->next_wait;
    p->next_wait = 0;
    p->wait_head = 0;
}

#define SIGMASK_STOPS (SIGMASK(SIGSTOP) | SIGMASK(SIGTSTP) | \
                       SIGMASK(SIGTTIN) | SIGMASK(SIGTTOU))

void signal_send(struct proc *p, int sig) {
    if (!p) return;
    if (sig <= 0 || sig >= SIG_MAX) return;
    if (p->pid == 0) return;
    if (p->state == PROC_UNUSED || p->state == PROC_EMBRYO ||
        p->state == PROC_TERMINATED) return;

#ifdef CHROMIUM_BOOT
    /* [sig] Who kills whom. signal_send is the ONLY way a signal becomes
     * pending, so every fatal signal passes here. chrome's GPU process dies of
     * an untraced SIGKILL mid-Mojo-conversation and the sender is unknown --
     * a kill()/tgkill() from another process stamps that process, while a
     * kernel-internal kill shows sender == target (or the interrupted proc). */
    if (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT || sig == SIGSEGV) {
        static int sc = 0;
        if (sc < 120) {
            sc++;
            struct proc *snd = current_proc();
            kprintf("[sig] sig=%d -> pid=%d '%s' FROM pid=%d '%s'%s\n",
                    sig, p->pid, p->name ? p->name : "?",
                    snd ? snd->pid : -1, (snd && snd->name) ? snd->name : "?",
                    (snd == p) ? " (SELF/kernel)" : "");
        }
        /* Capture the RENDERER thread group's user call chains BEFORE it dies
         * -- this is how we finally see the deadlocked renderer's stack. Only
         * the renderer (not gpu/utility): it's the process that must complete a
         * document load for --dump-dom, and it's SIGKILL'd/exits in a window the
         * timer heartbeat can't hit. One-shot inside bt_dump_group. */
        if (sig == SIGKILL && p->is_renderer) {
            extern void bt_dump_group(int tgid);
            bt_dump_group(p->tgid ? p->tgid : p->pid);
        }
    }
#endif

    /* Job control (POSIX): SIGCONT resumes a stopped proc even when SIGCONT
     * itself is ignored or blocked -- the resume happens at SEND time, not
     * delivery. Generating SIGCONT also discards pending stop signals, and
     * generating a stop signal discards a pending SIGCONT (they cancel). */
    if (sig == SIGCONT) {
        p->pending_signals  &= ~SIGMASK_STOPS;
        p->sigstate.pending &= ~SIGMASK_STOPS;
        if (p->state == PROC_STOPPED) {
            p->state = PROC_READY;
            sched_enqueue(p);
        }
    } else if (SIGMASK(sig) & SIGMASK_STOPS) {
        p->pending_signals  &= ~SIGMASK(SIGCONT);
        p->sigstate.pending &= ~SIGMASK(SIGCONT);
    }

    /* Check if signal is ignored (SIG_IGN) and not SIGKILL/SIGSTOP */
    if (sig != SIGKILL && sig != SIGSTOP) {
        struct sigaction *sa = &p->sigstate.actions[sig];
        if (sa->sa_handler == SIG_IGN) return;
    }

    p->pending_signals |= SIGMASK(sig);
    p->sigstate.pending |= SIGMASK(sig);

    /* Record the sender for SA_SIGINFO's siginfo_t. current_proc() is the
     * proc that invoked kill()/raise(), or the interrupted proc for a
     * tty/kernel-generated signal (Ctrl-C from the IRQ path) -- for those
     * "kernel" sources we stamp pid 0, which reads as SI_USER pid 0. */
    {
        struct proc *snd = current_proc();
        bool from_user = snd && snd != p && snd->pid > 0;
        p->sigstate.si_pid[sig] = from_user ? snd->pid : 0;
        p->sigstate.si_uid[sig] = from_user ? snd->uid : 0;
    }

    /* Unblock if asleep */
    if (p->state == PROC_BLOCKED) {
        wait_queue_unlink(p);
        p->state = PROC_READY;
        sched_enqueue(p);
    }

    /* A stopped proc stays stopped for ordinary signals (they stay pending
     * until SIGCONT) -- but SIGKILL must wake it so it can die. */
    if (sig == SIGKILL && p->state == PROC_STOPPED) {
        p->state = PROC_READY;
        sched_enqueue(p);
    }
}

void signal_send_to_pid(int pid, int sig) {
    /* The shell runs as a kernel thread and never returns to user mode, so
     * the normal delivery point -- the return-to-user path -- never runs for
     * it and a `trap ... USR1` handler would never fire. Hand it the signal
     * directly; it dispatches pending traps at the next command boundary,
     * which is where POSIX says a trap action runs. */
    if (shell_owns_pid(pid)) shell_deliver_signal(sig);
    signal_send(proc_lookup(pid), sig);
}

void signal_send_to_foreground(int sig) {
    if (g_foreground_pid > 0) {
        signal_send_to_pid(g_foreground_pid, sig);
    }
}

bool signal_pending_self(void) {
    struct proc *p = current_proc();
    if (!p) return false;
    /* Pending signals not blocked by the mask */
    uint32_t deliverable = p->pending_signals & ~p->sigstate.mask;
    /* SIGKILL and SIGSTOP cannot be blocked */
    deliverable |= p->pending_signals & (SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    return deliverable != 0;
}

/* Determine default action for a signal */
static int signal_default_action(int sig) {
    switch (sig) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    case SIGCONT:
        return 0;  /* ignore by default */
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return 2;  /* stop */
    default:
        return 1;  /* terminate */
    }
}

/* Apply the default disposition (terminate / ignore / stop) for `sig`.
 * Never returns when the action is "terminate". */
static void signal_apply_default(struct proc *p, int sig) {
    int action = signal_default_action(sig);
    if (action == 0) return;          /* ignore */
    if (action == 1) {                /* terminate */
        if (g_foreground_pid == p->pid) g_foreground_pid = 0;
        kprintf("[signal] pid=%d '%s' killed by signal %d\n",
                p->pid, p->name, sig);
        proc_exit(128 + sig);
    }

    /* action == 2: job-control stop. We are running as `p` (delivery happens
     * on the target's own syscall-return / tick path), so park ourselves:
     * leave RUNNING without re-enqueueing -- sched_yield only requeues a
     * PROC_RUNNING proc, so a STOPPED one just gets switched away from,
     * exactly like a BLOCKED proc. signal_send's SIGCONT (or SIGKILL) path
     * later flips us READY + enqueues, and execution resumes right here. */
    kprintf("[signal] pid=%d '%s' stopped by signal %d\n",
            p->pid, p->name, sig);
    /* A TRACED process entering a stop must be visible to its tracer, and it
     * is the TRACER that resumes it, not SIGCONT. Without this the two
     * deadlock on the very first thing any tracing session does: strace's
     * handshake is TRACEME followed by raise(SIGSTOP), so the tracer waits for
     * a ptrace-stop while the tracee sits in a job-control one.
     *
     * NO run-queue work here. An earlier version woke the tracer with
     * sched_enqueue() from this path and wedged the guest to a dead stop
     * (heartbeats=0) -- handoff §7: "signal_send touches the run queue".
     * wait4's traced-child arm polls, so setting the flag IS the
     * notification. */
    if (p->tracer_pid) {
        extern void proc_wake_waiters(int pid);
        p->ptrace_stopsig = sig;
        p->ptrace_stopped = 1;
        proc_wake_waiters(p->pid);
    }
    p->state = PROC_STOPPED;
    sched_yield();
    /* Resumed: by SIGCONT, or by the tracer's PTRACE_CONT/PTRACE_SYSCALL. */
    p->ptrace_stopped = 0;
}

/* Build a signal frame on the user stack and redirect the saved syscall
 * trapframe so the SYSRETQ at the end of the syscall path lands in the
 * handler instead of the interrupted instruction. Returns false if no frame
 * could be built (no restorer registered) -- caller then falls back to the
 * default disposition. Requires a valid `regs` (syscall return path). */
/* Write the siginfo_t + ucontext_t a 3-arg SA_SIGINFO handler reads, in either
 * native tobyOS layout or x86-64 Linux layout (B15), at info_addr/uctx_addr on
 * the user stack. `ctx` carries the saved registers of the interrupted context;
 * rcx/r11 (absent from sig_context) are supplied separately. Returns false on a
 * uaccess failure. */
static bool sig_emit_info_uctx(struct proc *p, int sig, int si_code,
                               uint64_t fault_addr,
                               const struct sig_context *ctx,
                               uint64_t rcx, uint64_t r11,
                               uint64_t info_addr, uint64_t uctx_addr,
                               bool linux_layout) {
    bool is_fault = (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE ||
                     sig == SIGILL  || sig == SIGTRAP);

    if (linux_layout) {
        uint8_t ib[LX_SIGINFO_SIZE];
        uint8_t ub[LX_UCONTEXT_SIZE];
        memset(ib, 0, sizeof ib);
        memset(ub, 0, sizeof ub);

        *(int32_t *)(ib + LXSI_SIGNO) = sig;
        *(int32_t *)(ib + LXSI_ERRNO) = 0;
        *(int32_t *)(ib + LXSI_CODE)  = si_code;
        if (is_fault) {
            *(uint64_t *)(ib + LXSI_ADDR) = fault_addr;
        } else {
            *(int32_t  *)(ib + LXSI_PID) = p->sigstate.si_pid[sig];
            *(uint32_t *)(ib + LXSI_UID) = p->sigstate.si_uid[sig];
        }

        uint64_t *g = (uint64_t *)(ub + LXUC_GREGS);
        g[LXREG_R8]=ctx->r8;   g[LXREG_R9]=ctx->r9;   g[LXREG_R10]=ctx->r10;
        g[LXREG_R11]=r11;      g[LXREG_R12]=ctx->r12; g[LXREG_R13]=ctx->r13;
        g[LXREG_R14]=ctx->r14; g[LXREG_R15]=ctx->r15; g[LXREG_RDI]=ctx->rdi;
        g[LXREG_RSI]=ctx->rsi; g[LXREG_RBP]=ctx->rbp; g[LXREG_RBX]=ctx->rbx;
        g[LXREG_RDX]=ctx->rdx; g[LXREG_RAX]=ctx->rax; g[LXREG_RCX]=rcx;
        g[LXREG_RSP]=ctx->rsp; g[LXREG_RIP]=ctx->rip; g[LXREG_EFL]=ctx->rflags;
        g[LXREG_CSGSFS]=LX_USER_CS; g[LXREG_ERR]=0; g[LXREG_TRAPNO]=0;
        g[LXREG_OLDMASK]=0; g[LXREG_CR2]=fault_addr;
        *(uint64_t *)(ub + LXUC_SIGMASK) = (uint64_t)p->sigstate.mask;

        if (copy_to_user((void *)info_addr, ib, sizeof ib) != 0 ||
            copy_to_user((void *)uctx_addr, ub, sizeof ub) != 0)
            return false;
        return true;
    }

    /* Native tobyOS layout (libtoby <signal.h>). */
    siginfo_t info = {
        .si_signo  = sig,
        .si_code   = si_code,
        .si_pid    = is_fault ? 0 : p->sigstate.si_pid[sig],
        .si_uid    = is_fault ? 0 : p->sigstate.si_uid[sig],
        .si_addr   = (void *)(uintptr_t)(is_fault ? fault_addr : 0),
        .si_status = 0,
        ._pad      = 0,
    };
    ucontext_t uctx;
    memset(&uctx, 0, sizeof(uctx));
    uctx.uc_sigmask = p->sigstate.mask;
    uctx.uc_mcontext.rax = ctx->rax; uctx.uc_mcontext.rbx = ctx->rbx;
    uctx.uc_mcontext.rcx = rcx;      uctx.uc_mcontext.rdx = ctx->rdx;
    uctx.uc_mcontext.rsi = ctx->rsi; uctx.uc_mcontext.rdi = ctx->rdi;
    uctx.uc_mcontext.rbp = ctx->rbp;
    uctx.uc_mcontext.r8  = ctx->r8;  uctx.uc_mcontext.r9  = ctx->r9;
    uctx.uc_mcontext.r10 = ctx->r10; uctx.uc_mcontext.r11 = r11;
    uctx.uc_mcontext.r12 = ctx->r12; uctx.uc_mcontext.r13 = ctx->r13;
    uctx.uc_mcontext.r14 = ctx->r14; uctx.uc_mcontext.r15 = ctx->r15;
    uctx.uc_mcontext.rip = ctx->rip;
    uctx.uc_mcontext.rsp = ctx->rsp;
    uctx.uc_mcontext.rflags = ctx->rflags;
    if (copy_to_user((void *)info_addr, &info, sizeof(info)) != 0 ||
        copy_to_user((void *)uctx_addr, &uctx, sizeof(uctx)) != 0)
        return false;
    return true;
}

/* Compute the on-user-stack siginfo_t / ucontext_t block sizes for the given
 * personality. Linux binaries get the larger Linux-ABI structures. */
static inline uint64_t sig_info_size(bool linux_layout) {
    return linux_layout ? LX_SIGINFO_SIZE : (uint64_t)sizeof(siginfo_t);
}
static inline uint64_t sig_uctx_size(bool linux_layout) {
    return linux_layout ? LX_UCONTEXT_SIZE : (uint64_t)sizeof(ucontext_t);
}

static bool signal_setup_user_frame(struct proc *p, int sig,
                                     struct sigaction *sa,
                                     struct syscall_regs *regs, long rv,
                                     long num) {
    if (!regs) return false;
    if (p->sigstate.restorer == 0) {
        kprintf("[signal] pid=%d sig %d: no sigreturn trampoline registered; "
                "applying default action\n", p->pid, sig);
        return false;
    }

    bool siginfo = (sa->sa_flags & SA_SIGINFO) != 0;
    bool linux_layout = (p->personality == ABI_PERS_LINUX);

    /* Carve the frame out of the user stack, below the SysV red zone. Build
     * each piece in a kernel local and copy out (per-copy uaccess). Layout,
     * high address -> low:
     *     [siginfo_t]      (SA_SIGINFO only)   -- &info passed in RSI
     *     [ucontext_t]     (SA_SIGINFO only)   -- &uctx passed in RDX
     *     [sig_context]                        -- sigreturn reads this
     *     [restorer addr]  == frame            -- handler entry RSP
     * info/uctx sit ABOVE the context so the handler's own (downward) stack
     * growth from `frame` never clobbers them. */
    uint64_t top = regs->user_rsp - 128;      /* skip the 128-byte red zone */
    uint64_t info_addr = 0, uctx_addr = 0;
    if (siginfo) {
        info_addr = (top - sig_info_size(linux_layout)) & ~(uint64_t)0xF;
        uctx_addr = (info_addr - sig_uctx_size(linux_layout)) & ~(uint64_t)0xF;
        top = uctx_addr;
    }
    uint64_t sp = (top - sizeof(struct sig_context)) & ~(uint64_t)0xF;
    struct sig_context kctx;
    struct sig_context *ctx = &kctx;

    /* SA_RESTART: if this handler interrupted a blocking syscall (it bailed
     * with EINTR) and the action asks for restart, resume at the `syscall`
     * instruction itself (2 bytes, 0F 05, so saved-RIP minus 2) with RAX
     * reloaded with the syscall number -- the arg registers below are the
     * originals from the entry trapframe, so after the handler returns via
     * sigreturn the kernel re-executes the interrupted call transparently.
     * Otherwise the handler returns into code that sees rax == -EINTR. */
    /* Linux slice 3: SA_RESTART does NOT apply to every syscall.
     *
     * Linux keeps a set that is never restarted no matter what the handler
     * asked for, because restarting them would be wrong rather than merely
     * surprising: pause() has no success case at all, and the poll/select
     * family would silently restart with their ORIGINAL timeout, turning a
     * bounded wait into an unbounded one.
     *
     * We restarted everything, which is how alarm(1)+pause() hung forever:
     * glibc's signal() sets SA_RESTART, so the SIGALRM handler ran, pause was
     * restarted, and the alarm that would have ended it had already been
     * consumed. Caught by the slice-3 test; the same bug would have hit any
     * poll loop using a SA_RESTART handler as its wakeup. */
    bool never_restart = false;
    if (p->personality == ABI_PERS_LINUX) {
        switch (num) {
        case 7:    /* poll            */  case 23:  /* select          */
        case 34:   /* pause           */  case 35:  /* nanosleep       */
        case 128:  /* rt_sigtimedwait */  case 130: /* rt_sigsuspend   */
        case 230:  /* clock_nanosleep */  case 232: /* epoll_wait      */
        case 270:  /* pselect6        */  case 271: /* ppoll           */
        case 281:  /* epoll_pwait     */
            never_restart = true; break;
        default: break;
        }
    }
    if (rv == EINTR_RET && num >= 0 && (sa->sa_flags & SA_RESTART) &&
        !never_restart) {
        ctx->rax = (uint64_t)num;
        ctx->rip = regs->rcx - 2;
    } else {
        ctx->rax = (uint64_t)rv;
        ctx->rip = regs->rcx;          /* original user RIP    */
    }
    ctx->rdi = regs->rdi; ctx->rsi = regs->rsi; ctx->rdx = regs->rdx;
    ctx->r10 = regs->r10; ctx->r8  = regs->r8;  ctx->r9 = regs->r9;
    ctx->rbx = regs->rbx; ctx->rbp = regs->rbp;
    ctx->r12 = regs->r12; ctx->r13 = regs->r13;
    ctx->r14 = regs->r14; ctx->r15 = regs->r15;
    ctx->rsp       = regs->user_rsp;   /* original user RSP    */
    ctx->rflags    = regs->r11;        /* original user RFLAGS */
    ctx->saved_mask = p->sigstate.mask;
    ctx->magic     = SIG_FRAME_MAGIC;

    /* Write the context onto the user stack through the accessor (this is
     * a kernel-mode write to user memory; a CoW/demand fault inside the
     * window resolves through the page-fault path). */
    if (copy_to_user((void *)sp, &kctx, sizeof(kctx)) != 0) {
        kprintf("[signal] pid=%d sig %d: cannot write signal frame\n",
                p->pid, sig);
        return false;
    }

    /* SA_SIGINFO: also write the siginfo_t and ucontext_t the 3-arg handler
     * expects (native or Linux ABI layout), and pass their addresses in
     * RSI/RDX. regs->rcx/r11 hold the user's RIP/RFLAGS on the sysret frame. */
    if (siginfo) {
        if (!sig_emit_info_uctx(p, sig, SI_USER, 0, ctx,
                                regs->rcx, regs->r11,
                                info_addr, uctx_addr, linux_layout)) {
            kprintf("[signal] pid=%d sig %d: cannot write siginfo/ucontext\n",
                    p->pid, sig);
            return false;
        }
    }

    /* Push the handler's return address (the restorer trampoline) one slot
     * below the context. Handler entry RSP must be %16==8 per SysV; since
     * the context base is 16-aligned, frame = ctx-8 satisfies that. */
    uint64_t frame = (uint64_t)sp - 8;
    if (put_user_u64((void *)frame, p->sigstate.restorer) != 0) {
        kprintf("[signal] pid=%d sig %d: cannot write restorer slot\n",
                p->pid, sig);
        return false;
    }

    /* Block the signal (and sa_mask) for the duration of the handler unless
     * SA_NODEFER. SIGKILL/SIGSTOP can never be blocked. */
    sigset_t newmask = p->sigstate.mask | sa->sa_mask;
    if (!(sa->sa_flags & SA_NODEFER)) newmask |= SIGMASK(sig);
    newmask &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    p->sigstate.mask = newmask;

    /* Capture the handler entry BEFORE the SA_RESETHAND reset below -- we still
     * have to ENTER it this once. (Reading sa->sa_handler AFTER the reset gave
     * rip=SIG_DFL=0, so a one-shot handler -- e.g. chrome/V8's SA_RESETHAND
     * SIGSEGV trap handler -- was "delivered" straight to address 0.) */
    void (*entry)(int) = sa->sa_handler;
    if (sa->sa_flags & SA_RESETHAND) sa->sa_handler = SIG_DFL;

    /* Redirect the return-to-user: RIP -> handler, RSP -> frame, RDI -> sig.
     * For SA_SIGINFO the handler is void(int, siginfo_t*, void*), so also
     * pass &info in RSI and &uctx in RDX (SysV arg regs 2 and 3). */
    regs->rcx      = (uint64_t)(uintptr_t)entry;
    regs->user_rsp = frame;
    regs->rdi      = (uint64_t)sig;
    if (siginfo) {
        regs->rsi = info_addr;
        regs->rdx = uctx_addr;
    }
    return true;
}

/* Core delivery. `regs` is the saved syscall trapframe on the SYSCALL return
 * path, or NULL when called from an IRQ (PIT) where no such frame exists.
 * `rv` is the syscall return value and `num` the syscall number (only
 * meaningful when regs != NULL; pass num = -1 on the IRQ path). */
static void signal_deliver(struct syscall_regs *regs, long rv, long num) {
    struct proc *p = current_proc();
    if (!p || p->pending_signals == 0) return;
    if (p->pid == 0) {
        p->pending_signals = 0;
        p->sigstate.pending = 0;
        return;
    }

    /* Lowest-numbered deliverable signal (unblocked, plus the two that can
     * never be blocked). */
    uint32_t deliverable = p->pending_signals & ~p->sigstate.mask;
    deliverable |= p->pending_signals & (SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    if (deliverable == 0) return;

    int sig = 0;
    for (int i = 1; i < SIG_MAX; i++) {
        if (deliverable & SIGMASK(i)) { sig = i; break; }
    }
    if (sig == 0) return;

    struct sigaction *sa = &p->sigstate.actions[sig];

    /* A caught (user-handler) signal can only be delivered when we have a
     * trapframe to rewrite. On the IRQ path we leave it pending so the next
     * syscall return delivers it -- do NOT consume it here. */
    bool caught = (sig != SIGKILL &&
                   sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN);
    if (caught && !regs) return;

    /* Committed to acting on this signal -- consume it. */
    p->pending_signals  &= ~SIGMASK(sig);
    p->sigstate.pending &= ~SIGMASK(sig);

    /* SIGKILL is always fatal and can never be caught. */
    if (sig == SIGKILL) {
        if (g_foreground_pid == p->pid) g_foreground_pid = 0;
        kprintf("[signal] pid=%d killed by SIGKILL\n", p->pid);
        proc_exit(128 + sig);
    }

    if (sa->sa_handler == SIG_IGN) return;

    if (sa->sa_handler == SIG_DFL) {
        signal_apply_default(p, sig);
        return;
    }

    /* User handler: set up the frame; fall back to default if we can't. */
    if (!signal_setup_user_frame(p, sig, sa, regs, rv, num))
        signal_apply_default(p, sig);
}

/* IRQ-context entry (PIT). Handles fatal/default dispositions; caught
 * handlers are deferred to the next syscall return. */
/* Linux slice 3: fire any alarm(2) deadlines that have come due.
 *
 * Driven from the same timer tick that delivers signals, so an alarm lands
 * even on a process that is asleep in pause()/sigsuspend() -- which is the
 * only reason alarm() is useful. One deadline per process (see proc.h);
 * clearing it BEFORE raising the signal keeps a one-shot from re-firing if
 * the handler takes longer than a tick. */
/* Fire due alarm(2) deadlines.
 *
 * CALLED FROM sched_yield()'s slow path (next to poll_tick), NOT from the
 * timer IRQ. Two failed attempts got us here and both are worth recording:
 *
 *  1. Hanging it off signal_deliver_if_pending() -- which the PIT calls only
 *     when it interrupted RING 3 -- meant the alarm fired for every process
 *     EXCEPT the ones asleep in pause()/sigsuspend(), which hlt() in ring 0.
 *     Those are precisely the callers alarm() exists to wake.
 *
 *  2. Calling it unconditionally from the PIT IRQ instead DEADLOCKED THE
 *     MACHINE: signal_send() does wait_queue_unlink() + sched_enqueue(), and
 *     an IRQ that preempts a context holding the run-queue lock and then
 *     grabs it wedges everything. (That is *why* the handler's signal work is
 *     gated on ring 3 -- the gate is a safety condition, not a style choice.)
 *
 * The yield slow path is the right home: it runs at a ~10 ms cadence, holds
 * the BKL and no other locks, and can therefore call the full signal_send()
 * -- which is what actually WAKES a blocked process rather than just marking
 * it pending. A bare pending-bit set is not enough: a process parked in
 * poll_wait_block() only re-scans when something wakes it, and poll_tick's
 * sweep does not run when pid 0 is parked in proc_wait (the boot-harness
 * shape slice 96 documented). */
void signal_tick_alarms(void) {
    extern struct proc g_proc[];          /* mirrors the idiom used below */
    extern uint64_t perf_now_ns(void);
    uint64_t now = perf_now_ns();
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = &g_proc[i];
        if (p->state == PROC_UNUSED || p->alarm_deadline_ns == 0) continue;
        if (now < p->alarm_deadline_ns) continue;
        p->alarm_deadline_ns = 0;         /* clear BEFORE raising: one-shot */
        signal_send(p, SIGALRM);          /* full send: also WAKES a blocked proc */
    }
}

void signal_deliver_if_pending(void) {
    signal_deliver(0, 0, -1);
}

/* SYSCALL-return entry. Has access to the saved trapframe, so it can deliver
 * caught handlers by pushing a signal frame and redirecting the return. */
void signal_deliver_syscall(long rv, long num) {
    signal_deliver(current_syscall_regs(), rv, num);
}

/* SYNCHRONOUS CPU-fault delivery (B15). Called from the exception dispatcher
 * (isr.c) for a ring-3 fault (#PF/#GP/#DE/#UD/...) that maps to a catchable
 * signal. Builds a signal frame from the CPU exception trapframe `r` and
 * rewrites it so the trailing iretq enters the user handler; the handler later
 * returns through the restorer -> rt_sigreturn, which restores the saved
 * context via sys_sigreturn exactly like the syscall path. Returns false (and
 * leaves `r` untouched) when the process has no usable handler -- the caller
 * then takes the fatal proc_exit path.
 *
 * NOTE: tobyOS's sigreturn restores from its own sig_context, not from the
 * handler-visible ucontext, so a handler cannot RESUME at a different RIP by
 * editing uc_mcontext (returning re-executes the faulting instruction). Real
 * crash handlers that report-and-exit (or longjmp) work; in-place fixups that
 * rely on ucontext-driven resumption do not yet. */
bool signal_deliver_fault(struct regs *r, int sig, int si_code,
                          uint64_t fault_addr) {
    if (!r) return false;
    struct proc *p = current_proc();
    if (!p || p->pid == 0) return false;
    if (sig <= 0 || sig >= SIG_MAX) return false;

    struct sigaction *sa = &p->sigstate.actions[sig];
    if (sa->sa_handler == SIG_DFL || sa->sa_handler == SIG_IGN) return false;
    if (p->sigstate.restorer == 0) return false;
    /* A fault arriving while its own signal is blocked is undefined in POSIX
     * and would recurse forever -- take the fatal path instead. */
    if (p->sigstate.mask & SIGMASK(sig)) return false;

    bool linux_layout = (p->personality == ABI_PERS_LINUX);
    bool siginfo = (sa->sa_flags & SA_SIGINFO) != 0;

    uint64_t top = r->rsp - 128;              /* skip the 128-byte red zone */
    uint64_t info_addr = 0, uctx_addr = 0;
    if (siginfo) {
        info_addr = (top - sig_info_size(linux_layout)) & ~(uint64_t)0xF;
        uctx_addr = (info_addr - sig_uctx_size(linux_layout)) & ~(uint64_t)0xF;
        top = uctx_addr;
    }
    uint64_t sp = (top - sizeof(struct sig_context)) & ~(uint64_t)0xF;

    struct sig_context kctx;
    struct sig_context *ctx = &kctx;
    ctx->rax = r->rax; ctx->rdi = r->rdi; ctx->rsi = r->rsi; ctx->rdx = r->rdx;
    ctx->r10 = r->r10; ctx->r8 = r->r8;   ctx->r9 = r->r9;
    ctx->rbx = r->rbx; ctx->rbp = r->rbp;
    ctx->r12 = r->r12; ctx->r13 = r->r13; ctx->r14 = r->r14; ctx->r15 = r->r15;
    ctx->rip = r->rip;                 /* resume at the faulting instruction */
    ctx->rsp = r->rsp;
    ctx->rflags = r->rflags;
    ctx->saved_mask = p->sigstate.mask;
    ctx->magic = SIG_FRAME_MAGIC;

    if (copy_to_user((void *)sp, &kctx, sizeof(kctx)) != 0)
        return false;

    if (siginfo) {
        if (!sig_emit_info_uctx(p, sig, si_code, fault_addr, ctx,
                                r->rcx, r->r11,
                                info_addr, uctx_addr, linux_layout))
            return false;
    }

    uint64_t frame = (uint64_t)sp - 8;
    if (put_user_u64((void *)frame, p->sigstate.restorer) != 0)
        return false;

    sigset_t newmask = p->sigstate.mask | sa->sa_mask;
    if (!(sa->sa_flags & SA_NODEFER)) newmask |= SIGMASK(sig);
    newmask &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    p->sigstate.mask = newmask;

    /* Capture the handler BEFORE the SA_RESETHAND reset zeroes it (else rip
     * becomes SIG_DFL=0 -- the bug that crashed chrome/V8's one-shot SIGSEGV
     * trap handler at address 0 on a worker thread ~14s into the render path). */
    void (*entry)(int) = sa->sa_handler;
    if (sa->sa_flags & SA_RESETHAND) sa->sa_handler = SIG_DFL;

    /* Rewrite the iret frame: enter the handler with the SysV arg registers. */
    r->rip = (uint64_t)(uintptr_t)entry;
    r->rsp = frame;
    r->rdi = (uint64_t)sig;
    if (siginfo) {
        r->rsi = info_addr;
        r->rdx = uctx_addr;
    }
    return true;
}

/* ---- Syscall implementations ---- */

/* On-user-stack layout of `struct sigaction`, matching libtoby's
 * <signal.h>. NOTE: libtoby's sigset_t is 64-bit (`unsigned long`) while the
 * kernel keeps a 32-bit internal mask, so the user struct is 24 bytes, not
 * 16. We MUST marshal field-by-field through this view -- a wholesale
 * `*kern = *user` struct copy would misread sa_flags from the wrong offset
 * (the old code did exactly that, which is why sa_flags was always garbage
 * before handler delivery existed). */
struct abi_sigaction {
    uint64_t sa_handler;
    uint64_t sa_mask;     /* user sigset_t is 64-bit */
    uint32_t sa_flags;
    uint32_t _pad;
};

int sys_sigaction(int sig, const void *uact, void *uoldact) {
    struct proc *p = current_proc();
    if (!p) return -1;
    if (sig <= 0 || sig >= SIG_MAX) return -22; /* EINVAL */
    if (sig == SIGKILL || sig == SIGSTOP) return -22; /* can't change */

    struct sigaction *cur = &p->sigstate.actions[sig];

    if (uoldact) {
        struct abi_sigaction old = {
            .sa_handler = (uint64_t)(uintptr_t)cur->sa_handler,
            .sa_mask    = (uint64_t)cur->sa_mask,
            .sa_flags   = (uint32_t)cur->sa_flags,
            ._pad       = 0,
        };
        if (copy_to_user(uoldact, &old, sizeof(old)) != 0) return -14;
    }
    if (uact) {
        struct abi_sigaction a;
        if (copy_from_user(&a, uact, sizeof(a)) != 0) return -14;
        cur->sa_handler = (void (*)(int))(uintptr_t)a.sa_handler;
        cur->sa_mask    = (sigset_t)a.sa_mask;
        cur->sa_flags   = (int)a.sa_flags;
    }
    return 0;
}

int sys_sigprocmask(int how, const void *uset, void *uoldset) {
    struct proc *p = current_proc();
    if (!p) return -1;

    /* User sigset_t is 64-bit; read/write the full 8 bytes even though only
     * the low SIG_MAX bits are meaningful here. */
    if (uoldset && put_user_u64(uoldset, (uint64_t)p->sigstate.mask) != 0)
        return -14; /* EFAULT */

    if (uset) {
        uint64_t s;
        if (get_user_u64(&s, uset) != 0) return -14;
        s &= ~(uint64_t)(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));

        switch (how) {
        case SIG_BLOCK:   p->sigstate.mask |= (sigset_t)s;  break;
        case SIG_UNBLOCK: p->sigstate.mask &= ~(sigset_t)s; break;
        case SIG_SETMASK: p->sigstate.mask  = (sigset_t)s;  break;
        default:          return -22; /* EINVAL */
        }
    }
    return 0;
}

/* Register the user-space sigreturn trampoline. Called once by libc startup
 * (lazily, on the first sigaction). The kernel pushes this address as the
 * handler's return address so that when the handler returns it traps back
 * into sys_sigreturn. */
void sys_sigrestorer(uint64_t addr) {
    struct proc *p = current_proc();
    if (p) p->sigstate.restorer = addr;
}

/* Restore the interrupted context saved by signal_setup_user_frame. The
 * handler has returned to the restorer trampoline, which issued SYS_SIGRETURN
 * with RSP pointing at the saved sig_context. We copy that context back into
 * the syscall trapframe so the SYSRETQ at the end of this very syscall
 * resumes the originally-interrupted instruction. Returns the value to leave
 * in the user's RAX. */
long sys_sigreturn(void) {
    struct proc *p = current_proc();
    struct syscall_regs *regs = current_syscall_regs();
    if (!p || !regs) return -1;

    /* Copy the saved context off the user stack through the accessor
     * (per-copy uaccess) before trusting any field. */
    struct sig_context kctx;
    if (copy_from_user(&kctx, (const void *)regs->user_rsp,
                       sizeof(kctx)) != 0) {
        kprintf("[signal] pid=%d sigreturn: unreadable frame -- killing\n",
                p->pid);
        if (g_foreground_pid == p->pid) g_foreground_pid = 0;
        proc_exit(128 + SIGSEGV);
    }
    struct sig_context *ctx = &kctx;
    if (ctx->magic != SIG_FRAME_MAGIC) {
        kprintf("[signal] pid=%d sigreturn: bad frame magic 0x%llx -- "
                "killing\n", p->pid, (unsigned long long)ctx->magic);
        if (g_foreground_pid == p->pid) g_foreground_pid = 0;
        proc_exit(128 + SIGSEGV);
    }

    regs->rdi = ctx->rdi; regs->rsi = ctx->rsi; regs->rdx = ctx->rdx;
    regs->r10 = ctx->r10; regs->r8  = ctx->r8;  regs->r9 = ctx->r9;
    regs->rbx = ctx->rbx; regs->rbp = ctx->rbp;
    regs->r12 = ctx->r12; regs->r13 = ctx->r13;
    regs->r14 = ctx->r14; regs->r15 = ctx->r15;
    regs->rcx      = ctx->rip;      /* resume RIP    */
    regs->r11      = ctx->rflags;   /* resume RFLAGS */
    regs->user_rsp = ctx->rsp;      /* resume RSP    */

    p->sigstate.mask = (sigset_t)ctx->saved_mask;

    return (long)ctx->rax;          /* becomes user RAX after SYSRETQ */
}

int sys_kill(int pid, int sig) {
    if (sig < 0 || sig >= SIG_MAX) return -22;

    struct proc *p = current_proc();
    if (!p) return -1;

    /* kill()/raise() is an explicit USER source: the siginfo si_pid/si_uid
     * must report the calling process (the sender) -- including the self-kill
     * case kill(getpid(), sig), which Linux reports as si_pid == getpid().
     * signal_send's generic stamp can't tell a kill() syscall from a tty/kernel
     * source, so stamp the true sender here, after delivery. */
    /* Slice 10: `pid` arrives as a vpid in the CALLER's pid namespace, and
     * si_pid must be reported as the sender's number in the TARGET's namespace.
     * Both are identity in the initial namespace, so nothing changes for a
     * system that never used CLONE_NEWPID. */
    if (pid > 0) {
        int kpid = pid_knr(pid);
        if (!kpid) return -3;                        /* not visible => ESRCH */
        struct proc *target = proc_lookup(kpid);
        if (!target) return -3; /* ESRCH */
        signal_send(target, sig);
        if (sig > 0) {
            target->sigstate.si_pid[sig] = pid_vnr_in(target->pid_ns, p->pid);
            target->sigstate.si_uid[sig] = p->uid;
        }
    } else if (pid == 0) {
        /* Send to all processes in the same process group (simplified:
         * send to all in same session) */
        extern struct proc g_proc[];
        for (int i = 1; i < PROC_MAX; i++) {
            if (g_proc[i].state != PROC_UNUSED &&
                g_proc[i].state != PROC_EMBRYO &&
                g_proc[i].session_id == p->session_id) {
                /* A broadcast must not escape the caller's pid namespace: a
                 * container that can signal the host is not a container. The
                 * visibility map is the whole check. */
                if (!pid_ns_can_see(p, &g_proc[i])) continue;
                signal_send(&g_proc[i], sig);
                if (sig > 0) {
                    g_proc[i].sigstate.si_pid[sig] =
                        pid_vnr_in(g_proc[i].pid_ns, p->pid);
                    g_proc[i].sigstate.si_uid[sig] = p->uid;
                }
            }
        }
    } else if (pid == -1) {
        /* Send to all processes (except pid 0) */
        extern struct proc g_proc[];
        for (int i = 1; i < PROC_MAX; i++) {
            if (g_proc[i].state != PROC_UNUSED &&
                g_proc[i].state != PROC_EMBRYO) {
                if (!pid_ns_can_see(p, &g_proc[i])) continue;   /* slice 10 */
                signal_send(&g_proc[i], sig);
                if (sig > 0) {
                    g_proc[i].sigstate.si_pid[sig] =
                        pid_vnr_in(g_proc[i].pid_ns, p->pid);
                    g_proc[i].sigstate.si_uid[sig] = p->uid;
                }
            }
        }
    }
    return 0;
}
