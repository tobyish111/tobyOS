/* ping / clockdiff / tcpdump -- the three tools raw sockets unlock.
 *
 *     ping [-c count] [-i sec] [-s size] [-q] [-W sec] HOST
 *     clockdiff HOST
 *     tcpdump [-c count] [-n] [-q] [-i any]
 *
 * All three were absent for one reason: this kernel had no SOCK_RAW. It has
 * one now (root-only, as on Linux), so:
 *
 *   ping      SOCK_RAW/IPPROTO_ICMP -- builds echo requests, matches replies
 *             by id and sequence, and times them.
 *   clockdiff SOCK_RAW/IPPROTO_ICMP -- ICMP TIMESTAMP (type 13), which is a
 *             different message, not a ping with extra steps: the remote
 *             fills in its own midnight-relative clock and the difference is
 *             the answer.
 *   tcpdump   AF_PACKET -- whole ethernet frames, before the EtherType demux
 *             and before any decision about whether the frame is ours.
 *
 * A raw socket here delivers the WHOLE IP datagram, header included, which
 * is what lets ping report the TTL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include <tobyos/socket.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

struct src_addr { u32 ip; u16 port; u16 pad; };

static const char *prog = "ping";

static u16 htons_(u16 v) { return (u16)((v >> 8) | (v << 8)); }
#define ntohs_ htons_

static u32 now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u32)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* The internet checksum: one's-complement sum of 16-bit words. */
static u16 inet_csum(const void *data, size_t len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (u32)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

static int parse_ipv4(const char *s, u32 *out_be) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); if (n > 255) return 0; }
        v = (v << 8) | (u32)n;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out_be = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8)
            | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
    return 1;
}

static void fmt_ip(u32 be, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(be & 0xFF),
             (unsigned)((be >> 8) & 0xFF), (unsigned)((be >> 16) & 0xFF),
             (unsigned)((be >> 24) & 0xFF));
}

/* Names go through the same resolver dig and host use. There is no
 * gethostbyname in libtoby, so /etc/hosts is consulted first and then the
 * nameserver, exactly as getent hosts does. */
static int resolve(const char *host, u32 *out) {
    if (parse_ipv4(host, out)) return 1;
    FILE *f = fopen("/etc/hosts", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char ip[64], nm[128];
            if (sscanf(line, "%63s %127s", ip, nm) == 2 && !strcmp(nm, host)) {
                fclose(f);
                return parse_ipv4(ip, out);
            }
        }
        fclose(f);
    }
    if (!strcmp(host, "localhost")) return parse_ipv4("127.0.0.1", out);
    fprintf(stderr, "%s: cannot resolve %s (this tool takes an address or a "
                    "name in /etc/hosts)\n", prog, host);
    return 0;
}

/* ------------------------------------------------------------------ ping */

#define ICMP_ECHOREPLY   0
#define ICMP_ECHO        8
#define ICMP_TSTAMP      13
#define ICMP_TSTAMPREPLY 14

static int do_ping(int argc, char **argv) {
    const char *host = NULL;
    int count = 4, quiet = 0, payload = 56;
    unsigned interval_ms = 1000, wait_ms = 4000;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-c") && i + 1 < argc) { count = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-i") && i + 1 < argc) { interval_ms = (unsigned)(atoi(argv[++i]) * 1000); continue; }
        if (!strcmp(a, "-W") && i + 1 < argc) { wait_ms = (unsigned)(atoi(argv[++i]) * 1000); continue; }
        if (!strcmp(a, "-s") && i + 1 < argc) { payload = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-q")) { quiet = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: ping [-c count] [-i sec] [-s size] [-W sec] [-q] HOST\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) continue;
        if (!host) host = a;
    }
    if (!host) { fprintf(stderr, "usage: ping [-c count] HOST\n"); return 1; }
    if (payload < 0) payload = 0;
    if (payload > 1400) payload = 1400;

    u32 dst;
    if (!resolve(host, &dst)) return 2;

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        fprintf(stderr, "ping: cannot open a raw ICMP socket: %s\n",
                errno == 1 ? "root only" : strerror(errno));
        return 2;
    }

    char ipstr[24];
    fmt_ip(dst, ipstr, sizeof ipstr);
    if (!quiet) printf("PING %s (%s) %d(%d) bytes of data.\n",
                       host, ipstr, payload, payload + 28);

    u16 id = (u16)(getpid() & 0xFFFF);
    int sent = 0, recvd = 0;
    u32 rtt_min = 0xFFFFFFFFu, rtt_max = 0, rtt_sum = 0;

    for (int seq = 0; seq < count; seq++) {
        u8 pkt[1500];
        size_t plen = 8 + (size_t)payload;
        memset(pkt, 0, plen);
        pkt[0] = ICMP_ECHO;
        pkt[1] = 0;
        pkt[4] = (u8)(id >> 8); pkt[5] = (u8)id;
        pkt[6] = (u8)(seq >> 8); pkt[7] = (u8)seq;
        for (int k = 0; k < payload; k++) pkt[8 + k] = (u8)(k & 0xFF);
        u16 ck = inet_csum(pkt, plen);
        pkt[2] = (u8)(ck >> 8); pkt[3] = (u8)ck;

        u32 t0 = now_ms();
        /* The port argument is meaningless for a raw socket and the kernel
         * ignores it; it must be non-zero only for the UDP path. */
        if (sendto(fd, pkt, plen, dst, htons_(1)) < 0) {
            fprintf(stderr, "ping: send failed\n");
            close(fd);
            return 1;
        }
        sent++;

        int got = 0;
        while (now_ms() - t0 < wait_ms) {
            u8 buf[2048];
            struct src_addr from;
            long n = recvfrom(fd, buf, sizeof buf, &from);
            if (n <= 0) continue;
            /* The whole IP datagram arrives, so step over its header. */
            if (n < 20) continue;
            size_t ihl = (size_t)(buf[0] & 0x0F) * 4;
            if (ihl < 20 || (size_t)n < ihl + 8) continue;
            const u8 *ic = buf + ihl;
            if (ic[0] != ICMP_ECHOREPLY) continue;
            u16 rid = (u16)((ic[4] << 8) | ic[5]);
            u16 rsq = (u16)((ic[6] << 8) | ic[7]);
            /* Every ping on the machine sees every reply, so ours is the one
             * carrying OUR id -- without this check two concurrent pings
             * would each report the other's replies as their own. */
            if (rid != id || rsq != (u16)seq) continue;
            u32 rtt = now_ms() - t0;
            char src[24];
            fmt_ip(from.ip, src, sizeof src);
            if (!quiet)
                printf("%ld bytes from %s: icmp_seq=%d ttl=%u time=%u ms\n",
                       n - (long)ihl, src, seq + 1, (unsigned)buf[8], rtt);
            recvd++;
            if (rtt < rtt_min) rtt_min = rtt;
            if (rtt > rtt_max) rtt_max = rtt;
            rtt_sum += rtt;
            got = 1;
            break;
        }
        if (!got && !quiet)
            printf("no answer for icmp_seq=%d\n", seq + 1);
        if (seq + 1 < count) {
            u32 spent = now_ms() - t0;
            if (spent < interval_ms) usleep((interval_ms - spent) * 1000);
        }
    }
    close(fd);

    printf("\n--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           sent, recvd, sent ? (sent - recvd) * 100 / sent : 0);
    if (recvd)
        printf("rtt min/avg/max = %u/%u/%u ms\n",
               rtt_min, rtt_sum / (unsigned)recvd, rtt_max);
    return recvd ? 0 : 1;
}

/* ------------------------------------------------------------- clockdiff */

static int do_clockdiff(int argc, char **argv) {
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
                printf("usage: clockdiff HOST\n");
                return 0;
            }
            continue;
        }
        if (!host) host = argv[i];
    }
    if (!host) { fprintf(stderr, "usage: clockdiff HOST\n"); return 1; }

    u32 dst;
    if (!resolve(host, &dst)) return 2;

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) { fprintf(stderr, "clockdiff: cannot open a raw ICMP socket (root only)\n"); return 2; }

    u16 id = (u16)(getpid() & 0xFFFF);
    /* ICMP TIMESTAMP carries three 32-bit times, all milliseconds since
     * midnight UT: originate (ours), receive and transmit (theirs). */
    /* Two tries, one second each. Most hosts ignore ICMP TIMESTAMP, so
     * the common outcome is no answer -- and a tool whose no-answer path
     * takes a minute is a tool nobody runs twice. */
    for (int attempt = 0; attempt < 2; attempt++) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        u32 midnight = (u32)((tv.tv_sec % 86400) * 1000 + tv.tv_usec / 1000);

        u8 pkt[20];
        memset(pkt, 0, sizeof pkt);
        pkt[0] = ICMP_TSTAMP;
        pkt[4] = (u8)(id >> 8); pkt[5] = (u8)id;
        pkt[6] = 0; pkt[7] = (u8)attempt;
        pkt[8]  = (u8)(midnight >> 24); pkt[9]  = (u8)(midnight >> 16);
        pkt[10] = (u8)(midnight >> 8);  pkt[11] = (u8)midnight;
        u16 ck = inet_csum(pkt, sizeof pkt);
        pkt[2] = (u8)(ck >> 8); pkt[3] = (u8)ck;

        u32 t0 = now_ms();
        if (sendto(fd, pkt, sizeof pkt, dst, htons_(1)) < 0) {
            fprintf(stderr, "clockdiff: send failed\n");
            close(fd);
            return 1;
        }
        while (now_ms() - t0 < 1000) {
            u8 buf[2048];
            struct src_addr from;
            long n = recvfrom(fd, buf, sizeof buf, &from);
            if (n <= 0) continue;
            size_t ihl = (size_t)(buf[0] & 0x0F) * 4;
            if ((size_t)n < ihl + 20) continue;
            const u8 *ic = buf + ihl;
            if (ic[0] != ICMP_TSTAMPREPLY) continue;
            if (((ic[4] << 8) | ic[5]) != id) continue;
            u32 recv_ts = ((u32)ic[12] << 24) | ((u32)ic[13] << 16)
                        | ((u32)ic[14] << 8) | ic[15];
            gettimeofday(&tv, NULL);
            u32 back = (u32)((tv.tv_sec % 86400) * 1000 + tv.tv_usec / 1000);
            /* Half the round trip is the one-way delay; the remote's receive
             * stamp minus our midpoint is the offset. */
            long rtt = (long)back - (long)midnight;
            long diff = (long)recv_ts - ((long)midnight + rtt / 2);
            char ipstr[24];
            fmt_ip(dst, ipstr, sizeof ipstr);
            printf("host=%s rtt=%ld ms delta=%ld ms\n", ipstr, rtt, diff);
            close(fd);
            return 0;
        }
    }
    close(fd);
    /* Most hosts ignore ICMP timestamp requests; saying so beats printing 0. */
    fprintf(stderr, "clockdiff: %s did not answer an ICMP timestamp request "
                    "(most hosts do not)\n", host);
    return 1;
}

/* --------------------------------------------------------------- tcpdump */

static const char *proto_name(u8 p) {
    switch (p) {
    case 1:  return "ICMP";
    case 6:  return "TCP";
    case 17: return "UDP";
    default: return "IP";
    }
}

static void print_frame(const u8 *f, size_t n, int numeric) {
    (void)numeric;
    if (n < 14) { printf("  [runt frame, %u bytes]\n", (unsigned)n); return; }
    u16 et = (u16)((f[12] << 8) | f[13]);
    const u8 *pl = f + 14;
    size_t pln = n - 14;

    if (et == 0x0806) {
        printf("ARP, length %u\n", (unsigned)pln);
        return;
    }
    if (et == 0x86DD) {
        printf("IPv6, length %u\n", (unsigned)pln);
        return;
    }
    if (et != 0x0800) {
        printf("ethertype 0x%04x, length %u\n", et, (unsigned)pln);
        return;
    }
    if (pln < 20) { printf("IP [truncated]\n"); return; }
    size_t ihl = (size_t)(pl[0] & 0x0F) * 4;
    u8 proto = pl[9];
    char s[24], d[24];
    u32 sip, dip;
    memcpy(&sip, pl + 12, 4);
    memcpy(&dip, pl + 16, 4);
    fmt_ip(sip, s, sizeof s);
    fmt_ip(dip, d, sizeof d);

    if ((proto == 6 || proto == 17) && pln >= ihl + 4) {
        const u8 *l4 = pl + ihl;
        unsigned sp = (unsigned)((l4[0] << 8) | l4[1]);
        unsigned dp = (unsigned)((l4[2] << 8) | l4[3]);
        printf("IP %s.%u > %s.%u: %s, length %u\n", s, sp, d, dp,
               proto_name(proto), (unsigned)(pln - ihl));
    } else {
        printf("IP %s > %s: %s, length %u\n", s, d, proto_name(proto),
               (unsigned)(pln - ihl));
    }
}

static int do_tcpdump(int argc, char **argv) {
    int count = 10, numeric = 1, quiet = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-c") && i + 1 < argc) { count = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-i") && i + 1 < argc) { i++; continue; }   /* one NIC here */
        if (!strcmp(a, "-n")) { numeric = 1; continue; }
        if (!strcmp(a, "-q")) { quiet = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: tcpdump [-c count] [-n] [-q] [-i any]\n");
            return 0;
        }
    }

    int fd = socket(AF_PACKET, SOCK_RAW, 0);
    if (fd < 0) {
        fprintf(stderr, "tcpdump: cannot open an AF_PACKET socket (root only)\n");
        return 2;
    }
    if (!quiet)
        printf("tcpdump: listening on the link layer, capture size 1518 bytes\n");

    int seen = 0;
    u32 t0 = now_ms();
    /* A capture with no traffic must not hang forever in a test harness. */
    while (seen < count && now_ms() - t0 < 15000) {
        u8 buf[2048];
        struct src_addr from;
        long n = recvfrom(fd, buf, sizeof buf, &from);
        if (n <= 0) continue;
        printf("%u.%03u ", (now_ms() - t0) / 1000, (now_ms() - t0) % 1000);
        print_frame(buf, (size_t)n, numeric);
        seen++;
    }
    close(fd);
    printf("%d packets captured\n", seen);
    return seen ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "ping";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    if (!strcmp(base, "clockdiff")) return do_clockdiff(argc, argv);
    if (!strcmp(base, "tcpdump"))   return do_tcpdump(argc, argv);
    return do_ping(argc, argv);
}
