/* des.h -- single-block DES (ECB), for RFB "VNC Authentication".
 *
 * DES is obsolete as a cipher and is NOT used here as one. RFB security
 * type 2 specifies a DES challenge-response, so a VNC server that wants
 * to interoperate with stock clients (TigerVNC, RealVNC, Remmina, macOS
 * Screen Sharing) has to speak exactly this. Do not reach for this file
 * for anything else -- ssh_crypto.h has the modern primitives.
 *
 * VNC's quirk: the key bytes are consumed least-significant-bit first,
 * i.e. each byte is bit-reversed relative to textbook DES. That lives in
 * des_set_key_vnc() rather than being smeared through the caller.
 */
#ifndef TOBYOS_DES_H
#define TOBYOS_DES_H

#include <stdint.h>
#include <stdbool.h>

struct des_ctx {
    uint8_t subkey[16][48];   /* 16 rounds x 48 bits, one bit per byte */
};

/* Textbook DES key schedule: `key` is 8 bytes, MSB-first. */
void des_set_key(struct des_ctx *ctx, const uint8_t key[8]);

/* RFB key schedule: as above, but each key byte is bit-reversed first. */
void des_set_key_vnc(struct des_ctx *ctx, const uint8_t key[8]);

/* One 8-byte ECB block. in and out may alias. */
void des_encrypt_block(const struct des_ctx *ctx, const uint8_t in[8],
                       uint8_t out[8]);
void des_decrypt_block(const struct des_ctx *ctx, const uint8_t in[8],
                       uint8_t out[8]);

/* Known-answer test against published FIPS vectors. A silently wrong DES
 * would look exactly like "wrong password", so this is worth running. */
bool des_selftest(void);

#endif /* TOBYOS_DES_H */
