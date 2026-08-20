/* user_sh/main.c -- /bin/sh, the tobyOS userland shell.
 *
 * POSIX-like shell with pipes, redirects, variables, scripting,
 * glob expansion, and command history. Linked with libtoby. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

#define MAX_LINE    1024
#define MAX_ARGS    64
#define MAX_PIPES   8
#define MAX_VARS    128
#define MAX_HISTORY 64
#define MAX_GLOB    256
#define PATH_MAX_SH 256

/* ---- Shell variables -------------------------------------------- */

static struct {
    char name[64];
    char value[256];
} g_vars[MAX_VARS];
static int g_nvar = 0;

static int  g_last_status = 0;
static int  g_running     = 1;
static int  g_interactive = 0;
static pid_t g_mypid      = 0;

/* ---- History ---------------------------------------------------- */

static char g_history[MAX_HISTORY][MAX_LINE];
static int  g_hist_count = 0;

static void hist_add(const char *line) {
    if (!line[0]) return;
    if (g_hist_count > 0 && strcmp(g_history[(g_hist_count-1) % MAX_HISTORY], line) == 0)
        return;
    strncpy(g_history[g_hist_count % MAX_HISTORY], line, MAX_LINE - 1);
    g_history[g_hist_count % MAX_HISTORY][MAX_LINE - 1] = '\0';
    g_hist_count++;
}

/* ---- Variable operations ---------------------------------------- */

static const char *var_get(const char *name) {
    if (strcmp(name, "?") == 0) {
        static char buf[16];
        int v = g_last_status;
        int i = 0;
        if (v < 0) { buf[i++] = '-'; v = -v; }
        if (v >= 100) buf[i++] = '0' + (v / 100) % 10;
        if (v >= 10)  buf[i++] = '0' + (v / 10) % 10;
        buf[i++] = '0' + v % 10;
        buf[i] = '\0';
        return buf;
    }
    if (strcmp(name, "$") == 0) {
        static char buf[16];
        int v = (int)g_mypid, i = 0;
        if (v >= 10000) buf[i++] = '0' + (v/10000)%10;
        if (v >= 1000) buf[i++] = '0' + (v/1000)%10;
        if (v >= 100) buf[i++] = '0' + (v/100)%10;
        if (v >= 10) buf[i++] = '0' + (v/10)%10;
        buf[i++] = '0' + v%10;
        buf[i] = '\0';
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

/* ---- Variable expansion ----------------------------------------- */

static int expand_vars(const char *src, char *dst, int dstsz) {
    int di = 0;
    for (int si = 0; src[si] && di < dstsz - 1; ) {
        if (src[si] == '\\' && src[si+1]) {
            si++;
            dst[di++] = src[si++];
        } else if (src[si] == '\'') {
            si++;
            while (src[si] && src[si] != '\'' && di < dstsz - 1)
                dst[di++] = src[si++];
            if (src[si] == '\'') si++;
        } else if (src[si] == '$') {
            si++;
            char vname[64];
            int vi = 0;
            if (src[si] == '{') {
                si++;
                while (src[si] && src[si] != '}' && vi < 63)
                    vname[vi++] = src[si++];
                if (src[si] == '}') si++;
            } else if (src[si] == '?' || src[si] == '$' || src[si] == '!') {
                vname[vi++] = src[si++];
            } else {
                while ((src[si] >= 'a' && src[si] <= 'z') ||
                       (src[si] >= 'A' && src[si] <= 'Z') ||
                       (src[si] >= '0' && src[si] <= '9') ||
                       src[si] == '_') {
                    if (vi < 63) vname[vi++] = src[si];
                    si++;
                }
            }
            vname[vi] = '\0';
            const char *val = var_get(vname);
            if (val) {
                while (*val && di < dstsz - 1)
                    dst[di++] = *val++;
            }
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return di;
}

/* ---- Glob expansion --------------------------------------------- */

static int glob_match(const char *pattern, const char *str) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*str) {
                if (glob_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?') {
            if (!*str) return 0;
            pattern++;
            str++;
        } else {
            if (*pattern != *str) return 0;
            pattern++;
            str++;
        }
    }
    return *str == '\0';
}

static int has_glob(const char *s) {
    for (; *s; s++)
        if (*s == '*' || *s == '?') return 1;
    return 0;
}

static int do_glob(const char *pattern, char **results, int max) {
    const char *slash = strrchr(pattern, '/');
    char dir[PATH_MAX_SH];
    const char *base_pat;

    if (slash) {
        int dlen = (int)(slash - pattern);
        if (dlen >= PATH_MAX_SH) dlen = PATH_MAX_SH - 1;
        memcpy(dir, pattern, (size_t)dlen);
        dir[dlen] = '\0';
        base_pat = slash + 1;
    } else {
        strcpy(dir, ".");
        base_pat = pattern;
    }

    DIR *d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.' && base_pat[0] != '.') continue;
        if (glob_match(base_pat, ent->d_name)) {
            char *buf = malloc(PATH_MAX_SH);
            if (!buf) break;
            if (slash) {
                snprintf(buf, PATH_MAX_SH, "%s/%s", dir, ent->d_name);
            } else {
                strncpy(buf, ent->d_name, PATH_MAX_SH - 1);
                buf[PATH_MAX_SH - 1] = '\0';
            }
            results[count++] = buf;
        }
    }
    closedir(d);
    return count;
}

/* ---- Tokenizer -------------------------------------------------- */

struct token {
    char *argv[MAX_ARGS];
    int   argc;
    char *redir_in;
    char *redir_out;
    char *redir_err;
    int   append_out;
    int   background;
};

static int tokenize_command(char *line, struct token *stages, int max_stages) {
    int nstages = 0;
    char *seg = line;

    while (seg && *seg && nstages < max_stages) {
        struct token *t = &stages[nstages];
        memset(t, 0, sizeof(*t));

        char *pipe_pos = NULL;
        int in_sq = 0, in_dq = 0;
        for (char *p = seg; *p; p++) {
            if (*p == '\'' && !in_dq) in_sq = !in_sq;
            else if (*p == '"' && !in_sq) in_dq = !in_dq;
            else if (!in_sq && !in_dq && *p == '|') {
                pipe_pos = p;
                break;
            }
        }

        char *end = pipe_pos ? pipe_pos : seg + strlen(seg);
        char saved = *end;
        *end = '\0';

        /* Parse redirects and arguments from this segment */
        char *s = seg;
        while (*s) {
            while (*s == ' ' || *s == '\t') s++;
            if (!*s) break;

            if (*s == '>' && s[1] == '>') {
                s += 2;
                while (*s == ' ') s++;
                t->redir_out = s;
                t->append_out = 1;
                while (*s && *s != ' ' && *s != '\t') s++;
                if (*s) *s++ = '\0';
            } else if (*s == '>') {
                s++;
                if (*s == '&') { s++; /* 2>&1 style - ignore for now */ continue; }
                while (*s == ' ') s++;
                t->redir_out = s;
                t->append_out = 0;
                while (*s && *s != ' ' && *s != '\t') s++;
                if (*s) *s++ = '\0';
            } else if (*s == '<') {
                s++;
                while (*s == ' ') s++;
                t->redir_in = s;
                while (*s && *s != ' ' && *s != '\t') s++;
                if (*s) *s++ = '\0';
            } else if (*s == '2' && s[1] == '>') {
                s += 2;
                while (*s == ' ') s++;
                t->redir_err = s;
                while (*s && *s != ' ' && *s != '\t') s++;
                if (*s) *s++ = '\0';
            } else if (*s == '&' && !s[1]) {
                t->background = 1;
                s++;
            } else {
                /* Regular argument - handle quotes */
                char *arg_start = s;
                char *w = s;
                while (*s && *s != ' ' && *s != '\t') {
                    if (*s == '\'' ) {
                        s++;
                        while (*s && *s != '\'') *w++ = *s++;
                        if (*s == '\'') s++;
                    } else if (*s == '"') {
                        s++;
                        while (*s && *s != '"') *w++ = *s++;
                        if (*s == '"') s++;
                    } else {
                        *w++ = *s++;
                    }
                }
                if (*s) s++;
                *w = '\0';
                if (t->argc < MAX_ARGS - 1)
                    t->argv[t->argc++] = arg_start;
            }
        }
        t->argv[t->argc] = NULL;
        nstages++;

        if (pipe_pos) {
            *end = saved;
            seg = pipe_pos + 1;
        } else {
            seg = NULL;
        }
    }
    return nstages;
}

/* ---- Builtins --------------------------------------------------- */

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
    char buf[PATH_MAX_SH];
    if (getcwd(buf, sizeof(buf))) {
        printf("%s\n", buf);
        return 0;
    }
    return 1;
}

static int builtin_export(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            var_set(argv[i], eq + 1);
            *eq = '=';
        }
    }
    return 0;
}

static int builtin_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        var_unset(argv[i]);
    return 0;
}

static int builtin_echo(int argc, char **argv) {
    int newline = 1;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { newline = 0; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        fputs(argv[i], stdout);
    }
    if (newline) putchar('\n');
    fflush(stdout);
    return 0;
}

static int builtin_test(int argc, char **argv) {
    if (argc < 2) return 1;
    if (argc == 2) return argv[1][0] ? 0 : 1;

    if (argc >= 4 && strcmp(argv[2], "=") == 0)
        return strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "!=") == 0)
        return strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "-eq") == 0)
        return atoi(argv[1]) == atoi(argv[3]) ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "-ne") == 0)
        return atoi(argv[1]) != atoi(argv[3]) ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "-lt") == 0)
        return atoi(argv[1]) < atoi(argv[3]) ? 0 : 1;
    if (argc >= 4 && strcmp(argv[2], "-gt") == 0)
        return atoi(argv[1]) > atoi(argv[3]) ? 0 : 1;
    if (argc >= 3 && strcmp(argv[1], "-f") == 0) {
        struct stat st;
        return (stat(argv[2], &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
        struct stat st;
        return (stat(argv[2], &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        struct stat st;
        return stat(argv[2], &st) == 0 ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "-z") == 0)
        return argv[2][0] == '\0' ? 0 : 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0)
        return argv[2][0] != '\0' ? 0 : 1;

    return 1;
}

static int builtin_history(void) {
    int start = g_hist_count > MAX_HISTORY ? g_hist_count - MAX_HISTORY : 0;
    for (int i = start; i < g_hist_count; i++)
        printf("%4d  %s\n", i + 1, g_history[i % MAX_HISTORY]);
    return 0;
}

static int try_builtin(struct token *t) {
    if (t->argc == 0) return -1;
    char *cmd = t->argv[0];

    if (strcmp(cmd, "cd") == 0) return builtin_cd(t->argc, t->argv);
    if (strcmp(cmd, "pwd") == 0) return builtin_pwd();
    if (strcmp(cmd, "export") == 0) return builtin_export(t->argc, t->argv);
    if (strcmp(cmd, "unset") == 0) return builtin_unset(t->argc, t->argv);
    if (strcmp(cmd, "echo") == 0) return builtin_echo(t->argc, t->argv);
    /* `exit [n]` TAKES A STATUS. It returned 0 no matter what was asked, so
     * `sh -c 'exit 33'` exited 0 and every caller that checked the status of
     * a sub-shell got the wrong answer -- including the real bash in the
     * initrd, which is the oracle the conformance gate compares against. With
     * no operand the status is that of the last command, as POSIX says. */
    if (strcmp(cmd, "exit") == 0) {
        g_running = 0;
        return (t->argc > 1) ? atoi(t->argv[1]) & 0xff : g_last_status;
    }
    if (strcmp(cmd, "true") == 0) return 0;
    if (strcmp(cmd, "false") == 0) return 1;
    if (strcmp(cmd, "test") == 0 || strcmp(cmd, "[") == 0)
        return builtin_test(t->argc, t->argv);
    if (strcmp(cmd, "history") == 0) return builtin_history();
    return -1;
}

/* ---- PATH resolution -------------------------------------------- */

static int resolve_path(const char *cmd, char *out, int outsz) {
    if (strchr(cmd, '/')) {
        strncpy(out, cmd, (size_t)(outsz - 1));
        out[outsz - 1] = '\0';
        struct stat st;
        return stat(out, &st) == 0 ? 0 : -1;
    }
    const char *path = var_get("PATH");
    if (!path) path = "/bin";

    while (*path) {
        const char *colon = strchr(path, ':');
        int plen = colon ? (int)(colon - path) : (int)strlen(path);
        if (plen + 1 + (int)strlen(cmd) + 1 < outsz) {
            memcpy(out, path, (size_t)plen);
            out[plen] = '/';
            strcpy(out + plen + 1, cmd);
            struct stat st;
            if (stat(out, &st) == 0) return 0;
        }
        path += plen;
        if (*path == ':') path++;
    }
    return -1;
}

/* ---- Execute a single pipeline ---------------------------------- */

static int exec_pipeline(struct token *stages, int nstages) {
    if (nstages == 0) return 0;

    /* Single command - try builtin first */
    if (nstages == 1 && !stages[0].redir_in && !stages[0].redir_out) {
        int rc = try_builtin(&stages[0]);
        if (rc >= 0) return rc;
    }

    pid_t pids[MAX_PIPES];
    int prev_read_fd = -1;

    for (int i = 0; i < nstages; i++) {
        struct token *t = &stages[i];
        if (t->argc == 0) continue;

        /* For single-stage builtins with redirects, still try builtin */
        if (nstages == 1) {
            int rc = try_builtin(t);
            if (rc >= 0) return rc;
        }

        int pipe_fds[2] = {-1, -1};
        if (i < nstages - 1) {
            if (pipe(pipe_fds) < 0) {
                fprintf(stderr, "sh: pipe failed\n");
                return 1;
            }
        }

        /* Determine fd0/fd1/fd2 for spawn */
        int child_fd0 = 0, child_fd1 = 0, child_fd2 = 0;

        if (prev_read_fd >= 0)
            child_fd0 = prev_read_fd;
        if (t->redir_in) {
            int rfd = open(t->redir_in, O_RDONLY);
            if (rfd < 0) {
                fprintf(stderr, "sh: %s: %s\n", t->redir_in, strerror(errno));
                if (prev_read_fd >= 0) close(prev_read_fd);
                if (pipe_fds[0] >= 0) { close(pipe_fds[0]); close(pipe_fds[1]); }
                return 1;
            }
            child_fd0 = rfd;
        }

        if (pipe_fds[1] >= 0)
            child_fd1 = pipe_fds[1];
        if (t->redir_out) {
            int flags = O_WRONLY | O_CREAT | (t->append_out ? O_APPEND : O_TRUNC);
            int wfd = open(t->redir_out, flags, 0644);
            if (wfd < 0) {
                fprintf(stderr, "sh: %s: %s\n", t->redir_out, strerror(errno));
                if (prev_read_fd >= 0) close(prev_read_fd);
                if (pipe_fds[0] >= 0) { close(pipe_fds[0]); close(pipe_fds[1]); }
                return 1;
            }
            child_fd1 = wfd;
        }

        if (t->redir_err) {
            int efd = open(t->redir_err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (efd >= 0) child_fd2 = efd;
        }

        /* Resolve command path */
        char path[PATH_MAX_SH];
        if (resolve_path(t->argv[0], path, sizeof(path)) < 0) {
            fprintf(stderr, "sh: %s: command not found\n", t->argv[0]);
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipe_fds[0] >= 0) { close(pipe_fds[0]); close(pipe_fds[1]); }
            if (child_fd0 > 0 && child_fd0 != prev_read_fd) close(child_fd0);
            if (child_fd1 > 0 && child_fd1 != pipe_fds[1]) close(child_fd1);
            if (child_fd2 > 0) close(child_fd2);
            return 127;
        }

        pid_t pid = toby_spawn(path, t->argv, environ,
                               child_fd0, child_fd1, child_fd2);

        /* Close fds we passed to child */
        if (child_fd0 > 0) close(child_fd0);
        if (child_fd1 > 0) close(child_fd1);
        if (child_fd2 > 0) close(child_fd2);

        if (pid < 0) {
            fprintf(stderr, "sh: %s: spawn failed: %s\n", t->argv[0], strerror(errno));
            if (pipe_fds[0] >= 0) close(pipe_fds[0]);
            return 1;
        }

        pids[i] = pid;
        prev_read_fd = pipe_fds[0];
    }

    /* Wait for all children */
    int last_status = 0;
    for (int i = 0; i < nstages; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (i == nstages - 1) {
            if (WIFEXITED(status))
                last_status = WEXITSTATUS(status);
            else
                last_status = 128;
        }
    }
    return last_status;
}

/* ---- Scripting: if/while/for ------------------------------------ */

/* Simple line-at-a-time script execution. We buffer lines for
 * control structures and execute them as blocks. */

static int execute_line(char *line);

struct script_block {
    char lines[256][MAX_LINE];
    int  count;
};

static int exec_if(FILE *fp, char *first_line) {
    /* first_line = "if <condition>" */
    char *cond = first_line + 3;
    while (*cond == ' ') cond++;

    /* Execute condition */
    int cond_result = execute_line(cond);
    int in_else = 0, found_then = 0;
    int should_exec = (cond_result == 0);
    int done = 0;
    char line[MAX_LINE];

    while (!done && fgets(line, MAX_LINE, fp)) {
        int len = (int)strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strncmp(trimmed, "then", 4) == 0 &&
            (trimmed[4] == '\0' || trimmed[4] == ' ')) {
            found_then = 1;
            continue;
        }
        if (strncmp(trimmed, "elif", 4) == 0 && found_then) {
            if (!should_exec && !in_else) {
                char *econd = trimmed + 5;
                while (*econd == ' ') econd++;
                should_exec = (execute_line(econd) == 0);
            } else {
                should_exec = 0;
            }
            continue;
        }
        if (strncmp(trimmed, "else", 4) == 0 &&
            (trimmed[4] == '\0' || trimmed[4] == ' ')) {
            in_else = 1;
            should_exec = (cond_result != 0);
            continue;
        }
        if (strncmp(trimmed, "fi", 2) == 0 &&
            (trimmed[2] == '\0' || trimmed[2] == ' ')) {
            done = 1;
            break;
        }
        if (found_then && should_exec) {
            g_last_status = execute_line(trimmed);
        }
    }
    return g_last_status;
}

static int exec_while(FILE *fp, char *first_line) {
    char *cond = first_line + 6;
    while (*cond == ' ') cond++;

    char body[64][MAX_LINE];
    int body_count = 0;
    int found_do = 0;
    char line[MAX_LINE];

    while (fgets(line, MAX_LINE, fp)) {
        int len = (int)strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strncmp(trimmed, "do", 2) == 0 &&
            (trimmed[2] == '\0' || trimmed[2] == ' ')) {
            found_do = 1;
            continue;
        }
        if (strncmp(trimmed, "done", 4) == 0 &&
            (trimmed[4] == '\0' || trimmed[4] == ' ')) {
            break;
        }
        if (found_do && body_count < 64) {
            strncpy(body[body_count], trimmed, MAX_LINE - 1);
            body[body_count][MAX_LINE - 1] = '\0';
            body_count++;
        }
    }

    int iterations = 0;
    while (iterations < 10000) {
        if (execute_line((char *)cond) != 0) break;
        for (int i = 0; i < body_count; i++)
            g_last_status = execute_line(body[i]);
        iterations++;
    }
    return g_last_status;
}

static int exec_for(FILE *fp, char *first_line) {
    /* for VAR in word1 word2 ... */
    char *p = first_line + 4;
    while (*p == ' ') p++;
    char varname[64];
    int vi = 0;
    while (*p && *p != ' ' && vi < 63) varname[vi++] = *p++;
    varname[vi] = '\0';

    while (*p == ' ') p++;
    if (strncmp(p, "in", 2) == 0) p += 2;
    while (*p == ' ') p++;

    /* Collect words */
    char *words[MAX_ARGS];
    int nwords = 0;
    while (*p && nwords < MAX_ARGS - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        words[nwords] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        nwords++;
    }

    /* Collect body */
    char body[64][MAX_LINE];
    int body_count = 0;
    int found_do = 0;
    char line[MAX_LINE];

    while (fgets(line, MAX_LINE, fp)) {
        int len = (int)strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strncmp(trimmed, "do", 2) == 0 &&
            (trimmed[2] == '\0' || trimmed[2] == ' ')) {
            found_do = 1;
            continue;
        }
        if (strncmp(trimmed, "done", 4) == 0 &&
            (trimmed[4] == '\0' || trimmed[4] == ' ')) {
            break;
        }
        if (found_do && body_count < 64) {
            strncpy(body[body_count], trimmed, MAX_LINE - 1);
            body[body_count][MAX_LINE - 1] = '\0';
            body_count++;
        }
    }

    (void)found_do;
    for (int w = 0; w < nwords; w++) {
        var_set(varname, words[w]);
        for (int i = 0; i < body_count; i++)
            g_last_status = execute_line(body[i]);
    }
    return g_last_status;
}

/* ---- Main execute function -------------------------------------- */

static FILE *g_script_fp = NULL;

static int execute_line(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return 0;

    /* Variable assignment: VAR=value */
    char *eq = strchr(line, '=');
    if (eq && eq != line) {
        int is_assign = 1;
        for (char *c = line; c < eq; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '_')) {
                is_assign = 0;
                break;
            }
        }
        if (is_assign && (eq == line + (int)strlen(line) - 1 ||
                         eq[1] == '\0' || eq > line)) {
            /* Check there's no space before = and no command after */
            char *after = eq + 1;
            /* Find end of value */
            char *end = after;
            while (*end && *end != ' ' && *end != '\t') end++;
            if (*end == '\0' || strchr(after, ' ') == NULL) {
                *eq = '\0';
                char expanded[MAX_LINE];
                expand_vars(eq + 1, expanded, MAX_LINE);
                var_set(line, expanded);
                return 0;
            }
        }
    }

    /* Expand variables */
    char expanded[MAX_LINE];
    expand_vars(line, expanded, MAX_LINE);

    /* Check for control structures */
    if (strncmp(expanded, "if ", 3) == 0) {
        return exec_if(g_script_fp ? g_script_fp : stdin, expanded);
    }
    if (strncmp(expanded, "while ", 6) == 0) {
        return exec_while(g_script_fp ? g_script_fp : stdin, expanded);
    }
    if (strncmp(expanded, "for ", 4) == 0) {
        return exec_for(g_script_fp ? g_script_fp : stdin, expanded);
    }

    /* source / . command */
    if (strncmp(expanded, "source ", 7) == 0 || strncmp(expanded, ". ", 2) == 0) {
        char *script = expanded + (expanded[0] == '.' ? 2 : 7);
        while (*script == ' ') script++;
        FILE *f = fopen(script, "r");
        if (!f) { fprintf(stderr, "sh: %s: %s\n", script, strerror(errno)); return 1; }
        FILE *old_fp = g_script_fp;
        g_script_fp = f;
        char sline[MAX_LINE];
        while (fgets(sline, MAX_LINE, f)) {
            int slen = (int)strlen(sline);
            if (slen > 0 && sline[slen-1] == '\n') sline[--slen] = '\0';
            g_last_status = execute_line(sline);
        }
        fclose(f);
        g_script_fp = old_fp;
        return g_last_status;
    }

    /* Glob-expand arguments, tokenize, and execute pipeline */
    struct token stages[MAX_PIPES];
    int nstages = tokenize_command(expanded, stages, MAX_PIPES);
    if (nstages == 0) return 0;

    /* Glob expand each stage's arguments */
    for (int s = 0; s < nstages; s++) {
        struct token *t = &stages[s];
        char *new_argv[MAX_ARGS];
        int new_argc = 0;

        for (int a = 0; a < t->argc && new_argc < MAX_ARGS - 1; a++) {
            if (has_glob(t->argv[a])) {
                char *results[MAX_GLOB];
                int n = do_glob(t->argv[a], results, MAX_GLOB);
                if (n > 0) {
                    for (int g = 0; g < n && new_argc < MAX_ARGS - 1; g++)
                        new_argv[new_argc++] = results[g];
                } else {
                    new_argv[new_argc++] = t->argv[a];
                }
            } else {
                new_argv[new_argc++] = t->argv[a];
            }
        }
        new_argv[new_argc] = NULL;
        memcpy(t->argv, new_argv, sizeof(char *) * (size_t)(new_argc + 1));
        t->argc = new_argc;
    }

    return exec_pipeline(stages, nstages);
}

/* ---- Interactive line reading ----------------------------------- */

static int read_line(char *buf, int bufsz) {
    int pos = 0;
    while (pos < bufsz - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) return pos > 0 ? pos : -1;
        if (c == '\n' || c == '\r') {
            buf[pos] = '\0';
            if (g_interactive) write(STDOUT_FILENO, "\n", 1);
            return pos;
        }
        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                if (g_interactive) write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if (c == 0x03) { /* Ctrl+C */
            buf[0] = '\0';
            if (g_interactive) write(STDOUT_FILENO, "^C\n", 3);
            return 0;
        }
        if (c == 0x04 && pos == 0) { /* Ctrl+D at empty line */
            return -1;
        }
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
            buf[pos++] = c;
            if (g_interactive) write(STDOUT_FILENO, &c, 1);
        }
    }
    buf[pos] = '\0';
    return pos;
}

static void print_prompt(void) {
    char cwd[PATH_MAX_SH];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");

    /* Shorten home-relative paths */
    const char *display = cwd;
    const char *home = var_get("HOME");
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        static char short_cwd[PATH_MAX_SH];
        short_cwd[0] = '~';
        strcpy(short_cwd + 1, cwd + strlen(home));
        display = short_cwd;
    }

    printf("%s$ ", display);
    fflush(stdout);
}

/* ---- Main ------------------------------------------------------- */

int main(int argc, char **argv) {
    g_mypid = getpid();

    /* Set default variables */
    var_set("PATH", "/bin");
    var_set("HOME", "/");
    var_set("SHELL", "/bin/sh");

    /* -c "command" mode */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        char line[MAX_LINE];
        strncpy(line, argv[2], MAX_LINE - 1);
        line[MAX_LINE - 1] = '\0';
        g_last_status = execute_line(line);
        return g_last_status;
    }

    /* Script file mode */
    if (argc >= 2 && argv[1][0] != '-') {
        FILE *f = fopen(argv[1], "r");
        if (!f) {
            fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        g_script_fp = f;
        char line[MAX_LINE];
        while (fgets(line, MAX_LINE, f)) {
            int len = (int)strlen(line);
            if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
            g_last_status = execute_line(line);
            if (!g_running) break;
        }
        fclose(f);
        return g_last_status;
    }

    /* Interactive mode */
    g_interactive = isatty(STDIN_FILENO);

    if (g_interactive) {
        printf("tobyOS sh v1.0\n");
    }

    char line[MAX_LINE];
    while (g_running) {
        if (g_interactive) print_prompt();

        int n = read_line(line, sizeof(line));
        if (n < 0) break;
        if (n == 0) continue;

        hist_add(line);
        g_last_status = execute_line(line);
    }

    return g_last_status;
}
