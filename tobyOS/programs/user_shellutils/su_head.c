/* su_head.c -- head, as GNU coreutils 9.7's (logs/utilgate/staging/
 * head.sh): the first part of each FILE (or standard input, or "-").
 *
 * WHAT WAS MEASURED, in the C locale:
 *   - -n N lines (10 by default) or -c N bytes; -n -N / -c -N all but the
 *     last N. N is gnulib's xstrtoumax (su_txio.c) with the suffixes
 *     b k K m M G T P E Z Y R Q and their kB/KiB/KD forms -- but not g t
 *     c w B (1g is "invalid number of lines: '1g'"). One leading '-' is
 *     the "all but" sign and is not part of what a message quotes
 *     (`-n --3` is "invalid number of lines: '-3'"); a '+' is allowed.
 *     Too large is the largest number, never an error.
 *   - The old form, only as the FIRST argument: -NUM then any of b k m
 *     (bytes, times 512, 1024 or 1048576), c (bytes, no multiplier), l
 *     (lines, keeping one), q v z; anything else is "invalid trailing
 *     option -- X". A digit as a later option is the same message.
 *   - Headers "==> NAME <==" (standard input for "-"), a blank line
 *     before all but the first one PRINTED: with more than one FILE, or
 *     -v; -q never; the last of -q/-v wins. Printed once the file is
 *     open, before it is read, so a directory gets its header and then
 *     "error reading 'd': Is a directory".
 *   - The input is left where head stopped when it can seek: `head -n 1
 *     - - < file` prints line 1 then line 2, and after -c -N or -n -N the
 *     offset is just past what was printed (unchanged when nothing was).
 *     A pipe is read in 8 KiB pieces, so -n loses what it read past the
 *     last line; -c reads no more than it needs.
 *   - -n 0 and -c 0 open the file and read nothing; -n -0 copies it all.
 *   - Write errors as glibc's stdio makes them (su_txio.c): "error
 *     writing 'standard output'" when a write fails as it is made, "write
 *     error" at exit.
 *
 * The old head (sbase's) said "head: write: ..." for every write error,
 * read counts with atoi (no suffixes, no errors), had no -z, no long
 * options, no old form but a bare -NUM, and printed "head: open X:". */

#include "su_txio.c"

static struct {
    int count_lines;        /* -n, not -c */
    int elide;              /* the -N forms: all but the last N */
    uintmax_t n;
    int headers;            /* -1 never, 0 when several files, 1 always */
    char delim;
    int first;              /* no header printed yet */
    int presume_pipe;       /* ---presume-input-pipe: never seek for -N */
} txh;

static const char txh_suffixes[] = "bkKmMGTPEZYRQ0";

/* -n/-c's value: the "all but" sign, then a number */
static void txh_count(const char *arg, int lines) {
    const char *s = arg;
    txh.elide = 0;
    if (*s == '-') { txh.elide = 1; s++; }
    uintmax_t v;
    int rc = tx_xstrtoumax(s, &v, txh_suffixes);
    if (rc == TX_NUM_INVALID)
        tx_error(1, 0, "invalid number of %s: %s", lines ? "lines" : "bytes", tx_quote(s));
    txh.n = rc == TX_NUM_OVERFLOW ? UINTMAX_MAX : v;
    txh.count_lines = lines;
}

/* fwrite to standard output, and head's message when it fails */
static void txh_write(const void *p, size_t n) {
    if (n && tx_fwrite(p, n) < n) {
        int e = errno;
        tx_oerr = 0;            /* this message is the one: none at exit */
        tx_error(1, e, "error writing %s", tx_quoteaf("standard output"));
    }
}

static int txh_read_error(const char *disp) {
    tx_error(0, errno, "error reading %s", tx_quoteaf(disp));
    return 0;
}

static int txh_bytes(int fd, const char *disp, uintmax_t n) {
    char buf[8192];
    while (n) {
        size_t want = n < sizeof buf ? (size_t)n : sizeof buf;
        ssize_t r = tx_read(fd, buf, want);
        if (r < 0) return txh_read_error(disp);
        if (r == 0) break;
        txh_write(buf, (size_t)r);
        n -= (uintmax_t)r;
    }
    return 1;
}

static int txh_lines(int fd, const char *disp, uintmax_t n) {
    char buf[8192];
    while (n) {
        ssize_t r = tx_read(fd, buf, sizeof buf);
        if (r < 0) return txh_read_error(disp);
        if (r == 0) break;
        size_t used = (size_t)r;
        for (size_t i = 0; i < (size_t)r; i++)
            if (buf[i] == txh.delim && --n == 0) { used = i + 1; break; }
        /* what was read past the last line goes back, where it can */
        if (used < (size_t)r) (void)lseek(fd, -(off_t)((size_t)r - used), SEEK_CUR);
        txh_write(buf, used);
    }
    return 1;
}

/* everything: -n -0 and -c -0 */
static int txh_copy(int fd, const char *disp) {
    char buf[8192];
    for (;;) {
        ssize_t r = tx_read(fd, buf, sizeof buf);
        if (r < 0) return txh_read_error(disp);
        if (r == 0) return 1;
        txh_write(buf, (size_t)r);
    }
}

/* All but the last N bytes or lines. What may still be among the last N
 * is held in B[start, end); what cannot be any more is written. The
 * file's offset, where it can seek, ends just past what was written. */
static int txh_elide(int fd, const char *disp, uintmax_t n, int lines) {
    if (n == 0) return txh_copy(fd, disp);
    struct stat st;
    int seekable = !txh.presume_pipe && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    off_t pos0 = seekable ? lseek(fd, 0, SEEK_CUR) : -1;
    if (pos0 < 0) seekable = 0;
    uintmax_t written = 0;
    size_t cap = 65536, start = 0, end = 0;
    uintmax_t held_lines = 0;          /* delimiters in B */
    char *b = (char *)malloc(cap);
    if (!b) tx_error(1, ENOMEM, "memory exhausted");
    int ok = 1;
    for (;;) {
        if (end + 8192 > cap) {
            if (start > 0) {
                memmove(b, b + start, end - start);
                end -= start;
                start = 0;
            }
            if (end + 8192 > cap) {
                size_t nc = cap * 2;
                char *nb = (char *)realloc(b, nc);
                if (!nb) { free(b); tx_error(1, ENOMEM, "memory exhausted"); }
                b = nb;
                cap = nc;
            }
        }
        ssize_t r = tx_read(fd, b + end, 8192);
        if (r < 0) { ok = txh_read_error(disp); break; }
        if (r == 0) break;
        if (lines) {
            for (ssize_t i = 0; i < r; i++) if (b[end + (size_t)i] == txh.delim) held_lines++;
            end += (size_t)r;
            /* a line followed by N more whole lines is not among the last N */
            size_t from = start;
            while (held_lines > n) {
                char *nl = (char *)memchr(b + start, txh.delim, end - start);
                start = (size_t)(nl - b) + 1;
                held_lines--;
            }
            if (start > from) {
                txh_write(b + from, start - from);
                written += start - from;
            }
        } else {
            end += (size_t)r;
            if ((uintmax_t)(end - start) > n) {
                size_t out = (size_t)((uintmax_t)(end - start) - n);
                txh_write(b + start, out);
                written += out;
                start += out;
            }
        }
    }
    /* a last line with no delimiter is a line too */
    if (ok && lines && end > start && b[end - 1] != txh.delim && held_lines == n) {
        char *nl = (char *)memchr(b + start, txh.delim, end - start);
        if (nl) {
            size_t out = (size_t)(nl - b) + 1 - start;
            txh_write(b + start, out);
            written += out;
        }
    }
    free(b);
    if (seekable) (void)lseek(fd, pos0 + (off_t)written, SEEK_SET);
    return ok;
}

static int txh_file(const char *name, int nfiles) {
    int fd;
    const char *disp;
    if (!strcmp(name, "-")) {
        fd = 0;
        disp = "standard input";
    } else {
        fd = su_open_read(name, 0);
        if (fd < 0) {
            tx_error(0, errno, "cannot open %s for reading", tx_quoteaf(name));
            return 0;
        }
        disp = name;
    }
    if (txh.headers > 0 || (txh.headers == 0 && nfiles > 1)) {
        tx_printf("%s==> %s <==\n", txh.first ? "" : "\n", disp);
        txh.first = 0;
    }
    int ok;
    if (txh.elide) ok = txh_elide(fd, disp, txh.n, txh.count_lines);
    else if (txh.count_lines) ok = txh_lines(fd, disp, txh.n);
    else ok = txh_bytes(fd, disp, txh.n);
    if (fd != 0 && close(fd) < 0) {
        tx_error(0, errno, "failed to close %s", tx_quoteaf(name));
        ok = 0;
    }
    return ok;
}

static void txh_help(void) {
    tx_printf("Usage: %s [OPTION]... [FILE]...\n"
              "Print the first 10 lines of each FILE to standard output.\n"
              "With more than one FILE, each is preceded by a header naming it.\n"
              "\n"
              "With no FILE, or when FILE is -, read standard input.\n"
              "\n"
              "Mandatory arguments to long options are mandatory for short options too.\n"
              "  -c, --bytes=[-]NUM       the first NUM bytes of each file; with a\n"
              "                             leading '-', all of it but the last NUM bytes\n"
              "  -n, --lines=[-]NUM       the first NUM lines, not the first 10; with a\n"
              "                             leading '-', all of it but the last NUM lines\n"
              "  -q, --quiet, --silent    never print the headers naming the files\n"
              "  -v, --verbose            always print the headers naming the files\n"
              "  -z, --zero-terminated    lines end in NUL, not newline\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n"
              "\n"
              "NUM may end in a multiplier: b 512, kB 1000, K 1024, MB 1000*1000,\n"
              "M 1024*1024, GB 1000*1000*1000, G 1024*1024*1024, and the same for\n"
              "T, P, E, Z, Y, R and Q; KiB is K, MiB is M, and so on.\n", su_name);
}

static int do_head(int argc, char **argv) {
    enum { PRESUME_INPUT_PIPE = 0x100 };
    static const struct su_option longs[] = {
        { "bytes", SU_REQ_ARG, 0, 'c' },
        { "lines", SU_REQ_ARG, 0, 'n' },
        { "-presume-input-pipe", SU_NO_ARG, 0, PRESUME_INPUT_PIPE },
        { "quiet", SU_NO_ARG, 0, 'q' },
        { "silent", SU_NO_ARG, 0, 'q' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    tx_begin(1);
    memset(&txh, 0, sizeof txh);
    txh.count_lines = 1;
    txh.n = 10;
    txh.delim = '\n';
    txh.first = 1;

    /* the old form: -NUM[bcklmqvz]..., as the first argument only */
    char **av = argv;
    int ac = argc;
    if (argc > 1 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
        const char *p = argv[1] + 1;
        const char *digits = p;
        while (isdigit((unsigned char)*p)) p++;
        size_t nd = (size_t)(p - digits);
        char mult = 0;
        int lines = 1;
        for (; *p; p++) {
            switch (*p) {
            case 'c': lines = 0; mult = 0; break;
            case 'b': case 'k': case 'm': lines = 0; mult = *p; break;
            case 'l': lines = 1; break;
            case 'q': txh.headers = -1; break;
            case 'v': txh.headers = 1; break;
            case 'z': txh.delim = 0; break;
            default:
                tx_error(0, 0, "invalid trailing option -- %c", *p);
                tx_try_help(1);
            }
        }
        char *num = (char *)malloc(nd + 2);
        if (!num) tx_error(1, ENOMEM, "memory exhausted");
        memcpy(num, digits, nd);
        num[nd] = mult;
        num[nd + 1] = 0;
        uintmax_t v;
        int rc = tx_xstrtoumax(num, &v, txh_suffixes);
        if (rc == TX_NUM_INVALID)
            tx_error(1, 0, "invalid number of %s: %s", lines ? "lines" : "bytes", tx_quote(num));
        txh.n = rc == TX_NUM_OVERFLOW ? UINTMAX_MAX : v;
        txh.count_lines = lines;
        free(num);
        /* the rest is parsed as though the old form were not there */
        av = (char **)malloc(((size_t)argc + 1) * sizeof *av);
        if (!av) tx_error(1, ENOMEM, "memory exhausted");
        av[0] = argv[0];
        for (int i = 2; i <= argc; i++) av[i - 1] = argv[i];
        ac = argc - 1;
    }

    int c;
    while ((c = su_getopt_long(ac, av, "c:n:qvz0123456789", longs, 0)) != -1) {
        switch (c) {
        case 'c': txh_count(su_optarg, 0); break;
        case 'n': txh_count(su_optarg, 1); break;
        case 'q': txh.headers = -1; break;
        case 'v': txh.headers = 1; break;
        case 'z': txh.delim = 0; break;
        case PRESUME_INPUT_PIPE: txh.presume_pipe = 1; break;
        case SU_HELP_OPT: txh_help(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default:
            if (c >= '0' && c <= '9') tx_error(0, 0, "invalid trailing option -- %c", c);
            tx_try_help(1);
        }
    }
    int nfiles = ac - su_optind;
    int ok = 1;
    if (nfiles == 0) ok = txh_file("-", 1);
    for (int i = su_optind; i < ac; i++) ok &= txh_file(av[i], nfiles);
    return ok ? 0 : 1;
}
