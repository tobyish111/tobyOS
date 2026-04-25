/* irq.h -- thin facade over (legacy PIC | IO APIC) for legacy ISA IRQs.
 *
 * Drivers (pit, kbd, mouse, anything else on the legacy ISA bus) call
 * irq_install_isa()/irq_eoi_isa() instead of speaking PIC directly.
 * The facade keeps the whole "IO APIC up, route via that unless absent,
 * then EOI to the LAPIC instead of the PIC" decision tree in one place.
 *
 * For non-ISA interrupts (MSI/MSI-X, LAPIC timer, the LAPIC spurious
 * vector) the EOI is always apic_eoi(). Use irq_eoi_apic() (or call
 * apic_eoi() directly) from those handlers.
 *
 * Boot phases:
 *   1. irq_init() runs before any driver registers an ISA IRQ. The
 *      facade starts in "PIC mode" so the early PIT + keyboard set up
 *      via the legacy 8259 path -- exactly what perf_init needs.
 *   2. After SMP brings up the BSP LAPIC, irq_switch_to_ioapic() is
 *      called. The facade masks all 16 PIC IRQs at both IMRs, walks
 *      every previously-installed handler, and re-routes its line
 *      through the IO APIC (applying any matching ACPI ISO override).
 *      Subsequent irq_install_isa() calls (e.g. mouse_init) take the
 *      IO APIC path automatically.
 *
 * If no IO APIC is present in MADT, irq_switch_to_ioapic() is a no-op
 * and we stay in PIC mode for the life of the boot -- preserving the
 * worst-case real-PC fallback path.
 *
 * Vector layout owned by the facade:
 *   0x20..0x2F  -- legacy ISA IRQs 0..15 (same vectors the PIC remap
 *                  chose, so the existing isr_register() calls keep
 *                  working unchanged)
 *   0x40..0x4F  -- LAPIC LVT (timer at 0x40, room for thermal/error/
 *                  perfmon if we ever wire them; apic.h owns these)
 *   0x50..0xEF  -- IRQ allocator pool (irq_alloc_vector(), step 2)
 *                  159 vectors -- MSI/MSI-X handlers land here
 *   0xFF        -- LAPIC spurious vector (apic.h owns this)
 */

#ifndef TOBYOS_IRQ_H
#define TOBYOS_IRQ_H

#include <tobyos/types.h>
#include <tobyos/isr.h>

/* Vector that legacy ISA IRQ `isa_irq` (0..15) lands on. Matches the
 * PIC_IRQ_BASE convention so PIC + IO APIC paths share the same IDT
 * slot for each IRQ. */
#define IRQ_ISA_VECTOR(isa_irq)  ((uint8_t)(0x20u + (isa_irq)))

/* Initialise the facade in PIC mode. Idempotent. Called from kernel
 * boot AFTER pic_init() and BEFORE the first irq_install_isa(). */
void irq_init(void);

/* Promote the facade to IO APIC mode: mask the legacy PIC at both
 * IMRs, then walk every previously-installed ISA IRQ and re-route it
 * through the IO APIC (resolving ISO entries on the way). EOI for
 * every IRQ now goes via apic_eoi().
 *
 * Returns true if the switch happened, false if there's no IO APIC
 * (caller stays in PIC mode). Idempotent: safe to call twice. */
bool irq_switch_to_ioapic(void);

/* Are we currently delivering legacy IRQs via the IO APIC (true) or
 * the legacy PIC (false)? */
bool irq_using_ioapic(void);

/* Install a C handler for a legacy ISA IRQ + unmask the line at the
 * appropriate controller. The driver's handler must call
 * irq_eoi_isa(isa_irq) at the end. */
void irq_install_isa(uint8_t isa_irq, isr_handler_fn handler);

/* End-of-interrupt for a handler that was wired by irq_install_isa
 * (or for any legacy-IRQ handler that wants to be portable across the
 * PIC and IO APIC paths). */
void irq_eoi_isa(uint8_t isa_irq);

/* End-of-interrupt for a non-ISA APIC-delivered interrupt (LAPIC
 * timer, MSI/MSI-X, IPIs). Just a thin wrapper on apic_eoi() but it
 * keeps every IRQ EOI inside the facade. */
void irq_eoi_apic(void);

/* ============================================================== */
/* Dynamic IDT vector allocator (for MSI/MSI-X handlers)           */
/* ============================================================== */

/* Driver-style IRQ handler. Receives the opaque `ctx` registered at
 * allocation time. Implementations should NOT call apic_eoi() --
 * the dispatcher trampoline handles that. */
typedef void (*irq_handler_fn)(void *ctx);

/* Reserve a fresh IDT vector from the dynamic pool [IRQ_DYN_FIRST..
 * IRQ_DYN_LAST] and wire it to the given (handler, ctx). Returns the
 * allocated vector, or 0 if the pool is exhausted (0 happens to be
 * the divide-by-zero exception, so any caller can treat 0 as failure
 * unambiguously). The trampoline calls handler(ctx) and then
 * apic_eoi() for the caller. */
uint8_t irq_alloc_vector(irq_handler_fn handler, void *ctx);

/* Vector pool bounds (inclusive). Exposed so MSI-X probe code that
 * needs a contiguous range can sanity-check that vector_base + count
 * fits before asking the device to deliver. */
#define IRQ_DYN_FIRST  0x50u
#define IRQ_DYN_LAST   0xEEu
/* Convenience: irq_alloc_vector_range allocates `count` consecutive
 * vectors from the pool and registers ALL of them on (handler, ctx).
 * The driver ISR can disambiguate via the regs->vector value (passed
 * to handler indirectly: ctx is the same for every vector). For
 * per-vector ctx (e.g. one per virtio-net queue) call irq_alloc_vector
 * once per queue. Returns the BASE vector or 0 on failure. */
uint8_t irq_alloc_vector_range(uint32_t count, irq_handler_fn handler,
                               void *ctx);

#endif /* TOBYOS_IRQ_H */
