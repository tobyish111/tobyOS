/* sec.h -- Milestone 34 security primitives.
 *
 * Tiny, well-contained crypto + integrity layer used by the package
 * manager and other security-relevant kernel subsystems. Everything
 * here is deliberately self-contained:
 *
 *   - No dynamic allocation in the hash core (caller-owned context)
 *   - No platform-specific intrinsics
 *   - No kernel locks (callers serialise per-context)
 *
 * What lives here:
 *
 *   SHA-256       -- M34A package + file integrity hashing.
 *   HMAC-SHA-256  -- M34C symmetric-key package signing groundwork.
 *                    A clear TODO documents future migration to a
 *                    public-key scheme (e.g. Ed25519); the trust-store
 *                    file format and verifier plumbing are designed
 *                    so that scheme can be added without a wire-format
 *                    break (each KEY entry already names its algorithm).
 *   hex helpers   -- printable hex <-> raw byte conversion.
 *   trust store   -- text-file trust DB read from /system/keys/trust.db,
 *                    consulted by sig_verify_package().
 *
 * NOTE: HMAC-SHA-256 with a SHARED key only authenticates packages
 * built by parties that already share that key with the OS. It is NOT
 * equivalent to a real digital signature. We use it as a pragmatic
 * stepping stone to validate the format + verification flow with a
 * single small auditable primitive; the trust-store file format
 * already carries an algorithm tag, so swapping in Ed25519 later is a
 * verifier-side change only.
 */

#ifndef TOBYOS_SEC_H
#define TOBYOS_SEC_H

#include <tobyos/types.h>

/* ---- SHA-256 ------------------------------------------------------ */

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN    64    /* 2 hex chars per byte, no NUL       */

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint32_t datalen;           /* bytes buffered in `data`           */
    uint8_t  data[SHA256_BLOCK_SIZE];
};

void sha256_init  (struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *buf, size_t n);
/* Writes SHA256_DIGEST_LEN raw bytes to out. */
void sha256_final (struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience. */
void sha256_buf(const void *buf, size_t n, uint8_t out[SHA256_DIGEST_LEN]);

/* ---- HMAC-SHA-256 ------------------------------------------------- */

struct hmac_sha256_ctx {
    struct sha256_ctx inner;
    uint8_t k_opad[SHA256_BLOCK_SIZE];
};

void hmac_sha256_init  (struct hmac_sha256_ctx *c,
                        const void *key, size_t key_len);
void hmac_sha256_update(struct hmac_sha256_ctx *c,
                        const void *buf, size_t n);
void hmac_sha256_final (struct hmac_sha256_ctx *c,
                        uint8_t out[SHA256_DIGEST_LEN]);

/* ---- hex helpers -------------------------------------------------- */

/* Lowercase. Writes exactly 2*n chars + NUL into `out`; caller must
 * size out to at least 2*n + 1. */
void sec_to_hex(const uint8_t *bytes, size_t n, char *out);

/* Returns 0 on success and writes exactly `n` raw bytes into `out`.
 * Returns -1 if `hex` isn't 2*n lowercase or uppercase hex chars (no
 * separators, no whitespace). Tolerant of either case. */
int  sec_from_hex(const char *hex, size_t n, uint8_t *out);

/* Constant-time memcmp. Returns 0 iff a==b for `n` bytes. Used to
 * compare digests / signatures without leaking timing info. */
int  sec_memeq_ct(const void *a, const void *b, size_t n);

/* ---- trust store -------------------------------------------------- *
 *
 * On-disk format (/system/keys/trust.db, one entry per line):
 *
 *     KEY <id> <algo> <hex-key>
 *
 * Where:
 *   id     -- short identifier, no spaces, [A-Za-z0-9._-]+, <= 31 chars
 *   algo   -- "HMAC-SHA256" today; future "ED25519" etc. accepted as
 *             unknown (lookup returns NOT_FOUND so verification fails
 *             closed if a package references an algo we don't impl).
 *   hex    -- raw key bytes encoded as lowercase hex; current schema
 *             pins HMAC-SHA256 keys to 32 bytes (64 hex chars).
 *
 * Lines starting with '#' are comments. Blank lines are ignored.
 *
 * The store is loaded lazily on first use AND whenever
 * sig_trust_store_reload() is called (after writes by the package
 * manager). It is also re-loaded after a sysprot privileged write to
 * /system/keys/trust.db. */

#define SEC_KEY_ID_MAX  32
#define SEC_KEY_BYTES   32      /* HMAC-SHA-256 key length            */
#define SEC_TRUST_PATH  "/system/keys/trust.db"

struct sec_key {
    char    id[SEC_KEY_ID_MAX];
    int     algo;               /* SEC_ALGO_*                         */
    uint8_t key[SEC_KEY_BYTES];
    size_t  key_len;
    bool    in_use;
};

enum {
    SEC_ALGO_NONE        = 0,
    SEC_ALGO_HMAC_SHA256 = 1,
    /* TODO: SEC_ALGO_ED25519 when public-key support lands. */
};

/* Trust store API. All return 0 on success, -1 on failure. */
int  sig_trust_store_init(void);          /* called from kmain          */
int  sig_trust_store_reload(void);        /* re-read SEC_TRUST_PATH     */
const struct sec_key *sig_trust_store_find(const char *id);
int  sig_trust_store_count(void);
const struct sec_key *sig_trust_store_at(int idx);

/* Returns the algorithm name string for an id, or "?" if unknown. */
const char *sig_algo_name(int algo);

/* Verify an HMAC-SHA-256 signature `sig_hex` (64-char hex of a 32-byte
 * tag) over `buf` of `n` bytes using the trusted key with `id`.
 * Returns:
 *   1   verified -- signature matches
 *   0   key found but signature does NOT match
 *  -1   no such key id in trust store
 *  -2   algorithm mismatch (key isn't HMAC-SHA256)
 *  -3   sig_hex malformed (wrong length / non-hex chars) */
int sig_verify_hmac(const char *id, const char *sig_hex,
                    const void *buf, size_t n);

/* Boot self-test for the crypto primitives. Called once from kmain.
 * Panics on regression so a broken crypto build can't silently
 * propagate to verification calls downstream. */
void sec_selftest(void);

#endif /* TOBYOS_SEC_H */
