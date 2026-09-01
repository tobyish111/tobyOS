/* head -- output first N lines of files.
 *
 * Modeled on sbase's head.c (ISC license). Subset:
 *
 *     head [-c N] [-n N] [-qv] [file ...]
 *
 *   -n N   print the first N lines (default 10)
 *   -n -N  print all BUT the last N lines
 *   -c N   the same, counted in bytes
 *
 * A NEGATIVE count used to be rejected outright ("if (n_lines < 0) usage()"),
 * which turned a valid GNU request into a usage error. It is the mirror of
 * the bug tail had with `-n +2`; both are now handled.
 *
 * Multiple files print "==> name <==" headers between them, like
 * GNU/sbase. With no arguments head reads from stdin. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static int  n_lines = 10;
static long n_bytes = -1;   /* -c: bytes instead of lines */
static int  neg;            /* the -N forms mean "all but the last N" */
static int  qflag, vflag;

static int head_but_last_lines(int fd, long n);
static int head_but_last_bytes(int fd, long n);

static int head_fd(int fd, const char *label) {
    char  buf[4096];
    if (neg)
        return n_bytes >= 0 ? head_but_last_bytes(fd, n_bytes)
                            : head_but_last_lines(fd, n_lines);
    /* -c counts BYTES, not lines. GNU has had it forever and scripts use it
     * to peek at binary headers; without it `head -c 11 file` was a usage
     * error. */
    if (n_bytes >= 0) {
        long left_b = n_bytes;
        while (left_b > 0) {
            size_t want = (size_t)(left_b < (long)sizeof(buf) ? left_b : (long)sizeof(buf));
            ssize_t n = read(fd, buf, want);
            if (n == 0) break;
            if (n < 0) { weprintf("read %s:", label); return -1; }
            if (write(1, buf, (size_t)n) < 0) { weprintf("write:"); return -1; }
            left_b -= n;
        }
        return 0;
    }
    int   left = n_lines;
    for (;;) {
        if (left <= 0) return 0;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) { weprintf("read %s:", label); return -1; }

        ssize_t emit = 0;
        for (ssize_t i = 0; i < n && left > 0; i++) {
            if (buf[i] == '\n') {
                ssize_t w = write(1, buf + emit, (size_t)(i - emit + 1));
                if (w < 0) { weprintf("write:"); return -1; }
                emit = i + 1;
                left--;
            }
        }
        /* Trailing partial line (no newline) only if we haven't
         * yet hit our limit -- otherwise stop. */
        if (left > 0 && emit < n) {
            ssize_t w = write(1, buf + emit, (size_t)(n - emit));
            if (w < 0) { weprintf("write:"); return -1; }
        }
    }
}


/* ---- the negative forms: hold back the last N and print the rest ----
 * Both need a ring, because "all but the last N" cannot be known until the
 * input ends. */

static int head_but_last_lines(int fd, long n) {
    if (n <= 0) { char b[4096]; ssize_t k;
                  while ((k = read(fd, b, sizeof b)) > 0)
                      if (write(1, b, (size_t)k) < 0) return -1;
                  return 0; }
    char **ring = (char **)calloc((size_t)n, sizeof(char *));
    if (!ring) return -1;
    long pos = 0, held = 0;
    char line[8192];
    size_t ll = 0;
    char b[4096];
    ssize_t k;
    int rc = 0;
    for (;;) {
        k = read(fd, b, sizeof b);
        if (k < 0) { rc = -1; break; }
        for (ssize_t i = 0; i <= (k ? k - 1 : -1); i++) {
            if (ll + 1 < sizeof line) line[ll++] = b[i];
            if (b[i] != '\n') continue;
            line[ll] = 0;
            if (held == n) {   /* the oldest held line is now safe to print */
                char *old = ring[pos];
                if (old) { if (write(1, old, strlen(old)) < 0) rc = -1; free(old); }
            } else held++;
            ring[pos] = (char *)malloc(ll + 1);
            if (ring[pos]) memcpy(ring[pos], line, ll + 1);
            pos = (pos + 1) % n;
            ll = 0;
        }
        if (k == 0) {
            /* A final line with no newline is still a line, and must enter
             * the ring or it would be printed when it belongs to the
             * held-back tail. */
            if (ll > 0) {
                line[ll] = 0;
                if (held == n) {
                    char *old = ring[pos];
                    if (old) { if (write(1, old, strlen(old)) < 0) rc = -1; free(old); }
                } else held++;
                ring[pos] = (char *)malloc(ll + 1);
                if (ring[pos]) memcpy(ring[pos], line, ll + 1);
                pos = (pos + 1) % n;
            }
            break;
        }
    }
    for (long i = 0; i < n; i++) free(ring[i]);
    free(ring);
    return rc;
}

static int head_but_last_bytes(int fd, long n) {
    if (n <= 0) { char b[4096]; ssize_t k;
                  while ((k = read(fd, b, sizeof b)) > 0)
                      if (write(1, b, (size_t)k) < 0) return -1;
                  return 0; }
    char *ring = (char *)malloc((size_t)n);
    if (!ring) return -1;
    long pos = 0, held = 0;
    char b[4096];
    ssize_t k;
    int rc = 0;
    while ((k = read(fd, b, sizeof b)) > 0) {
        for (ssize_t i = 0; i < k; i++) {
            if (held == n) { if (write(1, &ring[pos], 1) < 0) { rc = -1; break; } }
            else held++;
            ring[pos] = b[i];
            pos = (pos + 1) % n;
        }
        if (rc) break;
    }
    if (k < 0) rc = -1;
    free(ring);
    return rc;
}

static void usage(void) { eprintf("usage: %s [-n lines] [-c bytes] [-qv] [file ...]", argv0); }

int main(int argc, char *argv[]) {
    util_argv0_set(argv[0]);

    /* GNU accepts `head -5` as shorthand for `head -n 5`, and scripts use it
     * constantly (`chage -l alice | head -2`). getopt() cannot express a
     * numeric option, so consume it before getopt sees it.
     *
     * The argument is REMOVED rather than rewritten into "-n" plus a value:
     * inserting needs one slot more than argv has -- argv[argc] is the NULL
     * terminator and nothing past it is ours -- so growing it writes out of
     * bounds. Shrinking is always safe. (tail had the same gap and the same
     * trap; see programs/user_tail/main.c.) */
    for (int i = 1; i < argc; ) {
        const char *a = argv[i];
        /* `-2` here may be the VALUE of a preceding -n/-c rather than the
         * `head -2` shorthand. Consuming it left `-n` with no argument, so
         * `head -n -2` silently became `head five` -- it printed nothing.
         * Only treat it as the shorthand when nothing is expecting it. */
        const char *pv = argv[i - 1];
        if (i > 1 && (!strcmp(pv, "-n") || !strcmp(pv, "-c"))) { i++; continue; }
        if (a[0] != '-' || a[1] < '0' || a[1] > '9') { i++; continue; }
        int ok = 1, v = 0;
        for (const char *q = a + 1; *q; q++) {
            if (*q < '0' || *q > '9') { ok = 0; break; }
            v = v * 10 + (*q - '0');
        }
        if (!ok) { i++; continue; }
        n_lines = v;
        for (int k = i; k < argc; k++) argv[k] = argv[k + 1];   /* NULL moves too */
        argc--;
    }

    int opt;
    while ((opt = getopt(argc, argv, "n:c:qv")) != -1) {
        switch (opt) {
        case 'n': if (optarg[0] == '-') { neg = 1; n_lines = atoi(optarg + 1); }
                  else n_lines = atoi(optarg);
                  if (n_lines < 0) usage();
                  break;
        case 'c': if (optarg[0] == '-') { neg = 1; n_bytes = atol(optarg + 1); }
                  else n_bytes = atol(optarg);
                  if (n_bytes < 0) usage();
                  break;
        case 'q': qflag = 1; vflag = 0; break;
        case 'v': vflag = 1; qflag = 0; break;
        default:  usage();
        }
    }

    int rc      = 0;
    int nfiles  = argc - optind;
    int show_hd = vflag || (nfiles > 1 && !qflag);

    if (nfiles <= 0) return head_fd(0, "<stdin>") < 0 ? 1 : 0;

    for (int i = optind; i < argc; i++) {
        if (show_hd) printf("%s==> %s <==\n", i > optind ? "\n" : "", argv[i]);
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { weprintf("open %s:", argv[i]); rc = 1; continue; }
        if (head_fd(fd, argv[i]) < 0) rc = 1;
        close(fd);
    }
    return rc;
}
