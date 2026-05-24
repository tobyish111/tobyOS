/* mdns.c -- mDNS responder/browser for tobyOS.
 *
 * Listens on multicast 224.0.0.251:5353, parses DNS records,
 * registers local hostname as HOSTNAME.local, responds to queries
 * for our name, and browses for services.
 */

#include <tobyos/types.h>
#include <tobyos/net.h>
#include <tobyos/udp.h>
#include <tobyos/pit.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define MDNS_PORT          5353
#define MDNS_MULTICAST_IP  0xFB0000E0  /* 224.0.0.251 in network byte order */
#define MDNS_MAX_NAME      128
#define MDNS_MAX_SERVICES  16
#define MDNS_TTL           120
#define MDNS_MAX_RECORDS   32

/* DNS record types */
#define DNS_TYPE_A          1
#define DNS_TYPE_AAAA       28
#define DNS_TYPE_PTR        12
#define DNS_TYPE_SRV        33
#define DNS_TYPE_TXT        16
#define DNS_TYPE_ANY        255

/* DNS header flags */
#define DNS_QR_RESPONSE     0x8400  /* response + authoritative */
#define DNS_QR_QUERY        0x0000

struct __attribute__((packed)) dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
};

struct mdns_service {
    char     name[64];
    char     type[64];
    uint16_t port;
    char     txt[128];
    bool     active;
};

struct mdns_record {
    char     name[MDNS_MAX_NAME];
    uint16_t type;
    uint32_t ip;
    uint16_t port;
    uint16_t priority;
    uint16_t weight;
    char     target[MDNS_MAX_NAME];
    char     txt[128];
    uint32_t ttl;
    uint64_t discovered_ms;
};

static char g_mdns_hostname[64] = "tobyos";
static struct mdns_service g_mdns_services[MDNS_MAX_SERVICES];
static int g_mdns_service_count;
static struct mdns_record g_mdns_discovered[MDNS_MAX_RECORDS];
static int g_mdns_discovered_count;
static bool g_mdns_running;

static void mdns_strncpy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (i < max && src[i]) { dst[i] = src[i]; i++; }
    if (i < max + 1) dst[i] = '\0';
}

/* Encode a DNS name into wire format (labels). Returns bytes written. */
static size_t mdns_encode_name(uint8_t *buf, size_t cap, const char *name) {
    size_t off = 0;
    const char *p = name;

    while (*p && off < cap - 1) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        size_t label_len = (size_t)(dot - p);
        if (label_len > 63 || off + 1 + label_len >= cap) break;
        buf[off++] = (uint8_t)label_len;
        memcpy(buf + off, p, label_len);
        off += label_len;
        p = dot;
        if (*p == '.') p++;
    }
    if (off < cap) buf[off++] = 0;
    return off;
}

/* Decode a DNS name from wire format. Handles compression pointers. */
static size_t mdns_decode_name(const uint8_t *pkt, size_t pkt_len,
                               size_t offset, char *out, size_t out_cap) {
    size_t pos = offset;
    size_t out_pos = 0;
    size_t end_pos = 0;
    int hops = 0;

    while (pos < pkt_len && hops < 16) {
        uint8_t len = pkt[pos];
        if (len == 0) {
            pos++;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len) break;
            if (end_pos == 0) end_pos = pos + 2;
            pos = ((size_t)(len & 0x3F) << 8) | pkt[pos + 1];
            hops++;
            continue;
        }
        pos++;
        if (pos + len > pkt_len) break;
        if (out_pos > 0 && out_pos < out_cap - 1)
            out[out_pos++] = '.';
        size_t copy = len;
        if (out_pos + copy >= out_cap) copy = out_cap - out_pos - 1;
        memcpy(out + out_pos, pkt + pos, copy);
        out_pos += copy;
        pos += len;
    }
    if (out_pos < out_cap) out[out_pos] = '\0';
    return end_pos ? end_pos : pos;
}

/* Build and send an mDNS response for our hostname */
static void mdns_send_response(void) {
    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));

    struct dns_header *hdr = (struct dns_header *)pkt;
    hdr->flags = htons(DNS_QR_RESPONSE);
    hdr->ancount = htons(1);

    size_t off = sizeof(struct dns_header);

    /* Our hostname.local A record */
    char fqdn[128];
    ksnprintf(fqdn, sizeof(fqdn), "%s.local", g_mdns_hostname);
    off += mdns_encode_name(pkt + off, sizeof(pkt) - off, fqdn);

    /* Type A, class IN (cache flush bit set) */
    *(uint16_t *)(pkt + off) = htons(DNS_TYPE_A); off += 2;
    *(uint16_t *)(pkt + off) = htons(0x8001); off += 2;
    /* TTL */
    *(uint32_t *)(pkt + off) = htonl(MDNS_TTL); off += 4;
    /* RDLENGTH */
    *(uint16_t *)(pkt + off) = htons(4); off += 2;
    /* RDATA: our IP */
    *(uint32_t *)(pkt + off) = g_my_ip; off += 4;

    /* Send to multicast */
    udp_send(htons(MDNS_PORT), MDNS_MULTICAST_IP, htons(MDNS_PORT), pkt, off);
}

/* Check if a query is for our hostname */
static bool mdns_is_our_name(const char *name) {
    char fqdn[128];
    ksnprintf(fqdn, sizeof(fqdn), "%s.local", g_mdns_hostname);

    /* Case-insensitive compare */
    const char *a = fqdn;
    const char *b = name;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Process an incoming mDNS packet */
static void mdns_process_packet(const uint8_t *pkt, size_t len) {
    if (len < sizeof(struct dns_header)) return;

    struct dns_header *hdr = (struct dns_header *)pkt;
    uint16_t flags = ntohs(hdr->flags);
    uint16_t qdcount = ntohs(hdr->qdcount);
    uint16_t ancount = ntohs(hdr->ancount);

    size_t off = sizeof(struct dns_header);

    /* Process queries */
    if (!(flags & 0x8000)) {
        for (uint16_t i = 0; i < qdcount && off < len; i++) {
            char qname[MDNS_MAX_NAME];
            off = mdns_decode_name(pkt, len, off, qname, sizeof(qname));
            if (off + 4 > len) break;
            uint16_t qtype = ntohs(*(uint16_t *)(pkt + off)); off += 2;
            off += 2; /* qclass */

            if ((qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY) &&
                mdns_is_our_name(qname)) {
                mdns_send_response();
            }
        }
        return;
    }

    /* Process answers (for service discovery) */
    for (uint16_t i = 0; i < qdcount && off < len; i++) {
        char dummy[MDNS_MAX_NAME];
        off = mdns_decode_name(pkt, len, off, dummy, sizeof(dummy));
        if (off + 4 > len) break;
        off += 4;
    }

    for (uint16_t i = 0; i < ancount && off < len; i++) {
        char rname[MDNS_MAX_NAME];
        off = mdns_decode_name(pkt, len, off, rname, sizeof(rname));
        if (off + 10 > len) break;

        uint16_t rtype = ntohs(*(uint16_t *)(pkt + off)); off += 2;
        off += 2; /* class */
        uint32_t ttl = ntohl(*(uint32_t *)(pkt + off)); off += 4;
        uint16_t rdlen = ntohs(*(uint16_t *)(pkt + off)); off += 2;

        if (off + rdlen > len) break;

        /* Store discovered records */
        if (g_mdns_discovered_count < MDNS_MAX_RECORDS) {
            struct mdns_record *rec = &g_mdns_discovered[g_mdns_discovered_count];
            mdns_strncpy(rec->name, rname, MDNS_MAX_NAME - 1);
            rec->type = rtype;
            rec->ttl = ttl;
            rec->discovered_ms = pit_ticks() * 1000 / pit_hz();

            switch (rtype) {
            case DNS_TYPE_A:
                if (rdlen >= 4)
                    rec->ip = *(uint32_t *)(pkt + off);
                break;
            case DNS_TYPE_SRV:
                if (rdlen >= 6) {
                    rec->priority = ntohs(*(uint16_t *)(pkt + off));
                    rec->weight = ntohs(*(uint16_t *)(pkt + off + 2));
                    rec->port = ntohs(*(uint16_t *)(pkt + off + 4));
                    mdns_decode_name(pkt, len, off + 6, rec->target,
                                     sizeof(rec->target));
                }
                break;
            case DNS_TYPE_TXT:
                if (rdlen > 0 && rdlen < sizeof(rec->txt)) {
                    size_t tlen = pkt[off];
                    if (tlen > rdlen - 1) tlen = rdlen - 1;
                    memcpy(rec->txt, pkt + off + 1, tlen);
                    rec->txt[tlen] = '\0';
                }
                break;
            case DNS_TYPE_PTR:
                mdns_decode_name(pkt, len, off, rec->target, sizeof(rec->target));
                break;
            default:
                break;
            }
            g_mdns_discovered_count++;
        }
        off += rdlen;
    }
}

/* Called from UDP receive path when port 5353 data arrives */
void mdns_recv_hook(uint32_t src_ip_be, const void *data, size_t len) {
    (void)src_ip_be;
    if (!g_mdns_running) return;
    mdns_process_packet((const uint8_t *)data, len);
}

/* Browse for a service type (e.g. "_http._tcp.local") */
bool mdns_browse(const char *service_type) {
    uint8_t pkt[256];
    memset(pkt, 0, sizeof(pkt));

    struct dns_header *hdr = (struct dns_header *)pkt;
    hdr->flags = htons(DNS_QR_QUERY);
    hdr->qdcount = htons(1);

    size_t off = sizeof(struct dns_header);
    off += mdns_encode_name(pkt + off, sizeof(pkt) - off, service_type);

    *(uint16_t *)(pkt + off) = htons(DNS_TYPE_PTR); off += 2;
    *(uint16_t *)(pkt + off) = htons(0x0001); off += 2;

    return udp_send(htons(MDNS_PORT), MDNS_MULTICAST_IP, htons(MDNS_PORT), pkt, off);
}

/* Register a local service */
bool mdns_register_service(const char *name, const char *type, uint16_t port,
                           const char *txt) {
    if (g_mdns_service_count >= MDNS_MAX_SERVICES) return false;

    struct mdns_service *svc = &g_mdns_services[g_mdns_service_count++];
    mdns_strncpy(svc->name, name, sizeof(svc->name) - 1);
    mdns_strncpy(svc->type, type, sizeof(svc->type) - 1);
    svc->port = port;
    if (txt) mdns_strncpy(svc->txt, txt, sizeof(svc->txt) - 1);
    svc->active = true;

    kprintf("[mdns] registered service: %s (%s) port %u\n", name, type, port);
    return true;
}

void mdns_set_hostname(const char *name) {
    mdns_strncpy(g_mdns_hostname, name, sizeof(g_mdns_hostname) - 1);
}

const char *mdns_hostname(void) {
    return g_mdns_hostname;
}

int mdns_get_discovered(struct mdns_record *out, int cap) {
    int count = g_mdns_discovered_count < cap ? g_mdns_discovered_count : cap;
    if (count > 0)
        memcpy(out, g_mdns_discovered, (size_t)count * sizeof(struct mdns_record));
    return count;
}

void mdns_announce(void) {
    mdns_send_response();
}

void mdns_init(void) {
    g_mdns_service_count = 0;
    g_mdns_discovered_count = 0;
    g_mdns_running = true;
    mdns_send_response();
    kprintf("[mdns] started: %s.local\n", g_mdns_hostname);
}
