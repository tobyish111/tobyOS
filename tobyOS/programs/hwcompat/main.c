/* programs/hwcompat/main.c -- M35D hardware-compatibility tool.
 *
 * Calls SYS_HWCOMPAT_LIST through the libtoby wrapper and renders the
 * combined PCI+USB compatibility view. The same data is exposed three
 * ways to keep operator UX flexible:
 *
 *   hwcompat                pretty table (default)
 *   hwcompat --json         one-line machine-readable summary
 *   hwcompat --summary      counts only ("supported=N partial=N ...")
 *   hwcompat --boot         M35D harness sentinels (M35D_HWC: ...)
 *
 * Exit codes (mirrors the rest of the M29/M35 tools):
 *   0    snapshot fetched successfully
 *   1    SYS_HWCOMPAT_LIST returned an error
 *   2    bad usage
 *   3    --boot mode could not validate the snapshot
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tobyos_hwinfo.h>

#define HWC_BUF_CAP  ABI_HWCOMPAT_MAX_ENTRIES

static void usage(void) {
    fprintf(stderr,
            "usage: hwcompat [--json|--summary|--boot]\n"
            "  default     pretty PCI+USB compatibility table\n"
            "  --json      one-line machine-readable summary\n"
            "  --summary   counts only (supported/partial/unsupported)\n"
            "  --boot      M35D boot-harness mode (sentinels)\n");
}

/* Tally the snapshot so renderers can show the per-tier counts in
 * any of the three output modes. Walking the array twice (once for
 * the totals, once for the per-row print) keeps output ordering
 * stable while still letting --summary skip the per-row pass. */
static void tally(const struct abi_hwcompat_entry *rows, int n,
                  int *sup, int *par, int *uns) {
    *sup = *par = *uns = 0;
    for (int i = 0; i < n; i++) {
        switch (rows[i].status) {
        case ABI_HWCOMPAT_SUPPORTED:   (*sup)++; break;
        case ABI_HWCOMPAT_PARTIAL:     (*par)++; break;
        case ABI_HWCOMPAT_UNSUPPORTED: (*uns)++; break;
        default:                       break;
        }
    }
}

static void render_pretty(const struct abi_hwcompat_entry *rows, int n) {
    int sup = 0, par = 0, uns = 0;
    tally(rows, n, &sup, &par, &uns);

    printf("tobyOS hardware-compatibility database "
           "(total=%d supported=%d partial=%d unsupported=%d)\n",
           n, sup, par, uns);
    printf("BUS  VID:DID    CLS.SUB.PI  STATUS       BOUND DRIVER         "
           "FRIENDLY\n");
    for (int i = 0; i < n; i++) {
        const struct abi_hwcompat_entry *r = &rows[i];
        printf("%-3s  %04x:%04x  %02x.%02x.%02x   %-12s %-5s %-14s %s\n",
               tobyhw_compat_bus_str(r->bus),
               (unsigned)r->vendor, (unsigned)r->product,
               (unsigned)r->class_code, (unsigned)r->subclass,
               (unsigned)r->prog_if,
               tobyhw_compat_status_str(r->status),
               r->bound ? "yes" : "no",
               r->driver[0] ? r->driver : "(none)",
               r->friendly);
        if (r->reason[0]) {
            printf("                                   reason: %s\n",
                   r->reason);
        }
    }
}

static void render_json(const struct abi_hwcompat_entry *rows, int n) {
    int sup = 0, par = 0, uns = 0;
    tally(rows, n, &sup, &par, &uns);

    printf("{\"total\":%d,\"supported\":%d,\"partial\":%d,"
           "\"unsupported\":%d,\"entries\":[",
           n, sup, par, uns);
    for (int i = 0; i < n; i++) {
        const struct abi_hwcompat_entry *r = &rows[i];
        printf("%s{\"bus\":\"%s\",\"vendor\":%u,\"product\":%u,"
               "\"class\":%u,\"subclass\":%u,\"prog_if\":%u,"
               "\"status\":\"%s\",\"bound\":%u,\"driver\":\"%s\","
               "\"friendly\":\"%s\"}",
               i ? "," : "",
               tobyhw_compat_bus_str(r->bus),
               (unsigned)r->vendor, (unsigned)r->product,
               (unsigned)r->class_code, (unsigned)r->subclass,
               (unsigned)r->prog_if,
               tobyhw_compat_status_str(r->status),
               (unsigned)r->bound,
               r->driver,
               r->friendly);
    }
    printf("]}\n");
}

static void render_summary(const struct abi_hwcompat_entry *rows, int n) {
    int sup = 0, par = 0, uns = 0;
    tally(rows, n, &sup, &par, &uns);
    printf("hwcompat: total=%d supported=%d partial=%d unsupported=%d\n",
           n, sup, par, uns);
}

/* M35D boot harness. Spits stable sentinels test_m35.ps1 (m35d phase)
 * can pattern-match on, and emits PASS only when:
 *   - SYS_HWCOMPAT_LIST succeeded
 *   - at least one PCI row exists (PCI is mandatory for the QEMU
 *     machine type we boot)
 *   - every row has a non-empty friendly name
 *   - the per-bus counters match the listed rows */
static int do_boot(void) {
    struct abi_hwcompat_entry rows[HWC_BUF_CAP];
    int n = tobyhw_compat_list(rows, HWC_BUF_CAP, 0);
    if (n < 0) {
        printf("M35D_HWC: FAIL: tobyhw_compat_list errno=%d\n", errno);
        return 1;
    }
    int sup = 0, par = 0, uns = 0;
    tally(rows, n, &sup, &par, &uns);

    int pci_n = 0, usb_n = 0, blank_friendly = 0;
    int pci_bound = 0, usb_bound = 0, unknown_status = 0;
    for (int i = 0; i < n; i++) {
        const struct abi_hwcompat_entry *r = &rows[i];
        if (r->bus == ABI_DEVT_BUS_PCI) {
            pci_n++;
            if (r->bound) pci_bound++;
        } else if (r->bus == ABI_DEVT_BUS_USB) {
            usb_n++;
            if (r->bound) usb_bound++;
        }
        if (!r->friendly[0]) blank_friendly++;
        if (r->status == ABI_HWCOMPAT_UNKNOWN) unknown_status++;
    }

    printf("M35D_HWC: total=%d pci=%d usb=%d "
           "supported=%d partial=%d unsupported=%d\n",
           n, pci_n, usb_n, sup, par, uns);
    printf("M35D_HWC: bound pci=%d usb=%d blank_friendly=%d "
           "unknown_status=%d\n",
           pci_bound, usb_bound, blank_friendly, unknown_status);

    int ok = (pci_n > 0)               /* QEMU always has a PCI host */
          && (pci_bound > 0)           /* at least one PCI driver bound */
          && (blank_friendly == 0)     /* every row has a name */
          && (unknown_status == 0)     /* status field always populated */
          && (sup + par + uns == n);   /* tally must round-trip */
    if (ok) {
        printf("M35D_HWC: PASS\n");
        return 0;
    }
    printf("M35D_HWC: FAIL\n");
    return 3;
}

int main(int argc, char **argv) {
    int do_json = 0, do_sum = 0, do_boot_mode = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--json"))    do_json      = 1;
        else if (!strcmp(argv[i], "--summary")) do_sum       = 1;
        else if (!strcmp(argv[i], "--boot"))    do_boot_mode = 1;
        else if (!strcmp(argv[i], "--help") ||
                 !strcmp(argv[i], "-h")) { usage(); return 0; }
        else {
            fprintf(stderr, "FAIL: hwcompat: unknown arg '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (do_boot_mode) return do_boot();

    struct abi_hwcompat_entry rows[HWC_BUF_CAP];
    int n = tobyhw_compat_list(rows, HWC_BUF_CAP, 0);
    if (n < 0) {
        fprintf(stderr,
                "FAIL: hwcompat: tobyhw_compat_list errno=%d\n", errno);
        return 1;
    }

    if (do_json) {
        render_json(rows, n);
    } else if (do_sum) {
        render_summary(rows, n);
    } else {
        render_pretty(rows, n);
    }
    return 0;
}
