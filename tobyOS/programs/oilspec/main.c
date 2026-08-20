/* oilspec/main.c -- the THIRD-PARTY shell conformance gate: /bin/oilspec.
 *
 * WHY A SECOND SHELL GATE
 * -----------------------
 * /bin/shparity runs 54 cases we wrote ourselves. Cases you write yourself can
 * only find bugs you already suspected, and after enough rounds of "add a case,
 * fix it, go green" the corpus stops being evidence and becomes a description
 * of what we happen to do. This gate runs a corpus written by SOMEONE ELSE, for
 * a DIFFERENT shell, years before tobyOS existed: the Oils spec suite
 * (third_party/oils-spec), whose entire purpose is to pin down the corners
 * where bash, dash, mksh, ash and zsh disagree. Those corners are exactly the
 * ones a from-scratch shell gets wrong, and none of them were chosen by us.
 *
 * The mechanism is deliberately identical to shparity's -- same differential
 * comparison against the real GNU bash 5.2 in the initrd, same fresh-directory
 * discipline, same stdout+status contract. Only the corpus and its size differ.
 *
 * WHAT THE HOST DID FIRST
 * -----------------------
 * logs/oilspec_host.py has already run every case under real bash AND real
 * dash on the build machine, and split them:
 *
 *   POSIX      bash and dash agree exactly -- the POSIX core. This is the set
 *              the compliance score is computed over.
 *   BASH-ONLY  they disagree; tsh follows bash, per the superset contract.
 *   UNUSABLE   nondeterministic even across two runs of bash. Listed in
 *              /etc/oilspec/EXCLUDE and skipped HERE TOO, so a case that
 *              cannot decide anything cannot fail us either.
 *
 * That split lives on the host because it needs dash, which tobyOS does not
 * ship. This program does not know or care which class a case is in -- it
 * reports per-case results and the host script joins them back up. Keeping the
 * classification out of here means the guest cannot quietly grade itself.
 *
 * OUTPUT SHAPE, AND WHY IT IS A BITMAP
 * ------------------------------------
 * 2,700-odd cases cannot each get a line: the serial console is the bottleneck
 * on real hardware (one diagnostic once accounted for 84% of the log and cost
 * 38% of the browser's frame rate), and a gate that takes ten minutes to print
 * its own results is a gate nobody runs. So the complete per-case result goes
 * out as a compact MAP bitmap -- 64 cases per line, one character each -- and
 * only failures get prose, bounded. The bitmap is the full data set; the host
 * script decodes it against the manifest.
 *
 * A filter argument (`oilspec 0123`, `oilspec 0100-0199`) restricts the run to
 * matching ids WITH full diffs for every one. That is the iteration loop: find
 * the failures with a whole-corpus run, then re-run one band with detail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

#define CORPUS_DIR   "/etc/oilspec"
#define EXCLUDE_PATH "/etc/oilspec/EXCLUDE"
#define POSIX_PATH   "/etc/oilspec/POSIX"
/* Scratch on tmpfs for the same reason shparity uses it: the root filesystem
 * is the read-only initrd ramfs, and writing to the persistent volume let a
 * hard kill mid-run corrupt /data until the gate could only report SKIP. */
#define SCRATCH_DIR  "/tmp/oilspec"
#define SCRATCH_ALT  "/data/oilspec"
/* Shared $TMP for the cases. Fixed path so both shells see the same
 * string; wiped before each shell run so neither sees the other. */
#define SCRATCH_TMP  "/tmp/oilspec/t"
#define BASH_PATH    "/bin/bash"
#define TSH_PATH     "/bin/tsh"

#define MAX_CASES    4096
#define MAX_PATH     256
#define CAP_OUT      262144     /* per-stream capture cap */
#define MAP_COLS     64         /* cases per MAP line */
/* Prose budget for a full run. Raised from 60: with broken=0 the gate now
 * decides every case, and the bottleneck moved from "can it finish" to "how
 * many failures can I diagnose per 18-minute cycle". Spent on POSIX-class
 * cases only -- see g_posix -- because those are what the score is over. */
#define DETAIL_MAX   500

/* Captures live in .bss: two 256 KiB buffers do not fit on a user stack, and
 * a 66 KiB stack frame has already cost this project one debugging session. */
static char g_out_a[CAP_OUT];
static char g_out_b[CAP_OUT];
static char g_err_a[8192];
static char g_err_b[8192];

static char g_cases[MAX_CASES][32];     /* case id, e.g. "0123" */
static char g_result[MAX_CASES + 1];    /* the MAP bitmap */
static int  g_ncases;

static char g_excl[MAX_CASES][32];
static int  g_nexcl;

/* Ids the host oracle classified POSIX (real bash == real dash). Used only to
 * aim the diagnostic budget; the PASS/FAIL contract is identical either way,
 * so the guest still cannot grade itself. */
static char g_posix[MAX_CASES][32];
static int  g_nposix;

/* ---- output -------------------------------------------------------------
 *
 * One write() per line. The serial console splits a printf at its format
 * conversions, so a line assembled from several printf calls arrives with
 * other logging interleaved -- and a whole-line grep on the host then matches
 * nothing while the run underneath was fine. That trap has been hit three
 * times in this tree; one write per line is what makes the greps trustworthy. */
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

/* ---- small helpers ------------------------------------------------------ */

static int has_suffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

static long read_all(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    for (;;) {
        if (got >= cap) { close(fd); return -2; }
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    return (long)got;
}

/* Delete every regular file in `dir`, one level deep, then the subdirectories
 * that are now empty. Unlike shparity's corpus, these cases were written by
 * someone else and some of them DO mkdir, so a flat sweep would leave state
 * behind for the next case -- the exact cross-contamination the fresh-directory
 * rule exists to prevent. */
static int wipe_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int left = 0;
    char subs[64][MAX_PATH];
    int nsub = 0;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (de->d_type == DT_DIR) {
            if (nsub < 64) snprintf(subs[nsub++], MAX_PATH, "%s", p);
            continue;
        }
        if (unlink(p) != 0) left++;
    }
    closedir(d);
    for (int i = 0; i < nsub; i++) {
        (void)wipe_dir(subs[i]);
        if (rmdir(subs[i]) != 0) left++;
    }
    return left;
}

/* ---- bounded wait -------------------------------------------------------
 *
 * A blocking waitpid() means ONE case can end the run. That is not a
 * hypothetical: tsh's printf reuse loop spun forever on case 0210, and case
 * 0468 (three background subshells and a `wait`) wedged the next attempt 2,300
 * cases short. Both times the run died silently and cost a fifteen-minute
 * round trip to find out where.
 *
 * There is no way to pre-guarantee that 2,776 scripts written for another
 * shell all terminate under this one -- that is precisely what the gate is
 * measuring -- so the gate has to assume they might not. A case that outstays
 * its deadline is killed and recorded as a TIMEOUT, which is a RESULT: the
 * corpus says bash finishes it, so tsh not finishing it is a real difference,
 * not an excuse to drop the case.
 *
 * The bash side gets the same deadline. If the oracle itself hangs, that is a
 * kernel bug and must be visible rather than being charged to tsh. */
#define CASE_TIMEOUT_MS 5000

static long ms_since(const struct timespec *t0) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (long)(now.tv_sec - t0->tv_sec) * 1000L +
           (long)(now.tv_nsec - t0->tv_nsec) / 1000000L;
}

static int wait_bounded(pid_t pid, int timeout_ms) {
    struct timespec t0;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        /* No clock: fall back to the blocking wait rather than spin forever
         * on a deadline that can never be reached. */
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return -105;
        return WEXITSTATUS(status);
    }

    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WEXITSTATUS(status);
        if (r < 0) return -105;

        if (ms_since(&t0) > timeout_ms) {
            kill(pid, SIGKILL);
            /* Reap it, but do not block on the reap either: a child that will
             * not die must not become the hang we just removed. */
            struct timespec t1;
            (void)clock_gettime(CLOCK_MONOTONIC, &t1);
            while (ms_since(&t1) < 1000) {
                if (waitpid(pid, &status, WNOHANG) == pid) break;
                struct timespec nap = { 0, 1000 * 1000 };
                nanosleep(&nap, 0);
            }
            return -106;                       /* the TIMEOUT result */
        }
        /* 1 ms, NOT 2. sys_nanosleep parks the caller (state=PROC_BLOCKED,
         * woken by the scheduler sweep) at >= 2 ms and spins with sched_yield
         * below it. The gate is often the ONLY runnable process -- its child
         * has just exited -- and the park did not wake: the run froze after
         * the first case whose child had not finished by the first poll.
         * Staying under the threshold keeps this on the path that always
         * returns. (The park not waking with an otherwise idle machine is
         * worth its own look; it is not this gate's problem to solve.) */
        struct timespec nap = { 0, 1000 * 1000 };
        nanosleep(&nap, 0);
    }
}

/* ---- running one shell over one case ------------------------------------ */

static int run_shell(const char *shell, char *const argv[], const char *workdir,
                     const char *out, const char *err, int devnull) {
    /* PATH puts the corpus's own helper binaries first: the cases call
     * argv.py/printenv.py, which programs/spechelp provides under those names.
     * BOTH shells get the same PATH, so the helper cannot favour either. */
    /* TMP is part of the corpus's contract: 135 cases write scratch files
     * under $TMP, and with it unset they resolved to "/..." on the read-only
     * initrd root and failed with "Read-only file system". The Oils harness
     * sets it; so must this one.
     *
     * The SAME string for both shells, deliberately -- a case that echoes
     * $TMP must print identical bytes under bash and tsh, so this cannot be
     * per-shell. It is wiped before EACH shell runs (see the main loop), so
     * the two still cannot see each other's files. */
    static char *const envp[] = {
        (char *)"PATH=/etc/oilspec/bin:/bin",
        (char *)"HOME=/",
        (char *)"TERM=dumb",
        (char *)"LC_ALL=C",
        (char *)"TMP=" SCRATCH_TMP,
        (char *)"TMPDIR=" SCRATCH_TMP,
        (char *)"TSH_FDTRACE=1",
        0
    };

    int fo = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fo < 0) return -101;
    int fe = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fe < 0) { close(fo); return -102; }

    char saved[MAX_PATH];
    if (!getcwd(saved, sizeof saved)) saved[0] = '\0';
    if (chdir(workdir) != 0) { close(fo); close(fe); return -103; }

    /* stdin is /dev/null, not the console. A case that reads stdin would
     * otherwise block on a keyboard that is never going to type, and hang the
     * whole gate somewhere in the middle with no indication of where. The host
     * oracle redirects stdin the same way, so both see EOF. */
    pid_t pid = toby_spawn(shell, argv, envp, devnull, fo, fe);

    if (saved[0]) (void)chdir(saved);
    close(fo);
    close(fe);
    if (pid < 0) return -104;
    return wait_bounded(pid, CASE_TIMEOUT_MS);
}

/* ---- diffing ------------------------------------------------------------ */

#define DIFF_MAX_LINES 24

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

/* Show the differing lines. Bounded twice over -- per case and per run -- so a
 * corpus that goes wholly divergent cannot bury the verdict under its own
 * diagnostics. */
static void show_diff(const char *a, long na, const char *b, long nb) {
    int la = count_lines(a, na), lb = count_lines(b, nb);
    emitf("[oilspec]     lines bash=%d tsh=%d\n", la, lb);
    int shown = 0;
    int most = (la > lb) ? la : lb;
    for (int n = 0; n < most && shown < DIFF_MAX_LINES; n++) {
        char ta[140], tb[140];
        int ra = line_at(a, na, n, ta, sizeof ta);
        int rb = line_at(b, nb, n, tb, sizeof tb);
        if (ra < 0) ta[0] = '\0';
        if (rb < 0) tb[0] = '\0';
        if (ra == rb && strcmp(ta, tb) == 0) continue;
        emitf("[oilspec]     L%-3d bash: %s\n", n + 1, ra < 0 ? "<none>" : ta);
        emitf("[oilspec]     L%-3d tsh : %s\n", n + 1, rb < 0 ? "<none>" : tb);
        shown++;
    }
    if (shown >= DIFF_MAX_LINES)
        emitf("[oilspec]     ... further differences suppressed\n");
}

/* Free memory, in MiB, from /proc/meminfo.
 *
 * The run died at case ~2700 with pmm_alloc_page() OOM and 4 MiB free of
 * 5120: something leaks per spawn, and 5,400 spawns is enough to notice what
 * ordinary use never would. A single number sampled every N cases turns "it
 * ran out" into a RATE, which is the difference between knowing there is a
 * leak and knowing how big it is. */
static long meminfo_kb(const char *label) {
    char buf[1024];
    long n = read_all("/proc/meminfo", buf, sizeof buf - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    const char *p = strstr(buf, label);
    if (!p) return -1;
    p += strlen(label);
    while (*p == ' ') p++;
    long kb = 0;
    while (*p >= '0' && *p <= '9') kb = kb * 10 + (*p++ - '0');
    return kb;
}

/* ---- resource-exhaustion diagnosis --------------------------------------
 *
 * The first whole-corpus run died at case 0128 with every later case reporting
 * "could not run": open() on the capture files began failing and never
 * recovered. 128 is not a coincidence -- the gate opens exactly 8 files per
 * case (2 opendir + 4 capture + 2 read-back), and PROC_NFDS is 1024, while
 * tmpfs allows TMPFS_MAX_NODES=512 and the gate creates 4 files per case. Both
 * arithmetics land on 128, so reading the code cannot tell them apart.
 *
 * This runs ONCE, at the first failure, and separates them: descriptors and
 * filesystem nodes are exhausted by different probes, and a probe that still
 * works rules its resource out. Printing "it broke" without this would leave
 * the next person doing the same arithmetic. */
static void diagnose_exhaustion(const char *scratch, const char *dir_a,
                                const char *dir_b) {
    emit("[oilspec] ---- resource diagnosis (first failure) ----\n");

    /* 1. Descriptors. Reading an initrd file needs an fd but no new node, so
     *    this fails only if the fd table is full. */
    int probe[80];
    int nfd = 0;
    while (nfd < 80) {
        probe[nfd] = open(EXCLUDE_PATH, O_RDONLY);
        if (probe[nfd] < 0) break;
        nfd++;
    }
    emitf("[oilspec]   spare fds (open initrd file, no new node): %d%s\n",
          nfd, nfd ? "" : "   <-- FD TABLE EXHAUSTED");
    for (int i = 0; i < nfd; i++) close(probe[i]);

    /* 2. Nodes. Creating a NEW name needs a node but only one fd, which the
     *    probe above just proved we have. */
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s/probe.tmp", scratch);
    int f = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    emitf("[oilspec]   create a NEW file in scratch: fd=%d%s\n",
          f, f < 0 ? "   <-- FILESYSTEM NODES EXHAUSTED" : "");
    if (f >= 0) { close(f); unlink(p); }

    /* 3. Re-open an EXISTING capture file. Creating a NEW name needs the
     *    PARENT DIRECTORY to exist; re-opening an existing path does not.
     *    So (2) failing while this works does not mean "out of nodes" -- it
     *    equally means the scratch DIRECTORY has stopped existing, and the
     *    first version of this probe read the first meaning into it and sent
     *    the investigation after a node leak that was never there. */
    snprintf(p, sizeof p, "%s/out.a", scratch);
    f = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    emitf("[oilspec]   re-open an EXISTING capture file: fd=%d\n", f);
    if (f >= 0) close(f);

    /* 3b. Is the scratch directory still a directory? */
    DIR *sd = opendir(scratch);
    emitf("[oilspec]   opendir(scratch)=%s\n", sd ? "OK" : "FAILED  <-- SCRATCH DIR IS GONE");
    if (sd) closedir(sd);
    struct stat st;
    if (stat(scratch, &st) == 0)
        emitf("[oilspec]   stat(scratch): mode=0%o isdir=%d\n",
              (unsigned)(st.st_mode & 07777), S_ISDIR(st.st_mode) ? 1 : 0);
    else
        emit("[oilspec]   stat(scratch): FAILED  <-- SCRATCH DIR IS GONE\n");

    /* 3c. A new name one level UP, in the tmpfs root. If this works while (2)
     *     fails, the mount is fine and only our directory is broken. */
    int f2 = open("/tmp/oilspec-probe.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    emitf("[oilspec]   create a NEW file in /tmp root: fd=%d%s\n", f2,
          f2 >= 0 ? "   <-- mount is fine; the SCRATCH DIR is the problem" : "");
    if (f2 >= 0) { close(f2); unlink("/tmp/oilspec-probe.tmp"); }

    /* 3d. And are the per-shell working directories still there? They are
     *     what the cases actually run in, and a case that deleted its own cwd
     *     would take them out. */
    DIR *da = opendir(dir_a), *db = opendir(dir_b);
    emitf("[oilspec]   opendir(a)=%s opendir(b)=%s\n",
          da ? "OK" : "FAILED", db ? "OK" : "FAILED");
    if (da) closedir(da);
    if (db) closedir(db);

    /* 4. What the sweep could not remove. wipe_dir returns a survivor count
     *    that the main loop ignores; if the cases are leaving files behind,
     *    every one of them is a node that never comes back. */
    emitf("[oilspec]   wipe_dir survivors: a=%d b=%d\n",
          wipe_dir(dir_a), wipe_dir(dir_b));
    emit("[oilspec] ---- end diagnosis ----\n");
}

/* ---- corpus discovery --------------------------------------------------- */

/* Load a one-id-per-line list ('#' comments; anything after a space ignored)
 * into `dst`. Shared by EXCLUDE and POSIX so the two cannot drift in how they
 * are parsed -- the same host script writes both from the same JSON. */
static int load_id_list(const char *path, char dst[][32], int cap) {
    static char buf[32768];
    long n = read_all(path, buf, sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    int count = 0;
    char *p = buf;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        if (*p && *p != '#' && count < cap) {
            char *sp = strchr(p, ' ');
            if (sp) *sp = '\0';
            /* Trim trailing whitespace, and CR in particular.
             *
             * The host writes these two files from Windows Python in text
             * mode, so every line ends "\r\n". EXCLUDE survived that by pure
             * luck -- its lines are "NNNN reason", and cutting at the space
             * took the CR with it. POSIX lines are a bare id, so the CR stayed
             * on, every id read as "0002\r", is_posix() matched NOTHING, and a
             * whole-corpus run silently produced ZERO per-case diffs while
             * still scoring correctly. The score looked healthy and the one
             * thing the run existed to produce was missing. Trim here rather
             * than only at the writer: this loader is the last place both
             * lists pass through. */
            size_t len = strlen(p);
            while (len && (p[len - 1] == '\r' || p[len - 1] == '\n' ||
                           p[len - 1] == '\t' || p[len - 1] == ' '))
                p[--len] = '\0';
            if (*p) snprintf(dst[count++], 32, "%s", p);
        }
        if (!eol) break;
        p = eol + 1;
    }
    return count;
}

static void load_exclude(void) {
    g_nexcl  = load_id_list(EXCLUDE_PATH, g_excl,  MAX_CASES);
    g_nposix = load_id_list(POSIX_PATH,   g_posix, MAX_CASES);
}

static int is_excluded(const char *id) {
    for (int i = 0; i < g_nexcl; i++)
        if (!strcmp(g_excl[i], id)) return 1;
    return 0;
}

static int is_posix(const char *id) {
    for (int i = 0; i < g_nposix; i++)
        if (!strcmp(g_posix[i], id)) return 1;
    return 0;
}

/* `filter` is empty (everything), an exact id, or a "LO-HI" band. Matching on
 * the id rather than the case name keeps the argument short enough to type at
 * a serial console. */
static int matches(const char *id, const char *filter) {
    if (!filter || !*filter) return 1;
    const char *dash = strchr(filter, '-');
    if (dash) {
        char lo[32];
        size_t n = (size_t)(dash - filter);
        if (n >= sizeof lo) n = sizeof lo - 1;
        memcpy(lo, filter, n); lo[n] = '\0';
        return strcmp(id, lo) >= 0 && strcmp(id, dash + 1) <= 0;
    }
    return !strcmp(id, filter);
}

static void collect_cases(const char *filter) {
    DIR *d = opendir(CORPUS_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_ncases < MAX_CASES) {
        if (!has_suffix(de->d_name, ".sh")) continue;
        char id[32];
        snprintf(id, sizeof id, "%s", de->d_name);
        id[strlen(id) - 3] = '\0';          /* strip .sh */
        if (!matches(id, filter)) continue;
        snprintf(g_cases[g_ncases++], 32, "%s", id);
    }
    closedir(d);
    /* Insertion sort: readdir order is the filesystem's, and the MAP bitmap is
     * positional -- an unsorted run would silently mis-attribute every result
     * to the wrong case. */
    for (int i = 1; i < g_ncases; i++) {
        char key[32];
        snprintf(key, 32, "%s", g_cases[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(g_cases[j], key) > 0) {
            snprintf(g_cases[j + 1], 32, "%s", g_cases[j]);
            j--;
        }
        snprintf(g_cases[j + 1], 32, "%s", key);
    }
}

/* ---- main --------------------------------------------------------------- */

/* The filter comes from a FILE, not from argv or a -D.
 *
 * It used to be `-DOILSPEC_FILTER="LO-HI"` baked into the kernel hook. The
 * quotes did not survive make -> sh -> clang, so the macro expanded to the
 * INTEGER EXPRESSION 0001-0200 and `(char *)OILSPEC_FILTER` became
 * (char *)1 - 200: a wild pointer, dereferenced on the first line of main()
 * before anything was printed. The gate loaded and died in silence, which
 * reads exactly like a hang. Reading a file has no quoting layers to lose. */
static void load_filter(char *out, size_t cap) {
    out[0] = '\0';
    int fd = open(CORPUS_DIR "/FILTER", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) { out[0] = '\0'; return; }
    out[n] = '\0';
    for (char *p = out; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == ' ') { *p = '\0'; break; }
}

int main(void) {
    char filter_buf[64];
    load_filter(filter_buf, sizeof filter_buf);
    const char *filter = filter_buf;
    int detail_all = (filter && *filter);   /* a filtered run is a debug run */

    emit("[oilspec] ==== third-party shell conformance gate (Oils spec suite) ====\n");

    int fd = open(BASH_PATH, O_RDONLY);
    if (fd < 0) {
        emit("[OILSPEC] VERDICT: SKIP reason=no-bash "
             "(need programs/realbash/bash staged as /bin/bash)\n");
        return 0;
    }
    close(fd);
    fd = open(TSH_PATH, O_RDONLY);
    if (fd < 0) {
        emit("[OILSPEC] VERDICT: SKIP reason=no-tsh (/bin/tsh not staged)\n");
        return 0;
    }
    close(fd);

    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) {
        /* Not fatal, but say so: with the console on stdin instead, a case
         * that reads stdin hangs the gate rather than failing it. */
        emit("[oilspec] WARNING: no /dev/null -- stdin is the console, a case "
             "that reads stdin will HANG this run\n");
        devnull = 0;
    }

    load_exclude();
    collect_cases(filter);
    if (g_ncases == 0) {
        emitf("[OILSPEC] VERDICT: SKIP reason=no-corpus filter='%s' "
              "(expected %s/NNNN.sh)\n", filter, CORPUS_DIR);
        return 0;
    }

    const char *scratch = 0;
    static const char *const candidates[] = { SCRATCH_DIR, SCRATCH_ALT, 0 };
    for (int c = 0; candidates[c] && !scratch; c++) {
        if (mkdir(candidates[c], 0755) == 0) { scratch = candidates[c]; break; }
        DIR *probe = opendir(candidates[c]);
        if (probe) { closedir(probe); scratch = candidates[c]; }
    }
    if (!scratch) {
        emit("[OILSPEC] VERDICT: SKIP reason=no-scratch "
             "(need a writable /tmp or /data; / is the read-only initrd)\n");
        return 0;
    }

    /* ONE working directory, not one per shell.
     *
     * The two shells used to run in `.../a` and `.../b`, wiped before each
     * case. That is one directory name too many: a case that PRINTS where it
     * is could never match.
     *
     *     basename $(pwd)      bash: a      tsh: b
     *
     * -- a difference manufactured by the harness and reported as a shell
     * divergence. What actually prevents one shell from seeing the other's
     * leftovers is the WIPE, and the wipe is unchanged; it simply happens
     * twice per case now, once before each shell, exactly as it already did
     * for the shared $TMP. */
    char dir_w[MAX_PATH];
    snprintf(dir_w, sizeof dir_w, "%s/w", scratch);
    mkdir(dir_w, 0755);
    mkdir(SCRATCH_TMP, 0755);        /* the $TMP the cases write into */

    char out_a[MAX_PATH], out_b[MAX_PATH], err_a[MAX_PATH], err_b[MAX_PATH];
    snprintf(out_a, sizeof out_a, "%s/out.a", scratch);
    snprintf(out_b, sizeof out_b, "%s/out.b", scratch);
    snprintf(err_a, sizeof err_a, "%s/err.a", scratch);
    snprintf(err_b, sizeof err_b, "%s/err.b", scratch);

    emitf("[oilspec] corpus=%d cases excluded=%d posixlist=%d scratch=%s filter='%s'\n",
          g_ncases, g_nexcl, g_nposix, scratch, filter);
    /* A whole-corpus run produced ZERO per-case diffs once: the detail budget
     * is aimed with is_posix(), and if that list is empty the aim silently
     * excludes everything rather than including everything. The count above
     * makes the load visible; this makes the consequence loud, and the
     * fallback below keeps a full run diagnostic instead of merely scored. */
    if (g_nposix == 0)
        emit("[oilspec] WARNING: POSIX id list is EMPTY -- per-case detail "
             "cannot be aimed at the compliance subset; detailing failures "
             "UNAIMED up to the budget\n");

    int pass = 0, fail = 0, skip = 0, broken = 0, detail = 0;
    int diagnosed = 0;
    int recreated_a = 0, recreated_b = 0;

    for (int i = 0; i < g_ncases; i++) {
        const char *id = g_cases[i];
        g_result[i] = '?';

        /* Progress + free memory, every 200 cases. The old progress line sat
         * at the bottom of the FAILURE branch, so it only printed when a
         * failing case happened to land on the boundary -- most runs showed no
         * progress at all, which is why a wedged run looked like a silent one.
         * freeMiB turns "it ran out of memory at case 2700" into a rate. */
        if ((i % 200) == 0)
            emitf("[oilspec] ... %d/%d pass=%d fail=%d freeMiB=%ld "
                  "heapUsedKiB=%ld heapTotalKiB=%ld allocs=%ld frees=%ld\n",
                  i, g_ncases, pass, fail,
                  meminfo_kb("MemFree:") / 1024,
                  meminfo_kb("HeapUsed:"), meminfo_kb("HeapTotal:"),
                  meminfo_kb("HeapAllocs:"), meminfo_kb("HeapFrees:"));

        if (is_excluded(id)) { g_result[i] = 'S'; skip++; continue; }

        char casepath[MAX_PATH];
        snprintf(casepath, sizeof casepath, "%s/%s.sh", CORPUS_DIR, id);

        char *av_bash[] = { (char *)"bash", (char *)"--norc", (char *)"--noprofile",
                            casepath, 0 };
        char *av_tsh[]  = { (char *)"tsh", casepath, 0 };

        /* The working directory and $TMP are both shared, so both are wiped
         * BETWEEN the two shells as well as between cases -- otherwise bash's
         * leftovers would be visible to tsh and the second shell could look
         * correct because the first one did the work.
         *
         * The re-create counters stay because the corpus was written by
         * someone else and some cases remove directories, including the one
         * they are running in. Losing the working directory used to end the
         * run with every later case reporting "could not run", which reads
         * like the filesystem broke rather than like one case deleted a
         * directory. */
        wipe_dir(dir_w);
        if (mkdir(dir_w, 0755) == 0) recreated_a++;
        wipe_dir(SCRATCH_TMP); mkdir(SCRATCH_TMP, 0755);
        int rc_a = run_shell(BASH_PATH, av_bash, dir_w, out_a, err_a, devnull);

        wipe_dir(dir_w);
        if (mkdir(dir_w, 0755) == 0) recreated_b++;
        wipe_dir(SCRATCH_TMP); mkdir(SCRATCH_TMP, 0755);
        int rc_b = run_shell(TSH_PATH,  av_tsh,  dir_w, out_b, err_b, devnull);

        /* A timeout is a RESULT, not a broken run: the corpus's own oracle
         * finishes this case, so tsh failing to finish it is a real
         * difference. Kept separate from E so a hang can never be read as a
         * harness fault -- and so the count of them is visible in the verdict. */
        if (rc_a == -106 || rc_b == -106) {
            g_result[i] = 'T'; fail++;
            emitf("[oilspec] TIMEOUT %s after %dms bash=%s tsh=%s\n", id,
                  CASE_TIMEOUT_MS,
                  rc_a == -106 ? "HUNG" : "ok",
                  rc_b == -106 ? "HUNG" : "ok");
            /* Whatever it managed before it hung. A timeout used to report
             * nothing at all, which made a hang the one failure mode with no
             * evidence attached -- exactly the one that needs it most. */
            {
                long pn = read_all(out_b, g_out_b, sizeof g_out_b);
                if (pn > 0) {
                    char t[140];
                    for (int ln = 0; ln < 6; ln++) {
                        if (line_at(g_out_b, pn, ln, t, sizeof t) != 0) break;
                        emitf("[oilspec]     tsh partial stdout: %s\n", t);
                    }
                }
                long pe = read_all(err_b, g_err_b, sizeof g_err_b);
                if (pe > 0) {
                    char t[140];
                    for (int ln = 0; ln < 8; ln++) {
                        if (line_at(g_err_b, pe, ln, t, sizeof t) != 0) break;
                        if (!t[0]) continue;
                        if (strstr(t, "kernel-only, not available")) continue;
                        if (strstr(t, "[_exit] code=")) continue;
                        emitf("[oilspec]     tsh partial stderr: %s\n", t);
                    }
                }
            }
            continue;
        }
        if (rc_a < -100 || rc_b < -100) {
            g_result[i] = 'E'; broken++;
            emitf("[oilspec] BROKEN %s could-not-run bash=%d tsh=%d\n", id, rc_a, rc_b);
            if (!diagnosed) { diagnosed = 1; diagnose_exhaustion(scratch, dir_w, dir_w); }
            continue;
        }

        long na = read_all(out_a, g_out_a, sizeof g_out_a);
        long nb = read_all(out_b, g_out_b, sizeof g_out_b);
        if (na < 0 || nb < 0) {
            g_result[i] = 'E'; broken++;
            emitf("[oilspec] BROKEN %s capture-failed a=%ld b=%ld\n", id, na, nb);
            continue;
        }

        int same_out = (na == nb) && (memcmp(g_out_a, g_out_b, (size_t)na) == 0);
        int same_rc  = (rc_a == rc_b);

        if (same_out && same_rc) { g_result[i] = 'P'; pass++; continue; }

        g_result[i] = same_rc ? 'O' : (same_out ? 'X' : 'D');
        fail++;

        /* Diagnose POSIX-class failures first: they are what the compliance
         * number is over, and the serial budget is finite. A filtered run
         * still details everything -- that is the debug path. */
        if (detail_all || (detail < DETAIL_MAX && (g_nposix == 0 || is_posix(id)))) {
            detail++;
            emitf("[oilspec] FAIL %s  stdout=%s exit bash=%d tsh=%d\n",
                  id, same_out ? "same" : "DIFF", rc_a, rc_b);
            if (!same_out) show_diff(g_out_a, na, g_out_b, nb);
            /* stderr is reported, never required to match: a diagnostic names
             * the shell that produced it, so "bash: line 3:" can never equal a
             * tsh message and requiring equality would bake in a failure we
             * would then be tempted to paper over. */
            long ea = read_all(err_a, g_err_a, sizeof g_err_a);
            long eb = read_all(err_b, g_err_b, sizeof g_err_b);
            if (eb > 0) {
                /* The FIRST INFORMATIVE line, not simply the first.
                 *
                 * tsh emits a kstub notice ("signal_set_foreground:
                 * kernel-only") on every foreground spawn and libtoby traces
                 * its own exit. Reporting stderr[0] verbatim made that notice
                 * the reported cause for 68 POSIX failures at once -- they
                 * clustered together and looked like one bug, when it was one
                 * piece of noise standing in front of 68 different ones. */
                char t[140];
                for (int ln = 0; ln < 12; ln++) {
                    if (line_at(g_err_b, eb, ln, t, sizeof t) != 0) break;
                    if (!t[0]) continue;
                    if (strstr(t, "kernel-only, not available")) continue;
                    if (strstr(t, "[_exit] code=")) continue;
                    emitf("[oilspec]     tsh stderr: %s\n", t);
                    break;
                }
            }
            if (ea > 0 && eb <= 0)
                emitf("[oilspec]     (bash had stderr, tsh silent)\n");
        } else if (detail == DETAIL_MAX) {
            detail++;
            emitf("[oilspec] (further per-case detail suppressed -- "
                  "re-run `oilspec NNNN` or `oilspec LO-HI` for a band)\n");
        }

    }

    g_result[g_ncases] = '\0';

    /* The complete result set, 64 cases per line. This is the data the host
     * script joins against the manifest to produce the per-feature census;
     * the prose above is only for eyeballing a run in progress.
     *   P pass   O stdout differs   X exit differs   D both differ
     *   E could not run   S excluded by the host oracle */
    emit("[oilspec] MAP legend P=pass O=stdout-diff X=exit-diff D=both "
         "T=timeout E=broken S=skipped\n");
    for (int i = 0; i < g_ncases; i += MAP_COLS) {
        char chunk[MAP_COLS + 1];
        int n = g_ncases - i;
        if (n > MAP_COLS) n = MAP_COLS;
        memcpy(chunk, g_result + i, (size_t)n);
        chunk[n] = '\0';
        emitf("[oilspec] MAP %s %s\n", g_cases[i], chunk);
    }

    int decided = pass + fail;
    if (recreated_a || recreated_b)
        emitf("[oilspec] working dir re-created mid-run: bash=%d tsh=%d "
              "(cases that deleted their own working directory)\n",
              recreated_a, recreated_b);

    emitf("[OILSPEC] VERDICT: %s %d/%d pass fail=%d skipped=%d broken=%d "
          "(stdout+exit must match GNU bash 5.2 exactly)\n",
          fail == 0 && broken == 0 ? "PASS" : "FAIL",
          pass, decided, fail, skip, broken);
    return (fail == 0 && broken == 0) ? 0 : 1;
}
