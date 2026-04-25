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

static void default_exception(struct regs *r) {
    bool from_user = (r->cs & 3) == 3;

    kprintf("\n*** EXCEPTION %lu: %s%s ***\n",
            r->vector, exc_name(r->vector),
            from_user ? "  (in user mode)" : "");
    dump_regs(r);

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
