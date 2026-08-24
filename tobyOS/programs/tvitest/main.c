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
static int run_tvi(const char *file, const char *keys) {
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

    static char *const envp[] = {
        (char *)"PATH=/bin", (char *)"HOME=/", (char *)"TERM=vt100",
        (char *)"LC_ALL=C", 0
    };
    char *argv[] = { (char *)"tvi", (char *)file, 0 };

    pid_t self_pg = getpgrp();
    (void)ioctl(sfd, TIOCSPGRP, &self_pg);

    pid_t pid = toby_spawn("/bin/tvi", argv, envp, sfd, sfd, sfd);
    close(sfd);
    if (pid < 0) { close(mfd); return -4; }

    /* Let the editor paint its first screen before typing at it. */
    drain_ms(mfd, 300);

    /* One byte at a time, draining to quiet between each. */
    for (const char *p = keys; *p; p++) {
        if (write(mfd, p, 1) != 1) break;
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

static void tcase(const char *name, const char *initial,
                  const char *keys, const char *expect) {
    char got[8192];
    if (write_file(WORK, initial) != 0) {
        printf("[TVI]   FAIL %-28s (cannot write fixture)\n", name);
        g_fail++; return;
    }
    int rc = run_tvi(WORK, keys);
    if (rc == -5) {
        printf("[TVI]   FAIL %-28s (editor never exited)\n", name);
        g_fail++; return;
    }
    if (rc < 0) {
        printf("[TVI]   FAIL %-28s (spawn/pty error %d)\n", name, rc);
        g_fail++; return;
    }
    read_file(WORK, got, sizeof got);
    if (strcmp(got, expect) == 0) {
        printf("[TVI]   ok   %-28s -> \"", name); show(got); printf("\"\n");
        g_pass++;
    } else {
        printf("[TVI]   FAIL %-28s\n", name);
        printf("[TVI]        want \""); show(expect); printf("\"\n");
        printf("[TVI]        got  \""); show(got);    printf("\"\n");
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

    printf("[TVI] VERDICT: %s pass=%d fail=%d\n",
           g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
