/* user_netutils -- arp arping ip netstat route telnet traceroute, native
 * (2026-09-04, native-parity batch). One binary dispatched on argv[0].
 *
 * Sources: /proc/net/{route,arp,tcp,udp,unix,dev} (the first two are new
 * with this batch), ABI_SYS_SYSTEM_METRICS for the address, ABI_SYS_NETCFG
 * for the setters and the MAC, ABI_SYS_ARP for arping and arp -d/-s,
 * ABI_SYS_SOCKOPT(IP_TTL) for traceroute, libtoby's TCP client for telnet.
 * Output formats follow iproute2, net-tools, iputils and inetutils. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <termios.h>
#include <sys/stat.h>
#include <tobyos/abi/abi.h>
#include <tobyos/socket.h>
#include <toby/net.h>

static const char *g_prog = "netutils";

/* recvfrom's source-address record (the same shape ping uses) */
struct src_addr { uint32_t ip; uint16_t port; uint16_t pad; };

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}

/* --------------------------------------------------------------- helpers */
static uint16_t htons_(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static int parse_ip(const char *s, uint32_t *be) {
    unsigned a, b, c, d; char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    memcpy(be, x, 4);
    return 0;
}

static int parse_prefix(const char *s, uint32_t *be, int *plen) {
    char buf[64]; snprintf(buf, sizeof buf, "%s", s);
    char *sl = strchr(buf, '/');
    *plen = 32;
    if (sl) { *sl = '\0'; *plen = atoi(sl + 1); if (*plen < 0 || *plen > 32) return -1; }
    if (!strcmp(buf, "default")) { *be = 0; *plen = 0; return 0; }
    return parse_ip(buf, be);
}

static uint32_t mask_from_plen(int plen) {
    if (plen <= 0) return 0;
    uint32_t host = plen >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> plen);
    uint8_t x[4] = { (uint8_t)(host >> 24), (uint8_t)(host >> 16), (uint8_t)(host >> 8), (uint8_t)host };
    uint32_t be; memcpy(&be, x, 4); return be;
}

static int plen_from_mask(uint32_t be) {
    uint8_t x[4]; memcpy(x, &be, 4);
    uint32_t host = (uint32_t)x[0] << 24 | (uint32_t)x[1] << 16 | (uint32_t)x[2] << 8 | x[3];
    int n = 0; while (n < 32 && (host & (0x80000000u >> n))) n++;
    return n;
}

static void fmt_ip(uint32_t be, char *out) {
    uint8_t x[4]; memcpy(x, &be, 4);
    sprintf(out, "%u.%u.%u.%u", x[0], x[1], x[2], x[3]);
}

static void fmt_mac(const uint8_t *m, char *out) {
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int parse_mac(const char *s, uint8_t *m) {
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) { if (v[i] > 255) return -1; m[i] = (uint8_t)v[i]; }
    return 0;
}

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct ifinfo { int up; uint32_t ip, mask, gw; uint8_t mac[6]; };

struct rt { char iface[16]; uint32_t dst, gw, mask; unsigned flags, metric; };
static int read_routes(struct rt *out, int cap) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return 0;
    char line[256]; int n = 0;
    while (fgets(line, sizeof line, f) && n < cap) {
        char iface[16]; unsigned long dst, gw, mask; unsigned flags, refcnt, use, metric;
        if (sscanf(line, "%15s %lx %lx %x %u %u %u %lx", iface, &dst, &gw, &flags, &refcnt, &use, &metric, &mask) != 8) continue;
        snprintf(out[n].iface, 16, "%s", iface);
        out[n].dst = (uint32_t)dst; out[n].gw = (uint32_t)gw; out[n].mask = (uint32_t)mask; out[n].flags = flags; out[n].metric = metric;
        n++;
    }
    fclose(f);
    return n;
}

static void read_if(struct ifinfo *i) {
    memset(i, 0, sizeof *i);
    struct abi_system_metrics m;
    if (sc2(ABI_SYS_SYSTEM_METRICS, (long)(uintptr_t)&m, sizeof m) >= 0) { i->up = (int)m.net_up; i->ip = m.net_ip_be; }
    struct rt r[16]; int n = read_routes(r, 16);
    for (int k = 0; k < n; k++) {
        if (r[k].dst == 0 && r[k].mask == 0 && r[k].gw) i->gw = r[k].gw;
        else if (r[k].mask && i->ip && (i->ip & r[k].mask) == r[k].dst) i->mask = r[k].mask;
    }
    if (!i->mask && i->ip) i->mask = mask_from_plen(24);
    sc3(ABI_SYS_NETCFG, 5, (long)(uintptr_t)i->mac, 0);
}

struct arpent { uint32_t ip; uint8_t mac[6]; };
static int read_arp(struct arpent *out, int cap) {
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return 0;
    char line[256]; int n = 0;
    while (fgets(line, sizeof line, f) && n < cap) {
        char ip[32], hw[32], fl[8], mac[32], mask[8], dev[16];
        if (sscanf(line, "%31s %31s %7s %31s %7s %15s", ip, hw, fl, mac, mask, dev) != 6) continue;
        if (!strcmp(ip, "IP")) continue;
        if (parse_ip(ip, &out[n].ip) || parse_mac(mac, out[n].mac)) continue;
        n++;
    }
    fclose(f);
    return n;
}

static const char *neterr(long rc) {
    switch (-rc) {
    case ABI_EPERM: return "Operation not permitted";
    case ABI_ENOENT: return "No such file or directory";
    case ABI_EINVAL: return "Invalid argument";
    case ABI_ENODEV: return "No such device";
    case ABI_ENOSPC: return "No space left on device";
    default: return "Unknown error";
    }
}

/* ====================================================================
 *                                  ip
 * ==================================================================== */
static void ip_show_link(int idx, const char *name, struct ifinfo *i, int with_addr, int oneline) {
    if (!strcmp(name, "lo")) {
        printf("%d: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000%s", idx, oneline ? "\\    " : "\n    ");
        printf("link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
        if (with_addr) printf("%s    inet 127.0.0.1/8 scope host lo\n       valid_lft forever preferred_lft forever\n", oneline ? "1: lo" : "");
        return;
    }
    char mac[24]; fmt_mac(i->mac, mac);
    printf("%d: eth0: <BROADCAST,MULTICAST%s> mtu 1500 qdisc fq_codel state %s group default qlen 1000%s",
           idx, i->up ? ",UP,LOWER_UP" : "", i->up ? "UP" : "DOWN", oneline ? "\\    " : "\n    ");
    printf("link/ether %s brd ff:ff:ff:ff:ff:ff\n", mac);
    if (with_addr && i->ip) {
        char a[20], b[20]; fmt_ip(i->ip, a); fmt_ip(i->ip | ~i->mask, b);
        printf("%s    inet %s/%d brd %s scope global eth0\n       valid_lft forever preferred_lft forever\n", oneline ? "2: eth0" : "", a, plen_from_mask(i->mask), b);
    }
}

static int cmd_ip(int argc, char **argv) {
    int i = 1, oneline = 0, brief = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-4") || !strcmp(a, "-s") || !strcmp(a, "-d") || !strcmp(a, "-c") || !strcmp(a, "-color") || !strcmp(a, "-n") || !strcmp(a, "-numeric") || !strcmp(a, "-h") || !strcmp(a, "-human")) continue;
        if (!strcmp(a, "-6")) { return 0; }
        if (!strcmp(a, "-o") || !strcmp(a, "-oneline")) { oneline = 1; continue; }
        if (!strcmp(a, "-br") || !strcmp(a, "-brief")) { brief = 1; continue; }
        if (!strcmp(a, "-V") || !strcmp(a, "-Version")) { printf("ip utility, iproute2-6.1.0 (tobyOS)\n"); return 0; }
        if (!strcmp(a, "-f") || !strcmp(a, "-family")) { i++; continue; }
        fprintf(stderr, "Option \"%s\" is unknown, try \"ip -help\".\n", a); return 255;
    }
    const char *obj = i < argc ? argv[i++] : "help";
    const char *cmd = i < argc ? argv[i] : "show";
    if (!strcmp(obj, "help") || !strcmp(obj, "-help")) {
        printf("Usage: ip [ OPTIONS ] OBJECT { COMMAND | help }\nwhere  OBJECT := { address | link | route | neigh }\n       OPTIONS := { -4 | -o[neline] | -br[ief] | -V[ersion] }\n");
        return 0;
    }
    struct ifinfo ifi; read_if(&ifi);
    int is_addr = !strncmp(obj, "a", 1) && strncmp(obj, "addr", 4) == 0 ? 1 : (!strcmp(obj, "a") || !strcmp(obj, "address") || !strncmp(obj, "addr", 4));
    if (!strcmp(obj, "a") || !strncmp(obj, "addr", 4) || !strcmp(obj, "l") || !strncmp(obj, "link", 4)) {
        int link_only = !is_addr;
        if (!strcmp(cmd, "show") || !strcmp(cmd, "list") || !strcmp(cmd, "ls") || !strcmp(cmd, "sh") || !strcmp(cmd, "lst") || i >= argc) {
            const char *dev = NULL;
            for (int k = i + 1; k < argc; k++) { if (!strcmp(argv[k], "dev") && k + 1 < argc) dev = argv[++k]; else if (!strcmp(argv[k], "up") || !strcmp(argv[k], "scope") || !strcmp(argv[k], "to")) { if (strcmp(argv[k], "up")) k++; } else dev = argv[k]; }
            if (i < argc && strcmp(cmd, "show") && strcmp(cmd, "list") && strcmp(cmd, "ls") && strcmp(cmd, "sh") && strcmp(cmd, "lst")) dev = cmd;
            if (brief) {
                char a[20]; fmt_ip(ifi.ip, a);
                if (!dev || !strcmp(dev, "lo")) { if (link_only) printf("lo               UNKNOWN        00:00:00:00:00:00 <LOOPBACK,UP,LOWER_UP>\n"); else printf("lo               UNKNOWN        127.0.0.1/8\n"); }
                if (!dev || !strcmp(dev, "eth0")) { char mac[24]; fmt_mac(ifi.mac, mac); if (link_only) printf("eth0             %-14s %s <BROADCAST,MULTICAST%s>\n", ifi.up ? "UP" : "DOWN", mac, ifi.up ? ",UP,LOWER_UP" : ""); else printf("eth0             %-14s %s/%d\n", ifi.up ? "UP" : "DOWN", a, plen_from_mask(ifi.mask)); }
                return 0;
            }
            if (dev && strcmp(dev, "lo") && strcmp(dev, "eth0")) { fprintf(stderr, "Device \"%s\" does not exist.\n", dev); return 1; }
            if (!dev || !strcmp(dev, "lo")) ip_show_link(1, "lo", &ifi, !link_only, oneline);
            if (!dev || !strcmp(dev, "eth0")) ip_show_link(2, "eth0", &ifi, !link_only, oneline);
            return 0;
        }
        if (!strcmp(cmd, "set") && link_only) {
            const char *dev = NULL; int up = -1;
            for (int k = i + 1; k < argc; k++) {
                if (!strcmp(argv[k], "dev") && k + 1 < argc) dev = argv[++k];
                else if (!strcmp(argv[k], "up")) up = 1;
                else if (!strcmp(argv[k], "down")) up = 0;
                else if (!strcmp(argv[k], "mtu") || !strcmp(argv[k], "address") || !strcmp(argv[k], "name")) k++;
                else if (!dev) dev = argv[k];
            }
            if (!dev) { fprintf(stderr, "Not enough information: \"dev\" argument is required.\n"); return 1; }
            if (strcmp(dev, "eth0") && strcmp(dev, "lo")) { fprintf(stderr, "Cannot find device \"%s\"\n", dev); return 1; }
            if (up < 0 || !strcmp(dev, "lo")) return 0;
            long rc = sc3(ABI_SYS_NETCFG, 4, up, 0);
            if (rc < 0) { fprintf(stderr, "RTNETLINK answers: %s\n", neterr(rc)); return 2; }
            return 0;
        }
        if ((!strcmp(cmd, "add") || !strcmp(cmd, "del") || !strcmp(cmd, "delete") || !strcmp(cmd, "change") || !strcmp(cmd, "replace")) && !link_only) {
            uint32_t addr = 0; int plen = 24; const char *dev = NULL;
            for (int k = i + 1; k < argc; k++) {
                if (!strcmp(argv[k], "dev") && k + 1 < argc) dev = argv[++k];
                else if (!strcmp(argv[k], "brd") || !strcmp(argv[k], "broadcast") || !strcmp(argv[k], "label") || !strcmp(argv[k], "scope") || !strcmp(argv[k], "peer")) k++;
                else if (parse_prefix(argv[k], &addr, &plen)) { fprintf(stderr, "Error: any valid prefix is expected rather than \"%s\".\n", argv[k]); return 1; }
            }
            if (!dev) { fprintf(stderr, "Not enough information: \"dev\" argument is required.\n"); return 1; }
            if (strcmp(dev, "eth0")) { fprintf(stderr, "Cannot find device \"%s\"\n", dev); return 1; }
            int del = cmd[0] == 'd';
            if (del && addr != ifi.ip) { fprintf(stderr, "RTNETLINK answers: Cannot assign requested address\n"); return 2; }
            long rc = sc3(ABI_SYS_NETCFG, 0, del ? 0 : (long)addr, del ? 0 : (long)mask_from_plen(plen));
            if (rc < 0) { fprintf(stderr, "RTNETLINK answers: %s\n", neterr(rc)); return 2; }
            return 0;
        }
        if (!strcmp(cmd, "flush") && !link_only) { long rc = sc3(ABI_SYS_NETCFG, 0, 0, 0); return rc < 0 ? 2 : 0; }
        fprintf(stderr, "Command \"%s\" is unknown, try \"ip %s help\".\n", cmd, link_only ? "link" : "address");
        return 255;
    }
    if (!strcmp(obj, "r") || !strncmp(obj, "route", 2) || !strcmp(obj, "ro")) {
        if (!strcmp(cmd, "show") || !strcmp(cmd, "list") || !strcmp(cmd, "ls") || !strcmp(cmd, "sh") || i >= argc) {
            struct rt r[16]; int n = read_routes(r, 16);
            char a[20], g[20], s[20];
            for (int k = 0; k < n; k++) {
                if (r[k].dst == 0 && r[k].mask == 0) { fmt_ip(r[k].gw, g); printf("default via %s dev %s%s\n", g, r[k].iface, r[k].metric ? "" : ""); }
            }
            for (int k = 0; k < n; k++) {
                if (r[k].dst == 0 && r[k].mask == 0) continue;
                fmt_ip(r[k].dst, a);
                if (r[k].gw) { fmt_ip(r[k].gw, g); printf("%s/%d via %s dev %s\n", a, plen_from_mask(r[k].mask), g, r[k].iface); }
                else { fmt_ip(ifi.ip, s); printf("%s/%d dev %s proto kernel scope link src %s\n", a, plen_from_mask(r[k].mask), r[k].iface, s); }
            }
            return 0;
        }
        if (!strcmp(cmd, "get")) {
            if (i + 1 >= argc) { fprintf(stderr, "Command line is not complete. Try option \"help\"\n"); return 255; }
            uint32_t dst; if (parse_ip(argv[i + 1], &dst)) { fprintf(stderr, "Error: an inet address is expected rather than \"%s\".\n", argv[i + 1]); return 1; }
            char a[20], g[20], s[20]; fmt_ip(dst, a); fmt_ip(ifi.ip, s);
            if (ifi.mask && (dst & ifi.mask) == (ifi.ip & ifi.mask)) printf("%s dev eth0 src %s uid %d \n    cache \n", a, s, (int)getuid());
            else { fmt_ip(ifi.gw, g); printf("%s via %s dev eth0 src %s uid %d \n    cache \n", a, g, s, (int)getuid()); }
            return 0;
        }
        if (!strcmp(cmd, "add") || !strcmp(cmd, "del") || !strcmp(cmd, "delete") || !strcmp(cmd, "replace") || !strcmp(cmd, "change")) {
            uint32_t dst = 0, gw = 0; int plen = 0, have_dst = 0;
            for (int k = i + 1; k < argc; k++) {
                if (!strcmp(argv[k], "via") && k + 1 < argc) { if (parse_ip(argv[++k], &gw)) { fprintf(stderr, "Error: inet address is expected rather than \"%s\".\n", argv[k]); return 1; } }
                else if (!strcmp(argv[k], "dev") && k + 1 < argc) k++;
                else if (!strcmp(argv[k], "metric") || !strcmp(argv[k], "proto") || !strcmp(argv[k], "scope") || !strcmp(argv[k], "src") || !strcmp(argv[k], "table") || !strcmp(argv[k], "mtu")) k++;
                else if (!strcmp(argv[k], "onlink")) {}
                else if (!strcmp(argv[k], "to") && k + 1 < argc) { k++; if (parse_prefix(argv[k], &dst, &plen)) { fprintf(stderr, "Error: an inet prefix is expected rather than \"%s\".\n", argv[k]); return 1; } have_dst = 1; }
                else { if (parse_prefix(argv[k], &dst, &plen)) { fprintf(stderr, "Error: an inet prefix is expected rather than \"%s\".\n", argv[k]); return 1; } have_dst = 1; }
            }
            if (!have_dst) { fprintf(stderr, "Command line is not complete. Try option \"help\"\n"); return 255; }
            int del = cmd[0] == 'd';
            long rc;
            if (plen == 0) rc = sc3(ABI_SYS_NETCFG, 1, del ? 0 : (long)gw, 0);        /* default route == gateway */
            else if (del) rc = sc4(ABI_SYS_NETCFG, 3, (long)dst, (long)mask_from_plen(plen), 0);
            else rc = sc4(ABI_SYS_NETCFG, 2, (long)dst, (long)mask_from_plen(plen), (long)gw);
            if (rc < 0) { fprintf(stderr, "RTNETLINK answers: %s\n", rc == -ABI_ENOENT ? "No such process" : neterr(rc)); return 2; }
            return 0;
        }
        if (!strcmp(cmd, "flush")) return 0;
        fprintf(stderr, "Command \"%s\" is unknown, try \"ip route help\".\n", cmd);
        return 255;
    }
    if (!strcmp(obj, "n") || !strncmp(obj, "neigh", 1)) {
        if (!strcmp(cmd, "show") || !strcmp(cmd, "list") || !strcmp(cmd, "ls") || !strcmp(cmd, "sh") || i >= argc) {
            struct arpent e[32]; int n = read_arp(e, 32);
            for (int k = 0; k < n; k++) { char a[20], m[24]; fmt_ip(e[k].ip, a); fmt_mac(e[k].mac, m); printf("%s dev eth0 lladdr %s REACHABLE\n", a, m); }
            return 0;
        }
        if (!strcmp(cmd, "add") || !strcmp(cmd, "replace") || !strcmp(cmd, "change") || !strcmp(cmd, "del") || !strcmp(cmd, "delete")) {
            uint32_t ip = 0; int have = 0; uint8_t mac[6] = { 0 }; int have_mac = 0;
            for (int k = i + 1; k < argc; k++) {
                if (!strcmp(argv[k], "lladdr") && k + 1 < argc) { if (parse_mac(argv[++k], mac)) { fprintf(stderr, "Error: \"%s\" is not a valid lladdr.\n", argv[k]); return 1; } have_mac = 1; }
                else if (!strcmp(argv[k], "dev") && k + 1 < argc) k++;
                else if (!strcmp(argv[k], "nud") && k + 1 < argc) k++;
                else if (!strcmp(argv[k], "to") && k + 1 < argc) { if (parse_ip(argv[++k], &ip)) { fprintf(stderr, "Error: inet address is expected rather than \"%s\".\n", argv[k]); return 1; } have = 1; }
                else { if (parse_ip(argv[k], &ip)) { fprintf(stderr, "Error: inet address is expected rather than \"%s\".\n", argv[k]); return 1; } have = 1; }
            }
            if (!have) { fprintf(stderr, "Command line is not complete. Try option \"help\"\n"); return 255; }
            long rc;
            if (cmd[0] == 'd') rc = sc3(ABI_SYS_ARP, 3, (long)ip, 0);
            else { if (!have_mac) { fprintf(stderr, "Not enough information: \"lladdr\" argument is required.\n"); return 1; } rc = sc3(ABI_SYS_ARP, 4, (long)ip, (long)(uintptr_t)mac); }
            if (rc < 0) { fprintf(stderr, "RTNETLINK answers: %s\n", rc == -ABI_ENOENT ? "No such file or directory" : neterr(rc)); return 2; }
            return 0;
        }
        if (!strcmp(cmd, "flush")) { struct arpent e[32]; int n = read_arp(e, 32); for (int k = 0; k < n; k++) sc3(ABI_SYS_ARP, 3, (long)e[k].ip, 0); return 0; }
        fprintf(stderr, "Command \"%s\" is unknown, try \"ip neigh help\".\n", cmd);
        return 255;
    }
    fprintf(stderr, "Object \"%s\" is unknown, try \"ip help\".\n", obj);
    return 1;
}

/* ====================================================================
 *                            route / netstat -r
 * ==================================================================== */
static void print_route_table(int numeric) {
    struct rt r[16]; int n = read_routes(r, 16);
    printf("Kernel IP routing table\n");
    printf("Destination     Gateway         Genmask         Flags Metric Ref    Use Iface\n");
    for (int k = 0; k < n; k++) {
        char d[20], g[20], m[20];
        if (r[k].dst == 0 && r[k].mask == 0) snprintf(d, sizeof d, numeric ? "0.0.0.0" : "default"); else fmt_ip(r[k].dst, d);
        if (r[k].gw) fmt_ip(r[k].gw, g); else snprintf(g, sizeof g, numeric ? "0.0.0.0" : "*");
        fmt_ip(r[k].mask, m);
        char fl[8] = "U"; if (r[k].gw) strcat(fl, "G"); if (r[k].mask == 0xFFFFFFFFu) strcat(fl, "H");
        printf("%-15s %-15s %-15s %-5s %-6u %-6u %4u %s\n", d, g, m, fl, r[k].metric, 0, 0, r[k].iface);
    }
}

static int cmd_route(int argc, char **argv) {
    int numeric = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-n")) numeric = 1;
        else if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "-A") || !strcmp(argv[i], "-v") || !strcmp(argv[i], "-F") || !strcmp(argv[i], "-C")) { if (!strcmp(argv[i], "-A")) i++; }
        else if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) { printf("net-tools 2.10 (tobyOS)\nroute 1.98\n"); return 0; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { printf("Usage: route [-nNvee] [-FC] [<AF>]           List kernel routing tables\n       route [-v] [-FC] {add|del|flush} ...  Modify routing table for AF.\n"); return 0; }
        else { fprintf(stderr, "route: invalid option -- '%s'\n", argv[i] + 1); return 1; }
    }
    if (i >= argc) { print_route_table(numeric); return 0; }
    const char *cmd = argv[i++];
    if (!strcmp(cmd, "add") || !strcmp(cmd, "del") || !strcmp(cmd, "delete")) {
        uint32_t dst = 0, mask = 0, gw = 0; int is_default = 0, have = 0, host = 0;
        for (int k = i; k < argc; k++) {
            if (!strcmp(argv[k], "default")) { is_default = 1; have = 1; }
            else if (!strcmp(argv[k], "-net") && k + 1 < argc) { int pl; if (parse_prefix(argv[++k], &dst, &pl)) { fprintf(stderr, "route: bad address '%s'\n", argv[k]); return 1; } if (strchr(argv[k], '/')) mask = mask_from_plen(pl); have = 1; }
            else if (!strcmp(argv[k], "-host") && k + 1 < argc) { if (parse_ip(argv[++k], &dst)) { fprintf(stderr, "route: bad address '%s'\n", argv[k]); return 1; } mask = 0xFFFFFFFFu; host = 1; have = 1; }
            else if ((!strcmp(argv[k], "netmask") || !strcmp(argv[k], "genmask")) && k + 1 < argc) { if (parse_ip(argv[++k], &mask)) { fprintf(stderr, "route: bad netmask '%s'\n", argv[k]); return 1; } }
            else if ((!strcmp(argv[k], "gw") || !strcmp(argv[k], "gateway")) && k + 1 < argc) { if (parse_ip(argv[++k], &gw)) { fprintf(stderr, "route: bad gateway '%s'\n", argv[k]); return 1; } }
            else if (!strcmp(argv[k], "dev") && k + 1 < argc) k++;
            else if (!strcmp(argv[k], "metric") || !strcmp(argv[k], "mss") || !strcmp(argv[k], "window") || !strcmp(argv[k], "irtt")) k++;
            else if (!strcmp(argv[k], "reject") || !strcmp(argv[k], "mod") || !strcmp(argv[k], "dyn") || !strcmp(argv[k], "reinstate")) {}
            else { int pl; if (parse_prefix(argv[k], &dst, &pl)) { fprintf(stderr, "route: bad address '%s'\n", argv[k]); return 1; } if (strchr(argv[k], '/')) mask = mask_from_plen(pl); have = 1; }
        }
        if (!have) { fprintf(stderr, "Usage: route [-nNvee] [-FC] [<AF>]           List kernel routing tables\n       route [-v] [-FC] {add|del|flush} ...  Modify routing table for AF.\n"); return 1; }
        (void)host;
        int del = cmd[0] == 'd';
        long rc;
        if (is_default || (dst == 0 && mask == 0)) rc = sc3(ABI_SYS_NETCFG, 1, del ? 0 : (long)gw, 0);
        else {
            if (!mask) { uint8_t x[4]; memcpy(x, &dst, 4); mask = mask_from_plen(x[0] < 128 ? 8 : x[0] < 192 ? 16 : 24); }
            rc = del ? sc4(ABI_SYS_NETCFG, 3, (long)dst, (long)mask, 0) : sc4(ABI_SYS_NETCFG, 2, (long)dst, (long)mask, (long)gw);
        }
        if (rc < 0) { fprintf(stderr, "SIOC%sRT: %s\n", del ? "DEL" : "ADD", rc == -ABI_ENOENT ? "No such process" : neterr(rc)); return 7; }
        return 0;
    }
    if (!strcmp(cmd, "flush")) return 0;
    fprintf(stderr, "route: invalid command '%s'\n", cmd);
    return 1;
}

/* ====================================================================
 *                                netstat
 * ==================================================================== */
static const char *tcp_state(unsigned st) {
    static const char *n[] = { "", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT", "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING" };
    return st < 12 ? n[st] : "UNKNOWN";
}

static void fmt_sockaddr(unsigned long ip_hex, unsigned port, int numeric, char *out) {
    uint32_t be = (uint32_t)ip_hex;
    char a[20];
    if (be == 0) snprintf(a, sizeof a, numeric ? "0.0.0.0" : "0.0.0.0");
    else fmt_ip(be, a);
    if (port) snprintf(out, 48, "%s:%u", a, port); else snprintf(out, 48, "%s:*", a);
}

static void netstat_inet(const char *proto, int listening_only, int all, int numeric) {
    char path[32]; snprintf(path, sizeof path, "/proc/net/%s", proto);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned sl, st, txq, rxq; unsigned long la, ra; unsigned lp, rp;
        if (sscanf(line, "%u: %lx:%x %lx:%x %x %x:%x", &sl, &la, &lp, &ra, &rp, &st, &txq, &rxq) != 8) continue;
        int is_tcp = proto[0] == 't';
        int listen = is_tcp ? st == 10 : (ra == 0 && rp == 0);
        if (listening_only && !listen) continue;
        if (!listening_only && !all && listen) continue;
        char l[48], r[48]; fmt_sockaddr(la, lp, numeric, l); fmt_sockaddr(ra, rp, numeric, r);
        printf("%-5s %6u %6u %-23s %-23s %s\n", proto, rxq, txq, l, r, is_tcp ? tcp_state(st) : "");
    }
    fclose(f);
}

static void netstat_unix(int listening_only, int all) {
    FILE *f = fopen("/proc/net/unix", "r");
    if (!f) return;
    char line[512];
    printf("Active UNIX domain sockets (%s)\n", listening_only ? "only servers" : all ? "servers and established" : "w/o servers");
    printf("Proto RefCnt Flags       Type       State         I-Node   Path\n");
    while (fgets(line, sizeof line, f)) {
        unsigned long num; unsigned refcnt, proto, flags, type, st, inode; char pth[256] = "";
        if (sscanf(line, "%lx: %x %x %x %x %x %u %255s", &num, &refcnt, &proto, &flags, &type, &st, &inode, pth) < 7) continue;
        int listen = flags & 0x10000;
        if (listening_only && !listen) continue;
        if (!listening_only && !all && listen) continue;
        printf("unix  %-6u %-11s %-10s %-13s %-8u %s\n", refcnt, listen ? "[ ACC ]" : "[ ]", type == 1 ? "STREAM" : type == 2 ? "DGRAM" : "SEQPACKET",
               st == 1 ? "LISTENING" : st == 3 ? "CONNECTED" : "", inode, pth);
    }
    fclose(f);
}

static int cmd_netstat(int argc, char **argv) {
    int tcp = 0, udp = 0, unx = 0, all = 0, listening = 0, numeric = 0, route = 0, iface = 0, stats = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--version") || !strcmp(a, "-V")) { printf("net-tools 2.10 (tobyOS)\nnetstat 1.60\n"); return 0; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { printf("usage: netstat [-vWeenNcCF] [<Af>] -r         netstat {-V|--version|-h|--help}\n       netstat [-vWnNcaeol] [<Socket> ...]\n       netstat { [-vWeenNac] -i | [-cnNe] -M | -s [-6tuw] }\n"); return 0; }
        if (!strcmp(a, "--tcp")) { tcp = 1; continue; }
        if (!strcmp(a, "--udp")) { udp = 1; continue; }
        if (!strcmp(a, "--unix")) { unx = 1; continue; }
        if (!strcmp(a, "--all")) { all = 1; continue; }
        if (!strcmp(a, "--listening")) { listening = 1; continue; }
        if (!strcmp(a, "--numeric")) { numeric = 1; continue; }
        if (!strcmp(a, "--route")) { route = 1; continue; }
        if (!strcmp(a, "--interfaces")) { iface = 1; continue; }
        if (!strcmp(a, "--statistics")) { stats = 1; continue; }
        if (!strcmp(a, "--programs") || !strcmp(a, "--extend") || !strcmp(a, "--wide") || !strcmp(a, "--inet")) continue;
        if (a[0] == '-' && a[1] != '-') {
            for (const char *p = a + 1; *p; p++) switch (*p) {
                case 't': tcp = 1; break; case 'u': udp = 1; break; case 'x': unx = 1; break; case 'a': all = 1; break;
                case 'l': listening = 1; break; case 'n': numeric = 1; break; case 'r': route = 1; break; case 'i': iface = 1; break;
                case 's': stats = 1; break; case 'p': case 'e': case 'W': case 'w': case 'c': case 'v': case 'o': case '4': case 'A': case 'g': case 'C': case 'F': case 'N': break;
                case '6': return 0;
                default: fprintf(stderr, "netstat: invalid option -- '%c'\n", *p); return 1;
            }
            continue;
        }
        fprintf(stderr, "netstat: invalid option -- '%s'\n", a); return 1;
    }
    if (route) { print_route_table(numeric); return 0; }
    if (iface) {
        printf("Kernel Interface table\n");
        printf("Iface      MTU    RX-OK RX-ERR RX-DRP RX-OVR    TX-OK TX-ERR TX-DRP TX-OVR Flg\n");
        FILE *f = fopen("/proc/net/dev", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                char name[32]; unsigned long rb, rp, re, rd, ro, rf, rc, rm, tb, tp, te, td, to, tc, tca, tcr;
                char *colon = strchr(line, ':'); if (!colon) continue;
                *colon = ' ';
                if (sscanf(line, "%31s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", name, &rb, &rp, &re, &rd, &ro, &rf, &rc, &rm, &tb, &tp, &te, &td, &to, &tc, &tca, &tcr) < 17) continue;
                printf("%-9s %5d %8lu %6lu %6lu %6lu %8lu %6lu %6lu %6lu %s\n", name, !strcmp(name, "lo") ? 65536 : 1500, rp, re, rd, ro, tp, te, td, to, !strcmp(name, "lo") ? "LRU" : "BMRU");
            }
            fclose(f);
        }
        return 0;
    }
    if (stats) {
        struct abi_system_metrics m;
        if (sc2(ABI_SYS_SYSTEM_METRICS, (long)(uintptr_t)&m, sizeof m) < 0) memset(&m, 0, sizeof m);
        printf("Ip:\n    %lu total packets received\n    0 forwarded\n    0 incoming packets discarded\nIcmp:\n    0 ICMP messages received\nTcp:\n    0 active connection openings\nUdp:\n    0 packets received\n", (unsigned long)m.net_packets);
        return 0;
    }
    if (!tcp && !udp && !unx) tcp = udp = unx = 1;
    if (tcp || udp) {
        printf("Active Internet connections (%s)\n", listening ? "only servers" : all ? "servers and established" : "w/o servers");
        printf("Proto Recv-Q Send-Q Local Address           Foreign Address         State      \n");
        if (tcp) netstat_inet("tcp", listening, all, numeric);
        if (udp) netstat_inet("udp", listening, all, numeric);
    }
    if (unx) netstat_unix(listening, all);
    return 0;
}

/* ====================================================================
 *                                  arp
 * ==================================================================== */
static int cmd_arp(int argc, char **argv) {
    int all = 0, numeric = 0, verbose = 0; const char *del = NULL, *set = NULL, *mac = NULL; int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-a") || !strcmp(a, "--all")) { all = 1; if (i + 1 < argc && argv[i + 1][0] != '-') i++; continue; }
        if (!strcmp(a, "-n") || !strcmp(a, "--numeric")) { numeric = 1; continue; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; continue; }
        if ((!strcmp(a, "-i") || !strcmp(a, "-H") || !strcmp(a, "-t") || !strcmp(a, "-A") || !strcmp(a, "-p")) && i + 1 < argc) { i++; continue; }
        if (!strcmp(a, "-e") || !strcmp(a, "-D")) continue;
        if (!strcmp(a, "-d") && i + 1 < argc) { del = argv[++i]; continue; }
        if (!strcmp(a, "-s") && i + 2 < argc) { set = argv[++i]; mac = argv[++i]; continue; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("net-tools 2.10 (tobyOS)\narp 1.88\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("Usage:\n  arp [-vn]  [<HW>] [-i <if>] [-a] [<hostname>]             <-Display ARP cache\n  arp [-v]          [-i <if>] -d  <host> [pub]               <-Delete ARP entry\n  arp [-v]   [<HW>] [-i <if>] -s  <host> <hwaddr> [temp]     <-Add entry\n"); return 0; }
        if (a[0] == '-') { fprintf(stderr, "arp: invalid option -- '%s'\n", a + 1); return 1; }
    }
    if (del) {
        uint32_t ip; if (parse_ip(del, &ip)) { fprintf(stderr, "arp: %s: Unknown host\n", del); return 1; }
        long rc = sc3(ABI_SYS_ARP, 3, (long)ip, 0);
        if (rc < 0) { if (rc == -ABI_ENOENT) { fprintf(stderr, "No ARP entry for %s\n", del); return 1; } fprintf(stderr, "SIOCDARP(dontpub): %s\n", neterr(rc)); return 1; }
        return 0;
    }
    if (set) {
        uint32_t ip; uint8_t m[6];
        if (parse_ip(set, &ip)) { fprintf(stderr, "arp: %s: Unknown host\n", set); return 1; }
        if (parse_mac(mac, m)) { fprintf(stderr, "arp: invalid hardware address\n"); return 1; }
        long rc = sc3(ABI_SYS_ARP, 4, (long)ip, (long)(uintptr_t)m);
        if (rc < 0) { fprintf(stderr, "SIOCSARP: %s\n", neterr(rc)); return 1; }
        return 0;
    }
    struct arpent e[32]; int n = read_arp(e, 32);
    if (all) {
        for (int k = 0; k < n; k++) { char a[20], m[24]; fmt_ip(e[k].ip, a); fmt_mac(e[k].mac, m); printf("? (%s) at %s [ether] on eth0\n", a, m); }
        if (!n && verbose) printf("Entries: 0\tSkipped: 0\tFound: 0\n");
        return 0;
    }
    (void)numeric;
    printf("Address                  HWtype  HWaddress           Flags Mask            Iface\n");
    for (int k = 0; k < n; k++) { char a[20], m[24]; fmt_ip(e[k].ip, a); fmt_mac(e[k].mac, m); printf("%-24s %-7s %-19s %-5s %-15s %s\n", a, "ether", m, "C", "", "eth0"); }
    if (verbose) printf("Entries: %d\tSkipped: 0\tFound: %d\n", n, n);
    return 0;
}

/* ====================================================================
 *                                arping
 * ==================================================================== */
static int cmd_arping(int argc, char **argv) {
    int count = -1, quiet = 0, first_reply = 0, dad = 0, timeout = -1, interval_ms = 1000; const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-c") && i + 1 < argc) { count = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-w") && i + 1 < argc) { timeout = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-i") && i + 1 < argc) { interval_ms = atoi(argv[++i]) * 1000; if (interval_ms < 10) interval_ms = 10; continue; }
        if ((!strcmp(a, "-I") || !strcmp(a, "-s")) && i + 1 < argc) { i++; continue; }
        if (!strcmp(a, "-q")) { quiet = 1; continue; }
        if (!strcmp(a, "-f")) { first_reply = 1; continue; }
        if (!strcmp(a, "-D")) { dad = 1; continue; }
        if (!strcmp(a, "-U") || !strcmp(a, "-A") || !strcmp(a, "-b")) continue;
        if (!strcmp(a, "-V")) { printf("arping from iputils 20221126 (tobyOS)\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("\nUsage:\n  arping [options] <destination>\n\nOptions:\n  -f            quit on first reply\n  -q            be quiet\n  -b            keep on broadcasting, do not unicast\n  -D            duplicate address detection mode\n  -U            unsolicited ARP mode, update your neighbours\n  -A            ARP answer mode, update your neighbours\n  -V            print version and exit\n  -c <count>    how many packets to send\n  -w <timeout>  how long to wait for a reply\n  -i <interval> set interval between packets (default: 1 second)\n  -I <device>   which ethernet device to use\n  -s <source>   source ip address\n  <destination> dns name or ip address\n"); return 0; }
        if (a[0] == '-') { fprintf(stderr, "arping: invalid option -- '%c'\n", a[1]); return 2; }
        target = a;
    }
    if (!target) { fprintf(stderr, "arping: option requires an argument -- 'destination'\n"); return 2; }
    uint32_t ip;
    if (parse_ip(target, &ip)) { fprintf(stderr, "arping: %s: Name or service not known\n", target); return 2; }
    struct ifinfo ifi; read_if(&ifi);
    char src[20]; fmt_ip(ifi.ip, src);
    if (!ifi.up) { fprintf(stderr, "arping: Interface \"eth0\" is down\n"); return 2; }
    /* privilege check up front: the request op is root-only */
    if (getuid() != 0) { fprintf(stderr, "arping: socket: Operation not permitted\n"); return 2; }
    if (!quiet) printf("ARPING %s from %s eth0\n", target, src);
    int sent = 0, received = 0;
    uint64_t start = now_ns();
    while (count < 0 || sent < count) {
        if (timeout > 0 && (now_ns() - start) / 1000000000ull >= (uint64_t)timeout) break;
        uint8_t mac[6];
        sc3(ABI_SYS_ARP, 3, (long)ip, 0);                     /* forget it: measure a real round trip */
        uint64_t t0 = now_ns();
        sent++;
        int got = 0;
        while ((now_ns() - t0) < (uint64_t)interval_ms * 1000000ull) {
            /* op 5 sends the request and pumps the NIC; the sleep keeps the
             * wait in userspace so the reply is actually drained */
            long r = sc3(ABI_SYS_ARP, 5, (long)ip, (long)(uintptr_t)mac);
            if (r == 0) { got = 1; break; }
            if (r == -ABI_EIO) { fprintf(stderr, "arping: sendto: Network is unreachable\n"); return 2; }
            usleep(5000);
        }
        if (got) {
            received++;
            uint64_t dt = now_ns() - t0;
            if (!quiet) printf("Unicast reply from %s [%02X:%02X:%02X:%02X:%02X:%02X]  %lu.%03lums\n", target, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned long)(dt / 1000000), (unsigned long)((dt / 1000) % 1000));
            if (first_reply || dad) break;
            uint64_t left = (uint64_t)interval_ms * 1000000ull - dt;
            if (count < 0 || sent < count) usleep((unsigned)(left / 1000));
        }
        fflush(stdout);
    }
    if (!quiet) { printf("Sent %d probes (%d broadcast(s))\nReceived %d response(s)\n", sent, sent, received); }
    if (dad) return received ? 1 : 0;
    return received ? 0 : 1;
}

/* ====================================================================
 *                               traceroute
 * ==================================================================== */
static int cmd_traceroute(int argc, char **argv) {
    int max_ttl = 30, first_ttl = 1, nq = 3, wait_s = 5, numeric = 0, port = 33434; const char *host = NULL; int pktlen = 60;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-m") && i + 1 < argc) { max_ttl = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-f") && i + 1 < argc) { first_ttl = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-q") && i + 1 < argc) { nq = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-w") && i + 1 < argc) { wait_s = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-p") && i + 1 < argc) { port = atoi(argv[++i]); continue; }
        if ((!strcmp(a, "-i") || !strcmp(a, "-s") || !strcmp(a, "-t") || !strcmp(a, "-z") || !strcmp(a, "-N")) && i + 1 < argc) { i++; continue; }
        if (!strcmp(a, "-n")) { numeric = 1; continue; }
        if (!strcmp(a, "-I") || !strcmp(a, "-T") || !strcmp(a, "-U") || !strcmp(a, "-4") || !strcmp(a, "-F") || !strcmp(a, "-d") || !strcmp(a, "-r")) continue;
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("Modern traceroute for Linux, version 2.1.2 (tobyOS)\n"); return 0; }
        if (!strcmp(a, "--help")) { printf("Usage:\n  traceroute [ -46dFITnreAUDV ] [ -f first_ttl ] [ -m max_ttl ] [ -p port ] [ -q nqueries ] [ -w waittime ] host [ packetlen ]\n"); return 0; }
        if (a[0] == '-') { fprintf(stderr, "traceroute: invalid option -- '%c'\n", a[1]); return 2; }
        if (!host) host = a; else pktlen = atoi(a);
    }
    if (!host) { fprintf(stderr, "Specify \"host\" missing argument.\n"); return 2; }
    uint32_t dst;
    if (parse_ip(host, &dst)) {
        uint32_t r = 0;
        if (toby_dns_resolve(host, &r) == 0) dst = r;
        else { fprintf(stderr, "traceroute: unknown host %s\n", host); return 2; }
    }
    if (max_ttl < 1 || max_ttl > 255 || first_ttl < 1 || first_ttl > max_ttl) { fprintf(stderr, "traceroute: first hop out of range\n"); return 2; }
    if (getuid() != 0) { fprintf(stderr, "traceroute: socket: Operation not permitted\n"); return 2; }
    int icmp = socket(AF_INET, SOCK_RAW | (IPPROTO_ICMP << SOCK_RAW_PROTO_SHIFT), 0);
    if (icmp < 0) { fprintf(stderr, "traceroute: socket: %s\n", strerror(errno)); return 2; }
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) { fprintf(stderr, "traceroute: udp socket: %s\n", strerror(errno)); return 2; }
    uint16_t sport = (uint16_t)(32768 + (getpid() & 0x3FFF));
    if (bind(udp, htons_(sport)) < 0) { fprintf(stderr, "traceroute: bind: %s\n", strerror(errno)); return 2; }
    char d[20]; fmt_ip(dst, d);
    printf("traceroute to %s (%s), %d hops max, %d byte packets\n", host, d, max_ttl, pktlen);
    int payload = pktlen - 28; if (payload < 0) payload = 0; if (payload > 1400) payload = 1400;
    static uint8_t probe[1500];
    int done = 0;
    for (int ttl = first_ttl; ttl <= max_ttl && !done; ttl++) {
        printf("%2d ", ttl);
        uint32_t last_from = 0;
        for (int q = 0; q < nq; q++) {
            int dport = port + (ttl - 1) * nq + q;
            memset(probe, 0, sizeof probe);
            probe[0] = (uint8_t)ttl; probe[1] = (uint8_t)q;
            if (toby_sockopt(udp, ABI_SOCKOPT_IP_TTL, ttl) < 0) { fprintf(stderr, "traceroute: setsockopt IP_TTL: %s\n", strerror(errno)); return 2; }
            uint64_t t0 = now_ns();
            /* the first datagram to a fresh next hop loses to ARP: ip_send
             * fires a request and drops the packet. Retry, pumping the NIC
             * through a bounded recvfrom between attempts, until it goes. */
            int sent_ok = 0;
            for (int attempt = 0; attempt < 40 && !sent_ok; attempt++) {
                if (sendto(udp, probe, (size_t)payload, dst, htons_((uint16_t)dport)) >= 0) { sent_ok = 1; break; }
                uint8_t pump[64]; struct src_addr ps; recvfrom(icmp, pump, sizeof pump, &ps);
                usleep(20000);
            }
            t0 = now_ns();
            if (!sent_ok) { printf(" !S"); continue; }
            int got = 0; uint32_t from = 0; int final = 0;
            while ((now_ns() - t0) < (uint64_t)wait_s * 1000000000ull) {
                uint8_t buf[2048]; struct src_addr sa;
                long n = recvfrom(icmp, buf, sizeof buf, &sa);
                if (n <= 0) continue;
                size_t ihl = (size_t)(buf[0] & 0x0F) * 4;
                if ((size_t)n < ihl + 8 + 20 + 8) continue;
                const uint8_t *ic = buf + ihl;
                uint8_t type = ic[0], code = ic[1];
                if (type != 11 && type != 3) continue;
                const uint8_t *inner = ic + 8;
                size_t iihl = (size_t)(inner[0] & 0x0F) * 4;
                if (inner[9] != 17) continue;                 /* not our UDP */
                uint32_t idst; memcpy(&idst, inner + 16, 4);
                if (idst != dst) continue;
                const uint8_t *uh = inner + iihl;
                unsigned isp = (unsigned)uh[0] << 8 | uh[1], idp = (unsigned)uh[2] << 8 | uh[3];
                if (isp != sport || (int)idp != dport) continue;
                memcpy(&from, buf + 12, 4);
                got = 1;
                if (type == 3) { final = 1; if (code != 3) printf(" !%c", code == 0 ? 'N' : code == 1 ? 'H' : code == 2 ? 'P' : 'X'); }
                break;
            }
            uint64_t dt = now_ns() - t0;
            if (!got) { printf(" *"); fflush(stdout); continue; }
            if (from != last_from) { char f[20]; fmt_ip(from, f); if (numeric) printf(" %s", f); else printf(" %s (%s)", f, f); last_from = from; }
            printf("  %lu.%03lu ms", (unsigned long)(dt / 1000000), (unsigned long)((dt / 1000) % 1000));
            fflush(stdout);
            if (final) done = 1;
        }
        printf("\n");
    }
    close(udp); close(icmp);
    return 0;
}

/* ====================================================================
 *                                 telnet
 * ==================================================================== */
#define IAC 255
#define DONT 254
#define DO 253
#define WONT 252
#define WILL 251
#define SB 250
#define SE 240

static int telnet_filter(int conn, uint8_t *buf, int n, int *state, uint8_t *out) {
    /* returns the number of bytes to print; answers negotiations */
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        switch (*state) {
        case 0: if (c == IAC) *state = 1; else out[o++] = c; break;
        case 1:
            if (c == IAC) { out[o++] = IAC; *state = 0; }
            else if (c == DO || c == DONT || c == WILL || c == WONT) { *state = 10 + (c - WONT); }
            else if (c == SB) *state = 20;
            else *state = 0;
            break;
        case 10: case 11: case 12: case 13: {
            uint8_t cmd = (uint8_t)(WONT + (*state - 10));
            uint8_t reply[3] = { IAC, 0, c };
            if (cmd == DO) reply[1] = WONT; else if (cmd == WILL) reply[1] = DONT; else reply[1] = 0;
            if (reply[1]) toby_tcp_send(conn, reply, 3);
            *state = 0; break; }
        case 20: if (c == IAC) *state = 21; break;
        case 21: *state = c == SE ? 0 : 20; break;
        }
    }
    return o;
}

static int cmd_telnet(int argc, char **argv) {
    const char *host = NULL; int port = 23; int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-l") && i + 1 < argc) { i++; continue; }
        if (!strcmp(a, "-e") && i + 1 < argc) { i++; continue; }
        if (!strcmp(a, "-4") || !strcmp(a, "-8") || !strcmp(a, "-E") || !strcmp(a, "-a") || !strcmp(a, "-c") || !strcmp(a, "-d") || !strcmp(a, "-K")) continue;
        if (!strcmp(a, "--version")) { printf("telnet (tobyOS inetutils) 2.4\n"); return 0; }
        if (!strcmp(a, "--help")) { printf("Usage: telnet [OPTION...] [HOST [PORT]]\nLogin using the TELNET protocol.\n"); return 0; }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "telnet: invalid option -- '%c'\n", a[1]); return 1; }
        if (!host) host = a; else port = atoi(a);
    }
    if (!host) { fprintf(stderr, "telnet: no host specified (interactive mode is not supported)\n"); return 1; }
    if (port <= 0 || port > 65535) { fprintf(stderr, "telnet: %s: bad port number\n", argv[argc - 1]); return 1; }
    uint32_t ip; char ipstr[20];
    int conn;
    if (parse_ip(host, &ip) == 0) { fmt_ip(ip, ipstr); printf("Trying %s...\n", ipstr); fflush(stdout); conn = toby_tcp_connect(ip, htons_((uint16_t)port), 10000); }
    else { printf("Trying %s...\n", host); fflush(stdout); snprintf(ipstr, sizeof ipstr, "%s", host); conn = toby_tcp_connect_host(host, (uint16_t)port, 10000); }
    if (conn < 0) { printf("telnet: Unable to connect to remote host: Connection refused\n"); return 1; }
    printf("Connected to %s.\nEscape character is '^]'.\n", ipstr);
    fflush(stdout);
    int tty = isatty(0);
    struct termios saved, raw;
    if (tty && tcgetattr(0, &saved) == 0) { raw = saved; raw.c_lflag &= (unsigned)~(ICANON | ECHO | ISIG); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0; tcsetattr(0, TCSANOW, &raw); }
    int state = 0, eof_in = 0, idle = 0, rc = 0;
    uint8_t buf[4096], out[4096];
    for (;;) {
        int n = toby_tcp_recv(conn, buf, sizeof buf);
        if (n > 0) { int o = telnet_filter(conn, buf, n, &state, out); if (o) { fwrite(out, 1, (size_t)o, stdout); fflush(stdout); } idle = 0; continue; }
        if (n < 0) { printf("Connection closed by foreign host.\n"); break; }
        if (!eof_in) {
            if (tty) {
                uint8_t c;
                ssize_t r = read(0, &c, 1);
                if (r == 1) {
                    if (c == 0x1D) { /* ^] */ tcsetattr(0, TCSANOW, &saved); printf("\ntelnet> "); fflush(stdout); char cmd[64]; if (!fgets(cmd, sizeof cmd, stdin) || !strncmp(cmd, "q", 1) || !strncmp(cmd, "close", 5)) { printf("Connection closed.\n"); break; } tcsetattr(0, TCSANOW, &raw); continue; }
                    uint8_t o2[2] = { c, '\n' };
                    if (c == '\n' || c == '\r') { o2[0] = '\r'; o2[1] = '\n'; toby_tcp_send(conn, o2, 2); }
                    else if (c == IAC) { o2[1] = IAC; toby_tcp_send(conn, o2, 2); }
                    else toby_tcp_send(conn, &c, 1);
                    continue;
                }
                if (r == 0) { usleep(20000); }
                else eof_in = 1;
            } else {
                char line[4096];
                if (fgets(line, sizeof line, stdin)) {
                    size_t l = strlen(line);
                    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
                    line[l++] = '\r'; line[l++] = '\n'; line[l] = '\0';   /* NVT line ending */
                    if (toby_tcp_send(conn, line, l) < 0) { rc = 1; break; }
                    continue;
                }
                eof_in = 1;
            }
        }
        if (eof_in && ++idle >= 40) { /* ~4 s of silence after EOF: done */ printf("Connection closed by foreign host.\n"); break; }
        usleep(100000);
    }
    if (tty) tcsetattr(0, TCSANOW, &saved);
    toby_tcp_close(conn);
    return rc;
}

/* ============================================================== dispatch */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "arp",        cmd_arp },
    { "arping",     cmd_arping },
    { "ip",         cmd_ip },
    { "netstat",    cmd_netstat },
    { "route",      cmd_route },
    { "telnet",     cmd_telnet },
    { "traceroute", cmd_traceroute },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "netutils";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    g_prog = base;
    for (int i = 0; g_applets[i].name; i++)
        if (!strcmp(base, g_applets[i].name))
            return g_applets[i].fn(argc, argv);
    if (argc > 1) {
        for (int i = 0; g_applets[i].name; i++) {
            if (strcmp(argv[1], g_applets[i].name)) continue;
            g_prog = g_applets[i].name;
            return g_applets[i].fn(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "netutils: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
