/* lspci -- list PCI devices, reading /sys/bus/pci/devices.
 *
 * Deliberately a sysfs consumer rather than a wrapper around a tobyOS
 * syscall: the point is that the /sys tree the kernel now publishes is
 * genuinely usable by ordinary Linux-shaped software. devlist(1) is the
 * native view that talks to the kernel directly; this is the portable
 * one, and it exercises the same files pciutils would.
 *
 * Why it ships at all, given busybox carries an lspci applet: that applet
 * walks the tree in a way this kernel's readdir does not satisfy (it
 * prints "(null) Class 0000" for every device even though every file it
 * stats reads back correctly through cat). Rather than reverse-engineer
 * it further, this is ~200 honest lines that read exactly the documented
 * files. The initrd rule links applets only for names nothing else owns,
 * so shipping this binary takes the name back automatically.
 *
 * 2026-08-24 -- THE OPTION SET. This understood only -n/-m/-v/-k, so the
 * first thing anyone tries on real hardware (`lspci -t`, `lspci -s ...`,
 * or just `lspci --help`) bounced with "unknown option" and exit 2. That
 * was observed on the EliteDesk. A tool that carries a famous name owes
 * that name's spelling.
 *
 *   -n        numeric IDs (the only mode here -- we carry no PCI ID
 *             database, so there are no names to suppress)
 *   -nn       accepted; identical to -n for the same reason
 *   -m, -mm   machine-readable, one quoted field per column
 *   -v -vv    verbose: kernel driver, IRQ, revision, subsystem
 *   -k        kernel driver in use (implied by -v)
 *   -t        tree view
 *   -D        always print the domain, even though it is always 0000
 *   -s SLOT   filter [[[[domain]:]bus]:][device][.func]; empty = any
 *   -d ID     filter [vendor]:[device], hex; empty = any
 *   -h        usage
 *
 * NOT implemented, deliberately: -x / -xxx (config-space hex dump). This
 * tree publishes vendor/device/class/revision/irq/subsystem_* per device
 * and NOT raw config space, and printing fabricated bytes under a name
 * that means "the actual register contents" would be the exact lie the
 * rest of this program exists to avoid. It says so and exits non-zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define PCI_DIR "/sys/bus/pci/devices"

/* Read a small sysfs file into buf, NUL-terminated, trailing newline
 * stripped. Returns 0 on success. */
static int read_attr(const char *dev, const char *attr, char *buf, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s/%s", PCI_DIR, dev, attr);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return n ? 0 : -1;
}

static unsigned read_hex(const char *dev, const char *attr, int *ok) {
    char buf[64];
    if (read_attr(dev, attr, buf, sizeof buf) != 0) { if (ok) *ok = 0; return 0; }
    if (ok) *ok = 1;
    return (unsigned)strtoul(buf, NULL, 16);   /* handles the 0x prefix */
}

/* "0000:00:19.0" -> "00:19.0", which is what lspci prints for domain 0
 * unless -D asks for the long form. */
static const char *short_slot(const char *dev) {
    const char *p = strchr(dev, ':');
    return p ? p + 1 : dev;
}

static int cmp_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ---- filters --------------------------------------------------------
 *
 * -1 in any field means "any", which is how lspci spells an omitted
 * component. Both filters are parsed once and then applied per device. */
struct slot_filter { long domain, bus, dev, func; int active; };
struct id_filter    { long vendor, device;         int active; };

/* Parse one hex component; an empty string is the wildcard. Returns 0 on
 * success, -1 if the text is present but not a hex number -- a filter
 * nobody can satisfy is a user error worth reporting, not a silent
 * "matches nothing". */
static int parse_component(const char *s, size_t len, long *out) {
    if (len == 0) { *out = -1; return 0; }
    char tmp[16];
    if (len >= sizeof tmp) return -1;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    char *end = NULL;
    long v = strtol(tmp, &end, 16);
    if (!end || *end || v < 0) return -1;
    *out = v;
    return 0;
}

/* [[[[domain]:]bus]:][device][.func] -- parsed from the RIGHT, because
 * that is what makes the leading fields optional. */
static int parse_slot(const char *arg, struct slot_filter *f) {
    f->domain = f->bus = f->dev = f->func = -1;
    f->active = 1;

    const char *dot = strrchr(arg, '.');
    size_t head_len = dot ? (size_t)(dot - arg) : strlen(arg);
    if (dot && parse_component(dot + 1, strlen(dot + 1), &f->func) != 0) return -1;

    /* Split the head on ':' into at most three fields; assign them from
     * the right so "19" is a device and "00:19" is bus:device. */
    const char *parts[3]; size_t lens[3]; int np = 0;
    const char *p = arg, *end = arg + head_len;
    while (p <= end && np < 3) {
        const char *colon = memchr(p, ':', (size_t)(end - p));
        const char *stop = colon ? colon : end;
        parts[np] = p; lens[np] = (size_t)(stop - p); np++;
        if (!colon) break;
        p = colon + 1;
    }
    if (p <= end && np == 3 && memchr(p, ':', (size_t)(end - p))) return -1;

    long *slots[3] = { &f->dev, &f->bus, &f->domain };   /* right to left */
    for (int i = 0; i < np; i++)
        if (parse_component(parts[np - 1 - i], lens[np - 1 - i], slots[i]) != 0)
            return -1;
    return 0;
}

static int parse_id(const char *arg, struct id_filter *f) {
    f->vendor = f->device = -1;
    f->active = 1;
    const char *colon = strchr(arg, ':');
    if (!colon) return parse_component(arg, strlen(arg), &f->vendor);
    if (parse_component(arg, (size_t)(colon - arg), &f->vendor) != 0) return -1;
    return parse_component(colon + 1, strlen(colon + 1), &f->device);
}

/* dev is the canonical "0000:00:19.0" directory name. */
static int slot_matches(const struct slot_filter *f, const char *dev) {
    if (!f->active) return 1;
    long dom = -1, bus = -1, d = -1, fn = -1;
    if (sscanf(dev, "%lx:%lx:%lx.%lx", &dom, &bus, &d, &fn) != 4) return 0;
    if (f->domain >= 0 && f->domain != dom) return 0;
    if (f->bus    >= 0 && f->bus    != bus) return 0;
    if (f->dev    >= 0 && f->dev    != d)   return 0;
    if (f->func   >= 0 && f->func   != fn)  return 0;
    return 1;
}

static int id_matches(const struct id_filter *f, unsigned vendor, unsigned device) {
    if (!f->active) return 1;
    if (f->vendor >= 0 && (unsigned)f->vendor != vendor) return 0;
    if (f->device >= 0 && (unsigned)f->device != device) return 0;
    return 1;
}

/* ---- output --------------------------------------------------------- */

static void usage(FILE *out) {
    fprintf(out,
        "usage: lspci [-n|-nn] [-m|-mm] [-v|-vv] [-k] [-t] [-D]\n"
        "             [-s [[[[domain]:]bus]:][device][.func]]\n"
        "             [-d [vendor]:[device]]\n"
        "  -n   numeric IDs (the only mode: no PCI ID database is carried)\n"
        "  -m   machine-readable, one quoted field per column\n"
        "  -v   verbose (driver, IRQ, revision, subsystem)\n"
        "  -k   kernel driver in use\n"
        "  -t   tree view\n"
        "  -D   always show the domain\n"
        "  -s   select by slot; omitted components match anything\n"
        "  -d   select by vendor:device\n"
        "  -h   this message\n"
        "\n"
        "-x is not implemented: this system exposes per-device attributes\n"
        "but not raw config space, and printing invented register bytes\n"
        "would be worse than not offering the option.\n");
}

/* The driver name lives in uevent's DRIVER= line; there is no separate
 * `driver` attribute in this tree. */
static void read_driver(const char *dev, char *drv, size_t cap) {
    char ue[512];
    drv[0] = '\0';
    if (read_attr(dev, "uevent", ue, sizeof ue) != 0) return;
    char *p = strstr(ue, "DRIVER=");
    if (!p) return;
    p += 7;
    size_t k = 0;
    while (p[k] && p[k] != '\n' && k < cap - 1) { drv[k] = p[k]; k++; }
    drv[k] = '\0';
}

int main(int argc, char **argv) {
    int machine = 0, verbose = 0, tree = 0, show_domain = 0;
    struct slot_filter sf = { -1, -1, -1, -1, 0 };
    struct id_filter   idf = { -1, -1, 0 };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') continue;
        if (strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        for (const char *c = a + 1; *c; c++) {
            /* -s and -d take a value, either glued on or as the next
             * argument, which is how pciutils accepts them. */
            if (*c == 's' || *c == 'd') {
                const char *val = c[1] ? c + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!val) {
                    fprintf(stderr, "lspci: -%c needs a value\n", *c);
                    usage(stderr);
                    return 2;
                }
                int bad = (*c == 's') ? parse_slot(val, &sf) : parse_id(val, &idf);
                if (bad != 0) {
                    fprintf(stderr, "lspci: cannot parse -%c '%s'\n", *c, val);
                    usage(stderr);
                    return 2;
                }
                break;                       /* the value consumed the rest */
            }
            if      (*c == 'm') machine++;   /* -mm is still machine-readable */
            else if (*c == 'v') verbose++;   /* -vv/-vvv: nothing deeper to show */
            else if (*c == 'k') verbose++;
            else if (*c == 'n') { /* numeric: already the only mode */ }
            else if (*c == 't') tree = 1;
            else if (*c == 'D') show_domain = 1;
            else if (*c == 'h') { usage(stdout); return 0; }
            else if (*c == 'x') {
                fprintf(stderr, "lspci: -x is not available: this system "
                                "publishes per-device attributes but not raw "
                                "PCI config space, and inventing the bytes "
                                "would misreport the hardware\n");
                return 2;
            }
            else {
                fprintf(stderr, "lspci: unknown option -- %c\n", *c);
                usage(stderr);
                return 2;
            }
        }
    }

    DIR *d = opendir(PCI_DIR);
    if (!d) {
        fprintf(stderr, "lspci: cannot open %s: no PCI bus exposed\n", PCI_DIR);
        return 1;
    }

    /* Collect then sort: readdir order is the kernel's table order, but
     * lspci output is conventionally sorted by address. */
    static char names[256][64];
    char *ptrs[256];
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < 256) {
        if (ent->d_name[0] == '.') continue;
        snprintf(names[n], sizeof names[n], "%s", ent->d_name);
        ptrs[n] = names[n];
        n++;
    }
    closedir(d);
    if (n == 0) {
        fprintf(stderr, "lspci: %s is empty\n", PCI_DIR);
        return 1;
    }
    qsort(ptrs, (size_t)n, sizeof ptrs[0], cmp_name);

    /* Which devices survive the filters -- computed first so the tree can
     * draw its last-child connector correctly. */
    int keep[256], kept = 0;
    unsigned vend[256], devid[256];
    for (int i = 0; i < n; i++) {
        keep[i] = 0;
        int okv = 0, okd = 0;
        vend[i]  = read_hex(ptrs[i], "vendor", &okv);
        devid[i] = read_hex(ptrs[i], "device", &okd);
        if (!okv || !okd) continue;
        if (!slot_matches(&sf, ptrs[i])) continue;
        if (!id_matches(&idf, vend[i], devid[i])) continue;
        keep[i] = 1;
        kept++;
    }

    if (tree) {
        /* Bus 0 is the only bus this kernel enumerates, so the tree is one
         * level deep. Drawn in lspci's shape rather than invented. */
        printf("-[0000:00]-");
        int seen = 0;
        for (int i = 0; i < n; i++) {
            if (!keep[i]) continue;
            seen++;
            const char *slot = short_slot(ptrs[i]);
            const char *fn = strchr(slot, ':');
            fn = fn ? fn + 1 : slot;          /* "19.0" */
            if (seen == 1 && kept == 1)      printf("--%s\n", fn);
            else if (seen == 1)              printf("+-%s\n", fn);
            else if (seen == kept)           printf("           \\-%s\n", fn);
            else                             printf("           +-%s\n", fn);
        }
        if (!seen) printf("\n");
        return 0;
    }

    for (int i = 0; i < n; i++) {
        if (!keep[i]) continue;
        const char *dev = ptrs[i];
        int okc = 0;
        unsigned class = read_hex(dev, "class", &okc);
        if (!okc) {
            fprintf(stderr, "lspci: %s: incomplete attributes\n", dev);
            continue;
        }
        unsigned svendor = read_hex(dev, "subsystem_vendor", NULL);
        unsigned sdevice = read_hex(dev, "subsystem_device", NULL);
        const char *addr = show_domain ? dev : short_slot(dev);

        if (machine) {
            printf("%s \"Class %04x\" \"%04x\" \"%04x\" \"%04x\" \"%04x\"\n",
                   addr, (class >> 8) & 0xffff, vend[i], devid[i],
                   svendor, sdevice);
        } else {
            printf("%s Class %04x: %04x:%04x\n",
                   addr, (class >> 8) & 0xffff, vend[i], devid[i]);
        }

        if (verbose && !machine) {
            char drv[64], irq[32], rev[32];
            if (svendor || sdevice)
                printf("\tSubsystem: %04x:%04x\n", svendor, sdevice);
            if (read_attr(dev, "revision", rev, sizeof rev) == 0)
                printf("\tRevision: %s\n", rev);
            read_driver(dev, drv, sizeof drv);
            if (drv[0]) printf("\tKernel driver in use: %s\n", drv);
            if (read_attr(dev, "irq", irq, sizeof irq) == 0 && strcmp(irq, "0"))
                printf("\tIRQ %s\n", irq);
        }
    }
    return 0;
}
