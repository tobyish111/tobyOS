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
/* Opaque per-CONNECTION h2 state (HPACK dynamic table, stream ids,
 * read buffer). Park it with the connection to reuse it. */
struct h2;
void http2_state_free(struct h2 *h);

/* GET over a reusable h2 connection. *ph == NULL => fresh connection;
 * on success *ph is state the caller may park. On failure it is freed
 * and *ph set to NULL. */
int http2_fetch_on(struct h2 **ph, struct tls_conn *tls,
                   const struct http_url *u, unsigned flags, size_t max_body,
                   uint32_t timeout_ms, struct http_response *out);

int http2_fetch(struct tls_conn *tls, const struct http_url *u,
                unsigned flags, size_t max_body, uint32_t timeout_ms,
                struct http_response *out);

#endif /* TOBYOS_HTTP2_H */
