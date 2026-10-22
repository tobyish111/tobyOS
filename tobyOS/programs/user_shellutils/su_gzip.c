/* su_gzip.c -- gzip, gunzip and zcat, as GNU gzip 1.14's
 * (logs/utilgate/staging/gzip.sh, gunzip.sh, zcat.sh).
 *
 * Compressing: the deflate stream is gzip's own, byte for byte, at -1 to
 * -9 and with --rsyncable -- measured on some 2,200 inputs. The choices
 * that make it so were found by measurement: hash chains of three bytes
 * (15-bit hash, shift 5) over a 64 KiB window slid by 32 KiB; at -1..-3
 * the greedy coder, at -4..-9 the lazy one with gzip's table of good,
 * lazy, nice and chain lengths; position 0 never a match source, no
 * match sought past 64 KiB - 262 before the window slides, a lazy match
 * of three bytes dropped when farther than 4096; a block ends at 32767
 * symbols or 32768 distances, or (above -2) every 4096 symbols when few
 * are matches and the estimate is under half the input; each block is
 * stored, fixed or dynamic, the smallest (fixed on a tie, stored only
 * when the window still holds the block). --rsyncable ends a block where
 * a rolling sum of the last 4096 bytes is a multiple of 4096, and pads
 * to a byte boundary with an empty stored block when not on one.
 *
 * Decompressing: gzip members, zip's first member (deflated or stored),
 * compress(1)'s .Z and pack(1)'s .z; trailing zeros and garbage, -f's
 * pass-through, and what gzip writes before a failure (the window so far
 * at an unexpected end, whole windows at a format violation). An SCO
 * "compress -H" (LZH) file is refused: no reference to make one from.
 *
 * The rest is gzip's flow as measured: GZIP's environment options (only
 * the level and --rsyncable count), the suffix list and its case, -N/-n,
 * the links, modes and times, -l with its quirks (it decompresses; the
 * totals line repeats after trailing data; header bytes only for a lone
 * member), the warnings that make the status 2 and the ones -q or -r
 * silence. gunzip and zcat are GNU's scripts: --help and --version as
 * their first argument, everything else gzip -d / gzip -cd by gzip's
 * name. Ratios are printf's "%5.1f" of a double, done in integers (the
 * tobyOS build has no floating point). */

#include "su_czcommon.c"

/* ======================================================================
 *  deflate, compressing
 * ====================================================================== */
#define CZ_WSIZE 32768u
#define CZ_WMASK (CZ_WSIZE - 1)
#define CZ_HBITS 15
#define CZ_HSIZE (1u << CZ_HBITS)
#define CZ_HMASK (CZ_HSIZE - 1)
#define CZ_HSHIFT 5
#define CZ_MINM 3
#define CZ_MAXM 258
#define CZ_MINLA (CZ_MAXM + CZ_MINM + 1)
#define CZ_MAXDIST (CZ_WSIZE - CZ_MINLA)
#define CZ_TOOFAR 4096
#define CZ_LITBUF 0x8000u
#define CZ_LCODES 286
#define CZ_DCODES 30
#define CZ_BLCODES 19
#define CZ_HEAPSZ (2 * CZ_LCODES + 1)
#define CZ_ENDBLK 256
#define CZ_RSWIN 4096u
#define CZ_RSNONE 0xFFFFFFFFu

struct cz_tn { uint16_t freq; uint16_t code; uint16_t dad; uint16_t len; };

struct cz_tdesc {
    struct cz_tn *tree;
    const struct cz_tn *stree;
    const int *extra;
    int base, elems, maxlen, maxcode;
};

static const int cz_xlbits[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const int cz_xdbits[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const int cz_xblbits[19] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,3,7};
static const uint8_t cz_blorder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static uint8_t cz_lencode[256];
static int cz_baselen[29];
static uint8_t cz_distcode[512];
static int cz_basedist[30];
static struct cz_tn cz_sltree[288];
static struct cz_tn cz_sdtree[30];
static int cz_trees_ready;

struct cz_def {
    /* input */
    int (*rd)(void *ctx, uint8_t *buf, unsigned n);   /* bytes read, 0 at EOF, -1 error */
    void *rctx;
    int eofile, rderr;
    /* output */
    void (*wr)(void *ctx, const uint8_t *buf, size_t n);
    void *wctx;
    uint8_t obuf[16384];
    size_t on;
    uint32_t bibuf;
    int bivalid;
    /* the window */
    uint8_t window[2 * CZ_WSIZE + CZ_MAXM + 8];
    uint16_t prev[CZ_WSIZE];
    uint16_t head[CZ_HSIZE];
    unsigned ins_h, strstart, lookahead, match_start, prev_length;
    unsigned prev_match;
    long block_start;
    int level;
    unsigned good, lazy, nice, chain;
    int rsync;
    uint32_t rsync_sum, rsync_end;
    /* tallies */
    uint8_t lbuf[CZ_LITBUF];
    uint16_t dbuf[CZ_LITBUF];
    uint8_t isdist[CZ_LITBUF];
    unsigned last_lit, last_dist;
    /* trees */
    struct cz_tn dltree[CZ_HEAPSZ];
    struct cz_tn ddtree[2 * CZ_DCODES + 1];
    struct cz_tn bltree[2 * CZ_BLCODES + 1];
    struct cz_tdesc ld, dd, bd;
    uint16_t blcount[16];
    int heap[2 * CZ_LCODES + 1];
    int heaplen, heapmax;
    uint8_t depth[2 * CZ_LCODES + 1];
    unsigned long opt_len, static_len;
};

static unsigned cz_bitrev(unsigned code, int len) {
    unsigned r = 0;
    do { r |= code & 1; code >>= 1; r <<= 1; } while (--len > 0);
    return r >> 1;
}

static void cz_gen_codes(struct cz_tn *tree, int maxcode, const uint16_t *blcount) {
    uint16_t next[16];
    unsigned code = 0;
    for (int bits = 1; bits <= 15; bits++) {
        code = (code + blcount[bits - 1]) << 1;
        next[bits] = (uint16_t)code;
    }
    for (int n = 0; n <= maxcode; n++) {
        int len = tree[n].len;
        if (len == 0) continue;
        tree[n].code = (uint16_t)cz_bitrev(next[len]++, len);
    }
}

static void cz_trees_init(void) {
    if (cz_trees_ready) return;
    int length = 0, code;
    for (code = 0; code < 28; code++) {
        cz_baselen[code] = length;
        for (int n = 0; n < (1 << cz_xlbits[code]); n++) cz_lencode[length++] = (uint8_t)code;
    }
    cz_lencode[length - 1] = (uint8_t)code;
    cz_baselen[28] = 255;   /* not used: length 258 has its own code */
    int dist = 0;
    for (code = 0; code < 16; code++) {
        cz_basedist[code] = dist;
        for (int n = 0; n < (1 << cz_xdbits[code]); n++) cz_distcode[dist++] = (uint8_t)code;
    }
    dist >>= 7;
    for (; code < 30; code++) {
        cz_basedist[code] = dist << 7;
        for (int n = 0; n < (1 << (cz_xdbits[code] - 7)); n++) cz_distcode[256 + dist++] = (uint8_t)code;
    }
    uint16_t blc[16] = {0};
    int n = 0;
    while (n <= 143) cz_sltree[n++].len = 8, blc[8]++;
    while (n <= 255) cz_sltree[n++].len = 9, blc[9]++;
    while (n <= 279) cz_sltree[n++].len = 7, blc[7]++;
    while (n <= 287) cz_sltree[n++].len = 8, blc[8]++;
    cz_gen_codes(cz_sltree, 287, blc);
    for (n = 0; n < 30; n++) {
        cz_sdtree[n].len = 5;
        cz_sdtree[n].code = (uint16_t)cz_bitrev((unsigned)n, 5);
    }
    cz_trees_ready = 1;
}

static int cz_dcode(unsigned dist) {
    return dist < 256 ? cz_distcode[dist] : cz_distcode[256 + (dist >> 7)];
}

/* ---- bit output ---- */
static void cz_oflush(struct cz_def *s) {
    if (s->on) s->wr(s->wctx, s->obuf, s->on);
    s->on = 0;
}
static void cz_obyte(struct cz_def *s, unsigned b) {
    if (s->on == sizeof s->obuf) cz_oflush(s);
    s->obuf[s->on++] = (uint8_t)b;
}
static void cz_send(struct cz_def *s, unsigned value, int len) {
    s->bibuf |= (uint32_t)value << s->bivalid;
    s->bivalid += len;
    while (s->bivalid >= 8) {
        cz_obyte(s, s->bibuf & 0xff);
        s->bibuf >>= 8;
        s->bivalid -= 8;
    }
}
static void cz_windup(struct cz_def *s) {
    if (s->bivalid > 0) cz_obyte(s, s->bibuf & 0xff);
    s->bibuf = 0;
    s->bivalid = 0;
}

/* ---- Huffman trees ---- */
static void cz_init_block(struct cz_def *s) {
    for (int n = 0; n < CZ_LCODES; n++) s->dltree[n].freq = 0;
    for (int n = 0; n < CZ_DCODES; n++) s->ddtree[n].freq = 0;
    for (int n = 0; n < CZ_BLCODES; n++) s->bltree[n].freq = 0;
    s->dltree[CZ_ENDBLK].freq = 1;
    s->opt_len = s->static_len = 0;
    s->last_lit = s->last_dist = 0;
}

#define CZ_SMALLER(t, n, m) ((t)[n].freq < (t)[m].freq || \
    ((t)[n].freq == (t)[m].freq && s->depth[n] <= s->depth[m]))

static void cz_downheap(struct cz_def *s, struct cz_tn *tree, int k) {
    int v = s->heap[k];
    int j = k << 1;
    while (j <= s->heaplen) {
        if (j < s->heaplen && CZ_SMALLER(tree, s->heap[j + 1], s->heap[j])) j++;
        if (CZ_SMALLER(tree, v, s->heap[j])) break;
        s->heap[k] = s->heap[j];
        k = j;
        j <<= 1;
    }
    s->heap[k] = v;
}

static void cz_gen_bitlen(struct cz_def *s, struct cz_tdesc *d) {
    struct cz_tn *tree = d->tree;
    int h, n, m, bits, xbits, overflow = 0;
    for (bits = 0; bits <= 15; bits++) s->blcount[bits] = 0;
    tree[s->heap[s->heapmax]].len = 0;
    for (h = s->heapmax + 1; h < CZ_HEAPSZ; h++) {
        n = s->heap[h];
        bits = tree[tree[n].dad].len + 1;
        if (bits > d->maxlen) bits = d->maxlen, overflow++;
        tree[n].len = (uint16_t)bits;
        if (n > d->maxcode) continue;
        s->blcount[bits]++;
        xbits = 0;
        if (n >= d->base) xbits = d->extra[n - d->base];
        unsigned long f = tree[n].freq;
        s->opt_len += f * (unsigned long)(bits + xbits);
        if (d->stree) s->static_len += f * (unsigned long)(d->stree[n].len + xbits);
    }
    if (overflow == 0) return;
    do {
        bits = d->maxlen - 1;
        while (s->blcount[bits] == 0) bits--;
        s->blcount[bits]--;
        s->blcount[bits + 1] += 2;
        s->blcount[d->maxlen]--;
        overflow -= 2;
    } while (overflow > 0);
    for (bits = d->maxlen; bits != 0; bits--) {
        n = s->blcount[bits];
        while (n != 0) {
            m = s->heap[--h];
            if (m > d->maxcode) continue;
            if (tree[m].len != (unsigned)bits) {
                s->opt_len += ((unsigned long)bits - tree[m].len) * tree[m].freq;
                tree[m].len = (uint16_t)bits;
            }
            n--;
        }
    }
}

static void cz_build_tree(struct cz_def *s, struct cz_tdesc *d) {
    struct cz_tn *tree = d->tree;
    int elems = d->elems, n, m, maxcode = -1, node = elems;
    s->heaplen = 0;
    s->heapmax = CZ_HEAPSZ;
    for (n = 0; n < elems; n++) {
        if (tree[n].freq != 0) {
            s->heap[++s->heaplen] = maxcode = n;
            s->depth[n] = 0;
        } else tree[n].len = 0;
    }
    while (s->heaplen < 2) {
        int nw = s->heap[++s->heaplen] = (maxcode < 2 ? ++maxcode : 0);
        tree[nw].freq = 1;
        s->depth[nw] = 0;
        s->opt_len--;
        if (d->stree) s->static_len -= d->stree[nw].len;
    }
    d->maxcode = maxcode;
    for (n = s->heaplen / 2; n >= 1; n--) cz_downheap(s, tree, n);
    do {
        n = s->heap[1];
        s->heap[1] = s->heap[s->heaplen--];
        cz_downheap(s, tree, 1);
        m = s->heap[1];
        s->heap[--s->heapmax] = n;
        s->heap[--s->heapmax] = m;
        tree[node].freq = (uint16_t)(tree[n].freq + tree[m].freq);
        s->depth[node] = (uint8_t)((s->depth[n] >= s->depth[m] ? s->depth[n] : s->depth[m]) + 1);
        tree[n].dad = tree[m].dad = (uint16_t)node;
        s->heap[1] = node++;
        cz_downheap(s, tree, 1);
    } while (s->heaplen >= 2);
    s->heap[--s->heapmax] = s->heap[1];
    cz_gen_bitlen(s, d);
    cz_gen_codes(tree, maxcode, s->blcount);
}

static void cz_scan_tree(struct cz_def *s, struct cz_tn *tree, int maxcode) {
    int prevlen = -1, curlen, nextlen = tree[0].len, count = 0;
    int max_count = 7, min_count = 4;
    if (nextlen == 0) max_count = 138, min_count = 3;
    tree[maxcode + 1].len = 0xffff;
    for (int n = 0; n <= maxcode; n++) {
        curlen = nextlen;
        nextlen = tree[n + 1].len;
        if (++count < max_count && curlen == nextlen) continue;
        else if (count < min_count) s->bltree[curlen].freq = (uint16_t)(s->bltree[curlen].freq + count);
        else if (curlen != 0) {
            if (curlen != prevlen) s->bltree[curlen].freq++;
            s->bltree[16].freq++;
        } else if (count <= 10) s->bltree[17].freq++;
        else s->bltree[18].freq++;
        count = 0;
        prevlen = curlen;
        if (nextlen == 0) max_count = 138, min_count = 3;
        else if (curlen == nextlen) max_count = 6, min_count = 3;
        else max_count = 7, min_count = 4;
    }
}

static void cz_send_tree(struct cz_def *s, struct cz_tn *tree, int maxcode) {
    int prevlen = -1, curlen, nextlen = tree[0].len, count = 0;
    int max_count = 7, min_count = 4;
    if (nextlen == 0) max_count = 138, min_count = 3;
    for (int n = 0; n <= maxcode; n++) {
        curlen = nextlen;
        nextlen = tree[n + 1].len;
        if (++count < max_count && curlen == nextlen) continue;
        else if (count < min_count) {
            do cz_send(s, s->bltree[curlen].code, s->bltree[curlen].len); while (--count != 0);
        } else if (curlen != 0) {
            if (curlen != prevlen) {
                cz_send(s, s->bltree[curlen].code, s->bltree[curlen].len);
                count--;
            }
            cz_send(s, s->bltree[16].code, s->bltree[16].len);
            cz_send(s, (unsigned)count - 3, 2);
        } else if (count <= 10) {
            cz_send(s, s->bltree[17].code, s->bltree[17].len);
            cz_send(s, (unsigned)count - 3, 3);
        } else {
            cz_send(s, s->bltree[18].code, s->bltree[18].len);
            cz_send(s, (unsigned)count - 11, 7);
        }
        count = 0;
        prevlen = curlen;
        if (nextlen == 0) max_count = 138, min_count = 3;
        else if (curlen == nextlen) max_count = 6, min_count = 3;
        else max_count = 7, min_count = 4;
    }
}

static int cz_build_bl_tree(struct cz_def *s) {
    int maxb;
    cz_scan_tree(s, s->dltree, s->ld.maxcode);
    cz_scan_tree(s, s->ddtree, s->dd.maxcode);
    cz_build_tree(s, &s->bd);
    for (maxb = CZ_BLCODES - 1; maxb >= 3; maxb--)
        if (s->bltree[cz_blorder[maxb]].len != 0) break;
    s->opt_len += 3 * ((unsigned long)maxb + 1) + 5 + 5 + 4;
    return maxb;
}

static void cz_compress_block(struct cz_def *s, const struct cz_tn *lt, const struct cz_tn *dt) {
    unsigned dx = 0;
    for (unsigned lx = 0; lx < s->last_lit; lx++) {
        unsigned lc = s->lbuf[lx];
        if (!s->isdist[lx]) {
            cz_send(s, lt[lc].code, lt[lc].len);
        } else {
            int code = cz_lencode[lc];
            cz_send(s, lt[code + 257].code, lt[code + 257].len);
            int extra = cz_xlbits[code];
            if (extra) cz_send(s, lc - (unsigned)cz_baselen[code], extra);
            unsigned dist = s->dbuf[dx++];
            code = cz_dcode(dist);
            cz_send(s, dt[code].code, dt[code].len);
            extra = cz_xdbits[code];
            if (extra) cz_send(s, dist - (unsigned)cz_basedist[code], extra);
        }
    }
    cz_send(s, lt[CZ_ENDBLK].code, lt[CZ_ENDBLK].len);
}

/* the block so far, as stored, fixed or dynamic -- whichever is smallest */
static void cz_flush_block(struct cz_def *s, const uint8_t *buf, unsigned long stored_len, int eof) {
    cz_build_tree(s, &s->ld);
    cz_build_tree(s, &s->dd);
    int maxb = cz_build_bl_tree(s);
    unsigned long opt_lenb = (s->opt_len + 3 + 7) >> 3;
    unsigned long static_lenb = (s->static_len + 3 + 7) >> 3;
    if (static_lenb <= opt_lenb) opt_lenb = static_lenb;
    if (stored_len + 4 <= opt_lenb && buf) {
        cz_send(s, (unsigned)eof, 3);
        cz_windup(s);
        cz_obyte(s, stored_len & 0xff);
        cz_obyte(s, (stored_len >> 8) & 0xff);
        cz_obyte(s, ~stored_len & 0xff);
        cz_obyte(s, (~stored_len >> 8) & 0xff);
        for (unsigned long i = 0; i < stored_len; i++) cz_obyte(s, buf[i]);
    } else if (static_lenb == opt_lenb) {
        cz_send(s, (1u << 1) + (unsigned)eof, 3);
        cz_compress_block(s, cz_sltree, cz_sdtree);
    } else {
        cz_send(s, (2u << 1) + (unsigned)eof, 3);
        int lcodes = s->ld.maxcode + 1, dcodes = s->dd.maxcode + 1, blcodes = maxb + 1;
        cz_send(s, (unsigned)lcodes - 257, 5);
        cz_send(s, (unsigned)dcodes - 1, 5);
        cz_send(s, (unsigned)blcodes - 4, 4);
        for (int r = 0; r < blcodes; r++) cz_send(s, s->bltree[cz_blorder[r]].len, 3);
        cz_send_tree(s, s->dltree, lcodes - 1);
        cz_send_tree(s, s->ddtree, dcodes - 1);
        cz_compress_block(s, s->dltree, s->ddtree);
    }
    cz_init_block(s);
    if (eof) cz_windup(s);
}

static void cz_flush_cur(struct cz_def *s, int eof, int pad) {
    cz_flush_block(s, s->block_start >= 0 ? s->window + s->block_start : 0,
                   (unsigned long)((long)s->strstart - s->block_start), eof);
    if (pad && !eof && s->bivalid) {
        /* to a byte boundary, by an empty stored block -- only when the
         * stream is not on one already (measured) */
        cz_send(s, 0, 3);
        cz_windup(s);
        cz_obyte(s, 0); cz_obyte(s, 0); cz_obyte(s, 0xff); cz_obyte(s, 0xff);
    }
    s->block_start = (long)s->strstart;
}

/* one literal (dist 0) or match; 1 when the block should end here */
static int cz_tally(struct cz_def *s, unsigned dist, unsigned lc) {
    s->lbuf[s->last_lit] = (uint8_t)lc;
    s->isdist[s->last_lit] = dist != 0;
    s->last_lit++;
    if (dist == 0) {
        s->dltree[lc].freq++;
    } else {
        dist--;
        s->dltree[cz_lencode[lc] + 257].freq++;
        s->ddtree[cz_dcode(dist)].freq++;
        s->dbuf[s->last_dist++] = (uint16_t)dist;
    }
    if (s->level > 2 && (s->last_lit & 0xfff) == 0) {
        unsigned long out_length = (unsigned long)s->last_lit * 8;
        unsigned long in_length = (unsigned long)((long)s->strstart - s->block_start);
        for (int dc = 0; dc < CZ_DCODES; dc++)
            out_length += (unsigned long)s->ddtree[dc].freq * (5 + (unsigned long)cz_xdbits[dc]);
        out_length >>= 3;
        if (s->last_dist < s->last_lit / 2 && out_length < in_length / 2) return 1;
    }
    return s->last_lit == CZ_LITBUF - 1 || s->last_dist == CZ_LITBUF;
}

/* ---- the window ---- */
static unsigned cz_read_full(struct cz_def *s, uint8_t *buf, unsigned n) {
    unsigned got = 0;
    while (got < n) {
        int r = s->rd(s->rctx, buf + got, n - got);
        if (r < 0) { s->rderr = 1; break; }
        if (r == 0) break;
        got += (unsigned)r;
    }
    return got;
}

static void cz_fill_window(struct cz_def *s) {
    unsigned more = 2 * CZ_WSIZE - s->lookahead - s->strstart;
    if (s->strstart >= CZ_WSIZE + CZ_MAXDIST) {
        memcpy(s->window, s->window + CZ_WSIZE, CZ_WSIZE);
        s->match_start -= CZ_WSIZE;
        s->strstart -= CZ_WSIZE;
        s->block_start -= (long)CZ_WSIZE;
        for (unsigned n = 0; n < CZ_HSIZE; n++) {
            unsigned m = s->head[n];
            s->head[n] = (uint16_t)(m >= CZ_WSIZE ? m - CZ_WSIZE : 0);
        }
        for (unsigned n = 0; n < CZ_WSIZE; n++) {
            unsigned m = s->prev[n];
            s->prev[n] = (uint16_t)(m >= CZ_WSIZE ? m - CZ_WSIZE : 0);
        }
        more += CZ_WSIZE;
        if (s->rsync_end != CZ_RSNONE) s->rsync_end -= CZ_WSIZE;
    }
    if (!s->eofile) {
        unsigned n = cz_read_full(s, s->window + s->strstart + s->lookahead, more);
        if (n == 0) {
            s->eofile = 1;
            memset(s->window + s->strstart + s->lookahead, 0, CZ_MINM - 1);
        } else s->lookahead += n;
    }
}

#define CZ_UPDHASH(h, c) (h = (((h) << CZ_HSHIFT) ^ (c)) & CZ_HMASK)
#define CZ_INSERT(s, str, mh) do { \
    CZ_UPDHASH((s)->ins_h, (s)->window[(str) + CZ_MINM - 1]); \
    (s)->prev[(str) & CZ_WMASK] = (uint16_t)((mh) = (s)->head[(s)->ins_h]); \
    (s)->head[(s)->ins_h] = (uint16_t)(str); \
} while (0)

static unsigned cz_longest_match(struct cz_def *s, unsigned cur_match) {
    unsigned chain = s->chain;
    const uint8_t *scan = s->window + s->strstart, *match;
    int len, best_len = (int)s->prev_length;
    unsigned limit = s->strstart > CZ_MAXDIST ? s->strstart - CZ_MAXDIST : 0;
    const uint8_t *strend = s->window + s->strstart + CZ_MAXM;
    uint8_t scan_end1 = scan[best_len - 1], scan_end = scan[best_len];
    if (s->prev_length >= s->good) chain >>= 2;
    do {
        match = s->window + cur_match;
        if (match[best_len] != scan_end || match[best_len - 1] != scan_end1 ||
            *match != *scan || match[1] != scan[1])
            continue;
        const uint8_t *p = scan + 2, *q = match + 2;
        while (p < strend && *p == *q) p++, q++;
        len = (int)(p - scan);
        if (len > best_len) {
            s->match_start = cur_match;
            best_len = len;
            if ((unsigned)len >= s->nice) break;
            scan_end1 = scan[best_len - 1];
            scan_end = scan[best_len];
        }
    } while ((cur_match = s->prev[cur_match & CZ_WMASK]) > limit && --chain != 0);
    return (unsigned)best_len;
}

/* --rsyncable: a rolling sum of the last 4096 bytes; where it is a
 * multiple of 4096 the block ends, padded with an empty stored block, so
 * the compressed stream starts afresh at a point decided by the data
 * alone. Measured: the end is the first such byte after the last one
 * used, and the block is cut once the input position passes it. */
static void cz_rsync_roll(struct cz_def *s, unsigned start, unsigned num) {
    unsigned i = start, end = start + num;
    for (; i < end && i < CZ_RSWIN; i++) s->rsync_sum += s->window[i];
    for (; i < end; i++) {
        s->rsync_sum += s->window[i];
        s->rsync_sum -= s->window[i - CZ_RSWIN];
        if (s->rsync_end == CZ_RSNONE && s->rsync_sum % CZ_RSWIN == 0) s->rsync_end = i;
    }
}
#define CZ_ROLL(s, st, n) do { if ((s)->rsync) cz_rsync_roll((s), (st), (n)); } while (0)

static int cz_rsync_due(struct cz_def *s) {
    if (s->rsync && s->strstart > s->rsync_end) {
        s->rsync_end = CZ_RSNONE;
        return 1;
    }
    return 0;
}

static void cz_deflate_fast(struct cz_def *s) {
    unsigned hash_head = 0, match_length = 0;
    s->prev_length = CZ_MINM - 1;
    while (s->lookahead != 0) {
        CZ_INSERT(s, s->strstart, hash_head);
        if (hash_head != 0 && s->strstart - hash_head <= CZ_MAXDIST &&
            s->strstart <= 2 * CZ_WSIZE - CZ_MINLA) {
            match_length = cz_longest_match(s, hash_head);
            if (match_length > s->lookahead) match_length = s->lookahead;
        }
        int flush;
        if (match_length >= CZ_MINM) {
            flush = cz_tally(s, s->strstart - s->match_start, match_length - CZ_MINM);
            s->lookahead -= match_length;
            CZ_ROLL(s, s->strstart, match_length);
            if (match_length <= s->lazy) {
                match_length--;
                do {
                    s->strstart++;
                    CZ_INSERT(s, s->strstart, hash_head);
                } while (--match_length != 0);
                s->strstart++;
            } else {
                s->strstart += match_length;
                match_length = 0;
                s->ins_h = s->window[s->strstart];
                CZ_UPDHASH(s->ins_h, s->window[s->strstart + 1]);
            }
        } else {
            flush = cz_tally(s, 0, s->window[s->strstart]);
            CZ_ROLL(s, s->strstart, 1);
            s->lookahead--;
            s->strstart++;
        }
        if (cz_rsync_due(s)) flush = 2;
        if (flush) cz_flush_cur(s, 0, flush == 2);
        while (s->lookahead < CZ_MINLA && !s->eofile) cz_fill_window(s);
    }
    cz_flush_cur(s, 1, 0);
}

static void cz_deflate_lazy(struct cz_def *s) {
    unsigned hash_head = 0, prev_match, match_length = CZ_MINM - 1;
    int match_available = 0, flush;
    while (s->lookahead != 0) {
        CZ_INSERT(s, s->strstart, hash_head);
        s->prev_length = match_length;
        prev_match = s->match_start;
        match_length = CZ_MINM - 1;
        if (hash_head != 0 && s->prev_length < s->lazy &&
            s->strstart - hash_head <= CZ_MAXDIST &&
            s->strstart <= 2 * CZ_WSIZE - CZ_MINLA) {
            match_length = cz_longest_match(s, hash_head);
            if (match_length > s->lookahead) match_length = s->lookahead;
            if (match_length == CZ_MINM && s->strstart - s->match_start > CZ_TOOFAR)
                match_length--;
        }
        if (s->prev_length >= CZ_MINM && match_length <= s->prev_length) {
            flush = cz_tally(s, s->strstart - 1 - prev_match, s->prev_length - CZ_MINM);
            s->lookahead -= s->prev_length - 1;
            s->prev_length -= 2;
            CZ_ROLL(s, s->strstart, s->prev_length + 1);
            do {
                s->strstart++;
                CZ_INSERT(s, s->strstart, hash_head);
            } while (--s->prev_length != 0);
            match_available = 0;
            match_length = CZ_MINM - 1;
            s->strstart++;
            if (cz_rsync_due(s)) flush = 2;
            if (flush) cz_flush_cur(s, 0, flush == 2);
        } else if (match_available) {
            flush = cz_tally(s, 0, s->window[s->strstart - 1]);
            if (cz_rsync_due(s)) flush = 2;
            if (flush) cz_flush_cur(s, 0, flush == 2);
            CZ_ROLL(s, s->strstart, 1);
            s->strstart++;
            s->lookahead--;
        } else {
            if (cz_rsync_due(s)) cz_flush_cur(s, 0, 1);
            match_available = 1;
            CZ_ROLL(s, s->strstart, 1);
            s->strstart++;
            s->lookahead--;
        }
        while (s->lookahead < CZ_MINLA && !s->eofile) cz_fill_window(s);
    }
    if (match_available) cz_tally(s, 0, s->window[s->strstart - 1]);
    cz_flush_cur(s, 1, 0);
}

static const struct { uint16_t good, lazy, nice, chain; } cz_cfg[10] = {
    {0, 0, 0, 0}, {4, 4, 8, 4}, {4, 5, 16, 8}, {4, 6, 32, 32},
    {4, 4, 16, 16}, {8, 16, 32, 32}, {8, 16, 128, 128}, {8, 32, 128, 256},
    {32, 128, 258, 1024}, {32, 258, 258, 4096},
};

/* compress all of rd's bytes at LEVEL (1..9) as raw deflate to wr */
static void cz_deflate(struct cz_def *s, int level) {
    cz_trees_init();
    s->ld.tree = s->dltree; s->ld.stree = cz_sltree; s->ld.extra = cz_xlbits;
    s->ld.base = 257; s->ld.elems = CZ_LCODES; s->ld.maxlen = 15; s->ld.maxcode = 0;
    s->dd.tree = s->ddtree; s->dd.stree = cz_sdtree; s->dd.extra = cz_xdbits;
    s->dd.base = 0; s->dd.elems = CZ_DCODES; s->dd.maxlen = 15; s->dd.maxcode = 0;
    s->bd.tree = s->bltree; s->bd.stree = 0; s->bd.extra = cz_xblbits;
    s->bd.base = 0; s->bd.elems = CZ_BLCODES; s->bd.maxlen = 7; s->bd.maxcode = 0;
    cz_init_block(s);
    s->bibuf = 0; s->bivalid = 0; s->on = 0;
    memset(s->head, 0, sizeof s->head);
    memset(s->prev, 0, sizeof s->prev);
    s->level = level;
    s->good = cz_cfg[level].good;
    s->lazy = cz_cfg[level].lazy;
    s->nice = cz_cfg[level].nice;
    s->chain = cz_cfg[level].chain;
    s->strstart = 0;
    s->block_start = 0;
    s->match_start = 0;
    s->eofile = 0;
    s->rsync_sum = 0;
    s->rsync_end = 0xFFFFFFFFu;
    s->lookahead = cz_read_full(s, s->window, 2 * CZ_WSIZE);
    if (s->lookahead == 0) {
        s->eofile = 1;
        s->lookahead = 0;
    }
    while (s->lookahead < CZ_MINLA && !s->eofile) cz_fill_window(s);
    s->ins_h = 0;
    for (unsigned j = 0; j < CZ_MINM - 1; j++) CZ_UPDHASH(s->ins_h, s->window[j]);
    if (level <= 3) cz_deflate_fast(s);
    else cz_deflate_lazy(s);
    cz_oflush(s);
}

/* ======================================================================
 *  deflate, decompressing (RFC 1951)
 * ====================================================================== */
#define CZ_FASTB 9
struct cz_huff {
    uint16_t count[16];
    uint16_t sym[320];
    uint16_t fast[1 << CZ_FASTB];  /* sym << 4 | len, or 0: the slow path */
};

#define CZ_IOK 0
#define CZ_IEOF 1          /* the input ended inside the stream */
#define CZ_IBAD 2          /* not a valid deflate stream */

struct cz_inf {
    struct cz_in *in;
    uint32_t bitbuf;
    int bitcnt;
    int err;
    uint8_t win[CZ_WSIZE];
    unsigned wp;
    void (*out)(void *ctx, const uint8_t *p, size_t n);
    void *octx;
    uint32_t crc;
    uint64_t outlen;
    struct cz_huff lt, dt;
};

static void cz_iflush(struct cz_inf *z) {
    if (z->wp) {
        z->crc = cz_crc32(z->crc, z->win, z->wp);
        z->outlen += z->wp;
        z->out(z->octx, z->win, z->wp);
    }
    z->wp = 0;
}

static int cz_ineed(struct cz_inf *z, int n) {
    while (z->bitcnt < n) {
        int c = cz_getc(z->in);
        if (c < 0) { z->err = CZ_IEOF; return 0; }
        z->bitbuf |= (uint32_t)c << z->bitcnt;
        z->bitcnt += 8;
    }
    return 1;
}

static unsigned cz_ibits(struct cz_inf *z, int n) {
    if (n == 0) return 0;
    if (!cz_ineed(z, n)) return 0;
    unsigned v = z->bitbuf & ((1u << n) - 1);
    z->bitbuf >>= n;
    z->bitcnt -= n;
    return v;
}

/* 0 for a complete code, 1 for an incomplete one, -1 when the lengths
 * oversubscribe */
static int cz_hbuild(struct cz_huff *h, const uint8_t *len, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    int left = 1;
    for (int b = 1; b < 16; b++) {
        left <<= 1;
        left -= h->count[b];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int b = 1; b < 15; b++) offs[b + 1] = (uint16_t)(offs[b] + h->count[b]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
    /* the fast table: every code of at most CZ_FASTB bits, bit-reversed */
    memset(h->fast, 0, sizeof h->fast);
    unsigned code = 0;
    int idx = 0;
    for (int b = 1; b <= CZ_FASTB; b++) {
        for (int k = 0; k < h->count[b]; k++, idx++, code++) {
            unsigned r = cz_bitrev(code, b);
            for (unsigned f = r; f < (1u << CZ_FASTB); f += 1u << b)
                h->fast[f] = (uint16_t)(h->sym[idx] << 4 | (unsigned)b);
        }
        code <<= 1;
    }
    return left > 0 ? 1 : 0;
}

static int cz_hdecode(struct cz_inf *z, const struct cz_huff *h) {
    /* fast: when enough bits are at hand (or can be had) */
    while (z->bitcnt < CZ_FASTB) {
        int c = cz_peekc(z->in);
        if (c < 0) break;
        cz_getc(z->in);
        z->bitbuf |= (uint32_t)c << z->bitcnt;
        z->bitcnt += 8;
    }
    if (z->bitcnt >= CZ_FASTB) {
        unsigned e = h->fast[z->bitbuf & ((1u << CZ_FASTB) - 1)];
        if (e) {
            z->bitbuf >>= e & 15;
            z->bitcnt -= (int)(e & 15);
            return (int)(e >> 4);
        }
    }
    /* slow: a bit at a time, canonical order */
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        if (!cz_ineed(z, 1)) return -1;
        code |= (int)(z->bitbuf & 1);
        z->bitbuf >>= 1;
        z->bitcnt--;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->err = CZ_IBAD;
    return -1;
}

static const uint16_t cz_lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint16_t cz_dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};

static int cz_icodes(struct cz_inf *z) {
    for (;;) {
        int sym = cz_hdecode(z, &z->lt);
        if (sym < 0) return z->err ? z->err : CZ_IBAD;
        if (sym < 256) {
            z->win[z->wp++] = (uint8_t)sym;
            if (z->wp == CZ_WSIZE) cz_iflush(z);
        } else if (sym == 256) {
            return CZ_IOK;
        } else {
            sym -= 257;
            if (sym >= 29) return CZ_IBAD;
            unsigned len = cz_lbase[sym] + cz_ibits(z, cz_xlbits[sym]);
            if (z->err) return z->err;
            int ds = cz_hdecode(z, &z->dt);
            if (ds < 0) return z->err ? z->err : CZ_IBAD;
            if (ds >= 30) return CZ_IBAD;
            unsigned dist = cz_dbase[ds] + cz_ibits(z, cz_xdbits[ds]);
            if (z->err) return z->err;
            if (dist > z->outlen + z->wp) return CZ_IBAD;
            unsigned from = (z->wp - dist) & CZ_WMASK;
            while (len--) {
                z->win[z->wp++] = z->win[from];
                from = (from + 1) & CZ_WMASK;
                if (z->wp == CZ_WSIZE) cz_iflush(z);
            }
        }
    }
}

static int cz_ifixed(struct cz_inf *z) {
    uint8_t len[320];
    int n = 0;
    while (n < 144) len[n++] = 8;
    while (n < 256) len[n++] = 9;
    while (n < 280) len[n++] = 7;
    while (n < 288) len[n++] = 8;
    cz_hbuild(&z->lt, len, 288);
    for (n = 0; n < 30; n++) len[n] = 5;
    cz_hbuild(&z->dt, len, 30);
    return cz_icodes(z);
}

static int cz_idynamic(struct cz_inf *z) {
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t len[320];
    int nlen = (int)cz_ibits(z, 5) + 257;
    int ndist = (int)cz_ibits(z, 5) + 1;
    int ncode = (int)cz_ibits(z, 4) + 4;
    if (z->err) return z->err;
    if (nlen > 286 || ndist > 30) return CZ_IBAD;
    memset(len, 0, sizeof len);
    for (int i = 0; i < ncode; i++) len[order[i]] = (uint8_t)cz_ibits(z, 3);
    if (z->err) return z->err;
    if (cz_hbuild(&z->lt, len, 19) != 0) return CZ_IBAD;
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = cz_hdecode(z, &z->lt);
        if (sym < 0) return z->err ? z->err : CZ_IBAD;
        if (sym < 16) {
            len[idx++] = (uint8_t)sym;
        } else {
            int rep, v = 0;
            if (sym == 16) {
                if (idx == 0) return CZ_IBAD;
                v = len[idx - 1];
                rep = 3 + (int)cz_ibits(z, 2);
            } else if (sym == 17) rep = 3 + (int)cz_ibits(z, 3);
            else rep = 11 + (int)cz_ibits(z, 7);
            if (z->err) return z->err;
            if (idx + rep > nlen + ndist) return CZ_IBAD;
            while (rep--) len[idx++] = (uint8_t)v;
        }
    }
    if (len[256] == 0) return CZ_IBAD;
    int r = cz_hbuild(&z->lt, len, nlen);
    if (r < 0 || (r > 0 && nlen - z->lt.count[0] != 1)) return CZ_IBAD;
    r = cz_hbuild(&z->dt, len + nlen, ndist);
    if (r < 0 || (r > 0 && ndist - z->dt.count[0] != 1)) return CZ_IBAD;
    return cz_icodes(z);
}

/* whole bytes read ahead into the bit buffer go back to the input */
static void cz_igiveback(struct cz_inf *z) {
    int r = z->bitcnt & 7, k = z->bitcnt >> 3;
    for (int i = k - 1; i >= 0; i--) cz_ungetc(z->in, (int)((z->bitbuf >> (r + 8 * i)) & 0xff));
    z->bitbuf &= (1u << r) - 1;
    z->bitcnt = r;
}

static int cz_istored(struct cz_inf *z) {
    cz_igiveback(z);
    z->bitbuf = 0;
    z->bitcnt = 0;
    uint8_t h[4];
    for (int i = 0; i < 4; i++) {
        int c = cz_getc(z->in);
        if (c < 0) return CZ_IEOF;
        h[i] = (uint8_t)c;
    }
    unsigned len = h[0] | (unsigned)h[1] << 8;
    if (h[2] != (uint8_t)~h[0] || h[3] != (uint8_t)~h[1]) return CZ_IBAD;
    while (len) {
        unsigned k = CZ_WSIZE - z->wp;
        if (k > len) k = len;
        size_t got = cz_read(z->in, z->win + z->wp, k);
        z->wp += (unsigned)got;
        len -= (unsigned)got;
        if (z->wp == CZ_WSIZE) cz_iflush(z);
        if (got < k) return CZ_IEOF;
    }
    return CZ_IOK;
}

/* inflate one deflate stream from z->in; CZ_IOK, CZ_IEOF or CZ_IBAD.
 * What it decodes goes to z->out a window at a time, as it is made. */
static int cz_inflate(struct cz_inf *z) {
    int last, r;
    z->bitbuf = 0;
    z->bitcnt = 0;
    z->err = 0;
    z->wp = 0;
    z->crc = 0;
    z->outlen = 0;
    do {
        last = (int)cz_ibits(z, 1);
        int type = (int)cz_ibits(z, 2);
        if (z->err) { r = z->err; break; }
        if (type == 0) r = cz_istored(z);
        else if (type == 1) r = cz_ifixed(z);
        else if (type == 2) r = cz_idynamic(z);
        else r = CZ_IBAD;
    } while (r == CZ_IOK && !last);
    /* a broken stream's last partial window is not written (gzip writes
     * only full windows before it says "format violated") */
    if (r == CZ_IOK) cz_iflush(z);
    cz_igiveback(z);
    return r;
}

/* ======================================================================
 *  the gzip program
 * ====================================================================== */
#define CZ_OK 0
#define CZ_ERR 1
#define CZ_WARN 2

#define CZ_M_STORED 0
#define CZ_M_LZW 1
#define CZ_M_PACKED 2
#define CZ_M_LZH 3
#define CZ_M_DEFLATED 8

#define CZ_MAX_SUFFIX 30
#define CZ_STAMP_UNSET (-0x7fffffffffffffffLL)

static struct {
    /* options */
    int to_stdout, decompress, force, keep, list, quiet, verbose, recursive, test;
    int no_name, no_time, level, rsync, synchronous, ascii;
    const char *z_suffix;
    size_t z_len;
    int exit_code;
    int file_count;
    /* the file at hand */
    char *ifname, *ofname;
    size_t ifcap, ofcap;
    int ifd, ofd;
    struct stat istat;
    long long ifile_size;
    struct cz_in in;
    int part_nb, method, last_member, pkzip, ext_header;
    long long header_bytes, bytes_in, bytes_out, total_in, total_out;
    long long stamp;           /* the time to give the output / to list */
    long stamp_ns;
    uint32_t crc, zip_crc;
    uint64_t zip_usize;
    int save_orig_name;
    int list_first;
    int remove_ofname;
    int out_discard;           /* -t, -l: the output is only counted */
    int stdin_done;
    uint8_t *obuf;
    size_t on;
} cz_g;

static void cz_gz_flush_out(void);

static const char *cz_gz_prog = "gzip";

/* the exit everything fatal takes: the half-made output goes, status 1 */
static void cz_gz_abort(void) __attribute__((noreturn));
static void cz_gz_abort(void) {
    /* what was made before the failure is written, as gzip has */
    if (cz_g.on && !cz_g.out_discard && cz_g.ofd >= 0) cz_write_all(cz_g.ofd, cz_g.obuf, cz_g.on);
    cz_g.on = 0;
    fflush(stdout);
    if (cz_g.remove_ofname && cz_g.ofd >= 0) {
        close(cz_g.ofd);
        unlink(cz_g.ofname);
    }
    exit(CZ_ERR);
}

static void cz_gz_warnset(void) {
    if (cz_g.exit_code == CZ_OK) cz_g.exit_code = CZ_WARN;
}

/* "\ngzip: NAME: MESSAGE" and the run ends: what gzip says of a stream it
 * cannot finish (the leading newline ends a -v "NAME:\t" line) */
static void cz_gz_fatal(const char *msg) __attribute__((noreturn));
static void cz_gz_fatal(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "\n%s: %s: %s\n", cz_gz_prog, cz_g.ifname, msg);
    cz_gz_abort();
}

static void cz_gz_read_error(int e) __attribute__((noreturn));
static void cz_gz_read_error(int e) {
    fflush(stdout);
    if (e) fprintf(stderr, "\n%s: %s: %s\n", cz_gz_prog, cz_g.ifname, strerror(e));
    else fprintf(stderr, "\n%s: %s: unexpected end of file\n", cz_gz_prog, cz_g.ifname);
    cz_gz_abort();
}

static void cz_gz_write_error(int e) __attribute__((noreturn));
static void cz_gz_write_error(int e) {
    fflush(stdout);
    fprintf(stderr, "\n%s: %s: %s\n", cz_gz_prog, cz_g.ofname, strerror(e));
    cz_gz_abort();
}

/* "gzip: NAME: strerror", status 1 -- an operand that cannot be had */
static void cz_gz_progerror(const char *name, int e) {
    fflush(stdout);
    fprintf(stderr, "%s: %s: %s\n", cz_gz_prog, name, strerror(e));
    cz_g.exit_code = CZ_ERR;
}

static void cz_gz_setname(char **buf, size_t *cap, const char *s, size_t extra) {
    size_t n = strlen(s) + extra + 1;
    if (n > *cap) {
        *cap = n + 64;
        *buf = (char *)realloc(*buf, *cap);
        if (!*buf) { fprintf(stderr, "%s: memory exhausted\n", cz_gz_prog); exit(CZ_ERR); }
    }
    strcpy(*buf, s);
}

/* ---- output ---- */
#define CZ_OBUFSZ 65536
static void cz_gz_flush_out(void) {
    if (cz_g.on && !cz_g.out_discard) {
        int e = cz_write_all(cz_g.ofd, cz_g.obuf, cz_g.on);
        if (e) cz_gz_write_error(e);
    }
    cz_g.on = 0;
}

static void cz_gz_put(const uint8_t *p, size_t n) {
    cz_g.bytes_out += (long long)n;
    if (cz_g.out_discard) return;
    while (n) {
        size_t k = CZ_OBUFSZ - cz_g.on;
        if (k > n) k = n;
        memcpy(cz_g.obuf + cz_g.on, p, k);
        cz_g.on += k;
        p += k;
        n -= k;
        if (cz_g.on == CZ_OBUFSZ) cz_gz_flush_out();
    }
}

static void cz_gz_putbyte(unsigned b) {
    uint8_t c = (uint8_t)b;
    cz_gz_put(&c, 1);
}

static void cz_gz_inflate_out(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    cz_gz_put(p, n);
}

static void cz_gz_deflate_out(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    cz_gz_put(p, n);
}

/* ---- input ---- */
static int cz_gz_get_byte(void) {
    int c = cz_getc(&cz_g.in);
    if (c < 0) cz_gz_read_error(cz_g.in.err);
    return c;
}

static int cz_gz_try_byte(void) {
    int c = cz_getc(&cz_g.in);
    if (c < 0 && cz_g.in.err) cz_gz_read_error(cz_g.in.err);
    return c;
}

static unsigned cz_gz_get16(void) {
    unsigned a = (unsigned)cz_gz_get_byte();
    return a | (unsigned)cz_gz_get_byte() << 8;
}

static uint32_t cz_gz_get32(void) {
    uint32_t a = cz_gz_get16();
    return a | (uint32_t)cz_gz_get16() << 16;
}

/* ---- ratios: printf("%5.1f%%", 100.0 * NUM / DEN), in integers ----
 *
 * The tobyOS build has no floating point (-mno-sse), and the rounding is
 * the binary double's: -0.05% prints as -0.1 (0.05 is a little above in
 * binary), -0.25% as -0.2 (exact, half to even). So: the nearest tenth of
 * the exact quotient, and on an exact half the side the double falls on. */
static int cz_q_double_side(uint64_t a, uint64_t den) {
    /* where RN(a/den) lies against a/den exactly: 1 above, -1 below, 0 equal */
    if (a == 0) return 0;
    /* scale so that 2^52 <= a*2^e/den < 2^53, with e possibly negative */
    int e = 0;
    uint64_t hi = 0, lo = a;    /* the numerator, 128-bit: hi:lo */
    /* the integer part's bit length decides e */
    uint64_t q = a / den;
    int qbits = 0;
    while (qbits < 64 && (q >> qbits)) qbits++;
    e = 53 - qbits;            /* q*2^e is about 2^53; adjust below */
    if (qbits == 0) {
        /* a < den: count how far below 1 */
        uint64_t t = a;
        int sh = 0;
        while (t < den && sh < 1100) {
            if (t >> 63) break;
            t <<= 1;
            sh++;
        }
        e = 52 + sh;
    }
    for (int tries = 0; tries < 3; tries++) {
        /* numerator a*2^e (e >= 0 here: our quotients are small) */
        if (e < 0) return 0;
        hi = 0; lo = a;
        for (int i = 0; i < e; i++) {
            hi = hi << 1 | lo >> 63;
            lo <<= 1;
        }
        /* m = num / den, rem = num % den: long division */
        uint64_t m = 0, rem = 0;
        int overflow = 0;
        for (int i = 127; i >= 0; i--) {
            uint64_t bit = i >= 64 ? (hi >> (i - 64)) & 1 : (lo >> i) & 1;
            int carry = (int)(rem >> 63);
            rem = rem << 1 | bit;
            if (m >> 63) overflow = 1;
            m <<= 1;
            if (carry || rem >= den) { rem -= den; m |= 1; }
        }
        if (overflow || m >= (1ULL << 53)) { e--; continue; }
        if (m < (1ULL << 52)) { e++; continue; }
        if (rem == 0) return 0;
        /* round to nearest, ties to even, on the mantissa */
        if (rem > den - rem) return 1;           /* 2*rem > den */
        if (rem < den - rem) return -1;
        return (m & 1) ? 1 : -1;
    }
    return 0;
}

static void cz_ratio(FILE *f, long long num, long long den) {
    if (den == 0) {
        fputs("  0.0%", f);
        return;
    }
    int neg = (num < 0) != (den < 0);
    uint64_t an = num < 0 ? (uint64_t)-num : (uint64_t)num;
    uint64_t ad = den < 0 ? (uint64_t)-den : (uint64_t)den;
    /* tenths of a percent: 1000*an/ad, rounded */
    uint64_t t = an * 1000;
    uint64_t k = t / ad, r = t % ad;
    if (r > ad - r) k++;
    else if (r == ad - r) {
        /* a half: where the double 100*an/ad falls */
        int side = cz_q_double_side(an * 100, ad);
        if (side > 0) k++;
        else if (side == 0 && (k & 1)) k++;
    }
    char b[48];
    snprintf(b, sizeof b, "%s%llu.%llu", neg ? "-" : "", (unsigned long long)(k / 10),
             (unsigned long long)(k % 10));
    fprintf(f, "%5s%%", b);
}

/* ---- suffixes ---- */
static void cz_lower(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

/* where NAME's compression suffix starts, or 0: the -S suffix and gzip's
 * own, compared in lower case; the -S one goes last when it ends one of
 * the others (so -S z still reads .gz as .gz) */
static char *cz_get_suffix(char *name) {
    static const char *const builtin[] = {".gz", ".z", ".taz", ".tgz", "-gz", "-z", "_z"};
    const char *list[9];
    int n = 0, zlast = 0;
    char zl[CZ_MAX_SUFFIX + 1];
    snprintf(zl, sizeof zl, "%s", cz_g.z_suffix);
    cz_lower(zl);
    for (int i = 0; i < 7; i++) {
        size_t sl = strlen(builtin[i]);
        if (cz_g.z_len < sl && strcmp(cz_g.z_suffix, builtin[i] + sl - cz_g.z_len) == 0) zlast = 1;
    }
    if (!zlast) list[n++] = zl;
    for (int i = 0; i < 7; i++) list[n++] = builtin[i];
    if (zlast) list[n++] = zl;
    size_t nlen = strlen(name);
    char tail[CZ_MAX_SUFFIX + 3];
    const char *src = nlen <= CZ_MAX_SUFFIX + 2 ? name : name + nlen - CZ_MAX_SUFFIX - 2;
    snprintf(tail, sizeof tail, "%s", src);
    cz_lower(tail);
    size_t slen = strlen(tail);
    for (int i = 0; i < n; i++) {
        size_t s = strlen(list[i]);
        if (slen > s && tail[slen - s - 1] != '/' && strcmp(tail + slen - s, list[i]) == 0)
            return name + nlen - s;
    }
    return 0;
}

static const char *cz_base(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* ---- the gzip header ---- */
static unsigned cz_hdrn;
static uint32_t cz_hdrcrc;       /* CRC-32 of the header so far, for FHCRC */

static int cz_hb(void) {
    int c = cz_gz_get_byte();
    uint8_t b = (uint8_t)c;
    cz_hdrcrc = cz_crc32(cz_hdrcrc, &b, 1);
    cz_hdrn++;
    return c;
}

/* what kind of member comes next; the method, or -1 (an error, said),
 * -2 (trailing garbage) or -3 (trailing zeros) */
static int cz_get_method(void) {
    int m0, m1;
    uint64_t start = cz_g.in.total;
    if (cz_g.force && cz_g.to_stdout) {
        m0 = cz_gz_try_byte();
        m1 = cz_gz_try_byte();
    } else {
        m0 = cz_gz_get_byte();
        if (m0) m1 = cz_gz_get_byte();
        else m1 = cz_gz_try_byte();
    }
    cz_g.method = -1;
    cz_g.part_nb++;
    cz_g.header_bytes = 0;
    cz_g.last_member = 0;
    if (m0 == 0x1f && (m1 == 0x8b || m1 == 0x9e)) {
        cz_hdrcrc = 0;
        cz_hdrn = 0;
        uint8_t mg[2] = {(uint8_t)m0, (uint8_t)m1};
        cz_hdrcrc = cz_crc32(cz_hdrcrc, mg, 2);
        int method = cz_hb();
        if (method != CZ_M_DEFLATED) {
            fflush(stdout);
            fprintf(stderr, "%s: %s: unknown method %d -- not supported\n", cz_gz_prog, cz_g.ifname, method);
            cz_g.exit_code = CZ_ERR;
            return -1;
        }
        cz_g.method = method;
        int flags = cz_hb();
        if (flags & 0x20) {
            fflush(stdout);
            fprintf(stderr, "%s: %s is encrypted -- not supported\n", cz_gz_prog, cz_g.ifname);
            cz_g.exit_code = CZ_ERR;
            return -1;
        }
        if (flags & 0xc0) {
            fflush(stdout);
            fprintf(stderr, "%s: %s has flags 0x%x -- not supported\n", cz_gz_prog, cz_g.ifname, flags);
            cz_g.exit_code = CZ_ERR;
            if (cz_g.force <= 1) return -1;
        }
        uint32_t stamp = (uint32_t)cz_hb();
        stamp |= (uint32_t)cz_hb() << 8;
        stamp |= (uint32_t)cz_hb() << 16;
        stamp |= (uint32_t)cz_hb() << 24;
        if (stamp != 0 && !cz_g.no_time) {
            cz_g.stamp = (long long)stamp;
            cz_g.stamp_ns = 0;
        }
        cz_hb();   /* extra flags */
        cz_hb();   /* the OS */
        if (flags & 0x04) {
            unsigned len = (unsigned)cz_hb();
            len |= (unsigned)cz_hb() << 8;
            if (cz_g.verbose) {
                fflush(stdout);
                fprintf(stderr, "%s: %s: extra field of %u bytes ignored\n", cz_gz_prog, cz_g.ifname, len);
            }
            while (len--) cz_hb();
        }
        if (flags & 0x08) {
            if (cz_g.no_name || (cz_g.to_stdout && !cz_g.list) || cz_g.part_nb > 1) {
                while (cz_hb() != 0) continue;
            } else {
                /* the name goes where the input's directory is */
                size_t dirlen = (size_t)(cz_base(cz_g.ofname) - cz_g.ofname);
                size_t cap = 1024, n = 0;
                char *nm = (char *)malloc(cap);
                int c;
                while ((c = cz_hb()) != 0) {
                    if (n + 1 >= cap) { cap *= 2; nm = (char *)realloc(nm, cap); }
                    nm[n++] = (char)c;
                }
                nm[n] = 0;
                const char *b = cz_base(nm);
                char *full = (char *)malloc(dirlen + strlen(b) + 1);
                memcpy(full, cz_g.ofname, dirlen);
                strcpy(full + dirlen, b);
                if (!cz_g.list) {
                    /* a name longer than the file system takes is cut */
                    size_t bl = strlen(b);
                    if (bl > 255) {
                        full[dirlen + 255] = 0;
                        fflush(stdout);
                        fprintf(stderr, "%s: %s: warning, name truncated\n", cz_gz_prog, full);
                        cz_gz_warnset();
                    }
                }
                cz_gz_setname(&cz_g.ofname, &cz_g.ofcap, full, 0);
                free(full);
                free(nm);
            }
        }
        if (flags & 0x10) {
            while (cz_hb() != 0) continue;
        }
        if (flags & 0x02) {
            uint32_t want = cz_hdrcrc & 0xffff;
            unsigned got = (unsigned)cz_gz_get_byte();
            got |= (unsigned)cz_gz_get_byte() << 8;
            if (got != want) {
                fflush(stdout);
                fprintf(stderr, "%s: %s: header checksum 0x%04x != computed checksum 0x%04x\n",
                        cz_gz_prog, cz_g.ifname, got, (unsigned)want);
                cz_g.exit_code = CZ_ERR;
                if (cz_g.force <= 1) return -1;
            }
        }
        if (cz_g.part_nb == 1) cz_g.header_bytes = (long long)(cz_g.in.total - start) + 8;
    } else if (m0 == 'P' && m1 == 'K' && start == 0 && cz_peekc(&cz_g.in) == 3) {
        /* a zip file: its first member, deflated or stored */
        uint8_t h[30];
        h[0] = 'P';
        h[1] = 'K';
        for (int i = 2; i < 30; i++) h[i] = (uint8_t)cz_gz_get_byte();
        if (h[3] != 4) {
            fflush(stdout);
            fprintf(stderr, "\n%s: %s: not in gzip format\n", cz_gz_prog, cz_g.ifname);
            cz_g.exit_code = CZ_ERR;
            return -1;
        }
        unsigned zflags = h[6] | (unsigned)h[7] << 8;
        unsigned zmethod = h[8] | (unsigned)h[9] << 8;
        if (zmethod != 0 && zmethod != 8) {
            fflush(stdout);
            fprintf(stderr, "\n%s: %s: first entry not deflated or stored -- use unzip\n", cz_gz_prog, cz_g.ifname);
            cz_g.exit_code = CZ_ERR;
            return -1;
        }
        if (zflags & 1) {
            fflush(stdout);
            fprintf(stderr, "\n%s: %s: encrypted file -- use unzip\n", cz_gz_prog, cz_g.ifname);
            cz_g.exit_code = CZ_ERR;
            return -1;
        }
        cz_g.ext_header = (zflags & 8) != 0;
        cz_g.zip_crc = (uint32_t)h[14] | (uint32_t)h[15] << 8 | (uint32_t)h[16] << 16 | (uint32_t)h[17] << 24;
        cz_g.zip_usize = (uint32_t)h[22] | (uint32_t)h[23] << 8 | (uint32_t)h[24] << 16 | (uint32_t)h[25] << 24;
        unsigned skip = (h[26] | (unsigned)h[27] << 8) + (h[28] | (unsigned)h[29] << 8);
        while (skip--) cz_gz_get_byte();
        cz_g.pkzip = 1;
        cz_g.method = zmethod == 8 ? CZ_M_DEFLATED : CZ_M_STORED;
        cz_g.last_member = 1;
    } else if (m0 == 0x1f && m1 == 0x1e) {
        cz_g.method = CZ_M_PACKED;
    } else if (m0 == 0x1f && m1 == 0x9d) {
        cz_g.method = CZ_M_LZW;
        cz_g.last_member = 1;
    } else if (m0 == 0x1f && m1 == 0xa0) {
        cz_g.method = CZ_M_LZH;
        cz_g.last_member = 1;
    } else if (cz_g.force && cz_g.to_stdout && !cz_g.list) {
        /* not compressed: copied as it is, these two bytes and all */
        cz_g.method = CZ_M_STORED;
        cz_g.last_member = 1;
        cz_g.pkzip = 0;
        if (m0 >= 0) cz_gz_putbyte((unsigned)m0);
        if (m1 >= 0) cz_gz_putbyte((unsigned)m1);
        return -4;   /* the copy: see cz_gz_work */
    }
    if (cz_g.method >= 0) return cz_g.method;
    if (cz_g.part_nb == 1) {
        fflush(stdout);
        fprintf(stderr, "\n%s: %s: not in gzip format\n", cz_gz_prog, cz_g.ifname);
        cz_g.exit_code = CZ_ERR;
        return -1;
    }
    if (m0 == 0) {
        int b = m1;
        while (b == 0) b = cz_gz_try_byte();
        if (b < 0) {
            if (cz_g.verbose) {
                fflush(stdout);
                if (!cz_g.quiet)
                    fprintf(stderr, "\n%s: %s: decompression OK, trailing zero bytes ignored\n",
                            cz_gz_prog, cz_g.ifname);
                cz_gz_warnset();
            }
            return -3;
        }
    }
    fflush(stdout);
    if (!cz_g.quiet)
        fprintf(stderr, "\n%s: %s: decompression OK, trailing garbage ignored\n", cz_gz_prog, cz_g.ifname);
    cz_gz_warnset();
    return -2;
}

/* ---- the decoders ---- */
static struct cz_inf *cz_gz_inf;

static int cz_gz_unzip(void) {
    if (!cz_gz_inf) cz_gz_inf = (struct cz_inf *)malloc(sizeof *cz_gz_inf);
    struct cz_inf *z = cz_gz_inf;
    z->in = &cz_g.in;
    z->out = cz_gz_inflate_out;
    z->octx = 0;
    uint32_t crc;
    uint64_t len;
    if (cz_g.pkzip && cz_g.method == CZ_M_STORED) {
        /* a stored zip member: its size is in the local header */
        uint64_t n = cz_g.zip_usize;
        uint8_t buf[4096];
        crc = 0;
        while (n) {
            size_t k = n < sizeof buf ? (size_t)n : sizeof buf;
            size_t got = cz_read(&cz_g.in, buf, k);
            crc = cz_crc32(crc, buf, got);
            cz_gz_put(buf, got);
            if (got < k) cz_gz_read_error(cz_g.in.err);
            n -= got;
        }
        len = cz_g.zip_usize;
    } else {
        int r = cz_inflate(z);
        if (r == CZ_IEOF) {
            /* the input ran out: what was decoded is written first */
            cz_iflush(z);
            cz_gz_read_error(cz_g.in.err);
        }
        if (r != CZ_IOK) cz_gz_fatal("invalid compressed data--format violated");
        crc = z->crc;
        len = z->outlen;
    }
    uint32_t want_crc, want_len;
    if (!cz_g.pkzip) {
        want_crc = cz_gz_get32();
        want_len = cz_gz_get32();
    } else if (cz_g.ext_header) {
        /* the data descriptor, with or without its signature */
        uint32_t a = cz_gz_get32();
        if (a == 0x08074b50u) a = cz_gz_get32();
        want_crc = a;
        cz_gz_get32();
        want_len = cz_gz_get32();
    } else {
        want_crc = cz_g.zip_crc;
        want_len = (uint32_t)cz_g.zip_usize;
    }
    cz_g.crc = crc;
    const char *bad = want_crc != crc ? "invalid compressed data--crc error"
                    : want_len != (uint32_t)len ? "invalid compressed data--length error" : 0;
    if (bad) {
        /* the end of the run when there is output; -t and -l go on to
         * the next file */
        if (!cz_g.test && !cz_g.list) cz_gz_fatal(bad);
        fflush(stdout);
        fprintf(stderr, "\n%s: %s: %s\n", cz_gz_prog, cz_g.ifname, bad);
        cz_g.exit_code = CZ_ERR;
        return CZ_ERR;
    }
    if (cz_g.pkzip) {
        /* the next member's signature: one entry is all gzip takes */
        uint8_t sig[4];
        int n = 0;
        while (n < 4) {
            int c = cz_peekc(&cz_g.in);
            if (c < 0) break;
            sig[n++] = (uint8_t)cz_getc(&cz_g.in);
        }
        if (n == 4 && sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4) {
            fflush(stdout);
            if (!cz_g.quiet)
                fprintf(stderr, "%s: %s has more than one entry--rest ignored\n", cz_gz_prog, cz_g.ifname);
            cz_gz_warnset();
        }
        /* the rest of a zip is its directory: read, and passed over */
        while (cz_getc(&cz_g.in) >= 0) continue;
    }
    return CZ_OK;
}

/* compress(1)'s .Z: LZW with codes of 9 to 16 bits */
static int cz_gz_unlzw(void) {
    int flags = cz_gz_get_byte();
    int maxbits = flags & 0x1f;
    int block = flags & 0x80;
    if (flags & 0x60) {
        fflush(stdout);
        if (!cz_g.quiet)
            fprintf(stderr, "\n%s: %s: warning, unknown flags 0x%x\n", cz_gz_prog, cz_g.ifname, flags & 0x60);
        cz_gz_warnset();
    }
    if (maxbits > 16) {
        fflush(stdout);
        fprintf(stderr, "\n%s: %s: compressed with %d bits, can only handle %d bits\n",
                cz_gz_prog, cz_g.ifname, maxbits, 16);
        cz_g.exit_code = CZ_ERR;
        return CZ_ERR;
    }
    uint32_t maxmax = 1u << maxbits;
    uint16_t *prefix = (uint16_t *)malloc(sizeof(uint16_t) << 16);
    uint8_t *suffix = (uint8_t *)malloc(1u << 16);
    uint8_t *stack = (uint8_t *)malloc(1u << 16);
    if (!prefix || !suffix || !stack) { fprintf(stderr, "%s: memory exhausted\n", cz_gz_prog); exit(CZ_ERR); }
    for (unsigned i = 0; i < 256; i++) { prefix[i] = 0; suffix[i] = (uint8_t)i; }
    int n_bits = 9;
    uint32_t maxcode = (1u << n_bits) - 1;
    uint32_t free_ent = block ? 257 : 256;
    long oldcode = -1;
    int finchar = 0;
    /* codes come in groups of eight; a width change or a clear skips to
     * the next group of the old width */
    uint64_t pos = 0, grp = 0;     /* bits read, and where the group run began */
    uint32_t acc = 0;
    int nacc = 0;
    for (;;) {
        /* as gzip reads it: the width grows whenever the table outgrows
         * it -- even past a 9-bit maximum, which then reads 10-bit codes */
        if (free_ent > maxcode) {
            uint64_t g = (uint64_t)n_bits * 8, used = pos - grp;
            uint64_t skip = used % g ? g - used % g : 0;
            while (skip) {
                if (nacc == 0) {
                    int c = cz_getc(&cz_g.in);
                    if (c < 0) goto done;
                    acc = (uint32_t)c;
                    nacc = 8;
                }
                uint64_t k = skip < (uint64_t)nacc ? skip : (uint64_t)nacc;
                acc >>= k;
                nacc -= (int)k;
                skip -= k;
                pos += k;
            }
            grp = pos;
            n_bits++;
            maxcode = n_bits == maxbits ? maxmax : (1u << n_bits) - 1;
        }
        while (nacc < n_bits) {
            int c = cz_getc(&cz_g.in);
            if (c < 0) {
                if (cz_g.in.err) cz_gz_read_error(cz_g.in.err);
                goto done;
            }
            acc |= (uint32_t)c << nacc;
            nacc += 8;
        }
        uint32_t code = acc & ((1u << n_bits) - 1);
        acc >>= n_bits;
        nacc -= n_bits;
        pos += (uint64_t)n_bits;
        if (oldcode == -1) {
            if (code >= 256) { fflush(stdout); fprintf(stderr, "\n%s: %s: corrupt input.\n", cz_gz_prog, cz_g.ifname); cz_gz_abort(); }
            finchar = (int)code;
            oldcode = (long)code;
            cz_gz_putbyte(code);
            continue;
        }
        if (code == 256 && block) {
            for (unsigned i = 0; i < 256; i++) prefix[i] = 0;
            free_ent = 256;
            uint64_t g = (uint64_t)n_bits * 8, used = pos - grp;
            uint64_t skip = used % g ? g - used % g : 0;
            while (skip) {
                if (nacc == 0) {
                    int c = cz_getc(&cz_g.in);
                    if (c < 0) goto done;
                    acc = (uint32_t)c;
                    nacc = 8;
                }
                uint64_t k = skip < (uint64_t)nacc ? skip : (uint64_t)nacc;
                acc >>= k;
                nacc -= (int)k;
                skip -= k;
                pos += k;
            }
            grp = pos;
            n_bits = 9;
            maxcode = (1u << n_bits) - 1;
            continue;
        }
        uint32_t incode = code;
        size_t sp = 0;
        if (code >= free_ent) {
            if (code > free_ent) {
                cz_gz_flush_out();
                fflush(stdout);
                fprintf(stderr, "\n%s: %s: corrupt input.\n", cz_gz_prog, cz_g.ifname);
                cz_gz_abort();
            }
            stack[sp++] = (uint8_t)finchar;
            code = (uint32_t)oldcode;
        }
        while (code >= 256) {
            stack[sp++] = suffix[code];
            code = prefix[code];
        }
        finchar = suffix[code];
        stack[sp++] = (uint8_t)finchar;
        while (sp) cz_gz_putbyte(stack[--sp]);
        if (free_ent < maxmax) {
            prefix[free_ent] = (uint16_t)oldcode;
            suffix[free_ent] = (uint8_t)finchar;
            free_ent++;
        }
        oldcode = (long)incode;
    }
done:
    free(prefix);
    free(suffix);
    free(stack);
    return CZ_OK;
}

/* pack(1)'s .z: one static Huffman code, given as the number of leaves
 * at each depth (the deepest one's count less two) and the leaves' bytes;
 * at each depth the inner nodes take the lowest codes, the leaves the
 * rest, and the deepest level's last leaf is the end of the data */
static int cz_gz_unpack(void) {
    uint64_t orig = 0;
    for (int i = 0; i < 4; i++) orig = orig << 8 | (unsigned)cz_gz_get_byte();
    int max_len = cz_gz_get_byte();
    if (max_len > 25) cz_gz_fatal("invalid compressed data -- Huffman code > 32 bits");
    int leaves[27], n = 0, max_leaves = 1;
    for (int len = 1; len <= max_len; len++) {
        leaves[len] = cz_gz_get_byte();
        if (max_leaves - (len == max_len) < leaves[len]) cz_gz_fatal("too many leaves in Huffman tree");
        max_leaves = (max_leaves - leaves[len] + 1) * 2 - 1;
        n += leaves[len];
    }
    if (n >= 256) cz_gz_fatal("too many leaves in Huffman tree");
    if (max_len > 0) leaves[max_len]++;
    uint8_t lit[260];
    int lbase[27], base = 0;
    for (int len = 1; len <= max_len; len++) {
        lbase[len] = base;
        for (int k = leaves[len]; k > 0; k--) lit[base++] = (uint8_t)cz_gz_get_byte();
    }
    if (max_len > 0) leaves[max_len]++;
    int eob = base;          /* the index just past the last byte leaf */
    int parents[27], nch = 0;
    for (int len = max_len; len >= 1; len--) {
        nch >>= 1;
        parents[len] = nch;
        nch += leaves[len];
    }
    uint64_t out = 0;
    unsigned acc = 0;
    int nacc = 0;
    for (;;) {
        int len = 0, c = 0, idx;
        for (;;) {
            if (nacc == 0) {
                acc = (unsigned)cz_gz_get_byte();
                nacc = 8;
            }
            c = c << 1 | (int)((acc >> 7) & 1);
            acc = (acc << 1) & 0xff;
            nacc--;
            len++;
            if (len > max_len) cz_gz_fatal("invalid compressed data--format violated");
            if (c >= parents[len]) break;
        }
        idx = lbase[len] + (c - parents[len]);
        if (idx >= eob) break;
        cz_gz_putbyte(lit[idx]);
        out++;
    }
    if (out != orig) cz_gz_fatal("invalid compressed data--length error");
    return CZ_OK;
}

static int cz_gz_copy_rest(void) {
    uint8_t buf[8192];
    for (;;) {
        size_t n = cz_read(&cz_g.in, buf, sizeof buf);
        if (n == 0) break;
        cz_gz_put(buf, n);
    }
    if (cz_g.in.err) cz_gz_read_error(cz_g.in.err);
    return CZ_OK;
}

static int cz_gz_work_decompress(int method) {
    switch (method) {
    case -4: return cz_gz_copy_rest();
    case CZ_M_DEFLATED: return cz_gz_unzip();
    case CZ_M_STORED: return cz_g.pkzip ? cz_gz_unzip() : cz_gz_copy_rest();
    case CZ_M_LZW: return cz_gz_unlzw();
    case CZ_M_PACKED: return cz_gz_unpack();
    case CZ_M_LZH:
        fflush(stdout);
        fprintf(stderr, "\n%s: %s: lzh-compressed (compress -H) input is not supported here\n",
                cz_gz_prog, cz_g.ifname);
        cz_g.exit_code = CZ_ERR;
        return CZ_ERR;
    }
    return CZ_ERR;
}

/* ---- compressing ---- */
static int cz_gz_rd(void *ctx, uint8_t *buf, unsigned n) {
    (void)ctx;
    for (;;) {
        ssize_t r = read(cz_g.ifd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) cz_gz_read_error(errno);
        if (r > 0) {
            cz_g.crc = cz_crc32(cz_g.crc, buf, (size_t)r);
            cz_g.bytes_in += r;
        }
        return (int)r;
    }
}

static int cz_gz_zip(void) {
    uint8_t h[10];
    int flags = 0;
    uint32_t stamp = 0;
    const char *name = 0;
    if (cz_g.save_orig_name) {
        name = cz_base(cz_g.ifname);
        flags |= 0x08;
    }
    if (!cz_g.no_time && cz_g.stamp != CZ_STAMP_UNSET) {
        if (cz_g.stamp > 0 && cz_g.stamp <= 0xffffffffLL) stamp = (uint32_t)cz_g.stamp;
        else {
            fflush(stdout);
            if (!cz_g.quiet)
                fprintf(stderr, "%s: %s: warning: file timestamp out of range for gzip format\n",
                        cz_gz_prog, cz_g.ifname);
            cz_gz_warnset();
        }
    }
    h[0] = 0x1f; h[1] = 0x8b; h[2] = 8; h[3] = (uint8_t)flags;
    h[4] = (uint8_t)stamp; h[5] = (uint8_t)(stamp >> 8);
    h[6] = (uint8_t)(stamp >> 16); h[7] = (uint8_t)(stamp >> 24);
    h[8] = cz_g.level == 1 ? 4 : cz_g.level == 9 ? 2 : 0;
    h[9] = 3;
    cz_gz_put(h, 10);
    if (name) cz_gz_put((const uint8_t *)name, strlen(name) + 1);
    cz_g.header_bytes = cz_g.bytes_out;
    cz_g.crc = 0;
    struct cz_def *s = (struct cz_def *)calloc(1, sizeof *s);
    if (!s) { fprintf(stderr, "%s: memory exhausted\n", cz_gz_prog); exit(CZ_ERR); }
    s->rd = cz_gz_rd;
    s->wr = cz_gz_deflate_out;
    s->rsync = cz_g.rsync;
    cz_deflate(s, cz_g.level);
    free(s);
    uint8_t t[8];
    uint32_t len = (uint32_t)cz_g.bytes_in;
    for (int i = 0; i < 4; i++) t[i] = (uint8_t)(cz_g.crc >> (8 * i));
    for (int i = 0; i < 4; i++) t[4 + i] = (uint8_t)(len >> (8 * i));
    cz_gz_put(t, 8);
    cz_g.header_bytes += 8;
    return CZ_OK;
}

/* ---- listing (-l) ---- */
static void cz_gz_list(int method) {
    static const char *const methods[9] = {"store", "compr", "pack ", "lzh  ", "", "", "", "", "defla"};
    if (cz_g.list_first && method >= 0) {
        cz_g.list_first = 0;
        if (cz_g.verbose) printf("method  crc     date  time  ");
        if (!cz_g.quiet) printf("%19s %19s  ratio uncompressed_name\n", "compressed", "uncompressed");
    } else if (method < 0) {
        if (cz_g.total_in <= 0 || cz_g.total_out <= 0 || cz_g.quiet) return;
        if (cz_g.verbose) printf("                            ");
        if (cz_g.verbose || !cz_g.quiet) printf("%19lld %19lld ", cz_g.total_in, cz_g.total_out);
        cz_ratio(stdout, cz_g.total_out - (cz_g.total_in - cz_g.header_bytes), cz_g.total_out);
        printf(" (totals)\n");
        return;
    }
    if (cz_g.verbose) {
        static const char months[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        /* the CRC is known only of a gzip member; ~0 otherwise, as
         * gzip prints an unsigned long of it */
        if (method == CZ_M_DEFLATED && !cz_g.last_member) printf("%5s %08lx ", methods[method], (unsigned long)cz_g.crc);
        else printf("%5s %08lx ", methods[method], ~0UL);
        time_t t = (time_t)cz_g.stamp;
        struct tm *tm = localtime(&t);
        if (tm) printf("%s%3d %02d:%02d ", months[tm->tm_mon], tm->tm_mday, tm->tm_hour, tm->tm_min);
        else printf("??? ?? ??:?? ");
    }
    printf("%19lld %19lld ", cz_g.bytes_in, cz_g.bytes_out);
    if (cz_g.total_in >= 0) cz_g.total_in += cz_g.bytes_in;
    if (cz_g.total_out >= 0) cz_g.total_out += cz_g.bytes_out;
    cz_ratio(stdout, cz_g.bytes_out - (cz_g.bytes_in - cz_g.header_bytes), cz_g.bytes_out);
    printf(" %s\n", cz_g.ofname);
}

/* ---- the file's metadata onto the output ---- */
static void cz_gz_stamp_of(const struct stat *st, long long *sec, long *ns) {
#ifdef SU_HOST
    *sec = (long long)st->st_mtim.tv_sec;
    *ns = st->st_mtim.tv_nsec;
#else
    *sec = (long long)st->st_mtime;
    *ns = 0;
#endif
}

static void cz_gz_copy_stat(void) {
    struct timespec ts[2];
#ifdef SU_HOST
    ts[0] = cz_g.istat.st_atim;
#else
    ts[0].tv_sec = cz_g.istat.st_atime;
    ts[0].tv_nsec = 0;
#endif
    long long s;
    long ns;
    if (cz_g.stamp != CZ_STAMP_UNSET && cz_g.decompress) { s = cz_g.stamp; ns = cz_g.stamp_ns; }
    else cz_gz_stamp_of(&cz_g.istat, &s, &ns);
    ts[1].tv_sec = (time_t)s;
    ts[1].tv_nsec = ns;
    utimensat(AT_FDCWD, cz_g.ofname, ts, 0);
    /* the group first (a change of owner may clear the set-id bits), then
     * the mode, then the owner; a refusal is no error, as with gzip */
    if (chown(cz_g.ofname, (uid_t)-1, cz_g.istat.st_gid) != 0) { /* kept */ }
    chmod(cz_g.ofname, cz_g.istat.st_mode & 07777);
    if (chown(cz_g.ofname, cz_g.istat.st_uid, (gid_t)-1) != 0) { /* kept */ }
}

static void cz_gz_sync(void) {
#ifdef SU_HOST
    if (cz_g.ofd >= 0) fsync(cz_g.ofd);
#else
    sc0(ABI_SYS_SYNC);
#endif
}

/* ---- opening ---- */
#define CZ_GZ_NOFD (-2)      /* a FIFO known by its stat alone (tobyOS) */
static int cz_gz_open_one(const char *name) {
    int nofollow = !(cz_g.to_stdout || cz_g.force);
#ifdef SU_HOST
    return open(name, O_RDONLY | O_NONBLOCK | (nofollow ? O_NOFOLLOW : 0));
#else
    /* no O_NOFOLLOW: a symbolic link is refused as one, by lstat */
    struct stat ls;
    if (nofollow && lstat(name, &ls) == 0 && S_ISLNK(ls.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    /* no O_NONBLOCK: a FIFO would hold the open until a writer came. When
     * it is not to be read anyway (no -c), its stat is all that is needed */
    if (!cz_g.to_stdout && stat(name, &ls) == 0 && S_ISFIFO(ls.st_mode)) {
        cz_g.istat = ls;
        return CZ_GZ_NOFD;
    }
    return su_open_read(name, 0);
#endif
}

/* the input, by its name or (decompressing) its name with a suffix; -1
 * when there is none, said */
static int cz_gz_open_input(const char *iname) {
    cz_gz_setname(&cz_g.ifname, &cz_g.ifcap, iname, CZ_MAX_SUFFIX + 2);
    int fd = cz_gz_open_one(cz_g.ifname);
    if (fd == CZ_GZ_NOFD) return fd;
    if (fd >= 0) {
        if (fstat(fd, &cz_g.istat) != 0) {
            cz_gz_progerror(cz_g.ifname, errno);
            close(fd);
            return -1;
        }
        return fd;
    }
    int e = errno;
    if (!cz_g.decompress || e != ENOENT) {
        cz_gz_progerror(cz_g.ifname, e);
        return -1;
    }
    if (cz_get_suffix(cz_g.ifname)) {
        cz_gz_progerror(cz_g.ifname, e);
        return -1;
    }
    size_t ilen = strlen(cz_g.ifname);
    const char *sufs[] = {cz_g.z_suffix, ".gz", ".z", "-z", "_z", ".Z", 0};
    int zerr = ENOENT;
    for (int i = 0; sufs[i]; i++) {
        if (i > 0 && strcmp(sufs[i], cz_g.z_suffix) == 0) continue;
        strcpy(cz_g.ifname + ilen, sufs[i]);
        fd = cz_gz_open_one(cz_g.ifname);
        if (fd >= 0) {
            if (fstat(fd, &cz_g.istat) != 0) {
                cz_gz_progerror(cz_g.ifname, errno);
                close(fd);
                return -1;
            }
            return fd;
        }
        if (errno != ENOENT) {
            cz_gz_progerror(cz_g.ifname, errno);
            return -1;
        }
        if (i == 0) zerr = errno;
    }
    strcpy(cz_g.ifname + ilen, cz_g.z_suffix);
    cz_gz_progerror(cz_g.ifname, zerr);
    return -1;
}

/* the output's name; CZ_OK, or not when the file is skipped */
static int cz_gz_make_ofname(void) {
    cz_gz_setname(&cz_g.ofname, &cz_g.ofcap, cz_g.ifname, cz_g.z_len + 8);
    char *suff = cz_get_suffix(cz_g.ofname);
    if (cz_g.decompress) {
        if (!suff) {
            /* -t and -l take a file of any name (not under -r) */
            if (!cz_g.recursive && (cz_g.list || cz_g.test)) return CZ_OK;
            if (cz_g.verbose || (!cz_g.recursive && !cz_g.quiet)) {
                fflush(stdout);
                fprintf(stderr, "%s: %s: unknown suffix -- ignored\n", cz_gz_prog, cz_g.ifname);
                cz_gz_warnset();
            }
            return CZ_WARN;
        }
        char low[CZ_MAX_SUFFIX + 1];
        snprintf(low, sizeof low, "%s", suff);
        cz_lower(low);
        if (strcmp(low, ".tgz") == 0 || strcmp(low, ".taz") == 0) strcpy(suff, ".tar");
        else *suff = 0;
        return CZ_OK;
    }
    if (suff && !cz_g.force) {
        if (cz_g.verbose || (!cz_g.recursive && !cz_g.quiet)) {
            fflush(stdout);
            fprintf(stderr, "%s: %s already has %s suffix -- unchanged\n", cz_gz_prog, cz_g.ifname, suff);
        }
        return CZ_WARN;
    }
    strcat(cz_g.ofname, cz_g.z_suffix);
    return CZ_OK;
}

static int cz_gz_yesno(void) {
    int c = getchar(), first = c;
    while (c != EOF && c != '\n') c = getchar();
    return first == 'y' || first == 'Y';
}

/* an existing output: asked about on a terminal, else refused; with -f,
 * removed */
static int cz_gz_check_ofname(void) {
    struct stat ost;
    if (stat(cz_g.ofname, &ost) != 0) return CZ_OK;
    if (!cz_g.force) {
        int ok = 0;
        fflush(stdout);
        fprintf(stderr, "%s: %s already exists;", cz_gz_prog, cz_g.ofname);
        if (isatty(0)) {
            fprintf(stderr, " do you wish to overwrite (y or n)? ");
            fflush(stderr);
            ok = cz_gz_yesno();
        }
        if (!ok) {
            fprintf(stderr, "\tnot overwritten\n");
            cz_gz_warnset();
            return CZ_ERR;
        }
    }
    if (unlink(cz_g.ofname) != 0) {
        cz_gz_progerror(cz_g.ofname, errno);
        return CZ_ERR;
    }
    return CZ_OK;
}

static int cz_gz_create(void) {
    if (cz_gz_check_ofname() != CZ_OK) return CZ_ERR;
    cz_g.ofd = open(cz_g.ofname, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (cz_g.ofd < 0) {
        int e = errno;
        fflush(stdout);
        fprintf(stderr, "\n%s: %s: %s\n", cz_gz_prog, cz_g.ofname, strerror(e));
        cz_g.exit_code = CZ_ERR;
        return CZ_ERR;
    }
    cz_g.remove_ofname = 1;
    return CZ_OK;
}

/* ---- one input, from open to the verbose line ---- */
static void cz_gz_reset_counts(void) {
    cz_g.bytes_in = cz_g.bytes_out = 0;
    cz_g.header_bytes = 0;
    cz_g.part_nb = 0;
    cz_g.pkzip = 0;
    cz_g.ext_header = 0;
    cz_g.last_member = 0;
    cz_g.crc = 0;
    cz_g.on = 0;
}

/* decompress (or test, or list) all members of the open input */
static int cz_gz_decompress_all(int method) {
    for (;;) {
        if (cz_gz_work_decompress(method) != CZ_OK) { method = -1; break; }
        if (method == -4 || cz_g.last_member) break;
        if (cz_peekc(&cz_g.in) < 0) {
            if (cz_g.in.err) cz_gz_read_error(cz_g.in.err);
            break;
        }
        method = cz_get_method();
        if (method == -4) { cz_gz_copy_rest(); break; }
        if (method < 0) break;
    }
    cz_g.bytes_in = (long long)cz_g.in.total;
    return method;
}

static void cz_gz_verbose_end(void) {
    if (cz_g.test) fprintf(stderr, " OK");
    else if (cz_g.decompress) cz_ratio(stderr, cz_g.bytes_out - (cz_g.bytes_in - cz_g.header_bytes), cz_g.bytes_out);
    else cz_ratio(stderr, cz_g.bytes_in - (cz_g.bytes_out - cz_g.header_bytes), cz_g.bytes_in);
}

static void cz_gz_treat_stdin(void) {
    if (!cz_g.force && !cz_g.list && isatty(cz_g.decompress ? 0 : 1)) {
        if (!cz_g.quiet)
            fprintf(stderr, "%s: compressed data not %s a terminal. Use -f to force %scompression.\n"
                            "For help, type: %s -h\n",
                    cz_gz_prog, cz_g.decompress ? "read from" : "written to",
                    cz_g.decompress ? "de" : "", cz_gz_prog);
        exit(CZ_ERR);
    }
    cz_gz_setname(&cz_g.ifname, &cz_g.ifcap, "stdin", 0);
    cz_gz_setname(&cz_g.ofname, &cz_g.ofcap, "stdout", 0);
    if (fstat(0, &cz_g.istat) != 0) {
        cz_gz_progerror("standard input", errno);
        exit(CZ_ERR);
    }
    cz_g.ifile_size = S_ISREG(cz_g.istat.st_mode) ? (long long)cz_g.istat.st_size : -1;
    cz_g.stamp = CZ_STAMP_UNSET;
    cz_g.stamp_ns = 0;
    if ((!cz_g.no_time || cz_g.list) && S_ISREG(cz_g.istat.st_mode))
        cz_gz_stamp_of(&cz_g.istat, &cz_g.stamp, &cz_g.stamp_ns);
    int save_to_stdout = cz_g.to_stdout;
    cz_g.to_stdout = 1;
    cz_g.ifd = 0;
    cz_g.ofd = 1;
    cz_g.remove_ofname = 0;
    cz_gz_reset_counts();
    cz_g.out_discard = cz_g.test || cz_g.list;
    if (cz_g.decompress) {
        if (cz_g.stdin_done) {
            /* a second "-": what was read is gone */
        }
        cz_in_close(&cz_g.in);
        cz_in_open(&cz_g.in, 0, 65536);
        cz_g.stdin_done = 1;
        int method = cz_get_method();
        if (method < 0 && method != -4) exit(cz_g.exit_code);
        method = cz_gz_decompress_all(method);
        cz_gz_flush_out();
        if (cz_g.list) {
            cz_gz_list(method);
            cz_g.to_stdout = save_to_stdout;
            return;
        }
        if (method == -1) exit(cz_g.exit_code);
    } else {
        /* the name is stored as "stdin" once a FILE before has stored
         * its own: gzip's flag outlives the file that set it */
        cz_gz_zip();
        cz_gz_flush_out();
    }
    if (cz_g.verbose) {
        if (cz_g.test) fprintf(stderr, " OK\n");
        else if (!cz_g.decompress) {
            cz_ratio(stderr, cz_g.bytes_in - (cz_g.bytes_out - cz_g.header_bytes), cz_g.bytes_in);
            fprintf(stderr, "\n");
        }
    }
    cz_g.to_stdout = save_to_stdout;
}

static void cz_gz_treat_file(const char *iname);

static void cz_gz_treat_dir(int fd, const char *dir) {
    close(fd);
    DIR *d = opendir(dir);
    if (!d) {
        cz_gz_progerror(dir, errno);
        return;
    }
    /* all the names first: what is made in the directory meanwhile is
     * not visited */
    char **names = 0;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = (char **)realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    size_t dl = strlen(dir);
    for (size_t i = 0; i < n; i++) {
        char *path = (char *)malloc(dl + strlen(names[i]) + 2);
        strcpy(path, dir);
        if (dl == 0 || dir[dl - 1] != '/') strcat(path, "/");
        strcat(path, names[i]);
        cz_gz_treat_file(path);
        free(path);
        free(names[i]);
    }
    free(names);
}

static void cz_gz_treat_file(const char *iname) {
    if (strcmp(iname, "-") == 0) {
        cz_gz_treat_stdin();
        return;
    }
    int fd = cz_gz_open_input(iname);
    if (fd < 0 && fd != CZ_GZ_NOFD) return;
    if (S_ISDIR(cz_g.istat.st_mode)) {
        if (cz_g.recursive) {
            char *dir = strdup(cz_g.ifname);
            cz_gz_treat_dir(fd, dir);
            free(dir);
            return;
        }
        close(fd);
        fflush(stdout);
        if (!cz_g.quiet) fprintf(stderr, "%s: %s is a directory -- ignored\n", cz_gz_prog, cz_g.ifname);
        cz_gz_warnset();
        return;
    }
    if (!cz_g.to_stdout) {
        const char *why = 0;
        if (!S_ISREG(cz_g.istat.st_mode)) why = "is not a directory or a regular file - ignored";
        else if (cz_g.istat.st_mode & S_ISUID) why = "is set-user-ID on execution - ignored";
        else if (cz_g.istat.st_mode & S_ISGID) why = "is set-group-ID on execution - ignored";
        else if (!cz_g.force && (cz_g.istat.st_mode & S_ISVTX)) why = "has the sticky bit set - file ignored";
        if (why) {
            close(fd);
            fflush(stdout);
            if (!cz_g.quiet) fprintf(stderr, "%s: %s %s\n", cz_gz_prog, cz_g.ifname, why);
            cz_gz_warnset();
            return;
        }
        if (!cz_g.force && cz_g.istat.st_nlink > 1) {
            unsigned long others = (unsigned long)cz_g.istat.st_nlink - 1;
            close(fd);
            fflush(stdout);
            if (!cz_g.quiet)
                fprintf(stderr, "%s: %s has %lu other link%s -- file ignored\n", cz_gz_prog,
                        cz_g.ifname, others, others == 1 ? "" : "s");
            cz_gz_warnset();
            return;
        }
    }
    cz_g.ifile_size = S_ISREG(cz_g.istat.st_mode) ? (long long)cz_g.istat.st_size : -1;
    cz_g.stamp = CZ_STAMP_UNSET;
    cz_g.stamp_ns = 0;
    if (!cz_g.no_time || cz_g.list) cz_gz_stamp_of(&cz_g.istat, &cz_g.stamp, &cz_g.stamp_ns);
    if (cz_g.to_stdout && !cz_g.list && !cz_g.test) {
        cz_gz_setname(&cz_g.ofname, &cz_g.ofcap, "stdout", 0);
    } else if (cz_gz_make_ofname() != CZ_OK) {
        close(fd);
        return;
    }
    cz_g.ifd = fd;
    cz_gz_reset_counts();
    cz_g.remove_ofname = 0;
    cz_g.ofd = -1;
    cz_g.out_discard = cz_g.test || cz_g.list;
    int method = 0;
    if (cz_g.decompress) {
        cz_in_close(&cz_g.in);
        cz_in_open(&cz_g.in, fd, 65536);
        method = cz_get_method();
        if (method < 0 && method != -4) {
            close(fd);
            return;
        }
    }
    if (cz_g.list) {
        method = cz_gz_decompress_all(method);
        close(fd);
        cz_gz_list(method);   /* an error too: the totals so far, as gzip prints them */
        return;
    }
    if (cz_g.to_stdout) cz_g.ofd = 1;
    else if (cz_gz_create() != CZ_OK) {
        close(fd);
        return;
    }
    if (!cz_g.save_orig_name) cz_g.save_orig_name = !cz_g.no_name;
    if (cz_g.verbose) {
        fflush(stdout);
        fprintf(stderr, "%s:\t", cz_g.ifname);
    }
    if (cz_g.decompress) method = cz_gz_decompress_all(method);
    else cz_gz_zip();
    cz_gz_flush_out();
    close(fd);
    if (!cz_g.to_stdout) {
        cz_gz_copy_stat();
        if (cz_g.synchronous) cz_gz_sync();
        if (close(cz_g.ofd) != 0) cz_gz_write_error(errno);
        cz_g.ofd = -1;
    }
    if (method == -1) {
        if (!cz_g.to_stdout && cz_g.remove_ofname) unlink(cz_g.ofname);
        cz_g.remove_ofname = 0;
        return;
    }
    cz_g.remove_ofname = 0;
    if (cz_g.verbose) {
        cz_gz_verbose_end();
        if (!cz_g.test) fprintf(stderr, " -- %s %s", cz_g.keep ? "created" : "replaced with", cz_g.ofname);
        fprintf(stderr, "\n");
    }
    if (!cz_g.keep && !cz_g.to_stdout) {
        if (unlink(cz_g.ifname) != 0) cz_gz_progerror(cz_g.ifname, errno);
    }
}

/* ---- the command line ---- */
#define CZ_O_SYNC 0x11001
#define CZ_O_RSYNC 0x11002

static const struct su_option cz_gz_longs[] = {
    { "ascii", SU_NO_ARG, 0, 'a' },
    { "to-stdout", SU_NO_ARG, 0, 'c' },
    { "stdout", SU_NO_ARG, 0, 'c' },
    { "decompress", SU_NO_ARG, 0, 'd' },
    { "uncompress", SU_NO_ARG, 0, 'd' },
    { "force", SU_NO_ARG, 0, 'f' },
    { "help", SU_NO_ARG, 0, 'h' },
    { "keep", SU_NO_ARG, 0, 'k' },
    { "list", SU_NO_ARG, 0, 'l' },
    { "license", SU_NO_ARG, 0, 'L' },
    { "no-name", SU_NO_ARG, 0, 'n' },
    { "name", SU_NO_ARG, 0, 'N' },
    { "quiet", SU_NO_ARG, 0, 'q' },
    { "silent", SU_NO_ARG, 0, 'q' },
    { "synchronous", SU_NO_ARG, 0, CZ_O_SYNC },
    { "recursive", SU_NO_ARG, 0, 'r' },
    { "suffix", SU_REQ_ARG, 0, 'S' },
    { "test", SU_NO_ARG, 0, 't' },
    { "verbose", SU_NO_ARG, 0, 'v' },
    { "version", SU_NO_ARG, 0, 'V' },
    { "fast", SU_NO_ARG, 0, '1' },
    { "best", SU_NO_ARG, 0, '9' },
    { "lzw", SU_NO_ARG, 0, 'Z' },
    { "bits", SU_REQ_ARG, 0, 'b' },
    { "rsyncable", SU_NO_ARG, 0, CZ_O_RSYNC },
    { 0, 0, 0, 0 }
};
#define CZ_GZ_SHORTS "123456789acdfhH?klLmMnNqrS:tvVZb:"

static void cz_gz_try_help(void) __attribute__((noreturn));
static void cz_gz_try_help(void) {
    fflush(stdout);
    fprintf(stderr, "Try `%s --help' for more information.\n", cz_gz_prog);
    exit(CZ_ERR);
}

static void cz_gz_help(void) {
    printf("Usage: %s [OPTION]... [FILE]...\n"
           "Compress each FILE with gzip's format (or, with -d, restore it), each\n"
           "replaced by the result unless told otherwise.\n"
           "\n"
           "An option's long form takes the same argument as its short one.\n"
           "\n"
           "  -c, --stdout      send the result to standard output; leave FILEs as they are\n"
           "  -d, --decompress  restore compressed FILEs\n"
           "  -f, --force       overwrite outputs, take linked files, pass non-gzip data\n"
           "  -h, --help        print this help\n"
           "  -k, --keep        do not remove the input files\n"
           "  -l, --list        show each compressed FILE's sizes and ratio\n"
           "  -L, --license     print the license\n"
           "  -n, --no-name     neither store nor restore the original name and time\n"
           "  -N, --name        store and restore the original name and time\n"
           "  -q, --quiet       say nothing of warnings\n"
           "  -r, --recursive   descend into directories\n"
           "      --rsyncable   compress so that rsync can resynchronize\n"
           "  -S, --suffix=SUF  use the suffix SUF instead of .gz\n"
           "      --synchronous make each output reach the disk before going on\n"
           "  -t, --test        check that compressed FILEs are intact\n"
           "  -v, --verbose     say what is done to each FILE\n"
           "  -V, --version     print the version\n"
           "  -1, --fast        the fastest compression\n"
           "  -9, --best        the smallest output\n"
           "\n"
           "Standard input is read when there is no FILE, and for a FILE of -.\n",
           cz_gz_prog);
}

/* GZIP in the environment: only what changes the compressed data's
 * shape (-1..-9, --fast, --best, --rsyncable) is taken from it; the rest,
 * and anything it cannot parse, is passed over in silence */
static void cz_gz_env(void) {
    const char *env = getenv("GZIP");
    if (!env || !*env) return;
    char *copy = strdup(env);
    char *argv[128];
    int argc = 0;
    argv[argc++] = (char *)"gzip";
    for (char *p = strtok(copy, " \t\n"); p && argc < 127; p = strtok(0, " \t\n")) argv[argc++] = p;
    argv[argc] = 0;
    su_optind = 1; su_nextind = 1; su_cluster = 0; su_nnonopt = 0; su_noptelem = 0;
    su_opterr = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, CZ_GZ_SHORTS, cz_gz_longs, 0)) != -1) {
        if (c >= '1' && c <= '9') cz_g.level = c - '0';
        else if (c == CZ_O_RSYNC) cz_g.rsync = 1;
    }
    su_opterr = 1;
    su_optind = 1; su_nextind = 1; su_cluster = 0; su_nnonopt = 0; su_noptelem = 0;
    free(copy);
}

static int cz_gz_main(int argc, char **argv, int mode) {
    memset(&cz_g, 0, sizeof cz_g);
    cz_g.obuf = (uint8_t *)malloc(CZ_OBUFSZ);
    cz_g.ofd = -1;
    cz_g.level = 6;
    cz_g.no_name = -1;
    cz_g.no_time = -1;
    cz_g.z_suffix = ".gz";
    cz_g.list_first = 1;
    cz_gz_env();
    if (mode >= 1) cz_g.decompress = 1;
    if (mode == 2) cz_g.to_stdout = 1;
    int c;
    while ((c = su_getopt_long(argc, argv, CZ_GZ_SHORTS, cz_gz_longs, 0)) != -1) {
        switch (c) {
        case 'a': cz_g.ascii = 1; break;
        case 'b': {
            const char *p = su_optarg;
            if (!*p) goto badbits;
            for (; *p; p++) if (*p < '0' || *p > '9') goto badbits;
            break;
        badbits:
            fflush(stdout);
            fprintf(stderr, "%s: -b operand is not an integer\n", cz_gz_prog);
            cz_gz_try_help();
        }
        case 'c': cz_g.to_stdout = 1; break;
        case 'd': cz_g.decompress = 1; break;
        case 'f': cz_g.force++; break;
        case 'h': case 'H': cz_gz_help(); return 0;
        case 'k': cz_g.keep = 1; break;
        case 'l': cz_g.list = cz_g.decompress = cz_g.to_stdout = 1; break;
        case 'L':
            printf("%s (tobyOS) 1.0\n"
                   "This gzip is part of tobyOS, written for it from the gzip file format's\n"
                   "specification (RFC 1951, RFC 1952) and gzip's observable behaviour.\n",
                   cz_gz_prog);
            return 0;
        case 'm': cz_g.no_time = 1; break;
        case 'M': cz_g.no_time = 0; break;
        case 'n': cz_g.no_name = cz_g.no_time = 1; break;
        case 'N': cz_g.no_name = cz_g.no_time = 0; break;
        case 'q': cz_g.quiet = 1; cz_g.verbose = 0; break;
        case 'r': cz_g.recursive = 1; break;
        case 'S': cz_g.z_suffix = su_optarg; break;
        case CZ_O_SYNC: cz_g.synchronous = 1; break;
        case CZ_O_RSYNC: cz_g.rsync = 1; break;
        case 't': cz_g.test = cz_g.decompress = cz_g.to_stdout = 1; break;
        case 'v': cz_g.verbose++; cz_g.quiet = 0; break;
        case 'V': printf("%s (tobyOS) 1.0\n", cz_gz_prog); return 0;
        case 'Z':
            fflush(stdout);
            fprintf(stderr, "%s: -Z not supported in this version\n", cz_gz_prog);
            cz_gz_try_help();
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            cz_g.level = c - '0';
            break;
        default:
            cz_gz_try_help();
        }
    }
    if (cz_g.no_time < 0) cz_g.no_time = cz_g.decompress;
    if (cz_g.no_name < 0) cz_g.no_name = cz_g.decompress;
    cz_g.file_count = argc - su_optind;
    if (cz_g.ascii && !cz_g.quiet)
        fprintf(stderr, "%s: option --ascii ignored on this system\n", cz_gz_prog);
    cz_g.z_len = strlen(cz_g.z_suffix);
    if (cz_g.z_len == 0 || cz_g.z_len > CZ_MAX_SUFFIX) {
        fprintf(stderr, "%s: invalid suffix '%s'\n", cz_gz_prog, cz_g.z_suffix);
        return CZ_ERR;
    }
    if (strchr(cz_g.z_suffix, '/')) {
        fprintf(stderr, "%s: suffix contains '/'\n", cz_gz_prog);
        return CZ_ERR;
    }
    if (cz_g.file_count == 0) cz_gz_treat_stdin();
    else for (int i = su_optind; i < argc; i++) cz_gz_treat_file(argv[i]);
    if (cz_g.list) {
        if (cz_g.file_count > 1) cz_gz_list(-1);
        fflush(stdout);
        if (ferror(stdout)) {
            fprintf(stderr, "\n%s: %s: %s\n", cz_gz_prog, cz_g.ofname, strerror(errno));
            return CZ_ERR;
        }
    }
    fflush(stdout);
    return cz_g.exit_code;
}

/* gzip as gunzip and zcat are: GNU's are scripts that take --help and
 * --version as their first argument and run gzip -d (-dc) otherwise --
 * so every other message, -h's help included, is gzip's */
static int cz_gz_wrapper(int argc, char **argv, int mode) {
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        if (mode == 1)
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Restore FILEs compressed by gzip (each in place, by default).\n"
                   "\n"
                   "An option's long form takes the same argument as its short one.\n"
                   "\n"
                   "  -c, --stdout      send the result to standard output; leave FILEs as they are\n"
                   "  -f, --force       overwrite outputs, take linked files, pass non-gzip data\n"
                   "  -k, --keep        do not remove the input files\n"
                   "  -l, --list        show each compressed FILE's sizes and ratio\n"
                   "  -n, --no-name     do not restore the original name and time\n"
                   "  -N, --name        restore the original name and time\n"
                   "  -q, --quiet       say nothing of warnings\n"
                   "  -r, --recursive   descend into directories\n"
                   "  -S, --suffix=SUF  take SUF as a compressed file's suffix too\n"
                   "      --synchronous make each output reach the disk before going on\n"
                   "  -t, --test        check that compressed FILEs are intact\n"
                   "  -v, --verbose     say what is done to each FILE\n"
                   "      --help        print this help\n"
                   "      --version     print the version\n"
                   "\n"
                   "Standard input is read when there is no FILE, and for a FILE of -.\n",
                   argv[0]);
        else
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Write FILEs compressed by gzip to standard output, restored.\n"
                   "\n"
                   "  -f, --force       read compressed data from a terminal too\n"
                   "  -l, --list        show each compressed FILE's sizes and ratio\n"
                   "  -q, --quiet       say nothing of warnings\n"
                   "  -r, --recursive   descend into directories\n"
                   "  -S, --suffix=SUF  take SUF as a compressed file's suffix too\n"
                   "      --synchronous make each output reach the disk before going on\n"
                   "  -t, --test        check that compressed FILEs are intact\n"
                   "  -v, --verbose     say what is done to each FILE\n"
                   "      --help        print this help\n"
                   "      --version     print the version\n"
                   "\n"
                   "Standard input is read when there is no FILE, and for a FILE of -.\n",
                   argv[0]);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) return su_version();
    /* what follows is gzip's, by gzip's name */
    char *save0 = argv[0];
    argv[0] = (char *)"gzip";
    int r = cz_gz_main(argc, argv, mode);
    argv[0] = save0;
    return r;
}

static int do_gzip(int argc, char **argv) {
    const char *b = cz_base(argv[0]);
    if (strcmp(b, "gunzip") == 0) return cz_gz_wrapper(argc, argv, 1);
    if (strcmp(b, "zcat") == 0) return cz_gz_wrapper(argc, argv, 2);
    return cz_gz_main(argc, argv, 0);
}
