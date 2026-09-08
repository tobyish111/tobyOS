/* pty.c -- pseudoterminal pairs. See pty.h.
 *
 * A `struct pty` is one (master,slave) pair: two byte rings connected
 * back-to-back, plus the slave-side termios + cooked line discipline.
 *
 *   master write --> `in`  ring --> slave read  (line discipline applies here)
 *   slave  write --> `out` ring --> master read (raw, ONLCR mapping applies)
 *
 * Echo (when the slave's ECHO is set) happens at master-write time, the same
 * instant a real n_tty line discipline echoes received input -- so a terminal
 * holding the master sees keystrokes immediately, before the program on the
 * slave reads them.
 *
 * Blocking uses the same cooperative wait-queue protocol as pipe.c (one BKL,
 * sched_yield() is the only preemption point; signals splice a parked proc
 * out of the queue and surface as EINTR_RET).
 */

#include <tobyos/pty.h>
#include <tobyos/file.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/heap.h>
#include <tobyos/uaccess.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/cpu.h>

/* -DSSH_PTY_DIAG traces pty open/clone/close with refcounts. Useful for
 * lifecycle and refcount questions -- but DO NOT reach for it on a timing
 * bug: it masks them. The SMP deadlock in the SSH-on-tsh work reproduced
 * every 1-3 sessions on a plain build and went 10/10 GREEN with this
 * enabled (so did -DSMP_DIAG). sched.c carries the same warning about
 * per-event BKL logging. */
#define PTY_MAX  8
#define PTY_BUF  4096

struct pty {
    bool   used;
    int    index;
    int    master_refs;
    int    slave_refs;

    struct ktermios tio;       /* slave-side termios */
    struct kwinsize win;       /* slave window size */
    int    pgrp;               /* foreground process group (TIOCSPGRP) */

    /* master -> slave: bytes the program on the slave reads as its input */
    uint8_t in[PTY_BUF];
    size_t  in_head, in_tail;
    /* slave -> master: bytes the terminal holding the master reads */
    uint8_t out[PTY_BUF];
    size_t  out_head, out_tail;

    /* completed canonical line awaiting delivery across slave reads */
    uint8_t line[PTY_BUF];
    size_t  line_len, line_pos;

    struct proc *wq_master_read;   /* parked waiting for slave output */
    struct proc *wq_slave_read;    /* parked waiting for master input */

    /* The master says no more input will ever arrive, while the pty stays
     * open for OUTPUT. That is what an SSH client means by
     * SSH_MSG_CHANNEL_EOF, and there was no way to express it: the only
     * end-of-input this pty had was the master closing, which also tears
     * down the direction the command still needs to answer on. Without it
     * `ssh host 'cat > file' < local` -- how edremote uploads a script --
     * left cat blocked on a stdin that never ended, forever. */
    bool   in_eof;
};

static struct pty g_ptys[PTY_MAX];

/* ---- wait queue (mirrors pipe.c) -------------------------------- */
static void wq_add(struct proc **head, struct proc *p) {
    p->next_wait = *head;
    p->wait_head = head;
    *head = p;
}
static void wq_wake_all(struct proc **head) {
    struct proc *p = *head;
    *head = 0;
    while (p) {
        struct proc *next = p->next_wait;
        p->next_wait = 0;
        p->wait_head = 0;
        if (p->state == PROC_BLOCKED) { p->state = PROC_READY; sched_enqueue(p); }
        p = next;
    }
}

/* ---- ring helpers ----------------------------------------------- */
static size_t ring_count(size_t head, size_t tail) {
    return (head - tail) & (PTY_BUF - 1);
}
static bool ring_full(size_t head, size_t tail) {
    return ring_count(head, tail) == PTY_BUF - 1;
}
/* Returns false when the ring is full and the byte was NOT stored.
 *
 * It used to return void, and every caller therefore treated a dropped
 * byte as a written one. pty_master_write went on to report the full
 * count, so the SSH server -- which pushes a channel's data straight in
 * here -- believed a 20 KB upload had landed in a 4 KiB ring. The middle
 * of the file simply vanished, with nothing anywhere saying so. */
static bool ring_push(uint8_t *buf, size_t *head, size_t tail, uint8_t c) {
    if (ring_full(*head, tail)) return false;
    buf[*head] = c;
    *head = (*head + 1) & (PTY_BUF - 1);
    return true;
}
static int ring_pop(uint8_t *buf, size_t head, size_t *tail) {
    if (head == *tail) return -1;
    uint8_t c = buf[*tail];
    *tail = (*tail + 1) & (PTY_BUF - 1);
    return c;
}

/* ---- lifecycle -------------------------------------------------- */
static void pty_defaults(struct pty *t) {
    memset(&t->tio, 0, sizeof(t->tio));
    t->tio.c_iflag = TTY_ICRNL;
    t->tio.c_oflag = TTY_OPOST | TTY_ONLCR;
    t->tio.c_cflag = 0;
    t->tio.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK;
    t->tio.c_cc[TTY_VINTR]  = 3;     /* ^C */
    t->tio.c_cc[TTY_VQUIT]  = 0x1c;  /* ^\ */
    t->tio.c_cc[TTY_VERASE] = 0x7f;  /* DEL */
    t->tio.c_cc[TTY_VKILL]  = 0x15;  /* ^U */
    t->tio.c_cc[TTY_VEOF]   = 4;     /* ^D */
    t->tio.c_cc[TTY_VMIN]   = 1;
    t->tio.c_cc[TTY_VTIME]  = 0;
    t->tio.c_cc[TTY_VSUSP]  = 0x1a;  /* ^Z */
    t->win.ws_row = 24; t->win.ws_col = 80;
    t->win.ws_xpixel = 0; t->win.ws_ypixel = 0;
    t->pgrp = 0;
}

struct file *pty_open_master(void) {
    int idx = -1;
    for (int i = 0; i < PTY_MAX; i++) if (!g_ptys[i].used) { idx = i; break; }
    if (idx < 0) return 0;
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    struct pty *t = &g_ptys[idx];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->index = idx;
    t->master_refs = 1;
    t->slave_refs = 0;
    pty_defaults(t);
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_PTY_MASTER;
    f->pty  = t;
#ifdef SSH_PTY_DIAG
    kprintf("[ptydiag] open_master idx=%d m=%d s=%d\n", idx,
            t->master_refs, t->slave_refs);
#endif
    return f;
}

struct file *pty_open_slave(int index) {
    if (index < 0 || index >= PTY_MAX) return 0;
    struct pty *t = &g_ptys[index];
    if (!t->used || t->master_refs == 0) return 0;   /* master must be open */
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_PTY_SLAVE;
    f->pty  = t;
    t->slave_refs++;
#ifdef SSH_PTY_DIAG
    kprintf("[ptydiag] open_slave  idx=%d m=%d s=%d\n", index,
            t->master_refs, t->slave_refs);
#endif
    return f;
}

static void pty_maybe_free(struct pty *t) {
    if (t->master_refs == 0 && t->slave_refs == 0) {
        t->used = false;
    }
}

void pty_close(struct file *f) {
    struct pty *t = f->pty;
    if (!t) return;
    if (f->kind == FILE_KIND_PTY_MASTER) {
        if (t->master_refs > 0) t->master_refs--;
        if (t->master_refs == 0) wq_wake_all(&t->wq_slave_read);  /* slave EOF */
    } else {
        if (t->slave_refs > 0) t->slave_refs--;
        if (t->slave_refs == 0) wq_wake_all(&t->wq_master_read);  /* master HUP */
    }
#ifdef SSH_PTY_DIAG
    kprintf("[ptydiag] close  %s idx=%d m=%d s=%d used=%d\n",
            f->kind == FILE_KIND_PTY_MASTER ? "master" : "slave ",
            t->index, t->master_refs, t->slave_refs, (int)t->used);
#endif
    pty_maybe_free(t);
}

void pty_clone_ref(struct file *f) {
    struct pty *t = f->pty;
    if (!t) return;
    if (f->kind == FILE_KIND_PTY_MASTER) t->master_refs++;
    else                                 t->slave_refs++;
#ifdef SSH_PTY_DIAG
    kprintf("[ptydiag] clone  %s idx=%d m=%d s=%d\n",
            f->kind == FILE_KIND_PTY_MASTER ? "master" : "slave ",
            t->index, t->master_refs, t->slave_refs);
#endif
}

/* ---- canonical line editing (master-write time) -----------------
 *
 * 2026-08-24. VERASE and VKILL used to be applied only when the slave
 * ASSEMBLED a line (slave_canonical_read below), which got the buffer
 * right and the SCREEN wrong: the erase byte itself was echoed back to
 * the master verbatim, so a terminal drew a 0x7F and left the rubbed-out
 * character on display. The command that ran and the command you could
 * see stopped agreeing -- visible in the EliteDesk serial log as the
 * kernel reporting paths nobody had typed.
 *
 * n_tty does this editing at input-processing time, and that is the only
 * place with the context to echo a rub-out, so it moves here. The
 * read-time arms stay as a fallback for bytes that were written while
 * ICANON was off and are read after it came back on. */

/* Visible columns one echoed input byte occupies. Control characters
 * echo as two ("^C"); tab is echoed literally and its width depends on
 * the column, so it counts as one -- the conservative choice, since
 * over-erasing would eat the prompt. */
static int echo_width(uint8_t c) {
    return (c < 0x20 && c != '\t') ? 2 : 1;
}

/* Is the current, still-unterminated line empty? Everything after the
 * last newline in the input ring is that line. Erase must never reach
 * back past a newline into a line the program has already been given. */
static bool pending_line_empty(struct pty *t) {
    if (t->in_head == t->in_tail) return true;
    size_t prev = (t->in_head - 1) & (PTY_BUF - 1);
    return t->in[prev] == '\n';
}

static void erase_one(struct pty *t) {
    if (pending_line_empty(t)) return;            /* nothing to take back */
    size_t prev = (t->in_head - 1) & (PTY_BUF - 1);
    uint8_t victim = t->in[prev];
    t->in_head = prev;                            /* un-push */
    if ((t->tio.c_lflag & TTY_ECHO) && (t->tio.c_lflag & TTY_ECHOE)) {
        int w = echo_width(victim);
        for (int i = 0; i < w; i++) {
            ring_push(t->out, &t->out_head, t->out_tail, '\b');
            ring_push(t->out, &t->out_head, t->out_tail, ' ');
            ring_push(t->out, &t->out_head, t->out_tail, '\b');
        }
    }
}

/* ---- echo (master-write time, slave ECHO set) ------------------- */
static void echo_to_master(struct pty *t, uint8_t c) {
    if (!(t->tio.c_lflag & TTY_ECHO)) return;
    if (c == '\r' || c == '\n') {
        ring_push(t->out, &t->out_head, t->out_tail, '\r');
        ring_push(t->out, &t->out_head, t->out_tail, '\n');
    } else if (c < 0x20 && c != '\t') {
        ring_push(t->out, &t->out_head, t->out_tail, '^');
        ring_push(t->out, &t->out_head, t->out_tail, (uint8_t)('@' + c));
    } else {
        ring_push(t->out, &t->out_head, t->out_tail, c);
    }
}

/* ---- master end ------------------------------------------------- */
long pty_master_write(struct file *f, const void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    const uint8_t *in = (const uint8_t *)buf;
    size_t put = 0;
    for (; put < n; put++) {
        uint8_t c = in[put];
        if ((t->tio.c_iflag & TTY_ICRNL) && c == '\r') c = '\n';
        else if ((t->tio.c_iflag & TTY_INLCR) && c == '\n') c = '\r';
        /* ISIG: ^C and ^Z are SIGNALS to the foreground group, not input
         * bytes. They echo (^C / ^Z) like every control char, and never
         * reach the input ring -- exactly n_tty. Nothing acted on these
         * before this slice; ^Z on a pty was two bytes of data. */
        if ((t->tio.c_lflag & TTY_ISIG) && t->pgrp > 0) {
            if (c == t->tio.c_cc[TTY_VINTR]) {
                echo_to_master(t, c);
                signal_send_to_pgrp(t->pgrp, SIGINT);
                continue;
            }
            if (c == t->tio.c_cc[TTY_VSUSP]) {
                echo_to_master(t, c);
                signal_send_to_pgrp(t->pgrp, SIGTSTP);
                continue;
            }
        }
        /* ICANON editing, applied HERE rather than at line-assembly time
         * so the rub-out can be echoed. See erase_one() above. */
        if (t->tio.c_lflag & TTY_ICANON) {
            if (c == t->tio.c_cc[TTY_VERASE]) { erase_one(t); continue; }
            if (c == t->tio.c_cc[TTY_VKILL]) {
                /* ECHOKE rubs the line out column by column; plain ECHOK
                 * (what our defaults set, and what POSIX specifies for the
                 * flag) just moves to a fresh line. Either way the line
                 * itself goes. */
                bool ke = (t->tio.c_lflag & TTY_ECHOKE) != 0;
                while (!pending_line_empty(t)) {
                    if (ke) erase_one(t);
                    else { t->in_head = (t->in_head - 1) & (PTY_BUF - 1); }
                }
                if (!ke && (t->tio.c_lflag & TTY_ECHO) &&
                    (t->tio.c_lflag & TTY_ECHOK))
                    echo_to_master(t, '\n');
                continue;
            }
        }
        /* STOP at the first byte that does not fit and report a SHORT
         * WRITE. The caller has to know: this is the pty's only back
         * pressure, and pretending the byte landed is how an upload
         * larger than PTY_BUF lost its middle. */
        if (!ring_push(t->in, &t->in_head, t->in_tail, c)) break;
        echo_to_master(t, c);
    }
    if (put) {
        wq_wake_all(&t->wq_slave_read);   /* slave has input */
        if (t->tio.c_lflag & TTY_ECHO) wq_wake_all(&t->wq_master_read);
    }
    return (long)put;
}

/* Master side: "I will send no more input." Everything already in the ring
 * is still delivered; once it drains, slave reads return 0. The pty stays
 * open in the other direction, so the command can still write its answer. */
void pty_master_input_eof(struct file *f) {
    struct pty *t = f ? f->pty : 0;
    if (!t || t->in_eof) return;
    t->in_eof = true;
    wq_wake_all(&t->wq_slave_read);
}

long pty_master_read(struct file *f, void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    uint8_t *out = (uint8_t *)buf;
    while (t->out_head == t->out_tail) {
        if (t->slave_refs == 0) return 0;          /* slave gone -> EOF */
        struct proc *self = current_proc();
        if (SIG_SHOULD_EINTR(self)) return EINTR_RET;
        wq_add(&t->wq_master_read, self);
        self->state = PROC_BLOCKED;
        sched_yield();
        if (SIG_SHOULD_EINTR(self)) return EINTR_RET;
    }
    size_t got = 0;
    while (got < n) {
        int c = ring_pop(t->out, t->out_head, &t->out_tail);
        if (c < 0) break;
        out[got++] = (uint8_t)c;
    }
    return (long)got;
}

/* ---- kernel-side helpers (see pty.h) ----------------------------- */

int pty_index_of(struct file *f) {
    if (!f || f->kind != FILE_KIND_PTY_MASTER || !f->pty) return -1;
    return f->pty->index;
}

long pty_master_read_nb(struct file *f, void *buf, size_t n) {
    struct pty *t = f ? f->pty : 0;
    if (!t) return -1;
    /* Empty ring is NOT an error and NOT a reason to park: an in-kernel
     * poller calls this every tick and simply wants "nothing yet". Only a
     * departed slave is EOF, which is how the SSH server learns the shell
     * it spawned has exited. */
    if (t->out_head == t->out_tail) return (t->slave_refs == 0) ? 0 : -2;
    uint8_t *out = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        int c = ring_pop(t->out, t->out_head, &t->out_tail);
        if (c < 0) break;
        out[got++] = (uint8_t)c;
    }
    return (long)got;
}

/* Put the line discipline in RAW mode: no canonical line assembly, no
 * echo, no CR/LF translation, no signal characters.
 *
 * An SSH channel with no pty-req is not a terminal -- it is a byte pipe,
 * and OpenSSH gives it real pipes. This pty stands in for those, and in
 * canonical mode it could not carry the traffic: a 20 KB upload with no
 * newline in it is ONE line, and a canonical read assembles a line into a
 * PTY_BUF buffer, so `ssh host 'cat > file' < local` arrived truncated at
 * 4095 bytes. Echo would also have doubled every byte back at the client,
 * and ICRNL would have rewritten a \r in a binary file. */
void pty_set_raw(struct file *f) {
    struct pty *t = f ? f->pty : 0;
    if (!t) return;
    t->tio.c_lflag &= (uint32_t)~(TTY_ICANON | TTY_ECHO | TTY_ECHOE |
                                  TTY_ECHOK | TTY_ISIG);
    t->tio.c_iflag &= (uint32_t)~(TTY_ICRNL | TTY_INLCR);
    t->tio.c_oflag &= (uint32_t)~TTY_OPOST;
    t->tio.c_cc[TTY_VMIN] = 1;
    t->tio.c_cc[TTY_VTIME] = 0;
}

void pty_set_winsize(struct file *f, uint16_t cols, uint16_t rows) {
    struct pty *t = f ? f->pty : 0;
    if (!t) return;
    t->win.ws_col = cols;
    t->win.ws_row = rows;
}

/* ---- slave end -------------------------------------------------- */
/* True if the input ring already holds a complete canonical line (a '\n' or a
 * VEOF byte), so a canonical read can return without blocking. */
static bool in_has_line(struct pty *t) {
    uint8_t veof = t->tio.c_cc[TTY_VEOF];
    for (size_t i = t->in_tail; i != t->in_head; i = (i + 1) & (PTY_BUF - 1)) {
        if (t->in[i] == '\n' || t->in[i] == veof) return true;
    }
    return false;
}

static long slave_canonical_read(struct pty *t, uint8_t *out, size_t n) {
    /* Deliver any held-over completed line first. */
    if (t->line_pos < t->line_len) {
        size_t got = 0;
        while (got < n && t->line_pos < t->line_len) out[got++] = t->line[t->line_pos++];
        return (long)got;
    }
    /* Block until a whole line is available (or the master closed). */
    while (!in_has_line(t)) {
        if (t->master_refs == 0 || t->in_eof) {
            if (t->in_head == t->in_tail) return 0;   /* EOF */
            break;   /* deliver the partial trailing input on hangup */
        }
        struct proc *self = current_proc();
        if (SIG_SHOULD_EINTR(self)) return EINTR_RET;
        wq_add(&t->wq_slave_read, self);
        self->state = PROC_BLOCKED;
        sched_yield();
        if (SIG_SHOULD_EINTR(self)) return EINTR_RET;
    }
    /* Assemble one line into t->line[], applying VERASE/VKILL editing. */
    uint8_t verase = t->tio.c_cc[TTY_VERASE];
    uint8_t vkill  = t->tio.c_cc[TTY_VKILL];
    uint8_t veof   = t->tio.c_cc[TTY_VEOF];
    size_t  len = 0;
    bool    eof_line = false;
    for (;;) {
        int ci = ring_pop(t->in, t->in_head, &t->in_tail);
        if (ci < 0) break;
        uint8_t c = (uint8_t)ci;
        if (c == verase) { if (len) len--; continue; }
        if (c == vkill)  { len = 0; continue; }
        if (c == veof)   { eof_line = true; break; }   /* terminate (no NL) */
        if (len < PTY_BUF) t->line[len++] = c;
        if (c == '\n') break;
    }
    (void)eof_line;
    t->line_len = len;
    t->line_pos = 0;
    size_t got = 0;
    while (got < n && t->line_pos < t->line_len) out[got++] = t->line[t->line_pos++];
    return (long)got;
}

static long slave_raw_read(struct pty *t, uint8_t *out, size_t n) {
    size_t vmin = t->tio.c_cc[TTY_VMIN];
    if (vmin == 0) vmin = 1;
    size_t got = 0;
    while (got < n) {
        int c = ring_pop(t->in, t->in_head, &t->in_tail);
        if (c < 0) {
            if (got >= vmin) break;
            if (t->master_refs == 0 || t->in_eof) return got ? (long)got : 0;
            struct proc *self = current_proc();
            if (SIG_SHOULD_EINTR(self))
                return got ? (long)got : EINTR_RET;
            wq_add(&t->wq_slave_read, self);
            self->state = PROC_BLOCKED;
            sched_yield();
            if (SIG_SHOULD_EINTR(self))
                return got ? (long)got : EINTR_RET;
            continue;
        }
        out[got++] = (uint8_t)c;
    }
    return (long)got;
}

long pty_slave_read(struct file *f, void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    if (t->tio.c_lflag & TTY_ICANON) return slave_canonical_read(t, (uint8_t *)buf, n);
    return slave_raw_read(t, (uint8_t *)buf, n);
}

long pty_slave_write(struct file *f, const void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    const uint8_t *in = (const uint8_t *)buf;
    bool opost = (t->tio.c_oflag & TTY_OPOST) != 0;
    bool onlcr = (t->tio.c_oflag & TTY_ONLCR) != 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (opost && onlcr && c == '\n')
            ring_push(t->out, &t->out_head, t->out_tail, '\r');
        ring_push(t->out, &t->out_head, t->out_tail, c);
    }
    if (n) wq_wake_all(&t->wq_master_read);
    return (long)n;
}

/* ---- ioctl ------------------------------------------------------ */
long pty_ioctl(struct file *f, unsigned long cmd, unsigned long arg) {
    struct pty *t = f->pty;
    if (!t) return -ABI_EBADF;
    switch (cmd) {
    case TTY_TCGETS:
        if (copy_to_user((void *)arg, &t->tio, sizeof(t->tio)) != 0) return -ABI_EFAULT;
        return 0;
    case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF: {
        struct ktermios nt;
        if (copy_from_user(&nt, (const void *)arg, sizeof(nt)) != 0) return -ABI_EFAULT;
        t->tio = nt;
        return 0;
    }
    case TTY_TIOCGWINSZ:
        if (copy_to_user((void *)arg, &t->win, sizeof(t->win)) != 0) return -ABI_EFAULT;
        return 0;
    case TTY_TIOCSWINSZ: {
        struct kwinsize w;
        if (copy_from_user(&w, (const void *)arg, sizeof(w)) != 0) return -ABI_EFAULT;
        t->win = w;
        return 0;
    }
    case TTY_TIOCGPGRP: {
        int pg = t->pgrp;
        if (copy_to_user((void *)arg, &pg, sizeof(pg)) != 0) return -ABI_EFAULT;
        return 0;
    }
    case TTY_TIOCSPGRP: {
        int pg;
        if (copy_from_user(&pg, (const void *)arg, sizeof(pg)) != 0) return -ABI_EFAULT;
        t->pgrp = pg;
        return 0;
    }
    case TTY_FIONREAD: {
        int navail;
        if (f->kind == FILE_KIND_PTY_MASTER)
            navail = (int)ring_count(t->out_head, t->out_tail);
        else
            navail = (int)ring_count(t->in_head, t->in_tail) +
                     (int)(t->line_len - t->line_pos);
        if (copy_to_user((void *)arg, &navail, sizeof(navail)) != 0) return -ABI_EFAULT;
        return 0;
    }
    case PTY_TIOCGPTN: {                  /* master only */
        if (f->kind != FILE_KIND_PTY_MASTER) return -ABI_ENOTTY;
        int n = t->index;
        if (copy_to_user((void *)arg, &n, sizeof(n)) != 0) return -ABI_EFAULT;
        return 0;
    }
    case PTY_TIOCSPTLCK:                  /* unlock slave: accept, no locking */
        return 0;
    case PTY_TIOCSCTTY:                   /* set controlling tty: accept */
        return 0;
    case TTY_TCFLSH:
        return 0;
    default:
        return -ABI_ENOTTY;
    }
}

/* ---- poll readiness --------------------------------------------- */
bool pty_readable(struct file *f) {
    struct pty *t = f->pty;
    if (!t) return false;
    if (f->kind == FILE_KIND_PTY_MASTER)
        return t->out_head != t->out_tail;
    /* slave */
    if (t->line_pos < t->line_len) return true;
    if (t->tio.c_lflag & TTY_ICANON) return in_has_line(t);
    return t->in_head != t->in_tail;
}

bool pty_hup(struct file *f) {
    struct pty *t = f->pty;
    if (!t) return true;
    if (f->kind == FILE_KIND_PTY_MASTER) return t->slave_refs == 0;
    return t->master_refs == 0;
}
