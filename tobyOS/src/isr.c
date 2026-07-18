/* isr.c -- C-side interrupt dispatcher and default exception handler.
 *
 * Every interrupt enters here through the asm trampoline in isr.S. We
 * fan out via `g_handlers[]` (registered with isr_register), and fall
 * back to a generic exception dump + panic for vectors 0..31 that
 * nobody claimed.
 */

#include <tobyos/isr.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/cpu.h>
#include <tobyos/proc.h>
#include <tobyos/vmm.h>
#include <tobyos/page_fault.h>
#include <tobyos/pmm.h>
#include <tobyos/smp.h>
#include <tobyos/percpu.h>   /* MAX_CPUS */
#include <tobyos/signal.h>

/* During pid-0 bring-up, demand-map HHDM mirror gaps (UEFI memmap
 * holes, GOP framebuffer tagged RESERVED, freshly PMM'd pages). */
static bool kernel_boot_demand_map(uint64_t fault_addr) {
    struct proc *p = current_proc();
    if (!p || p->pid != 0)
        return false;
    if (vmm_kernel_pml4_phys() == 0)
        return false;

    uint64_t hhdm = vmm_hhdm_offset();
    if (hhdm == 0)
        return false;

    uint64_t page = fault_addr & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t phys;
    uint32_t flags = VMM_PRESENT | VMM_WRITE | VMM_NX;

    if (fault_addr >= hhdm) {
        phys = page - hhdm;
    } else if (fault_addr < 0x100000000ULL) {
        /* Code still dereferencing a raw physical GOP address. Map the
         * page into HHDM and alias it at the low virt the caller used. */
        phys = page;
        flags |= VMM_NOCACHE;
        if (vmm_translate(hhdm + phys) == 0) {
            if (!vmm_hhdm_ensure_mapped(phys, PAGE_SIZE, flags))
                return false;
        }
        if (vmm_translate(page) == 0) {
            if (!vmm_map(page, phys, PAGE_SIZE, flags))
                return false;
        }
        return true;
    } else {
        return false;
    }

    return vmm_hhdm_ensure_mapped(phys, PAGE_SIZE, flags);
}

static const char *exc_name(uint64_t v) {
    static const char *names[32] = {
        "Divide-by-zero",            /*  0 #DE */
        "Debug",                     /*  1 #DB */
        "NMI",                       /*  2     */
        "Breakpoint",                /*  3 #BP */
        "Overflow",                  /*  4 #OF */
        "Bound Range Exceeded",      /*  5 #BR */
        "Invalid Opcode",            /*  6 #UD */
        "Device Not Available",      /*  7 #NM */
        "Double Fault",              /*  8 #DF */
        "Coprocessor Segment",       /*  9     */
        "Invalid TSS",               /* 10 #TS */
        "Segment Not Present",       /* 11 #NP */
        "Stack-Segment Fault",       /* 12 #SS */
        "General Protection",        /* 13 #GP */
        "Page Fault",                /* 14 #PF */
        "Reserved",                  /* 15     */
        "x87 FPU Exception",         /* 16 #MF */
        "Alignment Check",           /* 17 #AC */
        "Machine Check",             /* 18 #MC */
        "SIMD FP Exception",         /* 19 #XM */
        "Virtualization",            /* 20 #VE */
        "Control Protection",        /* 21 #CP */
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor Injection",      /* 28 #HV */
        "VMM Communication",         /* 29 #VC */
        "Security",                  /* 30 #SX */
        "Reserved",                  /* 31     */
    };
    return v < 32 ? names[v] : "(unknown)";
}

static isr_handler_fn g_handlers[256];

void isr_register(uint8_t vector, isr_handler_fn fn) {
    g_handlers[vector] = fn;
}

static void dump_regs(struct regs *r) {
    kprintf("  rip=%p  cs=0x%lx  rflags=0x%lx\n",
            (void *)r->rip, r->cs, r->rflags);
    kprintf("  rsp=%p  ss=0x%lx  err=0x%lx\n",
            (void *)r->rsp, r->ss, r->error_code);
    kprintf("  rax=0x%016lx  rbx=0x%016lx  rcx=0x%016lx\n",
            r->rax, r->rbx, r->rcx);
    kprintf("  rdx=0x%016lx  rsi=0x%016lx  rdi=0x%016lx\n",
            r->rdx, r->rsi, r->rdi);
    kprintf("  rbp=0x%016lx  r8 =0x%016lx  r9 =0x%016lx\n",
            r->rbp, r->r8, r->r9);
    kprintf("  r10=0x%016lx  r11=0x%016lx  r12=0x%016lx\n",
            r->r10, r->r11, r->r12);
    kprintf("  r13=0x%016lx  r14=0x%016lx  r15=0x%016lx\n",
            r->r13, r->r14, r->r15);
    kprintf("  cr2=%p  cr3=%p  cr0=0x%lx  cr4=0x%lx\n",
            (void *)read_cr2(), (void *)read_cr3(),
            read_cr0(), read_cr4());
}

/* ---- repeated-fault (livelock) detector + mitigation --------------
 *
 * A page fault that the handler REPORTS as resolved but that then
 * immediately re-faults at the same rip+addr spins the process forever:
 * it stays RUNNABLE, makes no forward progress, never returns to its
 * event loop, yet each fault is brief and drops the BKL, so pid-0's
 * heartbeat keeps ticking -- the exact fingerprint of the EliteDesk
 * File Explorer freeze (window went unresponsive + empty; kernel
 * healthy; no crash logged). The prime suspect is a stale TLB entry on
 * a real multi-core box (this kernel has no cross-CPU TLB shootdown),
 * which QEMU-TCG does not reproduce.
 *
 * Per-CPU tracking: if the SAME (rip,addr) resolves PFLOOP_TRIP times in
 * a row, log it loudly (greppable [PFLOOP], names the exact faulting
 * instruction + address) and reload CR3 to flush this CPU's whole TLB.
 * A full TLB flush is always safe; if the loop was TLB-staleness it
 * ends here, and either way the log pins down the culprit on the next
 * real-HW boot. */
#define PFLOOP_TRIP  4096
static struct { uint64_t rip, addr; uint32_t count; bool warned; }
    g_pfloop[MAX_CPUS];

static void pfloop_note_resolved(struct regs *r, uint64_t fault_addr) {
    uint32_t ci = smp_current_cpu_idx();
    if (ci >= MAX_CPUS) return;
    struct proc *cp = current_proc();
    if (cp) { cp->fault_count++; cp->last_fault_rip = r->rip;
              cp->last_fault_cr2 = fault_addr; }
    if (g_pfloop[ci].rip == r->rip && g_pfloop[ci].addr == fault_addr) {
        if (++g_pfloop[ci].count >= PFLOOP_TRIP) {
            if (!g_pfloop[ci].warned) {
                kprintf("[PFLOOP] cpu%u pid=%d '%s' spun on the SAME fault "
                        "%u times: rip=%p cr2=%p err=0x%lx -- flushing TLB "
                        "(likely missing SMP shootdown)\n",
                        ci, cp ? cp->pid : -1, cp ? cp->name : "?",
                        (unsigned)g_pfloop[ci].count, (void *)r->rip,
                        (void *)fault_addr, r->error_code);
                g_pfloop[ci].warned = true;
            }
            write_cr3(read_cr3());          /* full TLB flush -- always safe */
            g_pfloop[ci].count = 0;
        }
    } else {
        g_pfloop[ci].rip = r->rip; g_pfloop[ci].addr = fault_addr;
        g_pfloop[ci].count = 1; g_pfloop[ci].warned = false;
    }
}

static void default_exception(struct regs *r) {
    bool from_user = (r->cs & 3) == 3;

    /* Vector 3 (#BP / int3). A KERNEL-mode int3 is a deliberate debug-marker
     * breakpoint: log it and continue (this was a separately-registered
     * breakpoint_handler before). A USER-mode int3 is a program's own trap --
     * Chromium's IMMEDIATE_CRASH()/CHECK()/DCHECK() emit it -- and must be
     * delivered as SIGTRAP (handled by the from_user signal switch below), never
     * silently resumed. (IDT[3] is DPL 3 so this path is reached at all.) */
    if (r->vector == 3 && !from_user) {
        kprintf("[isr] breakpoint hit at rip=%p (rflags=0x%lx)\n",
                (void *)r->rip, r->rflags);
        return;
    }

    /* Phase 1 M1.2: demand paging for page faults (vector 14).
     * Try to handle the fault via the VMA/mmap system before killing. */
    if (r->vector == 14) {
        uint64_t fault_addr = read_cr2();
        if (from_user) {
            if (page_fault_handler(fault_addr, r->error_code, current_proc())) {
                pfloop_note_resolved(r, fault_addr);
                return; /* fault resolved via COW / demand-zero / swap-in */
            }
            if (mmap_handle_page_fault(fault_addr, r->error_code)) {
                pfloop_note_resolved(r, fault_addr);
                return; /* fault resolved via mmap demand paging */
            }
        } else {
            /* Kernel-mode fault on a USER-HALF address: under the syscall-wide
             * SMAP stac window the kernel legitimately writes user memory
             * (syscall out-params, the signal frame pushed onto the user
             * stack), and after a CoW fork those pages are write-protected --
             * the fault must take the same COW/demand path a ring-3 write
             * would. Without this, the first kernel write into a post-fork
             * proc's user memory was fatal. Kernel-half addresses still take
             * the boot demand-map / fatal path below. */
            struct proc *cp = current_proc();
            if (fault_addr < 0x0000800000000000ULL && cp && cp->pid != 0) {
                if (page_fault_handler(fault_addr, r->error_code, cp))
                    return;
                if (mmap_handle_page_fault(fault_addr, r->error_code))
                    return;
            }
            if (kernel_boot_demand_map(fault_addr))
                return;
        }
    }

    /* B15: a ring-3 CPU fault that maps to a catchable POSIX signal is offered
     * to a user-installed handler before the fatal path. If the process
     * registered one (with a sigreturn trampoline), signal_deliver_fault
     * rewrites this trapframe so the trailing iretq enters the handler; we
     * return straight away. Otherwise it returns false and we fall through to
     * the diagnostic dump + terminate exactly as before. */
    if (from_user) {
        int sig = 0, code = 0;
        uint64_t addr = 0;
        switch (r->vector) {
        case 0:  sig = SIGFPE;  code = FPE_INTDIV; break;   /* #DE */
        case 3:  sig = SIGTRAP; code = TRAP_BRKPT; break;   /* #BP (int3) */
        case 4:  sig = SIGSEGV; code = SEGV_MAPERR; break;  /* #OF (into) */
        case 6:  sig = SIGILL;  code = ILL_ILLOPC; break;   /* #UD */
        case 13: sig = SIGSEGV; code = SEGV_ACCERR; break;  /* #GP */
        case 14:                                            /* #PF */
            sig  = SIGSEGV;
            addr = read_cr2();
            code = (r->error_code & 0x1) ? SEGV_ACCERR : SEGV_MAPERR;
            break;
        case 16: case 19: sig = SIGFPE; code = FPE_FLTDIV; break; /* #MF/#XM */
        default: break;
        }
        if (sig && signal_deliver_fault(r, sig, code, addr))
            return;   /* iretq lands in the user handler */
    }

    kprintf("\n*** EXCEPTION %lu: %s%s ***\n",
            r->vector, exc_name(r->vector),
            from_user ? "  (in user mode)" : "");
    dump_regs(r);

    /* SMEP diagnostic: a supervisor-mode #PF with the instruction-fetch bit
     * set (err bit 4) on a present user page means the kernel jumped INTO a
     * user-mapped page. This is the AP-first-run / argc>=1 / SMAP bug. Surface
     * the entry-path fields so the fault frame is self-explanatory (compare
     * fault rip/cr2 against user_rsp / user_entry to see what we jumped to). */
    if (r->vector == 14 && !from_user && (r->error_code & 0x10)) {
        /* Stop taking IRQs on THIS CPU so the dump below isn't interleaved
         * on the serial wire with another core's logging / pid-0 heartbeat.
         * The fault is fatal (we panic at the end) so losing IRQs is fine. */
        cli();
        struct proc *cp = current_proc();
        struct percpu *me = smp_this_cpu();
        /* Banner markers chosen to be trivially greppable in a captured
         * serial stream from the EliteDesk (null-modem @ 38400 8N1). */
        kprintf("\n===SMEP-FAULT-BEGIN===\n");
        kprintf("  [SMEP] kernel instruction-fetch from a user page!\n");
        kprintf("  [SMEP] cpu=%u  pid=%d '%s'  is_idle=%d is_thread=%d\n",
                me ? me->cpu_idx : 0,
                cp ? cp->pid : -1, cp ? cp->name : "(null)",
                cp ? cp->is_idle : -1, cp ? cp->is_thread : -1);
        if (cp) {
            kprintf("  [SMEP] user_entry=%p user_rsp=%p\n",
                    (void *)cp->user_entry, (void *)cp->user_rsp);
            kprintf("  [SMEP] kstack_top=%p saved_rsp=%p proc.cr3=%p\n",
                    cp->kstack_top, (void *)cp->saved_rsp, (void *)cp->cr3);
            kprintf("  [SMEP] fault_rip - user_rsp = %ld (0x%lx)\n",
                    (long)(r->rip - cp->user_rsp), r->rip - cp->user_rsp);
            kprintf("  [SMEP] fault_rip - user_entry = %ld (0x%lx)\n",
                    (long)(r->rip - cp->user_entry), r->rip - cp->user_entry);
        }
        kprintf("===SMEP-FAULT-END===\n");
    }

    if (from_user) {
        /* User code did something illegal -- bail out of ring 3 cleanly
         * instead of panicking the kernel. We're running on the
         * faulting process's per-process kernel stack (TSS.RSP0
         * pointed there at the last context switch). proc_exit marks
         * us TERMINATED, wakes the parent, and yields to the next
         * ready proc; the trap frame on this kstack is abandoned and
         * freed when the parent reaps. */
        kprintf("[isr] user-mode fault -- terminating user process "
                "pid=%d (rsp,rip both came from CPL=3)\n",
                current_proc()->pid);
        proc_exit(-1);
        /* unreachable */
    }

    kpanic("unhandled exception %lu (%s)", r->vector, exc_name(r->vector));
}

void isr_dispatch(struct regs *r) {
    isr_handler_fn fn = g_handlers[r->vector];
    if (fn) {
        fn(r);
        return;
    }
    if (r->vector < 32) {
        default_exception(r);
    }
    /* Vectors 32..255 with no handler: silently ignore for now.
     * Step 3 will register PIC IRQ handlers; later steps may add a
     * "spurious interrupt" warning here. */
}
