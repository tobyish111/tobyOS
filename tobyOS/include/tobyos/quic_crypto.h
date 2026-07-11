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

/* ---- AEAD selection --------------------------------------------- */

/* Which AEAD protects a packet. Initial is ALWAYS AES-128-GCM; the
 * Handshake and 1-RTT levels use whichever suite the TLS handshake
 * negotiated (0x1301 -> AES, 0x1303 -> ChaCha20). */
#define QUIC_AEAD_AES128GCM  0
#define QUIC_AEAD_CHACHA20   1

/* ---- Header protection (RFC 9001 s5.4) -------------------------- */

/* AES-based header-protection mask: mask = AES-ECB(hp_key, sample).
 * QUIC uses the first 5 bytes (1 for the first-byte bits, up to 4 for
 * the packet number). Writes 5 bytes to mask. */
void quic_hp_mask(const uint8_t hp[16], const uint8_t sample[16],
                  uint8_t mask[5]);

/* ChaCha20-based header-protection mask (RFC 9001 s5.4.4): the first 4
 * bytes of the sample are the ChaCha20 block counter and the remaining
 * 12 the nonce; the mask is ChaCha20 applied to 5 zero bytes. hp is a
 * 32-byte key. Writes 5 bytes to mask. */
void quic_hp_mask_chacha(const uint8_t hp[32], const uint8_t sample[16],
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

/* ---- Retry integrity (RFC 9001 s5.8) ----------------------------- */

/* Verify a Retry packet's 16-byte integrity tag: AES-128-GCM with the
 * fixed v1 key/nonce over the "Retry pseudo-packet" (1-byte ODCID
 * length + ODCID + the Retry packet minus its final 16 tag bytes) as
 * AAD, empty plaintext. `pkt`/`len` is the whole received Retry packet
 * (tag included); odcid is the DCID of the client's first Initial.
 * Returns 0 if the tag verifies, -1 otherwise. */
int quic_retry_tag_verify(const uint8_t *pkt, size_t len,
                          const uint8_t *odcid, size_t odcid_len);

/* ---- Self-test -------------------------------------------------- */

/* Verify the whole Initial-crypto path against the RFC 9001 Appendix A
 * vectors + an AES-128-GCM known-answer test. Prints "[quic] ..." to
 * the kernel log and returns the number of PASSing checks (all pass
 * when it equals QUIC_CRYPTO_SELFTEST_N). */
#define QUIC_CRYPTO_SELFTEST_N 9
int quic_crypto_selftest(void);

#endif /* TOBYOS_QUIC_CRYPTO_H */
