/* shparity/main.c -- the bash-parity gate: /bin/shparity.
 *
 * THE CONTRACT THIS MEASURES
 * --------------------------
 * The tobyOS shell is meant to be a SUPERSET of bash, the way TypeScript is
 * a superset of JavaScript: every script that runs under bash must run under
 * tsh and produce the SAME BYTES and the SAME EXIT STATUS. Extra features are
 * allowed; divergence on shared syntax is a bug, not a dialect.
 *
 * "Superset" is a claim until something checks it, so this program checks it
 * DIFFERENTIALLY against the real thing. tobyOS already ships an unmodified
 * GNU bash 5.2 (programs/realbash), so the oracle is not a specification we
 * interpret -- it is the actual binary every user compares us against. For
 * each corpus script we run:
 *
 *     /bin/bash --norc --noprofile CASE      (the oracle)
 *     /bin/tsh                     CASE      (the subject)
 *
 * with identical argv, identical environment, and identical fresh working
 * directories, then require byte-identical stdout and identical exit status.
 *
 * WHAT COUNTS AS A FAILURE, AND WHAT DOES NOT
 * -------------------------------------------
 * stdout and exit status must match EXACTLY -- that is the superset contract.
 * stderr is captured and reported but NOT required to match: diagnostics
 * legitimately name the shell that produced them ("bash: line 3: ..." can
 * never equal a tsh message), so requiring equality there would bake in a
 * failure we would then be tempted to paper over. It is reported so a case
 * that starts spewing errors is still visible.
 *
 * WHY A FRESH WORKING DIRECTORY PER RUN
 * -------------------------------------
 * Cases that create files (redirection, globbing) would otherwise let the
 * bash run seed state that the tsh run reads -- the second shell would look
 * correct because the first one already did the work. Each run gets a wiped
 * directory, so each shell is measured on what IT did.
 *
 * WHERE SCRATCH LIVES
 * -------------------
 * /tmp (tmpfs), falling back to /data. It cannot live on `/`: the root
 * filesystem is the read-only initrd ramfs, whose create hook returns ROFS
 * for any path not already in the tar. If neither is writable the gate
 * reports SKIP -- loudly, with the reason -- rather than inventing a pass. A
 * gate that silently degrades is worse than no gate. See SCRATCH_DIR below
 * for why this moved off the persistent volume.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

#define CORPUS_DIR   "/etc/shparity"
/* Scratch lives on tmpfs, not on the persistent volume.
 *
 * It used to be /data, because /tmp did not exist -- the root filesystem is
 * the read-only initrd ramfs and nothing had ever mounted a writable temp
 * area. That made every gate run write to the real disk, and killing QEMU
 * mid-run (which the harness script does on timeout) eventually corrupted the
 * volume: `mount of '/data' WARNING: 20 issue(s): ino=96 allocated but
 * type=FREE (orphan slot)`, after which the scratch directory could be
 * neither opened nor recreated and the gate could only report SKIP.
 *
 * A test harness has no business persisting anything. tmpfs is fresh every
 * boot, needs no cleanup, cannot be corrupted by a hard kill, and does not
 * make the gate depend on a disk being attached. */
#define SCRATCH_DIR  "/tmp/shparity"
#define SCRATCH_ALT  "/data/shparity"   /* fallback if /tmp is missing */
#define BASH_PATH    "/bin/bash"
#define TSH_PATH     "/bin/tsh"

#define MAX_CASES    160
#define MAX_PATH     256
#define CAP_OUT      65536      /* per-stream capture cap */

/* Captured output lives in .bss, not on the stack: two CAP_OUT buffers is
 * 128 KiB and user stacks here are far smaller than that. */
static char g_buf_a[CAP_OUT];
static char g_buf_b[CAP_OUT];
static char g_cases[MAX_CASES][NAME_MAX + 1];
static int  g_ncases;

/* ---- output -------------------------------------------------------------
 *
 * Every line is formatted into one buffer and emitted with a SINGLE write().
 * The serial console splits a printf at its format conversions, so a line
 * built from several printf calls arrives interleaved with whatever else is
 * logging -- and then a whole-line grep in the host harness silently matches
 * nothing. One write per line is what makes the host-side greps trustworthy. */
static void emit(const char *s) {
    write(1, s, strlen(s));
}

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

/* ---- small helpers ------------------------------------------------------ */

static int has_suffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* Read a whole file into `buf`, capped. Returns the byte count, or -1. A file
 * that would exceed the cap is reported as an error rather than silently
 * truncated -- a truncated capture compares equal too easily. */
static long read_all(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    for (;;) {
        if (got >= cap) { close(fd); return -2; }   /* overflow */
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    return (long)got;
}

/* Delete every regular file in `dir`. The corpus is written so that cases do
 * not create subdirectories, so a flat sweep is enough; a case that did would
 * show up as a leftover and is worth catching loudly rather than recursing
 * blindly under a path we then delete from. */
static int wipe_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int left = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_type == DT_DIR) { left++; continue; }
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (unlink(p) != 0) left++;
    }
    closedir(d);
    return left;      /* >0 == something survived; caller decides how loud */
}

/* ---- running one shell over one case ------------------------------------
 *
 * Returns the child's exit status, or a negative value if it could not run.
 * `workdir` is the child's cwd; `out`/`err` receive its two streams. */
static int run_shell(const char *shell, char *const argv[],
                     const char *workdir, const char *out, const char *err) {
    static char *const envp[] = {
        (char *)"PATH=/bin",
        (char *)"HOME=/",
        (char *)"TERM=dumb",
        (char *)"LC_ALL=C",
        0
    };

    int fo = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fo < 0) return -101;
    int fe = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fe < 0) { close(fo); return -102; }

    /* The child inherits our cwd, so chdir here and restore after. Both
     * shells therefore start from a byte-identical directory state. */
    char saved[MAX_PATH];
    if (!getcwd(saved, sizeof saved)) saved[0] = '\0';
    if (chdir(workdir) != 0) { close(fo); close(fe); return -103; }

    pid_t pid = toby_spawn(shell, argv, envp, 0, fo, fe);

    if (saved[0]) (void)chdir(saved);
    close(fo);
    close(fe);

    if (pid < 0) return -104;

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -105;
    return WEXITSTATUS(status);
}

/* ---- diffing ------------------------------------------------------------ */

/* Print the first differing line from each side, escaped and truncated. This
 * is the whole diagnostic value of the gate: "outputs differ" tells you
 * nothing, "line 3: bash said `a b` and tsh said `a  b`" tells you it is word
 * splitting. */
/* Print every differing LINE, not just the first differing byte.
 *
 * The old version reported one line per side, taken at the first byte that
 * differed. That is enough to know a case failed and almost never enough to
 * know why: if one shell emits an extra line early, every later line is
 * "different" and the reported pair points at whatever happened to be
 * adjacent rather than at the cause. Three root causes in a row were read
 * wrong that way -- a per-handle lseek looked like a flushing problem, a
 * multi-line function body looked like a getopts bug, and a builtin shadowing
 * /bin/cat looked like a pipeline bug.
 *
 * Comparing line by line and showing each mismatch makes the SHAPE of the
 * difference visible: a missing line, an extra line, or a changed one. Bounded,
 * so a wholly divergent case cannot flood the serial log. */

#define DIFF_MAX_LINES 14

/* Copy line `want` (0-based) of `buf` into `out`, escaping control characters.
 * Returns -1 if that line does not exist -- itself information, since it says
 * one side simply stopped producing output. */
static int line_at(const char *buf, long len, int want, char *out, size_t cap) {
    int line = 0;
    long i = 0;
    while (i < len && line < want) {
        if (buf[i] == '\n') line++;
        i++;
    }
    if (i >= len && line < want) return -1;
    size_t o = 0;
    for (; i < len && buf[i] != '\n' && o + 5 < cap; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20 || c == 0x7f)
            o += (size_t)snprintf(out + o, cap - o, "\\x%02x", c);
        else out[o++] = (char)c;
    }
    out[o] = '\0';
    return 0;
}

static int count_lines(const char *buf, long len) {
    int n = 0;
    for (long i = 0; i < len; i++) if (buf[i] == '\n') n++;
    if (len > 0 && buf[len - 1] != '\n') n++;
    return n;
}

static void show_diff(const char *a, long na, const char *b, long nb) {
    int la = count_lines(a, na), lb = count_lines(b, nb);
    emitf("[shparity]     lines bash=%d tsh=%d\n", la, lb);

    int shown = 0;
    int most = (la > lb) ? la : lb;
    for (int n = 0; n < most && shown < DIFF_MAX_LINES; n++) {
        char ta[160], tb[160];
        int ra = line_at(a, na, n, ta, sizeof ta);
        int rb = line_at(b, nb, n, tb, sizeof tb);
        if (ra < 0) ta[0] = '\0';
        if (rb < 0) tb[0] = '\0';
        if (ra == rb && strcmp(ta, tb) == 0) continue;
        emitf("[shparity]     L%-3d bash: %s\n", n + 1, ra < 0 ? "<none>" : ta);
        emitf("[shparity]     L%-3d tsh : %s\n", n + 1, rb < 0 ? "<none>" : tb);
        shown++;
    }
    if (shown >= DIFF_MAX_LINES)
        emitf("[shparity]     ... further differences suppressed\n");
}

/* ---- main --------------------------------------------------------------- */

int main(void) {
    emit("[shparity] ==== tobyOS shell <-> GNU bash differential parity gate ====\n");

    /* --- oracle and subject must both exist. Say which is missing. --- */
    int fd = open(BASH_PATH, O_RDONLY);
    if (fd < 0) {
        emit("[SHPARITY] VERDICT: SKIP reason=no-bash "
             "(need programs/realbash/bash staged as /bin/bash)\n");
        return 0;
    }
    close(fd);

    fd = open(TSH_PATH, O_RDONLY);
    if (fd < 0) {
        emit("[SHPARITY] VERDICT: SKIP reason=no-tsh "
             "(programs/sh/sh.elf must be staged as /bin/tsh)\n");
        return 0;
    }
    close(fd);

    /* --- scratch space: tmpfs, falling back to /data. See the header. --- */
    const char *scratch = 0;
    static const char *const candidates[] = { SCRATCH_DIR, SCRATCH_ALT, 0 };
    for (int c = 0; candidates[c] && !scratch; c++) {
        if (mkdir(candidates[c], 0755) == 0) { scratch = candidates[c]; break; }
        /* Already there is fine -- as long as it can actually be opened.
         * A directory whose inode was orphaned by a crash fails BOTH
         * mkdir (the name exists) and opendir (the inode does not), which
         * is exactly why this tries the next candidate instead of
         * assuming the first one works. */
        DIR *probe = opendir(candidates[c]);
        if (probe) { closedir(probe); scratch = candidates[c]; }
    }
    if (!scratch) {
        emit("[SHPARITY] VERDICT: SKIP reason=no-writable-scratch "
             "(tried /tmp and /data; root is the read-only initrd ramfs)\n");
        return 0;
    }
    emitf("[shparity] scratch: %s\n", scratch);

    char wdir_a[MAX_PATH], wdir_b[MAX_PATH];
    snprintf(wdir_a, sizeof wdir_a, "%s/a", scratch);
    snprintf(wdir_b, sizeof wdir_b, "%s/b", scratch);
    (void)mkdir(wdir_a, 0755);
    (void)mkdir(wdir_b, 0755);

    /* --- collect the corpus, sorted, so runs are comparable to each other. */
    DIR *cd = opendir(CORPUS_DIR);
    if (!cd) {
        emit("[SHPARITY] VERDICT: SKIP reason=no-corpus "
             "(expected " CORPUS_DIR "/*.sh in the initrd)\n");
        return 0;
    }
    struct dirent *de;
    while ((de = readdir(cd)) != NULL && g_ncases < MAX_CASES) {
        if (de->d_name[0] == '.') continue;
        if (!has_suffix(de->d_name, ".sh")) continue;
        snprintf(g_cases[g_ncases], sizeof g_cases[0], "%s", de->d_name);
        g_ncases++;
    }
    closedir(cd);

    for (int i = 1; i < g_ncases; i++) {          /* insertion sort by name */
        char key[NAME_MAX + 1];
        snprintf(key, sizeof key, "%s", g_cases[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(g_cases[j], key) > 0) {
            memcpy(g_cases[j + 1], g_cases[j], sizeof g_cases[0]);
            j--;
        }
        snprintf(g_cases[j + 1], sizeof g_cases[0], "%s", key);
    }

    if (g_ncases == 0) {
        emit("[SHPARITY] VERDICT: SKIP reason=empty-corpus\n");
        return 0;
    }
    emitf("[shparity] corpus: %d case(s) from %s\n", g_ncases, CORPUS_DIR);

    /* --- run every case under both shells. --- */
    int pass = 0, fail = 0, broke = 0, errdiff = 0;

    for (int i = 0; i < g_ncases; i++) {
        char cpath[MAX_PATH];
        snprintf(cpath, sizeof cpath, "%s/%s", CORPUS_DIR, g_cases[i]);

        char ao[MAX_PATH], ae[MAX_PATH], bo[MAX_PATH], be[MAX_PATH];
        snprintf(ao, sizeof ao, "%s/a.out", scratch);
        snprintf(ae, sizeof ae, "%s/a.err", scratch);
        snprintf(bo, sizeof bo, "%s/b.out", scratch);
        snprintf(be, sizeof be, "%s/b.err", scratch);

        /* Identical argv shape for both: argv[0], the script, nothing else.
         * bash gets --norc/--noprofile so no startup file can perturb it. */
        char *bash_argv[] = { (char *)"bash", (char *)"--norc",
                              (char *)"--noprofile", cpath, 0 };
        char *tsh_argv[]  = { (char *)"tsh", cpath, 0 };

        wipe_dir(wdir_a);
        int rc_a = run_shell(BASH_PATH, bash_argv, wdir_a, ao, ae);

        wipe_dir(wdir_b);
        int rc_b = run_shell(TSH_PATH, tsh_argv, wdir_b, bo, be);

        if (rc_a <= -100 || rc_b <= -100) {
            emitf("[shparity] %-28s BROKEN spawn rc bash=%d tsh=%d\n",
                  g_cases[i], rc_a, rc_b);
            broke++;
            continue;
        }

        long na = read_all(ao, g_buf_a, sizeof g_buf_a);
        long nb = read_all(bo, g_buf_b, sizeof g_buf_b);
        if (na < 0 || nb < 0) {
            emitf("[shparity] %-28s BROKEN capture bash=%ld tsh=%ld\n",
                  g_cases[i], na, nb);
            broke++;
            continue;
        }

        int same_out = (na == nb) && (memcmp(g_buf_a, g_buf_b, (size_t)na) == 0);
        int same_rc  = (rc_a == rc_b);

        if (same_out && same_rc) {
            emitf("[shparity] %-28s PASS  (%ld bytes, exit=%d)\n",
                  g_cases[i], na, rc_a);
            pass++;
        } else {
            emitf("[shparity] %-28s FAIL  stdout=%s exit bash=%d tsh=%d\n",
                  g_cases[i], same_out ? "same" : "DIFF", rc_a, rc_b);
            if (!same_out) show_diff(g_buf_a, na, g_buf_b, nb);
            fail++;
        }

        /* stderr is informational -- reported, never fatal. See header. But
         * its FIRST LINE is usually the actual cause ("[: not found" explains
         * a whole file of wrong branches), so print that rather than only a
         * byte count that leaves the reader guessing. */
        long ea_n = read_all(ae, g_buf_a, sizeof g_buf_a);
        long eb_n = read_all(be, g_buf_b, sizeof g_buf_b);
        if (ea_n > 0 || eb_n > 0) {
            errdiff++;
            emitf("[shparity]     (stderr bytes bash=%ld tsh=%ld -- informational)\n",
                  ea_n < 0 ? 0 : ea_n, eb_n < 0 ? 0 : eb_n);
            for (int side = 0; side < 2; side++) {
                const char *s = side ? g_buf_b : g_buf_a;
                long n = side ? eb_n : ea_n;
                if (n <= 0) continue;
                char first[120];
                size_t o = 0;
                for (long k = 0; k < n && s[k] != '\n' && o < sizeof first - 1; k++)
                    if ((unsigned char)s[k] >= 0x20) first[o++] = s[k];
                first[o] = '\0';
                emitf("[shparity]     %s stderr[0]: %s\n",
                      side ? "tsh " : "bash", first);
            }
        }
    }

    /* One line, one write: this is what the host harness greps for. */
    emitf("[SHPARITY] VERDICT: %s %d/%d pass fail=%d broken=%d stderr-cases=%d "
          "(stdout+exit must match GNU bash 5.2 exactly)\n",
          (fail == 0 && broke == 0) ? "PASS" : "FAIL",
          pass, g_ncases, fail, broke, errdiff);

    return (fail == 0 && broke == 0) ? 0 : 1;
}
