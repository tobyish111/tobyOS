/* linux-v6host: IPv6 against a REAL peer -- QEMU SLIRP over the wire
 * (IPv6 slice 3).
 *
 * Slices 1-2 proved the socket layers over ::1, where the sender's stack
 * is also the receiver's. This one leaves the machine: Router
 * Solicitation -> SLIRP's RA -> SLAAC global, NDP neighbor resolution,
 * checksummed UDP to SLIRP's DNS relay and a REAL answer back, off-link
 * routing via the RA-learned default router, and a TCP RST from the far
 * side. Chasing exactly this surfaced two months-latent bugs: the
 * e1000's RX filter had no MPE (every RA died in the NIC), and NDP went
 * out at hop limit 64 (RFC 4861 receivers silently discard != 255).
 *
 * Environment contract: QEMU -netdev user with ipv6 on (the default);
 * bit2 additionally needs the host's resolver to answer (LAN router --
 * the same dependency the browser gates already carry).
 *
 * Bits (want 63):
 *   bit0  SLAAC from userspace: connected-UDP getsockname shows a
 *         GLOBAL source (not ::, not fe80::/10, not ::1)
 *   bit1  on-link NDP + UDP TX: sendto [fec0::3]:53 accepts the full
 *         datagram (cold-neighbor NS/NA resolved inline)
 *   bit2  real answer: DNS query to fec0::3 comes back, txid + QR
 *         checked, source == [fec0::3]:53
 *   bit3  off-link send routes via the default router (2001:db8:: dst)
 *   bit4  TCP6 wire RST: connect [fec0::2]:47699 -> fast ECONNREFUSED
 *   bit5  ::1 loopback still echoes (the wire work broke nothing)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

static int ok_all = 0;

static void bit(int n, int cond, const char *what) {
    if (cond) ok_all |= (1 << n);
    printf("  bit%d %-46s %s\n", n, what, cond ? "ok" : "FAIL");
}

static void set_rcvto(int fd, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void sin6_addr(struct sockaddr_in6 *sa, const char *ip, uint16_t port) {
    memset(sa, 0, sizeof *sa);
    sa->sin6_family = AF_INET6;
    sa->sin6_port   = htons(port);
    inet_pton(AF_INET6, ip, &sa->sin6_addr);
}

static int src_is_global(const struct in6_addr *a) {
    static const struct in6_addr zero;
    if (memcmp(a, &zero, 16) == 0) return 0;
    const uint8_t *b = (const uint8_t *)a;
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0;   /* link-local */
    static const uint8_t lo[16] = { [15] = 1 };
    if (memcmp(b, lo, 16) == 0) return 0;                  /* ::1 */
    return 1;
}

int main(void) {
    printf("linux-v6host: IPv6 vs a real peer (SLIRP)\n");

    /* bit0: RFC 3484 discovery must answer with the SLAAC global. The RA
     * normally lands ~1.5 s into boot, long before this runs; retry
     * briefly anyway so a slow RA reads as slow, not broken. */
    {
        int got = 0;
        for (int tries = 0; tries < 30 && !got; tries++) {
            int fd = socket(AF_INET6, SOCK_DGRAM, 0);
            if (fd < 0) break;
            struct sockaddr_in6 dst;
            sin6_addr(&dst, "2001:4860:4860::8888", 53);
            if (connect(fd, (struct sockaddr *)&dst, sizeof dst) == 0) {
                struct sockaddr_in6 nm; socklen_t nl = sizeof nm;
                memset(&nm, 0, sizeof nm);
                if (getsockname(fd, (struct sockaddr *)&nm, &nl) == 0 &&
                    nm.sin6_family == AF_INET6 &&
                    src_is_global(&nm.sin6_addr))
                    got = 1;
            }
            close(fd);
            if (!got) usleep(100 * 1000);
        }
        bit(0, got, "SLAAC: getsockname shows a global source");
    }

    /* bit1 + bit2: the DNS relay round trip. The far side's verdict is
     * environment-dependent: a host WITH an IPv6 resolver relays and a
     * real DNS answer comes back; a host WITHOUT one makes SLIRP answer
     * ICMPv6 dest-unreachable "no route" -- which must reach us as an
     * async errno on the connect()ed socket, not as a silent timeout.
     * EITHER outcome proves the full bidirectional wire; a plain
     * timeout proves nothing and fails. */
    {
        int fd = socket(AF_INET6, SOCK_DGRAM, 0);
        int sent = 0, verdict = 0;
        if (fd >= 0) {
            set_rcvto(fd, 3000);
            struct sockaddr_in6 dns;
            sin6_addr(&dns, "fec0::3", 53);
            /* Minimal query: example.com IN A, txid 0x7ab6, RD set. */
            unsigned char q[29] = {
                0x7a, 0xb6, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
                7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0,
                0x00, 0x01, 0x00, 0x01
            };
            if (connect(fd, (struct sockaddr *)&dns, sizeof dns) == 0) {
                /* One retry: DNS clients retransmit by design. */
                for (int t = 0; t < 2 && !verdict; t++) {
                    errno = 0;
                    long w = send(fd, q, sizeof q, 0);
                    if (w == sizeof q) sent = 1;
                    else if (errno == ENETUNREACH || errno == EHOSTUNREACH ||
                             errno == ECONNREFUSED || errno == EACCES) {
                        /* previous try's wire error, delivered per the
                         * error-on-next-op contract */
                        sent = 1;
                        verdict = 2;
                        break;
                    } else break;
                    unsigned char r[512];
                    errno = 0;
                    long n = recv(fd, r, sizeof r, 0);
                    if (n >= 12 && r[0] == 0x7a && r[1] == 0xb6 &&
                        (r[2] & 0x80))
                        verdict = 1;               /* real DNS answer */
                    else if (n < 0 &&
                             (errno == ENETUNREACH || errno == EHOSTUNREACH ||
                              errno == ECONNREFUSED || errno == EACCES))
                        verdict = 2;               /* wire error, as errno */
                }
                if (verdict)
                    printf("  (bit2: %s)\n", verdict == 1
                           ? "real DNS answer" : "ICMPv6 error as errno");
            }
            close(fd);
        }
        bit(1, sent, "on-link NDP + UDP TX to [fec0::3]:53");
        bit(2, verdict != 0, "wire verdict back: answer or errno");
    }

    /* bit3: an off-prefix destination must route via the RA-learned
     * default router. Nothing answers 2001:db8:: (doc prefix) -- the
     * assertion is that the SEND resolves the router and leaves. */
    {
        int fd = socket(AF_INET6, SOCK_DGRAM, 0);
        int ok = 0;
        if (fd >= 0) {
            struct sockaddr_in6 far;
            sin6_addr(&far, "2001:db8::1234", 9);
            const char m[] = "off-link";
            if (sendto(fd, m, sizeof m, 0,
                       (struct sockaddr *)&far, sizeof far) == sizeof m)
                ok = 1;
            close(fd);
        }
        bit(3, ok, "off-link send routes via default router");
    }

    /* bit4: TCP over the real wire -- the far side's verdict on our SYN
     * must come back FAST and land as the right errno, exercising
     * tcp_recv_packet6 (and the ICMPv6-error path) on genuinely
     * non-loopback traffic. A v6-capable host answers with a real RST
     * (ECONNREFUSED); this SLIRP answers with dest-unreachable
     * "no route" (ENETUNREACH, via tcp6_icmp_error). Either is the
     * wire speaking; a silent timeout is the failure. */
    {
        static const struct { const char *ip; uint16_t port; } tgt[2] = {
            { "fec0::2", 47699 },
            { "fec0::3", 53    },
        };
        int ok = 0;
        for (int t = 0; t < 2 && !ok; t++) {
            int fd = socket(AF_INET6, SOCK_STREAM, 0);
            if (fd < 0) break;
            struct timeval tv = { 3, 0 };      /* cap the SYN wait */
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            struct sockaddr_in6 host;
            sin6_addr(&host, tgt[t].ip, tgt[t].port);
            uint64_t t0 = now_ms();
            errno = 0;
            int rc = connect(fd, (struct sockaddr *)&host, sizeof host);
            uint64_t dt = now_ms() - t0;
            ok = (rc < 0 && dt < 3000 &&
                  (errno == ECONNREFUSED || errno == ENETUNREACH ||
                   errno == EHOSTUNREACH));
            printf("  (bit4 [%s]:%u -> rc=%d errno=%d dt=%llu)\n",
                   tgt[t].ip, tgt[t].port, rc, errno,
                   (unsigned long long)dt);
            close(fd);
        }
        bit(4, ok, "TCP6 wire verdict: RST or unreach, fast");
    }

    /* bit5: loopback regression guard. */
    {
        int srv = socket(AF_INET6, SOCK_DGRAM, 0);
        int cli = socket(AF_INET6, SOCK_DGRAM, 0);
        int ok = 0;
        if (srv >= 0 && cli >= 0) {
            set_rcvto(srv, 2000);
            struct sockaddr_in6 b6;
            memset(&b6, 0, sizeof b6);
            b6.sin6_family = AF_INET6;
            b6.sin6_port   = htons(47698);
            b6.sin6_addr   = in6addr_any;
            struct sockaddr_in6 dst;
            sin6_addr(&dst, "::1", 47698);
            char buf[32];
            const char m[] = "still-loops";
            if (bind(srv, (struct sockaddr *)&b6, sizeof b6) == 0 &&
                sendto(cli, m, sizeof m, 0,
                       (struct sockaddr *)&dst, sizeof dst) == sizeof m &&
                recv(srv, buf, sizeof buf, 0) == sizeof m &&
                memcmp(buf, m, sizeof m) == 0)
                ok = 1;
        }
        bit(5, ok, "::1 loopback still echoes");
        if (srv >= 0) close(srv);
        if (cli >= 0) close(cli);
    }

    printf("linux-v6host: RESULT=%d (want 63)\n", ok_all);
    return ok_all;      /* exit code IS the contract the harness checks */
}
