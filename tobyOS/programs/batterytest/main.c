/* programs/batterytest/main.c -- ACPI battery validator (M26G).
 *
 * Lists every battery the kernel knows about, runs the kernel-side
 * "battery" selftest, and reports verdicts + descriptive metadata.
 *
 * Discovery sources understood by the kernel:
 *   - DSDT/SSDT byte scan for the PNP0C0A _HID literal (real HW)
 *   - QEMU fw_cfg blob "opt/tobyos/battery_mock" (test harness)
 *
 * Output:
 *   PASS / SKIP / FAIL line for the battery probe self-test
 *   plus one line per battery device with bus / driver / extra info.
 *
 * Exit codes:
 *   0  -- battery probe was either PASS or SKIP (graceful absence
 *         is success per M26G validation rules)
 *   1  -- battery probe FAILed (cached state corrupt, etc.) */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <tobyos_devtest.h>

int main(void) {
    static struct abi_dev_info recs[ABI_DEVT_MAX_DEVICES];
    int n = tobydev_list(recs, ABI_DEVT_MAX_DEVICES, ABI_DEVT_BUS_BATTERY);
    if (n < 0) {
        fprintf(stderr, "FAIL: batterytest list: errno=%d\n", errno);
        return 1;
    }
    if (n == 0) {
        printf("INFO: batterytest: no battery devices reported by kernel\n");
        printf("INFO: batterytest: this is expected on desktops and on "
               "QEMU x86 without -fw_cfg name=opt/tobyos/battery_mock,...\n");
    } else {
        tobydev_print_header(stdout);
        for (int i = 0; i < n; i++) tobydev_print_record(stdout, &recs[i]);
        printf("INFO: batterytest: %d battery device(s) reported\n", n);
    }

    char msg[ABI_DEVT_MSG_MAX];
    int rc = tobydev_test("battery", msg, sizeof msg);
    const char *tag = (rc == 0) ? "PASS" :
                      (rc == ABI_DEVT_SKIP) ? "SKIP" : "FAIL";
    printf("%s: batterytest: %s\n", tag, msg[0] ? msg : "(no message)");

    if (rc == ABI_DEVT_SKIP) {
        printf("INFO: batterytest: kernel reports no battery; harness "
               "treats this as success\n");
        printf("INFO: batterytest: to inject a mock battery in QEMU, add: "
               "-fw_cfg name=opt/tobyos/battery_mock,string=state=charging,"
               "percent=75,design=50000,remaining=37500,rate=1500\n");
    }
    if (rc == 0 && n > 0) {
        printf("INFO: batterytest: real-HW reading of charge requires an "
               "AML interpreter (future work); the heuristic _HID hit and "
               "fw_cfg mock paths are validated here\n");
    }
    return (rc < 0) ? 1 : 0;
}
