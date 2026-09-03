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

pid_t wait(int *wstatus) {
    return waitpid(-1, wstatus, 0);
}

pid_t fork(void) {
    return (pid_t)__toby_check(toby_sc0(ABI_SYS_FORK));
}

/* Bonus: a tiny posix_spawn-flavoured helper. Not exported via a
 * header in M25B (M25C will add a proper <spawn.h>); samples and
 * future ports include the prototype themselves if they need it. */
pid_t toby_spawn_ex(const char *path, char *const argv[], char *const envp[],
                    int fd0, int fd1, int fd2,
                    const struct abi_spawn_fd *extra, unsigned nextra);

pid_t toby_spawn(const char *path, char *const argv[], char *const envp[],
                 int fd0, int fd1, int fd2) {
    return toby_spawn_ex(path, argv, envp, fd0, fd1, fd2, 0, 0);
}

/* As toby_spawn, plus descriptors above 2. A shell needs them for
 * `exec 3>file` and `cmd 8<<EOF`; every other caller passes (0, 0) and gets
 * exactly the old behaviour. */
pid_t toby_spawn_ex(const char *path, char *const argv[], char *const envp[],
                    int fd0, int fd1, int fd2,
                    const struct abi_spawn_fd *extra, unsigned nextra) {
    struct abi_spawn_req req = {
        .path = path,
        .argv = argv,
        .envp = envp,
        .fd0  = fd0,
        .fd1  = fd1,
        .fd2  = fd2,
        .flags = 0,
        .nextra = nextra,
        .extra  = extra,
    };
    long rv = toby_sc1(ABI_SYS_SPAWN, (long)(uintptr_t)&req);
    return (pid_t)__toby_check(rv);
}

/* ============================================================
 *  exec (Milestone 25C, made real 2026-09-01)
 * ============================================================ */

/* THIS USED TO BE AN EMULATION, and the difference is not cosmetic.
 *
 * exec_emulate() spawned a NEW process, waited for it, and exited with its
 * status -- so the calling process survived as a wrapper and the program
 * ran as its CHILD. For a shell running a command that is close enough to
 * pass unnoticed, which is why it lasted. It is wrong for anything that
 * cares which process is which:
 *
 *   * A traced child that exec'd stayed a traced child sitting in waitpid
 *     while an UNTRACED grandchild ran the program. That is why the
 *     debugger could not put a breakpoint in anything -- found the hard
 *     way, over several boots, while the tracer looked like the broken
 *     part.
 *   * The pid changes, so anything holding the pre-exec pid (a supervisor,
 *     a pidfile, a `kill`) is now pointing at a wrapper.
 *   * Two processes exist where POSIX promises one, doubling the process
 *     table cost of every exec.
 *   * A setuid transition applies to the grandchild, not to the process
 *     the caller believes it is.
 *
 * ABI_SYS_EXECVE (142) has been dispatched in the kernel all along; the
 * header comment's premise ("without fork()") stopped being true when
 * fork landed. On success this does not return, because the image is
 * REPLACED. Descriptors survive it -- minus FD_CLOEXEC, which is the point
 * of that flag and which the emulation could not honour because it never
 * exec'd at all.
 */
int execve(const char *path, char *const argv[], char *const envp[]) {
    if (!path || !argv) { errno = EINVAL; return -1; }
    if (!envp) envp = environ;      /* execv/execvp pass environ; be safe */
    long rv = toby_sc3(ABI_SYS_EXECVE, (long)(uintptr_t)path,
                       (long)(uintptr_t)argv, (long)(uintptr_t)envp);
    /* Only reached on failure: a successful execve never comes back. */
    __toby_check(rv);
    return -1;
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

/* POSIX: execvp and execlp run a file the loader answers ENOEXEC for -- a
 * text file with no "#!" line -- through the shell, as [sh, path,
 * argv[1..]]. That shell is /bin/sh, as in glibc. (It was /bin/tsh by name
 * here while /bin/sh was still the minimal shell; /bin/sh IS tsh now.) */
static int execve_or_script(const char *path, char *const argv[]) {
    int rv = execve(path, argv, environ);
    if (errno != ENOEXEC) return rv;
    int argc = 0;
    while (argv[argc]) argc++;
    char **nargv = malloc((size_t)(argc + 3) * sizeof *nargv);
    if (!nargv) { errno = ENOMEM; return -1; }
    int n = 0;
    nargv[n++] = (char *)"/bin/sh";
    nargv[n++] = (char *)path;
    for (int i = 1; i < argc; i++) nargv[n++] = argv[i];
    nargv[n] = 0;
    rv = execve("/bin/sh", nargv, environ);
    free(nargv);
    return rv;
}

int execvp(const char *file, char *const argv[]) {
    if (!file || !argv) { errno = EINVAL; return -1; }
    /* If the caller embedded a slash anywhere, treat it as an
     * explicit path (POSIX behaviour). */
    for (const char *c = file; *c; c++) {
        if (*c == '/') return execve_or_script(file, argv);
    }
    char buf[256];
    if (path_search(file, buf, sizeof(buf)) < 0) return -1;
    return execve_or_script(buf, argv);
}

/* ============================================================
 *  system() -- POSIX: `sh -c cmd`, and the waitpid status back
 * ============================================================ *
 *
 * This ran `sh -c` only when it spotted a shell metacharacter, and
 * otherwise tokenised on whitespace and spawned the words itself --
 * because /bin/sh used to be a minimal shell nobody trusted, and the
 * tokeniser was the lesser evil. /bin/sh is tsh now (2026-09-03), so
 * every command gets the real parser: quoting, variables, redirection,
 * the lot. argv[0] is "sh", as glibc passes it, which is also what puts
 * tsh into POSIX mode.
 *
 * Returns:
 *   - if cmd is NULL: 1 (a "shell is available" probe)
 *   - on spawn or wait failure: -1, errno set
 *   - on success: the encoded status (WIFEXITED/WEXITSTATUS decode it,
 *     the same shape as waitpid) */

int system(const char *cmd) {
    if (!cmd) return 1;
    char *sh_argv[4];
    sh_argv[0] = (char *)"sh";
    sh_argv[1] = (char *)"-c";
    sh_argv[2] = (char *)cmd;
    sh_argv[3] = 0;
    pid_t pid = toby_spawn("/bin/sh", sh_argv, environ, 0, 0, 0);
    if (pid < 0) return -1;
    int status = 0;
    pid_t rc = waitpid(pid, &status, 0);
    if (rc < 0) return -1;
    return status;
}
