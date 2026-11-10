/* libtoby/src/xz.c -- .xz container and LZMA2/LZMA, from scratch.
 *
 * Written against the LZMA specification (LzmaSpec.cpp, Igor Pavlov) and
 * the .xz file format 1.1.0 (Lasse Collin). No code is derived from either
 * implementation; the range coder arithmetic, state machine and probability
 * layout are what the format defines, and there is only one way to write
 * them that decodes real files.
 *
 * Layout of an .xz file:
 *   stream header (12) | block* | index | stream footer (12) [| padding | stream ...]
 *   block = block header (4..1024, self-sized) | LZMA2 chunks | pad4 | check
 * LZMA2 chunks: 0x00 end, 0x01/0x02 uncompressed (BE16 size-1), 0x80.. LZMA
 * with reset mode in bits 5-6, 21-bit unpacked size, BE16 packed size, and a
 * properties byte when the mode says so.
 *
 * The decoder keeps the ENTIRE output in memory and uses it as the
 * dictionary: simpler than a sliding window, and exact (a distance is valid
 * iff it reaches into what has been produced). Inputs are files, not
 * pipes of unbounded length; unxz on a 100 MB archive is 100 MB of RAM. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <toby/xz.h>

/* ------------------------------------------------------------------ CRCs */
static uint32_t g_crc32_tab[256];
static uint64_t g_crc64_tab[256];
static int g_crc_ready;

static void crc_tables(void) {
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc32_tab[i] = c;
        uint64_t d = i;
        for (int k = 0; k < 8; k++) d = (d & 1) ? (0xC96C5795D7870F42ull ^ (d >> 1)) : (d >> 1);
        g_crc64_tab[i] = d;
    }
    g_crc_ready = 1;
}

uint32_t toby_xz_crc32(uint32_t crc, const void *buf, size_t len) {
    crc_tables();
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--) crc = g_crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint64_t toby_xz_crc64(uint64_t crc, const void *buf, size_t len) {
    crc_tables();
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--) crc = g_crc64_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------ growable out */
struct obuf { uint8_t *p; size_t len, cap; };

static int ob_reserve(struct obuf *o, size_t extra) {
    if (o->len + extra <= o->cap) return 0;
    size_t ncap = o->cap ? o->cap : 65536;
    while (ncap < o->len + extra) ncap = ncap + ncap / 2 + 65536;
    uint8_t *np = realloc(o->p, ncap);
    if (!np) return -1;
    o->p = np; o->cap = ncap;
    return 0;
}

static int ob_put(struct obuf *o, const void *src, size_t n) {
    if (ob_reserve(o, n)) return -1;
    memcpy(o->p + o->len, src, n);
    o->len += n;
    return 0;
}

static int ob_byte(struct obuf *o, uint8_t b) { return ob_put(o, &b, 1); }

/* ------------------------------------------------------------ range coder */
#define RC_TOP        (1u << 24)
#define PROB_BITS     11
#define PROB_INIT     (1u << (PROB_BITS - 1))
#define MOVE_BITS     5

struct rc {
    const uint8_t *p, *end;
    uint32_t range, code;
    int err;
};

static uint8_t rc_byte(struct rc *r) {
    if (r->p < r->end) return *r->p++;
    r->err = 1;
    return 0;
}

static void rc_init(struct rc *r, const uint8_t *p, const uint8_t *end) {
    r->p = p; r->end = end; r->range = 0xFFFFFFFFu; r->code = 0; r->err = 0;
    if (end - p < 5) { r->err = 1; return; }
    if (rc_byte(r) != 0) r->err = 1;
    for (int i = 0; i < 4; i++) r->code = (r->code << 8) | rc_byte(r);
    if (r->code == r->range) r->err = 1;
}

static inline void rc_normalize(struct rc *r) {
    if (r->range < RC_TOP) {
        r->range <<= 8;
        r->code = (r->code << 8) | rc_byte(r);
    }
}

static inline unsigned rc_bit(struct rc *r, uint16_t *prob) {
    unsigned v = *prob;
    uint32_t bound = (r->range >> PROB_BITS) * v;
    unsigned sym;
    if (r->code < bound) {
        v += ((1u << PROB_BITS) - v) >> MOVE_BITS;
        r->range = bound;
        sym = 0;
    } else {
        v -= v >> MOVE_BITS;
        r->code -= bound;
        r->range -= bound;
        sym = 1;
    }
    *prob = (uint16_t)v;
    rc_normalize(r);
    return sym;
}

static uint32_t rc_direct(struct rc *r, unsigned nbits) {
    uint32_t res = 0;
    do {
        r->range >>= 1;
        r->code -= r->range;
        uint32_t t = 0u - (r->code >> 31);
        r->code += r->range & t;
        if (r->code == r->range) r->err = 1;
        rc_normalize(r);
        res = (res << 1) + (t + 1);
    } while (--nbits);
    return res;
}

static unsigned bittree(struct rc *r, uint16_t *probs, unsigned nbits) {
    unsigned m = 1;
    for (unsigned i = 0; i < nbits; i++) m = (m << 1) + rc_bit(r, &probs[m]);
    return m - (1u << nbits);
}

static unsigned bittree_rev(struct rc *r, uint16_t *probs, unsigned nbits) {
    unsigned m = 1, sym = 0;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = rc_bit(r, &probs[m]);
        m = (m << 1) + bit;
        sym |= bit << i;
    }
    return sym;
}

/* ------------------------------------------------------------- LZMA state */
#define NUM_STATES        12
#define NUM_POS_BITS_MAX  4
#define NUM_LEN_TO_POS    4
#define END_POS_MODEL     14
#define NUM_FULL_DIST     128
#define NUM_ALIGN_BITS    4
#define MATCH_MIN_LEN     2

struct lendec {
    uint16_t choice, choice2;
    uint16_t low[1 << NUM_POS_BITS_MAX][1 << 3];
    uint16_t mid[1 << NUM_POS_BITS_MAX][1 << 3];
    uint16_t high[1 << 8];
};

static void lendec_init(struct lendec *d) {
    uint16_t *p = (uint16_t *)d;
    for (size_t i = 0; i < sizeof(*d) / sizeof(uint16_t); i++) p[i] = PROB_INIT;
}

static unsigned lendec_decode(struct lendec *d, struct rc *r, unsigned pos_state) {
    if (rc_bit(r, &d->choice) == 0) return bittree(r, d->low[pos_state], 3);
    if (rc_bit(r, &d->choice2) == 0) return 8 + bittree(r, d->mid[pos_state], 3);
    return 16 + bittree(r, d->high, 8);
}

struct lzma {
    unsigned lc, lp, pb;
    uint16_t *lit;                 /* 0x300 << (lc + lp) */
    size_t    lit_count;
    uint16_t is_match[NUM_STATES << NUM_POS_BITS_MAX];
    uint16_t is_rep[NUM_STATES];
    uint16_t is_rep_g0[NUM_STATES];
    uint16_t is_rep_g1[NUM_STATES];
    uint16_t is_rep_g2[NUM_STATES];
    uint16_t is_rep0_long[NUM_STATES << NUM_POS_BITS_MAX];
    uint16_t pos_slot[NUM_LEN_TO_POS][1 << 6];
    uint16_t spec_pos[1 + NUM_FULL_DIST - END_POS_MODEL];
    uint16_t align[1 << NUM_ALIGN_BITS];
    struct lendec len_dec, rep_len_dec;
    unsigned state;
    uint32_t rep0, rep1, rep2, rep3;
};

static int lzma_set_props(struct lzma *z, unsigned props) {
    if (props >= 9 * 5 * 5) return -1;
    unsigned lc = props % 9; props /= 9;
    unsigned lp = props % 5; unsigned pb = props / 5;
    if (lc + lp > 4 || pb > 4) return -1;   /* LZMA2 restriction */
    z->lc = lc; z->lp = lp; z->pb = pb;
    size_t n = (size_t)0x300 << (lc + lp);
    if (n != z->lit_count) {
        uint16_t *nl = realloc(z->lit, n * sizeof(uint16_t));
        if (!nl) return -1;
        z->lit = nl; z->lit_count = n;
    }
    return 0;
}

static void lzma_reset_state(struct lzma *z) {
    for (size_t i = 0; i < z->lit_count; i++) z->lit[i] = PROB_INIT;
#define INIT_ARR(a) do { for (size_t i_ = 0; i_ < sizeof(a) / sizeof((a)[0]); i_++) ((uint16_t *)(a))[i_] = PROB_INIT; } while (0)
    INIT_ARR(z->is_match); INIT_ARR(z->is_rep); INIT_ARR(z->is_rep_g0);
    INIT_ARR(z->is_rep_g1); INIT_ARR(z->is_rep_g2); INIT_ARR(z->is_rep0_long);
    INIT_ARR(z->spec_pos); INIT_ARR(z->align);
    for (int i = 0; i < NUM_LEN_TO_POS; i++)
        for (int k = 0; k < 64; k++) z->pos_slot[i][k] = PROB_INIT;
#undef INIT_ARR
    lendec_init(&z->len_dec);
    lendec_init(&z->rep_len_dec);
    z->state = 0;
    z->rep0 = z->rep1 = z->rep2 = z->rep3 = 0;
}

static uint32_t lzma_decode_distance(struct lzma *z, struct rc *r, unsigned len) {
    unsigned len_state = len < NUM_LEN_TO_POS - 1 ? len : NUM_LEN_TO_POS - 1;
    unsigned pos_slot = bittree(r, z->pos_slot[len_state], 6);
    if (pos_slot < 4) return pos_slot;
    unsigned nbits = (pos_slot >> 1) - 1;
    uint32_t dist = (2 | (pos_slot & 1)) << nbits;
    if (pos_slot < END_POS_MODEL)
        dist += bittree_rev(r, z->spec_pos + dist - pos_slot, nbits);
    else {
        dist += rc_direct(r, nbits - NUM_ALIGN_BITS) << NUM_ALIGN_BITS;
        dist += bittree_rev(r, z->align, NUM_ALIGN_BITS);
    }
    return dist;
}

/* Decode exactly `unpacked` bytes of one LZMA2 chunk into o (the whole
 * output so far is the dictionary; dict_start is the position of the last
 * dictionary reset, below which distances are invalid). */
static int lzma_decode_chunk(struct lzma *z, struct rc *r, struct obuf *o,
                             size_t unpacked, size_t dict_start, char *err, size_t errcap) {
    size_t end = o->len + unpacked;
    if (ob_reserve(o, unpacked)) { snprintf(err, errcap, "out of memory"); return -1; }
    uint32_t pb_mask = (1u << z->pb) - 1;
    uint32_t lp_mask = (1u << z->lp) - 1;
    while (o->len < end) {
        if (r->err) { snprintf(err, errcap, "corrupt LZMA data"); return -1; }
        size_t pos = o->len;
        unsigned pos_state = (unsigned)(pos & pb_mask);
        unsigned st = z->state;
        if (rc_bit(r, &z->is_match[(st << NUM_POS_BITS_MAX) + pos_state]) == 0) {
            /* literal */
            unsigned prev = pos > dict_start ? o->p[pos - 1] : 0;
            uint16_t *probs = z->lit + 0x300 * ((((uint32_t)pos & lp_mask) << z->lc) + (prev >> (8 - z->lc)));
            unsigned sym = 1;
            if (st >= 7) {
                if (pos < dict_start + z->rep0 + 1) { snprintf(err, errcap, "corrupt LZMA data (literal distance)"); return -1; }
                unsigned match_byte = o->p[pos - z->rep0 - 1];
                do {
                    unsigned match_bit = (match_byte >> 7) & 1;
                    match_byte <<= 1;
                    unsigned bit = rc_bit(r, &probs[((1 + match_bit) << 8) + sym]);
                    sym = (sym << 1) | bit;
                    if (match_bit != bit) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | rc_bit(r, &probs[sym]);
            o->p[o->len++] = (uint8_t)sym;
            z->state = st < 4 ? 0 : (st < 10 ? st - 3 : st - 6);
            continue;
        }
        unsigned len;
        if (rc_bit(r, &z->is_rep[st])) {
            if (pos == dict_start) { snprintf(err, errcap, "corrupt LZMA data (rep at start)"); return -1; }
            if (rc_bit(r, &z->is_rep_g0[st]) == 0) {
                if (rc_bit(r, &z->is_rep0_long[(st << NUM_POS_BITS_MAX) + pos_state]) == 0) {
                    /* short rep: one byte at rep0 */
                    z->state = st < 7 ? 9 : 11;
                    if (pos < dict_start + z->rep0 + 1) { snprintf(err, errcap, "corrupt LZMA data (shortrep)"); return -1; }
                    o->p[o->len] = o->p[o->len - z->rep0 - 1];
                    o->len++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (rc_bit(r, &z->is_rep_g1[st]) == 0) dist = z->rep1;
                else {
                    if (rc_bit(r, &z->is_rep_g2[st]) == 0) dist = z->rep2;
                    else { dist = z->rep3; z->rep3 = z->rep2; }
                    z->rep2 = z->rep1;
                }
                z->rep1 = z->rep0;
                z->rep0 = dist;
            }
            len = lendec_decode(&z->rep_len_dec, r, pos_state);
            z->state = st < 7 ? 8 : 11;
        } else {
            z->rep3 = z->rep2; z->rep2 = z->rep1; z->rep1 = z->rep0;
            len = lendec_decode(&z->len_dec, r, pos_state);
            z->state = st < 7 ? 7 : 10;
            z->rep0 = lzma_decode_distance(z, r, len);
            if (z->rep0 == 0xFFFFFFFFu) {
                /* end marker: LZMA2 chunks never need one, but it is legal */
                if (r->err) { snprintf(err, errcap, "corrupt LZMA data"); return -1; }
                snprintf(err, errcap, "unexpected LZMA end marker");
                return -1;
            }
            if ((size_t)z->rep0 >= pos - dict_start) { snprintf(err, errcap, "corrupt LZMA data (distance)"); return -1; }
        }
        len += MATCH_MIN_LEN;
        if (o->len + len > end) { snprintf(err, errcap, "corrupt LZMA data (chunk overrun)"); return -1; }
        size_t src = o->len - z->rep0 - 1;
        for (unsigned i = 0; i < len; i++) o->p[o->len + i] = o->p[src + i];
        o->len += len;
    }
    return 0;
}

/* ------------------------------------------------------------------ LZMA2 */
static int lzma2_decode(const uint8_t *p, const uint8_t *end, struct obuf *o,
                        const uint8_t **next, char *err, size_t errcap) {
    struct lzma z;
    memset(&z, 0, sizeof z);
    int need_props = 1, need_reset = 1;
    size_t dict_start = o->len;
    int rc = -1;
    for (;;) {
        if (p >= end) { snprintf(err, errcap, "truncated LZMA2 data"); goto out; }
        unsigned c = *p++;
        if (c == 0x00) { rc = 0; break; }
        if (c == 0x01 || c == 0x02) {
            if (end - p < 2) { snprintf(err, errcap, "truncated LZMA2 chunk"); goto out; }
            size_t n = ((size_t)p[0] << 8 | p[1]) + 1;
            p += 2;
            if ((size_t)(end - p) < n) { snprintf(err, errcap, "truncated LZMA2 chunk"); goto out; }
            if (c == 0x01) dict_start = o->len;
            else if (need_reset && dict_start == o->len && o->len == 0) { /* fine: first chunk */ }
            if (ob_put(o, p, n)) { snprintf(err, errcap, "out of memory"); goto out; }
            p += n;
            need_reset = need_reset && 0;   /* an uncompressed chunk does not reset LZMA state */
            continue;
        }
        if (c < 0x80) { snprintf(err, errcap, "bad LZMA2 control byte 0x%02x", c); goto out; }
        if (end - p < 4) { snprintf(err, errcap, "truncated LZMA2 chunk header"); goto out; }
        size_t unpacked = (((size_t)c & 0x1F) << 16 | (size_t)p[0] << 8 | p[1]) + 1;
        size_t packed = ((size_t)p[2] << 8 | p[3]) + 1;
        p += 4;
        unsigned mode = (c >> 5) & 3;
        if (mode == 3) dict_start = o->len;
        if (mode >= 2) {
            if (p >= end) { snprintf(err, errcap, "truncated LZMA2 props"); goto out; }
            if (lzma_set_props(&z, *p++)) { snprintf(err, errcap, "bad LZMA2 properties"); goto out; }
            need_props = 0;
        } else if (need_props) { snprintf(err, errcap, "LZMA2 chunk without properties"); goto out; }
        if (mode >= 1) { lzma_reset_state(&z); need_reset = 0; }
        else if (need_reset) { snprintf(err, errcap, "LZMA2 chunk without state reset"); goto out; }
        if ((size_t)(end - p) < packed) { snprintf(err, errcap, "truncated LZMA2 chunk data"); goto out; }
        struct rc r;
        rc_init(&r, p, p + packed);
        if (lzma_decode_chunk(&z, &r, o, unpacked, dict_start, err, errcap)) goto out;
        if (r.err || r.code != 0) { snprintf(err, errcap, "corrupt LZMA2 chunk"); goto out; }
        p += packed;
    }
out:
    free(z.lit);
    *next = p;
    return rc;
}

/* ------------------------------------------------------------- .xz parser */
static int rd_vli(const uint8_t **pp, const uint8_t *end, uint64_t *v) {
    const uint8_t *p = *pp;
    uint64_t r = 0;
    for (int i = 0; i < 9; i++) {
        if (p >= end) return -1;
        uint8_t b = *p++;
        r |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *pp = p; *v = r; return 0; }
    }
    return -1;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static const uint8_t XZ_MAGIC[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };

int toby_xz_is_xz(const uint8_t *p, size_t n) {
    return n >= 6 && memcmp(p, XZ_MAGIC, 6) == 0;
}

static size_t check_size(unsigned type) {
    if (type == 0) return 0;
    if (type <= 3) return 4;
    if (type <= 6) return 8;
    if (type <= 9) return 16;
    if (type <= 12) return 32;
    return 64;
}

int toby_xz_decode(const uint8_t *in, size_t in_len,
                   uint8_t **out, size_t *out_len, char *err, size_t errcap) {
    struct obuf o = { 0, 0, 0 };
    const uint8_t *p = in, *end = in + in_len;
    if (errcap) err[0] = '\0';
    if (in_len == 0) { snprintf(err, errcap, "empty input"); return -1; }
    int nstreams = 0;
    for (;;) {
        /* stream padding between streams: 4-byte groups of zeros */
        while (end - p >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) p += 4;
        if (p == end) break;
        if (end - p < 12 || memcmp(p, XZ_MAGIC, 6) != 0) {
            snprintf(err, errcap, nstreams ? "garbage after stream" : "not an xz file"); goto fail;
        }
        if (p[6] != 0 || (p[7] & 0xF0)) { snprintf(err, errcap, "unsupported stream flags"); goto fail; }
        unsigned check = p[7];
        if (toby_xz_crc32(0, p + 6, 2) != rd32(p + 8)) { snprintf(err, errcap, "stream header CRC mismatch"); goto fail; }
        p += 12;
        size_t csz = check_size(check);
        /* blocks */
        uint64_t nblocks = 0;
        for (;;) {
            if (p >= end) { snprintf(err, errcap, "truncated stream"); goto fail; }
            if (*p == 0x00) break;                   /* index indicator */
            size_t hsz = ((size_t)*p + 1) * 4;
            if ((size_t)(end - p) < hsz) { snprintf(err, errcap, "truncated block header"); goto fail; }
            if (toby_xz_crc32(0, p, hsz - 4) != rd32(p + hsz - 4)) { snprintf(err, errcap, "block header CRC mismatch"); goto fail; }
            const uint8_t *h = p + 1, *hend = p + hsz - 4;
            unsigned flags = *h++;
            unsigned nfilters = (flags & 3) + 1;
            uint64_t comp_size = 0, uncomp_size = 0;
            int have_comp = flags & 0x40, have_uncomp = flags & 0x80;
            if (flags & 0x3C) { snprintf(err, errcap, "reserved block flags"); goto fail; }
            if (have_comp && rd_vli(&h, hend, &comp_size)) { snprintf(err, errcap, "bad block header"); goto fail; }
            if (have_uncomp && rd_vli(&h, hend, &uncomp_size)) { snprintf(err, errcap, "bad block header"); goto fail; }
            int lzma2_ok = 0;
            for (unsigned f = 0; f < nfilters; f++) {
                uint64_t id, psz;
                if (rd_vli(&h, hend, &id) || rd_vli(&h, hend, &psz)) { snprintf(err, errcap, "bad filter flags"); goto fail; }
                if ((uint64_t)(hend - h) < psz) { snprintf(err, errcap, "bad filter flags"); goto fail; }
                if (id == 0x21 && psz == 1 && f == nfilters - 1) lzma2_ok = 1;
                else { snprintf(err, errcap, "unsupported filter 0x%x (only LZMA2)", (unsigned)id); goto fail; }
                h += psz;
            }
            if (!lzma2_ok) { snprintf(err, errcap, "block has no LZMA2 filter"); goto fail; }
            while (h < hend) if (*h++ != 0) { snprintf(err, errcap, "bad block header padding"); goto fail; }
            p += hsz;
            size_t before = o.len;
            const uint8_t *next;
            const uint8_t *data_end = have_comp && comp_size <= (uint64_t)(end - p) ? p + comp_size : end;
            if (lzma2_decode(p, data_end, &o, &next, err, errcap)) goto fail;
            size_t produced = o.len - before;
            if (have_uncomp && produced != uncomp_size) { snprintf(err, errcap, "uncompressed size mismatch"); goto fail; }
            if (have_comp && (size_t)(next - p) != comp_size) { snprintf(err, errcap, "compressed size mismatch"); goto fail; }
            p = next;
            while ((p - in) & 3) { if (p >= end || *p != 0) { snprintf(err, errcap, "bad block padding"); goto fail; } p++; }
            if ((size_t)(end - p) < csz) { snprintf(err, errcap, "truncated block check"); goto fail; }
            if (check == TOBY_XZ_CHECK_CRC32) {
                if (toby_xz_crc32(0, o.p + before, produced) != rd32(p)) { snprintf(err, errcap, "CRC32 mismatch"); goto fail; }
            } else if (check == TOBY_XZ_CHECK_CRC64) {
                if (toby_xz_crc64(0, o.p + before, produced) != rd64(p)) { snprintf(err, errcap, "CRC64 mismatch"); goto fail; }
            } /* SHA-256 and reserved types: accepted, not verified */
            p += csz;
            nblocks++;
        }
        /* index */
        const uint8_t *istart = p;
        p++;
        uint64_t nrec;
        if (rd_vli(&p, end, &nrec)) { snprintf(err, errcap, "bad index"); goto fail; }
        if (nrec != nblocks) { snprintf(err, errcap, "index record count mismatch"); goto fail; }
        for (uint64_t i = 0; i < nrec; i++) {
            uint64_t a, b;
            if (rd_vli(&p, end, &a) || rd_vli(&p, end, &b)) { snprintf(err, errcap, "bad index record"); goto fail; }
        }
        while ((p - in) & 3) { if (p >= end || *p != 0) { snprintf(err, errcap, "bad index padding"); goto fail; } p++; }
        if (end - p < 4 || toby_xz_crc32(0, istart, (size_t)(p - istart)) != rd32(p)) { snprintf(err, errcap, "index CRC mismatch"); goto fail; }
        p += 4;
        size_t index_size = (size_t)(p - istart);
        /* footer */
        if (end - p < 12) { snprintf(err, errcap, "truncated stream footer"); goto fail; }
        if (toby_xz_crc32(0, p + 4, 6) != rd32(p)) { snprintf(err, errcap, "stream footer CRC mismatch"); goto fail; }
        if (((size_t)rd32(p + 4) + 1) * 4 != index_size) { snprintf(err, errcap, "backward size mismatch"); goto fail; }
        if (p[8] != 0 || p[9] != check || p[10] != 'Y' || p[11] != 'Z') { snprintf(err, errcap, "bad stream footer"); goto fail; }
        p += 12;
        nstreams++;
    }
    if (!nstreams) { snprintf(err, errcap, "not an xz file"); goto fail; }
    if (!o.p) { o.p = malloc(1); if (!o.p) { snprintf(err, errcap, "out of memory"); goto fail; } }
    *out = o.p; *out_len = o.len;
    return 0;
fail:
    free(o.p);
    *out = NULL; *out_len = 0;
    return -1;
}

/* ------------------------------------------------------------ encoder */
static void wr32(struct obuf *o, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    ob_put(o, b, 4);
}
static void wr64(struct obuf *o, uint64_t v) { wr32(o, (uint32_t)v); wr32(o, (uint32_t)(v >> 32)); }
static void wr_vli(struct obuf *o, uint64_t v) {
    while (v >= 0x80) { ob_byte(o, (uint8_t)(v | 0x80)); v >>= 7; }
    ob_byte(o, (uint8_t)v);
}
static void pad4(struct obuf *o) { while (o->len & 3) ob_byte(o, 0); }

int toby_xz_encode_stored(const uint8_t *in, size_t in_len, int check,
                          uint8_t **out, size_t *out_len) {
    if (check != TOBY_XZ_CHECK_NONE && check != TOBY_XZ_CHECK_CRC32 && check != TOBY_XZ_CHECK_CRC64)
        return -1;
    struct obuf o = { 0, 0, 0 };
    if (ob_reserve(&o, in_len + in_len / 65536 * 3 + 128)) return -1;
    /* stream header */
    ob_put(&o, XZ_MAGIC, 6);
    uint8_t flags[2] = { 0, (uint8_t)check };
    ob_put(&o, flags, 2);
    wr32(&o, toby_xz_crc32(0, flags, 2));
    uint64_t unpadded = 0;
    if (in_len) {
        /* block header: 12 bytes = size(1) flags(1) filter(3) pad(3) crc(4) */
        size_t hstart = o.len;
        uint8_t hdr[8] = { 2, 0x00, 0x21, 0x01, 0x10, 0, 0, 0 };
        ob_put(&o, hdr, 8);
        wr32(&o, toby_xz_crc32(0, hdr, 8));
        /* LZMA2: uncompressed chunks, first with a dictionary reset */
        size_t off = 0;
        int first = 1;
        while (off < in_len) {
            size_t n = in_len - off; if (n > 65536) n = 65536;
            ob_byte(&o, first ? 0x01 : 0x02);
            ob_byte(&o, (uint8_t)((n - 1) >> 8));
            ob_byte(&o, (uint8_t)(n - 1));
            ob_put(&o, in + off, n);
            off += n; first = 0;
        }
        ob_byte(&o, 0x00);
        unpadded = o.len - hstart;
        pad4(&o);
        if (check == TOBY_XZ_CHECK_CRC32) { wr32(&o, toby_xz_crc32(0, in, in_len)); unpadded += 4; }
        else if (check == TOBY_XZ_CHECK_CRC64) { wr64(&o, toby_xz_crc64(0, in, in_len)); unpadded += 8; }
    }
    /* index */
    size_t istart = o.len;
    ob_byte(&o, 0x00);
    wr_vli(&o, in_len ? 1 : 0);
    if (in_len) { wr_vli(&o, unpadded); wr_vli(&o, in_len); }
    pad4(&o);
    wr32(&o, toby_xz_crc32(0, o.p + istart, o.len - istart));
    size_t index_size = o.len - istart;
    /* footer */
    uint8_t ftr[6];
    uint32_t bw = (uint32_t)(index_size / 4 - 1);
    ftr[0] = (uint8_t)bw; ftr[1] = (uint8_t)(bw >> 8); ftr[2] = (uint8_t)(bw >> 16); ftr[3] = (uint8_t)(bw >> 24);
    ftr[4] = 0; ftr[5] = (uint8_t)check;
    wr32(&o, toby_xz_crc32(0, ftr, 6));
    ob_put(&o, ftr, 6);
    ob_put(&o, "YZ", 2);
    if (!o.p) return -1;
    *out = o.p; *out_len = o.len;
    return 0;
}
