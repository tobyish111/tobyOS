/* su_touch.c -- touch, as GNU coreutils 9.7's (logs/utilgate/touch.sh).
 *
 *   touch [OPTION]... FILE...
 *
 *   - Each FILE made if it is not there (but -c), and given now, or -d
 *     STRING (GNU's date strings, su_time.c), -t [[CC]YY]MMDDhhmm[.ss], or
 *     -r FILE's times -- with -r and -d, the string read against each of
 *     FILE's two times; -a the access time alone, -m the modification time
 *     alone (--time=atime|access|use|mtime|modify), -h a link itself, "-"
 *     standard output's file; -f taken, as GNU's.
 *   - GNU's messages: "cannot touch 'F'", "setting times of 'F'", "failed
 *     to get attributes of 'R'", "invalid date format 'S'", "cannot specify
 *     times from more than one source", "missing file operand", --time's
 *     argmatch list.
 *
 * Times go through utimensat (ABI_SYS_UTIMENSAT): signed seconds, one time
 * left as it is, a link's own. The disk keeps whole seconds. It took no -d
 * and no -t. */

/* [[CC]YY]MMDDhhmm[.ss] as local time */
static int tc_posix(const char *s, int64_t *out) {
    const char *dot = strchr(s, '.');
    size_t main = dot ? (size_t)(dot - s) : strlen(s);
    for (size_t i = 0; i < main; i++) if (!isdigit((unsigned char)s[i])) return 0;
    if (dot && (strlen(dot + 1) != 2 || !isdigit((unsigned char)dot[1]) || !isdigit((unsigned char)dot[2])))
        return 0;
    if (main != 8 && main != 10 && main != 12) return 0;
    int64_t now_s;
    long now_ns;
    tx_now(&now_s, &now_ns);
    struct tx_tm now;
    tx_tz_local(tx_tz_cur(), now_s, 0, &now);
    int64_t year = now.year;
    const char *p = s;
    if (main == 12) {
        year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        p += 4;
    } else if (main == 10) {
        int yy = (p[0] - '0') * 10 + (p[1] - '0');
        year = yy + (yy < 69 ? 2000 : 1900);
        p += 2;
    }
    int v[4];
    for (int i = 0; i < 4; i++) v[i] = (p[2 * i] - '0') * 10 + (p[2 * i + 1] - '0');
    int sec = dot ? (dot[1] - '0') * 10 + (dot[2] - '0') : 0;
    int mo = v[0], d = v[1], h = v[2], mi = v[3];
    if (mo < 1 || mo > 12 || d < 1 || d > tx_mdays(year, mo) || h > 23 || mi > 59 || sec > 60) return 0;
    int64_t local = tx_days_from_civil(year, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
    *out = tx_tz_mktime(tx_tz_cur(), local, -1);
    return 1;
}

static int do_touch(int argc, char **argv) {
    enum { TIME_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "time", SU_REQ_ARG, 0, TIME_OPT }, { "no-create", SU_NO_ARG, 0, 'c' },
        { "date", SU_REQ_ARG, 0, 'd' }, { "reference", SU_REQ_ARG, 0, 'r' },
        { "no-dereference", SU_NO_ARG, 0, 'h' }, SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int atime = 0, mtime = 0, no_create = 0, nofollow = 0, c;
    const char *datestr = 0, *ref = 0, *stamp = 0;
    while ((c = su_getopt_long(argc, argv, "acd:fhmr:t:", longs, 0)) != -1) {
        switch (c) {
        case 'a': atime = 1; break;
        case 'c': no_create = 1; break;
        case 'd': datestr = su_optarg; break;
        case 'f': break;
        case 'h': nofollow = 1; break;
        case 'm': mtime = 1; break;
        case 'r': ref = su_optarg; break;
        case 't': stamp = su_optarg; break;
        case TIME_OPT: {
            static const char *const names[] = { "atime", "access", "use", "mtime", "modify", 0 };
            /* argmatch: an exact name, or a prefix whose every match means
             * the same (a: atime, access) */
            int k = -1, mixed = 0;
            size_t len = strlen(su_optarg);
            for (int i = 0; names[i]; i++) {
                if (!strcmp(names[i], su_optarg)) { k = i; mixed = 0; break; }
                if (len && !strncmp(names[i], su_optarg, len)) {
                    if (k >= 0 && (k < 3) != (i < 3)) mixed = 1;
                    k = i;
                }
            }
            if (mixed) k = -1;
            if (k < 0) {
                fflush(stdout);
                fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n"
                        "  - 'atime', 'access', 'use'\n  - 'mtime', 'modify'\n",
                        su_short, su_quote(su_optarg), su_quote("--time"));
                su_try_help(1);
            }
            if (k < 3) atime = 1; else mtime = 1;
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE...\n"
                   "Give each FILE the current time (or another), making it if it is\n"
                   "not there.\n"
                   "\n"
                   "  -a                     only the access time\n"
                   "  -c, --no-create        make no file\n"
                   "  -d, --date=STRING      the time STRING describes, not now\n"
                   "  -f                     (taken, as GNU's: does nothing)\n"
                   "  -h, --no-dereference   a symbolic link itself\n"
                   "  -m                     only the modification time\n"
                   "  -r, --reference=FILE   FILE's times\n"
                   "  -t [[CC]YY]MMDDhhmm[.ss]  that time\n"
                   "      --time=WORD        atime, access, use; mtime, modify\n"
                   "      --help             show this help and exit\n"
                   "      --version          show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if ((datestr && stamp) || (ref && stamp)) {
        su_error(0, 0, "cannot specify times from more than one source");
        su_try_help(1);
    }
    if (su_optind >= argc) {
        su_error(0, 0, "missing file operand");
        su_try_help(1);
    }
    if (!atime && !mtime) atime = mtime = 1;
    struct timespec ts[2];
    int explicit_time = 0;
    ts[0].tv_nsec = ts[1].tv_nsec = UTIME_NOW;
    ts[0].tv_sec = ts[1].tv_sec = 0;
    if (ref) {
        struct stat st;
        int r = nofollow ? lstat(ref, &st) : stat(ref, &st);
        if (r != 0) su_error(1, errno, "failed to get attributes of %s", su_quoteaf(ref));
        ts[0] = st.st_atim;      /* to the nanosecond (finding 239) */
        ts[1] = st.st_mtim;
        explicit_time = 1;
        if (datestr) {
            for (int k = 0; k < 2; k++) {
                int64_t s;
                long ns;
                if (!tx_parse_date(datestr, (int64_t)ts[k].tv_sec, 0, &s, &ns))
                    su_error(1, 0, "invalid date format %s", su_quote(datestr));
                ts[k].tv_sec = (time_t)s;
                ts[k].tv_nsec = ns;
            }
        }
    } else if (datestr || stamp) {
        int64_t s;
        long ns = 0;
        if (datestr) {
            int64_t now_s;
            long now_ns;
            tx_now(&now_s, &now_ns);
            if (!tx_parse_date(datestr, now_s, now_ns, &s, &ns))
                su_error(1, 0, "invalid date format %s", su_quote(datestr));
        } else if (!tc_posix(stamp, &s))
            su_error(1, 0, "invalid date format %s", su_quote(stamp));
        ts[0].tv_sec = ts[1].tv_sec = (time_t)s;
        ts[0].tv_nsec = ts[1].tv_nsec = ns;
        explicit_time = 1;
    }
    if (!atime) ts[0].tv_nsec = UTIME_OMIT;
    if (!mtime) ts[1].tv_nsec = UTIME_OMIT;
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        const char *f = argv[i];
        char self[64];
        if (!strcmp(f, "-")) {
            /* standard output's own file, when it is one */
            ssize_t n = readlink("/proc/self/fd/1", self, sizeof self - 1);
            if (n <= 0 || self[0] != '/') continue;
            self[n] = 0;
            f = self;
        } else if (!no_create && !nofollow) {
            struct stat st;
            if (stat(f, &st) != 0) {
                int fd = open(f, O_WRONLY | O_CREAT, 0666);
                if (fd < 0) {
                    int e = errno;
                    if (e != EISDIR) {
                        su_error(0, e, "cannot touch %s", su_quoteaf(f));
                        rc = 1;
                        continue;
                    }
                } else
                    close(fd);
            }
        }
        if (utimensat(AT_FDCWD, f, (explicit_time || !atime || !mtime) ? ts : 0,
                      nofollow ? AT_SYMLINK_NOFOLLOW : 0) != 0) {
            int e = errno;
            if (no_create && e == ENOENT) continue;
            su_error(0, e, "%s %s", e == ENOENT ? "cannot touch" : "setting times of",
                     su_quoteaf(f));
            rc = 1;
        }
    }
    return rc;
}
