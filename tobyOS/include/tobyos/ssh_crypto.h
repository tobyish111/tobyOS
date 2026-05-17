/* ssh_crypto.h -- SSH-facing crypto wrappers around vendored Monocypher.
 *
 * Keep direct third_party calls behind this tiny boundary so ssh.c can
 * speak in SSH algorithm terms instead of Monocypher API details.
 */

#ifndef TOBYOS_SSH_CRYPTO_H
#define TOBYOS_SSH_CRYPTO_H

#include <tobyos/types.h>

#define SSH_CRYPTO_X25519_KEY_LEN       32u
#define SSH_CRYPTO_ED25519_PUBLIC_LEN   32u
#define SSH_CRYPTO_ED25519_SECRET_LEN   64u
#define SSH_CRYPTO_ED25519_SIG_LEN      64u
#define SSH_CRYPTO_SHA512_LEN           64u
#define SSH_CRYPTO_CHACHA_KEY_LEN       32u
#define SSH_CRYPTO_CHACHAPOLY_KEY_LEN   64u
#define SSH_CRYPTO_XCHACHA_NONCE_LEN    24u
#define SSH_CRYPTO_POLY1305_TAG_LEN     16u

void ssh_crypto_selftest(void);

void ssh_crypto_wipe(void *buf, size_t n);

void ssh_crypto_x25519_public(uint8_t out_public[SSH_CRYPTO_X25519_KEY_LEN],
                              const uint8_t secret[SSH_CRYPTO_X25519_KEY_LEN]);
void ssh_crypto_x25519_shared(uint8_t out_shared[SSH_CRYPTO_X25519_KEY_LEN],
                              const uint8_t secret[SSH_CRYPTO_X25519_KEY_LEN],
                              const uint8_t peer_public[SSH_CRYPTO_X25519_KEY_LEN]);

void ssh_crypto_ed25519_keypair(
    uint8_t secret[SSH_CRYPTO_ED25519_SECRET_LEN],
    uint8_t public_key[SSH_CRYPTO_ED25519_PUBLIC_LEN],
    uint8_t seed[SSH_CRYPTO_ED25519_PUBLIC_LEN]);
void ssh_crypto_ed25519_sign(
    uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN],
    const uint8_t secret[SSH_CRYPTO_ED25519_SECRET_LEN],
    const void *msg, size_t msg_len);
bool ssh_crypto_ed25519_verify(
    const uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN],
    const uint8_t public_key[SSH_CRYPTO_ED25519_PUBLIC_LEN],
    const void *msg, size_t msg_len);

void ssh_crypto_sha512(uint8_t out[SSH_CRYPTO_SHA512_LEN],
                       const void *msg, size_t msg_len);
void ssh_crypto_hmac_sha512(uint8_t out[SSH_CRYPTO_SHA512_LEN],
                            const void *key, size_t key_len,
                            const void *msg, size_t msg_len);

void ssh_crypto_chacha20_djb(uint8_t *cipher_text,
                             const uint8_t *plain_text, size_t text_len,
                             const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN],
                             const uint8_t nonce[8],
                             uint64_t counter);
void ssh_crypto_poly1305(uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN],
                         const void *msg, size_t msg_len,
                         const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN]);

void ssh_crypto_xchacha20poly1305_encrypt(
    uint8_t *cipher_text,
    uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN],
    const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[SSH_CRYPTO_XCHACHA_NONCE_LEN],
    const void *ad, size_t ad_len,
    const void *plain_text, size_t text_len);
bool ssh_crypto_xchacha20poly1305_decrypt(
    uint8_t *plain_text,
    const uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN],
    const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[SSH_CRYPTO_XCHACHA_NONCE_LEN],
    const void *ad, size_t ad_len,
    const void *cipher_text, size_t text_len);

int  ssh_hostkey_init(void);
bool ssh_hostkey_ready(void);
const uint8_t *ssh_hostkey_public(void);
void ssh_hostkey_sign(uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN],
                      const void *msg, size_t msg_len);

#endif /* TOBYOS_SSH_CRYPTO_H */
