/* net.c -- network subsystem entry points + shared helpers.
 *
 * Owns the static IP configuration globals (g_my_ip, g_gateway_ip,
 * etc.), the network-device registry, the boot-time init sequence,
 * and the periodic poll callback the kernel idle loop drives.
 *
 * Boot ordering (milestone 21):
 *   1. Drivers e1000_register() etc. add a struct pci_driver to the
 *      bus registry.
 *   2. pci_bind_drivers() walks the device list, calls each match's
 *      probe(), and the probes call net_register() on success.
 *   3. net_init() picks net_default() and copies its MAC into
 *      g_my_mac, then brings up ARP + sockets.
 *
 * Link layer is IEEE 802.3 Ethernet only (wired NICs).  The
 * eth/arp/ip/udp/tcp stack only talks to net_default(); the L2
 * driver may be e1000, e1000e, rtl8169, virtio-net, etc.
 *
 * Helpers:
 *   - net_checksum  : 16-bit one's-complement Internet checksum.
 *   - net_udp_checksum : pseudo-header checksum for UDP.
 *   - net_format_ip / mac : printable strings for the shell.
 */

#include <tobyos/net.h>
#include <tobyos/nsproxy.h>
#include <tobyos/proc.h>
#include <tobyos/arp.h>
#include <tobyos/socket.h>
#include <tobyos/dhcp.h>
#include <tobyos/ssh.h>
#include <tobyos/tcp.h>
#include <tobyos/tcp_echo.h>
#include <tobyos/tcp_shell.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/cpu.h>
#include <tobyos/klibc.h>
#include <tobyos/vfs.h>

/* ---- runtime state --------------------------------------------- */

uint8_t  g_my_mac[ETH_ADDR_LEN] = { 0 };
uint32_t g_my_ip       = 0;
uint32_t g_my_netmask  = 0;
uint32_t g_gateway_ip  = 0;
uint32_t g_my_dns_be   = 0;

const uint8_t g_eth_broadcast[ETH_ADDR_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
const uint8_t g_eth_zero[ETH_ADDR_LEN] = { 0 };

static bool g_net_up;
static enum net_status g_net_status = NET_STATUS_DOWN;
static struct net_dev *g_net_devs[NET_MAX_DEVICES];
static size_t          g_net_dev_count;
static volatile int    g_net_service_busy;
static volatile bool   g_net_boot_requested;
static volatile bool   g_net_boot_done;
static volatile unsigned g_net_boot_attempts;
static uint64_t        g_net_boot_next_tick;

/* Set once in net_init(): true iff the running IPv4 config came from DHCP
 * (not static fallback). Used by bootlog UDP upload targeting. */
static bool g_net_boot_via_dhcp;

/* ---- Slice 12 cut 2: "my address", per network namespace ----------------
 *
 * g_my_ip is the INITIAL namespace's address and stays exactly that -- DHCP
 * writes it, ifconfig reads it, nothing about that path changes. These
 * accessors add the namespaced answer on top:
 *
 *   1. an active receive context wins (we are processing a frame that arrived on
 *      a veth end, so "us" means that namespace);
 *   2. otherwise the calling process's namespace;
 *   3. otherwise g_my_ip.
 *
 * Every consumer that used to read g_my_ip directly to answer "is this mine?"
 * now calls these, which is what stops a container's stack claiming the host's
 * address (and vice versa). */
/* ---- cut 5: one resolver, three accessors ------------------------------
 *
 * The order is the whole design:
 *   1. an ACTIVE network context wins outright -- it means some code that
 *      genuinely knows (a driver, a veth end) said so. Within it, the DEVICE's
 *      own address beats the namespace's primary, which is what makes a
 *      namespace with two interfaces answer for the right one.
 *   2. otherwise the CALLING PROCESS's namespace, which is correct for a
 *      send driven by a syscall.
 *   3. otherwise the host's globals.
 *
 * (1) existing at all is the cut-5 fix: it used to be impossible to say "this
 * frame is the host's", so the host receive path fell into (2) and answered
 * with the namespace of whatever process happened to be in the syscall that
 * pumped net_poll(). */
static void *net_ctx_resolve_ns(struct net_dev **dev_out) {
    if (net_ctx_active()) {
        if (dev_out) *dev_out = net_ctx_dev();
        return net_ctx_ns();
    }
    if (dev_out) *dev_out = 0;
    struct proc *cp = current_proc();
    return cp ? cp->net_ns : 0;
}

/* "Which namespace is this code acting for?" -- the same question net_my_ip()
 * answers, without the address. Exported because the PORT SPACE has to ask it
 * too: a port is only in use, and a datagram only deliverable, within one
 * namespace. */
void *net_current_ns(void) {
    struct net_dev *d = 0;
    return net_ctx_resolve_ns(&d);
}

uint32_t net_my_ip(void) {
    struct net_dev *d = 0;
    void *ns = net_ctx_resolve_ns(&d);
    if (d) { uint32_t ip = net_ns_dev_ip(ns, d); if (ip) return ip; }
    if (ns) return net_ns_ip(ns);
    return g_my_ip;
}
/* The MAC of the interface "we" would transmit from, namespace-relative.
 *
 * Needed because the ARP PAYLOAD carries a sender hardware address, and that has
 * to be the replying device's MAC. Fixing only the ethernet header (eth_send)
 * left ARP replies from inside a namespace advertising the HOST NIC's MAC -- so
 * the peer cached "container-ip is at host-mac", which resolves fine and is
 * completely wrong. Two layers, both needed. */
const uint8_t *net_my_mac(void) {
    struct net_dev *d = 0;
    void *ns = net_ctx_resolve_ns(&d);
    if (d) return d->mac;                  /* the interface that received it */
    if (ns) {
        struct net_dev *p = net_ns_dev(ns);
        if (p) return p->mac;
    }
    return g_my_mac;
}

uint32_t net_my_netmask(void) {
    struct net_dev *d = 0;
    void *ns = net_ctx_resolve_ns(&d);
    if (d) { uint32_t m = net_ns_dev_netmask(ns, d); if (m) return m; }
    if (ns) return net_ns_netmask(ns);
    return g_my_netmask;
}

/* The next hop for anything off-subnet. ip_send used g_gateway_ip
 * unconditionally, so a container sending off its own subnet resolved the
 * HOST's gateway -- the same "one more layer still carries the global" shape
 * cut 2 found in the ARP payload, one layer further up. */
uint32_t net_my_gateway(void) {
    struct net_dev *d = 0;
    void *ns = net_ctx_resolve_ns(&d);
    if (d) { uint32_t g = net_ns_dev_gateway(ns, d); if (g) return g; }
    if (ns) return net_ns_gateway(ns);
    return g_gateway_ip;
}

bool net_is_up(void) { return g_net_up; }

bool net_boot_used_dhcp(void) { return g_net_boot_via_dhcp; }

void net_boot_request(void) {
    if (g_net_boot_done) return;
    g_net_boot_requested = true;
    g_net_boot_next_tick = 0;
}

/* Real hardware can miss the first deferred service slot while PCI/IRQ
 * setup is still settling. Keep retrying from the idle service lane instead
 * of permanently declaring boot networking done after one early miss. */
static uint64_t net_delay_ticks(unsigned ms) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t ticks = ((uint64_t)hz * (uint64_t)ms + 999u) / 1000u;
    return ticks ? ticks : 1;
}

static unsigned net_boot_retry_delay_ms(unsigned attempt) {
    if (attempt <= 2u) return 250u;
    if (attempt <= 4u) return 500u;
    if (attempt <= 8u) return 1000u;
    return 3000u;
}

/* Cap on deferred DHCP retries. Without it, a NIC whose TX is dead (e.g.
 * the PCH I217 on the EliteDesk -- RX works, TX never drains) retries DHCP
 * forever from the idle lane, each attempt failing fast and flooding the
 * log. ~10 attempts with backoff spans ~20 s -- long enough to catch a
 * slow DHCP server, after which we stay on the lease we have. */
#define NET_BOOT_MAX_DEFERRED_ATTEMPTS 10u

static bool net_boot_has_dhcp_lease(void) {
    return g_net_up && g_net_boot_via_dhcp &&
           g_net_status == NET_STATUS_DHCP_OK && g_my_ip != 0;
}

enum net_status net_status(void) { return g_net_status; }

const char *net_status_name(void) {
    switch (g_net_status) {
    case NET_STATUS_DOWN:            return "down";
    case NET_STATUS_NO_NIC:          return "no-nic";
    case NET_STATUS_DHCP_WAIT:       return "dhcp-wait";
    case NET_STATUS_DHCP_OK:         return "dhcp-ok";
    case NET_STATUS_DHCP_EMPTY:      return "dhcp-empty";
    case NET_STATUS_STATIC_FALLBACK: return "static-fallback";
    default:                         return "unknown";
    }
}

void net_status_summary(char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    char ip[16], gw[16], dns[16], mac[18];
    net_format_ip(ip, g_my_ip);
    net_format_ip(gw, g_gateway_ip);
    net_format_ip(dns, g_my_dns_be);
    net_format_mac(mac, g_my_mac);
    struct net_dev *nd = net_default();
    ksnprintf(dst, cap, "status=%s nics=%u nic=%s ip=%s gw=%s dns=%s mac=%s",
              net_status_name(), (unsigned)g_net_dev_count,
              nd && nd->name ? nd->name : "?",
              ip, gw, dns, mac);
}

void net_debug_dump(void) {
    char summary[160];
    net_status_summary(summary, sizeof(summary));
    kprintf("[net-diag] %s\n", summary);
    net_dump();
}

/* ---- net_dev registry ------------------------------------------- */

void net_register(struct net_dev *dev) {
    if (!dev) return;
    if (g_net_dev_count >= NET_MAX_DEVICES) {
        kprintf("[net] WARN: registry full, dropping '%s'\n",
                dev->name ? dev->name : "?");
        return;
    }
    g_net_devs[g_net_dev_count++] = dev;
    char mb[18];
    net_format_mac(mb, dev->mac);
    kprintf("[net] +nic %s mac=%s (slot %u)\n",
            dev->name ? dev->name : "?", mb,
            (unsigned)(g_net_dev_count - 1));
}

struct net_dev *net_default(void) {
    return g_net_dev_count ? g_net_devs[0] : 0;
}

size_t net_dev_count(void) { return g_net_dev_count; }

struct net_dev *net_dev_get(size_t idx) {
    return idx < g_net_dev_count ? g_net_devs[idx] : 0;
}

void net_dump(void) {
    kprintf("[net] %u nic(s) registered:\n", (unsigned)g_net_dev_count);
    for (size_t i = 0; i < g_net_dev_count; i++) {
        char mb[18];
        net_format_mac(mb, g_net_devs[i]->mac);
        kprintf("  [%u] %s mac=%s\n", (unsigned)i,
                g_net_devs[i]->name ? g_net_devs[i]->name : "?", mb);
    }
}

/* ---- init ------------------------------------------------------- */

/* Pre-warm one ARP entry by firing a request and draining RX for a short
 * window while watching the cache. Used for the gateway AND for the DNS
 * server when it differs from the gateway (SLIRP: same subnet, first UDP
 * would otherwise ARP-miss and drop). */
#ifndef FAST_BOOT
#define NET_ARP_WARM_MS 220u
#else
#define NET_ARP_WARM_MS 120u
#endif

static void net_warm_arp(struct net_dev *nd, uint32_t ip_be, const char *what) {
    if (!ip_be) return;
    arp_request(ip_be);
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t warm_ticks = ((uint64_t)hz * (uint64_t)NET_ARP_WARM_MS) / 1000u;
    if (warm_ticks < 1) warm_ticks = 1;
    uint64_t deadline = pit_ticks() + warm_ticks;
    uint8_t scratch[ETH_ADDR_LEN];
    while (pit_ticks() < deadline) {
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        if (arp_resolve(ip_be, scratch)) break;
        sti();
        hlt();
    }
    char ipbuf[16];
    net_format_ip(ipbuf, ip_be);
    if (arp_resolve(ip_be, scratch)) {
        char mb[18];
        net_format_mac(mb, scratch);
        kprintf("[net] %s %s -> %s (cached)\n", what, ipbuf, mb);
    } else {
        kprintf("[net] WARN: %s %s did not respond to ARP within %ums\n",
                what, ipbuf, (unsigned)NET_ARP_WARM_MS);
    }
}

static void net_warm_gateway_arp(struct net_dev *nd) {
    net_warm_arp(nd, g_gateway_ip, "gateway");
    /* Skip when DNS is the same host as the gateway (typical home router). */
    if (g_my_dns_be && g_my_dns_be != g_gateway_ip) {
        /* SLICE 132: ...and skip when DNS is OFF-SUBNET, because ARPing it is
         * asking a question no one on this link can answer. The EliteDesk's
         * DHCP lease hands out a PUBLIC resolver (ip=192.168.68.77/22, dns=
         * 209.18.47.61), so this burned the full NET_ARP_WARM_MS and then
         * printed "WARN: dns ... did not respond to ARP", which reads like
         * broken DNS. It is not: ip_send already routes off-subnet traffic via
         * the gateway (see the netmask test in ip.c), and the gateway's ARP is
         * warmed just above -- so the useful cache entry was already there.
         *
         * QEMU CANNOT SHOW THIS. SLIRP hands out 10.0.2.3, which is inside the
         * guest's own /24, so the direct ARP always succeeded and the branch
         * looked correct for the entire life of this code. Same blind spot
         * class as the xHCI packet-size bug. */
        uint32_t mask = net_my_netmask();
        if (mask && ((g_my_dns_be & mask) == (net_my_ip() & mask))) {
            net_warm_arp(nd, g_my_dns_be, "dns    ");
        } else {
            char dbuf[16];
            net_format_ip(dbuf, g_my_dns_be);
            kprintf("[net] dns     %s is off-subnet -- reached via the "
                    "gateway (no direct ARP)\n", dbuf);
        }
    }
}

/* Brief pause + RX drain between DHCP attempts (home routers / PHY
 * sometimes miss the first DISCOVER or answer late). Kept short for
 * boot time — second acquire runs quickly like dhcpcd retry. */
static void net_dhcp_retry_gap(struct net_dev *nd, unsigned ms) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t end = pit_ticks() + ((uint64_t)hz * (uint64_t)ms) / 1000u;
    if (end <= pit_ticks()) end = pit_ticks() + 1;
    while (pit_ticks() < end) {
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        sti();
        hlt();
    }
}

/* Refresh the resolver config from the live DHCP lease so a Linux binary's
 * libc (e.g. musl getaddrinfo) finds the nameserver the same way it would on
 * Linux. Best-effort: the initrd root is a read-only ramfs, so today this is a
 * silent no-op and /etc/resolv.conf is the static one shipped in the initrd
 * (SLIRP's 10.0.2.3, which DHCP also hands out); on any writable root mount it
 * rewrites the file to match the real lease. */
static void net_write_resolv_conf(uint32_t dns_be) {
    if (dns_be == 0) return;
    char dnsbuf[16];
    net_format_ip(dnsbuf, dns_be);
    char line[64];
    int n = 0;
    const char *pfx = "nameserver ";
    for (const char *p = pfx; *p && n < (int)sizeof line - 1; p++) line[n++] = *p;
    for (const char *p = dnsbuf; *p && n < (int)sizeof line - 1; p++) line[n++] = *p;
    if (n < (int)sizeof line - 1) line[n++] = '\n';
    line[n] = 0;
    int rc = vfs_write_all("/etc/resolv.conf", line, (size_t)n);
    if (rc == VFS_OK) {
        kprintf("[net] /etc/resolv.conf -> %s\n", dnsbuf);
        return;
    }
    /* SAY SO. This used to log only on success, so on a read-only root it
     * failed in complete silence -- and the consequence is invisible until a
     * browser reports ERR_NAME_NOT_RESOLVED with nothing in the kernel log to
     * connect it to.
     *
     * It matters ONLY on real hardware, which is why it survived this long:
     * the initrd ships `nameserver 10.0.2.3`, QEMU's SLIRP hands out exactly
     * 10.0.2.3, and the static file is therefore accidentally correct in every
     * QEMU boot. On a real LAN the DHCP nameserver is something else entirely
     * and every name lookup goes to an address that does not exist there.
     *
     * The fix is a writable root (ramfs has .write = 0 today; Linux's
     * initramfs is a writable tmpfs, so the read-only root is the deviation).
     * Until then this is a WARNING with the actual numbers in it, because a
     * one-line diagnosis beats a silent wrong answer. */
    static bool warned;
    if (!warned) {
        warned = true;
        kprintf("[net] WARN: cannot update /etc/resolv.conf (rc=%d, read-only "
                "root). DHCP says the nameserver is %s, but resolvers will "
                "keep using whatever the initrd shipped -- if those differ, "
                "name resolution FAILS (ERR_NAME_NOT_RESOLVED) while ping and "
                "raw IP still work.\n", rc, dnsbuf);
    }
}

/* Apply a successful DHCP lease into the kernel globals. Logged with
 * a single human-readable line so post-mortem analysis is one grep. */
static void net_apply_lease(const struct dhcp_lease *L, const char *src) {
    g_my_ip       = L->ip_be;
    g_my_netmask  = L->netmask_be ? L->netmask_be : ip4(255, 255, 255, 0);
    g_gateway_ip  = L->gateway_be;
    g_my_dns_be   = L->dns_be;

    char ipbuf[16], mskbuf[16], gwbuf[16], dnsbuf[16];
    net_format_ip(ipbuf,  g_my_ip);
    net_format_ip(mskbuf, g_my_netmask);
    net_format_ip(gwbuf,  g_gateway_ip);
    net_format_ip(dnsbuf, g_my_dns_be);
    kprintf("[net] %s lease applied: ip=%s mask=%s gw=%s dns=%s\n",
            src, ipbuf, mskbuf, gwbuf, dnsbuf);

    net_write_resolv_conf(g_my_dns_be);
}

/* Static fallback when DHCP times out.
 *
 * Default: 192.168.68.10 / 255.255.252.0 (/22) with gateway + DNS
 * 192.168.68.1 — matches a typical Deco-style LAN (e.g. desktop at
 * 192.168.68.74/22, gateway .68.1).
 *
 * For QEMU `-netdev user` / SLIRP where you rely on this path, build with
 *   -DTOBY_NET_FALLBACK_SLIRP
 * to restore 10.0.2.15 / 10.0.2.2 / 10.0.2.3. */
static void net_apply_static_fallback(void) {
    struct dhcp_lease s;
#ifdef TOBY_NET_FALLBACK_SLIRP
    s.ip_be      = ip4(10, 0, 2, 15);
    s.netmask_be = ip4(255, 255, 255, 0);
    s.gateway_be = ip4(10, 0, 2, 2);
    s.dns_be     = ip4(10, 0, 2, 3);
#else
    s.ip_be      = ip4(192, 168, 68, 10);
    s.netmask_be = ip4(255, 255, 252, 0);
    s.gateway_be = ip4(192, 168, 68, 1);
    s.dns_be     = ip4(192, 168, 68, 1);
#endif
    s.server_be  = 0;
    s.lease_secs = 0;
    net_apply_lease(&s, "static-fallback");
}

bool net_init(void) {
    struct net_dev *nd = net_default();
    if (!nd) {
        g_net_status = NET_STATUS_NO_NIC;
        kprintf("[net] no NIC registered -- networking disabled\n");
        return false;
    }

    /* Adopt the first registered NIC's MAC as our identity on the wire. */
    memcpy(g_my_mac, nd->mac, ETH_ADDR_LEN);
    char macbuf[18];
    net_format_mac(macbuf, g_my_mac);
    kprintf("[net] nic=%s mac=%s -- starting up\n",
            nd->name ? nd->name : "?", macbuf);
    {
        unsigned z = 0;
        for (int i = 0; i < ETH_ADDR_LEN; i++) z |= g_my_mac[i];
        if (z == 0)
            kprintf("[net] WARN: NIC MAC is all-zero — Ethernet src will not "
                    "match your real adapter; fix driver / Wireshark filter\n");
    }

    arp_init();
    sock_init();
    tcp_init();

    /* g_my_ip stays 0 across the DHCP handshake: ip_send and udp_send
     * stamp the source IP from g_my_ip, which is exactly what BOOTP
     * wants (src = 0.0.0.0 until step 4). ip_recv accepts any IPv4
     * destination while g_my_ip==0 so yiaddr-unicast OFFER/ACK still
     * demux; once configured, unicast + 255.255.255.255 + subnet
     * directed broadcast are accepted (see ip_dst_is_for_us). */
    g_my_ip      = 0;
    g_my_netmask = 0;
    g_gateway_ip = 0;
    g_my_dns_be  = 0;
    g_net_up     = true;          /* mark up so udp_send / arp_send work */
    g_net_status = NET_STATUS_DHCP_WAIT;

    /* HP Realtek guardrail: send the first DISCOVER immediately after
     * stack init. Do not add a driver-specific pre-DHCP link wait here;
     * that previously produced boots with no DHCP packets on the wire. */

    /* Try DHCP: DISCOVER → OFFER → REQUEST → ACK; dhcp.c uses ~70% of the
     * budget waiting for OFFER (with DISCOVER retries) and the rest for ACK. */
#ifdef FAST_BOOT
    /* Some home routers answer DHCP slowly on cold boot. */
    enum { dhcp_boot_budget_ms = 7000 };
#else
    enum { dhcp_boot_budget_ms = 6000 };
#endif
#ifdef FAST_BOOT
    enum { dhcp_retry_gap_ms = 90u };
#else
    enum { dhcp_retry_gap_ms = 120u };
#endif
    struct dhcp_lease lease;
    bool dhcp_ok = false;
    for (unsigned attempt = 1; attempt <= 3u; attempt++) {
        dhcp_ok = dhcp_acquire(dhcp_boot_budget_ms, &lease);
        if (dhcp_ok) break;
        if (attempt < 3u) {
            kprintf("[net] DHCP attempt %u failed -- retrying after short gap\n",
                    attempt);
            net_dhcp_retry_gap(nd, dhcp_retry_gap_ms);
        }
    }
    if (dhcp_ok && lease.ip_be == 0) {
        kprintf("[net] DHCP returned an empty IP lease -- ignoring\n");
        g_net_status = NET_STATUS_DHCP_EMPTY;
        dhcp_ok = false;
    }
    if (dhcp_ok) {
        g_net_boot_via_dhcp = true;
        net_apply_lease(&lease, "DHCP");
        g_net_status = NET_STATUS_DHCP_OK;
    } else {
        g_net_boot_via_dhcp = false;
#ifdef TOBY_NET_FALLBACK_SLIRP
        kprintf("[net] DHCP failed (3 attempts) -- using static SLIRP fallback (10.0.2.15/24)\n");
#else
        kprintf("[net] DHCP failed (3 attempts) -- using static fallback 192.168.68.10/22 (gw/dns .68.1)\n");
#endif
        net_apply_static_fallback();
        g_net_status = NET_STATUS_STATIC_FALLBACK;
    }

    char ipbuf[16], gwbuf[16];
    net_format_ip(ipbuf, g_my_ip);
    net_format_ip(gwbuf, g_gateway_ip);
    kprintf("[net] up: nic=%s ip=%s gw=%s mac=%s\n",
            nd->name ? nd->name : "?", ipbuf, gwbuf, macbuf);

    /* Help LAN peers (and our own ARP path) learn this MAC for our IP
     * before they must ARP-request us — improves first ping / first UDP. */
    arp_gratuitous();

    net_warm_gateway_arp(nd);

    tcp_echo_init();
    tcp_shell_init();

    return true;
}

bool net_dhcp_renew(void) {
    struct net_dev *nd = net_default();
    if (!nd) {
        kprintf("[net] dhcp renew: no NIC\n");
        return false;
    }
    /* Drop the IP for the duration of the handshake so DISCOVER goes
     * out as src=0.0.0.0 like RFC 2131 wants. We restore-or-replace
     * below depending on the outcome. */
    uint32_t prev_ip = g_my_ip, prev_msk = g_my_netmask;
    uint32_t prev_gw = g_gateway_ip, prev_dns = g_my_dns_be;
    enum net_status prev_status = g_net_status;
    bool prev_boot_via_dhcp = g_net_boot_via_dhcp;
    g_my_ip = 0; g_my_netmask = 0; g_gateway_ip = 0; g_my_dns_be = 0;
    g_net_status = NET_STATUS_DHCP_WAIT;

    struct dhcp_lease lease;
    if (!dhcp_acquire(5000, &lease)) {
        kprintf("[net] dhcp renew: failed -- restoring previous lease\n");
        g_my_ip = prev_ip; g_my_netmask = prev_msk;
        g_gateway_ip = prev_gw; g_my_dns_be = prev_dns;
        g_net_status = prev_status;
        g_net_boot_via_dhcp = prev_boot_via_dhcp;
        return false;
    }
    if (lease.ip_be == 0) {
        kprintf("[net] dhcp renew: empty IP lease -- restoring previous lease\n");
        g_my_ip = prev_ip; g_my_netmask = prev_msk;
        g_gateway_ip = prev_gw; g_my_dns_be = prev_dns;
        g_net_status = prev_status;
        g_net_boot_via_dhcp = prev_boot_via_dhcp;
        return false;
    }
    g_net_boot_via_dhcp = true;
    net_apply_lease(&lease, "DHCP-renew");
    g_net_status = NET_STATUS_DHCP_OK;
    arp_init();                       /* gateway might have changed */
    net_warm_gateway_arp(nd);
    return true;
}

void net_poll(void) {
    if (!g_net_up) return;
    /* Drain every registered NIC. With a single NIC this matches the
     * old behaviour exactly; with two it gives both a fair turn. */
    for (size_t i = 0; i < g_net_dev_count; i++) {
        struct net_dev *nd = g_net_devs[i];
        if (nd && nd->rx_drain) nd->rx_drain(nd);
    }
    tcp_echo_poll();
    tcp_shell_poll();
    /* Drive TCP timers in the background so a closing connection no
     * longer needs its owner to sit and poll it (see tcp_close_nowait). */
    tcp_service_tick();
}

void net_service_tick(void) {
    if (__atomic_exchange_n(&g_net_service_busy, 1, __ATOMIC_ACQUIRE)) {
        return;
    }

    if (g_net_boot_requested && !g_net_boot_done) {
        uint64_t now = pit_ticks();
        if (g_net_boot_next_tick && now < g_net_boot_next_tick) {
            goto poll_and_release;
        }

        g_net_boot_requested = false;
        g_net_boot_attempts++;
        bool ok;
        if (g_net_up && g_my_ip != 0 && !net_boot_has_dhcp_lease()) {
            kprintf("[net] deferred DHCP retry starting (attempt %u)\n",
                    g_net_boot_attempts);
            ok = net_dhcp_renew();
        } else {
            kprintf("[net] deferred boot bring-up starting (attempt %u)\n",
                    g_net_boot_attempts);
            ok = net_init();
        }

        if (ok && net_boot_has_dhcp_lease()) {
            g_net_boot_done = true;
            kprintf("[net] deferred boot bring-up complete\n");
            ssh_init();
        } else if (g_net_boot_attempts >= NET_BOOT_MAX_DEFERRED_ATTEMPTS) {
            /* Give up the deferred DHCP storm. Static fallback was already
             * applied in net_init(), so we have a usable lease; stop
             * retrying (and stop flooding the log) rather than spin
             * forever on a NIC that can't transmit. Logged once. */
            g_net_boot_done = true;
            if (g_net_up && g_my_ip != 0) ssh_init();
            kprintf("[net] giving up deferred DHCP after %u attempts "
                    "(NIC TX / DHCP unavailable) -- staying on current lease\n",
                    g_net_boot_attempts);
        } else {
            unsigned delay_ms = net_boot_retry_delay_ms(g_net_boot_attempts);
            g_net_boot_requested = true;
            g_net_boot_next_tick = pit_ticks() + net_delay_ticks(delay_ms);
            if (g_net_up && g_my_ip != 0) {
                ssh_init();
                kprintf("[net] DHCP lease not established yet; retrying in %u ms\n",
                        delay_ms);
            } else {
                kprintf("[net] deferred boot bring-up failed; retrying in %u ms\n",
                        delay_ms);
            }
        }
    }

poll_and_release:
    net_poll();
    ssh_poll();

    __atomic_store_n(&g_net_service_busy, 0, __ATOMIC_RELEASE);
}

/* ---- checksum --------------------------------------------------- */

uint16_t net_checksum(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p   += 2;
        len -= 2;
    }
    if (len == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    /* Convert host result back to network byte order: htons of ~sum. */
    uint16_t cs = (uint16_t)~sum;
    return htons(cs);
}

uint16_t net_l4_checksum(uint8_t proto, uint32_t src_ip_be, uint32_t dst_ip_be,
                         const void *l4_packet, size_t l4_len) {
    /* Pseudo-header (12 bytes): src(4) | dst(4) | zero(1) | proto(1) | len(2). */
    uint32_t sum = 0;
    const uint8_t *s = (const uint8_t *)&src_ip_be;
    const uint8_t *d = (const uint8_t *)&dst_ip_be;
    sum += ((uint32_t)s[0] << 8) | s[1];
    sum += ((uint32_t)s[2] << 8) | s[3];
    sum += ((uint32_t)d[0] << 8) | d[1];
    sum += ((uint32_t)d[2] << 8) | d[3];
    sum += proto;
    sum += (uint32_t)l4_len;

    /* L4 packet itself. */
    const uint8_t *p = (const uint8_t *)l4_packet;
    size_t n = l4_len;
    while (n > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p   += 2;
        n   -= 2;
    }
    if (n == 1) sum += (uint32_t)p[0] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);

    /* DO NOT rewrite a 0 result to 0xFFFF here: that's a SEND-side
     * rule (RFC 768 reserves a wire value of 0 for "no checksum") and
     * the rewrite would break verification because, on receive, sum
     * = 0xFFFF and ~sum = 0 is the correct "valid" outcome. The send
     * path in udp.c does the rewrite at the call site instead.
     * (TCP doesn't have the "0 means no checksum" rule, so it's
     * always safe.) */
    return htons((uint16_t)~sum);
}

uint16_t net_udp_checksum(uint32_t src_ip_be, uint32_t dst_ip_be,
                          const void *udp_packet, size_t udp_len) {
    return net_l4_checksum(IP_PROTO_UDP, src_ip_be, dst_ip_be,
                           udp_packet, udp_len);
}

/* ---- pretty-printers ------------------------------------------- */

static char *put_uint(char *p, unsigned v) {
    char tmp[8]; int n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) *p++ = tmp[n];
    return p;
}

static char hex_lo(unsigned v) {
    v &= 0xF;
    return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

void net_format_ip(char dst[16], uint32_t ip_be) {
    /* ip_be stored as BE on the wire; the four bytes a.b.c.d are
     * already in memory order. */
    const uint8_t *b = (const uint8_t *)&ip_be;
    char *p = dst;
    p = put_uint(p, b[0]); *p++ = '.';
    p = put_uint(p, b[1]); *p++ = '.';
    p = put_uint(p, b[2]); *p++ = '.';
    p = put_uint(p, b[3]);
    *p = '\0';
}

void net_format_mac(char dst[18], const uint8_t mac[ETH_ADDR_LEN]) {
    char *p = dst;
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        *p++ = hex_lo((unsigned)mac[i] >> 4);
        *p++ = hex_lo((unsigned)mac[i]);
        if (i != ETH_ADDR_LEN - 1) *p++ = ':';
    }
    *p = '\0';
}
