/* dhcpv6.h -- stateful DHCPv6 client (RFC 8415).
 *
 * The IPv6 SLAAC path (icmpv6.c) already autoconfigures a global address
 * from a Router Advertisement's Prefix Information. DHCPv6 is the
 * *stateful* counterpart: when an RA sets the M (Managed) flag, the host
 * is told to obtain its address from a DHCPv6 server instead. This client
 * runs the exchange and installs the server-assigned address.
 *
 * Transport: UDP, client port 546, server port 547, sent to the
 * All_DHCP_Relay_Agents_and_Servers multicast ff02::1:2.
 *
 * Exchanges supported:
 *   - SOLICIT (+ Rapid Commit) -> REPLY            (2-message, preferred)
 *   - SOLICIT -> ADVERTISE -> REQUEST -> REPLY     (4-message fallback)
 *   - INFORMATION-REQUEST -> REPLY                 (O flag: other config)
 *
 * QEMU's user-mode SLIRP does not implement a DHCPv6 server (it doesn't
 * even answer RAs), so the receive/configure path is validated by an
 * opt-in synthetic self-test, exactly as SLAAC was.
 */

#ifndef TOBYOS_DHCPV6_H
#define TOBYOS_DHCPV6_H

#include <tobyos/types.h>

struct ipv6_addr;

/* Begin a DHCPv6 exchange. `stateful` (RA M flag) -> SOLICIT for an
 * address; otherwise (RA O flag only) -> INFORMATION-REQUEST for other
 * configuration (e.g. DNS). Idempotent-ish: re-arms the transaction. */
void dhcpv6_start(bool stateful);

/* Feed a received DHCPv6 message (the bytes after the UDP header) to the
 * client. Called from the IPv6 UDP demux for dst port 546. */
void dhcpv6_recv(const struct ipv6_addr *src, const uint8_t *msg, size_t len);

/* True once the client has bound a server-assigned address. */
bool dhcpv6_bound(void);

/* Self-test: builds a SOLICIT and checks its structure, then drives the
 * receive path with synthetic ADVERTISE/REPLY messages and asserts the
 * assigned address (and DNS server) are extracted + configured. Returns 0
 * on all-pass. Emits [DHCP6T] markers. -DDHCPV6_SELFTEST. */
int dhcpv6_self_test(void);

#endif /* TOBYOS_DHCPV6_H */
