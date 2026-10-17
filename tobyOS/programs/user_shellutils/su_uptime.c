/* su_uptime.c -- uptime, as procps-ng 4.0.4's (logs/utilgate/uptime.sh):
 *
 *   " 14:23:01 up 2 days,  3:04,  1 user,  load average: 0.00, 0.01, 0.05"
 *
 * the time; days if any, then hours:minutes (the hours right-aligned in
 * two) or "N min" under an hour; the users -- utmp's user processes, as
 * who counts them (su_who.c); /proc/loadavg's first three. -p, --pretty:
 * "up 1 week, 2 days, 3 hours, 4 minutes" (years, weeks, days, hours,
 * minutes, each when not 0); -s, --since: the boot instant, "%Y-%m-%d
 * %H:%M:%S" -- -s over -p. The uptime is /proc/uptime's. An operand, or an
 * option it does not know, is the usage on stderr and status 1. The old
 * uptime counted the desktop session alone as its users. */

static void ut_usage(FILE *f) {
    fprintf(f, "\nUsage:\n %s [options]\n\nOptions:\n"
               " -p, --pretty   the time up in words\n"
               " -h, --help     show this help and exit\n"
               " -s, --since    when the system came up\n"
               " -V, --version  show the version and exit\n"
               "\nuptime(1) has the rest.\n", su_short);
}

static int ut_users(void) {
    size_t n;
    struct who_ut *u = who_read(WHO_FILE, &n);
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        char user[40];
        who_str(user, sizeof user, u[i].ut_user, sizeof u[i].ut_user);
        if (u[i].ut_type == 7 && user[0]) k++;
    }
    free(u);
    return k;
}

static void ut_plural(char *out, size_t cap, long v, const char *what, int *first) {
    if (!v) return;
    size_t l = strlen(out);
    snprintf(out + l, cap - l, "%s%ld %s%s", *first ? "" : ", ", v, what, v == 1 ? "" : "s");
    *first = 0;
}

static int do_uptime(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "pretty", SU_NO_ARG, 0, 'p' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "since", SU_NO_ARG, 0, 's' },
        { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    int c, pretty = 0, since = 0;
    while ((c = su_getopt_long(argc, argv, "phsV", longs, 0)) != -1) {
        switch (c) {
        case 'p': pretty = 1; break;
        case 's': since = 1; break;
        case 'h': ut_usage(stdout); return 0;
        case 'V': printf("%s from tobyOS 1.0\n", su_short); return 0;
        default: ut_usage(stderr); return 1;
        }
    }
    if (su_optind < argc) { ut_usage(stderr); return 1; }

    long upsec = 0;
    {
        FILE *f = fopen("/proc/uptime", "r");
        char line[128];
        if (!f || !fgets(line, sizeof line, f)) su_error(1, errno, "/proc/uptime");
        fclose(f);
        upsec = strtol(line, 0, 10);
    }
    time_t now = time(0);
    if (since) {
        char *s = tx_strftime("%Y-%m-%d %H:%M:%S", (int64_t)now - upsec, 0, 0);
        puts(s ? s : "");
        free(s);
        return 0;
    }
    if (pretty) {
        char buf[256] = "";
        int first = 1;
        long m = upsec / 60;
        long years = m / (60L * 24 * 365);  m -= years * 60L * 24 * 365;
        long weeks = m / (60L * 24 * 7);    m -= weeks * 60L * 24 * 7;
        long days = m / (60L * 24);         m -= days * 60L * 24;
        long hours = m / 60;                m -= hours * 60;
        ut_plural(buf, sizeof buf, years, "year", &first);
        ut_plural(buf, sizeof buf, weeks, "week", &first);
        ut_plural(buf, sizeof buf, days, "day", &first);
        ut_plural(buf, sizeof buf, hours, "hour", &first);
        ut_plural(buf, sizeof buf, m, "minute", &first);
        printf("up %s\n", first ? "0 minutes" : buf);
        return 0;
    }
    char la[3][32] = { "0.00", "0.00", "0.00" };
    {
        FILE *f = fopen("/proc/loadavg", "r");
        char line[128];
        if (f && fgets(line, sizeof line, f)) {
            char *p = line;
            for (int i = 0; i < 3; i++) {
                while (*p == ' ') p++;
                char *e = p;
                while (*e && *e != ' ' && *e != '\n') e++;
                /* procps prints them %.2f: a value with more places is cut
                 * there, rounding (they come with two) */
                size_t l = (size_t)(e - p);
                if (l >= sizeof la[i]) l = sizeof la[i] - 1;
                memcpy(la[i], p, l);
                la[i][l] = 0;
                p = e;
            }
        }
        if (f) fclose(f);
    }
    char *clock = tx_strftime("%H:%M:%S", (int64_t)now, 0, 0);
    char upb[96] = "";
    long days = upsec / 86400, mins = upsec / 60 % 60, hours = upsec / 3600 % 24;
    size_t l = 0;
    if (days) l += (size_t)snprintf(upb + l, sizeof upb - l, "%ld day%s, ", days, days == 1 ? "" : "s");
    if (hours) snprintf(upb + l, sizeof upb - l, "%2ld:%02ld, ", hours, mins);
    else snprintf(upb + l, sizeof upb - l, "%ld min, ", mins);
    int users = ut_users();
    printf(" %s up %s%2d user%s,  load average: %s, %s, %s\n", clock ? clock : "", upb,
           users, users == 1 ? "" : "s", la[0], la[1], la[2]);
    free(clock);
    return 0;
}
