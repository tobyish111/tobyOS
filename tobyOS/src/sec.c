/* sec.c -- Milestone 34 security primitives.
 *
 * Implements SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104) plus
 * helpers for hex conversion, constant-time compare, and a tiny
 * text-file trust store.
 *
 * The SHA-256 core is a direct, unrolled implementation of the
 * FIPS 180-4 reference: same constants, same message schedule, same
 * rotations. No SIMD, no x86-specific intrinsics -- the entire
 * security layer is one self-contained TU so that future audits don't
 * have to chase symbols across the kernel.
 *
 * Test vectors from FIPS 180-2 / RFC 6234 are exercised at boot via
 * sec_selftest() (called from kmain) so a build-time regression of
 * any of these primitives shows up as an early panic rather than a
 * silent verification skip downstream.
 */

#include <tobyos/sec.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/panic.h>

/* ===================================================================
 *  SHA-256 (FIPS 180-4)
 * =================================================================== */

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(struct sha256_ctx *c, const uint8_t blk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4 + 0] << 24)
             | ((uint32_t)blk[i * 4 + 1] << 16)
             | ((uint32_t)blk[i * 4 + 2] <<  8)
             | ((uint32_t)blk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15],  7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >>  3);
        uint32_t s1 = rotr32(w[i -  2], 17) ^ rotr32(w[i -  2], 19) ^ (w[i -  2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g  = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a;  c->state[1] += b;  c->state[2] += cc; c->state[3] += d;
    c->state[4] += e;  c->state[5] += f;  c->state[6] += g;  c->state[7] += h;
}

void sha256_init(struct sha256_ctx *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0; c->datalen = 0;
}

void sha256_update(struct sha256_ctx *c, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n--) {
        c->data[c->datalen++] = *p++;
        if (c->datalen == 64) {
            sha256_compress(c, c->data);
            c->bitlen += 512;
            c->datalen = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]) {
    uint32_t i = c->datalen;
    /* Pad: 0x80 then zeros, until 56 bytes mod 64. */
    if (i < 56) {
        c->data[i++] = 0x80u;
        while (i < 56) c->data[i++] = 0x00u;
    } else {
        c->data[i++] = 0x80u;
        while (i < 64) c->data[i++] = 0x00u;
        sha256_compress(c, c->data);
        memset(c->data, 0, 56);
    }
    c->bitlen += (uint64_t)c->datalen * 8u;
    /* Big-endian 64-bit bit length. */
    c->data[63] = (uint8_t)(c->bitlen      );
    c->data[62] = (uint8_t)(c->bitlen >>  8);
    c->data[61] = (uint8_t)(c->bitlen >> 16);
    c->data[60] = (uint8_t)(c->bitlen >> 24);
    c->data[59] = (uint8_t)(c->bitlen >> 32);
    c->data[58] = (uint8_t)(c->bitlen >> 40);
    c->data[57] = (uint8_t)(c->bitlen >> 48);
    c->data[56] = (uint8_t)(c->bitlen >> 56);
    sha256_compress(c, c->data);

    for (int j = 0; j < 8; j++) {
        out[j * 4 + 0] = (uint8_t)(c->state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(c->state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(c->state[j] >>  8);
        out[j * 4 + 3] = (uint8_t)(c->state[j]      );
    }
    /* Wipe state to discourage stale-data attacks if the buffer is
     * reused. Caller-owned context, but cheap insurance. */
    memset(c, 0, sizeof(*c));
}

void sha256_buf(const void *buf, size_t n, uint8_t out[SHA256_DIGEST_LEN]) {
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, buf, n);
    sha256_final(&c, out);
}

/* ===================================================================
 *  HMAC-SHA-256 (RFC 2104)
 * =================================================================== */

void hmac_sha256_init(struct hmac_sha256_ctx *c,
                      const void *key, size_t key_len) {
    uint8_t k[SHA256_BLOCK_SIZE];
    memset(k, 0, sizeof(k));

    if (key_len > SHA256_BLOCK_SIZE) {
        /* Long keys are pre-hashed per RFC 2104. */
        uint8_t kh[SHA256_DIGEST_LEN];
        sha256_buf(key, key_len, kh);
        memcpy(k, kh, SHA256_DIGEST_LEN);
    } else if (key && key_len > 0) {
        memcpy(k, key, key_len);
    }

    uint8_t k_ipad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        k_ipad[i]    = (uint8_t)(k[i] ^ 0x36u);
        c->k_opad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }

    sha256_init(&c->inner);
    sha256_update(&c->inner, k_ipad, SHA256_BLOCK_SIZE);
}

void hmac_sha256_update(struct hmac_sha256_ctx *c,
                        const void *buf, size_t n) {
    sha256_update(&c->inner, buf, n);
}

void hmac_sha256_final(struct hmac_sha256_ctx *c,
                       uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t inner_digest[SHA256_DIGEST_LEN];
    sha256_final(&c->inner, inner_digest);

    struct sha256_ctx outer;
    sha256_init(&outer);
    sha256_update(&outer, c->k_opad, SHA256_BLOCK_SIZE);
    sha256_update(&outer, inner_digest, SHA256_DIGEST_LEN);
    sha256_final(&outer, out);

    memset(c, 0, sizeof(*c));
}

/* ===================================================================
 *  hex helpers + constant-time compare
 * =================================================================== */

static const char HEX_LO[16] = {
    '0','1','2','3','4','5','6','7',
    '8','9','a','b','c','d','e','f'
};

void sec_to_hex(const uint8_t *bytes, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        out[i * 2 + 0] = HEX_LO[(bytes[i] >> 4) & 0xfu];
        out[i * 2 + 1] = HEX_LO[ bytes[i]       & 0xfu];
    }
    out[n * 2] = '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int sec_from_hex(const char *hex, size_t n, uint8_t *out) {
    if (!hex || !out) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[i * 2 + 0]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    /* Reject trailing junk: hex string must be EXACTLY 2*n chars. */
    if (hex[n * 2] != '\0' && hex[n * 2] != '\n' &&
        hex[n * 2] != '\r' && hex[n * 2] != ' ') {
        return -1;
    }
    return 0;
}

int sec_memeq_ct(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0 ? 0 : 1;
}

/* ===================================================================
 *  Trust store
 * =================================================================== */

#define SEC_TRUST_MAX 16

static struct sec_key g_trust[SEC_TRUST_MAX];
static int            g_trust_loaded = 0;

const char *sig_algo_name(int algo) {
    switch (algo) {
    case SEC_ALGO_NONE:        return "none";
    case SEC_ALGO_HMAC_SHA256: return "HMAC-SHA256";
    default:                   return "?";
    }
}

static int parse_algo(const char *s, size_t n) {
    /* Tolerate trailing whitespace in the caller; n is the exact
     * token length. */
    if (n == 11 && strncmp(s, "HMAC-SHA256", 11) == 0)
        return SEC_ALGO_HMAC_SHA256;
    return SEC_ALGO_NONE;
}

/* Skip ASCII spaces + tabs, returning the first non-whitespace byte.*/
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/* Lex a non-whitespace token. The caller's `*out` and `*out_n` are
 * set to the token's extent. Returns the byte after the token (still
 * inside [start,end)). */
static const char *lex_tok(const char *p, const char *end,
                           const char **out, size_t *out_n) {
    p = skip_ws(p, end);
    *out = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    *out_n = (size_t)(p - *out);
    return p;
}

static bool valid_id_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static int parse_trust_line(const char *line, size_t n) {
    /* Format: "KEY <id> <algo> <hex-key>". Comments + blanks are
     * filtered by the caller. */
    const char *p = line, *end = line + n;
    p = skip_ws(p, end);

    const char *tag; size_t tagn;
    p = lex_tok(p, end, &tag, &tagn);
    if (tagn != 3 || strncmp(tag, "KEY", 3) != 0) return -1;

    const char *id; size_t idn;
    p = lex_tok(p, end, &id, &idn);
    if (idn == 0 || idn >= SEC_KEY_ID_MAX) return -1;
    for (size_t i = 0; i < idn; i++) if (!valid_id_char(id[i])) return -1;

    const char *al; size_t aln;
    p = lex_tok(p, end, &al, &aln);
    int algo = parse_algo(al, aln);
    if (algo == SEC_ALGO_NONE) return -1;

    const char *hk; size_t hkn;
    p = lex_tok(p, end, &hk, &hkn);
    if (hkn != SEC_KEY_BYTES * 2) return -1;

    /* Find a free slot (or replace existing entry with same id). */
    int slot = -1;
    for (int i = 0; i < SEC_TRUST_MAX; i++) {
        if (g_trust[i].in_use && idn == strlen(g_trust[i].id) &&
            strncmp(g_trust[i].id, id, idn) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < SEC_TRUST_MAX; i++) {
            if (!g_trust[i].in_use) { slot = i; break; }
        }
    }
    if (slot < 0) return -1;

    /* Need a NUL-terminated hex string for sec_from_hex's tail check.*/
    char hex_buf[SEC_KEY_BYTES * 2 + 1];
    memcpy(hex_buf, hk, hkn);
    hex_buf[hkn] = '\0';

    uint8_t raw[SEC_KEY_BYTES];
    if (sec_from_hex(hex_buf, SEC_KEY_BYTES, raw) != 0) return -1;

    memset(&g_trust[slot], 0, sizeof(g_trust[slot]));
    memcpy(g_trust[slot].id, id, idn);
    g_trust[slot].id[idn] = '\0';
    g_trust[slot].algo    = algo;
    memcpy(g_trust[slot].key, raw, SEC_KEY_BYTES);
    g_trust[slot].key_len = SEC_KEY_BYTES;
    g_trust[slot].in_use  = true;
    return 0;
}

int sig_trust_store_reload(void) {
    /* Wipe + repopulate in one go so a removed line in trust.db
     * actually causes the runtime entry to disappear. */
    memset(g_trust, 0, sizeof(g_trust));

    void *raw = 0; size_t sz = 0;
    int rc = vfs_read_all(SEC_TRUST_PATH, &raw, &sz);
    if (rc != VFS_OK) {
        /* No trust store -> empty (no signed packages can verify).
         * That's the safe state. Caller sees count==0. */
        g_trust_loaded = 1;
        return 0;
    }

    const char *p   = (const char *)raw;
    const char *end = p + sz;
    int loaded = 0, malformed = 0;
    while (p < end) {
        /* Find end-of-line. */
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        size_t ll = (size_t)(p - ls);
        if (p < end) p++;
        if (ll > 0 && ls[ll - 1] == '\r') ll--;

        /* Skip blanks and comments. */
        const char *tp = ls;
        while ((size_t)(tp - ls) < ll && (*tp == ' ' || *tp == '\t')) tp++;
        if ((size_t)(tp - ls) >= ll) continue;
        if (*tp == '#') continue;

        if (parse_trust_line(ls, ll) == 0) loaded++;
        else                               malformed++;
    }
    kfree(raw);

    g_trust_loaded = 1;
    if (malformed) {
        kprintf("[sec] trust.db: %d malformed entr%s ignored\n",
                malformed, malformed == 1 ? "y" : "ies");
    }
    kprintf("[sec] trust store loaded: %d key(s) from %s\n",
            loaded, SEC_TRUST_PATH);
    return 0;
}

int sig_trust_store_init(void) {
    return sig_trust_store_reload();
}

const struct sec_key *sig_trust_store_find(const char *id) {
    if (!id || !*id) return 0;
    if (!g_trust_loaded) sig_trust_store_init();
    for (int i = 0; i < SEC_TRUST_MAX; i++) {
        if (g_trust[i].in_use && strcmp(g_trust[i].id, id) == 0)
            return &g_trust[i];
    }
    return 0;
}

int sig_trust_store_count(void) {
    if (!g_trust_loaded) sig_trust_store_init();
    int n = 0;
    for (int i = 0; i < SEC_TRUST_MAX; i++) if (g_trust[i].in_use) n++;
    return n;
}

const struct sec_key *sig_trust_store_at(int idx) {
    if (!g_trust_loaded) sig_trust_store_init();
    int seen = 0;
    for (int i = 0; i < SEC_TRUST_MAX; i++) {
        if (!g_trust[i].in_use) continue;
        if (seen == idx) return &g_trust[i];
        seen++;
    }
    return 0;
}

int sig_verify_hmac(const char *id, const char *sig_hex,
                    const void *buf, size_t n) {
    const struct sec_key *k = sig_trust_store_find(id);
    if (!k) return -1;
    if (k->algo != SEC_ALGO_HMAC_SHA256) return -2;

    if (!sig_hex) return -3;
    /* Length must be exactly 64 hex chars; sec_from_hex's trailing
     * check enforces termination. */
    uint8_t expected[SHA256_DIGEST_LEN];
    if (sec_from_hex(sig_hex, SHA256_DIGEST_LEN, expected) != 0) return -3;

    uint8_t computed[SHA256_DIGEST_LEN];
    struct hmac_sha256_ctx h;
    hmac_sha256_init(&h, k->key, k->key_len);
    hmac_sha256_update(&h, buf, n);
    hmac_sha256_final(&h, computed);

    return sec_memeq_ct(expected, computed, SHA256_DIGEST_LEN) == 0 ? 1 : 0;
}

/* ===================================================================
 *  Boot self-test
 *
 *  Validates the SHA-256 + HMAC primitives against published test
 *  vectors. Called once from kmain. A regression panics rather than
 *  silently producing wrong digests downstream.
 * =================================================================== */

void sec_selftest(void);     /* declared in <tobyos/sec.h>? -- internal */

void sec_selftest(void) {
    /* SHA-256 of "abc" (FIPS 180-2 Appendix B.1) */
    static const uint8_t sha_abc[SHA256_DIGEST_LEN] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };
    uint8_t h[SHA256_DIGEST_LEN];
    sha256_buf("abc", 3, h);
    if (sec_memeq_ct(h, sha_abc, SHA256_DIGEST_LEN) != 0) {
        kpanic_self_test("sec: SHA-256(\"abc\") failed");
    }

    /* SHA-256 of "" = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    static const uint8_t sha_empty[SHA256_DIGEST_LEN] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
        0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
    };
    sha256_buf("", 0, h);
    if (sec_memeq_ct(h, sha_empty, SHA256_DIGEST_LEN) != 0) {
        kpanic_self_test("sec: SHA-256(\"\") failed");
    }

    /* HMAC-SHA-256 RFC 4231 test case 1:
     *   key  = 20 bytes of 0x0b
     *   data = "Hi There"
     *   tag  = b0344c61d8db38535ca8afceaf0bf12b...
     */
    static const uint8_t hmac_key1[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
    };
    static const uint8_t hmac_tag1[SHA256_DIGEST_LEN] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
        0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
        0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7,
    };
    struct hmac_sha256_ctx hc;
    hmac_sha256_init(&hc, hmac_key1, sizeof(hmac_key1));
    hmac_sha256_update(&hc, "Hi There", 8);
    hmac_sha256_final(&hc, h);
    if (sec_memeq_ct(h, hmac_tag1, SHA256_DIGEST_LEN) != 0) {
        kpanic_self_test("sec: HMAC-SHA256 RFC 4231 case 1 failed");
    }

    /* hex roundtrip + constant-time compare */
    char hex[SHA256_HEX_LEN + 1];
    sec_to_hex(sha_abc, SHA256_DIGEST_LEN, hex);
    uint8_t back[SHA256_DIGEST_LEN];
    if (sec_from_hex(hex, SHA256_DIGEST_LEN, back) != 0 ||
        sec_memeq_ct(back, sha_abc, SHA256_DIGEST_LEN) != 0) {
        kpanic_self_test("sec: hex roundtrip failed");
    }

    kprintf("[sec] M34 crypto self-test PASS "
            "(SHA-256, HMAC-SHA-256, hex/ct)\n");
}
