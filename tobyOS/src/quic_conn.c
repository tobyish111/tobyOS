/* quic_conn.c -- QUIC handshake first-flight assembly (HTTP/3 slice 4).
 * See quic_conn.h. Builds a QUIC ClientHello + client Initial packet
 * on the crypto (quic_crypto.h) + packet (quic_packet.h) layers. */

#include <tobyos/quic_conn.h>
#include <tobyos/quic_packet.h>
#include <tobyos/quic_crypto.h>
#include <tobyos/tls13.h>       /* tls_put_u16/u24 wire helpers */
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/rng.h>
#include <tobyos/udp.h>
#include <tobyos/net.h>         /* htons */
#include <tobyos/pit.h>
#include <tobyos/cpu.h>         /* sti/hlt */
#include <tobyos/sec.h>         /* sha256 transcript */
#include <tobyos/tls_x509.h>    /* shared cert chain + CertificateVerify (4f) */
#include <tobyos/http3.h>       /* H3 frames + QPACK (slice 5) */
#include <tobyos/http.h>        /* http_response + HTTP_ERR/HTTP_F flags (slice 5b) */
#include <tobyos/heap.h>        /* kmalloc/kfree for the response body (5b) */

#include "monocypher.h"
#include <bearssl.h>            /* br_x509_pkey for cert auth */

#define QUIC_CLIENT_PORT 56789   /* our UDP source port (reply dst) */

/* TLS 1.3 / QUIC constants (mirrors tls.c's set). */
#define TLS_VERSION_12  0x0303
#define TLS_VERSION_13  0x0304
#define TLS_HS_CLIENT_HELLO         1
#define TLS_EXT_SNI                 0x0000
#define TLS_EXT_SUPPORTED_GROUPS    0x000a
#define TLS_EXT_SIGNATURE_ALGOS     0x000d
#define TLS_EXT_ALPN                0x0010
#define TLS_EXT_SUPPORTED_VERSIONS  0x002b
#define TLS_EXT_KEY_SHARE           0x0033
#define TLS_EXT_QUIC_TP             0x0039   /* quic_transport_parameters */
#define TLS_GROUP_X25519            0x001d

/* ---- QUIC transport parameters (RFC 9000 s18) ------------------- */

/* Append one transport parameter (varint id + varint len + value). */
static size_t tp_put(uint8_t *p, uint64_t id, const uint8_t *val, size_t vlen) {
    size_t n = 0;
    n += quic_varint_encode(p + n, id);
    n += quic_varint_encode(p + n, vlen);
    if (vlen) { memcpy(p + n, val, vlen); n += vlen; }
    return n;
}
static size_t tp_put_int(uint8_t *p, uint64_t id, uint64_t v) {
    uint8_t vb[8];
    size_t vn = quic_varint_encode(vb, v);
    return tp_put(p, id, vb, vn);
}

/* Build the transport-parameters block. initial_source_connection_id
 * (0x0f) carries our SCID; the rest are sane client defaults. */
static size_t quic_build_transport_params(uint8_t *out,
                                          const uint8_t *scid, size_t scid_len) {
    size_t n = 0;
    n += tp_put(out + n, 0x0f, scid, scid_len);         /* initial_source_connection_id */
    n += tp_put_int(out + n, 0x01, 30000);              /* max_idle_timeout (ms) */
    n += tp_put_int(out + n, 0x03, 1472);               /* max_udp_payload_size */
    n += tp_put_int(out + n, 0x04, 1048576);            /* initial_max_data */
    n += tp_put_int(out + n, 0x05, 262144);             /* stream_data_bidi_local */
    n += tp_put_int(out + n, 0x06, 262144);             /* stream_data_bidi_remote */
    n += tp_put_int(out + n, 0x07, 262144);             /* stream_data_uni */
    n += tp_put_int(out + n, 0x08, 100);                /* initial_max_streams_bidi */
    n += tp_put_int(out + n, 0x09, 100);                /* initial_max_streams_uni */
    return n;
}

/* ---- ClientHello ------------------------------------------------ */

size_t quic_build_client_hello(uint8_t *out, size_t cap,
                               const uint8_t random[32],
                               const uint8_t pubkey[32],
                               const uint8_t *scid, size_t scid_len,
                               const char *hostname) {
    size_t hostname_len = 0;
    if (hostname) while (hostname[hostname_len]) hostname_len++;

    size_t pos = 4;                          /* leave room for hs header */
    if (cap < 512) return 0;

    tls_put_u16(out + pos, TLS_VERSION_12); pos += 2;   /* legacy_version */
    memcpy(out + pos, random, 32); pos += 32;           /* random */
    out[pos++] = 0;                                     /* session_id: empty */

    /* cipher_suites: AES-128-GCM + ChaCha20-Poly1305 (both SHA-256). */
    tls_put_u16(out + pos, 4); pos += 2;
    tls_put_u16(out + pos, 0x1301); pos += 2;           /* TLS_AES_128_GCM_SHA256 */
    tls_put_u16(out + pos, 0x1303); pos += 2;           /* TLS_CHACHA20_POLY1305 */

    out[pos++] = 1; out[pos++] = 0;                     /* compression: null */

    size_t ext_len_pos = pos; pos += 2;

    /* supported_versions -> TLS 1.3 */
    tls_put_u16(out + pos, TLS_EXT_SUPPORTED_VERSIONS); pos += 2;
    tls_put_u16(out + pos, 3); pos += 2;
    out[pos++] = 2;
    tls_put_u16(out + pos, TLS_VERSION_13); pos += 2;

    /* supported_groups -> x25519 */
    tls_put_u16(out + pos, TLS_EXT_SUPPORTED_GROUPS); pos += 2;
    tls_put_u16(out + pos, 4); pos += 2;
    tls_put_u16(out + pos, 2); pos += 2;
    tls_put_u16(out + pos, TLS_GROUP_X25519); pos += 2;

    /* key_share -> x25519 pubkey */
    tls_put_u16(out + pos, TLS_EXT_KEY_SHARE); pos += 2;
    tls_put_u16(out + pos, 38); pos += 2;
    tls_put_u16(out + pos, 36); pos += 2;
    tls_put_u16(out + pos, TLS_GROUP_X25519); pos += 2;
    tls_put_u16(out + pos, 32); pos += 2;
    memcpy(out + pos, pubkey, 32); pos += 32;

    /* signature_algorithms (the TLS 1.3 set we verify) */
    tls_put_u16(out + pos, TLS_EXT_SIGNATURE_ALGOS); pos += 2;
    tls_put_u16(out + pos, 10); pos += 2;
    tls_put_u16(out + pos, 8); pos += 2;
    tls_put_u16(out + pos, 0x0403); pos += 2;
    tls_put_u16(out + pos, 0x0503); pos += 2;
    tls_put_u16(out + pos, 0x0804); pos += 2;
    tls_put_u16(out + pos, 0x0805); pos += 2;

    /* SNI (if hostname) */
    if (hostname_len > 0 && hostname_len < 256) {
        tls_put_u16(out + pos, TLS_EXT_SNI); pos += 2;
        tls_put_u16(out + pos, (uint16_t)(hostname_len + 5)); pos += 2;
        tls_put_u16(out + pos, (uint16_t)(hostname_len + 3)); pos += 2;
        out[pos++] = 0;                                 /* host_name */
        tls_put_u16(out + pos, (uint16_t)hostname_len); pos += 2;
        memcpy(out + pos, hostname, hostname_len); pos += hostname_len;
    }

    /* ALPN -> "h3" */
    tls_put_u16(out + pos, TLS_EXT_ALPN); pos += 2;
    tls_put_u16(out + pos, 5); pos += 2;                /* ext len */
    tls_put_u16(out + pos, 3); pos += 2;                /* list len */
    out[pos++] = 2; out[pos++] = 'h'; out[pos++] = '3';

    /* quic_transport_parameters */
    {
        uint8_t tp[128];
        size_t tpl = quic_build_transport_params(tp, scid, scid_len);
        tls_put_u16(out + pos, TLS_EXT_QUIC_TP); pos += 2;
        tls_put_u16(out + pos, (uint16_t)tpl); pos += 2;
        memcpy(out + pos, tp, tpl); pos += tpl;
    }

    tls_put_u16(out + ext_len_pos, (uint16_t)(pos - ext_len_pos - 2));

    /* handshake header */
    out[0] = TLS_HS_CLIENT_HELLO;
    tls_put_u24(out + 1, (uint32_t)(pos - 4));
    return pos;
}

size_t quic_build_client_initial(uint8_t *out, size_t cap,
                                 const uint8_t *dcid, size_t dcid_len,
                                 const uint8_t *scid, size_t scid_len,
                                 const uint8_t *ch, size_t ch_len,
                                 uint64_t pkt_num, size_t pad_to) {
    uint8_t key[16], iv[12], hp[16];
    quic_initial_keys(dcid, dcid_len, 1, key, iv, hp);

    /* payload = CRYPTO(offset 0, ClientHello) + PADDING to pad_to. */
    static uint8_t payload[1500];
    size_t pl = quic_frame_crypto(payload, sizeof payload, 0, ch, ch_len);
    if (!pl) return 0;
    /* Overhead the packet adds around the payload: header (~pn_off) +
     * 16 tag. Pad the PAYLOAD so the whole packet reaches pad_to. */
    if (pad_to > 0) {
        size_t overhead = 7 + dcid_len + scid_len + 4 + 16; /* approx */
        while (pl + overhead < pad_to && pl < sizeof payload)
            payload[pl++] = QUIC_FRAME_PADDING;
    }
    return quic_build_initial(out, cap, dcid, dcid_len, scid, scid_len,
                              NULL, 0, pkt_num, 4, payload, pl, key, iv, hp);
}

/* ---- Self-test (deterministic; dumps hex for offline interop) ---- */

static void dump_hex(const char *tag, const uint8_t *p, size_t n) {
    /* Chunked so a long line doesn't overflow the log formatter. */
    char line[145];
    kprintf("[quicch] %s len=%u\n", tag, (unsigned)n);
    for (size_t off = 0; off < n; off += 64) {
        size_t k = n - off; if (k > 64) k = 64;
        int c = 0;
        for (size_t i = 0; i < k; i++) {
            static const char H[] = "0123456789abcdef";
            line[c++] = H[p[off + i] >> 4];
            line[c++] = H[p[off + i] & 15];
        }
        line[c] = 0;
        kprintf("[quicch] %s\n", line);
    }
}

int quic_conn_selftest(void) {
    int pass = 0;

    /* Fixed inputs -> a deterministic Initial the host can reproduce
     * and decrypt (Initial keys derive from dcid). */
    static const uint8_t dcid[8] =
        { 0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08 };
    static const uint8_t scid[4] = { 0xca, 0xfe, 0xba, 0xbe };
    uint8_t priv[32], rnd[32], pub[32];
    for (int i = 0; i < 32; i++) { priv[i] = (uint8_t)(i + 1); rnd[i] = (uint8_t)(0xa0 + i); }
    crypto_x25519_public_key(pub, priv);

    uint8_t ch[512];
    size_t chl = quic_build_client_hello(ch, sizeof ch, rnd, pub,
                                         scid, sizeof scid, "example.org");
    int ch_ok = (chl > 40 && ch[0] == TLS_HS_CLIENT_HELLO);
    kprintf("[quicch] ClientHello build          %s (%u bytes)\n",
            ch_ok ? "OK" : "FAIL", (unsigned)chl);
    pass += ch_ok;

    uint8_t pkt[1500];
    size_t plen = quic_build_client_initial(pkt, sizeof pkt, dcid, sizeof dcid,
                                            scid, sizeof scid, ch, chl, 0, 0);
    int pkt_ok = (plen > chl);
    kprintf("[quicch] Initial build              %s (%u bytes)\n",
            pkt_ok ? "OK" : "FAIL", (unsigned)plen);
    pass += pkt_ok;

    /* Dump the PROTECTED packet for offline aioquic validation --
     * BEFORE the round-trip below, since quic_open_initial decrypts +
     * removes header protection IN PLACE. */
    dump_hex("initial", pkt, plen);

    /* Round-trip on a copy: open our own Initial, recover the CH. */
    static uint8_t rtpkt[1500];
    memcpy(rtpkt, pkt, plen);
    uint8_t key[16], iv[12], hp[16];
    quic_initial_keys(dcid, sizeof dcid, 1, key, iv, hp);
    const uint8_t *rpay; uint64_t rpn;
    long rl = quic_open_initial(rtpkt, plen, key, iv, hp, &rpay, &rpn);
    struct quic_frame f;
    int rt_ok = 0;
    if (rl > 0 && quic_frame_parse(rpay, (size_t)rl, &f) > 0 &&
        f.type == QUIC_FRAME_CRYPTO && f.len == chl &&
        memcmp(f.data, ch, chl) == 0)
        rt_ok = 1;
    kprintf("[quicch] Initial open round-trip    %s\n", rt_ok ? "OK" : "FAIL");
    pass += rt_ok;

    kprintf("[quicch] client-hello self-test: %d/%d %s\n", pass, QUIC_CONN_SELFTEST_N,
            pass == QUIC_CONN_SELFTEST_N ? "ALL PASS" : "FAILURES");
    return pass;
}

/* ---- Receive path (slices 4c..4h) -------------------------------- *
 * A UDP recv hook (registered in udp.c for QUIC_CLIENT_PORT) queues
 * the server's reply datagrams (the QUIC packet after the UDP header)
 * into a small ring -- a live server sends its flights across SEVERAL
 * datagrams (Initial + multiple Handshake packets + 1-RTT), so keeping
 * only the first one (the 4c..4g scripted-responder behavior) drops
 * everything past the first. quic_udp_send_test polls + dequeues. */

#define QRX_SLOTS 8
#define QRX_MAX   2048
static uint8_t  g_qq[QRX_SLOTS][QRX_MAX];
static uint16_t g_qq_len[QRX_SLOTS];
static volatile uint32_t g_qq_head, g_qq_tail;   /* head=write, tail=read */

void quic_recv_hook(uint32_t src_ip_be, const void *udp_packet, size_t len) {
    (void)src_ip_be;
    if (len <= 8) return;
    uint32_t h = g_qq_head;
    if (h - g_qq_tail >= QRX_SLOTS) return;       /* full: drop (peer resends) */
    size_t n = len - 8;                           /* strip the 8-byte UDP header */
    if (n > QRX_MAX) n = QRX_MAX;
    memcpy(g_qq[h % QRX_SLOTS], (const uint8_t *)udp_packet + 8, n);
    g_qq_len[h % QRX_SLOTS] = (uint16_t)n;
    g_qq_head = h + 1;
}

static int qrx_pop(uint8_t *dst, size_t cap) {
    if (g_qq_tail == g_qq_head) return -1;
    uint32_t t = g_qq_tail;
    size_t n = g_qq_len[t % QRX_SLOTS];
    if (n > cap) n = cap;
    memcpy(dst, g_qq[t % QRX_SLOTS], n);
    g_qq_tail = t + 1;
    return (int)n;
}

/* Minimal ServerHello parser: pull the server random + the X25519
 * key_share. Returns 0 on success. (QUIC carries the same TLS 1.3
 * ServerHello as TLS-over-TCP, just inside a CRYPTO frame.) */
static int quic_parse_server_hello(const uint8_t *d, size_t len,
                                   uint8_t server_pub[32], uint16_t *cipher) {
    if (len < 40 || d[0] != 2) return -1;    /* handshake type 2 = SH */
    size_t p = 4;                            /* skip hs header */
    p += 2;                                  /* legacy_version */
    p += 32;                                 /* random */
    if (p >= len) return -1;
    uint8_t sid = d[p++]; p += sid;          /* session_id echo */
    if (p + 3 > len) return -1;
    *cipher = tls_get_u16(d + p); p += 2;
    p += 1;                                  /* compression */
    if (p + 2 > len) return -1;
    uint16_t ext_total = tls_get_u16(d + p); p += 2;
    size_t ext_end = p + ext_total; if (ext_end > len) ext_end = len;
    int have_ks = 0;
    while (p + 4 <= ext_end) {
        uint16_t et = tls_get_u16(d + p);
        uint16_t el = tls_get_u16(d + p + 2);
        p += 4;
        if (p + el > ext_end) break;
        if (et == 0x0033 && el >= 36) {       /* key_share */
            uint16_t grp = tls_get_u16(d + p);
            uint16_t kl = tls_get_u16(d + p + 2);
            if (grp == 0x001d && kl == 32) {
                memcpy(server_pub, d + p + 4, 32);
                have_ks = 1;
            }
        }
        p += el;
    }
    return have_ks ? 0 : -1;
}

/* ---- CRYPTO-stream reassembly (slice 4h) ------------------------- *
 * A live server's Certificate chain spans multiple CRYPTO frames
 * across multiple Handshake packets (and datagrams), possibly
 * retransmitted -- so CRYPTO data is copied in at its stream offset
 * with per-byte coverage bits, and the flight is only consumed once
 * the prefix is contiguous. */

struct qrsm {
    uint8_t *buf;
    uint8_t *bits;                /* coverage bitmap, cap/8 bytes */
    uint32_t cap;
};

static void qrsm_add(struct qrsm *r, uint64_t off, const uint8_t *d, uint64_t n) {
    if (off >= r->cap) return;
    if (off + n > r->cap) n = r->cap - off;
    memcpy(r->buf + off, d, n);
    for (uint64_t i = off; i < off + n; i++)
        r->bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static uint32_t qrsm_contig(const struct qrsm *r) {
    uint32_t i = 0;
    while (i < r->cap && (r->bits[i >> 3] & (1u << (i & 7)))) i++;
    return i;
}

/* ---- Received-packet-number tracking (for the ACKs we send) ------ */

struct qack {
    uint64_t largest;
    uint64_t seen;                /* bitmap of pns 0..63 */
    int any;                      /* any packet received in this space */
    int elicited;                 /* an ACK-eliciting packet needs acking */
};

static void qack_note(struct qack *a, uint64_t pn, int elicit) {
    if (pn < 64) a->seen |= 1ULL << pn;
    if (!a->any || pn > a->largest) a->largest = pn;
    a->any = 1;
    if (elicit) a->elicited = 1;
}

static uint64_t qack_first_range(const struct qack *a) {
    uint64_t r = 0, pn = a->largest;
    while (pn > 0 && pn - 1 < 64 && (a->seen & (1ULL << (pn - 1)))) { r++; pn--; }
    return r;
}

/* Structural long-header parse: total on-wire length of the packet at
 * pkt (so coalesced packets can be walked even when one can't be
 * opened -- the length field is not protected). 0 = malformed. */
static size_t long_pkt_wire_len(const uint8_t *pkt, size_t len) {
    if (len < 7) return 0;
    size_t p = 5;
    size_t dl = pkt[p++]; p += dl;
    if (p >= len) return 0;
    size_t sl = pkt[p++]; p += sl;
    if (p > len) return 0;
    unsigned type = pkt[0] & 0x30;
    if (type == 0x30) return len;              /* Retry: rest of datagram */
    if (type == 0x00) {                        /* Initial: token field */
        uint64_t tl; size_t n = quic_varint_decode(pkt + p, len - p, &tl);
        if (!n) return 0;
        p += n + (size_t)tl;
    }
    uint64_t length; size_t n = quic_varint_decode(pkt + p, len - p, &length);
    if (!n) return 0;
    p += n;
    if (p + length > len) return 0;
    return p + (size_t)length;
}

/* Build a client Initial datagram: the given frames + PADDING to the
 * RFC 9000 s14.1 1200-byte minimum (every client datagram carrying an
 * Initial must be padded, ACK-only ones included). Keys derive from
 * key_dcid (the original -- or post-Retry -- DCID); hdr_dcid is what
 * goes in the header (the server's chosen SCID once known). */
static size_t build_initial_1200(uint8_t *out, size_t cap,
                                 const uint8_t *key_dcid, size_t key_dcid_len,
                                 const uint8_t *hdr_dcid, size_t hdr_dcid_len,
                                 const uint8_t *scid, size_t scid_len,
                                 const uint8_t *token, size_t token_len,
                                 uint64_t pkt_num,
                                 const uint8_t *frames, size_t flen) {
    static uint8_t payload[1400];
    if (flen > sizeof payload) return 0;
    memcpy(payload, frames, flen);
    size_t pl = flen;
    uint8_t vt[8];
    size_t tvn = quic_varint_encode(vt, token_len);
    /* header: 1 + ver 4 + (1+dcid) + (1+scid) + token + len(2) + pn(4) */
    size_t overhead = 7 + hdr_dcid_len + scid_len + tvn + token_len + 2 + 4 + 16;
    while (pl + overhead < 1200 && pl < sizeof payload)
        payload[pl++] = QUIC_FRAME_PADDING;
    uint8_t key[16], iv[12], hp[16];
    quic_initial_keys(key_dcid, key_dcid_len, 1, key, iv, hp);
    return quic_build_initial(out, cap, hdr_dcid, hdr_dcid_len, scid, scid_len,
                              token, token_len, pkt_num, 4, payload, pl,
                              key, iv, hp);
}

/* ---- HTTP/3 response headers -> http_response (slices 5 + 5b) ------ */

static void cpy_field(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

struct h3_resp_build { struct http_response *out; int status_seen; int nhdr; };

static void h3_hdr_to_resp(void *ud, const char *name, const char *value) {
    struct h3_resp_build *b = ud;
    struct http_response *o = b->out;
    b->nhdr++;
    if (strcmp(name, ":status") == 0) {
        int s = 0;
        for (const char *c = value; *c >= '0' && *c <= '9'; c++)
            s = s * 10 + (*c - '0');
        o->status = s;
        b->status_seen = 1;
    } else if (strcmp(name, "content-type") == 0) {
        cpy_field(o->content_type, sizeof o->content_type, value);
    } else if (strcmp(name, "location") == 0) {
        cpy_field(o->location, sizeof o->location, value);
    } else if (strcmp(name, "content-length") == 0) {
        long n = 0; int any = 0;
        for (const char *c = value; *c >= '0' && *c <= '9'; c++) {
            n = n * 10 + (*c - '0'); any = 1;
        }
        if (any) o->content_len = n;
    } else if (strcmp(name, "content-encoding") == 0) {
        if (strcmp(value, "gzip") == 0)      o->encoding = HTTP_ENC_GZIP;
        else if (strcmp(value, "br") == 0)   o->encoding = HTTP_ENC_BR;
    }
    kprintf("[quich3]   %s: %s\n", name, value);
}

/* Response-stream reassembly cap: matches the initial_max_stream_data
 * we advertise (262144), so flow control never blocks inside it. A
 * larger response returns HTTP_ERR_TOOBIG -> the http.c ladder falls
 * back to h2/h1.1. */
#define H3_RESP_MAX 262144u

/* ---- http3_fetch: a reusable HTTP/3 GET transport (slices 4b..5b) - *
 * Run a full QUIC v1 handshake to `ip_be`:`port` (network-order IP),
 * then GET `path` as HTTP/3 and fill `out`. The handshake processes
 * the server's datagrams (coalesced packets, Version Negotiation,
 * Retry with integrity check, CRYPTO reassembly across packets), ACKs
 * at every encryption level, REQUIRES the certificate chain +
 * CertificateVerify to validate against `host` (the 13H path over
 * QUIC), verifies the server Finished, sends the client Finished, and
 * -- once the server's HANDSHAKE_DONE lands -- opens the request
 * (bidi stream 0: QPACK HEADERS, FIN) and reassembles the response
 * stream into `out` (status / content-type / content-length /
 * content-encoding / body). Returns 0 on success, a negative
 * HTTP_ERR_* otherwise. Synchronous, one fetch at a time (fixed UDP
 * source port). AES-128-GCM only; a ChaCha20 server is refused at
 * ServerHello. */
int http3_fetch(uint32_t ip_be, uint16_t port, const char *host,
                const char *path, unsigned flags, size_t max_body,
                uint32_t timeout_ms, struct http_response *out) {
    if (!host) host = "";
    if (!path || !*path) path = "/";
    if (out) { memset(out, 0, sizeof *out); out->content_len = -1; }
    if (max_body == 0 || max_body > H3_RESP_MAX) max_body = H3_RESP_MAX;
    if (timeout_ms == 0) timeout_ms = 15000;
    int want_gzip = (flags & HTTP_F_GZIP) != 0;

    /* --- connection identity --- */
    uint8_t odcid[8], scid[8], priv[32], rnd[32], pub[32];
    rng_fill(odcid, sizeof odcid);
    rng_fill(scid, sizeof scid);
    rng_fill(priv, sizeof priv);
    rng_fill(rnd, sizeof rnd);
    crypto_x25519_public_key(pub, priv);

    static uint8_t ch[512];
    size_t chl = quic_build_client_hello(ch, sizeof ch, rnd, pub,
                                         scid, sizeof scid, host);
    if (!chl) { kprintf("[quich3] ClientHello build failed\n"); return HTTP_ERR_CONNECT; }

    /* --- connection state --- */
    uint8_t key_dcid[20]; size_t key_dcid_len = sizeof odcid;
    memcpy(key_dcid, odcid, sizeof odcid);       /* replaced by Retry SCID */
    uint8_t server_cid[20]; size_t server_cid_len = key_dcid_len;
    memcpy(server_cid, key_dcid, key_dcid_len);  /* -> server's SCID once known */
    static uint8_t token[512]; size_t token_len = 0;
    uint64_t next_ipn = 1, next_hpn = 0, next_apn = 0;
    struct qack iack, hack, aack;
    memset(&iack, 0, sizeof iack); memset(&hack, 0, sizeof hack);
    memset(&aack, 0, sizeof aack);
    int got_retry = 0, got_initial = 0, have_scid = 0;
    int sh_parsed = 0, flight_done = 0, fin_sent = 0;
    int hs_done = 0, req_sent = 0;
    uint64_t s0_fin = (uint64_t)-1;          /* response stream end offset */

    /* server Initial keys (for opening its Initials; re-derived on Retry) */
    uint8_t sikey[16], siiv[12], sihp[16];
    quic_initial_keys(key_dcid, key_dcid_len, 0, sikey, siiv, sihp);

    /* handshake + 1-RTT keys/secrets (filled as the handshake advances) */
    uint8_t hs_secret[32], s_hs[32], c_hs[32];
    uint8_t shk[16], shiv[12], shhp[16];         /* server Handshake */
    uint8_t chk[16], chiv[12], chhp[16];         /* client Handshake */
    uint8_t sak[16], saiv[12], sahp[16];         /* server 1-RTT */
    uint8_t cak[16], caiv[12], cahp[16];         /* client 1-RTT */
    static uint8_t sh_msg[512]; size_t sh_len = 0;
    uint8_t zeros[32]; memset(zeros, 0, 32);
    uint8_t empty_hash[32]; sha256_buf(NULL, 0, empty_hash);
    uint8_t thash_af[32];                        /* transcript incl server Fin */

    /* CRYPTO reassembly (Initial + Handshake levels) + the response
     * stream (1-RTT stream 0, reassembled the same way) */
    static uint8_t ibuf[1024],  ibits[1024 / 8];
    static uint8_t hbuf[8192],  hbits[8192 / 8];
    static uint8_t s0buf[H3_RESP_MAX], s0bits[H3_RESP_MAX / 8];
    memset(ibits, 0, sizeof ibits); memset(hbits, 0, sizeof hbits);
    memset(s0bits, 0, sizeof s0bits);
    struct qrsm irsm = { ibuf, ibits, sizeof ibuf };
    struct qrsm hrsm = { hbuf, hbits, sizeof hbuf };
    struct qrsm s0rsm = { s0buf, s0bits, sizeof s0buf };

    g_qq_head = g_qq_tail = 0;

    uint32_t dst_ip = ip_be;                     /* network order */

    /* --- send the client Initial (CRYPTO(ClientHello), padded 1200) --- */
    static uint8_t pktbuf[1500];
    static uint8_t frames[1400];
    size_t fl = quic_frame_crypto(frames, sizeof frames, 0, ch, chl);
    size_t plen = build_initial_1200(pktbuf, sizeof pktbuf, key_dcid, key_dcid_len,
                                     server_cid, server_cid_len,
                                     scid, sizeof scid, NULL, 0,
                                     next_ipn++, frames, fl);
    if (!plen) { kprintf("[quich3] Initial build failed\n"); return HTTP_ERR_CONNECT; }
    bool ok = udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(port), pktbuf, plen);
    {
        uint8_t *ipp = (uint8_t *)&ip_be;
        kprintf("[quich3] QUIC connect %u.%u.%u.%u:%u (host %s) -- sent Initial "
                "%u bytes %s\n", ipp[0], ipp[1], ipp[2], ipp[3], port, host,
                (unsigned)plen, ok ? "OK" : "SEND-FAIL");
    }
    if (!ok) return HTTP_ERR_CONNECT;

    /* --- receive loop --- */
    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t tmo_ticks = (uint64_t)hz * timeout_ms / 1000;
    if (tmo_ticks < hz) tmo_ticks = hz;
    uint64_t deadline = pit_ticks() + tmo_ticks;
    struct net_dev *nd = net_default();
    static uint8_t dg[QRX_MAX];

    while (pit_ticks() < deadline) {
        int dn = qrx_pop(dg, sizeof dg);
        if (dn < 0) {
            if (nd && nd->rx_drain) nd->rx_drain(nd);
            sti();
            hlt();
            continue;
        }

        /* ---- walk the datagram's coalesced packets ---- */
        size_t off = 0;
        while (off < (size_t)dn) {
            uint8_t *pk = dg + off;
            size_t avail = (size_t)dn - off;

            if (!(pk[0] & 0x80)) {
                /* Short header (1-RTT): always the last packet in the
                 * datagram (no length field). Needs the server app keys. */
                if (fin_sent) {
                    const uint8_t *pay; uint64_t pn;
                    long pl = quic_open_short(pk, avail, sizeof scid,
                                              sak, saiv, sahp, &pay, &pn);
                    if (pl > 0) {
                        int elicit = 0;
                        size_t fp = 0; struct quic_frame f;
                        while (fp < (size_t)pl) {
                            size_t fn = quic_frame_parse(pay + fp,
                                                         (size_t)pl - fp, &f);
                            if (!fn) break;
                            switch (f.type) {
                            case QUIC_FRAME_HANDSHAKE_DONE:
                                if (!hs_done) {
                                    hs_done = 1;
                                    kprintf("[quicudp] 1-RTT HANDSHAKE_DONE "
                                            "received -- LIVE QUIC HANDSHAKE "
                                            "COMPLETE (server confirmed our "
                                            "Finished)\n");
                                }
                                elicit = 1;
                                break;
                            case QUIC_FRAME_STREAM:
                                /* Stream 0 = our request's response;
                                 * server uni streams (control/QPACK)
                                 * are ignored. */
                                if (f.stream_id == 0) {
                                    qrsm_add(&s0rsm, f.offset, f.data, f.len);
                                    if (f.fin)
                                        s0_fin = f.offset + f.len;
                                }
                                elicit = 1;
                                break;
                            case QUIC_FRAME_CONN_CLOSE:
                            case QUIC_FRAME_CONN_CLOSE_APP: {
                                char rsn[64]; size_t rl = f.len < 63 ? (size_t)f.len : 63;
                                memcpy(rsn, f.data, rl); rsn[rl] = 0;
                                kprintf("[quicudp] CONNECTION_CLOSE err=%u "
                                        "reason='%s' -- aborting\n",
                                        (unsigned)f.err_code, rsn);
                                return HTTP_ERR_RESET;
                            }
                            case QUIC_FRAME_ACK:
                            case QUIC_FRAME_ACK_ECN:
                            case QUIC_FRAME_PADDING:
                                break;
                            default:
                                elicit = 1;   /* CRYPTO(tickets)/NEW_CID/... */
                                break;
                            }
                            fp += fn;
                        }
                        qack_note(&aack, pn, elicit);
                    }
                }
                break;                       /* short header ends the datagram */
            }

            uint32_t ver = ((uint32_t)pk[1] << 24) | ((uint32_t)pk[2] << 16) |
                           ((uint32_t)pk[3] << 8)  |  (uint32_t)pk[4];
            if (ver == 0) {
                /* Version Negotiation: our version (1) is unsupported.
                 * List what the server offers, then fail closed. */
                size_t p = 5;
                size_t dl = (p < avail) ? pk[p++] : 0; p += dl;
                size_t sl = (p < avail) ? pk[p++] : 0; p += sl;
                unsigned nver = 0;
                kprintf("[quicudp] Version Negotiation received; server offers:");
                while (p + 4 <= avail && nver < 8) {
                    uint32_t v = ((uint32_t)pk[p] << 24) | ((uint32_t)pk[p+1] << 16) |
                                 ((uint32_t)pk[p+2] << 8) | (uint32_t)pk[p+3];
                    kprintf(" 0x%08x", v);
                    p += 4; nver++;
                }
                kprintf("\n[quicudp] v1 not supported by server -- aborting "
                        "(fail closed)\n");
                return HTTP_ERR_CONNECT;
            }

            size_t wire = long_pkt_wire_len(pk, avail);
            if (!wire) break;                /* malformed: drop rest */
            unsigned type = pk[0] & 0x30;

            if (type == 0x30) {
                /* Retry (RFC 9000 s17.2.5): adopt the server's new CID +
                 * token, re-derive Initial keys, resend the SAME
                 * ClientHello. Ignored after a processed Initial or a
                 * previous Retry (per spec). */
                if (!got_initial && !got_retry) {
                    const uint8_t *nscid; size_t nscid_len;
                    const uint8_t *tok; size_t tl;
                    if (quic_parse_retry(pk, avail, &nscid, &nscid_len,
                                         &tok, &tl) == 0 && nscid_len <= 20 &&
                        tl <= sizeof token) {
                        if (quic_retry_tag_verify(pk, avail, key_dcid,
                                                  key_dcid_len) != 0) {
                            kprintf("[quicudp] Retry integrity tag BAD -- "
                                    "ignoring packet\n");
                        } else {
                            memcpy(key_dcid, nscid, nscid_len);
                            key_dcid_len = nscid_len;
                            memcpy(server_cid, nscid, nscid_len);
                            server_cid_len = nscid_len;
                            memcpy(token, tok, tl); token_len = tl;
                            quic_initial_keys(key_dcid, key_dcid_len, 0,
                                              sikey, siiv, sihp);
                            got_retry = 1;
                            kprintf("[quicudp] Retry: integrity tag OK, new dcid=");
                            for (size_t i = 0; i < key_dcid_len; i++)
                                kprintf("%02x", key_dcid[i]);
                            kprintf(", token %u bytes -- resending Initial\n",
                                    (unsigned)token_len);
                            size_t f2 = quic_frame_crypto(frames, sizeof frames,
                                                          0, ch, chl);
                            size_t p2 = build_initial_1200(pktbuf, sizeof pktbuf,
                                            key_dcid, key_dcid_len,
                                            server_cid, server_cid_len,
                                            scid, sizeof scid,
                                            token, token_len,
                                            next_ipn++, frames, f2);
                            if (p2)
                                (void)udp_send(htons(QUIC_CLIENT_PORT), dst_ip,
                                               htons(port), pktbuf, p2);
                        }
                    }
                }
                off += wire;
                continue;
            }

            if (type == 0x00) {
                /* Server Initial. */
                const uint8_t *pay; uint64_t pn; size_t consumed = 0;
                long pl = quic_open_long(pk, avail, 1, sikey, siiv, sihp,
                                         &pay, &pn, &consumed);
                if (pl > 0) {
                    if (!have_scid) {
                        /* Adopt the server's SCID as our DCID from now on
                         * (a real server routes by it). */
                        size_t p2 = 5, dl2 = pk[p2]; p2 += 1 + dl2;
                        size_t sl2 = pk[p2]; p2 += 1;
                        if (sl2 <= 20) {
                            memcpy(server_cid, pk + p2, sl2);
                            server_cid_len = sl2;
                            have_scid = 1;
                        }
                    }
                    got_initial = 1;
                    int elicit = 0;
                    size_t fp = 0; struct quic_frame f;
                    while (fp < (size_t)pl) {
                        size_t fn = quic_frame_parse(pay + fp, (size_t)pl - fp, &f);
                        if (!fn) break;
                        if (f.type == QUIC_FRAME_CRYPTO) {
                            qrsm_add(&irsm, f.offset, f.data, f.len);
                            elicit = 1;
                        } else if (f.type == QUIC_FRAME_PING) {
                            elicit = 1;
                        } else if (f.type == QUIC_FRAME_CONN_CLOSE ||
                                   f.type == QUIC_FRAME_CONN_CLOSE_APP) {
                            char rsn[64]; size_t rl = f.len < 63 ? (size_t)f.len : 63;
                            memcpy(rsn, f.data, rl); rsn[rl] = 0;
                            kprintf("[quicudp] CONNECTION_CLOSE err=%u "
                                    "reason='%s' -- aborting\n",
                                    (unsigned)f.err_code, rsn);
                            return HTTP_ERR_RESET;
                        }
                        fp += fn;
                    }
                    qack_note(&iack, pn, elicit);
                }
                off += wire;
            } else if (type == 0x20) {
                /* Server Handshake -- needs the handshake keys. */
                if (sh_parsed) {
                    const uint8_t *pay; uint64_t pn;
                    long pl = quic_open_long(pk, avail, 0, shk, shiv, shhp,
                                             &pay, &pn, NULL);
                    if (pl > 0) {
                        int elicit = 0;
                        size_t fp = 0; struct quic_frame f;
                        while (fp < (size_t)pl) {
                            size_t fn = quic_frame_parse(pay + fp,
                                                         (size_t)pl - fp, &f);
                            if (!fn) break;
                            if (f.type == QUIC_FRAME_CRYPTO) {
                                qrsm_add(&hrsm, f.offset, f.data, f.len);
                                elicit = 1;
                            } else if (f.type == QUIC_FRAME_PING) {
                                elicit = 1;
                            } else if (f.type == QUIC_FRAME_CONN_CLOSE ||
                                       f.type == QUIC_FRAME_CONN_CLOSE_APP) {
                                char rsn[64]; size_t rl = f.len < 63 ? (size_t)f.len : 63;
                                memcpy(rsn, f.data, rl); rsn[rl] = 0;
                                kprintf("[quicudp] CONNECTION_CLOSE err=%u "
                                        "reason='%s' -- aborting\n",
                                        (unsigned)f.err_code, rsn);
                                return HTTP_ERR_RESET;
                            }
                            fp += fn;
                        }
                        qack_note(&hack, pn, elicit);
                    }
                }
                off += wire;
            } else {
                off += wire;                 /* 0-RTT etc: skip */
            }

            /* ---- ServerHello: parse once the Initial CRYPTO prefix
             * completes a message; derive the Handshake keys so
             * coalesced Handshake packets in this SAME datagram open. */
            if (!sh_parsed) {
                uint32_t ic = qrsm_contig(&irsm);
                if (ic >= 4) {
                    uint32_t ml = ((uint32_t)ibuf[1] << 16) |
                                  ((uint32_t)ibuf[2] << 8) | ibuf[3];
                    if (ibuf[0] == 2 && ic >= 4 + ml && 4 + ml <= sizeof sh_msg) {
                        sh_len = 4 + ml;
                        memcpy(sh_msg, ibuf, sh_len);
                        uint8_t server_pub[32]; uint16_t cipher = 0;
                        if (quic_parse_server_hello(sh_msg, sh_len,
                                                    server_pub, &cipher) != 0) {
                            kprintf("[quicudp] ServerHello parse FAILED\n");
                            return HTTP_ERR_PROTOCOL;
                        }
                        if (cipher != 0x1301) {
                            kprintf("[quicudp] server picked cipher 0x%04x "
                                    "(only AES-128-GCM QUIC keys wired) -- "
                                    "aborting\n", cipher);
                            return HTTP_ERR_CONNECT;
                        }
                        uint8_t shared[32];
                        crypto_x25519(shared, priv, server_pub);
                        kprintf("[quicudp] ServerHello OK: cipher=0x%04x, "
                                "x25519 shared secret ", cipher);
                        for (int i = 0; i < 8; i++) kprintf("%02x", shared[i]);
                        kprintf("\n");

                        /* RFC 8446 s7.1: transcript = SHA-256(CH||SH). */
                        uint8_t thash[32];
                        { struct sha256_ctx tc; sha256_init(&tc);
                          sha256_update(&tc, ch, chl);
                          sha256_update(&tc, sh_msg, sh_len);
                          sha256_final(&tc, thash); }
                        uint8_t early[32];
                        hkdf_extract(NULL, 0, zeros, 32, early);
                        uint8_t derived[32];
                        derive_secret(early, "derived", 7, empty_hash, derived);
                        hkdf_extract(derived, 32, shared, 32, hs_secret);
                        derive_secret(hs_secret, "s hs traffic", 12, thash, s_hs);
                        derive_secret(hs_secret, "c hs traffic", 12, thash, c_hs);
                        hkdf_expand_label(s_hs, "quic key", 8, NULL, 0, shk, 16);
                        hkdf_expand_label(s_hs, "quic iv",  7, NULL, 0, shiv, 12);
                        hkdf_expand_label(s_hs, "quic hp",  7, NULL, 0, shhp, 16);
                        hkdf_expand_label(c_hs, "quic key", 8, NULL, 0, chk, 16);
                        hkdf_expand_label(c_hs, "quic iv",  7, NULL, 0, chiv, 12);
                        hkdf_expand_label(c_hs, "quic hp",  7, NULL, 0, chhp, 16);
                        kprintf("[quicudp] derived Handshake keys "
                                "(server key ");
                        for (int i = 0; i < 6; i++) kprintf("%02x", shk[i]);
                        kprintf(")\n");
                        sh_parsed = 1;
                    }
                }
            }
        }

        /* ---- ACK what we received (per encryption level) ---- */
        if (iack.elicited) {
            uint8_t af[16];
            size_t al = quic_frame_ack(af, sizeof af, iack.largest,
                                       qack_first_range(&iack));
            size_t p2 = build_initial_1200(pktbuf, sizeof pktbuf,
                                           key_dcid, key_dcid_len,
                                           server_cid, server_cid_len,
                                           scid, sizeof scid,
                                           token, token_len,
                                           next_ipn++, af, al);
            if (p2)
                (void)udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(port),
                               pktbuf, p2);
            iack.elicited = 0;
        }
        if (hack.elicited && sh_parsed && !fin_sent) {
            uint8_t af[16];
            size_t al = quic_frame_ack(af, sizeof af, hack.largest,
                                       qack_first_range(&hack));
            static uint8_t hpkt[128];
            size_t hl = quic_build_long(hpkt, sizeof hpkt, 0x20,
                                        server_cid, server_cid_len,
                                        scid, sizeof scid,
                                        0, NULL, 0, next_hpn++, 4,
                                        af, al, chk, chiv, chhp);
            if (hl)
                (void)udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(port),
                               hpkt, hl);
            hack.elicited = 0;
        }

        /* ---- HTTP/3 (slice 5): send the GET once the handshake is
         * confirmed -- one 1-RTT packet carrying the ACK, our control
         * stream (SETTINGS; no QPACK dynamic table advertised) and the
         * request stream (QPACK HEADERS, FIN). ---- */
        if (hs_done && !req_sent) {
            uint8_t fs[512]; size_t fl2 = 0;
            fl2 += quic_frame_ack(fs + fl2, sizeof fs - fl2, aack.largest,
                                  qack_first_range(&aack));
            aack.elicited = 0;
            /* control stream (client-uni id 2): type 0x00 + empty SETTINGS */
            static const uint8_t ctrl[] = { H3_STREAM_CONTROL,
                                            H3_FRAME_SETTINGS, 0x00 };
            fl2 += quic_frame_stream(fs + fl2, sizeof fs - fl2, 2, 0,
                                     ctrl, sizeof ctrl, 0);
            /* request stream (client-bidi id 0): HEADERS frame, FIN */
            uint8_t sec[512];
            size_t sl2 = h3_qpack_encode_request(sec, sizeof sec,
                                                 host, path, want_gzip);
            uint8_t req[560]; size_t rl2 = 0;
            rl2 += h3_frame_hdr(req, sizeof req, H3_FRAME_HEADERS, sl2);
            memcpy(req + rl2, sec, sl2); rl2 += sl2;
            fl2 += quic_frame_stream(fs + fl2, sizeof fs - fl2, 0, 0,
                                     req, rl2, 1);
            static uint8_t qpkt[900];
            size_t ql = quic_build_short(qpkt, sizeof qpkt,
                                         server_cid, server_cid_len,
                                         next_apn++, 4, fs, fl2,
                                         cak, caiv, cahp);
            if (!ql) {
                kprintf("[quich3] request packet build failed\n");
                return HTTP_ERR_PROTOCOL;
            }
            (void)udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(port),
                           qpkt, ql);
            req_sent = 1;
            deadline = pit_ticks() + tmo_ticks;
            kprintf("[quich3] sent GET https://%s%s over HTTP/3 "
                    "(SETTINGS + QPACK HEADERS, %u byte 1-RTT packet)\n",
                    host, path, (unsigned)ql);
        } else if (aack.elicited && req_sent) {
            uint8_t af[16];
            size_t al = quic_frame_ack(af, sizeof af, aack.largest,
                                       qack_first_range(&aack));
            static uint8_t apkt[128];
            size_t apl = quic_build_short(apkt, sizeof apkt,
                                          server_cid, server_cid_len,
                                          next_apn++, 4, af, al,
                                          cak, caiv, cahp);
            if (apl)
                (void)udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(port),
                               apkt, apl);
            aack.elicited = 0;
        }

        /* ---- a stream 0 FIN whose end exceeds our buffer: too big for
         * the h3 path -> let the http.c ladder fall back to h2/h1.1. ---- */
        if (req_sent && s0_fin != (uint64_t)-1 && s0_fin > (uint64_t)sizeof s0buf) {
            kprintf("[quich3] response %llu bytes exceeds the %u cap -- "
                    "HTTP_ERR_TOOBIG (fall back)\n",
                    (unsigned long long)s0_fin, (unsigned)sizeof s0buf);
            return HTTP_ERR_TOOBIG;
        }

        /* ---- consume the response once stream 0 is complete ---- */
        if (req_sent && s0_fin != (uint64_t)-1 &&
            qrsm_contig(&s0rsm) >= s0_fin) {
            /* First pass: decode headers (fills status/type/len/encoding)
             * and total the DATA payload. */
            struct h3_resp_build rb = { out, 0, 0 };
            size_t body_total = 0;
            size_t o2 = 0;
            int bad = 0;
            while (o2 < s0_fin && !bad) {
                uint64_t ftype, flen2;
                size_t c = quic_varint_decode(s0buf + o2, s0_fin - o2, &ftype);
                if (!c) { bad = 1; break; }
                o2 += c;
                c = quic_varint_decode(s0buf + o2, s0_fin - o2, &flen2);
                if (!c || o2 + c + flen2 > s0_fin) { bad = 1; break; }
                o2 += c;
                if (ftype == H3_FRAME_HEADERS) {
                    kprintf("[quich3] response HEADERS (%u byte QPACK section):\n",
                            (unsigned)flen2);
                    if (h3_qpack_decode(s0buf + o2, (size_t)flen2,
                                        h3_hdr_to_resp, &rb) != 0) {
                        kprintf("[quich3] QPACK decode FAILED\n");
                        bad = 1;
                    }
                } else if (ftype == H3_FRAME_DATA) {
                    body_total += (size_t)flen2;
                }
                o2 += (size_t)flen2;
            }
            if (bad || !rb.status_seen) {
                kprintf("[quich3] response parse FAILED\n");
                return HTTP_ERR_PROTOCOL;
            }

            /* Second pass: copy the DATA bytes into a kmalloc'd body,
             * honouring max_body (truncate when HTTP_F_TRUNCATE). */
            int truncated = 0;
            size_t body_cap = body_total;
            if (body_cap > max_body) {
                if (!(flags & HTTP_F_TRUNCATE)) {
                    kprintf("[quich3] body %u > cap %u -- HTTP_ERR_TOOBIG\n",
                            (unsigned)body_total, (unsigned)max_body);
                    return HTTP_ERR_TOOBIG;
                }
                body_cap = max_body;
                truncated = 1;
            }
            uint8_t *body = body_cap ? (uint8_t *)kmalloc(body_cap) : NULL;
            if (body_cap && !body) return HTTP_ERR_NOMEM;
            size_t body_len = 0;
            o2 = 0;
            while (o2 < s0_fin) {
                uint64_t ftype, flen2;
                size_t c = quic_varint_decode(s0buf + o2, s0_fin - o2, &ftype);
                if (!c) break; o2 += c;
                c = quic_varint_decode(s0buf + o2, s0_fin - o2, &flen2);
                if (!c) break; o2 += c;
                if (ftype == H3_FRAME_DATA && body) {
                    size_t k = (size_t)flen2;
                    if (body_len + k > body_cap) k = body_cap - body_len;
                    memcpy(body + body_len, s0buf + o2, k);
                    body_len += k;
                }
                o2 += (size_t)flen2;
            }
            out->body = body;
            out->body_len = body_len;
            if (out->content_len < 0) out->content_len = (long)body_total;

            /* final ACK so the server doesn't retransmit at us */
            uint8_t af[16];
            size_t al = quic_frame_ack(af, sizeof af, aack.largest,
                                       qack_first_range(&aack));
            static uint8_t apkt2[128];
            size_t apl = quic_build_short(apkt2, sizeof apkt2,
                                          server_cid, server_cid_len,
                                          next_apn++, 4, af, al,
                                          cak, caiv, cahp);
            if (apl)
                (void)udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(port),
                               apkt2, apl);
            kprintf("[quich3] HTTP/3 GET COMPLETE: status=%d, %d headers, "
                    "%u body bytes%s -- PAGE FETCHED OVER HTTP/3\n",
                    out->status, rb.nhdr, (unsigned)body_len,
                    truncated ? " (truncated)" : "");
            return 0;
        }

        /* ---- consume the Handshake flight once contiguous ---- */
        if (sh_parsed && !flight_done) {
            uint32_t hc = qrsm_contig(&hrsm);
            size_t mp = 0, fin_off = (size_t)-1, cv_off = (size_t)-1;
            const uint8_t *sfin = NULL; size_t sfin_len = 0;
            const uint8_t *cert_msg = NULL; size_t cert_len = 0;
            const uint8_t *cv_body = NULL;  size_t cv_len = 0;
            int have_ee = 0;
            while (mp + 4 <= hc) {
                uint8_t mt = hbuf[mp];
                uint32_t ml = ((uint32_t)hbuf[mp+1] << 16) |
                              ((uint32_t)hbuf[mp+2] << 8) | hbuf[mp+3];
                if (mp + 4 + ml > hc) break;
                if (mt == 8)  have_ee = 1;
                else if (mt == 11) { cert_msg = hbuf + mp + 4; cert_len = ml; }
                else if (mt == 15) { cv_off = mp;
                                     cv_body = hbuf + mp + 4; cv_len = ml; }
                else if (mt == 20) { fin_off = mp;
                                     sfin = hbuf + mp + 4; sfin_len = ml; break; }
                mp += 4 + ml;
            }
            if (sfin && sfin_len >= 32) {
                flight_done = 1;
                kprintf("[quicudp] Handshake flight complete (%u bytes "
                        "reassembled): EE=%d Certificate=%u CertVerify=%u "
                        "Finished=1\n", (unsigned)(fin_off + 4 + sfin_len),
                        have_ee, (unsigned)cert_len, (unsigned)cv_len);

                /* ---- certificate authentication: REQUIRED (slice 4h).
                 * A live server must present a chain to a trusted root
                 * AND prove key possession via CertificateVerify. */
                if (!cert_msg || !cv_body || cv_len < 4) {
                    kprintf("[quicudp] server flight lacks Certificate/"
                            "CertificateVerify -- aborting\n");
                    return HTTP_ERR_CERT;
                }
                br_x509_pkey ee_pk;
                unsigned char ee_pk_buf[BR_X509_BUFSIZE_KEY];
                int chain = tls_x509_validate_chain(cert_msg, cert_len,
                                                    host, &ee_pk,
                                                    ee_pk_buf, sizeof ee_pk_buf);
                if (chain != 0) {
                    kprintf("[quicudp] cert chain UNTRUSTED -- aborting "
                            "(fail closed)\n");
                    return HTTP_ERR_CERT;
                }
                uint8_t thash_cv[32];
                { struct sha256_ctx tc; sha256_init(&tc);
                  sha256_update(&tc, ch, chl);
                  sha256_update(&tc, sh_msg, sh_len);
                  sha256_update(&tc, hbuf, cv_off);
                  sha256_final(&tc, thash_cv); }
                uint16_t scheme = (uint16_t)((cv_body[0] << 8) | cv_body[1]);
                size_t sl = ((size_t)cv_body[2] << 8) | cv_body[3];
                int cv_ok = 0;
                if (4 + sl <= cv_len)
                    cv_ok = tls_x509_verify_cv(scheme, cv_body + 4, sl,
                                               thash_cv, &ee_pk);
                if (!cv_ok) {
                    kprintf("[quicudp] CertificateVerify FAILED (scheme "
                            "0x%04x) -- aborting\n", scheme);
                    return HTTP_ERR_CERT;
                }
                kprintf("[quicudp] cert chain OK (trusted root); "
                        "CertificateVerify VERIFIED (scheme 0x%04x, peer "
                        "holds leaf key)\n", scheme);

                /* ---- server Finished over Hash(CH..CertVerify) ---- */
                uint8_t thash_sf[32];
                { struct sha256_ctx tc; sha256_init(&tc);
                  sha256_update(&tc, ch, chl);
                  sha256_update(&tc, sh_msg, sh_len);
                  sha256_update(&tc, hbuf, fin_off);
                  sha256_final(&tc, thash_sf); }
                uint8_t expect_fin[32];
                compute_finished(s_hs, thash_sf, expect_fin);
                if (memcmp(expect_fin, sfin, 32) != 0) {
                    kprintf("[quicudp] server Finished MAC MISMATCH -- "
                            "aborting\n");
                    return HTTP_ERR_PROTOCOL;
                }
                kprintf("[quicudp] server Finished VERIFIED\n");

                /* ---- 1-RTT keys (both directions) ---- */
                { struct sha256_ctx tc; sha256_init(&tc);
                  sha256_update(&tc, ch, chl);
                  sha256_update(&tc, sh_msg, sh_len);
                  sha256_update(&tc, hbuf, fin_off + 4 + sfin_len);
                  sha256_final(&tc, thash_af); }
                uint8_t derived2[32];
                derive_secret(hs_secret, "derived", 7, empty_hash, derived2);
                uint8_t master[32];
                hkdf_extract(derived2, 32, zeros, 32, master);
                uint8_t c_ap[32], s_ap[32];
                derive_secret(master, "c ap traffic", 12, thash_af, c_ap);
                derive_secret(master, "s ap traffic", 12, thash_af, s_ap);
                hkdf_expand_label(c_ap, "quic key", 8, NULL, 0, cak, 16);
                hkdf_expand_label(c_ap, "quic iv",  7, NULL, 0, caiv, 12);
                hkdf_expand_label(c_ap, "quic hp",  7, NULL, 0, cahp, 16);
                hkdf_expand_label(s_ap, "quic key", 8, NULL, 0, sak, 16);
                hkdf_expand_label(s_ap, "quic iv",  7, NULL, 0, saiv, 12);
                hkdf_expand_label(s_ap, "quic hp",  7, NULL, 0, sahp, 16);
                kprintf("[quicudp] 1-RTT keys installed (client app key ");
                for (int i = 0; i < 6; i++) kprintf("%02x", cak[i]);
                kprintf(")\n");

                /* ---- client Finished (+ Handshake ACK piggybacked) ---- */
                uint8_t cfin[32];
                compute_finished(c_hs, thash_af, cfin);
                uint8_t cfin_msg[36];
                cfin_msg[0] = 20; cfin_msg[1] = 0; cfin_msg[2] = 0;
                cfin_msg[3] = 32;
                memcpy(cfin_msg + 4, cfin, 32);
                uint8_t fr[80]; size_t frl = 0;
                frl += quic_frame_ack(fr + frl, sizeof fr - frl, hack.largest,
                                      qack_first_range(&hack));
                frl += quic_frame_crypto(fr + frl, sizeof fr - frl, 0,
                                         cfin_msg, 36);
                hack.elicited = 0;
                static uint8_t cpkt[256];
                size_t cplen = quic_build_long(cpkt, sizeof cpkt, 0x20,
                                               server_cid, server_cid_len,
                                               scid, sizeof scid,
                                               0, NULL, 0, next_hpn++, 4,
                                               fr, frl, chk, chiv, chhp);
                if (!cplen) {
                    kprintf("[quicudp] client Finished build failed\n");
                    return HTTP_ERR_PROTOCOL;
                }
                (void)udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(port),
                               cpkt, cplen);
                fin_sent = 1;
                kprintf("[quicudp] sent client Finished (%u byte Handshake "
                        "packet); waiting for HANDSHAKE_DONE\n",
                        (unsigned)cplen);
            }
        }
    }

    kprintf("[quicudp] TIMEOUT: handshake incomplete (initial=%d sh=%d "
            "flight=%d fin_sent=%d hs_contig=%u)\n",
            got_initial, sh_parsed, flight_done, fin_sent,
            (unsigned)qrsm_contig(&hrsm));
    return HTTP_ERR_TIMEOUT;
}

/* Boot-time on-the-wire test (slices 4b..5b). Two proofs against the
 * aioquic server on the SLIRP host (10.0.2.2:4433, cert for
 * tobyos.test), both under -DQUIC_SEND_TEST (with -DTLS_TEST_CA to
 * trust the test CA + resolve tobyos.test):
 *   1. http3_fetch directly -- the reusable transport in isolation.
 *   2. http_get_opt("https://tobyos.test:4433/", HTTP_F_TRY_H3) -- the
 *      browser-shaped path: URL parse -> DNS (test seam) -> h3 probe ->
 *      http3_fetch -> http_response. */
int quic_udp_send_test(void) {
    uint8_t ipb[4] = { 10, 0, 2, 2 };
    uint32_t ip_be; memcpy(&ip_be, ipb, 4);         /* network order */

    struct http_response resp;
    int rc = http3_fetch(ip_be, 4433, "tobyos.test", "/",
                         HTTP_F_TRUNCATE, H3_RESP_MAX, 15000, &resp);
    if (rc == 0) {
        char preview[121];
        size_t k = resp.body_len < sizeof preview - 1 ? resp.body_len
                                                      : sizeof preview - 1;
        if (resp.body && k) memcpy(preview, resp.body, k);
        preview[k] = 0;
        kprintf("[quich3] transport OK: status=%d type='%s' body=%u bytes: %s\n",
                resp.status, resp.content_type, (unsigned)resp.body_len, preview);
        http_free(&resp);
    } else {
        kprintf("[quich3] transport FAILED rc=%d\n", rc);
        return rc;
    }

    struct http_response r2;
    int rc2 = http_get_opt("https://tobyos.test:4433/", 65536, 15000,
                           HTTP_F_TRY_H3 | HTTP_F_TRUNCATE, &r2);
    if (rc2 == 0) {
        kprintf("[quich3] http_get_opt over h3 OK: status=%d body=%u bytes "
                "-- HTTP/3 WIRED INTO http.c\n", r2.status, (unsigned)r2.body_len);
        http_free(&r2);
    } else {
        kprintf("[quich3] http_get_opt over h3 FAILED rc=%d\n", rc2);
    }
    return rc2;
}
