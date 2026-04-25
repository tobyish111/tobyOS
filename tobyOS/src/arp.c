/* arp.c -- IPv4-over-Ethernet ARP cache + protocol handler.
 *
 * Cache: 16 entries, oldest-wins eviction (FIFO via g_next_evict).
 * No timeouts -- entries live forever or until evicted by a fresher
 * sender. For an educational stack on a virtual NIC this is fine.
 *
 * On RX we always learn from the sender's (mac, ip) pair, even when
 * the ARP frame isn't directed at us. This matches what most stacks
 * do and dramatically reduces the number of round-trips needed.
 */

#include <tobyos/arp.h>
#include <tobyos/eth.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define ARP_CACHE_MAX 16

struct arp_entry {
    bool     valid;
    uint32_t ip;                          /* network byte order */
    uint8_t  mac[ETH_ADDR_LEN];
};

static struct arp_entry g_cache[ARP_CACHE_MAX];
static unsigned         g_next_evict;

void arp_init(void) {
    memset(g_cache, 0, sizeof(g_cache));
    g_next_evict = 0;
}

bool arp_resolve(uint32_t ip_be, uint8_t out_mac[ETH_ADDR_LEN]) {
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip_be) {
            memcpy(out_mac, g_cache[i].mac, ETH_ADDR_LEN);
            return true;
        }
    }
    return false;
}

void arp_cache_set(uint32_t ip_be, const uint8_t mac[ETH_ADDR_LEN]) {
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip_be) {
            memcpy(g_cache[i].mac, mac, ETH_ADDR_LEN);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (!g_cache[i].valid) {
            g_cache[i].valid = true;
            g_cache[i].ip    = ip_be;
            memcpy(g_cache[i].mac, mac, ETH_ADDR_LEN);
            return;
        }
    }
    /* Pool full -- evict the oldest. */
    unsigned slot = g_next_evict % ARP_CACHE_MAX;
    g_cache[slot].valid = true;
    g_cache[slot].ip    = ip_be;
    memcpy(g_cache[slot].mac, mac, ETH_ADDR_LEN);
    g_next_evict++;
}

void arp_request(uint32_t ip_be) {
    struct arp_pkt p;
    p.hw_type    = htons(ARP_HW_ETHERNET);
    p.proto_type = htons(ETH_TYPE_IPV4);
    p.hw_len     = ETH_ADDR_LEN;
    p.proto_len  = 4;
    p.opcode     = htons(ARP_OP_REQUEST);
    memcpy(p.sender_mac, g_my_mac, ETH_ADDR_LEN);
    p.sender_ip  = g_my_ip;
    memset(p.target_mac, 0, ETH_ADDR_LEN);
    p.target_ip  = ip_be;
    eth_send(g_eth_broadcast, ETH_TYPE_ARP, &p, sizeof(p));
}

static void arp_send_reply(const uint8_t dst_mac[ETH_ADDR_LEN],
                           uint32_t      dst_ip_be) {
    struct arp_pkt p;
    p.hw_type    = htons(ARP_HW_ETHERNET);
    p.proto_type = htons(ETH_TYPE_IPV4);
    p.hw_len     = ETH_ADDR_LEN;
    p.proto_len  = 4;
    p.opcode     = htons(ARP_OP_REPLY);
    memcpy(p.sender_mac, g_my_mac, ETH_ADDR_LEN);
    p.sender_ip  = g_my_ip;
    memcpy(p.target_mac, dst_mac, ETH_ADDR_LEN);
    p.target_ip  = dst_ip_be;
    eth_send(dst_mac, ETH_TYPE_ARP, &p, sizeof(p));
}

void arp_recv(const void *payload, size_t len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *p = (const struct arp_pkt *)payload;

    if (ntohs(p->hw_type)    != ARP_HW_ETHERNET) return;
    if (ntohs(p->proto_type) != ETH_TYPE_IPV4)   return;
    if (p->hw_len    != ETH_ADDR_LEN) return;
    if (p->proto_len != 4)            return;

    /* Always learn from the sender. */
    arp_cache_set(p->sender_ip, p->sender_mac);

    uint16_t op = ntohs(p->opcode);
    if (op == ARP_OP_REQUEST && p->target_ip == g_my_ip) {
        arp_send_reply(p->sender_mac, p->sender_ip);
    }
    /* For ARP_OP_REPLY the cache_set above already did the work. */
}

void arp_dump(void) {
    kprintf("ARP cache:\n");
    int n = 0;
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (!g_cache[i].valid) continue;
        char ipbuf[16], macbuf[18];
        net_format_ip(ipbuf, g_cache[i].ip);
        net_format_mac(macbuf, g_cache[i].mac);
        kprintf("  %-15s  %s\n", ipbuf, macbuf);
        n++;
    }
    if (n == 0) kprintf("  (empty)\n");
}
