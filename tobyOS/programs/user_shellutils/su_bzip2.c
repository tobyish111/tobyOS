/* su_bzip2.c -- bzip2, bunzip2 and bzcat, as bzip2 1.0.8's
 * (logs/utilgate/staging/bzip2.sh, bunzip2.sh, bzcat.sh).
 *
 * Compressing: bzip2's own bytes at every block size -- measured on 417
 * inputs, multi-block ones included. The block is the run-length coding
 * of runs of 4..255 (a run in progress goes on into the next block when
 * one fills; only the last block takes it at the end); the Burrows-
 * Wheeler sort is a cyclic suffix sort by doubling, with the one thing a
 * sort may choose measured: the block's own rotation is last among the
 * rotations equal to it. Then move-to-front with RUNA/RUNB, and the
 * Huffman tables as bzip2 picks them: 2 to 6 tables by the symbol count,
 * first cut into runs of about equal frequency, four passes of choosing
 * a table per 50 symbols and remaking the code lengths (at most 17 bits,
 * ties to the shallower tree, frequencies halved when too deep).
 *
 * The verbose lines are bzip2's at -v to -vvv, and at -vvvv for blocks
 * under 10000 bytes (its fallback sort's statistics; bigger blocks' are
 * its main sort's, which this sort does not share: they are not printed,
 * nor the "N work" line of -vvv). The ratio line's doubles are computed
 * without the FPU (su_czcommon.c).
 *
 * Decompressing: multiple streams, trailing garbage, -f's pass-through
 * (the input again from its start, as bzip2 rewinds), output written in
 * bzip2's 5000-byte pieces (what a CRC failure loses), and its messages.
 * Randomised blocks (written only by bzip2 0.9.0) are not read. */

#include "su_czcommon.c"

/* ======================================================================
 *  the bzip2 format: a 32-bit CRC sent most significant bit first
 * ====================================================================== */
static uint32_t cz_b_crctab[256];
static int cz_b_crcready;

static void cz_b_crcinit(void) {
    if (cz_b_crcready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
        cz_b_crctab[i] = c;
    }
    cz_b_crcready = 1;
}

#define CZ_B_CRC(crc, b) ((crc) = ((crc) << 8) ^ cz_b_crctab[((crc) >> 24) ^ (uint8_t)(b)])

/* ======================================================================
 *  compressing
 * ====================================================================== */
#define CZ_B_MAXGROUPS 6
#define CZ_B_GSIZE 50
#define CZ_B_MAXALPHA 258
#define CZ_B_MAXSEL 18002

struct cz_b_enc {
    int level;
    int nblock_max;
    uint8_t *block;           /* the run-length coded block */
    int nblock;
    uint32_t bcrc, combined;
    int run_ch, run_len;      /* the run being gathered (run_ch -1: none) */
    uint8_t in_use[256];
    int nblocks_done;
    /* the sort */
    int32_t *sa, *rank, *tmp, *cnt;
    /* the coding */
    uint16_t *mtfv;
    int nmtf;
    int32_t mtf_freq[CZ_B_MAXALPHA];
    uint8_t len[CZ_B_MAXGROUPS][CZ_B_MAXALPHA];
    int32_t code[CZ_B_MAXGROUPS][CZ_B_MAXALPHA];
    int32_t rfreq[CZ_B_MAXGROUPS][CZ_B_MAXALPHA];
    uint8_t selector[CZ_B_MAXSEL];
    uint8_t selmtf[CZ_B_MAXSEL];
    /* output */
    void (*wr)(void *ctx, const uint8_t *p, size_t n);
    void *wctx;
    uint8_t obuf[8192];
    size_t on;
    uint32_t bitbuf;
    int bitcnt;
    uint64_t bytes_out;
    int verbose;              /* -vv: each block's numbers on stderr */
};

static void cz_b_oflush(struct cz_b_enc *e) {
    if (e->on) e->wr(e->wctx, e->obuf, e->on);
    e->on = 0;
}

static void cz_b_obyte(struct cz_b_enc *e, unsigned b) {
    if (e->on == sizeof e->obuf) cz_b_oflush(e);
    e->obuf[e->on++] = (uint8_t)b;
    e->bytes_out++;
}

static void cz_b_bits(struct cz_b_enc *e, int n, uint32_t v) {
    while (n > 0) {
        int k = n > 8 ? 8 : n;
        n -= k;
        e->bitbuf = e->bitbuf << k | ((v >> n) & ((1u << k) - 1));
        e->bitcnt += k;
        while (e->bitcnt >= 8) {
            e->bitcnt -= 8;
            cz_b_obyte(e, (e->bitbuf >> e->bitcnt) & 0xff);
        }
    }
}

/* the bytes bzip2's writer would have counted after a write of N bits
 * ending at the current position: it passes whole bytes on only at the
 * start of its next write */
static uint64_t cz_b_lazy_bytes(struct cz_b_enc *e, int n) {
    uint64_t t = e->bytes_out * 8 + (uint64_t)e->bitcnt;
    return (t - (uint64_t)n) / 8;
}

static void cz_b_bits_flush(struct cz_b_enc *e) {
    if (e->bitcnt > 0) cz_b_obyte(e, (e->bitbuf << (8 - e->bitcnt)) & 0xff);
    e->bitcnt = 0;
    e->bitbuf = 0;
}

/* ---- the run of equal bytes going into the block: one to four as they
 * are, four and a count of 0..251 more ---- */
static void cz_b_put_run(struct cz_b_enc *e) {
    int ch = e->run_ch, n = e->run_len;
    if (ch < 0 || n == 0) return;
    for (int i = 0; i < n; i++) CZ_B_CRC(e->bcrc, ch);
    e->in_use[ch] = 1;
    if (n <= 3) {
        for (int i = 0; i < n; i++) e->block[e->nblock++] = (uint8_t)ch;
    } else {
        for (int i = 0; i < 4; i++) e->block[e->nblock++] = (uint8_t)ch;
        e->block[e->nblock++] = (uint8_t)(n - 4);
        e->in_use[n - 4] = 1;
    }
    e->run_ch = -1;
    e->run_len = 0;
}

/* ---- the Burrows-Wheeler sort: rotations ranked by doubling prefixes.
 * A rotation equal to the first one is ranked before it: measured, the
 * block's own rotation is last among those equal to it. ---- */
static int cz_b_sort(struct cz_b_enc *e) {
    int n = e->nblock;
    int32_t *sa = e->sa, *rk = e->rank, *tmp = e->tmp, *cnt = e->cnt;
    int m = 256;
    for (int i = 0; i < m; i++) cnt[i] = 0;
    for (int i = 0; i < n; i++) cnt[e->block[i]]++;
    for (int i = 1; i < m; i++) cnt[i] += cnt[i - 1];
    for (int i = n - 1; i >= 0; i--) sa[--cnt[e->block[i]]] = i;
    /* dense ranks of the first byte */
    int r = 0;
    rk[sa[0]] = 0;
    for (int i = 1; i < n; i++) {
        if (e->block[sa[i]] != e->block[sa[i - 1]]) r++;
        rk[sa[i]] = r;
    }
    /* -vvvv: bzip2's fallback sort (blocks under 10000) reports how many
     * rotations are not yet told apart at each depth */
    int stats = e->verbose >= 4 && n < 10000;
    if (stats) fprintf(stderr, "        bucket sorting ...\n");
    if (stats) {
        int k = 1;
        for (;;) {
            int unres = 0;
            for (int i = 0; i < n;) {
                int j = i + 1;
                while (j < n && rk[sa[j]] == rk[sa[i]]) j++;
                if (j - i > 1) unres += j - i;
                i = j;
            }
            fprintf(stderr, "        depth %6d has %6d unresolved strings\n", k, unres);
            if (unres == 0 || 2 * k > n) break;
            for (int i = 0; i < n; i++) {
                int j = sa[i] - k;
                tmp[i] = j < 0 ? j + n : j;
            }
            m = r + 1;
            for (int i = 0; i < m; i++) cnt[i] = 0;
            for (int i = 0; i < n; i++) cnt[rk[tmp[i]]]++;
            for (int i = 1; i < m; i++) cnt[i] += cnt[i - 1];
            for (int i = n - 1; i >= 0; i--) sa[--cnt[rk[tmp[i]]]] = tmp[i];
            tmp[sa[0]] = 0;
            r = 0;
            for (int i = 1; i < n; i++) {
                int a = sa[i], b = sa[i - 1];
                int a2 = a + k >= n ? a + k - n : a + k, b2 = b + k >= n ? b + k - n : b + k;
                if (rk[a] != rk[b] || rk[a2] != rk[b2]) r++;
                tmp[a] = r;
            }
            int32_t *t = rk; rk = tmp; tmp = t;
            k <<= 1;
        }
        fprintf(stderr, "        reconstructing block ...\n");
    }
    for (int k = 1; r < n - 1 && k < n; k <<= 1) {
        /* ordered by the second half: the sorted order, shifted back */
        for (int i = 0; i < n; i++) {
            int j = sa[i] - k;
            tmp[i] = j < 0 ? j + n : j;
        }
        m = r + 1;
        for (int i = 0; i < m; i++) cnt[i] = 0;
        for (int i = 0; i < n; i++) cnt[rk[tmp[i]]]++;
        for (int i = 1; i < m; i++) cnt[i] += cnt[i - 1];
        for (int i = n - 1; i >= 0; i--) sa[--cnt[rk[tmp[i]]]] = tmp[i];
        /* new ranks: pairs (rank[i], rank[i+k]) */
        tmp[sa[0]] = 0;
        r = 0;
        for (int i = 1; i < n; i++) {
            int a = sa[i], b = sa[i - 1];
            int a2 = a + k >= n ? a + k - n : a + k, b2 = b + k >= n ? b + k - n : b + k;
            if (rk[a] != rk[b] || rk[a2] != rk[b2]) r++;
            tmp[a] = r;
        }
        int32_t *t = rk; rk = tmp; tmp = t;
    }
    /* rotation 0 goes last among its equals (equal rotations have equal
     * last bytes, so only where the block's own one stands matters) */
    int r0 = rk[0], last = -1, pos0 = -1;
    for (int i = 0; i < n; i++) {
        if (rk[sa[i]] == r0) last = i;
        if (sa[i] == 0) pos0 = i;
    }
    if (last != pos0) { sa[pos0] = sa[last]; sa[last] = 0; }
    e->rank = rk;
    e->tmp = tmp;
    return last;
}

/* ---- code lengths of at most MAXLEN bits for FREQ: a Huffman tree with
 * ties broken towards the shallower subtree; too deep, and the
 * frequencies are halved (plus one) and the tree made again ---- */
static void cz_b_code_lengths(uint8_t *len, const int32_t *freq, int alpha, int maxlen) {
    int32_t heap[CZ_B_MAXALPHA + 2];
    int32_t weight[CZ_B_MAXALPHA * 2];
    int32_t parent[CZ_B_MAXALPHA * 2];
    for (int i = 0; i < alpha; i++) weight[i + 1] = (freq[i] == 0 ? 1 : freq[i]) << 8;
    for (;;) {
        int nnodes = alpha, nheap = 0;
        heap[0] = 0;
        weight[0] = 0;
        parent[0] = -2;
        for (int i = 1; i <= alpha; i++) {
            parent[i] = -1;
            heap[++nheap] = i;
            int z = nheap, tmp = heap[z];
            while (weight[tmp] < weight[heap[z >> 1]]) { heap[z] = heap[z >> 1]; z >>= 1; }
            heap[z] = tmp;
        }
        while (nheap > 1) {
            int pick[2];
            for (int p = 0; p < 2; p++) {
                pick[p] = heap[1];
                heap[1] = heap[nheap--];
                int z = 1, tmp = heap[z];
                for (;;) {
                    int y = z << 1;
                    if (y > nheap) break;
                    if (y < nheap && weight[heap[y + 1]] < weight[heap[y]]) y++;
                    if (weight[tmp] < weight[heap[y]]) break;
                    heap[z] = heap[y];
                    z = y;
                }
                heap[z] = tmp;
            }
            nnodes++;
            parent[pick[0]] = parent[pick[1]] = nnodes;
            int32_t w1 = weight[pick[0]], w2 = weight[pick[1]];
            int d1 = w1 & 0xff, d2 = w2 & 0xff;
            weight[nnodes] = (int32_t)(((w1 & 0xffffff00) + (w2 & 0xffffff00)) | (1 + (d1 > d2 ? d1 : d2)));
            parent[nnodes] = -1;
            heap[++nheap] = nnodes;
            int z = nheap, tmp = heap[z];
            while (weight[tmp] < weight[heap[z >> 1]]) { heap[z] = heap[z >> 1]; z >>= 1; }
            heap[z] = tmp;
        }
        int toolong = 0;
        for (int i = 1; i <= alpha; i++) {
            int j = 0, k = i;
            while (parent[k] >= 0) { k = parent[k]; j++; }
            len[i - 1] = (uint8_t)j;
            if (j > maxlen) toolong = 1;
        }
        if (!toolong) break;
        for (int i = 1; i <= alpha; i++) {
            int j = weight[i] >> 8;
            j = 1 + j / 2;
            weight[i] = j << 8;
        }
    }
}

static void cz_b_mtf(struct cz_b_enc *e) {
    uint8_t seq[256];
    int ninuse = 0;
    for (int i = 0; i < 256; i++) if (e->in_use[i]) seq[i] = (uint8_t)ninuse++;
    int eob = ninuse + 1;
    for (int i = 0; i <= eob; i++) e->mtf_freq[i] = 0;
    uint8_t list[256];
    for (int i = 0; i < ninuse; i++) list[i] = (uint8_t)i;
    int wr = 0, zpend = 0, n = e->nblock;
    for (int i = 0; i < n; i++) {
        int j = e->sa[i] - 1;
        if (j < 0) j += n;
        uint8_t ll = seq[e->block[j]];
        if (list[0] == ll) {
            zpend++;
            continue;
        }
        if (zpend > 0) {
            zpend--;
            for (;;) {
                int s = (zpend & 1) ? 1 : 0;   /* RUNB : RUNA */
                e->mtfv[wr++] = (uint16_t)s;
                e->mtf_freq[s]++;
                if (zpend < 2) break;
                zpend = (zpend - 2) / 2;
            }
            zpend = 0;
        }
        int k = 1;
        uint8_t prev = list[0];
        while (list[k] != ll) {
            uint8_t t = list[k];
            list[k] = prev;
            prev = t;
            k++;
        }
        list[k] = prev;
        list[0] = ll;
        e->mtfv[wr++] = (uint16_t)(k + 1);
        e->mtf_freq[k + 1]++;
    }
    if (zpend > 0) {
        zpend--;
        for (;;) {
            int s = (zpend & 1) ? 1 : 0;
            e->mtfv[wr++] = (uint16_t)s;
            e->mtf_freq[s]++;
            if (zpend < 2) break;
            zpend = (zpend - 2) / 2;
        }
    }
    e->mtfv[wr++] = (uint16_t)eob;
    e->mtf_freq[eob]++;
    e->nmtf = wr;
}

static void cz_b_send_mtf(struct cz_b_enc *e) {
    int ninuse = 0;
    for (int i = 0; i < 256; i++) ninuse += e->in_use[i];
    int alpha = ninuse + 2;
    int nmtf = e->nmtf;
    int ngroups = nmtf < 200 ? 2 : nmtf < 600 ? 3 : nmtf < 1200 ? 4 : nmtf < 2400 ? 5 : 6;
    if (e->verbose >= 3)
        fprintf(stderr, "      %d in block, %d after MTF & 1-2 coding, %d+2 syms in use\n",
                e->nblock, nmtf, ninuse);
    for (int t = 0; t < ngroups; t++)
        for (int v = 0; v < alpha; v++) e->len[t][v] = 15;
    /* the first tables: the symbols cut into ngroups runs of about equal
     * frequency, each table cheap on its own run */
    {
        int npart = ngroups, remf = nmtf, gs = 0;
        while (npart > 0) {
            int tfreq = remf / npart, ge = gs - 1, afreq = 0;
            while (afreq < tfreq && ge < alpha - 1) {
                ge++;
                afreq += e->mtf_freq[ge];
            }
            if (ge > gs && npart != ngroups && npart != 1 && ((ngroups - npart) % 2 == 1)) {
                afreq -= e->mtf_freq[ge];
                ge--;
            }
            if (e->verbose >= 3) {
                char pc[32];
                cz_d_fmt(pc, sizeof pc, cz_d_ratio(100 * (uint64_t)afreq, (uint64_t)nmtf), 4, 1);
                fprintf(stderr, "      initial group %d, [%d .. %d], has %d syms (%s%%)\n",
                        npart, gs, ge, afreq, pc);
            }
            for (int v = 0; v < alpha; v++) e->len[npart - 1][v] = (uint8_t)((v >= gs && v <= ge) ? 0 : 15);
            npart--;
            gs = ge + 1;
            remf -= afreq;
        }
    }
    int nsel = 0;
    for (int iter = 0; iter < 4; iter++) {
        for (int t = 0; t < ngroups; t++)
            for (int v = 0; v < alpha; v++) e->rfreq[t][v] = 0;
        nsel = 0;
        int totc = 0, fave[CZ_B_MAXGROUPS] = {0};
        for (int gs = 0; gs < nmtf; gs += CZ_B_GSIZE) {
            int ge = gs + CZ_B_GSIZE - 1;
            if (ge >= nmtf) ge = nmtf - 1;
            int cost[CZ_B_MAXGROUPS] = {0};
            for (int i = gs; i <= ge; i++)
                for (int t = 0; t < ngroups; t++) cost[t] += e->len[t][e->mtfv[i]];
            int bc = 999999999, bt = -1;
            for (int t = 0; t < ngroups; t++)
                if (cost[t] < bc) { bc = cost[t]; bt = t; }
            totc += bc;
            fave[bt]++;
            e->selector[nsel++] = (uint8_t)bt;
            for (int i = gs; i <= ge; i++) e->rfreq[bt][e->mtfv[i]]++;
        }
        if (e->verbose >= 3) {
            fprintf(stderr, "      pass %d: size is %d, grp uses are ", iter + 1, totc / 8);
            for (int t = 0; t < ngroups; t++) fprintf(stderr, "%d ", fave[t]);
            fprintf(stderr, "\n");
        }
        for (int t = 0; t < ngroups; t++) cz_b_code_lengths(e->len[t], e->rfreq[t], alpha, 17);
    }
    /* the selectors, move-to-front coded */
    {
        uint8_t pos[CZ_B_MAXGROUPS];
        for (int i = 0; i < ngroups; i++) pos[i] = (uint8_t)i;
        for (int i = 0; i < nsel; i++) {
            uint8_t v = e->selector[i];
            int j = 0;
            uint8_t tmp = pos[0];
            while (v != tmp) {
                j++;
                uint8_t t2 = tmp;
                tmp = pos[j];
                pos[j] = t2;
            }
            pos[0] = tmp;
            e->selmtf[i] = (uint8_t)j;
        }
    }
    /* canonical codes, by length, then by symbol */
    for (int t = 0; t < ngroups; t++) {
        int minl = 32, maxl = 0;
        for (int v = 0; v < alpha; v++) {
            if (e->len[t][v] > maxl) maxl = e->len[t][v];
            if (e->len[t][v] < minl) minl = e->len[t][v];
        }
        int vec = 0;
        for (int n = minl; n <= maxl; n++) {
            for (int v = 0; v < alpha; v++)
                if (e->len[t][v] == n) e->code[t][v] = vec++;
            vec <<= 1;
        }
    }
    uint64_t z0 = cz_b_lazy_bytes(e, 24), z1, z2, z3, z4;
    /* the byte map */
    {
        int used16 = 0;
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                if (e->in_use[i * 16 + j]) { used16 |= 0x8000 >> i; break; }
        cz_b_bits(e, 16, (uint32_t)used16);
        for (int i = 0; i < 16; i++) {
            if (!(used16 & (0x8000 >> i))) continue;
            int m = 0;
            for (int j = 0; j < 16; j++) if (e->in_use[i * 16 + j]) m |= 0x8000 >> j;
            cz_b_bits(e, 16, (uint32_t)m);
        }
    }
    z1 = cz_b_lazy_bytes(e, 1);
    cz_b_bits(e, 3, (uint32_t)ngroups);
    cz_b_bits(e, 15, (uint32_t)nsel);
    for (int i = 0; i < nsel; i++) {
        for (int j = 0; j < e->selmtf[i]; j++) cz_b_bits(e, 1, 1);
        cz_b_bits(e, 1, 0);
    }
    z2 = cz_b_lazy_bytes(e, 1);
    for (int t = 0; t < ngroups; t++) {
        int curr = e->len[t][0];
        cz_b_bits(e, 5, (uint32_t)curr);
        for (int v = 0; v < alpha; v++) {
            while (curr < e->len[t][v]) { cz_b_bits(e, 2, 2); curr++; }
            while (curr > e->len[t][v]) { cz_b_bits(e, 2, 3); curr--; }
            cz_b_bits(e, 1, 0);
        }
    }
    z3 = cz_b_lazy_bytes(e, 1);
    int sel = 0, lastlen = 0;
    for (int gs = 0; gs < nmtf; gs += CZ_B_GSIZE, sel++) {
        int ge = gs + CZ_B_GSIZE - 1;
        if (ge >= nmtf) ge = nmtf - 1;
        int t = e->selector[sel];
        for (int i = gs; i <= ge; i++) {
            int s = e->mtfv[i];
            cz_b_bits(e, e->len[t][s], (uint32_t)e->code[t][s]);
            lastlen = e->len[t][s];
        }
    }
    z4 = cz_b_lazy_bytes(e, lastlen);
    if (e->verbose >= 3)
        fprintf(stderr, "      bytes: mapping %d, selectors %d, code lengths %d, codes %d\n",
                (int)(z1 - z0), (int)(z2 - z1), (int)(z3 - z2), (int)(z4 - z3));
}

static void cz_b_block(struct cz_b_enc *e, int last) {
    if (e->nblocks_done == 0) {
        cz_b_obyte(e, 'B');
        cz_b_obyte(e, 'Z');
        cz_b_obyte(e, 'h');
        cz_b_obyte(e, (unsigned)('0' + e->level));
    }
    if (e->nblock > 0) {
        uint32_t bcrc = ~e->bcrc;
        e->combined = (e->combined << 1) | (e->combined >> 31);
        e->combined ^= bcrc;
        if (e->verbose >= 2)
            fprintf(stderr, "    block %d: crc = 0x%08x, combined CRC = 0x%08x, size = %d\n",
                    e->nblocks_done + 1, bcrc, e->combined, e->nblock);
        int origptr = cz_b_sort(e);
        cz_b_bits(e, 8, 0x31); cz_b_bits(e, 8, 0x41); cz_b_bits(e, 8, 0x59);
        cz_b_bits(e, 8, 0x26); cz_b_bits(e, 8, 0x53); cz_b_bits(e, 8, 0x59);
        cz_b_bits(e, 32, bcrc);
        cz_b_bits(e, 1, 0);
        cz_b_bits(e, 24, (uint32_t)origptr);
        cz_b_mtf(e);
        cz_b_send_mtf(e);
        e->nblocks_done++;
    } else if (e->nblocks_done == 0) {
        e->nblocks_done = 1;
    }
    if (last) {
        cz_b_bits(e, 8, 0x17); cz_b_bits(e, 8, 0x72); cz_b_bits(e, 8, 0x45);
        cz_b_bits(e, 8, 0x38); cz_b_bits(e, 8, 0x50); cz_b_bits(e, 8, 0x90);
        cz_b_bits(e, 32, e->combined);
        cz_b_bits_flush(e);
        cz_b_oflush(e);
    }
    e->nblock = 0;
    e->bcrc = 0xffffffffu;
    memset(e->in_use, 0, sizeof e->in_use);
}

static int cz_b_enc_init(struct cz_b_enc *e, int level) {
    cz_b_crcinit();
    e->level = level;
    e->nblock_max = 100000 * level - 19;
    size_t cap = (size_t)100000 * (size_t)level + 64;
    e->block = (uint8_t *)malloc(cap);
    e->sa = (int32_t *)malloc(cap * sizeof(int32_t));
    e->rank = (int32_t *)malloc(cap * sizeof(int32_t));
    e->tmp = (int32_t *)malloc(cap * sizeof(int32_t));
    e->cnt = (int32_t *)malloc((cap > 256 ? cap : 256) * sizeof(int32_t));
    e->mtfv = (uint16_t *)malloc((cap + 8) * sizeof(uint16_t));
    if (!e->block || !e->sa || !e->rank || !e->tmp || !e->cnt || !e->mtfv) return -1;
    e->nblock = 0;
    e->bcrc = 0xffffffffu;
    e->combined = 0;
    e->run_ch = -1;
    e->run_len = 0;
    e->nblocks_done = 0;
    memset(e->in_use, 0, sizeof e->in_use);
    e->on = 0;
    e->bitbuf = 0;
    e->bitcnt = 0;
    e->bytes_out = 0;
    return 0;
}

static void cz_b_enc_free(struct cz_b_enc *e) {
    free(e->block); free(e->sa); free(e->rank); free(e->tmp); free(e->cnt); free(e->mtfv);
}

/* the input, a piece at a time; a block is made as soon as it is full --
 * the run being gathered goes on into the next block, as with bzip2:
 * only the last block takes it */
static void cz_b_enc_feed(struct cz_b_enc *e, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int c = p[i];
        if (c == e->run_ch && e->run_len < 255) {
            e->run_len++;
        } else {
            cz_b_put_run(e);
            e->run_ch = c;
            e->run_len = 1;
        }
        if (e->nblock >= e->nblock_max) cz_b_block(e, 0);
    }
}

static void cz_b_enc_finish(struct cz_b_enc *e) {
    cz_b_put_run(e);
    cz_b_block(e, 1);
}

/* ======================================================================
 *  decompressing
 * ====================================================================== */
#define CZ_B_D_OK 0           /* the stream ended as it should */
#define CZ_B_D_MAGIC 1        /* not a bzip2 stream at all */
#define CZ_B_D_DATA 2         /* a corrupt stream (or a CRC mismatch) */
#define CZ_B_D_EOF 3          /* the input ended inside the stream */

struct cz_b_dec {
    struct cz_in *in;
    uint64_t bitbuf;
    int bitcnt;
    int eof;
    int level;
    uint32_t *tt;
    int ttcap;
    void (*out)(void *ctx, const uint8_t *p, size_t n);
    void *octx;
    int verbosity;
    uint64_t total_out;
};

/* N bits, most significant first; -1 when the input ends */
static int64_t cz_b_dbits(struct cz_b_dec *d, int n) {
    while (d->bitcnt < n) {
        int c = cz_getc(d->in);
        if (c < 0) { d->eof = 1; return -1; }
        d->bitbuf = d->bitbuf << 8 | (uint32_t)c;
        d->bitcnt += 8;
    }
    d->bitcnt -= n;
    return (int64_t)((d->bitbuf >> d->bitcnt) & ((1ULL << n) - 1));
}

#define CZ_B_NEED(v, n) do { int64_t t_ = cz_b_dbits(d, (n)); if (t_ < 0) return CZ_B_D_EOF; (v) = (uint32_t)t_; } while (0)

static void cz_b_vmsg(struct cz_b_dec *d, int level, const char *s) {
    if (d->verbosity >= level) fputs(s, stderr);
}

/* one block after its magic; its output through d->out */
static int cz_b_dblock(struct cz_b_dec *d, int blockno, uint32_t *combined) {
    uint32_t v, bcrc, origptr;
    if (d->verbosity >= 2) fprintf(stderr, "\n    [%d: huff+mtf ", blockno);
    CZ_B_NEED(bcrc, 32);
    CZ_B_NEED(v, 1);
    int randomised = (int)v;
    CZ_B_NEED(origptr, 24);
    int nblock_max = 100000 * d->level;
    if ((int)origptr > 10 + nblock_max) return CZ_B_D_DATA;
    uint32_t used16;
    CZ_B_NEED(used16, 16);
    uint8_t seq_to_unseq[256];
    int ninuse = 0;
    for (int i = 0; i < 16; i++) {
        if (!(used16 & (0x8000u >> i))) continue;
        uint32_t m;
        CZ_B_NEED(m, 16);
        for (int j = 0; j < 16; j++)
            if (m & (0x8000u >> j)) seq_to_unseq[ninuse++] = (uint8_t)(i * 16 + j);
    }
    if (ninuse == 0) return CZ_B_D_DATA;
    int alpha = ninuse + 2;
    uint32_t ngroups, nsel;
    CZ_B_NEED(ngroups, 3);
    if (ngroups < 2 || ngroups > 6) return CZ_B_D_DATA;
    CZ_B_NEED(nsel, 15);
    if (nsel < 1) return CZ_B_D_DATA;
    static uint8_t selmtf[32768], sel[32768];
    for (uint32_t i = 0; i < nsel; i++) {
        uint32_t j = 0;
        for (;;) {
            CZ_B_NEED(v, 1);
            if (!v) break;
            j++;
            if (j >= ngroups) return CZ_B_D_DATA;
        }
        if (i < CZ_B_MAXSEL) selmtf[i] = (uint8_t)j;
    }
    if (nsel > CZ_B_MAXSEL) nsel = CZ_B_MAXSEL;
    {
        uint8_t pos[6];
        for (uint32_t i = 0; i < ngroups; i++) pos[i] = (uint8_t)i;
        for (uint32_t i = 0; i < nsel; i++) {
            int j = selmtf[i];
            uint8_t t = pos[j];
            while (j > 0) { pos[j] = pos[j - 1]; j--; }
            pos[0] = t;
            sel[i] = t;
        }
    }
    uint8_t len[6][CZ_B_MAXALPHA];
    for (uint32_t t = 0; t < ngroups; t++) {
        uint32_t curr;
        CZ_B_NEED(curr, 5);
        for (int i = 0; i < alpha; i++) {
            for (;;) {
                if (curr < 1 || curr > 20) return CZ_B_D_DATA;
                CZ_B_NEED(v, 1);
                if (!v) break;
                CZ_B_NEED(v, 1);
                if (!v) curr++; else curr--;
            }
            len[t][i] = (uint8_t)curr;
        }
    }
    /* the decoding tables: canonical codes, limit/base per length */
    int32_t limit[6][22], base[6][22], perm[6][CZ_B_MAXALPHA], minlen[6];
    for (uint32_t t = 0; t < ngroups; t++) {
        int mn = 32, mx = 0;
        for (int i = 0; i < alpha; i++) {
            if (len[t][i] > mx) mx = len[t][i];
            if (len[t][i] < mn) mn = len[t][i];
        }
        int pp = 0;
        for (int l = mn; l <= mx; l++)
            for (int i = 0; i < alpha; i++) if (len[t][i] == l) perm[t][pp++] = i;
        int cnt[23] = {0};
        for (int i = 0; i < alpha; i++) cnt[len[t][i] + 1]++;
        for (int i = 1; i < 23; i++) cnt[i] += cnt[i - 1];
        for (int l = 0; l < 22; l++) limit[t][l] = -1;
        /* a code of l bits is at perm[code + base[l]]: the codes before
         * it, less the first code of its length */
        {
            int code = 0, idx = 0;
            for (int l = mn; l <= mx; l++) {
                int nl = cnt[l + 1] - cnt[l];
                base[t][l] = idx - code;     /* index = code + base */
                code += nl;
                idx += nl;
                limit[t][l] = code - 1;
                code <<= 1;
            }
        }
        minlen[t] = mn;
    }
    /* the symbols: move-to-front, the runs of the front byte */
    int32_t unzftab[256];
    for (int i = 0; i < 256; i++) unzftab[i] = 0;
    uint8_t mtf[256];
    for (int i = 0; i < 256; i++) mtf[i] = (uint8_t)i;
    if (d->ttcap < nblock_max) {
        free(d->tt);
        d->tt = (uint32_t *)malloc((size_t)nblock_max * sizeof(uint32_t));
        if (!d->tt) { fprintf(stderr, "bzip2: memory exhausted\n"); exit(1); }
        d->ttcap = nblock_max;
    }
    uint32_t *tt = d->tt;
    int nblock = 0, eob = ninuse + 1;
    int group = -1, gpos = 0, es = 0, en = 1;
    for (;;) {
        if (gpos == 0) {
            group++;
            if ((uint32_t)group >= nsel) return CZ_B_D_DATA;
            gpos = CZ_B_GSIZE;
        }
        gpos--;
        int t = sel[group];
        int zn = minlen[t];
        uint32_t zvec;
        CZ_B_NEED(zvec, zn);
        for (;;) {
            if (zn > 20) return CZ_B_D_DATA;
            if ((int32_t)zvec <= limit[t][zn]) break;
            zn++;
            CZ_B_NEED(v, 1);
            zvec = zvec << 1 | v;
        }
        int idx = (int)zvec + base[t][zn];
        if (idx < 0 || idx >= alpha) return CZ_B_D_DATA;
        int sym = perm[t][idx];
        if (sym <= 1) {           /* RUNA, RUNB: a bijective base-2 count */
            if (en >= 2 * 1024 * 1024) return CZ_B_D_DATA;
            es += (sym + 1) * en;
            en <<= 1;
            continue;
        }
        if (es > 0) {
            uint8_t uc = seq_to_unseq[mtf[0]];
            unzftab[uc] += es;
            while (es > 0) {
                if (nblock >= nblock_max) return CZ_B_D_DATA;
                tt[nblock++] = uc;
                es--;
            }
            en = 1;
        }
        if (sym == eob) break;
        if (nblock >= nblock_max) return CZ_B_D_DATA;
        int nn = sym - 1;
        uint8_t uv = mtf[nn];
        while (nn > 0) { mtf[nn] = mtf[nn - 1]; nn--; }
        mtf[0] = uv;
        uint8_t uc = seq_to_unseq[uv];
        unzftab[uc]++;
        tt[nblock++] = uc;
    }
    if ((int)origptr >= nblock) return CZ_B_D_DATA;
    cz_b_vmsg(d, 2, "rt+rld");
    if (randomised) return CZ_B_D_DATA;   /* bzip2 0.9.0's randomised blocks: not read here */
    /* the inverse transform: each position's successor */
    int32_t cftab[257];
    cftab[0] = 0;
    for (int i = 1; i <= 256; i++) cftab[i] = unzftab[i - 1] + cftab[i - 1];
    for (int i = 0; i < nblock; i++) {
        uint8_t uc = (uint8_t)(tt[i] & 0xff);
        tt[cftab[uc]] |= (uint32_t)i << 8;
        cftab[uc]++;
    }
    uint32_t tpos = tt[origptr] >> 8;
    uint32_t crc = 0xffffffffu;
    uint8_t obuf[4096];
    size_t on = 0;
    int last = -1, run = 0;
    for (int i = 0; i < nblock; i++) {
        tpos = tt[tpos];
        uint8_t ch = (uint8_t)(tpos & 0xff);
        tpos >>= 8;
        if (run == 4) {
            /* the count after four equal bytes */
            for (int k = 0; k < ch; k++) {
                CZ_B_CRC(crc, last);
                obuf[on++] = (uint8_t)last;
                if (on == sizeof obuf) { d->out(d->octx, obuf, on); on = 0; }
            }
            run = 0;
            last = -1;
            continue;
        }
        if (ch == last) run++;
        else { run = 1; last = ch; }
        CZ_B_CRC(crc, ch);
        obuf[on++] = ch;
        if (on == sizeof obuf) { d->out(d->octx, obuf, on); on = 0; }
    }
    if (on) d->out(d->octx, obuf, on);
    crc = ~crc;
    if (d->verbosity >= 3) fprintf(stderr, " {0x%08x, 0x%08x}]", bcrc, crc);
    else cz_b_vmsg(d, 2, "]");
    if (crc != bcrc) return CZ_B_D_DATA;
    *combined = ((*combined << 1) | (*combined >> 31)) ^ crc;
    return CZ_B_D_OK;
}

/* one stream, from its "BZh" */
static int cz_b_dstream(struct cz_b_dec *d) {
    cz_b_crcinit();
    d->bitbuf = 0;
    d->bitcnt = 0;
    d->eof = 0;
    static const uint8_t magic[3] = {'B', 'Z', 'h'};
    for (int i = 0; i < 3; i++) {
        int c = cz_getc(d->in);
        if (c < 0) return CZ_B_D_EOF;
        if (c != magic[i]) return CZ_B_D_MAGIC;
    }
    int c = cz_getc(d->in);
    if (c < 0) return CZ_B_D_EOF;
    if (c < '1' || c > '9') return CZ_B_D_MAGIC;
    d->level = c - '0';
    uint32_t combined = 0;
    for (int blockno = 1;; blockno++) {
        uint32_t v;
        static const uint8_t bm[6] = {0x31, 0x41, 0x59, 0x26, 0x53, 0x59};
        static const uint8_t em[6] = {0x17, 0x72, 0x45, 0x38, 0x50, 0x90};
        CZ_B_NEED(v, 8);
        if (v == em[0]) {
            for (int i = 1; i < 6; i++) {
                CZ_B_NEED(v, 8);
                if (v != em[i]) return CZ_B_D_DATA;
            }
            uint32_t stored;
            CZ_B_NEED(stored, 32);
            if (d->verbosity >= 3)
                fprintf(stderr, "\n    combined CRCs: stored = 0x%08x, computed = 0x%08x", stored, combined);
            if (stored != combined) return CZ_B_D_DATA;
            d->bitcnt = 0;     /* the rest of the last byte */
            return CZ_B_D_OK;
        }
        if (v != bm[0]) return CZ_B_D_DATA;
        for (int i = 1; i < 6; i++) {
            CZ_B_NEED(v, 8);
            if (v != bm[i]) return CZ_B_D_DATA;
        }
        int r = cz_b_dblock(d, blockno, &combined);
        if (r != CZ_B_D_OK) return r;
    }
}

/* ======================================================================
 *  the bzip2 program
 * ====================================================================== */
#define CZ_B_SM_I2O 1          /* stdin to stdout */
#define CZ_B_SM_F2O 2          /* files to stdout */
#define CZ_B_SM_F2F 3          /* files to files */
#define CZ_B_OM_Z 1
#define CZ_B_OM_UNZ 2
#define CZ_B_OM_TEST 3

static struct {
    const char *prog;
    int src_mode, op_mode;
    int force, keep, small, noisy, verbosity, level;
    int exit_value;
    int nfiles, nprocessed;
    int longest;
    const char *in_name, *out_name;
    char *out_buf;
    int delete_output;       /* the output is ours to remove on failure */
    int ofd;
    int err;                 /* errno as bzip2 would have it at the time */
    int test_fails, unz_fails;
    struct stat in_st;
    /* output chunks, as bzip2 writes them */
    uint8_t chunk[5000];
    size_t cn;
    int discard;
} cz_bz;

static void cz_b_set_exit(int v) {
    if (v > cz_bz.exit_value) cz_bz.exit_value = v;
}

static void cz_b_pad(const char *s) {
    int n = (int)strlen(s);
    if (n >= cz_bz.longest) return;
    for (int i = 1; i <= cz_bz.longest - n; i++) fputc(' ', stderr);
}

static void cz_b_show_names(void) {
    if (cz_bz.noisy) fprintf(stderr, "\tInput file = %s, output file = %s\n", cz_bz.in_name, cz_bz.out_name);
}

static void cz_b_cadvise(void) {
    if (cz_bz.noisy)
        fprintf(stderr,
                "\nIt is possible that the compressed file(s) have become corrupted.\n"
                "You can use the -tvv option to test integrity of such files.\n\n"
                "You can use the `bzip2recover' program to attempt to recover\n"
                "data from undamaged sections of corrupted files.\n\n");
}

static void cz_b_fail(int ec) __attribute__((noreturn));
static void cz_b_fail(int ec) {
    if (cz_bz.src_mode == CZ_B_SM_F2F && cz_bz.op_mode != CZ_B_OM_TEST && cz_bz.delete_output) {
        struct stat st;
        if (stat(cz_bz.in_name, &st) == 0) {
            if (cz_bz.noisy) fprintf(stderr, "%s: Deleting output file %s, if it exists.\n", cz_bz.prog, cz_bz.out_name);
            if (cz_bz.ofd >= 0) close(cz_bz.ofd);
            cz_bz.ofd = -1;
            if (unlink(cz_bz.out_name) != 0)
                fprintf(stderr, "%s: WARNING: deletion of output file (apparently) failed.\n", cz_bz.prog);
        } else {
            fprintf(stderr, "%s: WARNING: deletion of output file suppressed\n", cz_bz.prog);
            fprintf(stderr, "%s:    since input file no longer exists.  Output file\n", cz_bz.prog);
            fprintf(stderr, "%s:    `%s' may be incomplete.\n", cz_bz.prog, cz_bz.out_name);
            fprintf(stderr, "%s:    I suggest doing an integrity test (bzip2 -tv) of it.\n", cz_bz.prog);
        }
    }
    if (cz_bz.noisy && cz_bz.nfiles > 0 && cz_bz.nprocessed < cz_bz.nfiles) {
        fprintf(stderr, "%s: WARNING: some files have not been processed:\n"
                        "%s:    %d specified on command line, %d not processed yet.\n\n",
                cz_bz.prog, cz_bz.prog, cz_bz.nfiles, cz_bz.nfiles - cz_bz.nprocessed);
    }
    cz_b_set_exit(ec);
    exit(cz_bz.exit_value);
}

/* bzip2's perror: the error the last failing call left */
static void cz_b_perror(int e) {
    fprintf(stderr, "%s: %s\n", cz_bz.prog, e ? strerror(e) : "Success");
}

static void cz_b_io_error(int e) __attribute__((noreturn));
static void cz_b_io_error(int e) {
    fflush(stdout);
    fprintf(stderr, "\n%s: I/O or other error, bailing out.  Possible reason follows.\n", cz_bz.prog);
    cz_b_perror(e);
    cz_b_show_names();
    cz_b_fail(1);
}

static void cz_b_crc_error(void) __attribute__((noreturn));
static void cz_b_crc_error(void) {
    fflush(stdout);
    fprintf(stderr, "\n%s: Data integrity error when decompressing.\n", cz_bz.prog);
    cz_b_show_names();
    cz_b_cadvise();
    cz_b_fail(2);
}

static void cz_b_eof_error(void) __attribute__((noreturn));
static void cz_b_eof_error(void) {
    fflush(stdout);
    if (cz_bz.noisy) {
        fprintf(stderr, "\n%s: Compressed file ends unexpectedly;\n\t"
                        "perhaps it is corrupted?  *Possible* reason follows.\n", cz_bz.prog);
        cz_b_perror(cz_bz.err);
        cz_b_show_names();
        cz_b_cadvise();
    }
    cz_b_fail(2);
}

/* ---- output ---- */
static void cz_b_write(const uint8_t *p, size_t n) {
    if (cz_bz.discard) return;
    int e = cz_write_all(cz_bz.ofd, p, n);
    if (e) cz_b_io_error(e);
}

static void cz_b_chunk_out(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    while (n) {
        size_t k = sizeof cz_bz.chunk - cz_bz.cn;
        if (k > n) k = n;
        memcpy(cz_bz.chunk + cz_bz.cn, p, k);
        cz_bz.cn += k;
        p += k;
        n -= k;
        if (cz_bz.cn == sizeof cz_bz.chunk) {
            cz_b_write(cz_bz.chunk, cz_bz.cn);
            cz_bz.cn = 0;
        }
    }
}

static void cz_b_zwrite(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    cz_b_write(p, n);
}

/* ---- suffixes ---- */
static const char *const cz_b_zsuf[4] = {".bz2", ".bz", ".tbz2", ".tbz"};
static const char *const cz_b_unzsuf[4] = {"", "", ".tar", ".tar"};

static int cz_b_has_suffix(const char *s, const char *suf) {
    size_t ns = strlen(s), nx = strlen(suf);
    return ns >= nx && strcmp(s + ns - nx, suf) == 0;
}

/* a file is there: what fopen(NAME, "rb") says */
static int cz_b_exists(const char *name) {
    int fd = open(name, O_RDONLY);
    if (fd < 0) { cz_bz.err = errno; return 0; }
    close(fd);
    return 1;
}

static int cz_b_is_dir(const char *name) {
    struct stat st;
    return stat(name, &st) == 0 && S_ISDIR(st.st_mode);
}

static int cz_b_not_regular(const char *name) {
    struct stat st;
    if (lstat(name, &st) != 0) return 1;
    return !S_ISREG(st.st_mode);
}

static int cz_b_other_links(const char *name) {
    struct stat st;
    if (lstat(name, &st) != 0) return 0;
    return (int)st.st_nlink - 1;
}

static int cz_b_create(const char *name) {
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) cz_bz.err = errno;
    return fd;
}

static void cz_b_apply_meta(const char *name) {
    struct timespec ts[2];
#ifdef SU_HOST
    ts[0] = cz_bz.in_st.st_atim;
    ts[1] = cz_bz.in_st.st_mtim;
#else
    ts[0].tv_sec = cz_bz.in_st.st_atime; ts[0].tv_nsec = 0;
    ts[1].tv_sec = cz_bz.in_st.st_mtime; ts[1].tv_nsec = 0;
#endif
    chmod(name, cz_bz.in_st.st_mode & 07777);
    if (chown(name, cz_bz.in_st.st_uid, cz_bz.in_st.st_gid) != 0) { /* as bzip2: no matter */ }
    utimensat(AT_FDCWD, name, ts, 0);
}

static int cz_b_open_in(const char *name) {
    int fd = su_open_read(name, 0);
    if (fd < 0) cz_bz.err = errno;
    return fd;
}

/* ---- the ratio line of -v ---- */
static void cz_b_verbose_ratio(uint64_t in, uint64_t out) {
    if (in == 0 || out == 0) {
        fprintf(stderr, " no data compressed.\n");
        return;
    }
    char r1[40], r2[40], r3[40];
    struct cz_dbl q = cz_d_ratio(out, in);
    cz_d_fmt(r1, sizeof r1, cz_d_ratio(in, out), 6, 3);
    cz_d_fmt(r2, sizeof r2, cz_d_ratio(8 * out, in), 6, 3);
    cz_d_fmt(r3, sizeof r3, cz_d_mul(cz_d_one_minus(q), 100), 5, 2);
    fprintf(stderr, "%s:1, %s bits/byte, %s%% saved, %llu in, %llu out.\n", r1, r2, r3,
            (unsigned long long)in, (unsigned long long)out);
}

/* ---- compress ---- */
static struct cz_b_enc *cz_b_e;

static void cz_b_compress_stream(int ifd) {
    if (!cz_b_e) cz_b_e = (struct cz_b_enc *)calloc(1, sizeof *cz_b_e);
    if (!cz_b_e || cz_b_enc_init(cz_b_e, cz_bz.level) != 0) {
        fprintf(stderr, "\n%s: couldn't allocate enough memory\n", cz_bz.prog);
        cz_b_fail(1);
    }
    cz_b_e->wr = cz_b_zwrite;
    cz_b_e->verbose = cz_bz.verbosity;
    if (cz_bz.verbosity >= 2) fputc('\n', stderr);
    uint8_t buf[5000];
    uint64_t nin = 0;
    for (;;) {
        ssize_t n = read(ifd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            cz_b_io_error(errno);
        }
        if (n == 0) break;
        nin += (uint64_t)n;
        cz_b_enc_feed(cz_b_e, buf, (size_t)n);
    }
    cz_b_enc_finish(cz_b_e);
    if (cz_bz.verbosity >= 2) fprintf(stderr, "    final combined CRC = 0x%08x\n   ", cz_b_e->combined);
    uint64_t nout = cz_b_e->bytes_out;
    cz_b_enc_free(cz_b_e);
    if (cz_bz.src_mode == CZ_B_SM_F2F) {
        cz_b_apply_meta(cz_bz.out_name);
        if (close(cz_bz.ofd) != 0) cz_b_io_error(errno);
        cz_bz.ofd = -1;
    }
    if (cz_bz.verbosity >= 1) cz_b_verbose_ratio(nin, nout);
}

static void cz_b_compress(const char *name) {
    char *outb = 0;
    if (cz_bz.src_mode == CZ_B_SM_I2O) { cz_bz.in_name = "(stdin)"; cz_bz.out_name = "(stdout)"; }
    else {
        cz_bz.in_name = name;
        if (cz_bz.src_mode == CZ_B_SM_F2F) {
            outb = (char *)malloc(strlen(name) + 8);
            strcpy(outb, name);
            strcat(outb, ".bz2");
            cz_bz.out_name = outb;
        } else cz_bz.out_name = "(stdout)";
    }
    if (cz_bz.src_mode != CZ_B_SM_I2O && !cz_b_exists(cz_bz.in_name)) {
        fprintf(stderr, "%s: Can't open input file %s: %s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
        cz_b_set_exit(1);
        goto out;
    }
    for (int i = 0; i < 4; i++) {
        if (cz_bz.src_mode != CZ_B_SM_I2O && cz_b_has_suffix(cz_bz.in_name, cz_b_zsuf[i])) {
            if (cz_bz.noisy)
                fprintf(stderr, "%s: Input file %s already has %s suffix.\n", cz_bz.prog, cz_bz.in_name, cz_b_zsuf[i]);
            cz_b_set_exit(1);
            goto out;
        }
    }
    if (cz_bz.src_mode != CZ_B_SM_I2O && cz_b_is_dir(cz_bz.in_name)) {
        fprintf(stderr, "%s: Input file %s is a directory.\n", cz_bz.prog, cz_bz.in_name);
        cz_b_set_exit(1);
        goto out;
    }
    if (cz_bz.src_mode == CZ_B_SM_F2F && !cz_bz.force && cz_b_not_regular(cz_bz.in_name)) {
        if (cz_bz.noisy) fprintf(stderr, "%s: Input file %s is not a normal file.\n", cz_bz.prog, cz_bz.in_name);
        cz_b_set_exit(1);
        goto out;
    }
    if (cz_bz.src_mode == CZ_B_SM_F2F && cz_b_exists(cz_bz.out_name)) {
        if (cz_bz.force) unlink(cz_bz.out_name);
        else {
            fprintf(stderr, "%s: Output file %s already exists.\n", cz_bz.prog, cz_bz.out_name);
            cz_b_set_exit(1);
            goto out;
        }
    }
    if (cz_bz.src_mode == CZ_B_SM_F2F && !cz_bz.force) {
        int n = cz_b_other_links(cz_bz.in_name);
        if (n > 0) {
            fprintf(stderr, "%s: Input file %s has %d other link%s.\n", cz_bz.prog, cz_bz.in_name, n, n > 1 ? "s" : "");
            cz_b_set_exit(1);
            goto out;
        }
    }
    if (cz_bz.src_mode == CZ_B_SM_F2F) stat(cz_bz.in_name, &cz_bz.in_st);
    int ifd = 0;
    if (cz_bz.src_mode == CZ_B_SM_I2O) {
        cz_bz.ofd = 1;
        if (isatty(1)) {
            fprintf(stderr, "%s: I won't write compressed data to a terminal.\n", cz_bz.prog);
            fprintf(stderr, "%s: For help, type: `%s --help'.\n", cz_bz.prog, cz_bz.prog);
            cz_b_set_exit(1);
            goto out;
        }
        cz_bz.err = ENOTTY;
    } else if (cz_bz.src_mode == CZ_B_SM_F2O) {
        ifd = cz_b_open_in(cz_bz.in_name);
        cz_bz.ofd = 1;
        if (isatty(1)) {
            fprintf(stderr, "%s: I won't write compressed data to a terminal.\n", cz_bz.prog);
            fprintf(stderr, "%s: For help, type: `%s --help'.\n", cz_bz.prog, cz_bz.prog);
            if (ifd >= 0) close(ifd);
            cz_b_set_exit(1);
            goto out;
        }
        cz_bz.err = ENOTTY;
        if (ifd < 0) {
            fprintf(stderr, "%s: Can't open input file %s: %s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
            cz_b_set_exit(1);
            goto out;
        }
    } else {
        ifd = cz_b_open_in(cz_bz.in_name);
        cz_bz.ofd = cz_b_create(cz_bz.out_name);
        if (cz_bz.ofd < 0) {
            fprintf(stderr, "%s: Can't create output file %s: %s.\n", cz_bz.prog, cz_bz.out_name, strerror(cz_bz.err));
            if (ifd >= 0) close(ifd);
            cz_b_set_exit(1);
            goto out;
        }
        if (ifd < 0) {
            fprintf(stderr, "%s: Can't open input file %s: %s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
            close(cz_bz.ofd);
            cz_bz.ofd = -1;
            cz_b_set_exit(1);
            goto out;
        }
    }
    if (cz_bz.verbosity >= 1) {
        fprintf(stderr, "  %s: ", cz_bz.in_name);
        cz_b_pad(cz_bz.in_name);
        fflush(stderr);
    }
    cz_bz.delete_output = 1;
    cz_b_compress_stream(ifd);
    if (ifd > 0) close(ifd);
    if (cz_bz.src_mode == CZ_B_SM_F2F) {
        cz_bz.delete_output = 0;
        if (!cz_bz.keep && unlink(cz_bz.in_name) != 0) cz_b_io_error(errno);
    }
    cz_bz.delete_output = 0;
out:
    free(outb);
}

/* ---- decompress and test ---- */
static struct cz_b_dec cz_b_d;

/* all of the input's streams; 1 when it is bzip2 data (or -f passed it
 * on), 0 when it is not */
static int cz_b_uncompress_stream(int ifd, int test) {
    struct cz_in in;
    cz_in_open(&in, ifd, 65536);
    cz_b_d.in = &in;
    cz_b_d.out = cz_b_chunk_out;
    cz_b_d.verbosity = cz_bz.verbosity;
    cz_bz.discard = test;
    int streamno = 0;
    for (;;) {
        cz_bz.cn = 0;
        streamno++;
        int r = cz_b_dstream(&cz_b_d);
        if (r == CZ_B_D_OK) {
            if (cz_bz.cn) cz_b_write(cz_bz.chunk, cz_bz.cn);
            cz_bz.cn = 0;
            if (cz_peekc(&in) < 0) break;
            continue;
        }
        cz_bz.cn = 0;
        if (r == CZ_B_D_MAGIC) {
            if (!test && cz_bz.force) {
                /* not bzip2 (or trailing data that is not): with -f, the
                 * whole input again, as it is -- bzip2 rewinds to its start */
                if (lseek(ifd, 0, SEEK_SET) == 0) {
                    cz_in_close(&in);
                    cz_in_open(&in, ifd, 65536);
                }
                uint8_t b[5000];
                size_t n;
                while ((n = cz_read(&in, b, sizeof b)) > 0) cz_b_write(b, n);
                break;
            }
            if (test) {
                if (cz_bz.verbosity == 0) fprintf(stderr, "%s: %s: ", cz_bz.prog, cz_bz.in_name);
                cz_in_close(&in);
                if (streamno == 1) {
                    fprintf(stderr, "bad magic number (file not created by bzip2)\n");
                    return 0;
                }
                if (cz_bz.noisy) fprintf(stderr, "trailing garbage after EOF ignored\n");
                return 1;
            }
            cz_in_close(&in);
            if (streamno == 1) return 0;
            if (cz_bz.noisy)
                fprintf(stderr, "\n%s: %s: trailing garbage after EOF ignored\n", cz_bz.prog, cz_bz.in_name);
            return 1;
        }
        if (test) {
            if (cz_bz.verbosity == 0) fprintf(stderr, "%s: %s: ", cz_bz.prog, cz_bz.in_name);
            cz_in_close(&in);
            fprintf(stderr, r == CZ_B_D_DATA ? "data integrity (CRC) error in data\n" : "file ends unexpectedly\n");
            return -1;
        }
        cz_in_close(&in);
        if (r == CZ_B_D_DATA) cz_b_crc_error();
        cz_b_eof_error();
    }
    cz_in_close(&in);
    if (cz_bz.verbosity >= 2) fprintf(stderr, "\n    ");
    return 1;
}

static void cz_b_uncompress(const char *name) {
    char *outb = 0;
    int cantguess = 0;
    if (cz_bz.src_mode == CZ_B_SM_I2O) { cz_bz.in_name = "(stdin)"; cz_bz.out_name = "(stdout)"; }
    else {
        cz_bz.in_name = name;
        if (cz_bz.src_mode == CZ_B_SM_F2F) {
            outb = (char *)malloc(strlen(name) + 8);
            strcpy(outb, name);
            int i;
            for (i = 0; i < 4; i++) {
                if (cz_b_has_suffix(outb, cz_b_zsuf[i])) {
                    strcpy(outb + strlen(outb) - strlen(cz_b_zsuf[i]), cz_b_unzsuf[i]);
                    break;
                }
            }
            if (i == 4) { cantguess = 1; strcat(outb, ".out"); }
            cz_bz.out_name = outb;
        } else cz_bz.out_name = "(stdout)";
    }
    if (cz_bz.src_mode != CZ_B_SM_I2O && !cz_b_exists(cz_bz.in_name)) {
        fprintf(stderr, "%s: Can't open input file %s: %s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
        cz_b_set_exit(1);
        goto out;
    }
    if (cz_bz.src_mode != CZ_B_SM_I2O && cz_b_is_dir(cz_bz.in_name)) {
        fprintf(stderr, "%s: Input file %s is a directory.\n", cz_bz.prog, cz_bz.in_name);
        cz_b_set_exit(1);
        goto out;
    }
    if (cz_bz.src_mode == CZ_B_SM_F2F && !cz_bz.force && cz_b_not_regular(cz_bz.in_name)) {
        if (cz_bz.noisy) fprintf(stderr, "%s: Input file %s is not a normal file.\n", cz_bz.prog, cz_bz.in_name);
        cz_b_set_exit(1);
        goto out;
    }
    if (cantguess && cz_bz.noisy)
        fprintf(stderr, "%s: Can't guess original name for %s -- using %s\n", cz_bz.prog, cz_bz.in_name, cz_bz.out_name);
    if (cz_bz.src_mode == CZ_B_SM_F2F && cz_b_exists(cz_bz.out_name)) {
        if (cz_bz.force) unlink(cz_bz.out_name);
        else {
            fprintf(stderr, "%s: Output file %s already exists.\n", cz_bz.prog, cz_bz.out_name);
            cz_b_set_exit(1);
            goto out;
        }
    }
    if (cz_bz.src_mode == CZ_B_SM_F2F && !cz_bz.force) {
        int n = cz_b_other_links(cz_bz.in_name);
        if (n > 0) {
            fprintf(stderr, "%s: Input file %s has %d other link%s.\n", cz_bz.prog, cz_bz.in_name, n, n > 1 ? "s" : "");
            cz_b_set_exit(1);
            goto out;
        }
    }
    if (cz_bz.src_mode == CZ_B_SM_F2F) stat(cz_bz.in_name, &cz_bz.in_st);
    int ifd = 0;
    if (cz_bz.src_mode == CZ_B_SM_I2O) {
        cz_bz.ofd = 1;
        if (isatty(0)) {
            fprintf(stderr, "%s: I won't read compressed data from a terminal.\n", cz_bz.prog);
            fprintf(stderr, "%s: For help, type: `%s --help'.\n", cz_bz.prog, cz_bz.prog);
            cz_b_set_exit(1);
            goto out;
        }
        cz_bz.err = ENOTTY;
    } else if (cz_bz.src_mode == CZ_B_SM_F2O) {
        ifd = cz_b_open_in(cz_bz.in_name);
        cz_bz.ofd = 1;
        if (ifd < 0) {
            fprintf(stderr, "%s: Can't open input file %s:%s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
            cz_b_set_exit(1);
            goto out;
        }
    } else {
        ifd = cz_b_open_in(cz_bz.in_name);
        cz_bz.ofd = cz_b_create(cz_bz.out_name);
        if (cz_bz.ofd < 0) {
            fprintf(stderr, "%s: Can't create output file %s: %s.\n", cz_bz.prog, cz_bz.out_name, strerror(cz_bz.err));
            if (ifd >= 0) close(ifd);
            cz_b_set_exit(1);
            goto out;
        }
        if (ifd < 0) {
            fprintf(stderr, "%s: Can't open input file %s: %s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
            close(cz_bz.ofd);
            cz_bz.ofd = -1;
            cz_b_set_exit(1);
            goto out;
        }
    }
    if (cz_bz.verbosity >= 1) {
        fprintf(stderr, "  %s: ", cz_bz.in_name);
        cz_b_pad(cz_bz.in_name);
        fflush(stderr);
    }
    cz_bz.delete_output = 1;
    int ok = cz_b_uncompress_stream(ifd, 0);
    if (ifd > 0) close(ifd);
    if (cz_bz.src_mode == CZ_B_SM_F2F) {
        if (ok) cz_b_apply_meta(cz_bz.out_name);
        if (close(cz_bz.ofd) != 0) cz_b_io_error(errno);
        cz_bz.ofd = -1;
    }
    if (ok) {
        cz_bz.delete_output = 0;
        if (cz_bz.src_mode == CZ_B_SM_F2F && !cz_bz.keep && unlink(cz_bz.in_name) != 0) cz_b_io_error(errno);
    } else {
        cz_bz.unz_fails = 1;
        cz_bz.delete_output = 0;
        if (cz_bz.src_mode == CZ_B_SM_F2F) unlink(cz_bz.out_name);
    }
    cz_bz.delete_output = 0;
    if (ok) {
        if (cz_bz.verbosity >= 1) fprintf(stderr, "done\n");
    } else {
        cz_b_set_exit(2);
        if (cz_bz.verbosity >= 1) fprintf(stderr, "not a bzip2 file.\n");
        else fprintf(stderr, "%s: %s is not a bzip2 file.\n", cz_bz.prog, cz_bz.in_name);
    }
out:
    free(outb);
}

static void cz_b_test(const char *name) {
    cz_bz.out_name = "(none)";
    cz_bz.in_name = cz_bz.src_mode == CZ_B_SM_I2O ? "(stdin)" : name;
    if (cz_bz.src_mode != CZ_B_SM_I2O && !cz_b_exists(cz_bz.in_name)) {
        fprintf(stderr, "%s: Can't open input %s: %s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
        cz_b_set_exit(1);
        return;
    }
    if (cz_bz.src_mode != CZ_B_SM_I2O && cz_b_is_dir(cz_bz.in_name)) {
        fprintf(stderr, "%s: Input file %s is a directory.\n", cz_bz.prog, cz_bz.in_name);
        cz_b_set_exit(1);
        return;
    }
    int ifd = 0;
    if (cz_bz.src_mode == CZ_B_SM_I2O) {
        if (isatty(0)) {
            fprintf(stderr, "%s: I won't read compressed data from a terminal.\n", cz_bz.prog);
            fprintf(stderr, "%s: For help, type: `%s --help'.\n", cz_bz.prog, cz_bz.prog);
            cz_b_set_exit(1);
            return;
        }
        cz_bz.err = ENOTTY;
    } else {
        ifd = cz_b_open_in(cz_bz.in_name);
        if (ifd < 0) {
            fprintf(stderr, "%s: Can't open input file %s:%s.\n", cz_bz.prog, cz_bz.in_name, strerror(cz_bz.err));
            cz_b_set_exit(1);
            return;
        }
    }
    if (cz_bz.verbosity >= 1) {
        fprintf(stderr, "  %s: ", cz_bz.in_name);
        cz_b_pad(cz_bz.in_name);
        fflush(stderr);
    }
    int ok = cz_b_uncompress_stream(ifd, 1);
    if (ifd > 0) close(ifd);
    if (ok > 0 && cz_bz.verbosity >= 1) fprintf(stderr, "ok\n");
    if (ok <= 0) cz_bz.test_fails = 1;
}

/* ---- the command line ---- */
static void cz_b_usage(void) {
    fprintf(stderr,
            "%s (tobyOS) 1.0, block-sorting compression.\n"
            "\n"
            "   usage: %s [flags and input files in any order]\n"
            "\n"
            "   -h --help           show this help\n"
            "   -d --decompress     restore compressed files\n"
            "   -z --compress       compress, whatever the name says\n"
            "   -k --keep           do not remove the input files\n"
            "   -f --force          replace existing outputs; take links\n"
            "   -t --test           check that compressed files are intact\n"
            "   -c --stdout         write to standard output\n"
            "   -q --quiet          say less about what goes wrong\n"
            "   -v --verbose        say what is done (more with -vv)\n"
            "   -L --license        show the version and the license\n"
            "   -V --version        show the version and the license\n"
            "   -s --small          decompress in less memory (blocks of 200k)\n"
            "   -1 .. -9            blocks of 100k .. 900k\n"
            "   --fast              the same as -1\n"
            "   --best              the same as -9\n"
            "\n"
            "   bzip2 compresses by default, bunzip2 decompresses, and bzcat\n"
            "   decompresses to standard output. With no files, standard input\n"
            "   is read and standard output written. Short flags combine: -v4.\n"
            "\n",
            cz_bz.prog, cz_bz.prog);
}

static void cz_b_license(void) {
    fprintf(stderr,
            "bzip2 (tobyOS) 1.0, block-sorting compression.\n"
            "   \n"
            "   Part of tobyOS, written for it from the bzip2 format's description\n"
            "   and bzip2's observable behaviour.\n"
            "   \n");
}

static int cz_b_main(int argc, char **argv) {
    memset(&cz_bz, 0, sizeof cz_bz);
    cz_bz.ofd = -1;
    cz_bz.noisy = 1;
    cz_bz.level = 9;
    cz_bz.longest = 7;
    const char *p = strrchr(argv[0], '/');
    cz_bz.prog = p ? p + 1 : argv[0];
    /* the arguments: BZIP2's words, BZIP's, then the command line's */
    int cap = argc + 64, na = 0;
    char **args = (char **)malloc((size_t)cap * sizeof *args);
    const char *envs[2] = {"BZIP2", "BZIP"};
    for (int k = 0; k < 2; k++) {
        const char *ev = getenv(envs[k]);
        if (!ev) continue;
        const char *s = ev;
        for (;;) {
            while (*s && isspace((unsigned char)*s)) s++;
            if (!*s) break;
            const char *e = s;
            while (*e && !isspace((unsigned char)*e)) e++;
            if (na + 1 >= cap) { cap *= 2; args = (char **)realloc(args, (size_t)cap * sizeof *args); }
            char *w = (char *)malloc((size_t)(e - s) + 1);
            memcpy(w, s, (size_t)(e - s));
            w[e - s] = 0;
            args[na++] = w;
            s = e;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (na + 1 >= cap) { cap *= 2; args = (char **)realloc(args, (size_t)cap * sizeof *args); }
        args[na++] = argv[i];
    }
    int decode = 1;
    for (int i = 0; i < na; i++) {
        if (strcmp(args[i], "--") == 0) { decode = 0; continue; }
        if (args[i][0] == '-' && decode) continue;
        cz_bz.nfiles++;
        if ((int)strlen(args[i]) > cz_bz.longest) cz_bz.longest = (int)strlen(args[i]);
    }
    cz_bz.src_mode = cz_bz.nfiles == 0 ? CZ_B_SM_I2O : CZ_B_SM_F2F;
    cz_bz.op_mode = CZ_B_OM_Z;
    if (strstr(cz_bz.prog, "unzip") || strstr(cz_bz.prog, "UNZIP")) cz_bz.op_mode = CZ_B_OM_UNZ;
    if (strstr(cz_bz.prog, "z2cat") || strstr(cz_bz.prog, "Z2CAT") ||
        strstr(cz_bz.prog, "zcat") || strstr(cz_bz.prog, "ZCAT")) {
        cz_bz.op_mode = CZ_B_OM_UNZ;
        cz_bz.src_mode = cz_bz.nfiles == 0 ? CZ_B_SM_I2O : CZ_B_SM_F2O;
    }
    /* the short flags, in order, then the long ones */
    for (int i = 0; i < na; i++) {
        const char *a = args[i];
        if (strcmp(a, "--") == 0) break;
        if (a[0] != '-' || a[1] == '-') continue;
        for (int j = 1; a[j]; j++) {
            switch (a[j]) {
            case 'c': cz_bz.src_mode = CZ_B_SM_F2O; break;
            case 'd': cz_bz.op_mode = CZ_B_OM_UNZ; break;
            case 'z': cz_bz.op_mode = CZ_B_OM_Z; break;
            case 'f': cz_bz.force = 1; break;
            case 't': cz_bz.op_mode = CZ_B_OM_TEST; break;
            case 'k': cz_bz.keep = 1; break;
            case 's': cz_bz.small = 1; break;
            case 'q': cz_bz.noisy = 0; break;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                cz_bz.level = a[j] - '0';
                break;
            case 'V':
            case 'L': cz_b_license(); exit(0);
            case 'v': cz_bz.verbosity++; break;
            case 'h': cz_b_usage(); exit(0);
            default:
                fprintf(stderr, "%s: Bad flag `%s'\n", cz_bz.prog, a);
                cz_b_usage();
                exit(1);
            }
        }
    }
    for (int i = 0; i < na; i++) {
        const char *a = args[i];
        if (strcmp(a, "--") == 0) break;
        if (strncmp(a, "--", 2) != 0) continue;
        if (!strcmp(a, "--stdout")) cz_bz.src_mode = CZ_B_SM_F2O;
        else if (!strcmp(a, "--decompress")) cz_bz.op_mode = CZ_B_OM_UNZ;
        else if (!strcmp(a, "--compress")) cz_bz.op_mode = CZ_B_OM_Z;
        else if (!strcmp(a, "--force")) cz_bz.force = 1;
        else if (!strcmp(a, "--test")) cz_bz.op_mode = CZ_B_OM_TEST;
        else if (!strcmp(a, "--keep")) cz_bz.keep = 1;
        else if (!strcmp(a, "--small")) cz_bz.small = 1;
        else if (!strcmp(a, "--quiet")) cz_bz.noisy = 0;
        else if (!strcmp(a, "--version") || !strcmp(a, "--license")) { cz_b_license(); exit(0); }
        else if (!strcmp(a, "--exponential")) { /* the sorting effort: no change to the output */ }
        else if (!strcmp(a, "--repetitive-best") || !strcmp(a, "--repetitive-fast"))
            fprintf(stderr, "%s: %s is redundant in versions 0.9.5 and above\n", cz_bz.prog, a);
        else if (!strcmp(a, "--fast")) cz_bz.level = 1;
        else if (!strcmp(a, "--best")) cz_bz.level = 9;
        else if (!strcmp(a, "--verbose")) cz_bz.verbosity++;
        else if (!strcmp(a, "--help")) { cz_b_usage(); exit(0); }
        else {
            fprintf(stderr, "%s: Bad flag `%s'\n", cz_bz.prog, a);
            cz_b_usage();
            exit(1);
        }
    }
    if (cz_bz.verbosity > 4) cz_bz.verbosity = 4;
    if (cz_bz.op_mode == CZ_B_OM_Z && cz_bz.small && cz_bz.level > 2) cz_bz.level = 2;
    if (cz_bz.op_mode == CZ_B_OM_TEST && cz_bz.src_mode == CZ_B_SM_F2O) {
        fprintf(stderr, "%s: -c and -t cannot be used together.\n", cz_bz.prog);
        exit(1);
    }
    if (cz_bz.src_mode == CZ_B_SM_F2O && cz_bz.nfiles == 0) cz_bz.src_mode = CZ_B_SM_I2O;
    if (cz_bz.op_mode == CZ_B_OM_Z) {
        if (cz_bz.src_mode == CZ_B_SM_I2O) cz_b_compress(0);
        else {
            decode = 1;
            for (int i = 0; i < na; i++) {
                if (strcmp(args[i], "--") == 0) { decode = 0; continue; }
                if (args[i][0] == '-' && decode) continue;
                cz_bz.nprocessed++;
                cz_b_compress(args[i]);
            }
        }
    } else if (cz_bz.op_mode == CZ_B_OM_UNZ) {
        if (cz_bz.src_mode == CZ_B_SM_I2O) cz_b_uncompress(0);
        else {
            decode = 1;
            for (int i = 0; i < na; i++) {
                if (strcmp(args[i], "--") == 0) { decode = 0; continue; }
                if (args[i][0] == '-' && decode) continue;
                cz_bz.nprocessed++;
                cz_b_uncompress(args[i]);
            }
        }
        if (cz_bz.unz_fails) {
            cz_b_set_exit(2);
            exit(cz_bz.exit_value);
        }
    } else {
        if (cz_bz.src_mode == CZ_B_SM_I2O) cz_b_test(0);
        else {
            decode = 1;
            for (int i = 0; i < na; i++) {
                if (strcmp(args[i], "--") == 0) { decode = 0; continue; }
                if (args[i][0] == '-' && decode) continue;
                cz_bz.nprocessed++;
                cz_b_test(args[i]);
            }
        }
        if (cz_bz.test_fails) {
            if (cz_bz.noisy)
                fprintf(stderr, "\nYou can use the `bzip2recover' program to attempt to recover\n"
                                "data from undamaged sections of corrupted files.\n\n");
            cz_b_set_exit(2);
            exit(cz_bz.exit_value);
        }
    }
    return cz_bz.exit_value;
}

static int do_bzip2(int argc, char **argv) {
    return cz_b_main(argc, argv);
}
