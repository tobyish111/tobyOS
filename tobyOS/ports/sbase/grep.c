/* grep -- print lines matching a pattern.
 *
 *     grep [-cEFHhilLnoqrvwx] pattern [file ...]
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
static int oflag, wflag, xflag, hflag, Hflag, rflag, Fflag, Eflag;
static const char *pattern;
static size_t      patlen;
static regex_t     preg;
static int         use_regex;

static void usage(void) {
    enprintf(2, "usage: %s [-cEFHhilLnoqrvwx] pattern [file ...]", argv0);
}

/* Fixed-string search, used for -F and as the fallback when a pattern will
 * not compile as a regex. */
static const char *fixed_find(const char *line) {
    if (patlen == 0) return line;
    size_t llen = strlen(line);
    if (llen < patlen) return 0;
    for (size_t i = 0; i + patlen <= llen; i++) {
        size_t k;
        for (k = 0; k < patlen; k++) {
            char a = line[i + k], b = pattern[k];
            if (iflag) { a = (char)tolower((unsigned char)a);
                         b = (char)tolower((unsigned char)b); }
            if (a != b) break;
        }
        if (k == patlen) return line + i;
    }
    return 0;
}

static int is_word_char(int c) { return isalnum(c) || c == '_'; }

/* Returns 1 on match; when `mstart`/`mlen` are given, reports WHERE, which
 * -o needs and -w uses to test the surrounding characters. */
static int line_matches(const char *line, size_t *mstart, size_t *mlen) {
    size_t s = 0, l = 0;
    int hit = 0;

    if (use_regex) {
        regmatch_t m;
        size_t base = 0;
        /* A pattern like [0-9]* matches the EMPTY string at offset 0, so a
         * naive -o prints a blank line where GNU prints "123". Skip
         * zero-length matches and keep looking, exactly as GNU does; a
         * pattern that can only ever match empty still reports a hit. */
        while (regexec(&preg, line + base, 1, &m, base ? REG_NOTBOL : 0) == 0) {
            size_t so = base + (size_t)m.rm_so;
            size_t ln = (size_t)(m.rm_eo - m.rm_so);
            if (ln > 0 || !oflag) { hit = 1; s = so; l = ln; break; }
            if (!line[so]) { hit = 1; s = so; l = 0; break; }
            base = so + 1;
        }
    } else {
        const char *p = fixed_find(line);
        if (p) { hit = 1; s = (size_t)(p - line); l = patlen; }
    }
    if (!hit) return 0;

    if (wflag) {
        /* A word match needs a non-word character (or an edge) on BOTH
         * sides; without this -w silently behaves like a plain match. */
        int before_ok = (s == 0) || !is_word_char((unsigned char)line[s - 1]);
        int after_ok  = !line[s + l] || line[s + l] == '\n' ||
                        !is_word_char((unsigned char)line[s + l]);
        if (!before_ok || !after_ok) return 0;
    }
    if (xflag) {
        size_t llen = strlen(line);
        while (llen && (line[llen-1] == '\n' || line[llen-1] == '\r')) llen--;
        if (!(s == 0 && l == llen)) return 0;
    }
    if (mstart) *mstart = s;
    if (mlen)   *mlen   = l;
    return 1;
}

static int grep_stream(FILE *f, const char *label, int show_label) {
    char  buf[4096];
    long  lineno = 0, matches = 0;
    int   any = 0;
    while (fgets(buf, sizeof(buf), f)) {
        lineno++;
        size_t ms = 0, ml = 0;
        int hit = line_matches(buf, &ms, &ml);
        if (vflag) hit = !hit;
        if (!hit) continue;
        matches++;
        any = 1;
        /* -q says nothing and stops at the first match: that is the whole
         * point of it, and reading the rest of a huge file would defeat it. */
        if (qflag) return 1;
        if (cflag || lflag || Lflag) continue;
        if (show_label) printf("%s:", label);
        if (nflag)      printf("%ld:", lineno);
        if (oflag && !vflag) {
            printf("%.*s\n", (int)ml, buf + ms);
        } else {
            fputs(buf, stdout);
            size_t L = strlen(buf);
            if (L == 0 || buf[L - 1] != '\n') putchar('\n');
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
    if (!d) { weprintf("%s:", path); return 0; }
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
    if (!f) { weprintf("%s:", path); return 0; }
    int any = grep_stream(f, path, show_label);
    fclose(f);
    return any;
}

int main(int argc, char *argv[]) {
    util_argv0_set(argv[0]);

    /* egrep and fgrep are this program under another name, exactly as they
     * are everywhere else. */
    const char *base = argv[0] ? argv[0] : "grep";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    if (!strcmp(base, "egrep")) Eflag = 1;
    if (!strcmp(base, "fgrep")) Fflag = 1;
    if (!strcmp(base, "rgrep")) { rflag = 1; }

    int opt;
    while ((opt = getopt(argc, argv, "cEFHhilLnoqrvwx")) != -1) {
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
        case 'v': vflag = 1; break;
        case 'w': wflag = 1; break;
        case 'x': xflag = 1; break;
        default:  usage();
        }
    }
    if (optind >= argc) usage();
    pattern = argv[optind++];
    patlen  = strlen(pattern);

    if (!Fflag) {
        int cf = REG_NOSUB;
        if (Eflag) cf |= REG_EXTENDED;
        if (iflag) cf |= REG_ICASE;
        /* -o and -w need the match OFFSETS, so REG_NOSUB has to come off
         * for them or rm_so/rm_eo come back meaningless. */
        if (oflag || wflag || xflag) cf &= ~REG_NOSUB;
        if (regcomp(&preg, pattern, cf) == 0) use_regex = 1;
        else {
            /* An invalid regex is an error in real grep, not a silent
             * downgrade to a literal search that would quietly match
             * something else. */
            enprintf(2, "invalid pattern: %s", pattern);
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
    if (use_regex) regfree(&preg);
    return any ? 0 : 1;
}
