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
    t->detached    = false;
    t->tls_base    = tls_base;
    t->join_waiters = 0;

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

long futex(uint32_t *uaddr, int op, uint32_t val) {
    struct proc *caller = current_proc();
    if (!caller) return -1;

    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    uint64_t cr3  = caller->cr3;
    if (!user_range_ok(addr, sizeof(uint32_t))) return -14; /* EFAULT */

    /* Pre-touch the futex word OUTSIDE the spinlock so any CoW/demand #PF
     * resolves with IRQs on; the locked re-read below then can't fault
     * (per-copy uaccess: each read opens its own stac window). */
    uint32_t cur_val;
    if (copy_from_user(&cur_val, uaddr, sizeof(cur_val)) != 0) return -14;

    if (op == FUTEX_WAIT) {
        /* Atomically: if *uaddr == val, block. Otherwise return -EAGAIN. */
        uint64_t flags = spin_lock_irqsave(&g_futex_lock);

        /* Re-read the user value under the lock -- we're in the caller's
         * address space and the page is present (pre-touched above). */
        unsigned long uw = uaccess_begin();
        cur_val = *(volatile uint32_t *)uaddr;
        uaccess_end(uw);
        if (cur_val != val) {
            spin_unlock_irqrestore(&g_futex_lock, flags);
            return -11; /* -EAGAIN */
        }

        struct futex_entry *e = futex_find_or_create(cr3, addr);
        if (!e) {
            spin_unlock_irqrestore(&g_futex_lock, flags);
            return -12; /* -ENOMEM */
        }

        /* Add caller to wait list */
        caller->state = PROC_BLOCKED;
        caller->next_wait = e->waiters;
        e->waiters = caller;

        spin_unlock_irqrestore(&g_futex_lock, flags);
        sched_yield();

        return 0;
    }

    if (op == FUTEX_WAKE) {
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
            w->state = PROC_READY;
            sched_enqueue(w);
            woken++;
        }

        /* If no more waiters, free the entry */
        if (!e->waiters) futex_free_entry(e);

        spin_unlock_irqrestore(&g_futex_lock, flags);
        return woken;
    }

    return -22; /* -EINVAL */
}
