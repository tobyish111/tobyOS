/* systemctl / service -- the service registry, under the names people type.
 *
 *     systemctl [status|is-active|is-failed|show|list-units|cat] [NAME]
 *     service NAME status | service --status-all
 *
 * WHAT THIS CANNOT DO, AND WHY IT SAYS SO INSTEAD OF PRETENDING:
 * the supervisor exposes exactly one syscall, ABI_SYS_SVC_LIST, and it
 * only READS. There is no start, no stop, no enable. Those verbs are
 * refused by name, with the reason -- because a `systemctl start` that
 * printed nothing and changed nothing is the worst possible answer, and a
 * script would believe it.
 *
 * Everything it CAN do is real: the state, pid, exit code, restart and
 * crash counts, backoff deadline and autorestart flag all come from the
 * live registry the kernel supervises.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <tobyos/abi/abi.h>
#include <tobyos_svc.h>

#define MAX_SVC 64

static const char *prog = "systemctl";

static const char *state_name(uint32_t s) {
    switch (s) {
    case ABI_SVC_STATE_STOPPED:  return "inactive";
    case ABI_SVC_STATE_RUNNING:  return "active";
    case ABI_SVC_STATE_FAILED:   return "failed";
    case ABI_SVC_STATE_BACKOFF:  return "activating";
    case ABI_SVC_STATE_DISABLED: return "failed";
    default:                     return "unknown";
    }
}

/* systemd prints a SUB state beside the load/active columns; these are the
 * ones this supervisor can actually distinguish. */
static const char *sub_name(uint32_t s) {
    switch (s) {
    case ABI_SVC_STATE_STOPPED:  return "dead";
    case ABI_SVC_STATE_RUNNING:  return "running";
    case ABI_SVC_STATE_FAILED:   return "failed";
    case ABI_SVC_STATE_BACKOFF:  return "auto-restart";
    case ABI_SVC_STATE_DISABLED: return "crash-loop";
    default:                     return "unknown";
    }
}

static int load_all(struct abi_service_info *out) {
    int n = tobysvc_list(out, MAX_SVC);
    if (n < 0) {
        fprintf(stderr, "%s: cannot read the service registry: %s\n",
                prog, strerror(errno));
        return -1;
    }
    return n;
}

/* A name matches with or without the .service suffix, the way systemctl
 * lets you type either. */
static int name_matches(const char *have, const char *want) {
    if (!strcmp(have, want)) return 1;
    size_t wl = strlen(want);
    if (wl > 8 && !strcmp(want + wl - 8, ".service") &&
        !strncmp(have, want, wl - 8) && strlen(have) == wl - 8) return 1;
    return 0;
}

static int find_one(struct abi_service_info *v, int n, const char *want) {
    for (int i = 0; i < n; i++) if (name_matches(v[i].name, want)) return i;
    return -1;
}

static void print_status(const struct abi_service_info *s) {
    printf("* %s.service - %s\n", s->name,
           s->kind == ABI_SVC_KIND_BUILTIN ? "kernel-resident service"
                                           : "supervised program");
    printf("     Loaded: loaded (%s; %s)\n",
           s->path[0] ? s->path : "built-in",
           s->autorestart ? "enabled" : "static");
    printf("     Active: %s (%s)\n", state_name(s->state), sub_name(s->state));
    if (s->pid > 0) printf("   Main PID: %d\n", (int)s->pid);
    if (s->last_start_ms)
        printf("      Since: %llu ms after boot\n",
               (unsigned long long)s->last_start_ms);
    if (s->restart_count) printf("   Restarts: %u\n", (unsigned)s->restart_count);
    if (s->crash_count) {
        printf("     Crashes: %u (last at %llu ms, exit %d)\n",
               (unsigned)s->crash_count,
               (unsigned long long)s->last_crash_ms, (int)s->last_exit);
    }
    if (s->state == ABI_SVC_STATE_BACKOFF)
        printf("    Backoff: until %llu ms\n",
               (unsigned long long)s->backoff_until_ms);
    if (s->state == ABI_SVC_STATE_DISABLED)
        printf("             disabled by the crash-loop guard\n");
}

static void print_show(const struct abi_service_info *s) {
    printf("Id=%s.service\n", s->name);
    printf("ExecStart=%s\n", s->path[0] ? s->path : "(built-in)");
    printf("ActiveState=%s\n", state_name(s->state));
    printf("SubState=%s\n", sub_name(s->state));
    printf("MainPID=%d\n", (int)s->pid);
    printf("ExecMainStatus=%d\n", (int)s->last_exit);
    printf("NRestarts=%u\n", (unsigned)s->restart_count);
    printf("NCrashes=%u\n", (unsigned)s->crash_count);
    printf("Restart=%s\n", s->autorestart ? "always" : "no");
}

static void list_units(struct abi_service_info *v, int n) {
    printf("%-24s %-6s %-8s %-12s %s\n",
           "UNIT", "LOAD", "ACTIVE", "SUB", "DESCRIPTION");
    for (int i = 0; i < n; i++) {
        char unit[ABI_SVC_NAME_MAX + 10];
        snprintf(unit, sizeof unit, "%s.service", v[i].name);
        printf("%-24s %-6s %-8s %-12s %s\n", unit, "loaded",
               state_name(v[i].state), sub_name(v[i].state),
               v[i].kind == ABI_SVC_KIND_BUILTIN ? "kernel-resident service"
                                                 : "supervised program");
    }
    printf("\n%d unit(s) listed.\n", n);
}

static int refuse_control(const char *verb) {
    fprintf(stderr,
            "%s: cannot %s a service: this supervisor exposes only "
            "ABI_SYS_SVC_LIST, which reads the registry. There is no "
            "start/stop/enable syscall to call.\n", prog, verb);
    return 1;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "systemctl";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    int is_service = !strcmp(base, "service");

    static struct abi_service_info v[MAX_SVC];

    /* `service --status-all` and `service NAME VERB` swap the order that
     * systemctl uses, so normalise to (verb, name) first. */
    const char *verb = NULL, *name = NULL;
    if (is_service) {
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "--status-all")) { verb = "list-units"; continue; }
            if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
                printf("usage: service NAME status | service --status-all\n");
                return 0;
            }
            if (!name) name = a;
            else if (!verb) verb = a;
        }
        if (!verb) verb = "status";
    } else {
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (a[0] == '-') {
                if (!strcmp(a, "--version")) { printf("systemctl (tobyOS) 1.0\n"); return 0; }
                if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
                    printf("usage: systemctl [status|is-active|is-failed|show|"
                           "list-units|cat] [NAME]\n");
                    return 0;
                }
                continue;                      /* --no-pager and friends */
            }
            if (!verb) verb = a;
            else if (!name) name = a;
        }
        if (!verb) verb = "list-units";
    }

    if (!strcmp(verb, "start") || !strcmp(verb, "stop") ||
        !strcmp(verb, "restart") || !strcmp(verb, "reload") ||
        !strcmp(verb, "enable") || !strcmp(verb, "disable") ||
        !strcmp(verb, "mask") || !strcmp(verb, "unmask"))
        return refuse_control(verb);

    int n = load_all(v);
    if (n < 0) return 1;

    if (!strcmp(verb, "list-units") || !strcmp(verb, "list-unit-files")) {
        list_units(v, n);
        return 0;
    }

    if (!name) {
        if (!strcmp(verb, "status")) { list_units(v, n); return 0; }
        fprintf(stderr, "%s: %s needs a unit name\n", prog, verb);
        return 1;
    }

    int idx = find_one(v, n, name);
    if (idx < 0) {
        fprintf(stderr, "%s: unit %s not found.\n", prog, name);
        /* systemctl exits 4 for an unknown unit; is-active exits 3. */
        return !strcmp(verb, "is-active") || !strcmp(verb, "is-failed") ? 3 : 4;
    }

    if (!strcmp(verb, "status")) {
        print_status(&v[idx]);
        /* systemctl exits 3 when the unit is not active. */
        return v[idx].state == ABI_SVC_STATE_RUNNING ? 0 : 3;
    }
    if (!strcmp(verb, "is-active")) {
        printf("%s\n", state_name(v[idx].state));
        return v[idx].state == ABI_SVC_STATE_RUNNING ? 0 : 3;
    }
    if (!strcmp(verb, "is-failed")) {
        int failed = v[idx].state == ABI_SVC_STATE_FAILED ||
                     v[idx].state == ABI_SVC_STATE_DISABLED;
        printf("%s\n", failed ? "failed" : state_name(v[idx].state));
        return failed ? 0 : 1;
    }
    if (!strcmp(verb, "show")) { print_show(&v[idx]); return 0; }
    if (!strcmp(verb, "cat")) {
        printf("# %s.service (kernel registry, not a unit file)\n", v[idx].name);
        printf("[Service]\nExecStart=%s\nRestart=%s\n",
               v[idx].path[0] ? v[idx].path : "(built-in)",
               v[idx].autorestart ? "always" : "no");
        return 0;
    }

    fprintf(stderr, "%s: unknown verb '%s'\n", prog, verb);
    return 1;
}
