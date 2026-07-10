/* tls13.c -- transport-independent TLS 1.3 cryptographic core.
 *
 * The RFC 8446 key schedule + RFC 8439 IETF ChaCha20-Poly1305 AEAD,
 * extracted verbatim from tls.c (stage 13, HTTP/3 groundwork) so the
 * TCP record layer (tls.c) and a future QUIC crypto layer can share
 * them. Pure functions, no transport, no global state. See tls13.h.
 */

#include <tobyos/tls13.h>
#include <tobyos/sec.h>
#include <tobyos/klibc.h>

#include "monocypher.h"

/* ---- HKDF (RFC 5869, SHA-256) ----------------------------------- */

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len,
                        uint8_t out[32]) {
    struct hmac_sha256_ctx ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, msg, msg_len);
    hmac_sha256_final(&ctx, out);
}

void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm, size_t ikm_len, uint8_t out[32]) {
    if (!salt || salt_len == 0) {
        uint8_t zero_salt[32];
        memset(zero_salt, 0, 32);
        hmac_sha256(zero_salt, 32, ikm, ikm_len, out);
    } else {
        hmac_sha256(salt, salt_len, ikm, ikm_len, out);
    }
}

static void hkdf_expand(const uint8_t prk[32],
                        const uint8_t *info, size_t info_len,
                        uint8_t *out, size_t out_len) {
    uint8_t t[32];
    size_t  t_len = 0;
    uint8_t counter = 1;
    size_t  done = 0;

    while (done < out_len) {
        struct hmac_sha256_ctx ctx;
        hmac_sha256_init(&ctx, prk, 32);
        if (t_len > 0) hmac_sha256_update(&ctx, t, t_len);
        hmac_sha256_update(&ctx, info, info_len);
        hmac_sha256_update(&ctx, &counter, 1);
        hmac_sha256_final(&ctx, t);
        t_len = 32;

        size_t copy = out_len - done;
        if (copy > 32) copy = 32;
        memcpy(out + done, t, copy);
        done += copy;
        counter++;
    }
}

/* TLS 1.3 HKDF-Expand-Label:
 * HKDF-Expand(Secret, HkdfLabel, Length) where HkdfLabel is:
 *   uint16 length
 *   opaque label<7..255> = "tls13 " + Label
 *   opaque context<0..255> = Hash.value
 */
void hkdf_expand_label(const uint8_t secret[32],
                       const char *label, size_t label_len,
                       const uint8_t *context, size_t ctx_len,
                       uint8_t *out, size_t out_len) {
    uint8_t info[256];
    size_t pos = 0;

    /* uint16 length */
    info[pos++] = (uint8_t)(out_len >> 8);
    info[pos++] = (uint8_t)(out_len);

    /* label length (including "tls13 " prefix) */
    size_t full_label_len = 6 + label_len;
    info[pos++] = (uint8_t)full_label_len;

    /* "tls13 " prefix */
    info[pos++] = 't'; info[pos++] = 'l'; info[pos++] = 's';
    info[pos++] = '1'; info[pos++] = '3'; info[pos++] = ' ';

    /* actual label */
    memcpy(info + pos, label, label_len);
    pos += label_len;

    /* context length + context */
    info[pos++] = (uint8_t)ctx_len;
    if (ctx_len > 0) {
        memcpy(info + pos, context, ctx_len);
        pos += ctx_len;
    }

    hkdf_expand(secret, info, pos, out, out_len);
}

/* Derive-Secret(Secret, Label, Messages) = HKDF-Expand-Label(Secret, Label, Hash(Messages), 32) */
void derive_secret(const uint8_t secret[32],
                   const char *label, size_t label_len,
                   const uint8_t transcript_hash[32], uint8_t out[32]) {
    hkdf_expand_label(secret, label, label_len, transcript_hash, 32, out, 32);
}

/* Derive handshake traffic keys from the shared secret */
void derive_handshake_keys(const uint8_t shared_secret[32],
                           const uint8_t transcript_hash[32],
                           uint8_t client_hs_key[32], uint8_t client_hs_iv[12],
                           uint8_t server_hs_key[32], uint8_t server_hs_iv[12],
                           uint8_t handshake_secret[32]) {
    /* early_secret = HKDF-Extract(salt=0, IKM=0) */
    uint8_t zero_ikm[32];
    memset(zero_ikm, 0, 32);
    uint8_t early_secret[32];
    hkdf_extract(NULL, 0, zero_ikm, 32, early_secret);

    /* derived_secret = Derive-Secret(early_secret, "derived", Hash("")) */
    uint8_t empty_hash[32];
    sha256_buf(NULL, 0, empty_hash);
    uint8_t derived[32];
    derive_secret(early_secret, "derived", 7, empty_hash, derived);

    /* handshake_secret = HKDF-Extract(derived, shared_secret) */
    hkdf_extract(derived, 32, shared_secret, 32, handshake_secret);

    /* client_handshake_traffic_secret */
    uint8_t c_hs_secret[32];
    derive_secret(handshake_secret, "c hs traffic", 12, transcript_hash, c_hs_secret);

    /* server_handshake_traffic_secret */
    uint8_t s_hs_secret[32];
    derive_secret(handshake_secret, "s hs traffic", 12, transcript_hash, s_hs_secret);

    /* Derive keys and IVs */
    hkdf_expand_label(c_hs_secret, "key", 3, NULL, 0, client_hs_key, 32);
    hkdf_expand_label(c_hs_secret, "iv", 2, NULL, 0, client_hs_iv, 12);
    hkdf_expand_label(s_hs_secret, "key", 3, NULL, 0, server_hs_key, 32);
    hkdf_expand_label(s_hs_secret, "iv", 2, NULL, 0, server_hs_iv, 12);
}

/* Derive application traffic keys */
void derive_app_keys(const uint8_t handshake_secret[32],
                     const uint8_t transcript_hash[32],
                     uint8_t client_app_key[32], uint8_t client_app_iv[12],
                     uint8_t server_app_key[32], uint8_t server_app_iv[12]) {
    /* derived = Derive-Secret(handshake_secret, "derived", Hash("")) */
    uint8_t empty_hash[32];
    sha256_buf(NULL, 0, empty_hash);
    uint8_t derived[32];
    derive_secret(handshake_secret, "derived", 7, empty_hash, derived);

    /* master_secret = HKDF-Extract(derived, 0) */
    uint8_t zero_ikm[32];
    memset(zero_ikm, 0, 32);
    uint8_t master_secret[32];
    hkdf_extract(derived, 32, zero_ikm, 32, master_secret);

    /* traffic secrets */
    uint8_t c_app_secret[32], s_app_secret[32];
    derive_secret(master_secret, "c ap traffic", 12, transcript_hash, c_app_secret);
    derive_secret(master_secret, "s ap traffic", 12, transcript_hash, s_app_secret);

    /* keys and IVs */
    hkdf_expand_label(c_app_secret, "key", 3, NULL, 0, client_app_key, 32);
    hkdf_expand_label(c_app_secret, "iv", 2, NULL, 0, client_app_iv, 12);
    hkdf_expand_label(s_app_secret, "key", 3, NULL, 0, server_app_key, 32);
    hkdf_expand_label(s_app_secret, "iv", 2, NULL, 0, server_app_iv, 12);
}

/* Compute Finished verify_data */
void compute_finished(const uint8_t base_key[32],
                      const uint8_t transcript_hash[32], uint8_t out[32]) {
    uint8_t finished_key[32];
    hkdf_expand_label(base_key, "finished", 8, NULL, 0, finished_key, 32);
    hmac_sha256(finished_key, 32, transcript_hash, 32, out);
}

/* ---- RFC 8439 IETF ChaCha20-Poly1305 ----------------------------- *
 * TLS_CHACHA20_POLY1305_SHA256 uses the IETF AEAD with a 12-byte
 * per-record nonce. Monocypher's crypto_aead_lock/unlock are the
 * XChaCha20 variant taking a nonce[24] -- feeding them the 12-byte TLS
 * nonce both overread the stack and computed a cipher no TLS peer
 * speaks (every record decrypt failed against real servers). Compose
 * the IETF construction from the low-level primitives instead. */

static void tls_poly1305_aead_mac(uint8_t mac[16], const uint8_t poly_key[32],
                                  const uint8_t *ad, size_t ad_len,
                                  const uint8_t *ct, size_t ct_len) {
    static const uint8_t zeros[16] = {0};
    crypto_poly1305_ctx ctx;
    crypto_poly1305_init(&ctx, poly_key);
    crypto_poly1305_update(&ctx, ad, ad_len);
    if (ad_len % 16) crypto_poly1305_update(&ctx, zeros, 16 - ad_len % 16);
    crypto_poly1305_update(&ctx, ct, ct_len);
    if (ct_len % 16) crypto_poly1305_update(&ctx, zeros, 16 - ct_len % 16);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) {
        lens[i]     = (uint8_t)((uint64_t)ad_len >> (8 * i));
        lens[8 + i] = (uint8_t)((uint64_t)ct_len >> (8 * i));
    }
    crypto_poly1305_update(&ctx, lens, 16);
    crypto_poly1305_final(&ctx, mac);
}

void tls_aead_encrypt(uint8_t *ct, uint8_t mac[16],
                      const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *ad, size_t ad_len,
                      const uint8_t *plain, size_t len) {
    uint8_t block0[64];
    crypto_chacha20_ietf(block0, NULL, 64, key, nonce, 0);   /* poly key */
    crypto_chacha20_ietf(ct, plain, len, key, nonce, 1);
    tls_poly1305_aead_mac(mac, block0, ad, ad_len, ct, len);
    crypto_wipe(block0, sizeof(block0));
}

int tls_aead_decrypt(uint8_t *plain, const uint8_t mac[16],
                     const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *ad, size_t ad_len,
                     const uint8_t *ct, size_t len) {
    uint8_t block0[64];
    crypto_chacha20_ietf(block0, NULL, 64, key, nonce, 0);
    uint8_t expect[16];
    tls_poly1305_aead_mac(expect, block0, ad, ad_len, ct, len);
    crypto_wipe(block0, sizeof(block0));
    if (crypto_verify16(expect, mac) != 0) return -1;
    crypto_chacha20_ietf(plain, ct, len, key, nonce, 1);
    return 0;
}
