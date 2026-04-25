/* apic.c -- xAPIC (MMIO) Local APIC driver.
 *
 * MMIO register offsets we use (Intel SDM vol 3, table 11-1):
 *
 *   0x020  ID                                (R/W on some CPUs, read here)
 *   0x030  Version
 *   0x080  TPR  -- Task Priority Register
 *   0x0B0  EOI                               (write any value)
 *   0x0F0  SVR  -- Spurious-Interrupt Vector Register
 *                  bit 8       = LAPIC software enable
 *                  bits[7:0]   = spurious vector
 *   0x300  ICR_LOW
 *   0x310  ICR_HIGH                          (target APIC ID in [31:24])
 *   0x320  LVT Timer                         bit 16 = mask, bit 17 = mode
 *   0x380  Initial Count
 *   0x390  Current Count
 *   0x3E0  Divide Configuration Register     0x0B = divide-by-1
 *
 * ICR_LOW field cheat-sheet (the bits we actually set):
 *
 *   [10:8]  Delivery mode:  0=fixed, 4=NMI, 5=INIT, 6=startup
 *   [11]    Destination mode (0 = physical APIC ID)
 *   [12]    Delivery status (RO; 1 = pending)
 *   [14]    Level (1 = assert)
 *   [15]    Trigger (0 = edge, 1 = level)
 *   [19:18] Destination shorthand (0=use ICR_HI)
 *
 * IA32_APIC_BASE MSR (0x1B):
 *   bit 11 (EN)  -- LAPIC hardware enable (we set if not already)
 *   bit 8  (BSP) -- read-only "is this the BSP?"
 */

#include <tobyos/apic.h>
#include <tobyos/acpi.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/isr.h>
#include <tobyos/sched.h>

/* Single virtual page mapped to LAPIC MMIO. Same kernel virt for every
 * CPU; the page resolves (in HW) to the per-CPU LAPIC. */
#define LAPIC_KVIRT 0xffffd20000000000ULL

#define LAPIC_REG_ID            0x020
#define LAPIC_REG_VERSION       0x030
#define LAPIC_REG_TPR           0x080
#define LAPIC_REG_EOI           0x0B0
#define LAPIC_REG_SVR           0x0F0
#define LAPIC_REG_ICR_LOW       0x300
#define LAPIC_REG_ICR_HIGH      0x310
#define LAPIC_REG_LVT_TIMER     0x320
#define LAPIC_REG_TIMER_INIT    0x380
#define LAPIC_REG_TIMER_CURR    0x390
#define LAPIC_REG_TIMER_DIV     0x3E0

#define IA32_APIC_BASE_MSR      0x1Bu
#define IA32_APIC_BASE_EN       (1ULL << 11)

#define ICR_DELIVERY_INIT       (5u << 8)
#define ICR_DELIVERY_STARTUP    (6u << 8)
#define ICR_LEVEL_ASSERT        (1u << 14)
#define ICR_TRIGGER_EDGE        (0u << 15)
#define ICR_DEST_PHYSICAL       (0u << 11)
#define ICR_DELIV_STATUS        (1u << 12)

#define SVR_SOFT_ENABLE         (1u << 8)

#define LVT_MASKED              (1u << 16)
#define LVT_TIMER_PERIODIC      (1u << 17)

static volatile uint8_t *g_lapic;     /* virt pointer to MMIO page */
static bool              g_ready;

bool apic_is_ready(void) { return g_ready; }

/* MMIO accessors -- LAPIC registers are dword-sized at dword offsets. */
static inline uint32_t lapic_read(uint32_t off) {
    return *(volatile uint32_t *)(g_lapic + off);
}
static inline void lapic_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_lapic + off) = val;
}

uint32_t apic_read_id(void) {
    /* APIC id lives in bits [31:24] of the ID register. */
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void apic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

void apic_send_ipi(uint8_t target_apic_id, uint32_t icr_low) {
    /* Spin until any prior delivery completes. */
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIV_STATUS) {
        __asm__ volatile ("pause" ::: "memory");
    }
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24);
    /* Writing the LOW register triggers the actual send. */
    lapic_write(LAPIC_REG_ICR_LOW, icr_low);
    /* Optional: spin until the send has been accepted by the bus. */
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIV_STATUS) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

void apic_send_init(uint8_t target_apic_id) {
    apic_send_ipi(target_apic_id,
                  ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT |
                  ICR_TRIGGER_EDGE  | ICR_DEST_PHYSICAL);
}

void apic_send_sipi(uint8_t target_apic_id, uint8_t vector) {
    apic_send_ipi(target_apic_id,
                  ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT |
                  ICR_TRIGGER_EDGE     | ICR_DEST_PHYSICAL | vector);
}

void apic_init_local(void) {
    /* Hardware-enable via IA32_APIC_BASE if the firmware left it off. */
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    if ((base & IA32_APIC_BASE_EN) == 0) {
        wrmsr(IA32_APIC_BASE_MSR, base | IA32_APIC_BASE_EN);
    }

    /* Accept all priorities. */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Software-enable + spurious vector. From this point on the LAPIC
     * will deliver any pending IPIs. */
    lapic_write(LAPIC_REG_SVR, SVR_SOFT_ENABLE | APIC_SPURIOUS_VECTOR);

    /* Configure the timer in a known-good state but keep it MASKED.
     * That way it won't fire (no scheduler yet) but a future milestone
     * can simply unmask + write TIMER_INIT to start ticking. */
    lapic_write(LAPIC_REG_TIMER_DIV,   0x0B);  /* divide by 1 */
    lapic_write(LAPIC_REG_LVT_TIMER,   LVT_MASKED | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT,  0);
}

/* Cached LAPIC timer rate from apic_timer_calibrate_global. Same on
 * every CPU because the LAPIC timer ticks off the bus clock, which
 * is shared across all logical CPUs in QEMU and on real x86_64 boxes
 * built around a single core complex. */
static uint32_t g_lapic_ticks_per_sec;

/* One-shot calibration: count LAPIC timer ticks during a 10 ms PIT
 * sleep. Purely informational -- we log the rate so the next milestone
 * has a starting point for "set timer to fire every N ms". */
static void apic_calibrate_timer(void) {
    /* Briefly unmask the counter (still no IRQ -- we don't even hit the
     * fire condition because we read the count before it expires). */
    lapic_write(LAPIC_REG_TIMER_DIV,  0x0B);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
    pit_sleep_ms(10);
    uint32_t curr = lapic_read(LAPIC_REG_TIMER_CURR);
    uint32_t elapsed = 0xFFFFFFFFu - curr;
    /* Re-mask, leave init=0 so it stops counting. */
    lapic_write(LAPIC_REG_LVT_TIMER,  LVT_MASKED | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    /* Stash for use by apic_timer_periodic_init. Multiply by 100 to
     * get per-second from per-10ms; saturate at UINT32_MAX-ish in
     * case calibration ever picks up a wildly fast clock. */
    uint64_t per_sec = (uint64_t)elapsed * 100ull;
    g_lapic_ticks_per_sec = per_sec > 0xFFFFFFFFull
        ? 0xFFFFFFFFu
        : (uint32_t)per_sec;

    kprintf("[apic] timer calibrated: ~%u ticks/10ms (~%u kHz, ~%u Hz/sec)\n",
            (unsigned)elapsed, (unsigned)(elapsed / 10u),
            (unsigned)g_lapic_ticks_per_sec);
}

void apic_timer_calibrate_global(void) {
    if (!g_ready) return;
    if (g_lapic_ticks_per_sec) return;          /* already calibrated */
    apic_calibrate_timer();
}

/* IDT-side ISR for vector APIC_TIMER_VECTOR. The same handler runs
 * on every CPU; sched_tick consults smp_this_cpu() internally so we
 * don't have to thread cpu_idx through the dispatch path. EOI is to
 * the LAPIC (apic_eoi) -- the LAPIC timer is delivered by the local
 * APIC, never via the PIC or IO APIC. */
static void apic_timer_isr(struct regs *r) {
    (void)r;
    sched_tick();
    apic_eoi();
}

bool apic_timer_periodic_init(uint32_t hz) {
    if (!g_ready)             return false;
    if (hz == 0)              return false;
    if (g_lapic_ticks_per_sec == 0) {
        /* Late callers can ask us to calibrate on the fly. Only the
         * BSP should ever hit this branch, since we calibrate during
         * apic_init_bsp; APs run after that and inherit the cached
         * rate. */
        apic_timer_calibrate_global();
        if (g_lapic_ticks_per_sec == 0) return false;
    }
    uint32_t reload = g_lapic_ticks_per_sec / hz;
    if (reload < 16) reload = 16;               /* don't program a runaway */

    /* Periodic mode + our chosen vector + UNMASKED. The divider is
     * already 0x0B (divide-by-1) from apic_init_local; reset it just
     * to be defensive in case some future code touched it. */
    lapic_write(LAPIC_REG_TIMER_DIV,   0x0B);
    lapic_write(LAPIC_REG_LVT_TIMER,   LVT_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT,  reload);
    return true;
}

bool apic_init_bsp(void) {
    const struct acpi_info *info = acpi_get();
    uint64_t lapic_phys = info && info->ok ? info->lapic_phys : 0xFEE00000ULL;

    /* Some firmware leaves bit-0 / low bits set in the lapic_phys field
     * by accident. The MMIO must be page-aligned. */
    lapic_phys &= ~((uint64_t)PAGE_SIZE - 1);

    if (!vmm_map(LAPIC_KVIRT, lapic_phys, PAGE_SIZE,
                 VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_NOCACHE)) {
        kprintf("[apic] vmm_map for LAPIC MMIO at phys %p failed\n",
                (void *)lapic_phys);
        return false;
    }
    g_lapic = (volatile uint8_t *)LAPIC_KVIRT;

    apic_init_local();   /* enable on the BSP */
    g_ready = true;

    uint32_t id  = apic_read_id();
    uint32_t ver = lapic_read(LAPIC_REG_VERSION) & 0xFF;
    kprintf("[apic] BSP local APIC: phys=%p virt=%p id=%u ver=0x%x\n",
            (void *)lapic_phys, (void *)LAPIC_KVIRT,
            (unsigned)id, (unsigned)ver);
    kprintf("[apic] SVR=0x%x  spurious_vec=0x%x  timer_vec=0x%x (masked)\n",
            (unsigned)lapic_read(LAPIC_REG_SVR),
            APIC_SPURIOUS_VECTOR, APIC_TIMER_VECTOR);

    apic_calibrate_timer();                     /* fills g_lapic_ticks_per_sec */

    /* Register the timer ISR on the IDT. The timer is still MASKED
     * after calibration, so this is harmless until somebody calls
     * apic_timer_periodic_init() to unmask + program a reload value.
     * The IDT is shared across all CPUs (every AP runs lidt with the
     * same kernel IDTR via the trampoline) so registering here once
     * covers BSP + every AP. */
    isr_register(APIC_TIMER_VECTOR, apic_timer_isr);
    return true;
}
