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

size_t quic_frame_parse(const uint8_t *p, size_t cap, struct quic_frame *f) {
    if (cap < 1) return 0;
    memset(f, 0, sizeof *f);
    f->type = p[0];
    if (p[0] == QUIC_FRAME_PADDING) {
        size_t n = 1;
        while (n < cap && p[n] == QUIC_FRAME_PADDING) n++;
        return n;                          /* collapse the PADDING run */
    }
    if (p[0] == QUIC_FRAME_PING) return 1;
    if (p[0] == QUIC_FRAME_CRYPTO) {
        size_t o = 1, n;
        n = quic_varint_decode(p + o, cap - o, &f->offset); if (!n) return 0; o += n;
        n = quic_varint_decode(p + o, cap - o, &f->len);    if (!n) return 0; o += n;
        if (o + f->len > cap) return 0;
        f->data = p + o;
        return o + (size_t)f->len;
    }
    if (p[0] == QUIC_FRAME_ACK) {
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
        return o;
    }
    return 0;                              /* unhandled frame type */
}

/* ---- Initial packets -------------------------------------------- */

size_t quic_build_initial(uint8_t *out, size_t cap,
                          const uint8_t *dcid, size_t dcid_len,
                          const uint8_t *scid, size_t scid_len,
                          const uint8_t *token, size_t token_len,
                          uint64_t pkt_num, unsigned pn_len,
                          const uint8_t *payload, size_t payload_len,
                          const uint8_t key[16], const uint8_t iv[12],
                          const uint8_t hp[16]) {
    if (pn_len < 1 || pn_len > 4) return 0;
    size_t p = 0;
    /* First byte: long(0x80) + fixed(0x40) + Initial(type 00) + pn_len-1. */
    if (p >= cap) return 0;
    out[p++] = (uint8_t)(0xc0 | (pn_len - 1));
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
    /* token length (varint) + token */
    uint8_t vt[8];
    size_t nvt = quic_varint_encode(vt, token_len);
    if (p + nvt + token_len > cap) return 0;
    memcpy(out + p, vt, nvt); p += nvt;
    if (token_len) { memcpy(out + p, token, token_len); p += token_len; }
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
