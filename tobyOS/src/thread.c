/* thread.c -- Per-process threading (Phase 1, M1.1).
 *
 * Threads are lightweight procs that share the leader's address space
 * (CR3), file descriptor table, heap (brk), capabilities, and sandbox.
 * Each thread has its own:
 *   - kernel stack (for syscalls / IRQ entry)
 *   - user stack (provided by caller at creation)
 *   - saved_rsp (context switch state)
 *   - TLS base (FS segment, per-thread local storage)
 *   - pending signals (signal mask is shared in the future)
 *
 * The scheduler treats threads identically to processes -- they sit
 * on the ready queue as normal struct proc entries. The only special
 * behavior is at exit/wait:
 *   - A non-leader thread that exits does NOT free the PML4 or fds.
 *   - The leader exiting kills all threads in its group.
 *   - thread_join() only waits for a specific TID in the same group.
 *
 * Futex: kernel-mediated atomic sleep/wake on userland addresses.
 * A simple hash table of wait queues indexed by (cr3, uaddr) allows
 * efficient wakeup without scanning all blocked procs.
 */

#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/heap.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/perf.h>
#include <tobyos/cpu.h>
#include <tobyos/uaccess.h>

/* ---- Thread creation ------------------------------------------------ */

extern struct proc g_proc[];

static struct proc *thread_alloc_slot(void) {
    for (int i = 1; i < PROC_MAX; i++) {
        if (g_proc[i].state == PROC_UNUSED) return &g_proc[i];
    }
    return 0;
}

int thread_create(uint64_t entry, uint64_t arg,
                  uint64_t user_stack_top, uint64_t tls_base) {
    struct proc *leader = current_proc();
    if (!leader) return -1;

    /* Find the actual thread-group leader */
    struct proc *tg_leader = leader;
    if (leader->is_thread && leader->tgid != leader->pid) {
        tg_leader = proc_lookup(leader->tgid);
        if (!tg_leader) return -1;
    }

    /* Count existing threads in group */
    int count = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proc[i].state != PROC_UNUSED && g_proc[i].tgid == tg_leader->pid)
            count++;
    }
    if (count >= THREAD_MAX_PER_PROC) return -1;

    struct proc *t = thread_alloc_slot();
    if (!t) return -1;

    memset(t, 0, sizeof(*t));
    t->pid   = (int)(t - g_proc);
    t->ppid  = tg_leader->ppid;
    t->state = PROC_UNUSED;  /* upgraded at end */

    /* Copy name with "+T" suffix */
    size_t nlen = strlen(tg_leader->name);
    if (nlen > PROC_NAME_MAX - 3) nlen = PROC_NAME_MAX - 3;
    memcpy(t->name, tg_leader->name, nlen);
    t->name[nlen] = '+';
    t->name[nlen + 1] = 'T';
    t->name[nlen + 2] = '\0';

    /* Share the leader's address space */
    t->cr3       = tg_leader->cr3;
    t->owns_pml4 = false;  /* thread never frees the PML4 */

    /* Allocate a kernel stack for this thread */
    t->kstack_base = kmalloc(PROC_KSTACK_SZ);
    if (!t->kstack_base) {
        t->state = PROC_UNUSED;
        return -1;
    }
    t->kstack_top = (void *)((uint64_t)t->kstack_base + PROC_KSTACK_SZ);

    /* Share the leader's heap and memory layout */
    t->brk_base = tg_leader->brk_base;
    t->brk_cur  = tg_leader->brk_cur;
    t->brk_max  = tg_leader->brk_max;

    /* Share the leader's user stack region info (not the actual stack) */
    t->user_stack_base  = user_stack_top - (8 * 4096); /* approximate */
    t->user_stack_pages = 8;

    /* User entry and stack */
    t->user_entry = entry;
    t->user_rsp   = user_stack_top;
    t->user_arg   = arg;

    /* Copy leader's CWD */
    memcpy(t->cwd, tg_leader->cwd, ABI_PATH_MAX);

    /* Share fd table by pointing to the same fds */
    memcpy(t->fds, tg_leader->fds, sizeof(t->fds));

    /* Inherit identity and caps */
    t->session_id = tg_leader->session_id;
    t->uid        = tg_leader->uid;
    t->gid        = tg_leader->gid;
    t->caps       = tg_leader->caps;
    memcpy(t->sandbox_root, tg_leader->sandbox_root, PROC_SANDBOX_MAX);

    /* Thread-group fields */
    t->tgid        = tg_leader->pid;
    t->is_thread   = true;
    t->is_renderer = tg_leader->is_renderer;   /* inherit so exit-hook fires on any thread */
    t->detached    = false;
    t->tls_base    = tls_base;
    t->join_waiters = 0;
    /* NB: no kprintf here -- a clone-time trace in slice 38 perturbed
     * thread-startup timing enough to flake FUTEXTEST's wake race. The
     * [isr] TLS dump plus the arch_prctl trace below cover provenance. */

    /* Build the initial kernel stack frame. We replicate the pattern
     * from spawn_internal: push a fake frame so that when the scheduler
     * context-switches to this thread via proc_context_switch(), it
     * "returns" to proc_first_user_entry which iretqs to ring 3 at
     * (user_entry, user_rsp).
     *
     * However, for threads we also need to pass `arg` in RDI. We do this
     * by setting user_rsp to point to a small trampoline:
     * The thread's entry is called with arg in RDI via the ABI.
     * We store arg at a known location that proc_first_user_entry can
     * load -- actually, we just push arg onto the user stack so the
     * thread function sees it via the standard calling convention.
     *
     * Actually, the simplest approach: we set user_entry = entry, and
     * place arg in a register that survives through iretq. The user_rsp
     * is the stack top. We'll modify proc_first_user_entry to also
     * set RDI from a field, OR we can just place arg on the user stack.
     *
     * For simplicity, push arg onto the user stack (rsp-8). The thread
     * function in userland pops it as its first argument. Actually no --
     * SystemV ABI passes first arg in RDI, so we need to set RDI.
     *
     * The cleanest solution: store arg in a proc field and have
     * proc_first_user_entry load it into RDI before iretq. Let's use
     * exit_code temporarily (it's unused before the thread runs). */

    /* Store thread arg in a temporary location -- we'll piggyback on
     * the wait_pid field which is -1 (unused at creation time). Actually
     * let's just use the `exit_code` field which has no meaning before
     * the thread terminates. We'll cast it to hold the arg (low 32 bits)
     * and put the high 32 bits in wait_pid. */

    /* BETTER: we align user_rsp - 8 and write a return address of 0
     * (thread_exit stub) plus put arg as if we're calling entry(arg).
     * In SystemV x86-64, arg goes in RDI. We'll store it in a new field.
     * For now, let's use a simpler approach: the thread trampoline in
     * userland handles this. Just set entry and stack_top normally. */

    /* Build a minimal initial kernel stack frame identical to what
     * spawn_internal does (see comment at top of proc.c).
     *
     * Layout from kstack_top downward:
     *   [kstack_top - 8]  = padding (alignment)
     *   [kstack_top - 16] = RIP = proc_first_user_entry (ret pops this)
     *   [kstack_top - 24] = rbp slot = 0
     *   [kstack_top - 32] = rbx slot = 0
     *   [kstack_top - 40] = r12 slot = 0
     *   [kstack_top - 48] = r13 slot = 0
     *   [kstack_top - 56] = r14 slot = 0
     *   [kstack_top - 64] = r15 slot = 0
     *   [kstack_top - 72] = RFLAGS = 0x202  <-- saved_rsp points here
     *
     * proc_context_switch does: popfq, pop r15..r12, pop rbx, pop rbp, ret
     */
    /* Clean default FPU/SSE state -- proc_first_user_entry fxrstor's it. */
    fpu_init_default(t->fpu_state);
    {
        uint64_t *sp = (uint64_t *)t->kstack_top;
        sp -= 1;  /* alignment padding */
        sp -= 1;  *sp = (uint64_t)proc_first_user_entry; /* ret addr */
        sp -= 1;  *sp = 0;  /* rbp */
        sp -= 1;  *sp = 0;  /* rbx */
        sp -= 1;  *sp = 0;  /* r12 */
        sp -= 1;  *sp = 0;  /* r13 */
        sp -= 1;  *sp = 0;  /* r14 */
        sp -= 1;  *sp = 0;  /* r15 */
        sp -= 1;  *sp = 0x202; /* RFLAGS (IF=1) */

        t->saved_rsp = (uint64_t)sp;
    }

    /* Performance bookkeeping */
    t->created_ns = perf_now_ns();
    t->exit_code  = 0;
    t->wait_pid   = -1;

    /* Make it runnable */
    t->state = PROC_READY;
    sched_enqueue(t);

    kprintf("[thread] created tid=%d in tgid=%d entry=%p stack=%p tls=%p\n",
            t->pid, t->tgid, (void *)entry,
            (void *)user_stack_top, (void *)tls_base);

    return t->pid;
}

/* ---- Thread exit ---------------------------------------------------- */

void thread_exit(int code) {
    struct proc *t = current_proc();
    if (!t) for (;;) hlt();

    kprintf("[thread] tid=%d exiting with code %d\n", t->pid, code);

    t->exit_code = code;

    /* Wake any joiners */
    struct proc *waiter = t->join_waiters;
    while (waiter) {
        struct proc *next = waiter->next_wait;
        waiter->state = PROC_READY;
        waiter->next_wait = 0;
        sched_enqueue(waiter);
        waiter = next;
    }
    t->join_waiters = 0;

    /* If detached, mark for immediate reap */
    if (t->detached || !t->is_thread) {
        /* For the leader or detached threads, use normal proc_exit */
        proc_exit(code);
    }

    /* Non-leader thread: don't free address space, just terminate */
    t->state = PROC_TERMINATED;

    /* Free kernel stack later (at join/reap time). For now just yield. */
    sched_yield();

    /* Should never reach here */
    for (;;) hlt();
}

/* ---- Thread join ----------------------------------------------------- */

int thread_join(int tid, int *out_code) {
    struct proc *caller = current_proc();
    if (!caller) return -1;

    struct proc *target = proc_lookup(tid);
    if (!target) return -1;

    /* Must be in the same thread group */
    if (target->tgid != caller->tgid) return -1;

    /* Can't join yourself */
    if (target->pid == caller->pid) return -1;

    /* Can't join a detached thread */
    if (target->detached) return -1;

    /* If already terminated, reap immediately */
    if (target->state == PROC_TERMINATED) {
        if (out_code) *out_code = target->exit_code;
        /* Free kernel stack */
        if (target->kstack_base) kfree(target->kstack_base);
        target->kstack_base = 0;
        target->kstack_top  = 0;
        target->state = PROC_UNUSED;
        return 0;
    }

    /* Block until target exits */
    caller->state = PROC_BLOCKED;
    caller->next_wait = target->join_waiters;
    target->join_waiters = caller;

    sched_yield();

    /* We've been woken -- target should be terminated now */
    if (out_code && target->state == PROC_TERMINATED)
        *out_code = target->exit_code;

    /* Reap */
    if (target->state == PROC_TERMINATED) {
        if (target->kstack_base) kfree(target->kstack_base);
        target->kstack_base = 0;
        target->kstack_top  = 0;
        target->state = PROC_UNUSED;
    }

    return 0;
}

/* ---- Thread detach -------------------------------------------------- */

int thread_detach(int tid) {
    struct proc *target = proc_lookup(tid);
    if (!target) return -1;

    struct proc *caller = current_proc();
    if (target->tgid != caller->tgid) return -1;

    target->detached = true;

    /* If already terminated, reap now */
    if (target->state == PROC_TERMINATED) {
        if (target->kstack_base) kfree(target->kstack_base);
        target->kstack_base = 0;
        target->kstack_top  = 0;
        target->state = PROC_UNUSED;
    }
    return 0;
}

/* ---- TLS base ------------------------------------------------------- */

void thread_set_tls(uint64_t base) {
    struct proc *t = current_proc();
    if (t) t->tls_base = base;
#ifdef CHROMIUM_BOOT
    /* Slice 38: pairs with the [tls] clone trace (see thread_create). */
    if (t && t->personality == 1 /* ABI_PERS_LINUX */)
        kprintf("[tls] arch_prctl pid=%d fs=%p\n", t->pid, (void *)base);
#endif
    /* Immediately update FS.base MSR for the calling thread */
    wrmsr(0xC0000100, base);  /* MSR_FS_BASE */
}

/* ---- Futex ---------------------------------------------------------- */

#define FUTEX_HASH_SIZE  64

struct futex_entry {
    uint64_t     key_cr3;
    uint64_t     key_addr;
    struct proc *waiters;
    struct futex_entry *next;
};

static struct futex_entry  g_futex_pool[256];
static struct futex_entry *g_futex_free;
static struct futex_entry *g_futex_hash[FUTEX_HASH_SIZE];
static spinlock_t          g_futex_lock;

void futex_init(void) {
    g_futex_free = 0;
    for (int i = 0; i < 256; i++) {
        g_futex_pool[i].next = g_futex_free;
        g_futex_free = &g_futex_pool[i];
    }
    memset(g_futex_hash, 0, sizeof(g_futex_hash));
    spin_init(&g_futex_lock);
}

static uint32_t futex_hash(uint64_t cr3, uint64_t addr) {
    uint64_t h = cr3 ^ (addr >> 2) ^ (addr >> 14);
    return (uint32_t)(h % FUTEX_HASH_SIZE);
}

static struct futex_entry *futex_find_or_create(uint64_t cr3, uint64_t addr) {
    uint32_t idx = futex_hash(cr3, addr);
    struct futex_entry *e = g_futex_hash[idx];
    while (e) {
        if (e->key_cr3 == cr3 && e->key_addr == addr) return e;
        e = e->next;
    }
    /* Allocate new */
    if (!g_futex_free) return 0;
    e = g_futex_free;
    g_futex_free = e->next;
    e->key_cr3  = cr3;
    e->key_addr = addr;
    e->waiters  = 0;
    e->next     = g_futex_hash[idx];
    g_futex_hash[idx] = e;
    return e;
}

static void futex_free_entry(struct futex_entry *e) {
    if (!e) return;
    /* Unlink from hash */
    uint32_t idx = futex_hash(e->key_cr3, e->key_addr);
    struct futex_entry **pp = &g_futex_hash[idx];
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp == e) *pp = e->next;
    /* Return to free list */
    e->next = g_futex_free;
    g_futex_free = e;
}

/* ---- Slice 64 (perf tier 2): BKL-free futex fast paths ---------------
 *
 * The futex subsystem's state (hash, waiter lists, pool) is entirely
 * g_futex_lock-protected -- the BKL wraps futex() only because the generic
 * syscall entry takes it. Chrome's futex traffic is dominated by two cases
 * that never touch the scheduler and can be served BEFORE bkl_enter:
 *
 *   WAIT with *uaddr != val  -> -EAGAIN   (caller re-checks; no park)
 *   WAKE with no waiter list -> 0         (every uncontended unlock)
 *
 * Correctness: the no-lost-wakeup argument is unchanged from the slow
 * path. A parking waiter holds g_futex_lock while re-checking *uaddr and
 * enqueueing; our WAKE fast path takes the same lock for its lookup, so it
 * either sees the fully-parked waiter (-> falls through to the BKL path to
 * wake it) or the waiter has not re-checked yet -- and since the waker
 * changed the futex word BEFORE calling WAKE (the futex protocol), that
 * waiter's locked re-check will see the new value and EAGAIN out.
 * Ordering: g_futex_lock -> per-CPU queue locks exists (wake/sweep); these
 * fast paths take g_futex_lock alone, and nothing under it takes the BKL.
 * Return 1 = fully served (*out set); 0 = fall through to the BKL path. */
int futex_fast(uint32_t *uaddr, int op, uint32_t val, long *out) {
    struct proc *caller = current_proc();
    if (!caller) return 0;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    if (!user_range_ok(addr, sizeof(uint32_t))) { *out = -14; return 1; }
    int cmd = op & 0x7f;

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        uint32_t cur;
        if (copy_from_user(&cur, uaddr, sizeof cur) != 0) { *out = -14; return 1; }
        if (cur != val) { *out = -11; return 1; }          /* -EAGAIN */
        return 0;                       /* would park: slow path under BKL */
    }
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        uint64_t cr3 = caller->cr3;
        uint64_t flags = spin_lock_irqsave(&g_futex_lock);
        uint32_t idx = futex_hash(cr3, addr);
        struct futex_entry *e = g_futex_hash[idx];
        while (e && !(e->key_cr3 == cr3 && e->key_addr == addr)) e = e->next;
        spin_unlock_irqrestore(&g_futex_lock, flags);
        if (e) return 0;                /* waiters exist: wake under BKL path */
        *out = 0; return 1;             /* nobody to wake */
    }
    return 0;
}

long futex(uint32_t *uaddr, int op, uint32_t val, const void *utimeout) {
    struct proc *caller = current_proc();
    if (!caller) return -1;

    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    uint64_t cr3  = caller->cr3;
    if (!user_range_ok(addr, sizeof(uint32_t))) return -14; /* EFAULT */

    int cmd = op & 0x7f;   /* strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256) */

    /* Pre-touch the futex word OUTSIDE the spinlock so any CoW/demand #PF
     * resolves with IRQs on; the locked re-read below then can't fault
     * (per-copy uaccess: each read opens its own stac window). */
    uint32_t cur_val;
    if (copy_from_user(&cur_val, uaddr, sizeof(cur_val)) != 0) return -14;

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        /* Compute the wake deadline (monotonic ns). NULL => infinite. FUTEX_WAIT
         * takes a RELATIVE timeout; FUTEX_WAIT_BITSET (glibc's pthread_cond_
         * timedwait / mutex_timedlock) takes an ABSOLUTE one -- and tobyOS's
         * clock_gettime/gettimeofday are all perf_now_ns-based, so a chrome
         * absolute deadline is directly comparable to perf_now_ns(). Honouring
         * the timeout is essential: without it, every timed wait blocked FOREVER
         * -> worker threads hung -> Chromium's deadline CHECKs fired (int3). */
        uint64_t deadline_ns = 0;
        if (utimeout) {
            struct { int64_t sec; int64_t nsec; } ts;
            if (copy_from_user(&ts, utimeout, sizeof ts) == 0) {
                uint64_t t = (uint64_t)ts.sec * 1000000000ull + (uint64_t)ts.nsec;
                if (cmd == FUTEX_WAIT_BITSET) {
                    deadline_ns = t;                       /* absolute */
                    if (op & 0x100) {
                        /* FUTEX_CLOCK_REALTIME (slice 56): the deadline is
                         * absolute UNIX-EPOCH ns (glibc default-clock condvars /
                         * sem_timedwait). Comparing that against boot-relative
                         * perf_now_ns() made the wait effectively INFINITE
                         * (~1.7e18 vs ~1e11). Rebase into the monotonic clock. */
                        extern uint64_t lx_realtime_offset_ns(void);
                        uint64_t off = lx_realtime_offset_ns();
                        deadline_ns = (t > off) ? t - off : 1;
                    }
                } else {
                    deadline_ns = perf_now_ns() + t;       /* relative */
                }
                if (deadline_ns == 0) deadline_ns = 1;   /* nonzero => "timed" */
            }
        }
#ifdef CHROMIUM_BOOT
        /* Slice 56 wake-latency instrument, park side. A deadline more than
         * 2 minutes out is almost certainly built on the WRONG CLOCK (e.g. an
         * absolute CLOCK_REALTIME deadline -- unix-epoch ns -- compared against
         * boot-relative perf_now_ns): chrome's real timers are ms..seconds. */
        caller->fx_park_ns = perf_now_ns();
        caller->fx_wake_ns = 0;
        if (op & 0x100) {                    /* FUTEX_CLOCK_REALTIME requested */
            extern uint64_t g_fx_rt_flag_count;
            g_fx_rt_flag_count++;
            static int rtl = 0;
            if (rtl < 16) { rtl++;
                kprintf("[fxrt] pid=%d futex op=0x%x CLOCK_REALTIME abs deadline"
                        " (rebased to monotonic)\n", caller->pid, op);
            }
        }
        if (deadline_ns && deadline_ns > caller->fx_park_ns &&
            deadline_ns - caller->fx_park_ns > 120000000000ull) {
            static int hp = 0;
            if (hp < 32) { hp++;
                kprintf("[fxpark] pid=%d addr=0x%lx deadline +%lums out (HUGE)\n",
                        caller->pid, (unsigned long)addr,
                        (unsigned long)((deadline_ns - caller->fx_park_ns)
                                        / 1000000ull));
            }
        }
#endif

        /* Atomically: if *uaddr == val, wait. Otherwise return -EAGAIN. */
        uint64_t flags = spin_lock_irqsave(&g_futex_lock);
        unsigned long uw = uaccess_begin();
        cur_val = *(volatile uint32_t *)uaddr;
        uaccess_end(uw);
        if (cur_val != val) {
            spin_unlock_irqrestore(&g_futex_lock, flags);
            return -11; /* -EAGAIN */
        }

        /* Block ON the wait list, whether timed or not. This is the DL2 fix:
         * the old code gave TIMED waits a SEPARATE poll-only path that watched
         * *uaddr and was NOT on the wait list, so FUTEX_WAKE could not wake them
         * -- only a *uaddr change could. glibc's pthread_cond_timedwait /
         * mutex_timedlock (pervasive in chrome) are woken by FUTEX_WAKE and do
         * NOT always change the word the waiter parked on, so those wakeups were
         * silently lost and chrome's renderer deadlocked. Now both timed and
         * untimed waiters register on the list; FUTEX_WAKE wakes them, and the
         * periodic futex_expire_timeouts() sweep wakes any past its deadline.
         * Validated in isolation by /bin/linux-futex (FAIL -> PASS). */
        if (deadline_ns && perf_now_ns() >= deadline_ns) {
            spin_unlock_irqrestore(&g_futex_lock, flags);
            return -110; /* -ETIMEDOUT: deadline already in the past */
        }
        struct futex_entry *e = futex_find_or_create(cr3, addr);
        if (!e) {
            spin_unlock_irqrestore(&g_futex_lock, flags);
            return -12; /* -ENOMEM */
        }
        caller->state             = PROC_BLOCKED;
        caller->futex_deadline_ns = deadline_ns;   /* 0 = infinite */
        caller->futex_timed_out   = false;
        caller->next_wait = e->waiters;
        e->waiters = caller;
        spin_unlock_irqrestore(&g_futex_lock, flags);
        sched_yield();     /* woken by FUTEX_WAKE or the timeout sweep */
#ifdef CHROMIUM_BOOT
        /* Slice 56 wake-latency instrument, wake side: requested vs actual.
         * rdy = time between the wake being DELIVERED (state->READY) and this
         * thread actually running again -- large rdy = scheduler latency, small
         * rdy with a large act-vs-req overshoot = the wake itself came late. */
        {
            uint64_t wnow  = perf_now_ns();
            uint64_t actns = wnow - caller->fx_park_ns;
            uint64_t rdyms = caller->fx_wake_ns
                             ? (wnow - caller->fx_wake_ns) / 1000000ull : 0;
            if (deadline_ns) {
                uint64_t reqns = (deadline_ns > caller->fx_park_ns)
                                 ? deadline_ns - caller->fx_park_ns : 0;
                if (actns > reqns + 300000000ull) {
                    static int fl = 0;
                    if (fl < 64) { fl++;
                        kprintf("[fxlate] pid=%d addr=0x%lx req=%lums act=%lums "
                                "rdy=%lums to=%d\n", caller->pid,
                                (unsigned long)addr,
                                (unsigned long)(reqns / 1000000ull),
                                (unsigned long)(actns / 1000000ull),
                                (unsigned long)rdyms,
                                caller->futex_timed_out ? 1 : 0);
                    }
                }
            } else if (actns > 15000000000ull) {
                /* Untimed parks lasting >15s are normal for idle workers; log
                 * the WAKE so the event that ENDS a silence window is named. */
                static int fw = 0;
                if (fw < 64) { fw++;
                    kprintf("[fxwake] pid=%d addr=0x%lx untimed woke after %lums "
                            "rdy=%lums\n", caller->pid, (unsigned long)addr,
                            (unsigned long)(actns / 1000000ull),
                            (unsigned long)rdyms);
                }
            }
        }
#endif
        caller->futex_deadline_ns = 0;
        if (caller->futex_timed_out) { caller->futex_timed_out = false; return -110; }
        return 0;          /* woken; glibc re-checks its condition + own clock */
    }

    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        uint64_t flags = spin_lock_irqsave(&g_futex_lock);

        uint32_t idx = futex_hash(cr3, addr);
        struct futex_entry *e = g_futex_hash[idx];
        while (e) {
            if (e->key_cr3 == cr3 && e->key_addr == addr) break;
            e = e->next;
        }
        if (!e) {
            spin_unlock_irqrestore(&g_futex_lock, flags);
            return 0;
        }

        /* Wake up to `val` waiters */
        int woken = 0;
        while (e->waiters && (uint32_t)woken < val) {
            struct proc *w = e->waiters;
            e->waiters = w->next_wait;
            w->next_wait = 0;
            w->futex_deadline_ns = 0;     /* woken by wake, not timeout */
#ifdef CHROMIUM_BOOT
            w->fx_wake_ns = perf_now_ns();
#endif
            w->state = PROC_READY;
            sched_enqueue(w);
            woken++;
        }

        /* If no more waiters, free the entry */
        if (!e->waiters) futex_free_entry(e);

        spin_unlock_irqrestore(&g_futex_lock, flags);
        return woken;
    }

    /* FUTEX_REQUEUE(3) / FUTEX_CMP_REQUEUE(4): glibc's pthread_cond_broadcast
     * requeues waiters from the condvar word onto the mutex word. Returning
     * -EINVAL (as this did until slice 59e) means a broadcast wakes NOBODY --
     * every waiter parks forever on an INFINITE futex. Measured exactly that:
     * the renderer's MAIN thread sat in futex(0xc130ac0, to=-1) for 8s and
     * climbing while userspace was ~0.06% busy, so the page stopped building
     * without any CPU cost. The bring-up-era note claiming chrome never uses
     * these ops was taken from a trace of a much simpler page.
     *
     * Implementation: WAKE EVERY waiter on uaddr rather than moving them to
     * uaddr2. Spurious wakeups are explicitly legal for futexes -- glibc
     * re-checks its predicate and re-blocks on the mutex itself -- so this is
     * correct, just less efficient than a true requeue (a thundering herd on
     * broadcast). Correctness first; optimise only if it ever shows up. */
    if (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE) {
        uint64_t flags = spin_lock_irqsave(&g_futex_lock);
        uint32_t idx = futex_hash(cr3, addr);
        struct futex_entry *e = g_futex_hash[idx];
        while (e) {
            if (e->key_cr3 == cr3 && e->key_addr == addr) break;
            e = e->next;
        }
        int woken = 0;
        if (e) {
            while (e->waiters) {
                struct proc *w = e->waiters;
                e->waiters = w->next_wait;
                w->next_wait = 0;
                w->futex_deadline_ns = 0;
#ifdef CHROMIUM_BOOT
                w->fx_wake_ns = perf_now_ns();
#endif
                w->state = PROC_READY;
                sched_enqueue(w);
                woken++;
            }
            futex_free_entry(e);
        }
        spin_unlock_irqrestore(&g_futex_lock, flags);
#ifdef CHROMIUM_BOOT
        { static int rq; if (rq < 24) { rq++;
            kprintf("[fxrq] pid=%d REQUEUE op=%d addr=0x%lx -> woke %d\n",
                    caller->pid, cmd, (unsigned long)addr, woken); } }
#endif
        return woken;
    }

#ifdef CHROMIUM_BOOT
    /* Any op we still do not implement: SAY SO. Silent -EINVAL is how the
     * above went unnoticed for the whole arc. */
    { static int un; if (un < 24) { un++;
        kprintf("[fxop] UNSUPPORTED futex op=%d (raw 0x%x) pid=%d addr=0x%lx\n",
                cmd, op, caller->pid, (unsigned long)addr); } }
#endif
    return -22; /* -EINVAL */
}

/* Remove `p` from any futex wait list it is parked on. MUST be called when a
 * thread is terminated/reaped: a thread can exit while blocked on a futex (now
 * common, since timed waits block on the list too), and a later FUTEX_WAKE or
 * the timeout sweep would otherwise wake a DEAD proc -> enqueue it -> the
 * scheduler switches into a freed address space. Mirrors sched_dequeue. */
void futex_forget_proc(struct proc *p) {
    if (!p) return;
    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    for (int b = 0; b < FUTEX_HASH_SIZE; b++) {
        struct futex_entry *e = g_futex_hash[b];
        while (e) {
            struct futex_entry *enext = e->next;
            struct proc **pp = &e->waiters;
            while (*pp) {
                if (*pp == p) { *pp = p->next_wait; p->next_wait = 0; }
                else          pp = &(*pp)->next_wait;
            }
            if (!e->waiters) futex_free_entry(e);
            e = enext;
        }
    }
    p->futex_deadline_ns = 0;
    spin_unlock_irqrestore(&g_futex_lock, flags);
}

#ifdef CHROMIUM_BOOT
/* Slice 56 tick-liveness counters, dumped by waitt_dump()'s [tick] line: how
 * often the futex timeout sweep actually RAN (it has no driver of its own --
 * only sched_yield's BSP slow path calls it, which nothing takes when every
 * ready queue is empty), how many waiters it expired, how often poll_tick
 * re-woke parked pollers, and whether any futex wait ever asked for a
 * CLOCK_REALTIME absolute deadline (which we compare against monotonic). */
uint64_t g_fx_sweeps, g_fx_sweep_woken, g_poll_tick_wakes, g_fx_rt_flag_count;
uint64_t g_poll_event_wakes;    /* slice 67: event-driven poller wakeups */
#endif

/* Wake any list-registered futex waiter whose deadline has passed. Called
 * periodically (rate-limited) from the scheduler tick so a TIMED FUTEX_WAIT that
 * is never FUTEX_WAKEd still returns -ETIMEDOUT. Cheap: <=256 entries, low rate. */
void futex_expire_timeouts(void) {
    uint64_t now = perf_now_ns();
#ifdef CHROMIUM_BOOT
    g_fx_sweeps++;
#endif
    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    for (int b = 0; b < FUTEX_HASH_SIZE; b++) {
        struct futex_entry *e = g_futex_hash[b];
        while (e) {
            struct futex_entry *enext = e->next;
            struct proc **pp = &e->waiters;
            while (*pp) {
                struct proc *w = *pp;
                if (w->futex_deadline_ns && now >= w->futex_deadline_ns) {
                    *pp = w->next_wait;          /* unlink */
                    w->next_wait = 0;
                    w->futex_deadline_ns = 0;
                    w->futex_timed_out = true;
#ifdef CHROMIUM_BOOT
                    w->fx_wake_ns = now;
                    g_fx_sweep_woken++;
#endif
                    w->state = PROC_READY;
                    sched_enqueue(w);
                } else {
                    pp = &w->next_wait;
                }
            }
            if (!e->waiters) futex_free_entry(e);
            e = enext;
        }
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);
}

/* ---- Blocking wait for poll / select / epoll_wait (slice 43) ---------
 *
 * These used to BUSY-WAIT: `scan fds; sched_yield(); repeat`. That keeps an
 * idle waiter PROC_RUNNING and on the ready queue, so under a heavy
 * multi-threaded load (chrome under WHPX: ~20 idle worker threads each
 * spinning) the few threads doing real work get only ~1/N of the CPU and the
 * whole user plane crawls -- YouTube starved, pollit climbed to 632M (ledger
 * slice 42).
 *
 * Now an idle waiter BLOCKS via poll_wait_block() -- it leaves the ready queue
 * -- and a rate-limited poll_tick() requeues every blocked poller to re-scan.
 * Each poller keeps its own deadline as a syscall local and re-checks it on the
 * re-scan, so NO per-proc deadline field is needed. poll_tick() is driven from
 * places that run even when every user thread is blocked (sched_yield's slow
 * path under load; pid 0's idle_loop when idle), so a waiter is always
 * re-scanned within the tick interval -- catching fd readiness (the re-scan
 * itself pumps net_poll for sockets), a passed deadline, or a pending signal.
 * Coarse (wake-all + re-scan) but correct, and it drops the spin rate ~1000x.
 * Same block/wake/teardown pattern as the futex path above. */
static struct proc *g_poll_waiters;      /* BKL-protected; linked via ->next_wait */

void poll_init(void) { g_poll_waiters = 0; }

/* Block the caller until poll_wake_all() requeues it; it re-scans on return.
 * Mirrors pipe.c's wq_add + sched_yield: BKL-serialised (no IRQ handler touches
 * this list, and the BKL serialises CPUs), with wait_head set so signal_send()
 * can splice us out if a signal force-wakes us before poll_wake_all() does --
 * without that back-pointer a signalled poller would dangle on the list and a
 * later re-add would splice it into a cycle. */
void poll_wait_block(void) {
    struct proc *caller = current_proc();
    if (!caller) { sched_yield(); return; }
    caller->next_wait = g_poll_waiters;
    caller->wait_head = &g_poll_waiters;
    g_poll_waiters    = caller;
    caller->state     = PROC_BLOCKED;
    sched_yield();                 /* woken by poll_wake_all (tick/event) */
    /* On return we've been spliced out (by poll_wake_all or signal_send). */
}

/* Requeue every blocked poller so it re-scans. BKL-held by every caller
 * (poll_tick's sites), so no extra lock is needed -- same convention as
 * pipe.c/socket.c wq_wake_all. The CURRENT proc is left parked: poll_tick runs
 * inside a blocking poller's own sched_yield, and waking it here would make it
 * re-scan immediately instead of sleeping (a non-current thread that wrote an
 * eventfd/pipe is never itself on this list, so skipping current is a no-op
 * for any future event-hook caller). */
void poll_wake_all(void) {
    struct proc *cur = current_proc();
    struct proc *w = g_poll_waiters;
    g_poll_waiters = 0;
    while (w) {
        struct proc *nx = w->next_wait;
        if (w == cur) {                       /* keep the yielding caller parked */
            w->next_wait = g_poll_waiters;
            g_poll_waiters = w;
            w = nx;
            continue;
        }
        w->next_wait = 0;
        w->wait_head = 0;
        if (w->state == PROC_BLOCKED) { w->state = PROC_READY; sched_enqueue(w); }
        w = nx;
    }
}

/* ---- Slice 67 (perf tier 2, final step): EVENT-DRIVEN poll wakeups ----
 *
 * MEASURED (slice 66): epoll_wait held the BKL ~0.77 ms PER CALL --
 * ~48,800 Mcyc across ~14,785 calls per 60s -- which is far more than one
 * scan of <=64 fds can cost. The reason was structural: every parked
 * poller was woken every ~1 ms by poll_tick REGARDLESS of whether any
 * watched fd had become ready, and each one then re-scanned all its fds
 * under the BKL. N pollers x M fds x 1000 times a second, almost all of it
 * futile -- a thundering herd on a timer.
 *
 * Now the producers say when something happened: poll_event_notify() is
 * called from the paths that actually make an fd readable/writable (pipe
 * data + closes, socket receive enqueues + peer shutdown), and poll_tick
 * degrades to a slow SAFETY NET for any readiness source not yet hooked
 * (a missed hook costs latency, bounded by the net's period -- never a
 * lost wakeup). Notifies are coalesced at 100us so a burst of arriving
 * packets cannot turn into a wake storm of its own.
 * Callers MUST hold the BKL (same convention as poll_wake_all). */
void poll_event_notify(void) {
    static uint64_t last_ns;
    if (!g_poll_waiters) return;              /* nobody parked: free */
    uint64_t now = perf_now_ns();
    if (now - last_ns < 100000ull) return;    /* coalesce bursts (100us) */
    last_ns = now;
#ifdef CHROMIUM_BOOT
    g_poll_event_wakes++;
#endif
    poll_wake_all();
}

/* Fallback sweep: covers readiness sources with no poll_event_notify hook
 * yet (and timeouts). Driven from the scheduler yield slow path and pid 0's
 * idle loop. Slice 67: 1 ms -> 20 ms now that real events wake pollers
 * directly -- this is the belt, not the braces. Callers MUST hold the BKL. */
void poll_tick(void) {
    static uint64_t last_ns;
    if (!g_poll_waiters) return;
    uint64_t now = perf_now_ns();
    if (now - last_ns < 20000000ull) return;  /* >= ~20 ms since last sweep */
    last_ns = now;
#ifdef CHROMIUM_BOOT
    g_poll_tick_wakes++;
#endif
    poll_wake_all();
}

/* Teardown: unlink a dying thread from the poll wait list (mirror of
 * futex_forget_proc); BKL-held at both proc.c call sites. */
void poll_forget_proc(struct proc *p) {
    if (!p) return;
    struct proc **pp = &g_poll_waiters;
    while (*pp) {
        if (*pp == p) { *pp = p->next_wait; p->next_wait = 0; p->wait_head = 0; }
        else          pp = &(*pp)->next_wait;
    }
}
