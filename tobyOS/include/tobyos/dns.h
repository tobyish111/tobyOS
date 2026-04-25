/* dns.h -- DNS resolver (RFC 1035 subset).
 *
 * Tiny synchronous one-shot resolver. Single in-flight transaction at a
 * time -- we keep a fixed kernel-side UDP source port (53999) and a
 * single transaction record. That's enough for boot-time hostname
 * lookups, the `nslookup` shell builtin, and HTTP GET. When 24D needs
 * concurrent connections we can grow the table, but the wire codec
 * stays the same.
 *
 * Pre-conditions:
 *   - g_my_dns_be != 0 (DHCP populated it -- see net.h)
 *   - net_default() != 0 (a NIC is bound)
 *   - The gateway ARP entry is warmed up (net_init() does this)
 *
 * Limits / non-goals (Milestone 24B):
 *   - A records only (TYPE=1, CLASS=1).
 *   - First answer wins (no round-robin or load-balancing).
 *   - No caching.
 *   - No retry, no fallback DNS server.
 *   - No CNAME chasing -- if the only A record is hidden behind a
 *     CNAME and the server doesn't include the inlined A, we fail.
 *     SLIRP/dnsmasq always inline; real resolvers do too.
 */

#ifndef TOBYOS_DNS_H
#define TOBYOS_DNS_H

#include <tobyos/types.h>

#define DNS_PORT_SERVER     53
#define DNS_PORT_CLIENT  53999       /* fixed kernel-reserved high port */

/* RFC 1035 record TYPE / CLASS values we care about. */
#define DNS_TYPE_A          1
#define DNS_TYPE_CNAME      5
#define DNS_CLASS_IN        1

/* RFC 1035 RCODE (low nibble of flags1). */
#define DNS_RCODE_OK        0
#define DNS_RCODE_FORMERR   1
#define DNS_RCODE_SERVFAIL  2
#define DNS_RCODE_NXDOMAIN  3
#define DNS_RCODE_NOTIMP    4
#define DNS_RCODE_REFUSED   5

/* The 12-byte fixed DNS header. All multi-byte fields are big-endian
 * on the wire, so we DON'T mark these `uint16_t` -- using two `uint8_t`
 * fields makes the bit-shifting explicit at the call site (and avoids
 * any confusion about whether QR/Opcode/RD live in flags[0] or flags[1]). */
struct __attribute__((packed)) dns_hdr {
    uint8_t  id_hi;
    uint8_t  id_lo;
    uint8_t  flags0;        /* QR | Opcode(4) | AA | TC | RD             */
    uint8_t  flags1;        /* RA | Z(3) | RCODE(4)                      */
    uint8_t  qd_hi, qd_lo;  /* question count                            */
    uint8_t  an_hi, an_lo;  /* answer count                              */
    uint8_t  ns_hi, ns_lo;  /* authority count                           */
    uint8_t  ar_hi, ar_lo;  /* additional count                          */
};

#define DNS_HDR_LEN sizeof(struct dns_hdr)

/* Result of a successful resolve. ip_be is the first A record, in
 * network byte order so it can be assigned straight into the existing
 * IP-family helpers. ttl_secs is in HOST byte order. */
struct dns_result {
    uint32_t ip_be;
    uint32_t ttl_secs;
};

/* Synchronous resolver.
 *
 * Sends one query for `name` (e.g. "example.com") to g_my_dns_be:53
 * and blocks for up to timeout_ms while polling the NIC for the
 * reply. Caller must NOT hold any spinlocks (we sti+hlt).
 *
 * Returns true on success; out is filled with the first A record.
 * Returns false on:
 *   - g_my_dns_be == 0 (no DNS server -- DHCP didn't run / failed)
 *   - bad hostname (NULL, empty, label > 63, total > 255)
 *   - udp_send failed (no NIC, ARP miss for next-hop, etc.)
 *   - server timeout
 *   - server error (FORMERR, SERVFAIL, NXDOMAIN, NOTIMP, REFUSED)
 *   - response missing any A record (e.g. CNAME-only chain) */
bool dns_resolve(const char *name, uint32_t timeout_ms, struct dns_result *out);

/* Hook from udp_recv() for the resolver port (53999). Always safe to
 * call: if no resolution is in flight, drops silently. */
void dns_recv_hook(uint32_t src_ip_be, const void *udp_packet, size_t len);

#endif /* TOBYOS_DNS_H */
