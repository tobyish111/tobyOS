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
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/printk.h>
#include <tobyos/uaccess.h>
#include <tobyos/klibc.h>

static int g_foreground_pid = 0;

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
    if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED) return;

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
    p->state = PROC_STOPPED;
    sched_yield();
    /* SIGCONT arrived: we're RUNNING again; resume the interrupted flow. */
}

/* Build a signal frame on the user stack and redirect the saved syscall
 * trapframe so the SYSRETQ at the end of the syscall path lands in the
 * handler instead of the interrupted instruction. Returns false if no frame
 * could be built (no restorer registered) -- caller then falls back to the
 * default disposition. Requires a valid `regs` (syscall return path). */
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
        info_addr = (top - sizeof(siginfo_t)) & ~(uint64_t)0xF;
        uctx_addr = (info_addr - sizeof(ucontext_t)) & ~(uint64_t)0xF;
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
    if (rv == EINTR_RET && num >= 0 && (sa->sa_flags & SA_RESTART)) {
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
     * expects, and pass their addresses in RSI/RDX. */
    if (siginfo) {
        siginfo_t info = {
            .si_signo = sig,
            .si_code  = SI_USER,
            .si_pid   = p->sigstate.si_pid[sig],
            .si_uid   = p->sigstate.si_uid[sig],
            .si_addr  = 0,
            .si_status = 0,
            ._pad     = 0,
        };
        ucontext_t uctx;
        memset(&uctx, 0, sizeof(uctx));
        uctx.uc_sigmask = p->sigstate.mask;
        uctx.uc_mcontext.rax = ctx->rax; uctx.uc_mcontext.rbx = ctx->rbx;
        uctx.uc_mcontext.rcx = regs->rcx; uctx.uc_mcontext.rdx = ctx->rdx;
        uctx.uc_mcontext.rsi = ctx->rsi; uctx.uc_mcontext.rdi = ctx->rdi;
        uctx.uc_mcontext.rbp = ctx->rbp;
        uctx.uc_mcontext.r8  = ctx->r8;  uctx.uc_mcontext.r9  = ctx->r9;
        uctx.uc_mcontext.r10 = ctx->r10; uctx.uc_mcontext.r11 = regs->r11;
        uctx.uc_mcontext.r12 = ctx->r12; uctx.uc_mcontext.r13 = ctx->r13;
        uctx.uc_mcontext.r14 = ctx->r14; uctx.uc_mcontext.r15 = ctx->r15;
        uctx.uc_mcontext.rip = ctx->rip;
        uctx.uc_mcontext.rsp = ctx->rsp;
        uctx.uc_mcontext.rflags = ctx->rflags;
        if (copy_to_user((void *)info_addr, &info, sizeof(info)) != 0 ||
            copy_to_user((void *)uctx_addr, &uctx, sizeof(uctx)) != 0) {
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

    if (sa->sa_flags & SA_RESETHAND) sa->sa_handler = SIG_DFL;

    /* Redirect the return-to-user: RIP -> handler, RSP -> frame, RDI -> sig.
     * For SA_SIGINFO the handler is void(int, siginfo_t*, void*), so also
     * pass &info in RSI and &uctx in RDX (SysV arg regs 2 and 3). */
    regs->rcx      = (uint64_t)(uintptr_t)sa->sa_handler;
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
void signal_deliver_if_pending(void) {
    signal_deliver(0, 0, -1);
}

/* SYSCALL-return entry. Has access to the saved trapframe, so it can deliver
 * caught handlers by pushing a signal frame and redirecting the return. */
void signal_deliver_syscall(long rv, long num) {
    signal_deliver(current_syscall_regs(), rv, num);
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

    if (pid > 0) {
        struct proc *target = proc_lookup(pid);
        if (!target) return -3; /* ESRCH */
        signal_send(target, sig);
    } else if (pid == 0) {
        /* Send to all processes in the same process group (simplified:
         * send to all in same session) */
        extern struct proc g_proc[];
        for (int i = 1; i < PROC_MAX; i++) {
            if (g_proc[i].state != PROC_UNUSED &&
                g_proc[i].session_id == p->session_id) {
                signal_send(&g_proc[i], sig);
            }
        }
    } else if (pid == -1) {
        /* Send to all processes (except pid 0) */
        extern struct proc g_proc[];
        for (int i = 1; i < PROC_MAX; i++) {
            if (g_proc[i].state != PROC_UNUSED) {
                signal_send(&g_proc[i], sig);
            }
        }
    }
    return 0;
}
