/* grep -- print lines matching a pattern.
 *
 *     grep [-cEFHhilLnoqrsvwx] [-m NUM] [-A NUM] [-B NUM] [-C NUM]
 *          [-e PATTERN]... [-f FILE]... [pattern] [file ...]
 *
 * WHAT CHANGED AND WHY. This was fixed-string only, with a comment saying
 * "sbase uses POSIX <regex.h>; we don't have it". libtoby has had regex.h
 * for a while -- that comment outlived the limitation it described -- so
 * this is a real grep now: BRE by default, ERE with -E, and -F to force the
 * literal match the old code always did.
 *
 * It also had exactly four flags: -c -i -n -v. `grep -q` is one of the most
 * common constructs in shell scripts anywhere, and its absence made every
 * `if grep -q ...` in a script fail with a usage error rather than a
 * verdict. -l, -L, -o, -w, -x, -h, -H and -r were missing for the same
 * reason.
 *
 * 2026-09-06, closing the native-parity item that said "grep has no -m":
 *
 *   -m NUM   stop after NUM matching lines -- the thing that makes
 *            `grep -m1` on a huge file cheap, and the flag scripts reach
 *            for most often after -q.
 *   -e / -f  more than ONE pattern. Without these, `grep -e a -e b` was a
 *            usage error and the idiom had to be rewritten as an ERE.
 *   -A -B -C context, with GNU's separators: `:` after the filename on a
 *            matching line, `-` on a context line, and a `--` line between
 *            non-adjacent groups.
 *   -s       suppress the "no such file" diagnostics.
 *
 * Two real bugs went with them:
 *   - `-o` printed only the FIRST match on a line. GNU prints every
 *     non-overlapping match, one per line, and `grep -o` is almost always
 *     used to extract several.
 *   - `-w` and `-x` gave up at the first regex match instead of looking
 *     further along the line, so `grep -w cat` missed "concat cat".
 *
 * Exit 0 if any line matched, 1 if none, 2 on error -- POSIX. sbase's
 * eprintf() exits 1 and already prefixes argv0, so the error paths use
 * enprintf(2, ...) without repeating the program name.
 */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>
#include <dirent.h>
#include <sys/stat.h>

#include "util.h"

static int cflag, iflag, nflag, vflag, qflag, lflag, Lflag;
static int oflag, wflag, xflag, hflag, Hflag, rflag, Fflag, Eflag, sflag;
static long mflag = -1;                 /* -m: max matching lines, -1 = all */
static long aftctx, befctx;             /* -A / -B (and -C sets both) */
static int  any_error;

/* Every -e and -f pattern, plus the bare one. A line matches if ANY of
 * them does, which is what makes -e repeatable. */
struct pat {
    char   *text;
    size_t  len;
    regex_t re;
    int     compiled;
};
static struct pat *pats;
static int         npats;

static void usage(void) {
    enprintf(2, "usage: %s [-cEFHhilLnoqrsvwx] [-m NUM] [-A NUM] [-B NUM] "
                "[-C NUM] [-e PATTERN] [-f FILE] [pattern] [file ...]", argv0);
}

static void add_pattern(const char *s) {
    pats = erealloc(pats, sizeof(*pats) * (size_t)(npats + 1));
    pats[npats].text = estrdup(s);
    pats[npats].len = strlen(s);
    pats[npats].compiled = 0;
    npats++;
}

static void add_pattern_file(const char *path) {
    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
    if (!f) enprintf(2, "%s:", path);
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        add_pattern(buf);
    }
    if (f != stdin) fclose(f);
}

/* Fixed-string search, used for -F and as the fallback when a pattern will
 * not compile as a regex. */
static const char *fixed_find(const struct pat *p, const char *line) {
    if (p->len == 0) return line;
    size_t llen = strlen(line);
    if (llen < p->len) return 0;
    for (size_t i = 0; i + p->len <= llen; i++) {
        size_t k;
        for (k = 0; k < p->len; k++) {
            char a = line[i + k], b = p->text[k];
            if (iflag) { a = (char)tolower((unsigned char)a);
                         b = (char)tolower((unsigned char)b); }
            if (a != b) break;
        }
        if (k == p->len) return line + i;
    }
    return 0;
}

static int is_word_char(int c) { return isalnum(c) || c == '_'; }

/* Find the leftmost match at or after `from`, across every pattern.
 * Returns 1 and fills in the start and length, or 0. -w and -x are
 * applied here, and a
 * candidate they reject does not end the search: the scan continues from
 * the next character, which is what GNU does and what the old code did
 * not -- it returned "no match" the moment the first candidate failed. */
static int find_match(const char *line, size_t from, size_t *ms, size_t *ml) {
    /* The caller has already stripped the line terminator, so the string
     * length IS the line length -- which is what -x has to compare
     * against. */
    size_t llen = strlen(line);

    for (size_t base = from; base <= llen; base++) {
        size_t best_s = 0, best_l = 0;
        int    have = 0;

        for (int i = 0; i < npats; i++) {
            size_t s, l;
            if (pats[i].compiled) {
                regmatch_t m;
                if (regexec(&pats[i].re, line + base, 1, &m,
                            base ? REG_NOTBOL : 0) != 0)
                    continue;
                s = base + (size_t)m.rm_so;
                l = (size_t)(m.rm_eo - m.rm_so);
            } else {
                const char *p = fixed_find(&pats[i], line + base);
                if (!p) continue;
                s = (size_t)(p - line);
                l = pats[i].len;
            }
            /* leftmost wins; on a tie the longer one wins */
            if (!have || s < best_s || (s == best_s && l > best_l)) {
                best_s = s; best_l = l; have = 1;
            }
        }
        if (!have) return 0;

        if (wflag) {
            int before_ok = (best_s == 0) ||
                            !is_word_char((unsigned char)line[best_s - 1]);
            int after_ok  = !line[best_s + best_l] ||
                            line[best_s + best_l] == '\n' ||
                            !is_word_char((unsigned char)line[best_s + best_l]);
            if (!before_ok || !after_ok) { base = best_s; continue; }
        }
        if (xflag && !(best_s == 0 && best_l == llen)) { base = best_s; continue; }

        *ms = best_s;
        *ml = best_l;
        return 1;
    }
    return 0;
}

static int line_matches(const char *line, size_t *ms, size_t *ml) {
    size_t s = 0, l = 0;
    if (!find_match(line, 0, &s, &l)) return 0;
    /* A pattern like [0-9]* matches the EMPTY string at offset 0, so a
     * naive -o prints a blank line where GNU prints "123". Skip
     * zero-length matches and keep looking; a pattern that can only ever
     * match empty still reports a hit. */
    if (oflag && l == 0) {
        size_t base = s;
        while (l == 0 && line[base]) {
            if (!find_match(line, base + 1, &s, &l)) return 1;
            base = s;
        }
    }
    if (ms) *ms = s;
    if (ml) *ml = l;
    return 1;
}

/* ---- context (-A/-B/-C) ---------------------------------------- */

struct ctxline { char *text; long no; };
static struct ctxline *ring;            /* the last `befctx` non-matching lines */
static long ring_n, ring_head;
static long last_printed;               /* line number of the last line printed */
static int  group_open;

static void ring_reset(void) {
    for (long i = 0; i < befctx; i++) { free(ring[i].text); ring[i].text = 0; }
    ring_n = ring_head = 0;
    last_printed = 0;
    group_open = 0;
}

static void ring_push(const char *s, long no) {
    if (befctx <= 0) return;
    long slot = (ring_head + ring_n) % befctx;
    if (ring_n == befctx) { free(ring[ring_head].text); ring_head = (ring_head + 1) % befctx; }
    else ring_n++;
    ring[slot].text = estrdup(s);
    ring[slot].no = no;
}

static void emit(const char *label, int show_label, long no, const char *text,
                 char sep) {
    if (show_label) printf("%s%c", label, sep);
    if (nflag)      printf("%ld%c", no, sep);
    fputs(text, stdout);
    putchar('\n');
    last_printed = no;
}

/* ---- the scan --------------------------------------------------- */

static int grep_stream(FILE *f, const char *label, int show_label) {
    char  buf[8192];
    long  lineno = 0, matches = 0, after = 0;
    int   any = 0;
    int   ctx = (aftctx > 0 || befctx > 0);

    ring_reset();
    if (mflag == 0) return 0;      /* -m 0 asks for no matches at all */

    while (fgets(buf, sizeof buf, f)) {
        lineno++;
        /* STRIP THE TRAILING NEWLINE BEFORE MATCHING.
         *
         * fgets keeps it, and the pattern is matched against the whole
         * buffer -- so `$` (which anchors at the end of the STRING) sat
         * one character short and `grep 'x$'`, `grep '^x$'` and every
         * anchored pattern in the tree matched NOTHING. It reported "no
         * lines", never an error, which is why it lasted. GNU grep strips
         * it for exactly this reason. */
        {
            size_t L = strlen(buf);
            while (L && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = 0;
        }
        size_t ms = 0, ml = 0;
        int hit = line_matches(buf, &ms, &ml);
        if (vflag) hit = !hit;

        if (!hit) {
            if (after > 0) {
                emit(label, show_label, lineno, buf, '-');
                after--;
            } else {
                ring_push(buf, lineno);
            }
            continue;
        }

        matches++;
        any = 1;
        /* -q says nothing and stops at the first match: that is the whole
         * point of it, and reading the rest of a huge file would defeat it. */
        if (qflag) return 1;
        if (cflag || lflag || Lflag) {
            if (mflag >= 0 && matches >= mflag) break;
            continue;
        }

        if (ctx) {
            /* GNU separates non-adjacent groups with a bare `--`. */
            long first = lineno - ring_n;
            if (group_open && first > last_printed + 1) puts("--");
            for (long i = 0; i < ring_n; i++) {
                long slot = (ring_head + i) % befctx;
                emit(label, show_label, ring[slot].no, ring[slot].text, '-');
            }
            ring_reset();
            group_open = 1;
        }

        if (oflag && !vflag) {
            /* EVERY non-overlapping match, one per line, as GNU does --
             * and never a zero-length one. A pattern that can only match
             * empty still counts the line as a hit (GNU exits 0), it just
             * has nothing to show, so printing a blank line for it was
             * output GNU does not produce. */
            size_t from = 0, s = ms, l = ml;
            for (;;) {
                if (l > 0) {
                    if (show_label) printf("%s:", label);
                    if (nflag)      printf("%ld:", lineno);
                    printf("%.*s\n", (int)l, buf + s);
                    last_printed = lineno;
                }
                from = s + (l ? l : 1);
                if (!buf[from]) break;
                if (!find_match(buf, from, &s, &l)) break;
            }
        } else {
            emit(label, show_label, lineno, buf, ':');
        }
        after = aftctx;

        if (mflag >= 0 && matches >= mflag) {
            /* GNU still prints the trailing context of the last match. */
            while (after > 0 && fgets(buf, sizeof buf, f)) {
                lineno++;
                emit(label, show_label, lineno, buf, '-');
                after--;
            }
            break;
        }
    }
    if (cflag && !qflag) {
        if (show_label) printf("%s:", label);
        printf("%ld\n", matches);
    }
    if (lflag && any)  printf("%s\n", label);
    if (Lflag && !any) printf("%s\n", label);
    return any;
}

static int grep_path(const char *path, int show_label);

static int grep_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) { if (!sflag) weprintf("%s:", path); any_error = 1; return 0; }
    int any = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (grep_path(sub, 1)) any = 1;
    }
    closedir(d);
    return any;
}

static int grep_path(const char *path, int show_label) {
    struct stat st;
    if (rflag && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return grep_dir(path);
    FILE *f = fopen(path, "r");
    if (!f) { if (!sflag) weprintf("%s:", path); any_error = 1; return 0; }
    int any = grep_stream(f, path, show_label);
    fclose(f);
    return any;
}

static long numarg(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < 0) enprintf(2, "invalid number: %s", s);
    return v;
}

int main(int argc, char *argv[]) {
    /* egrep and fgrep are this program under another name, exactly as they
     * are everywhere else. */
    const char *base = argv[0] ? argv[0] : "grep";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    /* Diagnostics name the tool, not the path it was found at -- which is
     * what GNU does and what a script grepping stderr expects. */
    util_argv0_set(base);
    if (!strcmp(base, "egrep")) Eflag = 1;
    if (!strcmp(base, "fgrep")) Fflag = 1;
    if (!strcmp(base, "rgrep")) { rflag = 1; }

    int opt, sawpat = 0;
    while ((opt = getopt(argc, argv, "cEFHhilLnoqrsvwxm:A:B:C:e:f:")) != -1) {
        switch (opt) {
        case 'c': cflag = 1; break;
        case 'E': Eflag = 1; break;
        case 'F': Fflag = 1; break;
        case 'H': Hflag = 1; break;
        case 'h': hflag = 1; break;
        case 'i': iflag = 1; break;
        case 'l': lflag = 1; break;
        case 'L': Lflag = 1; break;
        case 'n': nflag = 1; break;
        case 'o': oflag = 1; break;
        case 'q': qflag = 1; break;
        case 'r': rflag = 1; break;
        case 's': sflag = 1; break;
        case 'v': vflag = 1; break;
        case 'w': wflag = 1; break;
        case 'x': xflag = 1; break;
        case 'm': mflag = numarg(optarg); break;
        case 'A': aftctx = numarg(optarg); break;
        case 'B': befctx = numarg(optarg); break;
        case 'C': aftctx = befctx = numarg(optarg); break;
        case 'e': add_pattern(optarg); sawpat = 1; break;
        case 'f': add_pattern_file(optarg); sawpat = 1; break;
        default:  usage();
        }
    }
    /* The bare pattern operand is present only when no -e/-f gave one. */
    if (!sawpat) {
        if (optind >= argc) usage();
        add_pattern(argv[optind++]);
    }
    if (npats == 0) {
        /* `grep -f /dev/null` matches nothing, and says so by exiting 1. */
        return 1;
    }

    if (befctx > 0) {
        ring = emalloc(sizeof(*ring) * (size_t)befctx);
        memset(ring, 0, sizeof(*ring) * (size_t)befctx);
    }

    if (!Fflag) {
        for (int i = 0; i < npats; i++) {
            int cf = REG_NOSUB;
            if (Eflag) cf |= REG_EXTENDED;
            if (iflag) cf |= REG_ICASE;
            /* -o and -w need the match OFFSETS, so REG_NOSUB has to come
             * off for them or rm_so/rm_eo come back meaningless. */
            if (oflag || wflag || xflag) cf &= ~REG_NOSUB;
            if (regcomp(&pats[i].re, pats[i].text, cf) == 0) pats[i].compiled = 1;
            else {
                /* An invalid regex is an error in real grep, not a silent
                 * downgrade to a literal search that would quietly match
                 * something else. */
                enprintf(2, "invalid pattern: %s", pats[i].text);
            }
        }
    }

    int nfiles = argc - optind;
    int show   = Hflag || (nfiles > 1 && !hflag) || (rflag && !hflag);
    int any    = 0;

    if (nfiles <= 0) {
        any = grep_stream(stdin, "(standard input)", Hflag);
    } else {
        for (int i = optind; i < argc; i++)
            if (grep_path(argv[i], show)) any = 1;
    }
    for (int i = 0; i < npats; i++) if (pats[i].compiled) regfree(&pats[i].re);
    if (any_error && !any) return 2;
    return any ? 0 : 1;
}
