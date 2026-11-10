/* cputherm.c -- Intel package thermal sensor (slice 3).
 *
 * The digital thermal sensor does not report a temperature. It reports
 * how many degrees BELOW the junction maximum the die currently is, and
 * TjMax itself lives in a different MSR. So a reading is
 *
 *     temperature = TjMax - readout
 *
 * and getting TjMax wrong silently shifts every number by a constant --
 * the exact shape of "plausible but wrong" this arc exists to prevent.
 * Hence: TjMax is read from MSR_TEMPERATURE_TARGET and sanity-checked,
 * and if it is not credible we publish nothing at all rather than fall
 * back to the common-but-not-universal 100 C guess.
 *
 * Gating (see cputelem.h for why this is not optional): CPUID.06H:EAX
 * bit 6 = PTM, the package thermal facility. Bit 0 (DTS) advertises only
 * the PER-CORE sensor, which we cannot attribute to a core without a
 * cross-CPU call -- so DTS alone is reported as "unpublishable", not
 * quietly used as if it were the package.
 */

#include <tobyos/cputelem.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define MSR_IA32_PACKAGE_THERM_STATUS  0x1B1u
#define MSR_IA32_TEMPERATURE_TARGET    0x1A2u

static struct cputherm_info g_therm;
static bool g_therm_inited;

static inline void cpuid_raw(uint32_t leaf, uint32_t subleaf,
                             uint32_t *a, uint32_t *b,
                             uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

static bool is_intel(void) {
    uint32_t a, b, c, d;
    cpuid_raw(0, 0, &a, &b, &c, &d);
    /* "GenuineIntel" == ebx "Genu", edx "ineI", ecx "ntel" */
    return b == 0x756e6547u && d == 0x49656e69u && c == 0x6c65746eu;
}

bool cputherm_decode(uint64_t therm_status, uint32_t tjmax_c,
                     int32_t *out_millideg) {
    /* Bit 31 is "reading valid". Without it the readout field is stale
     * or meaningless, and a stale temperature is worse than none. */
    if (!(therm_status & (1ull << 31))) return false;
    uint32_t delta = (uint32_t)((therm_status >> 16) & 0x7Fu);
    if (out_millideg) {
        *out_millideg = ((int32_t)tjmax_c - (int32_t)delta) * 1000;
    }
    return true;
}

void cputherm_init(void) {
    memset(&g_therm, 0, sizeof(g_therm));
    g_therm_inited = true;
    g_therm.label = "Package id 0";

    if (!is_intel()) {
        g_therm.why = "not an Intel CPU (AMD reports temperature over SMN, "
                      "not these MSRs)";
        goto done;
    }

    uint32_t a, b, c, d;
    cpuid_raw(0, 0, &a, &b, &c, &d);
    if (a < 6) { g_therm.why = "CPUID leaf 6 unavailable"; goto done; }

    cpuid_raw(6, 0, &a, &b, &c, &d);
    bool dts = (a & (1u << 0)) != 0;
    bool ptm = (a & (1u << 6)) != 0;
    if (!ptm) {
        g_therm.why = dts
            ? "only the per-core sensor (DTS) is present; tobyOS has no "
              "cross-CPU read, so a reading cannot be attributed to a core"
            : "no thermal sensor advertised (CPUID.06H:EAX bit 6 clear)";
        goto done;
    }

    /* TjMax. Bits 23:16 of MSR_TEMPERATURE_TARGET. Gated by PTM above --
     * this MSR is Nehalem-and-later, same generation that introduced the
     * package facility. */
    uint64_t tt = rdmsr(MSR_IA32_TEMPERATURE_TARGET);
    uint32_t tjmax = (uint32_t)((tt >> 16) & 0xFFu);
    /* Every real part is between roughly 70 and 120. A 0 here means the
     * MSR is not really implemented (some emulators answer zero rather
     * than faulting), and 0 would make every temperature negative. */
    if (tjmax < 60 || tjmax > 130) {
        g_therm.why = "MSR_TEMPERATURE_TARGET gave an implausible TjMax";
        kprintf("[cputherm] TjMax readback %u C is out of range -- "
                "refusing to publish a shifted temperature\n", tjmax);
        goto done;
    }

    g_therm.tjmax_c = tjmax;
    g_therm.present = true;

done:
    if (g_therm.present) {
        int32_t mc = 0;
        bool ok = cputherm_read_mc(&mc);
        kprintf("[cputherm] package sensor: TjMax=%u C, now=%d.%u C%s\n",
                g_therm.tjmax_c, mc / 1000,
                (unsigned)((mc % 1000) / 100),
                ok ? "" : " (reading not valid yet)");
    } else {
        kprintf("[cputherm] no package thermal sensor: %s\n",
                g_therm.why ? g_therm.why : "unknown");
    }
}

bool cputherm_read_mc(int32_t *out_millideg) {
    if (!g_therm.present) return false;
    uint64_t st = rdmsr(MSR_IA32_PACKAGE_THERM_STATUS);
    return cputherm_decode(st, g_therm.tjmax_c, out_millideg);
}

const struct cputherm_info *cputherm_get(void) {
    if (!g_therm_inited) cputherm_init();
    return &g_therm;
}

#ifdef CPUTELEM_SELFTEST
/* The POSITIVE path cannot be exercised under QEMU -- it advertises no
 * thermal sensor, so the gate can only ever watch this module decline to
 * publish. That leaves the arithmetic, which is the part most likely to
 * be silently wrong, completely untested on the machine we can actually
 * run. So test the DECODER against known bit patterns instead: it needs
 * no sensor, and it is exactly where a TjMax or shift mistake would hide.
 *
 * These are not invented numbers pretending to be readings -- nothing is
 * published from them. They are inputs to a pure function with a
 * spec-defined output. */
int cputherm_selftest(void) {
    struct { uint64_t st; uint32_t tjmax; bool ok; int32_t mc; const char *what; }
    v[] = {
        /* valid bit set, 36 below a TjMax of 100 => 64.000 C */
        { (1ull << 31) | (36ull << 16), 100, true,  64000, "typical idle" },
        /* at TjMax exactly */
        { (1ull << 31) | (0ull  << 16), 100, true, 100000, "at TjMax" },
        /* 100 below TjMax => 0 C */
        { (1ull << 31) | (100ull << 16), 100, true,     0, "TjMax-100" },
        /* Haswell-ish: TjMax 100, 45 below => 55.000 C */
        { (1ull << 31) | (45ull << 16), 100, true,  55000, "Haswell 55C" },
        /* readout field is 7 bits: bit 23 must NOT leak in */
        { (1ull << 31) | (0xFFull << 16), 100, true, 100000 - 127000,
          "7-bit readout mask" },
        /* validity bit CLEAR must be refused, however sane the field */
        { (36ull << 16), 100, false, 0, "invalid bit refused" },
    };
    int pass = 0, n = (int)(sizeof(v) / sizeof(v[0]));
    for (int i = 0; i < n; i++) {
        int32_t got = 0x7f000000;
        bool ok = cputherm_decode(v[i].st, v[i].tjmax, &got);
        bool good = (ok == v[i].ok) && (!ok || got == v[i].mc);
        if (good) pass++;
        kprintf("[CPUTELEM] %-22s therm=0x%lx tjmax=%u -> ok=%d mc=%d "
                "(want ok=%d mc=%d) %s\n",
                v[i].what, (unsigned long)v[i].st, v[i].tjmax,
                (int)ok, ok ? got : 0, (int)v[i].ok, v[i].ok ? v[i].mc : 0,
                good ? "PASS" : "FAIL");
    }
    kprintf("[CPUTELEM] cputherm_decode: %d/%d\n", pass, n);
    return pass == n ? 0 : 1;
}
#endif
