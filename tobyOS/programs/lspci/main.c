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
 * Output matches lspci's default: "BB:DD.F Class cccc: vvvv:dddd".
 *   -n   numeric only (the default here -- we carry no PCI ID database)
 *   -m   machine-readable, one quoted field per column
 *   -v   add the driver bound to each device
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

/* "0000:00:19.0" -> "00:19.0", which is what lspci prints for domain 0. */
static const char *short_slot(const char *dev) {
    const char *p = strchr(dev, ':');
    return p ? p + 1 : dev;
}

static int cmp_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv) {
    int machine = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'm') machine = 1;
            else if (*c == 'v' || *c == 'k') verbose = 1;
            else if (*c == 'n') { /* numeric: already the only mode */ }
            else {
                fprintf(stderr, "lspci: unknown option -- %c\n", *c);
                fprintf(stderr, "usage: lspci [-n] [-m] [-v]\n");
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

    for (int i = 0; i < n; i++) {
        const char *dev = ptrs[i];
        int okv = 0, okd = 0, okc = 0;
        unsigned vendor = read_hex(dev, "vendor", &okv);
        unsigned device = read_hex(dev, "device", &okd);
        unsigned class  = read_hex(dev, "class",  &okc);
        if (!okv || !okd || !okc) {
            fprintf(stderr, "lspci: %s: incomplete attributes\n", dev);
            continue;
        }
        unsigned svendor = read_hex(dev, "subsystem_vendor", NULL);
        unsigned sdevice = read_hex(dev, "subsystem_device", NULL);

        if (machine) {
            printf("%s \"Class %04x\" \"%04x\" \"%04x\" \"%04x\" \"%04x\"\n",
                   short_slot(dev), (class >> 8) & 0xffff, vendor, device,
                   svendor, sdevice);
        } else {
            printf("%s Class %04x: %04x:%04x\n",
                   short_slot(dev), (class >> 8) & 0xffff, vendor, device);
        }

        if (verbose) {
            char drv[64], irq[32];
            /* The driver name lives in uevent's DRIVER= line; there is no
             * separate `driver` attribute in this tree. */
            char ue[512];
            drv[0] = '\0';
            if (read_attr(dev, "uevent", ue, sizeof ue) == 0) {
                char *p = strstr(ue, "DRIVER=");
                if (p) {
                    p += 7;
                    size_t k = 0;
                    while (p[k] && p[k] != '\n' && k < sizeof drv - 1) {
                        drv[k] = p[k]; k++;
                    }
                    drv[k] = '\0';
                }
            }
            if (drv[0]) printf("\tKernel driver in use: %s\n", drv);
            if (read_attr(dev, "irq", irq, sizeof irq) == 0 && strcmp(irq, "0"))
                printf("\tIRQ %s\n", irq);
        }
    }
    return 0;
}
