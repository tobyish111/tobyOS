/* des.c -- single-block DES (ECB) for RFB security type 2.
 *
 * Clarity over speed: bits are held one-per-byte and permuted through the
 * standard tables. A VNC handshake encrypts exactly two blocks, so the cost
 * is irrelevant and the tables stay directly checkable against FIPS 46-3.
 * See des.h for why DES is here at all.
 */

#include <tobyos/des.h>

/* Compiled twice, like shell.c: once into the kernel, and once into
 * /bin/vncpasswd, which needs the same fixed-key obfuscation to write a
 * password file the kernel can read back. Only the memcpy/memset source
 * differs between the two. */
#ifdef DES_HOSTED
#include <string.h>
#else
#include <tobyos/klibc.h>
#endif

/* ---- standard tables (1-indexed, as printed in the standard) ------ */

static const uint8_t IP[64] = {
    58,50,42,34,26,18,10, 2, 60,52,44,36,28,20,12, 4,
    62,54,46,38,30,22,14, 6, 64,56,48,40,32,24,16, 8,
    57,49,41,33,25,17, 9, 1, 59,51,43,35,27,19,11, 3,
    61,53,45,37,29,21,13, 5, 63,55,47,39,31,23,15, 7
};

static const uint8_t FP[64] = {
    40, 8,48,16,56,24,64,32, 39, 7,47,15,55,23,63,31,
    38, 6,46,14,54,22,62,30, 37, 5,45,13,53,21,61,29,
    36, 4,44,12,52,20,60,28, 35, 3,43,11,51,19,59,27,
    34, 2,42,10,50,18,58,26, 33, 1,41, 9,49,17,57,25
};

static const uint8_t E_TAB[48] = {
    32, 1, 2, 3, 4, 5,  4, 5, 6, 7, 8, 9,
     8, 9,10,11,12,13, 12,13,14,15,16,17,
    16,17,18,19,20,21, 20,21,22,23,24,25,
    24,25,26,27,28,29, 28,29,30,31,32, 1
};

static const uint8_t P_TAB[32] = {
    16, 7,20,21, 29,12,28,17,  1,15,23,26,  5,18,31,10,
     2, 8,24,14, 32,27, 3, 9, 19,13,30, 6, 22,11, 4,25
};

static const uint8_t PC1[56] = {
    57,49,41,33,25,17, 9,  1,58,50,42,34,26,18,
    10, 2,59,51,43,35,27, 19,11, 3,60,52,44,36,
    63,55,47,39,31,23,15,  7,62,54,46,38,30,22,
    14, 6,61,53,45,37,29, 21,13, 5,28,20,12, 4
};

static const uint8_t PC2[48] = {
    14,17,11,24, 1, 5,  3,28,15, 6,21,10,
    23,19,12, 4,26, 8, 16, 7,27,20,13, 2,
    41,52,31,37,47,55, 30,40,51,45,33,48,
    44,49,39,56,34,53, 46,42,50,36,29,32
};

static const uint8_t SHIFTS[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};

static const uint8_t SBOX[8][64] = {
    { 14, 4,13, 1, 2,15,11, 8, 3,10, 6,12, 5, 9, 0, 7,
       0,15, 7, 4,14, 2,13, 1,10, 6,12,11, 9, 5, 3, 8,
       4, 1,14, 8,13, 6, 2,11,15,12, 9, 7, 3,10, 5, 0,
      15,12, 8, 2, 4, 9, 1, 7, 5,11, 3,14,10, 0, 6,13 },
    { 15, 1, 8,14, 6,11, 3, 4, 9, 7, 2,13,12, 0, 5,10,
       3,13, 4, 7,15, 2, 8,14,12, 0, 1,10, 6, 9,11, 5,
       0,14, 7,11,10, 4,13, 1, 5, 8,12, 6, 9, 3, 2,15,
      13, 8,10, 1, 3,15, 4, 2,11, 6, 7,12, 0, 5,14, 9 },
    { 10, 0, 9,14, 6, 3,15, 5, 1,13,12, 7,11, 4, 2, 8,
      13, 7, 0, 9, 3, 4, 6,10, 2, 8, 5,14,12,11,15, 1,
      13, 6, 4, 9, 8,15, 3, 0,11, 1, 2,12, 5,10,14, 7,
       1,10,13, 0, 6, 9, 8, 7, 4,15,14, 3,11, 5, 2,12 },
    {  7,13,14, 3, 0, 6, 9,10, 1, 2, 8, 5,11,12, 4,15,
      13, 8,11, 5, 6,15, 0, 3, 4, 7, 2,12, 1,10,14, 9,
      10, 6, 9, 0,12,11, 7,13,15, 1, 3,14, 5, 2, 8, 4,
       3,15, 0, 6,10, 1,13, 8, 9, 4, 5,11,12, 7, 2,14 },
    {  2,12, 4, 1, 7,10,11, 6, 8, 5, 3,15,13, 0,14, 9,
      14,11, 2,12, 4, 7,13, 1, 5, 0,15,10, 3, 9, 8, 6,
       4, 2, 1,11,10,13, 7, 8,15, 9,12, 5, 6, 3, 0,14,
      11, 8,12, 7, 1,14, 2,13, 6,15, 0, 9,10, 4, 5, 3 },
    { 12, 1,10,15, 9, 2, 6, 8, 0,13, 3, 4,14, 7, 5,11,
      10,15, 4, 2, 7,12, 9, 5, 6, 1,13,14, 0,11, 3, 8,
       9,14,15, 5, 2, 8,12, 3, 7, 0, 4,10, 1,13,11, 6,
       4, 3, 2,12, 9, 5,15,10,11,14, 1, 7, 6, 0, 8,13 },
    {  4,11, 2,14,15, 0, 8,13, 3,12, 9, 7, 5,10, 6, 1,
      13, 0,11, 7, 4, 9, 1,10,14, 3, 5,12, 2,15, 8, 6,
       1, 4,11,13,12, 3, 7,14,10,15, 6, 8, 0, 5, 9, 2,
       6,11,13, 8, 1, 4,10, 7, 9, 5, 0,15,14, 2, 3,12 },
    { 13, 2, 8, 4, 6,15,11, 1,10, 9, 3,14, 5, 0,12, 7,
       1,15,13, 8,10, 3, 7, 4,12, 5, 6,11, 0,14, 9, 2,
       7,11, 4, 1, 9,12,14, 2, 0, 6,10,13,15, 3, 5, 8,
       2, 1,14, 7, 4,10, 8,13,15,12, 9, 0, 3, 5, 6,11 }
};

/* ---- bit plumbing ------------------------------------------------- */

static void bytes_to_bits(const uint8_t *src, size_t nbytes, uint8_t *bits) {
    for (size_t i = 0; i < nbytes; i++)
        for (int b = 0; b < 8; b++)
            bits[i * 8 + b] = (uint8_t)((src[i] >> (7 - b)) & 1);
}

static void bits_to_bytes(const uint8_t *bits, size_t nbytes, uint8_t *dst) {
    for (size_t i = 0; i < nbytes; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) v = (uint8_t)((v << 1) | bits[i * 8 + b]);
        dst[i] = v;
    }
}

/* out[i] = in[table[i] - 1] */
static void permute(uint8_t *out, const uint8_t *in,
                    const uint8_t *table, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[table[i] - 1];
}

static void rotl28(uint8_t *half, int n) {
    uint8_t tmp[28];
    for (int i = 0; i < 28; i++) tmp[i] = half[(i + n) % 28];
    memcpy(half, tmp, 28);
}

static void des_schedule(struct des_ctx *ctx, const uint8_t key[8]) {
    uint8_t kb[64], pc1[56];
    bytes_to_bits(key, 8, kb);
    permute(pc1, kb, PC1, 56);

    uint8_t c[28], d[28];
    memcpy(c, pc1, 28);
    memcpy(d, pc1 + 28, 28);

    for (int r = 0; r < 16; r++) {
        rotl28(c, SHIFTS[r]);
        rotl28(d, SHIFTS[r]);
        uint8_t cd[56];
        memcpy(cd, c, 28);
        memcpy(cd + 28, d, 28);
        permute(ctx->subkey[r], cd, PC2, 48);
    }
}

void des_set_key(struct des_ctx *ctx, const uint8_t key[8]) {
    des_schedule(ctx, key);
}

void des_set_key_vnc(struct des_ctx *ctx, const uint8_t key[8]) {
    /* RFB consumes each key byte LSB-first. Everything else is textbook. */
    uint8_t rev[8];
    for (int i = 0; i < 8; i++) {
        uint8_t v = key[i], o = 0;
        for (int b = 0; b < 8; b++) o = (uint8_t)((o << 1) | ((v >> b) & 1));
        rev[i] = o;
    }
    des_schedule(ctx, rev);
}

/* ---- the round function ------------------------------------------ */

static void feistel(const uint8_t r[32], const uint8_t subkey[48],
                    uint8_t out[32]) {
    uint8_t exp[48];
    permute(exp, r, E_TAB, 48);
    for (int i = 0; i < 48; i++) exp[i] ^= subkey[i];

    uint8_t sout[32];
    for (int box = 0; box < 8; box++) {
        const uint8_t *g = exp + box * 6;
        /* Outer bits pick the row, inner four the column. */
        int row = (g[0] << 1) | g[5];
        int col = (g[1] << 3) | (g[2] << 2) | (g[3] << 1) | g[4];
        uint8_t v = SBOX[box][row * 16 + col];
        for (int b = 0; b < 4; b++)
            sout[box * 4 + b] = (uint8_t)((v >> (3 - b)) & 1);
    }
    permute(out, sout, P_TAB, 32);
}

static void des_crypt(const struct des_ctx *ctx, const uint8_t in[8],
                      uint8_t out[8], bool decrypt) {
    uint8_t bits[64], pre[64];
    bytes_to_bits(in, 8, bits);
    permute(pre, bits, IP, 64);

    uint8_t l[32], r[32];
    memcpy(l, pre, 32);
    memcpy(r, pre + 32, 32);

    for (int round = 0; round < 16; round++) {
        const uint8_t *sk = ctx->subkey[decrypt ? (15 - round) : round];
        uint8_t f[32];
        feistel(r, sk, f);
        for (int i = 0; i < 32; i++) f[i] ^= l[i];
        memcpy(l, r, 32);
        memcpy(r, f, 32);
    }

    /* Final swap: the pre-output is R16 L16, not L16 R16. */
    uint8_t rl[64];
    memcpy(rl, r, 32);
    memcpy(rl + 32, l, 32);

    uint8_t post[64];
    permute(post, rl, FP, 64);
    bits_to_bytes(post, 8, out);
}

void des_encrypt_block(const struct des_ctx *ctx, const uint8_t in[8],
                       uint8_t out[8]) {
    des_crypt(ctx, in, out, false);
}

void des_decrypt_block(const struct des_ctx *ctx, const uint8_t in[8],
                       uint8_t out[8]) {
    des_crypt(ctx, in, out, true);
}

/* ---- known-answer tests ------------------------------------------- */

bool des_selftest(void) {
    struct des_ctx ctx;
    uint8_t out[8];

    /* FIPS 46-3 worked example. */
    static const uint8_t k1[8] = {0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1};
    static const uint8_t p1[8] = {0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
    static const uint8_t c1[8] = {0x85,0xE8,0x13,0x54,0x0F,0x0A,0xB4,0x05};
    des_set_key(&ctx, k1);
    des_encrypt_block(&ctx, p1, out);
    if (memcmp(out, c1, 8) != 0) return false;

    /* Round-trip: decrypt must undo encrypt. */
    des_decrypt_block(&ctx, c1, out);
    if (memcmp(out, p1, 8) != 0) return false;

    /* Second independent vector, from the DES known-answer set. */
    static const uint8_t k2[8] = {0x0E,0x32,0x92,0x32,0xEA,0x6D,0x0D,0x73};
    static const uint8_t p2[8] = {0x87,0x87,0x87,0x87,0x87,0x87,0x87,0x87};
    static const uint8_t c2[8] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    des_set_key(&ctx, k2);
    des_encrypt_block(&ctx, p2, out);
    if (memcmp(out, c2, 8) != 0) return false;

    /* All-zero key and data -- catches a schedule that silently degenerates. */
    static const uint8_t k3[8] = {0,0,0,0,0,0,0,0};
    static const uint8_t c3[8] = {0x8C,0xA6,0x4D,0xE9,0xC1,0xB1,0x23,0xA7};
    des_set_key(&ctx, k3);
    des_encrypt_block(&ctx, k3, out);
    if (memcmp(out, c3, 8) != 0) return false;

    return true;
}
