/* shell.c -- line editor + builtin command dispatch.
 *
 * The editor stores the in-progress line in a fixed buffer. Printable
 * ASCII appends to the buffer and echoes; backspace erases and rewinds;
 * Enter NUL-terminates, calls execute_line(), and prints the next prompt.
 *
 * Command parsing is intentionally small but shell-shaped: quoting,
 * environment expansion, command lists, conditionals, pipelines,
 * background jobs, and simple redirection are parsed before dispatch.
 * This is not a complete Bash clone, but it gives tobyOS the POSIX-like
 * grammar expected from an everyday command shell.
 */

#include <tobyos/shell.h>
#include <tobyos/keyboard.h>
#include <tobyos/console.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/heap.h>
#include <tobyos/pit.h>
#include <tobyos/elf.h>
#include <tobyos/limine.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/smp.h>
#include <tobyos/percpu.h>
#include <tobyos/apic.h>
#include <tobyos/vfs.h>
#include <tobyos/file.h>
#include <tobyos/pipe.h>
#include <tobyos/net.h>
#include <tobyos/arp.h>
#include <tobyos/socket.h>
#include <tobyos/dns.h>
#include <tobyos/tcp.h>
#include <tobyos/http.h>
#include <tobyos/gui.h>
#include <tobyos/users.h>
#include <tobyos/pkg.h>
#include <tobyos/cap.h>
#include <tobyos/perf.h>
#include <tobyos/installer.h>
#include <tobyos/blk.h>
#include <tobyos/partition.h>
#include <tobyos/tobyfs.h>
#include <tobyos/fat32.h>
#include <tobyos/acpi.h>
#include <tobyos/devtest.h>
#include <tobyos/hwinfo.h>
#include <tobyos/drvmatch.h>
#include <tobyos/slog.h>
#include <tobyos/sectest.h>
#include <tobyos/spinlock.h>

extern volatile struct limine_module_request module_req;

#define LINE_MAX 512
/* Parse scratch: also the cap on a multi-line compound command joined
 * into one line by shell_run_script_text. Defined here rather than beside
 * SHELL_TOKEN_MAX because that reader runs long before that point. */
#define SHELL_PARSE_BUF_MAX (LINE_MAX * 2)
/* Word-boundary marker planted by "$@" expansion; see
 * shell_append_positional_join(). Never appears in real shell text. */
/* Descriptors the shell can hold open. POSIX requires at least 0..9, and
 * `exec 3> log` is ordinary usage; three slots meant `exec 3>` and `>&3`
 * were rejected outright. Children still inherit only 0..2 (what
 * proc_spec carries), which is enough for the shell to use high
 * descriptors for its own redirections. */
#define SHELL_FD_MAX 10

#define SHELL_ARG_MARK '\x01'
/* Toggles "do not field-split" while expanding. See the note above
 * shell_append_positional_join for why a marker in the text is the only
 * place this information can live. Stripped before any word reaches argv. */
#define SHELL_NOSPLIT_MARK '\x02'
#define ARG_MAX  32
#define SHELL_ALIAS_MAX 32
#define SHELL_FUNC_MAX 32
#define SHELL_HEREDOC_MAX 4
#define SHELL_HEREDOC_BODY_MAX PIPE_BUF_SZ

/* Serialises execute_line() between keyboard shell_poll and remote
 * tcp_shell (both use the shared `line` buffer). */
static spinlock_t g_shell_line_lock = SPINLOCK_INIT;

static char line[LINE_MAX];
static size_t line_len;

static shell_write_fn_t g_shell_out;
static void *g_shell_out_ctx;
static struct file *g_shell_in;
static int g_last_status;
static int g_last_bg_pid;
static int g_getopts_last_optind;
static int g_getopts_char_index;
static bool g_getopts_internal_optind_write;
static struct file *g_shell_fd[SHELL_FD_MAX];

/* Command history, kept so `fc` has something to list and re-run. A ring:
 * g_hist_base is the history number of slot 0, so numbers keep climbing as
 * entries are overwritten. */
#define SHELL_HIST_MAX 128
static char *g_hist[SHELL_HIST_MAX];
static int g_hist_count;          /* entries currently held (<= MAX) */
static unsigned long g_hist_base; /* history number of the oldest entry */

/* $LINENO: line number of the command currently executing, counted within the
 * script (or from shell start for interactive input). */
static unsigned long g_shell_lineno;

static char *g_param0;
static char *g_positional[ARG_MAX];
static int g_positional_count;

enum shell_flow {
    SHELL_FLOW_NONE = 0,
    SHELL_FLOW_BREAK,
    SHELL_FLOW_CONTINUE,
    SHELL_FLOW_RETURN,
    SHELL_FLOW_EXIT,
};

static enum shell_flow g_shell_flow;
static int g_shell_flow_status;
static int g_shell_loop_depth;
static int g_script_depth;
static int g_subshell_depth;

static bool g_opt_errexit;   /* set -e */

/* Depth of "failure here is a DECISION, not an error".
 *
 * POSIX exempts a command from `set -e` when its exit status is being tested
 * rather than acted on: the condition of if/elif/while/until, any command in
 * an AND-OR list except the last, and the operand of `!`. A counter rather
 * than a flag because these nest -- a while condition may contain a pipeline
 * containing an if -- and the innermost scope must not clear the outer one. */
static int g_errexit_suspend;
static bool g_opt_nounset;   /* set -u */
static bool g_opt_xtrace;    /* set -x */
static bool g_opt_noglob;    /* set -f */
static bool g_opt_verbose;   /* set -v */
static bool g_opt_noclobber; /* set -C */
static bool g_opt_notify;    /* set -b */
static bool g_opt_noexec;    /* set -n */
/* Sticky non-zero once `set -n` has seen a malformed command, so the script
 * as a whole reports the failure it found. */
static int g_noexec_error;
static bool g_opt_allexport; /* set -a */

/* Alias expansion is an INTERACTIVE-shell behaviour. A script gets it only
 * when `shopt -s expand_aliases` asks for it. tsh expanded unconditionally,
 * so every alias defined in a script took effect where bash ignored it. */
static bool g_opt_expand_aliases;
/* True for a terminal session, false while running a script or `-c`. The
 * kernel shell is always a terminal session, so this defaults true and only
 * the hosted entry points clear it. */
static bool g_interactive = true;

struct shell_alias {
    char *name;
    char *value;
};

struct shell_function {
    char *name;
    char *body;
};

struct shell_heredoc {
    char body[SHELL_HEREDOC_BODY_MAX];
    size_t len;
};

static struct shell_alias g_aliases[SHELL_ALIAS_MAX];
static struct shell_function g_functions[SHELL_FUNC_MAX];
static struct shell_heredoc g_heredocs[SHELL_HEREDOC_MAX];
static int g_heredoc_count;
static char *g_traps[SIG_MAX];
static bool g_trap_running;

static bool g_heredoc_collecting;
static bool g_continuation_active;
static void prompt(void);
static void execute_line_text(const char *src);
static void execute_line_text_inner(const char *src);

/* 0 while no line is executing; 1 inside an input line; >1 inside a segment,
 * body or condition re-entering the executor. Only depth 0 is a NEW line. */
static int g_exec_line_depth;
static bool shell_name_is_valid(const char *s, size_t n);
static bool shell_special_builtin_name(const char *name);
static int shell_expand_param_word(const char *word, char *out, size_t cap);
static int shell_expand_literal_quotes(const char *word, char *out, size_t cap);
static int shell_append_char(char *buf, size_t *pos, size_t cap, char c);
static int shell_append_str(char *buf, size_t *pos, size_t cap, const char *s);
static bool shell_word_has(const char *word, char c);
static const char *shell_skip_blanks(const char *s);
static int parse_int(const char *s, int *out);
static int shell_run_exit_trap(int status);
static bool shell_starts_with_word(const char *s, const char *word);
static inline bool is_space(char c);
static bool shell_group_open_at(const char *p);
static bool shell_group_close_at(const char *p);
static int shell_copy_segment(char *dst, size_t cap,
                              const char *start, const char *end);
static bool shell_word_boundary_before(const char *start, const char *p);
struct shell_simple;
struct shell_io_frame;
static int shell_enter_io_frame(struct shell_simple *cmd, const char *label,
                                struct shell_io_frame *frame);
static void shell_restore_io_frame(struct shell_io_frame *frame);

static void shell_set_status(int status) {
    g_last_status = status;
}

void shell_set_output(shell_write_fn_t fn, void *ctx) {
    g_shell_out = fn;
    g_shell_out_ctx = ctx;
}

void shell_write(const char *s) {
    if (!s) return;

    if (g_shell_out) {
        g_shell_out(s, g_shell_out_ctx);
    } else {
        while (*s) kputc(*s++);
    }
}

static void shell_putc(char c) {
    char s[2] = { c, '\0' };
    if (g_shell_out) g_shell_out(s, g_shell_out_ctx);
    else kputc(c);
}

void shell_printf(const char *fmt, ...) {
    char buf[512];

    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    shell_write(buf);
}
/* ---- job table (milestone 8) ----------------------------------- *
 *
 * Tiny fixed-size table tracking shell-launched background processes.
 * A "job" is a single user-mode process spawned with trailing `&`
 * (multi-stage pipelines + `&` are intentionally not supported in this
 * milestone). The shell:
 *   - assigns a small monotonically-increasing job id per launch
 *   - prints "[id] pid" on launch
 *   - reaps completed bg jobs in shell_poll() (non-blocking proc_wait
 *     because the child is already PROC_TERMINATED)
 *   - lets the user query (`jobs`) or foreground (`fg <id>`) them
 */

#define JOB_MAX       8
#define JOB_NAME_MAX  32

struct job {
    int  id;                   /* shell-assigned, 1..  -- 0 = empty slot */
    int  pid;                  /* kernel PID of the bg proc */
    char name[JOB_NAME_MAX];   /* argv[0] copy for display */
};

static struct job g_jobs[JOB_MAX];
static int        g_next_job_id = 1;

static int jobs_add(int pid, const char *name) {
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].id == 0) {
            g_jobs[i].id  = g_next_job_id++;
            g_jobs[i].pid = pid;
            size_t n = 0;
            if (name) {
                while (n + 1 < JOB_NAME_MAX && name[n]) {
                    g_jobs[i].name[n] = name[n]; n++;
                }
            }
            g_jobs[i].name[n] = 0;
            return g_jobs[i].id;
        }
    }
    return -1;       /* table full */
}

static struct job *jobs_find(int id) {
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].id == id) return &g_jobs[i];
    }
    return 0;
}

static struct job *jobs_find_pid(int pid) {
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].id != 0 && g_jobs[i].pid == pid) return &g_jobs[i];
    }
    return 0;
}

static void jobs_remove(struct job *j) {
    if (!j) return;
    j->id  = 0;
    j->pid = 0;
    j->name[0] = 0;
}

/* Called from shell_poll(): for every tracked bg job, ask the kernel
 * what state its PID is in. If TERMINATED, proc_wait() reaps it (no
 * blocking, since the child is already terminated) and we report the
 * exit code + remove from the table.
 *
 * We also drop entries whose PCB slot has gone UNUSED out from under us
 * (e.g. somehow reaped elsewhere) -- this should not happen today, but
 * a stale jobs[] entry would be confusing. */
static void jobs_reap_finished(void) {
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].id == 0) continue;
        struct proc *p = proc_lookup(g_jobs[i].pid);
        if (!p) {
            kprintf("[%d] removed (pid=%d gone)\n",
                    g_jobs[i].id, g_jobs[i].pid);
            jobs_remove(&g_jobs[i]);
            continue;
        }
        if (p->state == PROC_TERMINATED) {
            int jid = g_jobs[i].id;
            int pid = g_jobs[i].pid;
            int rc  = proc_wait(pid);   /* doesn't block; just reaps */
            kprintf("\n[%d] done  pid=%d  '%s'  exit=%d (0x%x)\n",
                    jid, pid, g_jobs[i].name, rc, (unsigned)rc);
            jobs_remove(&g_jobs[i]);
            /* Reprint the prompt so the user gets a clean line back. */
            prompt();
            for (size_t k = 0; k < line_len; k++) shell_putc(line[k]);
        }
    }
}

/* ---- shell environment table (milestone 25C) ---------------------- *
 *
 * The shell holds a NULL-terminated array of "KEY=VALUE" strings that
 * is plumbed through to every spawned child via spec.envc/envp. This is
 * the userspace's view of the OS environment -- libtoby (`environ`,
 * `getenv`, `setenv`, ...) reads and mutates the per-process copy
 * unpacked from the user stack at exec time.
 *
 * Storage strategy: every entry is a fresh kmalloc()'d "KEY=VALUE\0"
 * blob. Replacing a key frees the old slot; unsetenv frees + compacts;
 * a fresh "K=V" allocation goes through env_set_kv. We deliberately
 * keep this O(N) -- the table caps at ~32 entries, which is plenty for
 * a hobby OS and lets us avoid a real hash table.
 *
 * Defaults stamped at shell_init:
 *   PATH=/bin
 *   HOME=/
 *   USER=admin
 *   PWD=/
 *   SHELL=tobysh
 */

/* ENV_MAX was 32, which is not a limit POSIX allows a shell to have: the
 * 33rd assignment in a script failed with "env: table full". The third-party
 * conformance corpus has cases that define more than that in a loop. */
#define ENV_MAX 512
#define SHELL_READONLY_MAX 16

static char *g_env[ENV_MAX + 1];     /* +1 reserved for NULL terminator */
static int   g_envc = 0;
static char *g_readonly[SHELL_READONLY_MAX];

/* Per-variable flags, parallel to g_env.
 *
 * g_env stays an array of bare "KEY=VALUE" blobs because it is handed to
 * proc_spawn as envp directly -- putting the flags in the blob would mean
 * rebuilding the array on every spawn. The two arrays are kept in step by the
 * four functions that mutate g_env (env_set_kv, env_remove_at, the frame
 * save/restore pair); nothing else may touch g_envc.
 *
 * SHVAR_EXPORTED is the one that changes observable behaviour: before this
 * existed, every shell variable reached every child process, so
 *     x=1; env | grep x
 * printed x=1 where POSIX (and bash, and dash) print nothing. */
#define SHVAR_EXPORTED  0x01u
#define SHVAR_READONLY  0x02u
static unsigned char g_env_flags[ENV_MAX];

/* Length of "KEY" up to (but not including) the '='. Returns the
 * byte count, 0 if `kv` doesn't start with at least one non-'=' char. */
static size_t env_key_len(const char *kv) {
    if (!kv) return 0;
    const char *eq = kv;
    while (*eq && *eq != '=') eq++;
    if (eq == kv) return 0;
    return (size_t)(eq - kv);
}

/* Find the index of the entry whose key matches `key`/`klen`, or -1. */
static int env_find(const char *key, size_t klen) {
    if (!key || klen == 0) return -1;
    for (int i = 0; i < g_envc; i++) {
        const char *e = g_env[i];
        size_t elen = env_key_len(e);
        if (elen == klen && memcmp(e, key, klen) == 0) return i;
    }
    return -1;
}

static int shell_readonly_find(const char *key, size_t klen) {
    if (!key || klen == 0) return -1;
    for (int i = 0; i < SHELL_READONLY_MAX; i++) {
        if (!g_readonly[i]) continue;
        if (strlen(g_readonly[i]) == klen &&
            memcmp(g_readonly[i], key, klen) == 0) {
            return i;
        }
    }
    return -1;
}

static void shell_readonly_print_error(const char *key, size_t klen,
                                       const char *label) {
    char name[64];
    size_t n = klen;
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    if (key && n > 0) memcpy(name, key, n);
    name[n] = '\0';
    kprintf("%s: %s: readonly variable\n", label ? label : "shell", name);
}

static bool shell_readonly_key(const char *key, size_t klen) {
    return shell_readonly_find(key, klen) >= 0;
}

static int shell_readonly_mark(const char *key, size_t klen) {
    if (!key || klen == 0) return -1;
    if (shell_readonly_key(key, klen)) return 0;
    for (int i = 0; i < SHELL_READONLY_MAX; i++) {
        if (g_readonly[i]) continue;
        char *copy = (char *)kmalloc(klen + 1);
        if (!copy) return -1;
        memcpy(copy, key, klen);
        copy[klen] = '\0';
        g_readonly[i] = copy;
        return 0;
    }
    kprintf("readonly: table full (max %d)\n", SHELL_READONLY_MAX);
    return -1;
}

static bool shell_key_eq(const char *key, size_t klen, const char *lit) {
    return key && lit && strlen(lit) == klen && memcmp(key, lit, klen) == 0;
}

static void shell_getopts_note_external_optind_write(const char *key,
                                                     size_t klen) {
    if (g_getopts_internal_optind_write) return;
    if (!shell_key_eq(key, klen, "OPTIND")) return;
    g_getopts_last_optind = 0;
    g_getopts_char_index = 1;
}

/* Walk the table looking for `key` (NUL-terminated) and return its
 * value pointer (right after '='), or NULL. The pointer aliases into
 * g_env[i], so callers must not retain it across env mutations. */
static const char *env_get(const char *key) {
    if (!key) return 0;
    size_t klen = strlen(key);
    int idx = env_find(key, klen);
    if (idx < 0) return 0;
    return g_env[idx] + klen + 1;
}

/* Drop entry at index `idx` (free its blob, shift the tail down). */
static void env_remove_at(int idx) {
    if (idx < 0 || idx >= g_envc) return;
    kfree(g_env[idx]);
    for (int i = idx; i < g_envc - 1; i++) {
        g_env[i] = g_env[i + 1];
        g_env_flags[i] = g_env_flags[i + 1];
    }
    g_envc--;
    g_env[g_envc] = 0;
    g_env_flags[g_envc] = 0;
}

/* Install a fully-formed "KEY=VALUE" string. `kv_in` is COPIED -- the
 * caller retains ownership of its storage. Replaces an existing key
 * in place (frees the previous slot) so the table stays compact. */
static int env_set_kv(const char *kv_in) {
    if (!kv_in) return -1;
    size_t klen = env_key_len(kv_in);
    if (klen == 0) return -1;             /* "=value" or "" -- reject */
    if (shell_readonly_key(kv_in, klen)) {
        shell_readonly_print_error(kv_in, klen, "shell");
        return -1;
    }

    size_t total = strlen(kv_in);
    char *blob = (char *)kcalloc(1, total + 1);
    if (!blob) return -1;
    memcpy(blob, kv_in, total + 1);

    int idx = env_find(kv_in, klen);
    if (idx >= 0) {
        /* Assigning to an existing variable does not change its export state:
         * once exported, `x=2` keeps x in the child environment. That is what
         * POSIX means by the export ATTRIBUTE being on the name. */
        kfree(g_env[idx]);
        g_env[idx] = blob;
        if (g_opt_allexport) g_env_flags[idx] |= SHVAR_EXPORTED;
        shell_getopts_note_external_optind_write(kv_in, klen);
        return 0;
    }
    if (g_envc >= ENV_MAX) {
        kfree(blob);
        kprintf("env: table full (max %d)\n", ENV_MAX);
        return -1;
    }
    /* A brand-new variable is NOT exported unless `set -a` is in force. */
    g_env_flags[g_envc] = g_opt_allexport ? SHVAR_EXPORTED : 0u;
    g_env[g_envc++] = blob;
    g_env[g_envc]   = 0;
    shell_getopts_note_external_optind_write(kv_in, klen);
    return 0;
}

/* Convenience: build "KEY=VALUE" from two NUL-terminated halves and
 * hand it to env_set_kv. */
static int env_set(const char *key, const char *val) {
    if (!key || !val) return -1;
    size_t klen = strlen(key);
    size_t vlen = strlen(val);
    if (klen == 0) return -1;
    size_t need = klen + 1 + vlen + 1;
    if (need > 256) return -1;
    char tmp[256];
    memcpy(tmp, key, klen);
    tmp[klen] = '=';
    memcpy(tmp + klen + 1, val, vlen + 1);
    return env_set_kv(tmp);
}

static int env_unset(const char *key) {
    if (!key) return -1;
    size_t klen = strlen(key);
    if (shell_readonly_key(key, klen)) {
        shell_readonly_print_error(key, klen, "unset");
        return -1;
    }
    int idx = env_find(key, klen);
    if (idx >= 0) env_remove_at(idx);
    shell_getopts_note_external_optind_write(key, klen);
    return 0;
}

/* Stamp the boot-time defaults. Called once from shell_init. Failures
 * are logged but non-fatal -- a degraded env is still usable. */
static void env_init_defaults(void) {
    g_envc = 0;
    g_env[0] = 0;
    for (int i = 0; i < ENV_MAX; i++) g_env_flags[i] = 0;
    for (int i = 0; i < SHELL_READONLY_MAX; i++) {
        if (g_readonly[i]) kfree(g_readonly[i]);
        g_readonly[i] = 0;
    }
    if (env_set("PATH",  "/bin")    < 0) kprintf("env: default PATH set failed\n");
    if (env_set("HOME",  "/")       < 0) kprintf("env: default HOME set failed\n");
    if (env_set("USER",  "admin")   < 0) kprintf("env: default USER set failed\n");
    if (env_set("PWD",   "/")       < 0) kprintf("env: default PWD set failed\n");
    if (env_set("SHELL", "tobysh")  < 0) kprintf("env: default SHELL set failed\n");
    if (env_set("OPTIND", "1")      < 0) kprintf("env: default OPTIND set failed\n");
    /* POSIX 2.5.3: IFS shall be SET by default to <space><tab><newline>.
     *
     * Every splitting site here already falls back to those three when IFS is
     * absent, so behaviour was right -- but the VARIABLE did not exist, so
     * `echo "[$IFS]"` printed empty where bash prints the three characters,
     * and a script that saves and restores IFS around a loop restored it to
     * nothing. Setting it makes the value observable and the fallback
     * redundant rather than load-bearing. */
    if (env_set("IFS", " \t\n")     < 0) kprintf("env: default IFS set failed\n");

    /* PATH, HOME, PWD, SHELL and USER are exported: a child process is
     * expected to inherit them, and every real shell does. IFS and OPTIND are
     * deliberately NOT -- POSIX describes both as shell variables, and bash
     * keeps them out of the child environment unless asked. Before the export
     * flag existed the distinction could not be expressed and all seven were
     * shipped to every child. */
    static const char *const exported[] = { "PATH", "HOME", "USER", "PWD",
                                            "SHELL", 0 };
    for (int e = 0; exported[e]; e++) {
        int idx = env_find(exported[e], strlen(exported[e]));
        if (idx >= 0) g_env_flags[idx] |= SHVAR_EXPORTED;
    }
}

/* ---- POSIX positional parameters ---------------------------------- */

struct shell_param_frame {
    char *param0;
    char *positional[ARG_MAX];
    int positional_count;
};

static char *shell_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *out = (char *)kmalloc(n + 1);
    if (!out) return 0;
    memcpy(out, s, n + 1);
    return out;
}

static void shell_free_current_params(void) {
    if (g_param0) {
        kfree(g_param0);
        g_param0 = 0;
    }
    for (int i = 0; i < g_positional_count; i++) {
        if (g_positional[i]) kfree(g_positional[i]);
        g_positional[i] = 0;
    }
    g_positional_count = 0;
}

static void shell_move_params_to_frame(struct shell_param_frame *f) {
    memset(f, 0, sizeof(*f));
    f->param0 = g_param0;
    f->positional_count = g_positional_count;
    for (int i = 0; i < g_positional_count; i++) {
        f->positional[i] = g_positional[i];
        g_positional[i] = 0;
    }
    g_param0 = 0;
    g_positional_count = 0;
}

static void shell_restore_params_from_frame(struct shell_param_frame *f) {
    shell_free_current_params();
    g_param0 = f->param0;
    g_positional_count = f->positional_count;
    for (int i = 0; i < g_positional_count; i++) {
        g_positional[i] = f->positional[i];
        f->positional[i] = 0;
    }
    f->param0 = 0;
    f->positional_count = 0;
}

static int shell_set_param0(const char *name) {
    char *copy = shell_strdup(name && *name ? name : "tobysh");
    if (!copy) return -1;
    if (g_param0) kfree(g_param0);
    g_param0 = copy;
    return 0;
}

static int shell_set_positional_params(int argc, char **argv) {
    if (argc < 0 || argc > ARG_MAX) return -1;

    char *tmp[ARG_MAX];
    memset(tmp, 0, sizeof(tmp));
    for (int i = 0; i < argc; i++) {
        tmp[i] = shell_strdup(argv[i]);
        if (!tmp[i]) {
            for (int j = 0; j < i; j++) kfree(tmp[j]);
            return -1;
        }
    }

    for (int i = 0; i < g_positional_count; i++) {
        if (g_positional[i]) kfree(g_positional[i]);
        g_positional[i] = 0;
    }
    g_positional_count = argc;
    for (int i = 0; i < argc; i++) g_positional[i] = tmp[i];
    return 0;
}

static int shell_enter_script_params(struct shell_param_frame *frame,
                                     const char *param0,
                                     int argc, char **argv) {
    shell_move_params_to_frame(frame);
    if (shell_set_param0(param0) < 0 ||
        shell_set_positional_params(argc, argv) < 0) {
        shell_restore_params_from_frame(frame);
        return -1;
    }
    return 0;
}

static int shell_enter_dot_params(struct shell_param_frame *frame,
                                  int argc, char **argv) {
    shell_move_params_to_frame(frame);
    const char *old0 = frame->param0 ? frame->param0 : "tobysh";
    if (shell_set_param0(old0) < 0 ||
        shell_set_positional_params(argc, argv) < 0) {
        shell_restore_params_from_frame(frame);
        return -1;
    }
    return 0;
}

/* ---- aliases, functions, and here-doc state ------------------------ */

static int shell_alias_find(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
        if (g_aliases[i].name && strcmp(g_aliases[i].name, name) == 0) return i;
    }
    return -1;
}

static const char *shell_alias_value(const char *name) {
    int idx = shell_alias_find(name);
    return idx >= 0 ? g_aliases[idx].value : 0;
}

/* An alias NAME is not a variable name. bash accepts anything that is not
 * whitespace, an operator, a quote or a paren -- `echo-x` and `ll.` are legal
 * aliases and illegal variables. shell_alias_set used the VARIABLE rule, so
 * `alias echo-x=...` failed with "failed to set" while the expander, which
 * uses shell_alias_name_char, would happily have expanded it. The definition
 * side and the expansion side must agree on what a name is, so they now share
 * one predicate. */
static bool shell_alias_name_char(char c);

static bool shell_alias_name_ok(const char *name) {
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++)
        if (*p == '=' || *p == '/' || !shell_alias_name_char(*p)) return false;
    return true;
}

static int shell_alias_set(const char *name, const char *value) {
    if (!name || !value || !shell_alias_name_ok(name)) return -1;

    char *ncopy = shell_strdup(name);
    char *vcopy = shell_strdup(value);
    if (!ncopy || !vcopy) {
        kfree(ncopy);
        kfree(vcopy);
        return -1;
    }

    int idx = shell_alias_find(name);
    if (idx < 0) {
        for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
            if (!g_aliases[i].name) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        kfree(ncopy);
        kfree(vcopy);
        return -1;
    }

    kfree(g_aliases[idx].name);
    kfree(g_aliases[idx].value);
    g_aliases[idx].name = ncopy;
    g_aliases[idx].value = vcopy;
    return 0;
}

static void shell_alias_unset(const char *name) {
    int idx = shell_alias_find(name);
    if (idx < 0) return;
    kfree(g_aliases[idx].name);
    kfree(g_aliases[idx].value);
    g_aliases[idx].name = 0;
    g_aliases[idx].value = 0;
}

static void shell_alias_clear_all(void) {
    for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
        shell_alias_unset(g_aliases[i].name);
    }
}

static int shell_function_find(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < SHELL_FUNC_MAX; i++) {
        if (g_functions[i].name && strcmp(g_functions[i].name, name) == 0) return i;
    }
    return -1;
}

static struct shell_function *shell_function_lookup(const char *name) {
    int idx = shell_function_find(name);
    return idx >= 0 ? &g_functions[idx] : 0;
}

static int shell_function_set(const char *name, const char *body) {
    if (!name || !body || !shell_name_is_valid(name, strlen(name))) return -1;
    if (shell_special_builtin_name(name)) {
        kprintf("%s: cannot define a function with a special builtin name\n",
                name);
        return -1;
    }

    char *ncopy = shell_strdup(name);
    char *bcopy = shell_strdup(body);
    if (!ncopy || !bcopy) {
        kfree(ncopy);
        kfree(bcopy);
        return -1;
    }

    int idx = shell_function_find(name);
    if (idx < 0) {
        for (int i = 0; i < SHELL_FUNC_MAX; i++) {
            if (!g_functions[i].name) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        kfree(ncopy);
        kfree(bcopy);
        return -1;
    }

    kfree(g_functions[idx].name);
    kfree(g_functions[idx].body);
    g_functions[idx].name = ncopy;
    g_functions[idx].body = bcopy;
    return 0;
}

static void shell_function_unset(const char *name) {
    int idx = shell_function_find(name);
    if (idx < 0) return;
    kfree(g_functions[idx].name);
    kfree(g_functions[idx].body);
    g_functions[idx].name = 0;
    g_functions[idx].body = 0;
}

/* Signal names for `kill -l`, `kill -NAME` and `trap NAME`. Indexed by
 * number, gaps included so the numbering stays true. */
struct shell_signal_name { int num; const char *name; };

static const struct shell_signal_name g_shell_signals[] = {
    { SIGHUP, "HUP" },   { SIGINT, "INT" },   { SIGQUIT, "QUIT" },
    { SIGILL, "ILL" },   { SIGTRAP, "TRAP" }, { SIGABRT, "ABRT" },
    { SIGBUS, "BUS" },   { SIGFPE, "FPE" },   { SIGKILL, "KILL" },
    { SIGUSR1, "USR1" }, { SIGSEGV, "SEGV" }, { SIGUSR2, "USR2" },
    { SIGPIPE, "PIPE" }, { SIGALRM, "ALRM" }, { SIGTERM, "TERM" },
    { SIGCHLD, "CHLD" }, { SIGCONT, "CONT" }, { SIGSTOP, "STOP" },
    { SIGTSTP, "TSTP" }, { SIGTTIN, "TTIN" }, { SIGTTOU, "TTOU" },
    { SIGURG, "URG" },   { SIGXCPU, "XCPU" }, { SIGXFSZ, "XFSZ" },
    { SIGVTALRM, "VTALRM" }, { SIGPROF, "PROF" }, { SIGWINCH, "WINCH" },
    { SIGIO, "IO" },     { SIGSYS, "SYS" },
};

static int shell_signal_by_name(const char *name) {
    if (!name || !*name) return -1;
    if (strncmp(name, "SIG", 3) == 0) name += 3;
    for (size_t i = 0; i < sizeof(g_shell_signals) / sizeof(g_shell_signals[0]);
         i++) {
        if (strcmp(g_shell_signals[i].name, name) == 0)
            return g_shell_signals[i].num;
    }
    return -1;
}

static const char *shell_signal_name(int num) {
    for (size_t i = 0; i < sizeof(g_shell_signals) / sizeof(g_shell_signals[0]);
         i++) {
        if (g_shell_signals[i].num == num) return g_shell_signals[i].name;
    }
    return 0;
}

static const char *shell_trap_name(int sig) {
    if (sig == 0) return "EXIT";
    return shell_signal_name(sig);
}

static void shell_trap_unset(int sig) {
    if (sig < 0 || sig >= SIG_MAX) return;
    if (g_traps[sig]) {
        kfree(g_traps[sig]);
        g_traps[sig] = 0;
    }
}

static void shell_trap_clear_all(void) {
    for (int i = 0; i < SIG_MAX; i++) shell_trap_unset(i);
}

struct shell_trap_frame {
    char *trap[SIG_MAX];
};

static int shell_trap_enter_child(struct shell_trap_frame *frame) {
    if (!frame) return -1;
    memset(frame, 0, sizeof(*frame));
    for (int i = 0; i < SIG_MAX; i++) {
        if (!g_traps[i]) continue;
        frame->trap[i] = shell_strdup(g_traps[i]);
        if (!frame->trap[i]) {
            for (int j = 0; j < i; j++) {
                if (frame->trap[j]) kfree(frame->trap[j]);
                frame->trap[j] = 0;
            }
            return -1;
        }
    }
    shell_trap_clear_all();
    return 0;
}

static void shell_trap_restore(struct shell_trap_frame *frame) {
    if (!frame) return;
    shell_trap_clear_all();
    for (int i = 0; i < SIG_MAX; i++) {
        g_traps[i] = frame->trap[i];
        frame->trap[i] = 0;
    }
}

static volatile int g_pending_signals[SIG_MAX];

static void shell_run_trap(int sig) {
    if (g_trap_running) return;
    if (sig < 0 || sig >= SIG_MAX) return;
    if (!g_traps[sig] || !*g_traps[sig]) return;

    char *action = shell_strdup(g_traps[sig]);
    if (!action) return;

    enum shell_flow saved_flow = g_shell_flow;
    int saved_flow_status = g_shell_flow_status;
    g_trap_running = true;
    g_shell_flow = SHELL_FLOW_NONE;
    g_shell_flow_status = 0;

    execute_line_text(action);

    if (g_shell_flow != SHELL_FLOW_EXIT) {
        g_shell_flow = saved_flow;
        g_shell_flow_status = saved_flow_status;
    }
    g_trap_running = false;
    kfree(action);
}

/* The pid the shell itself runs as, so a signal aimed at the shell can be
 * routed to its trap dispatcher. Recorded from the shell thread -- latching
 * it inside shell_owns_pid would record whichever thread happened to send the
 * first signal, and the shell's own pid is 0 here, so there is no sentinel
 * value to fall back on. */
static int g_shell_pid = -1;

static void shell_latch_pid(void) {
    struct proc *cur = current_proc();
    if (cur) g_shell_pid = cur->pid;
}

bool shell_owns_pid(int pid) {
    return g_shell_pid >= 0 && pid == g_shell_pid;
}

void shell_deliver_signal(int sig) {
    if (sig > 0 && sig < SIG_MAX) g_pending_signals[sig] = 1;
}

static void shell_check_pending_signals(void) {
    for (int i = 1; i < SIG_MAX; i++) {
        if (g_pending_signals[i]) {
            g_pending_signals[i] = 0;
            shell_run_trap(i);
        }
    }
}

static int shell_run_exit_trap(int status) {
    if (g_trap_running || !g_traps[0] || !*g_traps[0]) return status;

    char *action = shell_strdup(g_traps[0]);
    if (!action) return status;

    enum shell_flow saved_flow = g_shell_flow;
    int saved_flow_status = g_shell_flow_status;
    g_trap_running = true;
    g_shell_flow = SHELL_FLOW_NONE;
    g_shell_flow_status = 0;
    shell_set_status(status);

    execute_line_text(action);

    int final_status = status;
    if (g_shell_flow == SHELL_FLOW_EXIT) {
        final_status = g_shell_flow_status;
    }
    g_shell_flow = saved_flow;
    g_shell_flow_status = (saved_flow == SHELL_FLOW_EXIT)
                              ? final_status
                              : saved_flow_status;
    shell_set_status(final_status);
    g_trap_running = false;
    kfree(action);
    return final_status;
}

static void shell_heredoc_reset(void) {
    g_heredoc_count = 0;
    for (int i = 0; i < SHELL_HEREDOC_MAX; i++) {
        g_heredocs[i].len = 0;
        g_heredocs[i].body[0] = '\0';
    }
}

static int shell_heredoc_push(const char *body, size_t len) {
    if (g_heredoc_count >= SHELL_HEREDOC_MAX ||
        len + 1 > SHELL_HEREDOC_BODY_MAX) {
        return -1;
    }
    struct shell_heredoc *h = &g_heredocs[g_heredoc_count++];
    memcpy(h->body, body, len);
    h->body[len] = '\0';
    h->len = len;
    return 0;
}

static const struct shell_heredoc *shell_heredoc_pop(void) {
    if (g_heredoc_count <= 0) return 0;
    const struct shell_heredoc *h = &g_heredocs[0];
    for (int i = 1; i < g_heredoc_count; i++) {
        g_heredocs[i - 1] = g_heredocs[i];
    }
    g_heredoc_count--;
    return h;
}

/* ---- cwd/path helpers ------------------------------------------- *
 *
 * The VFS takes canonical absolute paths. User programs get cwd-aware
 * syscalls already; these helpers give kernel-side builtins the same
 * shell ergonomics for relative paths, ".", "..", and "~".
 */

static const char *shell_cwd(void) {
    struct proc *p = current_proc();
    if (p && p->cwd[0]) return p->cwd;
    const char *pwd = env_get("PWD");
    return (pwd && *pwd) ? pwd : "/";
}

static int shell_copy_path(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0 || !src) return -1;
    size_t n = strlen(src);
    if (n + 1 > cap) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

static int shell_canonicalize_path(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    const char *src = (in && *in) ? in : ".";
    char tmp[VFS_PATH_MAX];

    if (src[0] == '~' && (src[1] == '\0' || src[1] == '/')) {
        const char *home = env_get("HOME");
        if (!home || !*home) home = "/";
        size_t hlen = strlen(home);
        size_t slen = strlen(src + 1);
        if (hlen + slen + 1 > sizeof(tmp)) return -1;
        memcpy(tmp, home, hlen);
        memcpy(tmp + hlen, src + 1, slen + 1);
    } else if (src[0] == '/') {
        if (shell_copy_path(tmp, sizeof(tmp), src) < 0) return -1;
    } else {
        const char *cwd = shell_cwd();
        size_t clen = strlen(cwd);
        size_t slen = strlen(src);
        bool need_slash = (clen == 0 || cwd[clen - 1] != '/');
        if (clen + (need_slash ? 1 : 0) + slen + 1 > sizeof(tmp)) return -1;
        memcpy(tmp, cwd, clen);
        size_t pos = clen;
        if (need_slash) tmp[pos++] = '/';
        memcpy(tmp + pos, src, slen + 1);
    }

    char *parts[48];
    int count = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';

        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (count > 0) count--;
            continue;
        }
        if (count >= (int)(sizeof(parts) / sizeof(parts[0]))) return -1;
        parts[count++] = start;
    }

    if (count == 0) {
        if (cap < 2) return -1;
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    size_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < count; i++) {
        size_t n = strlen(parts[i]);
        if (pos + n + (i + 1 < count ? 1 : 0) + 1 > cap) return -1;
        memcpy(out + pos, parts[i], n);
        pos += n;
        if (i + 1 < count) out[pos++] = '/';
    }
    out[pos] = '\0';
    return 0;
}

#ifdef SHELL_HOSTED
/* The hosted shell must move the PROCESS, not just its own idea of where
 * it is. host.c's current_proc() re-reads the real getcwd() every call --
 * it is a view of a process the shell does not own -- so writing p->cwd
 * was overwritten microseconds later and every `cd` silently did nothing
 * while reporting success. Declared here rather than via <unistd.h>:
 * this file speaks the kernel's headers. */
extern int chdir(const char *path);
#endif

static int shell_set_cwd(const char *path) {
    if (!path || !*path) return -1;
#ifdef SHELL_HOSTED
    if (chdir(path) != 0) return -1;
#endif
    struct proc *p = current_proc();
    if (p) {
        size_t n = strlen(path);
        if (n + 1 > sizeof(p->cwd)) return -1;
        memcpy(p->cwd, path, n + 1);
    }
    return env_set("PWD", path);
}

static void shell_restore_cwd_only(const char *path) {
    if (!path || !*path) return;
#ifdef SHELL_HOSTED
    /* Unwinding a builtin/subshell frame has to move the process back too,
     * or the restore is as fictional as the cd was. */
    (void)chdir(path);
#endif
    struct proc *p = current_proc();
    if (!p) return;
    size_t n = strlen(path);
    if (n + 1 > sizeof(p->cwd)) return;
    memcpy(p->cwd, path, n + 1);
}

static int shell_resolve_path_arg(const char *arg, char *out, size_t cap,
                                  const char *label) {
    if (shell_canonicalize_path(arg, out, cap) < 0) {
        kprintf("%s: path too long: '%s'\n", label, arg ? arg : "");
        shell_set_status(1);
        return -1;
    }
    return 0;
}

/* ---- builtins ---- */

typedef void (*cmd_fn_t)(int argc, char **argv);

struct cmd {
    const char *name;
    const char *help;
    cmd_fn_t    fn;
};

static const struct cmd cmds[];   /* forward */

#ifdef SHELL_HOSTED
/* In userspace the builtin set is exactly the POSIX/bash one -- no more.
 *
 * The kernel shell carries convenience builtins (`cat`, `ls`, `mkdir`, `rm`,
 * `touch`, `ps`, `ifconfig`...) because when it runs there may be no /bin to
 * exec from. In /bin/tsh those same names SHADOW the real utilities and
 * behave differently: the builtin `cat` demands a path argument, so
 * `echo x | cat` printed a usage message where bash piped the text through
 * /bin/cat. A builtin that shares a utility's name and not its behaviour is
 * the one thing a superset shell must not do.
 *
 * So the hosted build answers "is this a builtin?" from an allow-list of the
 * names bash implements as builtins, and everything else falls through to a
 * PATH lookup. The kernel build is untouched -- this whole function body is
 * compiled only into /bin/tsh. */
static bool shell_hosted_builtin(const char *name) {
    static const char *const allow[] = {
        /* POSIX special builtins */
        ":", ".", "source", "break", "continue", "eval", "exec", "exit",
        "export", "readonly", "return", "set", "shift", "times", "trap",
        "unset",
        /* regular builtins bash also implements internally */
        "alias", "unalias", "bg", "cd", "command", "echo", "false", "fg",
        "getopts", "hash", "jobs", "kill", "local", "printf", "pwd", "read",
        "shopt", "test", "[", "true", "type", "umask", "wait", "sh",
        /* Implemented in the table above but absent from this list, so
         * /bin/tsh sent them to a PATH lookup and reported
         * "failed to launch '/bin/ulimit'". They are builtins in bash and in
         * the kernel shell; the hosted build simply never admitted it. */
        "ulimit", "fc", "getconf", "logname", "pathchk", "newgrp",
        0
    };
    for (int i = 0; allow[i]; i++)
        if (strcmp(name, allow[i]) == 0) return true;
    return false;
}
#endif

static const struct cmd *shell_cmd_lookup(const char *name) {
    if (!name) return 0;
#ifdef SHELL_HOSTED
    if (!shell_hosted_builtin(name)) return 0;
#endif
    for (const struct cmd *c = cmds; c->name; c++) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return 0;
}

static bool shell_special_builtin_name(const char *name) {
    static const char *const special[] = {
        ":", ".", "break", "continue", "eval", "exec", "exit",
        "export", "readonly", "return", "set", "shift", "times",
        "trap", "unset", 0
    };
    if (!name) return false;
    for (int i = 0; special[i]; i++) {
        if (strcmp(name, special[i]) == 0) return true;
    }
    return false;
}

/* `env`         -- print KEY=VALUE for every entry
 * `env K=V`     -- shortcut for `setenv K V`
 * `setenv K V`  -- create/replace
 * `unsetenv K`  -- remove */
static void cmd_env(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        /* The ENVIRONMENT, not the variable table -- `env` is defined as what
         * a child would receive, so it must apply the same export filter
         * shell_build_env_overlay does. */
        for (int i = 0; i < g_envc; i++)
            if (g_env_flags[i] & SHVAR_EXPORTED) shell_printf("%s\n", g_env[i]);
        return;
    }
    /* "env K=V [K=V ...]" -- treat each arg as a literal "KEY=VALUE"
     * blob and install via env_set_kv. */
    for (int i = 1; i < argc; i++) {
        size_t klen = env_key_len(argv[i]);
        const char *eq = (klen > 0) ? (argv[i] + klen) : 0;
        if (!eq || *eq != '=') {
            kprintf("env: '%s' is not KEY=VALUE\n", argv[i]);
            shell_set_status(1);
            continue;
        }
        if (env_set_kv(argv[i]) < 0) {
            kprintf("env: set '%s' failed\n", argv[i]);
            shell_set_status(1);
        }
    }
}

static void cmd_setenv(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 3) {
        kprintf("usage: setenv KEY VALUE\n");
        shell_set_status(1);
        return;
    }
    if (env_set(argv[1], argv[2]) < 0) {
        kprintf("setenv: failed to set '%s'\n", argv[1]);
        shell_set_status(1);
    }
}

static void cmd_unsetenv(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) {
        kprintf("usage: unsetenv KEY\n");
        shell_set_status(1);
        return;
    }
    if (env_unset(argv[1]) < 0) shell_set_status(1);
}

static bool shell_name_is_valid(const char *s, size_t n) {
    if (!s || n == 0) return false;
    char c = s[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static void shell_print_export_entry(const char *entry) {
    size_t klen = env_key_len(entry);
    if (entry[klen] == '=') {
        char name[64];
        if (klen + 1 > sizeof(name)) return;
        memcpy(name, entry, klen);
        name[klen] = '\0';
        shell_printf("export %s=\"%s\"\n", name, entry + klen + 1);
    } else {
        shell_printf("export %s\n", entry);
    }
}

/* ---- `local`: function-scoped variables ---------------------------------
 *
 * See the note above shell_locals_push. The stack is global and bounded; a
 * function that localises more than SHELL_LOCAL_MAX variables across the whole
 * call chain gets a diagnostic rather than silent breakage, because silently
 * dropping a save means the variable never gets restored and the caller's
 * value is gone. */
#define SHELL_LOCAL_MAX 256

struct shell_local_save {
    int   depth;          /* g_script_depth at which this was localised */
    char *name;           /* variable name, NUL-terminated */
    char *kv;             /* prior "KEY=VALUE" blob, or 0 if it did not exist */
    unsigned char flags;  /* prior flags */
    bool  existed;
};

static struct shell_local_save g_locals[SHELL_LOCAL_MAX];
static int g_localc;

/* Record `name`'s current state so the current function's return can put it
 * back. Called before the variable is overwritten. */
static int shell_locals_push(const char *name, int depth) {
    if (g_localc >= SHELL_LOCAL_MAX) {
        kprintf("local: too many local variables (max %d)\n", SHELL_LOCAL_MAX);
        return -1;
    }
    struct shell_local_save *sv = &g_locals[g_localc];
    sv->name = shell_strdup(name);
    if (!sv->name) return -1;

    size_t klen = strlen(name);
    int idx = env_find(name, klen);
    if (idx >= 0) {
        sv->kv = shell_strdup(g_env[idx]);
        if (!sv->kv) { kfree(sv->name); sv->name = 0; return -1; }
        sv->flags = g_env_flags[idx];
        sv->existed = true;
    } else {
        sv->kv = 0;
        sv->flags = 0;
        sv->existed = false;
    }
    sv->depth = depth;
    g_localc++;
    return 0;
}

/* Undo every localisation made at `depth`, newest first. */
static void shell_locals_pop(int depth) {
    while (g_localc > 0 && g_locals[g_localc - 1].depth >= depth) {
        struct shell_local_save *sv = &g_locals[--g_localc];
        size_t klen = strlen(sv->name);
        int idx = env_find(sv->name, klen);
        if (sv->existed) {
            char *blob = shell_strdup(sv->kv);
            if (blob) {
                if (idx >= 0) {
                    kfree(g_env[idx]);
                    g_env[idx] = blob;
                    g_env_flags[idx] = sv->flags;
                } else if (g_envc < ENV_MAX) {
                    g_env_flags[g_envc] = sv->flags;
                    g_env[g_envc++] = blob;
                    g_env[g_envc] = 0;
                } else {
                    kfree(blob);
                }
            }
        } else if (idx >= 0) {
            /* It did not exist before the function ran, so it must not exist
             * after. This is what makes `local x` (no value) mean "unset and
             * mine" rather than "empty and mine". */
            env_remove_at(idx);
        }
        kfree(sv->name);
        kfree(sv->kv);
        sv->name = 0;
        sv->kv = 0;
    }
}

/* `local NAME[=VALUE] ...`
 *
 * Bare `local x` leaves x UNSET but local, which is what bash and dash both
 * do: `f() { local x; echo "${x-unset}"; }` prints `unset`. Setting it empty
 * instead would make an unset local indistinguishable from an empty one. */
static void cmd_local(int argc, char **argv) {
    shell_set_status(0);
    if (g_script_depth <= 0) {
        kprintf("local: can only be used in a function\n");
        shell_set_status(1);
        return;
    }
    for (int i = 1; i < argc; i++) {
        size_t klen = env_key_len(argv[i]);
        bool has_value = (klen > 0 && argv[i][klen] == '=');
        if (!has_value) klen = strlen(argv[i]);
        if (!shell_name_is_valid(argv[i], klen)) {
            kprintf("local: bad name '%s'\n", argv[i]);
            shell_set_status(1);
            continue;
        }
        char name[128];
        if (klen >= sizeof name) {
            kprintf("local: name too long\n");
            shell_set_status(1);
            continue;
        }
        memcpy(name, argv[i], klen);
        name[klen] = '\0';

        if (shell_locals_push(name, g_script_depth) < 0) {
            shell_set_status(1);
            continue;
        }
        if (has_value) {
            if (env_set_kv(argv[i]) < 0) shell_set_status(1);
        } else {
            int idx = env_find(name, klen);
            if (idx >= 0) env_remove_at(idx);
        }
    }
}

/* ---- shopt ---------------------------------------------------------------
 *
 * See the note at the top of this change. `wired` records whether the option
 * actually does anything here; an unwired option is still accepted, because
 * refusing a name bash accepts would be a worse divergence than a no-op, but
 * it announces itself on stderr the first time it is turned on. */
struct shell_shopt {
    const char *name;
    bool       *slot;
    bool        wired;
    bool        warned;
};

static bool g_shopt_nullglob, g_shopt_failglob, g_shopt_dotglob;
static bool g_shopt_extglob, g_shopt_globstar, g_shopt_nocaseglob;
static bool g_shopt_xpg_echo, g_shopt_lastpipe, g_shopt_huponexit;
static bool g_shopt_checkwinsize, g_shopt_shift_verbose, g_shopt_execfail;
static bool g_shopt_histappend, g_shopt_cmdhist, g_shopt_progcomp;
/* bash has these ON by default. */
/* Backing store for the accepted-but-unwired option names above. */
static bool g_shopt_misc[40];
static bool g_shopt_interactive_comments = true;
static bool g_shopt_sourcepath = true;
static bool g_shopt_promptvars = true;

static struct shell_shopt g_shopts[] = {
    { "expand_aliases",       &g_opt_expand_aliases,         true,  false },
    { "nullglob",             &g_shopt_nullglob,             false, false },
    { "failglob",             &g_shopt_failglob,             false, false },
    { "dotglob",              &g_shopt_dotglob,              false, false },
    { "extglob",              &g_shopt_extglob,              false, false },
    { "globstar",             &g_shopt_globstar,             false, false },
    { "nocaseglob",           &g_shopt_nocaseglob,           false, false },
    { "xpg_echo",             &g_shopt_xpg_echo,             false, false },
    { "lastpipe",             &g_shopt_lastpipe,             false, false },
    { "huponexit",            &g_shopt_huponexit,            false, false },
    { "checkwinsize",         &g_shopt_checkwinsize,         false, false },
    { "shift_verbose",        &g_shopt_shift_verbose,        false, false },
    { "execfail",             &g_shopt_execfail,             false, false },
    { "histappend",           &g_shopt_histappend,           false, false },
    { "cmdhist",              &g_shopt_cmdhist,              false, false },
    { "progcomp",             &g_shopt_progcomp,             false, false },
    { "interactive_comments", &g_shopt_interactive_comments, false, false },
    { "sourcepath",           &g_shopt_sourcepath,           false, false },
    { "promptvars",           &g_shopt_promptvars,           false, false },
    /* The remainder of bash's set. None are wired; they are here because
     * REFUSING a name bash accepts changes the exit status a script sees, and
     * `shopt -s inherit_errexit` at the top of a file should not be an error.
     * Each still announces itself once on stderr when switched on. */
    { "inherit_errexit",      &g_shopt_misc[0],              false, false },
    { "strict_errexit",       &g_shopt_misc[1],              false, false },
    { "command_sub_errexit",  &g_shopt_misc[2],              false, false },
    { "nocasematch",          &g_shopt_misc[3],              false, false },
    { "autocd",               &g_shopt_misc[4],              false, false },
    { "cdable_vars",          &g_shopt_misc[5],              false, false },
    { "cdspell",              &g_shopt_misc[6],              false, false },
    { "checkhash",            &g_shopt_misc[7],              false, false },
    { "checkjobs",            &g_shopt_misc[8],              false, false },
    { "dirspell",             &g_shopt_misc[9],              false, false },
    { "extdebug",             &g_shopt_misc[10],             false, false },
    { "extquote",             &g_shopt_misc[11],             false, false },
    { "force_fignore",        &g_shopt_misc[12],             false, false },
    { "globasciiranges",      &g_shopt_misc[13],             false, false },
    { "gnu_errfmt",           &g_shopt_misc[14],             false, false },
    { "histreedit",           &g_shopt_misc[15],             false, false },
    { "histverify",           &g_shopt_misc[16],             false, false },
    { "hostcomplete",         &g_shopt_misc[17],             false, false },
    { "lithist",              &g_shopt_misc[18],             false, false },
    { "localvar_inherit",     &g_shopt_misc[19],             false, false },
    { "localvar_unset",       &g_shopt_misc[20],             false, false },
    { "login_shell",          &g_shopt_misc[21],             false, false },
    { "mailwarn",             &g_shopt_misc[22],             false, false },
    { "no_empty_cmd_completion", &g_shopt_misc[23],          false, false },
    { "nullglob",             &g_shopt_nullglob,             false, false },
    { "restricted_shell",     &g_shopt_misc[24],             false, false },
    { "shift_verbose",        &g_shopt_shift_verbose,        false, false },
    { "xpg_echo",             &g_shopt_xpg_echo,             false, false },
    { "assoc_expand_once",    &g_shopt_misc[25],             false, false },
    { "compat31",             &g_shopt_misc[26],             false, false },
    { "compat32",             &g_shopt_misc[27],             false, false },
    { "compat40",             &g_shopt_misc[28],             false, false },
    { "compat41",             &g_shopt_misc[29],             false, false },
    { "compat42",             &g_shopt_misc[30],             false, false },
    { "compat43",             &g_shopt_misc[31],             false, false },
    { "compat44",             &g_shopt_misc[32],             false, false },
    { "patsub_replacement",   &g_shopt_misc[33],             false, false },
    { "varredir_close",       &g_shopt_misc[34],             false, false },
    { 0, 0, false, false },
};

static struct shell_shopt *shell_shopt_find(const char *name) {
    for (int i = 0; g_shopts[i].name; i++)
        if (strcmp(g_shopts[i].name, name) == 0) return &g_shopts[i];
    return 0;
}

static void shell_shopt_print(const struct shell_shopt *o) {
    shell_printf("%s\t%s\n", o->name, *o->slot ? "on" : "off");
}

/* `shopt [-suqp] [NAME...]`
 *
 * Exit status follows bash: with -q, 0 if every named option is set; without
 * it, 0 unless a name is unknown. An unknown name is status 1 plus a
 * diagnostic, which is what bash does and what several corpus cases check. */
static void cmd_shopt(int argc, char **argv) {
    bool set = false, unset = false, quiet = false, print = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *c = argv[i] + 1; *c; c++) {
            switch (*c) {
            case 's': set = true; break;
            case 'u': unset = true; break;
            case 'q': quiet = true; break;
            case 'p': print = true; break;
            case 'o': break;   /* `shopt -o` addresses `set -o` names */
            default:
                kprintf("shopt: -%c: invalid option\n", *c);
                shell_set_status(2);
                return;
            }
        }
    }
    (void)print;

    if (i >= argc) {                       /* no names: list everything */
        for (int k = 0; g_shopts[k].name; k++) {
            if (set   && !*g_shopts[k].slot) continue;
            if (unset &&  *g_shopts[k].slot) continue;
            if (!quiet) shell_shopt_print(&g_shopts[k]);
        }
        shell_set_status(0);
        return;
    }

    int status = 0;
    for (; i < argc; i++) {
        struct shell_shopt *o = shell_shopt_find(argv[i]);
        if (!o) {
            if (!quiet) kprintf("shopt: %s: invalid shell option name\n", argv[i]);
            status = 1;
            continue;
        }
        if (set || unset) {
            *o->slot = set;
            if (!o->wired && set && !o->warned) {
                o->warned = true;
                kprintf("shopt: %s: accepted but not implemented -- "
                        "behaviour is unchanged\n", o->name);
            }
        } else {
            if (!quiet) shell_shopt_print(o);
            if (!*o->slot) status = 1;
        }
    }
    shell_set_status(status);
}

static void cmd_export(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
        /* Only exported names. `export` with no arguments is defined to list
         * the exported ones; it used to list the whole variable table. */
        for (int i = 0; i < g_envc; i++)
            if (g_env_flags[i] & SHVAR_EXPORTED)
                shell_print_export_entry(g_env[i]);
        return;
    }
    int start = 1;
    if (strcmp(argv[1], "-p") == 0) start = 2;
    for (int i = start; i < argc; i++) {
        size_t klen = env_key_len(argv[i]);
        if (!shell_name_is_valid(argv[i], klen)) {
            kprintf("export: bad name '%s'\n", argv[i]);
            shell_set_status(1);
            continue;
        }
        if (argv[i][klen] == '=') {
            if (env_set_kv(argv[i]) < 0) {
                kprintf("export: failed to set '%s'\n", argv[i]);
                shell_set_status(1);
                continue;
            }
        } else if (!env_get(argv[i])) {
            /* `export X` on a name that does not exist marks it exported
             * WITHOUT creating a value: POSIX says the attribute is set, and
             * bash keeps `${X-unset}` reporting unset until X is assigned.
             * Creating it empty here would make the two indistinguishable, so
             * the name is registered with an empty value and the export bit,
             * which is the closest this table can represent. */
            if (env_set(argv[i], "") < 0) {
                kprintf("export: failed to create '%s'\n", argv[i]);
                shell_set_status(1);
                continue;
            }
        }
        int idx = env_find(argv[i], klen);
        if (idx >= 0) g_env_flags[idx] |= SHVAR_EXPORTED;
    }
}

static void cmd_readonly(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1 || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
        for (int i = 0; i < SHELL_READONLY_MAX; i++) {
            if (!g_readonly[i]) continue;
            const char *v = env_get(g_readonly[i]);
            shell_printf("readonly %s", g_readonly[i]);
            if (v) shell_printf("=\"%s\"", v);
            shell_printf("\n");
        }
        return;
    }

    for (int i = 1; i < argc; i++) {
        size_t klen = env_key_len(argv[i]);
        if (!shell_name_is_valid(argv[i], klen)) {
            kprintf("readonly: bad name '%s'\n", argv[i]);
            shell_set_status(1);
            continue;
        }

        if (argv[i][klen] == '=') {
            if (env_set_kv(argv[i]) < 0) {
                shell_set_status(1);
                continue;
            }
        } else if (!env_get(argv[i])) {
            if (env_set(argv[i], "") < 0) {
                kprintf("readonly: failed to create '%s'\n", argv[i]);
                shell_set_status(1);
                continue;
            }
        }

        if (shell_readonly_mark(argv[i], klen) < 0) shell_set_status(1);
    }
}

static void cmd_unset(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        kprintf("usage: unset [-f|-v] NAME [NAME...]\n");
        shell_set_status(1);
        return;
    }

    bool functions = false;
    int first = 1;
    if (strcmp(argv[1], "-f") == 0) {
        functions = true;
        first = 2;
    } else if (strcmp(argv[1], "-v") == 0) {
        first = 2;
    }
    if (first >= argc) {
        kprintf("unset: missing name\n");
        shell_set_status(1);
        return;
    }
    for (int i = first; i < argc; i++) {
        if (functions) shell_function_unset(argv[i]);
        else if (env_unset(argv[i]) < 0) shell_set_status(1);
    }
}

static bool shell_set_opt(char c, bool on) {
    switch (c) {
    case 'e': g_opt_errexit = on; return true;
    case 'u': g_opt_nounset = on; return true;
    case 'x': g_opt_xtrace = on; return true;
    case 'f': g_opt_noglob = on; return true;
    case 'v': g_opt_verbose = on; return true;
    case 'C': g_opt_noclobber = on; return true;
    case 'b': g_opt_notify = on; return true;
    case 'n': g_opt_noexec = on; return true;
    case 'a': g_opt_allexport = on; return true;
    default: return false;
    }
}

static void shell_print_options(void) {
    shell_printf("allexport %s\n", g_opt_allexport ? "on" : "off");
    shell_printf("errexit   %s\n", g_opt_errexit ? "on" : "off");
    shell_printf("noclobber %s\n", g_opt_noclobber ? "on" : "off");
    shell_printf("noexec    %s\n", g_opt_noexec ? "on" : "off");
    shell_printf("noglob    %s\n", g_opt_noglob ? "on" : "off");
    shell_printf("notify    %s\n", g_opt_notify ? "on" : "off");
    shell_printf("nounset   %s\n", g_opt_nounset ? "on" : "off");
    shell_printf("verbose   %s\n", g_opt_verbose ? "on" : "off");
    shell_printf("xtrace    %s\n", g_opt_xtrace  ? "on" : "off");
}

static bool shell_set_opt_by_name(const char *name, bool on) {
    struct { const char *name; char flag; } map[] = {
        {"errexit", 'e'}, {"nounset", 'u'}, {"xtrace", 'x'},
        {"noglob", 'f'}, {"verbose", 'v'}, {"noclobber", 'C'},
        {"notify", 'b'}, {"noexec", 'n'}, {"allexport", 'a'},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0)
            return shell_set_opt(map[i].flag, on);
    }
    return false;
}

static void cmd_set(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        for (int i = 0; i < g_envc; i++) shell_printf("%s\n", g_env[i]);
        return;
    }

    int first = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { first = i + 1; break; }
        if (argv[i][0] == '-' && argv[i][1] == 'o' && argv[i][2] == '\0') {
            if (i + 1 < argc) {
                i++;
                if (!shell_set_opt_by_name(argv[i], true)) {
                    kprintf("set: unknown option '%s'\n", argv[i]); shell_set_status(1); return;
                }
            } else {
                shell_print_options();
            }
            first = i + 1;
            continue;
        }
        if (argv[i][0] == '+' && argv[i][1] == 'o' && argv[i][2] == '\0') {
            if (i + 1 < argc) {
                i++;
                if (!shell_set_opt_by_name(argv[i], false)) {
                    kprintf("set: unknown option '%s'\n", argv[i]); shell_set_status(1); return;
                }
            }
            first = i + 1;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (!shell_set_opt(*f, true)) {
                    kprintf("set: unknown flag '-%c'\n", *f);
                    shell_set_status(2);
                    return;
                }
            }
            first = i + 1;
            continue;
        }
        if (argv[i][0] == '+' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (!shell_set_opt(*f, false)) {
                    kprintf("set: unknown flag '+%c'\n", *f);
                    shell_set_status(2);
                    return;
                }
            }
            first = i + 1;
            continue;
        }
        break;
    }

    if (first > 1 && strcmp(argv[first - 1], "--") == 0) {
        if (shell_set_positional_params(argc - first, &argv[first]) < 0) {
            kprintf("set: positional parameter table full\n");
            shell_set_status(1);
        }
        return;
    }

    if (first >= argc) return;

    bool all_assignments = true;
    for (int i = first; i < argc; i++) {
        size_t klen = env_key_len(argv[i]);
        if (klen == 0 || argv[i][klen] != '=') {
            all_assignments = false;
            break;
        }
    }

    if (!all_assignments) {
        if (shell_set_positional_params(argc - first, &argv[first]) < 0) {
            kprintf("set: positional parameter table full\n");
            shell_set_status(1);
        }
        return;
    }

    for (int i = first; i < argc; i++) {
        size_t klen = env_key_len(argv[i]);
        if (!shell_name_is_valid(argv[i], klen) || env_set_kv(argv[i]) < 0) {
            kprintf("set: failed to set '%s'\n", argv[i]);
            shell_set_status(1);
        }
    }
}

static void cmd_shift(int argc, char **argv) {
    int n = 1;
    shell_set_status(0);
    if (argc > 2) {
        kprintf("shift: too many arguments\n");
        shell_set_status(2);
        return;
    }
    if (argc == 2 && parse_int(argv[1], &n) < 0) {
        kprintf("shift: numeric argument required\n");
        shell_set_status(2);
        return;
    }
    if (n < 0 || n > g_positional_count) {
        kprintf("shift: can't shift %d positional parameters\n", n);
        shell_set_status(1);
        return;
    }
    /* `shift 0` is a no-op, and must be one: the compaction below copies slot
     * i to slot i-n and then clears slot i, so with n == 0 every parameter is
     * assigned to itself and then NULLed. $# stayed correct while every $n
     * became empty -- which is what `shift $((OPTIND-1))` does after getopts
     * consumed no options, i.e. the common case. */
    if (n == 0) return;

    for (int i = 0; i < n; i++) {
        if (g_positional[i]) kfree(g_positional[i]);
    }
    for (int i = n; i < g_positional_count; i++) {
        g_positional[i - n] = g_positional[i];
        g_positional[i] = 0;
    }
    g_positional_count -= n;
}

static const char *shell_getopts_find(const char *optstring, char opt) {
    if (!optstring) return 0;
    const char *p = optstring;
    if (*p == ':') p++;
    for (; *p; p++) {
        if (*p == ':') continue;
        if (*p == opt) return p;
    }
    return 0;
}

static int shell_getopts_set_int(const char *key, int value) {
    char buf[16];
    ksnprintf(buf, sizeof(buf), "%d", value);
    bool old_internal = g_getopts_internal_optind_write;
    if (shell_key_eq(key, strlen(key), "OPTIND")) {
        g_getopts_internal_optind_write = true;
    }
    int rc = env_set(key, buf);
    g_getopts_internal_optind_write = old_internal;
    return rc;
}

static int shell_getopts_set_char(const char *key, char value) {
    char buf[2] = { value, '\0' };
    return env_set(key, buf);
}

static int shell_getopts_set_name(const char *name, char value) {
    char buf[2] = { value, '\0' };
    return env_set(name, buf);
}

static void shell_getopts_finish_end(const char *name, int optind) {
    (void)shell_getopts_set_name(name, '?');
    (void)env_unset("OPTARG");
    (void)shell_getopts_set_int("OPTIND", optind);
    g_getopts_last_optind = optind;
    g_getopts_char_index = 1;
    shell_set_status(1);
}

static void cmd_getopts(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 3) {
        kprintf("usage: getopts optstring NAME [ARG...]\n");
        shell_set_status(2);
        return;
    }

    const char *optstring = argv[1] ? argv[1] : "";
    const char *name = argv[2] ? argv[2] : "";
    if (!shell_name_is_valid(name, strlen(name))) {
        kprintf("getopts: invalid variable name '%s'\n", name);
        shell_set_status(2);
        return;
    }

    bool silent = optstring[0] == ':';
    int arg_count = (argc > 3) ? (argc - 3) : g_positional_count;
    char **args = (argc > 3) ? &argv[3] : g_positional;

    int optind = 1;
    const char *optind_env = env_get("OPTIND");
    if (optind_env && parse_int(optind_env, &optind) < 0) optind = 1;
    if (optind < 1) optind = 1;
    if (optind != g_getopts_last_optind) {
        g_getopts_char_index = 1;
    }
    if (g_getopts_char_index < 1) g_getopts_char_index = 1;

    const char *arg = 0;
    for (;;) {
        if (optind > arg_count) {
            shell_getopts_finish_end(name, optind);
            return;
        }
        arg = args[optind - 1] ? args[optind - 1] : "";
        if (strcmp(arg, "--") == 0) {
            optind++;
            shell_getopts_finish_end(name, optind);
            return;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            shell_getopts_finish_end(name, optind);
            return;
        }
        if (arg[g_getopts_char_index] != '\0') break;
        optind++;
        g_getopts_char_index = 1;
    }

    char opt = arg[g_getopts_char_index++];
    const char *spec = shell_getopts_find(optstring, opt);
    int rc = 0;

    if (!spec) {
        if (arg[g_getopts_char_index] == '\0') {
            optind++;
            g_getopts_char_index = 1;
        }
        if (shell_getopts_set_name(name, '?') < 0) rc = 1;
        if (silent) {
            if (shell_getopts_set_char("OPTARG", opt) < 0) rc = 1;
        } else {
            kprintf("getopts: illegal option -- %c\n", opt);
            if (env_unset("OPTARG") < 0) rc = 1;
        }
        if (shell_getopts_set_int("OPTIND", optind) < 0) rc = 1;
        g_getopts_last_optind = optind;
        shell_set_status(rc ? 1 : 0);
        return;
    }

    if (spec[1] == ':') {
        if (arg[g_getopts_char_index] != '\0') {
            if (shell_getopts_set_char(name, opt) < 0) rc = 1;
            if (env_set("OPTARG", &arg[g_getopts_char_index]) < 0) rc = 1;
            optind++;
            g_getopts_char_index = 1;
        } else if (optind < arg_count) {
            if (shell_getopts_set_char(name, opt) < 0) rc = 1;
            if (env_set("OPTARG", args[optind] ? args[optind] : "") < 0) {
                rc = 1;
            }
            optind += 2;
            g_getopts_char_index = 1;
        } else {
            optind++;
            g_getopts_char_index = 1;
            if (silent) {
                if (shell_getopts_set_name(name, ':') < 0) rc = 1;
                if (shell_getopts_set_char("OPTARG", opt) < 0) rc = 1;
            } else {
                kprintf("getopts: option requires an argument -- %c\n", opt);
                if (shell_getopts_set_name(name, '?') < 0) rc = 1;
                if (env_unset("OPTARG") < 0) rc = 1;
            }
        }
        if (shell_getopts_set_int("OPTIND", optind) < 0) rc = 1;
        g_getopts_last_optind = optind;
        shell_set_status(rc ? 1 : 0);
        return;
    }

    if (arg[g_getopts_char_index] == '\0') {
        optind++;
        g_getopts_char_index = 1;
    }
    if (shell_getopts_set_char(name, opt) < 0) rc = 1;
    if (env_unset("OPTARG") < 0) rc = 1;
    if (shell_getopts_set_int("OPTIND", optind) < 0) rc = 1;
    g_getopts_last_optind = optind;
    shell_set_status(rc ? 1 : 0);
}

static bool shell_ifs_has(const char *ifs, char c) {
    if (!ifs) ifs = " \t\n";
    for (const char *p = ifs; *p; p++) {
        if (*p == c) return true;
    }
    return false;
}

/* Returns 0 when the line ended at a NEWLINE, 1 when it ended at EOF with
 * data still unterminated, negative on error. POSIX requires `read` to
 * report a non-zero status in the second case even though it assigns what
 * it got -- that is how `while read line` terminates on a file whose last
 * line has no trailing newline instead of processing it twice or not at
 * all. The reader used to return 0 for both and the distinction was lost. */
static int shell_read_line_from_file(struct file *in, bool raw,
                                     char *out, size_t cap,
                                     bool *got_any) {
    if (!in || !out || cap == 0 || !got_any) return -1;
    bool hit_eof = false;
    size_t pos = 0;
    bool escaped = false;
    *got_any = false;

    for (;;) {
        char c = 0;
        long n = file_read(in, &c, 1);
        if (n < 0) return -1;
        if (n == 0) { hit_eof = true; break; }
        *got_any = true;

        if (!raw && escaped) {
            escaped = false;
            if (c == '\n') continue;
            if (pos + 1 >= cap) return -2;
            out[pos++] = c;
            continue;
        }
        if (!raw && c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '\n') break;
        if (pos + 1 >= cap) return -2;
        out[pos++] = c;
    }

    if (!raw && escaped) {
        if (pos + 1 >= cap) return -2;
        out[pos++] = '\\';
    }
    out[pos] = '\0';
    return hit_eof ? 1 : 0;
}

static int shell_read_assign_empty(int argc, char **argv, int first) {
    int rc = 0;
    for (int i = first; i < argc; i++) {
        if (!shell_name_is_valid(argv[i], strlen(argv[i])) ||
            env_set(argv[i], "") < 0) {
            kprintf("read: failed to set '%s'\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

static int shell_read_assign_fields(int argc, char **argv, int first,
                                    char *line) {
    const char *ifs = env_get("IFS");
    if (!ifs) ifs = " \t\n";

    if (ifs[0] == '\0') {
        if (!shell_name_is_valid(argv[first], strlen(argv[first])) ||
            env_set(argv[first], line) < 0) {
            kprintf("read: failed to set '%s'\n", argv[first]);
            return 1;
        }
        return shell_read_assign_empty(argc, argv, first + 1);
    }

    char *p = line;
    while (*p && shell_ifs_has(ifs, *p)) p++;

    int rc = 0;
    for (int i = first; i < argc; i++) {
        char *val = p;
        if (i + 1 == argc) {
            char *end = p + strlen(p);
            while (end > p && shell_ifs_has(ifs, end[-1])) end--;
            *end = '\0';
            p = end;
        } else {
            while (*p && !shell_ifs_has(ifs, *p)) p++;
            if (*p) *p++ = '\0';
            while (*p && shell_ifs_has(ifs, *p)) p++;
        }

        if (!shell_name_is_valid(argv[i], strlen(argv[i])) ||
            env_set(argv[i], val) < 0) {
            kprintf("read: failed to set '%s'\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

static void cmd_read(int argc, char **argv) {
    bool raw = false;
    int first = 1;
    shell_set_status(0);

    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'r') {
                raw = true;
            } else {
                kprintf("read: bad option '-%c'\n", *p);
                shell_set_status(2);
                return;
            }
        }
        first++;
    }
    if (first >= argc) {
        kprintf("usage: read [-r] NAME [NAME...]\n");
        shell_set_status(2);
        return;
    }

    struct file *tmp_console = 0;
    struct file *in = g_shell_in ? g_shell_in : g_shell_fd[0];
    if (!in) {
        tmp_console = console_file_make();
        in = tmp_console;
    }
    if (!in) {
        kprintf("read: failed to open stdin\n");
        shell_set_status(1);
        return;
    }

#ifdef SHELL_TRACE_READ
    kprintf("[readtrace] argc=%d first=%d in=%p g_shell_in=%p fd0=%p IFS='%s'\n",
            argc, first, (void *)in, (void *)g_shell_in, (void *)g_shell_fd[0],
            env_get("IFS") ? env_get("IFS") : "(unset)");
#endif
    char linebuf[LINE_MAX];
    bool got_any = false;
    int rr = shell_read_line_from_file(in, raw, linebuf, sizeof(linebuf),
                                       &got_any);
    if (tmp_console) file_close(tmp_console);
    if (rr < 0) {
        kprintf(rr == -2 ? "read: line too long\n"
                         : "read: input error\n");
        shell_set_status(1);
        return;
    }
    if (!got_any) {
        (void)shell_read_assign_empty(argc, argv, first);
        shell_set_status(1);
        return;
    }

    int rc = shell_read_assign_fields(argc, argv, first, linebuf);
    /* Fields are assigned either way; the status reports that input ran
     * out mid-line, which is what stops a `while read` loop. */
    shell_set_status(rc ? rc : (rr == 1 ? 1 : 0));
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_printf("%s\n", shell_cwd());   /* a RESULT: stdout */
    shell_set_status(0);
}

static void cmd_cd(int argc, char **argv) {
    shell_set_status(0);
    const char *target = 0;
    bool print_new = false;
    if (argc <= 1) {
        target = env_get("HOME");
        if (!target || !*target) target = "/";
    } else if (strcmp(argv[1], "-") == 0) {
        target = env_get("OLDPWD");
        if (!target || !*target) {
            kprintf("cd: OLDPWD not set\n");
            shell_set_status(1);
            return;
        }
        print_new = true;
    } else {
        target = argv[1];
    }

    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(target, path, sizeof(path), "cd") < 0) return;

    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
    if (rc != VFS_OK) {
        kprintf("cd: '%s': %s\n", target, vfs_strerror(rc));
        shell_set_status(1);
        return;
    }
    if (st.type != VFS_TYPE_DIR) {
        kprintf("cd: '%s': not a directory\n", target);
        shell_set_status(1);
        return;
    }

    const char *old = shell_cwd();
    (void)env_set("OLDPWD", old);
    if (shell_set_cwd(path) < 0) {
        kprintf("cd: failed to enter '%s'\n", path);
        shell_set_status(1);
        return;
    }
    if (print_new) kprintf("%s\n", path);
}

static void cmd_true(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_set_status(0);
}

static void cmd_false(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_set_status(1);
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_write("commands:\n");
    for (const struct cmd *c = cmds; c->name; c++) {
        shell_printf("  %-8s  %s\n", c->name, c->help);
    }
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();
}

static void shell_echo_escape(const char *s) {
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            switch (s[1]) {
            case 'n':  shell_write("\n"); s++; break;
            case 't':  shell_write("\t"); s++; break;
            case 'r':  shell_write("\r"); s++; break;
            case '\\': shell_write("\\"); s++; break;
            case 'a':  shell_write("\a"); s++; break;
            case 'b':  shell_write("\b"); s++; break;
            case 'f':  shell_write("\f"); s++; break;
            case 'v':  shell_write("\v"); s++; break;
            case '0': {
                unsigned val = 0;
                s++;
                for (int k = 0; k < 3 && s[1] >= '0' && s[1] <= '7'; k++)
                    val = val * 8 + (*(++s) - '0');
                char c = (char)val;
                if (c) { char tmp[2] = {c, 0}; shell_write(tmp); }
                break;
            }
            case 'c': return;
            default: { char tmp[2] = {'\\', 0}; shell_write(tmp); } break;
            }
        } else {
            char tmp[2] = {*s, 0};
            shell_write(tmp);
        }
    }
}

static void cmd_echo(int argc, char **argv) {
    bool newline = true;
    bool escapes = false;
    int i = 1;
    while (i < argc) {
        if (argv[i][0] != '-') break;
        bool valid = true;
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'n') newline = false;
            else if (*f == 'e') escapes = true;
            else if (*f == 'E') escapes = false;
            else { valid = false; break; }
        }
        if (!valid) break;
        i++;
    }
    for (; i < argc; i++) {
        if (escapes)
            shell_echo_escape(argv[i]);
        else
            shell_write(argv[i]);
        if (i + 1 < argc) shell_write(" ");
    }
    if (newline) shell_write("\n");
}
static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    size_t total = pmm_total_pages();
    size_t used  = pmm_used_pages();
    size_t free_ = pmm_free_pages();
    kprintf("pmm:  total=%lu used=%lu free=%lu pages (%lu KiB free)\n",
            (unsigned long)total, (unsigned long)used, (unsigned long)free_,
            (unsigned long)(free_ * PAGE_SIZE / 1024));

    struct heap_stats hs;
    heap_stats(&hs);
    kprintf("heap: arenas=%lu total=%lu used=%lu free=%lu allocs=%lu frees=%lu\n",
            (unsigned long)hs.arenas,    (unsigned long)hs.total_bytes,
            (unsigned long)hs.used_bytes,(unsigned long)hs.free_bytes,
            (unsigned long)hs.alloc_count,(unsigned long)hs.free_count);
    kprintf("heap: virt %p..%p (brk=%p, %lu KiB consumed)\n",
            (void *)heap_virt_base(), (void *)heap_virt_end(),
            (void *)heap_virt_brk(),
            (unsigned long)((heap_virt_brk() - heap_virt_base()) / 1024));
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 1;
    uint64_t t   = pit_ticks();
    uint64_t sec = t / hz;
    uint64_t cs  = (t % hz) * 100 / hz;   /* centiseconds */
    kprintf("uptime: %lu.%02lus (%lu ticks @ %u Hz)\n",
            (unsigned long)sec, (unsigned long)cs, (unsigned long)t, hz);
}

static void cmd_about(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t cols, rows;
    console_get_size(&cols, &rows);
    kprintf("tobyOS (milestone 13 -- GUI terminal + file manager)\n");
    kprintf("  console : %ux%u cells\n", cols, rows);
    kprintf("  drivers : serial, console, pic, pit, ps/2 kbd+mouse, lapic (xAPIC), ata-pio, e1000\n");
    kprintf("  memory  : pmm bitmap, own PML4, kmalloc on vmm_map'd arenas\n");
    kprintf("  vfs     : multi-mount, '/' ramfs (RO), '/data' tobyfs (RW, persistent)\n");
    kprintf("  procs   : per-proc PML4+kstack+fds[], FIFO sched, run/ps/wait\n");
    kprintf("  ipc     : kernel pipes + 'cmd | cmd' shell pipelines\n");
    kprintf("  signals : SIGINT/SIGTERM, Ctrl+C -> foreground process\n");
    kprintf("  jobs    : 'cmd &' -> bg, 'jobs' lists, 'fg N' to foreground\n");
    kprintf("  net     : e1000 + IPv4 + ARP + UDP sockets (`ifconfig`, `arp`, `netstat`)\n");
    kprintf("  gui     : compositor + windows + widgets (try `gui gui_widgets`)\n");
    kprintf("  desktop : taskbar + launcher + draggable windows (`desktop` to enter)\n");
    kprintf("  terminal: /bin/gui_term -- GUI shell in a window (builtin cmds)\n");
    kprintf("  files   : /bin/gui_files -- GUI file manager w/ viewer\n");
    kprintf("  loader  : ELF64 ET_EXEC, ring-3 + syscalls (21 total -- see syscall.h)\n");
    kprintf("  smp     : %u CPU(s) online (use 'cpus' for details)\n",
            smp_online_count());
}

/* ---- arg parsing helpers used by panic/peek ---- */

static int parse_hex(const char *s, uint64_t *out) {
    uint64_t v = 0;
    int      n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return -1;
    for (; *s; s++, n++) {
        char c = *s;
        uint64_t d;
        if      (c >= '0' && c <= '9') d = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
        else return -1;
        if (n >= 16) return -1;          /* would overflow uint64_t */
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

static void cmd_peek(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: peek <hex-phys-addr>\n");
        return;
    }
    uint64_t phys;
    if (parse_hex(argv[1], &phys) < 0) {
        kprintf("peek: bad hex '%s'\n", argv[1]);
        return;
    }
    /* Bounds-check against the PMM's view of physical memory. */
    if (phys / PAGE_SIZE >= pmm_total_pages()) {
        kprintf("peek: %p outside known physical range\n", (void *)phys);
        return;
    }
    /* Show 32 bytes (4 x u64) starting at phys, accessed via HHDM. */
    uint64_t *p = (uint64_t *)pmm_phys_to_virt(phys & ~7ULL);
    kprintf("[%p] %016lx %016lx %016lx %016lx\n",
            (void *)(phys & ~7ULL), p[0], p[1], p[2], p[3]);
}

static void cmd_modules(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!module_req.response || module_req.response->module_count == 0) {
        kprintf("(no modules)\n");
        return;
    }
    kprintf("%-3s  %-32s  %-10s  %s\n", "idx", "path", "size", "addr");
    for (uint64_t i = 0; i < module_req.response->module_count; i++) {
        struct limine_file *m = module_req.response->modules[i];
        kprintf("%-3lu  %-32s  %-10lu  %p\n",
                (unsigned long)i, m->path,
                (unsigned long)m->size, m->address);
    }
}

/* Forward decls: shared spawn helper used by both `run` and the
 * implicit-ELF dispatch path below, and the program-name resolver
 * (whose definition lives down in the pipeline section). */
static void        shell_spawn_program(const char *path, int argc, char **argv,
                                       bool background);
static void        shell_spawn_program_profile(const char *path, int argc,
                                               char **argv, bool background,
                                               const char *profile);
static const char *resolve_program(const char *name, char *out_buf,
                                   size_t out_sz);
static bool        path_is_file(const char *path);

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: run [--sandbox <profile>] <path> [args...]\n");
        kprintf("       (try 'run /bin/hello' or just 'hello')\n");
        kprintf("       'caps' lists available sandbox profiles\n");
        shell_set_status(1);
        return;
    }

    /* Optional: --sandbox <name> as the FIRST argv slot past "run".
     * Kept intentionally narrow so we don't turn this into a full
     * getopt parser; anything more elaborate can live in the new
     * `sandbox <profile> <cmd...>` builtin. */
    const char *profile = 0;
    int shift = 1;
    if (argc >= 4 && strcmp(argv[1], "--sandbox") == 0) {
        profile = argv[2];
        shift = 3;
    }
    if (argc - shift < 1) {
        kprintf("run: missing <path>\n");
        shell_set_status(1);
        return;
    }

    /* Re-pack argv so the spawned program sees argv[0] = its own
     * basename rather than the literal "run". This matches what
     * implicit-ELF dispatch does, so the program's behavior is
     * identical regardless of which form the user typed. */
    shell_spawn_program_profile(argv[shift], argc - shift, &argv[shift],
                                /*background=*/false, profile);
}

static void cmd_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    int shown = 0;
    for (int i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].id == 0) continue;
        struct proc *p = proc_lookup(g_jobs[i].pid);
        const char *st = p ? proc_state_name(p->state) : "GONE";
        shell_printf("  [%d]  pid=%-3d  %-10s  %s\n",
                g_jobs[i].id, g_jobs[i].pid, st, g_jobs[i].name);
        shown++;
    }
    if (shown == 0) shell_printf("  (no background jobs)\n");
}

static int parse_int(const char *s, int *out) {
    if (!s || !*s) return -1;
    int v = 0;
    for (const char *c = s; *c; c++) {
        if (*c < '0' || *c > '9') return -1;
        v = v * 10 + (*c - '0');
    }
    *out = v;
    return 0;
}

static void cmd_fg(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: fg <job_id>     (see 'jobs')\n");
        shell_set_status(1);
        return;
    }
    int jid;
    if (parse_int(argv[1], &jid) < 0 || jid <= 0) {
        kprintf("fg: bad job id '%s'\n", argv[1]);
        shell_set_status(1);
        return;
    }
    struct job *j = jobs_find(jid);
    if (!j) {
        kprintf("fg: no such job [%d]\n", jid);
        shell_set_status(1);
        return;
    }
    int pid = j->pid;
    /* Snapshot the name into a local buffer because the job slot may
     * be cleared during proc_wait/reap and we still want to print it. */
    char saved_name[JOB_NAME_MAX];
    size_t n = 0;
    while (n + 1 < JOB_NAME_MAX && j->name[n]) { saved_name[n] = j->name[n]; n++; }
    saved_name[n] = 0;

    kprintf("fg: bringing [%d] pid=%d '%s' to foreground\n",
            jid, pid, saved_name);

    /* Take ownership of Ctrl+C until this proc finishes. */
    signal_set_foreground(pid);
    int rc = proc_wait(pid);
    signal_set_foreground(0);

    /* proc_wait already reaped, so the job entry no longer points at a
     * live PCB. Just clear our table slot. */
    struct job *j2 = jobs_find(jid);   /* re-find: pointer may be stale */
    if (j2) jobs_remove(j2);

    kprintf("fg: '%s' (pid=%d) returned %d (0x%x)\n",
            saved_name, pid, rc, (unsigned)rc);
    shell_set_status(rc);
}

static void cmd_bg(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: bg <job_id>     (see 'jobs')\n");
        shell_set_status(1);
        return;
    }
    int jid;
    if (parse_int(argv[1], &jid) < 0 || jid <= 0) {
        kprintf("bg: bad job id '%s'\n", argv[1]);
        shell_set_status(1);
        return;
    }
    struct job *j = jobs_find(jid);
    if (!j) {
        kprintf("bg: no such job [%d]\n", jid);
        shell_set_status(1);
        return;
    }
    kprintf("[%d] %s &\n", j->id, j->name);
    shell_set_status(0);
}

static int shell_wait_job(struct job *j) {
    if (!j || j->id == 0) return 127;
    int pid = j->pid;
    signal_set_foreground(pid);
    int rc = proc_wait(pid);
    signal_set_foreground(0);
    if (rc < 0) {
        kprintf("wait: pid %d: not a child\n", pid);
        jobs_remove(j);
        return 127;
    }
    jobs_remove(j);
    return rc;
}

static struct job *shell_wait_lookup(const char *arg) {
    if (!arg || !*arg) return 0;
    int n = 0;
    if (arg[0] == '%') {
        if (parse_int(arg + 1, &n) < 0 || n <= 0) return 0;
        return jobs_find(n);
    }
    if (parse_int(arg, &n) < 0 || n <= 0) return 0;

    struct job *j = jobs_find_pid(n);
    if (j) return j;
    return jobs_find(n);
}

static void cmd_wait(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        int rc = 0;
        for (;;) {
            struct job *j = 0;
            for (int i = 0; i < JOB_MAX; i++) {
                if (g_jobs[i].id != 0) {
                    j = &g_jobs[i];
                    break;
                }
            }
            if (!j) break;
            rc = shell_wait_job(j);
        }
        shell_set_status(rc == 127 ? 127 : 0);
        return;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct job *j = shell_wait_lookup(argv[i]);
        if (!j) {
            kprintf("wait: %s: unknown pid or job\n", argv[i]);
            rc = 127;
            continue;
        }
        int wrc = shell_wait_job(j);
        rc = wrc;
    }
    shell_set_status(rc);
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    /* proc_dump_table() owns the rich multi-column format -- including
     * the milestone-19 cpu_ms / syscalls / pages fields. Keeping the
     * logic in one place means `ps` and any future kernel-side
     * diagnostic dump stay in sync. */
    proc_dump_table();
}

/* ---- Milestone 19: `top`, `time`, `perf`, `log` --------------------
 *
 * These live together because they all lean on the same perf.h /
 * <tobyos/proc.h> snapshot APIs. None of them are long, so inlining
 * here keeps the shell file self-contained. */

/* Very small integer atoi; -1 on parse failure. Only used for the
 * optional args of `top` (iteration count, delay ms). */
static int tiny_atoi(const char *s) {
    if (!s || !*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 1000000) return -1;        /* guard the delay arg */
    }
    return v;
}

/* Shared "one top iteration" print. Walks every live PCB and renders
 * a row with CPU% computed as (delta_cpu_ns / delta_wall_ns) * 100.
 * The caller owns the delta bookkeeping (a snapshot array of
 * previous cpu_ns values keyed by pid). */
static void top_print_iter(uint64_t wall_dns, uint64_t prev_cpu[PROC_MAX]) {
    struct perf_sys sys;
    perf_sys_snapshot(&sys);
    kprintf("---- top @ %lu ms -- ctx=%lu syscalls=%lu frames=%lu procs=%lu ----\n",
            (unsigned long)(sys.boot_ns / 1000000ull),
            (unsigned long)sys.context_switches,
            (unsigned long)sys.total_syscalls,
            (unsigned long)sys.gui_frames,
            (unsigned long)(sys.proc_spawns - sys.proc_exits));
    kprintf("  %-3s  %-10s  %-16s  %-5s  %-8s  %-10s  %-6s\n",
            "pid", "state", "name", "uid", "cpu%", "cpu_ms", "sys");
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = proc_lookup(i);
        if (!p) { prev_cpu[i] = 0; continue; }
        uint64_t cur = p->cpu_ns;
        uint64_t dcpu = cur > prev_cpu[i] ? cur - prev_cpu[i] : 0;
        unsigned pct = 0;
        if (wall_dns > 0) {
            /* pct = dcpu * 100 / wall_dns, careful not to overflow
             * for small wall_dns values. */
            pct = (unsigned)((dcpu * 100ull) / wall_dns);
            if (pct > 100) pct = 100;
        }
        prev_cpu[i] = cur;
        kprintf("  %-3d  %-10s  %-16s  %-5d  %-8u  %-10lu  %-6lu\n",
                p->pid, proc_state_name(p->state), p->name, p->uid,
                pct,
                (unsigned long)(cur / 1000000ull),
                (unsigned long)p->syscall_count);
    }
}

static void cmd_top(int argc, char **argv) {
    /* Syntax: top [-n iters] [-d ms]. Default 10 iterations at 500 ms
     * so the whole command wraps up in ~5 seconds. That's short enough
     * for interactive use and long enough to get meaningful deltas
     * out of whatever's currently running. */
    int iters  = 10;
    int ms     = 500;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            int v = tiny_atoi(argv[++i]);
            if (v > 0) iters = v;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            int v = tiny_atoi(argv[++i]);
            if (v > 0) ms = v;
        } else {
            kprintf("usage: top [-n iters] [-d ms]\n");
            return;
        }
    }

    /* Baseline snapshot: prev_cpu[i] starts at the current proc's
     * cpu_ns, so the FIRST reported %s are already meaningful
     * (delta since we sampled). */
    uint64_t prev_cpu[PROC_MAX];
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = proc_lookup(i);
        prev_cpu[i] = p ? p->cpu_ns : 0;
    }
    uint64_t prev_wall_ns = perf_now_ns();

    for (int it = 0; it < iters; it++) {
        pit_sleep_ms((uint64_t)ms);
        uint64_t now = perf_now_ns();
        uint64_t dwall = now - prev_wall_ns;
        prev_wall_ns = now;
        top_print_iter(dwall, prev_cpu);
    }
}

static void cmd_time(int argc, char **argv) {
    /* Syntax: time <cmd> [args...]. Runs the command as a foreground
     * child and reports wall / cpu / syscall counts once it exits.
     * Builtins are NOT supported (they run inside the shell, so
     * there's no separate PCB to measure -- use `perf reset; ...;
     * perf` for that). */
    if (argc < 2) {
        kprintf("usage: time <command> [args...]\n");
        return;
    }
    char path_buf[64];
    const char *path = resolve_program(argv[1], path_buf, sizeof(path_buf));
    struct proc_spec spec = {
        .path = path,
        .name = argv[1],
        .fd0 = 0, .fd1 = 0, .fd2 = 0,
        .argc = argc - 1,
        .argv = &argv[1],
        /* M25C: hand the shell env to every child so getenv works. */
        .envc = g_envc,
        .envp = g_env,
        .sandbox_profile = 0,
    };
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("time: failed to spawn '%s'\n", argv[1]);
        return;
    }
    signal_set_foreground(pid);

    struct proc_exit_info info;
    int rc = proc_wait_info(pid, &info);
    signal_set_foreground(0);

    if (rc < 0) {
        kprintf("time: wait failed\n");
        return;
    }
    /* Fractional ms with microsecond resolution -- matches
     * perf_proc_print_summary() so the formats are interchangeable. */
    uint64_t wall_ms = info.wall_ns / 1000000ull;
    uint64_t wall_us = (info.wall_ns / 1000ull) % 1000ull;
    uint64_t cpu_ms  = info.cpu_ns  / 1000000ull;
    uint64_t cpu_us  = (info.cpu_ns  / 1000ull) % 1000ull;
    kprintf("time: '%s' pid=%d  exit=%d  wall=%lu.%03lu ms  cpu=%lu.%03lu ms"
            "  syscalls=%lu\n",
            info.name, info.pid, info.exit_code,
            (unsigned long)wall_ms, (unsigned long)wall_us,
            (unsigned long)cpu_ms,  (unsigned long)cpu_us,
            (unsigned long)info.syscall_count);
}

/* Linux slice 1: dump the ENOSYS census on demand.
 *
 * The 60 s deep dump prints this too, but a census run wants the answer at a
 * chosen moment -- run a workload, then ask -- rather than on a timer. */
static void cmd_lxgaps(int argc, char **argv) {
    (void)argc; (void)argv;
    extern void lx_dump_gaps(void);
    lx_dump_gaps();
}

static void cmd_perf(int argc, char **argv) {
    /* Syntax:
     *   perf              -- dump zones + syscalls + sys metrics
     *   perf reset        -- zero everything
     *   perf on|off       -- toggle zone recording (global switch)
     */
    if (argc >= 2) {
        if (strcmp(argv[1], "reset") == 0) {
            perf_reset();
            return;
        }
        if (strcmp(argv[1], "on") == 0) {
            perf_set_enabled(true);
            return;
        }
        if (strcmp(argv[1], "off") == 0) {
            perf_set_enabled(false);
            return;
        }
        kprintf("usage: perf [reset|on|off]\n");
        return;
    }
    perf_dump_sys();
    perf_dump_zones();
    perf_dump_syscalls();
}

static void cmd_log(int argc, char **argv) {
    /* Syntax:
     *   log                         -- show current mask
     *   log enable  <cat|all>       -- turn a category on
     *   log disable <cat|all>       -- turn it off
     *
     * Categories: sched | syscall | proc | vfs | gui | perf | net | all
     */
    if (argc == 1) {
        uint32_t m = log_mask();
        kprintf("log mask = 0x%08x { ", (unsigned)m);
        static const uint32_t bits[] = {
            LOG_CAT_SCHED, LOG_CAT_SYSCALL, LOG_CAT_PROC, LOG_CAT_VFS,
            LOG_CAT_GUI, LOG_CAT_PERF, LOG_CAT_NET };
        for (unsigned i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
            if (m & bits[i]) kprintf("%s ", log_cat_name(bits[i]));
        }
        kprintf("}\n");
        return;
    }
    if (argc >= 3) {
        uint32_t cat = log_cat_from_name(argv[2]);
        if (!cat) {
            kprintf("log: unknown category '%s'\n", argv[2]);
            return;
        }
        if (strcmp(argv[1], "enable") == 0) {
            log_enable(cat);
            kprintf("log: enabled '%s'\n", argv[2]);
            return;
        }
        if (strcmp(argv[1], "disable") == 0) {
            log_disable(cat);
            kprintf("log: disabled '%s'\n", argv[2]);
            return;
        }
    }
    kprintf("usage: log [enable|disable <sched|syscall|proc|vfs|gui|perf|net|all>]\n");
}

/* ---- VFS-backed builtins (milestone 4) ---- */

/* Render a 9-bit perm field as "rwxr-xr-x". `out` must be >=10 bytes. */
static void mode_to_string(uint32_t mode, enum vfs_type type, char *out) {
    out[0] = (type == VFS_TYPE_DIR) ? 'd' : '-';
    if (mode & VFS_MODE_VALID) {
        out[1] = (mode & 00400) ? 'r' : '-';
        out[2] = (mode & 00200) ? 'w' : '-';
        out[3] = (mode & 00100) ? 'x' : '-';
        out[4] = (mode & 00040) ? 'r' : '-';
        out[5] = (mode & 00020) ? 'w' : '-';
        out[6] = (mode & 00010) ? 'x' : '-';
        out[7] = (mode & 00004) ? 'r' : '-';
        out[8] = (mode & 00002) ? 'w' : '-';
        out[9] = (mode & 00001) ? 'x' : '-';
    } else {
        /* Legacy inode -- the VFS treats it as fully accessible. */
        for (int i = 1; i <= 9; i++) out[i] = '?';
    }
    out[10] = '\0';
}

static void cmd_ls(int argc, char **argv) {
    /* Optional `-l` flag for long-form (perms + uid + size). Without
     * the flag we still print perms + uid -- the plain "name + size"
     * format from previous milestones is gone, since milestone 15 is
     * about MAKING ownership visible. */
    bool long_form = false;
    int  argi = 1;
    if (argi < argc && strcmp(argv[argi], "-l") == 0) {
        long_form = true; argi++;
    }
    const char *path_arg = (argi < argc) ? argv[argi] : ".";
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(path_arg, path, sizeof(path), "ls") < 0) return;
    struct vfs_dir d;
    int rc = vfs_opendir(path, &d);
    if (rc != VFS_OK) {
        kprintf("ls: '%s': %s\n", path_arg, vfs_strerror(rc));
        shell_set_status(1);
        return;
    }
    struct vfs_dirent ent;
    size_t shown = 0;
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        char perms[11];
        mode_to_string(ent.mode, ent.type, perms);
        if (long_form) {
            const struct user *u = users_lookup_by_uid((int)ent.uid);
            const char *uname = u ? u->name : "?";
            if (ent.type == VFS_TYPE_DIR) {
                kprintf("  %s  %-8s  %-24s  <DIR>\n",
                        perms, uname, ent.name);
            } else {
                kprintf("  %s  %-8s  %-24s  %lu B\n",
                        perms, uname, ent.name,
                        (unsigned long)ent.size);
            }
        } else {
            if (ent.type == VFS_TYPE_DIR) {
                kprintf("  %s  uid=%-3u  %-24s  <DIR>\n",
                        perms, (unsigned)ent.uid, ent.name);
            } else {
                kprintf("  %s  uid=%-3u  %-24s  %lu B\n",
                        perms, (unsigned)ent.uid, ent.name,
                        (unsigned long)ent.size);
            }
        }
        shown++;
    }
    vfs_closedir(&d);
    if (shown == 0) kprintf("  (empty)\n");
    shell_set_status(0);
}

/* ---- milestone 15 user/perm builtins ----------------------------- */

static void cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    struct proc *p = current_proc();
    int uid = p ? p->uid : 0;
    int gid = p ? p->gid : 0;
    const struct user *u = users_lookup_by_uid(uid);
    kprintf("%s (uid=%d gid=%d)\n",
            u ? u->name : "?", uid, gid);
}

static void users_print_one(const struct user *u, void *ctx) {
    (void)ctx;
    kprintf("  %-16s uid=%-4d gid=%-4d\n", u->name, u->uid, u->gid);
}

static void cmd_users(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "add") == 0) {
        if (argc != 5) {
            kprintf("usage: users add <name> <uid> <gid>\n");
            shell_set_status(1);
            return;
        }
        int uid = 0, gid = 0;
        for (const char *p = argv[3]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("users: bad uid\n"); shell_set_status(1); return; }
            uid = uid * 10 + (*p - '0');
        }
        for (const char *p = argv[4]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("users: bad gid\n"); shell_set_status(1); return; }
            gid = gid * 10 + (*p - '0');
        }
        if (current_proc() && current_proc()->uid != 0) {
            kprintf("users: only root may add users\n");
            shell_set_status(1);
            return;
        }
        if (users_add(argv[2], uid, gid) != 0) {
            kprintf("users: add failed\n");
            shell_set_status(1);
            return;
        }
        if (users_save() != 0) {
            kprintf("users: warning -- could not persist (kept in RAM)\n");
        } else {
            kprintf("users: added '%s' uid=%d gid=%d\n", argv[2], uid, gid);
        }
        shell_set_status(0);
        return;
    }
    kprintf("registered users:\n");
    users_visit(users_print_one, 0);
    shell_set_status(0);
}

/* Parse an octal string like "755". Returns -1 on bad input. */
static int parse_octal_str(const char *s) {
    if (!s || !*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '7') return -1;
        v = (v << 3) | (*s - '0');
    }
    return v;
}

static void cmd_chmod(int argc, char **argv) {
    if (argc != 3) {
        kprintf("usage: chmod <octal> <path>     e.g. chmod 644 /data/notes\n");
        shell_set_status(1);
        return;
    }
    int mode = parse_octal_str(argv[1]);
    if (mode < 0 || mode > 0777) {
        kprintf("chmod: bad mode '%s' -- must be 3 octal digits\n", argv[1]);
        shell_set_status(1);
        return;
    }
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(argv[2], path, sizeof(path), "chmod") < 0) return;
    int rc = vfs_chmod(path, (uint32_t)mode);
    if (rc != VFS_OK) {
        kprintf("chmod: '%s': %s\n", argv[2], vfs_strerror(rc));
        shell_set_status(1);
    } else {
        kprintf("chmod: '%s' -> 0%o\n", argv[2], (unsigned)mode);
        shell_set_status(0);
    }
}

static void cmd_chown(int argc, char **argv) {
    if (argc != 3) {
        kprintf("usage: chown <user> <path>      (root only)\n");
        shell_set_status(1);
        return;
    }
    const struct user *u = users_lookup_by_name(argv[1]);
    if (!u) {
        kprintf("chown: unknown user '%s'\n", argv[1]);
        shell_set_status(1);
        return;
    }
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(argv[2], path, sizeof(path), "chown") < 0) return;
    int rc = vfs_chown(path, (uint32_t)u->uid, (uint32_t)u->gid);
    if (rc != VFS_OK) {
        kprintf("chown: '%s': %s\n", argv[2], vfs_strerror(rc));
        shell_set_status(1);
    } else {
        kprintf("chown: '%s' -> %s (uid=%d gid=%d)\n",
                argv[2], u->name, u->uid, u->gid);
        shell_set_status(0);
    }
}

/* Debug helper: pretend to be a different user for the duration of a
 * single command. Lets the operator demonstrate access control without
 * leaving the kernel shell. The shell itself runs as pid 0 / uid 0
 * (root), so its commands always succeed otherwise. */
static void cmd_su(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: su <user>                (changes the SHELL's uid/gid)\n");
        shell_set_status(1);
        return;
    }
    const struct user *u = users_lookup_by_name(argv[1]);
    if (!u) {
        kprintf("su: unknown user '%s'\n", argv[1]);
        shell_set_status(1);
        return;
    }
    struct proc *p = current_proc();
    if (!p) { kprintf("su: no current proc\n"); shell_set_status(1); return; }
    /* Debug aid -- the kernel shell always runs on pid 0 (boot
     * thread), so we let it freely flip identity for demo purposes. */
    p->uid = u->uid;
    p->gid = u->gid;
    (void)env_set("USER", u->name);
    kprintf("su: now running as %s (uid=%d gid=%d). "
            "Type `su root` to restore.\n",
            u->name, u->uid, u->gid);
    shell_set_status(0);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: cat <path>\n");
        shell_set_status(1);
        return;
    }
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(argv[1], path, sizeof(path), "cat") < 0) return;

    /* Stream in 256-byte chunks instead of slurping the whole file --
     * keeps stack usage tiny and works for arbitrarily-large text
     * files without a heap allocation. */
    struct vfs_file f;
    int rc = vfs_open(path, &f);
    if (rc != VFS_OK) {
        kprintf("cat: '%s': %s\n", argv[1], vfs_strerror(rc));
        shell_set_status(1);
        return;
    }
    char buf[256];
    bool ok = true;
    for (;;) {
        long n = vfs_read(&f, buf, sizeof(buf));
        if (n < 0) {
            kprintf("\ncat: read error: %s\n", vfs_strerror((int)n));
            ok = false;
            break;
        }
        if (n == 0) break;
        for (long i = 0; i < n; i++) shell_putc(buf[i]);
    }
    vfs_close(&f);
    /* Many text files lack a trailing newline -- add one so the next
     * shell prompt doesn't visually collide with the last line. */
    shell_putc('\n');
    shell_set_status(ok ? 0 : 1);
}

/* ---- writable-FS builtins (milestone 6) ---- */

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: touch <path>      (creates empty file; '/data' is writable)\n");
        shell_set_status(1);
        return;
    }
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(argv[1], path, sizeof(path), "touch") < 0) return;
    int rc = vfs_create(path);
    if (rc == VFS_ERR_EXIST) {
        /* `touch` of an existing file is a no-op success in classic
         * shells; surface that politely. */
        kprintf("touch: '%s' already exists\n", argv[1]);
        shell_set_status(0);
        return;
    }
    if (rc != VFS_OK) {
        kprintf("touch: '%s': %s\n", argv[1], vfs_strerror(rc));
        shell_set_status(1);
    } else {
        shell_set_status(0);
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: mkdir <path>\n");
        shell_set_status(1);
        return;
    }
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(argv[1], path, sizeof(path), "mkdir") < 0) return;
    int rc = vfs_mkdir(path);
    if (rc != VFS_OK) {
        kprintf("mkdir: '%s': %s\n", argv[1], vfs_strerror(rc));
        shell_set_status(1);
    } else {
        shell_set_status(0);
    }
}

/* Slice 127: recursive delete.
 *
 * `rm` could only ever remove a file or an EMPTY directory, which means an
 * ordinary directory TREE could not be deleted from this OS at all -- a
 * chrome profile is thousands of files deep and hand-unlinking it is not a
 * thing a person can do. Found the practical way: a corrupt
 * /data/cr2 needed clearing and there was no command that could.
 *
 * Depth is bounded and the recursion carries ONE path buffer that it
 * appends to and truncates on the way back out, rather than a buffer per
 * level: this runs on the kernel stack, where VFS_PATH_MAX per frame would
 * be the thing that overflows first on a deep tree.
 *
 * Returns 0 on success, -1 if anything could not be removed (the caller
 * reports; each individual failure is printed as it happens so a partial
 * delete says exactly what survived). */
static void cmd_rm(int argc, char **argv) {
    bool recursive = false, force = false;
    int  argi = 1;
    for (; argi < argc && argv[argi][0] == '-' && argv[argi][1]; argi++) {
        for (const char *f = argv[argi] + 1; *f; f++) {
            if      (*f == 'r' || *f == 'R') recursive = true;
            else if (*f == 'f')              force     = true;
            else {
                kprintf("rm: unknown option -%c\n", *f);
                shell_set_status(1);
                return;
            }
        }
    }
    if (argi >= argc) {
        kprintf("usage: rm [-r] [-f] <path>...   (-r: recurse into directories)\n");
        shell_set_status(1);
        return;
    }

    int failed = 0;
    for (; argi < argc; argi++) {
        char path[VFS_PATH_MAX];
        if (shell_resolve_path_arg(argv[argi], path, sizeof(path), "rm") < 0) {
            failed = 1;
            continue;
        }
        /* Refuse to recurse from the root. Not paranoia about the user --
         * `rm -r /` here would walk into /proc and /sys and try to unlink
         * synthesised nodes, and the first confusing failure would come
         * from a filesystem the user never meant to touch. */
        if (recursive && path[0] == '/' && path[1] == 0) {
            kprintf("rm: refusing to recurse from '/'\n");
            failed = 1;
            continue;
        }
        if (recursive) {
            /* Slice 127: one implementation, in the VFS -- the GUI terminal
             * needs the same thing and the file manager will too. */
            char bad[VFS_PATH_MAX];
            int rc = vfs_rmtree(path, force, bad, sizeof(bad));
            if (rc != VFS_OK) {
                kprintf("rm: '%s': %s\n", bad[0] ? bad : path,
                        vfs_strerror(rc));
                failed = 1;
            }
        } else {
            int rc = vfs_unlink(path);
            if (rc != VFS_OK) {
                if (force && rc == VFS_ERR_NOENT) continue;
                /* This VFS has no distinct "directory not empty" code, so a
                 * failed unlink on a directory is ambiguous. Stat it and say
                 * the useful thing rather than echoing a generic errno at
                 * someone who just wants the tree gone. */
                struct vfs_stat st;
                if (vfs_stat(path, &st) == VFS_OK && st.type == VFS_TYPE_DIR)
                    kprintf("rm: '%s' is a directory -- use -r to remove it "
                            "and its contents\n", argv[argi]);
                else
                    kprintf("rm: '%s': %s\n", argv[argi], vfs_strerror(rc));
                failed = 1;
            }
        }
    }
    shell_set_status(failed);
}

/* `write <path> <text...>` -- joins all remaining args with single
 * spaces and writes them to `path`, creating/truncating as needed.
 * No newline is appended -- so `cat` will print the bytes verbatim. */
static void cmd_write(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: write <path> <text>\n");
        kprintf("       e.g. write /data/notes/todo buy groceries\n");
        shell_set_status(1);
        return;
    }
    /* Join argv[2..argc-1] into a single buffer with ' ' separators. */
    char body[512];
    size_t pos = 0;
    for (int i = 2; i < argc; i++) {
        size_t alen = strlen(argv[i]);
        if (pos + alen + 1 >= sizeof(body)) {
            kprintf("write: input too long (max %lu bytes)\n",
                    (unsigned long)(sizeof(body) - 1));
            shell_set_status(1);
            return;
        }
        if (i > 2) body[pos++] = ' ';
        memcpy(body + pos, argv[i], alen);
        pos += alen;
    }
    body[pos] = 0;

    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(argv[1], path, sizeof(path), "write") < 0) return;
    int rc = vfs_write_all(path, body, pos);
    if (rc != VFS_OK) {
        kprintf("write: '%s': %s\n", argv[1], vfs_strerror(rc));
        shell_set_status(1);
        return;
    }
    kprintf("write: '%s' <- %lu byte%s\n", argv[1],
            (unsigned long)pos, pos == 1 ? "" : "s");
    shell_set_status(0);
}

static void cmd_mounts(int argc, char **argv) {
    (void)argc; (void)argv;
    vfs_dump_mounts();
}

static void cmd_page(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: page <hex-virt-addr>\n");
        return;
    }
    uint64_t virt;
    if (parse_hex(argv[1], &virt) < 0) {
        kprintf("page: bad hex '%s'\n", argv[1]);
        return;
    }
    vmm_dump(virt);
    uint64_t phys = vmm_translate(virt);
    if (phys == 0) {
        kprintf("page: virt %p is not mapped\n", (void *)virt);
    } else {
        kprintf("page: virt %p -> phys %p\n", (void *)virt, (void *)phys);
    }
}

static void cmd_cpus(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t total = smp_cpu_count();
    if (total == 0) {
        kprintf("cpus: SMP not initialised\n");
        return;
    }
    kprintf("idx  apic_id  role  online  stack_top\n");
    for (uint32_t i = 0; i < total; i++) {
        const struct percpu *c = smp_cpu(i);
        kprintf("%-3u  %-7u  %-4s  %-6s  %p\n",
                (unsigned)c->cpu_idx,
                (unsigned)c->apic_id,
                c->is_bsp ? "BSP"  : "AP",
                c->online ? "yes"  : "no",
                (void *)c->stack_top);
    }
    kprintf("online: %u / %u  (current cpu apic_id=%u)\n",
            smp_online_count(), total, (unsigned)apic_read_id());
}

/* Both reboot and shutdown drain output before pulling the trigger
 * because acpi_reboot/acpi_shutdown disable interrupts and never
 * return -- without the spin-wait, the kprintf above might not flush
 * to the serial UART / VGA before the platform is gone. */
static void drain_console(void) {
    for (volatile int i = 0; i < 5000000; i++) { }
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("rebooting...\n");
    drain_console();
    acpi_reboot();   /* noreturn -- tries FADT reset, PCI 0xCF9, 8042, triple-fault */
}

static void cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("shutting down...\n");
    drain_console();
    acpi_shutdown(); /* noreturn -- writes SLP_TYPa | SLP_EN to PM1a_CNT */
}

static void cmd_ifconfig(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!net_is_up()) {
        kprintf("ifconfig: no NIC -- networking disabled\n");
        return;
    }
    char ipbuf[16], mskbuf[16], gwbuf[16], dnsbuf[16], macbuf[18];
    net_format_ip (ipbuf,  g_my_ip);
    net_format_ip (mskbuf, g_my_netmask);
    net_format_ip (gwbuf,  g_gateway_ip);
    net_format_ip (dnsbuf, g_my_dns_be);
    net_format_mac(macbuf, g_my_mac);
    kprintf("eth0:\n");
    kprintf("  inet     %s  netmask %s\n", ipbuf, mskbuf);
    kprintf("  ether    %s\n", macbuf);
    kprintf("  gateway  %s\n", gwbuf);
    kprintf("  dns      %s\n", dnsbuf);
}

static void cmd_dhcp(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!net_is_up()) {
        kprintf("dhcp: no NIC -- networking disabled\n");
        return;
    }
    if (net_dhcp_renew()) {
        kprintf("dhcp: lease renewed (see ifconfig for details)\n");
    } else {
        kprintf("dhcp: renew failed (kept previous lease)\n");
    }
}

static void cmd_nslookup(int argc, char **argv) {
    if (!net_is_up()) {
        kprintf("nslookup: no NIC -- networking disabled\n");
        return;
    }
    if (argc < 2) {
        kprintf("usage: nslookup <hostname>\n");
        return;
    }
    if (g_my_dns_be == 0) {
        kprintf("nslookup: no DNS server known (DHCP did not provide one)\n");
        return;
    }
    char dnsbuf[16];
    net_format_ip(dnsbuf, g_my_dns_be);
    kprintf("Server:  %s\n", dnsbuf);

    struct dns_result r;
    if (!dns_resolve(argv[1], 3000, &r)) {
        kprintf("nslookup: failed (see [dns] log lines above)\n");
        return;
    }
    char ipbuf[16];
    net_format_ip(ipbuf, r.ip_be);
    kprintf("Name:    %s\nAddress: %s\nTTL:     %u s\n",
            argv[1], ipbuf, (unsigned)r.ttl_secs);
}

static void cmd_arp(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!net_is_up()) { kprintf("arp: no NIC\n"); return; }
    arp_dump();
}

static void cmd_netstat(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!net_is_up()) { kprintf("netstat: no NIC\n"); return; }
    kprintf("UDP sockets:\n");
    sock_dump();
    kprintf("TCP connections:\n");
    tcp_dump();
}

/* Tiny dotted-quad parser. Returns true on success and writes the
 * result in network byte order. Doesn't bother with hostnames -- the
 * shell has nslookup for that. */
static bool parse_ipv4(const char *s, uint32_t *out_be) {
    uint32_t parts[4] = {0,0,0,0};
    int p = 0;
    int v = 0;
    bool any = false;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 255) return false;
            any = true;
        } else if (*s == '.') {
            if (!any || p >= 3) return false;
            parts[p++] = (uint32_t)v;
            v = 0;
            any = false;
        } else {
            return false;
        }
        s++;
    }
    if (!any || p != 3) return false;
    parts[p] = (uint32_t)v;
    *out_be = ip4((uint8_t)parts[0], (uint8_t)parts[1],
                  (uint8_t)parts[2], (uint8_t)parts[3]);
    return true;
}

static int parse_port(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
        s++;
    }
    if (*s) return -1;
    return v;
}

/* `tcpconn <ip|host> <port>` -- open a TCP connection, log the
 * state transitions, then immediately close it. Stand-alone smoke
 * test for the 24C state machine. */
static void cmd_tcpconn(int argc, char **argv) {
    if (!net_is_up()) { kprintf("tcpconn: no NIC -- networking disabled\n"); return; }
    if (argc < 3) {
        kprintf("usage: tcpconn <ip|host> <port>\n");
        return;
    }
    int port = parse_port(argv[2]);
    if (port <= 0 || port > 65535) {
        kprintf("tcpconn: bad port '%s'\n", argv[2]);
        return;
    }

    uint32_t ip_be = 0;
    if (!parse_ipv4(argv[1], &ip_be)) {
        /* Fall back to DNS resolution. */
        if (g_my_dns_be == 0) {
            kprintf("tcpconn: not an IP and no DNS server (DHCP off?)\n");
            return;
        }
        struct dns_result r;
        kprintf("tcpconn: resolving '%s'...\n", argv[1]);
        if (!dns_resolve(argv[1], 3000, &r)) {
            kprintf("tcpconn: hostname lookup failed\n");
            return;
        }
        ip_be = r.ip_be;
    }
    char ipbuf[16]; net_format_ip(ipbuf, ip_be);
    kprintf("tcpconn: connecting to %s:%d ...\n", ipbuf, port);

    struct tcp_conn *c = tcp_connect(ip_be, htons((uint16_t)port), 5000);
    if (!c) {
        kprintf("tcpconn: connect failed\n");
        return;
    }
    kprintf("tcpconn: ESTABLISHED -- closing.\n");
    tcp_close(c);
    kprintf("tcpconn: done.\n");
}

/* `httpget <url> [vfs-path]` -- fetch a URL via HTTP/1.0.
 *
 *   httpget http://10.0.2.2:8000/foo.txt              # print to console
 *   httpget http://10.0.2.2:8000/big.bin /data/big.bin # save to file
 *
 * Caps the response at 4 MiB (heap budget) and uses the default
 * 5-second per-recv timeout. */
static void cmd_httpget(int argc, char **argv) {
    if (!net_is_up()) { kprintf("httpget: no NIC -- networking disabled\n"); return; }
    if (argc < 2) {
        kprintf("usage: httpget <url> [vfs-path]\n");
        kprintf("       httpget http://10.0.2.2:8000/foo.txt\n");
        kprintf("       httpget http://10.0.2.2:8000/foo.bin /data/foo.bin\n");
        return;
    }

    struct http_response resp;
    int rc = http_get(argv[1], /*max=*/4u * 1024u * 1024u,
                      /*timeout_ms=*/0, &resp);
    if (rc != 0) {
        kprintf("httpget: %s (%d)\n", http_strerror(rc), rc);
        return;
    }

    kprintf("httpget: HTTP %d %s -- %lu bytes (%s)\n",
            resp.status, resp.reason,
            (unsigned long)resp.body_len,
            resp.content_type[0] ? resp.content_type : "no content-type");

    if (argc >= 3) {
        const char *out_path = argv[2];
        int wrc = vfs_write_all(out_path, resp.body, resp.body_len);
        if (wrc != VFS_OK) {
            kprintf("httpget: write '%s' failed: %s\n",
                    out_path, vfs_strerror(wrc));
        } else {
            kprintf("httpget: saved %lu bytes to %s\n",
                    (unsigned long)resp.body_len, out_path);
        }
    } else {
        /* Print to console, capped so we don't flood the screen with
         * a huge HTML page. */
        size_t cap = resp.body_len;
        if (cap > 1024) cap = 1024;
        for (size_t i = 0; i < cap; i++) {
            char b[2] = { (char)resp.body[i], 0 };
            /* Sanitise CRs so the line layout stays sensible. */
            if (b[0] == '\r') continue;
            kprintf("%s", b);
        }
        if (resp.body_len > cap) {
            kprintf("\n... (%lu more bytes; use 'httpget <url> <path>' to save)\n",
                    (unsigned long)(resp.body_len - cap));
        } else {
            kprintf("\n");
        }
    }

    http_free(&resp);
}

static void cmd_gui(int argc, char **argv) {
    (void)argc; (void)argv;
    /* `gui [name]` -- spawns /bin/<name> in the foreground, defaulting
     * to gui_demo. The compositor auto-activates on the first window
     * create syscall and auto-deactivates when the last window closes,
     * so we don't need a separate "exit GUI" path here -- Ctrl+C the
     * foreground program (or close all its windows) to come back. */
    const char *prog = (argc >= 2) ? argv[1] : "gui_demo";
    char path_buf[64];
    const char *path = resolve_program(prog, path_buf, sizeof(path_buf));
    char *fake_argv[2] = { (char *)prog, 0 };
    shell_spawn_program(path, 1, fake_argv, false);
}

static void cmd_panic(int argc, char **argv) {
    (void)argc; (void)argv;
    kpanic("user-initiated panic from shell");
}

/* Milestone 20: install tobyOS from the live ISO onto the primary
 * IDE disk. Usage:
 *
 *   install           -- show what would happen (dry run)
 *   install --yes     -- actually flash + format
 *
 * The `--yes` guard is intentional: the operation wipes the target
 * disk's first 4 MiB + a fresh tobyfs region, and we don't want a
 * stray keystroke to destroy someone's persistent /data. */
static void cmd_install(int argc, char **argv) {
    bool confirmed = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
            confirmed = true;
        }
    }

    if (!installer_image_available()) {
        kprintf("install: no install image loaded -- are you running "
                "from the live ISO?\n");
        kprintf("         (when booted from an installed disk, there is "
                "nothing to re-install.)\n");
        return;
    }

    /* Milestone 21: the registry knows about every block device that
     * successfully probed during PCI binding. blk_get_first() returns
     * the first one (IDE in QEMU's default i440fx; AHCI/NVMe on later
     * platforms once those drivers land). */
    struct blk_dev *target = blk_get_first();
    if (!target) {
        kprintf("install: no target disk (no block device registered).\n");
        return;
    }

    uint32_t img_kib = (installer_image_size() + 1023u) / 1024u;
    uint64_t tgt_kib = target->sector_count / 2u;

    kprintf("installer:\n");
    kprintf("  source   : live ISO module 'install.img' (%u KiB)\n", img_kib);
    kprintf("  target   : %s (%lu KiB)\n", target->name, (unsigned long)tgt_kib);
    kprintf("  layout   : LBA 0..%u = Limine boot image\n",
            INSTALLER_BOOT_SECTORS - 1);
    kprintf("             LBA %u..%u = fresh tobyfs /data partition\n",
            INSTALLER_BOOT_SECTORS,
            INSTALLER_BOOT_SECTORS +
                TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK - 1);

    if (!confirmed) {
        kprintf("\nThis will ERASE all data on %s.\n"
                "Re-run with `install --yes` to proceed.\n",
                target->name);
        return;
    }

    kprintf("\nStarting install. Do not power off until this completes.\n");
    int rc = installer_run(target);
    if (rc != 0) {
        kprintf("install: FAILED (rc=%d)\n", rc);
        return;
    }
    kprintf("install: SUCCESS -- type `reboot` and remove the CD-ROM "
            "on next boot.\n");
}

/* ---- milestone 23A: storage / partition diagnostics --------------- */

/* Render a 16-byte mixed-endian GUID as the canonical written form
 *   xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
 * Reverses the first three groups (LE in the on-disk form) and prints
 * the last two byte-wise. Caller-owned buffer; needs >= 37 bytes. */
static void format_guid(const uint8_t g[BLK_GUID_BYTES], char *out) {
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
                                   8, 9, 10, 11, 12, 13, 14, 15 };
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        uint8_t b = g[order[i]];
        out[o++] = hex[(b >> 4) & 0xF];
        out[o++] = hex[b & 0xF];
    }
    out[o] = 0;
}

static const char *guess_type_name(const uint8_t g[BLK_GUID_BYTES]) {
    if (partition_guid_cmp(g, GPT_TYPE_TOBYOS_DATA)   == 0) return "tobyOS-data";
    if (partition_guid_cmp(g, GPT_TYPE_EFI_SYSTEM)    == 0) return "EFI System";
    if (partition_guid_cmp(g, GPT_TYPE_BIOS_BOOT)     == 0) return "BIOS Boot";
    if (partition_guid_cmp(g, GPT_TYPE_MS_BASIC_DATA) == 0) return "MS Basic Data";
    if (partition_guid_cmp(g, GPT_TYPE_LINUX_FS)      == 0) return "Linux fs";
    if (partition_guid_cmp(g, GPT_TYPE_LINUX_HOME)    == 0) return "Linux /home";
    return "unknown";
}

static const char *class_name(enum blk_dev_class c) {
    switch (c) {
    case BLK_CLASS_DISK:      return "disk";
    case BLK_CLASS_PARTITION: return "part";
    case BLK_CLASS_WRAPPER:   return "wrap";
    default:                  return "?";
    }
}

/* `blkdump` -- one row per block device + one optional verbose row per
 * partition with full GUID + label. With no args: short form (delegates
 * to blk_dump). With `-v` / `--verbose`: full GUID strings + parent
 * link + offset summary. */
static void cmd_blkdump(int argc, char **argv) {
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 ||
            strcmp(argv[i], "--verbose") == 0) verbose = true;
    }
    if (!verbose) {
        blk_dump();
        kprintf("(use 'blkdump -v' for partition GUIDs + labels)\n");
        return;
    }
    size_t n = blk_count();
    if (n == 0) {
        kprintf("blkdump: no block devices registered\n");
        return;
    }
    kprintf("blkdump: %lu device(s)\n", (unsigned long)n);
    for (size_t i = 0; i < n; i++) {
        struct blk_dev *d = blk_get(i);
        if (!d) continue;
        kprintf("  [%2lu] %-22s class=%-4s sectors=%-8lu (%lu KiB)\n",
                (unsigned long)i,
                d->name ? d->name : "(anon)",
                class_name(d->class),
                (unsigned long)d->sector_count,
                (unsigned long)(d->sector_count / 2u));
        if (d->class == BLK_CLASS_PARTITION) {
            char gbuf[40];
            format_guid(d->type_guid, gbuf);
            kprintf("       parent=%-12s offset=%lu  index=%u\n",
                    d->parent && d->parent->name ? d->parent->name : "?",
                    (unsigned long)d->offset_lba,
                    (unsigned)d->partition_index);
            kprintf("       type=%s (%s)\n", gbuf, guess_type_name(d->type_guid));
            if (d->partition_label[0]) {
                kprintf("       label='%s'\n", d->partition_label);
            }
        } else if (d->class == BLK_CLASS_WRAPPER) {
            kprintf("       (legacy offset wrapper -- no partition metadata)\n");
        }
    }
}

/* `devlist [bus]` -- M26A peripheral inventory. Walks every
 * introspected subsystem via devtest_dump_kprintf (which itself
 * iterates devtest_enumerate). Optional argv[1] filters by bus name
 * (pci/usb/blk/input/audio/battery/hub). The kernel printer uses the
 * same `[INFO]` format the userland `devlist` binary emits, so a
 * shell-side and ring-3 invocation produce visually identical lines. */
static void cmd_devlist(int argc, char **argv) {
    uint32_t mask = ABI_DEVT_BUS_ALL;
    if (argc >= 2) {
        const char *s = argv[1];
        if      (!strcmp(s, "pci"))     mask = ABI_DEVT_BUS_PCI;
        else if (!strcmp(s, "usb"))     mask = ABI_DEVT_BUS_USB;
        else if (!strcmp(s, "blk"))     mask = ABI_DEVT_BUS_BLK;
        else if (!strcmp(s, "input"))   mask = ABI_DEVT_BUS_INPUT;
        else if (!strcmp(s, "audio"))   mask = ABI_DEVT_BUS_AUDIO;
        else if (!strcmp(s, "battery")) mask = ABI_DEVT_BUS_BATTERY;
        else if (!strcmp(s, "hub"))     mask = ABI_DEVT_BUS_HUB;
        else if (!strcmp(s, "all"))     mask = ABI_DEVT_BUS_ALL;
        else { kprintf("devlist: unknown bus '%s'\n", s); return; }
    }
    devtest_dump_kprintf(mask);
}

/* `hwinfo [persist]` -- M29A hardware-inventory shell builtin.
 * Default behaviour mirrors `/bin/hwinfo`: dump the cached
 * inventory via kprintf. `hwinfo persist` additionally writes the
 * snapshot to /data/hwinfo.snap and prints the rc. Convenient for
 * an interactive operator who wants to grab a fresh snapshot for
 * post-mortem debugging without having to spawn a userland tool. */
static void cmd_hwinfo(int argc, char **argv) {
    bool do_persist = false;
    if (argc >= 2) {
        if (!strcmp(argv[1], "persist")) {
            do_persist = true;
        } else if (!strcmp(argv[1], "help") ||
                   !strcmp(argv[1], "--help")) {
            kprintf("usage: hwinfo [persist]\n");
            return;
        }
    }
    hwinfo_dump_kprintf();
    if (do_persist) {
        long rc = hwinfo_persist();
        if (rc > 0) {
            kprintf("hwinfo: persisted %ld bytes -> /data/hwinfo.snap\n",
                    rc);
        } else if (rc == 0) {
            kprintf("hwinfo: SKIP persist (/data not available)\n");
        } else {
            kprintf("hwinfo: FAIL persist rc=%ld\n", rc);
        }
    }
}

/* `drvmatch` -- M29B driver matching + fallback report.
 *
 * No args  -> dump the live drvmatch table.
 * `disable <driver>` / `reenable <driver>` -- test-only knobs that
 * call drvmatch_disable_pci / drvmatch_reenable_pci. Both are
 * meant for interactive debugging; the boot-time M29B harness
 * uses the same APIs from kernel.c. */
static void cmd_drvmatch(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "help") ||
                      !strcmp(argv[1], "--help"))) {
        kprintf("usage: drvmatch [list|disable <drv>|reenable <drv>]\n");
        return;
    }
    if (argc >= 3 && !strcmp(argv[1], "disable")) {
        long rc = drvmatch_disable_pci(argv[2]);
        kprintf("drvmatch: disable '%s' rc=%ld\n", argv[2], rc);
        return;
    }
    if (argc >= 3 && !strcmp(argv[1], "reenable")) {
        long rc = drvmatch_reenable_pci(argv[2]);
        kprintf("drvmatch: reenable '%s' rc=%ld\n", argv[2], rc);
        return;
    }
    drvmatch_dump_kprintf();
}

/* `drvtest [name ...]` -- M26A driver self-test runner.
 *
 * No args  -> walk every registered test (devtest_for_each).
 * With args -> run only the named tests via devtest_run, in order.
 *
 * Output uses the same `[PASS]/[SKIP]/[FAIL]` shape devtest_boot_run
 * emits, so a single grep can reconcile boot-time + on-demand runs. */
static void cmd_drvtest_walk(const char *name, int rc, const char *msg,
                             void *cookie) {
    int *counters = (int *)cookie;
    const char *tag;
    if      (rc == 0)             { tag = "PASS"; counters[0]++; }
    else if (rc == ABI_DEVT_SKIP) { tag = "SKIP"; counters[2]++; }
    else                          { tag = "FAIL"; counters[1]++; }
    kprintf("[%s] %s: %s\n", tag, name, msg && msg[0] ? msg : "(no message)");
}
static void cmd_drvtest(int argc, char **argv) {
    int counters[3] = {0, 0, 0};   /* pass, fail, skip */
    int total;
    if (argc <= 1) {
        total = devtest_for_each(cmd_drvtest_walk, counters);
    } else {
        char msg[ABI_DEVT_MSG_MAX];
        total = 0;
        for (int i = 1; i < argc; i++) {
            msg[0] = '\0';
            int rc = devtest_run(argv[i], msg, sizeof msg);
            cmd_drvtest_walk(argv[i], rc, msg, counters);
            total++;
        }
    }
    kprintf("drvtest: %d test(s) -- pass=%d fail=%d skip=%d\n",
            total, counters[0], counters[1], counters[2]);
}

/* `partprobe [device]` -- rescan the GPT on one disk (by name) or
 * every registered disk. New partitions register lazily; existing
 * ones with the same "<disk>.pN" name are skipped. Useful after a
 * disk-label edit, or for re-running the discovery on demand from the
 * shell. */
static void cmd_partprobe(int argc, char **argv) {
    if (argc < 2) {
        int n = partition_scan_all();
        if (n < 0) {
            kprintf("partprobe: no GPT-formatted disks found\n");
        } else {
            kprintf("partprobe: %d partition(s) registered "
                    "(may include pre-existing entries)\n", n);
        }
        return;
    }
    struct blk_dev *d = blk_find(argv[1]);
    if (!d) {
        kprintf("partprobe: no block device '%s'\n", argv[1]);
        return;
    }
    if (d->class != BLK_CLASS_DISK) {
        kprintf("partprobe: '%s' is %s, not a disk -- "
                "cannot scan for partitions\n",
                argv[1], class_name(d->class));
        return;
    }
    int n = partition_scan_disk(d);
    if (n < 0) {
        kprintf("partprobe: '%s' has no GPT (or read failed)\n", argv[1]);
    } else {
        kprintf("partprobe: '%s' -- %d partition(s)\n", argv[1], n);
    }
}

/* `mountfs <mountpoint> <blkdev> [type]` -- in-kernel mount helper.
 *
 * If `type` is omitted we sniff the device:
 *   1. Try fat32_probe() -- looks for a valid BPB + 0x55AA signature.
 *   2. Otherwise assume tobyfs (it'll fail with a useful magic-mismatch
 *      message if there's no tobyfs there either).
 *
 * Explicit `type` values: 'tobyfs', 'fat32'.
 *
 * The block device argument is looked up by name in the registry
 * (e.g. 'ide0:master.p2', 'nvme0:n1.p1'). */
static void cmd_mountfs(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: mountfs <mountpoint> <blkdev> [type]\n");
        kprintf("       e.g. mountfs /data ide0:master.p2\n");
        kprintf("            mountfs /fat ide0:master.p3 fat32\n");
        kprintf("       known types: tobyfs, fat32 (auto-detected if omitted)\n");
        return;
    }
    struct blk_dev *d = blk_find(argv[2]);
    if (!d) {
        kprintf("mountfs: no block device '%s' (try 'blkdump')\n", argv[2]);
        return;
    }

    const char *type = argc >= 4 ? argv[3] : 0;
    if (type) {
        int rc;
        if (strcmp(type, "fat32") == 0) {
            rc = fat32_mount(argv[1], d);
            if (rc != VFS_OK) {
                kprintf("mountfs: '%s' on '%s' (fat32): %s\n",
                        argv[2], argv[1], vfs_strerror(rc));
                return;
            }
            kprintf("mountfs: mounted '%s' (fat32 from '%s')\n", argv[1], argv[2]);
        } else if (strcmp(type, "tobyfs") == 0) {
            rc = tobyfs_mount(argv[1], d);
            if (rc != VFS_OK) {
                kprintf("mountfs: '%s' on '%s' (tobyfs): %s\n",
                        argv[2], argv[1], vfs_strerror(rc));
                return;
            }
            kprintf("mountfs: mounted '%s' (tobyfs from '%s')\n", argv[1], argv[2]);
        } else {
            kprintf("mountfs: unknown type '%s' (want 'tobyfs' or 'fat32')\n", type);
        }
        return;
    }

    /* Auto-detect. */
    if (fat32_probe(d)) {
        int rc = fat32_mount(argv[1], d);
        if (rc != VFS_OK) {
            kprintf("mountfs: '%s' on '%s' (fat32, auto): %s\n",
                    argv[2], argv[1], vfs_strerror(rc));
            return;
        }
        kprintf("mountfs: mounted '%s' (fat32 from '%s', auto)\n",
                argv[1], argv[2]);
        return;
    }
    int rc = tobyfs_mount(argv[1], d);
    if (rc != VFS_OK) {
        kprintf("mountfs: '%s' on '%s' (tobyfs, auto): %s\n",
                argv[2], argv[1], vfs_strerror(rc));
        return;
    }
    kprintf("mountfs: mounted '%s' (tobyfs from '%s', auto)\n",
            argv[1], argv[2]);
}

/* ---- milestone 16: package manager ------------------------------- *
 *
 * Thin dispatcher that forwards to the pkg_* API in src/pkg.c. Usage:
 *
 *     pkg install <name-or-path>     -- pull a .tpkg onto disk
 *     pkg remove  <name>              -- uninstall
 *     pkg list                        -- what's installed
 *     pkg info    <name>              -- print the install record
 *     pkg repo                        -- list /data/repo + /repo
 *
 * Every subcommand is idempotent w.r.t. the launcher: the pkg module
 * refreshes the desktop's dynamic entries after any change. */
static void pkg_usage(void) {
    kprintf("usage: pkg <subcmd> [args]\n");
    kprintf("  pkg install <name-or-path>   install from /data/repo, /repo, or an explicit .tpkg\n");
    kprintf("  pkg remove  <name>           uninstall by package name\n");
    kprintf("  pkg list                     list installed packages\n");
    kprintf("  pkg info    <name>           show one package's install record\n");
    kprintf("  pkg repo                     list available .tpkg files\n");
    kprintf("  pkg update                   show installed packages with newer versions in repo\n");
    kprintf("  pkg upgrade [name]           upgrade one (or all) packages to latest available\n");
    kprintf("  pkg rollback <name>          restore a package from its .bak (post-upgrade)\n");
}

static void cmd_pkg(int argc, char **argv) {
    if (argc < 2) { pkg_usage(); return; }
    const char *sub = argv[1];

    if (!strcmp(sub, "install")) {
        if (argc < 3) { kprintf("usage: pkg install <name|path|http://url>\n"); return; }
        const char *arg = argv[2];

        /* Milestone 24D: detect http:// URLs and route through the
         * download-then-install path. Case-insensitive on the scheme
         * to mirror http_parse_url(). */
        bool is_url = false;
        if ((arg[0] == 'h' || arg[0] == 'H') &&
            (arg[1] == 't' || arg[1] == 'T') &&
            (arg[2] == 't' || arg[2] == 'T') &&
            (arg[3] == 'p' || arg[3] == 'P') &&
            arg[4] == ':' && arg[5] == '/' && arg[6] == '/') is_url = true;

        if (is_url) {
            int rc = pkg_install_url(arg);
            if (rc != 0) kprintf("pkg install: failed\n");
            return;
        }

        /* Anything containing a '/' is treated as an explicit file path;
         * bare names go through the repo search. */
        bool is_path = false;
        for (const char *c = arg; *c; c++) if (*c == '/') { is_path = true; break; }
        int rc = is_path ? pkg_install_path(arg) : pkg_install_name(arg);
        if (rc != 0) kprintf("pkg install: failed\n");
        return;
    }
    if (!strcmp(sub, "remove")) {
        if (argc < 3) { kprintf("usage: pkg remove <name>\n"); return; }
        if (pkg_remove(argv[2]) != 0) kprintf("pkg remove: failed\n");
        return;
    }
    if (!strcmp(sub, "list")) {
        (void)argc;
        pkg_list();
        return;
    }
    if (!strcmp(sub, "info")) {
        if (argc < 3) { kprintf("usage: pkg info <name>\n"); return; }
        (void)pkg_info(argv[2]);
        return;
    }
    if (!strcmp(sub, "repo")) {
        pkg_repo_dump();
        return;
    }
    if (!strcmp(sub, "update")) {
        pkg_update();
        return;
    }
    if (!strcmp(sub, "upgrade")) {
        if (argc < 3) {
            (void)pkg_upgrade_all();
        } else {
            (void)pkg_upgrade_one(argv[2]);
        }
        return;
    }
    if (!strcmp(sub, "rollback")) {
        if (argc < 3) { kprintf("usage: pkg rollback <name>\n"); return; }
        (void)pkg_rollback(argv[2]);
        return;
    }
    kprintf("pkg: unknown subcommand '%s'\n", sub);
    pkg_usage();
}

/* `desktop` -- enter the milestone-12 desktop environment.
 *
 * We just flip the GUI's "desktop mode" flag on. The compositor (in
 * gui.c) takes over the framebuffer immediately, paints the
 * wallpaper + taskbar, and starts dispatching mouse/keyboard input.
 * The shell is still running (pid 0, idle loop) underneath; the
 * compositor's launcher menu has an "Exit Desktop" entry that flips
 * the flag back off and returns the framebuffer to console_tick. */
static void cmd_desktop(int argc, char **argv) {
    (void)argc; (void)argv;
    if (gui_in_desktop_mode()) {
        kprintf("desktop: already running -- click [Apps] -> Exit Desktop to leave\n");
        return;
    }
    /* Auto-bump the trace to NORMAL so the operator gets a log of
     * the very first interaction. They can `trace off` to silence it
     * or `trace verbose` to also capture every GUI syscall. */
    if (gui_trace_level() == GUI_TRACE_OFF) {
        kprintf("desktop: enabling activity trace (use `trace off` to silence)\n");
        gui_trace_set(GUI_TRACE_NORMAL);
    }
    gui_set_desktop_mode(true);
    kprintf("desktop: entered (Apps menu in taskbar, X to close windows)\n");
    kprintf("        emergency hotkeys (also shown on-screen above the taskbar):\n");
    kprintf("          F1  / F11        -> dump GUI status to serial.log\n");
    kprintf("          F2  / F12        -> force-exit desktop + SIGINT all apps\n");
    kprintf("          Pause/Break      -> same as F2 (works on every host)\n");
    kprintf("        Note: some QEMU hosts intercept F11 (full-screen) and F12;\n");
    kprintf("              prefer F1/F2 if those don't seem to do anything.\n");
}

/* `trace [on|off|verbose|status]` -- control the desktop-activity
 * trace. Output goes to serial (and the framebuffer console when text
 * mode is up) prefixed with `[trace t=<ticks> pid=<n>] ` so the stream
 * can be grep'd out of serial.log later. */
static void cmd_trace(int argc, char **argv) {
    if (argc < 2) {
        const char *name = "off";
        switch (gui_trace_level()) {
        case GUI_TRACE_OFF:     name = "off";     break;
        case GUI_TRACE_NORMAL:  name = "on";      break;
        case GUI_TRACE_VERBOSE: name = "verbose"; break;
        }
        kprintf("trace: level=%d (%s)\n", gui_trace_level(), name);
        kprintf("usage: trace [on|off|verbose|status]\n");
        kprintf("  on      desktop control flow (clicks, launches, reaps)\n");
        kprintf("  verbose on + per-call GUI syscalls (gui_fill / flip / poll)\n");
        kprintf("  off     disable\n");
        return;
    }
    if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0) {
        gui_trace_set(GUI_TRACE_OFF);
    } else if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0) {
        gui_trace_set(GUI_TRACE_NORMAL);
    } else if (strcmp(argv[1], "verbose") == 0 || strcmp(argv[1], "2") == 0) {
        gui_trace_set(GUI_TRACE_VERBOSE);
    } else if (strcmp(argv[1], "status") == 0) {
        kprintf("trace: level=%d\n", gui_trace_level());
    } else if (strcmp(argv[1], "dump") == 0) {
        gui_dump_status("shell `trace dump`");
    } else if (strcmp(argv[1], "panic") == 0 ||
               strcmp(argv[1], "kill")  == 0) {
        gui_emergency_exit("shell `trace panic`");
    } else {
        kprintf("trace: unknown subcommand '%s'\n", argv[1]);
        kprintf("       try on / off / verbose / status / dump / panic\n");
    }
}

/* ---- milestone 18: capability / sandbox inspection --------------- */

/* One row per profile for `caps` printout. */
static int caps_print_profile(void *ctx, const struct cap_profile *p) {
    (void)ctx;
    char mask[96];
    cap_mask_to_string(p->caps, mask, sizeof(mask));
    kprintf("  %-18s  caps=0x%08x [%s]  root='%s'\n",
            p->name, (unsigned)p->caps, mask,
            p->root[0] ? p->root : "<none>");
    return 0;
}

/* `caps` with no args: print the shell's own capability set + a table
 * of profiles the user can launch programs under. `caps <pid>` dumps a
 * specific process. */
static void cmd_caps(int argc, char **argv) {
    if (argc <= 1) {
        struct proc *me = current_proc();
        if (me) {
            char mask[96];
            cap_mask_to_string(me->caps, mask, sizeof(mask));
            kprintf("shell: pid=%d name='%s' caps=0x%08x [%s]\n",
                    me->pid, me->name, (unsigned)me->caps, mask);
            if (me->sandbox_root[0]) {
                kprintf("       sandbox_root='%s'\n", me->sandbox_root);
            }
        }
        kprintf("\navailable sandbox profiles:\n");
        cap_profile_foreach(caps_print_profile, 0);
        kprintf("\nuse 'sandbox <profile> <cmd...>' or 'run --sandbox <profile> <path>'\n");
        return;
    }

    int pid = 0;
    if (parse_int(argv[1], &pid) < 0 || pid < 0) {
        kprintf("caps: bad pid '%s'\n", argv[1]);
        return;
    }
    struct proc *p = proc_lookup(pid);
    if (!p) { kprintf("caps: no such pid %d\n", pid); return; }
    cap_dump_proc(p);
}

/* M34F: `auditlog` -- dump the slog ring filtered to security-relevant
 * subsystems, in newest-last order, the way an admin reading a UNIX
 * /var/log/auth.log would expect. We deliberately keep this a kernel
 * builtin: it has direct access to slog_drain() and stays available
 * even if userland is wedged. The user-facing summary line at the end
 * is the cue tooling and humans look for ("auditlog: shown=N matched=M
 * total=T dropped=D").
 *
 * Filtering rules:
 *
 *   - Default subsystems: audit, sysprot, sec, pkg.
 *     These are the M34 sources.  pkg is included because the package
 *     manager already emits structured INFO lines for install/remove/
 *     upgrade/rollback outcomes -- M34F just makes them easy to find.
 *   - --sub=name        : restrict to one subsystem (overrides default).
 *   - --all             : show every subsystem (no audit filter).
 *   - --level=warn|info|...: drop records above the chosen severity
 *                            (e.g. --level=warn = ERROR + WARN only).
 *   - --since=<seq>     : start from a specific sequence number; useful
 *                         to "follow" the audit stream from tests.
 *   - -n N              : cap output at the latest N matching records.
 *
 * Output format is one line per record:
 *
 *   [seq] time_ms LEVEL sub pid=N message
 *
 * which matches what `logview` would print, so muscle memory carries.
 */
static const char *const k_audit_default_subs[] = {
    SLOG_SUB_AUDIT, SLOG_SUB_SYSPROT, SLOG_SUB_SEC, "pkg"
};

static bool audit_match_sub(const char *sub, bool show_all,
                            const char *only_sub) {
    if (show_all) return true;
    if (only_sub && only_sub[0]) {
        /* sub field is a fixed-size buffer (NUL-padded) so strcmp is
         * fine here -- the kernel writer always NUL-terminates. */
        return strcmp(sub, only_sub) == 0;
    }
    for (size_t i = 0;
         i < sizeof(k_audit_default_subs)/sizeof(k_audit_default_subs[0]);
         i++) {
        if (strcmp(sub, k_audit_default_subs[i]) == 0) return true;
    }
    return false;
}

static void cmd_auditlog(int argc, char **argv) {
    bool        show_all  = false;
    const char *only_sub  = NULL;
    uint32_t    max_lvl   = ABI_SLOG_LEVEL_DEBUG;     /* show everything */
    uint64_t    since_seq = 0;
    int         tail_n    = -1;                       /* -1 == unlimited */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--all") == 0) {
            show_all = true;
        } else if (strncmp(a, "--sub=", 6) == 0) {
            only_sub = a + 6;
        } else if (strncmp(a, "--level=", 8) == 0) {
            uint32_t l = slog_level_from_name(a + 8);
            if (l >= ABI_SLOG_LEVEL_MAX) {
                kprintf("auditlog: bad level '%s' (use error|warn|info|debug)\n",
                        a + 8);
                return;
            }
            max_lvl = l;
        } else if (strncmp(a, "--since=", 8) == 0) {
            int v = 0;
            if (parse_int(a + 8, &v) < 0 || v < 0) {
                kprintf("auditlog: bad --since '%s'\n", a + 8);
                return;
            }
            since_seq = (uint64_t)v;
        } else if (strcmp(a, "-n") == 0 && i + 1 < argc) {
            int v = 0;
            if (parse_int(argv[++i], &v) < 0 || v <= 0) {
                kprintf("auditlog: bad -n '%s'\n", argv[i]);
                return;
            }
            tail_n = v;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            kprintf("usage: auditlog [--all] [--sub=NAME] [--level=warn|info|debug]\n");
            kprintf("                [--since=SEQ] [-n N]\n");
            kprintf("default subs: audit sysprot sec pkg (M34F)\n");
            return;
        } else {
            kprintf("auditlog: unknown arg '%s' (try --help)\n", a);
            return;
        }
    }

    /* Snapshot the ring. ABI_SLOG_RING_DEPTH is 256; one record is 256
     * bytes; ~64 KiB of stack is far too much, so use a static. The
     * shell is single-threaded so this is safe. */
    static struct abi_slog_record snap[ABI_SLOG_RING_DEPTH];
    uint32_t got = slog_drain(snap, ABI_SLOG_RING_DEPTH, since_seq);

    struct abi_slog_stats st;
    slog_stats(&st);

    /* First pass: count matches so the tail window can pick the right
     * starting offset without a second drain. */
    uint32_t matched = 0;
    for (uint32_t i = 0; i < got; i++) {
        if (snap[i].level > max_lvl) continue;
        if (!audit_match_sub(snap[i].sub, show_all, only_sub)) continue;
        matched++;
    }

    uint32_t skip = 0;
    if (tail_n > 0 && (uint32_t)tail_n < matched)
        skip = matched - (uint32_t)tail_n;

    uint32_t shown   = 0;
    uint32_t seen    = 0;
    uint16_t dropped = (got > 0) ? snap[0].dropped : 0;
    for (uint32_t i = 0; i < got; i++) {
        if (snap[i].level > max_lvl) continue;
        if (!audit_match_sub(snap[i].sub, show_all, only_sub)) continue;
        if (seen++ < skip) continue;
        kprintf("[%llu] %llums %s %-7s pid=%d %s\n",
                (unsigned long long)snap[i].seq,
                (unsigned long long)snap[i].time_ms,
                slog_level_name(snap[i].level),
                snap[i].sub,
                (int)snap[i].pid,
                snap[i].msg);
        shown++;
    }

    /* Tail summary: parsed by tests + audit_log review tooling. */
    kprintf("auditlog: shown=%u matched=%u total=%u dropped=%u\n",
            (unsigned)shown, (unsigned)matched,
            (unsigned)got, (unsigned)dropped);
    if (matched == 0 && !show_all && !only_sub) {
        kprintf("auditlog: (no audit-tagged records yet; try --all)\n");
    }
    /* Do NOT touch the ring; this is read-only. A subsequent invocation
     * will see the same records (modulo overflow), which is intentional
     * -- nobody wants `auditlog` to silently consume the audit trail. */
    (void)st;
}

/* M34G: `securitytest` -- run the integrated security validation
 * suite from the live kernel and report PASS/FAIL/SKIPPED. Wraps
 * sectest_run so the same code path is shared with the boot-time
 * autorun (-DSECTEST_AUTORUN). The test driver test_m34g.ps1 reads
 * the OVERALL: line that sectest_run prints. */
static void cmd_securitytest(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sectest_summary sum;
    sectest_run(&sum);
    /* Echo a short, shell-friendly closer in addition to the kprintf
     * line sectest_run already emits, so an interactive operator who
     * scrolled past the verbose middle still sees the verdict. */
    kprintf("securitytest: %s pass=%d fail=%d skip=%d total=%d\n",
            sum.fail == 0 ? "PASS" : "FAIL",
            sum.pass, sum.fail, sum.skip, sum.total);
}

/* `sandbox <profile> <cmd> [args...]`: equivalent to prefixing any
 * user-program invocation with a narrow capability set. */
static void cmd_sandbox(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: sandbox <profile> <cmd> [args...]\n");
        kprintf("       'caps' lists profile names\n");
        return;
    }
    if (!cap_profile_lookup(argv[1])) {
        kprintf("sandbox: unknown profile '%s' (try 'caps')\n", argv[1]);
        return;
    }
    shell_spawn_program_profile(argv[2], argc - 2, &argv[2],
                                /*background=*/false, argv[1]);
}

static bool shell_heredoc_delim_char(char c) {
    return c && c != ' ' && c != '\t' && c != ';' && c != '&' &&
           c != '|' && c != '<' && c != '>';
}

/* One `<<WORD` operator: its delimiter and how it was written. */
struct shell_heredoc_spec {
    char delim[64];
    bool strip_tabs;      /* <<- : leading tabs stripped from the body */
    bool quoted;          /* delimiter was quoted: body does not expand */
};

/* Find EVERY here-document operator on `line`, left to right, which is the
 * order their bodies follow the line in. Returns how many were found.
 *
 * This used to stop at the first one, so `cat <<A | cat <<B` collected one
 * body and then ran a command still waiting for a second. The consumer side
 * (g_heredocs) was always a queue -- only the producer was singular. */
static int shell_line_find_heredocs(const char *line,
                                    struct shell_heredoc_spec *out, int max) {
    bool in_single = false;
    bool in_double = false;
    int found = 0;

    for (const char *p = line; *p && found < max; p++) {
        if (in_single) {
            if (*p == '\'') in_single = false;
            continue;
        }
        if (in_double) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') in_double = false;
            continue;
        }
        if (*p == '\'') {
            in_single = true;
            continue;
        }
        if (*p == '"') {
            in_double = true;
            continue;
        }
        /* Skip over $(( )) and $( ). Arithmetic contains the LEFT SHIFT
         * operator, and `$((1<<4))` was being read as a here-document
         * introduced by `<<` -- which swallowed the rest of the script
         * looking for a delimiter and took the whole shift family with
         * it. Quotes were already skipped here; substitutions were not. */
        if (*p == '$' && p[1] == '(') {
            int depth = 0;
            p++;                       /* at the first '(' */
            while (*p) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (depth == 0) break; }
                p++;
            }
            if (!*p) break;            /* unterminated: nothing to find */
            continue;
        }
        if (*p == '`') {              /* legacy command substitution */
            p++;
            while (*p && *p != '`') p++;
            if (!*p) break;
            continue;
        }
        /* `<<<` is a here-STRING, not a here-document: it takes its text
         * from the same line and has no body to collect. Reading it as `<<`
         * would send the reader hunting for a delimiter that never comes. */
        if (p[0] != '<' || p[1] != '<' || p[2] == '<') continue;

        struct shell_heredoc_spec *sp = &out[found];
        sp->strip_tabs = false;
        sp->quoted = false;
        sp->delim[0] = '\0';

        p += 2;
        if (*p == '-') {
            sp->strip_tabs = true;
            p++;
        }
        while (*p == ' ' || *p == '\t') p++;

        size_t pos = 0;
        while (shell_heredoc_delim_char(*p)) {
            if (*p == '\'' || *p == '"') {
                char q = *p++;
                sp->quoted = true;
                while (*p && *p != q) {
                    if (pos + 1 >= sizeof sp->delim) return found;
                    sp->delim[pos++] = *p++;
                }
                if (*p == q) p++;
                continue;
            }
            if (pos + 1 >= sizeof sp->delim) return found;
            sp->delim[pos++] = *p++;
        }
        sp->delim[pos] = '\0';
        if (!sp->delim[0]) return found;       /* `<<` with no word */
        found++;
        /* Keep scanning from here. The for-header's p++ would skip the
         * character the delimiter scan stopped on, so step back one. */
        p--;
    }
    return found;
}

/* Single-result wrapper. The interactive reader is a state machine spread
 * across calls and still handles one here-document at a time; scripts (the
 * path that matters for conformance) use the plural form below. */
static bool shell_line_find_heredoc(const char *line, char *delim, size_t cap,
                                    bool *strip_tabs, bool *quoted) {
    struct shell_heredoc_spec sp[1];
    if (shell_line_find_heredocs(line, sp, 1) < 1) {
        delim[0] = '\0';
        *strip_tabs = false;
        *quoted = false;
        return false;
    }
    size_t n = strlen(sp[0].delim);
    if (n + 1 > cap) return false;
    memcpy(delim, sp[0].delim, n + 1);
    *strip_tabs = sp[0].strip_tabs;
    *quoted = sp[0].quoted;
    return true;
}

static bool shell_heredoc_line_matches(const char *line, const char *delim,
                                       bool strip_tabs) {
    if (strip_tabs) {
        while (*line == '\t') line++;
    }
    return strcmp(line, delim) == 0;
}

static int shell_collect_heredoc(char **pp, const char *delim,
                                 bool strip_tabs, bool quoted) {
    /* Heap, not stack: SHELL_HEREDOC_BODY_MAX is 64 KiB, and this function
     * inlines into the script reader, which gave that reader a 66 KiB frame
     * on a kernel stack -- one script with a here-document was already at the
     * edge, and `sh` nests eight deep. */
    char *body = (char *)kmalloc(SHELL_HEREDOC_BODY_MAX);
    if (!body) {
        kprintf("sh: out of memory reading here-document\n");
        return -1;
    }
    size_t bpos = 0;
    char *p = *pp;

    while (*p) {
        char *line_start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        char saved = *p;
        if (*p) *p++ = '\0';
        if (saved == '\r' && *p == '\n') p++;

        const char *match_line = line_start;
        if (shell_heredoc_line_matches(match_line, delim, strip_tabs)) {
            *pp = p;
            int prc = shell_heredoc_push(body, bpos);
            kfree(body);
            return prc;
        }

        /* <<- strips leading TABS from the body too, not just from the line
         * the delimiter is on. Only the delimiter match stripped them, so the
         * here-document ended in the right place and then emitted the tabs. */
        const char *raw = line_start;
        if (strip_tabs) while (*raw == '\t') raw++;

        const char *emit = raw;
        char expanded[LINE_MAX * 2];
        if (!quoted) {
            if (shell_expand_literal_quotes(raw, expanded,
                                            sizeof(expanded)) < 0) {
                kfree(body);
                return -1;
            }
            emit = expanded;
        }
        size_t n = strlen(emit);
        if (bpos + n + 2 > SHELL_HEREDOC_BODY_MAX) { kfree(body); return -1; }
        memcpy(body + bpos, emit, n);
        bpos += n;
        body[bpos++] = '\n';
        body[bpos] = '\0';
    }

    kprintf("sh: here-document delimited by EOF (wanted '%s')\n", delim);
    kfree(body);
    return -1;
}

/* ---- multi-line compound commands ---------------------------------------
 *
 * The compound parsers below (shell_try_if_command and friends) work on ONE
 * line: they search for the literal markers "; then", "; do", "; fi",
 * "; done". That is exactly right for an interactive shell, where the user
 * types `if [ -f x ]; then echo yes; fi` on a single line.
 *
 * Scripts do not look like that. They look like this:
 *
 *     if [ 1 -eq 1 ]
 *     then
 *         echo one
 *     fi
 *
 * ...which the line-at-a-time reader below used to hand to the parser one
 * fragment at a time, producing "if: expected '; fi'" and then trying to run
 * `then`, `echo one` and `fi` as three separate commands. Against real bash
 * that failed five of the fourteen parity cases -- more than any other single
 * gap -- because essentially every real script writes compounds across lines.
 *
 * So: count unterminated compounds, and keep pulling lines until the count
 * returns to zero, joining them into the single line the parser expects.
 *
 * KNOWN LIMITS, stated rather than hidden:
 *   - `{ }` groups and `( )` subshells are not counted, because `{` is also
 *     brace expansion and `${`, and miscounting those would break lines that
 *     work today. Multi-line groups still need a trailing `;` per line.
 *   - A here-document opened INSIDE a compound has its body swallowed by the
 *     accumulator; heredocs at the top level (the common case) are collected
 *     by the caller as before.
 */

/* Collapse `;` + run-of-blanks to exactly `; `, in place, outside quotes.
 *
 * The compound parsers match their separators LITERALLY -- "; then", "; do",
 * "; done" -- so `for w in $v;   do ... done`, which bash accepts without
 * comment, produced "for: expected '; do'". Rather than teach five separate
 * matchers about arbitrary whitespace, normalise once here: the transform
 * only ever shortens the string, so it is safe to do in place, and it leaves
 * quoted text untouched. */
static void shell_normalise_separators(char *s) {
    bool in_sq = false, in_dq = false;
    char *w = s;
    for (char *r = s; *r; r++) {
        if (in_sq) { *w++ = *r; if (*r == '\'') in_sq = false; continue; }
        if (in_dq) {
            if (*r == '\\' && r[1]) { *w++ = *r++; *w++ = *r; continue; }
            *w++ = *r;
            if (*r == '"') in_dq = false;
            continue;
        }
        if (*r == '\'') { in_sq = true; *w++ = *r; continue; }
        if (*r == '"')  { in_dq = true; *w++ = *r; continue; }
        *w++ = *r;
        if (*r == ';') {
            char *q = r + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (q > r + 1) { *w++ = ' '; r = q - 1; }
        }
    }
    *w = '\0';
}

/* Does `s` leave a logical line INCOMPLETE?
 *
 * Two ways it can: a quote opened and never closed, or a trailing backslash
 * (POSIX 2.2.1 line continuation -- backslash and newline are both removed and
 * the next line continues the same command). The script reader already
 * stitches lines together for compound commands and here-documents; these are
 * the two remaining cases where a "line" is not a whole line.
 *
 * Returns 1 for an open quote, 2 for a continuation (whose backslash the
 * caller must also drop), 0 when the line stands on its own. */
static int shell_line_incomplete(const char *s) {
    bool in_sq = false, in_dq = false, esc = false;
    for (const char *p = s; *p; p++) {
        esc = false;
        if (in_sq) {
            if (*p == '\'') in_sq = false;
            continue;
        }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (*p == '\\') {
            if (!p[1]) { esc = true; break; }   /* trailing backslash */
            p++;                                /* escapes the next character */
            continue;
        }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"')  { in_dq = true; continue; }
        if (*p == '#' && (p == s || is_space(p[-1]))) break;   /* comment */
    }
    if (in_sq || in_dq) return 1;
    return esc ? 2 : 0;
}

/* Unterminated-compound depth of `s`, starting from `depth`. Quote-aware, so
 * the `fi` in `echo "fi"` does not close anything. */
static int shell_compound_depth(const char *s, int depth) {
    bool in_sq = false, in_dq = false;
    const char *start = s;
    for (const char *q = s; *q; q++) {
        if (in_sq) { if (*q == '\'') in_sq = false; continue; }
        if (in_dq) {
            if (*q == '\\' && q[1]) { q++; continue; }
            if (*q == '"') in_dq = false;
            continue;
        }
        if (*q == '\'') { in_sq = true; continue; }
        if (*q == '"')  { in_dq = true;  continue; }
        /* A comment runs to end of line; keywords inside it are text. */
        if (*q == '#' && (q == start || is_space(q[-1]))) break;
        if (!shell_word_boundary_before(start, q)) continue;
        if (shell_starts_with_word(q, "if")    ||
            shell_starts_with_word(q, "for")   ||
            shell_starts_with_word(q, "while") ||
            shell_starts_with_word(q, "until") ||
            shell_starts_with_word(q, "case")) {
            depth++;
        } else if (shell_starts_with_word(q, "fi")   ||
                   shell_starts_with_word(q, "done") ||
                   shell_starts_with_word(q, "esac")) {
            depth--;
        } else if (shell_group_open_at(q)) {
            /* A `{ }` group spanning lines -- which is what a
             * multi-line FUNCTION BODY is. Without this, `f() {` was
             * executed on its own as a command and the body lines
             * ran as top-level commands, so the function was never
             * defined and calls to it saw no arguments. */
            depth++;
        } else if (shell_group_close_at(q)) {
            depth--;
        }
    }
    return depth < 0 ? 0 : depth;
}

/* Separator to place between an accumulated compound and its next line.
 *
 * The parsers want a literal ';' before `then`/`do`/`fi`/`done`, but must NOT
 * see one straight after them -- `; then; echo x` would run an empty command
 * where the body belongs. So a line ending in a keyword that expects a
 * following command joins with a space; everything else joins with "; ". */
static const char *shell_compound_join_sep(const char *buf, size_t len) {
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) len--;
    if (len == 0) return "";
    char c = buf[len - 1];
    if (c == ';' || c == '&' || c == '|') return " ";   /* covers `;;` too */
    if (c == '{') return " ";   /* `{` opens a group; `{;` is a syntax error */
    static const char *const kw[] = { "then", "do", "else", "in", 0 };
    for (int i = 0; kw[i]; i++) {
        size_t kl = strlen(kw[i]);
        if (len < kl || strncmp(buf + len - kl, kw[i], kl) != 0) continue;
        char before = (len == kl) ? ' ' : buf[len - kl - 1];
        if (!((before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
              (before >= '0' && before <= '9') || before == '_'))
            return " ";
    }
    return "; ";
}

static int shell_run_script_text(char *text, bool run_exit_trap) {
    if (!text) return 1;
    if (g_script_depth >= 8) {
        kprintf("sh: script nesting too deep\n");
        return 2;
    }

    g_script_depth++;
    unsigned long saved_lineno = g_shell_lineno;
    int saved_noexec = g_noexec_error;
    g_noexec_error = 0;
    g_shell_lineno = 0;
    int last = 0;
    char *p = text;
    bool first_line = true;
    while (*p) {
        g_shell_lineno++;
        char *line_start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        char saved = *p;
        if (*p) *p++ = '\0';
        if (saved == '\r' && *p == '\n') p++;

        char *line = line_start;
        while (*line == ' ' || *line == '\t') line++;
        if (first_line && line[0] == '#' && line[1] == '!') {
            first_line = false;
            continue;
        }
        first_line = false;
        if (*line == '\0' || *line == '#') continue;

        /* If this line opens a compound, pull following lines until it closes
         * and hand the parser the single line it expects. See the comment
         * above shell_compound_depth. The buffer is heap-allocated, not a
         * local: scripts nest up to 8 deep and each level already puts two
         * SHELL_PARSE_BUF_MAX buffers on the stack inside execute_line_text. */
        char *accum = 0;

        /* First, join physical lines into one LOGICAL line: a trailing
         * backslash continues, and so does an unclosed quote. Done before the
         * compound scan so that a `fi` inside an unterminated string cannot
         * be mistaken for the end of a construct. */
        {
            int inc = shell_line_incomplete(line);
            if (inc) {
                accum = kmalloc(SHELL_PARSE_BUF_MAX);
                if (!accum) {
                    kprintf("sh: out of memory reading continued line\n");
                    shell_set_status(2);
                    break;
                }
                size_t alen = (size_t)ksnprintf(accum, SHELL_PARSE_BUF_MAX, "%s", line);
                bool bad = false;
                while (inc && *p) {
                    /* A continuation drops the backslash and joins directly;
                     * an open quote keeps the newline, because it is part of
                     * the string being built. */
                    if (inc == 2 && alen > 0 && accum[alen - 1] == '\\') alen--;
                    else if (inc == 1 && alen + 1 < SHELL_PARSE_BUF_MAX) accum[alen++] = '\n';

                    char *nstart = p;
                    while (*p && *p != '\n' && *p != '\r') p++;
                    char nsaved = *p;
                    if (*p) *p++ = '\0';
                    if (nsaved == '\r' && *p == '\n') p++;

                    size_t nlen = strlen(nstart);
                    if (alen + nlen + 1 > SHELL_PARSE_BUF_MAX) {
                        kprintf("sh: line too long after continuation\n");
                        bad = true;
                        break;
                    }
                    memcpy(accum + alen, nstart, nlen);
                    alen += nlen;
                    accum[alen] = '\0';
                    inc = shell_line_incomplete(accum);
                }
                if (bad) {
                    kfree(accum);
                    shell_set_status(2);
                    last = 2;
                    break;
                }
                line = accum;
            }
        }

        int depth = shell_compound_depth(line, 0);
        if (depth > 0) {
            /* The continuation pass above may already own this buffer, with
             * `line` pointing into it; reuse it rather than allocating a
             * second one and leaking the first. */
            if (!accum) {
                accum = kmalloc(SHELL_PARSE_BUF_MAX);
                if (!accum) {
                    kprintf("sh: out of memory reading compound command\n");
                    shell_set_status(2);
                    break;
                }
            }
            size_t alen = (line == accum) ? strlen(accum)
                        : (size_t)ksnprintf(accum, SHELL_PARSE_BUF_MAX, "%s", line);
            bool overflow = false;
            while (depth > 0 && *p) {
                char *nstart = p;
                while (*p && *p != '\n' && *p != '\r') p++;
                char nsaved = *p;
                if (*p) *p++ = '\0';
                if (nsaved == '\r' && *p == '\n') p++;

                char *nl = nstart;
                while (*nl == ' ' || *nl == '\t') nl++;
                if (*nl == '\0' || *nl == '#') continue;

                const char *sep = shell_compound_join_sep(accum, alen);
                size_t seplen = strlen(sep), nllen = strlen(nl);
                if (alen + seplen + nllen + 1 > SHELL_PARSE_BUF_MAX) {
                    kprintf("sh: compound command too long (max %d bytes)\n",
                            SHELL_PARSE_BUF_MAX);
                    overflow = true;
                    break;
                }
                memcpy(accum + alen, sep, seplen); alen += seplen;
                memcpy(accum + alen, nl, nllen);   alen += nllen;
                accum[alen] = '\0';
                depth = shell_compound_depth(nl, depth);
            }
            if (overflow || depth > 0) {
                if (!overflow)
                    kprintf("sh: unexpected end of file (unterminated compound)\n");
                kfree(accum);
                shell_set_status(2);
                last = 2;
                break;
            }
            line = accum;
        }

        /* In place: `line` points either into the script text or into accum,
         * both writable, and the result is never longer than the input. */
        shell_normalise_separators(line);

        /* Every here-document the line opens, collected in the order the
         * operators appear -- which is the order their bodies follow, and
         * the order the redirection code pops them off the queue. */
        struct shell_heredoc_spec hd[SHELL_HEREDOC_MAX];
        int nhd = shell_line_find_heredocs(line, hd, SHELL_HEREDOC_MAX);
        shell_heredoc_reset();
        bool hd_failed = false;
        for (int hi = 0; hi < nhd; hi++) {
            if (shell_collect_heredoc(&p, hd[hi].delim, hd[hi].strip_tabs,
                                      hd[hi].quoted) < 0) {
                hd_failed = true;
                break;
            }
        }
        if (hd_failed) {
            last = 2;
            shell_set_status(2);
            if (accum) kfree(accum);
            break;
        }
        execute_line_text(line);
        if (accum) { kfree(accum); accum = 0; line = 0; }
        last = g_last_status;
        shell_heredoc_reset();
        if (g_shell_flow == SHELL_FLOW_EXIT) {
            last = g_shell_flow_status;
            break;
        }
        if (g_shell_flow == SHELL_FLOW_RETURN) {
            last = g_shell_flow_status;
            g_shell_flow = SHELL_FLOW_NONE;
            g_shell_flow_status = 0;
            break;
        }
        if (g_shell_flow != SHELL_FLOW_NONE) break;
    }
    if (run_exit_trap) last = shell_run_exit_trap(last);
    if (g_noexec_error) last = g_noexec_error;
    g_noexec_error = saved_noexec;
    g_script_depth--;
    g_shell_lineno = saved_lineno;
    return last;
}

static int shell_run_script_path(const char *path_arg, bool run_exit_trap) {
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(path_arg, path, sizeof(path), "sh") < 0) return 1;
    void *buf = 0;
    size_t sz = 0;
    int rc = vfs_read_all(path, &buf, &sz);
    (void)sz;
    if (rc != VFS_OK) {
        kprintf("sh: '%s': %s\n", path_arg, vfs_strerror(rc));
        return 1;
    }
    int st = shell_run_script_text((char *)buf, run_exit_trap);
    kfree(buf);
    return st;
}

struct shell_opt_frame {
    bool errexit, nounset, xtrace, noglob, verbose, noclobber, notify, noexec, allexport;
};

static void shell_save_opts(struct shell_opt_frame *f) {
    f->errexit   = g_opt_errexit;
    f->nounset   = g_opt_nounset;
    f->xtrace    = g_opt_xtrace;
    f->noglob    = g_opt_noglob;
    f->verbose   = g_opt_verbose;
    f->noclobber = g_opt_noclobber;
    f->notify    = g_opt_notify;
    f->noexec    = g_opt_noexec;
    f->allexport = g_opt_allexport;
}

static void shell_restore_opts(const struct shell_opt_frame *f) {
    g_opt_errexit   = f->errexit;
    g_opt_nounset   = f->nounset;
    g_opt_xtrace    = f->xtrace;
    g_opt_noglob    = f->noglob;
    g_opt_verbose   = f->verbose;
    g_opt_noclobber = f->noclobber;
    g_opt_notify    = f->notify;
    g_opt_noexec    = f->noexec;
    g_opt_allexport = f->allexport;
}

static void shell_reset_opts(void) {
    g_opt_errexit = g_opt_nounset = g_opt_xtrace = false;
    g_opt_noglob = g_opt_verbose = g_opt_noclobber = false;
    g_opt_notify = g_opt_noexec = g_opt_allexport = false;
}

static void cmd_sh(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) {
        kprintf("usage: sh [-c command] [script]\n");
        shell_set_status(1);
        return;
    }
    if (strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            kprintf("sh: -c requires a command string\n");
            shell_set_status(2);
            return;
        }
        struct shell_param_frame frame;
        const char *param0 = (argc >= 4) ? argv[3] : "sh";
        int pargc = (argc >= 5) ? argc - 4 : 0;
        char **pargv = (argc >= 5) ? &argv[4] : &argv[argc];
        if (shell_enter_script_params(&frame, param0, pargc, pargv) < 0) {
            kprintf("sh: failed to set positional parameters\n");
            shell_set_status(1);
            return;
        }
        struct shell_trap_frame trap_frame;
        if (shell_trap_enter_child(&trap_frame) < 0) {
            shell_restore_params_from_frame(&frame);
            shell_set_status(1);
            return;
        }
        struct shell_opt_frame opt_frame;
        shell_save_opts(&opt_frame);
        shell_reset_opts();
        char *copy = shell_strdup(argv[2]);
        if (!copy) {
            shell_restore_opts(&opt_frame);
            shell_trap_restore(&trap_frame);
            shell_restore_params_from_frame(&frame);
            shell_set_status(1);
            return;
        }
        int st = shell_run_script_text(copy, true);
        kfree(copy);
        shell_restore_opts(&opt_frame);
        shell_trap_restore(&trap_frame);
        if (g_shell_flow == SHELL_FLOW_EXIT) {
            st = g_shell_flow_status;
            g_shell_flow = SHELL_FLOW_NONE;
            g_shell_flow_status = 0;
        }
        shell_restore_params_from_frame(&frame);
        shell_set_status(st);
        return;
    }
    struct shell_param_frame frame;
    int pargc = argc - 2;
    char **pargv = (argc >= 3) ? &argv[2] : &argv[argc];
    if (shell_enter_script_params(&frame, argv[1], pargc, pargv) < 0) {
        kprintf("sh: failed to set positional parameters\n");
        shell_set_status(1);
        return;
    }
    struct shell_trap_frame trap_frame;
    if (shell_trap_enter_child(&trap_frame) < 0) {
        shell_restore_params_from_frame(&frame);
        shell_set_status(1);
        return;
    }
    struct shell_opt_frame opt_frame;
    shell_save_opts(&opt_frame);
    shell_reset_opts();
    int st = shell_run_script_path(argv[1], true);
    shell_restore_opts(&opt_frame);
    shell_trap_restore(&trap_frame);
    if (g_shell_flow == SHELL_FLOW_EXIT) {
        st = g_shell_flow_status;
        g_shell_flow = SHELL_FLOW_NONE;
        g_shell_flow_status = 0;
    }
    shell_restore_params_from_frame(&frame);
    shell_set_status(st);
}

static int shell_find_dot_script(const char *arg, char *out, size_t cap) {
    bool has_slash = false;
    for (const char *p = arg; *p; p++) {
        if (*p == '/') { has_slash = true; break; }
    }
    if (has_slash)
        return shell_resolve_path_arg(arg, out, cap, ".");

    if (shell_canonicalize_path(arg, out, cap) >= 0) {
        struct vfs_stat st;
        if (vfs_stat(out, &st) == VFS_OK) return 0;
    }
    const char *path_env = env_get("PATH");
    if (!path_env) path_env = "/bin:/usr/bin";
    const char *p = path_env;
    while (*p) {
        const char *end = p;
        while (*end && *end != ':') end++;
        size_t dlen = (size_t)(end - p);
        size_t alen = strlen(arg);
        if (dlen == 0) { p = *end ? end + 1 : end; continue; }
        if (dlen + 1 + alen + 1 > cap) { p = *end ? end + 1 : end; continue; }
        memcpy(out, p, dlen);
        out[dlen] = '/';
        memcpy(out + dlen + 1, arg, alen + 1);
        struct vfs_stat st;
        if (vfs_stat(out, &st) == VFS_OK) return 0;
        p = *end ? end + 1 : end;
    }
    kprintf(".: %s: not found\n", arg);
    shell_set_status(1);
    return -1;
}

static void cmd_dot(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) {
        kprintf("usage: . script\n");
        shell_set_status(2);
        return;
    }
    char resolved[VFS_PATH_MAX];
    if (shell_find_dot_script(argv[1], resolved, sizeof(resolved)) < 0) return;
    argv[1] = resolved;
    if (argc <= 2) {
        shell_set_status(shell_run_script_path(argv[1], false));
        return;
    }

    struct shell_param_frame frame;
    if (shell_enter_dot_params(&frame, argc - 2, &argv[2]) < 0) {
        kprintf(".: failed to set positional parameters\n");
        shell_set_status(1);
        return;
    }
    int st = shell_run_script_path(argv[1], false);
    shell_restore_params_from_frame(&frame);
    shell_set_status(st);
}

static void cmd_colon(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_set_status(0);
}

static void shell_print_alias(const struct shell_alias *a) {
    if (!a || !a->name || !a->value) return;
    shell_printf("alias %s='", a->name);
    for (const char *p = a->value; *p; p++) {
        if (*p == '\'') shell_printf("'\\''");
        else shell_putc(*p);
    }
    shell_printf("'\n");
}

static void cmd_alias(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
            if (g_aliases[i].name) shell_print_alias(&g_aliases[i]);
        }
        return;
    }

    for (int i = 1; i < argc; i++) {
        const char *eq = argv[i];
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            size_t nlen = (size_t)(eq - argv[i]);
            char name[64];
            if (nlen == 0 || nlen + 1 > sizeof(name)) {
                kprintf("alias: bad name '%s'\n", argv[i]);
                shell_set_status(1);
                continue;
            }
            memcpy(name, argv[i], nlen);
            name[nlen] = '\0';
            if (shell_alias_set(name, eq + 1) < 0) {
                kprintf("alias: failed to set '%s'\n", name);
                shell_set_status(1);
            }
            continue;
        }

        int idx = shell_alias_find(argv[i]);
        if (idx < 0) {
            kprintf("alias: %s not found\n", argv[i]);
            shell_set_status(1);
        } else {
            shell_print_alias(&g_aliases[idx]);
        }
    }
}

static void cmd_unalias(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        kprintf("usage: unalias [-a] NAME [NAME...]\n");
        shell_set_status(1);
        return;
    }
    if (strcmp(argv[1], "-a") == 0) {
        shell_alias_clear_all();
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (shell_alias_find(argv[i]) < 0) {
            kprintf("unalias: %s not found\n", argv[i]);
            shell_set_status(1);
        } else {
            shell_alias_unset(argv[i]);
        }
    }
}

static int shell_status_arg(int argc, char **argv, int def, const char *label,
                            bool *ok) {
    *ok = true;
    if (argc <= 1) return def;
    int v = 0;
    if (parse_int(argv[1], &v) < 0 || v < 0 || v > 255) {
        kprintf("%s: numeric argument required\n", label);
        *ok = false;
        return 2;
    }
    return v;
}

static int g_shell_break_depth;

static void cmd_break(int argc, char **argv) {
    if (argc > 2) {
        kprintf("break: too many arguments\n");
        shell_set_status(2);
        return;
    }
    if (g_shell_loop_depth <= 0) {
        kprintf("break: not in a loop\n");
        shell_set_status(1);
        return;
    }
    int n = 1;
    if (argc == 2 && (parse_int(argv[1], &n) < 0 || n <= 0)) {
        kprintf("break: %s: numeric argument required\n", argv[1]);
        shell_set_status(2);
        return;
    }
    if (n > g_shell_loop_depth) n = g_shell_loop_depth;
    g_shell_break_depth = n;
    g_shell_flow = SHELL_FLOW_BREAK;
    g_shell_flow_status = 0;
    shell_set_status(0);
}

static void cmd_continue(int argc, char **argv) {
    if (argc > 2) {
        kprintf("continue: too many arguments\n");
        shell_set_status(2);
        return;
    }
    if (g_shell_loop_depth <= 0) {
        kprintf("continue: not in a loop\n");
        shell_set_status(1);
        return;
    }
    int n = 1;
    if (argc == 2 && (parse_int(argv[1], &n) < 0 || n <= 0)) {
        kprintf("continue: %s: numeric argument required\n", argv[1]);
        shell_set_status(2);
        return;
    }
    if (n > g_shell_loop_depth) n = g_shell_loop_depth;
    g_shell_break_depth = n;
    g_shell_flow = SHELL_FLOW_CONTINUE;
    g_shell_flow_status = 0;
    shell_set_status(0);
}

static void cmd_return(int argc, char **argv) {
    if (argc > 2) {
        kprintf("return: too many arguments\n");
        shell_set_status(2);
        return;
    }
    if (g_script_depth <= 0) {
        kprintf("return: not in a script or function\n");
        shell_set_status(1);
        return;
    }
    bool ok = true;
    int st = shell_status_arg(argc, argv, g_last_status, "return", &ok);
    if (!ok) {
        shell_set_status(st);
        return;
    }
    g_shell_flow = SHELL_FLOW_RETURN;
    g_shell_flow_status = st;
    shell_set_status(st);
}

static void cmd_exit(int argc, char **argv) {
    if (argc > 2) {
        kprintf("exit: too many arguments\n");
        shell_set_status(2);
        return;
    }
    bool ok = true;
    int st = shell_status_arg(argc, argv, g_last_status, "exit", &ok);
    if (!ok) {
        shell_set_status(st);
        return;
    }
    shell_set_status(st);
    if (g_script_depth > 0 || g_subshell_depth > 0) {
        g_shell_flow = SHELL_FLOW_EXIT;
        g_shell_flow_status = st;
    }
}

static void cmd_times(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        kprintf("times: too many arguments\n");
        shell_set_status(2);
        return;
    }
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 1;
    uint64_t t = pit_ticks();
    uint64_t cs = t * 100 / hz;
    shell_printf("0m0.00s 0m%lu.%02lus\n",
                 (unsigned long)(cs / 100),
                 (unsigned long)(cs % 100));
    shell_printf("0m0.00s 0m0.00s\n");
    shell_set_status(0);
}

static int shell_trap_parse_condition(const char *s, int *out_sig) {
    if (!s || !*s || !out_sig) return -1;
    if (strcmp(s, "0") == 0 || strcmp(s, "EXIT") == 0) {
        *out_sig = 0;
        return 0;
    }
    int named = shell_signal_by_name(s);
    if (named > 0) {
        *out_sig = named;
        return 0;
    }
    int v = 0;
    if (parse_int(s, &v) == 0 && v >= 0 && v < SIG_MAX) {
        *out_sig = v;
        return 0;
    }
    return -1;
}

static void shell_print_quoted_trap_action(const char *action) {
    shell_putc('\'');
    for (const char *p = action ? action : ""; *p; p++) {
        if (*p == '\'') shell_printf("'\\''");
        else shell_putc(*p);
    }
    shell_putc('\'');
}

static void cmd_trap(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        for (int i = 0; i < SIG_MAX; i++) {
            if (!g_traps[i]) continue;
            const char *name = shell_trap_name(i);
            if (!name) continue;
            shell_printf("trap -- ");
            shell_print_quoted_trap_action(g_traps[i]);
            shell_printf(" %s\n", name);
        }
        return;
    }

    const char *action = argv[1];
    bool reset = (strcmp(action, "-") == 0);
    for (int i = 2; i < argc; i++) {
        int sig = -1;
        if (shell_trap_parse_condition(argv[i], &sig) < 0) {
            kprintf("trap: bad condition '%s'\n", argv[i]);
            shell_set_status(1);
            continue;
        }
        shell_trap_unset(sig);
        if (reset) continue;
        g_traps[sig] = shell_strdup(action);
        if (!g_traps[sig]) {
            shell_set_status(1);
            return;
        }
    }
}

static void cmd_eval(int argc, char **argv) {
    if (argc <= 1) {
        shell_set_status(0);
        return;
    }
    char cmd[LINE_MAX];
    size_t pos = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (pos + n + (i + 1 < argc ? 1 : 0) + 1 > sizeof(cmd)) {
            kprintf("eval: command too long\n");
            shell_set_status(2);
            return;
        }
        memcpy(cmd + pos, argv[i], n);
        pos += n;
        if (i + 1 < argc) cmd[pos++] = ' ';
    }
    cmd[pos] = '\0';
    execute_line_text(cmd);
}

static void cmd_exec(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) return;
    shell_spawn_program(argv[1], argc - 1, &argv[1], /*background=*/false);
    if (g_last_status == 0 && g_script_depth > 0) {
        g_shell_flow = SHELL_FLOW_EXIT;
        g_shell_flow_status = 0;
    }
}

static void cmd_command(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) return;

    bool verbose = false;
    bool locate = false;
    bool use_default_path = false;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (strcmp(argv[i], "-v") == 0) { locate = true; i++; }
        else if (strcmp(argv[i], "-V") == 0) { verbose = true; i++; }
        else if (strcmp(argv[i], "-p") == 0) { use_default_path = true; i++; }
        else break;
    }
    (void)use_default_path;
    if (i >= argc) {
        shell_set_status(0);
        return;
    }

    if (locate || verbose) {
        int rc = 0;
        for (; i < argc; i++) {
            bool found = false;
            int aidx = shell_alias_find(argv[i]);
            if (aidx >= 0) {
                if (verbose) {
                    shell_printf("%s is an alias for '%s'\n",
                                 argv[i], g_aliases[aidx].value);
                } else {
                    shell_print_alias(&g_aliases[aidx]);
                }
                found = true;
            }
            if (found) continue;
            struct shell_function *fn = shell_function_lookup(argv[i]);
            if (fn) {
                shell_printf(verbose ? "%s is a shell function\n" : "%s\n",
                             argv[i]);
                found = true;
            }
            if (found) continue;
            for (const struct cmd *c = cmds; c->name; c++) {
                if (strcmp(argv[i], c->name) == 0) {
                    if (verbose && shell_special_builtin_name(c->name)) {
                        shell_printf("%s is a special shell builtin\n",
                                     argv[i]);
                    } else {
                        shell_printf(verbose ? "%s is a shell builtin\n"
                                             : "%s\n",
                                     argv[i]);
                    }
                    found = true;
                    break;
                }
            }
            if (found) continue;
            char path_buf[64];
            const char *path = resolve_program(argv[i], path_buf, sizeof(path_buf));
            if (path_is_file(path)) {
                shell_printf(verbose ? "%s is %s\n" : "%s\n", argv[i], path);
            } else {
                rc = 1;
            }
        }
        shell_set_status(rc);
        return;
    }

    shell_spawn_program_profile(argv[i], argc - i, &argv[i],
                                /*background=*/false, /*profile=*/0);
}

static void cmd_type(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) {
        shell_printf("usage: type NAME [NAME...]\n");
        shell_set_status(1);
        return;
    }
    for (int i = 1; i < argc; i++) {
        bool found = false;
        int aidx = shell_alias_find(argv[i]);
        if (aidx >= 0) {
            shell_printf("%s is an alias for '%s'\n",
                         argv[i], g_aliases[aidx].value);
            found = true;
        }
        if (found) continue;

        struct shell_function *fn = shell_function_lookup(argv[i]);
        if (fn) {
            shell_printf("%s is a shell function\n", argv[i]);
            found = true;
        }
        if (found) continue;

        for (const struct cmd *c = cmds; c->name; c++) {
            if (strcmp(argv[i], c->name) == 0) {
                if (shell_special_builtin_name(c->name)) {
                    shell_printf("%s is a special shell builtin\n", argv[i]);
                } else {
                    shell_printf("%s is a shell builtin\n", argv[i]);
                }
                found = true;
                break;
            }
        }
        if (found) continue;

        char path_buf[64];
        const char *path = resolve_program(argv[i], path_buf, sizeof(path_buf));
        if (path_is_file(path)) {
            shell_printf("%s is %s\n", argv[i], path);
        } else {
            shell_printf("%s: not found\n", argv[i]);
            shell_set_status(1);
        }
    }
}

static void cmd_which(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) {
        shell_printf("usage: which NAME [NAME...]\n");
        shell_set_status(1);
        return;
    }
    for (int i = 1; i < argc; i++) {
        char path_buf[64];
        const char *path = resolve_program(argv[i], path_buf, sizeof(path_buf));
        if (path_is_file(path)) {
            shell_printf("%s\n", path);
        } else {
            shell_printf("%s not found\n", argv[i]);
            shell_set_status(1);
        }
    }
}

/* ---- POSIX `test` / `[` builtin --------------------------------- */

static bool test_is_int(const char *s, long *out) {
    if (!s || !*s) return false;
    long v = 0;
    bool neg = false;
    const char *p = s;
    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') p++;
    if (!*p) return false;
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
        p++;
    }
    if (out) *out = neg ? -v : v;
    return true;
}

static int test_eval(int argc, char **argv);

static int test_primary(int argc, char **argv, int *pos) {
    if (*pos >= argc) return 1;
    const char *a = argv[*pos];

    if (strcmp(a, "!") == 0) {
        (*pos)++;
        return test_primary(argc, argv, pos) == 0 ? 1 : 0;
    }
    if (strcmp(a, "(") == 0) {
        (*pos)++;
        int r = 1;
        int depth = 1;
        int start = *pos;
        while (*pos < argc) {
            if (strcmp(argv[*pos], "(") == 0) depth++;
            else if (strcmp(argv[*pos], ")") == 0) {
                depth--;
                if (depth == 0) break;
            }
            (*pos)++;
        }
        int sub_argc = *pos - start;
        if (*pos < argc) (*pos)++;
        if (sub_argc > 0) {
            char **sub_argv = &argv[start];
            r = test_eval(sub_argc, sub_argv);
        }
        return r;
    }

    if (strcmp(a, "-n") == 0) {
        (*pos)++;
        if (*pos >= argc) return 0;
        int r = argv[*pos][0] != '\0' ? 0 : 1;
        (*pos)++;
        return r;
    }
    if (strcmp(a, "-z") == 0) {
        (*pos)++;
        if (*pos >= argc) return 0;
        int r = argv[*pos][0] == '\0' ? 0 : 1;
        (*pos)++;
        return r;
    }

    if (strcmp(a, "-e") == 0 || strcmp(a, "-f") == 0 ||
        strcmp(a, "-d") == 0 || strcmp(a, "-r") == 0 ||
        strcmp(a, "-w") == 0 || strcmp(a, "-x") == 0 ||
        strcmp(a, "-s") == 0 || strcmp(a, "-L") == 0 ||
        strcmp(a, "-h") == 0 || strcmp(a, "-p") == 0 ||
        strcmp(a, "-c") == 0 || strcmp(a, "-b") == 0 ||
        strcmp(a, "-g") == 0 || strcmp(a, "-u") == 0 ||
        strcmp(a, "-k") == 0 || strcmp(a, "-S") == 0 ||
        strcmp(a, "-G") == 0 || strcmp(a, "-O") == 0 ||
        strcmp(a, "-t") == 0) {
        char op = a[1];
        (*pos)++;
        if (*pos >= argc) return 1;
        const char *operand = argv[*pos];
        (*pos)++;
        if (op == 't') {
            long fd = 0;
            if (!test_is_int(operand, &fd)) return 1;
            return (fd >= 0 && fd <= 2) ? 0 : 1;
        }
        char resolved[VFS_PATH_MAX];
        if (shell_canonicalize_path(operand, resolved, sizeof(resolved)) < 0)
            return 1;
        struct vfs_stat st;
        int rc = vfs_stat(resolved, &st);
        if (rc != VFS_OK) return 1;
        if (op == 'f') return st.type == VFS_TYPE_FILE ? 0 : 1;
        if (op == 'd') return st.type == VFS_TYPE_DIR ? 0 : 1;
        if (op == 's') return st.size > 0 ? 0 : 1;
        if (op == 'L' || op == 'h') return 1;
        if (op == 'p' || op == 'c' || op == 'b' || op == 'S') return 1;
        if (op == 'g' || op == 'u' || op == 'k') return 1;
        if (op == 'G' || op == 'O') return 0;
        return 0;
    }

    if (*pos + 2 < argc) {
        const char *op = argv[*pos + 1];
        const char *b = argv[*pos + 2];
        bool is_str_op = strcmp(op, "=") == 0 || strcmp(op, "==") == 0 ||
                         strcmp(op, "!=") == 0;
        bool is_int_op = strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
                         strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0 ||
                         strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0;

        if (is_str_op) {
            *pos += 3;
            if (strcmp(op, "!=") == 0)
                return strcmp(a, b) != 0 ? 0 : 1;
            return strcmp(a, b) == 0 ? 0 : 1;
        }
        if (is_int_op) {
            *pos += 3;
            long va = 0, vb = 0;
            if (!test_is_int(a, &va) || !test_is_int(b, &vb)) return 2;
            if (strcmp(op, "-eq") == 0) return va == vb ? 0 : 1;
            if (strcmp(op, "-ne") == 0) return va != vb ? 0 : 1;
            if (strcmp(op, "-lt") == 0) return va < vb ? 0 : 1;
            if (strcmp(op, "-le") == 0) return va <= vb ? 0 : 1;
            if (strcmp(op, "-gt") == 0) return va > vb ? 0 : 1;
            if (strcmp(op, "-ge") == 0) return va >= vb ? 0 : 1;
        }
    }

    (*pos)++;
    return a[0] != '\0' ? 0 : 1;
}

static int test_and(int argc, char **argv, int *pos) {
    int r = test_primary(argc, argv, pos);
    while (*pos < argc && strcmp(argv[*pos], "-a") == 0) {
        (*pos)++;
        int r2 = test_primary(argc, argv, pos);
        if (r == 0) r = r2;
    }
    return r;
}

static int test_eval(int argc, char **argv) {
    int pos = 0;
    int r = test_and(argc, argv, &pos);
    while (pos < argc && strcmp(argv[pos], "-o") == 0) {
        pos++;
        int r2 = test_and(argc, argv, &pos);
        if (r != 0) r = r2;
    }
    return r;
}

/* The operators POSIX lists, split by arity. Used only by the count-based
 * dispatch below; the expression parser keeps its own inline tests. */
static bool test_is_unary_op(const char *s) {
    static const char *const u[] = {
        "-n", "-z", "-e", "-f", "-d", "-r", "-w", "-x", "-s", "-L", "-h",
        "-p", "-c", "-b", "-g", "-u", "-k", "-S", "-G", "-O", "-t", 0
    };
    for (int i = 0; u[i]; i++) if (strcmp(s, u[i]) == 0) return true;
    return false;
}

static bool test_is_binary_op(const char *s) {
    static const char *const b[] = {
        "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
        "-nt", "-ot", "-ef", 0
    };
    for (int i = 0; b[i]; i++) if (strcmp(s, b[i]) == 0) return true;
    return false;
}

/* One operand: true iff the string is non-empty. This is the rule that makes
 * `[ = ]` and `[ ! ]` true -- they are STRINGS here, not operators. */
static int test_one(const char *a) { return a[0] != '\0' ? 0 : 1; }

static int test_two(char **a) {
    if (strcmp(a[0], "!") == 0) return test_one(a[1]) == 0 ? 1 : 0;
    if (test_is_unary_op(a[0])) { int pos = 0; return test_primary(2, a, &pos); }
    return -1;                       /* caller reports the syntax error */
}

static int test_three(char **a) {
    if (test_is_binary_op(a[1])) { int pos = 0; return test_primary(3, a, &pos); }
    /* bash accepts `x -a y` / `x -o y` here even though POSIX does not list
     * them among the 3-operand binary operators. The superset contract owes
     * bash's answer wherever POSIX leaves it unspecified. */
    if (strcmp(a[1], "-a") == 0)
        return (test_one(a[0]) == 0 && test_one(a[2]) == 0) ? 0 : 1;
    if (strcmp(a[1], "-o") == 0)
        return (test_one(a[0]) == 0 || test_one(a[2]) == 0) ? 0 : 1;
    if (strcmp(a[0], "!") == 0) {
        int r = test_two(a + 1);
        return r < 0 ? -1 : (r == 0 ? 1 : 0);
    }
    if (strcmp(a[0], "(") == 0 && strcmp(a[2], ")") == 0) return test_one(a[1]);
    return -1;
}

static void cmd_test(int argc, char **argv) {
    bool bracket = (argc > 0 && strcmp(argv[0], "[") == 0);
    int end = argc;
    if (bracket) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            kprintf("[: missing ']'\n");
            shell_set_status(2);
            return;
        }
        end = argc - 1;
    }

    int n = end - 1;              /* operand count, ']' already removed */
    char **a = &argv[1];
    int r = -1;

    /* POSIX XCU defines test for 0..4 operands by COUNT, not by grammar. */
    switch (n) {
    case 0:
        shell_set_status(1);
        return;
    case 1:
        shell_set_status(test_one(a[0]));
        return;
    case 2:
        r = test_two(a);
        break;
    case 3:
        r = test_three(a);
        break;
    case 4:
        if (strcmp(a[0], "!") == 0) {
            int t = test_three(a + 1);
            r = (t < 0) ? -1 : (t == 0 ? 1 : 0);
        } else if (strcmp(a[0], "(") == 0 && strcmp(a[3], ")") == 0) {
            r = test_two(a + 1);
        }
        break;
    default:
        /* 5 or more: POSIX says the result is unspecified and bash applies its
         * own parser, which is what -a / -o chains rely on. Keep it. */
        shell_set_status(test_eval(n, a));
        return;
    }

    if (r < 0) {
        kprintf("%s: syntax error near '%s'\n", bracket ? "[" : "test",
                n > 0 ? a[n - 1] : "");
        shell_set_status(2);
        return;
    }
    shell_set_status(r);
}

/* ---- POSIX `printf` builtin ------------------------------------- */

/* Set when the escape was not one printf knows. The caller emits the
 * backslash before the character, so `\\Z` survives as the two characters
 * bash prints rather than the one tsh used to print. */
static bool g_printf_escape_raw;

/* Set by `\c` inside %b: printf stops producing output entirely -- the rest of
 * the operand AND the rest of the format -- and still exits 0. */
static bool g_printf_stop;

/* True while expanding a %b operand, which has a different escape dialect from
 * the format string: an optional leading zero before the three octal digits,
 * \x, and \c. */
static bool g_printf_b_mode;

static int printf_parse_escape(const char **pp) {
    const char *p = *pp;
    g_printf_escape_raw = false;
    if (*p != '\\') return -1;
    p++;
    char c = *p;
    if (!c) { *pp = p; return '\\'; }

    /* Octal. In the FORMAT string it is up to three digits starting here, so
     * `\377` is one byte 0xFF and `\0377` is \037 then a literal 7. In %b a
     * leading zero may come first, which is what makes both `\141` and `\0141`
     * mean `a`. Only the `\0...` spelling used to be recognised, so every
     * three-digit escape printed its digits. */
    if (c >= '0' && c <= '7') {
        if (g_printf_b_mode && c == '0' && p[1] >= '0' && p[1] <= '7') p++;
        int v = 0;
        int i = 0;
        for (; i < 3 && *p >= '0' && *p <= '7'; i++, p++) v = v * 8 + (*p - '0');
        *pp = p;
        return v & 0xFF;
    }

    if (g_printf_b_mode && (c == 'x' || c == 'X') &&
        (((p[1] >= '0' && p[1] <= '9')) || ((p[1] | 0x20) >= 'a' &&
                                            (p[1] | 0x20) <= 'f'))) {
        p++;                                   /* past the x */
        int v = 0;
        for (int i = 0; i < 2; i++) {
            char h = *p;
            int d;
            if (h >= '0' && h <= '9')                     d = h - '0';
            else if ((h | 0x20) >= 'a' && (h | 0x20) <= 'f') d = (h | 0x20) - 'a' + 10;
            else break;
            v = v * 16 + d;
            p++;
        }
        *pp = p;
        return v & 0xFF;
    }

    if (g_printf_b_mode && c == 'c') {
        g_printf_stop = true;
        *pp = p + 1;
        return -1;                             /* nothing more is emitted */
    }

    p++;
    *pp = p;
    switch (c) {
    case 'a':  return '\a';
    case 'b':  return '\b';
    case 'f':  return '\f';
    case 'n':  return '\n';
    case 'r':  return '\r';
    case 't':  return '\t';
    case 'v':  return '\v';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"':  return '"';
    default:
        g_printf_escape_raw = true;
        return c;
    }
}

/* Set by printf_arg_int when an operand is not a valid number. printf must
 * still PRINT what it managed to parse (bash prints 3 for "3abc") and then
 * exit 1, so the failure cannot be signalled by the return value. */
static bool g_printf_bad_num;

static bool printf_is_blank(char c) { return c == ' ' || c == '\t'; }

/* The unsigned view of the last operand printf_arg_int parsed. %u wants the
 * two's complement of the magnitude, which is why
 * `printf '%u' -18446744073709551615` is 1 rather than an error. */
static unsigned long g_printf_last_u64;

/* The last operand did not fit a SIGNED 64-bit conversion. Only %d and %i care;
 * %u happily prints values above 2^63. */
static bool g_printf_signed_over;

static long printf_arg_int(int argc, char **argv, int *argi) {
    g_printf_last_u64 = 0;
    if (*argi >= argc) return 0;
    const char *s = argv[(*argi)++];

    /* POSIX XCU: a leading ' or " means "the numeric value of the next
     * character". Extra characters after it are ignored, not an error. */
    if (*s == '\'' || *s == '"') {
        g_printf_last_u64 = (unsigned long)(unsigned char)s[1];
        return (long)(unsigned char)s[1];
    }

    while (printf_is_blank(*s)) s++;            /* LEADING blanks are fine */

    unsigned long mag = 0;
    bool neg = false, any = false, over = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;

    /* C literal syntax, which is what printf's numeric operands use: `0x55` is
     * 85 and `055` is 45. Reading decimal only turned every hex operand into 0
     * and left every octal one off by the value of its leading zero. The sign
     * comes FIRST, so `+077` is 63. */
    unsigned long base = 10ul;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        ((s[2] >= '0' && s[2] <= '9') || (s[2] >= 'a' && s[2] <= 'f') ||
         (s[2] >= 'A' && s[2] <= 'F'))) {
        base = 16ul;
        s += 2;
    } else if (s[0] == '0' && s[1] >= '0' && s[1] <= '7') {
        base = 8ul;
        s += 1;
    }

    for (;;) {
        unsigned long d;
        char c = *s;
        if (c >= '0' && c <= '9')                 d = (unsigned long)(c - '0');
        else if (base == 16ul && c >= 'a' && c <= 'f') d = (unsigned long)(c - 'a' + 10);
        else if (base == 16ul && c >= 'A' && c <= 'F') d = (unsigned long)(c - 'A' + 10);
        else break;
        if (d >= base) break;                      /* `09` is not octal 9 */
        if (mag > (~0ul - d) / base) over = true;  /* would wrap */
        else mag = mag * base + d;
        any = true;
        s++;
    }

    /* NO trailing-blank skip. bash accepts ` -123` and rejects ` -123 `, and
     * skipping them here reported success for both. */
    if (*s != '\0' || !any) g_printf_bad_num = true;

    if (over) {
        /* Past 64 bits: saturate to the unsigned maximum for EITHER sign --
         * `printf '%u' -18446744073709551616` is 18446744073709551615, not the
         * two's complement of a saturated magnitude (which would be 1). */
        g_printf_bad_num = true;
        g_printf_last_u64 = ~0ul;
        return neg ? (long)(1ul << 63) : (long)(~0ul >> 1);
    }

    g_printf_last_u64 = neg ? (unsigned long)(0ul - mag) : mag;

    /* Out of range for a SIGNED conversion is not out of range for %u:
     * 18446744073709551615 is a perfectly good %u operand and a saturating %d
     * one. Flagging it here made %u report failure for a value it had just
     * printed correctly, so the verdict is recorded and left for the
     * conversion to act on. */
    const unsigned long SMAX = ~0ul >> 1;            /* 2^63 - 1 */
    if (!neg && mag > SMAX) { g_printf_signed_over = true; return (long)SMAX; }
    if (neg && mag > SMAX + 1ul) { g_printf_signed_over = true; return (long)(SMAX + 1ul); }
    return neg ? -(long)mag : (long)mag;
}

/* printf's %f/%e/%g without floating point.
 *
 * The kernel is built -mno-sse, so there is no `double` to convert to. There
 * does not need to be: printf's argument arrives as decimal TEXT, so the
 * significant digits can be carried straight through as digits and rounded in
 * base 10. That is both simpler than a soft-float path and exact for the
 * values a shell script actually prints.
 *
 * Representation: value = 0.d[0]d[1]... * 10^exp, sign held separately. */
#define PF_DIG_MAX 40

struct printf_dec {
    bool neg;
    bool zero;
    char dig[PF_DIG_MAX];
    int  ndig;
    int  exp;
};

static void printf_dec_parse(const char *s, struct printf_dec *d) {
    memset(d, 0, sizeof(*d));
    d->zero = true;
    if (!s) return;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { d->neg = true; s++; }
    else if (*s == '+') s++;

    int pointexp = 0;      /* digits seen before the '.' */
    bool seen_point = false;
    bool leading = true;
    for (; *s; s++) {
        if (*s == '.' && !seen_point) { seen_point = true; continue; }
        if (*s < '0' || *s > '9') break;
        if (!seen_point) pointexp++;
        if (leading && *s == '0') {
            /* A leading zero is not a significant digit: undo the increment
             * above for one before the point, and push the exponent down for
             * one after it. Both cases are the same decrement. */
            pointexp--;
            continue;
        }
        leading = false;
        d->zero = false;
        if (d->ndig < PF_DIG_MAX) d->dig[d->ndig++] = *s;
    }
    d->exp = pointexp;

    if (*s == 'e' || *s == 'E') {
        s++;
        bool eneg = false;
        if (*s == '-') { eneg = true; s++; }
        else if (*s == '+') s++;
        int ev = 0;
        while (*s >= '0' && *s <= '9' && ev < 100000) {
            ev = ev * 10 + (*s - '0');
            s++;
        }
        d->exp += eneg ? -ev : ev;
    }
    if (d->zero) { d->ndig = 0; d->exp = 0; }
}

/* Round to `keep` significant digits, half away from zero. `keep` may be <= 0,
 * meaning the value rounds down to nothing or up to a single 1 a decade
 * higher. */
static void printf_dec_round(struct printf_dec *d, int keep) {
    if (d->zero) return;
    if (keep >= d->ndig) return;
    if (keep < 0) {
        d->zero = true;
        d->ndig = 0;
        d->exp = 0;
        return;
    }
    bool round_up = d->dig[keep] >= '5';
    d->ndig = keep;
    if (!round_up) {
        while (d->ndig > 0 && d->dig[d->ndig - 1] == '0') d->ndig--;
        if (d->ndig == 0) { d->zero = true; d->exp = 0; }
        return;
    }
    int i = keep - 1;
    while (i >= 0) {
        if (d->dig[i] != '9') { d->dig[i]++; break; }
        d->dig[i] = '0';
        i--;
    }
    if (i < 0) {
        /* Carried out of the top: 999 -> 1000, one decade higher. */
        d->dig[0] = '1';
        d->ndig = 1;
        d->exp++;
        d->zero = false;
        return;
    }
    while (d->ndig > 0 && d->dig[d->ndig - 1] == '0') d->ndig--;
}

static char printf_dec_at(const struct printf_dec *d, int i) {
    if (i < 0 || i >= d->ndig) return '0';
    return d->dig[i];
}

static void printf_buf_put(char *buf, size_t cap, size_t *n, char c) {
    if (*n + 1 < cap) buf[(*n)++] = c;
}

/* Render `d` in %f style with `prec` fraction digits (already rounded). */
static void printf_dec_fixed(const struct printf_dec *d, int prec,
                             char *buf, size_t cap, size_t *n) {
    if (d->exp <= 0) {
        printf_buf_put(buf, cap, n, '0');
    } else {
        for (int i = 0; i < d->exp; i++)
            printf_buf_put(buf, cap, n, printf_dec_at(d, i));
    }
    if (prec > 0) {
        printf_buf_put(buf, cap, n, '.');
        for (int j = 1; j <= prec; j++)
            printf_buf_put(buf, cap, n, printf_dec_at(d, d->exp + j - 1));
    }
}

/* Render `d` in %e style with `prec` fraction digits (already rounded). */
static void printf_dec_sci(const struct printf_dec *d, int prec, char espec,
                           char *buf, size_t cap, size_t *n) {
    printf_buf_put(buf, cap, n, d->zero ? '0' : printf_dec_at(d, 0));
    if (prec > 0) {
        printf_buf_put(buf, cap, n, '.');
        for (int j = 1; j <= prec; j++)
            printf_buf_put(buf, cap, n, printf_dec_at(d, j));
    }
    printf_buf_put(buf, cap, n, espec);
    int e = d->zero ? 0 : d->exp - 1;
    printf_buf_put(buf, cap, n, e < 0 ? '-' : '+');
    if (e < 0) e = -e;
    if (e >= 100) {
        printf_buf_put(buf, cap, n, (char)('0' + e / 100));
        printf_buf_put(buf, cap, n, (char)('0' + (e / 100) % 10));
    }
    printf_buf_put(buf, cap, n, (char)('0' + (e / 10) % 10));
    printf_buf_put(buf, cap, n, (char)('0' + e % 10));
}

static void printf_strip_trailing_zeros(char *buf, size_t *n) {
    size_t dot = 0;
    bool has_dot = false;
    for (size_t i = 0; i < *n; i++) {
        if (buf[i] == '.') { dot = i; has_dot = true; }
        if (buf[i] == 'e' || buf[i] == 'E') return;   /* handled by caller */
    }
    if (!has_dot) return;
    while (*n > dot + 1 && buf[*n - 1] == '0') (*n)--;
    if (*n == dot + 1) (*n)--;
}

/* Format one floating conversion into `buf`; returns its length. */
static size_t printf_format_float(const char *arg, char spec, int prec,
                                  bool alt, char *buf, size_t cap) {
    struct printf_dec d;
    printf_dec_parse(arg, &d);
    size_t n = 0;

    if (spec == 'f' || spec == 'F') {
        if (prec < 0) prec = 6;
        printf_dec_round(&d, d.exp + prec);
        printf_dec_fixed(&d, prec, buf, cap, &n);
        if (prec == 0 && alt) printf_buf_put(buf, cap, &n, '.');
    } else if (spec == 'e' || spec == 'E') {
        if (prec < 0) prec = 6;
        printf_dec_round(&d, prec + 1);
        printf_dec_sci(&d, prec, spec == 'E' ? 'E' : 'e', buf, cap, &n);
        if (prec == 0 && alt) printf_buf_put(buf, cap, &n, '.');
    } else {                                            /* g / G */
        int P = (prec < 0) ? 6 : (prec == 0 ? 1 : prec);
        printf_dec_round(&d, P);
        int X = d.zero ? 0 : d.exp - 1;
        if (X < -4 || X >= P) {
            printf_dec_sci(&d, P - 1, spec == 'G' ? 'E' : 'e', buf, cap, &n);
            if (!alt) {
                /* Strip trailing zeros in the mantissa only. */
                size_t epos = n;
                for (size_t i = 0; i < n; i++)
                    if (buf[i] == 'e' || buf[i] == 'E') { epos = i; break; }
                size_t mant = epos;
                bool has_dot = false;
                size_t dot = 0;
                for (size_t i = 0; i < mant; i++)
                    if (buf[i] == '.') { dot = i; has_dot = true; }
                if (has_dot) {
                    size_t end = mant;
                    while (end > dot + 1 && buf[end - 1] == '0') end--;
                    if (end == dot + 1) end--;
                    size_t tail = n - epos;
                    for (size_t i = 0; i < tail; i++) buf[end + i] = buf[epos + i];
                    n = end + tail;
                }
            }
        } else {
            printf_dec_fixed(&d, P - 1 - X, buf, cap, &n);
            if (!alt) printf_strip_trailing_zeros(buf, &n);
        }
    }
    buf[n < cap ? n : cap - 1] = '\0';
    return n;
}

static const char *printf_arg_str(int argc, char **argv, int *argi) {
    if (*argi >= argc) return "";
    return argv[(*argi)++];
}

/* Sink for `printf -v`: collects what would have gone to stdout. */
struct printf_capture { char buf[4096]; size_t len; };

static void printf_capture_sink(const char *s, void *ctx) {
    struct printf_capture *c = (struct printf_capture *)ctx;
    if (!c || !s) return;
    while (*s && c->len + 1 < sizeof c->buf) c->buf[c->len++] = *s++;
    c->buf[c->len] = '\0';
}

static void cmd_printf(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) {
        kprintf("usage: printf FORMAT [ARG...]\n");
        shell_set_status(2);      /* usage error, not a runtime failure */
        return;
    }

    /* `printf -v NAME FORMAT [ARG...]` assigns instead of printing. */
    const char *vname = 0;
    int base = 1;
    if (argc >= 4 && strcmp(argv[1], "-v") == 0) {
        vname = argv[2];
        base = 3;
    } else if (argc >= 2 && strcmp(argv[1], "--") == 0) {
        base = 2;
    }
    if (base >= argc) {
        kprintf("usage: printf [-v NAME] FORMAT [ARG...]\n");
        shell_set_status(2);
        return;
    }

    struct printf_capture cap;
    shell_write_fn_t saved_out = g_shell_out;
    void *saved_ctx = g_shell_out_ctx;
    if (vname) {
        cap.len = 0;
        cap.buf[0] = '\0';
        g_shell_out = printf_capture_sink;
        g_shell_out_ctx = &cap;
    }

    g_printf_bad_num = false;
    /* Per CALL, not per process: one `\c` must not silence every later
     * printf in the script. */
    g_printf_stop = false;
    g_printf_b_mode = false;

    const char *fmt = argv[base];
    int argi = base + 1;

    do {
        /* Where this pass started. A pass that consumes NO argument cannot be
         * followed by a useful one -- the format has no conversions to absorb
         * what is left -- and repeating it is the infinite loop this used to
         * be. See the note at the top of this change. */
        int argi_at_pass_start = argi;
        const char *p = fmt;
        while (*p) {
            if (*p == '\\') {
                int c = printf_parse_escape(&p);
                if (c >= 0) {
                    if (g_printf_escape_raw) shell_putc('\\');
                    shell_putc((char)c);
                }
                continue;
            }
            if (*p != '%') {
                shell_putc(*p++);
                continue;
            }
            p++;
            if (*p == '%') {
                shell_putc('%');
                p++;
                continue;
            }

            bool left = false;
            bool zero_pad = false;
            bool plus = false;
            bool space = false;
            bool alt = false;
            for (;;) {
                if (*p == '-')      { left = true; p++; }
                else if (*p == '0') { zero_pad = true; p++; }
                else if (*p == '+') { plus = true; p++; }
                else if (*p == ' ') { space = true; p++; }
                else if (*p == '#') { alt = true; p++; }
                else break;
            }

            int width = 0;
            if (*p == '*') {
                width = (int)printf_arg_int(argc, argv, &argi);
                if (width < 0) { left = true; width = -width; }
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    width = width * 10 + (*p - '0');
                    p++;
                }
            }

            int prec = -1;
            if (*p == '.') {
                p++;
                if (*p == '*') {
                    prec = (int)printf_arg_int(argc, argv, &argi);
                    if (prec < 0) prec = -1;
                    p++;
                } else {
                    prec = 0;
                    while (*p >= '0' && *p <= '9') {
                        prec = prec * 10 + (*p - '0');
                        p++;
                    }
                }
            }

            /* Length modifiers are accepted and ignored: every integer here is
             * parsed out of a string into a long already. */
            while (*p == 'l' || *p == 'h' || *p == 'j' || *p == 'z' ||
                   *p == 't' || *p == 'L') {
                p++;
            }

            char spec = *p;
            if (spec) p++;

            if (spec == 's') {
                const char *s = printf_arg_str(argc, argv, &argi);
                int slen = (int)strlen(s);
                if (prec >= 0 && slen > prec) slen = prec;
                int pad = width > slen ? width - slen : 0;
                if (!left) for (int i = 0; i < pad; i++) shell_putc(' ');
                for (int i = 0; i < slen; i++) shell_putc(s[i]);
                if (left) for (int i = 0; i < pad; i++) shell_putc(' ');
            } else if (spec == 'u') {
                /* Two's complement, not "absolute value without a sign":
                 * `printf '%u' -42` is 18446744073709551574 everywhere. Read
                 * the UNSIGNED view -- the signed return has been saturated
                 * for %d's benefit and would print the wrong thing here. */
                (void)printf_arg_int(argc, argv, &argi);
                unsigned long uv = g_printf_last_u64;
                char buf[32];
                int blen = 0;
                if (uv == 0) buf[blen++] = '0';
                while (uv > 0 && blen < (int)sizeof(buf) - 1) {
                    buf[blen++] = (char)('0' + (int)(uv % 10ul));
                    uv /= 10ul;
                }
                int pad = width > blen ? width - blen : 0;
                if (!left && !zero_pad) for (int k = 0; k < pad; k++) shell_putc(' ');
                if (!left && zero_pad)  for (int k = 0; k < pad; k++) shell_putc('0');
                while (blen > 0) shell_putc(buf[--blen]);
                if (left) for (int k = 0; k < pad; k++) shell_putc(' ');
            } else if (spec == 'd' || spec == 'i') {
                g_printf_signed_over = false;
                long v = printf_arg_int(argc, argv, &argi);
                /* A signed conversion DOES fail on a value it had to saturate. */
                if (g_printf_signed_over) g_printf_bad_num = true;
                char buf[32];
                int blen = 0;
                bool neg = (spec != 'u') && v < 0;
                unsigned long uv = (v < 0) ? (unsigned long)(-v)
                                           : (unsigned long)v;
                if (uv == 0) buf[blen++] = '0';
                while (uv > 0 && blen < (int)sizeof(buf) - 1) {
                    buf[blen++] = (char)('0' + (uv % 10));
                    uv /= 10;
                }
                char sign = neg ? '-' : (plus ? '+' : (space ? ' ' : '\0'));
                if (spec == 'u') sign = '\0';
                int numlen = blen + (sign ? 1 : 0);
                /* An explicit precision is a minimum digit count and it
                 * cancels zero padding. */
                int zeros = (prec > blen) ? prec - blen : 0;
                if (prec >= 0) zero_pad = false;
                numlen += zeros;
                int pad = width > numlen ? width - numlen : 0;
                if (!left && !zero_pad) for (int i = 0; i < pad; i++) shell_putc(' ');
                if (sign) shell_putc(sign);
                if (!left && zero_pad) for (int i = 0; i < pad; i++) shell_putc('0');
                for (int i = 0; i < zeros; i++) shell_putc('0');
                while (blen > 0) shell_putc(buf[--blen]);
                if (left) for (int i = 0; i < pad; i++) shell_putc(' ');
            } else if (spec == 'f' || spec == 'F' || spec == 'e' ||
                       spec == 'E' || spec == 'g' || spec == 'G') {
                const char *s = printf_arg_str(argc, argv, &argi);
                char buf[PF_DIG_MAX * 2 + 32];
                size_t blen = printf_format_float(s, spec, prec, alt,
                                                  buf, sizeof(buf));
                bool neg = false;
                for (const char *q = s; *q; q++) {
                    if (*q == ' ' || *q == '\t') continue;
                    neg = (*q == '-');
                    break;
                }
                char sign = neg ? '-' : (plus ? '+' : (space ? ' ' : '\0'));
                int numlen = (int)blen + (sign ? 1 : 0);
                int pad = width > numlen ? width - numlen : 0;
                if (!left && !zero_pad) for (int i = 0; i < pad; i++) shell_putc(' ');
                if (sign) shell_putc(sign);
                if (!left && zero_pad) for (int i = 0; i < pad; i++) shell_putc('0');
                for (size_t i = 0; i < blen; i++) shell_putc(buf[i]);
                if (left) for (int i = 0; i < pad; i++) shell_putc(' ');
            } else if (spec == 'b') {
                const char *s = printf_arg_str(argc, argv, &argi);
                const char *bp = s;
                g_printf_b_mode = true;
                while (*bp) {
                    if (*bp == '\\') {
                        int c = printf_parse_escape(&bp);
                        if (g_printf_stop) break;      /* \c: emit nothing more */
                        if (c >= 0) {
                            if (g_printf_escape_raw) shell_putc('\\');
                            shell_putc((char)c);
                        }
                    } else {
                        shell_putc(*bp++);
                    }
                }
                g_printf_b_mode = false;
                if (g_printf_stop) break;              /* and abandon the format */
            } else if (spec == 'c') {
                const char *s = printf_arg_str(argc, argv, &argi);
                int pad = width > 1 ? width - 1 : 0;
                if (!left) for (int i = 0; i < pad; i++) shell_putc(' ');
                if (s[0]) shell_putc(s[0]);
                if (left) for (int i = 0; i < pad; i++) shell_putc(' ');
            } else if (spec == 'o' || spec == 'x' || spec == 'X') {
                long v = printf_arg_int(argc, argv, &argi);
                unsigned long uv = (unsigned long)v;
                const char *hex = (spec == 'X') ? "0123456789ABCDEF"
                                                : "0123456789abcdef";
                int base = (spec == 'o') ? 8 : 16;
                char buf[32];
                int blen = 0;
                if (uv == 0) buf[blen++] = '0';
                while (uv > 0 && blen < (int)sizeof(buf) - 1) {
                    buf[blen++] = hex[uv % (unsigned)base];
                    uv /= (unsigned)base;
                }
                char pre[3];
                int prelen = 0;
                if (alt && v != 0) {
                    if (spec == 'o') {
                        pre[prelen++] = '0';
                    } else {
                        pre[prelen++] = '0';
                        pre[prelen++] = (spec == 'X') ? 'X' : 'x';
                    }
                }
                int zeros = (prec > blen) ? prec - blen : 0;
                if (prec >= 0) zero_pad = false;
                int numlen = blen + zeros + prelen;
                int pad = width > numlen ? width - numlen : 0;
                if (!left && !zero_pad) for (int i = 0; i < pad; i++) shell_putc(' ');
                for (int i = 0; i < prelen; i++) shell_putc(pre[i]);
                if (!left && zero_pad) for (int i = 0; i < pad; i++) shell_putc('0');
                for (int i = 0; i < zeros; i++) shell_putc('0');
                while (blen > 0) shell_putc(buf[--blen]);
                if (left) for (int i = 0; i < pad; i++) shell_putc(' ');
            } else {
                shell_putc('%');
                if (spec) shell_putc(spec);
            }
        }
        if (argi == argi_at_pass_start) break;   /* consumed nothing: stop */
    } while (argi < argc);

    /* An unparsable numeric operand is a runtime error: the output already
     * produced stands, and the status is 1. Reported AFTER the loop so one
     * bad operand does not suppress the rest of the output. */
    if (g_printf_bad_num) shell_set_status(1);

    if (vname) {
        g_shell_out = saved_out;
        g_shell_out_ctx = saved_ctx;
        if (!shell_name_is_valid(vname, strlen(vname))) {
            kprintf("printf: `%s': not a valid identifier\n", vname);
            shell_set_status(2);     /* bash: a bad -v name is a usage error */
            return;
        }
        if (env_set(vname, cap.buf) < 0) shell_set_status(1);
    }
}

static int g_shell_umask = 022;

static void cmd_umask(int argc, char **argv) {
    shell_set_status(0);
    if (argc <= 1) {
        shell_printf("%04o\n", (unsigned)g_shell_umask);
        return;
    }
    int val = 0;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '7') {
        val = val * 8 + (*s - '0');
        s++;
    }
    if (*s || val > 0777) {
        kprintf("umask: '%s': invalid octal mask\n", argv[1]);
        shell_set_status(1);
        return;
    }
    g_shell_umask = val;
}

static void cmd_hash(int argc, char **argv) {
    shell_set_status(0);
    if (argc >= 2 && strcmp(argv[1], "-r") == 0) {
        shell_printf("hash: table cleared\n");
        return;
    }
    if (argc <= 1) {
        shell_printf("hash: table is empty\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        char path_buf[64];
        const char *path = resolve_program(argv[i], path_buf, sizeof(path_buf));
        if (path_is_file(path)) {
            shell_printf("%s=%s\n", argv[i], path);
        } else {
            kprintf("hash: %s: not found\n", argv[i]);
            shell_set_status(1);
        }
    }
}

static void cmd_kill(int argc, char **argv) {
    shell_set_status(0);
    if (argc < 2) {
        kprintf("usage: kill [-SIGNAL] PID...\n");
        shell_set_status(2);
        return;
    }
    int sig = SIGTERM;
    int first = 1;
    if (strcmp(argv[1], "-l") == 0) {
        if (argc == 2) {
            size_t n = sizeof(g_shell_signals) / sizeof(g_shell_signals[0]);
            for (size_t i = 0; i < n; i++) {
                kprintf("%2d) SIG%-7s%s", g_shell_signals[i].num,
                        g_shell_signals[i].name, (i % 4 == 3) ? "\n" : " ");
            }
            if (n % 4 != 0) kprintf("\n");
            return;
        }
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            bool numeric = true;
            int v = 0;
            for (const char *q = a; *q; q++) {
                if (*q < '0' || *q > '9') { numeric = false; break; }
                v = v * 10 + (*q - '0');
            }
            if (numeric) {
                /* A status from `wait` encodes the signal in the low bits. */
                if (v > 128) v -= 128;
                const char *nm = shell_signal_name(v);
                if (nm) kprintf("%s\n", nm);
                else { kprintf("kill: %s: invalid signal number\n", a);
                       shell_set_status(1); }
            } else {
                int num = shell_signal_by_name(a);
                if (num > 0) kprintf("%d\n", num);
                else { kprintf("kill: %s: invalid signal name\n", a);
                       shell_set_status(1); }
            }
        }
        return;
    }
    if (strcmp(argv[1], "-s") == 0 && argc > 2) {
        int num = shell_signal_by_name(argv[2]);
        if (num < 0) {
            kprintf("kill: unknown signal '%s'\n", argv[2]);
            shell_set_status(1);
            return;
        }
        sig = num;
        first = 3;
    } else if (argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        int v = 0;
        for (const char *p = argv[1] + 1; *p; p++) {
            if (*p < '0' || *p > '9') {
                kprintf("kill: bad signal '%s'\n", argv[1] + 1);
                shell_set_status(2);
                return;
            }
            v = v * 10 + (*p - '0');
        }
        sig = v;
        first = 2;
    } else if (argv[1][0] == '-' && argv[1][1] != '\0') {
        int num = shell_signal_by_name(argv[1] + 1);
        if (num < 0) {
            kprintf("kill: unknown signal '%s'\n", argv[1] + 1);
            shell_set_status(1);
            return;
        }
        sig = num;
        first = 2;
    }
    for (int i = first; i < argc; i++) {
        int pid = 0;
        const char *p = argv[i];
        bool neg = false;
        if (*p == '-') { neg = true; p++; }
        while (*p >= '0' && *p <= '9') {
            pid = pid * 10 + (*p - '0');
            p++;
        }
        if (*p) {
            kprintf("kill: invalid pid '%s'\n", argv[i]);
            shell_set_status(1);
            continue;
        }
        if (neg) pid = -pid;
        signal_send_to_pid(pid, sig);
    }
}

static bool shell_word_has(const char *word, char c) {
    for (; *word; word++) {
        if (*word == c) return true;
    }
    return false;
}

/* POSIX `ulimit [-HS] [-acdfnstv] [value]`.
 *
 * The kernel does not police these yet, so nothing here constrains a running
 * process. What the shell CAN do correctly is be a faithful store: report a
 * value that was set, keep the soft/hard pair distinct, and enforce the two
 * rules that make the interface meaningful -- a soft limit may not exceed its
 * hard limit, and a hard limit may only ever be lowered. A script that reads
 * back what it set gets the right answer; one that tries to raise a hard
 * limit is refused, exactly as it would be on a system that did enforce them.
 */
#define ULIMIT_INF (-1L)

struct shell_rlimit {
    char  opt;
    const char *label;
    const char *units;
    long  soft;
    long  hard;
};

static struct shell_rlimit g_shell_rlimits[] = {
    { 'c', "core file size",  "(blocks)", ULIMIT_INF, ULIMIT_INF },
    { 'd', "data seg size",   "(kbytes)", ULIMIT_INF, ULIMIT_INF },
    { 'f', "file size",       "(blocks)", ULIMIT_INF, ULIMIT_INF },
    { 'n', "open files",      "",         ULIMIT_INF, ULIMIT_INF },
    { 's', "stack size",      "(kbytes)", ULIMIT_INF, ULIMIT_INF },
    { 't', "cpu time",        "(seconds)",ULIMIT_INF, ULIMIT_INF },
    { 'v', "virtual memory",  "(kbytes)", ULIMIT_INF, ULIMIT_INF },
};

static struct shell_rlimit *shell_rlimit_find(char opt) {
    for (size_t i = 0; i < sizeof(g_shell_rlimits) / sizeof(g_shell_rlimits[0]);
         i++) {
        if (g_shell_rlimits[i].opt == opt) return &g_shell_rlimits[i];
    }
    return 0;
}

static void shell_rlimit_print(const struct shell_rlimit *r, bool hard,
                               bool with_label) {
    long v = hard ? r->hard : r->soft;
    if (with_label) kprintf("%-24s%-10s", r->label, r->units);
    if (v == ULIMIT_INF) kprintf("unlimited\n");
    else kprintf("%ld\n", v);
}

static void cmd_ulimit(int argc, char **argv) {
    shell_set_status(0);
    bool show_all = false;
    bool want_hard = false, want_soft = false;
    char which = 'f';                       /* POSIX default resource */
    int i = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'a') { show_all = true; continue; }
            if (*f == 'H') { want_hard = true; continue; }
            if (*f == 'S') { want_soft = true; continue; }
            if (shell_rlimit_find(*f)) { which = *f; continue; }
            kprintf("ulimit: bad option '-%c'\n", *f);
            shell_set_status(2);
            return;
        }
    }
    /* Neither -H nor -S: report the soft limit, set both. */
    bool report_hard = want_hard && !want_soft;

    if (show_all) {
        for (size_t k = 0;
             k < sizeof(g_shell_rlimits) / sizeof(g_shell_rlimits[0]); k++) {
            shell_rlimit_print(&g_shell_rlimits[k], report_hard, true);
        }
        return;
    }

    struct shell_rlimit *r = shell_rlimit_find(which);
    if (!r) {
        kprintf("ulimit: no such resource\n");
        shell_set_status(2);
        return;
    }

    if (i >= argc) {
        shell_rlimit_print(r, report_hard, false);
        return;
    }

    long val;
    if (strcmp(argv[i], "unlimited") == 0) {
        val = ULIMIT_INF;
    } else {
        int v = 0;
        if (parse_int(argv[i], &v) < 0 || v < 0) {
            kprintf("ulimit: %s: invalid number\n", argv[i]);
            shell_set_status(2);
            return;
        }
        val = v;
    }

    bool set_hard = want_hard || !want_soft;
    bool set_soft = want_soft || !want_hard;

    /* A hard limit is a ceiling that only ever comes down. */
    if (set_hard && r->hard != ULIMIT_INF &&
        (val == ULIMIT_INF || val > r->hard)) {
        kprintf("ulimit: cannot raise a hard limit\n");
        shell_set_status(1);
        return;
    }
    long ceiling = set_hard ? val : r->hard;
    if (set_soft && ceiling != ULIMIT_INF &&
        (val == ULIMIT_INF || val > ceiling)) {
        kprintf("ulimit: soft limit exceeds hard limit\n");
        shell_set_status(1);
        return;
    }
    if (set_hard) r->hard = val;
    if (set_soft) r->soft = val;
}

/* POSIX `logname`: the login name, which for this shell is the identity the
 * session is running under. */
static void cmd_logname(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *u = env_get("LOGNAME");
    if (!u) u = env_get("USER");
    shell_printf("%s\n", u ? u : "root");
    shell_set_status(0);
}

/* POSIX `getconf NAME`: system configuration values. Only the variables this
 * kernel can answer for are listed; anything else is an error rather than a
 * fabricated number. */
static void cmd_getconf(int argc, char **argv) {
    static const struct { const char *name; long value; } vars[] = {
        { "ARG_MAX",         ARG_MAX     },
        { "LINE_MAX",        LINE_MAX    },
        { "NAME_MAX",        255         },
        { "PATH_MAX",        VFS_PATH_MAX},
        { "OPEN_MAX",        SHELL_FD_MAX},
        { "_POSIX_VERSION",  200809L     },
        { "_POSIX_ARG_MAX",  ARG_MAX     },
        { "_POSIX_OPEN_MAX", SHELL_FD_MAX},
        { "CHAR_BIT",        8           },
        { "INT_MAX",         2147483647L },
    };
    if (argc < 2) {
        for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
            shell_printf("%s=%ld\n", vars[i].name, vars[i].value);
        shell_set_status(0);
        return;
    }
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        if (strcmp(vars[i].name, argv[1]) == 0) {
            shell_printf("%ld\n", vars[i].value);
            shell_set_status(0);
            return;
        }
    }
    if (strcmp(argv[1], "PATH") == 0) {
        const char *pv = env_get("PATH");
        kprintf("%s\n", pv ? pv : "/bin");
        shell_set_status(0);
        return;
    }
    kprintf("getconf: %s: unknown configuration variable\n", argv[1]);
    shell_set_status(1);
}

/* POSIX `pathchk [-p] pathname...`: report pathnames that are not usable.
 * Without -p the check is against this system's actual limits; with -p it is
 * against the POSIX minimums, which is the portability check. */
static void cmd_pathchk(int argc, char **argv) {
    bool portable = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'p') { portable = true; continue; }
            kprintf("pathchk: bad option '-%c'\n", *f);
            shell_set_status(2);
            return;
        }
    }
    if (i >= argc) {
        kprintf("usage: pathchk [-p] pathname...\n");
        shell_set_status(2);
        return;
    }

    size_t path_max = portable ? 256 : VFS_PATH_MAX;
    size_t name_max = portable ? 14 : 255;
    static const char portable_set[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-";

    int rc = 0;
    for (; i < argc; i++) {
        const char *path = argv[i];
        if (strlen(path) >= path_max) {
            kprintf("pathchk: %s: pathname too long\n", path);
            rc = 1;
            continue;
        }
        bool bad = false;
        const char *comp = path;
        while (*comp == '/') comp++;
        while (*comp && !bad) {
            const char *end = comp;
            while (*end && *end != '/') end++;
            if ((size_t)(end - comp) > name_max) {
                kprintf("pathchk: %s: component too long\n", path);
                bad = true;
                break;
            }
            if (portable) {
                for (const char *c = comp; c < end; c++) {
                    if (!shell_word_has(portable_set, *c)) {
                        kprintf("pathchk: %s: non-portable character '%c'\n",
                                path, *c);
                        bad = true;
                        break;
                    }
                }
                if (!bad && comp < end && *comp == '-') {
                    kprintf("pathchk: %s: component starts with '-'\n", path);
                    bad = true;
                }
            }
            comp = end;
            while (*comp == '/') comp++;
        }
        if (bad) rc = 1;
    }
    shell_set_status(rc);
}

/* POSIX `newgrp [group]`: change the real group ID. tobyOS has users but no
 * supplementary-group database to switch between, so this reports that rather
 * than pretending to succeed -- a script that relies on the change would
 * otherwise carry on with the wrong privileges. */
static void cmd_newgrp(int argc, char **argv) {
    (void)argv;
    kprintf("newgrp: no group database on this system%s\n",
            argc > 1 ? "" : "");
    shell_set_status(1);
}

/* Record one command line in the history ring. Called for lines the user (or
 * the test harness) submitted, not for the pieces they decompose into. */
static void shell_history_add(const char *cmdline) {
    if (!cmdline) return;
    const char *t = shell_skip_blanks(cmdline);
    if (!*t) return;
    size_t n = strlen(cmdline);
    char *copy = (char *)kmalloc(n + 1);
    if (!copy) return;
    memcpy(copy, cmdline, n + 1);

    if (g_hist_count == SHELL_HIST_MAX) {
        kfree(g_hist[0]);
        for (int i = 1; i < SHELL_HIST_MAX; i++) g_hist[i - 1] = g_hist[i];
        g_hist[SHELL_HIST_MAX - 1] = copy;
        g_hist_base++;
    } else {
        g_hist[g_hist_count++] = copy;
    }
}

/* Drop the newest entry. `fc` uses this to take its own invocation back out
 * of the history before it looks at it, which is what POSIX means by the
 * re-executed command replacing the fc command. */
static void shell_history_pop(void) {
    if (g_hist_count <= 0) return;
    kfree(g_hist[--g_hist_count]);
    g_hist[g_hist_count] = 0;
}

/* Map a history number (or a negative offset, or a prefix string) to a ring
 * index; -1 if it names nothing. */
static int shell_history_resolve(const char *spec, int def_index) {
    if (!spec || !*spec) return def_index;
    /* parse_int rejects a leading '-', but a negative offset is exactly how
     * `fc -l -1` names the previous command, so handle the sign here. */
    bool neg = (spec[0] == '-');
    int v = 0;
    if (parse_int(neg ? spec + 1 : spec, &v) == 0) {
        long idx = neg ? (long)g_hist_count - v
                       : (long)v - (long)g_hist_base - 1;
        if (idx < 0 || idx >= g_hist_count) return -1;
        return (int)idx;
    }
    size_t n = strlen(spec);
    for (int i = g_hist_count - 1; i >= 0; i--) {
        if (strncmp(g_hist[i], spec, n) == 0) return i;
    }
    return -1;
}

/* POSIX `fc`:
 *     fc -l [-nr] [first [last]]   list
 *     fc -s [old=new] [first]      re-execute, with one substitution
 *     fc [-e editor] [first [last]]
 * There is no editor to hand off to, so the editing form is refused rather
 * than silently behaving like -s, which would run the wrong thing. */
static void cmd_fc(int argc, char **argv) {
    /* The fc command itself is not part of the range it operates on. */
    shell_history_pop();

    bool list = false, no_numbers = false, reverse = false, subst = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        /* `fc -l -1` -- a leading `-` followed by digits is a negative
         * history offset, an operand, not a bundle of option letters. */
        if (argv[i][1] >= '0' && argv[i][1] <= '9') break;
        for (const char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'l': list = true; break;
            case 'n': no_numbers = true; break;
            case 'r': reverse = true; break;
            case 's': subst = true; break;
            case 'e':
                kprintf("fc: no editor available; use -l or -s\n");
                shell_set_status(1);
                return;
            default:
                kprintf("fc: bad option '-%c'\n", *f);
                shell_set_status(2);
                return;
            }
        }
    }

    if (g_hist_count == 0) {
        if (list) { shell_set_status(0); return; }
        kprintf("fc: history is empty\n");
        shell_set_status(1);
        return;
    }

    int newest = g_hist_count - 1;

    if (subst) {
        const char *old_new = 0;
        const char *first = 0;
        for (int a = i; a < argc; a++) {
            if (!old_new && shell_word_has(argv[a], '=')) old_new = argv[a];
            else if (!first) first = argv[a];
        }
        int idx = shell_history_resolve(first, newest);
        if (idx < 0) {
            kprintf("fc: no such history entry\n");
            shell_set_status(1);
            return;
        }
        char buf[LINE_MAX];
        const char *src = g_hist[idx];
        if (old_new) {
            size_t klen = 0;
            while (old_new[klen] && old_new[klen] != '=') klen++;
            const char *rep = old_new + klen + 1;
            size_t pos = 0;
            bool done = false;
            for (const char *q = src; *q; ) {
                if (!done && klen && strncmp(q, old_new, klen) == 0) {
                    if (shell_append_str(buf, &pos, sizeof(buf), rep) < 0) break;
                    q += klen;
                    done = true;
                    continue;
                }
                if (shell_append_char(buf, &pos, sizeof(buf), *q++) < 0) break;
            }
            buf[pos] = '\0';
        } else {
            size_t n = strlen(src);
            if (n + 1 > sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, src, n);
            buf[n] = '\0';
        }
        kprintf("%s\n", buf);
        shell_history_add(buf);
        execute_line_text(buf);
        return;
    }

    const char *first_s = (i < argc) ? argv[i++] : 0;
    const char *last_s  = (i < argc) ? argv[i++] : 0;
    int first = shell_history_resolve(first_s, list ? (newest >= 15 ? newest - 15 : 0)
                                                    : newest);
    int last  = shell_history_resolve(last_s, list ? newest : first);
    if (first < 0 || last < 0) {
        kprintf("fc: no such history entry\n");
        shell_set_status(1);
        return;
    }
    if (!list) {
        kprintf("fc: no editor available; use -l or -s\n");
        shell_set_status(1);
        return;
    }
    if (first > last) { int t = first; first = last; last = t; }
    if (reverse) {
        for (int k = last; k >= first; k--) {
            if (no_numbers) kprintf("\t%s\n", g_hist[k]);
            else kprintf("%lu\t%s\n", g_hist_base + (unsigned long)k + 1, g_hist[k]);
        }
    } else {
        for (int k = first; k <= last; k++) {
            if (no_numbers) kprintf("\t%s\n", g_hist[k]);
            else kprintf("%lu\t%s\n", g_hist_base + (unsigned long)k + 1, g_hist[k]);
        }
    }
    shell_set_status(0);
}

static const struct cmd cmds[] = {
    { "help",   "list available commands",      cmd_help   },
    { "clear",  "clear the screen",             cmd_clear  },
    { "echo",   "print arguments",              cmd_echo   },
    { ":",      ": null command",                cmd_colon  },
    { "alias",  "alias [NAME=VALUE...]: define or list aliases", cmd_alias },
    { "unalias","unalias [-a] NAME...: remove aliases", cmd_unalias },
    { "break",  "break: leave the innermost loop", cmd_break },
    { "continue","continue: start next loop iteration", cmd_continue },
    { "return", "return [n]: return from script/function", cmd_return },
    { "exit",   "exit [n]: leave the current script", cmd_exit },
    { "eval",   "eval ARG...: execute concatenated arguments", cmd_eval },
    { "exec",   "exec [cmd args...]: execute command from shell", cmd_exec },
    { "command","command [-v|-V] NAME...: command lookup/run", cmd_command },
    { "sh",     "sh [-c cmd]|script: run shell text", cmd_sh },
    { ".",      ". script: run script in this shell", cmd_dot },
    { "type",   "type NAME...: describe command resolution", cmd_type },
    { "which",  "which NAME...: print resolved program path", cmd_which },
    { "pwd",    "print current directory",       cmd_pwd    },
    { "cd",     "cd [dir|-]: change current directory", cmd_cd },
    { "env",      "env [K=V ...]: print or set environment vars (M25C)", cmd_env      },
    { "export",   "export [NAME[=VALUE]...]: set shell environment",     cmd_export   },
    { "local",    "local NAME[=VALUE]...: function-scoped variable",      cmd_local    },
    { "shopt",    "shopt [-suqp] [NAME...]: shell option toggles",        cmd_shopt    },
    { "readonly", "readonly [NAME[=VALUE]...]: mark variables readonly", cmd_readonly },
    { "unset",    "unset NAME [NAME...]: remove shell variables",         cmd_unset    },
    { "set",      "set [NAME=VALUE...]: print or set shell variables",    cmd_set      },
    { "shift",    "shift [n]: shift positional parameters",              cmd_shift    },
    { "getopts",  "getopts optstring NAME [ARG...]: parse option args",   cmd_getopts  },
    { "read",     "read [-r] NAME...: read a line from standard input",   cmd_read     },
    { "times",    "times: print shell timing summary",                   cmd_times    },
    { "trap",     "trap [action condition...]: set shell traps",          cmd_trap     },
    { "setenv",   "setenv KEY VALUE: set an environment var (M25C)",     cmd_setenv   },
    { "unsetenv", "unsetenv KEY: remove an environment var (M25C)",      cmd_unsetenv },
    { "test",     "test EXPR: evaluate conditional expression",         cmd_test     },
    { "[",        "[ EXPR ]: evaluate conditional expression",          cmd_test     },
    { "printf",   "printf FORMAT [ARG...]: formatted output",          cmd_printf   },
    { "true",   "return successful status",      cmd_true   },
    { "false",  "return failing status",         cmd_false  },
    { "mem",    "show pmm + heap stats",        cmd_mem    },
    { "uptime", "show seconds since boot",      cmd_uptime },
    { "about",  "kernel info",                  cmd_about  },
    { "peek",   "peek <hex-phys-addr>",         cmd_peek   },
    { "page",   "walk page tables for virt",    cmd_page   },
    { "modules","list Limine-loaded modules",   cmd_modules},
    { "ls",     "ls [path]: list directory",    cmd_ls     },
    { "cat",    "cat <path>: print a file",     cmd_cat    },
    { "touch",  "touch <path>: create empty file (RW mounts)", cmd_touch },
    { "mkdir",  "mkdir <path>: create directory (RW mounts)",  cmd_mkdir },
    { "rm",     "rm [-r] [-f] <path>...: delete file or tree (RW mounts)", cmd_rm },
    { "write",  "write <path> <text>: write/overwrite file",   cmd_write },
    { "mounts", "list mounted filesystems",     cmd_mounts },
    { "run",    "run <path> [args]: spawn ring-3 process (fg)", cmd_run},
    { "jobs",   "list active background jobs",   cmd_jobs   },
    { "fg",     "fg <job_id>: bring bg job to foreground",  cmd_fg },
    { "bg",     "bg <job_id>: continue job in background", cmd_bg },
    { "wait",   "wait [pid|%job...]: wait for background jobs", cmd_wait },
    { "kill",   "kill [-SIGNAL] PID...: send signal to process", cmd_kill },
    { "umask",  "umask [MODE]: display or set file creation mask", cmd_umask },
    { "ulimit", "ulimit [-HS] [-acdfnstv] [value]: resource limits", cmd_ulimit },
    { "fc",     "fc [-l|-s] [first [last]]: list or re-run history", cmd_fc },
    { "logname","logname: print the login name",                  cmd_logname },
    { "getconf","getconf [NAME]: print system configuration values", cmd_getconf },
    { "pathchk","pathchk [-p] path...: check pathnames are usable", cmd_pathchk },
    { "newgrp", "newgrp [group]: change the real group ID",        cmd_newgrp },
    { "hash",   "hash [-r]: display or reset command hash table", cmd_hash },
    { "source", "source script: run script in current shell",     cmd_dot  },
    { "ps",     "list processes with cpu/syscalls/pages", cmd_ps },
    { "top",    "top [-n iters] [-d ms]: live process stats",  cmd_top  },
    { "time",   "time <cmd> [args]: measure wall + cpu + syscalls", cmd_time },
    { "perf",   "perf [reset|on|off]: dump profiling counters", cmd_perf },
    { "lxgaps", "dump the Linux ENOSYS census (ranked gap list)", cmd_lxgaps },
    { "log",    "log [enable|disable <cat>]: toggle structured log categories", cmd_log },
    { "cpus",   "list CPUs (SMP) + online state",cmd_cpus  },
    { "ifconfig","show NIC config (IP/MAC/gateway/DNS)", cmd_ifconfig },
    { "dhcp",   "renew DHCP lease (re-runs DISCOVER/REQUEST)", cmd_dhcp },
    { "nslookup","nslookup <hostname>: resolve via configured DNS server", cmd_nslookup },
    { "arp",    "dump ARP cache",                cmd_arp    },
    { "netstat","list active sockets + TCP connections", cmd_netstat},
    { "tcpconn","tcpconn <ip|host> <port>: open + close a TCP connection", cmd_tcpconn },
    { "httpget","httpget <url> [vfs-path]: HTTP GET, print or save", cmd_httpget },
    { "gui",    "gui [name]: launch GUI demo (default gui_demo)", cmd_gui },
    { "desktop","enter the desktop environment (taskbar + launcher)", cmd_desktop },
    { "trace",  "trace [on|off|verbose|status|dump|panic]: desktop log + emergency exit", cmd_trace },
    { "whoami", "print current user identity",  cmd_whoami },
    { "users",  "list users; `users add <n> <uid> <gid>`", cmd_users  },
    { "chmod",  "chmod <octal> <path>",          cmd_chmod  },
    { "chown",  "chown <user> <path>  (root only)", cmd_chown  },
    { "su",     "su <user>: change shell identity (root only)", cmd_su },
    { "pkg",    "pkg <install|remove|list|info|repo|update|upgrade|rollback>: package manager", cmd_pkg },
    { "caps",   "caps [pid]: show shell caps + profile table (milestone 18)", cmd_caps },
    { "sandbox","sandbox <profile> <cmd> [args]: run under a sandbox profile", cmd_sandbox },
    { "auditlog","auditlog [--all|--sub=NAME|--level=LVL|--since=SEQ|-n N]: M34F security audit log", cmd_auditlog },
    { "securitytest","run the M34G security validation suite (PASS/FAIL/SKIPPED)", cmd_securitytest },
    { "reboot",  "reboot the machine (ACPI reset reg, PCI 0xCF9, 8042 fallback)", cmd_reboot   },
    { "shutdown","power off the machine via ACPI S5",                              cmd_shutdown },
    { "panic",  "trigger a kernel panic (test)",cmd_panic  },
    { "install","install tobyOS to the primary IDE disk (add --yes to confirm)", cmd_install },
    { "blkdump","blkdump [-v]: list block devices (verbose: GUIDs + labels)", cmd_blkdump },
    { "partprobe","partprobe [disk]: rescan GPT on one disk or all disks", cmd_partprobe },
    { "mountfs", "mountfs <mp> <blkdev> [tobyfs|fat32]: mount blk_dev (auto-detect default)", cmd_mountfs },
    { "devlist", "devlist [bus]: M26A peripheral inventory (pci|usb|blk|input|audio|battery|hub|all)", cmd_devlist },
    { "drvtest", "drvtest [name ...]: M26A driver self-tests (defaults to all)", cmd_drvtest },
    { "hwinfo",  "hwinfo [persist]: M29A hardware inventory snapshot (CPU/mem/PCI/USB/blk/...)",        cmd_hwinfo  },
    { "drvmatch","drvmatch [disable <drv>|reenable <drv>]: M29B driver match + fallback report",        cmd_drvmatch },
    { 0, 0, 0 }
};

/* ---- line editing + dispatch ---- */

static inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void prompt(void) {
    if (g_heredoc_collecting || g_continuation_active) {
        console_set_color(0x0066FF66);
        shell_printf("> ");
        console_set_color(0x00CCCCCC);
        return;
    }
    console_set_color(0x0066FF66);   /* greenish */
    const char *user = env_get("USER");
    if (!user || !*user) user = "toby";
    const char *cwd = shell_cwd();
    struct proc *p = current_proc();
    char sigil = (p && p->uid == 0) ? '#' : '$';
    shell_printf("%s@tobyOS:%s%c ", user, cwd, sigil);
    console_set_color(0x00CCCCCC);
}

/* ---- POSIX-ish command execution -------------------------------- */

#define PIPE_BIN_PREFIX "/bin/"
#define SHELL_TOKEN_MAX 96
#define SHELL_STAGE_MAX 8
#define SHELL_REDIR_MAX 8

enum shell_tok_type {
    SH_TOK_WORD = 1,
    SH_TOK_SEMI,
    SH_TOK_BG,
    SH_TOK_PIPE,
    SH_TOK_AND_IF,
    SH_TOK_OR_IF,
    SH_TOK_REDIR_IN,
    SH_TOK_HEREDOC,
    SH_TOK_HEREDOC_TABS,
    SH_TOK_REDIR_OUT,
    SH_TOK_REDIR_APPEND,
    SH_TOK_REDIR_CLOBBER,
    SH_TOK_DUP_IN,
    SH_TOK_DUP_OUT,
};

enum shell_redir_op {
    SH_RD_OPEN_IN = 1,
    SH_RD_HEREDOC,
    SH_RD_OPEN_OUT,
    SH_RD_OPEN_APPEND,
    SH_RD_OPEN_EXCL,
    SH_RD_DUP_IN,
    SH_RD_DUP_OUT,
    SH_RD_CLOSE,
};

struct shell_token {
    enum shell_tok_type type;
    char *text;
    bool quoted;
    /* True if building this word consumed a `$` or a backtick. Only such
     * words are subject to field splitting (POSIX 2.6.5); a purely literal
     * word must survive whatever IFS happens to be, which is why
     * `IFS=o; echo hi` used to try to run /bin/ech. */
    bool expanded;
    /* True if the word was written as `name=...` in the SOURCE. Only then do
     * the declaration utilities treat it as an assignment and skip field
     * splitting -- `export ex=$words` does not split, `export $arg` does,
     * even when $arg holds the very same `ex=a b c`. */
    bool assign_src;
    int fd;
};

struct shell_redir {
    enum shell_redir_op op;
    int fd;
    int target_fd;
    const char *path;
    const char *text;
};

struct shell_simple {
    int argc;
    char *argv[ARG_MAX];
    const char *stdin_path;
    const char *stdin_text;
    const char *stdout_path;
    bool stdout_append;
    int redir_count;
    struct shell_redir redir[SHELL_REDIR_MAX];
};

struct shell_pipeline {
    int count;
    struct shell_simple stage[SHELL_STAGE_MAX];
    char expand_buf[SHELL_PARSE_BUF_MAX];
    size_t expand_pos;
};

/* Build "<prefix>/<name>" into out_buf and return a pointer to it,
 * skipping a redundant slash if `prefix` already ends in one. Returns
 * NULL on overflow. */
static const char *path_join(const char *prefix, size_t plen,
                             const char *name, size_t nlen,
                             char *out_buf, size_t out_sz) {
    bool need_slash = (plen == 0 || prefix[plen - 1] != '/');
    size_t total = plen + (need_slash ? 1 : 0) + nlen;
    if (total + 1 > out_sz) return 0;
    char *o = out_buf;
    memcpy(o, prefix, plen); o += plen;
    if (need_slash) *o++ = '/';
    memcpy(o, name, nlen);   o += nlen;
    *o = '\0';
    return out_buf;
}

/* Probe the VFS: returns true if `path` resolves to a regular file. */
static bool path_is_file(const char *path) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != VFS_OK) return false;
    return st.type == VFS_TYPE_FILE;
}

/* Resolve a single argv[0] to a runnable ELF path.
 *
 *   1. If `name` contains a slash, return it verbatim (explicit path).
 *   2. Otherwise walk $PATH from the shell env, ":"-separated, and
 *      return the first "<entry>/<name>" that exists as a regular file.
 *   3. Fallback: synthesize "/bin/<name>" so unresolved bare names
 *      still get a sensible message ("'foo' not found in $PATH").
 *
 * The returned pointer either aliases `name` or `out_buf`, so callers
 * must keep both live until the spawn is done. (M25C) */
static const char *resolve_program(const char *name, char *out_buf,
                                   size_t out_sz) {
    /* (1) explicit path */
    for (const char *c = name; *c; c++) {
        if (*c == '/') return name;
    }

    size_t nlen = strlen(name);

    /* (2) PATH search. Treat empty/missing PATH as if it were "/bin"
     * to keep behaviour stable when the user clears env unwisely. */
    const char *path_var = env_get("PATH");
    const char *cur = (path_var && *path_var) ? path_var : "/bin";
    while (*cur) {
        const char *colon = cur;
        while (*colon && *colon != ':') colon++;
        size_t plen = (size_t)(colon - cur);
        if (plen > 0) {
            const char *cand = path_join(cur, plen, name, nlen,
                                         out_buf, out_sz);
            if (cand && path_is_file(cand)) return cand;
        }
        cur = colon;
        if (*cur == ':') cur++;
    }

    /* (3) explicit fallback so the failure path has a stable label. */
    size_t prefix = sizeof(PIPE_BIN_PREFIX) - 1;
    if (prefix + nlen + 1 <= out_sz) {
        memcpy(out_buf, PIPE_BIN_PREFIX, prefix);
        memcpy(out_buf + prefix, name, nlen + 1);
        return out_buf;
    }
    return name;
}

static int shell_spawn_program_profile_fds(const char *path_arg, int argc,
                                           char **argv, bool background,
                                           const char *profile,
                                           struct file *fd0,
                                           struct file *fd1,
                                           struct file *fd2,
                                           int envc, char **envp) {
    char path_buf[64];
    const char *path = resolve_program(path_arg, path_buf, sizeof(path_buf));

    struct proc_spec spec = {
        .path = path,
        .name = argv[0],
        .fd0  = fd0, .fd1 = fd1, .fd2 = fd2,
        .argc = argc,
        .argv = argv,
        .envc = envc,
        .envp = envp,
        .sandbox_profile = profile,
        .cwd = shell_cwd(),
    };
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("spawn: failed to launch '%s'\n", path);
        return 127;
    }

    if (background) {
        g_last_bg_pid = pid;
        int jid = jobs_add(pid, argv[0]);
        if (jid < 0) {
            /* Out of job slots -- still leave the proc running, but warn
             * the user. They'll be reaped opportunistically later by
             * proc_wait() if anyone calls it, otherwise leak until
             * shutdown. */
            kprintf("[bg] pid=%d '%s' (job table full -- not tracked)\n",
                    pid, argv[0]);
        } else {
            kprintf("[%d] %d  '%s' &\n", jid, pid, argv[0]);
        }
        return 0;
    }

    signal_set_foreground(pid);
    int rc = proc_wait(pid);
    signal_set_foreground(0);
    return rc;
}

static void shell_spawn_program(const char *path_arg, int argc, char **argv,
                                bool background) {
    int rc = shell_spawn_program_profile_fds(path_arg, argc, argv, background,
                                             /*profile=*/0,
                                             g_shell_fd[0], g_shell_fd[1],
                                             g_shell_fd[2],
                                             g_envc, g_env);
    shell_set_status(rc);
}

static void shell_spawn_program_profile(const char *path_arg, int argc,
                                        char **argv, bool background,
                                        const char *profile) {
    int rc = shell_spawn_program_profile_fds(path_arg, argc, argv, background,
                                             profile,
                                             g_shell_fd[0], g_shell_fd[1],
                                             g_shell_fd[2],
                                             g_envc, g_env);
    shell_set_status(rc);
}

static int shell_append_char(char *buf, size_t *pos, size_t cap, char c) {
    if (*pos + 1 >= cap) return -1;
    buf[(*pos)++] = c;
    return 0;
}

static int shell_append_str(char *buf, size_t *pos, size_t cap,
                            const char *s) {
    if (!s) return 0;
    while (*s) {
        if (shell_append_char(buf, pos, cap, *s++) < 0) return -1;
    }
    return 0;
}

static int shell_append_n(char *buf, size_t *pos, size_t cap,
                          const char *s, size_t n) {
    if (!s) return 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (shell_append_char(buf, pos, cap, s[i]) < 0) return -1;
    }
    return 0;
}

static int shell_append_uint(char *buf, size_t *pos, size_t cap,
                             unsigned long v) {
    char tmp[32];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    while (n > 0) {
        if (shell_append_char(buf, pos, cap, tmp[--n]) < 0) return -1;
    }
    return 0;
}

struct shell_capture {
    char *buf;
    size_t cap;
    size_t pos;
    bool overflow;
};

static void shell_capture_char(struct shell_capture *cap, char c) {
    if (!cap || !cap->buf || cap->cap == 0) return;
    if (cap->pos + 1 >= cap->cap) {
        cap->overflow = true;
        return;
    }
    cap->buf[cap->pos++] = c;
    cap->buf[cap->pos] = '\0';
}

static void shell_capture_write(const char *s, void *ctx) {
    struct shell_capture *cap = (struct shell_capture *)ctx;
    if (!s) return;
    while (*s) shell_capture_char(cap, *s++);
}

static void shell_capture_kputc(void *ctx, char c) {
    shell_capture_char((struct shell_capture *)ctx, c);
}

/* How many double-quote spans enclose the text being expanded right now.
 *
 * It decides one thing: whether quotes inside the WORD of ${x:-WORD} are shell
 * syntax. Unquoted they are, so `echo ${u:-'a'}` prints a; inside double
 * quotes they are ordinary bytes, so `echo "${u:-'a'}"` prints 'a'. tsh
 * printed `a` for both.
 *
 * A counter rather than a parameter because shell_expand_var has six call
 * sites that would all have to thread it through, and it is cleared -- not
 * merely decremented -- across command substitution, whose body is a fresh
 * script with its own quoting context. */
static int g_dq_depth;

static int shell_capture_command(const char *cmd, char *out, size_t out_cap) {
    if (!cmd || !out || out_cap == 0) return -1;

    struct shell_capture cap = { .buf = out, .cap = out_cap, .pos = 0 };
    out[0] = '\0';

    shell_write_fn_t old_out = g_shell_out;
    void *old_out_ctx = g_shell_out_ctx;
    void (*old_sink)(void *ctx, char c) = 0;
    void *old_sink_ctx = 0;
    bool old_sink_suppress = false;

    printk_get_sink(&old_sink, &old_sink_ctx, &old_sink_suppress);
    shell_set_output(shell_capture_write, &cap);
    printk_set_sink_mode(shell_capture_kputc, &cap, true);

    /* A failure INSIDE the substitution is not the parent's error.
     *
     *     set -e; echo $(echo one; false); echo status=$?
     *
     * prints `one` then `status=0` in bash, dash and ash: turning that
     * failure into the parent's is what `shopt -s inherit_errexit` is
     * for, and it is off by default. The substitution runs through
     * execute_line_text, which is where the errexit trigger lives, so
     * without this the script simply stopped.
     *
     * g_last_status is restored for the same reason: $? belongs to the
     * ENCLOSING command, and letting the inner status leak out made
     * `echo $?` report the substitution's failure. */
    int saved_status = g_last_status;
    g_errexit_suspend++;
    execute_line_text(cmd);
    g_errexit_suspend--;
    g_last_status = saved_status;

    printk_set_sink_mode(old_sink, old_sink_ctx, old_sink_suppress);
    shell_set_output(old_out, old_out_ctx);

    while (cap.pos > 0 &&
           (cap.buf[cap.pos - 1] == '\n' || cap.buf[cap.pos - 1] == '\r')) {
        cap.buf[--cap.pos] = '\0';
    }
    if (cap.overflow) {
        kprintf("shell: command substitution output truncated\n");
        return -1;
    }
    return 0;
}

static int shell_parse_command_subst(const char **pp, char *cmd,
                                     size_t cmd_cap) {
    const char *p = *pp;
    if (p[0] != '$' || p[1] != '(') return -1;
    p += 2;

    size_t pos = 0;
    int depth = 1;
    while (*p) {
        if (*p == '\'') {
            if (pos + 1 >= cmd_cap) return -1;
            cmd[pos++] = *p++;
            while (*p && *p != '\'') {
                if (pos + 1 >= cmd_cap) return -1;
                cmd[pos++] = *p++;
            }
            if (*p == '\'') {
                if (pos + 1 >= cmd_cap) return -1;
                cmd[pos++] = *p++;
            }
            continue;
        }
        if (*p == '"') {
            if (pos + 1 >= cmd_cap) return -1;
            cmd[pos++] = *p++;
            while (*p && *p != '"') {
                if (p[0] == '$' && p[1] == '(') depth++;
                if (*p == ')' && depth > 1) depth--;
                if (pos + 1 >= cmd_cap) return -1;
                cmd[pos++] = *p++;
            }
            if (*p == '"') {
                if (pos + 1 >= cmd_cap) return -1;
                cmd[pos++] = *p++;
            }
            continue;
        }
        if (p[0] == '$' && p[1] == '(') {
            depth++;
            if (pos + 2 >= cmd_cap) return -1;
            cmd[pos++] = *p++;
            cmd[pos++] = *p++;
            continue;
        }
        /* A BARE `(` -- a subshell inside the substitution -- nests too. Only
         * `$(` was counted, while every `)` decremented, so `$( (echo x) )`
         * ended at the subshell's own paren and left a stray `)` in the word. */
        if (*p == '(') {
            depth++;
            if (pos + 1 >= cmd_cap) return -1;
            cmd[pos++] = *p++;
            continue;
        }
        if (*p == ')') {
            depth--;
            if (depth == 0) {
                p++;
                cmd[pos] = '\0';
                *pp = p;
                return 0;
            }
        }
        if (pos + 1 >= cmd_cap) return -1;
        cmd[pos++] = *p++;
    }
    return -1;
}

static int shell_expand_command_subst(const char **pp, char *buf,
                                      size_t *pos, size_t cap) {
    char cmd[LINE_MAX];
    char out[SHELL_PARSE_BUF_MAX];
    if (shell_parse_command_subst(pp, cmd, sizeof(cmd)) < 0) {
        kprintf("shell: bad command substitution\n");
        return -1;
    }
    int saved_dq = g_dq_depth;                  /* fresh script, fresh context */
    g_dq_depth = 0;
    int crc = shell_capture_command(cmd, out, sizeof(out));
    g_dq_depth = saved_dq;
    if (crc < 0) return -1;
    return shell_append_str(buf, pos, cap, out);
}

static int shell_expand_backtick(const char **pp, char *buf,
                                 size_t *pos, size_t cap) {
    const char *p = *pp;
    if (*p != '`') return -1;
    p++;

    char cmd[LINE_MAX];
    size_t cpos = 0;
    /* POSIX 2.6.3: within `...`, a backslash RETAINS ITS LITERAL MEANING
     * except when followed by `$`, a backtick, or another backslash. This
     * dropped every backslash, so `echo \z` inside backticks printed `z`
     * where every other shell prints `\z` -- and the escaped-backtick form,
     * the only way to nest backticks at all, never terminated in the right
     * place. The rule does not change inside double quotes. */
    /* Inside double quotes `"` joins the removal set, because the
     * double-quote layer reaches the backslash first. This is the difference
     * between `echo "x `echo \"hi\"`"` (prints x hi) and the same thing
     * written with $( ) (prints x "hi"); all shells agree, and unquoted the
     * backslash is retained. */
    const bool dq = (g_dq_depth > 0);
    while (*p && *p != '`') {
        if (*p == '\\' && (p[1] == '$' || p[1] == '`' || p[1] == '\\' ||
                            (dq && p[1] == '"'))) {
            p++;                          /* removed: the next byte is data */
        } else if (*p == '\\' && p[1]) {
            if (cpos + 2 >= sizeof(cmd)) {
                kprintf("shell: command substitution too long\n");
                return -1;
            }
            cmd[cpos++] = *p++;           /* retained: BOTH bytes are data */
        }
        if (cpos + 1 >= sizeof(cmd)) {
            kprintf("shell: command substitution too long\n");
            return -1;
        }
        cmd[cpos++] = *p++;
    }
    if (*p != '`') {
        kprintf("shell: unmatched backquote\n");
        return -1;
    }
    p++;
    cmd[cpos] = '\0';
    *pp = p;

    char out[SHELL_PARSE_BUF_MAX];
    /* The body is a fresh script. Whatever quotes enclose the backticks do not
     * enclose the commands inside them. */
    int saved_dq = g_dq_depth;
    g_dq_depth = 0;
    int rc = shell_capture_command(cmd, out, sizeof(out));
    g_dq_depth = saved_dq;
    if (rc < 0) return -1;
    return shell_append_str(buf, pos, cap, out);
}

static int shell_expand_var(const char **pp, char *buf, size_t *pos,
                            size_t cap);

static bool shell_var_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool shell_var_char(char c) {
    return shell_var_start(c) || (c >= '0' && c <= '9');
}

/* "$@" has to survive quoting as SEPARATE words -- that is the entire reason
 * it exists, and why `wrapper "$@"` is how every script forwards arguments
 * containing spaces. Expansion produces flat text, though, so there is
 * nowhere to record a word boundary... except in the text itself.
 *
 * So `$@` emits SHELL_ARG_MARK before each parameter, and shell_add_arg()
 * splits on that byte even for quoted words. 0x01 is not producible by any
 * expansion the shell performs and cannot appear in a shell word, so it
 * cannot collide with real data.
 *
 * The marker is a PREFIX rather than a separator so that zero parameters
 * yields zero words (bash: `set --; f "$@"` passes nothing) rather than one
 * empty one -- with a separator those two cases are indistinguishable.
 *
 * Known divergence, stated rather than hidden: bash glues an adjacent prefix
 * onto the first word, so `"pre$@"` with args a b gives `prea b`; here it
 * gives `pre a b`. `"$@"` alone -- essentially all real usage -- is exact.
 *
 * `star` selects "$*", which is the opposite case: a SINGLE word joined by
 * the first character of IFS. */
static int shell_append_positional_join(char *buf, size_t *pos, size_t cap,
                                        bool star) {
    if (!star) {                                /* "$@" -- one word each */
        /* Emit a marker FIRST, then one before each subsequent parameter:
         *
         *     "$@"      with a b c   ->   MARK a MARK b MARK c
         *     "pre$@"   with a b     ->   pre MARK a MARK b
         *     "$@"      with none    ->   MARK
         *
         * The leading marker is what makes the zero-parameter case
         * distinguishable from a literal empty word, and it makes the text
         * before it unambiguously a PREFIX that shell_add_marked_words glues
         * onto the first parameter -- which is how bash treats `"pre$@"`. */
        if (shell_append_char(buf, pos, cap, SHELL_ARG_MARK) < 0) return -1;
        for (int i = 0; i < g_positional_count; i++) {
            if (i > 0 && shell_append_char(buf, pos, cap, SHELL_ARG_MARK) < 0)
                return -1;
            if (shell_append_str(buf, pos, cap, g_positional[i]) < 0) return -1;
        }
        return 0;
    }
    char sep = ' ';
    const char *ifs = env_get("IFS");
    if (ifs && *ifs) sep = ifs[0];
    else if (ifs) sep = '\0';
    for (int i = 0; i < g_positional_count; i++) {
        if (i > 0 && sep && shell_append_char(buf, pos, cap, sep) < 0) return -1;
        if (shell_append_str(buf, pos, cap, g_positional[i]) < 0) return -1;
    }
    return 0;
}

static int shell_parameter_value(const char *name, char *out, size_t cap,
                                 bool *is_set) {
    size_t pos = 0;
    if (!name || !out || cap == 0 || !is_set) return -1;
    out[0] = '\0';
    *is_set = true;

    int rc = -1;
    if (strcmp(name, "?") == 0) {
        rc = shell_append_uint(out, &pos, cap, (unsigned long)g_last_status);
    } else if (strcmp(name, "!") == 0) {
        rc = shell_append_uint(out, &pos, cap, (unsigned long)g_last_bg_pid);
    } else if (strcmp(name, "$") == 0) {
        struct proc *cur = current_proc();
        rc = shell_append_uint(out, &pos, cap,
                               (unsigned long)(cur ? cur->pid : 0));
    } else if (strcmp(name, "#") == 0) {
        rc = shell_append_uint(out, &pos, cap,
                               (unsigned long)g_positional_count);
    } else if (strcmp(name, "-") == 0) {
        char opts[16];
        int oi = 0;
        if (g_opt_allexport) opts[oi++] = 'a';
        if (g_opt_notify)    opts[oi++] = 'b';
        if (g_opt_errexit)   opts[oi++] = 'e';
        if (g_opt_noglob)    opts[oi++] = 'f';
        if (g_opt_noexec)    opts[oi++] = 'n';
        if (g_opt_nounset)   opts[oi++] = 'u';
        if (g_opt_verbose)   opts[oi++] = 'v';
        if (g_opt_xtrace)    opts[oi++] = 'x';
        if (g_opt_noclobber) opts[oi++] = 'C';
        opts[oi] = '\0';
        rc = shell_append_str(out, &pos, cap, opts);
    } else if (strcmp(name, "*") == 0) {
        rc = shell_append_positional_join(out, &pos, cap, true);
    } else if (strcmp(name, "@") == 0) {
        rc = shell_append_positional_join(out, &pos, cap, false);
    } else if (name[0] >= '0' && name[0] <= '9') {
        int idx = 0;
        for (const char *d = name; *d >= '0' && *d <= '9'; d++)
            idx = idx * 10 + (*d - '0');
        if (idx == 0) {
            const char *p0 = g_param0 ? g_param0 : "tobysh";
            rc = shell_append_str(out, &pos, cap, p0);
        } else if (idx <= g_positional_count) {
            rc = shell_append_str(out, &pos, cap, g_positional[idx - 1]);
        } else {
            *is_set = false;
            return 0;
        }
    } else if (strcmp(name, "LINENO") == 0 && !env_get("LINENO")) {
        rc = shell_append_uint(out, &pos, cap, g_shell_lineno);
    } else {
        const char *v = env_get(name);
        if (!v) {
            *is_set = false;
            return 0;
        }
        rc = shell_append_str(out, &pos, cap, v);
    }
    if (rc >= 0 && pos < cap) out[pos] = '\0';
    return rc;
}

/* Expand the WORD half of ${name-WORD} / ${name=WORD} / ${name?WORD} /
 * ${name+WORD}.
 *
 * QUOTES INSIDE THE BRACES ARE REAL QUOTES. This used to copy them through
 * verbatim, so `${Unset:-'b'}` produced the three characters 'b' where bash
 * produces one, and every corpus case in var-sub-quote failed on the quote
 * marks alone. Single quotes suppress expansion; double quotes do not.
 *
 * KNOWN REMAINING GAP, stated rather than hidden: the quoted text is not yet
 * protected from the field splitting that happens later, so
 * `${Unset:-'a b c'}` yields three words where bash yields one. Fixing that
 * needs a "do not split this span" marker through the splitter, the way
 * SHELL_ARG_MARK carries "$@" word boundaries; quote REMOVAL is correct on its
 * own and is what most of these expansions actually need. */
static int shell_expand_word_ex(const char *word, char *out, size_t cap,
                               bool dq_syntax, bool sq_syntax) {
    const char *p = word ? word : "";
    size_t pos = 0;
    if (!out || cap == 0) return -1;
    out[0] = '\0';

    while (*p) {
        if (sq_syntax && *p == '\'') {           /* literal to next quote */
            p++;
            if (shell_append_char(out, &pos, cap, SHELL_NOSPLIT_MARK) < 0) return -1;
            while (*p && *p != '\'') {
                if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            }
            if (shell_append_char(out, &pos, cap, SHELL_NOSPLIT_MARK) < 0) return -1;
            if (*p == '\'') p++;
            continue;
        }
        if (dq_syntax && *p == '"') {            /* expands, quotes removed */
            p++;
            g_dq_depth++;
            if (shell_append_char(out, &pos, cap, SHELL_NOSPLIT_MARK) < 0) return -1;
            while (*p && *p != '"') {
                if (*p == '$') {
                    if (shell_expand_var(&p, out, &pos, cap) < 0) return -1;
                    continue;
                }
                if (*p == '`') {
                    if (shell_expand_backtick(&p, out, &pos, cap) < 0) return -1;
                    continue;
                }
                /* Inside double quotes a backslash only escapes these four. */
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\' ||
                                   p[1] == '$'  || p[1] == '`')) p++;
                if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            }
            g_dq_depth--;
            if (shell_append_char(out, &pos, cap, SHELL_NOSPLIT_MARK) < 0) return -1;
            if (*p == '"') p++;
            continue;
        }
        if (*p == '$') {
            if (shell_expand_var(&p, out, &pos, cap) < 0) return -1;
            continue;
        }
        if (*p == '`') {
            if (shell_expand_backtick(&p, out, &pos, cap) < 0) return -1;
            continue;
        }
        if (*p == '\\' && p[1]) {
            if (dq_syntax) {
                p++;              /* unquoted word: escape, next byte literal */
            } else if (p[1] == '$' || p[1] == '`' ||
                       p[1] == '\\' || p[1] == '\n') {
                p++;              /* here-doc body: special only before these */
            } else {
                /* here-doc body: an ORDINARY backslash. Both bytes are data,
                 * so `\"` stays `\"` -- dropping it made every escaped quote
                 * in a here-document come out bare. */
                if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            }
        }
        if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
    }
    out[pos] = '\0';
    return 0;
}

/* The WORD of ${name-WORD}: quotes are shell syntax and are removed. */
static int shell_expand_param_word(const char *word, char *out, size_t cap) {
    /* The two quote characters get different answers inside double quotes:
     *
     *     "${Unset:-'a b c'}"   ->  'a b c'   single quotes are ORDINARY BYTES
     *     "${Unset:-"a b c"}"   ->   a b c    double quotes are still syntax
     *
     * Unquoted, both are syntax. See g_dq_depth. */
    return shell_expand_word_ex(word, out, cap, true, g_dq_depth == 0);
}

/* A here-document body line, or the inside of $(( )): $ and ` still expand,
 * but a quote is an ORDINARY CHARACTER. `cat <<EOF` containing `it's fine`
 * must print the apostrophe, and the arithmetic evaluator has always been
 * handed its text with quotes intact. Sharing one expander with the ${...}
 * word above is what briefly made both of those wrong. */
static int shell_expand_literal_quotes(const char *word, char *out, size_t cap) {
    return shell_expand_word_ex(word, out, cap, false, false);
}

/* Copy `src` to `dst`, dropping split-protection markers. Called wherever a
 * word becomes an argument; a marker that escaped would be a stray 0x02 byte
 * in a program's argv. */
static void shell_strip_nosplit(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; p++)
        if (*p != SHELL_NOSPLIT_MARK) dst[o++] = *p;
    dst[o] = '\0';
}

/* Strip the marks where they stand. The words buffer is writable and marks
 * only ever shorten the text, so a consumer that is not the field splitter can
 * clean its own token rather than needing a scratch buffer. Redirect targets,
 * case patterns and `!` comparisons all read a token directly, and a mark left
 * in one of those is a raw 0x02 byte in a FILENAME. */
static void shell_strip_nosplit_inplace(char *s) {
    if (!s) return;
    char *w = s;
    for (const char *r = s; *r; r++)
        if (*r != SHELL_NOSPLIT_MARK) *w++ = *r;
    *w = ' ';
}

static bool shell_has_nosplit(const char *s) {
    for (const char *p = s; *p; p++)
        if (*p == SHELL_NOSPLIT_MARK) return true;
    return false;
}

static int shell_parse_braced_name(const char *expr, size_t *name_len) {
    if (!expr || !*expr || !name_len) return -1;
    if (expr[0] == '?' || expr[0] == '$' || expr[0] == '#' ||
        expr[0] == '@' || expr[0] == '*' || expr[0] == '-') {
        *name_len = 1;
        return 0;
    }
    if (expr[0] >= '0' && expr[0] <= '9') {
        size_t n = 1;
        while (expr[n] >= '0' && expr[n] <= '9') n++;
        *name_len = n;
        return 0;
    }
    if (!shell_var_start(expr[0])) return -1;
    size_t n = 1;
    while (shell_var_char(expr[n])) n++;
    *name_len = n;
    return 0;
}

static bool shell_glob_match(const char *pat, const char *name);

static int shell_expand_braced_parameter(const char *expr, char *buf,
                                         size_t *pos, size_t cap) {
    if (!expr) return -1;

    bool length_mode = false;
    if (expr[0] == '#' && expr[1] != '\0' && expr[1] != '-' &&
        expr[1] != '=' && expr[1] != '?' && expr[1] != '+') {
        length_mode = true;
        expr++;
    }

    size_t name_len = 0;
    if (shell_parse_braced_name(expr, &name_len) < 0) {
        kprintf("shell: bad substitution\n");
        return -1;
    }

    char name[64];
    if (name_len + 1 > sizeof(name)) {
        kprintf("shell: parameter name too long\n");
        return -1;
    }
    memcpy(name, expr, name_len);
    name[name_len] = '\0';

    const char *op = expr + name_len;
    bool colon = false;
    char opch = '\0';
    const char *word = "";
    if (*op) {
        if (*op == '#' || *op == '%') {
            char strip_op = *op++;
            bool greedy = (*op == strip_op);
            if (greedy) op++;
            const char *pattern = op;

            char value[SHELL_PARSE_BUF_MAX];
            bool is_set = false;
            if (shell_parameter_value(name, value, sizeof(value),
                                      &is_set) < 0) {
                kprintf("shell: parameter expansion too long\n");
                return -1;
            }
            if (length_mode)
                return shell_append_uint(buf, pos, cap,
                                         (unsigned long)strlen(value));

            size_t vlen = strlen(value);
            char tmp[SHELL_PARSE_BUF_MAX];
            if (strip_op == '#') {
                if (greedy) {
                    for (size_t i = vlen; i > 0; i--) {
                        memcpy(tmp, value, i);
                        tmp[i] = '\0';
                        if (shell_glob_match(pattern, tmp))
                            return shell_append_str(buf, pos, cap, value + i);
                    }
                } else {
                    for (size_t i = 0; i <= vlen; i++) {
                        memcpy(tmp, value, i);
                        tmp[i] = '\0';
                        if (shell_glob_match(pattern, tmp))
                            return shell_append_str(buf, pos, cap, value + i);
                    }
                }
            } else {
                if (greedy) {
                    for (size_t i = 0; i <= vlen; i++) {
                        if (shell_glob_match(pattern, value + i))
                            return shell_append_n(buf, pos, cap, value, i);
                    }
                } else {
                    for (size_t i = vlen; i > 0; i--) {
                        if (shell_glob_match(pattern, value + i))
                            return shell_append_n(buf, pos, cap, value, i);
                    }
                    if (shell_glob_match(pattern, value))
                        return shell_append_n(buf, pos, cap, value, 0);
                }
            }
            return shell_append_str(buf, pos, cap, value);
        }
        if (*op == ':') {
            colon = true;
            op++;
        }
        if (*op == '-' || *op == '=' || *op == '?' || *op == '+') {
            opch = *op++;
            word = op;
        } else {
            kprintf("shell: bad substitution\n");
            return -1;
        }
    }

    char value[SHELL_PARSE_BUF_MAX];
    bool is_set = false;
    if (shell_parameter_value(name, value, sizeof(value), &is_set) < 0) {
        kprintf("shell: parameter expansion too long\n");
        return -1;
    }
    bool is_null = (value[0] == '\0');

    if (length_mode) {
        return shell_append_uint(buf, pos, cap, (unsigned long)strlen(value));
    }

    if (opch == '\0') {
        if (!is_set && g_opt_nounset) {
            kprintf("%s: unbound variable\n", name);
            return -1;
        }
        return shell_append_str(buf, pos, cap, value);
    }

    bool missing = !is_set || (colon && is_null);
    char expanded_word[SHELL_PARSE_BUF_MAX];
    expanded_word[0] = '\0';

    switch (opch) {
    case '-':
        if (!missing) return shell_append_str(buf, pos, cap, value);
        if (shell_expand_param_word(word, expanded_word,
                                    sizeof(expanded_word)) < 0) return -1;
        return shell_append_str(buf, pos, cap, expanded_word);
    case '=':
        if (!missing) return shell_append_str(buf, pos, cap, value);
        if (!shell_var_start(name[0])) {
            kprintf("shell: cannot assign to special parameter\n");
            return -1;
        }
        if (shell_expand_param_word(word, expanded_word,
                                    sizeof(expanded_word)) < 0) return -1;
        if (env_set(name, expanded_word) < 0) {
            kprintf("shell: failed to assign '%s'\n", name);
            return -1;
        }
        return shell_append_str(buf, pos, cap, expanded_word);
    case '?':
        if (!missing) return shell_append_str(buf, pos, cap, value);
        if (shell_expand_param_word(word, expanded_word,
                                    sizeof(expanded_word)) < 0) return -1;
        kprintf("shell: %s: %s\n", name,
                expanded_word[0] ? expanded_word : "parameter null or not set");
        return -1;
    case '+':
        if (missing) return 0;
        if (shell_expand_param_word(word, expanded_word,
                                    sizeof(expanded_word)) < 0) return -1;
        return shell_append_str(buf, pos, cap, expanded_word);
    default:
        return -1;
    }
}

struct shell_arith {
    const char *p;
    bool ok;
};

static void shell_arith_skip(struct shell_arith *a) {
    while (*a->p == ' ' || *a->p == '\t') a->p++;
}

static long shell_arith_or(struct shell_arith *a);
static long shell_arith_ternary(struct shell_arith *a);
static int shell_append_long(char *buf, size_t *pos, size_t cap, long v);

static long shell_parse_long_value(const char *s, bool *ok) {
    long v = 0;
    bool neg = false;
    *ok = false;
    if (!s) return 0;
    if (*s == '-') {
        neg = true;
        s++;
    }
    if (*s < '0' || *s > '9') return 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s) return 0;
    *ok = true;
    return neg ? -v : v;
}

static long shell_arith_factor(struct shell_arith *a) {
    shell_arith_skip(a);
    if (*a->p == '!') {
        a->p++;
        return !shell_arith_factor(a);
    }
    if (*a->p == '~') {
        a->p++;
        return ~shell_arith_factor(a);
    }
    if (a->p[0] == '+' && a->p[1] == '+' && shell_var_start(a->p[2])) {
        a->p += 2;
        char name[64]; size_t n = 0;
        while (shell_var_char(*a->p) && n + 1 < sizeof(name)) name[n++] = *a->p++;
        name[n] = '\0';
        const char *vstr = env_get(name);
        bool ok = false;
        long v = shell_parse_long_value(vstr ? vstr : "0", &ok);
        if (!ok) v = 0;
        v++;
        char tmp[32]; size_t tpos = 0;
        shell_append_long(tmp, &tpos, sizeof(tmp), v);
        env_set(name, tmp);
        return v;
    }
    if (a->p[0] == '-' && a->p[1] == '-' && shell_var_start(a->p[2])) {
        a->p += 2;
        char name[64]; size_t n = 0;
        while (shell_var_char(*a->p) && n + 1 < sizeof(name)) name[n++] = *a->p++;
        name[n] = '\0';
        const char *vstr = env_get(name);
        bool ok = false;
        long v = shell_parse_long_value(vstr ? vstr : "0", &ok);
        if (!ok) v = 0;
        v--;
        char tmp[32]; size_t tpos = 0;
        shell_append_long(tmp, &tpos, sizeof(tmp), v);
        env_set(name, tmp);
        return v;
    }
    if (*a->p == '+') {
        a->p++;
        return shell_arith_factor(a);
    }
    if (*a->p == '-') {
        a->p++;
        return -shell_arith_factor(a);
    }
    if (*a->p == '(') {
        a->p++;
        long v = shell_arith_ternary(a);
        shell_arith_skip(a);
        if (*a->p != ')') {
            a->ok = false;
            return 0;
        }
        a->p++;
        return v;
    }
    if (*a->p >= '0' && *a->p <= '9') {
        long v = 0;
        if (*a->p == '0' && (a->p[1] == 'x' || a->p[1] == 'X')) {
            a->p += 2;
            while (1) {
                int d = -1;
                if (*a->p >= '0' && *a->p <= '9') d = *a->p - '0';
                else if (*a->p >= 'a' && *a->p <= 'f') d = *a->p - 'a' + 10;
                else if (*a->p >= 'A' && *a->p <= 'F') d = *a->p - 'A' + 10;
                if (d < 0) break;
                v = v * 16 + d;
                a->p++;
            }
        } else if (*a->p == '0' && a->p[1] >= '0' && a->p[1] <= '7') {
            while (*a->p >= '0' && *a->p <= '7') {
                v = v * 8 + (*a->p - '0');
                a->p++;
            }
        } else {
            while (*a->p >= '0' && *a->p <= '9') {
                v = v * 10 + (*a->p - '0');
                a->p++;
            }
        }
        return v;
    }
    if (shell_var_start(*a->p)) {
        char name[64];
        size_t n = 0;
        const char *name_start = a->p;
        while (shell_var_char(*a->p) && n + 1 < sizeof(name)) {
            name[n++] = *a->p++;
        }
        name[n] = '\0';
        shell_arith_skip(a);
        if (*a->p == '=' && a->p[1] != '=') {
            a->p++;
            long rhs = shell_arith_ternary(a);
            if (!a->ok) return 0;
            char tmp[32];
            size_t tpos = 0;
            shell_append_long(tmp, &tpos, sizeof(tmp), rhs);
            env_set(name, tmp);
            return rhs;
        }
        char assign_op = 0;
        if (a->p[1] == '=' &&
            (*a->p == '+' || *a->p == '-' || *a->p == '*' ||
             *a->p == '/' || *a->p == '%')) {
            assign_op = *a->p;
            a->p += 2;
        }
        const char *vstr = env_get(name);
        bool ok = false;
        long v = shell_parse_long_value(vstr ? vstr : "0", &ok);
        if (!ok) v = 0;
        if (assign_op) {
            long rhs = shell_arith_ternary(a);
            if (!a->ok) return 0;
            if (assign_op == '+') v += rhs;
            else if (assign_op == '-') v -= rhs;
            else if (assign_op == '*') v *= rhs;
            else if (assign_op == '/') { if (rhs == 0) { a->ok = false; return 0; } v /= rhs; }
            else if (assign_op == '%') { if (rhs == 0) { a->ok = false; return 0; } v %= rhs; }
            char tmp[32];
            size_t tpos = 0;
            shell_append_long(tmp, &tpos, sizeof(tmp), v);
            env_set(name, tmp);
        }
        shell_arith_skip(a);
        if (a->p[0] == '+' && a->p[1] == '+') {
            a->p += 2;
            long ret = v;
            char tmp[32]; size_t tpos = 0;
            shell_append_long(tmp, &tpos, sizeof(tmp), v + 1);
            env_set(name, tmp);
            return ret;
        }
        if (a->p[0] == '-' && a->p[1] == '-') {
            a->p += 2;
            long ret = v;
            char tmp[32]; size_t tpos = 0;
            shell_append_long(tmp, &tpos, sizeof(tmp), v - 1);
            env_set(name, tmp);
            return ret;
        }
        return v;
    }
    a->ok = false;
    return 0;
}

/* Exponentiation. Binds tighter than * / %, and is RIGHT-associative, so
 * 2 ** 3 ** 2 is 2 ** 9, not 8 ** 2 -- hence the recursion into itself for
 * the exponent rather than a loop. Sits between factor and mul so that
 * `2 ** 3 * 4` groups as `(2 ** 3) * 4`, which is what bash computes.
 *
 * Without this `$((2 ** 8))` reached the `*` case, consumed one star, and
 * then failed on the second with "bad arithmetic expansion". */
static long shell_arith_pow(struct shell_arith *a) {
    long base = shell_arith_factor(a);
    if (!a->ok) return 0;
    shell_arith_skip(a);
    if (a->p[0] != '*' || a->p[1] != '*') return base;
    a->p += 2;
    long exp = shell_arith_pow(a);
    if (!a->ok) return 0;
    if (exp < 0) {                 /* bash: "exponent less than 0" */
        a->ok = false;
        return 0;
    }
    long r = 1;
    while (exp-- > 0) r *= base;
    return r;
}

static long shell_arith_mul(struct shell_arith *a) {
    long v = shell_arith_pow(a);
    while (a->ok) {
        shell_arith_skip(a);
        char op = *a->p;
        if (op != '*' && op != '/' && op != '%') break;
        a->p++;
        long rhs = shell_arith_pow(a);
        if (!a->ok) return 0;
        if ((op == '/' || op == '%') && rhs == 0) {
            a->ok = false;
            return 0;
        }
        if (op == '*') v *= rhs;
        else if (op == '/') v /= rhs;
        else v %= rhs;
    }
    return v;
}

static long shell_arith_add(struct shell_arith *a) {
    long v = shell_arith_mul(a);
    while (a->ok) {
        shell_arith_skip(a);
        char op = *a->p;
        if (op != '+' && op != '-') break;
        a->p++;
        long rhs = shell_arith_mul(a);
        if (!a->ok) return 0;
        if (op == '+') v += rhs;
        else v -= rhs;
    }
    return v;
}

static long shell_arith_shift(struct shell_arith *a) {
    long v = shell_arith_add(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '<' && a->p[1] == '<') {
            a->p += 2;
            long rhs = shell_arith_add(a);
            if (!a->ok) return 0;
            v <<= rhs;
        } else if (a->p[0] == '>' && a->p[1] == '>') {
            a->p += 2;
            long rhs = shell_arith_add(a);
            if (!a->ok) return 0;
            v >>= rhs;
        } else break;
    }
    return v;
}

static long shell_arith_rel(struct shell_arith *a) {
    long v = shell_arith_shift(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '<' && a->p[1] == '=') {
            a->p += 2;
            long rhs = shell_arith_shift(a);
            if (!a->ok) return 0;
            v = (v <= rhs) ? 1 : 0;
        } else if (a->p[0] == '>' && a->p[1] == '=') {
            a->p += 2;
            long rhs = shell_arith_shift(a);
            if (!a->ok) return 0;
            v = (v >= rhs) ? 1 : 0;
        } else if (a->p[0] == '<' && a->p[1] != '<') {
            a->p++;
            long rhs = shell_arith_shift(a);
            if (!a->ok) return 0;
            v = (v < rhs) ? 1 : 0;
        } else if (a->p[0] == '>' && a->p[1] != '>') {
            a->p++;
            long rhs = shell_arith_shift(a);
            if (!a->ok) return 0;
            v = (v > rhs) ? 1 : 0;
        } else break;
    }
    return v;
}

static long shell_arith_eq(struct shell_arith *a) {
    long v = shell_arith_rel(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '=' && a->p[1] == '=') {
            a->p += 2;
            long rhs = shell_arith_rel(a);
            if (!a->ok) return 0;
            v = (v == rhs) ? 1 : 0;
        } else if (a->p[0] == '!' && a->p[1] == '=') {
            a->p += 2;
            long rhs = shell_arith_rel(a);
            if (!a->ok) return 0;
            v = (v != rhs) ? 1 : 0;
        } else break;
    }
    return v;
}

static long shell_arith_bitand(struct shell_arith *a) {
    long v = shell_arith_eq(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '&' && a->p[1] != '&') {
            a->p++;
            long rhs = shell_arith_eq(a);
            if (!a->ok) return 0;
            v &= rhs;
        } else break;
    }
    return v;
}

static long shell_arith_bitxor(struct shell_arith *a) {
    long v = shell_arith_bitand(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '^') {
            a->p++;
            long rhs = shell_arith_bitand(a);
            if (!a->ok) return 0;
            v ^= rhs;
        } else break;
    }
    return v;
}

static long shell_arith_bitor(struct shell_arith *a) {
    long v = shell_arith_bitxor(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '|' && a->p[1] != '|') {
            a->p++;
            long rhs = shell_arith_bitxor(a);
            if (!a->ok) return 0;
            v |= rhs;
        } else break;
    }
    return v;
}

static long shell_arith_and(struct shell_arith *a) {
    long v = shell_arith_bitor(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '&' && a->p[1] == '&') {
            a->p += 2;
            long rhs = shell_arith_bitor(a);
            if (!a->ok) return 0;
            v = (v && rhs) ? 1 : 0;
        } else break;
    }
    return v;
}

static long shell_arith_or(struct shell_arith *a) {
    long v = shell_arith_and(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (a->p[0] == '|' && a->p[1] == '|') {
            a->p += 2;
            long rhs = shell_arith_and(a);
            if (!a->ok) return 0;
            v = (v || rhs) ? 1 : 0;
        } else break;
    }
    return v;
}

static long shell_arith_ternary(struct shell_arith *a) {
    long v = shell_arith_or(a);
    shell_arith_skip(a);
    if (*a->p == '?') {
        a->p++;
        long if_true = shell_arith_ternary(a);
        shell_arith_skip(a);
        if (*a->p != ':') { a->ok = false; return 0; }
        a->p++;
        long if_false = shell_arith_ternary(a);
        if (!a->ok) return 0;
        return v ? if_true : if_false;
    }
    return v;
}

static long shell_arith_comma(struct shell_arith *a) {
    long v = shell_arith_ternary(a);
    while (a->ok) {
        shell_arith_skip(a);
        if (*a->p != ',') break;
        a->p++;
        v = shell_arith_ternary(a);
    }
    return v;
}

#define shell_arith_expr shell_arith_comma

static int shell_append_long(char *buf, size_t *pos, size_t cap, long v) {
    if (v < 0) {
        if (shell_append_char(buf, pos, cap, '-') < 0) return -1;
        return shell_append_uint(buf, pos, cap, (unsigned long)(-v));
    }
    return shell_append_uint(buf, pos, cap, (unsigned long)v);
}

static int shell_expand_arith(const char **pp, char *buf, size_t *pos,
                              size_t cap) {
    const char *p = *pp;
    if (p[0] != '$' || p[1] != '(' || p[2] != '(') return -1;
    p += 3;

    char expr[256];
    size_t epos = 0;
    int paren_depth = 0;
    while (*p) {
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            if (paren_depth == 0 && p[1] == ')') {
                p += 2;
                expr[epos] = '\0';
                *pp = p;
                /* Expand the expression text before evaluating it. The
                 * evaluator resolves bare names itself, but knows nothing
                 * about `$( )` or `${ }`, so `$(( $(echo 4) + 1 ))` reached
                 * it as literal text and died as "bad arithmetic expansion".
                 * Expanding first also makes `$(($x + 1))` and `${#a}` work
                 * inside arithmetic for free. */
                char *xexpr = kmalloc(SHELL_PARSE_BUF_MAX);
                if (xexpr) {
                    if (shell_expand_literal_quotes(expr, xexpr,
                                                    SHELL_PARSE_BUF_MAX) == 0 &&
                        strlen(xexpr) < sizeof(expr)) {
                        memcpy(expr, xexpr, strlen(xexpr) + 1);
                    }
                    kfree(xexpr);
                }
                struct shell_arith a = { .p = expr, .ok = true };
                long v = shell_arith_expr(&a);
                shell_arith_skip(&a);
                if (!a.ok || *a.p != '\0') {
                    kprintf("shell: bad arithmetic expansion\n");
                    return -1;
                }
                return shell_append_long(buf, pos, cap, v);
            }
            if (paren_depth > 0) paren_depth--;
        }
        if (epos + 1 >= sizeof(expr)) {
            kprintf("shell: arithmetic expression too long\n");
            return -1;
        }
        expr[epos++] = *p++;
    }
    kprintf("shell: unterminated arithmetic expansion\n");
    return -1;
}

static bool shell_operator_char(char c) {
    return c == ';' || c == '&' || c == '|' || c == '<' || c == '>';
}

static int shell_expand_var(const char **pp, char *buf, size_t *pos,
                            size_t cap) {
    const char *p = *pp;
    if (*p != '$') return shell_append_char(buf, pos, cap, *p++);
    p++;

    if (*p == '(' && p[1] == '(') {
        p--;
        *pp = p;
        return shell_expand_arith(pp, buf, pos, cap);
    }
    if (*p == '(') {
        p--;
        *pp = p;
        return shell_expand_command_subst(pp, buf, pos, cap);
    }
    if (*p == '?') {
        p++;
        *pp = p;
        return shell_append_uint(buf, pos, cap, (unsigned long)g_last_status);
    }
    if (*p == '!') {
        p++;
        *pp = p;
        return shell_append_uint(buf, pos, cap, (unsigned long)g_last_bg_pid);
    }
    if (*p == '$') {
        p++;
        *pp = p;
        struct proc *cur = current_proc();
        return shell_append_uint(buf, pos, cap,
                                 (unsigned long)(cur ? cur->pid : 0));
    }
    if (*p == '#') {
        p++;
        *pp = p;
        return shell_append_uint(buf, pos, cap,
                                 (unsigned long)g_positional_count);
    }
    if (*p == '-') {
        p++;
        *pp = p;
        char opts[16];
        int oi = 0;
        if (g_opt_allexport) opts[oi++] = 'a';
        if (g_opt_notify)    opts[oi++] = 'b';
        if (g_opt_errexit)   opts[oi++] = 'e';
        if (g_opt_noglob)    opts[oi++] = 'f';
        if (g_opt_noexec)    opts[oi++] = 'n';
        if (g_opt_nounset)   opts[oi++] = 'u';
        if (g_opt_verbose)   opts[oi++] = 'v';
        if (g_opt_xtrace)    opts[oi++] = 'x';
        if (g_opt_noclobber) opts[oi++] = 'C';
        opts[oi] = '\0';
        return shell_append_str(buf, pos, cap, opts);
    }
    if (*p == '*') {
        p++;
        *pp = p;
        return shell_append_positional_join(buf, pos, cap, true);
    }
    if (*p == '@') {
        p++;
        *pp = p;
        return shell_append_positional_join(buf, pos, cap, false);
    }
    if (*p >= '0' && *p <= '9') {
        int idx = *p++ - '0';
        *pp = p;
        if (idx == 0) {
            return shell_append_str(buf, pos, cap,
                                    g_param0 ? g_param0 : "tobysh");
        }
        if (idx <= g_positional_count) {
            return shell_append_str(buf, pos, cap, g_positional[idx - 1]);
        }
        return 0;
    }

    char name[64];
    size_t n = 0;
    if (*p == '{') {
        p++;
        char expr[128];
        while (*p && *p != '}' && n + 1 < sizeof(expr)) expr[n++] = *p++;
        if (*p != '}') {
            kprintf("shell: unterminated ${...}\n");
            return -1;
        }
        p++;
        expr[n] = '\0';
        *pp = p;
        return shell_expand_braced_parameter(expr, buf, pos, cap);
    } else if (shell_var_start(*p)) {
        while (shell_var_char(*p) && n + 1 < sizeof(name)) name[n++] = *p++;
    } else {
        if (shell_append_char(buf, pos, cap, '$') < 0) return -1;
        *pp = p;
        return 0;
    }
    name[n] = '\0';
    *pp = p;
    const char *val = env_get(name);
    if (!val && strcmp(name, "LINENO") == 0)
        return shell_append_uint(buf, pos, cap, g_shell_lineno);
    if (!val && g_opt_nounset) {
        kprintf("%s: unbound variable\n", name);
        return -1;
    }
    return shell_append_str(buf, pos, cap, val);
}

static bool g_tok_word_expanded;

/* True while the tokenizer is building a word that began, in the SOURCE,
 * with `name=`. Recorded on the token because the splitter sees only the
 * expanded text, where `ex=a b c` from a variable is indistinguishable
 * from a literal `ex=a b c` -- and bash splits the first, not the second. */
static bool g_tok_word_assign_src;

static int shell_emit_token_fd(struct shell_token *tok, int *ntok,
                               enum shell_tok_type type, char *text,
                               bool quoted, int fd) {
    if (*ntok >= SHELL_TOKEN_MAX) {
        kprintf("shell: too many tokens\n");
        return -1;
    }
    tok[*ntok].type = type;
    tok[*ntok].text = text;
    tok[*ntok].quoted = quoted;
    tok[*ntok].expanded = g_tok_word_expanded;
    tok[*ntok].assign_src = g_tok_word_assign_src;
    tok[*ntok].fd = fd;
    (*ntok)++;
    return 0;
}

static int shell_emit_token(struct shell_token *tok, int *ntok,
                            enum shell_tok_type type, char *text,
                            bool quoted) {
    return shell_emit_token_fd(tok, ntok, type, text, quoted, -1);
}

static bool shell_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int shell_tokenize(const char *src, struct shell_token *tok,
                          int *out_ntok, char *words, size_t word_cap) {
    int ntok = 0;
    size_t wpos = 0;
    const char *p = src;

    while (*p) {
        while (is_space(*p)) p++;
        if (!*p) break;
        if (*p == '#') break;

        int explicit_fd = -1;
        const char *redir_p = p;
        if (shell_is_digit(*redir_p)) {
            int n = 0;
            const char *d = redir_p;
            while (shell_is_digit(*d)) {
                n = n * 10 + (*d - '0');
                d++;
            }
            if (*d == '<' || *d == '>') {
                explicit_fd = n;
                p = d;
            }
        }

        if (*p == ';') {
            if (shell_emit_token(tok, &ntok, SH_TOK_SEMI, 0, false) < 0) return -1;
            p++;
            continue;
        }
        if (*p == '&') {
            enum shell_tok_type t = SH_TOK_BG;
            if (p[1] == '&') { t = SH_TOK_AND_IF; p++; }
            if (shell_emit_token(tok, &ntok, t, 0, false) < 0) return -1;
            p++;
            continue;
        }
        if (*p == '|') {
            enum shell_tok_type t = SH_TOK_PIPE;
            if (p[1] == '|') { t = SH_TOK_OR_IF; p++; }
            if (shell_emit_token(tok, &ntok, t, 0, false) < 0) return -1;
            p++;
            continue;
        }
        if (*p == '<') {
            enum shell_tok_type t = SH_TOK_REDIR_IN;
            if (p[1] == '<') {
                t = SH_TOK_HEREDOC;
                p++;
                if (p[1] == '-') {
                    t = SH_TOK_HEREDOC_TABS;
                    p++;
                }
            } else if (p[1] == '&') {
                t = SH_TOK_DUP_IN;
                p++;
            }
            if (shell_emit_token_fd(tok, &ntok, t, 0, false, explicit_fd) < 0) return -1;
            p++;
            continue;
        }
        if (*p == '>') {
            enum shell_tok_type t = SH_TOK_REDIR_OUT;
            if (p[1] == '>') { t = SH_TOK_REDIR_APPEND; p++; }
            else if (p[1] == '|') { t = SH_TOK_REDIR_CLOBBER; p++; }
            else if (p[1] == '&') { t = SH_TOK_DUP_OUT; p++; }
            if (shell_emit_token_fd(tok, &ntok, t, 0, false, explicit_fd) < 0) return -1;
            p++;
            continue;
        }

        char *start = words + wpos;
        bool got = false;
        bool word_quoted = false;
        g_tok_word_expanded = false;
        /* Look at the RAW text: a leading `name=` makes this an assignment
         * word. After expansion the same shape can arrive from a variable's
         * value, and that is NOT an assignment -- see g_tok_word_assign_src. */
        g_tok_word_assign_src = false;
        {
            const char *a = p;
            if ((*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z') || *a == '_') {
                a++;
                while ((*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z') ||
                       (*a >= '0' && *a <= '9') || *a == '_') a++;
                if (*a == '=') g_tok_word_assign_src = true;
            }
        }
        while (*p && !is_space(*p) && !shell_operator_char(*p)) {
            got = true;
            if (*p == '$' && p[1] == '\'') {
                word_quoted = true;
                p += 2;
                while (*p && *p != '\'') {
                    if (*p == '\\' && p[1]) {
                        p++;
                        char c = 0;
                        switch (*p) {
                        case 'a': c = '\a'; p++; break;
                        case 'b': c = '\b'; p++; break;
                        case 'e': c = 0x1B; p++; break;
                        case 'f': c = '\f'; p++; break;
                        case 'n': c = '\n'; p++; break;
                        case 'r': c = '\r'; p++; break;
                        case 't': c = '\t'; p++; break;
                        case 'v': c = '\v'; p++; break;
                        case '\\': c = '\\'; p++; break;
                        case '\'': c = '\''; p++; break;
                        case '"': c = '"'; p++; break;
                        case '0': {
                            p++;
                            int v = 0, cnt = 0;
                            while (cnt < 3 && *p >= '0' && *p <= '7') {
                                v = v * 8 + (*p++ - '0'); cnt++;
                            }
                            c = (char)v;
                            break;
                        }
                        case 'x': {
                            p++;
                            int v = 0, cnt = 0;
                            while (cnt < 2) {
                                int d = -1;
                                if (*p >= '0' && *p <= '9') d = *p - '0';
                                else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
                                else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
                                if (d < 0) break;
                                v = v * 16 + d; p++; cnt++;
                            }
                            c = (char)v;
                            break;
                        }
                        default: c = *p++; break;
                        }
                        if (c && shell_append_char(words, &wpos, word_cap, c) < 0) return -1;
                    } else {
                        if (shell_append_char(words, &wpos, word_cap, *p++) < 0) return -1;
                    }
                }
                if (*p == '\'') p++;
                continue;
            }
            if (*p == '\'') {
                word_quoted = true;
                p++;
                /* Mark the span so the splitter can see where quoting starts
                 * and stops WITHIN the word -- `$a"$b"` splits in the middle. */
                if (shell_append_char(words, &wpos, word_cap,
                                      SHELL_NOSPLIT_MARK) < 0) return -1;
                while (*p && *p != '\'') {
                    if (shell_append_char(words, &wpos, word_cap, *p++) < 0) {
                        kprintf("shell: word too long\n");
                        return -1;
                    }
                }
                if (shell_append_char(words, &wpos, word_cap,
                                      SHELL_NOSPLIT_MARK) < 0) return -1;
                if (*p != '\'') {
                    kprintf("shell: unmatched single quote\n");
                    return -1;
                }
                p++;
                continue;
            }
            if (*p == '"') {
                word_quoted = true;
                p++;
                g_dq_depth++;
                if (shell_append_char(words, &wpos, word_cap,
                                      SHELL_NOSPLIT_MARK) < 0) return -1;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) {
                        /* POSIX 2.2.3: inside double quotes a backslash is
                         * special ONLY before $ ` " \ or newline. Anywhere
                         * else it is an ordinary character and BOTH it and
                         * what follows are literal -- so "a\bc" is a\bc, not
                         * abc. Swallowing it unconditionally quietly ate a
                         * character out of every Windows path and regex a
                         * script ever put in double quotes. */
                        char nxt = p[1];
                        if (nxt != '$' && nxt != '`' && nxt != '"' &&
                            nxt != '\\' && nxt != '\n') {
                            if (shell_append_char(words, &wpos, word_cap, *p++) < 0) {
                                kprintf("shell: word too long\n");
                                return -1;
                            }
                            continue;
                        }
                        p++;
                        if (shell_append_char(words, &wpos, word_cap, *p++) < 0) {
                            kprintf("shell: word too long\n");
                            return -1;
                        }
                    } else if (*p == '$') {
                        if (shell_expand_var(&p, words, &wpos, word_cap) < 0) return -1;
                    } else if (*p == '`') {
                        if (shell_expand_backtick(&p, words, &wpos, word_cap) < 0) return -1;
                        g_tok_word_expanded = true;
                    } else {
                        if (shell_append_char(words, &wpos, word_cap, *p++) < 0) {
                            kprintf("shell: word too long\n");
                            return -1;
                        }
                    }
                }
                g_dq_depth--;
                if (shell_append_char(words, &wpos, word_cap,
                                      SHELL_NOSPLIT_MARK) < 0) return -1;
                if (*p != '"') {
                    kprintf("shell: unmatched double quote\n");
                    return -1;
                }
                p++;
                continue;
            }
            if (*p == '\\') {
                word_quoted = true;
                p++;
                if (!*p) {
                    kprintf("shell: trailing escape\n");
                    return -1;
                }
                if (shell_append_char(words, &wpos, word_cap, *p++) < 0) {
                    kprintf("shell: word too long\n");
                    return -1;
                }
                continue;
            }
            /* Flag AFTER the expander returns. Command substitution and
             * friends re-enter this tokenizer, and each nested word clears
             * the flag -- so setting it first left the OUTER word marked
             * with whatever the last inner word did. */
            if (*p == '`') {
                if (shell_expand_backtick(&p, words, &wpos, word_cap) < 0) return -1;
                g_tok_word_expanded = true;
                continue;
            }
            if (*p == '$') {
                if (shell_expand_var(&p, words, &wpos, word_cap) < 0) return -1;
                g_tok_word_expanded = true;
                continue;
            }
            if (shell_append_char(words, &wpos, word_cap, *p++) < 0) {
                kprintf("shell: word too long\n");
                return -1;
            }
        }

        if (got) {
            if (shell_append_char(words, &wpos, word_cap, '\0') < 0) {
                kprintf("shell: parse buffer full\n");
                return -1;
            }
            if (shell_emit_token(tok, &ntok, SH_TOK_WORD, start, word_quoted) < 0) return -1;
        }
    }

    *out_ntok = ntok;
    return 0;
}

static bool shell_alias_name_char(char c) {
    return c && !is_space(c) && !shell_operator_char(c) &&
           c != '(' && c != ')' && c != '\'' && c != '"' && c != '\\';
}

/* Names whose REPLACEMENT TEXT is currently being expanded.
 *
 * POSIX: the replacement is reprocessed for aliases, but the name being
 * replaced is not substituted again -- that is what stops
 * `alias echo='echo foo'` from recursing for ever. The set is scoped to the
 * recursion, NOT to the line: a later, separate occurrence of the same name
 * is an ordinary substitution, so
 *
 *     alias e=echo
 *     e two; e three
 *
 * expands both. A per-line set got the second one wrong. */
#define SHELL_ALIAS_DEPTH_MAX 16
static char g_alias_active[SHELL_ALIAS_DEPTH_MAX][64];
static int  g_alias_depth;

static bool shell_alias_already_used(const char *name) {
    for (int i = 0; i < g_alias_depth; i++)
        if (strcmp(g_alias_active[i], name) == 0) return true;
    return false;
}

static int shell_expand_aliases_once(const char *src, char *out, size_t cap,
                                     bool *changed) {
    const char *p = src ? src : "";
    size_t pos = 0;
    bool at_cmd = true;
    *changed = false;

    while (*p) {
        if (*p == '#') {
            if (shell_append_str(out, &pos, cap, p) < 0) return -1;
            break;
        }

        if (is_space(*p)) {
            if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            continue;
        }

        if (*p == ';') {
            at_cmd = true;
            if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            continue;
        }
        if (*p == '&') {
            at_cmd = true;
            if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            if (*p == '&') {
                if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            }
            continue;
        }
        if (*p == '|') {
            at_cmd = true;
            if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            if (*p == '|') {
                if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            }
            continue;
        }

        if (at_cmd && shell_alias_name_char(*p)) {
            const char *start = p;
            while (shell_alias_name_char(*p)) p++;
            size_t n = (size_t)(p - start);
            char name[64];
            if (n + 1 <= sizeof(name)) {
                memcpy(name, start, n);
                name[n] = '\0';
                const char *q = shell_skip_blanks(p);
                /* Not in a script unless asked. This is the rule, not an
                 * optimisation: `alias foo=bar; foo` runs bar interactively
                 * and reports "foo: not found" in a script. */
                bool may_expand = (g_interactive || g_opt_expand_aliases);
                /* ...and never a name already substituted on this line, or
                 * `alias echo='echo foo'` recurses until the pass counter
                 * gives up and the command produces nothing at all. */
                const char *av = (*q == '(' || !may_expand ||
                                  shell_alias_already_used(name))
                                     ? 0 : shell_alias_value(name);
                if (av) {
                    size_t avlen = strlen(av);
                    /* Expand the replacement NOW, with this name marked
                     * active, then carry on through the rest of the line with
                     * the outer set restored. One pass, correct scoping. */
                    if (g_alias_depth < SHELL_ALIAS_DEPTH_MAX) {
                        ksnprintf(g_alias_active[g_alias_depth], 64, "%s", name);
                        g_alias_depth++;
                        char *sub_buf = (char *)kmalloc(SHELL_PARSE_BUF_MAX);
                        if (!sub_buf) { g_alias_depth--; return -1; }
                        bool sub_changed = false;
                        int rc = shell_expand_aliases_once(av, sub_buf,
                                                           SHELL_PARSE_BUF_MAX,
                                                           &sub_changed);
                        g_alias_depth--;
                        if (rc < 0) { kfree(sub_buf); return -1; }
                        int arc = shell_append_str(out, &pos, cap, sub_buf);
                        kfree(sub_buf);
                        if (arc < 0) return -1;
                    } else if (shell_append_str(out, &pos, cap, av) < 0) {
                        return -1;
                    }
                    *changed = true;
                    /* POSIX: a replacement ending in a blank means the NEXT
                     * word is checked for aliases too -- which is what makes
                     * `alias sudo='sudo '` expand what follows it. */
                    at_cmd = (avlen > 0 && (av[avlen - 1] == ' ' ||
                                            av[avlen - 1] == '\t'));
                    continue;
                }
            }
            if (pos + n + 1 > cap) return -1;
            memcpy(out + pos, start, n);
            pos += n;
            out[pos] = '\0';
            at_cmd = false;
            continue;
        }

        if (*p == '\'' || *p == '"') {
            char quote = *p;
            at_cmd = false;
            if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            while (*p && *p != quote) {
                if (*p == '\\' && quote == '"' && p[1]) {
                    if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
                }
                if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            }
            if (*p && shell_append_char(out, &pos, cap, *p++) < 0) return -1;
            continue;
        }

        at_cmd = false;
        if (shell_append_char(out, &pos, cap, *p++) < 0) return -1;
    }

    out[pos] = '\0';
    return 0;
}

static const char *shell_expand_aliases(const char *src, char *buf,
                                        size_t cap) {
    char tmp[SHELL_PARSE_BUF_MAX];
    const char *cur = src ? src : "";
    bool changed = false;


    /* ONE pass. The expander recurses into each replacement itself, carrying
     * the name it is replacing, so there is nothing left for a second pass to
     * find -- and no pass counter to run out, which used to be the only
     * symptom of `alias echo='echo foo'`. */
    (void)tmp;
    g_alias_depth = 0;
    /* Alias expansion runs once per line, before anything else, so this is the
     * one place that reliably sees a line boundary. An expansion that failed
     * mid-way through a double-quoted span -- an unmatched quote, a word that
     * grew too long -- returns without decrementing, and a leaked depth would
     * make the NEXT line treat ${x:-'a'} as if it were quoted. */
    g_dq_depth = 0;
    if (shell_expand_aliases_once(cur, buf, cap, &changed) < 0) {
        kprintf("alias: expansion too long\n");
        return 0;
    }
    return buf;
}

static bool shell_is_list_sep(enum shell_tok_type t) {
    return t == SH_TOK_SEMI || t == SH_TOK_BG ||
           t == SH_TOK_AND_IF || t == SH_TOK_OR_IF;
}

static void shell_simple_init(struct shell_simple *s) {
    memset(s, 0, sizeof(*s));
}

static int shell_pipeline_save_word(struct shell_pipeline *pl,
                                    const char *s, char **out) {
    if (!pl || !s || !out) return -1;
    size_t n = strlen(s);
    if (pl->expand_pos + n + 1 > sizeof(pl->expand_buf)) {
        kprintf("shell: expansion buffer full\n");
        return -1;
    }
    char *dst = pl->expand_buf + pl->expand_pos;
    memcpy(dst, s, n + 1);
    pl->expand_pos += n + 1;
    *out = dst;
    return 0;
}

static bool shell_has_glob(const char *s) {
    if (!s) return false;
    for (; *s; s++) {
        if (*s == '*' || *s == '?' || *s == '[') return true;
    }
    return false;
}

static bool shell_glob_bracket(const char **pp, char c) {
    const char *p = *pp + 1;   /* skip '[' */
    bool negate = false;
    bool matched = false;

    if (*p == '!' || *p == '^') {
        negate = true;
        p++;
    }
    if (*p == ']') {
        if (c == ']') matched = true;
        p++;
    }

    bool closed = false;
    while (*p) {
        if (*p == ']') {
            closed = true;
            p++;
            break;
        }
        if (p[1] == '-' && p[2] && p[2] != ']') {
            char lo = p[0];
            char hi = p[2];
            if (lo <= c && c <= hi) matched = true;
            p += 3;
            continue;
        }
        if (*p == c) matched = true;
        p++;
    }

    if (!closed) {
        /* Malformed bracket expressions behave like a literal '['. */
        *pp = *pp + 1;
        return c == '[';
    }
    *pp = p;
    return negate ? !matched : matched;
}

static bool shell_glob_match(const char *pat, const char *name) {
    while (*pat) {
        /* A backslash makes the next character literal. This is how a QUOTED
         * metacharacter survives: `case $v in "*")` must match the one-character
         * string `*`, not everything. shell_case_unquote() strips the quotes
         * and escapes what was inside them, so the distinction between `*` and
         * `"*"` reaches here instead of being lost with the quotes. */
        if (*pat == '\\' && pat[1]) {
            if (*name != pat[1]) return false;
            pat += 2;
            name++;
            continue;
        }
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return true;
            for (const char *n = name; ; n++) {
                if (shell_glob_match(pat, n)) return true;
                if (!*n) return false;
            }
        }
        if (!*name) return false;
        if (*pat == '?') {
            pat++;
            name++;
            continue;
        }
        if (*pat == '[') {
            const char *after = pat;
            if (!shell_glob_bracket(&after, *name)) return false;
            pat = after;
            name++;
            continue;
        }
        if (*pat != *name) return false;
        pat++;
        name++;
    }
    return *name == '\0';
}

static int shell_expand_tilde_word(struct shell_pipeline *pl,
                                   const char *word, char **out) {
    if (!word || word[0] != '~') return 0;

    const char *slash = word + 1;
    while (*slash && *slash != '/') slash++;

    const char *home = 0;
    if (slash == word + 1) {
        home = env_get("HOME");
        if (!home || !*home) home = "/";
    } else {
        char uname[64];
        size_t ulen = (size_t)(slash - (word + 1));
        if (ulen + 1 > sizeof(uname)) return 0;
        memcpy(uname, word + 1, ulen);
        uname[ulen] = '\0';
        /* In this kernel, all users live under /home/<user> */
        static char ubuf[VFS_PATH_MAX];
        size_t n = 0;
        const char *pfx = "/home/";
        while (*pfx && n + 1 < sizeof(ubuf)) ubuf[n++] = *pfx++;
        for (size_t i = 0; i < ulen && n + 1 < sizeof(ubuf); i++)
            ubuf[n++] = uname[i];
        ubuf[n] = '\0';
        home = ubuf;
    }

    char tmp[VFS_PATH_MAX];
    size_t hlen = strlen(home);
    size_t rest = strlen(slash);
    bool drop_slash = (hlen > 1 && home[hlen - 1] == '/' && *slash == '/');
    if (hlen + rest + 1 > sizeof(tmp)) return -1;
    memcpy(tmp, home, hlen);
    size_t pos = hlen;
    const char *tail = slash;
    if (drop_slash) tail++;
    while (*tail && pos + 1 < sizeof(tmp)) tmp[pos++] = *tail++;
    if (*tail) return -1;
    tmp[pos] = '\0';
    return shell_pipeline_save_word(pl, tmp, out) < 0 ? -1 : 1;
}

static int shell_expand_glob_word(struct shell_pipeline *pl,
                                  struct shell_simple *cur,
                                  const char *word) {
    const char *last_slash = 0;
    for (const char *p = word; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    char dir_arg[VFS_PATH_MAX];
    char display_prefix[VFS_PATH_MAX];
    const char *pattern = word;

    if (!last_slash) {
        memcpy(dir_arg, ".", 2);
        display_prefix[0] = '\0';
    } else {
        size_t dlen = (size_t)(last_slash - word);
        size_t plen = (size_t)(last_slash - word + 1);
        if (dlen == 0) {
            memcpy(dir_arg, "/", 2);
        } else {
            if (dlen + 1 > sizeof(dir_arg)) return -1;
            memcpy(dir_arg, word, dlen);
            dir_arg[dlen] = '\0';
        }
        if (plen + 1 > sizeof(display_prefix)) return -1;
        memcpy(display_prefix, word, plen);
        display_prefix[plen] = '\0';
        pattern = last_slash + 1;
    }

    if (!pattern[0]) return 0;

    char dir_path[VFS_PATH_MAX];
    if (shell_canonicalize_path(dir_arg, dir_path, sizeof(dir_path)) < 0) {
        return -1;
    }
    if (word[0] == '~') {
        size_t dlen = strlen(dir_path);
        if (dlen + 2 > sizeof(display_prefix)) return -1;
        memcpy(display_prefix, dir_path, dlen);
        if (dlen == 0 || dir_path[dlen - 1] != '/') display_prefix[dlen++] = '/';
        display_prefix[dlen] = '\0';
    }

    struct vfs_dir d;
    int rc = vfs_opendir(dir_path, &d);
    if (rc != VFS_OK) return 0;

    int matches = 0;
    int first_arg = cur->argc;
    struct vfs_dirent ent;
    while ((rc = vfs_readdir(&d, &ent)) == VFS_OK) {
        if (ent.name[0] == '.' && pattern[0] != '.') continue;
        if (!shell_glob_match(pattern, ent.name)) continue;
        if (cur->argc >= ARG_MAX) {
            vfs_closedir(&d);
            kprintf("shell: too many arguments after expansion (max %d)\n", ARG_MAX);
            return -1;
        }
        char tmp[VFS_PATH_MAX];
        size_t pre = strlen(display_prefix);
        size_t nam = strlen(ent.name);
        if (pre + nam + 1 > sizeof(tmp)) {
            vfs_closedir(&d);
            return -1;
        }
        memcpy(tmp, display_prefix, pre);
        memcpy(tmp + pre, ent.name, nam + 1);
        if (shell_pipeline_save_word(pl, tmp, &cur->argv[cur->argc]) < 0) {
            vfs_closedir(&d);
            return -1;
        }
        cur->argc++;
        matches++;
    }
    vfs_closedir(&d);

    /* Glob results are SORTED. bash guarantees it, so scripts rely on it --
     * `for f in *.txt` processing files in directory order looks fine until
     * the directory is rehashed and the output silently reorders. readdir
     * gives us whatever order the filesystem stores, so sort here. */
    for (int a = first_arg + 1; a < cur->argc; a++) {
        char *key = cur->argv[a];
        int b = a - 1;
        while (b >= first_arg && strcmp(cur->argv[b], key) > 0) {
            cur->argv[b + 1] = cur->argv[b];
            b--;
        }
        cur->argv[b + 1] = key;
    }
    return matches;
}

static int shell_add_one_arg(struct shell_pipeline *pl, struct shell_simple *cur,
                             const char *word, bool quoted) {
    if (cur->argc >= ARG_MAX) {
        kprintf("shell: too many arguments (max %d)\n", ARG_MAX);
        return -1;
    }

    /* The funnel: every word becomes an argument through here, so this is the
     * one place that has to guarantee no split-protection marker escapes. */
    char unmarked[SHELL_PARSE_BUF_MAX];
    if (shell_has_nosplit(word)) {
        shell_strip_nosplit(unmarked, sizeof unmarked, word);
        char *saved = 0;
        if (shell_pipeline_save_word(pl, unmarked, &saved) < 0) return -1;
        word = saved;
    }

    if (!quoted) {
        if (!g_opt_noglob && shell_has_glob(word)) {
            int n = shell_expand_glob_word(pl, cur, word);
            if (n < 0) return -1;
            if (n > 0) return 0;
        }

        char *expanded = 0;
        int trc = shell_expand_tilde_word(pl, word, &expanded);
        if (trc < 0) {
            kprintf("shell: expansion too long\n");
            return -1;
        }
        if (trc > 0) {
            cur->argv[cur->argc++] = expanded;
            return 0;
        }

        /* POSIX 2.6.1: in an ASSIGNMENT, a tilde is also expanded right after
         * the `=` and after each `:`. That second rule is what makes
         * `PATH=~/bin:~/tools` work, and without the first `x=~` stored a
         * literal tilde that every later use of $x carried around. */
        size_t klen = env_key_len(word);
        if (word[klen] == '=' && shell_name_is_valid(word, klen)) {
            char asg[SHELL_PARSE_BUF_MAX];
            size_t apos = 0;
            bool changed = false;
            const char *q = word;
            /* copy `name=` verbatim */
            for (size_t i = 0; i <= klen; i++)
                if (shell_append_char(asg, &apos, sizeof(asg), *q++) < 0) return -1;
            while (*q) {
                if (*q == '~') {
                    const char *seg_end = q;
                    while (*seg_end && *seg_end != ':') seg_end++;
                    char seg[VFS_PATH_MAX];
                    size_t sl = (size_t)(seg_end - q);
                    if (sl + 1 <= sizeof(seg)) {
                        memcpy(seg, q, sl);
                        seg[sl] = '\0';
                        char *sub = 0;
                        int r = shell_expand_tilde_word(pl, seg, &sub);
                        if (r < 0) return -1;
                        if (r > 0) {
                            if (shell_append_str(asg, &apos, sizeof(asg), sub) < 0) return -1;
                            q = seg_end;
                            changed = true;
                            continue;
                        }
                    }
                }
                /* Only a tilde that STARTS a segment expands. */
                while (*q && *q != ':')
                    if (shell_append_char(asg, &apos, sizeof(asg), *q++) < 0) return -1;
                if (*q == ':')
                    if (shell_append_char(asg, &apos, sizeof(asg), *q++) < 0) return -1;
            }
            if (changed) {
                asg[apos] = '\0';
                char *saved = 0;
                if (shell_pipeline_save_word(pl, asg, &saved) < 0) return -1;
                cur->argv[cur->argc++] = saved;
                return 0;
            }
        }
    }

    cur->argv[cur->argc++] = (char *)word;
    return 0;
}

/* The utilities whose arguments are variable assignments rather than words.
 * POSIX names export and readonly; the rest are the bash spellings tsh already
 * implements, and they follow the same rule. */
static bool shell_is_declaration_utility(const char *name) {
    if (!name) return false;
    return strcmp(name, "export")  == 0 || strcmp(name, "readonly") == 0 ||
           strcmp(name, "local")   == 0 || strcmp(name, "declare")  == 0 ||
           strcmp(name, "typeset") == 0;
}

static bool shell_is_ifs_char(char c) {
    const char *ifs = env_get("IFS");
    if (!ifs) ifs = " \t\n";
    for (; *ifs; ifs++) {
        if (*ifs == c) return true;
    }
    return false;
}

/* IFS whitespace, as POSIX means it: space/tab/newline, and only when they
 * are actually in IFS. With IFS=':' a space is an ordinary character. */
static bool shell_is_ifs_white(char c) {
    return (c == ' ' || c == '\t' || c == '\n') && shell_is_ifs_char(c);
}

static int shell_add_arg(struct shell_pipeline *pl, struct shell_simple *cur,
                         const char *word, bool quoted);
static int shell_add_arg_ex(struct shell_pipeline *pl, struct shell_simple *cur,
                            const char *word, bool quoted, bool expanded,
                            bool assign_src);

/* Split a word on the "$@" word-boundary marker and add each piece. Runs for
 * quoted words too -- that is the whole point, since `"$@"` must yield one
 * argument per parameter despite being quoted. Empty pieces are dropped, so
 * zero parameters contributes zero arguments.
 *
 * Returns 1 if the word carried markers (and has been fully handled), 0 if it
 * carried none and should take the normal path, -1 on error. */
static int shell_add_marked_words(struct shell_pipeline *pl,
                                  struct shell_simple *cur,
                                  const char *word, bool quoted) {
    const char *first = word;
    while (*first && *first != SHELL_ARG_MARK) first++;
    if (!*first) return 0;                       /* no marker: normal path */

    size_t prefix_len = (size_t)(first - word);
    char tmp[SHELL_PARSE_BUF_MAX];
    int emitted = 0;

    for (const char *p = first; *p == SHELL_ARG_MARK; ) {
        p++;
        const char *start = p;
        while (*p && *p != SHELL_ARG_MARK) p++;
        size_t n = (size_t)(p - start);

        /* Exactly one marker with NO REAL TEXT after it is the zero-parameter
         * encoding, not a parameter that happens to be empty. "Real" excludes
         * the tokenizer's quote marks: `"$@"` now arrives wrapped in a pair of
         * them, so testing the raw length saw one byte of trailing mark and
         * emitted an empty argument where bash emits none. */
        size_t real = 0;
        for (const char *q = start; q < p; q++)
            if (*q != SHELL_NOSPLIT_MARK) real++;
        size_t real_prefix = 0;
        for (size_t q = 0; q < prefix_len; q++)
            if (word[q] != SHELL_NOSPLIT_MARK) real_prefix++;
        if (real == 0 && emitted == 0 && real_prefix == 0) {
            const char *tail = p;
            while (*tail == SHELL_NOSPLIT_MARK) tail++;
            if (!*tail) break;
        }

        size_t glue = (emitted == 0) ? prefix_len : 0;
        if (glue + n + 1 > sizeof(tmp)) {
            kprintf("shell: field too long\n");
            return -1;
        }
        if (glue) memcpy(tmp, word, glue);
        memcpy(tmp + glue, start, n);
        tmp[glue + n] = '\0';

        /* The tokenizer's quote marks travel with the text; strip them here or
         * they reach the program as raw 0x02 bytes in argv. */
        char clean[SHELL_PARSE_BUF_MAX];
        shell_strip_nosplit(clean, sizeof clean, tmp);

        char *saved = 0;
        if (shell_pipeline_save_word(pl, clean, &saved) < 0) return -1;
        /* An unquoted "$@" still field-splits each parameter; a quoted one
         * keeps each parameter whole, spaces and all. */
        if (quoted) {
            if (shell_add_one_arg(pl, cur, saved, true) < 0) return -1;
        } else {
            if (shell_add_arg(pl, cur, saved, false) < 0) return -1;
        }
        emitted++;
    }

    /* `"pre$@"` with no parameters is still the word `pre` -- but a prefix
     * made only of quote marks is not a prefix at all. `"$@"` opens with one,
     * so this fired for the bare form and contributed an argument where bash
     * contributes none. */
    if (emitted == 0 && prefix_len > 0) {
        if (prefix_len + 1 > sizeof(tmp)) return -1;
        memcpy(tmp, word, prefix_len);
        tmp[prefix_len] = '\0';
        char pre[SHELL_PARSE_BUF_MAX];
        shell_strip_nosplit(pre, sizeof pre, tmp);
        if (pre[0]) {
            char *saved = 0;
            if (shell_pipeline_save_word(pl, pre, &saved) < 0) return -1;
            if (shell_add_one_arg(pl, cur, saved, quoted) < 0) return -1;
        }
    }
    return 1;
}

static int shell_add_arg_ex(struct shell_pipeline *pl, struct shell_simple *cur,
                            const char *word, bool quoted, bool expanded,
                            bool assign_src) {
    int mrc = shell_add_marked_words(pl, cur, word, quoted);
    if (mrc != 0) return mrc < 0 ? -1 : 0;

    /* A word quoted with no marks is `$'...'`, whose span the tokenizer does
     * not delimit; it keeps the old all-or-nothing behaviour. Everything else
     * lets the marks say which parts are protected. */
    if (quoted && !shell_has_nosplit(word))
        return shell_add_one_arg(pl, cur, word, true);

    /* Nothing expanded, so there is nothing to split. POSIX splits the
     * RESULTS of expansion, never the literal text of a word -- without
     * this, `IFS=o` turns `echo` into `ech`. */
    if (!expanded) {
        if (!shell_has_nosplit(word))
            return shell_add_one_arg(pl, cur, word, false);
        char lit[SHELL_PARSE_BUF_MAX];
        shell_strip_nosplit(lit, sizeof lit, word);
        char *litsaved = 0;
        if (shell_pipeline_save_word(pl, lit, &litsaved) < 0) return -1;
        return shell_add_one_arg(pl, cur, litsaved, quoted);
    }

    /* An assignment's value is expanded but NOT field split -- POSIX 2.9.1.
     * That covers the prefix form `x=$words cmd` (argc == 0) and, because the
     * declaration utilities take assignments as ordinary-looking arguments,
     * `export x=$words` too. Without the second half, `export ex='a b c'`
     * reached the builtin as three arguments: it assigned the first and then
     * created two stray variables named after the remaining fields. */
    size_t klen = env_key_len(word);
    bool assign_shaped = (word[klen] == '=' && shell_name_is_valid(word, klen));
    if (assign_shaped &&
        (cur->argc == 0 ||
         (assign_src && shell_is_declaration_utility(cur->argv[0])))) {
        return shell_add_one_arg(pl, cur, word, false);
    }

    /* IFS splitting, POSIX rules -- and the two rules are different.
     *
     * A run of IFS WHITESPACE is a single delimiter, and leading/trailing
     * runs produce no fields, so "  x   y  " is two fields. But each IFS
     * NON-whitespace character delimits on its own, so with IFS=':' the
     * string "a::b" is THREE fields, the middle one empty. Collapsing both
     * kinds (which is what the loop here used to do) silently drops empty
     * fields, and `IFS=: read a b c` is exactly how scripts parse
     * /etc/passwd-shaped data. */
    /* `prot` tracks the SHELL_NOSPLIT_MARK toggle: while it is on, no
     * character is a delimiter, which is what keeps `${x:-'a b c'}` one word. */
    bool prot = false;
    const char *p = word;
    while (!prot && shell_is_ifs_char(*p) && shell_is_ifs_white(*p)) p++;

    while (*p) {
        const char *start = p;
        while (*p) {
            if (*p == SHELL_NOSPLIT_MARK) { prot = !prot; p++; continue; }
            if (!prot && shell_is_ifs_char(*p)) break;
            p++;
        }
        size_t n = (size_t)(p - start);
        char raw[SHELL_PARSE_BUF_MAX];
        char tmp[SHELL_PARSE_BUF_MAX];
        if (n + 1 > sizeof(raw)) {
            kprintf("shell: field too long\n");
            return -1;
        }
        memcpy(raw, start, n);
        raw[n] = '\0';
        shell_strip_nosplit(tmp, sizeof tmp, raw);
        char *saved = 0;
        if (shell_pipeline_save_word(pl, tmp, &saved) < 0) return -1;
        /* The WORD's flag, not the field's: globbing behaviour is left exactly
         * as it was, so this change is about splitting only. */
        if (shell_add_one_arg(pl, cur, saved, quoted) < 0) return -1;

        if (!*p) break;
        /* One delimiter: optional whitespace, at most one non-whitespace
         * IFS character, optional whitespace. */
        while (shell_is_ifs_char(*p) && shell_is_ifs_white(*p)) p++;
        if (shell_is_ifs_char(*p) && !shell_is_ifs_white(*p)) {
            p++;
            while (shell_is_ifs_char(*p) && shell_is_ifs_white(*p)) p++;
        }
        /* A trailing delimiter does not introduce an empty final field. */
        if (!*p) break;
    }

    /* An unquoted expansion that came out empty contributes NO word. bash:
     * `x=""; f $x` passes nothing, while `f ""` passes one empty argument --
     * and the quoted form never reaches here. */
    return 0;
}

/* Compatibility entry for callers that have no token to ask. The "$@"
 * re-entry below is one: its words ARE expansion results by construction. */
static int shell_add_arg(struct shell_pipeline *pl, struct shell_simple *cur,
                         const char *word, bool quoted) {
    return shell_add_arg_ex(pl, cur, word, quoted, true, false);
}

static int shell_default_redir_fd(enum shell_tok_type t, int explicit_fd) {
    if (explicit_fd >= 0) return explicit_fd;
    if (t == SH_TOK_REDIR_IN || t == SH_TOK_HEREDOC ||
        t == SH_TOK_HEREDOC_TABS || t == SH_TOK_DUP_IN) {
        return 0;
    }
    return 1;
}

static int shell_parse_fd_word(const char *word, int *out_fd) {
    if (!word || !*word || !out_fd) return -1;
    int v = 0;
    for (const char *p = word; *p; p++) {
        if (!shell_is_digit(*p)) return -1;
        v = v * 10 + (*p - '0');
        if (v >= SHELL_FD_MAX) return -1;
    }
    *out_fd = v;
    return 0;
}

static int shell_add_redir(struct shell_simple *cur, enum shell_redir_op op,
                           int fd, const char *path, const char *text,
                           int target_fd) {
    if (fd < 0 || fd >= SHELL_FD_MAX) {
        kprintf("shell: only file descriptors 0, 1, and 2 are supported\n");
        return -1;
    }
    if (cur->redir_count >= SHELL_REDIR_MAX) {
        kprintf("shell: too many redirections\n");
        return -1;
    }
    struct shell_redir *r = &cur->redir[cur->redir_count++];
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->fd = fd;
    r->target_fd = target_fd;
    r->path = path;
    r->text = text;
    return 0;
}

static int shell_parse_pipeline(struct shell_token *tok, int ntok, int *io,
                                struct shell_pipeline *pl) {
    memset(pl, 0, sizeof(*pl));
    if (*io >= ntok || shell_is_list_sep(tok[*io].type)) return 0;

    shell_simple_init(&pl->stage[0]);
    pl->count = 1;

    while (*io < ntok && !shell_is_list_sep(tok[*io].type)) {
        enum shell_tok_type t = tok[*io].type;
        struct shell_simple *cur = &pl->stage[pl->count - 1];

        if (t == SH_TOK_PIPE) {
            if (cur->argc == 0) {
                kprintf("shell: empty command before '|'\n");
                return -1;
            }
            (*io)++;
            if (pl->count >= SHELL_STAGE_MAX) {
                kprintf("shell: too many pipeline stages\n");
                return -1;
            }
            shell_simple_init(&pl->stage[pl->count++]);
            continue;
        }

        if (t == SH_TOK_REDIR_IN || t == SH_TOK_HEREDOC ||
            t == SH_TOK_HEREDOC_TABS || t == SH_TOK_REDIR_OUT ||
            t == SH_TOK_REDIR_APPEND || t == SH_TOK_REDIR_CLOBBER ||
            t == SH_TOK_DUP_IN || t == SH_TOK_DUP_OUT) {
            int fd = shell_default_redir_fd(t, tok[*io].fd);
            (*io)++;
            if (*io >= ntok || tok[*io].type != SH_TOK_WORD) {
                kprintf("shell: redirection needs a path\n");
                return -1;
            }
            /* A redirect operand is a FILENAME, never a field to split, so the
             * tokenizer's quote marks have to come off before anyone opens it.
             * `> "$f"` otherwise tried to create a file whose name began with
             * a 0x02 byte. */
            shell_strip_nosplit_inplace(tok[*io].text);
            if (t == SH_TOK_REDIR_IN) {
                cur->stdin_path = tok[*io].text;
                if (shell_add_redir(cur, SH_RD_OPEN_IN, fd, tok[*io].text,
                                    0, -1) < 0) return -1;
            } else if (t == SH_TOK_HEREDOC || t == SH_TOK_HEREDOC_TABS) {
                const struct shell_heredoc *h = shell_heredoc_pop();
                if (!h) {
                    kprintf("shell: here-document body missing\n");
                    return -1;
                }
                cur->stdin_text = h->body;
                if (shell_add_redir(cur, SH_RD_HEREDOC, fd, 0, h->body,
                                    -1) < 0) return -1;
            } else if (t == SH_TOK_DUP_IN || t == SH_TOK_DUP_OUT) {
                int target = -1;
                if (strcmp(tok[*io].text, "-") == 0) {
                    if (shell_add_redir(cur, SH_RD_CLOSE, fd, 0, 0,
                                        -1) < 0) return -1;
                } else if (shell_parse_fd_word(tok[*io].text, &target) < 0) {
                    kprintf("shell: bad file descriptor '%s'\n", tok[*io].text);
                    return -1;
                } else {
                    if (shell_add_redir(cur,
                                        t == SH_TOK_DUP_IN ? SH_RD_DUP_IN
                                                           : SH_RD_DUP_OUT,
                                        fd, 0, 0, target) < 0) return -1;
                }
            } else {
                cur->stdout_path = tok[*io].text;
                cur->stdout_append = (t == SH_TOK_REDIR_APPEND);
                enum shell_redir_op rk;
                if (t == SH_TOK_REDIR_APPEND) rk = SH_RD_OPEN_APPEND;
                else if (t == SH_TOK_REDIR_CLOBBER) rk = SH_RD_OPEN_OUT;
                else rk = SH_RD_OPEN_OUT;
                if (t == SH_TOK_REDIR_OUT && g_opt_noclobber) {
                    rk = SH_RD_OPEN_EXCL;
                }
                if (shell_add_redir(cur, rk, fd, tok[*io].text, 0, -1) < 0) return -1;
            }
            (*io)++;
            continue;
        }

        if (t != SH_TOK_WORD) {
            kprintf("shell: unexpected token\n");
            return -1;
        }
        if (shell_add_arg_ex(pl, cur, tok[*io].text, tok[*io].quoted,
                             tok[*io].expanded, tok[*io].assign_src) < 0) {
            return -1;
        }
        (*io)++;
    }

    for (int i = 0; i < pl->count; i++) {
        if (pl->stage[i].argc == 0) {
            if (pl->count == 1 && pl->stage[i].redir_count > 0) continue;
            kprintf("shell: empty command in pipeline\n");
            return -1;
        }
    }
    return pl->count;
}

static bool shell_is_assignment_word(const char *s) {
    if (!s) return false;
    size_t klen = env_key_len(s);
    return s[klen] == '=' && shell_name_is_valid(s, klen);
}

static bool shell_assignment_overrides(const char *kv, char **assignv,
                                       int assignc) {
    size_t klen = env_key_len(kv);
    for (int i = 0; i < assignc; i++) {
        size_t alen = env_key_len(assignv[i]);
        if (alen == klen && memcmp(kv, assignv[i], klen) == 0) return true;
    }
    return false;
}

/* Build the environment a child process receives.
 *
 * ONLY EXPORTED VARIABLES GO. This is the point of the export flag: before it
 * existed this loop copied the entire shell variable table, so
 *     x=1; printenv x
 * printed 1 where POSIX, bash and dash all print nothing. `assignv` carries
 * the one-shot `VAR=v cmd` prefix assignments, which ARE in the child's
 * environment by definition regardless of the export attribute. */
static int shell_build_env_overlay(char **assignv, int assignc,
                                   char **out_env, int *out_envc) {
    int n = 0;
    for (int i = 0; i < g_envc; i++) {
        if (!(g_env_flags[i] & SHVAR_EXPORTED)) continue;
        if (!shell_assignment_overrides(g_env[i], assignv, assignc)) {
            if (n >= ENV_MAX + ARG_MAX) return -1;
            out_env[n++] = g_env[i];
        }
    }
    for (int i = 0; i < assignc; i++) {
        if (n >= ENV_MAX + ARG_MAX) return -1;
        out_env[n++] = assignv[i];
    }
    out_env[n] = 0;
    *out_envc = n;
    return 0;
}

struct shell_env_frame {
    char *env[ENV_MAX + 1];
    unsigned char flags[ENV_MAX];
    int envc;
    int getopts_last_optind;
    int getopts_char_index;
};

static void shell_env_frame_clear(struct shell_env_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < frame->envc; i++) {
        if (frame->env[i]) kfree(frame->env[i]);
        frame->env[i] = 0;
    }
    frame->envc = 0;
}

static int shell_env_frame_capture(struct shell_env_frame *frame) {
    if (!frame) return -1;
    memset(frame, 0, sizeof(*frame));
    for (int i = 0; i < g_envc; i++) {
        frame->env[i] = shell_strdup(g_env[i]);
        if (!frame->env[i]) {
            frame->envc = i;
            shell_env_frame_clear(frame);
            return -1;
        }
        /* The export attribute is part of the variable, so a subshell that
         * exports something must not leak that attribute back out. */
        frame->flags[i] = g_env_flags[i];
    }
    frame->envc = g_envc;
    frame->env[g_envc] = 0;
    frame->getopts_last_optind = g_getopts_last_optind;
    frame->getopts_char_index = g_getopts_char_index;
    return 0;
}

static void shell_env_frame_restore(struct shell_env_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < g_envc; i++) {
        if (g_env[i]) kfree(g_env[i]);
        g_env[i] = 0;
    }
    g_envc = frame->envc;
    for (int i = 0; i < frame->envc; i++) {
        g_env[i] = frame->env[i];
        g_env_flags[i] = frame->flags[i];
        frame->env[i] = 0;
    }
    g_env[g_envc] = 0;
    g_getopts_last_optind = frame->getopts_last_optind;
    g_getopts_char_index = frame->getopts_char_index;
    frame->envc = 0;
}

static int shell_apply_assignments(char **assignv, int assignc,
                                   const char *label) {
    int rc = 0;
    for (int i = 0; i < assignc; i++) {
        if (env_set_kv(assignv[i]) < 0) {
            kprintf("%s: failed to set '%s'\n",
                    label ? label : "shell", assignv[i]);
            rc = 1;
        }
    }
    return rc;
}

static struct file *shell_open_vfs_file(const char *path_arg, bool write,
                                        bool append, const char *label) {
    char path[VFS_PATH_MAX];
    if (shell_resolve_path_arg(path_arg, path, sizeof(path), label) < 0) return 0;

    if (write) {
        struct vfs_stat st;
        int sr = vfs_stat(path, &st);
        if (sr == VFS_OK && st.type == VFS_TYPE_DIR) {
            kprintf("%s: '%s': is a directory\n", label, path_arg);
            return 0;
        }
        if (append) {
            if (sr == VFS_ERR_NOENT) {
                int cr = vfs_create(path);
                if (cr != VFS_OK && cr != VFS_ERR_EXIST) {
                    kprintf("%s: '%s': %s\n", label, path_arg, vfs_strerror(cr));
                    return 0;
                }
            } else if (sr != VFS_OK) {
                kprintf("%s: '%s': %s\n", label, path_arg, vfs_strerror(sr));
                return 0;
            }
        } else {
            if (sr == VFS_OK) {
                int ur = vfs_unlink(path);
                if (ur != VFS_OK) {
                    kprintf("%s: '%s': %s\n", label, path_arg, vfs_strerror(ur));
                    return 0;
                }
            } else if (sr != VFS_ERR_NOENT) {
                kprintf("%s: '%s': %s\n", label, path_arg, vfs_strerror(sr));
                return 0;
            }
            int cr = vfs_create(path);
            if (cr != VFS_OK && cr != VFS_ERR_EXIST) {
                kprintf("%s: '%s': %s\n", label, path_arg, vfs_strerror(cr));
                return 0;
            }
        }
    }

    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_VFS;
    {
        struct vfs_ofd *ofd = (struct vfs_ofd *)kmalloc(sizeof *ofd);
        if (!ofd) {
            kfree(f);
            return 0;
        }
        ofd->refs = 1;
        ofd->pos  = 0;
        f->vfs_refs = &ofd->refs;   /* refs is first; see struct vfs_ofd */
    }

    int rc = vfs_open(path, &f->vfs);
    if (rc != VFS_OK) {
        kprintf("%s: '%s': %s\n", label, path_arg, vfs_strerror(rc));
        kfree(f->vfs_refs);
        kfree(f);
        return 0;
    }
    if (write && append) file_pos_set(f, f->vfs.size);
    return f;
}

static struct file *shell_open_text_pipe(const char *text, const char *label) {
    struct file *r = 0;
    struct file *w = 0;
    if (pipe_create(&r, &w) != 0) {
        kprintf("%s: failed to create here-document pipe\n", label);
        return 0;
    }

    size_t len = text ? strlen(text) : 0;
    if (len > 0) {
        long n = file_write(w, text, len);
        if (n < 0 || (size_t)n != len) {
            kprintf("%s: failed to write here-document\n", label);
            file_close(r);
            file_close(w);
            return 0;
        }
    }
    file_close(w);
    return r;
}

struct shell_fd_state {
    struct file *fd[SHELL_FD_MAX];
    bool owned[SHELL_FD_MAX];
};

struct shell_io_frame {
    struct file *old_fd[SHELL_FD_MAX];
    bool owned[SHELL_FD_MAX];
};

static void shell_fd_state_close_owned(struct shell_fd_state *st) {
    if (!st) return;
    for (int i = 0; i < SHELL_FD_MAX; i++) {
        if (st->owned[i]) file_close(st->fd[i]);
        st->fd[i] = 0;
        st->owned[i] = false;
    }
}

static struct file *shell_closed_file_make(void) {
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_NULL;
    return f;
}

static int shell_fd_state_replace(struct shell_fd_state *st, int fd,
                                  struct file *f, bool owned) {
    if (!st || fd < 0 || fd >= SHELL_FD_MAX) return -1;
    if (st->owned[fd]) file_close(st->fd[fd]);
    st->fd[fd] = f;
    st->owned[fd] = owned;
    return 0;
}

static void shell_fd_state_init_from_defaults(struct shell_fd_state *st) {
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < SHELL_FD_MAX; i++) {
        st->fd[i] = g_shell_fd[i];
        st->owned[i] = false;
    }
}

static int shell_apply_redirs(struct shell_simple *cmd,
                              struct shell_fd_state *st,
                              const char *label) {
    if (!cmd || !st) return -1;
    for (int i = 0; i < cmd->redir_count; i++) {
        struct shell_redir *r = &cmd->redir[i];
        if (r->fd < 0 || r->fd >= SHELL_FD_MAX) {
            kprintf("%s: bad file descriptor %d\n", label, r->fd);
            return -1;
        }

        if (r->op == SH_RD_OPEN_IN) {
            struct file *f = shell_open_vfs_file(r->path, false, false, label);
            if (!f) return -1;
            if (shell_fd_state_replace(st, r->fd, f, true) < 0) {
                file_close(f);
                return -1;
            }
            continue;
        }
        if (r->op == SH_RD_HEREDOC) {
            struct file *f = shell_open_text_pipe(r->text, label);
            if (!f) return -1;
            if (shell_fd_state_replace(st, r->fd, f, true) < 0) {
                file_close(f);
                return -1;
            }
            continue;
        }
        if (r->op == SH_RD_OPEN_OUT || r->op == SH_RD_OPEN_APPEND ||
            r->op == SH_RD_OPEN_EXCL) {
            if (r->op == SH_RD_OPEN_EXCL) {
                char resolved[VFS_PATH_MAX];
                if (shell_canonicalize_path(r->path, resolved,
                                            sizeof(resolved)) >= 0) {
                    struct vfs_stat st2;
                    if (vfs_stat(resolved, &st2) == VFS_OK) {
                        kprintf("%s: cannot overwrite existing file\n", r->path);
                        shell_set_status(1);
                        return -1;
                    }
                }
            }
            struct file *f = shell_open_vfs_file(r->path, true,
                                                 r->op == SH_RD_OPEN_APPEND,
                                                 label);
            if (!f) return -1;
            if (shell_fd_state_replace(st, r->fd, f, true) < 0) {
                file_close(f);
                return -1;
            }
            continue;
        }
        if (r->op == SH_RD_CLOSE) {
            struct file *f = shell_closed_file_make();
            if (!f) return -1;
            if (shell_fd_state_replace(st, r->fd, f, true) < 0) {
                file_close(f);
                return -1;
            }
            continue;
        }
        if (r->op == SH_RD_DUP_IN || r->op == SH_RD_DUP_OUT) {
            if (r->target_fd < 0 || r->target_fd >= SHELL_FD_MAX) {
                kprintf("%s: bad file descriptor %d\n", label, r->target_fd);
                return -1;
            }
            struct file *clone = 0;
            bool owned = false;
            /* A NULL slot means "the shell's own descriptor", not "no
             * file". Cloning NULL used to produce NULL, which reads as
             * the default again -- so `1>&2` quietly did nothing. Ask
             * the host for a real handle on that standard descriptor. */
            struct file *target = st->fd[r->target_fd];
            struct file *synth = 0;
            if (!target) {
                synth = file_std_handle(r->target_fd);
                target = synth;
            }
            if (target) {
                clone = file_clone(target);
                if (synth) file_close(synth);
                if (!clone) {
                    kprintf("%s: failed to duplicate fd %d\n",
                            label, r->target_fd);
                    return -1;
                }
                owned = true;
            }
            if (shell_fd_state_replace(st, r->fd, clone, owned) < 0) {
                file_close(clone);
                return -1;
            }
            continue;
        }
    }
    return 0;
}

static int shell_enter_io_frame(struct shell_simple *cmd, const char *label,
                                struct shell_io_frame *frame) {
    if (!cmd || !frame) return -1;
    memset(frame, 0, sizeof(*frame));

    struct shell_fd_state fds;
    shell_fd_state_init_from_defaults(&fds);
    if (shell_apply_redirs(cmd, &fds, label) < 0) {
        shell_fd_state_close_owned(&fds);
        return -1;
    }

    for (int i = 0; i < SHELL_FD_MAX; i++) {
        frame->old_fd[i] = g_shell_fd[i];
        frame->owned[i] = fds.owned[i];
        g_shell_fd[i] = fds.fd[i];
        fds.owned[i] = false;
    }
    return 0;
}

static void shell_restore_io_frame(struct shell_io_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < SHELL_FD_MAX; i++) {
        if (frame->owned[i]) file_close(g_shell_fd[i]);
        g_shell_fd[i] = frame->old_fd[i];
        frame->old_fd[i] = 0;
        frame->owned[i] = false;
    }
}

static void shell_file_output(const char *s, void *ctx) {
    struct file *f = (struct file *)ctx;
    if (s && f) (void)file_write(f, s, strlen(s));
}

static void shell_file_kputc(void *ctx, char c) {
    struct file *f = (struct file *)ctx;
    if (f) (void)file_write(f, &c, 1);
}

struct shell_printk_bridge {
    shell_write_fn_t fn;
    void *ctx;
};

static void shell_printk_bridge_char(void *ctx, char c) {
    struct shell_printk_bridge *b = (struct shell_printk_bridge *)ctx;
    if (!b || !b->fn) return;
    char s[2] = { c, '\0' };
    b->fn(s, b->ctx);
}

/* Roll back ONLY the variables a command prefix assigned.
 *
 * A prefix on a non-special builtin applies to that command alone, so it has
 * to be undone afterwards -- but snapshotting the whole environment and
 * restoring it also undoes what the command itself did. `IFS=: read a b`
 * therefore read the line, assigned a and b, and then had both assignments
 * rolled back with IFS: the variables kept their previous values and the
 * builtin looked like it had never run. Save just the prefixed names. */
#define SHELL_PREFIX_MAX 16

struct shell_prefix_save {
    char  name[SHELL_PREFIX_MAX][64];
    char *prev[SHELL_PREFIX_MAX];   /* strdup of the old value, or NULL */
    bool  had[SHELL_PREFIX_MAX];
    int   count;
};

static void shell_prefix_capture(struct shell_prefix_save *sv,
                                 char **assignv, int assignc) {
    sv->count = 0;
    for (int i = 0; i < assignc && sv->count < SHELL_PREFIX_MAX; i++) {
        size_t klen = env_key_len(assignv[i]);
        if (klen == 0 || klen + 1 > sizeof(sv->name[0])) continue;
        int k = sv->count++;
        memcpy(sv->name[k], assignv[i], klen);
        sv->name[k][klen] = '\0';
        const char *old = env_get(sv->name[k]);
        sv->had[k]  = (old != 0);
        sv->prev[k] = old ? shell_strdup(old) : 0;
    }
}

static void shell_prefix_restore(struct shell_prefix_save *sv) {
    for (int i = 0; i < sv->count; i++) {
        if (sv->had[i] && sv->prev[i]) env_set(sv->name[i], sv->prev[i]);
        else if (!sv->had[i])          env_unset(sv->name[i]);
        if (sv->prev[i]) kfree(sv->prev[i]);
        sv->prev[i] = 0;
    }
    sv->count = 0;
}

static int shell_run_builtin(struct shell_simple *cmd, const struct cmd *c,
                             char **assignv, int assignc,
                             bool persist_assignments) {
    if (!c) c = shell_cmd_lookup(cmd->argv[0]);
    if (!c) return -1;

    struct shell_fd_state fds;
    memset(&fds, 0, sizeof(fds));
    shell_write_fn_t old_fn = g_shell_out;
    void *old_ctx = g_shell_out_ctx;
    struct file *old_in = g_shell_in;
    void (*old_sink)(void *ctx, char c) = 0;
    void *old_sink_ctx = 0;
    bool old_sink_suppress = false;
    bool sink_active = false;
    struct shell_printk_bridge bridge;
    struct shell_prefix_save prefix_save;
    bool restore_env = false;

    shell_fd_state_init_from_defaults(&fds);
    if (shell_apply_redirs(cmd, &fds, cmd->argv[0]) < 0) {
        shell_fd_state_close_owned(&fds);
        return 1;
    }
    if (assignc > 0 && !persist_assignments) {
        shell_prefix_capture(&prefix_save, assignv, assignc);
        restore_env = true;
    }
    if (assignc > 0 &&
        shell_apply_assignments(assignv, assignc, cmd->argv[0]) != 0) {
        if (restore_env) shell_prefix_restore(&prefix_save);
        shell_fd_state_close_owned(&fds);
        return 1;
    }
    if (fds.fd[1]) {
        shell_set_output(shell_file_output, fds.fd[1]);
        printk_get_sink(&old_sink, &old_sink_ctx, &old_sink_suppress);
        printk_set_sink_mode(shell_file_kputc, fds.fd[1], true);
        sink_active = true;
    } else if (g_shell_out) {
        bridge.fn = g_shell_out;
        bridge.ctx = g_shell_out_ctx;
        printk_get_sink(&old_sink, &old_sink_ctx, &old_sink_suppress);
        printk_set_sink_mode(shell_printk_bridge_char, &bridge,
            old_sink_suppress);
        sink_active = true;
    }
    g_shell_in = fds.fd[0];
    shell_set_status(0);
    c->fn(cmd->argc, cmd->argv);
    int rc = g_last_status;
    g_shell_in = old_in;
    if (restore_env) {
        shell_prefix_restore(&prefix_save);
        shell_set_status(rc);
    }
    if (sink_active) {
        printk_set_sink_mode(old_sink, old_sink_ctx, old_sink_suppress);
    }
    if (fds.fd[1]) {
        shell_set_output(old_fn, old_ctx);
    }
    shell_fd_state_close_owned(&fds);
    return rc;
}

static int shell_run_function(struct shell_simple *cmd) {
    struct shell_function *fn = shell_function_lookup(cmd->argv[0]);
    if (!fn) return -1;
    if (g_script_depth >= 8) {
        kprintf("%s: function nesting too deep\n", cmd->argv[0]);
        return 2;
    }

    struct shell_io_frame io_frame;
    bool io_active = false;
    if (cmd->redir_count > 0) {
        if (shell_enter_io_frame(cmd, cmd->argv[0], &io_frame) < 0) {
            return 1;
        }
        io_active = true;
    }

    struct shell_param_frame frame;
    if (shell_enter_script_params(&frame, cmd->argv[0],
                                  cmd->argc - 1, &cmd->argv[1]) < 0) {
        kprintf("%s: failed to set function parameters\n", cmd->argv[0]);
        if (io_active) shell_restore_io_frame(&io_frame);
        return 1;
    }

    g_script_depth++;
    int local_depth = g_script_depth;
    execute_line_text(fn->body);
    int rc = g_last_status;
    if (g_shell_flow == SHELL_FLOW_RETURN) {
        rc = g_shell_flow_status;
        g_shell_flow = SHELL_FLOW_NONE;
        g_shell_flow_status = 0;
    }
    /* Before the depth drops: undo anything this frame localised. `return`
     * from the middle of a function reaches here too, which is the whole
     * reason the unwind is here rather than at the end of the body. */
    shell_locals_pop(local_depth);
    g_script_depth--;
    shell_restore_params_from_frame(&frame);
    if (io_active) shell_restore_io_frame(&io_frame);
    return rc;
}

#ifdef SHELL_HOSTED
/* Run `text` in a forked copy of this shell, in the background.
 *
 * This is how a shell backgrounds anything it cannot hand to exec: a builtin,
 * a function, a brace group. The child inherits the whole shell state by
 * virtue of being a copy, runs the text, and _exits with its status; the
 * parent records the job so `wait`, `wait $!` and `wait %1` can find it.
 *
 * Hosted only. The kernel shell is a kernel thread and forking it would not
 * mean the same thing, so it keeps refusing -- loudly, as before. */
/* pid_t is not in scope here -- this file speaks the kernel headers.
 * libtoby's pid_t is int, so the declaration is ABI-identical. */
extern int fork(void);
extern void _exit(int status);

static int shell_background_forked(const char *text, const char *label) {
    int pid = fork();
    if (pid < 0) {
        kprintf("%s: cannot fork to background\n", label ? label : "shell");
        return 1;
    }
    if (pid == 0) {
        execute_line_text(text);
        int st = g_last_status;
        if (g_shell_flow == SHELL_FLOW_EXIT) st = g_shell_flow_status;
        _exit(st & 0xff);
    }
    g_last_bg_pid = (int)pid;
    jobs_add((int)pid, label ? label : text);
    return 0;
}
#endif

static int shell_run_single(struct shell_simple *cmd, bool background) {
    int assignc = 0;
    while (assignc < cmd->argc && shell_is_assignment_word(cmd->argv[assignc])) {
        assignc++;
    }

    if (assignc == cmd->argc) {
        struct shell_fd_state fds;
        shell_fd_state_init_from_defaults(&fds);
        if (shell_apply_redirs(cmd, &fds, "shell") < 0) {
            shell_fd_state_close_owned(&fds);
            return 1;
        }
        shell_fd_state_close_owned(&fds);
        int rc = shell_apply_assignments(cmd->argv, assignc, "shell");
        return rc;
    }

    struct shell_simple exec_cmd = *cmd;
    exec_cmd.argc = cmd->argc - assignc;
    for (int i = 0; i < exec_cmd.argc; i++) {
        exec_cmd.argv[i] = cmd->argv[i + assignc];
    }

    if (!background) {
        /* `exec` with redirections and NO command applies them to the SHELL
         * and returns -- that is how a script sends everything that follows to
         * a log file. cmd_exec() only knew how to replace the shell with a
         * program, so a bare `exec > file` did nothing at all and the output
         * kept going to the terminal.
         *
         * The handles are installed permanently and deliberately not owned by
         * an io frame: there is nothing to restore them to. */
        if (exec_cmd.argc == 1 && strcmp(exec_cmd.argv[0], "exec") == 0 &&
            cmd->redir_count > 0) {
            struct shell_fd_state fds;
            shell_fd_state_init_from_defaults(&fds);
            if (shell_apply_redirs(cmd, &fds, "exec") < 0) {
                shell_fd_state_close_owned(&fds);
                return 1;
            }
            for (int i = 0; i < SHELL_FD_MAX; i++) {
                g_shell_fd[i] = fds.fd[i];
                fds.owned[i] = false;   /* the shell keeps them now */
            }
            return 0;
        }

        const struct cmd *special = shell_cmd_lookup(exec_cmd.argv[0]);
        if (special && shell_special_builtin_name(special->name)) {
            return shell_run_builtin(&exec_cmd, special, cmd->argv, assignc,
                                     true);
        }
        /* A variable assignment PREFIXED to a command applies to that command
         * only -- `PREF=x show` must be visible inside `show` and gone after.
         * The builtin path below already threaded assignv/assignc through;
         * the function path did not, so the assignment was silently dropped
         * and the function saw the outer (or unset) value. */
        {
            struct shell_env_frame env_frame;
            bool restore = false;
            /* Only when there IS a prefix. A function shares the caller's
             * environment, so `f() { g=1; }; f` must leave g set -- snapshotting
             * around every call would roll that back. */
            if (assignc > 0 && shell_function_lookup(exec_cmd.argv[0])) {
                if (shell_env_frame_capture(&env_frame) == 0) restore = true;
                if (shell_apply_assignments(cmd->argv, assignc, "shell") != 0) {
                    if (restore) shell_env_frame_restore(&env_frame);
                    return 1;
                }
            }
            int frc = shell_run_function(&exec_cmd);
            if (restore) shell_env_frame_restore(&env_frame);
            if (frc >= 0) return frc;
        }
        const struct cmd *builtin = shell_cmd_lookup(exec_cmd.argv[0]);
        int brc = shell_run_builtin(&exec_cmd, builtin, cmd->argv, assignc,
                                    false);
        if (brc >= 0) return brc;
    } else {
        bool is_fn = shell_function_lookup(exec_cmd.argv[0]) != 0;
        bool is_bi = shell_cmd_lookup(exec_cmd.argv[0]) != 0;
        if (is_fn || is_bi) {
#ifdef SHELL_HOSTED
            /* Fork and run it there -- see shell_background_forked. Rebuild
             * the command text from argv: the caller has already split it,
             * and the child re-parses, which keeps this to one mechanism. */
            char line[SHELL_PARSE_BUF_MAX];
            size_t o = 0;
            for (int i = 0; i < exec_cmd.argc; i++) {
                size_t n = strlen(exec_cmd.argv[i]);
                if (o + n + 2 >= sizeof line) break;
                if (o) line[o++] = ' ';
                memcpy(line + o, exec_cmd.argv[i], n);
                o += n;
            }
            line[o] = '\0';
            return shell_background_forked(line, exec_cmd.argv[0]);
#else
            kprintf("'%s': %s can't be backgrounded with '&'\n",
                    exec_cmd.argv[0], is_fn ? "shell functions" : "builtins");
            return 1;
#endif
        }
    }

    char *env_overlay[ENV_MAX + ARG_MAX + 1];
    char **envp = g_env;
    int envc = g_envc;
    if (assignc > 0) {
        if (shell_build_env_overlay(cmd->argv, assignc,
                                    env_overlay, &envc) < 0) {
            kprintf("shell: environment too large\n");
            return 1;
        }
        envp = env_overlay;
    }

    struct shell_fd_state fds;
    shell_fd_state_init_from_defaults(&fds);
    if (shell_apply_redirs(&exec_cmd, &fds, exec_cmd.argv[0]) < 0) {
        shell_fd_state_close_owned(&fds);
        return 1;
    }

    int rc = shell_spawn_program_profile_fds(exec_cmd.argv[0], exec_cmd.argc,
                                             exec_cmd.argv, background,
                                             /*profile=*/0,
                                             fds.fd[0], fds.fd[1], fds.fd[2],
                                             envc, envp);
    shell_fd_state_close_owned(&fds);
    return rc;
}

static int shell_run_pipeline_shell_stage(struct shell_simple *cmd,
                                          struct file *pipe_in,
                                          struct file *pipe_out) {
    int assignc = 0;
    while (assignc < cmd->argc && shell_is_assignment_word(cmd->argv[assignc])) {
        assignc++;
    }
    if (assignc == cmd->argc) return -1;

    struct shell_simple exec_cmd = *cmd;
    exec_cmd.argc = cmd->argc - assignc;
    for (int i = 0; i < exec_cmd.argc; i++) {
        exec_cmd.argv[i] = cmd->argv[i + assignc];
    }

    const struct cmd *builtin = shell_cmd_lookup(exec_cmd.argv[0]);
    struct shell_function *fn = shell_function_lookup(exec_cmd.argv[0]);
    if (!builtin && !fn) return -1;

    struct shell_env_frame env_frame;
    if (shell_env_frame_capture(&env_frame) < 0) {
        kprintf("pipeline: failed to save shell environment\n");
        return 1;
    }

    char saved_cwd[VFS_PATH_MAX];
    const char *cwd = shell_cwd();
    if (!cwd || strlen(cwd) + 1 > sizeof(saved_cwd)) {
        shell_env_frame_restore(&env_frame);
        return 1;
    }
    memcpy(saved_cwd, cwd, strlen(cwd) + 1);

    enum shell_flow saved_flow = g_shell_flow;
    int saved_flow_status = g_shell_flow_status;
    int saved_loop_depth = g_shell_loop_depth;

    struct shell_fd_state fds;
    shell_fd_state_init_from_defaults(&fds);
    if (pipe_in && shell_fd_state_replace(&fds, 0, pipe_in, false) < 0) {
        shell_fd_state_close_owned(&fds);
        shell_env_frame_restore(&env_frame);
        return 1;
    }
    if (pipe_out && shell_fd_state_replace(&fds, 1, pipe_out, false) < 0) {
        shell_fd_state_close_owned(&fds);
        shell_env_frame_restore(&env_frame);
        return 1;
    }
    if (shell_apply_redirs(&exec_cmd, &fds, exec_cmd.argv[0]) < 0) {
        shell_fd_state_close_owned(&fds);
        shell_env_frame_restore(&env_frame);
        return 1;
    }

    struct shell_io_frame io_frame;
    memset(&io_frame, 0, sizeof(io_frame));
    for (int i = 0; i < SHELL_FD_MAX; i++) {
        io_frame.old_fd[i] = g_shell_fd[i];
        io_frame.owned[i] = fds.owned[i];
        g_shell_fd[i] = fds.fd[i];
        fds.owned[i] = false;
    }

    exec_cmd.redir_count = 0;
    exec_cmd.stdin_path = 0;
    exec_cmd.stdin_text = 0;
    exec_cmd.stdout_path = 0;
    exec_cmd.stdout_append = false;

    g_subshell_depth++;
    g_shell_flow = SHELL_FLOW_NONE;
    g_shell_flow_status = 0;
    g_shell_loop_depth = 0;

    int rc = 1;
    if (fn) {
        rc = shell_run_function(&exec_cmd);
    } else {
        rc = shell_run_builtin(&exec_cmd, builtin, cmd->argv, assignc, false);
    }
    if (g_shell_flow == SHELL_FLOW_EXIT) {
        rc = g_shell_flow_status;
    }

    g_subshell_depth--;
    shell_restore_io_frame(&io_frame);
    shell_env_frame_restore(&env_frame);
    shell_restore_cwd_only(saved_cwd);
    g_shell_flow = saved_flow;
    g_shell_flow_status = saved_flow_status;
    g_shell_loop_depth = saved_loop_depth;
    shell_set_status(rc);
    return rc;
}

static int shell_spawn_pipeline_stage(struct shell_simple *cmd,
                                      struct file *pipe_in,
                                      struct file *pipe_out,
                                      int *out_pid) {
    char *env_overlay[ENV_MAX + ARG_MAX + 1];
    char **envp = g_env;
    int envc = g_envc;

    int assignc = 0;
    while (assignc < cmd->argc && shell_is_assignment_word(cmd->argv[assignc])) {
        assignc++;
    }
    if (assignc == cmd->argc) {
        kprintf("pipeline: assignment-only stage has no command\n");
        return 1;
    }
    if (assignc > 0) {
        if (shell_build_env_overlay(cmd->argv, assignc,
                                    env_overlay, &envc) < 0) {
            kprintf("pipeline: environment too large\n");
            return 1;
        }
        envp = env_overlay;
    }

    char *argv[ARG_MAX];
    int argc = cmd->argc - assignc;
    for (int i = 0; i < argc; i++) argv[i] = cmd->argv[i + assignc];

    struct shell_fd_state fds;
    shell_fd_state_init_from_defaults(&fds);
    if (pipe_in && shell_fd_state_replace(&fds, 0, pipe_in, false) < 0) {
        shell_fd_state_close_owned(&fds);
        return 1;
    }
    if (pipe_out && shell_fd_state_replace(&fds, 1, pipe_out, false) < 0) {
        shell_fd_state_close_owned(&fds);
        return 1;
    }
    if (shell_apply_redirs(cmd, &fds, argv[0]) < 0) {
        shell_fd_state_close_owned(&fds);
        return 1;
    }

    char path_buf[64];
    const char *path = resolve_program(argv[0], path_buf, sizeof(path_buf));
    struct proc_spec spec = {
        .path = path, .name = argv[0],
        .fd0 = fds.fd[0], .fd1 = fds.fd[1], .fd2 = fds.fd[2],
        .argc = argc, .argv = argv,
        .envc = envc, .envp = envp,
        .cwd = shell_cwd(),
    };
    int pid = proc_spawn(&spec);
    shell_fd_state_close_owned(&fds);
    if (pid < 0) {
        kprintf("pipeline: failed to spawn '%s'\n", path);
        return 1;
    }
    *out_pid = pid;
    return 0;
}

static int shell_run_pipeline(struct shell_pipeline *pl, bool background) {
    if (pl->count == 1) return shell_run_single(&pl->stage[0], background);

    struct file *pipes_r[SHELL_STAGE_MAX - 1];
    struct file *pipes_w[SHELL_STAGE_MAX - 1];
    int pids[SHELL_STAGE_MAX];
    bool has_pid[SHELL_STAGE_MAX];
    int stage_status[SHELL_STAGE_MAX];
    memset(pipes_r, 0, sizeof(pipes_r));
    memset(pipes_w, 0, sizeof(pipes_w));
    memset(pids, 0, sizeof(pids));
    memset(has_pid, 0, sizeof(has_pid));
    memset(stage_status, 0, sizeof(stage_status));

    for (int i = 0; i < pl->count - 1; i++) {
        if (pipe_create(&pipes_r[i], &pipes_w[i]) != 0) {
            kprintf("pipeline: pipe_create failed\n");
            for (int j = 0; j < i; j++) {
                file_close(pipes_r[j]);
                file_close(pipes_w[j]);
            }
            return 1;
        }
    }

    for (int i = 0; i < pl->count; i++) {
        struct file *in = (i == 0) ? 0 : pipes_r[i - 1];
        struct file *out = (i + 1 == pl->count) ? 0 : pipes_w[i];

        int shell_rc = shell_run_pipeline_shell_stage(&pl->stage[i], in, out);
        if (shell_rc >= 0) {
            stage_status[i] = shell_rc;
        } else if (shell_spawn_pipeline_stage(&pl->stage[i], in, out,
                                              &pids[i]) == 0) {
            has_pid[i] = true;
        } else {
            for (int j = 0; j < pl->count - 1; j++) {
                if (pipes_r[j]) file_close(pipes_r[j]);
                if (pipes_w[j]) file_close(pipes_w[j]);
            }
            for (int j = 0; j < i; j++) {
                if (has_pid[j]) (void)proc_wait(pids[j]);
            }
            return 1;
        }

        if (in) {
            file_close(in);
            pipes_r[i - 1] = 0;
        }
        if (out) {
            file_close(out);
            pipes_w[i] = 0;
        }
    }

    if (background) {
        int last_pid = 0;
        for (int i = pl->count - 1; i >= 0; i--) {
            if (has_pid[i]) { last_pid = pids[i]; break; }
        }
        if (last_pid) {
            g_last_bg_pid = last_pid;
            int jid = jobs_add(last_pid, pl->stage[0].argv[0]);
            if (jid > 0) kprintf("[%d] %d\n", jid, last_pid);
        }
        return 0;
    }

    int rc = 0;
    for (int i = 0; i < pl->count; i++) {
        int stage_rc = stage_status[i];
        if (has_pid[i]) {
            signal_set_foreground(pids[i]);
            stage_rc = proc_wait(pids[i]);
        }
        if (i + 1 == pl->count) rc = stage_rc;
    }
    signal_set_foreground(0);
    return rc;
}

static enum shell_tok_type shell_consume_separator(struct shell_token *tok,
                                                   int ntok, int *io) {
    if (*io >= ntok) return SH_TOK_SEMI;
    enum shell_tok_type t = tok[*io].type;
    if (shell_is_list_sep(t)) {
        (*io)++;
        return t;
    }
    return SH_TOK_SEMI;
}

static const char *shell_skip_blanks(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool shell_starts_with_word(const char *s, const char *word) {
    size_t n = strlen(word);
    return strncmp(s, word, n) == 0 &&
           (s[n] == '\0' || s[n] == ' ' || s[n] == '\t' ||
            s[n] == ';' || s[n] == '\n' || s[n] == '\r');
}

static const char *shell_find_text(const char *s, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return s;
    for (; *s; s++) {
        if (strncmp(s, needle, n) == 0) return s;
    }
    return 0;
}

static int shell_copy_segment(char *dst, size_t cap,
                              const char *start, const char *end) {
    if (!dst || cap == 0 || !start || !end || end < start) return -1;
    while (start < end && (*start == ' ' || *start == '\t' || *start == ';')) {
        start++;
    }
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == ';')) {
        end--;
    }
    size_t n = (size_t)(end - start);
    if (n + 1 > cap) return -1;
    memcpy(dst, start, n);
    dst[n] = '\0';
    return 0;
}

static const char *shell_find_marker2(const char *s, const char *a,
                                      const char *b, const char **found) {
    const char *pa = shell_find_text(s, a);
    const char *pb = shell_find_text(s, b);
    if (pa && (!pb || pa < pb)) {
        if (found) *found = a;
        return pa;
    }
    if (pb) {
        if (found) *found = b;
        return pb;
    }
    return 0;
}

static bool shell_word_boundary_before(const char *start, const char *p);

static bool shell_is_word_end(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == ';' ||
           c == '|' || c == '&' || c == ')' || c == '>' || c == '<';
}

static const char *shell_find_matching_done(const char *start,
                                            const char **found_marker) {
    int depth = 1;
    bool in_sq = false, in_dq = false;
    for (const char *p = start; *p; p++) {
        if (in_sq) {
            if (*p == '\'') in_sq = false;
            continue;
        }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"') { in_dq = true; continue; }

        /* Check for "; done" / ";done" FIRST (before "; do") to avoid
         * false positive where "; done" starts with "; do". */
        if (strncmp(p, "; done", 6) == 0 && shell_is_word_end(p[6])) {
            depth--;
            if (depth <= 0) {
                if (found_marker) *found_marker = "; done";
                return p;
            }
            p += 5;
            continue;
        }
        if (strncmp(p, ";done", 5) == 0 && shell_is_word_end(p[5])) {
            depth--;
            if (depth <= 0) {
                if (found_marker) *found_marker = ";done";
                return p;
            }
            p += 4;
            continue;
        }
        /* Now check "; do" / ";do" */
        if (strncmp(p, "; do", 4) == 0 && shell_is_word_end(p[4])) {
            depth++;
            p += 3;
            continue;
        }
        if (strncmp(p, ";do", 3) == 0 && shell_is_word_end(p[3])) {
            depth++;
            p += 2;
            continue;
        }
    }
    return 0;
}

static const char *shell_find_if_fi(const char *start) {
    int depth = 1;
    bool in_sq = false, in_dq = false;
    for (const char *p = start; *p; p++) {
        if (in_sq) { if (*p == '\'') in_sq = false; continue; }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"')  { in_dq = true; continue; }
        if (shell_starts_with_word(p, "if") &&
            shell_word_boundary_before(start, p)) depth++;
        /* The character BEFORE the ';' is deliberately not examined. It used
         * to be required to be a space or ';' (shell_word_boundary_before),
         * which meant `if c; then echo eq; fi` did not parse -- the 'q' of
         * "eq" sat in front of the ';' -- while `... echo eq ; fi` did. bash
         * makes no such distinction, and a ';' cannot occur inside a bare
         * word anyway; quoted ones are already skipped above. What DOES
         * matter is that "fi" ends a word, so `; fifo` is not a terminator. */
        if ((strncmp(p, "; fi", 4) == 0 && shell_is_word_end(p[4])) ||
            (strncmp(p, ";fi",  3) == 0 && shell_is_word_end(p[3]))) {
            depth--;
            if (depth == 0) return p;
        }
    }
    return 0;
}

static const char *shell_find_elif_else(const char *start, const char *fi_at,
                                        const char **found_marker) {
    int depth = 0;
    bool in_sq = false, in_dq = false;
    *found_marker = 0;
    for (const char *p = start; p < fi_at && *p; p++) {
        if (in_sq) { if (*p == '\'') in_sq = false; continue; }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"')  { in_dq = true; continue; }
        if (shell_starts_with_word(p, "if") &&
            shell_word_boundary_before(start, p)) { depth++; continue; }
        if (depth > 0 &&
            (strncmp(p, "; fi", 4) == 0 || strncmp(p, ";fi", 3) == 0)) {
            depth--;
            continue;
        }
        if (depth > 0) continue;
        if (strncmp(p, "; elif", 6) == 0) {
            *found_marker = "; elif";
            return p;
        }
        if (strncmp(p, ";elif", 5) == 0) {
            *found_marker = ";elif";
            return p;
        }
        if (strncmp(p, "; else", 6) == 0) {
            *found_marker = "; else";
            return p;
        }
        if (strncmp(p, ";else", 5) == 0) {
            *found_marker = ";else";
            return p;
        }
    }
    return 0;
}

/* ---- redirections attached to a compound command ------------------------
 *
 * `while read l; do ...; done < file` redirects the WHOLE loop, and it is how
 * every script feeds a file into a read loop. The loop parsers used to stop
 * at `done` and ignore what followed, so the body read from the terminal and
 * the loop never ran.
 *
 * Parses the text after a compound's terminator as redirections only, into
 * `out`. Returns 0 on success (including an empty tail), -1 on a parse error
 * or if the tail contains anything that is not a redirection. */
/* Re-point a parsed redirection list's strings at caller-owned storage. */
static int shell_redirs_intern(struct shell_simple *out, char *buf, size_t cap) {
    size_t pos = 0;
    for (int i = 0; i < out->redir_count; i++) {
        struct shell_redir *r = &out->redir[i];
        const char **fields[2] = { &r->path, &r->text };
        for (int f = 0; f < 2; f++) {
            const char *src = *fields[f];
            if (!src) continue;
            size_t n = strlen(src) + 1;
            if (pos + n > cap) return -1;
            memcpy(buf + pos, src, n);
            *fields[f] = buf + pos;
            pos += n;
        }
    }
    /* These alias into `words` too and nothing on this path reads them. */
    out->stdin_path = out->stdin_text = out->stdout_path = 0;
    return 0;
}

static int shell_parse_compound_redirs(const char *tail, struct shell_simple *out,
                                       char *pathbuf, size_t pathcap,
                                       const char *label) {
    shell_simple_init(out);
    tail = shell_skip_blanks(tail);
    if (!*tail) return 0;

    /* Heap for the same reason as everywhere else on this path: the loop
     * parsers that call this already hold list[], body[], tok[] and words[],
     * and struct shell_pipeline alone is ~5.5 KB. */
    struct shell_token *tok = kmalloc(sizeof(*tok) * SHELL_TOKEN_MAX);
    char *words = kmalloc(SHELL_PARSE_BUF_MAX);
    struct shell_pipeline *pl = kmalloc(sizeof(*pl));
    int rc = -1;
    if (!tok || !words || !pl) {
        kprintf("%s: out of memory parsing redirections\n", label);
        goto out;
    }
    {
        int ntok = 0;
        if (shell_tokenize(tail, tok, &ntok, words, SHELL_PARSE_BUF_MAX) < 0)
            goto out;
        if (ntok == 0) { rc = 0; goto out; }

        int i = 0;
        int parsed = shell_parse_pipeline(tok, ntok, &i, pl);
        if (parsed <= 0 || pl->count != 1 || pl->stage[0].argc != 0) {
            kprintf("%s: expected only redirections after the loop\n", label);
            goto out;
        }
        *out = pl->stage[0];
        /* The struct's path/text pointers point INTO `words`, which is freed
         * at `out:` below. Copying the struct alone handed the caller
         * dangling pointers, so by the time it entered the io frame the
         * filename was freed memory -- `for ... done > file` reported success
         * and wrote to the terminal. Intern the strings into caller-owned
         * storage so they outlive this frame. */
        if (shell_redirs_intern(out, pathbuf, pathcap) < 0) {
            kprintf("%s: redirection target too long\n", label);
            goto out;
        }
        rc = 0;
    }
out:
    if (tok)   kfree(tok);
    if (words) kfree(words);
    if (pl)    kfree(pl);
    return rc;
}

static bool shell_try_if_command(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (!shell_starts_with_word(s, "if")) return false;
    s = shell_skip_blanks(s + 2);

    const char *fi_at = shell_find_if_fi(s);
    if (!fi_at) {
        kprintf("if: expected '; fi'\n");
        shell_set_status(2);
        return true;
    }
    const char *fi_skip = (strncmp(fi_at, "; fi", 4) == 0) ? fi_at + 4
                                                            : fi_at + 3;
    (void)fi_skip;

    const char *cond_start = s;
    bool done = false;
    while (!done) {
        const char *then_marker = 0;
        const char *then_at = shell_find_marker2(cond_start, "; then",
                                                 ";then", &then_marker);
        if (!then_at || then_at > fi_at) {
            kprintf("if: expected '; then'\n");
            shell_set_status(2);
            return true;
        }
        const char *after_then = then_at + strlen(then_marker);

        const char *branch_marker = 0;
        const char *branch_at = shell_find_elif_else(after_then, fi_at,
                                                     &branch_marker);

        char cond[LINE_MAX];
        if (shell_copy_segment(cond, sizeof(cond), cond_start, then_at) < 0) {
            kprintf("if: condition too long\n");
            shell_set_status(2);
            return true;
        }

        /* The condition's status is the decision, so `set -e` must not act
         * on it: `if false; then` used to exit the shell outright. */
        g_errexit_suspend++;
        execute_line_text(cond);
        g_errexit_suspend--;
        if (g_shell_flow != SHELL_FLOW_NONE) return true;

        if (g_last_status == 0) {
            char yes[LINE_MAX];
            const char *body_end = branch_at ? branch_at : fi_at;
            if (shell_copy_segment(yes, sizeof(yes), after_then, body_end) < 0) {
                kprintf("if: body too long\n");
                shell_set_status(2);
                return true;
            }
            execute_line_text(yes);
            return true;
        }

        if (!branch_at) {
            shell_set_status(0);
            return true;
        }

        bool is_elif = (strncmp(branch_marker, "; elif", 6) == 0 ||
                        strncmp(branch_marker, ";elif", 5) == 0);
        bool is_else = !is_elif;

        if (is_else) {
            char no[LINE_MAX];
            const char *else_body = branch_at + strlen(branch_marker);
            if (shell_copy_segment(no, sizeof(no), else_body, fi_at) < 0) {
                kprintf("if: else body too long\n");
                shell_set_status(2);
                return true;
            }
            execute_line_text(no);
            return true;
        }

        cond_start = shell_skip_blanks(branch_at + strlen(branch_marker));
    }

    shell_set_status(0);
    return true;
}

static bool shell_try_for_command(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (!shell_starts_with_word(s, "for")) return false;
    s = shell_skip_blanks(s + 3);

    const char *name_start = s;
    if (!shell_var_start(*s)) {
        kprintf("for: bad variable name\n");
        shell_set_status(2);
        return true;
    }
    while (shell_var_char(*s)) s++;
    size_t name_len = (size_t)(s - name_start);
    char name[64];
    if (name_len + 1 > sizeof(name)) {
        kprintf("for: variable name too long\n");
        shell_set_status(2);
        return true;
    }
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';

    s = shell_skip_blanks(s);
    bool implicit_at = false;
    if (shell_starts_with_word(s, "in")) {
        s = shell_skip_blanks(s + 2);
    } else if (*s == ';' || shell_starts_with_word(s, "do")) {
        implicit_at = true;
    } else {
        kprintf("for: expected 'in'\n");
        shell_set_status(2);
        return true;
    }

    const char *do_marker = 0;
    const char *do_at = shell_find_marker2(s, "; do", ";do", &do_marker);
    if (!do_at) {
        kprintf("for: expected '; do'\n");
        shell_set_status(2);
        return true;
    }
    const char *done_marker = 0;
    const char *done_at = shell_find_matching_done(do_at + strlen(do_marker),
                                                    &done_marker);
    if (!done_at) {
        kprintf("for: expected '; done'\n");
        shell_set_status(2);
        return true;
    }

    char list[LINE_MAX];
    char body[LINE_MAX];
    if (implicit_at) {
        size_t lpos = 0;
        for (int i = 0; i < g_positional_count; i++) {
            if (i > 0 && shell_append_char(list, &lpos, sizeof(list), ' ') < 0) {
                kprintf("for: list too long\n"); shell_set_status(2); return true;
            }
            if (shell_append_str(list, &lpos, sizeof(list), g_positional[i]) < 0) {
                kprintf("for: list too long\n"); shell_set_status(2); return true;
            }
        }
        list[lpos] = '\0';
    } else {
        if (shell_copy_segment(list, sizeof(list), s, do_at) < 0) {
            kprintf("for: command too long\n");
            shell_set_status(2);
            return true;
        }
    }
    if (shell_copy_segment(body, sizeof(body), do_at + strlen(do_marker),
                           done_at) < 0) {
        kprintf("for: command too long\n");
        shell_set_status(2);
        return true;
    }

    struct shell_simple tail_redirs;
#ifdef SHELL_TRACE_FOR
    kprintf("[fortrace] marker='%s' tail='%s'\n",
            done_marker ? done_marker : "(null)",
            done_at + strlen(done_marker));
#endif
    char tail_paths[512];
    if (shell_parse_compound_redirs(done_at + strlen(done_marker),
                                    &tail_redirs, tail_paths,
                                    sizeof(tail_paths), "for") < 0) {
        shell_set_status(2);
        return true;
    }
#ifdef SHELL_TRACE_FOR
    kprintf("[fortrace] redir_count=%d\n", tail_redirs.redir_count);
#endif

    struct shell_token tok[SHELL_TOKEN_MAX];
    char words[SHELL_PARSE_BUF_MAX];
    int ntok = 0;
    if (shell_tokenize(list, tok, &ntok, words, sizeof(words)) < 0) {
        shell_set_status(2);
        return true;
    }

    /* The list is a WORD LIST, and gets the same treatment a command's
     * arguments get: IFS splitting of unquoted expansions, "$@" to one word
     * per parameter, and globbing. Iterating the raw tokens instead meant
     * `for w in $v` ran once with "a b c" and `for f in *.txt` iterated over
     * the literal pattern -- the two most obvious things a for loop is for. */
    /* HEAP, not stack. struct shell_pipeline is ~5.5 KB (eight stages plus a
     * 1 KB expansion buffer), and this function already carries list[],
     * body[], tok[] and words[] -- about 5 KB more. A nested `for` recurses
     * through execute_line_text into another copy of this frame, so putting
     * the pipeline on the stack overflowed it and corrupted memory a byte at
     * a time: output came back with single characters missing at random
     * positions, in cases that had nothing to do with loops. */
    struct shell_pipeline *wl = kmalloc(sizeof(*wl));
    struct shell_simple *items = kmalloc(sizeof(*items));
    if (!wl || !items) {
        if (wl) kfree(wl);
        if (items) kfree(items);
        kprintf("for: out of memory expanding list\n");
        shell_set_status(2);
        return true;
    }
    wl->count = 0;
    wl->expand_pos = 0;
    shell_simple_init(items);
    if (implicit_at) {
        /* `for a` with no `in` iterates the positional parameters with
         * "$@" semantics: ONE iteration per parameter, each kept whole.
         * Flattening them into a string and re-splitting (which is what
         * building `list` above does) turns `set -- "one two"` into two
         * iterations, losing exactly the quoting the caller preserved. */
        for (int i = 0; i < g_positional_count; i++) {
            char *saved = 0;
            if (shell_pipeline_save_word(wl, g_positional[i], &saved) < 0 ||
                shell_add_one_arg(wl, items, saved, true) < 0) {
                kfree(wl); kfree(items);
                shell_set_status(2);
                return true;
            }
        }
    } else {
        for (int i = 0; i < ntok; i++) {
            if (tok[i].type != SH_TOK_WORD) continue;
            if (shell_add_arg_ex(wl, items, tok[i].text, tok[i].quoted,
                                 tok[i].expanded, tok[i].assign_src) < 0) {
                kfree(wl); kfree(items);
                shell_set_status(2);
                return true;
            }
        }
    }

    int last = 0;
    g_shell_loop_depth++;

    struct shell_io_frame io_frame;
    bool io_active = false;
    if (tail_redirs.redir_count > 0) {
        if (shell_enter_io_frame(&tail_redirs, "for", &io_frame) < 0) {
            g_shell_loop_depth--;
            kfree(wl); kfree(items);
            shell_set_status(1);
            return true;
        }
        io_active = true;
    }

    /* One exit path, so the redirection frame is always unwound. Every
     * `return true` inside the loop became a `goto done` for that reason. */
    bool set_last = true;
    for (int i = 0; i < items->argc; i++) {
        if (env_set(name, items->argv[i]) < 0) {
            kprintf("for: failed to set '%s'\n", name);
            last = 1;
            goto done;
        }
        execute_line_text(body);
        last = g_last_status;
        if (g_shell_flow == SHELL_FLOW_RETURN ||
            g_shell_flow == SHELL_FLOW_EXIT) {
            set_last = false;
            goto done;
        }
        if (g_shell_flow == SHELL_FLOW_BREAK) {
            if (--g_shell_break_depth <= 0) {
                g_shell_flow = SHELL_FLOW_NONE;
                g_shell_flow_status = 0;
            }
            goto done;
        }
        if (g_shell_flow == SHELL_FLOW_CONTINUE) {
            if (--g_shell_break_depth <= 0) {
                g_shell_flow = SHELL_FLOW_NONE;
                g_shell_flow_status = 0;
            } else {
                set_last = false;
                goto done;
            }
            continue;
        }
    }
done:
    g_shell_loop_depth--;
    if (io_active) shell_restore_io_frame(&io_frame);
    kfree(wl);
    kfree(items);
    if (set_last) shell_set_status(last);
    return true;
}

static bool shell_try_while_command(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (!shell_starts_with_word(s, "while")) return false;
    s = shell_skip_blanks(s + 5);

    const char *do_marker = 0;
    const char *do_at = shell_find_marker2(s, "; do", ";do", &do_marker);
    if (!do_at) {
        kprintf("while: expected '; do'\n");
        shell_set_status(2);
        return true;
    }
    const char *done_marker = 0;
    const char *done_at = shell_find_matching_done(do_at + strlen(do_marker),
                                                    &done_marker);
    if (!done_at) {
        kprintf("while: expected '; done'\n");
        shell_set_status(2);
        return true;
    }

    struct shell_simple tail_redirs;
    char tail_paths[512];
    if (shell_parse_compound_redirs(done_at + strlen(done_marker),
                                    &tail_redirs, tail_paths,
                                    sizeof(tail_paths), "while") < 0) {
        shell_set_status(2);
        return true;
    }

    char cond[LINE_MAX];
    char body[LINE_MAX];
    if (shell_copy_segment(cond, sizeof(cond), s, do_at) < 0 ||
        shell_copy_segment(body, sizeof(body), do_at + strlen(do_marker),
                           done_at) < 0) {
        kprintf("while: command too long\n");
        shell_set_status(2);
        return true;
    }

    int last = 0;
    g_shell_loop_depth++;

    /* The redirection wraps the WHOLE loop, so it is entered once here and
     * unwound once at `done` -- hence the single exit path below. */
    struct shell_io_frame io_frame;
    bool io_active = false;
    if (tail_redirs.redir_count > 0) {
        if (shell_enter_io_frame(&tail_redirs, "while", &io_frame) < 0) {
            g_shell_loop_depth--;
            shell_set_status(1);
            return true;
        }
        io_active = true;
    }

    bool set_last = true;
    bool overrun = false;
    for (int iter = 0; iter < 1024; iter++) {
        g_errexit_suspend++;              /* the condition is a decision */
        execute_line_text(cond);
        g_errexit_suspend--;
        if (g_last_status != 0) {
            goto done;
        }
        execute_line_text(body);          /* the BODY is not exempt */
        last = g_last_status;
        if (g_shell_flow == SHELL_FLOW_RETURN ||
            g_shell_flow == SHELL_FLOW_EXIT) {
            set_last = false;
            goto done;
        }
        if (g_shell_flow == SHELL_FLOW_BREAK) {
            if (--g_shell_break_depth <= 0) {
                g_shell_flow = SHELL_FLOW_NONE;
                g_shell_flow_status = 0;
            }
            goto done;
        }
        if (g_shell_flow == SHELL_FLOW_CONTINUE) {
            if (--g_shell_break_depth <= 0) {
                g_shell_flow = SHELL_FLOW_NONE;
                g_shell_flow_status = 0;
            } else {
                set_last = false;
                goto done;
            }
            continue;
        }
        if (iter == 1023) overrun = true;
    }
done:
    g_shell_loop_depth--;
    if (io_active) shell_restore_io_frame(&io_frame);
    if (overrun) {
        kprintf("while: iteration limit reached\n");
        shell_set_status(2);
    } else if (set_last) {
        shell_set_status(last);
    }
    return true;
}

static bool shell_try_until_command(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (!shell_starts_with_word(s, "until")) return false;
    s = shell_skip_blanks(s + 5);

    const char *do_marker = 0;
    const char *do_at = shell_find_marker2(s, "; do", ";do", &do_marker);
    if (!do_at) {
        kprintf("until: expected '; do'\n");
        shell_set_status(2);
        return true;
    }
    const char *done_marker = 0;
    const char *done_at = shell_find_matching_done(do_at + strlen(do_marker),
                                                    &done_marker);
    if (!done_at) {
        kprintf("until: expected '; done'\n");
        shell_set_status(2);
        return true;
    }

    struct shell_simple tail_redirs;
    char tail_paths[512];
    if (shell_parse_compound_redirs(done_at + strlen(done_marker),
                                    &tail_redirs, tail_paths,
                                    sizeof(tail_paths), "until") < 0) {
        shell_set_status(2);
        return true;
    }

    char cond[LINE_MAX];
    char body[LINE_MAX];
    if (shell_copy_segment(cond, sizeof(cond), s, do_at) < 0 ||
        shell_copy_segment(body, sizeof(body), do_at + strlen(do_marker),
                           done_at) < 0) {
        kprintf("until: command too long\n");
        shell_set_status(2);
        return true;
    }

    int last = 0;
    g_shell_loop_depth++;

    /* The redirection wraps the WHOLE loop, so it is entered once here and
     * unwound once at `done` -- hence the single exit path below. */
    struct shell_io_frame io_frame;
    bool io_active = false;
    if (tail_redirs.redir_count > 0) {
        if (shell_enter_io_frame(&tail_redirs, "until", &io_frame) < 0) {
            g_shell_loop_depth--;
            shell_set_status(1);
            return true;
        }
        io_active = true;
    }

    bool set_last = true;
    bool overrun = false;
    for (int iter = 0; iter < 1024; iter++) {
        execute_line_text(cond);
        if (g_last_status == 0) {
            goto done;
        }
        execute_line_text(body);
        last = g_last_status;
        if (g_shell_flow == SHELL_FLOW_RETURN ||
            g_shell_flow == SHELL_FLOW_EXIT) {
            set_last = false;
            goto done;
        }
        if (g_shell_flow == SHELL_FLOW_BREAK) {
            if (--g_shell_break_depth <= 0) {
                g_shell_flow = SHELL_FLOW_NONE;
                g_shell_flow_status = 0;
            }
            goto done;
        }
        if (g_shell_flow == SHELL_FLOW_CONTINUE) {
            if (--g_shell_break_depth <= 0) {
                g_shell_flow = SHELL_FLOW_NONE;
                g_shell_flow_status = 0;
            } else {
                set_last = false;
                goto done;
            }
            continue;
        }
        if (iter == 1023) overrun = true;
    }
done:
    g_shell_loop_depth--;
    if (io_active) shell_restore_io_frame(&io_frame);
    if (overrun) {
        kprintf("until: iteration limit reached\n");
        shell_set_status(2);
    } else if (set_last) {
        shell_set_status(last);
    }
    return true;
}

static bool shell_word_boundary_before(const char *start, const char *p) {
    return p == start || is_space(p[-1]) || p[-1] == ';';
}

static bool shell_word_boundary_after(const char *p) {
    return *p == '\0' || is_space(*p) || *p == ';' || *p == ')';
}

static const char *shell_find_word_marker(const char *start,
                                          const char *word) {
    size_t n = strlen(word);
    for (const char *p = start; *p; p++) {
        if (strncmp(p, word, n) == 0 &&
            shell_word_boundary_before(start, p) &&
            shell_word_boundary_after(p + n)) {
            return p;
        }
    }
    return 0;
}

static int shell_expand_case_word(const char *src, char *out, size_t cap) {
    struct shell_token tok[SHELL_TOKEN_MAX];
    char words[SHELL_PARSE_BUF_MAX];
    int ntok = 0;
    if (shell_tokenize(src, tok, &ntok, words, sizeof(words)) < 0) return -1;
    for (int i = 0; i < ntok; i++) {
        if (tok[i].type != SH_TOK_WORD) continue;
        shell_strip_nosplit_inplace(tok[i].text);
        size_t n = strlen(tok[i].text);
        if (n + 1 > cap) return -1;
        memcpy(out, tok[i].text, n + 1);
        return 0;
    }
    out[0] = '\0';
    return 0;
}

/* Strip one level of shell quoting from a case pattern, in place.
 *
 * `case "$v" in "") ...` is the idiomatic empty-string arm, and it never
 * matched: the pattern text is the two characters `""`, which were compared
 * LITERALLY against the (empty) word. Quotes in a pattern suppress globbing
 * of what they enclose rather than being part of the pattern, so remove them
 * -- and once `""` unquotes to the empty pattern, an empty pattern has to be
 * allowed to match the empty word instead of being rejected as blank. */
/* Glob metacharacters that must be neutralised when they came from inside
 * quotes -- `"*"` is the literal asterisk, not the match-anything pattern. */
static bool shell_glob_meta(char c) {
    return c == '*' || c == '?' || c == '[' || c == ']' || c == '\\';
}

static void shell_case_unquote(char *pat) {
    bool in_sq = false, in_dq = false;
    char *w = pat;
    for (char *r = pat; *r; r++) {
        if (in_sq) {
            if (*r == '\'') { in_sq = false; continue; }
            if (shell_glob_meta(*r)) *w++ = '\\';
            *w++ = *r;
            continue;
        }
        if (in_dq) {
            if (*r == '\\' && r[1]) { r++; if (shell_glob_meta(*r)) *w++ = '\\'; *w++ = *r; continue; }
            if (*r == '"') { in_dq = false; continue; }
            if (shell_glob_meta(*r)) *w++ = '\\';
            *w++ = *r;
            continue;
        }
        if (*r == '\'') { in_sq = true;  continue; }
        if (*r == '"')  { in_dq = true;  continue; }
        if (*r == '\\' && r[1]) { *w++ = *++r; continue; }
        *w++ = *r;
    }
    *w = '\0';
}

static bool shell_case_pattern_match(const char *patterns, const char *word) {
    const char *p = patterns;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '(') p++;
        const char *start = p;
        bool in_sq = false, in_dq = false;
        /* A `|` inside quotes is a literal, not an alternation separator. */
        while (*p) {
            if (in_sq) { if (*p == '\'') in_sq = false; p++; continue; }
            if (in_dq) { if (*p == '"')  in_dq = false; p++; continue; }
            if (*p == '\'') { in_sq = true; p++; continue; }
            if (*p == '"')  { in_dq = true; p++; continue; }
            if (*p == '|') break;
            p++;
        }
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

        char pat[128];
        if (shell_copy_segment(pat, sizeof(pat), start, end) == 0) {
            bool was_quoted = (end > start);
            shell_case_unquote(pat);
            /* An empty pattern is only meaningful if it came from quotes;
             * a genuinely blank clause is still skipped. */
            if ((pat[0] || was_quoted) && shell_glob_match(pat, word)) return true;
        }
        if (*p == '|') p++;
    }
    return false;
}

static bool shell_try_case_command(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (!shell_starts_with_word(s, "case")) return false;
    s = shell_skip_blanks(s + 4);

    const char *in_at = shell_find_word_marker(s, "in");
    if (!in_at) {
        kprintf("case: expected 'in'\n");
        shell_set_status(2);
        return true;
    }
    /* Find the esac that closes THIS case, not the first one in the text: a
     * `case` nested inside a clause body has its own, and matching that one
     * truncated the outer construct so every clause after the nested one was
     * silently dropped. */
    const char *esac_at = 0;
    {
        int depth = 1;
        for (const char *q = in_at + 2; *q; q++) {
            if (!shell_word_boundary_before(in_at + 2, q)) continue;
            if (shell_starts_with_word(q, "case")) { depth++; continue; }
            if (shell_starts_with_word(q, "esac")) {
                if (--depth == 0) { esac_at = q; break; }
            }
        }
    }
    if (!esac_at) {
        kprintf("case: expected 'esac'\n");
        shell_set_status(2);
        return true;
    }

    char word_src[LINE_MAX];
    char word[SHELL_PARSE_BUF_MAX];
    if (shell_copy_segment(word_src, sizeof(word_src), s, in_at) < 0 ||
        shell_expand_case_word(word_src, word, sizeof(word)) < 0) {
        kprintf("case: word too long\n");
        shell_set_status(2);
        return true;
    }

    const char *p = shell_skip_blanks(in_at + 2);
    if (*p == ';') p++;
    int last = 0;
    bool matched = false;

    while (p < esac_at) {
        while (p < esac_at && (is_space(*p) || *p == ';')) p++;
        if (p >= esac_at) break;

        const char *close = p;
        while (close < esac_at && *close != ')') close++;
        if (close >= esac_at) {
            kprintf("case: expected ')'\n");
            shell_set_status(2);
            return true;
        }

        const char *body = close + 1;
        /* The `;;` that ends THIS clause, skipping any nested case construct:
         * a `case` inside a clause body has clause terminators of its own, and
         * matching the first one truncated the outer body right after it. */
        const char *sep = 0;
        {
            int cdepth = 0;
            for (const char *q = body; *q && q < esac_at; q++) {
                if (shell_word_boundary_before(body, q)) {
                    if (shell_starts_with_word(q, "case")) { cdepth++; continue; }
                    if (shell_starts_with_word(q, "esac")) {
                        if (cdepth > 0) cdepth--;
                        continue;
                    }
                }
                if (cdepth == 0 && q[0] == ';' && q[1] == ';') { sep = q; break; }
            }
        }
        if (!sep || sep > esac_at) sep = esac_at;

        char pats[128];
        char body_text[LINE_MAX];
        if (shell_copy_segment(pats, sizeof(pats), p, close) < 0 ||
            shell_copy_segment(body_text, sizeof(body_text), body, sep) < 0) {
            kprintf("case: clause too long\n");
            shell_set_status(2);
            return true;
        }

        if (!matched && shell_case_pattern_match(pats, word)) {
            matched = true;
            execute_line_text(body_text);
            last = g_last_status;
            shell_set_status(last);
            return true;
        }

        p = (sep < esac_at) ? sep + 2 : esac_at;
    }

    shell_set_status(0);
    return true;
}

static bool shell_try_function_definition(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (!shell_var_start(*s)) return false;

    const char *name_start = s;
    while (shell_var_char(*s)) s++;
    size_t name_len = (size_t)(s - name_start);
    const char *after_name = shell_skip_blanks(s);
    if (after_name[0] != '(' || after_name[1] != ')') return false;

    s = shell_skip_blanks(after_name + 2);
    if (*s != '{') {
        kprintf("function: expected '{'\n");
        shell_set_status(2);
        return true;
    }
    const char *body_start = s + 1;
    const char *body_end = 0;
    int depth = 1;
    bool in_single = false;
    bool in_double = false;
    for (const char *p = body_start; *p; p++) {
        if (in_single) {
            if (*p == '\'') in_single = false;
            continue;
        }
        if (in_double) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') in_double = false;
            continue;
        }
        if (*p == '\'') {
            in_single = true;
            continue;
        }
        if (*p == '"') {
            in_double = true;
            continue;
        }
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                body_end = p;
                break;
            }
        }
    }
    if (!body_end) {
        kprintf("function: expected '}'\n");
        shell_set_status(2);
        return true;
    }

    char name[64];
    char body[LINE_MAX];
    if (name_len == 0 || name_len + 1 > sizeof(name) ||
        shell_copy_segment(body, sizeof(body), body_start, body_end) < 0) {
        kprintf("function: definition too long\n");
        shell_set_status(2);
        return true;
    }
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';
    if (shell_function_set(name, body) < 0) {
        kprintf("function: failed to define '%s'\n", name);
        shell_set_status(1);
        return true;
    }
    shell_set_status(0);
    return true;
}

struct shell_param_snapshot {
    char *param0;
    char *positional[ARG_MAX];
    int positional_count;
};

static void shell_param_snapshot_clear(struct shell_param_snapshot *frame) {
    if (!frame) return;
    if (frame->param0) {
        kfree(frame->param0);
        frame->param0 = 0;
    }
    for (int i = 0; i < frame->positional_count; i++) {
        if (frame->positional[i]) kfree(frame->positional[i]);
        frame->positional[i] = 0;
    }
    frame->positional_count = 0;
}

static int shell_param_snapshot_capture(struct shell_param_snapshot *frame) {
    if (!frame) return -1;
    memset(frame, 0, sizeof(*frame));
    if (g_param0) {
        frame->param0 = shell_strdup(g_param0);
        if (!frame->param0) return -1;
    }
    frame->positional_count = g_positional_count;
    for (int i = 0; i < g_positional_count; i++) {
        frame->positional[i] = shell_strdup(g_positional[i]);
        if (!frame->positional[i]) {
            shell_param_snapshot_clear(frame);
            return -1;
        }
    }
    return 0;
}

static void shell_param_snapshot_restore(struct shell_param_snapshot *frame) {
    if (!frame) return;
    shell_free_current_params();
    g_param0 = frame->param0;
    frame->param0 = 0;
    g_positional_count = frame->positional_count;
    for (int i = 0; i < g_positional_count; i++) {
        g_positional[i] = frame->positional[i];
        frame->positional[i] = 0;
    }
    frame->positional_count = 0;
}

struct shell_alias_frame {
    char *name[SHELL_ALIAS_MAX];
    char *value[SHELL_ALIAS_MAX];
};

static void shell_alias_frame_clear(struct shell_alias_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
        if (frame->name[i]) kfree(frame->name[i]);
        if (frame->value[i]) kfree(frame->value[i]);
        frame->name[i] = 0;
        frame->value[i] = 0;
    }
}

static int shell_alias_frame_capture(struct shell_alias_frame *frame) {
    if (!frame) return -1;
    memset(frame, 0, sizeof(*frame));
    for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
        if (!g_aliases[i].name) continue;
        frame->name[i] = shell_strdup(g_aliases[i].name);
        frame->value[i] = shell_strdup(g_aliases[i].value);
        if (!frame->name[i] || !frame->value[i]) {
            shell_alias_frame_clear(frame);
            return -1;
        }
    }
    return 0;
}

static void shell_alias_frame_restore(struct shell_alias_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
        if (g_aliases[i].name) kfree(g_aliases[i].name);
        if (g_aliases[i].value) kfree(g_aliases[i].value);
        g_aliases[i].name = frame->name[i];
        g_aliases[i].value = frame->value[i];
        frame->name[i] = 0;
        frame->value[i] = 0;
    }
}

struct shell_function_frame {
    char *name[SHELL_FUNC_MAX];
    char *body[SHELL_FUNC_MAX];
};

static void shell_function_frame_clear(struct shell_function_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < SHELL_FUNC_MAX; i++) {
        if (frame->name[i]) kfree(frame->name[i]);
        if (frame->body[i]) kfree(frame->body[i]);
        frame->name[i] = 0;
        frame->body[i] = 0;
    }
}

static int shell_function_frame_capture(struct shell_function_frame *frame) {
    if (!frame) return -1;
    memset(frame, 0, sizeof(*frame));
    for (int i = 0; i < SHELL_FUNC_MAX; i++) {
        if (!g_functions[i].name) continue;
        frame->name[i] = shell_strdup(g_functions[i].name);
        frame->body[i] = shell_strdup(g_functions[i].body);
        if (!frame->name[i] || !frame->body[i]) {
            shell_function_frame_clear(frame);
            return -1;
        }
    }
    return 0;
}

static void shell_function_frame_restore(struct shell_function_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < SHELL_FUNC_MAX; i++) {
        if (g_functions[i].name) kfree(g_functions[i].name);
        if (g_functions[i].body) kfree(g_functions[i].body);
        g_functions[i].name = frame->name[i];
        g_functions[i].body = frame->body[i];
        frame->name[i] = 0;
        frame->body[i] = 0;
    }
}

struct shell_readonly_frame {
    char *name[SHELL_READONLY_MAX];
};

static void shell_readonly_frame_clear(struct shell_readonly_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < SHELL_READONLY_MAX; i++) {
        if (frame->name[i]) kfree(frame->name[i]);
        frame->name[i] = 0;
    }
}

static int shell_readonly_frame_capture(struct shell_readonly_frame *frame) {
    if (!frame) return -1;
    memset(frame, 0, sizeof(*frame));
    for (int i = 0; i < SHELL_READONLY_MAX; i++) {
        if (!g_readonly[i]) continue;
        frame->name[i] = shell_strdup(g_readonly[i]);
        if (!frame->name[i]) {
            shell_readonly_frame_clear(frame);
            return -1;
        }
    }
    return 0;
}

static void shell_readonly_frame_restore(struct shell_readonly_frame *frame) {
    if (!frame) return;
    for (int i = 0; i < SHELL_READONLY_MAX; i++) {
        if (g_readonly[i]) kfree(g_readonly[i]);
        g_readonly[i] = frame->name[i];
        frame->name[i] = 0;
    }
}

struct shell_subshell_frame {
    struct shell_env_frame env;
    struct shell_param_snapshot params;
    struct shell_alias_frame aliases;
    struct shell_function_frame functions;
    struct shell_readonly_frame readonly;
    struct shell_trap_frame traps;
    struct shell_opt_frame opts;
    char cwd[VFS_PATH_MAX];
    enum shell_flow flow;
    int flow_status;
    int loop_depth;
    /* A subshell gets its own copy of the shell EXECUTION ENVIRONMENT,
     * and POSIX 2.12 counts open files as part of it. Without these,
     * `( exec > f; echo x )` left the shell redirected after the
     * subshell ended and every later command wrote into f. */
    struct file *fd[SHELL_FD_MAX];
};

static void shell_subshell_frame_clear(struct shell_subshell_frame *frame) {
    if (!frame) return;
    shell_env_frame_clear(&frame->env);
    shell_param_snapshot_clear(&frame->params);
    shell_alias_frame_clear(&frame->aliases);
    shell_function_frame_clear(&frame->functions);
    shell_readonly_frame_clear(&frame->readonly);
    for (int i = 0; i < SIG_MAX; i++) {
        if (frame->traps.trap[i]) kfree(frame->traps.trap[i]);
        frame->traps.trap[i] = 0;
    }
}

static int shell_subshell_frame_capture(struct shell_subshell_frame *frame) {
    if (!frame) return -1;
    memset(frame, 0, sizeof(*frame));
    const char *cwd = shell_cwd();
    if (!cwd || strlen(cwd) + 1 > sizeof(frame->cwd)) return -1;
    memcpy(frame->cwd, cwd, strlen(cwd) + 1);
    frame->flow = g_shell_flow;
    frame->flow_status = g_shell_flow_status;
    frame->loop_depth = g_shell_loop_depth;
    for (int i = 0; i < SHELL_FD_MAX; i++) frame->fd[i] = g_shell_fd[i];

    shell_save_opts(&frame->opts);
    if (shell_env_frame_capture(&frame->env) < 0 ||
        shell_param_snapshot_capture(&frame->params) < 0 ||
        shell_alias_frame_capture(&frame->aliases) < 0 ||
        shell_function_frame_capture(&frame->functions) < 0 ||
        shell_readonly_frame_capture(&frame->readonly) < 0 ||
        shell_trap_enter_child(&frame->traps) < 0) {
        shell_subshell_frame_clear(frame);
        return -1;
    }
    return 0;
}

static void shell_subshell_frame_restore(struct shell_subshell_frame *frame) {
    if (!frame) return;
    shell_env_frame_restore(&frame->env);
    shell_param_snapshot_restore(&frame->params);
    shell_alias_frame_restore(&frame->aliases);
    shell_function_frame_restore(&frame->functions);
    shell_readonly_frame_restore(&frame->readonly);
    shell_trap_restore(&frame->traps);
    shell_restore_opts(&frame->opts);
    shell_restore_cwd_only(frame->cwd);
    g_shell_flow = frame->flow;
    g_shell_flow_status = frame->flow_status;
    g_shell_loop_depth = frame->loop_depth;
    /* Anything the subshell installed with `exec` dies with it. The
     * handles it opened are deliberately not closed here: a clone may
     * still be referenced by a spawned child. */
    for (int i = 0; i < SHELL_FD_MAX; i++) g_shell_fd[i] = frame->fd[i];
}

static bool shell_subshell_tail_can_follow(const char *tail) {
    tail = shell_skip_blanks(tail);
    if (!*tail) return true;
    if (*tail == ';') {
        tail = shell_skip_blanks(tail + 1);
        return *tail == '\0';
    }
    if (*tail == '<' || *tail == '>') return true;
    if (shell_is_digit(*tail)) {
        while (shell_is_digit(*tail)) tail++;
        return *tail == '<' || *tail == '>';
    }
    return false;
}

static bool shell_try_subshell_command(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (*s != '(') return false;

    const char *body_start = s + 1;
    const char *body_end = 0;
    int depth = 1;
    bool in_single = false;
    bool in_double = false;
    for (const char *p = body_start; *p; p++) {
        if (in_single) {
            if (*p == '\'') in_single = false;
            continue;
        }
        if (in_double) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') in_double = false;
            continue;
        }
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '\'') {
            in_single = true;
            continue;
        }
        if (*p == '"') {
            in_double = true;
            continue;
        }
        if (*p == '(') {
            depth++;
            continue;
        }
        if (*p == ')') {
            if (depth > 1) {
                depth--;
                continue;
            }
            if (shell_subshell_tail_can_follow(p + 1)) {
                body_end = p;
                break;
            }
        }
    }
    if (!body_end) {
        kprintf("subshell: expected ')'\n");
        shell_set_status(2);
        return true;
    }

    char body[LINE_MAX];
    if (shell_copy_segment(body, sizeof(body), body_start, body_end) < 0) {
        kprintf("subshell: body too long\n");
        shell_set_status(2);
        return true;
    }

    struct shell_simple redirs;
    shell_simple_init(&redirs);
    const char *tail = shell_skip_blanks(body_end + 1);

    /* `( ... ) &` -- background the whole compound.
     *
     * Only redirections were accepted after `( ... )`, so a trailing `&`
     * was a syntax error and the compound never ran at all. That is
     * also why the corpus saw `wait $!` return 127: not a broken
     * wait, a job that was never created because its command failed
     * to parse. */
    if (tail[0] == '&' && tail[1] != '&') {
        const char *after = shell_skip_blanks(tail + 1);
        if (!*after || *after == ';') {
#ifdef SHELL_HOSTED
            shell_set_status(shell_background_forked(body, "subshell"));
#else
            kprintf("subshell: can't be backgrounded in the kernel shell\n");
            shell_set_status(1);
#endif
            return true;
        }
    }

    if (*tail) {
        struct shell_token tok[SHELL_TOKEN_MAX];
        char words[SHELL_PARSE_BUF_MAX];
        int ntok = 0;
        if (shell_tokenize(tail, tok, &ntok, words, sizeof(words)) < 0) {
            shell_set_status(2);
            return true;
        }
        int i = 0;
        struct shell_pipeline pl;
        int parsed = shell_parse_pipeline(tok, ntok, &i, &pl);
        if (parsed < 0 || parsed == 0 || pl.count != 1 ||
            pl.stage[0].argc != 0) {
            kprintf("subshell: expected only redirections after ')'\n");
            shell_set_status(2);
            return true;
        }
        enum shell_tok_type sep = shell_consume_separator(tok, ntok, &i);
        if (sep != SH_TOK_SEMI || i < ntok) {
            kprintf("subshell: unexpected text after redirection\n");
            shell_set_status(2);
            return true;
        }
        redirs = pl.stage[0];
    }

    struct shell_subshell_frame state;
    if (shell_subshell_frame_capture(&state) < 0) {
        kprintf("subshell: failed to save shell state\n");
        shell_set_status(1);
        return true;
    }

    struct shell_io_frame io_frame;
    bool io_active = false;
    if (redirs.redir_count > 0) {
        if (shell_enter_io_frame(&redirs, "subshell", &io_frame) < 0) {
            shell_subshell_frame_restore(&state);
            shell_set_status(1);
            return true;
        }
        io_active = true;
    }

    g_subshell_depth++;
    g_shell_flow = SHELL_FLOW_NONE;
    g_shell_flow_status = 0;
    g_shell_loop_depth = 0;

    execute_line_text(body);
    int rc = g_last_status;
    if (g_shell_flow == SHELL_FLOW_EXIT) {
        rc = g_shell_flow_status;
    }
    rc = shell_run_exit_trap(rc);

    g_subshell_depth--;
    if (io_active) shell_restore_io_frame(&io_frame);
    shell_subshell_frame_restore(&state);
    shell_set_status(rc);
    return true;
}

static bool shell_try_group_command(const char *src) {
    const char *s = shell_skip_blanks(src);
    if (*s != '{') return false;

    const char *body_start = s + 1;
    const char *body_end = 0;
    int depth = 1;
    bool in_single = false;
    bool in_double = false;
    for (const char *p = body_start; *p; p++) {
        if (in_single) {
            if (*p == '\'') in_single = false;
            continue;
        }
        if (in_double) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') in_double = false;
            continue;
        }
        if (*p == '\'') {
            in_single = true;
            continue;
        }
        if (*p == '"') {
            in_double = true;
            continue;
        }
        if (*p == '{') {
            depth++;
            continue;
        }
        if (*p == '}') {
            depth--;
            if (depth == 0) {
                body_end = p;
                break;
            }
        }
    }
    if (!body_end) {
        kprintf("group: expected '}'\n");
        shell_set_status(2);
        return true;
    }

    char body[LINE_MAX];
    if (shell_copy_segment(body, sizeof(body), body_start, body_end) < 0) {
        kprintf("group: body too long\n");
        shell_set_status(2);
        return true;
    }

    struct shell_simple redirs;
    shell_simple_init(&redirs);
    const char *tail = shell_skip_blanks(body_end + 1);

    /* `{ ...; } &` -- background the whole compound.
     *
     * Only redirections were accepted after `{ ...; }`, so a trailing `&`
     * was a syntax error and the compound never ran at all. That is
     * also why the corpus saw `wait $!` return 127: not a broken
     * wait, a job that was never created because its command failed
     * to parse. */
    if (tail[0] == '&' && tail[1] != '&') {
        const char *after = shell_skip_blanks(tail + 1);
        if (!*after || *after == ';') {
#ifdef SHELL_HOSTED
            shell_set_status(shell_background_forked(body, "group"));
#else
            kprintf("group: can't be backgrounded in the kernel shell\n");
            shell_set_status(1);
#endif
            return true;
        }
    }

    if (*tail) {
        struct shell_token tok[SHELL_TOKEN_MAX];
        char words[SHELL_PARSE_BUF_MAX];
        int ntok = 0;
        if (shell_tokenize(tail, tok, &ntok, words, sizeof(words)) < 0) {
            shell_set_status(2);
            return true;
        }
        int i = 0;
        struct shell_pipeline pl;
        int parsed = shell_parse_pipeline(tok, ntok, &i, &pl);
        if (parsed < 0 || parsed == 0 || pl.count != 1 ||
            pl.stage[0].argc != 0) {
            kprintf("group: expected only redirections after '}'\n");
            shell_set_status(2);
            return true;
        }
        enum shell_tok_type sep = shell_consume_separator(tok, ntok, &i);
        if (sep != SH_TOK_SEMI || i < ntok) {
            kprintf("group: unexpected text after redirection\n");
            shell_set_status(2);
            return true;
        }
        redirs = pl.stage[0];
    }

    struct shell_io_frame io_frame;
    bool io_active = false;
    if (redirs.redir_count > 0) {
        if (shell_enter_io_frame(&redirs, "group", &io_frame) < 0) {
            shell_set_status(1);
            return true;
        }
        io_active = true;
    }

    execute_line_text(body);
    int rc = g_last_status;
    if (io_active) shell_restore_io_frame(&io_frame);
    shell_set_status(rc);
    return true;
}

/* ---- top-level command lists --------------------------------------------
 *
 * A line is split into its `;` / `&&` / `||` segments HERE, before anything
 * is tokenized, and each segment is executed on its own. Two bugs die with
 * this, and they were the two most consequential ones the parity gate found:
 *
 *  1. `$?` was a line ahead of itself. shell_tokenize() EXPANDS as it
 *     tokenizes, and it was handed the whole line, so `false; echo $?`
 *     expanded `$?` before `false` ever ran and printed the status of
 *     whatever came before. The same ordering bug applied to `$( )` -- a
 *     command substitution in the second half of a line ran before the first
 *     half, side effects and all.
 *
 *  2. Anything after a compound command was silently dropped. The compound
 *     parsers each located their terminator and then ignored the rest of the
 *     line -- shell_try_if_command literally computed `fi_skip` and threw it
 *     away -- so `if a; then b; fi; echo c` never echoed. Harmless while
 *     compounds had to be one-liners; fatal once multi-line compounds got
 *     joined into one line. Splitting first means a compound parser only
 *     ever receives its own text.
 *
 * The scan must not split inside quotes, `$( )`, backticks, a compound
 * (if/for/while/until/case ... fi/done/esac), a `{ }` group, or a `( )`
 * subshell -- and `;;` is a case-clause terminator, not two separators.
 */

enum shell_list_link { SH_LINK_SEMI, SH_LINK_AND, SH_LINK_OR };

/* A `{` or `}` only opens/closes a group when it stands as its own word;
 * otherwise it is brace expansion, `${...}`, or part of a filename. */
static bool shell_group_open_at(const char *p) {
    return *p == '{' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0');
}
static bool shell_group_close_at(const char *p) {
    return *p == '}' && (p[1] == '\0' || p[1] == ' ' || p[1] == '\t' ||
                         p[1] == ';' || p[1] == '&' || p[1] == '|');
}

static const char *shell_find_list_sep(const char *s, enum shell_list_link *link,
                                       int *oplen) {
    bool in_sq = false, in_dq = false, in_bt = false;
    int paren = 0, compound = 0, brace = 0;
    const char *start = s;

    for (const char *p = s; *p; p++) {
        if (in_sq) { if (*p == '\'') in_sq = false; continue; }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (in_bt) { if (*p == '`') in_bt = false; continue; }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"')  { in_dq = true; continue; }
        if (*p == '`')  { in_bt = true; continue; }

        /* `$(`, `$((` and a bare `(` subshell all nest the same way here: we
         * only care that their contents are off limits to the splitter. */
        if (*p == '$' && p[1] == '(') { paren++; p++; continue; }
        if (*p == '(') { paren++; continue; }
        if (*p == ')') { if (paren > 0) paren--; continue; }
        if (paren > 0) continue;

        if (shell_word_boundary_before(start, p)) {
            if (shell_starts_with_word(p, "if")    ||
                shell_starts_with_word(p, "for")   ||
                shell_starts_with_word(p, "while") ||
                shell_starts_with_word(p, "until") ||
                shell_starts_with_word(p, "case")) { compound++; continue; }
            if (shell_starts_with_word(p, "fi")   ||
                shell_starts_with_word(p, "done") ||
                shell_starts_with_word(p, "esac")) {
                if (compound > 0) compound--;
                continue;
            }
            if (shell_group_open_at(p))  { brace++; continue; }
            if (shell_group_close_at(p)) { if (brace > 0) brace--; continue; }
        }
        if (compound > 0 || brace > 0) continue;

        if (*p == ';') {
            if (p[1] == ';') { p++; continue; }      /* case clause end */
            *link = SH_LINK_SEMI; *oplen = 1;
            return p;
        }
        if (*p == '&' && p[1] == '&') { *link = SH_LINK_AND; *oplen = 2; return p; }
        if (*p == '|' && p[1] == '|') { *link = SH_LINK_OR;  *oplen = 2; return p; }
    }
    return 0;
}

/* Returns false if `src` has no top-level separator, so the caller runs it
 * exactly as it did before -- single-command lines take an identical path. */
static bool shell_run_list_line(const char *src) {
    enum shell_list_link link = SH_LINK_SEMI;
    int oplen = 0;
    if (!shell_find_list_sep(src, &link, &oplen)) return false;

    const char *cur = src;
    enum shell_list_link prev = SH_LINK_SEMI;
    for (;;) {
        link = SH_LINK_SEMI;
        oplen = 0;
        const char *sep = shell_find_list_sep(cur, &link, &oplen);
        const char *end = sep ? sep : cur + strlen(cur);

        /* Heap: execute_line_text recurses (a compound body re-enters it),
         * and a kilobyte per level adds up fast on a kernel stack. */
        char *seg = kmalloc(SHELL_PARSE_BUF_MAX);
        if (!seg) {
            kprintf("shell: out of memory\n");
            shell_set_status(2);
            return true;
        }
        if (shell_copy_segment(seg, SHELL_PARSE_BUF_MAX, cur, end) < 0) {
            kfree(seg);
            kprintf("shell: command too long\n");
            shell_set_status(2);
            return true;
        }
        if (*shell_skip_blanks(seg)) {
            /* A skipped segment leaves the status alone, which is what makes
             * `false && a && b` skip BOTH a and b rather than reconsidering. */
            bool should_run = (prev == SH_LINK_SEMI) ||
                              (prev == SH_LINK_AND && g_last_status == 0) ||
                              (prev == SH_LINK_OR  && g_last_status != 0);
            if (should_run) {
                /* `set -e` does not act on a command that is followed by && or
                 * || -- POSIX exempts every command in an AND-OR list except
                 * the one after the final operator. `link` is the separator
                 * that FOLLOWS this segment, and is SH_LINK_SEMI for the last
                 * one, so the final command is judged normally and
                 * `set -e; false || false` still exits.
                 *
                 * This is the only place the rule can live: the line is split
                 * here, BEFORE tokenizing, so the token-level executor never
                 * sees an operator to key off. Implementing it there instead
                 * changed nothing at all, and `set -e; false && echo hi` kept
                 * killing the shell. */
                bool exempt = (sep != 0) &&
                              (link == SH_LINK_AND || link == SH_LINK_OR);
                if (exempt) g_errexit_suspend++;
                execute_line_text(seg);
                if (exempt) g_errexit_suspend--;
                if (g_shell_flow != SHELL_FLOW_NONE) { kfree(seg); return true; }
            }
        }
        kfree(seg);
        if (!sep) break;
        prev = link;
        cur = sep + oplen;
    }
    return true;
}

/* ---- pipelines whose stages are compound commands -----------------------
 *
 * POSIX 2.9.2 lets any pipeline stage be a compound command, and
 * `cmd | while read l; do ...; done` is one of the most common shapes in real
 * scripts. The token-level pipeline parser cannot express it: by the time it
 * runs, `while` is just a word, so the stage was dispatched as a program and
 * the shell reported `failed to spawn '/bin/while'`.
 *
 * So this catches such pipelines BEFORE tokenizing, where the stage is still
 * text, and runs each stage through execute_line_text() with the pipe wired
 * into the shell's own descriptors.
 *
 * Stages run SEQUENTIALLY -- stage N runs to completion, its write end is
 * closed, then stage N+1 reads. That is the same shape the existing builtin
 * pipeline stages already use, and it is bounded by the 64 KiB pipe buffer:
 * a stage producing more than that before the next one starts would block.
 * Real bash runs stages concurrently; this does not, and a genuinely
 * streaming pipeline is the one case where the difference is observable.
 */
static const char *shell_find_pipe_at(const char *s) {
    bool in_sq = false, in_dq = false, in_bt = false;
    int paren = 0, compound = 0, brace = 0;
    const char *start = s;

    for (const char *p = s; *p; p++) {
        if (in_sq) { if (*p == '\'') in_sq = false; continue; }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (in_bt) { if (*p == '`') in_bt = false; continue; }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"')  { in_dq = true; continue; }
        if (*p == '`')  { in_bt = true; continue; }
        if (*p == '$' && p[1] == '(') { paren++; p++; continue; }
        if (*p == '(') { paren++; continue; }
        if (*p == ')') { if (paren > 0) paren--; continue; }
        if (paren > 0) continue;

        if (shell_word_boundary_before(start, p)) {
            if (shell_starts_with_word(p, "if")    ||
                shell_starts_with_word(p, "for")   ||
                shell_starts_with_word(p, "while") ||
                shell_starts_with_word(p, "until") ||
                shell_starts_with_word(p, "case")) { compound++; continue; }
            if (shell_starts_with_word(p, "fi")   ||
                shell_starts_with_word(p, "done") ||
                shell_starts_with_word(p, "esac")) {
                if (compound > 0) compound--;
                continue;
            }
            if (shell_group_open_at(p))  { brace++; continue; }
            if (shell_group_close_at(p)) { if (brace > 0) brace--; continue; }
        }
        if (compound > 0 || brace > 0) continue;

        if (*p == '|') {
            if (p[1] == '|') { p++; continue; }   /* an OR, not a pipe */
            return p;
        }
    }
    return 0;
}

static bool shell_stage_is_compound(const char *s) {
    s = shell_skip_blanks(s);
    return shell_starts_with_word(s, "while") ||
           shell_starts_with_word(s, "until") ||
           shell_starts_with_word(s, "for")   ||
           shell_starts_with_word(s, "if")    ||
           shell_starts_with_word(s, "case")  ||
           shell_group_open_at(s) || *s == '(';
}

/* Returns false when this is not a pipeline, or when no stage is compound --
 * in which case the existing token-level path handles it exactly as before. */
static bool shell_try_compound_pipeline(const char *src) {
    if (!shell_find_pipe_at(src)) return false;

    char (*stages)[SHELL_PARSE_BUF_MAX] =
        kmalloc(sizeof(*stages) * SHELL_STAGE_MAX);
    if (!stages) return false;

    int n = 0;
    bool any_compound = false, overflow = false;
    const char *cur = src;
    for (;;) {
        const char *sep = shell_find_pipe_at(cur);
        const char *end = sep ? sep : cur + strlen(cur);
        if (n >= SHELL_STAGE_MAX) { overflow = true; break; }
        if (shell_copy_segment(stages[n], SHELL_PARSE_BUF_MAX, cur, end) < 0) {
            overflow = true;
            break;
        }
        if (shell_stage_is_compound(stages[n])) any_compound = true;
        n++;
        if (!sep) break;
        cur = sep + 1;
    }
    if (overflow || n < 2 || !any_compound) {
        kfree(stages);
        return false;
    }

    struct file *saved0 = g_shell_fd[0], *saved1 = g_shell_fd[1];
    struct file *prev_in = 0;
    int last = 0;

    for (int i = 0; i < n; i++) {
        struct file *r = 0, *w = 0;
        if (i + 1 < n && pipe_create(&r, &w) != 0) {
            kprintf("pipeline: pipe_create failed\n");
            break;
        }
        if (prev_in) g_shell_fd[0] = prev_in;
        if (w)       g_shell_fd[1] = w;

        execute_line_text(stages[i]);
        last = g_last_status;

        g_shell_fd[0] = saved0;
        g_shell_fd[1] = saved1;

        /* Close the write end before the reader runs, or it never sees EOF. */
        if (w) file_close(w);
        if (prev_in) file_close(prev_in);
        prev_in = r;

        if (g_shell_flow != SHELL_FLOW_NONE) break;
    }
    if (prev_in) file_close(prev_in);

    g_shell_fd[0] = saved0;
    g_shell_fd[1] = saved1;
    kfree(stages);
    shell_set_status(last);
    return true;
}

/* Syntax check for `set -n`.
 *
 * POSIX: "read commands but do not execute them", which exists so a script
 * can be checked without side effects. Merely skipping execution -- what this
 * used to do -- reports success on a file full of unbalanced `if`s, which
 * makes the option useless for the one job it has. Returns 0 if the text is
 * well formed, or writes a diagnostic and returns non-zero.
 *
 * This is a structural check over the same features the parser recognises:
 * quoting, compound-command balance, and the operators that must be followed
 * by a command. It is not a full grammar -- the parser is not one either. */
static int shell_syntax_check(const char *s) {
    int if_depth = 0, do_depth = 0, case_depth = 0;
    int brace = 0, paren = 0, loop_open = 0;
    bool in_sq = false, in_dq = false;

    for (const char *p = s; *p; p++) {
        if (in_sq) { if (*p == '\'') in_sq = false; continue; }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"')  { in_dq = true; continue; }
        if (*p == '#' && (p == s || is_space(p[-1]))) {
            while (*p && *p != '\n') p++;
            if (!*p) break;
            continue;
        }
        if (*p == '{') brace++;
        else if (*p == '}') brace--;
        else if (*p == '(') paren++;
        else if (*p == ')') paren--;

        if (!(p == s || is_space(p[-1]) || p[-1] == ';' || p[-1] == '\n'))
            continue;
        if (shell_starts_with_word(p, "if")) if_depth++;
        else if (shell_starts_with_word(p, "fi")) if_depth--;
        else if (shell_starts_with_word(p, "for") ||
                 shell_starts_with_word(p, "while") ||
                 shell_starts_with_word(p, "until")) loop_open++;
        else if (shell_starts_with_word(p, "do")) {
            do_depth++;
            if (loop_open > 0) loop_open--;
        }
        else if (shell_starts_with_word(p, "done")) do_depth--;
        else if (shell_starts_with_word(p, "case")) case_depth++;
        else if (shell_starts_with_word(p, "esac")) case_depth--;
    }

    const char *what = 0;
    if (in_sq)            what = "unterminated single quote";
    else if (in_dq)       what = "unterminated double quote";
    else if (if_depth > 0)   what = "'if' without matching 'fi'";
    else if (if_depth < 0)   what = "'fi' without matching 'if'";
    else if (do_depth > 0)   what = "'do' without matching 'done'";
    else if (do_depth < 0)   what = "'done' without matching 'do'";
    else if (loop_open > 0)  what = "loop without 'do'";
    else if (case_depth > 0) what = "'case' without matching 'esac'";
    else if (case_depth < 0) what = "'esac' without matching 'case'";
    else if (brace > 0)      what = "'{' without matching '}'";
    else if (brace < 0)      what = "'}' without matching '{'";
    else if (paren > 0)      what = "'(' without matching ')'";
    else if (paren < 0)      what = "')' without matching '('";

    if (!what) {
        /* A trailing operator has nothing to operate on. */
        const char *end = s + strlen(s);
        while (end > s && (is_space(end[-1]) || end[-1] == '\n')) end--;
        if (end > s && (end[-1] == '|' || end[-1] == '&')) {
            bool doubled = (end - s >= 2 && end[-2] == end[-1]);
            if (end[-1] == '|' || doubled) what = "unexpected end after operator";
        }
    }
    if (what) {
        kprintf("sh: syntax error: %s\n", what);
        return 2;
    }
    return 0;
}

static void execute_line_text_inner(const char *src) {
    struct shell_token tok[SHELL_TOKEN_MAX];
    char words[SHELL_PARSE_BUF_MAX];
    int ntok = 0;

    src = src ? src : "";
    if (g_shell_flow != SHELL_FLOW_NONE) return;
    if (g_opt_verbose) kprintf("%s\n", src);
    shell_check_pending_signals();
    if (g_shell_flow != SHELL_FLOW_NONE) return;
    /* Before tokenizing (which expands): split `;` / `&&` / `||` and run each
     * piece on its own, so expansion happens per command. See above. This
     * precedes the function-definition check so that `f() { ...; }; f` gives
     * that check a clean definition rather than one with a call glued on. */
    /* `set -n`: read the command and check it, but run nothing. The option
     * was recorded and reported in $- but never acted on, so a script asking
     * to be validated executed in full. */
    if (g_opt_noexec) {
        int rc = shell_syntax_check(src);
        if (rc) g_noexec_error = rc;
        shell_set_status(0);
        return;
    }
    if (shell_run_list_line(src)) return;
    if (shell_try_function_definition(src)) return;
    /* A pipeline with a compound stage must be split while the stages are
     * still TEXT; the tokenizer would reduce `while` to an ordinary word. */
    if (shell_try_compound_pipeline(src)) return;
    if (shell_try_if_command(src) ||
        shell_try_for_command(src) ||
        shell_try_while_command(src) ||
        shell_try_until_command(src) ||
        shell_try_case_command(src) ||
        shell_try_group_command(src) ||
        shell_try_subshell_command(src)) {
        return;
    }

    if (shell_tokenize(src, tok, &ntok, words, sizeof(words)) < 0) {
        shell_set_status(2);
        return;
    }
    if (ntok == 0) return;

    int i = 0;
    enum shell_tok_type prev_link = SH_TOK_SEMI;
    int last = g_last_status;

    while (i < ntok) {
        while (i < ntok && tok[i].type == SH_TOK_SEMI) i++;
        if (i >= ntok) break;

        bool should_run = (prev_link == SH_TOK_SEMI || prev_link == SH_TOK_BG) ||
                          (prev_link == SH_TOK_AND_IF && last == 0) ||
                          (prev_link == SH_TOK_OR_IF && last != 0);

        bool negate = false;
        while (i < ntok && tok[i].type == SH_TOK_WORD &&
               strcmp(tok[i].text, "!") == 0) {
            negate = !negate;
            i++;
        }
        if (i >= ntok || shell_is_list_sep(tok[i].type)) {
            if (negate) {
                last = g_last_status == 0 ? 1 : 0;
                shell_set_status(last);
            }
            enum shell_tok_type sep2 = shell_consume_separator(tok, ntok, &i);
            prev_link = sep2;
            continue;
        }

        struct shell_pipeline pl;
        int parsed = shell_parse_pipeline(tok, ntok, &i, &pl);
        if (parsed < 0) {
            shell_set_status(2);
            return;
        }
        if (parsed == 0) {
            kprintf("shell: expected command\n");
            shell_set_status(2);
            return;
        }

        enum shell_tok_type sep = shell_consume_separator(tok, ntok, &i);
        bool background = (sep == SH_TOK_BG);
        if (should_run) {
            if (g_opt_xtrace) {
                kprintf("+ ");
                for (int s = 0; s < pl.count; s++) {
                    if (s > 0) kprintf("| ");
                    for (int a = 0; a < pl.stage[s].argc; a++)
                        kprintf("%s%s", pl.stage[s].argv[a],
                                a + 1 < pl.stage[s].argc ? " " : "");
                }
                kprintf("\n");
            }
            last = shell_run_pipeline(&pl, background);
            if (negate) last = last == 0 ? 1 : 0;
            shell_set_status(last);
            if (g_shell_flow != SHELL_FLOW_NONE) return;
            /* A segment reaching here has already been split off any && or ||
             * by shell_run_list_line, which is where the AND-OR exemption
             * lives; `sep` is all but always SH_TOK_SEMI. It is still tested
             * because a `;`-separated list does reach this loop. */
            bool in_and_or = (prev_link == SH_TOK_AND_IF ||
                              prev_link == SH_TOK_OR_IF ||
                              sep == SH_TOK_AND_IF || sep == SH_TOK_OR_IF);
            if (g_opt_errexit && last != 0 && !negate &&
                !in_and_or && g_errexit_suspend == 0) {
                g_shell_flow = SHELL_FLOW_EXIT;
                g_shell_flow_status = last;
                return;
            }
        }
        prev_link = sep;
    }
}

/* See patch note: aliases are a LINE-level rewrite, applied before the line
 * is split or parsed, and never re-applied to the pieces. */
static void execute_line_text(const char *src) {
    if (g_exec_line_depth > 0) {           /* a piece of a line already being run */
        g_exec_line_depth++;
        execute_line_text_inner(src);
        g_exec_line_depth--;
        return;
    }

    char *abuf = (char *)kmalloc(SHELL_PARSE_BUF_MAX);
    const char *line_src = src;
    if (abuf) {
        const char *expanded = shell_expand_aliases(src ? src : "", abuf,
                                                    SHELL_PARSE_BUF_MAX);
        if (expanded) line_src = expanded;
        else {
            /* Expansion overflowed. Say so rather than silently running the
             * unexpanded text, which would look like the alias never existed. */
            kprintf("shell: alias expansion too long\n");
            shell_set_status(2);
            kfree(abuf);
            return;
        }
    }

    g_exec_line_depth++;
    execute_line_text_inner(line_src);
    g_exec_line_depth--;
    if (abuf) kfree(abuf);
}

static char g_heredoc_cmd[LINE_MAX];
static char g_heredoc_delim[64];
static bool g_heredoc_strip_tabs;
static bool g_heredoc_quoted;
static char g_heredoc_body[SHELL_HEREDOC_BODY_MAX];
static size_t g_heredoc_body_len;

static char g_continuation_buf[LINE_MAX * 4];
static size_t g_continuation_len;

static bool shell_line_needs_continuation(const char *s) {
    int if_depth = 0, do_depth = 0, case_depth = 0;
    int brace_depth = 0, paren_depth = 0;
    bool in_sq = false, in_dq = false;
    bool last_was_pipe = false, last_was_and = false, last_was_or = false;
    bool last_was_backslash = false;

    for (const char *p = s; *p; p++) {
        if (in_sq) {
            if (*p == '\'') in_sq = false;
            continue;
        }
        if (in_dq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_dq = false;
            continue;
        }
        if (*p == '\\' && !p[1]) { last_was_backslash = true; continue; }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '\'') { in_sq = true; continue; }
        if (*p == '"') { in_dq = true; continue; }
        if (*p == '{') brace_depth++;
        if (*p == '}') brace_depth--;
        if (*p == '(') paren_depth++;
        if (*p == ')') paren_depth--;

        last_was_pipe = false;
        last_was_and = false;
        last_was_or = false;

        if (shell_starts_with_word(p, "if") && (p == s || is_space(p[-1]) || p[-1] == ';'))
            if_depth++;
        if (shell_starts_with_word(p, "then") && (p == s || is_space(p[-1]) || p[-1] == ';'))
            {} /* then is part of if, no depth change */
        if (shell_starts_with_word(p, "fi") && (p == s || is_space(p[-1]) || p[-1] == ';'))
            if_depth--;
        if ((shell_starts_with_word(p, "do") && (p == s || is_space(p[-1]) || p[-1] == ';')))
            do_depth++;
        if (shell_starts_with_word(p, "done") && (p == s || is_space(p[-1]) || p[-1] == ';'))
            do_depth--;
        if (shell_starts_with_word(p, "case") && (p == s || is_space(p[-1]) || p[-1] == ';'))
            case_depth++;
        if (shell_starts_with_word(p, "esac") && (p == s || is_space(p[-1]) || p[-1] == ';'))
            case_depth--;
    }

    /* Trailing pipe or logical operator means continuation */
    const char *end = s + strlen(s);
    while (end > s && is_space(end[-1])) end--;
    if (end > s && end[-1] == '|' && !(end - 1 > s && end[-2] == '|'))
        last_was_pipe = true;
    if (end - s >= 2 && end[-1] == '&' && end[-2] == '&')
        last_was_and = true;
    if (end - s >= 2 && end[-1] == '|' && end[-2] == '|')
        last_was_or = true;

    if (in_sq || in_dq) return true;
    if (last_was_backslash) return true;
    if (if_depth > 0 || do_depth > 0 || case_depth > 0) return true;
    if (brace_depth > 0 || paren_depth > 0) return true;
    if (last_was_pipe || last_was_and || last_was_or) return true;
    return false;
}

static void execute_line(void) {
    shell_latch_pid();
    if (g_heredoc_collecting) {
        const char *check = line;
        if (g_heredoc_strip_tabs)
            while (*check == '\t') check++;
        if (strcmp(check, g_heredoc_delim) == 0) {
            g_heredoc_collecting = false;
            shell_heredoc_reset();
            shell_heredoc_push(g_heredoc_body, g_heredoc_body_len);
            execute_line_text(g_heredoc_cmd);
            shell_heredoc_reset();
        } else {
            /* Same <<- rule as the script reader above -- these are two
             * separate collectors and have drifted apart before. */
            const char *raw = line;
            if (g_heredoc_strip_tabs) while (*raw == '\t') raw++;

            const char *emit = raw;
            char expanded[LINE_MAX * 2];
            if (!g_heredoc_quoted) {
                if (shell_expand_literal_quotes(raw, expanded,
                                                sizeof(expanded)) >= 0)
                    emit = expanded;
            }
            size_t n = strlen(emit);
            if (g_heredoc_body_len + n + 2 <= sizeof(g_heredoc_body)) {
                memcpy(g_heredoc_body + g_heredoc_body_len, emit, n);
                g_heredoc_body_len += n;
                g_heredoc_body[g_heredoc_body_len++] = '\n';
                g_heredoc_body[g_heredoc_body_len] = '\0';
            }
        }
        return;
    }

    char delim[64];
    bool strip_tabs = false, quoted = false;
    if (shell_line_find_heredoc(line, delim, sizeof(delim),
                                &strip_tabs, &quoted)) {
        size_t ll = strlen(line);
        if (ll + 1 <= sizeof(g_heredoc_cmd)) {
            memcpy(g_heredoc_cmd, line, ll + 1);
            memcpy(g_heredoc_delim, delim, strlen(delim) + 1);
            g_heredoc_strip_tabs = strip_tabs;
            g_heredoc_quoted = quoted;
            g_heredoc_body_len = 0;
            g_heredoc_body[0] = '\0';
            g_heredoc_collecting = true;
            return;
        }
    }
    if (g_continuation_active) {
        size_t ll = strlen(line);
        if (g_continuation_len + 1 + ll + 1 <= sizeof(g_continuation_buf)) {
            g_continuation_buf[g_continuation_len++] = '\n';
            memcpy(g_continuation_buf + g_continuation_len, line, ll);
            g_continuation_len += ll;
            g_continuation_buf[g_continuation_len] = '\0';
        }
        if (shell_line_needs_continuation(g_continuation_buf)) return;
        g_continuation_active = false;
        execute_line_text(g_continuation_buf);
        return;
    }

    if (shell_line_needs_continuation(line)) {
        size_t ll = strlen(line);
        if (ll + 1 <= sizeof(g_continuation_buf)) {
            memcpy(g_continuation_buf, line, ll + 1);
            g_continuation_len = ll;
            g_continuation_active = true;
        }
        return;
    }

    execute_line_text(line);
}

void shell_init(void) {
    line_len = 0;
    line[0]  = '\0';
    /* Milestone 25C: stamp boot-time PATH/HOME/USER/PWD/SHELL so any
     * implicit-ELF dispatch immediately has a search path and so any
     * libtoby-linked child sees a meaningful environ at exec. */
    for (int i = 0; i < SHELL_FD_MAX; i++) g_shell_fd[i] = 0;
    env_init_defaults();
    g_getopts_last_optind = 1;
    g_getopts_char_index = 1;
    shell_trap_clear_all();
    if (shell_set_param0("tobysh") < 0) kprintf("shell: failed to set $0\n");
    (void)shell_set_positional_params(0, 0);
    kprintf("type 'help' to list commands.\n");
    prompt();
}

/* Milestone 25C: drive `execute_line` over an arbitrary string. Used
 * from the boot harness to exercise the shell's PATH/env/spawn path
 * without input. Truncates lines that don't fit the editor buffer
 * (with a kprintf so the caller notices) instead of stomping memory. */
void shell_run_test_line(const char *in) {
    if (!in) return;
    uint64_t irqf = spin_lock_irqsave(&g_shell_line_lock);
    size_t n = 0;
    while (in[n] && n + 1 < LINE_MAX) {
        line[n] = in[n];
        n++;
    }
    if (in[n]) {
        kprintf("shell: test line truncated at %u bytes\n",
                (unsigned)(LINE_MAX - 1));
    }
    line[n]  = '\0';
    line_len = n;
    /* Echo the synthetic command so logs read like a normal session. */
    console_set_color(0x00FFCC66);
    kprintf("[shell-test] $ %s\n", line);
    console_set_color(0x00CCCCCC);
    shell_history_add(line);
    execute_line();
    line_len = 0;
    line[0]  = '\0';
    spin_unlock_irqrestore(&g_shell_line_lock, irqf);
}

void shell_poll(void) {
    /* Lazy reap of any background jobs that have terminated since the
     * previous poll. Non-blocking: proc_wait() on an already-TERMINATED
     * child returns immediately. */
    jobs_reap_finished();

    int c;
    while ((c = kbd_trygetc()) >= 0) {
        char ch = (char)c;

        if (ch == '\n') {
            line[line_len] = '\0';
            kputc('\n');
            {
                uint64_t irqf = spin_lock_irqsave(&g_shell_line_lock);
                execute_line();
                line_len = 0;
                line[0]  = '\0';
                spin_unlock_irqrestore(&g_shell_line_lock, irqf);
            }
            prompt();
            continue;
        }

        if (ch == '\b') {
            if (line_len > 0) {
                line_len--;
                line[line_len] = '\0';
                console_backspace();
            }
            continue;
        }

        /* Printable ASCII only; ignore the rest for now. */
        if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) continue;

        if (line_len + 1 >= LINE_MAX) continue;   /* drop overflow */
        line[line_len++] = ch;
        kputc(ch);
    }
}

/* ---- hosted entry points (userspace /bin/tsh) ---------------------------
 *
 * This file is compiled TWICE: once into the kernel, where the shell is
 * driven by the keyboard poll loop above, and once into /bin/tsh, where it is
 * driven by main() in programs/tsh/host.c. Same language, two hosts.
 *
 * That is the whole point. The language core here -- the tokenizer, the
 * ${...} expansion family, the recursive-descent arithmetic evaluator,
 * command substitution, globbing, functions, traps, here-documents -- took a
 * long time to get right, and a second implementation in userspace would have
 * been a second set of the same bugs. The userspace shell that existed before
 * this proved the point: its parser had if/for/while/case, but the expansion
 * layer underneath was mostly missing, and it scored 0/14 against real bash.
 *
 * Everything below is ADDITIVE. No existing function is modified and nothing
 * is compiled out, so the kernel shell's behaviour is unchanged by
 * construction -- these are simply three doors into machinery that was
 * already here but only reachable from the keyboard loop.
 *
 * The host supplies the other side of the seam (kmalloc, vfs_*, proc_spawn,
 * file_*, kprintf...). In the kernel those are the real subsystems; in
 * userspace they are thin wrappers over libtoby syscalls. */

/* Initialise shell state WITHOUT printing a banner or a prompt: a script run
 * must not emit anything the oracle would not. */
void shell_set_interactive_hosted(bool on) { g_interactive = on; }

void shell_init_hosted(const char *argv0) {
    line_len = 0;
    line[0]  = '\0';
    for (int i = 0; i < SHELL_FD_MAX; i++) g_shell_fd[i] = 0;
    env_init_defaults();

#ifdef SHELL_HOSTED
    /* INHERIT THE ENVIRONMENT. env_init_defaults() stamps PATH=/bin, HOME=/,
     * and friends -- which is right for the kernel shell, where there is no
     * parent to inherit from, and wrong for /bin/tsh, where there always is.
     *
     * Without this the hosted shell silently discarded everything its parent
     * exported and ran with a hardcoded PATH of /bin. A command anywhere else
     * on PATH was simply not found: the third-party gate puts its helper
     * binaries in /etc/oilspec/bin and passes PATH=/etc/oilspec/bin:/bin, and
     * 318 cases failed with "failed to launch '/bin/argv.py'" -- the shell had
     * thrown the PATH away and gone looking in the only directory it knew.
     *
     * Imported AFTER the defaults so the real values win, and marked exported
     * because that is what they are: they arrived in the environment, so they
     * belong in a child's environment too.
     *
     * environ is declared here rather than pulled from <unistd.h>: this file
     * speaks the kernel's headers, and the kernel build has no environ at all
     * -- hence the guard. */
    {
        extern char **environ;
        for (char **e = environ; e && *e; e++) {
            if (env_set_kv(*e) != 0) continue;
            size_t klen = env_key_len(*e);
            int idx = env_find(*e, klen);
            if (idx >= 0) g_env_flags[idx] |= SHVAR_EXPORTED;
        }
    }
#endif

    g_getopts_last_optind = 1;
    g_getopts_char_index  = 1;
    shell_trap_clear_all();
    (void)shell_set_param0(argv0 ? argv0 : "tsh");
    (void)shell_set_positional_params(0, 0);
}

/* Run a script file to completion; returns its exit status. */
int shell_run_script_hosted(const char *path) {
    return shell_run_script_path(path, true);
}

/* Run one command line (the `-c` form); returns its exit status. */
int shell_run_line_hosted(const char *text) {
    if (!text) return 0;
    char *copy = shell_strdup(text);
    if (!copy) return 2;
    int rc = shell_run_script_text(copy, true);
    kfree(copy);
    return rc;
}

/* Set the positional parameters ($1..$n) from a hosted caller. */
int shell_set_args_hosted(int argc, char **argv) {
    return shell_set_positional_params(argc, argv);
}
