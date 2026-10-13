/* su_cksum.c -- cksum, as GNU coreutils 9.7's (logs/utilgate/cksum.sh).
 *
 *   - With no -a, POSIX's CRC: "CRC SIZE NAME" ("CRC SIZE" for standard
 *     input not named). -a picks one of bsd sysv crc crc32b (the legacy
 *     forms: "SUM BLOCKS NAME", --tag --untagged --base64 meaning nothing
 *     to them) or md5 sha1 sha224 sha256 sha384 sha512 blake2b sm3: "ALGO
 *     (NAME) = DIGEST", or with --untagged "DIGEST  NAME"; --base64 the
 *     digest in base64, --raw the bare bytes (a sum's big-endian), -z a
 *     NUL after each, and a name with a backslash or a newline escaped and
 *     the line begun with '\' (not under -z). -l BITS for blake2b (a
 *     multiple of 8; 0 or past 512 is 512), "BLAKE2b-BITS" in the tag.
 *   - -c FILE... reads such lines back and checks them: "NAME: OK",
 *     "NAME: FAILED", "NAME: FAILED open or read", and GNU's warnings and
 *     statuses with --quiet --status --strict -w --ignore-missing. A
 *     tagged line names its algorithm; an untagged one needs -a.
 *   - GNU's messages: a file that cannot be read by name (quoted shell
 *     style when it must be), "invalid argument 'X' for '--algorithm'"
 *     with the list, "--length is only supported with
 *     --algorithm=blake2b".
 *
 * The digests are libtoby's (md5sha1.c, sha2.c) and monocypher's BLAKE2b;
 * SM3, CRC-32B and the BSD and System V sums are here.
 *
 * It was POSIX's CRC and nothing else: every option taken as nothing, a
 * whole file read into memory, "cksum: NAME: ERROR" for one it could not
 * open and a crash for a directory. */

#include <toby/md5sha1.h>
#include <toby/sha2.h>
#include <monocypher.h>

/* ---- the sums -------------------------------------------------------------- */

/* POSIX cksum is a CRC-32 with a DIFFERENT convention from gzip's: the
 * polynomial runs MSB-first and the LENGTH is fed in after the data,
 * which is why the two never agree on the same file. */
static uint32_t ck_tab[256], ck_tabr[256];
static int ck_ready;
static void ck_tables(void) {
    if (ck_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
        ck_tab[i] = c;
        uint32_t r = i;
        for (int k = 0; k < 8; k++) r = (r & 1) ? (r >> 1) ^ 0xEDB88320u : r >> 1;
        ck_tabr[i] = r;
    }
    ck_ready = 1;
}

/* ---- SM3 (GB/T 32905-2016) ------------------------------------------------- */

struct sm3_ctx { uint32_t v[8]; uint64_t len; uint8_t buf[64]; size_t n; };
static uint32_t sm3_rol(uint32_t x, int r) { r &= 31; return r ? (x << r) | (x >> (32 - r)) : x; }
static void sm3_init(struct sm3_ctx *c) {
    static const uint32_t iv[8] = { 0x7380166f, 0x4914b2b9, 0x172442d7, 0xda8a0600,
                                    0xa96f30bc, 0x163138aa, 0xe38dee4d, 0xb0fb0e4e };
    memcpy(c->v, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}
static void sm3_block(struct sm3_ctx *c, const uint8_t *p) {
    uint32_t w[68], w1[64];
    for (int j = 0; j < 16; j++)
        w[j] = (uint32_t)p[4 * j] << 24 | (uint32_t)p[4 * j + 1] << 16 |
               (uint32_t)p[4 * j + 2] << 8 | p[4 * j + 3];
    for (int j = 16; j < 68; j++) {
        uint32_t x = w[j - 16] ^ w[j - 9] ^ sm3_rol(w[j - 3], 15);
        w[j] = (x ^ sm3_rol(x, 15) ^ sm3_rol(x, 23)) ^ sm3_rol(w[j - 13], 7) ^ w[j - 6];
    }
    for (int j = 0; j < 64; j++) w1[j] = w[j] ^ w[j + 4];
    uint32_t A = c->v[0], B = c->v[1], C = c->v[2], D = c->v[3];
    uint32_t E = c->v[4], F = c->v[5], G = c->v[6], H = c->v[7];
    for (int j = 0; j < 64; j++) {
        uint32_t T = j < 16 ? 0x79cc4519u : 0x7a879d8au;
        uint32_t ss1 = sm3_rol(sm3_rol(A, 12) + E + sm3_rol(T, j % 32), 7);
        uint32_t ss2 = ss1 ^ sm3_rol(A, 12);
        uint32_t ff = j < 16 ? A ^ B ^ C : (A & B) | (A & C) | (B & C);
        uint32_t gg = j < 16 ? E ^ F ^ G : (E & F) | (~E & G);
        uint32_t tt1 = ff + D + ss2 + w1[j];
        uint32_t tt2 = gg + H + ss1 + w[j];
        D = C; C = sm3_rol(B, 9); B = A; A = tt1;
        H = G; G = sm3_rol(F, 19); F = E; E = tt2 ^ sm3_rol(tt2, 9) ^ sm3_rol(tt2, 17);
    }
    c->v[0] ^= A; c->v[1] ^= B; c->v[2] ^= C; c->v[3] ^= D;
    c->v[4] ^= E; c->v[5] ^= F; c->v[6] ^= G; c->v[7] ^= H;
}
static void sm3_update(struct sm3_ctx *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n) {
        size_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { sm3_block(c, c->buf); c->n = 0; }
    }
}
static void sm3_final(struct sm3_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0;
    sm3_update(c, &pad, 1);
    while (c->n != 56) sm3_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sm3_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->v[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->v[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->v[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->v[i];
    }
}

/* ---- the algorithms -------------------------------------------------------- */

enum { CK_BSD, CK_SYSV, CK_CRC, CK_CRC32B, CK_MD5, CK_SHA1, CK_SHA224, CK_SHA256,
       CK_SHA384, CK_SHA512, CK_BLAKE2B, CK_SM3, CK_N };
static const char *const ck_names[CK_N] = {
    "bsd", "sysv", "crc", "crc32b", "md5", "sha1", "sha224", "sha256",
    "sha384", "sha512", "blake2b", "sm3" };
static const char *const ck_tags[CK_N] = {
    "", "", "", "", "MD5", "SHA1", "SHA224", "SHA256", "SHA384", "SHA512", "BLAKE2b", "SM3" };
static const int ck_bytes[CK_N] = { 2, 2, 4, 4, 16, 20, 28, 32, 48, 64, 64, 32 };
#define CK_LEGACY(a) ((a) <= CK_CRC32B)

union ck_ctx {
    toby_md5_ctx md5;
    toby_sha1_ctx sha1;
    toby_sha256_ctx s256;
    toby_sha512_ctx s512;
    crypto_blake2b_ctx b2;
    struct sm3_ctx sm3;
    struct { uint32_t v; uint64_t len; } sum;
};

/* the digest of FD into OUT (up to 64 bytes), and its length in bytes;
 * -1 (errno set) if it could not be read */
static int ck_digest(int fd, int algo, int b2bytes, uint8_t *out, uint64_t *size) {
    union ck_ctx c;
    memset(&c, 0, sizeof c);
    switch (algo) {
    case CK_MD5: toby_md5_init(&c.md5); break;
    case CK_SHA1: toby_sha1_init(&c.sha1); break;
    case CK_SHA224: toby_sha256_init(&c.s256, 1); break;
    case CK_SHA256: toby_sha256_init(&c.s256, 0); break;
    case CK_SHA384: toby_sha512_init(&c.s512, 1); break;
    case CK_SHA512: toby_sha512_init(&c.s512, 0); break;
    case CK_BLAKE2B: crypto_blake2b_init(&c.b2, (size_t)b2bytes); break;
    case CK_SM3: sm3_init(&c.sm3); break;
    case CK_CRC32B: c.sum.v = 0xffffffffu; break;
    default: c.sum.v = 0; break;
    }
    ck_tables();
    static uint8_t buf[65536];
    uint64_t total = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        total += (uint64_t)r;
        switch (algo) {
        case CK_MD5: toby_md5_update(&c.md5, buf, (size_t)r); break;
        case CK_SHA1: toby_sha1_update(&c.sha1, buf, (size_t)r); break;
        case CK_SHA224: case CK_SHA256: toby_sha256_update(&c.s256, buf, (size_t)r); break;
        case CK_SHA384: case CK_SHA512: toby_sha512_update(&c.s512, buf, (size_t)r); break;
        case CK_BLAKE2B: crypto_blake2b_update(&c.b2, buf, (size_t)r); break;
        case CK_SM3: sm3_update(&c.sm3, buf, (size_t)r); break;
        case CK_CRC:
            for (ssize_t i = 0; i < r; i++)
                c.sum.v = (c.sum.v << 8) ^ ck_tab[((c.sum.v >> 24) ^ buf[i]) & 0xff];
            break;
        case CK_CRC32B:
            for (ssize_t i = 0; i < r; i++)
                c.sum.v = (c.sum.v >> 8) ^ ck_tabr[(c.sum.v ^ buf[i]) & 0xff];
            break;
        case CK_SYSV:
            for (ssize_t i = 0; i < r; i++) c.sum.v += buf[i];
            break;
        case CK_BSD:
            for (ssize_t i = 0; i < r; i++) {
                c.sum.v = (c.sum.v >> 1) + ((c.sum.v & 1) << 15);
                c.sum.v = (c.sum.v + buf[i]) & 0xffff;
            }
            break;
        }
    }
    *size = total;
    uint8_t big[64];
    switch (algo) {
    case CK_MD5: toby_md5_final(&c.md5, out); return 16;
    case CK_SHA1: toby_sha1_final(&c.sha1, out); return 20;
    case CK_SHA224: toby_sha256_final(&c.s256, big); memcpy(out, big, 28); return 28;
    case CK_SHA256: toby_sha256_final(&c.s256, big); memcpy(out, big, 32); return 32;
    case CK_SHA384: toby_sha512_final(&c.s512, big); memcpy(out, big, 48); return 48;
    case CK_SHA512: toby_sha512_final(&c.s512, big); memcpy(out, big, 64); return 64;
    case CK_BLAKE2B: crypto_blake2b_final(&c.b2, out); return b2bytes;
    case CK_SM3: sm3_final(&c.sm3, out); return 32;
    case CK_CRC: {
        uint32_t v = c.sum.v;
        for (uint64_t len = total; len; len >>= 8)
            v = (v << 8) ^ ck_tab[((v >> 24) ^ (uint32_t)(len & 0xff)) & 0xff];
        v = ~v;
        out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
        out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v;
        return 4;
    }
    case CK_CRC32B: {
        uint32_t v = ~c.sum.v;
        out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
        out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v;
        return 4;
    }
    case CK_SYSV: {
        uint32_t s = c.sum.v;
        uint32_t r = (s & 0xffff) + (s >> 16);
        r = (r & 0xffff) + (r >> 16);
        out[0] = (uint8_t)(r >> 8); out[1] = (uint8_t)r;
        return 2;
    }
    default:
        out[0] = (uint8_t)(c.sum.v >> 8); out[1] = (uint8_t)c.sum.v;
        return 2;
    }
}

static void ck_hex(const uint8_t *d, int n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[2 * n] = 0;
}

static void ck_b64(const uint8_t *d, int n, char *out) {
    static const char b[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) |
                     (i + 2 < n ? d[i + 2] : 0);
        out[o++] = b[(v >> 18) & 63];
        out[o++] = b[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b[v & 63] : '=';
    }
    out[o] = 0;
}

/* NAME as a line shows it: \\ and \n escaped (and whether it had to be) */
static int ck_escape(const char *name, char *out, size_t cap) {
    int esc = strchr(name, '\\') || strchr(name, '\n') || strchr(name, '\r');
    size_t o = 0;
    for (const char *p = name; *p && o + 3 < cap; p++) {
        if (esc && *p == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (esc && *p == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (esc && *p == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else out[o++] = *p;
    }
    out[o] = 0;
    return esc;
}

struct ck_opts {
    int algo, b2bits, base64, raw, untagged, zero;
    int quiet, status, strict, warn, ignore_missing;
};

static int ck_open(const char *name) {
    if (!strcmp(name, "-")) return 0;
    return su_open_read(name, 0);
}

/* one FILE's line, or its error; 0 or 1 */
static int ck_one(const struct ck_opts *o, const char *name, int named) {
    int fd = ck_open(name);
    if (fd < 0) { su_error(0, errno, "%s", su_quotef(name)); return 1; }
    uint8_t d[64];
    uint64_t size;
    int b2 = o->b2bits ? o->b2bits / 8 : 64;
    int n = ck_digest(fd, o->algo, b2, d, &size);
    int e = errno;
    if (fd > 0) close(fd);
    if (n < 0) { su_error(0, e, "%s", su_quotef(name)); return 1; }
    char end = o->zero ? '\0' : '\n';
    if (o->raw) {
        fwrite(d, 1, (size_t)n, stdout);
        return 0;
    }
    if (CK_LEGACY(o->algo)) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = v << 8 | d[i];
        if (o->algo == CK_BSD) printf("%05u %5" PRIu64, v, (size + 1023) / 1024);
        else if (o->algo == CK_SYSV) printf("%u %" PRIu64, v, (size + 511) / 512);
        else printf("%u %" PRIu64, v, size);
        if (named) printf(" %s", name);
        putchar(end);
        return 0;
    }
    char dig[200];
    if (o->base64) ck_b64(d, n, dig); else ck_hex(d, n, dig);
    char ename[8192];
    int esc = !o->zero && ck_escape(name, ename, sizeof ename);
    const char *shown = o->zero ? name : ename;
    if (o->untagged) {
        printf("%s%s  %s", esc ? "\\" : "", dig, shown);
    } else {
        printf("%s%s", esc ? "\\" : "", ck_tags[o->algo]);
        if (o->algo == CK_BLAKE2B && o->b2bits && o->b2bits != 512) printf("-%d", o->b2bits);
        printf(" (%s) = %s", shown, dig);
    }
    putchar(end);
    return 0;
}

/* ---- --check ------------------------------------------------------------- */

static int ck_unhex(const char *s, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n / 2; i++) {
        int hi = isdigit((unsigned char)s[2 * i]) ? s[2 * i] - '0' : tolower(s[2 * i]) - 'a' + 10;
        int lo = isdigit((unsigned char)s[2 * i + 1]) ? s[2 * i + 1] - '0' : tolower(s[2 * i + 1]) - 'a' + 10;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

/* a digest field of an algorithm whose digest is NB bytes: hex, or the
 * base64 of it; into OUT, 0, or -1 if it is neither */
static int ck_parse_digest(const char *s, size_t len, int nb, uint8_t *out) {
    if (len == (size_t)nb * 2 && strspn(s, "0123456789abcdefABCDEF") >= len) {
        ck_unhex(s, len, out);
        return 0;
    }
    size_t b64len = (size_t)((nb + 2) / 3 * 4);
    if (len == b64len) {
        char tmp[200];
        uint8_t back[64] = { 0 };
        static const char b[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int o = 0;
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '=') break;
            const char *pp = strchr(b, s[i]);
            if (!pp || !s[i]) return -1;
            acc = acc << 6 | (uint32_t)(pp - b);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (o < 64) back[o++] = (uint8_t)(acc >> bits);
            }
        }
        if (o != nb) return -1;
        ck_b64(back, nb, tmp);
        if (strncmp(tmp, s, len) != 0) return -1;
        memcpy(out, back, (size_t)nb);
        return 0;
    }
    return -1;
}

static void ck_unescape(char *s) {
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1] == '\\') { *w++ = '\\'; p++; }
        else if (*p == '\\' && p[1] == 'n') { *w++ = '\n'; p++; }
        else if (*p == '\\' && p[1] == 'r') { *w++ = '\r'; p++; }
        else *w++ = *p;
    }
    *w = 0;
}

/* LINE: its algorithm, BLAKE2b bits, digest and file name; 0, or -1 */
static int ck_parse_line(const struct ck_opts *o, char *line, int *algo, int *bits,
                         uint8_t *dig, char **name) {
    int esc = line[0] == '\\';
    if (esc) line++;
    /* tagged: ALGO[-BITS] (NAME) = DIGEST */
    for (int a = CK_MD5; a < CK_N; a++) {
        size_t tl = strlen(ck_tags[a]);
        if (strncmp(line, ck_tags[a], tl) != 0) continue;
        char *p = line + tl;
        int b = 0;
        if (a == CK_BLAKE2B && *p == '-') {
            char *e;
            long v = strtol(p + 1, &e, 10);
            if (e == p + 1 || v <= 0 || v > 512 || v % 8) return -1;
            b = (int)v;
            p = e;
        }
        if (p[0] != ' ' || p[1] != '(') continue;
        if (o->algo >= 0 && o->algo != a) return -1;
        p += 2;
        char *close = strstr(p, ") = ");
        if (!close) return -1;
        /* the last ") = ": a name may hold one */
        for (char *q = close; (q = strstr(q + 1, ") = ")); ) close = q;
        *close = 0;
        char *dg = close + 4;
        int nb = a == CK_BLAKE2B ? (b ? b / 8 : 64) : ck_bytes[a];
        if (ck_parse_digest(dg, strlen(dg), nb, dig) != 0) return -1;
        if (esc) ck_unescape(p);
        *algo = a;
        *bits = b;
        *name = p;
        return 0;
    }
    /* untagged: DIGEST  NAME (or DIGEST *NAME), and only with -a */
    if (o->algo < 0 || CK_LEGACY(o->algo)) return -1;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char *sp = p;
    while (*sp && *sp != ' ') sp++;
    if (!*sp || (sp[1] != ' ' && sp[1] != '*')) return -1;
    int a = o->algo;
    int nb = a == CK_BLAKE2B ? (o->b2bits ? o->b2bits / 8 : 64) : ck_bytes[a];
    if (ck_parse_digest(p, (size_t)(sp - p), nb, dig) != 0) return -1;
    char *nm = sp + 2;
    if (!*nm) return -1;
    if (esc) ck_unescape(nm);
    *algo = a;
    *bits = o->b2bits;
    *name = nm;
    return 0;
}

static int ck_check_file(const struct ck_opts *o, const char *list) {
    FILE *f = strcmp(list, "-") ? fopen(list, "r") : stdin;
    if (!f) { su_error(0, errno, "%s", su_quotef(list)); return 1; }
    char *line = 0;
    size_t cap = 0;
    long lineno = 0;
    int bad_fmt = 0, good = 0, mismatch = 0, unread = 0, rc = 0;
    const char *last_tag = o->algo >= 0 ? ck_tags[o->algo] : 0;
    ssize_t len;
    char end = o->zero ? '\0' : '\n';
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        if (len && line[len - 1] == '\n') line[--len] = 0;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        int algo, bits;
        uint8_t want[64];
        char *name;
        if (ck_parse_line(o, line, &algo, &bits, want, &name) != 0) {
            bad_fmt++;
            if (o->warn)
                su_error(0, 0, "%s: %ld: improperly formatted %s checksum line",
                         su_quotef(list), lineno, last_tag ? last_tag : "MD5");
            continue;
        }
        last_tag = ck_tags[algo];
        good++;
        int fd = ck_open(name);
        uint8_t got[64];
        uint64_t size;
        int n = -1, e = 0;
        if (fd >= 0) {
            int nb = algo == CK_BLAKE2B ? (bits ? bits / 8 : 64) : ck_bytes[algo];
            n = ck_digest(fd, algo, nb, got, &size);
            e = errno;
            if (fd > 0) close(fd);
        } else {
            e = errno;
        }
        char ename[8192];
        int esc = ck_escape(name, ename, sizeof ename);
        if (n < 0) {
            if (o->ignore_missing && e == ENOENT) { good--; continue; }
            unread++;
            if (!o->status) {
                su_error(0, e, "%s", su_quotef(name));
                printf("%s%s: FAILED open or read%c", esc ? "\\" : "", ename, end);
            }
            continue;
        }
        int nb = algo == CK_BLAKE2B ? (bits ? bits / 8 : 64) : ck_bytes[algo];
        int ok = memcmp(got, want, (size_t)nb) == 0;
        if (!ok) mismatch++;
        if (!o->status && (!ok || !o->quiet))
            printf("%s%s: %s%c", esc ? "\\" : "", ename, ok ? "OK" : "FAILED", end);
    }
    free(line);
    if (f != stdin) fclose(f);
    if (!good) {
        su_error(0, 0, "%s: no properly formatted checksum lines found", su_quotef(list));
        return 1;
    }
    if (!o->status) {
        if (bad_fmt)
            su_error(0, 0, "WARNING: %d line%s improperly formatted", bad_fmt,
                     bad_fmt == 1 ? " is" : "s are");
        if (unread)
            su_error(0, 0, "WARNING: %d listed file%s could not be read", unread,
                     unread == 1 ? "" : "s");
        if (mismatch)
            su_error(0, 0, "WARNING: %d computed checksum%s did NOT match", mismatch,
                     mismatch == 1 ? "" : "s");
    }
    if (mismatch || unread || (o->strict && bad_fmt)) rc = 1;
    return rc;
}

static int do_cksum(int argc, char **argv) {
    enum { BASE64_OPT = 0x100, RAW_OPT, TAG_OPT, UNTAG_OPT, IGN_OPT, QUIET_OPT,
           STATUS_OPT, STRICT_OPT, DEBUG_OPT };
    static const struct su_option longs[] = {
        { "algorithm", SU_REQ_ARG, 0, 'a' },
        { "base64", SU_NO_ARG, 0, BASE64_OPT },
        { "check", SU_NO_ARG, 0, 'c' },
        { "length", SU_REQ_ARG, 0, 'l' },
        { "raw", SU_NO_ARG, 0, RAW_OPT },
        { "tag", SU_NO_ARG, 0, TAG_OPT },
        { "untagged", SU_NO_ARG, 0, UNTAG_OPT },
        { "zero", SU_NO_ARG, 0, 'z' },
        { "ignore-missing", SU_NO_ARG, 0, IGN_OPT },
        { "quiet", SU_NO_ARG, 0, QUIET_OPT },
        { "status", SU_NO_ARG, 0, STATUS_OPT },
        { "strict", SU_NO_ARG, 0, STRICT_OPT },
        { "warn", SU_NO_ARG, 0, 'w' },
        { "debug", SU_NO_ARG, 0, DEBUG_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    struct ck_opts o;
    memset(&o, 0, sizeof o);
    o.algo = -1;
    int check = 0, tag = 0, c;
    const char *lenarg = 0;
    while ((c = su_getopt_long(argc, argv, "a:cl:wz", longs, 0)) != -1) {
        switch (c) {
        case 'a': {
            int found = -1;
            for (int i = 0; i < CK_N; i++) if (!strcmp(su_optarg, ck_names[i])) found = i;
            if (found < 0) {
                fflush(stdout);
                fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n",
                        su_short, su_quote(su_optarg), su_quote("--algorithm"));
                for (int i = 0; i < CK_N; i++) fprintf(stderr, "  - %s\n", su_quote(ck_names[i]));
                su_try_help(1);
            }
            o.algo = found;
            break;
        }
        case 'c': check = 1; break;
        case 'l': lenarg = su_optarg; break;
        case 'w': o.warn = 1; break;
        case 'z': o.zero = 1; break;
        case BASE64_OPT: o.base64 = 1; break;
        case RAW_OPT: o.raw = 1; break;
        case TAG_OPT: tag = 1; o.untagged = 0; break;
        case UNTAG_OPT: o.untagged = 1; tag = 0; break;
        case IGN_OPT: o.ignore_missing = 1; break;
        case QUIET_OPT: o.quiet = 1; break;
        case STATUS_OPT: o.status = 1; break;
        case STRICT_OPT: o.strict = 1; break;
        case DEBUG_OPT: break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Print a checksum and the size of each FILE, or check the\n"
                   "checksums listed in them. With no FILE, or when FILE is -,\n"
                   "read standard input.\n"
                   "\n"
                   "  -a, --algorithm=TYPE  bsd sysv crc crc32b md5 sha1 sha224 sha256\n"
                   "                        sha384 sha512 blake2b sm3; crc unless given\n"
                   "      --base64          a digest in base64, not hexadecimal\n"
                   "  -c, --check           check the checksums listed in each FILE\n"
                   "  -l, --length=BITS     blake2b's digest length, a multiple of 8\n"
                   "      --raw             the digest's bytes alone\n"
                   "      --tag             ALGO (FILE) = DIGEST lines (the default)\n"
                   "      --untagged        DIGEST  FILE lines\n"
                   "  -z, --zero            end each line with a NUL; no escaping\n"
                   "\n"
                   "When checking:\n"
                   "      --ignore-missing  say nothing of files that are not there\n"
                   "      --quiet           print nothing for a file that checks out\n"
                   "      --status          print nothing; the status says\n"
                   "      --strict          fail on a line that is not a checksum\n"
                   "  -w, --warn            report each line that is not a checksum\n"
                   "\n"
                   "      --debug           (taken; says nothing here)\n"
                   "      --help            show this help and exit\n"
                   "      --version         show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (lenarg) {
        char *e;
        long v = strtol(lenarg, &e, 10);
        if (e == lenarg || *e || v < 0) {
            su_error(0, 0, "invalid length: %s", su_quote(lenarg));
            su_error(1, 0, "length is not a multiple of 8");
        }
        if (o.algo != CK_BLAKE2B)
            su_error(1, 0, "--length is only supported with --algorithm=blake2b");
        if (v % 8) {
            su_error(0, 0, "invalid length: %s", su_quote(lenarg));
            su_error(1, 0, "length is not a multiple of 8");
        }
        o.b2bits = (v == 0 || v > 512) ? 0 : (int)v;
    }
    if (check && tag) {
        su_error(0, 0, "the --binary and --text options are meaningless when verifying checksums");
        su_try_help(1);
    }
    if (!check && o.algo < 0) o.algo = CK_CRC;
    int rc = 0;
    if (check) {
        if (su_optind == argc) rc |= ck_check_file(&o, "-");
        for (int i = su_optind; i < argc; i++) rc |= ck_check_file(&o, argv[i]);
        return rc;
    }
    if (su_optind == argc) rc |= ck_one(&o, "-", 0);
    for (int i = su_optind; i < argc; i++) rc |= ck_one(&o, argv[i], 1);
    if (fflush(stdout) != 0) su_error(1, errno, "write error");
    return rc;
}
