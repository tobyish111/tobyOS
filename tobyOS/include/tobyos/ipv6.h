/* ipv6.h -- IPv6 packet send/receive (RFC 8200).
 *
 * Link-local only for now: auto-configured from the NIC MAC via EUI-64.
 * No SLAAC, no DHCPv6, no global/ULA addresses. Provides the foundation
 * for neighbor discovery (ICMPv6) and future TCP/UDP over IPv6.
 */

#ifndef TOBYOS_IPV6_H
#define TOBYOS_IPV6_H

#include <tobyos/types.h>

struct ipv6_addr {
    uint8_t bytes[16];
};

struct __attribute__((packed)) ipv6_hdr {
    uint32_t ver_tc_fl;       /* version(4) + traffic class(8) + flow label(20) */
    uint16_t payload_len;
    uint8_t  next_header;
    uint8_t  hop_limit;
    struct ipv6_addr src;
    struct ipv6_addr dst;
};

#define IPV6_HDR_LEN 40

/* Next header values */
#define IPV6_NH_TCP     6
#define IPV6_NH_UDP    17
#define IPV6_NH_ICMPV6 58

/* Well-known addresses */
extern const struct ipv6_addr ipv6_addr_any;
extern const struct ipv6_addr ipv6_addr_loopback;
extern const struct ipv6_addr ipv6_addr_all_nodes;    /* ff02::1 */
extern const struct ipv6_addr ipv6_addr_all_routers;  /* ff02::2 */

/* Utility */
bool ipv6_addr_is_zero(const struct ipv6_addr *a);
bool ipv6_addr_equal(const struct ipv6_addr *a, const struct ipv6_addr *b);
bool ipv6_addr_is_linklocal(const struct ipv6_addr *a);
bool ipv6_addr_is_multicast(const struct ipv6_addr *a);
void ipv6_make_linklocal(struct ipv6_addr *out, const uint8_t mac[6]);
void ipv6_make_solicited_node(struct ipv6_addr *out, const struct ipv6_addr *target);
void ipv6_format(char *buf, size_t cap, const struct ipv6_addr *a);

/* Core */
void ipv6_init(void);
void ipv6_recv(const void *frame, size_t len);
int  ipv6_send(const struct ipv6_addr *dst, uint8_t next_header,
               const void *payload, size_t payload_len);
const struct ipv6_addr *ipv6_our_linklocal(void);
bool ipv6_is_up(void);

/* ---- SLAAC (stateless address autoconfiguration) ----
 *
 * Filled in by the ICMPv6 Router Advertisement handler: a /64 prefix with
 * the autonomous flag yields a global address (prefix | our interface id),
 * and the RA source becomes the default router for off-link traffic. */

/* Install a SLAAC global address formed from `prefix` (top 64 bits) and our
 * interface id. `router` is the RA source (default router); its MAC is
 * cached separately by the ND layer. Idempotent for the same prefix. */
void ipv6_slaac_configure(const struct ipv6_addr *prefix,
                          const struct ipv6_addr *router);

/* Our autoconfigured global address, or NULL if SLAAC hasn't completed. */
const struct ipv6_addr *ipv6_our_global(void);
bool ipv6_have_global(void);

/* The default router (RA source), or NULL if none learned. */
const struct ipv6_addr *ipv6_default_router(void);

#endif /* TOBYOS_IPV6_H */
