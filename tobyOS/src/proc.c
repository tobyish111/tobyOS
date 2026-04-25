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
#include <tobyos/heap.h>
#include <tobyos/vfs.h>
#include <tobyos/elf.h>
#include <tobyos/tss.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/klibc.h>
#include <tobyos/file.h>
#include <tobyos/cap.h>
#include <tobyos/perf.h>

/* Asm helpers from proc_switch.S. */
extern __attribute__((noreturn)) void proc_enter_user_asm(uint64_t rip,
                                                          uint64_t rsp);

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
#define USER_STACK_PAGES     4
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

static struct proc g_proc[PROC_MAX];
struct proc       *g_current_proc;

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
    }
    return "?";
}

struct proc *proc_lookup(int pid) {
    if (pid < 0 || pid >= PROC_MAX) return 0;
    struct proc *p = &g_proc[pid];
    return p->state == PROC_UNUSED ? 0 : p;
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

static struct proc *alloc_slot(void) {
    /* Slot 0 is reserved for kernel_main. Search 1..PROC_MAX-1. */
    for (int i = 1; i < PROC_MAX; i++) {
        if (g_proc[i].state == PROC_UNUSED) return &g_proc[i];
    }
    return 0;
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

    g_current_proc = k;

    kprintf("[proc] table initialised (%d slots), pid 0 = '%s' RUNNING\n",
            PROC_MAX, k->name);
}

/* ---- per-process fd table helpers --------------------------------- */

static void close_all_fds(struct proc *p) {
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
                                struct file *fd2) {
    struct file *src[3] = { fd0, fd1, fd2 };
    for (int i = 0; i < 3; i++) {
        struct file *nf;
        if (src[i]) {
            nf = file_clone(src[i]);
        } else {
            nf = console_file_make();
        }
        if (!nf) {
            close_all_fds(p);
            return false;
        }
        p->fds[i] = nf;
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
        uint64_t phys = pmm_alloc_page();
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
static int spawn_internal(const char *path, const char *name,
                          struct file *fd0, struct file *fd1, struct file *fd2,
                          int argc, char **argv,
                          int envc, char **envp,
                          const char *cwd_override) {
    if (!path) return -1;

    struct proc *p = alloc_slot();
    if (!p) {
        kprintf("[proc] cannot create '%s': process table full\n", path);
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->pid       = (int)(p - g_proc);
    p->state     = PROC_UNUSED;       /* upgraded to READY at the very end */
    p->wait_pid  = -1;
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
    /* Inherit session tag + user identity from the parent
     * (current_proc()). pid 0 has session_id == 0 / uid == 0 unless
     * the desktop launcher temporarily flipped them to the active
     * session's tag + uid -- which is exactly how desktop-launched
     * apps get tagged with the user's session AND end up running as
     * that user. */
    {
        struct proc *parent = current_proc();
        p->session_id = parent ? parent->session_id : 0;
        p->uid        = parent ? parent->uid        : 0;
        p->gid        = parent ? parent->gid        : 0;
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

    /* ---- 0. inherit fds (clone the explicit ones, default the rest) ---- */
    if (!install_initial_fds(p, fd0, fd1, fd2)) {
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

    /* Milestone 25D: peek for PT_INTERP BEFORE loading. Done outside
     * the CR3 swap window because elf_peek_interp only reads from the
     * kernel-virtual `image` buffer -- no user mappings involved. */
    char interp_path[ABI_PATH_MAX];
    bool has_interp = elf_peek_interp(image, image_size,
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
    struct elf_load_info prog_info = {0};
    bool ok = elf_load_user_at(image, image_size, prog_load_base, &prog_info);
    perf_zone_end(PERF_Z_ELF_LOAD, t_elf);
    kfree(image);                  /* segments now live in their own frames */

    /* Milestone 25D: when the program declares PT_INTERP, also load
     * the interpreter (the dynamic linker, /lib/ld-toby.so) into the
     * same address space at a non-overlapping base. The kernel's
     * single role here is to make both images resident and to give
     * the interpreter the auxv it needs to find the program's PHDRs;
     * actual relocation and symbol resolution happen in user mode. */
    struct elf_load_info interp_info = {0};
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
    struct abi_auxv aux[8];
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
    }

    /* Default RSP if no argv -- pack a canonical
     *   { argc=0, argv=[NULL], envp=[NULL], auxv=[AT_NULL] }
     * frame anyway so the user-side trampoline always finds a valid
     * layout at the top of the stack. */
    uint64_t user_rsp = USER_STACK_RSP_INIT;
    if (ok) {
        struct user_stack_pack pack = {
            .argc = argc, .argv = argv,
            .envc = envc, .envp = envp,
            .auxv = aux,  .auxc = auxc,
        };
        ok = pack_user_stack(&pack, &user_rsp);
    }

    if (read_cr3() != saved_cr3) write_cr3(saved_cr3);
    vmm_set_editor_root(old_editor);

    if (!ok) {
        vmm_destroy_user_pml4(pml4);
        close_all_fds(p);
        memset(p, 0, sizeof(*p));
        p->state = PROC_UNUSED;
        return -1;
    }

    /* Initial RIP: hand control to the dynamic linker if present so
     * it can self-relocate, load DT_NEEDED libraries, and resolve
     * relocations before jumping to AT_ENTRY. */
    p->user_entry = has_interp ? interp_info.entry : prog_info.entry;
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

    /* Milestone 19: approximate RSS -- we only count the pages we
     * explicitly mapped for this proc (user stack + vmm-style page
     * walk over the user half is expensive). This is "user_pages",
     * i.e. a lower bound on resident set size. Enough for ps/top. */
    p->user_pages = p->user_stack_pages + 1 /* rough elf overhead */;

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
    return spawn_internal(path, name, 0, 0, 0, 0, 0, 0, 0, 0);
}

int proc_spawn(const struct proc_spec *spec) {
    if (!spec) return -1;
    /* Milestone 19: span the whole spawn pipeline so `perf` can show
     * elf_load (inner zone) vs the spawn bookkeeping around it. */
    uint64_t t_spawn = perf_rdtsc();
    int pid = spawn_internal(spec->path, spec->name,
                             spec->fd0, spec->fd1, spec->fd2,
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

__attribute__((noreturn)) void proc_exit(int code) {
    /* Disable IRQs while we tweak shared state. The exiting context
     * has its own kstack, but if a timer IRQ fired between marking
     * TERMINATED and yielding, the IRQ handler running on this same
     * stack would still find a coherent snapshot -- belt-and-braces. */
    cli();

    struct proc *p = current_proc();
    p->exit_code   = code;

    /* If we WERE the foreground process, the shell's perception of who
     * owns Ctrl+C must be cleared right now -- otherwise the next
     * Ctrl+C between our exit and the shell reaping us would target a
     * TERMINATED PCB (signal_send no-ops on that, but we still want
     * the foreground slot reset cleanly). */
    if (signal_get_foreground() == p->pid) {
        signal_set_foreground(0);
    }

    /* Close all fds BEFORE flipping to TERMINATED. This lets pipe ends
     * decrement their reader/writer counts immediately -- so a sibling
     * process blocked on read() sees EOF the instant we exit, regardless
     * of whether/when our parent gets around to reaping us.
     *
     * Safe to do at this point: we won't run any more user code, and we
     * still own the kernel stack we're standing on. The PML4 + kstack
     * stay valid for the eventual proc_reap. */
    close_all_fds(p);

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

    if (p->owns_pml4 && p->cr3) {
        /* Safe because the caller (the parent) is running on the
         * kernel PML4 -- we are NOT pulling the rug out from under
         * the active CR3. */
        vmm_destroy_user_pml4(p->cr3);
    }
    if (p->kstack_base) {
        kfree(p->kstack_base);
    }

    int pid = p->pid;
    memset(p, 0, sizeof(*p));
    p->state = PROC_UNUSED;
    kprintf("[proc] reaped pid=%d, slot recycled\n", pid);
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
            uint64_t phys = pmm_alloc_page();
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
        /* Shrinking past at least one page boundary: free those pages. */
        for (uint64_t va = new_aligned; va < old_aligned; va += PAGE_SIZE) {
            uint64_t phys = vmm_translate(va) & ~((uint64_t)PAGE_SIZE - 1);
            vmm_unmap(va, PAGE_SIZE);
            if (phys) pmm_free_page(phys);
        }
    }

    p->brk_cur = new_brk;
    result = p->brk_cur;

out:
    vmm_set_editor_root(old_root);
    return result;
}
