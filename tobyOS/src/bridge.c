/* bridge.c -- an ethernet BRIDGE (Phase 3 slice 12, cut 6).
 *
 * ---------------------------------------------------------------------------
 * WHAT A BRIDGE IS FOR HERE
 *
 * A veth pair connects exactly two namespaces. A bridge is what lets MANY
 * namespaces share one attachment point: each container gets a pair, every
 * host-side end is attached to the bridge, and the bridge carries an address
 * that every container can use as its default gateway. That is the shape
 * `docker0` has, and it is what forwarding and NAT are built on top of.
 *
 * Like veth, this needed no change to `struct net_dev`: a bridge is a device
 * whose `tx` hands the frame to its PORTS instead of to hardware. The vtable
 * (tx / rx_drain / link_up / priv) fit unchanged for the third time in this
 * slice.
 *
 * ---------------------------------------------------------------------------
 * IT LEARNS, so it is a bridge and not a hub
 *
 * Flooding every frame to every port is functionally correct -- the wrong
 * container discards it by MAC and then by IP -- and it is also a way to leak
 * every container's traffic to every other container. So the source MAC of an
 * arriving frame is remembered against the port it came in on, and a later
 * unicast to that MAC goes to that port ALONE. Unknown destinations and
 * broadcast/multicast still flood, which is what a real bridge does.
 *
 * ---------------------------------------------------------------------------
 * THE LOOP GUARD IS NOT OPTIONAL
 *
 * A port's `tx` is a veth end, which delivers to its peer, which may itself be
 * a bridge port. Attach both ends of one pair to the same bridge -- a mistake a
 * user can make with two ordinary `ip link set` commands -- and the flood
 * recurses until the kernel stack is gone. There is no spanning-tree protocol
 * here, so a depth counter is the honest defence: it bounds the damage and it
 * is checked rather than assumed.
 */

#include <tobyos/net.h>
#include <tobyos/nsproxy.h>
#include <tobyos/eth.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define BRIDGE_MAX        4
#define BRIDGE_FDB_MAX    16
#define BRIDGE_MAX_DEPTH  4

struct bridge_fdb {
    uint8_t         mac[ETH_ADDR_LEN];
    struct net_dev *port;
    bool            in_use;
};

struct bridge {
    struct net_dev    dev;
    void             *ns;
    char              name[24];
    bool              in_use;
    uint32_t          tx_frames, rx_frames, flooded, unicast;
    struct bridge_fdb fdb[BRIDGE_FDB_MAX];
};

static struct bridge g_bridges[BRIDGE_MAX];
static int           g_bridge_depth;

static struct bridge *br_of(struct net_dev *dev);

/* ---- the forwarding database ---- */

static void fdb_learn(struct bridge *b, const uint8_t *mac,
                      struct net_dev *port) {
    if (mac[0] & 1) return;               /* never learn a group address */
    for (int i = 0; i < BRIDGE_FDB_MAX; i++) {
        if (b->fdb[i].in_use && memcmp(b->fdb[i].mac, mac, ETH_ADDR_LEN) == 0) {
            b->fdb[i].port = port;        /* the station moved */
            return;
        }
    }
    for (int i = 0; i < BRIDGE_FDB_MAX; i++) {
        if (b->fdb[i].in_use) continue;
        memcpy(b->fdb[i].mac, mac, ETH_ADDR_LEN);
        b->fdb[i].port = port;
        b->fdb[i].in_use = true;
        return;
    }
    /* Table full: forget entry 0 rather than stop learning. A bridge that
     * stops learning silently degrades into a hub, which is worse than losing
     * one entry. */
    memcpy(b->fdb[0].mac, mac, ETH_ADDR_LEN);
    b->fdb[0].port = port;
}

static struct net_dev *fdb_lookup(struct bridge *b, const uint8_t *mac) {
    for (int i = 0; i < BRIDGE_FDB_MAX; i++)
        if (b->fdb[i].in_use && memcmp(b->fdb[i].mac, mac, ETH_ADDR_LEN) == 0)
            return b->fdb[i].port;
    return 0;
}

static void fdb_forget_port(struct bridge *b, struct net_dev *port) {
    for (int i = 0; i < BRIDGE_FDB_MAX; i++)
        if (b->fdb[i].in_use && b->fdb[i].port == port)
            memset(&b->fdb[i], 0, sizeof b->fdb[i]);
}

/* ---- forwarding ---- */

/* Send `frame` out one port, honouring its admin state. `in_port` is never a
 * target: a bridge must not send a frame back where it came from. */
static void br_out(struct bridge *b, struct net_dev *p, struct net_dev *in_port,
                   const void *frame, size_t len) {
    if (!p || p == in_port || p == &b->dev) return;
    if (net_ns_dev_master(b->ns, p) != &b->dev) return;
    if (!net_ns_dev_is_up(b->ns, p)) return;
    if (p->tx) p->tx(p, frame, len);
}

static void br_flood(struct bridge *b, struct net_dev *in_port,
                     const void *frame, size_t len) {
    size_t n = net_ns_dev_count(b->ns);
    for (size_t i = 0; i < n; i++)
        br_out(b, net_ns_dev_at(b->ns, i), in_port, frame, len);
}

/* A frame arriving on a PORT. Called from the port's own receive path (veth)
 * instead of handing it to that port's stack -- an enslaved interface has no
 * stack of its own, exactly as on Linux. */
void netns_bridge_input(struct net_dev *brdev, struct net_dev *in_port,
                        const void *frame, size_t len) {
    struct bridge *b = br_of(brdev);
    if (!b || len < ETH_HDR_LEN) return;
    if (!net_ns_dev_is_up(b->ns, brdev)) return;
    if (g_bridge_depth >= BRIDGE_MAX_DEPTH) return;      /* see the header */
    g_bridge_depth++;

    b->rx_frames++;
    const uint8_t *f = (const uint8_t *)frame;
    fdb_learn(b, f + ETH_ADDR_LEN, in_port);             /* source MAC */

    bool group  = (f[0] & 1) != 0;
    bool for_us = memcmp(f, brdev->mac, ETH_ADDR_LEN) == 0;

    if (group) {
        b->flooded++;
        br_flood(b, in_port, frame, len);
    } else if (!for_us) {
        struct net_dev *out = fdb_lookup(b, f);
        if (out) { b->unicast++; br_out(b, out, in_port, frame, len); }
        else     { b->flooded++; br_flood(b, in_port, frame, len); }
    }
    /* ...and up to the bridge's OWN stack when it is addressed to the bridge
     * (or to everyone). This is what gives the bridge an IP the containers can
     * use as their gateway. */
    if (group || for_us) eth_recv_dev(frame, len, b->ns, brdev);

    g_bridge_depth--;
}

/* The bridge transmitting from its own stack: it has no wire, so "sending" is
 * flooding to the ports, narrowed by the FDB when the destination is known. */
static bool bridge_tx(struct net_dev *dev, const void *frame, size_t len) {
    struct bridge *b = br_of(dev);
    if (!b || len < ETH_HDR_LEN) return false;
    if (g_bridge_depth >= BRIDGE_MAX_DEPTH) return false;
    g_bridge_depth++;
    b->tx_frames++;
    const uint8_t *f = (const uint8_t *)frame;
    struct net_dev *out = (f[0] & 1) ? 0 : fdb_lookup(b, f);
    if (out) { b->unicast++; br_out(b, out, 0, frame, len); }
    else     { b->flooded++; br_flood(b, 0, frame, len); }
    g_bridge_depth--;
    return true;
}

static void bridge_rx_drain(struct net_dev *dev) { (void)dev; /* push, not poll */ }

/* A bridge is UP when it has at least one live port. Reporting carrier on a
 * bridge with nothing attached would make an unconfigured bridge look usable. */
static bool bridge_link_up(struct net_dev *dev) {
    struct bridge *b = br_of(dev);
    if (!b) return false;
    size_t n = net_ns_dev_count(b->ns);
    for (size_t i = 0; i < n; i++) {
        struct net_dev *p = net_ns_dev_at(b->ns, i);
        if (p && p != dev && net_ns_dev_master(b->ns, p) == dev) return true;
    }
    return false;
}

static struct bridge *br_of(struct net_dev *dev) {
    if (!dev || dev->tx != bridge_tx) return 0;
    struct bridge *b = (struct bridge *)dev->priv;
    return (b && b->in_use) ? b : 0;
}

bool netns_bridge_is_bridge(struct net_dev *dev) { return br_of(dev) != 0; }

long netns_bridge_create(void *ns, const char *name, struct net_dev **out) {
    if (!name || !*name) return -1;
    if (net_ns_find_dev(ns, name)) return -1;
    struct bridge *b = 0;
    for (int i = 0; i < BRIDGE_MAX; i++)
        if (!g_bridges[i].in_use) { b = &g_bridges[i]; break; }
    if (!b) return -1;

    memset(b, 0, sizeof *b);
    b->in_use = true;
    b->ns     = ns;
    ksnprintf(b->name, sizeof b->name, "%s", name);
    b->dev.name     = b->name;
    b->dev.priv     = b;
    b->dev.tx       = bridge_tx;
    b->dev.rx_drain = bridge_rx_drain;
    b->dev.link_up  = bridge_link_up;
    /* Locally-administered, distinct from veth's 02:00:0[012]:.. prefixes so a
     * MAC in a trace names which kind of device emitted it. */
    b->dev.mac[0] = 0x02; b->dev.mac[1] = 0x00; b->dev.mac[2] = 0x03;
    b->dev.mac[3] = 0x00; b->dev.mac[4] = 0x00;
    b->dev.mac[5] = (uint8_t)(b - g_bridges);

    if (!net_ns_add_dev(ns, &b->dev)) { b->in_use = false; return -1; }
    if (out) *out = &b->dev;
    kprintf("[bridge] created %s in net:[%lu]\n",
            b->name, (unsigned long)net_ns_inum(ns));
    return 0;
}

bool netns_bridge_delete(struct net_dev *dev) {
    struct bridge *b = br_of(dev);
    if (!b) return false;
    /* Release every port first: a device left pointing at a freed bridge would
     * hand netns_bridge_input a dangling pointer on the next frame. */
    size_t n = net_ns_dev_count(b->ns);
    for (size_t i = 0; i < n; i++) {
        struct net_dev *p = net_ns_dev_at(b->ns, i);
        if (p && net_ns_dev_master(b->ns, p) == dev)
            net_ns_set_dev_master(b->ns, p, 0);
    }
    net_ns_del_dev(b->ns, dev);
    b->in_use = false;
    return true;
}

void netns_bridge_port_gone(struct net_dev *port) {
    for (int i = 0; i < BRIDGE_MAX; i++)
        if (g_bridges[i].in_use) fdb_forget_port(&g_bridges[i], port);
}

void netns_bridge_stats(struct net_dev *dev, uint32_t *tx, uint32_t *rx,
                        uint32_t *flooded, uint32_t *unicast) {
    struct bridge *b = br_of(dev);
    if (tx)       *tx       = b ? b->tx_frames : 0;
    if (rx)       *rx       = b ? b->rx_frames : 0;
    if (flooded)  *flooded  = b ? b->flooded   : 0;
    if (unicast)  *unicast  = b ? b->unicast   : 0;
}
