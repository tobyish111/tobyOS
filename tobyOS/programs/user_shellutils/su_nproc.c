/* su_nproc.c -- nproc, as GNU coreutils' (logs/utilgate/nproc.sh).
 *
 * The processors this process may use; --all, the processors installed.
 * Without --all OpenMP's variables count, as GNU's does: OMP_NUM_THREADS's
 * first element, when it is a positive integer, replaces the count, and
 * OMP_THREAD_LIMIT, when positive, caps it. --ignore=N then takes N away,
 * never below 1. A bad N is "invalid number: 'N'", status 1.
 *
 * tobyOS has no CPU affinity, so "may use" is every online processor --
 * /proc/cpuinfo's "processor" lines, which --all counts as well. The old
 * nproc took --ignore= alone (`--ignore 3` was an unrecognized option),
 * read N with atoi (`--ignore=-1` added one) and knew nothing of OpenMP. */

static long np_count(int all) {
#ifdef SU_HOST
    long n = sysconf(all ? _SC_NPROCESSORS_CONF : _SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
#else
    (void)all;
    FILE *f = fopen("/proc/cpuinfo", "r");
    long n = 0;
    char line[256];
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "processor", 9)) n++;
        fclose(f);
    }
    return n > 0 ? n : 1;
#endif
}

/* A positive decimal at the start of S, up to a ',' or the end (OpenMP's
 * syntax); 0 when it is not one. */
static unsigned long np_omp(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return 0;
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v > (ULONG_MAX - (unsigned long)(*s - '0')) / 10) return 0;
        v = v * 10 + (unsigned long)(*s - '0');
        s++;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (*s && *s != ',') return 0;
    return v;
}

static int do_nproc(int argc, char **argv) {
    enum { ALL = 1000, IGNORE };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, ALL }, { "ignore", SU_REQ_ARG, 0, IGNORE },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int c, all = 0;
    uintmax_t ignore = 0;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        switch (c) {
        case ALL: all = 1; break;
        case IGNORE: {
            /* past UINTMAX_MAX is the most there is to ignore, as GNU
             * takes it: the answer is 1, not an error */
            int r = su_strtoumax(su_optarg, 0, &ignore, "");
            if (r == SU_NUM_OVERFLOW) { ignore = UINTMAX_MAX; break; }
            if (r != SU_NUM_OK) su_error(1, 0, "invalid number: %s", su_quote(su_optarg));
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]...\n"
                   "Print how many processors this process may use.\n"
                   "\n"
                   "      --all       the processors installed, not the ones usable\n"
                   "      --ignore=N  take N away from the count, leaving at least 1\n"
                   "      --help      show this help and exit\n"
                   "      --version   show the version and exit\n"
                   "\n"
                   "Without --all, OMP_NUM_THREADS replaces the count and\n"
                   "OMP_THREAD_LIMIT caps it, as OpenMP programs read them.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind < argc) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
    unsigned long n = (unsigned long)np_count(all);
    if (!all) {
        unsigned long omp = np_omp(getenv("OMP_NUM_THREADS"));
        if (omp) n = omp;
        unsigned long lim = np_omp(getenv("OMP_THREAD_LIMIT"));
        if (lim && n > lim) n = lim;
    }
    n = ignore < n ? n - (unsigned long)ignore : 1;
    printf("%lu\n", n);
    return 0;
}
