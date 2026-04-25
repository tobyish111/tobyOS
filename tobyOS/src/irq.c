/* irq.c -- legacy-ISA IRQ facade (PIC vs IO APIC).
 *
 * Goal: let pit/keyboard/mouse/etc. ignore which interrupt controller
 * the firmware actually wired up. They register their handlers with
 * irq_install_isa() and EOI with irq_eoi_isa(), and we route to the
 * right chip based on whether ioapic_init() succeeded.
 *
 * Two-phase boot:
 *   1. Early boot: facade is in PIC mode. PIT + keyboard register
 *      themselves so perf_init can sample wall time before the LAPIC
 *      / IO APIC come up.
 *   2. Post-SMP: irq_switch_to_ioapic() flips us over -- we mask both
 *      PIC IMRs, then re-route every ISA IRQ that already has a
 *      handler through the IO APIC (applying any matching ACPI ISO).
 *      mouse_init() runs after this, so it takes the IO APIC path
 *      directly without needing a re-route.
 *
 * If the firmware reports no IO APIC, irq_switch_to_ioapic() is a
 * no-op and we stay in PIC mode for the life of the boot.
 */

#include <tobyos/irq.h>
#include <tobyos/ioapic.h>
#include <tobyos/apic.h>
#include <tobyos/pic.h>
#include <tobyos/printk.h>

static bool     g_use_ioapic;        /* mode flag, flipped by irq_switch_to_ioapic */
static uint16_t g_isa_installed;     /* bit i = irq_install_isa(i, _) was called */

/* Dynamic vector allocator state (MSI/MSI-X handlers). */
struct dyn_slot {
    irq_handler_fn handler;
    void          *ctx;
};
static struct dyn_slot g_dyn[256];
static uint8_t         g_next_dyn_vec = IRQ_DYN_FIRST;

/* Single shared trampoline registered with isr_register() for every
 * dynamically-allocated vector. We dispatch to the per-vector slot
 * and EOI to the LAPIC -- MSI/MSI-X always uses the LAPIC. */
static void dyn_irq_trampoline(struct regs *r) {
    if (r->vector < 256u) {
        struct dyn_slot *s = &g_dyn[r->vector];
        if (s->handler) s->handler(s->ctx);
    }
    apic_eoi();
}

void irq_init(void) {
    g_use_ioapic    = false;
    g_isa_installed = 0;

    g_next_dyn_vec = IRQ_DYN_FIRST;
    for (int i = 0; i < 256; i++) {
        g_dyn[i].handler = 0;
        g_dyn[i].ctx     = 0;
    }
}

bool irq_using_ioapic(void) { return g_use_ioapic; }

void irq_install_isa(uint8_t isa_irq, isr_handler_fn handler) {
    if (isa_irq >= 16) return;
    isr_register(IRQ_ISA_VECTOR(isa_irq), handler);
    g_isa_installed |= (uint16_t)(1u << isa_irq);

    if (g_use_ioapic) {
        struct ioapic_isa_route r = ioapic_resolve_isa(isa_irq);
        bool ok = ioapic_route(r.gsi, IRQ_ISA_VECTOR(isa_irq),
                               (uint8_t)apic_read_id(),
                               r.level, r.active_low);
        if (!ok) {
            kprintf("[irq] WARN: ioapic_route failed for ISA IRQ %u "
                    "(GSI %u) -- IRQ will be silent\n",
                    (unsigned)isa_irq, (unsigned)r.gsi);
            return;
        }
        if (r.gsi != (uint32_t)isa_irq || r.level || r.active_low) {
            kprintf("[irq] ISA IRQ %u -> GSI %u vec=0x%02x %s/%s\n",
                    (unsigned)isa_irq, (unsigned)r.gsi,
                    (unsigned)IRQ_ISA_VECTOR(isa_irq),
                    r.level      ? "level" : "edge",
                    r.active_low ? "alow"  : "ahigh");
        }
    } else {
        pic_unmask(isa_irq);
    }
}

bool irq_switch_to_ioapic(void) {
    if (g_use_ioapic) return true;             /* already switched */
    if (!ioapic_active()) return false;        /* no IO APIC -- stay on PIC */

    /* Step 1: park the legacy 8259 pair so it can't deliver any line
     * during the brief window between mask and re-route. */
    pic_mask_all();
    g_use_ioapic = true;

    /* Step 2: walk every ISA IRQ we previously wired through the PIC
     * and re-route it through the IO APIC at the SAME IDT vector
     * (the handler is unchanged). */
    uint8_t lapic_id = (uint8_t)apic_read_id();
    uint32_t routed = 0;
    for (uint8_t i = 0; i < 16; i++) {
        if (!(g_isa_installed & (uint16_t)(1u << i))) continue;
        struct ioapic_isa_route r = ioapic_resolve_isa(i);
        if (!ioapic_route(r.gsi, IRQ_ISA_VECTOR(i), lapic_id,
                          r.level, r.active_low)) {
            kprintf("[irq] WARN: failed to reroute ISA IRQ %u (GSI %u)\n",
                    (unsigned)i, (unsigned)r.gsi);
            continue;
        }
        routed++;
        if (r.gsi != (uint32_t)i || r.level || r.active_low) {
            kprintf("[irq]   re-route ISA IRQ %u -> GSI %u vec=0x%02x "
                    "%s/%s\n",
                    (unsigned)i, (unsigned)r.gsi,
                    (unsigned)IRQ_ISA_VECTOR(i),
                    r.level      ? "level" : "edge",
                    r.active_low ? "alow"  : "ahigh");
        } else {
            kprintf("[irq]   re-route ISA IRQ %u (identity, edge/ahigh) -> "
                    "vec=0x%02x\n",
                    (unsigned)i, (unsigned)IRQ_ISA_VECTOR(i));
        }
    }
    kprintf("[irq] facade: switched to IO APIC mode (%u IRQs re-routed)\n",
            (unsigned)routed);
    return true;
}

void irq_eoi_isa(uint8_t isa_irq) {
    if (g_use_ioapic) {
        apic_eoi();
    } else {
        pic_send_eoi(isa_irq);
    }
}

void irq_eoi_apic(void) {
    apic_eoi();
}

/* ---------- dynamic vector allocator ---------- */

uint8_t irq_alloc_vector(irq_handler_fn handler, void *ctx) {
    if (!handler) return 0;
    if (g_next_dyn_vec > IRQ_DYN_LAST) {
        kprintf("[irq] vector pool exhausted (next=0x%02x > 0x%02x)\n",
                (unsigned)g_next_dyn_vec, (unsigned)IRQ_DYN_LAST);
        return 0;
    }
    uint8_t v = g_next_dyn_vec++;
    g_dyn[v].handler = handler;
    g_dyn[v].ctx     = ctx;
    isr_register(v, dyn_irq_trampoline);
    return v;
}

uint8_t irq_alloc_vector_range(uint32_t count, irq_handler_fn handler,
                               void *ctx) {
    if (!handler || count == 0) return 0;
    if ((uint32_t)g_next_dyn_vec + count - 1u > (uint32_t)IRQ_DYN_LAST) {
        kprintf("[irq] vector pool exhausted (need %u from 0x%02x, "
                "ceiling 0x%02x)\n",
                (unsigned)count, (unsigned)g_next_dyn_vec,
                (unsigned)IRQ_DYN_LAST);
        return 0;
    }
    uint8_t base = g_next_dyn_vec;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t v = (uint8_t)(base + i);
        g_dyn[v].handler = handler;
        g_dyn[v].ctx     = ctx;
        isr_register(v, dyn_irq_trampoline);
    }
    g_next_dyn_vec = (uint8_t)(base + count);
    return base;
}
