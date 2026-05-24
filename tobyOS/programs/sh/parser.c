/* programs/sh/parser.c -- Command parsing and execution (Phase 5).
 *
 * Tokenizer, pipe/redirect/background handling, variable expansion,
 * globbing, builtins, and control-flow (if/for/while/case/functions).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <errno.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);
extern int kill(pid_t pid, int sig);

/* Forward declarations */
int parser_execute_line(const char *line);
int parser_execute_file(const char *path);

/* ---- Limits ---- */

#define MAX_TOKENS    256
#define MAX_ARGS      64
#define MAX_PIPES     16
#define MAX_VARS      256
#define MAX_ALIASES   64
#define MAX_FUNCS     32
#define MAX_JOBS      32
#define MAX_LINE      2048
#define PATH_MAX_SH   256
#define MAX_GLOB      128

/* ---- Token types ---- */

enum token_type {
    TOK_WORD,       /* bare word or quoted string */
    TOK_PIPE,       /* | */
    TOK_REDIR_OUT,  /* > */
    TOK_REDIR_APP,  /* >> */
    TOK_REDIR_IN,   /* < */
    TOK_REDIR_ERR,  /* 2> */
    TOK_BG,         /* & */
    TOK_SEMI,       /* ; */
    TOK_AND,        /* && */
    TOK_OR,         /* || */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_NEWLINE,
    TOK_EOF,
};

struct token {
    enum token_type type;
    char value[MAX_LINE];
};

/* ---- Shell state ---- */

static int  g_last_status  = 0;
static int  g_running      = 1;
static int  g_interactive  = 0;
static pid_t g_mypid       = 0;
static pid_t g_last_bg_pid = 0;

/* Variables */
static struct {
    char name[64];
    char value[256];
    int  exported;
} g_vars[MAX_VARS];
static int g_nvar = 0;

/* Aliases */
static struct {
    char name[64];
    char value[MAX_LINE];
} g_aliases[MAX_ALIASES];
static int g_nalias = 0;

/* Functions */
static struct {
    char name[64];
    char body[MAX_LINE * 4];
} g_funcs[MAX_FUNCS];
static int g_nfunc = 0;

/* Jobs */
struct job {
    int  active;
    pid_t pid;
    int  stopped;
    char cmd[128];
};
static struct job g_jobs[MAX_JOBS];
static int g_njobs = 0;

/* ---- Public interface ---- */

void parser_init(void) {
    g_last_status = 0;
    g_running = 1;
    g_nvar = 0;
    g_nalias = 0;
    g_nfunc = 0;
    g_njobs = 0;

    /* Set default variables */
    char *path_env = getenv("PATH");
    if (!path_env) path_env = "/bin";
    /* Will be available through var_get */
}

int  shell_get_last_status(void) { return g_last_status; }
int  shell_is_running(void)      { return g_running; }
void shell_set_interactive(int v) { g_interactive = v; }
void shell_set_pid(pid_t p)      { g_mypid = p; }

/* ---- Variable operations ---- */

static const char *var_get(const char *name) {
    if (strcmp(name, "?") == 0) {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%d", g_last_status);
        return buf;
    }
    if (strcmp(name, "$") == 0) {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)g_mypid);
        return buf;
    }
    if (strcmp(name, "!") == 0) {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)g_last_bg_pid);
        return buf;
    }
    for (int i = 0; i < g_nvar; i++) {
        if (strcmp(g_vars[i].name, name) == 0)
            return g_vars[i].value;
    }
    return getenv(name);
}

static void var_set(const char *name, const char *value) {
    for (int i = 0; i < g_nvar; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            strncpy(g_vars[i].value, value, 255);
            g_vars[i].value[255] = '\0';
            return;
        }
    }
    if (g_nvar < MAX_VARS) {
        strncpy(g_vars[g_nvar].name, name, 63);
        g_vars[g_nvar].name[63] = '\0';
        strncpy(g_vars[g_nvar].value, value, 255);
        g_vars[g_nvar].value[255] = '\0';
        g_vars[g_nvar].exported = 0;
        g_nvar++;
    }
}

static void var_unset(const char *name) {
    for (int i = 0; i < g_nvar; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            g_vars[i] = g_vars[--g_nvar];
            return;
        }
    }
}

static void var_export(const char *name) {
    for (int i = 0; i < g_nvar; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            g_vars[i].exported = 1;
            setenv(name, g_vars[i].value, 1);
            return;
        }
    }
}

/* ---- Alias operations ---- */

static const char *alias_get(const char *name) {
    for (int i = 0; i < g_nalias; i++) {
        if (strcmp(g_aliases[i].name, name) == 0)
            return g_aliases[i].value;
    }
    return NULL;
}

static void alias_set(const char *name, const char *value) {
    for (int i = 0; i < g_nalias; i++) {
        if (strcmp(g_aliases[i].name, name) == 0) {
            strncpy(g_aliases[i].value, value, MAX_LINE - 1);
            return;
        }
    }
    if (g_nalias < MAX_ALIASES) {
        strncpy(g_aliases[g_nalias].name, name, 63);
        g_aliases[g_nalias].name[63] = '\0';
        strncpy(g_aliases[g_nalias].value, value, MAX_LINE - 1);
        g_aliases[g_nalias].value[MAX_LINE - 1] = '\0';
        g_nalias++;
    }
}

/* ---- Function operations ---- */

static const char *func_get(const char *name) {
    for (int i = 0; i < g_nfunc; i++) {
        if (strcmp(g_funcs[i].name, name) == 0)
            return g_funcs[i].body;
    }
    return NULL;
}

static void func_set(const char *name, const char *body) {
    for (int i = 0; i < g_nfunc; i++) {
        if (strcmp(g_funcs[i].name, name) == 0) {
            strncpy(g_funcs[i].body, body, sizeof(g_funcs[0].body) - 1);
            return;
        }
    }
    if (g_nfunc < MAX_FUNCS) {
        strncpy(g_funcs[g_nfunc].name, name, 63);
        g_funcs[g_nfunc].name[63] = '\0';
        strncpy(g_funcs[g_nfunc].body, body, sizeof(g_funcs[0].body) - 1);
        g_funcs[g_nfunc].body[sizeof(g_funcs[0].body) - 1] = '\0';
        g_nfunc++;
    }
}

/* ---- Job control ---- */

static int job_add(pid_t pid, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!g_jobs[i].active) {
            g_jobs[i].active = 1;
            g_jobs[i].pid = pid;
            g_jobs[i].stopped = 0;
            strncpy(g_jobs[i].cmd, cmd, 127);
            g_jobs[i].cmd[127] = '\0';
            if (i >= g_njobs) g_njobs = i + 1;
            return i + 1;
        }
    }
    return -1;
}

static void job_remove(pid_t pid) {
    for (int i = 0; i < g_njobs; i++) {
        if (g_jobs[i].active && g_jobs[i].pid == pid) {
            g_jobs[i].active = 0;
            return;
        }
    }
}

/* ---- Variable expansion ---- */

static int expand_variables(const char *src, char *dst, int dstsize) {
    int si = 0, di = 0;
    int slen = (int)strlen(src);

    while (si < slen && di < dstsize - 1) {
        if (src[si] == '\\' && si + 1 < slen) {
            si++;
            dst[di++] = src[si++];
            continue;
        }
        if (src[si] == '$') {
            si++;
            if (si >= slen) { dst[di++] = '$'; break; }

            char varname[128];
            int vi = 0;
            int has_brace = 0;
            const char *defval = NULL;
            (void)has_brace;

            if (src[si] == '{') {
                has_brace = 1;
                si++;
                while (si < slen && src[si] != '}' && src[si] != ':' && vi < 126) {
                    varname[vi++] = src[si++];
                }
                varname[vi] = '\0';
                if (si < slen && src[si] == ':' && si + 1 < slen && src[si+1] == '-') {
                    si += 2;
                    int ds = si;
                    while (si < slen && src[si] != '}') si++;
                    static char defbuf[256];
                    int dlen = si - ds;
                    if (dlen > 255) dlen = 255;
                    memcpy(defbuf, src + ds, dlen);
                    defbuf[dlen] = '\0';
                    defval = defbuf;
                }
                if (si < slen && src[si] == '}') si++;
            } else if (src[si] == '?' || src[si] == '$' || src[si] == '!') {
                varname[0] = src[si++];
                varname[1] = '\0';
            } else {
                while (si < slen && vi < 126 &&
                       (src[si] == '_' ||
                        (src[si] >= 'a' && src[si] <= 'z') ||
                        (src[si] >= 'A' && src[si] <= 'Z') ||
                        (src[si] >= '0' && src[si] <= '9'))) {
                    varname[vi++] = src[si++];
                }
                varname[vi] = '\0';
            }

            const char *val = var_get(varname);
            if (!val || val[0] == '\0') val = defval;
            if (!val) val = "";

            int vlen = (int)strlen(val);
            if (di + vlen >= dstsize - 1) vlen = dstsize - 1 - di;
            memcpy(dst + di, val, vlen);
            di += vlen;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return di;
}

/* ---- Globbing ---- */

static int match_glob(const char *pattern, const char *text) {
    while (*pattern && *text) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*text) {
                if (match_glob(pattern, text)) return 1;
                text++;
            }
            return 0;
        }
        if (*pattern == '?') {
            pattern++;
            text++;
        } else {
            if (*pattern != *text) return 0;
            pattern++;
            text++;
        }
    }
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *text == '\0');
}

static int expand_glob(const char *pattern, char results[][PATH_MAX_SH], int max) {
    int count = 0;
    int has_glob = 0;

    for (const char *p = pattern; *p; p++) {
        if (*p == '*' || *p == '?') { has_glob = 1; break; }
    }
    if (!has_glob) {
        strncpy(results[0], pattern, PATH_MAX_SH - 1);
        results[0][PATH_MAX_SH - 1] = '\0';
        return 1;
    }

    /* Find directory part */
    const char *slash = NULL;
    for (const char *p = pattern; *p; p++) {
        if (*p == '/') slash = p;
    }

    char dir[PATH_MAX_SH];
    const char *file_pattern;
    if (slash) {
        int dlen = (int)(slash - pattern);
        if (dlen >= PATH_MAX_SH) dlen = PATH_MAX_SH - 1;
        memcpy(dir, pattern, dlen);
        dir[dlen] = '\0';
        file_pattern = slash + 1;
    } else {
        strcpy(dir, ".");
        file_pattern = pattern;
    }

    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.' && file_pattern[0] != '.') continue;
        if (match_glob(file_pattern, ent->d_name)) {
            if (slash) {
                snprintf(results[count], PATH_MAX_SH, "%s/%s", dir, ent->d_name);
            } else {
                strncpy(results[count], ent->d_name, PATH_MAX_SH - 1);
                results[count][PATH_MAX_SH - 1] = '\0';
            }
            count++;
        }
    }
    closedir(d);
    return count;
}

/* ---- Tokenizer ---- */

static int tokenize(const char *line, struct token *tokens, int max_tok) {
    int ntok = 0;
    int i = 0;
    int len = (int)strlen(line);

    while (i < len && ntok < max_tok - 1) {
        /* Skip whitespace */
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= len) break;

        /* Check operators */
        if (line[i] == '|' && i + 1 < len && line[i+1] == '|') {
            tokens[ntok].type = TOK_OR;
            tokens[ntok].value[0] = '|'; tokens[ntok].value[1] = '|';
            tokens[ntok].value[2] = '\0';
            ntok++; i += 2; continue;
        }
        if (line[i] == '|') {
            tokens[ntok].type = TOK_PIPE;
            tokens[ntok].value[0] = '|'; tokens[ntok].value[1] = '\0';
            ntok++; i++; continue;
        }
        if (line[i] == '&' && i + 1 < len && line[i+1] == '&') {
            tokens[ntok].type = TOK_AND;
            tokens[ntok].value[0] = '&'; tokens[ntok].value[1] = '&';
            tokens[ntok].value[2] = '\0';
            ntok++; i += 2; continue;
        }
        if (line[i] == '&') {
            tokens[ntok].type = TOK_BG;
            tokens[ntok].value[0] = '&'; tokens[ntok].value[1] = '\0';
            ntok++; i++; continue;
        }
        if (line[i] == ';') {
            tokens[ntok].type = TOK_SEMI;
            tokens[ntok].value[0] = ';'; tokens[ntok].value[1] = '\0';
            ntok++; i++; continue;
        }
        if (line[i] == '>' && i + 1 < len && line[i+1] == '>') {
            tokens[ntok].type = TOK_REDIR_APP;
            tokens[ntok].value[0] = '>'; tokens[ntok].value[1] = '>';
            tokens[ntok].value[2] = '\0';
            ntok++; i += 2; continue;
        }
        if (line[i] == '>') {
            tokens[ntok].type = TOK_REDIR_OUT;
            tokens[ntok].value[0] = '>'; tokens[ntok].value[1] = '\0';
            ntok++; i++; continue;
        }
        if (line[i] == '<') {
            tokens[ntok].type = TOK_REDIR_IN;
            tokens[ntok].value[0] = '<'; tokens[ntok].value[1] = '\0';
            ntok++; i++; continue;
        }
        if (line[i] == '2' && i + 1 < len && line[i+1] == '>') {
            tokens[ntok].type = TOK_REDIR_ERR;
            tokens[ntok].value[0] = '2'; tokens[ntok].value[1] = '>';
            tokens[ntok].value[2] = '\0';
            ntok++; i += 2; continue;
        }
        if (line[i] == '(') {
            tokens[ntok].type = TOK_LPAREN;
            tokens[ntok].value[0] = '('; tokens[ntok].value[1] = '\0';
            ntok++; i++; continue;
        }
        if (line[i] == ')') {
            tokens[ntok].type = TOK_RPAREN;
            tokens[ntok].value[0] = ')'; tokens[ntok].value[1] = '\0';
            ntok++; i++; continue;
        }

        /* Word (possibly quoted) */
        tokens[ntok].type = TOK_WORD;
        int vi = 0;
        while (i < len && line[i] != ' ' && line[i] != '\t' &&
               line[i] != '|' && line[i] != '&' && line[i] != ';' &&
               line[i] != '>' && line[i] != '<' && line[i] != '(' &&
               line[i] != ')' && vi < MAX_LINE - 1) {
            if (line[i] == '"') {
                i++;
                while (i < len && line[i] != '"' && vi < MAX_LINE - 1) {
                    if (line[i] == '\\' && i + 1 < len) {
                        i++;
                        switch (line[i]) {
                        case 'n': tokens[ntok].value[vi++] = '\n'; break;
                        case 't': tokens[ntok].value[vi++] = '\t'; break;
                        case '\\': tokens[ntok].value[vi++] = '\\'; break;
                        case '"': tokens[ntok].value[vi++] = '"'; break;
                        case '$': tokens[ntok].value[vi++] = '$'; break;
                        default: tokens[ntok].value[vi++] = line[i]; break;
                        }
                    } else {
                        tokens[ntok].value[vi++] = line[i];
                    }
                    i++;
                }
                if (i < len && line[i] == '"') i++;
            } else if (line[i] == '\'') {
                i++;
                while (i < len && line[i] != '\'' && vi < MAX_LINE - 1) {
                    tokens[ntok].value[vi++] = line[i++];
                }
                if (i < len && line[i] == '\'') i++;
            } else if (line[i] == '\\' && i + 1 < len) {
                i++;
                tokens[ntok].value[vi++] = line[i++];
            } else {
                tokens[ntok].value[vi++] = line[i++];
            }
        }
        tokens[ntok].value[vi] = '\0';
        ntok++;
    }

    tokens[ntok].type = TOK_EOF;
    tokens[ntok].value[0] = '\0';
    return ntok;
}

/* ---- Command resolution ---- */

static int resolve_path(const char *cmd, char *out, int outsize) {
    if (cmd[0] == '/' || cmd[0] == '.') {
        strncpy(out, cmd, outsize - 1);
        out[outsize - 1] = '\0';
        return 0;
    }
    /* Search /bin */
    snprintf(out, outsize, "/bin/%s", cmd);
    return 0;
}

/* ---- Simple command execution ---- */

struct simple_cmd {
    char *argv[MAX_ARGS];
    int   argc;
    char *redir_in;
    char *redir_out;
    char *redir_err;
    int   append_out;
    int   background;
};

static int exec_simple(struct simple_cmd *cmd) {
    if (cmd->argc == 0) return 0;

    char path[PATH_MAX_SH];
    resolve_path(cmd->argv[0], path, sizeof(path));

    int fd_in  = 0;
    int fd_out = 1;
    int fd_err = 2;

    if (cmd->redir_in) {
        fd_in = open(cmd->redir_in, O_RDONLY);
        if (fd_in < 0) {
            fprintf(stderr, "sh: cannot open %s: %s\n", cmd->redir_in, strerror(errno));
            return 1;
        }
    }
    if (cmd->redir_out) {
        int flags = O_WRONLY | O_CREAT;
        flags |= cmd->append_out ? O_APPEND : O_TRUNC;
        fd_out = open(cmd->redir_out, flags, 0644);
        if (fd_out < 0) {
            fprintf(stderr, "sh: cannot open %s: %s\n", cmd->redir_out, strerror(errno));
            if (fd_in > 2) close(fd_in);
            return 1;
        }
    }
    if (cmd->redir_err) {
        fd_err = open(cmd->redir_err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_err < 0) {
            fprintf(stderr, "sh: cannot open %s: %s\n", cmd->redir_err, strerror(errno));
            if (fd_in > 2) close(fd_in);
            if (fd_out > 2) close(fd_out);
            return 1;
        }
    }

    pid_t pid = toby_spawn(path, cmd->argv, NULL, fd_in, fd_out, fd_err);

    if (fd_in > 2) close(fd_in);
    if (fd_out > 2) close(fd_out);
    if (fd_err > 2) close(fd_err);

    if (pid < 0) {
        fprintf(stderr, "sh: %s: command not found\n", cmd->argv[0]);
        return 127;
    }

    if (cmd->background) {
        int jid = job_add(pid, cmd->argv[0]);
        g_last_bg_pid = pid;
        if (g_interactive) {
            printf("[%d] %d\n", jid, pid);
        }
        return 0;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

/* ---- Pipeline execution ---- */

static int exec_pipeline(struct token *tokens, int start, int end, int bg) {
    /* Count pipes */
    int pipe_pos[MAX_PIPES];
    int npipes = 0;

    for (int i = start; i < end; i++) {
        if (tokens[i].type == TOK_PIPE) {
            if (npipes < MAX_PIPES) pipe_pos[npipes++] = i;
        }
    }

    if (npipes == 0) {
        /* Single command */
        struct simple_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.background = bg;

        char expanded[MAX_LINE];
        for (int i = start; i < end; i++) {
            if (tokens[i].type != TOK_WORD) continue;

            if (i + 1 < end && tokens[i].type == TOK_WORD) {
                /* Check for redirections that follow */
            }

            if (tokens[i].type == TOK_REDIR_IN && i + 1 < end) {
                cmd.redir_in = tokens[++i].value;
                continue;
            }
            if (tokens[i].type == TOK_REDIR_OUT && i + 1 < end) {
                cmd.redir_out = tokens[++i].value;
                cmd.append_out = 0;
                continue;
            }
            if (tokens[i].type == TOK_REDIR_APP && i + 1 < end) {
                cmd.redir_out = tokens[++i].value;
                cmd.append_out = 1;
                continue;
            }
            if (tokens[i].type == TOK_REDIR_ERR && i + 1 < end) {
                cmd.redir_err = tokens[++i].value;
                continue;
            }

            expand_variables(tokens[i].value, expanded, MAX_LINE);

            /* Try glob expansion */
            static char glob_results[MAX_GLOB][PATH_MAX_SH];
            int nglob = expand_glob(expanded, glob_results, MAX_GLOB);
            for (int g = 0; g < nglob && cmd.argc < MAX_ARGS - 1; g++) {
                cmd.argv[cmd.argc] = glob_results[g];
                cmd.argc++;
            }
        }
        cmd.argv[cmd.argc] = NULL;

        if (cmd.argc == 0) return 0;
        return exec_simple(&cmd);
    }

    /* Multi-stage pipeline */
    int segments[MAX_PIPES + 2];
    segments[0] = start;
    for (int i = 0; i < npipes; i++) segments[i + 1] = pipe_pos[i] + 1;
    segments[npipes + 1] = end;

    int prev_read = 0;
    pid_t pids[MAX_PIPES + 1];
    int nprocs = npipes + 1;

    for (int s = 0; s < nprocs; s++) {
        int seg_start = segments[s];
        int seg_end   = segments[s + 1];
        if (s < nprocs - 1 && tokens[seg_end - 1].type == TOK_PIPE) seg_end--;

        int pipefd[2] = {-1, -1};
        if (s < nprocs - 1) {
            pipe(pipefd);
        }

        /* Build argv for this segment */
        char *argv[MAX_ARGS];
        int argc = 0;
        char expanded[MAX_LINE];

        for (int i = seg_start; i < seg_end && argc < MAX_ARGS - 1; i++) {
            if (tokens[i].type != TOK_WORD) continue;
            expand_variables(tokens[i].value, expanded, MAX_LINE);
            static char arg_buf[MAX_ARGS][PATH_MAX_SH];
            strncpy(arg_buf[argc], expanded, PATH_MAX_SH - 1);
            argv[argc] = arg_buf[argc];
            argc++;
        }
        argv[argc] = NULL;

        if (argc == 0) {
            if (pipefd[0] >= 0) close(pipefd[0]);
            if (pipefd[1] >= 0) close(pipefd[1]);
            if (prev_read > 2) close(prev_read);
            continue;
        }

        char path[PATH_MAX_SH];
        resolve_path(argv[0], path, sizeof(path));

        int fd_in  = (s == 0) ? 0 : prev_read;
        int fd_out = (s == nprocs - 1) ? 1 : pipefd[1];

        pids[s] = toby_spawn(path, argv, NULL, fd_in, fd_out, 2);

        if (fd_in > 2) close(fd_in);
        if (pipefd[1] >= 0 && pipefd[1] > 2) close(pipefd[1]);

        prev_read = pipefd[0];
    }

    /* Wait for all processes */
    int last_status = 0;
    for (int s = 0; s < nprocs; s++) {
        if (pids[s] > 0) {
            int st = 0;
            waitpid(pids[s], &st, 0);
            if (s == nprocs - 1) last_status = st;
        }
    }
    return last_status;
}

/* ---- Builtins ---- */

static int builtin_cd(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : var_get("HOME");
    if (!dir) dir = "/";
    if (chdir(dir) < 0) {
        fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int builtin_pwd(void) {
    char cwd[PATH_MAX_SH];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("%s\n", cwd);
        return 0;
    }
    return 1;
}

static int builtin_echo(int argc, char **argv) {
    int newline = 1;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start = 2;
    }
    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        printf("%s", argv[i]);
    }
    if (newline) putchar('\n');
    return 0;
}

static int builtin_export(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            var_set(argv[i], eq + 1);
            var_export(argv[i]);
            *eq = '=';
        } else {
            var_export(argv[i]);
        }
    }
    return 0;
}

static int builtin_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) var_unset(argv[i]);
    return 0;
}

static int builtin_alias(int argc, char **argv) {
    if (argc == 1) {
        for (int i = 0; i < g_nalias; i++) {
            printf("alias %s='%s'\n", g_aliases[i].name, g_aliases[i].value);
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            alias_set(argv[i], eq + 1);
            *eq = '=';
        }
    }
    return 0;
}

static int builtin_jobs(void) {
    for (int i = 0; i < g_njobs; i++) {
        if (g_jobs[i].active) {
            printf("[%d] %s  %s\n", i + 1,
                   g_jobs[i].stopped ? "Stopped" : "Running",
                   g_jobs[i].cmd);
        }
    }
    return 0;
}

static int builtin_fg(int argc, char **argv) {
    int jid = 0;
    if (argc > 1) jid = atoi(argv[1]) - 1;
    else {
        for (int i = g_njobs - 1; i >= 0; i--) {
            if (g_jobs[i].active) { jid = i; break; }
        }
    }
    if (jid < 0 || jid >= g_njobs || !g_jobs[jid].active) {
        fprintf(stderr, "fg: no such job\n");
        return 1;
    }
    pid_t pid = g_jobs[jid].pid;
    g_jobs[jid].stopped = 0;
    kill(pid, 18);  /* SIGCONT */
    int st = 0;
    waitpid(pid, &st, 0);
    job_remove(pid);
    return st;
}

static int builtin_bg(int argc, char **argv) {
    int jid = 0;
    if (argc > 1) jid = atoi(argv[1]) - 1;
    else {
        for (int i = g_njobs - 1; i >= 0; i--) {
            if (g_jobs[i].active && g_jobs[i].stopped) { jid = i; break; }
        }
    }
    if (jid < 0 || jid >= g_njobs || !g_jobs[jid].active) {
        fprintf(stderr, "bg: no such job\n");
        return 1;
    }
    g_jobs[jid].stopped = 0;
    kill(g_jobs[jid].pid, 18);  /* SIGCONT */
    printf("[%d] %s &\n", jid + 1, g_jobs[jid].cmd);
    return 0;
}

static int builtin_kill(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill [-sig] pid\n");
        return 1;
    }
    int sig = 15;  /* SIGTERM */
    int pidarg = 1;
    if (argv[1][0] == '-') {
        sig = atoi(argv[1] + 1);
        pidarg = 2;
    }
    if (pidarg >= argc) return 1;
    pid_t pid = (pid_t)atoi(argv[pidarg]);
    return kill(pid, sig);
}

static int builtin_set(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < g_nvar; i++) {
        printf("%s=%s\n", g_vars[i].name, g_vars[i].value);
    }
    return 0;
}

static int is_builtin(const char *cmd) {
    static const char *builtins[] = {
        "cd", "pwd", "echo", "exit", "export", "unset", "alias",
        "history", "jobs", "fg", "bg", "kill", "source", "set",
        "true", "false", NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(cmd, builtins[i]) == 0) return 1;
    }
    return 0;
}

static int run_builtin(int argc, char **argv) {
    if (strcmp(argv[0], "cd") == 0)      return builtin_cd(argc, argv);
    if (strcmp(argv[0], "pwd") == 0)     return builtin_pwd();
    if (strcmp(argv[0], "echo") == 0)    return builtin_echo(argc, argv);
    if (strcmp(argv[0], "exit") == 0) {
        g_running = 0;
        if (argc > 1) g_last_status = atoi(argv[1]);
        return g_last_status;
    }
    if (strcmp(argv[0], "export") == 0)  return builtin_export(argc, argv);
    if (strcmp(argv[0], "unset") == 0)   return builtin_unset(argc, argv);
    if (strcmp(argv[0], "alias") == 0)   return builtin_alias(argc, argv);
    if (strcmp(argv[0], "jobs") == 0)    return builtin_jobs();
    if (strcmp(argv[0], "fg") == 0)      return builtin_fg(argc, argv);
    if (strcmp(argv[0], "bg") == 0)      return builtin_bg(argc, argv);
    if (strcmp(argv[0], "kill") == 0)    return builtin_kill(argc, argv);
    if (strcmp(argv[0], "set") == 0)     return builtin_set(argc, argv);
    if (strcmp(argv[0], "true") == 0)    return 0;
    if (strcmp(argv[0], "false") == 0)   return 1;
    if (strcmp(argv[0], "source") == 0) {
        if (argc < 2) return 1;
        return parser_execute_file(argv[1]);
    }
    if (strcmp(argv[0], "history") == 0) {
        extern char g_history[][2048];
        extern int  g_hist_count;
        /* handled in main.c */
        return 0;
    }
    return 1;
}

/* ---- Control flow: if/for/while/case/functions ---- */

static int find_keyword(struct token *tokens, int start, int end, const char *kw) {
    int depth = 0;
    for (int i = start; i < end; i++) {
        if (tokens[i].type != TOK_WORD) continue;
        if (strcmp(tokens[i].value, "if") == 0 ||
            strcmp(tokens[i].value, "for") == 0 ||
            strcmp(tokens[i].value, "while") == 0 ||
            strcmp(tokens[i].value, "case") == 0) depth++;
        if (strcmp(tokens[i].value, "fi") == 0 ||
            strcmp(tokens[i].value, "done") == 0 ||
            strcmp(tokens[i].value, "esac") == 0) {
            if (depth > 0) depth--;
            else if (strcmp(tokens[i].value, kw) == 0) return i;
        }
        if (depth == 0 && strcmp(tokens[i].value, kw) == 0) return i;
    }
    return -1;
}

static int exec_tokens(struct token *tokens, int start, int end);

static int exec_if(struct token *tokens, int start, int end) {
    /* if cond; then body; [elif cond; then body;]... [else body;] fi */
    int i = start + 1;  /* skip "if" */

    /* Find "then" */
    int then_pos = find_keyword(tokens, i, end, "then");
    if (then_pos < 0) return 1;

    /* Execute condition */
    int cond = exec_tokens(tokens, i, then_pos);

    /* Find else/elif/fi */
    int else_pos = find_keyword(tokens, then_pos + 1, end, "else");
    int elif_pos = find_keyword(tokens, then_pos + 1, end, "elif");
    int fi_pos   = find_keyword(tokens, then_pos + 1, end, "fi");
    if (fi_pos < 0) return 1;

    if (cond == 0) {
        int body_end = fi_pos;
        if (else_pos >= 0 && else_pos < fi_pos) body_end = else_pos;
        if (elif_pos >= 0 && elif_pos < body_end) body_end = elif_pos;
        return exec_tokens(tokens, then_pos + 1, body_end);
    } else if (elif_pos >= 0 && elif_pos < fi_pos &&
               (else_pos < 0 || elif_pos < else_pos)) {
        /* Treat elif...fi as a new if...fi */
        tokens[elif_pos].value[0] = 'i'; tokens[elif_pos].value[1] = 'f';
        tokens[elif_pos].value[2] = '\0';
        return exec_if(tokens, elif_pos, fi_pos + 1);
    } else if (else_pos >= 0 && else_pos < fi_pos) {
        return exec_tokens(tokens, else_pos + 1, fi_pos);
    }
    return 0;
}

static int exec_for(struct token *tokens, int start, int end) {
    /* for VAR in words...; do body; done */
    int i = start + 1;
    if (i >= end || tokens[i].type != TOK_WORD) return 1;
    char varname[64];
    strncpy(varname, tokens[i].value, 63);
    varname[63] = '\0';
    i++;

    if (i >= end || strcmp(tokens[i].value, "in") != 0) return 1;
    i++;

    /* Collect words until "do" */
    char *words[MAX_ARGS];
    int nwords = 0;
    while (i < end && tokens[i].type == TOK_WORD &&
           strcmp(tokens[i].value, "do") != 0 &&
           nwords < MAX_ARGS - 1) {
        words[nwords++] = tokens[i].value;
        i++;
    }
    /* Skip semicolons */
    while (i < end && tokens[i].type == TOK_SEMI) i++;

    int do_pos = i;
    if (do_pos >= end || strcmp(tokens[do_pos].value, "do") != 0) return 1;
    do_pos++;

    int done_pos = find_keyword(tokens, do_pos, end, "done");
    if (done_pos < 0) return 1;

    int result = 0;
    for (int w = 0; w < nwords; w++) {
        char expanded[MAX_LINE];
        expand_variables(words[w], expanded, MAX_LINE);

        static char glob_results[MAX_GLOB][PATH_MAX_SH];
        int nglob = expand_glob(expanded, glob_results, MAX_GLOB);
        for (int g = 0; g < nglob; g++) {
            var_set(varname, glob_results[g]);
            result = exec_tokens(tokens, do_pos, done_pos);
        }
    }
    return result;
}

static int exec_while(struct token *tokens, int start, int end) {
    /* while cond; do body; done */
    int i = start + 1;
    int do_pos = find_keyword(tokens, i, end, "do");
    if (do_pos < 0) return 1;
    int done_pos = find_keyword(tokens, do_pos + 1, end, "done");
    if (done_pos < 0) return 1;

    int result = 0;
    for (;;) {
        int cond = exec_tokens(tokens, i, do_pos);
        if (cond != 0) break;
        result = exec_tokens(tokens, do_pos + 1, done_pos);
    }
    return result;
}

static int exec_case(struct token *tokens, int start, int end) {
    /* case WORD in pattern) body;; ... esac */
    int i = start + 1;
    if (i >= end || tokens[i].type != TOK_WORD) return 1;

    char expanded[MAX_LINE];
    expand_variables(tokens[i].value, expanded, MAX_LINE);
    i++;

    if (i >= end || strcmp(tokens[i].value, "in") != 0) return 1;
    i++;

    int esac_pos = find_keyword(tokens, i, end, "esac");
    if (esac_pos < 0) return 1;

    while (i < esac_pos) {
        /* Find pattern) */
        if (tokens[i].type != TOK_WORD) { i++; continue; }
        char *pattern = tokens[i].value;
        i++;
        if (i >= esac_pos) break;
        /* Expect ')' at end of pattern word or as next token */
        int plen = (int)strlen(pattern);
        if (plen > 0 && pattern[plen-1] == ')') pattern[plen-1] = '\0';

        /* Find ;; */
        int body_start = i;
        int body_end = i;
        while (body_end < esac_pos) {
            if (tokens[body_end].type == TOK_SEMI &&
                body_end + 1 < esac_pos && tokens[body_end+1].type == TOK_SEMI) {
                break;
            }
            body_end++;
        }

        if (match_glob(pattern, expanded) || strcmp(pattern, "*") == 0) {
            return exec_tokens(tokens, body_start, body_end);
        }

        i = body_end + 2;  /* skip ;; */
    }
    return 0;
}

/* ---- Execute a token range (handles ;, &&, ||) ---- */

static int exec_tokens(struct token *tokens, int start, int end) {
    if (start >= end) return 0;

    /* Check for function definition: name() { body } */
    if (end - start >= 4 && tokens[start].type == TOK_WORD &&
        start + 1 < end && tokens[start+1].type == TOK_LPAREN &&
        start + 2 < end && tokens[start+2].type == TOK_RPAREN) {
        /* Function definition */
        /* Find matching } -- simplified: body is from { to } */
        int brace_start = start + 3;
        if (brace_start < end && tokens[brace_start].type == TOK_WORD &&
            strcmp(tokens[brace_start].value, "{") == 0) {
            brace_start++;
            int brace_end = brace_start;
            int depth = 1;
            while (brace_end < end && depth > 0) {
                if (tokens[brace_end].type == TOK_WORD) {
                    if (strcmp(tokens[brace_end].value, "{") == 0) depth++;
                    if (strcmp(tokens[brace_end].value, "}") == 0) depth--;
                }
                if (depth > 0) brace_end++;
            }
            /* Serialize body tokens back to string */
            char body[MAX_LINE * 4];
            int bpos = 0;
            for (int t = brace_start; t < brace_end && bpos < (int)sizeof(body) - 2; t++) {
                int vl = (int)strlen(tokens[t].value);
                memcpy(body + bpos, tokens[t].value, vl);
                bpos += vl;
                body[bpos++] = ' ';
            }
            body[bpos] = '\0';
            func_set(tokens[start].value, body);
            return 0;
        }
    }

    /* Check for control flow keywords */
    if (tokens[start].type == TOK_WORD) {
        if (strcmp(tokens[start].value, "if") == 0)
            return exec_if(tokens, start, end);
        if (strcmp(tokens[start].value, "for") == 0)
            return exec_for(tokens, start, end);
        if (strcmp(tokens[start].value, "while") == 0)
            return exec_while(tokens, start, end);
        if (strcmp(tokens[start].value, "case") == 0)
            return exec_case(tokens, start, end);
    }

    /* Split on ; && || */
    int result = 0;
    int seg_start = start;

    for (int i = start; i <= end; i++) {
        int is_sep = (i == end) ||
                     tokens[i].type == TOK_SEMI ||
                     tokens[i].type == TOK_AND ||
                     tokens[i].type == TOK_OR;

        if (!is_sep) continue;

        /* Determine if this segment should run */
        int seg_end = i;
        int bg = 0;
        if (seg_end > seg_start && tokens[seg_end - 1].type == TOK_BG) {
            bg = 1;
            seg_end--;
        }

        if (seg_end > seg_start) {
            /* Check for variable assignment */
            if (tokens[seg_start].type == TOK_WORD &&
                strchr(tokens[seg_start].value, '=') &&
                tokens[seg_start].value[0] != '=') {
                char *eq = strchr(tokens[seg_start].value, '=');
                *eq = '\0';
                char expanded[MAX_LINE];
                expand_variables(eq + 1, expanded, MAX_LINE);
                var_set(tokens[seg_start].value, expanded);
                result = 0;
            }
            /* Check for builtin */
            else if (is_builtin(tokens[seg_start].value)) {
                char expanded[MAX_LINE];
                char *argv[MAX_ARGS];
                int argc = 0;
                for (int t = seg_start; t < seg_end && argc < MAX_ARGS - 1; t++) {
                    if (tokens[t].type != TOK_WORD) continue;
                    expand_variables(tokens[t].value, expanded, MAX_LINE);
                    static char arg_store[MAX_ARGS][PATH_MAX_SH];
                    strncpy(arg_store[argc], expanded, PATH_MAX_SH - 1);
                    argv[argc] = arg_store[argc];
                    argc++;
                }
                argv[argc] = NULL;
                result = run_builtin(argc, argv);
            }
            /* Check for function call */
            else if (tokens[seg_start].type == TOK_WORD &&
                     func_get(tokens[seg_start].value)) {
                const char *body = func_get(tokens[seg_start].value);
                result = parser_execute_line(body);
            }
            /* External command / pipeline */
            else {
                /* Alias expansion */
                if (tokens[seg_start].type == TOK_WORD) {
                    const char *al = alias_get(tokens[seg_start].value);
                    if (al) {
                        strncpy(tokens[seg_start].value, al, MAX_LINE - 1);
                    }
                }
                result = exec_pipeline(tokens, seg_start, seg_end, bg);
            }
        }

        g_last_status = result;

        /* Handle && and || */
        if (i < end) {
            if (tokens[i].type == TOK_AND && result != 0) {
                /* Skip until next || or ; */
                i++;
                while (i < end && tokens[i].type != TOK_OR &&
                       tokens[i].type != TOK_SEMI) i++;
                if (i < end) i++;
                seg_start = i;
                continue;
            }
            if (tokens[i].type == TOK_OR && result == 0) {
                /* Skip until next && or ; */
                i++;
                while (i < end && tokens[i].type != TOK_AND &&
                       tokens[i].type != TOK_SEMI) i++;
                if (i < end) i++;
                seg_start = i;
                continue;
            }
        }

        seg_start = i + 1;
    }

    return result;
}

/* ---- Public API ---- */

int parser_execute_line(const char *line) {
    if (!line || !line[0]) return 0;

    /* Skip comments */
    if (line[0] == '#') return 0;

    struct token tokens[MAX_TOKENS];
    int ntok = tokenize(line, tokens, MAX_TOKENS);
    if (ntok == 0) return 0;

    return exec_tokens(tokens, 0, ntok);
}

int parser_execute_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    char buf[MAX_LINE * 8];
    int total = 0;
    int n;
    while ((n = (int)read(fd, buf + total, sizeof(buf) - total - 1)) > 0) {
        total += n;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    close(fd);
    buf[total] = '\0';

    /* Execute line by line */
    int result = 0;
    char *line_start = buf;
    for (int i = 0; i <= total; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            buf[i] = '\0';
            if (line_start[0] && line_start[0] != '#') {
                result = parser_execute_line(line_start);
                if (!g_running) break;
            }
            line_start = buf + i + 1;
        }
    }
    return result;
}
