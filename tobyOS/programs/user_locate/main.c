/* locate / updatedb -- find files by name from a prebuilt index.
 *
 *     updatedb [-U root] [-o dbfile] [--prunepaths 'a b c']
 *     locate   [-icrbe] [-l N] pattern ...
 *
 * The database is PLAIN TEXT, one absolute path per line, not GNU's
 * front-compressed mlocate format. Said plainly because it matters: the
 * file is bigger than mlocate's would be, and no mlocate tool will read
 * it. What it buys is that the index is inspectable with cat and grep,
 * which on a system with no locate at all is the better trade.
 *
 * updatedb prunes /proc, /sys and /dev by default -- walking /proc means
 * walking every live process, and the result is stale the moment it is
 * written.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <regex.h>

#define DEFAULT_DB "/var/lib/locate.db"

static const char *prog = "locate";

/* ---------------------------------------------------------- updatedb --- */

static const char *prune_default[] = {
    "/proc", "/sys", "/dev", "/tmp/.locate", NULL
};
static const char *prune_extra[32];
static int         n_prune_extra;

static int pruned(const char *path) {
    for (int i = 0; prune_default[i]; i++)
        if (!strcmp(path, prune_default[i])) return 1;
    for (int i = 0; i < n_prune_extra; i++)
        if (!strcmp(path, prune_extra[i])) return 1;
    return 0;
}

static long walk(const char *dir, FILE *out, int depth) {
    if (depth > 32) return 0;          /* a symlink loop would be endless */
    DIR *d = opendir(dir);
    if (!d) return 0;
    long n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[1024];
        if (!strcmp(dir, "/")) snprintf(path, sizeof path, "/%s", e->d_name);
        else                   snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (pruned(path)) continue;
        fprintf(out, "%s\n", path);
        n++;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            n += walk(path, out, depth + 1);
    }
    closedir(d);
    return n;
}

static int do_updatedb(int argc, char **argv) {
    const char *root = "/";
    const char *db   = DEFAULT_DB;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-U") && i + 1 < argc) { root = argv[++i]; continue; }
        if (!strcmp(a, "-o") && i + 1 < argc) { db = argv[++i]; continue; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; continue; }
        if (!strncmp(a, "--output=", 9)) { db = a + 9; continue; }
        if (!strncmp(a, "--database-root=", 16)) { root = a + 16; continue; }
        if (!strcmp(a, "--prunepaths") && i + 1 < argc) {
            /* A space-separated list, as updatedb.conf writes it. */
            static char buf[1024];
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            char *p = buf;
            while (*p && n_prune_extra < 32) {
                while (*p == ' ') p++;
                if (!*p) break;
                prune_extra[n_prune_extra++] = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = 0;
            }
            continue;
        }
        if (!strcmp(a, "--help")) {
            printf("usage: updatedb [-U root] [-o dbfile] "
                   "[--prunepaths 'a b c']\n");
            return 0;
        }
    }

    /* Write to a temp file and rename, so a locate running at the same
     * time never sees a half-written index. */
    char tmp[1088];
    snprintf(tmp, sizeof tmp, "%s.tmp", db);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "updatedb: cannot write %s\n", tmp);
        return 1;
    }
    long n = walk(root, f, 0);
    fclose(f);
    unlink(db);
    if (rename(tmp, db) != 0) {
        fprintf(stderr, "updatedb: cannot rename %s to %s\n", tmp, db);
        return 1;
    }
    if (verbose) printf("updatedb: indexed %ld paths from %s into %s\n",
                        n, root, db);
    return 0;
}

/* ------------------------------------------------------------ locate --- */

/* A pattern with no wildcard matches as a SUBSTRING, which is what makes
 * `locate passwd` useful; one with a wildcard is matched as a glob against
 * the whole path, as GNU does. */
static int glob_match(const char *pat, const char *s, int icase) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (const char *t = s; *t; t++)
                if (glob_match(pat, t, icase)) return 1;
            return glob_match(pat, s, icase);
        }
        if (!*s) return 0;
        if (*pat == '?') { pat++; s++; continue; }
        char a = *pat, b = *s;
        if (icase) { a = (char)tolower((unsigned char)a); b = (char)tolower((unsigned char)b); }
        if (a != b) return 0;
        pat++; s++;
    }
    return !*s;
}

static int substr(const char *hay, const char *needle, int icase) {
    if (!icase) return strstr(hay, needle) != NULL;
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int do_locate(int argc, char **argv) {
    const char *db = DEFAULT_DB;
    int icase = 0, count_only = 0, basename_only = 0, use_regex = 0;
    int check_exists = 0;
    long limit = 0;
    const char *pats[32];
    int npats = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-d") && i + 1 < argc) { db = argv[++i]; continue; }
        if (!strcmp(a, "-l") && i + 1 < argc) { limit = atol(argv[++i]); continue; }
        if (!strncmp(a, "--database=", 11)) { db = a + 11; continue; }
        if (!strcmp(a, "--help")) {
            printf("usage: locate [-icrbe] [-l N] [-d db] pattern ...\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'i': icase = 1; break;
                case 'c': count_only = 1; break;
                case 'b': basename_only = 1; break;
                case 'r': use_regex = 1; break;
                case 'e': check_exists = 1; break;
                case 'A': break;
                default: break;
                }
            }
            continue;
        }
        if (npats < 32) pats[npats++] = a;
    }
    if (npats == 0) {
        fprintf(stderr, "locate: no pattern given\n");
        return 1;
    }

    FILE *f = fopen(db, "r");
    if (!f) {
        fprintf(stderr, "locate: cannot open %s: run updatedb first\n", db);
        return 1;
    }

    regex_t re[32];
    int re_ok[32];
    for (int k = 0; k < npats; k++) {
        re_ok[k] = 0;
        if (use_regex)
            re_ok[k] = (regcomp(&re[k], pats[k], icase ? REG_ICASE : 0) == 0);
    }

    long hits = 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L) continue;

        const char *subject = line;
        if (basename_only) {
            const char *b = line;
            for (const char *q = line; *q; q++) if (*q == '/') b = q + 1;
            subject = b;
        }

        int matched = 0;
        for (int k = 0; k < npats && !matched; k++) {
            if (use_regex) {
                if (re_ok[k] && regexec(&re[k], subject, 0, NULL, 0) == 0) matched = 1;
            } else if (strchr(pats[k], '*') || strchr(pats[k], '?')) {
                matched = glob_match(pats[k], subject, icase);
            } else {
                matched = substr(subject, pats[k], icase);
            }
        }
        if (!matched) continue;
        if (check_exists) {
            struct stat st;
            if (stat(line, &st) != 0) continue;
        }
        hits++;
        if (!count_only) printf("%s\n", line);
        if (limit > 0 && hits >= limit) break;
    }
    fclose(f);
    for (int k = 0; k < npats; k++) if (re_ok[k]) regfree(&re[k]);

    if (count_only) printf("%ld\n", hits);
    return hits ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "locate";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    if (!strcmp(base, "updatedb")) return do_updatedb(argc, argv);
    return do_locate(argc, argv);
}
