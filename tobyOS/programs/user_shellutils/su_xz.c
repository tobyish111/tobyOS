/* su_xz.c -- xz, unxz and xzcat, as XZ Utils 5.8.3's
 * (logs/utilgate/staging/xz.sh, unxz.sh, xzcat.sh).
 *
 * Decompressing: .xz (several streams, stream padding, every check: none,
 * CRC32, CRC64, SHA-256, an unknown one warned of), .lzma, lzip members
 * and raw streams; LZMA1 and LZMA2 through delta and the branch
 * converters (x86, PowerPC, IA-64, ARM, ARM-Thumb, ARM64, SPARC; not
 * RISC-V). The block index is checked against the blocks, as xz does.
 *
 * Compressing: the container is xz's, byte for byte: the threaded
 * layout by default (blocks of three dictionaries or 1 MiB, each header
 * written into the room the largest sizes would need, with the sizes
 * known after), -T1's single block without sizes, the index and footer.
 * The LZMA coder is tobyOS's own (hash chains, the repeated distances,
 * one step of lazy choice by rough bit costs): where liblzma has a choice
 * to make its bytes differ (valid; some 15% larger than xz -6's); where
 * it has none -- empty input, data LZMA2 stores as it is, runs, short
 * inputs -- they are xz's own. LZMA2's chunking is liblzma's: a chunk
 * stored when its LZMA form is no smaller, state reset after.
 *
 * -l reads a file from its end as xz does, and prints the listing at
 * -v, -vv and --robot with xz's columns, units (KiB from 10000 bytes,
 * one decimal) and memory figures (a decoder's dictionary plus 66200
 * bytes; an encoder's from liblzma's match-finder sizes, at -vv with
 * -T1: the threaded mode's lines are the machine's). The option parser
 * is xz's table, its filter options, presets and their order rules, its
 * messages and statuses (warnings 2, -Q 0, -q and -qq). --memlimit is
 * checked for decompression; for compression it is accepted. */

#include "su_czcommon.c"
#include <toby/sha2.h>

/* ======================================================================
 *  checks
 * ====================================================================== */
static uint64_t cz_x_crc64tab[256];
static int cz_x_crc64ready;

static uint64_t cz_x_crc64(uint64_t crc, const uint8_t *p, size_t n) {
    if (!cz_x_crc64ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint64_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xC96C5795D7870F42ULL ^ (c >> 1) : c >> 1;
            cz_x_crc64tab[i] = c;
        }
        cz_x_crc64ready = 1;
    }
    crc = ~crc;
    while (n--) crc = cz_x_crc64tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

#define CZ_X_CHECK_NONE 0
#define CZ_X_CHECK_CRC32 1
#define CZ_X_CHECK_CRC64 4
#define CZ_X_CHECK_SHA256 10

/* the size of each check ID's field, as the format reserves them */
static int cz_x_check_size(int id) {
    static const uint8_t sz[16] = {0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64};
    return id >= 0 && id < 16 ? sz[id] : 0;
}

struct cz_x_check {
    int id;
    uint32_t crc32;
    uint64_t crc64;
    toby_sha256_ctx sha;
};

static void cz_x_check_init(struct cz_x_check *c, int id) {
    c->id = id;
    c->crc32 = 0;
    c->crc64 = 0;
    if (id == CZ_X_CHECK_SHA256) toby_sha256_init(&c->sha, 0);
}

static void cz_x_check_update(struct cz_x_check *c, const uint8_t *p, size_t n) {
    switch (c->id) {
    case CZ_X_CHECK_CRC32: c->crc32 = cz_crc32(c->crc32, p, n); break;
    case CZ_X_CHECK_CRC64: c->crc64 = cz_x_crc64(c->crc64, p, n); break;
    case CZ_X_CHECK_SHA256: toby_sha256_update(&c->sha, p, n); break;
    }
}

/* the check's bytes as stored */
static int cz_x_check_final(struct cz_x_check *c, uint8_t *out) {
    switch (c->id) {
    case CZ_X_CHECK_CRC32:
        for (int i = 0; i < 4; i++) out[i] = (uint8_t)(c->crc32 >> (8 * i));
        return 4;
    case CZ_X_CHECK_CRC64:
        for (int i = 0; i < 8; i++) out[i] = (uint8_t)(c->crc64 >> (8 * i));
        return 8;
    case CZ_X_CHECK_SHA256:
        toby_sha256_final(&c->sha, out);
        return 32;
    }
    return 0;
}

/* ======================================================================
 *  LZMA: the model both directions share
 * ====================================================================== */
#define CZ_X_PROB_INIT 1024
#define CZ_X_NSTATES 12
#define CZ_X_POS_STATES_MAX 16
#define CZ_X_LEN_LOW 8
#define CZ_X_LEN_MID 8
#define CZ_X_LEN_HIGH 256
#define CZ_X_MATCH_MIN 2
#define CZ_X_MATCH_MAX 273
#define CZ_X_DIST_STATES 4
#define CZ_X_DIST_SLOTS 64
#define CZ_X_DIST_MODEL_START 4
#define CZ_X_DIST_MODEL_END 14
#define CZ_X_FULL_DIST 128
#define CZ_X_ALIGN_BITS 4

struct cz_x_len_model {
    uint16_t choice, choice2;
    uint16_t low[CZ_X_POS_STATES_MAX][CZ_X_LEN_LOW];
    uint16_t mid[CZ_X_POS_STATES_MAX][CZ_X_LEN_MID];
    uint16_t high[CZ_X_LEN_HIGH];
};

struct cz_x_model {
    int lc, lp, pb;
    uint16_t *literal;                 /* 0x300 << (lc + lp) */
    uint16_t is_match[CZ_X_NSTATES][CZ_X_POS_STATES_MAX];
    uint16_t is_rep[CZ_X_NSTATES], is_rep0[CZ_X_NSTATES], is_rep1[CZ_X_NSTATES], is_rep2[CZ_X_NSTATES];
    uint16_t is_rep0_long[CZ_X_NSTATES][CZ_X_POS_STATES_MAX];
    uint16_t dist_slot[CZ_X_DIST_STATES][CZ_X_DIST_SLOTS];
    uint16_t dist_special[CZ_X_FULL_DIST - CZ_X_DIST_MODEL_END];
    uint16_t dist_align[1 << CZ_X_ALIGN_BITS];
    struct cz_x_len_model len, replen;
    int state;
    uint32_t rep[4];
};

static int cz_x_model_props(struct cz_x_model *m, int lc, int lp, int pb) {
    if (m->literal && m->lc + m->lp == lc + lp) {
        m->lc = lc; m->lp = lp; m->pb = pb;
        return 0;
    }
    free(m->literal);
    m->literal = (uint16_t *)malloc((size_t)0x300 << (lc + lp) << 1);
    m->lc = lc; m->lp = lp; m->pb = pb;
    return m->literal ? 0 : -1;
}

static void cz_x_len_reset(struct cz_x_len_model *l) {
    uint16_t *p = (uint16_t *)l;
    for (size_t i = 0; i < sizeof *l / 2; i++) p[i] = CZ_X_PROB_INIT;
}

static void cz_x_model_reset(struct cz_x_model *m) {
    size_t n = (size_t)0x300 << (m->lc + m->lp);
    for (size_t i = 0; i < n; i++) m->literal[i] = CZ_X_PROB_INIT;
    uint16_t *p = &m->is_match[0][0];
    for (int i = 0; i < CZ_X_NSTATES * CZ_X_POS_STATES_MAX; i++) p[i] = CZ_X_PROB_INIT;
    for (int i = 0; i < CZ_X_NSTATES; i++)
        m->is_rep[i] = m->is_rep0[i] = m->is_rep1[i] = m->is_rep2[i] = CZ_X_PROB_INIT;
    p = &m->is_rep0_long[0][0];
    for (int i = 0; i < CZ_X_NSTATES * CZ_X_POS_STATES_MAX; i++) p[i] = CZ_X_PROB_INIT;
    p = &m->dist_slot[0][0];
    for (int i = 0; i < CZ_X_DIST_STATES * CZ_X_DIST_SLOTS; i++) p[i] = CZ_X_PROB_INIT;
    for (int i = 0; i < CZ_X_FULL_DIST - CZ_X_DIST_MODEL_END; i++) m->dist_special[i] = CZ_X_PROB_INIT;
    for (int i = 0; i < 1 << CZ_X_ALIGN_BITS; i++) m->dist_align[i] = CZ_X_PROB_INIT;
    cz_x_len_reset(&m->len);
    cz_x_len_reset(&m->replen);
    m->state = 0;
    m->rep[0] = m->rep[1] = m->rep[2] = m->rep[3] = 0;
}

static int cz_x_st_lit(int s) { return s < 4 ? 0 : s < 10 ? s - 3 : s - 6; }
static int cz_x_st_match(int s) { return s < 7 ? 7 : 10; }
static int cz_x_st_rep(int s) { return s < 7 ? 8 : 11; }
static int cz_x_st_short(int s) { return s < 7 ? 9 : 11; }

/* ======================================================================
 *  LZMA decoding, into a circular dictionary
 * ====================================================================== */
#define CZ_X_D_OK 0
#define CZ_X_D_DATA 1        /* corrupt */
#define CZ_X_D_EOF 2         /* the input ended */
#define CZ_X_D_MEM 3

struct cz_x_dict {
    uint8_t *buf;
    size_t size, pos;       /* pos: where the next byte goes */
    uint64_t full;          /* bytes ever written (for valid distances) */
    void (*out)(void *ctx, const uint8_t *p, size_t n);
    void *octx;
    size_t flushed;         /* buf[flushed..pos) not yet handed out */
};

static int cz_x_dict_init(struct cz_x_dict *d, size_t size) {
    if (size < 4096) size = 4096;
    if (!d->buf || d->size != size) {
        free(d->buf);
        d->buf = (uint8_t *)malloc(size);
        if (!d->buf) return -1;
        d->size = size;
    }
    d->pos = 0;
    d->full = 0;
    d->flushed = 0;
    return 0;
}

static void cz_x_dict_flush(struct cz_x_dict *d) {
    if (d->pos > d->flushed) d->out(d->octx, d->buf + d->flushed, d->pos - d->flushed);
    if (d->pos == d->size) d->pos = 0;
    d->flushed = d->pos;
}

static void cz_x_dict_put(struct cz_x_dict *d, uint8_t b) {
    d->buf[d->pos++] = b;
    d->full++;
    if (d->pos == d->size) cz_x_dict_flush(d);
}

static uint8_t cz_x_dict_get(struct cz_x_dict *d, uint32_t dist) {   /* dist: 0 = the last byte */
    size_t i = d->pos >= (size_t)dist + 1 ? d->pos - dist - 1 : d->pos + d->size - dist - 1;
    return d->buf[i];
}

struct cz_x_rcd {
    struct cz_in *in;
    uint32_t range, code;
    uint64_t limit;          /* bytes this chunk may read; UINT64_MAX: no limit */
    uint64_t used;
    int eof;
};

static int cz_x_rcd_byte(struct cz_x_rcd *r) {
    if (r->used >= r->limit) { r->eof = 2; return 0; }
    int c = cz_getc(r->in);
    if (c < 0) { r->eof = 1; return 0; }
    r->used++;
    return c;
}

static int cz_x_rcd_init(struct cz_x_rcd *r) {
    r->range = 0xffffffffu;
    r->code = 0;
    r->eof = 0;
    int first = cz_x_rcd_byte(r);
    for (int i = 0; i < 4; i++) r->code = r->code << 8 | (uint32_t)cz_x_rcd_byte(r);
    if (r->eof) return r->eof == 1 ? CZ_X_D_EOF : CZ_X_D_DATA;
    if (first != 0 || r->code == r->range) return CZ_X_D_DATA;
    return CZ_X_D_OK;
}

#define CZ_X_RC_NORM(r) do { if ((r)->range < (1u << 24)) { (r)->range <<= 8; \
    (r)->code = (r)->code << 8 | (uint32_t)cz_x_rcd_byte(r); } } while (0)

static int cz_x_rcd_bit(struct cz_x_rcd *r, uint16_t *p) {
    uint32_t bound = (r->range >> 11) * *p;
    int b;
    if (r->code < bound) {
        r->range = bound;
        *p += (2048 - *p) >> 5;
        b = 0;
    } else {
        r->range -= bound;
        r->code -= bound;
        *p -= *p >> 5;
        b = 1;
    }
    CZ_X_RC_NORM(r);
    return b;
}

static uint32_t cz_x_rcd_direct(struct cz_x_rcd *r, int n) {
    uint32_t v = 0;
    while (n--) {
        r->range >>= 1;
        uint32_t t = (r->code - r->range) >> 31;   /* 1 when code < range */
        r->code -= r->range & (t - 1);
        v = v << 1 | (1 - t);
        CZ_X_RC_NORM(r);
    }
    return v;
}

static uint32_t cz_x_rcd_tree(struct cz_x_rcd *r, uint16_t *p, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) m = m << 1 | (uint32_t)cz_x_rcd_bit(r, &p[m]);
    return m - (1u << bits);
}

static uint32_t cz_x_rcd_rtree(struct cz_x_rcd *r, uint16_t *p, int bits) {
    uint32_t m = 1, v = 0;
    for (int i = 0; i < bits; i++) {
        uint32_t b = (uint32_t)cz_x_rcd_bit(r, &p[m]);
        m = m << 1 | b;
        v |= b << i;
    }
    return v;
}

static uint32_t cz_x_rcd_len(struct cz_x_rcd *r, struct cz_x_len_model *l, int ps) {
    if (!cz_x_rcd_bit(r, &l->choice)) return cz_x_rcd_tree(r, l->low[ps], 3);
    if (!cz_x_rcd_bit(r, &l->choice2)) return 8 + cz_x_rcd_tree(r, l->mid[ps], 3);
    return 16 + cz_x_rcd_tree(r, l->high, 8);
}

/* decode until OUTSIZE bytes are out (UINT64_MAX: until the end marker);
 * *ENDMARK is set when the end marker was seen */
static int cz_x_lzma_decode(struct cz_x_model *m, struct cz_x_rcd *r, struct cz_x_dict *d,
                          uint64_t outsize, int allow_eopm, int *endmark) {
    uint64_t done = 0;
    uint32_t pbmask = (1u << m->pb) - 1, lpmask = (1u << m->lp) - 1;
    *endmark = 0;
    while (done < outsize) {
        int ps = (int)(d->full & pbmask);
        int st = m->state;
        if (r->eof) return r->eof == 1 ? CZ_X_D_EOF : CZ_X_D_DATA;
        if (!cz_x_rcd_bit(r, &m->is_match[st][ps])) {
            /* a literal */
            uint8_t prev = d->full ? cz_x_dict_get(d, 0) : 0;
            uint16_t *probs = m->literal + 0x300 * (((d->full & lpmask) << m->lc) + (prev >> (8 - m->lc)));
            uint32_t sym = 1;
            if (st >= 7) {
                if (m->rep[0] >= d->full) return CZ_X_D_DATA;
                uint32_t mb = cz_x_dict_get(d, m->rep[0]);
                do {
                    uint32_t mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    uint32_t bit = (uint32_t)cz_x_rcd_bit(r, &probs[0x100 + (mbit << 8) + sym]);
                    sym = sym << 1 | bit;
                    if (mbit != bit) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = sym << 1 | (uint32_t)cz_x_rcd_bit(r, &probs[sym]);
            cz_x_dict_put(d, (uint8_t)sym);
            done++;
            m->state = cz_x_st_lit(st);
            continue;
        }
        uint32_t len;
        if (cz_x_rcd_bit(r, &m->is_rep[st])) {
            if (d->full == 0) return CZ_X_D_DATA;
            if (!cz_x_rcd_bit(r, &m->is_rep0[st])) {
                if (!cz_x_rcd_bit(r, &m->is_rep0_long[st][ps])) {
                    /* a short rep: one byte at rep0 */
                    if (m->rep[0] >= d->full) return CZ_X_D_DATA;
                    cz_x_dict_put(d, cz_x_dict_get(d, m->rep[0]));
                    done++;
                    m->state = cz_x_st_short(st);
                    continue;
                }
            } else {
                uint32_t dist;
                if (!cz_x_rcd_bit(r, &m->is_rep1[st])) {
                    dist = m->rep[1];
                } else {
                    if (!cz_x_rcd_bit(r, &m->is_rep2[st])) dist = m->rep[2];
                    else { dist = m->rep[3]; m->rep[3] = m->rep[2]; }
                    m->rep[2] = m->rep[1];
                }
                m->rep[1] = m->rep[0];
                m->rep[0] = dist;
            }
            len = cz_x_rcd_len(r, &m->replen, ps);
            m->state = cz_x_st_rep(st);
        } else {
            m->rep[3] = m->rep[2];
            m->rep[2] = m->rep[1];
            m->rep[1] = m->rep[0];
            len = cz_x_rcd_len(r, &m->len, ps);
            m->state = cz_x_st_match(st);
            uint32_t ls = len < CZ_X_DIST_STATES ? len : CZ_X_DIST_STATES - 1;
            uint32_t slot = cz_x_rcd_tree(r, m->dist_slot[ls], 6);
            uint32_t dist;
            if (slot < CZ_X_DIST_MODEL_START) dist = slot;
            else {
                int nb = (int)(slot >> 1) - 1;
                dist = (2 | (slot & 1)) << nb;
                if (slot < CZ_X_DIST_MODEL_END) {
                    dist += cz_x_rcd_rtree(r, m->dist_special + dist - slot - 1, nb);
                } else {
                    dist += cz_x_rcd_direct(r, nb - CZ_X_ALIGN_BITS) << CZ_X_ALIGN_BITS;
                    dist += cz_x_rcd_rtree(r, m->dist_align, CZ_X_ALIGN_BITS);
                }
            }
            if (dist == 0xffffffffu) {
                if (r->eof) return r->eof == 1 ? CZ_X_D_EOF : CZ_X_D_DATA;
                if (!allow_eopm) return CZ_X_D_DATA;
                *endmark = 1;
                return CZ_X_D_OK;
            }
            m->rep[0] = dist;
        }
        if (r->eof) return r->eof == 1 ? CZ_X_D_EOF : CZ_X_D_DATA;
        len += CZ_X_MATCH_MIN;
        if (m->rep[0] >= d->full || m->rep[0] >= d->size) return CZ_X_D_DATA;
        if (outsize - done < len) return CZ_X_D_DATA;
        for (uint32_t i = 0; i < len; i++) cz_x_dict_put(d, cz_x_dict_get(d, m->rep[0]));
        done += len;
    }
    if (r->eof) return r->eof == 1 ? CZ_X_D_EOF : CZ_X_D_DATA;
    return CZ_X_D_OK;
}

/* ======================================================================
 *  LZMA encoding: the range coder, and a parser of tobyOS's own (hash
 *  chains, the four repeated distances tried first, one step of lazy
 *  evaluation). Its choices are not liblzma's, so only what has no choice
 *  to make (no matches at all, or data stored as it is) comes out as xz's
 *  own bytes; everything else is a valid stream of other bytes.
 * ====================================================================== */
struct cz_x_rce {
    uint64_t low;
    uint32_t range;
    uint8_t cache;
    uint64_t cache_size;
    uint8_t *out;
    size_t on, ocap;
};

static void cz_x_rce_reset(struct cz_x_rce *e) {
    e->low = 0;
    e->range = 0xffffffffu;
    e->cache = 0;
    e->cache_size = 1;
    e->on = 0;
}

static void cz_x_rce_byte(struct cz_x_rce *e, uint8_t b) {
    if (e->on == e->ocap) {
        e->ocap = e->ocap ? e->ocap * 2 : 65536 + 1024;
        e->out = (uint8_t *)realloc(e->out, e->ocap);
        if (!e->out) { fprintf(stderr, "xz: Memory allocation failed\n"); exit(1); }
    }
    e->out[e->on++] = b;
}

static void cz_x_rce_shift(struct cz_x_rce *e) {
    if ((uint32_t)e->low < 0xff000000u || (e->low >> 32) != 0) {
        uint8_t t = e->cache;
        do {
            cz_x_rce_byte(e, (uint8_t)(t + (uint8_t)(e->low >> 32)));
            t = 0xff;
        } while (--e->cache_size != 0);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low & 0x00ffffffu) << 8;
}

static void cz_x_rce_bit(struct cz_x_rce *e, uint16_t *p, int b) {
    uint32_t bound = (e->range >> 11) * *p;
    if (!b) {
        e->range = bound;
        *p += (2048 - *p) >> 5;
    } else {
        e->low += bound;
        e->range -= bound;
        *p -= *p >> 5;
    }
    while (e->range < (1u << 24)) {
        e->range <<= 8;
        cz_x_rce_shift(e);
    }
}

static void cz_x_rce_direct(struct cz_x_rce *e, uint32_t v, int n) {
    while (n--) {
        e->range >>= 1;
        e->low += e->range & (0u - ((v >> n) & 1));
        while (e->range < (1u << 24)) {
            e->range <<= 8;
            cz_x_rce_shift(e);
        }
    }
}

static void cz_x_rce_flush(struct cz_x_rce *e) {
    for (int i = 0; i < 5; i++) cz_x_rce_shift(e);
}

/* bytes the chunk would have if it ended now */
static size_t cz_x_rce_pending(const struct cz_x_rce *e) {
    return e->on + (size_t)e->cache_size + 4;
}

static void cz_x_rce_tree(struct cz_x_rce *e, uint16_t *p, int bits, uint32_t v) {
    uint32_t m = 1;
    for (int i = bits - 1; i >= 0; i--) {
        int b = (int)((v >> i) & 1);
        cz_x_rce_bit(e, &p[m], b);
        m = m << 1 | (uint32_t)b;
    }
}

static void cz_x_rce_rtree(struct cz_x_rce *e, uint16_t *p, int bits, uint32_t v) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) {
        int b = (int)(v & 1);
        v >>= 1;
        cz_x_rce_bit(e, &p[m], b);
        m = m << 1 | (uint32_t)b;
    }
}

static void cz_x_rce_len(struct cz_x_rce *e, struct cz_x_len_model *l, uint32_t len, int ps) {
    len -= CZ_X_MATCH_MIN;
    if (len < 8) {
        cz_x_rce_bit(e, &l->choice, 0);
        cz_x_rce_tree(e, l->low[ps], 3, len);
    } else if (len < 16) {
        cz_x_rce_bit(e, &l->choice, 1);
        cz_x_rce_bit(e, &l->choice2, 0);
        cz_x_rce_tree(e, l->mid[ps], 3, len - 8);
    } else {
        cz_x_rce_bit(e, &l->choice, 1);
        cz_x_rce_bit(e, &l->choice2, 1);
        cz_x_rce_tree(e, l->high, 8, len - 16);
    }
}

/* the encoder: the model, the coder, and the input window with its hash
 * chains */
#define CZ_X_HASH_BITS 18
struct cz_x_lzenc {
    struct cz_x_model m;
    struct cz_x_rce rc;
    uint8_t *win;              /* win[0] is absolute position base */
    size_t wcap, wlen;         /* bytes held */
    uint64_t base;             /* absolute position of win[0] */
    size_t cur;                /* the next position to code, in win */
    size_t history;            /* how far back matches may reach */
    int32_t *head;             /* CZ_X_HASH_BITS: position+1 in win, 0 none */
    int32_t *chain;            /* per position in win */
    int32_t *head2;            /* two-byte matches: the last position */
    int depth, nice;
    int lc, lp, pb;
    int fast;                  /* the fast mode's choices (presets 0-3) */
};

static int cz_x_lzenc_init(struct cz_x_lzenc *z, size_t history, int depth, int nice, int lc, int lp, int pb) {
    memset(z, 0, sizeof *z);
    z->history = history;
    z->wcap = history + (1u << 20) + 4096;
    z->win = (uint8_t *)malloc(z->wcap);
    z->chain = (int32_t *)malloc(z->wcap * sizeof(int32_t));
    z->head = (int32_t *)calloc((size_t)1 << CZ_X_HASH_BITS, sizeof(int32_t));
    z->head2 = (int32_t *)calloc(65536, sizeof(int32_t));
    if (!z->win || !z->chain || !z->head || !z->head2) return -1;
    z->depth = depth;
    z->nice = nice;
    z->lc = lc; z->lp = lp; z->pb = pb;
    if (cz_x_model_props(&z->m, lc, lp, pb) != 0) return -1;
    cz_x_model_reset(&z->m);
    cz_x_rce_reset(&z->rc);
    return 0;
}

static void cz_x_lzenc_free(struct cz_x_lzenc *z) {
    free(z->win); free(z->chain); free(z->head); free(z->head2);
    free(z->m.literal); free(z->rc.out);
}

static uint32_t cz_x_hash3(const uint8_t *p) {
    uint32_t h = ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u;
    return h >> (32 - CZ_X_HASH_BITS);
}

/* make room: keep the last HISTORY bytes before cur */
static void cz_x_lzenc_slide(struct cz_x_lzenc *z) {
    if (z->cur <= z->history) return;
    size_t drop = z->cur - z->history;
    memmove(z->win, z->win + drop, z->wlen - drop);
    memmove(z->chain, z->chain + drop, (z->wlen - drop) * sizeof(int32_t));
    z->wlen -= drop;
    z->cur -= drop;
    z->base += drop;
    for (size_t i = 0; i < z->wlen; i++) {
        int32_t v = z->chain[i];
        z->chain[i] = v > (int32_t)drop ? v - (int32_t)drop : 0;
    }
    for (size_t i = 0; i < ((size_t)1 << CZ_X_HASH_BITS); i++) {
        int32_t v = z->head[i];
        z->head[i] = v > (int32_t)drop ? v - (int32_t)drop : 0;
    }
    for (size_t i = 0; i < 65536; i++) {
        int32_t v = z->head2[i];
        z->head2[i] = v > (int32_t)drop ? v - (int32_t)drop : 0;
    }
}

static void cz_x_lzenc_insert(struct cz_x_lzenc *z, size_t pos) {
    if (pos + 3 > z->wlen) return;
    uint32_t h = cz_x_hash3(z->win + pos);
    z->chain[pos] = z->head[h];
    z->head[h] = (int32_t)pos + 1;
    z->head2[z->win[pos] | (uint32_t)z->win[pos + 1] << 8] = (int32_t)pos + 1;
}

static uint32_t cz_x_mlen(const uint8_t *a, const uint8_t *b, uint32_t max) {
    uint32_t n = 0;
    while (n < max && a[n] == b[n]) n++;
    return n;
}

/* the longest match at POS (before inserting POS); *DIST = distance-1 */
static uint32_t cz_x_lzenc_find(struct cz_x_lzenc *z, size_t pos, uint32_t avail, uint32_t *dist) {
    uint32_t best = 0;
    *dist = 0;
    if (avail < 2) return 0;
    uint32_t maxlen = avail < CZ_X_MATCH_MAX ? avail : CZ_X_MATCH_MAX;
    if (avail >= 3) {
        int32_t cand = z->head[cz_x_hash3(z->win + pos)];
        int d = z->depth;
        while (cand > 0 && d-- > 0) {
            size_t c = (size_t)cand - 1;
            if (pos - c > z->history) break;
            if (z->win[c + best] == z->win[pos + best]) {
                uint32_t l = cz_x_mlen(z->win + c, z->win + pos, maxlen);
                if (l > best) {
                    best = l;
                    *dist = (uint32_t)(pos - c - 1);
                    if (l >= (uint32_t)z->nice || l == maxlen) break;
                }
            }
            cand = z->chain[c];
        }
    }
    if (best < 2) {
        int32_t c2 = z->head2[z->win[pos] | (uint32_t)z->win[pos + 1] << 8];
        if (c2 > 0) {
            size_t c = (size_t)c2 - 1;
            if (pos - c - 1 < 64) {
                best = 2;
                *dist = (uint32_t)(pos - c - 1);
            }
        }
    }
    return best;
}

static void cz_x_enc_literal(struct cz_x_lzenc *z, size_t pos) {
    struct cz_x_model *m = &z->m;
    uint64_t apos = z->base + pos;
    int ps = (int)(apos & ((1u << m->pb) - 1));
    cz_x_rce_bit(&z->rc, &m->is_match[m->state][ps], 0);
    uint8_t prev = apos ? z->win[pos - 1] : 0;
    uint16_t *probs = m->literal + 0x300 * ((((uint32_t)apos & ((1u << m->lp) - 1)) << m->lc) + (prev >> (8 - m->lc)));
    uint32_t sym = z->win[pos] | 0x100;
    if (m->state >= 7) {
        /* after a match: the byte at rep0 steers the first bits */
        uint32_t mb = z->win[pos - m->rep[0] - 1];
        uint32_t offs = 0x100;
        do {
            mb <<= 1;
            cz_x_rce_bit(&z->rc, &probs[offs + (mb & offs) + (sym >> 8)], (int)((sym >> 7) & 1));
            sym <<= 1;
            offs &= ~(mb ^ sym);
        } while (sym < 0x10000);
    } else {
        do {
            cz_x_rce_bit(&z->rc, &probs[sym >> 8], (int)((sym >> 7) & 1));
            sym <<= 1;
        } while (sym < 0x10000);
    }
    m->state = cz_x_st_lit(m->state);
}

static void cz_x_enc_match(struct cz_x_lzenc *z, size_t pos, uint32_t dist, uint32_t len) {
    struct cz_x_model *m = &z->m;
    int ps = (int)((z->base + pos) & ((1u << m->pb) - 1));
    cz_x_rce_bit(&z->rc, &m->is_match[m->state][ps], 1);
    cz_x_rce_bit(&z->rc, &m->is_rep[m->state], 0);
    cz_x_rce_len(&z->rc, &m->len, len, ps);
    uint32_t ls = len - CZ_X_MATCH_MIN < CZ_X_DIST_STATES ? len - CZ_X_MATCH_MIN : CZ_X_DIST_STATES - 1;
    uint32_t slot;
    if (dist < CZ_X_DIST_MODEL_START) slot = dist;
    else {
        int top = 31 - __builtin_clz(dist);
        slot = (uint32_t)(top * 2) + ((dist >> (top - 1)) & 1);
    }
    cz_x_rce_tree(&z->rc, m->dist_slot[ls], 6, slot);
    if (slot >= CZ_X_DIST_MODEL_START) {
        int nb = (int)(slot >> 1) - 1;
        uint32_t basev = (2 | (slot & 1)) << nb;
        uint32_t rem = dist - basev;
        if (slot < CZ_X_DIST_MODEL_END) {
            cz_x_rce_rtree(&z->rc, m->dist_special + basev - slot - 1, nb, rem);
        } else {
            cz_x_rce_direct(&z->rc, rem >> CZ_X_ALIGN_BITS, nb - CZ_X_ALIGN_BITS);
            cz_x_rce_rtree(&z->rc, m->dist_align, CZ_X_ALIGN_BITS, rem & 15);
        }
    }
    m->rep[3] = m->rep[2];
    m->rep[2] = m->rep[1];
    m->rep[1] = m->rep[0];
    m->rep[0] = dist;
    m->state = cz_x_st_match(m->state);
}

static void cz_x_enc_rep(struct cz_x_lzenc *z, size_t pos, int which, uint32_t len) {
    struct cz_x_model *m = &z->m;
    int ps = (int)((z->base + pos) & ((1u << m->pb) - 1));
    cz_x_rce_bit(&z->rc, &m->is_match[m->state][ps], 1);
    cz_x_rce_bit(&z->rc, &m->is_rep[m->state], 1);
    if (which == 0) {
        cz_x_rce_bit(&z->rc, &m->is_rep0[m->state], 0);
        cz_x_rce_bit(&z->rc, &m->is_rep0_long[m->state][ps], len == 1 ? 0 : 1);
        if (len == 1) {
            m->state = cz_x_st_short(m->state);
            return;
        }
    } else {
        uint32_t d = m->rep[which];
        cz_x_rce_bit(&z->rc, &m->is_rep0[m->state], 1);
        if (which == 1) cz_x_rce_bit(&z->rc, &m->is_rep1[m->state], 0);
        else {
            cz_x_rce_bit(&z->rc, &m->is_rep1[m->state], 1);
            cz_x_rce_bit(&z->rc, &m->is_rep2[m->state], which == 3);
            if (which == 3) m->rep[3] = m->rep[2];
            m->rep[2] = m->rep[1];
        }
        m->rep[1] = m->rep[0];
        m->rep[0] = d;
    }
    cz_x_rce_len(&z->rc, &m->replen, len, ps);
    m->state = cz_x_st_rep(m->state);
}

/* the end-of-payload marker (a match of distance 2^32) */
static void cz_x_enc_eopm(struct cz_x_lzenc *z, size_t pos) {
    struct cz_x_model *m = &z->m;
    int ps = (int)((z->base + pos) & ((1u << m->pb) - 1));
    cz_x_rce_bit(&z->rc, &m->is_match[m->state][ps], 1);
    cz_x_rce_bit(&z->rc, &m->is_rep[m->state], 0);
    cz_x_rce_len(&z->rc, &m->len, CZ_X_MATCH_MIN, ps);
    cz_x_rce_tree(&z->rc, m->dist_slot[0], 6, 63);
    cz_x_rce_direct(&z->rc, 0x3ffffff, 26);
    cz_x_rce_rtree(&z->rc, m->dist_align, CZ_X_ALIGN_BITS, 15);
}

/* rough costs in bits, to choose between what can be coded at a place:
 * a match's length and distance, a repeated distance's index */
static int cz_x_cost_len(uint32_t len) {
    return len < 10 ? 4 : len < 18 ? 5 : 10;
}

static int cz_x_cost_dist(uint32_t dist) {
    if (dist < 4) return 6;
    int top = 31 - __builtin_clz(dist);
    return 6 + top - 1;
}

/* the best thing to code at POS: *KIND 0 literal, 1 match, 2 rep (*WHICH);
 * its length; returns its gain over literals, in bits */
static int cz_x_best_at(struct cz_x_lzenc *z, size_t pos, size_t stop, int *kind, int *which,
                      uint32_t *outlen, uint32_t *outdist) {
    uint32_t avail = (uint32_t)(stop - pos < CZ_X_MATCH_MAX ? stop - pos : CZ_X_MATCH_MAX);
    uint64_t apos = z->base + pos;
    int best = 0;
    *kind = 0;
    *which = 0;
    *outlen = 1;
    *outdist = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t d = (uint64_t)z->m.rep[i] + 1;
        if (d > apos || d > pos) continue;
        uint32_t l = cz_x_mlen(z->win + pos - d, z->win + pos, avail);
        if (l < 2) continue;
        int g = (int)l * 9 - (2 + (i == 0 ? 2 : 2 + i) + cz_x_cost_len(l));
        if (g > best) { best = g; *kind = 2; *which = i; *outlen = l; }
    }
    uint32_t dist = 0;
    uint32_t len = cz_x_lzenc_find(z, pos, avail, &dist);
    if (len == 2 && (dist >= 64 || z->fast)) len = 0;
    if (len >= 2) {
        int g = (int)len * 9 - (2 + cz_x_cost_len(len) + cz_x_cost_dist(dist));
        if (g > best) { best = g; *kind = 1; *outlen = len; *outdist = dist; }
    }
    return best;
}

/* code win[cur .. stop) (stop <= wlen); at most CHUNK_LIMIT compressed
 * bytes pending: returns how many bytes were coded */
static size_t cz_x_lzenc_run(struct cz_x_lzenc *z, size_t stop, size_t climit) {
    size_t start = z->cur;
    while (z->cur < stop) {
        if (climit && cz_x_rce_pending(&z->rc) > climit) break;
        size_t pos = z->cur;
        uint64_t apos = z->base + pos;
        int kind, which;
        uint32_t len, dist;
        int gain = cz_x_best_at(z, pos, stop, &kind, &which, &len, &dist);
        int shortrep = pos >= (size_t)z->m.rep[0] + 1 && apos >= (uint64_t)z->m.rep[0] + 1 &&
                       z->win[pos - z->m.rep[0] - 1] == z->win[pos] && (z->m.state >= 7 || !z->fast);
        if (kind != 0 && len < (uint32_t)z->nice && pos + 1 < stop) {
            /* one step of lazy evaluation: a literal now, if what comes
             * next is clearly better */
            cz_x_lzenc_insert(z, pos);
            int k2, w2;
            uint32_t l2, d2;
            int g2 = cz_x_best_at(z, pos + 1, stop, &k2, &w2, &l2, &d2);
            if (g2 > gain + 9) {
                if (shortrep) cz_x_enc_rep(z, pos, 0, 1);
                else cz_x_enc_literal(z, pos);
                z->cur = pos + 1;
                continue;
            }
            if (kind == 1) cz_x_enc_match(z, pos, dist, len);
            else cz_x_enc_rep(z, pos, which, len);
            for (size_t i = pos + 1; i < pos + len; i++) cz_x_lzenc_insert(z, i);
            z->cur = pos + len;
            continue;
        }
        if (kind == 1) cz_x_enc_match(z, pos, dist, len);
        else if (kind == 2) cz_x_enc_rep(z, pos, which, len);
        else {
            len = 1;
            if (shortrep) cz_x_enc_rep(z, pos, 0, 1);
            else cz_x_enc_literal(z, pos);
        }
        for (size_t i = pos; i < pos + len; i++) cz_x_lzenc_insert(z, i);
        z->cur = pos + len;
    }
    return z->cur - start;
}

/* ======================================================================
 *  the filters before LZMA2: delta and the branch converters (BCJ)
 * ====================================================================== */
#define CZ_X_F_DELTA 0x03
#define CZ_X_F_X86 0x04
#define CZ_X_F_PPC 0x05
#define CZ_X_F_IA64 0x06
#define CZ_X_F_ARM 0x07
#define CZ_X_F_ARMTHUMB 0x08
#define CZ_X_F_SPARC 0x09
#define CZ_X_F_ARM64 0x0a
#define CZ_X_F_RISCV 0x0b
#define CZ_X_F_LZMA1 0x4000000000000001ULL
#define CZ_X_F_LZMA2 0x21

struct cz_x_filter {
    uint64_t id;
    uint32_t start;           /* BCJ: the start offset */
    int dist;                 /* delta */
    int has_start;            /* the start offset was given (props of 4 bytes) */
    /* LZMA */
    uint32_t dict;
    int lc, lp, pb;
    int fast, nice, mf, depth, preset;
    /* state */
    uint32_t now;
    uint32_t prev_mask, prev_pos;
    uint8_t hist[256];
    uint8_t hpos;
};

static int cz_x_x86_ms(uint8_t b) { return b == 0 || b == 0xff; }

/* convert BUF in place; how much of it was done (the rest needs the
 * bytes after it) */
static size_t cz_x_bcj(struct cz_x_filter *f, uint8_t *buf, size_t size, int enc) {
    size_t i = 0;
    uint32_t now = f->now;
    switch (f->id) {
    case CZ_X_F_X86: {
        static const uint8_t allowed[8] = {1, 1, 1, 0, 1, 0, 0, 0};
        static const uint8_t bitnum[8] = {0, 1, 2, 2, 3, 3, 3, 3};
        uint32_t pmask = f->prev_mask, ppos = f->prev_pos;
        if (size < 5) return 0;
        if (now - ppos > 5) ppos = now - 5;
        size_t limit = size - 5;
        while (i <= limit) {
            uint8_t b = buf[i];
            if (b != 0xe8 && b != 0xe9) { i++; continue; }
            uint32_t off = now + (uint32_t)i - ppos;
            ppos = now + (uint32_t)i;
            if (off > 5) pmask = 0;
            else for (uint32_t k = 0; k < off; k++) { pmask &= 0x77; pmask <<= 1; }
            b = buf[i + 4];
            if (cz_x_x86_ms(b) && allowed[(pmask >> 1) & 7] && (pmask >> 1) < 0x10) {
                uint32_t src = (uint32_t)b << 24 | (uint32_t)buf[i + 3] << 16 | (uint32_t)buf[i + 2] << 8 | buf[i + 1];
                uint32_t dest;
                for (;;) {
                    uint32_t here = now + (uint32_t)i + 5;
                    dest = enc ? src + here : src - here;
                    if (pmask == 0) break;
                    uint32_t k = bitnum[pmask >> 1];
                    b = (uint8_t)(dest >> (24 - k * 8));
                    if (!cz_x_x86_ms(b)) break;
                    src = dest ^ ((1u << (32 - k * 8)) - 1);
                }
                buf[i + 4] = (uint8_t)~(((dest >> 24) & 1) - 1);
                buf[i + 3] = (uint8_t)(dest >> 16);
                buf[i + 2] = (uint8_t)(dest >> 8);
                buf[i + 1] = (uint8_t)dest;
                i += 5;
                pmask = 0;
            } else {
                i++;
                pmask |= 1;
                if (cz_x_x86_ms(b)) pmask |= 0x10;
            }
        }
        f->prev_mask = pmask;
        f->prev_pos = ppos;
        break;
    }
    case CZ_X_F_ARM:
        for (i = 0; i + 4 <= size; i += 4) {
            if (buf[i + 3] != 0xeb) continue;
            uint32_t src = ((uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 1] << 8 | buf[i]) << 2;
            uint32_t here = now + (uint32_t)i + 8;
            uint32_t dest = (enc ? src + here : src - here) >> 2;
            buf[i + 2] = (uint8_t)(dest >> 16);
            buf[i + 1] = (uint8_t)(dest >> 8);
            buf[i] = (uint8_t)dest;
        }
        break;
    case CZ_X_F_ARMTHUMB:
        for (i = 0; i + 4 <= size; i += 2) {
            if ((buf[i + 1] & 0xf8) == 0xf0 && (buf[i + 3] & 0xf8) == 0xf8) {
                uint32_t src = (((uint32_t)buf[i + 1] & 7) << 19 | (uint32_t)buf[i] << 11 |
                                ((uint32_t)buf[i + 3] & 7) << 8 | buf[i + 2]) << 1;
                uint32_t here = now + (uint32_t)i + 4;
                uint32_t dest = (enc ? src + here : src - here) >> 1;
                buf[i + 1] = (uint8_t)(0xf0 | ((dest >> 19) & 7));
                buf[i] = (uint8_t)(dest >> 11);
                buf[i + 3] = (uint8_t)(0xf8 | ((dest >> 8) & 7));
                buf[i + 2] = (uint8_t)dest;
                i += 2;
            }
        }
        break;
    case CZ_X_F_PPC:
        for (i = 0; i + 4 <= size; i += 4) {
            if ((buf[i] >> 2) != 0x12 || (buf[i + 3] & 3) != 1) continue;
            uint32_t src = ((uint32_t)buf[i] & 3) << 24 | (uint32_t)buf[i + 1] << 16 |
                           (uint32_t)buf[i + 2] << 8 | ((uint32_t)buf[i + 3] & ~3u);
            uint32_t here = now + (uint32_t)i;
            uint32_t dest = enc ? src + here : src - here;
            buf[i] = (uint8_t)(0x48 | ((dest >> 24) & 3));
            buf[i + 1] = (uint8_t)(dest >> 16);
            buf[i + 2] = (uint8_t)(dest >> 8);
            buf[i + 3] = (uint8_t)((buf[i + 3] & 3) | (dest & ~3u));
        }
        break;
    case CZ_X_F_SPARC:
        for (i = 0; i + 4 <= size; i += 4) {
            if (!((buf[i] == 0x40 && (buf[i + 1] & 0xc0) == 0) || (buf[i] == 0x7f && (buf[i + 1] & 0xc0) == 0xc0)))
                continue;
            uint32_t src = ((uint32_t)buf[i] << 24 | (uint32_t)buf[i + 1] << 16 | (uint32_t)buf[i + 2] << 8 | buf[i + 3]) << 2;
            uint32_t here = now + (uint32_t)i;
            uint32_t dest = (enc ? src + here : src - here) >> 2;
            dest = (((0u - ((dest >> 22) & 1)) << 22) & 0x3fffffffu) | (dest & 0x3fffffu) | 0x40000000u;
            buf[i] = (uint8_t)(dest >> 24);
            buf[i + 1] = (uint8_t)(dest >> 16);
            buf[i + 2] = (uint8_t)(dest >> 8);
            buf[i + 3] = (uint8_t)dest;
        }
        break;
    case CZ_X_F_IA64: {
        static const uint8_t branch[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                           4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
        for (i = 0; i + 16 <= size; i += 16) {
            uint32_t mask = branch[buf[i] & 0x1f];
            for (uint32_t slot = 0, bitpos = 5; slot < 3; slot++, bitpos += 41) {
                if (!((mask >> slot) & 1)) continue;
                size_t bytepos = bitpos >> 3;
                uint32_t bitres = bitpos & 7;
                uint64_t ins = 0;
                for (int j = 0; j < 6; j++) ins |= (uint64_t)buf[i + (size_t)j + bytepos] << (8 * j);
                uint64_t norm = ins >> bitres;
                if (((norm >> 37) & 0xf) == 0x5 && ((norm >> 9) & 7) == 0) {
                    uint32_t src = (uint32_t)((norm >> 13) & 0xfffff);
                    src |= (uint32_t)((norm >> 36) & 1) << 20;
                    src <<= 4;
                    uint32_t here = now + (uint32_t)i;
                    uint32_t dest = (enc ? src + here : src - here) >> 4;
                    norm &= ~((uint64_t)0x8fffff << 13);
                    norm |= (uint64_t)(dest & 0xfffff) << 13;
                    norm |= (uint64_t)(dest & 0x100000) << (36 - 20);
                    ins &= (1u << bitres) - 1;
                    ins |= norm << bitres;
                    for (int j = 0; j < 6; j++) buf[i + (size_t)j + bytepos] = (uint8_t)(ins >> (8 * j));
                }
            }
        }
        break;
    }
    case CZ_X_F_ARM64:
        for (i = 0; i + 4 <= size; i += 4) {
            uint32_t pc = now + (uint32_t)i;
            uint32_t ins = (uint32_t)buf[i] | (uint32_t)buf[i + 1] << 8 | (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24;
            if ((ins >> 26) == 0x25) {
                uint32_t src = ins;
                pc >>= 2;
                if (!enc) pc = 0u - pc;
                ins = 0x94000000u | ((src + pc) & 0x03ffffffu);
            } else if ((ins & 0x9f000000u) == 0x90000000u) {
                uint32_t src = ((ins >> 29) & 3) | ((ins >> 3) & 0x001ffffcu);
                if ((src + 0x00020000u) & 0x001c0000u) continue;
                ins &= 0x9000001fu;
                pc >>= 12;
                if (!enc) pc = 0u - pc;
                uint32_t dest = src + pc;
                ins |= (dest & 3) << 29;
                ins |= (dest & 0x0003fffcu) << 3;
                ins |= (0u - (dest & 0x00020000u)) & 0x00e00000u;
            } else continue;
            buf[i] = (uint8_t)ins;
            buf[i + 1] = (uint8_t)(ins >> 8);
            buf[i + 2] = (uint8_t)(ins >> 16);
            buf[i + 3] = (uint8_t)(ins >> 24);
        }
        break;
    case CZ_X_F_DELTA:
        for (i = 0; i < size; i++) {
            uint8_t h = f->hist[(uint8_t)(f->dist + f->hpos)];
            if (enc) {
                uint8_t t = buf[i];
                buf[i] = (uint8_t)(t - h);
                f->hist[f->hpos--] = t;
            } else {
                buf[i] = (uint8_t)(buf[i] + h);
                f->hist[f->hpos--] = buf[i];
            }
        }
        return size;
    default:
        return size;
    }
    f->now = now + (uint32_t)i;
    return i;
}

static void cz_x_filter_reset(struct cz_x_filter *f) {
    f->now = f->start;
    f->prev_mask = 0;
    f->prev_pos = (uint32_t)-5;
    memset(f->hist, 0, sizeof f->hist);
    f->hpos = 0;
}

/* a filter between a producer and the next stage: what the filter cannot
 * yet convert waits for more bytes, and passes as it is at the end */
struct cz_x_fstage {
    struct cz_x_filter *f;
    int enc;
    uint8_t *buf;
    size_t n, cap;
    void (*next)(void *ctx, const uint8_t *p, size_t n);
    void *nctx;
};

static void cz_x_fstage_feed(void *ctx, const uint8_t *p, size_t n) {
    struct cz_x_fstage *s = (struct cz_x_fstage *)ctx;
    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2 + 4096;
        s->buf = (uint8_t *)realloc(s->buf, s->cap);
        if (!s->buf) { fprintf(stderr, "xz: Memory allocation failed\n"); exit(1); }
    }
    memcpy(s->buf + s->n, p, n);
    s->n += n;
    size_t done = cz_x_bcj(s->f, s->buf, s->n, s->enc);
    if (done) {
        s->next(s->nctx, s->buf, done);
        memmove(s->buf, s->buf + done, s->n - done);
        s->n -= done;
    }
}

static void cz_x_fstage_finish(struct cz_x_fstage *s) {
    if (s->n) s->next(s->nctx, s->buf, s->n);
    s->n = 0;
}

/* ======================================================================
 *  decoding the containers
 * ====================================================================== */
#define CZ_X_E_OK 0
#define CZ_X_E_FORMAT 1       /* File format not recognized */
#define CZ_X_E_DATA 2         /* Compressed data is corrupt */
#define CZ_X_E_EOF 3          /* Unexpected end of input */
#define CZ_X_E_OPTIONS 4      /* Unsupported options */
#define CZ_X_E_MEM 5          /* Memory usage limit reached */

struct cz_x_dctx {
    struct cz_in *in;
    void (*out)(void *ctx, const uint8_t *p, size_t n);
    void *octx;
    uint64_t memlimit;       /* 0: none */
    uint64_t memneeded;      /* when CZ_X_E_MEM */
    int ignore_check;
    int single_stream;
    int unsupported_check;   /* a check of an unknown kind was met */
    int no_check_warned;
    uint64_t total_out;
    struct cz_x_model m;
    struct cz_x_dict dict;
    struct cz_x_filter flt[4];
    int nflt;
    /* raw mode: the chain from the command line */
    struct cz_x_filter *raw;
    int nraw;
};

static int cz_x_lz_err(int r) {
    return r == CZ_X_D_EOF ? CZ_X_E_EOF : r == CZ_X_D_MEM ? CZ_X_E_MEM : CZ_X_E_DATA;
}

static int cz_x_getb(struct cz_x_dctx *c, int *v) {
    int b = cz_getc(c->in);
    if (b < 0) return CZ_X_E_EOF;
    *v = b;
    return CZ_X_E_OK;
}

/* the decoded output: the check, the count, then the caller */
struct cz_x_sink {
    struct cz_x_dctx *c;
    struct cz_x_check *chk;
    uint64_t n;
};

static void cz_x_sink_out(void *ctx, const uint8_t *p, size_t n) {
    struct cz_x_sink *s = (struct cz_x_sink *)ctx;
    if (s->chk) cz_x_check_update(s->chk, p, n);
    s->n += n;
    s->c->total_out += n;
    s->c->out(s->c->octx, p, n);
}

/* the decoder memory a chain needs, as liblzma counts it */
static uint64_t cz_x_chain_mem(const struct cz_x_filter *f, int n) {
    uint64_t m = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].id == CZ_X_F_LZMA2 || f[i].id == CZ_X_F_LZMA1) m += (uint64_t)f[i].dict + 66200;
        else if (f[i].id == CZ_X_F_DELTA) m += 1376 - 0;
        else m += 0;
    }
    return m;
}

static uint64_t cz_x_enc_mem(const struct cz_x_filter *f, int n) {
    uint64_t m = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].id != CZ_X_F_LZMA2 && f[i].id != CZ_X_F_LZMA1) continue;
        uint64_t dict = f[i].dict;
        uint64_t before = 4096, after = 4097, mlen = 273;
        uint64_t keep_before = before + dict, keep_after = after + mlen;
        uint64_t reserve = dict / 2;
        if (reserve > (1u << 30)) reserve /= 2;
        reserve += (before + mlen + after) / 2 + (1u << 19);
        uint64_t size = keep_before + reserve + keep_after;
        int hb = f[i].mf % 10 == 2 ? 2 : f[i].mf % 10 == 3 ? 3 : 4;
        if (f[i].mf == 3) hb = 3;
        if (f[i].mf == 4) hb = 4;
        int bt = f[i].mf >= 12;
        uint64_t hs;
        if (hb == 2) hs = 0xffff;
        else {
            hs = dict - 1;
            hs |= hs >> 1; hs |= hs >> 2; hs |= hs >> 4; hs |= hs >> 8; hs |= hs >> 16;
            hs >>= 1;
            hs |= 0xffff;
            if (hs > (1u << 24)) hs = hb == 3 ? (1u << 24) - 1 : hs >> 1;
        }
        hs++;
        if (hb > 2) hs += 1u << 10;
        if (hb > 3) hs += 1u << 16;
        uint64_t sons = dict + 1;
        if (bt) sons *= 2;
        m += (hs + sons) * 4 + size + 300000;
        /* the rest of the coder, to the byte as xz -M judges it (bisected,
         * logs/b34/xzmem.py): LZMA2's 48264 bytes more, LZMA1's 65704 fewer
         * than LZMA2's -- this sum matched -vv's MiB alone */
        m += f[i].id == CZ_X_F_LZMA2 ? 48264 : 48264 - 65704;
    }
    /* each filter before it: a branch converter 1024 bytes, delta 352
     * (xzmemflt.py) */
    for (int i = 0; i < n; i++) {
        if (f[i].id == CZ_X_F_LZMA2 || f[i].id == CZ_X_F_LZMA1) continue;
        m += f[i].id == CZ_X_F_DELTA ? 352 : 1024;
    }
    return m;
}

/* LZMA2 chunks until the end marker; through the stage chain to SINK */
static int cz_x_lzma2_decode(struct cz_x_dctx *c, uint32_t dictsize, uint64_t *consumed) {
    uint64_t start = c->in->total;
    int need_dict = 1, need_props = 1;
    if (cz_x_dict_init(&c->dict, dictsize) != 0) return CZ_X_E_MEM;
    for (;;) {
        int ctrl;
        if (cz_x_getb(c, &ctrl)) return CZ_X_E_EOF;
        if (ctrl == 0) break;
        if (ctrl >= 0xe0 || ctrl == 1) {
            need_dict = 0;
            c->dict.pos = 0; c->dict.full = 0; c->dict.flushed = 0;
        } else if (need_dict) return CZ_X_E_DATA;
        if (ctrl >= 0x80) {
            int b1, b2, b3, b4;
            if (cz_x_getb(c, &b1) || cz_x_getb(c, &b2) || cz_x_getb(c, &b3) || cz_x_getb(c, &b4)) return CZ_X_E_EOF;
            uint64_t usize = ((uint64_t)(ctrl & 0x1f) << 16 | (uint64_t)b1 << 8 | (uint64_t)b2) + 1;
            uint64_t csize = ((uint64_t)b3 << 8 | (uint64_t)b4) + 1;
            int mode = (ctrl >> 5) & 3;
            if (mode >= 2) {
                int pr;
                if (cz_x_getb(c, &pr)) return CZ_X_E_EOF;
                if (pr >= 9 * 5 * 5) return CZ_X_E_DATA;
                int lc = pr % 9, rest = pr / 9, lp = rest % 5, pb = rest / 5;
                if (lc + lp > 4) return CZ_X_E_DATA;
                if (cz_x_model_props(&c->m, lc, lp, pb) != 0) return CZ_X_E_MEM;
                need_props = 0;
            } else if (need_props) return CZ_X_E_DATA;
            if (mode >= 1) cz_x_model_reset(&c->m);
            struct cz_x_rcd r;
            r.in = c->in;
            r.limit = csize;
            r.used = 0;
            int ir = cz_x_rcd_init(&r);
            if (ir == CZ_X_D_EOF) return CZ_X_E_EOF;
            if (ir != CZ_X_D_OK) return CZ_X_E_DATA;
            int em;
            int dr = cz_x_lzma_decode(&c->m, &r, &c->dict, usize, 0, &em);
            if (dr != CZ_X_D_OK) { cz_x_dict_flush(&c->dict); return cz_x_lz_err(dr); }
            /* the chunk's compressed size, exactly, and a finished coder */
            if (r.used != csize || r.code != 0) { cz_x_dict_flush(&c->dict); return CZ_X_E_DATA; }
        } else if (ctrl == 1 || ctrl == 2) {
            int b1, b2;
            if (cz_x_getb(c, &b1) || cz_x_getb(c, &b2)) return CZ_X_E_EOF;
            uint64_t usize = ((uint64_t)b1 << 8 | (uint64_t)b2) + 1;
            for (uint64_t i = 0; i < usize; i++) {
                int b;
                if (cz_x_getb(c, &b)) { cz_x_dict_flush(&c->dict); return CZ_X_E_EOF; }
                cz_x_dict_put(&c->dict, (uint8_t)b);
            }
        } else return CZ_X_E_DATA;
        cz_x_dict_flush(&c->dict);
    }
    cz_x_dict_flush(&c->dict);
    if (consumed) *consumed = c->in->total - start;
    return CZ_X_E_OK;
}

/* the filter flags of a block header; CZ_X_E_OPTIONS for what is not known */
static int cz_x_parse_filters(const uint8_t *h, size_t n, size_t *pos, struct cz_x_filter *f, int nf) {
    for (int i = 0; i < nf; i++) {
        memset(&f[i], 0, sizeof f[i]);
        uint64_t id = 0, psz = 0;
        for (int k = 0, sh = 0;; k++, sh += 7) {
            if (*pos >= n || k >= 9) return CZ_X_E_DATA;
            uint8_t b = h[(*pos)++];
            id |= (uint64_t)(b & 0x7f) << sh;
            if (!(b & 0x80)) break;
        }
        for (int k = 0, sh = 0;; k++, sh += 7) {
            if (*pos >= n || k >= 9) return CZ_X_E_DATA;
            uint8_t b = h[(*pos)++];
            psz |= (uint64_t)(b & 0x7f) << sh;
            if (!(b & 0x80)) break;
        }
        if (*pos + psz > n) return CZ_X_E_DATA;
        const uint8_t *p = h + *pos;
        *pos += psz;
        f[i].id = id;
        if (id == CZ_X_F_LZMA2) {
            if (psz != 1 || p[0] > 40) return CZ_X_E_OPTIONS;
            f[i].dict = p[0] == 40 ? 0xffffffffu : (2u | (p[0] & 1u)) << (p[0] / 2 + 11);
            if (i != nf - 1) return CZ_X_E_OPTIONS;
        } else if (id == CZ_X_F_DELTA) {
            if (psz != 1) return CZ_X_E_OPTIONS;
            f[i].dist = p[0] + 1;
            if (i == nf - 1) return CZ_X_E_OPTIONS;
        } else if (id >= CZ_X_F_X86 && id <= CZ_X_F_ARM64) {
            if (psz == 4) {
                f[i].start = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
                f[i].has_start = 1;
            } else if (psz != 0) return CZ_X_E_OPTIONS;
            if (i == nf - 1) return CZ_X_E_OPTIONS;
        } else return CZ_X_E_OPTIONS;
    }
    return CZ_X_E_OK;
}

/* the decoded bytes through the chain's non-LZMA filters (in reverse) */
static int cz_x_run_chain_lzma2(struct cz_x_dctx *c, struct cz_x_filter *f, int nf, struct cz_x_sink *sink, uint64_t *csz) {
    struct cz_x_fstage st[3];
    void (*next)(void *, const uint8_t *, size_t) = cz_x_sink_out;
    void *nctx = sink;
    for (int i = 0; i < nf - 1; i++) {
        cz_x_filter_reset(&f[i]);
        st[i].f = &f[i];
        st[i].enc = 0;
        st[i].buf = 0;
        st[i].n = st[i].cap = 0;
        st[i].next = next;
        st[i].nctx = nctx;
        next = cz_x_fstage_feed;
        nctx = &st[i];
    }
    c->dict.out = next;
    c->dict.octx = nctx;
    int r = cz_x_lzma2_decode(c, f[nf - 1].dict, csz);
    for (int i = nf - 2; i >= 0; i--) {
        if (r == CZ_X_E_OK) cz_x_fstage_finish(&st[i]);
        free(st[i].buf);
    }
    return r;
}

/* one .xz stream (its magic already read and matched) */
static int cz_x_xz_stream(struct cz_x_dctx *c) {
    uint8_t flags[2], crc[4];
    for (int i = 0; i < 2; i++) { int b; if (cz_x_getb(c, &b)) return CZ_X_E_EOF; flags[i] = (uint8_t)b; }
    for (int i = 0; i < 4; i++) { int b; if (cz_x_getb(c, &b)) return CZ_X_E_EOF; crc[i] = (uint8_t)b; }
    uint32_t want = (uint32_t)crc[0] | (uint32_t)crc[1] << 8 | (uint32_t)crc[2] << 16 | (uint32_t)crc[3] << 24;
    if (cz_crc32(0, flags, 2) != want) return CZ_X_E_DATA;
    if (flags[0] != 0 || (flags[1] & 0xf0)) return CZ_X_E_OPTIONS;
    int check = flags[1] & 0x0f;
    int csize = cz_x_check_size(check);
    int verify = !c->ignore_check && (check == 0 || check == 1 || check == 4 || check == 10);
    if (check != 0 && check != 1 && check != 4 && check != 10) c->unsupported_check = 1;
    /* the blocks */
    uint64_t *rec = 0;
    size_t nrec = 0, caprec = 0;
    for (;;) {
        int hb;
        if (cz_x_getb(c, &hb)) { free(rec); return CZ_X_E_EOF; }
        if (hb == 0) break;   /* the index */
        size_t hsize = ((size_t)hb + 1) * 4;
        uint8_t h[1024];
        h[0] = (uint8_t)hb;
        for (size_t i = 1; i < hsize; i++) { int b; if (cz_x_getb(c, &b)) { free(rec); return CZ_X_E_EOF; } h[i] = (uint8_t)b; }
        uint32_t hc = (uint32_t)h[hsize - 4] | (uint32_t)h[hsize - 3] << 8 | (uint32_t)h[hsize - 2] << 16 | (uint32_t)h[hsize - 1] << 24;
        if (cz_crc32(0, h, hsize - 4) != hc) { free(rec); return CZ_X_E_DATA; }
        if (h[1] & 0x3c) { free(rec); return CZ_X_E_OPTIONS; }
        int nf = (h[1] & 3) + 1;
        size_t pos = 2;
        uint64_t hcsize = 0, husize = 0;
        int have_c = (h[1] & 0x40) != 0, have_u = (h[1] & 0x80) != 0;
        for (int which = 0; which < 2; which++) {
            if (!(which == 0 ? have_c : have_u)) continue;
            uint64_t v = 0;
            int k, sh = 0;
            for (k = 0;; k++, sh += 7) {
                if (pos >= hsize - 4 || k >= 9) { free(rec); return CZ_X_E_DATA; }
                uint8_t b = h[pos++];
                v |= (uint64_t)(b & 0x7f) << sh;
                if (!(b & 0x80)) break;
            }
            if (which == 0) hcsize = v; else husize = v;
        }
        if (have_c && hcsize == 0) { free(rec); return CZ_X_E_DATA; }
        struct cz_x_filter f[4];
        int pr = cz_x_parse_filters(h, hsize - 4, &pos, f, nf);
        if (pr != CZ_X_E_OK) { free(rec); return pr; }
        for (size_t i = pos; i < hsize - 4; i++) if (h[i]) { free(rec); return CZ_X_E_OPTIONS; }
        uint64_t mem = cz_x_chain_mem(f, nf);
        if (c->memlimit && mem > c->memlimit) { c->memneeded = mem; free(rec); return CZ_X_E_MEM; }
        struct cz_x_check chk;
        cz_x_check_init(&chk, verify ? check : 0);
        struct cz_x_sink sink = {c, &chk, 0};
        uint64_t used = 0;
        int r = cz_x_run_chain_lzma2(c, f, nf, &sink, &used);
        if (r != CZ_X_E_OK) { free(rec); return r; }
        if ((have_c && used != hcsize) || (have_u && sink.n != husize)) { free(rec); return CZ_X_E_DATA; }
        for (uint64_t i = used; i % 4; i++) {
            int b;
            if (cz_x_getb(c, &b)) { free(rec); return CZ_X_E_EOF; }
            if (b) { free(rec); return CZ_X_E_DATA; }
        }
        uint8_t got[64], mine[64];
        for (int i = 0; i < csize; i++) { int b; if (cz_x_getb(c, &b)) { free(rec); return CZ_X_E_EOF; } got[i] = (uint8_t)b; }
        if (verify && check != 0) {
            cz_x_check_final(&chk, mine);
            if (memcmp(got, mine, (size_t)csize) != 0) { free(rec); return CZ_X_E_DATA; }
        }
        if (nrec + 2 > caprec) { caprec = caprec ? caprec * 2 : 16; rec = (uint64_t *)realloc(rec, caprec * sizeof *rec); }
        rec[nrec++] = hsize + used + (uint64_t)csize;
        rec[nrec++] = sink.n;
    }
    /* the index: the same records, then its CRC */
    uint64_t istart = c->in->total - 1;
    uint32_t icrc = cz_crc32(0, (const uint8_t *)"\0", 1);
    uint64_t count;
    uint8_t ib[1];
    /* a varint, read byte by byte to feed the CRC */
#define CZ_X_IVAR(dst) do { uint64_t x_ = 0; int k_; for (k_ = 0; ; k_++) { int b_; \
        if (k_ >= 9) { free(rec); return CZ_X_E_DATA; } \
        if (cz_x_getb(c, &b_)) { free(rec); return CZ_X_E_EOF; } \
        ib[0] = (uint8_t)b_; icrc = cz_crc32(icrc, ib, 1); \
        x_ |= (uint64_t)(b_ & 0x7f) << (7 * k_); if (!(b_ & 0x80)) break; } (dst) = x_; } while (0)
    CZ_X_IVAR(count);
    if (count != nrec / 2) { free(rec); return CZ_X_E_DATA; }
    for (uint64_t i = 0; i < count; i++) {
        uint64_t unp, unc;
        CZ_X_IVAR(unp);
        CZ_X_IVAR(unc);
        if (unp != rec[2 * i] || unc != rec[2 * i + 1]) { free(rec); return CZ_X_E_DATA; }
    }
    free(rec);
    while ((c->in->total - istart) % 4) {
        int b;
        if (cz_x_getb(c, &b)) return CZ_X_E_EOF;
        if (b) return CZ_X_E_DATA;
        ib[0] = 0;
        icrc = cz_crc32(icrc, ib, 1);
    }
    uint64_t isize = c->in->total - istart;
    uint8_t t[4];
    for (int i = 0; i < 4; i++) { int b; if (cz_x_getb(c, &b)) return CZ_X_E_EOF; t[i] = (uint8_t)b; }
    if (((uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24) != icrc) return CZ_X_E_DATA;
    isize += 4;
    /* the footer */
    uint8_t ft[12];
    for (int i = 0; i < 12; i++) { int b; if (cz_x_getb(c, &b)) return CZ_X_E_EOF; ft[i] = (uint8_t)b; }
    uint32_t fcrc = (uint32_t)ft[0] | (uint32_t)ft[1] << 8 | (uint32_t)ft[2] << 16 | (uint32_t)ft[3] << 24;
    if (ft[10] != 'Y' || ft[11] != 'Z') return CZ_X_E_DATA;
    if (cz_crc32(0, ft + 4, 6) != fcrc) return CZ_X_E_DATA;
    uint32_t back = (uint32_t)ft[4] | (uint32_t)ft[5] << 8 | (uint32_t)ft[6] << 16 | (uint32_t)ft[7] << 24;
    if (((uint64_t)back + 1) * 4 != isize) return CZ_X_E_DATA;
    if (ft[8] != flags[0] || ft[9] != flags[1]) return CZ_X_E_DATA;
    return CZ_X_E_OK;
}

static const uint8_t cz_x_xz_magic[6] = {0xfd, '7', 'z', 'X', 'Z', 0x00};

/* all the .xz streams of the input, with stream padding between */
static int cz_x_xz_decode(struct cz_x_dctx *c) {
    int first = 1;
    for (;;) {
        /* the magic; a later stream's twelve header bytes are all read
         * before it is judged (so a short tail is the input's end) */
        uint8_t mg[12];
        size_t need = first ? 6 : 12;
        size_t got = cz_read(c->in, mg, need);
        if (got < need) return first ? CZ_X_E_FORMAT : CZ_X_E_EOF;
        if (memcmp(mg, cz_x_xz_magic, 6) != 0) return first ? CZ_X_E_FORMAT : CZ_X_E_DATA;
        if (!first) for (int i = 11; i >= 6; i--) cz_ungetc(c->in, mg[i]);
        int r = cz_x_xz_stream(c);
        if (r != CZ_X_E_OK) return r;
        first = 0;
        if (c->single_stream) return CZ_X_E_OK;
        /* stream padding: zeros, four at a time */
        for (;;) {
            int b = cz_peekc(c->in);
            if (b < 0) {
                if (c->in->err) return CZ_X_E_EOF;
                return CZ_X_E_OK;
            }
            if (b != 0) break;
            uint8_t z[4];
            size_t got = cz_read(c->in, z, 4);
            if (got < 4) return CZ_X_E_DATA;
            if (z[0] | z[1] | z[2] | z[3]) return CZ_X_E_DATA;
        }
    }
}

/* .lzma: 13 header bytes, then LZMA1 with a known size or an end marker */
static int cz_x_lzma_header_ok(const uint8_t *h) {
    if (h[0] > (4 * 5 + 4) * 9 + 8) return 0;
    uint32_t d = (uint32_t)h[1] | (uint32_t)h[2] << 8 | (uint32_t)h[3] << 16 | (uint32_t)h[4] << 24;
    if (d != 0xffffffffu) {
        /* a power of two, or one and a half of one */
        int ok = 0;
        for (int n = 0; n < 32 && !ok; n++) {
            uint32_t p = 1u << n;
            if (d == p || (n > 0 && d == p + (p >> 1))) ok = 1;
        }
        if (!ok) return 0;
    }
    uint64_t usz = 0;
    for (int i = 0; i < 8; i++) usz |= (uint64_t)h[5 + i] << (8 * i);
    if (usz != UINT64_MAX && usz >= (1ULL << 38)) return 0;
    return 1;
}

static int cz_x_lzma_alone_decode(struct cz_x_dctx *c, const uint8_t *h) {
    int pr = h[0];
    int lc = pr % 9, rest = pr / 9, lp = rest % 5, pb = rest / 5;
    uint32_t d = (uint32_t)h[1] | (uint32_t)h[2] << 8 | (uint32_t)h[3] << 16 | (uint32_t)h[4] << 24;
    uint64_t usz = 0;
    for (int i = 0; i < 8; i++) usz |= (uint64_t)h[5 + i] << (8 * i);
    if (d < 4096) d = 4096;
    uint64_t mem = (uint64_t)d + 66200;
    if (c->memlimit && mem > c->memlimit) { c->memneeded = mem; return CZ_X_E_MEM; }
    if (cz_x_model_props(&c->m, lc, lp, pb) != 0) return CZ_X_E_MEM;
    cz_x_model_reset(&c->m);
    if (cz_x_dict_init(&c->dict, d) != 0) return CZ_X_E_MEM;
    struct cz_x_sink sink = {c, 0, 0};
    c->dict.out = cz_x_sink_out;
    c->dict.octx = &sink;
    struct cz_x_rcd r;
    r.in = c->in;
    r.limit = UINT64_MAX;
    r.used = 0;
    int ir = cz_x_rcd_init(&r);
    if (ir == CZ_X_D_EOF) return CZ_X_E_EOF;
    if (ir != CZ_X_D_OK) return CZ_X_E_DATA;
    int em;
    int dr = cz_x_lzma_decode(&c->m, &r, &c->dict, usz, 1, &em);
    cz_x_dict_flush(&c->dict);
    if (dr != CZ_X_D_OK) return cz_x_lz_err(dr);
    if (usz != UINT64_MAX && !em) {
        /* a known size: the end marker may still follow */
    }
    if (r.code != 0) return CZ_X_E_DATA;
    /* nothing may follow */
    if (cz_peekc(c->in) >= 0) return CZ_X_E_DATA;
    return CZ_X_E_OK;
}

/* lzip: "LZIP", a version, the dictionary size; LZMA1 (lc 3, lp 0, pb 2)
 * with an end marker; the CRC-32, the data size and the member size */
static int cz_x_lzip_decode(struct cz_x_dctx *c, int first_read) {
    int first = 1;
    for (;;) {
        uint8_t h[6];
        int n0 = 0;
        if (first && first_read) { memcpy(h, "LZIP", 4); n0 = 4; }
        if (!first && cz_peekc(c->in) < 0) return CZ_X_E_OK;
        for (int i = n0; i < 6; i++) {
            int b;
            if (cz_x_getb(c, &b)) return first ? CZ_X_E_EOF : CZ_X_E_EOF;
            h[i] = (uint8_t)b;
        }
        if (memcmp(h, "LZIP", 4) != 0) return first ? CZ_X_E_FORMAT : CZ_X_E_DATA;
        if (h[4] > 1) return CZ_X_E_OPTIONS;
        uint32_t d = 1u << (h[5] & 0x1f);
        d -= (d / 16) * ((h[5] >> 5) & 7);
        if (d < 4096 || d > (512u << 20)) return CZ_X_E_DATA;
        uint64_t mem = (uint64_t)d + 66200;
        if (c->memlimit && mem > c->memlimit) { c->memneeded = mem; return CZ_X_E_MEM; }
        if (cz_x_model_props(&c->m, 3, 0, 2) != 0) return CZ_X_E_MEM;
        cz_x_model_reset(&c->m);
        if (cz_x_dict_init(&c->dict, d) != 0) return CZ_X_E_MEM;
        struct cz_x_check chk;
        cz_x_check_init(&chk, CZ_X_CHECK_CRC32);
        struct cz_x_sink sink = {c, &chk, 0};
        c->dict.out = cz_x_sink_out;
        c->dict.octx = &sink;
        uint64_t mstart = c->in->total - 6;
        struct cz_x_rcd r;
        r.in = c->in;
        r.limit = UINT64_MAX;
        r.used = 0;
        int ir = cz_x_rcd_init(&r);
        if (ir == CZ_X_D_EOF) return CZ_X_E_EOF;
        if (ir != CZ_X_D_OK) return CZ_X_E_DATA;
        int em;
        int dr = cz_x_lzma_decode(&c->m, &r, &c->dict, UINT64_MAX, 1, &em);
        cz_x_dict_flush(&c->dict);
        if (dr != CZ_X_D_OK) return cz_x_lz_err(dr);
        int tl = h[4] == 0 ? 12 : 20;
        uint8_t t[20];
        for (int i = 0; i < tl; i++) { int b; if (cz_x_getb(c, &b)) return CZ_X_E_EOF; t[i] = (uint8_t)b; }
        uint32_t crc = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
        uint64_t dsz = 0, msz = 0;
        for (int i = 0; i < 8; i++) dsz |= (uint64_t)t[4 + i] << (8 * i);
        if (tl == 20) for (int i = 0; i < 8; i++) msz |= (uint64_t)t[12 + i] << (8 * i);
        if (!c->ignore_check && crc != chk.crc32) return CZ_X_E_DATA;
        if (dsz != sink.n) return CZ_X_E_DATA;
        if (tl == 20 && msz != c->in->total - mstart) return CZ_X_E_DATA;
        first = 0;
        if (c->single_stream) return CZ_X_E_OK;
        if (cz_peekc(c->in) < 0) return CZ_X_E_OK;
    }
}

/* raw: the chain given, LZMA1 to its end marker or LZMA2 to its own end */
static int cz_x_raw_decode(struct cz_x_dctx *c) {
    struct cz_x_filter *f = c->raw;
    int nf = c->nraw;
    for (int i = 0; i < nf; i++)          /* no RISC-V converter (see su_xz.c's validation) */
        if (f[i].id == CZ_X_F_RISCV) return CZ_X_E_OPTIONS;
    uint64_t mem = cz_x_chain_mem(f, nf);
    if (c->memlimit && mem > c->memlimit) { c->memneeded = mem; return CZ_X_E_MEM; }
    struct cz_x_sink sink = {c, 0, 0};
    if (f[nf - 1].id == CZ_X_F_LZMA2) {
        uint64_t used;
        return cz_x_run_chain_lzma2(c, f, nf, &sink, &used);
    }
    /* LZMA1 */
    struct cz_x_fstage st[3];
    void (*next)(void *, const uint8_t *, size_t) = cz_x_sink_out;
    void *nctx = &sink;
    for (int i = 0; i < nf - 1; i++) {
        cz_x_filter_reset(&f[i]);
        st[i].f = &f[i]; st[i].enc = 0; st[i].buf = 0; st[i].n = st[i].cap = 0;
        st[i].next = next; st[i].nctx = nctx;
        next = cz_x_fstage_feed; nctx = &st[i];
    }
    struct cz_x_filter *l = &f[nf - 1];
    if (cz_x_model_props(&c->m, l->lc, l->lp, l->pb) != 0) return CZ_X_E_MEM;
    cz_x_model_reset(&c->m);
    if (cz_x_dict_init(&c->dict, l->dict) != 0) return CZ_X_E_MEM;
    c->dict.out = next;
    c->dict.octx = nctx;
    struct cz_x_rcd r;
    r.in = c->in; r.limit = UINT64_MAX; r.used = 0;
    int ir = cz_x_rcd_init(&r);
    int res = CZ_X_E_OK;
    if (ir == CZ_X_D_EOF) res = CZ_X_E_EOF;
    else if (ir != CZ_X_D_OK) res = CZ_X_E_DATA;
    else {
        int em;
        int dr = cz_x_lzma_decode(&c->m, &r, &c->dict, UINT64_MAX, 1, &em);
        cz_x_dict_flush(&c->dict);
        if (dr != CZ_X_D_OK) res = cz_x_lz_err(dr);
    }
    for (int i = nf - 2; i >= 0; i--) {
        if (res == CZ_X_E_OK) cz_x_fstage_finish(&st[i]);
        free(st[i].buf);
    }
    return res;
}

/* ======================================================================
 *  writing: LZMA2 chunks, .xz streams, .lzma, raw
 * ====================================================================== */
#define CZ_X_L2_UMAX (2u << 20)        /* uncompressed bytes in one LZMA chunk */
#define CZ_X_L2_CMAX 65536u            /* compressed bytes in one */

/* the presets: dictionary, mode, nice length, match finder, depth */
static const struct { uint32_t dict; int fast, nice, mf, depth; } cz_x_presets[10] = {
    {1u << 18, 1, 128, 3, 4}, {1u << 20, 1, 128, 4, 8}, {2u << 20, 1, 273, 4, 24},
    {4u << 20, 1, 273, 4, 48}, {4u << 20, 0, 16, 14, 0}, {8u << 20, 0, 32, 14, 0},
    {8u << 20, 0, 64, 14, 0}, {16u << 20, 0, 64, 14, 0}, {32u << 20, 0, 64, 14, 0},
    {64u << 20, 0, 64, 14, 0},
};
/* mf: 3 hc3, 4 hc4, 12 bt2, 13 bt3, 14 bt4 */

static void cz_x_lzma_preset(struct cz_x_filter *f, int preset, int extreme) {
    f->dict = cz_x_presets[preset].dict;
    f->lc = 3; f->lp = 0; f->pb = 2;
    f->fast = cz_x_presets[preset].fast;
    f->nice = cz_x_presets[preset].nice;
    f->mf = cz_x_presets[preset].mf;
    f->depth = cz_x_presets[preset].depth;
    f->preset = preset;
    if (extreme) {
        f->fast = 0;
        f->mf = 14;
        if (preset == 3 || preset == 5) { f->nice = 192; f->depth = 0; }
        else { f->nice = 273; f->depth = 512; }
    }
}

/* the dictionary size as LZMA2's property byte */
static uint8_t cz_x_dict_prop(uint32_t d) {
    for (int i = 0; i < 40; i++) {
        uint32_t s = (2u | ((uint32_t)i & 1)) << (i / 2 + 11);
        if (s >= d) return (uint8_t)i;
    }
    return 40;
}

struct cz_x_out {
    void (*wr)(void *ctx, const uint8_t *p, size_t n);
    void *wctx;
    uint64_t n;
};

static void cz_x_put(struct cz_x_out *o, const uint8_t *p, size_t n) {
    o->n += n;
    o->wr(o->wctx, p, n);
}

/* the parser's effort from the LZMA options */
static void cz_x_effort(const struct cz_x_filter *f, int *depth, int *nice) {
    *nice = f->nice;
    if (f->depth) *depth = f->depth > 4096 ? 4096 : f->depth;
    else *depth = f->fast ? 8 : (f->mf >= 12 ? 16 + f->nice / 2 : 4 + f->nice / 4);
    if (*depth < 1) *depth = 1;
}

/* an LZMA2 stream of DATA (n bytes) into a growing buffer */
struct cz_x_buf { uint8_t *p; size_t n, cap; };

static void cz_x_buf_put(struct cz_x_buf *b, const uint8_t *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->p = (uint8_t *)realloc(b->p, b->cap);
        if (!b->p) { fprintf(stderr, "xz: Memory allocation failed\n"); exit(1); }
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

/* LZMA2 encoding of a whole block's data */
static int cz_x_lzma2_encode(const struct cz_x_filter *f, const uint8_t *data, size_t n, struct cz_x_buf *out) {
    int depth, nice;
    cz_x_effort(f, &depth, &nice);
    size_t hist = f->dict < (4u << 20) ? f->dict : (4u << 20);
    struct cz_x_lzenc *z = (struct cz_x_lzenc *)calloc(1, sizeof *z);
    if (!z || cz_x_lzenc_init(z, hist, depth, nice, f->lc, f->lp, f->pb) != 0) {
        free(z);
        return -1;
    }
    z->fast = f->fast;
    uint8_t props = (uint8_t)((f->pb * 5 + f->lp) * 9 + f->lc);
    int first = 1, need_props = 1, need_state = 1;
    size_t done = 0;
    while (done < n) {
        /* fill the window */
        cz_x_lzenc_slide(z);
        size_t room = z->wcap - z->wlen;
        size_t take = n - (done + (z->wlen - z->cur)) < room ? n - (done + (z->wlen - z->cur)) : room;
        memcpy(z->win + z->wlen, data + done + (z->wlen - z->cur), take);
        z->wlen += take;
        /* one chunk */
        size_t cstart_cur = z->cur;
        size_t ulimit = CZ_X_L2_UMAX;
        size_t stop = z->wlen;
        if (stop - z->cur > ulimit) stop = z->cur + ulimit;
        if (need_state) {
            cz_x_model_reset(&z->m);
        }
        cz_x_rce_reset(&z->rc);
        size_t got = cz_x_lzenc_run(z, stop, CZ_X_L2_CMAX - 64);
        cz_x_rce_flush(&z->rc);
        size_t csz = z->rc.on;
        if (csz >= got || csz > CZ_X_L2_CMAX) {
            /* stored as it is, in pieces of at most 64 KiB */
            size_t off = 0;
            while (off < got) {
                size_t k = got - off < 65536 ? got - off : 65536;
                uint8_t h[3] = {(uint8_t)(first ? 1 : 2), (uint8_t)((k - 1) >> 8), (uint8_t)(k - 1)};
                cz_x_buf_put(out, h, 3);
                cz_x_buf_put(out, z->win + cstart_cur + off, k);
                off += k;
                first = 0;
            }
            need_state = 1;
        } else {
            uint8_t ctrl = first ? 0xe0 : need_props ? 0xc0 : need_state ? 0xa0 : 0x80;
            size_t u = got - 1, c = csz - 1;
            uint8_t h[6] = {(uint8_t)(ctrl | (u >> 16)), (uint8_t)(u >> 8), (uint8_t)u,
                            (uint8_t)(c >> 8), (uint8_t)c, props};
            cz_x_buf_put(out, h, (ctrl >= 0xc0) ? 6 : 5);
            cz_x_buf_put(out, z->rc.out, csz);
            first = 0;
            need_props = 0;
            need_state = 0;
        }
        done += got;
    }
    uint8_t end = 0;
    cz_x_buf_put(out, &end, 1);
    cz_x_lzenc_free(z);
    free(z);
    return 0;
}

/* LZMA1 of the whole of DATA, ending with the end marker */
static int cz_x_lzma1_encode(const struct cz_x_filter *f, const uint8_t *data, size_t n, struct cz_x_buf *out) {
    int depth, nice;
    cz_x_effort(f, &depth, &nice);
    size_t hist = f->dict < (4u << 20) ? f->dict : (4u << 20);
    struct cz_x_lzenc *z = (struct cz_x_lzenc *)calloc(1, sizeof *z);
    if (!z || cz_x_lzenc_init(z, hist, depth, nice, f->lc, f->lp, f->pb) != 0) { free(z); return -1; }
    z->fast = f->fast;
    size_t done = 0;
    while (done < n) {
        cz_x_lzenc_slide(z);
        size_t pending = z->wlen - z->cur;
        size_t room = z->wcap - z->wlen;
        size_t take = n - done - pending < room ? n - done - pending : room;
        memcpy(z->win + z->wlen, data + done + pending, take);
        z->wlen += take;
        size_t got = cz_x_lzenc_run(z, z->wlen, 0);
        done += got;
        cz_x_buf_put(out, z->rc.out, z->rc.on);
        z->rc.on = 0;
    }
    cz_x_enc_eopm(z, z->cur);
    cz_x_rce_flush(&z->rc);
    cz_x_buf_put(out, z->rc.out, z->rc.on);
    cz_x_lzenc_free(z);
    free(z);
    return 0;
}

/* the filters before LZMA, on a whole block in place */
static void cz_x_prefilter(struct cz_x_filter *f, int nf, uint8_t *data, size_t n) {
    for (int i = 0; i < nf - 1; i++) {
        cz_x_filter_reset(&f[i]);
        cz_x_bcj(&f[i], data, n, 1);
    }
}

static size_t cz_x_varint_put(uint8_t *p, uint64_t v) {
    size_t i = 0;
    while (v >= 0x80) { p[i++] = (uint8_t)(v | 0x80); v >>= 7; }
    p[i++] = (uint8_t)v;
    return i;
}

/* the filter flags of a chain */
static size_t cz_x_filter_flags(const struct cz_x_filter *f, int nf, uint8_t *p) {
    size_t k = 0;
    for (int i = 0; i < nf; i++) {
        k += cz_x_varint_put(p + k, f[i].id);
        if (f[i].id == CZ_X_F_LZMA2) {
            p[k++] = 1;
            p[k++] = cz_x_dict_prop(f[i].dict);
        } else if (f[i].id == CZ_X_F_DELTA) {
            p[k++] = 1;
            p[k++] = (uint8_t)(f[i].dist - 1);
        } else if (f[i].start) {
            p[k++] = 4;
            for (int j = 0; j < 4; j++) p[k++] = (uint8_t)(f[i].start >> (8 * j));
        } else p[k++] = 0;
    }
    return k;
}

struct cz_x_xzw {
    struct cz_x_out *o;
    int check;
    uint64_t *rec;
    size_t nrec, caprec;
};

static void cz_x_xz_header(struct cz_x_xzw *w) {
    uint8_t h[12];
    memcpy(h, cz_x_xz_magic, 6);
    h[6] = 0;
    h[7] = (uint8_t)w->check;
    uint32_t c = cz_crc32(0, h + 6, 2);
    for (int i = 0; i < 4; i++) h[8 + i] = (uint8_t)(c >> (8 * i));
    cz_x_put(w->o, h, 12);
}

/* one block of DATA (filtered in place); SIZES: the threaded layout's
 * block header with both sizes */
static size_t cz_x_varint_len(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) { v >>= 7; n++; }
    return n;
}

/* the threaded encoder writes each block's header after the block, in
 * room it set aside before: the room the largest sizes need -- the block
 * size, and the most its compressed form can take (LZMA2 stored, a header
 * of three bytes a 64 KiB, rounded to four, and liblzma's 92 for headers) */
static size_t cz_x_reserved_header(uint64_t bs, const struct cz_x_filter *f, int nf) {
    uint8_t tmp[64];
    uint64_t l2 = bs + ((bs + 65535) / 65536) * 3 + 1;
    uint64_t bound = 92 + ((l2 + 3) & ~(uint64_t)3);
    size_t k = 2 + cz_x_varint_len(bound) + cz_x_varint_len(bs) + cz_x_filter_flags(f, nf, tmp);
    return (k + 3) & ~(size_t)3;
}

static int cz_x_xz_block(struct cz_x_xzw *w, struct cz_x_filter *f, int nf, uint8_t *data, size_t n, int sizes, uint64_t reserve_bs) {
    struct cz_x_check chk;
    cz_x_check_init(&chk, w->check);
    cz_x_check_update(&chk, data, n);
    cz_x_prefilter(f, nf, data, n);
    struct cz_x_buf cb = {0, 0, 0};
    if (cz_x_lzma2_encode(&f[nf - 1], data, n, &cb) != 0) return -1;
    uint8_t h[1024];
    size_t k = 2;
    if (sizes) {
        k += cz_x_varint_put(h + k, cb.n);
        k += cz_x_varint_put(h + k, n);
    }
    k += cz_x_filter_flags(f, nf, h + k);
    if (sizes) {
        size_t want = cz_x_reserved_header(reserve_bs, f, nf);
        while (k < want) h[k++] = 0;
    }
    while ((k + 4) % 4) h[k++] = 0;
    h[0] = (uint8_t)((k + 4) / 4 - 1);
    h[1] = (uint8_t)((nf - 1) | (sizes ? 0xc0 : 0));
    uint32_t c = cz_crc32(0, h, k);
    for (int i = 0; i < 4; i++) h[k++] = (uint8_t)(c >> (8 * i));
    cz_x_put(w->o, h, k);
    cz_x_put(w->o, cb.p, cb.n);
    uint8_t pad[4] = {0, 0, 0, 0};
    if (cb.n % 4) cz_x_put(w->o, pad, 4 - cb.n % 4);
    uint8_t cv[64];
    int cl = cz_x_check_final(&chk, cv);
    cz_x_put(w->o, cv, (size_t)cl);
    if (w->nrec + 2 > w->caprec) {
        w->caprec = w->caprec ? w->caprec * 2 : 16;
        w->rec = (uint64_t *)realloc(w->rec, w->caprec * sizeof *w->rec);
    }
    w->rec[w->nrec++] = k + cb.n + (uint64_t)cl;
    w->rec[w->nrec++] = n;
    free(cb.p);
    return 0;
}

static void cz_x_xz_finish(struct cz_x_xzw *w) {
    struct cz_x_buf ib = {0, 0, 0};
    uint8_t t[24];
    t[0] = 0;
    cz_x_buf_put(&ib, t, 1);
    cz_x_buf_put(&ib, t, cz_x_varint_put(t, w->nrec / 2));
    for (size_t i = 0; i < w->nrec; i++) cz_x_buf_put(&ib, t, cz_x_varint_put(t, w->rec[i]));
    while (ib.n % 4) { t[0] = 0; cz_x_buf_put(&ib, t, 1); }
    uint32_t c = cz_crc32(0, ib.p, ib.n);
    for (int i = 0; i < 4; i++) t[i] = (uint8_t)(c >> (8 * i));
    cz_x_buf_put(&ib, t, 4);
    cz_x_put(w->o, ib.p, ib.n);
    uint8_t f[12];
    uint32_t back = (uint32_t)(ib.n / 4 - 1);
    for (int i = 0; i < 4; i++) f[4 + i] = (uint8_t)(back >> (8 * i));
    f[8] = 0;
    f[9] = (uint8_t)w->check;
    c = cz_crc32(0, f + 4, 6);
    for (int i = 0; i < 4; i++) f[i] = (uint8_t)(c >> (8 * i));
    f[10] = 'Y';
    f[11] = 'Z';
    cz_x_put(w->o, f, 12);
    free(ib.p);
    free(w->rec);
    w->rec = 0;
    w->nrec = w->caprec = 0;
}

/* ======================================================================
 *  numbers as xz prints them
 * ====================================================================== */
/* "N B" below 10000 bytes, else KiB, MiB, ... with one decimal: the
 * value as a double divided by 1024 until it is at most 9999.9 */
static const char *cz_x_nice(uint64_t v, int also_bytes, char *buf, size_t cap) {
    static const char *const unit[5] = {"B", "KiB", "MiB", "GiB", "TiB"};
    if (v < 10000) {
        snprintf(buf, cap, "%llu B", (unsigned long long)v);
        return buf;
    }
    int u = 0;
    uint64_t div = 1;
    do {
        div *= 1024;
        u++;
    } while (u < 4 && (unsigned __int128)v * 10 > (unsigned __int128)99999 * div);
    char num[48];
    cz_d_fmt(num, sizeof num, cz_d_ratio(v, div), 0, 1);
    if (also_bytes) snprintf(buf, cap, "%s %s (%llu B)", num, unit[u], (unsigned long long)v);
    else snprintf(buf, cap, "%s %s", num, unit[u]);
    return buf;
}

/* compressed/uncompressed with three decimals, "---" when it says nothing */
static const char *cz_x_ratio(uint64_t comp, uint64_t uncomp, char *buf, size_t cap) {
    if (uncomp == 0) { snprintf(buf, cap, "---"); return buf; }
    /* above 9.999: "---" too */
    if ((unsigned __int128)comp * 1000 > (unsigned __int128)uncomp * 9999) { snprintf(buf, cap, "---"); return buf; }
    cz_d_fmt(buf, cap, cz_d_ratio(comp, uncomp), 0, 3);
    return buf;
}

static const char *cz_x_check_name(int id) {
    static char unk[24];
    switch (id) {
    case 0: return "None";
    case 1: return "CRC32";
    case 4: return "CRC64";
    case 10: return "SHA-256";
    }
    snprintf(unk, sizeof unk, "Unknown-%d", id);
    return unk;
}

/* the checks of a mask, in ID order, joined with SEP */
static void cz_x_checks(unsigned mask, const char *sep, char *buf, size_t cap) {
    buf[0] = 0;
    int first = 1;
    for (int i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        size_t l = strlen(buf);
        snprintf(buf + l, cap - l, "%s%s", first ? "" : sep, cz_x_check_name(i));
        first = 0;
    }
}

/* MiB rounded up, as the memory columns show it */
static uint64_t cz_x_mib_up(uint64_t b) {
    return (b + (1u << 20) - 1) >> 20;
}

/* the dictionary size as the chain line spells it */
static void cz_x_dict_str(uint32_t d, char *buf, size_t cap) {
    if (d % (1u << 30) == 0) snprintf(buf, cap, "%uGiB", d >> 30);
    else if (d % (1u << 20) == 0) snprintf(buf, cap, "%uMiB", d >> 20);
    else if (d % (1u << 10) == 0) snprintf(buf, cap, "%uKiB", d >> 10);
    else snprintf(buf, cap, "%u", d);
}

static const char *cz_x_filter_name(uint64_t id) {
    switch (id) {
    case CZ_X_F_DELTA: return "delta";
    case CZ_X_F_X86: return "x86";
    case CZ_X_F_PPC: return "powerpc";
    case CZ_X_F_IA64: return "ia64";
    case CZ_X_F_ARM: return "arm";
    case CZ_X_F_ARMTHUMB: return "armthumb";
    case CZ_X_F_SPARC: return "sparc";
    case CZ_X_F_ARM64: return "arm64";
    case CZ_X_F_RISCV: return "riscv";
    case CZ_X_F_LZMA2: return "lzma2";
    case CZ_X_F_LZMA1: return "lzma1";
    }
    return "?";
}

/* "--x86 --lzma2=dict=8MiB": a chain as -lvv shows it (FULL: every LZMA
 * option, as -vv's "Filter chain:" line) */
static void cz_x_chain_str(const struct cz_x_filter *f, int nf, int full, char *buf, size_t cap) {
    buf[0] = 0;
    for (int i = 0; i < nf; i++) {
        size_t l = strlen(buf);
        const char *sp = i ? " " : "";
        if (f[i].id == CZ_X_F_LZMA2 || f[i].id == CZ_X_F_LZMA1) {
            char ds[32];
            cz_x_dict_str(f[i].dict, ds, sizeof ds);
            if (full) {
                static const char *const mfn[15] = {"", "", "", "hc3", "hc4", "", "", "", "", "", "", "", "bt2", "bt3", "bt4"};
                snprintf(buf + l, cap - l, "%s--%s=dict=%s,lc=%d,lp=%d,pb=%d,mode=%s,nice=%d,mf=%s,depth=%d", sp,
                         cz_x_filter_name(f[i].id), ds, f[i].lc, f[i].lp, f[i].pb, f[i].fast ? "fast" : "normal",
                         f[i].nice, mfn[f[i].mf], f[i].depth);
            } else {
                snprintf(buf + l, cap - l, "%s--%s=dict=%s", sp, cz_x_filter_name(f[i].id), ds);
            }
        } else if (f[i].id == CZ_X_F_DELTA) {
            snprintf(buf + l, cap - l, "%s--delta=dist=%d", sp, f[i].dist);
        } else if (f[i].start) {
            snprintf(buf + l, cap - l, "%s--%s=start=%u", sp, cz_x_filter_name(f[i].id), f[i].start);
        } else {
            snprintf(buf + l, cap - l, "%s--%s", sp, cz_x_filter_name(f[i].id));
        }
    }
}

/* ======================================================================
 *  -l: a file read from its end, stream by stream
 * ====================================================================== */
struct cz_x_lblock {
    uint64_t number_in_file, number_in_stream;
    uint64_t comp_off, uncomp_off, total_size, uncomp_size;
    uint64_t unpadded;
    int hsize, flags_c, flags_u;
    uint64_t comp_size;     /* compressed data alone */
    uint64_t mem;
    char filters[256];
    uint8_t checkval[64];
    int checklen;
    int minver;             /* the XZ Utils version that reads it: 50000002, 50040002 ... */
};

struct cz_x_lstream {
    uint64_t number, nblocks, comp_off, uncomp_off, comp_size, uncomp_size, padding;
    int check;
    struct cz_x_lblock *blocks;
};

struct cz_x_lfile {
    struct cz_x_lstream *st;
    int nst;
    uint64_t comp, uncomp, padding, blocks;
    unsigned checks;
    uint64_t mem;
    int all_sizes;
    int minver;
};

static int cz_x_pread(int fd, uint64_t off, uint8_t *p, size_t n) {
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static uint64_t cz_x_le32(const uint8_t *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24;
}

static int cz_x_mvarint(const uint8_t *p, size_t n, size_t *pos, uint64_t *v) {
    uint64_t x = 0;
    for (int i = 0; i < 9; i++) {
        if (*pos >= n) return -1;
        uint8_t b = p[(*pos)++];
        x |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) {
            if (b == 0 && i > 0) return -1;
            *v = x;
            return 0;
        }
    }
    return -1;
}

static void cz_x_lfile_free(struct cz_x_lfile *lf) {
    for (int i = 0; i < lf->nst; i++) free(lf->st[i].blocks);
    free(lf->st);
    memset(lf, 0, sizeof *lf);
}

/* CZ_X_E_* */
static int cz_x_list_parse(int fd, uint64_t size, int detail, struct cz_x_lfile *lf) {
    memset(lf, 0, sizeof *lf);
    lf->all_sizes = 1;
    lf->minver = 50000002;
    uint8_t hdr[12];
    if (size < 32) return -1;      /* "Too small" */
    if (cz_x_pread(fd, 0, hdr, 12) != 0) return CZ_X_E_DATA;
    if (memcmp(hdr, cz_x_xz_magic, 6) != 0) return CZ_X_E_FORMAT;
    uint64_t pos = size;
    struct cz_x_lstream *st = 0;
    int nst = 0, capst = 0;
    uint64_t padding = 0;
    while (pos > 0) {
        if (pos < 12) goto corrupt;
        uint8_t w[4];
        if (cz_x_pread(fd, pos - 4, w, 4) != 0) goto corrupt;
        if (!(w[0] | w[1] | w[2] | w[3])) {
            padding += 4;
            pos -= 4;
            continue;
        }
        if (pos < 24) goto corrupt;
        uint8_t ft[12];
        if (cz_x_pread(fd, pos - 12, ft, 12) != 0) goto corrupt;
        if (ft[10] != 'Y' || ft[11] != 'Z') goto corrupt;
        if (cz_crc32(0, ft + 4, 6) != (uint32_t)cz_x_le32(ft)) goto corrupt;
        if (ft[8] != 0 || (ft[9] & 0xf0)) { free(st); return CZ_X_E_OPTIONS; }
        uint64_t isize = (cz_x_le32(ft + 4) + 1) * 4;
        if (isize > pos - 24) goto corrupt;
        uint64_t istart = pos - 12 - isize;
        uint8_t *ib = (uint8_t *)malloc((size_t)isize);
        if (!ib || cz_x_pread(fd, istart, ib, (size_t)isize) != 0) { free(ib); goto corrupt; }
        if (ib[0] != 0 || cz_crc32(0, ib, (size_t)isize - 4) != (uint32_t)cz_x_le32(ib + isize - 4)) { free(ib); goto corrupt; }
        size_t ip = 1;
        uint64_t nrec;
        if (cz_x_mvarint(ib, (size_t)isize - 4, &ip, &nrec) != 0) { free(ib); goto corrupt; }
        uint64_t *unp = (uint64_t *)malloc((size_t)(nrec ? nrec : 1) * 2 * sizeof(uint64_t));
        uint64_t blocks_total = 0, unc_total = 0;
        for (uint64_t i = 0; i < nrec; i++) {
            if (cz_x_mvarint(ib, (size_t)isize - 4, &ip, &unp[2 * i]) != 0 ||
                cz_x_mvarint(ib, (size_t)isize - 4, &ip, &unp[2 * i + 1]) != 0 || unp[2 * i] == 0) {
                free(unp); free(ib); goto corrupt;
            }
            blocks_total += (unp[2 * i] + 3) & ~(uint64_t)3;
            unc_total += unp[2 * i + 1];
        }
        while (ip < isize - 4) if (ib[ip++] != 0) { free(unp); free(ib); goto corrupt; }
        free(ib);
        if (blocks_total + 12 > istart) { free(unp); goto corrupt; }
        uint64_t sstart = istart - blocks_total - 12;
        uint8_t sh[12];
        if (cz_x_pread(fd, sstart, sh, 12) != 0 || memcmp(sh, cz_x_xz_magic, 6) != 0) { free(unp); goto corrupt; }
        if (cz_crc32(0, sh + 6, 2) == (uint32_t)cz_x_le32(sh + 8) && (sh[6] != 0 || (sh[7] & 0xf0))) {
            free(unp);
            for (int i = 0; i < nst; i++) free(st[i].blocks);
            free(st);
            return CZ_X_E_OPTIONS;
        }
        if (cz_crc32(0, sh + 6, 2) != (uint32_t)cz_x_le32(sh + 8)) { free(unp); goto corrupt; }
        if (sh[6] != ft[8] || sh[7] != ft[9]) { free(unp); goto corrupt; }
        if (nst == capst) { capst = capst ? capst * 2 : 4; st = (struct cz_x_lstream *)realloc(st, (size_t)capst * sizeof *st); }
        memmove(st + 1, st, (size_t)nst * sizeof *st);
        nst++;
        struct cz_x_lstream *s = &st[0];
        memset(s, 0, sizeof *s);
        s->check = sh[7] & 0x0f;
        s->nblocks = nrec;
        s->comp_off = sstart;
        s->comp_size = pos - sstart;
        s->uncomp_size = unc_total;
        s->padding = padding;
        s->blocks = (struct cz_x_lblock *)calloc((size_t)(nrec ? nrec : 1), sizeof(struct cz_x_lblock));
        uint64_t off = sstart + 12, uoff = 0;
        int csz = cz_x_check_size(s->check);
        for (uint64_t i = 0; i < nrec; i++) {
            struct cz_x_lblock *b = &s->blocks[i];
            b->number_in_stream = i + 1;
            b->comp_off = off;
            b->uncomp_off = uoff;
            b->unpadded = unp[2 * i];
            b->total_size = (unp[2 * i] + 3) & ~(uint64_t)3;
            b->uncomp_size = unp[2 * i + 1];
            b->minver = 50000002;
            if (detail) {
                uint8_t h[1024];
                if (cz_x_pread(fd, off, h, 1) != 0 || h[0] == 0) { free(unp); goto corrupt_st; }
                int hs = (h[0] + 1) * 4;
                if ((uint64_t)hs + (uint64_t)csz >= b->unpadded || cz_x_pread(fd, off, h, (size_t)hs) != 0) { free(unp); goto corrupt_st; }
                if (cz_crc32(0, h, (size_t)hs - 4) != (uint32_t)cz_x_le32(h + hs - 4)) { free(unp); goto corrupt_st; }
                b->hsize = hs;
                b->flags_c = (h[1] & 0x40) != 0;
                b->flags_u = (h[1] & 0x80) != 0;
                if (!(b->flags_c && b->flags_u)) lf->all_sizes = 0;
                size_t hp = 2;
                uint64_t v;
                if (b->flags_c && cz_x_mvarint(h, (size_t)hs - 4, &hp, &v) != 0) { free(unp); goto corrupt_st; }
                if (b->flags_u && cz_x_mvarint(h, (size_t)hs - 4, &hp, &v) != 0) { free(unp); goto corrupt_st; }
                struct cz_x_filter f[4];
                int nf = (h[1] & 3) + 1;
                int pr = cz_x_parse_filters(h, (size_t)hs - 4, &hp, f, nf);
                if (pr != CZ_X_E_OK) { free(unp); goto corrupt_st; }
                b->comp_size = b->unpadded - (uint64_t)hs - (uint64_t)csz;
                b->mem = cz_x_chain_mem(f, nf);
                for (int k = 0; k < nf; k++) if (f[k].id == CZ_X_F_ARM64) b->minver = 50040002;
                cz_x_chain_str(f, nf, 0, b->filters, sizeof b->filters);
                b->checklen = csz;
                if (csz && cz_x_pread(fd, off + b->total_size - (uint64_t)csz, b->checkval, (size_t)csz) != 0) { free(unp); goto corrupt_st; }
                /* a CRC is shown as the number it is (stored little end
                 * first); a hash as its bytes */
                if (s->check == CZ_X_CHECK_CRC32 || s->check == CZ_X_CHECK_CRC64) {
                    for (int j = 0; j < csz / 2; j++) {
                        uint8_t t = b->checkval[j];
                        b->checkval[j] = b->checkval[csz - 1 - j];
                        b->checkval[csz - 1 - j] = t;
                    }
                }
                if (b->mem > lf->mem) lf->mem = b->mem;
                if (b->minver > lf->minver) lf->minver = b->minver;
            }
            off += b->total_size;
            uoff += b->uncomp_size;
        }
        free(unp);
        pos = sstart;
        padding = 0;
        continue;
    corrupt_st:
        goto corrupt;
    }
    /* numbering, offsets within the file */
    {
        uint64_t uoff = 0, bnum = 0;
        for (int i = 0; i < nst; i++) {
            st[i].number = (uint64_t)i + 1;
            st[i].uncomp_off = uoff;
            for (uint64_t k = 0; k < st[i].nblocks; k++) {
                st[i].blocks[k].uncomp_off += uoff;
                st[i].blocks[k].number_in_file = ++bnum;
            }
            uoff += st[i].uncomp_size;
            lf->comp += st[i].comp_size + st[i].padding;
            lf->uncomp += st[i].uncomp_size;
            lf->padding += st[i].padding;
            lf->blocks += st[i].nblocks;
            lf->checks |= 1u << st[i].check;
        }
    }
    lf->st = st;
    lf->nst = nst;
    return CZ_X_E_OK;
corrupt:
    for (int i = 0; i < nst; i++) free(st[i].blocks);
    free(st);
    return CZ_X_E_DATA;
}

/* ======================================================================
 *  the xz program
 * ====================================================================== */
#define CZ_X_M_COMPRESS 0
#define CZ_X_M_DECOMPRESS 1
#define CZ_X_M_TEST 2
#define CZ_X_M_LIST 3
#define CZ_X_FMT_AUTO 0
#define CZ_X_FMT_XZ 1
#define CZ_X_FMT_LZMA 2
#define CZ_X_FMT_LZIP 3
#define CZ_X_FMT_RAW 4

static struct {
    const char *prog;
    int mode, format, check, check_set;
    int keep, force, to_stdout, no_sync, single_stream, no_sparse, ignore_check;
    int quiet, verbose, no_warn, robot;
    const char *suffix;
    int preset, extreme;
    struct cz_x_filter chain[4];
    int nchain, chain_kind;     /* 0 the preset, 1 options, 2 --filters */
    long threads;
    int threads_plus;        /* -T+1: threaded with one thread */
    uint64_t block_size;
    uint64_t blist[256];
    int nblist;
    uint64_t memlimit_c, memlimit_d;
    uint64_t memlimit_mt;    /* 0: the machine's default (a quarter of RAM) */
    int no_adjust;
    int exit_status;
    int flush_timeout;
    const char *files_from;
    int files0, files_given;
    /* the file at hand */
    const char *src_name;
    char *dst_name;
    int dst_fd;
    int dst_made;
    struct stat src_st;
    uint64_t in_bytes, out_bytes;
    /* -l totals */
    struct cz_x_lfile tot;
    int list_files, list_done;
    uint64_t tot_streams, tot_blocks, tot_comp, tot_uncomp, tot_padding;
    unsigned tot_checks;
    uint64_t tot_mem;
    int tot_all_sizes, tot_minver;
    int ninputs;
} cz_xz;

/* single-threaded mode: -T1 (not -T+1) */
#define CZ_X_SINGLE() (cz_xz.threads == 1 && !cz_xz.threads_plus)

static void cz_x_error(const char *name, const char *msg) {
    fflush(stdout);
    if (cz_xz.quiet < 2) {
        if (name) fprintf(stderr, "%s: %s: %s\n", cz_xz.prog, name, msg);
        else fprintf(stderr, "%s: %s\n", cz_xz.prog, msg);
    }
    cz_xz.exit_status = 1;
}

static void cz_x_warning(const char *name, const char *msg) {
    fflush(stdout);
    if (cz_xz.quiet < 1) {
        if (name) fprintf(stderr, "%s: %s: %s\n", cz_xz.prog, name, msg);
        else fprintf(stderr, "%s: %s\n", cz_xz.prog, msg);
    }
    if (!cz_xz.no_warn && cz_xz.exit_status == 0) cz_xz.exit_status = 2;
}

static void cz_x_fatal(const char *msg) __attribute__((noreturn));
static void cz_x_fatal(const char *msg) {
    fflush(stdout);
    if (cz_xz.quiet < 2) fprintf(stderr, "%s: %s\n", cz_xz.prog, msg);
    exit(1);
}

/* ---- memory: the machine, the limits, what an encoder needs ----
 *
 * As xz 5.8 has them (logs/b34/xzmem*.py, measured against it in WSL).
 * The machine's RAM and processor threads are its own: MemTotal and the
 * "processor" lines of /proc on tobyOS, sysconf on the host. */

static uint64_t cz_x_physmem(void) {
    static uint64_t ram;
    if (ram) return ram;
#ifdef SU_HOST
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) ram = (uint64_t)pages * (uint64_t)psz;
#else
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            unsigned long long kb;
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) { ram = (uint64_t)kb * 1024; break; }
        }
        fclose(f);
    }
#endif
    return ram;
}

static uint64_t cz_x_ncpu(void) {
#ifdef SU_HOST
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint64_t)n : 1;
#else
    FILE *f = fopen("/proc/cpuinfo", "r");
    uint64_t n = 0;
    char line[256];
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "processor", 9)) n++;
        fclose(f);
    }
    return n ? n : 1;
#endif
}

/* the threaded decompressor's limit by default, and -T0's: a quarter of RAM */
static uint64_t cz_x_quarter_ram(void) { return cz_x_physmem() / 4; }

/* the threaded decompressor's limit in force: its own or the default, no
 * more than the decompressor's limit (--info-memory shows that) */
static uint64_t cz_x_mt_limit(void) {
    uint64_t v = cz_xz.memlimit_mt ? cz_xz.memlimit_mt : cz_x_quarter_ram();
    if (cz_xz.memlimit_d && cz_xz.memlimit_d < v) v = cz_xz.memlimit_d;
    return v;
}

/* a limit as a notice says it: MiB rounded up; as "The limit is" says it,
 * in bytes under 1 MiB */
static void cz_x_lim_mib(uint64_t v, char *buf, size_t cap) {
    snprintf(buf, cap, "%llu MiB", (unsigned long long)cz_x_mib_up(v));
}
static void cz_x_lim_str(uint64_t v, char *buf, size_t cap) {
    if (v < (1u << 20)) snprintf(buf, cap, "%llu B", (unsigned long long)v);
    else cz_x_lim_mib(v, buf, cap);
}

static uint64_t cz_x_num(const char *name, const char *s, uint64_t min, uint64_t max);

/* a memory limit as xz takes one: "max", a share of RAM ("70%", 1 to 100,
 * rounded down), or a size */
static uint64_t cz_x_limit(const char *name, const char *s) {
    if (!strcmp(s, "max")) return UINT64_MAX;
    size_t l = strlen(s);
    if (l && s[l - 1] == '%') {
        char body[256], nm[64];
        snprintf(body, sizeof body, "%.*s", (int)(l - 1), s);
        snprintf(nm, sizeof nm, "%s%%", name);
        uint64_t p = cz_x_num(nm, body, 1, 100);
        return cz_x_physmem() * p / 100;
    }
    return cz_x_num(name, s, 0, UINT64_MAX);
}

static void cz_x_info_memory(void) {
    uint64_t ram = cz_x_physmem(), mt = cz_x_mt_limit(), t0 = cz_x_quarter_ram();
    if (cz_xz.robot) {
        printf("%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n", (unsigned long long)ram,
               (unsigned long long)cz_xz.memlimit_c, (unsigned long long)cz_xz.memlimit_d,
               (unsigned long long)mt, (unsigned long long)t0,
               (unsigned long long)cz_x_ncpu());
        return;
    }
    char b[5][64];
    uint64_t v[5] = { ram, cz_xz.memlimit_c, cz_xz.memlimit_d, mt, t0 };
    for (int i = 0; i < 5; i++) {
        if (i > 0 && (v[i] == 0 || v[i] == UINT64_MAX)) snprintf(b[i], sizeof b[i], "Disabled");
        else snprintf(b[i], sizeof b[i], "%llu MiB (%llu B)",
                      (unsigned long long)cz_x_mib_up(v[i]), (unsigned long long)v[i]);
    }
    printf("Hardware information:\n"
           "  Amount of physical memory (RAM):  %s\n"
           "  Number of processor threads:      %llu\n\n"
           "Memory usage limits:\n"
           "  Compression:                      %s\n"
           "  Decompression:                    %s\n"
           "  Multi-threaded decompression:     %s\n"
           "  Default for -T0:                  %s\n",
           b[0], (unsigned long long)cz_x_ncpu(), b[1], b[2], b[3], b[4]);
}

static uint64_t cz_x_enc_mem(const struct cz_x_filter *f, int n);

/* the threaded encoder's block: --block-size's, else three dictionaries,
 * at least 1 MiB */
static uint64_t cz_x_mt_block(const struct cz_x_filter *f, int nf) {
    if (cz_xz.block_size) return cz_xz.block_size;
    uint64_t b = (uint64_t)f[nf - 1].dict * 3;
    return b < (1u << 20) ? 1u << 20 : b;
}

/* the threaded encoder's memory with N threads, as xz counts it (bisected,
 * logs/b34/xzmemmt.py and xzmemblk.py): 33336 bytes, and for each thread
 * the single-threaded encoder's, its block, and two buffers of the block's
 * bound -- the block, 3 bytes a 64 KiB chunk and 429 more, to 4 bytes */
static uint64_t cz_x_mt_mem(const struct cz_x_filter *f, int nf, uint64_t n) {
    uint64_t b = cz_x_mt_block(f, nf);
    uint64_t bound = (b + 3 * ((b + 65535) / 65536) + 429 + 3) & ~(uint64_t)3;
    return 33336 + n * (cz_x_enc_mem(f, nf) + b + 2 * bound);
}

static void cz_x_mem_too_low(uint64_t need) __attribute__((noreturn));
static void cz_x_mem_too_low(uint64_t need) {
    fflush(stdout);
    char lim[64];
    cz_x_lim_str(cz_xz.memlimit_c, lim, sizeof lim);
    if (cz_xz.quiet < 2) {
        fprintf(stderr, "%s: Memory usage limit is too low for the given filter setup.\n", cz_xz.prog);
        fprintf(stderr, "%s: %llu MiB of memory is required. The limit is %s.\n", cz_xz.prog,
                (unsigned long long)cz_x_mib_up(need), lim);
    }
    exit(1);
}

static void cz_x_chain_str(const struct cz_x_filter *f, int n, int full, char *buf, size_t cap);
static uint64_t cz_x_chain_mem(const struct cz_x_filter *f, int n);
static void cz_x_effective_chain(struct cz_x_filter *f, int *nf);

/* compression's memory, set up once before any file as xz's is: -vv's
 * lines; then, over the limit (-M, --memlimit-compress; -T0's own quarter
 * of RAM when none is set), fewer threads (said at -vv), single-threaded
 * mode, and a dictionary smaller by 1 MiB at a time from its whole MiB --
 * none of the last two with --no-adjust or a raw stream -- or the error.
 * A dictionary made smaller is kept as the chain from then on. */
static void cz_x_mem_setup(void) {
    struct cz_x_filter f[4];
    int nf;
    cz_x_effective_chain(f, &nf);
    int mt = cz_xz.format == CZ_X_FMT_XZ && !CZ_X_SINGLE();
    uint64_t n = cz_xz.threads == 0 ? cz_x_ncpu() : (uint64_t)cz_xz.threads;
    uint64_t limit = cz_xz.memlimit_c ? cz_xz.memlimit_c : UINT64_MAX;
    uint64_t use = mt ? cz_x_mt_mem(f, nf, n) : cz_x_enc_mem(f, nf);
    char lim[64], mib[64];
    cz_x_lim_mib(limit, mib, sizeof mib);
    if (cz_xz.verbose >= 2 && cz_xz.quiet < 1) {
        char s[512];
        cz_x_chain_str(f, nf, 1, s, sizeof s);
        fprintf(stderr, "%s: Filter chain: %s\n", cz_xz.prog, s);
        if (mt) fprintf(stderr, "%s: Using up to %llu threads.\n", cz_xz.prog, (unsigned long long)n);
        if (cz_xz.memlimit_c && cz_xz.memlimit_c != UINT64_MAX) {   /* "max" is none */
            cz_x_lim_str(limit, lim, sizeof lim);
            fprintf(stderr, "%s: %llu MiB of memory is required. The limit is %s.\n", cz_xz.prog,
                    (unsigned long long)cz_x_mib_up(use), lim);
        } else {
            fprintf(stderr, "%s: %llu MiB of memory is required. The limiter is disabled.\n",
                    cz_xz.prog, (unsigned long long)cz_x_mib_up(use));
        }
        fprintf(stderr, "%s: Decompression will need %llu MiB of memory.\n", cz_xz.prog,
                (unsigned long long)cz_x_mib_up(cz_x_chain_mem(f, nf)));
    }
    if (mt) {
        int soft = cz_xz.threads == 0 && !cz_xz.memlimit_c;
        uint64_t hold = soft ? cz_x_quarter_ram() : limit;
        if (use > hold) {
            uint64_t k = n;
            while (k > 1 && cz_x_mt_mem(f, nf, k) > hold) k--;
            if (cz_x_mt_mem(f, nf, k) <= hold || soft) {
                if (k != n && cz_xz.verbose >= 2 && cz_xz.quiet < 1) {
                    char h[64];
                    cz_x_lim_mib(hold, h, sizeof h);
                    fprintf(stderr, "%s: Reduced the number of threads from %llu to %llu to not "
                            "exceed the memory usage limit of %s\n", cz_xz.prog,
                            (unsigned long long)n, (unsigned long long)k, h);
                }
                return;
            }
            if (cz_xz.no_adjust) cz_x_mem_too_low(cz_x_mt_mem(f, nf, 1));
            if (cz_xz.quiet < 1)
                fprintf(stderr, "%s: Switching to single-threaded mode to not exceed the memory "
                        "usage limit of %s\n", cz_xz.prog, mib);
            cz_xz.threads = 1;
            cz_xz.threads_plus = 0;
            use = cz_x_enc_mem(f, nf);
        } else {
            return;
        }
    }
    if (use <= limit) return;
    if (cz_xz.no_adjust || cz_xz.format == CZ_X_FMT_RAW) cz_x_mem_too_low(use);
    struct cz_x_filter *lz = &f[nf - 1];
    uint32_t orig = lz->dict, d = orig & ~((1u << 20) - 1);
    while (d >= (1u << 20)) {
        lz->dict = d;
        use = cz_x_enc_mem(f, nf);
        if (use <= limit) break;
        d -= 1u << 20;
    }
    if (d < (1u << 20)) cz_x_mem_too_low(use);
    if (cz_xz.quiet < 1)
        fprintf(stderr, "%s: Adjusted LZMA%d dictionary size from %u MiB to %u MiB to not exceed "
                "the memory usage limit of %s\n", cz_xz.prog, lz->id == CZ_X_F_LZMA1 ? 1 : 2,
                orig >> 20, d >> 20, mib);
    memcpy(cz_xz.chain, f, sizeof cz_xz.chain);
    cz_xz.nchain = nf;
}

static void cz_x_try_help(void) __attribute__((noreturn));
static void cz_x_try_help(void) {
    fflush(stdout);
    if (cz_xz.quiet < 1) fprintf(stderr, "%s: Try '%s --help' for more information.\n", cz_xz.prog, cz_xz.prog);
    exit(1);
}

/* ---- writing the output ---- */
static int cz_x_wr_err;

static void cz_x_write_out(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    cz_xz.out_bytes += n;
    if (cz_x_wr_err || cz_xz.mode == CZ_X_M_TEST) return;
    int e = cz_write_all(cz_xz.dst_fd, p, n);
    if (e) cz_x_wr_err = e;
}

/* ---- the filter chain from the options ---- */
static void cz_x_default_chain(struct cz_x_filter *f, int *nf) {
    memset(f, 0, sizeof *f);
    f->id = cz_xz.format == CZ_X_FMT_LZMA ? CZ_X_F_LZMA1 : CZ_X_F_LZMA2;
    cz_x_lzma_preset(f, cz_xz.preset, cz_xz.extreme);
    *nf = 1;
}

static void cz_x_effective_chain(struct cz_x_filter *f, int *nf) {
    if (cz_xz.nchain) {
        memcpy(f, cz_xz.chain, sizeof cz_xz.chain);
        *nf = cz_xz.nchain;
    } else cz_x_default_chain(f, nf);
}

/* what liblzma refuses of a chain, said before any file is touched */
static void cz_x_validate_chain(void) {
    struct cz_x_filter f[4];
    int nf;
    cz_x_effective_chain(f, &nf);
    int last = nf - 1;
    int is_lz_last = f[last].id == CZ_X_F_LZMA1 || f[last].id == CZ_X_F_LZMA2;
    if (cz_xz.format == CZ_X_FMT_LZMA) {
        if (nf != 1 || f[0].id != CZ_X_F_LZMA1) cz_x_fatal("The .lzma format supports only the LZMA1 filter");
        return;
    }
    for (int i = 0; i < nf; i++) {
        if (f[i].id == CZ_X_F_LZMA1 && cz_xz.format == CZ_X_FMT_XZ)
            cz_x_fatal("LZMA1 cannot be used with the .xz format");
    }
    for (int i = 0; i < last; i++)
        if (f[i].id == CZ_X_F_LZMA1 || f[i].id == CZ_X_F_LZMA2) cz_x_fatal("Unsupported filter chain or filter options");
    if (!is_lz_last) cz_x_fatal("Unsupported options in filter chain 0");
    /* the RISC-V converter is not here (the format documents give its id,
     * not its algorithm): refused as a liblzma built without it refuses,
     * never a stream that names it over data it did not convert */
    for (int i = 0; i < nf; i++)
        if (f[i].id == CZ_X_F_RISCV) cz_x_fatal("Unsupported filter chain or filter options");
}

/* ---- names ---- */
static char *cz_x_compressed_name(const char *src) {
    const char *suf = cz_xz.suffix;
    if (!suf) suf = cz_xz.format == CZ_X_FMT_LZMA ? ".lzma" : ".xz";
    char *d = (char *)malloc(strlen(src) + strlen(suf) + 1);
    strcpy(d, src);
    strcat(d, suf);
    return d;
}

static int cz_x_ends(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a > b && strcmp(s + a - b, suf) == 0;
}

/* the suffix a compressed SRC already has, for the format at hand */
static const char *cz_x_has_suffix(const char *src) {
    static const char *const xz[] = {".xz", ".txz", 0};
    static const char *const lz[] = {".lzma", ".tlz", 0};
    if (cz_xz.suffix && cz_x_ends(src, cz_xz.suffix)) return cz_xz.suffix;
    const char *const *list = cz_xz.format == CZ_X_FMT_LZMA ? lz : cz_xz.format == CZ_X_FMT_RAW ? 0 : xz;
    for (int i = 0; list && list[i]; i++) if (cz_x_ends(src, list[i])) return list[i];
    return 0;
}

static char *cz_x_uncompressed_name(const char *src) {
    struct { const char *suf, *rep; int fmt; } tab[] = {
        {".xz", "", CZ_X_FMT_XZ}, {".txz", ".tar", CZ_X_FMT_XZ},
        {".lzma", "", CZ_X_FMT_LZMA}, {".tlz", ".tar", CZ_X_FMT_LZMA}, {".lz", "", CZ_X_FMT_LZIP},
    };
    if (cz_xz.suffix && cz_x_ends(src, cz_xz.suffix)) {
        size_t n = strlen(src) - strlen(cz_xz.suffix);
        char *d = (char *)malloc(n + 1);
        memcpy(d, src, n);
        d[n] = 0;
        return d;
    }
    if (cz_xz.format == CZ_X_FMT_RAW) return 0;
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (cz_xz.format != CZ_X_FMT_AUTO && cz_xz.format != tab[i].fmt) continue;
        if (!cz_x_ends(src, tab[i].suf)) continue;
        size_t n = strlen(src) - strlen(tab[i].suf);
        char *d = (char *)malloc(n + strlen(tab[i].rep) + 1);
        memcpy(d, src, n);
        strcpy(d + n, tab[i].rep);
        return d;
    }
    return 0;
}

/* ---- one input ---- */
static void cz_x_summary(const char *name) {
    char a[64], b[64], r[32];
    uint64_t comp = cz_xz.mode == CZ_X_M_COMPRESS ? cz_xz.out_bytes : cz_xz.in_bytes;
    uint64_t unc = cz_xz.mode == CZ_X_M_COMPRESS ? cz_xz.in_bytes : cz_xz.out_bytes;
    cz_x_nice(comp, 0, a, sizeof a);
    cz_x_nice(unc, 0, b, sizeof b);
    if (unc == 0 || (unsigned __int128)comp * 1000 > (unsigned __int128)unc * 9999)
        fprintf(stderr, "%s: %s / %s > 9.999\n", name, a, b);
    else {
        cz_d_fmt(r, sizeof r, cz_d_ratio(comp, unc), 0, 3);
        fprintf(stderr, "%s: %s / %s = %s\n", name, a, b, r);
    }
}

static int cz_x_read_all(int fd, uint8_t **out, size_t *n) {
    size_t cap = 65536, len = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) return ENOMEM;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(b, cap);
            if (!nb) { free(b); return ENOMEM; }
            b = nb;
        }
        ssize_t r = read(fd, b + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(b);
            return e;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    *out = b;
    *n = len;
    return 0;
}

static int cz_x_do_compress(int ifd) {
    uint8_t *data = 0;
    size_t n = 0;
    int e = cz_x_read_all(ifd, &data, &n);
    if (e) { cz_x_error(cz_xz.src_name, strerror(e)); return -1; }
    cz_xz.in_bytes = n;
    struct cz_x_filter f[4];
    int nf;
    cz_x_effective_chain(f, &nf);
    struct cz_x_out o = {cz_x_write_out, 0, 0};
    if (cz_xz.format == CZ_X_FMT_LZMA) {
        uint8_t h[13];
        h[0] = (uint8_t)((f[0].pb * 5 + f[0].lp) * 9 + f[0].lc);
        for (int i = 0; i < 4; i++) h[1 + i] = (uint8_t)(f[0].dict >> (8 * i));
        memset(h + 5, 0xff, 8);
        cz_x_put(&o, h, 13);
        struct cz_x_buf b = {0, 0, 0};
        cz_x_lzma1_encode(&f[0], data, n, &b);
        cz_x_put(&o, b.p, b.n);
        free(b.p);
    } else if (cz_xz.format == CZ_X_FMT_RAW) {
        cz_x_prefilter(f, nf, data, n);
        struct cz_x_buf b = {0, 0, 0};
        if (f[nf - 1].id == CZ_X_F_LZMA1) cz_x_lzma1_encode(&f[nf - 1], data, n, &b);
        else cz_x_lzma2_encode(&f[nf - 1], data, n, &b);
        cz_x_put(&o, b.p, b.n);
        free(b.p);
    } else {
        struct cz_x_xzw w = {&o, cz_xz.check, 0, 0, 0};
        cz_x_xz_header(&w);
        /* the threaded layout (the default, -T0, -T+1 and -T2 up): blocks
         * of three dictionaries or 1 MiB, each with its sizes in its
         * header; -T1: one block -- or --block-size's blocks -- with no
         * sizes, the single-threaded coder knowing none ahead (measured:
         * xz 5.8 -T1 --block-size=1000 writes 5172 bytes of 5000 random,
         * 12-byte headers) */
        uint32_t dict = f[nf - 1].dict;
        int sizes = !CZ_X_SINGLE();
        uint64_t bs = cz_xz.block_size ? cz_xz.block_size : sizes ? (uint64_t)dict * 3 : (uint64_t)n;
        if (!cz_xz.block_size && sizes && bs < (1u << 20)) bs = 1u << 20;
        uint64_t reserve = bs;
        /* blocks of at most 8 MiB here (memory), the headers as xz's */
        if (!cz_xz.block_size && bs > (8u << 20) && sizes) bs = 8u << 20;
        size_t off = 0;
        int bi = 0;
        while (off < n) {
            if (cz_xz.nblist) {
                uint64_t want = cz_xz.blist[bi < cz_xz.nblist ? bi : cz_xz.nblist - 1];
                if (bi < cz_xz.nblist) bi++;
                if (want == 0) want = n - off;
                bs = want;
                reserve = want;
            }
            size_t k = n - off < bs ? n - off : (size_t)bs;
            struct cz_x_filter g[4];
            memcpy(g, f, sizeof g);
            if (cz_x_xz_block(&w, g, nf, data + off, k, sizes, reserve) != 0) {
                free(data);
                cz_x_error(cz_xz.src_name, "Memory allocation failed");
                return -1;
            }
            off += k;
        }
        cz_x_xz_finish(&w);
    }
    free(data);
    return 0;
}

static int cz_x_detect(struct cz_in *in) {
    uint8_t h[13];
    size_t n = cz_read(in, h, 13);
    for (size_t i = n; i > 0; i--) cz_ungetc(in, h[i - 1]);
    if (cz_xz.format == CZ_X_FMT_RAW) return CZ_X_FMT_RAW;
    if ((cz_xz.format == CZ_X_FMT_AUTO || cz_xz.format == CZ_X_FMT_XZ) && n >= 6 && memcmp(h, cz_x_xz_magic, 6) == 0) return CZ_X_FMT_XZ;
    if ((cz_xz.format == CZ_X_FMT_AUTO || cz_xz.format == CZ_X_FMT_LZMA) && n >= 13 && cz_x_lzma_header_ok(h)) return CZ_X_FMT_LZMA;
    if ((cz_xz.format == CZ_X_FMT_AUTO || cz_xz.format == CZ_X_FMT_LZIP) && n >= 4 && memcmp(h, "LZIP", 4) == 0) return CZ_X_FMT_LZIP;
    return -1;
}

/* the input, of format FMT (told already), decoded to the output */
static int cz_x_do_decompress_in(struct cz_in *pin, int fmt) {
    struct cz_in in = *pin;
    struct cz_x_dctx *c = (struct cz_x_dctx *)calloc(1, sizeof *c);
    c->in = &in;
    c->out = cz_x_write_out;
    c->ignore_check = cz_xz.ignore_check;
    c->single_stream = cz_xz.single_stream;
    c->memlimit = cz_xz.memlimit_d;
    int r;
    if (fmt == CZ_X_FMT_RAW) {
        struct cz_x_filter f[4];
        int nf;
        cz_x_effective_chain(f, &nf);
        c->raw = f;
        c->nraw = nf;
        r = cz_x_raw_decode(c);
    } else if (fmt == CZ_X_FMT_XZ) {
        r = cz_x_xz_decode(c);
    } else if (fmt == CZ_X_FMT_LZIP) {
        r = cz_x_lzip_decode(c, 0);
    } else if (fmt == CZ_X_FMT_LZMA) {
        uint8_t h[13];
        cz_read(&in, h, 13);
        r = cz_x_lzma_alone_decode(c, h);
    } else r = CZ_X_E_FORMAT;
    cz_xz.in_bytes = in.total;
    if (c->unsupported_check && r == CZ_X_E_OK)
        cz_x_warning(cz_xz.src_name, "Unsupported type of integrity check; not verifying file integrity");
    free(c->m.literal);
    free(c->dict.buf);
    free(c);
    *pin = in;
    if (r != CZ_X_E_OK) {
        static const char *const msg[] = {"", "File format not recognized", "Compressed data is corrupt",
                                          "Unexpected end of input", "Unsupported options",
                                          "Memory usage limit reached"};
        cz_x_error(cz_xz.src_name, msg[r]);
        if (r == CZ_X_E_MEM && cz_xz.quiet < 2) {
            /* the amount xz names is what is in use when the block's
             * decoder is refused: its base, under 1 MiB; the limit in
             * bytes below 1 MiB, else in MiB rounded up */
            char lim[64];
            cz_x_lim_str(cz_xz.memlimit_d, lim, sizeof lim);
            fprintf(stderr, "%s: 1 MiB of memory is required. The limit is %s.\n", cz_xz.prog, lim);
        }
        return -1;
    }
    return 0;
}

/* ---- -l ---- */
static void cz_x_list_one(int fd, const char *name) {
    struct cz_x_lfile lf;
    int detail = cz_xz.verbose >= 2;
    int r = cz_x_list_parse(fd, (uint64_t)cz_xz.src_st.st_size, detail, &lf);
    if (r == -1) { cz_x_error(name, cz_xz.src_st.st_size == 0 ? "File is empty" : "Too small to be a valid .xz file"); return; }
    if (r != CZ_X_E_OK) {
        cz_x_error(name, r == CZ_X_E_FORMAT ? "File format not recognized" : r == CZ_X_E_OPTIONS ? "Unsupported options" : "Compressed data is corrupt");
        return;
    }
    char a[96], b[96], ra[32], ck[128];
    cz_xz.list_done++;
    if (cz_xz.robot) {
        cz_x_checks(lf.checks, ",", ck, sizeof ck);
        printf("name\t%s\n", name);
        printf("file\t%d\t%llu\t%llu\t%llu\t%s\t%s\t%llu\n", lf.nst, (unsigned long long)lf.blocks,
               (unsigned long long)lf.comp, (unsigned long long)lf.uncomp, cz_x_ratio(lf.comp, lf.uncomp, ra, sizeof ra),
               ck, (unsigned long long)lf.padding);
        if (cz_xz.verbose >= 1) {
            for (int i = 0; i < lf.nst; i++) {
                struct cz_x_lstream *s = &lf.st[i];
                printf("stream\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\t%s\t%llu\n", (unsigned long long)s->number,
                       (unsigned long long)s->nblocks, (unsigned long long)s->comp_off, (unsigned long long)s->uncomp_off,
                       (unsigned long long)s->comp_size, (unsigned long long)s->uncomp_size,
                       cz_x_ratio(s->comp_size, s->uncomp_size, ra, sizeof ra), cz_x_check_name(s->check),
                       (unsigned long long)s->padding);
            }
            for (int i = 0; i < lf.nst; i++) {
                struct cz_x_lstream *s = &lf.st[i];
                for (uint64_t k = 0; k < s->nblocks; k++) {
                    struct cz_x_lblock *bl = &s->blocks[k];
                    printf("block\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\t%s", (unsigned long long)s->number,
                           (unsigned long long)bl->number_in_stream, (unsigned long long)bl->number_in_file,
                           (unsigned long long)bl->comp_off, (unsigned long long)bl->uncomp_off,
                           (unsigned long long)bl->total_size, (unsigned long long)bl->uncomp_size,
                           cz_x_ratio(bl->total_size, bl->uncomp_size, ra, sizeof ra), cz_x_check_name(s->check));
                    if (detail) {
                        char cv[160] = "---";
                        if (bl->checklen) { cv[0] = 0; for (int j = 0; j < bl->checklen; j++) sprintf(cv + 2 * j, "%02x", bl->checkval[j]); }
                        printf("\t%s\t%d\t%c%c\t%llu\t%llu\t%s", cv, bl->hsize, bl->flags_c ? 'c' : '-', bl->flags_u ? 'u' : '-',
                               (unsigned long long)bl->comp_size, (unsigned long long)bl->mem, bl->filters);
                    }
                    printf("\n");
                }
            }
            if (detail) printf("summary\t%llu\t%s\t%d\n", (unsigned long long)lf.mem, lf.all_sizes ? "yes" : "no", lf.minver);
        }
    } else if (cz_xz.verbose == 0) {
        if (cz_xz.list_done == 1)
            printf("Strms  Blocks   Compressed Uncompressed  Ratio  Check   Filename\n");
        cz_x_checks(lf.checks, ",", ck, sizeof ck);
        printf("%5d %7llu  %11s %12s  %5s  %-7s %s\n", lf.nst, (unsigned long long)lf.blocks,
               cz_x_nice(lf.comp, 0, a, sizeof a), cz_x_nice(lf.uncomp, 0, b, sizeof b),
               cz_x_ratio(lf.comp, lf.uncomp, ra, sizeof ra), ck, name);
    } else {
        if (cz_xz.list_done > 1) printf("\n");
        printf("%s (%d/%d)\n", name, cz_xz.list_done + cz_xz.list_files - cz_xz.list_files, cz_xz.list_files);
        cz_x_checks(lf.checks, ", ", ck, sizeof ck);
        printf("  Streams:           %d\n", lf.nst);
        printf("  Blocks:            %llu\n", (unsigned long long)lf.blocks);
        printf("  Compressed size:   %s\n", cz_x_nice(lf.comp, 1, a, sizeof a));
        printf("  Uncompressed size: %s\n", cz_x_nice(lf.uncomp, 1, b, sizeof b));
        printf("  Ratio:             %s\n", cz_x_ratio(lf.comp, lf.uncomp, ra, sizeof ra));
        printf("  Check:             %s\n", ck);
        printf("  Stream Padding:    %s\n", cz_x_nice(lf.padding, 1, a, sizeof a));
        printf("  Streams:\n    Stream    Blocks      CompOffset    UncompOffset        CompSize      UncompSize  Ratio  Check      Padding\n");
        for (int i = 0; i < lf.nst; i++) {
            struct cz_x_lstream *s = &lf.st[i];
            printf("    %6llu %9llu %15llu %15llu %15llu %15llu  %5s  %-10s %7llu\n", (unsigned long long)s->number,
                   (unsigned long long)s->nblocks, (unsigned long long)s->comp_off, (unsigned long long)s->uncomp_off,
                   (unsigned long long)s->comp_size, (unsigned long long)s->uncomp_size,
                   cz_x_ratio(s->comp_size, s->uncomp_size, ra, sizeof ra), cz_x_check_name(s->check),
                   (unsigned long long)s->padding);
        }
        if (lf.blocks) {
            int cw = 8;
            for (int i = 0; i < lf.nst; i++) if (cz_x_check_size(lf.st[i].check) * 2 > cw) cw = cz_x_check_size(lf.st[i].check) * 2;
            printf("  Blocks:\n    Stream     Block      CompOffset    UncompOffset       TotalSize      UncompSize  Ratio  Check");
            if (detail) printf("      %-*s  Header  Flags        CompSize    MemUsage  Filters", cw, "CheckVal");
            printf("\n");
            for (int i = 0; i < lf.nst; i++) {
                struct cz_x_lstream *s = &lf.st[i];
                for (uint64_t k = 0; k < s->nblocks; k++) {
                    struct cz_x_lblock *bl = &s->blocks[k];
                    printf("    %6llu %9llu %15llu %15llu %15llu %15llu  %5s  ", (unsigned long long)s->number,
                           (unsigned long long)bl->number_in_stream, (unsigned long long)bl->comp_off,
                           (unsigned long long)bl->uncomp_off, (unsigned long long)bl->total_size,
                           (unsigned long long)bl->uncomp_size, cz_x_ratio(bl->total_size, bl->uncomp_size, ra, sizeof ra));
                    if (detail) {
                        char cv[160] = "---";
                        if (bl->checklen) { cv[0] = 0; for (int j = 0; j < bl->checklen; j++) sprintf(cv + 2 * j, "%02x", bl->checkval[j]); }
                        char mem[32];
                        snprintf(mem, sizeof mem, "%llu MiB", (unsigned long long)cz_x_mib_up(bl->mem));
                        printf("%-10s %-*s %7d  %c%c    %15llu %11s  %s\n", cz_x_check_name(s->check), cw, cv, bl->hsize,
                               bl->flags_c ? 'c' : '-', bl->flags_u ? 'u' : '-', (unsigned long long)bl->comp_size, mem,
                               bl->filters);
                    } else printf("%s\n", cz_x_check_name(s->check));
                }
            }
        }
        if (detail) {
            printf("  Memory needed:     %llu MiB\n", (unsigned long long)cz_x_mib_up(lf.mem));
            printf("  Sizes in headers:  %s\n", lf.all_sizes ? "Yes" : "No");
            printf("  Minimum XZ Utils version: %d.%d.%d\n", lf.minver / 10000000, lf.minver / 10000 % 1000, lf.minver / 10 % 1000);
        }
    }
    cz_xz.tot_streams += (uint64_t)lf.nst;
    cz_xz.tot_blocks += lf.blocks;
    cz_xz.tot_comp += lf.comp;
    cz_xz.tot_uncomp += lf.uncomp;
    cz_xz.tot_padding += lf.padding;
    cz_xz.tot_checks |= lf.checks;
    if (lf.mem > cz_xz.tot_mem) cz_xz.tot_mem = lf.mem;
    if (!lf.all_sizes) cz_xz.tot_all_sizes = 0;
    if (lf.minver > cz_xz.tot_minver) cz_xz.tot_minver = lf.minver;
    cz_x_lfile_free(&lf);
}

static void cz_x_list_totals(void) {
    char a[96], b[96], ra[32], ck[128];
    if (cz_xz.robot) {
        cz_x_checks(cz_xz.tot_checks, ",", ck, sizeof ck);
        printf("totals\t%llu\t%llu\t%llu\t%llu\t%s\t%s\t%llu\t%d", (unsigned long long)cz_xz.tot_streams,
               (unsigned long long)cz_xz.tot_blocks, (unsigned long long)cz_xz.tot_comp, (unsigned long long)cz_xz.tot_uncomp,
               cz_x_ratio(cz_xz.tot_comp, cz_xz.tot_uncomp, ra, sizeof ra), ck, (unsigned long long)cz_xz.tot_padding,
               cz_xz.list_done);
        if (cz_xz.verbose >= 2)
            printf("\t%llu\t%s\t%d", (unsigned long long)cz_xz.tot_mem, cz_xz.tot_all_sizes ? "yes" : "no", cz_xz.tot_minver);
        printf("\n");
        return;
    }
    if (cz_xz.list_done < 2) return;
    if (cz_xz.verbose == 0) {
        cz_x_checks(cz_xz.tot_checks, ",", ck, sizeof ck);
        printf("-------------------------------------------------------------------------------\n");
        printf("%5llu %7llu  %11s %12s  %5s  %-7s %d files\n", (unsigned long long)cz_xz.tot_streams,
               (unsigned long long)cz_xz.tot_blocks, cz_x_nice(cz_xz.tot_comp, 0, a, sizeof a),
               cz_x_nice(cz_xz.tot_uncomp, 0, b, sizeof b), cz_x_ratio(cz_xz.tot_comp, cz_xz.tot_uncomp, ra, sizeof ra), ck,
               cz_xz.list_done);
        return;
    }
    cz_x_checks(cz_xz.tot_checks, ", ", ck, sizeof ck);
    printf("\nTotals:\n");
    printf("  Number of files:   %d\n", cz_xz.list_done);
    printf("  Streams:           %llu\n", (unsigned long long)cz_xz.tot_streams);
    printf("  Blocks:            %llu\n", (unsigned long long)cz_xz.tot_blocks);
    printf("  Compressed size:   %s\n", cz_x_nice(cz_xz.tot_comp, 1, a, sizeof a));
    printf("  Uncompressed size: %s\n", cz_x_nice(cz_xz.tot_uncomp, 1, b, sizeof b));
    printf("  Ratio:             %s\n", cz_x_ratio(cz_xz.tot_comp, cz_xz.tot_uncomp, ra, sizeof ra));
    printf("  Check:             %s\n", ck);
    printf("  Stream Padding:    %s\n", cz_x_nice(cz_xz.tot_padding, 1, a, sizeof a));
    if (cz_xz.verbose >= 2) {
        printf("  Memory needed:     %llu MiB\n", (unsigned long long)cz_x_mib_up(cz_xz.tot_mem));
        printf("  Sizes in headers:  %s\n", cz_xz.tot_all_sizes ? "Yes" : "No");
        printf("  Minimum XZ Utils version: %d.%d.%d\n", cz_xz.tot_minver / 10000000, cz_xz.tot_minver / 10000 % 1000,
               cz_xz.tot_minver / 10 % 1000);
    }
}

static void cz_x_copy_meta(void) {
    struct timespec ts[2];
#ifdef SU_HOST
    ts[0] = cz_xz.src_st.st_atim;
    ts[1] = cz_xz.src_st.st_mtim;
#else
    ts[0].tv_sec = cz_xz.src_st.st_atime; ts[0].tv_nsec = 0;
    ts[1].tv_sec = cz_xz.src_st.st_mtime; ts[1].tv_nsec = 0;
#endif
    if (chown(cz_xz.dst_name, (uid_t)-1, cz_xz.src_st.st_gid) != 0) { /* kept */ }
    chmod(cz_xz.dst_name, cz_xz.src_st.st_mode & 07777);
    if (chown(cz_xz.dst_name, cz_xz.src_st.st_uid, (gid_t)-1) != 0) { /* kept */ }
    utimensat(AT_FDCWD, cz_xz.dst_name, ts, 0);
}

static void cz_x_one(const char *name) {
    if (!*name) {
        cz_x_error(0, "Empty filename, skipping");
        return;
    }
    int is_stdin = strcmp(name, "-") == 0;
    const char *shown = is_stdin ? "(stdin)" : name;
    cz_xz.src_name = shown;
    cz_xz.in_bytes = cz_xz.out_bytes = 0;
    cz_x_wr_err = 0;
    int ifd;
    if (is_stdin) {
        if (cz_xz.mode == CZ_X_M_LIST) {
            cz_x_error(0, "--list does not support reading from standard input");
            return;
        }
        ifd = 0;
        if (fstat(0, &cz_xz.src_st) != 0) memset(&cz_xz.src_st, 0, sizeof cz_xz.src_st);
        if (cz_xz.mode == CZ_X_M_COMPRESS && !cz_xz.force && isatty(1)) {
            cz_x_error(0, "Compressed data cannot be written to a terminal");
            return;
        }
        if (cz_xz.mode != CZ_X_M_COMPRESS && !cz_xz.force && isatty(0)) {
            cz_x_error(0, "Compressed data cannot be read from a terminal");
            return;
        }
    } else {
        int follow = cz_xz.to_stdout || cz_xz.force || cz_xz.mode == CZ_X_M_TEST || cz_xz.mode == CZ_X_M_LIST;
#ifdef SU_HOST
        ifd = open(name, O_RDONLY | O_NONBLOCK | (follow ? 0 : O_NOFOLLOW));
#else
        struct stat ls;
        if (!follow && lstat(name, &ls) == 0 && S_ISLNK(ls.st_mode)) { errno = ELOOP; ifd = -1; }
        else if (((!cz_xz.to_stdout && cz_xz.mode != CZ_X_M_TEST) || cz_xz.mode == CZ_X_M_LIST) &&
                 stat(name, &ls) == 0 && S_ISFIFO(ls.st_mode)) {
            /* no O_NONBLOCK: a FIFO would hold the open until a writer
             * came, and it is skipped unread anyway */
            cz_x_warning(name, "Not a regular file, skipping");
            return;
        } else ifd = su_open_read(name, 0);
#endif
        if (ifd < 0) {
            if (errno == ELOOP && !follow) cz_x_warning(name, "Is a symbolic link, skipping");
            else cz_x_error(name, strerror(errno));
            return;
        }
        if (fstat(ifd, &cz_xz.src_st) != 0) {
            cz_x_error(name, strerror(errno));
            close(ifd);
            return;
        }
        const char *why = 0;
        if (S_ISDIR(cz_xz.src_st.st_mode)) why = "Is a directory, skipping";
        else if (!S_ISREG(cz_xz.src_st.st_mode) && ((!cz_xz.to_stdout && cz_xz.mode != CZ_X_M_TEST) || cz_xz.mode == CZ_X_M_LIST))
            why = "Not a regular file, skipping";
        else if (!cz_xz.to_stdout && !cz_xz.force && cz_xz.mode != CZ_X_M_TEST && cz_xz.mode != CZ_X_M_LIST) {
            if (cz_xz.src_st.st_mode & (S_ISUID | S_ISGID)) why = "File has setuid or setgid bit set, skipping";
            else if (cz_xz.src_st.st_mode & S_ISVTX) why = "File has sticky bit set, skipping";
            else if (cz_xz.src_st.st_nlink > 1) why = "Input file has more than one hard link, skipping";
        }
        if (why) {
            cz_x_warning(name, why);
            close(ifd);
            return;
        }
    }
    if (cz_xz.mode == CZ_X_M_LIST) {
        cz_x_list_one(ifd, name);
        close(ifd);
        return;
    }
    struct cz_in din;
    int fmt = 0;
    if (cz_xz.mode != CZ_X_M_COMPRESS) {
        /* the format, from the first bytes, before any output is made */
        cz_in_open(&din, ifd, 65536);
        fmt = cz_x_detect(&din);
        if (fmt < 0) {
            if (cz_xz.force && cz_xz.to_stdout && cz_xz.mode == CZ_X_M_DECOMPRESS) {
                /* not compressed: passed on as it is */
                uint8_t buf[8192];
                size_t k;
                cz_xz.dst_fd = 1;
                while ((k = cz_read(&din, buf, sizeof buf)) > 0) cz_x_write_out(0, buf, k);
                cz_in_close(&din);
                if (!is_stdin) close(ifd);
                if (cz_x_wr_err) {
                    char m[160];
                    snprintf(m, sizeof m, "Write error: %s", strerror(cz_x_wr_err));
                    cz_x_error("(stdout)", m);
                }
                return;
            }
            cz_x_error(shown, "File format not recognized");
            cz_in_close(&din);
            if (!is_stdin) close(ifd);
            return;
        }
    }
    /* the output */
    cz_xz.dst_name = 0;
    cz_xz.dst_made = 0;
    if (cz_xz.to_stdout || cz_xz.mode == CZ_X_M_TEST || is_stdin) {
        cz_xz.dst_fd = 1;
    } else {
        if (cz_xz.mode == CZ_X_M_COMPRESS) {
            const char *had = cz_x_has_suffix(name);
            if (had) {
                char m[96];
                snprintf(m, sizeof m, "File already has '%s' suffix, skipping", had);
                cz_x_warning(name, m);
                close(ifd);
                return;
            }
            cz_xz.dst_name = cz_x_compressed_name(name);
        } else {
            cz_xz.dst_name = cz_x_uncompressed_name(name);
            if (!cz_xz.dst_name || !*cz_xz.dst_name || cz_xz.dst_name[strlen(cz_xz.dst_name) - 1] == '/') {
                free(cz_xz.dst_name);
                cz_xz.dst_name = 0;
                cz_x_warning(name, "Filename has an unknown suffix, skipping");
                cz_in_close(&din);
                close(ifd);
                return;
            }
        }
        struct stat ds;
        if (cz_xz.force && lstat(cz_xz.dst_name, &ds) == 0) {
            if (unlink(cz_xz.dst_name) != 0 && errno != ENOENT) {
                cz_x_error(cz_xz.dst_name, strerror(errno));
                free(cz_xz.dst_name);
                close(ifd);
                return;
            }
        }
        cz_xz.dst_fd = open(cz_xz.dst_name, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (cz_xz.dst_fd < 0) {
            cz_x_error(cz_xz.dst_name, strerror(errno));
            free(cz_xz.dst_name);
            close(ifd);
            return;
        }
        cz_xz.dst_made = 1;
    }
    int r;
    if (cz_xz.mode == CZ_X_M_COMPRESS) r = cz_x_do_compress(ifd);
    else {
        r = cz_x_do_decompress_in(&din, fmt);
        cz_in_close(&din);
    }
    if (!is_stdin) close(ifd);
    if (r == 0 && cz_x_wr_err) {
        char m[160];
        snprintf(m, sizeof m, "Write error: %s", strerror(cz_x_wr_err));
        cz_x_error(cz_xz.dst_made ? cz_xz.dst_name : "(stdout)", m);
        r = -1;
    }
    if (cz_xz.dst_made) {
        if (r == 0) {
            cz_x_copy_meta();
#ifdef SU_HOST
            if (!cz_xz.no_sync) fsync(cz_xz.dst_fd);
#endif
        }
        if (close(cz_xz.dst_fd) != 0 && r == 0) {
            cz_x_error(cz_xz.dst_name, strerror(errno));
            r = -1;
        }
        if (r != 0) unlink(cz_xz.dst_name);
        else if (!cz_xz.keep) unlink(name);
        free(cz_xz.dst_name);
        cz_xz.dst_name = 0;
    }
    if (r == 0 && cz_xz.verbose >= 1) cz_x_summary(shown);
}

/* ---- the command line ---- */
/* a number with xz's suffixes (KiB, MiB, GiB and their short forms); in
 * [MIN, MAX]: otherwise the run ends, as with xz */
static uint64_t cz_x_num(const char *name, const char *s, uint64_t min, uint64_t max) {
    const char *p = s;
    char m[256];
    while (*p == ' ' || *p == '\t') p++;    /* blanks before it are passed over */
    if (*p == '\0' || *p < '0' || *p > '9') {
        snprintf(m, sizeof m, "%s: Value is not a non-negative decimal integer", s);
        cz_x_fatal(m);
    }
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) { v = UINT64_MAX; while (*p >= '0' && *p <= '9') p++; break; }
        v = v * 10 + d;
        p++;
    }
    if (*p) {
        uint64_t mul = 0;
        if (!strcmp(p, "k") || !strcmp(p, "K") || !strcmp(p, "KB") || !strcmp(p, "Ki") || !strcmp(p, "KiB")) mul = 1u << 10;
        else if (!strcmp(p, "M") || !strcmp(p, "MB") || !strcmp(p, "Mi") || !strcmp(p, "MiB")) mul = 1u << 20;
        else if (!strcmp(p, "G") || !strcmp(p, "GB") || !strcmp(p, "Gi") || !strcmp(p, "GiB")) mul = 1u << 30;
        if (!mul) {
            snprintf(m, sizeof m, "%s: Invalid multiplier suffix", p);
            fflush(stdout);
            if (cz_xz.quiet < 2) {
                fprintf(stderr, "%s: %s\n", cz_xz.prog, m);
                fprintf(stderr, "%s: Valid suffixes are 'KiB' (2^10), 'MiB' (2^20), and 'GiB' (2^30).\n", cz_xz.prog);
            }
            exit(1);
        }
        v = v > UINT64_MAX / mul ? UINT64_MAX : v * mul;
    }
    if (v < min || v > max) {
        snprintf(m, sizeof m, "Value of the option '%s' must be in the range [%llu, %llu]", name,
                 (unsigned long long)min, (unsigned long long)max);
        cz_x_fatal(m);
    }
    return v;
}

/* NAME=VALUE,... for one filter of the chain */
static void cz_x_filter_opts(struct cz_x_filter *f, const char *opts) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", opts ? opts : "");
    char m[1100];
    for (char *tok = buf; tok && *tok;) {
        char *next = strchr(tok, ',');
        if (next) *next++ = 0;
        if (!*tok) { tok = next; continue; }
        char *eq = strchr(tok, '=');
        if (!eq || !eq[1]) {
            snprintf(m, sizeof m, "%s: Options must be 'name=value' pairs separated with commas", tok);
            cz_x_fatal(m);
        }
        *eq = 0;
        const char *val = eq + 1;
        int lz = f->id == CZ_X_F_LZMA1 || f->id == CZ_X_F_LZMA2;
        if (lz && !strcmp(tok, "preset")) {
            int pv = -1, ex = 0;
            if (val[0] >= '0' && val[0] <= '9' && (val[1] == 0 || (val[1] == 'e' && val[2] == 0))) {
                pv = val[0] - '0';
                ex = val[1] == 'e';
            }
            if (pv < 0) {
                snprintf(m, sizeof m, "Unsupported LZMA1/LZMA2 preset: %s", val);
                cz_x_fatal(m);
            }
            cz_x_lzma_preset(f, pv, ex);
        } else if (lz && !strcmp(tok, "dict")) f->dict = (uint32_t)cz_x_num("dict", val, 4096, 1610612736);
        else if (lz && !strcmp(tok, "lc")) f->lc = (int)cz_x_num("lc", val, 0, 4);
        else if (lz && !strcmp(tok, "lp")) f->lp = (int)cz_x_num("lp", val, 0, 4);
        else if (lz && !strcmp(tok, "pb")) f->pb = (int)cz_x_num("pb", val, 0, 4);
        else if (lz && !strcmp(tok, "nice")) f->nice = (int)cz_x_num("nice", val, 2, 273);
        else if (lz && !strcmp(tok, "depth")) f->depth = (int)cz_x_num("depth", val, 0, 4294967295u);
        else if (lz && !strcmp(tok, "mode")) {
            if (!strcmp(val, "fast")) f->fast = 1;
            else if (!strcmp(val, "normal")) f->fast = 0;
            else { snprintf(m, sizeof m, "%s: Invalid option value", val); cz_x_fatal(m); }
        } else if (lz && !strcmp(tok, "mf")) {
            static const char *const names[] = {"hc3", "hc4", "bt2", "bt3", "bt4"};
            static const int ids[] = {3, 4, 12, 13, 14};
            int k = -1;
            for (int i = 0; i < 5; i++) if (!strcmp(val, names[i])) k = i;
            if (k < 0) { snprintf(m, sizeof m, "%s: Invalid option value", val); cz_x_fatal(m); }
            f->mf = ids[k];
        } else if (f->id == CZ_X_F_DELTA && !strcmp(tok, "dist")) f->dist = (int)cz_x_num("dist", val, 1, 256);
        else if (!lz && f->id != CZ_X_F_DELTA && !strcmp(tok, "start")) {
            f->start = (uint32_t)cz_x_num("start", val, 0, 4294967295u);
        } else {
            snprintf(m, sizeof m, "%s: Invalid option name", tok);
            cz_x_fatal(m);
        }
        tok = next;
    }
    if ((f->id == CZ_X_F_LZMA1 || f->id == CZ_X_F_LZMA2) && f->lc + f->lp > 4)
        cz_x_fatal("The sum of lc and lp must not exceed 4");
}

static void cz_x_add_filter(uint64_t id, const char *opts) {
    if (cz_xz.chain_kind != 1) {
        cz_xz.nchain = 0;
        cz_xz.chain_kind = 1;
    }
    if (cz_xz.nchain == 4) cz_x_fatal("Maximum number of filters is four");
    struct cz_x_filter *f = &cz_xz.chain[cz_xz.nchain++];
    memset(f, 0, sizeof *f);
    f->id = id;
    if (id == CZ_X_F_LZMA1 || id == CZ_X_F_LZMA2) cz_x_lzma_preset(f, 6, 0);
    if (id == CZ_X_F_DELTA) f->dist = 1;
    cz_x_filter_opts(f, opts);
}

/* --filters=STR: filters split by spaces or "--", each NAME[:OPTS], or a
 * preset 0-9[e] */
static void cz_x_filters_str(const char *optname, const char *s) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    struct cz_x_filter f[4];
    int n = 0;
    char *p = buf;
    for (;;) {
        while (*p == ' ' || *p == '\t' || (p[0] == '-' && p[1] == '-')) p += *p == '-' ? 2 : 1;
        if (!*p) break;
        char *e = p;
        while (*e && *e != ' ' && *e != '\t' && !(e[0] == '-' && e[1] == '-')) e++;
        char save = *e;
        *e = 0;
        char *colon = strchr(p, ':');
        if (colon) *colon = 0;
        static const struct { const char *n; uint64_t id; } tab[] = {
            {"lzma1", CZ_X_F_LZMA1}, {"lzma2", CZ_X_F_LZMA2}, {"x86", CZ_X_F_X86}, {"arm", CZ_X_F_ARM},
            {"armthumb", CZ_X_F_ARMTHUMB}, {"arm64", CZ_X_F_ARM64}, {"riscv", CZ_X_F_RISCV}, {"powerpc", CZ_X_F_PPC},
            {"ia64", CZ_X_F_IA64}, {"sparc", CZ_X_F_SPARC}, {"delta", CZ_X_F_DELTA},
        };
        int found = -1;
        for (int i = 0; i < 11; i++) if (!strcmp(p, tab[i].n)) found = i;
        if (n == 0 && !colon && p[0] >= '0' && p[0] <= '9' && (p[1] == 0 || (p[1] == 'e' && p[2] == 0)) && save == 0) {
            memset(&f[0], 0, sizeof f[0]);
            f[0].id = CZ_X_F_LZMA2;
            cz_x_lzma_preset(&f[0], p[0] - '0', p[1] == 'e');
            n = 1;
            break;
        }
        if (found < 0 || n == 4) {
            fflush(stdout);
            fprintf(stderr, "%s: Error in --%s=FILTERS option:\n%s: %s\n", cz_xz.prog, optname, cz_xz.prog, s);
            fprintf(stderr, "%s: %*s^\n%s: %s\n", cz_xz.prog, (int)(p - buf), "", cz_xz.prog,
                    found < 0 ? "Unknown filter name" : "The maximum number of filters is four");
            exit(1);
        }
        memset(&f[n], 0, sizeof f[n]);
        f[n].id = tab[found].id;
        if (f[n].id == CZ_X_F_LZMA1 || f[n].id == CZ_X_F_LZMA2) cz_x_lzma_preset(&f[n], 6, 0);
        if (f[n].id == CZ_X_F_DELTA) f[n].dist = 1;
        if (colon) {
            struct cz_x_filter *t = &f[n];
            cz_x_filter_opts(t, colon + 1);
        }
        n++;
        *e = save;
        p = e;
    }
    if (n == 0) return;
    if (f[n - 1].id != CZ_X_F_LZMA1 && f[n - 1].id != CZ_X_F_LZMA2) {
        fflush(stdout);
        fprintf(stderr, "%s: Error in --%s=FILTERS option:\n%s: %s\n%s: %*s^\n%s: %s\n", cz_xz.prog, optname, cz_xz.prog, s,
                cz_xz.prog, (int)strlen(s), "", cz_xz.prog, "The last filter must be LZMA1 or LZMA2");
        exit(1);
    }
    if (strcmp(optname, "filters") != 0) return;   /* --filters1..9: for --block-list only */
    memcpy(cz_xz.chain, f, sizeof f);
    cz_xz.nchain = n;
    cz_xz.chain_kind = 2;
}

#define CZ_X_O_NOSYNC 0x12000
#define CZ_X_O_SINGLE 0x12001
#define CZ_X_O_NOSPARSE 0x12002
#define CZ_X_O_FILES 0x12003
#define CZ_X_O_FILES0 0x12004
#define CZ_X_O_IGNCHECK 0x12005
#define CZ_X_O_BSIZE 0x12006
#define CZ_X_O_BLIST 0x12007
#define CZ_X_O_MEMC 0x12008
#define CZ_X_O_MEMD 0x12009
#define CZ_X_O_MEMMT 0x1200a
#define CZ_X_O_NOADJ 0x1200b
#define CZ_X_O_FLUSH 0x1200c
#define CZ_X_O_FILTERS 0x1200d
#define CZ_X_O_FILTERSN 0x1200e
#define CZ_X_O_FHELP 0x1200f
#define CZ_X_O_LZMA1 0x12010
#define CZ_X_O_LZMA2 0x12011
#define CZ_X_O_X86 0x12012
#define CZ_X_O_PPC 0x12013
#define CZ_X_O_IA64 0x12014
#define CZ_X_O_ARM 0x12015
#define CZ_X_O_ARMT 0x12016
#define CZ_X_O_ARM64 0x12017
#define CZ_X_O_RISCV 0x12018
#define CZ_X_O_SPARC 0x12019
#define CZ_X_O_DELTA 0x1201a
#define CZ_X_O_ROBOT 0x1201b
#define CZ_X_O_INFOMEM 0x1201c

static const struct su_option cz_x_longs[] = {
    { "compress", SU_NO_ARG, 0, 'z' },
    { "decompress", SU_NO_ARG, 0, 'd' },
    { "uncompress", SU_NO_ARG, 0, 'd' },
    { "test", SU_NO_ARG, 0, 't' },
    { "list", SU_NO_ARG, 0, 'l' },
    { "keep", SU_NO_ARG, 0, 'k' },
    { "force", SU_NO_ARG, 0, 'f' },
    { "stdout", SU_NO_ARG, 0, 'c' },
    { "to-stdout", SU_NO_ARG, 0, 'c' },
    { "no-sync", SU_NO_ARG, 0, CZ_X_O_NOSYNC },
    { "single-stream", SU_NO_ARG, 0, CZ_X_O_SINGLE },
    { "no-sparse", SU_NO_ARG, 0, CZ_X_O_NOSPARSE },
    { "suffix", SU_REQ_ARG, 0, 'S' },
    { "files", SU_OPT_ARG, 0, CZ_X_O_FILES },
    { "files0", SU_OPT_ARG, 0, CZ_X_O_FILES0 },
    { "format", SU_REQ_ARG, 0, 'F' },
    { "check", SU_REQ_ARG, 0, 'C' },
    { "ignore-check", SU_NO_ARG, 0, CZ_X_O_IGNCHECK },
    { "block-size", SU_REQ_ARG, 0, CZ_X_O_BSIZE },
    { "block-list", SU_REQ_ARG, 0, CZ_X_O_BLIST },
    { "memlimit-compress", SU_REQ_ARG, 0, CZ_X_O_MEMC },
    { "memlimit-decompress", SU_REQ_ARG, 0, CZ_X_O_MEMD },
    { "memlimit-mt-decompress", SU_REQ_ARG, 0, CZ_X_O_MEMMT },
    { "memlimit", SU_REQ_ARG, 0, 'M' },
    { "memory", SU_REQ_ARG, 0, 'M' },
    { "no-adjust", SU_NO_ARG, 0, CZ_X_O_NOADJ },
    { "threads", SU_REQ_ARG, 0, 'T' },
    { "flush-timeout", SU_REQ_ARG, 0, CZ_X_O_FLUSH },
    { "extreme", SU_NO_ARG, 0, 'e' },
    { "fast", SU_NO_ARG, 0, '0' },
    { "best", SU_NO_ARG, 0, '9' },
    { "filters", SU_REQ_ARG, 0, CZ_X_O_FILTERS },
    { "filters1", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x100 },
    { "filters2", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x200 },
    { "filters3", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x300 },
    { "filters4", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x400 },
    { "filters5", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x500 },
    { "filters6", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x600 },
    { "filters7", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x700 },
    { "filters8", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x800 },
    { "filters9", SU_REQ_ARG, 0, CZ_X_O_FILTERSN + 0x900 },
    { "filters-help", SU_NO_ARG, 0, CZ_X_O_FHELP },
    { "lzma1", SU_OPT_ARG, 0, CZ_X_O_LZMA1 },
    { "lzma2", SU_OPT_ARG, 0, CZ_X_O_LZMA2 },
    { "x86", SU_OPT_ARG, 0, CZ_X_O_X86 },
    { "powerpc", SU_OPT_ARG, 0, CZ_X_O_PPC },
    { "ia64", SU_OPT_ARG, 0, CZ_X_O_IA64 },
    { "arm", SU_OPT_ARG, 0, CZ_X_O_ARM },
    { "armthumb", SU_OPT_ARG, 0, CZ_X_O_ARMT },
    { "arm64", SU_OPT_ARG, 0, CZ_X_O_ARM64 },
    { "riscv", SU_OPT_ARG, 0, CZ_X_O_RISCV },
    { "sparc", SU_OPT_ARG, 0, CZ_X_O_SPARC },
    { "delta", SU_OPT_ARG, 0, CZ_X_O_DELTA },
    { "quiet", SU_NO_ARG, 0, 'q' },
    { "verbose", SU_NO_ARG, 0, 'v' },
    { "no-warn", SU_NO_ARG, 0, 'Q' },
    { "robot", SU_NO_ARG, 0, CZ_X_O_ROBOT },
    { "info-memory", SU_NO_ARG, 0, CZ_X_O_INFOMEM },
    { "help", SU_NO_ARG, 0, 'h' },
    { "long-help", SU_NO_ARG, 0, 'H' },
    { "version", SU_NO_ARG, 0, 'V' },
    { 0, 0, 0, 0 }
};
#define CZ_X_SHORTS "cC:defF:hHlkM:qQrS:tT:vVz0123456789"

static void cz_x_help(int lng) {
    printf("Usage: %s [OPTION]... [FILE]...\n"
           "Compress or decompress FILEs in the .xz format (or .lzma, raw; lzip to\n"
           "decompress).\n\n", cz_xz.prog);
    printf("  -z, --compress      compress, whatever the name says\n"
           "  -d, --decompress    decompress\n"
           "  -t, --test          check that compressed files are intact\n"
           "  -l, --list          show what is inside .xz files\n"
           "  -k, --keep          do not remove the input files\n"
           "  -f, --force         replace outputs; take links and special modes\n"
           "  -c, --stdout        write to standard output, keep the input files\n"
           "  -0 ... -9           the compression preset (6 by default)\n"
           "  -e, --extreme       spend more time for a smaller result\n"
           "  -T, --threads=NUM   the number of threads (0: one per processor)\n"
           "  -q, --quiet         say less (twice: say no errors either)\n"
           "  -v, --verbose       say more (twice: more again)\n"
           "  -h, --help          this help (-H: all the options)\n"
           "  -V, --version       the version\n");
    if (lng)
        printf("\n"
               "  -S, --suffix=.SUF   the suffix of compressed files\n"
               "  -F, --format=FMT    auto, xz, lzma, lzip (decompressing) or raw\n"
               "  -C, --check=NAME    none, crc32, crc64 or sha256\n"
               "      --ignore-check  do not verify the integrity check\n"
               "      --single-stream decompress the first stream only\n"
               "      --no-sparse     write no sparse files\n"
               "      --no-sync       do not synchronize outputs before removing inputs\n"
               "      --files[=FILE]  take the file names from FILE (or stdin), one a line\n"
               "      --files0[=FILE] the same, each name ended by a null byte\n"
               "      --block-size=SIZE   a block every SIZE bytes of input\n"
               "      --block-list=LIST   blocks of the sizes listed\n"
               "      --flush-timeout=MS  accepted; data is never held back here\n"
               "  -M, --memlimit=LIMIT    the memory limit (also --memlimit-compress,\n"
               "                          --memlimit-decompress, --memlimit-mt-decompress)\n"
               "      --no-adjust     do not lower the settings to fit the limit\n"
               "      --filters=FILTERS   the filter chain in liblzma's string syntax\n"
               "      --lzma1[=OPTS] --lzma2[=OPTS]   preset, dict, lc, lp, pb, mode,\n"
               "                          nice, mf, depth\n"
               "      --x86 --arm --armthumb --arm64 --powerpc --ia64 --sparc [=start=N]\n"
               "      --delta[=dist=N]\n"
               "  -Q, --no-warn       warnings do not change the exit status\n"
               "      --robot         machine-readable output\n"
               "      --info-memory   show the memory limits\n");
    printf("\nWith no FILE, or when FILE is -, standard input is read.\n");
}

static void cz_x_run_files(const char *src, int nul) {
    FILE *fp = stdin;
    const char *shown = src ? src : "(stdin)";
    if (src) {
        fp = fopen(src, "r");
        if (!fp) { cz_x_error(src, strerror(errno)); return; }
    }
    char *line = 0;
    size_t cap = 0, n = 0;
    int c;
    for (;;) {
        c = getc(fp);
        if (c == EOF) break;
        if (n + 1 >= cap) { cap = cap ? cap * 2 : 256; line = (char *)realloc(line, cap); }
        if (c == (nul ? 0 : '\n')) {
            line[n] = 0;
            if (n) cz_x_one(line);
            n = 0;
            continue;
        }
        line[n++] = (char)c;
    }
    if (n) cz_x_error(shown, "Unexpected end of input when reading filenames");
    free(line);
    if (src) fclose(fp);
}

static int cz_x_main(int argc, char **argv) {
    memset(&cz_xz, 0, sizeof cz_xz);
    const char *b = strrchr(argv[0], '/');
    cz_xz.prog = b ? b + 1 : argv[0];
    cz_xz.check = CZ_X_CHECK_CRC64;
    cz_xz.preset = 6;
    cz_xz.dst_fd = -1;
    cz_xz.tot_all_sizes = 1;
    cz_xz.tot_minver = 50000002;
    if (strstr(cz_xz.prog, "xzcat") || strstr(cz_xz.prog, "lzcat")) { cz_xz.mode = CZ_X_M_DECOMPRESS; cz_xz.to_stdout = 1; }
    else if (strstr(cz_xz.prog, "unxz") || strstr(cz_xz.prog, "unlzma")) cz_xz.mode = CZ_X_M_DECOMPRESS;
    if (strstr(cz_xz.prog, "lzma") || strstr(cz_xz.prog, "lzcat")) cz_xz.format = CZ_X_FMT_LZMA;
    int c;
    while ((c = su_getopt_long(argc, argv, CZ_X_SHORTS, cz_x_longs, 0)) != -1) {
        switch (c) {
        case 'z': cz_xz.mode = CZ_X_M_COMPRESS; break;
        case 'd': cz_xz.mode = CZ_X_M_DECOMPRESS; break;
        case 't': cz_xz.mode = CZ_X_M_TEST; break;
        case 'l': cz_xz.mode = CZ_X_M_LIST; break;
        case 'k': cz_xz.keep = 1; break;
        case 'f': cz_xz.force = 1; break;
        case 'c': cz_xz.to_stdout = 1; break;
        case CZ_X_O_NOSYNC: cz_xz.no_sync = 1; break;
        case CZ_X_O_SINGLE: cz_xz.single_stream = 1; break;
        case CZ_X_O_NOSPARSE: cz_xz.no_sparse = 1; break;
        case 'S':
            if (!*su_optarg || strchr(su_optarg, '/')) {
                char m[300];
                snprintf(m, sizeof m, "%s: Invalid filename suffix", su_optarg);
                cz_x_fatal(m);
            }
            cz_xz.suffix = su_optarg;
            break;
        case CZ_X_O_FILES: cz_xz.files_from = su_optarg; cz_xz.files0 = 0; cz_xz.files_given = 1; break;
        case CZ_X_O_FILES0: cz_xz.files_from = su_optarg; cz_xz.files0 = 1; cz_xz.files_given = 1; break;
        case 'F': {
            static const char *const names[] = {"auto", "xz", "lzma", "alone", "lzip", "raw"};
            static const int vals[] = {CZ_X_FMT_AUTO, CZ_X_FMT_XZ, CZ_X_FMT_LZMA, CZ_X_FMT_LZMA, CZ_X_FMT_LZIP, CZ_X_FMT_RAW};
            int k = -1;
            for (int i = 0; i < 6; i++) if (!strcmp(su_optarg, names[i])) k = i;
            if (k < 0) {
                char m[300];
                snprintf(m, sizeof m, "%s: Unknown file format type", su_optarg);
                cz_x_fatal(m);
            }
            cz_xz.format = vals[k];
            break;
        }
        case 'C': {
            static const char *const names[] = {"none", "crc32", "crc64", "sha256"};
            static const int vals[] = {0, 1, 4, 10};
            int k = -1;
            for (int i = 0; i < 4; i++) if (!strcmp(su_optarg, names[i])) k = i;
            if (k < 0) {
                char m[300];
                snprintf(m, sizeof m, "%s: Unsupported integrity check type", su_optarg);
                cz_x_fatal(m);
            }
            cz_xz.check = vals[k];
            cz_xz.check_set = 1;
            break;
        }
        case CZ_X_O_IGNCHECK: cz_xz.ignore_check = 1; break;
        case CZ_X_O_BSIZE: cz_xz.block_size = cz_x_num("block-size", su_optarg, 0, UINT64_MAX); break;
        case CZ_X_O_BLIST: {
            /* [N:]SIZE,...: the sizes of the blocks; the last one again
             * for the rest */
            char tmp[2048];
            snprintf(tmp, sizeof tmp, "%s", su_optarg);
            cz_xz.nblist = 0;
            for (char *t = strtok(tmp, ","); t && cz_xz.nblist < 256; t = strtok(0, ",")) {
                char *colon = strchr(t, ':');
                if (colon) t = colon + 1;
                cz_xz.blist[cz_xz.nblist++] = cz_x_num("block-list", t, 0, UINT64_MAX);
            }
            break;
        }
        case CZ_X_O_MEMC: cz_xz.memlimit_c = cz_x_limit("memlimit-compress", su_optarg); break;
        case CZ_X_O_MEMD: cz_xz.memlimit_d = cz_x_limit("memlimit-decompress", su_optarg); break;
        case CZ_X_O_MEMMT: cz_xz.memlimit_mt = cz_x_limit("memlimit-mt-decompress", su_optarg); break;
        case 'M':
            cz_xz.memlimit_c = cz_xz.memlimit_d = cz_xz.memlimit_mt = cz_x_limit("memlimit", su_optarg);
            break;
        case CZ_X_O_NOADJ: cz_xz.no_adjust = 1; break;
        case 'T': {
            /* "+N": the threaded mode even with one thread (its layout,
             * its memory) -- measured, as xz 5.8's */
            const char *t = su_optarg;
            int plus = *t == '+';
            uint64_t v = cz_x_num("threads", t + plus, 0, UINT64_MAX);
            if (v > 16384) cz_x_fatal("Value of the option 'threads' must be in the range [0, 16384]");
            cz_xz.threads = (long)v;
            cz_xz.threads_plus = plus && v == 1;
            break;
        }
        case CZ_X_O_FLUSH:
            cz_x_num("flush-timeout", su_optarg, 0, UINT64_MAX);
            cz_xz.flush_timeout = 1;
            break;
        case 'e': cz_xz.extreme = 1; break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            cz_xz.preset = c - '0';
            cz_xz.nchain = 0;
            cz_xz.chain_kind = 0;
            break;
        case CZ_X_O_FILTERS: cz_x_filters_str("filters", su_optarg); break;
        case CZ_X_O_FHELP:
            printf("Filter chains: --filters=FILTERS (or --filters1=FILTERS ... --filters9=FILTERS,\n"
                   "for --block-list) names the filters, separated by spaces or '--', each\n"
                   "NAME[:OPTS], or gives a preset 0-9[e]. The filters and their options:\n"
                   "lzma2:preset=<0-9[e]>,dict=<4KiB-1536MiB>,lc=<0-4>,lp=<0-4>,pb=<0-4>,mode=<fast|normal>,nice=<2-273>,mf=<hc3|hc4|bt2|bt3|bt4>,depth=<0-4294967295>\n"
                   "x86:start=<0-4294967295>\narm:start=<0-4294967295>\narmthumb:start=<0-4294967295>\n"
                   "arm64:start=<0-4294967295>\nriscv:start=<0-4294967295>\npowerpc:start=<0-4294967295>\n"
                   "ia64:start=<0-4294967295>\nsparc:start=<0-4294967295>\ndelta:dist=<1-256>\n");
            return 0;
        case CZ_X_O_LZMA1: cz_x_add_filter(CZ_X_F_LZMA1, su_optarg); break;
        case CZ_X_O_LZMA2: cz_x_add_filter(CZ_X_F_LZMA2, su_optarg); break;
        case CZ_X_O_X86: cz_x_add_filter(CZ_X_F_X86, su_optarg); break;
        case CZ_X_O_PPC: cz_x_add_filter(CZ_X_F_PPC, su_optarg); break;
        case CZ_X_O_IA64: cz_x_add_filter(CZ_X_F_IA64, su_optarg); break;
        case CZ_X_O_ARM: cz_x_add_filter(CZ_X_F_ARM, su_optarg); break;
        case CZ_X_O_ARMT: cz_x_add_filter(CZ_X_F_ARMTHUMB, su_optarg); break;
        case CZ_X_O_ARM64: cz_x_add_filter(CZ_X_F_ARM64, su_optarg); break;
        case CZ_X_O_RISCV: cz_x_add_filter(CZ_X_F_RISCV, su_optarg); break;
        case CZ_X_O_SPARC: cz_x_add_filter(CZ_X_F_SPARC, su_optarg); break;
        case CZ_X_O_DELTA: cz_x_add_filter(CZ_X_F_DELTA, su_optarg); break;
        case 'q': if (cz_xz.quiet < 2) cz_xz.quiet++; break;
        case 'v': if (cz_xz.verbose < 3) cz_xz.verbose++; break;
        case 'Q': cz_xz.no_warn = 1; break;
        case CZ_X_O_ROBOT: cz_xz.robot = 1; break;
        case CZ_X_O_INFOMEM:
            cz_x_info_memory();
            return 0;
        case 'h': cz_x_help(0); return 0;
        case 'H': cz_x_help(1); return 0;
        case 'V': printf("xz (tobyOS) 1.0\n"); return 0;
        default:
            if (c >= CZ_X_O_FILTERSN && c < CZ_X_O_FILTERSN + 0x1000) {
                char nm[16];
                snprintf(nm, sizeof nm, "filters%d", (c - CZ_X_O_FILTERSN) >> 8);
                cz_x_filters_str(nm, su_optarg);
                break;
            }
            cz_x_try_help();
        }
    }
    if (cz_xz.mode == CZ_X_M_COMPRESS && cz_xz.format == CZ_X_FMT_LZIP)
        cz_x_fatal("Compression of lzip files (.lz) is not supported");
    if (cz_xz.mode == CZ_X_M_COMPRESS && cz_xz.format == CZ_X_FMT_AUTO) cz_xz.format = CZ_X_FMT_XZ;
    if (cz_xz.mode == CZ_X_M_COMPRESS && cz_xz.robot && !cz_xz.to_stdout) {
        /* xz says this when it has files to (de)compress */
    }
    int nfiles = argc - su_optind;
    if (cz_xz.format == CZ_X_FMT_RAW && cz_xz.mode != CZ_X_M_TEST && cz_xz.mode != CZ_X_M_LIST && !cz_xz.to_stdout && !cz_xz.suffix && nfiles > 0) {
        int all_stdin = 1;
        for (int i = su_optind; i < argc; i++) if (strcmp(argv[i], "-")) all_stdin = 0;
        if (!all_stdin) cz_x_fatal("With --format=raw, --suffix=.SUF is required unless writing to stdout");
    }
    if (cz_xz.format == CZ_X_FMT_RAW && cz_xz.nchain == 0) {
        /* said, but no warning as the exit status counts them */
        if (cz_xz.quiet < 1) {
            fprintf(stderr, "%s: Using a preset in raw mode is discouraged.\n", cz_xz.prog);
            fprintf(stderr, "%s: The exact options of the presets may vary between software versions.\n", cz_xz.prog);
        }
    }
    if (cz_xz.mode == CZ_X_M_COMPRESS) cz_x_validate_chain();
    if (cz_xz.mode == CZ_X_M_COMPRESS && cz_xz.flush_timeout && !CZ_X_SINGLE() && cz_xz.format == CZ_X_FMT_XZ) {
        if (cz_xz.quiet < 1) fprintf(stderr, "%s: Switching to single-threaded mode due to --flush-timeout\n", cz_xz.prog);
        cz_xz.threads = 1;
        cz_xz.threads_plus = 0;
    }
    /* the memory is set up first, then --robot refused (measured: the
     * notices and the limit's error come before the refusal) */
    if (cz_xz.mode == CZ_X_M_COMPRESS) cz_x_mem_setup();
    if (cz_xz.robot && (cz_xz.mode == CZ_X_M_COMPRESS || cz_xz.mode == CZ_X_M_DECOMPRESS || cz_xz.mode == CZ_X_M_TEST))
        cz_x_fatal("Compression and decompression with --robot are not supported yet.");
    if (cz_xz.mode == CZ_X_M_LIST && cz_xz.files_given == 0 && nfiles == 0) {
        cz_x_error(0, "--list does not support reading from standard input");
        return cz_xz.exit_status;
    }
    {
        int uses_stdin = (nfiles == 0 && !cz_xz.files_given);
        for (int i = su_optind; i < argc; i++) if (!strcmp(argv[i], "-")) uses_stdin = 1;
        if (cz_xz.mode == CZ_X_M_COMPRESS && !cz_xz.force && (cz_xz.to_stdout || uses_stdin) && isatty(1)) {
            cz_x_error(0, "Compressed data cannot be written to a terminal");
            cz_x_try_help();
        }
        if ((cz_xz.mode == CZ_X_M_DECOMPRESS || cz_xz.mode == CZ_X_M_TEST) && !cz_xz.force && uses_stdin && isatty(0)) {
            cz_x_error(0, "Compressed data cannot be read from a terminal");
            return cz_xz.exit_status;
        }
    }
    cz_xz.list_files = nfiles;
    if (nfiles == 0 && !cz_xz.files_given) cz_x_one("-");
    for (int i = su_optind; i < argc; i++) cz_x_one(argv[i]);
    if (cz_xz.files_given) cz_x_run_files(cz_xz.files_from, cz_xz.files0);
    if (cz_xz.mode == CZ_X_M_LIST && cz_xz.list_done > 0) cz_x_list_totals();
    fflush(stdout);
    if (ferror(stdout)) cz_x_error(0, "Writing to standard output failed");
    return cz_xz.exit_status;
}

static int do_xz(int argc, char **argv) {
    return cz_x_main(argc, argv);
}
