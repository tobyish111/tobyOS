/* programs/wdogtest/main.c -- Milestone 28C: watchdog inspector.
 *
 * Reads the kernel watchdog state via SYS_WDOG_STATUS and emits a
 * compact, grep-able summary. Modes:
 *
 *   wdogtest               -- pretty-print the current snapshot
 *   wdogtest --json        -- machine-readable, single line of JSON
 *   wdogtest --boot        -- harness mode: validates that the kernel
 *                             saw at least one bite event, prints a
 *                             stable "M28C_WDOG: PASS" sentinel for
 *                             test_m28c.ps1 to grep
 *
 * Exit codes:
 *   0  PASS
 *   1  syscall failed (errno set)
 *   2  bad usage
 *   3  --boot saw zero bite events (kernel never detected the stall)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tobyos_wdog.h>

static void usage(void) {
    fprintf(stderr,
            "usage: wdogtest [--json] [--boot]\n");
}

static int read_status(struct abi_wdog_status *out) {
    int rv = tobywdog_status(out);
    if (rv < 0) {
        fprintf(stderr, "wdogtest: SYS_WDOG_STATUS failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

static void print_pretty(const struct abi_wdog_status *s) {
    printf("watchdog status:\n");
    printf("  enabled            = %u\n", (unsigned)s->enabled);
    printf("  timeout_ms         = %u\n", (unsigned)s->timeout_ms);
    printf("  kernel_heartbeats  = %lu\n", (unsigned long)s->kernel_heartbeats);
    printf("  sched_heartbeats   = %lu\n", (unsigned long)s->sched_heartbeats);
    printf("  syscall_heartbeats = %lu\n", (unsigned long)s->syscall_heartbeats);
    printf("  ms_since_kernel    = %lu\n", (unsigned long)s->ms_since_kernel_kick);
    printf("  ms_since_sched     = %lu\n", (unsigned long)s->ms_since_sched_kick);
    printf("  event_count        = %lu\n", (unsigned long)s->event_count);
    printf("  last_event_ms      = %lu\n", (unsigned long)s->last_event_ms);
    printf("  last_event_kind    = %s (%u)\n",
           tobywdog_kind_str(s->last_event_kind),
           (unsigned)s->last_event_kind);
    printf("  last_event_pid     = %d\n", (int)s->last_event_pid);
    /* Reason buffer is fixed-size, may not be NUL-terminated. */
    char reason[ABI_WDOG_REASON_MAX + 1];
    memcpy(reason, s->last_event_reason, ABI_WDOG_REASON_MAX);
    reason[ABI_WDOG_REASON_MAX] = '\0';
    printf("  last_event_reason  = \"%s\"\n", reason);
}

static void print_json(const struct abi_wdog_status *s) {
    char reason[ABI_WDOG_REASON_MAX + 1];
    memcpy(reason, s->last_event_reason, ABI_WDOG_REASON_MAX);
    reason[ABI_WDOG_REASON_MAX] = '\0';
    /* Cheap escape: replace " with ' for one-line JSON. */
    for (char *p = reason; *p; p++) if (*p == '"') *p = '\'';
    printf("{\"enabled\":%u,\"timeout_ms\":%u,"
           "\"kernel_hb\":%lu,\"sched_hb\":%lu,\"syscall_hb\":%lu,"
           "\"ms_since_kernel\":%lu,\"ms_since_sched\":%lu,"
           "\"events\":%lu,\"last_ms\":%lu,"
           "\"last_kind\":\"%s\",\"last_pid\":%d,"
           "\"last_reason\":\"%s\"}\n",
           (unsigned)s->enabled, (unsigned)s->timeout_ms,
           (unsigned long)s->kernel_heartbeats,
           (unsigned long)s->sched_heartbeats,
           (unsigned long)s->syscall_heartbeats,
           (unsigned long)s->ms_since_kernel_kick,
           (unsigned long)s->ms_since_sched_kick,
           (unsigned long)s->event_count,
           (unsigned long)s->last_event_ms,
           tobywdog_kind_str(s->last_event_kind),
           (int)s->last_event_pid,
           reason);
}

static int cmd_boot(void) {
    struct abi_wdog_status s;
    if (read_status(&s) < 0) {
        printf("M28C_WDOG: FAIL syscall errno=%d\n", errno);
        return 1;
    }
    /* Always emit the same sentinel block so the harness can grep
     * unconditionally. */
    char reason[ABI_WDOG_REASON_MAX + 1];
    memcpy(reason, s.last_event_reason, ABI_WDOG_REASON_MAX);
    reason[ABI_WDOG_REASON_MAX] = '\0';
    printf("M28C_WDOG: enabled=%u timeout_ms=%u kernel_hb=%lu sched_hb=%lu "
           "syscall_hb=%lu events=%lu last_kind=%s last_pid=%d\n",
           (unsigned)s.enabled, (unsigned)s.timeout_ms,
           (unsigned long)s.kernel_heartbeats,
           (unsigned long)s.sched_heartbeats,
           (unsigned long)s.syscall_heartbeats,
           (unsigned long)s.event_count,
           tobywdog_kind_str(s.last_event_kind),
           (int)s.last_event_pid);
    printf("M28C_WDOG: reason=\"%s\"\n", reason);

    /* Required: kernel must have noticed the simulated stall. The
     * boot harness in kernel.c calls wdog_simulate_kernel_stall()
     * BEFORE spawning us, so by the time we run there must be at
     * least one event recorded with kind = sched_stall. */
    if (!s.enabled) {
        printf("M28C_WDOG: FAIL (watchdog not enabled)\n");
        return 3;
    }
    if (s.event_count == 0) {
        printf("M28C_WDOG: FAIL (no bite events recorded)\n");
        return 3;
    }
    if (s.last_event_kind != ABI_WDOG_KIND_SCHED_STALL) {
        printf("M28C_WDOG: FAIL (unexpected last_kind=%u expected=%u)\n",
               (unsigned)s.last_event_kind,
               (unsigned)ABI_WDOG_KIND_SCHED_STALL);
        return 3;
    }
    if (s.kernel_heartbeats < 10) {
        printf("M28C_WDOG: FAIL (kernel hb suspiciously low: %lu)\n",
               (unsigned long)s.kernel_heartbeats);
        return 3;
    }
    if (s.syscall_heartbeats == 0) {
        printf("M28C_WDOG: FAIL (no syscall hb -- proc kick path broken)\n");
        return 3;
    }

    printf("M28C_WDOG: PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    int json = 0;
    int do_boot = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--json"))       json = 1;
        else if (!strcmp(a, "--boot"))  do_boot = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "wdogtest: unknown arg '%s'\n", a);
            usage();
            return 2;
        }
    }
    if (do_boot) return cmd_boot();

    struct abi_wdog_status s;
    if (read_status(&s) < 0) return 1;
    if (json) print_json(&s);
    else      print_pretty(&s);
    return 0;
}
