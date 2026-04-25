/* net.h -- shared types + helpers for the milestone-9 network stack.
 *
 * Static configuration:
 *   guest IP   = 10.0.2.15
 *   netmask    = 255.255.255.0
 *   gateway    = 10.0.2.2     (QEMU SLIRP host)
 *   guest MAC  = read from the e1000 (QEMU defaults to 52:54:00:12:34:56)
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

#define IP_PROTO_ICMP    1
#define IP_PROTO_TCP     6
#define IP_PROTO_UDP     17

/* Per-interface configuration. All IPs in network byte order.
 *
 * Initial values (Milestone 24A onward):
 *   - At net_init() entry, all four are zero.
 *   - DHCP populates them from the lease.
 *   - If DHCP fails, net_init() falls back to QEMU SLIRP defaults
 *     (10.0.2.15 / 255.255.255.0 / 10.0.2.2 / 10.0.2.3) so the kernel
 *     still boots offline. */
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
};

void           net_register(struct net_dev *dev);
struct net_dev *net_default(void);          /* first registered or NULL */
size_t         net_dev_count(void);
struct net_dev *net_dev_get(size_t idx);
void           net_dump(void);

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

/* Re-run DHCP at runtime. Same effect as net_init's DHCP branch:
 * acquires a fresh lease and updates the globals. Returns true on
 * success. Used by the `dhcp` shell builtin. */
bool net_dhcp_renew(void);

/* Drain any RX descriptors and dispatch frames into the stack. Called
 * from the kernel idle loop alongside shell_poll(). Cheap when nothing
 * is pending. Must NOT be called from IRQ context. */
void net_poll(void);

/* Did net_init() succeed? Kept so syscall stubs can reject cleanly
 * with -ENONET on systems where the e1000 wasn't present. */
bool net_is_up(void);

#endif /* TOBYOS_NET_H */
