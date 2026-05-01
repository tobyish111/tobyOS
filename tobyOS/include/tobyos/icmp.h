/* icmp.h -- minimal ICMPv4 (echo reply for ping). */

#ifndef TOBYOS_ICMP_H
#define TOBYOS_ICMP_H

#include <tobyos/types.h>

/* Deliver one ICMP payload after IPv4 demux (IP header stripped). */
void icmp_recv(uint32_t src_ip_be, const void *icmp_pkt, size_t len);

#endif /* TOBYOS_ICMP_H */
