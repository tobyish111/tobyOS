/* su_who.c -- who and users, as GNU coreutils' (logs/utilgate/who.sh): the
 * records of a utmp file -- /var/run/utmp, the kernel's (src/utmp.c), or
 * FILE -- as Linux x86-64 lays them out, 384 bytes each.
 *
 * who prints the users logged in (USER_PROCESS records with a name); -b
 * the boot record, -r the run level, -l login processes, -p processes init
 * spawned, -d dead ones, -t clock changes, -a all of those with -T and -u;
 * -q only the names and a count; -m (or two operands, "am i") only the
 * line on stdin; -H a heading. A line is
 *   NAME(8) [MESG] LINE(12) TIME [IDLE(6)] [PID(10)] COMMENT(8) [EXIT]
 * each after one space, trailing blanks dropped -- PID unless it is the
 * plain listing, IDLE with -u -l -r -d, MESG (+ - or ?, by the line's
 * group-write bit) with -T, EXIT with -d. TIME is "%b %e %H:%M"; IDLE "."
 * for under a minute, "hh:mm", "old" past a day, "?" without the line.
 *
 * users prints the names, sorted, on one line. A file that does not exist
 * is no one logged in. The old who and users asked the desktop session
 * alone and never saw an SSH login. */

#define WHO_FILE "/var/run/utmp"

struct who_ut {
    int16_t ut_type;
    int16_t pad0;
    int32_t ut_pid;
    char    ut_line[32];
    char    ut_id[4];
    char    ut_user[32];
    char    ut_host[256];
    int16_t e_termination, e_exit;
    int32_t ut_session;
    int32_t tv_sec, tv_usec;
    int32_t ut_addr_v6[4];
    char    unused[20];
};

static struct who_ut *who_read(const char *file, size_t *n) {
    *n = 0;
    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        su_error(1, errno, "%s", su_quotef(file));
    }
    size_t cap = 0, len = 0;
    char *buf = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) su_error(1, ENOMEM, "%s", su_quotef(file));
            buf = nb;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) { int e = errno; close(fd); su_error(1, e, "%s", su_quotef(file)); }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    *n = len / sizeof(struct who_ut);
    return (struct who_ut *)buf;
}

/* a field that may fill its array with no NUL */
static void who_str(char *out, size_t cap, const char *f, size_t n) {
    size_t l = 0;
    while (l < n && f[l]) l++;
    if (l >= cap) l = cap - 1;
    memcpy(out, f, l);
    out[l] = 0;
}

static struct {
    int boot, dead, login, initspawn, runlevel, clockchange, users;
    int mesg, idle, exitcol, heading, shortlist, short_out, mine;
} wo;

static char who_myline[64];
static time_t who_now;

static void who_time(char *out, size_t cap, int64_t t) {
    char *s = tx_strftime("%b %e %H:%M", t, 0, 0);
    snprintf(out, cap, "%s", s ? s : "");
    free(s);
}

static void who_line(const char *user, char state, const char *line, const char *when,
                     const char *idle, const char *pid, const char *comment, const char *exitstr) {
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%-8s", user);
    if (wo.mesg) n += snprintf(buf + n, sizeof buf - (size_t)n, " %c", state);
    n += snprintf(buf + n, sizeof buf - (size_t)n, " %-12s %-12s", line, when);
    if (wo.idle) n += snprintf(buf + n, sizeof buf - (size_t)n, " %-6s", idle);
    if (!wo.short_out) n += snprintf(buf + n, sizeof buf - (size_t)n, " %10s", pid);
    n += snprintf(buf + n, sizeof buf - (size_t)n, " %-8s", comment);
    if (wo.exitcol) n += snprintf(buf + n, sizeof buf - (size_t)n, " %-12s", exitstr);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = 0;
    puts(buf);
}

/* the line's mesg state and idle time, from /dev/LINE */
static void who_tty(const char *line, char *state, char *idle, size_t icap) {
    char path[64];
    struct stat st;
    snprintf(path, sizeof path, "/dev/%s", line);
    if (!line[0] || stat(path, &st) != 0) {
        *state = '?';
        snprintf(idle, icap, "  ?");
        return;
    }
    *state = (st.st_mode & S_IWGRP) ? '+' : '-';
    long d = (long)(who_now - st.st_atime);
    if (d < 60) snprintf(idle, icap, "  .  ");
    else if (d < 24 * 3600) snprintf(idle, icap, "%02ld:%02ld", d / 3600, d % 3600 / 60);
    else snprintf(idle, icap, " old ");
}

static void who_record(const struct who_ut *u) {
    char user[40], line[40], id[8], host[264], when[64], pid[16], comment[300], ex[40], idle[16];
    char state = ' ';
    who_str(user, sizeof user, u->ut_user, sizeof u->ut_user);
    who_str(line, sizeof line, u->ut_line, sizeof u->ut_line);
    who_str(id, sizeof id, u->ut_id, sizeof u->ut_id);
    who_str(host, sizeof host, u->ut_host, sizeof u->ut_host);
    who_time(when, sizeof when, u->tv_sec);
    snprintf(pid, sizeof pid, "%d", (int)u->ut_pid);
    idle[0] = 0;
    switch (u->ut_type) {
    case 7:     /* USER_PROCESS */
        if (!wo.users || !user[0]) return;
        if (wo.mine && strcmp(line, who_myline)) return;
        who_tty(line, &state, idle, sizeof idle);
        if (host[0]) snprintf(comment, sizeof comment, "(%s)", host);
        else comment[0] = 0;
        who_line(user, state, line, when, idle, pid, comment, "");
        return;
    case 2:     /* BOOT_TIME */
        if (!wo.boot) return;
        who_line("", ' ', "system boot", when, "", "", "", "");
        return;
    case 1: {   /* RUN_LVL */
        if (!wo.runlevel) return;
        int cur = u->ut_pid % 256, last = u->ut_pid / 256;
        char lv[40];
        snprintf(lv, sizeof lv, "run-level %c", cur);
        snprintf(comment, sizeof comment, "last=%c", last == 'N' ? 'S' : last);
        who_line("", ' ', lv, when, "", "", comment, "");
        return;
    }
    case 6:     /* LOGIN_PROCESS */
        if (!wo.login) return;
        snprintf(comment, sizeof comment, "id=%s", id);
        who_line("LOGIN", ' ', line, when, "", pid, comment, "");
        return;
    case 5:     /* INIT_PROCESS */
        if (!wo.initspawn) return;
        snprintf(comment, sizeof comment, "id=%s", id);
        who_line("", ' ', line, when, "", pid, comment, "");
        return;
    case 8:     /* DEAD_PROCESS */
        if (!wo.dead) return;
        snprintf(comment, sizeof comment, "id=%s", id);
        snprintf(ex, sizeof ex, "term=%d exit=%d", u->e_termination, u->e_exit);
        who_line("", ' ', line, when, "", pid, comment, ex);
        return;
    case 3:     /* NEW_TIME */
        if (!wo.clockchange) return;
        who_line("", ' ', "clock change", when, "", "", "", "");
        return;
    }
}

static int do_who(int argc, char **argv) {
    enum { LOOKUP_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "boot", SU_NO_ARG, 0, 'b' },
        { "count", SU_NO_ARG, 0, 'q' },
        { "dead", SU_NO_ARG, 0, 'd' },
        { "heading", SU_NO_ARG, 0, 'H' },
        { "login", SU_NO_ARG, 0, 'l' },
        { "lookup", SU_NO_ARG, 0, LOOKUP_OPT },
        { "message", SU_NO_ARG, 0, 'T' },
        { "mesg", SU_NO_ARG, 0, 'T' },
        { "process", SU_NO_ARG, 0, 'p' },
        { "runlevel", SU_NO_ARG, 0, 'r' },
        { "short", SU_NO_ARG, 0, 's' },
        { "time", SU_NO_ARG, 0, 't' },
        { "users", SU_NO_ARG, 0, 'u' },
        { "writable", SU_NO_ARG, 0, 'T' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&wo, 0, sizeof wo);
    int c, assumptions = 1;
    while ((c = su_getopt_long(argc, argv, "abdlmpqrstuwHT", longs, 0)) != -1) {
        switch (c) {
        case 'a':
            wo.boot = wo.dead = wo.login = wo.initspawn = wo.runlevel = wo.clockchange = 1;
            wo.users = wo.mesg = wo.idle = wo.exitcol = 1;
            assumptions = 0;
            break;
        case 'b': wo.boot = 1; assumptions = 0; break;
        case 'd': wo.dead = 1; wo.idle = 1; wo.exitcol = 1; assumptions = 0; break;
        case 'H': wo.heading = 1; break;
        case 'l': wo.login = 1; wo.idle = 1; assumptions = 0; break;
        case 'm': wo.mine = 1; break;
        case 'p': wo.initspawn = 1; assumptions = 0; break;
        case 'q': wo.shortlist = 1; break;
        case 'r': wo.runlevel = 1; wo.idle = 1; assumptions = 0; break;
        case 's': wo.short_out = 1; break;
        case 't': wo.clockchange = 1; assumptions = 0; break;
        case 'T': case 'w': wo.mesg = 1; break;
        case 'u': wo.users = 1; wo.idle = 1; assumptions = 0; break;
        case LOOKUP_OPT: break;     /* no resolver to canonicalize with */
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [ FILE | ARG1 ARG2 ]\n"
                   "Show who is logged in.\n"
                   "\n"
                   "  -a, --all         the same as -b -d --login -p -r -t -T -u\n"
                   "  -b, --boot        when the system last booted\n"
                   "  -d, --dead        processes that have died\n"
                   "  -H, --heading     a line of column headings first\n"
                   "  -l, --login       system login processes\n"
                   "      --lookup      canonicalize host names through DNS (not done here)\n"
                   "  -m                only the user on standard input's terminal\n"
                   "  -p, --process     active processes init started\n"
                   "  -q, --count       the login names, and how many are logged in\n"
                   "  -r, --runlevel    the current run level\n"
                   "  -s, --short       name, line and time only (the default)\n"
                   "  -t, --time        the last change of the system clock\n"
                   "  -T, -w, --mesg    each user's message status: + - or ?\n"
                   "  -u, --users       the users logged in, with idle time and pid\n"
                   "      --message     the same as -T\n"
                   "      --writable    the same as -T\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "With no FILE, %s is read; /var/log/wtmp is a common FILE.\n"
                   "Two operands (\"am i\", \"mom likes\") mean -m.\n", su_name, WHO_FILE);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (assumptions) { wo.users = 1; wo.short_out = 1; }
    const char *file = WHO_FILE;
    int nop = argc - su_optind;
    if (nop == 1) file = argv[su_optind];
    else if (nop == 2) wo.mine = 1;
    else if (nop > 2) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 2]));
        su_try_help(1);
    }
    who_now = time(0);
    if (wo.mine) {
        char t[64];
        who_myline[0] = 0;
        if (isatty(0) && tty_name(t, sizeof t) == 0 && !strncmp(t, "/dev/", 5))
            snprintf(who_myline, sizeof who_myline, "%s", t + 5);
        else wo.mine = 2;            /* no terminal: no line is ours */
    }
    size_t n;
    struct who_ut *u = who_read(file, &n);
    if (wo.shortlist) {
        int count = 0;
        for (size_t i = 0; i < n; i++) {
            char user[40];
            who_str(user, sizeof user, u[i].ut_user, sizeof u[i].ut_user);
            if (u[i].ut_type != 7 || !user[0]) continue;
            printf("%s%s", count ? " " : "", user);
            count++;
        }
        printf("\n# users=%d\n", count);
        free(u);
        return 0;
    }
    if (wo.heading) who_line("NAME", ' ', "LINE", "TIME", "IDLE", "PID", "COMMENT", "EXIT");
    if (wo.mine != 2)
        for (size_t i = 0; i < n; i++) who_record(&u[i]);
    free(u);
    return 0;
}

static int who_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int do_users(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        switch (c) {
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]\n"
                   "Write the names of the users logged in, by FILE's records\n"
                   "(%s when there is no FILE; /var/log/wtmp is a common FILE).\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name, WHO_FILE);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int nop = argc - su_optind;
    if (nop > 1) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 1]));
        su_try_help(1);
    }
    size_t n;
    struct who_ut *u = who_read(nop ? argv[su_optind] : WHO_FILE, &n);
    char **names = (char **)malloc((n ? n : 1) * sizeof *names);
    char (*store)[40] = (char (*)[40])malloc((n ? n : 1) * 40);
    if (!names || !store) su_error(1, ENOMEM, "memory exhausted");
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        who_str(store[k], 40, u[i].ut_user, sizeof u[i].ut_user);
        if (u[i].ut_type != 7 || !store[k][0]) continue;
        names[k] = store[k];
        k++;
    }
    qsort(names, k, sizeof *names, who_cmp);
    for (size_t i = 0; i < k; i++) printf("%s%s", i ? " " : "", names[i]);
    if (k) putchar('\n');
    free(names);
    free(store);
    free(u);
    return 0;
}
