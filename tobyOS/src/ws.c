/* ws.c -- WebSocket client (RFC 6455) on a ring-0 kernel worker
 * (stage 13). See ws.h for the model and locking rules.
 *
 * Life of a connection:
 *   ws_open()   -- syscall ctx: claim a slot (CONNECTING), lazily
 *                  spawn the worker (proc_create_kernel).
 *   worker      -- owns the transport end to end: TCP (ws://) or
 *                  TLS 1.3 (wss://, cert-validated) connect, the
 *                  HTTP/1.1 Upgrade handshake (Sec-WebSocket-Key /
 *                  -Accept), then a pump: drain the send queue,
 *                  decode incoming frames (unmask, reassemble
 *                  fragments, auto-pong pings) into the message
 *                  queue. Yields its CPU inside tcp waits
 *                  (tcp_yield_wait) so the UI stays live.
 *   ws_poll()   -- syscall ctx: state + head-of-queue message info.
 *   ws_recv()   -- syscall ctx: pop the head message.
 *   ws_send()   -- syscall ctx: mask + frame the message into the
 *                  send queue (pure memory; the worker transmits).
 *   ws_close()  -- syscall ctx: detach the app; the worker sends a
 *                  Close frame and frees the slot.
 *
 * Only the worker ever touches the tcp/tls connection, so a syscall
 * can never race it inside a transport wait; syscalls touch only the
 * slot's queues, in short non-yielding sections under the BKL. The
 * worker recv()s only when the transport reports buffered bytes
 * (tcp_poll_flags / tls_poll_flags) -- an idle connection therefore
 * never sits in a timed-out read that would lose partial-record
 * state, and an idle read can never kill the socket. */

#include <tobyos/ws.h>
#include <tobyos/http.h>       /* http_parse_url + HTTP_ERR_* codes */
#include <tobyos/dns.h>
#include <tobyos/tcp.h>
#include <tobyos/tls.h>
#include <tobyos/net.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/cpu.h>
#include <tobyos/heap.h>
#include <tobyos/rng.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#include <bearssl.h>           /* br_sha1 for Sec-WebSocket-Accept */

#define WS_MAX            4
#define WS_MSG_MAX        (256u << 10)  /* per-message cap */
#define WS_RXQ_MAX        (512u << 10)  /* queued undelivered bytes cap */
#define WS_TXQ_MAX        (128u << 10)  /* queued unsent bytes cap */
#define WS_FRAME_OVERHEAD 14            /* header + ext len + mask */
#define WS_READ_CHUNK     (16u << 10)
#define WS_HDR_CAP        8192          /* Upgrade response header cap */
#define WS_CONNECT_TO_MS  15000
#define WS_RECV_TO_MS     10000         /* only entered with bytes pending */

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* A decoded, complete message queued for the app. */
struct ws_msg {
    struct ws_msg *next;
    uint32_t len;
    uint8_t  op;                /* 1 text, 2 binary */
    uint8_t  data[];
};

struct ws_slot {
    bool     used;
    bool     cancel;            /* app detached (ws_close) */
    bool     close_req;         /* send a Close frame before teardown */
    uint16_t close_req_code;
    int      state;             /* ABI_WS_* */
    int      owner;             /* pid of the requesting process */
    int      err;               /* HTTP_ERR_* when closed abnormally */
    uint16_t close_code;        /* peer's close code (0 = none) */
    bool     peer_closed;       /* peer's Close frame seen */
    char     url[512];

    /* Transport -- WORKER-ONLY after ws_open. */
    struct tcp_conn *tcp;
    struct tls_conn *tls;

    /* Wire-byte accumulator (worker-only): unparsed frame bytes. */
    uint8_t *acc;  uint32_t acc_len, acc_cap;
    /* Fragmented-message assembly (worker-only). */
    uint8_t *frag; uint32_t frag_len; int frag_op;

    /* Decoded message queue (shared: worker appends, syscall pops). */
    struct ws_msg *qh, *qt;
    uint32_t q_bytes;
    /* Outgoing pre-framed bytes (shared: syscall appends, worker
     * steals the whole buffer to transmit). */
    uint8_t *tx;   uint32_t tx_len, tx_cap;
};

static struct ws_slot g_ws[WS_MAX];
static int g_ws_worker_pid = -1;

/* ---- small helpers ---------------------------------------------------- */

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool starts_with_ci(const char *s, const char *pre) {
    while (*pre) {
        if (lc(*s) != lc(*pre)) return false;
        s++; pre++;
    }
    return true;
}

/* klibc has no strtol/strchr; same shape as http.c's local helper. */
static bool parse_dotted_quad(const char *s, uint32_t *out_ip_be) {
    uint32_t parts[4];
    int np = 0;
    uint32_t v = 0;
    bool digit = false;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255) return false;
            digit = true;
        } else if (*s == '.' || *s == 0) {
            if (!digit || np >= 4) return false;
            parts[np++] = v;
            v = 0;
            digit = false;
            if (*s == 0) break;
        } else {
            return false;
        }
    }
    if (np != 4) return false;
    uint8_t *ip = (uint8_t *)out_ip_be;
    for (int i = 0; i < 4; i++) ip[i] = (uint8_t)parts[i];
    return true;
}

static void b64_encode(const uint8_t *src, size_t n, char *dst) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8) | src[i+2];
        dst[o++] = T[(v >> 18) & 63]; dst[o++] = T[(v >> 12) & 63];
        dst[o++] = T[(v >> 6) & 63];  dst[o++] = T[v & 63];
        i += 3;
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        dst[o++] = T[(v >> 18) & 63]; dst[o++] = T[(v >> 12) & 63];
        dst[o++] = '='; dst[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8);
        dst[o++] = T[(v >> 18) & 63]; dst[o++] = T[(v >> 12) & 63];
        dst[o++] = T[(v >> 6) & 63];  dst[o++] = '=';
    }
    dst[o] = 0;
}

/* Find a header's value in a CRLF header block (case-insensitive
 * name). Returns pointer to the value and its trimmed length. */
static const char *hdr_find(const char *blk, uint32_t blen,
                            const char *name, uint32_t *out_len) {
    uint32_t nl = (uint32_t)strlen(name);
    uint32_t i = 0;
    while (i < blen) {
        /* at a line start */
        uint32_t j = i, k = 0;
        while (j < blen && k < nl && lc(blk[j]) == lc(name[k])) { j++; k++; }
        if (k == nl && j < blen && blk[j] == ':') {
            j++;
            while (j < blen && (blk[j] == ' ' || blk[j] == '\t')) j++;
            uint32_t e = j;
            while (e < blen && blk[e] != '\r' && blk[e] != '\n') e++;
            while (e > j && (blk[e-1] == ' ' || blk[e-1] == '\t')) e--;
            *out_len = e - j;
            return blk + j;
        }
        while (i < blen && blk[i] != '\n') i++;
        i++;
    }
    return NULL;
}

/* ---- transport shims (worker context) --------------------------------- */

static long ws_tr_send(struct ws_slot *s, const void *buf, size_t len) {
    if (s->tls) return tls_send(s->tls, buf, len);
    if (s->tcp) return tcp_send(s->tcp, buf, len);
    return -1;
}

/* > 0 bytes; 0 = nothing/timeout; < 0 = connection gone. */
static long ws_tr_recv(struct ws_slot *s, void *buf, size_t cap, uint32_t to) {
    if (s->tls) {
        long n = tls_recv(s->tls, buf, cap, to);
        if (n > 0) return n;
        if (n == 0) return -1;             /* close_notify: clean TLS close */
        if (n == TLS_ERR_RECV) return 0;   /* timeout */
        return -1;
    }
    if (s->tcp) {
        long n = tcp_recv(s->tcp, buf, cap, to);
        if (n > 0) return n;
        if (n == 0) return 0;              /* timeout */
        return -1;                          /* FIN / RST */
    }
    return -1;
}

static int ws_tr_poll(struct ws_slot *s) {
    if (s->tls) return tls_poll_flags(s->tls);
    if (s->tcp) return tcp_poll_flags(s->tcp);
    return TCP_RDY_ERR;
}

/* Abortive teardown: RST + free, never blocks (the 13H lesson --
 * tcp_close's FIN/TIME_WAIT linger would park the worker for seconds
 * and starve every other WebSocket). The RFC close handshake happens
 * at the frame layer above; TCP-level abort after it is harmless. */
static void ws_tr_drop(struct ws_slot *s) {
    if (s->tls) { tls_abort(s->tls); s->tls = NULL; }
    if (s->tcp) { tcp_abort(s->tcp); s->tcp = NULL; }
}

/* ---- frame building ---------------------------------------------------- */

/* Client frames MUST be masked (RFC 6455 5.3). dst needs len +
 * WS_FRAME_OVERHEAD bytes. Returns bytes written. */
static uint32_t ws_frame_build(uint8_t *dst, int op,
                               const uint8_t *payload, uint32_t len) {
    uint32_t p = 0;
    dst[p++] = 0x80u | (uint8_t)(op & 0x0F);        /* FIN + opcode */
    uint8_t mask[4];
    rng_fill(mask, 4);
    if (len < 126) {
        dst[p++] = 0x80u | (uint8_t)len;
    } else if (len < 65536) {
        dst[p++] = 0x80u | 126;
        dst[p++] = (uint8_t)(len >> 8);
        dst[p++] = (uint8_t)len;
    } else {
        dst[p++] = 0x80u | 127;
        for (int i = 7; i >= 0; i--)
            dst[p++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    memcpy(dst + p, mask, 4);
    p += 4;
    for (uint32_t i = 0; i < len; i++)
        dst[p + i] = payload[i] ^ mask[i & 3];
    return p + len;
}

/* Append a framed message to the slot's send queue (BKL, no yields;
 * grow-by-copy since the heap has no krealloc). */
static int ws_tx_queue(struct ws_slot *s, int op,
                       const uint8_t *payload, uint32_t len) {
    uint32_t need = s->tx_len + len + WS_FRAME_OVERHEAD;
    if (need > WS_TXQ_MAX) return -1;
    if (need > s->tx_cap) {
        uint32_t cap = s->tx_cap ? s->tx_cap : 1024;
        while (cap < need) cap *= 2;
        uint8_t *nb = (uint8_t *)kmalloc(cap);
        if (!nb) return -1;
        if (s->tx_len) memcpy(nb, s->tx, s->tx_len);
        if (s->tx) kfree(s->tx);
        s->tx = nb;
        s->tx_cap = cap;
    }
    s->tx_len += ws_frame_build(s->tx + s->tx_len, op, payload, len);
    return 0;
}

/* ---- message queue ------------------------------------------------------ */

static int ws_msg_push(struct ws_slot *s, int op,
                       const uint8_t *data, uint32_t len) {
    if (len > WS_MSG_MAX || s->q_bytes + len > WS_RXQ_MAX) return -1;
    struct ws_msg *m = (struct ws_msg *)kmalloc(sizeof(*m) + len);
    if (!m) return -1;
    m->next = NULL;
    m->len = len;
    m->op = (uint8_t)op;
    if (len) memcpy(m->data, data, len);
    if (s->qt) s->qt->next = m; else s->qh = m;
    s->qt = m;
    s->q_bytes += len;
    return 0;
}

static void ws_queue_free(struct ws_slot *s) {
    struct ws_msg *m = s->qh;
    while (m) {
        struct ws_msg *n = m->next;
        kfree(m);
        m = n;
    }
    s->qh = s->qt = NULL;
    s->q_bytes = 0;
}

/* Free everything a slot owns. Worker calls it after dropping the
 * transport; ws_close calls it directly only once the worker is done
 * with the slot (state CLOSED, transport gone). */
static void slot_free(struct ws_slot *s) {
    ws_queue_free(s);
    if (s->acc)  { kfree(s->acc);  s->acc = NULL; }
    if (s->frag) { kfree(s->frag); s->frag = NULL; }
    if (s->tx)   { kfree(s->tx);   s->tx = NULL; }
    s->used = false;
    s->cancel = false;
}

/* ---- frame parsing (worker) -------------------------------------------- */

/* Mark the connection failed: drop the transport, publish CLOSED. */
static void ws_fail(struct ws_slot *s, int err) {
    ws_tr_drop(s);
    if (s->err == 0) s->err = err;
    s->state = ABI_WS_CLOSED;
}

/* Parse one frame from acc[0..n). Returns bytes consumed, 0 when the
 * frame is still incomplete, -1 on a protocol error (conn failed). */
static long ws_parse_one(struct ws_slot *s, uint8_t *p, uint32_t n) {
    if (n < 2) return 0;
    uint8_t b0 = p[0], b1 = p[1];
    int fin = (b0 & 0x80u) != 0;
    int op  = b0 & 0x0F;
    if (b0 & 0x70u) return -1;             /* RSV: no extensions negotiated */
    int masked = (b1 & 0x80u) != 0;        /* servers must not mask, but cope */
    uint64_t len = b1 & 0x7Fu;
    uint32_t hdr = 2;
    if (len == 126) {
        if (n < 4) return 0;
        len = ((uint64_t)p[2] << 8) | p[3];
        hdr = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
        hdr = 10;
    }
    if (len > WS_MSG_MAX) { ws_fail(s, HTTP_ERR_TOOBIG); return -1; }
    const uint8_t *mask = NULL;
    if (masked) {
        if (n < hdr + 4) return 0;
        mask = p + hdr;
        hdr += 4;
    }
    if ((uint64_t)n < hdr + len) return 0;
    uint8_t *pl = p + hdr;
    if (mask)
        for (uint32_t i = 0; i < (uint32_t)len; i++) pl[i] ^= mask[i & 3];

    if (op & 0x8) {                        /* control frame */
        if (!fin || len > 125) return -1;
        if (op == 0x8) {                   /* Close */
            s->peer_closed = true;
            s->close_code = (len >= 2)
                ? (uint16_t)(((uint16_t)pl[0] << 8) | pl[1]) : 0;
        } else if (op == 0x9) {            /* Ping -> queue Pong */
            if (ws_tx_queue(s, 0xA, pl, (uint32_t)len) != 0) return -1;
        }
        /* 0xA Pong: ignore. */
        return (long)(hdr + len);
    }

    if (op == 0x1 || op == 0x2) {          /* new data message */
        if (s->frag_len || s->frag) return -1;   /* interleaved data */
        if (fin) {
            if (ws_msg_push(s, op, pl, (uint32_t)len) != 0) {
                ws_fail(s, HTTP_ERR_TOOBIG);
                return -1;
            }
        } else {
            s->frag = (uint8_t *)kmalloc(WS_MSG_MAX);
            if (!s->frag) { ws_fail(s, HTTP_ERR_NOMEM); return -1; }
            memcpy(s->frag, pl, (size_t)len);
            s->frag_len = (uint32_t)len;
            s->frag_op = op;
        }
        return (long)(hdr + len);
    }

    if (op == 0x0) {                       /* continuation */
        if (!s->frag) return -1;
        if (s->frag_len + len > WS_MSG_MAX) {
            ws_fail(s, HTTP_ERR_TOOBIG);
            return -1;
        }
        memcpy(s->frag + s->frag_len, pl, (size_t)len);
        s->frag_len += (uint32_t)len;
        if (fin) {
            int rc = ws_msg_push(s, s->frag_op, s->frag, s->frag_len);
            kfree(s->frag);
            s->frag = NULL;
            s->frag_len = 0;
            if (rc != 0) { ws_fail(s, HTTP_ERR_TOOBIG); return -1; }
        }
        return (long)(hdr + len);
    }

    return -1;                             /* reserved opcode */
}

/* Feed freshly received wire bytes through the frame parser. Returns
 * 0 ok, -1 when the connection was failed. */
static int ws_rx_feed(struct ws_slot *s, const uint8_t *buf, uint32_t n) {
    uint32_t need = s->acc_len + n;
    if (need > s->acc_cap) {
        uint32_t cap = s->acc_cap ? s->acc_cap : 4096;
        while (cap < need) cap *= 2;
        if (cap > WS_MSG_MAX + 2 * WS_READ_CHUNK) {
            ws_fail(s, HTTP_ERR_TOOBIG);
            return -1;
        }
        uint8_t *nb = (uint8_t *)kmalloc(cap);
        if (!nb) { ws_fail(s, HTTP_ERR_NOMEM); return -1; }
        if (s->acc_len) memcpy(nb, s->acc, s->acc_len);
        if (s->acc) kfree(s->acc);
        s->acc = nb;
        s->acc_cap = cap;
    }
    memcpy(s->acc + s->acc_len, buf, n);
    s->acc_len += n;

    uint32_t off = 0;
    while (off < s->acc_len && !s->peer_closed) {
        long used = ws_parse_one(s, s->acc + off, s->acc_len - off);
        if (used < 0) {
            if (s->state != ABI_WS_CLOSED)
                ws_fail(s, HTTP_ERR_PROTOCOL);
            return -1;
        }
        if (used == 0) break;
        off += (uint32_t)used;
    }
    if (off) {
        memmove(s->acc, s->acc + off, s->acc_len - off);
        s->acc_len -= off;
    }
    return 0;
}

/* ---- the Upgrade handshake (worker) ------------------------------------ */

static int ws_do_connect(struct ws_slot *s) {
    /* ws:// -> http:// so http_parse_url handles host/port/path. */
    char rew[560];
    int is_tls;
    if (starts_with_ci(s->url, "ws://")) {
        is_tls = 0;
        ksnprintf(rew, sizeof(rew), "http://%s", s->url + 5);
    } else if (starts_with_ci(s->url, "wss://")) {
        is_tls = 1;
        ksnprintf(rew, sizeof(rew), "https://%s", s->url + 6);
    } else {
        return HTTP_ERR_URL;
    }
    struct http_url u;
    if (http_parse_url(rew, &u) != 0) return HTTP_ERR_URL;

    uint32_t ip_be = 0;
    if (!parse_dotted_quad(u.host, &ip_be)) {
        struct dns_result dr;
        if (!dns_resolve(u.host, 1500, &dr)) {
            kprintf("[ws] DNS lookup failed for '%s'\n", u.host);
            return HTTP_ERR_DNS;
        }
        ip_be = dr.ip_be;
    }
    kprintf("[ws] connect %s://%s:%u%s\n",
            is_tls ? "wss" : "ws", u.host, u.port, u.path);

    if (is_tls) {
        int terr = 0;
        s->tls = tls_connect(ip_be, htons(u.port), u.host,
                             WS_CONNECT_TO_MS, 0, &terr);
        if (!s->tls) {
            kprintf("[ws] TLS connect failed: %s\n", tls_strerror(terr));
            return (terr == TLS_ERR_CERT) ? HTTP_ERR_CERT : HTTP_ERR_CONNECT;
        }
    } else {
        s->tcp = tcp_connect(ip_be, htons(u.port), WS_CONNECT_TO_MS);
        if (!s->tcp) return HTTP_ERR_CONNECT;
    }

    /* Sec-WebSocket-Key: base64 of 16 random bytes (RFC 6455 4.1). */
    uint8_t nonce[16];
    rng_fill(nonce, sizeof(nonce));
    char key[25];
    b64_encode(nonce, sizeof(nonce), key);

    char portsuf[8];
    uint16_t defport = is_tls ? 443 : 80;
    if (u.port != defport) ksnprintf(portsuf, sizeof(portsuf), ":%u", u.port);
    else portsuf[0] = 0;

    char req[1024];
    int rl = ksnprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s%s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "Origin: %s://%s%s\r\n"
                       "User-Agent: TobyOS/4.0 (tobyOS x86_64)\r\n"
                       "\r\n",
                       u.path, u.host, portsuf, key,
                       is_tls ? "https" : "http", u.host, portsuf);
    if (rl <= 0 || rl >= (int)sizeof(req)) return HTTP_ERR_URL;
    if (ws_tr_send(s, req, (size_t)rl) < 0) return HTTP_ERR_RESET;

    /* Expected Sec-WebSocket-Accept = b64(SHA1(key + magic GUID)). */
    char accept_exp[32];
    {
        br_sha1_context sc;
        uint8_t dig[20];
        br_sha1_init(&sc);
        br_sha1_update(&sc, key, strlen(key));
        br_sha1_update(&sc, WS_GUID, sizeof(WS_GUID) - 1);
        br_sha1_out(&sc, dig);
        b64_encode(dig, sizeof(dig), accept_exp);
    }

    /* Read the 101 response header block. */
    uint8_t *hb = (uint8_t *)kmalloc(WS_HDR_CAP);
    if (!hb) return HTTP_ERR_NOMEM;
    uint32_t hlen = 0, hend = 0;
    while (!hend) {
        if (hlen >= WS_HDR_CAP - 1) { kfree(hb); return HTTP_ERR_PROTOCOL; }
        long n = ws_tr_recv(s, hb + hlen, WS_HDR_CAP - 1 - hlen,
                            WS_CONNECT_TO_MS);
        if (n <= 0) { kfree(hb); return n == 0 ? HTTP_ERR_TIMEOUT
                                               : HTTP_ERR_RESET; }
        hlen += (uint32_t)n;
        for (uint32_t i = (hlen > (uint32_t)n + 3) ? hlen - (uint32_t)n - 3 : 0;
             i + 3 < hlen; i++)
            if (hb[i] == '\r' && hb[i+1] == '\n' &&
                hb[i+2] == '\r' && hb[i+3] == '\n') {
                hend = i + 4;
                break;
            }
        if (s->cancel) { kfree(hb); return HTTP_ERR_RESET; }
    }

    /* Status line: HTTP/1.x 101. */
    if (hend < 13 || strncmp((char *)hb, "HTTP/1.", 7) != 0 ||
        strncmp((char *)hb + 9, "101", 3) != 0) {
        kprintf("[ws] handshake refused (no 101)\n");
        kfree(hb);
        return HTTP_ERR_PROTOCOL;
    }
    uint32_t al = 0;
    const char *av = hdr_find((char *)hb, hend, "Sec-WebSocket-Accept", &al);
    if (!av || al != strlen(accept_exp) ||
        strncmp(av, accept_exp, al) != 0) {
        kprintf("[ws] bad Sec-WebSocket-Accept\n");
        kfree(hb);
        return HTTP_ERR_PROTOCOL;
    }

    /* Bytes past the header block are already frame data. */
    int rc = 0;
    if (hlen > hend) rc = ws_rx_feed(s, hb + hend, hlen - hend);
    kfree(hb);
    if (rc != 0) return s->err ? s->err : HTTP_ERR_PROTOCOL;

    kprintf("[ws] h%d open %s\n", (int)(s - g_ws), s->url);
    return 0;
}

/* ---- worker ------------------------------------------------------------- */

/* Send a Close frame directly (small, framed on the stack). */
static void ws_send_close_frame(struct ws_slot *s, uint16_t code) {
    if (!s->tcp && !s->tls) return;
    uint8_t pl[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    uint8_t frame[2 + WS_FRAME_OVERHEAD];
    uint32_t n = ws_frame_build(frame, 0x8, pl, 2);
    (void)ws_tr_send(s, frame, n);
}

/* Drain the send queue. Steal the buffer first: tcp/tls sends yield,
 * and a concurrent ws_send must not append into (or reallocate) a
 * buffer we're transmitting from. */
static int ws_tx_drain(struct ws_slot *s) {
    if (!s->tx_len) return 0;
    uint8_t *buf = s->tx;
    uint32_t len = s->tx_len;
    s->tx = NULL;
    s->tx_len = 0;
    s->tx_cap = 0;
    long rc = ws_tr_send(s, buf, len);
    kfree(buf);
    return rc < 0 ? -1 : 0;
}

/* Finish the closing handshake after the peer's Close frame: echo a
 * Close, then RST-drop (no linger). Queued messages stay for the app. */
static void ws_finish_peer_close(struct ws_slot *s) {
    ws_tx_drain(s);
    ws_send_close_frame(s, s->close_code ? s->close_code : 1000);
    ws_tr_drop(s);
    s->state = ABI_WS_CLOSED;
    kprintf("[ws] h%d closed by peer (code %u)\n",
            (int)(s - g_ws), s->close_code);
}

/* One service pass over an OPEN connection. */
static void ws_service(struct ws_slot *s) {
    if (s->peer_closed) { ws_finish_peer_close(s); return; }
    if (ws_tx_drain(s) != 0) { ws_fail(s, HTTP_ERR_RESET); return; }

    /* Receive only when the transport actually has bytes: an idle
     * connection must never enter a timed read (a mid-record timeout
     * would desync TLS framing, and 12D taught us idle reads kill
     * slow-but-alive peers). */
    for (int pass = 0; pass < 8; pass++) {
        if (s->cancel || s->state != ABI_WS_OPEN) return;
        int pf = ws_tr_poll(s);
        if (!(pf & TCP_RDY_RECV)) {
            if (pf & (TCP_RDY_HUP | TCP_RDY_ERR)) {
                /* Transport gone with nothing buffered. */
                if (s->peer_closed) { ws_tr_drop(s); s->state = ABI_WS_CLOSED; }
                else ws_fail(s, HTTP_ERR_RESET);
            }
            return;
        }
        static uint8_t rd[WS_READ_CHUNK];   /* single worker: no reentry */
        long n = ws_tr_recv(s, rd, sizeof(rd), WS_RECV_TO_MS);
        if (n < 0) {
            if (s->peer_closed) { ws_tr_drop(s); s->state = ABI_WS_CLOSED; }
            else ws_fail(s, HTTP_ERR_RESET);
            return;
        }
        if (n == 0) return;
        if (s->cancel) return;              /* app left during the wait */
        if (ws_rx_feed(s, rd, (uint32_t)n) != 0) return;
        if (s->peer_closed && s->state == ABI_WS_OPEN) {
            ws_finish_peer_close(s);
            return;
        }
    }
}

static void ws_worker(void) {
    if (!bkl_held()) bkl_enter();
    struct proc *self = current_proc();
    if (self) self->tcp_yield_wait = true;
    kprintf("[ws] worker up (pid=%d)\n", self ? self->pid : -1);

    for (;;) {
        for (int i = 0; i < WS_MAX; i++) {
            struct ws_slot *s = &g_ws[i];
            if (!s->used) continue;

            if (s->cancel) {
                /* App detached. Finish politely if the conn is live:
                 * Close frame out (TCP ACKs it before we RST). */
                if (s->state == ABI_WS_OPEN && (s->tcp || s->tls)) {
                    ws_tx_drain(s);
                    ws_send_close_frame(s, s->close_req_code
                                               ? s->close_req_code : 1000);
                }
                ws_tr_drop(s);
                slot_free(s);
                continue;
            }
            if (s->state == ABI_WS_CONNECTING) {
                int rc = ws_do_connect(s);
                if (s->cancel) {            /* app left mid-handshake */
                    ws_tr_drop(s);
                    slot_free(s);
                    continue;
                }
                if (rc != 0) {
                    ws_fail(s, rc);
                    kprintf("[ws] h%d connect failed (%d)\n", i, rc);
                } else if (s->state == ABI_WS_CONNECTING) {
                    s->state = ABI_WS_OPEN;
                }
            } else if (s->state == ABI_WS_OPEN) {
                ws_service(s);
            }
            /* CLOSED slots wait for the app's ws_close. */
        }
        /* Share the CPU between passes -- never yield holding the BKL. */
        bkl_exit();
        sched_yield();
        bkl_enter();
    }
}

/* ---- syscall entry points (BKL, non-yielding) --------------------------- */

static void reap_orphans(void) {
    for (int i = 0; i < WS_MAX; i++) {
        struct ws_slot *s = &g_ws[i];
        if (!s->used || s->cancel) continue;
        struct proc *p = proc_lookup(s->owner);
        if (!p || p->pid != s->owner) {
            if (s->state == ABI_WS_CLOSED && !s->tcp && !s->tls)
                slot_free(s);
            else
                s->cancel = true;          /* worker tears down + frees */
        }
    }
}

static struct ws_slot *slot_of(int h, int owner_pid) {
    if (h < 0 || h >= WS_MAX) return NULL;
    struct ws_slot *s = &g_ws[h];
    if (!s->used || s->owner != owner_pid || s->cancel) return NULL;
    return s;
}

int ws_open(const char *url, int owner_pid) {
    if (!url || !url[0]) return -1;
    if (!starts_with_ci(url, "ws://") && !starts_with_ci(url, "wss://"))
        return -1;

    if (g_ws_worker_pid < 0) {
        g_ws_worker_pid = proc_create_kernel(ws_worker, "wsock");
        if (g_ws_worker_pid < 0) return -1;
    }

    struct ws_slot *s = NULL;
    for (int pass = 0; pass < 2 && !s; pass++) {
        for (int i = 0; i < WS_MAX; i++)
            if (!g_ws[i].used) { s = &g_ws[i]; break; }
        if (!s && pass == 0) reap_orphans();
    }
    if (!s) return -1;

    memset(s, 0, sizeof(*s));
    s->used  = true;
    s->owner = owner_pid;
    s->state = ABI_WS_CONNECTING;
    size_t l = strlen(url);
    if (l >= sizeof(s->url)) l = sizeof(s->url) - 1;
    memcpy(s->url, url, l);
    s->url[l] = 0;
    return (int)(s - g_ws);
}

int ws_poll(int h, int owner_pid, struct abi_ws_poll *out) {
    struct ws_slot *s = slot_of(h, owner_pid);
    if (!s || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->state = (int32_t)s->state;
    out->err = (int32_t)s->err;
    out->close_code = s->close_code;
    if (s->qh) {
        out->msg_avail = 1;
        out->msg_len = s->qh->len;
        out->msg_type = s->qh->op;
        uint32_t cnt = 0;
        for (struct ws_msg *m = s->qh; m; m = m->next) cnt++;
        out->queued = cnt;
    }
    return 0;
}

long ws_recv(int h, int owner_pid, void *dst, uint32_t cap) {
    struct ws_slot *s = slot_of(h, owner_pid);
    if (!s || !dst) return -1;
    struct ws_msg *m = s->qh;
    if (!m) return -1;
    s->qh = m->next;
    if (!s->qh) s->qt = NULL;
    s->q_bytes -= m->len;
    uint32_t n = m->len < cap ? m->len : cap;
    if (n) memcpy(dst, m->data, n);
    kfree(m);
    return (long)n;
}

int ws_send(int h, int owner_pid, const void *msg, uint32_t len, int op) {
    struct ws_slot *s = slot_of(h, owner_pid);
    if (!s || s->state != ABI_WS_OPEN) return -1;
    if (op != ABI_WS_OP_TEXT && op != ABI_WS_OP_BINARY) return -1;
    return ws_tx_queue(s, op, (const uint8_t *)msg, len);
}

int ws_close(int h, int owner_pid, uint16_t code) {
    if (h < 0 || h >= WS_MAX) return -1;
    struct ws_slot *s = &g_ws[h];
    if (!s->used || s->owner != owner_pid) return -1;
    if (s->cancel) return 0;               /* already detaching */
    if (s->state == ABI_WS_CLOSED && !s->tcp && !s->tls) {
        slot_free(s);                       /* worker is done with it */
        return 0;
    }
    s->close_req = true;
    s->close_req_code = code ? code : 1000;
    s->cancel = true;                       /* worker tears down + frees */
    return 0;
}
