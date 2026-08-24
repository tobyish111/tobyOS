/* sensors -- report hardware monitoring readings from /sys/class/hwmon.
 *
 * The lm-sensors layout: one hwmonN directory per chip, each with a
 * `name` and numbered tempN_input / tempN_label / tempN_crit files.
 * Temperatures are in MILLIdegrees C.
 *
 * On a machine whose firmware exposes no thermal sensor this prints a
 * plain "no sensors found" and exits 1 -- the same thing real sensors(1)
 * does before you run sensors-detect. It never estimates, and it never
 * falls back to a nominal figure: an invented die temperature is the
 * kind of number somebody could act on.
 *
 *   -u   raw values, one per line (scripting form)
 *   -A   omit the adapter/label decoration
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define HWMON_DIR "/sys/class/hwmon"
#define MAX_CHIP  16
#define MAX_TEMP  16

static int read_attr(const char *chip, const char *attr, char *buf, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s/%s", HWMON_DIR, chip, attr);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return -1; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return 0;
}

/* Returns 0 and sets *out on success. An EMPTY file is NOT a zero
 * reading -- the kernel writes nothing when the sensor says its value is
 * not currently valid, and printing 0.0 C for that would be a fabricated
 * measurement. */
static int read_milli(const char *chip, const char *attr, long *out) {
    char buf[64];
    if (read_attr(chip, attr, buf, sizeof buf) != 0) return -1;
    if (buf[0] == '\0') return -1;
    char *end = buf;
    long v = strtol(buf, &end, 10);
    if (end == buf) return -1;
    *out = v;
    return 0;
}

static void print_temp(const char *label, long milli, long crit, int have_crit,
                       int raw) {
    if (raw) {
        printf("%s:\n  temp_input: %ld.%03ld\n", label, milli / 1000,
               (milli < 0 ? -milli : milli) % 1000);
        if (have_crit)
            printf("  temp_crit: %ld.%03ld\n", crit / 1000, crit % 1000);
        return;
    }
    /* lm-sensors renders one decimal: "+55.0 C". */
    long whole = milli / 1000;
    long frac  = (milli < 0 ? -milli : milli) % 1000 / 100;
    printf("%-14s %c%ld.%ld C", label, milli < 0 ? '-' : '+',
           whole < 0 ? -whole : whole, frac);
    if (have_crit) printf("  (crit = +%ld.0 C)", crit / 1000);
    printf("\n");
}

int main(int argc, char **argv) {
    int raw = 0, no_adapter = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') continue;
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'u') raw = 1;
            else if (*c == 'A') no_adapter = 1;
            else {
                fprintf(stderr, "sensors: unknown option -- %c\n", *c);
                fprintf(stderr, "usage: sensors [-u] [-A]\n");
                return 2;
            }
        }
    }

    DIR *d = opendir(HWMON_DIR);
    if (!d) {
        fprintf(stderr, "sensors: no sensors found\n");
        fprintf(stderr, "sensors: %s does not exist -- this machine's CPU "
                        "exposes no thermal sensor tobyOS can read\n",
                HWMON_DIR);
        return 1;
    }

    static char chips[MAX_CHIP][64];
    int nchip = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && nchip < MAX_CHIP) {
        if (ent->d_name[0] == '.') continue;
        snprintf(chips[nchip], sizeof chips[nchip], "%s", ent->d_name);
        nchip++;
    }
    closedir(d);

    if (nchip == 0) {
        fprintf(stderr, "sensors: no sensors found\n");
        return 1;
    }

    int printed = 0;
    for (int i = 0; i < nchip; i++) {
        char name[64];
        if (read_attr(chips[i], "name", name, sizeof name) != 0 || !name[0])
            snprintf(name, sizeof name, "%s", chips[i]);
        printf("%s-isa-0000\n", name);
        if (!no_adapter) printf("Adapter: MSR interface\n");

        for (int t = 1; t <= MAX_TEMP; t++) {
            char attr[32], label[64];
            long milli = 0, crit = 0;
            snprintf(attr, sizeof attr, "temp%d_input", t);
            if (read_milli(chips[i], attr, &milli) != 0) {
                /* Distinguish "no such sensor" from "sensor present but
                 * not reporting right now": the label file existing is
                 * what tells them apart. */
                snprintf(attr, sizeof attr, "temp%d_label", t);
                if (read_attr(chips[i], attr, label, sizeof label) == 0
                    && label[0]) {
                    printf("%-14s (reading not valid)\n", label);
                    printed++;
                }
                continue;
            }
            snprintf(attr, sizeof attr, "temp%d_label", t);
            if (read_attr(chips[i], attr, label, sizeof label) != 0 || !label[0])
                snprintf(label, sizeof label, "temp%d", t);
            snprintf(attr, sizeof attr, "temp%d_crit", t);
            int have_crit = (read_milli(chips[i], attr, &crit) == 0);

            char lbl[70];
            snprintf(lbl, sizeof lbl, "%s:", label);
            print_temp(lbl, milli, crit, have_crit, raw);
            printed++;
        }
        printf("\n");
    }

    if (!printed) {
        fprintf(stderr, "sensors: hwmon present but reported no readings\n");
        return 1;
    }
    return 0;
}
