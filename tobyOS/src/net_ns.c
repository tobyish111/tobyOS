/* net_ns.c -- NETWORK namespaces, cut 1 (Phase 3 slice 12).
 *
 * ---------------------------------------------------------------------------
 * WHAT CUT 1 IS, AND WHAT IT IS NOT
 *
 * The plan called this "the one that can blow up" and estimated ~93 static
 * globals across net.c/tcp.c/socket.c needing to gather into a `struct net_ns`.
 * Two measurements changed the shape of the work:
 *
 *   1. There are ~38 static variables across those files, not ~93.
 *   2. THIS STACK HAS NO LOOPBACK. There is no 127.0.0.1 path anywhere in
 *      socket.c / tcp.c / ip.c -- grep finds not one reference.
 *
 * (2) is the important one. The plan's cut 1 was "CLONE_NEWNET yields an EMPTY
 * namespace (loopback only)". With no loopback to speak of, an empty namespace
 * means simply NO NETWORK -- which is precisely the property a sandbox wants,
 * and is reached WITHOUT replicating any of that state.
 *
 * So cut 1 deliberately does NOT namespace the network stack. The ARP table,
 * routes, interface list and TCP connection table all remain the initial
 * namespace's, and a new namespace is empty because it has no interfaces --
 * so there is nothing to replicate, and every network operation from inside it
 * is refused at the socket boundary.
 *
 * BE PRECISE ABOUT THIS WHEN READING RESULTS: cut 1 achieves isolation by
 * DENIAL, not by replication. It is honest (an interface-less namespace really
 * can't reach anything) and it permanently unblocks slice 14, but it is not a
 * network namespace in the full sense. Cut 2 -- veth pairs and a bridge, so a
 * container can actually reach the network -- is the refactor the plan
 * described, and it still has to gather those globals.
 * ---------------------------------------------------------------------------
 */

#include <tobyos/nsproxy.h>
#include <tobyos/net.h>
#include <tobyos/proc.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/abi/abi.h>

struct net_ns {
    int      refs;
    uint64_t inum;
    /* ---- cut 2 ----
     * The namespace's interface. ONE device is enough for a container: a veth
     * end. Deliberately NOT a refactor of net.c's `g_net_devs` -- the INITIAL
     * namespace keeps that array untouched, so e1000, DHCP and everything built
     * on them are byte-for-byte unaffected, and a non-initial namespace simply
     * has its own device here. Same "NULL == initial, zero behaviour change"
     * shape as slices 8-12, and it turns a risky rewrite of the stack every
     * other gate depends on into an additive change. */
    /* CUT 3: a LIST, because one device per namespace cannot express what a
     * control plane is for. `ip link add` creates interfaces by name, and a
     * bridge exists precisely to have several attached to it -- neither is
     * sayable when the namespace has a single `dev` slot.
     *
     * MEASURED BEFORE REFACTORING, exactly as cut 1's note says to: the whole
     * blast radius was SIX call sites (eth.c, net.c, veth.c x2, and the two
     * accessors here), not the sweeping change "the one that can blow up"
     * implies. devs[0] stays THE primary device, so net_ns_dev() answers what
     * it always did and every existing caller is untouched.
     *
     * Addressing is still per-NAMESPACE rather than per-device (ip/netmask/
     * gateway below). That is a stated limit, not an oversight: `ip addr add`
     * on a second interface therefore has nowhere to put the address, and
     * making it truthful means one address block per device. Named here so the
     * next cut starts from the gap rather than discovering it. */
#define NET_NS_MAX_DEVS 8
    /* CUT 3b: the ADDRESS travels with the device.
     *
     * ip/netmask/gateway used to be per-NAMESPACE, which is fine while a
     * namespace has one interface and wrong the moment it has two: RTM_NEWADDR
     * names an interface, so a per-namespace address gives `ip addr add` two
     * interfaces and one place to put an answer. Writing the netlink handler
     * against that would have meant writing one that lies -- it would report
     * success and the second interface would silently carry the first's
     * address.
     *
     * The per-namespace accessors below still answer for devs[0], so net.c,
     * eth.c and the ARP path are unchanged. */
    /* CUT 4 (the control plane) adds the two fields rtnetlink cannot work
     * without:
     *
     *   ifindex -- netlink names an interface by NUMBER, not by name. RTM_NEWADDR
     *     carries ifa_index and nothing else, so without a stable per-namespace
     *     index there is no way to say which interface an address belongs to.
     *     It must survive deletion of an earlier device, hence a counter rather
     *     than the array position.
     *
     *   up -- the ADMIN state, and it is ENFORCED (veth.c consults it before it
     *     will move a frame in either direction). A stored-and-ignored IFF_UP
     *     would report a guarantee that does not exist, which is the exact shape
     *     of lie this arc keeps deleting.
     *
     * DELIBERATE DEVIATION FROM LINUX, stated rather than hidden: a device here
     * is created UP. Linux creates one DOWN and requires `ip link set X up`,
     * but this kernel has no admin-state concept for its real NIC either, and
     * every pre-control-plane veth (netns_veth_pair, LXVETH) passes frames the
     * instant it exists. Defaulting to DOWN would have made that gate fail for
     * a semantic nothing else here honours. Taking a device DOWN is real. */
    struct net_ns_if {
        struct net_dev *dev;
        uint32_t        ip;        /* network byte order */
        uint32_t        netmask;
        uint32_t        gateway;
        int             ifindex;
        bool            up;
    }               ifs[NET_NS_MAX_DEVS];
    size_t          ndev;
    int             next_ifindex;
    uint32_t        ip;           /* network byte order */
    uint32_t        netmask;
    uint32_t        gateway;
};

/* THE INITIAL NAMESPACE CAN HOLD CONTROL-PLANE DEVICES TOO, and next_ifindex
 * starts at 3 because rtnetlink reports its lo as 1 and eth0 as 2 -- those two
 * are net.c's, not this list's, and a collision would make `ip addr add` name
 * the wrong interface.
 *
 * That the initial namespace may hold a list at all is a change from cut 3, and
 * it was MEASURED before it was made: every packet-path caller of net_ns_dev()
 * / net_ns_ip() / net_ns_netmask() (eth.c:51, net.c:99, net_my_ip, net_my_mac,
 * net_my_netmask) guards on the RAW namespace pointer being non-NULL, and the
 * initial namespace IS the NULL pointer. So none of them ever consults this
 * list for the initial namespace, and e1000/DHCP/ARP are unaffected by
 * construction. Without this, `ip link add` would have been refusable only in
 * the initial namespace -- and the standard container recipe is to create the
 * pair on the host and move one end in. */
static struct net_ns g_init_net_ns = {
    .refs = 1, .inum = NS_INUM_INIT_NET, .next_ifindex = 3,
};

static spinlock_t g_netns_lock = SPINLOCK_INIT;

static inline struct net_ns *as_nns(void *p) {
    return p ? (struct net_ns *)p : &g_init_net_ns;
}

void *net_ns_create(void) {
    struct net_ns *ns = kmalloc(sizeof(*ns));
    if (!ns) return 0;
    memset(ns, 0, sizeof(*ns));
    ns->refs = 1;
    ns->inum = ns_inum_alloc();
    /* 1, not 3: a fresh namespace has no lo and no eth0 to leave room for.
     * REPORTING A LOOPBACK IT DOES NOT HAVE WOULD BE THE FABRICATION -- this
     * stack has no loopback anywhere (cut 1's founding measurement), so a new
     * namespace's first interface really is interface 1. */
    ns->next_ifindex = 1;
    kprintf("[netns] created net:[%lu] -- EMPTY (no interfaces, no routes)\n",
            (unsigned long)ns->inum);
    return ns;
}

void net_ns_get(void *p) {
    struct net_ns *ns = (struct net_ns *)p;
    if (!ns || ns == &g_init_net_ns) return;
    uint64_t f = spin_lock_irqsave(&g_netns_lock);
    ns->refs++;
    spin_unlock_irqrestore(&g_netns_lock, f);
}

void net_ns_put(void *p) {
    struct net_ns *ns = (struct net_ns *)p;
    if (!ns || ns == &g_init_net_ns) return;
    uint64_t f = spin_lock_irqsave(&g_netns_lock);
    int left = --ns->refs;
    spin_unlock_irqrestore(&g_netns_lock, f);
    if (left > 0) return;
    /* A namespace that dies takes its interfaces with it. Before the control
     * plane nothing could create one at runtime, so leaking the veth slots was
     * invisible; now every `ip link add` inside a container that exits would
     * burn two of them permanently and the pool would run dry mid-gate.
     *
     * The PEER is deliberately NOT deleted, only unpaired: it may live in
     * another namespace that is still running, and that namespace still owns
     * it. It goes carrier-down (veth_link_up is "my peer exists"), which is
     * exactly what Linux reports for the surviving half of a torn-down pair. */
    for (size_t i = 0; i < ns->ndev; i++)
        if (ns->ifs[i].dev) netns_veth_release(ns->ifs[i].dev);
    kfree(ns);
}

uint64_t net_ns_inum(void *p) { return as_nns(p)->inum; }

/* THE enforcement predicate, and the only one.
 *
 * A socket carries the network namespace it was created in (latched in
 * sock_alloc alongside the SO_PEERCRED creds). Every operation that would touch
 * a real interface asks this, and a non-initial namespace answers "no" --
 * because it has no interfaces, which is the literal truth rather than a policy
 * decision.
 *
 * Deliberately a property of the SOCKET, not of the calling process: a socket
 * created before unshare(CLONE_NEWNET) keeps its network, exactly as on Linux,
 * where a namespace change does not retroactively unplug open sockets. That is
 * also what lets a sandboxed child keep an inherited socketpair. */
bool net_ns_has_network(void *sock_ns) {
    struct net_ns *ns = as_nns(sock_ns);
    if (ns == &g_init_net_ns) return true;
    /* CUT 2 CHANGES THIS ANSWER. In cut 1 a non-initial namespace was always
     * "no network" because it could not possibly have an interface. Now it can:
     * a namespace with a veth end has a route, so it is networked. A namespace
     * without one is still empty -- which is exactly the sandbox case, and the
     * cut-1 acceptance test still passes because it never creates a veth. */
    return ns->ndev != 0;
}

/* ---- cut 2 accessors, used by veth.c and the IP/ARP path ---- */

/* The PRIMARY device: devs[0]. Every pre-cut-3 caller means this one, and
 * keeping the name pointing at it is what made the list additive. */
struct net_dev *net_ns_dev(void *nsp) {
    struct net_ns *ns = as_nns(nsp);
    return ns->ndev ? ns->ifs[0].dev : 0;
}
size_t net_ns_dev_count(void *nsp) { return as_nns(nsp)->ndev; }
struct net_dev *net_ns_dev_at(void *nsp, size_t i) {
    struct net_ns *ns = as_nns(nsp);
    return (i < ns->ndev) ? ns->ifs[i].dev : 0;
}
struct net_dev *net_ns_find_dev(void *nsp, const char *name) {
    struct net_ns *ns = as_nns(nsp);
    if (!name) return 0;
    for (size_t i = 0; i < ns->ndev; i++)
        if (ns->ifs[i].dev && ns->ifs[i].dev->name &&
            strcmp(ns->ifs[i].dev->name, name) == 0) return ns->ifs[i].dev;
    return 0;
}

/* ---- per-DEVICE addressing ---- */
static struct net_ns_if *nsif(struct net_ns *ns, struct net_dev *dev) {
    for (size_t i = 0; i < ns->ndev; i++)
        if (ns->ifs[i].dev == dev) return &ns->ifs[i];
    return 0;
}
/* Addressing a device the namespace HOLDS is allowed even in the initial
 * namespace -- that device came from the control plane, not from net.c. What
 * stays refused is addressing anything net.c owns: e1000 is not in this list,
 * so nsif() misses and `ip addr add ... dev eth0` gets ENODEV rather than
 * silently overwriting the DHCP lease. */
bool net_ns_set_dev_addr(void *nsp, struct net_dev *dev, uint32_t ip,
                         uint32_t mask, uint32_t gw) {
    struct net_ns *ns = as_nns(nsp);
    struct net_ns_if *e = nsif(ns, dev);
    if (!e) return false;      /* addressing an interface this ns does not have
                                * is a refusal, not a silent no-op */
    e->ip = ip; e->netmask = mask; e->gateway = gw;
    return true;
}
uint32_t net_ns_dev_ip(void *nsp, struct net_dev *dev) {
    struct net_ns_if *e = nsif(as_nns(nsp), dev);
    return e ? e->ip : 0;
}
uint32_t net_ns_dev_netmask(void *nsp, struct net_dev *dev) {
    struct net_ns_if *e = nsif(as_nns(nsp), dev);
    return e ? e->netmask : 0;
}
/* Append. Returns false when the namespace is full -- a refusal, because a
 * device that is "added" and silently absent is the kind of half-answer this
 * arc keeps deleting. */
bool net_ns_add_dev(void *nsp, struct net_dev *dev) {
    struct net_ns *ns = as_nns(nsp);
    if (!dev) return false;
    if (ns->ndev >= NET_NS_MAX_DEVS) return false;
    for (size_t i = 0; i < ns->ndev; i++) if (ns->ifs[i].dev == dev) return true;
    ns->ifs[ns->ndev].dev = dev;
    ns->ifs[ns->ndev].ip = 0;
    ns->ifs[ns->ndev].netmask = 0;
    ns->ifs[ns->ndev].gateway = 0;
    ns->ifs[ns->ndev].ifindex = ns->next_ifindex++;
    ns->ifs[ns->ndev].up = true;              /* see the struct comment */
    ns->ndev++;
    return true;
}
bool net_ns_del_dev(void *nsp, struct net_dev *dev) {
    struct net_ns *ns = as_nns(nsp);
    for (size_t i = 0; i < ns->ndev; i++) {
        if (ns->ifs[i].dev != dev) continue;
        for (size_t j = i + 1; j < ns->ndev; j++) ns->ifs[j - 1] = ns->ifs[j];
        ns->ndev--;
        ns->ifs[ns->ndev].dev = 0;
        return true;
    }
    return false;
}
/* The per-NAMESPACE accessors answer for the PRIMARY interface, which is
 * what every pre-cut-3b caller means by "this namespace's address". */
uint32_t net_ns_ip(void *nsp) {
    struct net_ns *ns = as_nns(nsp);
    return ns->ndev ? ns->ifs[0].ip : 0;
}
uint32_t net_ns_netmask(void *nsp) {
    struct net_ns *ns = as_nns(nsp);
    return ns->ndev ? ns->ifs[0].netmask : 0;
}
uint32_t net_ns_gateway(void *nsp) {
    struct net_ns *ns = as_nns(nsp);
    return ns->ndev ? ns->ifs[0].gateway : 0;
}

void net_ns_set_dev(void *nsp, struct net_dev *dev) {
    struct net_ns *ns = as_nns(nsp);
    if (ns == &g_init_net_ns) return;      /* the initial ns keeps g_net_devs */
    /* Install as the PRIMARY. Existing callers (veth.c) mean "this namespace's
     * interface", and that is devs[0]; additional ones go through
     * net_ns_add_dev. */
    if (ns->ndev == 0) { ns->ifs[0].dev = dev; ns->ifs[0].ip = 0;
                         ns->ifs[0].netmask = 0; ns->ifs[0].gateway = 0;
                         ns->ifs[0].ifindex = ns->next_ifindex++;
                         ns->ifs[0].up = true;
                         ns->ndev = 1; }
    else                 ns->ifs[0].dev = dev;
}

/* ---- cut 4: what a control plane needs on top of the list ---- */

/* The netlink interface INDEX of `dev` in `ns`, or 0 for "this namespace does
 * not have it" -- 0 is never a valid ifindex on Linux either, so the caller
 * cannot mistake absence for interface zero. */
int net_ns_dev_index(void *nsp, struct net_dev *dev) {
    struct net_ns_if *e = nsif(as_nns(nsp), dev);
    return e ? e->ifindex : 0;
}
struct net_dev *net_ns_dev_by_index(void *nsp, int idx) {
    struct net_ns *ns = as_nns(nsp);
    if (idx <= 0) return 0;
    for (size_t i = 0; i < ns->ndev; i++)
        if (ns->ifs[i].ifindex == idx) return ns->ifs[i].dev;
    return 0;
}
/* A device this namespace does not hold answers UP. That is the honest answer
 * for e1000 in the initial namespace: net.c owns it, there is no admin state
 * for it anywhere in this kernel, and returning "down" would stop the host's
 * traffic on a question this list has no business answering. */
bool net_ns_dev_is_up(void *nsp, struct net_dev *dev) {
    struct net_ns_if *e = nsif(as_nns(nsp), dev);
    return e ? e->up : true;
}
bool net_ns_set_dev_up(void *nsp, struct net_dev *dev, bool up) {
    struct net_ns_if *e = nsif(as_nns(nsp), dev);
    if (!e) return false;
    e->up = up;
    return true;
}
uint32_t net_ns_dev_gateway(void *nsp, struct net_dev *dev) {
    struct net_ns_if *e = nsif(as_nns(nsp), dev);
    return e ? e->gateway : 0;
}
void net_ns_set_addr(void *nsp, uint32_t ip, uint32_t mask, uint32_t gw) {
    struct net_ns *ns = as_nns(nsp);
    if (ns == &g_init_net_ns) return;      /* net.c owns the initial config */
    if (!ns->ndev) return;                 /* no interface to address */
    ns->ifs[0].ip = ip; ns->ifs[0].netmask = mask; ns->ifs[0].gateway = gw;
}

/* ---- the RECEIVE-SIDE namespace context -------------------------------
 * A frame handed to a veth end has to be processed AS the namespace that owns
 * that end: ip_dst_is_for_us() and arp must compare against THAT namespace's
 * address, not the host's. The packet path is a synchronous call chain with no
 * namespace parameter, so the delivering code brackets it with this.
 *
 * Not a lock and not per-CPU on purpose: veth delivery happens inline on the
 * sender's stack under the BKL, exactly like every other packet path here, so a
 * single depth-1 save/restore is sufficient and honest. If veth delivery ever
 * becomes concurrent this must become per-CPU -- noted rather than pretended. */
static void *g_rx_ns;

void *net_ns_rx_enter(void *ns) { void *prev = g_rx_ns; g_rx_ns = ns; return prev; }
void  net_ns_rx_leave(void *prev) { g_rx_ns = prev; }
void *net_ns_rx_current(void) { return g_rx_ns; }

bool net_ns_is_initial(struct proc *p) {
    return !p || !p->net_ns;
}
