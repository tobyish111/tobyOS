/* su_czcommon.c -- what su_gzip.c, su_bzip2.c and su_xz.c share: gzip's
 * and xz's CRC-32, a buffered input with a little pushback (decoders read
 * ahead), and doubles without the FPU for the ratios bzip2 and xz print.
 * Each of the three includes this file; the guard makes a second
 * inclusion empty, so main.c may include them in any order (and need not
 * include this one itself). */
#ifndef CZ_COMMON_INCLUDED
#define CZ_COMMON_INCLUDED

/* CRC-32 as gzip and xz use it: the reflected polynomial 0xEDB88320. */
static uint32_t cz_crctab[256];
static int cz_crcready;

static uint32_t cz_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    if (!cz_crcready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            cz_crctab[i] = c;
        }
        cz_crcready = 1;
    }
    crc = ~crc;
    while (n--) crc = cz_crctab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}


/* ---- buffered input ----------------------------------------------------
 * One input file read through a buffer; cz_getc gives -1 at the end (or
 * on a read error, with err set). total counts what has been consumed. */
struct cz_in {
    int fd;
    uint8_t *buf;
    size_t cap, pos, len;
    int eof, err;
    uint64_t total;           /* bytes handed out */
    uint8_t pb[16];           /* bytes given back (a decoder read ahead) */
    int npb;
};

static int cz_in_open(struct cz_in *in, int fd, size_t cap) {
    memset(in, 0, sizeof *in);
    in->fd = fd;
    in->cap = cap;
    in->buf = (uint8_t *)malloc(cap);
    return in->buf ? 0 : -1;
}

static void cz_in_close(struct cz_in *in) {
    free(in->buf);
    in->buf = 0;
}

/* refill; 0 when nothing more comes */
static int cz_in_fill(struct cz_in *in) {
    if (in->eof) return 0;
    in->pos = in->len = 0;
    for (;;) {
        ssize_t n = read(in->fd, in->buf, in->cap);
        if (n < 0) {
            if (errno == EINTR) continue;
            in->err = errno;
            in->eof = 1;
            return 0;
        }
        if (n == 0) { in->eof = 1; return 0; }
        in->len = (size_t)n;
        return 1;
    }
}

static int cz_getc(struct cz_in *in) {
    if (in->npb) { in->total++; return in->pb[--in->npb]; }
    if (in->pos == in->len && !cz_in_fill(in)) return -1;
    in->total++;
    return in->buf[in->pos++];
}

/* the next byte without taking it, or -1 */
static int cz_peekc(struct cz_in *in) {
    if (in->npb) return in->pb[in->npb - 1];
    if (in->pos == in->len && !cz_in_fill(in)) return -1;
    return in->buf[in->pos];
}

/* up to N bytes into P; how many came */
static size_t cz_read(struct cz_in *in, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n && in->npb) p[got++] = in->pb[--in->npb];
    while (got < n) {
        if (in->pos == in->len && !cz_in_fill(in)) break;
        size_t k = in->len - in->pos;
        if (k > n - got) k = n - got;
        memcpy(p + got, in->buf + in->pos, k);
        in->pos += k;
        got += k;
    }
    in->total += got;
    return got;
}

/* give C back: the next cz_getc returns it (the last given, first) */
static void cz_ungetc(struct cz_in *in, int c) {
    if (in->npb < (int)sizeof in->pb) {
        in->pb[in->npb++] = (uint8_t)c;
        in->total--;
    }
}

/* write all of P to FD; 0, or the errno */
static int cz_write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return errno ? errno : EIO;
        }
        if (w == 0) return EIO;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}


/* ---- doubles without the FPU ------------------------------------------
 *
 * bzip2 and xz print ratios computed in double precision ("%6.3f",
 * "%5.2f%%"); the tobyOS build has no floating point (-mno-sse). So the
 * few operations they use are done here on (sign, 53-bit mantissa, power
 * of two), rounded to nearest-even as the FPU rounds, and printed with
 * printf's %.Nf rounding of the exact binary value. */
struct cz_dbl { int neg; uint64_t m; int e; };   /* (-1)^neg * m * 2^e */

static int cz_bitlen128(unsigned __int128 v) {
    uint64_t hi = (uint64_t)(v >> 64), lo = (uint64_t)v;
    if (hi) return 128 - __builtin_clzll(hi);
    if (lo) return 64 - __builtin_clzll(lo);
    return 0;
}

/* V * 2^E, rounded to 53 bits; STICKY: nonzero bits below V were lost */
static struct cz_dbl cz_d_round(int neg, unsigned __int128 v, int e, int sticky) {
    struct cz_dbl d = {neg, 0, 0};
    int L = cz_bitlen128(v);
    if (L == 0) return d;
    if (L > 53) {
        int sh = L - 53;
        unsigned __int128 rem = v & (((unsigned __int128)1 << sh) - 1);
        unsigned __int128 half = (unsigned __int128)1 << (sh - 1);
        uint64_t m = (uint64_t)(v >> sh);
        int up = rem > half || (rem == half && (sticky || (m & 1)));
        if (rem == half && sticky) up = 1;
        if (up) m++;
        if (m == (1ULL << 53)) { m >>= 1; sh++; }
        d.m = m;
        d.e = e + sh;
    } else {
        d.m = (uint64_t)v;
        d.e = e;
    }
    return d;
}

/* A / B, rounded */
static struct cz_dbl cz_d_ratio(uint64_t a, uint64_t b) {
    struct cz_dbl d = {0, 0, 0};
    if (a == 0 || b == 0) return d;
    uint64_t q = a / b, r = a % b;
    unsigned __int128 m = q;
    int e = 0;
    /* fraction bits until 55 significant ones are had */
    while (cz_bitlen128(m) < 55) {
        int carry = (int)(r >> 63);
        r <<= 1;
        unsigned bit = 0;
        if (carry || r >= b) { r -= b; bit = 1; }
        m = m << 1 | bit;
        e--;
    }
    return cz_d_round(0, m, e, r != 0);
}

static struct cz_dbl cz_d_mul(struct cz_dbl x, uint64_t k) {
    return cz_d_round(x.neg, (unsigned __int128)x.m * k, x.e, 0);
}

/* 1 - X */
static struct cz_dbl cz_d_one_minus(struct cz_dbl x) {
    int E = x.e < 0 ? x.e : 0;
    if (-E > 120) E = -120;      /* X is far below 1: 1 - X rounds to 1 */
    unsigned __int128 one = (unsigned __int128)1 << (0 - E);
    unsigned __int128 xv = x.e - E >= 0 ? (unsigned __int128)x.m << (x.e - E) : (unsigned __int128)x.m >> (E - x.e);
    int sticky = x.e - E < 0 && (x.m & ((1ULL << (E - x.e)) - 1)) != 0;
    if (x.neg) return cz_d_round(0, one + xv, E, sticky);
    if (xv <= one) return cz_d_round(0, one - xv, E, 0);
    return cz_d_round(1, xv - one, E, 0);
}

/* printf("%W.Pf", X) */
static void cz_d_fmt(char *out, size_t cap, struct cz_dbl x, int width, int prec) {
    static const uint64_t p10[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};
    unsigned __int128 X = (unsigned __int128)x.m * p10[prec];
    unsigned __int128 ip;
    if (x.e >= 0) ip = X << x.e;
    else {
        int sh = -x.e;
        if (sh >= 127) ip = 0;
        else {
            unsigned __int128 rem = X & (((unsigned __int128)1 << sh) - 1);
            unsigned __int128 half = (unsigned __int128)1 << (sh - 1);
            ip = X >> sh;
            if (rem > half || (rem == half && (ip & 1))) ip++;
        }
    }
    /* (no 128-bit division: the values printed are far below 2^64) */
    uint64_t iv = (ip >> 64) ? UINT64_MAX : (uint64_t)ip;
    char digs[64];
    int n = 0;
    do { digs[n++] = (char)('0' + (int)(iv % 10)); iv /= 10; } while (iv);
    while (n <= prec) digs[n++] = '0';
    char num[80];
    int k = 0;
    if (x.neg) num[k++] = '-';
    for (int i = n - 1; i >= 0; i--) {
        num[k++] = digs[i];
        if (i == prec && prec > 0) num[k++] = '.';
    }
    num[k] = 0;
    snprintf(out, cap, "%*s", width, num);
}

#endif /* CZ_COMMON_INCLUDED */
