/* acpi_sleep.c -- Milestone 8 ACPI power management.
 *
 * ACPI sleep states (S3/S4/S5), CPU frequency scaling via MSR,
 * battery monitoring, backlight control, and idle/lid policy.
 */

#include <tobyos/power.h>
#include <tobyos/cputelem.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>

/* ACPI PM registers (found from FADT) */
static struct {
    uint16_t pm1a_ctrl;
    uint16_t pm1b_ctrl;
    uint16_t slp_typa_s3;
    uint16_t slp_typb_s3;
    uint16_t slp_typa_s5;
    uint16_t slp_typb_s5;
    bool     fadt_found;
    enum power_plan current_plan;
    int      backlight_pct;
    int      idle_dim_sec;
    int      idle_off_sec;
    int      idle_sleep_sec;
    uint64_t last_activity_ms;
    bool     lid_closed;
    bool     ready;
} g_power;

/* Port I/O */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* MSR access */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

#define MSR_IA32_PERF_STATUS 0x198
#define MSR_IA32_PERF_CTL    0x199

#define ACPI_SLP_EN  (1 << 13)

/* FADT structure offsets we care about */
struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
} __attribute__((packed));

static void find_fadt(void) {
    /* Scan EBDA and BIOS ROM area for RSDP */
    /* In a VM environment (QEMU), ACPI tables are typically provided
     * via fw_cfg or mapped at known addresses. For now we use
     * reasonable defaults for QEMU's PIIX4 PM. */
    g_power.pm1a_ctrl = 0x604; /* QEMU PIIX4 PM1a control */
    g_power.pm1b_ctrl = 0;
    g_power.slp_typa_s3 = (1 << 10); /* SLP_TYP for S3 on PIIX4 */
    g_power.slp_typb_s3 = 0;
    g_power.slp_typa_s5 = (5 << 10); /* SLP_TYP for S5 on PIIX4 */
    g_power.slp_typb_s5 = 0;
    g_power.fadt_found = true;
}

void power_init(void) {
    memset(&g_power, 0, sizeof(g_power));
    find_fadt();
    g_power.current_plan = PLAN_BALANCED;
    g_power.backlight_pct = 100;
    g_power.idle_dim_sec = 300;
    g_power.idle_off_sec = 600;
    g_power.idle_sleep_sec = 900;
    g_power.last_activity_ms = pit_ticks();
    g_power.ready = true;

    cpufreq_init();
    kprintf("[power] init: PM1a=0x%x plan=balanced\n", g_power.pm1a_ctrl);
}

int power_suspend(void) {
    if (!g_power.fadt_found) return -1;

    kprintf("[power] suspending to S3 (sleep)...\n");

    /* Save critical state would go here in a real implementation */

    uint16_t val = inw(g_power.pm1a_ctrl);
    val &= ~(7 << 10); /* clear SLP_TYP */
    val |= g_power.slp_typa_s3 | ACPI_SLP_EN;
    outw(g_power.pm1a_ctrl, val);

    if (g_power.pm1b_ctrl) {
        val = inw(g_power.pm1b_ctrl);
        val &= ~(7 << 10);
        val |= g_power.slp_typb_s3 | ACPI_SLP_EN;
        outw(g_power.pm1b_ctrl, val);
    }

    /* If we return here, resume happened */
    kprintf("[power] resumed from S3\n");
    return 0;
}

int power_hibernate(void) {
    if (!g_power.fadt_found) return -1;

    kprintf("[power] hibernating to S4...\n");
    /* S4 requires saving all memory to disk first.
     * For now, just enter S5 as a simplified hibernate. */
    power_shutdown();
    return 0;
}

void power_shutdown(void) {
    kprintf("[power] shutdown (S5)...\n");

    if (g_power.fadt_found) {
        uint16_t val = inw(g_power.pm1a_ctrl);
        val &= ~(7 << 10);
        val |= g_power.slp_typa_s5 | ACPI_SLP_EN;
        outw(g_power.pm1a_ctrl, val);

        if (g_power.pm1b_ctrl) {
            val = inw(g_power.pm1b_ctrl);
            val &= ~(7 << 10);
            val |= g_power.slp_typb_s5 | ACPI_SLP_EN;
            outw(g_power.pm1b_ctrl, val);
        }
    }

    /* Fallback: triple-fault */
    __asm__ volatile("cli; hlt");
    for (;;) __asm__ volatile("hlt");
}

void power_reboot(void) {
    kprintf("[power] rebooting...\n");
    /* Pulse the keyboard controller reset line */
    outb(0x64, 0xFE);
    /* Fallback: triple-fault */
    __asm__ volatile("cli");
    uint64_t *null_idt = 0;
    __asm__ volatile("lidt %0" : : "m"(*null_idt));
    __asm__ volatile("int3");
    for (;;) __asm__ volatile("hlt");
}

/* CPU frequency scaling.
 *
 * 2026-08-24 (slice 4): these three had NO callers, and every one of them
 * touched a P-state MSR with no gate at all. Dead code is exactly where
 * that survives -- nothing exercised it, so nothing exposed it. The swap
 * arc already paid for this lesson once: WIRING A DEAD SUBSYSTEM ARMS THE
 * BUGS IN EVERYTHING IT CALLS, so they are fixed here BEFORE anything
 * wires them, not after.
 *
 * What was wrong:
 *   - rdmsr/wrmsr on IA32_PERF_CTL and IA32_PERF_STATUS with no check
 *     that the CPU implements them. There is no exception-fixup table in
 *     this kernel, so on a part without SpeedStep that is a #GP in ring 0,
 *     and the wrmsr is the worse half.
 *   - the P-state targets were INVENTED: 0x1D00 was commented "typical
 *     max ratio for QEMU", 0x0600 "typical min ratio". On the EliteDesk's
 *     i5-4590 the real base ratio is 33 (0x21), so "performance" would
 *     have requested 2.9 GHz on a 3.3 GHz part and "saver" 600 MHz on a
 *     part whose efficiency floor is 800 MHz.
 *
 * Both are now answered from MSR_PLATFORM_INFO via cpufreq_msr_*, which
 * gates itself on CPUID and sanity-checks what it reads. When that says
 * the machine has no readable P-state information, these decline rather
 * than guess. */
void cpufreq_init(void) {
    const struct cpufreq_msr_info *fi = cpufreq_msr_get();
    if (fi->present) {
        kprintf("[cpufreq] P-state control available: base %u kHz, "
                "min %u kHz\n", fi->base_khz, fi->min_khz);
    } else {
        kprintf("[cpufreq] P-state control unavailable: %s\n",
                fi->why ? fi->why : "unknown");
    }
}

void cpufreq_set_governor(enum power_plan plan) {
    const struct cpufreq_msr_info *fi = cpufreq_msr_get();
    if (!fi->present || fi->bus_khz == 0) {
        kprintf("[cpufreq] refusing to set a P-state: %s\n",
                fi->why ? fi->why : "no P-state information");
        return;
    }
    g_power.current_plan = plan;

    /* Real ratios for THIS part, not a guess about a typical one. */
    uint32_t base_ratio = fi->base_khz / fi->bus_khz;
    uint32_t min_ratio  = fi->min_khz  / fi->bus_khz;
    uint32_t ratio;
    switch (plan) {
    case PLAN_PERFORMANCE: ratio = base_ratio; break;
    case PLAN_SAVER:       ratio = min_ratio;  break;
    case PLAN_BALANCED:
    default:               ratio = min_ratio + (base_ratio - min_ratio) / 2;
                           break;
    }
    if (ratio < min_ratio) ratio = min_ratio;
    if (ratio > base_ratio) ratio = base_ratio;

    uint64_t perf_ctl = rdmsr(MSR_IA32_PERF_CTL);
    perf_ctl &= ~0xFFFFULL;
    perf_ctl |= ((uint64_t)ratio & 0xFFu) << 8;
    wrmsr(MSR_IA32_PERF_CTL, perf_ctl);
    kprintf("[cpufreq] governor set to %s (ratio %u => %u kHz)\n",
            plan == PLAN_PERFORMANCE ? "performance" :
            plan == PLAN_SAVER ? "saver" : "balanced",
            ratio, ratio * fi->bus_khz);
}

/* 0 means "not knowable on this machine" -- callers must not read that as
 * a real 0 MHz. Note this reports the CURRENT CORE's ratio: IA32_PERF_STATUS
 * is per-core, so on an SMP box the answer belongs to whichever CPU ran
 * this call, which is why /sys does not publish it per-cpu. */
uint32_t cpufreq_get_mhz(void) {
    const struct cpufreq_msr_info *fi = cpufreq_msr_get();
    if (!fi->present) return 0;
    uint64_t status = rdmsr(MSR_IA32_PERF_STATUS);
    uint32_t ratio = (status >> 8) & 0xFF;
    return ratio * (fi->bus_khz / 1000u);
}

/* Battery */
int battery_get_info(struct battery_info *out) {
    if (!out) return -1;

    /* Read from ACPI EC / _BST method.
     * In QEMU without battery emulation, report not present. */
    memset(out, 0, sizeof(*out));
    out->present = 0;
    out->charging = 0;
    out->percent = 100;
    out->minutes_remaining = -1;
    out->voltage_mv = 0;
    out->current_ma = 0;

    return 0;
}

/* Backlight */
void backlight_set(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_power.backlight_pct = percent;
    /* Would write ACPI _BCM method in a real implementation */
}

int backlight_get(void) {
    return g_power.backlight_pct;
}

/* Idle and lid */
void power_set_idle_timeout(int seconds_dim, int seconds_off, int seconds_sleep) {
    g_power.idle_dim_sec = seconds_dim;
    g_power.idle_off_sec = seconds_off;
    g_power.idle_sleep_sec = seconds_sleep;
}

void power_handle_lid(int closed) {
    g_power.lid_closed = (closed != 0);
    if (closed) {
        kprintf("[power] lid closed\n");
        /* Default action: suspend */
        power_suspend();
    } else {
        kprintf("[power] lid opened\n");
    }
}
