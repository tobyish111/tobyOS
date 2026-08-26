/* cpufreq.c -- Intel P-state frequency limits (slice 4).
 *
 * READ-ONLY, deliberately. Setting a P-state (IA32_PERF_CTL, 0x199) is a
 * separate and riskier change; the reporting work must not carry it.
 *
 * MSR_PLATFORM_INFO (0xCE) describes the package's guaranteed operating
 * points as RATIOS against the reference clock:
 *
 *     bits 15:8   max non-turbo ratio   ("base" frequency)
 *     bits 47:40  max efficiency ratio  (the lowest guaranteed P-state)
 *
 * Frequency = ratio * bus clock, and the bus clock is 100 MHz on every
 * part that has this MSR (Nehalem onward). That constant is the one
 * assumption here, and it is why the gate below refuses anything older:
 * on pre-Nehalem parts the reference clock came from the FSB and varied
 * per board, so the same arithmetic would produce confidently wrong
 * numbers rather than no numbers.
 *
 * WHAT IS NOT PUBLISHED, and why (see cputelem.h):
 *   - scaling_cur_freq. IA32_PERF_STATUS is PER-CORE and a sysfs read is
 *     serviced by an arbitrary CPU, so the answer could not honestly be
 *     filed under cpu<N>.
 *   - scaling_governor. tobyOS does not govern P-states at all. Linux
 *     software reads this file to learn the policy in force; publishing
 *     "performance" would claim a policy we do not implement.
 */

#include <tobyos/cputelem.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define MSR_PLATFORM_INFO   0x0CEu
#define BUS_CLOCK_KHZ       100000u   /* 100 MHz, Nehalem and later */

static struct cpufreq_msr_info g_freq;
static bool g_freq_inited;

static inline void cpuid_raw(uint32_t leaf, uint32_t subleaf,
                             uint32_t *a, uint32_t *b,
                             uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

bool cpufreq_msr_decode(uint64_t platform_info, uint32_t bus_khz,
                    uint32_t *base_khz, uint32_t *min_khz) {
    uint32_t base_ratio = (uint32_t)((platform_info >> 8)  & 0xFFu);
    uint32_t min_ratio  = (uint32_t)((platform_info >> 40) & 0xFFu);

    /* An emulator that answers 0 to an MSR it does not implement must be
     * rejected here, not turned into "0 kHz". A real ratio is at least 4
     * (400 MHz) and the efficiency ratio never exceeds the base one. */
    if (base_ratio < 4 || base_ratio > 120) return false;
    if (min_ratio  < 1 || min_ratio > base_ratio) return false;

    if (base_khz) *base_khz = base_ratio * bus_khz;
    if (min_khz)  *min_khz  = min_ratio  * bus_khz;
    return true;
}

void cpufreq_msr_init(void) {
    memset(&g_freq, 0, sizeof(g_freq));
    g_freq_inited = true;
    g_freq.driver  = "tobyos-msr";
    g_freq.bus_khz = BUS_CLOCK_KHZ;

    uint32_t a, b, c, d;
    cpuid_raw(0, 0, &a, &b, &c, &d);
    bool intel = (b == 0x756e6547u && d == 0x49656e69u && c == 0x6c65746eu);
    if (!intel) {
        g_freq.why = "not an Intel CPU (MSR_PLATFORM_INFO is Intel-specific)";
        goto done;
    }

    cpuid_raw(1, 0, &a, &b, &c, &d);
    uint32_t family = (a >> 8) & 0xFu;
    uint32_t model  = (a >> 4) & 0xFu;
    if (family == 0xF) family += (a >> 20) & 0xFFu;
    if (family == 6 || family == 0xF) model |= ((a >> 16) & 0xFu) << 4;

    /* EIST (SpeedStep) is the documented gate for the P-state MSRs. */
    if (!(c & (1u << 7))) {
        g_freq.why = "no SpeedStep/EIST advertised (CPUID.01H:ECX bit 7 clear)";
        goto done;
    }
    /* MSR_PLATFORM_INFO arrived with Nehalem (family 6, model >= 0x1A).
     * Older parts either #GP or, worse, alias something else. */
    if (family != 6 || model < 0x1A) {
        g_freq.why = "pre-Nehalem: MSR_PLATFORM_INFO not architectural, and "
                     "the reference clock is not 100 MHz";
        goto done;
    }

    uint64_t pi = rdmsr(MSR_PLATFORM_INFO);
    if (!cpufreq_msr_decode(pi, BUS_CLOCK_KHZ, &g_freq.base_khz, &g_freq.min_khz)) {
        g_freq.why = "MSR_PLATFORM_INFO ratios failed the sanity check";
        kprintf("[cpufreq] MSR_PLATFORM_INFO=0x%lx decoded to implausible "
                "ratios -- publishing nothing\n", (unsigned long)pi);
        goto done;
    }
    /* max == base on purpose. Turbo (MSR 0x1AD) is opportunistic and
     * bounded by power and thermal budget, so reporting it as a ceiling
     * would advertise a frequency the part does not guarantee. */
    g_freq.max_khz = g_freq.base_khz;
    g_freq.present = true;

done:
    if (g_freq.present) {
        kprintf("[cpufreq] %s: base %u.%02u GHz, min %u.%02u GHz "
                "(bus %u MHz)\n",
                g_freq.driver,
                g_freq.base_khz / 1000000u, (g_freq.base_khz / 10000u) % 100u,
                g_freq.min_khz  / 1000000u, (g_freq.min_khz  / 10000u) % 100u,
                g_freq.bus_khz / 1000u);
    } else {
        kprintf("[cpufreq] no P-state information: %s\n",
                g_freq.why ? g_freq.why : "unknown");
    }
}

/* Self-initialising: the ACPI power code calls this from a path whose
 * ordering against kmain is not fixed, and an uninitialised read would
 * silently answer "no P-state information" on a machine that has it. */
const struct cpufreq_msr_info *cpufreq_msr_get(void) {
    if (!g_freq_inited) cpufreq_msr_init();
    return &g_freq;
}

#ifdef CPUTELEM_SELFTEST
/* Same reasoning as cputherm_selftest: QEMU advertises no EIST, so the
 * gate can only watch this decline to publish. The ratio arithmetic and
 * -- more importantly -- the SANITY CHECK that rejects an emulator
 * answering zero are what actually need proving. */
int cpufreq_msr_selftest(void) {
    struct { uint64_t pi; bool ok; uint32_t base, min; const char *what; }
    v[] = {
        /* i5-4590 (the EliteDesk): base ratio 33, efficiency ratio 8 */
        { (33ull << 8) | (8ull << 40), true, 3300000, 800000, "i5-4590 33x/8x" },
        /* 8-bit fields: neighbouring bits must not leak in */
        { (0xFFull << 8) | (0xFFull << 40), false, 0, 0, "ratio 255 rejected" },
        /* THE case that matters on an emulator: MSR reads back as zero */
        { 0, false, 0, 0, "all-zero MSR rejected" },
        /* implausibly low base ratio */
        { (3ull << 8) | (2ull << 40), false, 0, 0, "base ratio 3 rejected" },
        /* efficiency ratio above base is nonsense */
        { (20ull << 8) | (30ull << 40), false, 0, 0, "min>base rejected" },
        /* a plain 2.0 GHz part */
        { (20ull << 8) | (8ull << 40), true, 2000000, 800000, "20x/8x" },
    };
    int pass = 0, n = (int)(sizeof(v) / sizeof(v[0]));
    for (int i = 0; i < n; i++) {
        uint32_t base = 0xFFFFFFFFu, min = 0xFFFFFFFFu;
        bool ok = cpufreq_msr_decode(v[i].pi, BUS_CLOCK_KHZ, &base, &min);
        bool good = (ok == v[i].ok) &&
                    (!ok || (base == v[i].base && min == v[i].min));
        if (good) pass++;
        kprintf("[CPUTELEM] %-22s pi=0x%lx -> ok=%d base=%u min=%u "
                "(want ok=%d base=%u min=%u) %s\n",
                v[i].what, (unsigned long)v[i].pi, (int)ok,
                ok ? base : 0, ok ? min : 0,
                (int)v[i].ok, v[i].base, v[i].min, good ? "PASS" : "FAIL");
    }
    kprintf("[CPUTELEM] cpufreq_msr_decode: %d/%d\n", pass, n);
    return pass == n ? 0 : 1;
}
#endif
