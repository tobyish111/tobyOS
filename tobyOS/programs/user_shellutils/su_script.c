/* su_script.c -- script, as util-linux 2.41's: a command (or $SHELL) run on
 * a fresh pseudoterminal, everything it writes copied to standard output and
 * to a typescript file.
 *
 *   script [-a] [-c COMMAND] [-e] [-E auto|always|never] [-f] [-q] [FILE]
 *
 *   - The child is `$SHELL -c COMMAND` (or `$SHELL -i`; /bin/sh when SHELL
 *     is unset), leading a session of its own whose terminal is the pty.
 *   - Standard input is copied to the pty; when it ends (and it is not a
 *     terminal) the pty's end-of-file character follows, as util-linux
 *     sends it. A terminal on standard input gives the pty its settings
 *     and size and is put in raw mode meanwhile; otherwise the pty is
 *     0 rows by 0 columns, a Linux pty's own size.
 *   - FILE (default "typescript", -a to append) gets util-linux's
 *     "Script started on DATE [...]" and "Script done on DATE
 *     [COMMAND_EXIT_CODE="N"]" lines around the output; without -q,
 *     standard output gets "Script started, output log file is 'FILE'."
 *     and "Script done.".
 *   - Status 0, or with -e the command's (128 + N for signal N).
 *
 * The gates run a command on a terminal with `script -qc CMD /dev/null`
 * (logs/utilgate/sttypty.sh). The timing logs (-t, -T, -B, -I, -O, -m) are
 * not here. */

#include <pthread.h>
#ifdef SU_HOST
int grantpt(int);
int unlockpt(int);
char *ptsname(int);
#else
extern pid_t toby_spawn_flags(const char *path, char *const argv[], char *const envp[],
                              int fd0, int fd1, int fd2,
                              const struct abi_spawn_fd *extra, unsigned nextra,
                              unsigned flags);
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0          /* tobyOS: opening a terminal never takes it as one's own */
#endif

static int sc_mfd = -1;
static int sc_stdin_tty;

/* SIGTERM, SIGINT, SIGHUP, SIGQUIT end the session, as util-linux's */
static volatile int sc_stop;
static void sc_on_signal(int sig) { (void)sig; sc_stop = 1; }

static int sc_write_all(int fd, const char *b, size_t n) {
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* THE END OF A SESSION, as util-linux's script has it:
 *   - standard input goes to the pty while the command lives; what comes
 *     after it has exited is dropped;
 *   - when standard input ends (and is not a terminal), a ^D follows: once
 *     the command has read what is waiting (a quarter second at a time, two
 *     seconds at most -- then regardless), and 10 ms later. Always ^D, so a
 *     command that has put the pty in raw mode, or made another character
 *     its end of file, sees (and echoes) the byte;
 *   - script ends when it has no child left -- the command, and anything
 *     else it is the parent of: bash runs `script ... < <(cmd)`'s cmd in
 *     script's own process, before the exec -- 10 ms after, without
 *     waiting for standard input, unless the ^D is already on its way. */
static int sc_peek = -1;             /* a slave descriptor: to look with, and
                                      * to keep the pty open to the end */
static pthread_mutex_t sc_lock = PTHREAD_MUTEX_INITIALIZER;
static int sc_phase;                 /* 0 input, 1 sending the ^D, 2 done */
static volatile int sc_child_done;
static pid_t sc_child;
static int sc_status;

/* the pty's end: the master reads to the end once its last slave is gone */
static void sc_finish(int from_phase) {
    pthread_mutex_lock(&sc_lock);
    if (sc_phase == from_phase) {
        if (sc_peek >= 0) close(sc_peek);
        sc_peek = -1;
        sc_phase = 2;
    }
    pthread_mutex_unlock(&sc_lock);
}

static void *sc_input(void *arg) {
    (void)arg;
    char buf[4096];
    for (;;) {
        ssize_t r = read(0, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (sc_child_done) continue;
        if (sc_write_all(sc_mfd, buf, (size_t)r) != 0) break;
    }
    pthread_mutex_lock(&sc_lock);
    int mine = sc_phase == 0 && sc_peek >= 0;
    if (mine) sc_phase = 1;
    pthread_mutex_unlock(&sc_lock);
    if (!mine) return 0;
    for (int tries = 0; tries < 8; tries++) {
        int waiting = 0;
        if (ioctl(sc_peek, 0x541B /* FIONREAD */, &waiting) != 0 || waiting <= 0) {
            usleep(10000);
            if (ioctl(sc_peek, 0x541B, &waiting) != 0 || waiting <= 0) break;
        }
        usleep(250000);
    }
    char eof = 4;
    (void)sc_write_all(sc_mfd, &eof, 1);
    sc_finish(1);
    return 0;
}

/* every child reaped, the command's status kept; none left: the end */
static void *sc_waiter(void *arg) {
    (void)arg;
    for (;;) {
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p == sc_child) {
            sc_status = st;
            sc_child_done = 1;
        }
    }
    usleep(10000);
    sc_finish(0);
    return 0;
}

/* "2026-10-13 09:44:37-05:00" */
static void sc_now(char *out, size_t cap) {
    int64_t s;
    long ns;
    tx_now(&s, &ns);
    char *t = tx_strftime("%Y-%m-%d %H:%M:%S%:z", s, 0, 0);
    snprintf(out, cap, "%s", t ? t : "");
    free(t);
}

static int do_script(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "append", SU_NO_ARG, 0, 'a' }, { "command", SU_REQ_ARG, 0, 'c' },
        { "echo", SU_REQ_ARG, 0, 'E' }, { "return", SU_NO_ARG, 0, 'e' },
        { "flush", SU_NO_ARG, 0, 'f' }, { "quiet", SU_NO_ARG, 0, 'q' },
        { "help", SU_NO_ARG, 0, 'h' }, { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    int append = 0, ret = 0, quiet = 0, c;
    const char *cmd = 0;
    while ((c = su_getopt_long(argc, argv, "+ac:eE:fqhV", longs, 0)) != -1) {
        switch (c) {
        case 'a': append = 1; break;
        case 'c': cmd = su_optarg; break;
        case 'e': ret = 1; break;
        case 'E':
            if (strcmp(su_optarg, "auto") && strcmp(su_optarg, "always") &&
                strcmp(su_optarg, "never"))
                su_error(1, 0, "unsupported echo mode: '%s'", su_optarg);
            break;
        case 'f': break;                      /* every write goes out at once */
        case 'q': quiet = 1; break;
        case 'h':
            printf("\nUsage:\n %s [options] [file]\n\n"
                   "Run a command (or a shell) on a new terminal, copying all it\n"
                   "writes to standard output and to a file.\n\n"
                   "Options:\n"
                   " -a, --append            add to the file, do not replace it\n"
                   " -c, --command <command> run the command, not a shell\n"
                   " -E, --echo <when>       echo input: auto, always or never\n"
                   " -e, --return            exit with the command's status\n"
                   " -f, --flush             write the file as output arrives\n"
                   " -q, --quiet             no start and done messages\n"
                   " -h, --help              show this help\n"
                   " -V, --version           show the version\n",
                   su_short);
            return 0;
        case 'V': return su_version();
        default: su_try_help(1);
        }
    }
    int nops = argc - su_optind;
    if (nops > 1) {
        su_error(0, 0, "unexpected number of arguments");
        su_try_help(1);
    }
    const char *file = nops ? argv[su_optind] : "typescript";
    int tsfd = open(file, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666);
    if (tsfd < 0) su_error(1, errno, "cannot open %s", file);

    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";
    sc_stdin_tty = isatty(0);

    /* the pty */
    int mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (mfd < 0) su_error(1, errno, "failed to create pseudo-terminal");
    int sfd = -1;
#ifdef SU_HOST
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0) su_error(1, errno, "failed to create pseudo-terminal");
    const char *spath = ptsname(mfd);
    if (spath) sfd = open(spath, O_RDWR | O_NOCTTY);
#else
    {
        int unlock = 0, idx = -1;
        (void)ioctl(mfd, 0x40045431UL /* TIOCSPTLCK */, &unlock);
        if (ioctl(mfd, 0x80045430UL /* TIOCGPTN */, &idx) == 0 && idx >= 0) {
            char spath[32];
            snprintf(spath, sizeof spath, "/dev/pts/%d", idx);
            sfd = open(spath, O_RDWR | O_NOCTTY);
        }
    }
#endif
    if (sfd < 0) su_error(1, errno, "failed to create pseudo-terminal");
    sc_mfd = mfd;

    struct termios orig;
    struct { unsigned short row, col, xp, yp; } ws = { 0, 0, 0, 0 };
    if (sc_stdin_tty && tcgetattr(0, &orig) == 0) {
        (void)tcsetattr(sfd, TCSANOW, &orig);
        (void)ioctl(0, 0x5413 /* TIOCGWINSZ */, &ws);
    } else {
        sc_stdin_tty = 0;
        sc_peek = dup(sfd);
    }
    (void)ioctl(sfd, 0x5414 /* TIOCSWINSZ */, &ws);

    /* the typescript's header, and the message */
    char when[64];
    sc_now(when, sizeof when);
    {
        char hdr[1024];
        int n;
        if (sc_stdin_tty) {
            const char *term = getenv("TERM");
            char tnb[64];
            ssize_t tl = readlink("/proc/self/fd/0", tnb, sizeof tnb - 1);
            const char *tn = tl > 0 ? (tnb[tl] = 0, tnb) : 0;
            n = snprintf(hdr, sizeof hdr, "Script started on %s [%s%s%sTERM=\"%s\" TTY=\"%s\" COLUMNS=\"%u\" LINES=\"%u\"]\n",
                         when, cmd ? "COMMAND=\"" : "", cmd ? cmd : "", cmd ? "\" " : "",
                         term ? term : "unknown", tn ? tn : "unknown", ws.col, ws.row);
        } else
            n = snprintf(hdr, sizeof hdr, "Script started on %s [%s%s%s<not executed on terminal>]\n",
                         when, cmd ? "COMMAND=\"" : "", cmd ? cmd : "", cmd ? "\" " : "");
        if (n > 0) (void)sc_write_all(tsfd, hdr, (size_t)n < sizeof hdr ? (size_t)n : sizeof hdr - 1);
    }
    if (!quiet) {
        printf("Script started, output log file is '%s'.\n", file);
        fflush(stdout);
    }

    /* the child */
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    char *cargv[4];
    cargv[0] = (char *)base;
    cargv[1] = (char *)(cmd ? "-c" : "-i");
    cargv[2] = (char *)cmd;
    cargv[3] = 0;
    pid_t child;
#ifdef SU_HOST
    child = fork();
    if (child == 0) {
        close(mfd);
        setsid();
        (void)ioctl(sfd, TIOCSCTTY, 0);
        dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
        if (sfd > 2) close(sfd);
        execv(shell, cargv);
        fprintf(stderr, "%s: failed to execute %s: %s\n", su_short, shell, strerror(errno));
        _exit(127);
    }
#else
    child = toby_spawn_flags(shell, cargv, environ, sfd, sfd, sfd, 0, 0, ABI_SPAWN_F_SETSID);
#endif
    int spawn_errno = errno;
    close(sfd);
    struct termios raw;
    if (sc_stdin_tty) {
        raw = orig;
        raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        raw.c_oflag &= (tcflag_t)~OPOST;
        raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        raw.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
        raw.c_cflag |= CS8;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        (void)tcsetattr(0, TCSANOW, &raw);
    }
    int status = 0;
    if (child < 0) {
        char m[512];
        int n = snprintf(m, sizeof m, "%s: failed to execute %s: %s\r\n", su_short, shell,
                         strerror(spawn_errno));
        (void)sc_write_all(1, m, (size_t)n);
        (void)sc_write_all(tsfd, m, (size_t)n);
        status = 127 << 8;
    } else {
        static const int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT };
        for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = sc_on_signal;     /* no SA_RESTART: the read ends */
            sigaction(sigs[i], &sa, 0);
        }
        sc_child = child;
        pthread_t wt, th;
        int waiting = pthread_create(&wt, 0, sc_waiter, 0) == 0;
        if (pthread_create(&th, 0, sc_input, 0) == 0) pthread_detach(th);
        char buf[4096];
        while (!sc_stop) {
            ssize_t r = read(mfd, buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;                 /* the terminal's last writer left */
            (void)sc_write_all(1, buf, (size_t)r);
            (void)sc_write_all(tsfd, buf, (size_t)r);
        }
        if (sc_stop) {
            /* the command gets SIGTERM, and SIGKILL if it is still there
             * two seconds later */
            fflush(stdout);
            fputs("\nSession terminated, killing shell...", stderr);
            kill(child, SIGTERM);
            for (int i = 0; i < 40 && !sc_child_done; i++) usleep(50000);
            if (!sc_child_done) {
                kill(child, SIGKILL);
                for (int i = 0; i < 100 && !sc_child_done; i++) usleep(20000);
            }
            fputs(" ...killed.\n", stderr);
            status = 0;
            ret = 0;
        } else {
            if (waiting) pthread_join(wt, 0);
            status = sc_status;
        }
    }
    if (sc_stdin_tty) (void)tcsetattr(0, TCSANOW, &orig);

    int code = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
    sc_now(when, sizeof when);
    {
        char ftr[256];
        int n = snprintf(ftr, sizeof ftr, "\nScript done on %s [COMMAND_EXIT_CODE=\"%d\"]\n", when, code);
        if (n > 0) (void)sc_write_all(tsfd, ftr, (size_t)n);
    }
    close(tsfd);
    if (!quiet) {
        printf("Script done.\n");
        fflush(stdout);
    }
    return ret ? code : 0;
}
