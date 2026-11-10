/* su_fold.c -- fold, as GNU coreutils 9.7's (logs/utilgate/
 * fold.sh): each line of each FILE (or standard input) broken so that no
 * piece is wider than WIDTH columns (80 by default), in the C locale.
 *
 * What a column is, measured: a TAB moves to the next multiple of 8, a
 * backspace back one (not before column 0), a carriage return to column
 * 0, every other byte one; with -b every byte but the newline is one.
 * A byte that would pass WIDTH ends the piece before it -- or, with -s,
 * the piece ends after its last blank (space or TAB; not \v or \f) and
 * what followed the blank starts the next one -- except that a piece
 * never ends empty: a byte wider than WIDTH on its own (a TAB at -w 5)
 * is a piece by itself. A file's unfinished last line is written as it
 * is, with no newline, and the next file starts at column 0.
 *
 * WIDTH is a decimal from 1 to SIZE_MAX - 9, leading blanks and a '+'
 * allowed; 0 and larger numbers are out of range (ERANGE), anything else
 * invalid. -N is the old form of -w N, and takes the rest of its
 * argument with it (-5s is the width "5s", invalid).
 *
 * The old fold had -b -s -w but counted every byte as a column. */

struct tfo_buf { char *s; size_t n, cap; };

static void tfo_put(struct tfo_buf *b, char c) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->s = (char *)realloc(b->s, b->cap);
        if (!b->s) su_error(1, ENOMEM, "memory exhausted");
    }
    b->s[b->n++] = c;
}

static int tfo_bytes;

static size_t tfo_adjust(size_t col, unsigned char c) {
    if (tfo_bytes) return col + 1;
    if (c == '\b') return col ? col - 1 : 0;
    if (c == '\r') return 0;
    if (c == '\t') return col + 8 - col % 8;
    return col + 1;
}

static size_t tfo_width(const char *arg) {
    const char *p = arg;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) su_error(1, 0, "invalid number of columns: %s", su_quote(arg));
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p) su_error(1, 0, "invalid number of columns: %s", su_quote(arg));
    if (over || v == 0 || v > (uintmax_t)SIZE_MAX - 9)
        su_error(1, ERANGE, "invalid number of columns: %s", su_quote(arg));
    return (size_t)v;
}

/* one file: 0, or 1 when it could not be read */
static int tfo_file(const char *name, size_t width, int spaces) {
    int fd = 0;
    if (strcmp(name, "-")) {
        fd = su_open_read(name, 0);
        if (fd < 0) { su_error(0, errno, "%s", su_quotef(name)); return 1; }
    }
    static unsigned char ibuf[16384];
    struct tfo_buf line = { 0, 0, 0 };
    size_t col = 0;
    int rc = 0;
    for (;;) {
        ssize_t r = read(fd, ibuf, sizeof ibuf);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { su_error(0, errno, "%s", su_quotef(name)); rc = 1; break; }
        if (r == 0) break;
        for (ssize_t k = 0; k < r; k++) {
            unsigned char c = ibuf[k];
            if (c == '\n') {
                fwrite(line.s, 1, line.n, stdout);
                putchar('\n');
                line.n = 0;
                col = 0;
                continue;
            }
            for (;;) {
                size_t nc = tfo_adjust(col, c);
                if (nc <= width || line.n == 0) { col = nc; break; }
                if (spaces) {
                    size_t i = line.n;
                    while (i > 0 && line.s[i - 1] != ' ' && line.s[i - 1] != '\t') i--;
                    if (i > 0) {
                        /* up to the blank, then what follows it again */
                        fwrite(line.s, 1, i, stdout);
                        putchar('\n');
                        memmove(line.s, line.s + i, line.n - i);
                        line.n -= i;
                        col = 0;
                        for (size_t j = 0; j < line.n; j++)
                            col = tfo_adjust(col, (unsigned char)line.s[j]);
                        continue;
                    }
                }
                fwrite(line.s, 1, line.n, stdout);
                putchar('\n');
                line.n = 0;
                col = 0;
            }
            tfo_put(&line, (char)c);
        }
    }
    fwrite(line.s, 1, line.n, stdout);
    free(line.s);
    if (fd > 0) close(fd);
    return rc;
}

static int do_fold(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "bytes", SU_NO_ARG, 0, 'b' },
        { "spaces", SU_NO_ARG, 0, 's' },
        { "width", SU_REQ_ARG, 0, 'w' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    size_t width = 80;
    int spaces = 0, c;
    tfo_bytes = 0;
    while ((c = su_getopt_long(argc, argv, "bsw:0::1::2::3::4::5::6::7::8::9::", longs, 0)) != -1) {
        switch (c) {
        case 'b': tfo_bytes = 1; break;
        case 's': spaces = 1; break;
        case 'w': width = tfo_width(su_optarg); break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n", su_name);
            printf("Write each FILE to standard output with its lines broken to fit a width.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "A long option's argument is required by its short form too.\n"
                   "  -b, --bytes         measure in bytes, not in columns\n"
                   "  -s, --spaces        break after the last blank that fits, when there is one\n"
                   "  -w, --width=WIDTH   break at WIDTH columns, not at 80\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default:
            if (c >= '0' && c <= '9') {
                size_t len = su_optarg ? strlen(su_optarg) : 0;
                char *w = (char *)malloc(len + 2);
                if (!w) su_error(1, ENOMEM, "memory exhausted");
                w[0] = (char)c;
                if (len) memcpy(w + 1, su_optarg, len);
                w[len + 1] = 0;
                width = tfo_width(w);
                free(w);
                break;
            }
            su_try_help(1);
        }
    }
    int rc = 0;
    if (su_optind >= argc) rc = tfo_file("-", width, spaces);
    for (int i = su_optind; i < argc; i++) rc |= tfo_file(argv[i], width, spaces);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}
