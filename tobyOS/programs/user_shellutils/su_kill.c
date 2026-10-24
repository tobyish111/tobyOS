/* su_kill.c -- kill, as procps-ng 4.0.4's /usr/bin/kill (Linux's /bin/kill
 * is procps', not coreutils'): logs/utilgate/staging/kill.sh.
 *
 *   kill [-SIGNAL | -s SIGNAL | --signal SIGNAL] [-q VALUE] PID...
 *   kill -l [SIGNAL], kill -L, kill -h, kill -V
 *
 * Everything below was measured by running procps' kill in WSL, every case
 * that could signal anything inside a private pid namespace of its own.
 *
 * THE -SIGNAL ARGUMENT. Before the options are read, the arguments are
 * searched -- all of them, past "--" and option arguments too -- for the
 * first one that is '-' and a signal; that one is the signal and is taken
 * out, and the rest are read as options. So `kill P -USR1` sends USR1,
 * `kill -q -5 P` sends 5 and queues P as the value (then has no PID: the
 * usage), `kill -- -1` is signal 1 with no PID (the usage), and `kill -9
 * -9 P` leaves the second -9 to the option reader. A signal here is a
 * name (below), or a number of 0..93, or RTMIN+N whose value is at most
 * 127 (its arithmetic wraps as a long's: RTMIN+9223372036854775774 is
 * accepted).
 *
 * SIGNAL NAMES (-SIGNAL and -s alike): case is ignored, "SIG" may lead,
 * and the names are Linux's -- IOT, CLD, IO and POLL included, EXIT is 0,
 * RTMIN is 34, RTMIN+N is 34+N (N as strtol reads it, not negative, the
 * sum a long's: past INT_MAX it is no signal, but one that wrapped
 * negative is cut to an int -- RTMIN+99999999999999999999 is 33); RTMAX
 * is not known. A number is strtol's (blanks and '+' allowed, all of
 * it digits after them). -s takes any spelling: what it cannot read
 * becomes -1, which the kernel refuses -- "(PID): Invalid argument", or
 * "No such process" when PID does not exist (the kernel looks for the
 * process first).
 *
 * THE OPTIONS are getopt_long's with its own messages off: an unknown
 * option, a missing argument or an argument to an option that takes none
 * is "invalid argument C" (C the option character -- a NUL byte for a long
 * option that has none) and the usage, on stderr, status 1. Except a
 * DIGIT: `kill -USR1 -2 P` (the scan took -USR1) reads the argument after
 * the digit's -- or, when the digit is not the last in its cluster, the
 * digit's own argument ("-21") -- as one PID, signals it and exits 1 if
 * that WORKED and 0 if it did not; with nothing there the PID is
 * "(null)". So `kill -9 -1` does not reach every process (no PID after
 * -1); `kill -9 -- -1` does.
 *   -l [SIG], --list[=SIG]: the names of 1..31, or SIG's number or name
 *   (SIG attached, or the next argument unless it starts with '-'); it
 *   knows only those 31 names (POLL, not IO), any case, "SIG" allowed;
 *   numbers are all digits, cut to an int; else "unknown signal name
 *   SIG", status 0 still. -L, --table: the table. -h, --help: the usage
 *   on stdout, status 0. -V, --version. Each acts the moment it is read.
 *   -q, --queue VALUE: sigqueue(3) with VALUE (strtol's, cut to an int).
 *
 * PIDs are strtol's, all of the argument, cut to a pid_t ("4294967298" is
 * pid 2), each signalled in turn: "(PID): strerror" for a failure (status
 * 1, the rest still tried); an argument that is not a number ends it at
 * once: "failed to parse argument: 'X'" (": Numerical result out of range"
 * when too big). The messages name argv[0] as it was typed.
 *
 * One deliberately kept oddity: `kill -l SIG9` (SIG and then a digit)
 * makes procps' kill free a pointer it did not allocate, and glibc aborts
 * it -- "free(): invalid pointer", SIGABRT. That is reproduced, so a
 * script sees the same status.
 *
 * The old kill knew 30 names in upper case only, no -q, -L, --list, the
 * scan, or procps' messages. */

/* the names -l and -L show, 1..31 */
static const char *const sy_ksig[32] = {
    0, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL",
    "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT",
    "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF",
    "WINCH", "POLL", "PWR", "SYS",
};

/* the other names a signal may be given by */
static const struct { const char *name; int num; } sy_ksig_alias[] = {
    { "IOT", 6 }, { "CLD", 17 }, { "IO", 29 }, { "EXIT", 0 },
};

#define SY_KRTMIN 34

static int sy_kcaseeq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == *b;
}

/* S less a leading "SIG" in any case */
static const char *sy_knosig(const char *s) {
    if (toupper((unsigned char)s[0]) == 'S' && toupper((unsigned char)s[1]) == 'I' &&
        toupper((unsigned char)s[2]) == 'G')
        return s + 3;
    return s;
}

/* strtol over all of S (blanks and a sign allowed first); 0 when it is
 * not a number. *ERANGE_ says it saturated. */
static int sy_kstrtol(const char *s, long *out, int *erange_) {
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (e == s || *e) return 0;
    if (erange_) *erange_ = errno == ERANGE;
    *out = v;
    return 1;
}

/* a signal by name: 1 and *NUM, or 0. LIMIT_RT (the scan) refuses an
 * RTMIN+N past 127. */
static int sy_kname(const char *s, long *num, int limit_rt) {
    s = sy_knosig(s);
    for (int i = 1; i < 32; i++)
        if (sy_kcaseeq(s, sy_ksig[i])) { *num = i; return 1; }
    for (size_t i = 0; i < sizeof sy_ksig_alias / sizeof sy_ksig_alias[0]; i++)
        if (sy_kcaseeq(s, sy_ksig_alias[i].name)) { *num = sy_ksig_alias[i].num; return 1; }
    if (strncasecmp(s, "RTMIN", 5) == 0) {
        if (!s[5]) { *num = SY_KRTMIN; return 1; }
        long n;
        if (s[5] != '+' || !sy_kstrtol(s + 6, &n, 0) || n < 0) return 0;
        /* the sum as a long's, wrapping; what is past an int is no
         * signal -- but a sum that wrapped negative passes both checks
         * and is cut to an int */
        long v = (long)((unsigned long)SY_KRTMIN + (unsigned long)n);
        if (limit_rt ? v > 127 : v > INT_MAX) return 0;
        *num = v;
        return 1;
    }
    return 0;
}

/* -s SIGNAL: the number to send; -1 when SIGNAL is none */
static int sy_ksig_s(const char *s) {
    long v;
    if (sy_kname(s, &v, 0)) return (int)v;
    if (sy_kstrtol(sy_knosig(s), &v, 0) && v >= 0 && v <= INT_MAX) return (int)v;
    return -1;
}

/* the scan's question: is "-S" a signal (and which) */
static int sy_ksig_scan(const char *s, int *out) {
    long v;
    if (sy_kname(s, &v, 1)) { *out = (int)v; return 1; }
    if (sy_kstrtol(sy_knosig(s), &v, 0) && v >= 0 && v <= 93) { *out = (int)v; return 1; }
    return 0;
}

static void sy_kusage(FILE *f, int status) __attribute__((noreturn));
static void sy_kusage(FILE *f, int status) {
    fflush(stdout);
    fprintf(f,
            "\n"
            "Usage:\n"
            " kill [options] <pid> [...]\n"
            "\n"
            "Options:\n"
            " <pid> [...]            each process to signal (a negative number is\n"
            "                        a process group, -1 every process you may signal)\n"
            " -<signal>, -s, --signal <signal>\n"
            "                        the signal to send, by name or number (TERM\n"
            "                        when none is given)\n"
            " -q, --queue <value>    send it with sigqueue(3), carrying this integer\n"
            " -l, --list=[<signal>]  name every signal, or turn one name into its\n"
            "                        number and one number into its name\n"
            " -L, --table            every signal with its number, as a table\n"
            "\n"
            " -h, --help     show this text and exit\n"
            " -V, --version  show the version and exit\n"
            "\n"
            "See kill(1).\n");
    exit(status);
}

/* "MSG: 'ARG'[: strerror]" and exit 1 -- or the number */
static long sy_kpid(const char *arg, const char *msg) {
    long v;
    int er = 0;
    if (!arg || !sy_kstrtol(arg, &v, &er) || er) {
        fflush(stdout);
        fprintf(stderr, "%s: %s: '%s'", su_name, msg, arg ? arg : "(null)");
        if (er) fprintf(stderr, ": %s", strerror(ERANGE));
        fputc('\n', stderr);
        exit(1);
    }
    return v;
}

static int sy_kqueue, sy_kvalue;

/* 0 sent, else errno */
static int sy_ksend(long pid, int sig) {
    int r;
    /* -q's VALUE travels with the signal on both builds: libtoby's
     * sigqueue is the kernel's (finding 215; it was kill, the value lost) */
    if (sy_kqueue) {
        union sigval sv;
        sv.sival_int = sy_kvalue;
        r = sigqueue((pid_t)pid, sig, sv);
    } else
        r = kill((pid_t)pid, sig);
    return r == 0 ? 0 : errno;
}

static void sy_klist_all(void) {
    /* the names in lines of at most 80 columns */
    int col = 0;
    for (int i = 1; i < 32; i++) {
        int l = (int)strlen(sy_ksig[i]);
        if (col && col + 1 + l > 80) { putchar('\n'); col = 0; }
        else if (col) { putchar(' '); col++; }
        fputs(sy_ksig[i], stdout);
        col += l;
    }
    putchar('\n');
}

static void sy_ktable(void) {
    for (int i = 1; i < 32; i++) {
        if (i % 7 == 0) printf("%2d %s\n", i, sy_ksig[i]);
        else printf("%2d %-8s", i, sy_ksig[i]);
    }
    if (31 % 7) putchar('\n');
}

/* -l SIG: its number or its name */
static void sy_klist_one(const char *s) {
    const char *p = s;
    int digits = *p != 0;
    for (; *p; p++) if (!isdigit((unsigned char)*p)) digits = 0;
    if (digits) {
        long v;
        if (sy_kstrtol(s, &v, 0)) {
            int n = (int)v;
            if (n >= 1 && n < 32) { printf("%s\n", sy_ksig[n]); return; }
        }
    } else {
        const char *n = sy_knosig(s);
        if (n != s && isdigit((unsigned char)*n)) {
            /* procps frees a pointer it never allocated here; glibc's
             * check of that free ends the process */
            fflush(stdout);
            fputs("free(): invalid pointer\n", stderr);
            abort();
        }
        for (int i = 1; i < 32; i++)
            if (sy_kcaseeq(n, sy_ksig[i])) { printf("%d\n", i); return; }
    }
    fflush(stdout);
    fprintf(stderr, "%s: unknown signal name %s\n", su_name, s);
}

static int do_kill(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "list", SU_OPT_ARG, 0, 'l' },
        { "table", SU_NO_ARG, 0, 'L' },
        { "signal", SU_REQ_ARG, 0, 's' },
        { "queue", SU_REQ_ARG, 0, 'q' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    if (argc < 2) sy_kusage(stderr, 1);
    int sig = SIGTERM;
    for (int i = 1; i < argc; i++) {
        int v;
        if (argv[i][0] == '-' && sy_ksig_scan(argv[i] + 1, &v)) {
            sig = v;
            for (int j = i; j < argc; j++) argv[j] = argv[j + 1];
            argc--;
            break;
        }
    }
    su_opterr = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "l::Ls:hVq:", longs, 0)) != -1) {
        switch (c) {
        case 'l': {
            const char *s = su_optarg;
            if (!s && su_optind < argc && argv[su_optind][0] != '-') s = argv[su_optind];
            if (s) sy_klist_one(s);
            else sy_klist_all();
            exit(0);
        }
        case 'L':
            sy_ktable();
            exit(0);
        case 's':
            sig = sy_ksig_s(su_optarg);
            break;
        case 'h':
            sy_kusage(stdout, 0);
        case 'V':
            exit(su_version());
        case 'q':
            sy_kvalue = (int)sy_kpid(su_optarg,
                                     "must be an integer value to be passed with the signal.");
            sy_kqueue = 1;
            break;
        default:
            if (isdigit(su_optopt)) {
                /* the digit's argument when more of it is left, else
                 * the next one: one PID, and the status turned round */
                const char *t = (su_cluster && *su_cluster) ? argv[su_nextind - 1]
                              : su_nextind < argc ? argv[su_nextind] : 0;
                long pid = sy_kpid(t, "failed to parse argument");
                exit(sy_ksend(pid, sig) == 0 ? 1 : 0);
            }
            fflush(stdout);
            fprintf(stderr, "%s: invalid argument ", su_name);
            fputc(su_optopt, stderr);
            fputc('\n', stderr);
            sy_kusage(stderr, 1);
        }
    }
    if (su_optind >= argc) sy_kusage(stderr, 1);
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        long pid = sy_kpid(argv[i], "failed to parse argument");
        int e = sy_ksend(pid, sig);
        if (e) {
            fflush(stdout);
            fprintf(stderr, "%s: (%ld): %s\n", su_name, pid, strerror(e));
            rc = 1;
        }
    }
    return rc;
}
