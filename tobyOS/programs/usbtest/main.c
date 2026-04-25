/* programs/usbtest/main.c -- USB-focused validator (M26A..M26E).
 *
 * Subcommands:
 *   usbtest list       -- list USB devices via SYS_DEV_LIST(USB)
 *   usbtest controller -- run xhci self-test
 *   usbtest devices    -- run usb (device list) self-test
 *   usbtest hub        -- M26B: list hubs + run hub self-test +
 *                                 list devices behind hubs (depth>0)
 *   usbtest hotplug    -- M26C: drain hot-plug ring + ring round-trip
 *                                 self-test ("hotplug")
 *   usbtest hid        -- M26D: list INPUT bus + run input + usb_hid
 *                                 self-tests + dump per-device counters
 *   usbtest storage    -- M26E: list BLK bus + usb_msc self-test +
 *                                 optional FAT32 round-trip in /usb
 *   usbtest all        -- everything above
 *
 * Each subcommand prints a single trailing PASS/FAIL/SKIP/INFO line. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <tobyos_devtest.h>

static int do_list(void) {
    static struct abi_dev_info recs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(recs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_USB);
    if (n < 0) {
        fprintf(stderr, "FAIL: usbtest list: errno=%d\n", errno);
        return 1;
    }
    if (n == 0) {
        printf("INFO: usbtest list: no USB devices\n");
        return 0;
    }
    tobydev_print_header(stdout);
    for (int i = 0; i < n; i++) tobydev_print_record(stdout, &recs[i]);
    printf("PASS: usbtest list: %d USB device(s)\n", n);
    return 0;
}

static int run_test(const char *name, const char *label) {
    char msg[ABI_DEVT_MSG_MAX];
    int rc = tobydev_test(name, msg, sizeof msg);
    const char *tag = (rc == 0) ? "PASS" :
                      (rc == ABI_DEVT_SKIP) ? "SKIP" : "FAIL";
    printf("%s: usbtest %s: %s\n", tag, label, msg[0] ? msg : "(no message)");
    return (rc < 0) ? 1 : 0;
}

/* M26B: hub-focused subcommand. Sequence:
 *   1. list ABI_DEVT_BUS_HUB devices (the hubs themselves)
 *   2. list ABI_DEVT_BUS_USB devices, splitting into root vs behind-hub
 *   3. run drvtest "usb_hub" -- aggregate self-test from the kernel
 * Returns 0 on PASS or SKIP (the hub class driver is allowed to skip
 * cleanly when no hub is plugged in), 1 on hard failure. */
static int do_hub(void) {
    static struct abi_dev_info hubs[ABI_DEVT_MAX_DEVICES];
    int nh = tobydev_list(hubs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_HUB);
    if (nh < 0) {
        fprintf(stderr, "FAIL: usbtest hub: list errno=%d\n", errno);
        return 1;
    }

    if (nh > 0) {
        printf("INFO: usbtest hub: %d hub(s) registered\n", nh);
        tobydev_print_header(stdout);
        for (int i = 0; i < nh; i++) tobydev_print_record(stdout, &hubs[i]);
    } else {
        printf("INFO: usbtest hub: no USB hub registered\n");
    }

    /* Split the USB device list into root vs behind-hub. */
    static struct abi_dev_info devs[ABI_DEVT_MAX_DEVICES];
    int nu = tobydev_list(devs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_USB);
    if (nu < 0) {
        fprintf(stderr, "FAIL: usbtest hub: usb list errno=%d\n", errno);
        return 1;
    }
    int root = 0, behind = 0;
    for (int i = 0; i < nu; i++) {
        if (devs[i].hub_depth > 0) behind++;
        else                        root++;
    }
    printf("INFO: usbtest hub: %d USB dev(s) total: %d root, %d behind hub\n",
           nu, root, behind);
    if (behind > 0) {
        printf("INFO: usbtest hub: enumerating downstream devices --\n");
        tobydev_print_header(stdout);
        for (int i = 0; i < nu; i++) {
            if (devs[i].hub_depth > 0) tobydev_print_record(stdout, &devs[i]);
        }
    }

    /* Hub class driver self-test. SKIP is OK: graceful "no hub". */
    char msg[ABI_DEVT_MSG_MAX];
    int rc = tobydev_test("usb_hub", msg, sizeof msg);
    const char *tag = (rc == 0) ? "PASS" :
                      (rc == ABI_DEVT_SKIP) ? "SKIP" : "FAIL";
    printf("%s: usbtest hub: %s\n", tag, msg[0] ? msg : "(no message)");
    return (rc < 0) ? 1 : 0;
}

/* M26C: hot-plug subcommand.
 *
 * Two complementary checks:
 *
 *   (1) Run the kernel-side ring round-trip self-test (registered
 *       as "hotplug" in src/devtest.c). It synthesizes one ATTACH +
 *       one DETACH event and verifies they survive a drain. PASS
 *       proves the producer/consumer plumbing works.
 *
 *   (2) Drain whatever real hot-plug events have piled up since the
 *       last drain (boot enumeration, QMP device_add/del, hub-port
 *       changes). 0 events is fine; we print INFO not FAIL because
 *       the QEMU automation may run this BEFORE issuing device_add.
 *
 * Exits 0 on PASS or SKIP, 1 on hard FAIL of (1). */
static int do_hotplug(void) {
    /* Step (1): synthetic ring round-trip. */
    char msg[ABI_DEVT_MSG_MAX];
    int rc = tobydev_test("hotplug", msg, sizeof msg);
    const char *tag = (rc == 0) ? "PASS" :
                      (rc == ABI_DEVT_SKIP) ? "SKIP" : "FAIL";
    printf("%s: usbtest hotplug ring: %s\n",
           tag, msg[0] ? msg : "(no message)");

    /* Step (2): drain everything that's pending. The selftest above
     * left two synthetic events ahead of any real ones, so we expect
     * the first drain to include those plus whatever the kernel saw
     * between boot and now. */
    static struct abi_hot_event evs[ABI_DEVT_HOT_RING];
    int n = tobydev_hot_drain(evs, ABI_DEVT_HOT_RING);
    if (n < 0) {
        fprintf(stderr,
                "FAIL: usbtest hotplug drain: errno=%d\n", errno);
        return 1;
    }
    if (n == 0) {
        printf("INFO: usbtest hotplug drain: no live events queued\n");
    } else {
        printf("INFO: usbtest hotplug drain: %d event(s)\n", n);
        for (int i = 0; i < n; i++) {
            tobydev_print_hot_event(stdout, &evs[i]);
        }
    }
    return (rc < 0) ? 1 : 0;
}

/* M26D: HID-focused subcommand. Sequence:
 *   1. list ABI_DEVT_BUS_INPUT records (PS/2 + USB HID)
 *   2. run drvtest "input" -- always-PASS counter snapshot
 *   3. run drvtest "usb_hid" -- SKIP cleanly if no USB HID present
 * Exit 0 on PASS or SKIP; 1 on hard failure. */
static int do_hid(void) {
    static struct abi_dev_info recs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(recs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_INPUT);
    if (n < 0) {
        fprintf(stderr, "FAIL: usbtest hid: list errno=%d\n", errno);
        return 1;
    }
    int ps2 = 0, usbhid = 0;
    for (int i = 0; i < n; i++) {
        if (!strncmp(recs[i].driver, "ps2_", 4)) ps2++;
        else if (!strncmp(recs[i].driver, "usb_hid", 7)) usbhid++;
    }
    printf("INFO: usbtest hid: %d INPUT dev(s): %d ps2 + %d usb_hid\n",
           n, ps2, usbhid);
    if (n > 0) {
        tobydev_print_header(stdout);
        for (int i = 0; i < n; i++) tobydev_print_record(stdout, &recs[i]);
    }

    char msg[ABI_DEVT_MSG_MAX];
    int rc_in = tobydev_test("input", msg, sizeof msg);
    const char *tag_in = (rc_in == 0) ? "PASS" :
                         (rc_in == ABI_DEVT_SKIP) ? "SKIP" : "FAIL";
    printf("%s: usbtest hid input: %s\n",
           tag_in, msg[0] ? msg : "(no message)");

    int rc_h = tobydev_test("usb_hid", msg, sizeof msg);
    const char *tag_h = (rc_h == 0) ? "PASS" :
                        (rc_h == ABI_DEVT_SKIP) ? "SKIP" : "FAIL";
    printf("%s: usbtest hid usb_hid: %s\n",
           tag_h, msg[0] ? msg : "(no message)");

    /* Hard failure: any test returns < 0. Both SKIP + PASS exit 0. */
    if (rc_in < 0 || rc_h < 0) return 1;
    return 0;
}

/* M26E: storage-focused subcommand. Sequence:
 *
 *   1. List the BLK bus and pick out USB-MSC disks vs partitions.
 *   2. Detect which (if any) of those are currently mounted (the
 *      kernel sets ABI_DEVT_ACTIVE on the BLK record + appends
 *      "mount=<path>" to extra).
 *   3. Run drvtest "usb_msc" -- SKIP cleanly when no usb-storage
 *      device is plugged.
 *   4. If a usb disk is mounted at /usb (the kernel boot mount),
 *      perform a non-destructive RW round-trip on a temp file:
 *      create -> write known pattern -> read back -> compare ->
 *      delete. The selftest treats this round-trip as best-effort
 *      INFO; the real automated regression script (test_m26e.ps1)
 *      drives a bigger mount/umount/yank scenario from outside.
 *
 * Exit 0 on PASS or SKIP, 1 on hard failure. */
static int do_storage(void) {
    static struct abi_dev_info recs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(recs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_BLK);
    if (n < 0) {
        fprintf(stderr, "FAIL: usbtest storage: list errno=%d\n", errno);
        return 1;
    }

    int disks = 0, parts = 0, gone = 0, mounted = 0;
    char first_mount[64] = {0};
    for (int i = 0; i < n; i++) {
        const struct abi_dev_info *r = &recs[i];
        bool is_part = (strncmp(r->driver, "partition", 9) == 0);
        bool is_disk = (strncmp(r->driver, "disk", 4) == 0);
        if (is_part)  parts++;
        if (is_disk)  disks++;
        if (!(r->status & ABI_DEVT_BOUND)) gone++;
        const char *mtag = strstr(r->extra, "mount=");
        if (mtag) {
            mounted++;
            if (!first_mount[0]) {
                snprintf(first_mount, sizeof first_mount, "%s", mtag + 6);
                /* Trim at first space so trailing extra fields drop. */
                for (char *p = first_mount; *p; p++) {
                    if (*p == ' ' || *p == '\n') { *p = '\0'; break; }
                }
            }
        }
    }
    printf("INFO: usbtest storage: %d BLK dev(s): %d disk + %d part, "
           "%d gone, %d mounted\n", n, disks, parts, gone, mounted);
    if (n > 0) {
        tobydev_print_header(stdout);
        for (int i = 0; i < n; i++) tobydev_print_record(stdout, &recs[i]);
    }

    char msg[ABI_DEVT_MSG_MAX];
    int rc = tobydev_test("usb_msc", msg, sizeof msg);
    const char *tag = (rc == 0) ? "PASS" :
                      (rc == ABI_DEVT_SKIP) ? "SKIP" : "FAIL";
    printf("%s: usbtest storage: %s\n", tag, msg[0] ? msg : "(no message)");

    /* M26E: optional FAT32 RW round-trip on the live mount. We pick
     * the FIRST mount we see -- in QEMU this is /usb. We deliberately
     * use a unique filename so re-runs don't collide. */
    if (mounted > 0 && first_mount[0]) {
        char path[128];
        snprintf(path, sizeof path, "%s/usbtest.tmp", first_mount);
        const char pattern[] = "tobyOS-M26E-storage-roundtrip\n";
        const size_t patlen  = sizeof(pattern) - 1;

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            printf("INFO: usbtest storage: skip RW (open '%s' errno=%d)\n",
                   path, errno);
        } else {
            ssize_t w = write(fd, pattern, patlen);
            close(fd);
            if (w != (ssize_t)patlen) {
                printf("FAIL: usbtest storage: write returned %ld (want %lu)\n",
                       (long)w, (unsigned long)patlen);
                unlink(path);
                return 1;
            }
            int fd2 = open(path, O_RDONLY);
            if (fd2 < 0) {
                printf("FAIL: usbtest storage: reopen for read errno=%d\n",
                       errno);
                unlink(path);
                return 1;
            }
            char buf[64] = {0};
            ssize_t rb = read(fd2, buf, sizeof buf - 1);
            close(fd2);
            unlink(path);
            if (rb != (ssize_t)patlen || memcmp(buf, pattern, patlen) != 0) {
                printf("FAIL: usbtest storage: roundtrip mismatch "
                       "(read %ld bytes)\n", (long)rb);
                return 1;
            }
            printf("PASS: usbtest storage: FAT32 roundtrip on '%s' "
                   "(%lu bytes)\n", first_mount, (unsigned long)patlen);
        }
    } else {
        printf("INFO: usbtest storage: no live USB mount -- skipping "
               "FAT32 roundtrip\n");
    }

    return (rc < 0) ? 1 : 0;
}

static const char *g_unimplemented[] = {
    NULL
};

static int is_future_phase(const char *cmd) {
    for (int i = 0; g_unimplemented[i]; i++) {
        if (!strcmp(cmd, g_unimplemented[i])) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = (argc >= 2) ? argv[1] : "all";

    if (!strcmp(cmd, "list"))       return do_list();
    if (!strcmp(cmd, "controller")) return run_test("xhci", "controller");
    if (!strcmp(cmd, "devices"))    return run_test("usb",  "devices");
    if (!strcmp(cmd, "hub"))        return do_hub();
    if (!strcmp(cmd, "hotplug"))    return do_hotplug();
    if (!strcmp(cmd, "hid"))        return do_hid();
    if (!strcmp(cmd, "storage"))    return do_storage();

    if (!strcmp(cmd, "all")) {
        int rc = 0;
        rc |= do_list();
        rc |= run_test("xhci", "controller");
        rc |= run_test("usb",  "devices");
        rc |= do_hub();
        rc |= do_hotplug();
        rc |= do_hid();
        rc |= do_storage();
        return rc;
    }

    if (is_future_phase(cmd)) {
        /* Document the phase rather than fail -- userland tests can
         * re-run this program in later milestones without changing
         * their invocation. */
        printf("SKIP: usbtest %s: feature lands in a later M26 phase\n", cmd);
        return 0;
    }

    fprintf(stderr,
            "FAIL: usbtest: unknown subcommand '%s'\n"
            "usage: usbtest [list|controller|devices|hub|hotplug|hid|storage|all]\n",
            cmd);
    return 2;
}
