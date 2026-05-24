/* user_ifconfig/main.c -- network interface status for tobyOS.
 *
 * Usage: ifconfig
 *
 * Queries SYS_SYSTEM_METRICS (74) for live network state and prints
 * an ifconfig-style summary: link status, IP address, packet rate,
 * and basic memory/uptime context.
 */

#include <stdio.h>

typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;
typedef unsigned long      uint64_t;

struct sys_metrics {
    uint64_t uptime_ms;
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t cpu_user_ns;
    uint64_t cpu_total_ns;
    uint64_t total_syscalls;
    uint64_t context_switches;
    uint64_t gui_frames;
    uint64_t proc_spawns;
    uint64_t proc_exits;
    uint64_t vfs_ops;
    uint64_t vfs_ns;
    uint64_t gui_ns;
    uint64_t net_packets;
    uint64_t net_ns;

    uint32_t process_count;
    uint32_t ready_count;
    uint32_t blocked_count;
    uint32_t cpu_pct;
    uint32_t ram_pct;
    uint32_t gui_pct;
    uint32_t disk_pct;
    uint32_t net_pct;
    uint32_t syscalls_per_s;
    uint32_t gui_fps;
    uint32_t vfs_ops_per_s;
    uint32_t net_packets_per_s;
    uint32_t net_up;
    uint32_t net_ip_be;
    uint32_t tsc_mhz;
    uint32_t _reserved[9];
};

static inline long sys_system_metrics(struct sys_metrics *out) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(74L), "D"((long)out)
        : "rcx", "r11", "memory");
    return r;
}

static void fmt_ip(char *out, uint32_t ip_be) {
    const uint8_t *b = (const uint8_t *)&ip_be;
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = b[i];
        char tmp[4];
        int n = 0;
        if (v == 0) tmp[n++] = '0';
        else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
        while (n--) out[pos++] = tmp[n];
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct sys_metrics m;
    if (sys_system_metrics(&m) < 0) {
        fprintf(stderr, "ifconfig: failed to read system metrics\n");
        return 1;
    }

    char ipstr[20];
    fmt_ip(ipstr, m.net_ip_be);

    printf("eth0      Link status: %s\n", m.net_up ? "UP" : "DOWN");
    printf("          inet addr: %s\n", m.net_up ? ipstr : "(none)");
    printf("          Packets/s: %u\n", m.net_packets_per_s);
    printf("          Total packets: %lu\n", (unsigned long)m.net_packets);
    printf("          Net utilization: %u%%\n", m.net_pct);
    printf("\n");

    unsigned long up_s = (unsigned long)(m.uptime_ms / 1000);
    unsigned long hrs = up_s / 3600;
    unsigned long min = (up_s / 60) % 60;
    unsigned long sec = up_s % 60;

    printf("System    Uptime: %luh %lum %lus\n", hrs, min, sec);
    printf("          Memory: %lu / %lu pages (%u%%)\n",
           (unsigned long)m.used_pages, (unsigned long)m.total_pages,
           m.ram_pct);
    printf("          CPU: %u%%   TSC: %u MHz\n", m.cpu_pct, m.tsc_mhz);

    return 0;
}
