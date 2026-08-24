/* cputelem.h -- CPU telemetry read from Model-Specific Registers.
 *
 * Two independent facilities that share one discipline:
 *   - cputherm: package temperature      (slices' hwmon / `sensors`)
 *   - cpufreq:  P-state frequency limits (slices' cpufreq / `cpupower`)
 *
 * THE RULE THAT SHAPES ALL OF THIS: an MSR that the CPU does not
 * implement raises #GP, and tobyOS has NO exception-fixup table (see
 * src/page_fault.c) -- a speculative rdmsr is therefore a triple-fault
 * waiting for the wrong machine. Every read below is gated on a
 * DOCUMENTED CPUID feature bit or family/model range first, exactly as
 * Linux's coretemp and intel_pstate do, and every decoded value is
 * sanity-checked before it is believed. When a gate says no, we publish
 * NOTHING rather than a plausible number.
 *
 * WHAT IS DELIBERATELY NOT HERE, and why: per-CORE readings. MSR 0x19C
 * (IA32_THERM_STATUS) and IA32_PERF_STATUS are per-core registers, and
 * a sysfs read is serviced by whichever CPU happens to run the reader.
 * tobyOS has no cross-CPU call primitive and sched_setaffinity is a
 * no-op stub, so a value read for "cpu3" could come from any core.
 * Publishing it under cpu3's name would be a mislabelled fact, which is
 * the same sin as an invented one. Only genuinely PACKAGE-WIDE registers
 * are published: MSR 0x1B1 (package thermal status), MSR 0x1A2
 * (temperature target) and MSR 0xCE (platform info) all read identically
 * from any core.
 */

#ifndef TOBYOS_CPUTELEM_H
#define TOBYOS_CPUTELEM_H

#include <tobyos/types.h>

/* ---- thermal ---- */

struct cputherm_info {
    bool     present;      /* package thermal status is readable      */
    uint32_t tjmax_c;      /* junction max, degrees C                 */
    const char *label;     /* "Package id 0"                          */
    const char *why;       /* when !present, the reason, for logging  */
};

void cputherm_init(void);
const struct cputherm_info *cputherm_get(void);

/* Live package temperature in MILLIdegrees C. Returns false when the
 * reading is not valid right now (the MSR's own validity bit is clear),
 * which is distinct from "this machine has no sensor". */
bool cputherm_read_mc(int32_t *out_millideg);

/* Decode helper, split out so the arithmetic can be tested against known
 * bit patterns without a thermal sensor. `therm_status` is the raw MSR
 * value. Returns false if its validity bit (31) is clear. */
bool cputherm_decode(uint64_t therm_status, uint32_t tjmax_c,
                     int32_t *out_millideg);

/* ---- frequency ---- */

struct cpufreq_msr_info {
    bool     present;
    uint32_t base_khz;     /* max non-turbo ("base") frequency        */
    uint32_t min_khz;      /* max efficiency ratio                    */
    uint32_t max_khz;      /* == base: turbo is NOT reported as a      */
                           /* guaranteed ceiling, see cpufreq.c        */
    uint32_t bus_khz;      /* reference clock actually used            */
    const char *driver;
    const char *why;
};

void cpufreq_msr_init(void);
const struct cpufreq_msr_info *cpufreq_msr_get(void);

/* Decode MSR_PLATFORM_INFO. Split out for the same testability reason as
 * cputherm_decode. Returns false when the ratios fail their sanity
 * check, which is how a machine that answers 0 (or garbage) to an MSR it
 * does not really implement is rejected. */
bool cpufreq_msr_decode(uint64_t platform_info, uint32_t bus_khz,
                    uint32_t *base_khz, uint32_t *min_khz);

#endif /* TOBYOS_CPUTELEM_H */
