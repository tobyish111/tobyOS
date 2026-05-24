/* ip_frag.h -- IP fragmentation and reassembly (RFC 791).
 *
 * Standalone module that exposes a structured reassembly API and a
 * fragmenting send path. ip.c already contains inline reassembly
 * (ip_recv_maybe_reassemble) and fragmented send (ip_send); this
 * module provides an explicit public API so higher layers (e.g. a
 * future tunneling or VPN path) can invoke reassembly and MTU-aware
 * fragmented send independently.
 */

#ifndef TOBYOS_IP_FRAG_H
#define TOBYOS_IP_FRAG_H

#include <tobyos/types.h>

#define IP_FRAG_MAX_SLOTS     8
#define IP_FRAG_MAX_SIZE      8192
#define IP_FRAG_TIMEOUT_MS    30000

struct ip_fragment {
    uint16_t id;
    uint32_t src_ip;
    uint8_t *data;
    size_t   total_len;
    uint32_t received_mask;
    uint64_t timestamp;
    int      complete;
    uint8_t  protocol;
    uint32_t dst_ip;
    size_t   filled;
};

void  ip_frag_init(void);
int   ip_reassemble(const void *ip_packet, size_t len,
                    void **complete_out, size_t *complete_len);
int   ip_fragment_send(const void *payload, size_t len,
                       uint32_t dst_ip, uint8_t protocol, uint16_t mtu);
void  ip_frag_timeout_check(void);

#endif /* TOBYOS_IP_FRAG_H */
