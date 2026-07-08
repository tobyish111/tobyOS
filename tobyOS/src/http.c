/* http.c -- small synchronous HTTP client (Milestone 24D + browser depth).
 *
 * Architecture (one synchronous orchestrator on top of dns/tcp/tls):
 *
 *   http_get_opt(url, max, timeout, flags, &out)
 *     ├── http_parse_url()          -- split into host/port/path
 *     ├── parse_dotted_quad() OR
 *     │   dns_resolve()             -- get destination IP
 *     ├── keep_take() OR            -- reuse a parked keep-alive conn
 *     │   tcp_connect()/tls_connect() -- fresh handshake
 *     ├── build_request() + send    -- ASCII GET request (1.0 or 1.1)
 *     ├── recv_header_block()       -- pump recv until "\r\n\r\n",
 *     │                                skipping 1xx interim blocks
 *     ├── parse_status_line()       -- HTTP/1.x SSS reason
 *     ├── parse_headers()           -- Content-Length, Content-Type,
 *     │                                Transfer-Encoding, Connection...
 *     ├── body collection           -- Content-Length-bounded, chunked
 *     │                                (dechunk_feed state machine), or
 *     │                                read-until-FIN
 *     └── keep_park() OR            -- park a still-good 1.1 connection
 *         transport_close()         -- graceful FIN exchange
 *
 * A stale parked connection (server closed it while idle) is detected
 * by the first request on it dying before any response byte; the
 * orchestrator then retries ONCE on a fresh connection.
 *
 * Buffers are kmalloc'd and explicitly grown (no krealloc in our
 * heap). The header buffer is hard-capped at HTTP_MAX_HEADER_BYTES;
 * the body buffer is capped at the caller's max_body_bytes (read cap
 * raised for gzip, which inflates back down into the caller's cap).
 */

#include <tobyos/http.h>
#include <tobyos/dns.h>
#include <tobyos/tcp.h>
#include <tobyos/tls.h>
#include <tobyos/net.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/puff.h>
#include <tobyos/brotli.h>
#include <tobyos/pit.h>

/* Transport abstraction: either raw TCP or TLS-wrapped TCP. */
struct http_transport {
    struct tcp_conn *tcp;      /* non-NULL for plain HTTP */
    struct tls_conn *tls;      /* non-NULL for HTTPS */
    uint32_t timeout_ms;
};

static long transport_send(struct http_transport *t, const void *buf, size_t len) {
    if (t->tls) return tls_send(t->tls, buf, len);
    return tcp_send(t->tcp, buf, len);
}

static long transport_recv(struct http_transport *t, void *buf, size_t cap) {
    if (t->tls) {
        long r = tls_recv(t->tls, buf, cap, t->timeout_ms);
        if (r == 0 || r == TLS_ERR_CLOSED) return -1;  /* map to EOF */
        if (r == TLS_ERR_RECV) return 0;                /* map to timeout */
        if (r < 0) return -2;                           /* map to reset */
        return r;
    }
    return tcp_recv(t->tcp, buf, cap, t->timeout_ms);
}

static void transport_close(struct http_transport *t) {
    if (t->tls) { tls_close(t->tls); t->tls = NULL; }
    else if (t->tcp) { tcp_close(t->tcp); t->tcp = NULL; }
}

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

    const char *p;
    if (ascii_starts_with_ci(url, "https://")) {
        out->tls = 1;
        p = url + 8;
    } else if (ascii_starts_with_ci(url, "http://")) {
        out->tls = 0;
        p = url + 7;
    } else {
        return HTTP_ERR_URL;
    }

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
    out->port = out->tls ? 443 : 80;
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

/* ---- cookie jar (RFC 6265-lite, in-memory session cookies) ---------- */

#define COOKIE_MAX      48
#define COOKIE_HOST_MAX 128
#define COOKIE_NAME_MAX 64
#define COOKIE_VAL_MAX  320

struct cookie {
    char host[COOKIE_HOST_MAX];
    char name[COOKIE_NAME_MAX];
    char value[COOKIE_VAL_MAX];
    bool used;
};
static struct cookie g_cookies[COOKIE_MAX];

void cookie_jar_clear(void) {
    for (int i = 0; i < COOKIE_MAX; i++) g_cookies[i].used = false;
}
int cookie_jar_count(void) {
    int n = 0;
    for (int i = 0; i < COOKIE_MAX; i++) if (g_cookies[i].used) n++;
    return n;
}

/* Does request host `h` match a cookie stored for `ch`? Exact, or a
 * domain-suffix match (".example.com" style) -- lenient for v1. */
static bool cookie_host_match(const char *h, const char *ch) {
    size_t hl = strlen(h), cl = strlen(ch);
    if (hl == cl) return ascii_strncasecmp(h, ch, hl) == 0;
    if (cl < hl) {                     /* ch is a suffix of h (subdomain) */
        return ascii_strncasecmp(h + (hl - cl), ch, cl) == 0 &&
               h[hl - cl - 1] == '.';
    }
    return false;
}

/* Store/replace a cookie from a Set-Cookie value ("name=value; attrs").
 * Attributes (Path/Domain/Expires/Secure/HttpOnly) are ignored for v1. */
static void cookie_store(const char *host, const char *setval) {
    /* split name=value at the first '=', up to ';' or end */
    const char *eq = NULL, *p = setval;
    while (*p && *p != ';' && *p != '=') p++;
    if (*p != '=') return;             /* attribute-only / malformed */
    eq = p;
    char name[COOKIE_NAME_MAX];
    size_t nl = (size_t)(eq - setval);
    while (nl > 0 && (setval[nl-1] == ' ')) nl--;   /* rtrim name */
    if (nl == 0 || nl >= sizeof(name)) return;
    memcpy(name, setval, nl); name[nl] = 0;

    const char *vs = eq + 1;
    while (*vs == ' ') vs++;
    const char *ve = vs;
    while (*ve && *ve != ';') ve++;
    size_t vl = (size_t)(ve - vs);
    while (vl > 0 && vs[vl-1] == ' ') vl--;
    if (vl >= COOKIE_VAL_MAX) vl = COOKIE_VAL_MAX - 1;

    /* find existing (host+name) or a free slot */
    int slot = -1, freeslot = -1;
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (!g_cookies[i].used) { if (freeslot < 0) freeslot = i; continue; }
        if (ascii_strncasecmp(g_cookies[i].host, host, COOKIE_HOST_MAX) == 0 &&
            strcmp(g_cookies[i].name, name) == 0) { slot = i; break; }
    }
    if (slot < 0) slot = freeslot;
    if (slot < 0) slot = 0;            /* jar full: clobber slot 0 */
    struct cookie *c = &g_cookies[slot];
    size_t hl2 = strlen(host);
    if (hl2 >= sizeof(c->host)) hl2 = sizeof(c->host) - 1;
    memcpy(c->host, host, hl2); c->host[hl2] = 0;
    memcpy(c->name, name, nl + 1);
    memcpy(c->value, vs, vl); c->value[vl] = 0;
    c->used = true;
}

/* Build the "n1=v1; n2=v2" Cookie header value for `host` into `out`.
 * Returns the length (0 if no cookies apply). */
static size_t cookie_header(const char *host, char *out, size_t cap) {
    size_t pos = 0;
    for (int i = 0; i < COOKIE_MAX; i++) {
        struct cookie *c = &g_cookies[i];
        if (!c->used || !cookie_host_match(host, c->host)) continue;
        size_t need = strlen(c->name) + strlen(c->value) + 3;
        if (pos + need >= cap) break;
        if (pos) { out[pos++] = ';'; out[pos++] = ' '; }
        size_t l = strlen(c->name); memcpy(out + pos, c->name, l); pos += l;
        out[pos++] = '=';
        l = strlen(c->value); memcpy(out + pos, c->value, l); pos += l;
    }
    out[pos] = 0;
    return pos;
}

/* ---- request emission ----------------------------------------------- */

/* Construct the request into `buf`. Returns the number of bytes
 * written (excluding NUL), or -1 if it didn't fit. */
static long build_request(const struct http_url *u, char *buf, size_t cap,
                          unsigned flags, const char *cookie_hdr) {
    /* "GET <path> HTTP/1.0\r\nHost: <host>:<port>\r\n..." */
    static const char ua[] =
        "Mozilla/5.0 (compatible; tobyOS 1.0; x86_64) tobyOS-Browser/3.0";
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
    if (flags & HTTP_F_KEEPALIVE) APPEND_LIT(" HTTP/1.1\r\nHost: ");
    else                          APPEND_LIT(" HTTP/1.0\r\nHost: ");
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
    /* Browser-shaped UA: several endpoints (DuckDuckGo's html frontend
     * among them) answer obvious robot UAs with 202 bot-challenge pages
     * instead of content. */
    APPEND_LIT("\r\nUser-Agent: ");
    APPEND_LIT("Mozilla/5.0 (compatible; tobyOS 1.0; x86_64) tobyOS-Browser/3.0");
    APPEND_LIT("\r\nAccept: */*");
    if (flags & HTTP_F_GZIP)
        APPEND_LIT("\r\nAccept-Encoding: gzip, br");
    if (cookie_hdr && cookie_hdr[0]) {
        APPEND_LIT("\r\nCookie: ");
        APPEND_STR(cookie_hdr);
    }
    if (flags & HTTP_F_KEEPALIVE) APPEND_LIT("\r\nConnection: keep-alive\r\n\r\n");
    else                          APPEND_LIT("\r\nConnection: close\r\n\r\n");

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
 * and out->reason; *out_http11 reports version >= 1.1 (keep-alive is
 * only ever attempted on 1.1 responses). Returns 0 on success,
 * HTTP_ERR_PROTOCOL on parse failure. */
static int parse_status_line(const char *line, size_t len,
                             struct http_response *out, bool *out_http11) {
    /* Need at least "HTTP/1.x SSS" = 12 chars. */
    if (len < 12) return HTTP_ERR_PROTOCOL;
    if (line[0] != 'H' || line[1] != 'T' || line[2] != 'T' || line[3] != 'P' ||
        line[4] != '/') return HTTP_ERR_PROTOCOL;
    if (out_http11)
        *out_http11 = (line[5] == '1' && line[6] == '.' && line[7] >= '1');
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
 *   *out_conn_close  -- true iff Connection lists "close"
 *
 * out->content_type is populated if a Content-Type header is seen.
 *
 * Returns 0 on success, HTTP_ERR_PROTOCOL if a line lacks ':'. */
static int parse_headers(const char *base, size_t len,
                         long *out_content_len, bool *out_chunked,
                         bool *out_conn_close,
                         struct http_response *out, const char *host) {
    *out_content_len = -1;
    *out_chunked     = false;
    *out_conn_close  = false;
    out->content_type[0] = 0;
    out->location[0] = 0;
    out->encoding = HTTP_ENC_IDENTITY;

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
        } else if (colon == 8 && ascii_strncasecmp(base, "Location", 8) == 0) {
            size_t cl = v_len;
            if (cl >= sizeof(out->location)) cl = sizeof(out->location) - 1;
            memcpy(out->location, base + v_off, cl);
            out->location[cl] = 0;
        } else if (colon == 16 && ascii_strncasecmp(base, "Content-Encoding", 16) == 0) {
            /* One transfer content-encoding (values are from a tiny closed
             * set -- gzip/deflate/br/identity -- so a loose substring scan
             * never false-matches). gzip checked first. */
            for (size_t i = 0; i < v_len; i++) {
                if (i + 4 <= v_len &&
                    ascii_strncasecmp(base + v_off + i, "gzip", 4) == 0) {
                    out->encoding = HTTP_ENC_GZIP; break;
                }
                if (i + 2 <= v_len &&
                    ascii_strncasecmp(base + v_off + i, "br", 2) == 0) {
                    out->encoding = HTTP_ENC_BR; break;
                }
            }
        } else if (colon == 10 && ascii_strncasecmp(base, "Connection", 10) == 0) {
            for (size_t i = 0; i + 4 < v_len; i++)
                if (ascii_strncasecmp(base + v_off + i, "close", 5) == 0) {
                    *out_conn_close = true; break;
                }
        } else if (colon == 10 && ascii_strncasecmp(base, "Set-Cookie", 10) == 0 &&
                   host) {
            char sv[COOKIE_VAL_MAX + COOKIE_NAME_MAX + 8];
            size_t cl = v_len;
            if (cl >= sizeof(sv)) cl = sizeof(sv) - 1;
            memcpy(sv, base + v_off, cl); sv[cl] = 0;
            cookie_store(host, sv);
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

/* ---- chunked transfer decoding (RFC 9112 §7.1) ----------------------- *
 *
 * Incremental state machine: wire bytes go in as they arrive from the
 * transport, decoded payload bytes come out appended to a growing body
 * buffer. Chunk extensions and trailer fields are parsed and discarded.
 * Feeding may stop mid-stream when the output cap is reached (the
 * TRUNCATE path); the connection is then mid-frame and NOT reusable. */

enum {
    DC_SIZE = 0,     /* accumulating hex chunk-size digits            */
    DC_EXT,          /* inside ";ext" -- skip to CR                   */
    DC_SIZE_LF,      /* saw CR after size line, expect LF             */
    DC_DATA,         /* consuming `remaining` payload bytes           */
    DC_DATA_CR,      /* payload done, expect CR                       */
    DC_DATA_LF,      /* ... expect LF, then next chunk size           */
    DC_TRL_START,    /* start of a trailer line (after the 0 chunk)   */
    DC_TRL_SKIP,     /* inside a trailer field -- skip to CR          */
    DC_TRL_LF,       /* saw CR inside trailers, expect LF             */
    DC_END_LF,       /* saw CR on the empty trailer line, expect LF   */
};

struct dechunk {
    int           st;
    unsigned long size;          /* chunk size being accumulated */
    unsigned long remaining;     /* payload bytes left in this chunk */
    int           ndigits;
    bool          done;          /* final chunk + trailers consumed */
};

static void dechunk_init(struct dechunk *dc) {
    memset(dc, 0, sizeof(*dc));
    dc->st = DC_SIZE;
}

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Feed `n` wire bytes. Decoded payload is appended to *pbody (grown
 * geometrically, never beyond max_out). Returns 0 on progress (check
 * dc->done and *hit_cap), HTTP_ERR_PROTOCOL on malformed framing,
 * HTTP_ERR_NOMEM if a growth allocation failed. *hit_cap is set when
 * payload had to be dropped because the output reached max_out --
 * the caller must stop reading and poison the connection. *consumed
 * reports how many input bytes were used -- input left over after
 * dc->done means garbage past the terminal chunk (also poisons). */
static int dechunk_feed(struct dechunk *dc, const uint8_t *in, size_t n,
                        uint8_t **pbody, size_t *plen, size_t *pcap,
                        size_t max_out, bool *hit_cap, size_t *consumed)
{
    size_t i = 0;
    while (i < n && !dc->done) {
        uint8_t c = in[i];
        switch (dc->st) {
        case DC_SIZE: {
            int hv = hexval(c);
            if (hv >= 0) {
                dc->size = dc->size * 16 + (unsigned long)hv;
                if (++dc->ndigits > 8 || dc->size > (64ul << 20))
                    return HTTP_ERR_PROTOCOL;
                i++; break;
            }
            if (dc->ndigits == 0) return HTTP_ERR_PROTOCOL;
            if (c == ';')       { dc->st = DC_EXT;     i++; break; }
            if (c == '\r')      { dc->st = DC_SIZE_LF; i++; break; }
            return HTTP_ERR_PROTOCOL;
        }
        case DC_EXT:
            if (c == '\r') dc->st = DC_SIZE_LF;
            i++;
            break;
        case DC_SIZE_LF:
            if (c != '\n') return HTTP_ERR_PROTOCOL;
            i++;
            if (dc->size == 0) dc->st = DC_TRL_START;
            else { dc->remaining = dc->size; dc->st = DC_DATA; }
            break;
        case DC_DATA: {
            size_t take = n - i;
            if (take > dc->remaining) take = (size_t)dc->remaining;
            size_t room = max_out - *plen;
            size_t keep = take > room ? room : take;
            if (keep > 0) {
                if (*plen + keep > *pcap) {
                    size_t nc = *pcap ? *pcap * 2 : 4096;
                    while (nc < *plen + keep) nc *= 2;
                    if (nc > max_out) nc = max_out;
                    uint8_t *nb = grow_buf(*pbody, *plen, nc);
                    if (!nb) return HTTP_ERR_NOMEM;
                    *pbody = nb; *pcap = nc;
                }
                memcpy(*pbody + *plen, in + i, keep);
                *plen += keep;
            }
            if (keep < take) { *hit_cap = true; *consumed = i; return 0; }
            dc->remaining -= take;
            i += take;
            if (dc->remaining == 0) dc->st = DC_DATA_CR;
            break;
        }
        case DC_DATA_CR:
            if (c != '\r') return HTTP_ERR_PROTOCOL;
            dc->st = DC_DATA_LF; i++;
            break;
        case DC_DATA_LF:
            if (c != '\n') return HTTP_ERR_PROTOCOL;
            dc->st = DC_SIZE; dc->size = 0; dc->ndigits = 0; i++;
            break;
        case DC_TRL_START:
            if (c == '\r') { dc->st = DC_END_LF; i++; break; }
            dc->st = DC_TRL_SKIP; i++;
            break;
        case DC_TRL_SKIP:
            if (c == '\r') dc->st = DC_TRL_LF;
            i++;
            break;
        case DC_TRL_LF:
            if (c != '\n') return HTTP_ERR_PROTOCOL;
            dc->st = DC_TRL_START; i++;
            break;
        case DC_END_LF:
            if (c != '\n') return HTTP_ERR_PROTOCOL;
            dc->done = true; i++;
            break;
        default:
            return HTTP_ERR_PROTOCOL;
        }
    }
    *consumed = i;
    return 0;
}

/* ---- keep-alive connection cache ------------------------------------- *
 *
 * A handful of parked HTTP/1.1 connections keyed by (host, port,
 * scheme). One consumer at a time: keep_take() removes the entry, the
 * orchestrator either parks it back after a cleanly-framed response or
 * closes it. Serialized by the same big-lock discipline as the cookie
 * jar (http_get is synchronous; the browser is the only KEEPALIVE
 * caller). Idle entries are swept on every lookup: servers drop idle
 * connections after a few seconds, so anything older than KEEP_IDLE_MS
 * is closed here rather than discovered dead mid-request. */

/* 4 parked conns covers a page + its asset hosts (e.g. en.wikipedia.org
 * + upload.wikimedia.org) while leaving TCP_MAX_CONNS slots free for the
 * active fetch and TIME_WAIT remnants. */
#define KEEP_MAX      4
#define KEEP_IDLE_MS  8000

struct keep_conn {
    char             host[HTTP_MAX_HOST_LEN];
    uint16_t         port;
    uint8_t          tls;
    bool             used;
    struct tcp_conn *tcp;        /* plain-HTTP connection  */
    struct tls_conn *tlsc;       /* HTTPS connection       */
    uint64_t         parked_at;  /* pit_ticks() ms at park */
};
static struct keep_conn g_keep[KEEP_MAX];
static unsigned g_ka_reused, g_ka_handshakes;

static void keep_entry_close(struct keep_conn *k) {
    if (k->tlsc)     tls_close(k->tlsc);
    else if (k->tcp) tcp_close(k->tcp);
    k->tlsc = NULL; k->tcp = NULL; k->used = false;
}

void http_keepalive_flush(void) {
    for (int i = 0; i < KEEP_MAX; i++)
        if (g_keep[i].used) keep_entry_close(&g_keep[i]);
}

void http_keepalive_stats(unsigned *out_reused, unsigned *out_handshakes) {
    if (out_reused)     *out_reused = g_ka_reused;
    if (out_handshakes) *out_handshakes = g_ka_handshakes;
}

/* Take a live parked connection for (host, port, tls) out of the cache
 * into `tr`. Sweeps idle-expired and locally-dead entries as it walks.
 * Returns true on a hit. */
static bool keep_take(const struct http_url *u, struct http_transport *tr) {
    uint64_t now = pit_ticks();          /* 1000 Hz => 1 ms per tick */
    bool hit = false;
    for (int i = 0; i < KEEP_MAX; i++) {
        struct keep_conn *k = &g_keep[i];
        if (!k->used) continue;
        if (now - k->parked_at > KEEP_IDLE_MS) { keep_entry_close(k); continue; }
        /* A plain-TCP conn that already saw FIN/RST while parked is
         * dead; TLS conns can't be probed (opaque) -- the stale-retry
         * in the orchestrator covers those. */
        if (k->tcp && (tcp_poll_flags(k->tcp) & (TCP_RDY_HUP | TCP_RDY_ERR))) {
            keep_entry_close(k); continue;
        }
        if (hit) continue;
        if (k->tls != u->tls || k->port != u->port) continue;
        if (ascii_strncasecmp(k->host, u->host, sizeof(k->host)) != 0) continue;
        tr->tcp = k->tcp; tr->tls = k->tlsc;
        k->tcp = NULL; k->tlsc = NULL; k->used = false;
        hit = true;
    }
    return hit;
}

/* Park a still-established connection for later reuse, evicting the
 * least-recently-parked entry if the cache is full. Ownership of the
 * transport moves into the cache (tr is cleared). */
static void keep_park(const struct http_url *u, struct http_transport *tr) {
    int slot = -1;
    uint64_t oldest = ~0ull;
    for (int i = 0; i < KEEP_MAX; i++) {
        if (!g_keep[i].used) { slot = i; break; }
        if (g_keep[i].parked_at < oldest) { oldest = g_keep[i].parked_at; slot = i; }
    }
    struct keep_conn *k = &g_keep[slot];
    if (k->used) keep_entry_close(k);
    size_t hl = strlen(u->host);
    if (hl >= sizeof(k->host)) hl = sizeof(k->host) - 1;
    memcpy(k->host, u->host, hl); k->host[hl] = 0;
    k->port = u->port;
    k->tls  = u->tls;
    k->tcp  = tr->tcp;
    k->tlsc = tr->tls;
    k->parked_at = pit_ticks();
    k->used = true;
    tr->tcp = NULL; tr->tls = NULL;
}

/* ---- header-block receive -------------------------------------------- */

/* Pump transport_recv into *pbuf (growing up to HTTP_MAX_HEADER_BYTES)
 * until a complete header block ("\r\n\r\n") is present among the
 * first *pused bytes. Returns 0 with *pend set to one past the
 * terminator, or an HTTP_ERR_* (buffer ownership stays with the
 * caller; *pused reports how much arrived before the failure). */
static int recv_header_block(struct http_transport *tr, uint8_t **pbuf,
                             size_t *pcap, size_t *pused, size_t *pend,
                             bool *out_fin)
{
    for (;;) {
        *pend = find_header_end(*pbuf, *pused);
        if (*pend != 0) return 0;
        if (*pused == *pcap) {
            size_t new_cap = *pcap * 2;
            if (new_cap > HTTP_MAX_HEADER_BYTES) new_cap = HTTP_MAX_HEADER_BYTES;
            if (new_cap == *pcap) return HTTP_ERR_PROTOCOL;
            uint8_t *nb = grow_buf(*pbuf, *pused, new_cap);
            if (!nb) return HTTP_ERR_NOMEM;
            *pbuf = nb; *pcap = new_cap;
        }
        long n = transport_recv(tr, *pbuf + *pused, *pcap - *pused);
        if (n > 0) { *pused += (size_t)n; continue; }
        if (n == -1) { *out_fin = true; return HTTP_ERR_PROTOCOL; }
        if (n == -2) return HTTP_ERR_RESET;
        if (n == 0)  return HTTP_ERR_TIMEOUT;
        return HTTP_ERR_PROTOCOL;
    }
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
    r->location[0] = 0;
}

int http_get(const char *url,
             size_t      max_body_bytes,
             uint32_t    timeout_ms,
             struct http_response *out)
{
    return http_get_opt(url, max_body_bytes, timeout_ms, 0, out);
}

int http_get_opt(const char *url,
                 size_t      max_body_bytes,
                 uint32_t    timeout_ms,
                 unsigned    flags,
                 struct http_response *out)
{
    if (!url || !out) return HTTP_ERR_URL;
    memset(out, 0, sizeof(*out));
    out->content_len = -1;

    if (timeout_ms == 0)    timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;
    if (max_body_bytes == 0) max_body_bytes = 1u << 20; /* 1 MiB default */

    /* When gzip is allowed the body is read COMPRESSED, so the wire
     * read cap must exceed the caller's (decompressed) output cap -- a
     * page that renders to 512 KiB may compress from many times that.
     * Read up to HTTP_GZIP_READ_MAX compressed, then inflate into
     * max_body_bytes. */
    size_t read_cap = max_body_bytes;
    if (flags & HTTP_F_GZIP) {
        read_cap = HTTP_GZIP_READ_MAX;
        if (read_cap < max_body_bytes) read_cap = max_body_bytes;
    }

    /* 1. URL parse. */
    struct http_url u;
    int prc = http_parse_url(url, &u);
    if (prc != 0) {
        kprintf("[http] bad URL: %s\n", url);
        return prc;
    }

    bool want_ka = (flags & HTTP_F_KEEPALIVE) != 0;

    /* 2. Resolve lazily: a keep-alive hit skips DNS entirely. */
    uint32_t ip_be = 0;
    bool have_ip = parse_dotted_quad(u.host, &ip_be);

    /* 3.-5. Connect (cached or fresh), send the request, receive the
     * header block. When a REUSED connection dies before yielding a
     * single response byte, the server closed it while it was parked:
     * close it and retry ONCE on a fresh connection (GET is
     * idempotent). */
    struct http_transport tr = { .tcp = NULL, .tls = NULL, .timeout_ms = timeout_ms };

    char cookiebuf[1024];
    cookie_header(u.host, cookiebuf, sizeof(cookiebuf));

    size_t   hdr_cap = 2048;
    uint8_t *buf = (uint8_t *)kmalloc(hdr_cap);
    if (!buf) return HTTP_ERR_NOMEM;
    size_t buf_used = 0, header_end = 0;
    bool   peer_fin = false;

    for (int attempt = 0; ; attempt++) {
        buf_used = 0; header_end = 0; peer_fin = false;
        bool reused = want_ka && attempt == 0 && keep_take(&u, &tr);

        if (reused) {
            g_ka_reused++;
            kprintf("[http] keep-alive: reuse %s:%u (reused=%u handshakes=%u)\n",
                    u.host, u.port, g_ka_reused, g_ka_handshakes);
        } else {
            if (!have_ip) {
                struct dns_result dr;
                if (!dns_resolve(u.host, 1500, &dr)) {
                    kprintf("[http] DNS lookup failed for '%s'\n", u.host);
                    kfree(buf);
                    return HTTP_ERR_DNS;
                }
                ip_be = dr.ip_be;
                have_ip = true;
            }
            {
                uint8_t *ip = (uint8_t *)&ip_be;
                kprintf("[http] %s -> %u.%u.%u.%u:%u%s\n",
                        u.host, ip[0], ip[1], ip[2], ip[3], u.port, u.path);
            }
            if (u.tls) {
                int tls_err;
                tr.tls = tls_connect(ip_be, htons(u.port), u.host,
                                     timeout_ms, &tls_err);
                if (!tr.tls) {
                    kprintf("[http] TLS connect to %s:%u failed: %s\n",
                            u.host, u.port, tls_strerror(tls_err));
                    kfree(buf);
                    return HTTP_ERR_CONNECT;
                }
            } else {
                tr.tcp = tcp_connect(ip_be, htons(u.port), 3000);
                if (!tr.tcp) {
                    kprintf("[http] tcp_connect to %s:%u failed\n", u.host, u.port);
                    kfree(buf);
                    return HTTP_ERR_CONNECT;
                }
            }
            g_ka_handshakes++;
        }

        char reqbuf[2048];
        long reqlen = build_request(&u, reqbuf, sizeof(reqbuf), flags, cookiebuf);
        if (reqlen <= 0) {
            transport_close(&tr);
            kfree(buf);
            return HTTP_ERR_URL;
        }
        long sent = transport_send(&tr, reqbuf, (size_t)reqlen);
        if (sent != reqlen) {
            transport_close(&tr);
            if (reused) {
                kprintf("[http] stale keep-alive (send failed) -- reconnecting\n");
                continue;
            }
            kprintf("[http] send returned %ld (wanted %ld)\n", sent, reqlen);
            kfree(buf);
            return HTTP_ERR_RESET;
        }

        int hrc = recv_header_block(&tr, &buf, &hdr_cap, &buf_used,
                                    &header_end, &peer_fin);
        if (hrc == 0) break;
        transport_close(&tr);
        if (reused && buf_used == 0) {
            kprintf("[http] stale keep-alive (no response) -- reconnecting\n");
            continue;
        }
        kfree(buf);
        return hrc;
    }

    /* 6. Parse status line + headers, skipping 1xx interim responses
     * (e.g. "103 Early Hints" -- Cloudflare sends these on 1.1). */
    long content_len = -1;
    bool chunked = false, http11 = false, conn_close = false;
    for (int interim = 0; ; interim++) {
        size_t status_line_len = 0;
        const char *eol = find_line((const char *)buf, header_end, &status_line_len);
        if (!eol) { kfree(buf); transport_close(&tr); return HTTP_ERR_PROTOCOL; }

        int rc = parse_status_line((const char *)buf, status_line_len, out, &http11);
        if (rc != 0) { kfree(buf); transport_close(&tr); return rc; }

        const char *headers_base = (const char *)buf + status_line_len + 2;
        /* `header_end - 2` because the trailing \r\n\r\n: parse_headers
         * walks until it sees a blank line, so we want to leave the final
         * empty line in the slice. */
        size_t      headers_len  = (size_t)(header_end - status_line_len - 2);
        rc = parse_headers(headers_base, headers_len, &content_len, &chunked,
                           &conn_close, out, u.host);
        if (rc != 0) { kfree(buf); transport_close(&tr); return rc; }

        if (out->status < 100 || out->status > 199) break;
        if (interim >= 4) { kfree(buf); transport_close(&tr); return HTTP_ERR_PROTOCOL; }

        kprintf("[http] skipping interim %d response\n", out->status);
        memmove(buf, buf + header_end, buf_used - header_end);
        buf_used  -= header_end;
        header_end = 0;
        int hrc = recv_header_block(&tr, &buf, &hdr_cap, &buf_used,
                                    &header_end, &peer_fin);
        if (hrc != 0) { kfree(buf); transport_close(&tr); return hrc; }
    }

    /* 7. Body collection. We have already-buffered post-header bytes.
     *
     * Strategy by framing:
     *   - 204/304: no body by definition.
     *   - Transfer-Encoding chunked: pump wire bytes through the
     *     dechunk state machine (payload grows geometrically).
     *   - Content-Length: alloc exactly that, copy `already`, then
     *     loop recv to fill the rest.
     *   - Neither: grow geometrically until peer FIN or cap hit.
     *
     * framing_ok records whether the response was consumed EXACTLY to
     * its framing boundary -- only then can the connection be parked
     * for keep-alive reuse. */
    size_t already = buf_used - header_end;
    uint8_t *body = NULL;
    size_t   body_cap = 0;
    size_t   body_len = 0;
    bool     framing_ok = false;

    out->content_len = content_len;

    bool no_body = (out->status == 204 || out->status == 304);

    if (no_body) {
        body = (uint8_t *)kmalloc(1);
        if (!body) { kfree(buf); transport_close(&tr); return HTTP_ERR_NOMEM; }
        framing_ok = (already == 0);
    } else if (chunked) {
        struct dechunk dc;
        dechunk_init(&dc);
        bool   hit_cap  = false;
        size_t consumed = 0;
        int drc = dechunk_feed(&dc, buf + header_end, already,
                               &body, &body_len, &body_cap, read_cap,
                               &hit_cap, &consumed);
        bool leftover = (dc.done && consumed < already);
        while (drc == 0 && !dc.done && !hit_cap && !peer_fin) {
            /* the header buffer doubles as wire scratch from here on */
            long n = transport_recv(&tr, buf, hdr_cap);
            if (n > 0) {
                drc = dechunk_feed(&dc, buf, (size_t)n,
                                   &body, &body_len, &body_cap, read_cap,
                                   &hit_cap, &consumed);
                if (dc.done && consumed < (size_t)n) leftover = true;
                continue;
            }
            if (n == -1) { peer_fin = true; break; }
            if (n == -2 && body_len > 0) {
                kprintf("[http] reset after %lu bytes -- keeping partial body\n",
                        (unsigned long)body_len);
                peer_fin = true; break;
            }
            if (body) kfree(body);
            kfree(buf); transport_close(&tr);
            if (n == -2) return HTTP_ERR_RESET;
            if (n == 0)  return HTTP_ERR_TIMEOUT;
            return HTTP_ERR_PROTOCOL;
        }
        if (drc != 0) {
            kprintf("[http] malformed chunked framing after %lu bytes\n",
                    (unsigned long)body_len);
            if (body) kfree(body);
            kfree(buf); transport_close(&tr);
            return drc;
        }
        if (hit_cap) {
            if (!(flags & HTTP_F_TRUNCATE)) {
                kprintf("[http] chunked body exceeds max %lu bytes\n",
                        (unsigned long)read_cap);
                if (body) kfree(body);
                kfree(buf); transport_close(&tr);
                return HTTP_ERR_TOOBIG;
            }
            kprintf("[http] truncating chunked body at %lu bytes\n",
                    (unsigned long)body_len);
        } else if (!dc.done) {
            kprintf("[http] chunked stream ended early (%lu bytes)\n",
                    (unsigned long)body_len);
        } else {
            kprintf("[http] dechunked %lu bytes\n", (unsigned long)body_len);
        }
        if (!body) {                          /* zero-length chunked body */
            body = (uint8_t *)kmalloc(1);
            if (!body) { kfree(buf); transport_close(&tr); return HTTP_ERR_NOMEM; }
        }
        framing_ok = dc.done && !hit_cap && !leftover;
    } else if (content_len >= 0) {
        body_cap = (size_t)content_len;
        bool truncated = false;
        if ((size_t)content_len > read_cap) {
            if (!(flags & HTTP_F_TRUNCATE)) {
                kprintf("[http] Content-Length %ld > max %lu\n",
                        content_len, (unsigned long)read_cap);
                kfree(buf); transport_close(&tr);
                return HTTP_ERR_TOOBIG;
            }
            /* Truncate mode: read only what the caller can use, then
             * drop the connection -- no point downloading megabytes the
             * renderer will never see. */
            kprintf("[http] truncating body: Content-Length %ld, keeping %lu\n",
                    content_len, (unsigned long)read_cap);
            body_cap = read_cap;
            truncated = true;
        }
        body = (uint8_t *)kmalloc(body_cap > 0 ? body_cap : 1);
        if (!body) { kfree(buf); transport_close(&tr); return HTTP_ERR_NOMEM; }
        bool over_read = (already > body_cap);    /* server sent extra */
        if (already > body_cap) already = body_cap;
        if (already > 0) memcpy(body, buf + header_end, already);
        body_len = already;

        /* If we haven't already received the whole body and the peer
         * hasn't closed, keep pulling until full or error. A reset
         * after data has arrived degrades to a short body rather than
         * discarding everything (some peers RST instead of FIN). */
        while (body_len < body_cap && !peer_fin) {
            long n = transport_recv(&tr, body + body_len, body_cap - body_len);
            if (n > 0) { body_len += (size_t)n; continue; }
            if (n == -1) { peer_fin = true; break; }
            if (n == -2 && body_len > 0) {
                kprintf("[http] reset after %lu bytes -- keeping partial body\n",
                        (unsigned long)body_len);
                peer_fin = true; break;
            }
            if (n == -2) { kfree(body); kfree(buf); transport_close(&tr); return HTTP_ERR_RESET; }
            if (n == 0)  { kfree(body); kfree(buf); transport_close(&tr); return HTTP_ERR_TIMEOUT; }
            kfree(body); kfree(buf); transport_close(&tr); return HTTP_ERR_PROTOCOL;
        }
        if (body_len < body_cap) {
            kprintf("[http] short body: got %lu of %lu (peer FIN early)\n",
                    (unsigned long)body_len, (unsigned long)body_cap);
        }
        framing_ok = !truncated && !over_read &&
                     body_len == (size_t)content_len;
    } else {
        /* No framing: read until close. Connection can never be reused. */
        body_cap = (already > 0 ? already : 1024);
        body = (uint8_t *)kmalloc(body_cap);
        if (!body) { kfree(buf); transport_close(&tr); return HTTP_ERR_NOMEM; }
        if (already > 0) memcpy(body, buf + header_end, already);
        body_len = already;

        while (!peer_fin) {
            if (body_len == body_cap) {
                size_t new_cap = body_cap * 2;
                if (new_cap > read_cap) new_cap = read_cap;
                if (new_cap == body_cap) {
                    if (flags & HTTP_F_TRUNCATE) {
                        kprintf("[http] truncating body at max %lu bytes\n",
                                (unsigned long)read_cap);
                        break;
                    }
                    kprintf("[http] body exceeds max %lu bytes\n",
                            (unsigned long)read_cap);
                    kfree(body); kfree(buf); transport_close(&tr);
                    return HTTP_ERR_TOOBIG;
                }
                uint8_t *nb = grow_buf(body, body_len, new_cap);
                if (!nb) { kfree(body); kfree(buf); transport_close(&tr); return HTTP_ERR_NOMEM; }
                body = nb; body_cap = new_cap;
            }
            long n = transport_recv(&tr, body + body_len, body_cap - body_len);
            if (n > 0) { body_len += (size_t)n; continue; }
            if (n == -1) { peer_fin = true; break; }
            if (n == -2 && body_len > 0) {
                kprintf("[http] reset after %lu bytes -- keeping partial body\n",
                        (unsigned long)body_len);
                peer_fin = true; break;
            }
            if (n == -2) { kfree(body); kfree(buf); transport_close(&tr); return HTTP_ERR_RESET; }
            if (n == 0)  { kfree(body); kfree(buf); transport_close(&tr); return HTTP_ERR_TIMEOUT; }
            kfree(body); kfree(buf); transport_close(&tr); return HTTP_ERR_PROTOCOL;
        }
    }

    /* 8. Park or close. A cleanly-framed HTTP/1.1 response without
     * "Connection: close" leaves the connection good for another
     * request to the same (host, port, scheme). */
    if (want_ka && http11 && !conn_close && framing_ok && !peer_fin &&
        (tr.tcp || tr.tls)) {
        kprintf("[http] keep-alive: parked %s:%u\n", u.host, u.port);
        keep_park(&u, &tr);            /* moves the conn; clears tr */
    }
    kfree(buf);
    transport_close(&tr);              /* no-op when parked */

    /* Decompress a gzip or brotli body kernel-side (transparent to the
     * caller): the compressed body was read up to read_cap; decompress
     * into a buffer capped at max_body_bytes, so the caller still sees at
     * most its requested (decompressed) size. On failure keep the raw
     * body. Brotli (RFC 7932) is now more common than gzip on CDNs. */
    if ((out->encoding == HTTP_ENC_GZIP || out->encoding == HTTP_ENC_BR) &&
        body_len > 0) {
        unsigned long dlen = max_body_bytes;
        uint8_t *dbody = (uint8_t *)kmalloc(dlen > 0 ? dlen : 1);
        if (dbody) {
            int ok, trunc;
            const char *alg;
            if (out->encoding == HTTP_ENC_BR) {
                int pr = brotli_decompress(dbody, &dlen, body, body_len);
                ok = (pr == BROTLI_OK || pr == BROTLI_TRUNC);
                trunc = (pr == BROTLI_TRUNC);
                alg = "unbrotli";
            } else {
                int pr = puff_gzip(dbody, &dlen, body, body_len);
                ok = (pr == PUFF_OK || pr == PUFF_TRUNC);
                trunc = (pr == PUFF_TRUNC);
                alg = "gunzip";
            }
            if (ok) {
                kprintf("[http] %s %lu -> %lu%s\n", alg,
                        (unsigned long)body_len, dlen,
                        trunc ? " (truncated)" : "");
                kfree(body);
                body = dbody; body_len = dlen;
                out->encoding = HTTP_ENC_IDENTITY;
                out->content_len = (long)dlen;
            } else {
                kprintf("[http] %s FAILED -- keeping raw body\n", alg);
                kfree(dbody);
            }
        }
    }

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
