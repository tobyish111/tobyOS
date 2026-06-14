/* dhcpv6.c -- stateful DHCPv6 client (RFC 8415). See dhcpv6.h.
 *
 * Messages and options are network byte order. A DHCPv6 message is:
 *   msg-type(1) | transaction-id(3) | options...
 * each option being: option-code(2) | option-len(2) | option-data.
 */

#include <tobyos/dhcpv6.h>
#include <tobyos/ipv6.h>
#include <tobyos/net.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* ---- protocol constants ---- */
#define DHCP6_CLIENT_PORT 546
#define DHCP6_SERVER_PORT 547

#define DHCP6_MSG_SOLICIT       1
#define DHCP6_MSG_ADVERTISE     2
#define DHCP6_MSG_REQUEST       3
#define DHCP6_MSG_REPLY         7
#define DHCP6_MSG_INFO_REQUEST  11

#define DHCP6_OPT_CLIENTID      1
#define DHCP6_OPT_SERVERID      2
#define DHCP6_OPT_IA_NA         3
#define DHCP6_OPT_IAADDR        5
#define DHCP6_OPT_ORO           6
#define DHCP6_OPT_ELAPSED_TIME  8
#define DHCP6_OPT_STATUS_CODE   13
#define DHCP6_OPT_RAPID_COMMIT  14
#define DHCP6_OPT_DNS_SERVERS   23

#define DHCP6_DUID_LL           3       /* DUID based on link-layer address */
#define DHCP6_SERVERID_MAX      130

/* All_DHCP_Relay_Agents_and_Servers = ff02::1:2 */
static const struct ipv6_addr DHCP6_MCAST = {{
    0xFF,0x02, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0x01, 0,0x02
}};

/* ---- client state ---- */
enum dhcp6_state { DHCP6_IDLE, DHCP6_SOLICITING, DHCP6_REQUESTING, DHCP6_BOUND };
static enum dhcp6_state g_state;
static uint8_t  g_xid[3];
static uint32_t g_xid_ctr = 0x010203;
static const uint8_t g_iaid[4] = { 0, 0, 0, 1 };
static bool     g_have_serverid;
static uint8_t  g_serverid[DHCP6_SERVERID_MAX];
static uint16_t g_serverid_len;
static struct ipv6_addr g_offered;       /* address from an ADVERTISE */
static struct ipv6_addr g_dns;           /* first DNS server, if offered */
static bool     g_have_dns;

/* ---- UDP-over-IPv6 checksum (RFC 8200 pseudo-header) ---- */
static uint16_t udp6_checksum(const struct ipv6_addr *src,
                              const struct ipv6_addr *dst,
                              const void *payload, size_t len) {
    uint32_t sum = 0;
    const uint16_t *s = (const uint16_t *)src->bytes;
    const uint16_t *d = (const uint16_t *)dst->bytes;
    for (int i = 0; i < 8; i++) { sum += ntohs(s[i]); sum += ntohs(d[i]); }
    sum += (uint32_t)len;
    sum += IPV6_NH_UDP;
    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~(uint16_t)sum);
}

/* ---- option / message builders ---- */

static size_t put_opt(uint8_t *p, uint16_t code, uint16_t len, const void *data) {
    p[0] = (uint8_t)(code >> 8); p[1] = (uint8_t)code;
    p[2] = (uint8_t)(len >> 8);  p[3] = (uint8_t)len;
    if (len && data) memcpy(p + 4, data, len);
    return (size_t)4 + len;
}

static size_t build_duid(uint8_t *out) {
    out[0] = 0x00; out[1] = DHCP6_DUID_LL;   /* DUID-LL */
    out[2] = 0x00; out[3] = 0x01;            /* hardware type: Ethernet */
    memcpy(out + 4, g_my_mac, 6);
    return 10;
}

static void new_xid(void) {
    g_xid_ctr = g_xid_ctr * 1664525u + 1013904223u;
    g_xid[0] = (uint8_t)(g_xid_ctr >> 16);
    g_xid[1] = (uint8_t)(g_xid_ctr >> 8);
    g_xid[2] = (uint8_t)(g_xid_ctr);
}

/* Build a SOLICIT / REQUEST / INFORMATION-REQUEST into `buf`. When
 * `iaaddr` is non-NULL it's embedded as an IAADDR inside the IA_NA (used
 * by REQUEST to confirm the offered address). Returns the message length. */
static size_t build_msg(uint8_t *buf, size_t cap, uint8_t type,
                        const struct ipv6_addr *iaaddr) {
    if (cap < 64) return 0;
    size_t n = 0;
    buf[n++] = type;
    buf[n++] = g_xid[0]; buf[n++] = g_xid[1]; buf[n++] = g_xid[2];

    uint8_t duid[10]; build_duid(duid);
    n += put_opt(buf + n, DHCP6_OPT_CLIENTID, 10, duid);

    if (type == DHCP6_MSG_REQUEST && g_have_serverid)
        n += put_opt(buf + n, DHCP6_OPT_SERVERID, g_serverid_len, g_serverid);

    /* IA_NA (not in INFORMATION-REQUEST, which is for non-address config). */
    if (type != DHCP6_MSG_INFO_REQUEST) {
        uint8_t iana[12 + 4 + 24];
        size_t il = 0;
        memcpy(iana + il, g_iaid, 4); il += 4;       /* IAID */
        memset(iana + il, 0, 8);      il += 8;        /* T1, T2 = 0 (server) */
        if (iaaddr) {
            uint8_t ab[24];
            memcpy(ab, iaaddr->bytes, 16);
            memset(ab + 16, 0, 8);                    /* preferred/valid = 0 */
            il += put_opt(iana + il, DHCP6_OPT_IAADDR, 24, ab);
        }
        n += put_opt(buf + n, DHCP6_OPT_IA_NA, (uint16_t)il, iana);
    }

    uint8_t et[2] = { 0, 0 };
    n += put_opt(buf + n, DHCP6_OPT_ELAPSED_TIME, 2, et);

    if (type == DHCP6_MSG_SOLICIT)
        n += put_opt(buf + n, DHCP6_OPT_RAPID_COMMIT, 0, 0);

    uint8_t oro[2] = { 0x00, DHCP6_OPT_DNS_SERVERS };  /* request DNS servers */
    n += put_opt(buf + n, DHCP6_OPT_ORO, 2, oro);
    return n;
}

/* ---- option parsing ---- */

static bool find_opt(const uint8_t *buf, size_t len, uint16_t code,
                     const uint8_t **odata, uint16_t *olen) {
    size_t off = 0;
    while (off + 4 <= len) {
        uint16_t c = ((uint16_t)buf[off] << 8) | buf[off + 1];
        uint16_t l = ((uint16_t)buf[off + 2] << 8) | buf[off + 3];
        if (off + 4 + l > len) break;
        if (c == code) { *odata = buf + off + 4; *olen = l; return true; }
        off += 4u + l;
    }
    return false;
}

/* Find the IA_NA option, then the IAADDR within it, and copy the address. */
static bool extract_address(const uint8_t *msg, size_t len, struct ipv6_addr *out) {
    if (len < 4) return false;
    const uint8_t *iana; uint16_t ial;
    if (!find_opt(msg + 4, len - 4, DHCP6_OPT_IA_NA, &iana, &ial)) return false;
    if (ial < 12) return false;
    const uint8_t *iaaddr; uint16_t al;
    if (!find_opt(iana + 12, (size_t)ial - 12, DHCP6_OPT_IAADDR, &iaaddr, &al))
        return false;
    if (al < 16) return false;
    memcpy(out->bytes, iaaddr, 16);
    return true;
}

static void capture_serverid(const uint8_t *msg, size_t len) {
    if (len < 4) return;
    const uint8_t *sid; uint16_t sl;
    if (find_opt(msg + 4, len - 4, DHCP6_OPT_SERVERID, &sid, &sl) &&
        sl <= DHCP6_SERVERID_MAX) {
        memcpy(g_serverid, sid, sl);
        g_serverid_len = sl;
        g_have_serverid = true;
    }
}

static void capture_dns(const uint8_t *msg, size_t len) {
    if (len < 4) return;
    const uint8_t *dns; uint16_t dl;
    if (find_opt(msg + 4, len - 4, DHCP6_OPT_DNS_SERVERS, &dns, &dl) && dl >= 16) {
        memcpy(g_dns.bytes, dns, 16);          /* first server */
        g_have_dns = true;
    }
}

/* ---- send ---- */

static void dhcpv6_send(const uint8_t *msg, size_t mlen) {
    size_t ulen = 8 + mlen;
    uint8_t udp[ETH_MTU];
    if (ulen > sizeof(udp)) return;

    udp[0] = (uint8_t)(DHCP6_CLIENT_PORT >> 8); udp[1] = (uint8_t)DHCP6_CLIENT_PORT;
    udp[2] = (uint8_t)(DHCP6_SERVER_PORT >> 8); udp[3] = (uint8_t)DHCP6_SERVER_PORT;
    udp[4] = (uint8_t)(ulen >> 8); udp[5] = (uint8_t)ulen;
    udp[6] = 0; udp[7] = 0;
    memcpy(udp + 8, msg, mlen);

    const struct ipv6_addr *src = ipv6_our_linklocal();
    uint16_t cks = udp6_checksum(src, &DHCP6_MCAST, udp, ulen);
    memcpy(udp + 6, &cks, 2);                  /* already network order */

    ipv6_send(&DHCP6_MCAST, IPV6_NH_UDP, udp, ulen);
}

/* ---- public: start + receive ---- */

void dhcpv6_start(bool stateful) {
    new_xid();
    g_state = DHCP6_SOLICITING;
    g_have_serverid = false;
    g_have_dns = false;

    uint8_t buf[256];
    size_t n = build_msg(buf, sizeof(buf),
                         stateful ? DHCP6_MSG_SOLICIT : DHCP6_MSG_INFO_REQUEST, 0);
    dhcpv6_send(buf, n);
    kprintf("[dhcp6] %s sent (xid %02x%02x%02x)\n",
            stateful ? "SOLICIT+rapid-commit" : "INFORMATION-REQUEST",
            g_xid[0], g_xid[1], g_xid[2]);
}

static void commit_reply(const uint8_t *msg, size_t len) {
    struct ipv6_addr addr;
    capture_serverid(msg, len);
    capture_dns(msg, len);
    if (extract_address(msg, len, &addr)) {
        ipv6_dhcp6_configure(&addr);
        g_state = DHCP6_BOUND;
    } else {
        /* INFORMATION-REQUEST REPLY carries no address -- just other config. */
        g_state = DHCP6_BOUND;
    }
    if (g_have_dns) {
        char b[40]; ipv6_format(b, sizeof(b), &g_dns);
        kprintf("[dhcp6] DNS server %s\n", b);
    }
}

void dhcpv6_recv(const struct ipv6_addr *src, const uint8_t *msg, size_t len) {
    (void)src;
    if (len < 4) return;
    uint8_t type = msg[0];
    if (memcmp(msg + 1, g_xid, 3) != 0) return;   /* not our transaction */

    switch (type) {
    case DHCP6_MSG_REPLY:
        commit_reply(msg, len);
        break;
    case DHCP6_MSG_ADVERTISE: {
        /* No rapid-commit: capture the offer and send a REQUEST. */
        if (g_state != DHCP6_SOLICITING) break;
        capture_serverid(msg, len);
        if (!extract_address(msg, len, &g_offered)) break;
        new_xid();                                /* new exchange */
        g_state = DHCP6_REQUESTING;
        uint8_t req[256];
        size_t n = build_msg(req, sizeof(req), DHCP6_MSG_REQUEST, &g_offered);
        dhcpv6_send(req, n);
        break;
    }
    default:
        break;
    }
}

bool dhcpv6_bound(void) { return g_state == DHCP6_BOUND; }

/* ================================================================
 * Self-test (-DDHCPV6_SELFTEST). QEMU SLIRP has no DHCPv6 server, so we
 * synthesize server messages and drive the real receive path. [DHCP6T].
 * ================================================================ */

/* Append a server REPLY/ADVERTISE skeleton matching xid `g_xid`, with a
 * SERVERID, an IA_NA carrying IAADDR `addr`, and a DNS server. Returns len. */
static size_t synth_server_msg(uint8_t *buf, uint8_t type,
                               const struct ipv6_addr *addr,
                               const struct ipv6_addr *dns) {
    size_t n = 0;
    buf[n++] = type;
    buf[n++] = g_xid[0]; buf[n++] = g_xid[1]; buf[n++] = g_xid[2];

    uint8_t sid[10] = { 0x00, 0x03, 0x00, 0x01, 0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    n += put_opt(buf + n, DHCP6_OPT_SERVERID, 10, sid);

    uint8_t cid[10]; build_duid(cid);
    n += put_opt(buf + n, DHCP6_OPT_CLIENTID, 10, cid);

    /* IA_NA = IAID(4) + T1(4) + T2(4) + IAADDR{addr(16)+pref(4)+valid(4)} */
    uint8_t iana[12 + 4 + 24];
    size_t il = 0;
    memcpy(iana + il, g_iaid, 4); il += 4;
    iana[il++]=0;iana[il++]=0;iana[il++]=0x0e;iana[il++]=0x10;   /* T1 = 3600 */
    iana[il++]=0;iana[il++]=0;iana[il++]=0x1c;iana[il++]=0x20;   /* T2 = 7200 */
    uint8_t ab[24];
    memcpy(ab, addr->bytes, 16);
    ab[16]=0;ab[17]=0;ab[18]=0x1c;ab[19]=0x20;   /* preferred 7200 */
    ab[20]=0;ab[21]=0;ab[22]=0x38;ab[23]=0x40;   /* valid 14400 */
    il += put_opt(iana + il, DHCP6_OPT_IAADDR, 24, ab);
    n += put_opt(buf + n, DHCP6_OPT_IA_NA, (uint16_t)il, iana);

    n += put_opt(buf + n, DHCP6_OPT_DNS_SERVERS, 16, dns->bytes);
    return n;
}

int dhcpv6_self_test(void) {
    int fails = 0;
    kprintf("[DHCP6T] DHCPv6 client self-test\n");

    /* assigned address 2001:db8::abcd and DNS 2001:db8::53 */
    struct ipv6_addr want; memset(&want, 0, sizeof(want));
    want.bytes[0]=0x20; want.bytes[1]=0x01; want.bytes[2]=0x0d; want.bytes[3]=0xb8;
    want.bytes[14]=0xab; want.bytes[15]=0xcd;
    struct ipv6_addr dns; memset(&dns, 0, sizeof(dns));
    dns.bytes[0]=0x20; dns.bytes[1]=0x01; dns.bytes[2]=0x0d; dns.bytes[3]=0xb8;
    dns.bytes[15]=0x53;

    /* 1. SOLICIT is well-formed: type + the required options present. */
    {
        new_xid();
        uint8_t sol[256];
        size_t n = build_msg(sol, sizeof(sol), DHCP6_MSG_SOLICIT, 0);
        const uint8_t *d; uint16_t l;
        bool ok = n > 4 && sol[0] == DHCP6_MSG_SOLICIT &&
                  find_opt(sol + 4, n - 4, DHCP6_OPT_CLIENTID, &d, &l) &&
                  find_opt(sol + 4, n - 4, DHCP6_OPT_IA_NA, &d, &l) &&
                  find_opt(sol + 4, n - 4, DHCP6_OPT_RAPID_COMMIT, &d, &l) &&
                  find_opt(sol + 4, n - 4, DHCP6_OPT_ELAPSED_TIME, &d, &l) &&
                  find_opt(sol + 4, n - 4, DHCP6_OPT_ORO, &d, &l);
        if (!ok) fails++;
        kprintf("[DHCP6T] t1 SOLICIT well-formed: %s (%lu bytes)\n",
                ok ? "PASS" : "FAIL", (unsigned long)n);
    }

    /* 2. Rapid-commit: a REPLY directly binds the address + DNS. */
    {
        g_state = DHCP6_SOLICITING; new_xid();
        ipv6_dhcp6_reset_for_test();
        uint8_t rep[256];
        size_t n = synth_server_msg(rep, DHCP6_MSG_REPLY, &want, &dns);
        struct ipv6_addr from = DHCP6_MCAST;
        dhcpv6_recv(&from, rep, n);
        bool ok = dhcpv6_bound() && ipv6_have_dhcp6() &&
                  ipv6_addr_equal(ipv6_dhcp6_addr(), &want) &&
                  g_have_dns && ipv6_addr_equal(&g_dns, &dns);
        if (!ok) fails++;
        kprintf("[DHCP6T] t2 rapid-commit REPLY -> bound: %s\n",
                ok ? "PASS" : "FAIL");
    }

    /* 3. Four-message: ADVERTISE -> (REQUEST) -> REPLY. */
    {
        g_state = DHCP6_SOLICITING; new_xid();
        ipv6_dhcp6_reset_for_test();
        uint8_t adv[256];
        size_t n = synth_server_msg(adv, DHCP6_MSG_ADVERTISE, &want, &dns);
        struct ipv6_addr from = DHCP6_MCAST;
        dhcpv6_recv(&from, adv, n);
        bool advanced = (g_state == DHCP6_REQUESTING) &&
                        ipv6_addr_equal(&g_offered, &want) && g_have_serverid;
        /* the REQUEST rolled a new xid; the server's REPLY echoes it */
        uint8_t rep[256];
        size_t m = synth_server_msg(rep, DHCP6_MSG_REPLY, &want, &dns);
        dhcpv6_recv(&from, rep, m);
        bool ok = advanced && dhcpv6_bound() &&
                  ipv6_have_dhcp6() && ipv6_addr_equal(ipv6_dhcp6_addr(), &want);
        if (!ok) fails++;
        kprintf("[DHCP6T] t3 ADVERTISE->REQUEST->REPLY: %s (advanced=%d)\n",
                ok ? "PASS" : "FAIL", advanced);
    }

    /* Leave the client idle so a real RA-triggered exchange starts clean. */
    g_state = DHCP6_IDLE;
    ipv6_dhcp6_reset_for_test();

    kprintf("[DHCP6T] %s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : -1;
}
