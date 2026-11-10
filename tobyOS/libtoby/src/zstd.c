/* zstd.c -- Zstandard (RFC 8878) decompression, and a raw-block encoder.
 *
 * Nothing in this tree could read a .zst file, so zstd/unzstd/zstdcat could
 * not exist -- and .zst is what modern distros ship packages and kernels
 * in, so it is the one remaining format that actually blocks reading real
 * data.
 *
 * WHAT IS HERE. A complete decoder: frame header, raw/RLE/compressed
 * blocks, all four literals-section kinds (raw, RLE, Huffman, treeless),
 * FSE-coded Huffman weights, the three sequence tables in every mode
 * (predefined, RLE, FSE, repeat), and the repeat-offset machinery.
 *
 * WHAT IS NOT. The encoder emits RAW blocks only: a valid .zst frame that
 * any zstd decodes, but not actually compressed. Real zstd compression
 * means a match finder feeding FSE and Huffman encoders, which is a much
 * bigger piece than the decoder and buys little here -- gzip and bzip2 are
 * both real in this tree, so anything that wants a smaller file has two
 * working choices. `zstd` says this on stderr rather than letting someone
 * discover it from the file size.
 *
 * THE TRAP, if you extend this: zstd's entropy bitstreams are written
 * forward but READ BACKWARD from the last byte, and the highest set bit of
 * that last byte is an end marker, not data. Getting that wrong decodes a
 * plausible-looking prefix and then falls apart.
 */

#include <toby/zstd.h>

#include <stdlib.h>
#include <string.h>

#define ZSTD_MAGIC        0xFD2FB528u
#define ZSTD_SKIP_MASK    0xFFFFFFF0u
#define ZSTD_SKIP_MAGIC   0x184D2A50u

#define MAX_HUF_BITS      11
#define MAX_FSE_LOG       9

/* ------------------------------------------------------------ bit reader */

/* Reads BACKWARD: bit position `pos` counts from the start of the buffer in
 * the usual LSB-first-within-a-byte order, and each read takes the bits
 * immediately below the current position, most significant first. */
struct bitr {
    const uint8_t *buf;
    long           pos;
    int            bad;
};

static int br_init(struct bitr *b, const uint8_t *p, size_t n) {
    memset(b, 0, sizeof *b);
    if (n == 0) return -1;
    uint8_t last = p[n - 1];
    if (last == 0) return -1;            /* the end marker must be present */
    int hb = 7;
    while (hb > 0 && !(last & (1u << hb))) hb--;
    b->buf = p;
    b->pos = (long)(n - 1) * 8 + hb;     /* the marker bit itself is skipped */
    return 0;
}

static uint32_t br_read(struct bitr *b, int nb) {
    uint32_t v = 0;
    while (nb-- > 0) {
        b->pos--;
        uint32_t bit = 0;
        if (b->pos >= 0) bit = (b->buf[b->pos >> 3] >> (b->pos & 7)) & 1u;
        else b->bad = 1;
        v = (v << 1) | bit;
    }
    return v;
}


/* --------------------------------------------------------------- FSE */

struct fse_entry {
    uint8_t  symbol;
    uint8_t  nbits;
    uint16_t newstate;
};

struct fse {
    struct fse_entry *tab;
    int               log;
};

/* Read a normalised count table, as the format writes it: a variable
 * number of bits per symbol, narrowing as the remaining budget shrinks. */
static int fse_read_ncount(const uint8_t *src, size_t srclen, size_t *consumed,
                           short *counts, int *maxsym, int *tablelog)
{
    if (srclen < 1) return -1;
    /* This header is read FORWARD, unlike the entropy streams. */
    size_t bitpos = 0;
    size_t nbits_avail = srclen * 8;

    #define RD(n) ({ uint32_t _v = 0; \
                     for (int _i = 0; _i < (n); _i++) { \
                         if (bitpos >= nbits_avail) return -1; \
                         _v |= (uint32_t)((src[bitpos >> 3] >> (bitpos & 7)) & 1u) << _i; \
                         bitpos++; } _v; })

    int accuracy = (int)RD(4) + 5;
    if (accuracy > MAX_FSE_LOG) return -1;
    *tablelog = accuracy;

    int remaining = (1 << accuracy) + 1;
    int threshold = 1 << accuracy;
    int nbits = accuracy + 1;
    int sym = 0;
    int previous0 = 0;

    while (remaining > 1 && sym < 256) {
        if (previous0) {
            /* Runs of zero-probability symbols are coded as repeated
             * 2-bit groups, with 3 meaning "and more". */
            int n = 0;
            for (;;) {
                uint32_t v = RD(2);
                n += (int)v;
                if (v != 3) break;
            }
            while (n-- > 0 && sym < 256) counts[sym++] = 0;
            previous0 = 0;
            continue;
        }
        int max = (2 * threshold - 1) - remaining;
        int count;
        uint32_t v = RD(nbits - 1);
        if ((int)v < max) {
            count = (int)v;
        } else {
            uint32_t extra = RD(1);
            uint32_t vv = v + (extra << (nbits - 1));
            if ((int)vv >= threshold) vv -= (uint32_t)max;
            count = (int)vv;
        }
        count -= 1;                       /* -1 encodes "less than one" */
        remaining -= (count < 0) ? -count : count;
        counts[sym++] = (short)count;
        if (count == 0) previous0 = 1;
        while (remaining < threshold) { nbits--; threshold >>= 1; }
    }
    #undef RD

    *maxsym = sym - 1;
    *consumed = (bitpos + 7) / 8;
    return 0;
}

/* Spread the symbols across the table in the order the format defines --
 * a fixed stride that visits every slot exactly once. */
static int fse_build(struct fse *f, const short *counts, int maxsym, int log) {
    int size = 1 << log;
    f->log = log;
    f->tab = (struct fse_entry *)calloc((size_t)size, sizeof(struct fse_entry));
    if (!f->tab) return -1;

    uint8_t *symbols = (uint8_t *)malloc((size_t)size);
    uint16_t *nextstate = (uint16_t *)calloc((size_t)size, sizeof(uint16_t));
    if (!symbols || !nextstate) { free(symbols); free(nextstate); free(f->tab); f->tab = NULL; return -1; }

    int highthresh = size - 1;
    /* Symbols with a "less than one" probability sit at the top, in
     * reverse order. */
    for (int s = 0; s <= maxsym; s++)
        if (counts[s] == -1) { symbols[highthresh--] = (uint8_t)s; nextstate[s] = 1; }

    int step = (size >> 1) + (size >> 3) + 3;
    int mask = size - 1;
    int pos = 0;
    for (int s = 0; s <= maxsym; s++) {
        for (int i = 0; i < counts[s]; i++) {
            symbols[pos] = (uint8_t)s;
            do { pos = (pos + step) & mask; } while (pos > highthresh);
        }
    }
    if (pos != 0) { /* the walk must return to 0 exactly */ }

    uint16_t *cnt = (uint16_t *)calloc(256, sizeof(uint16_t));
    if (!cnt) { free(symbols); free(nextstate); free(f->tab); f->tab = NULL; return -1; }
    for (int s = 0; s <= maxsym; s++)
        cnt[s] = (uint16_t)(counts[s] == -1 ? 1 : (counts[s] > 0 ? counts[s] : 0));

    uint16_t *state_next = (uint16_t *)calloc(256, sizeof(uint16_t));
    if (!state_next) { free(cnt); free(symbols); free(nextstate); free(f->tab); f->tab = NULL; return -1; }
    for (int s = 0; s <= maxsym; s++) state_next[s] = cnt[s];

    for (int i = 0; i < size; i++) {
        uint8_t s = symbols[i];
        uint16_t nx = state_next[s]++;
        int nb = log;
        while ((1 << nb) > nx) nb--;      /* highest bit of nx */
        nb = log - nb;
        f->tab[i].symbol   = s;
        f->tab[i].nbits    = (uint8_t)nb;
        f->tab[i].newstate = (uint16_t)((nx << nb) - size);
    }

    free(cnt); free(state_next); free(symbols); free(nextstate);
    return 0;
}

static void fse_free(struct fse *f) { free(f->tab); f->tab = NULL; }

/* --------------------------------------------------------- Huffman */

struct huf {
    uint8_t  sym[1 << MAX_HUF_BITS];
    uint8_t  nb [1 << MAX_HUF_BITS];
    int      maxbits;
};

/* Emit the current symbol and advance the state, which is the order FSE
 * decoding uses everywhere. */
static uint8_t fse_next(struct fse *f, uint32_t *st, struct bitr *b) {
    uint8_t sym = f->tab[*st].symbol;
    int nb = f->tab[*st].nbits;
    *st = (uint32_t)f->tab[*st].newstate + br_read(b, nb);
    return sym;
}

static int huf_build_from_weights(struct huf *h, const uint8_t *w, int n) {
    uint32_t total = 0;
    for (int i = 0; i < n; i++) if (w[i] > 0) total += 1u << (w[i] - 1);
    if (total == 0) return -1;
    int maxbits = 0;
    while ((1u << (maxbits + 1)) <= total) maxbits++;
    /* The LAST symbol's weight is whatever completes the power of two. */
    uint32_t left = (1u << (maxbits + 1)) - total;
    if (left == 0 || (left & (left - 1))) return -1;
    int lastw = 0;
    while ((1u << lastw) < left) lastw++;
    lastw += 1;
    if (maxbits + 1 > MAX_HUF_BITS) return -1;

    uint8_t weights[256];
    for (int i = 0; i < n; i++) weights[i] = w[i];
    weights[n] = (uint8_t)lastw;
    int nsym = n + 1;

    h->maxbits = maxbits + 1;
    int tsize = 1 << h->maxbits;
    memset(h->sym, 0, (size_t)tsize);
    memset(h->nb, 0, (size_t)tsize);

    /* Ranks first, then fill each symbol's span of the table. */
    uint32_t rank_start[MAX_HUF_BITS + 2];
    memset(rank_start, 0, sizeof rank_start);
    for (int i = 0; i < nsym; i++)
        if (weights[i] > 0) rank_start[weights[i]]++;
    /* Start index per WEIGHT, walking weights ASCENDING with a span of
     * 1 << (w-1) -- weight 1 has the longest code and comes first. Walking
     * them the other way, or with a span keyed on the code length instead
     * of the weight, leaves the offsets inconsistent with the fill below
     * and the table never sums to its size. */
    uint32_t next = 0;
    for (int r = 1; r <= h->maxbits; r++) {
        uint32_t cnt = rank_start[r];
        rank_start[r] = next;
        next += cnt << (r - 1);
    }
    if (next != (uint32_t)tsize) return -1;

    for (int i = 0; i < nsym; i++) {
        if (weights[i] == 0) continue;
        int bits = h->maxbits + 1 - weights[i];
        uint32_t span = 1u << (h->maxbits - bits);
        uint32_t start = rank_start[weights[i]];
        for (uint32_t k = 0; k < span; k++) {
            h->sym[start + k] = (uint8_t)i;
            h->nb [start + k] = (uint8_t)bits;
        }
        rank_start[weights[i]] = start + span;
    }
    return 0;
}

static int huf_decode_sym(struct huf *h, struct bitr *b, uint8_t *out) {
    /* Peek maxbits, index the flat table, then consume only the code
     * length. The PEEK is allowed to run past the end of the stream --
     * zstd pads there -- so only the position after consuming has to be
     * in range. */
    if (b->pos <= 0) return -1;
    long save = b->pos;
    b->bad = 0;
    uint32_t idx = br_read(b, h->maxbits);
    uint8_t bits = h->nb[idx];
    if (bits == 0) return -1;
    if (save - (long)bits < 0) return -1;
    b->pos = save - (long)bits;
    *out = h->sym[idx];
    return 0;
}

/* Read the Huffman description: either direct 4-bit weights or an
 * FSE-compressed weight stream. */
static int huf_read_table(struct huf *h, const uint8_t *src, size_t n, size_t *used) {
    if (n < 1) return -1;
    uint8_t hb = src[0];
    uint8_t weights[256];
    int nw;

    if (hb >= 128) {
        nw = hb - 127;
        size_t need = ((size_t)nw + 1) / 2;
        if (1 + need > n) return -1;
        for (int i = 0; i < nw; i++) {
            uint8_t byte = src[1 + i / 2];
            weights[i] = (i & 1) ? (byte & 15) : (byte >> 4);
        }
        *used = 1 + need;
    } else {
        size_t csize = hb;
        if (1 + csize > n) return -1;
        short counts[256];
        memset(counts, 0, sizeof counts);
        int maxsym = 0, log = 0;
        size_t hdr = 0;
        if (fse_read_ncount(src + 1, csize, &hdr, counts, &maxsym, &log) != 0) return -1;
        struct fse f;
        memset(&f, 0, sizeof f);
        if (fse_build(&f, counts, maxsym, log) != 0) return -1;

        /* TWO interleaved FSE streams over the weight bytes. */
        struct bitr b;
        if (br_init(&b, src + 1 + hdr, csize - hdr) != 0) { fse_free(&f); return -1; }
        uint32_t s1 = br_read(&b, log);
        uint32_t s2 = br_read(&b, log);
        /* Two interleaved states, alternating, until the stream runs out --
         * and the overflow case still emits the OTHER state, as the format
         * requires. */
        nw = 0;
        for (;;) {
            if (nw >= 255) break;
            weights[nw++] = fse_next(&f, &s1, &b);
            if (b.pos < 0) { if (nw < 255) weights[nw++] = fse_next(&f, &s2, &b); break; }
            if (nw >= 255) break;
            weights[nw++] = fse_next(&f, &s2, &b);
            if (b.pos < 0) { if (nw < 255) weights[nw++] = fse_next(&f, &s1, &b); break; }
        }
        fse_free(&f);
        *used = 1 + csize;
    }
    return huf_build_from_weights(h, weights, nw);
}

/* ---------------------------------------------------------------- XXH64
 *
 * The frame's Content_Checksum is the low 32 bits of XXH64 over the
 * decompressed content. gzip and bzip2 here both verify their checksums,
 * and `zstd -t` would mean nothing without this -- a corrupted payload
 * that still parses would decode to silent garbage and be reported fine.
 */

#define XXP1 11400714785074694791ULL
#define XXP2 14029467366897019727ULL
#define XXP3  1609587929392839161ULL
#define XXP4  9650029242287828579ULL
#define XXP5  2870177450012600261ULL

static uint64_t rol64(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t xxh64_round(uint64_t acc, uint64_t input) {
    acc += input * XXP2;
    acc = rol64(acc, 31);
    acc *= XXP1;
    return acc;
}

static uint64_t xxh64(const uint8_t *p, size_t len) {
    const uint8_t *end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = XXP1 + XXP2, v2 = XXP2, v3 = 0, v4 = 0 - XXP1;
        const uint8_t *limit = end - 32;
        do {
            v1 = xxh64_round(v1, rd64(p)); p += 8;
            v2 = xxh64_round(v2, rd64(p)); p += 8;
            v3 = xxh64_round(v3, rd64(p)); p += 8;
            v4 = xxh64_round(v4, rd64(p)); p += 8;
        } while (p <= limit);
        h = rol64(v1, 1) + rol64(v2, 7) + rol64(v3, 12) + rol64(v4, 18);
        v1 = xxh64_round(0, v1); h ^= v1; h = h * XXP1 + XXP4;
        v2 = xxh64_round(0, v2); h ^= v2; h = h * XXP1 + XXP4;
        v3 = xxh64_round(0, v3); h ^= v3; h = h * XXP1 + XXP4;
        v4 = xxh64_round(0, v4); h ^= v4; h = h * XXP1 + XXP4;
    } else {
        h = XXP5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        uint64_t k = xxh64_round(0, rd64(p));
        h ^= k;
        h = rol64(h, 27) * XXP1 + XXP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)rd32(p) * XXP1;
        h = rol64(h, 23) * XXP2 + XXP3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p) * XXP5;
        h = rol64(h, 11) * XXP1;
        p++;
    }
    h ^= h >> 33; h *= XXP2;
    h ^= h >> 29; h *= XXP3;
    h ^= h >> 32;
    return h;
}

/* ------------------------------------------------------------- output */

struct out {
    uint8_t *d;
    size_t   len, cap;
    int      err;
};

static int out_need(struct out *o, size_t n) {
    if (o->len + n <= o->cap) return 1;
    size_t nc = o->cap ? o->cap : 65536;
    while (nc < o->len + n) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(o->d, nc);
    if (!p) { o->err = 1; return 0; }
    o->d = p; o->cap = nc;
    return 1;
}

static int out_add(struct out *o, const void *p, size_t n) {
    if (!out_need(o, n)) return 0;
    memcpy(o->d + o->len, p, n);
    o->len += n;
    return 1;
}

/* ------------------------------------------------- sequence code tables */

static const uint32_t LL_BASE[36] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,18,20,22,24,28,32,40,48,64,128,256,512,1024,2048,4096,8192,16384,32768,65536
};
static const uint8_t LL_BITS[36] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,1,1,2,2,3,3,4,6,7,8,9,10,11,12,13,14,15,16
};
static const uint32_t ML_BASE[53] = {
    3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,
    35,37,39,41,43,47,51,59,67,83,99,131,259,515,1027,2051,4099,8195,16387,32771,65539
};
static const uint8_t ML_BITS[53] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,1,1,2,2,3,3,4,4,5,7,8,9,10,11,12,13,14,15,16
};

static const short LL_DEFAULT[36] = {
    4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1,-1,-1,-1,-1
};
/* The predefined Match_Length distribution: 46 positive entries and SEVEN
 * low-probability ones. Getting that boundary wrong shifts every
 * low-probability symbol by two slots, so a short file still decodes and
 * a long match silently comes out as a short one -- 40000 bytes became
 * 43 before this was right. */
static const short ML_DEFAULT[53] = {
    1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    -1,-1,-1,-1,-1,-1,-1
};
static const short OF_DEFAULT[29] = {
    1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1
};

/* ---------------------------------------------------------- the decoder */

struct dctx {
    struct fse ll, ml, of;
    int        have_ll, have_ml, have_of;
    uint32_t   rep[3];
};

static int build_default(struct fse *f, const short *counts, int maxsym, int log) {
    return fse_build(f, counts, maxsym, log);
}

/* Decode one sequences section and apply it. */
static int do_sequences(struct dctx *dc, const uint8_t *src, size_t n,
                        const uint8_t *lit, size_t litlen, struct out *o,
                        const char **why)
{
    size_t p = 0;
    if (n < 1) { *why = "truncated sequences section"; return -1; }
    uint32_t nseq = src[p++];
    if (nseq == 0) {
        /* No sequences: the literals are the whole block. */
        if (!out_add(o, lit, litlen)) { *why = "out of memory"; return -1; }
        return 0;
    }
    if (nseq == 255) {
        if (p + 2 > n) { *why = "truncated sequence count"; return -1; }
        nseq = (uint32_t)src[p] + ((uint32_t)src[p+1] << 8) + 0x7F00u;
        p += 2;
    } else if (nseq > 127) {
        if (p >= n) { *why = "truncated sequence count"; return -1; }
        nseq = ((nseq - 128) << 8) + src[p++];
    }

    if (p >= n) { *why = "missing symbol modes"; return -1; }
    uint8_t modes = src[p++];
    int ll_mode = (modes >> 6) & 3;
    int of_mode = (modes >> 4) & 3;
    int ml_mode = (modes >> 2) & 3;

    uint8_t ll_rle = 0, of_rle = 0, ml_rle = 0;
    short counts[256];
    int maxsym, log;
    size_t used;

    #define LOADTAB(mode, f, have, dflt, dmax, dlog, rlev)                    \
        do {                                                                  \
            if ((mode) == 0) {                                                \
                if (have) fse_free(&(f));                                     \
                if (build_default(&(f), (dflt), (dmax), (dlog)) != 0)          \
                    { *why = "out of memory"; return -1; }                     \
                (have) = 1;                                                    \
            } else if ((mode) == 1) {                                          \
                if (p >= n) { *why = "truncated RLE mode"; return -1; }         \
                (rlev) = src[p++];                                             \
            } else if ((mode) == 2) {                                          \
                memset(counts, 0, sizeof counts);                              \
                if (fse_read_ncount(src + p, n - p, &used, counts,             \
                                    &maxsym, &log) != 0)                       \
                    { *why = "bad FSE table"; return -1; }                     \
                p += used;                                                     \
                if (have) fse_free(&(f));                                      \
                if (fse_build(&(f), counts, maxsym, log) != 0)                  \
                    { *why = "out of memory"; return -1; }                      \
                (have) = 1;                                                     \
            } else if (!(have)) {                                               \
                *why = "repeat mode with no previous table"; return -1;         \
            }                                                                   \
        } while (0)

    LOADTAB(ll_mode, dc->ll, dc->have_ll, LL_DEFAULT, 35, 6, ll_rle);
    LOADTAB(of_mode, dc->of, dc->have_of, OF_DEFAULT, 28, 5, of_rle);
    LOADTAB(ml_mode, dc->ml, dc->have_ml, ML_DEFAULT, 52, 6, ml_rle);
    #undef LOADTAB

    if (p > n) { *why = "sequence header ran past the block"; return -1; }

    struct bitr b;
    if (br_init(&b, src + p, n - p) != 0) { *why = "bad sequence bitstream"; return -1; }

    uint32_t sll = 0, sof = 0, sml = 0;
    if (ll_mode != 1) sll = br_read(&b, dc->ll.log);
    if (of_mode != 1) sof = br_read(&b, dc->of.log);
    if (ml_mode != 1) sml = br_read(&b, dc->ml.log);

    size_t lit_pos = 0;
    for (uint32_t i = 0; i < nseq; i++) {
        uint8_t llc = (ll_mode == 1) ? ll_rle : dc->ll.tab[sll].symbol;
        uint8_t ofc = (of_mode == 1) ? of_rle : dc->of.tab[sof].symbol;
        uint8_t mlc = (ml_mode == 1) ? ml_rle : dc->ml.tab[sml].symbol;
        if (llc > 35 || mlc > 52 || ofc > 31) { *why = "sequence code out of range"; return -1; }

        /* The order matters: offset bits first, then match, then literal. */
        uint32_t off_val = (ofc ? (1u << ofc) : 1u);
        if (ofc) off_val += br_read(&b, ofc);
        uint32_t mlen = ML_BASE[mlc] + (ML_BITS[mlc] ? br_read(&b, ML_BITS[mlc]) : 0);
        uint32_t llen = LL_BASE[llc] + (LL_BITS[llc] ? br_read(&b, LL_BITS[llc]) : 0);

        uint32_t offset;
        if (off_val > 3) {
            offset = off_val - 3;
            dc->rep[2] = dc->rep[1];
            dc->rep[1] = dc->rep[0];
            dc->rep[0] = offset;
        } else {
            /* Offset_Value 1..3 names a REPEAT offset -- and it is the
             * VALUE that decides, not the code: code 1 yields value 2 or 3,
             * and code 2 already yields 4, a literal offset. Branching on
             * the code instead drifts only once a repeat appears, which is
             * why a short file decodes and a long one does not.
             * A zero literal length shifts which repeat is meant. */
            uint32_t idx = off_val;
            if (llen == 0) idx++;
            if (idx == 1) {
                offset = dc->rep[0];
            } else if (idx == 2) {
                offset = dc->rep[1];
                dc->rep[1] = dc->rep[0];
                dc->rep[0] = offset;
            } else if (idx == 3) {
                offset = dc->rep[2];
                dc->rep[2] = dc->rep[1];
                dc->rep[1] = dc->rep[0];
                dc->rep[0] = offset;
            } else {
                offset = dc->rep[0] - 1;
                dc->rep[2] = dc->rep[1];
                dc->rep[1] = dc->rep[0];
                dc->rep[0] = offset;
            }
        }
        if (offset == 0) { *why = "zero offset"; return -1; }

        if (lit_pos + llen > litlen) { *why = "literal length past the section"; return -1; }
        if (!out_add(o, lit + lit_pos, llen)) { *why = "out of memory"; return -1; }
        lit_pos += llen;

        if ((size_t)offset > o->len) { *why = "offset points before the output"; return -1; }
        if (!out_need(o, mlen)) { *why = "out of memory"; return -1; }
        /* Byte by byte on purpose: an overlapping match (offset < length)
         * is legal and is how runs are encoded. */
        for (uint32_t k = 0; k < mlen; k++) {
            o->d[o->len] = o->d[o->len - offset];
            o->len++;
        }

        if (i + 1 < nseq) {
            if (ll_mode != 1) sll = (uint32_t)dc->ll.tab[sll].newstate + br_read(&b, dc->ll.tab[sll].nbits);
            if (ml_mode != 1) sml = (uint32_t)dc->ml.tab[sml].newstate + br_read(&b, dc->ml.tab[sml].nbits);
            if (of_mode != 1) sof = (uint32_t)dc->of.tab[sof].newstate + br_read(&b, dc->of.tab[sof].nbits);
        }
    }

    /* Whatever literals the sequences did not consume are appended. */
    if (lit_pos < litlen && !out_add(o, lit + lit_pos, litlen - lit_pos)) {
        *why = "out of memory";
        return -1;
    }
    return 0;
}

/* Decode a compressed block's literals section. */
static int do_literals(struct dctx *dc, struct huf *huf, int *huf_ready,
                       const uint8_t *src, size_t n, uint8_t **lit,
                       size_t *litlen, size_t *used, const char **why)
{
    (void)dc;
    if (n < 1) { *why = "truncated literals"; return -1; }
    uint8_t b0 = src[0];
    int type = b0 & 3;
    int fmt  = (b0 >> 2) & 3;
    size_t regen = 0, comp = 0, hdr = 0;
    int nstreams = 1;

    if (type == 0 || type == 1) {
        if (fmt == 0 || fmt == 2)      { regen = b0 >> 3; hdr = 1; }
        else if (fmt == 1)             { if (n < 2) { *why = "truncated literals"; return -1; }
                                         regen = (size_t)(b0 >> 4) | ((size_t)src[1] << 4); hdr = 2; }
        else                           { if (n < 3) { *why = "truncated literals"; return -1; }
                                         regen = (size_t)(b0 >> 4) | ((size_t)src[1] << 4)
                                               | ((size_t)src[2] << 12); hdr = 3; }
        if (type == 0) {
            if (hdr + regen > n) { *why = "raw literals past the block"; return -1; }
            uint8_t *L = (uint8_t *)malloc(regen ? regen : 1);
            if (!L) { *why = "out of memory"; return -1; }
            memcpy(L, src + hdr, regen);
            *lit = L; *litlen = regen; *used = hdr + regen;
            return 0;
        }
        if (hdr >= n) { *why = "truncated RLE literals"; return -1; }
        uint8_t *L = (uint8_t *)malloc(regen ? regen : 1);
        if (!L) { *why = "out of memory"; return -1; }
        memset(L, src[hdr], regen);
        *lit = L; *litlen = regen; *used = hdr + 1;
        return 0;
    }

    /* Compressed (2) or treeless (3). */
    if (fmt == 0)      { if (n < 3) { *why = "truncated literals"; return -1; }
                         regen = ((size_t)(b0 >> 4)) | ((size_t)(src[1] & 0x3F) << 4);
                         comp  = ((size_t)(src[1] >> 6)) | ((size_t)src[2] << 2);
                         hdr = 3; nstreams = 1; }
    else if (fmt == 1) { if (n < 3) { *why = "truncated literals"; return -1; }
                         regen = ((size_t)(b0 >> 4)) | ((size_t)(src[1] & 0x3F) << 4);
                         comp  = ((size_t)(src[1] >> 6)) | ((size_t)src[2] << 2);
                         hdr = 3; nstreams = 4; }
    else if (fmt == 2) { if (n < 4) { *why = "truncated literals"; return -1; }
                         regen = ((size_t)(b0 >> 4)) | ((size_t)src[1] << 4)
                               | ((size_t)(src[2] & 3) << 12);
                         comp  = ((size_t)(src[2] >> 2)) | ((size_t)src[3] << 6);
                         hdr = 4; nstreams = 4; }
    else               { if (n < 5) { *why = "truncated literals"; return -1; }
                         regen = ((size_t)(b0 >> 4)) | ((size_t)src[1] << 4)
                               | ((size_t)(src[2] & 0x3F) << 12);
                         comp  = ((size_t)(src[2] >> 6)) | ((size_t)src[3] << 2)
                               | ((size_t)src[4] << 10);
                         hdr = 5; nstreams = 4; }

    if (hdr + comp > n) { *why = "compressed literals past the block"; return -1; }
    const uint8_t *body = src + hdr;
    size_t bodylen = comp;

    if (type == 2) {
        size_t tused = 0;
        if (huf_read_table(huf, body, bodylen, &tused) != 0) {
            *why = "bad Huffman table";
            return -1;
        }
        *huf_ready = 1;
        body += tused;
        bodylen -= tused;
    } else if (!*huf_ready) {
        *why = "treeless literals with no previous Huffman table";
        return -1;
    }

    uint8_t *L = (uint8_t *)malloc(regen ? regen : 1);
    if (!L) { *why = "out of memory"; return -1; }
    size_t produced = 0;

    if (nstreams == 1) {
        struct bitr br;
        if (br_init(&br, body, bodylen) != 0) { free(L); *why = "bad literal stream"; return -1; }
        while (produced < regen) {
            uint8_t s;
            if (huf_decode_sym(huf, &br, &s) != 0) break;
            L[produced++] = s;
        }
    } else {
        if (bodylen < 6) { free(L); *why = "truncated four-stream literals"; return -1; }
        size_t s1 = (size_t)body[0] | ((size_t)body[1] << 8);
        size_t s2 = (size_t)body[2] | ((size_t)body[3] << 8);
        size_t s3 = (size_t)body[4] | ((size_t)body[5] << 8);
        const uint8_t *p0 = body + 6;
        if (s1 + s2 + s3 > bodylen - 6) { free(L); *why = "stream sizes past the block"; return -1; }
        size_t s4 = bodylen - 6 - s1 - s2 - s3;
        const uint8_t *starts[4] = { p0, p0 + s1, p0 + s1 + s2, p0 + s1 + s2 + s3 };
        size_t sizes[4] = { s1, s2, s3, s4 };
        /* The output is split into four equal parts, the last taking the
         * remainder. */
        size_t part = (regen + 3) / 4;
        for (int k = 0; k < 4 && produced < regen; k++) {
            struct bitr br;
            if (br_init(&br, starts[k], sizes[k]) != 0) break;
            size_t want = (k == 3) ? (regen - 3 * part) : part;
            for (size_t q = 0; q < want && produced < regen; q++) {
                uint8_t s;
                if (huf_decode_sym(huf, &br, &s) != 0) break;
                L[produced++] = s;
            }
        }
    }

    if (produced != regen) { free(L); *why = "literals section did not decode fully"; return -1; }
    *lit = L; *litlen = regen; *used = hdr + comp;
    return 0;
}

int zstd_decompress(const uint8_t *in, size_t inlen,
                    uint8_t **out, size_t *outlen, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "ok";
    if (!in || !out || !outlen) { *why = "bad arguments"; return -1; }

    struct out o;
    memset(&o, 0, sizeof o);
    struct dctx dc;
    memset(&dc, 0, sizeof dc);
    dc.rep[0] = 1; dc.rep[1] = 4; dc.rep[2] = 8;
    struct huf huf;
    memset(&huf, 0, sizeof huf);
    int huf_ready = 0;

    size_t p = 0;
    int frames = 0;

    while (p + 4 <= inlen) {
        uint32_t magic = (uint32_t)in[p] | ((uint32_t)in[p+1] << 8)
                       | ((uint32_t)in[p+2] << 16) | ((uint32_t)in[p+3] << 24);
        p += 4;
        if ((magic & ZSTD_SKIP_MASK) == ZSTD_SKIP_MAGIC) {
            /* A skippable frame carries a size and no content we want. */
            if (p + 4 > inlen) { *why = "truncated skippable frame"; goto fail; }
            uint32_t sz = (uint32_t)in[p] | ((uint32_t)in[p+1] << 8)
                        | ((uint32_t)in[p+2] << 16) | ((uint32_t)in[p+3] << 24);
            p += 4 + sz;
            continue;
        }
        if (magic != ZSTD_MAGIC) { *why = "not in zstd format"; goto fail; }
        frames++;
        size_t frame_out_start = o.len;

        if (p >= inlen) { *why = "truncated frame header"; goto fail; }
        uint8_t fhd = in[p++];
        int fcs_flag = (fhd >> 6) & 3;
        int single   = (fhd >> 5) & 1;
        int checksum = (fhd >> 2) & 1;
        int did_flag = fhd & 3;
        if ((fhd >> 3) & 1) { *why = "reserved bit set in frame header"; goto fail; }

        if (!single) { if (p >= inlen) { *why = "truncated window descriptor"; goto fail; } p++; }
        static const int did_size[4] = { 0, 1, 2, 4 };
        p += (size_t)did_size[did_flag];
        int fcs_size = (fcs_flag == 0) ? (single ? 1 : 0)
                     : (fcs_flag == 1) ? 2 : (fcs_flag == 2) ? 4 : 8;
        p += (size_t)fcs_size;
        if (p > inlen) { *why = "truncated frame header"; goto fail; }

        for (;;) {
            if (p + 3 > inlen) { *why = "truncated block header"; goto fail; }
            uint32_t bh = (uint32_t)in[p] | ((uint32_t)in[p+1] << 8) | ((uint32_t)in[p+2] << 16);
            p += 3;
            int last  = bh & 1;
            int btype = (bh >> 1) & 3;
            size_t bsize = bh >> 3;

            if (btype == 0) {
                if (p + bsize > inlen) { *why = "raw block past the input"; goto fail; }
                if (!out_add(&o, in + p, bsize)) { *why = "out of memory"; goto fail; }
                p += bsize;
            } else if (btype == 1) {
                if (p >= inlen) { *why = "truncated RLE block"; goto fail; }
                if (!out_need(&o, bsize)) { *why = "out of memory"; goto fail; }
                memset(o.d + o.len, in[p], bsize);
                o.len += bsize;
                p += 1;
            } else if (btype == 2) {
                if (p + bsize > inlen) { *why = "compressed block past the input"; goto fail; }
                uint8_t *lit = NULL;
                size_t litlen = 0, lused = 0;
                if (do_literals(&dc, &huf, &huf_ready, in + p, bsize,
                                &lit, &litlen, &lused, why) != 0) goto fail;
                if (do_sequences(&dc, in + p + lused, bsize - lused,
                                 lit, litlen, &o, why) != 0) { free(lit); goto fail; }
                free(lit);
                p += bsize;
            } else {
                *why = "reserved block type";
                goto fail;
            }
            if (last) break;
        }
        if (checksum) {
            if (p + 4 > inlen) { *why = "truncated content checksum"; goto fail; }
            uint32_t stored = (uint32_t)in[p] | ((uint32_t)in[p+1] << 8)
                            | ((uint32_t)in[p+2] << 16) | ((uint32_t)in[p+3] << 24);
            uint32_t got = (uint32_t)(xxh64(o.d + frame_out_start,
                                            o.len - frame_out_start) & 0xFFFFFFFFu);
            if (stored != got) { *why = "content checksum mismatch"; goto fail; }
            p += 4;
        }
    }

    if (!frames) { *why = "not in zstd format"; goto fail; }
    if (dc.have_ll) fse_free(&dc.ll);
    if (dc.have_ml) fse_free(&dc.ml);
    if (dc.have_of) fse_free(&dc.of);
    if (o.err) { *why = "out of memory"; free(o.d); return -1; }
    *out = o.d;
    *outlen = o.len;
    return 0;

fail:
    if (dc.have_ll) fse_free(&dc.ll);
    if (dc.have_ml) fse_free(&dc.ml);
    if (dc.have_of) fse_free(&dc.of);
    free(o.d);
    return -1;
}

/* ------------------------------------------------------------- encoder */

int zstd_compress(const uint8_t *in, size_t inlen,
                  uint8_t **out, size_t *outlen, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "ok";
    if (!out || !outlen || (!in && inlen)) { *why = "bad arguments"; return -1; }

    /* RAW blocks only -- a valid frame every zstd decodes, but the bytes
     * are not compressed. See the note at the top of this file. */
    struct out o;
    memset(&o, 0, sizeof o);
    /* A SINGLE-SEGMENT frame carrying the real content size. The first
     * version wrote a window descriptor of 0 -- a 1 KiB window -- while
     * emitting 64 KiB blocks, which real zstd rejects as corrupt the
     * moment a file needs more than one block. Single-segment says "the
     * window is the whole content", so there is no window to get wrong. */
    uint8_t hdr[16];
    size_t hn = 0;
    hdr[hn++] = 0x28; hdr[hn++] = 0xB5; hdr[hn++] = 0x2F; hdr[hn++] = 0xFD;
    int fcs_flag;
    if (inlen <= 255)              fcs_flag = 0;   /* 1 byte  */
    else if (inlen <= 65535 + 256) fcs_flag = 1;   /* 2 bytes, biased by 256 */
    else if (inlen <= 0xFFFFFFFFu) fcs_flag = 2;   /* 4 bytes */
    else                           fcs_flag = 3;   /* 8 bytes */
    hdr[hn++] = (uint8_t)((fcs_flag << 6) | (1 << 5));   /* single segment */
    if (fcs_flag == 0) {
        hdr[hn++] = (uint8_t)inlen;
    } else if (fcs_flag == 1) {
        uint32_t v = (uint32_t)(inlen - 256);
        hdr[hn++] = (uint8_t)(v & 0xFF);
        hdr[hn++] = (uint8_t)((v >> 8) & 0xFF);
    } else if (fcs_flag == 2) {
        uint32_t v = (uint32_t)inlen;
        for (int k = 0; k < 4; k++) hdr[hn++] = (uint8_t)((v >> (8 * k)) & 0xFF);
    } else {
        uint64_t v = (uint64_t)inlen;
        for (int k = 0; k < 8; k++) hdr[hn++] = (uint8_t)((v >> (8 * k)) & 0xFF);
    }
    if (!out_add(&o, hdr, hn)) { *why = "out of memory"; free(o.d); return -1; }

    size_t p = 0;
    do {
        size_t n = inlen - p;
        if (n > 65535) n = 65535;
        int last = (p + n >= inlen);
        uint32_t bh = (uint32_t)((n << 3) | (0u << 1) | (uint32_t)(last ? 1 : 0));
        uint8_t bb[3] = { (uint8_t)(bh & 0xFF), (uint8_t)((bh >> 8) & 0xFF),
                          (uint8_t)((bh >> 16) & 0xFF) };
        if (!out_add(&o, bb, 3)) { *why = "out of memory"; free(o.d); return -1; }
        if (n && !out_add(&o, in + p, n)) { *why = "out of memory"; free(o.d); return -1; }
        p += n;
    } while (p < inlen);

    if (o.err) { *why = "out of memory"; free(o.d); return -1; }
    *out = o.d;
    *outlen = o.len;
    return 0;
}
