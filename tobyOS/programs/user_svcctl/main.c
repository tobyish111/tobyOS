/* user_svcctl/main.c -- /bin/svcctl, enhanced service manager CLI.
 *
 * Provides start/stop/restart/status/log subcommands for managing
 * kernel-supervised services. Communicates via SYS_SVC_LIST for
 * reading and SYS_SLOG_WRITE/READ for commands and logs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define SYS_SVC_LIST      65
#define SYS_SLOG_READ     58
#define SYS_SLOG_WRITE    59
#define SYS_CLOCK_MS      48

struct service_info {
    char     name[32];
    unsigned int kind;
    unsigned int state;
    int      pid;
    unsigned int restarts;
    unsigned long uptime_ms;
};

#define SVC_STATE_STOPPED  0
#define SVC_STATE_RUNNING  1
#define SVC_STATE_STARTING 2
#define SVC_STATE_FAILED   3

struct slog_record {
    unsigned long seq;
    unsigned long timestamp_ms;
    unsigned int  level;
    char          subsystem[16];
    char          message[128];
};

static inline long sys_svc_list(struct service_info *out, unsigned int cap) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_SVC_LIST), "D"(out), "S"((long)cap)
        : "rcx", "r11", "memory");
    return r;
}

static inline long sys_slog_read(struct slog_record *out, unsigned int cap,
                                  unsigned long since) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_SLOG_READ), "D"(out), "S"((long)cap), "d"((long)since)
        : "rcx", "r11", "memory");
    return r;
}

static inline long sys_slog_write(unsigned int level, const char *sub,
                                   const char *msg) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_SLOG_WRITE), "D"((long)level), "S"(sub), "d"(msg)
        : "rcx", "r11", "memory");
    return r;
}

static inline long sys_clock_ms(void) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"((long)SYS_CLOCK_MS) : "rcx", "r11", "memory");
    return r;
}

static const char *state_str(unsigned int s) {
    switch (s) {
    case SVC_STATE_STOPPED:  return "stopped";
    case SVC_STATE_RUNNING:  return "running";
    case SVC_STATE_STARTING: return "starting";
    case SVC_STATE_FAILED:   return "failed";
    default: return "unknown";
    }
}

static const char *kind_str(unsigned int k) {
    return k == 0 ? "builtin" : "program";
}

static void format_uptime(unsigned long ms, char *buf, int bufsz) {
    unsigned long sec = ms / 1000;
    unsigned long min = sec / 60; sec %= 60;
    unsigned long hr  = min / 60; min %= 60;
    snprintf(buf, (size_t)bufsz, "%luh%lum%lus", hr, min, sec);
}

/* ---- Commands --------------------------------------------------- */

static int cmd_status(void) {
    struct service_info svcs[32];
    long count = sys_svc_list(svcs, 32);
    if (count < 0) {
        fprintf(stderr, "svcctl: failed to query services\n");
        return 1;
    }

    printf("%-16s %-8s %-10s %5s %8s %10s\n",
           "NAME", "KIND", "STATE", "PID", "RESTARTS", "UPTIME");
    printf("%-16s %-8s %-10s %5s %8s %10s\n",
           "----", "----", "-----", "---", "--------", "------");

    for (long i = 0; i < count; i++) {
        char up[32];
        format_uptime(svcs[i].uptime_ms, up, sizeof(up));
        printf("%-16s %-8s %-10s %5d %8u %10s\n",
               svcs[i].name,
               kind_str(svcs[i].kind),
               state_str(svcs[i].state),
               svcs[i].pid,
               svcs[i].restarts,
               up);
    }
    printf("\n%ld service(s) total\n", count);
    return 0;
}

static int cmd_start(const char *name) {
    char msg[128];
    snprintf(msg, sizeof(msg), "svcctl: start %s", name);
    sys_slog_write(4, "svcctl", msg);
    printf("Requested start of '%s' (via slog command)\n", name);
    printf("Note: kernel service supervisor handles actual start.\n");
    return 0;
}

static int cmd_stop(const char *name) {
    char msg[128];
    snprintf(msg, sizeof(msg), "svcctl: stop %s", name);
    sys_slog_write(4, "svcctl", msg);
    printf("Requested stop of '%s' (via slog command)\n", name);
    return 0;
}

static int cmd_restart(const char *name) {
    cmd_stop(name);
    usleep(500000);
    cmd_start(name);
    return 0;
}

static int cmd_log(const char *name, int num_entries) {
    struct slog_record records[64];
    long count = sys_slog_read(records, 64, 0);
    if (count < 0) {
        fprintf(stderr, "svcctl: failed to read slog\n");
        return 1;
    }

    printf("Log entries");
    if (name) printf(" for '%s'", name);
    printf(":\n\n");

    int shown = 0;
    for (long i = 0; i < count && shown < num_entries; i++) {
        if (name && strstr(records[i].subsystem, name) == NULL &&
            strstr(records[i].message, name) == NULL)
            continue;
        char ts[32];
        unsigned long sec = records[i].timestamp_ms / 1000;
        unsigned long ms  = records[i].timestamp_ms % 1000;
        snprintf(ts, sizeof(ts), "%lu.%03lu", sec, ms);

        const char *lvl = "???";
        switch (records[i].level) {
        case 1: lvl = "ERR"; break;
        case 2: lvl = "WRN"; break;
        case 3: lvl = "INF"; break;
        case 4: lvl = "DBG"; break;
        case 5: lvl = "TRC"; break;
        }

        printf("[%10s] %-3s %-8s: %s\n",
               ts, lvl, records[i].subsystem, records[i].message);
        shown++;
    }

    if (shown == 0) printf("(no matching log entries)\n");
    return 0;
}

static void usage(void) {
    printf("Usage: svcctl <command> [args]\n\n");
    printf("Commands:\n");
    printf("  status               Show all services\n");
    printf("  start <name>         Start a service\n");
    printf("  stop <name>          Stop a service\n");
    printf("  restart <name>       Restart a service\n");
    printf("  log [name] [-n N]    Show service log entries\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "list") == 0)
        return cmd_status();

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) { fprintf(stderr, "svcctl: start requires a service name\n"); return 1; }
        return cmd_start(argv[2]);
    }
    if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) { fprintf(stderr, "svcctl: stop requires a service name\n"); return 1; }
        return cmd_stop(argv[2]);
    }
    if (strcmp(argv[1], "restart") == 0) {
        if (argc < 3) { fprintf(stderr, "svcctl: restart requires a service name\n"); return 1; }
        return cmd_restart(argv[2]);
    }
    if (strcmp(argv[1], "log") == 0) {
        const char *name = NULL;
        int num = 30;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
                num = atoi(argv[++i]);
            else
                name = argv[i];
        }
        return cmd_log(name, num);
    }

    fprintf(stderr, "svcctl: unknown command '%s'\n", argv[1]);
    usage();
    return 1;
}
