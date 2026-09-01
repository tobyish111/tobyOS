/* deflate.c -- fixed-Huffman DEFLATE (RFC 1951) in a gzip wrapper.
 *
 * LZ77 over a 32 KiB window with hash chains, then the fixed Huffman tables
 * of RFC 1951. Dynamic tables would compress a few percent better, but they
 * mean shipping a second Huffman tree in every block; fixed tables are a
 * complete, standard-conformant encoder that any gzip or zlib on earth
 * decodes, which is the property that actually matters here.
 *
 * Bit order is the classic trap: DEFLATE packs bits into bytes starting at
 * the LEAST significant bit, but Huffman codes are emitted MOST significant
 * bit first. Those are two different rules in one stream, and mixing them
 * up produces output that looks plausible and decodes to garbage.
 * bw_bits() and bw_huff() below keep them apart.
 *
 * The decompression half already existed: src/puff.c has been inflating
 * Content-Encoding: gzip for the browser for a long time, and userland
 * compiles that same source rather than carrying a second copy.
 */

#include <toby/deflate.h>

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- CRC-32 */

static uint32_t crc_tab[256];
static int      crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

uint32_t gz_crc32(const uint8_t *p, size_t n, uint32_t seed) {
    if (!crc_ready) crc_init();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t gz_isize(const uint8_t *p, size_t n) {
    if (!p || n < 8) return 0;
    const uint8_t *t = p + n - 4;
    return (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
           ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
}

/* ------------------------------------------------------------ bit writer */

struct bw {
    uint8_t *buf;
    size_t   cap, len;
    uint32_t acc;
    int      nbits;
    int      err;
};

static int bw_grow(struct bw *b, size_t need) {
    if (b->len + need <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + need) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->buf, nc);
    if (!p) { b->err = 1; return 0; }
    b->buf = p;
    b->cap = nc;
    return 1;
}

/* Packs LSB-first. n is never more than 16 on any path here. */
static void bw_bits(struct bw *b, uint32_t val, int n) {
    if (b->err || n <= 0) return;
    b->acc |= (val & ((1u << n) - 1u)) << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) {
        if (!bw_grow(b, 1)) return;
        b->buf[b->len++] = (uint8_t)(b->acc & 0xFFu);
        b->acc >>= 8;
        b->nbits -= 8;
    }
}

/* Huffman codes go out MSB-first -- the opposite of bw_bits(). */
static void bw_huff(struct bw *b, uint32_t code, int n) {
    for (int i = n - 1; i >= 0; i--) bw_bits(b, (code >> i) & 1u, 1);
}

static void bw_align(struct bw *b) {
    if (b->err || b->nbits == 0) return;
    if (!bw_grow(b, 1)) return;
    b->buf[b->len++] = (uint8_t)(b->acc & 0xFFu);
    b->acc = 0;
    b->nbits = 0;
}

static void bw_byte(struct bw *b, uint8_t v) {
    if (b->err) return;
    if (!bw_grow(b, 1)) return;
    b->buf[b->len++] = v;
}

/* ------------------------------------------------- RFC 1951 fixed tables */

static void fixed_lit(unsigned sym, uint32_t *code, int *len) {
    if (sym <= 143)      { *code = 0x30u  + sym;         *len = 8; }
    else if (sym <= 255) { *code = 0x190u + (sym - 144); *len = 9; }
    else if (sym <= 279) { *code = 0x00u  + (sym - 256); *len = 7; }
    else                 { *code = 0xC0u  + (sym - 280); *len = 8; }
}

static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static void emit_literal(struct bw *b, uint8_t c) {
    uint32_t code; int n;
    fixed_lit(c, &code, &n);
    bw_huff(b, code, n);
}

static void emit_match(struct bw *b, unsigned len, unsigned dist) {
    int i = 28;
    while (i > 0 && len < len_base[i]) i--;
    uint32_t code; int n;
    fixed_lit(257u + (unsigned)i, &code, &n);
    bw_huff(b, code, n);
    if (len_extra[i]) bw_bits(b, len - len_base[i], len_extra[i]);

    int j = 29;
    while (j > 0 && dist < dist_base[j]) j--;
    /* Fixed distance codes are five bits, straight binary, MSB first. */
    bw_huff(b, (uint32_t)j, 5);
    if (dist_extra[j]) bw_bits(b, dist - dist_base[j], dist_extra[j]);
}

/* ----------------------------------------------------------- LZ77 window */

#define MIN_MATCH  3
#define MAX_MATCH  258
#define WSIZE      32768
#define HASH_BITS  15
#define HASH_SIZE  (1 << HASH_BITS)

static unsigned hash3(const uint8_t *p) {
    return (unsigned)((((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^
                       (unsigned)p[2]) & (HASH_SIZE - 1));
}


/* Stored blocks: BTYPE=00, byte-aligned, LEN/NLEN, then the bytes raw.
 * Real gzip falls back to these when a block will not compress, which is
 * what keeps `gzip` on already-compressed data from growing the file by
 * several percent. Without it random input expanded 5.4%. */
static void emit_stored(struct bw *b, const uint8_t *in, size_t inlen) {
    size_t off = 0;
    do {
        size_t n = inlen - off;
        if (n > 65535) n = 65535;
        int last = (off + n >= inlen);
        bw_bits(b, (uint32_t)(last ? 1 : 0), 1);
        bw_bits(b, 0, 2);                 /* BTYPE = 00 */
        bw_align(b);                      /* LEN is byte-aligned */
        bw_byte(b, (uint8_t)(n & 0xFF));
        bw_byte(b, (uint8_t)((n >> 8) & 0xFF));
        bw_byte(b, (uint8_t)(~n & 0xFF));
        bw_byte(b, (uint8_t)((~n >> 8) & 0xFF));
        for (size_t i = 0; i < n; i++) bw_byte(b, in[off + i]);
        off += n;
    } while (off < inlen);
}

/* The gzip wrapper and the bare stream share one encoder: `raw_only`
 * skips the header and trailer, which is exactly what a ZIP entry stores.
 * Two copies of an LZ77 matcher would be two places for it to drift. */
static int deflate_core(const uint8_t *in, size_t inlen, int level,
                        const char *name, uint32_t mtime, int raw_only,
                        uint8_t **out, size_t *outlen)
{
    if (!out || !outlen) return -1;
    if (!in && inlen) return -1;

    if (level < 1) level = 1;
    if (level > 9) level = 9;
    /* How far down a hash chain to walk, and when a match is good enough to
     * stop looking. This is the whole speed-versus-ratio knob; the output is
     * a valid stream at any setting. */
    unsigned max_chain = (level <= 1) ? 8u
                       : (level >= 9) ? 4096u
                       : (unsigned)level * 48u;
    unsigned good_len  = (level <= 3) ? 8u : 32u;

    struct bw b;
    memset(&b, 0, sizeof b);

    /* ---- gzip header (RFC 1952), skipped for a bare stream ---- */
    int have_name = !raw_only && name && name[0];
    if (!raw_only) {
    bw_byte(&b, 0x1F);
    bw_byte(&b, 0x8B);
    bw_byte(&b, 8);                               /* CM = deflate */
    bw_byte(&b, (uint8_t)(have_name ? 0x08 : 0)); /* FLG: FNAME */
    bw_byte(&b, (uint8_t)(mtime & 0xFF));
    bw_byte(&b, (uint8_t)((mtime >> 8) & 0xFF));
    bw_byte(&b, (uint8_t)((mtime >> 16) & 0xFF));
    bw_byte(&b, (uint8_t)((mtime >> 24) & 0xFF));
    bw_byte(&b, (uint8_t)(level >= 9 ? 2 : (level <= 1 ? 4 : 0))); /* XFL */
    bw_byte(&b, 3);                               /* OS = Unix */
    if (have_name) {
        for (const char *p = name; *p; p++) bw_byte(&b, (uint8_t)*p);
        bw_byte(&b, 0);
    }
    }
    if (b.err) { free(b.buf); return -1; }

    /* Where the deflate body begins, so it can be discarded and re-emitted
     * as stored blocks if compressing made things worse. */
    size_t body_start = b.len;

    int32_t *head = (int32_t *)malloc((size_t)HASH_SIZE * sizeof(int32_t));
    int32_t *prev = (int32_t *)malloc((inlen ? inlen : 1) * sizeof(int32_t));
    if (!head || !prev) { free(head); free(prev); free(b.buf); return -1; }
    for (size_t i = 0; i < HASH_SIZE; i++) head[i] = -1;

    /* ---- one final fixed-Huffman block ---- */
    bw_bits(&b, 1, 1);   /* BFINAL */
    bw_bits(&b, 1, 2);   /* BTYPE = 01, fixed Huffman */

    size_t pos = 0;
    while (pos < inlen) {
        unsigned best_len = 0;
        size_t   best_dist = 0;

        if (pos + MIN_MATCH <= inlen) {
            unsigned h = hash3(in + pos);
            int32_t  cand = head[h];
            unsigned chain = max_chain;
            size_t   limit = (pos > WSIZE) ? pos - WSIZE : 0;

            while (cand >= 0 && (size_t)cand >= limit && chain--) {
                const uint8_t *s = in + pos;
                const uint8_t *m = in + cand;
                size_t maxlen = inlen - pos;
                if (maxlen > MAX_MATCH) maxlen = MAX_MATCH;
                unsigned l = 0;
                while (l < maxlen && s[l] == m[l]) l++;
                if (l > best_len) {
                    best_len  = l;
                    best_dist = pos - (size_t)cand;
                    if (l >= good_len || l >= MAX_MATCH) break;
                }
                cand = prev[cand];
            }
        }

        if (best_len >= MIN_MATCH) {
            emit_match(&b, best_len, (unsigned)best_dist);
            /* Every position inside the match still has to enter the hash
             * chains, or later matches cannot see through it. */
            for (unsigned k = 0; k < best_len; k++) {
                size_t q = pos + k;
                if (q + MIN_MATCH <= inlen) {
                    unsigned h = hash3(in + q);
                    prev[q] = head[h];
                    head[h] = (int32_t)q;
                }
            }
            pos += best_len;
        } else {
            emit_literal(&b, in[pos]);
            if (pos + MIN_MATCH <= inlen) {
                unsigned h = hash3(in + pos);
                prev[pos] = head[h];
                head[h] = (int32_t)pos;
            }
            pos++;
        }
        if (b.err) break;
    }

    free(head);
    free(prev);
    if (b.err) { free(b.buf); return -1; }

    /* End-of-block symbol, then pad to a byte boundary before the trailer. */
    { uint32_t code; int n; fixed_lit(256, &code, &n); bw_huff(&b, code, n); }
    bw_align(&b);

    /* If Huffman coding made it BIGGER -- which it does on data that is
     * already compressed -- throw that away and store the bytes instead.
     * Stored costs 5 bytes per 64 KiB block, so the comparison is against
     * that, not against inlen alone. */
    {
        size_t nblocks = inlen ? (inlen + 65534) / 65535 : 1;
        size_t stored_len = inlen + 5 * nblocks;
        if (b.len - body_start > stored_len) {
            b.len   = body_start;
            b.acc   = 0;
            b.nbits = 0;
            emit_stored(&b, in, inlen);
            bw_align(&b);
        }
    }

    if (raw_only) {
        if (b.err) { free(b.buf); return -1; }
        *out = b.buf; *outlen = b.len;
        return 0;
    }

    uint32_t crc = gz_crc32(in, inlen, 0);
    uint32_t isz = (uint32_t)(inlen & 0xFFFFFFFFu);
    bw_byte(&b, (uint8_t)(crc & 0xFF));
    bw_byte(&b, (uint8_t)((crc >> 8) & 0xFF));
    bw_byte(&b, (uint8_t)((crc >> 16) & 0xFF));
    bw_byte(&b, (uint8_t)((crc >> 24) & 0xFF));
    bw_byte(&b, (uint8_t)(isz & 0xFF));
    bw_byte(&b, (uint8_t)((isz >> 8) & 0xFF));
    bw_byte(&b, (uint8_t)((isz >> 16) & 0xFF));
    bw_byte(&b, (uint8_t)((isz >> 24) & 0xFF));

    if (b.err) { free(b.buf); return -1; }
    *out = b.buf;
    *outlen = b.len;
    return 0;
}

int gz_compress(const uint8_t *in, size_t inlen, int level,
                const char *name, uint32_t mtime,
                uint8_t **out, size_t *outlen)
{
    return deflate_core(in, inlen, level, name, mtime, 0, out, outlen);
}

int deflate_raw(const uint8_t *in, size_t inlen, int level,
                uint8_t **out, size_t *outlen)
{
    return deflate_core(in, inlen, level, NULL, 0, 1, out, outlen);
}
