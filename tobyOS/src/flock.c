/* flock.c -- POSIX record locks (fcntl F_SETLK/F_SETLKW/F_GETLK) and
 * flock(2), for real (2026-08-22).
 *
 * Before this file, every locking command was `return 0`: SQLite, dpkg-style
 * tools and every multi-process database got "yes, you hold the lock" with
 * no exclusion behind it -- silent corruption instead of contention. That is
 * the accept-and-ignore lie this tree keeps hunting, in its most dangerous
 * costume.
 *
 * IDENTITY. A lock attaches to THE FILE, not the descriptor. This VFS's
 * durable file identity is the one fstat and the MAP_SHARED page cache
 * already use: (mnt, ino, ino_gen) where the fs assigns inode numbers
 * (tobyfs), falling back to the driver's stable node pointer (vfs.priv) for
 * ramfs et al. Two opens of the same file agree on it; two files never do.
 *
 * OWNERSHIP, per POSIX:
 *   - fcntl record locks are owned by the PROCESS (tgid, so threads share
 *     ownership -- two threads of one process never conflict). They die when
 *     the process exits or closes ANY descriptor to the file (the classic
 *     POSIX wart, kept because software is written against it).
 *   - flock locks are owned by the OPEN FILE DESCRIPTION. The description
 *     identity here is the vfs_refs counter pointer (dup/fork share it,
 *     separate opens do not), and the lock dies when the description does.
 *
 * BLOCKING (F_SETLKW, flock without LOCK_NB) is a cooperative retry loop:
 * the waiter stays RUNNING and yields, re-checks on each pass, and leaves
 * with EINTR when a signal lands -- same shape as the socket wait loops.
 * Lock contention is rare enough that a parked-waiter design isn't worth
 * its wake plumbing yet.
 *
 * All state is BKL-serialised (every caller is a syscall arm). */

#include <tobyos/flock.h>
#include <tobyos/proc.h>
#include <tobyos/file.h>
#include <tobyos/sched.h>
#include <tobyos/abi/abi.h>

#define FL_MAX 128

#define FL_END_MAX 0xffffffffffffffffull   /* "to EOF" == unbounded */

struct file_lock {
    bool     used;
    /* file identity */
    void    *mnt;
    uint64_t ino, gen;
    void    *node;         /* fallback identity when ino == 0 */
    /* owner: pid for fcntl locks, description pointer for flock locks */
    int      owner_pid;
    void    *owner_ofd;
    bool     is_flock;
    bool     exclusive;    /* F_WRLCK / LOCK_EX */
    uint64_t start, end;   /* [start, end); flock locks are [0, FL_END_MAX) */
};

static struct file_lock g_locks[FL_MAX];

/* ---- identity ---------------------------------------------------- */

struct fl_key { void *mnt; uint64_t ino, gen; void *node; };

static bool fl_file_lockable(struct file *f) {
    return f && f->kind == FILE_KIND_VFS;
}

static struct fl_key fl_key_of(struct file *f) {
    struct fl_key k;
    k.mnt  = f->vfs.mnt;
    k.ino  = f->vfs.ino;
    k.gen  = f->vfs.ino_gen;
    k.node = f->vfs.ino ? 0 : f->vfs.priv;
    return k;
}

static bool fl_same_file(const struct file_lock *l, const struct fl_key *k) {
    return l->mnt == k->mnt && l->ino == k->ino &&
           l->gen == k->gen && l->node == k->node;
}

static bool fl_overlap(const struct file_lock *l, uint64_t s, uint64_t e) {
    return l->start < e && s < l->end;
}

/* The tgid is the fcntl-lock owner so threads of one process share locks. */
static int fl_owner_pid(struct proc *p) {
    if (!p) return -1;
    return p->is_thread ? p->tgid : p->pid;
}

static struct file_lock *fl_alloc(void) {
    for (int i = 0; i < FL_MAX; i++)
        if (!g_locks[i].used) { return &g_locks[i]; }
    return 0;
}

/* ---- conflict scan ------------------------------------------------ */

/* First lock that would deny (exclusive_wanted, [s,e)) to fcntl-owner
 * `pid` (want_flock=false) or flock-owner `ofd` (want_flock=true) on file
 * `k`. The two families are fully INDEPENDENT, as on Linux: fcntl locks
 * never see flock locks and vice versa. The tempting "conservative" cross-
 * check is a trap -- a process holding flock(LOCK_EX) would then deadlock
 * against its OWN F_SETLKW, a hang Linux-correct software has every right
 * not to expect. A process (or description) never conflicts with itself. */
static struct file_lock *fl_conflict(const struct fl_key *k, bool want_flock,
                                     int pid, void *ofd, bool excl,
                                     uint64_t s, uint64_t e) {
    for (int i = 0; i < FL_MAX; i++) {
        struct file_lock *l = &g_locks[i];
        if (!l->used || l->is_flock != want_flock || !fl_same_file(l, k))
            continue;
        if (want_flock ? (l->owner_ofd == ofd) : (l->owner_pid == pid))
            continue;                               /* own lock: no conflict */
        if (!fl_overlap(l, s, e)) continue;
        if (excl || l->exclusive) return l;
    }
    return 0;
}

/* ---- fcntl record locks ------------------------------------------- */

/* Remove/trim every fcntl lock owned by (k, pid) overlapping [s,e).
 * A range punched out of a lock's middle SPLITS it; the split needs a
 * fresh slot and reports ENOLCK when the table is full (POSIX's answer). */
static int fl_carve(const struct fl_key *k, int pid, uint64_t s, uint64_t e) {
    for (int i = 0; i < FL_MAX; i++) {
        struct file_lock *l = &g_locks[i];
        if (!l->used || l->is_flock || l->owner_pid != pid ||
            !fl_same_file(l, k) || !fl_overlap(l, s, e))
            continue;
        bool head = l->start < s;         /* piece below the carve survives */
        bool tail = l->end   > e;         /* piece above survives */
        if (head && tail) {
            struct file_lock *nl = fl_alloc();
            if (!nl) return -1;           /* ENOLCK: cannot represent split */
            *nl = *l;
            nl->start = e;                /* upper remainder */
            nl->used  = true;
            l->end = s;                   /* lower remainder */
        } else if (head) {
            l->end = s;
        } else if (tail) {
            l->start = e;
        } else {
            l->used = false;              /* fully covered */
        }
    }
    return 0;
}

long fl_fcntl(struct file *f, struct proc *p, int cmd, struct lx_flock *fl) {
    if (!fl_file_lockable(f)) return 0;   /* non-VFS: keep the old tolerance */
    struct fl_key k = fl_key_of(f);
    int pid = fl_owner_pid(p);

    /* Resolve l_whence/l_start/l_len to an absolute [s,e). */
    int64_t base = 0;
    switch (fl->l_whence) {
    case 0: base = 0; break;                       /* SEEK_SET */
    case 1: base = (int64_t)f->vfs.pos; break;     /* SEEK_CUR */
    case 2: base = (int64_t)f->vfs.size; break;    /* SEEK_END */
    default: return -ABI_EINVAL;
    }
    int64_t st = base + fl->l_start;
    int64_t ln = fl->l_len;
    if (ln < 0) { st += ln; ln = -ln; }            /* negative len: below start */
    if (st < 0) return -ABI_EINVAL;
    uint64_t s = (uint64_t)st;
    uint64_t e = ln == 0 ? FL_END_MAX : s + (uint64_t)ln;

    bool excl;
    switch (fl->l_type) {
    case 0: excl = false; break;                   /* F_RDLCK */
    case 1: excl = true;  break;                   /* F_WRLCK */
    case 2:                                        /* F_UNLCK */
        if (cmd == 5 /* F_GETLK */) return -ABI_EINVAL;
        return fl_carve(&k, pid, s, e) ? -ABI_ENOLCK : 0;
    default: return -ABI_EINVAL;
    }

    if (cmd == 5 /* F_GETLK */) {
        struct file_lock *c = fl_conflict(&k, false, pid, 0, excl, s, e);
        if (!c) { fl->l_type = 2 /* F_UNLCK */; return 0; }
        fl->l_type   = c->exclusive ? 1 : 0;
        fl->l_whence = 0;
        fl->l_start  = (int64_t)c->start;
        fl->l_len    = c->end == FL_END_MAX ? 0 : (int64_t)(c->end - c->start);
        fl->l_pid    = c->is_flock ? -1 : c->owner_pid;
        return 0;
    }

    /* F_SETLK (6) / F_SETLKW (7): acquire, replacing our own overlaps. */
    for (;;) {
        struct file_lock *c = fl_conflict(&k, false, pid, 0, excl, s, e);
        if (!c) break;
        if (cmd == 6 /* F_SETLK */) return -ABI_EAGAIN;
        if (p && p->pending_signals) return -ABI_EINTR;
        sched_yield();                     /* cooperative F_SETLKW wait */
    }
    if (fl_carve(&k, pid, s, e)) return -ABI_ENOLCK;
    struct file_lock *nl = fl_alloc();
    if (!nl) return -ABI_ENOLCK;
    nl->mnt = k.mnt; nl->ino = k.ino; nl->gen = k.gen; nl->node = k.node;
    nl->owner_pid = pid; nl->owner_ofd = 0;
    nl->is_flock = false; nl->exclusive = excl;
    nl->start = s; nl->end = e;
    nl->used = true;
    return 0;
}

/* ---- flock(2) ------------------------------------------------------ */

long fl_flock(struct file *f, struct proc *p, int op) {
    if (!fl_file_lockable(f)) return 0;   /* pipes &c: keep old tolerance */
    struct fl_key k = fl_key_of(f);
    void *ofd = f->vfs_refs ? (void *)f->vfs_refs : (void *)f;
    bool nb = (op & 4 /* LOCK_NB */) != 0;
    int  kind = op & ~4;

    if (kind == 8 /* LOCK_UN */) {
        for (int i = 0; i < FL_MAX; i++)
            if (g_locks[i].used && g_locks[i].is_flock &&
                g_locks[i].owner_ofd == ofd && fl_same_file(&g_locks[i], &k))
                g_locks[i].used = false;
        return 0;
    }
    if (kind != 1 /* LOCK_SH */ && kind != 2 /* LOCK_EX */)
        return -ABI_EINVAL;
    bool excl = (kind == 2);

    for (;;) {
        struct file_lock *c = fl_conflict(&k, true, -1, ofd, excl, 0, FL_END_MAX);
        if (!c) break;
        if (nb) return -ABI_EAGAIN;
        if (p && p->pending_signals) return -ABI_EINTR;
        sched_yield();
    }
    /* Upgrade/downgrade in place if this description already holds one. */
    for (int i = 0; i < FL_MAX; i++) {
        struct file_lock *l = &g_locks[i];
        if (l->used && l->is_flock && l->owner_ofd == ofd &&
            fl_same_file(l, &k)) {
            l->exclusive = excl;
            return 0;
        }
    }
    struct file_lock *nl = fl_alloc();
    if (!nl) return -ABI_ENOLCK;
    nl->mnt = k.mnt; nl->ino = k.ino; nl->gen = k.gen; nl->node = k.node;
    nl->owner_pid = fl_owner_pid(p); nl->owner_ofd = ofd;
    nl->is_flock = true; nl->exclusive = excl;
    nl->start = 0; nl->end = FL_END_MAX;
    nl->used = true;
    return 0;
}

/* ---- teardown ------------------------------------------------------ */

/* POSIX: closing ANY descriptor for the file drops the process's record
 * locks on it. Called from close with the file still open. flock locks are
 * NOT dropped here -- theirs is the description's lifetime (below). */
void fl_release_close(struct file *f, struct proc *p) {
    if (!fl_file_lockable(f)) return;
    struct fl_key k = fl_key_of(f);
    int pid = fl_owner_pid(p);
    for (int i = 0; i < FL_MAX; i++)
        if (g_locks[i].used && !g_locks[i].is_flock &&
            g_locks[i].owner_pid == pid && fl_same_file(&g_locks[i], &k))
            g_locks[i].used = false;
}

/* The open file description died (last dup/fork reference closed). */
void fl_release_ofd(void *ofd) {
    if (!ofd) return;
    for (int i = 0; i < FL_MAX; i++)
        if (g_locks[i].used && g_locks[i].is_flock &&
            g_locks[i].owner_ofd == ofd)
            g_locks[i].used = false;
}

/* Process exit: everything it owned goes. Descriptions it held references
 * to are released by the per-fd close path (fl_release_ofd), so only the
 * pid-owned record locks need sweeping here. */
void fl_release_proc(struct proc *p) {
    int pid = fl_owner_pid(p);
    if (pid < 0) return;
    for (int i = 0; i < FL_MAX; i++)
        if (g_locks[i].used && !g_locks[i].is_flock &&
            g_locks[i].owner_pid == pid)
            g_locks[i].used = false;
}
