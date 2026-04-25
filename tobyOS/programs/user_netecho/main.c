/* user_netecho/main.c -- minimal UDP echo / send / listen tool.
 *
 * Three modes:
 *
 *   netecho listen <port>
 *       Bind to <port>, print every datagram that arrives along with
 *       the sender's "ip:port", and echo it back. Loops forever.
 *       Press Ctrl+C from the foreground to kill it.
 *
 *   netecho send <ip> <port> <text...>
 *       Build "text..." (joined with spaces), send one UDP datagram,
 *       wait up to ~2 seconds for a reply, print it, exit.
 *
 *   netecho echo <port>
 *       Bind to <port> and just echo without printing the payload --
 *       handy for benchmarking / spammy peers.
 *
 * Try this from the shell, with a host-side `nc -u 127.0.0.1 5555`:
 *
 *   tobyOS> run /bin/netecho listen 5555 &
 *   [host] $ echo "hello from host" | nc -u 127.0.0.1 5555
 *   [guest] netecho: 10.0.2.2:54321 -> "hello from host"
 *   [host]   nc receives "hello from host" back.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_YIELD     5
#define SYS_SOCKET    6
#define SYS_BIND      7
#define SYS_SENDTO    8
#define SYS_RECVFROM  9

#define AF_INET       2
#define SOCK_DGRAM    2

struct sockaddr_in_be {
    uint32_t ip;
    uint16_t port;
    uint16_t _pad;
};

/* ---- syscall stubs --------------------------------------------- */

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline void sys_exit(int code) {
    __asm__ volatile ("syscall"
        : : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}
static inline int sys_socket(int domain, int type) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SOCKET), "D"((long)domain), "S"((long)type)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_bind(int fd, uint16_t port_be) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_BIND), "D"((long)fd), "S"((long)port_be)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline long sys_sendto(int fd, const void *buf, size_t len,
                              uint32_t dst_ip_be, uint16_t dst_port_be) {
    long r;
    register long r10 __asm__("r10") = (long)dst_ip_be;
    register long r8  __asm__("r8")  = (long)dst_port_be;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SENDTO), "D"((long)fd), "S"(buf), "d"(len),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_recvfrom(int fd, void *buf, size_t len,
                                struct sockaddr_in_be *src_out) {
    long r;
    register long r10 __asm__("r10") = (long)src_out;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_RECVFROM), "D"((long)fd), "S"(buf), "d"(len),
          "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- string helpers -------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
static int  streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static void putstr(const char *s) {
    sys_write(1, s, my_strlen(s));
}
static size_t put_uint(char *buf, size_t off, unsigned v) {
    char tmp[16]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    while (n--) buf[off++] = tmp[n];
    return off;
}

/* ---- byte-order helpers (mirror kernel) ----------------------- */

static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }

/* Parse an unsigned decimal in [0..65535]. Returns -1 on garbage. */
static long parse_u(const char *s) {
    if (!s || !*s) return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    return v;
}

/* Parse "a.b.c.d" into a network-byte-order 32-bit value. -1 on bad. */
static long parse_ip(const char *s, uint32_t *out) {
    uint32_t parts[4]; int idx = 0; long acc = 0; int seen = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            acc = acc * 10 + (*s - '0');
            if (acc > 255) return -1;
            seen = 1;
        } else if (*s == '.' || *s == '\0') {
            if (!seen) return -1;
            if (idx >= 4) return -1;
            parts[idx++] = (uint32_t)acc;
            acc = 0; seen = 0;
            if (*s == '\0') break;
        } else return -1;
    }
    if (idx != 4) return -1;
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

/* Format ip into "a.b.c.d" -> returns end pointer. */
static char *fmt_ip(char *p, uint32_t ip_be) {
    const uint8_t *b = (const uint8_t *)&ip_be;
    for (int i = 0; i < 4; i++) {
        char tmp[4]; int n = 0; unsigned v = b[i];
        if (v == 0) tmp[n++] = '0';
        else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
        while (n--) *p++ = tmp[n];
        if (i != 3) *p++ = '.';
    }
    return p;
}

/* ---- modes ------------------------------------------------------ */

static int mode_listen(uint16_t port_host, int echo_back, int verbose) {
    int fd = sys_socket(AF_INET, SOCK_DGRAM);
    if (fd < 0) { putstr("netecho: socket() failed\n"); return 1; }
    if (sys_bind(fd, htons(port_host)) < 0) {
        putstr("netecho: bind() failed (port already in use?)\n");
        return 1;
    }
    if (verbose) {
        char buf[128]; size_t off = 0;
        const char *p1 = "netecho: listening on UDP port ";
        for (size_t k = 0; k < my_strlen(p1); k++) buf[off++] = p1[k];
        off = put_uint(buf, off, port_host);
        buf[off++] = '\n';
        sys_write(1, buf, off);
    }
    char buf[1500];
    for (;;) {
        struct sockaddr_in_be src;
        long n = sys_recvfrom(fd, buf, sizeof(buf), &src);
        if (n < 0) {
            /* -EINTR (-4) means a signal arrived; the kernel will
             * tear us down before we run again, so this branch is
             * mostly defensive. */
            if (n == -4) sys_exit(130);
            putstr("netecho: recvfrom error\n");
            return 1;
        }
        if (verbose) {
            char hdr[96]; size_t off = 0;
            const char *pre = "netecho: ";
            for (size_t k = 0; k < my_strlen(pre); k++) hdr[off++] = pre[k];
            char *p = hdr + off;
            p = fmt_ip(p, src.ip);
            *p++ = ':';
            off = (size_t)(p - hdr);
            off = put_uint(hdr, off, ntohs(src.port));
            const char *mid = " -> \"";
            for (size_t k = 0; k < my_strlen(mid); k++) hdr[off++] = mid[k];
            sys_write(1, hdr, off);
            sys_write(1, buf, (size_t)n);
            const char *suf = "\"\n";
            sys_write(1, suf, my_strlen(suf));
        }
        if (echo_back) {
            (void)sys_sendto(fd, buf, (size_t)n, src.ip, src.port);
        }
    }
}

static int mode_send(uint32_t dst_ip, uint16_t dst_port_host,
                     int argc, char **argv, int first_text_arg) {
    /* Concatenate argv[first_text_arg..argc-1] separated by spaces. */
    char msg[1400]; size_t off = 0;
    for (int i = first_text_arg; i < argc; i++) {
        size_t l = my_strlen(argv[i]);
        if (off + l + 1 >= sizeof(msg)) break;
        for (size_t k = 0; k < l; k++) msg[off++] = argv[i][k];
        if (i + 1 < argc) msg[off++] = ' ';
    }

    int fd = sys_socket(AF_INET, SOCK_DGRAM);
    if (fd < 0) { putstr("netecho: socket() failed\n"); return 1; }

    long sent = sys_sendto(fd, msg, off, dst_ip, htons(dst_port_host));
    if (sent < 0) {
        putstr("netecho: sendto failed (ARP miss? gateway down?)\n");
        return 1;
    }

    /* Wait briefly for one reply. recvfrom blocks; the host may never
     * answer, so we yield+wait a bounded number of times by leaning on
     * SIGINT to interrupt us if the user gets bored. */
    char buf[1500];
    struct sockaddr_in_be src;
    long n = sys_recvfrom(fd, buf, sizeof(buf), &src);
    if (n < 0) {
        if (n == -4) sys_exit(130);
        putstr("netecho: recvfrom failed\n");
        return 1;
    }
    char hdr[64]; size_t hlen = 0;
    const char *pre = "reply from ";
    for (size_t k = 0; k < my_strlen(pre); k++) hdr[hlen++] = pre[k];
    char *p = hdr + hlen;
    p = fmt_ip(p, src.ip);
    *p++ = ':';
    hlen = (size_t)(p - hdr);
    hlen = put_uint(hdr, hlen, ntohs(src.port));
    const char *mid = ": ";
    for (size_t k = 0; k < my_strlen(mid); k++) hdr[hlen++] = mid[k];
    sys_write(1, hdr, hlen);
    sys_write(1, buf, (size_t)n);
    sys_write(1, "\n", 1);
    return 0;
}

/* ---- entry ----------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    if (argc < 2) {
usage:
        putstr("usage:\n"
               "  netecho listen <port>             # bind, print, echo back\n"
               "  netecho echo   <port>             # bind, echo (silent)\n"
               "  netecho send   <ip> <port> <text...>\n");
        return 1;
    }

    if (streq(argv[1], "listen")) {
        if (argc < 3) goto usage;
        long p = parse_u(argv[2]);
        if (p <= 0) { putstr("netecho: bad port\n"); return 1; }
        return mode_listen((uint16_t)p, 1, 1);
    }
    if (streq(argv[1], "echo")) {
        if (argc < 3) goto usage;
        long p = parse_u(argv[2]);
        if (p <= 0) { putstr("netecho: bad port\n"); return 1; }
        return mode_listen((uint16_t)p, 1, 0);
    }
    if (streq(argv[1], "send")) {
        if (argc < 5) goto usage;
        uint32_t ip;
        if (parse_ip(argv[2], &ip) < 0) {
            putstr("netecho: bad ip\n"); return 1;
        }
        long p = parse_u(argv[3]);
        if (p <= 0) { putstr("netecho: bad port\n"); return 1; }
        return mode_send(ip, (uint16_t)p, argc, argv, 4);
    }
    goto usage;
}
