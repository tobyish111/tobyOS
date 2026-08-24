/* lsusb -- list USB devices, reading /sys/bus/usb/devices.
 *
 * A sysfs consumer by design, exactly like the native lspci(1) next to
 * it: the value of the slice is that the standard Linux path now carries
 * real data, so ANY tool that walks it works -- this binary is the proof,
 * not the point.
 *
 * busybox carries an lsusb applet and the initrd hard-links it onto
 * PATH, but the staging rule links an applet only for a name no real
 * binary owns, so shipping this takes the name back.
 *
 * What we can and cannot print, and why:
 *   - The trailing description is the DEVICE'S OWN manufacturer/product
 *     string descriptors, fetched by the xHCI driver at enumeration.
 *     usbutils' lsusb would look the ids up in /usr/share/usb.ids; we
 *     carry no such database, so a device that exposes no strings gets
 *     no description rather than an invented one.
 *   - tobyOS does not synthesise a root-hub device the way Linux does
 *     (Linux fabricates one per host controller with its own 1d6b:000N
 *     ids), so no "Linux Foundation root hub" line appears. Every line
 *     here is a device that really answered GET_DESCRIPTOR.
 *
 * Output matches lsusb's default:
 *   Bus 001 Device 002: ID 0627:0001 QEMU QEMU USB Keyboard
 *
 *   -v   verbose: the descriptor fields sysfs exposes
 *   -t   tree: topology derived from the sysfs device names
 *   -d vendor:product   filter (either half may be empty)
 *   -s [[bus]:][devnum] filter
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define USB_DIR "/sys/bus/usb/devices"
#define MAX_DEV 64

/* Read a small sysfs file, NUL-terminated, trailing newline stripped.
 * Returns 0 on success -- INCLUDING a legitimately empty file. This does
 * not fold "no bytes" into failure the way lspci's helper does, because
 * an absent optional string (a device with no iProduct) must stay
 * distinguishable from a read error. */
static int read_attr(const char *dev, const char *attr, char *buf, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s/%s", USB_DIR, dev, attr);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return -1; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return 0;
}

static unsigned read_num(const char *dev, const char *attr, int base, int *ok) {
    char buf[64];
    if (read_attr(dev, attr, buf, sizeof buf) != 0 || buf[0] == '\0') {
        if (ok) *ok = 0;
        return 0;
    }
    if (ok) *ok = 1;
    return (unsigned)strtoul(buf, NULL, base);
}

/* USB-IF assigned base class codes (usb.org "Defined Class Codes"). A
 * static standards table, not per-device data. */
static const char *class_name(unsigned c) {
    switch (c) {
        case 0x00: return "Defined at Interface level";
        case 0x01: return "Audio";
        case 0x02: return "Communications";
        case 0x03: return "Human Interface Device";
        case 0x05: return "Physical Interface Device";
        case 0x06: return "Imaging";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0a: return "CDC Data";
        case 0x0b: return "Smart Card";
        case 0x0d: return "Content Security";
        case 0x0e: return "Video";
        case 0x0f: return "Personal Healthcare";
        case 0x10: return "Audio/Video";
        case 0x11: return "Billboard";
        case 0xdc: return "Diagnostic";
        case 0xe0: return "Wireless";
        case 0xef: return "Miscellaneous";
        case 0xfe: return "Application Specific";
        case 0xff: return "Vendor Specific Class";
        default:   return "Unknown";
    }
}

struct udev {
    char     name[64];       /* sysfs dir name, e.g. "1-5" or "1-1.2" */
    unsigned busnum, devnum;
    unsigned vid, pid;
    unsigned dclass, dsub, dproto;
    unsigned bcd_device, mps0, nconfigs;
    char     version[16];
    char     speed[16];
    char     manufacturer[64];
    char     product[64];
    char     serial[64];
    char     driver[32];
};

static int cmp_dev(const void *a, const void *b) {
    const struct udev *x = (const struct udev *)a;
    const struct udev *y = (const struct udev *)b;
    if (x->busnum != y->busnum) return (int)x->busnum - (int)y->busnum;
    if (x->devnum != y->devnum) return (int)x->devnum - (int)y->devnum;
    return strcmp(x->name, y->name);
}

/* uevent's DRIVER= line; the tree has no separate `driver` attribute. */
static void read_driver(const char *dev, char *out, size_t cap) {
    char ue[512];
    out[0] = '\0';
    if (read_attr(dev, "uevent", ue, sizeof ue) != 0) return;
    char *p = strstr(ue, "DRIVER=");
    if (!p) return;
    p += 7;
    size_t k = 0;
    while (p[k] && p[k] != '\n' && k + 1 < cap) { out[k] = p[k]; k++; }
    out[k] = '\0';
}

/* "1-1.2" -> port-path depth 2. Depth 1 == directly on a root-hub port. */
static int name_depth(const char *name) {
    const char *dash = strchr(name, '-');
    if (!dash) return 0;
    int depth = 1;
    for (const char *p = dash + 1; *p; p++) if (*p == '.') depth++;
    return depth;
}

/* Last component of the port path: the port on the device's parent. */
static unsigned name_port(const char *name) {
    const char *dash = strchr(name, '-');
    if (!dash) return 0;
    const char *last = strrchr(name, '.');
    return (unsigned)strtoul(last ? last + 1 : dash + 1, NULL, 10);
}

/* Is `child` directly below `parent` in the port-path naming?
 * "1-1" is the parent of "1-1.2" but not of "1-1.2.3". */
static int is_child_of(const char *parent, const char *child) {
    size_t pl = strlen(parent);
    if (strncmp(child, parent, pl) != 0 || child[pl] != '.') return 0;
    return strchr(child + pl + 1, '.') == NULL;
}

static void print_tree(struct udev *d, int n, const char *parent, int indent) {
    for (int i = 0; i < n; i++) {
        int is_root = (parent == NULL && name_depth(d[i].name) == 1);
        if (!is_root && (parent == NULL || !is_child_of(parent, d[i].name)))
            continue;
        printf("%*s|__ Port %u: Dev %u, Class=%s, Driver=%s, %sM\n",
               indent, "", name_port(d[i].name), d[i].devnum,
               class_name(d[i].dclass),
               d[i].driver[0] ? d[i].driver : "(none)",
               d[i].speed[0] ? d[i].speed : "?");
        print_tree(d, n, d[i].name, indent + 4);
    }
}

int main(int argc, char **argv) {
    int verbose = 0, tree = 0;
    long f_vid = -1, f_pid = -1, f_bus = -1, f_dev = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') continue;
        if (strcmp(a, "-d") == 0 || strcmp(a, "-s") == 0) {
            const char *val = (i + 1 < argc) ? argv[++i] : "";
            const char *colon = strchr(val, ':');
            if (a[1] == 'd') {
                if (val[0] && val[0] != ':') f_vid = strtol(val, NULL, 16);
                if (colon && colon[1]) f_pid = strtol(colon + 1, NULL, 16);
            } else {
                if (colon) {
                    if (colon != val) f_bus = strtol(val, NULL, 10);
                    if (colon[1]) f_dev = strtol(colon + 1, NULL, 10);
                } else if (val[0]) {
                    f_dev = strtol(val, NULL, 10);
                }
            }
            continue;
        }
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'v') verbose = 1;
            else if (*c == 't') tree = 1;
            else {
                fprintf(stderr, "lsusb: unknown option -- %c\n", *c);
                fprintf(stderr,
                        "usage: lsusb [-v] [-t] [-d vid:pid] [-s [bus]:[dev]]\n");
                return 2;
            }
        }
    }

    DIR *dir = opendir(USB_DIR);
    if (!dir) {
        fprintf(stderr, "lsusb: cannot open %s: no USB bus exposed\n", USB_DIR);
        return 1;
    }

    static struct udev devs[MAX_DEV];
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < MAX_DEV) {
        if (ent->d_name[0] == '.') continue;
        struct udev *u = &devs[n];
        memset(u, 0, sizeof *u);
        snprintf(u->name, sizeof u->name, "%s", ent->d_name);

        int okb = 0, okd = 0, okv = 0, okp = 0;
        u->busnum = read_num(u->name, "busnum", 10, &okb);
        u->devnum = read_num(u->name, "devnum", 10, &okd);
        u->vid    = read_num(u->name, "idVendor",  16, &okv);
        u->pid    = read_num(u->name, "idProduct", 16, &okp);
        if (!okb || !okd || !okv || !okp) {
            fprintf(stderr, "lsusb: %s: incomplete attributes\n", u->name);
            continue;
        }
        u->dclass     = read_num(u->name, "bDeviceClass",       16, NULL);
        u->dsub       = read_num(u->name, "bDeviceSubClass",    16, NULL);
        u->dproto     = read_num(u->name, "bDeviceProtocol",    16, NULL);
        u->bcd_device = read_num(u->name, "bcdDevice",          16, NULL);
        u->mps0       = read_num(u->name, "bMaxPacketSize0",    10, NULL);
        u->nconfigs   = read_num(u->name, "bNumConfigurations", 10, NULL);
        read_attr(u->name, "version", u->version, sizeof u->version);
        read_attr(u->name, "speed",   u->speed,   sizeof u->speed);
        read_attr(u->name, "manufacturer", u->manufacturer, sizeof u->manufacturer);
        read_attr(u->name, "product",      u->product,      sizeof u->product);
        read_attr(u->name, "serial",       u->serial,       sizeof u->serial);
        read_driver(u->name, u->driver, sizeof u->driver);
        n++;
    }
    closedir(dir);

    if (n == 0) {
        fprintf(stderr, "lsusb: %s is empty -- no USB devices enumerated\n",
                USB_DIR);
        return 1;
    }
    qsort(devs, (size_t)n, sizeof devs[0], cmp_dev);

    if (tree) {
        unsigned last_bus = 0;
        for (int i = 0; i < n; i++) {
            if (devs[i].busnum != last_bus) {
                last_bus = devs[i].busnum;
                printf("/:  Bus %03u\n", last_bus);
            }
        }
        print_tree(devs, n, NULL, 4);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        struct udev *u = &devs[i];
        if (f_vid >= 0 && (long)u->vid != f_vid) continue;
        if (f_pid >= 0 && (long)u->pid != f_pid) continue;
        if (f_bus >= 0 && (long)u->busnum != f_bus) continue;
        if (f_dev >= 0 && (long)u->devnum != f_dev) continue;

        /* Description: the device's own strings, or nothing at all. */
        char desc[132];
        desc[0] = '\0';
        if (u->manufacturer[0] && u->product[0])
            snprintf(desc, sizeof desc, " %s %s", u->manufacturer, u->product);
        else if (u->product[0])
            snprintf(desc, sizeof desc, " %s", u->product);
        else if (u->manufacturer[0])
            snprintf(desc, sizeof desc, " %s", u->manufacturer);

        printf("Bus %03u Device %03u: ID %04x:%04x%s\n",
               u->busnum, u->devnum, u->vid, u->pid, desc);

        if (verbose) {
            printf("Device Descriptor:\n");
            if (u->version[0]) printf("  bcdUSB              %s\n", u->version);
            printf("  bDeviceClass          %3u %s\n",
                   u->dclass, class_name(u->dclass));
            printf("  bDeviceSubClass       %3u\n", u->dsub);
            printf("  bDeviceProtocol       %3u\n", u->dproto);
            printf("  bMaxPacketSize0       %3u\n", u->mps0);
            printf("  idVendor           0x%04x\n", u->vid);
            printf("  idProduct          0x%04x\n", u->pid);
            printf("  bcdDevice          %2x.%02x\n",
                   u->bcd_device >> 8, u->bcd_device & 0xff);
            if (u->manufacturer[0])
                printf("  iManufacturer           %s\n", u->manufacturer);
            if (u->product[0])
                printf("  iProduct                %s\n", u->product);
            if (u->serial[0])
                printf("  iSerial                 %s\n", u->serial);
            printf("  bNumConfigurations    %3u\n", u->nconfigs);
            printf("  (speed)             %s Mbit/s\n",
                   u->speed[0] ? u->speed : "unknown");
            printf("  (driver)            %s\n",
                   u->driver[0] ? u->driver : "(none)");
        }
    }
    return 0;
}
