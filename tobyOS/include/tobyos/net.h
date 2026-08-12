/* net.h -- shared types + helpers for the milestone-9 network stack.
 *
 * After DHCP failure, net_init() applies a static fallback (see net.c):
 *   default: 192.168.68.10 / 255.255.252.0, gateway + DNS 192.168.68.1
 *   with -DTOBY_NET_FALLBACK_SLIRP: QEMU user-net 10.0.2.15 / 10.0.2.2 / 10.0.2.3
 *
 * guest MAC is read from the NIC at net_init().
 *
 * Byte order: the wire is big-endian. We keep IP addresses and ports
 * in network byte order in every kernel struct that mirrors the wire,
 * and only convert when printing or doing arithmetic. The htons/htonl
 * helpers below are the only allowed swap primitives.
 */

#ifndef TOBYOS_NET_H
#define TOBYOS_NET_H

#include <tobyos/types.h>

#define ETH_ADDR_LEN     6
#define ETH_MTU          1500
#define ETH_FRAME_MAX    1536    /* MTU + headers + slack, ring buffer size */

#define ETH_TYPE_IPV4    0x0800
#define ETH_TYPE_ARP     0x0806
#define ETH_TYPE_IPV6    0x86DD
#define ETH_TYPE_VLAN    0x8100    /* 802.1Q customer VLAN tag before EtherType */
#define ETH_TYPE_VLAN_S  0x88A8    /* 802.1ad service VLAN (outer); inner may be 8100 */

#define IP_PROTO_ICMP    1
#define IP_PROTO_TCP     6
#define IP_PROTO_UDP     17

/* Per-interface configuration. All IPs in network byte order.
 *
 * Initial values (Milestone 24A onward):
 *   - At net_init() entry, all four are zero.
 *   - DHCP populates them from the lease.
 *   - If DHCP fails, net_init() falls back to static 192.168.68.10/22
 *     (gw/dns .68.1), or SLIRP defaults when built with TOBY_NET_FALLBACK_SLIRP. */
extern uint8_t  g_my_mac[ETH_ADDR_LEN];   /* learned from the NIC at init */
extern uint32_t g_my_ip;
extern uint32_t g_my_netmask;
extern uint32_t g_gateway_ip;
extern uint32_t g_my_dns_be;              /* DNS server (24A populates, 24B uses) */

/* Byte-order helpers. The kernel runs little-endian on x86, the wire is
 * big-endian, hence the unconditional swap. */
static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }

static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) <<  8) |
           ((x & 0x00FF0000u) >>  8) |
           ((x & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Pack four octets a.b.c.d into a 32-bit network-byte-order address. */
static inline uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a) |
           ((uint32_t)b <<  8) |
           ((uint32_t)c << 16) |
           ((uint32_t)d << 24);
}

/* RFC-1071 one's-complement Internet checksum. Used by IP/UDP/ICMP.
 * Accepts an odd byte count by zero-padding the last word. */
uint16_t net_checksum(const void *data, size_t len);

/* Pseudo-header checksum for any L4 protocol that uses the same
 * IPv4 pseudo header (UDP and TCP). `proto` is the IP protocol
 * number (IP_PROTO_UDP, IP_PROTO_TCP, ...), `l4_packet` points at the
 * start of the L4 header (followed by data), `l4_len` is the total
 * L4 length (header + data, in bytes).
 *
 * The L4 packet's own `checksum` field MUST be zeroed when computing
 * a fresh checksum to insert. Verification feeds the wire bytes
 * unchanged and expects a return value of 0. (For UDP, RFC 768
 * lets a SEND-side computed result of 0 be rewritten to 0xFFFF; the
 * caller does that rewrite, not this helper.) */
uint16_t net_l4_checksum(uint8_t proto, uint32_t src_ip_be, uint32_t dst_ip_be,
                         const void *l4_packet, size_t l4_len);

/* Backwards-compat thin wrapper. Equivalent to
 * net_l4_checksum(IP_PROTO_UDP, ...). */
uint16_t net_udp_checksum(uint32_t src_ip_be, uint32_t dst_ip_be,
                          const void *udp_packet, size_t udp_len);

/* Pretty-print: writes "a.b.c.d" (max 16 bytes incl. NUL) to dst. */
void net_format_ip (char dst[16], uint32_t ip_be);

/* ---- Slice 12 cut 2: the namespace-relative address ---------------------
 * g_my_ip stays the INITIAL namespace's address (DHCP writes it, ifconfig reads
 * it). These return the CALLER's / RECEIVER's namespace address instead, falling
 * back to g_my_ip -- so the wire path is unchanged and a namespace with a veth
 * end answers for its own address only. */
uint32_t net_my_ip(void);
uint32_t net_my_netmask(void);
const uint8_t *net_my_mac(void);   /* the transmitting interface's MAC */

/* veth (src/veth.c). These two are the KERNEL-ONLY constructors: a NULL ns means
 * the initial namespace, and that end is registered with net.c as an ordinary
 * interface rather than placed in a namespace list. For the userspace-drivable
 * path (`ip link add ... type veth` -> RTM_NEWLINK -> netns_veth_create) see
 * the cut-4 block below. */
long netns_veth_pair_named(void *ns_a, const char *name_a, uint32_t ip_a,
                           void *ns_b, const char *name_b, uint32_t ip_b,
                           uint32_t mask);
long netns_veth_pair(void *ns_a, uint32_t ip_a, void *ns_b, uint32_t ip_b,
                     uint32_t mask);
void netns_veth_stats(void *ns, uint32_t *tx, uint32_t *rx);

/* Forward declaration: struct net_dev is defined further down, and a `struct
 * net_dev *` first mentioned inside a prototype would be a DIFFERENT,
 * function-scoped type -- clang says so (-Wvisibility) and then rejects every
 * call. */
struct net_dev;
/* Cut 4: the entry points rtnetlink drives. Unlike the two above, these put
 * BOTH ends in a namespace list (which is what RTM_GETLINK enumerates) and
 * never call net_register. `peer_name` may be NULL/empty for Linux's automatic
 * vethN naming -- busybox's `ip` cannot express a peer name at all. */
long netns_veth_create(void *ns, const char *name, const char *peer_name,
                       struct net_dev **out_a, struct net_dev **out_b);
bool netns_veth_is_veth(struct net_dev *dev);
bool netns_veth_delete(struct net_dev *dev);       /* removes BOTH ends */
void netns_veth_release(struct net_dev *dev);      /* namespace teardown: this
                                                    * end only, peer unpaired */
bool netns_veth_move_ns(struct net_dev *dev, void *ns);
void netns_veth_dev_stats(struct net_dev *dev, uint32_t *tx, uint32_t *rx);
/* Pretty-print: writes "aa:bb:cc:dd:ee:ff" (max 18 bytes incl. NUL). */
void net_format_mac(char dst[18], const uint8_t mac[ETH_ADDR_LEN]);

/* Constant byte patterns. */
extern const uint8_t g_eth_broadcast[ETH_ADDR_LEN];
extern const uint8_t g_eth_zero[ETH_ADDR_LEN];

/* ---- network-device registry (milestone 21) -----------------------
 *
 * Each NIC driver (e1000, e1000e, RTL8169, virtio-net, ...) calls
 * net_register() from inside its PCI probe. The eth/arp/ip/udp stack
 * sends through net_default()->tx and drains via
 * net_default()->rx_drain -- it has no idea which silicon is below.
 *
 * Registered devices have static lifetime; the registry stores raw
 * pointers and never allocates. */

#define NET_MAX_DEVICES 4

struct net_dev {
    const char *name;                       /* "e1000:00:03.0" */
    uint8_t     mac[ETH_ADDR_LEN];
    void       *priv;                       /* driver-private */
    bool      (*tx)      (struct net_dev *dev, const void *frame, size_t len);
    void      (*rx_drain)(struct net_dev *dev);
    bool      (*link_up) (struct net_dev *dev); /* optional physical-link check */
};

void           net_register(struct net_dev *dev);
struct net_dev *net_default(void);          /* first registered or NULL */
size_t         net_dev_count(void);
struct net_dev *net_dev_get(size_t idx);
void           net_dump(void);

enum net_status {
    NET_STATUS_DOWN = 0,
    NET_STATUS_NO_NIC,
    NET_STATUS_DHCP_WAIT,
    NET_STATUS_DHCP_OK,
    NET_STATUS_DHCP_EMPTY,
    NET_STATUS_STATIC_FALLBACK,
};

enum net_status net_status(void);
const char *net_status_name(void);
void net_status_summary(char *dst, size_t cap);
void net_debug_dump(void);

/* Driver registration entry points called from kernel.c during boot.
 * Each one inserts a struct pci_driver into the bus registry. */
void e1000_register(void);
void e1000e_register(void);       /* Intel 82574L + 82577/82579/I217 family */
void virtio_net_register(void);   /* virtio-net-pci 1.0+ (modern transport) */
void rtl8169_register(void);      /* Realtek RTL8169/8168/8111 gigabit family */

/* Top-level network init. Picks net_default(), copies its MAC into
 * g_my_mac, brings up ARP + sockets, runs a DHCP handshake (with a
 * static-IP fallback if it times out), and preloads the gateway ARP
 * entry. Safe to call once after heap_init() AND after
 * pci_bind_drivers(). Logs progress + on-failure reason; returns
 * true on success (i.e. the wire is usable, regardless of whether
 * the IP came from DHCP or the fallback). */
bool net_init(void);

/* Request one boot-time network bring-up. This records intent only:
 * net_service_tick() performs the actual net_init() from the cooperative
 * network lane so the desktop can continue booting even if DHCP is slow
 * or the NIC is absent. */
void net_boot_request(void);

/* Re-run DHCP at runtime. Same effect as net_init's DHCP branch:
 * acquires a fresh lease and updates the globals. Returns true on
 * success. Used by the `dhcp` shell builtin. */
bool net_dhcp_renew(void);

/* Drain any RX descriptors and dispatch frames into the stack. Called
 * from the kernel idle loop alongside shell_poll(). Cheap when nothing
 * is pending. Must NOT be called from IRQ context. */
void net_poll(void);

/* Guarded network maintenance lane. This wraps net_poll() plus protocol
 * services that sit beside the packet stack (currently SSH). It is safe
 * to call from cooperative kernel/service contexts before GUI work; a
 * non-reentrant guard drops nested calls instead of letting GUI/input
 * paths overlap network maintenance. Must NOT be called from IRQ context. */
void net_service_tick(void);

/* Did net_init() succeed? Kept so syscall stubs can reject cleanly
 * with -ENONET on systems where the e1000 wasn't present. */
bool net_is_up(void);

/* True if net_init()'s DHCP handshake succeeded (IPv4 from lease, not the
 * static fallback). Unchanged by later net_dhcp_renew(); for bootlog only. */
bool net_boot_used_dhcp(void);

#endif /* TOBYOS_NET_H */
