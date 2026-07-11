/* http3.c -- HTTP/3 framing + QPACK field-section coding (RFC 9114 /
 * RFC 9204). See http3.h. The static table is generated from the
 * pylsqpack (ls-qpack) reference (qpack_static.h); the string Huffman
 * code is RFC 7541's, shared with HPACK (hpack_huff_table.h). */

#include <tobyos/http3.h>
#include <tobyos/quic_crypto.h>   /* quic_varint_encode (H3 frame headers) */
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#include "qpack_static.h"         /* qpack_static[], QPACK_STATIC_N */
#include "hpack_huff_table.h"     /* hpack_huff[257] = {code,nbits} */

/* ---- QPACK prefix integers (RFC 9204 s4.1.1, == RFC 7541 s5.1) --- */

static size_t qp_int_enc(uint8_t *out, size_t cap, uint8_t pattern,
                         unsigned prefix_bits, uint64_t value) {
    uint8_t maxp = (uint8_t)((1u << prefix_bits) - 1);
    if (cap < 1) return 0;
    if (value < maxp) { out[0] = pattern | (uint8_t)value; return 1; }
    out[0] = pattern | maxp;
    value -= maxp;
    size_t i = 1;
    while (value >= 128) {
        if (i >= cap) return 0;
        out[i++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }
    if (i >= cap) return 0;
    out[i++] = (uint8_t)value;
    return i;
}

static size_t qp_int_dec(const uint8_t *p, size_t cap,
                         unsigned prefix_bits, uint64_t *out) {
    if (cap == 0) return 0;
    uint8_t maxp = (uint8_t)((1u << prefix_bits) - 1);
    *out = p[0] & maxp;
    if (*out < maxp) return 1;
    size_t i = 1; unsigned m = 0;
    while (i < cap) {
        *out += (uint64_t)(p[i] & 0x7F) << m;
        if (!(p[i] & 0x80)) return i + 1;
        m += 7; i++;
        if (m > 56) return 0;              /* overlong -> malformed */
    }
    return 0;
}

/* ---- Huffman-coded strings (RFC 7541 Appendix B, shared w/ HPACK) - */

static int qp_huff_decode(const uint8_t *src, size_t slen,
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

/* Read one QPACK string: length as a prefix int on the SAME first byte
 * that carries the H (Huffman) flag at `hbit`. Returns bytes consumed,
 * 0 on malformed. */
static size_t qp_string_dec(const uint8_t *p, size_t cap,
                            unsigned prefix_bits, uint8_t hbit,
                            char *dst, size_t dcap) {
    if (cap == 0) return 0;
    int huff = (p[0] & hbit) != 0;
    uint64_t len;
    size_t c = qp_int_dec(p, cap, prefix_bits, &len);
    if (!c || c + len > cap) return 0;
    if (huff) {
        qp_huff_decode(p + c, (size_t)len, dst, dcap);
    } else {
        size_t l = (size_t)len; if (l >= dcap) l = dcap - 1;
        memcpy(dst, p + c, l); dst[l] = 0;
    }
    return c + (size_t)len;
}

/* ---- Field-section encode (request; static/literal only) --------- */

/* One field line: exact static match -> indexed field line; name-only
 * static match -> literal with name reference; else literal with
 * literal name. Values written raw (H=0) -- interoperable and simple. */
static size_t qp_put_hdr(uint8_t *out, size_t cap,
                         const char *name, const char *value) {
    size_t nlen = strlen(name), vlen = strlen(value);
    int name_idx = -1;
    for (int i = 0; i < QPACK_STATIC_N; i++) {
        if (strcmp(qpack_static[i].name, name) != 0) continue;
        if (name_idx < 0) name_idx = i;
        if (strcmp(qpack_static[i].value, value) == 0) {
            /* Indexed Field Line: '1' T=1(static) + 6-bit index. */
            return qp_int_enc(out, cap, 0xC0, 6, (uint64_t)i);
        }
    }
    size_t o = 0;
    if (name_idx >= 0) {
        /* Literal Field Line With Name Reference:
         * '01' N=0 T=1(static) + 4-bit name index; value H=0 + 7-bit. */
        size_t c = qp_int_enc(out, cap, 0x50, 4, (uint64_t)name_idx);
        if (!c) return 0;
        o += c;
    } else {
        /* Literal Field Line With Literal Name:
         * '001' N=0 H=0 + 3-bit name length; name; value H=0 + 7-bit. */
        size_t c = qp_int_enc(out + o, cap - o, 0x20, 3, nlen);
        if (!c || o + c + nlen > cap) return 0;
        o += c;
        memcpy(out + o, name, nlen); o += nlen;
    }
    size_t c = qp_int_enc(out + o, cap - o, 0x00, 7, vlen);
    if (!c || o + c + vlen > cap) return 0;
    o += c;
    memcpy(out + o, value, vlen); o += vlen;
    return o;
}

size_t h3_qpack_encode_request(uint8_t *out, size_t cap,
                               const char *authority, const char *path) {
    if (cap < 8) return 0;
    /* Encoded field section prefix: Required Insert Count 0 (no
     * dynamic table) + Base 0. */
    out[0] = 0; out[1] = 0;
    size_t o = 2, c;
    if (!(c = qp_put_hdr(out + o, cap - o, ":method", "GET"))) return 0;
    o += c;
    if (!(c = qp_put_hdr(out + o, cap - o, ":scheme", "https"))) return 0;
    o += c;
    if (!(c = qp_put_hdr(out + o, cap - o, ":path", path))) return 0;
    o += c;
    if (!(c = qp_put_hdr(out + o, cap - o, ":authority", authority))) return 0;
    o += c;
    return o;
}

/* ---- Field-section decode (response) ------------------------------ */

int h3_qpack_decode(const uint8_t *p, size_t len, h3_hdr_cb cb, void *ud) {
    size_t o = 0;
    uint64_t ric, base;
    size_t c = qp_int_dec(p, len, 8, &ric);
    if (!c) return -1;
    o += c;
    if (ric != 0) return -1;               /* dynamic table: we offered none */
    c = qp_int_dec(p + o, len - o, 7, &base);
    if (!c) return -1;
    o += c;

    char name[128], value[512];
    while (o < len) {
        uint8_t b = p[o];
        if (b & 0x80) {
            /* Indexed Field Line: T at 0x40, 6-bit index. */
            if (!(b & 0x40)) return -1;    /* dynamic reference */
            uint64_t idx;
            c = qp_int_dec(p + o, len - o, 6, &idx);
            if (!c || idx >= QPACK_STATIC_N) return -1;
            o += c;
            cb(ud, qpack_static[idx].name, qpack_static[idx].value);
        } else if (b & 0x40) {
            /* Literal With Name Reference: N 0x20, T 0x10, 4-bit index. */
            if (!(b & 0x10)) return -1;    /* dynamic reference */
            uint64_t idx;
            c = qp_int_dec(p + o, len - o, 4, &idx);
            if (!c || idx >= QPACK_STATIC_N) return -1;
            o += c;
            c = qp_string_dec(p + o, len - o, 7, 0x80, value, sizeof value);
            if (!c) return -1;
            o += c;
            cb(ud, qpack_static[idx].name, value);
        } else if (b & 0x20) {
            /* Literal With Literal Name: N 0x10, name-H 0x08, 3-bit len. */
            c = qp_string_dec(p + o, len - o, 3, 0x08, name, sizeof name);
            if (!c) return -1;
            o += c;
            c = qp_string_dec(p + o, len - o, 7, 0x80, value, sizeof value);
            if (!c) return -1;
            o += c;
            cb(ud, name, value);
        } else {
            return -1;                     /* post-base forms: dynamic only */
        }
    }
    return 0;
}

/* ---- H3 frame header ---------------------------------------------- */

size_t h3_frame_hdr(uint8_t *out, size_t cap, uint64_t type, uint64_t plen) {
    uint8_t buf[16];
    size_t n = quic_varint_encode(buf, type);
    n += quic_varint_encode(buf + n, plen);
    if (n > cap) return 0;
    memcpy(out, buf, n);
    return n;
}

/* ---- Self-test ----------------------------------------------------- */

/* A response field section encoded by pylsqpack (ls-qpack) with an
 * empty dynamic table: indexed-static, literal-with-name-reference and
 * literal-with-literal-name lines, Huffman-coded literals. Decodes to
 *   :status: 200
 *   content-type: text/html; charset=utf-8
 *   content-length: 1234
 *   x-tobyos: h3 vector                       (see scratchpad qpack_gen.py) */
static const uint8_t qv_resp[] = {
    0x00, 0x00, 0xd9, 0xf4, 0x54, 0x83, 0x08, 0x99, 0x6b, 0x2e, 0xf2, 0xb2,
    0x4f, 0x1f, 0xa3, 0xa3, 0x87, 0x9d, 0x95, 0x3b, 0x94, 0x89, 0x3d, 0x9f,
};

struct qv_check {
    int n, ok;
};

static void qv_cb(void *ud, const char *name, const char *value) {
    static const char *exp[][2] = {
        { ":status", "200" },
        { "content-type", "text/html; charset=utf-8" },
        { "content-length", "1234" },
        { "x-tobyos", "h3 vector" },
    };
    struct qv_check *ck = ud;
    if (ck->n < 4 && strcmp(name, exp[ck->n][0]) == 0 &&
        strcmp(value, exp[ck->n][1]) == 0)
        ck->ok++;
    ck->n++;
}

int h3_selftest(void) {
    int pass = 0;

    /* 1: decode the pylsqpack reference vector (incl Huffman). */
    struct qv_check ck = { 0, 0 };
    int rc = h3_qpack_decode(qv_resp, sizeof qv_resp, qv_cb, &ck);
    int dec_ok = (rc == 0 && ck.n == 4 && ck.ok == 4);
    kprintf("[h3] QPACK decode (pylsqpack vector) %s (%d/4 fields)\n",
            dec_ok ? "OK" : "FAIL", ck.ok);
    pass += dec_ok;

    /* 2: encode a request section; sanity + hex dump for offline
     * pylsqpack validation (the 4a pattern). */
    uint8_t req[256];
    size_t rl = h3_qpack_encode_request(req, sizeof req, "tobyos.test", "/");
    int enc_ok = (rl > 4 && req[0] == 0 && req[1] == 0);
    kprintf("[h3] QPACK request encode           %s (%u bytes)\n",
            enc_ok ? "OK" : "FAIL", (unsigned)rl);
    pass += enc_ok;
    kprintf("[h3] request-section ");
    for (size_t i = 0; i < rl; i++) kprintf("%02x", req[i]);
    kprintf("\n");

    /* 3: H3 frame header round-trip sizes. */
    uint8_t fh[16];
    size_t f1 = h3_frame_hdr(fh, sizeof fh, H3_FRAME_HEADERS, rl);
    int fh_ok = (f1 == 2 && fh[0] == H3_FRAME_HEADERS && fh[1] == rl);
    kprintf("[h3] frame header                   %s\n", fh_ok ? "OK" : "FAIL");
    pass += fh_ok;

    kprintf("[h3] http3 self-test: %d/%d %s\n", pass, H3_SELFTEST_N,
            pass == H3_SELFTEST_N ? "ALL PASS" : "FAILURES");
    return pass;
}
