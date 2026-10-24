/* su_attr.c -- getfattr and setfattr, as attr 2.5.2's (logs/utilgate/
 * getfattr.sh, setfattr.sh; finding 225). There were none: nothing native
 * could read or write an extended attribute.
 *
 * Measured on attr's (WSL, tmpfs):
 *   getfattr [-hRLP] [-n NAME | -d] [-e EN] [-m PATTERN] PATH...
 *   - -n NAME prints that attribute; -d every one whose name matches
 *     PATTERN (an extended regular expression, "^user\\." when none is
 *     given; "-" or "" matches all); neither prints the matching names
 *     alone. -n wins over -d. -d's names are sorted.
 *   - A file's block is "# file: PATH", a line per attribute, a blank
 *     line; a file with nothing to show prints nothing. A leading '/' is
 *     taken off PATH (once said on stderr) unless --absolute-names. In
 *     PATH a backslash, a newline and a carriage return are octal escapes.
 *   - Values: "text" in double quotes, a backslash, a quote, a newline,
 *     a carriage return and a NUL as escapes (\\, \", \012, \015, \000),
 *     one trailing NUL left off, every other byte as it is; "hex" 0x and
 *     two lowercase digits a byte; "base64" 0s and the standard alphabet.
 *     With no -e a value is text when at most one byte in eight (a
 *     trailing NUL not counted) is not printable ASCII, else base64.
 *     --only-values writes the bare bytes and nothing else.
 *   - -h reads a link's own attributes; otherwise links are followed.
 *     -R walks directories, each before what is in it, in readdir order;
 *     a link to a directory is walked into with -L, and when it is a PATH
 *     operand also by default (not with -P). --one-file-system stays on
 *     the operand's filesystem.
 *   - Errors: an attribute's "PATH: NAME: ERR" (ENODATA is "No such
 *     attribute"), a listing's "getfattr: PATH: ERR", status 1; a bad
 *     PATTERN "getfattr: invalid regular expression "P"", status 1; usage
 *     (no PATH, a bad -e, a bad option) the synopsis and its Try line,
 *     status 2.
 *   setfattr {-n NAME [-v VALUE] | -x NAME} [-h] FILE...
 *   setfattr [-hP] --restore=FILE
 *   - VALUE: 0x hex pairs, 0s base64, else text: a pair of enclosing
 *     quotes dropped, \ and one to three octal digits that byte, \\ and \"
 *     the character, any other backslash kept. A bad encoding is "bad
 *     input encoding", status 1; an empty 0s sets nothing. --raw takes
 *     VALUE as it is. No -v is an empty value.
 *   - A failure is "setfattr: FILE: ERR", status 1, and the next FILE is
 *     tried. -h acts on a link itself.
 *   - --restore reads getfattr -d's output ("-" stdin): a block is a
 *     "# file: NAME" line (octal escapes undone), then NAME=VALUE lines
 *     (a NAME alone sets an empty value) up to a blank line; a block
 *     without its "# file: " line is "setfattr: FILE: No filename found in
 *     line N, aborting", status 1, N the lines read before the blank lines
 *     ahead of it. A FILE that cannot be opened is said, status 0.
 *   - Usage (no -n nor -x, both, -v without -n, no FILE) is three
 *     synopsis lines and the Try line, status 2. */

#include <sys/xattr.h>
#include <regex.h>

enum { AT_AUTO, AT_TEXT, AT_HEX, AT_B64 };

static const char at_b64set[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char *at_strerror(int e) {
    return e == ENODATA ? "No such attribute" : strerror(e);
}

static void at_put_text(const unsigned char *v, size_t n) {
    if (n && v[n - 1] == 0) n--;
    putchar('"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = v[i];
        if (c == '\\') fputs("\\\\", stdout);
        else if (c == '"') fputs("\\\"", stdout);
        else if (c == '\n' || c == '\r' || c == 0) printf("\\%03o", c);
        else putchar(c);
    }
    putchar('"');
}

static void at_put_value(const unsigned char *v, size_t n, int enc) {
    if (enc == AT_AUTO) {
        size_t m = (n && v[n - 1] == 0) ? n - 1 : n, bad = 0;
        for (size_t i = 0; i < m; i++)
            if (v[i] < 0x20 || v[i] >= 0x7f) bad++;
        enc = m >= bad * 8 ? AT_TEXT : AT_B64;
    }
    if (enc == AT_TEXT) { at_put_text(v, n); return; }
    if (enc == AT_HEX) {
        fputs("0x", stdout);
        for (size_t i = 0; i < n; i++) printf("%02x", v[i]);
        return;
    }
    fputs("0s", stdout);
    for (size_t i = 0; i < n; i += 3) {
        unsigned w = (unsigned)v[i] << 16 | (i + 1 < n ? (unsigned)v[i + 1] << 8 : 0) |
                     (i + 2 < n ? v[i + 2] : 0);
        putchar(at_b64set[w >> 18 & 63]);
        putchar(at_b64set[w >> 12 & 63]);
        putchar(i + 1 < n ? at_b64set[w >> 6 & 63] : '=');
        putchar(i + 2 < n ? at_b64set[w & 63] : '=');
    }
}

/* a PATH or NAME as getfattr shows one: backslash, newline, CR octal */
static void at_put_name(FILE *o, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || c == '\n' || c == '\r') fprintf(o, "\\%03o", c);
        else fputc(c, o);
    }
}

/* ---- getfattr ------------------------------------------------------------ */

static struct {
    const char *name;           /* -n */
    int dump, enc, nodef, recursive, absolute, only, onefs;
    char walk;                  /* 'L' 'P', or 0: the default */
    regex_t re;
    int warned;
    dev_t dev;
    int status;
} at_g;

static const char *at_shown(const char *path) {
    if (at_g.absolute || path[0] != '/') return path;
    if (!at_g.warned) {
        fprintf(stderr, "getfattr: Removing leading '/' from absolute path names\n");
        at_g.warned = 1;
    }
    while (*path == '/') path++;
    return *path ? path : ".";
}

/* an attribute's value into a malloc'd buffer: its length, or -1 */
static long at_get(const char *path, const char *name, unsigned char **out) {
    for (;;) {
        ssize_t n = at_g.nodef ? lgetxattr(path, name, 0, 0) : getxattr(path, name, 0, 0);
        if (n < 0) return -1;
        unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
        if (!b) su_error(1, ENOMEM, "memory exhausted");
        ssize_t m = at_g.nodef ? lgetxattr(path, name, b, (size_t)n) : getxattr(path, name, b, (size_t)n);
        if (m < 0) {
            int e = errno;
            free(b);
            if (e == ERANGE) continue;          /* it grew */
            errno = e;
            return -1;
        }
        *out = b;
        return (long)m;
    }
}

static int at_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void at_header(const char *path, int *said) {
    if (*said || at_g.only) return;
    fputs("# file: ", stdout);
    at_put_name(stdout, at_shown(path));
    putchar('\n');
    *said = 1;
}

static void at_show(const char *path) {
    int said = 0;
    if (at_g.name) {
        unsigned char *v;
        long n = at_get(path, at_g.name, &v);
        if (n < 0) {
            int e = errno;
            fflush(stdout);
            at_put_name(stderr, path);
            fputs(": ", stderr);
            at_put_name(stderr, at_g.name);
            fprintf(stderr, ": %s\n", at_strerror(e));
            at_g.status = 1;
            return;
        }
        if (at_g.only) fwrite(v, 1, (size_t)n, stdout);
        else {
            at_header(path, &said);
            at_put_name(stdout, at_g.name);
            putchar('=');
            at_put_value(v, (size_t)n, at_g.enc);
            fputs("\n\n", stdout);
        }
        free(v);
        return;
    }
    char *list = 0;
    ssize_t ln;
    for (;;) {
        ln = at_g.nodef ? llistxattr(path, 0, 0) : listxattr(path, 0, 0);
        if (ln < 0) break;
        free(list);
        list = (char *)malloc((size_t)ln + 1);
        if (!list) su_error(1, ENOMEM, "memory exhausted");
        ssize_t m = at_g.nodef ? llistxattr(path, list, (size_t)ln) : listxattr(path, list, (size_t)ln);
        if (m >= 0) { ln = m; break; }
        if (errno != ERANGE) { ln = -1; break; }
    }
    if (ln < 0) {
        int e = errno;
        free(list);
        fflush(stdout);
        fputs("getfattr: ", stderr);
        at_put_name(stderr, path);
        fprintf(stderr, ": %s\n", strerror(e));
        at_g.status = 1;
        return;
    }
    char **names = 0;
    size_t nn = 0;
    for (ssize_t o = 0; o < ln; o += (ssize_t)strlen(list + o) + 1) {
        if (regexec(&at_g.re, list + o, 0, 0, 0) != 0) continue;
        names = (char **)realloc(names, (nn + 1) * sizeof *names);
        if (!names) su_error(1, ENOMEM, "memory exhausted");
        names[nn++] = list + o;
    }
    qsort(names, nn, sizeof *names, at_cmp);
    for (size_t i = 0; i < nn; i++) {
        if (!at_g.dump) {
            at_header(path, &said);
            if (!at_g.only) { at_put_name(stdout, names[i]); putchar('\n'); }
            continue;
        }
        unsigned char *v;
        long n = at_get(path, names[i], &v);
        if (n < 0) {
            int e = errno;
            fflush(stdout);
            at_put_name(stderr, path);
            fputs(": ", stderr);
            at_put_name(stderr, names[i]);
            fprintf(stderr, ": %s\n", at_strerror(e));
            at_g.status = 1;
            continue;
        }
        if (at_g.only) fwrite(v, 1, (size_t)n, stdout);
        else {
            at_header(path, &said);
            at_put_name(stdout, names[i]);
            putchar('=');
            at_put_value(v, (size_t)n, at_g.enc);
            putchar('\n');
        }
        free(v);
    }
    if (said) putchar('\n');
    free(names);
    free(list);
}

/* the directories on the walk's way down, so a looping link stops it */
struct at_seen { dev_t dev; ino_t ino; const struct at_seen *up; };

static void at_walk(const char *path, int top, const struct at_seen *up) {
    at_show(path);
    if (!at_g.recursive) return;
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISLNK(st.st_mode)) {
        int follow = at_g.walk == 'L' || (top && at_g.walk != 'P');
        if (!follow || stat(path, &st) != 0) return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    if (top) at_g.dev = st.st_dev;
    else if (at_g.onefs && st.st_dev != at_g.dev) return;
    for (const struct at_seen *s = up; s; s = s->up)
        if (s->dev == st.st_dev && s->ino == st.st_ino) return;
    struct at_seen me = { st.st_dev, st.st_ino, up };
    DIR *d = opendir(path);
    if (!d) {
        fflush(stdout);
        fputs("getfattr: ", stderr);
        at_put_name(stderr, path);
        fprintf(stderr, ": %s\n", strerror(errno));
        at_g.status = 1;
        return;
    }
    char **names = 0;
    size_t nn = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        names = (char **)realloc(names, (nn + 1) * sizeof *names);
        if (!names) su_error(1, ENOMEM, "memory exhausted");
        names[nn++] = strdup(de->d_name);
    }
    closedir(d);
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') pl--;
    for (size_t i = 0; i < nn; i++) {
        size_t l = pl + strlen(names[i]) + 2;
        char *c = (char *)malloc(l);
        if (!c) su_error(1, ENOMEM, "memory exhausted");
        snprintf(c, l, "%.*s%s%s", (int)pl, path, (pl == 1 && path[0] == '/') ? "" : "/", names[i]);
        at_walk(c, 0, &me);
        free(c);
        free(names[i]);
    }
    free(names);
}

static void at_gusage(void) {
    fputs("Usage: getfattr [-hRLP] [-n name|-d] [-e en] [-m pattern] path...\n"
          "Try `getfattr --help' for more information.\n", stderr);
    exit(2);
}

static int do_getfattr(int argc, char **argv) {
    enum { ONLY_OPT = 0x100, ABS_OPT, ONEFS_OPT, HELP_OPT, VER_OPT };
    static const struct su_option longs[] = {
        { "name", SU_REQ_ARG, 0, 'n' }, { "dump", SU_NO_ARG, 0, 'd' },
        { "encoding", SU_REQ_ARG, 0, 'e' }, { "match", SU_REQ_ARG, 0, 'm' },
        { "only-values", SU_NO_ARG, 0, ONLY_OPT },
        { "no-dereference", SU_NO_ARG, 0, 'h' },
        { "one-file-system", SU_NO_ARG, 0, ONEFS_OPT },
        { "absolute-names", SU_NO_ARG, 0, ABS_OPT },
        { "recursive", SU_NO_ARG, 0, 'R' }, { "logical", SU_NO_ARG, 0, 'L' },
        { "physical", SU_NO_ARG, 0, 'P' },
        { "version", SU_NO_ARG, 0, VER_OPT }, { "help", SU_NO_ARG, 0, HELP_OPT },
        { 0, 0, 0, 0 }
    };
    memset(&at_g, 0, sizeof at_g);
    at_g.enc = AT_AUTO;
    const char *pattern = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "n:de:m:hRLP", longs, 0)) != -1) {
        switch (c) {
        case 'n': at_g.name = su_optarg; break;
        case 'd': at_g.dump = 1; break;
        case 'e':
            if (!strcmp(su_optarg, "text")) at_g.enc = AT_TEXT;
            else if (!strcmp(su_optarg, "hex")) at_g.enc = AT_HEX;
            else if (!strcmp(su_optarg, "base64")) at_g.enc = AT_B64;
            else at_gusage();
            break;
        case 'm': pattern = su_optarg; break;
        case 'h': at_g.nodef = 1; break;
        case 'R': at_g.recursive = 1; break;
        case 'L': case 'P': at_g.walk = (char)c; break;
        case ONLY_OPT: at_g.only = 1; break;
        case ABS_OPT: at_g.absolute = 1; break;
        case ONEFS_OPT: at_g.onefs = 1; break;
        case VER_OPT: puts("getfattr (tobyOS) 1.0"); return 0;
        case HELP_OPT:
            fputs("getfattr -- print the extended attributes of files\n"
                  "Usage: getfattr [-hRLP] [-n name|-d] [-e en] [-m pattern] path...\n"
                  "  -n, --name=NAME         the value of attribute NAME\n"
                  "  -d, --dump              every attribute whose name matches -m's pattern\n"
                  "  -e, --encoding=EN       values as text, hex or base64\n"
                  "  -m, --match=PATTERN     names matching this extended regular expression\n"
                  "                          (^user\\. if none; - or empty for every name)\n"
                  "      --only-values       the bare values, nothing around them\n"
                  "  -h, --no-dereference    a symbolic link's own attributes\n"
                  "      --one-file-system   do not walk onto another filesystem\n"
                  "      --absolute-names    keep a path's leading '/'\n"
                  "  -R, --recursive         walk into directories\n"
                  "  -L, --logical           walk through every link to a directory\n"
                  "  -P, --physical          walk through none, a PATH's own included\n"
                  "      --version           say the version and stop\n"
                  "      --help              say this and stop\n", stdout);
            return 0;
        default: at_gusage();
        }
    }
    if (su_optind >= argc) at_gusage();
    if (!pattern) pattern = "^user\\.";
    if (!strcmp(pattern, "-")) pattern = "";
    if (regcomp(&at_g.re, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
        fprintf(stderr, "getfattr: invalid regular expression \"%s\"\n", pattern);
        return 1;
    }
    for (int i = su_optind; i < argc; i++) at_walk(argv[i], 1, 0);
    fflush(stdout);
    return at_g.status;
}

/* ---- setfattr ------------------------------------------------------------ */

static int at_b64val(int c) {
    const char *p = c ? strchr(at_b64set, c) : 0;
    return p ? (int)(p - at_b64set) : -1;
}

/* VALUE as setfattr reads it, into *OUT (malloc'd) and *LEN: 1 done, 0 a
 * bad encoding, -1 an empty 0s (nothing to set) */
static int at_decode(const char *s, unsigned char **out, size_t *len) {
    size_t n = strlen(s);
    unsigned char *b = (unsigned char *)malloc(n + 1);
    if (!b) su_error(1, ENOMEM, "memory exhausted");
    size_t o = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        const char *h = s + 2;
        if (strlen(h) % 2) { free(b); return 0; }
        for (; *h; h += 2) {
            int hi = isxdigit((unsigned char)h[0]) ? (isdigit((unsigned char)h[0]) ? h[0] - '0' : (tolower((unsigned char)h[0]) - 'a' + 10)) : -1;
            int lo = isxdigit((unsigned char)h[1]) ? (isdigit((unsigned char)h[1]) ? h[1] - '0' : (tolower((unsigned char)h[1]) - 'a' + 10)) : -1;
            if (hi < 0 || lo < 0) { free(b); return 0; }
            b[o++] = (unsigned char)(hi << 4 | lo);
        }
    } else if (s[0] == '0' && (s[1] == 's' || s[1] == 'S')) {
        const char *q = s + 2;
        size_t ql = strlen(q);
        if (ql == 0) { free(b); return -1; }
        if (ql % 4) { free(b); return 0; }
        for (size_t i = 0; i < ql; i += 4) {
            int v[4], pad = 0;
            for (int k = 0; k < 4; k++) {
                if (q[i + k] == '=' && i + 4 == ql && k >= 2) { v[k] = 0; pad++; continue; }
                if (pad) { free(b); return 0; }
                v[k] = at_b64val(q[i + k]);
                if (v[k] < 0) { free(b); return 0; }
            }
            unsigned w = (unsigned)v[0] << 18 | (unsigned)v[1] << 12 | (unsigned)v[2] << 6 | (unsigned)v[3];
            b[o++] = (unsigned char)(w >> 16);
            if (pad < 2) b[o++] = (unsigned char)(w >> 8);
            if (pad < 1) b[o++] = (unsigned char)w;
        }
    } else {
        const char *p = s, *end = s + n;
        if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { p++; end--; }
        while (p < end) {
            if (*p == '\\' && p + 1 < end && p[1] >= '0' && p[1] <= '7') {
                int v = 0, k = 0;
                for (p++; k < 3 && p < end && *p >= '0' && *p <= '7'; k++, p++) v = v * 8 + (*p - '0');
                b[o++] = (unsigned char)v;
            } else if (*p == '\\' && p + 1 < end && (p[1] == '\\' || p[1] == '"')) {
                b[o++] = (unsigned char)p[1];
                p += 2;
            } else {
                b[o++] = (unsigned char)*p++;
            }
        }
    }
    *out = b;
    *len = o;
    return 1;
}

/* a name from a restore file: getfattr's octal escapes undone */
static char *at_unescape(const char *s) {
    char *b = (char *)malloc(strlen(s) + 1);
    if (!b) su_error(1, ENOMEM, "memory exhausted");
    size_t o = 0;
    while (*s) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7') {
            int v = 0, k = 0;
            for (s++; k < 3 && *s >= '0' && *s <= '7'; k++, s++) v = v * 8 + (*s - '0');
            b[o++] = (char)v;
        } else b[o++] = *s++;
    }
    b[o] = 0;
    return b;
}

static int at_nodef_s;

static int at_set_one(const char *file, const char *name, const unsigned char *v, size_t n) {
    int r = at_nodef_s ? lsetxattr(file, name, v, n, 0) : setxattr(file, name, v, n, 0);
    if (r == 0) return 0;
    int e = errno;
    fflush(stdout);
    fputs("setfattr: ", stderr);
    at_put_name(stderr, file);
    fprintf(stderr, ": %s\n", at_strerror(e));
    return 1;
}

static void at_susage(void) {
    fputs("Usage: setfattr {-n name} [-v value] [-h] file...\n"
          "       setfattr {-x name} [-h] file...\n"
          "       setfattr [-hP] --restore=file\n"
          "Try `setfattr --help' for more information.\n", stderr);
    exit(2);
}

/* one line of a restore file, its newline gone: 0 at the end */
static char *at_line(FILE *f) {
    size_t cap = 128, n = 0;
    char *b = (char *)malloc(cap);
    if (!b) su_error(1, ENOMEM, "memory exhausted");
    int c;
    while ((c = getc(f)) != EOF && c != '\n') {
        if (n + 1 >= cap) {
            cap *= 2;
            b = (char *)realloc(b, cap);
            if (!b) su_error(1, ENOMEM, "memory exhausted");
        }
        b[n++] = (char)c;
    }
    if (c == EOF && n == 0) { free(b); return 0; }
    b[n] = 0;
    return b;
}

static int at_restore(const char *rfile) {
    FILE *f = strcmp(rfile, "-") ? fopen(rfile, "r") : stdin;
    if (!f) {
        fputs("setfattr: ", stderr);
        at_put_name(stderr, rfile);
        fprintf(stderr, ": %s\n", strerror(errno));
        return 0;                               /* attr's status here */
    }
    int status = 0;
    long lineno = 0;
    for (;;) {
        long before = lineno;
        char *l;
        while ((l = at_line(f)) && !*l) { free(l); lineno++; }
        if (!l) break;
        lineno++;
        if (strncmp(l, "# file: ", 8) != 0) {
            fputs("setfattr: ", stderr);
            at_put_name(stderr, rfile);
            fprintf(stderr, ": No filename found in line %ld, aborting\n", before);
            free(l);
            if (f != stdin) fclose(f);
            return 1;
        }
        char *file = at_unescape(l + 8);
        free(l);
        struct stat st;
        int there = (at_nodef_s ? lstat(file, &st) : stat(file, &st)) == 0;
        if (!there) {
            fputs("setfattr: ", stderr);
            at_put_name(stderr, file);
            fprintf(stderr, ": %s\n", strerror(errno));
            status = 1;
        }
        while ((l = at_line(f))) {
            lineno++;
            if (!*l) { free(l); break; }
            if (there) {
                char *eq = strchr(l, '=');
                if (eq) *eq = 0;
                char *name = at_unescape(l);
                unsigned char *v = (unsigned char *)"";
                size_t n = 0;
                int dr = eq ? at_decode(eq + 1, &v, &n) : 1;
                if (dr == 0) { fputs("bad input encoding\n", stderr); status = 1; }
                else if (dr > 0 && at_set_one(file, name, v, n)) status = 1;
                if (eq && dr > 0) free(v);
                free(name);
            }
            free(l);
        }
        free(file);
    }
    if (f != stdin) fclose(f);
    return status;
}

static int do_setfattr(int argc, char **argv) {
    enum { RESTORE_OPT = 0x100, RAW_OPT, HELP_OPT, VER_OPT };
    static const struct su_option longs[] = {
        { "name", SU_REQ_ARG, 0, 'n' }, { "remove", SU_REQ_ARG, 0, 'x' },
        { "value", SU_REQ_ARG, 0, 'v' }, { "no-dereference", SU_NO_ARG, 0, 'h' },
        { "physical", SU_NO_ARG, 0, 'P' }, { "restore", SU_REQ_ARG, 0, RESTORE_OPT },
        { "raw", SU_NO_ARG, 0, RAW_OPT },
        { "version", SU_NO_ARG, 0, VER_OPT }, { "help", SU_NO_ARG, 0, HELP_OPT },
        { 0, 0, 0, 0 }
    };
    const char *name = 0, *rm = 0, *value = 0, *restore = 0;
    int raw = 0, c;
    at_nodef_s = 0;
    while ((c = su_getopt_long(argc, argv, "n:x:v:hP", longs, 0)) != -1) {
        switch (c) {
        case 'n': name = su_optarg; break;
        case 'x': rm = su_optarg; break;
        case 'v': value = su_optarg; break;
        case 'h': case 'P': at_nodef_s = 1; break;
        case RESTORE_OPT: restore = su_optarg; break;
        case RAW_OPT: raw = 1; break;
        case VER_OPT: puts("setfattr (tobyOS) 1.0"); return 0;
        case HELP_OPT:
            fputs("setfattr -- set or remove an extended attribute of files\n"
                  "Usage: setfattr {-n name} [-v value] [-h] file...\n"
                  "       setfattr {-x name} [-h] file...\n"
                  "       setfattr [-hP] --restore=file\n"
                  "  -n, --name=NAME         give attribute NAME the value -v names\n"
                  "  -x, --remove=NAME       take attribute NAME away\n"
                  "  -v, --value=VALUE       the value: 0x hex, 0s base64, else text\n"
                  "  -h, --no-dereference    a symbolic link's own attributes\n"
                  "  -P, --physical          the same, for a restore\n"
                  "      --restore=FILE      set what getfattr -d wrote into FILE\n"
                  "      --raw               take VALUE as it is, no encoding\n"
                  "      --version           say the version and stop\n"
                  "      --help              say this and stop\n", stdout);
            return 0;
        default: at_susage();
        }
    }
    if (restore) return at_restore(restore);
    if ((!name && !rm) || (name && rm) || (value && !name) || su_optind >= argc) at_susage();
    unsigned char *v = (unsigned char *)"";
    size_t n = 0;
    int own = 0;
    if (name && value) {
        if (raw) { v = (unsigned char *)value; n = strlen(value); }
        else {
            int dr = at_decode(value, &v, &n);
            if (dr == 0) { fputs("bad input encoding\n", stderr); return 1; }
            if (dr < 0) return 0;              /* an empty 0s: nothing */
            own = 1;
        }
    }
    int status = 0;
    for (int i = su_optind; i < argc; i++) {
        if (name) { if (at_set_one(argv[i], name, v, n)) status = 1; continue; }
        int r = at_nodef_s ? lremovexattr(argv[i], rm) : removexattr(argv[i], rm);
        if (r != 0) {
            int e = errno;
            fputs("setfattr: ", stderr);
            at_put_name(stderr, argv[i]);
            fprintf(stderr, ": %s\n", at_strerror(e));
            status = 1;
        }
    }
    if (own) free(v);
    return status;
}
