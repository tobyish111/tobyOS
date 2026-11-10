/* su_md5sum.c -- md5sum sha1sum sha224sum sha256sum sha384sum sha512sum
 * b2sum, as GNU coreutils 9.7's (logs/utilgate/staging/md5sum.sh and
 * friends). One engine, the algorithm picked by the name it was run as.
 *
 * PRINTING: "DIGEST  NAME" ('*' in place of the second blank with -b),
 * or with --tag "ALGO (NAME) = DIGEST" ("BLAKE2b-BITS" for b2sum -l
 * BITS short of 512). Standard input is "-". A name holding a backslash,
 * a newline or a carriage return is shown with those escaped (\\ \n \r)
 * and the whole line starts with a backslash -- unless -z, which ends each
 * line with a NUL and escapes nothing. A file that cannot be read is
 * "PROG: NAME: ERROR" and status 1, and the rest go on.
 *
 * OPTION CONFLICTS, in the order GNU tests them (measured): b2sum's
 * --length (a value up to 512 must be a multiple of 8; past 512, and 0,
 * mean 512; a value that is not a number is refused as it is read);
 * --tag then -t; -z with -c; --tag with -c; -b or -t with -c; and without
 * -c each of --ignore-missing, --status/--warn/--quiet (one setting: the
 * last of the three wins), --strict.
 *
 * CHECKING (-c), line by line, as measured against GNU's on odd input:
 *   - the line loses its newline and then one carriage return; an empty
 *     line, or one starting with '#', is passed over without a word;
 *   - blanks (space, tab) are skipped, then a backslash means the name is
 *     escaped (\\ \n \r, anything else makes the line bad);
 *   - "ALGO (NAME) = HEX": the tag, at most one space, '(' ... the LAST
 *     ')' of the line, blanks, '=', blanks, then exactly the digest to the
 *     end of the line. b2sum's "BLAKE2b-N" reads N as strtoumax(base 0)
 *     does (so 0x10 and 020 are 16) and must match the digest's length;
 *   - "HEX  NAME": the digest (b2sum: any even length up to 128 -- that
 *     sets the length; -l does not count when checking), then one blank
 *     (or NUL), then -- GNU's guard against names that start with a blank
 *     -- either a ' ' or '*' mode character, or none at all when the name
 *     follows the single blank directly. Which of the two layouts is in
 *     use is fixed by the first such line of the whole run, across every
 *     list given; a line in the other layout is then bad (or, after a
 *     single-blank line, the ' ' or '*' is part of the name). A line not
 *     tagged must be at least the digest plus two bytes long. The name is
 *     a C string: a NUL ends it.
 * Then "NAME: OK" (not with --quiet), "NAME: FAILED", or for a file that
 * cannot be read its error and "NAME: FAILED open or read" (--status
 * keeps the error, drops the line). --ignore-missing passes over a file
 * that does not exist. After each list: "WARNING: N line(s) is/are
 * improperly formatted", "N listed file(s) could not be read", "N computed
 * checksum(s) did NOT match" (not with --status); "LIST: no properly
 * formatted checksum lines found" when no line was good (even with
 * --status), and with --ignore-missing "LIST: no file was verified" when
 * none checked out. -w names each bad line: "LIST: N: improperly
 * formatted ALGO checksum line". A list that cannot be read is "LIST: read
 * error" (no reason given -- GNU's words, for a directory).
 *
 * The digests are su_cksum.c's ck_digest -- cksum -a's code -- so the two
 * can never disagree (su_cksum.c must be included before this file).
 * The old commands (programs/user_coreutil) printed a digest per file
 * and took no option but -c. */

#include "su_sm_common.c"   /* sm_quotef, sm_quote: GNU's quoting */

enum { SM_MD5, SM_SHA1, SM_SHA224, SM_SHA256, SM_SHA384, SM_SHA512, SM_B2 };

static const struct {
    const char *tag;    /* the --tag word, and the one in -w's message */
    int ck;             /* su_cksum.c's algorithm */
    int bytes;          /* the digest's length (b2sum: the longest) */
    const char *what;   /* --help's description */
} sm_algs[] = {
    { "MD5", CK_MD5, 16, "MD5 (128-bit)" },
    { "SHA1", CK_SHA1, 20, "SHA-1 (160-bit)" },
    { "SHA224", CK_SHA224, 28, "SHA-224" },
    { "SHA256", CK_SHA256, 32, "SHA-256" },
    { "SHA384", CK_SHA384, 48, "SHA-384" },
    { "SHA512", CK_SHA512, 64, "SHA-512" },
    { "BLAKE2b", CK_BLAKE2B, 64, "BLAKE2b (512-bit)" },
};

enum { SM_V_NORMAL, SM_V_STATUS, SM_V_QUIET, SM_V_WARN };

static struct {
    int alg;
    int b2bits;         /* b2sum -l, 512 when not given */
    int binary;         /* -1 neither -b nor -t, 0 -t, 1 -b */
    int tag, zero, check, ignore_missing, strict;
    int verbosity;      /* --status, --quiet, --warn: the last one given */
    int layout;         /* -1 not yet known, 0 "HEX  NAME", 1 "HEX NAME" */
} sm;

/* ---- reading lines from a descriptor -------------------------------------
 *
 * Not stdio: the native fopen refuses a directory, where GNU's opens it
 * and then fails to read ("LIST: read error"); su_open_read opens it as
 * Linux does, and read() then says EISDIR. A line may hold NULs. */
struct sm_rd {
    int fd;
    char buf[65536];
    size_t pos, len;
    int eof;
    char *line;
    size_t cap;
};

/* the next line into R->line (its newline gone), its length in *N;
 * 1, 0 at the end, -1 on a read error (errno set) */
static int sm_getline(struct sm_rd *r, size_t *n) {
    size_t used = 0;
    int any = 0;
    for (;;) {
        if (r->pos == r->len) {
            if (r->eof) break;
            ssize_t k = read(r->fd, r->buf, sizeof r->buf);
            if (k < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (k == 0) { r->eof = 1; break; }
            r->pos = 0;
            r->len = (size_t)k;
        }
        any = 1;
        char *nl = memchr(r->buf + r->pos, '\n', r->len - r->pos);
        size_t take = nl ? (size_t)(nl - (r->buf + r->pos)) : r->len - r->pos;
        if (used + take + 1 > r->cap) {
            size_t nc = r->cap ? r->cap : 256;
            while (nc < used + take + 1) nc *= 2;
            char *p = (char *)realloc(r->line, nc);
            if (!p) su_error(1, ENOMEM, "memory exhausted");
            r->line = p;
            r->cap = nc;
        }
        memcpy(r->line + used, r->buf + r->pos, take);
        used += take;
        r->pos += take;
        if (nl) { r->pos++; break; }
    }
    if (!any) return 0;
    r->line[used] = 0;
    *n = used;
    return 1;
}

/* ---- names ---------------------------------------------------------------- */

static int sm_needs_escape(const char *s) {
    return strpbrk(s, "\\\n\r") != 0;
}

static void sm_put_name(const char *s, int esc) {
    for (; *s; s++) {
        if (esc && *s == '\\') fputs("\\\\", stdout);
        else if (esc && *s == '\n') fputs("\\n", stdout);
        else if (esc && *s == '\r') fputs("\\r", stdout);
        else putchar(*s);
    }
}

/* undo the escaping of a name in place: 0, or -1 for an escape GNU does
 * not write (which makes the line bad) */
static int sm_unescape(char *s) {
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p != '\\') { *w++ = *p; continue; }
        p++;
        if (*p == '\\') *w++ = '\\';
        else if (*p == 'n') *w++ = '\n';
        else if (*p == 'r') *w++ = '\r';
        else return -1;
    }
    *w = 0;
    return 0;
}

/* ---- digests -------------------------------------------------------------- */

static void sm_hex(const uint8_t *d, int n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[2 * n] = 0;
}

static int sm_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* LEN hex digits at S into OUT: 0, or -1 if one is not a hex digit */
static int sm_unhex(const char *s, size_t len, uint8_t *out) {
    for (size_t i = 0; i < len; i += 2) {
        int hi = sm_hexval((unsigned char)s[i]), lo = sm_hexval((unsigned char)s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

/* the NBYTES-long digest of NAME ("-" is standard input) into OUT: 0, or
 * -1 with errno set when it cannot be opened or read */
static int sm_digest(const char *name, int nbytes, uint8_t *out) {
    int fd = strcmp(name, "-") ? su_open_read(name, 0) : 0;
    if (fd < 0) return -1;
    uint64_t size;
    int n = ck_digest(fd, sm_algs[sm.alg].ck, nbytes, out, &size);
    int e = errno;
    if (fd > 0) close(fd);
    errno = e;
    return n < 0 ? -1 : 0;
}

/* ---- one FILE's line ------------------------------------------------------ */

static int sm_sum_one(const char *name) {
    uint8_t d[64];
    int nb = sm.alg == SM_B2 ? sm.b2bits / 8 : sm_algs[sm.alg].bytes;
    if (sm_digest(name, nb, d) != 0) {
        su_error(0, errno, "%s", sm_quotef(name));
        return 1;
    }
    char hex[130];
    sm_hex(d, nb, hex);
    int esc = !sm.zero && sm_needs_escape(name);
    if (esc) putchar('\\');
    if (sm.tag) {
        fputs(sm_algs[sm.alg].tag, stdout);
        if (sm.alg == SM_B2 && sm.b2bits != 512) printf("-%d", sm.b2bits);
        fputs(" (", stdout);
        sm_put_name(name, esc);
        printf(") = %s", hex);
    } else {
        printf("%s %c", hex, sm.binary == 1 ? '*' : ' ');
        sm_put_name(name, esc);
    }
    putchar(sm.zero ? '\0' : '\n');
    return 0;
}

/* ---- --check -------------------------------------------------------------- */

/* "0x10", "020", " +16": what strtoumax(S, &end, 0) makes of it, with
 * *END where it stopped; -1 for no number, or a negative or overflowing
 * one (none of which can be a digest length) */
static long sm_strtou0(const char *s, const char **end) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    int base = 10;
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X') && sm_hexval((unsigned char)p[2]) >= 0) {
        base = 16;
        p += 2;
    } else if (*p == '0') {
        base = 8;
    }
    const char *start = p;
    unsigned long long v = 0;
    int over = 0;
    for (;; p++) {
        int d = sm_hexval((unsigned char)*p);
        if (d < 0 || d >= base) break;
        if (v > (~0ULL - (unsigned)d) / (unsigned)base) over = 1;
        else v = v * (unsigned)base + (unsigned)d;
    }
    *end = p;
    if (p == start || over || (neg && v)) return -1;
    if (v > 1000000) return 1000000;     /* far past any length: refused */
    return (long)v;
}

/* LINE (LEN bytes, NUL-terminated) as a checksum line: its digest into
 * WANT (*NB bytes) and its name; 0, or -1 if it is not well formed */
static int sm_parse(char *line, size_t len, uint8_t *want, int *nb, char **name) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    int esc = 0;
    if (i < len && line[i] == '\\') { esc = 1; i++; }
    char *s = line + i;
    size_t n = len - i;

    /* tagged */
    const char *tag = sm_algs[sm.alg].tag;
    size_t tl = strlen(tag);
    if (n >= tl && !strncmp(s, tag, tl)) {
        char *p = s + tl;
        long bits = sm.alg == SM_B2 ? 512 : sm_algs[sm.alg].bytes * 8;
        if (sm.alg == SM_B2 && *p == '-') {
            const char *e;
            bits = sm_strtou0(p + 1, &e);
            if (bits <= 0 || bits > 512 || bits % 8) return -1;
            p = (char *)e;
        }
        if (*p == ' ') p++;
        if (*p != '(') return -1;
        char *nm = p + 1;
        char *close = 0;
        for (char *q = nm; q < line + len; q++) if (*q == ')') close = q;
        if (!close) return -1;
        char *q = close + 1;
        while (q < line + len && (*q == ' ' || *q == '\t')) q++;
        if (q == line + len || *q != '=') return -1;
        q++;
        while (q < line + len && (*q == ' ' || *q == '\t')) q++;
        size_t dl = (size_t)(line + len - q);
        if (dl != (size_t)bits / 4 || sm_unhex(q, dl, want) != 0) return -1;
        *close = 0;
        if (esc && sm_unescape(nm) != 0) return -1;
        *nb = (int)(bits / 8);
        *name = nm;
        return 0;
    }

    /* "HEX  NAME" */
    int fixed = sm.alg != SM_B2;
    size_t hexlen = (size_t)sm_algs[sm.alg].bytes * 2;
    if (fixed && n < hexlen + 2) return -1;
    size_t dl = 0;
    while (dl < n && s[dl] != ' ' && s[dl] != '\t' && s[dl] != '\0') dl++;
    if (fixed ? dl != hexlen : (dl == 0 || dl % 2 || dl > hexlen)) return -1;
    if (sm_unhex(s, dl, want) != 0) return -1;
    if (dl == n) return -1;                  /* no blank after it */
    size_t j = dl + 1;
    if (n - j == 1 || (s[j] != ' ' && s[j] != '*')) {
        if (sm.layout == 0) return -1;
        sm.layout = 1;
    } else if (sm.layout != 1) {
        sm.layout = 0;
        j++;
    }
    char *nm = s + j;
    if (esc && sm_unescape(nm) != 0) return -1;
    *nb = (int)(dl / 2);
    *name = nm;
    return 0;
}

static int sm_check_list(const char *list) {
    int is_stdin = !strcmp(list, "-");
    const char *shown = is_stdin ? "standard input" : list;
    struct sm_rd *r = (struct sm_rd *)calloc(1, sizeof *r);
    if (!r) su_error(1, ENOMEM, "memory exhausted");
    r->fd = is_stdin ? 0 : su_open_read(list, 0);
    if (r->fd < 0) {
        su_error(0, errno, "%s", sm_quotef(list));
        free(r);
        return 1;
    }
    long lineno = 0, bad = 0, good = 0, mismatch = 0, unread = 0, ok = 0;
    size_t len;
    int g, readerr = 0;
    while ((g = sm_getline(r, &len)) != 0) {
        if (g < 0) { readerr = 1; break; }
        lineno++;
        char *line = r->line;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (len == 0 || line[0] == '#') continue;
        uint8_t want[64], got[64];
        int nb;
        char *name;
        if (sm_parse(line, len, want, &nb, &name) != 0) {
            bad++;
            if (sm.verbosity == SM_V_WARN)
                su_error(0, 0, "%s: %ld: improperly formatted %s checksum line",
                         sm_quotef(shown), lineno, sm_algs[sm.alg].tag);
            continue;
        }
        good++;
        int esc = sm_needs_escape(name);
        if (sm_digest(name, nb, got) != 0) {
            if (sm.ignore_missing && errno == ENOENT) continue;
            unread++;
            su_error(0, errno, "%s", sm_quotef(name));
            if (sm.verbosity != SM_V_STATUS) {
                if (esc) putchar('\\');
                sm_put_name(name, esc);
                fputs(": FAILED open or read\n", stdout);
            }
            continue;
        }
        int match = memcmp(got, want, (size_t)nb) == 0;
        if (match) ok++;
        else mismatch++;
        if (sm.verbosity == SM_V_STATUS || (match && sm.verbosity == SM_V_QUIET)) continue;
        if (esc) putchar('\\');
        sm_put_name(name, esc);
        fputs(match ? ": OK\n" : ": FAILED\n", stdout);
    }
    if (!is_stdin) close(r->fd);
    free(r->line);
    free(r);
    if (readerr) {
        su_error(0, 0, "%s: read error", sm_quotef(shown));
        return 1;
    }
    if (!good) {
        su_error(0, 0, "%s: no properly formatted checksum lines found", sm_quotef(shown));
        return 1;
    }
    if (sm.verbosity != SM_V_STATUS) {
        if (bad)
            su_error(0, 0, "WARNING: %ld line%s improperly formatted", bad,
                     bad == 1 ? " is" : "s are");
        if (unread)
            su_error(0, 0, "WARNING: %ld listed file%s could not be read", unread,
                     unread == 1 ? "" : "s");
        if (mismatch)
            su_error(0, 0, "WARNING: %ld computed checksum%s did NOT match", mismatch,
                     mismatch == 1 ? "" : "s");
        if (sm.ignore_missing && !ok)
            su_error(0, 0, "%s: no file was verified", sm_quotef(shown));
    }
    return (mismatch || unread || (sm.strict && bad) || (sm.ignore_missing && !ok)) ? 1 : 0;
}

/* ---- the command ------------------------------------------------------------ */

static void sm_usage_error(const char *msg) {
    su_error(0, 0, "%s", msg);
    su_try_help(1);
}

static int sm_sum_main(int argc, char **argv, int alg) {
    enum { O_TAG = 0x100, O_IGN, O_QUIET, O_STATUS, O_STRICT };
    static const struct su_option longs_b2[] = {
        /* GNU's order where it shows: "--s" lists --status then
         * --strict, "--t" --tag then --text */
        { "binary", SU_NO_ARG, 0, 'b' },
        { "check", SU_NO_ARG, 0, 'c' },
        { "ignore-missing", SU_NO_ARG, 0, O_IGN },
        { "length", SU_REQ_ARG, 0, 'l' },
        { "quiet", SU_NO_ARG, 0, O_QUIET },
        { "status", SU_NO_ARG, 0, O_STATUS },
        { "strict", SU_NO_ARG, 0, O_STRICT },
        { "tag", SU_NO_ARG, 0, O_TAG },
        { "text", SU_NO_ARG, 0, 't' },
        { "warn", SU_NO_ARG, 0, 'w' },
        { "zero", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    /* the same without --length (index 3), for the fixed-length sums */
    static struct su_option longs_fixed[sizeof longs_b2 / sizeof longs_b2[0] - 1];
    if (!longs_fixed[0].name) {
        int k = 0;
        for (int i = 0; longs_b2[i].name; i++)
            if (longs_b2[i].val != 'l') longs_fixed[k++] = longs_b2[i];
    }
    memset(&sm, 0, sizeof sm);
    sm.alg = alg;
    sm.b2bits = 512;
    sm.binary = -1;
    sm.layout = -1;
    long length = -1;
    const char *lenarg = 0;     /* the -l the length came from, for its message */
    int c;
    const char *shorts = alg == SM_B2 ? "bcl:twz" : "bctwz";
    while ((c = su_getopt_long(argc, argv, shorts, alg == SM_B2 ? longs_b2 : longs_fixed, 0)) != -1) {
        switch (c) {
        case 'b': sm.binary = 1; break;
        case 't': sm.binary = 0; break;
        case 'c': sm.check = 1; break;
        case 'z': sm.zero = 1; break;
        case 'w': sm.verbosity = SM_V_WARN; break;
        case O_QUIET: sm.verbosity = SM_V_QUIET; break;
        case O_STATUS: sm.verbosity = SM_V_STATUS; break;
        case O_STRICT: sm.strict = 1; break;
        case O_IGN: sm.ignore_missing = 1; break;
        case O_TAG: sm.tag = 1; sm.binary = 1; break;
        case 'l': {
            /* decimal, blanks and '+' allowed; too big for any type is
             * still "past 512" */
            const char *p = su_optarg;
            while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
            if (*p == '+') p++;
            if (!isdigit((unsigned char)*p)) su_error(1, 0, "invalid length: %s", sm_quote(su_optarg));
            unsigned long long v = 0;
            for (; isdigit((unsigned char)*p); p++)
                if (v < 1000000) v = v * 10 + (unsigned)(*p - '0');
            if (*p) su_error(1, 0, "invalid length: %s", sm_quote(su_optarg));
            length = (long)v;
            lenarg = su_optarg;
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n", su_name);
            printf("Print the %s checksum of each FILE, or check the checksums\n"
                   "listed in them.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n", sm_algs[alg].what);
            if (alg == SM_B2)
                printf("Mandatory arguments to long options are mandatory for short options too.\n");
            printf("  -b, --binary          mark each file as read in binary mode ('*')\n"
                   "  -c, --check           check the checksums listed in each FILE\n");
            if (alg == SM_B2)
                printf("  -l, --length=BITS     the digest's length: a multiple of 8, at most 512\n");
            printf("      --tag             lines in the BSD layout: ALGO (FILE) = DIGEST\n"
                   "  -t, --text            mark each file as read in text mode (the default)\n"
                   "  -z, --zero            end each line with a NUL instead of a newline,\n"
                   "                          and leave file names unescaped\n"
                   "\n"
                   "Only when checking:\n"
                   "      --ignore-missing  say nothing of listed files that do not exist\n"
                   "      --quiet           print nothing for a file that checks out\n"
                   "      --status          print nothing; the exit status tells\n"
                   "      --strict          fail when a line is not a checksum line\n"
                   "  -w, --warn            name each line that is not a checksum line\n"
                   "\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n"
                   "\n"
                   "A list to check is what this program printed: a digest, a blank, a\n"
                   "mode character (' ' text, '*' binary) and the file name per line.\n"
                   "Text and binary mode read the same bytes here.\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (length >= 0) {
        if (length > 512 || length == 0) length = 512;
        else if (length % 8) {
            su_error(0, 0, "invalid length: %s", sm_quote(lenarg));
            su_error(1, 0, "length is not a multiple of 8");
        }
        sm.b2bits = (int)length;
    }
    if (sm.tag && sm.binary == 0) sm_usage_error("--tag does not support --text mode");
    if (sm.check && sm.zero) sm_usage_error("the --zero option is not supported when verifying checksums");
    if (sm.check && sm.tag) sm_usage_error("the --tag option is meaningless when verifying checksums");
    if (sm.check && sm.binary >= 0)
        sm_usage_error("the --binary and --text options are meaningless when verifying checksums");
    if (!sm.check) {
        if (sm.ignore_missing)
            sm_usage_error("the --ignore-missing option is meaningful only when verifying checksums");
        if (sm.verbosity == SM_V_STATUS)
            sm_usage_error("the --status option is meaningful only when verifying checksums");
        if (sm.verbosity == SM_V_WARN)
            sm_usage_error("the --warn option is meaningful only when verifying checksums");
        if (sm.verbosity == SM_V_QUIET)
            sm_usage_error("the --quiet option is meaningful only when verifying checksums");
        if (sm.strict)
            sm_usage_error("the --strict option is meaningful only when verifying checksums");
    }
    int rc = 0;
    char *dash = (char *)"-";
    char **ops = su_optind < argc ? argv + su_optind : &dash;
    int nops = su_optind < argc ? argc - su_optind : 1;
    for (int i = 0; i < nops; i++)
        rc |= sm.check ? sm_check_list(ops[i]) : sm_sum_one(ops[i]);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}

static int do_md5sum(int argc, char **argv) { return sm_sum_main(argc, argv, SM_MD5); }
static int do_sha1sum(int argc, char **argv) { return sm_sum_main(argc, argv, SM_SHA1); }
static int do_sha224sum(int argc, char **argv) { return sm_sum_main(argc, argv, SM_SHA224); }
static int do_sha256sum(int argc, char **argv) { return sm_sum_main(argc, argv, SM_SHA256); }
static int do_sha384sum(int argc, char **argv) { return sm_sum_main(argc, argv, SM_SHA384); }
static int do_sha512sum(int argc, char **argv) { return sm_sum_main(argc, argv, SM_SHA512); }
static int do_b2sum(int argc, char **argv) { return sm_sum_main(argc, argv, SM_B2); }
