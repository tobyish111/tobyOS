/* ioapic.h -- I/O APIC (82093AA / ICH IOxAPIC) driver.
 *
 * The IO APIC is the chip that takes hardware interrupt lines (legacy
 * ISA IRQs, PCI INTx pins, MSI doorbell writes for older boards) and
 * delivers them to a Local APIC as a vector message. We map every IO
 * APIC the firmware reports in MADT, then route specific GSIs (Global
 * System Interrupts) via ioapic_route().
 *
 * Boot dependency: must run AFTER acpi_init (we read the IOAPIC + ISO
 * lists from there) and AFTER apic_init_bsp (so vmm + the LAPIC are
 * already up; we share the same VMM_NOCACHE pattern for MMIO).
 *
 * If the firmware reports no IO APIC, ioapic_init() is a no-op and
 * ioapic_active() returns false -- callers should keep the legacy PIC
 * path live in that case.
 *
 * GSI vs ISA IRQ: a GSI is a global, OS-visible number for an
 * interrupt source. The legacy IRQ pins (0..15 on the i8259 pair) map
 * to GSIs in a 1:1 default unless the firmware says otherwise via an
 * ISO entry. Use ioapic_resolve_isa() to translate a legacy ISA IRQ
 * into the GSI you actually need to program.
 */

#ifndef TOBYOS_IOAPIC_H
#define TOBYOS_IOAPIC_H

#include <tobyos/types.h>

/* Trigger / polarity flags for ioapic_route() -- the names match the
 * IOAPIC redirection-entry bits we set, NOT the MPS INTI flags. */
#define IOAPIC_TRIGGER_EDGE    false
#define IOAPIC_TRIGGER_LEVEL   true
#define IOAPIC_POL_ACTIVE_HIGH false
#define IOAPIC_POL_ACTIVE_LOW  true

/* Resolved routing for a legacy ISA IRQ after applying any matching
 * MADT Interrupt Source Override. `gsi` is what to program; `level`
 * and `active_low` come from the ISO flags (or default to edge/high
 * for ISA when no ISO matched). */
struct ioapic_isa_route {
    uint32_t gsi;
    bool     level;
    bool     active_low;
};

/* Walk the ACPI IOAPIC list, map each MMIO region into kernel virt,
 * read IOAPICVER to learn the redirection-entry count for each chip,
 * leave every redir entry MASKED. Idempotent: safe to call once.
 *
 * Returns true if at least one IO APIC was successfully brought up.
 * Returns false (and is a no-op for the rest of the kernel) on a board
 * where the firmware reports no IO APIC -- the caller should leave
 * the legacy PIC live in that case. */
bool ioapic_init(void);

/* Did ioapic_init() bring up at least one IO APIC? */
bool ioapic_active(void);

/* Resolve an ISA IRQ (0..15) to its actual GSI + trigger/polarity by
 * checking the ACPI ISO list. The default for ISA IRQs (when no ISO
 * matches) is the identity mapping (gsi == irq), edge-triggered,
 * active-high. */
struct ioapic_isa_route ioapic_resolve_isa(uint8_t isa_irq);

/* Program a GSI -> (vector, lapic_id) routing in physical destination
 * mode, fixed delivery. Caller picks edge/level + polarity (use
 * ioapic_resolve_isa() to pull those from ACPI for legacy IRQs).
 *
 * The redir entry is left UNMASKED on success -- call ioapic_mask()
 * if you want it gated until your handler is registered. Returns
 * false if the GSI is out of range (no IO APIC owns it). */
bool ioapic_route(uint32_t gsi, uint8_t vector, uint8_t lapic_id,
                  bool level, bool active_low);

/* Mask / unmask a single redirection entry. masked=true blocks delivery
 * (useful for one-shot disables); masked=false re-enables a previously
 * routed entry. Returns false if the GSI is out of range. */
bool ioapic_mask(uint32_t gsi, bool masked);

#endif /* TOBYOS_IOAPIC_H */
