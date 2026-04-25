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

static struct tss64 g_tss __attribute__((aligned(16)));

uint64_t tss_kernel_rsp_top(void) {
    /* SysV ABI wants RSP 16-byte aligned at function entry; KERNEL_IRQ_
     * STACK_SIZE is a multiple of 16 and the array itself is 16-aligned,
     * so the past-the-end address is naturally aligned. The CPU subtracts
     * before storing on push, so the very first byte we write will land
     * inside the array. */
    return (uint64_t)&g_kernel_irq_stack[KERNEL_IRQ_STACK_SIZE];
}

void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}

void tss_init(void) {
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.rsp0       = tss_kernel_rsp_top();
    g_tss.iomap_base = sizeof(g_tss);   /* "no I/O permission bitmap" */

    /* TSS descriptor "limit" is sizeof(tss) - 1, just like a code/data
     * segment limit. The CPU uses it to bounds-check IST/RSP fetches. */
    gdt_install_tss((uint64_t)&g_tss, (uint32_t)(sizeof(g_tss) - 1));

    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS_SEL));

    kprintf("[tss] installed: tss=%p rsp0=%p (irq stack %u KiB)\n",
            (void *)&g_tss, (void *)g_tss.rsp0,
            (unsigned)(KERNEL_IRQ_STACK_SIZE / 1024));
}
