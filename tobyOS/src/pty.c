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
#include <tobyos/cpu.h>

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
static void ring_push(uint8_t *buf, size_t *head, size_t tail, uint8_t c) {
    if (ring_full(*head, tail)) return;   /* drop on overflow */
    buf[*head] = c;
    *head = (*head + 1) & (PTY_BUF - 1);
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
    pty_maybe_free(t);
}

void pty_clone_ref(struct file *f) {
    struct pty *t = f->pty;
    if (!t) return;
    if (f->kind == FILE_KIND_PTY_MASTER) t->master_refs++;
    else                                 t->slave_refs++;
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
        ring_push(t->in, &t->in_head, t->in_tail, c);
        echo_to_master(t, c);
    }
    if (put) {
        wq_wake_all(&t->wq_slave_read);   /* slave has input */
        if (t->tio.c_lflag & TTY_ECHO) wq_wake_all(&t->wq_master_read);
    }
    return (long)put;
}

long pty_master_read(struct file *f, void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    uint8_t *out = (uint8_t *)buf;
    while (t->out_head == t->out_tail) {
        if (t->slave_refs == 0) return 0;          /* slave gone -> EOF */
        struct proc *self = current_proc();
        if (self->pending_signals) return EINTR_RET;
        wq_add(&t->wq_master_read, self);
        self->state = PROC_BLOCKED;
        sched_yield();
        if (self->pending_signals) return EINTR_RET;
    }
    size_t got = 0;
    while (got < n) {
        int c = ring_pop(t->out, t->out_head, &t->out_tail);
        if (c < 0) break;
        out[got++] = (uint8_t)c;
    }
    return (long)got;
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
        if (t->master_refs == 0) {
            if (t->in_head == t->in_tail) return 0;   /* EOF */
            break;   /* deliver the partial trailing input on hangup */
        }
        struct proc *self = current_proc();
        if (self->pending_signals) return EINTR_RET;
        wq_add(&t->wq_slave_read, self);
        self->state = PROC_BLOCKED;
        sched_yield();
        if (self->pending_signals) return EINTR_RET;
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
            if (t->master_refs == 0) return got ? (long)got : 0;
            struct proc *self = current_proc();
            if (self->pending_signals) return got ? (long)got : EINTR_RET;
            wq_add(&t->wq_slave_read, self);
            self->state = PROC_BLOCKED;
            sched_yield();
            if (self->pending_signals) return got ? (long)got : EINTR_RET;
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
