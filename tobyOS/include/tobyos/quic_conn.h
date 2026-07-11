/* quic_conn.h -- QUIC handshake first-flight assembly (HTTP/3 slice 4).
 *
 * Builds a real TLS 1.3 ClientHello configured for QUIC (X25519
 * key_share, ALPN "h3", and the mandatory quic_transport_parameters
 * extension), wraps it in a CRYPTO frame inside a client Initial
 * packet (quic_packet.h), and protects it with the Initial keys
 * (quic_crypto.h). This is the packet a QUIC client sends first.
 *
 * The connection state machine + UDP live in later slices; this slice
 * produces and verifies the outbound Initial (deterministically, and
 * interoperably against the aioquic reference). */

#ifndef TOBYOS_QUIC_CONN_H
#define TOBYOS_QUIC_CONN_H

#include <tobyos/types.h>

/* Build a TLS 1.3 ClientHello for QUIC into out (>= ~512 bytes).
 * random[32] + the X25519 public key are the caller's; scid is echoed
 * in the transport parameters (initial_source_connection_id). hostname
 * may be NULL. fc_window sizes the response flow-control limits
 * (initial_max_data / stream_data_bidi_local). Returns the ClientHello
 * length (including its 4-byte handshake header), or 0 on overflow. */
size_t quic_build_client_hello(uint8_t *out, size_t cap,
                               const uint8_t random[32],
                               const uint8_t pubkey[32],
                               const uint8_t *scid, size_t scid_len,
                               const char *hostname, uint32_t fc_window);

/* Build a complete, protected client Initial packet carrying `ch`
 * (a ClientHello) in a CRYPTO frame, padded to `pad_to` bytes (0 = no
 * padding; live sends use 1200). dcid/scid are the connection IDs;
 * the Initial keys are derived from dcid internally. Returns the
 * protected packet length in *out, or 0 on failure. */
size_t quic_build_client_initial(uint8_t *out, size_t cap,
                                  const uint8_t *dcid, size_t dcid_len,
                                  const uint8_t *scid, size_t scid_len,
                                  const uint8_t *ch, size_t ch_len,
                                  uint64_t pkt_num, size_t pad_to);

/* Self-test: build a ClientHello Initial with fixed inputs, round-trip
 * it through the packet layer, and dump the ClientHello + full Initial
 * as hex to the log for offline interop checking (aioquic). Prints
 * "[quicch] ..." and returns the PASS count. */
#define QUIC_CONN_SELFTEST_N 3
int quic_conn_selftest(void);

/* Slices 4b..5b: a reusable HTTP/3 GET over QUIC v1. Runs the full
 * handshake to ip_be:port (Version Negotiation / Retry / coalesced
 * packets / CRYPTO reassembly / ACKs), REQUIRES the cert chain +
 * CertificateVerify to validate against `host` (the 13H path), then
 * GETs `path` as HTTP/3 (QPACK HEADERS on bidi stream 0) and fills
 * `out` (status / content-type / content-length / content-encoding /
 * kmalloc'd body -- free with http_free). flags takes HTTP_F_* (GZIP
 * advertises accept-encoding; TRUNCATE caps the body at max_body).
 * Returns 0 on success, a negative HTTP_ERR_* otherwise. Synchronous,
 * one fetch at a time (fixed UDP source port). */
struct http_response;
int http3_fetch(uint32_t ip_be, uint16_t port, const char *host,
                const char *path, unsigned flags, size_t max_body,
                uint32_t timeout_ms, struct http_response *out);

/* Boot-time on-the-wire test: http3_fetch against the aioquic server
 * on the SLIRP host (10.0.2.2:4433, tobyos.test). Called at boot under
 * -DQUIC_SEND_TEST (trust the test CA with -DTLS_TEST_CA). */
int quic_udp_send_test(void);

#endif /* TOBYOS_QUIC_CONN_H */
