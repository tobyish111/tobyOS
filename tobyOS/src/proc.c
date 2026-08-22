/* proc.c -- process table, lifecycle, wait/reap.
 *
 * See proc.h for the high-level model. Implementation notes:
 *
 *   - Fixed-size table (PROC_MAX). PIDs are dense ints starting at 0
 *     (kernel_main). Slot index == PID for simplicity.
 *
 *   - Address-space construction sequence in spawn_internal:
 *       a. allocate fresh PML4 (kernel half mirrored by reference)
 *       b. saved_cr3 = read_cr3();  vmm_set_editor_root(new_pml4);
 *          write_cr3(new_pml4)             -> editor + CPU view both
 *                                             aimed at the new PML4
 *       c. elf_load_user(image)            -> populates user-half
 *       d. build_user_stack + pack argv    -> ditto
 *       e. write_cr3(saved_cr3); vmm_set_editor_root(old_editor)
 *          -> restore both. saved_cr3 is whatever the syscall came in
 *             with (parent user PML4 for sys_spawn, kernel PML4 for
 *             kernel boot calls); using read_cr3 instead of relying
 *             on g_pml4_phys is what makes user-to-user spawns safe.
 *
 *   - Initial kernel stack layout (so the first context switch lands
 *     in proc_first_user_entry):
 *
 *       kstack_top -> [unused 8 B padding for alignment]
 *                     [RIP = proc_first_user_entry]   <-- ret pops this
 *                     [r15 slot = 0]
 *                     [r14 slot = 0]
 *                     [r13 slot = 0]
 *                     [r12 slot = 0]
 *                     [rbx slot = 0]
 *                     [rbp slot = 0]
 *                     [RFLAGS = 0x202]                <-- popfq pops first
 *       saved_rsp ->  ^^^^^^^^^^
 *
 *     proc_context_switch pops RFLAGS + r15..rbp then `ret`s -- which
 *     lands at proc_first_user_entry, running on the new process's
 *     PML4, on a fresh kstack, with IRQs enabled (RFLAGS.IF=1) so the
 *     very first idle-loop/schedule decision is preemptible.
 *
 *   - proc_exit / user-fault path:
 *       sys_exit(code)            (or default_exception)
 *         -> proc_exit(code)
 *           -> mark TERMINATED + wakeup parent + sched_yield()
 *           sched_yield switches to a different proc; we never come
 *           back. The parent's proc_wait sees TERMINATED and reaps.
 */

#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/cgroup.h>   /* slice 16: the user-page charge funnel */
#include <tobyos/page_fault.h>   /* page_ref_* for CoW-aware brk shrink */
#include <tobyos/apic.h>         /* tlb_shootdown_remote */
#include <tobyos/heap.h>
#include <tobyos/vfs.h>
#include <tobyos/elf.h>
#include <tobyos/pe.h>
#include <tobyos/tss.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/klibc.h>
#include <tobyos/uaccess.h>
#include <tobyos/smp.h>
#include <tobyos/percpu.h>
#include <tobyos/file.h>
#include <tobyos/cap.h>
#include <tobyos/perf.h>
#include <tobyos/rng.h>
#include <tobyos/nsproxy.h>   /* nsproxy_release (slice 8) */

/* Asm helpers from proc_switch.S. */
extern __attribute__((noreturn)) void proc_enter_user_asm(uint64_t rip,
                                                          uint64_t rsp);
extern __attribute__((noreturn)) void proc_enter_user_thread_asm(uint64_t rip,
                                                                  uint64_t rsp,
                                                                  uint64_t arg);

/* User stack layout, post-Milestone-25A.
 *
 * The user stack must live entirely inside the canonical user half of
 * the address space. On x86_64 with 48-bit canonical VAs, the last
 * legal user byte is 0x00007FFFFFFFFFFF; the first non-canonical /
 * kernel-half page begins at 0x0000800000000000. So we anchor the
 * *top* of the stack at that boundary and let the *base* float down
 * by USER_STACK_PAGES pages.
 *
 * (The old layout fixed the base at 0x00007FFFFFFFE000 and let the top
 * drift, which silently worked when USER_STACK_PAGES==1 but pushed
 * page #2 into the kernel half once we bumped to 4 pages. vmm_map
 * correctly refused that, so /bin/hello failed to spawn. Don't repeat
 * that mistake.)
 *
 * Mapped range: [USER_STACK_TOP_PAGE, USER_STACK_TOP_VA)
 * Initial RSP : USER_STACK_TOP_VA - 16, OR -- if argv is supplied --
 *               just below the argc/argv/envp/string pool we packed at
 *               the top of the stack (see pack_argv_envp_on_user_stack). */
/* 64 pages = 256 KiB. The old 8-page (32 KiB) stack was too small for
 * non-trivial ported programs (e.g. interpreters with sizable frames);
 * 256 KiB is a sane default while still tiny next to a Linux 8 MiB stack. */
#define USER_STACK_PAGES     64
#define USER_STACK_BYTES     (USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_TOP_VA    0x0000800000000000ULL
#define USER_STACK_TOP_PAGE  (USER_STACK_TOP_VA - USER_STACK_BYTES)
#define USER_STACK_RSP_INIT  (USER_STACK_TOP_VA - 16)

/* Milestone 25A: per-proc heap region. Lives in a fresh slice of the
 * user half well clear of both the ELF (anchored at 0x400000 / runs
 * upward) and the user stack (anchored at the top of the user half).
 * proc_brk grows this region 4-KiB at a time on demand.
 *
 * 256 MiB cap is plenty for the smallish ports we run (dash-style
 * shell, coreutils-shape utilities, the dynamic loader). The kernel
 * still bounds-checks every grow against PMM availability, so this
 * is just the architectural ceiling. */
#define USER_HEAP_BASE       0x0000000010000000ULL
#define USER_HEAP_MAX_BYTES  (256ull * 1024ull * 1024ull)

struct proc g_proc[PROC_MAX];
struct proc       *g_current_proc;

/* The process running on the calling CPU. Per-CPU via g_percpu[].current
 * (kept current by the scheduler on every switch). Before the scheduler has
 * populated this CPU's slot (very early boot), fall back to the global
 * last-switched pointer. smp_this_cpu() safely returns cpu 0 pre-SMP. */
struct proc *current_proc(void) {
    struct proc *p = smp_this_cpu()->current;
    return p ? p : g_current_proc;
}

/* Pid of the current task, or -1. For debug instrumentation in files that
 * cannot include proc.h (vmm.c's page journal). */
int proc_current_pid_dbg(void) {
    struct proc *p = current_proc();
    return p ? p->pid : -1;
}

/* Tiny strncpy substitute -- copies up to max-1 chars and always
 * NUL-terminates the destination. */
static void name_copy(char *dst, const char *src, size_t max) {
    if (max == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < max && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

const char *proc_state_name(enum proc_state s) {
    switch (s) {
    case PROC_UNUSED:     return "UNUSED";
    case PROC_READY:      return "READY";
    case PROC_RUNNING:    return "RUNNING";
    case PROC_BLOCKED:    return "BLOCKED";
    case PROC_TERMINATED: return "TERMINATED";
    case PROC_STOPPED:    return "STOPPED";
    case PROC_EMBRYO:     return "EMBRYO";
    }
    return "?";
}

struct proc *proc_lookup(int pid) {
    if (pid < 0 || pid >= PROC_MAX) return 0;
    struct proc *p = &g_proc[pid];
    return (p->state == PROC_UNUSED || p->state == PROC_EMBRYO) ? 0 : p;
}

void proc_dump_table(void) {
    /* Milestone 19: ps-style dump includes cpu/syscall/page metrics
     * alongside the classic identity columns. We intentionally keep
     * this one function the "complete" process dump -- the `ps` shell
     * builtin just calls through, so any column added here shows up
     * there too. */
    kprintf("pid  state       name              uid  ses  cpu_ms    syscalls  pages  caps       exit\n");
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = &g_proc[i];
        if (p->state == PROC_UNUSED) continue;
        uint64_t cpu_ms = p->cpu_ns / 1000000ull;
        kprintf("%-3d  %-10s  %-16s  %-3d  %-3d  %-8lu  %-8lu  %-5lu  0x%08x  %d\n",
                p->pid, proc_state_name(p->state), p->name,
                p->uid, p->session_id,
                (unsigned long)cpu_ms,
                (unsigned long)p->syscall_count,
                (unsigned long)p->user_pages,
                (unsigned)p->caps, p->exit_code);
    }
}

/* Slice 109: the ONLY slot allocator. Claim by CAS so two concurrent
 * allocators (sys_fork mid-cow_fork vs sys_clone_thread, two forks, spawn
 * vs clone, ...) can never both build in the same slot. The old pattern --
 * scan for UNUSED, leave state UNUSED until the final READY -- held the
 * slot "free" for the whole build (hundreds of ms across cow_fork), and a
 * chrome thread-create landing in that window built a live thread inside
 * the fork child's slot: the child then ran with the thread's identity
 * (mongrel pid/tgid/is_thread), executed a demand-zeroed text page and
 * died at NULL, while the victim process lost the thread (the mp
 * bootstrap flake, gpuperf_viz_anim.001.log). */
struct proc *proc_slot_claim(void) {
    /* Slot 0 is reserved for kernel_main. Search 1..PROC_MAX-1. */
    for (int i = 1; i < PROC_MAX; i++) {
        enum proc_state expected = PROC_UNUSED;
        if (__atomic_compare_exchange_n(&g_proc[i].state, &expected,
                                        PROC_EMBRYO, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return &g_proc[i];
    }
    return 0;
}

void proc_slot_wipe(struct proc *p) {
    /* Zero everything below and above `state` separately: a plain memset
     * writes state=0 (PROC_UNUSED), and that instant is enough for a
     * concurrent proc_slot_claim to steal the slot being built. */
    size_t off = __builtin_offsetof(struct proc, state);
    memset(p, 0, off);
    memset((uint8_t *)p + off + sizeof(p->state), 0,
           sizeof(*p) - off - sizeof(p->state));
    p->state = PROC_EMBRYO;
}

static struct proc *alloc_slot(void) {
    return proc_slot_claim();
}

void proc_init(void) {
    memset(g_proc, 0, sizeof(g_proc));

    struct proc *k = &g_proc[0];
    k->pid         = 0;
    k->ppid        = 0;            /* kernel has no parent */
    k->state       = PROC_RUNNING;
    k->cr3         = vmm_kernel_pml4_phys();
    k->owns_pml4   = false;
    k->kstack_base = 0;            /* uses the boot stack -- never freed */
    k->kstack_top  = 0;
    k->wait_pid    = -1;
    /* pid 0 goes through sched_yield's fpu_save/restore like any proc, so
     * its FXSAVE area must be a valid clean state from the start. */
    fpu_init_default(k->fpu_state);
    /* Milestone 25A: pid 0 has no per-proc heap and starts at "/". */
    k->brk_base = k->brk_cur = k->brk_max = 0;
    k->cwd[0] = '/'; k->cwd[1] = '\0';
    /* Kernel uses kprintf/kputc/kbd directly; pid 0 leaves all fds NULL. */
    name_copy(k->name, "kernel_main", PROC_NAME_MAX);
    /* Milestone 18: pid 0 gets the ADMIN blanket bypass so every
     * kernel-invoked VFS/syscall path sails through cap_check. User
     * procs will inherit from pid 0 by default -- but the desktop
     * launch queue applies a sandbox profile on the way in, so they
     * end up with the correct (narrower) user-level cap set. */
    cap_grant_admin(k);

    /* Milestone 19: stamp last_switch_tsc so pid 0's idle/CPU time
     * starts being charged immediately. perf_init() happens later,
     * but rdtsc itself is valid from reset -- the conversion to ns
     * is a no-op until calibration completes. */
    k->last_switch_tsc = perf_rdtsc();

    /* Phase 1 M1.1: pid 0 is its own thread-group leader. */
    k->tgid        = 0;
    k->is_thread   = false;
    k->detached    = false;
    k->tls_base    = 0;
    k->join_waiters = 0;
    k->user_arg    = 0;

    /* Phase 1 M1.3: init signal state for pid 0 */
    signal_init_proc(&k->sigstate);

    /* pid 0 is the BSP's idle/kernel thread (drives the GUI) -- mark it so
     * the scheduler never steals it onto a secondary core. */
    k->is_idle = true;

    g_current_proc = k;
    /* cpu 0's per-CPU current is pid 0, so current_proc() works on the BSP
     * from the very start (before sched_init). */
    smp_cpu_mut(0)->current = k;

    kprintf("[proc] table initialised (%d slots), pid 0 = '%s' RUNNING\n",
            PROC_MAX, k->name);
}

/* Per-AP idle process. Each secondary CPU runs a kernel idle context the
 * scheduler can switch user work in and out of. It lives on the AP boot stack
 * and uses the kernel address space. pid 0 so it's treated as a kernel thread
 * everywhere `pid == 0` is special-cased (signals, faults); is_idle so it's
 * never stolen onto another CPU. */
static struct proc g_ap_idle[MAX_CPUS];

struct proc *proc_ap_idle(uint32_t cpu, uint64_t kstack_top) {
    struct proc *k = &g_ap_idle[cpu];
    memset(k, 0, sizeof(*k));
    k->pid         = 0;
    k->ppid        = 0;
    k->state       = PROC_RUNNING;
    k->cr3         = vmm_kernel_pml4_phys();
    k->owns_pml4   = false;
    k->kstack_base = 0;                       /* AP boot stack -- never freed */
    k->kstack_top  = (void *)kstack_top;
    k->wait_pid    = -1;
    k->is_idle     = true;
    k->cwd[0] = '/'; k->cwd[1] = '\0';
    name_copy(k->name, "ap_idle", PROC_NAME_MAX);
    /* An AP idle proc is pid-0 kernel context, exactly like the BSP's
     * kernel_main. It must hold the same ADMIN blanket bypass so any
     * kernel-invoked VFS/syscall path sails through cap_check -- notably
     * the desktop launch queue (gui_tick -> drain_launch_queue), which
     * vfs_stat/reads the app binary while running on whichever idle loop
     * happens to be pumping the compositor. Without this, a launch that
     * drained on an AP failed with "[cap] deny pid=0 'ap_idle' ... missing
     * FILE_READ" and the app never started. */
    cap_grant_admin(k);
    signal_init_proc(&k->sigstate);
    fpu_init_default(k->fpu_state);
    return k;
}

/* ---- per-process fd table helpers --------------------------------- */

static void close_all_fds(struct proc *p) {
    /* Exit sweep: the process's fcntl record locks die with it (flock
     * locks die with their descriptions inside file_close below). */
    { extern void fl_release_proc(struct proc *p);
      fl_release_proc(p); }
    for (int i = 0; i < PROC_NFDS; i++) {
        if (p->fds[i]) {
            file_close(p->fds[i]);
            p->fds[i] = 0;
        }
    }
}

/* Populate fd 0/1/2 by either cloning the explicit file the caller
 * supplied, or by allocating a fresh console-backed file as a default.
 * Returns true on success; on failure (OOM) closes everything we
 * already opened so the caller can fail cleanly. */
static bool install_initial_fds(struct proc *p,
                                struct file *fd0,
                                struct file *fd1,
                                struct file *fd2,
                                struct file *fd3,
                                struct file *fd4,
                                const struct proc_fd_map *extra,
                                int nextra) {
    /* fd0..fd2 default to the console; fd3/fd4 are OPTIONAL preopens
     * (slice 39: chrome's --remote-debugging-pipe reads DevTools JSON on
     * fd 3 and writes it on fd 4 -- the browser-window host hands the
     * pipe ends straight to the spawned browser). */
    struct file *src[5] = { fd0, fd1, fd2, fd3, fd4 };
    for (int i = 0; i < 5; i++) {
        struct file *nf;
        if (src[i]) {
            nf = file_clone(src[i]);
        } else if (i < 3) {
            nf = console_file_make();
        } else {
            continue;                       /* fd3/fd4 absent unless given */
        }
        if (!nf) {
            close_all_fds(p);
            return false;
        }
        p->fds[i] = nf;
    }

    /* Descriptors at chosen numbers, for `exec 3>file` and `cmd 8<<EOF`.
     * Cloned like the fixed ones, so the caller keeps its own reference. */
    for (int i = 0; i < nextra; i++) {
        int fd = extra[i].fd;
        if (fd < 0 || fd >= PROC_NFDS || !extra[i].f) continue;
        struct file *nf = file_clone(extra[i].f);
        if (!nf) {
            close_all_fds(p);
            return false;
        }
        if (p->fds[fd]) file_close(p->fds[fd]);
        p->fds[fd] = nf;
    }
    return true;
}

/* Pack { argc, argv[], envp[], auxv[], strings } at the top of the
 * user stack and return the new initial RSP via *out_rsp. Lives in
 * this file because it knows the user-stack VA layout
 * build_user_stack just installed.
 *
 * Stack picture (high addr at top), milestone 25D layout:
 *
 *   USER_STACK_TOP_VA - 16        <-- 16 bytes of head padding (small scratch)
 *   ----------------------
 *   string pool (argv strings followed by envp strings)
 *   ----------------------
 *   auxv[auxc].a_type  = ABI_AT_NULL          \  always present so
 *   auxv[auxc].a_val   = 0                    /  ld.so / static
 *   auxv[auxc-1] ... auxv[0]                     crt0 can iterate
 *   ----------------------
 *   envp[envc] = NULL
 *   envp[envc-1] = ptr ...
 *   envp[0]    = ptr ...
 *   ----------------------
 *   argv[argc] = NULL
 *   argv[argc-1] = ptr ...
 *   argv[0]    = ptr ...
 *   ----------------------
 *   argc                                  <-- *out_rsp points here
 *
 * The user-side trampoline (libtoby crt0.S since milestone 25C)
 * reads:
 *   argc  = [rsp]
 *   argv  = rsp + 8
 *   envp  = argv + (argc+1)*8
 *   auxv  = envp_walked_to_NULL + 8         (Milestone 25D adds this)
 *
 * Caller must have just switched CR3 to the new process's PML4 so we
 * can write through the user-half virtual addresses directly.
 *
 * For argc == 0 we still push { argc=0, argv=[NULL], envp=[NULL] } so
 * user code can always assume the canonical layout. envp may be NULL
 * (= "no environment"); we still emit a single NULL terminator so the
 * trampoline can iterate it without a special-case. The auxv array is
 * always written, even if it only contains the trailing AT_NULL.
 *
 * SysV alignment requirement: at the point the user-mode crt0 is
 * about to `call main`, RSP must be 16-byte aligned. The crt0 has
 * already pushed an 8-byte return address by then, so RSP at entry
 * (i.e. our argc_va here) must be 16-byte aligned -- we adjust by
 * sliding the whole packed block down by 8 if needed. */
struct user_stack_pack {
    int                     argc;
    char                  **argv;
    int                     envc;
    char                  **envp;
    /* Auxv entries (excluding the trailing AT_NULL which we always
     * append). May be NULL/0 -- in that case only the AT_NULL
     * sentinel is pushed. */
    const struct abi_auxv  *auxv;
    int                     auxc;
};

static bool pack_user_stack(const struct user_stack_pack *L,
                            uint64_t *out_rsp) {
    if (!L || L->argc < 0 || L->envc < 0 || L->auxc < 0) return false;
    if (L->argc > ABI_ARGV_MAX || L->envc > ABI_ENVP_MAX) {
        kprintf("[proc] argv/envp too long (%d/%d), max %d/%d\n",
                L->argc, L->envc, ABI_ARGV_MAX, ABI_ENVP_MAX);
        return false;
    }

    /* Total string-pool bytes (argv strings + envp strings). */
    size_t pool = 0;
    for (int i = 0; i < L->argc; i++) {
        if (!L->argv[i]) return false;
        size_t l = strlen(L->argv[i]);
        if (l + 1 > ABI_ARG_MAX) return false;
        pool += l + 1;
    }
    for (int i = 0; i < L->envc; i++) {
        if (!L->envp[i]) return false;
        size_t l = strlen(L->envp[i]);
        if (l + 1 > ABI_ARG_MAX) return false;
        pool += l + 1;
    }
    size_t pool_padded   = (pool + 7) & ~7ULL;                  /* 8-B align */
    size_t argv_bytes    = (size_t)(L->argc + 1) * 8;           /* +NULL term */
    size_t envp_bytes    = (size_t)(L->envc + 1) * 8;           /* +NULL term */
    size_t auxv_bytes    = (size_t)(L->auxc + 1) * 16;          /* +AT_NULL */
    size_t pad_head      = 16;                                  /* scratch */

    size_t total = pad_head + pool_padded + auxv_bytes
                 + envp_bytes + argv_bytes + 8;
    if (total + 64 > USER_STACK_BYTES) {
        kprintf("[proc] argv+envp+auxv too large for user stack "
                "(%lu bytes)\n", (unsigned long)total);
        return false;
    }

    /* Lay out from top of stack downwards. */
    uint64_t pool_va = USER_STACK_TOP_VA - pad_head - pool_padded;
    uint64_t auxv_va = pool_va - auxv_bytes;
    uint64_t envp_va = auxv_va - envp_bytes;
    uint64_t argv_va = envp_va - argv_bytes;
    uint64_t argc_va = argv_va - 8;

    /* SysV alignment: argc_va % 16 == 0 keeps the stack 16-aligned at
     * main's entry after the trampoline's `call main`. */
    if (argc_va % 16 != 0) {
        argc_va -= 8;
        argv_va -= 8;
        envp_va -= 8;
        auxv_va -= 8;
        pool_va -= 8;
    }

    /* Write argv strings then envp strings into the pool. */
    uint8_t          *pool_ptr  = (uint8_t  *)pool_va;
    uint64_t         *argv_ptr  = (uint64_t *)argv_va;
    uint64_t         *envp_ptr  = (uint64_t *)envp_va;
    struct abi_auxv  *auxv_ptr  = (struct abi_auxv *)auxv_va;
    size_t            off       = 0;

    /* All the destinations below are user-half VAs in the new process's
     * address space; under SMAP they need a uaccess window. Spawn runs in
     * kernel context (window closed) while execve runs inside the syscall
     * window (open) -- uaccess_begin/end save+restore AC so both nest. */
    unsigned long uflags = uaccess_begin();

    for (int i = 0; i < L->argc; i++) {
        size_t l = strlen(L->argv[i]) + 1;
        memcpy(pool_ptr + off, L->argv[i], l);
        argv_ptr[i] = pool_va + off;
        off += l;
    }
    argv_ptr[L->argc] = 0;

    for (int i = 0; i < L->envc; i++) {
        size_t l = strlen(L->envp[i]) + 1;
        memcpy(pool_ptr + off, L->envp[i], l);
        envp_ptr[i] = pool_va + off;
        off += l;
    }
    envp_ptr[L->envc] = 0;

    /* Auxv: copy real entries verbatim, then the AT_NULL sentinel.
     * The kernel-side L->auxv may be NULL when L->auxc == 0. */
    for (int i = 0; i < L->auxc; i++) {
        auxv_ptr[i] = L->auxv[i];
    }
    auxv_ptr[L->auxc].a_type = ABI_AT_NULL;
    auxv_ptr[L->auxc].a_val  = 0;

    /* Write argc. */
    *(uint64_t *)argc_va = (uint64_t)(uint32_t)L->argc;

    uaccess_end(uflags);

    *out_rsp = argc_va;
    return true;
}

/* Allocate USER_STACK_PAGES contiguous user-half pages, mapped
 * VMM_USER|VMM_WRITE|VMM_NX. Records the base page so the reaper can
 * unmap+free them later. */
static bool build_user_stack(struct proc *p) {
    p->user_stack_base  = USER_STACK_TOP_PAGE;
    p->user_stack_pages = USER_STACK_PAGES;

    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = mm_user_page_alloc(p);      /* slice 16: charged */
        if (phys == 0) {
            kprintf("[proc] OOM allocating user stack page %lu/%d\n",
                    (unsigned long)i, USER_STACK_PAGES);
            return false;
        }
        if (!vmm_map(USER_STACK_TOP_PAGE + i * PAGE_SIZE, phys, PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_USER)) {
            kprintf("[proc] vmm_map failed for user stack page %lu\n",
                    (unsigned long)i);
            pmm_free_page(phys);
            return false;
        }
        memset((void *)pmm_phys_to_virt(phys), 0, PAGE_SIZE);
    }
    return true;
}

/* Allocate + prime a kernel stack. Returns base on success (so the
 * reaper can free it). Initial layout makes the first context_switch
 * land at proc_first_user_entry. */
static bool build_kstack(struct proc *p) {
    void *base = kmalloc(PROC_KSTACK_SZ);
    if (!base) {
        kprintf("[proc] OOM allocating kstack (%d bytes)\n", PROC_KSTACK_SZ);
        return false;
    }
    memset(base, 0, PROC_KSTACK_SZ);
    p->kstack_base = base;
    p->kstack_top  = (uint8_t *)base + PROC_KSTACK_SZ;

    /* Clean default FPU/SSE state for this new process, restored on its
     * first entry by proc_first_user_entry (see cpu.h fpu_init_default). */
    fpu_init_default(p->fpu_state);

    /* Build the fake initial frame (memory from high -> low, top -> bot):
     *   [padding]               -- 8 B for SysV 16-B alignment after ret
     *   [RIP = proc_first_user_entry]  <-- ret pops this last
     *   [r15-slot]              -- pop rbp reads this (all zero, labels are
     *   [r14-slot]                 cosmetic: the saved/restored bytes
     *   [r13-slot]                 are the same whichever register
     *   [r12-slot]                 ends up holding them).
     *   [rbx-slot]
     *   [rbp-slot]              -- pop r15 reads this
     *   [RFLAGS = 0x202]        <-- popfq reads this FIRST, IF=1
     *                              (saved_rsp points here)
     *
     * proc_context_switch's resume path is:
     *   popfq; pop r15..rbp; ret
     * so RFLAGS has to sit at the lowest address (top of stack for the
     * new task), below the six callee-saved slots. IF=1 makes sure the
     * brand-new task starts with IRQs enabled -- otherwise the very
     * first idle-loop hlt() would deadlock. Bit 1 of RFLAGS is the
     * mandatory "always 1" reserved bit. */
    uint64_t *sp = (uint64_t *)p->kstack_top;
    *--sp = 0;                                    /* alignment padding */
    *--sp = (uint64_t)proc_first_user_entry;      /* return RIP for ret */
    *--sp = 0;                                    /* r15 slot */
    *--sp = 0;                                    /* r14 slot */
    *--sp = 0;                                    /* r13 slot */
    *--sp = 0;                                    /* r12 slot */
    *--sp = 0;                                    /* rbx slot */
    *--sp = 0;                                    /* rbp slot */
    *--sp = 0x202ull;                             /* RFLAGS (IF=1) */
    p->saved_rsp = (uint64_t)sp;
    return true;
}

/* Shared worker for proc_create_from_elf / proc_spawn. The two front-
 * ends differ only in fd setup, argv/envp passing, and the optional
 * cwd override.
 *
 * Milestone 25C: this is the single canonical user-program-launch
 * path. Both proc_create_from_elf (kernel boot path) and proc_spawn
 * (sys_spawn syscall) funnel through here. The shell, the desktop
 * launcher, the test harness, and the dynamic-linker stub
 * (milestone 25D) all use proc_spawn under the hood. There is no
 * "fork" -- we always build a fresh PML4 + a fresh user-space image,
 * which keeps the model trivially correct for an MMU-only kernel
 * without a copy-on-write story. */
/* Kernel-side seam for the shell's `set -m`: assign PID's process group
 * directly. The kernel shell is trusted context -- the permission checks
 * live in the syscall path (lx_do_setpgid); the hosted /bin/tsh reaches
 * the same state through setpgid(2) via its host.c shim. */
int proc_set_pgid(int pid, int pgid) {
    struct proc *t = proc_lookup(pid);
    if (!t) return -1;
    t->pgid = pgid;
    return 0;
}

/* Kernel-side WUNTRACED-style foreground wait (see proc.h). Yield-poll,
 * the same shape as the syscall arms; the kernel shell runs as a kernel
 * thread, so there is no syscall boundary to deliver an EINTR across. */
int proc_wait_fg(int pid, int *stop_sig) {
    if (stop_sig) *stop_sig = 0;
    for (;;) {
        struct proc *c = proc_lookup(pid);
        if (!c) return -1;
        if (c->state == PROC_STOPPED && !c->stop_reported) {
            c->stop_reported = true;
            if (stop_sig) *stop_sig = c->stop_sig > 0 ? c->stop_sig : 19;
            return 0;
        }
        if (c->state == PROC_TERMINATED) return proc_wait(pid);
        sched_yield();
    }
}

static int spawn_internal(const char *path, const char *name,
                          struct file *fd0, struct file *fd1, struct file *fd2,
                          struct file *fd3, struct file *fd4,
                          const struct proc_fd_map *extra, int nextra,
                          int argc, char **argv,
                          int envc, char **envp,
                          const char *cwd_override) {
    if (!path) return -1;

    struct proc *p = alloc_slot();
    if (!p) {
        kprintf("[proc] cannot create '%s': process table full\n", path);
        return -1;
    }

    proc_slot_wipe(p);                /* stays EMBRYO; READY at the very end */
    p->pid       = (int)(p - g_proc);
    p->wait_pid  = -1;
    /* Slice 64c: "not inside a syscall" is -1, but memset leaves 0, which
     * is a VALID syscall number -- BKL hold time would be misattributed to
     * read()/native-0 for every proc that never made one. */
    p->cursys     = -1;
    p->cursys_nat = -1;
    p->exit_code = -1;
    /* Milestone 25A: parent-pid bookkeeping. The parent is whatever
     * proc was running when the spawn was issued. pid 0 (kernel) is
     * the canonical fallback for early boot / kthreads. */
    {
        struct proc *parent_for_ppid = current_proc();
        p->ppid = parent_for_ppid ? parent_for_ppid->pid : 0;
    }
    /* Milestone 25A: per-proc heap starts empty -- proc_brk grows it. */
    p->brk_base = USER_HEAP_BASE;
    p->brk_cur  = USER_HEAP_BASE;
    p->brk_max  = USER_HEAP_BASE + USER_HEAP_MAX_BYTES;
    /* Milestone 19: stamp creation time for ps/top + init metrics. */
    p->created_ns      = perf_now_ns();
    p->cpu_ns          = 0;
    p->syscall_count   = 0;
    p->user_pages      = 0;
    p->last_switch_tsc = 0;
    /* Phase 1 M1.1: new process = its own thread group leader */
    p->tgid         = p->pid;
    p->is_thread    = false;
    p->detached     = false;
    p->tls_base     = 0;
    p->clear_child_tid = 0;       /* B11: no pthread_join futex address yet */
    p->join_waiters = 0;
    p->user_arg     = 0;
    /* Phase 1 M1.3: init signal state for new process */
    signal_init_proc(&p->sigstate);
    /* Inherit session tag + user identity from the parent
     * (current_proc()). pid 0 has session_id == 0 / uid == 0 unless
     * the desktop launcher temporarily flipped them to the active
     * session's tag + uid -- which is exactly how desktop-launched
     * apps get tagged with the user's session AND end up running as
     * that user. */
    {
        struct proc *parent = current_proc();
        p->session_id = parent ? parent->session_id : 0;
        /* Process group: inherited, like the session tag. A parent that
         * predates the pgid field (or pid 0 itself) reads as leading its
         * own group. */
        p->pgid       = (parent && parent->pgid > 0) ? parent->pgid
                      : (parent ? parent->pid : p->pid);
        p->uid        = parent ? parent->uid        : 0;
        p->gid        = parent ? parent->gid        : 0;
        /* Linux slice 1: umask is inherited, like uid/gid. 0022 for a
         * parentless (kernel-spawned) proc -- the conventional default. */
        p->umask      = parent ? parent->umask      : 0022u;
        /* Linux slice 2: the rest of the credential set is inherited too.
         * A plain spawn is not a privilege transition, so real/effective/saved
         * all agree on entry -- exactly what fork(2) gives you. execve may
         * then raise them if the image is setuid (see execve_apply_setid).
         *
         * All three are seeded from the INHERITED EFFECTIVE ids, never from
         * the parent's real ids. That distinction is load-bearing, not
         * stylistic: the desktop launcher (gui.c) and session code (kernel.c)
         * both temporarily flip pid 0's EFFECTIVE uid to the logged-in user
         * around a spawn and restore it afterwards. Copying parent->ruid there
         * would hand the child real=root with effective=user -- and setuid(2)
         * lets any process return to its real uid, so the user's app could
         * simply setuid(0) and be root. Seeding from effective closes that.
         *
         * fork(2) is unaffected and correct already: it memcpy's the whole PCB,
         * so a forked child inherits the parent's genuine triple. */
        p->ruid       = p->uid;
        p->rgid       = p->gid;
        p->suid       = p->uid;
        p->sgid       = p->gid;
        p->ngroups    = parent ? parent->ngroups : 0;
        for (int gi = 0; gi < p->ngroups && gi < PROC_NGROUPS_MAX; gi++)
            p->groups[gi] = parent->groups[gi];
        /* Linux capabilities: root gets the full set, everyone else none.
         * This mirrors the "uid 0 bypasses every check" rule the VFS already
         * follows, expressed in the ABI's own terms. */
        if (p->uid == 0) {
            p->lcap_eff = p->lcap_perm = p->lcap_inh = ~0ull;
        } else {
            p->lcap_eff = p->lcap_perm = p->lcap_inh = 0;
        }
        /* Milestone 18: inherit capability mask + sandbox root from
         * parent. A NULL parent shouldn't be reachable in practice
         * (spawn_internal runs in the context of a live proc) but we
         * fall back to admin-for-kernel to mirror the cap_has(NULL)
         * convention. */
        if (parent) {
            p->caps = parent->caps;
            size_t n = strlen(parent->sandbox_root);
            if (n >= PROC_SANDBOX_MAX) n = PROC_SANDBOX_MAX - 1;
            memcpy(p->sandbox_root, parent->sandbox_root, n);
            p->sandbox_root[n] = '\0';
            /* Milestone 25A: cwd inherits from parent unless caller
             * provides an explicit override (proc_spawn does so when
             * the spec sets one). */
            size_t cn = strlen(parent->cwd);
            if (cn >= ABI_PATH_MAX) cn = ABI_PATH_MAX - 1;
            memcpy(p->cwd, parent->cwd, cn);
            p->cwd[cn] = '\0';
        } else {
            p->caps = CAP_GROUP_ALL;
            p->sandbox_root[0] = '\0';
            p->cwd[0] = '/'; p->cwd[1] = '\0';
        }
        if (cwd_override && cwd_override[0]) {
            size_t cn = strlen(cwd_override);
            if (cn >= ABI_PATH_MAX) cn = ABI_PATH_MAX - 1;
            memcpy(p->cwd, cwd_override, cn);
            p->cwd[cn] = '\0';
        }
    }
    if (name) {
        name_copy(p->name, name, PROC_NAME_MAX);
    } else {
        const char *base = path;
        for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
        name_copy(p->name, base, PROC_NAME_MAX);
    }

    /* B20 (procfs): record the executable path for /proc/<pid>/exe. */
    if (path) {
        size_t pn = strlen(path);
        if (pn >= ABI_PATH_MAX) pn = ABI_PATH_MAX - 1;
        memcpy(p->exe_path, path, pn);
        p->exe_path[pn] = '\0';
    } else {
        p->exe_path[0] = '\0';
    }

    /* ---- 0. inherit fds (clone the explicit ones, default the rest) ---- */
    if (!install_initial_fds(p, fd0, fd1, fd2, fd3, fd4, extra, nextra)) {
        kprintf("[proc] '%s': OOM installing initial fds\n", path);
        memset(p, 0, sizeof(*p));
        p->state = PROC_UNUSED;
        return -1;
    }

    /* ---- 1. read the ELF off the VFS ---- */
    void  *image      = 0;
    size_t image_size = 0;
    int rc = vfs_read_all(path, &image, &image_size);
    if (rc != VFS_OK) {
        kprintf("[proc] cannot read '%s': %s\n", path, vfs_strerror(rc));
        close_all_fds(p);
        memset(p, 0, sizeof(*p));
        p->state = PROC_UNUSED;
        return -1;
    }

    /* ---- 2. allocate fresh PML4 ---- */
    uint64_t pml4 = vmm_create_user_pml4();
    if (pml4 == 0) {
        kfree(image);
        close_all_fds(p);
        memset(p, 0, sizeof(*p));
        p->state = PROC_UNUSED;
        return -1;
    }
    p->cr3       = pml4;
    p->owns_pml4 = true;

    /* ---- 3. populate user-half mappings (ELF + stack + argv) ----
     *
     * CRITICAL (Milestone 25C): when sys_spawn is invoked from a user
     * process, the CPU's CR3 holds *that user's* PML4 (the scheduler
     * left it there during context_switch -- the kernel never resets
     * CR3 on the syscall path). Meanwhile g_pml4_phys (which
     * vmm_set_active_root reads to compute its return value) still
     * points at the kernel PML4 because no one synchronises it on
     * context-switch.
     *
     * If we used vmm_set_active_root(pml4) -> vmm_set_active_root(old)
     * here, the second call would land the *kernel* PML4 in CR3 and
     * the parent would instruction-fetch fault on the very next ring-3
     * instruction after sysretq (cr2=rip in the user .text, err=0x14:
     * user-mode instruction fetch on a non-present page).
     *
     * Capture the truthful CR3 ourselves (read_cr3()), retarget the
     * editor pointer so vmm_map calls from build_user_stack land in
     * the child's PML4, and explicitly write CR3 to the child so
     * pack_argv_envp_on_user_stack can write through the child's
     * user-half VAs. On the way out we restore CR3 to whatever the
     * caller's CR3 actually was -- parent's user PML4 for syscall
     * spawns, kernel PML4 for kernel-boot spawns. The editor pointer
     * goes back to whatever it was so subsequent kernel page-table
     * edits target the right tree. */
    uint64_t saved_cr3   = read_cr3();
    uint64_t old_editor  = vmm_set_editor_root(pml4);
    if (saved_cr3 != pml4) write_cr3(pml4);

    /* Track C: a Windows PE/COFF image is loaded by an entirely separate
     * path (sections + IAT thunks, no ELF program headers). Detect it by
     * the 'MZ'/'PE' magic in the kernel-side `image` buffer (CR3-agnostic)
     * and branch the whole image-load below. */
    bool is_pe = pe_is_image(image, image_size);

    /* Outputs shared by both arms, consumed after the CR3 window closes. */
    bool ok;
    bool has_interp = false;
    struct elf_load_info prog_info   = {0};
    struct elf_load_info interp_info = {0};
    struct pe_load_info  pe_info     = {0};
    uint64_t user_rsp = USER_STACK_RSP_INIT;

  if (is_pe) {
    /* ---- Windows PE/COFF image (Track C) ---- */
    int prc = pe_load_user(image, image_size, argc, argv, &pe_info);
    kfree(image);
    ok = (prc == 0);
    if (ok) {
        p->personality = ABI_PERS_WIN32;
        /* GS base = the PE's TEB so the CRT's gs:[0x30] (NtCurrentTeb) works.
         * Loaded into the IA32_KERNEL_GS_BASE shadow on context switch and
         * SWAPGS'd into the active GS base on the way to CPL3 (do_switch +
         * syscall_entry.S). */
        p->gs_base = pe_info.teb;
        /* C14: record the image base + .rsrc dir so DialogBoxParamA can walk
         * RT_DIALOG resource templates from this process's mapped image. */
        p->win_image_base = pe_info.image_base;
        p->win_rsrc_rva   = pe_info.rsrc_rva;
        p->win_rsrc_size  = pe_info.rsrc_size;
        /* C18b: remember the .tls template for per-thread TLS in CreateThread. */
        p->win_tls_raw_va   = pe_info.tls_raw_va;
        p->win_tls_raw_size = pe_info.tls_raw_size;
        p->win_tls_total    = pe_info.tls_total;
        p->win_tls_index    = pe_info.tls_index;
        kprintf("[proc] pid %d '%s' -> Win32 PE personality (entry=%p teb=%p)\n",
                p->pid, p->name, (void *)pe_info.entry, (void *)pe_info.teb);
        ok = build_user_stack(p);
    }
    if (ok) {
        /* Windows entry points are entered with RSP%16==8 -- i.e. as if reached
         * by a CALL (return address pushed). The mingw mainCRTStartup relies on
         * this: its `sub rsp,0x28; call __tmainCRTStartup` only keeps the stack
         * 16-aligned at the call if it starts at %16==8, and the whole CRT chain
         * (-> main) inherits that. A 16-aligned (%16==0) entry desyncs every
         * frame by 8 and faults the first `movaps [rbp+x]`. Leave headroom above
         * RSP for the marshalling gate's MS-x64 stack-arg reads. No argv/auxv
         * frame -- a PE gets its command line from the CRT-data region / TEB. */
        user_rsp = ((USER_STACK_TOP_VA - 0x400) & ~0xFULL) - 8;
    }
  } else {
    /* ---- ELF image (tobyOS-native or Linux personality) ---- */
    /* Milestone 25D: peek for PT_INTERP BEFORE loading. Done outside
     * the CR3 swap window because elf_peek_interp only reads from the
     * kernel-virtual `image` buffer -- no user mappings involved. */
    char interp_path[ABI_PATH_MAX];
    has_interp = elf_peek_interp(image, image_size,
                                 interp_path, sizeof(interp_path));

    /* Pick load bases:
     *  - ET_EXEC: load_base = 0 (vaddrs are absolute).
     *  - ET_DYN program: chosen base low in user half.
     *  - ET_DYN interp: a separate base far enough above the program
     *    that they can never overlap, even after generous BSS growth.
     * The exact constants are policy and can move freely later -- they
     * only have to be:
     *   * page-aligned,
     *   * inside the canonical user half,
     *   * non-overlapping with the user heap (USER_HEAP_BASE = 256 MiB)
     *     and the user stack (top of user half).
     */
    uint64_t prog_load_base   = 0;
    uint64_t interp_load_base = 0;

    {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
        if (image_size >= sizeof(Elf64_Ehdr) && eh->e_type == ET_DYN) {
            prog_load_base = 0x0000000000500000ULL;     /* 5 MiB */
        }
    }
    if (has_interp) {
        interp_load_base = 0x0000000040000000ULL;       /* 1 GiB */
    }

    /* Milestone 19: track ELF-load cost as a perf zone -- it's the
     * single most expensive operation during spawn (reads segments,
     * alloc+maps each page, memcpys the bytes in). */
    uint64_t t_elf = perf_rdtsc();
    ok = elf_load_user_at(image, image_size, prog_load_base, &prog_info);
    perf_zone_end(PERF_Z_ELF_LOAD, t_elf);
    kfree(image);                  /* segments now live in their own frames */

    /* Track B: latch the ABI personality. A binary runs through the Linux
     * syscall translation layer if it's branded ELFOSABI_LINUX *or* (B10)
     * names a known Linux dynamic loader as its PT_INTERP -- so an unbranded
     * off-the-shelf dynamic Linux binary drops-and-runs. Everything else keeps
     * the tobyOS default. */
    if (ok) {
        bool is_linux = elf_is_linux_abi(prog_info.osabi, has_interp,
                                         interp_path, prog_info.has_gnu_phdr);
        p->personality = is_linux ? ABI_PERS_LINUX : ABI_PERS_TOBY;
        if (is_linux) {
            kprintf("[proc] pid %d '%s' -> Linux ABI personality "
                    "(EI_OSABI=%u interp=%s gnu_phdr=%d)\n", p->pid, p->name,
                    prog_info.osabi, has_interp ? interp_path : "(none)",
                    (int)prog_info.has_gnu_phdr);
        }
    }

    /* Milestone 25D: when the program declares PT_INTERP, also load
     * the interpreter (the dynamic linker, /lib/ld-toby.so) into the
     * same address space at a non-overlapping base. The kernel's
     * single role here is to make both images resident and to give
     * the interpreter the auxv it needs to find the program's PHDRs;
     * actual relocation and symbol resolution happen in user mode. */
    void  *interp_image      = 0;
    size_t interp_image_size = 0;
    if (ok && has_interp) {
        /* vfs_read_all uses kernel mappings, so it works regardless of
         * the active CR3. */
        int irc = vfs_read_all(interp_path, &interp_image, &interp_image_size);
        if (irc != VFS_OK) {
            kprintf("[proc] PT_INTERP='%s' missing from VFS: %s\n",
                    interp_path, vfs_strerror(irc));
            ok = false;
        } else {
            ok = elf_load_user_at(interp_image, interp_image_size,
                                  interp_load_base, &interp_info);
            kfree(interp_image);
            if (!ok) {
                kprintf("[proc] failed loading interpreter '%s'\n",
                        interp_path);
            } else {
                kprintf("[proc] loaded interpreter '%s' base=%p entry=%p\n",
                        interp_path,
                        (void *)interp_info.load_base,
                        (void *)interp_info.entry);
            }
        }
    }

    if (ok) ok = build_user_stack(p);

    /* Pack auxv (Milestone 25D). Always emit a tiny vector even for
     * static programs -- the trailing AT_NULL is harmless to libtoby
     * crt0 (which doesn't read auxv at all today) and makes the stack
     * shape uniform across static and dynamic launches. */
    struct abi_auxv aux[20];
    int             auxc = 0;
    if (ok) {
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PHDR,   prog_info.phdr_va  };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PHNUM,  prog_info.phnum    };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PHENT,  prog_info.phent    };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_BASE,   has_interp
                                                          ? interp_info.load_base
                                                          : 0              };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_ENTRY,  prog_info.entry    };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_PAGESZ, PAGE_SIZE          };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_FLAGS,  0                  };
        /* Real credentials (2026-08-22; were hardcoded 0). glibc reads
         * AT_SECURE to decide secure-mode (ignore LD_* env, etc.) and
         * compares AT_EUID against geteuid() -- both lie if these do.
         * Translated at the user-namespace reporting boundary, same as
         * getuid/stat/procfs (slice 11's rule). */
        aux[auxc++] = (struct abi_auxv){ ABI_AT_UID,
                                         userns_cur_uid((uint32_t)p->ruid) };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_EUID,
                                         userns_cur_uid((uint32_t)p->uid)  };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_GID,
                                         userns_cur_gid((uint32_t)p->rgid) };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_EGID,
                                         userns_cur_gid((uint32_t)p->gid)  };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_HWCAP,  ABI_AT_HWCAP_X86_64_BASE };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_CLKTCK, 100                };
        aux[auxc++] = (struct abi_auxv){ ABI_AT_SECURE,
                                         (p->uid != p->ruid ||
                                          p->gid != p->rgid) ? 1u : 0u     };
        /* AT_RANDOM: 16 bytes a Linux libc reads for its stack canary.
         * Point at the 16-byte scratch pad pack_user_stack always leaves
         * at the very top of the (zeroed) user stack -- valid + readable.
         * Slice 87: seed with CSPRNG after packing (was left zero). */
        aux[auxc++] = (struct abi_auxv){ ABI_AT_RANDOM, USER_STACK_TOP_VA - 16 };
    }

    /* Default RSP if no argv -- pack a canonical
     *   { argc=0, argv=[NULL], envp=[NULL], auxv=[AT_NULL] }
     * frame anyway so the user-side trampoline always finds a valid
     * layout at the top of the stack. */
    if (ok) {
        struct user_stack_pack pack = {
            .argc = argc, .argv = argv,
            .envc = envc, .envp = envp,
            .auxv = aux,  .auxc = auxc,
        };
        ok = pack_user_stack(&pack, &user_rsp);
        if (ok) {
            uint8_t rnd[16];
            rng_fill(rnd, sizeof rnd);
            unsigned long uf = uaccess_begin();
            memcpy((void *)(uintptr_t)(USER_STACK_TOP_VA - 16), rnd, 16);
            uaccess_end(uf);
        }
    }
  } /* end ELF arm */

    if (read_cr3() != saved_cr3) write_cr3(saved_cr3);
    vmm_set_editor_root(old_editor);

    if (!ok) {
        vmm_destroy_user_pml4(pml4);
        close_all_fds(p);
        memset(p, 0, sizeof(*p));
        p->state = PROC_UNUSED;
        return -1;
    }

    /* Initial RIP: the PE entry point for a Windows image; otherwise the
     * dynamic linker if present (so it can self-relocate, load DT_NEEDED
     * libraries, and resolve relocations before jumping to AT_ENTRY) or
     * the program entry for a static ELF. */
    p->user_entry = is_pe       ? pe_info.entry
                  : has_interp  ? interp_info.entry
                                : prog_info.entry;
    p->user_rsp   = user_rsp;

    /* ---- 4. kernel stack with the fake initial frame ---- */
    if (!build_kstack(p)) {
        vmm_destroy_user_pml4(pml4);
        close_all_fds(p);
        memset(p, 0, sizeof(*p));
        p->state = PROC_UNUSED;
        return -1;
    }

    /* ---- 5. ready to run ---- */
    p->state = PROC_READY;
    sched_enqueue(p);

    /* SLICE 16: user_pages is now MAINTAINED, not estimated.
     *
     * It used to be assigned here, once, as user_stack_pages plus one page of
     * "rough elf overhead", and never updated again -- so `ps` reported a constant
     * RSS for every process no matter what it mapped, and anything built on it
     * (oom.c's
     * victim score) was scoring a fabricated number. The counter is now bumped by
     * mm_user_page_alloc / mm_user_page_free at every user-page event, so this
     * assignment MUST NOT clobber what the stack build already charged. */
    (void)0;   /* user_pages accumulated by the funnel; nothing to assign */

    /* Milestone 19 metric: another proc spawned. */
    perf_count_proc_spawn();

    kprintf("[proc] created pid=%d ppid=%d '%s' entry=%p rsp=%p cr3=0x%lx"
            " kstack=%p argc=%d envc=%d cwd='%s'\n",
            p->pid, p->ppid, p->name, (void *)p->user_entry,
            (void *)p->user_rsp, p->cr3, p->kstack_top, argc, envc,
            p->cwd);
    return p->pid;
}

int proc_create_from_elf(const char *path, const char *name) {
    /* Default: console for fd 0/1/2, no argv, no envp, inherit cwd. */
    return spawn_internal(path, name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* Ring-0 kernel worker: no user half, no ELF, no fds. Same PCB + fake
 * initial frame as any proc; proc_first_user_entry sees is_kernel and
 * CALLs user_entry (a kernel function) instead of iret-ing to ring 3.
 * Runs on the kernel PML4. See proc.h. */
int proc_create_kernel(void (*entry)(void), const char *name) {
    if (!entry) return -1;

    struct proc *p = alloc_slot();
    if (!p) {
        kprintf("[proc] cannot create kworker '%s': table full\n",
                name ? name : "?");
        return -1;
    }
    proc_slot_wipe(p);                /* stays EMBRYO; READY at the end */
    p->pid        = (int)(p - g_proc);
    p->wait_pid   = -1;
    p->exit_code  = -1;
    p->ppid       = 0;
    p->tgid       = p->pid;
    p->cr3        = vmm_kernel_pml4_phys();
    p->owns_pml4  = false;
    p->is_kernel  = true;
    p->user_entry = (uint64_t)entry;      /* kernel fn, CALLed on entry */
    p->cwd[0] = '/'; p->cwd[1] = '\0';
    name_copy(p->name, name ? name : "kworker", PROC_NAME_MAX);
    /* Kernel context: same ADMIN blanket as pid 0 / ap_idle so any
     * kernel-invoked VFS/net path sails through cap_check. */
    cap_grant_admin(p);
    signal_init_proc(&p->sigstate);
    p->created_ns = perf_now_ns();

    if (!build_kstack(p)) {
        p->state = PROC_UNUSED;
        return -1;
    }

    p->state = PROC_READY;
    sched_enqueue(p);
    kprintf("[proc] created kworker pid=%d '%s' entry=%p kstack=%p\n",
            p->pid, p->name, (void *)p->user_entry, p->kstack_top);
    return p->pid;
}

int proc_spawn(const struct proc_spec *spec) {
    if (!spec) return -1;
    /* Milestone 19: span the whole spawn pipeline so `perf` can show
     * elf_load (inner zone) vs the spawn bookkeeping around it. */
    uint64_t t_spawn = perf_rdtsc();
    int pid = spawn_internal(spec->path, spec->name,
                             spec->fd0, spec->fd1, spec->fd2,
                             spec->fd3, spec->fd4,
                             spec->extra_fds, spec->extra_nfds,
                             spec->argc, spec->argv,
                             spec->envc, spec->envp,
                             spec->cwd);
    perf_zone_end(PERF_Z_PROC_SPAWN, t_spawn);
    if (pid < 0) return pid;

    /* Milestone 18: apply the sandbox profile now that the proc is
     * fully constructed. We deliberately narrow AFTER inheritance and
     * AFTER all the build steps so a spawn failure won't leave a
     * partially-applied policy behind. cap_profile_apply can never
     * widen caps (it AND's with the profile mask), so even if a
     * caller passes "unrestricted" to a narrow parent, the child
     * stays narrow. */
    if (spec->sandbox_profile && spec->sandbox_profile[0]) {
        struct proc *p = proc_lookup(pid);
        if (p) {
            (void)cap_profile_apply(p, spec->sandbox_profile);
            char caps_str[96];
            cap_mask_to_string(p->caps, caps_str, sizeof(caps_str));
            kprintf("[cap] pid=%d '%s' sandbox='%s' caps=%s root=%s\n",
                    p->pid, p->name, spec->sandbox_profile, caps_str,
                    p->sandbox_root[0] ? p->sandbox_root : "(none)");
        }
    }

    /* Milestone 34D: apply manifest-declared capabilities AFTER the
     * sandbox profile. Pure narrowing -- if the declared list asks for
     * a cap the profile already stripped, that cap stays stripped. The
     * point is to enforce least-privilege even when the package author
     * accidentally requests an over-broad sandbox profile. */
    if (spec->declared_caps && spec->declared_caps[0]) {
        struct proc *p = proc_lookup(pid);
        if (p) (void)cap_apply_declared(p, spec->declared_caps);
    }
    return pid;
}

/* C-side first entry: arrived here via proc_context_switch's ret on
 * the new process's kernel stack. We're running on the new process's
 * PML4 (CR3 was switched), TSS.RSP0 + g_kernel_syscall_rsp already
 * point at our kstack_top. Drop to ring 3. */
__attribute__((noreturn)) void proc_first_user_entry(void) {
    struct proc *p = current_proc();
    /* Slice 39: we arrived via proc_context_switch but NOT through
     * do_switch's post-switch line -- release the proc this CPU switched
     * away from, or it stays on_cpu (unschedulable) forever. */
    sched_finish_switch();
    /* Load this process's initial FPU/SSE state (we arrived via a context
     * switch that saved the PREVIOUS proc's state but couldn't restore ours
     * -- first-run procs never parked in sched_yield's restore). */
    fpu_restore(p->fpu_state);
    /* Ring-0 worker (proc_create_kernel): user_entry is a kernel
     * function -- CALL it in kernel mode; if it ever returns, exit. */
    if (p->is_kernel) {
        void (*kfn)(void) = (void (*)(void))p->user_entry;
        kfn();
        proc_exit(0);
    }
    /* For threads, pass the thread arg in RDI and load TLS base */
    if (p->is_thread) {
        if (p->tls_base)
            wrmsr(0xC0000100, p->tls_base);  /* MSR_FS_BASE */
        proc_enter_user_thread_asm(p->user_entry, p->user_rsp, p->user_arg);
    }
    proc_enter_user_asm(p->user_entry, p->user_rsp);
}

/* Wake any process blocked on `pid`. Marks them READY and re-enqueues
 * onto the scheduler. */
static void wakeup_waiters(int pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *q = &g_proc[i];
        if (q->state == PROC_BLOCKED && q->wait_pid == pid) {
            q->wait_pid = -1;
            q->state    = PROC_READY;
            sched_enqueue(q);
        }
    }
}

/* Public wrapper: a tracee entering a ptrace-stop has to wake a parent that is
 * already parked in proc_wait, and this is the mechanism proc_exit already
 * uses for the same job (BLOCKED + wait_pid match -> READY + enqueue). Reusing
 * it rather than inventing a second wake is deliberate -- an earlier attempt
 * hand-rolled one inside the signal path and wedged the guest. */
void proc_wake_waiters(int pid) { wakeup_waiters(pid); }

/* Wait for `pid` to either TERMINATE or enter a ptrace-stop belonging to the
 * caller. Returns 1 for the stop, 0 for termination (the caller then uses the
 * ordinary proc_wait to collect and reap), -1 if there is no such child.
 *
 * This exists because of a race that is easy to miss and fatal when hit: a
 * tracer typically forks and calls waitpid IMMEDIATELY, before the child has
 * run PTRACE_TRACEME. At that instant the child is not traced, so a test for
 * "is this child traced?" is false, and the caller commits to a wait that only
 * ever wakes on termination -- while the child goes on to trace itself and
 * park forever. Both sides then wait for the other. Observed exactly:
 *
 *   [fork] parent pid=2 -> child pid=3
 *   [ptrace] pid=3 TRACEME (tracer=2)
 *   [signal] pid=3 stopped by signal 19
 *   <nothing, ever>
 *
 * So the condition has to be re-evaluated after the block, not before it. */
int proc_wait_or_ptrace(int pid) {
    struct proc *self = current_proc();
    struct proc *child = proc_lookup(pid);
    if (!self || !child || pid == self->pid) return -1;
    for (;;) {
        if (child->ptrace_stopped && child->tracer_pid == self->pid) return 1;
        if (child->state == PROC_TERMINATED) return 0;
        self->wait_pid = pid;
        self->state    = PROC_BLOCKED;
        sched_yield();
        /* Woken by proc_exit's wakeup_waiters, or by proc_wake_waiters from a
         * ptrace-stop. Re-check both conditions -- which is the whole point. */
    }
}

/* Defined in syscall.c: if this process owns an active Linux fbdev mmap,
 * present its final frame to the scanout before the address space is torn
 * down (real fbdev programs draw into the mmap and exit without panning). */
extern void fbdev_proc_exit(int pid);

void proc_wait_off_cpu(struct proc *p) {
    if (!p) return;
    while (__atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

/* Slice 89: bounded variant for group teardown. A member wedged ON-CPU
 * (in-kernel spin that never parks) must not hang the whole exit_group
 * forever -- report failure so the caller can leak that slot instead.
 * ~2e8 pauses is seconds of real time: far beyond any legitimate park. */
bool proc_wait_off_cpu_bounded(struct proc *p) {
    if (!p) return true;
    for (uint64_t spins = 0; spins < 200000000ull; spins++) {
        if (!__atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE)) return true;
        __asm__ volatile("pause");
    }
    return false;
}

/* Linux exit_group(2): terminate every task in the caller's thread group.
 * A non-leader chrome thread used to call exit_group(191) and only kill
 * itself, leaving the browser zombie'd on X11 recvmsg with no GPU. */
__attribute__((noreturn)) void proc_exit_group(int code) {
    struct proc *p = current_proc();
    if (!p) proc_exit(code);

    if (p->is_thread) {
        int tgid = p->tgid;
        struct proc *leader = proc_lookup(tgid);
        if (bkl_held()) bkl_exit();
        for (int i = 0; i < PROC_MAX; i++) {
            struct proc *q = &g_proc[i];
            if (q == p || q->state == PROC_UNUSED ||
                q->state == PROC_EMBRYO) continue;
            if (!(q->tgid == tgid || q->pid == tgid)) continue;
            q->exit_code = code;
            q->state = PROC_TERMINATED;
            struct proc *w = q->join_waiters;
            while (w) {
                struct proc *nxt = w->next_wait;
                w->state = PROC_READY;
                w->next_wait = 0;
                sched_enqueue(w);
                w = nxt;
            }
            q->join_waiters = 0;
            sched_dequeue(q);
            { extern void futex_forget_proc(struct proc *); futex_forget_proc(q); }
            { extern void poll_forget_proc(struct proc *);  poll_forget_proc(q);  }
            /* Slice 89: a member parked in cow_fork_lock_acquire's quiesce
             * loop spins ON-CPU until vm_quiesce clears -- and the forker
             * that set it may be exactly who we just terminated, so nobody
             * would ever clear it. Release the member (its state is already
             * TERMINATED; it parks at the next tick/park point) or the
             * off-cpu wait below never returns and the WHOLE group teardown
             * hangs mid-way -- observed as exit_group(191) closing 9 sock
             * slots then never printing a single member [proc] exit line,
             * with half-dead threads running in the wreckage for minutes. */
            __atomic_store_n(&q->vm_quiesce, 0, __ATOMIC_RELEASE);
            if (!proc_wait_off_cpu_bounded(q)) {
                kprintf("[exitg] pid=%d never left cpu -- slot leaked, "
                        "kstack NOT freed\n", q->pid);
                continue;      /* leak beats freeing a live thread's stack */
            }
            if (q->is_thread) {
                if (q->kstack_base) kfree(q->kstack_base);
                q->kstack_base = 0;
                q->kstack_top  = 0;
                /* Slice 8: this path frees the slot WITHOUT going through
                 * proc_reap, so it owes the namespace release itself -- a
                 * thread holds its own reference (sys_clone_thread takes one). */
                nsproxy_release(q);
                q->state = PROC_UNUSED;
            } else {
                /* Leader: close shared fds; leave TERMINATED for wait/reap.
                 * Slice 89: close under the BKL. This loop runs with the BKL
                 * DROPPED (necessary for the off-cpu waits), but file_close
                 * refcounts (vfs_refs/sock refs/pipe writer counts) are plain
                 * non-atomic ints whose only guard IS the BKL -- closing here
                 * unlocked races every other CPU's open/close/dup on shared
                 * file objects (fd-table over-release: sockets torn down at
                 * refs=1 while the group still used them). */
                bkl_enter();
                close_all_fds(q);
                bkl_exit();
                wakeup_waiters(q->pid);
            }
        }
        (void)leader;
        /* Fall through: terminate the calling thread without closing fds
         * again (leader already closed the shared table). */
        p->is_thread = true; /* keep non-leader fd path in proc_exit */
    }
    proc_exit(code);
}

__attribute__((noreturn)) void proc_exit(int code) {
    struct proc *p = current_proc();

#ifdef CHROMIUM_BOOT
    /* If the renderer is terminating itself (e.g. the Mojo "no connection"
     * watchdog after 15 s) rather than being SIGKILL'd, capture its group's
     * user call chains here -- the exit path is the last moment its threads'
     * blocked stacks are still mapped. One-shot inside bt_dump_group. */
    if (p && p->is_renderer) {
        extern void bt_dump_group(int tgid);
        bt_dump_group(p->is_thread ? p->tgid : p->pid);
    }
#endif

    /* Share-until-exec: child _exit without exec must unblock the launcher. */
    if (p && p->vfork_parent > 0)
        vfork_child_done(p);

    /* Track B graphics: flush a Linux /dev/fb0 mmap to the display while this
     * process's pages are still mapped (before cli()/teardown below). */
    if (p) fbdev_proc_exit(p->is_thread ? p->tgid : p->pid);

    /* B11: Linux pthread_join. If this thread registered a clear_child_tid
     * (via clone(CLONE_CHILD_CLEARTID) or set_tid_address), write 0 to that
     * user word and FUTEX_WAKE it -- the joining thread is parked in
     * futex(FUTEX_WAIT, &tid). Done BEFORE cli() so a demand/CoW fault on the
     * word resolves with IRQs on, and while we're still in this thread's CR3. */
    if (p && p->clear_child_tid) {
        uint32_t zero = 0;
        (void)copy_to_user((void *)(uintptr_t)p->clear_child_tid,
                           &zero, sizeof(zero));
        (void)futex((uint32_t *)(uintptr_t)p->clear_child_tid, FUTEX_WAKE, 1,
                    0, 0);
        p->clear_child_tid = 0;
    }

    /* ---- Slice 10: PID-namespace exit semantics --------------------------
     * Deliberately BEFORE cli(): both branches call signal_send(), which does
     * wait_queue_unlink() + sched_enqueue() on the run queue. Doing that from a
     * cli()/IRQ context deadlocked the whole machine once (see the alarm-scan
     * placement note in signal.c) -- so this runs in ordinary process context
     * with interrupts on, exactly like sys_kill does.
     *
     * Both branches are no-ops in the initial namespace. */
    if (p && !p->is_thread) {
        if (pid_ns_is_init(p)) {
            /* Linux ties a pid namespace's lifetime to its init: when init
             * exits, every other member is SIGKILLed. Without this a container
             * whose init died would leave its processes running, still
             * invisible to the host's pid view -- unkillable by name. */
            pid_ns_kill_members(p);
        } else {
            /* Orphans inside a namespace reparent to that namespace's init,
             * not to the host's. Scoped to non-initial namespaces on purpose:
             * the initial namespace has never reparented orphans in this
             * kernel, and changing that as a side effect of a namespace slice
             * is a behaviour change nobody asked for (recorded as an open
             * item instead). */
            int reaper = pid_ns_reaper_kpid(p);
            if (reaper) {
                for (int i = 1; i < PROC_MAX; i++) {
                    struct proc *q = &g_proc[i];
                    if (q == p || q->state == PROC_UNUSED ||
                        q->state == PROC_EMBRYO) continue;
                    if (q->ppid == p->pid) q->ppid = reaper;
                }
            }
        }
    }

    /* SIGCHLD to the parent. The header of signal.c has promised this
     * since M1 and NOTHING ever sent it. bash's job table only updates
     * through its SIGCHLD handler; without the signal a job killed while
     * stopped stayed "stopped" in bash's books and an interactive `exit`
     * refused with "There are stopped jobs." forever. Default disposition
     * is ignore, so parents that don't care see nothing. Same
     * before-cli() reasoning as the namespace block above. */
    if (p && !p->is_thread && p->ppid > 0) {
        struct proc *par = proc_lookup(p->ppid);
        if (par) signal_send(par, SIGCHLD);
    }

    cli();

    p->exit_code   = code;

    if (signal_get_foreground() == p->pid) {
        signal_set_foreground(0);
    }

    /* Phase 1 M1.1: thread-group exit logic.
     * If we are the leader, terminate all threads in our group first.
     * If we are a non-leader thread, don't close shared fds. */
    if (!p->is_thread) {
        /* Drop BKL before waiting on remote on_cpu: a sibling spinning in
         * bkl_enter would never leave the CPU while we hold the lock. */
        if (bkl_held()) bkl_exit();
        /* Leader exiting -- kill all threads in this group */
        for (int i = 0; i < PROC_MAX; i++) {
            struct proc *q = &g_proc[i];
            if (q == p || q->state == PROC_UNUSED ||
                q->state == PROC_EMBRYO) continue;
            if (q->tgid == p->pid && q->is_thread) {
                /* Force-terminate the thread */
                q->exit_code = code;
                q->state = PROC_TERMINATED;
                /* Wake any joiners */
                struct proc *w = q->join_waiters;
                while (w) {
                    struct proc *nxt = w->next_wait;
                    w->state = PROC_READY;
                    w->next_wait = 0;
                    sched_enqueue(w);
                    w = nxt;
                }
                q->join_waiters = 0;
                /* Remove it from the ready queue BEFORE freeing its stack. A
                 * force-terminated sibling that was PROC_READY is still linked
                 * in a run queue; freeing its kstack and marking the slot
                 * UNUSED without unlinking leaves the scheduler able to pop it
                 * and switch in with kstack_top == NULL -> triple fault in
                 * syscall_entry. (Measured: pid=27 is_thread=1 tgid=17,
                 * kstack_top=0x0.) */
                sched_dequeue(q);
                { extern void futex_forget_proc(struct proc *); futex_forget_proc(q); }
                { extern void poll_forget_proc(struct proc *);  poll_forget_proc(q);  }
                /* Slice 88: wait until the sibling is off-CPU before freeing
                 * its kstack -- sched_dequeue does not stop a thread still
                 * running on another core.
                 * Slice 89: release a quiesce-parked spinner first, and bound
                 * the wait (see proc_exit_group -- same hang, same fix). */
                __atomic_store_n(&q->vm_quiesce, 0, __ATOMIC_RELEASE);
                if (!proc_wait_off_cpu_bounded(q)) {
                    kprintf("[exitg] pid=%d never left cpu -- slot leaked, "
                            "kstack NOT freed\n", q->pid);
                    continue;
                }
                /* Free the thread's kernel stack */
                if (q->kstack_base) kfree(q->kstack_base);
                q->kstack_base = 0;
                q->kstack_top  = 0;
                nsproxy_release(q);       /* slice 8 -- bypasses proc_reap */
                q->state = PROC_UNUSED;
            }
        }
        /* Slice 89: close under the BKL -- file_close refcounts are plain
         * ints guarded ONLY by the BKL, and this path runs with it dropped;
         * unlocked closes raced other CPUs' open/close/dup on the shared
         * objects (sockets torn down while still in use -> EPIPE storms,
         * fd-number reuse under live owners). IRQs must be ON while waiting
         * for the BKL: a remote holder mid-TLB-shootdown waits for THIS
         * cpu's ACK, which needs interrupts. */
        sti();
        bkl_enter();
        close_all_fds(p);
        bkl_exit();
        cli();
    } else {
        /* Non-leader thread: don't close shared fds, just wake joiners */
        struct proc *w = p->join_waiters;
        while (w) {
            struct proc *nxt = w->next_wait;
            w->state = PROC_READY;
            w->next_wait = 0;
            sched_enqueue(w);
            w = nxt;
        }
        p->join_waiters = 0;
    }

    p->state       = PROC_TERMINATED;

    /* Milestone 19 metric: one more process exited. */
    perf_count_proc_exit();

    kprintf("[proc] pid=%d '%s' exit code=%d (0x%x)"
            " cpu=%lu ms syscalls=%lu\n",
            p->pid, p->name, code, (unsigned)code,
            (unsigned long)(p->cpu_ns / 1000000ull),
            (unsigned long)p->syscall_count);

    wakeup_waiters(p->pid);

    /* Re-enable IRQs before yielding so the next process is preemptible
     * (even though we don't preempt today, the new process may itself
     * sti+hlt and rely on IRQs to wake it). */
    sti();

    sched_yield();          /* never returns: nothing puts us back ready */
    kpanic("proc_exit: sched_yield returned for terminated pid=%d", p->pid);
}

/* Free everything owned by a TERMINATED process and recycle the slot. */
static void proc_reap(struct proc *p) {
    if (!p || p->state != PROC_TERMINATED) return;

    /* Unlink from any ready queue before we free its stack and zero the slot.
     * The same reachable-after-teardown hazard the thread-group-exit path had. */
    sched_dequeue(p);
    { extern void futex_forget_proc(struct proc *); futex_forget_proc(p); }
    { extern void poll_forget_proc(struct proc *);  poll_forget_proc(p);  }
    proc_wait_off_cpu(p);

    /* SLICE 16: return this process's whole memory charge to its cgroup.
     *
     * Done from the process's OWN counter rather than by counting frames during
     * the page-table walk, so it is correct no matter which path released the
     * pages -- including vmm_destroy_user_pml4's bulk walk below, which frees the
     * user half without visiting the accounting at all. A per-frame uncharge there
     * would also double-count the pages a thread shares with its leader. */
    mm_user_pages_release_all(p);

    /* Tear down the address space only when NOBODY else is still in it.
     *
     * Threads share their leader's cr3 with owns_pml4 == false, and a thread
     * can outlive its leader -- chrome's GPU process is SIGKILLed, the leader
     * is reaped, and its threads are still in the table. Destroying the tables
     * there left those threads pointing at freed memory, and the next context
     * switch into one loaded a CR3 that no longer mapped the kernel: the
     * instruction fetch right after `mov %rdx,%cr3` faulted, the fault handler
     * was unmapped too, and the machine triple-faulted instantly -- no panic,
     * no log, just gone. (Measured: "switching to pid=21 with DEAD
     * cr3=0x1ce38000 (is_thread=1 tgid=16 owns_pml4=0)".)
     *
     * So: hand ownership to a surviving member instead of freeing. Every reap
     * re-runs this, so whoever is reaped LAST finds no heir and does the
     * teardown -- exactly once, and never while anyone can still run in it.
     * Note the old guard also required !is_thread; ownership can now land on a
     * thread, so owns_pml4 alone is authoritative. */
    if (p->owns_pml4 && p->cr3) {
        struct proc *heir = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            struct proc *q = &g_proc[i];
            if (q == p || q->state == PROC_UNUSED ||
                q->state == PROC_EMBRYO) continue;
            if (q->cr3 != p->cr3) continue;
            /* Prefer a still-runnable member; a TERMINATED-but-unreaped one is
             * an acceptable heir too, since it will run this same check. */
            heir = q;
            if (q->state != PROC_TERMINATED) break;
        }
        if (heir) {
            heir->owns_pml4 = true;
            p->owns_pml4    = false;
        } else {
            /* Clear before free so a concurrent do_switch sees owns_pml4=0 /
             * cr3=0 rather than a dangling live-looking pointer. */
            uint64_t doomed = p->cr3;
            p->cr3 = 0;
            p->owns_pml4 = false;
            /* One more barrier: no CPU may still be `current` in this space. */
            for (uint32_t c = 0; c < smp_cpu_count(); c++) {
                struct percpu *cpu = smp_cpu_mut(c);
                if (!cpu || !cpu->current) continue;
                while (cpu->current->cr3 == doomed)
                    __asm__ volatile("pause");
            }
            vmm_destroy_user_pml4(doomed);
            /* The page tables are gone; drop the VMA bookkeeping that
             * described them. This is the ONLY place it is unambiguously
             * safe: we are in the no-heir branch, so this was the last proc
             * in the address space, and the barrier above proved no CPU is
             * still running in it.
             *
             * Without this, g_vma_tables[] was never cleared at all -- and it
             * is keyed by proc_mm_pid(), which for an ordinary process is its
             * pid, which is a RECYCLED g_proc[] slot index. The next process
             * to land in this slot inherited these mappings and could fault a
             * fresh zero page into an address it never asked for. Read
             * proc_mm_pid BEFORE the memset at the end of this function. */
            mmap_cleanup_proc(proc_mm_pid(p));
        }
    }
    if (p->kstack_base) {
        kfree(p->kstack_base);
    }

    /* Slice 8: drop this process's namespace references. Must happen before
     * the memset below, which would otherwise lose the pointers and leak the
     * objects. nsproxy_release NULLs as it goes, so the two thread-teardown
     * paths that also call it cannot double-free. */
    nsproxy_release(p);

    int pid = p->pid;
#ifdef CHROMIUM_BOOT
    /* AUDIT BEFORE THE memset. Zeroing a struct that some list still points at
     * corrupts that list -- and the system wedges silently (no crash, no panic,
     * heartbeat simply stops) immediately after a slot is reaped and instantly
     * re-forked. Check whether this slot is still linked into a ready queue,
     * still flagged on_rq, or still on a wait queue at the moment we free it. */
    {
        int qcpu = -1; bool cyc = false;
        int queued = sched_debug_find_queued(p, &qcpu, &cyc);
        if (queued || p->on_rq || p->next_wait || p->wait_head) {
            kprintf("[reap!] pid=%d STILL LINKED: rq=%d(cpu=%d,cycle=%d) "
                    "on_rq=%d next_wait=%p wait_head=%p state=%d\n",
                    pid, queued, qcpu, (int)cyc, (int)p->on_rq,
                    (void *)p->next_wait, (void *)p->wait_head, (int)p->state);
        }
    }
#endif
    memset(p, 0, sizeof(*p));
    p->state = PROC_UNUSED;
    kprintf("[proc] reaped pid=%d, slot recycled\n", pid);
}

/* Find a child of `ppid` for Linux wait4(-1) ("wait for any child"). Prefers
 * an already-TERMINATED child (so a ready reap returns immediately); otherwise
 * returns the first live child so the caller can block on it. Returns the
 * child pid, or -1 if `ppid` has no children. */
int proc_any_child(int ppid) {
    int live = -1;
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *q = &g_proc[i];
        if (q->state == PROC_UNUSED || q->state == PROC_EMBRYO ||
            q->ppid != ppid) continue;
        if (q->state == PROC_TERMINATED) return q->pid;   /* reap-ready */
        if (live < 0) live = q->pid;
    }
    return live;
}

int proc_wait_info(int pid, struct proc_exit_info *out) {
    struct proc *self = current_proc();
    if (pid == self->pid) return -1;

    struct proc *child = proc_lookup(pid);
    if (!child) return -1;

    /* Milestone 19: stamp the wall clock at the moment of the wait
     * call so `time <cmd>` reports wall = [wait -> exit], which
     * matches the user's intuition ("the time I pressed enter until
     * the prompt came back") rather than [spawn -> exit]. Pre-perf
     * this is 0. */
    uint64_t wait_start_ns = perf_now_ns();

    while (child->state != PROC_TERMINATED) {
        self->wait_pid = pid;
        self->state    = PROC_BLOCKED;
        sched_yield();
        /* When we resume here, the child should be TERMINATED (or some
         * other event woke us spuriously -- loop again). */
    }

    int code = child->exit_code;

    /* Milestone 19: capture per-proc metrics BEFORE reap wipes them. */
    if (out) {
        out->pid           = child->pid;
        out->exit_code     = code;
        out->cpu_ns        = child->cpu_ns;
        out->syscall_count = child->syscall_count;
        out->wall_ns       = perf_now_ns() - wait_start_ns;
        size_t n = strlen(child->name);
        if (n >= PROC_NAME_MAX) n = PROC_NAME_MAX - 1;
        memcpy(out->name, child->name, n);
        out->name[n] = '\0';
    }

    proc_reap(child);
    return code;
}

int proc_wait(int pid) {
    /* Back-compat wrapper: callers that don't care about metrics
     * still get the exit code. */
    return proc_wait_info(pid, 0);
}

/* ===================================================================
 * Milestone 25A: per-process heap (brk)
 *
 * Each user proc owns a contiguous user-half region [brk_base, brk_max)
 * pre-baked at spawn (see USER_HEAP_BASE / USER_HEAP_MAX_BYTES).
 * Pages get mapped on demand as the user-side malloc grows the heap
 * via SYS_BRK. Unmapping happens when the user shrinks brk back below
 * a previously committed page boundary -- libc rarely does this in
 * practice, but the kernel honors it so a long-running daemon can
 * release its peak working set if it wants to.
 *
 * Constraints we enforce:
 *   - Always page-align internally (we map whole pages).
 *   - Never grow past brk_max.
 *   - Never shrink below brk_base.
 *   - Caller's PML4 must be the active one (we don't switch CR3 here);
 *     this is true on the syscall path where current_proc() == p.
 *
 * Returns the new brk on success, or 0 on any failure. proc_brk(p, 0)
 * is the canonical way to QUERY the current brk without changing it.
 * =================================================================== */
uint64_t proc_brk(struct proc *p, uint64_t new_brk) {
    if (!p) return 0;
    /* pid 0 (the kernel) has no per-proc heap -- it uses the kernel
     * heap (kmalloc/kfree). Refuse the call cleanly. */
    if (p->brk_base == 0) return 0;

    /* Query mode. */
    if (new_brk == 0) return p->brk_cur;

    if (new_brk < p->brk_base || new_brk > p->brk_max) return 0;

    /* Page-align. We grow up to the next page; we shrink down to the
     * next page boundary BELOW or EQUAL to new_brk so we never unmap
     * a page that contains live bytes the user expects. */
    uint64_t old_aligned = (p->brk_cur + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t new_aligned = (new_brk + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    bool growing = new_brk > p->brk_cur;

    /* CRITICAL: vmm_map / vmm_unmap walk the *editor root* g_pml4, not
     * the CPU's CR3. On the syscall path CR3 is already p->cr3 (user
     * never switches CR3 on `syscall`), but g_pml4 is whatever was last
     * set by vmm_set_active_root -- and spawn_internal restores it to
     * the kernel PML4 once it's done populating the new user half. So
     * if we just called vmm_map() here we'd silently install the new
     * heap pages into the *kernel* PML4, the user's CR3 wouldn't see
     * them, and the next user-mode heap access would page-fault.
     *
     * Use the editor-only swap (vmm_set_editor_root) so we retarget
     * g_pml4 without touching CR3. CR3 already points at p->cr3, and
     * restoring the *editor* back to old_root on exit must NOT yank
     * CR3 along with it -- doing so would land the kernel PML4 in
     * CR3 just before sysretq, and the user would instruction-fetch
     * fault on the very next ring-3 instruction. */
    uint64_t old_root = vmm_set_editor_root(p->cr3);

    uint64_t result = 0;

    if (growing) {
        /* Map [old_aligned, new_aligned). */
        for (uint64_t va = old_aligned; va < new_aligned; va += PAGE_SIZE) {
            uint64_t phys = mm_user_page_alloc(p);  /* slice 16: charged */
            if (phys == 0) {
                /* OOM partway through: roll back the pages we just
                 * mapped so the proc's address space stays consistent
                 * with brk_cur. */
                for (uint64_t r = old_aligned; r < va; r += PAGE_SIZE) {
                    uint64_t rphys = vmm_translate(r) & ~((uint64_t)PAGE_SIZE - 1);
                    vmm_unmap(r, PAGE_SIZE);
                    if (rphys) pmm_free_page(rphys);
                }
                kprintf("[proc] brk: OOM growing heap of pid=%d to %p\n",
                        p->pid, (void *)new_brk);
                goto out;
            }
            if (!vmm_map(va, phys, PAGE_SIZE,
                         VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_USER)) {
                pmm_free_page(phys);
                for (uint64_t r = old_aligned; r < va; r += PAGE_SIZE) {
                    uint64_t rphys = vmm_translate(r) & ~((uint64_t)PAGE_SIZE - 1);
                    vmm_unmap(r, PAGE_SIZE);
                    if (rphys) pmm_free_page(rphys);
                }
                kprintf("[proc] brk: vmm_map failed at %p\n", (void *)va);
                goto out;
            }
            /* Zero the fresh page -- libc malloc relies on it. */
            memset((void *)pmm_phys_to_virt(phys), 0, PAGE_SIZE);
        }
    } else if (new_aligned < old_aligned) {
        /* Shrinking past at least one page boundary: free those pages.
         * CoW-aware: a fork sibling may still map the frame (refs > 1);
         * drop only our reference and let the last owner free it.
         *
         * Slice 87: was free-THEN-shootdown (the classic stale-TLB reuse
         * window). Unmap + batch, shoot down, THEN free; on an un-acked
         * shootdown LEAK the batch (same trade as mmap.c quarantine). */
        enum { BRK_FREE_MAX = 256 };
        uint64_t doomed[BRK_FREE_MAX];
        int n_doom = 0;
        for (uint64_t va = new_aligned; va < old_aligned; va += PAGE_SIZE) {
            uint64_t phys = vmm_translate(va) & ~((uint64_t)PAGE_SIZE - 1);
            vmm_unmap(va, PAGE_SIZE);
            if (!phys) continue;
            int refs = page_ref_get(phys);
            if (refs > 1) {
                page_ref_dec(phys);
                continue;
            }
            if (refs == 1) page_ref_dec(phys);
            if (n_doom == BRK_FREE_MAX) {
                if (tlb_shootdown_remote_sync())
                    for (int i = 0; i < n_doom; i++) pmm_free_page(doomed[i]);
                /* !acked: leak this batch */
                n_doom = 0;
            }
            doomed[n_doom++] = phys;
        }
        if (n_doom) {
            if (tlb_shootdown_remote_sync())
                for (int i = 0; i < n_doom; i++) pmm_free_page(doomed[i]);
        }
    }

    p->brk_cur = new_brk;
    result = p->brk_cur;

out:
    vmm_set_editor_root(old_root);
    return result;
}
