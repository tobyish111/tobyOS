/* eth.c -- Ethernet II frame send + RX dispatcher.
 *
 * On send we build the 14-byte header into a small per-call stack
 * buffer (1500 bytes payload + 14 bytes header well below the 4 KiB
 * stack), copy the payload behind it, and hand the whole thing to
 * the registered network device (net_default()->tx). On receive we
 * get the frame as-it-came-off-the-wire, peel off the header, and
 * demux on ethertype.
 *
 * RX peels 802.1Q (0x8100) and 802.1ad service (0x88A8) tags before
 * demux. Real switches often deliver DHCP OFFER/ACK tagged while
 * DISCOVER leaves the host untagged; without this, EtherType is not
 * 0x0800 and every tagged reply is dropped after DISCOVER succeeds.
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
#include <tobyos/ipv6.h>
#include <tobyos/nsproxy.h>   /* net_ns_dev / net_ctx_* (slice 12 cuts 2+5) */
#include <tobyos/proc.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>    /* capped v6-arrival diagnostic (2026-08-23) */

bool eth_send(const uint8_t dst_mac[ETH_ADDR_LEN],
              uint16_t      ethertype,
              const void   *payload,
              size_t        payload_len) {
    if (payload_len > ETH_MTU) return false;

    /* Slice 12 cut 2: pick the device from the NETWORK NAMESPACE.
     *
     * Two sources, in this order:
     *   1. an active receive context -- we are replying to a frame that arrived
     *      on a veth end, so the reply must go back out THAT end (an ARP reply
     *      leaving by the host's e1000 would simply vanish);
     *   2. the calling process's namespace.
     * Neither applies for the initial namespace, which falls through to
     * net_default() exactly as before -- so the e1000/DHCP path is unchanged. */
    struct net_dev *nd = 0;
    if (net_ctx_active()) {
        /* Cut 5: inside an explicit context the reply leaves by the interface
         * the frame arrived on. That is what makes a namespace with two veth
         * ends answer each one on its own wire instead of sending every reply
         * out its primary. */
        nd = net_ctx_dev();
        if (!nd) nd = net_ns_dev(net_ctx_ns());
    } else {
        struct proc *cp = current_proc();
        void *ns = cp ? cp->net_ns : 0;
        if (ns) nd = net_ns_dev(ns);
    }
    if (!nd) nd = net_default();
    if (!nd || !nd->tx) return false;

    uint8_t frame[ETH_FRAME_MAX];
    struct eth_hdr *h = (struct eth_hdr *)frame;
    memcpy(h->dst, dst_mac,  ETH_ADDR_LEN);
    /* Source MAC must be THIS device's, not the default's: ARP resolution on the
     * peer end learns the sender's hardware address from here, so borrowing the
     * host NIC's MAC would make the two ends of a pair indistinguishable. */
    memcpy(h->src, (nd == net_default()) ? g_my_mac : nd->mac, ETH_ADDR_LEN);
    h->ethertype = htons(ethertype);
    if (payload_len) memcpy(frame + ETH_HDR_LEN, payload, payload_len);

    return nd->tx(nd, frame, ETH_HDR_LEN + payload_len);
}

static void eth_recv_inner(const void *frame, size_t len);

/* CUT 5: EVERY FRAME IS NOW PROCESSED UNDER AN EXPLICIT NETWORK CONTEXT.
 *
 * eth_recv() is what all five hardware drivers call (e1000, e1000e, igb,
 * rtl8168/8169, virtio-net), and a frame off the wire belongs to the INITIAL
 * namespace. Saying so explicitly is the fix, not a formality: before this,
 * "no context" and "the initial namespace" were the same NULL, so the host
 * receive path fell through to current_proc()->net_ns -- and e1000's drain runs
 * from its IRQ handler and from net_poll(), which poll/select/recvmsg call. A
 * process inside a container calling poll() therefore drained the host's NIC
 * while net_my_ip() answered with the container's address, and
 * ip_dst_is_for_us() dropped the host's own packets.
 *
 * Wrapping it HERE rather than in each driver is what keeps the fix to one
 * function: the five drivers are untouched and get the correct context for
 * free. veth calls eth_recv_dev() instead, because it knows both the namespace
 * and the interface. */
void eth_recv(const void *frame, size_t len) {
    eth_recv_dev(frame, len, 0, 0);      /* the initial namespace, said out loud */
}

void eth_recv_dev(const void *frame, size_t len, void *ns,
                  struct net_dev *dev) {
    /* ALWAYS brackets, even for (NULL, NULL). Making the bracket conditional
     * would reintroduce exactly the bug this fixes: a frame processed under
     * whatever context happened to be left active by the caller. */
    struct net_ctx prev = net_ctx_enter(ns, dev);
    eth_recv_inner(frame, len);
    net_ctx_leave(prev);
}

static void eth_recv_inner(const void *frame, size_t len) {
    if (len < ETH_HDR_LEN) return;

    const uint8_t *f = (const uint8_t *)frame;
    /* `et_off` indexes the 16-bit EtherType field (always 12 bytes after
     * the start of the current DMAC+SMAC header). VLAN tags insert 4 bytes
     * each *before* the final EtherType without moving the MAC addresses. */
    size_t et_off = 12;
    for (unsigned depth = 0; depth < 4u; depth++) {
        if (et_off + 2 > len) return;
        uint16_t et = ((uint16_t)f[et_off] << 8) | (uint16_t)f[et_off + 1];
        if (et == ETH_TYPE_VLAN || et == ETH_TYPE_VLAN_S) {
            if (et_off + 6 > len) return;
            et_off += 4;
            continue;
        }
        break;
    }
    if (et_off + 2 > len) return;
    uint16_t et = ((uint16_t)f[et_off] << 8) | (uint16_t)f[et_off + 1];
    const uint8_t *pl  = f + et_off + 2;
    size_t         pln = len - (et_off + 2);

    switch (et) {
    case ETH_TYPE_ARP:
        arp_recv(pl, pln);
        break;
    case ETH_TYPE_IPV4:
        ip_recv(pl, pln);
        break;
    case ETH_TYPE_IPV6:
        /* Capped arrival log (2026-08-23): the one guest-side fact that
         * separates "SLIRP never sent it" from "we dropped it" -- a
         * netdev filter-dump turned out to capture only the guest->host
         * direction, so the wire pcap cannot answer this. */
        {   static int v6_rx_logged;
            if (v6_rx_logged < 8) { v6_rx_logged++;
                kprintf("[eth] v6 frame in len=%u dst=%02x:%02x:%02x:"
                        "%02x:%02x:%02x\n", (unsigned)pln,
                        f[0], f[1], f[2], f[3], f[4], f[5]);
            } }
        ipv6_recv(pl, pln);
        break;
    default:
        break;
    }
}
