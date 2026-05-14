/* tcp_echo.h -- kernel TCP echo service (RFC 862-style, TCP port 7).
 *
 * After net_init() succeeds, call tcp_echo_init() once. The idle loop
 * drives net_poll(), which calls tcp_echo_poll() to accept clients and
 * echo any received bytes back on the same connection.
 */

#ifndef TOBYOS_TCP_ECHO_H
#define TOBYOS_TCP_ECHO_H

void tcp_echo_init(void);
void tcp_echo_poll(void);

#endif /* TOBYOS_TCP_ECHO_H */
