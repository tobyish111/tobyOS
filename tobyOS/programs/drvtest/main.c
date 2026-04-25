/* programs/drvtest/main.c -- driver self-test runner (M26A).
 *
 * Usage:
 *   drvtest               -- run every built-in test
 *   drvtest <name> [...]  -- run only the listed tests
 *
 * Built-in test names (registered by the kernel devtest module):
 *   devtest    -- registry smoke check
 *   pci        -- PCI enumeration sanity
 *   blk        -- block-device registry summary
 *   xhci       -- xHCI controller health
 *   usb        -- USB device enumeration
 *   usb_hub    -- USB hub class driver (M26B)
 *   audio      -- HD Audio controller probe
 *   battery    -- ACPI battery
 *
 * Output format:
 *   [PASS] xhci: xHCI v0.10 slots=8 ports=4 irq=on/12
 *   [SKIP] battery: no ACPI battery present (desktop or QEMU default)
 *   ...
 *   PASS: drvtest: pass=N skip=M fail=K (total=T)
 *
 * Exit codes:
 *   0  -- no failures (skips are OK)
 *   1  -- at least one FAIL
 *   2  -- bad usage / kernel call error */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <tobyos_devtest.h>

/* The "run everything" mode iterates a fixed list. We don't have a
 * SYS_DEV_TEST_LIST yet (would be M26 follow-up); the kernel-side
 * devtest_for_each is internal-only. So the userland tool ships with
 * the same canonical list the kernel registers in devtest_init().
 * If the kernel adds a new test, also add it here -- the test will
 * just SKIP cleanly if the underlying subsystem is not present. */
static const char *g_default_tests[] = {
    "devtest", "pci", "blk", "xhci", "usb", "usb_hub", "audio", "battery",
};

static int run_one(const char *name, int *pass, int *fail, int *skip) {
    char msg[ABI_DEVT_MSG_MAX];
    int rc = tobydev_test(name, msg, sizeof msg);
    if (rc == 0) {
        printf("[PASS] %s: %s\n", name, msg[0] ? msg : "(ok)");
        (*pass)++;
        return 0;
    }
    if (rc == ABI_DEVT_SKIP) {
        printf("[SKIP] %s: %s\n", name, msg[0] ? msg : "(skipped)");
        (*skip)++;
        return 0;
    }
    /* rc == -1, errno set */
    printf("[FAIL] %s: errno=%d %s\n", name, errno,
           msg[0] ? msg : "(no message)");
    (*fail)++;
    return 1;
}

int main(int argc, char **argv) {
    int pass = 0, fail = 0, skip = 0;

    if (argc <= 1) {
        size_t n = sizeof(g_default_tests) / sizeof(g_default_tests[0]);
        for (size_t i = 0; i < n; i++) {
            run_one(g_default_tests[i], &pass, &fail, &skip);
        }
    } else {
        for (int i = 1; i < argc; i++) {
            run_one(argv[i], &pass, &fail, &skip);
        }
    }

    int total = pass + fail + skip;
    const char *tag = (fail == 0) ? "PASS" : "FAIL";
    printf("%s: drvtest: pass=%d skip=%d fail=%d (total=%d)\n",
           tag, pass, skip, fail, total);
    return fail ? 1 : 0;
}
