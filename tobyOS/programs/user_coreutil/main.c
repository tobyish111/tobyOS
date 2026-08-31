/* user_coreutil -- the GNU coreutils commands tobyOS was missing.
 *
 * A census against the Debian/Ubuntu/RHEL/SUSE coreutils set (105 commands)
 * found 86 present, between the purpose-built binaries in /bin and the
 * busybox applets linked onto PATH. These are the other 19.
 *
 * ONE binary, dispatched on argv[0], with the initrd hard-linking each name
 * to it -- exactly how busybox is already installed in this tree. Nineteen
 * separate programs would have meant nineteen sets of Makefile entries and
 * nineteen copies of crt0 and libtoby.
 *
 * Where a command cannot do its real job on this system, it says so and
 * fails, rather than succeeding silently: chcon and runcon need SELinux,
 * which tobyOS does not have, and GNU's own chcon reports exactly that on a
 * kernel without it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

#define LINE_CAP 8192

static const char *g_prog = "coreutil";

static int err(const char *msg, const char *arg) {
    if (arg) fprintf(stderr, "%s: %s: %s\n", g_prog, arg, msg);
    else     fprintf(stderr, "%s: %s\n", g_prog, msg);
    return 1;
}

/* fgets into a growable-enough buffer; returns 0 at EOF. libtoby has no
 * getline(), and every applet here is line-oriented. */
static int readline_into(FILE *f, char *buf, size_t cap) {
    if (!fgets(buf, (int)cap, f)) return 0;
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') buf[n - 1] = '\0';
    return 1;
}

static FILE *open_or_stdin(int argc, char **argv, int idx) {
    if (idx >= argc) return stdin;
    FILE *f = fopen(argv[idx], "r");
    if (!f) { err(strerror(errno), argv[idx]); return 0; }
    return f;
}

/* ---------------------------------------------------------------- tsort
 * Topological sort of "a b" pairs read from input: a must precede b.
 * A cycle is an ERROR, not a silent ordering -- make(1) and friends rely on
 * tsort to detect exactly that. */
#define TS_MAX 512
static int cmd_tsort(int argc, char **argv) {
    static char names[TS_MAX][64];
    static unsigned char edge[TS_MAX][TS_MAX];
    static int indeg[TS_MAX], done[TS_MAX];
    int n = 0;

    FILE *f = open_or_stdin(argc, argv, 1);
    if (!f) return 1;

    char line[LINE_CAP];
    char pend[64];
    int have_pend = 0;
    while (readline_into(f, line, sizeof line)) {
        char *p = line;
        for (;;) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char tok[64];
            size_t k = 0;
            while (*p && *p != ' ' && *p != '\t' && k + 1 < sizeof tok) tok[k++] = *p++;
            tok[k] = 0;
            int id = -1;
            for (int i = 0; i < n; i++) if (!strcmp(names[i], tok)) { id = i; break; }
            if (id < 0) {
                if (n >= TS_MAX) { if (f != stdin) fclose(f); return err("too many items", 0); }
                snprintf(names[n], sizeof names[0], "%s", tok);
                id = n++;
            }
            if (!have_pend) { snprintf(pend, sizeof pend, "%s", tok); have_pend = 1; }
            else {
                int a = -1;
                for (int i = 0; i < n; i++) if (!strcmp(names[i], pend)) { a = i; break; }
                /* a==b is a self-loop; GNU tsort ignores it rather than
                 * reporting a cycle, because "x x" only says x exists. */
                if (a >= 0 && a != id && !edge[a][id]) { edge[a][id] = 1; indeg[id]++; }
                have_pend = 0;
            }
        }
    }
    if (f != stdin) fclose(f);
    if (have_pend) return err("input contains an odd number of tokens", 0);

    int emitted = 0;
    for (;;) {
        int pick = -1;
        for (int i = 0; i < n; i++)
            if (!done[i] && indeg[i] == 0) { pick = i; break; }
        if (pick < 0) break;
        printf("%s\n", names[pick]);
        done[pick] = 1;
        emitted++;
        for (int j = 0; j < n; j++)
            if (edge[pick][j]) { edge[pick][j] = 0; indeg[j]--; }
    }
    if (emitted != n) {
        fprintf(stderr, "%s: input contains a loop\n", g_prog);
        for (int i = 0; i < n; i++) if (!done[i]) fprintf(stderr, "%s: %s\n", g_prog, names[i]);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ fmt
 * Reflow to a width, preserving blank lines as paragraph breaks and keeping
 * each paragraph's leading indent. */
static int cmd_fmt(int argc, char **argv) {
    int width = 75;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (argv[argi][1] == 'w' && argv[argi][2]) { width = atoi(argv[argi] + 2); continue; }
        if (!strcmp(argv[argi], "-w") && argi + 1 < argc) { width = atoi(argv[++argi]); continue; }
        if (isdigit((unsigned char)argv[argi][1])) { width = atoi(argv[argi] + 1); continue; }
        return err("unknown option", argv[argi]);
    }
    if (width < 1) width = 75;

    FILE *f = open_or_stdin(argc, argv, argi);
    if (!f) return 1;

    char line[LINE_CAP];
    char indent[64] = "";
    int col = 0;
    while (readline_into(f, line, sizeof line)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) {                        /* blank line ends the paragraph */
            if (col) { putchar('\n'); col = 0; }
            putchar('\n');
            indent[0] = 0;
            continue;
        }
        if (!col) {                       /* first line sets the indent */
            size_t k = 0;
            while (line[k] && (line[k] == ' ' || line[k] == '\t') &&
                   k + 1 < sizeof indent) { indent[k] = line[k]; k++; }
            indent[k] = 0;
        }
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            const char *w = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int wl = (int)(p - w);
            /* GNU fmt does not fill greedily to the maximum width: it
             * aims at a GOAL of about 93% of it and only overshoots when
             * a word would otherwise not fit at all. A greedy fill gives
             * legal lines that break in different places from every
             * other fmt -- which for a text formatter is the whole of
             * the observable behaviour. */
            int goal = (width * 93) / 100; if (goal < 1) goal = 1;
            if (col && col + 1 + wl > goal) { putchar('\n'); col = 0; }
            if (!col) { fputs(indent, stdout); col = (int)strlen(indent); }
            else      { putchar(' '); col++; }
            fwrite(w, 1, (size_t)wl, stdout);
            col += wl;
        }
    }
    if (col) putchar('\n');
    if (f != stdin) fclose(f);
    return 0;
}

/* GNU verifies order AS IT READS, which is why its diagnostic appears
 * before any matched line -- checking at compare time (as this did first)
 * lets an already-matched pair print ahead of the warning and the transcript
 * comes out in a different order from GNU for the same input. */
static int join_check_order(const char *prev, const char *cur,
                            const char *fname, int lineno,
                            const char *line, int *flagged) {
    if (*flagged || !prev[0] || strcmp(cur, prev) >= 0) return 0;
    fflush(stdout);
    fprintf(stderr, "%s: %s:%d: is not sorted: %s\n", g_prog, fname, lineno, line);
    *flagged = 1;
    return 1;
}

/* ----------------------------------------------------------------- join
 * Relational join on one field of two SORTED files. */
static int cmd_join(int argc, char **argv) {
    int f1 = 1, f2 = 1;                 /* 1-based join fields */
    char sep = 0;                       /* 0 = runs of blanks */
    int argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (!strcmp(argv[argi], "-1") && argi + 1 < argc) { f1 = atoi(argv[++argi]); continue; }
        if (!strcmp(argv[argi], "-2") && argi + 1 < argc) { f2 = atoi(argv[++argi]); continue; }
        if (!strcmp(argv[argi], "-j") && argi + 1 < argc) { f1 = f2 = atoi(argv[++argi]); continue; }
        if (!strcmp(argv[argi], "-t") && argi + 1 < argc) { sep = argv[++argi][0]; continue; }
        return err("unknown option", argv[argi]);
    }
    if (argi + 1 >= argc) return err("missing operand", 0);
    FILE *a = fopen(argv[argi], "r");
    if (!a) return err(strerror(errno), argv[argi]);
    FILE *b = fopen(argv[argi + 1], "r");
    if (!b) { fclose(a); return err(strerror(errno), argv[argi + 1]); }

    /* Split `s` into fields; returns the count and fills starts/lens. */
    #define MAXF 64
    char la[LINE_CAP], lb[LINE_CAP];
    char ka[256], kb[256];
    int have_a = readline_into(a, la, sizeof la);
    int have_b = readline_into(b, lb, sizeof lb);

    /* field(s, n, out) -> 1 on success */
    #define FIELD(src, num, out) do {                                        \
        const char *q = (src); int fn = 1; out[0] = 0;                       \
        for (;;) {                                                           \
            if (!sep) while (*q == ' ' || *q == '\t') q++;                   \
            const char *st = q;                                              \
            while (*q && (sep ? *q != sep : (*q != ' ' && *q != '\t'))) q++;  \
            if (fn == (num)) {                                               \
                size_t L = (size_t)(q - st);                                  \
                if (L >= sizeof(out)) L = sizeof(out) - 1;                    \
                memcpy(out, st, L); out[L] = 0;                               \
                break;                                                        \
            }                                                                 \
            if (!*q) break;                                                   \
            q++; fn++;                                                        \
        }                                                                     \
    } while (0)

    /* GNU join VERIFIES its inputs are sorted and names the first line
     * that is not. A join over unsorted input silently drops matches --
     * a wrong answer that looks exactly like a right one. */
    char pa[256], pb[256];
    int unsorted = 0, na = 1, nb = 1;   /* lines read from each file */
    pa[0] = 0; pb[0] = 0;
    while (have_a && have_b) {
        FIELD(la, f1, ka);
        FIELD(lb, f2, kb);

        snprintf(pa, sizeof pa, "%s", ka);
        snprintf(pb, sizeof pb, "%s", kb);
        int c = strcmp(ka, kb);
        if (c < 0)      { have_a = readline_into(a, la, sizeof la); na++;
                          if (have_a) { char nk[256]; FIELD(la, f1, nk);
                              join_check_order(ka, nk, argv[argi], na, la, &unsorted); } }
        else if (c > 0) { have_b = readline_into(b, lb, sizeof lb); nb++;
                          if (have_b) { char nk[256]; FIELD(lb, f2, nk);
                              join_check_order(kb, nk, argv[argi + 1], nb, lb, &unsorted); } }
        else {
            /* GNU prints the key, then the rest of file 1, then of file 2. */
            printf("%s", ka);
            const char *q = la;
            int fn = 1;
            for (;;) {
                if (!sep) while (*q == ' ' || *q == '\t') q++;
                const char *st = q;
                while (*q && (sep ? *q != sep : (*q != ' ' && *q != '\t'))) q++;
                if (fn != f1 && q > st) printf("%c%.*s", sep ? sep : ' ', (int)(q - st), st);
                if (!*q) break;
                q++; fn++;
            }
            q = lb; fn = 1;
            for (;;) {
                if (!sep) while (*q == ' ' || *q == '\t') q++;
                const char *st = q;
                while (*q && (sep ? *q != sep : (*q != ' ' && *q != '\t'))) q++;
                if (fn != f2 && q > st) printf("%c%.*s", sep ? sep : ' ', (int)(q - st), st);
                if (!*q) break;
                q++; fn++;
            }
            putchar('\n');
            have_a = readline_into(a, la, sizeof la); na++;
            if (have_a) { char nk[256]; FIELD(la, f1, nk);
                join_check_order(ka, nk, argv[argi], na, la, &unsorted); }
        }
    }
    fclose(a); fclose(b);
    if (unsorted) {
        fflush(stdout);
        fprintf(stderr, "%s: input is not in sorted order\n", g_prog);
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- pathchk
 * Reports names that are not portable filenames. */
static int cmd_pathchk(int argc, char **argv) {
    int portable = 0, argi = 1, rc = 0;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-p") || !strcmp(argv[argi], "-P")) { portable = 1; continue; }
        if (argv[argi][0] == '-' && argv[argi][1]) return err("unknown option", argv[argi]);
        break;
    }
    if (argi >= argc) return err("missing operand", 0);
    for (; argi < argc; argi++) {
        const char *p = argv[argi];
        size_t len = strlen(p);
        if (len == 0) { fprintf(stderr, "%s: empty file name\n", g_prog); rc = 1; continue; }
        if (len > 255) {
            fprintf(stderr, "%s: name '%s' is too long\n", g_prog, p);
            rc = 1; continue;
        }
        if (portable) {
            int bad = 0;
            char badc = 0;
            for (const char *q = p; *q; q++) {
                if (isalnum((unsigned char)*q) || *q == '.' || *q == '_' ||
                    *q == '-' || *q == '/') continue;
                bad = 1; badc = *q; break;
            }
            if (bad) {
                /* GNU's exact wording, character included: pathchk output is
                 * something build scripts grep, so the phrasing is part
                 * of the interface rather than decoration. */
                fprintf(stderr, "%s: nonportable character '%c' in file name '%s'\n",
                        g_prog, badc, p);
                rc = 1;
            }
        }
    }
    return rc;
}

/* ------------------------------------------------------------------- dir
 * dir and vdir are ls with a fixed default format -- that IS the whole of
 * them in coreutils, so they exec the real ls rather than reimplementing it
 * and drifting from its behaviour. */
static int run_ls(int argc, char **argv, const char *flag) {
    char *av[64];
    int n = 0;
    av[n++] = (char *)"ls";
    if (flag) av[n++] = (char *)flag;
    for (int i = 1; i < argc && n < 62; i++) av[n++] = argv[i];
    av[n] = 0;
    execv("/bin/ls", av);
    return err("cannot run /bin/ls", 0);
}
/* sbase ls accepts only -aFl, so `dir` passes NO flag: -C would be rejected
 * as an unknown option and print a usage error where a listing belongs. Its
 * default single-column output is what dir shows here. */
static int cmd_dir(int argc, char **argv)  { return run_ls(argc, argv, 0); }
static int cmd_vdir(int argc, char **argv) { return run_ls(argc, argv, "-l"); }

/* ------------------------------------------------------------- dircolors
 * Emits a settable LS_COLORS. The database is small on purpose: a wrong
 * colour is harmless, but claiming to have read a real dir_colors file we
 * never parsed would not be. */
static int cmd_dircolors(int argc, char **argv) {
    int csh = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--csh")) csh = 1;
        else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--sh")) csh = 0;
        else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--print-database")) {
            printf("# tobyOS dircolors defaults\n");
            printf("NORMAL 00\nFILE 00\nDIR 01;34\nLINK 01;36\nEXEC 01;32\n");
            return 0;
        }
    }
    const char *db = "rs=0:di=01;34:ln=01;36:ex=01;32:*.tar=01;31:*.gz=01;31:"
                     "*.zip=01;31:*.png=01;35:*.jpg=01;35:*.c=00;33:*.h=00;33";
    if (csh) printf("setenv LS_COLORS '%s'\n", db);
    else     printf("LS_COLORS='%s'; export LS_COLORS\n", db);
    return 0;
}

/* ---------------------------------------------------------------- csplit
 * Splits a file at line numbers, writing xx00, xx01, ... */
static int cmd_csplit(int argc, char **argv) {
    const char *prefix = "xx";
    int argi = 1, quiet = 0;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (!strcmp(argv[argi], "-f") && argi + 1 < argc) { prefix = argv[++argi]; continue; }
        if (!strcmp(argv[argi], "-s") || !strcmp(argv[argi], "-q") ||
            !strcmp(argv[argi], "--silent")) { quiet = 1; continue; }
        if (!strcmp(argv[argi], "-k")) continue;
        return err("unknown option", argv[argi]);
    }
    if (argi >= argc) return err("missing operand", 0);
    FILE *f = fopen(argv[argi], "r");
    if (!f) return err(strerror(errno), argv[argi]);
    argi++;

    int cuts[64], ncut = 0;
    for (; argi < argc && ncut < 64; argi++) {
        if (argv[argi][0] < '0' || argv[argi][0] > '9') {
            fclose(f);
            return err("only line-number patterns are supported", argv[argi]);
        }
        cuts[ncut++] = atoi(argv[argi]);
    }

    char line[LINE_CAP];
    int lineno = 0, part = 0, ci = 0;
    char name[256];
    snprintf(name, sizeof name, "%s%02d", prefix, part);
    FILE *out = fopen(name, "w");
    if (!out) { fclose(f); return err(strerror(errno), name); }
    long bytes = 0;

    while (readline_into(f, line, sizeof line)) {
        lineno++;
        if (ci < ncut && lineno == cuts[ci]) {
            fclose(out);
            if (!quiet) printf("%ld\n", bytes);
            bytes = 0;
            part++; ci++;
            snprintf(name, sizeof name, "%s%02d", prefix, part);
            out = fopen(name, "w");
            if (!out) { fclose(f); return err(strerror(errno), name); }
        }
        fprintf(out, "%s\n", line);
        bytes += (long)strlen(line) + 1;
    }
    fclose(out);
    if (!quiet) printf("%ld\n", bytes);
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------- pr
 * Paginate for printing: a header per page, then the lines. */
static int cmd_pr(int argc, char **argv) {
    int lines_per_page = 66, header_lines = 5, body = 56;
    const char *header = 0;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (!strcmp(argv[argi], "-h") && argi + 1 < argc) { header = argv[++argi]; continue; }
        if (!strcmp(argv[argi], "-t")) { header_lines = 0; body = lines_per_page; continue; }
        if (argv[argi][1] == 'l' && argv[argi][2]) {
            lines_per_page = atoi(argv[argi] + 2);
            body = lines_per_page - 10; if (body < 1) body = 1;
            continue;
        }
        return err("unknown option", argv[argi]);
    }
    const char *fname = (argi < argc) ? argv[argi] : "";
    FILE *f = open_or_stdin(argc, argv, argi);
    if (!f) return 1;
    if (!header) header = fname;

    char line[LINE_CAP];
    int page = 1, onpage = 0;
    while (readline_into(f, line, sizeof line)) {
        if (onpage == 0 && header_lines) {
            printf("\n\n%s  Page %d\n\n\n", header[0] ? header : "", page);
        }
        printf("%s\n", line);
        onpage++;
        if (onpage >= body) { onpage = 0; page++; }
    }
    if (f != stdin) fclose(f);
    return 0;
}

/* ---------------------------------------------------------------- numfmt
 * Human-readable <-> plain numbers.
 *
 * INTEGER ARITHMETIC ONLY. These programs build with -mno-sse, so a double
 * return is a compile error ("SSE register return with SSE disabled") -- and
 * enabling SSE for one applet to format numbers would be the wrong trade.
 * The one decimal place GNU shows for small values is produced by scaling
 * by 10 before dividing, not by floating point.
 */
static int cmd_numfmt(int argc, char **argv) {
    int to_human = 0, from_human = 0;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (!strncmp(argv[argi], "--to=", 5))   { to_human = 1; continue; }
        if (!strncmp(argv[argi], "--from=", 7)) { from_human = 1; continue; }
        if (!strcmp(argv[argi], "--to") && argi + 1 < argc)   { argi++; to_human = 1; continue; }
        if (!strcmp(argv[argi], "--from") && argi + 1 < argc) { argi++; from_human = 1; continue; }
        return err("unknown option", argv[argi]);
    }
    char line[LINE_CAP];
    int used_args = argi < argc;
    for (;;) {
        const char *s;
        if (used_args) { if (argi >= argc) break; s = argv[argi++]; }
        else { if (!readline_into(stdin, line, sizeof line)) break; s = line; }

        if (from_human) {
            /* value, then at most one fractional digit, then a unit. */
            long long whole = 0, frac = 0;
            int neg = 0;
            const char *p = s;
            while (*p == ' ') p++;
            if (*p == '-') { neg = 1; p++; }
            while (isdigit((unsigned char)*p)) whole = whole * 10 + (*p++ - '0');
            if (*p == '.') { p++; if (isdigit((unsigned char)*p)) frac = *p++ - '0';
                             while (isdigit((unsigned char)*p)) p++; }
            long long mult = 1;
            switch (*p) {
            case 'K': case 'k': mult = 1000LL; break;
            case 'M': mult = 1000000LL; break;
            case 'G': mult = 1000000000LL; break;
            case 'T': mult = 1000000000000LL; break;
            default: break;
            }
            long long v = whole * mult + frac * (mult / 10);
            printf("%lld\n", neg ? -v : v);
        } else if (to_human) {
            long long v = 0;
            int neg = 0;
            const char *p = s;
            while (*p == ' ') p++;
            if (*p == '-') { neg = 1; p++; }
            while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
            const char *suf = "";
            long long div = 1;
            if      (v >= 1000000000000LL) { div = 1000000000000LL; suf = "T"; }
            else if (v >= 1000000000LL)    { div = 1000000000LL;    suf = "G"; }
            else if (v >= 1000000LL)       { div = 1000000LL;       suf = "M"; }
            else if (v >= 1000LL)          { div = 1000LL;          suf = "K"; }
            long long whole = v / div;
            /* GNU shows one decimal only while the mantissa is < 10. */
            if (suf[0] && whole < 10) {
                long long tenths = (v * 10 / div) % 10;
                printf("%s%lld.%lld%s\n", neg ? "-" : "", whole, tenths, suf);
            } else {
                printf("%s%lld%s\n", neg ? "-" : "", whole, suf);
            }
        } else {
            printf("%s\n", s);
        }
    }
    return 0;
}

/* ---------------------------------------------------------- chcon / runcon
 * SELinux. tobyOS has no security-context labelling at all, and GNU's own
 * chcon reports precisely this on a kernel without SELinux -- so failing
 * with that message is the CORRECT behaviour, not a stub. Succeeding
 * silently would tell a script its labels were applied when nothing was. */
static int cmd_chcon(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "%s: can't apply security context: "
                    "SELinux is not enabled on this system\n", g_prog);
    return 1;
}
static int cmd_runcon(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "%s: SELinux is not enabled on this system\n", g_prog);
    return 1;
}

/* ------------------------------------------------------------------ pinky
 * Lightweight finger, from /etc/passwd. */
static int cmd_pinky(int argc, char **argv) {
    int longfmt = 0;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (argv[argi][1] == 'l') longfmt = 1;
        else if (strchr("sfwiqbhp", argv[argi][1])) continue;
        else return err("unknown option", argv[argi]);
    }
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return err("cannot read /etc/passwd", 0);
    if (!longfmt) printf("Login      Name                 TTY      Idle\n");
    char line[LINE_CAP];
    while (readline_into(f, line, sizeof line)) {
        char *name = line;
        char *p = strchr(line, ':');
        if (!p) continue;
        *p = 0;
        char *rest = p + 1;
        char *gecos = rest;
        for (int i = 0; i < 3 && gecos; i++) {
            gecos = strchr(gecos, ':');
            if (gecos) gecos++;
        }
        char *gend = gecos ? strchr(gecos, ':') : 0;
        if (gend) *gend = 0;
        if (argi < argc) {
            int want = 0;
            for (int i = argi; i < argc; i++) if (!strcmp(argv[i], name)) want = 1;
            if (!want) continue;
        }
        if (longfmt) printf("Login name: %-20sIn real life: %s\n",
                            name, gecos ? gecos : "");
        else printf("%-10s %-20s %-8s %s\n", name, gecos ? gecos : "", "-", "-");
    }
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------- ptx
 * Permuted index in GNU's DEFAULT layout, which was derived by running GNU
 * ptx rather than guessed:
 *
 *     printf 'one two three' | ptx
 *                                  one two   three
 *                                      one   two three
 *
 * `before` is RIGHT-justified to end at column 36 (half of the default
 * width 72), then a 3-column gap, then the keyword and the rest from column
 * 39. Lines come out sorted by keyword. Trailing blanks are trimmed.
 *
 * The first version emitted roff `.xx "..."` macros, which is what ptx -R
 * produces -- readable, and not what anyone piping ptx into anything else
 * would get. */
#define PTX_HALF 36
#define PTX_GAP  3

struct ptx_line { char key[64]; char before[256]; char after[256]; };

static int ptx_cmp(const void *a, const void *b) {
    return strcmp(((const struct ptx_line *)a)->key,
                  ((const struct ptx_line *)b)->key);
}

static int cmd_ptx(int argc, char **argv) {
    FILE *f = open_or_stdin(argc, argv, 1);
    if (!f) return 1;
    static struct ptx_line out[512];
    int n = 0;
    char line[LINE_CAP];
    while (readline_into(f, line, sizeof line)) {
        char words[64][64];
        int nw = 0;
        const char *p = line;
        while (*p && nw < 64) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            size_t k = 0;
            while (*p && *p != ' ' && *p != '\t' && k + 1 < sizeof words[0])
                words[nw][k++] = *p++;
            words[nw][k] = 0;
            nw++;
        }
        for (int r = 0; r < nw && n < 512; r++) {
            struct ptx_line *L = &out[n++];
            snprintf(L->key, sizeof L->key, "%s", words[r]);
            L->before[0] = 0;
            for (int i = 0; i < r; i++) {
                if (i) strncat(L->before, " ", sizeof L->before - strlen(L->before) - 1);
                strncat(L->before, words[i], sizeof L->before - strlen(L->before) - 1);
            }
            L->after[0] = 0;
            for (int i = r; i < nw; i++) {
                if (i > r) strncat(L->after, " ", sizeof L->after - strlen(L->after) - 1);
                strncat(L->after, words[i], sizeof L->after - strlen(L->after) - 1);
            }
        }
    }
    if (f != stdin) fclose(f);
    qsort(out, (size_t)n, sizeof out[0], ptx_cmp);
    for (int i = 0; i < n; i++) {
        int bl = (int)strlen(out[i].before);
        int pad = PTX_HALF - bl;
        if (pad < 0) pad = 0;
        for (int k = 0; k < pad; k++) putchar(' ');
        fputs(out[i].before, stdout);
        for (int k = 0; k < PTX_GAP; k++) putchar(' ');
        fputs(out[i].after, stdout);
        putchar('\n');
    }
    return 0;
}

/* ---------------------------------------------------------------- stdbuf
 * GNU stdbuf works by preloading a shim into the child, which needs a
 * dynamic loader in the path this system does not use for its own binaries.
 * Rather than drop the buffering request silently, run the command and say
 * the request had no effect -- the command still does its job, and the
 * caller is told the one thing that did not happen. */
static int cmd_stdbuf(int argc, char **argv) {
    int argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (strchr("ioe", argv[argi][1])) {
            if (!argv[argi][2] && argi + 1 < argc) argi++;
            continue;
        }
        break;
    }
    if (argi >= argc) return err("missing command", 0);
    fprintf(stderr, "%s: buffering cannot be changed on this system; "
                    "running '%s' unmodified\n", g_prog, argv[argi]);
    execvp(argv[argi], &argv[argi]);
    return err(strerror(errno), argv[argi]);
}

/* ---------------------------------------------------------------- basenc
 * base32/base64/base16 encode and decode. */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static const char B16[] = "0123456789ABCDEF";

static int basenc_encode(FILE *f, int base, int wrap) {
    unsigned char buf[3];
    size_t n;
    int col = 0;
    if (base == 16) {
        int c;
        while ((c = fgetc(f)) != EOF) {
            putchar(B16[(c >> 4) & 0xF]);
            putchar(B16[c & 0xF]);
            col += 2;
            if (wrap && col >= wrap) { putchar('\n'); col = 0; }
        }
    } else if (base == 64) {
        while ((n = fread(buf, 1, 3, f)) > 0) {
            unsigned v = (unsigned)buf[0] << 16;
            if (n > 1) v |= (unsigned)buf[1] << 8;
            if (n > 2) v |= buf[2];
            putchar(B64[(v >> 18) & 63]);
            putchar(B64[(v >> 12) & 63]);
            putchar(n > 1 ? B64[(v >> 6) & 63] : '=');
            putchar(n > 2 ? B64[v & 63] : '=');
            col += 4;
            if (wrap && col >= wrap) { putchar('\n'); col = 0; }
        }
    } else {
        unsigned char g[5];
        while ((n = fread(g, 1, 5, f)) > 0) {
            unsigned long long v = 0;
            for (size_t i = 0; i < 5; i++) v = (v << 8) | (i < n ? g[i] : 0);
            static const int keep[6] = { 0, 2, 4, 5, 7, 8 };
            int out = keep[n];
            for (int i = 0; i < 8; i++)
                putchar(i < out ? B32[(v >> (35 - 5 * i)) & 31] : '=');
            col += 8;
            if (wrap && col >= wrap) { putchar('\n'); col = 0; }
        }
    }
    if (col) putchar('\n');
    return 0;
}

static int b64val(int c) {
    const char *p = strchr(B64, c);
    return p ? (int)(p - B64) : -1;
}

static int basenc_decode(FILE *f, int base) {
    int c;
    if (base == 16) {
        int hi = -1;
        while ((c = fgetc(f)) != EOF) {
            if (isspace(c)) continue;
            const char *p = strchr(B16, toupper(c));
            if (!p) return err("invalid input", 0);
            int v = (int)(p - B16);
            if (hi < 0) hi = v;
            else { putchar((hi << 4) | v); hi = -1; }
        }
        return 0;
    }
    if (base == 64) {
        int acc = 0, bits = 0;
        while ((c = fgetc(f)) != EOF) {
            if (isspace(c) || c == '=') continue;
            int v = b64val(c);
            if (v < 0) return err("invalid input", 0);
            acc = (acc << 6) | v; bits += 6;
            if (bits >= 8) { bits -= 8; putchar((acc >> bits) & 0xFF); }
        }
        return 0;
    }
    int acc = 0, bits = 0;
    while ((c = fgetc(f)) != EOF) {
        if (isspace(c) || c == '=') continue;
        const char *p = strchr(B32, toupper(c));
        if (!p) return err("invalid input", 0);
        acc = (acc << 5) | (int)(p - B32); bits += 5;
        if (bits >= 8) { bits -= 8; putchar((acc >> bits) & 0xFF); }
    }
    return 0;
}

static int cmd_basenc(int argc, char **argv) {
    int base = 64, decode = 0, wrap = 76, argi = 1;
    for (; argi < argc; argi++) {
        if (argv[argi][0] != '-' || !argv[argi][1]) break;
        if (!strcmp(argv[argi], "--base64"))      { base = 64; continue; }
        if (!strcmp(argv[argi], "--base32"))      { base = 32; continue; }
        if (!strcmp(argv[argi], "--base16"))      { base = 16; continue; }
        if (!strcmp(argv[argi], "-d") || !strcmp(argv[argi], "--decode")) { decode = 1; continue; }
        if (!strcmp(argv[argi], "-w") && argi + 1 < argc) { wrap = atoi(argv[++argi]); continue; }
        if (!strncmp(argv[argi], "-w", 2) && argv[argi][2]) { wrap = atoi(argv[argi] + 2); continue; }
        return err("unknown option", argv[argi]);
    }
    FILE *f = open_or_stdin(argc, argv, argi);
    if (!f) return 1;
    int rc = decode ? basenc_decode(f, base) : basenc_encode(f, base, wrap);
    if (f != stdin) fclose(f);
    return rc;
}

/* --------------------------------------------------------- SHA-2 (224/384)
 * sha224 and sha384 are the same cores as sha256/sha512 with different
 * initial values and a truncated output -- that is the entire difference,
 * and is why they are here rather than as separate implementations. */
typedef struct { unsigned int h[8]; unsigned long long len; unsigned char buf[64]; size_t n; } sha256_ctx;
static unsigned int ror32(unsigned int x, int r) { return (x >> r) | (x << (32 - r)); }
static const unsigned int K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

static void sha256_block(sha256_ctx *c, const unsigned char *p) {
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned)p[i*4] << 24) | ((unsigned)p[i*4+1] << 16) |
               ((unsigned)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = ror32(w[i-15],7) ^ ror32(w[i-15],18) ^ (w[i-15] >> 3);
        unsigned int s1 = ror32(w[i-2],17) ^ ror32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int t1 = h + S1 + ch + K256[i] + w[i];
        unsigned int S0 = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        unsigned int mj = (a & b) ^ (a & cc) ^ (b & cc);
        unsigned int t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
static void sha256_update(sha256_ctx *c, const unsigned char *p, size_t n) {
    c->len += n;
    while (n) {
        size_t k = 64 - c->n; if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}
static void sha256_final(sha256_ctx *c, unsigned char *out) {
    unsigned long long bits = c->len * 8;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    unsigned char z = 0;
    while (c->n != 56) sha256_update(c, &z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8*i));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

static int cmd_sha224(int argc, char **argv) {
    int argi = 1;
    for (; argi < argc; argi++) if (argv[argi][0] != '-' || !argv[argi][1]) break;
    int any = 0, rc = 0;
    do {
        const char *nm = (argi < argc) ? argv[argi] : "-";
        FILE *f = (argi < argc && strcmp(nm, "-")) ? fopen(nm, "rb") : stdin;
        if (!f) { rc = err(strerror(errno), nm); argi++; continue; }
        sha256_ctx c;
        /* SHA-224: the SHA-256 core with FIPS 180-4's other IV, truncated. */
        c.h[0]=0xc1059ed8; c.h[1]=0x367cd507; c.h[2]=0x3070dd17; c.h[3]=0xf70e5939;
        c.h[4]=0xffc00b31; c.h[5]=0x68581511; c.h[6]=0x64f98fa7; c.h[7]=0xbefa4fa4;
        c.len = 0; c.n = 0;
        unsigned char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) sha256_update(&c, b, n);
        unsigned char d[32];
        sha256_final(&c, d);
        for (int i = 0; i < 28; i++) printf("%02x", d[i]);
        printf("  %s\n", (argi < argc) ? nm : "-");
        if (f != stdin) fclose(f);
        any = 1;
        argi++;
    } while (argi < argc);
    (void)any;
    return rc;
}

/* SHA-384 is SHA-512's core; implemented with 64-bit words. */
typedef struct { unsigned long long h[8], len; unsigned char buf[128]; size_t n; } sha512_ctx;
static unsigned long long ror64(unsigned long long x, int r) { return (x >> r) | (x << (64 - r)); }
static const unsigned long long K512[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };

static void sha512_block(sha512_ctx *c, const unsigned char *p) {
    unsigned long long w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int k = 0; k < 8; k++) w[i] = (w[i] << 8) | p[i*8+k];
    }
    for (int i = 16; i < 80; i++) {
        unsigned long long s0 = ror64(w[i-15],1) ^ ror64(w[i-15],8) ^ (w[i-15] >> 7);
        unsigned long long s1 = ror64(w[i-2],19) ^ ror64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned long long a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 80; i++) {
        unsigned long long S1 = ror64(e,14) ^ ror64(e,18) ^ ror64(e,41);
        unsigned long long ch = (e & f) ^ ((~e) & g);
        unsigned long long t1 = h + S1 + ch + K512[i] + w[i];
        unsigned long long S0 = ror64(a,28) ^ ror64(a,34) ^ ror64(a,39);
        unsigned long long mj = (a & b) ^ (a & cc) ^ (b & cc);
        unsigned long long t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
static void sha512_update(sha512_ctx *c, const unsigned char *p, size_t n) {
    c->len += n;
    while (n) {
        size_t k = 128 - c->n; if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}
static void sha512_final(sha512_ctx *c, unsigned char *out) {
    unsigned long long bits = c->len * 8;
    unsigned char pad = 0x80, z = 0;
    sha512_update(c, &pad, 1);
    while (c->n != 112) sha512_update(c, &z, 1);
    unsigned char lb[16];
    memset(lb, 0, 8);
    for (int i = 0; i < 8; i++) lb[8+i] = (unsigned char)(bits >> (56 - 8*i));
    sha512_update(c, lb, 16);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 8; k++)
            out[i*8+k] = (unsigned char)(c->h[i] >> (56 - 8*k));
}

static int cmd_sha384(int argc, char **argv) {
    int argi = 1;
    for (; argi < argc; argi++) if (argv[argi][0] != '-' || !argv[argi][1]) break;
    int rc = 0;
    do {
        const char *nm = (argi < argc) ? argv[argi] : "-";
        FILE *f = (argi < argc && strcmp(nm, "-")) ? fopen(nm, "rb") : stdin;
        if (!f) { rc = err(strerror(errno), nm); argi++; continue; }
        sha512_ctx c;
        c.h[0]=0xcbbb9d5dc1059ed8ULL; c.h[1]=0x629a292a367cd507ULL;
        c.h[2]=0x9159015a3070dd17ULL; c.h[3]=0x152fecd8f70e5939ULL;
        c.h[4]=0x67332667ffc00b31ULL; c.h[5]=0x8eb44a8768581511ULL;
        c.h[6]=0xdb0c2e0d64f98fa7ULL; c.h[7]=0x47b5481dbefa4fa4ULL;
        c.len = 0; c.n = 0;
        unsigned char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) sha512_update(&c, b, n);
        unsigned char d[64];
        sha512_final(&c, d);
        for (int i = 0; i < 48; i++) printf("%02x", d[i]);
        printf("  %s\n", (argi < argc) ? nm : "-");
        if (f != stdin) fclose(f);
        argi++;
    } while (argi < argc);
    return rc;
}

/* ----------------------------------------------------------------- b2sum
 * BLAKE2b-512, RFC 7693. */
static const unsigned long long B2IV[8] = {
0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
static const unsigned char B2SIG[12][16] = {
{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
{11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},{7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
{9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},{2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
{12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},{13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
{6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},{10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0},
{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3} };

typedef struct { unsigned long long h[8], t; unsigned char buf[128]; size_t n; } b2_ctx;

static void b2_compress(b2_ctx *c, const unsigned char *blk, int last) {
    unsigned long long v[16], m[16];
    for (int i = 0; i < 8; i++) { v[i] = c->h[i]; v[i+8] = B2IV[i]; }
    v[12] ^= c->t;
    if (last) v[14] = ~v[14];
    for (int i = 0; i < 16; i++) {
        m[i] = 0;
        for (int k = 7; k >= 0; k--) m[i] = (m[i] << 8) | blk[i*8+k];
    }
    #define G(a,b,cx,d,x,y) do { \
        v[a]=v[a]+v[b]+(x); v[d]=ror64(v[d]^v[a],32); \
        v[cx]=v[cx]+v[d];   v[b]=ror64(v[b]^v[cx],24); \
        v[a]=v[a]+v[b]+(y); v[d]=ror64(v[d]^v[a],16); \
        v[cx]=v[cx]+v[d];   v[b]=ror64(v[b]^v[cx],63); } while (0)
    for (int r = 0; r < 12; r++) {
        const unsigned char *s = B2SIG[r];
        G(0,4,8,12,m[s[0]],m[s[1]]);   G(1,5,9,13,m[s[2]],m[s[3]]);
        G(2,6,10,14,m[s[4]],m[s[5]]);  G(3,7,11,15,m[s[6]],m[s[7]]);
        G(0,5,10,15,m[s[8]],m[s[9]]);  G(1,6,11,12,m[s[10]],m[s[11]]);
        G(2,7,8,13,m[s[12]],m[s[13]]); G(3,4,9,14,m[s[14]],m[s[15]]);
    }
    #undef G
    for (int i = 0; i < 8; i++) c->h[i] ^= v[i] ^ v[i+8];
}

static int cmd_b2sum(int argc, char **argv) {
    int argi = 1;
    for (; argi < argc; argi++) if (argv[argi][0] != '-' || !argv[argi][1]) break;
    int rc = 0;
    do {
        const char *nm = (argi < argc) ? argv[argi] : "-";
        FILE *f = (argi < argc && strcmp(nm, "-")) ? fopen(nm, "rb") : stdin;
        if (!f) { rc = err(strerror(errno), nm); argi++; continue; }
        b2_ctx c;
        for (int i = 0; i < 8; i++) c.h[i] = B2IV[i];
        c.h[0] ^= 0x01010000ULL ^ 64ULL;      /* no key, 64-byte digest */
        c.t = 0; c.n = 0;
        unsigned char in[128];
        size_t got;
        /* One block must be held back: BLAKE2's final compression is
         * flagged, so the last full block cannot be processed early. */
        int have = 0;
        unsigned char hold[128];
        while ((got = fread(in, 1, 128, f)) > 0) {
            if (have) { c.t += 128; b2_compress(&c, hold, 0); }
            memcpy(hold, in, got);
            c.n = got;
            have = 1;
            if (got < 128) break;
        }
        if (!have) { c.n = 0; memset(hold, 0, 128); }
        memset(hold + c.n, 0, 128 - c.n);
        c.t += c.n;
        b2_compress(&c, hold, 1);
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 8; k++)
                printf("%02x", (unsigned)((c.h[i] >> (8*k)) & 0xFF));
        printf("  %s\n", (argi < argc) ? nm : "-");
        if (f != stdin) fclose(f);
        argi++;
    } while (argi < argc);
    return rc;
}

/* ---------------------------------------------------------------- dispatch */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "tsort",     cmd_tsort },
    { "fmt",       cmd_fmt },
    { "join",      cmd_join },
    { "pathchk",   cmd_pathchk },
    { "dir",       cmd_dir },
    { "vdir",      cmd_vdir },
    { "dircolors", cmd_dircolors },
    { "csplit",    cmd_csplit },
    { "pr",        cmd_pr },
    { "numfmt",    cmd_numfmt },
    { "chcon",     cmd_chcon },
    { "runcon",    cmd_runcon },
    { "pinky",     cmd_pinky },
    { "ptx",       cmd_ptx },
    { "stdbuf",    cmd_stdbuf },
    { "basenc",    cmd_basenc },
    { "sha224sum", cmd_sha224 },
    { "sha384sum", cmd_sha384 },
    { "b2sum",     cmd_b2sum },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "coreutil";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    g_prog = base;

    for (int i = 0; g_applets[i].name; i++)
        if (!strcmp(base, g_applets[i].name))
            return g_applets[i].fn(argc, argv);

    /* Invoked by its own name: behave like busybox and take the applet as
     * the first argument, which is also how the gate can test it before the
     * hard links exist. */
    if (argc > 1) {
        for (int i = 0; g_applets[i].name; i++) {
            if (strcmp(argv[1], g_applets[i].name)) continue;
            g_prog = g_applets[i].name;
            return g_applets[i].fn(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "coreutil: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
