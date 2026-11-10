/* prharness -- the tree and printer in src/shell_bashsyn.inc, on the host.
 * For each FILE (a corpus script wrapped as `f() { ... }; declare -f f`)
 * parse the first command with the tree on, and print what `declare -f f`
 * prints: bash's own reconstruction, the oracle in oracle/NAME.out.
 *
 *   prharness OUTDIR FILE...    writes OUTDIR/NAME.out
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define BSYN_ALLOC(n) malloc(n)
#define BSYN_FREE(p)  free(p)
#include "../../../src/shell_bashsyn.inc"

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
    if (argc < 3) return 2;
    const char *outdir = argv[1];
    struct bsyn_env env;
    memset(&env, 0, sizeof env);
    env.max_depth = 2000;
    env.build = true;
    env.c_locale = true;          /* the oracle ran under LC_ALL=C */
    for (int i = 2; i < argc; i++) {
        size_t len = 0;
        char *buf = read_file(argv[i], &len);
        const char *name = strrchr(argv[i], '/');
        name = name ? name + 1 : argv[i];
        char op[1024];
        snprintf(op, sizeof op, "%s/%s.out", outdir, name);
        FILE *o = fopen(op, "wb");
        if (!buf || !o) { if (o) fclose(o); free(buf); continue; }
        struct bsyn *h = bsyn_open(buf, len, 0, 0, &env);
        struct bsyn_result res;
        if (getenv("PR_ALL")) {
            /* every command, printed as make_command_string does */
            for (int guard = 0; guard < 100000; guard++) {
                int kk = bsyn_next(h, &res);
                if (kk == BSYN_OK && res.tree && getenv("PR_DECL")) {
                    /* every definition, as declare -f prints it */
                    if (res.tree->type == BN_FUNC) {
                        struct bsp p;
                        bsp_init(&p, h);
                        bsp_named_function(&p, res.tree->name, res.tree->a, true);
                        if (p.out.p) fwrite(p.out.p, 1, p.out.len, o);
                        fputc('\n', o);
                        bsp_free(&p);
                    }
                } else if (kk == BSYN_OK && res.tree) {
                    struct bsp p;
                    bsp_init(&p, h);
                    bsp_command_string(&p, res.tree);
                    if (p.out.p) fwrite(p.out.p, 1, p.out.len, o);
                    fputc('\n', o);
                    bsp_free(&p);
                }
                bsyn_result_free(&res);
                if (kk == BSYN_ERROR || kk == BSYN_EOF || kk == BSYN_SKIP) break;
            }
            bsyn_close(h);
            fclose(o);
            free(buf);
            continue;
        }
        int k = bsyn_next(h, &res);
        if (k == BSYN_OK && res.tree && res.tree->type == BN_FUNC) {
            struct bsp p;
            bsp_init(&p, h);
            bsp_named_function(&p, res.tree->name, res.tree->a, true);
            if (p.out.p) fwrite(p.out.p, 1, p.out.len, o);
            fputc('\n', o);
            bsp_free(&p);
        }
        bsyn_result_free(&res);
        bsyn_close(h);
        fclose(o);
        free(buf);
    }
    return 0;
}
