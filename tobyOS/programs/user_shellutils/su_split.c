/* su_split.c -- split, as GNU coreutils 9.7's (logs/utilgate/
 * split.sh): FILE (or standard input) cut into files PREFIXaa, PREFIXab,
 * ... (PREFIX "x") -- by lines (-l, 1000 by default, or the old -N), by
 * bytes (-b), by bytes holding whole records where they fit (-C), or
 * into N chunks (-n N, K/N, l/N, l/K/N, r/N, r/K/N); K/N writes only the
 * Kth chunk, to standard output. Each piece can go to a shell command
 * instead (--filter, with $FILE its name).
 *
 * WHAT WAS MEASURED (logs/utilgate/split.sh and probes):
 *
 * Names. Suffixes count in a, z (default), 0..9 (-d, --numeric-
 * suffixes[=FROM]) or 0..f (-x, --hex-suffixes[=FROM]); the last of these
 * picks the digits, a FROM stays. With no -a and no FROM the length
 * widens as it runs out: when the first counting digit would become the
 * last symbol it is kept and the count goes on with one more digit
 * (xyz, xzaaa .. xzyzz, xzzaaaa; x89, x9000 .. x9899, x990000). An
 * explicit -a (0 is no -a) or a FROM fixes the length and running out is
 * "output file suffixes exhausted". -n N picks a length that holds N
 * names (at least 2); with -a too small for them it is "the suffix length
 * needs to be at least L" -- counting FROM in only when FROM < N, as GNU
 * does. A FROM longer than the length (leading zeros aside) is refused.
 * (GNU 9.7 misreads a FROM with the hex letters a-f -- --hex-suffixes=fe
 * gives xfe, xf2, xf3 -- reading past its digit table; this split counts
 * xfe, xff and then runs out.)
 *
 * -C. A piece takes, of the next bytes that still fit, everything up to
 * the LAST separator among them; when there is none it ends -- unless it
 * is empty, and then it takes all the bytes that fit (a record longer
 * than the size is cut). So the tail of a long record can share a file
 * with whole records after it, and a last record with no separator
 * starts a new file when the current one is not empty.
 *
 * -n. N chunks of the input's size (a regular file's, or what a pipe
 * held: it is read whole first), the first size % N chunks one byte
 * larger. l/ gives each record to the chunk its first byte falls in, so
 * a long record leaves empty chunks behind it; r/ deals records in turn.
 * Every chunk is a file, even an empty one, unless -e. A directory is
 * "cannot determine file size" where the size is needed.
 *
 * Options. -l, -b, -C, -n and -N each set the way to split, and a second
 * one (even the same option) is "cannot split in more than one way";
 * the old -N may be repeated, the last one counting, and its digits run
 * on through one argument (-3d4 is 34 lines). Sizes take b k K m M ... Q
 * with B (1000) or iB (1024) after a letter; numbers too large are as
 * large as can be; 0 is refused. Errors come in option order, the way
 * conflict found before the bad number after it.
 *
 * --filter runs "$SHELL -c COMMAND" (/bin/sh when SHELL is unset) for
 * each piece; a filter that exits early just stops being written to (a
 * SIGPIPE death is no error); one that fails ends split with its status,
 * a signal with 128+N. An output that is the input (same inode) is
 * refused before it is truncated.
 *
 * The old split had -a -b -d -l and wrote at most 1000-line pieces. */

enum { TSP_NONE, TSP_BYTES, TSP_LINEBYTES, TSP_LINES, TSP_CHUNKB, TSP_CHUNKL, TSP_RR };

struct tsp_out {
    int fd;
    pid_t pid;
    int dead;
    char *name;
    size_t blen;
    unsigned char buf[8192];
};

static struct {
    int type, obsolete;
    uintmax_t units, k;
    int sep, sep_set;
    int digits;                 /* 'a', 'd' or 'x' */
    const char *from;
    uintmax_t suflen;
    int suflen_set;
    const char *addsuf, *filter, *shell;
    int elide, unbuffered, verbose;
    const char *inname, *prefix;
    int in_fd;
    int in_reg;
    dev_t in_dev;
    ino_t in_ino;
    /* names */
    unsigned char *idx;
    size_t len, fixed, base;
    int widen, named;
    /* input buffer */
    unsigned char *ib;
    size_t ipos, ilen, icap;
    int ieof;
} tsp;

static const char *tsp_alpha(void) {
    return tsp.digits == 'd' ? "0123456789"
         : tsp.digits == 'x' ? "0123456789abcdef" : "abcdefghijklmnopqrstuvwxyz";
}

static void tsp_conflict(void) {
    su_error(0, 0, "cannot split in more than one way");
    su_try_help(1);
}

static void tsp_type(int t) {
    if (tsp.type != TSP_NONE) tsp_conflict();
    tsp.type = t;
}

/* a count (lines, chunks): blanks and '+' allowed; too large is the
 * largest; -1 when it is not a number */
static int tsp_count(const char *s, uintmax_t *out) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    uintmax_t v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        v = v > (UINTMAX_MAX - d) / 10 ? UINTMAX_MAX : v * 10 + d;
    }
    if (*p) return -1;
    *out = v;
    return 0;
}

static uintmax_t tsp_mul(uintmax_t v, uintmax_t m) {
    return v && m > UINTMAX_MAX / v ? UINTMAX_MAX : v * m;
}

/* a SIZE for -b and -C, or exits with WHAT */
static uintmax_t tsp_size(const char *s, const char *what) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '+') p++;
    uintmax_t v = 1;
    if (isdigit((unsigned char)*p)) {
        v = 0;
        for (; isdigit((unsigned char)*p); p++) {
            unsigned d = (unsigned)(*p - '0');
            v = v > (UINTMAX_MAX - d) / 10 ? UINTMAX_MAX : v * 10 + d;
        }
    } else if (!*p || !strchr("bkKmMGTPEZYRQ", *p)) {
        su_error(1, 0, "%s: %s", what, su_quote(s));
    }
    if (*p) {
        static const char pw[] = "kKmMGTPEZYRQ";
        const char *w = strchr(pw, *p);
        if (*p == 'b') { v = tsp_mul(v, 512); p++; }
        else if (w) {
            int power = w - pw < 2 ? 1 : w - pw < 4 ? 2 : (int)(w - pw) - 1;
            uintmax_t base = 1024;
            p++;
            if (p[0] == 'B') { base = 1000; p++; }
            else if (p[0] == 'i' && p[1] == 'B') p += 2;
            for (int i = 0; i < power; i++) v = tsp_mul(v, base);
        }
        if (*p) su_error(1, 0, "%s: %s", what, su_quote(s));
    }
    if (v == 0) su_error(1, 0, "%s: %s", what, su_quote(s));
    return v;
}

static void tsp_chunks(const char *arg) {
    const char *s = arg;
    if (!strncmp(s, "l/", 2)) { tsp_type(TSP_CHUNKL); s += 2; }
    else if (!strncmp(s, "r/", 2)) { tsp_type(TSP_RR); s += 2; }
    else tsp_type(TSP_CHUNKB);
    const char *slash = strchr(s, '/');
    uintmax_t n;
    if (slash) {
        /* K: a plain number up to the '/' -- one too large is an error */
        const char *p = s;
        while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
        if (*p == '+') p++;
        if (!isdigit((unsigned char)*p)) su_error(1, 0, "invalid number of chunks: %s", su_quote(s));
        uintmax_t k = 0;
        int over = 0;
        for (; isdigit((unsigned char)*p); p++) {
            unsigned d = (unsigned)(*p - '0');
            if (k > (UINTMAX_MAX - d) / 10) over = 1;
            else k = k * 10 + d;
        }
        if (p != slash) su_error(1, 0, "invalid number of chunks: %s", su_quote(s));
        if (over) su_error(1, ERANGE, "invalid number of chunks: %s", su_quote(s));
        if (tsp_count(slash + 1, &n) || n == 0)
            su_error(1, 0, "invalid number of chunks: %s", su_quote(slash + 1));
        if (k == 0 || k > n) {
            char *ks = strndup(s, (size_t)(slash - s));
            su_error(1, 0, "invalid chunk number: %s", su_quote(ks ? ks : s));
        }
        tsp.k = k;
    } else {
        if (tsp_count(s, &n) || n == 0) su_error(1, 0, "invalid number of chunks: %s", su_quote(s));
        tsp.k = 0;
    }
    tsp.units = n;
}

static void tsp_from(const char *arg, int hex) {
    if (!*arg) return;              /* --numeric-suffixes= is no FROM */
    const char *ok = hex ? "0123456789abcdef" : "0123456789";
    if (arg[strspn(arg, ok)]) {
        su_error(0, 0, "%s: invalid start value for %s suffix", su_quote(arg),
                 hex ? "hexadecimal" : "numerical");
        su_try_help(1);
    }
    tsp.from = arg;
}

/* the digits of X in base B */
static size_t tsp_ndigits(uintmax_t x, size_t b) {
    size_t n = 1;
    while (x >= b) { x /= b; n++; }
    return n;
}

static uintmax_t tsp_from_value(void) {
    uintmax_t v = 0;
    size_t b = strlen(tsp_alpha());
    for (const char *p = tsp.from; *p; p++) {
        unsigned d = (unsigned)(*p <= '9' ? *p - '0' : *p - 'a' + 10);
        if (v > (UINTMAX_MAX - d) / b) return UINTMAX_MAX;
        v = v * b + d;
    }
    return v;
}

/* the next output name, malloc'd */
static char *tsp_next_name(void) {
    const char *al = tsp_alpha();
    size_t b = tsp.base;
    if (tsp.named) {
        size_t i = tsp.len;
        for (;;) {
            if (i == tsp.fixed) su_error(1, 0, "output file suffixes exhausted");
            i--;
            if (++tsp.idx[i] < b) break;
            tsp.idx[i] = 0;
        }
        if (tsp.widen && tsp.idx[tsp.fixed] == b - 1) {
            /* keep the last symbol and count on with one more digit */
            size_t nl = tsp.len + 2;
            unsigned char *ni = (unsigned char *)realloc(tsp.idx, nl);
            if (!ni) su_error(1, ENOMEM, "memory exhausted");
            tsp.idx = ni;
            tsp.fixed++;
            memset(tsp.idx + tsp.fixed, 0, nl - tsp.fixed);
            tsp.len = nl;
        }
    }
    tsp.named = 1;
    size_t pl = strlen(tsp.prefix), al2 = tsp.addsuf ? strlen(tsp.addsuf) : 0;
    char *n = (char *)malloc(pl + tsp.len + al2 + 1);
    if (!n) su_error(1, ENOMEM, "memory exhausted");
    memcpy(n, tsp.prefix, pl);
    for (size_t i = 0; i < tsp.len; i++) n[pl + i] = al[tsp.idx[i]];
    if (al2) memcpy(n + pl + tsp.len, tsp.addsuf, al2);
    n[pl + tsp.len + al2] = 0;
    return n;
}

static const char *tsp_signame(int sig) {
    static const char *const names[] = {
        0, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL",
        "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT",
        "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF",
        "WINCH", "POLL", "PWR", "SYS"
    };
    static char buf[16];
    if (sig > 0 && sig < 32) return names[sig];
    if (sig >= 34 && sig <= 64) {
        if (sig - 34 <= 15) snprintf(buf, sizeof buf, sig == 34 ? "RTMIN" : "RTMIN+%d", sig - 34);
        else snprintf(buf, sizeof buf, sig == 64 ? "RTMAX" : "RTMAX-%d", 64 - sig);
        return buf;
    }
    snprintf(buf, sizeof buf, "%d", sig);
    return buf;
}

/* every output open, so a filter's child closes its siblings' pipes */
static struct tsp_out **tsp_all;
static size_t tsp_nall;
static struct tsp_out *tsp_one[1];

static void tsp_open(struct tsp_out *o) {
    o->name = tsp_next_name();
    o->blen = 0;
    o->dead = 0;
    o->pid = 0;
    if (tsp.filter) {
        if (tsp.verbose) { printf("executing with FILE=%s\n", su_quotef(o->name)); fflush(stdout); }
        int fds[2];
        if (pipe(fds) < 0) su_error(1, errno, "failed to create pipe");
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid < 0) su_error(1, errno, "fork system call failed");
        if (pid == 0) {
            close(fds[1]);
            if (fds[0] != 0) { dup2(fds[0], 0); close(fds[0]); }
            for (size_t i = 0; i < tsp_nall; i++)
                if (tsp_all[i] && tsp_all[i] != o && tsp_all[i]->fd > 0) close(tsp_all[i]->fd);
            if (tsp.in_fd > 0) close(tsp.in_fd);
            setenv("FILE", o->name, 1);
            signal(SIGPIPE, SIG_DFL);
            char *av[4] = { (char *)tsp.shell, (char *)"-c", (char *)tsp.filter, 0 };
            execv(tsp.shell, av);
            su_error(1, errno, "failed to run command: \"%s -c %s\"", tsp.shell, tsp.filter);
        }
        close(fds[0]);
        o->fd = fds[1];
        o->pid = pid;
        return;
    }
    if (tsp.verbose) { printf("creating file %s\n", su_quoteaf(o->name)); fflush(stdout); }
    int fd = open(o->name, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) su_error(1, errno, "%s", su_quotef(o->name));
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (tsp.in_reg && st.st_dev == tsp.in_dev && st.st_ino == tsp.in_ino)
            su_error(1, 0, "%s would overwrite input; aborting", su_quoteaf(o->name));
        if (S_ISREG(st.st_mode) && ftruncate(fd, 0) < 0)
            su_error(1, errno, "%s", su_quotef(o->name));
    }
    o->fd = fd;
}

static void tsp_flush(struct tsp_out *o) {
    size_t off = 0;
    while (off < o->blen && !o->dead) {
        ssize_t w = write(o->fd, o->buf + off, o->blen - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (o->pid && errno == EPIPE) { o->dead = 1; break; }
            su_error(1, errno, "%s", su_quotef(o->name));
        }
        off += (size_t)w;
    }
    o->blen = 0;
}

static void tsp_write(struct tsp_out *o, const unsigned char *p, size_t n) {
    if (o->fd < 0) tsp_open(o);
    while (n) {
        if (o->dead) return;
        size_t room = sizeof o->buf - o->blen;
        size_t take = n < room ? n : room;
        memcpy(o->buf + o->blen, p, take);
        o->blen += take;
        p += take;
        n -= take;
        if (o->blen == sizeof o->buf) tsp_flush(o);
    }
}

static void tsp_close(struct tsp_out *o) {
    if (o->fd < 0) return;
    tsp_flush(o);
    if (close(o->fd) < 0 && !o->pid) su_error(1, errno, "%s", su_quotef(o->name));
    o->fd = -1;
    if (o->pid) {
        int st = 0;
        while (waitpid(o->pid, &st, 0) < 0 && errno == EINTR) {}
        o->pid = 0;
        if (WIFSIGNALED(st)) {
            int sig = WTERMSIG(st);
            if (sig != SIGPIPE) {
                su_error(0, 0, "with FILE=%s, signal %s from command: %s",
                         su_quotef(o->name), tsp_signame(sig), tsp.filter);
                exit(128 + sig);
            }
        } else if (WIFEXITED(st) && WEXITSTATUS(st)) {
            int code = WEXITSTATUS(st);
            su_error(0, 0, "with FILE=%s, exit %d from command: %s",
                     su_quotef(o->name), code, tsp.filter);
            exit(code);
        }
    }
    free(o->name);
    o->name = 0;
}

/* K/N's output: standard output, named "-" in its errors as GNU's is */
static struct tsp_out tsp_so;

/* ---- the input ---- */

static void tsp_read_err(int e) {
    su_error(1, e, "%s", su_quotef(tsp.inname));
}

/* make at least N bytes available (fewer only at the end); the count */
static size_t tsp_fill(size_t n) {
    if (tsp.ipos > 0 && tsp.ipos == tsp.ilen) tsp.ipos = tsp.ilen = 0;
    while (tsp.ilen - tsp.ipos < n && !tsp.ieof) {
        if (tsp.ipos > 0) {
            memmove(tsp.ib, tsp.ib + tsp.ipos, tsp.ilen - tsp.ipos);
            tsp.ilen -= tsp.ipos;
            tsp.ipos = 0;
        }
        if (tsp.ilen == tsp.icap) {
            size_t nc = tsp.icap ? tsp.icap * 2 : 65536;
            unsigned char *nb = (unsigned char *)realloc(tsp.ib, nc);
            if (!nb) su_error(1, ENOMEM, "memory exhausted");
            tsp.ib = nb;
            tsp.icap = nc;
        }
        ssize_t r = read(tsp.in_fd, tsp.ib + tsp.ilen, tsp.icap - tsp.ilen);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) tsp_read_err(errno);
        if (r == 0) { tsp.ieof = 1; break; }
        tsp.ilen += (size_t)r;
    }
    return tsp.ilen - tsp.ipos;
}

/* -l and -b: a new piece every UNITS records or bytes */
static void tsp_by_count(int lines) {
    struct tsp_out *o = (struct tsp_out *)calloc(1, sizeof *o);
    if (!o) su_error(1, ENOMEM, "memory exhausted");
    o->fd = -1;
    tsp_one[0] = o;
    tsp_all = tsp_one;
    tsp_nall = 1;
    uintmax_t done = 0;           /* records or bytes in the current piece */
    for (;;) {
        size_t avail = tsp_fill(1);
        if (!avail) break;
        unsigned char *p = tsp.ib + tsp.ipos;
        size_t take;
        if (lines) {
            size_t i = 0;
            while (i < avail) {
                unsigned char *q = (unsigned char *)memchr(p + i, tsp.sep, avail - i);
                if (!q) { i = avail; break; }
                i = (size_t)(q - p) + 1;
                if (++done == tsp.units) break;
            }
            take = i;
        } else {
            uintmax_t left = tsp.units - done;
            take = left < avail ? (size_t)left : avail;
            done += take;
        }
        tsp_write(o, p, take);
        tsp.ipos += take;
        if (done == tsp.units) { tsp_close(o); done = 0; }
    }
    tsp_close(o);
}

/* -C */
static void tsp_line_bytes(void) {
    struct tsp_out *o = (struct tsp_out *)calloc(1, sizeof *o);
    if (!o) su_error(1, ENOMEM, "memory exhausted");
    o->fd = -1;
    tsp_one[0] = o;
    tsp_all = tsp_one;
    tsp_nall = 1;
    size_t max = tsp.units > (uintmax_t)SIZE_MAX / 2 ? SIZE_MAX / 2 : (size_t)tsp.units;
    size_t filled = 0;
    for (;;) {
        size_t room = max - filled;
        size_t avail = tsp_fill(room);
        if (!avail) break;
        unsigned char *p = tsp.ib + tsp.ipos;
        size_t take;
        if (avail < room) {
            /* the input ends short of the room left: all of it goes here,
             * separator or not (when it would just fill the room, it does
             * not -- measured) */
            take = avail;
        } else {
            size_t last = (size_t)-1;
            for (size_t i = room; i > 0; i--)
                if (p[i - 1] == (unsigned char)tsp.sep) { last = i - 1; break; }
            if (last != (size_t)-1) take = last + 1;
            else if (filled) { tsp_close(o); filled = 0; continue; }
            else take = room;
        }
        tsp_write(o, p, take);
        tsp.ipos += take;
        filled += take;
        if (filled == max) { tsp_close(o); filled = 0; }
    }
    tsp_close(o);
}

/* the whole input in memory, for the chunk modes on a non-regular file */
static void tsp_slurp(void) {
    for (;;) {
        if (tsp.ilen == tsp.icap) {
            size_t nc = tsp.icap ? tsp.icap * 2 : 65536;
            unsigned char *nb = (unsigned char *)realloc(tsp.ib, nc);
            if (!nb) su_error(1, ENOMEM, "memory exhausted");
            tsp.ib = nb;
            tsp.icap = nc;
        }
        ssize_t r = read(tsp.in_fd, tsp.ib + tsp.ilen, tsp.icap - tsp.ilen);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) su_error(1, errno, "%s: cannot determine file size", su_quotef(tsp.inname));
        if (r == 0) break;
        tsp.ilen += (size_t)r;
    }
    tsp.ieof = 1;
}

/* chunk I's (0-based) first byte, of N chunks over TOTAL bytes */
static uintmax_t tsp_chunk_start(uintmax_t i, uintmax_t n, uintmax_t total) {
    uintmax_t q = total / n, r = total % n;
    return i * q + (i < r ? i : r);
}

static void tsp_chunk_modes(void) {
    uintmax_t total;
    if (tsp.in_reg) {
        struct stat st;
        fstat(tsp.in_fd, &st);
        off_t cur = lseek(tsp.in_fd, 0, SEEK_CUR);
        total = (uintmax_t)(st.st_size - (cur > 0 ? cur : 0));
    } else {
        tsp_slurp();
        total = tsp.ilen;
    }
    uintmax_t n = tsp.units;
    struct tsp_out *o = (struct tsp_out *)calloc(1, sizeof *o);
    if (!o) su_error(1, ENOMEM, "memory exhausted");
    o->fd = -1;
    tsp_one[0] = o;
    tsp_all = tsp_one;
    tsp_nall = 1;
    uintmax_t pos = 0;              /* input offset of tsp.ib[tsp.ipos] */
    if (tsp.type == TSP_CHUNKB) {
        uintmax_t first = tsp.k ? tsp.k - 1 : 0, last = tsp.k ? tsp.k : n;
        for (uintmax_t c = first; c < last; c++) {
            uintmax_t s = tsp_chunk_start(c, n, total), e = tsp_chunk_start(c + 1, n, total);
            /* skip to S */
            while (pos < s) {
                size_t a = tsp_fill(1);
                if (!a) break;
                size_t t = (uintmax_t)a < s - pos ? a : (size_t)(s - pos);
                tsp.ipos += t;
                pos += t;
            }
            if (!tsp.k && !(tsp.elide && e == s)) tsp_open(o);
            while (pos < e) {
                size_t a = tsp_fill(1);
                if (!a) break;
                size_t t = (uintmax_t)a < e - pos ? a : (size_t)(e - pos);
                if (tsp.k) tsp_write(&tsp_so, tsp.ib + tsp.ipos, t);
                else tsp_write(o, tsp.ib + tsp.ipos, t);
                tsp.ipos += t;
                pos += t;
            }
            tsp_close(o);
        }
        return;
    }
    /* l/: each record to the chunk its first byte is in */
    uintmax_t cur = 0;              /* the chunk being written */
    int opened = 0;                 /* whether chunk CUR's file is made */
    uintmax_t end = tsp_chunk_start(1, n, total);
    for (;;) {
        size_t a = tsp_fill(1);
        if (!a) break;
        /* the record starting at POS */
        unsigned char *p = tsp.ib + tsp.ipos;
        if (pos >= end) {
            while (pos >= end && cur + 1 < n) {
                if (!tsp.k) {
                    if (!opened && !tsp.elide) tsp_open(o);
                    tsp_close(o);
                }
                opened = 0;
                cur++;
                end = tsp_chunk_start(cur + 1, n, total);
            }
        }
        int mine = !tsp.k || cur == tsp.k - 1;
        if (!tsp.k && !opened) { tsp_open(o); opened = 1; }
        /* write through the record's end */
        for (;;) {
            unsigned char *q = (unsigned char *)memchr(p, tsp.sep, a);
            size_t t = q ? (size_t)(q - p) + 1 : a;
            if (mine) {
                if (tsp.k) tsp_write(&tsp_so, p, t);
                else tsp_write(o, p, t);
            }
            tsp.ipos += t;
            pos += t;
            if (q) break;
            a = tsp_fill(1);
            if (!a) break;
            p = tsp.ib + tsp.ipos;
        }
        if (tsp.k && cur > tsp.k - 1) break;
    }
    if (!tsp.k) {
        for (;;) {
            if (!opened && !tsp.elide) tsp_open(o);
            tsp_close(o);
            opened = 0;
            if (++cur >= n) break;
        }
    }
}

/* r/: records dealt in turn */
static void tsp_round_robin(void) {
    uintmax_t n = tsp.units;
    struct tsp_out **outs = 0;
    if (!tsp.k) {
        if (n > SIZE_MAX / sizeof *outs) su_error(1, ENOMEM, "memory exhausted");
        outs = (struct tsp_out **)calloc((size_t)n, sizeof *outs);
        if (!outs) su_error(1, ENOMEM, "memory exhausted");
        tsp_all = outs;
        tsp_nall = (size_t)n;
    }
    uintmax_t i = 0;
    for (;;) {
        size_t a = tsp_fill(1);
        if (!a) break;
        uintmax_t slot = i % n;
        struct tsp_out *o = 0;
        if (!tsp.k) {
            if (!outs[slot]) {
                outs[slot] = (struct tsp_out *)calloc(1, sizeof **outs);
                if (!outs[slot]) su_error(1, ENOMEM, "memory exhausted");
                outs[slot]->fd = -1;
            }
            o = outs[slot];
        }
        int mine = !tsp.k || slot == tsp.k - 1;
        for (;;) {
            unsigned char *p = tsp.ib + tsp.ipos;
            unsigned char *q = (unsigned char *)memchr(p, tsp.sep, a);
            size_t t = q ? (size_t)(q - p) + 1 : a;
            if (mine) {
                if (tsp.k) tsp_write(&tsp_so, p, t);
                else tsp_write(o, p, t);
            }
            tsp.ipos += t;
            if (q) break;
            a = tsp_fill(1);
            if (!a) break;
        }
        if (o && tsp.unbuffered) tsp_flush(o);
        i++;
    }
    if (tsp.k) return;
    /* the files no record reached, then all of them closed in order */
    for (uintmax_t s = 0; s < n; s++) {
        if (!outs[s]) {
            if (tsp.elide) continue;
            outs[s] = (struct tsp_out *)calloc(1, sizeof **outs);
            if (!outs[s]) su_error(1, ENOMEM, "memory exhausted");
            outs[s]->fd = -1;
            tsp_open(outs[s]);
        }
    }
    for (uintmax_t s = 0; s < n; s++) if (outs[s]) tsp_close(outs[s]);
}

static int do_split(int argc, char **argv) {
    enum { ADDSUF_OPT = 0x100, FILTER_OPT, NUMSUF_OPT, HEXSUF_OPT, VERBOSE_OPT };
    static const struct su_option longs[] = {
        { "bytes", SU_REQ_ARG, 0, 'b' },
        { "lines", SU_REQ_ARG, 0, 'l' },
        { "line-bytes", SU_REQ_ARG, 0, 'C' },
        { "number", SU_REQ_ARG, 0, 'n' },
        { "elide-empty-files", SU_NO_ARG, 0, 'e' },
        { "unbuffered", SU_NO_ARG, 0, 'u' },
        { "suffix-length", SU_REQ_ARG, 0, 'a' },
        { "additional-suffix", SU_REQ_ARG, 0, ADDSUF_OPT },
        { "numeric-suffixes", SU_OPT_ARG, 0, NUMSUF_OPT },
        { "hex-suffixes", SU_OPT_ARG, 0, HEXSUF_OPT },
        { "filter", SU_REQ_ARG, 0, FILTER_OPT },
        { "verbose", SU_NO_ARG, 0, VERBOSE_OPT },
        { "separator", SU_REQ_ARG, 0, 't' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&tsp, 0, sizeof tsp);
    tsp.sep = '\n';
    tsp.digits = 'a';
    int c, obs_elem = -1;
    while ((c = su_getopt_long(argc, argv, "0123456789C:a:b:del:n:t:ux", longs, 0)) != -1) {
        switch (c) {
        case 'a': {
            const char *p = su_optarg;
            while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
            int neg = *p == '-';
            if (*p == '+' || *p == '-') p++;
            if (!isdigit((unsigned char)*p) || p[strspn(p, "0123456789")])
                su_error(1, 0, "invalid suffix length: %s", su_quote(su_optarg));
            uintmax_t v = 0;
            int over = 0;
            for (; isdigit((unsigned char)*p); p++) {
                unsigned d = (unsigned)(*p - '0');
                if (v > (UINTMAX_MAX - d) / 10) over = 1;
                else v = v * 10 + d;
            }
            if (over || (neg && v) || v > (uintmax_t)INTMAX_MAX)
                su_error(1, EOVERFLOW, "invalid suffix length: %s", su_quote(su_optarg));
            tsp.suflen = v;
            tsp.suflen_set = v != 0;
            break;
        }
        case ADDSUF_OPT:
            if (strchr(su_optarg, '/')) {
                su_error(0, 0, "invalid suffix %s, contains directory separator", su_quote(su_optarg));
                su_try_help(1);
            }
            tsp.addsuf = su_optarg;
            break;
        case 'b':
            tsp_type(TSP_BYTES);
            tsp.units = tsp_size(su_optarg, "invalid number of bytes");
            break;
        case 'C':
            tsp_type(TSP_LINEBYTES);
            tsp.units = tsp_size(su_optarg, "invalid number of lines");
            break;
        case 'l':
            tsp_type(TSP_LINES);
            if (tsp_count(su_optarg, &tsp.units) || tsp.units == 0)
                su_error(1, 0, "invalid number of lines: %s", su_quote(su_optarg));
            break;
        case 'n': tsp_chunks(su_optarg); break;
        case 'd': tsp.digits = 'd'; break;
        case 'x': tsp.digits = 'x'; break;
        case NUMSUF_OPT:
            tsp.digits = 'd';
            if (su_optarg) tsp_from(su_optarg, 0);
            break;
        case HEXSUF_OPT:
            tsp.digits = 'x';
            if (su_optarg) tsp_from(su_optarg, 1);
            break;
        case 'e': tsp.elide = 1; break;
        case 'u': tsp.unbuffered = 1; break;
        case FILTER_OPT: tsp.filter = su_optarg; break;
        case VERBOSE_OPT: tsp.verbose = 1; break;
        case 't': {
            int s = 0;
            if (!*su_optarg) su_error(1, 0, "empty record separator");
            if (su_optarg[1]) {
                if (!strcmp(su_optarg, "\\0")) s = 0;
                else su_error(1, 0, "multi-character separator %s", su_quote(su_optarg));
            } else s = (unsigned char)su_optarg[0];
            if (tsp.sep_set && s != tsp.sep) su_error(1, 0, "multiple separator characters specified");
            tsp.sep = s;
            tsp.sep_set = 1;
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE [PREFIX]]\n", su_name);
            printf("Write FILE in pieces named PREFIXaa, PREFIXab, ...; a piece is 1000 lines\n"
                   "unless told otherwise, and PREFIX is 'x' unless given.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "A long option's argument is required by its short form too.\n"
                   "  -a, --suffix-length=N   suffixes N characters long (2 by default)\n"
                   "      --additional-suffix=SUFFIX  end every name with SUFFIX too\n"
                   "  -b, --bytes=SIZE        SIZE bytes in each piece\n"
                   "  -C, --line-bytes=SIZE   at most SIZE bytes of whole records in each piece\n"
                   "  -d                      number the suffixes in decimal from 0\n"
                   "      --numeric-suffixes[=FROM]  like -d, counting from FROM\n"
                   "  -x                      number the suffixes in hexadecimal from 0\n"
                   "      --hex-suffixes[=FROM]  like -x, counting from FROM\n"
                   "  -e, --elide-empty-files  make no empty pieces with -n\n"
                   "      --filter=COMMAND    give each piece to the shell COMMAND; $FILE is\n"
                   "                            the name it would have had\n"
                   "  -l, --lines=NUMBER      NUMBER records in each piece\n"
                   "  -n, --number=CHUNKS     make CHUNKS pieces (see below)\n"
                   "  -t, --separator=SEP     records end in SEP, not newline; '\\0' is NUL\n"
                   "  -u, --unbuffered        with -n r/..., pass each record on at once\n"
                   "      --verbose           say which piece is about to be made\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "SIZE is a whole number with an optional unit: K M G T P E Z Y R Q are\n"
                   "powers of 1024, KB MB ... powers of 1000, and KiB MiB ... are K M ....\n"
                   "\n"
                   "CHUNKS is one of:\n"
                   "  N       N pieces of about equal size\n"
                   "  K/N     only the Kth of those N, written to standard output\n"
                   "  l/N     N pieces of about equal size, records kept whole\n"
                   "  l/K/N   only the Kth of those, to standard output\n"
                   "  r/N     N pieces, the records dealt out in turn\n"
                   "  r/K/N   only the Kth of those, to standard output\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default:
            if (c >= '0' && c <= '9') {
                /* the old -N: its digits run on through one argument */
                int elem = su_optind;
                if (elem != obs_elem) {
                    if (tsp.type != TSP_NONE && !tsp.obsolete) tsp_conflict();
                    tsp.type = TSP_LINES;
                    tsp.obsolete = 1;
                    tsp.units = 0;
                    obs_elem = elem;
                }
                unsigned d = (unsigned)(c - '0');
                tsp.units = tsp.units > (UINTMAX_MAX - d) / 10 ? UINTMAX_MAX : tsp.units * 10 + d;
                break;
            }
            su_try_help(1);
        }
    }
    int nop = argc - su_optind;
    if (nop > 2) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 2]));
        su_try_help(1);
    }
    if (tsp.obsolete && tsp.units == 0) {
        su_error(0, 0, "invalid number of lines: %s", su_quote("0"));
        su_try_help(1);
    }
    if (tsp.type == TSP_NONE) { tsp.type = TSP_LINES; tsp.units = 1000; }
    int chunked = tsp.type == TSP_CHUNKB || tsp.type == TSP_CHUNKL || tsp.type == TSP_RR;
    if (tsp.filter && chunked && tsp.k) {
        su_error(0, 0, "--filter does not process a chunk extracted to stdout");
        su_try_help(1);
    }
    tsp.inname = nop >= 1 ? argv[su_optind] : "-";
    tsp.prefix = nop >= 2 ? argv[su_optind + 1] : "x";

    /* the suffix length */
    tsp.base = strlen(tsp_alpha());
    tsp.len = tsp.suflen_set ? (size_t)tsp.suflen : 2;
    if (tsp.suflen_set && tsp.suflen > SIZE_MAX / 4) su_error(1, 0, "memory exhausted");
    tsp.widen = !tsp.suflen_set && !tsp.from && !chunked;
    if (chunked && !tsp.k) {
        uintmax_t top = tsp.units - 1;
        if (tsp.from) {
            uintmax_t f = tsp_from_value();
            if (f < tsp.units) top = top + f < top ? UINTMAX_MAX : top + f;
        }
        size_t need = tsp_ndigits(top, tsp.base);
        if (!tsp.suflen_set) { if (need > tsp.len) tsp.len = need; }
        else if (tsp.len < need) su_error(1, 0, "the suffix length needs to be at least %zu", need);
    }
    tsp.idx = (unsigned char *)calloc(tsp.len ? tsp.len : 1, 1);
    if (!tsp.idx) su_error(1, 0, "memory exhausted");
    if (tsp.from) {
        const char *f = tsp.from;
        while (*f == '0' && f[1]) f++;
        size_t fl = strlen(f);
        if (fl > tsp.len) {
            su_error(0, 0, "numerical suffix start value is too large for the suffix length");
            su_try_help(1);
        }
        for (size_t i = 0; i < fl; i++)
            tsp.idx[tsp.len - fl + i] = (unsigned char)(f[i] <= '9' ? f[i] - '0' : f[i] - 'a' + 10);
    }
    if (tsp.filter) {
        const char *sh = getenv("SHELL");
        tsp.shell = sh ? sh : "/bin/sh";
        signal(SIGPIPE, SIG_IGN);
    }

    /* the input */
    if (!strcmp(tsp.inname, "-")) tsp.in_fd = 0;
    else {
        tsp.in_fd = su_open_read(tsp.inname, 0);
        if (tsp.in_fd < 0) su_error(1, errno, "cannot open %s for reading", su_quote(tsp.inname));
    }
    struct stat st;
    if (fstat(tsp.in_fd, &st) == 0 && S_ISREG(st.st_mode)) {
        tsp.in_reg = 1;
        tsp.in_dev = st.st_dev;
        tsp.in_ino = st.st_ino;
    }
    memset(&tsp_so, 0, offsetof(struct tsp_out, buf));
    tsp_so.fd = 1;
    tsp_so.name = (char *)"-";
    fflush(stdout);
    switch (tsp.type) {
    case TSP_LINES: tsp_by_count(1); break;
    case TSP_BYTES: tsp_by_count(0); break;
    case TSP_LINEBYTES: tsp_line_bytes(); break;
    case TSP_CHUNKB: case TSP_CHUNKL: tsp_chunk_modes(); break;
    case TSP_RR: tsp_round_robin(); break;
    }
    tsp_flush(&tsp_so);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}
