/* su_txio.c -- what the text commands share: head, tail, wc, cut, tr,
 * basename, dirname, expr and sleep (su_head.c ... su_sleep.c each
 * #include this file; the guard makes the second inclusion nothing).
 *
 * STANDARD OUTPUT, AS GNU's COMMANDS SEE IT THROUGH glibc's stdio. The
 * message a failed write gets depends on WHEN the write fails, and that
 * is decided by glibc's buffering, measured with coreutils 9.7 (logs/
 * utilgate/head.sh):
 *
 *   head -c 4095 z > /dev/full    head: write error: No space left on device
 *   head -c 4096 z > /dev/full    head: error writing 'standard output': ...
 *   head -n1 f nosuch > /dev/full head: write error            (no reason!)
 *
 * The first is buffered whole and fails when the stream is closed at exit
 * ("write error", with the reason); the second is a whole block, written
 * by the call that hands it over, which fails at once (head's own "error
 * writing"); the third is buffered, then flushed by the error message for
 * nosuch -- every message flushes standard output first -- and that
 * flush's failure is only remembered, so at exit there is nothing left to
 * write and the reason is gone ("write error" alone).
 *
 * So the output here is glibc's, done by hand rather than left to the
 * libc underneath (libtoby buffers differently, and writes its buffer out
 * before every system call): a 4096-byte block buffer (st_blksize of a
 * pipe, a file or /dev/full on Linux), nothing allocated before the first
 * write, so a first write of N bytes writes N rounded down to blocks at
 * once and buffers the rest; a later write fills the buffer, writes it if
 * more remains, writes whole blocks of the rest at once and buffers what
 * is left; a failed write empties the buffer and sets the error flag. At
 * exit the buffer is flushed and descriptor 1 closed, and GNU's
 * close_stdout rule decides the message: a failure earlier, or one now
 * with something pending (or for any reason but EBADF), is "write
 * error", with the reason only when it is a failure now. A command
 * that makes its standard output line buffered (wc does: each count
 * line is out as it is printed) writes the buffer at every newline. */

#ifndef SU_TXIO_INCLUDED
#define SU_TXIO_INCLUDED

#define TX_BLK 4096
/* not every command uses every helper */
#define TX_UNUSED __attribute__((unused))

static unsigned char tx_ob[TX_BLK];
static size_t tx_on;            /* bytes waiting in tx_ob */
static int tx_oalloc;           /* the buffer "exists" (after the first write) */
static int tx_oerr;             /* a write failed: ferror(stdout) */
static int tx_exit_failure = 1; /* the status a failure at exit gives */
static int tx_atexit_set;
static int tx_olinebuf;         /* line buffered: written out at each newline */

/* write all of P, as glibc's file write does: the count written, which
 * is short only when a write failed (errno says why) */
static TX_UNUSED size_t tx_wall(int fd, const void *p, size_t n) {
    const char *s = (const char *)p;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, s + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return done;
        }
        if (w == 0) { errno = EIO; return done; }
        done += (size_t)w;
    }
    return done;
}

/* fflush(stdout): 0, or -1 with errno set and the buffer emptied anyway */
static TX_UNUSED int tx_oflush(void) {
    if (!tx_on) return 0;
    size_t n = tx_on;
    tx_on = 0;
    if (tx_wall(1, tx_ob, n) < n) { tx_oerr = 1; return -1; }
    return 0;
}

/* fwrite(P, 1, N, stdout): how much was taken, short on a failed write */
static TX_UNUSED size_t tx_fwrite(const void *p, size_t n) {
    const unsigned char *s = (const unsigned char *)p;
    size_t to_do = n;
    if (n == 0) return 0;
    if (tx_oalloc) {
        size_t room = TX_BLK - tx_on;
        if (room > to_do) room = to_do;
        memcpy(tx_ob + tx_on, s, room);
        tx_on += room;
        s += room;
        to_do -= room;
    }
    if (to_do) {
        if (!tx_oalloc) tx_oalloc = 1;
        else if (tx_oflush() < 0) return n - to_do;
        size_t direct = to_do - to_do % TX_BLK;
        if (direct) {
            size_t w = tx_wall(1, s, direct);
            s += w;
            to_do -= w;
            if (w < direct) { tx_oerr = 1; return n - to_do; }
        }
        memcpy(tx_ob, s, to_do);
        tx_on = to_do;
        to_do = 0;
    }
    if (tx_olinebuf && memchr(p, '\n', n)) tx_oflush();
    return n;
}

/* putc(C, stdout): 0, or -1 when the full buffer could not be written */
static TX_UNUSED int tx_putc(int c) {
    if (!tx_oalloc) tx_oalloc = 1;
    else if (tx_on == TX_BLK && tx_oflush() < 0) return -1;
    tx_ob[tx_on++] = (unsigned char)c;
    if (tx_olinebuf && c == '\n') return tx_oflush();
    return 0;
}

static TX_UNUSED size_t tx_puts(const char *s) { return tx_fwrite(s, strlen(s)); }

/* printf(FMT, ...) to standard output, formatted first, then written */
static TX_UNUSED void tx_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static TX_UNUSED void tx_printf(const char *fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) { tx_fwrite(small, (size_t)n); return; }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    tx_fwrite(big, (size_t)n);
    free(big);
}

/* GNU's close_stdout, at exit */
static TX_UNUSED void tx_close_stdout(void) {
    int pending = tx_on != 0, prev = tx_oerr, fail = 0, e = 0;
    if (pending && tx_oflush() < 0) { fail = 1; e = errno; }
    if (close(1) < 0 && !fail) { fail = 1; e = errno; }
    if (prev || (fail && (pending || e != EBADF))) {
        if (!fail) e = 0;
        fprintf(stderr, "%s: write error", su_short);
        if (e) fprintf(stderr, ": %s", strerror(e));
        fputc('\n', stderr);
        _exit(tx_exit_failure);
    }
}

/* the commands call this first: the output is closed as GNU's is, on
 * every way out (a return from main, an exit after a message) */
static TX_UNUSED void tx_begin(int exit_failure) {
    tx_exit_failure = exit_failure;
    if (!tx_atexit_set) { tx_atexit_set = 1; atexit(tx_close_stdout); }
}

/* GNU's error(): standard output flushed first (its failure only
 * remembered), then "PROG: MESSAGE[: REASON]"; exit(STATUS) unless 0 */
static TX_UNUSED void tx_error(int status, int errnum, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static TX_UNUSED void tx_error(int status, int errnum, const char *fmt, ...) {
    tx_oflush();
    fprintf(stderr, "%s: ", su_short);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errnum) fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
    if (status) exit(status);
}

/* "Try 'PROG --help' for more information.", exit(STATUS) */
static TX_UNUSED void tx_try_help(int status) __attribute__((noreturn));
static TX_UNUSED void tx_try_help(int status) {
    tx_oflush();
    fprintf(stderr, "Try '%s --help' for more information.\n", su_name);
    exit(status);
}

/* NAME (tobyOS) 1.0, through the output above */
static TX_UNUSED int tx_version(void) {
    tx_printf("%s (tobyOS) 1.0\n", su_short);
    return 0;
}

/* ---- quoting names in messages, as gnulib's quotearg does in C ---------
 *
 * Measured through the messages of wc, head and the rest (the shared
 * su_quote* print bytes past 0x7f as they are, \a as \007, leave ':'
 * '=' '^' bare and end 'f3'$'\n''' with an empty pair; GNU's do not):
 *
 *   tx_quote   quote(): 'X', with \' \\ \a \b \f \n \r \t \v, and any
 *              other byte below 0x20 or from 0x7f up as \NNN.
 *   tx_quotef  quotef(): bare when every byte is a letter, a digit or
 *              one of % + , - . / @ ] _ { } (and ~ # not first);
 *              "X" when it holds a ' but none of " $ ` \ ! nor a
 *              control byte; else 'X', each ' as '\'', each run of
 *              control bytes (and bytes from 0x7f) as $'\n\001' between
 *              the quoted parts -- ''$'\n''a', 'a'$'\n' (no '' after the
 *              last run).
 *   tx_quoteaf quoteaf(): the same, but never bare.
 *
 * The results live in rotating buffers, so a message can hold several. */
static char *tx_qb[8];
static size_t tx_qcap[8];
static int tx_qslot;

static char *tx_qbuf(size_t need) {
    int k = tx_qslot++ & 7;
    if (tx_qcap[k] < need) {
        char *nb = (char *)realloc(tx_qb[k], need);
        if (!nb) { fputs("memory exhausted\n", stderr); exit(1); }
        tx_qb[k] = nb;
        tx_qcap[k] = need;
    }
    return tx_qb[k];
}

static int tx_qctrl(unsigned char c) { return c < 0x20 || c >= 0x7f; }

/* \a \b \f \n \r \t \v, or 3 octal digits; the count written */
static int tx_qesc(char *o, unsigned char c) {
    static const char let[] = "\aa\bb\ff\nn\rr\tt\vv";
    for (int i = 0; let[i]; i += 2)
        if ((unsigned char)let[i] == c) { o[0] = '\\'; o[1] = let[i + 1]; return 2; }
    o[0] = '\\';
    o[1] = (char)('0' + ((c >> 6) & 7));
    o[2] = (char)('0' + ((c >> 3) & 7));
    o[3] = (char)('0' + (c & 7));
    return 4;
}

static TX_UNUSED const char *tx_quote(const char *s) {
    char *b = tx_qbuf(strlen(s) * 4 + 3);
    size_t n = 0;
    b[n++] = '\'';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\'' || c == '\\') { b[n++] = '\\'; b[n++] = (char)c; }
        else if (tx_qctrl(c)) n += (size_t)tx_qesc(b + n, c);
        else b[n++] = (char)c;
    }
    b[n++] = '\'';
    b[n] = 0;
    return b;
}

static const char *tx_shquote(const char *s, int always) {
    int plain = *s != 0, ctrl = 0, squote = 0, dqbad = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (tx_qctrl(*p)) ctrl = 1;
        if (*p == '\'') squote = 1;
        if (strchr("\"$`\\!", *p)) dqbad = 1;
        if (!(isalnum(*p) || strchr("%+,-./@]_{}~#", *p))) plain = 0;
    }
    if (*s == '~' || *s == '#') plain = 0;
    size_t len = strlen(s);
    char *b = tx_qbuf(len * 10 + 8);
    size_t n = 0;
    if (plain) {
        if (always) b[n++] = '\'';
        memcpy(b + n, s, len);
        n += len;
        if (always) b[n++] = '\'';
        b[n] = 0;
        return b;
    }
    if (squote && !ctrl && !dqbad) {
        b[n++] = '"';
        memcpy(b + n, s, len);
        n += len;
        b[n++] = '"';
        b[n] = 0;
        return b;
    }
    int dollar = 0;
    b[n++] = '\'';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\'') {
            memcpy(b + n, "'\\''", 4);
            n += 4;
            dollar = 0;
        } else if (tx_qctrl(*p)) {
            if (!dollar) { memcpy(b + n, "'$'", 3); n += 3; dollar = 1; }
            n += (size_t)tx_qesc(b + n, *p);
        } else {
            if (dollar) { b[n++] = '\''; b[n++] = '\''; dollar = 0; }
            b[n++] = (char)*p;
        }
    }
    b[n++] = '\'';
    b[n] = 0;
    return b;
}

static TX_UNUSED const char *tx_quotef(const char *s) { return tx_shquote(s, 0); }
static TX_UNUSED const char *tx_quoteaf(const char *s) { return tx_shquote(s, 1); }

/* gnulib's argmatch for an option's argument: the index of the value ARG
 * names -- exactly, or as the prefix of only one -- else "invalid
 * argument 'X' for '--opt'" (or "ambiguous", when it begins several),
 * the valid values one per line, and the "Try" line: exit 1. */
static TX_UNUSED int tx_argmatch(const char *opt, const char *arg, const char *const *vals) {
    size_t len = strlen(arg);
    int hit = -1, nhit = 0;
    for (int k = 0; vals[k]; k++) {
        if (!strcmp(vals[k], arg)) return k;
        if (!strncmp(vals[k], arg, len)) { hit = k; nhit++; }
    }
    if (nhit == 1) return hit;
    tx_oflush();
    fprintf(stderr, "%s: %s argument %s for %s\nValid arguments are:\n",
            su_short, nhit ? "ambiguous" : "invalid", tx_quote(arg), tx_quote(opt));
    for (int k = 0; vals[k]; k++) fprintf(stderr, "  - %s\n", tx_quote(vals[k]));
    tx_try_help(1);
}

/* read(2), again after a signal */
static TX_UNUSED ssize_t tx_read(int fd, void *buf, size_t n) {
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

/* ---- numbers: gnulib's xstrtoumax, as GNU's commands answer it --------
 *
 * Measured through head -n, tail -n and the others' counts: blanks may
 * come first, then a '+' (never a '-': " -3" is refused), then decimal
 * digits; a value too large is the largest there is, and says so
 * (TX_NUM_OVERFLOW). With VALID (the suffix letters a command takes) the
 * digits may be followed by one suffix -- b 512, c 1, w 2, B 1024, and
 * k K m M g G t T P E Z Y R Q, powers of 1024 -- and, when VALID holds
 * '0', a power letter may be followed by "B" or "D" (powers of 1000) or
 * "iB" (1024 again): 1kB is 1000, 1KiB 1024, 1bB is wrong. A suffix with
 * no digits before it is one of it ("k" is 1024), but only when it is
 * the very first character (" k" and "+k" are wrong). Anything after is
 * TX_NUM_INVALID. */
#define TX_NUM_OK 0
#define TX_NUM_INVALID 1
#define TX_NUM_OVERFLOW 2

static TX_UNUSED int tx_mul(uintmax_t *v, uintmax_t m) {
    if (*v && m > UINTMAX_MAX / *v) { *v = UINTMAX_MAX; return TX_NUM_OVERFLOW; }
    *v *= m;
    return TX_NUM_OK;
}

static TX_UNUSED int tx_xstrtoumax(const char *s, uintmax_t *out, const char *valid) {
    const char *p = s;
    int rc = TX_NUM_OK;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') return TX_NUM_INVALID;
    const char *q = p;
    if (*q == '+') q++;
    uintmax_t v = 0;
    if (!isdigit((unsigned char)*q)) {
        if (!valid || !*s || !strchr(valid, *s)) return TX_NUM_INVALID;
        v = 1;
        p = s;
    } else {
        for (; isdigit((unsigned char)*q); q++) {
            unsigned d = (unsigned)(*q - '0');
            if (v > (UINTMAX_MAX - d) / 10) { v = UINTMAX_MAX; rc = TX_NUM_OVERFLOW; }
            else if (rc == TX_NUM_OK) v = v * 10 + d;
        }
        p = q;
    }
    if (*p) {
        if (!valid || !strchr(valid, *p)) { *out = v; return TX_NUM_INVALID; }
        int base = 1024, len = 1, power = -1;
        uintmax_t mult = 1;
        switch (*p) {
        case 'b': mult = 512; break;
        case 'B': mult = 1024; break;
        case 'c': mult = 1; break;
        case 'w': mult = 2; break;
        case 'k': case 'K': power = 1; break;
        case 'm': case 'M': power = 2; break;
        case 'g': case 'G': power = 3; break;
        case 't': case 'T': power = 4; break;
        case 'P': power = 5; break;
        case 'E': power = 6; break;
        case 'Z': power = 7; break;
        case 'Y': power = 8; break;
        case 'R': power = 9; break;
        case 'Q': power = 10; break;
        default: *out = v; return TX_NUM_INVALID;
        }
        if (power > 0 && strchr(valid, '0')) {
            if (p[1] == 'i' && p[2] == 'B') len = 3;
            else if (p[1] == 'B' || p[1] == 'D') { base = 1000; len = 2; }
        }
        int r2 = TX_NUM_OK;
        if (power < 0) r2 = tx_mul(&v, mult);
        else for (int i = 0; i < power && r2 == TX_NUM_OK; i++) r2 = tx_mul(&v, (uintmax_t)base);
        if (r2 != TX_NUM_OK) rc = r2;
        p += len;
        if (*p) { *out = v; return TX_NUM_INVALID; }
    }
    *out = v;
    return rc;
}

/* ---- a time interval, as strtod reads it, in nanoseconds ---------------
 *
 * Integer arithmetic only: the guest's programs are built without SSE
 * (no strtod). What strtod takes, measured through sleep and tail -s:
 * blanks, a sign, then decimal digits with a fraction and an exponent, or
 * 0x hex digits with a fraction and a binary p exponent, or inf/infinity,
 * or nan (which every caller here refuses) -- its END is where the number
 * stops. The value saturates at UINT64_MAX nanoseconds, which is forever.
 * Returns 0 and sets *NS, *NEG (a '-' was given, the value may be 0) and
 * *END, or -1 when there is no number at all. */
#define TX_NS_PER_S 1000000000ULL
#define TX_FOREVER (~0ULL)

static TX_UNUSED unsigned long long tx_sat_mul(unsigned long long a, unsigned long long b) {
    if (a == 0 || b == 0) return 0;
    if (a == TX_FOREVER || b == TX_FOREVER || b > TX_FOREVER / a) return TX_FOREVER;
    return a * b;
}

static TX_UNUSED unsigned long long tx_sat_add(unsigned long long a, unsigned long long b) {
    return a > TX_FOREVER - b ? TX_FOREVER : a + b;
}

static TX_UNUSED int tx_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static TX_UNUSED int tx_starts_ci(const char *p, const char *w) {
    for (; *w; p++, w++)
        if (tx_lower((unsigned char)*p) != *w) return 0;
    return 1;
}

static TX_UNUSED int tx_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tx_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* the exponent's digits after e or p, signed; without digits *PP stays
 * on the e or p, which strtod leaves unread */
static TX_UNUSED long tx_exponent(const char **pp) {
    const char *q = *pp + 1;
    long sign = 1, e = 0;
    if (*q == '+' || *q == '-') { if (*q == '-') sign = -1; q++; }
    if (*q < '0' || *q > '9') return 0;
    while (*q >= '0' && *q <= '9') {
        if (e < 100000) e = e * 10 + (*q - '0');
        q++;
    }
    *pp = q;
    return sign * e;
}

static TX_UNUSED int tx_strtons(const char *arg, unsigned long long *ns_out, int *neg_out,
                      int *nan_out, const char **end) {
    const char *p = arg;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    *nan_out = 0;
    unsigned long long m = 0, ns;
    long e = 0;
    if (tx_starts_ci(p, "inf")) {
        p += tx_starts_ci(p, "infinity") ? 8 : 3;
        *ns_out = TX_FOREVER;
        *neg_out = neg;
        *end = p;
        return 0;
    }
    if (tx_starts_ci(p, "nan")) {
        p += 3;
        if (*p == '(') {          /* nan(chars) */
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_') q++;
            if (*q == ')') p = q + 1;
        }
        *nan_out = 1;
        *ns_out = 0;
        *neg_out = neg;
        *end = p;
        return 0;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (tx_hexval((unsigned char)p[2]) >= 0 ||
         (p[2] == '.' && tx_hexval((unsigned char)p[3]) >= 0))) {
        p += 2;
        for (; tx_hexval((unsigned char)*p) >= 0; p++) {
            if (m < (1ULL << 56)) m = m * 16 + (unsigned)tx_hexval((unsigned char)*p);
            else e += 4;
        }
        if (*p == '.')
            for (p++; tx_hexval((unsigned char)*p) >= 0; p++)
                if (m < (1ULL << 56)) { m = m * 16 + (unsigned)tx_hexval((unsigned char)*p); e -= 4; }
        if (*p == 'p' || *p == 'P') e += tx_exponent(&p);
        /* m * 2^e seconds */
        ns = m;
        while (e < 0 && ns > TX_FOREVER / TX_NS_PER_S) { ns >>= 1; e++; }
        ns = tx_sat_mul(ns, TX_NS_PER_S);
        for (; e > 0 && ns != TX_FOREVER && ns; e--) ns = tx_sat_mul(ns, 2);
        for (; e < 0 && ns; e++) ns >>= 1;
    } else {
        int digits = 0;
        for (; *p >= '0' && *p <= '9'; p++, digits++) {
            if (m < 100000000000000000ULL) m = m * 10 + (unsigned)(*p - '0');
            else e++;
        }
        if (*p == '.')
            for (p++; *p >= '0' && *p <= '9'; p++, digits++)
                if (m < 100000000000000000ULL) { m = m * 10 + (unsigned)(*p - '0'); e--; }
        if (!digits) return -1;
        if (*p == 'e' || *p == 'E') e += tx_exponent(&p);
        /* m * 10^(e+9) nanoseconds */
        long k = e + 9;
        ns = m;
        for (; k > 0 && ns != TX_FOREVER && ns; k--) ns = tx_sat_mul(ns, 10);
        for (; k < 0 && ns; k++) ns /= 10;
    }
    *ns_out = ns;
    *neg_out = neg;
    *end = p;
    return 0;
}

/* sleep for NS nanoseconds (TX_FOREVER: forever), a day at a time */
static TX_UNUSED void tx_nap(unsigned long long ns) {
    const unsigned long long day = 86400ULL * TX_NS_PER_S;
    while (ns) {
        unsigned long long chunk = ns > day ? day : ns;
        struct timespec ts;
        ts.tv_sec = (time_t)(chunk / TX_NS_PER_S);
        ts.tv_nsec = (long)(chunk % TX_NS_PER_S);
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
        if (ns != TX_FOREVER) ns -= chunk;
    }
}

#endif /* SU_TXIO_INCLUDED */
