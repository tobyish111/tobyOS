#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sys_metrics {
    long uptime_ms;
    long total_pages;
    long used_pages;
    long free_pages;
    long cpu_user_ns;
    long cpu_total_ns;
    long total_syscalls;
    long context_switches;
    long gui_frames;
    long proc_spawns;
    int  cpu_pct;
    int  mem_pct;
    int  disk_pct;
    int  net_pct;
    long syscalls_per_s;
    long gui_fps;
    long vfs_ops_per_s;
    long net_packets_per_s;
    int  net_up;
    unsigned int net_ip_be;
    long tsc_mhz;
};

static inline long sys_get_metrics(struct sys_metrics *out) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(74L), "D"((long)out)
        : "rcx", "r11", "memory");
    return r;
}

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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sys_metrics m;
    struct svc_info procs[128];
    char buf[2];

    for (;;) {
        write(1, "\x0c", 1);

        if (sys_get_metrics(&m) < 0) {
            printf("top: metrics syscall failed\n");
            return 1;
        }
        long nprocs = sys_svc_list(procs, 128);
        if (nprocs < 0) nprocs = 0;

        long sec = m.uptime_ms / 1000;
        long min = sec / 60;
        long hrs = min / 60;
        long days = hrs / 24;
        sec %= 60; min %= 60; hrs %= 24;

        printf("tobyOS top - up %ldd %ldh %ldm %lds\n", days, hrs, min, sec);
        printf("CPU: %d%%   MEM: %d%% (%ld/%ld pages)   Procs: %ld\n",
            m.cpu_pct, m.mem_pct, m.used_pages, m.total_pages, nprocs);
        printf("Syscalls/s: %ld   Ctx switches: %ld   TSC: %ld MHz\n",
            m.syscalls_per_s, m.context_switches, m.tsc_mhz);
        printf("Disk: %d%%   Net: %s   VFS ops/s: %ld\n",
            m.disk_pct, m.net_up ? "UP" : "DOWN", m.vfs_ops_per_s);
        printf("\n%-6s %-20s %-8s %s\n", "PID", "NAME", "STATE", "UPTIME(s)");
        printf("----------------------------------------------\n");

        for (long i = 0; i < nprocs && i < 20; i++) {
            const char *st = "?";
            if (procs[i].state == 0) st = "stop";
            else if (procs[i].state == 1) st = "run";
            else if (procs[i].state == 2) st = "crash";
            printf("%-6d %-20s %-8s %ld\n",
                procs[i].pid, procs[i].name, st, procs[i].uptime_ms / 1000);
        }

        printf("\nPress 'q' to quit.\n");

        usleep(1000000);

        long n = read(0, buf, 1);
        if (n > 0 && buf[0] == 'q')
            break;
    }
    return 0;
}
