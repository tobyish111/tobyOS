/* linux-sockserver -- the peer-required half of the socket semantics tests
 * (2026-08-22). Driven by logs/lxsock.sh: QEMU forwards host 127.0.0.1:18083
 * to guest :8081, and the HOST script is the peer. This is the b14 pattern;
 * no loopback exists in this stack, so a real remote is the only honest peer.
 *
 * What it proves (each line greppable by the gate):
 *   A  BLOCKING accept() waits past the old 3-second EAGAIN cap: the host
 *      deliberately connects ~4.5 s after "listening" prints. On the old
 *      kernel accept() returned EAGAIN at 3 s and this run says ACCEPT-FAIL.
 *   B  A non-blocking (MSG_DONTWAIT) read BEFORE the host sends is EAGAIN
 *      -- the owed errno-collision test, on the REAL TCP receive path.
 *   C  shutdown(SHUT_WR) is a live half-close over the wire: the host sees
 *      our FIN (EOF) after "PONG", and its "AFTER" line still reaches us.
 *   D  send after our SHUT_WR is EPIPE, exactly.
 *
 * Exit code = bits (15 = all four). The gate also asserts the HOST half:
 * that EOF actually arrived on the host after PONG. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { printf("[lxsock] socket errno=%d\n", errno); return 0; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8081);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 4) != 0) {
        printf("[lxsock] bind/listen errno=%d\n", errno);
        return 0;
    }
    printf("[lxsock] listening on :8081 (blocking accept, no timeout)\n");

    int c = accept(ls, 0, 0);          /* must survive the host's 4.5 s delay */
    if (c < 0) {
        printf("[lxsock] ACCEPT-FAIL errno=%d (the old 3s EAGAIN cap?)\n", errno);
        printf("LXSOCKSRV: VERDICT bits=%d (15=all)\n", bits);
        return bits;
    }
    printf("[lxsock] accepted after the long wait\n");
    bits |= 1;                                                   /* A */

    /* The pre-data probe must happen while the wire is PROVABLY quiet.
     * The host is blocked reading our RDY token and sends nothing until it
     * arrives -- so this MSG_DONTWAIT recv races nothing. (v1 probed right
     * after a delayed connect whose PING was already in flight: rc=4.) */
    char b[64];
    errno = 0;
    ssize_t r = recv(c, b, sizeof b, MSG_DONTWAIT);
    printf("[lxsock] pre-data recv rc=%zd errno=%d (want EAGAIN=%d)\n",
           r, errno, EAGAIN);
    if (r == -1 && errno == EAGAIN) bits |= 2;                   /* B */

    (void)!send(c, "RDY\n", 4, 0);     /* now the host may speak */
    r = recv(c, b, sizeof b, 0);
    if (r > 0 && memcmp(b, "PING", 4) == 0) {
        (void)!send(c, "PONG\n", 5, 0);
        if (shutdown(c, SHUT_WR) == 0)
            printf("[lxsock] sent PONG then SHUT_WR (host should see EOF)\n");
        /* Half-close: the host's post-EOF line must still reach us. */
        r = recv(c, b, sizeof b, 0);
        if (r > 0 && memcmp(b, "AFTER", 5) == 0) {
            printf("[lxsock] read '%.*s' AFTER our FIN -- rx side alive\n",
                   (int)(r - 1), b);
            bits |= 4;                                           /* C */
        } else {
            printf("[lxsock] post-FIN read rc=%zd errno=%d\n", r, errno);
        }
        errno = 0;
        ssize_t w = send(c, "nope", 4, 0);
        printf("[lxsock] send after SHUT_WR rc=%zd errno=%d (want EPIPE=%d)\n",
               w, errno, EPIPE);
        if (w == -1 && errno == EPIPE) bits |= 8;                /* D */
    } else {
        printf("[lxsock] expected PING, got rc=%zd\n", r);
    }

    close(c);
    close(ls);
    printf("LXSOCKSRV: VERDICT bits=%d (15=all)\n", bits);
    return bits;
}
