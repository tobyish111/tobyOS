/* setpci -- read and write PCI configuration space.
 *
 *     setpci [-v] -s [[bus]:][slot][.fn] REG[.B|.W|.L][=VALUE] ...
 *     setpci [-v] -d [vendor]:[device]   REG[...]
 *     setpci --dump -s ADDR                  the whole 256-byte header
 *
 * Reads go through the same 256 bytes /sys/bus/pci/devices/<addr>/config
 * serves, so `setpci -s 00:03.0 COMMAND` and `lspci -x` cannot disagree.
 *
 * WRITES ARE ROOT-ONLY, and the kernel is what enforces that -- not this
 * program. It also bounds the offset to the 256-byte header and refuses
 * an address it never enumerated, because config space is where one wrong
 * dword unmaps a disk controller in the middle of a write. A read-modify-
 * write is used for byte and word widths: the hardware interface is
 * 32 bits wide, and writing a whole dword to set one byte would clear the
 * three beside it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <tobyos/abi/abi.h>

static long sc4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static int verbose;

struct devaddr { int bus, slot, fn; };

/* The register names setpci accepts, with the width each one naturally
 * has. A numeric offset has no natural width, so it needs a suffix. */
static const struct { const char *name; int off, width; } REGS[] = {
    { "VENDOR_ID",          0x00, 2 }, { "DEVICE_ID",       0x02, 2 },
    { "COMMAND",            0x04, 2 }, { "STATUS",          0x06, 2 },
    { "REVISION",           0x08, 1 }, { "CLASS_PROG",      0x09, 1 },
    { "CLASS_DEVICE",       0x0a, 2 }, { "CACHE_LINE_SIZE", 0x0c, 1 },
    { "LATENCY_TIMER",      0x0d, 1 }, { "HEADER_TYPE",     0x0e, 1 },
    { "BIST",               0x0f, 1 },
    { "BASE_ADDRESS_0",     0x10, 4 }, { "BASE_ADDRESS_1",  0x14, 4 },
    { "BASE_ADDRESS_2",     0x18, 4 }, { "BASE_ADDRESS_3",  0x1c, 4 },
    { "BASE_ADDRESS_4",     0x20, 4 }, { "BASE_ADDRESS_5",  0x24, 4 },
    { "CARDBUS_CIS",        0x28, 4 },
    { "SUBSYSTEM_VENDOR_ID", 0x2c, 2 }, { "SUBSYSTEM_ID",   0x2e, 2 },
    { "ROM_ADDRESS",        0x30, 4 }, { "CAPABILITIES",    0x34, 1 },
    { "INTERRUPT_LINE",     0x3c, 1 }, { "INTERRUPT_PIN",   0x3d, 1 },
    { "MIN_GNT",            0x3e, 1 }, { "MAX_LAT",         0x3f, 1 },
    { NULL, 0, 0 }
};

static int pack(const struct devaddr *d) {
    return (d->bus << 16) | (d->slot << 8) | d->fn;
}

static long cfg_read32(const struct devaddr *d, int off) {
    return sc4(ABI_SYS_PCI_CFG, ABI_PCI_CFG_READ32, pack(d), off, 0);
}

static long cfg_write32(const struct devaddr *d, int off, unsigned val) {
    return sc4(ABI_SYS_PCI_CFG, ABI_PCI_CFG_WRITE32, pack(d), off, (long)val);
}

static const char *errname(long rc) {
    switch (-rc) {
    case ABI_EPERM:  return "permission denied (writing config space needs root)";
    case ABI_ENODEV: return "no such PCI function";
    case ABI_EINVAL: return "offset out of range, or not 4-byte aligned";
    default:         return "the kernel refused the request";
    }
}

static int hexval(const char *s, unsigned *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0;
    unsigned v = 0;
    for (; *s; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

/* -s [[bus]:][slot][.fn] -- the fields setpci lets you leave out default
 * the way pciutils defaults them: bus 0, function 0. */
static int parse_slot(const char *s, struct devaddr *d) {
    d->bus = 0; d->slot = -1; d->fn = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    char *dot = strchr(buf, '.');
    if (dot) { *dot = 0; d->fn = (int)strtol(dot + 1, NULL, 16); }
    char *colon = strrchr(buf, ':');
    if (colon) {
        *colon = 0;
        d->slot = (int)strtol(colon + 1, NULL, 16);
        /* a leading domain is accepted and ignored: one domain here */
        char *c2 = strrchr(buf, ':');
        d->bus = (int)strtol(c2 ? c2 + 1 : buf, NULL, 16);
    } else {
        d->slot = (int)strtol(buf, NULL, 16);
    }
    return d->slot >= 0 && d->slot < 32 && d->fn >= 0 && d->fn < 8;
}

/* Every enumerated function, from the directory lspci walks. */
static int all_devices(struct devaddr *out, int max) {
    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(dir)) && n < max) {
        /* 0000:BB:SS.F */
        const char *p = e->d_name;
        if (strlen(p) < 12) continue;
        struct devaddr d;
        d.bus  = (int)strtol(p + 5, NULL, 16);
        d.slot = (int)strtol(p + 8, NULL, 16);
        d.fn   = (int)strtol(p + 11, NULL, 16);
        out[n++] = d;
    }
    closedir(dir);
    return n;
}

static int match_vendor_device(const struct devaddr *d, int want_v, int want_dv) {
    long v = cfg_read32(d, 0);
    if (v < 0) return 0;
    int vendor = (int)(v & 0xFFFF), device = (int)((v >> 16) & 0xFFFF);
    if (want_v >= 0 && vendor != want_v) return 0;
    if (want_dv >= 0 && device != want_dv) return 0;
    return 1;
}

static int do_op(const struct devaddr *d, const char *op) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", op);

    char *eq = strchr(buf, '=');
    const char *valstr = NULL;
    if (eq) { *eq = 0; valstr = eq + 1; }

    int width = 0;
    char *dotw = strrchr(buf, '.');
    if (dotw && dotw[1] && !dotw[2]) {
        char w = dotw[1];
        if (w == 'b' || w == 'B') { width = 1; *dotw = 0; }
        else if (w == 'w' || w == 'W') { width = 2; *dotw = 0; }
        else if (w == 'l' || w == 'L') { width = 4; *dotw = 0; }
    }

    int off = -1;
    for (int i = 0; REGS[i].name; i++) {
        if (strcmp(buf, REGS[i].name)) continue;
        off = REGS[i].off;
        if (!width) width = REGS[i].width;
        break;
    }
    if (off < 0) {
        unsigned o;
        if (!hexval(buf, &o)) {
            fprintf(stderr, "setpci: unknown register '%s'\n", buf);
            return 1;
        }
        off = (int)o;
        if (!width) {
            fprintf(stderr, "setpci: '%s' is a numeric offset and needs a "
                            "width suffix (.B, .W or .L)\n", buf);
            return 1;
        }
    }
    if (off < 0 || off + width > 256) {
        fprintf(stderr, "setpci: offset 0x%02x is outside the 256-byte header\n", off);
        return 1;
    }

    int dword = off & ~3;
    int shift = (off & 3) * 8;
    long cur = cfg_read32(d, dword);
    if (cur < 0) {
        fprintf(stderr, "setpci: %02x:%02x.%d: %s\n", d->bus, d->slot, d->fn,
                errname(cur));
        return 1;
    }
    unsigned mask = (width == 4) ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u);

    if (!valstr) {
        unsigned v = (width == 4) ? (unsigned)cur
                                  : (((unsigned)cur >> shift) & mask);
        printf("%0*x\n", width * 2, v);
        return 0;
    }

    unsigned nv;
    if (!hexval(valstr, &nv)) {
        fprintf(stderr, "setpci: '%s' is not a hex value\n", valstr);
        return 1;
    }
    if (width != 4 && (nv & ~mask)) {
        fprintf(stderr, "setpci: value 0x%x does not fit in %d byte(s)\n", nv, width);
        return 1;
    }
    /* Read-modify-write: the interface is 32 bits wide, so writing a byte
     * means preserving the three next to it. */
    unsigned out = (width == 4) ? nv
                                : (((unsigned)cur & ~(mask << shift)) | ((nv & mask) << shift));
    if (verbose)
        printf("%02x:%02x.%d @%02x %0*x -> %0*x (dword %08x -> %08x)\n",
               d->bus, d->slot, d->fn, off, width * 2,
               (width == 4) ? (unsigned)cur : (((unsigned)cur >> shift) & mask),
               width * 2, nv, (unsigned)cur, out);
    long rc = cfg_write32(d, dword, out);
    if (rc < 0) {
        fprintf(stderr, "setpci: %02x:%02x.%d: %s\n", d->bus, d->slot, d->fn,
                errname(rc));
        return 1;
    }
    return 0;
}

static int dump_device(const struct devaddr *d) {
    for (int off = 0; off < 256; off += 16) {
        printf("%02x:", off);
        for (int i = 0; i < 16; i += 4) {
            long v = cfg_read32(d, off + i);
            if (v < 0) {
                fprintf(stderr, "\nsetpci: %02x:%02x.%d: %s\n",
                        d->bus, d->slot, d->fn, errname(v));
                return 1;
            }
            for (int b = 0; b < 4; b++)
                printf(" %02x", (unsigned)((v >> (b * 8)) & 0xFF));
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    struct devaddr sel[64];
    int nsel = 0, dump = 0;
    int want_v = -1, want_d = -1, filter_vd = 0;
    const char *ops[32];
    int nops = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-s") && i + 1 < argc) {
            struct devaddr d;
            if (!parse_slot(argv[++i], &d)) {
                fprintf(stderr, "setpci: cannot parse the address '%s'\n", argv[i]);
                return 1;
            }
            if (nsel < 64) sel[nsel++] = d;
            continue;
        }
        if (!strcmp(a, "-d") && i + 1 < argc) {
            const char *p = argv[++i];
            const char *colon = strchr(p, ':');
            unsigned v;
            if (colon) {
                char vb[16];
                size_t vl = (size_t)(colon - p);
                if (vl >= sizeof vb) vl = sizeof vb - 1;
                memcpy(vb, p, vl); vb[vl] = 0;
                if (*vb && hexval(vb, &v)) want_v = (int)v;
                if (colon[1] && hexval(colon + 1, &v)) want_d = (int)v;
            } else if (hexval(p, &v)) {
                want_v = (int)v;
            }
            filter_vd = 1;
            continue;
        }
        if (!strcmp(a, "-v")) { verbose = 1; continue; }
        if (!strcmp(a, "--dump") || !strcmp(a, "-x")) { dump = 1; continue; }
        if (!strcmp(a, "--version")) { printf("setpci (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: setpci [-v] -s [[bus]:][slot][.fn] REG[.B|.W|.L][=VALUE] ...\n"
                   "       setpci [-v] -d [vendor]:[device] REG ...\n"
                   "       setpci --dump -s ADDR\n"
                   "registers:");
            for (int k = 0; REGS[k].name; k++) printf(" %s", REGS[k].name);
            printf("\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "setpci: unknown option %s\n", a); return 1; }
        if (nops < 32) ops[nops++] = a;
    }

    if (nsel == 0) {
        nsel = all_devices(sel, 64);
        if (nsel == 0) {
            fprintf(stderr, "setpci: no PCI devices found "
                            "(is /sys/bus/pci/devices there?)\n");
            return 1;
        }
        if (!filter_vd && !dump && nops) {
            /* pciutils applies an op to every device when none is named;
             * that is a lot of writes to make by accident. */
            if (strchr(ops[0], '=')) {
                fprintf(stderr, "setpci: refusing to write to every device -- "
                                "name one with -s or -d\n");
                return 1;
            }
        }
    }

    if (nops == 0 && !dump) {
        fprintf(stderr, "usage: setpci [-v] -s ADDR REG[.B|.W|.L][=VALUE] ...\n");
        return 1;
    }

    int rc = 0, acted = 0;
    for (int i = 0; i < nsel; i++) {
        if (filter_vd && !match_vendor_device(&sel[i], want_v, want_d)) continue;
        acted++;
        if (dump) { rc |= dump_device(&sel[i]); continue; }
        for (int k = 0; k < nops; k++) rc |= do_op(&sel[i], ops[k]);
    }
    if (!acted) {
        fprintf(stderr, "setpci: no device matched\n");
        return 1;
    }
    return rc;
}
