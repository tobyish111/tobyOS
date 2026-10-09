/* tcp_shell.c -- one-client TCP line editor driving the kernel shell. */

#include <tobyos/tcp_shell.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/shell.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/perf.h>

#define TSH_LINE_MAX 256u

static struct tcp_conn *g_lsn;
static struct tcp_conn *g_cli;
static char             g_buf[TSH_LINE_MAX];
static size_t           g_len;

/* OUTPUT IS BUFFERED AND SENT IN ONE PIECE PER POLL.
 *
 * Every byte the kernel shell wrote went out as its own tcp_send() -- and
 * tcp_send BLOCKS until what it sent is acknowledged. A 700-byte `ls /dev`
 * was 700 round trips against a client that delays small ACKs; a send that
 * gave up dropped its byte and logged "send failed/stalled" (itself a line
 * that takes ~30 ms at 38400 baud); and the connection was left backed off,
 * so on the EliteDesk the NEXT command's reply -- even a bare `echo` -- took
 * ~30 s (measured: alone 1.2 s, after `ls /dev` 30.8 s and 33.1 s). QEMU
 * acknowledges at once and never showed it.
 *
 * Now everything -- the shell's output, the echo of typed keys, prompts --
 * collects here and is flushed by tsh_flush(): at the end of each poll pass,
 * before a close, and whenever the buffer fills. One send, one round trip.
 *
 * WHAT DID NOT GO STAYS. The flush was one tcp_send() of the buffer, and the
 * buffer was emptied whatever it returned: when a segment could not be queued
 * (-1 -- tcp_send waits for the window but not for a free pending slot) the
 * command's whole output was lost with the connection still up -- kbigpipe p4
 * on the EliteDesk, 98 bytes (finding 43). Now the buffer goes out through
 * tcp_send_nb(), which says how much it took; the rest stays queued for the
 * next poll pass, which sends it whether or not the client typed anything.
 * Only a full buffer (the command is still writing) and a closing session
 * WAIT for room, and that wait is bounded: a connection that takes nothing
 * for TSH_SEND_WAIT_MS is reported and its queue dropped, as before. */
#define TSH_OUT_MAX 4096u
#define TSH_SEND_WAIT_MS 30000u
static char   g_out[TSH_OUT_MAX];
static size_t g_out_len;
static bool   g_out_failed;

static void tsh_flush_ex(bool wait) {
    if (!g_cli || g_out_len == 0) { g_out_len = 0; return; }
    size_t off = 0;
    long w = 0;
    uint64_t dl = perf_now_ns() + (uint64_t)TSH_SEND_WAIT_MS * 1000000ull;
    while (off < g_out_len) {
        w = tcp_send_nb(g_cli, g_out + off, g_out_len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0) break;                       /* the connection is gone */
        if (!wait) break;                       /* the next pass sends it */
        if (perf_now_ns() >= dl) break;
        if (tcp_wait_sendable(g_cli, 100) < 0) { w = -1; break; }
    }
    if (off < g_out_len && (w < 0 || wait)) {
        if (!g_out_failed) {
            g_out_failed = true;          /* once per client, not per byte */
            kprintf("[tcp-shell] send failed/stalled (rc=%ld, %u bytes "
                    "dropped)\n", w, (unsigned)(g_out_len - off));
        }
        off = g_out_len;
    }
    if (off) memmove(g_out, g_out + off, g_out_len - off);
    g_out_len -= off;
}

static void tsh_flush(void) { tsh_flush_ex(false); }

static void tsh_out(const char *s) {
    if (!g_cli || !s) return;
    for (; *s; s++) {
        if (g_out_len == TSH_OUT_MAX) tsh_flush_ex(true);
        g_out[g_out_len++] = *s;
    }
}

static void tsh_shell_write(const char *s, void *ctx) {
    (void)ctx;
    tsh_out(s);
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
            g_out_len = 0;
            g_out_failed = false;

            tsh_out("tobyOS TCP shell\r\n"
                    "Input and selected builtin output are accepted over TCP.\r\n"
                    "Commands still using kprintf will print to local console/serial.\r\n");
            tsh_flush();
        } else {
            return;
        }
    }

    /* What an earlier pass could not send goes first, typed key or not. */
    if (g_out_len) tsh_flush();

    uint8_t chunk[256];
    /* QUIET: this is a poller. tcp_recv logs "[tcp] RECV-TIMEOUT" on every
     * empty poll -- twice a millisecond while a client sat connected, far
     * more than a 38400-baud console can carry, so the serial log overflowed
     * and garbled, lagging minutes behind the machine. tcp_recv_quiet exists
     * for exactly this (the SSH worker's same flood). */
    long n = tcp_recv_quiet(g_cli, chunk, sizeof(chunk), 0);

    if (n > 0) {
        for (long i = 0; i < n; i++) {
            char ch = (char)chunk[i];

            if (ch == '\r')
                continue;

            if (ch == '\b' || ch == 0x7F) {
                if (g_len > 0) {
                    g_len--;
                    g_buf[g_len] = '\0';
                    tsh_out("\b \b");
                }
                continue;
            }

            if (ch == '\n') {
                g_buf[g_len] = '\0';
            
                tsh_out("\r\n");
            
                shell_set_output(tsh_shell_write, g_cli);
                shell_run_test_line(g_buf);
                shell_set_output(NULL, NULL);

                /* `exit` ends THIS session, as it ends a bash login: the
                 * kernel shell has consumed the request and keeps running
                 * for the next connection. */
                if (shell_take_exit_request()) {
                    tsh_out("exit\r\n");
                    tsh_flush_ex(true);
                    /* the FIN takes a pending slot of its own */
                    (void)tcp_wait_sendable(g_cli, 5000);
                    kprintf("[tcp-shell] client ended the session (exit)\n");
                    tcp_close(g_cli);
                    g_cli = NULL;
                    g_len = 0;
                    g_buf[0] = '\0';
                    return;
                }
            
                g_len = 0;
                g_buf[0] = '\0';
            
                /* A CONTINUATION PROMPT while the shell is still collecting.
                 * This printed "tobyOS> " after every line, so `if true` or
                 * `cat <<EOF` looked as if it had run and printed nothing --
                 * the user had no sign that the next lines were being
                 * gathered. The console already says "> " (prompt()). */
                tsh_out(shell_input_pending() ? "\r\n> " : "\r\ntobyOS> ");
                continue;
            }

            if ((unsigned char)ch < 0x20 && ch != '\t')
                continue;

            if (g_len + 1 >= TSH_LINE_MAX) {
                tsh_out("\r\n[line too long, cleared]\r\ntobyOS> ");
                g_len = 0;
                g_buf[0] = '\0';
                continue;
            }

            g_buf[g_len++] = ch;
            char echo[2] = { ch, '\0' };
            tsh_out(echo);
        }

        tsh_flush();          /* this pass's echo, output and prompt: one send */
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