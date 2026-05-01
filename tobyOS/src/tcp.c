/* tcp.c -- TCP: active + passive open, pipelined TX, RTO, TIME_WAIT. */

#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/ip.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define TCP_MAX_CONNS         12
#define TCP_RX_BUF_BYTES    8192
#define TCP_DEFAULT_MSS     1460
#define TCP_MAX_TX_PENDING  4
#define TCP_LISTEN_BACKLOG  4
#define TCP_EPHEMERAL_LO    49152
#define TCP_EPHEMERAL_HI    65535
#define TCP_RETX_LIMIT      8
#define TCP_TW_MSL_MS       2000u
#define TCP_MIN_RTO_MS      200u
#define TCP_MAX_RTO_MS      12000u
#define TCP_INIT_CWND_BYTES (TCP_DEFAULT_MSS * 2u)
#define TCP_MAX_CWND_BYTES  (TCP_DEFAULT_MSS * 8u)

struct tx_pend {
    bool     used;
    uint32_t seq;
    size_t   len;
    uint8_t  xflags;       /* SYN / FIN bits for this segment */
    uint64_t sent_at;
    unsigned retries;
    uint8_t  buf[TCP_DEFAULT_MSS];
};

struct tcp_conn {
    bool         in_use;
    tcp_state_t  state;
    uint32_t     remote_ip_be;
    uint16_t     remote_port_be;
    uint16_t     local_port_be;
    uint32_t     snd_una;
    uint32_t     snd_nxt;
    uint16_t     snd_wnd;
    uint32_t     rcv_nxt;
    uint8_t      rx_buf[TCP_RX_BUF_BYTES];
    size_t       rx_head, rx_tail, rx_count;
    struct tx_pend pend[TCP_MAX_TX_PENDING];
    bool         remote_fin_seen;
    bool         remote_rst_seen;
    bool         peer_acked_our_fin;
    uint32_t     srtt_ms;
    uint32_t     rttvar_ms;
    uint32_t     rto_ms;
    uint32_t     cwnd_bytes;
    uint8_t      acc_head, acc_tail, acc_count, backlog_cap;
    int8_t       acc_q[TCP_LISTEN_BACKLOG];
    int8_t       parent_lsn;
    uint64_t     tw_deadline_tick;
};

static struct tcp_conn g_conns[TCP_MAX_CONNS];
static uint16_t        g_eph_next = TCP_EPHEMERAL_LO;

static int conn_index(const struct tcp_conn *c) {
    return (int)(c - g_conns);
}

bool tcp_init(void) {
    memset(g_conns, 0, sizeof(g_conns));
    g_eph_next = TCP_EPHEMERAL_LO;
    return true;
}

static struct tcp_conn *conn_alloc(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->in_use) {
            memset(c, 0, sizeof(*c));
            c->in_use     = true;
            c->state      = TCP_CLOSED;
            c->snd_wnd    = 65535;
            c->parent_lsn = -1;
            c->rto_ms     = 1000;
            c->cwnd_bytes = TCP_INIT_CWND_BYTES;
            return c;
        }
    }
    return NULL;
}

static void conn_free(struct tcp_conn *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
}

static struct tcp_conn *conn_lookup(uint32_t rip, uint16_t rport,
                                     uint16_t lport) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->in_use) continue;
        if (c->remote_ip_be != rip || c->remote_port_be != rport ||
            c->local_port_be != lport)
            continue;
        return c;
    }
    return NULL;
}

static struct tcp_conn *listen_lookup(uint16_t local_port_be) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (c->in_use && c->state == TCP_LISTEN &&
            c->local_port_be == local_port_be)
            return c;
    }
    return NULL;
}

static bool port_in_use(uint16_t port_be) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (g_conns[i].in_use && g_conns[i].local_port_be == port_be)
            return true;
    }
    return false;
}

static unsigned syn_recv_count(int lsn_idx) {
    unsigned n = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->in_use || c->parent_lsn != lsn_idx) continue;
        if (c->state == TCP_SYN_RECEIVED) n++;
    }
    return n;
}

static uint16_t alloc_ephemeral_port(void) {
    for (int tries = 0; tries < (TCP_EPHEMERAL_HI - TCP_EPHEMERAL_LO + 1);
         tries++) {
        uint16_t p = g_eph_next++;
        if (g_eph_next > TCP_EPHEMERAL_HI) g_eph_next = TCP_EPHEMERAL_LO;
        uint16_t p_be = htons(p);
        if (!port_in_use(p_be)) return p_be;
    }
    return 0;
}

static void rx_push(struct tcp_conn *c, const uint8_t *data, size_t n) {
    while (n > 0 && c->rx_count < TCP_RX_BUF_BYTES) {
        c->rx_buf[c->rx_head] = *data++;
        c->rx_head = (c->rx_head + 1) % TCP_RX_BUF_BYTES;
        c->rx_count++;
        n--;
    }
}

static size_t rx_pop(struct tcp_conn *c, uint8_t *buf, size_t cap) {
    size_t got = 0;
    while (got < cap && c->rx_count > 0) {
        buf[got++] = c->rx_buf[c->rx_tail];
        c->rx_tail = (c->rx_tail + 1) % TCP_RX_BUF_BYTES;
        c->rx_count--;
    }
    return got;
}

static inline int32_t seq_delta(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

static void pend_clear(struct tcp_conn *c) {
    memset(c->pend, 0, sizeof(c->pend));
}

static int pend_alloc(struct tcp_conn *c) {
    for (int i = 0; i < TCP_MAX_TX_PENDING; i++)
        if (!c->pend[i].used) return i;
    return -1;
}

static size_t pend_flight_bytes(const struct tcp_conn *c) {
    size_t sum = 0;
    for (int i = 0; i < TCP_MAX_TX_PENDING; i++) {
        if (!c->pend[i].used) continue;
        uint32_t extra = 0;
        if (c->pend[i].xflags & TCP_FLAG_SYN) extra++;
        if (c->pend[i].xflags & TCP_FLAG_FIN) extra++;
        sum += c->pend[i].len + extra;
    }
    return sum;
}

static void rto_update_on_ack(struct tcp_conn *c, uint32_t age_ms) {
    if (age_ms == 0) age_ms = 1;
    if (c->srtt_ms == 0) {
        c->srtt_ms   = age_ms;
        c->rttvar_ms = age_ms / 2u;
    } else {
        int32_t delta = (int32_t)age_ms - (int32_t)c->srtt_ms;
        if (delta < 0) delta = -delta;
        c->rttvar_ms = (3u * c->rttvar_ms + (uint32_t)delta) / 4u;
        c->srtt_ms   = (7u * c->srtt_ms + age_ms) / 8u;
    }
    uint32_t r = c->srtt_ms + 4u * c->rttvar_ms;
    if (r < TCP_MIN_RTO_MS) r = TCP_MIN_RTO_MS;
    if (r > TCP_MAX_RTO_MS) r = TCP_MAX_RTO_MS;
    c->rto_ms = r;
}

/* Drop TX slots fully covered by `ack` (host order). Sample RTT from oldest. */
static void pend_ack(struct tcp_conn *c, uint32_t ack) {
    bool sampled = false;
    for (int i = 0; i < TCP_MAX_TX_PENDING; i++) {
        struct tx_pend *p = &c->pend[i];
        if (!p->used) continue;
        uint32_t extra = 0;
        if (p->xflags & TCP_FLAG_SYN) extra++;
        if (p->xflags & TCP_FLAG_FIN) extra++;
        uint32_t end = p->seq + (uint32_t)p->len + extra;
        if (seq_delta(ack, end) >= 0) {
            if (!sampled) {
                uint32_t hz = pit_hz();
                if (hz == 0) hz = 100;
                uint64_t age_ms =
                    ((pit_ticks() - p->sent_at) * 1000ull) / (uint64_t)hz;
                rto_update_on_ack(c, (uint32_t)age_ms);
                sampled = true;
            }
            if (p->xflags & TCP_FLAG_FIN) c->peer_acked_our_fin = true;
            memset(p, 0, sizeof(*p));
        }
    }
    if (seq_delta(ack, c->snd_una) > 0) c->snd_una = ack;
    if (c->cwnd_bytes < TCP_MAX_CWND_BYTES)
        c->cwnd_bytes += TCP_DEFAULT_MSS / 2u;
}

static bool tcp_emit(struct tcp_conn *c, uint8_t flags,
                      const void *payload, size_t plen) {
    uint8_t buf[TCP_HDR_LEN + TCP_DEFAULT_MSS];
    if (plen > TCP_DEFAULT_MSS) return false;

    struct tcp_hdr *h = (struct tcp_hdr *)buf;
    memset(h, 0, TCP_HDR_LEN);
    h->src_port = c->local_port_be;
    h->dst_port = c->remote_port_be;
    h->seq      = htonl(c->snd_nxt);
    h->ack      = htonl(c->rcv_nxt);
    h->data_off = (uint8_t)(5u << 4);
    h->flags    = flags;
    h->window   = htons((uint16_t)(TCP_RX_BUF_BYTES - c->rx_count));
    h->urgent   = 0;
    h->checksum = 0;
    if (plen) memcpy(buf + TCP_HDR_LEN, payload, plen);
    h->checksum = net_l4_checksum(IP_PROTO_TCP, g_my_ip, c->remote_ip_be,
                                   buf, TCP_HDR_LEN + plen);
    return ip_send(c->remote_ip_be, IP_PROTO_TCP, buf, TCP_HDR_LEN + plen);
}

static void tcp_send_ack(struct tcp_conn *c) {
    (void)tcp_emit(c, TCP_FLAG_ACK, NULL, 0);
}

static bool tcp_send_data_segment(struct tcp_conn *c, uint8_t xf,
                                  const void *payload, size_t plen) {
    int pi = pend_alloc(c);
    if (pi < 0) return false;

    uint8_t flags = xf;
    if (c->state == TCP_SYN_RECEIVED || c->state >= TCP_ESTABLISHED)
        flags |= TCP_FLAG_ACK;
    if (plen > 0) flags |= TCP_FLAG_PSH;

    if (!tcp_emit(c, flags, payload, plen)) return false;

    struct tx_pend *p = &c->pend[pi];
    p->used    = true;
    p->seq     = c->snd_nxt;
    p->len     = plen;
    p->xflags  = xf;
    p->sent_at = pit_ticks();
    p->retries = 0;
    if (plen) memcpy(p->buf, payload, plen);

    uint32_t consumed = (uint32_t)plen;
    if (xf & TCP_FLAG_SYN) consumed++;
    if (xf & TCP_FLAG_FIN) consumed++;
    c->snd_nxt += consumed;
    return true;
}

static bool tcp_retransmit_slot(struct tcp_conn *c, int pi) {
    struct tx_pend *p = &c->pend[pi];
    if (!p->used) return true;
    uint8_t flags = p->xflags;
    if (c->state == TCP_SYN_RECEIVED || c->state >= TCP_ESTABLISHED)
        flags |= TCP_FLAG_ACK;
    if (p->len > 0) flags |= TCP_FLAG_PSH;

    uint32_t saved = c->snd_nxt;
    c->snd_nxt     = p->seq;
    bool ok        = tcp_emit(c, flags, p->len ? p->buf : NULL, p->len);
    c->snd_nxt     = saved;
    if (ok) {
        p->sent_at = pit_ticks();
        p->retries++;
        if (c->rto_ms < TCP_MAX_RTO_MS / 2u)
            c->rto_ms *= 2u;
        else
            c->rto_ms = TCP_MAX_RTO_MS;
        if (c->cwnd_bytes > TCP_DEFAULT_MSS * 2u)
            c->cwnd_bytes = TCP_DEFAULT_MSS * 2u;
    }
    return ok;
}

static bool tcp_tick_one(struct tcp_conn *c) {
    if (!c || !c->in_use) return true;

    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;

    for (int i = 0; i < TCP_MAX_TX_PENDING; i++) {
        struct tx_pend *p = &c->pend[i];
        if (!p->used) continue;
        uint64_t age_ms = ((pit_ticks() - p->sent_at) * 1000ull) / hz;
        if ((uint32_t)age_ms < c->rto_ms) continue;
        if (p->retries >= TCP_RETX_LIMIT) {
            kprintf("[tcp] retx limit (lp=%u)\n",
                    (unsigned)ntohs(c->local_port_be));
            return false;
        }
        if (!tcp_retransmit_slot(c, i)) return false;
    }
    return true;
}

static void tcp_tick_all(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (g_conns[i].in_use) (void)tcp_tick_one(&g_conns[i]);
    }
}

static void listen_enqueue(struct tcp_conn *lsn, int child_idx) {
    if (lsn->acc_count >= TCP_LISTEN_BACKLOG) return;
    lsn->acc_q[lsn->acc_tail] = (int8_t)child_idx;
    lsn->acc_tail = (uint8_t)((lsn->acc_tail + 1) % TCP_LISTEN_BACKLOG);
    lsn->acc_count++;
}

static void passive_syn(struct tcp_conn *lsn, uint32_t src_ip,
                        uint16_t src_port, uint16_t dst_port, uint32_t seq) {
    int lidx = conn_index(lsn);
    if (syn_recv_count(lidx) >= lsn->backlog_cap) return;
    if (conn_lookup(src_ip, src_port, dst_port)) return;

    struct tcp_conn *ch = conn_alloc();
    if (!ch) return;

    ch->remote_ip_be   = src_ip;
    ch->remote_port_be = src_port;
    ch->local_port_be  = dst_port;
    ch->parent_lsn     = (int8_t)lidx;
    ch->rcv_nxt        = seq + 1u;

    uint64_t mix = (uint64_t)pit_ticks() * 0x9E3779B97F4A7C15ull;
    mix ^= ((uint64_t)g_my_mac[2] << 24) | ((uint64_t)g_my_mac[4] << 8);
    ch->snd_nxt = ch->snd_una = (uint32_t)(mix ^ (mix >> 32));

    ch->state = TCP_SYN_RECEIVED;
    if (!tcp_send_data_segment(ch, TCP_FLAG_SYN, NULL, 0)) {
        conn_free(ch);
        return;
    }
}

void tcp_recv_packet(uint32_t src_ip_be, const void *tcp_packet, size_t len) {
    if (len < TCP_HDR_LEN) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)tcp_packet;
    unsigned hlen = tcp_hdr_bytes(h->data_off);
    if (hlen < TCP_HDR_LEN || hlen > len) return;

    if (g_my_ip != 0) {
        if (net_l4_checksum(IP_PROTO_TCP, src_ip_be, g_my_ip, tcp_packet,
                             len) != 0)
            return;
    }

    uint16_t dstp = h->dst_port;
    uint16_t srcp = h->src_port;

    struct tcp_conn *c = conn_lookup(src_ip_be, srcp, dstp);
    if (!c) {
        struct tcp_conn *lsn = listen_lookup(dstp);
        if (lsn && (h->flags & TCP_FLAG_SYN) &&
            !(h->flags & TCP_FLAG_ACK)) {
            passive_syn(lsn, src_ip_be, srcp, dstp, ntohl(h->seq));
        }
        return;
    }

    uint32_t seq = ntohl(h->seq);
    uint32_t ack = ntohl(h->ack);
    uint8_t  fl  = h->flags;
    const uint8_t *payload = (const uint8_t *)tcp_packet + hlen;
    size_t        plen     = len - hlen;
    c->snd_wnd = ntohs(h->window);

    if (fl & TCP_FLAG_RST) {
        c->remote_rst_seen = true;
        tcp_state_t was = c->state;
        c->state = TCP_CLOSED;
        if (c->parent_lsn >= 0 && was == TCP_SYN_RECEIVED) {
            conn_free(c);
            return;
        }
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((fl & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
                (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == c->snd_nxt) {
            pend_ack(c, ack);
            c->rcv_nxt = seq + 1u;
            c->state   = TCP_ESTABLISHED;
            tcp_send_ack(c);
        }
        return;
    }

    if (c->state == TCP_SYN_RECEIVED) {
        if ((fl & TCP_FLAG_ACK) && !(fl & TCP_FLAG_SYN) &&
            ack == c->snd_nxt && seq == c->rcv_nxt) {
            pend_ack(c, ack);
            c->state = TCP_ESTABLISHED;
            if (c->parent_lsn >= 0 && c->parent_lsn < TCP_MAX_CONNS) {
                struct tcp_conn *lsn = &g_conns[c->parent_lsn];
                if (lsn->in_use && lsn->state == TCP_LISTEN)
                    listen_enqueue(lsn, conn_index(c));
            }
            tcp_send_ack(c);
        }
        return;
    }

    if (fl & TCP_FLAG_ACK) {
        if (seq_delta(ack, c->snd_una) > 0 &&
            seq_delta(ack, c->snd_nxt) <= 0) {
            pend_ack(c, ack);
        }
    }

    bool need_ack = false;
    if (plen > 0 && seq == c->rcv_nxt) {
        size_t free_space = TCP_RX_BUF_BYTES - c->rx_count;
        if (plen <= free_space) {
            rx_push(c, payload, plen);
            c->rcv_nxt += (uint32_t)plen;
            need_ack = true;
        } else {
            need_ack = true;
        }
    } else if (plen > 0 && seq != c->rcv_nxt) {
        need_ack = true;
    }

    if (fl & TCP_FLAG_FIN) {
        if (seq + (uint32_t)plen == c->rcv_nxt) {
            c->rcv_nxt += 1u;
            c->remote_fin_seen = true;
            need_ack = true;
            switch (c->state) {
            case TCP_ESTABLISHED:
                c->state = TCP_CLOSE_WAIT;
                break;
            case TCP_FIN_WAIT_1:
                c->state = TCP_CLOSE_WAIT;
                break;
            case TCP_FIN_WAIT_2:
                c->state = TCP_TIME_WAIT;
                {
                    uint32_t hz = pit_hz();
                    if (hz == 0) hz = 100;
                    c->tw_deadline_tick =
                        pit_ticks() +
                        ((uint64_t)hz * TCP_TW_MSL_MS) / 1000u;
                }
                break;
            default:
                break;
            }
        }
    }

    if (c->peer_acked_our_fin) {
        switch (c->state) {
        case TCP_FIN_WAIT_1:
            c->state = TCP_FIN_WAIT_2;
            break;
        case TCP_LAST_ACK:
            c->state = TCP_TIME_WAIT;
            {
                uint32_t hz = pit_hz();
                if (hz == 0) hz = 100;
                c->tw_deadline_tick =
                    pit_ticks() +
                    ((uint64_t)hz * TCP_TW_MSL_MS) / 1000u;
            }
            break;
        default:
            break;
        }
        c->peer_acked_our_fin = false;
    }

    if (need_ack) tcp_send_ack(c);
}

static int tcp_poll_until(struct tcp_conn *c, uint64_t deadline,
                          int (*pred)(const struct tcp_conn *)) {
    struct net_dev *nd = net_default();
    for (;;) {
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        tcp_tick_all();
        int p = pred(c);
        if (p) return p;
        if (!tcp_tick_one(c)) return -1;
        if (pit_ticks() >= deadline) return 0;
        sti();
        hlt();
    }
}

tcp_state_t tcp_state(const struct tcp_conn *c) {
    return c ? c->state : TCP_CLOSED;
}

const char *tcp_state_name(tcp_state_t s) {
    switch (s) {
    case TCP_CLOSED:
        return "CLOSED";
    case TCP_LISTEN:
        return "LISTEN";
    case TCP_SYN_SENT:
        return "SYN_SENT";
    case TCP_SYN_RECEIVED:
        return "SYN_RCVD";
    case TCP_ESTABLISHED:
        return "ESTABLISHED";
    case TCP_FIN_WAIT_1:
        return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:
        return "FIN_WAIT_2";
    case TCP_CLOSE_WAIT:
        return "CLOSE_WAIT";
    case TCP_LAST_ACK:
        return "LAST_ACK";
    case TCP_TIME_WAIT:
        return "TIME_WAIT";
    default:
        return "?";
    }
}

static int pred_est(const struct tcp_conn *c) {
    if (c->state == TCP_ESTABLISHED) return 1;
    if (c->state == TCP_CLOSED) return -1;
    if (c->remote_rst_seen) return -1;
    return 0;
}

struct tcp_conn *tcp_connect(uint32_t dst_ip_be, uint16_t dst_port_be,
                             uint32_t timeout_ms) {
    if (dst_ip_be == 0 || g_my_ip == 0) return NULL;
    struct tcp_conn *c = conn_alloc();
    if (!c) return NULL;
    uint16_t lp = alloc_ephemeral_port();
    if (lp == 0) {
        conn_free(c);
        return NULL;
    }
    c->local_port_be  = lp;
    c->remote_ip_be   = dst_ip_be;
    c->remote_port_be = dst_port_be;
    c->state          = TCP_SYN_SENT;

    uint64_t mix = (uint64_t)pit_ticks() * 0x9E3779B97F4A7C15ull;
    mix ^= ((uint64_t)g_my_mac[3] << 16) | ((uint64_t)g_my_mac[5]);
    c->snd_nxt = c->snd_una = (uint32_t)(mix ^ (mix >> 32));

    if (!tcp_send_data_segment(c, TCP_FLAG_SYN, NULL, 0)) {
        conn_free(c);
        return NULL;
    }

    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t dl = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;
    if (tcp_poll_until(c, dl, pred_est) != 1) {
        conn_free(c);
        return NULL;
    }
    return c;
}

struct tcp_conn *tcp_listen(uint16_t local_port_be, int backlog) {
    if (g_my_ip == 0) return NULL;
    if (port_in_use(local_port_be)) return NULL;
    struct tcp_conn *c = conn_alloc();
    if (!c) return NULL;
    c->local_port_be = local_port_be;
    c->remote_ip_be  = 0;
    c->remote_port_be = 0;
    c->state         = TCP_LISTEN;
    c->backlog_cap   = (uint8_t)(backlog <= 0 ? 1 : backlog);
    if (c->backlog_cap > TCP_LISTEN_BACKLOG)
        c->backlog_cap = TCP_LISTEN_BACKLOG;
    return c;
}

static int pred_accept(const struct tcp_conn *lsn) {
    return lsn->acc_count > 0 ? 1 : 0;
}

struct tcp_conn *tcp_accept(struct tcp_conn *listener, uint32_t timeout_ms) {
    if (!listener || listener->state != TCP_LISTEN) return NULL;
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t dl = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;
    if (tcp_poll_until(listener, dl, pred_accept) != 1) return NULL;
    int idx = listener->acc_q[listener->acc_head];
    listener->acc_head =
        (uint8_t)((listener->acc_head + 1) % TCP_LISTEN_BACKLOG);
    listener->acc_count--;
    if (idx < 0 || idx >= TCP_MAX_CONNS) return NULL;
    return &g_conns[idx];
}

static int pred_pend_clear(const struct tcp_conn *c) {
    if (!c->in_use) return -2;
    if (c->remote_rst_seen) return -2;
    if (c->state == TCP_CLOSED) return -2;
    for (int i = 0; i < TCP_MAX_TX_PENDING; i++)
        if (c->pend[i].used) return 0;
    return 1;
}

long tcp_send(struct tcp_conn *c, const void *buf, size_t len) {
    if (!c || !c->in_use || c->state != TCP_ESTABLISHED) return -1;
    if (len == 0) return 0;

    const uint8_t *p          = (const uint8_t *)buf;
    size_t         remaining  = len;
    uint32_t       hz         = pit_hz();
    if (hz == 0) hz = 100;

    while (remaining > 0) {
        while (pend_flight_bytes(c) >= c->cwnd_bytes ||
               pend_flight_bytes(c) >= (size_t)c->snd_wnd) {
            uint64_t dl = pit_ticks() + ((uint64_t)hz * (c->rto_ms + 500u)) / 1000u;
            int r = tcp_poll_until(c, dl, pred_pend_clear);
            if (r == -1 || r == -2) {
                if (c->remote_rst_seen) return -2;
                return -3;
            }
            if (r == 0) return -3;
        }

        size_t chunk = remaining;
        if (chunk > TCP_DEFAULT_MSS) chunk = TCP_DEFAULT_MSS;
        if (!tcp_send_data_segment(c, 0, p, chunk)) return -1;
        p += chunk;
        remaining -= chunk;
    }

    uint64_t dl =
        pit_ticks() + ((uint64_t)hz * (TCP_RETX_LIMIT + 2u) * c->rto_ms) / 1000u;
    int r = tcp_poll_until(c, dl, pred_pend_clear);
    if (r != 1) {
        if (c->remote_rst_seen) return -2;
        return -3;
    }
    return (long)len;
}

static int pred_recv(const struct tcp_conn *c) {
    if (c->rx_count > 0) return 1;
    if (c->remote_rst_seen) return -2;
    if (c->state == TCP_CLOSE_WAIT && c->rx_count == 0) return -1;
    if (c->state == TCP_CLOSED) return -1;
    return 0;
}

long tcp_recv(struct tcp_conn *c, void *buf, size_t cap, uint32_t timeout_ms) {
    if (!c || !c->in_use || cap == 0) return -1;
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t dl = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;
    int r = tcp_poll_until(c, dl, pred_recv);
    if (r == -2) return -2;
    if (r == -1) {
        if (c->rx_count > 0) return (long)rx_pop(c, buf, cap);
        return -1;
    }
    if (r == 0) return 0;
    size_t before = c->rx_count;
    long got = (long)rx_pop(c, buf, cap);
    if (got > 0 && c->state == TCP_ESTABLISHED &&
        before >= TCP_RX_BUF_BYTES / 2u)
        tcp_send_ack(c);
    return got;
}

static int pred_closed_basic(const struct tcp_conn *c) {
    if (c->remote_rst_seen) return 1;
    if (c->state == TCP_CLOSED) return 1;
    return 0;
}

void tcp_close(struct tcp_conn *c) {
    if (!c || !c->in_use) return;

    if (c->state == TCP_LISTEN) {
        int myi = conn_index(c);
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            struct tcp_conn *ch = &g_conns[i];
            if (!ch->in_use || ch->parent_lsn != myi) continue;
            conn_free(ch);
        }
        conn_free(c);
        return;
    }

    if (c->state == TCP_ESTABLISHED) {
        if (tcp_send_data_segment(c, TCP_FLAG_FIN, NULL, 0))
            c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        if (tcp_send_data_segment(c, TCP_FLAG_FIN, NULL, 0))
            c->state = TCP_LAST_ACK;
    }

    if (c->state == TCP_CLOSED || c->state == TCP_SYN_SENT) {
        conn_free(c);
        return;
    }

    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t dl = pit_ticks() + hz * 5u;
    (void)tcp_poll_until(c, dl, pred_closed_basic);

    if (c->in_use && c->state == TCP_TIME_WAIT) {
        while (pit_ticks() < c->tw_deadline_tick) {
            struct net_dev *nd = net_default();
            if (nd && nd->rx_drain) nd->rx_drain(nd);
            tcp_tick_all();
            sti();
            hlt();
        }
    }
    if (c->in_use) conn_free(c);
}

void tcp_dump(void) {
    int n = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        const struct tcp_conn *c = &g_conns[i];
        if (!c->in_use) continue;
        char rip[16];
        net_format_ip(rip, c->remote_ip_be);
        kprintf("  tcp[%d]  lp=%u  -> %s:%u  state=%s  rxq=%u  pend=%u\n", i,
                (unsigned)ntohs(c->local_port_be), rip,
                (unsigned)ntohs(c->remote_port_be), tcp_state_name(c->state),
                (unsigned)c->rx_count, (unsigned)pend_flight_bytes(c));
        n++;
    }
    if (n == 0) kprintf("  (no TCP connections)\n");
}
