/* ps -- list the services the supervisor is managing.
 *
 * THIS TOOL HAD NEVER ONCE WORKED, and it was broken in two independent
 * ways that each hid behind the other. Found 2026-09-02 by running it on
 * real hardware through the remote channel, where it printed
 * "ps: syscall failed (-22)".
 *
 *   1. It asked for 128 entries. ABI_SYS_SVC_LIST refuses anything above
 *      ABI_SVC_LIST_MAX (64) with -EINVAL, so the call never returned data
 *      at all -- which is the only reason bug 2 was never visible.
 *
 *   2. It declared its OWN `struct svc_info` that did not match the
 *      kernel's `struct abi_service_info` in size, field order or field
 *      widths: a 32-byte name where the ABI has a 16-byte name followed by
 *      a 64-byte path, `kind` and `state` transposed, `int` where the ABI
 *      has fixed-width types, and a `long uptime_ms` against the ABI's
 *      `last_start_ms`. Had the cap been right, this would have printed
 *      confident garbage, which is worse than an error.
 *
 * Both fixed by using the ABI's own declaration instead of a hand-copied
 * one. A struct shared across a syscall boundary has exactly one correct
 * definition and it lives in the ABI header.
 *
 * NOTE ON SCOPE: this lists SERVICES, not every process -- that is what
 * ABI_SYS_SVC_LIST publishes. The shell's `ps` builtin dumps the real
 * process table. The two disagreeing is a naming wart worth fixing
 * separately; it is not this fix.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <tobyos/abi/abi.h>

static inline long sys_svc_list(struct abi_service_info *out, unsigned int cap) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"((long)ABI_SYS_SVC_LIST), "D"((long)out), "S"((long)cap)
        : "rcx", "r11", "memory");
    return r;
}

static const char *state_str(unsigned s) {
    switch (s) {
    case ABI_SVC_STATE_STOPPED:  return "stopped";
    case ABI_SVC_STATE_RUNNING:  return "running";
    case ABI_SVC_STATE_FAILED:   return "failed";
    case ABI_SVC_STATE_BACKOFF:  return "backoff";
    case ABI_SVC_STATE_DISABLED: return "disabled";
    default:                     return "unknown";
    }
}

static const char *kind_str(unsigned k) {
    return k == ABI_SVC_KIND_BUILTIN ? "builtin" : "program";
}

/* last_start_ms is an absolute boot-relative stamp, so uptime is now minus
 * then. The old code printed the stamp itself as though it were a duration. */
static void fmt_uptime(unsigned long long start_ms, unsigned long long now_ms,
                       char *buf, int sz) {
    if (start_ms == 0 || now_ms < start_ms) { snprintf(buf, sz, "-"); return; }
    unsigned long long sec = (now_ms - start_ms) / 1000ull;
    unsigned long long m = sec / 60, h = m / 60;
    sec %= 60; m %= 60;
    if (h > 0)      snprintf(buf, sz, "%lluh%llum%llus", h, m, sec);
    else if (m > 0) snprintf(buf, sz, "%llum%llus", m, sec);
    else            snprintf(buf, sz, "%llus", sec);
}

static unsigned long long now_ms(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"((long)ABI_SYS_CLOCK_MS) : "rcx", "r11", "memory");
    return r < 0 ? 0ull : (unsigned long long)r;
}

int main(int argc, char **argv) {
    int json = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = 1;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: ps [--json]\n"
                   "Lists the services the supervisor manages.\n");
            return 0;
        }
    }

    struct abi_service_info list[ABI_SVC_LIST_MAX];
    long count = sys_svc_list(list, ABI_SVC_LIST_MAX);
    if (count < 0) {
        fprintf(stderr, "ps: cannot read the service list: error %ld\n", -count);
        return 1;
    }

    unsigned long long t = now_ms();

    if (json) {
        printf("[\n");
        for (long i = 0; i < count; i++) {
            char up[32];
            fmt_uptime(list[i].last_start_ms, t, up, sizeof up);
            printf("  {\"pid\":%d,\"name\":\"%s\",\"kind\":\"%s\","
                   "\"state\":\"%s\",\"uptime\":\"%s\",\"restarts\":%u}%s\n",
                   list[i].pid, list[i].name, kind_str(list[i].kind),
                   state_str(list[i].state), up, list[i].restart_count,
                   (i < count - 1) ? "," : "");
        }
        printf("]\n");
    } else {
        printf("%-6s %-16s %-8s %-9s %-8s %s\n",
               "PID", "NAME", "KIND", "STATE", "UPTIME", "RESTARTS");
        for (long i = 0; i < count; i++) {
            char up[32];
            fmt_uptime(list[i].last_start_ms, t, up, sizeof up);
            printf("%-6d %-16s %-8s %-9s %-8s %u\n",
                   list[i].pid, list[i].name, kind_str(list[i].kind),
                   state_str(list[i].state), up, list[i].restart_count);
        }
        printf("\n%ld service(s)\n", count);
    }
    return 0;
}
