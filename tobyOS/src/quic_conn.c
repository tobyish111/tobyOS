/* quic_conn.c -- QUIC handshake first-flight assembly (HTTP/3 slice 4).
 * See quic_conn.h. Builds a QUIC ClientHello + client Initial packet
 * on the crypto (quic_crypto.h) + packet (quic_packet.h) layers. */

#include <tobyos/quic_conn.h>
#include <tobyos/quic_packet.h>
#include <tobyos/quic_crypto.h>
#include <tobyos/tls13.h>       /* tls_put_u16/u24 wire helpers */
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#include "monocypher.h"

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
