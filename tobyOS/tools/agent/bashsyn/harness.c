/* The bash-grammar check (src/shell_bashsyn.inc) on the host, run the way
 * `bash -n FILE` runs: one command at a time until the input ends or a
 * syntax error stops it. One line per file:
 *
 *     NAME<TAB>STATUS<TAB>message|message|...
 *
 * the same shape tools/agent/bashsyn/oracle.sh writes from bash 5.2.15, so
 * the two compare with diff.
 *
 *   harness [-O extglob] [-a NAME=VALUE]... [-s] FILE...
 *     -O extglob  as `bash -O extglob -n`
 *     -a          an alias (implies expand_aliases)
 *     -s          read FILE as eval text rather than a file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define BSYN_ALLOC(n) malloc(n)
#define BSYN_FREE(p)  free(p)
#include "../../../src/shell_bashsyn.inc"

static char *g_anames[64], *g_avals[64];
static int g_nalias;

static const char *alias_lookup(void *ctx, const char *name, bool *expand_next) {
    (void)ctx;
    for (int i = 0; i < g_nalias; i++) {
        if (strcmp(g_anames[i], name) == 0) {
            size_t n = strlen(g_avals[i]);
            *expand_next = n && (g_avals[i][n - 1] == ' ' || g_avals[i][n - 1] == '\t');
            return g_avals[i];
        }
    }
    return 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap + 1);
    size_t r;
    while ((r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { cap *= 2; b = realloc(b, cap + 1); }
    }
    fclose(f);
    b[n] = '\0';
    *len = n;
    return b;
}

int main(int argc, char **argv) {
    struct bsyn_env env;
    memset(&env, 0, sizeof env);
    env.max_depth = 2000;
    env.alias = alias_lookup;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0 && i + 1 < argc) {
            if (strcmp(argv[++i], "extglob") == 0) env.extglob = true;
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            char *d = argv[++i];
            char *eq = strchr(d, '=');
            if (!eq || g_nalias >= 64) continue;
            *eq = '\0';
            g_anames[g_nalias] = d;
            g_avals[g_nalias] = eq + 1;
            g_nalias++;
            env.expand_aliases = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            env.is_string = true;
        } else {
            break;
        }
    }
    for (; i < argc; i++) {
        size_t len;
        char *buf = read_file(argv[i], &len);
        const char *name = argv[i];
        const char *slash = strrchr(name, '/');
        if (slash) name = slash + 1;
        if (!buf) { printf("%s\t?\tunreadable\n", name); continue; }
        int status = 0;
        int first = 1;
        printf("%s\t", name);
        char out[65536];
        size_t on = 0;
        out[0] = '\0';
        struct bsyn *h = bsyn_open(buf, len, 0, 0, &env);
        size_t lastpos = (size_t)-1;
        int stuck = 0;
        for (;;) {
            struct bsyn_result res;
            int k = bsyn_next(h, &res);
            for (int m = 0; m < res.nmsg; m++) {
                int w = snprintf(out + on, sizeof out - on, "%s%s: line %lu: %s%s",
                                 first ? "" : "|", name, res.msg[m].line,
                                 res.msg[m].kind == BSYN_MSG_WARNING ? "warning: " : "",
                                 res.msg[m].text);
                if (w > 0 && on + (size_t)w < sizeof out) {
                    /* the oracle's output went through tr '\n' '|' */
                    for (size_t z = on; z < on + (size_t)w; z++) if (out[z] == '\n') out[z] = '|';
                    on += (size_t)w;
                }
                first = 0;
            }
            if (res.status_set) status = res.status;
            else if (res.status_ifzero && status == 0) status = res.status_ifzero;
            bsyn_result_free(&res);
            if (k == BSYN_SKIP) {
                int w = snprintf(out + on, sizeof out - on, "%sSKIP", first ? "" : "|");
                if (w > 0) on += (size_t)w;
                break;
            }
            if (k == BSYN_ERROR || k == BSYN_EOF) break;
            if (res.end == lastpos && ++stuck > 1000) {
                int w = snprintf(out + on, sizeof out - on, "%sNO-PROGRESS", first ? "" : "|");
                if (w > 0) on += (size_t)w;
                break;
            }
            if (res.end != lastpos) stuck = 0;
            lastpos = res.end;
        }
        bsyn_close(h);
        printf("%d\t%s\n", status, out);
        free(buf);
    }
    return 0;
}
