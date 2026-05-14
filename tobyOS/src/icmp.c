/* icmp.c -- ICMPv4 echo reply so LAN hosts can `ping` tobyOS. */

#include <tobyos/icmp.h>
#include <tobyos/ip.h>
#include <tobyos/net.h>
#include <tobyos/printk.h>
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
