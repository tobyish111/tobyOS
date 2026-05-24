/* make/main.c -- Minimal make utility for tobyOS.
 *
 * Supports a useful subset of POSIX make:
 *   - Target: prerequisite ... rules
 *   - Recipe lines (tab-indented)
 *   - Variable assignment: VAR = value, VAR := value
 *   - Variable expansion: $(VAR), $@, $<, $^
 *   - Pattern rules: %.o: %.c
 *   - Implicit rules: .c -> .o using $(CC) $(CFLAGS) -c
 *   - Dependency resolution with timestamp comparison
 *   - .PHONY targets
 *   - -f flag for alternate Makefile, -j stub, -B (unconditional)
 *   - Default target: first non-phony target in file
 *   - @ prefix to suppress command echo
 *   - - prefix to ignore command errors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_LINE    2048
#define MAX_RULES   256
#define MAX_DEPS    64
#define MAX_RECIPES 32
#define MAX_VARS    128
#define MAX_PHONY   64

struct make_var {
    char name[128];
    char value[1024];
};

struct make_rule {
    char target[256];
    char deps[MAX_DEPS][256];
    int ndeps;
    char recipes[MAX_RECIPES][MAX_LINE];
    int nrecipes;
    int is_pattern;
    char pattern_stem[256];
};

static struct make_var g_vars[MAX_VARS];
static int g_nvars;
static struct make_rule g_rules[MAX_RULES];
static int g_nrules;
static char *g_default_target;
static char g_phony[MAX_PHONY][256];
static int g_nphony;
static int g_force_build;

/* ---- Variables ---- */

static void set_var(const char *name, const char *value) {
    for (int i = 0; i < g_nvars; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            strncpy(g_vars[i].value, value, 1023);
            g_vars[i].value[1023] = '\0';
            return;
        }
    }
    if (g_nvars < MAX_VARS) {
        struct make_var *v = &g_vars[g_nvars++];
        strncpy(v->name, name, 127); v->name[127] = '\0';
        strncpy(v->value, value, 1023); v->value[1023] = '\0';
    }
}

static const char *get_var(const char *name) {
    for (int i = 0; i < g_nvars; i++)
        if (strcmp(g_vars[i].name, name) == 0)
            return g_vars[i].value;
    return "";
}

static void expand_vars_ctx(char *dst, const char *src, size_t cap,
                            const char *target, const char *first_dep,
                            const char *all_deps) {
    size_t di = 0;
    while (*src && di < cap - 1) {
        if (*src == '$') {
            src++;
            if (*src == '@' && target) {
                const char *v = target;
                while (*v && di < cap - 1) dst[di++] = *v++;
                src++;
            } else if (*src == '<' && first_dep) {
                const char *v = first_dep;
                while (*v && di < cap - 1) dst[di++] = *v++;
                src++;
            } else if (*src == '^' && all_deps) {
                const char *v = all_deps;
                while (*v && di < cap - 1) dst[di++] = *v++;
                src++;
            } else if (*src == '(') {
                src++;
                char vname[128];
                int vi = 0;
                while (*src && *src != ')' && vi < 127) vname[vi++] = *src++;
                vname[vi] = '\0';
                if (*src == ')') src++;
                const char *val = get_var(vname);
                while (*val && di < cap - 1) dst[di++] = *val++;
            } else if (*src == '$') {
                dst[di++] = '$';
                src++;
            } else {
                dst[di++] = '$';
            }
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static void expand_vars(char *dst, const char *src, size_t cap) {
    expand_vars_ctx(dst, src, cap, NULL, NULL, NULL);
}

/* ---- Phony check ---- */

static int is_phony(const char *target) {
    for (int i = 0; i < g_nphony; i++)
        if (strcmp(g_phony[i], target) == 0) return 1;
    return 0;
}

/* ---- File timestamps ---- */

static long get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* ---- Rule lookup ---- */

static struct make_rule *find_rule(const char *target) {
    for (int i = 0; i < g_nrules; i++)
        if (!g_rules[i].is_pattern && strcmp(g_rules[i].target, target) == 0)
            return &g_rules[i];
    return NULL;
}

static struct make_rule *find_pattern_rule(const char *target, char *stem, size_t stem_cap) {
    for (int i = 0; i < g_nrules; i++) {
        if (!g_rules[i].is_pattern) continue;
        const char *pct = strchr(g_rules[i].target, '%');
        if (!pct) continue;
        size_t prefix_len = (size_t)(pct - g_rules[i].target);
        const char *suffix = pct + 1;
        size_t suffix_len = strlen(suffix);
        size_t tgt_len = strlen(target);

        if (tgt_len < prefix_len + suffix_len) continue;
        if (strncmp(target, g_rules[i].target, prefix_len) != 0) continue;
        if (suffix_len > 0 && strcmp(target + tgt_len - suffix_len, suffix) != 0) continue;

        size_t slen = tgt_len - prefix_len - suffix_len;
        if (slen >= stem_cap) slen = stem_cap - 1;
        memcpy(stem, target + prefix_len, slen);
        stem[slen] = '\0';
        return &g_rules[i];
    }
    return NULL;
}

static void apply_stem(char *dst, const char *pattern, const char *stem, size_t cap) {
    size_t di = 0;
    while (*pattern && di < cap - 1) {
        if (*pattern == '%') {
            const char *s = stem;
            while (*s && di < cap - 1) dst[di++] = *s++;
            pattern++;
        } else {
            dst[di++] = *pattern++;
        }
    }
    dst[di] = '\0';
}

/* ---- Recipe execution ---- */

static int run_recipe(const char *cmd, const char *target,
                      const char *first_dep, const char *all_deps) {
    char expanded[MAX_LINE];
    expand_vars_ctx(expanded, cmd, sizeof(expanded), target, first_dep, all_deps);

    const char *run = expanded;
    int silent = 0, ignore_err = 0;
    while (*run == '@' || *run == '-') {
        if (*run == '@') silent = 1;
        if (*run == '-') ignore_err = 1;
        run++;
    }
    while (*run == ' ') run++;

    if (!silent) printf("%s\n", run);

    int status = system(run);
    if (status != 0 && !ignore_err) return status;
    return 0;
}

/* ---- Build target ---- */

static int build_target(const char *target);

static int needs_rebuild(const char *target, struct make_rule *rule) {
    if (g_force_build) return 1;
    if (is_phony(target)) return 1;
    if (!file_exists(target)) return 1;

    long tgt_time = get_mtime(target);
    for (int i = 0; i < rule->ndeps; i++) {
        long dep_time = get_mtime(rule->deps[i]);
        if (dep_time < 0) return 1;
        if (dep_time > tgt_time) return 1;
    }
    return 0;
}

static int build_target(const char *target) {
    struct make_rule *rule = find_rule(target);

    if (!rule) {
        char stem[256];
        struct make_rule *prule = find_pattern_rule(target, stem, sizeof(stem));
        if (prule) {
            /* Expand pattern rule deps with stem */
            static struct make_rule expanded;
            memset(&expanded, 0, sizeof(expanded));
            strncpy(expanded.target, target, 255);
            for (int i = 0; i < prule->ndeps; i++) {
                apply_stem(expanded.deps[i], prule->deps[i], stem, 256);
                expanded.ndeps++;
            }
            for (int i = 0; i < prule->nrecipes; i++) {
                strncpy(expanded.recipes[i], prule->recipes[i], MAX_LINE - 1);
                expanded.nrecipes++;
            }
            rule = &expanded;
        }
    }

    if (!rule) {
        /* Try implicit .c -> .o */
        size_t tlen = strlen(target);
        if (tlen > 2 && strcmp(target + tlen - 2, ".o") == 0) {
            char csrc[256];
            memcpy(csrc, target, tlen - 2);
            csrc[tlen - 2] = '\0';
            strcat(csrc, ".c");
            if (file_exists(csrc)) {
                static struct make_rule implicit;
                memset(&implicit, 0, sizeof(implicit));
                strncpy(implicit.target, target, 255);
                strncpy(implicit.deps[0], csrc, 255);
                implicit.ndeps = 1;
                snprintf(implicit.recipes[0], MAX_LINE,
                         "$(CC) $(CFLAGS) -c $< -o $@");
                implicit.nrecipes = 1;
                rule = &implicit;
            }
        }
    }

    if (!rule) {
        if (file_exists(target)) return 0;
        fprintf(stderr, "make: *** No rule to make target '%s'\n", target);
        return -1;
    }

    /* Build dependencies first */
    for (int i = 0; i < rule->ndeps; i++) {
        char dep_expanded[256];
        expand_vars(dep_expanded, rule->deps[i], sizeof(dep_expanded));
        strncpy(rule->deps[i], dep_expanded, 255);
        int rc = build_target(rule->deps[i]);
        if (rc != 0) return rc;
    }

    if (!needs_rebuild(target, rule)) return 0;

    /* Build all_deps string for $^ */
    char all_deps[MAX_LINE];
    all_deps[0] = '\0';
    for (int i = 0; i < rule->ndeps; i++) {
        if (i > 0) strcat(all_deps, " ");
        strncat(all_deps, rule->deps[i], MAX_LINE - strlen(all_deps) - 1);
    }

    const char *first_dep = rule->ndeps > 0 ? rule->deps[0] : "";

    for (int i = 0; i < rule->nrecipes; i++) {
        int rc = run_recipe(rule->recipes[i], target, first_dep, all_deps);
        if (rc != 0) {
            fprintf(stderr, "make: *** [%s] Error %d\n", target, rc);
            return rc;
        }
    }
    return 0;
}

/* ---- Makefile parsing ---- */

static void strip_trailing(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t')) s[--len] = '\0';
}

static int parse_makefile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[MAX_LINE];
    struct make_rule *current = NULL;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[--len] = '\0';

        /* Line continuation */
        while (len > 0 && line[len-1] == '\\') {
            line[len-1] = ' ';
            if (!fgets(line + len, (int)(sizeof(line) - len), f)) break;
            len = strlen(line);
            if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
            if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
        }

        if (line[0] == '#' || len == 0) continue;

        /* Recipe line (tab-indented) */
        if (line[0] == '\t' && current) {
            if (current->nrecipes < MAX_RECIPES) {
                char *src = line + 1;
                strncpy(current->recipes[current->nrecipes], src, MAX_LINE - 1);
                current->recipes[current->nrecipes][MAX_LINE - 1] = '\0';
                strip_trailing(current->recipes[current->nrecipes]);
                current->nrecipes++;
            }
            continue;
        }

        /* .PHONY handling */
        if (strncmp(line, ".PHONY", 6) == 0) {
            char *p = strchr(line, ':');
            if (p) {
                p++;
                while (*p) {
                    while (*p == ' ' || *p == '\t') p++;
                    if (!*p) break;
                    char *end = p;
                    while (*end && *end != ' ' && *end != '\t') end++;
                    if (g_nphony < MAX_PHONY) {
                        size_t n = (size_t)(end - p);
                        if (n > 255) n = 255;
                        memcpy(g_phony[g_nphony], p, n);
                        g_phony[g_nphony][n] = '\0';
                        g_nphony++;
                    }
                    p = end;
                }
            }
            current = NULL;
            continue;
        }

        /* Variable assignment */
        char *eq = strstr(line, "=");
        char *colon = strchr(line, ':');

        /* Check for := or = (but not in a rule target) */
        if (eq && (!colon || eq < colon)) {
            char *name_end = eq;
            int is_immediate = 0;
            if (name_end > line && name_end[-1] == ':') {
                name_end--;
                is_immediate = 1;
            }
            (void)is_immediate;
            while (name_end > line && name_end[-1] == ' ') name_end--;
            char saved = *name_end;
            *name_end = '\0';

            char *val = eq + 1;
            while (*val == ' ' || *val == '\t') val++;
            strip_trailing(val);

            char name_buf[128];
            strncpy(name_buf, line, 127); name_buf[127] = '\0';
            *name_end = saved;

            char val_expanded[1024];
            expand_vars(val_expanded, val, sizeof(val_expanded));
            set_var(name_buf, val_expanded);
            current = NULL;
            continue;
        }

        /* Rule: target: deps */
        if (colon && colon[1] != '=') {
            if (g_nrules >= MAX_RULES) continue;
            current = &g_rules[g_nrules++];
            memset(current, 0, sizeof(*current));

            size_t tlen = (size_t)(colon - line);
            while (tlen > 0 && (line[tlen-1] == ' ' || line[tlen-1] == '\t')) tlen--;
            if (tlen > 255) tlen = 255;
            memcpy(current->target, line, tlen);
            current->target[tlen] = '\0';

            if (strchr(current->target, '%')) current->is_pattern = 1;

            if (!g_default_target && !current->is_pattern &&
                current->target[0] != '.')
                g_default_target = current->target;

            char *dep_str = colon + 1;
            while (*dep_str == ' ' || *dep_str == '\t') dep_str++;
            char *tok = dep_str;
            while (*tok && current->ndeps < MAX_DEPS) {
                while (*tok == ' ' || *tok == '\t') tok++;
                if (!*tok) break;
                char *end = tok;
                while (*end && *end != ' ' && *end != '\t') end++;
                size_t dlen = (size_t)(end - tok);
                if (dlen > 255) dlen = 255;
                memcpy(current->deps[current->ndeps], tok, dlen);
                current->deps[current->ndeps][dlen] = '\0';
                current->ndeps++;
                tok = end;
            }
        }
    }

    fclose(f);
    return 0;
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    const char *makefile = NULL;
    const char *targets[32];
    int ntargets = 0;

    set_var("CC", "cc");
    set_var("CFLAGS", "");
    set_var("LDFLAGS", "");
    set_var("AR", "ar");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            makefile = argv[++i];
        } else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            i++; /* stub: parse but ignore parallelism */
        } else if (strcmp(argv[i], "-B") == 0) {
            g_force_build = 1;
        } else if (argv[i][0] != '-') {
            /* Check for VAR=VALUE on command line */
            char *eq = strchr(argv[i], '=');
            if (eq) {
                char name[128];
                size_t nlen = (size_t)(eq - argv[i]);
                if (nlen > 127) nlen = 127;
                memcpy(name, argv[i], nlen);
                name[nlen] = '\0';
                set_var(name, eq + 1);
            } else if (ntargets < 32) {
                targets[ntargets++] = argv[i];
            }
        }
    }

    if (makefile) {
        if (parse_makefile(makefile) != 0) {
            fprintf(stderr, "make: %s: No such file or directory\n", makefile);
            return 2;
        }
    } else {
        if (parse_makefile("Makefile") != 0) {
            if (parse_makefile("makefile") != 0) {
                fprintf(stderr, "make: *** No makefile found\n");
                return 2;
            }
        }
    }

    if (ntargets == 0) {
        if (!g_default_target) {
            fprintf(stderr, "make: *** No targets\n");
            return 2;
        }
        targets[0] = g_default_target;
        ntargets = 1;
    }

    for (int i = 0; i < ntargets; i++) {
        int rc = build_target(targets[i]);
        if (rc != 0) return 2;
    }
    return 0;
}
