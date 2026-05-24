/* user_nc/main.c -- minimal UDP netcat for tobyOS.
 *
 * Usage:
 *   nc -l <port>            listen: bind, print datagrams, echo back
 *   nc <ip> <port>          send:  read stdin lines, send as UDP, print replies
 */

#include <stdio.h>
#include <string.h>

typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

struct sockaddr_in_be {
    uint32_t ip;
    uint16_t port;
    uint16_t _pad;
};

static inline int sys_socket(int domain, int type) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(6L), "D"((long)domain), "S"((long)type)
        : "rcx", "r11", "memory");
    return (int)r;
}

static inline int sys_bind(int fd, uint16_t port_be) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(7L), "D"((long)fd), "S"((long)port_be)
        : "rcx", "r11", "memory");
    return (int)r;
}

static inline long sys_sendto(int fd, const void *buf, unsigned long len,
                              uint32_t dst_ip_be, uint16_t dst_port_be) {
    long r;
    register long r10 __asm__("r10") = (long)dst_ip_be;
    register long r8  __asm__("r8")  = (long)dst_port_be;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(8L), "D"((long)fd), "S"(buf), "d"(len),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

static inline long sys_recvfrom(int fd, void *buf, unsigned long len,
                                struct sockaddr_in_be *src_out) {
    long r;
    register long r10 __asm__("r10") = (long)src_out;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(9L), "D"((long)fd), "S"(buf), "d"(len),
          "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}

static inline void sys_yield(void) {
    long _dummy;
    __asm__ volatile("syscall"
        : "=a"(_dummy) : "0"(5L)
        : "rcx", "r11", "memory");
}

static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }

static int parse_port(const char *s) {
    if (!s || !*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    return v;
}

static int parse_ip(const char *s, uint32_t *out) {
    uint32_t parts[4];
    int idx = 0;
    int acc = 0, seen = 0;
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
    const uint8_t *b = (const uint8_t *)&ip_be;
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = b[i];
        char tmp[4];
        int n = 0;
        if (v == 0) tmp[n++] = '0';
        else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
        while (n--) out[pos++] = tmp[n];
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

static int mode_listen(uint16_t port) {
    int fd = sys_socket(2, 2);
    if (fd < 0) {
        fprintf(stderr, "nc: socket() failed\n");
        return 1;
    }
    if (sys_bind(fd, htons(port)) < 0) {
        fprintf(stderr, "nc: bind to port %u failed\n", (unsigned)port);
        return 1;
    }
    fprintf(stderr, "nc: listening on UDP port %u\n", (unsigned)port);

    char buf[1500];
    for (;;) {
        struct sockaddr_in_be src;
        long n = sys_recvfrom(fd, buf, sizeof(buf), &src);
        if (n < 0) {
            if (n == -4) return 130;
            fprintf(stderr, "nc: recvfrom error (%ld)\n", n);
            return 1;
        }

        char ipstr[20];
        fmt_ip(ipstr, src.ip);
        fprintf(stderr, "[%s:%u] ", ipstr, (unsigned)ntohs(src.port));
        fwrite(buf, 1, (size_t)n, stdout);
        if (n == 0 || buf[n - 1] != '\n')
            putchar('\n');

        sys_sendto(fd, buf, (unsigned long)n, src.ip, src.port);
    }
}

static int mode_send(uint32_t dst_ip, uint16_t port) {
    int fd = sys_socket(2, 2);
    if (fd < 0) {
        fprintf(stderr, "nc: socket() failed\n");
        return 1;
    }

    char ipstr[20];
    fmt_ip(ipstr, dst_ip);
    fprintf(stderr, "nc: sending to %s:%u (type lines, Ctrl+D to quit)\n",
            ipstr, (unsigned)port);

    char line[1400];
    while (fgets(line, (int)sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            len--;
        if (len == 0) continue;

        long sent = sys_sendto(fd, line, (unsigned long)len,
                               dst_ip, htons(port));
        if (sent < 0) {
            fprintf(stderr, "nc: sendto failed (%ld)\n", sent);
            return 1;
        }

        char rbuf[1500];
        struct sockaddr_in_be src;
        long n = sys_recvfrom(fd, rbuf, sizeof(rbuf), &src);
        if (n < 0) {
            if (n == -4) return 130;
            fprintf(stderr, "nc: no reply (error %ld)\n", n);
            continue;
        }
        fwrite(rbuf, 1, (size_t)n, stdout);
        if (n == 0 || rbuf[n - 1] != '\n')
            putchar('\n');
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage:\n"
            "  nc -l <port>       listen on UDP port, echo datagrams\n"
            "  nc <ip> <port>     send stdin lines as UDP to ip:port\n");
        return 1;
    }

    if (strcmp(argv[1], "-l") == 0) {
        int p = parse_port(argv[2]);
        if (p <= 0) {
            fprintf(stderr, "nc: bad port '%s'\n", argv[2]);
            return 1;
        }
        return mode_listen((uint16_t)p);
    }

    uint32_t ip;
    if (parse_ip(argv[1], &ip) < 0) {
        fprintf(stderr, "nc: bad IP address '%s'\n", argv[1]);
        return 1;
    }
    int p = parse_port(argv[2]);
    if (p <= 0) {
        fprintf(stderr, "nc: bad port '%s'\n", argv[2]);
        return 1;
    }
    return mode_send(ip, (uint16_t)p);
}
