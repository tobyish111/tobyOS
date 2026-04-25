/* eth.c -- Ethernet II frame send + RX dispatcher.
 *
 * On send we build the 14-byte header into a small per-call stack
 * buffer (1500 bytes payload + 14 bytes header well below the 4 KiB
 * stack), copy the payload behind it, and hand the whole thing to
 * the registered network device (net_default()->tx). On receive we
 * get the frame as-it-came-off-the-wire, peel off the header, and
 * demux on ethertype.
 *
 * Frames smaller than ETH_HDR_LEN are silently dropped (corrupt /
 * not for us). Anything we don't understand goes the same way.
 *
 * Milestone-21 note: this file has no idea which NIC is below it.
 * net_default() is set up by whichever PCI driver bound first
 * (e1000, e1000e, RTL8169, virtio-net, ...).
 */

#include <tobyos/eth.h>
#include <tobyos/net.h>
#include <tobyos/arp.h>
#include <tobyos/ip.h>
#include <tobyos/klibc.h>

bool eth_send(const uint8_t dst_mac[ETH_ADDR_LEN],
              uint16_t      ethertype,
              const void   *payload,
              size_t        payload_len) {
    if (payload_len > ETH_MTU) return false;

    struct net_dev *nd = net_default();
    if (!nd || !nd->tx) return false;

    uint8_t frame[ETH_FRAME_MAX];
    struct eth_hdr *h = (struct eth_hdr *)frame;
    memcpy(h->dst, dst_mac,  ETH_ADDR_LEN);
    memcpy(h->src, g_my_mac, ETH_ADDR_LEN);
    h->ethertype = htons(ethertype);
    if (payload_len) memcpy(frame + ETH_HDR_LEN, payload, payload_len);

    return nd->tx(nd, frame, ETH_HDR_LEN + payload_len);
}

void eth_recv(const void *frame, size_t len) {
    if (len < ETH_HDR_LEN) return;

    const struct eth_hdr *h   = (const struct eth_hdr *)frame;
    const uint8_t        *pl  = (const uint8_t *)frame + ETH_HDR_LEN;
    size_t                pln = len - ETH_HDR_LEN;
    uint16_t              et  = ntohs(h->ethertype);

    switch (et) {
    case ETH_TYPE_ARP:
        arp_recv(pl, pln);
        break;
    case ETH_TYPE_IPV4:
        ip_recv(pl, pln);
        break;
    default:
        /* Drop quietly -- IPv6 multicast, LLDP, STP, etc. */
        break;
    }
}
