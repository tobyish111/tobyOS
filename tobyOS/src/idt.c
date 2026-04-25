/* idt.c -- build and load the 64-bit IDT.
 *
 * Long-mode gate descriptor (16 bytes):
 *
 *   bytes  0..1  : offset[0..15]
 *   bytes  2..3  : selector (kernel CS)
 *   byte   4     : ist[2..0] (zero = use the current kernel stack)
 *   byte   5     : type_attr -- P(1) DPL(2) 0(1) type(4)
 *                  0x8E = present, DPL 0, interrupt gate (auto-CLI)
 *                  0x8F = present, DPL 0, trap gate      (no auto-CLI)
 *                  0xEE = present, DPL 3, interrupt gate (entry from user)
 *   bytes  6..7  : offset[16..31]
 *   bytes  8..11 : offset[32..63]
 *   bytes 12..15 : reserved (zero)
 */

#include <tobyos/idt.h>
#include <tobyos/isr.h>
#include <tobyos/gdt.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_gate g_idt[256] __attribute__((aligned(16)));
static struct idtr     g_idtr;

const void *idt_pseudo_descriptor(void) { return &g_idtr; }

/* Defined in src/isr_stubs.S: array of stub function pointers, one per
 * IDT vector. We install ALL 256 -- vectors 0..31 are CPU exceptions,
 * 32..47 were the legacy PIC IRQs (now repurposed for IO APIC GSIs at
 * the same vectors), 0x40 is APIC_TIMER, 0x40..0xEF are the IRQ
 * allocator's free pool (M22 step 2), 0xFF is the spurious vector.
 * Drivers register C handlers via isr_register(); empty slots fall
 * through to the default in isr.c which silently EOIs (vectors >=32)
 * or dumps + panics (vectors <32). */
#define IDT_STUB_COUNT 256
extern void *isr_stub_table[IDT_STUB_COUNT];

static void set_gate(int vec, void *handler, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    g_idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    g_idt[vec].selector    = GDT_KERNEL_CS;
    g_idt[vec].ist         = 0;
    g_idt[vec].type_attr   = type_attr;
    g_idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vec].reserved    = 0;
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));

    /* All 256 vectors get a DPL=0 interrupt gate. The C dispatcher
     * (isr.c) silently no-ops for unclaimed vectors >= 32, so an
     * unsolicited IO APIC / MSI delivery never panics us. Vectors
     * 0..31 still hit the default exception path on no-handler. */
    for (int i = 0; i < IDT_STUB_COUNT; i++) {
        set_gate(i, isr_stub_table[i], 0x8E);
    }

    g_idtr.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idtr.base  = (uint64_t)&g_idt[0];

    __asm__ volatile ("lidt %0" : : "m"(g_idtr) : "memory");

    kprintf("[idt] loaded: base=%p limit=%u (%d gates wired)\n",
            (void *)g_idtr.base, (unsigned)g_idtr.limit, IDT_STUB_COUNT);
}
