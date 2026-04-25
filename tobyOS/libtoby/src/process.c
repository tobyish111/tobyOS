/* libtoby/src/process.c -- waitpid + posix_spawn-style wrapper +
 * the POSIX exec family + system(). Without fork() we model exec as
 * synchronous "spawn-then-wait-then-exit", which is opaque to anyone
 * who waits on this pid: from the parent's view, the calling process
 * "becomes" the child the same way an exec on Linux would.
 *
 * The kernel exposes spawn as a single syscall taking
 * struct abi_spawn_req; this file reuses the libtoby wrappers that
 * surround it so user programs can compose the more idiomatic
 * posix_spawn() flow without touching the abi header. */

#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>
#include "libtoby_internal.h"

pid_t waitpid(pid_t pid, int *status_out, int flags) {
    long rv = toby_sc3(ABI_SYS_WAITPID, pid,
                       (long)(uintptr_t)status_out, flags);
    return (pid_t)__toby_check(rv);
}

/* Bonus: a tiny posix_spawn-flavoured helper. Not exported via a
 * header in M25B (M25C will add a proper <spawn.h>); samples and
 * future ports include the prototype themselves if they need it. */
pid_t toby_spawn(const char *path, char *const argv[], char *const envp[],
                 int fd0, int fd1, int fd2) {
    struct abi_spawn_req req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .fd0  = fd0,
        .fd1  = fd1,
        .fd2  = fd2,
        .flags = 0,
    };
    long rv = toby_sc1(ABI_SYS_SPAWN, (long)(uintptr_t)&req);
    return (pid_t)__toby_check(rv);
}

/* ============================================================
 *  Exec emulation (Milestone 25C)
 * ============================================================ */

/* spawn-then-wait-then-exit. Returns -1 with errno set on failure;
 * otherwise never returns (the synchronous "exec" semantics). */
static int exec_emulate(const char *path, char *const argv[],
                        char *const envp[]) {
    pid_t pid = toby_spawn(path, argv, envp, 0, 0, 0);
    if (pid < 0) return -1;
    int status = 0;
    pid_t rc = waitpid(pid, &status, 0);
    if (rc < 0) return -1;
    /* Mirror the child's outcome: exited normally -> _exit(code).
     * Anything else collapses to a non-zero exit so the parent can
     * still tell something went wrong (the kernel doesn't surface
     * signal info today). */
    if (WIFEXITED(status)) _exit(WEXITSTATUS(status));
    _exit(WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
    /* unreachable */
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    if (!path || !argv) { errno = EINVAL; return -1; }
    return exec_emulate(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

/* PATH-search for `file`, building "<dir>/<file>" candidates in
 * out_buf and stat()-checking each. Returns 0 on hit (out_buf holds
 * the resolved path), -1 if nothing matched. errno is set per
 * POSIX rules: ENOENT for "no candidate worked", E2BIG for buffer
 * overflow on the longest candidate. */
static int path_search(const char *file, char *out_buf, size_t out_sz) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    size_t flen = strlen(file);
    bool any_overflow = false;

    while (*path) {
        const char *colon = path;
        while (*colon && *colon != ':') colon++;
        size_t plen = (size_t)(colon - path);
        if (plen > 0) {
            bool need_slash = (path[plen - 1] != '/');
            size_t total = plen + (need_slash ? 1 : 0) + flen;
            if (total + 1 > out_sz) {
                any_overflow = true;
            } else {
                char *o = out_buf;
                memcpy(o, path, plen); o += plen;
                if (need_slash) *o++ = '/';
                memcpy(o, file, flen); o += flen;
                *o = '\0';
                struct stat st;
                if (stat(out_buf, &st) == 0 &&
                    (st.st_mode & S_IFMT) == S_IFREG) {
                    return 0;
                }
            }
        }
        path = colon;
        if (*path == ':') path++;
    }
    errno = any_overflow ? E2BIG : ENOENT;
    return -1;
}

int execvp(const char *file, char *const argv[]) {
    if (!file || !argv) { errno = EINVAL; return -1; }
    /* If the caller embedded a slash anywhere, treat it as an
     * explicit path (POSIX behaviour). */
    for (const char *c = file; *c; c++) {
        if (*c == '/') return execve(file, argv, environ);
    }
    char buf[256];
    if (path_search(file, buf, sizeof(buf)) < 0) return -1;
    return execve(buf, argv, environ);
}

/* ============================================================
 *  system() (Milestone 25C)
 * ============================================================ *
 *
 * tobyOS doesn't have a /bin/sh yet, so we emulate "sh -c" with an
 * inline whitespace tokenizer + execvp-style PATH search + waitpid.
 *
 * Returns:
 *   - if cmd is NULL: 1 (a "shell is available" probe; we always say yes)
 *   - on spawn or wait failure: -1, errno set
 *   - on success: the encoded status (use WIFEXITED/WEXITSTATUS to
 *     decode -- same shape as waitpid). */

#define SYSTEM_ARG_MAX 32

int system(const char *cmd) {
    if (!cmd) return 1;

    /* Duplicate cmd so we can tokenise in-place. */
    size_t clen = strlen(cmd);
    char *buf = (char *)malloc(clen + 1);
    if (!buf) { errno = ENOMEM; return -1; }
    memcpy(buf, cmd, clen + 1);

    char *argv[SYSTEM_ARG_MAX];
    int argc = 0;
    char *s = buf;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (argc >= SYSTEM_ARG_MAX - 1) {
            free(buf);
            errno = E2BIG;
            return -1;
        }
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = '\0';
    }
    argv[argc] = 0;
    if (argc == 0) { free(buf); return 0; }       /* empty command */

    /* PATH-resolve argv[0] explicitly so we can spawn with its full
     * path (the kernel's open() doesn't search). */
    char path_buf[256];
    const char *path = argv[0];
    bool has_slash = false;
    for (const char *c = argv[0]; *c; c++) if (*c == '/') { has_slash = true; break; }
    if (!has_slash) {
        if (path_search(argv[0], path_buf, sizeof(path_buf)) < 0) {
            free(buf);
            return -1;
        }
        path = path_buf;
    }

    pid_t pid = toby_spawn(path, argv, environ, 0, 0, 0);
    if (pid < 0) { free(buf); return -1; }
    int status = 0;
    pid_t rc = waitpid(pid, &status, 0);
    free(buf);
    if (rc < 0) return -1;
    return status;
}
