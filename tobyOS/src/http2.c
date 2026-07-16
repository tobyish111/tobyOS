/* http2.c -- minimal HTTP/2 client for tobyOS (stage 13G).
 *
 * A focused *single-request* GET client that runs over an established
 * TLS 1.3 connection whose ALPN selected "h2" (see tls.c / tls_alpn()).
 * http_get_opt() calls http2_fetch() for h2 servers and falls back to
 * HTTP/1.1 on any failure, so this never has to be perfect to be safe.
 *
 * Implements: the connection preface + SETTINGS, one HEADERS request on
 * stream 1, and reassembly of the HEADERS(+CONTINUATION)/DATA response
 * into a struct http_response. HPACK decode is complete: static table,
 * dynamic table (with evictions + size updates), and the RFC 7541
 * Huffman code. Bodies are decompressed here (gzip/brotli) so the result
 * matches the h1 path exactly. Flow control is handled by advertising a
 * large initial window + a big connection WINDOW_UPDATE up front, which
 * covers our 512 KiB read cap without per-frame bookkeeping.
 *
 * v1 limits: one stream per connection (no multiplexing / reuse), no
 * server push (rejected), no cookie-jar integration, no trailers.
 */

#include <tobyos/types.h>
#include <tobyos/net.h>
#include <tobyos/tls.h>
#include <tobyos/http.h>
#include <tobyos/puff.h>
#include <tobyos/brotli.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* Frame types */
#define H2_DATA          0x0
#define H2_HEADERS       0x1
#define H2_RST_STREAM    0x3
#define H2_SETTINGS      0x4
#define H2_PUSH_PROMISE  0x5
#define H2_PING          0x6
#define H2_GOAWAY        0x7
#define H2_WINDOW_UPDATE 0x8
#define H2_CONTINUATION  0x9

/* Frame flags */
#define H2_FLAG_END_STREAM  0x01
#define H2_FLAG_END_HEADERS 0x04
#define H2_FLAG_PADDED      0x08
#define H2_FLAG_PRIORITY    0x20
#define H2_FLAG_ACK         0x01

/* Settings ids */
#define H2_SETTINGS_HEADER_TABLE_SIZE   0x1
#define H2_SETTINGS_ENABLE_PUSH         0x2
#define H2_SETTINGS_INITIAL_WINDOW_SIZE 0x4
#define H2_SETTINGS_MAX_FRAME_SIZE      0x5

static const char H2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
#define H2_PREFACE_LEN 24

/* We advertise a big window so the whole (<= read cap) body streams
 * without us having to top up flow control mid-transfer. */
#define H2_MY_WINDOW   (8u * 1024 * 1024)
#define H2_MAX_FRAME   16384
#define HTTP2_HEADER_CAP 65536         /* max decoded header block */

/* ---- HPACK static table (RFC 7541 Appendix A) ---------------------- */
static const struct { const char *name; const char *value; } hpack_static[] = {
    { NULL, NULL },
    { ":authority", "" }, { ":method", "GET" }, { ":method", "POST" },
    { ":path", "/" }, { ":path", "/index.html" }, { ":scheme", "http" },
    { ":scheme", "https" }, { ":status", "200" }, { ":status", "204" },
    { ":status", "206" }, { ":status", "304" }, { ":status", "400" },
    { ":status", "404" }, { ":status", "500" }, { "accept-charset", "" },
    { "accept-encoding", "gzip, deflate" }, { "accept-language", "" },
    { "accept-ranges", "" }, { "accept", "" },
    { "access-control-allow-origin", "" }, { "age", "" }, { "allow", "" },
    { "authorization", "" }, { "cache-control", "" },
    { "content-disposition", "" }, { "content-encoding", "" },
    { "content-language", "" }, { "content-length", "" },
    { "content-location", "" }, { "content-range", "" },
    { "content-type", "" }, { "cookie", "" }, { "date", "" }, { "etag", "" },
    { "expect", "" }, { "expires", "" }, { "from", "" }, { "host", "" },
    { "if-match", "" }, { "if-modified-since", "" }, { "if-none-match", "" },
    { "if-range", "" }, { "if-unmodified-since", "" }, { "last-modified", "" },
    { "link", "" }, { "location", "" }, { "max-forwards", "" },
    { "proxy-authenticate", "" }, { "proxy-authorization", "" },
    { "range", "" }, { "referer", "" }, { "refresh", "" },
    { "retry-after", "" }, { "server", "" }, { "set-cookie", "" },
    { "strict-transport-security", "" }, { "transfer-encoding", "" },
    { "user-agent", "" }, { "vary", "" }, { "via", "" },
    { "www-authenticate", "" },
};
#define HPACK_STATIC_N 61

#include "hpack_huff_table.h"   /* hpack_huff[257] = {code,nbits} */

/* ---- HPACK integer coding (RFC 7541 5.1) --------------------------- */
static size_t hpack_enc_int(uint8_t *buf, uint32_t value,
                            uint8_t prefix_bits, uint8_t pattern) {
    uint8_t maxp = (uint8_t)((1u << prefix_bits) - 1);
    if (value < maxp) { buf[0] = pattern | (uint8_t)value; return 1; }
    buf[0] = pattern | maxp;
    value -= maxp;
    size_t i = 1;
    while (value >= 128) { buf[i++] = (uint8_t)(value & 0x7F) | 0x80; value >>= 7; }
    buf[i++] = (uint8_t)value;
    return i;
}

static size_t hpack_dec_int(const uint8_t *buf, size_t len,
                            uint8_t prefix_bits, uint32_t *out) {
    if (len == 0) return 0;
    uint8_t maxp = (uint8_t)((1u << prefix_bits) - 1);
    *out = buf[0] & maxp;
    if (*out < maxp) return 1;
    size_t i = 1; uint32_t m = 0;
    while (i < len) {
        *out += (uint32_t)(buf[i] & 0x7F) << m;
        if (!(buf[i] & 0x80)) return i + 1;
        m += 7; i++;
        if (m > 28) return 0;               /* overlong -> malformed */
    }
    return 0;
}

/* Decode a Huffman-coded string (RFC 7541 Appendix B). Bit-by-bit prefix
 * match; the code is prefix-free so the first complete match is unique.
 * Trailing <8 all-ones bits are EOS padding and left unmatched. */
static int hpack_huff_decode(const uint8_t *src, size_t slen,
                             char *dst, size_t cap) {
    uint64_t acc = 0; int nbits = 0; size_t di = 0;
    for (size_t i = 0; i < slen; i++) {
        acc = (acc << 8) | src[i];
        nbits += 8;
        for (;;) {
            int sym = -1, mlen = 0;
            for (int len = 5; len <= nbits && len <= 30; len++) {
                uint32_t codev =
                    (uint32_t)((acc >> (nbits - len)) & (((uint64_t)1 << len) - 1));
                for (int s = 0; s < 256; s++) {
                    if (hpack_huff[s].nbits == len && hpack_huff[s].code == codev) {
                        sym = s; mlen = len; break;
                    }
                }
                if (sym >= 0) break;
            }
            if (sym < 0) break;
            if (di < cap - 1) dst[di++] = (char)sym;
            nbits -= mlen;
        }
    }
    if (di < cap) dst[di] = 0;
    return (int)di;
}

/* ---- HPACK decoder dynamic table ----------------------------------- */
#define H2_DYN_MAX 128
struct h2_dyn { char *name; char *value; uint32_t size; };

struct h2 {
    struct tls_conn *tls;
    uint32_t timeout_ms;
    /* transport read buffer */
    uint8_t  rbuf[H2_MAX_FRAME + 64];
    size_t   rlen, roff;
    /* HPACK dynamic table (newest at index 0). This is CONNECTION state:
     * the server indexes into it across requests, so it must survive for
     * as long as the connection is reused -- which is the whole reason
     * this struct is now parked with the connection rather than freed
     * per fetch. */
    struct h2_dyn dyn[H2_DYN_MAX];
    int      dyn_count;
    uint32_t dyn_size, dyn_max;
    /* connection reuse */
    int      started;            /* preface + SETTINGS already sent */
    uint32_t next_sid;           /* next client stream id (odd, +2) */
};

static void dyn_evict_to(struct h2 *h, uint32_t target) {
    while (h->dyn_count > 0 && h->dyn_size > target) {
        struct h2_dyn *e = &h->dyn[h->dyn_count - 1];
        h->dyn_size -= e->size;
        if (e->name) kfree(e->name);
        if (e->value) kfree(e->value);
        e->name = e->value = NULL;
        h->dyn_count--;
    }
}

static void dyn_insert(struct h2 *h, const char *name, const char *value) {
    uint32_t nlen = (uint32_t)strlen(name), vlen = (uint32_t)strlen(value);
    uint32_t esz = nlen + vlen + 32;
    dyn_evict_to(h, h->dyn_max > esz ? h->dyn_max - esz : 0);
    if (esz > h->dyn_max || h->dyn_count >= H2_DYN_MAX) return;  /* too big */
    char *nn = kmalloc(nlen + 1), *vv = kmalloc(vlen + 1);
    if (!nn || !vv) { if (nn) kfree(nn); if (vv) kfree(vv); return; }
    memcpy(nn, name, nlen + 1); memcpy(vv, value, vlen + 1);
    for (int i = h->dyn_count; i > 0; i--) h->dyn[i] = h->dyn[i - 1];
    h->dyn[0].name = nn; h->dyn[0].value = vv; h->dyn[0].size = esz;
    h->dyn_count++; h->dyn_size += esz;
}

static void dyn_free_all(struct h2 *h) {
    for (int i = 0; i < h->dyn_count; i++) {
        if (h->dyn[i].name) kfree(h->dyn[i].name);
        if (h->dyn[i].value) kfree(h->dyn[i].value);
    }
    h->dyn_count = 0; h->dyn_size = 0;
}

/* Resolve HPACK index -> name/value (value may be NULL out). */
static int hpack_lookup(struct h2 *h, uint32_t idx,
                        char *name, size_t ncap, char *value, size_t vcap) {
    if (idx == 0) return 0;
    if (idx <= HPACK_STATIC_N) {
        const char *n = hpack_static[idx].name, *v = hpack_static[idx].value;
        if (name) { size_t l = strlen(n); if (l >= ncap) l = ncap - 1;
                    memcpy(name, n, l); name[l] = 0; }
        if (value) { size_t l = strlen(v); if (l >= vcap) l = vcap - 1;
                     memcpy(value, v, l); value[l] = 0; }
        return 1;
    }
    uint32_t di = idx - HPACK_STATIC_N - 1;      /* 0 = newest */
    if ((int)di >= h->dyn_count) return 0;
    struct h2_dyn *e = &h->dyn[di];
    if (name) { size_t l = strlen(e->name); if (l >= ncap) l = ncap - 1;
                memcpy(name, e->name, l); name[l] = 0; }
    if (value) { size_t l = strlen(e->value); if (l >= vcap) l = vcap - 1;
                 memcpy(value, e->value, l); value[l] = 0; }
    return 1;
}

/* Decode one HPACK string (H-bit + length + octets). Returns bytes
 * consumed from *pp (advances it), -1 on malformed. */
static int hpack_dec_string(const uint8_t **pp, const uint8_t *end,
                            char *dst, size_t cap) {
    const uint8_t *p = *pp;
    if (p >= end) return -1;
    int huff = (p[0] & 0x80) != 0;
    uint32_t len = 0;
    size_t c = hpack_dec_int(p, (size_t)(end - p), 7, &len);
    if (c == 0) return -1;
    p += c;
    if (p + len > end) return -1;
    if (huff) {
        hpack_huff_decode(p, len, dst, cap);
    } else {
        size_t l = len; if (l >= cap) l = cap - 1;
        memcpy(dst, p, l); dst[l] = 0;
    }
    p += len;
    *pp = p;
    return 0;
}

/* ---- response accumulation ----------------------------------------- */
struct h2_resp {
    int      status;
    char     content_type[64];
    char     location[256];
    uint8_t  encoding;             /* HTTP_ENC_* */
    long     content_len;
    const char *host;              /* for Alt-Svc discovery (RFC 7838) */
};

static int str_has(const char *hay, const char *needle) {
    for (const char *s = hay; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void on_header(struct h2_resp *r, const char *name, const char *value) {
    if (name[0] == ':' ) {
        if (ci_eq(name, ":status")) {
            int s = 0; for (const char *p = value; *p >= '0' && *p <= '9'; p++)
                           s = s * 10 + (*p - '0');
            r->status = s;
        }
        return;
    }
    if (ci_eq(name, "content-type")) {
        size_t l = strlen(value); if (l >= sizeof(r->content_type)) l = sizeof(r->content_type) - 1;
        memcpy(r->content_type, value, l); r->content_type[l] = 0;
    } else if (ci_eq(name, "location")) {
        size_t l = strlen(value); if (l >= sizeof(r->location)) l = sizeof(r->location) - 1;
        memcpy(r->location, value, l); r->location[l] = 0;
    } else if (ci_eq(name, "content-encoding")) {
        if (str_has(value, "br")) r->encoding = HTTP_ENC_BR;
        else if (str_has(value, "gzip")) r->encoding = HTTP_ENC_GZIP;
    } else if (ci_eq(name, "content-length")) {
        long v = 0; for (const char *p = value; *p >= '0' && *p <= '9'; p++)
                        v = v * 10 + (*p - '0');
        r->content_len = v;
    } else if (ci_eq(name, "alt-svc")) {
        http_altsvc_note(r->host, value);    /* RFC 7838 discovery over h2 */
    }
}

/* Decode a complete header block (HEADERS + any CONTINUATIONs). */
static void hpack_decode_block(struct h2 *h, const uint8_t *data, size_t len,
                               struct h2_resp *r) {
    const uint8_t *p = data, *end = data + len;
    static char name[512], value[8192];      /* big; header decode is serial */
    while (p < end) {
        uint8_t b = *p;
        if (b & 0x80) {                        /* indexed */
            uint32_t idx = 0;
            size_t c = hpack_dec_int(p, (size_t)(end - p), 7, &idx);
            if (!c) break; p += c;
            if (hpack_lookup(h, idx, name, sizeof name, value, sizeof value))
                on_header(r, name, value);
        } else if (b & 0x40) {                 /* literal, incremental index */
            uint32_t idx = 0;
            size_t c = hpack_dec_int(p, (size_t)(end - p), 6, &idx);
            if (!c) break; p += c;
            if (idx) { if (!hpack_lookup(h, idx, name, sizeof name, NULL, 0)) break; }
            else if (hpack_dec_string(&p, end, name, sizeof name) < 0) break;
            if (hpack_dec_string(&p, end, value, sizeof value) < 0) break;
            dyn_insert(h, name, value);
            on_header(r, name, value);
        } else if (b & 0x20) {                 /* dynamic table size update */
            uint32_t sz = 0;
            size_t c = hpack_dec_int(p, (size_t)(end - p), 5, &sz);
            if (!c) break; p += c;
            if (sz <= HTTP2_HEADER_CAP) { h->dyn_max = sz; dyn_evict_to(h, sz); }
        } else {                               /* literal, no/never index */
            uint32_t idx = 0;
            size_t c = hpack_dec_int(p, (size_t)(end - p), 4, &idx);
            if (!c) break; p += c;
            if (idx) { if (!hpack_lookup(h, idx, name, sizeof name, NULL, 0)) break; }
            else if (hpack_dec_string(&p, end, name, sizeof name) < 0) break;
            if (hpack_dec_string(&p, end, value, sizeof value) < 0) break;
            on_header(r, name, value);
        }
    }
}

/* ---- frame transport over TLS -------------------------------------- */
static int h2_fill(struct h2 *h) {
    h->roff = 0;
    long n = tls_recv(h->tls, h->rbuf, sizeof(h->rbuf), h->timeout_ms);
    if (n <= 0) return -1;
    h->rlen = (size_t)n;
    return 1;
}

/* Read exactly n bytes into dst. */
static int h2_read(struct h2 *h, uint8_t *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (h->roff >= h->rlen && h2_fill(h) <= 0) return -1;
        size_t avail = h->rlen - h->roff, take = n - got;
        if (take > avail) take = avail;
        memcpy(dst + got, h->rbuf + h->roff, take);
        h->roff += take; got += take;
    }
    return 0;
}

static int h2_send(struct h2 *h, const void *buf, size_t len) {
    long r = tls_send(h->tls, buf, len);
    return (r == (long)len) ? 0 : -1;
}

static int h2_send_frame(struct h2 *h, uint8_t type, uint8_t flags,
                         uint32_t stream_id, const void *payload, size_t len) {
    uint8_t fh[9];
    fh[0] = (uint8_t)((len >> 16) & 0xFF);
    fh[1] = (uint8_t)((len >> 8) & 0xFF);
    fh[2] = (uint8_t)(len & 0xFF);
    fh[3] = type; fh[4] = flags;
    uint32_t sid = htonl(stream_id & 0x7FFFFFFF);
    memcpy(fh + 5, &sid, 4);
    if (h2_send(h, fh, 9) < 0) return -1;
    if (len > 0 && h2_send(h, payload, len) < 0) return -1;
    return 0;
}

/* Encode one request header (RFC 7541). Uses a fully-indexed static
 * entry on an exact name+value match, else literal-without-indexing with
 * a static name index when the name is known, else a fresh name. We
 * never add to our own (encoder) dynamic table -- simpler and legal. */
static size_t hpack_encode_req(uint8_t *buf, size_t cap,
                               const char *name, const char *value) {
    for (int i = 1; i <= HPACK_STATIC_N; i++)
        if (strcmp(hpack_static[i].name, name) == 0 &&
            strcmp(hpack_static[i].value, value) == 0)
            return hpack_enc_int(buf, (uint32_t)i, 7, 0x80);
    int name_idx = 0;
    for (int i = 1; i <= HPACK_STATIC_N; i++)
        if (strcmp(hpack_static[i].name, name) == 0) { name_idx = i; break; }
    size_t o = 0;
    if (name_idx > 0) {
        o += hpack_enc_int(buf + o, (uint32_t)name_idx, 4, 0x00);
    } else {
        buf[o++] = 0x00;
        size_t nlen = strlen(name);
        o += hpack_enc_int(buf + o, (uint32_t)nlen, 7, 0x00);
        if (o + nlen > cap) return o;
        memcpy(buf + o, name, nlen); o += nlen;
    }
    size_t vlen = strlen(value);
    o += hpack_enc_int(buf + o, (uint32_t)vlen, 7, 0x00);
    if (o + vlen > cap) return o;
    memcpy(buf + o, value, vlen); o += vlen;
    return o;
}

/* ---- the fetch ----------------------------------------------------- */
/* One request on connection state `h`. Never allocates or frees h --
 * the caller owns it so the connection can be parked and reused. */
static int h2_run(struct h2 *h, struct tls_conn *tls, const struct http_url *u,
                  unsigned flags, size_t max_body, uint32_t timeout_ms,
                  struct http_response *out) {
    h->tls = tls;
    h->timeout_ms = timeout_ms ? timeout_ms : 15000;
    uint32_t my_sid = h->next_sid;
    h->next_sid += 2;                           /* client streams are odd */
    uint64_t data_seen = 0;                     /* bytes to hand back below */

    /* Preface + our SETTINGS (initial window + push disabled) + a big
     * connection WINDOW_UPDATE so the server may stream the whole body. */
    if (!h->started) {
    if (h2_send(h, H2_PREFACE, H2_PREFACE_LEN) < 0) { return HTTP_ERR_RESET; }
    {
        uint8_t s[12];
        s[0] = 0; s[1] = H2_SETTINGS_ENABLE_PUSH;   s[2]=0;s[3]=0;s[4]=0;s[5]=0;
        s[6] = 0; s[7] = H2_SETTINGS_INITIAL_WINDOW_SIZE;
        s[8] = (uint8_t)(H2_MY_WINDOW >> 24); s[9] = (uint8_t)(H2_MY_WINDOW >> 16);
        s[10] = (uint8_t)(H2_MY_WINDOW >> 8); s[11] = (uint8_t)(H2_MY_WINDOW);
        if (h2_send_frame(h, H2_SETTINGS, 0, 0, s, 12) < 0) { return HTTP_ERR_RESET; }
    }
    {
        uint32_t inc = htonl(H2_MY_WINDOW);
        h2_send_frame(h, H2_WINDOW_UPDATE, 0, 0, &inc, 4);
    }
    h->started = 1;
    }

    /* HEADERS on our stream id: pseudo-headers + accept-encoding + UA. */
    {
        uint8_t hb[1024];
        size_t o = 0;
        o += hpack_encode_req(hb + o, sizeof hb - o, ":method", "GET");
        o += hpack_encode_req(hb + o, sizeof hb - o, ":scheme", "https");
        o += hpack_encode_req(hb + o, sizeof hb - o, ":path",
                              u->path[0] ? u->path : "/");
        o += hpack_encode_req(hb + o, sizeof hb - o, ":authority", u->host);
        if (flags & HTTP_F_GZIP)
            o += hpack_encode_req(hb + o, sizeof hb - o, "accept-encoding",
                                  "gzip, br");
        o += hpack_encode_req(hb + o, sizeof hb - o, "accept", "*/*");
        o += hpack_encode_req(hb + o, sizeof hb - o, "user-agent",
                              "Mozilla/5.0 (compatible; tobyOS 1.0; x86_64) "
                              "tobyOS-Browser/3.0");
        if (h2_send_frame(h, H2_HEADERS,
                          H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM,
                          my_sid, hb, o) < 0) { return HTTP_ERR_RESET; }
    }

    /* Response body buffer (raw / possibly compressed). */
    uint8_t *body = (uint8_t *)kmalloc(max_body ? max_body : 1);
    if (!body) { return HTTP_ERR_NOMEM; }
    size_t body_len = 0;

    struct h2_resp r;
    memset(&r, 0, sizeof(r));
    r.content_len = -1;
    r.host = u ? u->host : NULL;

    uint8_t *fpay = (uint8_t *)kmalloc(H2_MAX_FRAME + 1);
    uint8_t *hdrblock = (uint8_t *)kmalloc(HTTP2_HEADER_CAP);
    size_t hdrblock_len = 0;
    int collecting_headers = 0;
    if (!fpay || !hdrblock) {
        if (fpay) kfree(fpay); if (hdrblock) kfree(hdrblock);
        kfree(body);
        return HTTP_ERR_NOMEM;
    }

    int done = 0, rc = 0;
    int guard = 0;
    while (!done && guard++ < 100000) {
        uint8_t fh[9];
        if (h2_read(h, fh, 9) < 0) { rc = body_len ? 0 : HTTP_ERR_TIMEOUT; break; }
        uint32_t flen = ((uint32_t)fh[0] << 16) | ((uint32_t)fh[1] << 8) | fh[2];
        uint8_t ftype = fh[3], fflags = fh[4];
        uint32_t sid; memcpy(&sid, fh + 5, 4); sid = ntohl(sid) & 0x7FFFFFFF;
        if (flen > H2_MAX_FRAME) { rc = HTTP_ERR_PROTOCOL; break; }
        if (flen && h2_read(h, fpay, flen) < 0) { rc = HTTP_ERR_RESET; break; }

        switch (ftype) {
        case H2_SETTINGS:
            if (!(fflags & H2_FLAG_ACK))
                h2_send_frame(h, H2_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
            break;
        case H2_PING:
            if (!(fflags & H2_FLAG_ACK))
                h2_send_frame(h, H2_PING, H2_FLAG_ACK, 0, fpay, flen);
            break;
        case H2_GOAWAY:
            done = 1;                          /* stop; keep what we have */
            break;
        case H2_RST_STREAM:
            if (sid == my_sid) { rc = body_len ? 0 : HTTP_ERR_RESET; done = 1; }
            break;
        case H2_PUSH_PROMISE:
            /* refuse pushes (we set ENABLE_PUSH=0 anyway) */
            break;
        case H2_WINDOW_UPDATE:
            break;                             /* we advertised a big window */
        case H2_HEADERS:
        case H2_CONTINUATION: {
            const uint8_t *hp = fpay; size_t hlen = flen;
            if (ftype == H2_HEADERS) {
                if (fflags & H2_FLAG_PADDED) {  /* strip pad length + padding */
                    if (hlen < 1) break;
                    uint8_t pad = hp[0]; hp++; hlen--;
                    if (pad > hlen) break; hlen -= pad;
                }
                if (fflags & H2_FLAG_PRIORITY) { /* skip 5-byte priority */
                    if (hlen < 5) break;
                    hp += 5; hlen -= 5;
                }
                hdrblock_len = 0;
                collecting_headers = 1;
            }
            if (collecting_headers && hdrblock_len + hlen <= HTTP2_HEADER_CAP) {
                memcpy(hdrblock + hdrblock_len, hp, hlen);
                hdrblock_len += hlen;
            }
            if (fflags & H2_FLAG_END_HEADERS) {
                hpack_decode_block(h, hdrblock, hdrblock_len, &r);
                collecting_headers = 0;
            }
            if ((fflags & H2_FLAG_END_STREAM) && sid == my_sid) done = 1;
            break;
        }
        case H2_DATA: {
            const uint8_t *dp = fpay; size_t dlen = flen;
            /* Flow control counts the WHOLE payload, padding included
             * (RFC 7540 6.9.1), so account before de-padding. */
            data_seen += flen;
            if (fflags & H2_FLAG_PADDED) {
                if (dlen < 1) break;
                uint8_t pad = dp[0]; dp++; dlen--;
                if (pad > dlen) break; dlen -= pad;
            }
            if (sid == my_sid && dlen) {
                size_t room = max_body - body_len;
                size_t take = dlen < room ? dlen : room;
                if (take) { memcpy(body + body_len, dp, take); body_len += take; }
            }
            if ((fflags & H2_FLAG_END_STREAM) && sid == my_sid) done = 1;
            break;
        }
        default:
            break;
        }
    }

    kfree(fpay); kfree(hdrblock);

    /* Hand the consumed DATA bytes back to the CONNECTION flow-control
     * window. The initial grant is one-shot; without this a reused
     * connection would stall once its cumulative bodies exhausted it. */
    if (data_seen) {
        uint32_t inc = htonl((uint32_t)(data_seen & 0x7FFFFFFF));
        h2_send_frame(h, H2_WINDOW_UPDATE, 0, 0, &inc, 4);
    }

    if (rc != 0 && body_len == 0) { kfree(body); return rc; }

    /* Populate the response; decompress like the h1 path. */
    memset(out, 0, sizeof(*out));
    out->status = r.status ? r.status : 200;
    memcpy(out->content_type, r.content_type, sizeof(out->content_type));
    memcpy(out->location, r.location, sizeof(out->location));
    out->encoding = r.encoding;
    out->content_len = r.content_len;

    if ((r.encoding == HTTP_ENC_GZIP || r.encoding == HTTP_ENC_BR) && body_len) {
        unsigned long dlen = max_body ? max_body : 1;
        uint8_t *d = (uint8_t *)kmalloc(dlen);
        if (d) {
            int ok;
            if (r.encoding == HTTP_ENC_BR) {
                int pr = brotli_decompress(d, &dlen, body, body_len);
                ok = (pr == BROTLI_OK || pr == BROTLI_TRUNC);
            } else {
                int pr = puff_gzip(d, &dlen, body, body_len);
                ok = (pr == PUFF_OK || pr == PUFF_TRUNC);
            }
            if (ok) {
                kprintf("[h2] %s %lu -> %lu\n",
                        r.encoding == HTTP_ENC_BR ? "unbrotli" : "gunzip",
                        (unsigned long)body_len, dlen);
                kfree(body); body = d; body_len = dlen;
                out->encoding = HTTP_ENC_IDENTITY;
            } else {
                kfree(d);
            }
        }
    }

    out->body = body;
    out->body_len = body_len;
    out->content_len = (long)body_len;
    kprintf("[h2] %d; body=%lu bytes; type=\"%s\"\n",
            out->status, (unsigned long)body_len,
            out->content_type[0] ? out->content_type : "(none)");
    return 0;
}

/* ---- public entry points ------------------------------------------- */

void http2_state_free(struct h2 *h) {
    if (!h) return;
    dyn_free_all(h);
    kfree(h);
}

/* Run a GET, carrying connection state across calls so the TLS
 * connection can be reused. *ph == NULL starts a fresh connection; on
 * success *ph holds state the caller may park alongside the tls_conn.
 * On ANY failure the state is freed and *ph set to NULL: a half-failed
 * h2 connection has indeterminate HPACK/stream state and must not be
 * reused. */
int http2_fetch_on(struct h2 **ph, struct tls_conn *tls,
                   const struct http_url *u, unsigned flags, size_t max_body,
                   uint32_t timeout_ms, struct http_response *out) {
    if (!ph) return HTTP_ERR_PROTOCOL;
    struct h2 *h = *ph;
    if (!h) {
        h = (struct h2 *)kmalloc(sizeof(struct h2));
        if (!h) return HTTP_ERR_NOMEM;
        memset(h, 0, sizeof(*h));
        h->dyn_max  = 4096;                     /* HPACK default */
        h->next_sid = 1;                        /* client streams are odd */
    }
    *ph = h;
    int rc = h2_run(h, tls, u, flags, max_body, timeout_ms, out);
    if (rc != 0) { http2_state_free(h); *ph = NULL; }
    return rc;
}

/* One-shot: fresh connection state, discarded afterwards. */
int http2_fetch(struct tls_conn *tls, const struct http_url *u,
                unsigned flags, size_t max_body, uint32_t timeout_ms,
                struct http_response *out) {
    struct h2 *h = NULL;
    int rc = http2_fetch_on(&h, tls, u, flags, max_body, timeout_ms, out);
    http2_state_free(h);
    return rc;
}
