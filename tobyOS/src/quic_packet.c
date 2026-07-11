/* quic_packet.c -- QUIC packet + frame wire format (RFC 9000/9001).
 * See quic_packet.h. Builds on quic_crypto.h (Initial keys, header +
 * packet protection) and tls13 wire helpers. */

#include <tobyos/quic_packet.h>
#include <tobyos/quic_crypto.h>
#include <tobyos/sec.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* ---- Frames ----------------------------------------------------- */

size_t quic_frame_crypto(uint8_t *out, size_t cap,
                         uint64_t offset, const uint8_t *data, size_t len) {
    uint8_t vo[8], vl[8];
    size_t no = quic_varint_encode(vo, offset);
    size_t nl = quic_varint_encode(vl, len);
    size_t total = 1 + no + nl + len;
    if (total > cap) return 0;
    size_t p = 0;
    out[p++] = QUIC_FRAME_CRYPTO;
    memcpy(out + p, vo, no); p += no;
    memcpy(out + p, vl, nl); p += nl;
    if (len) memcpy(out + p, data, len);
    (void)p;
    return total;
}

size_t quic_frame_ack(uint8_t *out, size_t cap,
                      uint64_t largest, uint64_t first_range) {
    uint8_t buf[1 + 8 * 4];
    size_t p = 0;
    buf[p++] = QUIC_FRAME_ACK;
    p += quic_varint_encode(buf + p, largest);
    p += quic_varint_encode(buf + p, 0);           /* ack delay */
    p += quic_varint_encode(buf + p, 0);           /* range count */
    p += quic_varint_encode(buf + p, first_range); /* first ack range */
    if (p > cap) return 0;
    memcpy(out, buf, p);
    return p;
}

size_t quic_frame_parse(const uint8_t *p, size_t cap, struct quic_frame *f) {
    if (cap < 1) return 0;
    memset(f, 0, sizeof *f);
    f->type = p[0];
    if (p[0] == QUIC_FRAME_PADDING) {
        size_t n = 1;
        while (n < cap && p[n] == QUIC_FRAME_PADDING) n++;
        return n;                          /* collapse the PADDING run */
    }
    if (p[0] == QUIC_FRAME_PING || p[0] == QUIC_FRAME_HANDSHAKE_DONE)
        return 1;
    if (p[0] == QUIC_FRAME_CRYPTO || p[0] == QUIC_FRAME_NEW_TOKEN) {
        size_t o = 1, n;
        if (p[0] == QUIC_FRAME_CRYPTO) {
            n = quic_varint_decode(p + o, cap - o, &f->offset); if (!n) return 0; o += n;
        }
        n = quic_varint_decode(p + o, cap - o, &f->len);    if (!n) return 0; o += n;
        if (o + f->len > cap) return 0;
        f->data = p + o;
        return o + (size_t)f->len;
    }
    if (p[0] == QUIC_FRAME_ACK || p[0] == QUIC_FRAME_ACK_ECN) {
        size_t o = 1, n; uint64_t delay, rc, first;
        n = quic_varint_decode(p + o, cap - o, &f->largest); if (!n) return 0; o += n;
        n = quic_varint_decode(p + o, cap - o, &delay);      if (!n) return 0; o += n;
        n = quic_varint_decode(p + o, cap - o, &rc);         if (!n) return 0; o += n;
        n = quic_varint_decode(p + o, cap - o, &first);      if (!n) return 0; o += n;
        for (uint64_t i = 0; i < rc; i++) {                  /* gap+range pairs */
            uint64_t g, r;
            n = quic_varint_decode(p + o, cap - o, &g); if (!n) return 0; o += n;
            n = quic_varint_decode(p + o, cap - o, &r); if (!n) return 0; o += n;
        }
        if (p[0] == QUIC_FRAME_ACK_ECN) {                    /* 3 ECN counts */
            for (int i = 0; i < 3; i++) {
                uint64_t c;
                n = quic_varint_decode(p + o, cap - o, &c); if (!n) return 0; o += n;
            }
        }
        return o;
    }
    if (p[0] == QUIC_FRAME_NEW_CID) {
        size_t o = 1, n; uint64_t seq, retire;
        n = quic_varint_decode(p + o, cap - o, &seq);    if (!n) return 0; o += n;
        n = quic_varint_decode(p + o, cap - o, &retire); if (!n) return 0; o += n;
        if (o >= cap) return 0;
        size_t cl = p[o++];                                  /* CID length */
        if (o + cl + 16 > cap) return 0;                     /* CID + reset token */
        return o + cl + 16;
    }
    if (p[0] == QUIC_FRAME_RETIRE_CID) {
        size_t o = 1, n; uint64_t seq;
        n = quic_varint_decode(p + o, cap - o, &seq); if (!n) return 0; o += n;
        return o;
    }
    if (p[0] == QUIC_FRAME_CONN_CLOSE || p[0] == QUIC_FRAME_CONN_CLOSE_APP) {
        size_t o = 1, n;
        n = quic_varint_decode(p + o, cap - o, &f->err_code); if (!n) return 0; o += n;
        if (p[0] == QUIC_FRAME_CONN_CLOSE) {                 /* frame type */
            uint64_t ft;
            n = quic_varint_decode(p + o, cap - o, &ft); if (!n) return 0; o += n;
        }
        n = quic_varint_decode(p + o, cap - o, &f->len);     if (!n) return 0; o += n;
        if (o + f->len > cap) return 0;
        f->data = p + o;                                     /* reason phrase */
        return o + (size_t)f->len;
    }
    return 0;                              /* unhandled frame type */
}

/* ---- Long-header packets ---------------------------------------- */

size_t quic_build_long(uint8_t *out, size_t cap, unsigned type_bits,
                       const uint8_t *dcid, size_t dcid_len,
                       const uint8_t *scid, size_t scid_len,
                       int has_token,
                       const uint8_t *token, size_t token_len,
                       uint64_t pkt_num, unsigned pn_len,
                       const uint8_t *payload, size_t payload_len,
                       const uint8_t key[16], const uint8_t iv[12],
                       const uint8_t hp[16]) {
    if (pn_len < 1 || pn_len > 4) return 0;
    size_t p = 0;
    /* First byte: long(0x80) + fixed(0x40) + type_bits + pn_len-1. */
    if (p >= cap) return 0;
    out[p++] = (uint8_t)(0xc0 | (type_bits & 0x30) | (pn_len - 1));
    /* version = 1 */
    if (p + 4 > cap) return 0;
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 1;
    /* DCID */
    if (p + 1 + dcid_len > cap) return 0;
    out[p++] = (uint8_t)dcid_len;
    memcpy(out + p, dcid, dcid_len); p += dcid_len;
    /* SCID */
    if (p + 1 + scid_len > cap) return 0;
    out[p++] = (uint8_t)scid_len;
    memcpy(out + p, scid, scid_len); p += scid_len;
    /* token length (varint) + token -- Initial only */
    if (has_token) {
        uint8_t vt[8];
        size_t nvt = quic_varint_encode(vt, token_len);
        if (p + nvt + token_len > cap) return 0;
        memcpy(out + p, vt, nvt); p += nvt;
        if (token_len) { memcpy(out + p, token, token_len); p += token_len; }
    }
    /* length (varint) = pn_len + payload_len + 16 tag */
    uint64_t length = pn_len + payload_len + 16;
    uint8_t vl[8];
    size_t nvl = quic_varint_encode(vl, length);
    if (p + nvl > cap) return 0;
    memcpy(out + p, vl, nvl); p += nvl;
    /* packet number (unprotected for now) */
    size_t pn_off = p;
    if (p + pn_len > cap) return 0;
    for (unsigned i = 0; i < pn_len; i++)
        out[p++] = (uint8_t)(pkt_num >> (8 * (pn_len - 1 - i)));
    size_t header_len = p;
    /* payload */
    if (p + payload_len + 16 > cap) return 0;
    memcpy(out + p, payload, payload_len);

    /* Packet protection: AEAD over header (AAD) + payload, tag after. */
    uint8_t nonce[12];
    quic_packet_nonce(iv, pkt_num, nonce);
    quic_aead_encrypt(key, nonce, out, header_len,
                      out + header_len, payload_len, out + header_len + payload_len);
    size_t total = header_len + payload_len + 16;

    /* Header protection: sample 16 bytes at pn_off + 4. */
    uint8_t mask[5];
    quic_hp_mask(hp, out + pn_off + 4, mask);
    out[0] ^= mask[0] & 0x0f;              /* long header: low 4 bits */
    for (unsigned i = 0; i < pn_len; i++)
        out[pn_off + i] ^= mask[1 + i];
    return total;
}

size_t quic_build_initial(uint8_t *out, size_t cap,
                          const uint8_t *dcid, size_t dcid_len,
                          const uint8_t *scid, size_t scid_len,
                          const uint8_t *token, size_t token_len,
                          uint64_t pkt_num, unsigned pn_len,
                          const uint8_t *payload, size_t payload_len,
                          const uint8_t key[16], const uint8_t iv[12],
                          const uint8_t hp[16]) {
    return quic_build_long(out, cap, 0x00, dcid, dcid_len, scid, scid_len,
                           1, token, token_len, pkt_num, pn_len,
                           payload, payload_len, key, iv, hp);
}

long quic_open_long(uint8_t *pkt, size_t len, int is_initial,
                    const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t hp[16],
                    const uint8_t **out_payload, uint64_t *out_pn,
                    size_t *consumed) {
    if (len < 7 || (pkt[0] & 0xc0) != 0xc0) return -1;   /* long header */
    size_t p = 5;                          /* first byte + version */
    if (p >= len) return -1;
    size_t dl = pkt[p++]; p += dl;         /* DCID */
    if (p >= len) return -1;
    size_t sl = pkt[p++]; p += sl;         /* SCID */
    if (is_initial) {                       /* Initial has a token field */
        uint64_t tl; size_t nt = quic_varint_decode(pkt + p, len - p, &tl);
        if (!nt) return -1; p += nt + (size_t)tl;
    }
    uint64_t length; size_t n = quic_varint_decode(pkt + p, len - p, &length);
    if (!n) return -1; p += n;
    size_t pn_off = p;
    if (pn_off + 4 + 16 > len) return -1;

    /* Remove header protection using the sample at pn_off + 4. */
    uint8_t mask[5];
    quic_hp_mask(hp, pkt + pn_off + 4, mask);
    uint8_t first = pkt[0] ^ (mask[0] & 0x0f);
    unsigned pn_len = (first & 0x03) + 1;
    uint64_t pn = 0;
    for (unsigned i = 0; i < pn_len; i++) {
        uint8_t b = pkt[pn_off + i] ^ mask[1 + i];
        pn = (pn << 8) | b;
    }
    pkt[0] = first;
    for (unsigned i = 0; i < pn_len; i++) pkt[pn_off + i] ^= mask[1 + i];

    size_t header_len = pn_off + pn_len;
    size_t ct_len = (size_t)length - pn_len;   /* ciphertext incl 16 tag */
    if (header_len + ct_len > len || ct_len < 16) return -1;
    size_t body = ct_len - 16;

    uint8_t nonce[12];
    quic_packet_nonce(iv, pn, nonce);
    if (quic_aead_decrypt(key, nonce, pkt, header_len,
                          pkt + header_len, body,
                          pkt + header_len + body) != 0)
        return -1;
    if (out_payload) *out_payload = pkt + header_len;
    if (out_pn) *out_pn = pn;
    if (consumed) *consumed = pn_off + (size_t)length;
    return (long)body;
}

long quic_open_initial(uint8_t *pkt, size_t len,
                       const uint8_t key[16], const uint8_t iv[12],
                       const uint8_t hp[16],
                       const uint8_t **out_payload, uint64_t *out_pn) {
    return quic_open_long(pkt, len, 1, key, iv, hp, out_payload, out_pn, NULL);
}

/* ---- Short-header (1-RTT) packets -------------------------------- */

size_t quic_build_short(uint8_t *out, size_t cap,
                        const uint8_t *dcid, size_t dcid_len,
                        uint64_t pkt_num, unsigned pn_len,
                        const uint8_t *payload, size_t payload_len,
                        const uint8_t key[16], const uint8_t iv[12],
                        const uint8_t hp[16]) {
    if (pn_len < 1 || pn_len > 4) return 0;
    size_t p = 0;
    /* Flags: fixed(0x40) + spin 0 + key phase 0 + pn_len-1. */
    if (1 + dcid_len + pn_len + payload_len + 16 > cap) return 0;
    out[p++] = (uint8_t)(0x40 | (pn_len - 1));
    memcpy(out + p, dcid, dcid_len); p += dcid_len;
    size_t pn_off = p;
    for (unsigned i = 0; i < pn_len; i++)
        out[p++] = (uint8_t)(pkt_num >> (8 * (pn_len - 1 - i)));
    size_t header_len = p;
    memcpy(out + p, payload, payload_len);

    uint8_t nonce[12];
    quic_packet_nonce(iv, pkt_num, nonce);
    quic_aead_encrypt(key, nonce, out, header_len,
                      out + header_len, payload_len, out + header_len + payload_len);
    size_t total = header_len + payload_len + 16;

    /* The HP sample needs pn_off + 4 + 16 <= total; a 1-RTT packet
     * whose payload is too short must pad (caller's job -- ACK-only
     * payloads here are >= 4 bytes with pn_len 4, so total works). */
    if (pn_off + 4 + 16 > total) return 0;
    uint8_t mask[5];
    quic_hp_mask(hp, out + pn_off + 4, mask);
    out[0] ^= mask[0] & 0x1f;              /* short header: low 5 bits */
    for (unsigned i = 0; i < pn_len; i++)
        out[pn_off + i] ^= mask[1 + i];
    return total;
}

long quic_open_short(uint8_t *pkt, size_t len, size_t dcid_len,
                     const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t hp[16],
                     const uint8_t **out_payload, uint64_t *out_pn) {
    if (len < 1 + dcid_len + 4 + 16 + 4) return -1;
    if (pkt[0] & 0x80) return -1;          /* not a short header */
    size_t pn_off = 1 + dcid_len;

    uint8_t mask[5];
    quic_hp_mask(hp, pkt + pn_off + 4, mask);
    uint8_t first = pkt[0] ^ (mask[0] & 0x1f);
    unsigned pn_len = (first & 0x03) + 1;
    uint64_t pn = 0;
    for (unsigned i = 0; i < pn_len; i++) {
        uint8_t b = pkt[pn_off + i] ^ mask[1 + i];
        pn = (pn << 8) | b;
    }
    pkt[0] = first;
    for (unsigned i = 0; i < pn_len; i++) pkt[pn_off + i] ^= mask[1 + i];

    size_t header_len = pn_off + pn_len;
    if (header_len + 16 > len) return -1;
    size_t body = len - header_len - 16;   /* extends to datagram end */

    uint8_t nonce[12];
    quic_packet_nonce(iv, pn, nonce);
    if (quic_aead_decrypt(key, nonce, pkt, header_len,
                          pkt + header_len, body,
                          pkt + header_len + body) != 0)
        return -1;
    if (out_payload) *out_payload = pkt + header_len;
    if (out_pn) *out_pn = pn;
    return (long)body;
}

/* ---- Retry packets ------------------------------------------------ */

int quic_parse_retry(const uint8_t *pkt, size_t len,
                     const uint8_t **out_scid, size_t *out_scid_len,
                     const uint8_t **out_token, size_t *out_token_len) {
    if (len < 7 + 16 || (pkt[0] & 0xf0) != 0xf0) return -1;  /* long + type 3 */
    size_t p = 5;                          /* first byte + version */
    if (p >= len) return -1;
    size_t dl = pkt[p++];                  /* DCID (ours; ignored) */
    if (p + dl >= len) return -1;
    p += dl;
    size_t sl = pkt[p++];                  /* SCID = our next DCID */
    if (p + sl + 16 > len) return -1;
    *out_scid = pkt + p; *out_scid_len = sl;
    p += sl;
    /* Everything up to the final 16-byte integrity tag is the token. */
    *out_token = pkt + p;
    *out_token_len = len - 16 - p;
    return 0;
}

/* ---- Self-test (vs aioquic reference) --------------------------- */

int quic_packet_selftest(void) {
    int pass = 0;

    static const uint8_t dcid[8] =
        { 0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08 };
    uint8_t key[16], iv[12], hp[16];
    quic_initial_keys(dcid, sizeof dcid, 1, key, iv, hp);

    /* CRYPTO frame: offset 0, 32 bytes of data[i] = i*7+3. */
    uint8_t data[32];
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)(i * 7 + 3);
    uint8_t payload[64];
    size_t pl = quic_frame_crypto(payload, sizeof payload, 0, data, 32);

    uint8_t pkt[256];
    size_t plen = quic_build_initial(pkt, sizeof pkt, dcid, 8, NULL, 0,
                                     NULL, 0, 2, 4, payload, pl, key, iv, hp);

    /* aioquic protected this exact packet to a 72-byte value with this
     * SHA-256 (generated with aioquic 1.x, RFC 9001 v1). */
    static const uint8_t exp_sha[32] = {
        0x26,0xe5,0xdf,0xae,0x3e,0xbb,0xe8,0xfd,0x67,0x65,0x9e,0x2e,0x0b,0x32,0x52,0xd3,
        0x3e,0x0b,0xe2,0x77,0x20,0xca,0xa3,0x98,0xd5,0x59,0x73,0xd3,0x2f,0x7f,0x75,0xd0 };
    uint8_t got[32];
    sha256_buf(pkt, plen, got);
    int len_ok = (plen == 72);
    int sha_ok = (memcmp(got, exp_sha, 32) == 0);
    kprintf("[quicpkt] build len=72              %s\n", len_ok ? "OK" : "FAIL");
    kprintf("[quicpkt] protected == aioquic      %s\n", sha_ok ? "OK" : "FAIL");
    pass += len_ok + sha_ok;

    /* Open it back (remove protection) and check the recovered payload. */
    const uint8_t *rpay; uint64_t rpn;
    long rlen = quic_open_initial(pkt, plen, key, iv, hp, &rpay, &rpn);
    int open_ok = (rlen == (long)pl && rpn == 2 &&
                   memcmp(rpay, payload, pl) == 0);
    kprintf("[quicpkt] open round-trip           %s\n", open_ok ? "OK" : "FAIL");
    pass += open_ok;

    /* Parse the CRYPTO frame back out of the recovered payload. */
    struct quic_frame f;
    size_t fn = open_ok ? quic_frame_parse(rpay, (size_t)rlen, &f) : 0;
    int frame_ok = (fn > 0 && f.type == QUIC_FRAME_CRYPTO && f.offset == 0 &&
                    f.len == 32 && memcmp(f.data, data, 32) == 0);
    kprintf("[quicpkt] CRYPTO frame parse        %s\n", frame_ok ? "OK" : "FAIL");
    pass += frame_ok;

    kprintf("[quicpkt] packet self-test: %d/%d %s\n", pass, QUIC_PACKET_SELFTEST_N,
            pass == QUIC_PACKET_SELFTEST_N ? "ALL PASS" : "FAILURES");
    return pass;
}
