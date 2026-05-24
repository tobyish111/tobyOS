#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

struct svc_info {
    char name[32];
    int  kind;
    int  state;
    int  pid;
    int  restarts;
    long uptime_ms;
};

static inline long sys_svc_list(struct svc_info *out, unsigned int cap) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(65L), "D"((long)out), "S"((long)cap)
        : "rcx", "r11", "memory");
    return r;
}

static const char *state_str(int s) {
    switch (s) {
    case 0: return "stopped";
    case 1: return "running";
    case 2: return "crashed";
    default: return "unknown";
    }
}

static void fmt_uptime(long ms, char *buf, int sz) {
    long sec = ms / 1000;
    long m = sec / 60;
    long h = m / 60;
    sec %= 60;
    m %= 60;
    if (h > 0)
        snprintf(buf, sz, "%ldh%ldm%lds", h, m, sec);
    else if (m > 0)
        snprintf(buf, sz, "%ldm%lds", m, sec);
    else
        snprintf(buf, sz, "%lds", sec);
}

int main(int argc, char **argv) {
    int json = 0;
    int opt;
    while ((opt = getopt(argc, argv, "-")) != -1) {
        (void)opt;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = 1;
    }

    struct svc_info list[128];
    long count = sys_svc_list(list, 128);
    if (count < 0) {
        fprintf(stderr, "ps: syscall failed (%ld)\n", count);
        return 1;
    }

    if (json) {
        printf("[\n");
        for (long i = 0; i < count; i++) {
            char up[32];
            fmt_uptime(list[i].uptime_ms, up, sizeof(up));
            printf("  {\"pid\":%d,\"name\":\"%s\",\"state\":\"%s\",\"uptime\":\"%s\"}%s\n",
                list[i].pid, list[i].name, state_str(list[i].state), up,
                (i < count - 1) ? "," : "");
        }
        printf("]\n");
    } else {
        printf("%-6s %-20s %-8s %s\n", "PID", "NAME", "STATE", "UPTIME");
        printf("%-6s %-20s %-8s %s\n", "---", "----", "-----", "------");
        for (long i = 0; i < count; i++) {
            char up[32];
            fmt_uptime(list[i].uptime_ms, up, sizeof(up));
            printf("%-6d %-20s %-8s %s\n",
                list[i].pid, list[i].name, state_str(list[i].state), up);
        }
        printf("\n%ld process(es)\n", count);
    }
    return 0;
}
