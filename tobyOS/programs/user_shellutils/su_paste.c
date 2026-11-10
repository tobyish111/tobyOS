/* su_paste.c -- paste, as GNU coreutils 9.7's (logs/utilgate/
 * paste.sh): the Nth lines of every FILE joined into one line, separated
 * by TABs or by -d's list (taken in turn, starting again on each line),
 * or with -s each file's lines joined into one line in turn. "-" is
 * standard input -- one stream, read in turn when it is named twice.
 *
 * The list's escapes, measured: \n \t \\ \b \f \r \v as in C, \0 an
 * empty delimiter (nothing at all), \ before any other character that
 * character; an empty list is one empty delimiter, and a list that ends
 * in a lone backslash is refused, the list shown bare or, when it holds
 * a double quote or a byte that does not print, as a C string.
 *
 * In parallel, a file that has ended gives empty fields and its
 * delimiters are still written; output stops at the first line that no
 * file has, and a missing file ends the run before anything is written.
 * A file that cannot be read (a directory) is reported where its first
 * line would be and then counts as ended. Serially, every file gives a
 * line, an empty one when the file is; a file that cannot be read is
 * reported after its (empty) line, and one that cannot be opened is
 * reported and skipped. -z: NUL ends lines, in and out. A last line with
 * no newline gets one.
 *
 * The old paste had -d (escapes \n \t \\ \0 only) and -s, but no -z, and
 * stopped at the first file that ran out. */

struct tpa_in {
    int fd;
    int eof, err;
    const char *name;
    size_t pos, len;
    unsigned char buf[8192];
};

struct tpa_line { char *s; size_t n, cap; };

static void tpa_append(struct tpa_line *l, const void *p, size_t n) {
    if (l->n + n > l->cap) {
        size_t nc = l->cap ? l->cap : 256;
        while (nc < l->n + n) nc *= 2;
        char *ns = (char *)realloc(l->s, nc);
        if (!ns) su_error(1, ENOMEM, "memory exhausted");
        l->s = ns;
        l->cap = nc;
    }
    memcpy(l->s + l->n, p, n);
    l->n += n;
}

/* the next line of IN appended to OUT: 1, or 0 at its end (or error) */
static int tpa_getline(struct tpa_in *in, struct tpa_line *out, int delim) {
    int any = 0;
    for (;;) {
        if (in->pos == in->len) {
            if (in->eof || in->err) break;
            ssize_t r = read(in->fd, in->buf, sizeof in->buf);
            if (r < 0) {
                if (errno == EINTR) continue;
                in->err = errno;
                break;
            }
            if (r == 0) { in->eof = 1; break; }
            in->pos = 0;
            in->len = (size_t)r;
        }
        unsigned char *p = in->buf + in->pos;
        size_t avail = in->len - in->pos;
        unsigned char *q = (unsigned char *)memchr(p, delim, avail);
        size_t take = q ? (size_t)(q - p) : avail;
        tpa_append(out, p, take);
        any = 1;
        in->pos += take;
        if (q) { in->pos++; return 1; }
    }
    return any && !in->err;
}

/* the list as GNU's quotearg c-maybe style shows it */
static void tpa_show_list(const char *s) {
    int quote = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p == '"' || *p < 0x20 || *p >= 0x7f) quote = 1;
    if (!quote) { fputs(s, stderr); return; }
    fputc('"', stderr);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        const char *e = 0;
        switch (c) {
        case '"': e = "\\\""; break;
        case '\\': e = "\\\\"; break;
        case '\a': e = "\\a"; break;
        case '\b': e = "\\b"; break;
        case '\f': e = "\\f"; break;
        case '\n': e = "\\n"; break;
        case '\r': e = "\\r"; break;
        case '\t': e = "\\t"; break;
        case '\v': e = "\\v"; break;
        }
        if (e) fputs(e, stderr);
        else if (c < 0x20 || c >= 0x7f) fprintf(stderr, "\\%03o", c);
        else fputc(c, stderr);
    }
    fputc('"', stderr);
}

/* the delimiters: DCH[i] the byte, DLEN[i] 0 or 1 */
static char *tpa_dch;
static unsigned char *tpa_dlen;
static size_t tpa_nd;

static void tpa_delims(const char *list) {
    size_t n = strlen(list);
    tpa_dch = (char *)malloc(n + 1);
    tpa_dlen = (unsigned char *)malloc(n + 1);
    if (!tpa_dch || !tpa_dlen) su_error(1, ENOMEM, "memory exhausted");
    tpa_nd = 0;
    for (const char *p = list; *p; p++) {
        char c = *p;
        unsigned char len = 1;
        if (c == '\\') {
            if (!p[1]) {
                fflush(stdout);
                fprintf(stderr, "%s: delimiter list ends with an unescaped backslash: ", su_short);
                tpa_show_list(list);
                fputc('\n', stderr);
                exit(1);
            }
            c = *++p;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'r': c = '\r'; break;
            case 'v': c = '\v'; break;
            case '0': c = 0; len = 0; break;
            default: break;          /* \\ and any other: the character */
            }
        }
        tpa_dch[tpa_nd] = c;
        tpa_dlen[tpa_nd] = len;
        tpa_nd++;
    }
    if (tpa_nd == 0) {               /* "" is one empty delimiter */
        tpa_dch[0] = 0;
        tpa_dlen[0] = 0;
        tpa_nd = 1;
    }
}

static int tpa_parallel(struct tpa_in **in, int n, int delim) {
    int status = 0;
    struct tpa_line row = { 0, 0, 0 };
    int *open_ = (int *)malloc((size_t)n * sizeof *open_);
    if (!open_) su_error(1, ENOMEM, "memory exhausted");
    for (int i = 0; i < n; i++) open_[i] = 1;
    for (;;) {
        int got = 0;
        row.n = 0;
        size_t d = 0;
        for (int i = 0; i < n; i++) {
            if (open_[i]) {
                if (tpa_getline(in[i], &row, delim)) got = 1;
                else {
                    open_[i] = 0;
                    if (in[i]->err) {
                        /* what was written so far goes first */
                        if (got) fwrite(row.s, 1, row.n, stdout), row.n = 0, got = 2;
                        su_error(0, in[i]->err, "%s", su_quotef(in[i]->name));
                        in[i]->err = 0;
                        in[i]->eof = 1;
                        status = 1;
                    }
                }
            }
            if (i + 1 < n) {
                tpa_append(&row, &tpa_dch[d], tpa_dlen[d]);
                if (++d == tpa_nd) d = 0;
            }
        }
        if (!got) break;
        fwrite(row.s, 1, row.n, stdout);
        putchar(delim);
    }
    free(open_);
    free(row.s);
    return status;
}

static int tpa_serial(struct tpa_in **in, int n, int delim) {
    int status = 0;
    struct tpa_line line = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        if (!in[i]) { status = 1; continue; }       /* could not be opened */
        size_t d = 0;
        int first = 1;
        line.n = 0;
        while (tpa_getline(in[i], &line, delim)) {
            if (!first) {
                fwrite(&tpa_dch[d], 1, tpa_dlen[d], stdout);
                if (++d == tpa_nd) d = 0;
            }
            first = 0;
            fwrite(line.s, 1, line.n, stdout);
            line.n = 0;
        }
        putchar(delim);
        if (in[i]->err) {
            su_error(0, in[i]->err, "%s", su_quotef(in[i]->name));
            in[i]->err = 0;
            in[i]->eof = 1;
            status = 1;
        }
    }
    free(line.s);
    return status;
}

static int do_paste(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "serial", SU_NO_ARG, 0, 's' },
        { "delimiters", SU_REQ_ARG, 0, 'd' },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    const char *list = "\t";
    int serial = 0, delim = '\n', c;
    while ((c = su_getopt_long(argc, argv, "d:sz", longs, 0)) != -1) {
        switch (c) {
        case 'd': list = su_optarg; break;
        case 's': serial = 1; break;
        case 'z': delim = 0; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n", su_name);
            printf("Write lines made of the matching lines of every FILE, joined by TABs, to\n"
                   "standard output.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "A long option's argument is required by its short form too.\n"
                   "  -d, --delimiters=LIST   join with the characters of LIST in turn, not TABs\n"
                   "  -s, --serial            join each file's own lines, one file after another\n"
                   "  -z, --zero-terminated   lines end in NUL, not newline\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    tpa_delims(list);
    static char *dash[] = { (char *)"-", 0 };
    char **names = argv + su_optind;
    int n = argc - su_optind;
    if (n <= 0) { names = dash; n = 1; }
    struct tpa_in **in = (struct tpa_in **)calloc((size_t)n, sizeof *in);
    struct tpa_in *std = 0;
    if (!in) su_error(1, ENOMEM, "memory exhausted");
    int status = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(names[i], "-")) {
            if (!std) {
                std = (struct tpa_in *)calloc(1, sizeof *std);
                if (!std) su_error(1, ENOMEM, "memory exhausted");
                std->fd = 0;
                std->name = "-";
            }
            in[i] = std;
            continue;
        }
        if (serial) continue;           /* opened as it comes */
        struct tpa_in *f = (struct tpa_in *)calloc(1, sizeof *f);
        if (!f) su_error(1, ENOMEM, "memory exhausted");
        f->name = names[i];
        f->fd = su_open_read(names[i], 0);
        if (f->fd < 0) su_error(1, errno, "%s", su_quotef(names[i]));
        in[i] = f;
    }
    if (serial) {
        /* each file opened when its turn comes, and closed after */
        for (int i = 0; i < n; i++) {
            struct tpa_in *f = in[i];
            int mine = 0;
            if (!f) {
                f = (struct tpa_in *)calloc(1, sizeof *f);
                if (!f) su_error(1, ENOMEM, "memory exhausted");
                f->name = names[i];
                f->fd = su_open_read(names[i], 0);
                if (f->fd < 0) {
                    su_error(0, errno, "%s", su_quotef(names[i]));
                    free(f);
                    status = 1;
                    continue;
                }
                mine = 1;
            }
            status |= tpa_serial(&f, 1, delim);
            if (mine) { close(f->fd); free(f); }
        }
    } else {
        status = tpa_parallel(in, n, delim);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return status;
}
