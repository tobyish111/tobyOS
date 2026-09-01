/* bzip2.c -- bzip2 decompression (Burrows-Wheeler).
 *
 * Nothing in this tree could read a .bz2 file, which also made bzcat,
 * bzgrep and bzdiff impossible. This is a from-scratch decoder for the
 * format as documented: a bit stream read MOST significant bit first,
 * carrying a symbol map, 2..6 Huffman tables with a selector per 50
 * symbols, MTF + run-length coded values, and a Burrows-Wheeler transform
 * that is inverted with the block's origin pointer.
 *
 * Two details bite anyone writing this the first time:
 *
 *   - bzip2 is MSB-first, the opposite of DEFLATE. The two formats are
 *     otherwise similar enough that mixing the bit orders produces a
 *     stream that decodes for a while and then falls apart.
 *
 *   - the "randomised" block flag is a dead feature from bzip2 0.9.0 and
 *     earlier. Nothing has produced one this century, and rather than
 *     carry an untestable derandomisation table this refuses such a block
 *     and says so, which is better than silently emitting wrong bytes.
 */

#include <toby/bzip2.h>

#include <stdlib.h>
#include <string.h>

#define BZ_MAX_ALPHA_SIZE 258
#define BZ_MAX_CODE_LEN   23
#define BZ_N_GROUPS       6
#define BZ_G_SIZE         50
#define BZ_RUNA           0
#define BZ_RUNB           1

/* bzip2's CRC feeds each byte in MOST significant bit first -- the mirror
 * of the one gzip uses. Shared with the encoder, which must produce the
 * same value the decoder checks. */
static uint32_t bzcrc_tab[256];
static int      bzcrc_ready;

uint32_t bz_crc32(const uint8_t *p, size_t n) {
    if (!bzcrc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; k++)
                c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
            bzcrc_tab[i] = c;
        }
        bzcrc_ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = (c << 8) ^ bzcrc_tab[((c >> 24) ^ p[i]) & 0xFFu];
    return ~c;
}

struct bzin {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
    uint32_t       acc;
    int            nbits;
    int            err;
};

/* MSB-first, unlike DEFLATE. n is at most 32. */
static uint32_t bz_bits(struct bzin *s, int n) {
    uint32_t v = 0;
    while (n > 0) {
        if (s->nbits == 0) {
            if (s->pos >= s->len) { s->err = 1; return 0; }
            s->acc   = s->p[s->pos++];
            s->nbits = 8;
        }
        int take = (n < s->nbits) ? n : s->nbits;
        v = (v << take) |
            ((s->acc >> (s->nbits - take)) & ((1u << take) - 1u));
        s->nbits -= take;
        n        -= take;
    }
    return v;
}

/* The decode tables bzip2 uses: `perm` lists symbols in code-length order,
 * and limit/base let a symbol be found by widening the code one bit at a
 * time. */
static void hb_create(int32_t *limit, int32_t *base, int32_t *perm,
                      const uint8_t *length, int minLen, int maxLen,
                      int alphaSize)
{
    int pp = 0;
    for (int i = minLen; i <= maxLen; i++)
        for (int j = 0; j < alphaSize; j++)
            if (length[j] == i) perm[pp++] = j;

    for (int i = 0; i < BZ_MAX_CODE_LEN; i++) base[i]  = 0;
    for (int i = 0; i < alphaSize; i++)       base[length[i] + 1]++;
    for (int i = 1; i < BZ_MAX_CODE_LEN; i++) base[i] += base[i - 1];
    for (int i = 0; i < BZ_MAX_CODE_LEN; i++) limit[i] = 0;

    int32_t vec = 0;
    for (int i = minLen; i <= maxLen; i++) {
        vec += (base[i + 1] - base[i]);
        limit[i] = vec - 1;
        vec <<= 1;
    }
    for (int i = minLen + 1; i <= maxLen; i++)
        base[i] = ((limit[i - 1] + 1) << 1) - base[i];
}

struct outbuf {
    uint8_t *d;
    size_t   cap, len;
    int      err;
};

static int out_byte(struct outbuf *o, uint8_t c) {
    if (o->err) return 0;
    if (o->len == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 65536;
        uint8_t *nd = (uint8_t *)realloc(o->d, nc);
        if (!nd) { o->err = 1; return 0; }
        o->d = nd; o->cap = nc;
    }
    o->d[o->len++] = c;
    return 1;
}

int bz_decompress(const uint8_t *in, size_t inlen,
                  uint8_t **out, size_t *outlen, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "ok";
    if (!in || !out || !outlen) { *why = "bad arguments"; return -1; }
    if (inlen < 10 || in[0] != 'B' || in[1] != 'Z' || in[2] != 'h') {
        *why = "not in bzip2 format";
        return -1;
    }
    int level = in[3] - '0';
    if (level < 1 || level > 9) { *why = "bad block-size byte"; return -1; }
    uint32_t maxblock = (uint32_t)level * 100000u;

    struct bzin s;
    memset(&s, 0, sizeof s);
    s.p = in; s.len = inlen; s.pos = 4;

    struct outbuf o;
    memset(&o, 0, sizeof o);

    uint32_t *tt    = (uint32_t *)malloc((size_t)maxblock * sizeof(uint32_t));
    uint8_t  *block = (uint8_t  *)malloc((size_t)maxblock);
    if (!tt || !block) { free(tt); free(block); *why = "out of memory"; return -1; }

    int rc = -1;
    for (;;) {
        uint32_t hi = bz_bits(&s, 24), lo = bz_bits(&s, 24);
        if (s.err) { *why = "truncated stream"; goto done; }

        if (hi == 0x177245u && lo == 0x385090u) {   /* end of stream */
            (void)bz_bits(&s, 32);                  /* combined CRC */
            rc = 0;
            goto done;
        }
        if (hi != 0x314159u || lo != 0x265359u) {
            *why = "bad block magic";
            goto done;
        }

        uint32_t stored_crc = bz_bits(&s, 32);
        size_t   block_out_start = o.len;
        if (bz_bits(&s, 1)) {
            *why = "randomised block (bzip2 0.9.0 and older) is not supported";
            goto done;
        }
        uint32_t origPtr = bz_bits(&s, 24);

        /* ---- symbol map ---- */
        int inUse[256];
        memset(inUse, 0, sizeof inUse);
        uint32_t used16 = bz_bits(&s, 16);
        for (int i = 0; i < 16; i++)
            if (used16 & (0x8000u >> i)) {
                uint32_t bits = bz_bits(&s, 16);
                for (int j = 0; j < 16; j++)
                    if (bits & (0x8000u >> j)) inUse[i * 16 + j] = 1;
            }
        uint8_t seqToUnseq[256];
        int nInUse = 0;
        for (int i = 0; i < 256; i++) if (inUse[i]) seqToUnseq[nInUse++] = (uint8_t)i;
        if (nInUse == 0) { *why = "empty symbol map"; goto done; }
        int alphaSize = nInUse + 2;

        /* ---- Huffman group selectors ---- */
        int nGroups    = (int)bz_bits(&s, 3);
        int nSelectors = (int)bz_bits(&s, 15);
        if (nGroups < 2 || nGroups > BZ_N_GROUPS || nSelectors < 1) {
            *why = "bad group header";
            goto done;
        }
        uint8_t *selectors = (uint8_t *)malloc((size_t)nSelectors);
        if (!selectors) { *why = "out of memory"; goto done; }
        {
            uint8_t mtf[BZ_N_GROUPS];
            for (int i = 0; i < nGroups; i++) mtf[i] = (uint8_t)i;
            for (int i = 0; i < nSelectors; i++) {
                int j = 0;
                while (bz_bits(&s, 1)) {
                    if (++j >= nGroups || s.err) { s.err = 1; break; }
                }
                if (s.err) { free(selectors); *why = "bad selector"; goto done; }
                uint8_t v = mtf[j];
                for (int k = j; k > 0; k--) mtf[k] = mtf[k - 1];
                mtf[0] = v;
                selectors[i] = v;
            }
        }

        /* ---- code lengths, delta coded ---- */
        static uint8_t len[BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE];
        static int32_t limit[BZ_N_GROUPS][BZ_MAX_CODE_LEN];
        static int32_t base [BZ_N_GROUPS][BZ_MAX_CODE_LEN];
        static int32_t perm [BZ_N_GROUPS][BZ_MAX_ALPHA_SIZE];
        int minLens[BZ_N_GROUPS];

        for (int t = 0; t < nGroups; t++) {
            int curr = (int)bz_bits(&s, 5);
            for (int i = 0; i < alphaSize; i++) {
                for (;;) {
                    if (curr < 1 || curr > 20 || s.err) { s.err = 1; break; }
                    if (!bz_bits(&s, 1)) break;
                    if (!bz_bits(&s, 1)) curr++; else curr--;
                }
                if (s.err) { free(selectors); *why = "bad code length"; goto done; }
                len[t][i] = (uint8_t)curr;
            }
        }
        for (int t = 0; t < nGroups; t++) {
            int mn = 32, mx = 0;
            for (int i = 0; i < alphaSize; i++) {
                if (len[t][i] > mx) mx = len[t][i];
                if (len[t][i] < mn) mn = len[t][i];
            }
            hb_create(limit[t], base[t], perm[t], len[t], mn, mx, alphaSize);
            minLens[t] = mn;
        }

        /* ---- MTF + run-length decode into the BWT block ---- */
        uint32_t unzftab[256];
        memset(unzftab, 0, sizeof unzftab);
        uint8_t mtfv[256];
        for (int i = 0; i < nInUse; i++) mtfv[i] = seqToUnseq[i];

        uint32_t nblock = 0;
        int      groupNo = -1, groupPos = 0, gsel = 0;
        int      EOB = alphaSize - 1;
        uint32_t runLen = 0;
        uint32_t runBit = 0;

#define BZ_NEXT_SYM(dst)                                                     \
        do {                                                                 \
            if (groupPos == 0) {                                             \
                if (++groupNo >= nSelectors) { s.err = 1; break; }            \
                groupPos = BZ_G_SIZE;                                        \
                gsel = selectors[groupNo];                                   \
            }                                                                \
            groupPos--;                                                      \
            int zn_ = minLens[gsel];                                         \
            int32_t zv_ = (int32_t)bz_bits(&s, zn_);                          \
            while (zn_ <= BZ_MAX_CODE_LEN && zv_ > limit[gsel][zn_]) {        \
                zn_++;                                                       \
                zv_ = (zv_ << 1) | (int32_t)bz_bits(&s, 1);                   \
            }                                                                \
            int32_t ix_ = zv_ - base[gsel][zn_];                              \
            if (zn_ > BZ_MAX_CODE_LEN || ix_ < 0 || ix_ >= alphaSize)        \
                { s.err = 1; break; }                                        \
            (dst) = perm[gsel][ix_];                                         \
        } while (0)

        int sym = 0;
        BZ_NEXT_SYM(sym);
        while (!s.err && sym != EOB) {
            if (sym == BZ_RUNA || sym == BZ_RUNB) {
                /* A run of the CURRENT front-of-MTF byte, encoded in a
                 * bijective base-2 so that a run length is never
                 * ambiguous. */
                runLen = 0; runBit = 0;
                while (sym == BZ_RUNA || sym == BZ_RUNB) {
                    if (sym == BZ_RUNA) runLen += (1u << runBit);
                    else                runLen += (2u << runBit);
                    runBit++;
                    BZ_NEXT_SYM(sym);
                    if (s.err) break;
                }
                if (s.err) break;
                uint8_t c = mtfv[0];
                if (nblock + runLen > maxblock) { s.err = 1; break; }
                unzftab[c] += runLen;
                for (uint32_t k = 0; k < runLen; k++) block[nblock++] = c;
                continue;
            }
            /* An ordinary MTF index: pull that byte to the front. */
            int j = sym - 1;
            if (j < 0 || j >= nInUse) { s.err = 1; break; }
            uint8_t c = mtfv[j];
            for (int k = j; k > 0; k--) mtfv[k] = mtfv[k - 1];
            mtfv[0] = c;
            if (nblock >= maxblock) { s.err = 1; break; }
            unzftab[c]++;
            block[nblock++] = c;
            BZ_NEXT_SYM(sym);
        }
        free(selectors);
        if (s.err) { *why = "corrupt block data"; goto done; }
        if (origPtr >= nblock) { *why = "origin pointer out of range"; goto done; }

        /* ---- inverse Burrows-Wheeler ----
         * block[] is the last column. cftab gives each byte's first row,
         * and tt[] then links each position to the next in original
         * order. */
        uint32_t cftab[257];
        cftab[0] = 0;
        for (int i = 0; i < 256; i++) cftab[i + 1] = unzftab[i];
        for (int i = 1; i <= 256; i++) cftab[i] += cftab[i - 1];
        for (uint32_t i = 0; i < nblock; i++)
            tt[cftab[block[i]]++] = i;
        uint32_t tPos = tt[origPtr];

        /* ---- and the run-length layer bzip2 applies before the BWT:
         * four equal bytes are followed by a count of extra repeats. ---- */
        uint32_t produced = 0;
        int  runc = 0;
        int  prev = -1;
        while (produced < nblock) {
            uint8_t c = block[tPos];
            tPos = tt[tPos];
            produced++;
            if (runc == 4) {
                /* c is the repeat count for the four bytes just emitted. */
                for (int k = 0; k < (int)c; k++)
                    if (!out_byte(&o, (uint8_t)prev)) { *why = "out of memory"; goto done; }
                runc = 0;
                prev = -1;
                continue;
            }
            if ((int)c == prev) runc++;
            else { runc = 1; prev = (int)c; }
            if (!out_byte(&o, c)) { *why = "out of memory"; goto done; }
        }

        /* The stored CRC is over the block's ORIGINAL bytes, which is
         * exactly what was just emitted. Without this check a corrupted
         * stream that still parses decodes to silent garbage -- and
         * `bzip2 -t` would report it as fine. */
        if (bz_crc32(o.d + block_out_start, o.len - block_out_start) != stored_crc) {
            *why = "data integrity (CRC) error in data";
            goto done;
        }
    }

done:
    free(tt);
    free(block);
    if (rc != 0 || o.err) {
        free(o.d);
        if (o.err) *why = "out of memory";
        return -1;
    }
    *out    = o.d;
    *outlen = o.len;
    return 0;
}
