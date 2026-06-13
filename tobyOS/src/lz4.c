/* lz4.c -- minimal LZ4 block-format codec. See lz4.h for the format. */

#include <tobyos/lz4.h>
#include <tobyos/klibc.h>

#define LZ4_MINMATCH      4
#define LZ4_LAST_LITERALS 5
#define LZ4_MFLIMIT       12      /* matches never start in the last 12 B */
#define LZ4_MAX_OFFSET    65535

static inline uint32_t lz4_read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);            /* unaligned-safe */
    return v;
}

/* 4-byte sequence -> hash bucket. The classic LZ4 multiplier. */
static inline uint32_t lz4_hash(uint32_t seq) {
    return (seq * 2654435761u) >> (32 - LZ4_HASH_BITS);
}

int lz4_compress(const void *src_, int slen, void *dst_, int dcap,
                 void *hashtable) {
    const uint8_t *src = (const uint8_t *)src_;
    uint8_t *dst = (uint8_t *)dst_;
    int *ht = (int *)hashtable;
    if (slen < 0 || dcap <= 0) return -1;

    for (uint32_t i = 0; i < LZ4_HASH_SIZE; i++) ht[i] = -1;

    const uint8_t *ip     = src;
    const uint8_t *anchor = src;
    const uint8_t *iend   = src + slen;
    const uint8_t *mflimit = iend - LZ4_MFLIMIT;     /* last match-start */
    const uint8_t *matchlimit = iend - LZ4_LAST_LITERALS;
    uint8_t *op   = dst;
    uint8_t *oend = dst + dcap;

    /* Inputs too small to hold a match are emitted as a single literal
     * run by the tail code below. */
    while (slen >= LZ4_MFLIMIT + 1 && ip <= mflimit) {
        uint32_t seq = lz4_read32(ip);
        uint32_t h   = lz4_hash(seq);
        int cand     = ht[h];
        ht[h]        = (int)(ip - src);

        const uint8_t *match = (cand >= 0) ? src + cand : NULL;
        if (!match || (ip - match) > LZ4_MAX_OFFSET ||
            lz4_read32(match) != seq) {
            ip++;                          /* no match: advance one literal */
            continue;
        }

        /* Extend the match forward (but not into the last 5 literals). */
        const uint8_t *m = match + LZ4_MINMATCH;
        const uint8_t *p = ip + LZ4_MINMATCH;
        while (p < matchlimit && *p == *m) { p++; m++; }
        int mlen = (int)(p - ip);                  /* total match length */
        int extra_mlen = mlen - LZ4_MINMATCH;      /* token-encoded length */
        int litlen = (int)(ip - anchor);
        int offset = (int)(ip - match);

        /* --- emit one sequence: token + literals + offset + match-extra --- */
        uint8_t *token = op;
        if (op >= oend) return -1;
        *op++ = (uint8_t)(((litlen >= 15 ? 15 : litlen) << 4) |
                          (extra_mlen >= 15 ? 15 : extra_mlen));
        if (litlen >= 15) {
            int rem = litlen - 15;
            while (rem >= 255) { if (op >= oend) return -1; *op++ = 255; rem -= 255; }
            if (op >= oend) return -1; *op++ = (uint8_t)rem;
        }
        if (op + litlen > oend) return -1;
        memcpy(op, anchor, (size_t)litlen); op += litlen;

        if (op + 2 > oend) return -1;
        *op++ = (uint8_t)(offset & 0xFF);
        *op++ = (uint8_t)((offset >> 8) & 0xFF);

        if (extra_mlen >= 15) {
            int rem = extra_mlen - 15;
            while (rem >= 255) { if (op >= oend) return -1; *op++ = 255; rem -= 255; }
            if (op >= oend) return -1; *op++ = (uint8_t)rem;
        }
        (void)token;

        ip = p;                            /* skip the whole match */
        anchor = ip;
    }

    /* --- final literal run [anchor, iend) --- */
    {
        int litlen = (int)(iend - anchor);
        if (op >= oend) return -1;
        *op++ = (uint8_t)((litlen >= 15 ? 15 : litlen) << 4);
        if (litlen >= 15) {
            int rem = litlen - 15;
            while (rem >= 255) { if (op >= oend) return -1; *op++ = 255; rem -= 255; }
            if (op >= oend) return -1; *op++ = (uint8_t)rem;
        }
        if (op + litlen > oend) return -1;
        memcpy(op, anchor, (size_t)litlen); op += litlen;
    }

    int clen = (int)(op - dst);
    if (clen >= slen) return -1;           /* didn't actually shrink */
    return clen;
}

int lz4_decompress(const void *src_, int slen, void *dst_, int dcap) {
    const uint8_t *sp   = (const uint8_t *)src_;
    const uint8_t *send = sp + slen;
    uint8_t *dst  = (uint8_t *)dst_;
    uint8_t *dp   = dst;
    uint8_t *dend = dst + dcap;
    if (slen < 0 || dcap < 0) return -1;

    while (sp < send) {
        uint8_t token = *sp++;

        /* literals */
        int litlen = token >> 4;
        if (litlen == 15) {
            uint8_t b;
            do {
                if (sp >= send) return -1;
                b = *sp++;
                litlen += b;
            } while (b == 255);
        }
        if (litlen) {
            if (sp + litlen > send) return -1;
            if (dp + litlen > dend) return -1;
            memcpy(dp, sp, (size_t)litlen);
            dp += litlen; sp += litlen;
        }
        if (sp == send) break;             /* last sequence: literals only */

        /* match */
        if (sp + 2 > send) return -1;
        int offset = sp[0] | (sp[1] << 8);
        sp += 2;
        if (offset == 0) return -1;
        int matchlen = token & 0x0F;
        if (matchlen == 15) {
            uint8_t b;
            do {
                if (sp >= send) return -1;
                b = *sp++;
                matchlen += b;
            } while (b == 255);
        }
        matchlen += LZ4_MINMATCH;

        const uint8_t *mref = dp - offset;
        if (mref < dst) return -1;          /* offset points before output */
        if (dp + matchlen > dend) return -1;
        /* Byte-wise copy: matches may overlap (offset < matchlen). */
        for (int k = 0; k < matchlen; k++) dp[k] = mref[k];
        dp += matchlen;
    }
    return (int)(dp - dst);
}
