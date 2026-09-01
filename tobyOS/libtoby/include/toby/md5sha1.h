/* md5sha1.h -- MD5 and SHA-1, for md5sum and sha1sum.
 *
 * NEITHER IS A SECURITY PRIMITIVE ANY MORE. They exist to read the
 * checksum files the world already has. Use SHA-256 (toby/sha2.h) or
 * Ed25519 for anything that needs to be trusted.
 */
#ifndef TOBY_MD5SHA1_H
#define TOBY_MD5SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[4];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} toby_md5_ctx;

void toby_md5_init  (toby_md5_ctx *c);
void toby_md5_update(toby_md5_ctx *c, const void *data, size_t n);
void toby_md5_final (toby_md5_ctx *c, uint8_t out[16]);

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} toby_sha1_ctx;

void toby_sha1_init  (toby_sha1_ctx *c);
void toby_sha1_update(toby_sha1_ctx *c, const void *data, size_t n);
void toby_sha1_final (toby_sha1_ctx *c, uint8_t out[20]);

#endif /* TOBY_MD5SHA1_H */
