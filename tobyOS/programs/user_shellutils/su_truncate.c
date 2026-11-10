/* su_truncate.c -- truncate, as GNU coreutils': set each FILE's size,
 * creating it unless -c.
 *
 *   -s, --size=SIZE   a number with an optional suffix (K M G T P E Z Y R
 *                     Q, and g m t; KB is 1000, K and KiB 1024), after an
 *                     optional + (grow by), - (shrink by), < (at most), >
 *                     (at least), / (round down to a multiple), % (round
 *                     up to one)
 *   -r, --reference=RFILE  RFILE's size, or the base for a relative -s
 *   -o, --io-blocks   SIZE counts the file's I/O blocks, not bytes
 *   -c, --no-create   a missing FILE is skipped, quietly
 *
 * The messages are GNU's: "Invalid number: 'X'" (with ": Value too large
 * for data type" past the largest), "division by zero", "cannot open 'F'
 * for writing", and the usage errors with the Try line. Measured on
 * coreutils 9.7: logs/utilgate/truncate.sh.
 *
 * It took -s alone, absolute, K M G only (b as 512 too), and gave no
 * -c -o -r and no GNU message. */

enum { TR_SET, TR_ADD, TR_SUB, TR_MAX, TR_MIN, TR_RDN, TR_RUP };

/* SIZE into *N and *MODE; 0, or the errno to say with "Invalid number" */
static int tr_parse(const char *arg, intmax_t *n, int *mode) {
    const char *p = arg;
    while (isspace((unsigned char)*p)) p++;
    *mode = TR_SET;
    switch (*p) {
    case '+': *mode = TR_ADD; p++; break;
    case '-': *mode = TR_SUB; p++; break;
    case '<': *mode = TR_MAX; p++; break;
    case '>': *mode = TR_MIN; p++; break;
    case '/': *mode = TR_RDN; p++; break;
    case '%': *mode = TR_RUP; p++; break;
    }
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-' || *p == '+') return EINVAL;
    uintmax_t v;
    char *end;
    /* g m t are G M T here, as GNU's truncate takes them */
    char buf[128];
    snprintf(buf, sizeof buf, "%s", p);
    for (char *q = buf; *q; q++) {
        if (isdigit((unsigned char)*q)) continue;
        if (*q == 'g') *q = 'G';
        else if (*q == 'm') *q = 'M';
        else if (*q == 't') *q = 'T';
        break;
    }
    int r = su_strtoumax(buf, &end, &v, "EGkKMPQRTYZ0");
    if (r == SU_NUM_OVERFLOW || (r == SU_NUM_OK && v > (uintmax_t)INTMAX_MAX))
        return EOVERFLOW;
    if (r != SU_NUM_OK || end == buf) return EINVAL;
    *n = (intmax_t)v;
    return 0;
}

static int do_truncate(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "no-create", SU_NO_ARG, 0, 'c' },
        { "io-blocks", SU_NO_ARG, 0, 'o' },
        { "reference", SU_REQ_ARG, 0, 'r' },
        { "size", SU_REQ_ARG, 0, 's' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int nocreate = 0, blocks = 0, mode = TR_SET, have_size = 0, c;
    const char *ref = 0;
    intmax_t size = 0;
    while ((c = su_getopt_long(argc, argv, "cor:s:", longs, 0)) != -1) {
        switch (c) {
        case 'c': nocreate = 1; break;
        case 'o': blocks = 1; break;
        case 'r': ref = su_optarg; break;
        case 's': {
            int e = tr_parse(su_optarg, &size, &mode);
            if (e == EOVERFLOW)
                su_error(1, EOVERFLOW, "Invalid number: %s", su_quote(su_optarg));
            if (e) su_error(1, 0, "Invalid number: %s", su_quote(su_optarg));
            have_size = 1;
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s OPTION... FILE...\n"
                   "Make each FILE the size asked for, creating it if it does not\n"
                   "exist. A file made longer reads as zero bytes in what was added.\n"
                   "\n"
                   "  -c, --no-create        create no file; skip a missing one\n"
                   "  -o, --io-blocks        SIZE counts I/O blocks, not bytes\n"
                   "  -r, --reference=RFILE  the size RFILE has\n"
                   "  -s, --size=SIZE        the size, or with a leading + - < > / %%\n"
                   "                         a change to it: grow by, shrink by, at\n"
                   "                         most, at least, round down or up to a\n"
                   "                         multiple of\n"
                   "      --help             show this help and exit\n"
                   "      --version          show the version and exit\n"
                   "\n"
                   "SIZE is a whole number with an optional unit: K, M, G, T, P, E,\n"
                   "Z, Y, R, Q (powers of 1024), or KB, MB, ... (powers of 1000).\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (ref && have_size && mode == TR_SET) {
        su_error(0, 0, "you must specify a relative %s with %s",
                 su_quote("--size"), su_quote("--reference"));
        su_try_help(1);
    }
    if (!ref && !have_size) {
        su_error(0, 0, "you must specify either %s or %s",
                 su_quote("--size"), su_quote("--reference"));
        su_try_help(1);
    }
    if (blocks && !have_size) {
        su_error(0, 0, "%s was specified but %s was not",
                 su_quote("--io-blocks"), su_quote("--size"));
        su_try_help(1);
    }
    if (su_optind == argc) {
        su_error(0, 0, "missing file operand");
        su_try_help(1);
    }
    if ((mode == TR_RDN || mode == TR_RUP) && size == 0)
        su_error(1, 0, "division by zero");
    intmax_t rsize = -1;
    if (ref) {
        struct stat st;
        if (stat(ref, &st) != 0) su_error(1, errno, "cannot stat %s", su_quoteaf(ref));
        rsize = (intmax_t)st.st_size;
        if (!have_size) { size = rsize; mode = TR_SET; }
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        const char *f = argv[i];
        int fd = open(f, O_WRONLY | O_NONBLOCK | (nocreate ? 0 : O_CREAT), 0666);
        if (fd < 0) {
            if (nocreate && errno == ENOENT) continue;
            su_error(0, errno, "cannot open %s for writing", su_quoteaf(f));
            rc = 1;
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) != 0) {
            su_error(0, errno, "cannot fstat %s", su_quoteaf(f));
            close(fd);
            rc = 1;
            continue;
        }
        intmax_t n = size;
        if (blocks) {
            intmax_t bs = st.st_blksize > 0 ? (intmax_t)st.st_blksize : 512;
            if (n > INTMAX_MAX / bs) {
                su_error(0, 0, "overflow in %jd * %jd byte blocks for file %s",
                         n, bs, su_quoteaf(f));
                close(fd);
                rc = 1;
                continue;
            }
            n *= bs;
        }
        if (mode != TR_SET) {
            intmax_t fsize = rsize >= 0 ? rsize : (intmax_t)st.st_size;
            switch (mode) {
            case TR_ADD:
                if (fsize > INTMAX_MAX - n) {
                    su_error(0, 0, "overflow extending size of file %s", su_quoteaf(f));
                    close(fd);
                    rc = 1;
                    continue;
                }
                n = fsize + n;
                break;
            case TR_SUB: n = fsize - n; break;
            case TR_MAX: n = fsize < n ? fsize : n; break;
            case TR_MIN: n = fsize > n ? fsize : n; break;
            case TR_RDN: n = fsize / n * n; break;
            case TR_RUP: n = (fsize + n - 1) / n * n; break;
            }
            if (n < 0) n = 0;
        }
        if (ftruncate(fd, (off_t)n) != 0) {
            su_error(0, errno, "failed to truncate %s at %jd bytes", su_quoteaf(f), n);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
