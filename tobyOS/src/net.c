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
#include <tobyos/arp.h>
#include <tobyos/socket.h>
#include <tobyos/dhcp.h>
#include <tobyos/tcp.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/cpu.h>
#include <tobyos/klibc.h>

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

/* Set once in net_init(): true iff the running IPv4 config came from DHCP
 * (not static fallback). Used by bootlog UDP upload targeting. */
static bool g_net_boot_via_dhcp;

bool net_is_up(void) { return g_net_up; }

bool net_boot_used_dhcp(void) { return g_net_boot_via_dhcp; }

/* ---- net_dev registry ------------------------------------------- */

static struct net_dev *g_net_devs[NET_MAX_DEVICES];
static size_t          g_net_dev_count;

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
        net_warm_arp(nd, g_my_dns_be, "dns    ");
    }
}

/* Brief pause + RX drain between DHCP attempts (home routers / PHY
 * sometimes miss the first DISCOVER or answer late). */
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
        kprintf("[net] no NIC registered -- networking disabled\n");
        return false;
    }

    /* Adopt the first registered NIC's MAC as our identity on the wire. */
    memcpy(g_my_mac, nd->mac, ETH_ADDR_LEN);
    char macbuf[18];
    net_format_mac(macbuf, g_my_mac);
    kprintf("[net] nic=%s mac=%s -- starting up\n",
            nd->name ? nd->name : "?", macbuf);

    arp_init();
    sock_init();
    tcp_init();

    /* g_my_ip stays 0 across the DHCP handshake: ip_send and udp_send
     * stamp the source IP from g_my_ip, which is exactly what BOOTP
     * wants (src = 0.0.0.0 until step 4). Receiving DHCP frames is
     * also fine -- ip_recv accepts datagrams addressed to the
     * 255.255.255.255 broadcast regardless of g_my_ip. */
    g_my_ip      = 0;
    g_my_netmask = 0;
    g_gateway_ip = 0;
    g_my_dns_be  = 0;
    g_net_up     = true;          /* mark up so udp_send / arp_send work */

    /* Try DHCP. Budget is several seconds; dhcp.c spends ~65% on
     * DISCOVER/OFFER (retransmit every 300 ms) then REQUEST/ACK to the
     * same deadline. */
#ifdef FAST_BOOT
    /* Some home routers answer DHCP slowly on cold boot. */
    enum { dhcp_boot_budget_ms = 5000 };
#else
    enum { dhcp_boot_budget_ms = 6000 };
#endif
    struct dhcp_lease lease;
    bool dhcp_ok = dhcp_acquire(dhcp_boot_budget_ms, &lease);
    if (!dhcp_ok) {
        kprintf("[net] DHCP attempt 1 failed — retrying after short gap\n");
        net_dhcp_retry_gap(nd, 250);
        dhcp_ok = dhcp_acquire(dhcp_boot_budget_ms, &lease);
    }
    if (dhcp_ok) {
        g_net_boot_via_dhcp = true;
        net_apply_lease(&lease, "DHCP");
    } else {
        g_net_boot_via_dhcp = false;
#ifdef TOBY_NET_FALLBACK_SLIRP
        kprintf("[net] DHCP failed (2 attempts) -- using static SLIRP fallback (10.0.2.15/24)\n");
#else
        kprintf("[net] DHCP failed (2 attempts) -- using static fallback 192.168.68.10/22 (gw/dns .68.1)\n");
#endif
        net_apply_static_fallback();
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
    g_my_ip = 0; g_my_netmask = 0; g_gateway_ip = 0; g_my_dns_be = 0;

    struct dhcp_lease lease;
    if (!dhcp_acquire(5000, &lease)) {
        kprintf("[net] dhcp renew: failed -- restoring previous lease\n");
        g_my_ip = prev_ip; g_my_netmask = prev_msk;
        g_gateway_ip = prev_gw; g_my_dns_be = prev_dns;
        return false;
    }
    net_apply_lease(&lease, "DHCP-renew");
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
