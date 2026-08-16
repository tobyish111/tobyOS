/* linux-netlink -- Phase 3 slice 12 (cut 4) acceptance test: THE CONTROL PLANE.
 *
 * ===========================================================================
 * WHAT THIS TEST IS FOR, AND THE TRAP IT IS SHAPED AROUND
 *
 * Before cut 4, rtnetlink here answered RTM_GETLINK and RTM_GETADDR with a
 * HARDCODED `lo` + `eth0` -- the same two interfaces no matter which network
 * namespace asked -- and accepted no commands at all. The named danger in
 * making it accept commands was this: "a dump that still says lo and eth0
 * after `ip link add` succeeded would be worse than no control plane at all."
 *
 * So the shape of every check here is DO SOMETHING, THEN RE-READ IT THROUGH
 * THE DUMP. Nothing passes on an ACK alone. An ACK is the kernel saying it
 * agreed; the dump is the kernel saying what is actually there, and those are
 * exactly the two things that were allowed to disagree.
 *
 * bit1 is the one that proves the dump became namespace-aware at all: a fresh
 * network namespace must report ZERO links. The old code reported the host's
 * lo + eth0 there, which reads perfectly and is a complete fabrication -- the
 * namespace has no interfaces and cannot send a packet.
 *
 * bit5 asserts REFUSALS BY SPECIFIC ERRNO, never "it failed". `ip link add
 * type bridge` failing with EINVAL, EPERM or a timeout would all "fail", and
 * only EOPNOTSUPP means "this kernel knows what you asked for and does not
 * have it". Same reason cut 1's test asserted ENETUNREACH rather than "connect
 * failed".
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  the INITIAL namespace still dumps lo + eth0 (the control: chrome's
 *         AddressTrackerLinux and glibc's check_pf read this exact reply)
 *   bit1  a fresh CLONE_NEWNET namespace dumps NOTHING -- no invented lo
 *   bit2  RTM_NEWLINK type veth creates a REAL pair: ACK 0, and the dump then
 *         names both ends with distinct indices
 *   bit3  RTM_NEWADDR lands on the interface it NAMED -- the address comes back
 *         on that ifindex with that prefix length, and not on its peer
 *   bit4  RTM_SETLINK down/up is visible: IFF_UP disappears from the dump and
 *         comes back, carrier flags with it
 *   bit5  refusals by exact errno: unknown link type EOPNOTSUPP, duplicate
 *         name EEXIST, address on a nonexistent ifindex ENODEV
 *   bit6  RTM_DELLINK removes BOTH ends of a pair, and a bogus index is ENODEV
 *   bit7  RTM_SETLINK IFLA_NET_NS_PID MOVES an interface into another
 *         namespace: it leaves this dump and appears in the child's own
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define NS_NEWNET      0x40000000

#define AF_NL          16          /* AF_NETLINK */
#define NETLINK_ROUTE   0

#define NLMSG_ERROR     2
#define NLMSG_DONE      3
#define RTM_NEWLINK    16
#define RTM_DELLINK    17
#define RTM_GETLINK    18
#define RTM_SETLINK    19
#define RTM_NEWADDR    20
#define RTM_GETADDR    22

#define F_REQUEST   0x001
#define F_MULTI     0x002
#define F_ACK       0x004
#define F_EXCL      0x200
#define F_CREATE    0x400
#define F_ROOT      0x100
#define F_MATCH     0x200
#define F_DUMP      (F_ROOT | F_MATCH)

#define IFLA_ADDRESS      1
#define IFLA_IFNAME       3
#define IFLA_LINKINFO    18
#define IFLA_NET_NS_PID  19
#define IFLA_INFO_KIND    1
#define IFLA_INFO_DATA    2
#define VETH_INFO_PEER    1

#define IFA_ADDRESS       1
#define IFA_LOCAL         2

#define IFF_UP        0x0001
#define IFF_RUNNING   0x0040
#define IFF_LOWER_UP  0x10000

#define NLA(n) (((n) + 3u) & ~3u)

struct nlhdr {
    uint32_t len;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint32_t pid;
};
struct ifinfo {                      /* struct ifinfomsg */
    uint8_t  family, pad;
    uint16_t type;
    int32_t  index;
    uint32_t flags, change;
};
struct ifaddr {                      /* struct ifaddrmsg */
    uint8_t  family, prefixlen, flags, scope;
    uint32_t index;
};
struct sa_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    uint32_t       nl_pid;
    uint32_t       nl_groups;
};

/* ---- request building -------------------------------------------------- */

struct req { unsigned char b[1024]; size_t len; };

static uint32_t g_seq = 1000;

static void rq_init(struct req *r, uint16_t type, uint16_t flags,
                    const void *body, size_t blen) {
    memset(r, 0, sizeof *r);
    struct nlhdr h;
    memset(&h, 0, sizeof h);
    h.type = type;
    h.flags = (uint16_t)(F_REQUEST | flags);
    h.seq = ++g_seq;
    h.pid = 0;
    memcpy(r->b, &h, sizeof h);
    r->len = sizeof h;
    if (blen) memcpy(r->b + r->len, body, blen);
    r->len += NLA(blen);
}
static void rq_attr(struct req *r, uint16_t type, const void *v, size_t vl) {
    uint16_t rl = (uint16_t)(4 + vl);
    memcpy(r->b + r->len,     &rl,   2);
    memcpy(r->b + r->len + 2, &type, 2);
    if (vl) memcpy(r->b + r->len + 4, v, vl);
    r->len += 4 + NLA(vl);
}
/* A nested attribute's length is only known once its children are in, so the
 * header is stamped first and patched at the end. */
static size_t rq_nest(struct req *r, uint16_t type) {
    size_t at = r->len;
    rq_attr(r, type, NULL, 0);
    return at;
}
static void rq_nest_end(struct req *r, size_t at) {
    uint16_t rl = (uint16_t)(r->len - at);
    memcpy(r->b + at, &rl, 2);
}
static void rq_send(int fd, struct req *r) {
    uint32_t total = (uint32_t)r->len;
    memcpy(r->b, &total, 4);                 /* patch nlmsg_len */
    (void)send(fd, r->b, r->len, 0);
}

static int nl_open(void) {
    int fd = socket(AF_NL, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sa_nl a;
    memset(&a, 0, sizeof a);
    a.nl_family = AF_NL;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

/* Send a command and return the errno the kernel ACKed with: 0 on success, a
 * POSITIVE errno on refusal, -1 if no NLMSG_ERROR came back at all.
 *
 * "no ACK came back" is kept DISTINCT from "the ACK said 0". `ip` hangs when an
 * ACK never arrives, so a silent success and a real success are different
 * defects and the test must be able to tell them apart. */
static int nl_cmd(int fd, struct req *r) {
    rq_send(fd, r);
    unsigned char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return -1;
    size_t off = 0;
    while (off + sizeof(struct nlhdr) <= (size_t)n) {
        struct nlhdr h;
        memcpy(&h, buf + off, sizeof h);
        if (h.len < sizeof h || off + h.len > (size_t)n) break;
        if (h.type == NLMSG_ERROR) {
            int32_t e = 0;
            if (h.len >= sizeof h + 4) memcpy(&e, buf + off + sizeof h, 4);
            return e <= 0 ? -e : e;          /* kernel sends a NEGATIVE errno */
        }
        off += NLA(h.len);
    }
    return -1;
}

/* ---- dump parsing ------------------------------------------------------ */

struct link_ent { int index; char name[24]; uint32_t flags; };

static int dump_links(int fd, struct link_ent *out, int max) {
    struct ifinfo ii;
    memset(&ii, 0, sizeof ii);
    struct req r;
    rq_init(&r, RTM_GETLINK, F_DUMP, &ii, sizeof ii);
    rq_send(fd, &r);

    unsigned char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return -1;
    int cnt = 0;
    size_t off = 0;
    while (off + sizeof(struct nlhdr) <= (size_t)n) {
        struct nlhdr h;
        memcpy(&h, buf + off, sizeof h);
        if (h.len < sizeof h || off + h.len > (size_t)n) break;
        if (h.type == NLMSG_DONE) break;
        if (h.type == RTM_NEWLINK && h.len >= sizeof h + sizeof(struct ifinfo)) {
            struct ifinfo m;
            memcpy(&m, buf + off + sizeof h, sizeof m);
            struct link_ent e;
            memset(&e, 0, sizeof e);
            e.index = m.index;
            e.flags = m.flags;
            size_t ao = off + sizeof h + sizeof m, aend = off + h.len;
            while (ao + 4 <= aend) {
                uint16_t rl, ty;
                memcpy(&rl, buf + ao,     2);
                memcpy(&ty, buf + ao + 2, 2);
                if (rl < 4 || ao + rl > aend) break;
                if (ty == IFLA_IFNAME) {
                    size_t vl = rl - 4;
                    if (vl >= sizeof e.name) vl = sizeof e.name - 1;
                    memcpy(e.name, buf + ao + 4, vl);
                    e.name[vl] = '\0';
                }
                ao += NLA(rl);
            }
            if (cnt < max) out[cnt] = e;
            cnt++;
        }
        off += NLA(h.len);
    }
    return cnt;
}

struct addr_ent { int index; uint8_t plen; uint32_t ip; };

static int dump_addrs(int fd, struct addr_ent *out, int max) {
    struct ifaddr ia;
    memset(&ia, 0, sizeof ia);
    ia.family = 2;                      /* AF_INET */
    struct req r;
    rq_init(&r, RTM_GETADDR, F_DUMP, &ia, sizeof ia);
    rq_send(fd, &r);

    unsigned char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return -1;
    int cnt = 0;
    size_t off = 0;
    while (off + sizeof(struct nlhdr) <= (size_t)n) {
        struct nlhdr h;
        memcpy(&h, buf + off, sizeof h);
        if (h.len < sizeof h || off + h.len > (size_t)n) break;
        if (h.type == NLMSG_DONE) break;
        if (h.type == RTM_NEWADDR && h.len >= sizeof h + sizeof(struct ifaddr)) {
            struct ifaddr m;
            memcpy(&m, buf + off + sizeof h, sizeof m);
            struct addr_ent e;
            memset(&e, 0, sizeof e);
            e.index = (int)m.index;
            e.plen  = m.prefixlen;
            size_t ao = off + sizeof h + sizeof m, aend = off + h.len;
            while (ao + 4 <= aend) {
                uint16_t rl, ty;
                memcpy(&rl, buf + ao,     2);
                memcpy(&ty, buf + ao + 2, 2);
                if (rl < 4 || ao + rl > aend) break;
                if ((ty == IFA_LOCAL || ty == IFA_ADDRESS) && rl >= 8)
                    memcpy(&e.ip, buf + ao + 4, 4);
                ao += NLA(rl);
            }
            if (cnt < max) out[cnt] = e;
            cnt++;
        }
        off += NLA(h.len);
    }
    return cnt;
}

static struct link_ent *find_link(struct link_ent *v, int n, const char *nm) {
    for (int i = 0; i < n; i++) if (strcmp(v[i].name, nm) == 0) return &v[i];
    return NULL;
}

/* ---- commands, spelled the way iproute2 spells them -------------------- */

static int add_veth(int fd, const char *name, const char *peer) {
    struct ifinfo ii;
    memset(&ii, 0, sizeof ii);
    struct req r;
    rq_init(&r, RTM_NEWLINK, F_CREATE | F_EXCL | F_ACK, &ii, sizeof ii);
    rq_attr(&r, IFLA_IFNAME, name, strlen(name) + 1);
    size_t li = rq_nest(&r, IFLA_LINKINFO);
    rq_attr(&r, IFLA_INFO_KIND, "veth", 5);
    if (peer) {
        /* IFLA_INFO_DATA -> VETH_INFO_PEER -> { ifinfomsg, IFLA_IFNAME }.
         * The peer block carries a whole ifinfomsg of its own before its
         * attributes -- that layout is what iproute2's link_veth.c emits. */
        size_t id = rq_nest(&r, IFLA_INFO_DATA);
        size_t pr = rq_nest(&r, VETH_INFO_PEER);
        struct ifinfo pi;
        memset(&pi, 0, sizeof pi);
        memcpy(r.b + r.len, &pi, sizeof pi);
        r.len += sizeof pi;
        rq_attr(&r, IFLA_IFNAME, peer, strlen(peer) + 1);
        rq_nest_end(&r, pr);
        rq_nest_end(&r, id);
    }
    rq_nest_end(&r, li);
    return nl_cmd(fd, &r);
}

static int add_kind(int fd, const char *name, const char *kind) {
    struct ifinfo ii;
    memset(&ii, 0, sizeof ii);
    struct req r;
    rq_init(&r, RTM_NEWLINK, F_CREATE | F_EXCL | F_ACK, &ii, sizeof ii);
    rq_attr(&r, IFLA_IFNAME, name, strlen(name) + 1);
    size_t li = rq_nest(&r, IFLA_LINKINFO);
    rq_attr(&r, IFLA_INFO_KIND, kind, strlen(kind) + 1);
    rq_nest_end(&r, li);
    return nl_cmd(fd, &r);
}

static int add_addr(int fd, int index, uint32_t ip_be, uint8_t plen) {
    struct ifaddr ia;
    memset(&ia, 0, sizeof ia);
    ia.family = 2;
    ia.prefixlen = plen;
    ia.index = (uint32_t)index;
    struct req r;
    rq_init(&r, RTM_NEWADDR, F_CREATE | F_EXCL | F_ACK, &ia, sizeof ia);
    rq_attr(&r, IFA_LOCAL,   &ip_be, 4);
    rq_attr(&r, IFA_ADDRESS, &ip_be, 4);
    return nl_cmd(fd, &r);
}

static int set_updown(int fd, int index, int up) {
    struct ifinfo ii;
    memset(&ii, 0, sizeof ii);
    ii.index  = index;
    ii.change = IFF_UP;                 /* the MASK: only this bit is meant */
    ii.flags  = up ? IFF_UP : 0;
    struct req r;
    rq_init(&r, RTM_SETLINK, F_ACK, &ii, sizeof ii);
    return nl_cmd(fd, &r);
}

static int del_link(int fd, int index) {
    struct ifinfo ii;
    memset(&ii, 0, sizeof ii);
    ii.index = index;
    struct req r;
    rq_init(&r, RTM_DELLINK, F_ACK, &ii, sizeof ii);
    return nl_cmd(fd, &r);
}

static int move_ns(int fd, int index, int pid) {
    struct ifinfo ii;
    memset(&ii, 0, sizeof ii);
    ii.index = index;
    struct req r;
    rq_init(&r, RTM_SETLINK, F_ACK, &ii, sizeof ii);
    rq_attr(&r, IFLA_NET_NS_PID, &pid, 4);
    return nl_cmd(fd, &r);
}

/* 10.9.0.N in network byte order, built without <arpa/inet.h>. */
static uint32_t ip4(int a, int b, int c, int d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) |
           ((uint32_t)d << 24);
}

int main(void) {
    int bits = 0;
    struct link_ent lv[16];
    struct addr_ent av[16];

    /* ---- bit0: the INITIAL namespace, unchanged ------------------------ */
    {
        int fd = nl_open();
        if (fd >= 0) {
            int n = dump_links(fd, lv, 16);
            struct link_ent *lo = find_link(lv, n, "lo");
            struct link_ent *e0 = find_link(lv, n, "eth0");
            printf("[nl] initial ns: %d links, lo=%d eth0=%d\n", n,
                   lo ? lo->index : -1, e0 ? e0->index : -1);
            /* The indices matter as much as the names: eth0 is index 2 and the
             * address dump points at index 2. If those two ever disagree,
             * chrome sees an address on an interface it has no record of. */
            if (n == 2 && lo && lo->index == 1 && e0 && e0->index == 2 &&
                (e0->flags & IFF_UP))
                bits |= 1 << 0;
            close(fd);
        }
    }

    /* ---- into our own network namespace -------------------------------- */
    if (syscall(SYS_unshare, NS_NEWNET) != 0) {
        printf("[nl] unshare(CLONE_NEWNET) failed: %s\n", strerror(errno));
        printf("[nl] BITS=0x%02x\n", bits);
        return bits;
    }

    int fd = nl_open();
    if (fd < 0) {
        printf("[nl] netlink socket in the new ns failed: %s\n",
               strerror(errno));
        printf("[nl] BITS=0x%02x\n", bits);
        return bits;
    }

    /* ---- bit1: a fresh namespace reports NOTHING ------------------------ */
    {
        int n = dump_links(fd, lv, 16);
        printf("[nl] fresh netns: %d links (want 0)\n", n);
        /* THE check that the dump became namespace-aware. The old code
         * answered lo + eth0 here, for a namespace that has no interface at
         * all and cannot send a byte. */
        if (n == 0) bits |= 1 << 1;
    }

    /* ---- bit2: RTM_NEWLINK type veth creates a real pair ---------------- */
    int c0_idx = 0, peer_idx = 0;
    char peer_name[24] = "";
    {
        int rc = add_veth(fd, "c0", NULL);       /* no peer name: auto vethN */
        int n = dump_links(fd, lv, 16);
        struct link_ent *c0 = find_link(lv, n, "c0");
        struct link_ent *pe = NULL;
        for (int i = 0; i < n && i < 16; i++)
            if (strcmp(lv[i].name, "c0") != 0) pe = &lv[i];
        printf("[nl] add veth c0: ack=%d -> %d links (%s, %s)\n", rc, n,
               n > 0 ? lv[0].name : "-", n > 1 ? lv[1].name : "-");
        if (rc == 0 && n == 2 && c0 && pe && c0->index != pe->index &&
            c0->index > 0 && pe->index > 0) {
            bits |= 1 << 2;
            c0_idx = c0->index;
            peer_idx = pe->index;
            snprintf(peer_name, sizeof peer_name, "%s", pe->name);
        }
    }

    /* ---- bit3: RTM_NEWADDR lands on the interface it named -------------- */
    if (c0_idx) {
        uint32_t want = ip4(10, 9, 0, 1);
        int rc = add_addr(fd, c0_idx, want, 24);
        int n = dump_addrs(fd, av, 16);
        int ok = (rc == 0 && n == 1 && av[0].index == c0_idx &&
                  av[0].ip == want && av[0].plen == 24);
        printf("[nl] addr 10.9.0.1/24 on if%d: ack=%d -> %d addr(s), "
               "if%d ip=0x%08x/%u\n", c0_idx, rc, n,
               n > 0 ? av[0].index : -1, n > 0 ? av[0].ip : 0,
               n > 0 ? av[0].plen : 0);
        /* Exactly ONE address: the peer must NOT have acquired it. A
         * per-namespace address would have given both ends the same one, and
         * `ip addr add` would have reported success while lying about which
         * interface it addressed. */
        if (ok) bits |= 1 << 3;
    }

    /* ---- bit4: down/up is VISIBLE in the dump --------------------------- */
    if (c0_idx) {
        int rd = set_updown(fd, c0_idx, 0);
        int n1 = dump_links(fd, lv, 16);
        struct link_ent *d = find_link(lv, n1, "c0");
        uint32_t after_down = d ? d->flags : 0xffffffffu;

        int ru = set_updown(fd, c0_idx, 1);
        int n2 = dump_links(fd, lv, 16);
        struct link_ent *u = find_link(lv, n2, "c0");
        uint32_t after_up = u ? u->flags : 0;

        printf("[nl] set down/up: ack=%d/%d flags 0x%x -> 0x%x\n",
               rd, ru, after_down, after_up);
        if (rd == 0 && ru == 0 &&
            !(after_down & IFF_UP) && !(after_down & IFF_RUNNING) &&
            (after_up & IFF_UP) && (after_up & IFF_RUNNING) &&
            (after_up & IFF_LOWER_UP))
            bits |= 1 << 4;
    }

    /* ---- bit5: refusals, by EXACT errno --------------------------------- */
    {
        /* `bridge` USED TO BE THE UNSUPPORTED KIND HERE and this assertion
         * caught its own obsolescence the moment bridges landed: the gate went
         * red with type-bridge=0 against a wanted 95. That is the assertion
         * working. It now checks the opposite -- bridge SUCCEEDS -- and picks
         * `vlan` as the kind this kernel genuinely does not have.
         *
         * Keeping both in one bit is deliberate: a supported kind and an
         * unsupported one are the two halves of "the kind is actually looked
         * at", and a kernel that ignored IFLA_INFO_KIND entirely would fail
         * one of them whichever way it guessed. */
        int e_kind = add_kind(fd, "vl0", "vlan");     /* want EOPNOTSUPP 95 */
        int e_br   = add_kind(fd, "brx", "bridge");   /* want 0 -- created */
        int n_br   = dump_links(fd, lv, 16);
        struct link_ent *bx = find_link(lv, n_br, "brx");
        /* Remove it again so the counts the later bits assert are unaffected.
         * A test that leaves state behind makes the next one's numbers depend
         * on the order they happen to run in. */
        if (bx) del_link(fd, bx->index);
        int e_dup  = add_veth(fd, "c0", NULL);        /* want EEXIST 17 */
        int e_addr = add_addr(fd, 99, ip4(10, 9, 0, 9), 24);  /* ENODEV 19 */
        printf("[nl] kinds: vlan=%d bridge=%d(listed=%d) dup-name=%d "
               "addr-if99=%d (want 95/0/1/17/19)\n",
               e_kind, e_br, bx ? 1 : 0, e_dup, e_addr);
        /* "It failed" would be satisfied by a kernel that refuses everything,
         * including the things it is supposed to do. */
        if (e_kind == 95 && e_br == 0 && bx && e_dup == 17 && e_addr == 19)
            bits |= 1 << 5;
    }

    /* ---- bit6: RTM_DELLINK removes BOTH ends ---------------------------- */
    {
        int before = dump_links(fd, lv, 16);
        int rc_add = add_veth(fd, "d0", "d1");        /* explicit peer name */
        int mid = dump_links(fd, lv, 16);
        struct link_ent *d0 = find_link(lv, mid, "d0");
        struct link_ent *d1 = find_link(lv, mid, "d1");
        int rc_del = d0 ? del_link(fd, d0->index) : -1;
        int after = dump_links(fd, lv, 16);
        int e_bogus = del_link(fd, 99);
        printf("[nl] del: %d -> %d (d0=%d d1=%d) -> %d, add=%d del=%d "
               "bogus=%d\n", before, mid, d0 ? d0->index : -1,
               d1 ? d1->index : -1, after, rc_add, rc_del, e_bogus);
        /* Both ends must go. Deleting one end of a pair and leaving the other
         * would leave a device that can never carry a frame but still lists. */
        if (rc_add == 0 && d0 && d1 && mid == before + 2 &&
            rc_del == 0 && after == before && e_bogus == 19)
            bits |= 1 << 6;
    }

    /* ---- bit7: move an interface into ANOTHER namespace ------------------
     * The container recipe: the pair is made where the tooling runs, and one
     * end is pushed into the target namespace. The child reports what IT can
     * see, so the claim is checked from inside the destination rather than
     * inferred from the sender's side. */
    if (peer_idx) {
        int to_child[2], to_parent[2];
        if (pipe(to_child) == 0 && pipe(to_parent) == 0) {
            pid_t kid = fork();
            if (kid == 0) {
                close(to_child[1]); close(to_parent[0]);
                char ok = 0;
                if (syscall(SYS_unshare, NS_NEWNET) == 0) {
                    char go = 0;
                    /* Tell the parent we are in a namespace of our own, then
                     * wait: the move has to happen while this process is alive
                     * and settled, not racing its own unshare. */
                    ok = 1; (void)write(to_parent[1], &ok, 1);
                    if (read(to_child[0], &go, 1) == 1) {
                        int cfd = nl_open();
                        struct link_ent cv[16];
                        int cn = cfd >= 0 ? dump_links(cfd, cv, 16) : -1;
                        ok = (cn == 1 && strcmp(cv[0].name, peer_name) == 0)
                             ? 2 : 3;
                        printf("[nl] child ns: %d link(s), first=%s\n",
                               cn, cn > 0 ? cv[0].name : "-");
                        (void)write(to_parent[1], &ok, 1);
                        if (cfd >= 0) close(cfd);
                    }
                } else {
                    (void)write(to_parent[1], &ok, 1);
                }
                _exit(0);
            }
            close(to_child[0]); close(to_parent[1]);
            char st = 0;
            int moved = -1, left = -1;
            if (read(to_parent[0], &st, 1) == 1 && st == 1) {
                moved = move_ns(fd, peer_idx, (int)kid);
                left = dump_links(fd, lv, 16);
                char go = 1;
                (void)write(to_child[1], &go, 1);
                st = 0;
                (void)read(to_parent[0], &st, 1);
            }
            int status = 0;
            waitpid(kid, &status, 0);
            close(to_child[1]); close(to_parent[0]);
            printf("[nl] move if%d -> pid %d: ack=%d, here=%d links, "
                   "child verdict=%d (want 0/1/2)\n",
                   peer_idx, (int)kid, moved, left, st);
            /* Both halves: gone from here AND present over there. Only one of
             * those would be satisfied by a kernel that simply deleted it. */
            if (moved == 0 && left == 1 && st == 2) bits |= 1 << 7;
        }
    }

    close(fd);
    printf("[nl] BITS=0x%02x\n", bits);
    return bits;
}
