/* ttyparity/main.c -- the INTERACTIVE bash-parity gate: /bin/ttyparity.
 *
 * THE GAP THIS CLOSES
 * -------------------
 * /bin/shparity measures the script surface: stdin is a file, so bash
 * disables every interactive behaviour and the gate is structurally blind
 * to prompts, PS2 continuation, ignoreeof, interactive option defaults,
 * history, and job notification. This gate runs BOTH shells on a real
 * pseudoterminal and compares the terminal byte streams.
 *
 * THE PROTOCOL
 * ------------
 * Interleaving on a tty is timing-dependent, so input is never blasted at
 * the shell; it is paced by PROMPT SENTINELS:
 *
 *   1. spawn the shell on a fresh pty slave; wait for its default prompt
 *      (both end with "$ ")
 *   2. send  PS1='@P@ '; PS2='@C@ '   -- from here both shells frame
 *      their output identically; the transcript COMPARED starts at the
 *      first "@P@ "
 *   3. for each case line: wait until the transcript ends with a
 *      sentinel, then write the line
 *   4. the case must end in something that exits the shell; wait for the
 *      child, then drain the master
 *
 * A line that is exactly  %EOF%  sends one VEOF byte (^D) instead.
 *
 * WHY bash RUNS --noediting
 * -------------------------
 * Interactive bash hands the tty to readline, which puts it in raw mode
 * and does its OWN echo -- a byte stream no canonical-mode shell can
 * match. --noediting makes bash read the tty cooked, so the LINE
 * DISCIPLINE does the echo for both shells and the streams are comparable.
 *
 * THE INSTRUMENT VALIDATES ITSELF FIRST
 * -------------------------------------
 * Before any case runs, bash is compared against a SECOND bash over a
 * fixed script. If the two transcripts differ, the pacing is broken and
 * every later comparison would be noise: the gate reports
 * INSTRUMENT-BROKEN and aborts. (A gate verified only against a passing
 * run has not been verified -- logs/cwwebgl.sh shipped that mistake.)
 *
 * stdout+exit must match byte-for-byte; there is no stderr split on a
 * terminal -- fd 1 and fd 2 are the same slave, which is exactly how a
 * user sees a shell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

#define CORPUS_DIR  "/etc/ttyparity"
#define BASH_PATH   "/bin/bash"
#define TSH_PATH    "/bin/tsh"

#define TIOCSPTLCK  0x40045431UL
#define TIOCGPTN    0x80045430UL

#define MAX_CASES   64
#define MAX_LINES   64
#define CAP_TRANS   32768
#define STEP_MS     20
#define WAIT_MS     8000

#define SENT_P      "@P@ "
#define SENT_C      "@C@ "
/* The sentinels are spelled with SPLIT QUOTES here so the ECHO of this
 * line never contains the sentinel bytes -- otherwise a drain() landing
 * mid-echo (ending exactly at ...@P@ ) would satisfy the suffix test
 * before the prompt ever printed, and the cut would land inside the echo. */
#define SETUP_LINE  "PS1='@''P@ '; PS2='@''C@ '\n"

static char g_ta[CAP_TRANS];       /* transcript, shell A */
static char g_tb[CAP_TRANS];       /* transcript, shell B */
static char g_cases[MAX_CASES][NAME_MAX + 1];
static int  g_ncases;

static void emit(const char *s) { write(1, s, strlen(s)); }

static void emitf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emitf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
    write(1, line, (size_t)n);
}

/* ---- transcript-driven pty session -------------------------------------- */

struct session {
    int    mfd;
    pid_t  pid;
    char  *trans;
    size_t tlen;
    int    broken;            /* a wait timed out; everything after is void */
    int    shell_pg;          /* tty fg group once the shell settled -- the
                               * interrupt settle waits for it to CHANGE */
};

/* Every deadline in this file is REAL TIME. The first version counted
 * spin iterations and guessed their duration; the guess was ~20x off and
 * every "8 second" deadline was ~0.4s -- slow-but-correct steps (a
 * post-pipeline prompt, a job handover) were being killed as timeouts. */
static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(0, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000 + (long)(ts.tv_nsec / 1000000);
}

static int ends_with(const char *buf, size_t len, const char *suf) {
    size_t sl = strlen(suf);
    return len >= sl && memcmp(buf + len - sl, suf, sl) == 0;
}

/* Pull whatever the master has, appending to the transcript.
 *
 * NOT poll(): libtoby's poll falls back to "mark everything ready" when
 * the kernel lacks SYS_POLL, and pty_master_read blocks with no O_NONBLOCK
 * -- the pair wedged this gate's first flight inside an unbounded read.
 * FIONREAD asks the master how many bytes actually exist; a read for
 * exactly that many can never block. */
static void drain(struct session *s, int ms) {
    long deadline = now_ms() + ms;
    for (;;) {
        int avail = 0;
        if (ioctl(s->mfd, 0x541BUL /* FIONREAD */, &avail) != 0) return;
        if (avail <= 0) {
            if (now_ms() >= deadline) return;
            /* NOT usleep(): sleep syscalls in this kernel have a history of
             * wedging (the TKAPP stall), and one hung sleep here freezes the
             * whole gate. A short bounded spin paces the re-poll; the
             * DEADLINE is real time above, never an iteration guess. */
            for (volatile int spin = 0; spin < 100000; spin++) { }
            continue;
        }
        char chunk[512];
        size_t want = (size_t)avail < sizeof chunk ? (size_t)avail
                                                   : sizeof chunk;
        long n = read(s->mfd, chunk, want);
        if (n <= 0) return;
        if (s->tlen + (size_t)n < CAP_TRANS - 1) {
            memcpy(s->trans + s->tlen, chunk, (size_t)n);
            s->tlen += (size_t)n;
            s->trans[s->tlen] = '\0';
        }
        deadline = now_ms();  /* got some; keep pulling until quiet */
    }
}

static void emit_escaped(const char *tag, const char *s, size_t len);

/* Wait until the transcript ends with one of up to two suffixes. */
static int wait_prompt(struct session *s, const char *a, const char *b) {
    long deadline = now_ms() + WAIT_MS;
    while (now_ms() < deadline) {
        drain(s, STEP_MS);
        if (ends_with(s->trans, s->tlen, a)) return 0;
        if (b && ends_with(s->trans, s->tlen, b)) return 0;
    }
    s->broken = 1;
    /* What DID arrive, if not the prompt -- the diagnostic that solved
     * every bring-up mystery this gate had. Stays. */
    emit_escaped("tail", s->trans + (s->tlen > 60 ? s->tlen - 60 : 0),
                 s->tlen > 60 ? 60 : s->tlen);
    return -1;
}

static int session_start(struct session *s, const char *shell, char *trans) {
    memset(s, 0, sizeof *s);
    s->trans = trans;
    trans[0] = '\0';

    int mfd = open("/dev/ptmx", O_RDWR);
    if (mfd < 0) { emit("[ttyparity]   . ptmx open FAILED\n"); return -1; }
    int unlock = 0;
    (void)ioctl(mfd, TIOCSPTLCK, &unlock);
    int idx = -1;
    if (ioctl(mfd, TIOCGPTN, &idx) != 0 || idx < 0) {
        emit("[ttyparity]   . TIOCGPTN FAILED\n");
        close(mfd);
        return -2;
    }
    char spath[32];
    snprintf(spath, sizeof spath, "/dev/pts/%d", idx);
    int sfd = open(spath, O_RDWR);
    if (sfd < 0) { close(mfd); return -3; }

    static char *const envp[] = {
        (char *)"PATH=/bin",
        (char *)"HOME=/",
        (char *)"TERM=dumb",
        (char *)"LC_ALL=C",
        0
    };
    char *argv_bash[] = { (char *)"bash", (char *)"--norc",
                          (char *)"--noprofile", (char *)"--noediting",
                          (char *)"-i", 0 };
    char *argv_tsh[]  = { (char *)"tsh", 0 };
    char *const *argv = strcmp(shell, BASH_PATH) == 0 ? argv_bash : argv_tsh;

    /* Terminal etiquette: make OUR process group the pty's foreground
     * group BEFORE the shell exists. The child inherits our pgid, so its
     * job-control init (`while (tcgetpgrp != getpgrp) kill(0, SIGTTIN)`)
     * sees itself foreground from its first instruction and never stops;
     * it then leads its own group and tcsetpgrp's itself, as bash does. */
    pid_t self_pg = getpgrp();
    (void)ioctl(sfd, 0x5410UL /* TIOCSPGRP */, &self_pg);

    pid_t pid = toby_spawn(shell, argv, envp, sfd, sfd, sfd);
    close(sfd);
    if (pid < 0) { close(mfd); return -4; }
    s->mfd = mfd;
    s->pid = pid;
    return 0;
}

/* Run one scripted conversation. Returns the shell's exit status (>= 0),
 * or a negative marker. `*cut` receives the offset where the compared
 * region begins (the first sentinel prompt). */
static int session_run(struct session *s, char lines[][256], int nlines,
                       size_t *cut) {
    *cut = 0;
    /* The gate runs as root: bash's \$ prompt escape prints '#', tsh's
     * default ends "$ " -- accept either for the pre-sentinel prompt. */
    if (wait_prompt(s, "# ", "$ ") != 0) { emit("[ttyparity]   . TIMEOUT first prompt\n"); goto out; }

    write(s->mfd, SETUP_LINE, strlen(SETUP_LINE));
    if (wait_prompt(s, SENT_P, 0) != 0) { emit("[ttyparity]   . TIMEOUT sentinel\n"); goto out; }
    *cut = s->tlen - strlen(SENT_P);
    /* The tty's foreground group with the shell AT ITS PROMPT: the
     * interrupt settle below waits for this to CHANGE, which is the
     * event "the shell handed the terminal to the job" -- not a timing
     * guess that races the fork. */
    (void)ioctl(s->mfd, 0x540FUL /* TIOCGPGRP */, &s->shell_pg);

    for (int i = 0; i < nlines; i++) {
        /* %CTRLZ%/%CTRLC% interrupt a RUNNING job: no prompt precedes
         * them, and the settle drain gives the shell time to hand the
         * terminal over. %EOF% is TYPED AT A PROMPT like any other line --
         * treating it as an interrupt shifted its timing and made bash's
         * ignoreeof message land after the next echo. */
        int is_intr = (strcmp(lines[i], "%CTRLZ%") == 0 ||
                       strcmp(lines[i], "%CTRLC%") == 0);
        if (is_intr) {
            /* Wait for the EVENT, not an interval: the fg group changing
             * away from the shell's means the job owns the terminal and
             * the ^C/^Z will hit the job. Bounded in real time. */
            long dl = now_ms() + 4000;
            while (now_ms() < dl) {
                int pg = 0;
                if (ioctl(s->mfd, 0x540FUL /* TIOCGPGRP */, &pg) != 0) break;
                if (pg != 0 && pg != s->shell_pg) break;
                drain(s, STEP_MS);
            }
            drain(s, 100);
            char c = (lines[i][1] == 'C' && lines[i][5] == 'C') ? 0x03 : 0x1a;
            write(s->mfd, &c, 1);
        } else if (strcmp(lines[i], "%EOF%") == 0) {
            char c = 0x04;
            write(s->mfd, &c, 1);
        } else {
            write(s->mfd, lines[i], strlen(lines[i]));
            write(s->mfd, "\n", 1);
        }
        if (i + 1 < nlines &&
            strcmp(lines[i + 1], "%CTRLZ%") != 0 &&
            strcmp(lines[i + 1], "%CTRLC%") != 0) {
            if (wait_prompt(s, SENT_P, SENT_C) != 0) goto out;
        }
    }

out:;
    int status = 0;
    int rc = 0;
    if (!s->broken) {
        /* Bounded: a shell that survives its own `exit` line must not hang
         * the gate -- WNOHANG-poll with a REAL-time deadline. */
        long dl = now_ms() + WAIT_MS;
        while (now_ms() < dl) {
            rc = (int)waitpid(s->pid, &status, WNOHANG);
            if (rc != 0) break;
            drain(s, STEP_MS);
        }
    }
    if (s->broken || rc == 0) {
        kill(s->pid, 9);
        (void)waitpid(s->pid, &status, 0);
        drain(s, 100);
        close(s->mfd);
        return -100;
    }
    drain(s, 200);
    close(s->mfd);
    if (rc < 0) return -101;
    return WEXITSTATUS(status);
}

/* ---- case files --------------------------------------------------------- */

static int load_case(const char *name, char lines[][256], int *nlines) {
    char path[NAME_MAX + 32];
    snprintf(path, sizeof path, CORPUS_DIR "/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[8192];
    long got = 0, n;
    while ((n = read(fd, buf + got, sizeof buf - 1 - (size_t)got)) > 0) got += n;
    close(fd);
    buf[got] = '\0';
    *nlines = 0;
    char *p = buf;
    while (*p && *nlines < MAX_LINES) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0 && p[0] != '#') {
            if (len > 255) len = 255;
            memcpy(lines[*nlines], p, len);
            lines[*nlines][len] = '\0';
            (*nlines)++;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* ---- diff reporting ----------------------------------------------------- */

static void emit_escaped(const char *tag, const char *s, size_t len) {
    char out[200];
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < sizeof out; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 32 || c > 126) {
            o += (size_t)snprintf(out + o, sizeof out - o, "\\x%02x", c);
        } else out[o++] = (char)c;
    }
    out[o] = '\0';
    emitf("[ttyparity]     %s: %s\n", tag, out);
}

static void report_diff(const char *a, size_t alen, const char *b, size_t blen) {
    size_t i = 0;
    while (i < alen && i < blen && a[i] == b[i]) i++;
    size_t from = (i > 40) ? i - 40 : 0;
    size_t ae = (alen - from > 120) ? from + 120 : alen;
    size_t be = (blen - from > 120) ? from + 120 : blen;
    emitf("[ttyparity]     first divergence at byte %u\n", (unsigned)i);
    emit_escaped("bash", a + from, ae - from);
    emit_escaped("tsh ", b + from, be - from);
}

/* ---- one case, both shells ---------------------------------------------- */

static int run_pair(const char *name, const char *shell_b, int *pass) {
    static char lines[MAX_LINES][256];
    int nlines = 0;
    if (load_case(name, lines, &nlines) != 0 || nlines == 0) {
        emitf("[ttyparity] %-26s BROKEN unreadable case\n", name);
        return -1;
    }

    struct session sa, sb;
    size_t cut_a = 0, cut_b = 0;
    int st_a = -110, st_b = -110;

    /* One LOGGED retry per side: a session that never says a byte has shown
     * up intermittently (fresh pty, live child, silent for the whole
     * deadline). A deterministic failure fails twice and is still reported;
     * a flake is retried in the open, never silently absorbed. */
    for (int attempt = 0; attempt < 2 && st_a < 0; attempt++) {
        if (attempt) emitf("[ttyparity]   . RETRY bash (st=%d)\n", st_a);
        if (session_start(&sa, BASH_PATH, g_ta) == 0)
            st_a = session_run(&sa, lines, nlines, &cut_a);
    }
    for (int attempt = 0; attempt < 2 && st_b < 0; attempt++) {
        if (attempt) emitf("[ttyparity]   . RETRY tsh (st=%d)\n", st_b);
        if (session_start(&sb, shell_b, g_tb) == 0)
            st_b = session_run(&sb, lines, nlines, &cut_b);
    }

    if (st_a < 0 || st_b < 0) {
        emitf("[ttyparity] %-26s BROKEN st_a=%d st_b=%d\n", name, st_a, st_b);
        return -1;
    }

    const char *a = g_ta + cut_a;
    size_t alen = sa.tlen - cut_a;
    const char *b = g_tb + cut_b;
    size_t blen = sb.tlen - cut_b;

    if (alen == blen && memcmp(a, b, alen) == 0 && st_a == st_b) {
        emitf("[ttyparity] %-26s PASS  (%u bytes, exit=%d)\n",
              name, (unsigned)alen, st_a);
        *pass = 1;
        return 0;
    }
    emitf("[ttyparity] %-26s FAIL  bytes bash=%u tsh=%u exit bash=%d tsh=%d\n",
          name, (unsigned)alen, (unsigned)blen, st_a, st_b);
    report_diff(a, alen, b, blen);
    *pass = 0;
    return 0;
}

/* ---- main --------------------------------------------------------------- */

static int cmp_names(const void *x, const void *y) {
    return strcmp((const char *)x, (const char *)y);
}

/* The gate must be UNSTOPPABLE by the signals it injects: a mis-aimed ^Z
 * once landed on the runner's own group and froze the entire gate until
 * the host-side timeout (the pty's foreground group was still ours). */
static void tp_ignore(int sig) { (void)sig; }

int main(void) {
    /* Lead our own process group: a confused child's kill(0, ...) then
     * reaches at most this gate and its shells, never the console session. */
    (void)setpgid(0, 0);
    signal(SIGTSTP, tp_ignore);
    signal(SIGTTOU, tp_ignore);
    signal(SIGTTIN, tp_ignore);
    signal(SIGINT,  tp_ignore);

    emit("[TTYPARITY] ==== interactive bash-parity gate ====\n");

    /* Gate -1: the instrument itself. bash vs bash over a fixed script
     * must be byte-identical, or the pacing protocol is broken and every
     * later comparison is noise. */
    {
        static char self[MAX_LINES][256];
        strcpy(self[0], "echo selfcheck-$((6*7))");
        strcpy(self[1], "printf 'two\\nlines\\n'");
        strcpy(self[2], "exit 5");
        struct session sa, sb;
        size_t ca = 0, cb = 0;
        int st_a = -110, st_b = -110;
        if (session_start(&sa, BASH_PATH, g_ta) == 0)
            st_a = session_run(&sa, self, 3, &ca);
        if (session_start(&sb, BASH_PATH, g_tb) == 0)
            st_b = session_run(&sb, self, 3, &cb);
        size_t la = sa.tlen - ca, lb = sb.tlen - cb;
        if (st_a != 5 || st_b != 5 || la != lb ||
            memcmp(g_ta + ca, g_tb + cb, la) != 0) {
            emitf("[ttyparity] selfcheck st_a=%d st_b=%d la=%u lb=%u\n",
                  st_a, st_b, (unsigned)la, (unsigned)lb);
            report_diff(g_ta + ca, la, g_tb + cb, lb);
            emit("[TTYPARITY] VERDICT: SKIP reason=INSTRUMENT-BROKEN "
                 "(bash vs bash transcripts differ)\n");
            return 2;
        }
        emitf("[ttyparity] selfcheck: bash==bash over %u bytes, exit 5/5\n",
              (unsigned)la);
    }

    /* Corpus. */
    DIR *d = opendir(CORPUS_DIR);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && g_ncases < MAX_CASES) {
            size_t l = strlen(de->d_name);
            if (l > 4 && strcmp(de->d_name + l - 4, ".txt") == 0) {
                strcpy(g_cases[g_ncases++], de->d_name);
            }
        }
        closedir(d);
    }
    if (g_ncases == 0) {
        emit("[TTYPARITY] VERDICT: SKIP reason=no-corpus\n");
        return 2;
    }
    qsort(g_cases, (size_t)g_ncases, sizeof g_cases[0], cmp_names);

    int pass = 0, fail = 0, broken = 0;
    for (int i = 0; i < g_ncases; i++) {
        int ok = 0;
        if (run_pair(g_cases[i], TSH_PATH, &ok) != 0) broken++;
        else if (ok) pass++;
        else fail++;
    }

    emitf("[TTYPARITY] VERDICT: %s %d/%d pass fail=%d broken=%d "
          "(terminal byte stream + exit must match GNU bash 5.2)\n",
          (fail == 0 && broken == 0) ? "PASS" : "FAIL",
          pass, g_ncases, fail, broken);
    return (fail == 0 && broken == 0) ? 0 : 1;
}
