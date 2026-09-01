/* bzip2_enc.c -- bzip2 compression.
 *
 * The pipeline the format defines, in order: a run-length pass, the
 * Burrows-Wheeler transform, move-to-front, a second run-length pass over
 * the zeros, and Huffman coding. Decompression is in bzip2.c.
 *
 * The BWT sorts every CYCLIC ROTATION of the block. That is done here by
 * prefix doubling with a counting sort -- O(n log n) -- rather than by
 * handing rotations to a comparison sort, which has a quadratic worst case
 * on exactly the long repetitive runs this format is usually aimed at.
 *
 * WHAT THIS ENCODER DOES NOT DO, stated rather than left to be found:
 * real bzip2 builds up to six Huffman tables and iteratively reassigns
 * 50-symbol groups between them to find the cheapest split. This emits ONE
 * table (written twice, because the format requires at least two groups)
 * and points every selector at it. The result is a fully valid bzip2
 * stream that any bunzip2 decodes; it is simply somewhat larger than what
 * bzip2 itself would produce for the same input.
 */

#include <toby/bzip2.h>

#include <stdlib.h>
#include <string.h>

#define BZE_MAX_ALPHA 258
#define BZE_G_SIZE    50
#define BZE_RUNA      0
#define BZE_RUNB      1

/* The CRC lives in bzip2.c and is shared: the value this stamps into each
 * block is the same value the decoder checks, so there must be exactly one
 * implementation of it. */
uint32_t bz_crc32(const uint8_t *p, size_t n);

/* ------------------------------------------------------------ bit writer */

struct bzout {
    uint8_t *d;
    size_t   cap, len;
    uint32_t acc;
    int      nbits;
    int      err;
};

static void bo_byte(struct bzout *o, uint8_t v) {
    if (o->err) return;
    if (o->len == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 65536;
        uint8_t *nd = (uint8_t *)realloc(o->d, nc);
        if (!nd) { o->err = 1; return; }
        o->d = nd; o->cap = nc;
    }
    o->d[o->len++] = v;
}

/* MSB-first, matching the reader in bzip2.c. */
static void bo_bits(struct bzout *o, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        o->acc = (o->acc << 1) | ((v >> i) & 1u);
        if (++o->nbits == 8) {
            bo_byte(o, (uint8_t)(o->acc & 0xFFu));
            o->acc = 0;
            o->nbits = 0;
        }
    }
}

static void bo_flush(struct bzout *o) {
    while (o->nbits) bo_bits(o, 0, 1);
}

/* ------------------------------------------- cyclic-shift sort (the BWT) */

static int bwt_sort(const uint8_t *s, int n, int *sa, int *origPtr) {
    int  alpha = 256;
    int  csz   = (alpha > n ? alpha : n) + 1;
    int *cnt   = (int *)malloc((size_t)csz * sizeof(int));
    int *rank  = (int *)malloc((size_t)n * sizeof(int));
    int *tmp   = (int *)malloc((size_t)n * sizeof(int));
    int *pn    = (int *)malloc((size_t)n * sizeof(int));
    if (!cnt || !rank || !tmp || !pn) {
        free(cnt); free(rank); free(tmp); free(pn);
        return -1;
    }

    memset(cnt, 0, (size_t)alpha * sizeof(int));
    for (int i = 0; i < n; i++) cnt[s[i]]++;
    for (int i = 1; i < alpha; i++) cnt[i] += cnt[i - 1];
    for (int i = n - 1; i >= 0; i--) sa[--cnt[s[i]]] = i;

    int classes = 1;
    rank[sa[0]] = 0;
    for (int i = 1; i < n; i++) {
        if (s[sa[i]] != s[sa[i - 1]]) classes++;
        rank[sa[i]] = classes - 1;
    }

    for (int k = 1; k < n && classes < n; k <<= 1) {
        for (int i = 0; i < n; i++) {
            pn[i] = sa[i] - k;
            if (pn[i] < 0) pn[i] += n;
        }
        memset(cnt, 0, (size_t)classes * sizeof(int));
        for (int i = 0; i < n; i++) cnt[rank[pn[i]]]++;
        for (int i = 1; i < classes; i++) cnt[i] += cnt[i - 1];
        for (int i = n - 1; i >= 0; i--) sa[--cnt[rank[pn[i]]]] = pn[i];

        tmp[sa[0]] = 0;
        int cl = 1;
        for (int i = 1; i < n; i++) {
            int a1 = rank[sa[i]],     a2 = rank[(sa[i] + k) % n];
            int b1 = rank[sa[i - 1]], b2 = rank[(sa[i - 1] + k) % n];
            if (a1 != b1 || a2 != b2) cl++;
            tmp[sa[i]] = cl - 1;
        }
        memcpy(rank, tmp, (size_t)n * sizeof(int));
        classes = cl;
    }

    *origPtr = 0;
    for (int i = 0; i < n; i++) if (sa[i] == 0) { *origPtr = i; break; }
    free(cnt); free(rank); free(tmp); free(pn);
    return 0;
}

/* ------------------------------------------------- Huffman code lengths */

/* Capped at `maxLen` bits. When the tree comes out deeper than the format
 * allows, the frequencies are flattened and it is rebuilt -- the same
 * escape bzip2 itself uses. The alphabet is at most 258 symbols, so
 * repeatedly scanning for the two lightest nodes is cheaper than a heap. */
static void make_lengths(const uint32_t *freq, uint8_t *lenv, int alphaSize,
                         int maxLen)
{
    uint32_t f[BZE_MAX_ALPHA];
    for (int i = 0; i < alphaSize; i++) f[i] = freq[i] ? freq[i] : 1;

    for (;;) {
        static int      parent[2 * BZE_MAX_ALPHA];
        static uint32_t w     [2 * BZE_MAX_ALPHA];
        static int      alive [2 * BZE_MAX_ALPHA];
        int nnodes = alphaSize;
        for (int i = 0; i < alphaSize; i++) {
            w[i] = f[i]; parent[i] = -1; alive[i] = 1;
        }
        for (;;) {
            int a = -1, b = -1;
            for (int i = 0; i < nnodes; i++) {
                if (!alive[i]) continue;
                if (a < 0 || w[i] < w[a])      { b = a; a = i; }
                else if (b < 0 || w[i] < w[b]) { b = i; }
            }
            if (b < 0) break;
            alive[a] = 0; alive[b] = 0;
            w[nnodes]      = w[a] + w[b];
            parent[nnodes] = -1;
            alive[nnodes]  = 1;
            parent[a] = nnodes;
            parent[b] = nnodes;
            nnodes++;
        }

        int over = 0;
        for (int i = 0; i < alphaSize; i++) {
            int d = 0;
            for (int q = i; parent[q] >= 0; q = parent[q]) d++;
            if (d < 1) d = 1;              /* a one-symbol alphabet */
            if (d > maxLen) over = 1;
            lenv[i] = (uint8_t)d;
        }
        if (!over) return;
        for (int i = 0; i < alphaSize; i++) f[i] = (f[i] >> 1) + 1;
    }
}

/* ------------------------------------------------------------- compress */

int bz_compress(const uint8_t *in, size_t inlen, int level,
                uint8_t **out, size_t *outlen, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "ok";
    if (!out || !outlen || (!in && inlen)) { *why = "bad arguments"; return -1; }

    if (level < 1) level = 1;
    if (level > 9) level = 9;
    uint32_t maxblock = (uint32_t)level * 100000u;
    /* Headroom so the run-length pass, which can add a count byte, cannot
     * overrun the block on its last step. */
    uint32_t blocklimit = maxblock - 20;

    struct bzout o;
    memset(&o, 0, sizeof o);
    bo_byte(&o, 'B');
    bo_byte(&o, 'Z');
    bo_byte(&o, 'h');
    bo_byte(&o, (uint8_t)('0' + level));

    uint8_t  *rle  = (uint8_t  *)malloc(maxblock);
    int      *sa   = (int      *)malloc((size_t)maxblock * sizeof(int));
    uint8_t  *L    = (uint8_t  *)malloc(maxblock);
    uint16_t *mtfv = (uint16_t *)malloc((size_t)(maxblock + 16) * sizeof(uint16_t));
    if (!rle || !sa || !L || !mtfv) {
        free(rle); free(sa); free(L); free(mtfv); free(o.d);
        *why = "out of memory";
        return -1;
    }

    uint32_t combined = 0;
    size_t   ipos = 0;
    int      wrote_block = 0;

    while (ipos < inlen) {
        /* ---- 1. run-length pass: four equal bytes, then a count of the
         * extra repeats (0..255). ---- */
        uint32_t nrle = 0;
        size_t   crc_from = ipos;
        while (ipos < inlen && nrle < blocklimit) {
            uint8_t c = in[ipos];
            size_t  run = 1;
            while (ipos + run < inlen && in[ipos + run] == c && run < 255 + 4) run++;
            if (run < 4) {
                for (size_t k = 0; k < run; k++) rle[nrle++] = c;
            } else {
                rle[nrle++] = c; rle[nrle++] = c; rle[nrle++] = c; rle[nrle++] = c;
                rle[nrle++] = (uint8_t)(run - 4);
            }
            ipos += run;
        }
        if (nrle == 0) break;

        /* The block CRC is over the ORIGINAL bytes this block consumed,
         * not over the run-length coded form. */
        uint32_t bcrc = bz_crc32(in + crc_from, ipos - crc_from);
        combined = ((combined << 1) | (combined >> 31)) ^ bcrc;

        /* ---- 2. Burrows-Wheeler ---- */
        int origPtr = 0;
        if (bwt_sort(rle, (int)nrle, sa, &origPtr) != 0) {
            *why = "out of memory";
            goto fail;
        }
        for (uint32_t i = 0; i < nrle; i++)
            L[i] = rle[(sa[i] + (int)nrle - 1) % (int)nrle];

        /* ---- 3. symbol map ---- */
        int inUse[256];
        memset(inUse, 0, sizeof inUse);
        for (uint32_t i = 0; i < nrle; i++) inUse[L[i]] = 1;
        uint8_t unseqToSeq[256];
        int nInUse = 0;
        for (int i = 0; i < 256; i++)
            if (inUse[i]) { unseqToSeq[i] = (uint8_t)nInUse; nInUse++; }
        int alphaSize = nInUse + 2;
        int EOB = alphaSize - 1;

        /* ---- 4. move-to-front, zero runs in bijective base 2 ---- */
        uint8_t mtf[256];
        for (int i = 0; i < nInUse; i++) mtf[i] = (uint8_t)i;
        uint32_t nMTF = 0, zeros = 0;

        for (uint32_t i = 0; i < nrle; i++) {
            uint8_t sym = unseqToSeq[L[i]];
            int j = 0;
            while (mtf[j] != sym) j++;
            for (int k = j; k > 0; k--) mtf[k] = mtf[k - 1];
            mtf[0] = sym;
            if (j == 0) { zeros++; continue; }
            while (zeros) {
                zeros--;
                mtfv[nMTF++] = (uint16_t)((zeros & 1) ? BZE_RUNB : BZE_RUNA);
                zeros >>= 1;
            }
            mtfv[nMTF++] = (uint16_t)(j + 1);
        }
        while (zeros) {
            zeros--;
            mtfv[nMTF++] = (uint16_t)((zeros & 1) ? BZE_RUNB : BZE_RUNA);
            zeros >>= 1;
        }
        mtfv[nMTF++] = (uint16_t)EOB;

        /* ---- 5. one Huffman table ---- */
        uint32_t freq[BZE_MAX_ALPHA];
        memset(freq, 0, sizeof freq);
        for (uint32_t i = 0; i < nMTF; i++) freq[mtfv[i]]++;

        uint8_t  clen[BZE_MAX_ALPHA];
        uint32_t code[BZE_MAX_ALPHA];
        make_lengths(freq, clen, alphaSize, 20);
        {
            /* Canonical codes: all codes of one length are consecutive,
             * and the counter doubles between lengths. */
            uint32_t next = 0;
            for (int b = 1; b <= 20; b++) {
                for (int i = 0; i < alphaSize; i++)
                    if (clen[i] == b) code[i] = next++;
                next <<= 1;
            }
        }

        int nSelectors = (int)((nMTF + BZE_G_SIZE - 1) / BZE_G_SIZE);
        if (nSelectors < 1) nSelectors = 1;

        /* ---- 6. write the block ---- */
        bo_bits(&o, 0x314159u, 24);
        bo_bits(&o, 0x265359u, 24);
        bo_bits(&o, bcrc, 32);
        bo_bits(&o, 0, 1);                        /* not randomised */
        bo_bits(&o, (uint32_t)origPtr, 24);

        uint32_t used16 = 0;
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                if (inUse[i * 16 + j]) { used16 |= (1u << (15 - i)); break; }
        bo_bits(&o, used16, 16);
        for (int i = 0; i < 16; i++) {
            if (!(used16 & (1u << (15 - i)))) continue;
            uint32_t bits = 0;
            for (int j = 0; j < 16; j++)
                if (inUse[i * 16 + j]) bits |= (1u << (15 - j));
            bo_bits(&o, bits, 16);
        }

        bo_bits(&o, 2, 3);                        /* nGroups: the minimum */
        bo_bits(&o, (uint32_t)nSelectors, 15);
        /* Every selector picks group 0. After the selector MTF a repeated
         * 0 is a single zero bit. */
        for (int i = 0; i < nSelectors; i++) bo_bits(&o, 0, 1);

        /* Both groups carry the same table; the lengths are delta coded,
         * "10" to step up and "11" to step down, then a 0 to stop. */
        for (int t = 0; t < 2; t++) {
            int curr = clen[0];
            bo_bits(&o, (uint32_t)curr, 5);
            for (int i = 0; i < alphaSize; i++) {
                while (curr < clen[i]) { bo_bits(&o, 2, 2); curr++; }
                while (curr > clen[i]) { bo_bits(&o, 3, 2); curr--; }
                bo_bits(&o, 0, 1);
            }
        }

        for (uint32_t i = 0; i < nMTF; i++)
            bo_bits(&o, code[mtfv[i]], clen[mtfv[i]]);

        if (o.err) { *why = "out of memory"; goto fail; }
        wrote_block = 1;
    }

    /* An empty input is still a valid stream: header, end marker, CRC 0.
     * bzip2 produces exactly that, and refusing it here would make
     * `bzip2 < /dev/null` an error where it is not one. */
    (void)wrote_block;

    bo_bits(&o, 0x177245u, 24);
    bo_bits(&o, 0x385090u, 24);
    bo_bits(&o, combined, 32);
    bo_flush(&o);

    free(rle); free(sa); free(L); free(mtfv);
    if (o.err) { free(o.d); *why = "out of memory"; return -1; }
    *out    = o.d;
    *outlen = o.len;
    return 0;

fail:
    free(rle); free(sa); free(L); free(mtfv); free(o.d);
    return -1;
}
