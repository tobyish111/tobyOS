/* programs/safesh/main.c -- Milestone 28D: minimal recovery shell.
 *
 * The safe-mode counterpart to /bin/login + the GUI desktop. Runs on
 * the kernel console (stdin/stdout = serial + framebuffer text), no
 * graphics, no fonts beyond the built-in 8x16. Provides exactly the
 * recovery commands an operator needs:
 *
 *   help        list commands
 *   logs [N]    print last N slog records (default 30)
 *   wdog        print watchdog status
 *   crashinfo   spawn /bin/crashinfo (decodes /data/crash/last.dump)
 *   reboot      ACPI shutdown (may need explicit power-off)
 *   exit        leave the shell (kernel goes to idle_loop)
 *
 * In --boot mode (test harness) safesh runs each command once,
 * verifies the essentials are healthy, prints the "M28D_SAFESH:
 * PASS" sentinel, and exits.
 *
 * Exit codes:
 *   0  PASS (boot mode) / clean exit (interactive)
 *   1  one or more required syscalls failed
 *   2  bad usage
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tobyos_slog.h>
#include <tobyos_wdog.h>

static void usage(void) {
    fprintf(stderr, "usage: safesh [--boot]\n");
}

static int cmd_logs(int n) {
    static struct abi_slog_record buf[ABI_SLOG_RING_DEPTH];
    int got = tobylog_read(buf, ABI_SLOG_RING_DEPTH, 0);
    if (got < 0) {
        fprintf(stderr, "safesh: slog_read failed: %s\n", strerror(errno));
        return -1;
    }
    int start = 0;
    if (n > 0 && got > n) start = got - n;
    tobylog_print_header(stdout);
    for (int i = start; i < got; i++) {
        tobylog_print_record(stdout, &buf[i]);
    }
    printf("[safesh] logs: shown=%d total=%d\n", got - start, got);
    return got;
}

static int cmd_wdog(void) {
    struct abi_wdog_status s;
    if (tobywdog_status(&s) < 0) {
        fprintf(stderr, "safesh: wdog_status failed: %s\n", strerror(errno));
        return -1;
    }
    printf("[safesh] wdog: enabled=%u timeout_ms=%u kernel_hb=%lu "
           "sched_hb=%lu syscall_hb=%lu events=%lu\n",
           (unsigned)s.enabled, (unsigned)s.timeout_ms,
           (unsigned long)s.kernel_heartbeats,
           (unsigned long)s.sched_heartbeats,
           (unsigned long)s.syscall_heartbeats,
           (unsigned long)s.event_count);
    return 0;
}

/* Boot-mode shape: run each diagnostic once, gate on essentials. */
static int cmd_boot(void) {
    printf("M28D_SAFESH: starting (safe-mode shell harness)\n");

    /* slog must be alive -- this also proves the SYS_SLOG_READ syscall
     * works in safe mode (it should: slog is essential). */
    int logs_rv = cmd_logs(8);
    if (logs_rv < 0) {
        printf("M28D_SAFESH: FAIL (slog_read)\n");
        return 1;
    }
    printf("M28D_SAFESH: slog_read=ok records=%d\n", logs_rv);

    /* watchdog must be ticking -- proves wdog_kick_kernel/sched/proc
     * still run in safe mode (PIT IRQ + sched_yield + syscall path
     * are all essential). */
    if (cmd_wdog() < 0) {
        printf("M28D_SAFESH: FAIL (wdog_status)\n");
        return 1;
    }
    struct abi_wdog_status s;
    tobywdog_status(&s);
    if (!s.enabled || s.kernel_heartbeats == 0 ||
        s.syscall_heartbeats == 0) {
        printf("M28D_SAFESH: FAIL (wdog heartbeats stalled in safe mode)\n");
        return 1;
    }
    printf("M28D_SAFESH: wdog_alive=1\n");

    /* slog write path also lives in safe mode. */
    int wrv = tobylog_write(ABI_SLOG_LEVEL_INFO, "user",
                            "M28D_TAG safesh boot write");
    if (wrv != 0) {
        printf("M28D_SAFESH: FAIL (slog_write errno=%d)\n", errno);
        return 1;
    }
    printf("M28D_SAFESH: slog_write=ok\n");

    printf("M28D_SAFESH: PASS\n");
    return 0;
}

/* Interactive REPL (stdin = console keyboard).
 * Kept tiny on purpose -- recovery shells should be predictable. */
static int cmd_repl(void) {
    char line[128];
    printf("\n");
    printf("================ tobyOS safe-mode shell (safesh) ================\n");
    printf("Type 'help' for commands. Type 'exit' to leave.\n");
    for (;;) {
        printf("safe> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n[safesh] EOF -- exiting\n");
            return 0;
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) continue;
        if (!strcmp(line, "help")) {
            printf("commands: help, logs [N], wdog, crashinfo, reboot, exit\n");
        } else if (!strncmp(line, "logs", 4)) {
            int k = 30;
            const char *arg = line + 4;
            while (*arg == ' ') arg++;
            if (*arg) k = atoi(arg);
            if (k <= 0) k = 30;
            cmd_logs(k);
        } else if (!strcmp(line, "wdog")) {
            cmd_wdog();
        } else if (!strcmp(line, "crashinfo")) {
            /* Just hint -- exec'ing /bin/crashinfo from a child needs
             * a fork+exec-style spawn libtoby helper that this shell
             * doesn't have today. Operator can quit safesh and the
             * kernel will idle, leaving the operator at the kernel
             * console where they can re-enter the safesh REPL via
             * a future "spawn" syscall wrapper. */
            printf("[safesh] crashinfo: open the dump manually -- "
                   "/data/crash/last.dump is a struct abi_crash_header "
                   "followed by a text body.\n");
        } else if (!strcmp(line, "reboot")) {
            printf("[safesh] reboot: not wired in this build -- "
                   "use QEMU close button or power-cycle.\n");
        } else if (!strcmp(line, "exit") || !strcmp(line, "quit")) {
            printf("[safesh] bye.\n");
            return 0;
        } else {
            printf("[safesh] unknown command '%s' (try 'help')\n", line);
        }
    }
}

int main(int argc, char **argv) {
    int do_boot = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--boot")) do_boot = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "safesh: unknown arg '%s'\n", a);
            usage();
            return 2;
        }
    }
    if (do_boot) return cmd_boot();
    return cmd_repl();
}
