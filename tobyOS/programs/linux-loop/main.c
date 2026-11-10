/* linux-loop -- the loopback datapath exists (2026-08-22).
 *
 * Before this slice the stack had NO loopback anywhere (cut 1's founding
 * measurement): `lo` was advertised to getifaddrs while 127.0.0.1 routed
 * to the gateway and died, so every local test server, self-connection
 * and localhost-only service was structurally impossible. Delivery is
 * INLINE (the veth model), which is why a single-threaded process can be
 * both ends of its own TCP connection here.
 *
 *   bit0  UDP over 127.0.0.1: a datagram sent to a local listener ARRIVES
 *   bit1  TCP self-connection through 127.0.0.1: connect + accept + data
 *         BOTH directions, one process, no threads
 *   bit2  closing the accepted end gives the client a real EOF
 *   bit3  connect() to a closed 127.0.0.1 port is ECONNREFUSED, and FAST
 *         (the new RST-for-closed-port answering over the loop)
 *   bit4  connected UDP to a closed local port learns ECONNREFUSED via
 *         the full ICMP chain: unreachable emitted, looped, decoded,
 *         latched as so_error
 *   bit5  the machine's OWN unicast address loops too (Linux routes
 *         self-addressed traffic via lo)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static struct sockaddr_in mkaddr(uint32_t ip_netorder, int port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = ip_netorder;
    return sa;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t lo = inet_addr("127.0.0.1");

    /* ---- bit0: UDP loop ---- */
    {
        int rs = socket(AF_INET, SOCK_DGRAM, 0);
        int ss = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ra = mkaddr(lo, 34568);
        char rb[32] = {0};
        int ok = 0;
        if (rs >= 0 && ss >= 0 && bind(rs, (void *)&ra, sizeof ra) == 0) {
            sendto(ss, "LOOPUDP", 7, 0, (void *)&ra, sizeof ra);
            ssize_t n = recvfrom(rs, rb, sizeof rb, MSG_DONTWAIT, 0, 0);
            ok = (n == 7 && memcmp(rb, "LOOPUDP", 7) == 0);
            printf("loop: udp n=%zd data='%s'\n", n, rb);
        }
        if (rs >= 0) close(rs);
        if (ss >= 0) close(ss);
        if (ok) bits |= 1;
    }

    /* ---- bits 1+2: TCP self-connection, then EOF ---- */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        int cs = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in la = mkaddr(lo, 34569);
        int ok = 0, eof_ok = 0;
        if (ls >= 0 && cs >= 0 &&
            bind(ls, (void *)&la, sizeof la) == 0 && listen(ls, 4) == 0 &&
            connect(cs, (void *)&la, sizeof la) == 0) {
            int as = accept(ls, 0, 0);
            if (as >= 0) {
                char b[16] = {0};
                ssize_t w1 = send(cs, "PING", 4, 0);
                ssize_t r1 = recv(as, b, sizeof b, 0);
                int fwd = (w1 == 4 && r1 == 4 && memcmp(b, "PING", 4) == 0);
                memset(b, 0, sizeof b);
                ssize_t w2 = send(as, "PONG!", 5, 0);
                ssize_t r2 = recv(cs, b, sizeof b, 0);
                int rev = (w2 == 5 && r2 == 5 && memcmp(b, "PONG!", 5) == 0);
                ok = fwd && rev;
                printf("loop: tcp fwd=%d rev=%d\n", fwd, rev);
                close(as);                       /* server side closes */
                ssize_t r3 = recv(cs, b, sizeof b, 0);
                eof_ok = (r3 == 0);
                printf("loop: post-close recv=%zd (want 0/EOF)\n", r3);
            }
        }
        if (cs >= 0) close(cs);
        if (ls >= 0) close(ls);
        if (ok) bits |= 2;
        if (eof_ok) bits |= 4;
    }

    /* ---- bit3: refused, and fast ---- */
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in da = mkaddr(lo, 34570);   /* nothing listens */
        long t0 = now_ms();
        errno = 0;
        int rc = fd >= 0 ? connect(fd, (void *)&da, sizeof da) : -1;
        long dt = now_ms() - t0;
        printf("loop: refused connect rc=%d errno=%d after %ldms "
               "(want ECONNREFUSED=%d, fast)\n", rc, errno, dt, ECONNREFUSED);
        if (rc == -1 && errno == ECONNREFUSED && dt < 1500) bits |= 8;
        if (fd >= 0) close(fd);
    }

    /* ---- bit4: UDP refused via the ICMP chain ---- */
    {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in da = mkaddr(lo, 34571);   /* no listener */
        int ok = 0;
        if (fd >= 0 && connect(fd, (void *)&da, sizeof da) == 0) {
            (void)!send(fd, "x", 1, 0);        /* triggers unreachable */
            usleep(50 * 1000);
            errno = 0;
            ssize_t r2 = send(fd, "y", 1, 0);  /* returns the latched error */
            ok = (r2 == -1 && errno == ECONNREFUSED);
            printf("loop: udp refused second-send rc=%zd errno=%d "
                   "(want ECONNREFUSED=%d)\n", r2, errno, ECONNREFUSED);
        }
        if (fd >= 0) close(fd);
        if (ok) bits |= 16;
    }

    /* ---- bit5: own unicast address loops ---- */
    {
        /* Learn our address: connect a UDP socket outward and read the
         * chosen source back. */
        uint32_t me = 0;
        int probe = socket(AF_INET, SOCK_DGRAM, 0);
        if (probe >= 0) {
            struct sockaddr_in out = mkaddr(inet_addr("10.0.2.2"), 53);
            if (connect(probe, (void *)&out, sizeof out) == 0) {
                struct sockaddr_in got; socklen_t gl = sizeof got;
                if (getsockname(probe, (void *)&got, &gl) == 0)
                    me = got.sin_addr.s_addr;
            }
            close(probe);
        }
        int ok = 0;
        if (me) {
            int ls = socket(AF_INET, SOCK_STREAM, 0);
            int cs = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in la = mkaddr(me, 34572);
            if (ls >= 0 && cs >= 0 &&
                bind(ls, (void *)&la, sizeof la) == 0 && listen(ls, 2) == 0 &&
                connect(cs, (void *)&la, sizeof la) == 0) {
                int as = accept(ls, 0, 0);
                if (as >= 0) {
                    char b[8] = {0};
                    (void)!send(cs, "SELF", 4, 0);
                    ok = (recv(as, b, sizeof b, 0) == 4 &&
                          memcmp(b, "SELF", 4) == 0);
                    close(as);
                }
            }
            char ip[32];
            printf("loop: own-ip %s ok=%d\n",
                   inet_ntop(AF_INET, &me, ip, sizeof ip) ? ip : "?", ok);
            if (cs >= 0) close(cs);
            if (ls >= 0) close(ls);
        } else {
            printf("loop: could not learn own ip\n");
        }
        if (ok) bits |= 32;
    }

    printf("LXLOOP: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
