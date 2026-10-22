/* su_base64.c -- base64, base32 and basenc, as GNU coreutils 9.7's
 * (logs/utilgate/staging/base64.sh, base32.sh, basenc.sh).
 *
 * ENCODING reads the input in blocks of 30720 bytes (a multiple of 3, 4
 * and 5, so only the last block can end in a partial group) and prints
 * each block's text, broken into lines of -w COLS (76; 0 for one line
 * with no newline at all), with a newline after the last line. basenc
 * --z85 takes only a multiple of 4 bytes: a block that is not one is
 * "invalid input (length must be multiple of 4 characters)" -- after the
 * blocks before it were printed, as GNU's does.
 *
 * DECODING skips newlines (and with -i every byte that is neither in the
 * alphabet nor '=') and decodes group by group, printing what it decoded
 * before any "invalid input" (status 1). Measured on odd input:
 *   - base64 and base32 take an unpadded last group (YQ, MFRGG): it is
 *     padded with '=' -- but only when the input's last byte is not '='
 *     (with -i: the last byte -i kept), so "YQ=" fails and "YQ=\n" and
 *     "YQ" do not;
 *   - the bits a padded group leaves over must be zero (YR== fails, after
 *     printing its 'a');
 *   - base64 prints the bytes of a failing group up to the failure, a
 *     short last group included; base32 does too for a whole group, but a
 *     short last group it could not pad prints nothing;
 *   - base16 takes either case, z85 refuses a group worth 2^32 or more;
 *   - base32hex decodes W X Y Z as base32's 22..25 (unless -i drops
 *     them), and base64url refuses a '+' or '/' for the whole 5600-byte
 *     block it is read in, printing nothing of that block.
 * -w is checked as it is read: a negative or non-numeric COLS is "invalid
 * wrap size"; a size past what intmax_t holds wraps nothing. An input
 * that cannot be read is "read error: REASON", with no name, as GNU's.
 *
 * The old commands (programs/user_coreutil) knew base64, base32 and
 * base16, decoded nothing but well-formed input and had no -i. */

#include "su_sm_common.c"   /* sm_quotef, sm_quote: GNU's quoting */

enum { SM_B_B64, SM_B_B64URL, SM_B_B32, SM_B_B32HEX, SM_B_B16, SM_B_B2M, SM_B_B2L, SM_B_Z85 };

#define SM_B_BLOCK 30720

static const char sm_b_a64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char sm_b_a64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char sm_b_a32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static const char sm_b_a32hex[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
static const char sm_b_a16[] = "0123456789ABCDEF";
static const char sm_b_az85[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#";

static struct {
    int enc;
    intmax_t wrap;
    size_t col;
    int ignore;
    signed char val[256];   /* a byte's value when decoding, or -1 */
    char in_alpha[256];     /* in the alphabet proper: what -i keeps */
} sm_b;

static void sm_b_setup_alphabet(void) {
    const char *a = sm_b.enc == SM_B_B64 ? sm_b_a64 : sm_b.enc == SM_B_B64URL ? sm_b_a64url
                  : sm_b.enc == SM_B_B32 ? sm_b_a32 : sm_b.enc == SM_B_B32HEX ? sm_b_a32hex
                  : sm_b.enc == SM_B_B16 ? sm_b_a16 : sm_b.enc == SM_B_Z85 ? sm_b_az85 : "01";
    memset(sm_b.val, -1, sizeof sm_b.val);
    memset(sm_b.in_alpha, 0, sizeof sm_b.in_alpha);
    for (int i = 0; a[i]; i++) {
        sm_b.val[(unsigned char)a[i]] = (signed char)i;
        sm_b.in_alpha[(unsigned char)a[i]] = 1;
    }
    if (sm_b.enc == SM_B_B16)       /* decoding takes lower case too */
        for (int i = 10; i < 16; i++) {
            sm_b.val['a' + i - 10] = (signed char)i;
            sm_b.in_alpha['a' + i - 10] = 1;
        }
    /* GNU's base32hex decodes W X Y Z -- not in its alphabet -- as
     * base32 does (22..25): C5H6W=== is "abk" (measured). -i still
     * drops them as the garbage they are. */
    if (sm_b.enc == SM_B_B32HEX && !sm_b.ignore)
        for (int i = 0; i < 4; i++) sm_b.val['W' + i] = (signed char)(22 + i);
}

/* ---- encoding ---------------------------------------------------------------- */

static void sm_b_out(char c) {
    putchar(c);
    if (sm_b.wrap > 0 && (intmax_t)++sm_b.col >= sm_b.wrap) {
        putchar('\n');
        sm_b.col = 0;
    }
}

static void sm_b_encode_block(const unsigned char *p, size_t n) {
    switch (sm_b.enc) {
    case SM_B_B64: case SM_B_B64URL: {
        const char *a = sm_b.enc == SM_B_B64 ? sm_b_a64 : sm_b_a64url;
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) |
                         (i + 2 < n ? p[i + 2] : 0);
            sm_b_out(a[v >> 18 & 63]);
            sm_b_out(a[v >> 12 & 63]);
            sm_b_out(i + 1 < n ? a[v >> 6 & 63] : '=');
            sm_b_out(i + 2 < n ? a[v & 63] : '=');
        }
        break;
    }
    case SM_B_B32: case SM_B_B32HEX: {
        const char *a = sm_b.enc == SM_B_B32 ? sm_b_a32 : sm_b_a32hex;
        for (size_t i = 0; i < n; i += 5) {
            size_t k = n - i < 5 ? n - i : 5;
            uint64_t v = 0;
            for (size_t j = 0; j < 5; j++) v = v << 8 | (j < k ? p[i + j] : 0);
            /* 1..5 bytes make 2, 4, 5, 7 or 8 characters */
            static const int chars[6] = { 0, 2, 4, 5, 7, 8 };
            for (int j = 0; j < 8; j++)
                sm_b_out(j < chars[k] ? a[v >> (35 - 5 * j) & 31] : '=');
        }
        break;
    }
    case SM_B_B16:
        for (size_t i = 0; i < n; i++) {
            sm_b_out(sm_b_a16[p[i] >> 4]);
            sm_b_out(sm_b_a16[p[i] & 15]);
        }
        break;
    case SM_B_B2M: case SM_B_B2L:
        for (size_t i = 0; i < n; i++)
            for (int b = 0; b < 8; b++)
                sm_b_out((p[i] >> (sm_b.enc == SM_B_B2M ? 7 - b : b) & 1) ? '1' : '0');
        break;
    case SM_B_Z85:
        for (size_t i = 0; i + 4 <= n; i += 4) {
            uint32_t v = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 |
                         (uint32_t)p[i + 2] << 8 | p[i + 3];
            char c[5];
            for (int j = 4; j >= 0; j--) { c[j] = sm_b_az85[v % 85]; v /= 85; }
            for (int j = 0; j < 5; j++) sm_b_out(c[j]);
        }
        break;
    }
}

/* fill BUF with up to N bytes of FD, as fread fills a block: short only
 * at the end. The count, or -1 (errno set). */
static ssize_t sm_b_fill(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, buf + got, n - got);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (k == 0) break;
        got += (size_t)k;
    }
    return (ssize_t)got;
}

static int sm_b_encode(int fd) {
    static unsigned char buf[SM_B_BLOCK];
    for (;;) {
        ssize_t n = sm_b_fill(fd, buf, sizeof buf);
        if (n < 0) su_error(1, errno, "read error");
        if (n == 0) break;
        if (sm_b.enc == SM_B_Z85 && n % 4)
            su_error(1, 0, "invalid input (length must be multiple of 4 characters)");
        sm_b_encode_block(buf, (size_t)n);
        if ((size_t)n < sizeof buf) break;
    }
    if (sm_b.wrap > 0 && sm_b.col > 0) putchar('\n');
    return 0;
}

/* ---- decoding ---------------------------------------------------------------- */

static void sm_b_invalid(void) {
    su_error(1, 0, "invalid input");
}

static int sm_b_v(unsigned char c) { return sm_b.val[c]; }

/* a whole group of base64: its bytes, or "invalid input" after the
 * bytes before the failure */
static void sm_b_dec64(const unsigned char *g) {
    int v0 = sm_b_v(g[0]), v1 = sm_b_v(g[1]);
    if (v0 < 0 || v1 < 0) sm_b_invalid();
    putchar(v0 << 2 | v1 >> 4);
    if (g[2] == '=') {
        if (g[3] != '=' || (v1 & 15)) sm_b_invalid();
        return;
    }
    int v2 = sm_b_v(g[2]);
    if (v2 < 0) sm_b_invalid();
    putchar((v1 & 15) << 4 | v2 >> 2);
    if (g[3] == '=') {
        if (v2 & 3) sm_b_invalid();
        return;
    }
    int v3 = sm_b_v(g[3]);
    if (v3 < 0) sm_b_invalid();
    putchar((v2 & 3) << 6 | v3);
}

static void sm_b_dec32(const unsigned char *g) {
    int v[8];
    for (int i = 0; i < 8; i++) v[i] = sm_b_v(g[i]);
    /* the groups' padded forms: '=' from character 2, 4, 5 or 7 on */
#define SM_B_PAD_FROM(k) do { for (int j_ = (k); j_ < 8; j_++) if (g[j_] != '=') sm_b_invalid(); } while (0)
    if (v[0] < 0 || v[1] < 0) sm_b_invalid();
    putchar(v[0] << 3 | v[1] >> 2);
    if (g[2] == '=') {
        SM_B_PAD_FROM(2);
        if (v[1] & 3) sm_b_invalid();
        return;
    }
    if (v[2] < 0 || v[3] < 0) sm_b_invalid();
    putchar((v[1] & 3) << 6 | v[2] << 1 | v[3] >> 4);
    if (g[4] == '=') {
        SM_B_PAD_FROM(4);
        if (v[3] & 15) sm_b_invalid();
        return;
    }
    if (v[4] < 0) sm_b_invalid();
    putchar((v[3] & 15) << 4 | v[4] >> 1);
    if (g[5] == '=') {
        SM_B_PAD_FROM(5);
        if (v[4] & 1) sm_b_invalid();
        return;
    }
    if (v[5] < 0 || v[6] < 0) sm_b_invalid();
    putchar((v[4] & 1) << 7 | v[5] << 2 | v[6] >> 3);
    if (g[7] == '=') {
        if (v[6] & 7) sm_b_invalid();
        return;
    }
    if (v[7] < 0) sm_b_invalid();
    putchar((v[6] & 7) << 5 | v[7]);
#undef SM_B_PAD_FROM
}

static void sm_b_decz85(const unsigned char *g) {
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) {
        int d = sm_b_v(g[i]);
        if (d < 0) sm_b_invalid();
        v = v * 85 + (uint64_t)d;
    }
    if (v > 0xffffffffu) sm_b_invalid();
    putchar((int)(v >> 24 & 255));
    putchar((int)(v >> 16 & 255));
    putchar((int)(v >> 8 & 255));
    putchar((int)(v & 255));
}

static void sm_b_decode_group(const unsigned char *g) {
    switch (sm_b.enc) {
    case SM_B_B64: case SM_B_B64URL: sm_b_dec64(g); break;
    case SM_B_B32: case SM_B_B32HEX: sm_b_dec32(g); break;
    case SM_B_B16: {
        int hi = sm_b_v(g[0]), lo = sm_b_v(g[1]);
        if (hi < 0 || lo < 0) sm_b_invalid();
        putchar(hi << 4 | lo);
        break;
    }
    case SM_B_B2M: case SM_B_B2L: {
        int byte = 0;
        for (int i = 0; i < 8; i++) {
            int b = sm_b_v(g[i]);
            if (b < 0) sm_b_invalid();
            byte |= b << (sm_b.enc == SM_B_B2M ? 7 - i : i);
        }
        putchar(byte);
        break;
    }
    case SM_B_Z85: sm_b_decz85(g); break;
    }
}

/* decoding reads blocks of 5600 bytes, newlines and all (measured: a
 * '+' after N good base64url characters keeps the output of the whole
 * 5600-byte blocks before it, nothing of its own block) */
#define SM_B_DECBLOCK 5600

static int sm_b_decode(int fd) {
    static unsigned char buf[SM_B_DECBLOCK];
    int gsize = sm_b.enc == SM_B_B64 || sm_b.enc == SM_B_B64URL ? 4 : sm_b.enc == SM_B_B16 ? 2
              : sm_b.enc == SM_B_Z85 ? 5 : 8;
    int pads = sm_b.enc <= SM_B_B32HEX;      /* the families with '=' */
    unsigned char g[8];
    int k = 0;
    int last = -1;                       /* the last byte that counts */
    for (;;) {
        ssize_t n = sm_b_fill(fd, buf, sizeof buf);
        if (n < 0) su_error(1, errno, "read error");
        if (n == 0) break;
        /* base64url refuses base64's own '+' and '/' block by block,
         * before decoding any of the block */
        if (sm_b.enc == SM_B_B64URL && !sm_b.ignore &&
            (memchr(buf, '+', (size_t)n) || memchr(buf, '/', (size_t)n)))
            sm_b_invalid();
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = buf[i];
            if (sm_b.ignore && !sm_b.in_alpha[c] && c != '=') continue;
            last = c;
            if (c == '\n') continue;
            g[k++] = c;
            if (k == gsize) {
                sm_b_decode_group(g);
                k = 0;
            }
        }
        if ((size_t)n < sizeof buf) break;
    }
    if (k) {
        if (pads && last != '=') {
            while (k < gsize) g[k++] = '=';
            sm_b_decode_group(g);
        } else if (sm_b.enc == SM_B_B64 || sm_b.enc == SM_B_B64URL) {
            /* a short group that cannot be padded: its bytes up to where
             * it runs out (YQ= prints 'a'), then the failure */
            int v0 = sm_b_v(g[0]), v1 = k > 1 ? sm_b_v(g[1]) : -1;
            if (k > 1 && v0 >= 0 && v1 >= 0) {
                putchar(v0 << 2 | v1 >> 4);
                if (k > 2 && g[2] != '=' && sm_b_v(g[2]) >= 0)
                    putchar((v1 & 15) << 4 | sm_b_v(g[2]) >> 2);
            }
            sm_b_invalid();
        } else {
            sm_b_invalid();
        }
    }
    return 0;
}

/* ---- the commands ------------------------------------------------------------ */

/* -w COLS: decimal, leading blanks and a sign allowed; negative refused;
 * a value too big for intmax_t wraps nothing (GNU's, measured) */
static void sm_b_wrap_arg(const char *s) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (!isdigit((unsigned char)*p)) su_error(1, 0, "invalid wrap size: %s", sm_quote(s));
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p || (neg && (v || over))) su_error(1, 0, "invalid wrap size: %s", sm_quote(s));
    sm_b.wrap = (over || v > (uintmax_t)INTMAX_MAX) ? 0 : (intmax_t)v;
}

static int sm_b_main(int argc, char **argv, int fixed) {
    enum { O_B64 = 0x100, O_B64URL, O_B32, O_B32HEX, O_B16, O_B2M, O_B2L, O_Z85 };
    static const struct su_option longs_basenc[] = {
        /* GNU's order, which "--base"'s ambiguity message shows */
        { "base64", SU_NO_ARG, 0, O_B64 },
        { "base64url", SU_NO_ARG, 0, O_B64URL },
        { "base32", SU_NO_ARG, 0, O_B32 },
        { "base32hex", SU_NO_ARG, 0, O_B32HEX },
        { "base16", SU_NO_ARG, 0, O_B16 },
        { "base2msbf", SU_NO_ARG, 0, O_B2M },
        { "base2lsbf", SU_NO_ARG, 0, O_B2L },
        { "decode", SU_NO_ARG, 0, 'd' },
        { "ignore-garbage", SU_NO_ARG, 0, 'i' },
        { "wrap", SU_REQ_ARG, 0, 'w' },
        { "z85", SU_NO_ARG, 0, O_Z85 },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    static const struct su_option longs_fixed[] = {
        { "decode", SU_NO_ARG, 0, 'd' },
        { "ignore-garbage", SU_NO_ARG, 0, 'i' },
        { "wrap", SU_REQ_ARG, 0, 'w' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    memset(&sm_b, 0, sizeof sm_b);
    sm_b.enc = fixed;
    sm_b.wrap = 76;
    int decode = 0, c;
    while ((c = su_getopt_long(argc, argv, "diw:", fixed < 0 ? longs_basenc : longs_fixed, 0)) != -1) {
        switch (c) {
        case 'd': decode = 1; break;
        case 'i': sm_b.ignore = 1; break;
        case 'w': sm_b_wrap_arg(su_optarg); break;
        case O_B64: sm_b.enc = SM_B_B64; break;
        case O_B64URL: sm_b.enc = SM_B_B64URL; break;
        case O_B32: sm_b.enc = SM_B_B32; break;
        case O_B32HEX: sm_b.enc = SM_B_B32HEX; break;
        case O_B16: sm_b.enc = SM_B_B16; break;
        case O_B2M: sm_b.enc = SM_B_B2M; break;
        case O_B2L: sm_b.enc = SM_B_B2L; break;
        case O_Z85: sm_b.enc = SM_B_Z85; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]\n", su_name);
            if (fixed < 0)
                printf("Encode FILE, or standard input, in the chosen encoding to standard\n"
                       "output, or decode it.\n");
            else
                printf("Encode FILE, or standard input, in %s to standard output, or\n"
                       "decode it.\n", fixed == SM_B_B64 ? "base64" : "base32");
            printf("\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "Mandatory arguments to long options are mandatory for short options too.\n");
            if (fixed < 0)
                printf("      --base64          RFC 4648's base64 (section 4), as base64 does\n"
                       "      --base64url       base64 with the file-name and URL safe alphabet (5)\n"
                       "      --base32          RFC 4648's base32 (section 6), as base32 does\n"
                       "      --base32hex       base32 with the extended hex alphabet (7)\n"
                       "      --base16          hexadecimal, upper case (8)\n"
                       "      --base2msbf       each byte as 8 bits, the most significant first\n"
                       "      --base2lsbf       each byte as 8 bits, the least significant first\n");
            printf("  -d, --decode          decode the input instead of encoding it\n"
                   "  -i, --ignore-garbage  when decoding, skip bytes outside the alphabet\n"
                   "  -w, --wrap=COLS       break encoded lines after COLS characters (76);\n"
                   "                          0 for no line breaks\n");
            if (fixed < 0)
                printf("      --z85             ZeroMQ's Z85 (spec 32), an ascii85 variant: the\n"
                       "                          input to encode must be a multiple of 4 bytes,\n"
                       "                          the input to decode a multiple of 5 characters\n");
            printf("      --help        show this help and exit\n"
                   "      --version     show the version and exit\n"
                   "\n"
                   "Decoding takes newlines anywhere in the encoded text; any other byte\n"
                   "outside the alphabet is an error unless -i skips it.\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (sm_b.enc < 0) {
        su_error(0, 0, "missing encoding type");
        su_try_help(1);
    }
    if (argc - su_optind > 1) {
        su_error(0, 0, "extra operand %s", sm_quote(argv[su_optind + 1]));
        su_try_help(1);
    }
    sm_b_setup_alphabet();
    const char *name = su_optind < argc ? argv[su_optind] : "-";
    int fd = 0;
    if (strcmp(name, "-")) {
        fd = su_open_read(name, 0);
        if (fd < 0) su_error(1, errno, "%s", sm_quotef(name));
    }
    if (decode) sm_b_decode(fd);
    else sm_b_encode(fd);
    if (fd > 0) close(fd);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}

static int do_base64(int argc, char **argv) { return sm_b_main(argc, argv, SM_B_B64); }
static int do_base32(int argc, char **argv) { return sm_b_main(argc, argv, SM_B_B32); }
static int do_basenc(int argc, char **argv) { return sm_b_main(argc, argv, -1); }
