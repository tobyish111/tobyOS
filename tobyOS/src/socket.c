/* socket.c -- UDP socket pool + recv-queue + sendto/recvfrom.
 *
 * Sockets live in a fixed-size pool (SOCK_MAX). The state machine is
 * deliberately minimal: in_use {true,false}, with bound = local_port
 * != 0. Receive queue is a bounded ring of struct sock_dgram; when
 * full we drop the oldest dgram (head++) so a slow consumer doesn't
 * permanently lose every new packet. udp_recv reaches in via
 * sock_deliver() (a private extern) to enqueue.
 *
 * Wait queue: sock_recvfrom uses the same wq_add / wq_wake_all
 * pattern pipes use, so it cooperates with milestone-8 signal
 * delivery -- a SIGINT cleanly unblocks a hung recvfrom and the
 * syscall path then exits the process.
 */

#include <tobyos/socket.h>
#include <tobyos/udp.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/cap.h>

static struct sock g_socks[SOCK_MAX];
static uint16_t    g_next_ephemeral = 33000;     /* network byte order */

/* ---- wait-queue helpers (mirrors pipe.c) ----------------------- */

static void wq_add(struct proc **head, struct proc *p) {
    p->next_wait = *head;
    p->wait_head = head;
    *head = p;
}

static void wq_wake_all(struct proc **head) {
    struct proc *p = *head;
    *head = 0;
    while (p) {
        struct proc *next = p->next_wait;
        p->next_wait = 0;
        p->wait_head = 0;
        if (p->state == PROC_BLOCKED) {
            p->state = PROC_READY;
            sched_enqueue(p);
        }
        p = next;
    }
}

/* ---- pool ------------------------------------------------------- */

void sock_init(void) {
    memset(g_socks, 0, sizeof(g_socks));
}

struct sock *sock_alloc(int kind) {
    if (kind != SOCK_KIND_UDP) return 0;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!g_socks[i].in_use) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].in_use = true;
            g_socks[i].kind   = SOCK_KIND_UDP;
            return &g_socks[i];
        }
    }
    return 0;
}

void sock_close(struct sock *s) {
    if (!s) return;
    /* Wake every blocked recvfrom so they return -EINTR-ish. We mark
     * the socket free first so the wakers don't try to re-bind. */
    s->in_use = false;
    wq_wake_all(&s->wq_recv);
    for (int i = 0; i < SOCK_RX_DGRAMS; i++) {
        if (s->dgrams[i].payload) {
            kfree(s->dgrams[i].payload);
            s->dgrams[i].payload = 0;
        }
    }
    memset(s, 0, sizeof(*s));
}

/* Find a socket by bound local port. Returns NULL if none matches. */
struct sock *sock_lookup_by_port(uint16_t dst_port_be) {
    if (dst_port_be == 0) return 0;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].in_use && g_socks[i].local_port == dst_port_be)
            return &g_socks[i];
    }
    return 0;
}

int sock_bind(struct sock *s, uint16_t port_be) {
    if (!s || !s->in_use) return -1;
    if (port_be == 0)     return -1;
    if (sock_lookup_by_port(port_be)) return -1;
    s->local_port = port_be;
    return 0;
}

static int sock_bind_ephemeral(struct sock *s) {
    for (int tries = 0; tries < 1000; tries++) {
        uint16_t p = g_next_ephemeral++;
        if (g_next_ephemeral >= 34000) g_next_ephemeral = 33000;
        uint16_t pbe = htons(p);
        if (!sock_lookup_by_port(pbe)) {
            s->local_port = pbe;
            return 0;
        }
    }
    return -1;
}

/* ---- delivery (called from udp_recv via extern) ---------------- */

void sock_deliver(struct sock *s,
                  uint32_t src_ip_be, uint16_t src_port_be,
                  const void *payload, size_t len) {
    if (!s || !s->in_use) return;
    if (len > ETH_MTU) len = ETH_MTU;

    if (s->count == SOCK_RX_DGRAMS) {
        /* Queue full -- drop the oldest to make room. Counts the
         * lifetime drops for `netstat`-style diagnostics. */
        struct sock_dgram *old = &s->dgrams[s->tail];
        if (old->payload) { kfree(old->payload); old->payload = 0; }
        s->tail = (uint8_t)((s->tail + 1) % SOCK_RX_DGRAMS);
        s->count--;
        s->dropped++;
    }

    uint8_t *copy = (uint8_t *)kmalloc(len ? len : 1);
    if (!copy) return;
    if (len) memcpy(copy, payload, len);

    struct sock_dgram *d = &s->dgrams[s->head];
    d->src_ip   = src_ip_be;
    d->src_port = src_port_be;
    d->len      = (uint16_t)len;
    d->payload  = copy;
    s->head = (uint8_t)((s->head + 1) % SOCK_RX_DGRAMS);
    s->count++;

    wq_wake_all(&s->wq_recv);
}

/* ---- syscall surface ------------------------------------------- */

long sock_sendto(struct sock *s, const void *buf, size_t len,
                 uint32_t dst_ip_be, uint16_t dst_port_be) {
    /* Defence-in-depth (milestone 18): the syscall layer already gates
     * on CAP_NET, but a socket fd could in principle reach a less-
     * privileged child via a future fd-passing mechanism. Re-check
     * here so the wire never sees a packet from a proc lacking the
     * capability. The kernel itself (NULL current_proc) always passes. */
    if (!cap_check(current_proc(), CAP_NET, "sock_sendto")) return -1;
    if (!s || !s->in_use)  return -1;
    if (!buf && len)       return -1;
    if (dst_port_be == 0)  return -1;
    if (len > ETH_MTU - 28 /* IP+UDP */) return -1;

    if (s->local_port == 0) {
        if (sock_bind_ephemeral(s) != 0) return -1;
    }

    if (!net_is_up()) return -4;

    if (!udp_send(s->local_port, dst_ip_be, dst_port_be, buf, len)) {
        return -3;
    }
    return (long)len;
}

long sock_recvfrom(struct sock *s, void *buf, size_t n,
                   uint32_t *src_ip_be, uint16_t *src_port_be) {
    if (!cap_check(current_proc(), CAP_NET, "sock_recvfrom")) return -1;
    if (!s || !s->in_use)  return -1;
    if (!buf && n)         return -1;

    while (s->count == 0) {
        struct proc *self = current_proc();
        if (self->pending_signals) return EINTR_RET;
        wq_add(&s->wq_recv, self);
        self->state = PROC_BLOCKED;
        sched_yield();
        if (self->pending_signals) return EINTR_RET;
        if (!s->in_use)            return -1;   /* closed under us */
    }

    struct sock_dgram *d = &s->dgrams[s->tail];
    size_t copy = d->len < n ? d->len : n;
    if (copy && d->payload) memcpy(buf, d->payload, copy);

    if (src_ip_be)   *src_ip_be   = d->src_ip;
    if (src_port_be) *src_port_be = d->src_port;

    if (d->payload) { kfree(d->payload); d->payload = 0; }
    s->tail = (uint8_t)((s->tail + 1) % SOCK_RX_DGRAMS);
    s->count--;

    return (long)copy;
}

/* ---- diagnostics ----------------------------------------------- */

void sock_dump(void) {
    kprintf("sockets:\n");
    int n = 0;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!g_socks[i].in_use) continue;
        kprintf("  [%d]  port=%u  queued=%u  dropped=%u\n",
                i, (unsigned)ntohs(g_socks[i].local_port),
                (unsigned)g_socks[i].count,
                (unsigned)g_socks[i].dropped);
        n++;
    }
    if (n == 0) kprintf("  (none)\n");
}
