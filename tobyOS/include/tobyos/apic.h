/* apic.h -- Local APIC (xAPIC, MMIO at lapic_phys from MADT).
 *
 * apic_init_bsp() must run after vmm_init + acpi_init: it maps the
 * LAPIC MMIO into kernel-virt (NX + write-through-disabled) and
 * software-enables the BSP's local APIC. After this, IPIs become
 * available -- smp.c uses them to wake APs.
 *
 * apic_init_local() runs on every CPU (BSP and each AP) once that
 * CPU is in long mode. It just enables this CPU's LAPIC; the MMIO
 * mapping is shared (LAPIC MMIO is per-CPU at the same physical
 * address, the CPU sees its own LAPIC at that page).
 *
 * The timer is configured (vector 0x40, masked, divide-by-1) so we
 * could fire it later, but we leave it MASKED in this milestone --
 * scheduler integration is a future step. We do calibrate it once
 * against the PIT and log the result, just to prove it counts.
 */

#ifndef TOBYOS_APIC_H
#define TOBYOS_APIC_H

#include <tobyos/types.h>

/* Vector reserved for the LAPIC spurious-interrupt handler. Convention
 * is the highest free vector (0xFF). */
#define APIC_SPURIOUS_VECTOR  0xFFu
/* Vector for LAPIC timer (set up but masked). */
#define APIC_TIMER_VECTOR     0x40u

/* Map LAPIC MMIO + enable BSP's local APIC. Returns false on error
 * (e.g. ACPI didn't give us a sane lapic_phys). */
bool apic_init_bsp(void);

/* Software-enable the local APIC on the *current* CPU. Idempotent.
 * Used both from apic_init_bsp (after the mapping is established) and
 * by ap_entry() on every AP. */
void apic_init_local(void);

/* Read this CPU's APIC ID (from the local APIC's ID register). */
uint32_t apic_read_id(void);

/* Send an IPI: caller-controlled ICR_LOW value, target apic_id. We
 * spin on DELIV_STATUS before AND after to be safe. */
void apic_send_ipi(uint8_t target_apic_id, uint32_t icr_low);

/* INIT IPI helper: assert INIT to a specific APIC. */
void apic_send_init(uint8_t target_apic_id);

/* SIPI helper: send a Startup IPI with the given trampoline vector
 * (vector_value = trampoline_phys >> 12, must fit in 8 bits). */
void apic_send_sipi(uint8_t target_apic_id, uint8_t vector);

/* End-of-interrupt for the current LVT-driven interrupt (timer,
 * spurious, etc.). NOT used for legacy PIC IRQs -- those go through
 * pic_send_eoi as before. */
void apic_eoi(void);

/* Did apic_init_bsp succeed? */
bool apic_is_ready(void);

/* ---- Milestone 22 step 5: per-CPU periodic LAPIC timer -------- */
/*
 * Configure THIS CPU's LAPIC timer to fire periodically at `hz` Hz on
 * vector APIC_TIMER_VECTOR. Returns true if the timer was started.
 *
 * apic_timer_calibrate_global() must run first (BSP only). It samples
 * the LAPIC timer rate against the PIT and stashes the result so
 * subsequent apic_timer_periodic_init() calls (BSP and APs) can pick
 * a reload value without re-doing the calibration. The shared rate
 * is fine because every LAPIC ticks at the same bus clock.
 *
 * The ISR for APIC_TIMER_VECTOR must call irq_eoi_apic() before
 * returning (sched_tick + apic_eoi() is the conventional combo). */
void apic_timer_calibrate_global(void);
bool apic_timer_periodic_init(uint32_t hz);

#endif /* TOBYOS_APIC_H */
