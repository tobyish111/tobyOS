/* tls.h -- Minimal TLS 1.3 client for tobyOS.
 *
 * Provides a thin transport-encryption layer over TCP using:
 *   - X25519 key exchange (via Monocypher)
 *   - ChaCha20-Poly1305 AEAD (via Monocypher)
 *   - SHA-256 transcript hashing (via sec.h)
 *   - HKDF-Expand-Label key derivation
 *
 * Limitations (hobby OS scope):
 *   - TLS 1.3 only (no TLS 1.2 fallback)
 *   - Single cipher suite: TLS_CHACHA20_POLY1305_SHA256 (0x1303)
 *   - No certificate verification (accepts any server cert)
 *   - No client certificates
 *   - No session resumption / PSK / 0-RTT
 *   - No SNI extension (basic)
 *   - Single-threaded, synchronous, polling-based
 */

#ifndef TOBYOS_TLS_H
#define TOBYOS_TLS_H

#include <tobyos/types.h>

struct tcp_conn;

/* TLS connection state. Opaque to callers; use the API below. */
struct tls_conn;

/* Error codes */
#define TLS_OK               0
#define TLS_ERR_CONNECT     -1   /* TCP connect failed */
#define TLS_ERR_HANDSHAKE   -2   /* TLS handshake failed */
#define TLS_ERR_SEND        -3   /* failed to send data */
#define TLS_ERR_RECV        -4   /* failed to receive / timeout */
#define TLS_ERR_CLOSED      -5   /* peer sent close_notify or FIN */
#define TLS_ERR_NOMEM       -6   /* allocation failure */
#define TLS_ERR_ALERT       -7   /* peer sent a fatal alert */
#define TLS_ERR_RECORD      -8   /* malformed record */
#define TLS_ERR_VERSION     -9   /* unsupported protocol version */

/* Connect to a TLS server. Performs TCP connect + TLS 1.3 handshake.
 * When offer_h2 is nonzero the ClientHello advertises ALPN
 * ["h2","http/1.1"]; query the result with tls_alpn() (stage 13G).
 * Returns a tls_conn on success, NULL on failure (check *out_err). */
struct tls_conn *tls_connect(uint32_t dst_ip_be, uint16_t dst_port_be,
                             const char *hostname,
                             uint32_t timeout_ms, int offer_h2, int *out_err);

/* The ALPN protocol the server selected ("h2", "http/1.1"), or "" if
 * none was offered/selected. Valid after a successful tls_connect. */
const char *tls_alpn(struct tls_conn *c);

/* Send application data. Returns bytes sent or negative error. */
long tls_send(struct tls_conn *c, const void *buf, size_t len);

/* Receive application data. Returns bytes received or negative error.
 * May return less than `cap` (short read). Returns 0 on clean close. */
long tls_recv(struct tls_conn *c, void *buf, size_t cap, uint32_t timeout_ms);

/* Close the TLS connection (sends close_notify, then TCP close). */
void tls_close(struct tls_conn *c);

/* Human-readable error string. */
const char *tls_strerror(int err);

#endif /* TOBYOS_TLS_H */
