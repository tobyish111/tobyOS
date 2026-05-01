/* dhcp.h -- DHCPv4 client (RFC 2131 / 2132).
 *
 * Tiny one-shot client: at boot we run DISCOVER -> OFFER -> REQUEST -> ACK
 * once and commit the resulting lease into the existing net globals
 * (g_my_ip / g_my_netmask / g_gateway_ip / g_my_dns_be).
 *
 * Limits / non-goals (Milestone 24A):
 *   - one network interface (whatever net_default() returns)
 *   - DISCOVER is retransmitted during the offer-phase window (see dhcp.c)
 *   - no lease renewal (we just log T1/T2 and lease seconds)
 *   - no INFORM / RELEASE / DECLINE
 *   - no per-vendor identifiers
 *   - if anything goes wrong (no NIC, no server, malformed reply) the
 *     caller can fall back to a static IP and the rest of the kernel
 *     happily boots.
 *
 * Layering:
 *   dhcp.c only ever calls udp_send and reads from globals owned by
 *   net.h. Frames arrive through one tiny hook in udp_recv that fires
 *   when dst_port == 68. Nothing in the socket layer is touched.
 */

#ifndef TOBYOS_DHCP_H
#define TOBYOS_DHCP_H

#include <tobyos/types.h>

/* Well-known UDP ports for BOOTP/DHCP. Both kept in network byte
 * order at the call site (we use htons() inline). */
#define DHCP_PORT_SERVER    67
#define DHCP_PORT_CLIENT    68

/* On-the-wire BOOTP / DHCP packet (RFC 951 + RFC 2131).
 *
 *   layout: 240 fixed bytes (op..magic) + 312 bytes of options.
 *   Total: 552 bytes -- well below the 1500-byte Ethernet MTU.
 */
struct __attribute__((packed)) dhcp_pkt {
    uint8_t  op;                /* 1=BOOTREQUEST, 2=BOOTREPLY     */
    uint8_t  htype;             /* 1 = Ethernet                   */
    uint8_t  hlen;              /* 6 = MAC length                 */
    uint8_t  hops;              /* 0 from a client                */
    uint32_t xid;               /* random; mirrored by the server */
    uint16_t secs;              /* seconds since started; we use 0 */
    uint16_t flags;             /* bit15 = "broadcast reply, please" */
    uint32_t ciaddr;            /* client IP, if it already has one */
    uint32_t yiaddr;            /* "your" IP -- server fills in   */
    uint32_t siaddr;            /* next-server IP (unused here)   */
    uint32_t giaddr;            /* relay agent IP (always 0)      */
    uint8_t  chaddr[16];        /* client hw address (MAC + zeros) */
    uint8_t  sname[64];         /* server hostname (unused)       */
    uint8_t  file[128];         /* boot file name  (unused)       */
    uint32_t magic;             /* 0x63 0x82 0x53 0x63 (BE on wire) */
    uint8_t  options[312];      /* DHCP options + END marker      */
};

#define DHCP_PKT_LEN     sizeof(struct dhcp_pkt)
#define DHCP_OPTIONS_LEN 312u

#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2
#define DHCP_HTYPE_ETH   1
#define DHCP_HLEN_ETH    6
#define DHCP_MAGIC_BE    0x63538263u    /* 0x63825363 in BE byte-order  */

/* DHCP message types (option 53, 1-byte payload). */
#define DHCP_MSG_DISCOVER  1
#define DHCP_MSG_OFFER     2
#define DHCP_MSG_REQUEST   3
#define DHCP_MSG_DECLINE   4
#define DHCP_MSG_ACK       5
#define DHCP_MSG_NAK       6
#define DHCP_MSG_RELEASE   7
#define DHCP_MSG_INFORM    8

/* DHCP option codes (RFC 2132). */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET         1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME      12
#define DHCP_OPT_REQUESTED_IP  50
#define DHCP_OPT_LEASE_TIME    51
#define DHCP_OPT_OVERLOAD      52 /* RFC 2132 §9.4: opts spill to file/sname */
#define DHCP_OPT_MSG_TYPE      53
#define DHCP_OPT_SERVER_ID     54
#define DHCP_OPT_PARAM_REQ     55
#define DHCP_OPT_CLIENT_ID     61
#define DHCP_OPT_END           255

/* Result of a successful DHCP handshake.
 *   All IPs are in network byte order.
 *   lease_secs is in HOST order (RFC 2132 puts a uint32_t big-endian on
 *   the wire; we ntohl into here for printing convenience). */
struct dhcp_lease {
    uint32_t ip_be;
    uint32_t netmask_be;
    uint32_t gateway_be;
    uint32_t dns_be;
    uint32_t server_be;
    uint32_t lease_secs;
};

/* Run a DISCOVER/OFFER/REQUEST/ACK cycle on the default NIC. Blocks
 * for up to `timeout_ms` total (split across the two RX waits).
 *
 * Pre-conditions:
 *   - net_default() != NULL  (a NIC is registered)
 *   - g_my_mac is populated  (caller copies it out of the NIC)
 *   - g_my_ip == 0           (so outbound packets carry src 0.0.0.0)
 *   - arp_init() and sock_init() already called
 *
 * On success returns true and fills *out. The caller is responsible
 * for committing the lease into g_my_ip / g_my_netmask / g_gateway_ip
 * / g_my_dns_be (kept separate so the caller can also fall back). */
bool dhcp_acquire(uint32_t timeout_ms, struct dhcp_lease *out);

/* RX hook: called by udp_recv() the instant a UDP datagram arrives
 * with dst_port == 68 (the BOOTP client port). Always safe to call:
 * if no DHCP exchange is in flight, this is a no-op. */
void dhcp_recv_hook(uint32_t src_ip_be, const void *udp_packet, size_t len);

#endif /* TOBYOS_DHCP_H */
