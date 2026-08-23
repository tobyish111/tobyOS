/* linux-ipv6: AF_INET6 UDP datagram sockets + ::1 loopback (slice 1).
 *
 * Before this slice socket(AF_INET6, ...) was a blanket EAFNOSUPPORT and
 * every v6 datagram that was not DHCPv6 port 546 was silently dropped in
 * the demux -- the v6 substrate (SLAAC, NDP, send/recv) existed for months
 * with no socket that could reach it. This proves the new socket layer by
 * VALUE: payloads echo intact over ::1, sources come back as real v6
 * addresses, and the dual-stack v4-mapped spelling works in both
 * directions.
 *
 * Bits (want 63):
 *   bit0  socket(AF_INET6, SOCK_DGRAM) opens; SOCK_STREAM is still the
 *         getaddrinfo-load-bearing EAFNOSUPPORT (TCP6 is the next slice)
 *   bit1  ::1 echo round trip, BOTH directions, payload + source verified
 *   bit2  getsockname: bound port round-trips; a connect()ed socket
 *         reports source ::1, not :: (glibc's RFC 3484 discovery probe)
 *   bit3  connected-UDP: send()/recv()/write() byte paths + getpeername
 *   bit4  dual-stack: v6 socket -> ::ffff:127.0.0.1 lands on a v4 socket;
 *         the v4 reply comes back spelled ::ffff:127.0.0.1
 *   bit5  1000-byte patterned payload intact; AF_INET sockaddr on a v6
 *         socket refused; recv on a silent port times out with EAGAIN
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
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

static int mksock6(uint16_t bind_port) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    set_rcvto(fd, 2000);
    if (bind_port) {
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof sa);
        sa.sin6_family = AF_INET6;
        sa.sin6_port   = htons(bind_port);
        sa.sin6_addr   = in6addr_any;
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void sin6_lo(struct sockaddr_in6 *sa, uint16_t port) {
    memset(sa, 0, sizeof *sa);
    sa->sin6_family = AF_INET6;
    sa->sin6_port   = htons(port);
    sa->sin6_addr   = in6addr_loopback;
}

int main(void) {
    printf("linux-ipv6: AF_INET6 UDP + ::1 loopback\n");

    /* bit0: DGRAM opens, STREAM is EAFNOSUPPORT (not EINVAL -- glibc/musl
     * getaddrinfo only falls back to v4 on exactly that errno). */
    {
        int d = socket(AF_INET6, SOCK_DGRAM, 0);
        errno = 0;
        int t = socket(AF_INET6, SOCK_STREAM, 0);
        int terr = errno;
        bit(0, d >= 0 && t < 0 && terr == EAFNOSUPPORT,
            "socket(): DGRAM opens, STREAM EAFNOSUPPORT");
        if (d >= 0) close(d);
        if (t >= 0) close(t);
    }

    /* bit1: full ::1 echo. srv on 47601; cli sends "ping6-payload", srv
     * verifies payload AND that the source is ::1 with the client's real
     * ephemeral port, echoes "pong6-reply" back to exactly that source,
     * cli verifies payload AND source ::1:47601. */
    {
        int srv = mksock6(47601);
        int cli = mksock6(0);
        int step = 0;
        if (srv >= 0 && cli >= 0) {
            struct sockaddr_in6 dst; sin6_lo(&dst, 47601);
            const char ping[] = "ping6-payload";
            if (sendto(cli, ping, sizeof ping, 0,
                       (struct sockaddr *)&dst, sizeof dst) == sizeof ping)
                step = 1;
            char buf[64];
            struct sockaddr_in6 from; socklen_t fl = sizeof from;
            memset(&from, 0, sizeof from);
            long n = recvfrom(srv, buf, sizeof buf, 0,
                              (struct sockaddr *)&from, &fl);
            if (step == 1 && n == sizeof ping && memcmp(buf, ping, n) == 0 &&
                from.sin6_family == AF_INET6 && ntohs(from.sin6_port) != 0 &&
                memcmp(&from.sin6_addr, &in6addr_loopback, 16) == 0)
                step = 2;
            const char pong[] = "pong6-reply";
            if (step == 2 && sendto(srv, pong, sizeof pong, 0,
                                    (struct sockaddr *)&from,
                                    sizeof from) == sizeof pong)
                step = 3;
            struct sockaddr_in6 f2; socklen_t f2l = sizeof f2;
            memset(&f2, 0, sizeof f2);
            long m = recvfrom(cli, buf, sizeof buf, 0,
                              (struct sockaddr *)&f2, &f2l);
            if (step == 3 && m == sizeof pong && memcmp(buf, pong, m) == 0 &&
                f2.sin6_family == AF_INET6 && ntohs(f2.sin6_port) == 47601 &&
                memcmp(&f2.sin6_addr, &in6addr_loopback, 16) == 0)
                step = 4;
        }
        bit(1, step == 4, "::1 echo both ways, payload+source checked");
        if (srv >= 0) close(srv);
        if (cli >= 0) close(cli);
    }

    /* bit2: getsockname. A bound socket round-trips its port; a socket
     * connect()ed to ::1 reports source ::1 -- glibc's RFC 3484 sort
     * reads "::" there as "no route" and re-resolves on a 3 s cadence. */
    {
        int a = mksock6(47602);
        int b = mksock6(0);
        int okA = 0, okB = 0;
        if (a >= 0) {
            struct sockaddr_in6 nm; socklen_t nl = sizeof nm;
            memset(&nm, 0, sizeof nm);
            okA = getsockname(a, (struct sockaddr *)&nm, &nl) == 0 &&
                  nm.sin6_family == AF_INET6 && ntohs(nm.sin6_port) == 47602;
        }
        if (b >= 0) {
            struct sockaddr_in6 dst; sin6_lo(&dst, 53);
            if (connect(b, (struct sockaddr *)&dst, sizeof dst) == 0) {
                struct sockaddr_in6 nm; socklen_t nl = sizeof nm;
                memset(&nm, 0, sizeof nm);
                okB = getsockname(b, (struct sockaddr *)&nm, &nl) == 0 &&
                      nm.sin6_family == AF_INET6 &&
                      memcmp(&nm.sin6_addr, &in6addr_loopback, 16) == 0;
            }
        }
        bit(2, okA && okB, "getsockname: port + RFC3484 source ::1");
        if (a >= 0) close(a);
        if (b >= 0) close(b);
    }

    /* bit3: connected-UDP byte paths. connect() then plain send() AND
     * plain write() both land on the peer; getpeername names ::1:47603. */
    {
        int srv = mksock6(47603);
        int cli = mksock6(0);
        int step = 0;
        if (srv >= 0 && cli >= 0) {
            struct sockaddr_in6 dst; sin6_lo(&dst, 47603);
            if (connect(cli, (struct sockaddr *)&dst, sizeof dst) == 0)
                step = 1;
            struct sockaddr_in6 pn; socklen_t pl = sizeof pn;
            memset(&pn, 0, sizeof pn);
            if (step == 1 && getpeername(cli, (struct sockaddr *)&pn, &pl) == 0 &&
                pn.sin6_family == AF_INET6 && ntohs(pn.sin6_port) == 47603 &&
                memcmp(&pn.sin6_addr, &in6addr_loopback, 16) == 0)
                step = 2;
            const char via_send[]  = "conn6-send";
            const char via_write[] = "conn6-write";
            char buf[64];
            if (step == 2 &&
                send(cli, via_send, sizeof via_send, 0) == sizeof via_send &&
                recv(srv, buf, sizeof buf, 0) == sizeof via_send &&
                memcmp(buf, via_send, sizeof via_send) == 0)
                step = 3;
            if (step == 3 &&
                write(cli, via_write, sizeof via_write) == sizeof via_write &&
                recv(srv, buf, sizeof buf, 0) == sizeof via_write &&
                memcmp(buf, via_write, sizeof via_write) == 0)
                step = 4;
        }
        bit(3, step == 4, "connected: send()+write() paths, getpeername");
        if (srv >= 0) close(srv);
        if (cli >= 0) close(cli);
    }

    /* bit4: dual-stack. A v6 sendto ::ffff:127.0.0.1 must ride the v4
     * path into a plain AF_INET socket; the v4 reply must come back on
     * the v6 socket spelled as the mapped address. */
    {
        int v4 = socket(AF_INET, SOCK_DGRAM, 0);
        int v6 = mksock6(0);
        int step = 0;
        if (v4 >= 0 && v6 >= 0) {
            set_rcvto(v4, 2000);
            struct sockaddr_in b4;
            memset(&b4, 0, sizeof b4);
            b4.sin_family = AF_INET;
            b4.sin_port   = htons(47604);
            b4.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(v4, (struct sockaddr *)&b4, sizeof b4) == 0)
                step = 1;
            struct sockaddr_in6 dst;
            memset(&dst, 0, sizeof dst);
            dst.sin6_family = AF_INET6;
            dst.sin6_port   = htons(47604);
            inet_pton(AF_INET6, "::ffff:127.0.0.1", &dst.sin6_addr);
            const char m1[] = "mapped6to4";
            if (step == 1 && sendto(v6, m1, sizeof m1, 0,
                                    (struct sockaddr *)&dst,
                                    sizeof dst) == sizeof m1)
                step = 2;
            char buf[64];
            struct sockaddr_in from4; socklen_t f4l = sizeof from4;
            memset(&from4, 0, sizeof from4);
            long n = recvfrom(v4, buf, sizeof buf, 0,
                              (struct sockaddr *)&from4, &f4l);
            if (step == 2 && n == sizeof m1 && memcmp(buf, m1, n) == 0 &&
                from4.sin_family == AF_INET && ntohs(from4.sin_port) != 0)
                step = 3;
            const char m2[] = "reply4to6";
            if (step == 3 && sendto(v4, m2, sizeof m2, 0,
                                    (struct sockaddr *)&from4,
                                    sizeof from4) == sizeof m2)
                step = 4;
            struct sockaddr_in6 from6; socklen_t f6l = sizeof from6;
            memset(&from6, 0, sizeof from6);
            long m = recvfrom(v6, buf, sizeof buf, 0,
                              (struct sockaddr *)&from6, &f6l);
            struct in6_addr want;
            inet_pton(AF_INET6, "::ffff:127.0.0.1", &want);
            if (step == 4 && m == sizeof m2 && memcmp(buf, m2, m) == 0 &&
                from6.sin6_family == AF_INET6 &&
                memcmp(&from6.sin6_addr, &want, 16) == 0)
                step = 5;
        }
        bit(4, step == 5, "dual-stack: mapped out via v4, back as ::ffff");
        if (v4 >= 0) close(v4);
        if (v6 >= 0) close(v6);
    }

    /* bit5: integrity + refusals. A 1000-byte patterned datagram arrives
     * byte-identical (v6 UDP checksums are computed over the real
     * pseudo-header, so any header lie corrupts nothing silently -- the
     * length/copy paths are what this catches). A v4 sockaddr on a v6
     * socket is refused, and a silent port times out with EAGAIN instead
     * of inventing data. */
    {
        int srv = mksock6(47605);
        int cli = mksock6(0);
        int step = 0;
        if (srv >= 0 && cli >= 0) {
            static char big[1000], got[1200];
            for (int i = 0; i < 1000; i++) big[i] = (char)(i * 7 + 3);
            struct sockaddr_in6 dst; sin6_lo(&dst, 47605);
            if (sendto(cli, big, sizeof big, 0,
                       (struct sockaddr *)&dst, sizeof dst) == sizeof big)
                step = 1;
            long n = recv(srv, got, sizeof got, 0);
            if (step == 1 && n == sizeof big &&
                memcmp(got, big, sizeof big) == 0)
                step = 2;
            struct sockaddr_in wrong;
            memset(&wrong, 0, sizeof wrong);
            wrong.sin_family = AF_INET;
            wrong.sin_port   = htons(47605);
            wrong.sin_addr.s_addr = htonl(0x7f000001);
            errno = 0;
            if (step == 2 &&
                sendto(cli, "x", 1, 0, (struct sockaddr *)&wrong,
                       sizeof wrong) < 0)
                step = 3;
            int quiet = mksock6(47606);
            if (step == 3 && quiet >= 0) {
                set_rcvto(quiet, 300);
                char b2[8];
                errno = 0;
                long q = recv(quiet, b2, sizeof b2, 0);
                if (q < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    step = 4;
                close(quiet);
            }
        }
        bit(5, step == 4, "1000B intact; v4 addr refused; timeout EAGAIN");
        if (srv >= 0) close(srv);
        if (cli >= 0) close(cli);
    }

    printf("linux-ipv6: RESULT=%d (want 63)\n", ok_all);
    return ok_all;      /* exit code IS the contract the harness checks */
}
