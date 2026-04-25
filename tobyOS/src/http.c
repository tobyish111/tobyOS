/* http.c -- minimal HTTP/1.0 client (Milestone 24D).
 *
 * Architecture (one synchronous orchestrator on top of dns/tcp):
 *
 *   http_get(url, max, timeout, &out)
 *     ├── http_parse_url()          -- split into host/port/path
 *     ├── parse_dotted_quad() OR
 *     │   dns_resolve()             -- get destination IP
 *     ├── tcp_connect()             -- 3-way handshake
 *     ├── build_request() + tcp_send  -- ASCII GET request
 *     ├── recv_headers()            -- pump tcp_recv into a buffer
 *     │                                until we see "\r\n\r\n"
 *     ├── parse_status_line()       -- HTTP/1.x SSS reason
 *     ├── parse_headers()           -- pluck Content-Length,
 *     │                                Content-Type, Transfer-Encoding
 *     ├── recv_body()               -- copy any post-header bytes
 *     │                                already buffered, then keep
 *     │                                pumping tcp_recv until either
 *     │                                Content-Length is reached or
 *     │                                the peer FINs.
 *     └── tcp_close()               -- graceful FIN exchange.
 *
 * Buffers are kmalloc'd and explicitly grown (no krealloc in our
 * heap). The header buffer is hard-capped at HTTP_MAX_HEADER_BYTES;
 * the body buffer is capped at the caller's max_body_bytes.
 */

#include <tobyos/http.h>
#include <tobyos/dns.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- tiny ASCII helpers (kept local; klibc.h is intentionally small) - */

static inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int ascii_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = ascii_lower(a[i]);
        char cb = ascii_lower(b[i]);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

static bool ascii_starts_with_ci(const char *s, const char *prefix) {
    size_t pl = strlen(prefix);
    if (strlen(s) < pl) return false;
    return ascii_strncasecmp(s, prefix, pl) == 0;
}

/* atoi-style: parse a non-negative decimal integer, stopping at the
 * first non-digit. Returns the value, or -1 if no digits were seen. */
static long parse_decimal(const char *s, size_t max_len, size_t *consumed) {
    long v = 0;
    size_t i = 0;
    bool any = false;
    while (i < max_len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 0x7FFFFFFFL) {        /* overflow guard */
            v = 0x7FFFFFFFL;
        }
        i++;
        any = true;
    }
    if (consumed) *consumed = i;
    return any ? v : -1;
}

/* Parse a dotted-decimal IPv4 like "10.0.2.2" into network byte order.
 * Returns true on success. Stops at end-of-string only -- partial
 * parses (e.g. "10.0.2.2:80") MUST be split before calling. */
static bool parse_dotted_quad(const char *s, uint32_t *out_ip_be) {
    uint32_t v[4] = {0};
    int oct = 0;
    const char *p = s;
    while (*p && oct < 4) {
        long d = 0;
        int  digits = 0;
        while (*p >= '0' && *p <= '9') {
            d = d * 10 + (*p - '0');
            digits++;
            if (digits > 3 || d > 255) return false;
            p++;
        }
        if (digits == 0) return false;
        v[oct++] = (uint32_t)d;
        if (*p == '.') {
            p++;
            if (oct == 4) return false;     /* "1.2.3.4." */
            continue;
        }
        if (*p == 0) break;
        return false;                       /* unexpected char */
    }
    if (oct != 4 || *p != 0) return false;
    /* Network byte order = first octet in lowest address. */
    *out_ip_be = (v[0]) | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
    return true;
}

/* ---- URL parsing ---------------------------------------------------- */

int http_parse_url(const char *url, struct http_url *out) {
    if (!url || !out) return HTTP_ERR_URL;
    memset(out, 0, sizeof(*out));

    if (!ascii_starts_with_ci(url, "http://")) return HTTP_ERR_URL;
    const char *p = url + 7;                /* skip scheme */

    /* Reject userinfo (we don't implement it). */
    for (const char *q = p; *q && *q != '/' && *q != '#'; q++) {
        if (*q == '@') return HTTP_ERR_URL;
    }
    /* Reject IPv6 literals. */
    if (*p == '[') return HTTP_ERR_URL;

    /* Host runs to ':' or '/' or end-of-string. */
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '#') host_end++;
    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(out->host)) return HTTP_ERR_URL;
    memcpy(out->host, p, host_len);
    out->host[host_len] = 0;
    p = host_end;

    /* Optional :port */
    out->port = 80;
    if (*p == ':') {
        p++;
        size_t consumed = 0;
        long v = parse_decimal(p, 6, &consumed);
        if (v < 1 || v > 65535) return HTTP_ERR_URL;
        out->port = (uint16_t)v;
        p += consumed;
        if (*p && *p != '/' && *p != '#') return HTTP_ERR_URL;
    }

    /* Optional path. Default "/". Reject fragments. */
    if (*p == 0 || *p == '#') {
        out->path[0] = '/';
        out->path[1] = 0;
        return 0;
    }
    if (*p != '/') return HTTP_ERR_URL;

    const char *path_end = p;
    while (*path_end && *path_end != '#') path_end++;
    size_t path_len = (size_t)(path_end - p);
    if (path_len >= sizeof(out->path)) return HTTP_ERR_URL;
    memcpy(out->path, p, path_len);
    out->path[path_len] = 0;
    return 0;
}

/* ---- request emission ----------------------------------------------- */

/* Construct the request into `buf`. Returns the number of bytes
 * written (excluding NUL), or -1 if it didn't fit. */
static long build_request(const struct http_url *u, char *buf, size_t cap) {
    /* "GET <path> HTTP/1.0\r\nHost: <host>:<port>\r\n..." */
    static const char ua[] = "tobyOS-http/0.1";
    size_t need = 0;

    /* Quick upper bound check. Paths and hosts already capped by
     * http_parse_url(); add fixed overhead. */
    size_t hostlen = strlen(u->host);
    size_t pathlen = strlen(u->path);
    need = pathlen + hostlen + sizeof(ua) + 96;
    if (need > cap) return -1;

    size_t pos = 0;
    #define APPEND_LIT(s) do {                                  \
        size_t l = sizeof(s) - 1;                               \
        if (pos + l > cap) return -1;                           \
        memcpy(buf + pos, (s), l); pos += l;                    \
    } while (0)
    #define APPEND_STR(s) do {                                  \
        size_t l = strlen(s);                                   \
        if (pos + l > cap) return -1;                           \
        memcpy(buf + pos, (s), l); pos += l;                    \
    } while (0)

    APPEND_LIT("GET ");
    APPEND_STR(u->path);
    APPEND_LIT(" HTTP/1.0\r\nHost: ");
    APPEND_STR(u->host);
    if (u->port != 80) {
        /* ":<port>" */
        if (pos + 7 > cap) return -1;
        buf[pos++] = ':';
        char tmp[6];
        int  ti = 0;
        uint16_t v = u->port;
        if (v == 0) { tmp[ti++] = '0'; }
        else {
            char rev[6]; int ri = 0;
            while (v > 0) { rev[ri++] = (char)('0' + (v % 10)); v /= 10; }
            while (ri > 0) tmp[ti++] = rev[--ri];
        }
        memcpy(buf + pos, tmp, (size_t)ti);
        pos += (size_t)ti;
    }
    APPEND_LIT("\r\nUser-Agent: ");
    APPEND_LIT("tobyOS-http/0.1");
    APPEND_LIT("\r\nAccept: */*\r\nConnection: close\r\n\r\n");

    if (pos < cap) buf[pos] = 0;            /* convenience NUL */
    return (long)pos;
    #undef APPEND_LIT
    #undef APPEND_STR
}

/* ---- response parsing ----------------------------------------------- */

/* Locate "\r\n\r\n" in the first `len` bytes of `buf`. Returns the
 * offset of the FIRST byte AFTER the terminator, or 0 if not found. */
static size_t find_header_end(const uint8_t *buf, size_t len) {
    if (len < 4) return 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

/* Walk a single line (terminated by \r\n inside [base, base+len)).
 * Returns pointer to the line-end (the \r), or NULL if no \r\n in
 * range. Sets *out_len to the line length excluding the terminator. */
static const char *find_line(const char *base, size_t len, size_t *out_len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (base[i] == '\r' && base[i+1] == '\n') {
            if (out_len) *out_len = i;
            return base + i;
        }
    }
    return NULL;
}

/* Parse the status line "HTTP/x.y SSS reason\r\n". Fills out->status
 * and out->reason. Returns 0 on success, HTTP_ERR_PROTOCOL on parse
 * failure. */
static int parse_status_line(const char *line, size_t len,
                             struct http_response *out) {
    /* Need at least "HTTP/1.x SSS" = 12 chars. */
    if (len < 12) return HTTP_ERR_PROTOCOL;
    if (line[0] != 'H' || line[1] != 'T' || line[2] != 'T' || line[3] != 'P' ||
        line[4] != '/') return HTTP_ERR_PROTOCOL;
    size_t i = 5;
    while (i < len && line[i] != ' ') i++;
    if (i == len || i + 4 > len) return HTTP_ERR_PROTOCOL;
    i++;                                    /* skip the space */
    size_t consumed = 0;
    long status = parse_decimal(line + i, len - i, &consumed);
    if (status < 100 || status > 599 || consumed != 3) return HTTP_ERR_PROTOCOL;
    out->status = (int)status;
    i += consumed;
    /* optional " reason..." */
    if (i < len && line[i] == ' ') i++;
    size_t rlen = len - i;
    if (rlen >= sizeof(out->reason)) rlen = sizeof(out->reason) - 1;
    memcpy(out->reason, line + i, rlen);
    out->reason[rlen] = 0;
    return 0;
}

/* Parse headers. `base` points just past the status line's \r\n.
 * `len` is the remaining length up to the \r\n\r\n terminator
 * (inclusive of the final empty line's \r\n).
 *
 * Out parameters:
 *   *out_content_len -- value of Content-Length:, or -1 if absent
 *   *out_chunked     -- true iff Transfer-Encoding lists "chunked"
 *
 * out->content_type is populated if a Content-Type header is seen.
 *
 * Returns 0 on success, HTTP_ERR_PROTOCOL if a line lacks ':'. */
static int parse_headers(const char *base, size_t len,
                         long *out_content_len, bool *out_chunked,
                         struct http_response *out) {
    *out_content_len = -1;
    *out_chunked     = false;
    out->content_type[0] = 0;

    while (len > 0) {
        size_t line_len = 0;
        const char *eol = find_line(base, len, &line_len);
        if (!eol) return HTTP_ERR_PROTOCOL;
        if (line_len == 0) {
            return 0;                       /* blank line = end of headers */
        }
        /* Find ':' separating key and value. */
        size_t colon = 0;
        bool   found_colon = false;
        for (size_t i = 0; i < line_len; i++) {
            if (base[i] == ':') { colon = i; found_colon = true; break; }
        }
        if (!found_colon) return HTTP_ERR_PROTOCOL;

        /* Skip OWS after ':'. */
        size_t v_off = colon + 1;
        while (v_off < line_len && (base[v_off] == ' ' || base[v_off] == '\t')) v_off++;
        size_t v_len = line_len - v_off;

        /* Match the headers we care about (case-insensitive on the key). */
        if (colon == 14 && ascii_strncasecmp(base, "Content-Length", 14) == 0) {
            size_t consumed = 0;
            long n = parse_decimal(base + v_off, v_len, &consumed);
            if (n >= 0) *out_content_len = n;
        } else if (colon == 17 && ascii_strncasecmp(base, "Transfer-Encoding", 17) == 0) {
            /* Look for substring "chunked" anywhere in the value. */
            for (size_t i = 0; i + 6 < v_len; i++) {
                if (ascii_strncasecmp(base + v_off + i, "chunked", 7) == 0) {
                    *out_chunked = true;
                    break;
                }
            }
        } else if (colon == 12 && ascii_strncasecmp(base, "Content-Type", 12) == 0) {
            size_t cl = v_len;
            if (cl >= sizeof(out->content_type)) cl = sizeof(out->content_type) - 1;
            memcpy(out->content_type, base + v_off, cl);
            out->content_type[cl] = 0;
        }

        size_t advance = (size_t)((eol + 2) - base);
        if (advance > len) return HTTP_ERR_PROTOCOL;
        base += advance;
        len  -= advance;
    }
    return HTTP_ERR_PROTOCOL;               /* never saw blank line */
}

/* ---- buffer growth -------------------------------------------------- */

/* Grow *buf from current_cap to new_cap, preserving the first
 * `live_bytes`. Returns the new buffer (== *buf on success), or NULL
 * on OOM (caller should keep the old buffer to free). */
static uint8_t *grow_buf(uint8_t *old, size_t live_bytes,
                         size_t new_cap) {
    uint8_t *nb = (uint8_t *)kmalloc(new_cap);
    if (!nb) return NULL;
    if (old && live_bytes) memcpy(nb, old, live_bytes);
    if (old) kfree(old);
    return nb;
}

/* ---- the orchestrator ----------------------------------------------- */

static const char *err_strs[] = {
    [-HTTP_ERR_URL]      = "bad URL",
    [-HTTP_ERR_DNS]      = "DNS lookup failed",
    [-HTTP_ERR_CONNECT]  = "TCP connect failed",
    [-HTTP_ERR_PROTOCOL] = "malformed HTTP response",
    [-HTTP_ERR_CHUNKED]  = "chunked transfer-encoding not supported",
    [-HTTP_ERR_TOOBIG]   = "response exceeds size limit",
    [-HTTP_ERR_TIMEOUT]  = "timed out",
    [-HTTP_ERR_NOMEM]    = "out of memory",
    [-HTTP_ERR_RESET]    = "connection reset by peer",
};

const char *http_strerror(int err) {
    if (err >= 0) return "ok";
    int idx = -err;
    if (idx >= (int)(sizeof(err_strs) / sizeof(err_strs[0]))) return "unknown";
    return err_strs[idx] ? err_strs[idx] : "unknown";
}

void http_free(struct http_response *r) {
    if (!r) return;
    if (r->body) { kfree(r->body); r->body = NULL; }
    r->body_len = 0;
    r->status = 0;
    r->reason[0] = 0;
    r->content_type[0] = 0;
}

int http_get(const char *url,
             size_t      max_body_bytes,
             uint32_t    timeout_ms,
             struct http_response *out)
{
    if (!url || !out) return HTTP_ERR_URL;
    memset(out, 0, sizeof(*out));

    if (timeout_ms == 0)    timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;
    if (max_body_bytes == 0) max_body_bytes = 1u << 20; /* 1 MiB default */

    /* 1. URL parse. */
    struct http_url u;
    int prc = http_parse_url(url, &u);
    if (prc != 0) {
        kprintf("[http] bad URL: %s\n", url);
        return prc;
    }

    /* 2. Resolve host. Accept dotted-quad inline; fall through to DNS. */
    uint32_t ip_be = 0;
    if (!parse_dotted_quad(u.host, &ip_be)) {
        struct dns_result dr;
        if (!dns_resolve(u.host, 1500, &dr)) {
            kprintf("[http] DNS lookup failed for '%s'\n", u.host);
            return HTTP_ERR_DNS;
        }
        ip_be = dr.ip_be;
    }
    {
        uint8_t *ip = (uint8_t *)&ip_be;
        kprintf("[http] %s -> %u.%u.%u.%u:%u%s\n",
                u.host, ip[0], ip[1], ip[2], ip[3], u.port, u.path);
    }

    /* 3. TCP connect. */
    struct tcp_conn *c = tcp_connect(ip_be, htons(u.port), 3000);
    if (!c) {
        kprintf("[http] tcp_connect to %s:%u failed\n", u.host, u.port);
        return HTTP_ERR_CONNECT;
    }

    /* 4. Build + send request. */
    char  reqbuf[768];
    long  reqlen = build_request(&u, reqbuf, sizeof(reqbuf));
    if (reqlen <= 0) {
        tcp_close(c);
        return HTTP_ERR_URL;
    }
    long sent = tcp_send(c, reqbuf, (size_t)reqlen);
    if (sent != reqlen) {
        kprintf("[http] tcp_send returned %ld (wanted %ld)\n", sent, reqlen);
        tcp_close(c);
        return HTTP_ERR_RESET;
    }

    /* 5. Read into a growing buffer until we see \r\n\r\n. */
    size_t   hdr_cap   = 1024;
    uint8_t *buf       = (uint8_t *)kmalloc(hdr_cap);
    if (!buf) { tcp_close(c); return HTTP_ERR_NOMEM; }
    size_t   buf_used  = 0;
    size_t   header_end = 0;
    bool     peer_fin  = false;

    for (;;) {
        if (buf_used == hdr_cap) {
            size_t new_cap = hdr_cap * 2;
            if (new_cap > HTTP_MAX_HEADER_BYTES) new_cap = HTTP_MAX_HEADER_BYTES;
            if (new_cap == hdr_cap) {
                /* Already at the cap and still no \r\n\r\n -- fail. */
                kfree(buf); tcp_close(c);
                return HTTP_ERR_PROTOCOL;
            }
            uint8_t *nb = grow_buf(buf, buf_used, new_cap);
            if (!nb) { kfree(buf); tcp_close(c); return HTTP_ERR_NOMEM; }
            buf = nb; hdr_cap = new_cap;
        }
        long n = tcp_recv(c, buf + buf_used, hdr_cap - buf_used, timeout_ms);
        if (n > 0) {
            buf_used += (size_t)n;
            header_end = find_header_end(buf, buf_used);
            if (header_end != 0) break;
            continue;
        }
        if (n == -1) { peer_fin = true; break; }   /* clean EOF */
        if (n == -2) { kfree(buf); tcp_close(c); return HTTP_ERR_RESET; }
        if (n == 0)  { kfree(buf); tcp_close(c); return HTTP_ERR_TIMEOUT; }
        kfree(buf); tcp_close(c); return HTTP_ERR_PROTOCOL;
    }
    if (header_end == 0) {
        kfree(buf); tcp_close(c);
        return HTTP_ERR_PROTOCOL;             /* peer closed before headers */
    }

    /* 6. Parse status line + headers. */
    size_t status_line_len = 0;
    const char *eol = find_line((const char *)buf, header_end, &status_line_len);
    if (!eol) { kfree(buf); tcp_close(c); return HTTP_ERR_PROTOCOL; }

    int rc = parse_status_line((const char *)buf, status_line_len, out);
    if (rc != 0) { kfree(buf); tcp_close(c); return rc; }

    const char *headers_base = (const char *)buf + status_line_len + 2;
    /* `header_end - 2` because the trailing \r\n\r\n: parse_headers
     * walks until it sees a blank line, so we want to leave the final
     * empty line in the slice. */
    size_t      headers_len  = (size_t)(header_end - status_line_len - 2);
    long content_len = -1;
    bool chunked     = false;
    rc = parse_headers(headers_base, headers_len, &content_len, &chunked, out);
    if (rc != 0) { kfree(buf); tcp_close(c); return rc; }
    if (chunked) {
        kprintf("[http] response uses chunked encoding (unsupported)\n");
        kfree(buf); tcp_close(c);
        return HTTP_ERR_CHUNKED;
    }

    /* 7. Body collection. We have already-buffered post-header bytes. */
    size_t already = buf_used - header_end;

    /* Pick a body buffer strategy:
     *   - With Content-Length: alloc exactly that, copy `already`,
     *     then loop tcp_recv to fill the rest.
     *   - Without Content-Length: grow geometrically until peer FIN
     *     or max_body_bytes hit.
     */
    uint8_t *body = NULL;
    size_t   body_cap = 0;
    size_t   body_len = 0;

    if (content_len >= 0) {
        if ((size_t)content_len > max_body_bytes) {
            kprintf("[http] Content-Length %ld > max %lu\n",
                    content_len, (unsigned long)max_body_bytes);
            kfree(buf); tcp_close(c);
            return HTTP_ERR_TOOBIG;
        }
        body_cap = (size_t)content_len;
        body = (uint8_t *)kmalloc(body_cap > 0 ? body_cap : 1);
        if (!body) { kfree(buf); tcp_close(c); return HTTP_ERR_NOMEM; }
        if (already > body_cap) already = body_cap;       /* guard */
        if (already > 0) memcpy(body, buf + header_end, already);
        body_len = already;

        /* If we haven't already received the whole body and the peer
         * hasn't closed, keep pulling until full or error. */
        while (body_len < body_cap && !peer_fin) {
            long n = tcp_recv(c, body + body_len, body_cap - body_len, timeout_ms);
            if (n > 0) { body_len += (size_t)n; continue; }
            if (n == -1) { peer_fin = true; break; }
            if (n == -2) { kfree(body); kfree(buf); tcp_close(c); return HTTP_ERR_RESET; }
            if (n == 0)  { kfree(body); kfree(buf); tcp_close(c); return HTTP_ERR_TIMEOUT; }
            kfree(body); kfree(buf); tcp_close(c); return HTTP_ERR_PROTOCOL;
        }
        if (body_len < body_cap) {
            kprintf("[http] short body: got %lu of %lu (peer FIN early)\n",
                    (unsigned long)body_len, (unsigned long)body_cap);
        }
    } else {
        body_cap = (already > 0 ? already : 1024);
        body = (uint8_t *)kmalloc(body_cap);
        if (!body) { kfree(buf); tcp_close(c); return HTTP_ERR_NOMEM; }
        if (already > 0) memcpy(body, buf + header_end, already);
        body_len = already;

        while (!peer_fin) {
            if (body_len == body_cap) {
                size_t new_cap = body_cap * 2;
                if (new_cap > max_body_bytes) new_cap = max_body_bytes;
                if (new_cap == body_cap) {
                    kprintf("[http] body exceeds max %lu bytes\n",
                            (unsigned long)max_body_bytes);
                    kfree(body); kfree(buf); tcp_close(c);
                    return HTTP_ERR_TOOBIG;
                }
                uint8_t *nb = grow_buf(body, body_len, new_cap);
                if (!nb) { kfree(body); kfree(buf); tcp_close(c); return HTTP_ERR_NOMEM; }
                body = nb; body_cap = new_cap;
            }
            long n = tcp_recv(c, body + body_len, body_cap - body_len, timeout_ms);
            if (n > 0) { body_len += (size_t)n; continue; }
            if (n == -1) { peer_fin = true; break; }
            if (n == -2) { kfree(body); kfree(buf); tcp_close(c); return HTTP_ERR_RESET; }
            if (n == 0)  { kfree(body); kfree(buf); tcp_close(c); return HTTP_ERR_TIMEOUT; }
            kfree(body); kfree(buf); tcp_close(c); return HTTP_ERR_PROTOCOL;
        }
    }

    /* 8. Wrap up. */
    kfree(buf);
    tcp_close(c);

    out->body     = body;
    out->body_len = body_len;
    kprintf("[http] %d %s; body=%lu bytes; type=\"%s\"\n",
            out->status, out->reason,
            (unsigned long)body_len,
            out->content_type[0] ? out->content_type : "(none)");
    return 0;
}

/* ---- Milestone 24D boot self-test -------------------------------- */

#ifdef HTTP_M24D_SELFTEST

#include <tobyos/pkg.h>

void http_m24d_selftest(void) {
    kprintf("[m24d-selftest] >>> step 1: GET /m24d_smoke.txt\n");
    struct http_response r;
    int rc = http_get("http://10.0.2.2:8000/m24d_smoke.txt",
                      /*max=*/4096, /*timeout_ms=*/3000, &r);
    if (rc != 0) {
        kprintf("[m24d-selftest] FAIL: http_get text returned %d (%s)\n",
                rc, http_strerror(rc));
        return;
    }
    if (r.status != 200) {
        kprintf("[m24d-selftest] FAIL: expected HTTP 200, got %d %s\n",
                r.status, r.reason);
        http_free(&r);
        return;
    }
    /* Expect exactly "tobyOS-m24d-ok\n" (15 bytes). */
    static const char expected[] = "tobyOS-m24d-ok\n";
    const size_t expected_len = sizeof(expected) - 1;
    if (r.body_len != expected_len ||
        memcmp(r.body, expected, expected_len) != 0) {
        kprintf("[m24d-selftest] FAIL: smoke body mismatch (len=%lu)\n",
                (unsigned long)r.body_len);
        http_free(&r);
        return;
    }
    kprintf("[m24d-selftest]     OK: smoke body matches \"%s\" minus newline\n",
            "tobyOS-m24d-ok");
    http_free(&r);

    kprintf("[m24d-selftest] >>> step 2: pkg install http://10.0.2.2:8000/helloapp.tpkg\n");
    int irc = pkg_install_url("http://10.0.2.2:8000/helloapp.tpkg");
    if (irc != 0) {
        kprintf("[m24d-selftest] FAIL: pkg_install_url returned %d\n", irc);
        return;
    }

    kprintf("[m24d-selftest] >>> step 3: verify install record exists\n");
    if (pkg_info("helloapp") != 0) {
        kprintf("[m24d-selftest] FAIL: pkg_info('helloapp') after install\n");
        return;
    }

    kprintf("[m24d-selftest] >>> step 4: pkg remove helloapp (cleanup)\n");
    int rrc = pkg_remove("helloapp");
    if (rrc != 0) {
        kprintf("[m24d-selftest] FAIL: pkg_remove returned %d\n", rrc);
        return;
    }

    kprintf("[m24d-selftest] SUCCESS\n");
}

#else  /* !HTTP_M24D_SELFTEST */

void http_m24d_selftest(void) { /* no-op stub */ }

#endif
