/* user_ed -- the POSIX line editor, native (2026-09-04, native-parity batch).
 *
 * Commands: a c d e E f g G h H i j k l m n p P q Q r s t u v V w W wq z ! = #
 * and the bare address (print). Addresses: n . $ 'x /re/ ?re? +n -n + - ^ ,
 * ; and arithmetic on them. Regexes are POSIX basic through libtoby's
 * regex.h. Substitution supports & \1..\9 \& \\ %, the g/p/l/n/N flags and a
 * multi-line replacement via a trailing backslash. g/G/v/V run command lists
 * with backslash continuation. One level of undo (the whole buffer is
 * snapshotted before each modifying command; files here are small).
 *
 * Diagnostics follow GNU ed: `?` on error, `h` explains, `H` toggles
 * explanations, byte counts on e/r/w unless -s. Exit status is 0, or 1 if
 * the last command before EOF failed or the buffer was left modified. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <regex.h>
#include <sys/stat.h>

/* --------------------------------------------------------------- buffer */
static char **L;          /* lines 1..n stored at L[1..n]; L[0] unused */
static int    n, cap;
static int    cur;        /* current line */
static char **U; static int un; static int u_cur;   /* undo snapshot */
static int    modified, warned;
static int    marks[26];
static char   fname[512];
static char   last_re[512];
static char   last_rep[512]; static int have_last_rep;
static const char *last_err = "";
static int    hmode, quiet, last_z = 22;
static const char *prompt = NULL; static int prompt_on;
static int    last_failed;
static char   sflag_last[16];

static void die(const char *m) { fprintf(stderr, "ed: %s\n", m); exit(2); }

static void ensure(int need) {
    if (need + 1 <= cap) return;
    while (cap < need + 1) cap = cap ? cap * 2 : 256;
    L = realloc(L, (size_t)cap * sizeof *L);
    if (!L) die("out of memory");
}

static void snapshot(void) {
    for (int i = 1; i <= un; i++) free(U[i]);
    free(U);
    U = malloc((size_t)(n + 1) * sizeof *U);
    if (!U) die("out of memory");
    for (int i = 1; i <= n; i++) U[i] = strdup(L[i]);
    un = n; u_cur = cur;
}

static void insert_after(int a, char **lines, int k) {
    ensure(n + k);
    memmove(&L[a + 1 + k], &L[a + 1], (size_t)(n - a) * sizeof *L);
    for (int i = 0; i < k; i++) L[a + 1 + i] = lines[i];
    n += k;
    for (int i = 0; i < 26; i++) if (marks[i] > a) marks[i] += k;
    modified = 1;
}

static void delete_range(int a, int b) {
    for (int i = a; i <= b; i++) free(L[i]);
    memmove(&L[a], &L[b + 1], (size_t)(n - b) * sizeof *L);
    int k = b - a + 1;
    n -= k;
    for (int i = 0; i < 26; i++) {
        if (marks[i] >= a && marks[i] <= b) marks[i] = 0;
        else if (marks[i] > b) marks[i] -= k;
    }
    modified = 1;
}

/* ---------------------------------------------------------------- input */
static char *read_line(FILE *f) {
    static char buf[8192];
    if (!fgets(buf, sizeof buf, f)) return NULL;
    size_t l = strlen(buf);
    if (l && buf[l - 1] == '\n') buf[l - 1] = '\0';
    return buf;
}

/* Read text lines up to a lone "." (or EOF). Returns malloc'd array. */
static int read_text(char ***out) {
    char **arr = NULL; int k = 0, kc = 0;
    char *s;
    while ((s = read_line(stdin))) {
        if (!strcmp(s, ".")) break;
        if (k == kc) { kc = kc ? kc * 2 : 16; arr = realloc(arr, (size_t)kc * sizeof *arr); }
        arr[k++] = strdup(s);
    }
    *out = arr;
    return k;
}

/* ---------------------------------------------------------------- error */
static int err(const char *m) { last_err = m; printf("?\n"); if (hmode) printf("%s\n", m); last_failed = 1; return -1; }

/* ---------------------------------------------------------------- regex */
static regex_t g_re; static int g_re_ok;

static int compile_re(const char *pat) {
    if (g_re_ok) { regfree(&g_re); g_re_ok = 0; }
    if (regcomp(&g_re, pat, 0) != 0) return -1;
    g_re_ok = 1;
    return 0;
}

/* Parse /re/ or ?re? at *pp (pointing at the delimiter). Copies the pattern
 * into pat; an empty pattern means the last one. Returns 0 or -1. */
static int parse_re(const char **pp, char delim, char *pat, size_t cap) {
    const char *p = *pp + 1;
    size_t k = 0;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1]) {
            if (p[1] == delim) { p++; }               /* escaped delimiter */
            else { if (k + 1 < cap) pat[k++] = *p; p++; }
        }
        if (k + 1 < cap) pat[k++] = *p;
        p++;
    }
    pat[k] = '\0';
    if (*p == delim) p++;
    *pp = p;
    if (!pat[0]) {
        if (!last_re[0]) return -1;
        snprintf(pat, cap, "%s", last_re);
    } else snprintf(last_re, sizeof last_re, "%s", pat);
    return 0;
}

static int line_matches(int i) {
    return regexec(&g_re, L[i], 0, NULL, 0) == 0;
}

/* --------------------------------------------------------------- address */
/* Returns 1 with *out set, 0 if no address at *pp, -1 on error. */
static int parse_one_addr(const char **pp, int *out) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    int have = 0, v = 0;
    if (isdigit((unsigned char)*p)) { v = (int)strtol(p, (char **)&p, 10); have = 1; }
    else if (*p == '.') { v = cur; p++; have = 1; }
    else if (*p == '$') { v = n; p++; have = 1; }
    else if (*p == '\'') {
        p++;
        if (*p < 'a' || *p > 'z') return -1;
        v = marks[*p - 'a']; p++;
        if (v == 0) { last_err = "Invalid mark"; return -1; }
        have = 1;
    } else if (*p == '/' || *p == '?') {
        char pat[512]; char d = *p;
        if (parse_re(&p, d, pat, sizeof pat)) { last_err = "No previous pattern"; return -1; }
        if (compile_re(pat)) { last_err = "Invalid pattern"; return -1; }
        if (n == 0) { last_err = "No match"; return -1; }
        int i = cur, found = 0;
        for (int k = 0; k < n; k++) {
            if (d == '/') { i = i % n + 1; } else { i = i <= 1 ? n : i - 1; }
            if (line_matches(i)) { found = 1; break; }
        }
        if (!found) { last_err = "No match"; return -1; }
        v = i; have = 1;
    }
    /* offsets */
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '+' || *p == '-') {
            char op = *p++;
            int k = 1;
            if (isdigit((unsigned char)*p)) k = (int)strtol(p, (char **)&p, 10);
            if (!have) { v = cur; have = 1; }
            v += op == '+' ? k : -k;
        } else if (isdigit((unsigned char)*p) && have) {
            /* "3 5" style: second number is an offset in some eds; treat as error like GNU? GNU: addr followed by number = +number */
            int k = (int)strtol(p, (char **)&p, 10);
            v += k;
        } else break;
    }
    *pp = p;
    if (!have) return 0;
    *out = v;
    return 1;
}

/* Parse the full address prefix: returns count of addresses (0,1,2), sets
 * a1/a2, -1 on error. Handles , ; and % shorthands. */
static int parse_addrs(const char **pp, int *a1, int *a2) {
    const char *p = *pp;
    int cnt = 0, v;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ',' || *p == ';' || *p == '%') {
        char c = *p++;
        int base = (c == ';') ? cur : 1;
        int r = parse_one_addr(&p, &v);
        if (r < 0) return -1;
        if (r == 0 && c == '%') { *a1 = 1; *a2 = n; *pp = p; return 2; }
        if (r == 0) { *a1 = base; *a2 = n; *pp = p; return 2; }
        if (c == ';') cur = base;
        *a1 = base; *a2 = v; *pp = p; return 2;
    }
    int r = parse_one_addr(&p, &v);
    if (r < 0) return -1;
    if (r == 0) { *pp = p; return 0; }
    *a1 = *a2 = v; cnt = 1;
    while (*p == ' ' || *p == '\t') p++;
    while (*p == ',' || *p == ';') {
        char c = *p++;
        if (c == ';') cur = *a2;
        r = parse_one_addr(&p, &v);
        if (r < 0) return -1;
        if (r == 0) v = (c == ';') ? n : *a2;
        *a1 = *a2; *a2 = v; cnt = 2;
        while (*p == ' ' || *p == '\t') p++;
    }
    *pp = p;
    return cnt;
}

static int check_range(int a, int b, int allow_zero) {
    if (a > b) return err("Invalid address");
    if (a < (allow_zero ? 0 : 1) || b > n) return err("Invalid address");
    return 0;
}

/* ----------------------------------------------------------------- print */
static void print_line(int i, int mode /* 'p' 'n' 'l' */) {
    if (mode == 'n') printf("%d\t", i);
    if (mode == 'l') {
        for (const unsigned char *s = (const unsigned char *)L[i]; *s; s++) {
            if (*s == '\\') fputs("\\\\", stdout);
            else if (*s == '\t') fputs("\\t", stdout);
            else if (*s == '\b') fputs("\\b", stdout);
            else if (*s == '\f') fputs("\\f", stdout);
            else if (*s == '\r') fputs("\\r", stdout);
            else if (*s == '\v') fputs("\\v", stdout);
            else if (*s == 7) fputs("\\a", stdout);
            else if (*s < 32 || *s == 127) printf("\\%03o", *s);
            else if (*s == '$') fputs("\\$", stdout);
            else putchar(*s);
        }
        puts("$");
    } else puts(L[i]);
}

/* ------------------------------------------------------------------ file */
static long read_file_into(const char *path, int after, int *count) {
    FILE *f;
    int pipe = path[0] == '!';
    if (pipe) f = popen(path + 1, "r"); else f = fopen(path, "r");
    if (!f) return -1;
    char **arr = NULL; int k = 0, kc = 0; long bytes = 0;
    char *s;
    while ((s = read_line(f))) {
        bytes += (long)strlen(s) + 1;
        if (k == kc) { kc = kc ? kc * 2 : 64; arr = realloc(arr, (size_t)kc * sizeof *arr); }
        arr[k++] = strdup(s);
    }
    if (pipe) pclose(f); else fclose(f);
    if (k) insert_after(after, arr, k);
    free(arr);
    *count = k;
    return bytes;
}

static long write_file_from(const char *path, int a, int b, int append) {
    FILE *f;
    int pipe = path[0] == '!';
    if (pipe) f = popen(path + 1, "w"); else f = fopen(path, append ? "a" : "w");
    if (!f) return -1;
    long bytes = 0;
    for (int i = a; i <= b && i <= n; i++) { fputs(L[i], f); fputc('\n', f); bytes += (long)strlen(L[i]) + 1; }
    if (pipe) pclose(f); else fclose(f);
    return bytes;
}

/* ------------------------------------------------------------ substitute */
static int do_subst(int a, int b, const char *args) {
    const char *p = args;
    char pat[512], rep[1024];
    int gflag = 0, pflag = 0, nth = 1;
    if (!*p || isspace((unsigned char)*p) || isdigit((unsigned char)*p) || *p == 'g' || *p == 'p' || *p == 'l' || *p == 'n') {
        /* repeat last substitution: s [flags] */
        if (!last_re[0] || !have_last_rep) return err("No previous substitution");
        snprintf(pat, sizeof pat, "%s", last_re);
        snprintf(rep, sizeof rep, "%s", last_rep);
        for (; *p; p++) { if (*p == 'g') gflag = 1; else if (*p == 'p') pflag = 'p'; else if (*p == 'l') pflag = 'l'; else if (*p == 'n') pflag = 'n'; else if (*p == 'r') {} else if (isdigit((unsigned char)*p)) { nth = (int)strtol(p, (char **)&p, 10); p--; } }
    } else {
        char delim = *p;
        if (parse_re(&p, delim, pat, sizeof pat)) return err("No previous pattern");
        /* replacement up to the delimiter; a trailing unescaped backslash at
         * end of input means "continue on the next line" (multi-line). */
        size_t k = 0;
        for (;;) {
            while (*p && *p != delim) {
                if (*p == '\\' && p[1]) { if (k + 2 < sizeof rep) { rep[k++] = *p; rep[k++] = p[1]; } p += 2; continue; }
                if (*p == '\\' && !p[1]) {
                    /* escaped newline: read continuation */
                    char *nx = read_line(stdin);
                    if (!nx) break;
                    if (k + 1 < sizeof rep) rep[k++] = '\n';
                    p = nx; continue;
                }
                if (k + 1 < sizeof rep) rep[k++] = *p;
                p++;
            }
            break;
        }
        rep[k] = '\0';
        if (*p == delim) p++;
        if (!strcmp(rep, "%")) { if (!have_last_rep) return err("No previous substitution"); snprintf(rep, sizeof rep, "%s", last_rep); }
        else { snprintf(last_rep, sizeof last_rep, "%s", rep); have_last_rep = 1; }
        for (; *p; p++) {
            if (*p == 'g') gflag = 1;
            else if (*p == 'p') pflag = 'p';
            else if (*p == 'l') pflag = 'l';
            else if (*p == 'n') pflag = 'n';
            else if (isdigit((unsigned char)*p)) { nth = (int)strtol(p, (char **)&p, 10); p--; }
            else if (isspace((unsigned char)*p)) {}
            else return err("Invalid command suffix");
        }
    }
    if (compile_re(pat)) return err("Invalid pattern");
    snprintf(sflag_last, sizeof sflag_last, "%s%s", gflag ? "g" : "", pflag ? "p" : "");
    int any = 0, last = 0;
    for (int i = a; i <= b; i++) {
        const char *s = L[i];
        char out[16384]; size_t o = 0;
        size_t off = 0; int count = 0, did = 0;
        regmatch_t m[10];
        while (regexec(&g_re, s + off, 10, m, off ? REG_NOTBOL : 0) == 0) {
            count++;
            size_t ms = off + (size_t)m[0].rm_so, me = off + (size_t)m[0].rm_eo;
            /* copy text before the match */
            if (o + (ms - off) < sizeof out) { memcpy(out + o, s + off, ms - off); o += ms - off; }
            if (count >= nth && (gflag || count == nth)) {
                did = 1;
                for (const char *r = rep; *r; r++) {
                    if (*r == '&') { size_t l = me - ms; if (o + l < sizeof out) { memcpy(out + o, s + ms, l); o += l; } }
                    else if (*r == '\\' && r[1]) {
                        r++;
                        if (*r >= '1' && *r <= '9') {
                            int g = *r - '0';
                            if (m[g].rm_so >= 0) { size_t gs = off + (size_t)m[g].rm_so, ge = off + (size_t)m[g].rm_eo; if (o + (ge - gs) < sizeof out) { memcpy(out + o, s + gs, ge - gs); o += ge - gs; } }
                        } else if (*r == 'n') { if (o + 1 < sizeof out) out[o++] = '\n'; }
                        else if (o + 1 < sizeof out) out[o++] = *r;
                    } else if (o + 1 < sizeof out) out[o++] = *r;
                }
            } else {
                if (o + (me - ms) < sizeof out) { memcpy(out + o, s + ms, me - ms); o += me - ms; }
            }
            if (me == ms) {                 /* empty match: step one char */
                if (s[me]) { if (o + 1 < sizeof out) out[o++] = s[me]; me++; }
                else { off = me; break; }
            }
            off = me;
            if (!gflag && did) break;
        }
        if (!did) continue;
        size_t rest = strlen(s + off);
        if (o + rest < sizeof out) { memcpy(out + o, s + off, rest); o += rest; }
        out[o] = '\0';
        /* split on embedded newlines */
        int parts = 1;
        for (size_t k = 0; k < o; k++) if (out[k] == '\n') parts++;
        if (parts == 1) {
            free(L[i]); L[i] = strdup(out);
            last = i;
        } else {
            char **arr = malloc((size_t)parts * sizeof *arr);
            int k = 0; char *seg = out;
            for (char *q = out; ; q++) {
                if (*q == '\n' || *q == '\0') { char c = *q; *q = '\0'; arr[k++] = strdup(seg); seg = q + 1; if (!c) break; }
            }
            free(L[i]);
            L[i] = arr[0];
            insert_after(i, arr + 1, parts - 1);
            i += parts - 1; b += parts - 1;
            last = i;
            free(arr);
        }
        any = 1;
        modified = 1;
    }
    if (!any) return err("No match");
    cur = last;
    if (pflag) print_line(cur, pflag);
    return 0;
}

/* ------------------------------------------------------------- commands */
static int exec_cmd(const char *line, int in_global);

static int run_global(int a, int b, const char *args, int invert, int interactive) {
    char pat[512];
    if (!*args) return err("Invalid pattern delimiter");
    char delim = *args;
    const char *p = args;
    if (parse_re(&p, delim, pat, sizeof pat)) return err("No previous pattern");
    if (compile_re(pat)) return err("Invalid pattern");
    /* mark matching lines by identity */
    char **marked = malloc((size_t)(n + 1) * sizeof *marked); int nm = 0;
    for (int i = a; i <= b; i++) { int hit = line_matches(i); if (hit != invert) marked[nm++] = L[i]; }
    /* command list: rest of line, with backslash continuation */
    char cmds[8192]; size_t cl = 0;
    snprintf(cmds, sizeof cmds, "%s", p);
    cl = strlen(cmds);
    while (cl && cmds[cl - 1] == '\\') {
        cmds[cl - 1] = '\n';
        char *nx = read_line(stdin);
        if (!nx) break;
        size_t nl = strlen(nx);
        if (cl + nl + 1 < sizeof cmds) { memcpy(cmds + cl, nx, nl + 1); cl += nl; }
    }
    if (!interactive && !cmds[0]) snprintf(cmds, sizeof cmds, "p");
    int rc = 0;
    for (int k = 0; k < nm; k++) {
        int idx = 0;
        for (int i = 1; i <= n; i++) if (L[i] == marked[k]) { idx = i; break; }
        if (!idx) continue;
        cur = idx;
        if (interactive) {
            print_line(cur, 'p');
            char *c = read_line(stdin);
            if (!c) break;
            if (!strcmp(c, "&")) c = cmds; else snprintf(cmds, sizeof cmds, "%s", c);
            if (!cmds[0]) continue;
        }
        /* execute each command line in the list */
        char *copy = strdup(cmds);
        char *s = copy;
        while (s && *s) {
            char *nl = strchr(s, '\n');
            if (nl) *nl = '\0';
            if (exec_cmd(s, 1) < 0) { rc = -1; break; }
            s = nl ? nl + 1 : NULL;
        }
        free(copy);
        if (rc < 0) break;
    }
    free(marked);
    return rc;
}

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

static int exec_cmd(const char *line, int in_global) {
    const char *p = line;
    int a1 = 0, a2 = 0;
    int na = parse_addrs(&p, &a1, &a2);
    if (na < 0) return err(last_err[0] ? last_err : "Invalid address");
    p = skip_ws(p);
    char c = *p ? *p++ : '\0';
    int a = na ? a1 : cur, b = na ? a2 : cur;
    switch (c) {
    case '\0':
        /* bare address: print it; no address: next line */
        if (!na) { a = b = cur + 1; }
        if (check_range(b, b, 0)) return -1;
        cur = b; print_line(cur, 'p'); return 0;
    case 'p': case 'n': case 'l': {
        if (check_range(a, b, 0)) return -1;
        int m = c;
        for (const char *q = skip_ws(p); *q; q++) { if (*q == 'p') m = 'p'; else if (*q == 'n') m = 'n'; else if (*q == 'l') m = 'l'; else return err("Invalid command suffix"); }
        for (int i = a; i <= b; i++) print_line(i, m);
        cur = b; return 0;
    }
    case '=':
        if (!na) b = n;
        if (b < 0 || b > n) return err("Invalid address");
        printf("%d\n", b); return 0;
    case '#': return 0;
    case 'a': case 'i': case 'c': {
        if (c == 'a') { if (na && (a2 < 0 || a2 > n)) return err("Invalid address"); if (!na) b = cur; }
        else if (c == 'i') { if (!na) b = cur; if (b < 0 || b > n) return err("Invalid address"); if (b > 0) b--; else b = 0; if (n == 0) b = 0; }
        else { if (check_range(a, b, 0)) return -1; }
        if (!in_global) snapshot();
        char **text; int k = read_text(&text);
        if (c == 'c') { delete_range(a, b); b = a - 1; }
        if (k) { insert_after(b, text, k); cur = b + k; }
        else cur = c == 'c' ? (b < 1 ? (n ? 1 : 0) : b) : b;
        if (c == 'i' && k == 0 && n) cur = b + 1 <= n ? b + 1 : n;
        free(text);
        return 0;
    }
    case 'd':
        if (check_range(a, b, 0)) return -1;
        if (!in_global) snapshot();
        delete_range(a, b);
        cur = a <= n ? a : n;
        return 0;
    case 'j':
        if (!na) { a = cur; b = cur + 1; }
        if (check_range(a, b, 0)) return -1;
        if (a == b) return 0;
        if (!in_global) snapshot();
        {
            size_t tot = 0;
            for (int i = a; i <= b; i++) tot += strlen(L[i]);
            char *s = malloc(tot + 1); s[0] = '\0';
            for (int i = a; i <= b; i++) strcat(s, L[i]);
            delete_range(a + 1, b);
            free(L[a]); L[a] = s;
            cur = a; modified = 1;
        }
        return 0;
    case 'k': {
        p = skip_ws(p);
        if (*p < 'a' || *p > 'z') return err("Invalid mark character");
        if (check_range(b, b, 0)) return -1;
        marks[*p - 'a'] = b; return 0;
    }
    case 'm': case 't': {
        if (check_range(a, b, 0)) return -1;
        int dest;
        int r = parse_one_addr(&p, &dest);
        if (r < 0) return err("Invalid address");
        if (r == 0) dest = cur;
        if (dest < 0 || dest > n) return err("Invalid address");
        if (!in_global) snapshot();
        int k = b - a + 1;
        char **tmp = malloc((size_t)k * sizeof *tmp);
        if (c == 't') {
            for (int i = 0; i < k; i++) tmp[i] = strdup(L[a + i]);
            insert_after(dest, tmp, k);
            cur = dest + k;
        } else {
            if (dest >= a && dest < b) { free(tmp); return err("Invalid destination"); }
            for (int i = 0; i < k; i++) tmp[i] = L[a + i];
            memmove(&L[a], &L[b + 1], (size_t)(n - b) * sizeof *L);
            n -= k;
            if (dest > b) dest -= k;
            insert_after(dest, tmp, k);
            cur = dest + k;
            modified = 1;
        }
        free(tmp);
        return 0;
    }
    case 's':
        if (check_range(a, b, 0)) return -1;
        if (!in_global) snapshot();
        return do_subst(a, b, p);
    case 'g': case 'v': case 'G': case 'V':
        if (in_global) return err("Cannot nest global commands");
        if (!na) { a = 1; b = n; }
        if (check_range(a, b, 0)) return -1;
        snapshot();
        return run_global(a, b, p, c == 'v' || c == 'V', c == 'G' || c == 'V');
    case 'u': {
        if (na) return err("Unexpected address");
        if (!U) return err("Nothing to undo");
        /* swap buffer and snapshot */
        char **tl = L; int tn = n, tcap = cap, tc = cur;
        L = malloc((size_t)(un + 1) * sizeof *L); cap = un + 1;
        for (int i = 1; i <= un; i++) L[i] = U[i];
        n = un; cur = u_cur <= n ? u_cur : n;
        free(U);
        U = malloc((size_t)(tn + 1) * sizeof *U);
        for (int i = 1; i <= tn; i++) U[i] = tl[i];
        un = tn; u_cur = tc;
        free(tl); (void)tcap;
        modified = 1;
        return 0;
    }
    case 'z': {
        if (!na) a = cur + 1;
        else a = b;
        int k = last_z;
        p = skip_ws(p);
        if (isdigit((unsigned char)*p)) { k = atoi(p); last_z = k; }
        if (a < 1 || a > n) return err("Invalid address");
        int e = a + k - 1; if (e > n) e = n;
        for (int i = a; i <= e; i++) print_line(i, 'p');
        cur = e; return 0;
    }
    case 'h': printf("%s\n", last_err); return 0;
    case 'H': hmode = !hmode; if (hmode && last_err[0]) printf("%s\n", last_err); return 0;
    case 'P': prompt_on = !prompt_on; if (!prompt) prompt = "*"; return 0;
    case 'f':
        p = skip_ws(p);
        if (*p) snprintf(fname, sizeof fname, "%s", p);
        if (!fname[0]) return err("No current filename");
        printf("%s\n", fname); return 0;
    case 'e': case 'E': {
        if (na) return err("Unexpected address");
        if (c == 'e' && modified && !warned) { warned = 1; return err("Warning: buffer modified"); }
        p = skip_ws(p);
        char path[512];
        if (*p) snprintf(path, sizeof path, "%s", p); else snprintf(path, sizeof path, "%s", fname);
        if (!path[0]) return err("No current filename");
        for (int i = 1; i <= n; i++) free(L[i]);
        n = 0; cur = 0;
        for (int i = 0; i < 26; i++) marks[i] = 0;
        int k; long bytes = read_file_into(path, 0, &k);
        if (bytes < 0) { if (!quiet) printf("%s: No such file or directory\n", path); if (path[0] != '!') snprintf(fname, sizeof fname, "%s", path); return err("Cannot open input file"); }
        if (path[0] != '!') snprintf(fname, sizeof fname, "%s", path);
        if (!quiet) printf("%ld\n", bytes);
        cur = n; modified = 0; warned = 0;
        return 0;
    }
    case 'r': {
        if (!na) b = n;
        if (b < 0 || b > n) return err("Invalid address");
        p = skip_ws(p);
        char path[512];
        if (*p) snprintf(path, sizeof path, "%s", p); else snprintf(path, sizeof path, "%s", fname);
        if (!path[0]) return err("No current filename");
        if (!fname[0] && path[0] != '!') snprintf(fname, sizeof fname, "%s", path);
        if (!in_global) snapshot();
        int k; long bytes = read_file_into(path, b, &k);
        if (bytes < 0) return err("Cannot open input file");
        if (!quiet) printf("%ld\n", bytes);
        cur = b + k;
        return 0;
    }
    case 'w': case 'W': {
        int quit = 0;
        if (*p == 'q') { quit = 1; p++; }
        p = skip_ws(p);
        if (!na) { a = 1; b = n; }
        if (n > 0 && check_range(a, b, 0)) return -1;
        char path[512];
        if (*p) snprintf(path, sizeof path, "%s", p); else snprintf(path, sizeof path, "%s", fname);
        if (!path[0]) return err("No current filename");
        if (!fname[0] && path[0] != '!') snprintf(fname, sizeof fname, "%s", path);
        long bytes = write_file_from(path, a, b, c == 'W');
        if (bytes < 0) return err("Cannot open output file");
        if (!quiet) printf("%ld\n", bytes);
        if (!na && path[0] != '!') modified = 0;
        if (quit) exit(0);
        return 0;
    }
    case 'q':
        if (na) return err("Unexpected address");
        if (modified && !warned) { warned = 1; return err("Warning: buffer modified"); }
        exit(last_failed ? 1 : 0);
    case 'Q':
        if (na) return err("Unexpected address");
        exit(0);
    case '!': {
        if (na) return err("Unexpected address");
        int rc = system(p);
        (void)rc;
        printf("!\n");
        return 0;
    }
    default:
        return err("Unknown command");
    }
}

int main(int argc, char **argv) {
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--version")) { printf("GNU ed (tobyOS) 1.20\n"); return 0; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { printf("Usage: ed [-s] [-p PROMPT] [FILE]\n"); return 0; }
        if (!strcmp(a, "-s") || !strcmp(a, "--quiet") || !strcmp(a, "--silent")) { quiet = 1; continue; }
        if (!strcmp(a, "-p") && i + 1 < argc) { prompt = argv[++i]; prompt_on = 1; continue; }
        if (!strncmp(a, "-p", 2) && a[2]) { prompt = a + 2; prompt_on = 1; continue; }
        if (!strncmp(a, "--prompt=", 9)) { prompt = a + 9; prompt_on = 1; continue; }
        if (!strcmp(a, "-") || !strcmp(a, "-G") || !strcmp(a, "-l") || !strcmp(a, "-v") || !strcmp(a, "-r") || !strcmp(a, "-E")) continue;
        if (a[0] == '-' && a[1]) { fprintf(stderr, "ed: invalid option -- '%s'\n", a + 1); return 1; }
        file = a;
    }
    ensure(16);
    if (file) {
        snprintf(fname, sizeof fname, "%s", file);
        int k; long bytes = read_file_into(file, 0, &k);
        if (bytes < 0) { if (!quiet) printf("%s: No such file or directory\n", file); }
        else if (!quiet) printf("%ld\n", bytes);
        cur = n; modified = 0;
    }
    for (;;) {
        if (prompt_on && prompt) { fputs(prompt, stdout); fflush(stdout); }
        char *line = read_line(stdin);
        if (!line) {
            if (modified && !warned) { warned = 1; err("Warning: buffer modified"); return 1; }
            return last_failed ? 1 : 0;
        }
        last_failed = 0;
        exec_cmd(line, 0);
        fflush(stdout);
    }
}
