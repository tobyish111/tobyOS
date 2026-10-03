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
        if (diag) { (void)file_write(diag, buf, (size_t)n); return; }
    }
    write(2, buf, (size_t)n);           /* diagnostics -> stderr. See header. */
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
        long put = write(fd, p + done, n - done);
        if (put < 0) {
            if (errno == EINTR) continue;
            /* the errno, negated, for the caller's "write error: REASON"
             * (/dev/full is ENOSPC) */
            return done > 0 ? (long)done : -(long)errno;
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

/* The kernel hands out a console handle; here stdout already is one. */
struct file *console_file_make(void) {
    struct file *f = hf_make(1);
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
    int rc = vfs_open(path, out);
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
    if (got > 0) f->pos += (size_t)got;
    return got;
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    struct stat st;
    if (stat(path, &st) != 0) return errno_to_vfs();
    memset(out, 0, sizeof *out);
    out->size = (size_t)st.st_size;
    out->type = S_ISDIR(st.st_mode) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    /* Keep the S_IFMT bits (2026-08-23): sys_stat reports /dev nodes as
     * S_IFCHR, and test_unary's -c/-b/-p/-S read the format from mode.
     * Masking to 07777 here silently declassed every device node, which
     * went unnoticed exactly as long as the kernel's own stat did too --
     * the fidelity arc fixed the kernel side and the oilspec oracle
     * (in-guest bash) immediately diverged on `test -c /dev/zero`. */
    out->mode = st.st_mode & 0177777;
    out->uid  = st.st_uid;
    out->gid  = st.st_gid;
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
    out->nlink = (uint32_t)st.st_nlink;
    out->ino   = (uint64_t)st.st_ino;
    out->dev   = (uint32_t)st.st_dev;
    out->mtime = (uint64_t)st.st_mtime;
    out->atime = (uint64_t)st.st_atime;
    out->ctime = (uint64_t)st.st_mtime;
    return VFS_OK;
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

int vfs_write_all(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return errno_to_vfs();
    long w = write(fd, buf, n);
    close(fd);
    return (w == (long)n) ? VFS_OK : VFS_ERR_IO;
}

int vfs_create(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return errno_to_vfs();
    close(fd);
    return VFS_OK;
}

int vfs_mkdir(const char *path)  { return mkdir(path, 0755) == 0 ? VFS_OK : errno_to_vfs(); }
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
        if (!f) continue;
        int pfd = VF_FD(&f->vfs);
        if (pfd < 0) continue;
        extra[nextra].child_fd  = spec->extra_fds[i].fd;
        extra[nextra].parent_fd = pfd;
        nextra++;
    }

    int rc = (int)toby_spawn_ex(spec->path, argv, spec->envp ? envp : 0,
                                fd0, fd1, fd2, nextra ? extra : 0, nextra);
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
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int r = poll(&pfd, 1, (int)timeout_ms);
    if (r < 0) return 1;
    return r > 0 ? 1 : 0;
}

int proc_exec_in_place(const struct proc_spec *spec) {
    if (!spec || !spec->path) return -1;
    int fd0 = spec->fd0 ? VF_FD(&spec->fd0->vfs) : 0;
    int fd1 = spec->fd1 ? VF_FD(&spec->fd1->vfs) : 1;
    int fd2 = spec->fd2 ? VF_FD(&spec->fd2->vfs) : 2;
    /* Descriptors go over where they are -- see proc_spawn. */
    /* Extras first, then 0/1/2: an extra that targets 0/1/2 would otherwise
     * be clobbered, and dup2 onto itself is a no-op so the common case is
     * untouched. */
    for (int i = 0; i < spec->extra_nfds; i++) {
        struct file *f = spec->extra_fds[i].f;
        if (!f) continue;
        int pfd = VF_FD(&f->vfs);
        int cfd = spec->extra_fds[i].fd;
        if (pfd < 0 || cfd < 0 || pfd == cfd) continue;
        if (dup2(pfd, cfd) < 0) return -1;
    }
    if (fd0 != 0 && !host_slot_closed(spec->fd0) && dup2(fd0, 0) < 0) return -1;
    if (fd1 != 1 && !host_slot_closed(spec->fd1) && dup2(fd1, 1) < 0) return -1;
    if (fd2 != 2 && !host_slot_closed(spec->fd2) && dup2(fd2, 2) < 0) return -1;
    /* closed last: a slot another was copied from must still be there */
    if (host_slot_closed(spec->fd0)) close(0);
    if (host_slot_closed(spec->fd1)) close(1);
    if (host_slot_closed(spec->fd2)) close(2);

    char **argv = host_strvec(spec->argv, spec->argc);
    char **envp = host_strvec(spec->envp, spec->envp ? spec->envc : 0);
    if (!argv || !envp) { free(argv); free(envp); return -1; }
    (void)execve(spec->path, argv, spec->envp ? envp : 0);
    int saved = errno;
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
     * fail because the signal it exists to consume arrived. */
    for (int tries = 0; tries < 1000; tries++) {
        if (waitpid(pid, &status, 0) >= 0) return host_reaped(status);
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
int proc_wait_fg(int pid, int *stop_sig) {
    int st = 0;
    if (stop_sig) *stop_sig = 0;
    for (int tries = 0; tries < 1000; tries++) {
        if (waitpid(pid, &st, WUNTRACED) < 0) continue;   /* EINTR retry */
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
    (void)ioctl(0, 0x5410UL /* TIOCSPGRP */, &pg);
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

/* ---- main --------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage: tsh [script [args...]]\n"
        "       tsh -c 'command'\n"
        "       tsh              (interactive)\n");
}

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

#include "lineedit.h"

/* Kept free below the deepest nesting the shell allows. */
#define TSH_STACK_RESERVE (512ul << 10)

int main(int argc, char **argv) {
    shell_init_hosted(argv[0]);
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

    /* `-c` and a script file are NOT terminal sessions. Only the loop at the
     * bottom of this function is, and it leaves the default alone. Getting
     * this backwards makes a script expand aliases where bash does not. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        shell_set_interactive_hosted(false);
        /* `sh -c CMD NAME ARGS...`: NAME is $0 and ARGS are $1.., as POSIX
         * and bash have it. NAME was taken as $1:
         *
         *     find . -exec sh -c 'echo "$1"' _ {} \;     printed `_`
         *     sh -c 'echo $#' _ a b                       bash: 2, tsh: 3
         */
        if (argc > 3) {
            (void)shell_set_param0_hosted(argv[3]);
            if (argc > 4) (void)shell_set_args_hosted(argc - 4, argv + 4);
        }
        return shell_run_line_hosted(argv[2]);
    }
    /* --noediting: read the tty COOKED, no line editor. Same spelling and
     * same meaning as bash, because the reasons to want it are the same --
     * a harness comparing terminal byte streams, a terminal that mangles the
     * escape sequences, or simply preferring the line discipline. Consumed
     * here so it never reaches the script/-c arms below. */
    int noediting = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--noediting") != 0) continue;
        noediting = 1;
        for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
        argc--; argv[argc] = 0;
        i--;
    }

    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
        usage();
        return 2;
    }
    if (argc >= 2) {
        shell_set_interactive_hosted(false);
        if (argc > 2) (void)shell_set_args_hosted(argc - 2, argv + 2);
        return shell_run_script_hosted(argv[1]);
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

    /* PS1/PS2 come from the SHELL's variable table (they are rarely
     * exported, so getenv cannot see them), and prompts are written with a
     * bare write() so they reach the tty before the blocking read -- the
     * interactive gate frames its whole protocol on prompt sentinels.
     * Multi-line commands accumulate under PS2 using the same continuation
     * test the script accumulator uses. */
    static char line[4096];
    static char accum[16384];
    size_t alen = 0;
    int last = 0;
    int eofs = 0;
    int cont = 0;
    for (;;) {
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
            long r = tsh_readline(ps, line, sizeof line);
            if (r == TSH_LE_INTR) {
                interrupted = 1;
                n = 0;
            } else if (r == TSH_LE_EOF) {
                if (!cont) {
                    if (shell_opt_ignoreeof_hosted() && ++eofs < 10) {
                        fputs("Use \"exit\" to leave the shell.\n", stderr);
                        continue;
                    }
                    write(1, "exit\n", 5);
                    return last;
                }
                n = 0;
            } else {
                n = (size_t)r;
            }
            goto have_line;
        }

        write(1, ps, strlen(ps));
        for (;;) {
            char c;
            long r = read(0, &c, 1);
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
                        fputs("Use \"exit\" to leave the shell.\n", stderr);
                        break;
                    }
                    /* bash announces an EOF exit -- the "exit" you see
                     * when ^D ends an interactive shell is printed by the
                     * shell, not echoed by the terminal. */
                    write(1, "exit\n", 5);
                    return last;
                }
                break;
            }
            if (c == '\n') break;
            if (n + 1 < sizeof line) line[n++] = c;
        }
        line[n] = '\0';
    have_line:
        if (interrupted) {
            /* The tty already echoed "^C" with no newline, so the next
             * prompt would run on from it. bash prints the newline; so do
             * we. Everything accumulated (including a half-finished PS2
             * continuation) is discarded, and $? becomes 130. */
            write(1, "\n", 1);
            alen = 0;
            cont = 0;
            /* Consume it HERE. Leaving the flag set for the interpreter to
             * find would make the next command the user types unwind
             * instead of run -- one ^C silently eating one command. */
            (void)shell_take_interrupt();
            last = 130;
            continue;
        }
        if (n == 0 && !cont) continue;
        eofs = 0;
        if (alen + n + 2 > sizeof accum) { alen = 0; cont = 0; continue; }
        memcpy(accum + alen, line, n);
        alen += n;
        accum[alen] = '\0';
        int inc = shell_line_incomplete_hosted(accum);
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
        last = shell_run_line_hosted(accum);
        /* ^C that landed DURING the command, rather than at the prompt.
         * The tty echoed "^C" with no newline of its own and the read loop
         * never saw it (it was not blocked -- the command was running), so
         * the newline bash prints has to come from here or the next prompt
         * runs on from the "^C". Measured against bash on the gate's pty:
         * two bytes, and the only thing between a passing case and a
         * failing one. */
        if (g_tsh_sigint) { write(1, "\n", 1); g_tsh_sigint = 0; }
        alen = 0;
        cont = 0;
        {
            int st;
            if (shell_wants_exit_hosted(&st)) {
                /* Measured on the gate's pty: interactive bash announces
                 * "exit" for the exit BUILTIN too, not only for ^D. */
                write(1, "exit\n", 5);
                return st;
            }
        }
    }
}
