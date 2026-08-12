/* rtnetlink.c -- the NETWORKING CONTROL PLANE (Phase 3 slice 12, cut 4).
 *
 * ---------------------------------------------------------------------------
 * WHAT WAS HERE BEFORE, AND WHY IT HAD TO GROW
 *
 * Cuts 1-3 built network namespaces, veth pairs and a per-namespace device
 * list with per-device addressing. Every one of those was created by a KERNEL
 * CALL. netlink existed only to BUILD REPLY MESSAGES for two dumps, and those
 * dumps answered with a hardcoded `lo` + `eth0` regardless of which namespace
 * asked. So `ip link add` could not work, and nothing in userspace could
 * observe or change the interface set.
 *
 * This file is the other half: rtnetlink requests are PARSED and EXECUTED, and
 * the dumps report what is actually there, per namespace.
 *
 * THE TRAP THIS FILE EXISTS TO AVOID, named in cut 2's notes before any of it
 * was written: a dump that still says "lo and eth0" after `ip link add`
 * succeeded would be WORSE than no control plane at all -- it would report
 * success and leave every reader with a false picture. So the dump work comes
 * first here (rtnl_dump_links / rtnl_dump_addrs) and the command handlers are
 * written against it, not beside it.
 *
 * ---------------------------------------------------------------------------
 * THE INITIAL NAMESPACE'S REPLY IS BYTE-IDENTICAL, AND THAT IS LOAD-BEARING
 *
 * Chromium's net::AddressTrackerLinux reads these dumps. A link that is online
 * must report IFF_UP|IFF_LOWER_UP|IFF_RUNNING and must NOT be IFF_LOOPBACK, or
 * it leaves online_links_ empty and chrome concludes CONNECTION_NONE -- "this
 * machine is offline". glibc's check_pf runs an RTM_GETADDR on EVERY
 * getaddrinfo and drops any reply whose nlmsg_pid is not its own, and a dropped
 * dump makes it filter out every IPv4 answer, so name resolution returns
 * nothing at all.
 *
 * The initial namespace therefore emits EXACTLY the messages it always did --
 * same order, same attributes, same flags -- and only then appends whatever the
 * control plane has created in it (nothing, until someone runs `ip link add`).
 * The additive shape is the one that has worked all through this arc.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS DELIBERATELY NOT HERE
 *
 *   - No BRIDGE and no NAT/forwarding. Those are the next two steps and each is
 *     its own piece of work; half-building a bridge on top of a control plane
 *     that has not been proven would repeat the mistake this file avoids.
 *   - No RTM_*ROUTE / RTM_*NEIGH commands. The dumps for them answer "nothing",
 *     which they always did, and a route table this stack does not have would
 *     be a fabrication. Routing here is "my subnet or the gateway", in ip.c.
 *   - A NON-INITIAL namespace REPORTS NO `lo`. Linux always creates one; this
 *     stack has no loopback anywhere (cut 1's founding measurement), so
 *     inventing one in the dump would be reporting an interface that cannot
 *     carry a packet.
 *   - One reply DATAGRAM per request. Linux splits a large dump across several;
 *     with at most 8 devices per namespace the whole answer is under 1 KiB, and
 *     nlb_msg_begin refuses to emit a partial message rather than corrupting
 *     one that is already complete.
 */

#include <tobyos/net.h>
#include <tobyos/eth.h>
#include <tobyos/arp.h>
#include <tobyos/socket.h>   /* the cross-namespace UDP subtest (cut 5) */
#include <tobyos/udp.h>
#include <tobyos/nsproxy.h>
#include <tobyos/proc.h>
#include <tobyos/rtnetlink.h>
#include <tobyos/uaccess.h>   /* the SIOC* ioctls copy a struct ifreq */
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/abi/abi.h>

/* ---- the wire vocabulary ---------------------------------------------- */

#define NL_NLMSG_ERROR     2
#define NL_NLMSG_DONE      3

#define NL_RTM_NEWLINK    16
#define NL_RTM_DELLINK    17
#define NL_RTM_GETLINK    18
#define NL_RTM_SETLINK    19
#define NL_RTM_NEWADDR    20
#define NL_RTM_DELADDR    21
#define NL_RTM_GETADDR    22

#define NL_NLM_F_REQUEST 0x001
#define NL_NLM_F_MULTI   0x002
#define NL_NLM_F_ACK     0x004
#define NL_NLM_F_EXCL    0x200
#define NL_NLM_F_CREATE  0x400

#define NL_IFA_ADDRESS     1
#define NL_IFA_LOCAL       2
#define NL_IFA_LABEL       3

#define NL_IFLA_ADDRESS      1
#define NL_IFLA_IFNAME       3
#define NL_IFLA_MTU          4
#define NL_IFLA_TXQLEN      13
#define NL_IFLA_OPERSTATE   16
#define NL_IFLA_LINKINFO    18
#define NL_IFLA_NET_NS_PID  19
#define NL_IFLA_NET_NS_FD   28

#define NL_IFLA_INFO_KIND    1
#define NL_IFLA_INFO_DATA    2
#define NL_VETH_INFO_PEER    1

#define NL_IFF_UP        0x0001
#define NL_IFF_BROADCAST 0x0002
#define NL_IFF_LOOPBACK  0x0008
#define NL_IFF_RUNNING   0x0040
#define NL_IFF_MULTICAST 0x1000
#define NL_IFF_LOWER_UP  0x10000

#define NL_ARPHRD_ETHER      1
#define NL_ARPHRD_LOOPBACK 772

#define NL_IF_OPER_DOWN      2
#define NL_IF_OPER_UP        6

/* Not in abi.h -- these two are the ones a control plane needs and nothing
 * else here had a use for. Linux numbers, so a Linux binary's strerror agrees. */
#define NL_EOPNOTSUPP       95
#define NL_EAFNOSUPPORT     97

#define NL_ALIGN(n)  (((n) + 3u) & ~3u)
#define NL_IFNAMSIZ  16

struct nl_msghdr {                 /* struct nlmsghdr */
    uint32_t len;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint32_t pid;
};
/* struct ifaddrmsg (8 bytes) */
struct nl_ifaddrmsg {
    uint8_t  family, prefixlen, flags, scope;
    uint32_t index;
};
/* struct ifinfomsg (16 bytes) */
struct nl_ifinfomsg {
    uint8_t  family, pad;
    uint16_t type;
    int32_t  index;
    uint32_t flags, change;
};

/* ---- the message BUILDER (moved verbatim from socket.c) ---------------- */

struct nl_build {
    uint8_t *buf;
    size_t   cap;
    size_t   off;
    /* Offset of the message header currently being built, so its length can be
     * patched once every attribute has been appended. */
    size_t   msg_start;
    /* Set once anything did not fit. Every later append is then a no-op --
     * without this a truncated begin() would leave msg_start pointing at the
     * PREVIOUS message and end() would overwrite its length, corrupting a
     * message that was already complete. Netlink has no partial messages. */
    bool     full;
};

static void nlb_msg_begin(struct nl_build *b, uint16_t type, uint16_t flags,
                          uint32_t seq, uint32_t pid,
                          const void *body, size_t body_len) {
    if (b->full) return;
    if (b->off + sizeof(struct nl_msghdr) + NL_ALIGN(body_len) > b->cap) {
        b->full = true;
        return;
    }
    b->msg_start = b->off;
    struct nl_msghdr h = { 0 };
    h.type = type; h.flags = flags; h.seq = seq; h.pid = pid;
    memcpy(b->buf + b->off, &h, sizeof h);
    b->off += sizeof h;
    if (body_len) memcpy(b->buf + b->off, body, body_len);
    /* Pad the fixed body out to a 4-byte boundary; attributes start aligned. */
    memset(b->buf + b->off + body_len, 0, NL_ALIGN(body_len) - body_len);
    b->off += NL_ALIGN(body_len);
}

static void nlb_attr(struct nl_build *b, uint16_t type,
                     const void *val, size_t len) {
    if (b->full) return;
    size_t need = 4u + NL_ALIGN(len);
    if (b->off + need > b->cap) { b->full = true; return; }
    uint16_t rta_len = (uint16_t)(4u + len);          /* excludes the padding */
    memcpy(b->buf + b->off,     &rta_len, 2);
    memcpy(b->buf + b->off + 2, &type,    2);
    if (len) memcpy(b->buf + b->off + 4, val, len);
    memset(b->buf + b->off + 4 + len, 0, NL_ALIGN(len) - len);
    b->off += need;
}

static void nlb_msg_end(struct nl_build *b) {
    if (b->full) return;
    if (b->msg_start + sizeof(struct nl_msghdr) > b->cap) return;
    uint32_t total = (uint32_t)(b->off - b->msg_start);
    memcpy(b->buf + b->msg_start, &total, 4);         /* patch nlmsg_len */
}

/* ---- the message WALKER, which did not exist before -------------------- */

/* NLA_TYPE_MASK: the top two bits of rta_type are NLA_F_NESTED and
 * NLA_F_NET_BYTEORDER, not part of the type. iproute2 does not set either for
 * rtnetlink today, but a walker that compares the raw value would silently stop
 * finding attributes the day it does -- and the failure would look like "the
 * kernel ignored my argument". */
#define NLA_TYPE_MASK 0x3FFFu

static bool nla_find(const void *buf, size_t len, uint16_t want,
                     const void **val, size_t *vlen) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off + 4 <= len) {
        uint16_t rl, ty;
        memcpy(&rl, p + off,     2);
        memcpy(&ty, p + off + 2, 2);
        /* A malformed length must STOP the walk, not skip to a guessed offset:
         * advancing by anything after a bad header means reading attacker-
         * chosen bytes as an attribute header. */
        if (rl < 4 || off + rl > len) return false;
        if ((ty & NLA_TYPE_MASK) == want) {
            if (val)  *val  = p + off + 4;
            if (vlen) *vlen = (size_t)rl - 4u;
            return true;
        }
        off += NL_ALIGN(rl);
    }
    return false;
}

/* Copy a netlink string attribute out. Senders disagree about the trailing NUL
 * (iproute2 and busybox both send strlen+1; some send strlen), and the
 * attribute is padded to 4 bytes on top of that -- so trailing NULs are
 * stripped rather than trusted, and an over-long name is REFUSED rather than
 * truncated. A truncated interface name would name a different interface. */
static bool nla_str(const void *v, size_t vl, char *dst, size_t cap) {
    const char *s = (const char *)v;
    if (!s) return false;
    while (vl && s[vl - 1] == '\0') vl--;
    if (vl == 0 || vl >= cap) return false;
    memcpy(dst, s, vl);
    dst[vl] = '\0';
    return true;
}

/* Number of leading 1-bits in a network-order netmask (255.255.255.0 -> 24). */
static uint8_t nl_prefix_len(uint32_t mask_be) {
    uint32_t m = ntohl(mask_be);
    uint8_t n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}
static uint32_t nl_mask_from_prefix(uint8_t plen) {
    if (plen == 0)  return 0;               /* a 32-bit shift is UB, not 0 */
    if (plen > 32)  plen = 32;
    return htonl(0xFFFFFFFFu << (32 - plen));
}

/* ---- DUMPS: what is actually there, per namespace ---------------------- */

static void rtnl_link_msg(struct nl_build *b, void *ns, struct net_dev *d,
                          uint32_t seq, uint32_t pid) {
    bool up      = net_ns_dev_is_up(ns, d);
    bool carrier = d->link_up ? d->link_up(d) : true;

    struct nl_ifinfomsg ii = { 0 };
    ii.type  = NL_ARPHRD_ETHER;
    ii.index = net_ns_dev_index(ns, d);
    /* IFF_UP is the ADMIN state and IFF_RUNNING|IFF_LOWER_UP is the CARRIER --
     * two different questions, and reporting one for both is how a link with a
     * dead peer would still read as usable. A veth's carrier is "my peer
     * exists", which is exactly Linux's rule for it. */
    ii.flags = (up ? NL_IFF_UP : 0u) | NL_IFF_BROADCAST | NL_IFF_MULTICAST |
               ((up && carrier) ? (NL_IFF_RUNNING | NL_IFF_LOWER_UP) : 0u);
    nlb_msg_begin(b, NL_RTM_NEWLINK, NL_NLM_F_MULTI, seq, pid, &ii, sizeof ii);
    nlb_attr(b, NL_IFLA_IFNAME, d->name, strlen(d->name) + 1);
    nlb_attr(b, NL_IFLA_ADDRESS, d->mac, ETH_ADDR_LEN);
    { uint32_t mtu = ETH_MTU;   nlb_attr(b, NL_IFLA_MTU, &mtu, 4); }
    { uint32_t qlen = 1000;     nlb_attr(b, NL_IFLA_TXQLEN, &qlen, 4); }
    { uint8_t st = (up && carrier) ? NL_IF_OPER_UP : NL_IF_OPER_DOWN;
      nlb_attr(b, NL_IFLA_OPERSTATE, &st, 1); }
    nlb_msg_end(b);
}

static void rtnl_dump_links(struct nl_build *b, void *ns,
                            uint32_t seq, uint32_t pid) {
    if (!ns) {
        /* THE INITIAL NAMESPACE'S TWO MESSAGES, UNCHANGED. Do not "tidy" these
         * into the loop below: chrome's tracker and glibc's check_pf both read
         * this exact reply, and eth0's index 2 is what its addresses point at.
         * Loopback is reported because AddressTrackerLinux expects to see it
         * and discards it itself; a NON-initial namespace gets none, because
         * this stack has no loopback to report. */
        struct nl_ifinfomsg lo = { 0 };
        lo.type  = NL_ARPHRD_LOOPBACK;
        lo.index = 1;
        lo.flags = NL_IFF_UP | NL_IFF_LOOPBACK | NL_IFF_RUNNING |
                   NL_IFF_LOWER_UP;
        nlb_msg_begin(b, NL_RTM_NEWLINK, NL_NLM_F_MULTI, seq, pid,
                      &lo, sizeof lo);
        nlb_attr(b, NL_IFLA_IFNAME, "lo", 3);
        nlb_msg_end(b);

        /* eth0 -- the one that must read as ONLINE. */
        struct nl_ifinfomsg eth = { 0 };
        eth.type  = NL_ARPHRD_ETHER;
        eth.index = 2;
        eth.flags = NL_IFF_UP | NL_IFF_BROADCAST | NL_IFF_MULTICAST |
                    (net_is_up() ? (NL_IFF_RUNNING | NL_IFF_LOWER_UP) : 0);
        nlb_msg_begin(b, NL_RTM_NEWLINK, NL_NLM_F_MULTI, seq, pid,
                      &eth, sizeof eth);
        nlb_attr(b, NL_IFLA_IFNAME,  "eth0", 5);
        nlb_attr(b, NL_IFLA_ADDRESS, g_my_mac, ETH_ADDR_LEN);
        nlb_msg_end(b);
    }

    /* ...and then whatever the control plane created. In the initial namespace
     * that list is empty until someone runs `ip link add`, so the reply above
     * is still the whole answer and stays byte-for-byte what it was. */
    size_t n = net_ns_dev_count(ns);
    for (size_t i = 0; i < n; i++) {
        struct net_dev *d = net_ns_dev_at(ns, i);
        if (d && d->name) rtnl_link_msg(b, ns, d, seq, pid);
    }
}

static void rtnl_addr_msg(struct nl_build *b, void *ns, struct net_dev *d,
                          uint32_t seq, uint32_t pid) {
    uint32_t ip = net_ns_dev_ip(ns, d);
    if (!ip) return;                    /* an unaddressed interface has no entry */
    struct nl_ifaddrmsg ia = { 0 };
    ia.family    = 2;                                  /* AF_INET */
    ia.prefixlen = nl_prefix_len(net_ns_dev_netmask(ns, d));
    ia.scope     = 0;                                  /* RT_SCOPE_UNIVERSE */
    ia.index     = (uint32_t)net_ns_dev_index(ns, d);
    nlb_msg_begin(b, NL_RTM_NEWADDR, NL_NLM_F_MULTI, seq, pid, &ia, sizeof ia);
    nlb_attr(b, NL_IFA_ADDRESS, &ip, 4);
    nlb_attr(b, NL_IFA_LOCAL,   &ip, 4);
    nlb_attr(b, NL_IFA_LABEL,   d->name, strlen(d->name) + 1);
    nlb_msg_end(b);
}

static void rtnl_dump_addrs(struct nl_build *b, void *ns,
                            uint32_t seq, uint32_t pid) {
    if (!ns) {
        /* One IPv4 address on eth0 (index 2). Loopback is deliberately
         * omitted: the tracker discards loopback/unspecified addresses. */
        if (g_my_ip) {
            struct nl_ifaddrmsg ia = { 0 };
            ia.family    = 2;                     /* AF_INET */
            ia.prefixlen = nl_prefix_len(g_my_netmask);
            ia.scope     = 0;                     /* RT_SCOPE_UNIVERSE */
            ia.index     = 2;
            nlb_msg_begin(b, NL_RTM_NEWADDR, NL_NLM_F_MULTI, seq, pid,
                          &ia, sizeof ia);
            nlb_attr(b, NL_IFA_ADDRESS, &g_my_ip, 4);
            nlb_attr(b, NL_IFA_LOCAL,   &g_my_ip, 4);
            nlb_attr(b, NL_IFA_LABEL,   "eth0", 5);
            nlb_msg_end(b);
        }
    }
    size_t n = net_ns_dev_count(ns);
    for (size_t i = 0; i < n; i++) {
        struct net_dev *d = net_ns_dev_at(ns, i);
        if (d && d->name) rtnl_addr_msg(b, ns, d, seq, pid);
    }
}

/* ---- COMMANDS ---------------------------------------------------------- */

/* Resolve the interface a command names: by index if it carries one, else by
 * IFLA_IFNAME. Both, because `ip link add`-style creates carry a name and no
 * index while `ip link set` carries an index and no name. */
/* The initial namespace's lo (1) and eth0 (2) come from net.c, not from a
 * namespace device list -- so every lookup below misses them. They EXIST, they
 * just cannot be commanded from here, and saying "no such device" about an
 * interface `ip link show` has just listed sends the reader hunting the wrong
 * bug. Distinguish the two so the errno is truthful. */
static bool rtnl_is_synth(void *ns, int index) {
    return !ns && (index == 1 || index == 2);
}

static struct net_dev *rtnl_pick_dev(void *ns, int index,
                                     const void *attrs, size_t alen) {
    if (index > 0) return net_ns_dev_by_index(ns, index);
    const void *v; size_t vl;
    char name[NL_IFNAMSIZ];
    if (nla_find(attrs, alen, NL_IFLA_IFNAME, &v, &vl) &&
        nla_str(v, vl, name, sizeof name))
        return net_ns_find_dev(ns, name);
    return 0;
}

/* RTM_SETLINK, and RTM_NEWLINK when it carries an index (which is how busybox's
 * `ip link set DEV netns PID` arrives -- it sends RTM_NEWLINK, not SETLINK). */
static int rtnl_setlink(void *ns, const struct nl_ifinfomsg *ii,
                        const void *attrs, size_t alen) {
    struct net_dev *d = rtnl_pick_dev(ns, ii->index, attrs, alen);
    if (!d) return rtnl_is_synth(ns, ii->index) ? -NL_EOPNOTSUPP : -ABI_ENODEV;

    const void *v; size_t vl;

    /* ifi_change is the MASK of which flag bits this request is about. Applying
     * ifi_flags without consulting it would silently clear every flag the
     * caller did not mention. */
    if (ii->change & NL_IFF_UP) {
        if (!net_ns_set_dev_up(ns, d, (ii->flags & NL_IFF_UP) != 0))
            return -ABI_ENODEV;
    }

    if (nla_find(attrs, alen, NL_IFLA_NET_NS_PID, &v, &vl) && vl >= 4) {
        int vpid = 0;
        memcpy(&vpid, v, 4);
        /* The pid is in the CALLER's pid namespace, so it has to be translated
         * before it means a process. Skipping this would make `ip link set X
         * netns 1` inside a container move the device to whatever host process
         * happens to be pid 1. */
        int kpid = pid_knr(vpid);
        struct proc *t = kpid ? proc_lookup(kpid) : 0;
        if (!t) return -ABI_ESRCH;
        if (!netns_veth_move_ns(d, t->net_ns)) return -ABI_EINVAL;
        return 0;
    }
    if (nla_find(attrs, alen, NL_IFLA_NET_NS_FD, &v, &vl))
        return -NL_EOPNOTSUPP;   /* /proc/PID/ns/net fds are not resolvable here */

    /* An MTU or an address this kernel cannot honour is REFUSED, not accepted
     * and dropped: a caller that asked for a 1280-byte MTU and got silence
     * would build on a guarantee that does not exist. */
    if (nla_find(attrs, alen, NL_IFLA_MTU, &v, &vl))     return -NL_EOPNOTSUPP;
    if (nla_find(attrs, alen, NL_IFLA_ADDRESS, &v, &vl)) return -NL_EOPNOTSUPP;
    return 0;
}

static int rtnl_newlink(void *ns, const void *body, size_t len) {
    if (len < sizeof(struct nl_ifinfomsg)) return -ABI_EINVAL;
    struct nl_ifinfomsg ii;
    memcpy(&ii, body, sizeof ii);
    const void *attrs = (const uint8_t *)body + sizeof ii;
    size_t      alen  = len - sizeof ii;

    /* A non-zero index means "change this existing link", not "create". */
    if (ii.index > 0) return rtnl_setlink(ns, &ii, attrs, alen);

    const void *v; size_t vl;
    char name[NL_IFNAMSIZ];
    if (!nla_find(attrs, alen, NL_IFLA_IFNAME, &v, &vl) ||
        !nla_str(v, vl, name, sizeof name))
        return -ABI_EINVAL;

    const void *li; size_t lil;
    if (!nla_find(attrs, alen, NL_IFLA_LINKINFO, &li, &lil))
        return -ABI_EINVAL;                       /* iproute2: "need 'type'" */
    const void *k; size_t kl;
    char kind[16];
    if (!nla_find(li, lil, NL_IFLA_INFO_KIND, &k, &kl) ||
        !nla_str(k, kl, kind, sizeof kind))
        return -ABI_EINVAL;
    /* veth is the ONE kind this kernel has. Every other kind is refused by
     * name rather than created as an empty shell -- `ip link add ... type
     * bridge` returning success and producing a device that forwards nothing
     * is precisely the half-answer that makes a control plane worse than none. */
    if (strcmp(kind, "veth") != 0) return -NL_EOPNOTSUPP;

    /* IFLA_INFO_DATA -> VETH_INFO_PEER -> { ifinfomsg, IFLA_IFNAME }. Optional:
     * without it Linux auto-names the peer, and busybox's `ip` cannot send it. */
    char peer[NL_IFNAMSIZ];
    peer[0] = '\0';
    const void *idata; size_t idl;
    if (nla_find(li, lil, NL_IFLA_INFO_DATA, &idata, &idl)) {
        const void *pd; size_t pdl;
        if (nla_find(idata, idl, NL_VETH_INFO_PEER, &pd, &pdl) &&
            pdl >= sizeof(struct nl_ifinfomsg)) {
            const void *pn; size_t pnl;
            if (nla_find((const uint8_t *)pd + sizeof(struct nl_ifinfomsg),
                         pdl - sizeof(struct nl_ifinfomsg),
                         NL_IFLA_IFNAME, &pn, &pnl))
                (void)nla_str(pn, pnl, peer, sizeof peer);
        }
    }

    if (net_ns_find_dev(ns, name)) return -ABI_EEXIST;
    if (peer[0] && net_ns_find_dev(ns, peer)) return -ABI_EEXIST;
    if (netns_veth_create(ns, name, peer[0] ? peer : 0, 0, 0) != 0)
        return -ABI_ENOSPC;      /* the namespace or the veth pool is full */
    return 0;
}

static int rtnl_dellink(void *ns, const void *body, size_t len) {
    if (len < sizeof(struct nl_ifinfomsg)) return -ABI_EINVAL;
    struct nl_ifinfomsg ii;
    memcpy(&ii, body, sizeof ii);
    struct net_dev *d = rtnl_pick_dev(ns, ii.index,
                                      (const uint8_t *)body + sizeof ii,
                                      len - sizeof ii);
    if (!d) return rtnl_is_synth(ns, ii.index) ? -NL_EOPNOTSUPP : -ABI_ENODEV;
    /* Deleting the host's real NIC is not a thing this kernel can undo, and
     * net.c has no unregister -- refused rather than half-done. */
    if (!netns_veth_delete(d)) return -NL_EOPNOTSUPP;
    return 0;
}

static int rtnl_newaddr(void *ns, const void *body, size_t len, bool del) {
    if (len < sizeof(struct nl_ifaddrmsg)) return -ABI_EINVAL;
    struct nl_ifaddrmsg ia;
    memcpy(&ia, body, sizeof ia);
    const void *attrs = (const uint8_t *)body + sizeof ia;
    size_t      alen  = len - sizeof ia;

    /* AF_INET only. AF_INET6 is refused by family rather than accepted and
     * dropped, because a v6 address that reports success and never appears in
     * a dump is a lie a dual-stack caller cannot detect. */
    if (ia.family != 2) return -NL_EAFNOSUPPORT;

    struct net_dev *d = net_ns_dev_by_index(ns, (int)ia.index);
    if (!d) return rtnl_is_synth(ns, (int)ia.index) ? -NL_EOPNOTSUPP
                                                    : -ABI_ENODEV;

    if (del) {
        if (!net_ns_set_dev_addr(ns, d, 0, 0, 0)) return -ABI_ENODEV;
        return 0;
    }

    const void *v; size_t vl;
    uint32_t ip = 0;
    if ((nla_find(attrs, alen, NL_IFA_LOCAL,   &v, &vl) && vl >= 4) ||
        (nla_find(attrs, alen, NL_IFA_ADDRESS, &v, &vl) && vl >= 4))
        memcpy(&ip, v, 4);
    if (!ip) return -ABI_EINVAL;

    if (!net_ns_set_dev_addr(ns, d, ip, nl_mask_from_prefix(ia.prefixlen), 0))
        return -ABI_ENODEV;
    return 0;
}

/* ---- the request loop -------------------------------------------------- */

static void rtnl_ack(struct nl_build *b, const struct nl_msghdr *rq,
                     uint32_t pid, int err) {
    /* struct nlmsgerr is { int error; struct nlmsghdr msg; } and readers CHECK
     * that length: busybox's rtnl_talk prints "ERROR truncated" and fails the
     * command if the payload is under 20 bytes. Echoing the request header back
     * is what Linux does and what makes the 20 bytes real rather than padding. */
    struct { int32_t error; struct nl_msghdr req; } e;
    e.error = err;                       /* 0 == success, else NEGATIVE errno */
    e.req   = *rq;
    nlb_msg_begin(b, NL_NLMSG_ERROR, 0, rq->seq, pid, &e, sizeof e);
    nlb_msg_end(b);
}

size_t rtnl_process(void *ns, uint32_t nl_pid, bool privileged,
                    const void *kbuf, size_t n, uint8_t *out, size_t cap) {
    struct nl_build b = { out, cap, 0, 0, false };

    size_t off = 0;
    while (off + sizeof(struct nl_msghdr) <= n) {
        struct nl_msghdr rq;
        memcpy(&rq, (const uint8_t *)kbuf + off, sizeof rq);
        if (rq.len < sizeof(struct nl_msghdr) || off + rq.len > n) break;
        const void *body = (const uint8_t *)kbuf + off + sizeof rq;
        size_t body_len  = rq.len - sizeof rq;

        switch (rq.type) {
        case NL_RTM_GETLINK:
            rtnl_dump_links(&b, ns, rq.seq, nl_pid);
            goto done_msg;
        case NL_RTM_GETADDR:
            rtnl_dump_addrs(&b, ns, rq.seq, nl_pid);
            goto done_msg;

        case NL_RTM_NEWLINK:
        case NL_RTM_SETLINK:
        case NL_RTM_DELLINK:
        case NL_RTM_NEWADDR:
        case NL_RTM_DELADDR: {
            int err;
            /* CAP_NET_ADMIN. The DECISION is the caller's, made at the syscall
             * boundary where the caller's credentials exist -- this file is
             * reached from kernel context too (rtnl_selftest), where
             * current_proc() is a boot task with an empty capability set and
             * asking userns_capable() here answered EPERM for a caller that has
             * no credentials to check. The DUMPS are deliberately outside the
             * check: chrome's sandboxed renderer reads them with no privilege
             * at all, and requiring one would take the network away from it. */
            if (!privileged) {
                err = -ABI_EPERM;
            } else if (rq.type == NL_RTM_NEWLINK) {
                err = rtnl_newlink(ns, body, body_len);
            } else if (rq.type == NL_RTM_SETLINK) {
                if (body_len < sizeof(struct nl_ifinfomsg)) err = -ABI_EINVAL;
                else {
                    struct nl_ifinfomsg ii;
                    memcpy(&ii, body, sizeof ii);
                    err = rtnl_setlink(ns, &ii,
                                       (const uint8_t *)body + sizeof ii,
                                       body_len - sizeof ii);
                }
            } else if (rq.type == NL_RTM_DELLINK) {
                err = rtnl_dellink(ns, body, body_len);
            } else {
                err = rtnl_newaddr(ns, body, body_len,
                                   rq.type == NL_RTM_DELADDR);
            }
            /* Linux answers a command with NLMSG_ERROR when the caller asked
             * for an ACK, and ALWAYS when it failed. `ip` sets NLM_F_ACK on
             * every command it does not want a body back from, and hangs
             * waiting if the ACK never comes. */
            if (err != 0 || (rq.flags & NL_NLM_F_ACK))
                rtnl_ack(&b, &rq, nl_pid, err);
            break;
        }

        default:
        done_msg:
            /* Every dump terminates with NLMSG_DONE; its body is an int error
             * code, which for a successful dump is 0. Unknown request types
             * (RTM_GETROUTE, RTM_GETNEIGH, ...) reach here too and get an empty
             * dump, which is what they always got and is a valid answer. */
            { int32_t done = 0;
              nlb_msg_begin(&b, NL_NLMSG_DONE, NL_NLM_F_MULTI,
                            rq.seq, nl_pid, &done, sizeof done);
              nlb_msg_end(&b); }
            break;
        }

        off += NL_ALIGN(rq.len);
    }
    return b.off;
}

/* ---- THE SIOC* ioctls, which netlink does NOT replace --------------------
 *
 * FOUND BY THE THIRD-PARTY WITNESS, not by reasoning: busybox's
 * `ip link add name bb0 type veth` worked over netlink, and the very next
 * command died with `ip: can't find device 'bb0'`. Its `ip addr add` resolves
 * an interface NAME to an INDEX through ll_name_to_index(), which consults a
 * cache that only the LISTING commands populate and otherwise falls straight
 * through to libc's if_nametoindex() -- and that is an ioctl (SIOCGIFINDEX),
 * not netlink at all. A control plane with a perfect netlink surface is
 * therefore still unusable for `ip addr add ... dev NAME`.
 *
 * This is exactly what a tool we did not write is for. A hand-rolled test
 * would have gone on resolving indices from its own dump and never noticed.
 *
 * Every value here is READ FROM THE SAME PLACE THE DUMP READS IT, so the two
 * surfaces cannot drift: an interface that reports index 3 to `ip link show`
 * reports 3 to SIOCGIFINDEX. The only WRITER is SIOCSIFFLAGS, and it maps onto
 * the same enforced admin state that RTM_SETLINK sets. */

#define SIOCGIFNAME     0x8910
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCGIFNETMASK  0x891b
#define SIOCGIFMTU      0x8921
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933
#define SIOCGIFTXQLEN   0x8942

/* struct ifreq: char ifr_name[16] then a union. The union's members this needs
 * all live at offset 16, so the union's total size never has to be modelled --
 * only its first bytes are ever read or written. */
#define IFR_NAME_OFF     0
#define IFR_UNION_OFF   16
#define IFR_MIN_LEN     32

/* The initial namespace's two synthetic interfaces, answered from net.c's state
 * so ioctl and the dump cannot disagree. */
static bool rtnl_init_ns_if(const char *name, int *index, uint32_t *flags,
                            uint32_t *ip, uint32_t *mask, const uint8_t **mac) {
    if (strcmp(name, "lo") == 0) {
        *index = 1;
        *flags = NL_IFF_UP | NL_IFF_LOOPBACK | NL_IFF_RUNNING | NL_IFF_LOWER_UP;
        *ip = 0; *mask = 0; *mac = g_eth_zero;
        return true;
    }
    if (strcmp(name, "eth0") == 0) {
        *index = 2;
        *flags = NL_IFF_UP | NL_IFF_BROADCAST | NL_IFF_MULTICAST |
                 (net_is_up() ? (NL_IFF_RUNNING | NL_IFF_LOWER_UP) : 0u);
        *ip = g_my_ip; *mask = g_my_netmask; *mac = g_my_mac;
        return true;
    }
    return false;
}

long rtnl_sock_ioctl(void *ns, bool privileged, unsigned long rq,
                     unsigned long uarg) {
    switch (rq) {
    case SIOCGIFNAME: case SIOCGIFFLAGS: case SIOCSIFFLAGS:
    case SIOCGIFADDR: case SIOCGIFNETMASK: case SIOCGIFMTU:
    case SIOCGIFHWADDR: case SIOCGIFINDEX: case SIOCGIFTXQLEN:
        break;
    default:
        /* ENOTTY, not EINVAL: "this fd does not implement that request" is
         * what a caller probes for, and several of them fall back to a
         * different mechanism on ENOTTY. */
        return -ABI_ENOTTY;
    }
    if (!uarg) return -ABI_EFAULT;

    uint8_t ifr[IFR_MIN_LEN];
    if (copy_from_user(ifr, (const void *)(uintptr_t)uarg, sizeof ifr) != 0)
        return -ABI_EFAULT;
    char name[NL_IFNAMSIZ + 1];
    memcpy(name, ifr + IFR_NAME_OFF, NL_IFNAMSIZ);
    name[NL_IFNAMSIZ] = '\0';

    int index = 0;
    uint32_t flags = 0, ip = 0, mask = 0;
    const uint8_t *mac = g_eth_zero;
    struct net_dev *d = 0;
    bool synth = false;

    if (rq == SIOCGIFNAME) {
        /* The one request that names an interface by INDEX rather than by
         * name -- ifr_ifindex is the input and ifr_name the output. */
        int want = 0;
        memcpy(&want, ifr + IFR_UNION_OFF, 4);
        const char *nm = 0;
        if (!ns && want == 1) nm = "lo";
        else if (!ns && want == 2) nm = "eth0";
        else { d = net_ns_dev_by_index(ns, want); nm = d ? d->name : 0; }
        if (!nm) return -ABI_ENODEV;
        memset(ifr + IFR_NAME_OFF, 0, NL_IFNAMSIZ);
        size_t l = strlen(nm);
        if (l > NL_IFNAMSIZ - 1) l = NL_IFNAMSIZ - 1;
        memcpy(ifr + IFR_NAME_OFF, nm, l);
        if (copy_to_user((void *)(uintptr_t)uarg, ifr, sizeof ifr) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    if (!name[0]) return -ABI_EINVAL;
    d = net_ns_find_dev(ns, name);
    if (d) {
        index = net_ns_dev_index(ns, d);
        flags = (net_ns_dev_is_up(ns, d) ? NL_IFF_UP : 0u) |
                NL_IFF_BROADCAST | NL_IFF_MULTICAST |
                ((net_ns_dev_is_up(ns, d) &&
                  (!d->link_up || d->link_up(d))) ?
                     (NL_IFF_RUNNING | NL_IFF_LOWER_UP) : 0u);
        ip   = net_ns_dev_ip(ns, d);
        mask = net_ns_dev_netmask(ns, d);
        mac  = d->mac;
    } else if (!ns && rtnl_init_ns_if(name, &index, &flags, &ip, &mask, &mac)) {
        synth = true;
    } else {
        return -ABI_ENODEV;
    }

    if (rq == SIOCSIFFLAGS) {
        if (!privileged) return -ABI_EPERM;
        /* net.c owns lo and eth0. Accepting a flag change for them and doing
         * nothing would report an administrative power this kernel does not
         * have over its own NIC. */
        if (synth) return -NL_EOPNOTSUPP;
        int16_t want = 0;
        memcpy(&want, ifr + IFR_UNION_OFF, 2);
        if (!net_ns_set_dev_up(ns, d, (want & NL_IFF_UP) != 0))
            return -ABI_ENODEV;
        return 0;
    }

    memset(ifr + IFR_UNION_OFF, 0, sizeof ifr - IFR_UNION_OFF);
    switch (rq) {
    case SIOCGIFINDEX:
        memcpy(ifr + IFR_UNION_OFF, &index, 4);
        break;
    case SIOCGIFFLAGS: {
        int16_t f16 = (int16_t)(flags & 0xFFFFu);   /* ifr_flags is a short */
        memcpy(ifr + IFR_UNION_OFF, &f16, 2);
        break;
    }
    case SIOCGIFMTU: {
        int mtu = ETH_MTU;
        memcpy(ifr + IFR_UNION_OFF, &mtu, 4);
        break;
    }
    case SIOCGIFTXQLEN: {
        int qlen = 1000;                            /* same value the dump gives */
        memcpy(ifr + IFR_UNION_OFF, &qlen, 4);
        break;
    }
    case SIOCGIFHWADDR: {
        uint16_t fam = NL_ARPHRD_ETHER;             /* sa_family, then sa_data */
        if (synth && index == 1) fam = NL_ARPHRD_LOOPBACK;
        memcpy(ifr + IFR_UNION_OFF, &fam, 2);
        memcpy(ifr + IFR_UNION_OFF + 2, mac, ETH_ADDR_LEN);
        break;
    }
    case SIOCGIFADDR:
    case SIOCGIFNETMASK: {
        /* struct sockaddr_in: family, port, addr. */
        uint16_t fam = 2;                           /* AF_INET */
        uint32_t v = (rq == SIOCGIFADDR) ? ip : mask;
        if (!v) return -ABI_ENODEV;   /* Linux: EADDRNOTAVAIL-ish; not zero */
        memcpy(ifr + IFR_UNION_OFF, &fam, 2);
        memcpy(ifr + IFR_UNION_OFF + 4, &v, 4);
        break;
    }
    default:
        return -ABI_ENOTTY;
    }
    if (copy_to_user((void *)(uintptr_t)uarg, ifr, sizeof ifr) != 0)
        return -ABI_EFAULT;
    return 0;
}

#ifdef LXNETLINK_BOOT
/* =======================================================================
 * THE KERNEL-SIDE HALF OF THE GATE
 *
 * /bin/linux-netlink drives this same code from userspace over a real socket
 * and asserts what a caller can see. Two things it CANNOT see are checked here
 * instead, and they are the two that matter most:
 *
 *   1. THE INITIAL NAMESPACE'S REPLY. Chromium's AddressTrackerLinux and
 *      glibc's check_pf read it, and both fail SILENTLY and CATASTROPHICALLY
 *      when it changes -- chrome decides the machine is offline, getaddrinfo
 *      returns nothing at all. A userspace test can see the parsed result; only
 *      here can the exact byte count and message order be pinned. The lengths
 *      below are arithmetic, not observation: 16-byte header + aligned body +
 *      4-byte attribute headers.
 *
 *   2. WHETHER A NETLINK-CREATED PAIR ACTUALLY CARRIES A FRAME. `ip link add`
 *      returning 0 and a dump listing the device both pass on a control plane
 *      that has produced two names and nothing else. Only the ARP round trip
 *      proves a real device came out the other end -- the same assertion cut 2
 *      used, and it is the one that caught a green test resolving to the HOST
 *      NIC's MAC.
 * ======================================================================= */

struct hnl { uint8_t b[512]; size_t len; };

static void hnl_init(struct hnl *r, uint16_t type, uint16_t flags,
                     const void *body, size_t bl) {
    memset(r, 0, sizeof *r);
    struct nl_msghdr h = { 0 };
    h.type = type; h.flags = (uint16_t)(NL_NLM_F_REQUEST | flags); h.seq = 7;
    memcpy(r->b, &h, sizeof h);
    r->len = sizeof h;
    if (bl) memcpy(r->b + r->len, body, bl);
    r->len += NL_ALIGN(bl);
}
static void hnl_attr(struct hnl *r, uint16_t t, const void *v, size_t vl) {
    uint16_t rl = (uint16_t)(4 + vl);
    memcpy(r->b + r->len,     &rl, 2);
    memcpy(r->b + r->len + 2, &t,  2);
    if (vl) memcpy(r->b + r->len + 4, v, vl);
    r->len += 4 + NL_ALIGN(vl);
}
static size_t hnl_nest(struct hnl *r, uint16_t t) {
    size_t at = r->len; hnl_attr(r, t, 0, 0); return at;
}
static void hnl_nest_end(struct hnl *r, size_t at) {
    uint16_t rl = (uint16_t)(r->len - at);
    memcpy(r->b + at, &rl, 2);
}
static size_t hnl_go(struct hnl *r, void *ns, uint8_t *out, size_t cap) {
    uint32_t total = (uint32_t)r->len;
    memcpy(r->b, &total, 4);
    return rtnl_process(ns, 4242, true, r->b, r->len, out, cap);
}
/* Same request, unprivileged -- so the refusal is exercised on a call that
 * would otherwise SUCCEED. A permission check tested against a request that
 * fails for another reason proves nothing. */
static size_t hnl_go_unpriv(struct hnl *r, void *ns, uint8_t *out, size_t cap) {
    uint32_t total = (uint32_t)r->len;
    memcpy(r->b, &total, 4);
    return rtnl_process(ns, 4242, false, r->b, r->len, out, cap);
}

/* Walk a reply. Returns the number of messages and hands each one to `fn`. */
struct hmsg { uint16_t type; const uint8_t *body; size_t blen; };
static int hnl_msgs(const uint8_t *buf, size_t n, struct hmsg *out, int max) {
    int c = 0; size_t off = 0;
    while (off + sizeof(struct nl_msghdr) <= n) {
        struct nl_msghdr h;
        memcpy(&h, buf + off, sizeof h);
        if (h.len < sizeof h || off + h.len > n) break;
        if (c < max) { out[c].type = h.type;
                       out[c].body = buf + off + sizeof h;
                       out[c].blen = h.len - sizeof h; }
        c++;
        off += NL_ALIGN(h.len);
    }
    return c;
}
static bool hnl_name_is(const struct hmsg *m, size_t fixed, const char *want) {
    const void *v; size_t vl;
    if (m->blen < fixed) return false;
    if (!nla_find(m->body + fixed, m->blen - fixed, NL_IFLA_IFNAME, &v, &vl))
        return false;
    char nm[NL_IFNAMSIZ];
    return nla_str(v, vl, nm, sizeof nm) && strcmp(nm, want) == 0;
}
static int hnl_ack(const uint8_t *buf, size_t n) {
    struct hmsg m[8];
    int c = hnl_msgs(buf, n, m, 8);
    for (int i = 0; i < c; i++)
        if (m[i].type == NL_NLMSG_ERROR && m[i].blen >= 4) {
            int32_t e; memcpy(&e, m[i].body, 4);
            return e <= 0 ? -e : e;              /* positive errno, 0 == ok */
        }
    return -1;                                   /* no ACK at all */
}

void rtnl_selftest(void) {
    enum { CAP = 3072 };
    static uint8_t out[CAP];
    struct hnl r;
    struct hmsg m[8];
    int pass = 0, total = 8;

    kprintf("[LXNETLINK] ==== the rtnetlink control plane ====\n");

    /* ---- 1. the INITIAL namespace's link dump, unchanged ---------------- */
    {
        struct nl_ifinfomsg ii = { 0 };
        hnl_init(&r, NL_RTM_GETLINK, 0x300 /* DUMP */, &ii, sizeof ii);
        size_t n = hnl_go(&r, 0, out, CAP);
        int c = hnl_msgs(out, n, m, 8);
        bool ok = (n == 116 && c == 3);
        /* The carrier bits MIRROR net_is_up() rather than assuming it. Chrome's
         * requirement is that an ONLINE link reports IFF_RUNNING|IFF_LOWER_UP;
         * hardcoding them would turn "DHCP has not finished" into a spurious
         * control-plane failure, and dropping the check entirely would stop
         * testing the thing chrome actually depends on. */
        uint32_t want_eth = NL_IFF_UP | NL_IFF_BROADCAST | NL_IFF_MULTICAST |
                            (net_is_up() ? (NL_IFF_RUNNING | NL_IFF_LOWER_UP)
                                         : 0u);
        if (ok) {
            struct nl_ifinfomsg lo, eth;
            memcpy(&lo,  m[0].body, sizeof lo);
            memcpy(&eth, m[1].body, sizeof eth);
            ok = m[0].type == NL_RTM_NEWLINK && lo.index == 1 &&
                 lo.flags == (NL_IFF_UP | NL_IFF_LOOPBACK | NL_IFF_RUNNING |
                              NL_IFF_LOWER_UP) &&
                 hnl_name_is(&m[0], sizeof lo, "lo") &&
                 m[1].type == NL_RTM_NEWLINK && eth.index == 2 &&
                 eth.flags == want_eth &&
                 hnl_name_is(&m[1], sizeof eth, "eth0") &&
                 m[2].type == NL_NLMSG_DONE;
        }
        if (ok) { pass++;
            kprintf("[LXNETLINK]   ok   initial ns GETLINK unchanged: 116 B, "
                    "lo(1)+eth0(2)+DONE, eth0 flags 0x%x (net_is_up=%d)\n",
                    want_eth, (int)net_is_up()); }
        else
            kprintf("[LXNETLINK]   FAIL initial ns GETLINK: %lu B, %d msgs, "
                    "want 116 B / 3 msgs / eth0 flags 0x%x (net_is_up=%d) -- "
                    "chrome reads this reply\n",
                    (unsigned long)n, c, want_eth, (int)net_is_up());
    }

    /* ---- 2. the INITIAL namespace's address dump, unchanged ------------- */
    {
        struct nl_ifaddrmsg ia = { 0 };
        ia.family = 2;
        hnl_init(&r, NL_RTM_GETADDR, 0x300, &ia, sizeof ia);
        size_t n = hnl_go(&r, 0, out, CAP);
        int c = hnl_msgs(out, n, m, 8);
        bool ok = false;
        if (g_my_ip && n == 72 && c == 2 && m[0].type == NL_RTM_NEWADDR) {
            struct nl_ifaddrmsg a;
            memcpy(&a, m[0].body, sizeof a);
            const void *v; size_t vl; uint32_t ip = 0;
            if (nla_find(m[0].body + sizeof a, m[0].blen - sizeof a,
                         NL_IFA_LOCAL, &v, &vl) && vl >= 4) memcpy(&ip, v, 4);
            ok = a.index == 2 && ip == g_my_ip && m[1].type == NL_NLMSG_DONE;
        }
        if (ok) { pass++;
            kprintf("[LXNETLINK]   ok   initial ns GETADDR unchanged: 72 B, "
                    "eth0(2) carries g_my_ip\n"); }
        else if (!g_my_ip)
            /* Not a control-plane failure but still a RED: this subtest cannot
             * check the reply chrome depends on when there is no address to
             * report, and a check that quietly passes when it could not run is
             * the failure shape this arc keeps deleting. */
            kprintf("[LXNETLINK]   FAIL initial ns GETADDR: g_my_ip is 0 -- "
                    "the host never got an address, so this subtest could not "
                    "run (it is NOT being counted as a pass)\n");
        else
            kprintf("[LXNETLINK]   FAIL initial ns GETADDR: %lu B, %d msgs, "
                    "g_my_ip=0x%08x (want 72 B, 2 msgs)\n",
                    (unsigned long)n, c, g_my_ip);
    }

    /* ---- 3. an EMPTY namespace reports nothing -------------------------- */
    void *ns = net_ns_create();
    {
        struct nl_ifinfomsg ii = { 0 };
        hnl_init(&r, NL_RTM_GETLINK, 0x300, &ii, sizeof ii);
        size_t n = hnl_go(&r, ns, out, CAP);
        int c = hnl_msgs(out, n, m, 8);
        /* 20 bytes == a lone NLMSG_DONE. The OLD code answered 116 here: the
         * host's lo and eth0, for a namespace that has no interface and cannot
         * send a byte. That is the fabrication this cut removes. */
        bool ok = (n == 20 && c == 1 && m[0].type == NL_NLMSG_DONE);
        if (ok) { pass++;
            kprintf("[LXNETLINK]   ok   empty ns dumps NOTHING (20 B = DONE "
                    "only), not the host's lo+eth0\n"); }
        else
            kprintf("[LXNETLINK]   FAIL empty ns dump: %lu B, %d msgs "
                    "(want 20 B, DONE only)\n", (unsigned long)n, c);
    }

    /* ---- 3b. an UNPRIVILEGED command is refused ------------------------- */
    {
        struct nl_ifinfomsg ii = { 0 };
        hnl_init(&r, NL_RTM_NEWLINK,
                 NL_NLM_F_CREATE | NL_NLM_F_EXCL | NL_NLM_F_ACK,
                 &ii, sizeof ii);
        hnl_attr(&r, NL_IFLA_IFNAME, "np0", 4);
        size_t li = hnl_nest(&r, NL_IFLA_LINKINFO);
        hnl_attr(&r, NL_IFLA_INFO_KIND, "veth", 5);
        hnl_nest_end(&r, li);
        int err = hnl_ack(out, hnl_go_unpriv(&r, ns, out, CAP));
        size_t made = net_ns_dev_count(ns);
        /* EPERM specifically, AND nothing created. "It failed" would be
         * satisfied by a malformed request; a device that appeared anyway
         * would mean the check ran after the work. */
        if (err == ABI_EPERM && made == 0) { pass++;
            kprintf("[LXNETLINK]   ok   an unprivileged RTM_NEWLINK is EPERM "
                    "and creates nothing\n"); }
        else
            kprintf("[LXNETLINK]   FAIL unprivileged RTM_NEWLINK: err=%d "
                    "devs=%lu (want 1/EPERM and 0)\n",
                    err, (unsigned long)made);
    }

    /* ---- 4. RTM_NEWLINK makes a pair that CARRIES A FRAME --------------- */
    struct net_dev *da = 0, *db = 0;
    int idx_a = 0, idx_b = 0;
    void *ns2 = net_ns_create();
    {
        struct nl_ifinfomsg ii = { 0 };
        hnl_init(&r, NL_RTM_NEWLINK,
                 NL_NLM_F_CREATE | NL_NLM_F_EXCL | NL_NLM_F_ACK,
                 &ii, sizeof ii);
        hnl_attr(&r, NL_IFLA_IFNAME, "k0", 3);
        size_t li = hnl_nest(&r, NL_IFLA_LINKINFO);
        hnl_attr(&r, NL_IFLA_INFO_KIND, "veth", 5);
        {   /* the full iproute2 shape: INFO_DATA -> VETH_INFO_PEER ->
             * { ifinfomsg, IFLA_IFNAME }. busybox cannot send this, so the
             * userspace witness exercises the auto-named path and this one
             * exercises the explicit one. */
            size_t id = hnl_nest(&r, NL_IFLA_INFO_DATA);
            size_t pr = hnl_nest(&r, NL_VETH_INFO_PEER);
            struct nl_ifinfomsg pi = { 0 };
            memcpy(r.b + r.len, &pi, sizeof pi);
            r.len += sizeof pi;
            hnl_attr(&r, NL_IFLA_IFNAME, "k1", 3);
            hnl_nest_end(&r, pr);
            hnl_nest_end(&r, id);
        }
        hnl_nest_end(&r, li);
        size_t n = hnl_go(&r, ns, out, CAP);
        int ack = hnl_ack(out, n);

        da = net_ns_find_dev(ns, "k0");
        db = net_ns_find_dev(ns, "k1");

        /* THE PEER MOVES TO ITS OWN NAMESPACE BEFORE THE FRAME TEST, and the
         * reason is a real limitation worth stating rather than designing
         * around silently: arp_recv answers only when the target IP equals
         * net_my_ip(), and net_my_ip() is the RECEIVING NAMESPACE'S PRIMARY
         * device's address. Cut 3b made the stored address per-device, but the
         * RECEIVE path still has no per-device context (eth_recv does not carry
         * the device), so a namespace holding two interfaces answers ARP for
         * the first one's address only. Both ends in one namespace would
         * therefore never complete a round trip -- not because RTM_NEWLINK
         * built a bad device, which is what this subtest is about.
         *
         * One end per namespace is also the real container topology, and it is
         * the configuration cut 2 proved. The move itself is a kernel call
         * here; its NETLINK form (IFLA_NET_NS_PID) needs a live process to name
         * and is covered by /bin/linux-netlink bit7. */
        if (db) (void)netns_veth_move_ns(db, ns2);
        idx_a = net_ns_dev_index(ns, da);
        idx_b = net_ns_dev_index(ns2, db);

        /* Address BOTH ends over netlink too -- if RTM_NEWADDR wrote to the
         * wrong interface the ARP below would answer for the wrong address. */
        uint32_t ip_a = 10u | (55u << 8) | (0u << 16) | (1u << 24);
        uint32_t ip_b = 10u | (55u << 8) | (0u << 16) | (2u << 24);
        int aa = -1, ab = -1;
        if (idx_a && idx_b) {
            struct nl_ifaddrmsg ia = { 0 };
            ia.family = 2; ia.prefixlen = 24; ia.index = (uint32_t)idx_a;
            hnl_init(&r, NL_RTM_NEWADDR, NL_NLM_F_CREATE | NL_NLM_F_ACK,
                     &ia, sizeof ia);
            hnl_attr(&r, NL_IFA_LOCAL, &ip_a, 4);
            aa = hnl_ack(out, hnl_go(&r, ns, out, CAP));

            ia.index = (uint32_t)idx_b;
            hnl_init(&r, NL_RTM_NEWADDR, NL_NLM_F_CREATE | NL_NLM_F_ACK,
                     &ia, sizeof ia);
            hnl_attr(&r, NL_IFA_LOCAL, &ip_b, 4);
            ab = hnl_ack(out, hnl_go(&r, ns2, out, CAP));
        }

        /* THE assertion: a real frame, answered by the peer's OWN MAC.
         * Comparing against db->mac rather than a literal is deliberate --
         * the failure this guards against is the reply carrying the HOST
         * e1000's MAC, and db->mac can never be that. */
        uint8_t mac[ETH_ADDR_LEN];
        bool resolved = false;
        if (da && db) {
            struct net_ctx prev = net_ctx_enter(ns, da);
            arp_request(ip_b);
            net_ctx_leave(prev);
            resolved = arp_resolve(ip_b, mac) &&
                       memcmp(mac, db->mac, ETH_ADDR_LEN) == 0;
        }
        uint32_t atx = 0, arx = 0, btx = 0, brx = 0;
        netns_veth_dev_stats(da, &atx, &arx);
        netns_veth_dev_stats(db, &btx, &brx);

        if (ack == 0 && aa == 0 && ab == 0 && da && db && da != db &&
            idx_a > 0 && idx_b > 0 &&
            net_ns_dev_ip(ns, da) == ip_a && net_ns_dev_ip(ns2, db) == ip_b &&
            atx >= 1 && brx >= 1 && btx >= 1 && arx >= 1 && resolved) {
            pass++;
            kprintf("[LXNETLINK]   ok   RTM_NEWLINK made a REAL pair: k0(%d) "
                    "<-> k1(%d), frames a tx=%u rx=%u b tx=%u rx=%u, ARP "
                    "answered by k1's own MAC\n",
                    idx_a, idx_b, atx, arx, btx, brx);
        } else {
            kprintf("[LXNETLINK]   FAIL RTM_NEWLINK: ack=%d addr=%d/%d "
                    "k0=%p(%d) k1=%p(%d) frames a %u/%u b %u/%u arp=%d\n",
                    ack, aa, ab, (void *)da, idx_a, (void *)db, idx_b,
                    atx, arx, btx, brx, (int)resolved);
        }
    }

    /* ---- 5. RTM_SETLINK down actually STOPS frames ---------------------- */
    if (da && db && idx_a) {
        uint32_t rx0 = 0, rx1 = 0, rx2 = 0, dummy = 0;
        uint32_t ip_b = 10u | (55u << 8) | (0u << 16) | (2u << 24);

        struct nl_ifinfomsg ii = { 0 };
        ii.index = idx_a; ii.change = NL_IFF_UP; ii.flags = 0;   /* DOWN */
        hnl_init(&r, NL_RTM_SETLINK, NL_NLM_F_ACK, &ii, sizeof ii);
        int down = hnl_ack(out, hnl_go(&r, ns, out, CAP));

        netns_veth_dev_stats(db, &dummy, &rx0);
        { struct net_ctx prev = net_ctx_enter(ns, da); arp_request(ip_b);
          net_ctx_leave(prev); }
        netns_veth_dev_stats(db, &dummy, &rx1);

        ii.flags = NL_IFF_UP;                                    /* UP again */
        hnl_init(&r, NL_RTM_SETLINK, NL_NLM_F_ACK, &ii, sizeof ii);
        int up = hnl_ack(out, hnl_go(&r, ns, out, CAP));
        { struct net_ctx prev = net_ctx_enter(ns, da); arp_request(ip_b);
          net_ctx_leave(prev); }
        netns_veth_dev_stats(db, &dummy, &rx2);

        /* The counter is the evidence, not the flag. A stored-and-ignored
         * IFF_UP would report "down" in the dump and keep forwarding, which is
         * the failure mode a flag check cannot see. */
        bool flag_ok = false;
        {
            struct nl_ifinfomsg gi = { 0 };
            ii.flags = 0; ii.change = NL_IFF_UP;
            hnl_init(&r, NL_RTM_SETLINK, NL_NLM_F_ACK, &ii, sizeof ii);
            (void)hnl_go(&r, ns, out, CAP);            /* down once more */
            hnl_init(&r, NL_RTM_GETLINK, 0x300, &gi, sizeof gi);
            size_t n = hnl_go(&r, ns, out, CAP);
            int c = hnl_msgs(out, n, m, 8);
            for (int i = 0; i < c; i++) {
                if (m[i].type != NL_RTM_NEWLINK) continue;
                struct nl_ifinfomsg li;
                memcpy(&li, m[i].body, sizeof li);
                if (li.index == idx_a) flag_ok = !(li.flags & NL_IFF_UP);
            }
            ii.flags = NL_IFF_UP;
            hnl_init(&r, NL_RTM_SETLINK, NL_NLM_F_ACK, &ii, sizeof ii);
            (void)hnl_go(&r, ns, out, CAP);
        }

        if (down == 0 && up == 0 && rx1 == rx0 && rx2 > rx1 && flag_ok) {
            pass++;
            kprintf("[LXNETLINK]   ok   `link set down` is ENFORCED: peer rx "
                    "%u -> %u while down, %u after up, IFF_UP cleared in the "
                    "dump\n", rx0, rx1, rx2);
        } else {
            kprintf("[LXNETLINK]   FAIL set down/up: ack=%d/%d rx %u->%u->%u "
                    "flag_cleared=%d (want no rx while down)\n",
                    down, up, rx0, rx1, rx2, (int)flag_ok);
        }
    } else {
        kprintf("[LXNETLINK]   FAIL set down/up: no pair to test\n");
    }

    /* ---- 5b. REAL IP TRAFFIC, AND WHOSE ADDRESS IT CARRIES ---------------
     *
     * THE SUBTEST THAT WOULD HAVE CAUGHT CUT 5's BUG. Every check before this
     * one is satisfied by a stack that builds interfaces correctly and then
     * stamps the HOST's address on every packet they carry -- which is exactly
     * what ip_send_frame did (`h->src_ip = g_my_ip`, unconditionally). Cut 2
     * proved ARP crosses the boundary; nothing had ever sent IP across it, so
     * nothing could see this.
     *
     * The assertion is the SOURCE ADDRESS THE RECEIVER RECORDS, not that a
     * datagram arrived. It fails in two independent ways on a partial fix:
     *   - old code: delivered, but src reads as the host's 10.0.2.15;
     *   - src_ip fixed but the UDP pseudo-header still computed over g_my_ip:
     *     the checksum mismatches and udp_recv drops it, so nothing arrives.
     * Only fixing both layers passes -- the same two-layer shape as cut 2's
     * ethernet-header-vs-ARP-payload MAC. */
    if (da && db && idx_a && idx_b) {
        uint32_t ip_a = 10u | (55u << 8) | (0u << 16) | (1u << 24);
        uint32_t ip_b = 10u | (55u << 8) | (0u << 16) | (2u << 24);
        struct sock *rx = sock_alloc(SOCK_KIND_UDP);
        bool ok = false;
        uint32_t got_src = 0, got_cnt = 0;
        if (rx) {
            /* Receive AS the peer namespace. sock_alloc latches the CREATOR's
             * namespace and the creator here is a boot task in the initial
             * one, so this is the harness standing in for a process that lives
             * in ns2. */
            rx->net_ns = ns2;
            if (sock_bind(rx, htons(7777)) == 0) {
                static const char msg[] = "cut5";
                struct net_ctx prev = net_ctx_enter(ns, da);
                (void)udp_send(htons(7778), ip_b, htons(7777),
                               msg, sizeof msg - 1);
                net_ctx_leave(prev);
                got_cnt = rx->count;
                if (got_cnt) got_src = rx->dgrams[rx->tail].src_ip;
                ok = (got_cnt == 1 && got_src == ip_a);
            }
            sock_close(rx);
        }
        if (ok) {
            pass++;
            kprintf("[LXNETLINK]   ok   UDP crossed the boundary and carries "
                    "the SENDER's address (10.55.0.1), not the host's\n");
        } else {
            kprintf("[LXNETLINK]   FAIL cross-ns UDP: %u datagram(s), src="
                    "0x%08x (want 1 and 0x%08x; host g_my_ip=0x%08x -- if src "
                    "equals THAT, ip_send is still stamping the host)\n",
                    got_cnt, got_src, ip_a, g_my_ip);
        }
    } else {
        kprintf("[LXNETLINK]   FAIL cross-ns UDP: no addressed pair to use\n");
    }

    /* ---- 6. RTM_DELLINK removes BOTH ends ------------------------------- */
    if (da && idx_a) {
        struct nl_ifinfomsg ii = { 0 };
        ii.index = idx_a;
        hnl_init(&r, NL_RTM_DELLINK, NL_NLM_F_ACK, &ii, sizeof ii);
        int del = hnl_ack(out, hnl_go(&r, ns, out, CAP));
        size_t left = net_ns_dev_count(ns);
        /* The PEER is in another namespace by now, and deleting one end of a
         * pair destroys both -- so ns2 must lose its interface as well. A
         * delete that only cleaned up the namespace it was issued in would
         * leave a device that can never carry a frame and still lists. */
        size_t left2 = net_ns_dev_count(ns2);

        struct nl_ifinfomsg gi = { 0 };
        hnl_init(&r, NL_RTM_GETLINK, 0x300, &gi, sizeof gi);
        size_t n = hnl_go(&r, ns, out, CAP);

        if (del == 0 && left == 0 && left2 == 0 && n == 20) {
            pass++;
            kprintf("[LXNETLINK]   ok   RTM_DELLINK removed BOTH ends, across "
                    "namespaces; the dump is empty again\n");
        } else {
            kprintf("[LXNETLINK]   FAIL RTM_DELLINK: ack=%d devs left=%lu/%lu "
                    "dump=%lu B (want 0, 0, 0, 20)\n",
                    del, (unsigned long)left, (unsigned long)left2,
                    (unsigned long)n);
        }
    } else {
        kprintf("[LXNETLINK]   FAIL RTM_DELLINK: no pair to delete\n");
    }

    net_ns_put(ns2);
    net_ns_put(ns);
    kprintf("[LXNETLINK] VERDICT: %s subtests=%d/%d\n",
            (pass == total) ? "PASS" : "FAIL", pass, total);
}
#endif /* LXNETLINK_BOOT */
