/* diff / cmp -- one multi-call binary, dispatching on argv[0].
 *
 *     diff [-bcilNqrsuw] [-U n] file1 file2
 *     cmp  [-ls] file1 file2
 *
 * Neither existed natively. busybox in this tree has both, but it is a
 * gitignored, opt-in Linux-ABI demo that is not part of the userland, so
 * anything depending on it would break on a plain build -- and zdiff/zcmp
 * depend on exactly these two.
 *
 * The line matching is Myers' O(ND) diff (the same algorithm GNU diff and
 * git use), which walks D-paths outward until the two files meet. That is
 * fast when the files are similar, which is the case that matters. D is
 * capped: past the cap the answer degrades to "replace the whole thing"
 * rather than spending unbounded memory, and that is stated rather than
 * silently produced.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *prog = "diff";
static int  iflag, bflag, wflag, qflag, sflag, uflag, rflag, Nflag, lflag;
static int  ucontext = 3;
static int  status;          /* 0 same, 1 differ, 2 trouble */

/* ------------------------------------------------------------- file load */

struct filebuf {
    char  *data;
    size_t len;
    char **line;     /* pointers into data, NUL-terminated */
    size_t nlines;
    int    missing;
    int    no_final_nl;
};

static int load(const char *path, struct filebuf *fb) {
    memset(fb, 0, sizeof *fb);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (Nflag) { fb->missing = 1; fb->data = (char *)calloc(1, 1); return 0; }
        return -1;
    }
    size_t cap = 65536, len = 0;
    char *b = (char *)malloc(cap);
    if (!b) { fclose(f); return -1; }
    for (;;) {
        if (len == cap) {
            char *nb = (char *)realloc(b, cap * 2);
            if (!nb) { free(b); fclose(f); return -1; }
            b = nb; cap *= 2;
        }
        size_t n = fread(b + len, 1, cap - len, f);
        if (n == 0) break;
        len += n;
    }
    fclose(f);

    /* Split into lines, keeping the text NUL-terminated per line. */
    size_t count = 0;
    for (size_t i = 0; i < len; i++) if (b[i] == '\n') count++;
    if (len && b[len - 1] != '\n') count++;
    char **lv = (char **)malloc((count + 1) * sizeof(char *));
    if (!lv) { free(b); return -1; }
    size_t nl = 0, start = 0;
    for (size_t i = 0; i < len; i++) {
        if (b[i] == '\n') { b[i] = 0; lv[nl++] = b + start; start = i + 1; }
    }
    if (start < len) {
        /* No trailing newline. The last line was never NUL-terminated, so
         * it ran off the end of the buffer and printed trailing garbage.
         * Terminate it explicitly, and remember, so the report can say so
         * the way diff does. */
        char *nb = (char *)realloc(b, len + 1);
        if (!nb) { free(b); free(lv); return -1; }
        b = nb;
        b[len] = 0;
        /* realloc may have moved the buffer; rebuild the pointers. */
        nl = 0; start = 0;
        for (size_t q = 0; q < len; q++)
            if (b[q] == 0) { lv[nl++] = b + start; start = q + 1; }
        lv[nl++] = b + start;
        fb->no_final_nl = 1;
    }

    fb->data = b; fb->len = len; fb->line = lv; fb->nlines = nl;
    return 0;
}

/* Comparison honouring -i/-b/-w. -w drops ALL whitespace; -b collapses
 * runs of it and ignores it at the ends. */
static int same_line(const char *a, const char *b) {
    if (wflag) {
        for (;;) {
            while (*a == ' ' || *a == '\t') a++;
            while (*b == ' ' || *b == '\t') b++;
            if (!*a || !*b) break;
            char ca = iflag ? (char)tolower((unsigned char)*a) : *a;
            char cb = iflag ? (char)tolower((unsigned char)*b) : *b;
            if (ca != cb) return 0;
            a++; b++;
        }
        while (*a == ' ' || *a == '\t') a++;
        while (*b == ' ' || *b == '\t') b++;
        return !*a && !*b;
    }
    if (bflag) {
        for (;;) {
            if ((*a == ' ' || *a == '\t') && (*b == ' ' || *b == '\t')) {
                while (*a == ' ' || *a == '\t') a++;
                while (*b == ' ' || *b == '\t') b++;
                continue;
            }
            if (!*a || !*b) break;
            char ca = iflag ? (char)tolower((unsigned char)*a) : *a;
            char cb = iflag ? (char)tolower((unsigned char)*b) : *b;
            if (ca != cb) return 0;
            a++; b++;
        }
        while (*a == ' ' || *a == '\t') a++;
        while (*b == ' ' || *b == '\t') b++;
        return !*a && !*b;
    }
    if (iflag) {
        while (*a && *b) {
            if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
            a++; b++;
        }
        return !*a && !*b;
    }
    return strcmp(a, b) == 0;
}

/* Whether a line ends with a newline is part of the line: GNU calls
 * "a<nl>b" and "a<nl>b<nl>" different files, and reports it with the
 * "\ No newline" marker. Comparing text alone called them identical. */
static struct filebuf *g_A, *g_B;

static int line_terminated(struct filebuf *f, size_t i) {
    return !(f->no_final_nl && i + 1 == f->nlines);
}

static int eq_at(size_t x, size_t y) {
    if (line_terminated(g_A, x) != line_terminated(g_B, y)) return 0;
    return same_line(g_A->line[x], g_B->line[y]);
}

/* ------------------------------------------------------ Myers O(ND) diff */

/* The edit script, as a run of operations over the two line arrays. */
enum { OP_SAME, OP_DEL, OP_ADD };
struct edit { int op; size_t a, b; };

static struct edit *edits;
static size_t       nedits, edits_cap;

static void push_edit(int op, size_t a, size_t b) {
    if (nedits == edits_cap) {
        edits_cap = edits_cap ? edits_cap * 2 : 256;
        edits = (struct edit *)realloc(edits, edits_cap * sizeof *edits);
    }
    edits[nedits].op = op; edits[nedits].a = a; edits[nedits].b = b;
    nedits++;
}

#define MAXD 20000

/* Returns 0 on success, -1 if D exceeded the cap. */
static int myers(struct filebuf *A, struct filebuf *B) {
    g_A = A; g_B = B;
    long n = (long)A->nlines, m = (long)B->nlines;
    long max = n + m;
    if (max > MAXD) max = MAXD;
    long off = max + 1;
    long vsize = 2 * max + 3;

    long *v = (long *)malloc((size_t)vsize * sizeof(long));
    long **trace = (long **)malloc((size_t)(max + 1) * sizeof(long *));
    if (!v || !trace) { free(v); free(trace); return -1; }
    for (long i = 0; i < vsize; i++) v[i] = 0;

    long dfinal = -1;
    for (long d = 0; d <= max; d++) {
        trace[d] = (long *)malloc((size_t)vsize * sizeof(long));
        if (!trace[d]) { for (long q = 0; q < d; q++) free(trace[q]);
                         free(trace); free(v); return -1; }
        memcpy(trace[d], v, (size_t)vsize * sizeof(long));
        for (long k = -d; k <= d; k += 2) {
            long x;
            if (k == -d || (k != d && v[k - 1 + off] < v[k + 1 + off]))
                x = v[k + 1 + off];
            else
                x = v[k - 1 + off] + 1;
            long y = x - k;
            while (x < n && y < m && eq_at((size_t)x, (size_t)y)) { x++; y++; }
            v[k + off] = x;
            if (x >= n && y >= m) { dfinal = d; break; }
        }
        if (dfinal >= 0) break;
    }
    free(v);
    if (dfinal < 0) {
        /* The loop ran to completion, so every d was allocated. */
        for (long q = 0; q <= max; q++) free(trace[q]);
        free(trace);
        return -1;
    }

    /* Walk the trace backwards into an edit script, then reverse it. */
    long x = n, y = m;
    size_t base = nedits;
    for (long d = dfinal; d > 0; d--) {
        long *vp = trace[d];
        long k = x - y;
        long prev_k;
        if (k == -d || (k != d && vp[k - 1 + off] < vp[k + 1 + off]))
            prev_k = k + 1;
        else
            prev_k = k - 1;
        long prev_x = vp[prev_k + off];
        long prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) { push_edit(OP_SAME, (size_t)--x, (size_t)--y); }
        if (x > prev_x)      push_edit(OP_DEL, (size_t)--x, (size_t)y);
        else if (y > prev_y) push_edit(OP_ADD, (size_t)x, (size_t)--y);
    }
    while (x > 0 && y > 0) { push_edit(OP_SAME, (size_t)--x, (size_t)--y); }

    for (long q = 0; q <= dfinal; q++) free(trace[q]);
    free(trace);

    /* reverse -- guarded, because nedits == base for two empty files and
     * `nedits - 1` would wrap to SIZE_MAX. */
    if (nedits > base) {
        for (size_t i = base, j = nedits - 1; i < j; i++, j--) {
            struct edit t = edits[i]; edits[i] = edits[j]; edits[j] = t;
        }
    }
    return 0;
}

/* ------------------------------------------------------------- reporting */

/* diff marks a file whose last line has no newline. Without this the two
 * sides look identical in the output even though the bytes differ. */
static void no_nl(struct filebuf *f, size_t ln) {
    if (f->no_final_nl && ln + 1 == f->nlines)
        printf("\\ No newline at end of file\n");
}

static void print_normal(struct filebuf *A, struct filebuf *B) {
    size_t i = 0;
    while (i < nedits) {
        if (edits[i].op == OP_SAME) { i++; continue; }
        size_t j = i, ndel = 0, nadd = 0;
        size_t a0 = edits[i].a, b0 = edits[i].b;
        while (j < nedits && edits[j].op != OP_SAME) {
            if (edits[j].op == OP_DEL) ndel++; else nadd++;
            j++;
        }
        /* 1-based inclusive ranges, the way `diff` has always printed them. */
        char ra[64], rb[64];
        if (ndel == 0) snprintf(ra, sizeof ra, "%zu", a0);
        else if (ndel == 1) snprintf(ra, sizeof ra, "%zu", a0 + 1);
        else snprintf(ra, sizeof ra, "%zu,%zu", a0 + 1, a0 + ndel);
        if (nadd == 0) snprintf(rb, sizeof rb, "%zu", b0);
        else if (nadd == 1) snprintf(rb, sizeof rb, "%zu", b0 + 1);
        else snprintf(rb, sizeof rb, "%zu,%zu", b0 + 1, b0 + nadd);

        const char *op = ndel && nadd ? "c" : (ndel ? "d" : "a");
        printf("%s%s%s\n", ra, op, rb);
        for (size_t k = i; k < j; k++)
            if (edits[k].op == OP_DEL) {
                printf("< %s\n", A->line[edits[k].a]);
                no_nl(A, edits[k].a);
            }
        if (ndel && nadd) printf("---\n");
        for (size_t k = i; k < j; k++)
            if (edits[k].op == OP_ADD) {
                printf("> %s\n", B->line[edits[k].b]);
                no_nl(B, edits[k].b);
            }
        i = j;
    }
}

/* A unified range: "start,count", except that a count of one is written
 * bare and a count of zero keeps the line BEFORE the insertion point. */
static void fmt_range(char *buf, size_t cap, size_t start0, size_t count) {
    if (count == 0)      snprintf(buf, cap, "%zu,0", start0);
    else if (count == 1) snprintf(buf, cap, "%zu", start0 + 1);
    else                 snprintf(buf, cap, "%zu,%zu", start0 + 1, count);
}

static void print_unified(struct filebuf *A, struct filebuf *B,
                          const char *na, const char *nb) {
    printf("--- %s\n", na);
    printf("+++ %s\n", nb);
    size_t i = 0;
    while (i < nedits) {
        if (edits[i].op == OP_SAME) { i++; continue; }
        /* Open the hunk `ucontext` lines earlier, and keep extending while
         * another change falls within 2*context of the last one. */
        size_t start = i;
        size_t ctx = (size_t)ucontext;
        size_t s = start > ctx ? start - ctx : 0;
        while (s < start && edits[s].op != OP_SAME) s--;
        size_t j = i;
        for (;;) {
            while (j < nedits && edits[j].op != OP_SAME) j++;
            size_t run = j;
            while (run < nedits && edits[run].op == OP_SAME) run++;
            if (run < nedits && run - j <= ctx * 2) { j = run; continue; }
            break;
        }
        size_t e = j + ctx;
        if (e > nedits) e = nedits;

        /* Counts are of the lines each side CONTRIBUTES; the start is the
         * first such line. Deriving the count from hi-lo was wrong when a
         * side contributes nothing (-U0 printed "-2,1 +1,2" for what GNU
         * calls "-2 +2"). */
        size_t alo = edits[s].a, blo = edits[s].b;
        size_t acount = 0, bcount = 0;
        int seen_a = 0, seen_b = 0;
        for (size_t k = s; k < e; k++) {
            if (edits[k].op != OP_ADD) {
                if (!seen_a) { alo = edits[k].a; seen_a = 1; }
                acount++;
            }
            if (edits[k].op != OP_DEL) {
                if (!seen_b) { blo = edits[k].b; seen_b = 1; }
                bcount++;
            }
        }
        char ra[64], rb[64];
        fmt_range(ra, sizeof ra, alo, acount);
        fmt_range(rb, sizeof rb, blo, bcount);
        printf("@@ -%s +%s @@\n", ra, rb);
        for (size_t k = s; k < e; k++) {
            if (edits[k].op == OP_SAME) {
                printf(" %s\n", A->line[edits[k].a]); no_nl(A, edits[k].a);
            } else if (edits[k].op == OP_DEL) {
                printf("-%s\n", A->line[edits[k].a]); no_nl(A, edits[k].a);
            } else {
                printf("+%s\n", B->line[edits[k].b]); no_nl(B, edits[k].b);
            }
        }
        i = e;
    }
}

static int diff_files(const char *pa, const char *pb) {
    struct filebuf A, B;
    if (load(pa, &A) != 0) { fprintf(stderr, "%s: %s: No such file or directory\n", prog, pa); status = 2; return 2; }
    if (load(pb, &B) != 0) { fprintf(stderr, "%s: %s: No such file or directory\n", prog, pb); status = 2; return 2; }

    nedits = 0;
    int capped = (myers(&A, &B) != 0);

    int differ = 0;
    if (capped) {
        differ = 1;
        fprintf(stderr, "%s: %s vs %s: too many differences to describe "
                        "(over %d edits); reporting them as wholly "
                        "different\n", prog, pa, pb, MAXD);
    } else {
        for (size_t k = 0; k < nedits; k++)
            if (edits[k].op != OP_SAME) { differ = 1; break; }
    }

    if (!differ) {
        if (sflag) printf("Files %s and %s are identical\n", pa, pb);
    } else if (qflag || capped) {
        printf("Files %s and %s differ\n", pa, pb);
    } else if (uflag) {
        print_unified(&A, &B, pa, pb);
    } else {
        print_normal(&A, &B);
    }

    free(A.data); free(A.line); free(B.data); free(B.line);
    if (differ && status < 1) status = 1;
    return differ;
}

static void diff_dirs(const char *da, const char *db) {
    DIR *d = opendir(da);
    if (!d) { fprintf(stderr, "%s: %s: cannot read directory\n", prog, da); status = 2; return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char pa[1024], pb[1024];
        snprintf(pa, sizeof pa, "%s/%s", da, e->d_name);
        snprintf(pb, sizeof pb, "%s/%s", db, e->d_name);
        struct stat sa, sb;
        int hb = (stat(pb, &sb) == 0);
        if (stat(pa, &sa) == 0 && S_ISDIR(sa.st_mode)) {
            if (hb && S_ISDIR(sb.st_mode)) diff_dirs(pa, pb);
            else printf("Only in %s: %s\n", da, e->d_name);
            continue;
        }
        if (!hb) {
            if (Nflag) diff_files(pa, pb);
            else { printf("Only in %s: %s\n", da, e->d_name); if (status < 1) status = 1; }
            continue;
        }
        printf("diff %s %s\n", pa, pb);
        diff_files(pa, pb);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ cmp */

/* GNU quotes the name, and says "which is empty" rather than
 * "after byte 0" when the file has no bytes at all. */
static void cmp_eof(const char *name, long nbytes, long line) {
    if (nbytes <= 0)
        fprintf(stderr, "cmp: EOF on '%s' which is empty\n", name);
    else
        fprintf(stderr, "cmp: EOF on '%s' after byte %ld, in line %ld\n",
                name, nbytes, line);
}

static int do_cmp(const char *pa, const char *pb) {
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    if (!fa || !fb) {
        fprintf(stderr, "cmp: %s: No such file or directory\n", !fa ? pa : pb);
        if (fa) fclose(fa); if (fb) fclose(fb);
        return 2;
    }
    long off = 1, line = 1;
    int rc = 0;
    for (;;) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca == EOF && cb == EOF) break;
        if (ca == EOF) { if (!sflag) cmp_eof(pa, off - 1, line);
            rc = 1; break; }
        if (cb == EOF) { if (!sflag) cmp_eof(pb, off - 1, line);
            rc = 1; break; }
        if (ca != cb) {
            rc = 1;
            if (!sflag) {
                if (lflag) { printf("%ld %o %o\n", off, ca, cb); off++; continue; }
                printf("%s %s differ: char %ld, line %ld\n", pa, pb, off, line);
            }
            if (!lflag) break;
        }
        if (ca == '\n') line++;
        off++;
    }
    fclose(fa); fclose(fb);
    return rc;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "diff";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    int is_cmp = !strcmp(base, "cmp");

    int files[8], nf = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && strcmp(a, "-")) {
            if (!strcmp(a, "--brief"))    { qflag = 1; continue; }
            if (!strcmp(a, "--unified"))  { uflag = 1; continue; }
            if (!strcmp(a, "--silent") || !strcmp(a, "--quiet")) { sflag = 1; continue; }
            if (!strncmp(a, "-U", 2)) {
                uflag = 1;
                const char *v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : "3");
                ucontext = atoi(v);
                continue;
            }
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'b': bflag = 1; break;
                case 'i': iflag = 1; break;
                case 'l': lflag = 1; break;
                case 'N': Nflag = 1; break;
                case 'q': qflag = 1; break;
                case 'r': rflag = 1; break;
                case 's': sflag = 1; break;
                case 'u': uflag = 1; break;
                case 'w': wflag = 1; break;
                case 'c': uflag = 1; break;   /* context: unified is close enough
                                               * and is what everything reads */
                default:
                    fprintf(stderr, "%s: invalid option -- '%c'\n", prog, *p);
                    return 2;
                }
            }
            continue;
        }
        if (nf < 8) files[nf++] = i;
    }

    if (nf < 2) {
        fprintf(stderr, "usage: %s [options] file1 file2\n", prog);
        return 2;
    }
    const char *pa = argv[files[0]], *pb = argv[files[1]];

    if (is_cmp) return do_cmp(pa, pb);

    struct stat sa, sb;
    if (rflag || (stat(pa, &sa) == 0 && S_ISDIR(sa.st_mode) &&
                  stat(pb, &sb) == 0 && S_ISDIR(sb.st_mode))) {
        diff_dirs(pa, pb);
        return status;
    }
    diff_files(pa, pb);
    return status;
}
