/* The arithmetic evaluator (src/shell_arith.inc) on the host, against bash.
 * Reads a case file -- one case per line, "SETUP<TAB>EXPR", SETUP being
 * space-separated `name=value`, `name[i]=value` (indexed element),
 * `name{key}=value` (associative element) and `-u` (set -u) -- and prints
 * for each what gen.py's bash script prints for it:
 *
 *     ==== N
 *     MSG                       (an error, as bash words it after "NAME: line N: ")
 *     val=V                     (when it evaluated)
 *     st=S
 *     declare -- x="5"          (declare -p of a fixed list of names)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SARITH_ALLOC(n) malloc(n)
#define SARITH_INT long long
#define SARITH_FREE(p)  free(p)
#include "../../../src/shell_arith.inc"

/* ---- a variable store with bash's array rules -------------------------- */

#define MAXV 64
#define MAXE 64
struct elem { char key[64]; long idx; char *val; };
struct var {
    char name[32];
    int kind;                 /* 0 scalar, 1 indexed, 2 assoc */
    char *val;                /* scalar */
    struct elem e[MAXE];
    int ne;
};
static struct var V[MAXV];
static int NV;

static struct var *vfind(const char *n) {
    for (int i = 0; i < NV; i++) if (strcmp(V[i].name, n) == 0) return &V[i];
    return 0;
}

static struct var *vmake(const char *n, int kind) {
    struct var *v = vfind(n);
    if (v) return v;
    v = &V[NV++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", n);
    v->kind = kind;
    return v;
}

static struct elem *efind_idx(struct var *v, long i) {
    for (int k = 0; k < v->ne; k++) if (v->e[k].idx == i) return &v->e[k];
    return 0;
}
static struct elem *efind_key(struct var *v, const char *key) {
    for (int k = 0; k < v->ne; k++) if (strcmp(v->e[k].key, key) == 0) return &v->e[k];
    return 0;
}
static long vmaxidx(struct var *v) {
    long m = -1;
    for (int k = 0; k < v->ne; k++) if (v->e[k].idx > m) m = v->e[k].idx;
    return m;
}
static void eset_idx(struct var *v, long i, const char *val) {
    struct elem *e = efind_idx(v, i);
    if (!e) { e = &v->e[v->ne++]; e->idx = i; e->key[0] = 0; e->val = 0; }
    free(e->val);
    e->val = strdup(val);
}
static void eset_key(struct var *v, const char *key, const char *val) {
    struct elem *e = efind_key(v, key);
    if (!e) { e = &v->e[v->ne++]; snprintf(e->key, sizeof e->key, "%s", key); e->idx = 0; e->val = 0; }
    free(e->val);
    e->val = strdup(val);
}

static struct sarith_env ENV;
static int g_suberr;

/* an indexed subscript is arithmetic; a negative one counts from the end */
/* 1: *out is the index; -2: a negative subscript past the start -- bash
 * says "NAME: bad array subscript" and the read is of nothing; 0: the
 * subscript failed; -3: it met an unbound variable under set -u (fatal) */
static int eval_index(struct var *v, const char *sub, long *out) {
    sa_int r;
    char *msg = 0;
    int k = sarith_eval(sub, &ENV, &r, &msg);
    if (k != SARITH_OK) {
        if (k == SARITH_ERROR && msg) printf("%s\n", msg);
        else if (k == SARITH_UNBOUND && msg) printf("%s: unbound variable\n", msg);
        free(msg);
        g_suberr = k;
        return k == SARITH_UNBOUND ? -3 : 0;
    }
    if (r < 0) {                /* only an indexed array counts back */
        if (v && v->kind == 1) r = vmaxidx(v) + 1 + r;
        if (r < 0) { *out = (long)r; return -2; }
    }
    *out = (long)r;
    return 1;
}

static int cb_get(void *ctx, const char *name, const char *sub, char **val,
                  bool *set, long *ind) {
    (void)ctx;
    *val = 0; *set = false; *ind = -1;
    struct var *v = vfind(name);
    *set = v != 0;
    if (!v) {
        /* under set -u bash reports the unset array before its subscript */
        if (!sub || ENV.nounset) return SARITH_CB_OK;
        /* bash evaluates the subscript of an unset array too (side
         * effects), and calls a negative one bad */
        long i;
        int r = eval_index(0, sub, &i);
        if (r == 0) return SARITH_CB_FAIL;
        if (r == -3) return SARITH_CB_FATAL;
        if (r == -2) { printf("%s: bad array subscript\n", name); return SARITH_CB_OK; }
        *ind = i;
        return SARITH_CB_OK;
    }
    if (!sub) {
        if (v->kind == 0) { *val = v->val ? strdup(v->val) : 0; return SARITH_CB_OK; }
        if (v->kind == 1) { struct elem *e = efind_idx(v, 0); *val = e ? strdup(e->val) : 0; return SARITH_CB_OK; }
        struct elem *e = efind_key(v, "0");
        *val = e ? strdup(e->val) : 0;
        return SARITH_CB_OK;
    }
    if (v->kind == 2) {
        struct elem *e = efind_key(v, sub);
        *val = e ? strdup(e->val) : 0;
        return SARITH_CB_OK;
    }
    long i;
    int r = eval_index(v, sub, &i);
    if (r == 0) return SARITH_CB_FAIL;
    if (r == -3) return SARITH_CB_FATAL;
    if (r == -2) { printf("%s: bad array subscript\n", v->name); return SARITH_CB_OK; }
    *ind = i;
    if (v->kind == 0) { *val = (i == 0 && v->val) ? strdup(v->val) : 0; return SARITH_CB_OK; }
    struct elem *e = efind_idx(v, i);
    *val = e ? strdup(e->val) : 0;
    return SARITH_CB_OK;
}

static int cb_set(void *ctx, const char *name, const char *sub, long ind,
                  const char *val) {
    (void)ctx;
    struct var *v = vfind(name);
    if (!sub) {
        if (!v) v = vmake(name, 0);
        if (v->kind == 0) { free(v->val); v->val = strdup(val); }
        else if (v->kind == 1) eset_idx(v, 0, val);
        else eset_key(v, "0", val);
        return SARITH_CB_OK;
    }
    if (v && v->kind == 2) { eset_key(v, sub, val); return SARITH_CB_OK; }
    long i = ind;
    if (i < 0) {
        int r = eval_index(v, sub, &i);
        if (r == -3) return SARITH_CB_FATAL;
        if (r == 0) return SARITH_CB_FAIL;
        if (r == -2) {          /* "a[SUB]: bad array subscript"; not assigned */
            printf("%s[%s]: bad array subscript\n", name, sub);
            return SARITH_CB_OK;
        }
    }
    if (!v) v = vmake(name, 1);
    if (v->kind == 0) {         /* a scalar becomes element 0 */
        char *old = v->val;
        v->kind = 1;
        v->val = 0;
        if (old) { eset_idx(v, 0, old); free(old); }
    }
    eset_idx(v, i, val);
    return SARITH_CB_OK;
}

/* ---- declare -p, bash's shape ------------------------------------------ */

static int cmp_elem(const void *a, const void *b) {
    const struct elem *x = a, *y = b;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

static void dump(const char *n) {
    struct var *v = vfind(n);
    if (!v) return;
    if (v->kind == 0) { printf("declare -- %s=\"%s\"\n", n, v->val ? v->val : ""); return; }
    if (v->kind == 1) {
        qsort(v->e, (size_t)v->ne, sizeof v->e[0], cmp_elem);
        printf("declare -a %s=(", n);
        for (int k = 0; k < v->ne; k++)
            printf("%s[%ld]=\"%s\"", k ? " " : "", v->e[k].idx, v->e[k].val);
        printf(")\n");
        return;
    }
    printf("declare -A %s=(", n);
    for (int k = 0; k < v->ne; k++) printf("[%s]=\"%s\" ", v->e[k].key, v->e[k].val);
    printf(")\n");
}

int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    if (!f) return 1;
    char line[4096];
    int n = 0;
    ENV.get = cb_get;
    ENV.set = cb_set;
    ENV.stack_ok = 0;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *setup = line, *expr = tab + 1;
        n++;
        for (int i = 0; i < NV; i++) {
            free(V[i].val);
            for (int k = 0; k < V[i].ne; k++) free(V[i].e[k].val);
        }
        NV = 0;
        ENV.nounset = false;
        printf("==== %d\n", n);
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", setup);
        for (char *t = strtok(buf, " "); t; t = strtok(0, " ")) {
            if (strcmp(t, "-u") == 0) { ENV.nounset = true; continue; }
            char *eq = strchr(t, '=');
            if (!eq) continue;
            *eq = 0;
            char *val = eq + 1;
            /* `\s` is a space (setup tokens are split on spaces) */
            for (char *r = val, *w = val;; r++) {
                if (r[0] == '\\' && r[1] == 's') { *w++ = ' '; r++; continue; }
                *w++ = *r;
                if (!*r) break;
            }
            char *lb = strchr(t, '['), *lc = strchr(t, '{');
            if (lb) {
                *lb = 0;
                struct var *v = vmake(t, 1);
                eset_idx(v, atol(lb + 1), val);
            } else if (lc) {
                *lc = 0;
                char *rc = strchr(lc + 1, '}');
                if (rc) *rc = 0;
                struct var *v = vmake(t, 2);
                eset_key(v, lc + 1, val);
            } else {
                struct var *v = vmake(t, 0);
                v->val = strdup(val);
            }
        }
        sa_int r;
        char *msg = 0;
        g_suberr = 0;
        char padded[4200];
        snprintf(padded, sizeof padded, " %s ", expr);
        int k = sarith_eval(padded, &ENV, &r, &msg);
        int st = 0;
        if (k == SARITH_OK) {
            printf("val=%lld\n", r);
        } else if (k == SARITH_UNBOUND) {
            if (msg) printf("%s: unbound variable\n", msg);
            free(msg);
            continue;           /* bash's script ends there */
        } else {
            if (k == SARITH_ERROR && msg) printf("%s\n", msg);
            st = 1;
        }
        free(msg);
        printf("st=%d\n", st);
        const char *names[] = {"a","b","c","i","j","k","m","n","x","y","z","s","t","u","v","w",0};
        for (int q = 0; names[q]; q++) dump(names[q]);
    }
    return 0;
}
