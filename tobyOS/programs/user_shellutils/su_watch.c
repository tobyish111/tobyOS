/* su_watch.c -- watch, as procps-ng 4.0.4's (logs/utilgate/watch.sh).
 *
 *   watch [options] command
 *
 *   - Runs COMMAND (through `sh -c` with its words joined by spaces, or
 *     with -x as they are) every -n SECONDS (WATCH_INTERVAL, else 2; at
 *     least 0.1), its stdout and stderr shown full-screen under the title
 *     "Every 2.0s: COMMAND" and "HOST: DATE" (-t: no title).
 *   - The screen as procps's curses paints one: the alternate screen,
 *     each row placed with the cursor -- no newlines -- and only the rows
 *     that changed written again; -w cuts long lines instead of wrapping
 *     them, -d marks what changed since the last run (=permanent: since
 *     the first), -c lets the command's colour sequences through.
 *   - -g stops (status 0) when the output changes, -q N when it has not
 *     changed for N runs, -e when the command fails (a message, a key, and
 *     status 8); -b rings the bell when it fails; -p keeps to the interval
 *     however long a run takes; -r does not rerun on a resize. SIGINT,
 *     SIGTERM and SIGHUP end it with status 0, the terminal put back.
 *   - procps's messages: "failed to parse argument: 'X': Invalid argument",
 *     usage on stderr (status 1) for no command or a bad option.
 *
 * It printed the output again after a "clear" every interval, and ran
 * once and stopped when stdout was not a terminal. */

struct wt_opts {
    long double interval;
    int title, wrap, color, diff, diff_perm, beep, errexit, chgexit, precise, rerun, exec;
    long equexit;
};

static volatile int wt_stop, wt_resized;
static void wt_on_stop(int s) { (void)s; wt_stop = 1; }
static void wt_on_winch(int s) { (void)s; wt_resized = 1; }

static int wt_rows = 24, wt_cols = 80;
static char **wt_prev;          /* the screen as last written, a row each */
static unsigned char **wt_mark; /* -d=permanent: what has ever changed */

static void wt_size(void) {
    struct { unsigned short row, col, xp, yp; } ws;
    int r = 0, c = 0;
    if (ioctl(1, 0x5413 /* TIOCGWINSZ */, &ws) == 0) { r = ws.row; c = ws.col; }
    const char *e = getenv("LINES");
    if (e && atoi(e) > 0) r = atoi(e);
    e = getenv("COLUMNS");
    if (e && atoi(e) > 0) c = atoi(e);
    wt_rows = r > 0 ? r : 24;
    wt_cols = c > 0 ? c : 80;
}

static void wt_usage(FILE *out) {
    fprintf(out, "\nUsage:\n %s [options] command\n\nOptions:\n"
            "  -b, --beep             ring the bell when the command fails\n"
            "  -c, --color            show the command's colours and styles\n"
            "  -C, --no-color         show them as text\n"
            "  -d, --differences[=permanent]\n"
            "                         mark what changed since the last run\n"
            "  -e, --errexit          stop when the command fails\n"
            "  -g, --chgexit          stop when the output changes\n"
            "  -q, --equexit <cycles> stop when it has not changed for CYCLES runs\n"
            "  -n, --interval <secs>  run every SECS seconds (2)\n"
            "  -p, --precise          keep to the interval exactly\n"
            "  -r, --no-rerun         do not run again when the window resizes\n"
            "  -t, --no-title         no title line\n"
            "  -w, --no-wrap          cut long lines, do not wrap them\n"
            "  -x, --exec             run the command itself, not with sh -c\n"
            "\n"
            " -h, --help     show this help and exit\n"
            " -v, --version  show the version and exit\n", su_name);
}

static long double wt_seconds(const char *s) {
    char *end;
    errno = 0;
    long double v = su_strtold(s, &end);
    if (end == s || *end || errno) su_error(1, EINVAL, "failed to parse argument: %s", su_quote(s));
    return v;
}

/* run the command; its output (stdout and stderr) in *OUT, its status */
static int wt_run(char **cmd, const char *joined, int exec, char **out, size_t *len) {
    int p[2];
    *out = 0;
    *len = 0;
    if (pipe(p) != 0) su_error(7, errno, "unable to create IPC pipes");
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) su_error(2, errno, "unable to fork process");
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1);
        dup2(p[1], 2);
        if (p[1] > 2) close(p[1]);
        if (exec) {
            execvp(cmd[0], cmd);
            fprintf(stderr, "%s: unable to execute %s: %s\n", su_short, su_quote(cmd[0]), strerror(errno));
            _exit(4);
        }
        char *av[] = { (char *)"sh", (char *)"-c", (char *)joined, 0 };
        execvp("sh", av);
        _exit(4);
    }
    close(p[1]);
    size_t cap = 0;
    char buf[4096];
    for (;;) {
        ssize_t r = read(p[0], buf, sizeof buf);
        if (r < 0 && errno == EINTR) { if (wt_stop) break; continue; }
        if (r <= 0) break;
        if (*len + (size_t)r + 1 > cap) {
            cap = (cap + (size_t)r + 1) * 2;
            *out = (char *)realloc(*out, cap);
            if (!*out) su_error(1, ENOMEM, "memory exhausted");
        }
        memcpy(*out + *len, buf, (size_t)r);
        *len += (size_t)r;
    }
    close(p[0]);
    if (!*out) *out = (char *)calloc(1, 1);
    else (*out)[*len] = 0;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    return st;
}

/* the screen: the rows the command's output fills, as text */
static char **wt_layout(const struct wt_opts *o, const char *joined, const char *text, size_t len) {
    char **rows = (char **)calloc((size_t)wt_rows, sizeof *rows);
    for (int r = 0; r < wt_rows; r++) {
        rows[r] = (char *)malloc((size_t)wt_cols * 8 + 1);
        rows[r][0] = 0;
    }
    int r0 = 0;
    if (o->title) {
        char left[512], right[512], when[64], host[256] = "";
        struct sn_sb b = { 0, 0, 0 };
        struct su_fmtspec fs = { 0, 0, 0, 0, 0, -1, 1, 'f' };
        su_ldfmt(&b, (long double)o->interval, &fs);
        snprintf(left, sizeof left, "Every %ss: %s", b.s ? b.s : "2.0", joined);
        free(b.s);
        FILE *f = fopen("/proc/sys/kernel/hostname", "r");
        if (f) {
            if (fgets(host, sizeof host, f)) host[strcspn(host, "\n")] = 0;
            fclose(f);
        }
        time_t now = time(0);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(when, sizeof when, "%a %b %e %H:%M:%S %Y", &tm);
        snprintf(right, sizeof right, "%s: %s", host, when);
        int rl = (int)strlen(right), ll = (int)strlen(left);
        if (rl >= wt_cols) rl = 0;
        if (ll > wt_cols - rl - (rl ? 1 : 0)) ll = wt_cols - rl - (rl ? 1 : 0);
        if (ll < 0) ll = 0;
        snprintf(rows[0], (size_t)wt_cols * 8 + 1, "%.*s%*s%s", ll, left,
                 rl ? wt_cols - ll - rl : 0, "", rl ? right : "");
        r0 = 2;
    }
    int r = r0, col = 0;
    char *line = r < wt_rows ? rows[r] : 0;
    size_t lp = 0;
    for (size_t i = 0; i < len && r < wt_rows; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            line[lp] = 0;
            r++;
            col = 0;
            lp = 0;
            line = r < wt_rows ? rows[r] : 0;
            continue;
        }
        if (c == '\033' && o->color) {          /* -c: a whole sequence through */
            size_t j = i + 1;
            if (j < len && text[j] == '[') {
                j++;
                while (j < len && !(text[j] >= 0x40 && text[j] <= 0x7e)) j++;
            }
            for (; i <= j && i < len; i++) line[lp++] = text[i];
            i--;
            continue;
        }
        char cell[4];
        int w = 1;
        if (c == '\t') {
            int n = 8 - col % 8;
            for (int k = 0; k < n && col < wt_cols; k++) { line[lp++] = ' '; col++; }
            continue;
        } else if (c < 32 || c == 127) {
            cell[0] = '^';
            cell[1] = (char)(c == 127 ? '?' : c + 64);
            w = 2;
        } else
            cell[0] = (char)c;
        for (int k = 0; k < w; k++) {
            if (col >= wt_cols) {
                if (!o->wrap) break;
                line[lp] = 0;
                r++;
                col = 0;
                lp = 0;
                if (r >= wt_rows) break;
                line = rows[r];
            }
            line[lp++] = cell[k];
            col++;
        }
    }
    if (r < wt_rows && line) line[lp] = 0;
    return rows;
}

/* write the rows that differ from what is on the screen */
static void wt_paint(const struct wt_opts *o, char **rows, int first) {
    if (first) fputs("\033[H\033[2J", stdout);
    for (int r = 0; r < wt_rows; r++) {
        const char *was = wt_prev ? wt_prev[r] : 0;
        if (!first && was && !strcmp(was, rows[r])) continue;
        printf("\033[%d;1H", r + 1);
        if (o->diff && was && !first && !(o->title && r == 0)) {
            /* -d: what differs from the last run, in reverse video */
            size_t n = strlen(rows[r]), m = strlen(was);
            for (size_t k = 0; k < n; k++) {
                int changed = k >= m || was[k] != rows[r][k];
                if (o->diff_perm) {
                    if (changed) wt_mark[r][k < 1023 ? k : 1023] = 1;
                    changed = wt_mark[r][k < 1023 ? k : 1023];
                }
                if (changed) printf("\033[7m%c\033[27m", rows[r][k]);
                else putchar(rows[r][k]);
            }
        } else
            fputs(rows[r], stdout);
        fputs("\033[K", stdout);
    }
    fflush(stdout);
    if (wt_prev) {
        for (int r = 0; r < wt_rows; r++) free(wt_prev[r]);
        free(wt_prev);
    }
    wt_prev = rows;
}

static void wt_end(void) {
    printf("\033[%d;1H\033[?1049l", wt_rows);
    fflush(stdout);
}

static int do_watch(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "beep", SU_NO_ARG, 0, 'b' }, { "color", SU_NO_ARG, 0, 'c' },
        { "no-color", SU_NO_ARG, 0, 'C' }, { "differences", SU_OPT_ARG, 0, 'd' },
        { "errexit", SU_NO_ARG, 0, 'e' }, { "chgexit", SU_NO_ARG, 0, 'g' },
        { "equexit", SU_REQ_ARG, 0, 'q' }, { "interval", SU_REQ_ARG, 0, 'n' },
        { "precise", SU_NO_ARG, 0, 'p' }, { "no-rerun", SU_NO_ARG, 0, 'r' },
        { "no-title", SU_NO_ARG, 0, 't' }, { "no-wrap", SU_NO_ARG, 0, 'w' },
        { "exec", SU_NO_ARG, 0, 'x' }, { "help", SU_NO_ARG, 0, 'h' },
        { "version", SU_NO_ARG, 0, 'v' }, { 0, 0, 0, 0 }
    };
    struct wt_opts o;
    memset(&o, 0, sizeof o);
    o.title = o.wrap = o.rerun = 1;
    o.interval = 2;
    const char *env = getenv("WATCH_INTERVAL");
    if (env && *env) o.interval = wt_seconds(env);
    int c;
    while ((c = su_getopt_long(argc, argv, "+bcCd::egq:n:prtwxhv", longs, 0)) != -1) {
        switch (c) {
        case 'b': o.beep = 1; break;
        case 'c': o.color = 1; break;
        case 'C': o.color = 0; break;
        case 'd':
            o.diff = 1;
            if (su_optarg && !strcmp(su_optarg, "permanent")) o.diff_perm = 1;
            break;
        case 'e': o.errexit = 1; break;
        case 'g': o.chgexit = 1; break;
        case 'q': {
            char *end;
            long v = strtol(su_optarg, &end, 10);
            if (end == su_optarg || *end) su_error(1, EINVAL, "failed to parse argument: %s", su_quote(su_optarg));
            o.equexit = v;
            break;
        }
        case 'n': o.interval = wt_seconds(su_optarg); break;
        case 'p': o.precise = 1; break;
        case 'r': o.rerun = 0; break;
        case 't': o.title = 0; break;
        case 'w': o.wrap = 0; break;
        case 'x': o.exec = 1; break;
        case 'h': wt_usage(stdout); return 0;
        case 'v': return su_version();
        default: wt_usage(stderr); return 1;
        }
    }
    if (su_optind >= argc) { wt_usage(stderr); return 1; }
    if (o.interval < 0.1) o.interval = 0.1;
    if (o.interval > 4294967295.0) o.interval = 4294967295.0;
    char **cmd = argv + su_optind;
    size_t jl = 1;
    for (char **a = cmd; *a; a++) jl += strlen(*a) + 1;
    char *joined = (char *)malloc(jl);
    joined[0] = 0;
    for (char **a = cmd; *a; a++) {
        if (a != cmd) strcat(joined, " ");
        strcat(joined, *a);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = wt_on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
    if (o.rerun) {
        sa.sa_handler = wt_on_winch;
        sigaction(28 /* SIGWINCH */, &sa, 0);
    }
    wt_size();
    wt_mark = (unsigned char **)calloc((size_t)wt_rows, sizeof *wt_mark);
    for (int r = 0; r < wt_rows; r++) wt_mark[r] = (unsigned char *)calloc(1024, 1);
    fputs("\033[?1049h", stdout);
    char *last = 0;
    size_t last_len = 0;
    long same = 0;
    int first = 1, status = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (long round = 0; !wt_stop; round++) {
        char *out;
        size_t len;
        int st = wt_run(cmd, joined, o.exec, &out, &len);
        if (wt_stop) { free(out); break; }
        if (wt_resized) {
            wt_resized = 0;
            wt_size();
            first = 1;
        }
        int failed = !WIFEXITED(st) || WEXITSTATUS(st) != 0;
        char **rows = wt_layout(&o, joined, out, len);
        wt_paint(&o, rows, first);
        first = 0;
        int changed = last && (len != last_len || memcmp(out, last, len));
        if (last && !changed) same++;
        else same = 0;
        free(last);
        last = out;
        last_len = len;
        if (failed && o.beep) { putchar('\a'); fflush(stdout); }
        if (failed && o.errexit) {
            printf("\033[%d;1H%s\033[K", wt_rows, "command exit with a non-zero status, press a key to exit");
            fflush(stdout);
            char k;
            struct termios t0, t1;
            int tty = tcgetattr(0, &t0) == 0;
            if (tty) {
                t1 = t0;
                t1.c_lflag &= (tcflag_t)~(ICANON | ECHO);
                tcsetattr(0, 0, &t1);
            }
            ssize_t rr = read(0, &k, 1);
            (void)rr;
            if (tty) tcsetattr(0, 0, &t0);
            status = 8;
            break;
        }
        if (o.chgexit && changed) break;
        if (o.equexit > 0 && same >= o.equexit) break;
        /* the wait: to the next interval mark with -p, else the interval */
        double wait = o.interval;
        if (o.precise) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double el = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            wait = (double)(round + 1) * o.interval - el;
            if (wait < 0) wait = 0;
        }
        struct timespec ts;
        ts.tv_sec = (time_t)wait;
        ts.tv_nsec = (long)((wait - (double)ts.tv_sec) * 1e9);
        while (!wt_stop && !wt_resized && nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
    }
    wt_end();
    return status;
}
