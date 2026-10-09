/* programs/tsh/host.c -- the userspace host for the tobyOS shell language.
 *
 * ONE LANGUAGE, TWO HOSTS
 * -----------------------
 * `src/shell.c` is compiled twice. In the kernel it is driven by the keyboard
 * poll loop and sits on the real subsystems (vfs, proc, pipe, heap). Here it
 * is driven by main() below and sits on libtoby syscall wrappers. The shell
 * LANGUAGE -- tokenizer, the ${...} expansion family, the recursive-descent
 * arithmetic evaluator, command substitution, globbing, functions, traps,
 * here-documents -- exists exactly once and therefore cannot drift.
 *
 * The alternative was writing the language a second time in userspace, and we
 * already know how that ends: `programs/sh` had a 1,297-line parser with
 * if/for/while/case and still scored 0/14 against real bash, because the
 * expansion layer underneath was never finished. The hard part of a shell is
 * not the grammar, it is everything the grammar delegates to.
 *
 * `src/shell.c` is NOT modified to make this work -- it gained three additive
 * entry points and nothing else, so the kernel shell's behaviour is unchanged
 * by construction. This file provides the other half of the seam.
 *
 * WHAT'S REAL AND WHAT'S NOT
 * --------------------------
 * The ~40 symbols the language core actually reaches for are implemented here
 * for real. The ~110 remaining undefined symbols belong to kernel-only
 * builtins (blkdump, ifconfig, pkg, gui, perf...) that cannot mean anything in
 * userspace; they live in kstubs.c and say so out loud when called, rather
 * than silently returning zero and letting a builtin appear to work.
 *
 * THE ONE ROUTING DECISION THAT DECIDES PARITY
 * --------------------------------------------
 * shell.c splits its output two ways: builtins emit normal output through
 * shell_write()/shell_printf() (which honour redirection, falling back to
 * kputc), and emit DIAGNOSTICS through kprintf(). So:
 *
 *     kputc   -> fd 1     (stdout: real program output)
 *     kprintf -> fd 2     (stderr: diagnostics)
 *
 * Get this backwards and every error message lands in stdout, where the
 * parity gate compares it against bash's clean stdout and fails a case for a
 * reason that has nothing to do with the language.
 */

#include <tobyos/shell.h>
#include <tobyos/vfs.h>
#include <tobyos/file.h>
#include <tobyos/pipe.h>
#include <tobyos/proc.h>
#include <tobyos/abi/abi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>

/* NOT <signal.h>: this file straddles the kernel headers (tobyos/signal.h
 * arrives via the shell/proc includes) and libtoby's, whose sigaction/
 * siginfo definitions collide. Declare the one libc entry point needed. */
typedef void (*tsh_sighandler_fn)(int);
extern tsh_sighandler_fn signal(int signum, tsh_sighandler_fn handler);
/* Same reason as signal() above: declared rather than included. This one
 * arms a handler WITHOUT SA_RESTART, which is what makes ^C at the prompt
 * observable -- see the comment at tsh_arm_sigint(). */
extern tsh_sighandler_fn signal_norestart(int signum, tsh_sighandler_fn handler);
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>

/* NOT <signal.h>: libtoby's and the kernel's <tobyos/signal.h> (pulled in by
 * proc.h) define sigaction/siginfo_t/ucontext differently, and shell.c needs
 * the kernel's. Only kill() is wanted here, so declare just that. */
extern int kill(pid_t pid, int sig);

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);
extern pid_t toby_spawn_ex(const char *path, char *const argv[],
                           char *const envp[], int fd0, int fd1, int fd2,
                           const struct abi_spawn_fd *extra, unsigned nextra);
extern pid_t toby_spawn_flags(const char *path, char *const argv[],
                              char *const envp[], int fd0, int fd1, int fd2,
                              const struct abi_spawn_fd *extra, unsigned nextra,
                              unsigned flags);
/* libtoby's, declared for the reason signal() is: its sigset_t is an
 * unsigned long with signal N at bit N; SIG_UNBLOCK is 1. */
extern int sigprocmask(int how, const unsigned long *set, unsigned long *old);

/* A blocking call that a signal interrupted (EINTR) asks the shell what
 * to do: act on a fatal signal the EXIT trap waits for (which does not
 * return), stop a `wait` a trapped signal ends (1), or go on (0). See
 * shell_host_eintr in src/shell.c. */
extern int shell_host_eintr(void);
uint64_t perf_now_ns(void);              /* below */

/* Two of this file's handlers must not restart what they interrupt: ^C's
 * and the hangup's at a terminal (tsh_arm_sigint). Putting one back must
 * keep that -- shell.c's own non-restarting one (the EXIT trap's fatal
 * signals) it puts back itself. */
static void tsh_sigint(int sig);
static void tsh_sighup(int sig);
bool shell_host_norestart_handler(tsh_sighandler_fn h) {
    return h == tsh_sigint || h == tsh_sighup;
}
static void host_sig_put(int sig, tsh_sighandler_fn h) {
    if (shell_host_norestart_handler(h)) (void)signal_norestart(sig, h);
    else (void)signal(sig, h);
}

/* The spawn request's ABI_SPAWN_F_* for a spec's sig_default/sig_ignore. */
static unsigned host_spawn_sigflags(const struct proc_spec *spec) {
    unsigned fl = 0;
    if (spec->sig_ignore & SIGMASK(SIGINT))        fl |= ABI_SPAWN_F_IGN_INT;
    else if (spec->sig_default & SIGMASK(SIGINT))  fl |= ABI_SPAWN_F_DFL_INT;
    if (spec->sig_ignore & SIGMASK(SIGQUIT))       fl |= ABI_SPAWN_F_IGN_QUIT;
    else if (spec->sig_default & SIGMASK(SIGQUIT)) fl |= ABI_SPAWN_F_DFL_QUIT;
    return fl;
}

/* Did the last proc_spawn fail because the arguments were too big? The
 * shell asks straight after the failure (shell_explain_spawn_failure);
 * proc_spawn leaves errno as the spawn syscall set it. */
int host_spawn_was_e2big(void) { return errno == E2BIG; }

/* argv and envp for a spawn or an exec, NULL-terminated, on the heap and
 * sized from the spec. They were 128-slot stack arrays that dropped the
 * rest without a word -- a program got its first 127 arguments. What bounds
 * a program's arguments now is the kernel (ABI_ARGS_TOTAL_MAX: E2BIG).
 * Returns 0 when memory ran out; the caller frees with free(). */
static char **host_strvec(char **v, int n) {
    char **out = (char **)malloc((size_t)(n + 1) * sizeof(char *));
    if (!out) return 0;
    for (int i = 0; i < n; i++) out[i] = v[i];
    out[n] = 0;
    return out;
}

/* ---- printk, and the sink that makes $( ) work --------------------------
 *
 * This is not decoration. shell.c captures the output of a BUILTIN inside
 * `$(...)` by installing a printk sink and running the command -- builtin
 * output reaches kputc, the sink intercepts it, and the captured text becomes
 * the substitution. The same mechanism redirects builtin output to a file.
 *
 * Stub these and `x=$(echo hi)` silently yields the empty string: the command
 * runs, prints nothing anyone collects, and the shell substitutes "". So the
 * sink is implemented for real, and kputc goes through it. */

static void (*g_sink)(void *ctx, char c);
static void  *g_sink_ctx;
static bool   g_sink_suppress;

void printk_set_sink_mode(void (*cb)(void *ctx, char c), void *ctx,
                          bool suppress) {
    g_sink = cb;
    g_sink_ctx = ctx;
    g_sink_suppress = suppress;
}

void printk_get_sink(void (**cb)(void *ctx, char c), void **ctx,
                     bool *suppress) {
    if (cb)       *cb       = g_sink;
    if (ctx)      *ctx      = g_sink_ctx;
    if (suppress) *suppress = g_sink_suppress;
}

/* A DIAGNOSTIC DOES NOT PUSH THE BUFFERED STDOUT OUT AHEAD OF IT. libtoby
 * writes pending stdout before every syscall (K5); bash's stdio does not,
 * so within one builtin bash's error lines come BEFORE the output the
 * builtin had buffered -- `printf '%d %d\n' 1 x >file 2>&1` is "printf: x:
 * invalid number" and then "1 0" (logs/b13 q53). The shell's own
 * diagnostics are written straight to the kernel; stdout still goes out at
 * the end of the builtin, as bash's does. */
static bool g_diag_nosync;
static long host_write_nosync(int fd, const void *buf, size_t n) {
    extern long toby_sc_raw3(long num, long a, long b, long c);
    long r = toby_sc_raw3(1 /* ABI_SYS_WRITE */, fd, (long)(uintptr_t)buf, (long)n);
    if (r < 0) {
        errno = (unsigned long)r > (unsigned long)-4096L ? (int)-r : EIO;
        return -1;
    }
    return r;
}

/* STDOUT IS BUFFERED, AS BASH'S IS (S2, 2026-09-30). This was
 * write(1, &c, 1): one syscall per character of every builtin's output,
 * so two shells printing at once interleaved INSIDE words -- 28 of 30 runs
 * of `tsh -c 'echo word_a & echo word_b'` printed things like
 * "wwoorrdd__ba", where bash, whose echo is one write, never splits a word.
 * Now the bytes go through libtoby's stdout (line-buffered on a terminal,
 * fully buffered otherwise), and shell_host_builtin_done() flushes it at
 * the end of every builtin -- bash flushes stdout after each builtin the
 * same way. libtoby also writes pending output out before ANY syscall
 * (K5), so a spawn, a pipe, a read of the next line or a diagnostic on
 * fd 2 all still see every byte printed before them. */
void kputc(char c) {
    if (g_sink) {
        g_sink(g_sink_ctx, c);
        if (g_sink_suppress) return;   /* captured INSTEAD of printed */
    }
    (void)fputc((unsigned char)c, stdout);
}

void shell_host_builtin_done(void) {
    (void)fflush(stdout);
}

/* BASH'S initialize_job_control (jobs.c), for an interactive shell or one
 * given -m. Find the terminal -- fd 2's, or /dev/tty's when -i was forced
 * and fd 2 is not one -- wait (SIGTTIN to its own group) until the shell's
 * group is that terminal's foreground, lead a group of its own and make it
 * the foreground. Where a step fails the shell says so in bash's words and
 * has no job control:
 *
 *     setsid bash -i < /dev/null 2>&1 | cat
 *     bash: cannot set terminal process group (-1): Not a tty
 *     bash: no job control in this shell             ($- has no m)
 *
 * /bin/tsh looked at no terminal, kept job control on whatever it had, and
 * never led its own group (finding 61). NAME is $0, which is how bash
 * names itself in these. Returns whether job control is on. */
/* TIOCSPGRP with SIGTTOU, SIGTTIN, SIGTSTP and SIGCHLD blocked, as bash's
 * give_terminal_to: a change to the terminal from the background sends
 * SIGTTOU (pty.c pty_job_check), and the shell is in the background
 * exactly when it takes the terminal back from a job -- and when it first
 * leads its own group. Blocked, the signal lets the call through; caught,
 * its handler would restart the call into a second one, and a login
 * shell's group is orphaned, which makes it EIO. */
static int tsh_give_terminal(int fd, int pg) {
    unsigned long m = (1UL << SIGTTOU) | (1UL << SIGTTIN) |
                      (1UL << SIGTSTP) | (1UL << SIGCHLD), old = 0;
    (void)sigprocmask(SIG_BLOCK, &m, &old);
    int rc = ioctl(fd, 0x5410UL /* TIOCSPGRP */, &pg);
    int e = errno;
    (void)sigprocmask(SIG_SETMASK, &old, 0);
    errno = e;
    return rc;
}

static bool tsh_init_job_control(const char *name, bool forced_interactive) {
    int tty = -1;
    if (forced_interactive && !isatty(2)) tty = open("/dev/tty", O_RDWR);
    if (tty < 0) tty = dup(2);
    int shell_pgrp = (int)getpgrp();
    int terminal_pgrp = -1, t_errno = -1, tty_sigs = 0;
    bool jc = false;
    for (;;) {
        int pg = 0;
        if (tty < 0 || ioctl(tty, 0x540FUL /* TIOCGPGRP */, &pg) != 0) {
            terminal_pgrp = -1;
            break;
        }
        terminal_pgrp = pg;
        if (shell_pgrp == terminal_pgrp) break;
        signal(SIGTTIN, SIG_DFL);
        (void)kill(0, SIGTTIN);
        if (tty_sigs++ > 16) {
            fprintf(stderr, "%s: initialize_job_control: no job control in background: %s\n",
                    name, strerror(errno));
            if (tty >= 0) close(tty);
            return false;
        }
    }
    if (terminal_pgrp == -1) t_errno = errno;
    int original = shell_pgrp;
    shell_pgrp = (int)getpid();
    if (original != shell_pgrp && setpgid(0, shell_pgrp) < 0) {
        fprintf(stderr, "%s: initialize_job_control: setpgid: %s\n", name, strerror(errno));
        shell_pgrp = original;
    }
    jc = true;
    if (shell_pgrp != original && shell_pgrp != terminal_pgrp) {
        if (tty < 0 || tsh_give_terminal(tty, shell_pgrp) != 0) {
            int e = errno;
            (void)setpgid(0, original);
            shell_pgrp = original;
            fprintf(stderr, "%s: cannot set terminal process group (%d): %s\n",
                    name, shell_pgrp, strerror(e));
            jc = false;
        }
    }
    if (jc) {
        int t = -1;
        if (tty < 0 || ioctl(tty, 0x540FUL /* TIOCGPGRP */, &t) != 0) t = -1;
        if (t == -1 || t != shell_pgrp) {
            int e = (t_errno != -1) ? t_errno : errno;
            fprintf(stderr, "%s: cannot set terminal process group (%d): %s\n",
                    name, t, strerror(e));
            jc = false;
        }
    }
    if (!jc) fprintf(stderr, "%s: no job control in this shell\n", name);
    if (tty >= 0) close(tty);
    return jc;
}

/* bash's setup_async_signals: an asynchronous command started without job
 * control ignores SIGINT and SIGQUIT -- the keyboard's signals belong to
 * the foreground -- and so does the program it execs, since an ignore
 * survives exec. tsh's async children died of `kill -INT $!` (logs/b14
 * r11). */
void shell_host_async_signals(void) {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
}

void kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    /* ...THROUGH THE SHELL'S fd 2, when it has one. A redirection replaces an
     * entry in the shell's own descriptor table, not this process's fd 2, so
     * `{ cmd ; } 2> err.txt` used to leave the shell's diagnostics going to
     * the terminal while the script counted the lines it thought it had
     * captured. shell.c hands back whatever is currently installed. */
    {
        extern struct file *shell_current_diag_file(void);
        struct file *diag = shell_current_diag_file();
        if (diag) {
            g_diag_nosync = true;
            (void)file_write(diag, buf, (size_t)n);
            g_diag_nosync = false;
            return;
        }
    }
    /* diagnostics -> stderr (see header), not flushing stdout first (see
     * file_write) */
    (void)host_write_nosync(2, buf, (size_t)n);
}

int ksnprintf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}

int kvsnprintf(char *dst, size_t cap, const char *fmt, va_list ap) {
    return vsnprintf(dst, cap, fmt, ap);
}

void kpanic_at(const char *file, int line, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "tsh: fatal at %s:%d: %s\n", file, line, buf);
    exit(70);
}

/* ---- heap --------------------------------------------------------------- */

void *kmalloc(size_t n)            { return malloc(n); }
void *kcalloc(size_t n, size_t sz) { return calloc(n, sz); }
void  kfree(void *p)               { free(p); }

/* ---- struct file --------------------------------------------------------
 *
 * The first cut of this assumed shell.c treated `struct file *` as opaque and
 * wrapped it in a host-side struct. It does not: shell_open_vfs_file()
 * kmallocs a REAL `struct file`, sets kind = FILE_KIND_VFS, allocates the
 * vfs_refs refcount and fills ->vfs via vfs_open(). Casting that to a larger
 * wrapper read past the allocation and dereferenced whatever followed it --
 * two GP faults in the parity run, on exactly the two cases that redirect to
 * a file.
 *
 * So the host uses the kernel's own representation instead of inventing one:
 * the descriptor lives in f->vfs.priv (where this file's vfs_open puts it),
 * sharing is counted in f->vfs_refs (where shell.c puts it), and the offset
 * is f->vfs.pos -- which shell.c SETS to ->size to implement `>>`, so honour
 * it with lseek rather than letting the kernel fd's own offset decide.
 */

#define VF_FD(vf)      ((int)(long)(vf)->priv)
#define VF_SET(vf, fd) ((vf)->priv = (void *)(long)(fd))

static struct file *hf_make(int fd) {
    struct file *f = calloc(1, sizeof *f);
    if (!f) return 0;
    f->kind = FILE_KIND_VFS;
    f->vfs_refs = malloc(sizeof(int));
    if (!f->vfs_refs) { free(f); return 0; }
    *f->vfs_refs = 1;
    VF_SET(&f->vfs, fd);
    return f;
}

struct file *file_clone(struct file *src) {
    if (!src) return 0;
    struct file *f = calloc(1, sizeof *f);
    if (!f) return 0;
    *f = *src;
    if (f->vfs_refs) (*f->vfs_refs)++;
    return f;
}

void file_close(struct file *f) {
    if (!f) return;
    if (f->kind == FILE_KIND_VFS) {
        bool last = !f->vfs_refs || --(*f->vfs_refs) == 0;
        if (last) {
            int fd = VF_FD(&f->vfs);
            /* 0/1/2 are this process's own standard descriptors: closing one
             * because a redirect went out of scope would silence the shell. */
            if (fd > 2) close(fd);
            free(f->vfs_refs);
        }
    }
    free(f);
}

/* Pipes and the standard descriptors have no meaningful offset; only real
 * files do. pipe_create() stamps the sentinel below on both ends. */
#define VF_UNSEEKABLE ((size_t)-1)

static bool hf_seekable(const struct file *f, int fd) {
    return fd > 2 && f->vfs.size != VF_UNSEEKABLE;
}

long file_read(struct file *f, void *buf, size_t n) {
    if (!f) return -1;
    /* `<&-` CLOSED it: a read fails, as bash's does -- `read` says "read
     * error: 0: Bad file descriptor" and `mapfile` stores nothing (logs/b12
     * p42, p73). It read as an empty input. */
    if (f->kind == FILE_KIND_NULL) return -EBADF;
    if (f->kind != FILE_KIND_VFS) return -1;
    int fd = VF_FD(&f->vfs);
    long got = read(fd, buf, n);
    /* An interruption is not an error: the shell acts on a fatal signal
     * the EXIT trap waits for (shell_host_eintr does not return then), and
     * the read goes on -- `read` reported "Interrupted system call". */
    while (got < 0 && errno == EINTR) {
        (void)shell_host_eintr();
        got = read(fd, buf, n);
    }
    if (got > 0) f->vfs.pos += (size_t)got;
    /* the errno, negated, for `read`'s "read error: FD: REASON" -- a
     * directory is EISDIR */
    return got < 0 ? -(long)errno : got;
}

/* ALL OF IT, OR AN ERROR. write(2) may stop short: a signal that arrives
 * while it is blocked on a full pipe ends it with the count so far -- a
 * SIGCHLD from the `seq` a `$( )` just ran is enough. Every caller in
 * shell.c hands over its whole string once and moves on, so a short count
 * was data lost: `echo "$x" | wc -c` with a 108 KB $x counted 65537, the
 * first pipe-full and the newline. Retried, as bash retries. */
long file_write(struct file *f, const void *buf, size_t n) {
    if (!f) return -1;
    /* `>&-` CLOSED it: a write fails, as bash's does -- "echo: write
     * error: Bad file descriptor", status 1. It returned success, so a
     * builtin writing to a closed stdout said nothing (logs/b12 p50). */
    if (f->kind == FILE_KIND_NULL) return -EBADF;
    if (f->kind != FILE_KIND_VFS) return -1;
    int fd = VF_FD(&f->vfs);
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < n) {
        long put = g_diag_nosync ? host_write_nosync(fd, p + done, n - done)
                                 : write(fd, p + done, n - done);
        if (put < 0) {
            int e = errno;
            /* a fatal signal the EXIT trap waits for is acted on first --
             * the SIGPIPE that came with an EPIPE above all: bash's
             * `trap ... EXIT; while :; do echo y; done | head -1` runs the
             * trap and dies of it, saying no "write error" (p4 h9) */
            (void)shell_host_eintr();
            if (e == EINTR) continue;
            /* the errno, negated, for the caller's "write error: REASON"
             * (/dev/full is ENOSPC) */
            return done > 0 ? (long)done : -(long)e;
        }
        if (put == 0) break;
        done += (size_t)put;
        f->vfs.pos += (size_t)put;
    }
    return (long)done;
}

/* They exist because src/shell.c calls them (for `>>`), and it is the same
 * source file in both builds. The descriptor owns the offset here, exactly
 * as it does in a real process: handles produced by file_clone() share one
 * fd, so they share one position -- and so do the children it is handed to.
 * ->vfs.pos is only what this copy has counted, so it is never the answer
 * for a descriptor that can say where it is. */
size_t file_pos_get(struct file *f) {
    if (!f) return 0;
    int fd = VF_FD(&f->vfs);
    if (hf_seekable(f, fd)) {           /* ->vfs.pos is this COPY's count */
        off_t at = lseek(fd, 0, SEEK_CUR);
        if (at >= 0) return (size_t)at;
    }
    return f->vfs.pos;
}

void file_pos_set(struct file *f, size_t pos) {
    if (!f) return;
    f->vfs.pos = pos;
    int fd = VF_FD(&f->vfs);
    if (hf_seekable(f, fd)) (void)lseek(fd, (off_t)pos, SEEK_SET);
}

/* Unlike the kernel, a user process really does have three distinct standard
 * descriptors, so the fd number selects one. This is what makes `1>&2` reach
 * the actual stderr instead of collapsing back to stdout. */
struct file *file_std_handle(int fd) {
    if (fd < 0 || fd > 2) return 0;
    struct file *f = hf_make(fd);
    if (f) f->vfs.size = VF_UNSEEKABLE;
    return f;
}

/* Descriptors above 2 that this process INHERITED, for the shell to adopt
 * (shell_init_hosted). /proc/self/fd lists them; the directory's own
 * descriptor is on that list while it is being read, so every number is
 * re-checked with dup() once the directory is closed. Returns the count. */
int host_inherited_fds(int *out, int max) {
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != 0) {
        const char *s = e->d_name;
        if (*s < '0' || *s > '9') continue;
        int v = 0;
        for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
        if (*s == '\0' && v > 2) out[n++] = v;
    }
    closedir(d);
    int w = 0;
    for (int i = 0; i < n; i++) {
        int x = dup(out[i]);
        if (x < 0) continue;
        close(x);
        out[w++] = out[i];
    }
    return w;
}

/* An inherited descriptor as a file. Unseekable, like the standard three:
 * a write goes wherever the parent left the offset. */
struct file *host_wrap_inherited_fd(int fd) {
    struct file *f = hf_make(fd);
    if (f) f->vfs.size = VF_UNSEEKABLE;
    return f;
}

int pipe_create(struct file **out_r, struct file **out_w) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    *out_r = hf_make(fds[0]);
    *out_w = hf_make(fds[1]);
    if (!*out_r || !*out_w) {
        if (*out_r) file_close(*out_r); else close(fds[0]);
        if (*out_w) file_close(*out_w); else close(fds[1]);
        return -1;
    }
    /* Mark both ends unseekable: see file_write. */
    (*out_r)->vfs.size = VF_UNSEEKABLE;
    (*out_w)->vfs.size = VF_UNSEEKABLE;
    return 0;
}

/* The kernel hands out a console handle; here the shell's own STDIN is the
 * one: shell.c asks for it only as the input of `read` and `mapfile` when
 * nothing redirected theirs. It was fd 1 -- which a terminal also reads,
 * so every interactive test passed, while `echo ab | tsh -c 'read x'` and
 * `tsh -c 'read x' < file` read nothing at all (logs/b13 q56 q57). */
struct file *console_file_make(void) {
    struct file *f = hf_make(0);
    if (f) f->vfs.size = VF_UNSEEKABLE;
    return f;
}

/* ---- VFS ---------------------------------------------------------------
 *
 * shell.c speaks VFS_ERR_*, not errno, so translate at this boundary rather
 * than letting raw errno values escape into a layer that will print them
 * through vfs_strerror() and produce a confidently wrong message. */

static int errno_to_vfs(void) {
    switch (errno) {
        case ENOENT:  return VFS_ERR_NOENT;
        case ENOTDIR: return VFS_ERR_NOTDIR;
        case EISDIR:  return VFS_ERR_ISDIR;
        case ENOMEM:  return VFS_ERR_NOMEM;
        case EINVAL:  return VFS_ERR_INVAL;
        case EEXIST:  return VFS_ERR_EXIST;
        case EROFS:   return VFS_ERR_ROFS;
        case ENOSPC:  return VFS_ERR_NOSPC;
        case EACCES:  return VFS_ERR_PERM;
        case EPERM:   return VFS_ERR_NOTPERM;
        case ENAMETOOLONG: return VFS_ERR_NAMETOOLONG;
        case ENXIO:   return SHELL_HOSTED_ERR_NXIO;   /* /dev/tty, no terminal */
        default:      return VFS_ERR_IO;
    }
}

int vfs_open(const char *path, struct vfs_file *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    /* O_RDWR first: shell.c opens `>` and `>>` targets through vfs_open and
     * then writes to the handle, so a read-only fd here makes every file
     * redirection fail with nothing written and no error anyone can see.
     * Fall back to O_RDONLY for genuinely unwritable files (scripts on the
     * read-only initrd), which are the read-side cases. */
    int fd = open(path, O_RDWR);
    if (fd < 0) fd = open(path, O_RDONLY);
    if (fd < 0) return errno_to_vfs();
    memset(out, 0, sizeof *out);
    VF_SET(out, fd);
    struct stat st;
    out->size = (fstat(fd, &st) == 0) ? (size_t)st.st_size : 0;
    out->mode = 0644;
    return VFS_OK;
}

/* `<` opens O_RDONLY and `>` O_WRONLY, as bash's redirections do -- the
 * O_RDWR that vfs_open tries first made a FIFO both of its ends at once:
 * `while read x; do ...; done < fifo` never saw EOF, being a writer itself,
 * and `echo x > fifo` never waited for a reader (logs/b16 fifo). */
int vfs_open_output(const char *path, struct vfs_file *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    int fd = open(path, O_WRONLY);
    if (fd < 0) return errno_to_vfs();
    memset(out, 0, sizeof *out);
    VF_SET(out, fd);
    struct stat st;
    out->size = (fstat(fd, &st) == 0) ? (size_t)st.st_size : 0;
    out->mode = 0644;
    return VFS_OK;
}

/* `>>` IS A REAL O_APPEND DESCRIPTOR, as bash's is: every write through it
 * -- the shell's own, every child's, and anyone else's who opened the file
 * separately -- goes to the end of the file. A plain descriptor seeked to
 * the size at open only appends while nobody else writes:
 *
 *     { echo one; echo two >> f; echo three; } >> f
 *         bash: one two three
 *
 * The kernel honours it on every write since vfs_file.append. Write-only,
 * as bash opens it: reading a `>>` descriptor is an error there. */
/* `cmd < DIR` OPENS, as bash's does: a directory is opened for reading
 * (O_DIRECTORY -- the kernel refuses a native open of one otherwise) and
 * it is the READ that fails, with EISDIR. bash's readers each take that
 * their own way -- `read` reports "read error: 0: Is a directory", `mapfile`
 * stores nothing and succeeds, `cat` says "-: Is a directory" (Oils 1122;
 * logs/b12 p59). tsh refused at the open: "DIR: Is a directory", status 1,
 * command not run. Only the input redirection asks for this; `source DIR`
 * and every other vfs_open keep the refusal. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
int vfs_open_input(const char *path, struct vfs_file *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    int rc;
    {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            memset(out, 0, sizeof *out);
            VF_SET(out, fd);
            struct stat st;
            out->size = (fstat(fd, &st) == 0) ? (size_t)st.st_size : 0;
            out->mode = 0644;
            return VFS_OK;
        }
        rc = errno_to_vfs();
    }
    if (rc != VFS_ERR_ISDIR) return rc;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return errno_to_vfs();
    memset(out, 0, sizeof *out);
    VF_SET(out, fd);
    out->size = 0;
    out->mode = 0644;
    return VFS_OK;
}

int vfs_open_append(const char *path, struct vfs_file *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) return errno_to_vfs();
    memset(out, 0, sizeof *out);
    VF_SET(out, fd);
    struct stat st;
    out->size = (fstat(fd, &st) == 0) ? (size_t)st.st_size : 0;
    out->mode = 0644;
    return VFS_OK;
}

int vfs_close(struct vfs_file *f) {
    if (!f) return VFS_ERR_INVAL;
    if (VF_FD(f) >= 0) close(VF_FD(f));
    VF_SET(f, -1);
    return VFS_OK;
}

long vfs_read(struct vfs_file *f, void *buf, size_t n) {
    if (!f) return VFS_ERR_INVAL;
    long got = read(VF_FD(f), buf, n);
    while (got < 0 && errno == EINTR) {          /* as file_read */
        (void)shell_host_eintr();
        got = read(VF_FD(f), buf, n);
    }
    if (got > 0) f->pos += (size_t)got;
    return got;
}

/* struct stat into the shell's vfs_stat: for stat (vfs_stat, which
 * follows a link, as libtoby's stat does now) and lstat (vfs_lstat). */
static int host_vstat(const struct stat *stp, struct vfs_stat *out) {
    memset(out, 0, sizeof *out);
    out->size = (size_t)stp->st_size;
    out->type = S_ISDIR(stp->st_mode) ? VFS_TYPE_DIR :
                S_ISLNK(stp->st_mode) ? VFS_TYPE_SYMLINK : VFS_TYPE_FILE;
    /* Keep the S_IFMT bits (2026-08-23): sys_stat reports /dev nodes as
     * S_IFCHR, and test_unary's -c/-b/-p/-S read the format from mode.
     * Masking to 07777 here silently declassed every device node, which
     * went unnoticed exactly as long as the kernel's own stat did too --
     * the fidelity arc fixed the kernel side and the oilspec oracle
     * (in-guest bash) immediately diverged on `test -c /dev/zero`.
     *
     * AND THE BITS ARE REAL: stat(2) answers for a filesystem that keeps
     * no modes with 0644/0755 itself, so what comes back is the file's
     * mode -- VFS_MODE_VALID, which test_access_ok reads. Without it a
     * file chmod'ed 000 looked like "no permission information" and
     * `test -x` said yes to root; it was hidden while the kernel reported
     * a 0000 as 0644 (until 25225d94; distrotail tt_test_perm). */
    out->mode = (stp->st_mode & 0177777) | VFS_MODE_VALID;
    out->uid  = stp->st_uid;
    out->gid  = stp->st_gid;
    /* EVERYTHING struct stat CARRIES, not just the fields the shim happened
     * to need first. Each omission here is silently wrong somewhere in the
     * shell rather than obviously missing:
     *   - ino/dev absent made `test -ef` fall back to comparing paths, so
     *     two hard links to one file answered "different" -- the only
     *     question that operator exists to answer;
     *   - nlink absent left the count at 0;
     *   - mtime/atime absent left `test -nt` and `-ot` comparing 0 with 0,
     *     which is FALSE for every pair of files in the system. They were
     *     implemented and gated, and this zeroed their input. */
    out->nlink = (uint32_t)stp->st_nlink;
    out->ino   = (uint64_t)stp->st_ino;
    out->dev   = (uint32_t)stp->st_dev;
    out->mtime = (uint64_t)stp->st_mtime;
    out->atime = (uint64_t)stp->st_atime;
    out->ctime = (uint64_t)stp->st_mtime;
    return VFS_OK;
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    struct stat st;
    if (stat(path, &st) != 0) return errno_to_vfs();
    return host_vstat(&st, out);
}

/* The link itself: `test -h` and `test -L` (src/shell.c). There was
 * no lstat, and both were hard-wired false. */
int vfs_lstat(const char *path, struct vfs_stat *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    struct stat st;
    if (lstat(path, &st) != 0) return errno_to_vfs();
    return host_vstat(&st, out);
}

int vfs_read_all(const char *path, void **out_buf, size_t *out_size) {
    if (!path || !out_buf) return VFS_ERR_INVAL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno_to_vfs();
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return VFS_ERR_NOMEM; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) { free(buf); close(fd); return VFS_ERR_NOMEM; }
            buf = nb; cap = ncap;
        }
        long n = read(fd, buf + len, 4096);
        if (n < 0)  { free(buf); close(fd); return VFS_ERR_IO; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';            /* shell.c runs script text as a C string */
    *out_buf = buf;
    if (out_size) *out_size = len;
    return VFS_OK;
}

/* The shell writes whole files only as its history file, which a missing one
 * is created 0600 for, as readline's write_history makes it; the kernel takes
 * the umask off every mode below. */
int vfs_write_all(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return errno_to_vfs();
    long w = write(fd, buf, n);
    close(fd);
    return (w == (long)n) ? VFS_OK : VFS_ERR_IO;
}

int vfs_create_mode(const char *path, uint32_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)(mode & 07777u));
    if (fd < 0) return errno_to_vfs();
    close(fd);
    return VFS_OK;
}

/* bash's redirections open with 0666; so does every caller of this */
int vfs_create(const char *path) { return vfs_create_mode(path, 0666u); }

int vfs_mkdir_mode(const char *path, uint32_t mode) {
    return mkdir(path, (int)(mode & 07777u)) == 0 ? VFS_OK : errno_to_vfs();
}
int vfs_mkdir(const char *path)  { return vfs_mkdir_mode(path, 0777u); }
int vfs_unlink(const char *path) { return unlink(path)      == 0 ? VFS_OK : errno_to_vfs(); }

/* Truncate in place. The kernel has a real vfs_truncate; here it is an open
 * with O_TRUNC, which is the same thing through the syscall layer -- and
 * critically NOT unlink+create, so redirecting over a device node or a file
 * someone else holds open keeps the same inode. */
int vfs_truncate(const char *path, uint64_t len) {
    if (len != 0) return VFS_ERR_INVAL;      /* the shell only ever wants 0 */
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return errno_to_vfs();
    close(fd);
    return VFS_OK;
}

int vfs_chmod(const char *path, uint32_t mode) { (void)path; (void)mode; return VFS_ERR_ROFS; }
int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    (void)path; (void)uid; (void)gid; return VFS_ERR_ROFS;
}

int vfs_opendir(const char *path, struct vfs_dir *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    DIR *d = opendir(path);
    if (!d) return errno_to_vfs();
    memset(out, 0, sizeof *out);
    out->priv = d;
    return VFS_OK;
}

int vfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    if (!d || !d->priv || !out) return VFS_ERR_INVAL;
    struct dirent *de = readdir((DIR *)d->priv);
    if (!de) return VFS_ERR_NOENT;              /* end of directory */
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", de->d_name);
    out->type = (de->d_type == DT_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    out->mode = 0644;
    d->index++;
    return VFS_OK;
}

int vfs_closedir(struct vfs_dir *d) {
    if (d && d->priv) { closedir((DIR *)d->priv); d->priv = 0; }
    return VFS_OK;
}

const char *vfs_strerror(int err) {
    switch (err) {
        case VFS_OK:                 return "ok";
        case VFS_ERR_NOENT:          return "no such file or directory";
        case VFS_ERR_NOTDIR:         return "not a directory";
        case VFS_ERR_ISDIR:          return "is a directory";
        case VFS_ERR_NOMEM:          return "out of memory";
        case VFS_ERR_INVAL:          return "invalid argument";
        case VFS_ERR_NOMOUNT:        return "no filesystem mounted";
        case VFS_ERR_IO:             return "i/o error";
        case VFS_ERR_EXIST:          return "file exists";
        case VFS_ERR_ROFS:           return "read-only filesystem";
        case VFS_ERR_NOSPC:          return "no space left";
        case VFS_ERR_NAMETOOLONG:    return "name too long";
        case VFS_ERR_PERM:           return "permission denied";
        case VFS_ERR_NOTPERM:        return "operation not permitted";
        default:                     return "unknown error";
    }
}

/* ---- processes ---------------------------------------------------------- */

/* shell.c reads exactly four fields off current_proc(): cwd, uid, gid, pid.
 * A single static instance kept in step with the real process is enough, and
 * is honest about being a view rather than pretending to be the kernel's. */
static struct proc g_self;

struct proc *current_proc(void) {
    if (!getcwd(g_self.cwd, sizeof g_self.cwd)) g_self.cwd[0] = '\0';
    g_self.pid = getpid();
    g_self.uid = getuid();
    g_self.gid = getgid();
    return &g_self;
}

int proc_exec_in_place(const struct proc_spec *spec);

/* A standard descriptor the command has CLOSED (`cmd <&-`, `cmd >&-`). */
static bool host_slot_closed(const struct file *f) {
    return f && f->kind == FILE_KIND_NULL;
}

int proc_spawn(const struct proc_spec *spec) {
    if (!spec || !spec->path) return -1;

    /* ...AND A CLOSED ONE STAYS CLOSED. The kernel's spawn hands a child
     * either a descriptor or the console, never nothing, and a closed
     * handle has no descriptor -- so `cat <&-` got the shell's own fd 0, the
     * SSH session, and sat reading it (logs/b12 p73 never finished). bash
     * runs the program with fd 0 closed. That is fork, close, exec -- what
     * proc_exec_in_place does -- for the rare command that asks for it. */
    if (host_slot_closed(spec->fd0) || host_slot_closed(spec->fd1) ||
        host_slot_closed(spec->fd2)) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            (void)proc_exec_in_place(spec);
            _exit(127);
        }
        return (int)pid;
    }

    /* A NULL fd in the spec means "inherit"; the kernel would hand the child
     * the console. Here the standard descriptors already are that. */
    int fd0 = spec->fd0 ? VF_FD(&spec->fd0->vfs) : 0;
    int fd1 = spec->fd1 ? VF_FD(&spec->fd1->vfs) : 1;
    int fd2 = spec->fd2 ? VF_FD(&spec->fd2->vfs) : 2;

    /* The child gets the DESCRIPTOR exactly where it is. This used to seek
     * it to ->vfs.pos first, the offset the shell had counted for itself --
     * but that is a per-copy count (file_clone copies it) that never sees a
     * child's writes, so the second child of a group was put back over the
     * first child's output:
     *
     *     { echo one; cat f; echo two; cat f; echo three; } >> out
     *         bash: one xx two xx three      tsh: one xx txx three
     *
     * The descriptor's own offset is shared by the shell and every child
     * (one open file description), and `>>` is O_APPEND (vfs_open_append),
     * so there is nothing to correct. */

    /* proc_spec carries argv WITHOUT a NULL terminator (argc says how many);
     * toby_spawn wants the NULL-terminated POSIX shape. */
    char **argv = host_strvec(spec->argv, spec->argc);
    char **envp = host_strvec(spec->envp, spec->envp ? spec->envc : 0);

    /* Descriptors above 2 travel as (child fd, parent fd) pairs. The shell
     * side holds struct file*; here they are already real descriptors, so the
     * mapping is just VF_FD. As many as the spec has (it stopped at 16). */
    int nmax = spec->extra_nfds > 0 ? spec->extra_nfds : 1;
    struct abi_spawn_fd *extra =
        (struct abi_spawn_fd *)malloc((size_t)nmax * sizeof *extra);
    if (!argv || !envp || !extra) {
        free(argv); free(envp); free(extra);
        errno = ENOMEM;
        return -1;
    }
    unsigned nextra = 0;
    for (int i = 0; i < spec->extra_nfds && nextra < ABI_SPAWN_EXTRA_MAX; i++) {
        struct file *f = spec->extra_fds[i].f;
        /* a slot the shell has closed (`exec 3>&-`) is not passed: the
         * child does not have it. Its handle names no descriptor, and
         * VF_FD read 0 -- so the child's fd 3 was the shell's STDIN
         * (logs/b14 r18). */
        if (!f || host_slot_closed(f)) continue;
        int pfd = VF_FD(&f->vfs);
        if (pfd < 0) continue;
        extra[nextra].child_fd  = spec->extra_fds[i].fd;
        extra[nextra].parent_fd = pfd;
        nextra++;
    }

    int rc = (int)toby_spawn_flags(spec->path, argv, spec->envp ? envp : 0,
                                   fd0, fd1, fd2, nextra ? extra : 0, nextra,
                                   host_spawn_sigflags(spec));
    int saved = errno;
    free(argv); free(envp); free(extra);
    errno = saved;
    return rc;
}

/* REPLACE THIS PROCESS with the program -- the exec half of fork+exec.
 *
 * proc_spawn() above is the only way tsh has ever run an external, and a
 * backgrounded command therefore became TWO processes: the forked shell
 * that expanded the text, and the program that shell then spawned and
 * waited on. `$!` named the shell. Measured against bash on the box:
 *
 *     tsh    SELF=111  $!=113 (a tsh)   actual sleep=114
 *     bash   SELF=106  $!=109            actual sleep=109
 *
 * So the child installs the same descriptors proc_spawn would have handed a
 * new process -- into ITS OWN table, with dup2 -- and execs. On success it
 * does not return; the fork pid is now the program, and `$!` is right for
 * the same reason it is right in bash. Returns -1 only on failure, and the
 * caller falls back to the spawn it would have done anyway. */
/* THE REAL DESCRIPTOR BEHIND A HANDLE, and moving one to a chosen number.
 *
 * Process substitution needs both. The filename it hands a command is
 * /dev/fd/N, and N has to name the pipe in the SHELL (which may open that
 * path itself, for a redirect target) and in the COMMAND (which opens it
 * after being spawned). One number, two processes -- so the shell moves its
 * end to a number it picked, rather than reading off whatever the pipe
 * happened to get. VF_FD and dup2 are the shim's, not the shell's. */
int host_file_fd(struct file *f) { return f ? VF_FD(&f->vfs) : -1; }

/* fstat of the descriptor behind a handle: `test -p /dev/stdin` asks about
 * the BUILTIN's fd 0, which a redirection or a pipe may have set in the
 * shell's own table and not in this process's (shell.c test_fd_stat). */
int host_file_vstat(struct file *f, struct vfs_stat *out) {
    int fd = host_file_fd(f);
    if (fd < 0 || !out) return VFS_ERR_INVAL;
    struct stat st;
    if (fstat(fd, &st) != 0) return errno_to_vfs();
    return host_vstat(&st, out);
}

int host_file_move_fd(struct file *f, int want) {
    if (!f || want < 0) return -1;
    int cur = VF_FD(&f->vfs);
    if (cur < 0) return -1;
    if (cur == want) return want;
    if (dup2(cur, want) < 0) return -1;
    close(cur);
    VF_SET(&f->vfs, want);
    return want;
}

/* `read -t`: is there input on this descriptor, or would a read return EOF at
 * once? poll(2) says; a timeout above zero waits that long for it. Lives
 * here because VF_FD and <poll.h> are the host shim's, not the shell's. */
int host_input_ready(struct file *f, long timeout_ms) {
    int fd = f ? VF_FD(&f->vfs) : -1;
    if (fd < 0) return 1;
    /* A signal ends the poll (it is never restarted, as on Linux): the
     * shell acts on a fatal one the EXIT trap waits for, and the wait goes
     * on for what is left of the timeout -- bash's `read -t 3` with a
     * trapped USR1 at 1 s still times out at 3 (logs/b15 p6 r3, WSL). */
    uint64_t end = perf_now_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    for (;;) {
        long left = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t now = perf_now_ns();
            left = now >= end ? 0 : (long)((end - now + 999999ull) / 1000000ull);
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&pfd, 1, (int)left);
        if (r > 0) return 1;
        if (r == 0) return 0;
        if (errno != EINTR) return 1;
        (void)shell_host_eintr();
    }
}

int proc_exec_in_place(const struct proc_spec *spec) {
    if (!spec || !spec->path) return -1;
    /* Descriptors go over where they are -- see proc_spawn.
     *
     * EVERY SOURCE IS TAKEN BEFORE ANY TARGET CHANGES. A source can sit on
     * a number another slot is about to be given: fd 2 copying fd 1 after
     * `exec 2>&1` (shparity 228b), or -- once `exec 3>&-` had freed 3 --
     * the read end of a background pipeline's pipe, which the next pipe
     * took as fd 3: the extra for slot 3 went in first and the element's
     * stdin was then copied from what had replaced it, the shell's own
     * stdin (logs/b14 r18: `echo hi | cat > f &` read the SSH session).
     * So each source is copied above every number in play, and the targets
     * are filled from the copies -- the extras first, so a slot 0, 1 or 2
     * wins over an extra that names it. A slot the shell has CLOSED is
     * closed here. One that is already where it goes stays as it is,
     * uncopied: it may name a descriptor that is no longer open -- a
     * process substitution's child closes the shell's end of it -- and
     * copying that failed the exec, so `f 2> >(grep x) | tac` left a tsh
     * holding tac's pipe and deadlocked (Oils 1983). */
    int nx = spec->extra_nfds > 0 ? spec->extra_nfds : 0;
    int n = nx + 3;
    int *tgt = (int *)malloc((size_t)n * sizeof *tgt);
    int *src = (int *)malloc((size_t)n * sizeof *src);
    if (!tgt || !src) { free(tgt); free(src); return -1; }
    int top = 2;
    for (int i = 0; i < n; i++) {
        struct file *f;
        int t;
        if (i < nx) {
            f = spec->extra_fds[i].f;
            t = spec->extra_fds[i].fd;
        } else {
            t = i - nx;
            f = t == 0 ? spec->fd0 : t == 1 ? spec->fd1 : spec->fd2;
        }
        tgt[i] = t;
        if (host_slot_closed(f)) src[i] = -1;                   /* close it */
        else if (f) src[i] = VF_FD(&f->vfs);
        else src[i] = t;                         /* none: as it is */
        if (src[i] < -1 || (src[i] < 0 && !host_slot_closed(f))) {
            if (i >= nx) { free(tgt); free(src); return -1; }
            tgt[i] = -1;                         /* an extra with no fd */
        }
        if (src[i] == tgt[i]) tgt[i] = -1;       /* already in place */
        if (tgt[i] > top) top = tgt[i];
        if (src[i] > top) top = src[i];
    }
    if (top + 1 + n >= 1024) { free(tgt); free(src); return -1; }
    for (int i = 0; i < n; i++)
        if (tgt[i] >= 0 && src[i] >= 0 && dup2(src[i], top + 1 + i) < 0) {
            free(tgt); free(src);
            return -1;
        }
    for (int i = 0; i < n; i++) {
        if (tgt[i] < 0) continue;
        if (src[i] == -1) (void)close(tgt[i]);
        else (void)dup2(top + 1 + i, tgt[i]);
    }
    for (int i = 0; i < n; i++)
        if (tgt[i] >= 0 && src[i] >= 0) (void)close(top + 1 + i);
    free(tgt);
    free(src);

    char **argv = host_strvec(spec->argv, spec->argc);
    char **envp = host_strvec(spec->envp, spec->envp ? spec->envc : 0);
    if (!argv || !envp) { free(argv); free(envp); return -1; }
    /* INT and QUIT as the shell asked (proc_spec.sig_default/sig_ignore),
     * as proc_spawn's flags give them; exec keeps an ignore and resets a
     * handler, so these are what the program starts with. */
    tsh_sighandler_fn was[SIGQUIT + 1] = { 0 };
    bool put[SIGQUIT + 1] = { false };
    for (int s = SIGINT; s <= SIGQUIT; s++) {
        if (spec->sig_ignore & SIGMASK(s))       { was[s] = signal(s, SIG_IGN); put[s] = true; }
        else if (spec->sig_default & SIGMASK(s)) { was[s] = signal(s, SIG_DFL); put[s] = true; }
    }
    (void)execve(spec->path, argv, spec->envp ? envp : 0);
    int saved = errno;
    for (int s = SIGINT; s <= SIGQUIT; s++)     /* the exec failed: put back */
        if (put[s]) host_sig_put(s, was[s]);
    free(argv); free(envp);         /* only reached when the exec failed */
    errno = saved;
    return -1;
}

/* The signal that killed the child last reaped through these seams, 0 for
 * an exit (proc_last_term_sig). bash names it in its job reports. */
static int g_last_term_sig;

int proc_last_term_sig(void) { return g_last_term_sig; }

static int host_reaped(int status) {
    g_last_term_sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    return WEXITSTATUS(status);
}

int proc_wait(int pid) {
    int status = 0;
    /* Retry EINTR: the parent now receives SIGCHLD, and a wait must not
     * fail because the signal it exists to consume arrived. An interruption
     * is the shell's to judge first (shell_host_eintr): a fatal signal the
     * EXIT trap waits for is acted on at once, as bash's termsig handler
     * does mid-wait (logs/b15 p4 h1), and `wait` stops for a trapped one
     * -- -2, which only the `wait` builtin ever asks for. */
    for (int tries = 0; tries < 1000; ) {
        if (waitpid(pid, &status, 0) >= 0) return host_reaped(status);
        if (errno == EINTR) {
            if (shell_host_eintr()) return -2;
            continue;
        }
        tries++;
    }
    return -1;
}

int proc_wait_info(int pid, struct proc_exit_info *out) {
    int rc = proc_wait(pid);
    if (out) {
        memset(out, 0, sizeof *out);
        out->pid = pid;
        out->exit_code = rc;
    }
    return rc;
}

/* ---- clock, keyboard, signals ------------------------------------------
 *
 * These have honest userspace equivalents, so they are implemented rather
 * than stubbed: `sleep`, `times` and `kill` are language-adjacent builtins a
 * script may legitimately use, and a stub would make them lie quietly. */

uint64_t pit_ticks(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

uint32_t pit_hz(void) { return 1000; }        /* pit_ticks() counts in ms */

/* The shell's user and system time and its reaped children's, ns:
 * ABI_SYS_TIMES (times(2) and getrusage(2) for a native program). */
int host_cputimes(uint64_t *out) {
    struct abi_times t;
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "0"((long)ABI_SYS_TIMES), "D"(0L), "S"((long)&t)
                     : "rcx", "r11", "memory");
    if (r < 0) return -1;
    out[0] = t.utime_ns;
    out[1] = t.stime_ns;
    out[2] = t.cutime_ns;
    out[3] = t.cstime_ns;
    return 0;
}

void pit_sleep_ms(uint64_t ms) {
    struct timespec req = { (time_t)(ms / 1000), (long)((ms % 1000) * 1000000L) };
    (void)nanosleep(&req, 0);
}

uint64_t perf_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The kernel's RTC-anchored realtime clock, under the name the shell calls
 * it by (printf %(fmt)T). The kernel's converter takes a monotonic reading;
 * here libtoby's CLOCK_REALTIME already IS that converter's answer, via
 * ABI_SYS_CLOCK_NS, so the argument is not needed. */
uint64_t lx_realtime_ns(uint64_t mono_ns) {
    (void)mono_ns;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 32 bits from the kernel's CSPRNG, for $SRANDOM and $RANDOM's seed (S13).
 * The kernel shell calls rng_fill directly. */
uint32_t host_random_u32(void) {
    uint32_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, &v, sizeof v);
        close(fd);
    }
    return v;
}

/* The hosted shell reads stdin line-at-a-time in main(); the kernel's
 * keyboard ring has no userspace counterpart. -1 == "nothing queued". */
int kbd_trygetc(void) { return -1; }

void signal_send_to_pid(int pid, int sig) { (void)kill(pid, sig); }
int signal_send_to_pid_checked(int pid, int sig) { return kill(pid, sig); }
int proc_set_pgid(int pid, int pgid) { return setpgid(pid, pgid); }

/* Job-control seams for the monitor-mode foreground path. */
/* While this is set the shell is parked in a read at its prompt or in the
 * wait for a foreground command: what `set -b`'s SIGCHLD handler may report
 * at (see tsh_sigchld). */
volatile int g_tsh_async_ok;

int proc_wait_fg(int pid, int *stop_sig) {
    int st = 0;
    if (stop_sig) *stop_sig = 0;
    for (int tries = 0; tries < 1000; tries++) {
        g_tsh_async_ok = 1;
        pid_t r = waitpid(pid, &st, WUNTRACED);
        g_tsh_async_ok = 0;
        if (r < 0) {                                      /* EINTR retry */
            if (errno == EINTR) (void)shell_host_eintr();
            continue;
        }
        if (WIFSTOPPED(st)) {
            if (stop_sig) *stop_sig = WSTOPSIG(st);
            return 0;
        }
        return host_reaped(st);
    }
    return -1;
}

void shell_tty_set_fg_pgrp(int pgrp) {
    int pg = pgrp > 0 ? pgrp : (int)getpgrp();
    (void)tsh_give_terminal(0, pg);
}

void signal_send_to_pgrp(int pgrp, int sig) { (void)kill(-pgrp, sig); }

/* Non-blocking reap for the prompt-time job notifier: 0 = still running,
 * 1 = reaped (exit code in *code), -1 = no such child. */
int proc_try_reap(int pid, int *code) {
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == 0) return 0;
    if (r < 0) return -1;
    int c = host_reaped(st);
    if (code) *code = c;
    return 1;
}

/* ...and, with UNTRACED, a stop (2, its signal in *stopsig) or a continue
 * (3), each reported once by the kernel -- what a job-control shell asks
 * of its background jobs (bash's waitchld: WUNTRACED|WCONTINUED). */
int proc_try_reap_state(int pid, int *code, int *stopsig, bool untraced) {
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG | (untraced ? WUNTRACED | WCONTINUED : 0));
    if (r == 0) return 0;
    if (r < 0) return -1;
    if (WIFSTOPPED(st)) {
        if (stopsig) *stopsig = WSTOPSIG(st);
        return 2;
    }
    if (WIFCONTINUED(st)) return 3;
    int c = host_reaped(st);
    if (code) *code = c;
    return 1;
}

/* Is SIG's bit set in a /proc/PID/status mask -- SigBlk, SigIgn and SigCgt
 * are hex in Linux numbering (bit SIG-1)? */
static bool host_hexbit(const char *s, int sig) {
    unsigned long long v = 0;
    for (; *s && *s != '\n'; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' :
                (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
                (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (d < 0) break;
        v = (v << 4) | (unsigned)d;
    }
    return sig >= 1 && sig <= 64 && ((v >> (sig - 1)) & 1u);
}
/* /proc/PID/status into BUF, NUL-terminated; its length, 0 if unreadable. */
static long host_proc_status(int pid, char *buf, size_t cap) {
    char path[48];
    if (pid > 0) snprintf(path, sizeof path, "/proc/%d/status", pid);
    else snprintf(path, sizeof path, "/proc/self/status");
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    long n = 0, r;
    while (n < (long)cap - 1 && (r = read(fd, buf + n, cap - 1 - n)) > 0)
        n += r;
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return n;
}

/* THE SIGNALS THIS SHELL WAS STARTED WITH IGNORED (SIGMASK bits), from
 * /proc/self/status. bash's SIG_HARD_IGNORE: a non-interactive shell may
 * neither trap nor reset one, and lists it as `trap -- '' SIGX` (POSIX
 * 2.14 trap; logs/b15 p3 b1-b5, c, e). 0 if /proc cannot say. */
static uint64_t host_entry_ignored(void) {
    char buf[4096];
    if (!host_proc_status(0, buf, sizeof buf)) return 0;
    const char *f = strstr(buf, "\nSigIgn:");
    if (!f) return 0;
    f += 8;
    while (*f == ' ' || *f == '\t') f++;
    uint64_t m = 0;
    for (int s = 1; s < SIG_MAX; s++)
        if (host_hexbit(f, s)) m |= SIGMASK(s);
    return m;
}

/* EVERY bash IGNORES SIGQUIT (sig.c initialize_shell_signals), and
 * `trap - QUIT` puts that back: `bash -c 'kill -QUIT $$; echo alive'`
 * prints alive where tsh died, 131 (logs/b15 p5 q1 q2 q5). A no-op
 * handler, not SIG_IGN, so that what the shell runs starts with the
 * default -- an exec resets a handler and keeps an ignore -- and a
 * subshell sets the default itself (shell_child_signals in shell.c),
 * as bash's restore_original_signals gives both. */
static void tsh_quit(int sig) { (void)sig; }
void shell_host_quit_ignore(void) { (void)signal(SIGQUIT, tsh_quit); }

/* bash's kill_shell: die OF the signal -- its default action, unblocked
 * -- so the parent sees the death; the exit status it would give if that
 * somehow fails. Called once the EXIT trap has run (shell.c
 * shell_termsig_act). */
void shell_host_kill_self(int sig) {
    unsigned long m = 1UL << sig;
    (void)signal(sig, SIG_DFL);
    (void)sigprocmask(1 /* SIG_UNBLOCK */, &m, 0);
    (void)kill(getpid(), sig);
    _exit(128 + sig);
}

/* ---- main --------------------------------------------------------------- */

/* bash's long options, as it lists them (shell.c's long_args) */
static const char *const g_long_opts[] = {
    "debug", "debugger", "dump-po-strings", "dump-strings", "help",
    "init-file", "login", "noediting", "noprofile", "norc", "posix",
    "pretty-print", "rcfile", "restricted", "verbose", "version", 0 };

static void usage(FILE *f, bool extra) {
    fprintf(f, "Usage:\ttsh [long option] [option] ...\n"
               "\ttsh [long option] [option] script-file ...\n");
    fprintf(f, "Long options:\n");
    for (int i = 0; g_long_opts[i]; i++) fprintf(f, "\t--%s\n", g_long_opts[i]);
    fprintf(f, "Shell options:\n"
               "\t-ilrsD or -c command or -O shopt_option\t\t(invocation only)\n"
               "\t-abefhkmnptuvxBCEHPT or -o option\n");
    if (extra)
        fprintf(f, "\n`tsh -c \"help set\"' describes the shell options, and "
                   "`tsh -c help' the builtins.\n");
}

extern bool shell_set_flag_hosted(char flag, bool on);
extern bool shell_set_option_hosted(const char *name, bool on);
extern void shell_list_options_hosted(bool reinput);
extern bool shell_shopt_hosted(const char *name, bool on);
extern void shell_list_shopts_hosted(bool reinput);
extern void shell_set_invocation_hosted(bool c, bool s);
extern void shell_set_login_hosted(bool set_home);
extern void shell_exit_notice_hosted(void);
extern bool shell_eof_refused_hosted(void);
extern void shell_exit_jobs_hosted(void);
extern int  shell_exit_trap_hosted(int status);
extern int  shell_run_stdin_unit_hosted(const char *text, unsigned long lines);
extern bool shell_is_login_hosted(void);
extern bool shell_hist_active_hosted(void);
extern const char *shell_hist_line_hosted(const char *line, const char *accum, int *rc);
extern void shell_startup_files_hosted(int login, bool interactive, bool norc,
                                       bool noprofile, const char *rcfile,
                                       bool act_like_sh);
extern void shell_logout_hosted(void);
extern void shell_make_restricted_hosted(void);
extern void shell_async_child_hosted(const char *snap, const char *state,
                                     const char *text)
    __attribute__((noreturn));
extern void shell_note_entry_ignores_hosted(uint64_t mask);

/* Absorb tty job-control signals that reach the shell itself. A handler,
 * not SIG_IGN -- see the interactive setup below for why. */
static void tsh_job_signal(int sig) { (void)sig; }

/* ^C. Records the interrupt for the interpreter (which unwinds every
 * enclosing loop/function back to the prompt) and for the read loop
 * below. Nothing else happens in here -- a signal handler that tried to
 * reprint the prompt would be writing to the tty from inside whatever
 * the shell was doing. */
extern void shell_note_interrupt(void);
extern int  shell_take_interrupt(void);
static volatile int g_tsh_sigint;
static void tsh_sigint(int sig) {
    (void)sig;
    g_tsh_sigint = 1;
    shell_note_interrupt();
}

/* SIGINT MUST NOT RESTART THE READ.
 *
 * libtoby's signal() sets SA_RESTART, which is right for almost every
 * handler and exactly wrong for this one: with it, ^C at the prompt ran
 * the handler and then RESUMED the blocked read(), so the half-typed line
 * stayed put, no newline appeared, and no new prompt was drawn. The key
 * did nothing observable, which is the bug. sigaction without SA_RESTART
 * makes the read return EINTR so the loop can react. */
static void tsh_arm_sigint(void) {
    (void)signal_norestart(SIGINT, tsh_sigint);
}

/* A HANGUP at a terminal: noted, and the read it interrupts returns, so
 * the loop can hang up the jobs and leave (shell_hangup_hosted). */
static volatile int g_tsh_sighup;
static void tsh_sighup(int sig) { (void)sig; g_tsh_sighup = 1; }

/* Set when this shell made SIGTERM a no-op, as an interactive shell does
 * (main, below): its forks must not keep that (shell_fork's child, in
 * src/shell.c, takes it once with shell_host_take_term_noop). */
static bool g_tsh_term_noop;
bool shell_host_take_term_noop(void) {
    bool was = g_tsh_term_noop;
    g_tsh_term_noop = false;
    return was;
}

/* The hangup, acted on: the jobs are hung up, the EXIT trap runs, and the
 * shell dies of the signal, as bash's does (kill_shell). */
extern int shell_hangup_hosted(void);
static void tsh_hangup_exit(void) {
    (void)shell_hangup_hosted();
    signal(SIGHUP, SIG_DFL);
    kill(getpid(), SIGHUP);
    _exit(128 + SIGHUP);
}

/* SIGCHLD -- A CHILD HAS ENDED, STOPPED OR BEEN CONTINUED. What the shell
 * knows of its jobs moves with this signal and only with it, as bash's
 * does with the waitchld its handler runs: the handler counts it
 * (g_tsh_chld_seq) and the shell's next look at its jobs collects what it
 * says (shell.c shell_jobs_learn). tsh polled at each look instead, and
 * saw a death bash had not yet been told of -- or, from a heuristic for
 * that, one bash had already been told of where it had not: Oils 0945 is
 * 143 in bash on the EliteDesk, whose SIGCHLD comes after the `wait`, and
 * 0 in QEMU, whose comes before (finding 44).
 *
 * Installed for good at startup, with SA_RESTART, so no read or wait sees
 * an EINTR it did not expect. A CHLD trap is relayed from here: it does
 * not change the signal's disposition, which is the shell's (bash's
 * SIG_NO_TRAP). `set -b` reports at once -- when the shell is parked at
 * its prompt or in a foreground wait (g_tsh_async_ok), where nothing it is
 * doing can be disturbed; it is not in the middle of a malloc there. */
extern volatile int g_tsh_async_ok;
/* The line editor's reads may be reported at too, but not the work it
 * does between them: lineedit.c sets g_tsh_async_ok around each read(). */
volatile int g_tsh_le_async;
volatile unsigned g_tsh_chld_seq;
extern bool shell_opt_notify_hosted(void);
extern void shell_async_notify_hosted(void);
extern bool shell_chld_trapped_hosted(void);
extern void shell_deliver_signal(int sig);
static void tsh_sigchld(int sig) {
    g_tsh_chld_seq++;
    if (shell_chld_trapped_hosted()) shell_deliver_signal(sig);
    if (g_tsh_async_ok && shell_opt_notify_hosted()) shell_async_notify_hosted();
}
static void tsh_sigchld_arm(void) { (void)signal(SIGCHLD, tsh_sigchld); }

/* The shell's process group, for `jobs -x` (shell_own_pgrp). */
int host_getpgrp(void) { return (int)getpgrp(); }

#include "lineedit.h"

/* Kept free below the deepest nesting the shell allows. */
#define TSH_STACK_RESERVE (512ul << 10)

int main(int argc, char **argv) {
    /* STDOUT IS LINE-BUFFERED, whatever it is: bash's shell_initialize does
     * sh_setlinebuf(stdout) before anything else. A diagnostic in the
     * middle of a builtin's output therefore follows the last whole line
     * and precedes a part line -- `printf '%d|' 1 x 2` to a file is the
     * error, then "1|0|2|", and a builtin printing lines then failing has
     * them out first (logs/b13 q53). libtoby buffered a file fully. */
    (void)setvbuf(stdout, 0, _IOLBF, BUFSIZ);
    /* THE KERNEL SHELL'S ASYNCHRONOUS CHILD (finding 63): the kernel shell
     * cannot fork, so it backgrounds a function, a builtin or a compound
     * by starting `tsh -c TEXT [NAME ARGS...]` with its state in these two
     * (see shell_async_child_hosted). They leave the environment before
     * the shell imports it, so neither is ever a variable. */
    char *async_snap = 0, *async_state = 0;
    {
        const char *s = getenv("TOBYOS_TSH_ASYNC");
        const char *t = getenv("TOBYOS_TSH_ASYNC_STATE");
        if (s && t && argc >= 3 && strcmp(argv[1], "-c") == 0) {
            async_snap = strdup(s);
            async_state = strdup(t);
        }
        if (s) (void)unsetenv("TOBYOS_TSH_ASYNC");
        if (t) (void)unsetenv("TOBYOS_TSH_ASYNC_STATE");
    }
    shell_init_hosted(argv[0]);
    tsh_sigchld_arm();                  /* before any child: see tsh_sigchld */
    /* HOW DEEP THE SHELL MAY NEST IS SET BY THE STACK (g_stack_floor in
     * src/shell.c). A native process's stack tops out at the end of the
     * user half and grows on demand down to USER_STACK_FLOOR_VA, 8 MiB
     * below, as Linux's default RLIMIT_STACK does for bash; the shell may
     * use all but TSH_STACK_RESERVE of it. Only if this frame is on that
     * stack -- otherwise the shell keeps its fallback count. */
    {
        unsigned long sp = (unsigned long)__builtin_frame_address(0);
        if (sp > USER_STACK_FLOOR_VA && sp <= USER_STACK_TOP_LIMIT_VA)
            shell_set_stack_floor_hosted(USER_STACK_FLOOR_VA +
                                         TSH_STACK_RESERVE);
    }
    if (async_snap && async_state) {
        if (argc > 3) (void)shell_set_param0_hosted(argv[3]);
        if (argc > 4) (void)shell_set_args_hosted(argc - 4, argv + 4);
        shell_async_child_hosted(async_snap, async_state, argv[2]);
    }
    /* What was ignored at entry is ignored for good (host_entry_ignored),
     * and the shell itself ignores QUIT (tsh_quit). Not for the kernel
     * shell's asynchronous child above: its signals are its parent's, and
     * the kernel shell ignores nothing at entry. */
    {
        uint64_t ign = host_entry_ignored();
        shell_note_entry_ignores_hosted(ign);
        if (!(ign & SIGMASK(SIGQUIT))) shell_host_quit_ignore();
    }
    /* Invoked as `sh` -- /bin/sh is a link to this binary -- means POSIX
     * mode, as it does for bash. */
    {
        const char *b = argv[0] ? strrchr(argv[0], '/') : 0;
        b = b ? b + 1 : (argv[0] ? argv[0] : "");
        if (strcmp(b, "sh") == 0) shell_set_posix_hosted(true);
    }
    {
        /* XCU 2.5.3: PPID is set at shell startup. It was simply absent. */
        char pb[16];
        snprintf(pb, sizeof pb, "%d", (int)getppid());
        shell_set_var_hosted("PPID", pb);
    }

    /* BASH'S COMMAND LINE (shell.c's main, parse_long_options and
     * parse_shell_options): long options first -- `--name`, or `-name` for a
     * name on the list -- then clustered flags, `-` or `+` each: -c, -s, -l,
     * -i, -r, -D, `-o NAME` / `-O NAME` (with no NAME, the listing), and any
     * flag `set` takes; `-` or `--` ends them. Then the -c string and its $0
     * and arguments, or a script and its arguments, or -- with -s, or with
     * nothing left -- commands from standard input.
     *
     * tsh knew `-c`, a script and --noediting: `tsh -e script` -- which is
     * what `#!/bin/bash -e` runs -- was a usage error, and so were -x, -o
     * pipefail, -O extglob, -l, -s and the rest (logs/b13 q14 q27). */
    const char *self = argv[0] ? argv[0] : "tsh";
    bool o_c = false, o_s = false, o_l = false, o_i = false;
    bool o_norc = false, o_noprofile = false, o_posix = false, o_restricted = false;
    bool o_version = false, o_help = false, o_dump = false;
    const char *rcfile = 0;
    int noediting = 0;
    /* -o/-O requests and flags, applied in order once the line is read */
    struct inv_req { char kind; bool on; const char *name; char flag; };
    struct inv_req *reqs = (struct inv_req *)malloc((size_t)(argc + 1) * sizeof *reqs);
    int nreq = 0;
    if (!reqs) return 2;
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        const char *a = argv[ai];
        bool dbl = a[1] == '-' && a[2];
        const char *nm = dbl ? a + 2 : a + 1;
        int k = 0;
        while (g_long_opts[k] && strcmp(nm, g_long_opts[k]) != 0) k++;
        if (!g_long_opts[k]) {
            if (dbl) {
                fprintf(stderr, "%s: %s: invalid option\n", self, a);
                usage(stderr, false);
                return 2;
            }
            break;                          /* a flag cluster: below */
        }
        if (!strcmp(nm, "init-file") || !strcmp(nm, "rcfile")) {
            if (ai + 1 >= argc) {
                fprintf(stderr, "%s: %s: option requires an argument\n", self, nm);
                return 2;
            }
            rcfile = argv[++ai];
        } else if (!strcmp(nm, "login")) o_l = true;
        else if (!strcmp(nm, "noediting")) noediting = 1;
        else if (!strcmp(nm, "noprofile")) o_noprofile = true;
        else if (!strcmp(nm, "norc")) o_norc = true;
        else if (!strcmp(nm, "posix")) o_posix = true;
        else if (!strcmp(nm, "restricted")) o_restricted = true;
        else if (!strcmp(nm, "verbose")) reqs[nreq++] = (struct inv_req){ 'f', true, 0, 'v' };
        else if (!strcmp(nm, "version")) o_version = true;
        else if (!strcmp(nm, "help")) o_help = true;
        else if (!strcmp(nm, "debugger"))
            reqs[nreq++] = (struct inv_req){ 'O', true, "extdebug", 0 };
        else if (!strcmp(nm, "dump-strings") || !strcmp(nm, "dump-po-strings"))
            o_dump = true;
        /* --debug and --pretty-print: accepted, as bash accepts them */
        ai++;
    }
    while (ai < argc && (argv[ai][0] == '-' || argv[ai][0] == '+')) {
        const char *a = argv[ai];
        if (a[1] == '\0' || (a[0] == '-' && a[1] == '-' && a[2] == '\0')) {
            ai++;
            break;
        }
        bool on = a[0] == '-';
        int next = ai + 1;
        for (const char *f = a + 1; *f; f++) {
            switch (*f) {
            case 'c': o_c = true; break;
            case 's': o_s = true; break;
            case 'l': o_l = true; break;
            case 'i': o_i = on; break;
            case 'r': o_restricted = true; break;
            case 'D': o_dump = true; break;
            case 'o':
            case 'O':
                if (next >= argc) {
                    reqs[nreq++] = (struct inv_req){ (char)(*f == 'o' ? 'L' : 'S'), on, 0, 0 };
                } else {
                    reqs[nreq++] = (struct inv_req){ *f, on, argv[next], 0 };
                    next++;
                }
                break;
            default:
                if (*f && strchr("abefhkmnptuvxBCEHPT", *f)) {
                    reqs[nreq++] = (struct inv_req){ 'f', on, 0, *f };
                } else {
                    fprintf(stderr, "%s: %c%c: invalid option\n", self, a[0], *f);
                    usage(stderr, false);
                    return 2;
                }
            }
        }
        ai = next;
    }
    if (o_version) {
        printf("tsh (tobyOS), GNU bash 5.2.15(1)-release compatible (x86_64-pc-tobyos)\n");
        return 0;
    }
    if (o_help) {
        usage(stdout, true);
        return 0;
    }

    /* what to run, and is it interactive (bash's main: -i, or no -c and
     * nothing left -- or -s -- with a terminal on stdin and stderr) */
    const char *cmd = 0, *script = 0;
    if (o_c) {
        if (ai >= argc) {
            fprintf(stderr, "%s: -c: option requires an argument\n", self);
            return 2;
        }
        cmd = argv[ai++];
        /* `sh -c CMD NAME ARGS...`: NAME is $0 and ARGS are $1.., and an
         * option after the string is one of them (bash: `-c 'echo $-' -x`
         * makes $0 "-x") */
        if (ai < argc) {
            (void)shell_set_param0_hosted(argv[ai++]);
            if (ai < argc) (void)shell_set_args_hosted(argc - ai, argv + ai);
        }
    } else if (o_s || ai >= argc) {
        o_s = true;
        if (ai < argc) (void)shell_set_args_hosted(argc - ai, argv + ai);
    } else {
        script = argv[ai++];
        if (ai < argc) (void)shell_set_args_hosted(argc - ai, argv + ai);
    }
    bool interactive = o_i || (!cmd && !script && isatty(0) && isatty(2));
    shell_set_interactive_hosted(interactive);
    shell_set_invocation_hosted(cmd != 0, o_s);

    bool act_like_sh = false;
    {
        const char *b = strrchr(self, '/');
        b = b ? b + 1 : self;
        if (*b == '-') b++;
        act_like_sh = strcmp(b, "sh") == 0;
    }
    if (o_posix) shell_set_posix_hosted(true);
    int login = o_l ? -1 : (self[0] == '-' ? 1 : 0);
    if (login) shell_set_login_hosted(login == 1);

    int m_req = 0;                      /* -m (1) or +m (-1) at invocation */
    for (int r = 0; r < nreq; r++) {
        struct inv_req *q = &reqs[r];
        if ((q->kind == 'f' && q->flag == 'm') ||
            (q->kind == 'o' && q->name && strcmp(q->name, "monitor") == 0))
            m_req = q->on ? 1 : -1;
        if (q->kind == 'f') {
            (void)shell_set_flag_hosted(q->flag, q->on);
        } else if (q->kind == 'o') {
            if (!shell_set_option_hosted(q->name, q->on)) {
                fprintf(stderr, "%s: line 0: %s: %s: invalid option name\n",
                        self, self, q->name);
                return 2;
            }
        } else if (q->kind == 'O') {
            if (!shell_shopt_hosted(q->name, q->on)) {
                fprintf(stderr, "%s: line 0: %s: invalid shell option name\n",
                        self, q->name);
                return 2;
            }
        } else if (q->kind == 'L') {
            shell_list_options_hosted(!q->on);
        } else if (q->kind == 'S') {
            shell_list_shopts_hosted(!q->on);
        }
    }
    free(reqs);
    /* -D: the $"..." strings would be listed and nothing run (bash: -n);
     * tsh translates none, so there are none to list */
    if (o_dump) (void)shell_set_flag_hosted('n', true);

    /* JOB CONTROL as bash starts it, before the startup files (shell.c's
     * shell_initialize): an interactive shell, or one given -m, takes its
     * terminal or says it has none -- and then has no `m` (finding 61) */
    if (interactive || m_req > 0) {
        bool jc = tsh_init_job_control(self, o_i);
        if (m_req < 0) jc = false;
        (void)shell_set_flag_hosted('m', jc);
    }

    shell_startup_files_hosted(login, interactive, o_norc, o_noprofile, rcfile,
                               act_like_sh);
    /* restricted -- by -r, --restricted or the name rbash -- once the
     * startup files have run (bash's maybe_make_restricted) */
    {
        const char *b = strrchr(self, '/');
        b = b ? b + 1 : self;
        if (*b == '-') b++;
        if (o_restricted || strcmp(b, "rbash") == 0) shell_make_restricted_hosted();
    }

    if (cmd) {
        int rc = shell_run_line_hosted(cmd);
        shell_logout_hosted();
        shell_exit_jobs_hosted();
        return rc;
    }
    if (script) {
        int rc = shell_run_script_hosted(script);
        shell_exit_jobs_hosted();
        return rc;
    }

    /* bash gives a shell at a terminal HISTSIZE=500 when it has none, and
     * that is the bound on its history; tsh's list has no other (S6). */
    if (isatty(0) && !shell_get_var_hosted("HISTSIZE"))
        shell_set_var_hosted("HISTSIZE", "500");

    /* Interactive: one line at a time. Deliberately not the kernel's raw-mode
     * editor -- history and cursor keys are a separate concern from the
     * language, and this keeps the hosted path free of terminal state. */
    /* An interactive job-control shell must survive the tty's signals:
     * ^C/^Z go to the FOREGROUND JOB (which owns the terminal while it
     * runs), and any that reach the shell are absorbed. No-op HANDLERS,
     * not SIG_IGN -- exec resets handlers to default in children but
     * PRESERVES ignores, and an inherited ignore would make fg jobs
     * immune to the very signals job control exists to route. */
    tsh_arm_sigint();                   /* ^C: no SA_RESTART -- see above */
    signal(SIGTSTP, tsh_job_signal);
    signal(SIGTTOU, tsh_job_signal);
    signal(SIGTTIN, tsh_job_signal);
    /* bash at a terminal ignores SIGTERM -- `kill $$` does not end it --
     * and a hangup hangs up its jobs before it goes (bash's
     * set_signal_handlers, termsig_handler). A no-op handler, not SIG_IGN,
     * for the reason above. */
    if (interactive) {
        signal(SIGTERM, tsh_job_signal);
        g_tsh_term_noop = true;
        (void)signal_norestart(SIGHUP, tsh_sighup);
    }

    /* PS1/PS2 come from the SHELL's variable table (they are rarely
     * exported, so getenv cannot see them), and prompts are written with a
     * bare write() so they reach the tty before the blocking read -- the
     * interactive gate frames its whole protocol on prompt sentinels.
     * Multi-line commands accumulate under PS2 using the same continuation
     * test the script accumulator uses. */
    /* Both GROW: a line held 4096 bytes and a multi-line command 16384,
     * and one past that was thrown away without a word (batch 13). */
    char *line = 0;
    size_t line_cap = 0;
    char *accum = 0;
    size_t acap = 0;
    size_t alen = 0;
    int last = 0;
    int eofs = 0;
    int cont = 0;
    /* The input ended in the middle of a command -- an open quote, a
     * compound, a here-document owed: what has been read runs as it is,
     * and the script reader says what bash says ("unexpected EOF while
     * looking for matching", "delimited by end-of-file"). tsh read EOF
     * again and again for good: `printf 'echo "x' | tsh` never ended. */
    int at_eof = 0;
    /* A script on stdin numbers its lines from the first (see
     * shell_run_stdin_unit_hosted): lines read so far, and the first line of
     * the command being read. */
    unsigned long lines_read = 0, unit_line = 0;
    for (;;) {
        if (g_tsh_sighup) tsh_hangup_exit();
        /* bash announces finished background jobs right before a prompt. */
        if (!cont) shell_notify_jobs_hosted();
        const char *ps = shell_get_var_hosted(cont ? "PS2" : "PS1");
        if (!ps) ps = cont ? "> " : "tsh$ ";
        ps = shell_decode_prompt_hosted(ps);    /* bash's decode_prompt_string */
        size_t n = 0;
        g_tsh_sigint = 0;
        int interrupted = 0;

        /* A terminal gets the line editor: history, cursor keys and the
         * control bindings everyone has in their fingers. Anything else --
         * a pipe, a file, `echo cmd | tsh` -- keeps the plain reader below,
         * because there is no cursor to move there and putting a non-tty
         * into raw mode would break it. */
        if (!noediting && tsh_lineedit_available()) {
            g_tsh_le_async = interactive;     /* around its reads alone */
            long r = tsh_readline(ps, &line, &line_cap);
            g_tsh_le_async = 0;
            if (r == TSH_LE_INTR) {
                interrupted = 1;
                n = 0;
            } else if (r == TSH_LE_EOF) {
                if (!cont) {
                    if (shell_opt_ignoreeof_hosted() && ++eofs < 10) {
                        fputs(shell_is_login_hosted()
                                  ? "Use \"logout\" to leave the shell.\n"
                                  : "Use \"exit\" to leave the shell.\n", stderr);
                        continue;
                    }
                    if (interactive) {
                        shell_exit_notice_hosted();
                        if (shell_eof_refused_hosted()) continue;
                    }
                    shell_logout_hosted();
                    /* a script on stdin has ended too: its trap */
                    int st = shell_exit_trap_hosted(last);
                    shell_exit_jobs_hosted();
                    return st;
                }
                n = 0;
            } else {
                n = (size_t)r;
            }
            goto have_line;
        }

        /* ON STANDARD ERROR, as bash prints its prompts (and readline its
         * whole line) -- `tsh -i >file` keeps the prompt on the terminal */
        if (interactive) write(2, ps, strlen(ps));
        for (;;) {
            char c;
            g_tsh_async_ok = interactive;
            long r = read(0, &c, 1);
            g_tsh_async_ok = 0;
            /* ^C while waiting for input: throw the half-typed line away
             * and start a fresh prompt, which is what every shell does.
             * Checked BEFORE the r<=0 EOF arm -- an interrupted read also
             * returns <= 0, and treating it as end-of-input would exit the
             * shell on ^C instead of interrupting it. */
            if (g_tsh_sigint) { interrupted = 1; break; }
            if (r <= 0) {
                if (n == 0 && !cont) {
                    /* `set -o ignoreeof`: an EOF at the start of a line must
                     * not exit an interactive shell. bash gives up after ten
                     * in a row so a closed stdin cannot spin forever. */
                    if (shell_opt_ignoreeof_hosted() && ++eofs < 10) {
                        fputs(shell_is_login_hosted()
                                  ? "Use \"logout\" to leave the shell.\n"
                                  : "Use \"exit\" to leave the shell.\n", stderr);
                        break;
                    }
                    /* bash announces an EOF exit -- the "exit" you see
                     * when ^D ends an interactive shell is printed by the
                     * shell, not echoed by the terminal. */
                    if (interactive) {
                        shell_exit_notice_hosted();
                        if (shell_eof_refused_hosted()) break;
                    }
                    shell_logout_hosted();
                    /* a script on stdin has ended too: its trap */
                    int st = shell_exit_trap_hosted(last);
                    shell_exit_jobs_hosted();
                    return st;
                }
                if (cont) at_eof = 1;
                break;
            }
            if (c == '\n') break;
            if (n + 2 > line_cap) {
                size_t nc = line_cap ? line_cap * 2 : 256;
                char *nl = (char *)realloc(line, nc);
                if (!nl) break;
                line = nl;
                line_cap = nc;
            }
            line[n++] = c;
        }
        if (n + 1 > line_cap) {
            char *nl = (char *)realloc(line, n + 1);
            if (!nl) { n = 0; if (!line) continue; } else { line = nl; line_cap = n + 1; }
        }
        line[n] = '\0';
        /* READLINE ECHOES what it reads, terminal or not: bash edits every
         * line of an interactive shell (only --noediting stops it), and with
         * a pipe for input the line it displays is the whole echo there is
         * -- on standard error, after the prompt (logs/b13 q41). */
        if (interactive && !noediting && !g_tsh_sigint) {
            (void)!write(2, line, n);
            (void)!write(2, "\n", 1);
        }
    have_line:
        if (interrupted) {
            /* The tty already echoed "^C" with no newline, so the next
             * prompt would run on from it. bash prints the newline; so do
             * we. Everything accumulated (including a half-finished PS2
             * continuation) is discarded, and $? becomes 130. */
            write(2, "\n", 1);
            alen = 0;
            cont = 0;
            /* Consume it HERE. Leaving the flag set for the interpreter to
             * find would make the next command the user types unwind
             * instead of run -- one ^C silently eating one command. */
            (void)shell_take_interrupt();
            last = 130;
            continue;
        }
        if (n > 0 || !at_eof) lines_read++;
        /* HISTORY EXPANSION, a line at a time as bash's reader does it
         * (shell_histexp.inc): the expanded line is echoed and is what is
         * read; one that fails is reported and read as empty. */
        if (n > 0 && shell_hist_active_hosted()) {
            int hr = 0;
            const char *xl = shell_hist_line_hosted(line, cont ? accum : "", &hr);
            if (hr < 0) {
                n = 0;
                line[0] = '\0';
            } else if (hr > 0) {
                size_t xn = strlen(xl);
                if (xn + 1 > line_cap) {
                    char *nl = (char *)realloc(line, xn + 1);
                    if (nl) { line = nl; line_cap = xn + 1; }
                }
                if (xn + 1 <= line_cap) {
                    memcpy(line, xl, xn + 1);
                    n = xn;
                }
            }
        }
        if (!cont) unit_line = lines_read;
        if (n == 0 && !cont) continue;
        eofs = 0;
        if (alen + n + 2 > acap) {
            size_t nc = acap ? acap : 1024;
            while (nc < alen + n + 2) nc *= 2;
            char *na = (char *)realloc(accum, nc);
            if (!na) { alen = 0; cont = 0; continue; }
            accum = na;
            acap = nc;
        }
        memcpy(accum + alen, line, n);
        alen += n;
        accum[alen] = '\0';
        int inc = at_eof ? 0 : shell_line_incomplete_hosted(accum);
        at_eof = 0;
        if (inc == 2 && alen > 0 && accum[alen - 1] == '\\') {
            alen--;                        /* backslash-newline: splice */
            accum[alen] = '\0';
            cont = 1;
            continue;
        }
        if (inc) {
            accum[alen++] = '\n';          /* newline is part of the text */
            accum[alen] = '\0';
            cont = 1;
            continue;
        }
        shell_history_add_hosted(accum);
        {
            /* PS0: after the command is read, before it runs (bash 4.4+) */
            const char *p0 = shell_get_var_hosted("PS0");
            if (p0 && *p0) {
                const char *d0 = shell_decode_prompt_hosted(p0);
                (void)!write(2, d0, strlen(d0));
            }
        }
        last = interactive ? shell_run_line_hosted(accum)
                           : shell_run_stdin_unit_hosted(accum, unit_line ? unit_line - 1 : 0);
        /* ^C that landed DURING the command, rather than at the prompt.
         * The tty echoed "^C" with no newline of its own and the read loop
         * never saw it (it was not blocked -- the command was running), so
         * the newline bash prints has to come from here or the next prompt
         * runs on from the "^C". Measured against bash on the gate's pty:
         * two bytes, and the only thing between a passing case and a
         * failing one. */
        if (g_tsh_sigint) { write(2, "\n", 1); g_tsh_sigint = 0; }
        alen = 0;
        cont = 0;
        {
            int st;
            if (shell_wants_exit_hosted(&st)) {
                /* The exit BUILTIN has announced itself already, on
                 * standard error and before any EXIT trap ran, as bash's
                 * does (shell_exit_notice_hosted); a shell ended any other
                 * way -- set -e -- says nothing. */
                shell_exit_jobs_hosted();
                return st;
            }
        }
    }
}
