/* quic_crypto.c -- QUIC crypto primitives (RFC 9000 varint + RFC 9001
 * Initial keys / header protection / packet protection). See
 * quic_crypto.h. Built on tls13.h (HKDF) + vendored BearSSL AES-GCM. */

#include <tobyos/quic_crypto.h>
#include <tobyos/tls13.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#include <bearssl.h>

/* QUIC v1 Initial salt (RFC 9001 s5.2):
 * 0x38762cf7f55934b34d179ae6a4c80cadccbb7f0a. */
static const uint8_t QUIC_V1_SALT[20] = {
    0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
    0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a
};

/* ---- Varints (RFC 9000 s16) ------------------------------------- */

size_t quic_varint_decode(const uint8_t *p, size_t cap, uint64_t *out) {
    if (cap < 1) return 0;
    unsigned pref = p[0] >> 6;             /* top 2 bits = length class */
    size_t n = (size_t)1u << pref;          /* 1, 2, 4, or 8 */
    if (cap < n) return 0;
    uint64_t v = p[0] & 0x3f;
    for (size_t i = 1; i < n; i++) v = (v << 8) | p[i];
    *out = v;
    return n;
}

size_t quic_varint_encode(uint8_t *out, uint64_t v) {
    if (v <= 0x3f) {
        out[0] = (uint8_t)v;
        return 1;
    } else if (v <= 0x3fff) {
        out[0] = 0x40 | (uint8_t)(v >> 8);
        out[1] = (uint8_t)v;
        return 2;
    } else if (v <= 0x3fffffff) {
        out[0] = 0x80 | (uint8_t)(v >> 24);
        out[1] = (uint8_t)(v >> 16);
        out[2] = (uint8_t)(v >> 8);
        out[3] = (uint8_t)v;
        return 4;
    } else if (v <= 0x3fffffffffffffffULL) {
        out[0] = 0xc0 | (uint8_t)(v >> 56);
        for (int i = 1; i < 8; i++) out[i] = (uint8_t)(v >> (56 - 8 * i));
        return 8;
    }
    return 0;
}

/* ---- Initial keys (RFC 9001 s5.2) ------------------------------- */

void quic_initial_keys(const uint8_t *dcid, size_t dcid_len, int is_client,
                       uint8_t key[16], uint8_t iv[12], uint8_t hp[16]) {
    /* initial_secret = HKDF-Extract(salt, client_dcid) */
    uint8_t initial_secret[32];
    hkdf_extract(QUIC_V1_SALT, sizeof QUIC_V1_SALT, dcid, dcid_len,
                 initial_secret);

    /* {client,server}_initial_secret = Expand-Label(initial_secret,
     * "client in"/"server in", "", 32) */
    uint8_t side_secret[32];
    hkdf_expand_label(initial_secret,
                      is_client ? "client in" : "server in", 9,
                      NULL, 0, side_secret, 32);

    /* RFC 9001 s5.1: key/iv/hp from the traffic secret. */
    hkdf_expand_label(side_secret, "quic key", 8, NULL, 0, key, 16);
    hkdf_expand_label(side_secret, "quic iv",  7, NULL, 0, iv,  12);
    hkdf_expand_label(side_secret, "quic hp",  7, NULL, 0, hp,  16);
}

/* ---- Header protection (RFC 9001 s5.4) -------------------------- */

/* mask = AES-ECB(hp, sample). BearSSL's AES-CT exposes CTR, not a raw
 * ECB block, but CTR with counter-block == sample encrypting 16 zero
 * bytes yields the keystream == AES-ECB(sample). The counter block is
 * iv[0..11] || cc(big-endian u32 at 12..15), so split the sample that
 * way. */
void quic_hp_mask(const uint8_t hp[16], const uint8_t sample[16],
                  uint8_t mask[5]) {
    br_aes_ct_ctr_keys bc;
    br_aes_ct_ctr_init(&bc, hp, 16);
    uint32_t cc = ((uint32_t)sample[12] << 24) | ((uint32_t)sample[13] << 16) |
                  ((uint32_t)sample[14] << 8)  |  (uint32_t)sample[15];
    uint8_t block[16];
    memset(block, 0, sizeof block);
    br_aes_ct_ctr_run(&bc, sample, cc, block, 16);   /* iv = sample[0..11] */
    memcpy(mask, block, 5);
}

/* ---- Packet protection AEAD (RFC 9001 s5.3) --------------------- */

void quic_packet_nonce(const uint8_t iv[12], uint64_t pkt_num,
                       uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(pkt_num >> (8 * i));
}

void quic_aead_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       uint8_t *payload, size_t len, uint8_t tag[16]) {
    br_aes_ct_ctr_keys bc;
    br_aes_ct_ctr_init(&bc, key, 16);
    br_gcm_context gc;
    br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul);
    br_gcm_reset(&gc, nonce, 12);
    if (aad_len) br_gcm_aad_inject(&gc, aad, aad_len);
    br_gcm_flip(&gc);
    br_gcm_run(&gc, 1, payload, len);
    br_gcm_get_tag(&gc, tag);
}

int quic_aead_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      uint8_t *payload, size_t len, const uint8_t tag[16]) {
    br_aes_ct_ctr_keys bc;
    br_aes_ct_ctr_init(&bc, key, 16);
    br_gcm_context gc;
    br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul);
    br_gcm_reset(&gc, nonce, 12);
    if (aad_len) br_gcm_aad_inject(&gc, aad, aad_len);
    br_gcm_flip(&gc);
    br_gcm_run(&gc, 0, payload, len);
    return br_gcm_check_tag(&gc, tag) == 1 ? 0 : -1;
}

/* ---- Retry integrity (RFC 9001 s5.8) ----------------------------- */

/* Fixed v1 Retry integrity key + nonce (RFC 9001 s5.8; cross-checked
 * against the aioquic reference RETRY_AEAD_{KEY,NONCE}_VERSION_1). */
static const uint8_t QUIC_V1_RETRY_KEY[16] = {
    0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a,
    0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e
};
static const uint8_t QUIC_V1_RETRY_NONCE[12] = {
    0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63, 0x2b, 0xf2,
    0x23, 0x98, 0x25, 0xbb
};

int quic_retry_tag_verify(const uint8_t *pkt, size_t len,
                          const uint8_t *odcid, size_t odcid_len) {
    if (len < 16 || odcid_len > 20) return -1;
    /* pseudo-packet = odcid_len(1) || odcid || pkt-without-tag
     * (body budget covers a ~256-byte RSA-encrypted retry token) */
    uint8_t pseudo[1 + 20 + 512];
    size_t body = len - 16;
    if (1 + odcid_len + body > sizeof pseudo) return -1;
    pseudo[0] = (uint8_t)odcid_len;
    memcpy(pseudo + 1, odcid, odcid_len);
    memcpy(pseudo + 1 + odcid_len, pkt, body);
    uint8_t tag[16];
    quic_aead_encrypt(QUIC_V1_RETRY_KEY, QUIC_V1_RETRY_NONCE,
                      pseudo, 1 + odcid_len + body, NULL, 0, tag);
    return memcmp(tag, pkt + body, 16) == 0 ? 0 : -1;
}

/* ---- Self-test (RFC 9001 Appendix A + AES-128-GCM KAT) ---------- */

static int eq(const char *what, const uint8_t *got, const uint8_t *exp, size_t n) {
    int ok = memcmp(got, exp, n) == 0;
    kprintf("[quic] %-28s %s\n", what, ok ? "OK" : "FAIL");
    return ok;
}

int quic_crypto_selftest(void) {
    int pass = 0;

    /* RFC 9001 A.1: Destination Connection ID + expected Initial keys. */
    static const uint8_t dcid[8] =
        { 0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08 };
    static const uint8_t exp_ckey[16] =
        { 0x1f,0x36,0x96,0x13,0xdd,0x76,0xd5,0x46,0x77,0x30,0xef,0xcb,0xe3,0xb1,0xa2,0x2d };
    static const uint8_t exp_civ[12] =
        { 0xfa,0x04,0x4b,0x2f,0x42,0xa3,0xfd,0x3b,0x46,0xfb,0x25,0x5c };
    static const uint8_t exp_chp[16] =
        { 0x9f,0x50,0x44,0x9e,0x04,0xa0,0xe8,0x10,0x28,0x3a,0x1e,0x99,0x33,0xad,0xed,0xd2 };
    static const uint8_t exp_skey[16] =
        { 0xcf,0x3a,0x53,0x31,0x65,0x3c,0x36,0x4c,0x88,0xf0,0xf3,0x79,0xb6,0x06,0x7e,0x37 };
    static const uint8_t exp_siv[12] =
        { 0x0a,0xc1,0x49,0x3c,0xa1,0x90,0x58,0x53,0xb0,0xbb,0xa0,0x3e };
    static const uint8_t exp_shp[16] =
        { 0xc2,0x06,0xb8,0xd9,0xb9,0xf0,0xf3,0x76,0x44,0x43,0x0b,0x49,0x0e,0xea,0xa3,0x14 };

    uint8_t key[16], iv[12], hp[16];
    quic_initial_keys(dcid, sizeof dcid, 1, key, iv, hp);
    pass += eq("A.1 client key", key, exp_ckey, 16);
    pass += eq("A.1 client iv",  iv,  exp_civ,  12);
    pass += eq("A.1 client hp",  hp,  exp_chp,  16);
    quic_initial_keys(dcid, sizeof dcid, 0, key, iv, hp);
    pass += eq("A.1 server key", key, exp_skey, 16);
    pass += eq("A.1 server iv",  iv,  exp_siv,  12);
    pass += eq("A.1 server hp",  hp,  exp_shp,  16);

    /* RFC 9001 A.2: header-protection mask = AES-ECB(client hp, sample). */
    static const uint8_t sample[16] =
        { 0xd1,0xb1,0xc9,0x8d,0xd7,0x68,0x9f,0xb8,0xec,0x11,0xd2,0x42,0xb1,0x23,0xdc,0x9b };
    static const uint8_t exp_mask[5] = { 0x43,0x7b,0x9a,0xec,0x36 };
    uint8_t mask[5];
    quic_hp_mask(exp_chp, sample, mask);
    pass += eq("A.2 header-protect mask", mask, exp_mask, 5);

    /* AES-128-GCM KAT (NIST): key=0, iv=0, empty AAD+PT -> known tag. */
    {
        uint8_t k[16] = {0}, n[12] = {0}, t[16];
        static const uint8_t exp_tag[16] =
            { 0x58,0xe2,0xfc,0xce,0xfa,0x7e,0x30,0x61,0x36,0x7f,0x1d,0x57,0xa4,0xe7,0x45,0x5a };
        quic_aead_encrypt(k, n, NULL, 0, NULL, 0, t);
        pass += eq("AES-128-GCM KAT tag", t, exp_tag, 16);
    }

    /* AEAD round-trip (encrypt then decrypt with AAD): tag must verify. */
    {
        uint8_t payload[19];
        for (int i = 0; i < 19; i++) payload[i] = (uint8_t)(i * 7 + 1);
        uint8_t orig[19]; memcpy(orig, payload, 19);
        uint8_t aad[5] = { 0xc3, 0x00, 0x00, 0x00, 0x01 };
        uint8_t nonce[12], tag[16];
        quic_packet_nonce(iv, 2, nonce);
        quic_aead_encrypt(key, nonce, aad, sizeof aad, payload, 19, tag);
        int rt = quic_aead_decrypt(key, nonce, aad, sizeof aad, payload, 19, tag);
        int ok = (rt == 0) && memcmp(payload, orig, 19) == 0;
        kprintf("[quic] %-28s %s\n", "AEAD round-trip", ok ? "OK" : "FAIL");
        if (ok) pass++;
    }

    kprintf("[quic] crypto self-test: %d/%d %s\n", pass, QUIC_CRYPTO_SELFTEST_N,
            pass == QUIC_CRYPTO_SELFTEST_N ? "ALL PASS" : "FAILURES");
    return pass;
}
