/* find -- walk directory trees and evaluate an expression on each file.
 *
 * THE GNU findutils SUBSET SCRIPTS USE (finding 73: this knew -name and
 * nothing else, printed no starting point, and treated every other word as
 * an error). The grammar is find's: starting points, then an expression of
 * tests and actions joined by `!`/-not, -a/-and (also implied by
 * juxtaposition), -o/-or, `,` and parentheses; with no action in it,
 * -print is implied for whatever it is true of.
 *
 *   options   -H -L -P (before the paths); -maxdepth N -mindepth N -depth
 *             -d -xdev -mount -follow (anywhere in the expression)
 *   tests     -name -iname -path -ipath -wholename -iwholename -type -xtype
 *             -size -empty -newer -mtime -mmin -atime -amin -ctime -cmin
 *             -perm -user -group -uid -gid -nouser -nogroup -links -inum
 *             -readable -writable -executable -true -false -samefile
 *   actions   -print -print0 -printf -fprint? (no) -ls -exec ... ; -exec
 *             ... {} + -ok? (no) -delete -prune -quit
 *
 * Diagnostics are GNU find's words, on stderr, prefixed "find: "; a file
 * that cannot be reached makes the exit status 1 and the walk goes on. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* A diagnostic follows whatever was printed before it, as GNU find's
 * error() flushes stdout first: `find . nosuch` lists . and then says
 * nosuch is missing. */
#define FERR(...) do { fflush(stdout); fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- the expression tree ------------------------------------------- */

enum node_kind {
    N_AND, N_OR, N_NOT, N_COMMA,
    N_TRUE, N_FALSE,
    N_NAME, N_PATH, N_TYPE, N_XTYPE, N_SIZE, N_EMPTY, N_NEWER, N_TIME,
    N_PERM, N_UID, N_GID, N_NOUSER, N_NOGROUP, N_LINKS, N_INUM,
    N_ACCESS, N_SAMEFILE,
    N_PRINT, N_PRINT0, N_PRINTF, N_LS, N_EXEC, N_EXECPLUS, N_DELETE,
    N_PRUNE, N_QUIT
};

struct node {
    enum node_kind kind;
    struct node *l, *r;          /* operators */
    const char *s;               /* pattern, type letters, printf format */
    bool icase;
    int cmp;                     /* -1 less, 0 exactly, +1 more */
    long long n;                 /* a number; for N_TIME the field */
    long long unit;              /* N_SIZE: bytes per unit; N_TIME: secs */
    int which;                   /* N_TIME: 'm' 'a' 'c' */
    unsigned mode, mode_how;     /* N_PERM: how = 0 exact, '-' all, '/' any */
    dev_t dev; ino_t ino;        /* N_SAMEFILE */
    char **argv; int argc;       /* N_EXEC*: the command, {} still in it */
    char **batch; int nbatch, capbatch; size_t batch_bytes;  /* N_EXECPLUS */
    int access_mode;             /* N_ACCESS */
};

static int g_status;             /* the exit status: 1 once anything failed */
static int g_follow;             /* 0 -P, 1 -H, 2 -L */
static int g_maxdepth = -1, g_mindepth = 0;
static bool g_depth_first, g_xdev, g_quit;
static time_t g_now;
static struct node *g_expr;
static struct node **g_plus;     /* every -exec ... + node, for the end */
static int g_nplus;

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { FERR("find: memory exhausted\n"); exit(1); }
    return p;
}

static struct node *mk(enum node_kind k) {
    struct node *n = (struct node *)xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = k;
    return n;
}

/* ---- shell patterns (fnmatch without FNM_PATHNAME; `*` matches `/`) -- */

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* A bracket expression at P (just past `[`) against C; sets *end past `]`.
 * -1 when it is not a valid bracket expression (then `[` is literal). */
static int bracket(const char *p, int c, bool icase, const char **end) {
    bool neg = false, hit = false;
    if (*p == '!' || *p == '^') { neg = true; p++; }
    const char *start = p;
    while (*p && (*p != ']' || p == start)) {
        int lo = (unsigned char)*p;
        if (*p == '\\' && p[1]) lo = (unsigned char)*++p;
        if (*p == '[' && p[1] == ':') {          /* a class: [:alpha:] ... */
            const char *q = strstr(p + 2, ":]");
            if (q) {
                size_t len = (size_t)(q - (p + 2));
                const char *cl = p + 2;
                int ok = 0;
                #define CL(name, test) \
                    if (len == sizeof(name) - 1 && !strncmp(cl, name, len)) ok = (test);
                CL("alpha", (c|32) >= 'a' && (c|32) <= 'z')
                CL("digit", c >= '0' && c <= '9')
                CL("alnum", ((c|32) >= 'a' && (c|32) <= 'z') || (c >= '0' && c <= '9'))
                CL("upper", c >= 'A' && c <= 'Z')
                CL("lower", c >= 'a' && c <= 'z')
                CL("space", c == ' ' || (c >= '\t' && c <= '\r'))
                CL("blank", c == ' ' || c == '\t')
                CL("punct", c > 32 && c < 127 && !(((c|32) >= 'a' && (c|32) <= 'z') || (c >= '0' && c <= '9')))
                CL("xdigit", (c >= '0' && c <= '9') || ((c|32) >= 'a' && (c|32) <= 'f'))
                CL("print", c >= 32 && c < 127)
                CL("graph", c > 32 && c < 127)
                CL("cntrl", c < 32 || c == 127)
                #undef CL
                if (ok) hit = true;
                p = q + 2;
                continue;
            }
        }
        int hi = lo;
        if (p[1] == '-' && p[2] && p[2] != ']') {
            hi = (unsigned char)p[2];
            if (p[2] == '\\' && p[3]) { hi = (unsigned char)p[3]; p++; }
            p += 2;
        }
        if (icase) {
            if ((lc(c) >= lc(lo) && lc(c) <= lc(hi)) ||
                (c >= lo && c <= hi)) hit = true;
        } else if (c >= lo && c <= hi) hit = true;
        p++;
    }
    if (*p != ']') return -1;
    *end = p + 1;
    return hit != neg;
}

static bool pmatch(const char *p, const char *s, bool icase) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return true;
            for (; *s; s++) if (pmatch(p, s, icase)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p == '?') { p++; s++; continue; }
        if (*p == '[') {
            const char *end;
            int b = bracket(p + 1, (unsigned char)*s, icase, &end);
            if (b >= 0) {
                if (!b) return false;
                p = end; s++;
                continue;
            }
        }
        if (*p == '\\' && p[1]) p++;
        if (icase ? lc((unsigned char)*p) != lc((unsigned char)*s) : *p != *s)
            return false;
        p++; s++;
    }
    return *s == '\0';
}

/* ---- users and groups, from /etc/passwd and /etc/group --------------- */

/* The id of NAME in FILE (a passwd- or group-format file), -1 if none. */
static long id_of(const char *file, const char *name) {
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    char line[512];
    long id = -1;
    size_t nl = strlen(name);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, name, nl) || line[nl] != ':') continue;
        char *p = strchr(line + nl + 1, ':');      /* past the password */
        if (p) id = strtol(p + 1, 0, 10);
        break;
    }
    fclose(f);
    return id;
}

/* The name of ID in FILE into BUF, false if none. */
static bool name_of(const char *file, long id, char *buf, size_t cap) {
    FILE *f = fopen(file, "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2 || strtol(c2 + 1, 0, 10) != id || c2[1] < '0' || c2[1] > '9') continue;
        size_t n = (size_t)(c1 - line);
        if (n >= cap) n = cap - 1;
        memcpy(buf, line, n);
        buf[n] = '\0';
        found = true;
        break;
    }
    fclose(f);
    return found;
}

/* ---- parsing ---------------------------------------------------------- */

static char **g_av;
static int g_ac, g_ai;
static bool g_has_action;

static const char *peek(void) { return g_ai < g_ac ? g_av[g_ai] : 0; }

static const char *need_arg(const char *pred) {
    if (g_ai >= g_ac) {
        FERR("find: missing argument to `%s'\n", pred);
        exit(1);
    }
    return g_av[g_ai++];
}

/* [+-]N into cmp and n; false if it is not a number. */
static bool num_arg(const char *s, int *cmp, long long *n, const char **rest) {
    *cmp = 0;
    if (*s == '+') { *cmp = 1; s++; }
    else if (*s == '-') { *cmp = -1; s++; }
    if (*s < '0' || *s > '9') return false;
    long long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    *n = v;
    if (rest) *rest = s;
    else if (*s) return false;
    return true;
}

static void bad_number(const char *pred, const char *arg) {
    FERR("find: invalid argument `%s' to `%s'\n", arg, pred);
    exit(1);
}

/* An octal or symbolic mode for -perm: the bits it names. */
static bool parse_mode(const char *s, unsigned *out) {
    if (*s >= '0' && *s <= '7') {
        unsigned m = 0;
        for (; *s; s++) {
            if (*s < '0' || *s > '7') return false;
            m = m * 8 + (unsigned)(*s - '0');
        }
        *out = m & 07777;
        return true;
    }
    /* symbolic: [ugoa]*[=+-][rwxst]* , ... starting from 0 */
    unsigned m = 0;
    while (*s) {
        unsigned who = 0;
        while (*s == 'u' || *s == 'g' || *s == 'o' || *s == 'a') {
            who |= *s == 'u' ? 04700 : *s == 'g' ? 02070 : *s == 'o' ? 01007 : 07777;
            s++;
        }
        if (!who) who = 07777;
        if (*s != '=' && *s != '+' && *s != '-') return false;
        char op = *s++;
        unsigned bits = 0;
        for (; *s && *s != ','; s++) {
            switch (*s) {
            case 'r': bits |= 0444; break;
            case 'w': bits |= 0222; break;
            case 'x': bits |= 0111; break;
            case 's': bits |= 06000; break;
            case 't': bits |= 01000; break;
            default: return false;
            }
        }
        bits &= who;
        if (op == '-') m &= ~bits;
        else if (op == '=') m = (m & ~who) | bits;
        else m |= bits;
        if (*s == ',') s++;
    }
    *out = m;
    return true;
}

static struct node *parse_or(void);

static struct node *parse_primary(void) {
    const char *t = peek();
    if (!t) {
        FERR("find: expected an expression\n");
        exit(1);
    }
    g_ai++;
    if (!strcmp(t, "(")) {
        if (!peek() || !strcmp(peek(), ")")) {
            FERR("find: invalid expression; empty parentheses are not allowed.\n");
            exit(1);
        }
        struct node *e = parse_or();
        if (!peek() || strcmp(peek(), ")")) {
            FERR("find: invalid expression; I was expecting to find a ')' somewhere but did not see one.\n");
            exit(1);
        }
        g_ai++;
        return e;
    }
    if (!strcmp(t, "!") || !strcmp(t, "-not")) {
        struct node *n = mk(N_NOT);
        n->l = parse_primary();
        return n;
    }
    /* global options: true wherever they stand */
    if (!strcmp(t, "-maxdepth") || !strcmp(t, "-mindepth")) {
        const char *a = need_arg(t);
        int cmp; long long v;
        if (!num_arg(a, &cmp, &v, 0) || cmp) {
            FERR("find: Expected a positive decimal integer argument to %s, but got '%s'\n", t, a);
            exit(1);
        }
        if (t[2] == 'a') g_maxdepth = (int)v; else g_mindepth = (int)v;
        return mk(N_TRUE);
    }
    if (!strcmp(t, "-depth") || !strcmp(t, "-d")) { g_depth_first = true; return mk(N_TRUE); }
    if (!strcmp(t, "-xdev") || !strcmp(t, "-mount")) { g_xdev = true; return mk(N_TRUE); }
    if (!strcmp(t, "-follow")) { g_follow = 2; return mk(N_TRUE); }
    if (!strcmp(t, "-noleaf") || !strcmp(t, "-ignore_readdir_race") ||
        !strcmp(t, "-noignore_readdir_race")) return mk(N_TRUE);
    if (!strcmp(t, "-true")) return mk(N_TRUE);
    if (!strcmp(t, "-false")) return mk(N_FALSE);

    if (!strcmp(t, "-name") || !strcmp(t, "-iname") || !strcmp(t, "-path") ||
        !strcmp(t, "-ipath") || !strcmp(t, "-wholename") || !strcmp(t, "-iwholename")) {
        struct node *n = mk(t[1] == 'n' || t[2] == 'n' ? N_NAME : N_PATH);
        n->s = need_arg(t);
        n->icase = t[1] == 'i';
        return n;
    }
    if (!strcmp(t, "-type") || !strcmp(t, "-xtype")) {
        struct node *n = mk(t[1] == 't' ? N_TYPE : N_XTYPE);
        n->s = need_arg(t);
        for (const char *q = n->s; *q; q++) {
            if (!strchr("bcdpfls", *q) || (q[1] && q[1] != ',')) {
                FERR("find: Unknown argument to %s: %s\n", t, n->s);
                exit(1);
            }
            if (q[1] == ',') {
                q++;
                if (!q[1]) {
                    FERR("find: Last file type in list argument to %s is missing, i.e., list is ending on: ','\n", t);
                    exit(1);
                }
            }
        }
        if (!*n->s) {
            FERR("find: Arguments to %s should contain at least one letter\n", t);
            exit(1);
        }
        return n;
    }
    if (!strcmp(t, "-size")) {
        struct node *n = mk(N_SIZE);
        const char *a = need_arg(t), *rest;
        if (!num_arg(a, &n->cmp, &n->n, &rest)) bad_number(t, a);
        switch (*rest) {
        case 0: case 'b': n->unit = 512; break;
        case 'c': n->unit = 1; break;
        case 'w': n->unit = 2; break;
        case 'k': n->unit = 1024; break;
        case 'M': n->unit = 1024LL * 1024; break;
        case 'G': n->unit = 1024LL * 1024 * 1024; break;
        default:
            FERR("find: invalid -size type `%c'\n", *rest);
            exit(1);
        }
        if (rest[0] && rest[1]) bad_number(t, a);
        return n;
    }
    if (!strcmp(t, "-empty")) return mk(N_EMPTY);
    if (!strcmp(t, "-newer") || !strcmp(t, "-samefile")) {
        const char *a = need_arg(t);
        struct stat st;
        if ((g_follow == 2 ? stat(a, &st) : lstat(a, &st)) != 0) {
            FERR("find: '%s': %s\n", a, strerror(errno));
            exit(1);
        }
        struct node *n = mk(t[1] == 'n' ? N_NEWER : N_SAMEFILE);
        n->n = (long long)st.st_mtime;
        n->dev = st.st_dev;
        n->ino = st.st_ino;
        return n;
    }
    if (!strcmp(t, "-mtime") || !strcmp(t, "-atime") || !strcmp(t, "-ctime") ||
        !strcmp(t, "-mmin") || !strcmp(t, "-amin") || !strcmp(t, "-cmin")) {
        struct node *n = mk(N_TIME);
        const char *a = need_arg(t);
        if (!num_arg(a, &n->cmp, &n->n, 0)) bad_number(t, a);
        n->which = t[1];
        n->unit = t[2] == 't' ? 86400 : 60;
        return n;
    }
    if (!strcmp(t, "-perm")) {
        struct node *n = mk(N_PERM);
        const char *a = need_arg(t);
        n->mode_how = 0;
        if (*a == '-' || *a == '/' || *a == '+') n->mode_how = *a == '+' ? '/' : (unsigned char)*a, a++;
        if (!parse_mode(a, &n->mode)) {
            FERR("find: invalid mode '%s'\n", a);
            exit(1);
        }
        return n;
    }
    if (!strcmp(t, "-user") || !strcmp(t, "-group")) {
        struct node *n = mk(t[1] == 'u' ? N_UID : N_GID);
        const char *a = need_arg(t);
        long id = id_of(t[1] == 'u' ? "/etc/passwd" : "/etc/group", a);
        if (id < 0) {
            int cmp; long long v;
            if (!num_arg(a, &cmp, &v, 0) || cmp) {
                FERR("find: invalid %s name or %cID argument to %s: '%s'\n",
                        t[1] == 'u' ? "user" : "group", t[1] == 'u' ? 'U' : 'G',
                        t, a);
                exit(1);
            }
            id = (long)v;
        }
        n->cmp = 0;
        n->n = id;
        return n;
    }
    if (!strcmp(t, "-uid") || !strcmp(t, "-gid") || !strcmp(t, "-links") ||
        !strcmp(t, "-inum")) {
        struct node *n = mk(t[1] == 'u' ? N_UID : t[1] == 'g' ? N_GID :
                            t[1] == 'l' ? N_LINKS : N_INUM);
        const char *a = need_arg(t);
        if (!num_arg(a, &n->cmp, &n->n, 0)) {
            FERR("find: non-numeric argument to %s: '%s'\n", t, a);
            exit(1);
        }
        return n;
    }
    if (!strcmp(t, "-nouser")) return mk(N_NOUSER);
    if (!strcmp(t, "-nogroup")) return mk(N_NOGROUP);
    if (!strcmp(t, "-readable") || !strcmp(t, "-writable") || !strcmp(t, "-executable")) {
        struct node *n = mk(N_ACCESS);
        n->access_mode = t[1] == 'r' ? R_OK : t[1] == 'w' ? W_OK : X_OK;
        return n;
    }

    if (!strcmp(t, "-print")) { g_has_action = true; return mk(N_PRINT); }
    if (!strcmp(t, "-print0")) { g_has_action = true; return mk(N_PRINT0); }
    if (!strcmp(t, "-printf")) {
        struct node *n = mk(N_PRINTF);
        n->s = need_arg(t);
        g_has_action = true;
        return n;
    }
    if (!strcmp(t, "-ls")) { g_has_action = true; return mk(N_LS); }
    if (!strcmp(t, "-delete")) {
        g_has_action = true;
        g_depth_first = true;              /* -delete implies -depth */
        return mk(N_DELETE);
    }
    if (!strcmp(t, "-prune")) return mk(N_PRUNE);
    if (!strcmp(t, "-quit")) {          /* an action: no -print implied */
        g_has_action = true;
        return mk(N_QUIT);
    }
    if (!strcmp(t, "-exec")) {
        int start = g_ai, end = -1;
        bool plus = false;
        for (int i = start; i < g_ac; i++) {
            if (!strcmp(g_av[i], ";")) { end = i; break; }
            if (!strcmp(g_av[i], "+") && i > start && !strcmp(g_av[i - 1], "{}")) {
                end = i; plus = true; break;
            }
        }
        if (end < 0 || end == start) {
            FERR("find: missing argument to `-exec'\n");
            exit(1);
        }
        struct node *n = mk(plus ? N_EXECPLUS : N_EXEC);
        n->argc = end - start - (plus ? 1 : 0);
        n->argv = g_av + start;
        g_ai = end + 1;
        g_has_action = true;
        if (plus) {
            g_plus = (struct node **)realloc(g_plus, (size_t)(g_nplus + 1) * sizeof *g_plus);
            if (!g_plus) { FERR("find: memory exhausted\n"); exit(1); }
            g_plus[g_nplus++] = n;
        }
        return n;
    }
    if (t[0] == '-' && t[1]) {
        FERR("find: unknown predicate `%s'\n", t);
        exit(1);
    }
    FERR("find: paths must precede expression: `%s'\n", t);
    exit(1);
}

static bool starts_primary(const char *t) {
    return t && strcmp(t, ")") && strcmp(t, "-o") && strcmp(t, "-or") &&
           strcmp(t, ",");
}

static struct node *parse_and(void) {
    struct node *l = parse_primary();
    for (;;) {
        const char *t = peek();
        if (t && (!strcmp(t, "-a") || !strcmp(t, "-and"))) {
            g_ai++;
            if (!starts_primary(peek())) {
                FERR("find: expected an expression after '%s'\n", t);
                exit(1);
            }
        } else if (!starts_primary(t)) {
            return l;
        }
        struct node *n = mk(N_AND);
        n->l = l;
        n->r = parse_primary();
        l = n;
    }
}

static struct node *parse_or1(void) {
    struct node *l = parse_and();
    while (peek() && (!strcmp(peek(), "-o") || !strcmp(peek(), "-or"))) {
        const char *op = peek();
        g_ai++;
        if (!starts_primary(peek())) {
            FERR("find: expected an expression after '%s'\n", op);
            exit(1);
        }
        struct node *n = mk(N_OR);
        n->l = l;
        n->r = parse_and();
        l = n;
    }
    return l;
}

static struct node *parse_or(void) {
    struct node *l = parse_or1();
    while (peek() && !strcmp(peek(), ",")) {
        g_ai++;
        struct node *n = mk(N_COMMA);
        n->l = l;
        n->r = parse_or1();
        l = n;
    }
    return l;
}

/* ---- evaluation ------------------------------------------------------- */

struct ent {
    const char *path;            /* as find names it */
    const char *name;            /* its last component */
    struct stat st;              /* lstat, or stat when following */
    bool have;                   /* st is valid */
    int depth;
    bool prune;                  /* set by -prune */
};

static char type_char(mode_t m) {
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISSOCK(m)) return 's';
    if (S_ISCHR(m)) return 'c';
    if (S_ISBLK(m)) return 'b';
    return 'f';
}

static bool type_in(const char *set, char c) {
    for (; *set; set++) if (*set == c) return true;
    return false;
}

static bool cmp_num(int cmp, long long v, long long n) {
    return cmp > 0 ? v > n : cmp < 0 ? v < n : v == n;
}

static void put_mode(mode_t m, char *out) {
    out[0] = type_char(m) == 'f' ? '-' : type_char(m);
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & 04000) out[3] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[6] = (m & 0010) ? 's' : 'S';
    if (m & 01000) out[9] = (m & 0001) ? 't' : 'T';
    out[10] = '\0';
}

static void print_escaped_fmt(struct ent *e, const char *f) {
    char buf[64];
    for (; *f; f++) {
        if (*f == '\\' && f[1]) {
            f++;
            switch (*f) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case '0': putchar('\0'); break;
            case 'a': putchar('\a'); break;
            case 'b': putchar('\b'); break;
            case 'f': putchar('\f'); break;
            case 'r': putchar('\r'); break;
            case 'v': putchar('\v'); break;
            case '\\': putchar('\\'); break;
            case 'c': fflush(stdout); return;
            default: putchar('\\'); putchar(*f); break;
            }
            continue;
        }
        if (*f != '%' || !f[1]) { putchar(*f); continue; }
        f++;
        switch (*f) {
        case '%': putchar('%'); break;
        case 'p': fputs(e->path, stdout); break;
        case 'f': fputs(e->name, stdout); break;
        case 'h': {
            const char *sl = strrchr(e->path, '/');
            if (!sl) putchar('.');
            else if (sl == e->path) putchar('/');
            else fwrite(e->path, 1, (size_t)(sl - e->path), stdout);
            break;
        }
        case 'P': {                             /* path below its start */
            const char *p = e->path;
            /* the starting point is what precedes depth components */
            int skip = e->depth;
            const char *q = p + strlen(p);
            while (skip > 0 && q > p) { q--; if (*q == '/') skip--; }
            fputs(e->depth ? q + 1 : "", stdout);
            break;
        }
        case 'd': printf("%d", e->depth); break;
        case 's': printf("%lld", e->have ? (long long)e->st.st_size : 0LL); break;
        case 'm': printf("%o", e->have ? (unsigned)(e->st.st_mode & 07777) : 0); break;
        case 'M': if (e->have) { put_mode(e->st.st_mode, buf); fputs(buf, stdout); } break;
        case 'n': printf("%lu", e->have ? (unsigned long)e->st.st_nlink : 0UL); break;
        case 'i': printf("%lu", e->have ? (unsigned long)e->st.st_ino : 0UL); break;
        case 'U': printf("%lu", e->have ? (unsigned long)e->st.st_uid : 0UL); break;
        case 'G': printf("%lu", e->have ? (unsigned long)e->st.st_gid : 0UL); break;
        case 'u':
            if (e->have && name_of("/etc/passwd", (long)e->st.st_uid, buf, sizeof buf)) fputs(buf, stdout);
            else printf("%lu", e->have ? (unsigned long)e->st.st_uid : 0UL);
            break;
        case 'g':
            if (e->have && name_of("/etc/group", (long)e->st.st_gid, buf, sizeof buf)) fputs(buf, stdout);
            else printf("%lu", e->have ? (unsigned long)e->st.st_gid : 0UL);
            break;
        case 'y': putchar(e->have ? type_char(e->st.st_mode) : 'U'); break;
        case 'l': {
            if (e->have && S_ISLNK(e->st.st_mode)) {
                char lb[1024];
                ssize_t n = readlink(e->path, lb, sizeof lb - 1);
                if (n > 0) { lb[n] = '\0'; fputs(lb, stdout); }
            }
            break;
        }
        case 'T': case 't':                     /* seconds since the epoch only */
            if (*f == 'T' && f[1]) f++;
            printf("%lld", e->have ? (long long)e->st.st_mtime : 0LL);
            break;
        default:
            putchar('%');
            putchar(*f);
            break;
        }
    }
}

static void do_ls(struct ent *e) {
    char m[11], u[64], g[64];
    if (!e->have) return;
    put_mode(e->st.st_mode, m);
    if (!name_of("/etc/passwd", (long)e->st.st_uid, u, sizeof u))
        snprintf(u, sizeof u, "%lu", (unsigned long)e->st.st_uid);
    if (!name_of("/etc/group", (long)e->st.st_gid, g, sizeof g))
        snprintf(g, sizeof g, "%lu", (unsigned long)e->st.st_gid);
    printf("%6lu %4lld %s %3lu %-8s %-8s %8lld %s", (unsigned long)e->st.st_ino,
           ((long long)e->st.st_size + 1023) / 1024, m,
           (unsigned long)e->st.st_nlink, u, g, (long long)e->st.st_size, e->path);
    if (S_ISLNK(e->st.st_mode)) {
        char lb[1024];
        ssize_t n = readlink(e->path, lb, sizeof lb - 1);
        if (n > 0) { lb[n] = '\0'; printf(" -> %s", lb); }
    }
    putchar('\n');
}

/* Run ARGV (with {} as given) and wait; its success. */
static bool run_cmd(char **argv) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        FERR("find: cannot fork: %s\n", strerror(errno));
        g_status = 1;
        return false;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        FERR("find: '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) { g_status = 1; return false; }
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* -exec CMD ;  -- every {} in every word becomes the path. */
static bool do_exec(struct node *n, struct ent *e) {
    char **av = (char **)xmalloc((size_t)(n->argc + 1) * sizeof *av);
    size_t plen = strlen(e->path);
    for (int i = 0; i < n->argc; i++) {
        const char *w = n->argv[i];
        size_t cnt = 0;
        for (const char *q = strstr(w, "{}"); q; q = strstr(q + 2, "{}")) cnt++;
        if (!cnt) { av[i] = (char *)w; continue; }
        char *o = (char *)xmalloc(strlen(w) + cnt * plen + 1), *d = o;
        for (const char *q = w; *q; ) {
            if (q[0] == '{' && q[1] == '}') { memcpy(d, e->path, plen); d += plen; q += 2; }
            else *d++ = *q++;
        }
        *d = '\0';
        av[i] = o;
    }
    av[n->argc] = 0;
    bool ok = run_cmd(av);
    for (int i = 0; i < n->argc; i++) if (av[i] != n->argv[i]) free(av[i]);
    free(av);
    return ok;
}

/* -exec CMD {} +  -- the paths gathered and run in batches. */
static void flush_plus(struct node *n) {
    if (!n->nbatch) return;
    int k = n->argc + n->nbatch;
    char **av = (char **)xmalloc((size_t)(k + 1) * sizeof *av);
    for (int i = 0; i < n->argc; i++) av[i] = n->argv[i];
    for (int i = 0; i < n->nbatch; i++) av[n->argc + i] = n->batch[i];
    av[k] = 0;
    if (!run_cmd(av)) g_status = 1;
    for (int i = 0; i < n->nbatch; i++) free(n->batch[i]);
    free(av);
    n->nbatch = 0;
    n->batch_bytes = 0;
}

static void add_plus(struct node *n, const char *path) {
    if (n->nbatch == n->capbatch) {
        n->capbatch = n->capbatch ? n->capbatch * 2 : 64;
        n->batch = (char **)realloc(n->batch, (size_t)n->capbatch * sizeof *n->batch);
        if (!n->batch) { FERR("find: memory exhausted\n"); exit(1); }
    }
    n->batch[n->nbatch] = (char *)xmalloc(strlen(path) + 1);
    strcpy(n->batch[n->nbatch++], path);
    n->batch_bytes += strlen(path) + 1;
    if (n->nbatch >= 4096 || n->batch_bytes >= 96 * 1024) flush_plus(n);
}

static bool dir_is_empty(const char *path) {
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *de;
    bool empty = true;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { empty = false; break; }
    }
    closedir(d);
    return empty;
}

static bool eval(struct node *n, struct ent *e) {
    switch (n->kind) {
    case N_AND:   return eval(n->l, e) && eval(n->r, e);
    case N_OR:    return eval(n->l, e) || eval(n->r, e);
    case N_NOT:   return !eval(n->l, e);
    case N_COMMA: (void)eval(n->l, e); return eval(n->r, e);
    case N_TRUE:  return true;
    case N_FALSE: return false;
    case N_NAME: {
        /* a trailing slash on a starting point is not part of its name */
        char tmp[1024];
        const char *nm = e->name;
        size_t len = strlen(nm);
        if (len > 1 && nm[len - 1] == '/' && len < sizeof tmp) {
            memcpy(tmp, nm, len);
            while (len > 1 && tmp[len - 1] == '/') len--;
            tmp[len] = '\0';
            nm = tmp;
        }
        return pmatch(n->s, nm, n->icase);
    }
    case N_PATH:  return pmatch(n->s, e->path, n->icase);
    case N_TYPE:  return e->have && type_in(n->s, type_char(e->st.st_mode));
    case N_XTYPE: {
        struct stat st;
        if (e->have && S_ISLNK(e->st.st_mode)) {
            if (stat(e->path, &st) != 0) return type_in(n->s, 'l');
            return type_in(n->s, type_char(st.st_mode));
        }
        return e->have && type_in(n->s, type_char(e->st.st_mode));
    }
    case N_SIZE: {
        if (!e->have) return false;
        long long sz = (long long)e->st.st_size;
        long long units = (sz + n->unit - 1) / n->unit;   /* rounded up */
        return cmp_num(n->cmp, units, n->n);
    }
    case N_EMPTY:
        if (!e->have) return false;
        if (S_ISDIR(e->st.st_mode)) return dir_is_empty(e->path);
        return S_ISREG(e->st.st_mode) && e->st.st_size == 0;
    case N_NEWER: return e->have && (long long)e->st.st_mtime > n->n;
    case N_SAMEFILE:
        return e->have && e->st.st_dev == n->dev && e->st.st_ino == n->ino;
    case N_TIME: {
        if (!e->have) return false;
        long long t = n->which == 'a' ? (long long)e->st.st_atime :
                      n->which == 'c' ? (long long)e->st.st_mtime :   /* no st_ctime here */
                                        (long long)e->st.st_mtime;
        long long age = (long long)g_now - t;
        long long units;
        if (n->unit == 86400) units = age >= 0 ? age / 86400 : -((-age + 86399) / 86400);
        else units = age >= 0 ? (age + 59) / 60 : -((-age) / 60);
        return cmp_num(n->cmp, units, n->n);
    }
    case N_PERM: {
        if (!e->have) return false;
        unsigned m = (unsigned)e->st.st_mode & 07777;
        if (n->mode_how == '-') return (m & n->mode) == n->mode;
        if (n->mode_how == '/') return n->mode == 0 || (m & n->mode) != 0;
        return m == n->mode;
    }
    case N_UID:   return e->have && cmp_num(n->cmp, (long long)e->st.st_uid, n->n);
    case N_GID:   return e->have && cmp_num(n->cmp, (long long)e->st.st_gid, n->n);
    case N_NOUSER: {
        char b[64];
        return e->have && !name_of("/etc/passwd", (long)e->st.st_uid, b, sizeof b);
    }
    case N_NOGROUP: {
        char b[64];
        return e->have && !name_of("/etc/group", (long)e->st.st_gid, b, sizeof b);
    }
    case N_LINKS: return e->have && cmp_num(n->cmp, (long long)e->st.st_nlink, n->n);
    case N_INUM:  return e->have && cmp_num(n->cmp, (long long)e->st.st_ino, n->n);
    case N_ACCESS: return access(e->path, n->access_mode) == 0;
    case N_PRINT: printf("%s\n", e->path); return true;
    case N_PRINT0: fputs(e->path, stdout); putchar('\0'); return true;
    case N_PRINTF: print_escaped_fmt(e, n->s); return true;
    case N_LS: do_ls(e); return true;
    case N_EXEC: return do_exec(n, e);
    case N_EXECPLUS: add_plus(n, e->path); return true;
    case N_DELETE: {
        int r = (e->have && S_ISDIR(e->st.st_mode)) ? rmdir(e->path) : unlink(e->path);
        if (r != 0) {
            FERR("find: cannot delete '%s': %s\n", e->path, strerror(errno));
            g_status = 1;
            return false;
        }
        return true;
    }
    case N_PRUNE:
        if (!g_depth_first) e->prune = true;
        return true;
    case N_QUIT:
        g_quit = true;
        return true;
    }
    return false;
}

/* ---- the walk --------------------------------------------------------- */

static void visit(const char *path, const char *name, int depth, dev_t root_dev,
                  bool cmdline);

static void walk_dir(const char *path, int depth, dev_t root_dev) {
    DIR *d = opendir(path);
    if (!d) {
        FERR("find: '%s': %s\n", path, strerror(errno));
        g_status = 1;
        return;
    }
    size_t plen = strlen(path);
    struct dirent *de;
    while (!g_quit && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        size_t nlen = strlen(de->d_name);
        char *child = (char *)xmalloc(plen + nlen + 2);
        memcpy(child, path, plen);
        size_t k = plen;
        if (!(plen && path[plen - 1] == '/')) child[k++] = '/';
        memcpy(child + k, de->d_name, nlen + 1);
        visit(child, child + k, depth + 1, root_dev, false);
        free(child);
    }
    closedir(d);
}

static void visit(const char *path, const char *name, int depth, dev_t root_dev,
                  bool cmdline) {
    struct ent e;
    memset(&e, 0, sizeof e);
    e.path = path;
    e.name = name;
    e.depth = depth;
    bool follow = g_follow == 2 || (g_follow == 1 && cmdline);
    int r = follow ? stat(path, &e.st) : lstat(path, &e.st);
    if (r != 0 && follow && errno == ENOENT) r = lstat(path, &e.st);  /* dangling */
    if (r != 0) {
        FERR("find: '%s': %s\n", path, strerror(errno));
        g_status = 1;
        return;
    }
    e.have = true;
    if (cmdline) root_dev = e.st.st_dev;
    bool is_dir = S_ISDIR(e.st.st_mode);
    bool in_range = depth >= g_mindepth;
    if (!g_depth_first && in_range) (void)eval(g_expr, &e);
    if (g_quit) return;
    if (is_dir && !e.prune && (g_maxdepth < 0 || depth < g_maxdepth) &&
        !(g_xdev && !cmdline && e.st.st_dev != root_dev))
        walk_dir(path, depth, root_dev);
    if (g_quit) return;
    if (g_depth_first && in_range) (void)eval(g_expr, &e);
}

int main(int argc, char **argv) {
    g_now = time(0);
    int i = 1;
    for (; i < argc; i++) {                     /* -H -L -P, before the paths */
        if (!strcmp(argv[i], "-H")) g_follow = 1;
        else if (!strcmp(argv[i], "-L")) g_follow = 2;
        else if (!strcmp(argv[i], "-P")) g_follow = 0;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else break;
    }
    int first_path = i;
    while (i < argc && !(argv[i][0] == '-' && argv[i][1]) &&
           strcmp(argv[i], "(") && strcmp(argv[i], "!"))
        i++;
    int npaths = i - first_path;

    g_av = argv;
    g_ac = argc;
    g_ai = i;
    struct node *e = g_ai < g_ac ? parse_or() : 0;
    if (g_ai < g_ac) {
        if (!strcmp(g_av[g_ai], ")"))
            FERR("find: you have too many ')'\n");
        else
            FERR("find: paths must precede expression: `%s'\n", g_av[g_ai]);
        return 1;
    }
    if (!e) e = mk(N_TRUE);
    if (!g_has_action) {
        struct node *a = mk(N_AND);
        a->l = e;
        a->r = mk(N_PRINT);
        e = a;
    }
    g_expr = e;

    if (npaths == 0) {
        visit(".", ".", 0, 0, true);
    } else {
        for (int p = first_path; p < first_path + npaths && !g_quit; p++) {
            const char *path = argv[p];
            if (!*path) {
                FERR("find: '': No such file or directory\n");
                g_status = 1;
                continue;
            }
            visit(path, path, 0, 0, true);
        }
    }
    for (int k = 0; k < g_nplus; k++) flush_plus(g_plus[k]);
    fflush(stdout);
    return g_status;
}
