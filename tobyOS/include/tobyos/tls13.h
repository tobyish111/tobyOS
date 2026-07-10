/* tls13.h -- transport-independent TLS 1.3 cryptographic core.
 *
 * Extracted from tls.c (stage 13, HTTP/3 groundwork). This is the part
 * of TLS 1.3 that is NOT tied to the TCP record layer: the RFC 8446
 * key schedule (HKDF-Expand-Label, Derive-Secret, the handshake/app
 * traffic keys, the Finished MAC) and the RFC 8439 IETF
 * ChaCha20-Poly1305 AEAD. QUIC (HTTP/3) reuses exactly these pieces --
 * the same key schedule feeds QUIC's packet-protection keys, and the
 * same AEAD protects QUIC packets -- while carrying handshake bytes in
 * CRYPTO frames instead of TLS records. tls.c owns the record framing
 * and the connection state machine; a future quic.c owns its own
 * framing but drives this same core.
 *
 * All functions are pure (no I/O, no global state) and single-suite:
 * TLS_CHACHA20_POLY1305_SHA256, the one suite tobyOS implements.
 */

#ifndef TOBYOS_TLS13_H
#define TOBYOS_TLS13_H

#include <tobyos/types.h>

/* ---- Wire helpers (shared by the record layer + QUIC framing) ---- */

static inline void tls_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void tls_put_u24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}
static inline uint16_t tls_get_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t tls_get_u24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* ---- HKDF / key schedule (RFC 8446 s7.1, SHA-256) ---------------- */

/* HKDF-Extract(salt, IKM) -> 32-byte PRK. salt==NULL/len 0 uses a
 * zero salt (the early-secret case). */
void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm, size_t ikm_len, uint8_t out[32]);

/* HKDF-Expand-Label(secret, "tls13 "+label, context, out_len). */
void hkdf_expand_label(const uint8_t secret[32],
                       const char *label, size_t label_len,
                       const uint8_t *context, size_t ctx_len,
                       uint8_t *out, size_t out_len);

/* Derive-Secret(secret, label, transcript_hash) -> 32 bytes. */
void derive_secret(const uint8_t secret[32],
                   const char *label, size_t label_len,
                   const uint8_t transcript_hash[32], uint8_t out[32]);

/* Handshake traffic keys from the X25519 shared secret + the
 * CH..SH transcript hash. Also returns the handshake_secret for the
 * later app-key derivation. */
void derive_handshake_keys(const uint8_t shared_secret[32],
                           const uint8_t transcript_hash[32],
                           uint8_t client_hs_key[32], uint8_t client_hs_iv[12],
                           uint8_t server_hs_key[32], uint8_t server_hs_iv[12],
                           uint8_t handshake_secret[32]);

/* Application traffic keys from the handshake_secret + the
 * CH..server-Finished transcript hash. */
void derive_app_keys(const uint8_t handshake_secret[32],
                     const uint8_t transcript_hash[32],
                     uint8_t client_app_key[32], uint8_t client_app_iv[12],
                     uint8_t server_app_key[32], uint8_t server_app_iv[12]);

/* Finished verify_data = HMAC(finished_key, transcript_hash). */
void compute_finished(const uint8_t base_key[32],
                      const uint8_t transcript_hash[32], uint8_t out[32]);

/* ---- IETF ChaCha20-Poly1305 AEAD (RFC 8439) ---------------------- */

/* Encrypt `plain[len]` -> `ct[len]` + `mac[16]` under key/nonce with
 * additional data ad[ad_len]. */
void tls_aead_encrypt(uint8_t *ct, uint8_t mac[16],
                      const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *ad, size_t ad_len,
                      const uint8_t *plain, size_t len);

/* Decrypt + verify. Returns 0 on success, -1 on MAC mismatch (same
 * contract as crypto_aead_unlock). */
int tls_aead_decrypt(uint8_t *plain, const uint8_t mac[16],
                     const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *ad, size_t ad_len,
                     const uint8_t *ct, size_t len);

#endif /* TOBYOS_TLS13_H */
