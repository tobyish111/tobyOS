/* shell.c -- line editor + builtin command dispatch.
 *
 * The editor stores the in-progress line in a fixed buffer. Printable
 * ASCII appends to the buffer and echoes; backspace erases and rewinds;
 * Enter NUL-terminates, calls execute_line(), and prints the next prompt.
 *
 * Tokenisation is in-place: whitespace runs become NULs and we collect
 * pointers to each non-empty token in argv[]. That means once we've
 * dispatched, the buffer is shredded -- callers must not retain pointers
 * across shell_poll() calls.
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

extern volatile struct limine_module_request module_req;

#define LINE_MAX 256
#define ARG_MAX  16

static char line[LINE_MAX];
static size_t line_len;

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
            console_set_color(0x0066FF66);
            kprintf("tobyOS> ");
            console_set_color(0x00CCCCCC);
            for (size_t k = 0; k < line_len; k++) kputc(line[k]);
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

#define ENV_MAX 32

static char *g_env[ENV_MAX + 1];     /* +1 reserved for NULL terminator */
static int   g_envc = 0;

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
    for (int i = idx; i < g_envc - 1; i++) g_env[i] = g_env[i + 1];
    g_envc--;
    g_env[g_envc] = 0;
}

/* Install a fully-formed "KEY=VALUE" string. `kv_in` is COPIED -- the
 * caller retains ownership of its storage. Replaces an existing key
 * in place (frees the previous slot) so the table stays compact. */
static int env_set_kv(const char *kv_in) {
    if (!kv_in) return -1;
    size_t klen = env_key_len(kv_in);
    if (klen == 0) return -1;             /* "=value" or "" -- reject */

    size_t total = strlen(kv_in);
    char *blob = (char *)kmalloc(total + 1);
    if (!blob) return -1;
    memcpy(blob, kv_in, total + 1);

    int idx = env_find(kv_in, klen);
    if (idx >= 0) {
        kfree(g_env[idx]);
        g_env[idx] = blob;
        return 0;
    }
    if (g_envc >= ENV_MAX) {
        kfree(blob);
        kprintf("env: table full (max %d)\n", ENV_MAX);
        return -1;
    }
    g_env[g_envc++] = blob;
    g_env[g_envc]   = 0;
    return 0;
}

/* Convenience: build "KEY=VALUE" from two NUL-terminated halves and
 * hand it to env_set_kv. */
static int env_set(const char *key, const char *val) {
    if (!key || !val) return -1;
    size_t klen = strlen(key);
    size_t vlen = strlen(val);
    if (klen == 0) return -1;
    char *tmp = (char *)kmalloc(klen + 1 + vlen + 1);
    if (!tmp) return -1;
    memcpy(tmp, key, klen);
    tmp[klen] = '=';
    memcpy(tmp + klen + 1, val, vlen + 1);
    int rc = env_set_kv(tmp);
    kfree(tmp);
    return rc;
}

static void env_unset(const char *key) {
    if (!key) return;
    size_t klen = strlen(key);
    int idx = env_find(key, klen);
    if (idx >= 0) env_remove_at(idx);
}

/* Stamp the boot-time defaults. Called once from shell_init. Failures
 * are logged but non-fatal -- a degraded env is still usable. */
static void env_init_defaults(void) {
    g_envc = 0;
    g_env[0] = 0;
    if (env_set("PATH",  "/bin")    < 0) kprintf("env: default PATH set failed\n");
    if (env_set("HOME",  "/")       < 0) kprintf("env: default HOME set failed\n");
    if (env_set("USER",  "admin")   < 0) kprintf("env: default USER set failed\n");
    if (env_set("PWD",   "/")       < 0) kprintf("env: default PWD set failed\n");
    if (env_set("SHELL", "tobysh")  < 0) kprintf("env: default SHELL set failed\n");
}

/* ---- builtins ---- */

typedef void (*cmd_fn_t)(int argc, char **argv);

struct cmd {
    const char *name;
    const char *help;
    cmd_fn_t    fn;
};

static const struct cmd cmds[];   /* forward */

/* `env`         -- print KEY=VALUE for every entry
 * `env K=V`     -- shortcut for `setenv K V`
 * `setenv K V`  -- create/replace
 * `unsetenv K`  -- remove */
static void cmd_env(int argc, char **argv) {
    if (argc <= 1) {
        for (int i = 0; i < g_envc; i++) kprintf("%s\n", g_env[i]);
        return;
    }
    /* "env K=V [K=V ...]" -- treat each arg as a literal "KEY=VALUE"
     * blob and install via env_set_kv. */
    for (int i = 1; i < argc; i++) {
        size_t klen = env_key_len(argv[i]);
        const char *eq = (klen > 0) ? (argv[i] + klen) : 0;
        if (!eq || *eq != '=') {
            kprintf("env: '%s' is not KEY=VALUE\n", argv[i]);
            continue;
        }
        if (env_set_kv(argv[i]) < 0) {
            kprintf("env: set '%s' failed\n", argv[i]);
        }
    }
}

static void cmd_setenv(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: setenv KEY VALUE\n");
        return;
    }
    if (env_set(argv[1], argv[2]) < 0) {
        kprintf("setenv: failed to set '%s'\n", argv[1]);
    }
}

static void cmd_unsetenv(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: unsetenv KEY\n");
        return;
    }
    env_unset(argv[1]);
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("commands:\n");
    for (const struct cmd *c = cmds; c->name; c++) {
        kprintf("  %-8s  %s\n", c->name, c->help);
    }
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    }
    kprintf("\n");
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

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: run [--sandbox <profile>] <path> [args...]\n");
        kprintf("       (try 'run /bin/hello' or just 'hello')\n");
        kprintf("       'caps' lists available sandbox profiles\n");
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
        kprintf("  [%d]  pid=%-3d  %-10s  %s\n",
                g_jobs[i].id, g_jobs[i].pid, st, g_jobs[i].name);
        shown++;
    }
    if (shown == 0) kprintf("  (no background jobs)\n");
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
        return;
    }
    int jid;
    if (parse_int(argv[1], &jid) < 0 || jid <= 0) {
        kprintf("fg: bad job id '%s'\n", argv[1]);
        return;
    }
    struct job *j = jobs_find(jid);
    if (!j) {
        kprintf("fg: no such job [%d]\n", jid);
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
    const char *path = (argi < argc) ? argv[argi] : "/";
    struct vfs_dir d;
    int rc = vfs_opendir(path, &d);
    if (rc != VFS_OK) {
        kprintf("ls: '%s': %s\n", path, vfs_strerror(rc));
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
            return;
        }
        int uid = 0, gid = 0;
        for (const char *p = argv[3]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("users: bad uid\n"); return; }
            uid = uid * 10 + (*p - '0');
        }
        for (const char *p = argv[4]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("users: bad gid\n"); return; }
            gid = gid * 10 + (*p - '0');
        }
        if (current_proc() && current_proc()->uid != 0) {
            kprintf("users: only root may add users\n");
            return;
        }
        if (users_add(argv[2], uid, gid) != 0) {
            kprintf("users: add failed\n");
            return;
        }
        if (users_save() != 0) {
            kprintf("users: warning -- could not persist (kept in RAM)\n");
        } else {
            kprintf("users: added '%s' uid=%d gid=%d\n", argv[2], uid, gid);
        }
        return;
    }
    kprintf("registered users:\n");
    users_visit(users_print_one, 0);
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
        return;
    }
    int mode = parse_octal_str(argv[1]);
    if (mode < 0 || mode > 0777) {
        kprintf("chmod: bad mode '%s' -- must be 3 octal digits\n", argv[1]);
        return;
    }
    int rc = vfs_chmod(argv[2], (uint32_t)mode);
    if (rc != VFS_OK) {
        kprintf("chmod: '%s': %s\n", argv[2], vfs_strerror(rc));
    } else {
        kprintf("chmod: '%s' -> 0%o\n", argv[2], (unsigned)mode);
    }
}

static void cmd_chown(int argc, char **argv) {
    if (argc != 3) {
        kprintf("usage: chown <user> <path>      (root only)\n");
        return;
    }
    const struct user *u = users_lookup_by_name(argv[1]);
    if (!u) {
        kprintf("chown: unknown user '%s'\n", argv[1]);
        return;
    }
    int rc = vfs_chown(argv[2], (uint32_t)u->uid, (uint32_t)u->gid);
    if (rc != VFS_OK) {
        kprintf("chown: '%s': %s\n", argv[2], vfs_strerror(rc));
    } else {
        kprintf("chown: '%s' -> %s (uid=%d gid=%d)\n",
                argv[2], u->name, u->uid, u->gid);
    }
}

/* Debug helper: pretend to be a different user for the duration of a
 * single command. Lets the operator demonstrate access control without
 * leaving the kernel shell. The shell itself runs as pid 0 / uid 0
 * (root), so its commands always succeed otherwise. */
static void cmd_su(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: su <user>                (changes the SHELL's uid/gid)\n");
        return;
    }
    const struct user *u = users_lookup_by_name(argv[1]);
    if (!u) {
        kprintf("su: unknown user '%s'\n", argv[1]);
        return;
    }
    struct proc *p = current_proc();
    if (!p) { kprintf("su: no current proc\n"); return; }
    /* Debug aid -- the kernel shell always runs on pid 0 (boot
     * thread), so we let it freely flip identity for demo purposes. */
    p->uid = u->uid;
    p->gid = u->gid;
    kprintf("su: now running as %s (uid=%d gid=%d). "
            "Type `su root` to restore.\n",
            u->name, u->uid, u->gid);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: cat <path>\n");
        return;
    }
    const char *path = argv[1];

    /* Stream in 256-byte chunks instead of slurping the whole file --
     * keeps stack usage tiny and works for arbitrarily-large text
     * files without a heap allocation. */
    struct vfs_file f;
    int rc = vfs_open(path, &f);
    if (rc != VFS_OK) {
        kprintf("cat: '%s': %s\n", path, vfs_strerror(rc));
        return;
    }
    char buf[256];
    for (;;) {
        long n = vfs_read(&f, buf, sizeof(buf));
        if (n < 0) {
            kprintf("\ncat: read error: %s\n", vfs_strerror((int)n));
            break;
        }
        if (n == 0) break;
        for (long i = 0; i < n; i++) kputc(buf[i]);
    }
    vfs_close(&f);
    /* Many text files lack a trailing newline -- add one so the next
     * shell prompt doesn't visually collide with the last line. */
    kputc('\n');
}

/* ---- writable-FS builtins (milestone 6) ---- */

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: touch <path>      (creates empty file; '/data' is writable)\n");
        return;
    }
    int rc = vfs_create(argv[1]);
    if (rc == VFS_ERR_EXIST) {
        /* `touch` of an existing file is a no-op success in classic
         * shells; surface that politely. */
        kprintf("touch: '%s' already exists\n", argv[1]);
        return;
    }
    if (rc != VFS_OK) {
        kprintf("touch: '%s': %s\n", argv[1], vfs_strerror(rc));
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: mkdir <path>\n");
        return;
    }
    int rc = vfs_mkdir(argv[1]);
    if (rc != VFS_OK) {
        kprintf("mkdir: '%s': %s\n", argv[1], vfs_strerror(rc));
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: rm <path>      (file or empty directory)\n");
        return;
    }
    int rc = vfs_unlink(argv[1]);
    if (rc != VFS_OK) {
        kprintf("rm: '%s': %s\n", argv[1], vfs_strerror(rc));
    }
}

/* `write <path> <text...>` -- joins all remaining args with single
 * spaces and writes them to `path`, creating/truncating as needed.
 * No newline is appended -- so `cat` will print the bytes verbatim. */
static void cmd_write(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: write <path> <text>\n");
        kprintf("       e.g. write /data/notes/todo buy groceries\n");
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
            return;
        }
        if (i > 2) body[pos++] = ' ';
        memcpy(body + pos, argv[i], alen);
        pos += alen;
    }
    body[pos] = 0;

    int rc = vfs_write_all(argv[1], body, pos);
    if (rc != VFS_OK) {
        kprintf("write: '%s': %s\n", argv[1], vfs_strerror(rc));
        return;
    }
    kprintf("write: '%s' <- %lu byte%s\n", argv[1],
            (unsigned long)pos, pos == 1 ? "" : "s");
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

static const struct cmd cmds[] = {
    { "help",   "list available commands",      cmd_help   },
    { "clear",  "clear the screen",             cmd_clear  },
    { "echo",   "print arguments",              cmd_echo   },
    { "env",      "env [K=V ...]: print or set environment vars (M25C)", cmd_env      },
    { "setenv",   "setenv KEY VALUE: set an environment var (M25C)",     cmd_setenv   },
    { "unsetenv", "unsetenv KEY: remove an environment var (M25C)",      cmd_unsetenv },
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
    { "rm",     "rm <path>: delete file/empty dir (RW mounts)",cmd_rm    },
    { "write",  "write <path> <text>: write/overwrite file",   cmd_write },
    { "mounts", "list mounted filesystems",     cmd_mounts },
    { "run",    "run <path> [args]: spawn ring-3 process (fg)", cmd_run},
    { "jobs",   "list active background jobs",   cmd_jobs   },
    { "fg",     "fg <job_id>: bring bg job to foreground",  cmd_fg },
    { "ps",     "list processes with cpu/syscalls/pages", cmd_ps },
    { "top",    "top [-n iters] [-d ms]: live process stats",  cmd_top  },
    { "time",   "time <cmd> [args]: measure wall + cpu + syscalls", cmd_time },
    { "perf",   "perf [reset|on|off]: dump profiling counters", cmd_perf },
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
    return c == ' ' || c == '\t';
}

static int tokenize(char *s, char **argv, int argv_max) {
    int argc = 0;
    while (*s) {
        while (is_space(*s)) s++;
        if (!*s) break;
        if (argc >= argv_max) break;       /* drop overflow tokens */
        argv[argc++] = s;
        while (*s && !is_space(*s)) s++;
        if (*s) *s++ = '\0';
    }
    return argc;
}

static void prompt(void) {
    console_set_color(0x0066FF66);   /* greenish */
    kprintf("tobyOS> ");
    console_set_color(0x00CCCCCC);
}

/* ---- pipeline support (milestone 7) ---- */

#define PIPE_BIN_PREFIX "/bin/"

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

/* Execute a 2-stage pipeline: stage_a's stdout connected to stage_b's
 * stdin via a fresh kernel pipe.
 *
 * Both stages are spawned as user-mode ELFs (auto-prefixed with /bin/
 * if they don't already contain a slash). Builtins are NOT supported on
 * the LHS or RHS of a pipe in milestone 7 -- you write a pipeline as
 *   echo hello | cat
 * and we execute /bin/echo and /bin/cat as real ring-3 processes. */
static void run_pipeline(int argc_a, char **argv_a,
                         int argc_b, char **argv_b) {
    if (argc_a == 0 || argc_b == 0) {
        kprintf("pipeline: empty stage\n");
        return;
    }

    char path_a_buf[64], path_b_buf[64];
    const char *path_a = resolve_program(argv_a[0], path_a_buf, sizeof(path_a_buf));
    const char *path_b = resolve_program(argv_b[0], path_b_buf, sizeof(path_b_buf));

    /* Create the connecting pipe -- both ends are owned by the shell
     * for now; we bump their counts via file_clone() when handing them
     * to the spawned procs. */
    struct file *r = 0, *w = 0;
    if (pipe_create(&r, &w) != 0) {
        kprintf("pipeline: pipe_create failed (OOM?)\n");
        return;
    }

    /* Stage A: stdin = console default, stdout = pipe write end. */
    struct proc_spec spec_a = {
        .path = path_a, .name = argv_a[0],
        .fd0  = 0,   /* default: console */
        .fd1  = w,   /* clone bumps writers */
        .fd2  = 0,
        .argc = argc_a, .argv = argv_a,
        .envc = g_envc, .envp = g_env,    /* M25C */
    };
    int pid_a = proc_spawn(&spec_a);
    if (pid_a < 0) {
        kprintf("pipeline: failed to spawn '%s'\n", path_a);
        file_close(r); file_close(w);
        return;
    }

    /* Stage B: stdin = pipe read end, stdout = console default. */
    struct proc_spec spec_b = {
        .path = path_b, .name = argv_b[0],
        .fd0  = r,
        .fd1  = 0,
        .fd2  = 0,
        .argc = argc_b, .argv = argv_b,
        .envc = g_envc, .envp = g_env,    /* M25C */
    };
    int pid_b = proc_spawn(&spec_b);
    if (pid_b < 0) {
        kprintf("pipeline: failed to spawn '%s'\n", path_b);
        file_close(r); file_close(w);
        /* Stage A is already running; the easiest correct cleanup is
         * to close the shell's pipe ends (so A sees EPIPE on its next
         * write and exits) and then wait for it. */
        (void)proc_wait(pid_a);
        return;
    }

    /* Drop the shell's own refs to the pipe so the child processes are
     * the only ones holding it. This is critical: if we kept r alive,
     * /bin/cat would never see EOF (writers >= 1 -- us). */
    file_close(r);
    file_close(w);

    /* Wait for both. Order doesn't matter; the writer (pid_a) will
     * usually finish first, and its proc_exit closes its inherited
     * write end -- waking pid_b's blocked read with EOF. */
    int rc_a = proc_wait(pid_a);
    int rc_b = proc_wait(pid_b);
    kprintf("pipeline: '%s' (pid=%d, rc=%d) | '%s' (pid=%d, rc=%d)\n",
            argv_a[0], pid_a, rc_a, argv_b[0], pid_b, rc_b);
}

/* Returns true if any token in argv[0..argc-1] is the literal "|". */
static int find_pipe_token(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '|' && argv[i][1] == '\0') return i;
    }
    return -1;
}

/* Shared spawn-a-user-program helper used by both `cmd_run` and the
 * implicit-ELF dispatch in execute_line. Resolves a name via /bin/
 * if it doesn't already contain a slash, spawns it with the given
 * argv, and either:
 *   - fg: registers as foreground (so Ctrl+C lands on it),
 *         proc_wait()s, clears foreground, prints the exit code.
 *   - bg: registers in the job table and returns immediately. The
 *         child runs concurrently with the shell prompt; reaping
 *         happens lazily in shell_poll() via jobs_reap_finished. */
static void shell_spawn_program(const char *path_arg, int argc, char **argv,
                                bool background) {
    shell_spawn_program_profile(path_arg, argc, argv, background,
                                /*profile=*/0);
}

/* Same as shell_spawn_program, but narrows the child's capabilities /
 * sandbox root via `profile`. NULL or empty profile = inherit from the
 * shell (which is pid 0's child and so usually has admin -- fine for
 * an interactive shell). Unknown profile name is an error, reported
 * by cap_profile_apply() inside proc_spawn. */
static void shell_spawn_program_profile(const char *path_arg, int argc,
                                        char **argv, bool background,
                                        const char *profile) {
    char path_buf[64];
    const char *path = resolve_program(path_arg, path_buf, sizeof(path_buf));

    /* Pass argv straight through: argv[0] is the user-typed name (no
     * /bin/ prefix), which is what /bin/echo etc. expect to see. */
    struct proc_spec spec = {
        .path = path,
        .name = argv[0],
        .fd0  = 0, .fd1 = 0, .fd2 = 0,    /* console defaults */
        .argc = argc,
        .argv = argv,
        /* Milestone 25C: pipe the shell environment to every child so
         * libtoby getenv()/environ pick up PATH, HOME, etc. immediately. */
        .envc = g_envc,
        .envp = g_env,
        .sandbox_profile = profile,
    };
    int pid = proc_spawn(&spec);
    if (pid < 0) {
        kprintf("spawn: failed to launch '%s'\n", path);
        return;
    }

    if (background) {
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
        return;
    }

    /* Foreground: own Ctrl+C until the child exits. */
    signal_set_foreground(pid);
    int rc = proc_wait(pid);
    signal_set_foreground(0);
    kprintf("'%s' (pid=%d) returned %d (0x%x)\n",
            argv[0], pid, rc, (unsigned)rc);
}

static void execute_line(void) {
    char *argv[ARG_MAX];
    int argc = tokenize(line, argv, ARG_MAX);
    if (argc == 0) return;             /* blank line */

    /* Trailing `&` strips off and turns this into a background launch.
     * Only meaningful for user-program dispatch -- builtins reject `&`
     * (they run synchronously inside the shell), and pipelines + `&`
     * are intentionally out of scope for milestone 8. */
    bool background = false;
    if (argv[argc - 1][0] == '&' && argv[argc - 1][1] == '\0') {
        argc--;
        background = true;
        if (argc == 0) {
            kprintf("'&' must follow a command\n");
            return;
        }
    }

    /* Pipeline path: "<cmd_a args...> | <cmd_b args...>". */
    int pipe_at = find_pipe_token(argc, argv);
    if (pipe_at >= 0) {
        if (background) {
            kprintf("pipeline: '&' on pipelines not supported in this milestone\n");
            return;
        }
        if (pipe_at == 0 || pipe_at == argc - 1) {
            kprintf("pipeline: '|' needs a command on both sides\n");
            return;
        }
        if (find_pipe_token(argc - pipe_at - 1, argv + pipe_at + 1) >= 0) {
            kprintf("pipeline: only one '|' supported (got >=2)\n");
            return;
        }
        argv[pipe_at] = 0;
        run_pipeline(pipe_at,            argv,
                     argc - pipe_at - 1, argv + pipe_at + 1);
        return;
    }

    /* Builtin path: synchronous, runs inside the shell. */
    for (const struct cmd *c = cmds; c->name; c++) {
        if (strcmp(argv[0], c->name) == 0) {
            if (background) {
                kprintf("'%s': builtins can't be backgrounded with '&'\n",
                        argv[0]);
                return;
            }
            c->fn(argc, argv);
            return;
        }
    }

    /* Implicit-ELF dispatch: not a builtin -> assume the user typed a
     * program name (e.g. `long_task &`, `echo hi`). Resolve via /bin/
     * if no slash, spawn either fg or bg per the `&` flag. */
    shell_spawn_program(argv[0], argc, argv, background);
}

void shell_init(void) {
    line_len = 0;
    line[0]  = '\0';
    /* Milestone 25C: stamp boot-time PATH/HOME/USER/PWD/SHELL so any
     * implicit-ELF dispatch immediately has a search path and so any
     * libtoby-linked child sees a meaningful environ at exec. */
    env_init_defaults();
    kprintf("type 'help' to list commands.\n");
    prompt();
}

/* Milestone 25C: drive `execute_line` over an arbitrary string. Used
 * from the boot harness to exercise the shell's PATH/env/spawn path
 * without input. Truncates lines that don't fit the editor buffer
 * (with a kprintf so the caller notices) instead of stomping memory. */
void shell_run_test_line(const char *in) {
    if (!in) return;
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
    execute_line();
    line_len = 0;
    line[0]  = '\0';
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
            execute_line();
            line_len = 0;
            line[0]  = '\0';
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
