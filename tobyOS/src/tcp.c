/* tcp.c -- TCP: active + passive open, pipelined TX, RTO, TIME_WAIT. */

#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/ip.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/sched.h>   /* bkl_held/bkl_exit/bkl_enter for the wait loop */

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
#define TCP_MAX_CWND_BYTES  (TCP_DEFAULT_MSS * 64u)

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
    uint32_t     snd_wnd;
    uint32_t     rcv_nxt;
    uint8_t      rx_buf[TCP_RX_BUF_BYTES];
    size_t       rx_head, rx_tail, rx_count;
    struct tx_pend pend[TCP_MAX_TX_PENDING];
    bool         remote_fin_seen;
    bool         remote_rst_seen;
    bool         peer_acked_our_fin;

    /* Retransmission -- Jacobson/Karels (RFC 6298). */
    uint32_t     srtt_us;
    uint32_t     rttvar_us;
    uint32_t     rto_ms;
    uint32_t     retransmit_count;
    uint64_t     last_send_tsc;

    /* Legacy millisecond accessors kept for compatibility. */
    uint32_t     srtt_ms;
    uint32_t     rttvar_ms;

    /* Congestion control -- CUBIC (RFC 8312). */
    uint32_t     cwnd_bytes;
    uint32_t     ssthresh;
    uint32_t     bytes_in_flight;
    int          in_slow_start;
    uint32_t     dup_ack_count;

    /* CUBIC state */
    uint32_t     w_max;           /* cwnd before last loss (in bytes) */
    uint64_t     epoch_start;     /* pit_ticks() at last loss event */
    uint32_t     origin_point;    /* W_max for CUBIC calculation */
    uint32_t     tcp_friendliness_cwnd; /* TCP-friendly cwnd estimate */

    /* Window scaling (RFC 7323) */
    uint8_t      snd_wnd_shift;   /* peer's window scale factor */
    uint8_t      rcv_wnd_shift;   /* our window scale factor (advertised) */
    bool         wscale_ok;       /* both sides support window scaling */

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
            c->in_use        = true;
            c->state         = TCP_CLOSED;
            c->snd_wnd       = 65535u;
            c->parent_lsn    = -1;
            c->rto_ms        = 1000;
            c->cwnd_bytes    = TCP_INIT_CWND_BYTES;
            c->ssthresh      = TCP_MAX_CWND_BYTES;
            c->in_slow_start = 1;
            c->w_max         = 0;
            c->epoch_start   = 0;
            c->origin_point  = 0;
            c->tcp_friendliness_cwnd = 0;
            c->snd_wnd_shift = 0;
            c->rcv_wnd_shift = 4;  /* advertise shift=4 => up to 1 MiB */
            c->wscale_ok     = false;
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

/* Jacobson/Karels RTT estimator (RFC 6298) -- microsecond precision. */
void tcp_rtt_update(struct tcp_conn *c, uint32_t measured_rtt_ms) {
    uint32_t rtt_us = measured_rtt_ms * 1000u;
    if (rtt_us == 0) rtt_us = 1000;

    if (c->srtt_us == 0) {
        c->srtt_us   = rtt_us;
        c->rttvar_us = rtt_us / 2u;
    } else {
        int32_t delta = (int32_t)rtt_us - (int32_t)c->srtt_us;
        uint32_t abs_delta = delta < 0 ? (uint32_t)(-delta) : (uint32_t)delta;
        c->rttvar_us = (3u * c->rttvar_us + abs_delta) / 4u;
        c->srtt_us   = (7u * c->srtt_us + rtt_us) / 8u;
    }

    c->srtt_ms   = c->srtt_us / 1000u;
    c->rttvar_ms = c->rttvar_us / 1000u;

    uint32_t rto_us = c->srtt_us + 4u * c->rttvar_us;
    uint32_t r = rto_us / 1000u;
    if (r < TCP_MIN_RTO_MS) r = TCP_MIN_RTO_MS;
    if (r > TCP_MAX_RTO_MS) r = TCP_MAX_RTO_MS;
    c->rto_ms = r;
}

/* Legacy alias so old callers keep working. */
static void rto_update_on_ack(struct tcp_conn *c, uint32_t age_ms) {
    tcp_rtt_update(c, age_ms);
}

/* Drop TX slots fully covered by `ack` (host order). Sample RTT from oldest. */
static void pend_ack(struct tcp_conn *c, uint32_t ack) {
    bool sampled = false;
    uint32_t bytes_acked = 0;
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
            bytes_acked += (uint32_t)p->len + extra;
            if (p->xflags & TCP_FLAG_FIN) c->peer_acked_our_fin = true;
            memset(p, 0, sizeof(*p));
        }
    }
    if (seq_delta(ack, c->snd_una) > 0) {
        c->snd_una = ack;
        c->dup_ack_count = 0;
        c->retransmit_count = 0;
    }
    if (bytes_acked > 0)
        tcp_congestion_on_ack(c, bytes_acked);
}

static bool tcp_emit(struct tcp_conn *c, uint8_t flags,
                      const void *payload, size_t plen) {
    uint8_t buf[TCP_HDR_LEN + 4 + TCP_DEFAULT_MSS]; /* +4 for options */
    if (plen > TCP_DEFAULT_MSS) return false;

    /* Include window scale option on SYN segments */
    unsigned opt_len = 0;
    if (flags & TCP_FLAG_SYN) opt_len = 4; /* Kind=3, Len=3, Shift, Pad=NOP */

    unsigned hdr_len = TCP_HDR_LEN + opt_len;

    struct tcp_hdr *h = (struct tcp_hdr *)buf;
    memset(h, 0, hdr_len);
    h->src_port = c->local_port_be;
    h->dst_port = c->remote_port_be;
    h->seq      = htonl(c->snd_nxt);
    h->ack      = htonl(c->rcv_nxt);
    h->data_off = (uint8_t)((hdr_len / 4u) << 4);
    h->flags    = flags;

    /* Advertise receive window (shifted if wscale negotiated) */
    uint16_t adv_wnd = (uint16_t)(TCP_RX_BUF_BYTES - c->rx_count);
    if (c->wscale_ok && !(flags & TCP_FLAG_SYN))
        adv_wnd = (uint16_t)((TCP_RX_BUF_BYTES - c->rx_count) >> c->rcv_wnd_shift);
    h->window   = htons(adv_wnd);
    h->urgent   = 0;
    h->checksum = 0;

    if (opt_len > 0) {
        uint8_t *opts = buf + TCP_HDR_LEN;
        opts[0] = 1;               /* NOP (padding) */
        opts[1] = 3;               /* Kind = Window Scale */
        opts[2] = 3;               /* Length = 3 */
        opts[3] = c->rcv_wnd_shift; /* Shift count */
    }

    if (plen) memcpy(buf + hdr_len, payload, plen);
    h->checksum = net_l4_checksum(IP_PROTO_TCP, g_my_ip, c->remote_ip_be,
                                   buf, hdr_len + plen);
    return ip_send(c->remote_ip_be, IP_PROTO_TCP, buf, hdr_len + plen);
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
    c->bytes_in_flight += consumed;
    c->last_send_tsc = pit_ticks();
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
        c->retransmit_count++;
        /* Exponential backoff on the RTO. */
        if (c->rto_ms < TCP_MAX_RTO_MS / 2u)
            c->rto_ms *= 2u;
        else
            c->rto_ms = TCP_MAX_RTO_MS;
        tcp_congestion_on_loss(c);
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
        struct tcp_conn *c = &g_conns[i];
        if (!c->in_use) continue;

        if (!tcp_tick_one(c)) {
            kprintf("[tcp] closing tcp[%d] lp=%u after retransmit failure\n",
                    i, (unsigned)ntohs(c->local_port_be));

            c->remote_rst_seen = true;
            c->state = TCP_CLOSED;
            pend_clear(c);
        }
    }
}

static void listen_enqueue(struct tcp_conn *lsn, int child_idx) {
    if (lsn->acc_count >= TCP_LISTEN_BACKLOG) return;
    lsn->acc_q[lsn->acc_tail] = (int8_t)child_idx;
    lsn->acc_tail = (uint8_t)((lsn->acc_tail + 1) % TCP_LISTEN_BACKLOG);
    lsn->acc_count++;
}

static void passive_syn(struct tcp_conn *lsn, uint32_t src_ip,
    uint16_t src_port, uint16_t dst_port, uint32_t seq,
    const void *tcp_packet, unsigned hlen) {
int lidx = conn_index(lsn);

if (syn_recv_count(lidx) >= lsn->backlog_cap) {
kprintf("[tcp] listen backlog full lp=%u\n",
(unsigned)ntohs(dst_port));
return;
}

if (conn_lookup(src_ip, src_port, dst_port)) {
kprintf("[tcp] duplicate SYN ignored lp=%u rp=%u\n",
(unsigned)ntohs(dst_port),
(unsigned)ntohs(src_port));
return;
}

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

    /* Parse TCP options from the incoming SYN for window scale */
    if (hlen > TCP_HDR_LEN && tcp_packet) {
        const uint8_t *opts = (const uint8_t *)tcp_packet + TCP_HDR_LEN;
        unsigned opts_len = hlen - TCP_HDR_LEN;
        for (unsigned oi = 0; oi < opts_len; ) {
            uint8_t kind = opts[oi];
            if (kind == 0) break;
            if (kind == 1) { oi++; continue; }
            if (oi + 1 >= opts_len) break;
            uint8_t olen = opts[oi + 1];
            if (olen < 2 || oi + olen > opts_len) break;
            if (kind == 3 && olen == 3) {
                ch->snd_wnd_shift = opts[oi + 2];
                if (ch->snd_wnd_shift > 14) ch->snd_wnd_shift = 14;
                ch->wscale_ok = true;
            }
            oi += olen;
        }
    }

    kprintf("[tcp] SYN rx tcp[%d] lp=%u rp=%u seq=%u -> SYN_RECEIVED\n",
            conn_index(ch),
            (unsigned)ntohs(dst_port),
            (unsigned)ntohs(src_port),
            (unsigned)seq);
    
    if (!tcp_send_data_segment(ch, TCP_FLAG_SYN, NULL, 0)) {
        kprintf("[tcp] SYN/ACK send failed tcp[%d]\n", conn_index(ch));
        conn_free(ch);
        return;
    }
    
    kprintf("[tcp] SYN/ACK tx tcp[%d] iss=%u ack=%u\n",
            conn_index(ch),
            (unsigned)ch->snd_una,
            (unsigned)ch->rcv_nxt);
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
            passive_syn(lsn, src_ip_be, srcp, dstp, ntohl(h->seq),
                        tcp_packet, hlen);
        }
        return;
    }

    uint32_t seq = ntohl(h->seq);
    uint32_t ack = ntohl(h->ack);
    uint8_t  fl  = h->flags;
    const uint8_t *payload = (const uint8_t *)tcp_packet + hlen;
    size_t        plen     = len - hlen;
    c->snd_wnd = (uint32_t)ntohs(h->window) << c->snd_wnd_shift;

    if (fl & TCP_FLAG_RST) {
        kprintf("[tcp] RST rx tcp[%d] lp=%u rp=%u state=%s\n",
                conn_index(c),
                (unsigned)ntohs(c->local_port_be),
                (unsigned)ntohs(c->remote_port_be),
                tcp_state_name(c->state));
    
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
            /* Parse TCP options for window scale */
            if (hlen > TCP_HDR_LEN) {
                const uint8_t *opts = (const uint8_t *)tcp_packet + TCP_HDR_LEN;
                unsigned opts_len = hlen - TCP_HDR_LEN;
                for (unsigned oi = 0; oi < opts_len; ) {
                    uint8_t kind = opts[oi];
                    if (kind == 0) break;        /* End of options */
                    if (kind == 1) { oi++; continue; } /* NOP */
                    if (oi + 1 >= opts_len) break;
                    uint8_t olen = opts[oi + 1];
                    if (olen < 2 || oi + olen > opts_len) break;
                    if (kind == 3 && olen == 3) {
                        c->snd_wnd_shift = opts[oi + 2];
                        if (c->snd_wnd_shift > 14) c->snd_wnd_shift = 14;
                        c->wscale_ok = true;
                    }
                    oi += olen;
                }
            }
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
    
            kprintf("[tcp] ESTABLISHED passive tcp[%d] lp=%u rp=%u\n",
                    conn_index(c),
                    (unsigned)ntohs(c->local_port_be),
                    (unsigned)ntohs(c->remote_port_be));
    
            if (c->parent_lsn >= 0 && c->parent_lsn < TCP_MAX_CONNS) {
                struct tcp_conn *lsn = &g_conns[c->parent_lsn];
                if (lsn->in_use && lsn->state == TCP_LISTEN)
                    listen_enqueue(lsn, conn_index(c));
            }
    
            tcp_send_ack(c);
        } else {
            kprintf("[tcp] SYN_RECEIVED ignored tcp[%d] fl=0x%02x seq=%u ack=%u expected seq=%u ack=%u\n",
                    conn_index(c),
                    (unsigned)fl,
                    (unsigned)seq,
                    (unsigned)ack,
                    (unsigned)c->rcv_nxt,
                    (unsigned)c->snd_nxt);
        }
        return;
    }

    if (fl & TCP_FLAG_ACK) {
        if (seq_delta(ack, c->snd_una) > 0 &&
            seq_delta(ack, c->snd_nxt) <= 0) {
            pend_ack(c, ack);
        } else if (ack == c->snd_una && plen == 0 &&
                   c->state == TCP_ESTABLISHED) {
            /* Duplicate ACK detection (RFC 5681 sec 2). */
            c->dup_ack_count++;
            if (c->dup_ack_count == 3)
                tcp_fast_retransmit(c);
        }
    }

    bool need_ack = false;
    if (plen > 0 && seq == c->rcv_nxt) {
        size_t free_space = TCP_RX_BUF_BYTES - c->rx_count;
        if (plen <= free_space) {
            rx_push(c, payload, plen);
            c->rcv_nxt += (uint32_t)plen;
            need_ack = true;
    
            kprintf("[tcp] payload rx tcp[%d] len=%u seq=%u new_rcv_nxt=%u\n",
                    conn_index(c),
                    (unsigned)plen,
                    (unsigned)seq,
                    (unsigned)c->rcv_nxt);
        } else {
            kprintf("[tcp] RX buffer full tcp[%d] plen=%u free=%u\n",
                    conn_index(c),
                    (unsigned)plen,
                    (unsigned)free_space);
            need_ack = true;
        }
    } else if (plen > 0 && seq != c->rcv_nxt) {
        kprintf("[tcp] out-of-order payload tcp[%d] len=%u seq=%u expected=%u\n",
                conn_index(c),
                (unsigned)plen,
                (unsigned)seq,
                (unsigned)c->rcv_nxt);
        need_ack = true;
    }

    if (fl & TCP_FLAG_FIN) {
        if (seq + (uint32_t)plen == c->rcv_nxt) {
            c->rcv_nxt += 1u;
            c->remote_fin_seen = true;
            need_ack = true;
            kprintf("[tcp] FIN rx tcp[%d] state=%s seq=%u\n",
                conn_index(c),
                tcp_state_name(c->state),
                (unsigned)seq);
            switch (c->state) {
                case TCP_ESTABLISHED:
                    /*
                     * Peer initiated close. We ACK their FIN and wait for local user
                     * to call tcp_close(), which will send our FIN.
                     */
                    c->state = TCP_CLOSE_WAIT;
                    break;
                
                case TCP_FIN_WAIT_1:
                    /*
                     * Simultaneous close-ish case: we already sent FIN and now peer
                     * sent FIN too. We do not have TCP_CLOSING, so use TIME_WAIT as
                     * the simple hobby-OS fallback.
                     */
                    c->state = TCP_TIME_WAIT;
                    {
                        uint32_t hz = pit_hz();
                        if (hz == 0) hz = 100;
                        c->tw_deadline_tick =
                            pit_ticks() +
                            ((uint64_t)hz * TCP_TW_MSL_MS) / 1000u;
                    }
                    break;
                
                case TCP_FIN_WAIT_2:
                    /*
                     * Normal active close: our FIN was ACKed, then peer sent FIN.
                     */
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
            /*
             * Our FIN was ACKed, but we have not seen peer FIN yet.
             */
            c->state = TCP_FIN_WAIT_2;
            break;
    
        case TCP_LAST_ACK:
            /*
             * Passive close complete:
             * peer sent FIN -> we entered CLOSE_WAIT -> we sent FIN ->
             * peer ACKed our FIN. Done.
             */
            c->state = TCP_CLOSED;
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

        /* Idle until the next interrupt. CRITICAL: drop the big kernel lock
         * across the wait. This loop backs the blocking TCP syscalls
         * (connect/accept/send/recv/close). When one runs on a secondary CPU
         * the syscall holds the BKL, and holding it across this multi-tick
         * wait starves every other core -- in particular pid 0 on the BSP,
         * whose idle loop drives the GUI compositor AND pumps the network RX
         * that would satisfy our predicate. Holding it here was a hard,
         * intermittent full-desktop freeze (the waiter sits in `hlt` with the
         * BKL held while pid 0 spins forever in bkl_enter). Releasing it lets
         * pid 0 run (and advance the network) while we sleep; we re-take it
         * before touching shared net state on the next iteration. The
         * net-state work above all runs under the BKL, so nothing races. */
        bool had_bkl = bkl_held();
        if (had_bkl) bkl_exit();
        sti();
        hlt();
        if (had_bkl) bkl_enter();
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
    
    kprintf("[tcp] LISTEN tcp[%d] lp=%u backlog=%u\n",
            conn_index(c),
            (unsigned)ntohs(local_port_be),
            (unsigned)c->backlog_cap);

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
            /* Drop the BKL across the idle wait, exactly as tcp_poll_until does:
             * holding it across `hlt` deadlocks pid 0 (it spins forever in
             * bkl_enter while we sleep). This linger is reached by the ACTIVE
             * closer (a TCP server's closesocket -> TIME_WAIT); the passive-
             * close path (clients) never enters it, which is why it stayed
             * latent until the C17b winsock server. The net-state work above
             * runs under the BKL, so nothing races. */
            bool had_bkl = bkl_held();
            if (had_bkl) bkl_exit();
            sti();
            hlt();
            if (had_bkl) bkl_enter();
        }
    }
    if (c->in_use) conn_free(c);
}

/* ---- Congestion control -- CUBIC (RFC 8312) --------------------- */

static uint32_t icbrt(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = 1;
    for (int i = 0; i < 20; i++) {
        uint32_t x2 = x * x;
        if (x2 == 0) break;
        uint32_t x3 = (2 * x + n / x2) / 3;
        if (x3 >= x) break;
        x = x3;
    }
    return x;
}

void tcp_congestion_on_ack(struct tcp_conn *c, uint32_t bytes_acked) {
    if (!c || !bytes_acked) return;

    if (c->bytes_in_flight >= bytes_acked)
        c->bytes_in_flight -= bytes_acked;
    else
        c->bytes_in_flight = 0;

    if (c->in_slow_start) {
        c->cwnd_bytes += bytes_acked;
        if (c->cwnd_bytes >= c->ssthresh) {
            c->in_slow_start = 0;
            c->epoch_start = pit_ticks();
        }
        return;
    }

    /* CUBIC congestion avoidance */
    uint64_t now = pit_ticks();
    if (c->epoch_start == 0) c->epoch_start = now;

    /* Time since epoch in milliseconds (PIT at ~1000 Hz) */
    uint64_t elapsed_ticks = now - c->epoch_start;
    uint32_t elapsed_ms = (uint32_t)(elapsed_ticks);

    /* K = cubic_root(w_max * 0.3 / 0.4) in segments, converted to ms */
    uint32_t w_max_segs = c->w_max / TCP_DEFAULT_MSS;
    if (w_max_segs == 0) w_max_segs = 1;

    uint32_t k_input = (w_max_segs * 3) / 4; /* w_max * beta_cubic / C */
    uint32_t k_ms = icbrt(k_input) * 100;

    /* W_cubic(t) = C * (t - K)^3 + W_max */
    int32_t diff = (int32_t)elapsed_ms - (int32_t)k_ms;
    int64_t cubic_term = (int64_t)diff * diff * diff;
    /* C = 0.4, in fixed point: multiply by 4, divide by 10 */
    int64_t w_cubic_segs = (int64_t)w_max_segs + (cubic_term * 4 / (10 * 1000 * 1000));
    if (w_cubic_segs < 1) w_cubic_segs = 1;

    uint32_t w_cubic = (uint32_t)w_cubic_segs * TCP_DEFAULT_MSS;

    /* TCP-friendly mode: standard Reno increase */
    uint32_t w_reno = c->cwnd_bytes + (TCP_DEFAULT_MSS * bytes_acked) / c->cwnd_bytes;

    /* Use the larger of CUBIC and Reno */
    uint32_t target = (w_cubic > w_reno) ? w_cubic : w_reno;

    /* Cap growth at 1 MSS increase per RTT */
    if (target > c->cwnd_bytes + TCP_DEFAULT_MSS)
        target = c->cwnd_bytes + TCP_DEFAULT_MSS;

    c->cwnd_bytes = target;

    uint32_t max_cwnd = TCP_DEFAULT_MSS * 64;
    if (c->cwnd_bytes > max_cwnd) c->cwnd_bytes = max_cwnd;
}

void tcp_congestion_on_loss(struct tcp_conn *c) {
    if (!c) return;
    c->w_max = c->cwnd_bytes;
    /* CUBIC beta = 0.7 */
    c->cwnd_bytes = (c->cwnd_bytes * 7) / 10;
    if (c->cwnd_bytes < TCP_DEFAULT_MSS) c->cwnd_bytes = TCP_DEFAULT_MSS;
    c->ssthresh = c->cwnd_bytes;
    c->in_slow_start = 0;
    c->epoch_start = pit_ticks();
}

void tcp_retransmit_check(struct tcp_conn *c) {
    if (!c || !c->in_use) return;
    (void)tcp_tick_one(c);
}

void tcp_fast_retransmit(struct tcp_conn *c) {
    if (!c || !c->in_use) return;
    /* Find the oldest unACKed segment and retransmit it. */
    int oldest = -1;
    uint32_t oldest_seq = 0;
    for (int i = 0; i < TCP_MAX_TX_PENDING; i++) {
        struct tx_pend *p = &c->pend[i];
        if (!p->used) continue;
        if (oldest < 0 || seq_delta(p->seq, oldest_seq) < 0) {
            oldest = i;
            oldest_seq = p->seq;
        }
    }
    if (oldest >= 0) {
        kprintf("[tcp] fast retransmit tcp[%d] seq=%u dup_acks=%u\n",
                conn_index(c), oldest_seq, c->dup_ack_count);
        /* CUBIC fast retransmit: beta = 0.7 */
        c->w_max = c->cwnd_bytes;
        c->cwnd_bytes = (c->cwnd_bytes * 7) / 10;
        if (c->cwnd_bytes < TCP_DEFAULT_MSS) c->cwnd_bytes = TCP_DEFAULT_MSS;
        c->ssthresh   = c->cwnd_bytes;
        c->in_slow_start = 0;
        c->epoch_start = pit_ticks();

        tcp_retransmit_slot(c, oldest);
    }
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
