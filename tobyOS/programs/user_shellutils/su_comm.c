/* su_comm.c -- comm, as GNU coreutils 9.7's (logs/utilgate/
 * comm.sh): two sorted files merged line by line into three columns --
 * lines only in FILE1, lines only in FILE2, lines in both -- each column
 * indented by one delimiter (a TAB, or --output-delimiter's string; an
 * empty string means one NUL byte) for every shown column left of it.
 * -1 -2 -3 hide columns; --total ends with the three counts and "total";
 * -z makes NUL the line terminator, in and out. Lines compare as bytes
 * (the C locale), a shorter line before a longer one it begins.
 *
 * THE ORDER CHECK, measured against GNU (dozens of small file pairs):
 * every time a file's next line is read it is compared with the line it
 * follows, and a line smaller than its predecessor is a disorder. With
 * --check-order the first disorder ends the run ("file N is not in
 * sorted order", status 1); with --nocheck-order nothing is checked. By
 * default a disorder is only reported once some line has been unpairable
 * -- printed in column 1 or 2, shown or not -- so two identical unsorted
 * files pass; it is a warning, given once per file, and the run ends
 * with "input is not in sorted order" and status 1. When a file runs out,
 * its last two lines are compared once more: [a,c,b] against [a,c]
 * warns, since b became unpairable after it was read. "-" twice reads
 * one standard input alternately, and GNU then fails closing it the
 * second time: "comm: -", status 1, before any total.
 *
 * The old comm had no --check-order, --total, -z or delimiter choice and
 * never checked the order. */

struct tcm_in {
    int fd;
    int eof, err;
    size_t pos, len;
    unsigned char buf[16384];
};

struct tcm_line { char *s; size_t n, cap; };

static void tcm_append(struct tcm_line *l, const unsigned char *p, size_t n) {
    if (l->n + n + 1 > l->cap) {
        size_t nc = l->cap ? l->cap : 128;
        while (nc < l->n + n + 1) nc *= 2;
        char *ns = (char *)realloc(l->s, nc);
        if (!ns) su_error(1, ENOMEM, "memory exhausted");
        l->s = ns;
        l->cap = nc;
    }
    memcpy(l->s + l->n, p, n);
    l->n += n;
    l->s[l->n] = 0;
}

/* the next line of IN into L (its terminator not kept): 1, or 0 at the
 * end or on an error (IN->err says which) */
static int tcm_getline(struct tcm_in *in, struct tcm_line *l, int delim) {
    int any = 0;
    l->n = 0;
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
        tcm_append(l, p, take);
        any = 1;
        in->pos += take;
        if (q) { in->pos++; return 1; }
    }
    if (l->s) l->s[l->n] = 0;
    return any && !in->err;
}

static int tcm_cmp(const struct tcm_line *a, const struct tcm_line *b) {
    size_t m = a->n < b->n ? a->n : b->n;
    int c = m ? memcmp(a->s, b->s, m) : 0;
    if (c) return c;
    return a->n < b->n ? -1 : a->n > b->n;
}

static struct {
    int hide[3];
    int mode;               /* 0 default, 1 --check-order, 2 --nocheck-order */
    int delim;              /* the line terminator */
    const char *odelim;
    size_t odlen;
    int seen_unpaired;
    int warned[2];
    int disorder;
    const char *name[2];
    struct tcm_in *in[2];
    /* per file: the line being merged, the one before it (and whether
     * each is there), and the buffer the next one is read into */
    struct tcm_line cur[2], last[2], spare[2];
    int have[2], have_last[2];
} tcm;

/* PREV then NEXT in file I: a disorder if NEXT sorts first */
static void tcm_check(int i, const struct tcm_line *prev, const struct tcm_line *next) {
    if (tcm.mode == 2 || tcm.warned[i]) return;
    if (tcm.mode == 0 && !tcm.seen_unpaired) return;
    if (tcm_cmp(prev, next) <= 0) return;
    if (tcm.mode == 1) su_error(1, 0, "file %d is not in sorted order", i + 1);
    su_error(0, 0, "file %d is not in sorted order", i + 1);
    tcm.warned[i] = 1;
    tcm.disorder = 1;
}

static void tcm_read_err(int i) {
    su_error(1, tcm.in[i]->err, "%s", su_quotef(tcm.name[i]));
}

/* file I's next line becomes its current one */
static void tcm_advance(int i) {
    if (tcm_getline(tcm.in[i], &tcm.spare[i], tcm.delim)) {
        tcm_check(i, &tcm.cur[i], &tcm.spare[i]);
        struct tcm_line t = tcm.last[i];
        tcm.last[i] = tcm.cur[i];
        tcm.cur[i] = tcm.spare[i];
        tcm.spare[i] = t;
        tcm.have_last[i] = 1;
        return;
    }
    if (tcm.in[i]->err) tcm_read_err(i);
    /* the end: the last two lines are compared again, under the check
     * as it is now */
    if (tcm.have_last[i]) tcm_check(i, &tcm.last[i], &tcm.cur[i]);
    tcm.have[i] = 0;
}

static void tcm_out(const struct tcm_line *l, int col) {
    for (int c = 0; c < col; c++)
        if (!tcm.hide[c]) fwrite(tcm.odelim, 1, tcm.odlen, stdout);
    fwrite(l->s, 1, l->n, stdout);
    putchar(tcm.delim);
}

static int do_comm(int argc, char **argv) {
    enum { CHECK_OPT = 0x100, NOCHECK_OPT, ODELIM_OPT, TOTAL_OPT };
    static const struct su_option longs[] = {
        { "check-order", SU_NO_ARG, 0, CHECK_OPT },
        { "nocheck-order", SU_NO_ARG, 0, NOCHECK_OPT },
        { "output-delimiter", SU_REQ_ARG, 0, ODELIM_OPT },
        { "total", SU_NO_ARG, 0, TOTAL_OPT },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&tcm, 0, sizeof tcm);
    tcm.delim = '\n';
    int total = 0, c;
    while ((c = su_getopt_long(argc, argv, "123z", longs, 0)) != -1) {
        switch (c) {
        case '1': tcm.hide[0] = 1; break;
        case '2': tcm.hide[1] = 1; break;
        case '3': tcm.hide[2] = 1; break;
        case 'z': tcm.delim = 0; break;
        case CHECK_OPT: tcm.mode = 1; break;
        case NOCHECK_OPT: tcm.mode = 2; break;
        case TOTAL_OPT: total = 1; break;
        case ODELIM_OPT: {
            /* an empty string is one NUL byte */
            const char *d = *su_optarg ? su_optarg : "";
            size_t n = *su_optarg ? strlen(su_optarg) : 1;
            if (tcm.odelim && (tcm.odlen != n || memcmp(tcm.odelim, d, n)))
                su_error(1, 0, "multiple output delimiters specified");
            tcm.odelim = d;
            tcm.odlen = n;
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE1 FILE2\n", su_name);
            printf("Merge two sorted files, FILE1 and FILE2, line by line into three columns:\n"
                   "the lines found only in FILE1, those found only in FILE2, and those found\n"
                   "in both. Either file (not both) may be -, meaning standard input.\n"
                   "\n"
                   "  -1                      leave out the first column\n"
                   "  -2                      leave out the second column\n"
                   "  -3                      leave out the third column\n"
                   "\n"
                   "      --check-order       stop at the first line out of sorted order, even\n"
                   "                            when every line has a partner\n"
                   "      --nocheck-order     never check the order\n"
                   "      --output-delimiter=STR  put STR between columns, not a TAB\n"
                   "      --total             finish with a line of the three counts\n"
                   "  -z, --zero-terminated   lines end in NUL, not newline\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "Lines are compared byte by byte. By default an unsorted input is reported\n"
                   "once a line without a partner has been seen, and the status is then 1.\n"
                   "For example, '%s -12 a b' shows only the lines a and b share.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (!tcm.odelim) { tcm.odelim = "\t"; tcm.odlen = 1; }
    int nop = argc - su_optind;
    if (nop < 2) {
        if (nop == 0) su_error(0, 0, "missing operand");
        else su_error(0, 0, "missing operand after %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
    if (nop > 2) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 2]));
        su_try_help(1);
    }
    static struct tcm_in ins[2];
    memset(ins, 0, sizeof ins);
    for (int i = 0; i < 2; i++) {
        const char *n = argv[su_optind + i];
        tcm.name[i] = n;
        if (!strcmp(n, "-")) {
            /* "-" twice is one stream, read alternately */
            if (i == 1 && !strcmp(tcm.name[0], "-")) { tcm.in[1] = tcm.in[0]; continue; }
            ins[i].fd = 0;
        } else {
            ins[i].fd = su_open_read(n, 0);
            if (ins[i].fd < 0) su_error(1, errno, "%s", su_quotef(n));
        }
        tcm.in[i] = &ins[i];
    }

    for (int i = 0; i < 2; i++) {
        tcm.have[i] = tcm_getline(tcm.in[i], &tcm.cur[i], tcm.delim);
        if (!tcm.have[i] && tcm.in[i]->err) tcm_read_err(i);
    }
    uintmax_t count[3] = { 0, 0, 0 };
    while (tcm.have[0] || tcm.have[1]) {
        int order;
        if (!tcm.have[1]) order = -1;
        else if (!tcm.have[0]) order = 1;
        else order = tcm_cmp(&tcm.cur[0], &tcm.cur[1]);
        if (order == 0) {
            count[2]++;
            if (!tcm.hide[2]) tcm_out(&tcm.cur[0], 2);
            tcm_advance(0);
            tcm_advance(1);
        } else if (order < 0) {
            count[0]++;
            tcm.seen_unpaired = 1;
            if (!tcm.hide[0]) tcm_out(&tcm.cur[0], 0);
            tcm_advance(0);
        } else {
            count[1]++;
            tcm.seen_unpaired = 1;
            if (!tcm.hide[1]) tcm_out(&tcm.cur[1], 1);
            tcm_advance(1);
        }
    }
    if (tcm.in[0] == tcm.in[1]) su_error(1, 0, "-");
    for (int i = 0; i < 2; i++)
        if (tcm.in[i]->fd > 0) close(tcm.in[i]->fd);
    if (total) {
        printf("%ju", count[0]);
        fwrite(tcm.odelim, 1, tcm.odlen, stdout);
        printf("%ju", count[1]);
        fwrite(tcm.odelim, 1, tcm.odlen, stdout);
        printf("%ju", count[2]);
        fwrite(tcm.odelim, 1, tcm.odlen, stdout);
        fputs("total", stdout);
        putchar(tcm.delim);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    if (tcm.disorder) su_error(1, 0, "input is not in sorted order");
    return 0;
}
