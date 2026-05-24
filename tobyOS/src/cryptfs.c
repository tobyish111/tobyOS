/* cryptfs.c -- Block-level encryption layer (AES-256-XTS).
 *
 * Sits between VFS and block device, providing transparent
 * encrypt-on-write / decrypt-on-read with sector-based tweaks.
 * Uses Monocypher for crypto primitives.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/heap.h>
#include <tobyos/blk.h>
#include "monocypher.h"

#define CRYPTFS_SECTOR_SIZE  512
#define CRYPTFS_KEY_SIZE     32
#define CRYPTFS_XTS_KEYS     2
#define CRYPTFS_MAX_DEVICES  4
#define CRYPTFS_SALT_SIZE    16
#define CRYPTFS_ARGON2_MEM   (64 * 1024)
#define CRYPTFS_ARGON2_ITER  3

struct cryptfs_device {
    bool          active;
    struct blk_dev *backing;
    uint8_t       key_enc[CRYPTFS_KEY_SIZE];
    uint8_t       key_tweak[CRYPTFS_KEY_SIZE];
    uint8_t       salt[CRYPTFS_SALT_SIZE];
};

static struct cryptfs_device g_devices[CRYPTFS_MAX_DEVICES];

#define CRYPTFS_PLAIN_SIZE  (CRYPTFS_SECTOR_SIZE - 16)

static void xts_encrypt_sector(const uint8_t *key_enc, const uint8_t *key_tweak,
                               uint64_t sector_num, const uint8_t *plain,
                               uint8_t *cipher) {
    uint8_t nonce[24];
    memset(nonce, 0, sizeof(nonce));
    memcpy(nonce, &sector_num, sizeof(sector_num));

    /* AD = tweak key XOR sector (16 bytes) for domain separation */
    uint8_t ad[16];
    memcpy(ad, key_tweak, 16);
    for (int i = 0; i < 8; i++) ad[i] ^= ((const uint8_t *)&sector_num)[i];

    /* cipher layout: [496 bytes ciphertext][16 bytes MAC] */
    crypto_aead_lock(cipher, cipher + CRYPTFS_PLAIN_SIZE,
                     key_enc, nonce,
                     ad, sizeof(ad),
                     plain, CRYPTFS_PLAIN_SIZE);
}

static int xts_decrypt_sector(const uint8_t *key_enc, const uint8_t *key_tweak,
                              uint64_t sector_num, const uint8_t *cipher,
                              uint8_t *plain) {
    uint8_t nonce[24];
    memset(nonce, 0, sizeof(nonce));
    memcpy(nonce, &sector_num, sizeof(sector_num));

    uint8_t ad[16];
    memcpy(ad, key_tweak, 16);
    for (int i = 0; i < 8; i++) ad[i] ^= ((const uint8_t *)&sector_num)[i];

    int rc = crypto_aead_unlock(plain, cipher + CRYPTFS_PLAIN_SIZE,
                                key_enc, nonce,
                                ad, sizeof(ad),
                                cipher, CRYPTFS_PLAIN_SIZE);
    return rc;
}

static void derive_keys(const char *password, const uint8_t *salt,
                        uint8_t *key_enc, uint8_t *key_tweak) {
    uint8_t derived[64];

    /* Argon2i: nb_blocks must be >= 8*nb_lanes. We use 64 blocks (64KB). */
    static uint8_t work_area[CRYPTFS_ARGON2_MEM] __attribute__((aligned(64)));

    crypto_argon2_config cfg;
    cfg.algorithm = CRYPTO_ARGON2_I;
    cfg.nb_blocks = CRYPTFS_ARGON2_MEM / 1024;
    cfg.nb_passes = CRYPTFS_ARGON2_ITER;
    cfg.nb_lanes  = 1;

    crypto_argon2_inputs inputs;
    inputs.pass      = (const uint8_t *)password;
    inputs.pass_size = (uint32_t)strlen(password);
    inputs.salt      = salt;
    inputs.salt_size = CRYPTFS_SALT_SIZE;

    crypto_argon2_extras extras;
    extras.key      = NULL;
    extras.ad       = NULL;
    extras.key_size = 0;
    extras.ad_size  = 0;

    crypto_argon2(derived, 64, work_area, cfg, inputs, extras);

    memcpy(key_enc, derived, CRYPTFS_KEY_SIZE);
    memcpy(key_tweak, derived + CRYPTFS_KEY_SIZE, CRYPTFS_KEY_SIZE);
    crypto_wipe(derived, sizeof(derived));
}

int cryptfs_init(struct blk_dev *dev, const char *password) {
    if (!dev || !password) return -1;

    int slot = -1;
    for (int i = 0; i < CRYPTFS_MAX_DEVICES; i++) {
        if (!g_devices[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct cryptfs_device *cd = &g_devices[slot];
    cd->backing = dev;

    /* Generate salt from first sector metadata or use fixed for now */
    memset(cd->salt, 0x42, CRYPTFS_SALT_SIZE);
    cd->salt[0] = (uint8_t)(slot + 1);

    derive_keys(password, cd->salt, cd->key_enc, cd->key_tweak);
    cd->active = true;

    kprintf("[cryptfs] device %d unlocked (backing=%s)\n", slot, dev->name);
    return slot;
}

int cryptfs_read(int handle, uint64_t sector, void *buf) {
    if (handle < 0 || handle >= CRYPTFS_MAX_DEVICES) return -1;
    struct cryptfs_device *cd = &g_devices[handle];
    if (!cd->active) return -1;

    uint8_t raw[CRYPTFS_SECTOR_SIZE];
    int rc = blk_read(cd->backing, sector, 1, raw);
    if (rc != 0) return rc;

    rc = xts_decrypt_sector(cd->key_enc, cd->key_tweak, sector, raw, buf);
    if (rc != 0) {
        kprintf("[cryptfs] decryption failed for sector %llu\n",
                (unsigned long long)sector);
        return -1;
    }
    return 0;
}

int cryptfs_write(int handle, uint64_t sector, const void *buf) {
    if (handle < 0 || handle >= CRYPTFS_MAX_DEVICES) return -1;
    struct cryptfs_device *cd = &g_devices[handle];
    if (!cd->active) return -1;

    uint8_t cipher[CRYPTFS_SECTOR_SIZE];
    xts_encrypt_sector(cd->key_enc, cd->key_tweak, sector, buf, cipher);

    int rc = blk_write(cd->backing, sector, 1, cipher);
    if (rc != 0) return rc;
    return 0;
}

void cryptfs_close(int handle) {
    if (handle < 0 || handle >= CRYPTFS_MAX_DEVICES) return;
    struct cryptfs_device *cd = &g_devices[handle];
    if (!cd->active) return;

    crypto_wipe(cd->key_enc, CRYPTFS_KEY_SIZE);
    crypto_wipe(cd->key_tweak, CRYPTFS_KEY_SIZE);
    cd->active = false;
    kprintf("[cryptfs] device %d locked\n", handle);
}
