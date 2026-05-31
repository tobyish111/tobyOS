/* tss.c -- 64-bit TSS + dedicated kernel interrupt stack.
 *
 * Layout of the long-mode TSS we care about:
 *
 *   offset  field         size  purpose
 *   ------  ------------  ----  --------------------------------------
 *   0x04    rsp0          u64   stack used on CPL 3 -> 0 transition
 *   0x0C    rsp1          u64   (unused)
 *   0x14    rsp2          u64   (unused)
 *   0x24    ist1..ist7    7xu64 (unused for now)
 *   0x66    iomap_base    u16   set to sizeof(tss) -> "no I/O bitmap"
 *
 * The struct is packed so the awkward 4-byte alignment of rsp0 (which
 * actually starts at byte offset 4) lines up correctly.
 */

#include <tobyos/tss.h>
#include <tobyos/gdt.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/percpu.h>
#include <tobyos/smp.h>

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* 16 KiB kernel interrupt / syscall stack. Page-aligned so a page-fault
 * dump that prints rsp lands on a recognisable boundary. */
#define KERNEL_IRQ_STACK_SIZE (16 * 1024)

static uint8_t  g_kernel_irq_stack[KERNEL_IRQ_STACK_SIZE]
    __attribute__((aligned(16)));

/* One TSS per logical CPU. Each CPU loads RSP0 from ITS OWN TSS on a
 * CPL3->0 transition, so user code can run on multiple cores at once
 * without sharing one ring-0 stack. The scheduler keeps each CPU's
 * RSP0 pointed at the currently-running process's kernel stack. */
static struct tss64 g_tss[MAX_CPUS] __attribute__((aligned(16)));

uint64_t tss_kernel_rsp_top(void) {
    /* The BSP's interrupt stack top. Used to seed cpu 0's SYSCALL stack
     * before the first context switch. */
    return (uint64_t)&g_kernel_irq_stack[KERNEL_IRQ_STACK_SIZE];
}

/* Point the *current* CPU's TSS.RSP0 at `rsp0`. Called by the scheduler on
 * every context switch (per-CPU: an IRQ on this core lands on this core's
 * running process's kstack). */
void tss_set_rsp0(uint64_t rsp0) {
    g_tss[smp_current_cpu_idx()].rsp0 = rsp0;
}

/* Install + load this CPU's TSS. The GDT has a single TSS descriptor slot;
 * because LTR caches the TSS base in the hidden TR register and APs boot
 * serially, we can rewrite that one descriptor with each CPU's TSS base
 * just before its LTR -- after LTR the CPU no longer consults the GDT
 * entry, so the next CPU reusing the slot is harmless. */
static void tss_load_cpu(uint32_t cpu, uint64_t rsp0) {
    struct tss64 *t = &g_tss[cpu];
    memset(t, 0, sizeof(*t));
    t->rsp0       = rsp0;
    t->iomap_base = sizeof(*t);     /* "no I/O permission bitmap" */
    gdt_install_tss((uint64_t)t, (uint32_t)(sizeof(*t) - 1));
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS_SEL));
}

void tss_init(void) {
    /* BSP (cpu 0): RSP0 = the static kernel IRQ stack. */
    tss_load_cpu(0, tss_kernel_rsp_top());
    kprintf("[tss] cpu0 installed: tss=%p rsp0=%p (irq stack %u KiB)\n",
            (void *)&g_tss[0], (void *)g_tss[0].rsp0,
            (unsigned)(KERNEL_IRQ_STACK_SIZE / 1024));
}

/* Per-AP TSS setup, called from ap_entry on the secondary CPU. `rsp0` is a
 * stack the AP can use for an IRQ-from-ring3 before the scheduler hands it a
 * real process (the scheduler overrides RSP0 on the first switch). */
void tss_init_ap(uint32_t cpu_idx, uint64_t rsp0) {
    if (cpu_idx >= MAX_CPUS) return;
    tss_load_cpu(cpu_idx, rsp0);
}
