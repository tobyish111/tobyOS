/* tvitest -- the conformance gate for /bin/tvi, run inside the guest.
 *
 * WHAT IT ASSERTS, and why that shape: each case writes a fixture file,
 * spawns tvi on a REAL pty, feeds a keystroke script, waits for the
 * editor to exit, then reads the file back and compares it BYTE FOR BYTE
 * against the expected result.
 *
 * That is deliberately not "did tvi exit 0" and not "does the screen look
 * right". An editor's contract is what it leaves on disk; a screen
 * comparison would be a terminal-emulation test wearing an editor test's
 * name, and an exit status proves only that it did not crash. Every
 * expectation below is a POSIX vi(1) behaviour, so a failure means tvi is
 * wrong -- not that somebody's taste differs.
 *
 * A pty is mandatory: tvi refuses to run when stdin is not a terminal
 * (it would otherwise scribble escape sequences into a pipe and call
 * itself an editor), so a plain pipe harness could not drive it at all.
 *
 * PACING: the pty buffer is finite and tvi repaints the whole screen on
 * every keystroke, so the master MUST be drained while the script is
 * being written or the editor blocks on write() and the case times out
 * looking like a hang. FIONREAD is used for that -- libtoby's poll() is
 * on record as unreliable here (see the shell-ttyparity memory).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

#define TIOCSPTLCK  0x40045431UL
#define TIOCGPTN    0x80045430UL
#define FIONREAD    0x541BUL
#define TIOCSPGRP   0x5410UL

#define WORK "/tmp/tvitest.txt"

static int g_pass, g_fail;
static char g_wrote[512];

/* Drain until the master has been quiet for `ms` REAL milliseconds.
 *
 * Two lessons are baked in here, both already paid for in this tree:
 *
 * 1. A fixed-count drain is not enough. tvi repaints the whole screen per
 *    keystroke (~2 KB of escapes at 80x24), so letting output back up
 *    fills the pty, blocks tvi in write(), and every longer case times
 *    out. The signature was that the only passing cases were the two
 *    SHORTEST scripts -- failure correlated with LENGTH.
 *
 * 2. The bound must be REAL TIME, not an iteration count. Counting
 *    iterations made the harness a busy-spin whose duration depends on
 *    TCG load, and a spinning userspace process starves the one it is
 *    waiting for (see the scheduler note in the shell-ttyparity memory).
 *    The signature there was different: results shuffled between runs
 *    with the pass COUNT unchanged -- nondeterminism, not length.
 *    logs/ttyparity.sh's drain solved this first; this is the same shape.
 *
 * NOT poll() (libtoby's lies) and NOT usleep() (sleep syscalls here have
 * a history of wedging): FIONREAD for availability, a bounded spin to
 * pace the re-poll, and a real clock for the deadline. */
static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(0, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000 + (long)(ts.tv_nsec / 1000000);
}

static void drain_ms(int mfd, int ms) {
    long deadline = now_ms() + ms;
    char buf[4096];
    for (;;) {
        int avail = 0;
        if (ioctl(mfd, FIONREAD, &avail) != 0) return;
        if (avail <= 0) {
            if (now_ms() >= deadline) return;
            for (volatile int spin = 0; spin < 100000; spin++) { }
            continue;
        }
        int n = avail > (int)sizeof buf ? (int)sizeof buf : avail;
        if (read(mfd, buf, (size_t)n) <= 0) return;
        deadline = now_ms() + ms;   /* got some; keep pulling until quiet */
    }
}

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (content && *content) fputs(content, f);
    fclose(f);
    return 0;
}

static int read_file(const char *path, char *out, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { out[0] = '\0'; return -1; }
    int n = (int)fread(out, 1, (size_t)cap - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    out[n] = '\0';
    return n;
}

/* Render a string with newlines shown, so a diff is readable on one line. */
static void show(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\n') fputs("\\n", stdout);
        else if (*p == '\t') fputs("\\t", stdout);
        else putchar(*p);
    }
}

/* Drive one editing session. `keys` may contain \033 for ESC. */
static int run_prog(const char *prog, const char *file, const char *keys) {
    int mfd = open("/dev/ptmx", O_RDWR);
    if (mfd < 0) return -1;
    int unlock = 0;
    (void)ioctl(mfd, TIOCSPTLCK, &unlock);
    int idx = -1;
    if (ioctl(mfd, TIOCGPTN, &idx) != 0 || idx < 0) { close(mfd); return -2; }
    char spath[32];
    snprintf(spath, sizeof spath, "/dev/pts/%d", idx);
    int sfd = open(spath, O_RDWR);
    if (sfd < 0) { close(mfd); return -3; }

    /* TEDIT_TRACE makes the editor log every key it actually received to
     * a FILE. Reading the source gave three confident wrong answers about
     * where it was blocking; the trace answered it in one run. Harmless
     * for tvi, which ignores the variable. */
    static char *const envp[] = {
        (char *)"PATH=/bin", (char *)"HOME=/", (char *)"TERM=vt100",
        (char *)"LC_ALL=C", (char *)"TEDIT_TRACE=/tmp/tedit.trace", 0
    };
    const char *base = strrchr(prog, '/');
    char *argv[] = { (char *)(base ? base + 1 : prog), (char *)file, 0 };

    pid_t self_pg = getpgrp();
    (void)ioctl(sfd, TIOCSPGRP, &self_pg);

    pid_t pid = toby_spawn(prog, argv, envp, sfd, sfd, sfd);
    close(sfd);
    if (pid < 0) { close(mfd); return -4; }

    /* WAIT FOR THE FIRST PAINT before typing anything.
     *
     * Not a quiet-period wait -- an editor that has not started yet is
     * perfectly quiet, so drain_ms() returned instantly and the whole
     * script was written while the pty was still in CANONICAL mode. The
     * line discipline then ate it: \r became \n (ICRNL), DEL (127) was
     * VERASE and ERASED the preceding keystroke, ^D (4) was VEOF and
     * delivered end-of-file so the editor exited cleanly without saving,
     * and everything after the first \n stayed line-buffered forever --
     * which is what "editor never exited" actually meant.
     *
     * Output on the master is positive evidence the editor has painted,
     * which it can only do after tcsetattr() put the tty in raw mode. */
    {
        long deadline = now_ms() + 8000;
        for (;;) {
            int avail = 0;
            if (ioctl(mfd, FIONREAD, &avail) == 0 && avail > 0) break;
            if (now_ms() >= deadline) break;   /* fall through; the case
                                                * will fail loudly rather
                                                * than hang the run */
            for (volatile int s = 0; s < 20000; s++) { }
        }
        drain_ms(mfd, 120);
    }

    /* One byte at a time, draining to quiet between each. Every write is
     * accounted for: without the harness's own half of the story, a byte
     * the editor never received is indistinguishable from a byte the
     * harness never sent. */
    g_wrote[0] = '\0';
    int wn = 0;
    for (const char *p = keys; *p; p++) {
        /* An ESC and the byte after it go out TOGETHER.
         *
         * A real terminal emits Alt-U and the arrow keys as one
         * contiguous burst, and that contiguity is exactly how a program
         * tells "Meta" from "the user pressed Escape". Writing them a
         * byte apart with a 60 ms drain in between is a faithful
         * rendering of pressing Escape, waiting, then pressing U -- so
         * the editor was right to read it that way and the harness was
         * wrong to send it that way. */
        int nb = (*p == 27 && p[1]) ? 2 : 1;
        long w = write(mfd, p, (size_t)nb);
        wn += snprintf(g_wrote + wn, sizeof g_wrote - (size_t)wn, "%d%s%s ",
                       (unsigned char)*p, nb == 2 ? "+meta" : "",
                       w == nb ? "" : "!FAIL");
        if (w != nb) break;
        if (nb == 2) p++;
        drain_ms(mfd, 60);
    }

    int st = 0;
    long deadline = now_ms() + 20000;
    while (now_ms() < deadline) {
        drain_ms(mfd, 20);
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) { close(mfd); return 0; }
    }
    /* Still alive: it never processed the quit. Kill so the run continues
     * and report it as a timeout rather than hanging the whole gate. */
    kill(pid, 9);
    waitpid(pid, &st, 0);
    close(mfd);
    return -5;
}

static const char *g_prog = "/bin/tvi";
static const char *g_tag  = "TVI";

static void tcase(const char *name, const char *initial,
                  const char *keys, const char *expect) {
    char got[8192];
    if (write_file(WORK, initial) != 0) {
        printf("[%s]   FAIL %-28s (cannot write fixture)\n", g_tag, name);
        g_fail++; return;
    }
    int rc = run_prog(g_prog, WORK, keys);
    if (rc == -5) {
        printf("[%s]   FAIL %-28s (editor never exited)\n", g_tag, name);
        printf("[%s]        harness wrote: %s\n", g_tag, g_wrote);
        /* Say WHICH key it stopped on rather than only that it hung. */
        char tr[1024];
        if (read_file("/tmp/tedit.trace", tr, sizeof tr) > 0) {
            printf("[%s]        trace: ", g_tag);
            for (char *p = tr; *p; p++) putchar(*p == '\n' ? ' ' : *p);
            printf("\n");
        }
        g_fail++; return;
    }
    if (rc < 0) {
        printf("[%s]   FAIL %-28s (spawn/pty error %d)\n", g_tag, name, rc);
        g_fail++; return;
    }
    read_file(WORK, got, sizeof got);
    if (strcmp(got, expect) == 0) {
        printf("[%s]   ok   %-28s -> \"", g_tag, name); show(got); printf("\"\n");
        g_pass++;
    } else {
        printf("[%s]   FAIL %-28s\n", g_tag, name);
        printf("[%s]        want \"", g_tag); show(expect); printf("\"\n");
        printf("[%s]        got  \"", g_tag); show(got);    printf("\"\n");
        g_fail++;
    }
}

int main(void) {
    printf("[TVI] ==== native vi conformance (file bytes, not exit codes) ====\n");

    /* ---- insert-mode entry ---- */
    tcase("i inserts at cursor",      "abc\n",        "iXY\033:wq\n",  "XYabc\n");
    tcase("a appends after cursor",   "abc\n",        "aXY\033:wq\n",  "aXYbc\n");
    tcase("A appends at EOL",         "abc\n",        "AZ\033:wq\n",   "abcZ\n");
    tcase("I inserts at first nonblank", "  ab\n",    "IX\033:wq\n",   "  Xab\n");
    tcase("o opens line below",       "a\nb\n",       "oX\033:wq\n",   "a\nX\nb\n");
    tcase("O opens line above",       "a\nb\n",       "OX\033:wq\n",   "X\na\nb\n");

    /* ---- deletion ---- */
    tcase("x deletes char",           "abc\n",        "x:wq\n",        "bc\n");
    tcase("3x deletes three",         "abcde\n",      "3x:wq\n",       "de\n");
    tcase("X deletes before cursor",  "abc\n",        "llX:wq\n",      "ac\n");
    tcase("dd deletes line",          "a\nb\nc\n",    "dd:wq\n",       "b\nc\n");
    tcase("2dd deletes two lines",    "a\nb\nc\n",    "2dd:wq\n",      "c\n");
    tcase("dw deletes word",          "foo bar\n",    "dw:wq\n",       "bar\n");
    tcase("D deletes to EOL",         "abcdef\n",     "llD:wq\n",      "ab\n");
    tcase("d$ deletes to EOL",        "abcdef\n",     "lld$:wq\n",     "ab\n");

    /* ---- change ---- */
    tcase("cw changes word",          "foo bar\n",    "cwbaz\033:wq\n","baz bar\n");
    tcase("cc changes whole line",    "a\nb\n",       "ccZ\033:wq\n",  "Z\nb\n");
    tcase("C changes to EOL",         "abcdef\n",     "llCX\033:wq\n", "abX\n");
    tcase("s substitutes char",       "abc\n",        "sZ\033:wq\n",   "Zbc\n");
    tcase("r replaces one char",      "abc\n",        "rZ:wq\n",       "Zbc\n");
    tcase("~ toggles case",           "aBc\n",        "3~:wq\n",       "AbC\n");

    /* ---- yank / put (linewise vs charwise is the subtle one) ---- */
    tcase("yy p duplicates line",     "a\nb\n",       "yyp:wq\n",      "a\na\nb\n");
    tcase("dd p moves line down",     "a\nb\n",       "ddp:wq\n",      "b\na\n");
    tcase("yy P puts above",          "a\nb\n",       "yyP:wq\n",      "a\na\nb\n");

    /* ---- joins ---- */
    tcase("J joins with a space",     "ab\ncd\n",     "J:wq\n",        "ab cd\n");
    tcase("J strips leading blanks",  "ab\n   cd\n",  "J:wq\n",        "ab cd\n");

    /* ---- motion + counts ---- */
    tcase("G goes to last line",      "a\nb\nc\n",    "Gdd:wq\n",      "a\nb\n");
    tcase("3G goes to line 3",        "a\nb\nc\nd\n", "3Gdd:wq\n",     "a\nb\nd\n");
    tcase("gg goes to first line",    "a\nb\nc\n",    "Gggdd:wq\n",    "b\nc\n");
    tcase("$ moves to last char",     "abc\n",        "$x:wq\n",       "ab\n");
    tcase("0 moves to column 0",      "abc\n",        "$0x:wq\n",      "bc\n");
    tcase("w moves a word",           "foo bar\n",    "wD:wq\n",       "foo \n");
    tcase("b moves back a word",      "foo bar\n",    "$bD:wq\n",      "foo \n");
    tcase("f finds a char",           "a-b-c\n",      "f-x:wq\n",      "ab-c\n");

    /* ---- undo ---- */
    tcase("u undoes a delete",        "a\nb\n",       "ddu:wq\n",      "a\nb\n");
    tcase("u undoes an insert",       "abc\n",        "iXY\033u:wq\n", "abc\n");
    tcase("u then redo",             "a\nb\n",       "ddu\022:wq\n",  "b\n");

    /* ---- ex commands ---- */
    tcase(":s substitutes first",     "aaa\n",        ":s/a/b/\n:wq\n","baa\n");
    tcase(":s///g substitutes all",   "aaa\n",        ":s/a/b/g\n:wq\n","bbb\n");
    tcase(":%s across the file",      "aa\naa\n",     ":%s/a/X/g\n:wq\n","XX\nXX\n");
    tcase(":s with a real regex",     "a1b22c\n",     ":s/[0-9][0-9]/#/\n:wq\n","a1b#c\n");
    tcase(":s backreference",         "ab\n",         ":s/\\(a\\)\\(b\\)/\\2\\1/\n:wq\n","ba\n");
    tcase(":s ampersand is the match","ab\n",         ":s/a/[&]/\n:wq\n","[a]b\n");
    tcase(":N goes to a line",        "a\nb\nc\n",    ":2\ndd:wq\n",   "a\nc\n");
    tcase(":$ goes to last line",     "a\nb\nc\n",    ":$\ndd:wq\n",   "a\nb\n");
    tcase(":d deletes a line",        "a\nb\nc\n",    ":2\n:d\n:wq\n", "a\nc\n");
    tcase(":1,2d deletes a range",    "a\nb\nc\n",    ":1,2d\n:wq\n",  "c\n");
    tcase(":q! discards changes",     "a\nb\n",       "dd:q!\n",       "a\nb\n");

    /* ---- search ---- */
    tcase("/ finds forward",          "aa\nbb\ncc\n", "/bb\ndd:wq\n",  "aa\ncc\n");
    tcase("n repeats the search",     "x\nq\nx\nq\n", "/q\nndd:wq\n",  "x\nq\nx\n");
    tcase("? finds backward",         "aa\nbb\ncc\n", "G?bb\ndd:wq\n", "aa\ncc\n");

    /* ---- file handling ---- */
    tcase("trailing newline is kept", "a\nb\n",       ":wq\n",         "a\nb\n");
    tcase("missing final newline added", "a\nb",      ":wq\n",         "a\nb\n");
    tcase("empty file stays empty",   "",             ":wq\n",         "\n");

    int vi_pass = g_pass, vi_fail = g_fail;
    printf("[TVI] VERDICT: %s pass=%d fail=%d\n",
           vi_fail == 0 ? "PASS" : "FAIL", vi_pass, vi_fail);

    /* ============ tedit: the modeless (nano-shaped) editor =============
     * Same discipline: keystrokes in, FILE BYTES out. The escapes below
     * are nano's bindings as raw control codes --
     *   \017 ^O write out   \030 ^X exit      \013 ^K cut
     *   \025 ^U uncut       \027 ^W where-is  \034 ^\ replace
     *   \036 ^6 set mark    \037 ^_ goto      \033 ESC (the Meta prefix)
     *   \004 ^D delete      \005 ^E end       \006 ^F right  \016 ^N down
     * ================================================================== */
    g_prog = "/bin/tedit"; g_tag = "TED";
    g_pass = 0; g_fail = 0;
    printf("[TED] ==== native modeless editor (nano-shaped) ====\n");

    /* ---- modeless: typing just types, no mode to enter first ---- */
    tcase("types without a mode",      "abc\n",      "XY\017\n\030",   "XYabc\n");
    tcase("Enter splits a line",       "ab\n",       "\r\017\n\030",   "\nab\n");
    tcase("backspace joins lines",     "ab\ncd\n",   "\016\177\017\n\030", "abcd\n");
    tcase("^D deletes forward",        "abc\n",      "\004\017\n\030", "bc\n");
    tcase("^E goes to end of line",    "abc\n",      "\005Z\017\n\030","abcZ\n");

    /* ---- cut/uncut, incl. nano's accumulate-on-consecutive-^K rule ---- */
    tcase("^K cuts a line",            "a\nb\nc\n",  "\013\017\n\030", "b\nc\n");
    tcase("^K^K accumulates",          "a\nb\nc\n",  "\013\013\017\n\030", "c\n");
    tcase("^K then ^U round-trips",    "a\nb\n",     "\013\025\017\n\030", "a\nb\n");
    tcase("^U pastes both cut lines",  "a\nb\nc\n",  "\013\013\025\017\n\030", "a\nb\nc\n");

    /* ---- undo/redo, incl. the typing-run coalescing improvement ---- */
    tcase("M-U undoes a cut",          "a\nb\n",     "\013\033u\017\n\030", "a\nb\n");
    tcase("a typed RUN undoes as one", "abc\n",      "XYZ\033u\017\n\030",  "abc\n");
    tcase("M-E redoes",                "a\nb\n",     "\013\033u\033e\017\n\030", "b\n");

    /* ---- search then edit at the match ---- */
    tcase("^W finds and moves there",  "aa\nbb\ncc\n", "\027bb\r\013\017\n\030", "aa\ncc\n");

    /* ---- replace, incl. the regex + backreference improvements ---- */
    tcase("^\\ replace all",           "aaa\n",      "\034a\rb\rA\017\n\030", "bbb\n");
    /* ^C to stop, not a bare ESC. An ESC sent immediately before another
     * key IS Meta by universal terminal convention -- which is exactly
     * what the harness now (correctly) reproduces -- so "\033\017" reads
     * as M-^O, not as cancel. nano documents ^C for this anyway, and it
     * is unambiguous. That ^C arrives as DATA rather than as SIGINT is
     * itself only true because raw mode finally reaches the kernel. */
    tcase("^\\ replace one then stop", "aaa\n",      "\034a\rb\ry\003\017\n\030", "baa\n");
    tcase("replace with a regex",      "a1b22c\n",   "\034[0-9][0-9]\r#\rA\017\n\030", "a1b#c\n");
    tcase("replace with a backref",    "ab\n",       "\034\\(a\\)\\(b\\)\r\\2\\1\rA\017\n\030", "ba\n");
    tcase("& is the whole match",      "ab\n",       "\034a\r[&]\rA\017\n\030", "[a]b\n");

    /* ---- mark + region; M-6 copy-without-cutting is the improvement ---- */
    tcase("^6 mark then ^K cuts it",   "abcdef\n",   "\036\006\006\006\013\017\n\030", "def\n");

    /* ---- go to line, and line,column ---- */
    tcase("^_ goes to a line",         "a\nb\nc\n",  "\037" "2\r\013\017\n\030", "a\nc\n");
    /* Column is 1-BASED, so 1,3 puts the cursor ON the third character
     * and ^D removes it: "abcd" -> "abd". The first version of this case
     * expected "abc", which would have meant deleting the FOURTH -- the
     * test was wrong, not the editor. */
    tcase("^_ takes line,column",      "abcd\nx\n",  "\037" "1,3\r\004\017\n\030", "abd\nx\n");

    /* ---- file handling ---- */
    tcase("^O writes then ^X exits",   "hello\n",    "\017\n\030",     "hello\n");
    tcase("^X on a clean buffer",      "keep\n",     "\030",           "keep\n");
    tcase("^X answering N discards",   "a\nb\n",     "\013\030n",      "a\nb\n");
    tcase("missing final newline added","a\nb",      "\017\n\030",     "a\nb\n");

    printf("[TED] VERDICT: %s pass=%d fail=%d\n",
           g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);

    int total_fail = vi_fail + g_fail;
    printf("[EDIT] VERDICT: %s tvi=%d/%d tedit=%d/%d\n",
           total_fail == 0 ? "PASS" : "FAIL",
           vi_pass, vi_pass + vi_fail, g_pass, g_pass + g_fail);
    return total_fail == 0 ? 0 : 1;
}
