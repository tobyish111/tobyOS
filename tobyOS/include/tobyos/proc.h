/* proc.h -- process abstraction (milestone 5).
 *
 * The kernel now multiplexes execution across a fixed-size process
 * table. Each entry (struct proc) describes a single thread of
 * execution + its address space. PID 0 is special: it represents the
 * boot/idle context that runs the shell -- it owns no PML4 (uses the
 * kernel PML4), no user stack, and is never reaped.
 *
 * Lifecycle of a user process:
 *
 *   proc_create_from_elf("/bin/hello", "hello")
 *       1. Allocate a PCB slot, assign a PID.
 *       2. Allocate a fresh user PML4 (kernel half mirrored from the
 *          kernel PML4; user half empty).
 *       3. Temporarily make that PML4 the active editing root, then
 *          elf_load_user(...) populates the user-half mappings, and
 *          a few stack pages are mapped right below the user-half top.
 *          Finally we restore the kernel PML4 as the editing root.
 *       4. Allocate a kernel stack and prime it as if a context
 *          switch had just pushed callee-saved regs + a return RIP
 *          pointing at proc_first_user_entry. Save the resulting RSP.
 *       5. Mark the proc PROC_READY and enqueue it on the scheduler.
 *
 *   sched_yield() (running proc -> next ready proc)
 *       Save current's RSP onto its kstack, restore next's RSP from
 *       its PCB, write CR3 = next's PML4, update TSS.RSP0 +
 *       g_kernel_syscall_rsp = next->kstack_top, ret. The new context
 *       resumes wherever it last context-switched -- or, for a
 *       brand-new proc, lands inside proc_first_user_entry which
 *       iretqs to ring 3.
 *
 *   proc_exit(code) (called from sys_exit or user-fault unwinder)
 *       Marks current TERMINATED, wakes the parent if any, sched_yields
 *       to whatever's ready next. Never returns.
 *
 *   proc_wait(pid) (called from the shell after spawning a child)
 *       Blocks the caller (typically pid 0) until `pid` terminates,
 *       then reaps the child (frees PML4 + user mappings + kstack +
 *       PCB slot) and returns its exit code.
 *
 * Concurrency: single CPU runs the scheduler (the BSP). APs idle.
 */

#ifndef TOBYOS_PROC_H
#define TOBYOS_PROC_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>
#include <tobyos/signal.h>

#define PROC_MAX        256           /* proc/thread slots. Chromium single-
                                       * process spawns ~30-50 threads (each a
                                       * proc slot); 64 (minus boot procs) ran
                                       * out -> clone ENOMEM -> pthread_create
                                       * EAGAIN -> NULL thread handle. */
#define PROC_NAME_MAX   32
#define PROC_KSTACK_SZ  (32 * 1024)   /* per-process kernel stack: 32 KiB */
#define PROC_NFDS       1024          /* per-proc file descriptor table size.
                                       * Must match the RLIMIT_NOFILE we report
                                       * (syscall.c) -- Chromium+Mojo+threads
                                       * share ONE table (CLONE_FILES) and open
                                       * many fds (epoll/socketpairs/timerfds);
                                       * 16 exhausted instantly -> EMFILE ->
                                       * message_pump_epoll CHECK. */
#define PROC_SANDBOX_MAX 128          /* max length of sandbox path prefix */

/* Thread-group fields. A "thread" is a lightweight proc sharing
 * the same address space (CR3, brk, fds) as its leader.
 * - tgid: the PID of the thread-group leader (== pid for leaders).
 * - is_thread: if true, this proc shares its leader's CR3/fds/brk
 *   and does NOT own the PML4 (owns_pml4 = false).
 * - tls_base: userland thread-local storage base (FS.base for this thread).
 * The maximum threads per process (including the leader) is bounded
 * only by PROC_MAX -- no per-process thread table. */
#define THREAD_MAX_PER_PROC  32

struct file;                          /* see <tobyos/file.h> */

enum proc_state {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_TERMINATED,
    /* Job control: stopped by SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU, resumed by
     * SIGCONT (or killed by SIGKILL). Never on a ready queue; the scheduler
     * treats it like BLOCKED (won't requeue on yield). Appended after
     * TERMINATED so existing state ordinals are unchanged. */
    PROC_STOPPED,
    /* Slice 109: slot claimed by an allocator (fork/clone/spawn) but still
     * being built. Every slot allocator used to leave state at PROC_UNUSED
     * for the whole build -- sys_fork's window is hundreds of ms under
     * cow_fork, and sys_clone_thread's scan would claim the SAME slot and
     * interleave writes with it (the multi-process chrome bootstrap flake:
     * fork child and live thread sharing one proc struct). Claimed by CAS
     * in proc_slot_claim(); to every reader an EMBRYO slot must look
     * exactly like PROC_UNUSED did mid-build: not a process. Appended so
     * existing state ordinals are unchanged. */
    PROC_EMBRYO,
};

struct proc {
    int             pid;
    /* Milestone 25A: parent pid (0 if spawned by the kernel/pid 0).
     * Inherited from current_proc() in spawn_internal so SYS_GETPPID
     * always has a sensible answer. */
    int             ppid;
    char            name[PROC_NAME_MAX];
    /* B20 (procfs): absolute path of the executable image this process
     * runs, set at spawn and re-set on execve. Backs /proc/<pid>/exe and
     * /proc/self/exe (the symlink real software uses to find its own
     * binary). Empty for kernel threads. */
    char            exe_path[ABI_PATH_MAX];
    enum proc_state state;
    int             exit_code;

    /* Track B (foreign-binary compat): which syscall ABI this process
     * speaks. ABI_PERS_TOBY (0, the zero-init default) routes `syscall`
     * through the native tobyOS dispatcher; ABI_PERS_LINUX routes it
     * through the Linux x86-64 translation layer. Latched at ELF load
     * from e_ident[EI_OSABI] and inherited across fork (CoW copies the
     * whole PCB) -- execve re-latches it from the new image. */
    int             personality;

    /* Address space. For pid 0 (kernel_main) cr3 == kernel PML4 and
     * owns_pml4 is false (we never destroy the kernel PML4). */
    uint64_t        cr3;
    bool            owns_pml4;

    /* Kernel stack: backs both the IRQ stack used on CPL3->0 transitions
     * (TSS.RSP0) and the syscall stack swap (g_kernel_syscall_rsp).
     * For pid 0 we keep kstack_base = NULL: it uses the bootloader-
     * provided stack which is already live, and we never free it. */
    void           *kstack_base;
    void           *kstack_top;
    /* Saved kernel SP across cooperative context switches. */
    uint64_t        saved_rsp;

    /* x87/SSE FPU state (FXSAVE image), saved/restored across context
     * switches now that SSE is enabled and user code (incl. printf %f) uses
     * XMM. 16-byte aligned as fxsave/fxrstor require. */
    uint8_t         fpu_state[512] __attribute__((aligned(16)));

    /* User stack (NULL/0 for kernel_main). */
    uint64_t        user_stack_base;
    size_t          user_stack_pages;

    /* Milestone 25A: per-process user heap (sbrk/brk style).
     *   brk_base = anchor; never moves after spawn.
     *   brk_cur  = current top of the user-visible heap (page-aligned
     *              when grown via SYS_BRK).
     *   brk_max  = hard ceiling beyond which SYS_BRK refuses to grow.
     * The heap lives at a fixed user-half region that does NOT collide
     * with the ELF (anchored at 0x400000) or the user stack (anchored
     * at the top of the user half). All four counts are 0 for pid 0
     * which uses the kernel heap. */
    uint64_t        brk_base;
    uint64_t        brk_cur;
    uint64_t        brk_max;

    /* Milestone 25A: per-process current working directory. Written
     * by SYS_CHDIR, read by SYS_GETCWD, used as a prefix when a
     * relative path is passed to SYS_OPEN / SYS_STAT / etc. Empty
     * string means "/". */
    char            cwd[ABI_PATH_MAX];

    /* User entry point + initial RSP -- read once by proc_first_user_entry
     * the first time this process is scheduled in. */
    uint64_t        user_entry;
    uint64_t        user_rsp;
    /* Thread argument: passed in RDI to the thread entry function.
     * Only meaningful for threads (is_thread == true) on first entry. */
    uint64_t        user_arg;

    /* Wait-for-child book-keeping. */
    int             wait_pid;       /* pid we're blocked on, or -1 */

    /* Singly-linked ready-queue node. NOTE: this is NOT a reliable "am I
     * queued?" test -- the TAIL of the queue always has next_ready == NULL, so
     * it is indistinguishable from an unqueued proc. Use on_rq for that. */
    struct proc    *next_ready;

    /* True while this proc is linked into some CPU's ready queue. Maintained
     * under that queue's ready_lock by queue_push/pop/remove_locked.
     *
     * sched_enqueue used to guard against double-enqueue with `next_ready !=
     * NULL`, which silently fails for the tail: enqueuing the current tail
     * again ran `cpu->ready_tail->next_ready = p` with ready_tail == p, i.e.
     * p->next_ready = p -- a SELF-CYCLE. queue_pop_locked then walked the list
     * forever and the whole system froze with no crash and no panic (the
     * heartbeat simply stopped). Chrome's GPU-process respawn loop churns
     * fork/reap fast enough to hit it reliably. */
    bool            on_rq;

    /* Slice 39: true from the moment do_switch commits to running this proc
     * until the NEXT task on that CPU observes the switch completed (i.e.
     * this proc's register context has been SAVED by proc_context_switch).
     * Pick paths (queue_pop/steal) skip on_cpu procs: a waker on another CPU
     * may legally sched_enqueue a proc that is still DESCENDING into its
     * block (state already PROC_BLOCKED, context not yet saved) -- switching
     * into it then would load a STALE saved_rsp, and the real context saved
     * moments later would never be re-enqueued. Observed as chrome's
     * pthread_create deadlock: FUTEX_WAKE returned 1 but the woken proc sat
     * RUNNING/off-queue forever (ledger slice 39). Cleared with a RELEASE
     * store by sched_finish_switch(); tested with ACQUIRE in the pickers. */
    volatile uint8_t on_cpu;

    /* Slice 88: stop-the-world for CoW fork. Sibling threads sharing this
     * proc's CR3 must leave user mode (and stay off-CPU) while vmm_cow_fork
     * strips PTE_WRITABLE; otherwise stale WRITABLE TLBs corrupt PartitionAlloc
     * freelists. vm_quiesce is set on siblings for the duration; vm_quiesced
     * marks ones we parked (READY/RUNNING) so tg_vm_resume can re-enqueue
     * them without stealing futex/pipe waiters. */
    volatile uint8_t vm_quiesce;
    uint8_t          vm_quiesced;

    /* Share-until-exec (vfork / chrome LaunchProcess): child borrows the
     * parent's CR3 + VMA table (mm_owner) with owns_pml4=false. vfork_parent
     * is the launcher pid; vfork_child on the parent is the child pid while
     * waiting (cleared when the child execve/_exits — avoids a lost-wake
     * if the child finishes before the parent blocks). */
    int              mm_owner;
    int              vfork_parent;
    int              vfork_child;
    uint64_t         vfork_stack_va;   /* private stack in shared CR3; 0=none */
    uint32_t         vfork_stack_pages;

    /* Generic per-proc wait-queue link. A blocked proc lives on at most
     * ONE such queue (e.g. a pipe's wq_read). The queue owner walks the
     * chain via this field on wakeup. */
    struct proc    *next_wait;
    /* Futex: absolute monotonic-ns deadline for a TIMED FUTEX_WAIT (0 = none/
     * infinite). Timed waiters now block ON the wait list (so FUTEX_WAKE can
     * wake them -- the old poll-only timed path could only be woken by a *uaddr
     * value change, so glibc condvar/mutex FUTEX_WAKE wakeups were silently
     * lost). A periodic sweep (futex_expire_timeouts) wakes waiters past their
     * deadline and sets futex_timed_out so the return is -ETIMEDOUT. */
    uint64_t        futex_deadline_ns;
    bool            futex_timed_out;
    /* Wake-latency instruments (slice 56): when this proc PARKED on a futex,
     * when the wake was DELIVERED (state->READY, by FUTEX_WAKE or the timeout
     * sweep), and which Linux syscall it is currently inside (cursys, -1 =
     * none) since cursys_ns. Splits "wake delivered late" from "woken but not
     * scheduled": act-vs-req overshoot with a small (now - fx_wake_ns) means
     * the WAKE was late; a large one means the READY proc sat unscheduled. */
    uint64_t        fx_park_ns;
    uint64_t        fx_wake_ns;
    int32_t         cursys;
    uint64_t        cursys_ns;
    /* Slice 64c: the NATIVE (tobyOS ABI) syscall this proc is inside, -1
     * when none. chromewin's GUI/blit calls are native, and BKL hold-time
     * attribution was blind to them (>99% of hold time landed in no
     * bucket until this existed). */
    int32_t         cursys_nat;
    /* Slice 70: sigaltstack(2) state. Recorded and reported back honestly;
     * signal delivery still runs handlers on the normal stack (see
     * lx_sigaltstack), which is what returning -ENOSYS used to force
     * anyway -- minus the error every crash handler logs. */
    uint64_t        sas_sp;
    uint64_t        sas_size;
    int32_t         sas_flags;
    /* CHROMIUM_BOOT diagnostic: latched at execve when argv carries
     * "--type=renderer", so the SIGKILL/exit stack-dump hook can fire on the
     * renderer (the process that must complete a document load for --dump-dom)
     * and skip the gpu/utility children. Behaviour-neutral. */
    bool            is_renderer;
    /* Back-pointer to the wait-queue HEAD this proc is currently parked
     * on (NULL when not blocked on any queue). signal_send() uses it to
     * splice the proc out of the queue when it forcibly wakes up a
     * BLOCKED process -- without this we'd corrupt the queue when the
     * woken proc was later reaped. Set by wq_add(), cleared by
     * wq_wake_all() and by signal_send()'s unlink path. */
    struct proc   **wait_head;

    /* Pending-signal bitmap (milestone 8). Each bit `1 << sig` means
     * `sig` has been delivered but not yet acted upon. Default action
     * for any pending signal is proc_exit(128+sig); checked at syscall
     * return, in the PIT IRQ if it interrupted ring 3, and inside
     * blocking primitives that return -EINTR. See signal.h. */
    uint32_t        pending_signals;

    /* Phase 1 M1.3: full POSIX signal state (handlers, mask, restorer) */
    struct signal_state sigstate;

    /* Linux slice 5: chroot(2) filesystem root. Empty == "/".
     *
     * DELIBERATELY SEPARATE from `sandbox_root` below, though the slice plan
     * suggested reusing it. They are different mechanisms that happen to look
     * alike:
     *   sandbox_root -- a CHECK:       "you may not name paths outside this"
     *   fs_root      -- a TRANSLATION: "the names you use are relative to this"
     * Folding chroot into sandbox_root would silently convert the existing cap
     * sandbox from a denial into a redirect (a `restricted`-profile app that
     * currently gets EACCES for /etc would start reading <root>/etc instead),
     * changing a live security boundary to save 64 bytes per proc. They also
     * compose: a process can be chrooted AND sandboxed.
     *
     * Applied in resolve_user_path() AFTER lexical cleaning, so a path full of
     * ".." can never climb out of the new root. */
    char            fs_root[PROC_SANDBOX_MAX];

    /* Linux slice 5: this process's MOUNT NAMESPACE (struct mount_ns, private
     * to vfs.c -- void* to avoid dragging the VFS internals into every file
     * that includes proc.h). NULL means "the initial namespace", which is what
     * every process uses today; slice 9's CLONE_NEWNS allocates a copy and
     * points here instead. Inherited across fork via the whole-PCB memcpy. */
    void           *mnt_ns;

    /* Phase 3 slice 8: the UTS and IPC namespaces, same shape as mnt_ns above
     * -- void* to a refcounted object owned by the subsystem that owns the
     * payload (`struct uts_ns` in nsproxy.c, `struct ipc_ns` in shm.c), with
     * NULL meaning "the initial namespace".
     *
     * NULL-means-initial is load-bearing, not laziness: it is why adding a
     * namespace type cannot change the behaviour of a system where nobody has
     * called unshare(2), and why boot allocates nothing.
     *
     * The whole-PCB memcpy in fork copies these pointers, so every copying
     * path MUST call nsproxy_fork_inherit() to take the matching references --
     * see nsproxy.h. Teardown goes through nsproxy_release(). */
    void           *uts_ns;
    void           *ipc_ns;

    /* Slice 10: the PID namespace this process is IN, and the one its next
     * CHILD will be placed in. Both `struct pid_ns *` (pid_ns.c).
     *
     * THESE TWO ARE NOT REDUNDANT. unshare(CLONE_NEWPID) is the one namespace
     * operation that does not move the caller -- a process cannot change its
     * own pid namespace, because its number there would have to appear from
     * nowhere while every existing holder of the old number kept using it. So
     * unshare sets only `pid_ns_for_children`, and the next fork's child lands
     * in the new namespace as its init while the parent stays put. Linux has
     * exactly this split; collapsing them is the easiest way to get
     * CLONE_NEWPID quietly wrong.
     *
     * `p->pid` REMAINS the host / initial-namespace number, always. Namespace
     * -local pids are a translation at the syscall boundary (see nsproxy.h) --
     * the same decision slice 2 made for uids, for the same reason. */
    void           *pid_ns;
    void           *pid_ns_for_children;

    /* Slice 11: the USER namespace (`struct user_ns *`, user_ns.c). NULL means
     * the initial one, as everywhere else.
     *
     * `uid`/`gid`/`ruid`/`suid`/... above REMAIN host ids (Linux's kuid_t) --
     * slice 2 committed to that in advance so this field could be added without
     * touching vfs_perm_check, cap_check_path, oom scoring, SO_PEERCRED and the
     * other ~30 consumers that compare them. Translation happens only at the
     * syscall boundary; see nsproxy.h. */
    void           *user_ns;

    /* Slice 12 (cut 1): the NETWORK namespace. NULL == initial.
     *
     * Sockets latch this at creation (sock_alloc) and enforcement asks the
     * SOCKET's namespace, not the process's -- so an unshare does not
     * retroactively unplug already-open sockets, which is Linux's behaviour and
     * is what lets a sandboxed child keep an inherited socketpair. */
    void           *net_ns;

    /* Slice 13: seccomp-bpf. `seccomp_filter` is a `struct seccomp_filter *`
     * (seccomp.c) and is a CHAIN -- installing a filter pushes onto it rather
     * than replacing, because every filter must keep running (the lowest return
     * wins, so a policy can only ever tighten).
     *
     * `no_new_privs` is prctl(PR_SET_NO_NEW_PRIVS): an unprivileged process must
     * set it before it may install a filter, because a filter can make a setuid
     * binary misbehave by failing syscalls it does not expect to fail. Both are
     * inherited across fork and SURVIVE execve -- a sandbox a child could shed
     * by exec'ing would not be a sandbox. */
    void           *seccomp_filter;
    uint8_t         seccomp_mode;
    uint8_t         no_new_privs;

    /* Slice 15: cgroup v2 membership -- an index into cgroup.c's table, 0 ==
     * the root cgroup (which is also the zero-init default, so every existing
     * process is in the root without any initialisation).
     *
     * An int rather than a pointer because cgroup v2 is ONE hierarchy with
     * exactly one membership per process, and an index makes the whole-PCB
     * memcpy in fork inherit it correctly for free. */
    int             cgroup;

    /* Slice 8: CLONE_NEW* bits STAGED by the clone(2)/clone3(2) arm for the
     * fork about to happen, consumed inside sys_fork/sys_fork_share and
     * cleared there.
     *
     * This looks like an odd way to pass an argument, and the obvious
     * alternative -- fork first, then fix up the child by pid -- is a RACE:
     * sys_fork sets the child PROC_READY and sched_enqueue()s it before
     * returning, so on an SMP boot another core can run the child before the
     * parent's syscall returns. The child would then observe the parent's
     * namespaces, which is precisely the isolation failure the whole feature
     * exists to prevent. Staging here lets the flags be applied while the child
     * is still PROC_EMBRYO and unreachable.
     *
     * Only ever written by the calling process to itself, so no lock is
     * needed. */
    uint32_t        clone_ns_flags;

    /* clone(2)'s `child_stack` argument (a2), staged by the same arm and for
     * the same reason as clone_ns_flags above: it has to be installed into the
     * child's saved user register block while the child is still PROC_EMBRYO.
     * 0 == "not supplied" (fork semantics: the child resumes on the parent's
     * stack), which is also the zero-init default.
     *
     * This existed as a hole until the Chromium sandbox found it. `clone` has
     * TWO callers with different shapes and only one of them was tested:
     *
     *   syscall(SYS_clone, flags, NULL, ...)  -- the raw syscall, NULL stack.
     *      The child resumes on the parent's stack, exactly like fork(2).
     *      This is what programs/linux-clonens exercises, and it always worked.
     *
     *   clone(fn, child_stack, flags, arg)    -- the glibc LIBRARY function.
     *      __clone stores fn/arg at the TOP of child_stack, passes the adjusted
     *      pointer as the syscall's a2, and the child-side of the wrapper does
     *      `xor %ebp,%ebp; pop %rax; pop %rdi; call *%rax`. If a2 is ignored,
     *      that pop reads the PARENT's stack: %rax gets the return address of
     *      the `call clone@plt`, so the child "calls" back into the middle of
     *      its parent's caller with rbp == 0 and dies on the first frame-
     *      relative access. Chromium's sandbox does exactly this at
     *      credentials.cc, which is why its child died at cr2 = -0x28 (the
     *      stack-canary reload `cmp -0x28(%rbp),%rcx`) with syscalls=0. */
    uint64_t        clone_child_stack;

    /* ---- ptrace(2) ------------------------------------------------------
     * tracer_pid == 0 means untraced, the zero-init default.
     *
     * The stop reuses PROC_STOPPED, the job-control state SIGSTOP already
     * uses: to the scheduler a ptrace-stop and a job-control stop are the
     * same thing (off every run queue, not requeued on yield), and
     * ptrace_stopped is what distinguishes them for wait4. */
    int             tracer_pid;
    uint32_t        ptrace_opts;
    uint8_t         ptrace_stopped;
    uint8_t         ptrace_syscall;
    uint8_t         ptrace_atexit;
    int             ptrace_stopsig;
    long            ptrace_orig_rax;
    long            ptrace_retval;

    /* Linux slice 3: alarm(2) deadline, absolute monotonic ns; 0 == no alarm.
     * Checked by signal_check_alarms() on the timer tick, which raises SIGALRM
     * and clears it. Deliberately NOT a general timer subsystem -- one
     * deadline per process is exactly what alarm(2) offers, and setitimer's
     * richer shape can grow from here if something needs it. */
    uint64_t        alarm_deadline_ns;
    /* Slice 128: a REAL timed sleep. sys_nanosleep used to pause-spin with a
     * sched_yield() per iteration until the deadline, so a sleeping process
     * never actually slept -- it burned a core's worth of scheduling slots
     * and BKL round-trips for the whole duration. Measured on the EliteDesk:
     * chromewin, whose main loop is one usleep(15000) per pass, took 34% CPU
     * doing nothing while chrome itself got 8.7% and the browser rendered at
     * ~3 fps. Non-zero means "PROC_BLOCKED until perf_now_ns() reaches this";
     * the sweep in sched_yield wakes it. */
    uint64_t        sleep_deadline_ns;

    /* Per-process file descriptor table. Slot indices that fit in a
     * struct file pointer are owning -- close-on-exit drops them. */
    struct file    *fds[PROC_NFDS];

    /* Close-on-exec bitmap, one bit per fd slot (Linux FD_CLOEXEC). A
     * property of the DESCRIPTOR, not the open file description -- dup()
     * clears it on the new fd, fcntl(F_SETFD) flips it, and a successful
     * execve closes every marked fd before the new image runs. Lives
     * beside fds[] so fork's whole-PCB memcpy inherits it for free, and
     * threads reach the LEADER's copy through the same accessor
     * indirection as proc_fds() -- a per-thread copy would let one
     * thread's F_SETFD silently diverge from its siblings. Before this
     * existed every creator dropped O_CLOEXEC/SOCK_CLOEXEC on the floor
     * and every descriptor leaked across exec (2026-08-22 audit). */
    uint8_t         fd_cloexec[PROC_NFDS / 8];

    /* Session tag (milestone 14). 0 means "no user session" (kernel
     * thread / pre-login boot procs). On every spawn we copy this
     * field from the parent (current_proc()->session_id) so children
     * inherit ownership, and session_logout() walks the table looking
     * for matching tags to SIGTERM. The desktop launch queue
     * temporarily flips pid 0's tag to the active session id around
     * the spawn so apps launched from the desktop end up in the
     * correct session. */
    int             session_id;
    /* POSIX process group (job control). Inherited from the parent at
     * spawn; a parentless/kernel proc leads its own group. setpgid moves
     * it, kill(-pgid) fans out over it, and the tty delivers foreground
     * signals to the whole group. 0 means "never set" and is read as
     * "own pid" everywhere, so pre-existing code paths stay valid. */
    int             pgid;
    /* Job-control stop reporting (WUNTRACED): which signal stopped this
     * proc, and whether a wait has already reported that stop. Set when a
     * stop signal's default action runs, cleared by SIGCONT. */
    int             stop_sig;
    bool            stop_reported;
    /* WCONTINUED: a SIGCONT resumed this proc from a stop and no wait has
     * reported the continue yet. */
    bool            cont_pending;

    /* User identity (milestone 15). Inherited from the parent in
     * spawn_internal -- so a kernel-spawned proc starts as uid 0/gid 0
     * (pid 0 is root), and the desktop launcher temporarily flips
     * pid 0's uid/gid to the logged-in user around the spawn so apps
     * run as the user. The VFS uses ->uid to enforce file permissions;
     * uid 0 (root) bypasses every check. */
    int             uid;
    int             gid;

    /* ---- Linux slice 2: the full POSIX credential set ------------------
     *
     * `uid`/`gid` above remain the EFFECTIVE ids. That is deliberate: every
     * existing consumer (vfs_perm_check, cap_check_path, sysprot, uac, oom
     * scoring, SO_PEERCRED, siginfo si_uid) already means "effective" when it
     * says p->uid, so widening in place would have silently changed ~30 call
     * sites in a tree with no header-dependency tracking. Effective stays put;
     * the rest of the triple lives here.
     *
     * ruid/rgid  -- real: who launched us. Restored-to by setuid(real).
     * suid/sgid  -- saved-set: lets a setuid program drop privilege
     *               temporarily and regain it (POSIX saved-set-user-ID).
     *
     * NAMESPACE NOTE (slice 11): these are HOST/initial-namespace ids, the
     * equivalent of Linux's kuid_t. A user namespace does NOT rewrite them --
     * it interposes a uid_map at the REPORTING boundary (getuid, stat, the
     * per-pid procfs status file) and at ns entry. Keeping the stored ids
     * global is what makes that possible without touching every check. */
    int             ruid, rgid;
    int             suid, sgid;

    /* Supplementary groups. Linux allows 65536; 32 covers every real use and
     * keeps struct proc (256 slots) from growing by 64 KiB for nothing.
     * ngroups == 0 is the correct answer for a process in no extra groups,
     * which is what getgroups(2) reported before this slice. */
#define PROC_NGROUPS_MAX 32
    int             ngroups;
    int             groups[PROC_NGROUPS_MAX];

    /* Linux capability bits (CAP_SYS_ADMIN, CAP_NET_RAW, ...), a 64-bit mask
     * per set. DELIBERATELY SEPARATE from `caps` below, which is tobyOS's own
     * sandbox-profile mask (CAP_FILE_READ/NET/GUI/...). The two share a prefix
     * and nothing else: tobyOS caps are local policy, these are the Linux ABI
     * that capget/capset and (slice 11) user namespaces are defined in terms
     * of. Conflating them would make both wrong. */
    uint64_t        lcap_eff, lcap_perm, lcap_inh;

    /* File-creation mask (Linux slice 1). POSIX: bits SET here are CLEARED
     * from the mode of any file/directory this process creates. Inherited
     * across fork AND preserved across execve (unlike most process state),
     * which is why it lives in the PCB rather than being derived.
     *
     * 0022 is the conventional default. Note the shell has long had its own
     * `umask` builtin, but it was shell-local bookkeeping with nothing behind
     * it -- this is the field that actually reaches file creation. */
    uint32_t        umask;

    /* Capability mask + path-sandbox root (milestone 18). `caps` is a
     * bitwise-OR of CAP_* (see <tobyos/cap.h>). pid 0 is initialised
     * with CAP_GROUP_ALL (includes ADMIN) so the kernel bypasses
     * every check. A child inherits from the parent in spawn_internal
     * and then, if the spec provides a sandbox_profile, has its caps
     * narrowed via cap_profile_apply().
     *
     * `sandbox_root` is an empty string for "no path jail", or an
     * absolute path like "/data/sandbox" that every VFS call must
     * begin with. cap_check_path enforces it. */
    uint32_t        caps;
    char            sandbox_root[PROC_SANDBOX_MAX];

    /* Milestone 34E: depth of the active sysprot privileged scope.
     * Zero outside any scope (the default). Bumped by
     * sysprot_priv_begin, decremented by sysprot_priv_end. While > 0
     * the proc is allowed to mutate paths under the protected-prefix
     * table even though it would otherwise be denied. The counter is
     * never serialised across exec / spawn -- a child starts at 0
     * even if its parent is mid-scope. */
    uint32_t        sysprot_priv;

    /* ---- Threading (Phase 1 milestone M1.1) -------------------------
     *
     * tgid: thread-group ID (== pid of the leader). For the leader
     *       itself, tgid == pid. For spawned threads, tgid == leader's
     *       pid. Checked by waitpid/kill to operate on the whole group.
     *
     * is_thread: if true this proc is a non-leader thread sharing its
     *       leader's address space. On exit it does NOT free the PML4
     *       or the fd table -- only the leader does that.
     *
     * tls_base: user-visible FS segment base for this thread's TLS area.
     *       Written by SYS_SET_TLS, loaded into MSR_FS_BASE on context
     *       switch so each thread sees its own __thread variables.
     *
     * gs_base: user-visible GS base for this proc. 0 for native/Linux
     *       procs (which never use GS); for a Windows PE (ABI_PERS_WIN32)
     *       it points at the proc's TEB so the CRT's gs:[0x30]
     *       (NtCurrentTeb) works. Loaded into the IA32_KERNEL_GS_BASE
     *       shadow on context switch; SWAPGS restores it on the way to
     *       CPL3. See do_switch() + syscall_entry.S / isr_stubs.S.
     *
     * join_waiters: linked list of procs blocked on SYS_THREAD_JOIN
     *       for this specific thread. Woken by thread_exit().
     *
     * detached: if true, nobody will join this thread -- reap immediately
     *       on exit.
     */
    int             tgid;
    bool            is_thread;
    bool            detached;
    /* True for per-CPU idle procs (pid 0 on the BSP, ap_idle on APs). The
     * scheduler never steals an idle proc onto another CPU. */
    bool            is_idle;
    /* Ring-0 worker created by proc_create_kernel(): user_entry is a
     * kernel function pointer; proc_first_user_entry CALLs it instead
     * of iret-ing to ring 3 (stage 12D async-HTTP worker). */
    bool            is_kernel;
    /* Opt-in: tcp_poll_until waits sched_yield() instead of hlt(), so
     * a long transfer on a kernel worker shares its CPU instead of
     * parking it. Only the async-HTTP worker sets this. */
    bool            tcp_yield_wait;
    uint64_t        tls_base;
    /* B11: Linux pthread_join support. A user VA recorded via clone(
     * CLONE_CHILD_CLEARTID) or set_tid_address(): on this thread's exit the
     * kernel writes 0 there and FUTEX_WAKEs it, which is exactly what glibc/
     * musl pthread_join blocks on (futex FUTEX_WAIT on &thread->tid). 0 = unset. */
    uint64_t        clear_child_tid;
    uint64_t        gs_base;     /* Win32 TEB base (0 if not a PE); see above */
    /* Win32 CRT heap (Track C): a bump allocator over sys_mmap'd anonymous
     * user memory, backing the kernel32/ucrt malloc/calloc shims. Lazily
     * created on first malloc; never reclaims (free() is a no-op) -- fine for
     * a short-lived process. Both 0 until first use. */
    uint64_t        win_heap_cur;
    uint64_t        win_heap_end;
    /* C14: PE image base + resource directory (.rsrc), for DialogBoxParamA to
     * walk RT_DIALOG templates. All 0 for non-PE / no-resource processes. */
    uint64_t        win_image_base;
    uint64_t        win_rsrc_rva;
    uint64_t        win_rsrc_size;
    /* C18b: .tls template, so CreateThread can give each thread its own block. */
    uint64_t        win_tls_raw_va;
    uint64_t        win_tls_raw_size;
    uint64_t        win_tls_total;
    uint64_t        win_tls_index;
    struct proc    *join_waiters;

    /* ---- Priority scheduling (gap #3: priority classes + timeslicing) ----
     *
     * `prio` is the scheduling priority: HIGHER runs first. 0 == NORMAL,
     * which is exactly what a zero-initialised PCB gets for free, so every
     * spawn/fork/thread path defaults to NORMAL without extra code. Negative
     * values are background classes, positive are foreground/elevated; the
     * setpriority syscall clamps to [PRIO_MIN, PRIO_MAX] (see <tobyos/sched.h>).
     *
     * `quantum_left` is the number of LAPIC-timer ticks remaining in this
     * proc's current timeslice. It's reset to the proc's class quantum on
     * switch-in and decremented by the per-CPU timer ISR; reaching 0 while the
     * timer interrupts ring 3 forces a preemptive yield -- this is what gives
     * CPU-bound procs fair timeslicing on the APs (which have no PIT).
     *
     * `enq_tick` is the global tick at which this proc was last made READY and
     * pushed onto a ready queue. The dispatcher ages a long-waiting proc's
     * *effective* priority upward from this stamp so a busy high-priority proc
     * can never starve a low-priority one. */
    int             prio;
    int32_t         quantum_left;
    uint64_t        enq_tick;
    /* Slice 94: perf_now_ns() stamp of the last enqueue, consumed by
     * do_switch to measure wake->run scheduler latency ([wlat] in the deep
     * dump). 0 = no pending stamp. Struct grew: CLEAN BUILD required. */
    uint64_t        enq_ns;

    /* `io_boost` is a transient interactivity bonus added to the effective
     * priority (MLFQ-style). A proc that gives up the CPU by BLOCKING before
     * its quantum is spent -- the signature of an I/O-bound / interactive
     * task (waiting on a pipe, socket, key, sleep) -- earns the boost so it
     * preempts CPU-bound work and stays responsive on wakeup. A proc that
     * burns a whole quantum (CPU-bound) has the boost cleared. Bounded to one
     * priority class so it can't starve anything; aging still applies on top. */
    int             io_boost;

    /* ---- Milestone 19: performance metrics ---------------------
     *
     * `cpu_ns` accumulates wallclock time this proc has spent
     * RUNNING (not just runnable -- we only tick it across a real
     * context switch). `last_switch_tsc` is the TSC sample the
     * scheduler took when it context-switched INTO this proc;
     * sched_yield uses it as the "from" end of the delta. Zero
     * means "not currently running".
     *
     * `syscall_count` and `user_pages` are updated in-place by
     * syscall.c / proc.c. `created_ns` is the perf_now_ns() stamp
     * at spawn (0 for pid 0 which exists before perf_init).
     *
     * These are purely diagnostic; nothing in the kernel's control
     * flow consults them. They exist so `ps`, `top`, `time`, and
     * `perf` can show something interesting. */
    uint64_t        cpu_ns;
    uint64_t        last_switch_tsc;
    uint64_t        syscall_count;
    uint64_t        created_ns;
    uint64_t        user_pages;

    /* Page-fault forensics (isr.c). `fault_count` and the last resolved
     * fault's rip/cr2 let a freeze diagnosis distinguish a repeating-
     * fault livelock (fault_count RACES between two F1 dumps -> the rip
     * names the stuck instruction) from a pure userspace spin
     * (fault_count static while state stays RUN). Diagnostic only. */
    uint64_t        fault_count;
    uint64_t        last_fault_rip;
    uint64_t        last_fault_cr2;
};

/* ---- Milestone 19: time a child's exit --------------------------- *
 *
 * proc_wait() returns only the exit code, and by then the child's
 * PCB has been freed. The `time` shell builtin needs to learn how
 * much CPU the child consumed BEFORE the reap erases it, so the
 * full-form wait variant captures the metrics first and fills an
 * out-struct. */
struct proc_exit_info {
    int      pid;
    int      exit_code;
    uint64_t cpu_ns;
    uint64_t syscall_count;
    uint64_t wall_ns;       /* perf_now_ns() delta from proc_wait_info() call */
    char     name[PROC_NAME_MAX];
};

/* Same contract as proc_wait() but fills *out with a snapshot taken
 * right before the child is reaped. out may be NULL (then it's a
 * plain proc_wait). Returns exit_code or -1. */
int proc_wait_info(int pid, struct proc_exit_info *out);

/* Globals (defined in proc.c). */
extern struct proc *g_current_proc;     /* last proc switched-to on any CPU */

/* The process running on the CALLING CPU. Multi-core: this is per-CPU
 * (g_percpu[this_cpu].current). Defined out-of-line in proc.c so this header
 * needn't pull in the SMP/percpu machinery. Falls back to g_current_proc in
 * the brief early-boot window before per-CPU `current` is populated. */
struct proc *current_proc(void);

/* Build/return a secondary CPU's idle process (kernel address space, runs on
 * the AP boot stack). Set as that CPU's current+idle so the scheduler can
 * switch user work in and out on the AP. */
struct proc *proc_ap_idle(uint32_t cpu, uint64_t kstack_top);

/* Build pid 0 from the live boot context (the caller IS pid 0 after
 * this returns). Must be called once, after the kernel PML4 + heap +
 * TSS are up and before any other process is created. */
void proc_init(void);

/* Create a new ring-3 process from an ELF on the VFS. Returns the
 * newly-assigned PID (>= 1) on success, or -1 on any failure (no PCB
 * slot, PMM OOM, no such file, bad ELF). The new process is left in
 * PROC_READY and enqueued on the scheduler -- it'll run the next time
 * the caller yields. */
int proc_create_from_elf(const char *path, const char *name);

/* Create a ring-0 kernel worker process: runs `entry()` on its own
 * kernel stack in the kernel address space, scheduled like any other
 * proc. `entry` must never return unless it intends to proc_exit(0);
 * it must follow the BKL discipline (hold it while touching shared
 * kernel state, release it around sched_yield / long waits). Returns
 * the pid, or -1 on failure. (Stage 12D: the async-HTTP worker.) */
int proc_create_kernel(void (*entry)(void), const char *name);

/* Full-form spawn: explicit fds for stdin/stdout/stderr (NULL == default
 * to a fresh console-backed file), and an argv vector packed onto the
 * new process's user stack. argv strings are owned by the caller; we
 * copy them.
 *
 * fd0/fd1/fd2 are inherited via file_clone() -- the caller keeps its
 * own ref and must file_close() it separately. Returns the new pid on
 * success, or -1 on failure. */
/* A descriptor to install in the child at a CHOSEN number. fd0..fd2 (and the
 * fd3/fd4 chrome preopens) cover the fixed slots; this covers the rest, which
 * is what a shell needs for `exec 3>file` and `cmd 8<<EOF`. */
struct proc_fd_map {
    int          fd;            /* number in the CHILD */
    struct file *f;             /* caller keeps ownership; this is cloned */
};

struct proc_spec {
    const char  *path;
    const char  *name;          /* may be NULL -> derived from path */
    struct file *fd0;           /* may be NULL -> console */
    struct file *fd1;
    struct file *fd2;
    /* Slice 39: optional fd 3/4 preopens (NULL = not installed). Chrome's
     * --remote-debugging-pipe convention: the browser reads DevTools JSON
     * commands on fd 3 and writes responses/events on fd 4. */
    struct file *fd3;
    struct file *fd4;
    /* Descriptors at arbitrary numbers. NULL/0 = none, which is what a
     * zero-initialised proc_spec gives, so every existing caller is
     * unaffected. */
    const struct proc_fd_map *extra_fds;
    int                       extra_nfds;
    int          argc;
    char       **argv;          /* argc strings, each NUL-terminated */
    /* Milestone 25C: optional environment vector. NULL = inherit from
     * parent untouched. NULL-terminated array of "KEY=VALUE" strings.
     * The kernel deep-copies these onto the child's user stack right
     * above argv (see pack_argv_envp_on_user_stack). */
    int          envc;
    char       **envp;
    /* Optional sandbox profile name (milestone 18). NULL = inherit the
     * parent's caps untouched. Non-NULL = after inheritance, narrow
     * the child's caps via cap_profile_apply(). Unknown names are
     * logged and treated as NULL (no-op) so a typo never elevates. */
    const char  *sandbox_profile;
    /* Optional M34D declared-capability list, comma-separated cap
     * names (e.g. "FILE_READ,GUI"). Applied AFTER sandbox_profile so
     * the child ends up with (parent & profile & declared) -- pure
     * narrowing, never widens. NULL or "" = no extra narrowing. */
    const char  *declared_caps;
    /* Milestone 25A: optional override for the child's initial working
     * directory. NULL or "" = inherit from parent (default). Otherwise
     * an absolute path that becomes the child's `cwd`. */
    const char  *cwd;
};
int proc_spawn(const struct proc_spec *spec);

/* Milestone 25A: grow the calling process's per-proc heap so the
 * range [old_brk, new_brk) is mapped writable user-half memory. If
 * `new_brk == 0` the call only reports the current brk. Returns the
 * new (or unchanged) brk on success, or 0 on failure (brk_max reached
 * or PMM out of memory). Always page-aligns up. */
uint64_t proc_brk(struct proc *p, uint64_t new_brk);

/* Mark `current` as PROC_TERMINATED, set its exit code, wake any
 * parent blocked on us, and sched_yield to the next ready proc. Never
 * returns. Closes every open fd before yielding so any pipe ends drop
 * their reader/writer count immediately (rather than at reap time). */
__attribute__((noreturn)) void proc_exit(int code);

/* Spin until `p` is no longer live-on-CPU (on_cpu cleared). Call after
 * sched_dequeue / force-TERMINATED and BEFORE freeing kstack or zeroing
 * kstack_top -- otherwise another CPU can switch into a freed stack. */
void proc_wait_off_cpu(struct proc *p);
/* Slice 89: bounded variant for group teardown; false = never left CPU. */
bool proc_wait_off_cpu_bounded(struct proc *p);

/* Soft stop-the-world for the shared-CR3 thread group around vmm_cow_fork.
 * Defined in fork.c. Actor must be the forking thread; may briefly drop BKL. */
void tg_vm_quiesce(struct proc *actor);
void tg_vm_resume(struct proc *actor);
/* Slice 92: park an on-CPU proc that observed vm_quiesce from a kernel #PF.
 * Drops the BKL while spinning (holding it deadlocked the whole kernel
 * against the forker's mmap_cow_clone bkl_enter -- the -smp 4 freeze). */
void vm_quiesce_park_oncpu(struct proc *p);

/* VMA-table key for `p` (tgid for threads; mm_owner for share-until-exec). */
static inline int proc_mm_pid(const struct proc *p) {
    if (!p) return 0;
    if (p->mm_owner > 0) return p->mm_owner;
    return p->is_thread ? p->tgid : p->pid;
}

/* Wake a parent blocked in sys_fork_share once the child execve/_exits. */
void vfork_child_done(struct proc *child);

/* Share address space until exec/exit — no CoW write-protect of the parent. */
long sys_fork_share(void);

/* Linux exit_group: tear down the entire thread group (not just this thread). */
__attribute__((noreturn)) void proc_exit_group(int code);

/* Block the caller until the process with PID `pid` reaches
 * PROC_TERMINATED, then reap it (free PML4 + kstack + slot) and
 * return its exit code. Returns -1 if `pid` isn't found or the caller
 * tries to wait on itself. */
int proc_wait(int pid);

/* User stack geometry. The stack always TOPS OUT at the end of the user half
 * (both the ELF and PE execve arms anchor there) and grows DOWN on demand; the
 * fault handler expands it a page at a time (page_fault.c) as far as
 * USER_STACK_FLOOR_VA, which mirrors Linux's 8 MiB default RLIMIT_STACK.
 * execve maps only a few pages eagerly -- everything below is demand-grown, so
 * a process that never recurses deeply never pays for 8 MiB. */
#define USER_STACK_TOP_LIMIT_VA  0x0000800000000000ULL
#define USER_STACK_MAX_BYTES     (8ULL * 1024ULL * 1024ULL)
#define USER_STACK_FLOOR_VA      (USER_STACK_TOP_LIMIT_VA - USER_STACK_MAX_BYTES)

/* Look up a PCB by PID. Returns NULL if not found or slot is UNUSED/EMBRYO. */
struct proc *proc_lookup(int pid);

/* Assign PID's process group (kernel-trusted; the checked path is the
 * setpgid syscall). The shell's `set -m` uses this through a host seam:
 * kernel build calls it directly, /bin/tsh's host.c maps it to setpgid(2). */
int proc_set_pgid(int pid, int pgid);

/* Kernel-side WUNTRACED-style foreground wait: returns the exit code, or
 * 0 with *stop_sig set when the child job-control-stopped. The KERNEL
 * shell's monitor path uses this; /bin/tsh's host.c provides the same
 * signature over waitpid(WUNTRACED). */
int proc_wait_fg(int pid, int *stop_sig);

/* Atomically claim a free proc slot (state CAS UNUSED -> EMBRYO). The ONLY
 * way to allocate a slot -- a plain "scan for UNUSED" race is exactly the
 * chrome bootstrap flake. Release on failure by resetting state to
 * PROC_UNUSED after clearing the fields the builder touched. */
struct proc *proc_slot_claim(void);

/* Zero a claimed slot WITHOUT ever exposing state==PROC_UNUSED (a bare
 * memset would, letting a concurrent claimer steal the slot mid-build). */
void proc_slot_wipe(struct proc *p);

/* The fd table that `p` actually USES. For a CLONE_FILES thread this is the
 * thread-group leader's table, because a thread's own fds[] is deliberately left
 * empty (see sys_clone_thread). Anything that reads a process's descriptors must
 * go through here rather than touching p->fds directly -- reading p->fds on a
 * thread silently yields an EMPTY table. Defined in syscall.c. */
struct file **proc_fds(struct proc *p);

/* Close-on-exec bitmap accessors (syscall.c). Thread-aware: route to the
 * leader's bitmap exactly as proc_fds routes to the leader's table. */
void fd_cloexec_set(struct proc *p, int fd, int on);
int  fd_cloexec_get(struct proc *p, int fd);

/* Find a child of `ppid` (for Linux wait4(-1)); prefers a TERMINATED child.
 * Returns the child pid, or -1 if there are no children. */
int proc_any_child(int ppid);

/* For diagnostics. */
const char *proc_state_name(enum proc_state s);
void        proc_dump_table(void);

/* C-level first-time entry trampoline -- only called by the asm context
 * switch via the fake initial frame proc_create installed. Reads
 * current's user_entry/user_rsp and iretqs to ring 3. */
__attribute__((noreturn)) void proc_first_user_entry(void);

/* ---- Thread API (Phase 1 M1.1) ------------------------------------ */

/* Create a new thread in the calling process's thread group. The new
 * thread shares the leader's address space, fds, brk, caps, and cwd.
 * It gets its own kernel stack, user stack (at `user_stack_top`), and
 * starts execution at `entry(arg)`. Returns the new thread's TID (== pid)
 * on success, or -1 on failure. */
int thread_create(uint64_t entry, uint64_t arg,
                  uint64_t user_stack_top, uint64_t tls_base);

/* Exit the calling thread. If the caller is the last thread in the
 * group, the entire process exits with `code`. Otherwise only this
 * thread is terminated and its resources freed. */
__attribute__((noreturn)) void thread_exit(int code);

/* Block until thread `tid` exits. Stores its exit code in *out_code
 * (if non-NULL). Returns 0 on success, -1 if tid not found or not
 * in the same thread group as the caller. */
int thread_join(int tid, int *out_code);

/* Detach thread `tid` so it auto-reaps on exit. */
int thread_detach(int tid);

/* Futex: atomic wait/wake on a userland address.
 *   op=0 (FUTEX_WAIT): if *uaddr == expected, block until woken.
 *   op=1 (FUTEX_WAKE): wake up to `val` waiters on uaddr.
 * Returns number of waiters woken (WAKE) or 0/-EAGAIN (WAIT). */
#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
/* Slice 59e: glibc's pthread_cond_broadcast requeues waiters from the condvar
 * word to the mutex word with these. They used to fall through to -EINVAL, so
 * a broadcast woke NOBODY and its waiters parked on an infinite futex forever
 * (measured: the chrome renderer's main thread). See futex() in thread.c. */
#define FUTEX_REQUEUE      3
#define FUTEX_CMP_REQUEUE  4
#define FUTEX_LOCK_PI      6   /* pthread PI mutex; stub without true inheritance */
#define FUTEX_UNLOCK_PI    7
#define FUTEX_TRYLOCK_PI   8
#define FUTEX_WAIT_BITSET  9   /* like WAIT but ABSOLUTE timeout (glibc timedwait) */
#define FUTEX_WAKE_BITSET  10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
/* PI-futex word bits (Linux ABI). */
#define FUTEX_WAITERS      0x80000000u
#define FUTEX_OWNER_DIED   0x40000000u
#define FUTEX_TID_MASK     0x3fffffffu
/* utimeout: user pointer to a struct timespec, or NULL for an infinite wait.
 * For FUTEX_WAIT it is a RELATIVE timeout; for FUTEX_WAIT_BITSET, ABSOLUTE.
 * uaddr2: required for WAIT_REQUEUE_PI / CMP_REQUEUE*_PI (PI mutex word). */
long futex(uint32_t *uaddr, int op, uint32_t val, const void *utimeout,
           uint32_t *uaddr2);

/* Initialize the futex hash table. Call once during boot. */
void futex_init(void);

/* Blocking wait for poll/select/epoll_wait (slice 43). poll_wait_block()
 * parks the caller (leaves the ready queue) until poll_wake_all() requeues it;
 * poll_tick() is a rate-limited wake-all driven from the scheduler yield slow
 * path + pid 0's idle loop; poll_forget_proc() unlinks a dying thread in
 * teardown. poll_init() sets up the wait list. */
void poll_init(void);
void poll_wait_block(void);
void poll_wake_all(void);
void poll_tick(void);
/* Slice 67: called by paths that make an fd ready (pipe data/close, socket
 * receive enqueue, peer shutdown) so parked poll/select/epoll waiters are
 * woken by EVENTS instead of a 1ms timer sweep. BKL-held, coalesced. */
void poll_event_notify(void);
void poll_forget_proc(struct proc *p);

/* Set the calling thread's TLS base (loaded into FS.base). */
void thread_set_tls(uint64_t base);

#endif /* TOBYOS_PROC_H */
