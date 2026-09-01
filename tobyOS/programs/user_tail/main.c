/* tail -- print the last part of a file.
 *
 *     tail [-c N] [-n N] [-fqv] [file ...]
 *
 * WHAT WAS WRONG. This read its count with atoi(), and atoi("+2") is 2.
 * So `tail -n +2` -- the single most common way any shell script drops a
 * header line -- silently meant `tail -n 2`, printing the LAST two lines
 * instead of everything from the second line on. It produced plausible
 * output for the exact opposite request, which is why it went unnoticed:
 * a gate here was reading the last two rows of an lsblk listing and
 * concluding a device did not exist.
 *
 * So `+N` now means "start at line N", counted from one, as POSIX says,
 * and `-c` (bytes), multiple files with their `==> name <==` headers,
 * -q/-v, and -f are all here too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE 8192

static int cflag, fflag, qflag, vflag;
static int from_start;              /* the +N forms count from the front */
static long count = 10;
static int  exit_status;

/* Parse the argument of -n/-c. A leading '+' selects from_start; a leading
 * '-' is accepted and ignored, since `tail -n -5` and `tail -n 5` are the
 * same request. Returns 0 on a malformed number. */
static int parse_count(const char *s, long *out, int *plus) {
    *plus = 0;
    if (*s == '+') { *plus = 1; s++; }
    else if (*s == '-') s++;
    if (!*s) return 0;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
    }
    *out = v;
    return 1;
}

/* ---- the four modes ---- */

/* A line is BYTES up to and including a newline -- not a C string. These
 * used fgets()/fputs(), so a NUL anywhere in the input truncated the line
 * at it. That matters: `tail -n +N` over a binary file is exactly what a
 * self-extracting archive does, and it was silently corrupting the
 * payload. Everything below counts newlines over raw bytes. */
struct rline { char *p; size_t len; };

static void tail_lines_last(FILE *fp) {
    if (count <= 0) return;
    struct rline *ring = (struct rline *)calloc((size_t)count, sizeof *ring);
    if (!ring) return;
    long   pos = 0, total = 0;
    char  *cur = NULL;
    size_t clen = 0, ccap = 0;
    int    c;
    while ((c = fgetc(fp)) != EOF) {
        if (clen + 1 > ccap) {
            size_t nc = ccap ? ccap * 2 : 256;
            char *nb = (char *)realloc(cur, nc);
            if (!nb) { free(cur); cur = NULL; clen = 0; break; }
            cur = nb; ccap = nc;
        }
        cur[clen++] = (char)c;
        if (c != 10) continue;
        free(ring[pos].p);
        ring[pos].p = cur; ring[pos].len = clen;
        cur = NULL; clen = 0; ccap = 0;
        pos = (pos + 1) % count;
        total++;
    }
    if (clen) {                     /* a final line with no newline */
        free(ring[pos].p);
        ring[pos].p = cur; ring[pos].len = clen;
        pos = (pos + 1) % count;
        total++;
    } else {
        free(cur);
    }
    long start = (total < count) ? 0 : pos;
    long n     = (total < count) ? total : count;
    for (long k = 0; k < n; k++) {
        struct rline *r = &ring[(start + k) % count];
        if (r->p) fwrite(r->p, 1, r->len, stdout);
    }
    for (long k = 0; k < count; k++) free(ring[k].p);
    free(ring);
}

static void tail_lines_from(FILE *fp) {
    /* +1 means the whole file; +N skips N-1 lines. Byte-oriented, so a
     * NUL in the input cannot truncate anything. */
    long skip = count > 0 ? count - 1 : 0;
    unsigned char buf[65536];
    size_t n;
    long seen = 0;
    int copying = (skip == 0);
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        size_t k = 0;
        if (!copying) {
            while (k < n && seen < skip) {
                if (buf[k] == 10) seen++;
                k++;
            }
            if (seen >= skip) copying = 1;
        }
        if (copying && k < n) fwrite(buf + k, 1, n - k, stdout);
    }
}

static void tail_bytes_last(FILE *fp) {
    if (count <= 0) return;
    char *ring = (char *)malloc((size_t)count);
    if (!ring) return;
    long pos = 0, total = 0;
    int  c;
    while ((c = fgetc(fp)) != EOF) {
        ring[pos] = (char)c;
        pos = (pos + 1) % count;
        total++;
    }
    long start = (total < count) ? 0 : pos;
    long n     = (total < count) ? total : count;
    for (long i = 0; i < n; i++) fputc(ring[(start + i) % count], stdout);
    free(ring);
}

static void tail_bytes_from(FILE *fp) {
    long skip = count > 0 ? count - 1 : 0;
    int  c;
    long seen = 0;
    while ((c = fgetc(fp)) != EOF) {
        if (seen++ < skip) continue;
        fputc(c, stdout);
    }
}

static void emit(FILE *fp) {
    if (cflag) { if (from_start) tail_bytes_from(fp); else tail_bytes_last(fp); }
    else       { if (from_start) tail_lines_from(fp); else tail_lines_last(fp); }
    fflush(stdout);
}

/* -f: keep reading past end of file. Nothing here polls -- it re-reads and
 * sleeps, which is what tail does on a filesystem with no change
 * notification. Only reachable when the user asks for it. */
static void follow(const char *path) {
    for (;;) {
        sleep(1);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        static long consumed = -1;
        if (consumed < 0) { fseek(fp, 0, SEEK_END); consumed = ftell(fp); }
        fseek(fp, 0, SEEK_END);
        long end = ftell(fp);
        if (end > consumed) {
            fseek(fp, consumed, SEEK_SET);
            int c;
            while ((c = fgetc(fp)) != EOF) fputc(c, stdout);
            fflush(stdout);
            consumed = end;
        } else if (end < consumed) {
            consumed = end;   /* truncated underneath us */
        }
        fclose(fp);
    }
}

int main(int argc, char **argv) {
    int    files[256], nfiles = 0;
    int    plus = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        /* `tail -5` and `tail +5` are the historical shorthands, and
         * scripts still use both. */
        if ((a[0] == '-' || a[0] == '+') && a[1] >= '0' && a[1] <= '9') {
            long v;
            if (parse_count(a, &v, &plus)) {
                count = v; from_start = plus; continue;
            }
        }
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'c': case 'n': {
                    int is_c = (*p == 'c');
                    const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    long v;
                    if (!val || !parse_count(val, &v, &plus)) {
                        fprintf(stderr, "tail: invalid number of %s\n",
                                is_c ? "bytes" : "lines");
                        return 1;
                    }
                    cflag = is_c; count = v; from_start = plus;
                    p = ""; p--;      /* the value consumed the rest of this arg */
                    break;
                }
                case 'f': fflag = 1; break;
                case 'q': qflag = 1; vflag = 0; break;
                case 'v': vflag = 1; qflag = 0; break;
                default:
                    fprintf(stderr, "usage: tail [-c N] [-n N] [-fqv] [file ...]\n");
                    return 1;
                }
                if (!*p) break;
            }
            continue;
        }
        if (nfiles < (int)(sizeof files / sizeof files[0])) files[nfiles++] = i;
    }

    if (nfiles == 0) {
        emit(stdin);
        return exit_status;
    }

    int show = vflag || (nfiles > 1 && !qflag);
    for (int k = 0; k < nfiles; k++) {
        const char *path = argv[files[k]];
        /* "rb": tail must be byte-exact. On a host with text mode a
         * 0x1A would end the file early, and a self-extracting
         * archive would lose its payload. */
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "tail: cannot open '%s'\n", path);
            exit_status = 1;
            continue;
        }
        if (show) printf("%s==> %s <==\n", k ? "\n" : "", path);
        emit(fp);
        fclose(fp);
    }
    if (fflag && nfiles == 1) follow(argv[files[0]]);
    return exit_status;
}
