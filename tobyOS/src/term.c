/* term.c -- terminal session implementation.
 *
 * See term.h for the high-level contract. Implementation notes:
 *
 *   - Each session owns a cmd_line buffer (line being edited), an
 *     output ring (bytes for the GUI to render), and a cwd string.
 *
 *   - The output ring is a single-producer / single-consumer ring where
 *     both ends live in kernel memory. The producer is every code path
 *     that wants to push characters to the screen (echo, prompt,
 *     builtin output). The consumer is SYS_TERM_READ (the GUI app).
 *     No locking: syscalls are serialised per-process, and the only
 *     cross-process touch is SYS_TERM_READ on the very same session
 *     struct the producer wrote into -- user code never reaches in.
 *
 *   - Builtins use vfs_* directly. For `run`, we push onto the desktop
 *     launch queue (same path the Apps menu uses) so the actual
 *     proc_create_from_elf call runs on pid 0 rather than from our
 *     syscall context.
 *
 *   - Output helper term_printf uses a small local buffer so the
 *     builtins can format like kprintf without pulling in kvprintf's
 *     char-by-char callbacks.
 */

#include <tobyos/term.h>
#include <tobyos/vfs.h>
#include <tobyos/gui.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/pkg.h>
#include <tobyos/proc.h>
#include <tobyos/users.h>
#include <stdarg.h>

/* ---- session struct + pool --------------------------------------- */

struct term_session {
    bool   in_use;
    char   cmd[TERM_CMD_MAX];
    size_t cmd_len;

    char   out[TERM_OUT_RING];
    size_t out_head;        /* write index (producer) */
    size_t out_tail;        /* read  index (consumer) */

    char   cwd[TERM_CWD_MAX];
};

static struct term_session g_pool[TERM_MAX_SESSIONS];
static bool                g_ready;

/* ---- ring helpers (single-producer / single-consumer) ------------ */

/* Push one byte. If the ring is full, drops the OLDEST byte -- keeps
 * the stream "live" rather than stalling on a slow reader. Terminals
 * are usually many-KB-behind when this hits, which is acceptable. */
static void ring_push(struct term_session *s, char c) {
    size_t next = (s->out_head + 1u) % TERM_OUT_RING;
    if (next == s->out_tail) {
        /* full -- advance tail (drop oldest) */
        s->out_tail = (s->out_tail + 1u) % TERM_OUT_RING;
    }
    s->out[s->out_head] = c;
    s->out_head = next;
}

static void ring_push_str(struct term_session *s, const char *str) {
    if (!str) return;
    while (*str) ring_push(s, *str++);
}

/* Classic "BS SP BS" erases the last glyph on a character cell terminal
 * without needing escape sequences. The GUI app implements all three
 * operations on the text grid. */
static void ring_push_backspace(struct term_session *s) {
    ring_push(s, 0x08);
    ring_push(s, ' ');
    ring_push(s, 0x08);
}

/* Tiny snprintf-like helper. Supports %s, %d, %u, %lu, %x. Plenty for
 * our builtins. Truncates silently if the format expands past cap. */
static void term_vprintf(struct term_session *s, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { ring_push(s, *p); continue; }
        p++;
        /* No flags/widths -- we only need %-formatters that are raw. */
        if (*p == 's') {
            const char *q = va_arg(ap, const char *);
            if (!q) q = "(null)";
            while (*q) ring_push(s, *q++);
        } else if (*p == 'd' || *p == 'i') {
            int v = va_arg(ap, int);
            char buf[16]; int n = 0;
            unsigned u;
            bool neg = v < 0;
            u = neg ? (unsigned)(-v) : (unsigned)v;
            if (u == 0) buf[n++] = '0';
            while (u) { buf[n++] = (char)('0' + u % 10u); u /= 10u; }
            if (neg) buf[n++] = '-';
            while (n--) ring_push(s, buf[n]);
        } else if (*p == 'u') {
            unsigned v = va_arg(ap, unsigned);
            char buf[16]; int n = 0;
            if (v == 0) buf[n++] = '0';
            while (v) { buf[n++] = (char)('0' + v % 10u); v /= 10u; }
            while (n--) ring_push(s, buf[n]);
        } else if (*p == 'l' && p[1] == 'u') {
            p++;
            unsigned long v = va_arg(ap, unsigned long);
            char buf[24]; int n = 0;
            if (v == 0) buf[n++] = '0';
            while (v) { buf[n++] = (char)('0' + v % 10ul); v /= 10ul; }
            while (n--) ring_push(s, buf[n]);
        } else if (*p == 'x') {
            unsigned v = va_arg(ap, unsigned);
            char buf[16]; int n = 0;
            if (v == 0) buf[n++] = '0';
            while (v) {
                unsigned d = v & 0xFu;
                buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                v >>= 4;
            }
            while (n--) ring_push(s, buf[n]);
        } else if (*p == '%') {
            ring_push(s, '%');
        } else {
            ring_push(s, '%');
            if (*p) ring_push(s, *p);
        }
    }
}

__attribute__((format(printf, 2, 3)))
static void term_printf(struct term_session *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); term_vprintf(s, fmt, ap); va_end(ap);
}

/* ---- cwd + path helpers ------------------------------------------ */

static size_t str_len(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

/* Build an absolute path out of either an absolute input (just copied)
 * or a relative one (joined to cwd). Writes into `out` (cap bytes).
 * Returns true on success, false if the result would overflow. */
static bool resolve_path(const struct term_session *s, const char *in,
                         char *out, size_t cap) {
    size_t oi = 0;
    if (in[0] == '/') {
        while (in[oi] && oi + 1 < cap) { out[oi] = in[oi]; oi++; }
        out[oi] = '\0';
        return in[oi] == '\0';
    }
    /* Relative: cwd + '/' + in (skip cwd's trailing '/' if any). */
    size_t ci = 0;
    while (s->cwd[ci] && oi + 1 < cap) { out[oi++] = s->cwd[ci++]; }
    if (oi == 0 || out[oi - 1] != '/') {
        if (oi + 1 >= cap) return false;
        out[oi++] = '/';
    }
    for (size_t i = 0; in[i]; i++) {
        if (oi + 1 >= cap) return false;
        out[oi++] = in[i];
    }
    out[oi] = '\0';
    return true;
}

/* Replace cwd with the longest directory prefix of `cwd` -- i.e. go to
 * parent. Root stays root. */
static void cwd_pop(struct term_session *s) {
    size_t n = str_len(s->cwd);
    if (n <= 1) return;
    /* Drop trailing slash. */
    if (s->cwd[n - 1] == '/') { s->cwd[--n] = '\0'; }
    while (n > 0 && s->cwd[n - 1] != '/') { s->cwd[--n] = '\0'; }
    /* If we stripped back to "" leave "/". */
    if (n == 0) { s->cwd[0] = '/'; s->cwd[1] = '\0'; return; }
    /* If we stripped "/foo/" down to "/" drop the extra / above. */
    if (n > 1 && s->cwd[n - 1] == '/') s->cwd[n - 1] = '\0';
}

/* ---- tokenisation ------------------------------------------------- */

#define TERM_ARG_MAX 8

static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= TERM_ARG_MAX) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static bool streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* ---- builtin commands -------------------------------------------- */

static void builtin_help(struct term_session *s) {
    term_printf(s,
        "builtins: help, clear, about, echo, pwd, cd <p>, ls [p],\r\n"
        "          cat <p>, run <p> [arg], whoami, su <user>,\r\n"
        "          pkg <subcmd> ...\r\n"
        "apps are launched in the background -- their stdout goes to\r\n"
        "the kernel console/serial, not this window.\r\n"
        "pkg subcmds: install|remove|list|info|repo|update|upgrade|rollback\r\n"
        "  (pkg install/remove/upgrade/rollback auto-elevate -- you do\r\n"
        "   NOT need to `su root` first for those.)\r\n");
}

static void builtin_about(struct term_session *s) {
    term_printf(s,
        "tobyOS milestone 13 -- GUI terminal + file manager.\r\n"
        "this terminal is a mini-shell; use the main text shell for\r\n"
        "the full command set (proc/net/fs/disk).\r\n");
}

static void builtin_echo(struct term_session *s, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        term_printf(s, "%s%s", argv[i], i + 1 < argc ? " " : "");
    }
    ring_push(s, '\r'); ring_push(s, '\n');
}

static void builtin_pwd(struct term_session *s) {
    term_printf(s, "%s\r\n", s->cwd);
}

static void builtin_cd(struct term_session *s, int argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : "/";
    /* Special cases: "..", "/", relative, absolute. */
    if (streq(target, "..")) { cwd_pop(s); return; }
    if (streq(target, "."))  { return; }

    char abs[TERM_CWD_MAX];
    if (!resolve_path(s, target, abs, sizeof(abs))) {
        term_printf(s, "cd: path too long\r\n");
        return;
    }
    struct vfs_stat st;
    int rc = vfs_stat(abs, &st);
    if (rc != VFS_OK) {
        term_printf(s, "cd: '%s': %s\r\n", abs, vfs_strerror(rc));
        return;
    }
    if (st.type != VFS_TYPE_DIR) {
        term_printf(s, "cd: '%s': not a directory\r\n", abs);
        return;
    }
    /* Copy into session cwd (we already know it fits -- resolve_path
     * already rejected cap overflow). */
    size_t i = 0;
    for (; abs[i] && i + 1 < TERM_CWD_MAX; i++) s->cwd[i] = abs[i];
    s->cwd[i] = '\0';
}

static void builtin_ls(struct term_session *s, int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : s->cwd;
    char abs[TERM_CWD_MAX];
    if (path[0] != '/') {
        if (!resolve_path(s, path, abs, sizeof(abs))) {
            term_printf(s, "ls: path too long\r\n");
            return;
        }
        path = abs;
    }
    struct vfs_dir d;
    int rc = vfs_opendir(path, &d);
    if (rc != VFS_OK) {
        term_printf(s, "ls: '%s': %s\r\n", path, vfs_strerror(rc));
        return;
    }
    struct vfs_dirent ent;
    int shown = 0;
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        if (ent.type == VFS_TYPE_DIR) {
            term_printf(s, "  %s/\r\n", ent.name);
        } else {
            term_printf(s, "  %s  (%lu B)\r\n", ent.name,
                        (unsigned long)ent.size);
        }
        shown++;
    }
    vfs_closedir(&d);
    if (shown == 0) term_printf(s, "  (empty)\r\n");
}

static void builtin_cat(struct term_session *s, int argc, char **argv) {
    if (argc < 2) { term_printf(s, "usage: cat <path>\r\n"); return; }
    char abs[TERM_CWD_MAX];
    const char *path = argv[1];
    if (path[0] != '/') {
        if (!resolve_path(s, path, abs, sizeof(abs))) {
            term_printf(s, "cat: path too long\r\n"); return;
        }
        path = abs;
    }
    struct vfs_file f;
    int rc = vfs_open(path, &f);
    if (rc != VFS_OK) {
        term_printf(s, "cat: '%s': %s\r\n", path, vfs_strerror(rc));
        return;
    }
    char buf[128];
    for (;;) {
        long n = vfs_read(&f, buf, sizeof(buf));
        if (n < 0) {
            term_printf(s, "\r\ncat: read error: %s\r\n",
                        vfs_strerror((int)n));
            break;
        }
        if (n == 0) break;
        /* Bytes may include embedded newlines; translate LF to CRLF so
         * the GUI row-scroller gets a carriage-return paired with each
         * line feed. */
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') ring_push(s, '\r');
            ring_push(s, c);
        }
    }
    vfs_close(&f);
    /* Ensure the next prompt starts on a fresh line. */
    ring_push(s, '\r'); ring_push(s, '\n');
}

static void builtin_run(struct term_session *s, int argc, char **argv) {
    if (argc < 2) {
        term_printf(s, "usage: run <path> [arg]\r\n"); return;
    }
    const char *path = argv[1];
    const char *arg  = (argc >= 3) ? argv[2] : 0;
    if (gui_launch_enqueue_arg(path, arg) != 0) {
        term_printf(s, "run: queue full -- try again later\r\n");
        return;
    }
    term_printf(s, "run: queued '%s'%s%s (launch runs on pid 0 "
                "-- stdout goes to serial/console)\r\n",
                path, arg ? " arg=" : "", arg ? arg : "");
}

static void builtin_clear(struct term_session *s) {
    /* Form-feed byte -- the terminal emulator interprets this as
     * "clear the text grid and home the cursor". */
    ring_push(s, 0x0C);
}

/* ---- identity: whoami / su --------------------------------------- *
 *
 * The GUI terminal runs inside a logged-in user's session, so its
 * uid/gid are inherited from session_login (e.g. uid=1000 for `toby`).
 * Most things in this OS are open to non-root users -- but a handful
 * of admin-shaped commands (notably `pkg install` until M32, and
 * anything that touches /data outside the user's owned tree) need
 * root.
 *
 * `whoami` lets the user check their current identity. `su` flips
 * the GUI terminal proc's uid/gid to a named user from the on-disk
 * users database. Like the kernel-shell `cmd_su` it is unauthenticated
 * (no password column exists in this OS); the change is scoped to
 * THIS terminal process and does not leak into other apps. Restoring
 * by `su <original-name>` is up to the user.
 */
static void builtin_whoami(struct term_session *s) {
    struct proc *p = current_proc();
    if (!p) {
        term_printf(s, "whoami: no current proc\r\n");
        return;
    }
    term_printf(s, "uid=%d gid=%d\r\n", p->uid, p->gid);
}

static void builtin_su(struct term_session *s, int argc, char **argv) {
    if (argc < 2) {
        term_printf(s,
            "usage: su <user>           (changes THIS terminal's uid/gid)\r\n"
            "       su root             (elevate so `pkg install` works)\r\n");
        return;
    }
    const struct user *u = users_lookup_by_name(argv[1]);
    if (!u) {
        term_printf(s, "su: unknown user '%s'\r\n", argv[1]);
        return;
    }
    struct proc *p = current_proc();
    if (!p) {
        term_printf(s, "su: no current proc\r\n");
        return;
    }
    p->uid = u->uid;
    p->gid = u->gid;
    term_printf(s, "su: now running as %s (uid=%d gid=%d)\r\n",
                u->name, u->uid, u->gid);
}

/* ---- pkg dispatch -------------------------------------------------- *
 *
 * The package-manager builtins live in pkg.c and emit their output via
 * kprintf (serial + framebuffer console). To surface them in the GUI
 * terminal we install a printk "mirror sink" for the duration of each
 * pkg call -- every byte kprintf writes is also copied into our output
 * ring. We translate bare LF into CRLF because the ring consumer (the
 * terminal grid in user_gui_term) only advances to a new row on '\n'
 * but expects col=0 from a preceding '\r'. */

static void pkg_sink_cb(void *ctx, char c) {
    struct term_session *s = (struct term_session *)ctx;
    if (c == '\n') ring_push(s, '\r');
    ring_push(s, c);
}

static void pkg_usage_term(struct term_session *s) {
    term_printf(s,
        "usage: pkg <subcmd> [args]\r\n"
        "  pkg install <name-or-path>   install from /data/repo, /repo, or a .tpkg\r\n"
        "  pkg remove  <name>           uninstall by name\r\n"
        "  pkg list                     list installed packages\r\n"
        "  pkg info    <name>           show install record\r\n"
        "  pkg repo                     list available .tpkg files\r\n"
        "  pkg update                   show packages with newer versions in repo\r\n"
        "  pkg upgrade [name]           upgrade one (or all) packages\r\n"
        "  pkg rollback <name>          restore from .bak after an upgrade\r\n");
}

static void builtin_pkg(struct term_session *s, int argc, char **argv) {
    if (argc < 2) { pkg_usage_term(s); return; }
    const char *sub = argv[1];

    /* Start capturing kprintf output before calling into pkg.c. */
    printk_set_sink(pkg_sink_cb, s);

    if (streq(sub, "install")) {
        if (argc < 3) {
            printk_set_sink(0, 0);
            term_printf(s, "usage: pkg install <name-or-path>\r\n");
            return;
        }
        const char *arg = argv[2];
        bool is_path = false;
        for (const char *c = arg; *c; c++) if (*c == '/') { is_path = true; break; }
        int rc = is_path ? pkg_install_path(arg) : pkg_install_name(arg);
        if (rc != 0) kprintf("pkg install: failed\n");
    } else if (streq(sub, "remove")) {
        if (argc < 3) {
            printk_set_sink(0, 0);
            term_printf(s, "usage: pkg remove <name>\r\n");
            return;
        }
        if (pkg_remove(argv[2]) != 0) kprintf("pkg remove: failed\n");
    } else if (streq(sub, "list")) {
        pkg_list();
    } else if (streq(sub, "info")) {
        if (argc < 3) {
            printk_set_sink(0, 0);
            term_printf(s, "usage: pkg info <name>\r\n");
            return;
        }
        (void)pkg_info(argv[2]);
    } else if (streq(sub, "repo")) {
        pkg_repo_dump();
    } else if (streq(sub, "update")) {
        pkg_update();
    } else if (streq(sub, "upgrade")) {
        if (argc < 3) (void)pkg_upgrade_all();
        else          (void)pkg_upgrade_one(argv[2]);
    } else if (streq(sub, "rollback")) {
        if (argc < 3) {
            printk_set_sink(0, 0);
            term_printf(s, "usage: pkg rollback <name>\r\n");
            return;
        }
        (void)pkg_rollback(argv[2]);
    } else {
        printk_set_sink(0, 0);
        term_printf(s, "pkg: unknown subcommand '%s'\r\n", sub);
        pkg_usage_term(s);
        return;
    }

    printk_set_sink(0, 0);
}

/* ---- prompt + dispatch ------------------------------------------- */

static void print_prompt(struct term_session *s) {
    term_printf(s, "tobyOS:%s$ ", s->cwd);
}

static void execute_line(struct term_session *s) {
    /* Tokenize in-place on cmd[]. */
    char *argv[TERM_ARG_MAX];
    int argc = tokenize(s->cmd, argv);
    if (argc == 0) return;
    const char *cmd = argv[0];
    if      (streq(cmd, "help"))   builtin_help(s);
    else if (streq(cmd, "clear"))  builtin_clear(s);
    else if (streq(cmd, "about"))  builtin_about(s);
    else if (streq(cmd, "echo"))   builtin_echo(s, argc, argv);
    else if (streq(cmd, "pwd"))    builtin_pwd(s);
    else if (streq(cmd, "cd"))     builtin_cd(s, argc, argv);
    else if (streq(cmd, "ls"))     builtin_ls(s, argc, argv);
    else if (streq(cmd, "cat"))    builtin_cat(s, argc, argv);
    else if (streq(cmd, "run"))    builtin_run(s, argc, argv);
    else if (streq(cmd, "whoami")) builtin_whoami(s);
    else if (streq(cmd, "su"))     builtin_su(s, argc, argv);
    else if (streq(cmd, "pkg"))    builtin_pkg(s, argc, argv);
    else {
        term_printf(s, "unknown: '%s' (try 'help')\r\n", cmd);
    }
}

/* ---- public API -------------------------------------------------- */

void term_init(void) {
    if (g_ready) return;
    memset(g_pool, 0, sizeof(g_pool));
    g_ready = true;
    kprintf("[term] session pool ready (%d sessions, %d-byte rings)\n",
            TERM_MAX_SESSIONS, TERM_OUT_RING);
}

struct term_session *term_session_create(void) {
    if (!g_ready) term_init();
    for (int i = 0; i < TERM_MAX_SESSIONS; i++) {
        if (!g_pool[i].in_use) {
            struct term_session *s = &g_pool[i];
            memset(s, 0, sizeof(*s));
            s->in_use = true;
            s->cwd[0] = '/'; s->cwd[1] = '\0';
            /* Welcome + initial prompt -- shown the moment the app
             * polls the ring. */
            ring_push_str(s, "tobyOS terminal -- type 'help' for builtins.\r\n");
            print_prompt(s);
            return s;
        }
    }
    return 0;
}

void term_session_close(struct term_session *s) {
    if (!s || !s->in_use) return;
    s->in_use = false;
    /* Leave the ring untouched -- the slot is already marked free. */
}

long term_session_write_input(struct term_session *s,
                              const char *buf, size_t n) {
    if (!s || !s->in_use) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r' || c == '\n') {
            ring_push(s, '\r'); ring_push(s, '\n');
            s->cmd[s->cmd_len] = '\0';
            execute_line(s);
            s->cmd_len = 0;
            print_prompt(s);
        } else if (c == 0x08 || c == 0x7F) {
            if (s->cmd_len > 0) {
                s->cmd_len--;
                ring_push_backspace(s);
            }
        } else if (c >= 0x20 && c < 0x7F) {
            if (s->cmd_len + 1 < TERM_CMD_MAX) {
                s->cmd[s->cmd_len++] = (char)c;
                ring_push(s, (char)c);
            }
        }
        /* Everything else silently dropped (control keys we don't
         * support -- arrows, F-keys, etc). */
    }
    return (long)n;
}

long term_session_read_output(struct term_session *s, char *buf, size_t cap) {
    if (!s || !s->in_use) return -1;
    size_t copied = 0;
    while (copied < cap && s->out_tail != s->out_head) {
        buf[copied++] = s->out[s->out_tail];
        s->out_tail = (s->out_tail + 1u) % TERM_OUT_RING;
    }
    return (long)copied;
}
