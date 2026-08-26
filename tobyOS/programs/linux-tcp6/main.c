/* linux-tcp6: TCP over IPv6 (IPv6 slice 2).
 *
 * The TCP engine is shared with v4 -- only the prologue (demux, checksum,
 * emit) learned a second family -- so this proves the v6-specific seams by
 * VALUE: ::1 handshakes, v6 endpoint reporting, both dual-stack directions
 * (v4 client into a v6 listener; v6 mapped connect into a v4 listener),
 * the nonblocking EINPROGRESS ladder, and stream integrity at 48 KiB.
 *
 * On loopback delivery is INLINE (sender's send IS the receiver's recv),
 * so a single process can connect, accept and pump a whole stream with no
 * fork -- the handshake completes inside connect().
 *
 * Bits (want 63):
 *   bit0  ::1 listen/connect/accept, echo BOTH directions, payload checked
 *   bit1  getsockname/getpeername: client peer ::1:PORT, real ephemeral
 *         local port, accepted-fd peer port == client's local port
 *   bit2  dual-stack in: AF_INET client -> v6 listener; accept reports
 *         ::ffff:127.0.0.1; bytes flow
 *   bit3  dual-stack out: v6 socket connect ::ffff:127.0.0.1 -> AF_INET
 *         listener; bytes flow; getpeername keeps the mapped spelling
 *   bit4  nonblocking connect: EINPROGRESS -> POLLOUT -> SO_ERROR 0; and
 *         a blocking connect to a silent ::1 port is a fast ECONNREFUSED
 *   bit5  48 KiB patterned stream arrives byte-identical
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

static int ok_all = 0;

static void bit(int n, int cond, const char *what) {
    if (cond) ok_all |= (1 << n);
    printf("  bit%d %-46s %s\n", n, what, cond ? "ok" : "FAIL");
}

static void sin6_lo(struct sockaddr_in6 *sa, uint16_t port) {
    memset(sa, 0, sizeof *sa);
    sa->sin6_family = AF_INET6;
    sa->sin6_port   = htons(port);
    sa->sin6_addr   = in6addr_loopback;
}

static int listen6(uint16_t port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port   = htons(port);
    sa.sin6_addr   = in6addr_any;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Pump exactly n bytes through a blocking fd pair. */
static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n > 4096 ? 4096 : n);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
static int recv_all(int fd, char *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

int main(void) {
    printf("linux-tcp6: TCP over IPv6\n");

    /* bit0 + bit1: the core ::1 conversation. */
    {
        int lsn = listen6(47611);
        int cli = socket(AF_INET6, SOCK_STREAM, 0);
        int acc = -1;
        int step = 0, names = 0;
        if (lsn >= 0 && cli >= 0) {
            struct sockaddr_in6 dst; sin6_lo(&dst, 47611);
            if (connect(cli, (struct sockaddr *)&dst, sizeof dst) == 0)
                step = 1;
            struct sockaddr_in6 pa; socklen_t pl = sizeof pa;
            memset(&pa, 0, sizeof pa);
            if (step == 1 &&
                (acc = accept(lsn, (struct sockaddr *)&pa, &pl)) >= 0)
                step = 2;
            char buf[64];
            const char c2s[] = "tcp6-up";
            const char s2c[] = "tcp6-down";
            if (step == 2 &&
                write(cli, c2s, sizeof c2s) == sizeof c2s &&
                read(acc, buf, sizeof buf) == sizeof c2s &&
                memcmp(buf, c2s, sizeof c2s) == 0)
                step = 3;
            if (step == 3 &&
                write(acc, s2c, sizeof s2c) == sizeof s2c &&
                read(cli, buf, sizeof buf) == sizeof s2c &&
                memcmp(buf, s2c, sizeof s2c) == 0)
                step = 4;

            /* bit1's evidence, gathered while the pair is live. */
            struct sockaddr_in6 pn, ln;
            socklen_t nl = sizeof pn;
            memset(&pn, 0, sizeof pn);
            int okp = getpeername(cli, (struct sockaddr *)&pn, &nl) == 0 &&
                      pn.sin6_family == AF_INET6 &&
                      ntohs(pn.sin6_port) == 47611 &&
                      memcmp(&pn.sin6_addr, &in6addr_loopback, 16) == 0;
            nl = sizeof ln;
            memset(&ln, 0, sizeof ln);
            int okl = getsockname(cli, (struct sockaddr *)&ln, &nl) == 0 &&
                      ln.sin6_family == AF_INET6 &&
                      ntohs(ln.sin6_port) != 0;
            /* the accept-time address must name the client's real port */
            int oka = pa.sin6_family == AF_INET6 &&
                      pa.sin6_port == ln.sin6_port;
            names = okp && okl && oka;
        }
        bit(0, step == 4, "::1 handshake + echo both directions");
        bit(1, step == 4 && names, "endpoint names: peer/local/accepted");
        if (acc >= 0) close(acc);
        if (cli >= 0) close(cli);
        if (lsn >= 0) close(lsn);
    }

    /* bit2: a plain AF_INET client lands on the v6 listener; the accepted
     * peer is spelled ::ffff:127.0.0.1. Linux's dual-stack default. */
    {
        int lsn = listen6(47612);
        int cli = socket(AF_INET, SOCK_STREAM, 0);
        int acc = -1;
        int step = 0;
        if (lsn >= 0 && cli >= 0) {
            struct sockaddr_in d4;
            memset(&d4, 0, sizeof d4);
            d4.sin_family = AF_INET;
            d4.sin_port   = htons(47612);
            d4.sin_addr.s_addr = htonl(0x7f000001);
            if (connect(cli, (struct sockaddr *)&d4, sizeof d4) == 0)
                step = 1;
            struct sockaddr_in6 pa; socklen_t pl = sizeof pa;
            memset(&pa, 0, sizeof pa);
            if (step == 1 &&
                (acc = accept(lsn, (struct sockaddr *)&pa, &pl)) >= 0) {
                struct in6_addr want;
                inet_pton(AF_INET6, "::ffff:127.0.0.1", &want);
                if (pa.sin6_family == AF_INET6 &&
                    memcmp(&pa.sin6_addr, &want, 16) == 0)
                    step = 2;
            }
            char buf[32];
            const char msg[] = "v4-into-v6";
            if (step == 2 &&
                write(cli, msg, sizeof msg) == sizeof msg &&
                read(acc, buf, sizeof buf) == sizeof msg &&
                memcmp(buf, msg, sizeof msg) == 0)
                step = 3;
        }
        bit(2, step == 3, "dual-stack in: v4 client, mapped accept addr");
        if (acc >= 0) close(acc);
        if (cli >= 0) close(cli);
        if (lsn >= 0) close(lsn);
    }

    /* bit3: the other direction -- a v6 socket dials ::ffff:127.0.0.1
     * into a plain AF_INET listener and keeps its mapped spelling. */
    {
        int lsn = socket(AF_INET, SOCK_STREAM, 0);
        int cli = socket(AF_INET6, SOCK_STREAM, 0);
        int acc = -1;
        int step = 0;
        if (lsn >= 0 && cli >= 0) {
            struct sockaddr_in b4;
            memset(&b4, 0, sizeof b4);
            b4.sin_family = AF_INET;
            b4.sin_port   = htons(47613);
            b4.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(lsn, (struct sockaddr *)&b4, sizeof b4) == 0 &&
                listen(lsn, 4) == 0)
                step = 1;
            struct sockaddr_in6 d6;
            memset(&d6, 0, sizeof d6);
            d6.sin6_family = AF_INET6;
            d6.sin6_port   = htons(47613);
            inet_pton(AF_INET6, "::ffff:127.0.0.1", &d6.sin6_addr);
            if (step == 1 &&
                connect(cli, (struct sockaddr *)&d6, sizeof d6) == 0)
                step = 2;
            if (step == 2 && (acc = accept(lsn, NULL, NULL)) >= 0)
                step = 3;
            char buf[32];
            const char msg[] = "v6-into-v4";
            if (step == 3 &&
                write(cli, msg, sizeof msg) == sizeof msg &&
                read(acc, buf, sizeof buf) == sizeof msg &&
                memcmp(buf, msg, sizeof msg) == 0)
                step = 4;
            struct sockaddr_in6 pn; socklen_t pl = sizeof pn;
            memset(&pn, 0, sizeof pn);
            struct in6_addr want;
            inet_pton(AF_INET6, "::ffff:127.0.0.1", &want);
            if (step == 4 &&
                getpeername(cli, (struct sockaddr *)&pn, &pl) == 0 &&
                pn.sin6_family == AF_INET6 &&
                ntohs(pn.sin6_port) == 47613 &&
                memcmp(&pn.sin6_addr, &want, 16) == 0)
                step = 5;
        }
        bit(3, step == 5, "dual-stack out: mapped connect to v4 listener");
        if (acc >= 0) close(acc);
        if (cli >= 0) close(cli);
        if (lsn >= 0) close(lsn);
    }

    /* bit4: the nonblocking ladder, then the refused classification. */
    {
        int lsn = listen6(47614);
        int step = 0;
        if (lsn >= 0) {
            int cli = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (cli >= 0) {
                struct sockaddr_in6 dst; sin6_lo(&dst, 47614);
                errno = 0;
                int rc = connect(cli, (struct sockaddr *)&dst, sizeof dst);
                if (rc == 0 || (rc < 0 && errno == EINPROGRESS))
                    step = 1;
                struct pollfd pf = { cli, POLLOUT, 0 };
                if (step == 1 && poll(&pf, 1, 2000) == 1 &&
                    (pf.revents & POLLOUT))
                    step = 2;
                int soerr = -1; socklen_t sl = sizeof soerr;
                if (step == 2 &&
                    getsockopt(cli, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
                    soerr == 0)
                    step = 3;
                close(cli);
            }
        }
        {
            int c2 = socket(AF_INET6, SOCK_STREAM, 0);
            if (step == 3 && c2 >= 0) {
                struct sockaddr_in6 dead; sin6_lo(&dead, 47615);
                errno = 0;
                if (connect(c2, (struct sockaddr *)&dead, sizeof dead) < 0 &&
                    errno == ECONNREFUSED)
                    step = 4;
            }
            if (c2 >= 0) close(c2);
        }
        bit(4, step == 4, "EINPROGRESS->POLLOUT->SO_ERROR 0; RST refused");
        if (lsn >= 0) close(lsn);
    }

    /* bit5: 48 KiB of pattern through the stream, byte-identical. Stays
     * under the 64 KiB receive ring so the single-process pump (inline
     * loopback delivery) never deadlocks on a full window. */
    {
        enum { N = 48 * 1024 };
        static char tx[N], rx[N];
        for (int i = 0; i < N; i++) tx[i] = (char)(i * 131 + 17);
        int lsn = listen6(47616);
        int cli = socket(AF_INET6, SOCK_STREAM, 0);
        int acc = -1;
        int step = 0;
        if (lsn >= 0 && cli >= 0) {
            struct sockaddr_in6 dst; sin6_lo(&dst, 47616);
            if (connect(cli, (struct sockaddr *)&dst, sizeof dst) == 0 &&
                (acc = accept(lsn, NULL, NULL)) >= 0)
                step = 1;
            if (step == 1 &&
                send_all(cli, tx, N) == 0 &&
                recv_all(acc, rx, N) == 0 &&
                memcmp(tx, rx, N) == 0)
                step = 2;
        }
        bit(5, step == 2, "48 KiB stream arrives byte-identical");
        if (acc >= 0) close(acc);
        if (cli >= 0) close(cli);
        if (lsn >= 0) close(lsn);
    }

    printf("linux-tcp6: RESULT=%d (want 63)\n", ok_all);
    return ok_all;      /* exit code IS the contract the harness checks */
}
