/* eth.h -- Ethernet II framing.
 *
 * eth_send() prepends a 14-byte header in-place into a small bounce
 * buffer and hands the result to e1000_tx. eth_recv() is the reverse:
 * the e1000 RX path calls it with the frame as-it-came-off-the-wire,
 * and we demux on ethertype to ARP / IPv4 (anything else dropped).
 */

#ifndef TOBYOS_ETH_H
#define TOBYOS_ETH_H

#include <tobyos/types.h>
#include <tobyos/net.h>

struct __attribute__((packed)) eth_hdr {
    uint8_t  dst[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t ethertype;             /* network byte order */
};

#define ETH_HDR_LEN sizeof(struct eth_hdr)

/* Build an Ethernet II frame ([dst][src][ethertype][payload]) and
 * hand it to the NIC. Returns true on success. payload_len must fit
 * in ETH_MTU. ethertype is given in HOST byte order; we swap. */
bool eth_send(const uint8_t dst_mac[ETH_ADDR_LEN],
              uint16_t      ethertype,
              const void   *payload,
              size_t        payload_len);

/* Called by e1000_rx_drain for each received frame. Inspects the
 * ethertype and dispatches to the matching protocol handler. */
void eth_recv(const void *frame, size_t len);

#endif /* TOBYOS_ETH_H */
