/* Shared ML-KEM/Kyber-768 self-test core (freestanding-safe; also compiled
 * for the HOST to produce reference checksums -- see host_main.c).
 *
 * Purpose (ledger slice 36/38): chrome's https fails because BoringSSL's
 * ML-KEM path computes a wrong result on tobyOS. ML-KEM decapsulation ends in
 * the Fujisaki-Okamoto re-encryption equality check, so ANY single-bit error
 * across thousands of mod-3329/NTT/Keccak ops yields a pseudo-random shared
 * secret. This test runs the SAME computational shape (kyber768 reference:
 * NTT, Keccak, FO decaps) in ISOLATION on tobyOS:
 *   - self-consistency: keygen -> encaps -> decaps must give ss1 == ss2;
 *   - implicit rejection: decaps of a corrupted ct must give ss != ss1
 *     (exercises the FO re-encrypt+compare branch chrome dies in);
 *   - determinism: a chained FNV-1a checksum over (pk, ct, ss) of every
 *     round, comparable against the SAME code run on the host. A checksum
 *     mismatch with PASSing self-consistency = deterministic computational
 *     divergence (TCG or kernel); a self-consistency FAIL = transient
 *     corruption (context switch / paging / FPU-state class).
 */
#include <stdint.h>
#include <string.h>
#include "kem.h"
#include "params.h"

/* ---- deterministic randombytes (replaces randombytes.c) ---- */
static uint64_t g_rng_state = 1;

void randombytes(uint8_t *out, size_t outlen);
void randombytes(uint8_t *out, size_t outlen) {
    /* xorshift64* -- deterministic, seedable, portable */
    for (size_t i = 0; i < outlen; i++) {
        g_rng_state ^= g_rng_state >> 12;
        g_rng_state ^= g_rng_state << 25;
        g_rng_state ^= g_rng_state >> 27;
        out[i] = (uint8_t)((g_rng_state * 0x2545F4914F6CDD1DULL) >> 56);
    }
}

/* ---- FNV-1a 64 ---- */
static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

struct mlkem_result {
    uint64_t checksum;      /* chained over all rounds */
    int      fail_round;    /* round of first failure, -1 if none */
};

/* Returns 0 = PASS, 1 = self-consistency FAIL (ss1 != ss2),
 *         4 = implicit-rejection FAIL (corrupted ct gave the same ss). */
int mlkem_selftest(uint64_t seed, int rounds, struct mlkem_result *res);
int mlkem_selftest(uint64_t seed, int rounds, struct mlkem_result *res) {
    static uint8_t pk[CRYPTO_PUBLICKEYBYTES];
    static uint8_t sk[CRYPTO_SECRETKEYBYTES];
    static uint8_t ct[CRYPTO_CIPHERTEXTBYTES];
    uint8_t ss1[CRYPTO_BYTES], ss2[CRYPTO_BYTES], ss3[CRYPTO_BYTES];
    uint64_t h = 0xcbf29ce484222325ULL;

    g_rng_state = seed ? seed : 1;
    res->checksum = 0; res->fail_round = -1;

    for (int r = 0; r < rounds; r++) {
        crypto_kem_keypair(pk, sk);
        crypto_kem_enc(ct, ss1, pk);
        crypto_kem_dec(ss2, ct, sk);
        if (memcmp(ss1, ss2, CRYPTO_BYTES) != 0) {
            res->fail_round = r; res->checksum = h;
            return 1;
        }
        /* Implicit rejection: flip one ciphertext byte; decaps must yield a
         * DIFFERENT secret (this walks the exact FO compare that amplifies
         * any upstream computational error into chrome's handshake failure). */
        ct[r % CRYPTO_CIPHERTEXTBYTES] ^= 0x5a;
        crypto_kem_dec(ss3, ct, sk);
        ct[r % CRYPTO_CIPHERTEXTBYTES] ^= 0x5a;
        if (memcmp(ss3, ss1, CRYPTO_BYTES) == 0) {
            res->fail_round = r; res->checksum = h;
            return 4;
        }
        h = fnv1a(h, pk,  sizeof pk);
        h = fnv1a(h, ct,  sizeof ct);
        h = fnv1a(h, ss1, sizeof ss1);
        h = fnv1a(h, ss3, sizeof ss3);
    }
    res->checksum = h;
    return 0;
}
