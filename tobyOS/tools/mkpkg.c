/* mkpkg.c -- host tool that produces a tobyOS .tpkg package.
 *
 * Build (MSYS2/UCRT64, Linux, macOS):
 *     gcc -O2 -Wall -Wextra -std=c11 -o build/mkpkg tools/mkpkg.c
 *
 * Usage:
 *     mkpkg -o out.tpkg \
 *           --name helloapp --version 1.0 \
 *           [--desc "A demo hello-world app"] \
 *           [--publisher "tobyOS-builtin"] \
 *           [--sandbox <profile>] [--caps "FILE_READ,GUI"] \
 *           [--app "Hello Pkg|/data/apps/helloapp.elf"] \
 *           --file /data/apps/helloapp.elf:programs/...helloapp.elf \
 *           --file /data/apps/helloapp.txt:some/local/helloapp.txt \
 *           [--sign-key-file path/to/key.bin --sign-key-id default] \
 *           [--corrupt-body OFFSET]                               \
 *           [--no-hash]                                           \
 *           [--bad-hash]                                          \
 *           [--bad-fhash <abs-dest>]
 *
 * --name      : package name (required, becomes /data/packages/<name>.pkg)
 * --version   : package version (required, free-form ASCII)
 * --desc      : optional one-line description
 * --publisher : optional publisher / source label (M34A)
 * --sandbox   : optional named sandbox profile applied at app launch
 * --caps      : optional CSV of capability tokens (M34D), e.g. "FILE_READ,GUI"
 * --app       : optional "label|exec_path" launcher entry. The exec path
 *               MUST match one of the --file destinations. May repeat.
 * --file      : "<dest>:<src>"; copies bytes from host file <src> into the
 *               archive, tagged as destination <dest> inside the guest VFS.
 *               May repeat. All <dest> paths MUST start with "/data/".
 * -o          : output .tpkg path (required).
 *
 * Integrity / signing flags (M34A & M34C):
 *
 * --sign-key-file : raw HMAC-SHA256 key (binary file, any length)
 * --sign-key-id   : key identifier (must match an entry in
 *                   /system/keys/trust.db inside the guest)
 * --corrupt-body N: AFTER writing, flip one bit at body byte N. Use to
 *                   produce *_corrupt.tpkg fixtures for the M34A tests.
 * --no-hash       : skip emitting HASH/FHASH (legacy / pre-M34 fixture).
 * --bad-hash      : emit a deliberately-wrong HASH (M34A negative test).
 * --bad-fhash D   : emit a deliberately-wrong FHASH only for dest D
 *                   (M34A per-file negative test).
 *
 * The output format matches what src/pkg.c expects: ASCII header
 * (TPKG 1 / NAME / VERSION / DESC / PUBLISHER / SANDBOX / CAPS / APP /
 *  FILE ... / HASH / FHASH ... / SIG) then "BODY\n" then the
 * concatenated raw file payloads in --file declaration order.
 *
 * Exits with status 0 on success, non-zero on any argument / I/O error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#define MAX_FILES 32
#define MAX_APPS  8

/* ===================================================================
 * SHA-256 (FIPS 180-4) -- self-contained, no deps.
 * Mirrors the kernel implementation in src/sec.c so on-host hashes
 * match on-target verification byte-for-byte.
 * =================================================================== */

#define SHA256_DIGEST_LEN 32

struct sha256_ctx {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    size_t   buflen;
};

static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_compress(struct sha256_ctx *c, const uint8_t *blk) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4]     << 24)
             | ((uint32_t)blk[i * 4 + 1] << 16)
             | ((uint32_t)blk[i * 4 + 2] <<  8)
             | ((uint32_t)blk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i -  2],17) ^ ROTR(w[i -  2], 19) ^ (w[i -  2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g  = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e,6) ^ ROTR(e,11) ^ ROTR(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a,2) ^ ROTR(a,13) ^ ROTR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

static void sha256_init(struct sha256_ctx *c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->bits = 0; c->buflen = 0;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    c->bits += (uint64_t)n * 8;
    if (c->buflen) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { sha256_compress(c, c->buf); c->buflen = 0; }
    }
    while (n >= 64) { sha256_compress(c, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->buflen = n; }
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]) {
    uint64_t bits = c->bits;
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        while (c->buflen < 64) c->buf[c->buflen++] = 0;
        sha256_compress(c, c->buf); c->buflen = 0;
    }
    while (c->buflen < 56) c->buf[c->buflen++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->buflen++] = (uint8_t)(bits >> (i * 8));
    sha256_compress(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >>  8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

static void sha256_buf(const void *data, size_t n,
                       uint8_t out[SHA256_DIGEST_LEN]) {
    struct sha256_ctx c; sha256_init(&c); sha256_update(&c, data, n);
    sha256_final(&c, out);
}

/* ===================================================================
 * HMAC-SHA256 (RFC 2104) for --sign-key-file / M34C tooling.
 * =================================================================== */

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const void *msg, size_t msg_len,
                        uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k0[64] = {0};
    if (key_len > 64) sha256_buf(key, key_len, k0);
    else              memcpy(k0, key, key_len);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    struct sha256_ctx ctx;
    uint8_t inner[SHA256_DIGEST_LEN];
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, SHA256_DIGEST_LEN);
    sha256_final(&ctx, out);
}

static void to_hex(const uint8_t *bytes, size_t n, char *out) {
    static const char *hexd = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hexd[(bytes[i] >> 4) & 0xf];
        out[i * 2 + 1] = hexd[ bytes[i]       & 0xf];
    }
    out[n * 2] = '\0';
}

/* ===================================================================
 * Argument plumbing.
 * =================================================================== */

struct file_ent {
    const char *dest;
    const char *src;
    long        size;
};

struct app_ent {
    const char *label;
    const char *exec;
};

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s -o <out.tpkg> --name <n> --version <v>\n"
            "       [--desc <d>] [--publisher <p>] [--sandbox <s>] [--caps <csv>]\n"
            "       [--app 'label|exec'] --file <dest>:<src> [--file ...]\n"
            "       [--sign-key-file <path> --sign-key-id <id>]\n"
            "       [--corrupt-body <N>] [--no-hash] [--bad-hash]\n"
            "       [--bad-fhash <dest>]\n", prog);
}

static const char *next_arg(int argc, char **argv, int *i, const char *opt) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "mkpkg: %s requires a value\n", opt);
        exit(2);
    }
    return argv[++(*i)];
}

static const char *split_file_arg(char *arg, const char **out_dest) {
    char *colon = strchr(arg, ':');
    if (!colon) {
        fprintf(stderr, "mkpkg: --file expects <dest>:<src>, got '%s'\n", arg);
        exit(2);
    }
    *colon = '\0';
    *out_dest = arg;
    const char *src = colon + 1;
    if (arg[0] != '/') {
        fprintf(stderr, "mkpkg: dest '%s' must be absolute (start with '/')\n", arg);
        exit(2);
    }
    if (strncmp(arg, "/data/", 6) != 0) {
        fprintf(stderr, "mkpkg: dest '%s' must start with '/data/'\n", arg);
        exit(2);
    }
    if (!*src) {
        fprintf(stderr, "mkpkg: --file missing src after ':' (dest='%s')\n", arg);
        exit(2);
    }
    return src;
}

static unsigned char *slurp(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mkpkg: cannot open '%s': %s\n", path, strerror(errno));
        exit(3);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "mkpkg: fseek('%s'): %s\n", path, strerror(errno));
        exit(3);
    }
    long sz = ftell(f);
    if (sz < 0) {
        fprintf(stderr, "mkpkg: ftell('%s'): %s\n", path, strerror(errno));
        exit(3);
    }
    rewind(f);
    /* slurp at least one byte so the malloc never returns NULL on
     * legitimate empty files. */
    unsigned char *buf = (unsigned char *)malloc((size_t)(sz > 0 ? sz : 1));
    if (!buf) {
        fprintf(stderr, "mkpkg: OOM reading '%s' (%ld bytes)\n", path, sz);
        exit(3);
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "mkpkg: short read on '%s'\n", path);
        exit(3);
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

int main(int argc, char **argv) {
    const char *out_path  = 0;
    const char *name      = 0;
    const char *version   = 0;
    const char *desc      = 0;
    const char *publisher = 0;
    const char *sandbox   = 0;
    const char *caps      = 0;
    const char *sign_key_file = 0;
    const char *sign_key_id   = 0;
    const char *bad_fhash_dest = 0;
    long        corrupt_body  = -1;     /* <0 disables */
    int         no_hash       = 0;
    int         bad_hash      = 0;

    struct file_ent files[MAX_FILES];
    int file_count = 0;
    struct app_ent  apps[MAX_APPS];
    int app_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o"))             out_path = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--name"))    name      = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--version")) version   = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--desc"))    desc      = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--publisher")) publisher = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--sandbox")) sandbox   = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--caps"))    caps      = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--sign-key-file"))
            sign_key_file = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--sign-key-id"))
            sign_key_id = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--corrupt-body")) {
            const char *vs = next_arg(argc, argv, &i, a);
            corrupt_body = strtol(vs, 0, 0);
            if (corrupt_body < 0) {
                fprintf(stderr, "mkpkg: --corrupt-body needs >=0, got %s\n", vs);
                return 2;
            }
        }
        else if (!strcmp(a, "--no-hash"))  no_hash  = 1;
        else if (!strcmp(a, "--bad-hash")) bad_hash = 1;
        else if (!strcmp(a, "--bad-fhash"))
            bad_fhash_dest = next_arg(argc, argv, &i, a);
        else if (!strcmp(a, "--app")) {
            if (app_count >= MAX_APPS) {
                fprintf(stderr, "mkpkg: too many --app (max %d)\n", MAX_APPS);
                return 2;
            }
            char *spec = (char *)next_arg(argc, argv, &i, a);
            char *bar = strchr(spec, '|');
            if (!bar) {
                fprintf(stderr, "mkpkg: --app expects 'label|exec', got '%s'\n", spec);
                return 2;
            }
            *bar = '\0';
            apps[app_count].label = spec;
            apps[app_count].exec  = bar + 1;
            app_count++;
        } else if (!strcmp(a, "--file")) {
            if (file_count >= MAX_FILES) {
                fprintf(stderr, "mkpkg: too many --file (max %d)\n", MAX_FILES);
                return 2;
            }
            char *spec = (char *)next_arg(argc, argv, &i, a);
            const char *src = split_file_arg(spec, &files[file_count].dest);
            files[file_count].src  = src;
            files[file_count].size = 0;   /* filled below */
            file_count++;
        } else {
            fprintf(stderr, "mkpkg: unknown arg '%s'\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (!out_path || !name || !version || file_count == 0) {
        fprintf(stderr, "mkpkg: --name, --version, -o, and at least one --file required\n");
        usage(argv[0]);
        return 2;
    }

    if ((sign_key_file && !sign_key_id) || (!sign_key_file && sign_key_id)) {
        fprintf(stderr, "mkpkg: --sign-key-file and --sign-key-id must be paired\n");
        return 2;
    }

    /* Slurp every input payload. */
    unsigned char *payloads[MAX_FILES];
    long           total_body = 0;
    for (int i = 0; i < file_count; i++) {
        payloads[i] = slurp(files[i].src, &files[i].size);
        total_body += files[i].size;
    }

    /* Build the contiguous body buffer once -- we need it both for
     * hashing (HASH covers the concatenation) and for the file write.
     * Avoids hashing twice / hashing while writing. */
    unsigned char *body = NULL;
    if (total_body > 0) {
        body = (unsigned char *)malloc((size_t)total_body);
        if (!body) {
            fprintf(stderr, "mkpkg: OOM allocating body buffer (%ld bytes)\n",
                    total_body);
            return 3;
        }
        long off = 0;
        for (int i = 0; i < file_count; i++) {
            if (files[i].size > 0) {
                memcpy(body + off, payloads[i], (size_t)files[i].size);
                off += files[i].size;
            }
        }
    }

    /* Sanity: every APP's exec path must appear as a FILE dest. */
    for (int a = 0; a < app_count; a++) {
        int ok = 0;
        for (int f = 0; f < file_count; f++) {
            if (!strcmp(apps[a].exec, files[f].dest)) { ok = 1; break; }
        }
        if (!ok) {
            fprintf(stderr, "mkpkg: warning: app '%s' exec='%s' has no matching FILE\n",
                    apps[a].label, apps[a].exec);
        }
    }

    /* Compute digests. */
    char hex_pkg[SHA256_DIGEST_LEN * 2 + 1] = {0};
    char hex_files[MAX_FILES][SHA256_DIGEST_LEN * 2 + 1];
    if (!no_hash) {
        uint8_t pkg_digest[SHA256_DIGEST_LEN];
        sha256_buf(body ? body : (const unsigned char *)"", (size_t)total_body,
                   pkg_digest);
        to_hex(pkg_digest, SHA256_DIGEST_LEN, hex_pkg);
        if (bad_hash) {
            /* Flip the first nibble so the declared HASH definitely
             * does not match the computed body. */
            hex_pkg[0] = (hex_pkg[0] == '0') ? '1' : '0';
        }
        long off = 0;
        for (int i = 0; i < file_count; i++) {
            uint8_t d[SHA256_DIGEST_LEN];
            sha256_buf(body ? body + off : (const unsigned char *)"",
                       (size_t)files[i].size, d);
            to_hex(d, SHA256_DIGEST_LEN, hex_files[i]);
            if (bad_fhash_dest && !strcmp(bad_fhash_dest, files[i].dest)) {
                hex_files[i][0] = (hex_files[i][0] == '0') ? '1' : '0';
            }
            off += files[i].size;
        }
    }

    /* Compute optional HMAC signature. SIG covers the same body bytes
     * as HASH so a corrupted package fails fast on HASH well before we
     * ever check the signature. */
    char hex_sig[SHA256_DIGEST_LEN * 2 + 1] = {0};
    if (sign_key_file) {
        long klen = 0;
        unsigned char *kb = slurp(sign_key_file, &klen);
        if (klen <= 0) {
            fprintf(stderr, "mkpkg: signing key '%s' is empty\n", sign_key_file);
            return 3;
        }
        uint8_t mac[SHA256_DIGEST_LEN];
        hmac_sha256(kb, (size_t)klen, body ? body : (const unsigned char *)"",
                    (size_t)total_body, mac);
        to_hex(mac, SHA256_DIGEST_LEN, hex_sig);
        free(kb);
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "mkpkg: cannot open '%s' for writing: %s\n",
                out_path, strerror(errno));
        return 4;
    }

    /* Header. Order matches what src/pkg.c expects: TPKG, NAME,
     * VERSION, DESC, PUBLISHER, SANDBOX, CAPS, APP*, FILE+ (FHASH must
     * follow each FILE), HASH, SIG, BODY. */
    fprintf(out, "TPKG 1\n");
    fprintf(out, "NAME %s\n", name);
    fprintf(out, "VERSION %s\n", version);
    if (desc)      fprintf(out, "DESC %s\n",      desc);
    if (publisher) fprintf(out, "PUBLISHER %s\n", publisher);
    if (sandbox)   fprintf(out, "SANDBOX %s\n",   sandbox);
    if (caps)      fprintf(out, "CAPS %s\n",      caps);
    for (int a = 0; a < app_count; a++) {
        fprintf(out, "APP %s|%s\n", apps[a].label, apps[a].exec);
    }
    /* FILE lines first, then FHASH lines (parser insists on that
     * order for FHASH-FILE matching). */
    for (int i = 0; i < file_count; i++) {
        fprintf(out, "FILE %s %ld\n", files[i].dest, files[i].size);
    }
    if (!no_hash) {
        for (int i = 0; i < file_count; i++) {
            fprintf(out, "FHASH %s %s\n", files[i].dest, hex_files[i]);
        }
        fprintf(out, "HASH %s\n", hex_pkg);
    }
    if (sign_key_file) {
        fprintf(out, "SIG %s %s\n", sign_key_id, hex_sig);
    }
    fprintf(out, "BODY\n");

    long body_offset = ftell(out);
    if (body_offset < 0) {
        fprintf(stderr, "mkpkg: ftell on output failed\n");
        fclose(out); return 5;
    }

    /* Body. */
    if (total_body > 0 &&
        fwrite(body, 1, (size_t)total_body, out) != (size_t)total_body) {
        fprintf(stderr, "mkpkg: write failed for body\n");
        fclose(out); return 5;
    }

    /* --corrupt-body N: flip a bit at offset N inside the body, AFTER
     * everything (including HASH/SIG) is already written. The result
     * is a package whose declared HASH no longer matches the body --
     * exactly the M34A negative-test fixture we need. */
    if (corrupt_body >= 0) {
        if (corrupt_body >= total_body) {
            fprintf(stderr, "mkpkg: --corrupt-body %ld out of range "
                    "(body is %ld bytes)\n", corrupt_body, total_body);
            fclose(out); return 5;
        }
        if (fseek(out, body_offset + corrupt_body, SEEK_SET) != 0) {
            fprintf(stderr, "mkpkg: fseek for --corrupt-body failed\n");
            fclose(out); return 5;
        }
        unsigned char b = body[corrupt_body] ^ 0x01;
        if (fwrite(&b, 1, 1, out) != 1) {
            fprintf(stderr, "mkpkg: write failed for --corrupt-body\n");
            fclose(out); return 5;
        }
        fprintf(stderr, "mkpkg: corrupted body byte %ld (was 0x%02x -> 0x%02x)\n",
                corrupt_body, body[corrupt_body], b);
    }

    fclose(out);
    free(body);
    for (int i = 0; i < file_count; i++) free(payloads[i]);

    fprintf(stderr,
            "mkpkg: wrote %s (name=%s version=%s files=%d apps=%d body=%ld bytes%s%s%s)\n",
            out_path, name, version, file_count, app_count, total_body,
            no_hash ? " no-hash" : (bad_hash ? " bad-hash" : " hashed"),
            sign_key_file ? " signed" : "",
            corrupt_body >= 0 ? " CORRUPT" : "");
    return 0;
}
