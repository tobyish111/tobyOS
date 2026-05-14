/* tcp_shell.c -- one-client TCP line editor driving the kernel shell. */

#include <tobyos/tcp_shell.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/shell.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define TSH_LINE_MAX 256u

static struct tcp_conn *g_lsn;
static struct tcp_conn *g_cli;
static char             g_buf[TSH_LINE_MAX];
static size_t           g_len;

static void tsh_send_str(struct tcp_conn *c, const char *s) {
    if (!c || !s) return;

    size_t n = strlen(s);
    if (n == 0) return;

    long w = tcp_send(c, s, n);
    if (w <= 0) {
        kprintf("[tcp-shell] send failed/stalled\n");
    }
}

static void tsh_shell_write(const char *s, void *ctx) {
    struct tcp_conn *c = (struct tcp_conn *)ctx;
    tsh_send_str(c, s);
}

void tcp_shell_init(void) {
    if (!net_is_up() || g_my_ip == 0)
        return;

    if (g_lsn)
        return;

    g_lsn = tcp_listen(htons((uint16_t)TCP_SHELL_PORT), 4);
    if (!g_lsn) {
        kprintf("[tcp-shell] listen TCP/%u failed (busy or no slot)\n",
                (unsigned)TCP_SHELL_PORT);
        return;
    }

    kprintf("[tcp-shell] listening on TCP port %u\n",
            (unsigned)TCP_SHELL_PORT);
}

void tcp_shell_poll(void) {
    if (!g_lsn || !net_is_up())
        return;

    if (!g_cli) {
        g_cli = tcp_accept(g_lsn, 0);
        if (g_cli) {
            kprintf("[tcp-shell] client connected\n");

            g_len = 0;
            g_buf[0] = '\0';

            tsh_send_str(g_cli,
                         "tobyOS TCP shell\r\n"
                         "Input and selected builtin output are accepted over TCP.\r\n"
                        "Commands still using kprintf will print to local console/serial.\r\n");
        } else {
            return;
        }
    }

    uint8_t chunk[256];
    long n = tcp_recv(g_cli, chunk, sizeof(chunk), 0);

    if (n > 0) {
        for (long i = 0; i < n; i++) {
            char ch = (char)chunk[i];

            if (ch == '\r')
                continue;

            if (ch == '\b' || ch == 0x7F) {
                if (g_len > 0) {
                    g_len--;
                    g_buf[g_len] = '\0';
                    tsh_send_str(g_cli, "\b \b");
                }
                continue;
            }

            if (ch == '\n') {
                g_buf[g_len] = '\0';
            
                tsh_send_str(g_cli, "\r\n");
            
                shell_set_output(tsh_shell_write, g_cli);
                shell_run_test_line(g_buf);
                shell_set_output(NULL, NULL);
            
                g_len = 0;
                g_buf[0] = '\0';
            
                tsh_send_str(g_cli, "\r\ntobyOS> ");
                continue;
            }

            if ((unsigned char)ch < 0x20 && ch != '\t')
                continue;

            if (g_len + 1 >= TSH_LINE_MAX) {
                tsh_send_str(g_cli,
                             "\r\n[line too long, cleared]\r\ntobyOS> ");
                g_len = 0;
                g_buf[0] = '\0';
                continue;
            }

            g_buf[g_len++] = ch;
            char echo[2] = { ch, '\0' };
            tsh_send_str(g_cli, echo);
        }

        return;
    }

    if (n < 0) {
        kprintf("[tcp-shell] client disconnected/error\n");
        tcp_close(g_cli);
        g_cli = NULL;
        g_len = 0;
        g_buf[0] = '\0';
        return;
    }

    /*
     * n == 0: no data available this poll.
     * If the TCP state is no longer usable, clean up the client.
     */
    tcp_state_t st = tcp_state(g_cli);
    if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT) {
        kprintf("[tcp-shell] closing client state=%d\n", (int)st);
        tcp_close(g_cli);
        g_cli = NULL;
        g_len = 0;
        g_buf[0] = '\0';
    }
}