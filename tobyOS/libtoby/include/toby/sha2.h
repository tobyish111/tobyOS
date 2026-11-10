/* toby/sha2.h -- SHA-2 for userland.
 *
 * One implementation, two very different callers: the sha224sum/sha384sum
 * applets in user_coreutil, and /bin/ssh, which needs SHA-256 for the
 * curve25519-sha256 exchange hash and its key derivation. monocypher gives
 * userland X25519, Ed25519, SHA-512 and ChaCha20-Poly1305 but NOT SHA-256,
 * which is exactly the one the SSH transport is built on.
 *
 * SHA-224 is the SHA-256 core with a different IV and a truncated digest,
 * and SHA-384 is SHA-512's the same way -- which is why they are parameters
 * here rather than separate code.
 */
#ifndef TOBY_SHA2_H
#define TOBY_SHA2_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t      h[8];
    uint64_t      len;
    unsigned char buf[64];
    size_t        n;
} toby_sha256_ctx;

typedef struct {
    uint64_t      h[8];
    uint64_t      len;
    unsigned char buf[128];
    size_t        n;
} toby_sha512_ctx;

/* `is224` selects SHA-224's initial values and a 28-byte digest. */
void toby_sha256_init  (toby_sha256_ctx *c, int is224);
void toby_sha256_update(toby_sha256_ctx *c, const void *p, size_t n);
/* Writes 32 bytes (or 28 when initialised with is224). */
void toby_sha256_final (toby_sha256_ctx *c, unsigned char *out);

/* `is384` selects SHA-384's initial values and a 48-byte digest. */
void toby_sha512_init  (toby_sha512_ctx *c, int is384);
void toby_sha512_update(toby_sha512_ctx *c, const void *p, size_t n);
/* Writes 64 bytes (or 48 when initialised with is384). */
void toby_sha512_final (toby_sha512_ctx *c, unsigned char *out);

/* One-shot helpers. */
void toby_sha256(const void *p, size_t n, unsigned char out[32]);

#endif /* TOBY_SHA2_H */
