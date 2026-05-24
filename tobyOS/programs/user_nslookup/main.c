/* user_nslookup/main.c -- DNS A-record lookup for tobyOS.
 *
 * Usage: nslookup <hostname>
 *
 * Constructs a standard DNS A-record query, sends it via UDP to
 * 8.8.8.8:53, and parses the response for IP addresses.
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

static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }

static inline long sys_clock_ms(void) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(48L)
        : "rcx", "r11", "memory");
    return r;
}

static inline void sys_yield(void) {
    long _dummy;
    __asm__ volatile("syscall"
        : "=a"(_dummy) : "0"(5L)
        : "rcx", "r11", "memory");
}

#define DNS_SERVER_IP  0x08080808u   /* 8.8.8.8 in big-endian */
#define DNS_PORT       53
#define DNS_TYPE_A     1
#define DNS_CLASS_IN   1
#define DNS_TIMEOUT_MS 3000

static int build_query(uint8_t *pkt, int cap, const char *hostname,
                       uint16_t txid) {
    if (cap < 12) return -1;
    int off = 0;

    pkt[off++] = (uint8_t)(txid >> 8);
    pkt[off++] = (uint8_t)(txid & 0xFF);
    pkt[off++] = 0x01; pkt[off++] = 0x00;   /* flags: RD=1 */
    pkt[off++] = 0x00; pkt[off++] = 0x01;   /* qdcount = 1 */
    pkt[off++] = 0x00; pkt[off++] = 0x00;   /* ancount = 0 */
    pkt[off++] = 0x00; pkt[off++] = 0x00;   /* nscount = 0 */
    pkt[off++] = 0x00; pkt[off++] = 0x00;   /* arcount = 0 */

    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int label_len = (int)(dot - p);
        if (label_len <= 0 || label_len > 63 || off + 1 + label_len >= cap - 4)
            return -1;
        pkt[off++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++)
            pkt[off++] = (uint8_t)p[i];
        p = dot;
        if (*p == '.') p++;
    }
    pkt[off++] = 0x00;

    pkt[off++] = 0x00; pkt[off++] = (uint8_t)DNS_TYPE_A;
    pkt[off++] = 0x00; pkt[off++] = (uint8_t)DNS_CLASS_IN;

    return off;
}

static int skip_name(const uint8_t *pkt, int pkt_len, int off) {
    while (off < pkt_len) {
        uint8_t b = pkt[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) == 0xC0) return off + 2;
        off += 1 + b;
    }
    return -1;
}

static int parse_response(const uint8_t *pkt, int pkt_len,
                          const char *hostname) {
    if (pkt_len < 12) {
        fprintf(stderr, "nslookup: response too short\n");
        return -1;
    }

    uint16_t flags   = ((uint16_t)pkt[2] << 8) | pkt[3];
    uint16_t qdcount = ((uint16_t)pkt[4] << 8) | pkt[5];
    uint16_t ancount = ((uint16_t)pkt[6] << 8) | pkt[7];

    int rcode = flags & 0x0F;
    if (rcode != 0) {
        const char *reason = "unknown";
        if (rcode == 1) reason = "format error";
        else if (rcode == 2) reason = "server failure";
        else if (rcode == 3) reason = "name does not exist (NXDOMAIN)";
        else if (rcode == 4) reason = "not implemented";
        else if (rcode == 5) reason = "refused";
        printf("Server:  8.8.8.8\n\n");
        printf("** nslookup: %s: %s (rcode=%d)\n", hostname, reason, rcode);
        return -1;
    }

    if (ancount == 0) {
        printf("Server:  8.8.8.8\n\n");
        printf("** nslookup: %s: no answer records\n", hostname);
        return -1;
    }

    int off = 12;
    for (uint16_t i = 0; i < qdcount && off < pkt_len; i++) {
        off = skip_name(pkt, pkt_len, off);
        if (off < 0) return -1;
        off += 4;
    }

    printf("Server:  8.8.8.8\n\n");
    printf("Name:    %s\n", hostname);

    int found = 0;
    for (uint16_t i = 0; i < ancount && off < pkt_len; i++) {
        off = skip_name(pkt, pkt_len, off);
        if (off < 0 || off + 10 > pkt_len) break;

        uint16_t rtype  = ((uint16_t)pkt[off] << 8) | pkt[off + 1];
        uint16_t rdlen  = ((uint16_t)pkt[off + 8] << 8) | pkt[off + 9];
        off += 10;

        if (rtype == DNS_TYPE_A && rdlen == 4 && off + 4 <= pkt_len) {
            printf("Address: %u.%u.%u.%u\n",
                   pkt[off], pkt[off + 1], pkt[off + 2], pkt[off + 3]);
            found++;
        }
        off += rdlen;
    }

    if (!found)
        printf("(no A records found)\n");

    return found > 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: nslookup <hostname>\n");
        return 1;
    }

    const char *hostname = argv[1];
    if (strlen(hostname) > 253) {
        fprintf(stderr, "nslookup: hostname too long\n");
        return 1;
    }

    int fd = sys_socket(2, 2);
    if (fd < 0) {
        fprintf(stderr, "nslookup: socket() failed\n");
        return 1;
    }

    if (sys_bind(fd, htons(10053)) < 0) {
        if (sys_bind(fd, htons(10054)) < 0) {
            fprintf(stderr, "nslookup: bind() failed\n");
            return 1;
        }
    }

    uint16_t txid = (uint16_t)(sys_clock_ms() & 0xFFFF);
    uint8_t query[512];
    int qlen = build_query(query, (int)sizeof(query), hostname, txid);
    if (qlen < 0) {
        fprintf(stderr, "nslookup: failed to build DNS query\n");
        return 1;
    }

    long sent = sys_sendto(fd, query, (unsigned long)qlen,
                           DNS_SERVER_IP, htons(DNS_PORT));
    if (sent < 0) {
        fprintf(stderr, "nslookup: sendto 8.8.8.8:53 failed (%ld)\n", sent);
        return 1;
    }

    uint8_t resp[512];
    struct sockaddr_in_be src;
    long start = sys_clock_ms();
    long n = -1;

    while (sys_clock_ms() - start < DNS_TIMEOUT_MS) {
        n = sys_recvfrom(fd, resp, sizeof(resp), &src);
        if (n > 0) break;
        if (n == -4) return 130;
        sys_yield();
    }

    if (n <= 0) {
        fprintf(stderr, "nslookup: query timed out (no response from 8.8.8.8)\n");
        return 1;
    }

    if (n < 12) {
        fprintf(stderr, "nslookup: truncated response (%ld bytes)\n", n);
        return 1;
    }

    uint16_t resp_id = ((uint16_t)resp[0] << 8) | resp[1];
    if (resp_id != txid) {
        fprintf(stderr, "nslookup: transaction ID mismatch\n");
        return 1;
    }

    return parse_response(resp, (int)n, hostname) < 0 ? 1 : 0;
}
