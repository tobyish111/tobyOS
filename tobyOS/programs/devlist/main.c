/* programs/devlist/main.c -- enumerate kernel-known devices.
 *
 * Usage: devlist [pci|usb|blk|input|audio|battery|hub|all]
 *
 * Default is "all". Output format:
 *
 *   BUS    NAME            DRIVER     STAT  INFO
 *   ---    ----            ------     ----  ----
 *   pci    00:01.1         blk_ata    PB--- 8086:7010 cls=01.01.80 irq=14
 *   ...
 *   PASS: devlist: N device(s)
 *
 * Exit codes (M26A convention):
 *   0  -- at least one device returned
 *   1  -- syscall failed
 *   2  -- bad usage */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <tobyos_devtest.h>

static uint32_t parse_mask(const char *s) {
    if (!s || !*s)            return 0;
    if (!strcmp(s, "all"))     return 0;
    if (!strcmp(s, "pci"))     return ABI_DEVT_BUS_PCI;
    if (!strcmp(s, "usb"))     return ABI_DEVT_BUS_USB;
    if (!strcmp(s, "blk"))     return ABI_DEVT_BUS_BLK;
    if (!strcmp(s, "input"))   return ABI_DEVT_BUS_INPUT;
    if (!strcmp(s, "audio"))   return ABI_DEVT_BUS_AUDIO;
    if (!strcmp(s, "battery")) return ABI_DEVT_BUS_BATTERY;
    if (!strcmp(s, "hub"))     return ABI_DEVT_BUS_HUB;
    if (!strcmp(s, "display")) return ABI_DEVT_BUS_DISPLAY;
    return 0xFFFFFFFFu;        /* sentinel for "unknown" */
}

int main(int argc, char **argv) {
    uint32_t mask = 0;
    if (argc >= 2) {
        mask = parse_mask(argv[1]);
        if (mask == 0xFFFFFFFFu) {
            fprintf(stderr, "FAIL: devlist: unknown bus '%s'\n", argv[1]);
            fprintf(stderr,
                    "usage: devlist [pci|usb|blk|input|audio|battery|hub|display|all]\n");
            return 2;
        }
    }

    static struct abi_dev_info recs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(recs, ABI_DEVT_MAX_DEVICES, mask);
    if (n < 0) {
        fprintf(stderr, "FAIL: devlist: tobydev_list: errno=%d\n", errno);
        return 1;
    }

    tobydev_print_header(stdout);
    for (int i = 0; i < n; i++) {
        tobydev_print_record(stdout, &recs[i]);
    }
    /* Trailing summary -- mandatory PASS/INFO/FAIL token at start of
     * line so test scripts can `grep -E "^(PASS|FAIL|SKIP|INFO):"`. */
    if (n == 0) {
        printf("INFO: devlist: 0 devices\n");
    } else {
        printf("PASS: devlist: %d device(s)\n", n);
    }
    return 0;
}
