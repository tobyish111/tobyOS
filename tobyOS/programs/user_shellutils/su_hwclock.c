/* su_hwclock.c -- hwclock, as util-linux 2.41's (logs/utilgate/hwclock.sh,
 * logs/b28/hwclock-wsl-*.out).
 *
 *   hwclock [function] [option...]
 *
 * Functions -- one at most ("mutually exclusive arguments: ..."):
 *   -r, --show / --get   the RTC's time, as util-linux prints it: synchronized
 *                        to the RTC's tick, in local time with its offset,
 *                        "2026-10-15 08:06:59.989525-05:00" -- the time the RTC
 *                        said when hwclock started;
 *   -s, --hctosys        the system clock set from the RTC;
 *   -w, --systohc        the RTC set from the system clock, written as
 *                        util-linux writes it: second S at S + 0.5, the
 *                        RTC's next tick coming 500 ms after a write;
 *   --set --date STRING  the RTC set to STRING (GNU's date strings);
 *   --systz              no RTC access: accepted, nothing to send here.
 * Options: -u/--utc, -l/--localtime (the RTC's timescale; else /etc/adjtime's
 * third line, else UTC), -f/--rtc FILE, --date, --delay, --noadjfile,
 * --adjfile, --test (nothing written), -v/--verbose, -h/--help,
 * -V/--version. --adjust, --predict, --update-drift, --param-*, --vl-*,
 * --directisa are util-linux's drift tables and RTC parameters, which this
 * hwclock does not keep: refused.
 *
 * The RTC is /dev/rtc0 (then /dev/rtc), the kernel's (finding 157): RTC_RD_TIME
 * and RTC_SET_TIME, root's alone -- for anyone else util-linux's "Cannot access
 * the Hardware Clock via any known method." */

#define HW_RD_TIME  0x80247009ul
#define HW_SET_TIME 0x4024700aul
struct hw_rtc_time {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

static int hw_verbose;
static const char *hw_path = "/dev/rtc0";   /* the one opened */

static int hw_open(const char *file) {
    const char *names[] = { "/dev/rtc0", "/dev/rtc", "/dev/misc/rtc" };
    int n = file ? 1 : 3;
    for (int i = 0; i < n; i++) {
        const char *p = file ? file : names[i];
        if (hw_verbose) printf("Trying to open: %s\n", p);
        int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            if (hw_verbose) printf("Using the rtc interface to the clock.\n");
            hw_path = p;
            return fd;
        }
    }
    fprintf(stderr, "%s: Cannot access the Hardware Clock via any known method.\n", prog);
    fprintf(stderr, "%s: Use the --verbose option to see the details of our search "
                    "for an access method.\n", prog);
    return -1;
}

static int64_t hw_now_ns(void) {
    int64_t s;
    long ns;
    tx_now(&s, &ns);
    return s * 1000000000LL + ns;
}

/* the RTC's broken-down time as seconds since 1970, by its timescale */
static int64_t hw_to_epoch(const struct hw_rtc_time *t, int local) {
    int64_t days = tx_days_from_civil((int64_t)t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    int64_t s = days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
    return local ? tx_tz_mktime(tx_tz_cur(), s, -1) : s;
}

static void hw_from_epoch(int64_t s, int local, struct hw_rtc_time *t) {
    struct tx_tm tm;
    if (local) tx_tz_local(tx_tz_cur(), s, 0, &tm);
    else tx_breakdown(s, 0, &tm);
    memset(t, 0, sizeof *t);
    t->tm_year = (int)(tm.year - 1900);
    t->tm_mon = tm.mon - 1;
    t->tm_mday = tm.mday;
    t->tm_hour = tm.hour;
    t->tm_min = tm.min;
    t->tm_sec = tm.sec;
}

/* Read the RTC at its tick, as util-linux does without the update
 * interrupt: read until the second changes (1.5 s at most). *AT is the
 * system time at that tick; the RTC's time, in seconds since 1970. */
static int hw_read_tick(int fd, int local, int64_t *rtc_s, int64_t *at_ns) {
    struct hw_rtc_time a, b;
    if (hw_verbose) printf("Waiting for clock tick...\n");
    if (ioctl(fd, HW_RD_TIME, &a) != 0) return -1;
    int64_t start = hw_now_ns();
    for (;;) {
        if (ioctl(fd, HW_RD_TIME, &b) != 0) return -1;
        if (b.tm_sec != a.tm_sec) break;
        if (hw_now_ns() - start > 1500000000LL) break;   /* a stopped RTC */
    }
    *at_ns = hw_now_ns();
    *rtc_s = hw_to_epoch(&b, local);
    if (hw_verbose) {
        printf("...got clock tick\n");
        printf("Time read from Hardware Clock: %04d/%02d/%02d %02d:%02d:%02d\n",
               b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min, b.tm_sec);
        printf("Hw clock time : %04d/%02d/%02d %02d:%02d:%02d = %lld seconds since 1969\n",
               b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min, b.tm_sec,
               (long long)*rtc_s);
    }
    return 0;
}

/* util-linux's ISO form, local time and its offset, microseconds */
static void hw_print(int64_t ns) {
    int64_t s = ns / 1000000000LL;
    long frac = (long)(ns % 1000000000LL);
    if (frac < 0) { frac += 1000000000L; s--; }
    struct tx_tm tm;
    tx_tz_local(tx_tz_cur(), s, frac, &tm);
    long off = tm.gmtoff;
    char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    printf("%04lld-%02d-%02d %02d:%02d:%02d.%06ld%c%02ld:%02ld\n", (long long)tm.year,
           tm.mon, tm.mday, tm.hour, tm.min, tm.sec, frac / 1000, sign, off / 3600,
           off % 3600 / 60);
}

/* Set the RTC to the time TARGET (nanoseconds since 1970, as of the call)
 * represents. The kernel's write puts the RTC's next tick 500 ms after it
 * (the divider's reset, src/rtc.c), so second S goes in at S + 0.5 and
 * the RTC turns to S + 1 as the target does: sleep to 20 ms before the
 * half second, then spin to it; a write up to 10 ms late is taken (a busy
 * system preempts the spin, and a 2 ms window could be missed for seconds). */
static int hw_set(int fd, int local, int64_t target, int test) {
    int64_t delta = target - hw_now_ns();   /* what to add to the system clock */
    int64_t now;
    for (;;) {
        now = hw_now_ns() + delta;
        long frac = (long)(now % 1000000000LL);
        if (frac >= 500000000L && frac < 510000000L) break;
        long wait = frac < 500000000L ? 500000000L - frac : 1500000000L - frac;
        if (wait > 20000000L) {
            struct timespec d = { 0, wait - 20000000L };
            nanosleep(&d, NULL);
        }
    }
    int64_t sec = now / 1000000000LL;
    struct hw_rtc_time t;
    hw_from_epoch(sec, local, &t);
    if (test) {
        printf("Test mode: clock was not changed\n");
        return 0;
    }
    if (ioctl(fd, HW_SET_TIME, &t) != 0) {
        fprintf(stderr, "%s: ioctl(RTC_SET_TIME) to %s to set the time failed: %s\n",
                prog, hw_path, strerror(errno));
        return -1;
    }
    return 0;
}

static int hw_adjtime_local(const char *path) {
    FILE *f = fopen(path ? path : "/etc/adjtime", "r");
    if (!f) return 0;
    char line[128];
    int n = 0, local = 0;
    while (fgets(line, sizeof line, f)) {
        if (++n == 3) { local = !strncmp(line, "LOCAL", 5); break; }
    }
    fclose(f);
    return local;
}

static void hw_try(void) {
    fprintf(stderr, "Try '%s --help' for more information.\n", prog);
}

static void hw_help(void) {
    printf("\nUsage:\n %s [function] [option...]\n\n"
           "Read or set the hardware clock (the RTC).\n\n"
           "Functions:\n"
           " -r, --show            print the RTC's time, in local time\n"
           "     --get             the same\n"
           "     --set             set the RTC to the time --date gives\n"
           " -s, --hctosys         set the system clock from the RTC\n"
           " -w, --systohc         set the RTC from the system clock\n"
           "     --systz           nothing to do here; accepted\n\n"
           "Options:\n"
           " -u, --utc             the RTC keeps UTC (the default)\n"
           " -l, --localtime       the RTC keeps local time\n"
           " -f, --rtc <file>      the RTC device (default /dev/rtc0)\n"
           "     --date <time>     the time for --set\n"
           "     --delay <sec>     accepted; the write waits for the half second\n"
           "     --noadjfile       do not read /etc/adjtime\n"
           "     --adjfile <file>  read this file instead of /etc/adjtime\n"
           "     --test            change nothing; implies --verbose\n"
           " -v, --verbose         say more\n\n"
           " -h, --help            display this help\n"
           " -V, --version         display version\n", prog);
}

static int do_hwclock(int argc, char **argv) {
    enum { F_NONE, F_SHOW, F_GET, F_SET, F_HCTOSYS, F_SYSTOHC, F_SYSTZ } fn = F_NONE;
    int nfn = 0, utc = 0, localtime_ = 0, test = 0, noadj = 0;
    const char *rtcfile = 0, *datestr = 0, *adjfile = 0;
    int64_t start = hw_now_ns();
    enum { O_GET = 1000, O_SET, O_SYSTZ, O_DATE, O_DELAY, O_NOADJ, O_ADJFILE, O_TEST,
           O_ADJUST, O_PREDICT, O_UPDATE, O_PGET, O_PSET, O_PIDX, O_VLR, O_VLC, O_ISA };
    static const struct su_option longs[] = {
        { "show", SU_NO_ARG, 0, 'r' }, { "get", SU_NO_ARG, 0, O_GET },
        { "set", SU_NO_ARG, 0, O_SET }, { "hctosys", SU_NO_ARG, 0, 's' },
        { "systohc", SU_NO_ARG, 0, 'w' }, { "systz", SU_NO_ARG, 0, O_SYSTZ },
        { "adjust", SU_NO_ARG, 0, 'a' }, { "predict", SU_NO_ARG, 0, O_PREDICT },
        { "param-get", SU_REQ_ARG, 0, O_PGET }, { "param-set", SU_REQ_ARG, 0, O_PSET },
        { "param-index", SU_REQ_ARG, 0, O_PIDX }, { "vl-read", SU_NO_ARG, 0, O_VLR },
        { "vl-clear", SU_NO_ARG, 0, O_VLC },
        { "utc", SU_NO_ARG, 0, 'u' }, { "localtime", SU_NO_ARG, 0, 'l' },
        { "rtc", SU_REQ_ARG, 0, 'f' }, { "directisa", SU_NO_ARG, 0, O_ISA },
        { "date", SU_REQ_ARG, 0, O_DATE }, { "delay", SU_REQ_ARG, 0, O_DELAY },
        { "update-drift", SU_NO_ARG, 0, O_UPDATE }, { "noadjfile", SU_NO_ARG, 0, O_NOADJ },
        { "adjfile", SU_REQ_ARG, 0, O_ADJFILE }, { "test", SU_NO_ARG, 0, O_TEST },
        { "verbose", SU_NO_ARG, 0, 'v' }, { "help", SU_NO_ARG, 0, 'h' },
        { "version", SU_NO_ARG, 0, 'V' }, { 0, 0, 0, 0 }
    };
    int c, help = 0, version = 0;
    const char *unsupported = 0;
    while ((c = su_getopt_long(argc, argv, "rswaulf:vhV", longs, 0)) != -1) {
        switch (c) {
        case 'r':     fn = F_SHOW; nfn++; break;
        case O_GET:   fn = F_GET; nfn++; break;
        case O_SET:   fn = F_SET; nfn++; break;
        case 's':     fn = F_HCTOSYS; nfn++; break;
        case 'w':     fn = F_SYSTOHC; nfn++; break;
        case O_SYSTZ: fn = F_SYSTZ; nfn++; break;
        case 'a':     nfn++; unsupported = "--adjust"; break;
        case O_PREDICT: nfn++; unsupported = "--predict"; break;
        case O_PGET: case O_PSET: case O_PIDX: unsupported = "--param-*"; break;
        case O_VLR: case O_VLC: unsupported = "--vl-*"; break;
        case O_UPDATE: unsupported = "--update-drift"; break;
        case O_ISA:   unsupported = "--directisa"; break;
        case 'u':     utc = 1; break;
        case 'l':     localtime_ = 1; break;
        case 'f':     rtcfile = su_optarg; break;
        case O_DATE:  datestr = su_optarg; break;
        case O_DELAY: break;
        case O_NOADJ: noadj = 1; break;
        case O_ADJFILE: adjfile = su_optarg; break;
        case O_TEST:  test = 1; hw_verbose = 1; break;
        case 'v':     hw_verbose = 1; break;
        case 'h':     help = 1; break;
        case 'V':     version = 1; break;
        default:      hw_try(); return 1;
        }
    }
    if (help) { hw_help(); return 0; }
    if (version) { printf("%s (tobyOS) 1.0\n", prog); return 0; }
    if (su_optind < argc) {
        fprintf(stderr, "%s: too many arguments\n", prog);
        hw_try();
        return 1;
    }
    if (nfn > 1) {
        fprintf(stderr, "%s: mutually exclusive arguments: --adjust --show --hctosys "
                        "--systohc --get --predict --set --systz\n", prog);
        return 1;
    }
    if (utc && localtime_) {
        fprintf(stderr, "%s: mutually exclusive arguments: --localtime --utc\n", prog);
        return 1;
    }
    if (unsupported) {
        fprintf(stderr, "%s: %s: not supported here (no drift tables or RTC parameters)\n",
                prog, unsupported);
        return 1;
    }
    if (fn == F_NONE) fn = F_SHOW;
    int local = localtime_ ? 1 : utc ? 0 : (noadj ? 0 : hw_adjtime_local(adjfile));
    int64_t setto_s = 0;
    long setto_ns = 0;
    if (fn == F_SET) {
        if (!datestr) {
            fprintf(stderr, "%s: --date is required for --set or --predict\n", prog);
            return 1;
        }
        int64_t ns_s = start / 1000000000LL;
        if (!tx_parse_date(datestr, ns_s, (long)(start % 1000000000LL), &setto_s, &setto_ns)) {
            fprintf(stderr, "%s: invalid date '%s'\n", prog, datestr);
            return 1;
        }
    }
    if (hw_verbose) {
        printf("%s (tobyOS) 1.0\n", prog);
        printf("System Time: %lld.%06lld\n", (long long)(start / 1000000000LL),
               (long long)(start % 1000000000LL / 1000));
    }
    if (fn == F_SYSTZ) return 0;
    int fd = hw_open(rtcfile);
    if (fd < 0) return 1;
    if (hw_verbose) printf("Assuming hardware clock is kept in %s time.\n", local ? "local" : "UTC");
    int rc = 0;
    if (fn == F_SHOW || fn == F_GET) {
        int64_t rs, at;
        if (hw_read_tick(fd, local, &rs, &at) != 0) {
            fprintf(stderr, "%s: ioctl(RTC_RD_TIME) to %s to read the time failed: %s\n",
                    prog, hw_path, strerror(errno));
            rc = 1;
        } else {
            hw_print(rs * 1000000000LL + (start - at));
        }
    } else if (fn == F_HCTOSYS) {
        int64_t rs, at;
        if (hw_read_tick(fd, local, &rs, &at) != 0) {
            fprintf(stderr, "%s: ioctl(RTC_RD_TIME) to %s to read the time failed: %s\n",
                    prog, hw_path, strerror(errno));
            rc = 1;
        } else if (!test) {
            int64_t want = rs * 1000000000LL + (hw_now_ns() - at);
            struct timespec ts = { (time_t)(want / 1000000000LL), (long)(want % 1000000000LL) };
            if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
                fprintf(stderr, "%s: settimeofday() failed: %s\n", prog, strerror(errno));
                rc = 1;
            }
        } else {
            printf("Test mode: clock was not changed\n");
        }
    } else if (fn == F_SYSTOHC) {
        if (hw_set(fd, local, hw_now_ns(), test) != 0) rc = 1;
    } else if (fn == F_SET) {
        /* the time --date names, as of now: the wait for the half second
         * must not move it, so it goes as an offset from the system clock */
        int64_t target = setto_s * 1000000000LL + setto_ns + (hw_now_ns() - start);
        if (hw_set(fd, local, target, test) != 0) rc = 1;
    }
    close(fd);
    return rc;
}
