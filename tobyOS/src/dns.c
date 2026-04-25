/* dns.c -- DNS resolver (RFC 1035 subset).
 *
 * Single in-flight transaction. Caller invokes dns_resolve() which:
 *   1. arms g_dns (in_flight=true, picks an id),
 *   2. builds + sends a query,
 *   3. polls the NIC RX path until got_reply | got_error | timeout,
 *   4. returns.
 *
 * Wire receive flows from udp_recv -> dns_recv_hook(). The hook
 * verifies the id, walks past the question section, and grabs the
 * first A record's RDATA into g_dns.answer_ip_be. CNAMEs are skipped
 * silently in the answer loop -- we just keep walking until we find
 * an A or run out of records.
 *
 * Compression pointers: we never EMIT them. We always SKIP past them
 * in answers (one trailing 2-byte pointer is the common case for
 * "name in answer matches the question name"). We don't dereference
 * them because we don't actually need the answer's name to extract
 * its RDATA -- TYPE/CLASS/TTL/RDLENGTH/RDATA all live AFTER the name
 * regardless of how it was encoded.
 */

#include <tobyos/dns.h>
#include <tobyos/net.h>
#include <tobyos/udp.h>
#include <tobyos/pit.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- transaction state ----------------------------------------- */

struct dns_xact {
    bool      in_flight;
    uint16_t  id;                  /* host-order, mirrored back in flags  */
    bool      got_reply;
    bool      got_error;
    uint8_t   rcode;
    uint32_t  answer_ip_be;
    uint32_t  answer_ttl;          /* host-order seconds                  */
};

static struct dns_xact g_dns;

/* ---- name codec ------------------------------------------------- */

/* Encode "example.com" into out as `7example3com\0`. Returns the
 * number of bytes written including the trailing zero, or 0 on error
 * (label > 63, total > 255, no room in `cap`). Trailing dot in the
 * input name is tolerated. */
static size_t dns_encode_name(const char *name, uint8_t *out, size_t cap) {
    if (!name || !out) return 0;
    size_t out_pos = 0;
    const char *p = name;

    while (*p) {
        const char *start = p;
        while (*p && *p != '.') p++;
        size_t llen = (size_t)(p - start);
        if (llen == 0) {
            /* Empty label = "..", invalid except a single trailing dot
             * which is consumed below. Treat anything else as bad. */
            if (*p == '.' && *(p + 1) == 0) break;
            return 0;
        }
        if (llen > 63) return 0;
        if (out_pos + 1 + llen + 1 > cap) return 0;
        out[out_pos++] = (uint8_t)llen;
        memcpy(&out[out_pos], start, llen);
        out_pos += llen;
        if (*p == '.') p++;
    }

    if (out_pos == 0) return 0;            /* "" or "."  -> reject */
    if (out_pos + 1 > cap) return 0;
    out[out_pos++] = 0;                    /* root label */
    if (out_pos > 255) return 0;           /* RFC 1035 §2.3.4: max 255 */
    return out_pos;
}

/* Step past a name field starting at off. Returns the new offset
 * (one past the terminating zero or pointer), or 0 on malformed
 * input. We never need to materialise the name -- we just need to
 * know where it ends so we can read TYPE/CLASS/... that follows. */
static size_t dns_skip_name(const uint8_t *p, size_t total, size_t off) {
    /* Bound the walk so a label-loop attack can't hang us. */
    int hops = 0;
    while (off < total) {
        if (++hops > 256) return 0;        /* corrupt: too many labels */
        uint8_t b = p[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) == 0xC0) {
            if (off + 1 >= total) return 0;
            return off + 2;                /* compression pointer is 2 bytes total */
        }
        if (b > 63) return 0;              /* invalid label length */
        if (off + 1 + b >= total) return 0;
        off += 1 + b;
    }
    return 0;
}

/* ---- query builder ---------------------------------------------- */

static size_t dns_build_query(uint16_t id, const char *name,
                              uint8_t *out, size_t cap) {
    if (cap < DNS_HDR_LEN + 6) return 0;     /* hdr + 1-byte name + qtype + qclass */
    struct dns_hdr *h = (struct dns_hdr *)out;
    memset(h, 0, DNS_HDR_LEN);
    h->id_hi  = (uint8_t)(id >> 8);
    h->id_lo  = (uint8_t)(id & 0xFF);
    h->flags0 = 0x01;                        /* RD = 1 (recurse), QR = 0 */
    h->flags1 = 0x00;
    h->qd_lo  = 1;

    size_t name_len = dns_encode_name(name, out + DNS_HDR_LEN,
                                      cap - DNS_HDR_LEN - 4);
    if (name_len == 0) return 0;

    size_t off = DNS_HDR_LEN + name_len;
    /* QTYPE = 1 (A), QCLASS = 1 (IN), both big-endian. */
    out[off++] = 0;  out[off++] = DNS_TYPE_A;
    out[off++] = 0;  out[off++] = DNS_CLASS_IN;
    return off;
}

/* ---- RX hook ---------------------------------------------------- */

static const char *dns_rcode_name(uint8_t rc) {
    switch (rc) {
    case DNS_RCODE_OK:       return "NOERROR";
    case DNS_RCODE_FORMERR:  return "FORMERR";
    case DNS_RCODE_SERVFAIL: return "SERVFAIL";
    case DNS_RCODE_NXDOMAIN: return "NXDOMAIN";
    case DNS_RCODE_NOTIMP:   return "NOTIMP";
    case DNS_RCODE_REFUSED:  return "REFUSED";
    default:                 return "?";
    }
}

void dns_recv_hook(uint32_t src_ip_be, const void *udp_packet, size_t len) {
    (void)src_ip_be;
    if (!g_dns.in_flight) return;
    if (len < 8 + DNS_HDR_LEN) return;       /* UDP hdr + DNS hdr */

    /* Skip 8-byte UDP header. */
    const uint8_t *d  = (const uint8_t *)udp_packet + 8;
    size_t         dl = len - 8;

    const struct dns_hdr *h = (const struct dns_hdr *)d;
    uint16_t id = ((uint16_t)h->id_hi << 8) | h->id_lo;
    if (id != g_dns.id) return;              /* not our transaction */

    if ((h->flags0 & 0x80) == 0) return;     /* QR must be 1 (response) */

    uint8_t rcode = h->flags1 & 0x0F;
    if (rcode != DNS_RCODE_OK) {
        kprintf("[dns] reply id=0x%04x rcode=%u (%s)\n",
                id, rcode, dns_rcode_name(rcode));
        g_dns.rcode     = rcode;
        g_dns.got_error = true;
        return;
    }

    uint16_t qd = ((uint16_t)h->qd_hi << 8) | h->qd_lo;
    uint16_t an = ((uint16_t)h->an_hi << 8) | h->an_lo;

    size_t off = DNS_HDR_LEN;

    /* Skip the question section (the server echoes our QNAME/QTYPE/QCLASS). */
    for (uint16_t i = 0; i < qd; i++) {
        off = dns_skip_name(d, dl, off);
        if (off == 0 || off + 4 > dl) return;
        off += 4;                            /* QTYPE + QCLASS */
    }

    /* Walk answers. Take the first A record we find; skip CNAMEs. */
    for (uint16_t i = 0; i < an; i++) {
        off = dns_skip_name(d, dl, off);
        if (off == 0 || off + 10 > dl) return;
        uint16_t type = ((uint16_t)d[off] << 8) | d[off + 1];
        uint16_t cls  = ((uint16_t)d[off + 2] << 8) | d[off + 3];
        uint32_t ttl  = ((uint32_t)d[off + 4] << 24) |
                        ((uint32_t)d[off + 5] << 16) |
                        ((uint32_t)d[off + 6] <<  8) |
                        ((uint32_t)d[off + 7]);
        uint16_t rdl  = ((uint16_t)d[off + 8] << 8) | d[off + 9];
        off += 10;
        if (off + rdl > dl) return;

        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdl == 4) {
            memcpy(&g_dns.answer_ip_be, &d[off], 4);
            g_dns.answer_ttl = ttl;
            g_dns.got_reply  = true;
            return;
        }
        off += rdl;
    }

    /* No A record present (CNAME chain, AAAA-only, etc.). */
    kprintf("[dns] reply id=0x%04x had %u answer(s) but no A record\n",
            id, (unsigned)an);
    g_dns.rcode     = DNS_RCODE_OK;
    g_dns.got_error = true;
}

/* ---- main entry point ------------------------------------------- */

static uint16_t dns_make_id(void) {
    /* Same xid trick as DHCP: we just need it to round-trip. */
    uint64_t t = (uint64_t)pit_ticks() * 0x9E3779B97F4A7C15ull;
    t ^= ((uint64_t)g_my_mac[3] << 0)
      |  ((uint64_t)g_my_mac[4] << 8)
      |  ((uint64_t)g_my_mac[5] << 16);
    return (uint16_t)(t ^ (t >> 16));
}

bool dns_resolve(const char *name, uint32_t timeout_ms, struct dns_result *out) {
    if (!out)              return false;
    if (!name || !*name)   return false;
    if (g_my_dns_be == 0) {
        kprintf("[dns] no DNS server (DHCP did not provide one)\n");
        return false;
    }

    /* Build query into a heap-free local buffer. */
    uint8_t qbuf[256 + DNS_HDR_LEN + 4];
    memset(&g_dns, 0, sizeof(g_dns));
    g_dns.id = dns_make_id();
    size_t qlen = dns_build_query(g_dns.id, name, qbuf, sizeof(qbuf));
    if (qlen == 0) {
        kprintf("[dns] '%s' rejected: bad hostname\n", name);
        return false;
    }

    char serverbuf[16];
    net_format_ip(serverbuf, g_my_dns_be);
    kprintf("[dns] query  id=0x%04x %s @ %s (qbytes=%u)\n",
            g_dns.id, name, serverbuf, (unsigned)qlen);

    g_dns.in_flight = true;

    /* First attempt. If it fails (commonly an ARP cache miss for the
     * DNS server's MAC), drain RX briefly so the ARP reply lands in
     * the cache, then retry once. net_init pre-warms the DNS ARP
     * already, but we keep this here so the resolver works even when
     * called from a freshly-renewed lease where the DNS server may
     * have changed. */
    bool sent = udp_send(htons(DNS_PORT_CLIENT), g_my_dns_be,
                         htons(DNS_PORT_SERVER), qbuf, qlen);
    if (!sent) {
        struct net_dev *nd0 = net_default();
        uint32_t hz0 = pit_hz(); if (hz0 == 0) hz0 = 100;
        uint64_t arp_deadline = pit_ticks() + hz0 / 2;        /* ~500 ms */
        while (pit_ticks() < arp_deadline) {
            if (nd0 && nd0->rx_drain) nd0->rx_drain(nd0);
            sti(); hlt();
        }
        sent = udp_send(htons(DNS_PORT_CLIENT), g_my_dns_be,
                        htons(DNS_PORT_SERVER), qbuf, qlen);
    }
    if (!sent) {
        kprintf("[dns] send failed twice (NIC tx / ARP miss for %s)\n",
                serverbuf);
        g_dns.in_flight = false;
        return false;
    }

    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t deadline = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;

    struct net_dev *nd = net_default();
    while (pit_ticks() < deadline) {
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        if (g_dns.got_reply) break;
        if (g_dns.got_error) break;
        sti();
        hlt();
    }
    g_dns.in_flight = false;

    if (g_dns.got_reply) {
        char ipbuf[16];
        net_format_ip(ipbuf, g_dns.answer_ip_be);
        kprintf("[dns] reply  id=0x%04x %s -> %s (ttl=%us)\n",
                g_dns.id, name, ipbuf, (unsigned)g_dns.answer_ttl);
        out->ip_be    = g_dns.answer_ip_be;
        out->ttl_secs = g_dns.answer_ttl;
        return true;
    }

    if (g_dns.got_error) {
        kprintf("[dns] '%s' failed: rcode=%u (%s)\n",
                name, g_dns.rcode, dns_rcode_name(g_dns.rcode));
    } else {
        kprintf("[dns] '%s' failed: timeout after %u ms\n",
                name, (unsigned)timeout_ms);
    }
    return false;
}
