/* expr -- evaluate an expression.
 *
 *     expr operand [operator operand]...
 *
 * POSIX grammar, lowest precedence first:
 *
 *     |  &  { = > >= < <= != }  { + - }  { * / % }  : (match)
 *
 * Values are integers when they look like integers and strings
 * otherwise; the comparison operators compare numerically when both
 * sides are integers and as strings when either is not.
 *
 * Exit status is 0 if the result is neither null nor 0, 1 if it is,
 * 2 for an invalid expression -- so `expr` is usable in `if`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

struct val {
    long  num;
    char *str;          /* non-NULL when the value is a string */
};

static char **g_av;
static int    g_ac, g_pos;

static const char *peek(void) { return g_pos < g_ac ? g_av[g_pos] : 0; }
static const char *next(void) { return g_pos < g_ac ? g_av[g_pos++] : 0; }

static int is_int(const char *s, long *out) {
    if (!s || !*s) return 0;
    const char *p = s;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (!*p) return 0;
    long v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

static struct val mkstr(const char *s) {
    struct val v;
    v.str = estrdup(s);
    v.num = 0;
    (void)is_int(s, &v.num);
    return v;
}

static struct val mknum(long n) {
    struct val v;
    v.str = 0;
    v.num = n;
    return v;
}

static long tonum(struct val v) {
    long out;
    if (!v.str) return v.num;
    if (!is_int(v.str, &out)) eprintf("non-integer argument");
    return out;
}

static const char *tostr(struct val v, char *buf, size_t cap) {
    if (v.str) return v.str;
    snprintf(buf, cap, "%ld", v.num);
    return buf;
}

static int val_is_int(struct val v) {
    long out;
    return v.str ? is_int(v.str, &out) : 1;
}

static struct val expr_or(void);

/* `:` -- anchored BRE match. Without \( \) the result is the number of
 * characters matched; with them, the first captured group. This is a
 * plain backtracking matcher over the subset expr actually needs. */
static const char *bre_match(const char *pat, const char *str,
                             const char **cap_start, const char **cap_end);

static int bre_here(const char *pat, const char *str, const char **ce,
                    const char **cs, const char **cap_s, const char **cap_e) {
    if (!*pat) { *ce = str; return 1; }
    if (pat[0] == '\\' && pat[1] == '(') {
        const char *inner_s = str;
        /* Find the matching \) at this level. */
        const char *q = pat + 2;
        int depth = 1;
        while (*q && depth) {
            if (q[0] == '\\' && q[1] == '(') { depth++; q += 2; continue; }
            if (q[0] == '\\' && q[1] == ')') { depth--; q += 2; continue; }
            q++;
        }
        /* Try the longest match of the group first. */
        for (const char *end = str + strlen(str); end >= str; end--) {
            char sub[256];
            size_t n = (size_t)(q - 2 - (pat + 2));
            if (n + 1 > sizeof(sub)) return 0;
            memcpy(sub, pat + 2, n);
            sub[n] = '\0';
            char tail[256];
            size_t tn = (size_t)(end - str);
            if (tn + 1 > sizeof(tail)) continue;
            memcpy(tail, str, tn);
            tail[tn] = '\0';
            const char *ge = 0;
            if (bre_here(sub, tail, &ge, cs, cap_s, cap_e) && ge && !*ge) {
                const char *rest_end = 0;
                if (bre_here(q, end, &rest_end, cs, cap_s, cap_e)) {
                    *cap_s = inner_s;
                    *cap_e = end;
                    *ce = rest_end;
                    return 1;
                }
            }
        }
        return 0;
    }
    /* One atom, plus an optional `*`. */
    const char *after = pat;
    int cls_neg = 0;
    const char *cls_s = 0, *cls_e = 0;
    char lit = 0;
    int any = 0;
    if (pat[0] == '\\' && pat[1]) { lit = pat[1]; after = pat + 2; }
    else if (pat[0] == '.') { any = 1; after = pat + 1; }
    else if (pat[0] == '[') {
        const char *q = pat + 1;
        if (*q == '^') { cls_neg = 1; q++; }
        cls_s = q;
        if (*q == ']') q++;
        while (*q && *q != ']') q++;
        if (!*q) return 0;
        cls_e = q;
        after = q + 1;
    } else { lit = pat[0]; after = pat + 1; }

    int star = (*after == '*');
    const char *rest = star ? after + 1 : after;

    int count = 0;
    const char *s = str;
    for (;;) {
        int ok;
        if (any) ok = (*s != '\0');
        else if (cls_s) {
            ok = 0;
            if (*s) {
                for (const char *c = cls_s; c < cls_e; c++) {
                    if (c + 2 < cls_e && c[1] == '-') {
                        if (*s >= c[0] && *s <= c[2]) { ok = 1; break; }
                        c += 2;
                    } else if (*c == *s) { ok = 1; break; }
                }
                if (cls_neg) ok = !ok;
            }
        } else ok = (*s == lit && *s);
        if (!ok) break;
        s++;
        count++;
        if (!star) break;
    }
    if (!star && count == 0) return 0;

    for (int take = count; take >= (star ? 0 : count); take--) {
        const char *e = 0;
        if (bre_here(rest, str + take, &e, cs, cap_s, cap_e)) { *ce = e; return 1; }
        if (!star) break;
    }
    return 0;
}

static const char *bre_match(const char *pat, const char *str,
                             const char **cap_start, const char **cap_end) {
    const char *end = 0;
    *cap_start = *cap_end = 0;
    if (pat[0] == '^') pat++;
    if (bre_here(pat, str, &end, 0, cap_start, cap_end)) return end;
    return 0;
}

static struct val expr_match(void) {
    const char *a = next();
    if (!a) eprintf("syntax error");
    struct val left = mkstr(a);
    for (;;) {
        const char *op = peek();
        if (!op || strcmp(op, ":") != 0) break;
        next();
        const char *b = next();
        if (!b) eprintf("syntax error");
        char lb[64];
        const char *s = tostr(left, lb, sizeof(lb));
        const char *cs = 0, *ce = 0;
        const char *end = bre_match(b, s, &cs, &ce);
        if (strstr(b, "\\(")) {
            if (end && cs) {
                char cap[256];
                size_t n = (size_t)(ce - cs);
                if (n + 1 > sizeof(cap)) n = sizeof(cap) - 1;
                memcpy(cap, cs, n);
                cap[n] = '\0';
                left = mkstr(cap);
            } else {
                left = mkstr("");
            }
        } else {
            left = mknum(end ? (long)(end - s) : 0);
        }
    }
    return left;
}

static struct val expr_mul(void) {
    struct val left = expr_match();
    for (;;) {
        const char *op = peek();
        if (!op || (strcmp(op, "*") && strcmp(op, "/") && strcmp(op, "%")))
            break;
        next();
        struct val right = expr_match();
        long a = tonum(left), b = tonum(right);
        if (op[0] != '*' && b == 0) eprintf("division by zero");
        left = mknum(op[0] == '*' ? a * b : op[0] == '/' ? a / b : a % b);
    }
    return left;
}

static struct val expr_add(void) {
    struct val left = expr_mul();
    for (;;) {
        const char *op = peek();
        if (!op || (strcmp(op, "+") && strcmp(op, "-"))) break;
        next();
        struct val right = expr_mul();
        long a = tonum(left), b = tonum(right);
        left = mknum(op[0] == '+' ? a + b : a - b);
    }
    return left;
}

static struct val expr_cmp(void) {
    struct val left = expr_add();
    for (;;) {
        const char *op = peek();
        if (!op) break;
        if (strcmp(op, "=") && strcmp(op, "!=") && strcmp(op, "<") &&
            strcmp(op, "<=") && strcmp(op, ">") && strcmp(op, ">=")) break;
        next();
        struct val right = expr_add();
        int r;
        if (val_is_int(left) && val_is_int(right)) {
            long a = tonum(left), b = tonum(right);
            r = a < b ? -1 : a > b ? 1 : 0;
        } else {
            char lb[64], rb[64];
            r = strcmp(tostr(left, lb, sizeof(lb)), tostr(right, rb, sizeof(rb)));
        }
        int truth =
            !strcmp(op, "=")  ? r == 0 :
            !strcmp(op, "!=") ? r != 0 :
            !strcmp(op, "<")  ? r <  0 :
            !strcmp(op, "<=") ? r <= 0 :
            !strcmp(op, ">")  ? r >  0 : r >= 0;
        left = mknum(truth ? 1 : 0);
    }
    return left;
}

static int val_true(struct val v) {
    if (v.str) return v.str[0] != '\0' && strcmp(v.str, "0") != 0;
    return v.num != 0;
}

static struct val expr_and(void) {
    struct val left = expr_cmp();
    while (peek() && strcmp(peek(), "&") == 0) {
        next();
        struct val right = expr_cmp();
        if (!val_true(left) || !val_true(right)) left = mknum(0);
    }
    return left;
}

static struct val expr_or(void) {
    struct val left = expr_and();
    while (peek() && strcmp(peek(), "|") == 0) {
        next();
        struct val right = expr_and();
        if (!val_true(left)) left = right;
    }
    return left;
}

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "expr");
    if (argc < 2) eprintf("usage: %s expression", argv0);

    g_av = argv;
    g_ac = argc;
    g_pos = 1;

    struct val v = expr_or();
    if (g_pos != g_ac) eprintf("syntax error");

    char buf[64];
    const char *s = tostr(v, buf, sizeof(buf));
    printf("%s\n", s);
    return val_true(v) ? 0 : 1;
}
