/* http.h -- small synchronous HTTP client (Milestone 24D + browser depth).
 *
 * "Fetch a URL into a kmalloc buffer" client built directly on top of
 * dns_resolve() + tcp_connect()/tcp_send()/tcp_recv() (+ tls.c).
 *
 * What we implement:
 *   - URL parser for "http(s)://host[:port][/path]" (no fragments, no
 *     query-string special handling -- everything after the host is
 *     forwarded verbatim as the request-target).
 *   - HTTP GET; HTTPS via the TLS 1.3 client.
 *   - Status line + header parsing (case-insensitive header keys);
 *     1xx interim responses (e.g. 103 Early Hints) are skipped.
 *   - Content-Length-bounded body collection with a hard ceiling.
 *   - HTTP/1.1 chunked transfer decoding (dechunked kernel-side, the
 *     caller always sees the payload bytes).
 *   - Body collection by reading until peer FIN when no
 *     Content-Length was supplied.
 *   - Session cookies, gzip (HTTP_F_GZIP), truncate-at-cap semantics
 *     (HTTP_F_TRUNCATE).
 *   - HTTP/1.1 keep-alive (HTTP_F_KEEPALIVE): a small per-(host,port,
 *     scheme) connection cache skips the TCP+TLS handshake on repeated
 *     fetches (stylesheets, scripts, images). Without the flag every
 *     request is HTTP/1.0 + "Connection: close" (wget/pkg contract).
 *
 * Not implemented: pipelining, authentication, deflate/brotli,
 * TLS session resumption.
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
#define HTTP_GZIP_READ_MAX         (512u*1024u) /* compressed body read cap */
#define HTTP_MAX_URL_LEN           512u
#define HTTP_MAX_HOST_LEN          128u
/* 496: Wikipedia's combined Vector-skin load.php stylesheet URL carries
 * a path measured at 458 chars on the Cat article (the module list
 * varies per page); at the old 384 the skin sheet was rejected as a
 * bad URL and articles rendered unstyled. Total URL stays bounded by
 * the 512-char syscall buffer. */
#define HTTP_MAX_PATH_LEN          496u

/* Negative error codes returned by http_get(). */
#define HTTP_ERR_URL          -1   /* malformed URL or unsupported scheme  */
#define HTTP_ERR_DNS          -2   /* dns_resolve() failed                 */
#define HTTP_ERR_CONNECT      -3   /* tcp_connect() failed                 */
#define HTTP_ERR_PROTOCOL     -4   /* malformed status line / header block */
#define HTTP_ERR_CHUNKED      -5   /* legacy (chunked decodes now); kept so
                                    * error tables stay index-stable      */
#define HTTP_ERR_TOOBIG       -6   /* response > max_body_bytes            */
#define HTTP_ERR_TIMEOUT      -7   /* tcp_recv timed out mid-response      */
#define HTTP_ERR_NOMEM        -8   /* kmalloc failed                       */
#define HTTP_ERR_RESET        -9   /* peer RST mid-response                */
#define HTTP_ERR_CERT        -10   /* TLS certificate validation failed (13H) */

/* Parsed URL components. Strings are NUL-terminated. */
struct http_url {
    char     host[HTTP_MAX_HOST_LEN];
    char     path[HTTP_MAX_PATH_LEN];
    uint16_t port;            /* host order; default 80/443 if absent */
    uint8_t  tls;             /* 1 if https://, 0 if http:// */
};

/* Result of a successful (or partially successful) HTTP fetch. body
 * is kmalloc'd; the caller must release it with http_free() once
 * done -- even on error returns where status != 0. */
struct http_response {
    int        status;                  /* e.g. 200, 404, 500            */
    char       reason[64];              /* status-line reason phrase     */
    char       content_type[64];        /* "Content-Type:" if present    */
    char       location[256];           /* "Location:" for 3xx redirects */
    size_t     body_len;                /* bytes actually downloaded     */
    uint8_t   *body;                    /* kmalloc'd; may be NULL on err */
    long       content_len;             /* Content-Length header, -1 if absent */
    uint8_t    encoding;                /* HTTP_ENC_* Content-Encoding    */
};

/* Content-Encoding of the body (the client is responsible for inflating
 * a gzip body -- http_get does NOT decompress, it only advertises the
 * capability and reports what came back). */
#define HTTP_ENC_IDENTITY 0
#define HTTP_ENC_GZIP     1
#define HTTP_ENC_BR       2   /* Content-Encoding: br (RFC 7932 brotli) */

/* http_get_opt() flags. */
#define HTTP_F_TRUNCATE  0x1u  /* body > max_body_bytes: stop reading at the
                                * cap and return success with a truncated
                                * body (content_len still reports the full
                                * size) instead of HTTP_ERR_TOOBIG. The
                                * browser path wants this -- it only renders
                                * the first N KiB anyway; downloads (wget,
                                * pkg) must keep the exact-or-error contract. */
#define HTTP_F_GZIP      0x2u  /* advertise Accept-Encoding: gzip; a gzip
                                * response comes back RAW with encoding set
                                * (the caller inflates). Off => identity. */
#define HTTP_F_KEEPALIVE 0x4u  /* speak HTTP/1.1 and try to reuse a cached
                                * connection to (host, port, scheme); park
                                * the connection for the next fetch when
                                * the response framing allows it. Off =>
                                * HTTP/1.0 + Connection: close (the exact
                                * wget/pkg download contract). */
#define HTTP_F_TRY_H3    0x8u  /* for https, probe HTTP/3 (QUIC) first;
                                * on any h3 failure fall back to the
                                * h2/h1.1 ladder (GET is idempotent). Off
                                * => never attempt QUIC. v1 h3 caps the
                                * response at 256 KiB and speaks
                                * AES-128-GCM only, so this is an opt-in. */

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

/* http_get with behaviour flags (HTTP_F_*). http_get() == flags 0. */
int http_get_opt(const char *url,
                 size_t      max_body_bytes,
                 uint32_t    timeout_ms,
                 unsigned    flags,
                 struct http_response *out);

/* http_get_opt + redirect following (up to 5, relative Locations
 * resolved). cur_url is rewritten in place to the URL that produced
 * the final response. timeout_ms is per-recv (0 = default). Lives in
 * syscall.c; exported for the async-HTTP worker (http_async.c). */
int http_get_follow(char cur_url[512], size_t max_body, unsigned flags,
                    uint32_t timeout_ms, struct http_response *resp);

/* Free an http_response. NULL-safe; idempotent (clears the struct
 * after free so a double-free becomes a no-op). */
void http_free(struct http_response *r);

/* Cookie jar (RFC 6265-lite): session cookies kept in memory, keyed by
 * host, sent back on same-host requests. No Domain/Path/Expiry/Secure
 * semantics yet -- enough for login/session continuity within a site.
 * Managed automatically by http_get; exposed for tests/inspection. */
void cookie_jar_clear(void);
int  cookie_jar_count(void);

/* Keep-alive connection cache: close every parked connection (e.g. on
 * link down) and report reuse statistics for tests/inspection. */
void http_keepalive_flush(void);
void http_keepalive_stats(unsigned *out_reused, unsigned *out_handshakes);

/* Render a HTTP_ERR_* code into a static string for logging. */
const char *http_strerror(int err);

/* Record an Alt-Svc (RFC 7838) header value for `host` (used by the h2
 * and h3 response paths, which bypass the h1.1 header parser). If it
 * advertises h3, the next same-origin https fetch upgrades to HTTP/3. */
void http_altsvc_note(const char *host, const char *altsvc_value);

/* Deterministic self-test for the Alt-Svc (RFC 7838) parser + cache
 * used to auto-upgrade a same-origin fetch to HTTP/3. Prints
 * "[altsvc] ..." and returns the PASS count (5 = all pass). */
int http_altsvc_selftest(void);

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
