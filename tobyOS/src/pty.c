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
#include <tobyos/perf.h>

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
    struct proc *wq_slave_write;   /* parked waiting for room in `out` */

    /* IXON: output stopped by VSTOP (or TCOOFF) until VSTART -- what the
     * master reads waits in `out`, and a writer that fills it waits */
    bool   stopped;
    /* IEXTEN's VLNEXT: the next input byte is taken as itself */
    bool   lnext;
    /* the output column, for ONOCR and tab3 (n_tty's) */
    unsigned column;

    /* The master says no more input will ever arrive, while the pty stays
     * open for OUTPUT. That is what an SSH client means by
     * SSH_MSG_CHANNEL_EOF, and there was no way to express it: the only
     * end-of-input this pty had was the master closing, which also tears
     * down the direction the command still needs to answer on. Without it
     * `ssh host 'cat > file' < local` -- how edremote uploads a script --
     * left cat blocked on a stdin that never ended, forever. */
    bool   in_eof;

    /* THE SLAVE NODE'S OWNER AND TIMES, /dev/pts/N's stat (finding 190).
     * Every device node said 1970 and root, so GNU who's IDLE column --
     * now less the line's atime -- said "old" for every login. Linux's
     * devpts: the opener's uid, group tty, 0620 (as systemd mounts it),
     * the three times the creation's; a read of the slave moves the
     * atime and a write the mtime -- only when the second has left its
     * 8-second bucket, so the node does not time keystrokes for anyone
     * who can stat it (measured: logs/b31/ptytime-wsl.out). sshd then
     * gives the node to the user who logged in (pty_set_owner). */
    uint32_t uid, gid;
    uint64_t atime, mtime, ctime;     /* wall-clock seconds */
};

static struct pty g_ptys[PTY_MAX];

#define PTY_TTY_GID 5                  /* the tty group, as on Linux */

static uint64_t pty_wall_now(void) {
    extern uint64_t lx_realtime_ns(uint64_t mono_ns);
    return lx_realtime_ns(perf_now_ns()) / 1000000000ull;
}

/* Linux's tty_update_time rule: store NOW only once it differs from what
 * is stored in more than the low three bits. */
static void pty_touch(uint64_t *t) {
    uint64_t now = pty_wall_now();
    if ((now ^ *t) & ~(uint64_t)7) *t = now;
}
/* Each allocation of a slot bumps its generation (see pty.h): a
 * controlling-terminal reference names the pair, not the slot. */
static uint32_t g_pty_gen[PTY_MAX];

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
    /* As pipe.c's and socket.c's do (slice 67): a wake here is a pty end
     * becoming ready, which a parked poll()er -- and the SSH worker,
     * waiting on its session's master (finding 191) -- is waiting for.
     * Without it they found out at the 20 ms sweep. */
    poll_event_notify();
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
/* A Linux pty's settings (tty_std_termios, the pty slave's cflag): `stty`
 * on a fresh one lists "speed 38400 baud; line = 0;" and "-brkint
 * -imaxbel" and nothing else. Until batch 26 (finding 133) c_cflag was 0
 * -- "speed 0 baud ... cs5 -cread" -- and ixon, echoctl, echoke and iexten
 * were off, with the line discipline doing none of them. */
static void pty_defaults(struct pty *t) {
    memset(&t->tio, 0, sizeof(t->tio));
    t->tio.c_iflag = TTY_ICRNL | TTY_IXON;
    t->tio.c_oflag = TTY_OPOST | TTY_ONLCR;
    t->tio.c_cflag = TTY_B38400 | TTY_CS8 | TTY_CREAD | (TTY_B38400 << TTY_IBSHIFT);
    t->tio.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK |
                     TTY_ECHOCTL | TTY_ECHOKE | TTY_IEXTEN;
    t->tio.c_cc[TTY_VINTR]  = 3;     /* ^C */
    t->tio.c_cc[TTY_VQUIT]  = 0x1c;  /* ^\ */
    t->tio.c_cc[TTY_VERASE] = 0x7f;  /* DEL */
    t->tio.c_cc[TTY_VKILL]  = 0x15;  /* ^U */
    t->tio.c_cc[TTY_VEOF]   = 4;     /* ^D */
    t->tio.c_cc[TTY_VMIN]   = 1;
    t->tio.c_cc[TTY_VTIME]  = 0;
    t->tio.c_cc[TTY_VSTART] = 0x11;  /* ^Q */
    t->tio.c_cc[TTY_VSTOP]  = 0x13;  /* ^S */
    t->tio.c_cc[TTY_VSUSP]  = 0x1a;  /* ^Z */
    t->tio.c_cc[TTY_VREPRINT] = 0x12; /* ^R */
    t->tio.c_cc[TTY_VDISCARD] = 0x0f; /* ^O (kept, as Linux keeps it: unused) */
    t->tio.c_cc[TTY_VWERASE] = 0x17; /* ^W */
    t->tio.c_cc[TTY_VLNEXT] = 0x16;  /* ^V */
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
    g_pty_gen[idx]++;
    t->master_refs = 1;
    t->slave_refs = 0;
    pty_defaults(t);
    {
        struct proc *op = current_proc();
        t->uid = op ? (uint32_t)op->uid : 0;
        t->gid = PTY_TTY_GID;
        t->atime = t->mtime = t->ctime = pty_wall_now();
    }
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

bool pty_slave_live(int index) {
    if (index < 0 || index >= PTY_MAX) return false;
    return g_ptys[index].used && g_ptys[index].master_refs > 0;
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
        if (t->master_refs == 0) {
            wq_wake_all(&t->wq_slave_read);   /* slave EOF */
            wq_wake_all(&t->wq_slave_write);  /* and a writer's EIO */
        }
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
static bool echo_ctl(const struct pty *t, uint8_t c) {
    return (t->tio.c_lflag & TTY_ECHOCTL) && ((c < 0x20 && c != '\t') || c == 0x7f);
}

static int echo_width(const struct pty *t, uint8_t c) {
    return echo_ctl(t, c) ? 2 : 1;
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
        int w = echo_width(t, victim);
        for (int i = 0; i < w; i++) {
            ring_push(t->out, &t->out_head, t->out_tail, '\b');
            ring_push(t->out, &t->out_head, t->out_tail, ' ');
            ring_push(t->out, &t->out_head, t->out_tail, '\b');
        }
    }
}

/* ---- echo (master-write time, slave ECHO set) ------------------- */
static void echo_out(struct pty *t, uint8_t c) {
    ring_push(t->out, &t->out_head, t->out_tail, c);
}

/* A control character echoes as ^X (DEL as ^?) under ECHOCTL, as itself
 * without it -- it was ^X whatever the flag said. A newline is a new line
 * only in canonical mode (n_tty); otherwise it is a control character like
 * the others, ^J -- and \r, which was a new line too, is ^M. EXTPROC: the
 * other side echoes. */
static void echo_to_master(struct pty *t, uint8_t c) {
    uint32_t lf = t->tio.c_lflag;
    if (!(lf & TTY_ECHO) || (lf & TTY_EXTPROC)) return;
    if (c == '\n' && (lf & TTY_ICANON)) {
        uint32_t of = t->tio.c_oflag;
        if ((of & TTY_OPOST) && (of & TTY_ONLCR)) echo_out(t, '\r');
        echo_out(t, '\n');
    } else if (echo_ctl(t, c)) {
        echo_out(t, '^');
        echo_out(t, (uint8_t)(c ^ 0x40));
    } else {
        echo_out(t, c);
    }
}

/* The start of the line still being typed: after the last newline (or
 * other line end) in the input ring. */
static size_t pending_line_start(struct pty *t) {
    size_t i = t->in_head;
    while (i != t->in_tail) {
        size_t prev = (i - 1) & (PTY_BUF - 1);
        uint8_t c = t->in[prev];
        if (c == '\n' || (c && (c == t->tio.c_cc[TTY_VEOF] || c == t->tio.c_cc[TTY_VEOL] ||
                                c == t->tio.c_cc[TTY_VEOL2])))
            break;
        i = prev;
    }
    return i;
}

/* IEXTEN's VWERASE: back over what is not a word (letters, digits, _),
 * then over the word -- Linux's (BSD's ALTWERASE) rule */
static void werase(struct pty *t) {
    int seen = 0;
    while (!pending_line_empty(t)) {
        uint8_t c = t->in[(t->in_head - 1) & (PTY_BUF - 1)];
        bool word = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') || c == '_';
        if (word) seen++;
        else if (seen) break;
        erase_one(t);
    }
}

/* IEXTEN's VREPRINT: the character, a new line, the line so far */
static void reprint(struct pty *t, uint8_t c) {
    if (!(t->tio.c_lflag & TTY_ECHO)) return;
    echo_to_master(t, c);
    echo_out(t, '\r');
    echo_out(t, '\n');
    for (size_t i = pending_line_start(t); i != t->in_head; i = (i + 1) & (PTY_BUF - 1))
        echo_to_master(t, t->in[i]);
}

/* What a signal character does to the input not yet read: gone, unless
 * NOFLSH (Linux's isig). */
static void flush_input(struct pty *t) {
    t->in_tail = t->in_head;
    t->line_len = t->line_pos = 0;
    t->lnext = false;
}

/* Output may go on (VSTART, IXANY, TCOON, a signal character). */
static void start_output(struct pty *t) {
    if (!t->stopped) return;
    t->stopped = false;
    wq_wake_all(&t->wq_master_read);
    wq_wake_all(&t->wq_slave_write);
}

/* ---- master end ------------------------------------------------- */
long pty_master_write(struct file *f, const void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    const uint8_t *in = (const uint8_t *)buf;
    size_t put = 0;
    /* where this write's echoes begin: n_tty commits a write's echoes when
     * it has processed the whole of it, and a signal character's flush
     * drops the ones not yet out -- "xyz^C" echoes "^C" alone */
    size_t echo_from = t->out_head;
    for (; put < n; put++) {
        uint8_t c = in[put];
        uint32_t lf = t->tio.c_lflag, ifl = t->tio.c_iflag;
        const uint8_t *cc = t->tio.c_cc;
        /* the byte after IEXTEN's VLNEXT: itself, whatever it is */
        if (t->lnext) {
            if (!ring_push(t->in, &t->in_head, t->in_tail, c)) break;
            t->lnext = false;
            echo_to_master(t, c);
            continue;
        }
        if ((ifl & TTY_IGNCR) && c == '\r') continue;
        if ((ifl & TTY_ICRNL) && c == '\r') c = '\n';
        else if ((ifl & TTY_INLCR) && c == '\n') c = '\r';
        if ((ifl & TTY_IUCLC) && (lf & TTY_IEXTEN) && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        /* IXON: VSTOP and VSTART are not input; IXANY lets any byte
         * restart the output. A 0 in c_cc is a character switched off. */
        if (ifl & TTY_IXON) {
            if (c && c == cc[TTY_VSTOP]) { t->stopped = true; continue; }
            if (c && c == cc[TTY_VSTART]) { start_output(t); continue; }
            if (ifl & TTY_IXANY) start_output(t);
        }
        /* ISIG: ^C, ^\ and ^Z are SIGNALS to the foreground group, not input
         * bytes. They echo (^C ...) like every control char, and never
         * reach the input ring -- exactly n_tty -- and what was typed ahead
         * goes, unless NOFLSH; output stopped by IXON starts again. ^\ was
         * input here, and typed-ahead lines outlived a ^C. */
        if (lf & TTY_ISIG) {
            int sig = 0;
            if (c && c == cc[TTY_VINTR]) sig = SIGINT;
            else if (c && c == cc[TTY_VQUIT]) sig = SIGQUIT;
            else if (c && c == cc[TTY_VSUSP]) sig = SIGTSTP;
            if (sig) {
                if (!(lf & TTY_NOFLSH)) {
                    flush_input(t);
                    t->out_head = echo_from;
                }
                if (ifl & TTY_IXON) start_output(t);
                echo_to_master(t, c);
                if (t->pgrp > 0) signal_send_to_pgrp(t->pgrp, sig);
                continue;
            }
        }
        /* ICANON editing, applied HERE rather than at line-assembly time
         * so the rub-out can be echoed. See erase_one() above. */
        if (lf & TTY_ICANON) {
            if (c && c == cc[TTY_VERASE]) { erase_one(t); continue; }
            if ((lf & TTY_IEXTEN) && c) {
                if (c == cc[TTY_VWERASE]) { werase(t); continue; }
                if (c == cc[TTY_VREPRINT]) { reprint(t, c); continue; }
                if (c == cc[TTY_VLNEXT]) {
                    t->lnext = true;
                    if ((lf & TTY_ECHO) && (lf & TTY_ECHOCTL)) {
                        echo_out(t, '^');
                        echo_out(t, '\b');
                    }
                    continue;
                }
            }
            /* end of file is not echoed (n_tty) */
            if (c && c == cc[TTY_VEOF]) {
                if (!ring_push(t->in, &t->in_head, t->in_tail, c)) break;
                continue;
            }
            if (c && c == t->tio.c_cc[TTY_VKILL]) {
                /* ECHOK ECHOKE ECHOE (a Linux pty's defaults) rub the line
                 * out column by column; otherwise the kill character is
                 * echoed (^U) and, under ECHOK, a new line follows -- n_tty's
                 * rule. Either way the line itself goes. */
                uint32_t kf = TTY_ECHOK | TTY_ECHOKE | TTY_ECHOE;
                bool ke = (t->tio.c_lflag & kf) == kf;
                while (!pending_line_empty(t)) {
                    if (ke) erase_one(t);
                    else { t->in_head = (t->in_head - 1) & (PTY_BUF - 1); }
                }
                if (!ke) {
                    echo_to_master(t, c);
                    if (t->tio.c_lflag & TTY_ECHOK) echo_to_master(t, '\n');
                }
                continue;
            }
        }
        /* STOP at the first byte that does not fit and report a SHORT
         * WRITE. The caller has to know: this is the pty's only back
         * pressure, and pretending the byte landed is how an upload
         * larger than PTY_BUF lost its middle. */
        if (!ring_push(t->in, &t->in_head, t->in_tail, c)) break;
        if ((lf & TTY_ECHO) || !(lf & TTY_ICANON) || !(lf & TTY_ECHONL) || c != '\n')
            echo_to_master(t, c);
        else {                                   /* ECHONL: the newline only */
            echo_out(t, '\r');
            echo_out(t, '\n');
        }
    }
    if (put) {
        wq_wake_all(&t->wq_slave_read);   /* slave has input */
        wq_wake_all(&t->wq_master_read);  /* an echo, or output restarted */
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
    /* IXON: stopped output waits (until the slave side has gone) */
    while (t->out_head == t->out_tail || (t->stopped && t->slave_refs > 0)) {
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
    if (got) wq_wake_all(&t->wq_slave_write);      /* room again */
    return (long)got;
}

/* ---- kernel-side helpers (see pty.h) ----------------------------- */

int pty_index_of(struct file *f) {
    if (!f || f->kind != FILE_KIND_PTY_MASTER || !f->pty) return -1;
    return f->pty->index;
}

int pty_file_index(struct file *f) {
    if (!f || !f->pty) return -1;
    if (f->kind != FILE_KIND_PTY_MASTER && f->kind != FILE_KIND_PTY_SLAVE)
        return -1;
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
    if (t->stopped && t->slave_refs > 0) return -2;      /* IXON */
    uint8_t *out = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        int c = ring_pop(t->out, t->out_head, &t->out_tail);
        if (c < 0) break;
        out[got++] = (uint8_t)c;
    }
    if (got) wq_wake_all(&t->wq_slave_write);
    return (long)got;
}

/* What the master could read now without waiting, and whether the slave
 * end is still open -- the SSH server's linger asks (finding 191). */
size_t pty_master_pending(struct file *f, bool *slave_open) {
    struct pty *t = f ? f->pty : 0;
    if (slave_open) *slave_open = t && t->slave_refs > 0;
    if (!t || (t->stopped && t->slave_refs > 0)) return 0;
    return ring_count(t->out_head, t->out_tail);
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
static bool line_end(const struct pty *t, uint8_t c) {
    return c == '\n' || (c && (c == t->tio.c_cc[TTY_VEOF] || c == t->tio.c_cc[TTY_VEOL] ||
                              c == t->tio.c_cc[TTY_VEOL2]));
}

static bool in_has_line(struct pty *t) {
    for (size_t i = t->in_tail; i != t->in_head; i = (i + 1) & (PTY_BUF - 1)) {
        if (line_end(t, t->in[i])) return true;
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
    /* Assemble one line into t->line[]. It was edited as it was typed
     * (pty_master_write); a VERASE or VKILL byte here is one taken as
     * itself -- after VLNEXT, or before ICANON came on -- and stays, as
     * n_tty keeps it. (They were applied again here, so ^V^U killed.) */
    uint8_t veof   = t->tio.c_cc[TTY_VEOF];
    size_t  len = 0;
    bool    eof_line = false;
    for (;;) {
        int ci = ring_pop(t->in, t->in_head, &t->in_tail);
        if (ci < 0) break;
        uint8_t c = (uint8_t)ci;
        if (c && c == veof)   { eof_line = true; break; }   /* terminate (no NL) */
        if (len < PTY_BUF) t->line[len++] = c;
        if (line_end(t, c)) break;                /* \n, VEOL, VEOL2 */
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

/* Is process group PG of session SID ORPHANED -- no member with a parent
 * in another group of the same session (POSIX XBD 3.265), so nothing is
 * left that could continue it if it stopped? */
static bool pty_pgrp_orphaned(int pg, int sid) {
    extern struct proc g_proc[];
    for (int i = 1; i < PROC_MAX; i++) {
        struct proc *q = &g_proc[i];
        if (q->state == PROC_UNUSED || q->state == PROC_EMBRYO ||
            q->state == PROC_TERMINATED || q->is_thread) continue;
        if ((q->pgid > 0 ? q->pgid : q->pid) != pg) continue;
        struct proc *par = q->ppid > 0 ? proc_lookup(q->ppid) : 0;
        if (!par) continue;
        if ((par->pgid > 0 ? par->pgid : par->pid) != pg && par->sid == sid)
            return false;
    }
    return true;
}

/* JOB CONTROL AT THE TERMINAL (POSIX XBD 11.1.4; Linux's tty_check_change).
 * A process in a BACKGROUND group of the session whose controlling
 * terminal this is has SIG sent to its group when it READS the terminal
 * (SIGTTIN), WRITES it under TOSTOP, or CHANGES it -- its modes, its
 * foreground group, a flush (SIGTTOU): by default that stops the job, and
 * the call runs again once it is continued (signal_deliver restarts it).
 * A SIGTTIN ignored or blocked is EIO; a SIGTTOU ignored or blocked lets
 * the call through, which is how a shell takes its terminal back from a
 * finished job; an orphaned group, which nothing could continue, is EIO.
 * None was sent: `cat &` at a prompt read the keys typed at the shell
 * (finding 89), and a background `stty -echo` changed the terminal under
 * it. Only the session's own terminal, as Linux checks; 0 when the call
 * may go on. */
static long pty_job_check(struct pty *t, int sig) {
    struct proc *p = current_proc();
    if (!p || p->pid == 0 || p->is_kernel) return 0;
    if (p->ctty_kind != PROC_CTTY_PTY || p->ctty_index != t->index ||
        p->ctty_gen != g_pty_gen[t->index]) return 0;
    int pg = p->pgid > 0 ? p->pgid : p->pid;
    if (t->pgrp <= 0 || pg == t->pgrp) return 0;
    struct sigaction *act = sig_actions_of(p);
    if ((act && act[sig].sa_handler == SIG_IGN) ||
        (p->sigstate.mask & SIGMASK(sig)))
        return sig == SIGTTIN ? -ABI_EIO : 0;
    if (pty_pgrp_orphaned(pg, p->sid)) return -ABI_EIO;
    signal_send_to_pgrp(pg, sig);
    return EINTR_RET;
}

static long pty_bg_read(struct pty *t) { return pty_job_check(t, SIGTTIN); }

long pty_slave_read(struct file *f, void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    {
        long jc = pty_bg_read(t);
        if (jc) return jc;
    }
    long r = (t->tio.c_lflag & TTY_ICANON)
           ? slave_canonical_read(t, (uint8_t *)buf, n)
           : slave_raw_read(t, (uint8_t *)buf, n);
    if (r > 0) pty_touch(&t->atime);      /* tty_read's update, Linux's */
    return r;
}

long pty_slave_write(struct file *f, const void *buf, size_t n) {
    struct pty *t = f->pty;
    if (!t) return -1;
    if (t->tio.c_lflag & TTY_TOSTOP) {          /* see pty_job_check */
        long jc = pty_job_check(t, SIGTTOU);
        if (jc) return jc;
    }
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t of = t->tio.c_oflag;
    bool opost = (of & TTY_OPOST) != 0;
    /* A FULL RING WAITS for the master to read (batch 26): the bytes were
     * dropped and counted as written, which output stopped by ^S would
     * have made routine. pid 0 (an in-kernel writer) cannot wait on the
     * reader it may itself be, and keeps the old way. */
    struct proc *self = current_proc();
    bool can_wait = self && self->pid != 0 && !self->is_kernel;
    size_t i = 0;
    while (i < n) {
        uint8_t c = in[i];
        unsigned col0 = t->column;            /* for a retry after a wait */
        /* OPOST, as n_tty's do_output_char: what this byte becomes */
        uint8_t o[8];
        size_t need = 0;
        if (!opost) o[need++] = c;
        else if (c == '\n') {
            if (of & TTY_ONLRET) t->column = 0;
            if (of & TTY_ONLCR) { o[need++] = '\r'; t->column = 0; }
            o[need++] = '\n';
        } else if (c == '\r') {
            if ((of & TTY_ONOCR) && t->column == 0) { i++; continue; }
            if (of & TTY_OCRNL) {
                if (of & TTY_ONLRET) t->column = 0;
                o[need++] = '\n';
            } else {
                t->column = 0;
                o[need++] = '\r';
            }
        } else if (c == '\t' && (of & TTY_TABDLY) == TTY_XTABS) {
            unsigned sp = 8 - (t->column & 7);
            for (unsigned k = 0; k < sp; k++) o[need++] = ' ';
            t->column += sp;
        } else {
            if (c == '\t') t->column = (t->column | 7) + 1;
            else if (c == '\b') { if (t->column) t->column--; }
            else if (c >= 0x20 && c != 0x7f) {
                if ((of & TTY_OLCUC) && c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
                if ((c & 0xc0) != 0x80) t->column++;     /* not a UTF-8 continuation */
            }
            o[need++] = c;
        }
        if (can_wait && (t->stopped || PTY_BUF - 1 - ring_count(t->out_head, t->out_tail) < need)) {
            t->column = col0;
            if (t->master_refs == 0) return i ? (long)i : -ABI_EIO;
            if (i) wq_wake_all(&t->wq_master_read);
            if (SIG_SHOULD_EINTR(self)) return i ? (long)i : EINTR_RET;
            wq_add(&t->wq_slave_write, self);
            self->state = PROC_BLOCKED;
            sched_yield();
            if (SIG_SHOULD_EINTR(self)) return i ? (long)i : EINTR_RET;
            continue;
        }
        for (size_t k = 0; k < need; k++) ring_push(t->out, &t->out_head, t->out_tail, o[k]);
        i++;
    }
    if (n) {
        wq_wake_all(&t->wq_master_read);
        pty_touch(&t->mtime);             /* tty_write's update, Linux's */
    }
    return (long)n;
}

/* /dev/pts/INDEX's owner and times, for stat (finding 190); false if the
 * pair is not live. */
bool pty_node_info(int index, uint32_t *uid, uint32_t *gid, uint64_t *atime,
                   uint64_t *mtime, uint64_t *ctime) {
    if (index < 0 || index >= PTY_MAX || !g_ptys[index].used) return false;
    const struct pty *t = &g_ptys[index];
    if (uid) *uid = t->uid;
    if (gid) *gid = t->gid;
    if (atime) *atime = t->atime;
    if (mtime) *mtime = t->mtime;
    if (ctime) *ctime = t->ctime;
    return true;
}

/* Give the slave node to a login, as sshd's pty_setowner does: the user,
 * the tty group, 0620 -- a chown, so its ctime moves. */
void pty_set_owner(int index, uint32_t uid, uint32_t gid) {
    if (index < 0 || index >= PTY_MAX || !g_ptys[index].used) return;
    g_ptys[index].uid = uid;
    g_ptys[index].gid = gid;
    g_ptys[index].ctime = pty_wall_now();
}

/* ---- ioctl ------------------------------------------------------ */
long pty_ioctl(struct file *f, unsigned long cmd, unsigned long arg) {
    struct pty *t = f->pty;
    if (!t) return -ABI_EBADF;
    /* A CHANGE to the terminal from the background: SIGTTOU (see
     * pty_job_check) -- its modes, its foreground group, a flush, as
     * Linux's tty_check_change is asked for them. The slave's side only:
     * the master is the emulator's. */
    if (f->kind == FILE_KIND_PTY_SLAVE &&
        (cmd == TTY_TCSETS || cmd == TTY_TCSETSW || cmd == TTY_TCSETSF ||
         cmd == TTY_TIOCSPGRP || cmd == TTY_TCFLSH)) {
        long jc = pty_job_check(t, SIGTTOU);
        if (jc) return jc;
    }
    switch (cmd) {
    case TTY_TCGETS:
        if (copy_to_user((void *)arg, &t->tio, sizeof(t->tio)) != 0) return -ABI_EFAULT;
        return 0;
    case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF: {
        struct ktermios nt;
        if (copy_from_user(&nt, (const void *)arg, sizeof(nt)) != 0) return -ABI_EFAULT;
        /* What a Linux pty takes (pty_set_termios, tty_ioctl): its bytes
         * are 8 bits and always read -- CS8 and CREAD forced, PARENB
         * cleared -- and an input speed of 0 is the output speed. Asked
         * for nothing it can do, it says EINVAL and changes nothing, as
         * POSIX has tcsetattr when "no part of the request can be honored";
         * GNU stty then says "Invalid argument", and "unable to perform
         * all requested operations" when only part was. They were stored
         * as given. */
        bool asked = memcmp(&nt, &t->tio, sizeof nt) != 0;
        nt.c_cflag = (nt.c_cflag & ~(TTY_CSIZE | TTY_PARENB)) | TTY_CS8 | TTY_CREAD;
        if (asked && memcmp(&nt, &t->tio, sizeof nt) == 0) return -ABI_EINVAL;
        /* (an input speed of 0 is honoured -- it becomes the output's) */
        if (!(nt.c_cflag & TTY_CIBAUD))
            nt.c_cflag |= (nt.c_cflag & TTY_CBAUD) << TTY_IBSHIFT;
        if (cmd == TTY_TCSETSF) flush_input(t);
        t->tio = nt;
        if (!(t->tio.c_iflag & TTY_IXON)) start_output(t);
        if (!(t->tio.c_lflag & TTY_ICANON)) t->lnext = false;
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
            navail = (t->stopped && t->slave_refs > 0) ? 0
                     : (int)ring_count(t->out_head, t->out_tail);
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
    case PTY_TIOCSCTTY:                   /* set controlling tty */
        /* only for the session leader that has none -- a terminal
         * emulator's child after setsid() -- as Linux has it; it was
         * accepted and nothing was set */
        if (f->kind != FILE_KIND_PTY_SLAVE) return -ABI_EINVAL;
        return pty_make_ctty(current_proc(), t->index, false);
    case TTY_TCFLSH:                      /* TCIFLUSH 0, TCOFLUSH 1, TCIOFLUSH 2 */
        if (arg > 2) return -ABI_EINVAL;
        if (arg != 1) flush_input(t);
        if (arg != 0) {
            t->out_tail = t->out_head;
            wq_wake_all(&t->wq_slave_write);
        }
        return 0;
    case TTY_TCXONC:                      /* TCOOFF 0, TCOON 1, TCIOFF 2, TCION 3 */
        if (arg > 3) return -ABI_EINVAL;
        if (arg == 0) t->stopped = true;
        else if (arg == 1) start_output(t);
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
        return t->out_head != t->out_tail && (!t->stopped || t->slave_refs == 0);
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

/* ---- controlling terminals ------------------------------------- */

uint32_t pty_gen_of(int index) {
    return (index >= 0 && index < PTY_MAX) ? g_pty_gen[index] : 0;
}

/* The foreground process group of pair INDEX (TIOCGPGRP's answer), 0 if
 * the pair is not in use. /proc/PID/stat's tpgid (procfs.c). */
int pty_fg_pgrp(int index) {
    return (index >= 0 && index < PTY_MAX && g_ptys[index].used) ? g_ptys[index].pgrp : 0;
}

bool pty_ctty_live(int index, uint32_t gen) {
    return index >= 0 && index < PTY_MAX && g_ptys[index].used &&
           g_ptys[index].master_refs > 0 && g_pty_gen[index] == gen;
}

long pty_make_ctty(struct proc *p, int index, bool force) {
    if (!p || index < 0 || index >= PTY_MAX || !g_ptys[index].used)
        return -ABI_EINVAL;
    if (!force) {
        if (p->ctty_kind == PROC_CTTY_PTY && p->ctty_index == index &&
            p->ctty_gen == g_pty_gen[index])
            return 0;                       /* this one already */
        if (p->sid != p->pid || p->ctty_kind != PROC_CTTY_NONE)
            return -ABI_EPERM;
    }
    p->ctty_kind = PROC_CTTY_PTY;
    p->ctty_index = index;
    p->ctty_gen = g_pty_gen[index];
    g_ptys[index].pgrp = p->pgid > 0 ? p->pgid : p->pid;
    return 0;
}
