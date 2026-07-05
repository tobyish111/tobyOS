/* puff.c -- compact DEFLATE (RFC 1951) inflater + gzip/zlib wrappers.
 *
 * Derived from Mark Adler's public-domain "puff" reference inflater
 * (zlib/contrib/puff), trimmed to the kernel's needs and modified so a
 * full output buffer is a graceful stop (return PUFF_TRUNC) rather than
 * an error -- the browser only wants the first N KiB of a decompressed
 * page, so truncating the *decompressed* output is exactly right.
 *
 * No libc beyond what klibc provides; no allocation (caller supplies the
 * output buffer). Single-shot: the whole compressed input is in memory.
 */

#include <tobyos/puff.h>
#include <tobyos/types.h>

#define MAXBITS   15            /* max bits in a code */
#define MAXLCODES 286           /* max number of literal/length codes */
#define MAXDCODES 30            /* max number of distance codes */
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288           /* number of fixed literal/length codes */

struct state {
    uint8_t       *out;         /* output buffer */
    unsigned long  outlen;      /* capacity */
    unsigned long  outcnt;      /* bytes written so far */
    int            full;        /* output filled -> truncated */

    const uint8_t *in;          /* input buffer */
    unsigned long  inlen;
    unsigned long  incnt;
    int            bitbuf;      /* bit accumulator */
    int            bitcnt;      /* bits in accumulator */
};

struct huffman {
    short *count;               /* number of symbols of each length */
    short *symbol;              /* canonically ordered symbols */
};

/* Return `need` bits from the stream, LSB first. -1 on input underrun. */
static int bits(struct state *s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) return -1;
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

/* Copy a stored (uncompressed) block. */
static int stored(struct state *s) {
    s->bitbuf = 0; s->bitcnt = 0;                 /* to byte boundary */
    if (s->incnt + 4 > s->inlen) return 2;
    unsigned len = s->in[s->incnt++];
    len |= s->in[s->incnt++] << 8;
    /* skip the one's complement length */
    s->incnt += 2;
    if (s->incnt + len > s->inlen) return 2;
    while (len--) {
        if (s->outcnt < s->outlen) s->out[s->outcnt++] = s->in[s->incnt];
        else s->full = 1;
        s->incnt++;
    }
    return 0;
}

/* Decode one symbol using huffman table h. -10 on invalid/underrun. */
static int decode(struct state *s, const struct huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0) return -10;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -9;                                     /* ran out of codes */
}

/* Build a huffman table from a list of code lengths. */
static int construct(struct huffman *h, const short *length, int n) {
    int len, left;
    short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;               /* no codes */
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;                /* over-subscribed */
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static const short lens[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dists[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const short dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* Decode literal/length + distance codes until an end-of-block. */
static int codes(struct state *s, const struct huffman *lencode,
                 const struct huffman *distcode) {
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0) return symbol;
        if (symbol < 256) {                       /* literal */
            if (s->outcnt < s->outlen) s->out[s->outcnt++] = (uint8_t)symbol;
            else { s->full = 1; return 0; }       /* truncated: stop clean */
        } else if (symbol > 256) {                /* length */
            symbol -= 257;
            if (symbol >= 29) return -10;
            int b = bits(s, lext[symbol]); if (b < 0) return -10;
            int len = lens[symbol] + b;
            symbol = decode(s, distcode);
            if (symbol < 0) return symbol;
            b = bits(s, dext[symbol]); if (b < 0) return -10;
            unsigned dist = dists[symbol] + b;
            if (dist > s->outcnt) return -11;      /* before start of output */
            while (len--) {
                if (s->outcnt < s->outlen) {
                    s->out[s->outcnt] = s->out[s->outcnt - dist];
                    s->outcnt++;
                } else { s->full = 1; return 0; }  /* truncated: stop clean */
            }
        }
    } while (symbol != 256);
    return 0;
}

static int fixed(struct state *s) {
    static short lencnt[MAXBITS+1], lensym[FIXLCODES];
    static short distcnt[MAXBITS+1], distsym[MAXDCODES];
    static int   built = 0;
    static struct huffman lencode, distcode;
    if (!built) {
        short lengths[FIXLCODES];
        int symbol;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        lencode.count = lencnt; lencode.symbol = lensym;
        construct(&lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        distcode.count = distcnt; distcode.symbol = distsym;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(struct state *s) {
    short lengths[MAXCODES];
    short lencnt[MAXBITS+1], lensym[MAXLCODES];
    short distcnt[MAXBITS+1], distsym[MAXDCODES];
    struct huffman lencode = { lencnt, lensym };
    struct huffman distcode = { distcnt, distsym };
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

    int nlen  = bits(s, 5); if (nlen  < 0) return -10; nlen  += 257;
    int ndist = bits(s, 5); if (ndist < 0) return -10; ndist += 1;
    int ncode = bits(s, 4); if (ncode < 0) return -10; ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

    int index;
    for (index = 0; index < ncode; index++) {
        int b = bits(s, 3); if (b < 0) return -10;
        lengths[order[index]] = (short)b;
    }
    for (; index < 19; index++) lengths[order[index]] = 0;

    int err = construct(&lencode, lengths, 19);
    if (err != 0) return -4;

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0) return symbol;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            int len = 0;
            if (symbol == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                int b = bits(s, 2); if (b < 0) return -10; symbol = 3 + b;
            } else if (symbol == 17) {
                int b = bits(s, 3); if (b < 0) return -10; symbol = 3 + b;
            } else {
                int b = bits(s, 7); if (b < 0) return -10; symbol = 11 + b;
            }
            if (index + symbol > nlen + ndist) return -6;
            while (symbol--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -9;             /* no end-of-block code */

    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;

    return codes(s, &lencode, &distcode);
}

/* Raw DEFLATE. Returns PUFF_OK, PUFF_TRUNC, or PUFF_ERR. *destlen is set
 * to the number of bytes written. */
static int puff_raw(uint8_t *dest, unsigned long *destlen,
                    const uint8_t *source, unsigned long sourcelen) {
    struct state s = { dest, *destlen, 0, 0, source, sourcelen, 0, 0, 0 };
    int last, err = 0;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0) { err = -1; break; }
        if      (type == 0) err = stored(&s);
        else if (type == 1) err = fixed(&s);
        else if (type == 2) err = dynamic(&s);
        else { err = -1; break; }
        if (s.full) break;                        /* output-buffer full */
        if (err != 0) break;
    } while (!last);
    *destlen = s.outcnt;
    if (s.full) return PUFF_TRUNC;
    return err ? PUFF_ERR : PUFF_OK;
}

int puff_gzip(uint8_t *dest, unsigned long *destlen,
              const uint8_t *src, unsigned long srclen) {
    if (srclen < 18) return PUFF_ERR;             /* header(10)+trailer(8) */
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) return PUFF_ERR;
    uint8_t flg = src[3];
    unsigned long p = 10;
    if (flg & 0x04) {                             /* FEXTRA */
        if (p + 2 > srclen) return PUFF_ERR;
        unsigned xlen = src[p] | (src[p+1] << 8);
        p += 2 + xlen;
    }
    if (flg & 0x08) { while (p < srclen && src[p]) p++; p++; }   /* FNAME */
    if (flg & 0x10) { while (p < srclen && src[p]) p++; p++; }   /* FCOMMENT */
    if (flg & 0x02) p += 2;                       /* FHCRC */
    if (p >= srclen) return PUFF_ERR;
    unsigned long deflate_len = (srclen - p >= 8) ? (srclen - p - 8)
                                                  : (srclen - p);
    return puff_raw(dest, destlen, src + p, deflate_len);
}

int puff_zlib(uint8_t *dest, unsigned long *destlen,
              const uint8_t *src, unsigned long srclen) {
    if (srclen < 6) return PUFF_ERR;
    if ((src[0] & 0x0f) != 8) return PUFF_ERR;    /* CM=deflate */
    unsigned long body = srclen - 2;
    if (src[1] & 0x20) body -= 4;                 /* FDICT preset (rare) */
    return puff_raw(dest, destlen, src + 2, body - 4 /* adler32 trailer */);
}
