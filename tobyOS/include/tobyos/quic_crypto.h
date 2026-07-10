/* quic_crypto.h -- QUIC (RFC 9000/9001) crypto primitives for tobyOS.
 *
 * The first slice of the QUIC transport under HTTP/3: everything QUIC
 * needs from cryptography, built on the transport-independent TLS 1.3
 * key schedule (tls13.h, HKDF/Expand-Label) plus AES-128-GCM +
 * AES-ECB header protection (vendored BearSSL). QUIC Initial packets
 * are ALWAYS AES-128-GCM regardless of the negotiated suite (RFC 9001
 * s5.2), so this is mandatory even though our TLS-over-TCP path is
 * ChaCha20-Poly1305.
 *
 * This module is pure crypto + varint (no UDP, no packets, no state);
 * the packet/frame/handshake layers build on it in later slices. It's
 * verified deterministically against the RFC 9001 Appendix A test
 * vectors by quic_crypto_selftest() (no live server needed). */

#ifndef TOBYOS_QUIC_CRYPTO_H
#define TOBYOS_QUIC_CRYPTO_H

#include <tobyos/types.h>

/* ---- Variable-length integers (RFC 9000 s16) -------------------- */

/* Decode a QUIC varint at p (max `cap` bytes). Stores the value in
 * *out and returns the number of bytes consumed (1/2/4/8), or 0 if
 * `cap` is too small for the encoded length. */
size_t quic_varint_decode(const uint8_t *p, size_t cap, uint64_t *out);

/* Encode `v` as a QUIC varint into `out` (must hold >= 8 bytes).
 * Returns bytes written (1/2/4/8), or 0 if v exceeds 2^62-1. */
size_t quic_varint_encode(uint8_t *out, uint64_t v);

/* ---- Initial keys (RFC 9001 s5.2) ------------------------------- */

/* Derive the AES-128-GCM Initial packet-protection keys for one side
 * from the client's Destination Connection ID. is_client selects the
 * "client in" vs "server in" label. key[16], iv[12], hp[16]. */
void quic_initial_keys(const uint8_t *dcid, size_t dcid_len, int is_client,
                       uint8_t key[16], uint8_t iv[12], uint8_t hp[16]);

/* ---- Header protection (RFC 9001 s5.4) -------------------------- */

/* AES-based header-protection mask: mask = AES-ECB(hp_key, sample).
 * QUIC uses the first 5 bytes (1 for the first-byte bits, up to 4 for
 * the packet number). Writes 5 bytes to mask. */
void quic_hp_mask(const uint8_t hp[16], const uint8_t sample[16],
                  uint8_t mask[5]);

/* ---- Packet protection AEAD (RFC 9001 s5.3, AES-128-GCM) --------- */

/* Build the per-packet nonce = iv XOR (packet number, right-aligned
 * big-endian in the low 8 bytes). */
void quic_packet_nonce(const uint8_t iv[12], uint64_t pkt_num,
                       uint8_t nonce[12]);

/* Encrypt `len` bytes in place (payload) and append a 16-byte tag to
 * `tag`. `aad` is the (unprotected) packet header. */
void quic_aead_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       uint8_t *payload, size_t len, uint8_t tag[16]);

/* Decrypt + verify. Returns 0 on success (payload decrypted in place),
 * -1 on tag mismatch. */
int quic_aead_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      uint8_t *payload, size_t len, const uint8_t tag[16]);

/* ---- Self-test -------------------------------------------------- */

/* Verify the whole Initial-crypto path against the RFC 9001 Appendix A
 * vectors + an AES-128-GCM known-answer test. Prints "[quic] ..." to
 * the kernel log and returns the number of PASSing checks (all pass
 * when it equals QUIC_CRYPTO_SELFTEST_N). */
#define QUIC_CRYPTO_SELFTEST_N 9
int quic_crypto_selftest(void);

#endif /* TOBYOS_QUIC_CRYPTO_H */
