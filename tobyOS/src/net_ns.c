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
    struct net_dev *dev;          /* NULL == no interface (cut-1 behaviour) */
    uint32_t        ip;           /* network byte order */
    uint32_t        netmask;
    uint32_t        gateway;
};

static struct net_ns g_init_net_ns = {
    .refs = 1, .inum = NS_INUM_INIT_NET,
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
    if (left <= 0) kfree(ns);
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
    return ns->dev != 0;
}

/* ---- cut 2 accessors, used by veth.c and the IP/ARP path ---- */

struct net_dev *net_ns_dev(void *nsp)      { return as_nns(nsp)->dev; }
uint32_t        net_ns_ip(void *nsp)       { return as_nns(nsp)->ip; }
uint32_t        net_ns_netmask(void *nsp)  { return as_nns(nsp)->netmask; }
uint32_t        net_ns_gateway(void *nsp)  { return as_nns(nsp)->gateway; }

void net_ns_set_dev(void *nsp, struct net_dev *dev) {
    struct net_ns *ns = as_nns(nsp);
    if (ns == &g_init_net_ns) return;      /* the initial ns keeps g_net_devs */
    ns->dev = dev;
}
void net_ns_set_addr(void *nsp, uint32_t ip, uint32_t mask, uint32_t gw) {
    struct net_ns *ns = as_nns(nsp);
    if (ns == &g_init_net_ns) return;      /* net.c owns the initial config */
    ns->ip = ip; ns->netmask = mask; ns->gateway = gw;
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
