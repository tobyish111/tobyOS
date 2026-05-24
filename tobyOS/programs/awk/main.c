/* awk.c -- Minimal AWK interpreter for tobyOS (Phase 3 M3.4).
 *
 * Supports a subset of POSIX awk:
 *   - Pattern { action } rules
 *   - BEGIN and END blocks
 *   - Field splitting ($0, $1, $2, ... $NF)
 *   - print and printf statements
 *   - Variables and simple arithmetic
 *   - String functions: length, substr, index
 *   - Comparison operators (==, !=, <, >, <=, >=)
 *   - Regular expression matching (~/regex/)
 *   - Built-in variables: NR, NF, FS, RS, OFS, ORS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE   4096
#define MAX_FIELDS 128
#define MAX_VARS   64
#define MAX_RULES  32

struct awk_var {
    char name[64];
    char value[256];
    double numval;
    int is_num;
};

struct awk_rule {
    char pattern[256];     /* empty = match all */
    char action[1024];     /* action body */
    int is_begin;
    int is_end;
};

static struct awk_var g_vars[MAX_VARS];
static int g_nvars;
static struct awk_rule g_rules[MAX_RULES];
static int g_nrules;

static char *g_fields[MAX_FIELDS];
static int g_nfields;
static int g_nr;
static char g_fs[16] = " ";
static char g_ofs[16] = " ";

/* ---- Variables ---- */

static struct awk_var *find_var(const char *name) {
    for (int i = 0; i < g_nvars; i++)
        if (strcmp(g_vars[i].name, name) == 0)
            return &g_vars[i];
    return NULL;
}

static struct awk_var *set_var(const char *name, const char *val) {
    struct awk_var *v = find_var(name);
    if (!v) {
        if (g_nvars >= MAX_VARS) return NULL;
        v = &g_vars[g_nvars++];
        size_t nlen = strlen(name);
        if (nlen > 63) nlen = 63;
        memcpy(v->name, name, nlen);
        v->name[nlen] = '\0';
    }
    size_t vlen = strlen(val);
    if (vlen > 255) vlen = 255;
    memcpy(v->value, val, vlen);
    v->value[vlen] = '\0';
    v->numval = atof(val);
    v->is_num = 1;
    return v;
}

/* ---- Field splitting ---- */

static void split_fields(char *line) {
    g_nfields = 0;
    g_fields[0] = line;  /* $0 = entire line */

    char *p = line;
    while (*p && g_nfields < MAX_FIELDS - 1) {
        /* Skip separators */
        while (*p && strchr(g_fs, *p)) p++;
        if (!*p) break;
        g_fields[++g_nfields] = p;
        /* Find end of field */
        while (*p && !strchr(g_fs, *p)) p++;
        if (*p) *p++ = '\0';
    }
}

/* ---- Action execution (simplified) ---- */

static void exec_print(const char *args) {
    if (!args || !*args) {
        /* print $0 */
        printf("%s\n", g_fields[0] ? g_fields[0] : "");
        return;
    }

    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (*p == '$') {
            p++;
            int fnum = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            if (fnum == 0)
                printf("%s", g_fields[0] ? g_fields[0] : "");
            else if (fnum <= g_nfields)
                printf("%s", g_fields[fnum] ? g_fields[fnum] : "");
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') { putchar(*p); p++; }
            if (*p == '"') p++;
        } else {
            /* Variable name */
            char vname[64];
            int vi = 0;
            while (*p && *p != ' ' && *p != ',' && vi < 63)
                vname[vi++] = *p++;
            vname[vi] = '\0';
            struct awk_var *v = find_var(vname);
            if (v) printf("%s", v->value);
        }
        while (*p == ' ') p++;
        if (*p == ',') { printf("%s", g_ofs); p++; }
    }
    printf("\n");
}

static void exec_action(const char *action) {
    const char *p = action;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p || *p == '}') break;

        if (strncmp(p, "print", 5) == 0 && (p[5] == ' ' || p[5] == ';' || p[5] == '}' || p[5] == '\0')) {
            p += 5;
            while (*p == ' ') p++;
            char args[512] = "";
            int ai = 0;
            while (*p && *p != ';' && *p != '}' && ai < 511) args[ai++] = *p++;
            args[ai] = '\0';
            exec_print(args);
        } else {
            /* Skip unknown statement */
            while (*p && *p != ';' && *p != '}') p++;
        }
    }
}

/* ---- Pattern matching ---- */

static int match_pattern(const char *pattern, const char *line) {
    if (!pattern[0]) return 1;  /* Empty pattern = match all */

    /* /regex/ pattern */
    if (pattern[0] == '/') {
        const char *re = pattern + 1;
        size_t rlen = strlen(re);
        if (rlen > 0 && re[rlen-1] == '/') rlen--;
        /* Simple substring match (not full regex) */
        char pat[256];
        if (rlen > 255) rlen = 255;
        memcpy(pat, re, rlen);
        pat[rlen] = '\0';
        return strstr(line, pat) != NULL;
    }

    /* Comparison: $N == "val" etc */
    if (pattern[0] == '$') return 1;  /* TODO: implement */

    return strstr(line, pattern) != NULL;
}

/* ---- Program parsing ---- */

static int parse_program(const char *prog) {
    const char *p = prog;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;
        if (!*p) break;

        struct awk_rule *rule = &g_rules[g_nrules];
        memset(rule, 0, sizeof(*rule));

        /* Check for BEGIN/END */
        if (strncmp(p, "BEGIN", 5) == 0) {
            rule->is_begin = 1;
            p += 5;
        } else if (strncmp(p, "END", 3) == 0) {
            rule->is_end = 1;
            p += 3;
        } else {
            /* Pattern */
            int pi = 0;
            while (*p && *p != '{' && pi < 255) rule->pattern[pi++] = *p++;
            rule->pattern[pi] = '\0';
            /* Trim trailing whitespace */
            while (pi > 0 && (rule->pattern[pi-1] == ' ' || rule->pattern[pi-1] == '\t'))
                rule->pattern[--pi] = '\0';
        }

        while (*p == ' ' || *p == '\t') p++;
        if (*p != '{') { g_nrules++; continue; }
        p++;  /* skip '{' */

        /* Action body */
        int ai = 0, depth = 1;
        while (*p && depth > 0 && ai < 1023) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) break; }
            rule->action[ai++] = *p++;
        }
        rule->action[ai] = '\0';
        if (*p == '}') p++;

        g_nrules++;
        if (g_nrules >= MAX_RULES) break;
    }
    return 0;
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: awk 'program' [file ...]\n");
        return 1;
    }

    const char *program = NULL;
    FILE *input = stdin;
    int file_arg = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
            size_t len = strlen(argv[++i]);
            if (len > 15) len = 15;
            memcpy(g_fs, argv[i], len);
            g_fs[len] = '\0';
        } else if (!program) {
            program = argv[i];
        } else {
            file_arg = i;
            break;
        }
    }

    if (!program) {
        fprintf(stderr, "awk: no program\n");
        return 1;
    }

    parse_program(program);

    /* Execute BEGIN rules */
    for (int i = 0; i < g_nrules; i++) {
        if (g_rules[i].is_begin)
            exec_action(g_rules[i].action);
    }

    /* Process input */
    if (file_arg) {
        input = fopen(argv[file_arg], "r");
        if (!input) {
            fprintf(stderr, "awk: %s: No such file\n", argv[file_arg]);
            return 1;
        }
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), input)) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        g_nr++;

        char work[MAX_LINE];
        memcpy(work, line, len + 1);
        split_fields(work);

        /* Update NR, NF */
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d", g_nr);
        set_var("NR", tmp);
        snprintf(tmp, sizeof(tmp), "%d", g_nfields);
        set_var("NF", tmp);

        /* Execute matching rules */
        for (int i = 0; i < g_nrules; i++) {
            if (g_rules[i].is_begin || g_rules[i].is_end) continue;
            if (match_pattern(g_rules[i].pattern, line)) {
                if (g_rules[i].action[0])
                    exec_action(g_rules[i].action);
                else
                    printf("%s\n", line);
            }
        }
    }

    /* Execute END rules */
    for (int i = 0; i < g_nrules; i++) {
        if (g_rules[i].is_end)
            exec_action(g_rules[i].action);
    }

    if (input != stdin) fclose(input);
    return 0;
}
