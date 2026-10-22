/* su_tail.c -- tail, as GNU coreutils 9.7's (logs/utilgate/staging/
 * tail.sh): the last part of each FILE (or standard input, or "-"), and
 * with -f what is added to it afterwards.
 *
 * WHAT WAS MEASURED, in the C locale:
 *   - -n N lines (10 by default), -c N bytes; -n +N / -c +N from the Nth
 *     on (+0 is +1). N is gnulib's xstrtoumax with head's suffixes
 *     (su_txio.c); a '-' before it is dropped and not quoted in the
 *     message, a '+' stays ("invalid number of lines: '+x'"). -n 0 and
 *     -c 0 without -f open nothing at all (`tail -n 0 nosuch` is quiet).
 *   - The old forms, as the first argument and only with at most one
 *     file after them (or `--` and one): [+-][NUM][b|c|l][f] -- b is
 *     512-byte blocks, f is --follow. A '+' form is taken when
 *     _POSIX2_VERSION (200809 when unset or not a number) is below
 *     200112 or not below 200809; "-" and "-c" alone only below 200112.
 *     A digit given as a later option is "option used in invalid context
 *     -- 3".
 *   - Headers as head's. From a file that can seek, the last N are found
 *     from its end and the offset is left at the end; from the current
 *     offset, so `(head -c 45; tail -c 10) < f` prints what is left.
 *   - -f follows each file by descriptor (what is appended to it), -F
 *     and --follow=name by name (a file removed is "has become
 *     inaccessible", one created or replaced is read from its start:
 *     "has appeared;  following new file", "has been replaced;
 *     following new file", "has been replaced with an untailable file",
 *     "has become accessible"). A file that shrinks is "FILE: file
 *     truncated" and is read again from its start. A directory is
 *     "cannot follow end of this type of file" ("; giving up on this
 *     name" without --retry). A pipe on standard input is not followed;
 *     "-" by name is refused. With none left, "no files remaining".
 *     --pid=PID: stop once PID is gone and nothing more was read. -s N:
 *     the pause between looks, strtod's number (no SSE: su_txio.c).
 *   - GNU follows with inotify, seeing every change as it happens. This
 *     follows by looking every -s seconds, at each name every time
 *     (GNU's own polling looks only after --max-unchanged-stats quiet
 *     rounds), which is what inotify reports for changes further apart
 *     than the pause. When following ends with --pid, GNU's status is 0
 *     whenever it used inotify -- even after a file that could not be
 *     opened -- and it does not use it for a symbolic link, standard
 *     input, a file neither regular nor a FIFO, no file open at all, or
 *     -f after a failed open (all measured); that is the status here.
 *   - Write errors as head's ("error writing 'standard output'" at once,
 *     "write error" at exit or when following).
 *
 * The old tail read counts with its own parser (no suffixes, -n -5 the
 * same as 5), said "tail: cannot open 'X'" with no reason, followed one
 * file by re-reading it every second, and printed every other error as
 * a usage line. */

#include "su_txio.c"

struct txt_file {
    const char *name;           /* as given; "-" is standard input */
    const char *disp;           /* "standard input" for "-" */
    int fd;                     /* -1: not open */
    int ignore;                 /* no longer followed */
    int errnum;                 /* why the name could not be opened, or 0 */
    int tailable;               /* last seen as a file tail can follow */
    off_t size;                 /* where reading stopped (regular files) */
    dev_t dev;
    ino_t ino;
    mode_t mode;
};

static struct {
    int count_lines;
    int from_start;
    uintmax_t n;
    int headers;                /* -1 never, 0 several files, 1 always */
    char delim;
    int forever;
    int follow_name;
    int reopen;                 /* --retry */
    int presume_pipe;
    int no_inotify;             /* ---disable-inotify */
    unsigned long long interval_ns;
    pid_t *pids;
    int npids;
    int first;                  /* no header printed yet */
    struct txt_file *last;       /* whose data was printed last */
} txt;

static const char txt_suffixes[] = "bkKmMGTPEZYRQ0";

static void txt_count(const char *arg, int lines) {
    const char *s = arg;
    txt.from_start = 0;
    if (*s == '+') txt.from_start = 1;
    else if (*s == '-') s++;
    uintmax_t v;
    int rc = tx_xstrtoumax(s, &v, txt_suffixes);
    if (rc == TX_NUM_INVALID)
        tx_error(1, 0, "invalid number of %s: %s", lines ? "lines" : "bytes", tx_quote(s));
    txt.n = rc == TX_NUM_OVERFLOW ? UINTMAX_MAX : v;
    txt.count_lines = lines;
}

/* _POSIX2_VERSION as GNU reads it: a number, or 200809 */
static long txt_posix2(void) {
    const char *s = getenv("_POSIX2_VERSION");
    if (s && *s) {
        char *e;
        errno = 0;
        long v = strtol(s, &e, 10);
        if (e != s && !*e && !errno) return v;
    }
    return 200809;
}

/* the old forms; 1 if argv[1] was one (and has been taken) */
static int txt_obsolete(int argc, char **argv) {
    if (!(argc == 2 ||
          (argc == 3 && !(argv[2][0] == '-' && argv[2][1])) ||
          ((argc == 3 || argc == 4) && !strcmp(argv[2], "--"))))
        return 0;
    long ver = txt_posix2();
    int old = ver < 200112;
    const char *p = argv[1];
    int from_start;
    switch (*p++) {
    case '+':
        if (!old && ver < 200809) return 0;
        from_start = 1;
        break;
    case '-':
        if (!old && (!*p || (p[0] == 'c' && !p[1]))) return 0;
        from_start = 0;
        break;
    default:
        return 0;
    }
    const char *digits = p;
    while (isdigit((unsigned char)*p)) p++;
    size_t nd = (size_t)(p - digits);
    int lines = 1;
    uintmax_t mult = 1;
    if (*p == 'b') { lines = 0; mult = 512; p++; }
    else if (*p == 'c') { lines = 0; p++; }
    else if (*p == 'l') p++;
    int follow = 0;
    if (*p == 'f') { follow = 1; p++; }
    if (*p) return 0;
    uintmax_t v = 10;
    if (nd) {
        v = 0;
        for (size_t i = 0; i < nd; i++) {
            unsigned d = (unsigned)(digits[i] - '0');
            if (v > (UINTMAX_MAX - d) / 10) { v = UINTMAX_MAX; break; }
            v = v * 10 + d;
        }
    }
    if (tx_mul(&v, mult) != TX_NUM_OK) v = UINTMAX_MAX;
    txt.n = v;
    txt.count_lines = lines;
    txt.from_start = from_start;
    if (follow) txt.forever = 1;
    return 1;
}

static void txt_write(const void *p, size_t n) {
    if (n && tx_fwrite(p, n) < n) {
        int e = errno;
        tx_oerr = 0;
        tx_error(1, e, "error writing %s", tx_quoteaf("standard output"));
    }
}

static void txt_header(struct txt_file *f) {
    tx_printf("%s==> %s <==\n", txt.first ? "" : "\n", f->disp);
    txt.first = 0;
}

static int txt_show_headers;

static int txt_read_error(struct txt_file *f) {
    tx_error(0, errno, "error reading %s", tx_quoteaf(f->disp));
    return 0;
}

/* everything from where FD is to its end; -1 on a read error, else the
 * bytes read. A header goes first, when the output changes file. */
static intmax_t txt_dump(struct txt_file *f, int fd) {
    char buf[8192];
    intmax_t total = 0;
    for (;;) {
        ssize_t r = tx_read(fd, buf, sizeof buf);
        if (r < 0) { txt_read_error(f); return -1; }
        if (r == 0) return total;
        if (txt.last != f) {
            if (txt_show_headers) txt_header(f);
            txt.last = f;
        }
        txt_write(buf, (size_t)r);
        total += r;
    }
}

/* the last N lines of a file that can seek: found from END backwards,
 * not before START; the offset is left at the end */
static int txt_lines_seek(struct txt_file *f, int fd, uintmax_t n, off_t start, off_t end) {
    char buf[8192];
    off_t p = end, found = start;
    uintmax_t seen = 0;
    while (p > start) {
        size_t len = (size_t)((p - start) % (off_t)sizeof buf);
        if (len == 0) len = sizeof buf;
        p -= (off_t)len;
        if (lseek(fd, p, SEEK_SET) < 0) return txt_read_error(f);
        size_t got = 0;
        while (got < len) {
            ssize_t r = tx_read(fd, buf + got, len - got);
            if (r < 0) return txt_read_error(f);
            if (r == 0) break;
            got += (size_t)r;
        }
        for (size_t i = got; i > 0; i--) {
            if (buf[i - 1] != txt.delim) continue;
            if (p + (off_t)i == end) continue;      /* the last line's own end */
            if (++seen == n) { found = p + (off_t)i; goto out; }
        }
    }
out:
    if (lseek(fd, found, SEEK_SET) < 0) return txt_read_error(f);
    return txt_dump(f, fd) >= 0;
}

/* the last N lines or bytes of what cannot seek: what may still be among
 * them is held, the rest dropped as it comes */
static int txt_tail_pipe(struct txt_file *f, int fd, uintmax_t n, int lines) {
    size_t cap = 65536, start = 0, end = 0;
    uintmax_t held = 0;
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
                char *nb = (char *)realloc(b, cap * 2);
                if (!nb) { free(b); tx_error(1, ENOMEM, "memory exhausted"); }
                b = nb;
                cap *= 2;
            }
        }
        ssize_t r = tx_read(fd, b + end, 8192);
        if (r < 0) { ok = txt_read_error(f); break; }
        if (r == 0) break;
        if (lines) {
            for (ssize_t i = 0; i < r; i++) if (b[end + (size_t)i] == txt.delim) held++;
            end += (size_t)r;
            while (held > n) {
                char *nl = (char *)memchr(b + start, txt.delim, end - start);
                start = (size_t)(nl - b) + 1;
                held--;
            }
        } else {
            end += (size_t)r;
            if ((uintmax_t)(end - start) > n) start = end - (size_t)n;
        }
    }
    if (ok && lines && end > start && b[end - 1] != txt.delim && held == n) {
        /* the unterminated last line is one of the N: one fewer before it */
        char *nl = (char *)memchr(b + start, txt.delim, end - start);
        start = nl ? (size_t)(nl - b) + 1 : end;
    }
    if (ok && end > start) {
        if (txt.last != f) {
            if (txt_show_headers) txt_header(f);
            txt.last = f;
        }
        txt_write(b + start, end - start);
    }
    free(b);
    return ok;
}

/* +N: skip N-1 lines or bytes, then everything */
static int txt_from(struct txt_file *f, int fd, uintmax_t n, int lines, int seekable) {
    uintmax_t skip = n ? n - 1 : 0;
    if (!lines && seekable) {
        /* a skip too far to seek to is read past instead (and is the end) */
        off_t cur = lseek(fd, 0, SEEK_CUR);
        if (cur >= 0 && skip <= (uintmax_t)(INTMAX_MAX / 2) - (uintmax_t)cur) {
            if (skip && lseek(fd, (off_t)skip, SEEK_CUR) < 0) return txt_read_error(f);
            return txt_dump(f, fd) >= 0;
        }
    }
    char buf[8192];
    while (skip) {
        ssize_t r = tx_read(fd, buf, sizeof buf);
        if (r < 0) return txt_read_error(f);
        if (r == 0) return 1;
        size_t i = 0;
        if (lines) {
            for (; i < (size_t)r && skip; i++) if (buf[i] == txt.delim) skip--;
        } else {
            i = (uintmax_t)r < skip ? (size_t)r : (size_t)skip;
            skip -= i;
        }
        if (!skip && i < (size_t)r) {
            if (txt.last != f) {
                if (txt_show_headers) txt_header(f);
                txt.last = f;
            }
            txt_write(buf + i, (size_t)r - i);
        }
    }
    return txt_dump(f, fd) >= 0;
}

static int txt_tailable(mode_t m) {
    return S_ISREG(m) || S_ISFIFO(m) || S_ISCHR(m) || S_ISSOCK(m);
}

static void txt_record(struct txt_file *f, int fd, const struct stat *st) {
    f->fd = fd;
    f->dev = st->st_dev;
    f->ino = st->st_ino;
    f->mode = st->st_mode;
    f->tailable = 1;
    f->errnum = 0;
    f->size = S_ISREG(st->st_mode) ? lseek(fd, 0, SEEK_CUR) : 0;
    if (f->size < 0) f->size = 0;
}

/* the first look at one file: its last part, and what following needs */
static int txt_file(struct txt_file *f) {
    int fd;
    f->fd = -1;
    /* GNU counts a file as the last one shown once it is looked at, even
     * when it cannot be opened: `tail -F g h` with h made later prints
     * h's lines with no header, `tail -F h g` puts one before them */
    txt.last = f;
    if (!strcmp(f->name, "-")) {
        fd = 0;
    } else {
        fd = su_open_read(f->name, 0);
        if (fd < 0) {
            f->errnum = errno;
            tx_error(0, errno, "cannot open %s for reading", tx_quoteaf(f->name));
            /* only a name followed with --retry is looked for again --
             * and a descriptor with --retry, until it first opens */
            f->ignore = !(txt.forever && txt.reopen);
            f->tailable = 1;
            return 0;
        }
    }
    if (txt_show_headers) txt_header(f);
    struct stat st;
    int have_st = fstat(fd, &st) == 0;
    int seekable = !txt.presume_pipe && have_st && S_ISREG(st.st_mode);
    int ok;
    if (txt.from_start) {
        ok = txt_from(f, fd, txt.n, txt.count_lines, seekable);
    } else if (txt.n == 0) {
        ok = 1;
        if (seekable) (void)lseek(fd, 0, SEEK_END);
    } else if (seekable && st.st_size > 0) {
        off_t start = lseek(fd, 0, SEEK_CUR);
        off_t end = lseek(fd, 0, SEEK_END);
        if (start < 0 || end < 0) ok = txt_read_error(f);
        else if (start > end) ok = 1;
        else if (txt.count_lines) ok = txt_lines_seek(f, fd, txt.n, start, end);
        else {
            off_t from = (uintmax_t)(end - start) > txt.n ? end - (off_t)txt.n : start;
            if (lseek(fd, from, SEEK_SET) < 0) ok = txt_read_error(f);
            else ok = txt_dump(f, fd) >= 0;
        }
    } else {
        ok = txt_tail_pipe(f, fd, txt.n, txt.count_lines);
    }
    if (!txt.forever) {
        if (fd != 0 && close(fd) < 0) {
            tx_error(0, errno, "error reading %s", tx_quoteaf(f->disp));
            ok = 0;
        }
        return ok;
    }
    if (!have_st) {
        tx_error(0, errno, "cannot fstat %s", tx_quoteaf(f->disp));
        ok = 0;
    }
    if (!have_st || !txt_tailable(st.st_mode)) {
        if (have_st)
            tx_error(0, 0, "%s: cannot follow end of this type of file%s",
                     tx_quotef(f->disp), txt.reopen ? "" : "; giving up on this name");
        f->ignore = !(txt.reopen && txt.follow_name);
        f->tailable = 0;
        f->errnum = -1;
        if (fd != 0) close(fd);
        return 0;
    }
    if (!ok) {
        f->ignore = 1;
        if (fd != 0) close(fd);
        return 0;
    }
    txt_record(f, fd, &st);
    return ok;
}

/* following by name: has the name gone, come, or changed file? */
static void txt_recheck(struct txt_file *f) {
    struct stat st;
    if (stat(f->name, &st) < 0) {
        int e = errno;
        if (f->fd >= 0) {
            close(f->fd);
            f->fd = -1;
            if (txt.reopen) {
                tx_error(0, e, "%s has become inaccessible", tx_quoteaf(f->name));
            } else {
                tx_error(0, e, "%s", tx_quotef(f->name));
                f->ignore = 1;
            }
        }
        if (f->errnum != -1) f->errnum = e;
        return;
    }
    if (!txt_tailable(st.st_mode)) {
        if (f->tailable) {
            tx_error(0, 0, "%s has been replaced with an untailable file", tx_quoteaf(f->name));
            if (f->fd >= 0) { close(f->fd); f->fd = -1; }
            if (!txt.reopen) f->ignore = 1;
        }
        f->tailable = 0;
        f->errnum = -1;
        return;
    }
    if (f->fd >= 0 && st.st_dev == f->dev && st.st_ino == f->ino) return;
    int fd = su_open_read(f->name, 0);
    if (fd < 0) {
        int e = errno;
        if (f->fd >= 0) { close(f->fd); f->fd = -1; }
        if (f->errnum != e) tx_error(0, e, "cannot open %s for reading", tx_quoteaf(f->name));
        f->errnum = e;
        return;
    }
    if (f->fd >= 0) {
        tx_error(0, 0, "%s has been replaced;  following new file", tx_quoteaf(f->name));
        close(f->fd);
    } else if (f->errnum > 0 && f->errnum != ENOENT && f->errnum != ENOTDIR) {
        tx_error(0, 0, "%s has become accessible", tx_quoteaf(f->name));
    } else {
        tx_error(0, 0, "%s has appeared;  following new file", tx_quoteaf(f->name));
    }
    struct stat fst;
    if (fstat(fd, &fst) < 0) fst = st;
    txt_record(f, fd, &fst);
    f->size = 0;
}

/* is any --pid process still there? (signal 0 sends nothing) */
static int txt_pids_alive(void) {
    for (int i = 0; i < txt.npids; i++) {
        if (txt.pids[i] == 0) return 1;
        if (kill(txt.pids[i], 0) == 0 || errno == EPERM) return 1;
    }
    return 0;
}

static int txt_follow(struct txt_file *fs, int nf, int ok) {
    /* standard input that is a pipe is not followed; when that is every
     * file, there is nothing to follow and nothing to complain of */
    int pipes = 0;
    for (int i = 0; i < nf; i++) {
        struct txt_file *f = &fs[i];
        if (!strcmp(f->name, "-") && f->fd >= 0 && (S_ISFIFO(f->mode) || S_ISSOCK(f->mode))) {
            f->ignore = 1;
            pipes++;
        }
    }
    if (pipes == nf) return ok;

    /* would GNU have followed with inotify? (its status depends on it) */
    int inotify = !txt.no_inotify && !(!ok && !txt.follow_name);
    int any_open = 0;
    for (int i = 0; i < nf && inotify; i++) {
        struct txt_file *f = &fs[i];
        struct stat ls;
        if (!strcmp(f->name, "-")) { if (f->fd >= 0) inotify = 0; continue; }
        if (lstat(f->name, &ls) == 0 && S_ISLNK(ls.st_mode)) inotify = 0;
        if (f->fd >= 0) {
            any_open = 1;
            if (!S_ISREG(f->mode) && !S_ISFIFO(f->mode)) inotify = 0;
        }
    }
    if (!any_open) inotify = 0;

    int writer_dead = 0;
    for (;;) {
        int any_input = 0, live = 0;
        for (int i = 0; i < nf; i++) {
            struct txt_file *f = &fs[i];
            if (f->ignore) continue;
            if (strcmp(f->name, "-") && (txt.follow_name || (f->fd < 0 && txt.reopen)))
                txt_recheck(f);
            if (f->ignore) continue;
            if (f->fd < 0) { live++; continue; }
            live++;
            struct stat st;
            if (fstat(f->fd, &st) < 0) {
                tx_error(0, errno, "%s", tx_quotef(f->disp));
                close(f->fd);
                f->fd = -1;
                f->ignore = 1;
                live--;
                continue;
            }
            if (S_ISREG(st.st_mode)) {
                if (st.st_size < f->size) {
                    tx_error(0, 0, "%s: file truncated", tx_quotef(f->disp));
                    (void)lseek(f->fd, 0, SEEK_SET);
                    f->size = 0;
                }
                if (st.st_size == f->size) continue;
            }
            intmax_t got = txt_dump(f, f->fd);
            if (got < 0) {
                close(f->fd);
                f->fd = -1;
                f->ignore = 1;
                live--;
                continue;
            }
            if (got > 0) {
                any_input = 1;
                f->size += (off_t)got;
            }
        }
        if (!live) tx_error(1, 0, "no files remaining");
        if (tx_oflush() < 0) {
            int e = errno;
            tx_oerr = 0;
            tx_error(1, e, "write error");
        }
        if (!any_input) {
            if (writer_dead) break;
            if (txt.npids) writer_dead = !txt_pids_alive();
            if (!writer_dead) tx_nap(txt.interval_ns);
        }
    }
    return inotify ? 1 : ok;
}

static void txt_help(void) {
    tx_printf("Usage: %s [OPTION]... [FILE]...\n"
              "Print the last 10 lines of each FILE to standard output.\n"
              "With more than one FILE, each is preceded by a header naming it.\n"
              "\n"
              "With no FILE, or when FILE is -, read standard input.\n"
              "\n"
              "Mandatory arguments to long options are mandatory for short options too.\n"
              "  -c, --bytes=[+]NUM       the last NUM bytes; with +NUM, everything from\n"
              "                             byte NUM of each file on\n"
              "  -f, --follow[={name|descriptor}]\n"
              "                           keep printing what is added to the files;\n"
              "                             with no argument, follow the descriptor\n"
              "  -F                       the same as --follow=name --retry\n"
              "  -n, --lines=[+]NUM       the last NUM lines, not the last 10; with\n"
              "                             +NUM, everything from line NUM on\n"
              "      --max-unchanged-stats=N\n"
              "                           with --follow=name, look at the name again\n"
              "                             after N rounds (5 by default) in which the\n"
              "                             file did not change, in case it was renamed\n"
              "                             or removed (as a log is when rotated)\n"
              "      --pid=PID            with -f, stop once process PID has ended;\n"
              "                             may be given more than once\n"
              "  -q, --quiet, --silent    never print the headers naming the files\n"
              "      --retry              keep trying to open a file that cannot be opened\n"
              "  -s, --sleep-interval=N   with -f, wait about N seconds (1.0 by default)\n"
              "                             between looks at the files\n"
              "  -v, --verbose            always print the headers naming the files\n"
              "  -z, --zero-terminated    lines end in NUL, not newline\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n"
              "\n"
              "NUM may end in a multiplier: b 512, kB 1000, K 1024, MB 1000*1000,\n"
              "M 1024*1024, GB 1000*1000*1000, G 1024*1024*1024, and the same for\n"
              "T, P, E, Z, Y, R and Q; KiB is K, MiB is M, and so on.\n"
              "\n"
              "-f follows the open file: if the file is renamed, tail keeps reading\n"
              "it. To follow whatever file has the name -- a log that is rotated --\n"
              "use --follow=name, which notices the file being renamed, removed and\n"
              "created again.\n", su_name);
}

static int do_tail(int argc, char **argv) {
    enum { RETRY_OPT = 0x100, MAXSTATS_OPT, PID_OPT, PRESUME_OPT, NOINOTIFY_OPT, FOLLOW_OPT };
    static const struct su_option longs[] = {
        { "bytes", SU_REQ_ARG, 0, 'c' },
        { "follow", SU_OPT_ARG, 0, FOLLOW_OPT },
        { "lines", SU_REQ_ARG, 0, 'n' },
        { "max-unchanged-stats", SU_REQ_ARG, 0, MAXSTATS_OPT },
        { "-disable-inotify", SU_NO_ARG, 0, NOINOTIFY_OPT },
        { "pid", SU_REQ_ARG, 0, PID_OPT },
        { "-presume-input-pipe", SU_NO_ARG, 0, PRESUME_OPT },
        { "quiet", SU_NO_ARG, 0, 'q' },
        { "retry", SU_NO_ARG, 0, RETRY_OPT },
        { "silent", SU_NO_ARG, 0, 'q' },
        { "sleep-interval", SU_REQ_ARG, 0, 's' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    static const char *const follow_v[] = { "descriptor", "name", 0 };
    tx_begin(1);
    memset(&txt, 0, sizeof txt);
    txt.count_lines = 1;
    txt.n = 10;
    txt.delim = '\n';
    txt.first = 1;
    txt.interval_ns = TX_NS_PER_S;

    char **av = argv;
    int ac = argc;
    if (txt_obsolete(argc, argv)) {
        av = (char **)malloc(((size_t)argc + 1) * sizeof *av);
        if (!av) tx_error(1, ENOMEM, "memory exhausted");
        av[0] = argv[0];
        for (int i = 2; i <= argc; i++) av[i - 1] = argv[i];
        ac = argc - 1;
    }

    int c;
    while ((c = su_getopt_long(ac, av, "c:n:fFqs:vz0123456789", longs, 0)) != -1) {
        switch (c) {
        case 'c': txt_count(su_optarg, 0); break;
        case 'n': txt_count(su_optarg, 1); break;
        case 'f': txt.forever = 1; txt.follow_name = 0; break;
        case FOLLOW_OPT:
            txt.forever = 1;
            txt.follow_name = su_optarg ? tx_argmatch("--follow", su_optarg, follow_v) == 1 : 0;
            break;
        case 'F': txt.forever = 1; txt.follow_name = 1; txt.reopen = 1; break;
        case RETRY_OPT: txt.reopen = 1; break;
        case MAXSTATS_OPT: {
            uintmax_t v;
            if (tx_xstrtoumax(su_optarg, &v, "") == TX_NUM_INVALID)
                tx_error(1, 0, "invalid maximum number of unchanged stats between opens: %s",
                         tx_quote(su_optarg));
            break;      /* every round looks at the names here */
        }
        case PID_OPT: {
            const char *s = su_optarg, *p = s;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '+') p++;
            int over = 0;
            long long v = 0;
            if (!isdigit((unsigned char)*p)) tx_error(1, 0, "invalid PID: %s", tx_quote(s));
            for (; isdigit((unsigned char)*p); p++) {
                v = v * 10 + (*p - '0');
                if (v > INT_MAX) { over = 1; v = INT_MAX; }
            }
            if (*p) tx_error(1, 0, "invalid PID: %s", tx_quote(s));
            if (over) tx_error(1, EOVERFLOW, "invalid PID: %s", tx_quote(s));
            pid_t *np = (pid_t *)realloc(txt.pids, (size_t)(txt.npids + 1) * sizeof *np);
            if (!np) tx_error(1, ENOMEM, "memory exhausted");
            txt.pids = np;
            txt.pids[txt.npids++] = (pid_t)v;
            break;
        }
        case PRESUME_OPT: txt.presume_pipe = 1; break;
        case NOINOTIFY_OPT: txt.no_inotify = 1; break;
        case 'q': txt.headers = -1; break;
        case 'v': txt.headers = 1; break;
        case 'z': txt.delim = 0; break;
        case 's': {
            unsigned long long ns = 0;
            int neg = 0, isnan = 0;
            const char *end = su_optarg;
            if (tx_strtons(su_optarg, &ns, &neg, &isnan, &end) < 0 || *end || isnan ||
                (neg && ns != 0))
                tx_error(1, 0, "invalid number of seconds: %s", tx_quote(su_optarg));
            txt.interval_ns = ns;
            break;
        }
        case SU_HELP_OPT: txt_help(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default:
            /* a digit is the old form out of place: no "Try" line */
            if (c >= '0' && c <= '9') tx_error(1, 0, "option used in invalid context -- %c", c);
            tx_try_help(1);
        }
    }

    if (txt.reopen && !txt.forever)
        tx_error(0, 0, "warning: --retry ignored; --retry is useful only when following");
    else if (txt.reopen && !txt.follow_name)
        tx_error(0, 0, "warning: --retry only effective for the initial open");
    if (txt.npids && !txt.forever)
        tx_error(0, 0, "warning: PID ignored; --pid=PID is useful only when following");

    /* -n 0 / -c 0, not following: there is nothing to do */
    if (txt.n == 0 && !txt.forever && !txt.from_start) return 0;

    int nf = ac - su_optind;
    static char *dash[] = { (char *)"-" };
    char **names = nf ? av + su_optind : dash;
    if (!nf) nf = 1;
    if (txt.forever && txt.follow_name)
        for (int i = 0; i < nf; i++)
            if (!strcmp(names[i], "-")) tx_error(1, 0, "cannot follow %s by name", tx_quoteaf("-"));

    struct txt_file *fs = (struct txt_file *)calloc((size_t)nf, sizeof *fs);
    if (!fs) tx_error(1, ENOMEM, "memory exhausted");
    txt_show_headers = txt.headers > 0 || (txt.headers == 0 && nf > 1);
    int ok = 1;
    for (int i = 0; i < nf; i++) {
        fs[i].name = names[i];
        fs[i].disp = strcmp(names[i], "-") ? names[i] : "standard input";
        fs[i].fd = -1;
        ok &= txt_file(&fs[i]);
    }
    if (txt.forever) ok = txt_follow(fs, nf, ok);
    return ok ? 0 : 1;
}
