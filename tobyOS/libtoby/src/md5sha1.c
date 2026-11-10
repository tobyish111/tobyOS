/* md5sha1.c -- MD5 (RFC 1321) and SHA-1 (RFC 3174).
 *
 * libtoby had SHA-256/384/512 (sha2.c) and BLAKE2b, but neither MD5 nor
 * SHA-1, so md5sum and sha1sum -- which every distro ships and which every
 * download-verification script reaches for -- could not exist natively.
 *
 * NEITHER IS A SECURITY PRIMITIVE ANY MORE. MD5 has practical collisions
 * and SHA-1 has had one since 2017; they are here to read the checksum
 * files the world already has, not to protect anything. Nothing in this
 * tree should authenticate with either -- use SHA-256 (sha2.c) or the
 * Ed25519 in monocypher.
 */

#include <toby/md5sha1.h>

#include <string.h>

/* ------------------------------------------------------------------ MD5 */

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static const uint32_t md5_K[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,
    0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,
    0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,
    0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,
    0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,
    0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,
    0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,
    0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,
    0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
};
static const int md5_S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static void md5_block(toby_md5_ctx *c, const uint8_t *p) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++)                 /* MD5 is little-endian */
        M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);

    uint32_t a = c->h[0], b = c->h[1], d0 = c->h[2], d = c->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & d0) | (~b & d);          g = i; }
        else if (i < 32) { f = (d & b) | (~d & d0);          g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ d0 ^ d;                   g = (3*i + 5) & 15; }
        else             { f = d0 ^ (b | ~d);                g = (7*i) & 15; }
        uint32_t tmp = d;
        d = d0;
        d0 = b;
        b = b + rol32(a + f + md5_K[i] + M[g], md5_S[i]);
        a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d0; c->h[3] += d;
}

void toby_md5_init(toby_md5_ctx *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xefcdab89u;
    c->h[2] = 0x98badcfeu; c->h[3] = 0x10325476u;
    c->len = 0; c->n = 0;
}

void toby_md5_update(toby_md5_ctx *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += (uint64_t)n;
    while (n) {
        size_t take = 64 - c->n;
        if (take > n) take = n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { md5_block(c, c->buf); c->n = 0; }
    }
}

void toby_md5_final(toby_md5_ctx *c, uint8_t out[16]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    toby_md5_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->n != 56) toby_md5_update(c, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (8 * i));
    toby_md5_update(c, lenb, 8);
    for (int i = 0; i < 4; i++) {
        out[i*4+0] = (uint8_t)(c->h[i]);
        out[i*4+1] = (uint8_t)(c->h[i] >> 8);
        out[i*4+2] = (uint8_t)(c->h[i] >> 16);
        out[i*4+3] = (uint8_t)(c->h[i] >> 24);
    }
}

/* ---------------------------------------------------------------- SHA-1 */

static void sha1_block(toby_sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)                 /* SHA-1 is big-endian */
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8)  | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->h[0], b = c->h[1], d0 = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & d0) | (~b & d);        k = 0x5a827999u; }
        else if (i < 40) { f = b ^ d0 ^ d;                 k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & d0) | (b & d) | (d0 & d); k = 0x8f1bbcdcu; }
        else             { f = b ^ d0 ^ d;                 k = 0xca62c1d6u; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = d0; d0 = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d0; c->h[3] += d; c->h[4] += e;
}

void toby_sha1_init(toby_sha1_ctx *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xefcdab89u; c->h[2] = 0x98badcfeu;
    c->h[3] = 0x10325476u; c->h[4] = 0xc3d2e1f0u;
    c->len = 0; c->n = 0;
}

void toby_sha1_update(toby_sha1_ctx *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += (uint64_t)n;
    while (n) {
        size_t take = 64 - c->n;
        if (take > n) take = n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

void toby_sha1_final(toby_sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    toby_sha1_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->n != 56) toby_sha1_update(c, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    toby_sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4+0] = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}
