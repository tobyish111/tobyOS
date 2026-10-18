/* su_shuf.c -- shuf, as GNU coreutils 9.7's (logs/utilgate/
 * shuf.sh): the lines of FILE (or standard input), the operands (-e), or
 * the numbers LO..HI (-i) written in a random order, at most -n of them,
 * or with -r chosen again and again (forever without -n).
 *
 * THE RANDOMNESS IS GNU'S, measured black-box with --random-source files
 * of known bytes (single bytes, ramps, 0x80 runs, a byte that forces a
 * rejection), so that a given source gives GNU's output byte for byte:
 *
 *   A choice in 0..G keeps a pool: a value V in 0..M. While M < G a byte
 *   B is taken (V = V*256 + B, M = M*256 + 255). When M == G the choice
 *   is V and the pool empties. Otherwise, with N = G+1 and Q the number
 *   of whole N-blocks in M+1 values: V below Q*N gives V % N, and the
 *   pool keeps V / N in 0..Q-1; V past them is rejected, the pool keeping
 *   V - Q*N in 0..M-Q*N, and the choice is tried again. G == 0 takes no
 *   byte. A source that ends is "'NAME': end of file".
 *
 *   A permutation is Fisher-Yates from the front: for i = 0, 1, ... each
 *   of the first K places takes the element at i + choice(0..N-1-i) --
 *   only K choices are made. -r makes one choice in 0..N-1 per line.
 *   When -n is given for lines read from a file that is not a regular
 *   file of at most 8 MiB (a pipe, a big file), GNU samples a reservoir
 *   instead: the first K lines, then for each later line index I a
 *   choice in 0..I MADE BEFORE THE LINE IS READ (so one more is made at
 *   the end of the input), the line replacing reservoir slot J when J <
 *   K; the reservoir is then permuted as above.
 *
 * The order of the work, measured by which error wins: options (-n 0
 * without -r ends the run there), the input, the random source (opened
 * even when no choice is needed), then for -r the output file, "no lines
 * to repeat" and the lines; otherwise the permutation and then the
 * output file. Without --random-source the bytes are /dev/urandom's.
 * A range too big to hold, or a huge -n of it, is "memory exhausted" as
 * GNU's is.
 *
 * The old shuf had -e -i -n -r (a 64-bit LCG seeded from /dev/urandom)
 * but no -o, -z or --random-source. */

struct tsf_rand {
    int fd;
    const char *name;
    uintmax_t v, m;
    size_t pos, len;
    unsigned char buf[4096];
};

static int tsf_byte(struct tsf_rand *r) {
    while (r->pos == r->len) {
        ssize_t n = read(r->fd, r->buf, sizeof r->buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) su_error(1, errno, "%s: read error", su_quote(r->name));
        if (n == 0) su_error(1, 0, "%s: end of file", su_quote(r->name));
        r->pos = 0;
        r->len = (size_t)n;
    }
    return r->buf[r->pos++];
}

/* a choice in 0..G */
static uintmax_t tsf_choose(struct tsf_rand *r, uintmax_t g) {
    if (g == 0) return 0;
    for (;;) {
        while (r->m < g) {
            r->v = r->v * 256 + (uintmax_t)tsf_byte(r);
            r->m = r->m * 256 + 255;
        }
        if (r->m == g) {
            uintmax_t x = r->v;
            r->v = r->m = 0;
            return x;
        }
        uintmax_t n = g + 1;            /* g < m, so this does not wrap */
        /* M+1 values = Q*N + REM; computed without M+1 itself */
        uintmax_t q = r->m / n, rem = r->m % n + 1;
        if (rem == n) { q++; rem = 0; }
        if (rem == 0 || r->v < q * n) {
            uintmax_t x = r->v % n;
            r->v /= n;
            r->m = q - 1;
            return x;
        }
        r->v -= q * n;
        r->m = rem - 1;
    }
}

/* the permutation's places, a plain array or (for a big -i range) a hash
 * of the places that have moved */
struct tsf_perm {
    uintmax_t n;
    uintmax_t *dense;
    uintmax_t *keys, *vals;
    unsigned char *used;
    size_t cap;
};

static int tsf_perm_init(struct tsf_perm *p, uintmax_t n, uintmax_t k) {
    memset(p, 0, sizeof *p);
    p->n = n;
    if (n <= 65536 || n <= k) {
        if (n > SIZE_MAX / sizeof *p->dense) return -1;
        p->dense = (uintmax_t *)malloc((size_t)(n ? n : 1) * sizeof *p->dense);
        if (!p->dense) return -1;
        for (uintmax_t i = 0; i < n; i++) p->dense[i] = i;
        return 0;
    }
    /* at most 2 places move per step */
    if (k > SIZE_MAX / 64) return -1;
    size_t cap = 16;
    while (cap < (size_t)k * 4) cap *= 2;
    p->cap = cap;
    p->keys = (uintmax_t *)malloc(cap * sizeof *p->keys);
    p->vals = (uintmax_t *)malloc(cap * sizeof *p->vals);
    p->used = (unsigned char *)calloc(cap, 1);
    if (!p->keys || !p->vals || !p->used) return -1;
    return 0;
}

static size_t tsf_slot(const struct tsf_perm *p, uintmax_t key) {
    size_t h = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 17) & (p->cap - 1);
    while (p->used[h] && p->keys[h] != key) h = (h + 1) & (p->cap - 1);
    return h;
}

static uintmax_t tsf_get(const struct tsf_perm *p, uintmax_t i) {
    if (p->dense) return p->dense[i];
    size_t h = tsf_slot(p, i);
    return p->used[h] ? p->vals[h] : i;
}

static void tsf_set(struct tsf_perm *p, uintmax_t i, uintmax_t v) {
    if (p->dense) { p->dense[i] = v; return; }
    size_t h = tsf_slot(p, i);
    p->used[h] = 1;
    p->keys[h] = i;
    p->vals[h] = v;
}

/* the first K places of a permutation of N */
static void tsf_shuffle(struct tsf_perm *p, uintmax_t k, struct tsf_rand *r) {
    for (uintmax_t i = 0; i < k; i++) {
        uintmax_t j = i + tsf_choose(r, p->n - 1 - i);
        uintmax_t a = tsf_get(p, i), b = tsf_get(p, j);
        tsf_set(p, i, b);
        tsf_set(p, j, a);
    }
}

/* the input's lines */
struct tsf_lines {
    char **s;
    size_t *len;
    size_t n, cap;
};

static void tsf_push(struct tsf_lines *l, char *s, size_t len) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->s = (char **)realloc(l->s, l->cap * sizeof *l->s);
        l->len = (size_t *)realloc(l->len, l->cap * sizeof *l->len);
        if (!l->s || !l->len) su_error(1, ENOMEM, "memory exhausted");
    }
    l->s[l->n] = s;
    l->len[l->n] = len;
    l->n++;
}

struct tsf_reader {
    int fd;
    int eof;
    size_t pos, len;
    unsigned char buf[16384];
};

/* the next line (malloc'd, its length in *LEN), or NULL at the end */
static char *tsf_getline(struct tsf_reader *rd, int eol, size_t *len) {
    char *s = 0;
    size_t n = 0, cap = 0;
    int any = 0;
    for (;;) {
        if (rd->pos == rd->len) {
            if (rd->eof) break;
            ssize_t r = read(rd->fd, rd->buf, sizeof rd->buf);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) su_error(1, errno, "read error");
            if (r == 0) { rd->eof = 1; break; }
            rd->pos = 0;
            rd->len = (size_t)r;
        }
        unsigned char *p = rd->buf + rd->pos;
        size_t avail = rd->len - rd->pos;
        unsigned char *q = (unsigned char *)memchr(p, eol, avail);
        size_t take = q ? (size_t)(q - p) : avail;
        if (n + take + 1 > cap) {
            cap = cap ? cap : 64;
            while (cap < n + take + 1) cap *= 2;
            s = (char *)realloc(s, cap);
            if (!s) su_error(1, ENOMEM, "memory exhausted");
        }
        memcpy(s + n, p, take);
        n += take;
        rd->pos += take;
        any = 1;
        if (q) { rd->pos++; break; }
    }
    if (!any) { free(s); return 0; }
    if (!s) s = (char *)malloc(1);
    *len = n;
    return s;
}

static void tsf_open_rand(struct tsf_rand *r, const char *name) {
    memset(r, 0, offsetof(struct tsf_rand, buf));
    r->name = name ? name : "/dev/urandom";
    r->fd = su_open_read(r->name, 0);
    if (r->fd < 0) su_error(1, errno, "%s", su_quotef(r->name));
}

static void tsf_open_out(const char *out) {
    if (!out) return;
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) su_error(1, errno, "%s", su_quotef(out));
    fflush(stdout);
    if (fd != 1) { dup2(fd, 1); close(fd); }
}

/* LO-HI as GNU reads it; 0, or exits */
static void tsf_range(const char *arg, uintmax_t *lo, uintmax_t *hi) {
    char *end;
    int rc = su_strtoumax(arg, &end, lo, 0);
    if (rc == SU_NUM_OVERFLOW) su_error(1, EOVERFLOW, "invalid input range: %s", su_quote(arg));
    if (rc != SU_NUM_OK || end == arg || *end != '-')
        su_error(1, 0, "invalid input range: %s", su_quote(arg));
    const char *h = end + 1;
    rc = su_strtoumax(h, &end, hi, 0);
    if (rc == SU_NUM_OVERFLOW) su_error(1, EOVERFLOW, "invalid input range: %s", su_quote(arg));
    if (end == h || *end) su_error(1, 0, "invalid input range: %s", su_quote(arg));
    /* empty (HI = LO-1) is allowed; the count must fit a size_t */
    if (*hi < *lo ? *hi + 1 != *lo : *hi - *lo >= (uintmax_t)SIZE_MAX)
        su_error(1, 0, "invalid input range: %s", su_quote(arg));
}

static int do_shuf(int argc, char **argv) {
    enum { RANDSRC_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "echo", SU_NO_ARG, 0, 'e' },
        { "input-range", SU_REQ_ARG, 0, 'i' },
        { "head-count", SU_REQ_ARG, 0, 'n' },
        { "output", SU_REQ_ARG, 0, 'o' },
        { "random-source", SU_REQ_ARG, 0, RANDSRC_OPT },
        { "repeat", SU_NO_ARG, 0, 'r' },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int echo = 0, range = 0, repeat = 0, eol = '\n', c;
    uintmax_t lo = 0, hi = 0, head = UINTMAX_MAX;
    int have_head = 0;
    const char *out = 0, *rsrc = 0;
    while ((c = su_getopt_long(argc, argv, "ei:n:o:rz", longs, 0)) != -1) {
        switch (c) {
        case 'e': echo = 1; break;
        case 'i':
            if (range) su_error(1, 0, "multiple -i options specified");
            range = 1;
            tsf_range(su_optarg, &lo, &hi);
            break;
        case 'n': {
            uintmax_t v;
            char *end;
            int rc = su_strtoumax(su_optarg, &end, &v, 0);
            /* a count too big is as many as there are */
            if (rc == SU_NUM_OVERFLOW) v = UINTMAX_MAX;
            if ((rc != SU_NUM_OK && rc != SU_NUM_OVERFLOW) || end == su_optarg || *end)
                su_error(1, 0, "invalid line count: %s", su_quote(su_optarg));
            if (!have_head || v < head) head = v;
            have_head = 1;
            break;
        }
        case 'o':
            if (out && strcmp(out, su_optarg)) su_error(1, 0, "multiple output files specified");
            out = su_optarg;
            break;
        case RANDSRC_OPT:
            if (rsrc && strcmp(rsrc, su_optarg)) su_error(1, 0, "multiple random sources specified");
            rsrc = su_optarg;
            break;
        case 'r': repeat = 1; break;
        case 'z': eol = 0; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]\n"
                   "  or:  %s -e [OPTION]... [ARG]...\n"
                   "  or:  %s -i LO-HI [OPTION]...\n", su_name, su_name, su_name);
            printf("Write the input lines to standard output in a random order.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "A long option's argument is required by its short form too.\n"
                   "  -e, --echo                each ARG is an input line\n"
                   "  -i, --input-range=LO-HI   the numbers LO to HI are the input lines\n"
                   "  -n, --head-count=COUNT    write no more than COUNT lines\n"
                   "  -o, --output=FILE         write to FILE, not to standard output\n"
                   "      --random-source=FILE  take the random bytes from FILE\n"
                   "  -r, --repeat              choose every line anew, so lines can repeat\n"
                   "  -z, --zero-terminated     lines end in NUL, not newline\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int nop = argc - su_optind;
    if (echo && range) {
        su_error(0, 0, "cannot combine -e and -i options");
        su_try_help(1);
    }
    if (range && nop > 0) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
    if (!echo && !range && nop > 1) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 1]));
        su_try_help(1);
    }
    if (have_head && head == 0 && !repeat) return 0;

    struct tsf_lines lines = { 0, 0, 0, 0 };
    struct tsf_rand *rnd = (struct tsf_rand *)malloc(sizeof *rnd);
    if (!rnd) su_error(1, ENOMEM, "memory exhausted");
    int rand_open = 0, reservoir = 0;
    uintmax_t n = 0;
    if (echo) {
        for (int i = su_optind; i < argc; i++) tsf_push(&lines, argv[i], strlen(argv[i]));
        n = lines.n;
    } else if (range) {
        n = hi < lo ? 0 : hi - lo + 1;
    } else {
        struct tsf_reader *rd = (struct tsf_reader *)malloc(sizeof *rd);
        if (!rd) su_error(1, ENOMEM, "memory exhausted");
        memset(rd, 0, offsetof(struct tsf_reader, buf));
        const char *name = nop ? argv[su_optind] : "-";
        if (strcmp(name, "-")) {
            rd->fd = su_open_read(name, 0);
            if (rd->fd < 0) su_error(1, errno, "%s", su_quotef(name));
        }
        if (have_head && !repeat) {
            /* GNU samples a reservoir unless the input is a regular file
             * of at most 8 MiB */
            struct stat st;
            reservoir = 1;
            if (fstat(rd->fd, &st) == 0 && S_ISREG(st.st_mode)) {
                off_t cur = lseek(rd->fd, 0, SEEK_CUR);
                off_t size = st.st_size - (cur > 0 ? cur : 0);
                if (size <= 8 * 1024 * 1024) reservoir = 0;
            }
        }
        size_t len;
        char *s;
        if (reservoir) {
            tsf_open_rand(rnd, rsrc);
            rand_open = 1;
            while (lines.n < head && (s = tsf_getline(rd, eol, &len))) tsf_push(&lines, s, len);
            if (lines.n == head) {
                for (uintmax_t i = head;; i++) {
                    uintmax_t j = tsf_choose(rnd, i);
                    if (!(s = tsf_getline(rd, eol, &len))) break;
                    if (j < head) {
                        free(lines.s[j]);
                        lines.s[j] = s;
                        lines.len[j] = len;
                    } else free(s);
                }
            }
        } else {
            while ((s = tsf_getline(rd, eol, &len))) tsf_push(&lines, s, len);
        }
        if (rd->fd > 0) close(rd->fd);
        free(rd);
        n = lines.n;
    }
    if (!rand_open) tsf_open_rand(rnd, rsrc);

    if (repeat) {
        tsf_open_out(out);
        if (n == 0) {
            if (head != 0) su_error(1, 0, "no lines to repeat");
        } else {
            for (uintmax_t k = 0; !have_head || k < head; k++) {
                uintmax_t j = tsf_choose(rnd, n - 1);
                if (range) printf("%ju", lo + j);
                else fwrite(lines.s[j], 1, lines.len[j], stdout);
                putchar(eol);
                if (ferror(stdout)) break;
            }
        }
    } else {
        uintmax_t k = head < n ? head : n;
        struct tsf_perm perm;
        if (tsf_perm_init(&perm, n, k)) su_error(1, 0, "memory exhausted");
        tsf_shuffle(&perm, k, rnd);
        tsf_open_out(out);
        for (uintmax_t i = 0; i < k; i++) {
            uintmax_t j = tsf_get(&perm, i);
            if (range) printf("%ju", lo + j);
            else fwrite(lines.s[j], 1, lines.len[j], stdout);
            putchar(eol);
        }
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}
