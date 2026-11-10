/* lscpu -- summarise the CPU, from /proc/cpuinfo and /sys/devices/system/cpu.
 *
 * util-linux's lscpu is not a busybox applet, so this is the native one.
 * Like lspci(1) next door it is deliberately a /proc + /sys consumer: if
 * this prints the right thing, so will anything else that identifies the
 * machine the standard way.
 *
 * (/proc/cpuinfo answered "GenuineTobyOS" / "tobyOS virtual x86-64 CPU"
 * for every core until 2026-08-23, which is why this tool arrives in the
 * same change that made that file report the real CPUID strings.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CPUINFO "/proc/cpuinfo"

/* Value of the first "key<tab...>: value" line whose key matches.
 * Returns 0 on success. */
static int cpuinfo_field(const char *key, char *out, size_t cap) {
    FILE *f = fopen(CPUINFO, "r");
    if (!f) return -1;
    char line[512];
    size_t klen = strlen(key);
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) != 0) continue;
        /* the rest of the key must be only tabs/spaces up to the colon */
        const char *p = line + klen;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
        if (n >= cap) n = cap - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

static int count_processors(void) {
    FILE *f = fopen(CPUINFO, "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "processor", 9) == 0) n++;
    fclose(f);
    return n;
}

/* First line of a one-line sysfs file, or NULL. */
static int read_line(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)cap, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return 0;
}

int main(void) {
    int ncpu = count_processors();
    if (ncpu == 0) {
        fprintf(stderr, "lscpu: cannot read %s\n", CPUINFO);
        return 1;
    }

    char vendor[128] = "", brand[256] = "", family[32] = "";
    char model[32] = "", stepping[32] = "", flags[512] = "";
    cpuinfo_field("vendor_id",  vendor,   sizeof vendor);
    cpuinfo_field("model name", brand,    sizeof brand);
    cpuinfo_field("cpu family", family,   sizeof family);
    cpuinfo_field("model",      model,    sizeof model);
    cpuinfo_field("stepping",   stepping, sizeof stepping);
    cpuinfo_field("flags",      flags,    sizeof flags);

    printf("Architecture:        x86_64\n");
    printf("Byte Order:          Little Endian\n");
    printf("CPU(s):              %d\n", ncpu);

    char online[128];
    if (read_line("/sys/devices/system/cpu/online", online, sizeof online) == 0)
        printf("On-line CPU(s) list: %s\n", online);

    if (vendor[0])   printf("Vendor ID:           %s\n", vendor);
    if (brand[0])    printf("Model name:          %s\n", brand);
    if (family[0])   printf("CPU family:          %s\n", family);
    if (model[0])    printf("Model:               %s\n", model);
    if (stepping[0]) printf("Stepping:            %s\n", stepping);
    printf("Thread(s) per core:  1\n");
    printf("Core(s) per socket:  %d\n", ncpu);
    printf("Socket(s):           1\n");
    if (flags[0])    printf("Flags:               %s\n", flags);
    return 0;
}
