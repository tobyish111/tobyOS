/* http.h -- minimal HTTP/1.0 client (Milestone 24D).
 *
 * Tiny synchronous "fetch a URL into a kmalloc buffer" client built
 * directly on top of dns_resolve() + tcp_connect()/tcp_send()/tcp_recv().
 *
 * What we implement:
 *   - URL parser for "http://host[:port][/path]" (no fragments, no
 *     query-string special handling -- everything after the host is
 *     forwarded verbatim as the request-target).
 *   - HTTP GET over a fresh TCP connection.
 *   - Status line + header parsing (case-insensitive header keys).
 *   - Content-Length-bounded body collection with a hard ceiling.
 *   - Body collection by reading until peer FIN when no
 *     Content-Length was supplied (the common "Connection: close +
 *     no length" path that HTTP/1.0 servers and many 1.1 servers use).
 *
 * What we DON'T implement (not required by 24D):
 *   - HTTPS / TLS.
 *   - HTTP/1.1 chunked transfer encoding (we detect it and refuse).
 *   - Redirects (3xx returns the response as-is so the caller can log).
 *   - Persistent connections / pipelining -- we always send
 *     "Connection: close".
 *   - Authentication, cookies, gzip / deflate.
 *
 * Threading: synchronous, polling, no scheduler dependency. Same
 * shape as dhcp.c / dns.c / tcp.c -- safe to call from boot and from
 * the kernel shell.
 */

#ifndef TOBYOS_HTTP_H
#define TOBYOS_HTTP_H

#include <tobyos/types.h>

/* Default timeouts / limits. Callers may pass smaller values; pass 0
 * to use the defaults. */
#define HTTP_DEFAULT_TIMEOUT_MS    5000u
#define HTTP_MAX_HEADER_BYTES      8192u    /* cap on total header block */
#define HTTP_MAX_URL_LEN           512u
#define HTTP_MAX_HOST_LEN          128u
#define HTTP_MAX_PATH_LEN          384u

/* Negative error codes returned by http_get(). */
#define HTTP_ERR_URL          -1   /* malformed URL or unsupported scheme  */
#define HTTP_ERR_DNS          -2   /* dns_resolve() failed                 */
#define HTTP_ERR_CONNECT      -3   /* tcp_connect() failed                 */
#define HTTP_ERR_PROTOCOL     -4   /* malformed status line / header block */
#define HTTP_ERR_CHUNKED      -5   /* Transfer-Encoding: chunked (no impl) */
#define HTTP_ERR_TOOBIG       -6   /* response > max_body_bytes            */
#define HTTP_ERR_TIMEOUT      -7   /* tcp_recv timed out mid-response      */
#define HTTP_ERR_NOMEM        -8   /* kmalloc failed                       */
#define HTTP_ERR_RESET        -9   /* peer RST mid-response                */

/* Parsed URL components. Strings are NUL-terminated. */
struct http_url {
    char     host[HTTP_MAX_HOST_LEN];
    char     path[HTTP_MAX_PATH_LEN];
    uint16_t port;            /* host order; default 80 if absent */
};

/* Result of a successful (or partially successful) HTTP fetch. body
 * is kmalloc'd; the caller must release it with http_free() once
 * done -- even on error returns where status != 0. */
struct http_response {
    int        status;                  /* e.g. 200, 404, 500            */
    char       reason[64];              /* status-line reason phrase     */
    char       content_type[64];        /* "Content-Type:" if present    */
    size_t     body_len;                /* bytes actually downloaded     */
    uint8_t   *body;                    /* kmalloc'd; may be NULL on err */
};

/* Parse a URL string into `out`. Accepts:
 *   http://host
 *   http://host/
 *   http://host/some/path?with=query
 *   http://host:8080
 *   http://host:8080/
 *   http://10.0.2.2:8000/foo
 *
 * Returns 0 on success, HTTP_ERR_URL on failure. The scheme MUST be
 * "http://" (case-insensitive). Userinfo (user@host), IPv6 literals
 * ([::1]) and explicit fragments are rejected. */
int http_parse_url(const char *url, struct http_url *out);

/* Synchronous GET. Resolves DNS (or parses a dotted-quad), opens a
 * fresh TCP connection, sends a GET, parses the response, and fills
 * *out. Caller is responsible for http_free(out) on every successful
 * (status != 0) return.
 *
 * timeout_ms is applied independently to each tcp_recv() call. Pass
 * 0 to use HTTP_DEFAULT_TIMEOUT_MS.
 *
 * max_body_bytes caps the total response body size. The fetch is
 * aborted with HTTP_ERR_TOOBIG once that ceiling is exceeded.
 *
 * Return value:
 *     0 on success (out->status holds the HTTP status code).
 *   < 0 on failure (one of HTTP_ERR_*). out is left zeroed in that
 *       case; no need to call http_free(). */
int http_get(const char *url,
             size_t      max_body_bytes,
             uint32_t    timeout_ms,
             struct http_response *out);

/* Free an http_response. NULL-safe; idempotent (clears the struct
 * after free so a double-free becomes a no-op). */
void http_free(struct http_response *r);

/* Render a HTTP_ERR_* code into a static string for logging. */
const char *http_strerror(int err);

/* Boot-time self-test for milestone 24D. Drives the full
 *   httpget → pkg_install_url → pkg_remove
 * cycle against an HTTP server expected to be running on the SLIRP
 * host (10.0.2.2:8000) and serving:
 *
 *   /m24d_smoke.txt   -- the literal ASCII bytes "tobyOS-m24d-ok\n"
 *   /helloapp.tpkg    -- the helloapp v1.0 package built by the Makefile
 *
 * Only compiled in when HTTP_M24D_SELFTEST is defined at build time
 * (see the `make m24dtest` target); otherwise this is a no-op stub.
 * Prints "[m24d-selftest] SUCCESS" on success and "[m24d-selftest]
 * FAIL ..." on any failure -- the test driver greps for these. */
void http_m24d_selftest(void);

#endif /* TOBYOS_HTTP_H */
