/* tls_x509.h -- transport-independent TLS 1.3 certificate authentication.
 *
 * The X.509 chain validation + CertificateVerify verification, lifted
 * out of tls.c (stage 13, HTTP/3 slice 4f) so both the TLS-over-TCP
 * record layer and the QUIC handshake can authenticate the peer with
 * the same code. Built on the vendored BearSSL x509_minimal engine +
 * the stage-13H trust store (src/tls_trust.c).
 *
 * Both halves are required (see the stage-13H rationale): chain
 * validation proves the leaf chains to a trusted root and matches the
 * hostname; CertificateVerify proves the peer holds the leaf key by
 * signing THIS handshake's transcript. */

#ifndef TOBYOS_TLS_X509_H
#define TOBYOS_TLS_X509_H

#include <tobyos/types.h>
#include <bearssl.h>

/* Validate a TLS 1.3 Certificate message (`msg`, `msglen`) against the
 * built-in trust store, checking `hostname` (SAN/CN) and expiry. On
 * success (return 0) copies the leaf public key into *out_pk / pk_buf
 * (pk_buf must hold BR_X509_BUFSIZE_KEY bytes). Returns a negative
 * value on any failure (untrusted / malformed / expired / no memory);
 * fail closed. */
int tls_x509_validate_chain(const uint8_t *msg, size_t msglen,
                            const char *hostname,
                            br_x509_pkey *out_pk,
                            unsigned char *pk_buf, size_t pk_buf_cap);

/* Verify a TLS 1.3 CertificateVerify signature (RFC 8446 4.4.3): the
 * peer signed 64*0x20 || "TLS 1.3, server CertificateVerify" || 0x00 ||
 * transcript_hash with the leaf key. Returns 1 if valid, 0 otherwise.
 * ECDSA (P-256/384/521) + RSA-PSS (SHA-256/384/512). */
int tls_x509_verify_cv(uint16_t scheme, const uint8_t *sig, size_t siglen,
                       const uint8_t thash[32], const br_x509_pkey *pk);

#endif /* TOBYOS_TLS_X509_H */
