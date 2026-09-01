/* patch -- apply a diff to a file.
 *
 *     patch [-pN] [-R] [-b] [-f] [-i patchfile] [--dry-run] [-o out] [file]
 *
 * Reads unified (@@ -a,b +c,d @@) and normal (NcM with < / --- / >) diffs.
 * There was no patch in this tree; busybox has one but it is a gitignored,
 * opt-in demo a plain build never stages.
 *
 * A hunk is not required to be exactly where the diff says it is. Real
 * patch searches outward from the stated line, because a file that has
 * drifted by a few lines is the normal case, and reports the offset it
 * found. A hunk that matches nowhere is written to a .rej file rather than
 * being forced -- silently applying it at the wrong place would be far
 * worse than refusing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int pstrip = -1;          /* -pN; -1 means "not given" */
static int reverse, dry_run, backup, force, quiet;
static const char *outfile;
static int exit_status;

/* ------------------------------------------------------------- text file */

struct text {
    char **line;
    size_t n, cap;
    int    no_final_nl;
};

static void t_push(struct text *t, const char *s, size_t len) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->line = (char **)realloc(t->line, t->cap * sizeof(char *));
    }
    char *c = (char *)malloc(len + 1);
    memcpy(c, s, len);
    c[len] = 0;
    t->line[t->n++] = c;
}

static void t_free(struct text *t) {
    for (size_t i = 0; i < t->n; i++) free(t->line[i]);
    free(t->line);
    memset(t, 0, sizeof *t);
}

static int t_load(const char *path, struct text *t) {
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char   *buf = NULL;
    size_t  cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            buf = (char *)realloc(buf, cap);
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        if (got == 0) break;
        len += got;
    }
    fclose(f);
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') { t_push(t, buf + start, i - start); start = i + 1; }
    }
    if (start < len) { t_push(t, buf + start, len - start); t->no_final_nl = 1; }
    free(buf);
    return 0;
}

static int t_save(const char *path, struct text *t) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (size_t i = 0; i < t->n; i++) {
        fputs(t->line[i], f);
        if (i + 1 < t->n || !t->no_final_nl) fputc('\n', f);
    }
    fclose(f);
    return 0;
}

/* ----------------------------------------------------------------- hunks */

struct hunk {
    long   old_start, new_start;
    char **old_line;  size_t old_n;    /* context + deletions */
    char **new_line;  size_t new_n;    /* context + additions */
};

struct patchfile {
    char        *target;
    struct hunk *h;
    size_t       nh, cap;
};

static void h_push_old(struct hunk *h, const char *s) {
    h->old_line = (char **)realloc(h->old_line, (h->old_n + 1) * sizeof(char *));
    h->old_line[h->old_n++] = strdup(s);
}
static void h_push_new(struct hunk *h, const char *s) {
    h->new_line = (char **)realloc(h->new_line, (h->new_n + 1) * sizeof(char *));
    h->new_line[h->new_n++] = strdup(s);
}

static struct hunk *pf_new_hunk(struct patchfile *pf) {
    if (pf->nh == pf->cap) {
        pf->cap = pf->cap ? pf->cap * 2 : 16;
        pf->h = (struct hunk *)realloc(pf->h, pf->cap * sizeof(struct hunk));
    }
    struct hunk *h = &pf->h[pf->nh++];
    memset(h, 0, sizeof *h);
    return h;
}

/* Strip -pN leading components, the way patch has always counted them: a
 * leading "/" counts, and -p0 keeps the name whole. */
static const char *strip_path(const char *p) {
    if (pstrip < 0) {
        /* Not given: patch uses the basename when the path does not exist. */
        const char *b = p;
        for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
        struct stat st;
        if (stat(p, &st) == 0) return p;
        return b;
    }
    int n = pstrip;
    const char *q = p;
    while (n > 0 && *q) {
        while (*q && *q != '/') q++;
        while (*q == '/') q++;
        n--;
    }
    return *q ? q : p;
}

static char *dup_name(const char *line) {
    /* "--- a/foo.c\t2026-01-01" -> "a/foo.c" */
    const char *p = line + 4;
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != '\t' && *e != '\n' && *e != ' ') e++;
    size_t n = (size_t)(e - p);
    char *s = (char *)malloc(n + 1);
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

/* --------------------------------------------------------- patch parsing */

static int parse_patch(FILE *f, struct patchfile *pf) {
    memset(pf, 0, sizeof *pf);
    char line[8192];
    char *pending_old = NULL, *pending_new = NULL;
    struct hunk *h = NULL;
    long remaining_old = 0, remaining_new = 0;
    int  in_normal = 0;
    long norm_a1 = 0, norm_a2 = 0;

    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;

        if (!strncmp(line, "--- ", 4)) {
            free(pending_old);
            pending_old = dup_name(line);
            h = NULL; in_normal = 0;
            continue;
        }
        if (!strncmp(line, "+++ ", 4)) {
            free(pending_new);
            pending_new = dup_name(line);
            continue;
        }
        if (!strncmp(line, "@@ ", 3)) {
            /* @@ -a,b +c,d @@ */
            long a = 0, b = 1, c = 0, d = 1;
            const char *p = line + 3;
            if (*p == '-') p++;
            a = strtol(p, (char **)&p, 10);
            if (*p == ',') { p++; b = strtol(p, (char **)&p, 10); }
            while (*p == ' ') p++;
            if (*p == '+') p++;
            c = strtol(p, (char **)&p, 10);
            if (*p == ',') { p++; d = strtol(p, (char **)&p, 10); }
            h = pf_new_hunk(pf);
            h->old_start = a;
            h->new_start = c;
            remaining_old = b;
            remaining_new = d;
            in_normal = 0;
            if (!pf->target) {
                /* Prefer the +++ name, as patch does, falling back to ---. */
                const char *cand = pending_new ? pending_new : pending_old;
                if (cand) pf->target = strdup(strip_path(cand));
            }
            continue;
        }
        if (h && !in_normal && (remaining_old > 0 || remaining_new > 0)) {
            char tag = line[0];
            const char *body = line[0] ? line + 1 : "";
            if (tag == '\\') continue;              /* "\ No newline..." */
            if (tag == '-') { h_push_old(h, body); remaining_old--; continue; }
            if (tag == '+') { h_push_new(h, body); remaining_new--; continue; }
            if (tag == ' ' || tag == 0) {
                h_push_old(h, body); h_push_new(h, body);
                remaining_old--; remaining_new--;
                continue;
            }
            /* Anything else ends the hunk. */
            h = NULL;
        }

        /* Normal-format hunk header: 2c2, 3,4d2, 5a6 */
        {
            const char *p = line;
            if (*p >= '0' && *p <= '9') {
                long a1 = strtol(p, (char **)&p, 10);
                long a2 = a1;
                if (*p == ',') { p++; a2 = strtol(p, (char **)&p, 10); }
                char op = *p;
                if (op == 'a' || op == 'c' || op == 'd') {
                    p++;
                    long b1 = strtol(p, (char **)&p, 10);
                    (void)b1;
                    h = pf_new_hunk(pf);
                    h->old_start = (op == 'a') ? a1 + 1 : a1;
                    h->new_start = (op == 'a') ? a1 + 1 : a1;
                    norm_a1 = a1; norm_a2 = a2;
                    (void)norm_a1; (void)norm_a2;
                    in_normal = 1;
                    if (!pf->target && pending_new)
                        pf->target = strdup(strip_path(pending_new));
                    continue;
                }
            }
        }
        if (h && in_normal) {
            if (!strncmp(line, "< ", 2)) { h_push_old(h, line + 2); continue; }
            if (!strncmp(line, "> ", 2)) { h_push_new(h, line + 2); continue; }
            if (!strcmp(line, "---"))    { continue; }
            in_normal = 0;
            h = NULL;
        }
    }
    free(pending_old);
    free(pending_new);
    return pf->nh > 0 ? 0 : -1;
}

/* ------------------------------------------------------------- applying */

static int lines_match(struct text *t, size_t at, char **want, size_t n) {
    if (at + n > t->n) return 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(t->line[at + i], want[i])) return 0;
    return 1;
}

/* Find where the hunk's old text sits, searching outward from the stated
 * line. Returns the 0-based index, or -1. */
static long locate(struct text *t, struct hunk *h) {
    long want = h->old_start - 1;
    if (want < 0) want = 0;
    if (h->old_n == 0) {
        /* A pure insertion: nothing to match, so the stated line stands. */
        return want <= (long)t->n ? want : (long)t->n;
    }
    if (lines_match(t, (size_t)want, h->old_line, h->old_n)) return want;
    long span = (long)t->n + 1;
    for (long d = 1; d <= span; d++) {
        long a = want - d, b = want + d;
        if (a >= 0 && lines_match(t, (size_t)a, h->old_line, h->old_n)) return a;
        if (b >= 0 && (size_t)b <= t->n &&
            lines_match(t, (size_t)b, h->old_line, h->old_n)) return b;
    }
    return -1;
}

static void swap_hunk(struct hunk *h) {
    char **l = h->old_line; size_t n = h->old_n;
    h->old_line = h->new_line; h->old_n = h->new_n;
    h->new_line = l;          h->new_n = n;
    long s = h->old_start; h->old_start = h->new_start; h->new_start = s;
}

static int apply(struct patchfile *pf, const char *path) {
    struct text t;
    if (t_load(path, &t) != 0) {
        fprintf(stderr, "patch: can't find file to patch: %s\n", path);
        return 2;
    }

    struct text out;
    memset(&out, 0, sizeof out);
    out.no_final_nl = t.no_final_nl;

    size_t src = 0;
    size_t failed = 0;
    FILE  *rej = NULL;
    char   rejname[1024];
    snprintf(rejname, sizeof rejname, "%s.rej", path);

    for (size_t k = 0; k < pf->nh; k++) {
        struct hunk *h = &pf->h[k];
        if (reverse) swap_hunk(h);

        long at = locate(&t, h);
        if (at < 0 || (size_t)at < src) {
            failed++;
            if (!quiet)
                fprintf(stderr, "patch: **** hunk #%zu FAILED at %ld\n",
                        k + 1, h->old_start);
            if (!rej) rej = fopen(rejname, "w");
            if (rej) {
                fprintf(rej, "@@ -%ld +%ld @@\n", h->old_start, h->new_start);
                for (size_t i = 0; i < h->old_n; i++) fprintf(rej, "-%s\n", h->old_line[i]);
                for (size_t i = 0; i < h->new_n; i++) fprintf(rej, "+%s\n", h->new_line[i]);
            }
            continue;
        }
        long offset = at - (h->old_start - 1);
        if (offset != 0 && !quiet)
            fprintf(stderr, "patch: Hunk #%zu succeeded at %ld (offset %ld line%s).\n",
                    k + 1, at + 1, offset, offset == 1 || offset == -1 ? "" : "s");

        while (src < (size_t)at) t_push(&out, t.line[src], strlen(t.line[src])), src++;
        for (size_t i = 0; i < h->new_n; i++)
            t_push(&out, h->new_line[i], strlen(h->new_line[i]));
        src += h->old_n;
    }
    while (src < t.n) { t_push(&out, t.line[src], strlen(t.line[src])); src++; }

    if (rej) fclose(rej);

    if (dry_run) {
        if (!quiet) printf("patch: dry run: %zu of %zu hunks would apply\n",
                           pf->nh - failed, pf->nh);
    } else {
        if (backup) {
            char bak[1024];
            snprintf(bak, sizeof bak, "%s.orig", path);
            struct text o;
            if (t_load(path, &o) == 0) { t_save(bak, &o); t_free(&o); }
        }
        const char *dest = outfile ? outfile : path;
        if (t_save(dest, &out) != 0) {
            fprintf(stderr, "patch: cannot write %s\n", dest);
            t_free(&t); t_free(&out);
            return 2;
        }
    }

    t_free(&t);
    t_free(&out);
    if (failed) {
        if (!quiet)
            fprintf(stderr, "patch: %zu out of %zu hunks failed -- saving "
                            "rejects to %s\n", failed, pf->nh, rejname);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *patchpath = NULL;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--dry-run")) { dry_run = 1; continue; }
        if (!strcmp(a, "--reverse")) { reverse = 1; continue; }
        if (!strcmp(a, "--backup"))  { backup = 1; continue; }
        if (!strcmp(a, "--force"))   { force = 1; continue; }
        if (!strcmp(a, "--quiet") || !strcmp(a, "--silent")) { quiet = 1; continue; }
        if (!strcmp(a, "--version")) { printf("patch (tobyOS) 1.0\n"); return 0; }
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'p': {
                    const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : "0");
                    pstrip = atoi(v);
                    p = ""; p--;
                    break;
                }
                case 'i': {
                    const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    patchpath = v;
                    p = ""; p--;
                    break;
                }
                case 'o': {
                    const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    outfile = v;
                    p = ""; p--;
                    break;
                }
                case 'R': reverse = 1; break;
                case 'b': backup = 1; break;
                case 'f': force = 1; break;
                case 's': quiet = 1; break;
                case 'l': break;      /* ignore whitespace: accepted */
                case 'N': break;
                default:
                    fprintf(stderr, "patch: unknown option -- '%c'\n", *p);
                    return 2;
                }
            }
            continue;
        }
        if (!target) target = a;
    }
    (void)force;

    FILE *pf_in = stdin;
    if (patchpath) {
        pf_in = fopen(patchpath, "r");
        if (!pf_in) {
            fprintf(stderr, "patch: can't open patch file %s\n", patchpath);
            return 2;
        }
    }

    struct patchfile pf;
    if (parse_patch(pf_in, &pf) != 0) {
        fprintf(stderr, "patch: **** Only garbage was found in the patch input.\n");
        if (pf_in != stdin) fclose(pf_in);
        return 2;
    }
    if (pf_in != stdin) fclose(pf_in);

    const char *path = target ? target : pf.target;
    if (!path) {
        fprintf(stderr, "patch: no file to patch and none named in the diff\n");
        return 2;
    }

    exit_status = apply(&pf, path);

    for (size_t k = 0; k < pf.nh; k++) {
        for (size_t i = 0; i < pf.h[k].old_n; i++) free(pf.h[k].old_line[i]);
        for (size_t i = 0; i < pf.h[k].new_n; i++) free(pf.h[k].new_line[i]);
        free(pf.h[k].old_line);
        free(pf.h[k].new_line);
    }
    free(pf.h);
    free(pf.target);
    return exit_status;
}
