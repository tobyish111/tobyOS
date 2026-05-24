/* sed.c -- Minimal stream editor for tobyOS (Phase 3 M3.4).
 *
 * Supports a subset of POSIX sed:
 *   - s/pattern/replacement/[g] - substitution
 *   - d - delete line
 *   - p - print line
 *   - q - quit
 *   - Address types: line number, $ (last line), /regex/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 4096
#define MAX_CMDS 64

enum sed_cmd_type {
    SED_SUBST,
    SED_DELETE,
    SED_PRINT,
    SED_QUIT,
};

struct sed_cmd {
    enum sed_cmd_type type;
    int addr_line;            /* 0 = no address, >0 = line number, -1 = last */
    char pattern[256];        /* for s command */
    char replacement[256];    /* for s command */
    int global;               /* g flag */
};

static struct sed_cmd g_cmds[MAX_CMDS];
static int g_ncmds;

static int parse_subst(const char *expr, struct sed_cmd *cmd) {
    cmd->type = SED_SUBST;
    if (expr[0] != 's' || expr[1] == '\0') return -1;
    char delim = expr[1];
    const char *p = expr + 2;

    /* Extract pattern */
    int pi = 0;
    while (*p && *p != delim && pi < 255) cmd->pattern[pi++] = *p++;
    cmd->pattern[pi] = '\0';
    if (*p == delim) p++;

    /* Extract replacement */
    int ri = 0;
    while (*p && *p != delim && ri < 255) cmd->replacement[ri++] = *p++;
    cmd->replacement[ri] = '\0';
    if (*p == delim) p++;

    cmd->global = (*p == 'g');
    return 0;
}

static int parse_command(const char *expr) {
    if (g_ncmds >= MAX_CMDS) return -1;
    struct sed_cmd *cmd = &g_cmds[g_ncmds];
    memset(cmd, 0, sizeof(*cmd));

    const char *p = expr;

    /* Check for address */
    if (*p >= '0' && *p <= '9') {
        cmd->addr_line = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    } else if (*p == '$') {
        cmd->addr_line = -1;
        p++;
    }

    switch (*p) {
    case 's': parse_subst(p, cmd); break;
    case 'd': cmd->type = SED_DELETE; break;
    case 'p': cmd->type = SED_PRINT; break;
    case 'q': cmd->type = SED_QUIT; break;
    default: return -1;
    }

    g_ncmds++;
    return 0;
}

static char *str_replace_first(char *line, const char *pat, const char *rep) {
    static char buf[MAX_LINE];
    char *pos = strstr(line, pat);
    if (!pos) return line;

    size_t plen = strlen(pat);
    size_t rlen = strlen(rep);
    size_t prefix = (size_t)(pos - line);

    memcpy(buf, line, prefix);
    memcpy(buf + prefix, rep, rlen);
    strcpy(buf + prefix + rlen, pos + plen);
    strcpy(line, buf);
    return line;
}

static void process_line(char *line, int lineno, int is_last) {
    int deleted = 0;

    for (int i = 0; i < g_ncmds && !deleted; i++) {
        struct sed_cmd *cmd = &g_cmds[i];

        /* Address check */
        if (cmd->addr_line > 0 && cmd->addr_line != lineno) continue;
        if (cmd->addr_line == -1 && !is_last) continue;

        switch (cmd->type) {
        case SED_SUBST:
            if (cmd->global) {
                while (strstr(line, cmd->pattern))
                    str_replace_first(line, cmd->pattern, cmd->replacement);
            } else {
                str_replace_first(line, cmd->pattern, cmd->replacement);
            }
            break;
        case SED_DELETE:
            deleted = 1;
            break;
        case SED_PRINT:
            printf("%s", line);
            break;
        case SED_QUIT:
            printf("%s", line);
            exit(0);
        }
    }

    if (!deleted) printf("%s", line);
}

int main(int argc, char **argv) {
    int i = 1;
    FILE *input = stdin;

    while (i < argc) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            parse_command(argv[++i]);
            i++;
        } else if (argv[i][0] != '-') {
            /* First non-option might be expression or filename */
            if (g_ncmds == 0) {
                parse_command(argv[i]);
            } else {
                input = fopen(argv[i], "r");
                if (!input) {
                    fprintf(stderr, "sed: %s: No such file\n", argv[i]);
                    return 1;
                }
            }
            i++;
        } else {
            i++;
        }
    }

    if (g_ncmds == 0) {
        fprintf(stderr, "sed: no commands\n");
        return 1;
    }

    char line[MAX_LINE];
    int lineno = 0;
    while (fgets(line, sizeof(line), input)) {
        lineno++;
        process_line(line, lineno, 0);
    }

    if (input != stdin) fclose(input);
    return 0;
}
