/* udp.c -- UDP datagram send / receive (RFC 768).
 *
 * Send: build header in a stack buffer, copy payload, fill the
 * pseudo-header checksum, hand to ip_send. We always set a valid
 * checksum -- some hosts (and most security middleboxes) drop
 * datagrams that arrive with checksum=0.
 *
 * Receive: validate header length, look up a listening socket by
 * destination port, hand the payload to socket.c which queues it
 * and wakes any blocked recvfrom.
 */

#include <tobyos/udp.h>
#include <tobyos/ip.h>
#include <tobyos/heap.h>
#include <tobyos/socket.h>
#include <tobyos/dhcp.h>
#include <tobyos/dns.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

bool udp_send(uint16_t src_port_be, uint32_t dst_ip_be, uint16_t dst_port_be,
              const void *payload, size_t payload_len) {
    if (payload_len > 65535u - UDP_HDR_LEN - IP_HDR_LEN) return false;

    size_t   udp_len = UDP_HDR_LEN + payload_len;
    uint8_t  sbuf[ETH_MTU];
    uint8_t *buf     = sbuf;
    bool      heap   = false;

    if (udp_len > ETH_MTU) {
        buf = kmalloc(udp_len);
        if (!buf) return false;
        heap = true;
    }

    struct udp_hdr *h = (struct udp_hdr *)buf;
    h->src_port = src_port_be;
    h->dst_port = dst_port_be;
    h->length   = htons((uint16_t)udp_len);
    h->checksum = 0;

    if (payload_len) memcpy(buf + UDP_HDR_LEN, payload, payload_len);

    h->checksum = net_udp_checksum(g_my_ip, dst_ip_be, buf, udp_len);
    if (h->checksum == 0) h->checksum = htons(0xFFFF);

    bool ok = ip_send(dst_ip_be, IP_PROTO_UDP, buf, udp_len);
    if (heap) kfree(buf);
    return ok;
}

void udp_recv(uint32_t src_ip_be, const void *udp_packet, size_t len) {
    if (len < UDP_HDR_LEN) return;
    const struct udp_hdr *h = (const struct udp_hdr *)udp_packet;

    uint16_t udp_len = ntohs(h->length);
    if (udp_len < UDP_HDR_LEN || udp_len > len) return;

    /* Optional checksum verification: if non-zero, must validate.
     * RFC 768 lets the sender opt out (cs=0) so a zero is not an
     * error.
     *
     * Skip the checksum check when we don't yet have an IP (g_my_ip
     * == 0): the pseudo-header would use 0.0.0.0 as the destination
     * and never validate against the wire bytes. The DHCP path is the
     * canonical example -- replies arrive while we're still acquiring
     * a lease. */
    if (h->checksum != 0 && g_my_ip != 0) {
        if (net_udp_checksum(src_ip_be, g_my_ip, udp_packet, udp_len) != 0)
            return;
    }

    /* Kernel-side hooks: a few well-known kernel ports are owned by
     * subsystems that pre-date the userspace socket layer (or run
     * from boot context with no current_proc). They're always safe
     * to call -- if no transaction is in flight the hook drops
     * silently and we fall through to nothing.
     *
     *   port 68    = BOOTP client       -> dhcp.c (24A)
     *   port 53999 = kernel resolver    -> dns.c  (24B)
     */
    if (h->dst_port == htons(68)) {
        dhcp_recv_hook(src_ip_be, udp_packet, udp_len);
        return;
    }
    if (h->dst_port == htons(53999)) {
        dns_recv_hook(src_ip_be, udp_packet, udp_len);
        return;
    }

    struct sock *s = sock_lookup_by_port(h->dst_port);
    if (!s) return;                       /* no listener: silent drop */

    const uint8_t *payload = (const uint8_t *)udp_packet + UDP_HDR_LEN;
    size_t         pln     = udp_len - UDP_HDR_LEN;

    /* Hand off to socket.c via a tiny inline structure -- avoids
     * pulling sock internals into udp.c. socket.c walks the dgram
     * ring itself. */
    extern void sock_deliver(struct sock *s,
                             uint32_t src_ip_be, uint16_t src_port_be,
                             const void *payload, size_t len);
    sock_deliver(s, src_ip_be, h->src_port, payload, pln);
}
