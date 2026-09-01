/* tcp_user.c -- Userland TCP/TLS socket bridge.
 *
 * Exposes the kernel TCP and TLS stacks to userland programs via
 * syscalls. Maintains a connection table mapping integer IDs to
 * kernel tcp_conn/tls_conn pointers.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/tcp.h>
#include <tobyos/tls.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>

#define MAX_USER_CONNS 32

/* CONN_RESERVED exists because every allocator here hands out a slot and
 * then BLOCKS -- tcp_connect waits for a handshake, tcp_accept waits up to
 * thirty seconds for a peer. While it blocks the slot must not look free.
 *
 * It did. alloc_conn set only `pid` and returned the index, leaving
 * type == CONN_NONE until the blocking call came back, so a second process
 * allocating in that window got THE SAME SLOT. Measured: ftpd sat in
 * accept() holding slot N; ftp connected and wrote its connection into slot
 * N; ftpd's accept returned and overwrote it with the accepted socket. The
 * client then had a descriptor whose owning pid was the server's, so every
 * read returned EBADF -- which looked exactly like "the server never sent a
 * greeting". Any two processes using userland TCP at once could hit it. */
enum conn_type { CONN_NONE = 0, CONN_RESERVED, CONN_TCP, CONN_TLS, CONN_LISTEN };

struct user_conn {
    enum conn_type type;
    struct tcp_conn *tcp;
    struct tls_conn *tls;
    int pid;
};

static struct user_conn g_uconns[MAX_USER_CONNS];

static int alloc_conn(int pid) {
    for (int i = 0; i < MAX_USER_CONNS; i++) {
        if (g_uconns[i].type == CONN_NONE) {
            /* Claim it NOW, before the caller blocks. */
            g_uconns[i].type = CONN_RESERVED;
            g_uconns[i].tcp  = 0;
            g_uconns[i].tls  = 0;
            g_uconns[i].pid  = pid;
            return i;
        }
    }
    return -1;
}

static struct user_conn *get_conn(int id, int pid) {
    if (id < 0 || id >= MAX_USER_CONNS) return 0;
    struct user_conn *c = &g_uconns[id];
    /* A reserved slot has no connection behind it yet, so it is not a
     * usable descriptor either. */
    if (c->type == CONN_NONE || c->type == CONN_RESERVED || c->pid != pid)
        return 0;
    return c;
}

/* ---- TCP syscalls ---- */

long sys_tcp_user_connect(uint32_t ip_be, uint16_t port_be, uint32_t timeout_ms) {
    int pid = current_proc()->pid;
    int slot = alloc_conn(pid);
    if (slot < 0) return -11; /* EAGAIN */

    if (timeout_ms == 0) timeout_ms = 5000;

    struct tcp_conn *tc = tcp_connect(ip_be, port_be, timeout_ms);
    if (!tc) {
        g_uconns[slot].type = CONN_NONE;
        return -5; /* EIO */
    }

    g_uconns[slot].type = CONN_TCP;
    g_uconns[slot].tcp = tc;
    return slot;
}

long sys_tcp_user_send(int conn_id, const void *buf, uint32_t len) {
    int pid = current_proc()->pid;
    struct user_conn *c = get_conn(conn_id, pid);
    if (!c || c->type != CONN_TCP) return -9; /* EBADF */
    if (!buf || len == 0) return 0;

    return tcp_send(c->tcp, buf, len);
}

long sys_tcp_user_recv(int conn_id, void *buf, uint32_t len) {
    int pid = current_proc()->pid;
    struct user_conn *c = get_conn(conn_id, pid);
    if (!c || c->type != CONN_TCP) return -9; /* EBADF */
    if (!buf || len == 0) return 0;

    return tcp_recv(c->tcp, buf, len, 10000);
}

long sys_tcp_user_close(int conn_id) {
    int pid = current_proc()->pid;
    struct user_conn *c = get_conn(conn_id, pid);
    if (!c) return -9; /* EBADF */

    if (c->type == CONN_TCP || c->type == CONN_LISTEN) {
        tcp_close(c->tcp);
    } else if (c->type == CONN_TLS) {
        tls_close(c->tls);
    }

    memset(c, 0, sizeof(*c));
    return 0;
}

long sys_tcp_user_listen(uint16_t port_be, int backlog) {
    int pid = current_proc()->pid;
    int slot = alloc_conn(pid);
    if (slot < 0) return -11;

    struct tcp_conn *lc = tcp_listen(port_be, backlog);
    if (!lc) {
        g_uconns[slot].type = CONN_NONE;
        return -5;
    }

    g_uconns[slot].type = CONN_LISTEN;
    g_uconns[slot].tcp = lc;
    return slot;
}

long sys_tcp_user_accept(int listen_id) {
    int pid = current_proc()->pid;
    struct user_conn *c = get_conn(listen_id, pid);
    if (!c || c->type != CONN_LISTEN) return -9;

    int slot = alloc_conn(pid);
    if (slot < 0) return -11;

    struct tcp_conn *ac = tcp_accept(c->tcp, 30000);
    if (!ac) {
        g_uconns[slot].type = CONN_NONE;
        return -5;
    }

    g_uconns[slot].type = CONN_TCP;
    g_uconns[slot].tcp = ac;
    return slot;
}

/* ---- TLS syscalls ---- */

long sys_tls_user_connect(uint32_t ip_be, uint16_t port_be, const char *hostname) {
    int pid = current_proc()->pid;
    int slot = alloc_conn(pid);
    if (slot < 0) return -11;

    int err = 0;
    struct tls_conn *tc = tls_connect(ip_be, port_be, hostname, 10000, 0, &err);
    if (!tc) {
        g_uconns[slot].type = CONN_NONE;
        return -5;
    }

    g_uconns[slot].type = CONN_TLS;
    g_uconns[slot].tls = tc;
    return slot;
}

long sys_tls_user_send(int tls_id, const void *buf, uint32_t len) {
    int pid = current_proc()->pid;
    struct user_conn *c = get_conn(tls_id, pid);
    if (!c || c->type != CONN_TLS) return -9;
    if (!buf || len == 0) return 0;

    return tls_send(c->tls, buf, len);
}

long sys_tls_user_recv(int tls_id, void *buf, uint32_t len) {
    int pid = current_proc()->pid;
    struct user_conn *c = get_conn(tls_id, pid);
    if (!c || c->type != CONN_TLS) return -9;
    if (!buf || len == 0) return 0;

    return tls_recv(c->tls, buf, len, 10000);
}

long sys_tls_user_close(int tls_id) {
    return sys_tcp_user_close(tls_id);
}
