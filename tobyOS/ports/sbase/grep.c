/* grep -- print lines matching a pattern.
 *
 * Modeled on sbase's grep.c (ISC license) but reduced to fixed-string
 * matching only:
 *
 *     grep [-cinv] pattern [file ...]
 *
 *   -c   print a count of matching lines per file instead of lines
 *   -i   case-insensitive match
 *   -n   prefix each match with its line number
 *   -v   invert: print non-matching lines
 *
 * No regex (sbase uses POSIX <regex.h>; we don't have it). With no
 * file arguments, reads from stdin. Exit 0 if any match, 1 if none,
 * 2 on error -- POSIX semantics. */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static int cflag, iflag, nflag, vflag;
static const char *pattern;
static size_t      patlen;

static int line_matches(const char *line) {
    if (patlen == 0) return 1;
    if (!iflag) return strstr(line, pattern) != 0;

    size_t llen = strlen(line);
    if (llen < patlen) return 0;
    for (size_t i = 0; i + patlen <= llen; i++) {
        size_t k;
        for (k = 0; k < patlen; k++) {
            if (tolower((unsigned char)line[i + k]) !=
                tolower((unsigned char)pattern[k])) break;
        }
        if (k == patlen) return 1;
    }
    return 0;
}

static int grep_stream(FILE *f, const char *label, int show_label) {
    char  buf[4096];
    long  lineno = 0;
    long  matches = 0;
    int   any = 0;
    while (fgets(buf, sizeof(buf), f)) {
        lineno++;
        int hit = line_matches(buf);
        if (vflag) hit = !hit;
        if (!hit) continue;
        matches++;
        any = 1;
        if (cflag) continue;
        if (show_label) printf("%s:", label);
        if (nflag)      printf("%ld:", lineno);
        fputs(buf, stdout);
        size_t L = strlen(buf);
        if (L == 0 || buf[L - 1] != '\n') putchar('\n');
    }
    if (cflag) {
        if (show_label) printf("%s:", label);
        printf("%ld\n", matches);
    }
    return any;
}

static void usage(void) { eprintf("usage: %s [-cinv] pattern [file ...]", argv0); }

int main(int argc, char *argv[]) {
    util_argv0_set(argv[0]);

    int opt;
    while ((opt = getopt(argc, argv, "cinv")) != -1) {
        switch (opt) {
        case 'c': cflag = 1; break;
        case 'i': iflag = 1; break;
        case 'n': nflag = 1; break;
        case 'v': vflag = 1; break;
        default:  usage();
        }
    }
    if (optind >= argc) usage();

    pattern = argv[optind++];
    patlen  = strlen(pattern);

    int  any   = 0;
    int  rc    = 0;
    int  files = argc - optind;

    if (files <= 0) {
        any = grep_stream(stdin, "<stdin>", 0);
    } else {
        int show_label = files > 1;
        for (int i = optind; i < argc; i++) {
            FILE *f = fopen(argv[i], "r");
            if (!f) { weprintf("open %s:", argv[i]); rc = 2; continue; }
            if (grep_stream(f, argv[i], show_label)) any = 1;
            fclose(f);
        }
    }
    if (rc) return rc;
    return any ? 0 : 1;
}
