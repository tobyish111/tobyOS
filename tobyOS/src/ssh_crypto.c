/* ssh_crypto.c -- SSH-facing wrappers for Monocypher 4.0.2. */

#include <tobyos/ssh_crypto.h>
#include <tobyos/rng.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/klibc.h>

#include <monocypher.h>
#include <monocypher-ed25519.h>

#define SSH_KEY_DIR  "/data/ssh"
#define SSH_KEY_PATH "/data/ssh/ssh_host_ed25519_seed"

struct ssh_hostkey_file {
    char    magic[16];
    uint8_t seed[32];
};

static const char SSH_HOSTKEY_MAGIC[16] = {
    'T','O','B','Y','S','S','H','K','E','Y','1','\n',0,0,0,0
};

static uint8_t g_host_seed[SSH_CRYPTO_ED25519_PUBLIC_LEN];
static uint8_t g_host_secret[SSH_CRYPTO_ED25519_SECRET_LEN];
static uint8_t g_host_public[SSH_CRYPTO_ED25519_PUBLIC_LEN];
static bool    g_host_ready;

void ssh_crypto_wipe(void *buf, size_t n) {
    crypto_wipe(buf, n);
}

void ssh_crypto_x25519_public(uint8_t out_public[SSH_CRYPTO_X25519_KEY_LEN],
                              const uint8_t secret[SSH_CRYPTO_X25519_KEY_LEN]) {
    crypto_x25519_public_key(out_public, secret);
}

void ssh_crypto_x25519_shared(uint8_t out_shared[SSH_CRYPTO_X25519_KEY_LEN],
                              const uint8_t secret[SSH_CRYPTO_X25519_KEY_LEN],
                              const uint8_t peer_public[SSH_CRYPTO_X25519_KEY_LEN]) {
    crypto_x25519(out_shared, secret, peer_public);
}

void ssh_crypto_ed25519_keypair(
    uint8_t secret[SSH_CRYPTO_ED25519_SECRET_LEN],
    uint8_t public_key[SSH_CRYPTO_ED25519_PUBLIC_LEN],
    uint8_t seed[SSH_CRYPTO_ED25519_PUBLIC_LEN]) {
    crypto_ed25519_key_pair(secret, public_key, seed);
}

void ssh_crypto_ed25519_sign(
    uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN],
    const uint8_t secret[SSH_CRYPTO_ED25519_SECRET_LEN],
    const void *msg, size_t msg_len) {
    crypto_ed25519_sign(sig, secret, (const uint8_t *)msg, msg_len);
}

bool ssh_crypto_ed25519_verify(
    const uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN],
    const uint8_t public_key[SSH_CRYPTO_ED25519_PUBLIC_LEN],
    const void *msg, size_t msg_len) {
    return crypto_ed25519_check(sig, public_key,
                                (const uint8_t *)msg, msg_len) == 0;
}

void ssh_crypto_sha512(uint8_t out[SSH_CRYPTO_SHA512_LEN],
                       const void *msg, size_t msg_len) {
    crypto_sha512(out, (const uint8_t *)msg, msg_len);
}

void ssh_crypto_hmac_sha512(uint8_t out[SSH_CRYPTO_SHA512_LEN],
                            const void *key, size_t key_len,
                            const void *msg, size_t msg_len) {
    crypto_sha512_hmac(out, (const uint8_t *)key, key_len,
                       (const uint8_t *)msg, msg_len);
}

void ssh_crypto_chacha20_djb(uint8_t *cipher_text,
                             const uint8_t *plain_text, size_t text_len,
                             const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN],
                             const uint8_t nonce[8],
                             uint64_t counter) {
    crypto_chacha20_djb(cipher_text, plain_text, text_len,
                        key, nonce, counter);
}

void ssh_crypto_poly1305(uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN],
                         const void *msg, size_t msg_len,
                         const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN]) {
    crypto_poly1305(tag, (const uint8_t *)msg, msg_len, key);
}

void ssh_crypto_xchacha20poly1305_encrypt(
    uint8_t *cipher_text,
    uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN],
    const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[SSH_CRYPTO_XCHACHA_NONCE_LEN],
    const void *ad, size_t ad_len,
    const void *plain_text, size_t text_len) {
    crypto_aead_lock(cipher_text, tag, key, nonce,
                     (const uint8_t *)ad, ad_len,
                     (const uint8_t *)plain_text, text_len);
}

bool ssh_crypto_xchacha20poly1305_decrypt(
    uint8_t *plain_text,
    const uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN],
    const uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN],
    const uint8_t nonce[SSH_CRYPTO_XCHACHA_NONCE_LEN],
    const void *ad, size_t ad_len,
    const void *cipher_text, size_t text_len) {
    return crypto_aead_unlock(plain_text, tag, key, nonce,
                              (const uint8_t *)ad, ad_len,
                              (const uint8_t *)cipher_text, text_len) == 0;
}

static void ssh_hostkey_derive_from_seed(const uint8_t seed[32]) {
    memcpy(g_host_seed, seed, sizeof(g_host_seed));

    uint8_t seed_copy[32];
    memcpy(seed_copy, seed, sizeof(seed_copy));
    ssh_crypto_ed25519_keypair(g_host_secret, g_host_public, seed_copy);
    ssh_crypto_wipe(seed_copy, sizeof(seed_copy));
    g_host_ready = true;
}

static int ssh_hostkey_load(void) {
    void *raw = 0;
    size_t raw_sz = 0;
    int rc = vfs_read_all(SSH_KEY_PATH, &raw, &raw_sz);
    if (rc != VFS_OK) return rc;
    if (!raw || raw_sz != sizeof(struct ssh_hostkey_file)) {
        if (raw) kfree(raw);
        return VFS_ERR_INVAL;
    }

    struct ssh_hostkey_file *f = (struct ssh_hostkey_file *)raw;
    if (memcmp(f->magic, SSH_HOSTKEY_MAGIC, sizeof(f->magic)) != 0) {
        kfree(raw);
        return VFS_ERR_INVAL;
    }

    ssh_hostkey_derive_from_seed(f->seed);
    kfree(raw);
    kprintf("[ssh-crypto] loaded Ed25519 host key from %s\n", SSH_KEY_PATH);
    return VFS_OK;
}

static int ssh_hostkey_generate(void) {
    if (!rng_hardware_seeded()) {
        kprintf("[ssh-crypto] refusing to generate persistent host key: "
                "no hardware RNG seed available\n");
        return VFS_ERR_INVAL;
    }

    (void)vfs_mkdir(SSH_KEY_DIR);

    struct ssh_hostkey_file f;
    memcpy(f.magic, SSH_HOSTKEY_MAGIC, sizeof(f.magic));
    rng_fill(f.seed, sizeof(f.seed));

    ssh_hostkey_derive_from_seed(f.seed);
    int rc = vfs_write_all(SSH_KEY_PATH, &f, sizeof(f));
    if (rc == VFS_OK) {
        (void)vfs_chmod(SSH_KEY_PATH, 0600);
        kprintf("[ssh-crypto] generated Ed25519 host key at %s\n",
                SSH_KEY_PATH);
    } else {
        kprintf("[ssh-crypto] generated volatile Ed25519 host key; "
                "persist write failed: %s\n", vfs_strerror(rc));
    }

    ssh_crypto_wipe(&f, sizeof(f));
    return rc == VFS_OK ? VFS_OK : rc;
}

int ssh_hostkey_init(void) {
    if (g_host_ready) return 0;
    rng_init();

    int rc = ssh_hostkey_load();
    if (rc == VFS_OK) return 0;
    if (rc != VFS_ERR_NOENT) {
        kprintf("[ssh-crypto] host key load failed: %s; regenerating\n",
                vfs_strerror(rc));
    }

    rc = ssh_hostkey_generate();
    return g_host_ready ? 0 : rc;
}

bool ssh_hostkey_ready(void) {
    return g_host_ready;
}

const uint8_t *ssh_hostkey_public(void) {
    return g_host_ready ? g_host_public : 0;
}

void ssh_hostkey_sign(uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN],
                      const void *msg, size_t msg_len) {
    if (!g_host_ready || !sig) return;
    ssh_crypto_ed25519_sign(sig, g_host_secret, msg, msg_len);
}

static void expect_eq(const void *a, const void *b, size_t n,
                      const char *what) {
    if (memcmp(a, b, n) != 0) {
        kpanic_self_test(what);
    }
}

void ssh_crypto_selftest(void) {
    /* RFC 7748 X25519 test vector. */
    static const uint8_t alice_sk[32] = {
        0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,
        0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
        0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,
        0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a,
    };
    static const uint8_t alice_pk_expected[32] = {
        0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,
        0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
        0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,
        0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a,
    };
    static const uint8_t bob_pk[32] = {
        0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,
        0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,
        0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,
        0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f,
    };
    static const uint8_t shared_expected[32] = {
        0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,
        0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
        0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,
        0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42,
    };

    uint8_t pk[32], shared[32];
    ssh_crypto_x25519_public(pk, alice_sk);
    expect_eq(pk, alice_pk_expected, sizeof(pk),
              "ssh_crypto: X25519 public key vector failed");
    ssh_crypto_x25519_shared(shared, alice_sk, bob_pk);
    expect_eq(shared, shared_expected, sizeof(shared),
              "ssh_crypto: X25519 shared-secret vector failed");

    /* RFC 8032 Ed25519 test vector 1: empty message. */
    static const uint8_t ed_seed_vec[32] = {
        0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,
        0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,
        0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,
        0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60,
    };
    static const uint8_t ed_pk_expected[32] = {
        0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
        0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
        0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
        0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a,
    };
    static const uint8_t ed_sig_expected[64] = {
        0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,
        0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,
        0x84,0x87,0x7f,0x1e,0xb8,0xe5,0xd9,0x74,
        0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
        0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,
        0xc6,0x1e,0x39,0x70,0x1c,0xf9,0xb4,0x6b,
        0xd2,0x5b,0xf5,0xf0,0x59,0x5b,0xbe,0x24,
        0x65,0x51,0x41,0x43,0x8e,0x7a,0x10,0x0b,
    };
    uint8_t ed_sk[64], ed_pk[32], sig[64];
    uint8_t ed_seed[32];
    memcpy(ed_seed, ed_seed_vec, sizeof(ed_seed));
    ssh_crypto_ed25519_keypair(ed_sk, ed_pk, ed_seed);
    expect_eq(ed_pk, ed_pk_expected, sizeof(ed_pk),
              "ssh_crypto: Ed25519 public key vector failed");
    ssh_crypto_ed25519_sign(sig, ed_sk, "", 0);
    expect_eq(sig, ed_sig_expected, sizeof(sig),
              "ssh_crypto: Ed25519 signature vector failed");
    if (!ssh_crypto_ed25519_verify(sig, ed_pk, "", 0)) {
        kpanic_self_test("ssh_crypto: Ed25519 verify failed");
    }

    uint8_t key[SSH_CRYPTO_CHACHA_KEY_LEN];
    uint8_t nonce[SSH_CRYPTO_XCHACHA_NONCE_LEN];
    uint8_t ct[16], pt[16], tag[SSH_CRYPTO_POLY1305_TAG_LEN];
    const uint8_t msg[16] = {
        't','o','b','y','O','S','-','s','s','h','-','c','r','y','p','t',
    };
    const uint8_t ad[4] = { 's','s','h','2' };
    for (size_t i = 0; i < sizeof(key); i++) key[i] = (uint8_t)i;
    for (size_t i = 0; i < sizeof(nonce); i++) nonce[i] = (uint8_t)(0xa0u + i);
    ssh_crypto_xchacha20poly1305_encrypt(ct, tag, key, nonce,
                                          ad, sizeof(ad),
                                          msg, sizeof(msg));
    memset(pt, 0, sizeof(pt));
    if (!ssh_crypto_xchacha20poly1305_decrypt(pt, tag, key, nonce,
                                              ad, sizeof(ad),
                                              ct, sizeof(ct))) {
        kpanic_self_test("ssh_crypto: AEAD decrypt failed");
    }
    expect_eq(pt, msg, sizeof(msg),
              "ssh_crypto: AEAD roundtrip mismatch");

    ssh_crypto_wipe(ed_sk, sizeof(ed_sk));
    ssh_crypto_wipe(shared, sizeof(shared));
    kprintf("[ssh-crypto] Monocypher self-test PASS "
            "(X25519, Ed25519, XChaCha20-Poly1305)\n");
}
