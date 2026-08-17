/* tr -- translate, squeeze or delete characters.
 *
 *     tr [-cds] string1 [string2]
 *
 *   -c   complement string1
 *   -d   delete characters in string1
 *   -s   squeeze repeated output characters that are in the last string
 *
 * Ranges (a-z) and the common escapes (\n \t \\ \0nnn) are understood. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static int expand(const char *s, unsigned char *out, int cap) {
    int n = 0;
    for (const char *p = s; *p && n < cap; ) {
        unsigned char c;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'r': c = '\r'; p++; break;
            case '\\': c = '\\'; p++; break;
            default:
                if (*p >= '0' && *p <= '7') {
                    int v = 0, k = 0;
                    while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                    c = (unsigned char)v;
                } else c = (unsigned char)*p++;
            }
        } else {
            c = (unsigned char)*p++;
        }
        /* A range only counts when a real end character follows. */
        if (*p == '-' && p[1] && p[1] != '-') {
            unsigned char hi = (unsigned char)p[1];
            p += 2;
            for (unsigned int k = c; k <= hi && n < cap; k++)
                out[n++] = (unsigned char)k;
            continue;
        }
        out[n++] = c;
    }
    return n;
}

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "tr");
    int cflag = 0, dflag = 0, sflag = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'c' || *f == 'C') cflag = 1;
            else if (*f == 'd') dflag = 1;
            else if (*f == 's') sflag = 1;
            else eprintf("usage: %s [-cds] string1 [string2]", argv0);
        }
    }
    if (i >= argc) eprintf("usage: %s [-cds] string1 [string2]", argv0);

    unsigned char set1[512], set2[512];
    int n1 = expand(argv[i++], set1, sizeof(set1));
    int n2 = (i < argc) ? expand(argv[i++], set2, sizeof(set2)) : 0;
    if (i < argc) eprintf("too many arguments");
    if (!dflag && n2 == 0) eprintf("missing string2");

    int in1[256] = {0};
    for (int k = 0; k < n1; k++) in1[set1[k]] = 1;
    if (cflag) for (int k = 0; k < 256; k++) in1[k] = !in1[k];

    /* Translation target: string2, with its last character repeated to
     * cover a longer string1, which is what POSIX specifies. */
    unsigned char map[256];
    for (int k = 0; k < 256; k++) map[k] = (unsigned char)k;
    if (!dflag) {
        int j = 0;
        for (int k = 0; k < 256; k++) {
            if (!in1[k]) continue;
            map[k] = set2[j < n2 ? j : n2 - 1];
            if (j < n2 - 1) j++;
        }
    }

    int squeeze[256] = {0};
    if (sflag) {
        const unsigned char *ss = (dflag || n2 == 0) ? set1 : set2;
        int sn = (dflag || n2 == 0) ? n1 : n2;
        for (int k = 0; k < sn; k++) squeeze[ss[k]] = 1;
        if (cflag && (dflag || n2 == 0))
            for (int k = 0; k < 256; k++) squeeze[k] = !squeeze[k];
    }

    char inbuf[4096], outbuf[4096];
    int prev = -1;
    for (;;) {
        ssize_t n = read(0, inbuf, sizeof(inbuf));
        if (n == 0) break;
        if (n < 0) { weprintf("read:"); return 1; }
        size_t o = 0;
        for (ssize_t k = 0; k < n; k++) {
            unsigned char c = (unsigned char)inbuf[k];
            if (dflag && in1[c]) continue;
            unsigned char out = dflag ? c : map[c];
            if (sflag && squeeze[out] && prev == (int)out) continue;
            prev = out;
            outbuf[o++] = (char)out;
            if (o == sizeof(outbuf)) {
                if (write(1, outbuf, o) < 0) { weprintf("write:"); return 1; }
                o = 0;
            }
        }
        if (o && write(1, outbuf, o) < 0) { weprintf("write:"); return 1; }
    }
    return 0;
}
