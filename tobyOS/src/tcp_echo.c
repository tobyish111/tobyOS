/* tcp_echo.c -- kernel TCP echo test service. */

#include <stddef.h>
#include <stdint.h>

#include <tobyos/tcp_echo.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/printk.h>

#ifndef TCP_ECHO_PORT
#define TCP_ECHO_PORT 1234u
#endif

static struct tcp_conn *g_lsn;
static struct tcp_conn *g_cli;

void tcp_echo_init(void) {
    if (!net_is_up() || g_my_ip == 0)
        return;

    if (g_lsn)
        return;

    g_lsn = tcp_listen(htons((uint16_t)TCP_ECHO_PORT), 4);
    if (!g_lsn) {
        kprintf("[tcp-echo] listen TCP/%u failed\n",
                (unsigned)TCP_ECHO_PORT);
        return;
    }

    kprintf("[tcp-echo] listening on TCP port %u\n",
            (unsigned)TCP_ECHO_PORT);
}

void tcp_echo_poll(void) {
    if (!g_lsn || !net_is_up())
        return;

    if (!g_cli) {
        g_cli = tcp_accept(g_lsn, 0);
        if (g_cli) {
            kprintf("[tcp-echo] accepted new connection\n");
        } else {
            return;
        }
    }

    uint8_t buf[512];

    long n = tcp_recv(g_cli, buf, sizeof(buf), 0);
    if (n > 0) {
        kprintf("[tcp-echo] rx %ld bytes\n", n);

        if (tcp_send(g_cli, buf, (size_t)n) < 0) {
            kprintf("[tcp-echo] send failed; closing client\n");
            tcp_close(g_cli);
            g_cli = NULL;
        }
        return;
    }

    if (n < 0) {
        kprintf("[tcp-echo] recv closed/error; closing client\n");
        tcp_close(g_cli);
        g_cli = NULL;
        return;
    }

    tcp_state_t st = tcp_state(g_cli);
    if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT) {
        kprintf("[tcp-echo] closing client state=%d\n", (int)st);
        tcp_close(g_cli);
        g_cli = NULL;
    }
}