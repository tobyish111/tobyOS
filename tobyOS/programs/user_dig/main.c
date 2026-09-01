/* dig / host -- a real DNS client, over libtoby's UDP sockets.
 *
 *     dig  [@server] [-t TYPE] [-x addr] [+short] [+tcp-is-not-here] name [TYPE]
 *     host [-t TYPE] [-a] name [server]
 *
 * nslookup already existed here, but it asks for A records and nothing
 * else -- no MX, no TXT, no NS, no reverse lookup, and no way to say which
 * server to ask. Those are the reasons anyone reaches for dig or host, so
 * this is a second resolver rather than a rename of the first.
 *
 * Only UDP is spoken. A truncated answer says so (the TC flag is printed,
 * and `host` says the answer was truncated) rather than being silently
 * returned short -- there is no TCP fallback to hide behind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

static u16 htons_(u16 v) { return (u16)((v >> 8) | (v << 8)); }
#define ntohs_ htons_

/* The kernel's sockaddr for recvfrom: address then port, both big-endian. */
struct src_addr { u32 ip; u16 port; u16 pad; };

#define T_A     1
#define T_NS    2
#define T_CNAME 5
#define T_SOA   6
#define T_PTR  12
#define T_MX   15
#define T_TXT  16
#define T_AAAA 28
#define T_SRV  33
#define T_ANY 255

static const struct { const char *name; int val; } TYPES[] = {
    { "A", T_A }, { "NS", T_NS }, { "CNAME", T_CNAME }, { "SOA", T_SOA },
    { "PTR", T_PTR }, { "MX", T_MX }, { "TXT", T_TXT }, { "AAAA", T_AAAA },
    { "SRV", T_SRV }, { "ANY", T_ANY }, { NULL, 0 }
};

static int type_from_name(const char *s) {
    for (int i = 0; TYPES[i].name; i++) {
        const char *a = TYPES[i].name, *b = s;
        for (;; a++, b++) {
            char ca = *a, cb = *b;
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
            if (ca != cb) break;
            if (!ca) return TYPES[i].val;
        }
    }
    return -1;
}

static const char *type_name(int t) {
    for (int i = 0; TYPES[i].name; i++) if (TYPES[i].val == t) return TYPES[i].name;
    static char buf[16];
    snprintf(buf, sizeof buf, "TYPE%d", t);
    return buf;
}

static const char *rcode_name(int rc) {
    switch (rc) {
    case 0: return "NOERROR";
    case 1: return "FORMERR";
    case 2: return "SERVFAIL";
    case 3: return "NXDOMAIN";
    case 4: return "NOTIMP";
    case 5: return "REFUSED";
    default: return "RCODE";
    }
}

/* ------------------------------------------------------- the nameserver */

static int parse_ipv4(const char *s, u32 *out_be) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s++ - '0');
            if (n > 255) return 0;
        }
        v = (v << 8) | (u32)n;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    /* The socket layer takes the address in network order, and the octets
     * were just read most-significant first, so this is a byte swap. */
    *out_be = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8)
            | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
    return 1;
}

static void fmt_ipv4(u32 be, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u",
             (unsigned)(be & 0xFF), (unsigned)((be >> 8) & 0xFF),
             (unsigned)((be >> 16) & 0xFF), (unsigned)((be >> 24) & 0xFF));
}

/* Enough of RFC 5952 to be readable: the longest run of zero groups is
 * collapsed once. */
static void fmt_ipv6(const u8 *a, char *out, size_t cap) {
    u16 g[8];
    for (int i = 0; i < 8; i++) g[i] = (u16)((a[i * 2] << 8) | a[i * 2 + 1]);
    int best = -1, bestlen = 0;
    for (int i = 0; i < 8; ) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > bestlen && j - i > 1) { best = i; bestlen = j - i; }
        i = j;
    }
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < 8; ) {
        if (i == best) {
            n += (size_t)snprintf(out + n, cap - n, i == 0 ? "::" : ":");
            i += bestlen;
            continue;
        }
        n += (size_t)snprintf(out + n, cap - n, "%s%x", i && i != best ? ":" : "", g[i]);
        if (n >= cap) return;
        i++;
    }
}

static u32 resolver_from_conf(void) {
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;
    char line[256];
    u32 ip = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "nameserver", 10)) continue;
        p += 10;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p;
        while (*e && *e != '\n' && *e != '\r' && *e != ' ' && *e != '\t') e++;
        *e = 0;
        if (parse_ipv4(p, &ip)) break;
        ip = 0;
    }
    fclose(f);
    return ip;
}

/* ---------------------------------------------------------- the message */

static int encode_name(u8 *out, size_t cap, const char *name) {
    size_t n = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0) { p = dot ? dot + 1 : p + len; continue; }
        if (len > 63 || n + len + 1 >= cap) return -1;
        out[n++] = (u8)len;
        memcpy(out + n, p, len);
        n += len;
        if (!dot) break;
        p = dot + 1;
    }
    if (n + 1 > cap) return -1;
    out[n++] = 0;
    return (int)n;
}

/* Returns bytes consumed AT off, writing the decoded name into out. A
 * compression pointer costs 2 bytes here however far it jumps, and the
 * jump budget is what stops a self-referential message from looping. */
static int decode_name(const u8 *msg, size_t len, size_t off, char *out, size_t cap) {
    size_t n = 0, consumed = 0;
    int jumped = 0, budget = 64;
    out[0] = 0;
    for (;;) {
        if (off >= len) return -1;
        u8 l = msg[off];
        if ((l & 0xC0) == 0xC0) {
            if (off + 1 >= len) return -1;
            if (!jumped) consumed = off + 2;
            if (--budget < 0) return -1;
            off = (size_t)(((l & 0x3F) << 8) | msg[off + 1]);
            jumped = 1;
            continue;
        }
        if (l & 0xC0) return -1;
        if (l == 0) {
            if (!jumped) consumed = off + 1;
            if (n == 0 && cap > 1) { out[n++] = '.'; }
            out[n] = 0;
            return (int)consumed;
        }
        if (off + 1 + l > len || n + l + 2 > cap) return -1;
        memcpy(out + n, msg + off + 1, l);
        n += l;
        out[n++] = '.';
        out[n] = 0;
        off += 1 + l;
    }
}

struct rr {
    char  name[256];
    int   type, cls;
    u32   ttl;
    const u8 *rdata;
    size_t rdlen;
};

/* -------------------------------------------------------------- the ask */

static u32 server_be;
static int short_flag, all_flag, quiet_flag;
static const char *prog = "dig";

static int ask(const char *qname, int qtype, u8 *resp, size_t respcap,
               unsigned *msec)
{
    u8 q[512];
    static u16 seq;
    u16 id = (u16)(0x4000 + (++seq) * 7 + (u16)(getpid() & 0x3FFF));

    q[0] = (u8)(id >> 8); q[1] = (u8)id;
    q[2] = 0x01; q[3] = 0x00;          /* RD */
    q[4] = 0; q[5] = 1;                /* QDCOUNT */
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    int n = encode_name(q + 12, sizeof q - 16, qname);
    if (n < 0) { fprintf(stderr, "%s: name too long\n", prog); return -1; }
    size_t len = 12 + (size_t)n;
    q[len++] = (u8)(qtype >> 8); q[len++] = (u8)qtype;
    q[len++] = 0; q[len++] = 1;        /* IN */

    int fd = socket(2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0);
    if (fd < 0) { fprintf(stderr, "%s: cannot open a socket\n", prog); return -1; }
    /* The port is ours to pick; a couple of tries is enough to step past
     * one already in use. */
    int bound = 0;
    for (int try = 0; try < 8 && !bound; try++)
        if (bind(fd, htons_((u16)(20000 + ((id + try * 137) & 0x3FFF)))) >= 0) bound = 1;
    if (!bound) { fprintf(stderr, "%s: cannot bind a local port\n", prog); close(fd); return -1; }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    if (sendto(fd, q, len, server_be, htons_(53)) < 0) {
        fprintf(stderr, "%s: send failed\n", prog);
        close(fd);
        return -1;
    }

    /* The answer has to carry OUR id: a late reply to a previous query
     * arriving on this port would otherwise be reported as this one's. */
    for (int i = 0; i < 4; i++) {
        struct src_addr from;
        long got = recvfrom(fd, resp, respcap, &from);
        if (got < 12) continue;
        if (((resp[0] << 8) | resp[1]) != id) continue;
        gettimeofday(&t1, NULL);
        *msec = (unsigned)((t1.tv_sec - t0.tv_sec) * 1000
                         + (t1.tv_usec - t0.tv_usec) / 1000);
        close(fd);
        return (int)got;
    }
    fprintf(stderr, "%s: no answer from the server\n", prog);
    close(fd);
    return -1;
}

/* Walks the message, filling `out` with the answer section. Returns the
 * count, or -1 if the message does not parse. */
static int walk(const u8 *m, size_t len, struct rr *out, int maxout,
                int *ancount, int *nscount, int *arcount, char *qname_out)
{
    if (len < 12) return -1;
    int qd = (m[4] << 8) | m[5];
    *ancount = (m[6] << 8) | m[7];
    *nscount = (m[8] << 8) | m[9];
    *arcount = (m[10] << 8) | m[11];

    size_t off = 12;
    for (int i = 0; i < qd; i++) {
        char nm[256];
        int c = decode_name(m, len, off, nm, sizeof nm);
        if (c < 0) return -1;
        if (i == 0 && qname_out) memcpy(qname_out, nm, sizeof nm < 256 ? sizeof nm : 256);
        off = (size_t)c + 4;
        if (off > len) return -1;
    }

    int n = 0;
    int total = *ancount + *nscount + *arcount;
    for (int i = 0; i < total && n < maxout; i++) {
        struct rr r;
        int c = decode_name(m, len, off, r.name, sizeof r.name);
        if (c < 0) return -1;
        off = (size_t)c;
        if (off + 10 > len) return -1;
        r.type = (m[off] << 8) | m[off + 1];
        r.cls  = (m[off + 2] << 8) | m[off + 3];
        r.ttl  = ((u32)m[off + 4] << 24) | ((u32)m[off + 5] << 16)
               | ((u32)m[off + 6] << 8) | m[off + 7];
        r.rdlen = (size_t)((m[off + 8] << 8) | m[off + 9]);
        off += 10;
        if (off + r.rdlen > len) return -1;
        r.rdata = m + off;
        off += r.rdlen;
        out[n++] = r;
    }
    return n;
}

/* Renders one record's RDATA the way dig prints it. */
static void fmt_rdata(const u8 *m, size_t len, const struct rr *r,
                      char *out, size_t cap)
{
    size_t base = (size_t)(r->rdata - m);
    switch (r->type) {
    case T_A:
        if (r->rdlen == 4) {
            snprintf(out, cap, "%u.%u.%u.%u", r->rdata[0], r->rdata[1],
                     r->rdata[2], r->rdata[3]);
            return;
        }
        break;
    case T_AAAA:
        if (r->rdlen == 16) { fmt_ipv6(r->rdata, out, cap); return; }
        break;
    case T_NS: case T_CNAME: case T_PTR:
        if (decode_name(m, len, base, out, cap) >= 0) return;
        break;
    case T_MX:
        if (r->rdlen >= 3) {
            char host[256];
            if (decode_name(m, len, base + 2, host, sizeof host) >= 0) {
                snprintf(out, cap, "%u %s",
                         (unsigned)((r->rdata[0] << 8) | r->rdata[1]), host);
                return;
            }
        }
        break;
    case T_TXT: {
        /* TXT rdata is one or more counted strings, not one C string. */
        size_t n = 0, i = 0;
        n += (size_t)snprintf(out + n, cap - n, "\"");
        while (i < r->rdlen && n + 2 < cap) {
            size_t sl = r->rdata[i++];
            if (i + sl > r->rdlen) break;
            for (size_t k = 0; k < sl && n + 2 < cap; k++) out[n++] = (char)r->rdata[i + k];
            i += sl;
        }
        if (n + 2 < cap) out[n++] = '"';
        out[n] = 0;
        return;
    }
    case T_SOA: {
        char mname[256], rname[256];
        int c1 = decode_name(m, len, base, mname, sizeof mname);
        if (c1 > 0) {
            int c2 = decode_name(m, len, (size_t)c1, rname, sizeof rname);
            if (c2 > 0 && (size_t)c2 + 20 <= len) {
                const u8 *v = m + c2;
                snprintf(out, cap, "%s %s %u %u %u %u %u", mname, rname,
                         (unsigned)(((u32)v[0] << 24) | ((u32)v[1] << 16) | ((u32)v[2] << 8) | v[3]),
                         (unsigned)(((u32)v[4] << 24) | ((u32)v[5] << 16) | ((u32)v[6] << 8) | v[7]),
                         (unsigned)(((u32)v[8] << 24) | ((u32)v[9] << 16) | ((u32)v[10] << 8) | v[11]),
                         (unsigned)(((u32)v[12] << 24) | ((u32)v[13] << 16) | ((u32)v[14] << 8) | v[15]),
                         (unsigned)(((u32)v[16] << 24) | ((u32)v[17] << 16) | ((u32)v[18] << 8) | v[19]));
                return;
            }
        }
        break;
    }
    case T_SRV:
        if (r->rdlen >= 7) {
            char host[256];
            if (decode_name(m, len, base + 6, host, sizeof host) >= 0) {
                snprintf(out, cap, "%u %u %u %s",
                         (unsigned)((r->rdata[0] << 8) | r->rdata[1]),
                         (unsigned)((r->rdata[2] << 8) | r->rdata[3]),
                         (unsigned)((r->rdata[4] << 8) | r->rdata[5]), host);
                return;
            }
        }
        break;
    default: break;
    }
    /* Anything unrecognised is shown as its length and raw bytes rather
     * than as nothing -- an empty line reads like an empty answer. */
    size_t n = (size_t)snprintf(out, cap, "\\# %u", (unsigned)r->rdlen);
    for (size_t i = 0; i < r->rdlen && n + 3 < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, " %02x", r->rdata[i]);
}

/* A dotted quad becomes d.c.b.a.in-addr.arpa. */
static int make_arpa(const char *ip, char *out, size_t cap) {
    unsigned a, b, c, d;
    const char *p = ip;
    unsigned v[4];
    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') return 0;
        unsigned n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (unsigned)(*p++ - '0'); if (n > 255) return 0; }
        v[i] = n;
        if (i < 3) { if (*p != '.') return 0; p++; }
    }
    if (*p) return 0;
    a = v[0]; b = v[1]; c = v[2]; d = v[3];
    snprintf(out, cap, "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
    return 1;
}

/* ---------------------------------------------------------------- dig */

static int run_dig(const char *qname, int qtype) {
    static u8 resp[4096];
    unsigned msec = 0;
    int len = ask(qname, qtype, resp, sizeof resp, &msec);
    if (len < 0) return 9;

    struct rr rrs[64];
    int an, ns, ar;
    char qn[256] = "";
    int n = walk(resp, (size_t)len, rrs, 64, &an, &ns, &ar, qn);
    if (n < 0) { fprintf(stderr, "dig: the answer did not parse\n"); return 9; }

    int rcode = resp[3] & 0x0F;
    char srv[24];
    fmt_ipv4(server_be, srv, sizeof srv);

    if (short_flag) {
        for (int i = 0; i < an && i < n; i++) {
            if (qtype != T_ANY && rrs[i].type != qtype && rrs[i].type != T_CNAME) continue;
            char rd[1024];
            fmt_rdata(resp, (size_t)len, &rrs[i], rd, sizeof rd);
            printf("%s\n", rd);
        }
        return rcode ? 9 : 0;
    }

    printf("\n; <<>> DiG (tobyOS) <<>> %s %s\n", qname, type_name(qtype));
    printf(";; Got answer:\n");
    printf(";; ->>HEADER<<- opcode: QUERY, status: %s, id: %u\n",
           rcode_name(rcode), (unsigned)((resp[0] << 8) | resp[1]));
    printf(";; flags:%s%s%s%s%s; QUERY: %u, ANSWER: %d, AUTHORITY: %d, ADDITIONAL: %d\n",
           (resp[2] & 0x80) ? " qr" : "", (resp[2] & 0x04) ? " aa" : "",
           (resp[2] & 0x02) ? " tc" : "", (resp[2] & 0x01) ? " rd" : "",
           (resp[3] & 0x80) ? " ra" : "",
           (unsigned)((resp[4] << 8) | resp[5]), an, ns, ar);
    if (resp[2] & 0x02)
        printf(";; WARNING: the answer is TRUNCATED and this resolver has no TCP fallback\n");

    printf("\n;; QUESTION SECTION:\n;%s\t\t\tIN\t%s\n", qn, type_name(qtype));

    int idx = 0;
    const struct { const char *title; int count; } secs[3] = {
        { "ANSWER", an }, { "AUTHORITY", ns }, { "ADDITIONAL", ar }
    };
    for (int s = 0; s < 3; s++) {
        if (secs[s].count == 0) continue;
        printf("\n;; %s SECTION:\n", secs[s].title);
        for (int i = 0; i < secs[s].count && idx < n; i++, idx++) {
            char rd[1024];
            fmt_rdata(resp, (size_t)len, &rrs[idx], rd, sizeof rd);
            printf("%s\t%u\tIN\t%s\t%s\n", rrs[idx].name, (unsigned)rrs[idx].ttl,
                   type_name(rrs[idx].type), rd);
        }
    }

    printf("\n;; Query time: %u msec\n", msec);
    printf(";; SERVER: %s#53(%s)\n", srv, srv);
    printf(";; MSG SIZE  rcvd: %d\n\n", len);
    return rcode ? 9 : 0;
}

/* --------------------------------------------------------------- host */

static int host_one(const char *qname, int qtype, const char *display) {
    static u8 resp[4096];
    unsigned msec = 0;
    int len = ask(qname, qtype, resp, sizeof resp, &msec);
    if (len < 0) return 1;

    struct rr rrs[64];
    int an, ns, ar;
    int n = walk(resp, (size_t)len, rrs, 64, &an, &ns, &ar, NULL);
    if (n < 0) { fprintf(stderr, "host: the answer did not parse\n"); return 1; }

    int rcode = resp[3] & 0x0F;
    if (rcode == 3) { printf("Host %s not found: 3(NXDOMAIN)\n", display); return 1; }
    if (rcode) { printf("Host %s lookup failed: %d(%s)\n", display, rcode, rcode_name(rcode)); return 1; }
    if (resp[2] & 0x02)
        fprintf(stderr, "host: the answer was truncated (no TCP fallback here)\n");

    int printed = 0;
    for (int i = 0; i < an && i < n; i++) {
        char rd[1024];
        fmt_rdata(resp, (size_t)len, &rrs[i], rd, sizeof rd);
        /* Trailing dots read as noise in host's sentences except after a
         * name, where BIND keeps them. */
        switch (rrs[i].type) {
        case T_A:     printf("%s has address %s\n", display, rd); break;
        case T_AAAA:  printf("%s has IPv6 address %s\n", display, rd); break;
        case T_MX:    printf("%s mail is handled by %s\n", display, rd); break;
        case T_NS:    printf("%s name server %s\n", display, rd); break;
        case T_PTR:   printf("%s domain name pointer %s\n", display, rd); break;
        case T_TXT:   printf("%s descriptive text %s\n", display, rd); break;
        case T_CNAME: printf("%s is an alias for %s\n", display, rd); break;
        case T_SOA:   printf("%s has SOA record %s\n", display, rd); break;
        default:      printf("%s has %s record %s\n", display, type_name(rrs[i].type), rd); break;
        }
        printed++;
    }
    if (!printed && !quiet_flag) printf("%s has no %s record\n", display, type_name(qtype));
    return 0;
}

static int run_host(const char *name, int qtype, int explicit_type) {
    char arpa[80];
    if (make_arpa(name, arpa, sizeof arpa)) {
        /* An address argument means a reverse lookup, whatever -t said. */
        return host_one(arpa, T_PTR, arpa);
    }
    if (explicit_type) return host_one(name, qtype, name);
    /* Plain `host name` asks for the three things it prints by default. */
    int bad = 0;
    quiet_flag = 1;
    bad |= host_one(name, T_A, name);
    host_one(name, T_AAAA, name);
    host_one(name, T_MX, name);
    quiet_flag = 0;
    return bad;
}

/* --------------------------------------------------------------- main */

static void usage(void) {
    if (!strcmp(prog, "host"))
        fprintf(stderr, "usage: host [-t TYPE] [-a] name [server]\n");
    else
        fprintf(stderr, "usage: dig [@server] [-t TYPE] [-x addr] [+short] name [TYPE]\n");
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "dig";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    int is_host = !strcmp(base, "host");

    const char *name = NULL, *srv = NULL;
    int qtype = -1, explicit_type = 0;
    char revbuf[80];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '@') { srv = a + 1; continue; }
        if (!strcmp(a, "+short")) { short_flag = 1; continue; }
        if (a[0] == '+') continue;                 /* other dig knobs: accepted, ignored */
        if (!strcmp(a, "-t") && i + 1 < argc) {
            qtype = type_from_name(argv[++i]);
            if (qtype < 0) { fprintf(stderr, "%s: unknown type %s\n", prog, argv[i]); return 1; }
            explicit_type = 1;
            continue;
        }
        if (!strcmp(a, "-x") && i + 1 < argc) {
            if (!make_arpa(argv[++i], revbuf, sizeof revbuf)) {
                fprintf(stderr, "%s: -x wants a dotted-quad address\n", prog);
                return 1;
            }
            name = revbuf;
            qtype = T_PTR;
            explicit_type = 1;
            continue;
        }
        if (!strcmp(a, "-a")) { qtype = T_ANY; explicit_type = 1; all_flag = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("%s (tobyOS) 1.0\n", prog); return 0;
        }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "%s: unknown option %s\n", prog, a); return 1; }
        if (!name) name = a;
        else if (is_host && !srv) srv = a;
        else if (!explicit_type) {
            int t = type_from_name(a);          /* dig's trailing TYPE */
            if (t >= 0) { qtype = t; explicit_type = 1; }
        }
    }

    if (!name) { usage(); return 1; }

    if (srv) {
        if (!parse_ipv4(srv, &server_be)) {
            fprintf(stderr, "%s: the server must be a dotted-quad address\n", prog);
            return 1;
        }
    } else {
        server_be = resolver_from_conf();
        if (!server_be) {
            /* No resolv.conf yet (DHCP writes it): a public resolver keeps
             * the tool usable, and it is said out loud. */
            parse_ipv4("8.8.8.8", &server_be);
            if (!short_flag)
                fprintf(stderr, "%s: no nameserver in /etc/resolv.conf, using 8.8.8.8\n", prog);
        }
    }

    if (is_host) return run_host(name, qtype < 0 ? T_A : qtype, explicit_type);
    return run_dig(name, qtype < 0 ? T_A : qtype);
}
