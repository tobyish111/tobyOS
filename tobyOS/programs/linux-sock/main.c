/* linux-sock -- socket-semantics acceptance test, peer-less half (2026-08-22).
 *
 * Covers what can be proven WITHOUT a remote peer (this stack has no
 * loopback, so TCP client/server pairs live in logs/lxsock.sh with a real
 * host peer over SLIRP hostfwd). Every refusal asserts its errno: "the call
 * failed" is also what EBADF looks like, and half of these paths used to be
 * `return 0` no-ops that a lazy assertion would bless.
 *
 *   bit0  AF_UNIX shutdown(SHUT_WR) is a HALF-close: the peer reads EOF,
 *         but the peer's own sends still arrive and we still read them
 *   bit1  send after SHUT_WR is EPIPE (with SIGPIPE ignored), exactly
 *   bit2  shutdown(SHUT_RD): our reads report EOF at once, even with a
 *         live peer that could still send
 *   bit3  THE OWED TEST: read a non-blocking socket BEFORE data arrives
 *         and the answer is EAGAIN -- the errno-space collision that once
 *         shipped ENAMETOOLONG here went undetected for five days because
 *         no gate ever exercised this exact shape
 *   bit4  non-blocking connect to a SILENT port: EINPROGRESS, then the
 *         SO_SNDTIMEO deadline surfaces as a poll() edge with
 *         SO_ERROR == ETIMEDOUT. (First run of this test proved the host
 *         does NOT RST port 9 -- it drops -- and ALSO caught the deadline
 *         arithmetic running 15x slow: pit-tick deadlines under TCG.
 *         Timeout classification is what this environment can prove
 *         deterministically, so that is what is asserted.)
 *   bit5  BLOCKING connect to the same silent port: ETIMEDOUT, in time
 *         with the configured SO_SNDTIMEO -- not the 75 s SYN-retry
 *         give-up the broken deadline produced
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);          /* we assert EPIPE, not death */

    /* ---- bits 0-2: AF_UNIX half-close semantics on a socketpair ---- */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            /* Queue one message BEFORE the shutdown: EOF must come AFTER
             * the drain, not instead of it. */
            (void)!write(sv[0], "pre", 3);
            if (shutdown(sv[0], SHUT_WR) == 0) {
                char b[8];
                ssize_t r1 = read(sv[1], b, sizeof b);    /* the queued "pre" */
                ssize_t r2 = read(sv[1], b, sizeof b);    /* then EOF */
                /* Reverse direction must still be alive: peer -> us. */
                ssize_t w = write(sv[1], "back", 4);
                ssize_t r3 = read(sv[0], b, sizeof b);
                printf("sock: unix half-close drain=%zd eof=%zd back=%zd/%zd\n",
                       r1, r2, w, r3);
                if (r1 == 3 && r2 == 0 && w == 4 && r3 == 4) bits |= 1;

                errno = 0;
                ssize_t we = send(sv[0], "x", 1, 0);
                printf("sock: send after SHUT_WR rc=%zd errno=%d (want EPIPE=%d)\n",
                       we, errno, EPIPE);
                if (we == -1 && errno == EPIPE) bits |= 2;
            }
            /* SHUT_RD on the peer: immediate EOF for its reads. */
            if (shutdown(sv[1], SHUT_RD) == 0) {
                char b[4];
                ssize_t r = read(sv[1], b, sizeof b);
                printf("sock: read after SHUT_RD rc=%zd (want 0)\n", r);
                if (r == 0) bits |= 4;
            }
            close(sv[0]); close(sv[1]);
        }
    }

    /* ---- bit3: the owed EAGAIN test (non-blocking, no data yet) ---- */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0) {
            char b[8];
            errno = 0;
            ssize_t r = read(sv[0], b, sizeof b);
            int unix_ok = (r == -1 && errno == EAGAIN);
            /* Same shape through recvfrom on an unconnected UDP socket. */
            int u = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
            int udp_ok = 0;
            if (u >= 0) {
                struct sockaddr_in me;
                memset(&me, 0, sizeof me);
                me.sin_family = AF_INET;
                me.sin_port = htons(34567);
                me.sin_addr.s_addr = INADDR_ANY;
                if (bind(u, (struct sockaddr *)&me, sizeof me) == 0) {
                    errno = 0;
                    ssize_t ur = recvfrom(u, b, sizeof b, 0, 0, 0);
                    udp_ok = (ur == -1 && errno == EAGAIN);
                }
                close(u);
            }
            printf("sock: pre-data reads unix(errno=%d) udp(ok=%d) want EAGAIN=%d\n",
                   unix_ok ? EAGAIN : errno, udp_ok, EAGAIN);
            if (unix_ok && udp_ok) bits |= 8;
            close(sv[0]); close(sv[1]);
        }
    }

    /* ---- bit4: non-blocking connect timeout classifies via SO_ERROR ---- */
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd >= 0) {
            struct timeval tv = { 2, 0 };      /* connect deadline: 2 s */
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(9);            /* silent on the host (drops) */
            sa.sin_addr.s_addr = inet_addr("10.0.2.2");
            errno = 0;
            int c = connect(fd, (struct sockaddr *)&sa, sizeof sa);
            if (c == -1 && errno == EINPROGRESS) {
                long t0 = now_ms();
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                int pr = poll(&p, 1, 10000);   /* must beat this: deadline 2s */
                long dt = now_ms() - t0;
                int soe = 0; socklen_t sl = sizeof soe;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soe, &sl);
                printf("sock: nb connect poll=%d revents=0x%x after %ldms "
                       "SO_ERROR=%d (want ETIMEDOUT=%d)\n", pr, p.revents,
                       dt, soe, ETIMEDOUT);
                if (pr == 1 && (p.revents & POLLERR) && soe == ETIMEDOUT &&
                    dt < 8000) bits |= 16;
            } else {
                printf("sock: nb connect rc=%d errno=%d (want EINPROGRESS=%d)\n",
                       c, errno, EINPROGRESS);
            }
            close(fd);
        }
    }

    /* ---- bit5: blocking connect times out honestly and on time ---- */
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct timeval tv = { 2, 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(9);
            sa.sin_addr.s_addr = inet_addr("10.0.2.2");
            long t0 = now_ms();
            errno = 0;
            int c = connect(fd, (struct sockaddr *)&sa, sizeof sa);
            long dt = now_ms() - t0;
            printf("sock: blocking connect rc=%d errno=%d after %ldms "
                   "(want ETIMEDOUT=%d in ~2s)\n", c, errno, dt, ETIMEDOUT);
            if (c == -1 && errno == ETIMEDOUT && dt >= 1500 && dt < 8000)
                bits |= 32;
            close(fd);
        }
    }

    printf("LXSOCK: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
