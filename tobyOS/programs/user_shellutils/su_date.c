/* su_date.c -- date, as GNU coreutils 9.7's (logs/utilgate/date.sh).
 *
 *   date [OPTION]... [+FORMAT]
 *   date [-u|--utc|--universal] [MMDDhhmm[[CC]YY][.ss]]
 *
 *   - What it prints: now, -d STRING (GNU's date strings, su_time.c), each
 *     line of -f FILE, -r FILE's modification time; with +FORMAT (every GNU
 *     conversion and flag, %N %:z %::z %:::z %q %P, E and O), -I[TIMESPEC],
 *     -R/--rfc-email, --rfc-3339=TIMESPEC, or "%a %b %e %H:%M:%S %Z %Y".
 *   - TZ as a POSIX string, -u for UTC (su_time.c).
 *   - -s STRING and MMDDhhmm[[CC]YY][.ss] ask to set the clock: there is no
 *     call here that sets it, so it is "cannot set date" (EPERM, as on
 *     Linux for anyone but root), the date still printed, status 1.
 *   - GNU's messages: "invalid date 'S'", "extra operand", "multiple output
 *     formats specified", the "lacks a leading '+'" paragraph, the argmatch
 *     lists for -I and --rfc-3339.
 *
 * It took -u, +FORMAT, -d with @EPOCH and two ISO forms, -r, -R and -I, had
 * no time zones, no %N, and refused -s. */

static int dt_argmatch(const char *arg, const char *const *names, const char *opt) {
    int hit = -1, n = 0;
    size_t len = strlen(arg);
    for (int i = 0; names[i]; i++) {
        if (!strcmp(arg, names[i])) return i;
        if (!strncmp(arg, names[i], len)) { hit = i; n++; }
    }
    if (n == 1) return hit;
    fflush(stdout);
    fprintf(stderr, "%s: %s argument %s for %s\nValid arguments are:\n", su_short,
            n > 1 ? "ambiguous" : "invalid", su_quote(arg), su_quote(opt));
    for (int i = 0; names[i]; i++) fprintf(stderr, "  - %s\n", su_quote(names[i]));
    su_try_help(1);
}

/* MMDDhhmm[[CC]YY][.ss] (POSIX date's set form) */
static int dt_posix(const char *s, int64_t now_s, int64_t *out) {
    size_t n = strlen(s);
    const char *dot = strchr(s, '.');
    size_t main = dot ? (size_t)(dot - s) : n;
    for (size_t i = 0; i < main; i++) if (!isdigit((unsigned char)s[i])) return 0;
    if (dot && (strlen(dot + 1) != 2 || !isdigit((unsigned char)dot[1]) || !isdigit((unsigned char)dot[2]))) return 0;
    if (main != 8 && main != 10 && main != 12) return 0;
    int v[6];
    for (int i = 0; i < 4; i++) v[i] = (s[2 * i] - '0') * 10 + (s[2 * i + 1] - '0');
    struct tx_tm now;
    tx_tz_local(tx_tz_cur(), now_s, 0, &now);
    int64_t year = now.year;
    if (main == 10) { int yy = (s[8] - '0') * 10 + (s[9] - '0'); year = yy + (yy < 69 ? 2000 : 1900); }
    if (main == 12) year = (s[8] - '0') * 1000 + (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    int sec = dot ? (dot[1] - '0') * 10 + (dot[2] - '0') : 0;
    int mo = v[0], d = v[1], h = v[2], mi = v[3];
    if (mo < 1 || mo > 12 || d < 1 || d > tx_mdays(year, mo) || h > 23 || mi > 59 || sec > 60) return 0;
    int64_t local = tx_days_from_civil(year, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
    *out = tx_tz_mktime(tx_tz_cur(), local, -1);
    return 1;
}

static int dt_show(const char *fmt, int64_t s, long ns, int utc) {
    char *o = tx_strftime(fmt, s, ns, utc);
    fputs(o, stdout);
    putchar('\n');
    free(o);
    return 0;
}

static int do_date(int argc, char **argv) {
    enum { RFC3339 = 0x100, DEBUG_OPT, RESOLUTION };
    static const struct su_option longs[] = {
        { "date", SU_REQ_ARG, 0, 'd' }, { "debug", SU_NO_ARG, 0, DEBUG_OPT },
        { "file", SU_REQ_ARG, 0, 'f' }, { "iso-8601", SU_OPT_ARG, 0, 'I' },
        { "reference", SU_REQ_ARG, 0, 'r' }, { "resolution", SU_NO_ARG, 0, RESOLUTION },
        { "rfc-email", SU_NO_ARG, 0, 'R' }, { "rfc-822", SU_NO_ARG, 0, 'R' },
        { "rfc-2822", SU_NO_ARG, 0, 'R' }, { "rfc-3339", SU_REQ_ARG, 0, RFC3339 },
        { "set", SU_REQ_ARG, 0, 's' }, { "uct", SU_NO_ARG, 0, 'u' },
        { "utc", SU_NO_ARG, 0, 'u' }, { "universal", SU_NO_ARG, 0, 'u' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    static const char *const iso_names[] = { "hours", "minutes", "date", "seconds", "ns", 0 };
    static const char *const iso_fmts[] = {
        "%Y-%m-%dT%H%:z", "%Y-%m-%dT%H:%M%:z", "%Y-%m-%d", "%Y-%m-%dT%H:%M:%S%:z",
        "%Y-%m-%dT%H:%M:%S,%N%:z" };
    static const char *const rfc_names[] = { "date", "seconds", "ns", 0 };
    static const char *const rfc_fmts[] = { "%Y-%m-%d", "%Y-%m-%d %H:%M:%S%:z", "%Y-%m-%d %H:%M:%S.%N%:z" };
    const char *datestr = 0, *batch = 0, *ref = 0, *setstr = 0, *fmt = 0;
    int utc = 0, resolution = 0, c;
    while ((c = su_getopt_long(argc, argv, "d:f:I::r:Rs:u", longs, 0)) != -1) {
        const char *newfmt = 0;
        switch (c) {
        case 'd': datestr = su_optarg; break;
        case DEBUG_OPT: break;
        case 'f': batch = su_optarg; break;
        case 'I': newfmt = iso_fmts[su_optarg ? dt_argmatch(su_optarg, iso_names, "--iso-8601") : 2]; break;
        case 'r': ref = su_optarg; break;
        case RESOLUTION: resolution = 1; break;
        case 'R': newfmt = "%a, %d %b %Y %H:%M:%S %z"; break;
        case RFC3339: newfmt = rfc_fmts[dt_argmatch(su_optarg, rfc_names, "--rfc-3339")]; break;
        case 's': setstr = su_optarg; break;
        case 'u': utc = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [+FORMAT]\n"
                   "  or:  %s [-u|--utc|--universal] [MMDDhhmm[[CC]YY][.ss]]\n"
                   "Show the date and time in FORMAT, or set the clock.\n"
                   "\n"
                   "  -d, --date=STRING          show the time STRING describes, not now\n"
                   "  -f, --file=DATEFILE        -d for each line of DATEFILE\n"
                   "  -I[FMT], --iso-8601[=FMT]  ISO 8601: date, hours, minutes, seconds, ns\n"
                   "  -R, --rfc-email            RFC 5322 (\"Mon, 14 Aug 2006 02:34:56 -0600\")\n"
                   "      --rfc-3339=FMT         RFC 3339: date, seconds or ns\n"
                   "  -r, --reference=FILE       FILE's last modification time\n"
                   "      --resolution           the clock's resolution\n"
                   "  -s, --set=STRING           set the time STRING describes\n"
                   "  -u, --utc, --universal     in UTC\n"
                   "      --help                 show this help and exit\n"
                   "      --version              show the version and exit\n"
                   "\n"
                   "FORMAT takes strftime's conversions: %%a %%A %%b %%B %%c %%C %%d %%D %%e %%F\n"
                   "%%g %%G %%h %%H %%I %%j %%k %%l %%m %%M %%n %%N %%p %%P %%q %%r %%R %%s %%S %%t %%T\n"
                   "%%u %%U %%V %%w %%W %%x %%X %%y %%Y %%z %%:z %%::z %%:::z %%Z, with the flags\n"
                   "- _ 0 ^ # and a width.\n",
                   su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
        if (newfmt) {
            if (fmt) su_error(1, 0, "multiple output formats specified");
            fmt = newfmt;
        }
    }
    int sources = !!datestr + !!batch + !!ref;
    if (sources > 1) {
        su_error(0, 0, "the options to specify dates for printing are mutually exclusive");
        su_try_help(1);
    }
    if (setstr && sources) {
        su_error(0, 0, "the options to print and set the time may not be used together");
        su_try_help(1);
    }
    const char *posix_set = 0;
    if (su_optind < argc) {
        const char *op = argv[su_optind];
        if (op[0] == '+') {
            if (fmt) su_error(1, 0, "multiple output formats specified");
            fmt = op + 1;
            if (su_optind + 1 < argc) {
                su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 1]));
                su_try_help(1);
            }
        } else if (sources || setstr || resolution) {
            fflush(stdout);
            fprintf(stderr, "%s: the argument %s lacks a leading '+';\n"
                    "when using an option to specify date(s), any non-option\n"
                    "argument must be a format string beginning with '+'\n", su_short, su_quote(op));
            su_try_help(1);
        } else {
            posix_set = op;
            if (su_optind + 1 < argc) {
                if (argv[su_optind + 1][0] == '+') {
                    if (fmt) su_error(1, 0, "multiple output formats specified");
                    fmt = argv[su_optind + 1] + 1;
                }
                if (su_optind + 2 < argc || argv[su_optind + 1][0] != '+') {
                    su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + (argv[su_optind + 1][0] == '+' ? 2 : 1)]));
                    su_try_help(1);
                }
            }
        }
    }
    if (!fmt) fmt = "%a %b %e %H:%M:%S %Z %Y";
    if (utc) {
        setenv("TZ", "UTC0", 1);
        tx_tz_loaded = 0;
    }
    if (resolution) { puts("0.000000001"); return 0; }
    int64_t now_s;
    long now_ns;
    tx_now(&now_s, &now_ns);
    int64_t s = now_s;
    long ns = now_ns;
    if (batch) {
        FILE *f = !strcmp(batch, "-") ? stdin : fopen(batch, "r");
        if (!f) su_error(1, errno, "%s", su_quotef(batch));
        char *line = 0;
        size_t cap = 0;
        ssize_t n;
        int rc = 0;
        while ((n = getline(&line, &cap, f)) > 0) {
            if (line[n - 1] == '\n') line[--n] = 0;
            if (!tx_parse_date(line, now_s, now_ns, &s, &ns)) {
                su_error(0, 0, "invalid date %s", su_quote(line));
                rc = 1;
                continue;
            }
            dt_show(fmt, s, ns, utc);
        }
        free(line);
        if (f != stdin) fclose(f);
        return rc;
    }
    if (ref) {
        struct stat st;
        if (stat(ref, &st) != 0) su_error(1, errno, "%s", su_quotef(ref));
        s = (int64_t)st.st_mtime;
        ns = st.st_mtim.tv_nsec;
    } else if (datestr) {
        if (!tx_parse_date(datestr, now_s, now_ns, &s, &ns))
            su_error(1, 0, "invalid date %s", su_quote(datestr));
    }
    int rc = 0;
    if (setstr || posix_set) {
        if (setstr) {
            if (!tx_parse_date(setstr, now_s, now_ns, &s, &ns))
                su_error(1, 0, "invalid date %s", su_quote(setstr));
        } else {
            if (!dt_posix(posix_set, now_s, &s)) su_error(1, 0, "invalid date %s", su_quote(posix_set));
            ns = 0;
        }
        /* GNU's settime: the clock is set (clock_settime, root only), and
         * either way the date is printed; a refusal is "cannot set date:
         * ERROR" and status 1. (Until finding 157 there was no call that
         * set the clock, and root was refused too.) */
        struct timespec ts = { (time_t)s, (long)ns };
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            su_error(0, errno, "cannot set date");
            rc = 1;
        }
    }
    dt_show(fmt, s, ns, utc);
    return rc;
}
