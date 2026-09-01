/* lzw.c -- the .Z format, both directions.
 *
 * `uncompress` has been on PATH here for a while as another name for
 * gunzip, which cannot read a .Z file at all: the name was present and the
 * format was not. This is the format.
 *
 * .Z is LZW as compress(1) writes it:
 *
 *   byte 0,1   magic 0x1F 0x9D
 *   byte 2     bit 7 = block mode, bits 0-4 = maximum code width
 *   then       codes, LSB-first, starting at 9 bits and widening
 *
 * THE ONE THING THAT MAKES THIS FORMAT AWKWARD: when the code width
 * changes, the bit stream is padded to a multiple of (old width * 8) bits.
 * compress(1) buffers codes in groups of eight and flushes the partial
 * group on a width change, so a decoder that just keeps reading bits goes
 * out of step exactly once and then produces plausible garbage forever.
 *
 * AND THE BOUNDARY IS MEASURED FROM THE END OF THE 3-BYTE HEADER, not
 * from the start of the file. compress(1) tracks its bit position inside
 * a buffer that begins after the header, so the groups start at file byte
 * 3. Counting from byte 0 is off by 24 bits, which is invisible until the
 * first width change and then corrupts everything after it -- small files
 * round-trip perfectly and large ones do not.
 */

#include <toby/lzw.h>

#include <stdlib.h>
#include <string.h>

#define LZW_MAGIC_0   0x1F
#define LZW_MAGIC_1   0x9D
#define LZW_BLOCK     0x80
#define LZW_INIT_BITS 9
#define LZW_CLEAR     256

/* Rounds a bit position up to the next multiple of `group`. An exact
 * multiple is already aligned. */
static size_t align_up(size_t bits, size_t group) {
    return ((bits + group - 1) / group) * group;
}

/* ------------------------------------------------------------- decoding */

int lzw_decompress(const uint8_t *in, size_t inlen,
                   uint8_t **out, size_t *outlen, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "";
    *out = NULL;
    *outlen = 0;

    if (inlen < 3 || in[0] != LZW_MAGIC_0 || in[1] != LZW_MAGIC_1) {
        *why = "not in compress (.Z) format";
        return -1;
    }
    int maxbits    = in[2] & 0x1F;
    int block_mode = (in[2] & LZW_BLOCK) != 0;
    if (maxbits < LZW_INIT_BITS || maxbits > LZW_MAX_BITS) {
        *why = "unsupported maximum code width";
        return -1;
    }
    size_t maxmaxcode = (size_t)1 << maxbits;

    uint16_t *prefix = (uint16_t *)malloc(maxmaxcode * sizeof *prefix);
    uint8_t  *suffix = (uint8_t *)malloc(maxmaxcode);
    uint8_t  *stack  = (uint8_t *)malloc(maxmaxcode + 1);
    if (!prefix || !suffix || !stack) {
        free(prefix); free(suffix); free(stack);
        *why = "out of memory";
        return -1;
    }
    for (size_t i = 0; i < 256; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }

    size_t cap = inlen * 4 + 1024, len = 0;
    uint8_t *ob = (uint8_t *)malloc(cap);
    if (!ob) {
        free(prefix); free(suffix); free(stack);
        *why = "out of memory";
        return -1;
    }

    int n_bits = LZW_INIT_BITS;
    size_t maxcode = ((size_t)1 << n_bits) - 1;
    size_t free_ent = block_mode ? 257 : 256;
    /* Relative to the first byte after the header, which is where the
     * group boundaries are measured from. */
    size_t posbits = 0;
    size_t total_bits = (inlen - 3) * 8;
    long oldcode = -1;
    uint8_t finchar = 0;

    for (;;) {
        if (free_ent > maxcode) {
            posbits = align_up(posbits, (size_t)n_bits * 8);
            n_bits++;
            maxcode = (n_bits == maxbits) ? maxmaxcode : (((size_t)1 << n_bits) - 1);
        }
        if (posbits + (size_t)n_bits > total_bits) break;

        size_t byte = 3 + (posbits >> 3);
        unsigned shift = (unsigned)(posbits & 7);
        uint32_t acc = (uint32_t)in[byte];
        if (byte + 1 < inlen) acc |= (uint32_t)in[byte + 1] << 8;
        if (byte + 2 < inlen) acc |= (uint32_t)in[byte + 2] << 16;
        long code = (long)((acc >> shift) & (((uint32_t)1 << n_bits) - 1));
        posbits += (size_t)n_bits;

        if (block_mode && code == LZW_CLEAR) {
            posbits = align_up(posbits, (size_t)n_bits * 8);
            n_bits = LZW_INIT_BITS;
            maxcode = ((size_t)1 << n_bits) - 1;
            free_ent = 257;
            oldcode = -1;
            continue;
        }

        if (oldcode < 0) {
            if (code >= 256) { *why = "corrupt stream: first code is not a literal"; goto fail; }
            oldcode = code;
            finchar = (uint8_t)code;
            if (len + 1 > cap) {
                cap *= 2;
                uint8_t *nb = (uint8_t *)realloc(ob, cap);
                if (!nb) { *why = "out of memory"; goto fail; }
                ob = nb;
            }
            ob[len++] = finchar;
            continue;
        }

        long incode = code;
        size_t sp = 0;
        /* The KwKwK case: a code that has not been defined yet is the
         * previous string plus its own first character. */
        if ((size_t)code >= free_ent) {
            if ((size_t)code > free_ent) { *why = "corrupt stream: code past the table"; goto fail; }
            stack[sp++] = finchar;
            code = oldcode;
        }
        while (code >= 256) {
            if (sp > maxmaxcode) { *why = "corrupt stream: cyclic table entry"; goto fail; }
            stack[sp++] = suffix[code];
            code = prefix[code];
        }
        finchar = suffix[code];
        stack[sp++] = finchar;

        if (len + sp > cap) {
            while (len + sp > cap) cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(ob, cap);
            if (!nb) { *why = "out of memory"; goto fail; }
            ob = nb;
        }
        while (sp) ob[len++] = stack[--sp];

        if (free_ent < maxmaxcode) {
            prefix[free_ent] = (uint16_t)oldcode;
            suffix[free_ent] = finchar;
            free_ent++;
        }
        oldcode = incode;
    }

    free(prefix); free(suffix); free(stack);
    *out = ob;
    *outlen = len;
    return 0;

fail:
    free(prefix); free(suffix); free(stack); free(ob);
    return -1;
}

/* ------------------------------------------------------------- encoding */

/* One hash table entry per possible code: compress(1) uses an open hash
 * over (prefix, suffix); a direct table of 1 << maxbits chained buckets is
 * simpler and the sizes here are small. */
struct enc {
    uint8_t *buf;
    size_t   len, cap;
    size_t   bits;          /* bits written so far */
};

static int put_bits(struct enc *e, uint32_t code, int n) {
    size_t need = (e->bits + (size_t)n + 7) / 8 + 1;
    if (need > e->cap) {
        size_t nc = e->cap ? e->cap * 2 : 65536;
        while (nc < need) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(e->buf, nc);
        if (!nb) return -1;
        memset(nb + e->cap, 0, nc - e->cap);
        e->buf = nb;
        e->cap = nc;
    }
    for (int i = 0; i < n; i++) {
        if (code & ((uint32_t)1 << i)) e->buf[(e->bits + (size_t)i) >> 3] |=
            (uint8_t)(1u << ((e->bits + (size_t)i) & 7));
    }
    e->bits += (size_t)n;
    e->len = (e->bits + 7) / 8;
    return 0;
}

int lzw_compress(const uint8_t *in, size_t inlen, int maxbits,
                 uint8_t **out, size_t *outlen, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    *why = "";
    *out = NULL;
    *outlen = 0;
    if (maxbits < LZW_INIT_BITS || maxbits > LZW_MAX_BITS) maxbits = LZW_MAX_BITS;

    size_t maxmaxcode = (size_t)1 << maxbits;
    /* (prefix << 8) | suffix -> code. A flat table over 1<<maxbits prefixes
     * times 256 suffixes would be 16 M entries at maxbits 16, so this is a
     * chained hash instead. */
    size_t hsize = maxmaxcode * 2;
    int32_t *hcode = (int32_t *)malloc(hsize * sizeof *hcode);
    uint32_t *hkey = (uint32_t *)malloc(hsize * sizeof *hkey);
    if (!hcode || !hkey) { free(hcode); free(hkey); *why = "out of memory"; return -1; }

    struct enc e;
    memset(&e, 0, sizeof e);
    if (put_bits(&e, LZW_MAGIC_0, 8) || put_bits(&e, LZW_MAGIC_1, 8) ||
        put_bits(&e, (uint32_t)(LZW_BLOCK | maxbits), 8)) {
        free(hcode); free(hkey); free(e.buf);
        *why = "out of memory";
        return -1;
    }

    for (size_t i = 0; i < hsize; i++) hcode[i] = -1;

    int n_bits = LZW_INIT_BITS;
    size_t maxcode = ((size_t)1 << n_bits) - 1;
    size_t free_ent = 257;                 /* block mode reserves 256 */

    if (inlen == 0) {
        free(hcode); free(hkey);
        *out = e.buf;
        *outlen = e.len;
        return 0;
    }

    long ent = in[0];
    for (size_t i = 1; i < inlen; i++) {
        uint32_t key = ((uint32_t)ent << 8) | in[i];
        size_t h = ((key >> 12) ^ key) % hsize;
        size_t step = 1 + (key % (hsize - 1));
        long found = -1;
        for (;;) {
            if (hcode[h] < 0) break;
            if (hkey[h] == key) { found = hcode[h]; break; }
            h = (h + step) % hsize;
        }
        if (found >= 0) { ent = found; continue; }

        if (put_bits(&e, (uint32_t)ent, n_bits)) { *why = "out of memory"; goto fail; }

        /* THE WIDTH TEST HAPPENS BEFORE THIS STEP'S INSERT, and that is not
         * a detail. A decoder cannot add the entry for a code until it has
         * seen the NEXT one, so its table is permanently one entry behind
         * the encoder's. compress(1) tests the pre-insert value here, which
         * puts the encoder's widen exactly one code ahead of the decoder's
         * -- and one code ahead is precisely right, because the encoder
         * widens AFTER writing code N and the decoder widens BEFORE reading
         * code N+1. Testing the post-insert value instead widens one code
         * early, which round-trips fine until the first width change and is
         * garbage after it. */
        if (free_ent > maxcode && n_bits < maxbits) {
            /* Pad to the group boundary the decoder will skip to. */
            size_t group = (size_t)n_bits * 8;
            size_t aligned = 24 + align_up(e.bits - 24, group);
            while (e.bits < aligned)
                if (put_bits(&e, 0, 1)) { *why = "out of memory"; goto fail; }
            n_bits++;
            maxcode = (n_bits == maxbits) ? maxmaxcode : (((size_t)1 << n_bits) - 1);
        }

        if (free_ent < maxmaxcode) {
            hcode[h] = (int32_t)free_ent;
            hkey[h] = key;
            free_ent++;
        }
        ent = in[i];
    }
    if (put_bits(&e, (uint32_t)ent, n_bits)) { *why = "out of memory"; goto fail; }

    free(hcode); free(hkey);
    *out = e.buf;
    *outlen = e.len;
    return 0;

fail:
    free(hcode); free(hkey); free(e.buf);
    return -1;
}
