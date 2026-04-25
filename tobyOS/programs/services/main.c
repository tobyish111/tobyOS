/* programs/services/main.c -- Milestone 28F: service supervisor tool.
 *
 * Reads the kernel service registry via SYS_SVC_LIST and renders a
 * human-readable table. A `--boot` mode emits machine-grepable
 * sentinels for the test harness.
 *
 * Usage:
 *   services                -- list all services in a wide table
 *   services --json         -- machine-readable JSON line per record
 *   services --boot         -- emit `M28F_SERVICES:` sentinel block
 *                              and PASS/FAIL line for the test script.
 *   services --watch <name> -- print the named service's state as a
 *                              single line, then exit. Used by both
 *                              the harness and humans.
 *
 * Exit codes:
 *   0  the requested operation succeeded
 *   1  syscall failed (errno preserved)
 *   2  bad usage
 *   3  --watch <name>: service is DISABLED (crash-loop tripped)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tobyos_svc.h>

#define MAX_REC 16

static void usage(void) {
    fprintf(stderr,
            "usage: services [--json|--boot|--watch <name>]\n"
            "       services           list services in a table\n"
            "       services --json    one line of JSON per service\n"
            "       services --boot    test-harness sentinel mode\n"
            "       services --watch <name>   print the single record\n");
}

static const char *bool_str(uint8_t v) { return v ? "yes" : "no"; }

static void print_table(const struct abi_service_info *recs, int n) {
    printf("services: %d registered\n", n);
    printf("  %-12s %-7s %-9s %-4s %-5s %-5s %-5s %-5s %s\n",
           "name", "kind", "state", "auto", "pid",
           "rcnt", "ccnt", "lastx", "path");
    for (int i = 0; i < n; i++) {
        const struct abi_service_info *r = &recs[i];
        char name[ABI_SVC_NAME_MAX + 1];
        memcpy(name, r->name, ABI_SVC_NAME_MAX);
        name[ABI_SVC_NAME_MAX] = '\0';
        char path[ABI_SVC_PATH_MAX + 1];
        memcpy(path, r->path, ABI_SVC_PATH_MAX);
        path[ABI_SVC_PATH_MAX] = '\0';
        printf("  %-12s %-7s %-9s %-4s %-5d %-5u %-5u %-5d %s\n",
               name,
               tobysvc_kind_str(r->kind),
               tobysvc_state_str(r->state),
               bool_str(r->autorestart),
               (int)r->pid,
               (unsigned)r->restart_count,
               (unsigned)r->crash_count,
               (int)r->last_exit,
               path[0] ? path : "(builtin)");
    }
}

static void print_json(const struct abi_service_info *recs, int n) {
    for (int i = 0; i < n; i++) {
        const struct abi_service_info *r = &recs[i];
        char name[ABI_SVC_NAME_MAX + 1];
        memcpy(name, r->name, ABI_SVC_NAME_MAX);
        name[ABI_SVC_NAME_MAX] = '\0';
        char path[ABI_SVC_PATH_MAX + 1];
        memcpy(path, r->path, ABI_SVC_PATH_MAX);
        path[ABI_SVC_PATH_MAX] = '\0';
        printf("{\"name\":\"%s\",\"kind\":\"%s\",\"state\":\"%s\","
               "\"pid\":%d,\"autorestart\":%u,"
               "\"restart_count\":%u,\"crash_count\":%u,"
               "\"last_exit\":%d,\"last_start_ms\":%lu,"
               "\"last_crash_ms\":%lu,\"backoff_until_ms\":%lu,"
               "\"path\":\"%s\"}\n",
               name, tobysvc_kind_str(r->kind),
               tobysvc_state_str(r->state),
               (int)r->pid, (unsigned)r->autorestart,
               (unsigned)r->restart_count,
               (unsigned)r->crash_count,
               (int)r->last_exit,
               (unsigned long)r->last_start_ms,
               (unsigned long)r->last_crash_ms,
               (unsigned long)r->backoff_until_ms,
               path[0] ? path : "");
    }
}

static int find_idx(const struct abi_service_info *recs, int n,
                    const char *want) {
    for (int i = 0; i < n; i++) {
        char name[ABI_SVC_NAME_MAX + 1];
        memcpy(name, recs[i].name, ABI_SVC_NAME_MAX);
        name[ABI_SVC_NAME_MAX] = '\0';
        if (strcmp(name, want) == 0) return i;
    }
    return -1;
}

static int cmd_watch(const struct abi_service_info *recs, int n,
                     const char *want) {
    int idx = find_idx(recs, n, want);
    if (idx < 0) {
        fprintf(stderr, "services: no service named '%s'\n", want);
        return 1;
    }
    const struct abi_service_info *r = &recs[idx];
    char name[ABI_SVC_NAME_MAX + 1];
    memcpy(name, r->name, ABI_SVC_NAME_MAX);
    name[ABI_SVC_NAME_MAX] = '\0';
    printf("service '%s' state=%s pid=%d restart_count=%u "
           "crash_count=%u last_exit=%d backoff_until_ms=%lu\n",
           name, tobysvc_state_str(r->state),
           (int)r->pid, (unsigned)r->restart_count,
           (unsigned)r->crash_count, (int)r->last_exit,
           (unsigned long)r->backoff_until_ms);
    if (r->state == ABI_SVC_STATE_DISABLED) return 3;
    return 0;
}

/* M28F harness verdict. The kernel-side test deterministically
 * crashes a service /bin/svc_crasher 5+ times and we want to confirm
 * the supervisor recorded that, transitioned through BACKOFF, and
 * finally tripped DISABLED. */
static int cmd_boot(const struct abi_service_info *recs, int n) {
    /* Always print the per-service line so it's grepable even if
     * the service we're after is missing. */
    int found_any = 0;
    int found_login = 0;
    int crasher_idx = -1;
    for (int i = 0; i < n; i++) {
        const struct abi_service_info *r = &recs[i];
        char name[ABI_SVC_NAME_MAX + 1];
        memcpy(name, r->name, ABI_SVC_NAME_MAX);
        name[ABI_SVC_NAME_MAX] = '\0';
        printf("M28F_SERVICES: name=%s state=%s kind=%s pid=%d "
               "rcnt=%u ccnt=%u last_exit=%d backoff_until_ms=%lu\n",
               name, tobysvc_state_str(r->state),
               tobysvc_kind_str(r->kind),
               (int)r->pid,
               (unsigned)r->restart_count,
               (unsigned)r->crash_count,
               (int)r->last_exit,
               (unsigned long)r->backoff_until_ms);
        found_any = 1;
        if (strcmp(name, "login") == 0)        found_login = 1;
        if (strcmp(name, "crasher") == 0)      crasher_idx = i;
    }
    if (!found_any) {
        printf("M28F_SERVICES: FAIL no records returned by SVC_LIST\n");
        return 1;
    }
    if (!found_login) {
        printf("M28F_SERVICES: WARN login service not registered\n");
    }
    if (crasher_idx < 0) {
        /* No crasher service was registered (i.e. SVCTEST_FLAG was 0).
         * Just confirm the registry is non-empty and call it PASS. */
        printf("M28F_SERVICES: PASS (registry has %d records, no crash test)\n",
               n);
        return 0;
    }
    const struct abi_service_info *c = &recs[crasher_idx];
    /* Containment proof: the supervisor saw the crasher exit non-zero
     * SERVICE_DISABLE_THRESHOLD-or-more times. The current state can
     * be DISABLED (still tripped) or STOPPED (an operator cleared the
     * trip) -- both prove the crash-loop got broken. Anything else is
     * either a too-fast snapshot (RUNNING/BACKOFF) or a real bug. */
    int contained = (c->crash_count >= 5u) &&
                    (c->state == ABI_SVC_STATE_DISABLED ||
                     c->state == ABI_SVC_STATE_STOPPED);
    if (contained) {
        printf("M28F_SERVICES: PASS crasher state=%s crashes=%u "
               "restart_count=%u (crash-loop contained)\n",
               tobysvc_state_str(c->state),
               (unsigned)c->crash_count,
               (unsigned)c->restart_count);
        return 0;
    }
    printf("M28F_SERVICES: FAIL crasher state=%s crashes=%u restart_count=%u\n",
           tobysvc_state_str(c->state),
           (unsigned)c->crash_count,
           (unsigned)c->restart_count);
    return 1;
}

int main(int argc, char **argv) {
    int json = 0, boot = 0;
    const char *watch_name = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--json"))  json = 1;
        else if (!strcmp(a, "--boot"))  boot = 1;
        else if (!strcmp(a, "--watch")) {
            if (++i >= argc) { usage(); return 2; }
            watch_name = argv[i];
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "services: unexpected argument '%s'\n", a);
            usage();
            return 2;
        }
    }

    struct abi_service_info recs[MAX_REC];
    memset(recs, 0, sizeof(recs));
    int n = tobysvc_list(recs, MAX_REC);
    if (n < 0) {
        fprintf(stderr, "services: SVC_LIST syscall failed: %s\n",
                strerror(errno));
        if (boot) printf("M28F_SERVICES: FAIL syscall errno=%d\n", errno);
        return 1;
    }

    if (boot)        return cmd_boot(recs, n);
    if (watch_name)  return cmd_watch(recs, n, watch_name);
    if (json)        { print_json(recs, n); return 0; }
    print_table(recs, n);
    return 0;
}
