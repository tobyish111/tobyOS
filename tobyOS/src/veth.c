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
 * address is now per-namespace. See net_ctx_enter() and net_my_ip().
 * ---------------------------------------------------------------------------
 *
 * CUT 4 ADDED THE CONTROL PLANE -- this comment used to say there was none.
 * `ip link add ... type veth` now works: src/rtnetlink.c parses RTM_NEWLINK and
 * calls netns_veth_create() below. The pre-cut-4 entry points (netns_veth_pair,
 * netns_veth_pair_named) are unchanged and still kernel-only.
 *
 * WHAT IS STILL DELIBERATELY NOT HERE
 *
 *   - No bridge. A bridge is what lets MANY namespaces share one uplink; a pair
 *     is what proves frames cross a namespace boundary at all. The pair is the
 *     foundation, so it comes first.
 *   - No NAT/forwarding to the outside world. A container can reach its peer end
 *     (the host side of the pair); it cannot reach the internet, because that
 *     needs forwarding + address translation on top of a bridge.
 *
 * So this file means: REAL FRAMES CROSS A NAMESPACE BOUNDARY, with per-namespace
 * addressing, a working ARP+IP round trip, and interfaces userspace can create,
 * address, move and take down. It does not yet mean "a container can reach the
 * network".
 */

#include <tobyos/net.h>
#include <tobyos/nsproxy.h>
#include <tobyos/eth.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* 8 -> 16 with the control plane: userspace can now create pairs at will, and a
 * combined gate run (LXVETH + the rtnetlink selftest + /bin/linux-netlink + a
 * busybox `ip` witness) reaches 14 live ends at its peak. Ends ARE reclaimed
 * when a namespace dies (netns_veth_release), so this is headroom rather than a
 * substitute for that. */
#define VETH_MAX_PAIRS 16

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
    /* ADMIN-DOWN IS ENFORCED ON BOTH SIDES. Checking only tx would let a frame
     * INTO an interface that `ip link set X down` had just taken out of
     * service, which is the half of "down" that matters for isolation. The
     * counter is bumped only for a frame that is actually accepted, so a test
     * can tell "dropped" from "delivered" rather than having to trust a flag. */
    if (!net_ns_dev_is_up(dst->ns, &dst->dev)) return;
    dst->rx_frames++;

    /* eth_recv() is the stack's own ethertype demux -- reuse it rather than
     * re-implementing the dispatch, so a veth frame takes byte-for-byte the same
     * path through arp/ip/ipv6 that a wire frame does. Anything that works on
     * e1000 therefore works here by construction.
     *
     * Bracketed so it is processed AS the receiving namespace: everything below
     * compares against "my IP", which is per-namespace now. Without this the
     * container's stack would answer for the host's address.
     *
     * Cut 5 passes the DEVICE too, not just the namespace: a namespace holding
     * two interfaces used to answer for its PRIMARY's address no matter which
     * end the frame came in on, so the second interface could never complete an
     * ARP round trip for its own address. */
    eth_recv_dev(frame, len, dst->ns, &dst->dev);
}

static bool veth_tx(struct net_dev *dev, const void *frame, size_t len) {
    struct veth_end *e = (struct veth_end *)dev->priv;
    if (!e || !e->peer) return false;
    if (!net_ns_dev_is_up(e->ns, &e->dev)) return false;
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

    /* Address the DEVICE, not the namespace: two ends in one namespace need
     * two addresses, and net_ns_set_addr would give the second the first's. */
    if (ns_a) { net_ns_add_dev(ns_a, &a->dev);
                if (ip_a) net_ns_set_dev_addr(ns_a, &a->dev, ip_a, mask, 0); }
    else        net_register(&a->dev);
    if (ns_b) { net_ns_add_dev(ns_b, &b->dev);
                if (ip_b) net_ns_set_dev_addr(ns_b, &b->dev, ip_b, mask, 0); }
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

/* ---- cut 4: what the netlink control plane drives -----------------------
 *
 * `ip link add name v0 type veth` creates BOTH ends in the CALLER's namespace
 * and moves one afterwards -- that is Linux's order, and it is why this exists
 * separately from netns_veth_pair_named(), which registers a NULL-namespace end
 * with net.c instead of putting it in a namespace list. A control-plane device
 * must always be in a list: that list is what the RTM_GETLINK dump enumerates,
 * and a device the dump cannot see is exactly the "ip link add succeeded but
 * the dump still says lo and eth0" trap. */
static bool veth_name_taken(void *ns, const char *n) {
    return net_ns_find_dev(ns, n) != 0;
}

long netns_veth_create(void *ns, const char *name, const char *peer_name,
                       struct net_dev **out_a, struct net_dev **out_b) {
    char peer[24];
    if (!name || !*name) return -1;
    if (veth_name_taken(ns, name)) return -1;

    if (peer_name && *peer_name) {
        if (veth_name_taken(ns, peer_name)) return -1;
        ksnprintf(peer, sizeof peer, "%s", peer_name);
    } else {
        /* No VETH_INFO_PEER: Linux's veth driver auto-names the peer vethN,
         * and busybox's `ip` cannot send a peer name at all (its iplink keyword
         * set is link/name/type/dev/address -- no `peer`), so this is the path
         * the third-party witness actually takes. */
        int i;
        for (i = 0; i < 64; i++) {
            ksnprintf(peer, sizeof peer, "veth%d", i);
            if (!veth_name_taken(ns, peer) && strcmp(peer, name) != 0) break;
        }
        if (i == 64) return -1;
    }

    struct veth_end *a = veth_alloc();
    struct veth_end *b = veth_alloc();
    if (!a || !b) { if (a) a->in_use = false; if (b) b->in_use = false;
                    return -1; }

    static int create_no;
    int n = create_no++;
    a->peer = b; b->peer = a;
    a->ns = ns; b->ns = ns;

    for (int i = 0; i < 2; i++) {
        struct veth_end *e = i ? b : a;
        ksnprintf(e->name, sizeof e->name, "%s", i ? peer : name);
        e->dev.name     = e->name;
        e->dev.priv     = e;
        e->dev.tx       = veth_tx;
        e->dev.rx_drain = veth_rx_drain;
        e->dev.link_up  = veth_link_up;
        e->dev.mac[0] = 0x02; e->dev.mac[1] = 0x00; e->dev.mac[2] = 0x02;
        e->dev.mac[3] = 0x00; e->dev.mac[4] = (uint8_t)n;
        e->dev.mac[5] = (uint8_t)i;
    }

    if (!net_ns_add_dev(ns, &a->dev) || !net_ns_add_dev(ns, &b->dev)) {
        /* Partial success is worse than failure: a namespace holding one half
         * of a pair looks like a working interface and can never carry a
         * frame. Undo both. */
        net_ns_del_dev(ns, &a->dev);
        net_ns_del_dev(ns, &b->dev);
        a->in_use = b->in_use = false;
        return -1;
    }
    if (out_a) *out_a = &a->dev;
    if (out_b) *out_b = &b->dev;
    kprintf("[veth] created %s <-> %s in net:[%lu]\n",
            a->name, b->name, (unsigned long)net_ns_inum(ns));
    return 0;
}

bool netns_veth_is_veth(struct net_dev *dev) {
    return dev && dev->tx == veth_tx;
}

/* `ip link del` removes BOTH ends, as on Linux -- half a pair is not a device.
 *
 * REFUSED unless both ends are held by a namespace list. The pre-control-plane
 * netns_veth_pair() path hands a NULL-namespace end to net_register(), and
 * net.c has no unregister; freeing such an end would leave a dangling pointer
 * in g_net_devs. Refusing is the honest answer and the control plane never
 * creates one of those. */
bool netns_veth_delete(struct net_dev *dev) {
    if (!netns_veth_is_veth(dev)) return false;
    struct veth_end *e = (struct veth_end *)dev->priv;
    if (!e || !e->in_use) return false;
    struct veth_end *p = e->peer;
    if (!net_ns_dev_index(e->ns, &e->dev)) return false;
    if (p && !net_ns_dev_index(p->ns, &p->dev)) return false;

    net_ns_del_dev(e->ns, &e->dev);
    if (p) { net_ns_del_dev(p->ns, &p->dev); p->peer = 0; p->in_use = false; }
    e->peer = 0;
    e->in_use = false;
    return true;
}

/* Namespace teardown, NOT `ip link del`: free THIS end and leave the peer
 * alive but unpaired. See the comment in net_ns_put() for why the asymmetry is
 * the honest behaviour rather than a shortcut. */
void netns_veth_release(struct net_dev *dev) {
    if (!netns_veth_is_veth(dev)) return;
    struct veth_end *e = (struct veth_end *)dev->priv;
    if (!e || !e->in_use) return;
    if (e->peer) { e->peer->peer = 0; e->peer = 0; }
    e->in_use = false;
}

/* `ip link set DEV netns PID`. The device's ADDRESS is dropped, which is what
 * Linux does and is not a shortcut: an address is meaningful only in the
 * namespace whose routing it belongs to, and carrying 10.0.2.15 into a
 * container would make the container answer for the host. */
bool netns_veth_move_ns(struct net_dev *dev, void *ns) {
    if (!netns_veth_is_veth(dev)) return false;
    struct veth_end *e = (struct veth_end *)dev->priv;
    if (!e || !e->in_use) return false;
    if (e->ns == ns) return true;
    if (net_ns_find_dev(ns, e->name)) return false;   /* name clash over there */
    if (!net_ns_dev_index(e->ns, &e->dev)) return false;  /* not ours to move */
    void *old = e->ns;
    if (!net_ns_add_dev(ns, &e->dev)) return false;
    net_ns_del_dev(old, &e->dev);
    e->ns = ns;
    return true;
}

/* Per-DEVICE counters. netns_veth_stats() below reports the namespace's PRIMARY
 * device, which stops meaning anything once a namespace holds several -- and
 * "did THIS interface stop passing frames when I took it down" is a question
 * only a per-device counter can answer. */
void netns_veth_dev_stats(struct net_dev *dev, uint32_t *tx, uint32_t *rx) {
    struct veth_end *e = netns_veth_is_veth(dev) ?
                         (struct veth_end *)dev->priv : 0;
    if (tx) *tx = e ? e->tx_frames : 0;
    if (rx) *rx = e ? e->rx_frames : 0;
}

void netns_veth_stats(void *ns, uint32_t *tx, uint32_t *rx) {
    struct net_dev *d = net_ns_dev(ns);
    struct veth_end *e = d ? (struct veth_end *)d->priv : 0;
    if (tx) *tx = e ? e->tx_frames : 0;
    if (rx) *rx = e ? e->rx_frames : 0;
}
