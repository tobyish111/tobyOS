/* fork.c -- POSIX fork/exec/waitpid (Phase 3 M3.2).
 *
 * sys_fork()   : clone the current process (full page copy for now).
 * sys_execve() : replace the current process image with a new ELF.
 *
 * waitpid already lives in syscall.c (sys_waitpid) via proc_wait.
 * This file adds the two new entry points and wires them into the
 * process model established by proc.c.
 */

#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/heap.h>
#include <tobyos/vfs.h>
#include <tobyos/elf.h>
#include <tobyos/pe.h>
#include <tobyos/tss.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/file.h>
#include <tobyos/cap.h>
#include <tobyos/perf.h>
#include <tobyos/signal.h>
#include <tobyos/abi/abi.h>
#include <tobyos/page_fault.h>
#include <tobyos/uaccess.h>
#include <tobyos/mmap.h>
#include <tobyos/rng.h>
#include <tobyos/nsproxy.h>   /* nsproxy_fork_inherit (slice 8) */
#include <tobyos/seccomp.h>   /* seccomp_fork_inherit (slice 13) */
#include <tobyos/cgroup.h>    /* cgroup_can_fork (slice 15) */

extern struct proc g_proc[];

/* User-half extent: PML4 entries 0..255 cover user addresses. */
#define USER_PML4_ENTRIES  256

/* Page table entry flag masks (must match vmm.c internals). */
#define PTE_P    (1ULL << 0)
#define PTE_RW   (1ULL << 1)
#define PTE_US   (1ULL << 2)
#define PTE_PS   (1ULL << 7)   /* 2 MiB huge page */
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Convert a physical frame address to its HHDM kernel virtual. */
static inline void *phys_to_virt(uint64_t phys) {
    return pmm_phys_to_virt(phys);
}

typedef uint64_t pte_t;

/* ---- Deep-copy the user-half page tables into a fresh PML4 --------
 *
 * Walks the parent's PML4 entries 0..255, and for every present leaf
 * page (4 KiB data frame) allocates a new physical frame, copies 4 KiB,
 * and maps it at the same virtual address in the child's PML4. The
 * intermediate paging structures (PDPT/PD/PT) are freshly allocated too.
 *
 * This is intentionally a FULL copy (no COW) for simplicity. COW can
 * be layered via mmap_cow_clone() in a future milestone.
 */
static bool copy_user_pages(uint64_t parent_pml4_phys,
                            uint64_t child_pml4_phys) {
    pte_t *parent_pml4 = (pte_t *)phys_to_virt(parent_pml4_phys);
    pte_t *child_pml4  = (pte_t *)phys_to_virt(child_pml4_phys);

    for (int i4 = 0; i4 < USER_PML4_ENTRIES; i4++) {
        if (!(parent_pml4[i4] & PTE_P)) continue;

        uint64_t pdpt_phys = parent_pml4[i4] & PTE_ADDR_MASK;
        pte_t *parent_pdpt = (pte_t *)phys_to_virt(pdpt_phys);

        /* Alloc child PDPT. */
        uint64_t c_pdpt_phys = pmm_alloc_page();
        if (!c_pdpt_phys) return false;
        pte_t *child_pdpt = (pte_t *)phys_to_virt(c_pdpt_phys);
        memset(child_pdpt, 0, PAGE_SIZE);
        child_pml4[i4] = c_pdpt_phys | (parent_pml4[i4] & ~PTE_ADDR_MASK);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(parent_pdpt[i3] & PTE_P)) continue;

            uint64_t pd_phys = parent_pdpt[i3] & PTE_ADDR_MASK;
            pte_t *parent_pd = (pte_t *)phys_to_virt(pd_phys);

            uint64_t c_pd_phys = pmm_alloc_page();
            if (!c_pd_phys) return false;
            pte_t *child_pd = (pte_t *)phys_to_virt(c_pd_phys);
            memset(child_pd, 0, PAGE_SIZE);
            child_pdpt[i3] = c_pd_phys | (parent_pdpt[i3] & ~PTE_ADDR_MASK);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(parent_pd[i2] & PTE_P)) continue;
                if (parent_pd[i2] & PTE_PS) {
                    /* 2 MiB huge page -- skip for now, user-half shouldn't
                     * normally use these. */
                    continue;
                }

                uint64_t pt_phys = parent_pd[i2] & PTE_ADDR_MASK;
                pte_t *parent_pt = (pte_t *)phys_to_virt(pt_phys);

                uint64_t c_pt_phys = pmm_alloc_page();
                if (!c_pt_phys) return false;
                pte_t *child_pt = (pte_t *)phys_to_virt(c_pt_phys);
                memset(child_pt, 0, PAGE_SIZE);
                child_pd[i2] = c_pt_phys | (parent_pd[i2] & ~PTE_ADDR_MASK);

                for (int i1 = 0; i1 < 512; i1++) {
                    if (!(parent_pt[i1] & PTE_P)) continue;

                    uint64_t data_phys = parent_pt[i1] & PTE_ADDR_MASK;
                    uint64_t c_data = pmm_alloc_page();
                    if (!c_data) return false;

                    memcpy(phys_to_virt(c_data),
                           phys_to_virt(data_phys),
                           PAGE_SIZE);
                    child_pt[i1] = c_data | (parent_pt[i1] & ~PTE_ADDR_MASK);
                }
            }
        }
    }
    return true;
}

/* ---- Kernel-stack layout for the child ----------------------------
 *
 * We build the same fake frame that build_kstack() in proc.c does so
 * the first context_switch into the child lands at fork_child_entry,
 * which descends to ring 3 through the regular syscall unwind. */

extern __attribute__((noreturn)) void proc_enter_user_asm(uint64_t rip,
                                                          uint64_t rsp);
extern __attribute__((noreturn)) void fork_child_return_asm(uint64_t frame_rsp);

/* The child's first-ever entry after context switch. The child's kstack
 * top holds a verbatim copy of the parent's saved syscall register block
 * (made in sys_fork below), so replaying the syscall unwind on it resumes
 * the child at the instruction after the parent's `syscall`, with every
 * user register (incl. callee-saved rbx/rbp/r12-r15, which the compiler
 * assumes survive the libc fork() call) restored and rax = 0.
 *
 * This replaced a long-standing bug: the original implementation jumped to
 * p->user_entry/user_rsp, which are the SPAWN-TIME ELF entry + initial
 * stack (never updated after exec) -- so every fork child re-ran the
 * program from _start over a CoW copy of the parent's memory instead of
 * resuming at the fork point. Unnoticed because nothing in-tree forked
 * and checked resume semantics until the SA_RESTART sigtest did. */
static __attribute__((noreturn)) void fork_child_entry(void) {
    struct proc *p = current_proc();
    /* Slice 39: first entry via proc_context_switch bypasses do_switch's
     * post-switch line -- release the proc this CPU switched away from
     * (see sched_finish_switch), or it stays on_cpu/unschedulable. */
    sched_finish_switch();
    /* Load the child's FPU/SSE state (copied from the parent at fork time)
     * -- the switch that landed us here restored the previous proc's state. */
    fpu_restore(p->fpu_state);
    fork_child_return_asm((uint64_t)p->kstack_top
                          - sizeof(struct syscall_regs));
}

static bool build_fork_kstack(struct proc *child) {
    void *base = kmalloc(PROC_KSTACK_SZ);
    if (!base) return false;
    memset(base, 0, PROC_KSTACK_SZ);
    child->kstack_base = base;
    child->kstack_top  = (uint8_t *)base + PROC_KSTACK_SZ;

    /* The top sizeof(struct syscall_regs) bytes are reserved for the copy
     * of the parent's saved register block (sys_fork fills it in right
     * after us); fork_child_entry replays the syscall unwind on it. Build
     * the context-switch frame BELOW that so the first switch into the
     * child doesn't clobber the resume frame. */
    uint64_t *sp = (uint64_t *)((uint8_t *)child->kstack_top
                                - sizeof(struct syscall_regs));
    *--sp = 0;                                      /* alignment pad */
    *--sp = (uint64_t)fork_child_entry;             /* RIP for ret */
    *--sp = 0;                                      /* r15 */
    *--sp = 0;                                      /* r14 */
    *--sp = 0;                                      /* r13 */
    *--sp = 0;                                      /* r12 */
    *--sp = 0;                                      /* rbx */
    *--sp = 0;                                      /* rbp */
    *--sp = 0x202ULL;                               /* RFLAGS (IF=1) */
    child->saved_rsp = (uint64_t)sp;
    return true;
}

/* Slice 88: soft stop-the-world for multithreaded CoW fork.
 * Sibling threads sharing actor->cr3 keep WRITABLE TLBs while vmm_cow_fork
 * walks PTEs; park them off-CPU first, shootdown after WP, then resume.
 *
 * Concurrent forks in the same (or any) process must not interleave STWs:
 * each would wait for the other to leave PROC_RUNNING forever. Serialize
 * with g_cow_fork_lock; a forker that is itself the target of another's
 * vm_quiesce parks cooperatively instead of spinning on the lock. */
static volatile int g_cow_fork_lock;

static void cow_fork_lock_acquire(struct proc *actor) {
    bool had_bkl = bkl_held();
    if (had_bkl) bkl_exit();
    for (uint32_t spins = 0; ; spins++) {
        if (__atomic_load_n(&actor->vm_quiesce, __ATOMIC_ACQUIRE)) {
            /* Another STW wants us off RUNNING -- stay BLOCKED until it
             * resumes us. Do NOT bounce back to RUNNING while still
             * contending for the lock (that deadlocks the waiter's
             * !PROC_RUNNING loop). */
            actor->state = PROC_BLOCKED;
            actor->vm_quiesced = 1;
            while (__atomic_load_n(&actor->vm_quiesce, __ATOMIC_ACQUIRE))
                __asm__ volatile("pause");
        }
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_cow_fork_lock, &expected, 1,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            break;
        /* A forker killed mid-STW can leave the lock stuck at 1 forever.
         * After a long spin, steal it so chrome can make progress. */
        if (spins > 20000000u) {
#ifdef CHROMIUM_BOOT
            kprintf("[fork] cow_fork_lock steal pid=%d (was stuck)\n",
                    actor ? actor->pid : -1);
#endif
            __atomic_store_n(&g_cow_fork_lock, 1, __ATOMIC_RELEASE);
            break;
        }
        __asm__ volatile("pause");
    }
    if (actor->state == PROC_BLOCKED && actor->vm_quiesced &&
        !__atomic_load_n(&actor->vm_quiesce, __ATOMIC_ACQUIRE)) {
        /* We parked for a peer STW that has since resumed; we hold the
         * fork lock now so we are the STW actor — run as RUNNING. */
        actor->vm_quiesced = 0;
    }
    actor->state = PROC_RUNNING;
    if (had_bkl) bkl_enter();
}

static void cow_fork_lock_release(void) {
    __atomic_store_n(&g_cow_fork_lock, 0, __ATOMIC_RELEASE);
}

/* Slice 92: park an ON-CPU proc that observed vm_quiesce from a kernel-mode
 * #PF (the CoW-fork WP sweep is mid-flight). This is the ONE park point that
 * used to spin while HOLDING the BKL: a sibling mid-syscall touches user
 * memory (copy_to_user out-params under the SMAP stac window), the sweep has
 * just write-protected that page, and the resulting kernel #PF parked here
 * with the BKL still held. The forker meanwhile re-takes the BKL for
 * mmap_cow_clone BEFORE tg_vm_resume -- a hard cycle on a FIFO ticket lock,
 * and every other CPU's syscall entry queues behind it. That is the whole
 * -smp 4 freeze: READY threads never picked, [bkl] acq=0 on all CPUs,
 * [qstuck] blind because the spinner is on_cpu with vm_quiesce legitimately
 * set. Drop the BKL for the wait like every other park point does.
 *
 * IRQs are forced on so TLB-shootdown IPIs still ACK. The wait is not
 * broken on a timeout: proceeding mid-sweep is the PA-freelist corruption
 * documented in apic.c, and with the BKL dropped the blast radius of an
 * orphaned flag is one thread, not the kernel. We do REPORT a long park --
 * if [qpark] ever prints, the resume side has a new hole. */
uint64_t g_qpark_engaged;      /* total on-CPU quiesce parks (kernel #PF) */
uint64_t g_qpark_bkl;          /* ... of which arrived HOLDING the BKL */

void vm_quiesce_park_oncpu(struct proc *p) {
    if (!p || !__atomic_load_n(&p->vm_quiesce, __ATOMIC_ACQUIRE)) return;
    uint64_t rf;
    __asm__ volatile("pushfq; pop %0" : "=r"(rf));
    __asm__ volatile("sti");
    bool had_bkl = bkl_held();
    __atomic_fetch_add(&g_qpark_engaged, 1, __ATOMIC_RELAXED);
    if (had_bkl) __atomic_fetch_add(&g_qpark_bkl, 1, __ATOMIC_RELAXED);
#ifdef CHROMIUM_BOOT
    /* First few engagements name themselves; the 60s deep dump prints the
     * running totals ([qpark] engaged=/bkl=). If bkl>0 ever coincides with
     * a freeze again, this park is back in the story. */
    { static int qp; if (qp < 8) { qp++;
        kprintf("[qpark] pid=%d parks on vm_quiesce (bkl=%d) #%lu\n",
                p->pid, (int)had_bkl, (unsigned long)g_qpark_engaged); } }
#endif
    if (had_bkl) bkl_exit();
    p->state = PROC_BLOCKED;
    p->vm_quiesced = 1;
    uint64_t warn_at = 0;
    uint32_t spins = 0;
    while (__atomic_load_n(&p->vm_quiesce, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
        if ((++spins & 0xffffu) == 0) {
            uint64_t now = perf_now_ns();
            if (!warn_at) {
                warn_at = now + 30000000000ull;      /* 30 s: sweep is long over */
            } else if (now > warn_at) {
                warn_at = now + 30000000000ull;
                kprintf("[qpark] pid=%d STILL parked on vm_quiesce "
                        "(had_bkl=%d) -- resume never came\n",
                        p->pid, (int)had_bkl);
            }
        }
    }
    /* Self-resumed on-CPU: clear vm_quiesced ourselves. tg_vm_resume saw us
     * on_cpu and deferred to sched_finish_switch, which will never run for
     * us (we never switched out) -- a stale flag here would later make
     * sched_finish_switch / [qstuck] requeue us out of an unrelated BLOCKED
     * state (e.g. a futex wait). */
    p->state = PROC_RUNNING;
    p->vm_quiesced = 0;
    if (had_bkl) bkl_enter();
    if (!(rf & (1ull << 9))) __asm__ volatile("cli");
}

void tg_vm_quiesce(struct proc *actor) {
    if (!actor) return;
#ifdef CHROMIUM_BOOT
    kprintf("[fork] quiesce enter pid=%d\n", actor->pid);
#endif
    cow_fork_lock_acquire(actor);
#ifdef CHROMIUM_BOOT
    kprintf("[fork] quiesce locked pid=%d\n", actor->pid);
#endif

    uint64_t cr3 = actor->cr3;
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *q = &g_proc[i];
        if (q == actor || q->state == PROC_UNUSED ||
            q->state == PROC_EMBRYO ||
            q->state == PROC_TERMINATED) continue;
        if (q->cr3 != cr3) continue;
        __atomic_store_n(&q->vm_quiesce, 1, __ATOMIC_RELEASE);
        q->quantum_left = 0;
        if (q->state == PROC_READY) {
            sched_dequeue(q);
            q->state = PROC_BLOCKED;
            q->vm_quiesced = 1;
        } else if (q->state == PROC_RUNNING) {
            q->vm_quiesced = 1;   /* tick / syscall-return will park */
        }
    }

    /* Siblings stuck in bkl_enter need the lock to reach a park point. */
    bool had_bkl = bkl_held();
    if (had_bkl) bkl_exit();

    /* Wait until siblings have left USER mode. Do NOT wait on on_cpu: a
     * sibling mid-#PF / shootdown spins in-kernel with IRQs off, state
     * often still PROC_RUNNING, and MUST stay on_cpu to ACK IPIs.
     * Waiting for !RUNNING deadlocks those paths (observed: clone stuck
     * 100s+ with a peer in shootdown wait). Bound the wait so LAPIC ticks
     * can park user-mode siblings, then proceed — kernel siblings cannot
     * retire user stores until they iret, and syscall/#PF return parks. */
    for (uint32_t spins = 0; ; spins++) {
        bool user_busy = false;
        int running_on_cpu = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            struct proc *q = &g_proc[i];
            if (q == actor || q->state == PROC_UNUSED ||
                q->state == PROC_EMBRYO ||
                q->state == PROC_TERMINATED) continue;
            if (q->cr3 != cr3) continue;
            if (q->state == PROC_READY) {
                sched_dequeue(q);
                q->state = PROC_BLOCKED;
                q->vm_quiesced = 1;
            }
            if (q->state == PROC_RUNNING) {
                if (!__atomic_load_n(&q->on_cpu, __ATOMIC_ACQUIRE)) {
                    q->state = PROC_BLOCKED;
                    q->vm_quiesced = 1;
                } else {
                    running_on_cpu++;
                    user_busy = true;
                }
            }
        }
        if (!user_busy) break;
        /* ~5e5 pauses: park user threads via LAPIC tick, then proceed.
         * Leftover on_cpu RUNNING are assumed in-kernel (IRQ-off). */
        if (spins > 500000u) {
#ifdef CHROMIUM_BOOT
            static int qw;
            if (qw < 16) {
                qw++;
                kprintf("[fork] quiesce timeout: %d on_cpu RUNNING "
                        "(assuming in-kernel; proceeding)\n", running_on_cpu);
            }
#endif
            break;
        }
        /* UP: pause alone never lets READY siblings reach a park point —
         * periodically yield so they can observe vm_quiesce and BLOCK. */
        if ((spins & 0xfffu) == 0)
            sched_yield();
        else
            __asm__ volatile("pause");
    }

#ifdef CHROMIUM_BOOT
    kprintf("[fork] quiesce ready pid=%d\n", actor->pid);
#endif
    /* Do NOT re-take the BKL here. A sibling we timed out as "in-kernel"
     * may still hold it; re-entering deadlocks (clone stuck 50s+ after
     * "quiesce ready"). vmm_cow_fork runs without the BKL under STW;
     * sys_fork re-takes only around mmap_cow_clone. */
    (void)had_bkl;
}

void tg_vm_resume(struct proc *actor) {
    if (!actor) return;
    uint64_t cr3 = actor->cr3;
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *q = &g_proc[i];
        if (q == actor || q->state == PROC_UNUSED ||
            q->state == PROC_EMBRYO) continue;
        if (q->cr3 != cr3) continue;
        __atomic_store_n(&q->vm_quiesce, 0, __ATOMIC_RELEASE);
        if (!q->vm_quiesced) continue;
        /* Slice 89: SEQ_CST fence between the vm_quiesce store and the
         * on_cpu load -- pairs with sched_finish_switch's {on_cpu:=0;
         * fence; load vm_quiesce}. Without it both sides can read stale
         * values on x86 (StoreLoad) and the parked thread is never
         * requeued by EITHER path. See the matching comment in sched.c. */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        /* Re-enqueue only when off-CPU. If still on_cpu (mid-yield / #PF
         * spin), leave vm_quiesced=1: sched_finish_switch requeues when
         * on_cpu drops. Clearing vm_quiesced here without enqueue LOST
         * threads; enqueue while on_cpu dual-scheduled them (tlb wedge). */
        if (q->state == PROC_BLOCKED &&
            !__atomic_load_n(&q->on_cpu, __ATOMIC_ACQUIRE)) {
            q->vm_quiesced = 0;
            q->state = PROC_READY;
            sched_enqueue(q);
        }
    }
    cow_fork_lock_release();
}

/* ===================================================================
 * sys_fork -- Create a child process that is a copy of the parent.
 *
 * Returns child PID to the parent, 0 to the child.
 * =================================================================== */
long sys_fork(void) {
    struct proc *parent = current_proc();
    if (!parent || parent->pid == 0) return -ABI_EINVAL;

    /* Claim a free slot ATOMICALLY (state CAS UNUSED -> EMBRYO). This build
     * runs for hundreds of ms across cow_fork; with the slot left UNUSED a
     * concurrent sys_clone_thread claimed the SAME slot and both built in it
     * (the mp bootstrap flake -- see the slice 109 ledger entry). */
    /* Slice 15: the pids controller. Checked BEFORE claiming a slot so a denied
     * fork costs nothing and cannot leave a PROC_EMBRYO behind -- which, per the
     * slice-109 note directly above, is the state that must never leak. */
    { long cgrc = cgroup_can_fork(parent);
      if (cgrc != 0) return cgrc; }
    struct proc *child = proc_slot_claim();
    if (!child) {
#ifdef CHROMIUM_BOOT
        /* Slice 76: fork FAILURES were never logged, only successes -- and
         * crashpad's double-fork (first child forks the grandchild that
         * execs chrome_crashpad_handler, then _exit(0)s) shows only ONE fork
         * in our traces. If the second one is failing here, this names it. */
        { int used = 0;
          for (int i = 0; i < PROC_MAX; i++)
              if (g_proc[i].state != PROC_UNUSED) used++;
          kprintf("[fork] FAILED pid=%d: no free proc slot (%d/%d used)\n",
                  parent->pid, used, PROC_MAX); }
#endif
        return -ABI_ENOMEM;
    }

    /* Copy parent's proc struct as a starting point. */
    int child_pid = (int)(child - g_proc);
    memcpy(child, parent, sizeof(*child));

    child->pid       = child_pid;
    child->ppid      = parent->pid;
    child->state     = PROC_EMBRYO;   /* memcpy copied parent's state */
    child->wait_pid  = -1;
    child->exit_code = -1;
    child->next_ready = NULL;
    /* Slice 39: the memcpy above copied the parent's LIVE scheduler linkage
     * -- on_cpu is 1 (the parent is running RIGHT NOW) and on_rq may be set.
     * A child born with on_cpu=1 is skipped by every picker FOREVER (nothing
     * ever clears it: the prev_proc handoff only fires for procs that were
     * actually switched out), and stale on_rq makes sched_enqueue a no-op.
     * Observed: fork returned but the child never executed one instruction. */
    child->on_rq  = false;
    child->on_cpu = 0;
    child->vm_quiesce  = 0;
    child->vm_quiesced = 0;
    child->mm_owner    = 0;
    child->vfork_parent = 0;
    child->vfork_child  = 0;
    /* The clone(2) STAGING fields are per-call scratch, and the memcpy above
     * copied them. A child that keeps them applies them to ITS next fork:
     * clone_child_stack would point the grandchild's RSP at an address that
     * means nothing after an execve, and clone_ns_flags would silently build
     * namespaces nobody asked for.
     *
     * Found by the slice-16 capstone, not by reasoning: the container's first
     * ordinary fork() faulted at cr2 = rsp-8 with rsp = 0x39c130, which is the
     * top-minus-16 of the OCI runtime's OWN clone stack -- a BSS address from a
     * program that had already been replaced by execve. The staging worked; it
     * just did not stop working. */
    child->clone_ns_flags   = 0;
    child->clone_child_stack = 0;
    child->next_wait  = NULL;
    child->wait_head  = NULL;
    child->join_waiters = NULL;
    child->tgid      = child_pid;
    child->is_thread = false;
    child->detached  = false;
    child->created_ns     = perf_now_ns();
    child->cpu_ns         = 0;
    child->syscall_count  = 0;
    child->last_switch_tsc = 0;
    child->sysprot_priv   = 0;
    /* Slice 8: the memcpy copied the parent's namespace POINTERS, which is the
     * correct inheritance -- but each one now has a second user, so take the
     * references. Omitting this makes the parent's exit free a namespace the
     * child is still in. */
    nsproxy_fork_inherit(child);
    seccomp_fork_inherit(child);   /* slice 13: a child cannot escape the filter */
    /* ...then apply any CLONE_NEW* the caller staged. This happens HERE, while
     * the child is still PROC_EMBRYO and not yet enqueued, because the child
     * becomes runnable on another core before this function returns -- see the
     * clone_ns_flags comment in proc.h. */
    {
        uint32_t nsf = parent->clone_ns_flags;
        parent->clone_ns_flags = 0;
        long nsrc = nsf ? nsproxy_apply_clone_flags(child, nsf) : 0;
        /* Slice 10: pid placement runs UNCONDITIONALLY, not only when clone
         * flags were staged -- a plain fork(2) still has to land the child in
         * parent->pid_ns_for_children, which differs from the parent's own
         * namespace after unshare(CLONE_NEWPID), and still needs a vpid
         * allocated in that namespace and every ancestor. */
        if (nsrc == 0)
            nsrc = pid_ns_place_child(child, parent, nsf, false);
        if (nsrc != 0) {
            nsproxy_release(child);
            memset(child, 0, sizeof(*child));
            child->state = PROC_UNUSED;
            return nsrc;
        }
    }
    /* Slice 113: the child returns to user via fork_child_entry, skipping
     * the dispatcher epilogue that clears cursys -- the memcpy'd values
     * would read as the PARENT's syscall in every instrument until the
     * child's own first syscall ([bklmax]'s stale nat=142). */
    child->cursys     = -1;
    child->cursys_nat = -1;
    /* POSIX fork: the child inherits the parent's signal dispositions, mask,
     * and sigreturn trampoline (already copied by the memcpy above), but
     * starts with NO pending signals. (Previously this reset all handlers,
     * which both lost the inherited restorer and wrongly kept the parent's
     * copied pending bits.) */
    child->pending_signals  = 0;
    child->sigstate.pending = 0;

    /* Allocate a new PML4 with the kernel half mirrored. */
    uint64_t new_pml4 = vmm_create_user_pml4();
    if (!new_pml4) {
        nsproxy_release(child);        /* slice 8: undo fork_inherit */
        memset(child, 0, sizeof(*child));
        child->state = PROC_UNUSED;
        return -ABI_ENOMEM;
    }
    child->cr3       = new_pml4;
    child->owns_pml4 = true;

    /* Slice 88: park same-CR3 siblings before write-protecting their pages. */
    tg_vm_quiesce(parent);

    /* Walk+WP under STW without the BKL: chrome's address space is huge and
     * holding the global lock across vmm_cow_fork freezes every other syscall
     * (including DevTools pipe I/O) for tens of seconds. Siblings are parked;
     * page_ref atomics serialize with #PF CoW. Re-take BKL for VMA clone. */
    {
        bool held = bkl_held();
        if (held) bkl_exit();
#ifdef CHROMIUM_BOOT
        kprintf("[fork] cow_fork begin pid=%d\n", parent->pid);
#endif
        int cow_rc = vmm_cow_fork(parent->cr3, new_pml4);
#ifdef CHROMIUM_BOOT
        kprintf("[fork] cow_fork done pid=%d rc=%d\n", parent->pid, cow_rc);
#endif
        /* Slice 92: this bkl_enter is the actor edge of the freeze cycle --
         * a sibling parked on vm_quiesce while holding the BKL wedged us
         * here forever (before vm_quiesce_park_oncpu dropped it). Time it:
         * if [fork] bkl re-take ever prints, some park point still holds
         * the BKL and the cycle is back. */
        uint64_t bkl_t0 = perf_now_ns();
        if (held) bkl_enter();
        uint64_t bkl_dt = perf_now_ns() - bkl_t0;
        if (bkl_dt > 2000000000ull)
            kprintf("[fork] bkl re-take after sweep took %lu ms pid=%d\n",
                    (unsigned long)(bkl_dt / 1000000ull), parent->pid);
        /* VMA tables are tgid-keyed; chrome forks from a launcher THREAD. */
        int vma_pid = parent->is_thread ? parent->tgid : parent->pid;
        if (cow_rc == 0)
            mmap_cow_clone(vma_pid, child_pid);
        tg_vm_resume(parent);   /* always: releases g_cow_fork_lock */
        if (cow_rc != 0) {
            vmm_destroy_user_pml4(new_pml4);
            nsproxy_release(child);    /* slice 8 */
            memset(child, 0, sizeof(*child));
            child->state = PROC_UNUSED;
            return -ABI_ENOMEM;
        }
    }

    /* Clone file descriptors from the table the parent actually USES.
     *
     * This MUST go through proc_fds(): if the parent is a CLONE_FILES thread its
     * own fds[] is deliberately empty (the real table belongs to the
     * thread-group leader -- see sys_clone_thread), so reading parent->fds
     * directly hands the child an ENTIRELY EMPTY fd table. That is not a corner
     * case: chrome launches its child processes from a dedicated launcher
     * THREAD, so every forked child came out with no descriptors at all and
     * died on the first dup2 of its Mojo socket with _exit(127) ("GPU process
     * isn't usable. Goodbye."). POSIX fork() inherits the whole table. */
    struct file **ptab = proc_fds(parent);
    int cloned = 0, failed = 0;
    for (int i = 0; i < PROC_NFDS; i++) {
        if (ptab && ptab[i]) {
            child->fds[i] = file_clone(ptab[i]);
#ifdef CHROMIUM_BOOT
            /* Slice 76: a clone that FAILS leaves the child without that
             * descriptor while the parent keeps its own -- for an AF_UNIX
             * socketpair that means the child can never answer, and the
             * parent's next send hits a dead peer (the crashpad EPIPE).
             * Count both outcomes; name the failures. */
            if (!child->fds[i]) {
                failed++;
                static int fl = 0; if (fl < 16) { fl++;
                    kprintf("[forkfd] pid=%d->%d fd=%d CLONE FAILED kind=%d\n",
                            parent->pid, child->pid, i, (int)ptab[i]->kind);
                }
            } else cloned++;
#endif
        } else {
            child->fds[i] = NULL;
        }
    }
#ifdef CHROMIUM_BOOT
    { static int fk = 0; if (fk < 8) { fk++;
        kprintf("[forkfd] pid=%d -> child pid=%d: %d fds cloned, %d failed\n",
                parent->pid, child->pid, cloned, failed); } }
#endif

    /* Build the child's kernel stack. */
    if (!build_fork_kstack(child)) {
        for (int i = 0; i < PROC_NFDS; i++) {
            if (child->fds[i]) file_close(child->fds[i]);
            child->fds[i] = NULL;
        }
        vmm_destroy_user_pml4(new_pml4);
        nsproxy_release(child);        /* slice 8 */
        memset(child, 0, sizeof(*child));
        child->state = PROC_UNUSED;
        return -ABI_ENOMEM;
    }

    /* The child resumes at the parent's post-`syscall` instruction with the
     * parent's full user register state: copy the live saved register block
     * off the parent's kstack (we are inside the parent's fork() syscall, so
     * it sits exactly at kstack_top - sizeof) into the slot build_fork_kstack
     * reserved at the child's kstack top. fork_child_entry replays the
     * syscall unwind on this copy with rax = 0. */
    memcpy((uint8_t *)child->kstack_top  - sizeof(struct syscall_regs),
           (uint8_t *)parent->kstack_top - sizeof(struct syscall_regs),
           sizeof(struct syscall_regs));

    /* clone(2): if the caller supplied a child_stack, the child must resume ON
     * IT, not on the parent's. Staged by the clone arm (see proc.h
     * clone_child_stack) and applied HERE, for the same reason the namespace
     * flags are: the child is enqueued below and can be running on another core
     * before this function returns, so a post-fork fixup is a race.
     *
     * a2 == 0 means fork semantics (resume on the parent's stack), which is what
     * every raw syscall(SYS_clone, flags, NULL, ...) caller wants -- and is why
     * ignoring a2 outright went unnoticed until glibc's clone() wrapper, whose
     * child-side pops its function pointer off this exact stack. */
    {
        uint64_t cstk = parent->clone_child_stack;
        parent->clone_child_stack = 0;
        if (cstk) {
            struct syscall_regs *cr =
                (struct syscall_regs *)((uint8_t *)child->kstack_top
                                        - sizeof(struct syscall_regs));
            cr->user_rsp = cstk;
        }
    }

    /* Ready to run. */
    child->state = PROC_READY;
    sched_enqueue(child);

    /* SLICE 16: the child gets its OWN cr3 (vmm_cow_fork below), so it is its own
     * mm owner and is bulk-charged its parent's page count; cgroup_mem_charge bumps
     * child->user_pages itself, so assigning it as well would double it.
     *
     * NOTE this is a CoW clone, not a full copy -- copy_user_pages above, with its
     * "intentionally a FULL copy (no COW)" comment, is DEAD CODE (the compiler flags
     * it unused). An earlier version of this comment cited it as justification. So
     * pages shared until first write are counted in both cgroups; see cgroup.h. */
    child->user_pages = 0;
    cgroup_mem_charge(child, parent->user_pages);
    perf_count_proc_spawn();

    kprintf("[fork] parent pid=%d -> child pid=%d cr3=0x%lx\n",
            parent->pid, child->pid, child->cr3);

    return child->pid;  /* parent gets child PID */
}

/* Drop the VMA bookkeeping describing the image execve just replaced.
 *
 * execve builds a brand-new PML4 and destroys the old one, but nothing ever
 * cleared g_vma_tables[]. The entries describing the PREVIOUS image survived
 * into the new one, where mmap_handle_page_fault would happily service a
 * demand fault against one -- faulting a zero page into an address the new
 * image never mapped. (The reap-side half of this same leak is fixed in
 * proc_reap; this is the exec-side half.)
 *
 * CALL THIS ONLY AFTER vfork_child_done(). A share-until-exec child has
 * mm_owner pointing at its PARENT, so proc_mm_pid() would return the parent's
 * key and this would wipe the PARENT's live table. vfork_child_done clears
 * mm_owner precisely because "after exec the child owns a private mm", so
 * ordering this after it is what makes the key correct.
 *
 * TWO GUARDS, because execve here does not implement Linux's "exec kills the
 * other threads in the group":
 *   1. mmkey != p->pid  =>  p is a THREAD exec'ing (key is its tgid). Its
 *      siblings still run in that address space; leave their table alone.
 *   2. another live proc shares the key  =>  p is a group LEADER exec'ing
 *      with threads still alive. Same reasoning.
 * Both are skipped-and-logged rather than silently ignored. (In case 2 those
 * siblings are already in serious trouble -- the caller destroys old_pml4 out
 * from under them -- but that is a separate pre-existing bug, and making this
 * function the place it finally bites would be the wrong trade.) */
static void exec_release_vma_table(struct proc *p) {
    int mmkey = proc_mm_pid(p);
    if (mmkey != p->pid) {
        kprintf("[execve] pid=%d is a thread (mm key=%d); leaving the group's "
                "VMA table intact\n", p->pid, mmkey);
        return;
    }
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *q = &g_proc[i];
        if (q == p) continue;
        if (q->state == PROC_UNUSED || q->state == PROC_EMBRYO ||
            q->state == PROC_TERMINATED) continue;
        if (proc_mm_pid(q) == mmkey) {
            kprintf("[execve] pid=%d still shares its mm key with pid=%d; "
                    "leaving the VMA table intact\n", p->pid, q->pid);
            return;
        }
    }
    mmap_cleanup_proc(mmkey);
}

/* ===================================================================
 * sys_fork_share -- vfork / chrome LaunchProcess semantics.
 *
 * Child shares the parent's CR3 and VMA table until it execve()s or _exits.
 * The launching thread blocks until then. No vmm_cow_fork write-protect, so
 * sibling threads keep writing PartitionAlloc without a stale-TLB window.
 * =================================================================== */
void vfork_child_done(struct proc *child) {
    if (!child || child->vfork_parent <= 0) return;
    int ppid = child->vfork_parent;
    child->vfork_parent = 0;
    struct proc *par = proc_lookup(ppid);

    /* Tear down the private stack we mapped into the shared CR3 so it does
     * not leak into the parent's address space after exec. */
    if (child->vfork_stack_va && child->vfork_stack_pages && par) {
        uint64_t pages = child->vfork_stack_pages;
        uint64_t va = child->vfork_stack_va;
        uint64_t saved = vmm_set_editor_root(par->cr3);
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t pva = va + i * PAGE_SIZE;
            uint64_t phys = vmm_translate(pva);
            vmm_unmap(pva, PAGE_SIZE);
            if (phys) pmm_free_page(phys);
        }
        vmm_set_editor_root(saved);
        child->vfork_stack_va = 0;
        child->vfork_stack_pages = 0;
    }

    /* After exec the child owns a private mm; clear the borrow marker. */
    child->mm_owner = 0;
    if (!par) return;
    /* Signal completion on the PARENT so a late waiter cannot observe a
     * reaped child's PCB, and so a child that finishes before the parent
     * blocks does not lose the wakeup. Fence between the flag store and the
     * state load -- pairs with the BLOCKED-store -> fence -> flag-load order
     * in sys_fork_share's wait loop (slice 89 SMP lost-wakeup fix). */
    __atomic_store_n(&par->vfork_child, 0, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (par->state == PROC_BLOCKED) {
        par->state = PROC_READY;
        sched_enqueue(par);
    }
}

long sys_fork_share(void) {
    struct proc *parent = current_proc();
    if (!parent || parent->pid == 0) return -ABI_EINVAL;

    struct proc *tg = (parent->is_thread && parent->tgid != parent->pid)
                          ? proc_lookup(parent->tgid) : parent;
    if (!tg) tg = parent;

    { long cgrc = cgroup_can_fork(parent); if (cgrc != 0) return cgrc; }  /* slice 15 */
    struct proc *child = proc_slot_claim();
    if (!child) return -ABI_ENOMEM;

    int child_pid = (int)(child - g_proc);
    memcpy(child, parent, sizeof(*child));

    child->pid       = child_pid;
    child->ppid      = parent->pid;
    child->state     = PROC_EMBRYO;   /* memcpy copied parent's state */
    child->wait_pid  = -1;
    child->exit_code = -1;
    child->next_ready = NULL;
    child->on_rq  = false;
    child->on_cpu = 0;
    child->vm_quiesce  = 0;
    child->vm_quiesced = 0;
    /* Per-call clone staging must not be inherited -- see sys_fork. */
    child->clone_ns_flags    = 0;
    child->clone_child_stack = 0;
    child->next_wait  = NULL;
    child->wait_head  = NULL;
    child->join_waiters = NULL;
    child->tgid      = child_pid;
    child->is_thread = false;
    child->detached  = false;
    child->created_ns      = perf_now_ns();
    child->cpu_ns          = 0;
    child->syscall_count   = 0;
    child->last_switch_tsc = 0;
    child->sysprot_priv    = 0;
    child->cursys          = -1;   /* slice 113: see sys_fork */
    child->cursys_nat      = -1;
    child->pending_signals  = 0;
    child->sigstate.pending = 0;
    nsproxy_fork_inherit(child);
    seccomp_fork_inherit(child);   /* slice 13 -- see sys_fork */
    {
        uint32_t nsf = parent->clone_ns_flags;
        parent->clone_ns_flags = 0;
        long nsrc = nsf ? nsproxy_apply_clone_flags(child, nsf) : 0;
        if (nsrc == 0)
            nsrc = pid_ns_place_child(child, parent, nsf, false);  /* slice 10 */
        if (nsrc != 0) {
            nsproxy_release(child);
            memset(child, 0, sizeof(*child));
            child->state = PROC_UNUSED;
            return nsrc;
        }
    }

    /* Borrow parent's address space — do NOT CoW-WP. */
    child->cr3       = tg->cr3;
    child->owns_pml4 = false;
    child->mm_owner  = proc_mm_pid(parent);
    child->vfork_parent = parent->pid;
    child->vfork_child  = 0;
    child->vfork_stack_va = 0;
    child->vfork_stack_pages = 0;

    struct file **ptab = proc_fds(parent);
    for (int i = 0; i < PROC_NFDS; i++) {
        child->fds[i] = (ptab && ptab[i]) ? file_clone(ptab[i]) : NULL;
    }

    if (!build_fork_kstack(child)) {
        for (int i = 0; i < PROC_NFDS; i++) {
            if (child->fds[i]) file_close(child->fds[i]);
            child->fds[i] = NULL;
        }
        nsproxy_release(child);        /* slice 8 */
        memset(child, 0, sizeof(*child));
        child->state = PROC_UNUSED;
        return -ABI_ENOMEM;
    }

    memcpy((uint8_t *)child->kstack_top  - sizeof(struct syscall_regs),
           (uint8_t *)parent->kstack_top - sizeof(struct syscall_regs),
           sizeof(struct syscall_regs));

    /* clone(2) with an EXPLICIT child_stack takes precedence over the private
     * stack below, and the distinction is the whole reason this is safe.
     *
     * Two different callers arrive here:
     *
     *   vfork() / clone(CLONE_VM|CLONE_VFORK) with child_stack == NULL. The
     *     caller expects the child to run on the PARENT's stack. We give it a
     *     private one instead -- a documented, pre-existing deviation that the
     *     comment below explains and that the Chromium launcher path depends
     *     on. UNCHANGED here.
     *
     *   clone(fn, child_stack, CLONE_VM|CLONE_VFORK|SIGCHLD, arg) -- glibc's
     *     library wrapper. The caller supplied a stack PRECISELY so the child
     *     would not touch the parent's, and __clone's child side pops its entry
     *     function off it (`xor %ebp,%ebp; pop %rax; pop %rdi; call *%rax`).
     *     Handing that child a fresh ZEROED stack makes it pop 0 and `call *0`
     *     -- an instruction fetch at NULL, which is the rip=0/err=0x14 fault
     *     already recorded against busybox `unshare -f`. Chromium's
     *     ChrootToSafeEmptyDir (credentials.cc) is the same call, and it died
     *     the same way.
     *
     * Honouring an explicitly supplied stack cannot disturb the first caller,
     * because that caller supplies none. CLONE_VM means the address space is
     * shared, so the pointer is valid in the child by construction. */
    uint64_t user_cstk = parent->clone_child_stack;
    parent->clone_child_stack = 0;
    if (user_cstk) {
        struct syscall_regs *cr =
            (struct syscall_regs *)((uint8_t *)child->kstack_top
                                    - sizeof(struct syscall_regs));
        cr->user_rsp = user_cstk;
        child->vfork_stack_va = 0;      /* nothing for vfork_child_done to free */
        child->vfork_stack_pages = 0;
    } else
    /* Private user stack in the shared CR3. Sharing the parent's RSP lets
     * the child (crashpad double-fork, glibc) smash the launcher's frame;
     * parent then resumes at rip=0. Map a fresh 512 KiB stack at a high VA
     * keyed by child pid (visible to the parent too until vfork_child_done). */
    {
        const uint32_t np = 128; /* 512 KiB — glibc/crashpad probes past 32K */
        uint64_t slot = 0x00007f0000000000ULL +
                        ((uint64_t)child_pid * 0x200000ULL); /* 2 MiB slots */
        uint64_t stack_va = slot;
        uint64_t saved = vmm_set_editor_root(tg->cr3);
        bool ok = true;
        for (uint32_t i = 0; i < np; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) { ok = false; break; }
            if (!vmm_map(stack_va + (uint64_t)i * PAGE_SIZE, phys, PAGE_SIZE,
                         VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_USER)) {
                pmm_free_page(phys);
                ok = false;
                break;
            }
            memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);
        }
        vmm_set_editor_root(saved);
        if (!ok) {
            for (int i = 0; i < PROC_NFDS; i++) {
                if (child->fds[i]) file_close(child->fds[i]);
                child->fds[i] = NULL;
            }
            if (child->kstack_base) kfree(child->kstack_base);
            nsproxy_release(child);    /* slice 8 */
            memset(child, 0, sizeof(*child));
            child->state = PROC_UNUSED;
            return -ABI_ENOMEM;
        }
        child->vfork_stack_va = stack_va;
        child->vfork_stack_pages = np;
        struct syscall_regs *cr =
            (struct syscall_regs *)((uint8_t *)child->kstack_top
                                    - sizeof(struct syscall_regs));
        /* SysV ABI red zone is 128 bytes below RSP; leave headroom. */
        cr->user_rsp = stack_va + (uint64_t)np * PAGE_SIZE - 256;
    }

    /* SLICE 16: NO charge here, and that is deliberate.
     *
     * sys_fork_share SHARES the parent's address space (child->mm_owner = parent),
     * so every page the child can touch is already charged to the parent. Charging
     * the child too would double-count the whole space on every vfork -- and busybox
     * uses vfork constantly. The child's own counter stays 0, which is also what
     * makes its teardown a no-op while the parent still holds the mapping.
     *
     * The pages the child faults in later resolve to the mm owner via
     * mm_charge_target(), so they land on the parent exactly once. */
    child->user_pages = 0;
    __atomic_store_n(&parent->vfork_child, child_pid, __ATOMIC_RELEASE);
    child->state = PROC_READY;
    sched_enqueue(child);
    perf_count_proc_spawn();

    kprintf("[fork] share pid=%d -> child pid=%d cr3=0x%lx mm=%d stack=0x%lx\n",
            parent->pid, child_pid, (unsigned long)child->cr3, child->mm_owner,
            (unsigned long)child->vfork_stack_va);

    /* Block the launcher until child execve/_exit. Drop BKL so the child
     * (and siblings) can enter the kernel.
     *
     * Slice 89 SMP lost-wakeup fix: the old loop read vfork_child FIRST and
     * set BLOCKED after. On another CPU vfork_child_done could clear the
     * flag and test `state == PROC_BLOCKED` inside that window -- seeing
     * RUNNING, it skipped the wake; we then parked BLOCKED forever (chrome
     * launcher thread gone => whatever it owed the UI thread never happens).
     * Order it store-BLOCKED -> fence -> load-flag, with the matching
     * store-flag -> fence -> load-state in vfork_child_done: one side is
     * now guaranteed to see the other (plain release/acquire is NOT enough
     * -- x86 StoreLoad reordering lets both loads see stale values). */
    {
        bool held = bkl_held();
        if (held) bkl_exit();
        for (;;) {
            parent->state = PROC_BLOCKED;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&parent->vfork_child, __ATOMIC_ACQUIRE) == 0)
                break;
            sched_yield();
        }
        parent->state = PROC_RUNNING;
        if (held) bkl_enter();
    }

    return child_pid;
}

/* ===================================================================
 * sys_clone_thread -- Linux clone(CLONE_VM,...) : create a thread that
 * SHARES the caller's address space and, like fork, resumes after the
 * clone `syscall` (rax = 0) -- but on the caller-provided `stack`, with
 * `tls` as its FS base. This mirrors sys_fork's resume mechanism
 * (copy the trapframe, descend through fork_child_entry) but installs
 * the shared-VM thread fields thread_create uses instead of allocating a
 * fresh PML4. Returns the new thread's tid to the caller.
 *
 * x86-64 clone arg order: clone(flags, stack, parent_tid, child_tid, tls).
 * =================================================================== */
long sys_clone_thread(uint64_t flags, uint64_t stack, uint64_t ptid,
                      uint64_t ctid, uint64_t tls) {
    struct proc *parent = current_proc();
    if (!parent || parent->pid == 0) return -ABI_EINVAL;
    if (!stack) return -ABI_EINVAL;             /* a thread needs its own stack */

    /* The thread joins the caller's thread group (the leader owns the PML4). */
    struct proc *tg = (parent->is_thread && parent->tgid != parent->pid)
                          ? proc_lookup(parent->tgid) : parent;
    if (!tg) return -ABI_EINVAL;

    /* Slice 15: threads count too. Linux's pids controller limits TASKS, not
     * processes, so a cgroup at its limit must not be able to keep growing by
     * spawning threads instead of forking -- that would make pids.max trivially
     * evadable by any threaded program. cgroup_nr_procs counts g_proc entries,
     * which is threads included, so the two agree. */
    { long cgrc = cgroup_can_fork(parent); if (cgrc != 0) return cgrc; }
    struct proc *child = proc_slot_claim();
    if (!child) return -ABI_ENOMEM;
    int child_pid = (int)(child - g_proc);

    memcpy(child, parent, sizeof(*child));
    child->pid        = child_pid;
    child->ppid       = parent->ppid;
    child->tgid       = tg->pid;                /* same thread group */
    child->is_thread  = true;
    child->cr3        = tg->cr3;                /* SHARE the address space */
    child->owns_pml4  = false;                  /* never free the shared PML4 */
    child->mm_owner   = 0;
    child->vfork_parent = 0;
    child->vfork_child  = 0;
    child->detached   = true;
    child->state      = PROC_EMBRYO;  /* memcpy copied parent's state */
    child->wait_pid   = -1;
    child->exit_code  = -1;
    child->next_ready = NULL;
    /* Slice 39: same reset as sys_fork -- the memcpy copied the parent's
     * live on_cpu/on_rq, which would make this thread unschedulable. */
    child->on_rq  = false;
    child->on_cpu = 0;
    /* Per-call clone staging must not be inherited -- see sys_fork. */
    child->clone_ns_flags    = 0;
    child->clone_child_stack = 0;
    child->next_wait  = NULL;
    child->wait_head  = NULL;
    child->join_waiters = NULL;
    child->pending_signals  = 0;
    child->sigstate.pending = 0;
    child->created_ns      = perf_now_ns();
    child->cpu_ns          = 0;
    child->syscall_count   = 0;
    child->last_switch_tsc = 0;
    child->sysprot_priv    = 0;
    child->cursys          = -1;   /* slice 113: see sys_fork */
    child->cursys_nat      = -1;
    /* Slice 8: a thread shares its leader's namespaces (Linux shares the whole
     * nsproxy across CLONE_THREAD) but holds its OWN reference, because a
     * thread can outlive its leader in this kernel -- see proc_reap's PML4
     * heir logic for the same hazard. Both thread-teardown paths in proc.c call
     * nsproxy_release to balance this. */
    nsproxy_fork_inherit(child);
    seccomp_fork_inherit(child);   /* slice 13: a child cannot escape the filter */
    /* Slice 10: a tid is namespace-local exactly like a pid, so a thread needs
     * its own vpid in the thread group's namespace. CLONE_NEW* on a thread is
     * refused at the syscall boundary, so this never creates a namespace. */
    if (pid_ns_place_child(child, tg, 0, true) != 0) {
        nsproxy_release(child);
        memset(child, 0, sizeof(*child));
        child->state = PROC_UNUSED;
        return -ABI_EAGAIN;
    }

    size_t nlen = strlen(tg->name);
    if (nlen > PROC_NAME_MAX - 3) nlen = PROC_NAME_MAX - 3;
    memcpy(child->name, tg->name, nlen);
    child->name[nlen] = '+'; child->name[nlen + 1] = 'T'; child->name[nlen + 2] = '\0';

    /* CLONE_FILES: a thread SHARES the thread-group leader's fd table -- it does
     * NOT get its own copy. All runtime fd access routes through proc_fds()
     * (syscall.c), which returns the leader's fds[] for a thread. Leave the
     * child's own fds[] EMPTY (the memcpy above copied the parent's pointers --
     * clear them) so the thread's exit never closes the shared open
     * descriptions, and so an fd created on any thread is visible to all.
     * (Copying, as fork does, made chrome's IO-thread epoll/socket fds invisible
     * to worker threads -> NULL handle -> a timing-dependent NULL-pointer call.) */
    for (int i = 0; i < PROC_NFDS; i++)
        child->fds[i] = NULL;

    if (!build_fork_kstack(child)) {
        nsproxy_release(child);        /* slice 8 */
        memset(child, 0, sizeof(*child));
        child->state = PROC_UNUSED;
        return -ABI_ENOMEM;
    }

    /* Resume at the parent's post-`syscall` RIP with rax=0, but on the new
     * thread stack. (fork_child_entry applies rax=0 during the unwind.) */
    struct syscall_regs *cr = (struct syscall_regs *)
        ((uint8_t *)child->kstack_top - sizeof(struct syscall_regs));
    memcpy(cr, (uint8_t *)parent->kstack_top - sizeof(struct syscall_regs),
           sizeof(struct syscall_regs));
    cr->user_rsp = stack;

    child->tls_base = (flags & 0x00080000u /* CLONE_SETTLS */) ? tls
                                                              : parent->tls_base;

    /* CLONE_PARENT_SETTID / CLONE_CHILD_SETTID: publish the new tid into the
     * shared address space (we're already on the shared CR3).
     *
     * Slice 10: this MUST be the namespace-local tid, because user code
     * compares it against gettid() and passes it to tgkill(). Publishing a kpid
     * here while gettid() reported a vpid would break pthread_join in a pid
     * namespace -- and silently, since both numbers look equally plausible. */
    { uint32_t vtid = (uint32_t)(pid_vnr(child) ? pid_vnr(child) : child_pid);
      if ((flags & 0x00100000u /* CLONE_PARENT_SETTID */) && ptid)
          (void)put_user_u32((void *)ptid, vtid);
      if ((flags & 0x01000000u /* CLONE_CHILD_SETTID */) && ctid)
          (void)put_user_u32((void *)ctid, vtid);
    }

    /* B11: CLONE_CHILD_CLEARTID -- record the futex address the kernel must
     * zero + wake when THIS thread exits (the pthread_join rendezvous). The
     * child was memcpy'd from the parent, so set it explicitly (else it would
     * inherit the parent's, or a stale value). */
    child->clear_child_tid =
        (flags & 0x00200000u /* CLONE_CHILD_CLEARTID */) ? ctid : 0;

    child->state = PROC_READY;
    sched_enqueue(child);
    perf_count_proc_spawn();

    kprintf("[clone] thread tid=%d in tgid=%d (shared VM, stack=0x%lx)\n",
            child_pid, child->tgid, (unsigned long)stack);
    return child_pid;  /* caller gets the new tid */
}

/* ===================================================================
 * sys_execve -- Replace the current process image with a new ELF.
 *
 * path  : absolute path to the ELF binary.
 * argv  : NULL-terminated array of argument strings.
 * envp  : NULL-terminated array of environment strings.
 *
 * On success this never returns -- the process starts executing the
 * new program. On failure returns a negative ABI error code.
 * =================================================================== */

/* User-stack constants (must match proc.c). */
#define USER_STACK_PAGES     8
#define USER_STACK_BYTES     (USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_TOP_VA    0x0000800000000000ULL
#define USER_STACK_TOP_PAGE  (USER_STACK_TOP_VA - USER_STACK_BYTES)
#define USER_STACK_RSP_INIT  (USER_STACK_TOP_VA - 16)
#define USER_HEAP_BASE       0x0000000010000000ULL
#define USER_HEAP_MAX_BYTES  (256ULL * 1024ULL * 1024ULL)

/* SWAPGS shadow MSR -- holds the CPL3 GS base (TEB for a Win32 PE). See
 * do_switch (sched.c) + proc_switch.S. */
#define IA32_KERNEL_GS_BASE_MSR 0xC0000102u

/* Windows entry points are entered with a 256 KiB stack, matching proc.c's
 * build_user_stack (64 pages). The ELF execve path uses USER_STACK_PAGES (8),
 * but a Win32 CRT wants more headroom, so the PE arm maps its own. */
#define PE_STACK_PAGES  64

/* POSIX: a successful exec closes every descriptor marked FD_CLOEXEC.
 * Runs at the COMMIT point only -- a failed execve returns -1 with the
 * caller's descriptors intact, which is what lets shells retry. Only the
 * Linux personality sets bits, so a native or Win32 image pays one bitmap
 * walk and closes nothing. Shared by the ELF and PE exec arms: exec is
 * exec, whichever personality the new image lands in. */
static void exec_close_cloexec_fds(struct proc *p) {
    struct file **tab = proc_fds(p);
    if (!tab) return;
    for (int i = 0; i < PROC_NFDS; i++) {
        if (tab[i] && fd_cloexec_get(p, i)) {
            file_close(tab[i]);
            tab[i] = 0;
            fd_cloexec_set(p, i, 0);
        }
    }
}

/* ===================================================================
 * execve_pe -- the Windows-PE arm of sys_execve (Track X / X1).
 *
 * sys_execve() has already: copied the path/argv/envp into kernel buffers,
 * read the image, created `new_pml4`, and switched CR3 to it (so vmm_map here
 * targets the new address space). This loads the image as a Win32 PE instead
 * of an ELF, flips the process to ABI_PERS_WIN32, installs the TEB/GS base,
 * and enters the PE entry point -- so a Linux or native process (e.g. a shell
 * stage) can execve() a genuine .exe. This is the kernel feature that lets a
 * single pipeline mix Windows and Linux binaries.
 *
 * On success this never returns (drops to ring 3). On failure it restores the
 * caller's previous address space and returns a negative ABI error; the
 * caller's image is gone past the point of pe_load_user success, so a late
 * failure (stack OOM) returns -ABI_ENOMEM with the old image already torn
 * down -- identical to the ELF path's point-of-no-return semantics.
 * =================================================================== */
static long execve_pe(struct proc *p, void *image, size_t image_size,
                      int image_borrowed,
                      const char *kpath, int kargc, char **kargv_buf,
                      uint64_t old_pml4, uint64_t new_pml4,
                      uint64_t saved_cr3, uint64_t old_editor) {
    struct pe_load_info pe_info = {0};
    int prc = pe_load_user(image, image_size, kargc, kargv_buf, &pe_info);
    if (!image_borrowed) kfree(image);   /* slice 112: initrd borrow */

    bool ok = (prc == 0);

    /* Map the Win32 user stack (256 KiB). Top is shared with the ELF layout
     * (USER_STACK_TOP_VA); the base floats down PE_STACK_PAGES pages. */
    const uint64_t pe_stack_top_page =
        USER_STACK_TOP_VA - (uint64_t)PE_STACK_PAGES * PAGE_SIZE;
    if (ok) {
        p->user_stack_base  = pe_stack_top_page;
        p->user_stack_pages = PE_STACK_PAGES;
        for (size_t i = 0; i < PE_STACK_PAGES; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) { ok = false; break; }
            if (!vmm_map(pe_stack_top_page + i * PAGE_SIZE, phys, PAGE_SIZE,
                         VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_USER)) {
                pmm_free_page(phys);
                ok = false;
                break;
            }
            memset((void *)pmm_phys_to_virt(phys), 0, PAGE_SIZE);
        }
    }

    if (!ok) {
        write_cr3(saved_cr3);
        vmm_set_editor_root(old_editor);
        vmm_destroy_user_pml4(new_pml4);
        return -ABI_ENOMEM;
    }

    /* Success -- point of no return. Commit the new address space. */
    write_cr3(new_pml4);
    vmm_set_editor_root(old_editor);
    if (p->vfork_parent > 0)
        vfork_child_done(p);
    /* After vfork_child_done, so the mm key is this process's own. */
    exec_release_vma_table(p);
    if (old_pml4 != new_pml4 && p->owns_pml4) {
        vmm_destroy_user_pml4(old_pml4);
    }
    p->cr3       = new_pml4;
    p->owns_pml4 = true;

    /* Reset heap (both tobyOS and Win32 CRT heaps are per-image). */
    p->brk_base = USER_HEAP_BASE;
    p->brk_cur  = USER_HEAP_BASE;
    p->brk_max  = USER_HEAP_BASE + USER_HEAP_MAX_BYTES;
    p->win_heap_cur = 0;
    p->win_heap_end = 0;
    p->clear_child_tid = 0;

    /* FD_CLOEXEC acts on ANY successful exec, a .exe included. */
    exec_close_cloexec_fds(p);

    /* Flip to the Win32 personality and install the PE's TEB + resource/TLS
     * metadata -- mirrors the PE arm of proc_spawn (proc.c). */
    p->personality      = ABI_PERS_WIN32;
    p->gs_base          = pe_info.teb;
    p->win_image_base   = pe_info.image_base;
    p->win_rsrc_rva     = pe_info.rsrc_rva;
    p->win_rsrc_size    = pe_info.rsrc_size;
    p->win_tls_raw_va   = pe_info.tls_raw_va;
    p->win_tls_raw_size = pe_info.tls_raw_size;
    p->win_tls_total    = pe_info.tls_total;
    p->win_tls_index    = pe_info.tls_index;

    /* Update name from the new path. */
    const char *base = kpath;
    for (const char *c = kpath; *c; c++) if (*c == '/') base = c + 1;
    size_t n = strlen(base);
    if (n >= PROC_NAME_MAX) n = PROC_NAME_MAX - 1;
    memcpy(p->name, base, n);
    p->name[n] = '\0';

    /* B20 (procfs): re-point /proc/<pid>/exe at the new image. */
    {
        size_t pn = strlen(kpath);
        if (pn >= ABI_PATH_MAX) pn = ABI_PATH_MAX - 1;
        memcpy(p->exe_path, kpath, pn);
        p->exe_path[pn] = '\0';
    }

    p->user_entry = pe_info.entry;
    /* Win32 ABI: enter with RSP%16==8 (as if reached by a CALL). Headroom
     * above RSP for the marshalling gate's MS-x64 stack-arg reads. */
    p->user_rsp   = ((USER_STACK_TOP_VA - 0x400) & ~0xFULL) - 8;

    signal_init_proc(&p->sigstate);
    p->pending_signals = 0;

    /* execve normally relies on do_switch having put this proc's GS base in the
     * SWAPGS shadow before it runs. But we are NOT going through the scheduler
     * -- we iretq directly via proc_enter_user_asm, whose swapgs moves the
     * shadow into the active GS. The shadow still holds the OLD (pre-execve)
     * value, so install the PE's TEB now; otherwise the CRT's gs:[0x30]
     * (NtCurrentTeb) reads stale kernel per-CPU data. Safe in CPL0: a kernel
     * IRQ does not swapgs (it checks the trapped CS). */
    wrmsr(IA32_KERNEL_GS_BASE_MSR, pe_info.teb);

    kprintf("[execve] pid=%d now running Win32 PE '%s' entry=%p teb=%p "
            "rsp=%p\n", p->pid, p->name, (void *)p->user_entry,
            (void *)pe_info.teb, (void *)p->user_rsp);

    bkl_exit();
    proc_enter_user_asm(p->user_entry, p->user_rsp);
}

long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    struct proc *p = current_proc();
    if (!p || p->pid == 0) return -ABI_EINVAL;
#ifdef CHROMIUM_BOOT
    /* Slice 112 prep: [bklmax] attributed a ~730ms BKL hold to execve
     * (sys=59) for chrome-sized images, but not to a PHASE. Time the
     * candidate phases; one [exectime] line prints at commit. */
    uint64_t xt_entry = perf_now_ns();
    uint64_t xt_read = 0, xt_load = 0, xt_interp = 0, xt_stack = 0;
#endif

    /* Copy path into a kernel buffer AND resolve it the way every other
     * path-taking syscall does.
     *
     * Linux slice 7: this used a bare strncpy_from_user(), i.e. the RAW user
     * string, so execve was the ONE path syscall that ignored both the cwd
     * and -- once slice 5 added it -- the process's chroot root. Inside a
     * chroot that is not a cosmetic gap: `chroot /jail /bin/busybox` silently
     * exec'd the HOST's /bin/busybox (the path exists there too), so the jail
     * appeared to work while running the wrong binary entirely; and anything
     * whose executable exists ONLY inside the jail -- Alpine's /bin/sh,
     * /sbin/apk -- failed with "No such file or directory" while plainly
     * being present. One root cause, three symptoms.
     *
     * resolve_user_path() does the user copy itself and applies cwd + fs_root,
     * with the chroot prefix added AFTER lexical cleaning so ".." cannot climb
     * out (see its comment). */
    char kpath[ABI_PATH_MAX];
    {
        extern int resolve_user_path(const char *user_path, char *out, size_t cap);
        int prc = resolve_user_path(path, kpath, sizeof(kpath));
        if (prc != 0) return prc;
    }
    long plen = (long)strlen(kpath);
    if (plen == 0) return -ABI_EINVAL;

    /* Resolve symlinks NOW, so everything downstream -- the ELF read, p->name
     * and above all p->exe_path -- refers to the REAL binary. Linux execve does
     * this: after exec'ing a symlink, /proc/self/exe names the target, not the
     * link. Chrome depends on it twice over: it re-execs /proc/self/exe for
     * every child process, and it derives DIR_ASSETS (where icudtl.dat, the
     * .pak files and the v8 snapshot live) from readlink("/proc/self/exe").
     * Recording the literal "/proc/self/exe" made that self-referential, so
     * DIR_ASSETS became "/proc/self", every child failed to find icudtl.dat,
     * and each one died on `Invalid file descriptor to ICU data received.`
     * Best-effort: if the path isn't a symlink this is a straight copy, and on
     * any resolve error we keep the original and let the ELF read report it. */
    {
        char realpath_buf[VFS_PATH_MAX];
        if (vfs_follow_link(kpath, realpath_buf, sizeof(realpath_buf)) == VFS_OK) {
            size_t rn = strlen(realpath_buf);
            if (rn > 0 && rn < sizeof(kpath)) {
                memcpy(kpath, realpath_buf, rn + 1);
                plen = (long)rn;
            }
        }
    }

    /* Copy argv into kernel buffers. Each user pointer-array slot and
     * each string is read through an accessor; a bad slot just ends the
     * vector (matching the old tolerant behaviour). */
    int kargc = 0;
    char *kargv_buf[ABI_ARGV_MAX];
    static char kargv_pool[ABI_ARGV_MAX][ABI_ARG_MAX];

    if (argv) {
        for (int i = 0; i < ABI_ARGV_MAX; i++) {
            uint64_t slot = 0;
            if (get_user_u64(&slot, argv + i) != 0) break;
            if (!slot) break;
            if (strncpy_from_user(kargv_pool[kargc],
                                  (const void *)(uintptr_t)slot,
                                  ABI_ARG_MAX) < 0) break;
            kargv_buf[kargc] = kargv_pool[kargc];
            kargc++;
        }
    }

#ifdef CHROMIUM_BOOT
    /* Slice 21 instrument: chrome re-execs /proc/self/exe for every typed child
     * process and hands it inherited fds (ICU data, Mojo channel) via dup2 into
     * fixed slots, naming them on the command line. Print BOTH the argv and the
     * fd table the child actually has at exec time -- the pair tells you at a
     * glance whether an fd chrome expects is simply absent. Behaviour-neutral. */
    {
        kprintf("[execve-argv] pid=%d argc=%d:", p->pid, kargc);
        for (int i = 0; i < kargc; i++) {
            kprintf(" %s", kargv_buf[i]);
            if (strcmp(kargv_buf[i], "--type=renderer") == 0) p->is_renderer = true;
        }
        kprintf("\n");
        struct file **tab = proc_fds(p);
        kprintf("[execve-fds] pid=%d open:", p->pid);
        if (tab) {
            for (int i = 0; i < PROC_NFDS; i++)
                if (tab[i]) kprintf(" %d(k%d)", i, (int)tab[i]->kind);
        }
        kprintf("\n");
    }
#endif

    /* Copy envp into kernel buffers. */
    int kenvc = 0;
    char *kenvp_buf[ABI_ENVP_MAX];
    static char kenvp_pool[ABI_ENVP_MAX][ABI_ARG_MAX];

    if (envp) {
        for (int i = 0; i < ABI_ENVP_MAX; i++) {
            uint64_t slot = 0;
            if (get_user_u64(&slot, envp + i) != 0) break;
            if (!slot) break;
            if (strncpy_from_user(kenvp_pool[kenvc],
                                  (const void *)(uintptr_t)slot,
                                  ABI_ARG_MAX) < 0) break;
            kenvp_buf[kenvc] = kenvp_pool[kenvc];
            kenvc++;
        }
    }

    /* Read the ELF image from VFS. Slice 112: for an initrd (ramfs) file
     * this BORROWS a pointer into the resident tar instead of copying --
     * the copy was ~320 ms of chrome's ~700 ms execve BKL hold, paid five
     * times per bootstrap for the same immutable 188 MiB binary. */
    void  *image      = NULL;
    size_t image_size = 0;
    int    img_borrowed = 0;
#ifdef CHROMIUM_BOOT
    xt_read = perf_now_ns();
#endif
    int rc = vfs_read_all_ref(kpath, &image, &image_size, &img_borrowed);
#ifdef CHROMIUM_BOOT
    xt_read = perf_now_ns() - xt_read;
#endif
    if (rc != 0) {
        kprintf("[execve] cannot read '%s': %d\n", kpath, rc);
        return -ABI_ENOENT;
    }

    /* ---- Linux slice 2: the set-user-ID-on-exec transition ----
     *
     * Applied here: the image has been read (so we know the exec will not fail
     * for a missing/unreadable file) but the address space has not been torn
     * down yet. POSIX says the transition happens as part of a SUCCESSFUL exec.
     *
     * Semantics: S_ISUID sets the EFFECTIVE uid to the file's owner and copies
     * it to the SAVED uid -- the saved copy is the whole point, since it is
     * what lets the program drop privilege with seteuid() and pick it back up
     * later. The REAL uid never changes, which is how a setuid program can
     * still tell who invoked it.
     *
     * Only possible at all because VFS_MODE_PERMS widened from 00777 to 07777
     * in this slice; before that every filesystem masked S_ISUID off and a
     * setuid binary was indistinguishable from an ordinary one.
     *
     * MNT_NOSUID IS enforced (VFS_MNT_NOSUID, added in slice 2 alongside this).
     * Linux ignores setuid bits on filesystems mounted nosuid, and that check
     * is what stands between "userspace can mount" and "userspace is root":
     * without it, an attacker mounts any image containing a root-owned setuid
     * shell and wins. Every kernel-internal mount passes flags 0, so this is a
     * no-op today; mount(2) in slice 5 only has to pass the user's flags to
     * vfs_mount_flags() and this site already does the right thing. */
    {
        struct vfs_stat xst;
        uint32_t mflags = vfs_path_mount_flags(kpath);
        if (!(mflags & VFS_MNT_NOSUID) &&
            vfs_stat(kpath, &xst) == VFS_OK && (xst.mode & VFS_MODE_VALID)) {
            if (xst.mode & VFS_MODE_SETUID) {
                p->uid  = (int)xst.uid;
                p->suid = p->uid;
                if (p->uid == 0)
                    p->lcap_eff = p->lcap_perm = p->lcap_inh = ~0ull;
            }
            if (xst.mode & VFS_MODE_SETGID) {
                p->gid  = (int)xst.gid;
                p->sgid = p->gid;
            }
        } else if (mflags & VFS_MNT_NOSUID) {
            struct vfs_stat nst;
            if (vfs_stat(kpath, &nst) == VFS_OK &&
                (nst.mode & (VFS_MODE_SETUID | VFS_MODE_SETGID)))
                kprintf("[execve] nosuid mount: ignoring set-id bits on '%s'\n",
                        kpath);
        }
    }

    /* Destroy the old user-half mappings and rebuild the address space.
     * We destroy the OLD PML4 and create a fresh one. */
    uint64_t old_pml4 = p->cr3;
    uint64_t new_pml4 = vmm_create_user_pml4();
    if (!new_pml4) {
        if (!img_borrowed) kfree(image);
        return -ABI_ENOMEM;
    }

    /* Switch to the new PML4 for loading. */
    uint64_t saved_cr3  = read_cr3();
    uint64_t old_editor = vmm_set_editor_root(new_pml4);
    write_cr3(new_pml4);

    /* Track X / X1: a Windows PE/COFF image loads via an entirely separate
     * path (sections + IAT thunks, Win32 personality, TEB/GS). Detect it by
     * the 'MZ'/'PE' magic and branch -- this is what lets a Linux or native
     * process (e.g. a busybox `sh` pipeline stage) execve() a .exe, so a
     * single pipeline can mix Windows and Linux binaries. */
    if (pe_is_image(image, image_size)) {
        return execve_pe(p, image, image_size, img_borrowed,
                         kpath, kargc, kargv_buf,
                         old_pml4, new_pml4, saved_cr3, old_editor);
    }

    /* Check for PT_INTERP. */
    char interp_path[ABI_PATH_MAX];
    bool has_interp = elf_peek_interp(image, image_size,
                                      interp_path, sizeof(interp_path));

    uint64_t prog_load_base = 0;
    {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
        if (image_size >= sizeof(Elf64_Ehdr) && eh->e_type == ET_DYN)
            prog_load_base = 0x0000000000500000ULL;
    }
    uint64_t interp_load_base = has_interp ? 0x0000000040000000ULL : 0;

    struct elf_load_info prog_info = {0};
#ifdef CHROMIUM_BOOT
    xt_load = perf_now_ns();
#endif
    bool ok = elf_load_user_at(image, image_size, prog_load_base, &prog_info);
#ifdef CHROMIUM_BOOT
    xt_load = perf_now_ns() - xt_load;
#endif
    if (!img_borrowed) kfree(image);

    /* Track B: re-latch the ABI personality from the new image (brand OR a
     * Linux-loader PT_INTERP, per B10), so `exec`ing a Linux binary from a
     * native one (or vice-versa) switches the syscall ABI for the replaced
     * process. */
    if (ok) {
        p->personality = elf_is_linux_abi(prog_info.osabi, has_interp,
                                          interp_path, prog_info.has_gnu_phdr)
                             ? ABI_PERS_LINUX : ABI_PERS_TOBY;
    }

    struct elf_load_info interp_info = {0};
    if (ok && has_interp) {
#ifdef CHROMIUM_BOOT
        xt_interp = perf_now_ns();
#endif
        void *interp_image = NULL;
        size_t interp_size = 0;
        int irc = vfs_read_all(interp_path, &interp_image, &interp_size);
        if (irc != 0) {
            ok = false;
        } else {
            ok = elf_load_user_at(interp_image, interp_size,
                                  interp_load_base, &interp_info);
            kfree(interp_image);
        }
#ifdef CHROMIUM_BOOT
        xt_interp = perf_now_ns() - xt_interp;
#endif
    }

    /* Build user stack. */
    if (ok) {
#ifdef CHROMIUM_BOOT
        xt_stack = perf_now_ns();
#endif
        p->user_stack_base  = USER_STACK_TOP_PAGE;
        p->user_stack_pages = USER_STACK_PAGES;
        for (size_t i = 0; i < USER_STACK_PAGES; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) { ok = false; break; }
            if (!vmm_map(USER_STACK_TOP_PAGE + i * PAGE_SIZE, phys, PAGE_SIZE,
                         VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_USER)) {
                pmm_free_page(phys);
                ok = false;
                break;
            }
            memset((void *)pmm_phys_to_virt(phys), 0, PAGE_SIZE);
        }
    }

    /* Build auxv + pack argv/envp onto user stack. */
    struct abi_auxv aux[20];
    int auxc = 0;
    uint64_t user_rsp = USER_STACK_RSP_INIT;

    if (ok) {
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PHDR,   prog_info.phdr_va };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PHNUM,  prog_info.phnum   };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PHENT,  prog_info.phent   };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_BASE,
                                         has_interp ? interp_info.load_base : 0 };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_ENTRY,  prog_info.entry   };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PAGESZ, PAGE_SIZE         };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_FLAGS,  0                 };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_UID,    0                 };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_EUID,   0                 };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_GID,    0                 };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_EGID,   0                 };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_HWCAP,  ABI_AT_HWCAP_X86_64_BASE };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_CLKTCK, 100               };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_SECURE, 0                 };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_RANDOM, USER_STACK_TOP_VA - 16 };

        /* Pack using inline logic (we can't call proc.c's static pack_user_stack).
         * We build a minimal layout: argc, argv[], NULL, envp[], NULL,
         * auxv[], AT_NULL, string pool. */
        size_t pool = 0;
        for (int i = 0; i < kargc; i++) pool += strlen(kargv_buf[i]) + 1;
        for (int i = 0; i < kenvc; i++) pool += strlen(kenvp_buf[i]) + 1;
        size_t pool_padded = (pool + 7) & ~7ULL;
        size_t argv_bytes  = (size_t)(kargc + 1) * 8;
        size_t envp_bytes  = (size_t)(kenvc + 1) * 8;
        size_t auxv_bytes  = (size_t)(auxc + 1) * 16;
        size_t pad_head    = 16;

        size_t total = pad_head + pool_padded + auxv_bytes + envp_bytes + argv_bytes + 8;
        if (total + 64 > USER_STACK_BYTES) {
            ok = false;
        } else {
            uint64_t pool_va = USER_STACK_TOP_VA - pad_head - pool_padded;
            uint64_t auxv_va = pool_va - auxv_bytes;
            uint64_t envp_va = auxv_va - envp_bytes;
            uint64_t argv_va = envp_va - argv_bytes;
            uint64_t argc_va = argv_va - 8;

            if (argc_va % 16 != 0) {
                argc_va -= 8; argv_va -= 8;
                envp_va -= 8; auxv_va -= 8; pool_va -= 8;
            }

            uint8_t        *pool_ptr = (uint8_t  *)pool_va;
            uint64_t       *argv_ptr = (uint64_t *)argv_va;
            uint64_t       *envp_ptr = (uint64_t *)envp_va;
            struct abi_auxv *auxv_ptr = (struct abi_auxv *)auxv_va;
            size_t off = 0;

            /* These writes target the new image's user stack. Under SMAP the
             * kernel may not touch user memory without a stac window -- open
             * one around the whole pack (same as elf.c's segment copy). Without
             * this, execve from a CPL-3 caller #PFs on the first argv write
             * under +smap (exposed by busybox sh's fork+execve). */
            unsigned long uflags = uaccess_begin();
            for (int i = 0; i < kargc; i++) {
                size_t l = strlen(kargv_buf[i]) + 1;
                memcpy(pool_ptr + off, kargv_buf[i], l);
                argv_ptr[i] = pool_va + off;
                off += l;
            }
            argv_ptr[kargc] = 0;

            for (int i = 0; i < kenvc; i++) {
                size_t l = strlen(kenvp_buf[i]) + 1;
                memcpy(pool_ptr + off, kenvp_buf[i], l);
                envp_ptr[i] = pool_va + off;
                off += l;
            }
            envp_ptr[kenvc] = 0;

            for (int i = 0; i < auxc; i++) auxv_ptr[i] = aux[i];
            auxv_ptr[auxc].a_type = ABI_AT_NULL;
            auxv_ptr[auxc].a_val  = 0;

            /* Slice 87: seed AT_RANDOM (was left zero with the stack). */
            {
                uint8_t rnd[16];
                rng_fill(rnd, sizeof rnd);
                memcpy((void *)(uintptr_t)(USER_STACK_TOP_VA - 16), rnd, 16);
            }

            *(uint64_t *)argc_va = (uint64_t)(uint32_t)kargc;
            uaccess_end(uflags);
            user_rsp = argc_va;
        }
    }

#ifdef CHROMIUM_BOOT
    if (xt_stack) xt_stack = perf_now_ns() - xt_stack;   /* incl. auxv pack */
#endif
    if (!ok) {
        /* Restore previous address space on failure. */
        write_cr3(saved_cr3);
        vmm_set_editor_root(old_editor);
        vmm_destroy_user_pml4(new_pml4);
        return -ABI_ENOMEM;
    }

    /* Success -- point of no return. Destroy the old address space. */
    write_cr3(new_pml4);
    vmm_set_editor_root(old_editor);

    /* Share-until-exec: drop the borrowed mm and unblock the launcher
     * now that we have a private address space. */
    if (p->vfork_parent > 0)
        vfork_child_done(p);

    /* After vfork_child_done, so the mm key is this process's own. */
    exec_release_vma_table(p);

    /* Destroy the old PML4 now that we're on the new one. */
#ifdef CHROMIUM_BOOT
    uint64_t xt_old = perf_now_ns();
#endif
    if (old_pml4 != new_pml4 && p->owns_pml4) {
        vmm_destroy_user_pml4(old_pml4);
    }
#ifdef CHROMIUM_BOOT
    xt_old = perf_now_ns() - xt_old;
    kprintf("[exectime] pid=%d img=%luKiB total=%lums read=%lums load=%lums "
            "interp=%lums stack=%lums olddestroy=%lums\n",
            p->pid, (unsigned long)(image_size / 1024),
            (unsigned long)((perf_now_ns() - xt_entry) / 1000000ull),
            (unsigned long)(xt_read / 1000000ull),
            (unsigned long)(xt_load / 1000000ull),
            (unsigned long)(xt_interp / 1000000ull),
            (unsigned long)(xt_stack / 1000000ull),
            (unsigned long)(xt_old / 1000000ull));
#endif

    p->cr3       = new_pml4;
    p->owns_pml4 = true;

    /* Reset heap. */
    p->brk_base = USER_HEAP_BASE;
    p->brk_cur  = USER_HEAP_BASE;
    p->brk_max  = USER_HEAP_BASE + USER_HEAP_MAX_BYTES;

    /* B11: the new image hasn't registered a pthread-exit futex yet; drop any
     * clear_child_tid carried over from the replaced image. */
    p->clear_child_tid = 0;

    /* FD_CLOEXEC acts NOW -- past the commit, before the new image runs. */
    exec_close_cloexec_fds(p);

    /* Update name from the new path. */
    const char *base = kpath;
    for (const char *c = kpath; *c; c++) if (*c == '/') base = c + 1;
    size_t n = strlen(base);
    if (n >= PROC_NAME_MAX) n = PROC_NAME_MAX - 1;
    memcpy(p->name, base, n);
    p->name[n] = '\0';

    /* B20 (procfs): re-point /proc/<pid>/exe at the new image. */
    {
        size_t pn = strlen(kpath);
        if (pn >= ABI_PATH_MAX) pn = ABI_PATH_MAX - 1;
        memcpy(p->exe_path, kpath, pn);
        p->exe_path[pn] = '\0';
    }

    /* Set new entry point and stack. */
    p->user_entry = has_interp ? interp_info.entry : prog_info.entry;
    p->user_rsp   = user_rsp;

    /* Reset signals. */
    signal_init_proc(&p->sigstate);
    p->pending_signals = 0;

    kprintf("[execve] pid=%d now running '%s' entry=%p rsp=%p\n",
            p->pid, p->name, (void *)p->user_entry, (void *)p->user_rsp);

    /* This path enters ring 3 directly and never returns to syscall_dispatch,
     * so release the BKL here (execve was called from a syscall holding it).
     * Idempotent if already released. */
    bkl_exit();

    /* Jump to new program -- does not return. */
    proc_enter_user_asm(p->user_entry, p->user_rsp);
}
