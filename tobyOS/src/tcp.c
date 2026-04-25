/* tcp.c -- minimal RFC 793 TCP, client side only.
 *
 * Architectural notes
 * -------------------
 * Connection table:
 *   A fixed array of TCP_MAX_CONNS slots, looked up on every RX by
 *   the (remote_ip, remote_port, local_port) 3-tuple. Local IP is
 *   implicitly g_my_ip and never varies.
 *
 * Sequence-number arithmetic:
 *   All seq/ack values are stored in HOST byte order inside the conn
 *   slot and converted on the wire. Comparisons use the standard
 *   modular trick `(int32_t)(a - b) > 0` so a 32-bit wrap is handled
 *   correctly (irrelevant for our connection sizes, but easy to
 *   write and good hygiene).
 *
 * Send path:
 *   tcp_send_data_segment() builds a 20-byte header + up to N bytes
 *   of payload, computes the pseudo-header checksum, hands to
 *   ip_send. We immediately stash a copy of the segment in the
 *   conn's tx_buf for retransmission, recording snd_nxt before the
 *   bump and the timestamp.
 *
 * RX path -- IRQ context!
 *   tcp_recv_packet() runs from inside the NIC IRQ handler via
 *   eth_recv -> ip_recv. It MUST stay short, MUST NOT block, and
 *   MUST NOT call any kprintf-on-error path that could re-enter the
 *   network stack. We touch only the conn slot and (for ACK/FIN)
 *   send a single reply segment via ip_send (which is re-entrant
 *   because eth_send doesn't take any locks the RX path holds).
 *
 * Polling:
 *   Synchronous callers (connect/send/recv/close) use the same
 *   nic.rx_drain + sti+hlt loop the resolver uses, plus a periodic
 *   tcp_tick() to drive retransmission while we wait. */

#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/ip.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- compile-time policy --------------------------------------- */

#define TCP_MAX_CONNS         8
#define TCP_RX_BUF_BYTES   8192   /* per-conn receive ring         */
#define TCP_TX_BUF_BYTES   2048   /* per-conn unacked-send buffer  */
#define TCP_DEFAULT_MSS    1460   /* 1500 MTU - 20 IP - 20 TCP     */
#define TCP_RETX_MS         500   /* retransmit every 500 ms       */
#define TCP_RETX_LIMIT        5   /* give up after 5 attempts      */
#define TCP_EPHEMERAL_LO  49152
#define TCP_EPHEMERAL_HI  65535

/* ---- conn table ------------------------------------------------ */

struct tcp_conn {
    bool         in_use;
    tcp_state_t  state;

    uint32_t     remote_ip_be;
    uint16_t     remote_port_be;
    uint16_t     local_port_be;

    /* RFC 793 send-side variables (HOST byte order). */
    uint32_t     snd_una;        /* oldest seqno we still expect ACKed   */
    uint32_t     snd_nxt;        /* next seqno we will assign            */
    uint16_t     snd_wnd;        /* peer's advertised window             */

    /* RFC 793 receive-side variables. */
    uint32_t     rcv_nxt;        /* next seqno we expect from peer       */

    /* RX byte ring -- in-order data only. Producer = tcp_recv_packet
     * (IRQ), consumer = tcp_recv (poll thread). With at most one
     * producer and one consumer on the same CPU we don't need a lock;
     * IRQs are masked while we drain in tcp_recv. */
    uint8_t      rx_buf[TCP_RX_BUF_BYTES];
    size_t       rx_head;        /* next byte to write                   */
    size_t       rx_tail;        /* next byte to read                    */
    size_t       rx_count;

    /* Last unacked TX segment. We support exactly one outstanding
     * segment for simplicity: tcp_send returns only after the
     * segment is fully ACKed. Empty tx_pending_len => nothing
     * outstanding. */
    uint8_t      tx_pending[TCP_TX_BUF_BYTES];
    size_t       tx_pending_len;
    uint32_t     tx_pending_seq;
    uint8_t      tx_pending_flags;
    uint64_t     tx_pending_sent_at;
    int          tx_pending_retries;

    /* Sticky flags set from IRQ context, consumed by poll loops. */
    bool         remote_fin_seen;
    bool         remote_rst_seen;
    bool         peer_acked_our_fin;   /* set when ACK == snd_nxt after we sent FIN */
};

static struct tcp_conn g_conns[TCP_MAX_CONNS];
static uint16_t g_eph_next = TCP_EPHEMERAL_LO;

bool tcp_init(void) {
    memset(g_conns, 0, sizeof(g_conns));
    g_eph_next = TCP_EPHEMERAL_LO;
    return true;
}

/* ---- helpers --------------------------------------------------- */

static struct tcp_conn *conn_alloc(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!g_conns[i].in_use) {
            memset(&g_conns[i], 0, sizeof(g_conns[i]));
            g_conns[i].in_use = true;
            g_conns[i].state  = TCP_CLOSED;
            g_conns[i].snd_wnd = 65535;
            return &g_conns[i];
        }
    }
    return NULL;
}

static void conn_free(struct tcp_conn *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
}

/* Look up a slot whose 3-tuple matches the just-arrived packet. */
static struct tcp_conn *conn_lookup(uint32_t remote_ip_be,
                                    uint16_t remote_port_be,
                                    uint16_t local_port_be) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->in_use) continue;
        if (c->remote_ip_be   != remote_ip_be)   continue;
        if (c->remote_port_be != remote_port_be) continue;
        if (c->local_port_be  != local_port_be)  continue;
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

static uint16_t alloc_ephemeral_port(void) {
    for (int tries = 0; tries < (TCP_EPHEMERAL_HI - TCP_EPHEMERAL_LO + 1); tries++) {
        uint16_t p = g_eph_next++;
        if (g_eph_next > TCP_EPHEMERAL_HI) g_eph_next = TCP_EPHEMERAL_LO;
        uint16_t p_be = htons(p);
        if (!port_in_use(p_be)) return p_be;
    }
    return 0;
}

/* Helpers to push/pop the RX ring. */
static void rx_push(struct tcp_conn *c, const uint8_t *data, size_t n) {
    /* Drop overflow rather than wrap and corrupt. A real impl would
     * advertise window=0 to push back; we never get close enough to
     * the limit to need that for HTTP-sized responses. */
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

/* ---- segment send ---------------------------------------------- */

/* Build and emit one TCP segment. Caller provides:
 *   c            - the connection
 *   flags        - any combination of TCP_FLAG_* (we always OR ACK in if rcv_nxt is established)
 *   payload, plen - data to include after the header (may be 0/NULL)
 *
 * Does NOT touch c->snd_nxt or c->tx_pending; tcp_send_data_segment
 * does that wrapping work. */
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
    h->data_off = (uint8_t)(5u << 4);            /* 5*4 = 20 bytes header */
    h->flags    = flags;
    h->window   = htons((uint16_t)(TCP_RX_BUF_BYTES - c->rx_count));
    h->urgent   = 0;
    h->checksum = 0;

    if (plen) memcpy(buf + TCP_HDR_LEN, payload, plen);

    h->checksum = net_l4_checksum(IP_PROTO_TCP, g_my_ip, c->remote_ip_be,
                                  buf, TCP_HDR_LEN + plen);

    return ip_send(c->remote_ip_be, IP_PROTO_TCP, buf, TCP_HDR_LEN + plen);
}

/* Pure ACK (no data, no flags beyond ACK). Used after every received
 * data segment and during handshake completion. */
static void tcp_send_ack(struct tcp_conn *c) {
    (void)tcp_emit(c, TCP_FLAG_ACK, NULL, 0);
}

/* Send `len` bytes as one segment, advance snd_nxt, stash for retx.
 * `flags` lets the caller add SYN/FIN markers (each consumes one
 * sequence number on top of the data length). */
static bool tcp_send_data_segment(struct tcp_conn *c, uint8_t extra_flags,
                                  const void *payload, size_t plen) {
    uint8_t flags = extra_flags;
    if (c->state >= TCP_ESTABLISHED) flags |= TCP_FLAG_ACK;
    if (plen > 0) flags |= TCP_FLAG_PSH;

    if (!tcp_emit(c, flags, payload, plen)) return false;

    /* Stash for retransmission. SYN and FIN each consume 1 seq # in
     * addition to data bytes. */
    if (plen) memcpy(c->tx_pending, payload, plen);
    c->tx_pending_len     = plen;
    c->tx_pending_seq     = c->snd_nxt;
    c->tx_pending_flags   = extra_flags;       /* SYN or FIN we set */
    c->tx_pending_sent_at = pit_ticks();
    c->tx_pending_retries = 0;

    uint32_t consumed = (uint32_t)plen;
    if (extra_flags & TCP_FLAG_SYN) consumed++;
    if (extra_flags & TCP_FLAG_FIN) consumed++;
    c->snd_nxt += consumed;
    return true;
}

/* Re-emit the stashed segment exactly as it was sent. Bumps the
 * retry counter and updates the timestamp; caller decides when to
 * give up. */
static bool tcp_retransmit_pending(struct tcp_conn *c) {
    if (c->tx_pending_len == 0 &&
        (c->tx_pending_flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) == 0) {
        return true;            /* nothing to retransmit */
    }
    uint8_t flags = c->tx_pending_flags;
    if (c->state >= TCP_ESTABLISHED) flags |= TCP_FLAG_ACK;
    if (c->tx_pending_len > 0) flags |= TCP_FLAG_PSH;

    /* tcp_emit uses c->snd_nxt for the SEQ field, but we need to
     * resend with the OLD seqno. Temporarily override. */
    uint32_t saved_nxt = c->snd_nxt;
    c->snd_nxt = c->tx_pending_seq;
    bool ok = tcp_emit(c, flags, c->tx_pending,
                       c->tx_pending_len);
    c->snd_nxt = saved_nxt;
    if (ok) {
        c->tx_pending_sent_at = pit_ticks();
        c->tx_pending_retries++;
    }
    return ok;
}

/* Periodic timer hook called from the various poll loops. If the
 * pending segment hasn't been ACKed within TCP_RETX_MS, retransmit.
 * Returns false if we hit the retry limit (caller should treat the
 * connection as dead). */
static bool tcp_tick(struct tcp_conn *c) {
    if (!c || !c->in_use) return true;
    bool has_pending = (c->tx_pending_len > 0) ||
                       (c->tx_pending_flags & (TCP_FLAG_SYN | TCP_FLAG_FIN));
    if (!has_pending) return true;

    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t age_ticks = pit_ticks() - c->tx_pending_sent_at;
    uint64_t age_ms = (age_ticks * 1000ull) / hz;
    if (age_ms < TCP_RETX_MS) return true;

    if (c->tx_pending_retries >= TCP_RETX_LIMIT) {
        kprintf("[tcp] retx limit hit (lp=%u rp=%u) -- giving up\n",
                (unsigned)ntohs(c->local_port_be),
                (unsigned)ntohs(c->remote_port_be));
        return false;
    }
    return tcp_retransmit_pending(c);
}

/* ---- RX path (IRQ context) ------------------------------------- */

/* Walk the unsigned 32-bit space modularly.  a < b ?  (signed delta) */
static inline int32_t seq_delta(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

void tcp_recv_packet(uint32_t src_ip_be, const void *tcp_packet, size_t len) {
    if (len < TCP_HDR_LEN) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)tcp_packet;

    /* Validate header length. */
    unsigned hlen = tcp_hdr_bytes(h->data_off);
    if (hlen < TCP_HDR_LEN || hlen > len) return;

    /* Verify pseudo-header checksum (mandatory for TCP). */
    if (g_my_ip != 0) {
        if (net_l4_checksum(IP_PROTO_TCP, src_ip_be, g_my_ip,
                            tcp_packet, len) != 0) {
            return;                                       /* corrupt */
        }
    }

    /* Match a connection slot. */
    struct tcp_conn *c = conn_lookup(src_ip_be, h->src_port, h->dst_port);
    if (!c) {
        /* RFC 793: send RST to anyone who knocks on a port we don't
         * own. We don't bother (no LISTEN sockets means there's no
         * useful semantic, and SLIRP rarely delivers stray packets).
         * Drop silently. */
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
        c->state = TCP_CLOSED;
        return;
    }

    /* SYN_SENT: only valid response is SYN+ACK acknowledging our SYN. */
    if (c->state == TCP_SYN_SENT) {
        if ((fl & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
            (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == c->snd_nxt) {
            c->snd_una = ack;
            c->rcv_nxt = seq + 1;        /* SYN consumes one seqno */
            c->state   = TCP_ESTABLISHED;
            /* Clear the pending SYN so retx stops. */
            c->tx_pending_flags  = 0;
            c->tx_pending_len    = 0;
            tcp_send_ack(c);
        }
        return;
    }

    if (fl & TCP_FLAG_ACK) {
        /* Advance snd_una if the peer acknowledged new bytes. */
        if (seq_delta(ack, c->snd_una) > 0 &&
            seq_delta(ack, c->snd_nxt) <= 0) {
            c->snd_una = ack;
            /* Did peer ACK our pending segment in full? */
            uint32_t pend_end = c->tx_pending_seq +
                                (uint32_t)c->tx_pending_len +
                                ((c->tx_pending_flags & TCP_FLAG_FIN) ? 1u : 0u);
            if (seq_delta(ack, pend_end) >= 0) {
                bool was_fin = (c->tx_pending_flags & TCP_FLAG_FIN) != 0;
                c->tx_pending_len    = 0;
                c->tx_pending_flags  = 0;
                if (was_fin) c->peer_acked_our_fin = true;
            }
        }
    }

    /* In-order data delivery. We accept only segments whose seq
     * matches rcv_nxt exactly; out-of-order segments are dropped
     * and we re-ACK rcv_nxt to nudge the peer to retransmit.
     *
     * Segments that don't fit fully into the rx ring are also dropped
     * wholesale -- we MUST NOT advance rcv_nxt past bytes we couldn't
     * buffer, or the stream silently corrupts. The peer will
     * retransmit once we re-ACK with an updated (smaller / zero)
     * window and our application drains enough room. */
    bool need_ack = false;
    if (plen > 0 && seq == c->rcv_nxt) {
        size_t free_space = TCP_RX_BUF_BYTES - c->rx_count;
        if (plen <= free_space) {
            rx_push(c, payload, plen);
            c->rcv_nxt += (uint32_t)plen;
            need_ack = true;
        } else {
            /* Won't fit -- drop it, re-ACK old rcv_nxt with the
             * current (small/zero) window so the peer back-pressures
             * itself. tcp_recv() will send a window update once the
             * application drains. */
            need_ack = true;
        }
    } else if (plen > 0 && seq != c->rcv_nxt) {
        /* Out of order: send a duplicate ACK. */
        need_ack = true;
    }

    if (fl & TCP_FLAG_FIN) {
        /* A FIN counts only when in-order. */
        if (seq + (uint32_t)plen == c->rcv_nxt) {
            c->rcv_nxt += 1;
            c->remote_fin_seen = true;
            need_ack = true;
            switch (c->state) {
            case TCP_ESTABLISHED:  c->state = TCP_CLOSE_WAIT; break;
            case TCP_FIN_WAIT_1:   c->state = TCP_CLOSE_WAIT; break;  /* simultaneous close */
            case TCP_FIN_WAIT_2:   c->state = TCP_CLOSED;     break;
            default: break;
            }
        }
    }

    /* State transitions driven by ACK alone. */
    if (c->peer_acked_our_fin) {
        switch (c->state) {
        case TCP_FIN_WAIT_1:   c->state = TCP_FIN_WAIT_2; break;
        case TCP_LAST_ACK:     c->state = TCP_CLOSED;     break;
        default: break;
        }
        c->peer_acked_our_fin = false;
    }

    if (need_ack) tcp_send_ack(c);
}

/* ---- public API ------------------------------------------------- */

tcp_state_t tcp_state(const struct tcp_conn *c) {
    return c ? c->state : TCP_CLOSED;
}

const char *tcp_state_name(tcp_state_t s) {
    switch (s) {
    case TCP_CLOSED:       return "CLOSED";
    case TCP_SYN_SENT:     return "SYN_SENT";
    case TCP_ESTABLISHED:  return "ESTABLISHED";
    case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
    case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_LAST_ACK:     return "LAST_ACK";
    default:               return "?";
    }
}

/* Polling primitive shared by connect/send/recv/close. Each iteration:
 *   - drains the NIC RX queue,
 *   - runs tcp_tick (retransmission),
 *   - returns the predicate result.
 * Caller passes a deadline in PIT ticks (0 = poll once). */
static int tcp_poll_until(struct tcp_conn *c, uint64_t deadline,
                          int (*predicate)(const struct tcp_conn *)) {
    struct net_dev *nd = net_default();
    for (;;) {
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        int p = predicate(c);
        if (p) return p;
        if (!tcp_tick(c)) return -1;
        if (pit_ticks() >= deadline) return 0;
        sti();
        hlt();
    }
}

/* ---- connect ---------------------------------------------------- */

static int pred_established_or_closed(const struct tcp_conn *c) {
    if (c->state == TCP_ESTABLISHED) return 1;
    if (c->state == TCP_CLOSED)      return -1;     /* RST or retx limit */
    if (c->remote_rst_seen)          return -1;
    return 0;
}

struct tcp_conn *tcp_connect(uint32_t dst_ip_be, uint16_t dst_port_be,
                             uint32_t timeout_ms) {
    if (dst_ip_be == 0) return NULL;
    if (g_my_ip   == 0) return NULL;

    struct tcp_conn *c = conn_alloc();
    if (!c) {
        kprintf("[tcp] connect: out of conn slots\n");
        return NULL;
    }

    uint16_t lp = alloc_ephemeral_port();
    if (lp == 0) { conn_free(c); return NULL; }
    c->local_port_be  = lp;
    c->remote_ip_be   = dst_ip_be;
    c->remote_port_be = dst_port_be;
    c->state          = TCP_SYN_SENT;

    /* Pick an ISN. PIT-mixed; not random in the cryptographic sense
     * but unique per-call which is all the loopback harness needs. */
    uint64_t mix = (uint64_t)pit_ticks() * 0x9E3779B97F4A7C15ull;
    mix ^= ((uint64_t)g_my_mac[3] << 16) |
           ((uint64_t)g_my_mac[4] << 8)  |
           ((uint64_t)g_my_mac[5]);
    c->snd_nxt = (uint32_t)(mix ^ (mix >> 32));
    c->snd_una = c->snd_nxt;

    char ipbuf[16]; net_format_ip(ipbuf, dst_ip_be);
    kprintf("[tcp] connect %s:%u  lp=%u isn=0x%08x\n",
            ipbuf, (unsigned)ntohs(dst_port_be),
            (unsigned)ntohs(lp), (unsigned)c->snd_nxt);

    if (!tcp_send_data_segment(c, TCP_FLAG_SYN, NULL, 0)) {
        kprintf("[tcp] connect: SYN tx failed (ARP miss?)\n");
        conn_free(c);
        return NULL;
    }

    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t deadline = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;
    int r = tcp_poll_until(c, deadline, pred_established_or_closed);

    if (r != 1) {
        if (c->remote_rst_seen) {
            kprintf("[tcp] connect: RST (refused)\n");
        } else {
            kprintf("[tcp] connect: timeout / no SYN+ACK\n");
        }
        conn_free(c);
        return NULL;
    }

    kprintf("[tcp] ESTABLISHED  lp=%u rp=%u\n",
            (unsigned)ntohs(c->local_port_be),
            (unsigned)ntohs(c->remote_port_be));
    return c;
}

/* ---- send ------------------------------------------------------- */

static int pred_segment_acked(const struct tcp_conn *c) {
    if (c->remote_rst_seen)            return -2;
    if (c->state == TCP_CLOSED)        return -2;
    if (c->tx_pending_len == 0 &&
        (c->tx_pending_flags & TCP_FLAG_FIN) == 0) return 1;
    return 0;
}

long tcp_send(struct tcp_conn *c, const void *buf, size_t len) {
    if (!c || !c->in_use)            return -1;
    if (c->state != TCP_ESTABLISHED) return -1;
    if (len == 0)                    return 0;

    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;

    while (remaining > 0) {
        size_t chunk = remaining;
        if (chunk > TCP_DEFAULT_MSS)  chunk = TCP_DEFAULT_MSS;
        if (chunk > TCP_TX_BUF_BYTES) chunk = TCP_TX_BUF_BYTES;

        if (!tcp_send_data_segment(c, 0, p, chunk)) {
            kprintf("[tcp] tcp_send: ip_send failed\n");
            return -1;
        }

        /* Wait for the segment to get acked (or fail).  Generous
         * deadline -- TCP_RETX_LIMIT * TCP_RETX_MS plus headroom. */
        uint64_t deadline = pit_ticks() +
            ((uint64_t)hz * (TCP_RETX_LIMIT + 1) * TCP_RETX_MS) / 1000u;
        int r = tcp_poll_until(c, deadline, pred_segment_acked);
        if (r != 1) {
            if (c->remote_rst_seen) return -2;
            return -3;
        }
        p         += chunk;
        remaining -= chunk;
    }
    return (long)len;
}

/* ---- recv ------------------------------------------------------- */

static int pred_data_or_eof(const struct tcp_conn *c) {
    if (c->rx_count > 0)     return 1;
    if (c->remote_rst_seen)  return -2;
    if (c->state == TCP_CLOSE_WAIT && c->rx_count == 0) return -1;
    if (c->state == TCP_CLOSED) return -1;
    return 0;
}

long tcp_recv(struct tcp_conn *c, void *buf, size_t cap, uint32_t timeout_ms) {
    if (!c || !c->in_use) return -1;
    if (cap == 0) return 0;

    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t deadline = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;
    int r = tcp_poll_until(c, deadline, pred_data_or_eof);

    if (r == -2) return -2;
    if (r == -1) {
        if (c->rx_count > 0) return (long)rx_pop(c, buf, cap);
        return -1;
    }
    if (r ==  0) return 0;                     /* timeout */

    /* Snapshot pre-drain occupancy so we can decide whether to send
     * a window-update ACK after we pop. Without this the peer can
     * stall for streams larger than TCP_RX_BUF_BYTES because it
     * never learns our window has reopened. */
    size_t before = c->rx_count;
    long got = (long)rx_pop(c, buf, cap);

    /* Send a window-update ACK whenever we freed up at least half the
     * buffer or the window had previously closed completely. Cheap
     * insurance; the peer will start sending again immediately. */
    if (got > 0 && c->state == TCP_ESTABLISHED) {
        bool was_full_or_close = (before >= TCP_RX_BUF_BYTES / 2);
        if (was_full_or_close) {
            tcp_send_ack(c);
        }
    }
    return got;
}

/* ---- close ------------------------------------------------------ */

static int pred_closed(const struct tcp_conn *c) {
    if (c->state == TCP_CLOSED) return 1;
    if (c->remote_rst_seen)     return 1;
    return 0;
}

void tcp_close(struct tcp_conn *c) {
    if (!c || !c->in_use) return;

    /* If we're somewhere in the middle of an ESTABLISHED-style state,
     * send a FIN. Pick the right next state. */
    if (c->state == TCP_ESTABLISHED) {
        if (tcp_send_data_segment(c, TCP_FLAG_FIN, NULL, 0)) {
            c->state = TCP_FIN_WAIT_1;
        }
    } else if (c->state == TCP_CLOSE_WAIT) {
        if (tcp_send_data_segment(c, TCP_FLAG_FIN, NULL, 0)) {
            c->state = TCP_LAST_ACK;
        }
    }

    /* If we're already CLOSED or never connected, just free. */
    if (c->state == TCP_CLOSED || c->state == TCP_SYN_SENT) {
        conn_free(c);
        return;
    }

    /* Drive the close to completion (bounded). */
    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t deadline = pit_ticks() + (hz * 2);  /* ~2s */
    (void)tcp_poll_until(c, deadline, pred_closed);

    kprintf("[tcp] CLOSED       lp=%u rp=%u  (final state=%s)\n",
            (unsigned)ntohs(c->local_port_be),
            (unsigned)ntohs(c->remote_port_be),
            tcp_state_name(c->state));
    conn_free(c);
}

/* ---- diagnostic ------------------------------------------------ */

void tcp_dump(void) {
    int n = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        const struct tcp_conn *c = &g_conns[i];
        if (!c->in_use) continue;
        char rip[16]; net_format_ip(rip, c->remote_ip_be);
        kprintf("  tcp[%d]  lp=%u  -> %s:%u  state=%s  rxq=%u  pend=%u\n",
                i,
                (unsigned)ntohs(c->local_port_be),
                rip,
                (unsigned)ntohs(c->remote_port_be),
                tcp_state_name(c->state),
                (unsigned)c->rx_count,
                (unsigned)c->tx_pending_len);
        n++;
    }
    if (n == 0) kprintf("  (no TCP connections)\n");
}
