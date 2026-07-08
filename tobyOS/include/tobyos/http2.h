/* http2.h -- minimal HTTP/2 client (stage 13G). One GET over an
 * ALPN-negotiated "h2" TLS connection, producing a struct http_response
 * (body decompressed like the h1 path). http_get_opt() uses this for h2
 * servers and falls back to HTTP/1.1 on any failure. */
#ifndef TOBYOS_HTTP2_H
#define TOBYOS_HTTP2_H

#include <tobyos/types.h>
#include <tobyos/http.h>

struct tls_conn;

/* Fetch u over an already-connected h2 TLS conn. flags carries
 * HTTP_F_GZIP (advertise gzip+br). Fills out (caller frees out->body via
 * http_free). Returns 0 on success, negative HTTP_ERR_* on failure. */
int http2_fetch(struct tls_conn *tls, const struct http_url *u,
                unsigned flags, size_t max_body, uint32_t timeout_ms,
                struct http_response *out);

#endif /* TOBYOS_HTTP2_H */
