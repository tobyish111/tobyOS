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

/* ===================================================================
 * sys_fork -- Create a child process that is a copy of the parent.
 *
 * Returns child PID to the parent, 0 to the child.
 * =================================================================== */
long sys_fork(void) {
    struct proc *parent = current_proc();
    if (!parent || parent->pid == 0) return -ABI_EINVAL;

    /* Find a free slot. */
    struct proc *child = NULL;
    for (int i = 1; i < PROC_MAX; i++) {
        if (g_proc[i].state == PROC_UNUSED) {
            child = &g_proc[i];
            break;
        }
    }
    if (!child) return -ABI_ENOMEM;

    /* Copy parent's proc struct as a starting point. */
    int child_pid = (int)(child - g_proc);
    memcpy(child, parent, sizeof(*child));

    child->pid       = child_pid;
    child->ppid      = parent->pid;
    child->state     = PROC_UNUSED;
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
        memset(child, 0, sizeof(*child));
        child->state = PROC_UNUSED;
        return -ABI_ENOMEM;
    }
    child->cr3       = new_pml4;
    child->owns_pml4 = true;

    /* COW fork: share parent pages as copy-on-write instead of deep copy. */
    if (vmm_cow_fork(parent->cr3, new_pml4) != 0) {
        vmm_destroy_user_pml4(new_pml4);
        memset(child, 0, sizeof(*child));
        child->state = PROC_UNUSED;
        return -ABI_ENOMEM;
    }
    mmap_cow_clone(parent->pid, child_pid);

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
    for (int i = 0; i < PROC_NFDS; i++) {
        if (ptab && ptab[i]) {
            child->fds[i] = file_clone(ptab[i]);
        } else {
            child->fds[i] = NULL;
        }
    }

    /* Build the child's kernel stack. */
    if (!build_fork_kstack(child)) {
        for (int i = 0; i < PROC_NFDS; i++) {
            if (child->fds[i]) file_close(child->fds[i]);
            child->fds[i] = NULL;
        }
        vmm_destroy_user_pml4(new_pml4);
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

    /* Ready to run. */
    child->state = PROC_READY;
    sched_enqueue(child);

    child->user_pages = parent->user_pages;
    perf_count_proc_spawn();

    kprintf("[fork] parent pid=%d -> child pid=%d cr3=0x%lx\n",
            parent->pid, child->pid, child->cr3);

    return child->pid;  /* parent gets child PID */
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

    struct proc *child = NULL;
    for (int i = 1; i < PROC_MAX; i++) {
        if (g_proc[i].state == PROC_UNUSED) { child = &g_proc[i]; break; }
    }
    if (!child) return -ABI_ENOMEM;
    int child_pid = (int)(child - g_proc);

    memcpy(child, parent, sizeof(*child));
    child->pid        = child_pid;
    child->ppid       = parent->ppid;
    child->tgid       = tg->pid;                /* same thread group */
    child->is_thread  = true;
    child->cr3        = tg->cr3;                /* SHARE the address space */
    child->owns_pml4  = false;                  /* never free the shared PML4 */
    child->detached   = true;
    child->state      = PROC_UNUSED;
    child->wait_pid   = -1;
    child->exit_code  = -1;
    child->next_ready = NULL;
    /* Slice 39: same reset as sys_fork -- the memcpy copied the parent's
     * live on_cpu/on_rq, which would make this thread unschedulable. */
    child->on_rq  = false;
    child->on_cpu = 0;
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
     * shared address space (we're already on the shared CR3). */
    if ((flags & 0x00100000u /* CLONE_PARENT_SETTID */) && ptid)
        (void)put_user_u32((void *)ptid, (uint32_t)child_pid);
    if ((flags & 0x01000000u /* CLONE_CHILD_SETTID */) && ctid)
        (void)put_user_u32((void *)ctid, (uint32_t)child_pid);

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
                      const char *kpath, int kargc, char **kargv_buf,
                      uint64_t old_pml4, uint64_t new_pml4,
                      uint64_t saved_cr3, uint64_t old_editor) {
    struct pe_load_info pe_info = {0};
    int prc = pe_load_user(image, image_size, kargc, kargv_buf, &pe_info);
    kfree(image);

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

    /* Copy path into a kernel buffer (per-copy uaccess). */
    char kpath[ABI_PATH_MAX];
    long plen = strncpy_from_user(kpath, path, sizeof(kpath));
    if (plen < 0) return -ABI_EFAULT;
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

    /* Read the ELF image from VFS. */
    void  *image      = NULL;
    size_t image_size = 0;
    int rc = vfs_read_all(kpath, &image, &image_size);
    if (rc != 0) {
        kprintf("[execve] cannot read '%s': %d\n", kpath, rc);
        return -ABI_ENOENT;
    }

    /* Destroy the old user-half mappings and rebuild the address space.
     * We destroy the OLD PML4 and create a fresh one. */
    uint64_t old_pml4 = p->cr3;
    uint64_t new_pml4 = vmm_create_user_pml4();
    if (!new_pml4) {
        kfree(image);
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
        return execve_pe(p, image, image_size, kpath, kargc, kargv_buf,
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
    bool ok = elf_load_user_at(image, image_size, prog_load_base, &prog_info);
    kfree(image);

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
    }

    /* Build user stack. */
    if (ok) {
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
    struct abi_auxv aux[10];
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

            *(uint64_t *)argc_va = (uint64_t)(uint32_t)kargc;
            uaccess_end(uflags);
            user_rsp = argc_va;
        }
    }

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

    /* Destroy the old PML4 now that we're on the new one. */
    if (old_pml4 != new_pml4 && p->owns_pml4) {
        vmm_destroy_user_pml4(old_pml4);
    }

    p->cr3       = new_pml4;
    p->owns_pml4 = true;

    /* Reset heap. */
    p->brk_base = USER_HEAP_BASE;
    p->brk_cur  = USER_HEAP_BASE;
    p->brk_max  = USER_HEAP_BASE + USER_HEAP_MAX_BYTES;

    /* B11: the new image hasn't registered a pthread-exit futex yet; drop any
     * clear_child_tid carried over from the replaced image. */
    p->clear_child_tid = 0;

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
