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

#define PROC_MAX        16
#define PROC_NAME_MAX   32
#define PROC_KSTACK_SZ  (32 * 1024)   /* per-process kernel stack: 32 KiB */
#define PROC_NFDS       16            /* per-proc file descriptor table size */
#define PROC_SANDBOX_MAX 128          /* max length of sandbox path prefix */

struct file;                          /* see <tobyos/file.h> */

enum proc_state {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_TERMINATED,
};

struct proc {
    int             pid;
    /* Milestone 25A: parent pid (0 if spawned by the kernel/pid 0).
     * Inherited from current_proc() in spawn_internal so SYS_GETPPID
     * always has a sensible answer. */
    int             ppid;
    char            name[PROC_NAME_MAX];
    enum proc_state state;
    int             exit_code;

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

    /* Wait-for-child book-keeping. */
    int             wait_pid;       /* pid we're blocked on, or -1 */

    /* Singly-linked ready-queue node (NULL when not on the queue). */
    struct proc    *next_ready;

    /* Generic per-proc wait-queue link. A blocked proc lives on at most
     * ONE such queue (e.g. a pipe's wq_read). The queue owner walks the
     * chain via this field on wakeup. */
    struct proc    *next_wait;
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

    /* Per-process file descriptor table. Slot indices that fit in a
     * struct file pointer are owning -- close-on-exit drops them. */
    struct file    *fds[PROC_NFDS];

    /* Session tag (milestone 14). 0 means "no user session" (kernel
     * thread / pre-login boot procs). On every spawn we copy this
     * field from the parent (current_proc()->session_id) so children
     * inherit ownership, and session_logout() walks the table looking
     * for matching tags to SIGTERM. The desktop launch queue
     * temporarily flips pid 0's tag to the active session id around
     * the spawn so apps launched from the desktop end up in the
     * correct session. */
    int             session_id;

    /* User identity (milestone 15). Inherited from the parent in
     * spawn_internal -- so a kernel-spawned proc starts as uid 0/gid 0
     * (pid 0 is root), and the desktop launcher temporarily flips
     * pid 0's uid/gid to the logged-in user around the spawn so apps
     * run as the user. The VFS uses ->uid to enforce file permissions;
     * uid 0 (root) bypasses every check. */
    int             uid;
    int             gid;

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
extern struct proc *g_current_proc;     /* always non-NULL after proc_init */

static inline struct proc *current_proc(void) { return g_current_proc; }

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

/* Full-form spawn: explicit fds for stdin/stdout/stderr (NULL == default
 * to a fresh console-backed file), and an argv vector packed onto the
 * new process's user stack. argv strings are owned by the caller; we
 * copy them.
 *
 * fd0/fd1/fd2 are inherited via file_clone() -- the caller keeps its
 * own ref and must file_close() it separately. Returns the new pid on
 * success, or -1 on failure. */
struct proc_spec {
    const char  *path;
    const char  *name;          /* may be NULL -> derived from path */
    struct file *fd0;           /* may be NULL -> console */
    struct file *fd1;
    struct file *fd2;
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

/* Block the caller until the process with PID `pid` reaches
 * PROC_TERMINATED, then reap it (free PML4 + kstack + slot) and
 * return its exit code. Returns -1 if `pid` isn't found or the caller
 * tries to wait on itself. */
int proc_wait(int pid);

/* Look up a PCB by PID. Returns NULL if not found or slot is UNUSED. */
struct proc *proc_lookup(int pid);

/* For diagnostics. */
const char *proc_state_name(enum proc_state s);
void        proc_dump_table(void);

/* C-level first-time entry trampoline -- only called by the asm context
 * switch via the fake initial frame proc_create installed. Reads
 * current's user_entry/user_rsp and iretqs to ring 3. */
__attribute__((noreturn)) void proc_first_user_entry(void);

#endif /* TOBYOS_PROC_H */
