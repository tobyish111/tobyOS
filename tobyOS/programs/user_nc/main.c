/* nc / netcat -- TCP by default, UDP with -u.
 *
 *     nc [-v] [-w sec] HOST PORT        connect over TCP
 *     nc -u [-v] HOST PORT              send/receive UDP datagrams
 *     nc -l [-u] [-v] PORT              listen
 *     nc -z [-v] HOST PORT              connect, report, and exit
 *
 * THIS USED TO BE UDP-ONLY. `nc host 80` opened a UDP socket and sent the
 * request as a datagram; nothing answered, and the tool waited. Netcat on
 * every distro is TCP unless told otherwise, so every use of it here --
 * poking an HTTP server, checking a port, piping bytes between machines --
 * was quietly doing something else. TCP is now the default and -u is the
 * opt-in, which is the way round netcat has always had it.
 *
 * HOST may be a name: toby_tcp_connect_host resolves it through the same
 * resolver dig and host use.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <toby/net.h>

struct sockaddr_in_be { uint32_t ip; uint16_t port; uint16_t _pad; };

static int uflag, lflag, vflag, zflag;
static unsigned wait_sec = 10;

static uint16_t htons_(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }

/* libtoby has no yield wrapper; ABI_SYS_YIELD is 5. */
static void nc_yield(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(5L) : "rcx", "r11", "memory");
    (void)r;
}

static int parse_port(const char *s) {
    if (!s || !*s) return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    return (int)v;
}

static int parse_ip(const char *s, uint32_t *out) {
    uint32_t parts[4];
    int idx = 0, acc = 0, seen = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            acc = acc * 10 + (*s - '0');
            if (acc > 255) return -1;
            seen = 1;
        } else if (*s == '.' || *s == '\0') {
            if (!seen || idx >= 4) return -1;
            parts[idx++] = (uint32_t)acc;
            acc = 0; seen = 0;
            if (*s == '\0') break;
        } else {
            return -1;
        }
    }
    if (idx != 4) return -1;
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

static void fmt_ip(char *out, uint32_t ip_be) {
    snprintf(out, 20, "%u.%u.%u.%u",
             (unsigned)(ip_be & 0xFF), (unsigned)((ip_be >> 8) & 0xFF),
             (unsigned)((ip_be >> 16) & 0xFF), (unsigned)((ip_be >> 24) & 0xFF));
}

/* ------------------------------------------------------------ UDP paths */

static int sys_socket(int domain, int type) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(6L), "D"((long)domain), "S"((long)type)
                     : "rcx", "r11", "memory");
    return (int)r;
}
static int sys_bind(int fd, uint16_t port_be) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(7L), "D"((long)fd), "S"((long)port_be)
                     : "rcx", "r11", "memory");
    return (int)r;
}
static long sys_sendto(int fd, const void *buf, unsigned long len,
                       uint32_t ip_be, uint16_t port_be) {
    long r;
    register long r10 __asm__("r10") = (long)ip_be;
    register long r8  __asm__("r8")  = (long)port_be;
    __asm__ volatile("syscall" : "=a"(r)
                     : "0"(8L), "D"((long)fd), "S"(buf), "d"(len), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys_recvfrom(int fd, void *buf, unsigned long len, struct sockaddr_in_be *src) {
    long r;
    register long r10 __asm__("r10") = (long)src;
    __asm__ volatile("syscall" : "=a"(r)
                     : "0"(9L), "D"((long)fd), "S"(buf), "d"(len), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static int udp_listen(uint16_t port) {
    int fd = sys_socket(2, 2);
    if (fd < 0) { fprintf(stderr, "nc: socket failed\n"); return 1; }
    if (sys_bind(fd, htons_(port)) < 0) {
        fprintf(stderr, "nc: cannot bind UDP port %u\n", (unsigned)port);
        return 1;
    }
    if (vflag) fprintf(stderr, "nc: listening on UDP port %u\n", (unsigned)port);
    char buf[1500];
    for (;;) {
        struct sockaddr_in_be src;
        long n = sys_recvfrom(fd, buf, sizeof buf, &src);
        if (n <= 0) { nc_yield(); continue; }
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
}

static int udp_send(uint32_t ip, uint16_t port) {
    int fd = sys_socket(2, 2);
    if (fd < 0) { fprintf(stderr, "nc: socket failed\n"); return 1; }
    /* A source port is needed for anything to come back. */
    for (int i = 0; i < 8; i++)
        if (sys_bind(fd, htons_((uint16_t)(30000 + i * 61))) >= 0) break;
    char line[1500];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        if (sys_sendto(fd, line, n, ip, htons_(port)) < 0) {
            fprintf(stderr, "nc: send failed\n");
            return 1;
        }
        struct sockaddr_in_be src;
        char buf[1500];
        long got = sys_recvfrom(fd, buf, sizeof buf, &src);
        if (got > 0) { fwrite(buf, 1, (size_t)got, stdout); fflush(stdout); }
    }
    return 0;
}

/* ------------------------------------------------------------ TCP paths */

/* Pumps stdin to the connection and the connection to stdout.
 *
 * tcp_recv returns > 0 for data, 0 for "the ten-second cap expired and the
 * peer has not spoken", and < 0 when the peer has CLOSED. Reading 0 as
 * end-of-file -- which this did -- ends the session on the first quiet
 * moment and leaves a live connection looking finished. */
static int tcp_pump(int conn) {
    char buf[4096];
    int saw_eof_in = 0, idle = 0;
    for (;;) {
        int n = toby_tcp_recv(conn, buf, sizeof buf);
        if (n > 0) {
            fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
            idle = 0;
            continue;
        }
        if (n < 0) break;                  /* peer closed */
        if (!saw_eof_in) {
            if (fgets(buf, sizeof buf, stdin)) {
                size_t len = strlen(buf);
                if (toby_tcp_send(conn, buf, len) < 0) {
                    fprintf(stderr, "nc: send failed\n");
                    return 1;
                }
                continue;
            }
            saw_eof_in = 1;
        }
        /* stdin is finished and the peer is quiet. Two more waits, then the
         * exchange is over. */
        if (++idle >= 2) break;
    }
    return 0;
}

static int tcp_connect_to(const char *host, uint16_t port) {
    uint32_t ip;
    int conn;
    if (parse_ip(host, &ip) == 0) conn = toby_tcp_connect(ip, htons_(port), wait_sec * 1000);
    else                          conn = toby_tcp_connect_host(host, port, wait_sec * 1000);
    if (conn < 0) {
        fprintf(stderr, "nc: connect to %s port %u (tcp) failed\n", host, (unsigned)port);
        return -1;
    }
    if (vflag || zflag) {
        char ipbuf[24] = "";
        if (parse_ip(host, &ip) == 0) { fmt_ip(ipbuf, ip); }
        fprintf(stderr, "nc: %s%s%s port %u (tcp) open\n", host,
                ipbuf[0] ? " " : "", ipbuf, (unsigned)port);
    }
    return conn;
}

static int tcp_listen_on(uint16_t port) {
    int lid = toby_tcp_listen(htons_(port), 1);
    if (lid < 0) {
        fprintf(stderr, "nc: cannot listen on TCP port %u\n", (unsigned)port);
        return 1;
    }
    if (vflag) fprintf(stderr, "nc: listening on TCP port %u\n", (unsigned)port);
    int conn;
    for (;;) {
        conn = toby_tcp_accept(lid);
        if (conn >= 0) break;
        nc_yield();
    }
    if (vflag) fprintf(stderr, "nc: connection accepted\n");
    int rc = tcp_pump(conn);
    toby_tcp_close(conn);
    return rc;
}

int main(int argc, char **argv) {
    const char *host = NULL;
    int port = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-w") && i + 1 < argc) { wait_sec = (unsigned)atoi(argv[++i]); continue; }
        if (!strcmp(a, "--version")) { printf("nc (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: nc [-uvz] [-w sec] HOST PORT\n"
                   "       nc -l [-u] [-v] PORT\n"
                   "TCP unless -u is given.\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'u': uflag = 1; break;
                case 'l': lflag = 1; break;
                case 'v': vflag = 1; break;
                case 'z': zflag = 1; break;
                case 'n': break;              /* no DNS: we try an IP first anyway */
                case 'p': break;              /* source port: the stack picks one */
                default:
                    fprintf(stderr, "nc: unknown option -- '%c'\n", *p);
                    return 1;
                }
            }
            continue;
        }
        if (lflag && port < 0) { port = parse_port(a); continue; }
        if (!host) { host = a; continue; }
        if (port < 0) port = parse_port(a);
    }

    if (lflag) {
        if (port < 0) { fprintf(stderr, "usage: nc -l [-u] PORT\n"); return 1; }
        return uflag ? udp_listen((uint16_t)port) : tcp_listen_on((uint16_t)port);
    }

    if (!host || port < 0) {
        fprintf(stderr, "usage: nc [-uvz] [-w sec] HOST PORT\n");
        return 1;
    }

    if (uflag) {
        uint32_t ip;
        if (parse_ip(host, &ip) != 0) {
            fprintf(stderr, "nc: -u needs a dotted-quad address\n");
            return 1;
        }
        return udp_send(ip, (uint16_t)port);
    }

    int conn = tcp_connect_to(host, (uint16_t)port);
    if (conn < 0) return 1;
    if (zflag) { toby_tcp_close(conn); return 0; }
    int rc = tcp_pump(conn);
    toby_tcp_close(conn);
    return rc;
}
