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
#include <fcntl.h>
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

/* Caps for the argv/envp copy in proc_spawn. shell.c enforces its own ARG_MAX
 * before it gets here; these only bound the stack arrays. */
#define ARG_MAX_HOST 128
#define ENV_MAX_HOST 128

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

void kputc(char c) {
    if (g_sink) {
        g_sink(g_sink_ctx, c);
        if (g_sink_suppress) return;   /* captured INSTEAD of printed */
    }
    write(1, &c, 1);
}

void kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
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
    if (f->kind == FILE_KIND_NULL) return 0;          /* `<&-` reads as EOF */
    if (f->kind != FILE_KIND_VFS) return -1;
    int fd = VF_FD(&f->vfs);
    long got = read(fd, buf, n);
    if (got > 0) f->vfs.pos += (size_t)got;
    return got;
}

long file_write(struct file *f, const void *buf, size_t n) {
    if (!f) return -1;
    if (f->kind == FILE_KIND_NULL) return (long)n;    /* `>&-` discards */
    if (f->kind != FILE_KIND_VFS) return -1;
    int fd = VF_FD(&f->vfs);
    long put = write(fd, buf, n);
    if (put > 0) f->vfs.pos += (size_t)put;
    return put;
}

/* The kernel keeps the offset in a shared open file description so that
 * fork/dup2 siblings advance one cursor; here every handle is its own, so the
 * embedded cursor IS the description and these are plain accessors. They exist
 * because src/shell.c calls them (for `>>`), and it is the same source file in
 * both builds. */
/* The descriptor owns the offset here, exactly as it does in a real process:
 * handles produced by file_clone() share one fd, so they share one position
 * and appending from either of them lands after the other. */
size_t file_pos_get(struct file *f) { return f ? f->vfs.pos : 0; }

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
    out->mode = st.st_mode & 07777;
    out->uid  = st.st_uid;
    out->gid  = st.st_gid;
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

int proc_spawn(const struct proc_spec *spec) {
    if (!spec || !spec->path) return -1;

    /* A NULL fd in the spec means "inherit"; the kernel would hand the child
     * the console. Here the standard descriptors already are that. */
    int fd0 = spec->fd0 ? VF_FD(&spec->fd0->vfs) : 0;
    int fd1 = spec->fd1 ? VF_FD(&spec->fd1->vfs) : 1;
    int fd2 = spec->fd2 ? VF_FD(&spec->fd2->vfs) : 2;

    /* A redirect target opened by shell_open_vfs_file() carries the shell's
     * own offset in ->vfs.pos (that is how `>>` works). The child inherits the
     * DESCRIPTOR, whose offset is independent, so hand it over positioned
     * where the shell thinks it is -- otherwise `echo a >> f` from a child
     * lands at byte 0 and overwrites what the parent appended. */
    if (spec->fd1 && fd1 > 2) (void)lseek(fd1, (off_t)spec->fd1->vfs.pos, SEEK_SET);
    if (spec->fd2 && fd2 > 2) (void)lseek(fd2, (off_t)spec->fd2->vfs.pos, SEEK_SET);
    if (spec->fd0 && fd0 > 2) (void)lseek(fd0, (off_t)spec->fd0->vfs.pos, SEEK_SET);

    /* proc_spec carries argv WITHOUT a NULL terminator (argc says how many);
     * toby_spawn wants the NULL-terminated POSIX shape. */
    char *argv[ARG_MAX_HOST + 1];
    int n = spec->argc;
    if (n > ARG_MAX_HOST) n = ARG_MAX_HOST;
    for (int i = 0; i < n; i++) argv[i] = spec->argv[i];
    argv[n] = 0;

    char *envp[ENV_MAX_HOST + 1];
    int e = spec->envc;
    if (e > ENV_MAX_HOST) e = ENV_MAX_HOST;
    for (int i = 0; i < e; i++) envp[i] = spec->envp[i];
    envp[e] = 0;

    return (int)toby_spawn(spec->path, argv, spec->envp ? envp : 0,
                           fd0, fd1, fd2);
}

int proc_wait(int pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WEXITSTATUS(status);
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

/* The hosted shell reads stdin line-at-a-time in main(); the kernel's
 * keyboard ring has no userspace counterpart. -1 == "nothing queued". */
int kbd_trygetc(void) { return -1; }

void signal_send_to_pid(int pid, int sig) { (void)kill(pid, sig); }

/* ---- main --------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage: tsh [script [args...]]\n"
        "       tsh -c 'command'\n"
        "       tsh              (interactive)\n");
}

int main(int argc, char **argv) {
    shell_init_hosted(argv[0]);

    /* `-c` and a script file are NOT terminal sessions. Only the loop at the
     * bottom of this function is, and it leaves the default alone. Getting
     * this backwards makes a script expand aliases where bash does not. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        shell_set_interactive_hosted(false);
        if (argc > 3) (void)shell_set_args_hosted(argc - 3, argv + 3);
        return shell_run_line_hosted(argv[2]);
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

    /* Interactive: one line at a time. Deliberately not the kernel's raw-mode
     * editor -- history and cursor keys are a separate concern from the
     * language, and this keeps the hosted path free of terminal state. */
    char line[4096];
    int last = 0;
    for (;;) {
        fputs("tsh$ ", stdout);
        size_t n = 0;
        for (;;) {
            char c;
            long r = read(0, &c, 1);
            if (r <= 0) { if (n == 0) { fputs("\n", stdout); return last; } break; }
            if (c == '\n') break;
            if (n + 1 < sizeof line) line[n++] = c;
        }
        line[n] = '\0';
        if (n == 0) continue;
        last = shell_run_line_hosted(line);
    }
}
