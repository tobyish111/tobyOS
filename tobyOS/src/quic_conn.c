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

#include "monocypher.h"

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

/* ---- Receive path (slice 4c) ------------------------------------ *
 * A UDP recv hook (registered in udp.c for QUIC_CLIENT_PORT) captures
 * the server's reply datagram (the QUIC packet after the UDP header)
 * into a static buffer. quic_udp_send_test polls for it and processes
 * the server Initial: open it with the SERVER Initial keys (derived
 * from the client's original DCID), parse the ServerHello out of the
 * CRYPTO frame, and derive the handshake shared secret. */

static uint8_t  g_qrx[2048];
static size_t   g_qrx_len;
static volatile int g_qrx_ready;

void quic_recv_hook(uint32_t src_ip_be, const void *udp_packet, size_t len) {
    (void)src_ip_be;
    if (g_qrx_ready) return;                 /* keep the first datagram */
    if (len <= 8) return;
    size_t n = len - 8;                      /* strip the 8-byte UDP header */
    if (n > sizeof g_qrx) n = sizeof g_qrx;
    memcpy(g_qrx, (const uint8_t *)udp_packet + 8, n);
    g_qrx_len = n;
    g_qrx_ready = 1;
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

/* ---- On-the-wire send test (slice 4b) --------------------------- *
 * Build a real, RANDOM-CID client Initial padded to 1200 bytes (the
 * RFC 9000 anti-amplification minimum) and UDP-send it to a QUIC
 * listener on the SLIRP host (10.0.2.2:4433), which decrypts + parses
 * the ClientHello over the wire. Proves the outbound QUIC path end to
 * end. Called from boot under -DQUIC_SEND_TEST once the network is up. */
int quic_udp_send_test(void) {
    uint8_t dcid[8], scid[8], priv[32], rnd[32], pub[32];
    rng_fill(dcid, sizeof dcid);
    rng_fill(scid, sizeof scid);
    rng_fill(priv, sizeof priv);
    rng_fill(rnd, sizeof rnd);
    crypto_x25519_public_key(pub, priv);

    uint8_t ch[512];
    size_t chl = quic_build_client_hello(ch, sizeof ch, rnd, pub,
                                         scid, sizeof scid, "tobyos.test");
    if (!chl) { kprintf("[quicudp] ClientHello build failed\n"); return -1; }

    static uint8_t pkt[1500];
    size_t plen = quic_build_client_initial(pkt, sizeof pkt, dcid, sizeof dcid,
                                            scid, sizeof scid, ch, chl,
                                            1 /* pkt_num */, 1200 /* pad */);
    if (!plen) { kprintf("[quicudp] Initial build failed\n"); return -1; }

    uint8_t ipb[4] = { 10, 0, 2, 2 };
    uint32_t dst_ip; memcpy(&dst_ip, ipb, 4);        /* network order */
    g_qrx_ready = 0;
    bool ok = udp_send(htons(QUIC_CLIENT_PORT), dst_ip, htons(4433), pkt, plen);
    kprintf("[quicudp] sent Initial %u bytes to 10.0.2.2:4433 (dcid=", (unsigned)plen);
    for (int i = 0; i < 8; i++) kprintf("%02x", dcid[i]);
    kprintf(") %s\n", ok ? "OK" : "SEND-FAIL");
    if (!ok) return -1;

    /* ---- slice 4c: wait for the server's reply datagram ---- */
    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t deadline = pit_ticks() + hz * 3;        /* ~3 s */
    struct net_dev *nd = net_default();
    while (pit_ticks() < deadline && !g_qrx_ready) {
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        sti();
        hlt();
    }
    if (!g_qrx_ready) {
        kprintf("[quicudp] no server reply within 3s\n");
        return 0;                                    /* send still succeeded */
    }
    kprintf("[quicudp] received %u bytes from server\n", (unsigned)g_qrx_len);

    /* Open the server Initial with the SERVER Initial keys (derived
     * from our original DCID), parse the ServerHello, derive the
     * handshake shared secret. The reply may coalesce Initial +
     * Handshake packets; we process the leading Initial. */
    uint8_t skey[16], siv[12], shp[16];
    quic_initial_keys(dcid, sizeof dcid, 0 /* server */, skey, siv, shp);
    const uint8_t *rpay; uint64_t rpn;
    long rl = quic_open_initial(g_qrx, g_qrx_len, skey, siv, shp, &rpay, &rpn);
    if (rl <= 0) {
        kprintf("[quicudp] server Initial decrypt FAILED\n");
        return -1;
    }
    kprintf("[quicudp] server Initial decrypted (pn=%u, %ld payload bytes)\n",
            (unsigned)rpn, rl);

    /* Find the CRYPTO frame (ServerHello) among the reply's frames. */
    size_t fp = 0; struct quic_frame f; int found = 0;
    while (fp < (size_t)rl) {
        size_t fn = quic_frame_parse(rpay + fp, (size_t)rl - fp, &f);
        if (fn == 0) break;
        if (f.type == QUIC_FRAME_CRYPTO && f.len >= 4 && f.data[0] == 2) {
            found = 1; break;
        }
        fp += fn;
    }
    if (!found) {
        kprintf("[quicudp] no ServerHello CRYPTO frame in reply\n");
        return -1;
    }

    uint8_t server_pub[32]; uint16_t cipher = 0;
    if (quic_parse_server_hello(f.data, (size_t)f.len, server_pub, &cipher) != 0) {
        kprintf("[quicudp] ServerHello parse FAILED\n");
        return -1;
    }
    uint8_t shared[32];
    crypto_x25519(shared, priv, server_pub);
    kprintf("[quicudp] ServerHello OK: cipher=0x%04x, x25519 shared secret ",
            cipher);
    for (int i = 0; i < 8; i++) kprintf("%02x", shared[i]);
    kprintf("... HANDSHAKE KEYS DERIVABLE\n");
    return 0;
}
