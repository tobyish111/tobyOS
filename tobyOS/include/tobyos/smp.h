/* smp.h -- minimal SMP orchestrator.
 *
 * smp_start_aps() walks the ACPI CPU list, allocates a per-CPU stack,
 * patches the AP trampoline, and runs INIT-SIPI-SIPI on each AP one at
 * a time. Each AP runs ap_entry() (in src/smp.c), which prints
 * "CPU N online" and bumps an atomic counter. The BSP waits for that
 * counter to reach the expected value before returning.
 *
 * No scheduler. No multitasking. APs end up cli/hlt'ing forever.
 */

#ifndef TOBYOS_SMP_H
#define TOBYOS_SMP_H

#include <tobyos/types.h>
#include <tobyos/percpu.h>

/* Phys address we copy the AP trampoline blob to. Must be page-aligned,
 * <= 0xFF000 so it fits in the 8-bit SIPI vector field, AND in a
 * Limine-reported USABLE region. We pick 0x70000 which sits well clear
 * of Limine's bootloader data (0x1000..0x61000) and the bitmap page
 * (0x61000), in the USABLE window 0x62000..0x9F000. */
#define AP_TRAMPOLINE_PHYS 0x70000ULL

/* Bring up every AP enumerated by ACPI. Quietly does nothing if ACPI
 * found only one CPU. Returns the total number of CPUs that ended up
 * online (BSP + APs). */
uint32_t smp_start_aps(void);

/* Snapshot of per-CPU state for the `cpus` shell command. */
const struct percpu *smp_cpu(uint32_t idx);
uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);

/* Return the cpu_idx of the CPU currently executing this code. Reads
 * the LAPIC ID and matches it against g_percpu[].apic_id. Cheap
 * (couple of MMIO reads + a tiny linear scan over <= MAX_CPUS slots).
 * Safe to call from any context once smp_init_bsp has run; before
 * that it returns 0 (always the BSP). */
uint32_t smp_current_cpu_idx(void);

/* Mutable per-CPU slot for THIS CPU. Used by the scheduler + LAPIC
 * timer ISR to enqueue/dequeue without going through cpu_idx
 * arithmetic at every call site. NEVER returns NULL once the percpu
 * table is built; before that it returns the BSP slot. */
struct percpu *smp_this_cpu(void);

/* Mutable view of g_percpu[idx]. Internal-ish: the scheduler uses
 * this to push/pop the BSP queue from any CPU. NULL if idx is OOR. */
struct percpu *smp_cpu_mut(uint32_t idx);

#endif /* TOBYOS_SMP_H */
