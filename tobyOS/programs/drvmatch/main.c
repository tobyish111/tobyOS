/* programs/drvmatch/main.c -- M29B driver matching + fallback tool.
 *
 * Calls SYS_DRVMATCH via the libtoby wrapper. Two main modes:
 *
 *   drvmatch              list every PCI + USB device the kernel
 *                         knows about, with the strategy that bound
 *                         (or didn't bind) a driver to it.
 *
 *   drvmatch <bus> <vendor>:<device>
 *                         single-device query. <bus> is "pci" or
 *                         "usb"; vendor/device are 4-digit hex.
 *
 *   drvmatch --boot       boot-harness sentinel mode for
 *                         test_m29b.ps1. Walks every device in the
 *                         current snapshot, queries SYS_DRVMATCH for
 *                         each, then probes a deliberately-bogus
 *                         (vendor=0xDEAD, device=0xBEEF) PCI key to
 *                         confirm the unsupported-device path
 *                         returns NONE without crashing.
 *
 * Exit codes:
 *   0  success / boot mode passed
 *   1  SYS_DRVMATCH (or SYS_HWINFO) errored on a known device
 *   2  bad usage
 *   3  --boot mode found a malformed record
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <tobyos_hwinfo.h>
#include <tobyos_devtest.h>

static void usage(void) {
    fprintf(stderr,
            "usage: drvmatch [--boot] [pci|usb VENDOR:DEVICE]\n"
            "  default     list all known devices and their match strategy\n"
            "  --boot      boot-harness self-check (M29B sentinels)\n"
            "  pci VID:DID single-device query, hex (e.g. pci 8086:100e)\n"
            "  usb VID:PID single-USB-device query\n");
}

static const char *bus_name(uint32_t bus) {
    switch (bus) {
    case ABI_DEVT_BUS_PCI: return "pci";
    case ABI_DEVT_BUS_USB: return "usb";
    case ABI_DEVT_BUS_BLK: return "blk";
    case ABI_DEVT_BUS_INPUT: return "input";
    case ABI_DEVT_BUS_AUDIO: return "audio";
    case ABI_DEVT_BUS_BATTERY: return "bat";
    case ABI_DEVT_BUS_HUB: return "hub";
    case ABI_DEVT_BUS_DISPLAY: return "fb";
    default: return "?";
    }
}

static void render_record(const struct abi_drvmatch_info *r) {
    printf("  %-3s  %04x:%04x  cls=%02x.%02x  drv=%-12s strat=%-11s "
           "bound=%u\n",
           bus_name(r->bus),
           (unsigned)r->vendor, (unsigned)r->device,
           (unsigned)r->class_code, (unsigned)r->subclass,
           r->driver, tobyhw_strategy_str(r->strategy),
           (unsigned)r->bound);
    if (r->reason[0]) {
        printf("       reason: %s\n", r->reason);
    }
}

/* List every device returned by SYS_DEV_LIST and query drvmatch
 * for each. We only emit records for PCI/USB buses since those
 * are the ones with a meaningful driver-match concept. */
static int list_all(int verbose) {
    struct abi_dev_info devs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(devs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_ALL);
    if (n < 0) {
        fprintf(stderr, "FAIL: drvmatch: tobydev_list errno=%d\n", errno);
        return 1;
    }
    int errs = 0;
    int seen = 0;
    for (int i = 0; i < n; i++) {
        const struct abi_dev_info *d = &devs[i];
        if (d->bus != ABI_DEVT_BUS_PCI && d->bus != ABI_DEVT_BUS_USB) {
            continue;
        }
        struct abi_drvmatch_info r;
        long rc = tobyhw_drvmatch(d->bus, d->vendor, d->device, &r);
        if (rc != 0 && errno != ENOENT) {
            fprintf(stderr,
                    "FAIL: drvmatch %s %04x:%04x errno=%d\n",
                    bus_name(d->bus),
                    (unsigned)d->vendor,
                    (unsigned)d->device,
                    errno);
            errs++;
            continue;
        }
        seen++;
        if (verbose || rc != 0 ||
            r.strategy == ABI_DRVMATCH_NONE ||
            r.strategy == ABI_DRVMATCH_FORCED_OFF) {
            render_record(&r);
        } else {
            render_record(&r);
        }
    }
    printf("drvmatch: total queried=%d errors=%d\n", seen, errs);
    return errs ? 1 : 0;
}

/* Boot-harness self-check used by test_m29b.ps1.
 *
 * Walks every PCI/USB device in the current snapshot, queries
 * SYS_DRVMATCH, asserts each record is well-formed, then issues a
 * bogus (DEAD:BEEF) PCI query to confirm the unsupported-device
 * path returns NONE without crashing. */
static int do_boot(void) {
    struct abi_dev_info devs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(devs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_ALL);
    if (n < 0) {
        printf("M29B_DRV: FAIL: tobydev_list errno=%d\n", errno);
        return 3;
    }
    int pci_total = 0, usb_total = 0;
    int pci_bound = 0, usb_bound = 0;
    int pci_none  = 0, usb_none  = 0;
    int pci_class = 0, pci_exact = 0, pci_generic = 0;
    int malformed = 0;
    for (int i = 0; i < n; i++) {
        const struct abi_dev_info *d = &devs[i];
        if (d->bus != ABI_DEVT_BUS_PCI && d->bus != ABI_DEVT_BUS_USB) continue;
        uint32_t v = d->vendor;
        uint32_t p = d->device;
        struct abi_drvmatch_info r;
        long rc = tobyhw_drvmatch(d->bus, v, p, &r);
        if (rc != 0) {
            printf("M29B_DRV: FAIL: known device %s %04x:%04x missing "
                   "(rc=%ld errno=%d)\n",
                   bus_name(d->bus), (unsigned)v, (unsigned)p, rc, errno);
            malformed++;
            continue;
        }
        if (r.bus != d->bus) {
            printf("M29B_DRV: FAIL: bus mismatch on %04x:%04x "
                   "(expected %u got %u)\n",
                   (unsigned)v, (unsigned)p,
                   (unsigned)d->bus, (unsigned)r.bus);
            malformed++;
            continue;
        }
        if (d->bus == ABI_DEVT_BUS_PCI) {
            pci_total++;
            if (r.bound) pci_bound++;
            switch (r.strategy) {
            case ABI_DRVMATCH_NONE:    pci_none++;    break;
            case ABI_DRVMATCH_EXACT:   pci_exact++;   break;
            case ABI_DRVMATCH_CLASS:   pci_class++;   break;
            case ABI_DRVMATCH_GENERIC: pci_generic++; break;
            }
        } else {
            usb_total++;
            if (r.bound) usb_bound++;
            if (r.strategy == ABI_DRVMATCH_NONE) usb_none++;
        }
    }

    /* Probe a deliberately-bogus PCI ID. The kernel must NOT crash
     * and must return ENOENT with strategy=NONE. */
    struct abi_drvmatch_info bogus;
    long brc = tobyhw_drvmatch(ABI_DEVT_BUS_PCI, 0xDEAD, 0xBEEF, &bogus);
    int bogus_ok = (brc != 0 && errno == ENOENT &&
                    bogus.strategy == ABI_DRVMATCH_NONE);
    printf("M29B_DRV: bogus probe pci dead:beef rc=%ld errno=%d "
           "strat=%s -> %s\n",
           brc, errno, tobyhw_strategy_str(bogus.strategy),
           bogus_ok ? "PASS" : "FAIL");

    /* Probe a deliberately-bogus USB ID for symmetry. */
    long brc2 = tobyhw_drvmatch(ABI_DEVT_BUS_USB, 0xDEAD, 0xBEEF, &bogus);
    int bogus_ok2 = (brc2 != 0 && errno == ENOENT &&
                     bogus.strategy == ABI_DRVMATCH_NONE);
    printf("M29B_DRV: bogus probe usb dead:beef rc=%ld errno=%d "
           "strat=%s -> %s\n",
           brc2, errno, tobyhw_strategy_str(bogus.strategy),
           bogus_ok2 ? "PASS" : "FAIL");

    /* Probe a deliberately-bogus BUS tag. The kernel must reject
     * with EINVAL and the record must stay zeroed. */
    long brc3 = tobyhw_drvmatch(0xFFu, 0xDEAD, 0xBEEF, &bogus);
    int bogus_ok3 = (brc3 != 0 && errno == EINVAL);
    printf("M29B_DRV: bogus probe bad-bus rc=%ld errno=%d -> %s\n",
           brc3, errno, bogus_ok3 ? "PASS" : "FAIL");

    printf("M29B_DRV: pci total=%d bound=%d none=%d "
           "exact=%d class=%d generic=%d\n",
           pci_total, pci_bound, pci_none,
           pci_exact, pci_class, pci_generic);
    printf("M29B_DRV: usb total=%d bound=%d none=%d\n",
           usb_total, usb_bound, usb_none);
    printf("M29B_DRV: malformed=%d\n", malformed);

    int ok = (malformed == 0) && bogus_ok && bogus_ok2 && bogus_ok3 &&
             (pci_total > 0) && (pci_bound > 0);
    if (ok) {
        printf("M29B_DRV: PASS\n");
        return 0;
    }
    printf("M29B_DRV: FAIL: malformed=%d bogus_pci_ok=%d "
           "bogus_usb_ok=%d bogus_bus_ok=%d pci_total=%d pci_bound=%d\n",
           malformed, bogus_ok, bogus_ok2, bogus_ok3,
           pci_total, pci_bound);
    return 3;
}

/* Parse "VVVV:DDDD" into two uint32 values. Returns 0 on success. */
static int parse_id_pair(const char *s, uint32_t *v, uint32_t *d) {
    if (!s || !v || !d) return -1;
    char *end = 0;
    unsigned long lv = strtoul(s, &end, 16);
    if (!end || *end != ':') return -1;
    unsigned long ld = strtoul(end + 1, &end, 16);
    if (!end || (*end && *end != '\n')) return -1;
    *v = (uint32_t)lv;
    *d = (uint32_t)ld;
    return 0;
}

int main(int argc, char **argv) {
    if (argc <= 1) {
        return list_all(0);
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage();
        return 0;
    }
    if (!strcmp(argv[1], "--boot")) {
        return do_boot();
    }
    if (!strcmp(argv[1], "--list") || !strcmp(argv[1], "list")) {
        return list_all(1);
    }
    if (argc == 3 && (!strcmp(argv[1], "pci") || !strcmp(argv[1], "usb"))) {
        uint32_t bus = !strcmp(argv[1], "pci") ? ABI_DEVT_BUS_PCI
                                               : ABI_DEVT_BUS_USB;
        uint32_t v = 0, d = 0;
        if (parse_id_pair(argv[2], &v, &d) != 0) {
            fprintf(stderr,
                    "FAIL: drvmatch: bad VID:DID '%s'\n", argv[2]);
            usage();
            return 2;
        }
        struct abi_drvmatch_info r;
        long rc = tobyhw_drvmatch(bus, v, d, &r);
        render_record(&r);
        if (rc != 0) {
            printf("drvmatch: rc=%ld errno=%d\n", rc, errno);
            return 1;
        }
        return 0;
    }
    fprintf(stderr, "FAIL: drvmatch: unknown arg '%s'\n", argv[1]);
    usage();
    return 2;
}
