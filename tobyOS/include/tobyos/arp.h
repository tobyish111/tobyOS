/* arp.h -- IPv4-over-Ethernet Address Resolution Protocol (RFC 826).
 *
 * Tiny 16-entry cache. Lookup: O(N). Eviction: oldest (FIFO). Replies
 * to ARP requests for our own IP, and learns from any ARP reply we
 * see. arp_resolve() returns true only if the entry is already in the
 * cache; the caller is expected to fire arp_request() and try again
 * later (typically once at boot for the gateway).
 */

#ifndef TOBYOS_ARP_H
#define TOBYOS_ARP_H

#include <tobyos/types.h>
#include <tobyos/net.h>

#define ARP_HW_ETHERNET   1
#define ARP_OP_REQUEST    1
#define ARP_OP_REPLY      2

/* On-the-wire ARP packet (28 bytes for IPv4-over-Ethernet). */
struct __attribute__((packed)) arp_pkt {
    uint16_t hw_type;          /* 1 = Ethernet                         */
    uint16_t proto_type;       /* 0x0800 = IPv4                        */
    uint8_t  hw_len;           /* 6                                    */
    uint8_t  proto_len;        /* 4                                    */
    uint16_t opcode;           /* 1=request, 2=reply                   */
    uint8_t  sender_mac[ETH_ADDR_LEN];
    uint32_t sender_ip;        /* network byte order                   */
    uint8_t  target_mac[ETH_ADDR_LEN];
    uint32_t target_ip;        /* network byte order                   */
};

/* Initialise the ARP cache (zero out every entry). Called once at
 * net_init time. */
void arp_init(void);

/* Try to resolve `ip_be` into a MAC. Returns true and fills out_mac
 * on a cache hit; returns false on a miss (caller should send a
 * request and try again later). */
bool arp_resolve(uint32_t ip_be, uint8_t out_mac[ETH_ADDR_LEN]);

/* Add or update an ARP cache entry. Used by both the RX path (when
 * we observe a sender's MAC) and the boot-time pre-population path. */
void arp_cache_set(uint32_t ip_be, const uint8_t mac[ETH_ADDR_LEN]);

/* Send an ARP request asking "who has ip_be? tell me". Broadcast. */
void arp_request(uint32_t ip_be);

/* Called by eth_recv when an ARP frame lands. Parses, updates the
 * cache, and replies if the request targets our own IP. */
void arp_recv(const void *payload, size_t len);

/* Diagnostic dump for the `arp` shell builtin. */
void arp_dump(void);

#endif /* TOBYOS_ARP_H */
