/* diff / cmp / sdiff / diff3 -- one multi-call binary, on argv[0].
 *
 *     diff  [-bcilNqrsuw] [-U n] file1 file2
 *     cmp   [-ls] file1 file2
 *     sdiff [-bilw] [-w cols] file1 file2
 *     diff3 [-e] file1 file2 file3
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


/* ------------------------------------------------------------- sdiff ----
 *
 * Side by side. The right-hand column starts at the first TAB STOP past
 * (width-3)/2 -- measured against GNU sdiff across seven widths, which is
 * where its otherwise puzzling column choices come from (w=40 puts it at
 * 25, not 20). The padding itself is tabs, as GNU's is.
 *
 * NOT claimed as byte-identical to GNU: GNU mixes tabs and spaces before
 * the marker in a way that does not follow from the width alone. The
 * COLUMNS, the markers and the content match; the exact run of whitespace
 * before the marker may not.
 */

static int sdiff_width = 130;

static int next_tabstop(int col) {      /* stops at 1, 9, 17, ... */
    int t = 1;
    while (t <= col) t += 8;
    return t;
}

static void sdiff_row(const char *l, const char *r, char marker) {
    int half  = (sdiff_width - 3) / 2;
    int rcol  = next_tabstop(half);
    int mcol  = rcol - 2;

    int col = 1;
    if (l) {
        for (const char *p = l; *p; p++) { putchar(*p); col++; }
    }
    /* Tabs up to the marker column, then spaces for the remainder. */
    while (col < mcol) {
        int nt = next_tabstop(col);
        if (nt <= mcol) { putchar(9); col = nt; }
        else            { putchar(' '); col++; }
    }
    if (marker != ' ') { putchar(marker); col++; }
    while (col < rcol) {
        int nt = next_tabstop(col);
        if (nt <= rcol) { putchar(9); col = nt; }
        else            { putchar(' '); col++; }
    }
    if (r) fputs(r, stdout);
    putchar(10);
}

static int do_sdiff(const char *pa, const char *pb) {
    struct filebuf A, B;
    if (load(pa, &A) != 0) { fprintf(stderr, "sdiff: %s: No such file or directory" "\n", pa); return 2; }
    if (load(pb, &B) != 0) { fprintf(stderr, "sdiff: %s: No such file or directory" "\n", pb); return 2; }
    nedits = 0;
    if (myers(&A, &B) != 0) {
        fprintf(stderr, "sdiff: too many differences" "\n");
        return 2;
    }
    int differ = 0;
    size_t i = 0;
    while (i < nedits) {
        if (edits[i].op == OP_SAME) {
            sdiff_row(A.line[edits[i].a], B.line[edits[i].b], ' ');
            i++;
            continue;
        }
        /* Pair up a run of deletions with the additions that follow, so a
         * changed line shows as one row with `|` rather than as a delete
         * and an add on separate rows. */
        size_t d0 = i, nd = 0;
        while (i < nedits && edits[i].op == OP_DEL) { nd++; i++; }
        size_t a0 = i, na = 0;
        while (i < nedits && edits[i].op == OP_ADD) { na++; i++; }
        differ = 1;
        size_t k = 0;
        for (; k < nd && k < na; k++)
            sdiff_row(A.line[edits[d0 + k].a], B.line[edits[a0 + k].b], '|');
        for (; k < nd; k++) sdiff_row(A.line[edits[d0 + k].a], NULL, '<');
        for (; k < na; k++) sdiff_row(NULL, B.line[edits[a0 + k].b], '>');
    }
    free(A.data); free(A.line); free(B.data); free(B.line);
    return differ ? 1 : 0;
}

/* ------------------------------------------------------------- diff3 ----
 *
 * Aligns file1 and file3 against file2 (the classic MINE / OLDER / YOURS
 * order) and reports every region where the three do not agree. A region
 * is labelled ====N when only file N differs, and a bare ==== when all
 * three disagree -- which is what a merge conflict is.
 */

/* For each line of the base, the matching line in `other`, or -1. */
static int *align(struct filebuf *base, struct filebuf *other) {
    int *map = (int *)malloc((base->nlines + 1) * sizeof(int));
    if (!map) return NULL;
    for (size_t i = 0; i <= base->nlines; i++) map[i] = -1;
    nedits = 0;
    if (myers(base, other) != 0) { free(map); return NULL; }
    for (size_t k = 0; k < nedits; k++)
        if (edits[k].op == OP_SAME) map[edits[k].a] = (int)edits[k].b;
    return map;
}

static void d3_range(const char *label, long lo, long hi) {
    /* lo/hi are 0-based and inclusive; an empty region prints as `a` at
     * the line BEFORE it, which is how diff3 says "append here". */
    if (hi < lo) printf("%s:%lda" "\n", label, lo);
    else if (hi == lo) printf("%s:%ldc" "\n", label, lo + 1);
    else printf("%s:%ld,%ldc" "\n", label, lo + 1, hi + 1);
}

static void d3_text(struct filebuf *f, long lo, long hi) {
    for (long k = lo; k <= hi && k < (long)f->nlines; k++)
        printf("  %s" "\n", f->line[k]);
}


/* Do the two spans hold the same lines? Used to decide which two of the
 * three files agree in a region. */
static int same_span(struct filebuf *A, long alo, long ahi,
                     struct filebuf *B, long blo, long bhi) {
    if (ahi - alo != bhi - blo) return 0;
    for (long k = 0; k <= ahi - alo; k++) {
        long i = alo + k, j = blo + k;
        if (i < 0 || j < 0 || i >= (long)A->nlines || j >= (long)B->nlines) return 0;
        if (strcmp(A->line[i], B->line[j])) return 0;
    }
    return 1;
}

static int do_diff3(const char *p1, const char *p2, const char *p3) {
    struct filebuf F1, F2, F3;
    if (load(p1, &F1) != 0 || load(p2, &F2) != 0 || load(p3, &F3) != 0) {
        fprintf(stderr, "diff3: cannot read one of the files" "\n");
        return 2;
    }
    int *m1 = align(&F2, &F1);      /* base line -> file1 line, or -1 */
    int *m3 = align(&F2, &F3);      /* base line -> file3 line, or -1 */
    if (!m1 || !m3) {
        fprintf(stderr, "diff3: files are too different to align" "\n");
        free(m1); free(m3);
        return 2;
    }

    int any = 0;
    size_t i = 0;
    while (i < F2.nlines) {
        if (m1[i] >= 0 && m3[i] >= 0) { i++; continue; }
        /* A region begins here and runs while either side is unmatched. */
        size_t lo = i;
        while (i < F2.nlines && (m1[i] < 0 || m3[i] < 0)) i++;
        size_t hi = i - 1;

        /* The corresponding spans in files 1 and 3 lie between the nearest
         * matched lines on either side. */
        long a_lo = 0, c_lo = 0;
        for (long k = (long)lo - 1; k >= 0; k--) {
            if (m1[k] >= 0 && a_lo == 0) a_lo = m1[k] + 1;
            if (m3[k] >= 0 && c_lo == 0) c_lo = m3[k] + 1;
            if (a_lo && c_lo) break;
        }
        long a_hi = (long)F1.nlines - 1, c_hi = (long)F3.nlines - 1;
        for (size_t k = hi + 1; k < F2.nlines; k++) {
            if (m1[k] >= 0) { a_hi = m1[k] - 1; break; }
        }
        for (size_t k = hi + 1; k < F2.nlines; k++) {
            if (m3[k] >= 0) { c_hi = m3[k] - 1; break; }
        }

        /* Which of the three is the odd one out? */
        int one_differs = 0, three_differs = 0;
        for (size_t k = lo; k <= hi; k++) {
            if (m1[k] < 0) one_differs = 1;
            if (m3[k] < 0) three_differs = 1;
        }

        any = 1;

        /* diff3 prints the two files that AGREE as one block -- their
         * ranges consecutively, then the text once -- and the block
         * containing file 1 comes first. That is why ====2 reads
         * "1: 3: <text> 2: <text>" while ====1 reads "1: <text> 2: 3:
         * <text>": it is not numeric order, it is grouping. */
        long lo1 = a_lo, hi1 = a_hi;
        long lo2 = (long)lo, hi2 = (long)hi;
        long lo3 = c_lo, hi3 = c_hi;
        struct filebuf *fb[3] = { &F1, &F2, &F3 };
        long los[3] = { lo1, lo2, lo3 };
        long his[3] = { hi1, hi2, hi3 };

        int same13 = same_span(&F1, lo1, hi1, &F3, lo3, hi3);
        int same12 = same_span(&F1, lo1, hi1, &F2, lo2, hi2);
        int same23 = same_span(&F2, lo2, hi2, &F3, lo3, hi3);

        int g[2][3], gn[2] = {0, 0};
        if (same23 && !same12) {                 /* 2 and 3 agree: odd is 1 */
            printf("====1" "\n");
            g[0][gn[0]++] = 0;
            g[1][gn[1]++] = 1; g[1][gn[1]++] = 2;
        } else if (same13 && !same12) {          /* 1 and 3 agree: odd is 2 */
            printf("====2" "\n");
            g[0][gn[0]++] = 0; g[0][gn[0]++] = 2;
            g[1][gn[1]++] = 1;
        } else if (same12 && !same13) {          /* 1 and 2 agree: odd is 3 */
            printf("====3" "\n");
            g[0][gn[0]++] = 0; g[0][gn[0]++] = 1;
            g[1][gn[1]++] = 2;
        } else {                                  /* all three disagree */
            printf("====" "\n");
            g[0][gn[0]++] = 0;
            g[1][gn[1]++] = 1;
            /* a third group; handled by the loop below via gn[1] */
            g[1][gn[1]++] = -2;                   /* sentinel: break after 2 */
        }

        if (gn[1] == 2 && g[1][1] == -2) {
            /* Conflict: each file on its own, in order. */
            static const char *nm[3] = { "1", "2", "3" };
            for (int k = 0; k < 3; k++) {
                d3_range(nm[k], los[k], his[k]);
                d3_text(fb[k], los[k], his[k]);
            }
        } else {
            static const char *nm[3] = { "1", "2", "3" };
            for (int grp = 0; grp < 2; grp++) {
                for (int k = 0; k < gn[grp]; k++)
                    d3_range(nm[g[grp][k]], los[g[grp][k]], his[g[grp][k]]);
                int first = g[grp][0];
                d3_text(fb[first], los[first], his[first]);
            }
        }
    }

    free(m1); free(m3);
    free(F1.data); free(F1.line);
    free(F2.data); free(F2.line);
    free(F3.data); free(F3.line);
    (void)any;
    return 0;   /* GNU reserves 1 for merge conflicts, 2 for trouble */
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
    int is_cmp   = !strcmp(base, "cmp");
    int is_sdiff = !strcmp(base, "sdiff");
    int is_diff3 = !strcmp(base, "diff3");

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
                case 'w':
                    /* sdiff -w takes a COLUMN COUNT; diff -w means "ignore\n* all whitespace". Same letter, different tools. */
                    if (is_sdiff) {
                        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                        if (v) sdiff_width = atoi(v);
                        if (sdiff_width < 20) sdiff_width = 20;
                        p = ""; p--;
                    } else wflag = 1;
                    break;
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
    if (is_sdiff) return do_sdiff(pa, pb);
    if (is_diff3) {
        if (nf < 3) {
            fprintf(stderr, "usage: diff3 file1 file2 file3\n");
            return 2;
        }
        return do_diff3(pa, pb, argv[files[2]]);
    }

    struct stat sa, sb;
    if (rflag || (stat(pa, &sa) == 0 && S_ISDIR(sa.st_mode) &&
                  stat(pb, &sb) == 0 && S_ISDIR(sb.st_mode))) {
        diff_dirs(pa, pb);
        return status;
    }
    diff_files(pa, pb);
    return status;
}
