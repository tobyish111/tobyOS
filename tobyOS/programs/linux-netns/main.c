/* linux-netns -- Phase 3 slice 12 (cut 1) acceptance test: NETWORK namespaces.
 *
 * ===========================================================================
 * THE ASSERTION THAT CARRIES THIS TEST IS AN ERRNO
 *
 * Cut 1's claim is "a process in a new network namespace has no network". The
 * lazy way to test that is "connect() failed" -- which is worthless here,
 * because connect() to an address with nothing listening ALSO fails, and this
 * VM's SLIRP network has almost nothing listening. A test that accepted any
 * failure would pass on a kernel with no namespace support whatsoever.
 *
 * So every check asserts ENETUNREACH *specifically*, and the CONTROL asserts
 * the outside world gets something OTHER than ENETUNREACH. That pairing is what
 * distinguishes "there is no network here" from "the network is fine and the
 * peer is absent" -- and it needs no reachable peer to work, which keeps it
 * robust under a headless harness.
 *
 * The other half of correctness for a sandbox is what must KEEP working:
 * bit4 asserts AF_UNIX socketpair IPC still functions inside the namespace,
 * because that is exactly how a sandboxed renderer talks to its browser. An
 * "isolation" that severed local IPC would be useless for slice 14.
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  baseline: /proc/self/ns/net is the initial inum
 *   bit1  unshare(CLONE_NEWNET) moves the caller; inum differs
 *   b2    TCP connect() inside is ENETUNREACH; outside it is NOT (control)
 *   bit3  UDP sendto() inside is ENETUNREACH; outside it succeeds (control)
 *   bit4  AF_UNIX socketpair still works inside -- isolation must not break
 *         local IPC
 *   bit5  a socket created BEFORE the unshare keeps working afterwards (Linux
 *         semantics; what lets a sandbox keep an inherited socketpair)
 *   bit6  bind() to a specific address inside is EADDRNOTAVAIL, while
 *         INADDR_ANY still binds
 *   bit7  unprivileged CLONE_NEWUSER|CLONE_NEWNET works (that IS the sandbox
 *         recipe), and our own namespace is untouched
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NS_NEWNET   0x40000000
#define NS_NEWUSER  0x10000000

#define INIT_NET_INUM 4026531841ull      /* NS_INUM_INIT_NET in nsproxy.h */
#define TESTUID       1000

/* QEMU SLIRP: .2 is the gateway/host, .3 is the DNS forwarder. Nothing needs to
 * ANSWER for this test -- only the errno matters. */
#define PEER_IP   "10.0.2.2"
#define DNS_IP    "10.0.2.3"

static int k_unshare(int f) { return (int)syscall(SYS_unshare, f); }
static int k_setuid(int u)  { return (int)syscall(SYS_setuid, u); }

static unsigned long long ns_inum(const char *kind) {
    char path[128], link[128], want[32];
    snprintf(path, sizeof path, "/proc/self/ns/%s", kind);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) return 0;
    link[n] = '\0';
    snprintf(want, sizeof want, "%s:[", kind);
    size_t wl = strlen(want);
    if (strncmp(link, want, wl) != 0) return 0;
    char *e = strchr(link + wl, ']');
    if (!e) return 0;
    *e = '\0';
    return strtoull(link + wl, NULL, 10);
}

/* Attempt a TCP connect and return the errno (0 on success). */
static int try_tcp_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(80);
    sa.sin_addr.s_addr = inet_addr(PEER_IP);
    errno = 0;
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    int e = (rc == 0) ? 0 : errno;
    close(fd);
    return e;
}

/* Attempt a UDP sendto and return the errno (0 on success). */
static int try_udp_send(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(53);
    sa.sin_addr.s_addr = inet_addr(DNS_IP);
    errno = 0;
    ssize_t n = sendto(fd, "x", 1, 0, (struct sockaddr *)&sa, sizeof sa);
    int e = (n >= 0) ? 0 : errno;
    close(fd);
    return e;
}

/* bind() to a specific address; return errno (0 on success). */
static int try_bind(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    sa.sin_addr.s_addr = ip ? inet_addr(ip) : htonl(INADDR_ANY);
    errno = 0;
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    int e = (rc == 0) ? 0 : errno;
    close(fd);
    return e;
}

/* AF_UNIX socketpair round-trip. 0 on success. */
static int try_unix_ipc(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1;
    const char *msg = "netns-ipc";
    if (write(sv[1], msg, strlen(msg)) != (ssize_t)strlen(msg)) { close(sv[0]); close(sv[1]); return 2; }
    char buf[32] = {0};
    ssize_t n = read(sv[0], buf, sizeof buf - 1);
    close(sv[0]); close(sv[1]);
    if (n <= 0 || strcmp(buf, msg) != 0) return 3;
    return 0;
}

static int reap(pid_t p) {
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    unsigned long long my_inum = ns_inum("net");
    printf("netns: start net:[%llu]\n", my_inum);

    /* Measure the OUTSIDE world first: these are the controls, and they have to
     * be taken before anything unshares. */
    int out_tcp  = try_tcp_connect();
    int out_udp  = try_udp_send();
    int out_bind = try_bind(NULL, 0);
    printf("netns: outside -- tcp errno=%d udp errno=%d bind(ANY) errno=%d\n",
           out_tcp, out_udp, out_bind);

    /* ---- bit0: baseline ---- */
    if (my_inum == INIT_NET_INUM) bits |= 1;
    else printf("netns: baseline FAILED (want %llu)\n", INIT_NET_INUM);

    /* ---- bit1: unshare moves the caller ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNET) != 0) { printf("netns: unshare errno=%d\n", errno); _exit(1); }
            unsigned long long in = ns_inum("net");
            _exit((in != 0 && in != INIT_NET_INUM) ? 0 : 2);
        }
        if (reap(c) == 0) { bits |= 2; printf("netns: unshare(NEWNET) moved the caller\n"); }
        else printf("netns: bit1 FAILED\n");
    }

    /* ---- bit2: TCP connect is ENETUNREACH inside, something ELSE outside ----
     * The control is what makes this meaningful: on this VM nothing may be
     * listening on :80, so "connect failed" is not evidence of anything. Only
     * the errno DIFFERENCE is. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNET) != 0) _exit(1);
            int e = try_tcp_connect();
            if (e != ENETUNREACH) { printf("netns: in-ns tcp errno=%d, want ENETUNREACH\n", e); _exit(2); }
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0 && out_tcp != ENETUNREACH) {
            bits |= 4;
            printf("netns: TCP connect -- ENETUNREACH inside, errno=%d outside "
                   "(so it is the namespace, not a dead peer)\n", out_tcp);
        } else {
            printf("netns: bit2 FAILED rc=%d outside_errno=%d\n", rc, out_tcp);
        }
    }

    /* ---- bit3: UDP sendto ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNET) != 0) _exit(1);
            int e = try_udp_send();
            if (e != ENETUNREACH) { printf("netns: in-ns udp errno=%d, want ENETUNREACH\n", e); _exit(2); }
            _exit(0);
        }
        int rc = reap(c);
        /* Outside, a UDP sendto into SLIRP is accepted by the stack (it needs no
         * reply), so the control here is the stronger form: outright success. */
        if (rc == 0 && out_udp == 0) {
            bits |= 8;
            printf("netns: UDP sendto -- ENETUNREACH inside, succeeds outside\n");
        } else {
            printf("netns: bit3 FAILED rc=%d outside_errno=%d\n", rc, out_udp);
        }
    }

    /* ---- bit4: local IPC must SURVIVE the isolation ----
     * An isolation that also severed AF_UNIX would be useless for slice 14: a
     * sandboxed renderer reaches its browser over exactly this. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNET) != 0) _exit(1);
            _exit(try_unix_ipc() == 0 ? 0 : 2);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 16;
            printf("netns: AF_UNIX socketpair still works inside the namespace\n");
        } else printf("netns: bit4 FAILED rc=%d -- local IPC broke\n", rc);
    }

    /* ---- bit5: a socket that PREDATES the unshare keeps its network ----
     * Linux does not retroactively unplug open sockets, and this is what lets a
     * sandbox inherit a working socketpair (or an already-connected fd) from its
     * parent. Enforcement is a property of the socket, not of the process. */
    {
        pid_t c = fork();
        if (c == 0) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) _exit(1);
            if (k_unshare(NS_NEWNET) != 0) _exit(2);
            /* Same socket, created BEFORE the unshare: still networked. */
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(53);
            sa.sin_addr.s_addr = inet_addr(DNS_IP);
            errno = 0;
            ssize_t n = sendto(fd, "x", 1, 0, (struct sockaddr *)&sa, sizeof sa);
            int e = (n >= 0) ? 0 : errno;
            /* ...while a socket created AFTER it is not. */
            int e2 = try_udp_send();
            close(fd);
            if (e != 0)              { printf("netns: pre-unshare socket lost its network errno=%d\n", e); _exit(3); }
            if (e2 != ENETUNREACH)   { printf("netns: post-unshare socket errno=%d\n", e2); _exit(4); }
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 32;
            printf("netns: a socket created BEFORE the unshare keeps working; one "
                   "created after does not\n");
        } else printf("netns: bit5 FAILED rc=%d\n", rc);
    }

    /* ---- bit6: bind ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNET) != 0) _exit(1);
            int spec = try_bind(PEER_IP, 12345);
            if (spec != EADDRNOTAVAIL) { printf("netns: in-ns bind(specific) errno=%d, want EADDRNOTAVAIL\n", spec); _exit(2); }
            /* INADDR_ANY still works: binding a port is local, and nothing will
             * ever arrive -- which is what a sandboxed server should experience. */
            int any = try_bind(NULL, 12346);
            if (any != 0) { printf("netns: in-ns bind(ANY) errno=%d, want 0\n", any); _exit(3); }
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0 && out_bind == 0) {
            bits |= 64;
            printf("netns: bind(specific)=EADDRNOTAVAIL inside, bind(ANY) still ok\n");
        } else printf("netns: bit6 FAILED rc=%d outside_bind=%d\n", rc, out_bind);
    }

    /* ---- bit7: THE SANDBOX RECIPE, unprivileged ----
     * CLONE_NEWUSER|CLONE_NEWNET from a non-root uid is precisely what a
     * renderer sandbox does. If this does not work, slice 14 cannot. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_unshare(NS_NEWUSER | NS_NEWNET) != 0) {
                printf("netns: unprivileged NEWUSER|NEWNET errno=%d\n", errno);
                _exit(2);
            }
            if (ns_inum("net") == INIT_NET_INUM) _exit(3);
            if (try_tcp_connect() != ENETUNREACH) _exit(4);
            /* ...and it can still talk locally. */
            if (try_unix_ipc() != 0) _exit(5);
            _exit(0);
        }
        int rc = reap(c);
        int ours_intact = (ns_inum("net") == my_inum);
        if (rc == 0 && ours_intact) {
            bits |= 128;
            printf("netns: uid %d built a NEWUSER|NEWNET sandbox -- no network, "
                   "local IPC intact; our own ns untouched\n", TESTUID);
        } else {
            printf("netns: bit7 FAILED rc=%d ours_intact=%d\n", rc, ours_intact);
        }
    }

    printf("netns: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
