/* icmp.c -- ICMPv4 echo reply so LAN hosts can `ping` tobyOS. */

#include <tobyos/icmp.h>
#include <tobyos/ip.h>
#include <tobyos/net.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>
#include <tobyos/cpu.h>

#define ICMP_ECHOREPLY 0
#define ICMP_ECHO      8

struct __attribute__((packed)) icmp_echo_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
};

void icmp_recv(uint32_t src_ip_be, const void *icmp_pkt, size_t len) {
    if (g_my_ip == 0) return;
    if (len < sizeof(struct icmp_echo_hdr)) return;
    if (len > 65535u - IP_HDR_LEN) return;

    const uint8_t *pkt = (const uint8_t *)icmp_pkt;
    const struct icmp_echo_hdr *ih = (const struct icmp_echo_hdr *)pkt;
    if (ih->type != ICMP_ECHO || ih->code != 0) return;

    if (net_checksum(pkt, len) != 0) return;

    uint8_t *rbuf = kmalloc(len);
    if (!rbuf) return;
    memcpy(rbuf, pkt, len);

    struct icmp_echo_hdr *rh = (struct icmp_echo_hdr *)rbuf;
    rh->type     = ICMP_ECHOREPLY;
    rh->checksum = 0;
    rh->checksum = net_checksum(rbuf, len);

    /* ip_send() returns false on ARP miss and does not retry; the first
     * ping from a peer often arrives before we've seen their ARP, so we
     * wouldn't know their MAC to send the echo reply. */
    struct net_dev *nd = net_default();
    uint32_t          hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t end = pit_ticks() + ((uint64_t)hz * 600u) / 1000u;
    bool     sent = false;
    while (pit_ticks() < end) {
        if (ip_send(src_ip_be, IP_PROTO_ICMP, rbuf, len)) {
            sent = true;
            break;
        }
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        sti();
        hlt();
    }
    (void)sent;

    kfree(rbuf);
}
