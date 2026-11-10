/* shacrypt.c -- checking a SHA-crypt password hash, "$5$" (SHA-256) and
 * "$6$" (SHA-512), as glibc's crypt makes them (finding 213).
 *
 * chpasswd -e stores a hash made elsewhere as it is given, and the hash a
 * Linux system makes is one of these: without a way to check one, an
 * account carried over with its hash could never log in. The algorithm is
 * the published one (Drepper, "Unix crypt using SHA-256 and SHA-512"):
 *
 *   salt: after "$id$" and an optional "rounds=N$", up to 16 characters
 *         to the next '$'; rounds 5000 by default, held to 1000..999999999
 *   B = H(P S P)
 *   A = H(P S, then B's bytes for P's length, then for each bit of P's
 *         length from the lowest: B if it is 1, P if it is 0)
 *   DP = H(P repeated |P| times), the P-string its first |P| bytes
 *   DS = H(S repeated 16 + A[0] times), the S-string its first |S| bytes
 *   C = A; each round i: H(P-string if i is odd else C; S-string unless
 *         3 divides i; P-string unless 7 divides i; C if i is odd else
 *         P-string)
 *   the result: C's bytes in a fixed order, six bits a character from
 *         "./0-9A-Za-z", the low bits first.
 *
 * Compared in constant time. Any other "$id$" is not checked here. */

#include <tobyos/types.h>
#include <tobyos/klibc.h>
#include <tobyos/heap.h>
#include <tobyos/sec.h>
#include <monocypher-ed25519.h>

bool shacrypt_check(const char *password, const char *stored);

struct sc_h {
    int len;                                    /* 32 or 64 */
    union { struct sha256_ctx s256; crypto_sha512_ctx s512; } c;
};

static void sc_init(struct sc_h *h) {
    if (h->len == 32) sha256_init(&h->c.s256);
    else crypto_sha512_init(&h->c.s512);
}
static void sc_add(struct sc_h *h, const void *p, size_t n) {
    if (h->len == 32) sha256_update(&h->c.s256, p, n);
    else crypto_sha512_update(&h->c.s512, p, n);
}
static void sc_end(struct sc_h *h, uint8_t *out) {
    if (h->len == 32) sha256_final(&h->c.s256, out);
    else crypto_sha512_final(&h->c.s512, out);
}

static const char sc_b64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* the byte order the result is written in, three bytes a group (Drepper's
 * tables); the last group is the short one */
static const uint8_t sc_order256[] = {
    0, 10, 20,  21, 1, 11,  12, 22, 2,  3, 13, 23,  24, 4, 14,
    15, 25, 5,  6, 16, 26,  27, 7, 17,  18, 28, 8,  9, 19, 29,
};
static const uint8_t sc_order512[] = {
    0, 21, 42,  22, 43, 1,  44, 2, 23,  3, 24, 45,  25, 46, 4,
    47, 5, 26,  6, 27, 48,  28, 49, 7,  50, 8, 29,  9, 30, 51,
    31, 52, 10, 53, 11, 32, 12, 33, 54, 34, 55, 13, 56, 14, 35,
    15, 36, 57, 37, 58, 16, 59, 17, 38, 18, 39, 60, 40, 61, 19,
    62, 20, 41,
};

static size_t sc_put(char *out, size_t o, uint32_t w, int n) {
    while (n--) { out[o++] = sc_b64[w & 63]; w >>= 6; }
    return o;
}

bool shacrypt_check(const char *password, const char *stored) {
    if (!password || !stored || stored[0] != '$' || stored[2] != '$') return false;
    int dlen = stored[1] == '5' ? 32 : stored[1] == '6' ? 64 : 0;
    if (!dlen) return false;
    const char *p = stored + 3;
    uint32_t rounds = 5000;
    if (strncmp(p, "rounds=", 7) == 0) {
        const char *q = p + 7;
        uint64_t r = 0;
        if (*q < '0' || *q > '9') return false;
        while (*q >= '0' && *q <= '9') {
            r = r * 10 + (uint64_t)(*q++ - '0');
            if (r > 999999999u) r = 999999999u;
        }
        if (*q != '$') return false;
        rounds = r < 1000 ? 1000 : (uint32_t)r;
        p = q + 1;
    }
    const char *salt = p;
    size_t sl = 0;
    while (salt[sl] && salt[sl] != '$' && sl < 16) sl++;
    const char *want = salt + sl;
    while (*want && *want != '$') want++;      /* a longer salt: the rest */
    if (*want != '$') return false;
    want++;
    size_t pl = strlen(password);

    struct sc_h h;
    h.len = dlen;
    uint8_t b[64], a[64], dp[64], ds[64], c[64];
    sc_init(&h); sc_add(&h, password, pl); sc_add(&h, salt, sl); sc_add(&h, password, pl);
    sc_end(&h, b);
    sc_init(&h); sc_add(&h, password, pl); sc_add(&h, salt, sl);
    size_t k;
    for (k = pl; k > (size_t)dlen; k -= (size_t)dlen) sc_add(&h, b, (size_t)dlen);
    sc_add(&h, b, k);
    for (k = pl; k; k >>= 1) {
        if (k & 1) sc_add(&h, b, (size_t)dlen);
        else sc_add(&h, password, pl);
    }
    sc_end(&h, a);
    sc_init(&h);
    for (k = 0; k < pl; k++) sc_add(&h, password, pl);
    sc_end(&h, dp);
    sc_init(&h);
    for (k = 0; k < 16u + a[0]; k++) sc_add(&h, salt, sl);
    sc_end(&h, ds);
    /* the P- and S-strings, |P| and |S| bytes of DP and DS repeated */
    uint8_t *ps = (uint8_t *)kmalloc(pl ? pl : 1);
    if (!ps) return false;
    for (k = 0; k < pl; k++) ps[k] = dp[k % (size_t)dlen];
    uint8_t ss[16];
    for (k = 0; k < sl; k++) ss[k] = ds[k % (size_t)dlen];
    memcpy(c, a, (size_t)dlen);
    for (uint32_t i = 0; i < rounds; i++) {
        sc_init(&h);
        if (i & 1) sc_add(&h, ps, pl); else sc_add(&h, c, (size_t)dlen);
        if (i % 3) sc_add(&h, ss, sl);
        if (i % 7) sc_add(&h, ps, pl);
        if (i & 1) sc_add(&h, c, (size_t)dlen); else sc_add(&h, ps, pl);
        sc_end(&h, c);
    }
    kfree(ps);

    char got[96];
    size_t o = 0;
    const uint8_t *ord = dlen == 32 ? sc_order256 : sc_order512;
    size_t groups = dlen == 32 ? sizeof sc_order256 / 3 : sizeof sc_order512 / 3;
    for (size_t g = 0; g < groups; g++)
        o = sc_put(got, o, (uint32_t)c[ord[3 * g]] << 16 | (uint32_t)c[ord[3 * g + 1]] << 8 |
                           c[ord[3 * g + 2]], 4);
    if (dlen == 32) o = sc_put(got, o, (uint32_t)c[31] << 8 | c[30], 3);
    else            o = sc_put(got, o, c[63], 2);
    got[o] = 0;
    crypto_wipe(c, sizeof c);
    crypto_wipe(a, sizeof a);
    crypto_wipe(dp, sizeof dp);
    /* the whole of the stored digest, in constant time */
    size_t wl = strlen(want);
    unsigned diff = (unsigned)(wl ^ o);
    for (k = 0; k < o; k++) diff |= (unsigned)(got[k] ^ (k < wl ? want[k] : 0));
    return diff == 0;
}
