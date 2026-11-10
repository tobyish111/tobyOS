/* cpupower -- report CPU frequency policy from
 * /sys/devices/system/cpu/cpuN/cpufreq.
 *
 * Only `frequency-info` is implemented, and only its reporting half.
 * `frequency-set` is deliberately absent: tobyOS does not write
 * IA32_PERF_CTL, and a subcommand that accepted a target and silently
 * did nothing would be a knob that enforces nothing -- worse than no
 * subcommand at all.
 *
 * Two files real cpupower expects are NOT published by this kernel, and
 * the reason is worth repeating where a user will see it:
 *   - scaling_cur_freq: IA32_PERF_STATUS is a PER-CORE register, and a
 *     sysfs read is serviced by whichever CPU runs the reader. tobyOS
 *     has no cross-CPU read, so the answer could not honestly be filed
 *     under a named CPU.
 *   - scaling_governor: nothing in tobyOS governs P-states, so there is
 *     no policy to report.
 * This tool says so explicitly rather than printing "unknown".
 *
 *   -c N / --cpu N   report one CPU (default: all)
 *   -q               values only, no prose
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define CPU_DIR "/sys/devices/system/cpu"

static int read_attr(int cpu, const char *attr, char *buf, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/cpu%d/cpufreq/%s", CPU_DIR, cpu, attr);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return -1; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return buf[0] ? 0 : -1;
}

static int read_khz(int cpu, const char *attr, long *out) {
    char buf[64];
    if (read_attr(cpu, attr, buf, sizeof buf) != 0) return -1;
    char *end = buf;
    long v = strtol(buf, &end, 10);
    if (end == buf) return -1;
    *out = v;
    return 0;
}

/* cpupower prints "3.30 GHz" / "800 MHz" style figures. */
static void put_freq(const char *label, long khz) {
    if (khz >= 1000000)
        printf("  %-24s %ld.%02ld GHz\n", label, khz / 1000000,
               (khz / 10000) % 100);
    else
        printf("  %-24s %ld MHz\n", label, khz / 1000);
}

static int have_cpu(int cpu) {
    char buf[64];
    return read_attr(cpu, "scaling_driver", buf, sizeof buf) == 0;
}

static int report(int cpu, int quiet) {
    char driver[64];
    if (read_attr(cpu, "scaling_driver", driver, sizeof driver) != 0) {
        fprintf(stderr, "cpupower: no cpufreq data for cpu%d\n", cpu);
        return 1;
    }
    long base = 0, minf = 0, maxf = 0;
    int hb = read_khz(cpu, "base_frequency",   &base) == 0;
    int hn = read_khz(cpu, "cpuinfo_min_freq", &minf) == 0;
    int hx = read_khz(cpu, "cpuinfo_max_freq", &maxf) == 0;

    printf("analyzing CPU %d:\n", cpu);
    printf("  driver: %s\n", driver);
    if (hn && hx) {
        printf("  hardware limits: ");
        if (minf >= 1000000) printf("%ld.%02ld GHz", minf / 1000000, (minf / 10000) % 100);
        else                 printf("%ld MHz", minf / 1000);
        printf(" - ");
        if (maxf >= 1000000) printf("%ld.%02ld GHz\n", maxf / 1000000, (maxf / 10000) % 100);
        else                 printf("%ld MHz\n", maxf / 1000);
    }
    if (hb) put_freq("base frequency:", base);
    if (hx) put_freq("max non-turbo:", maxf);
    if (hn) put_freq("min (efficiency):", minf);

    if (!quiet) {
        char buf[64];
        if (read_attr(cpu, "scaling_cur_freq", buf, sizeof buf) != 0) {
            printf("  current frequency:       not available -- "
                   "IA32_PERF_STATUS is per-core and tobyOS has no "
                   "cross-CPU read\n");
        } else {
            printf("  current frequency:       %s kHz\n", buf);
        }
        if (read_attr(cpu, "scaling_governor", buf, sizeof buf) != 0) {
            printf("  governor:                none -- tobyOS does not "
                   "manage P-states\n");
        } else {
            printf("  governor:                %s\n", buf);
        }
        printf("  boost state support:     not queried (turbo is not "
               "reported as a guaranteed ceiling)\n");
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    int only = -1, quiet = 0, saw_cmd = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "frequency-info") == 0) { saw_cmd = 1; continue; }
        if (strcmp(a, "-c") == 0 || strcmp(a, "--cpu") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cpupower: -c needs a cpu\n"); return 2; }
            only = atoi(argv[++i]);
            continue;
        }
        if (strcmp(a, "-q") == 0) { quiet = 1; continue; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            printf("usage: cpupower frequency-info [-c CPU] [-q]\n");
            printf("note: frequency-set is not implemented -- tobyOS does "
                   "not write P-states.\n");
            return 0;
        }
        if (strcmp(a, "frequency-set") == 0) {
            fprintf(stderr, "cpupower: frequency-set is not implemented "
                            "(tobyOS does not write IA32_PERF_CTL)\n");
            return 2;
        }
        fprintf(stderr, "cpupower: unknown argument '%s'\n", a);
        return 2;
    }
    (void)saw_cmd;   /* frequency-info is the only mode; bare invocation ok */

    if (only >= 0) return report(only, quiet);

    int found = 0;
    for (int c = 0; c < 256; c++) {
        if (!have_cpu(c)) continue;
        report(c, quiet);
        found++;
    }
    if (!found) {
        fprintf(stderr, "cpupower: no cpufreq information published -- "
                        "this CPU's P-state MSRs are not readable\n");
        return 1;
    }
    return 0;
}
