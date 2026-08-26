/* icmp.c -- ICMPv4 echo reply so LAN hosts can `ping` tobyOS. */

#include <tobyos/icmp.h>
#include <tobyos/ip.h>
#include <tobyos/net.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>
#include <tobyos/cpu.h>
#include <tobyos/perf.h>   /* perf_now_ns: ICMP error rate limit */

#define ICMP_ECHOREPLY 0
#define ICMP_UNREACH   3
#define ICMP_ECHO      8

struct __attribute__((packed)) icmp_echo_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
};

void icmp_recv(uint32_t src_ip_be, const void *icmp_pkt, size_t len) {
    if (g_my_ip == 0) {
        kprintf("[icmp] drop: no local IP yet\n");
        return;
    }

    if (len < sizeof(struct icmp_echo_hdr)) {
        kprintf("[icmp] drop: too short len=%u\n", (unsigned)len);
        return;
    }

    if (len > 65535u - IP_HDR_LEN) {
        kprintf("[icmp] drop: too large len=%u\n", (unsigned)len);
        return;
    }

    const uint8_t *pkt = (const uint8_t *)icmp_pkt;
    const struct icmp_echo_hdr *ih = (const struct icmp_echo_hdr *)pkt;

    /* Destination-unreachable (2026-08-22): the error is finally
     * SURFACED. The payload is the original IP header + >=8 L4 bytes;
     * for a UDP original, hand (code, orig dst ip/port, orig src port)
     * to the socket layer, which latches so_error on the matching
     * connected socket -- that is how connect()ed-UDP senders learn
     * ECONNREFUSED instead of waiting forever. */
    if (ih->type == ICMP_UNREACH) {
        if (len < 8 + 20 + 8) return;          /* icmp + ip + 8 L4 bytes */
        const uint8_t *oip = pkt + 8;
        uint8_t ihl = (uint8_t)((oip[0] & 0x0f) * 4);
        if (ihl < 20 || len < (size_t)(8 + ihl + 8)) return;
        uint8_t proto = oip[9];
        if (proto == IP_PROTO_UDP) {
            uint32_t odst_ip;   memcpy(&odst_ip, oip + 16, 4);
            const uint8_t *ol4 = oip + ihl;
            uint16_t osport;    memcpy(&osport, ol4 + 0, 2);
            uint16_t odport;    memcpy(&odport, ol4 + 2, 2);
            extern void sock_udp_icmp_error(uint16_t local_port_be,
                                            uint32_t peer_ip_be,
                                            uint16_t peer_port_be,
                                            uint8_t code);
            sock_udp_icmp_error(osport, odst_ip, odport, ih->code);
        }
        return;
    }

    if (ih->type != ICMP_ECHO || ih->code != 0) {
        kprintf("[icmp] drop: type=%u code=%u\n",
                (unsigned)ih->type, (unsigned)ih->code);
        return;
    }

    if (net_checksum(pkt, len) != 0) {
        kprintf("[icmp] drop: bad checksum len=%u\n", (unsigned)len);
        return;
    }

    kprintf("[icmp] echo request from %u.%u.%u.%u id=%u seq=%u len=%u\n",
            (unsigned)(src_ip_be & 0xFF),
            (unsigned)((src_ip_be >> 8) & 0xFF),
            (unsigned)((src_ip_be >> 16) & 0xFF),
            (unsigned)((src_ip_be >> 24) & 0xFF),
            (unsigned)ntohs(ih->id),
            (unsigned)ntohs(ih->seq),
            (unsigned)len);

    uint8_t *rbuf = kmalloc(len);
    if (!rbuf) {
        kprintf("[icmp] drop: kmalloc failed\n");
        return;
    }

    memcpy(rbuf, pkt, len);

    struct icmp_echo_hdr *rh = (struct icmp_echo_hdr *)rbuf;
    rh->type     = ICMP_ECHOREPLY;
    rh->code     = 0;
    rh->checksum = 0;
    rh->checksum = net_checksum(rbuf, len);

    if (!ip_send(src_ip_be, IP_PROTO_ICMP, rbuf, len)) {
        kprintf("[icmp] echo reply send failed: ARP miss? dst=%u.%u.%u.%u\n",
                (unsigned)(src_ip_be & 0xFF),
                (unsigned)((src_ip_be >> 8) & 0xFF),
                (unsigned)((src_ip_be >> 16) & 0xFF),
                (unsigned)((src_ip_be >> 24) & 0xFF));
    } else {
        kprintf("[icmp] echo reply sent\n");
    }

    kfree(rbuf);
}

/* Emit ICMP destination-unreachable/port (type 3 code 3) for a UDP
 * datagram that arrived with NO listener (2026-08-22 -- the drop used to
 * be silent, so remote senders waited out their full timeouts against a
 * port that could have told them instantly). The original IP header is
 * reconstructed from what udp_recv knows -- receivers match on the
 * embedded ports, which are exact. Rate-limited: 1 per 10 ms, as Linux
 * rate-limits ICMP errors. */
void icmp_send_port_unreach(uint32_t orig_src_ip_be,
                            const void *orig_udp_hdr, size_t l4len) {
    if (g_my_ip == 0 || !orig_udp_hdr || l4len < 8) return;
    static uint64_t last_ns;
    uint64_t now = perf_now_ns();
    if (now - last_ns < 10000000ull) return;
    last_ns = now;

    uint8_t buf[8 + 20 + 8];
    memset(buf, 0, sizeof buf);
    buf[0] = ICMP_UNREACH; buf[1] = 3;         /* port unreachable */
    uint8_t *oip = buf + 8;
    oip[0] = 0x45;                              /* v4, ihl 5 */
    oip[8] = 64;                                /* ttl */
    oip[9] = IP_PROTO_UDP;
    memcpy(oip + 12, &orig_src_ip_be, 4);       /* orig src */
    {   /* Loopback datagrams travel src==dst (see ip_send), so the
         * "original destination" the sender's socket compares its peer
         * against is the loop address itself, not our eth unicast. */
        uint32_t odst = ((orig_src_ip_be & 0xFFu) == 127u)
                        ? orig_src_ip_be : g_my_ip;
        memcpy(oip + 16, &odst, 4);             /* orig dst */
    }
    memcpy(oip + 20, orig_udp_hdr, 8);          /* orig UDP header */
    uint16_t cs = net_checksum(buf, sizeof buf);
    memcpy(buf + 2, &cs, 2);
    (void)ip_send(orig_src_ip_be, IP_PROTO_ICMP, buf, sizeof buf);
}
