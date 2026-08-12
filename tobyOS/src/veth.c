/* veth.c -- virtual ethernet pairs (Phase 3 slice 12, cut 2).
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS SMALL: struct net_dev WAS ALREADY THE RIGHT ABSTRACTION
 *
 * A veth is a `net_dev` whose `tx` hands the frame to its peer's RECEIVE path
 * instead of to hardware. net.h's device vtable (tx / rx_drain / link_up / priv)
 * needed no change at all -- which is why cut 2's device half is a file this size
 * rather than a rewrite of net.c.
 *
 * The part that genuinely needed work is not the device, it is the CONTEXT: an
 * incoming frame has to be processed as the namespace that owns the receiving
 * end, because ip_dst_is_for_us() and arp compare against "my IP" and that
 * address is now per-namespace. See net_ns_rx_enter() and net_my_ip().
 * ---------------------------------------------------------------------------
 *
 * WHAT IS DELIBERATELY NOT HERE
 *
 *   - No control plane. Pairs are created by kernel call (netns_veth_pair), not
 *     by `ip link add ... type veth`, because netlink RTM_NEWLINK handling does
 *     not exist -- netlink here only ever BUILDS reply messages for interface
 *     dumps, it does not accept commands. Adding it is a slice of its own and
 *     pretending otherwise would mean a half-netlink nobody could drive.
 *   - No bridge. A bridge is what lets MANY namespaces share one uplink; a pair
 *     is what proves frames cross a namespace boundary at all. The pair is the
 *     foundation, so it comes first.
 *   - No NAT/forwarding to the outside world. A container can reach its peer end
 *     (the host side of the pair); it cannot reach the internet, because that
 *     needs forwarding + address translation on top of a bridge.
 *
 * So cut 2 as implemented here means: REAL FRAMES CROSS A NAMESPACE BOUNDARY,
 * with per-namespace addressing and a working ARP+IP round trip. It does not yet
 * mean "a container can reach the network".
 */

#include <tobyos/net.h>
#include <tobyos/nsproxy.h>
#include <tobyos/eth.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define VETH_MAX_PAIRS 8

struct veth_end {
    struct net_dev   dev;
    struct veth_end *peer;
    void            *ns;         /* the namespace this end lives in */
    char             name[24];
    bool             in_use;
    uint32_t         tx_frames, rx_frames;
};

static struct veth_end g_veth[VETH_MAX_PAIRS * 2];

/* Frames arriving on a veth end. Each driver in this kernel demuxes ethertype in
 * its own receive path (there is no single eth_input funnel -- e1000 does the
 * same thing inline), so this does too. */
static void veth_deliver(struct veth_end *dst, const uint8_t *frame, size_t len) {
    if (!dst || !dst->in_use || len < ETH_HDR_LEN) return;
    dst->rx_frames++;

    /* eth_recv() is the stack's own ethertype demux -- reuse it rather than
     * re-implementing the dispatch, so a veth frame takes byte-for-byte the same
     * path through arp/ip/ipv6 that a wire frame does. Anything that works on
     * e1000 therefore works here by construction.
     *
     * Bracketed so it is processed AS the receiving namespace: everything below
     * compares against "my IP", which is per-namespace now. Without this the
     * container's stack would answer for the host's address. */
    void *prev = net_ns_rx_enter(dst->ns);
    eth_recv(frame, len);
    net_ns_rx_leave(prev);
}

static bool veth_tx(struct net_dev *dev, const void *frame, size_t len) {
    struct veth_end *e = (struct veth_end *)dev->priv;
    if (!e || !e->peer) return false;
    e->tx_frames++;
    /* Delivered INLINE on the sender's stack, which is what makes the depth-1
     * rx-namespace save/restore in net_ns.c sufficient. */
    veth_deliver(e->peer, (const uint8_t *)frame, len);
    return true;
}

static void veth_rx_drain(struct net_dev *dev) { (void)dev; /* push, not poll */ }
static bool veth_link_up(struct net_dev *dev) {
    struct veth_end *e = (struct veth_end *)dev->priv;
    /* Linux reports a veth end DOWN while its peer is gone -- the honest answer,
     * and it is what stops a half-torn-down pair looking usable. */
    return e && e->peer && e->peer->in_use;
}

static struct veth_end *veth_alloc(void) {
    for (size_t i = 0; i < sizeof g_veth / sizeof g_veth[0]; i++)
        if (!g_veth[i].in_use) { memset(&g_veth[i], 0, sizeof g_veth[i]);
                                 g_veth[i].in_use = true; return &g_veth[i]; }
    return 0;
}

/* Create a veth pair with one end in each namespace, and address both.
 *
 * `ns_a` may be NULL, meaning the INITIAL namespace -- in which case that end is
 * registered with net.c the ordinary way (net_register) so the host's existing
 * stack can use it, while the container end lives only in its own namespace.
 * That asymmetry is the whole point of the additive design: the host side of the
 * pair is a normal interface; the container side is invisible outside its ns. */
/* Named variant: choose both interface names and APPEND each end to its
 * namespace's device list rather than installing it as the one interface.
 *
 * This is what a control plane needs and the original could not express. `ip
 * link add veth0 type veth peer name veth1` names both ends, and a bridge is
 * only interesting when several pairs land in the same namespace -- neither is
 * sayable when a namespace has a single device slot and the names are
 * generated from a global counter.
 *
 * Both ends may go in the SAME namespace, which is what `ip link add` does
 * before either end is moved anywhere: Linux creates the pair in the caller's
 * namespace and `ip link set NAME netns N` moves one end afterwards.
 */
long netns_veth_pair_named(void *ns_a, const char *name_a, uint32_t ip_a,
                           void *ns_b, const char *name_b, uint32_t ip_b,
                           uint32_t mask) {
    if (!name_a || !name_b || !*name_a || !*name_b) return -1;
    /* A duplicate name is refused, not silently accepted: two interfaces with
     * one name make every subsequent lookup ambiguous, and the caller would
     * have no way to tell which it got. */
    if (ns_a && net_ns_find_dev(ns_a, name_a)) return -1;
    if (ns_b && net_ns_find_dev(ns_b, name_b)) return -1;

    struct veth_end *a = veth_alloc();
    struct veth_end *b = veth_alloc();
    if (!a || !b) { if (a) a->in_use = false; if (b) b->in_use = false;
                    return -1; }

    static int named_no;
    int n = named_no++;
    a->peer = b; b->peer = a;
    a->ns = ns_a; b->ns = ns_b;

    for (int i = 0; i < 2; i++) {
        struct veth_end *e = i ? b : a;
        const char *nm = i ? name_b : name_a;
        ksnprintf(e->name, sizeof e->name, "%s", nm);
        e->dev.name     = e->name;
        e->dev.priv     = e;
        e->dev.tx       = veth_tx;
        e->dev.rx_drain = veth_rx_drain;
        e->dev.link_up  = veth_link_up;
        e->dev.mac[0] = 0x02; e->dev.mac[1] = 0x00; e->dev.mac[2] = 0x01;
        e->dev.mac[3] = 0x00; e->dev.mac[4] = (uint8_t)n;
        e->dev.mac[5] = (uint8_t)i;
    }

    if (ns_a) { net_ns_add_dev(ns_a, &a->dev);
                if (ip_a) net_ns_set_addr(ns_a, ip_a, mask, 0); }
    else        net_register(&a->dev);
    if (ns_b) { net_ns_add_dev(ns_b, &b->dev);
                if (ip_b) net_ns_set_addr(ns_b, ip_b, mask, 0); }
    else        net_register(&b->dev);

    kprintf("[veth] pair %s <-> %s (named)\n", a->name, b->name);
    return 0;
}

long netns_veth_pair(void *ns_a, uint32_t ip_a, void *ns_b, uint32_t ip_b,
                     uint32_t mask) {
    struct veth_end *a = veth_alloc();
    struct veth_end *b = veth_alloc();
    if (!a || !b) { if (a) a->in_use = false; if (b) b->in_use = false;
                    return -1; }

    static int pair_no;
    int n = pair_no++;
    a->peer = b; b->peer = a;
    a->ns = ns_a; b->ns = ns_b;

    for (int i = 0; i < 2; i++) {
        struct veth_end *e = i ? b : a;
        ksnprintf(e->name, sizeof e->name, "veth%d%c", n, i ? 'b' : 'a');
        e->dev.name     = e->name;
        e->dev.priv     = e;
        e->dev.tx       = veth_tx;
        e->dev.rx_drain = veth_rx_drain;
        e->dev.link_up  = veth_link_up;
        /* Locally-administered MAC (bit 1 of the first octet), unique per end --
         * ARP has to distinguish the two sides or the round trip is meaningless. */
        e->dev.mac[0] = 0x02; e->dev.mac[1] = 0x00; e->dev.mac[2] = 0x00;
        e->dev.mac[3] = 0x00; e->dev.mac[4] = (uint8_t)n;
        e->dev.mac[5] = (uint8_t)i;
    }

    /* Install each end where its namespace expects to find it. */
    if (ns_a) { net_ns_set_dev(ns_a, &a->dev); net_ns_set_addr(ns_a, ip_a, mask, 0); }
    else        net_register(&a->dev);
    if (ns_b) { net_ns_set_dev(ns_b, &b->dev); net_ns_set_addr(ns_b, ip_b, mask, 0); }
    else        net_register(&b->dev);

    char sa[20], sb[20];
    net_format_ip(sa, ip_a); net_format_ip(sb, ip_b);
    kprintf("[veth] pair %s(%s, net:[%lu]) <-> %s(%s, net:[%lu])\n",
            a->name, sa, (unsigned long)net_ns_inum(ns_a),
            b->name, sb, (unsigned long)net_ns_inum(ns_b));
    return 0;
}

void netns_veth_stats(void *ns, uint32_t *tx, uint32_t *rx) {
    struct net_dev *d = net_ns_dev(ns);
    struct veth_end *e = d ? (struct veth_end *)d->priv : 0;
    if (tx) *tx = e ? e->tx_frames : 0;
    if (rx) *rx = e ? e->rx_frames : 0;
}
