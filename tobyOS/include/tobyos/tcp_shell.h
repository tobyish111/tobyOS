/* tcp_shell.h -- remote shell over TCP (one client at a time).
 *
 * Listens on TCP_SHELL_PORT (default 7777). Each complete line (LF)
 * is passed to shell_run_test_line(); command output still goes to the
 * normal printk/console sinks — the TCP session gets a prompt and
 * optional status line only.
 *
 * tcp_shell_init() after net_init(); tcp_shell_poll() from net_poll().
 */

#ifndef TOBYOS_TCP_SHELL_H
#define TOBYOS_TCP_SHELL_H

#ifndef TCP_SHELL_PORT
#define TCP_SHELL_PORT 7777u
#endif

void tcp_shell_init(void);
void tcp_shell_poll(void);

#endif /* TOBYOS_TCP_SHELL_H */
