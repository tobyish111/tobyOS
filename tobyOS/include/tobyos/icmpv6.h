/* icmpv6.h -- ICMPv6: echo reply + Neighbor Discovery (RFC 4861). */

#ifndef TOBYOS_ICMPV6_H
#define TOBYOS_ICMPV6_H

#include <tobyos/types.h>
#include <tobyos/ipv6.h>

/* ICMPv6 types */
#define ICMPV6_ECHO_REQUEST     128
#define ICMPV6_ECHO_REPLY       129
#define ICMPV6_ROUTER_SOLICIT   133
#define ICMPV6_ROUTER_ADVERT    134
#define ICMPV6_NEIGHBOR_SOLICIT 135
#define ICMPV6_NEIGHBOR_ADVERT  136

struct __attribute__((packed)) icmpv6_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
};

/* Neighbor Discovery option types */
#define ND_OPT_SRC_LINKADDR 1
#define ND_OPT_TGT_LINKADDR 2
#define ND_OPT_PREFIX_INFO  3   /* Prefix Information (in Router Advert) */

/* Prefix Information option flag bits (byte after prefix_len) */
#define ND_PREFIX_FLAG_ONLINK 0x80   /* L: prefix is on-link            */
#define ND_PREFIX_FLAG_AUTO   0x40   /* A: usable for SLAAC autoconfig  */

void icmpv6_init(void);
void icmpv6_recv(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                 const void *payload, size_t len);

/* Send a Router Solicitation (to ff02::2) to kick off SLAAC. Safe to call
 * once link-local is configured; the router's RA reply drives autoconf. */
void icmpv6_send_router_solicit(void);

/* Opt-in self-test: synthesize an RA and assert SLAAC configures a global
 * address (QEMU SLIRP doesn't emit RAs, so the wire path can't be tested). */
void icmpv6_slaac_selftest(void);

/* Resolve an IPv6 address to a MAC via the neighbor cache.
 * Returns true if found, false if a solicitation was sent. */
bool icmpv6_resolve(const struct ipv6_addr *ip, uint8_t mac_out[6]);

#endif /* TOBYOS_ICMPV6_H */
