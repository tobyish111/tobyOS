/* sysvipc.c -- System V semaphores and message queues (Phase H, 2026-08-22).
 *
 * SysV shm has existed since the MIT-SHM slice; sem and msg fell through
 * to the ENOSYS census. Real users: PostgreSQL and Apache serialize on
 * SysV semaphore sets (with SEM_UNDO, so a crashed worker releases what
 * it held), and plenty of older Unix software ships msgsnd/msgrcv IPC.
 *
 * Blocking (semop without IPC_NOWAIT, msgsnd on a full queue, msgrcv on
 * an empty one) is the cooperative retry loop the file locks use: stay
 * RUNNING, yield, re-check, EINTR when a signal lands. All state is
 * BKL-serialised (every caller is a syscall arm).
 *
 * SEM_UNDO is real: each (set, process) pair accumulates an adjustment
 * per semaphore, applied when the process dies (sysv_release_proc from
 * the exit path) -- the entire point of the flag, and the part whose
 * absence deadlocks Apache on the first worker crash.
 *
 * Identity note: "process" here is the TGID, so threads share undo
 * state, as on Linux. */

#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/uaccess.h>
#include <tobyos/perf.h>
#include <tobyos/abi/abi.h>

/* ---- semaphores ------------------------------------------------------ */

#define SEM_MAX_SETS   32
#define SEM_MAX_NSEMS  32
#define SEM_UNDO_MAX  128

#define LX_IPC_CREAT   01000
#define LX_IPC_EXCL    02000
#define LX_IPC_NOWAIT  04000
#define LX_SEM_UNDO    0x1000

/* semctl / msgctl cmds */
#define LX_IPC_RMID 0
#define LX_IPC_SET  1
#define LX_IPC_STAT 2
#define LX_GETPID  11
#define LX_GETVAL  12
#define LX_GETALL  13
#define LX_GETNCNT 14
#define LX_GETZCNT 15
#define LX_SETVAL  16
#define LX_SETALL  17

struct sem_set {
    bool     used;
    int      key;          /* 0 = IPC_PRIVATE (never matched) */
    uint32_t seq;          /* stamped into the id so stale ids miss */
    int      nsems;
    uint16_t vals[SEM_MAX_NSEMS];
    int      lastpid;
};

static struct sem_set g_sems[SEM_MAX_SETS];
static uint32_t g_sem_seq = 1;

struct sem_undo {
    bool used;
    int  slot;             /* index into g_sems */
    uint32_t seq;
    int  tgid;
    int16_t adj[SEM_MAX_NSEMS];
};
static struct sem_undo g_undo[SEM_UNDO_MAX];

static int cur_tgid(void) {
    struct proc *p = current_proc();
    if (!p) return 0;
    return p->is_thread ? p->tgid : p->pid;
}

static int sem_id_of(int slot) { return (int)(g_sems[slot].seq << 5) | slot; }

static struct sem_set *sem_by_id(int id, int *slot_out) {
    int slot = id & 31;
    if (slot < 0 || slot >= SEM_MAX_SETS) return 0;
    struct sem_set *s = &g_sems[slot];
    if (!s->used || sem_id_of(slot) != id) return 0;
    if (slot_out) *slot_out = slot;
    return s;
}

long sysv_semget(int key, int nsems, int flags) {
    if (nsems < 0 || nsems > SEM_MAX_NSEMS) return -ABI_EINVAL;
    if (key != 0) {
        for (int i = 0; i < SEM_MAX_SETS; i++)
            if (g_sems[i].used && g_sems[i].key == key) {
                if ((flags & LX_IPC_CREAT) && (flags & LX_IPC_EXCL))
                    return -ABI_EEXIST;
                if (nsems > g_sems[i].nsems) return -ABI_EINVAL;
                return sem_id_of(i);
            }
        if (!(flags & LX_IPC_CREAT)) return -ABI_ENOENT;
    }
    if (nsems == 0) return -ABI_EINVAL;      /* creating needs a size */
    for (int i = 0; i < SEM_MAX_SETS; i++) {
        if (g_sems[i].used) continue;
        memset(&g_sems[i], 0, sizeof g_sems[i]);
        g_sems[i].used  = true;
        g_sems[i].key   = key;
        g_sems[i].seq   = g_sem_seq++;
        g_sems[i].nsems = nsems;
        return sem_id_of(i);
    }
    return -ABI_ENOSPC;
}

static struct sem_undo *undo_of(int slot, int tgid, bool create) {
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        struct sem_undo *u = &g_undo[i];
        if (u->used && u->slot == slot && u->tgid == tgid &&
            u->seq == g_sems[slot].seq)
            return u;
    }
    if (!create) return 0;
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        if (g_undo[i].used) continue;
        memset(&g_undo[i], 0, sizeof g_undo[i]);
        g_undo[i].used = true;
        g_undo[i].slot = slot;
        g_undo[i].seq  = g_sems[slot].seq;
        g_undo[i].tgid = tgid;
        return &g_undo[i];
    }
    return 0;
}

struct lx_sembuf { uint16_t num; int16_t op; int16_t flg; };

/* deadline_ns: perf_now_ns() deadline for semtimedop, 0 = block forever. */
long sysv_semop(int id, uint64_t usops, int nops, uint64_t deadline_ns) {
    if (nops <= 0 || nops > 16) return -ABI_EINVAL;
    struct lx_sembuf ops[16];
    if (copy_from_user(ops, (const void *)(uintptr_t)usops,
                       (size_t)nops * sizeof ops[0]) != 0)
        return -ABI_EFAULT;
    struct proc *p = current_proc();
    int tgid = cur_tgid();

    for (;;) {
        int slot;
        struct sem_set *s = sem_by_id(id, &slot);
        if (!s) return -ABI_EIDRM;       /* removed (or never existed) */

        /* Trial pass: does the whole array apply atomically right now? */
        bool would_block = false;
        int32_t trial[SEM_MAX_NSEMS];
        for (int i = 0; i < s->nsems; i++) trial[i] = s->vals[i];
        for (int i = 0; i < nops && !would_block; i++) {
            if (ops[i].num >= s->nsems) return -ABI_EINVAL;
            int32_t v = trial[ops[i].num] + ops[i].op;
            if (ops[i].op == 0) {        /* wait-for-zero */
                if (trial[ops[i].num] != 0) would_block = true;
            } else if (v < 0) {
                would_block = true;
            } else {
                trial[ops[i].num] = v;
            }
        }
        if (!would_block) {
            for (int i = 0; i < s->nsems; i++) s->vals[i] = (uint16_t)trial[i];
            for (int i = 0; i < nops; i++) {
                if (!(ops[i].flg & LX_SEM_UNDO) || ops[i].op == 0) continue;
                struct sem_undo *u = undo_of(slot, tgid, true);
                if (u) u->adj[ops[i].num] =
                    (int16_t)(u->adj[ops[i].num] - ops[i].op);
            }
            s->lastpid = tgid;
            return 0;
        }
        /* Any op that would block carries the whole call's flags check. */
        bool nowait = false;
        for (int i = 0; i < nops; i++)
            if (ops[i].flg & LX_IPC_NOWAIT) nowait = true;
        if (nowait) return -ABI_EAGAIN;
        if (deadline_ns && perf_now_ns() >= deadline_ns)
            return -ABI_EAGAIN;          /* semtimedop timeout, per Linux */
        if (p && p->pending_signals) return -ABI_EINTR;
        sched_yield();
    }
}

long sysv_semctl(int id, int semnum, int cmd, uint64_t arg) {
    int slot;
    struct sem_set *s = sem_by_id(id, &slot);
    if (!s) return -ABI_EIDRM;
    switch (cmd) {
    case LX_IPC_RMID:
        s->used = false;                 /* waiters see EIDRM on re-check */
        return 0;
    case LX_GETVAL:
        if (semnum < 0 || semnum >= s->nsems) return -ABI_EINVAL;
        return s->vals[semnum];
    case LX_SETVAL:
        if (semnum < 0 || semnum >= s->nsems) return -ABI_EINVAL;
        if ((int)arg < 0 || arg > 32767) return -ABI_ERANGE;
        s->vals[semnum] = (uint16_t)arg;
        return 0;
    case LX_GETPID:
        return s->lastpid;
    case LX_GETNCNT:
    case LX_GETZCNT:
        return 0;                        /* cooperative waiters: uncounted */
    case LX_GETALL: {
        uint16_t out[SEM_MAX_NSEMS];
        for (int i = 0; i < s->nsems; i++) out[i] = s->vals[i];
        if (copy_to_user((void *)(uintptr_t)arg, out,
                         (size_t)s->nsems * 2) != 0)
            return -ABI_EFAULT;
        return 0;
    }
    case LX_SETALL: {
        uint16_t in[SEM_MAX_NSEMS];
        if (copy_from_user(in, (const void *)(uintptr_t)arg,
                           (size_t)s->nsems * 2) != 0)
            return -ABI_EFAULT;
        for (int i = 0; i < s->nsems; i++) s->vals[i] = in[i];
        return 0;
    }
    case LX_IPC_STAT: {
        /* struct semid_ds, x86-64: ipc_perm(48) + otime(8) + pad + ctime(8)
         * + pad + nsems(8) + reserved. Zero except nsems -- callers that
         * read anything else get honest zeros, not stack garbage. */
        uint8_t ds[104];
        memset(ds, 0, sizeof ds);
        uint64_t nsems = (uint64_t)s->nsems;
        memcpy(ds + 72, &nsems, 8);
        if (copy_to_user((void *)(uintptr_t)arg, ds, sizeof ds) != 0)
            return -ABI_EFAULT;
        return 0;
    }
    case LX_IPC_SET:
        return 0;                        /* single-user: perms are a no-op */
    }
    return -ABI_EINVAL;
}

/* ---- message queues -------------------------------------------------- */

#define MSG_MAX_Q      32
#define MSG_MAX_BYTES  16384    /* per queue (Linux MSGMNB default) */
#define MSG_MAX_ONE     8192    /* per message (Linux MSGMAX default) */

struct msg_msg {
    struct msg_msg *next;
    long   mtype;
    size_t len;
    /* payload follows */
};

struct msg_q {
    bool     used;
    int      key;
    uint32_t seq;
    size_t   bytes;
    struct msg_msg *head, *tail;
};

static struct msg_q g_msgq[MSG_MAX_Q];
static uint32_t g_msg_seq = 1;

static int msg_id_of(int slot) { return (int)(g_msgq[slot].seq << 5) | slot; }

static struct msg_q *msg_by_id(int id) {
    int slot = id & 31;
    if (slot < 0 || slot >= MSG_MAX_Q) return 0;
    struct msg_q *q = &g_msgq[slot];
    if (!q->used || msg_id_of(slot) != id) return 0;
    return q;
}

static void msg_free_all(struct msg_q *q) {
    struct msg_msg *m = q->head;
    while (m) { struct msg_msg *n = m->next; kfree(m); m = n; }
    q->head = q->tail = 0;
    q->bytes = 0;
}

long sysv_msgget(int key, int flags) {
    if (key != 0) {
        for (int i = 0; i < MSG_MAX_Q; i++)
            if (g_msgq[i].used && g_msgq[i].key == key) {
                if ((flags & LX_IPC_CREAT) && (flags & LX_IPC_EXCL))
                    return -ABI_EEXIST;
                return msg_id_of(i);
            }
        if (!(flags & LX_IPC_CREAT)) return -ABI_ENOENT;
    }
    for (int i = 0; i < MSG_MAX_Q; i++) {
        if (g_msgq[i].used) continue;
        memset(&g_msgq[i], 0, sizeof g_msgq[i]);
        g_msgq[i].used = true;
        g_msgq[i].key  = key;
        g_msgq[i].seq  = g_msg_seq++;
        return msg_id_of(i);
    }
    return -ABI_ENOSPC;
}

long sysv_msgsnd(int id, uint64_t umsgp, size_t msgsz, int flags) {
    if (msgsz > MSG_MAX_ONE) return -ABI_EINVAL;
    long mtype;
    if (copy_from_user(&mtype, (const void *)(uintptr_t)umsgp, 8) != 0)
        return -ABI_EFAULT;
    if (mtype <= 0) return -ABI_EINVAL;
    struct proc *p = current_proc();
    for (;;) {
        struct msg_q *q = msg_by_id(id);
        if (!q) return -ABI_EIDRM;
        if (q->bytes + msgsz <= MSG_MAX_BYTES) {
            struct msg_msg *m =
                (struct msg_msg *)kmalloc(sizeof(*m) + msgsz);
            if (!m) return -ABI_ENOMEM;
            m->next  = 0;
            m->mtype = mtype;
            m->len   = msgsz;
            if (msgsz &&
                copy_from_user(m + 1, (const void *)(uintptr_t)(umsgp + 8),
                               msgsz) != 0) {
                kfree(m);
                return -ABI_EFAULT;
            }
            if (q->tail) q->tail->next = m; else q->head = m;
            q->tail = m;
            q->bytes += msgsz;
            return 0;
        }
        if (flags & LX_IPC_NOWAIT) return -ABI_EAGAIN;
        if (p && p->pending_signals) return -ABI_EINTR;
        sched_yield();
    }
}

#define LX_MSG_NOERROR 010000

long sysv_msgrcv(int id, uint64_t umsgp, size_t msgsz, long msgtyp,
                 int flags) {
    struct proc *p = current_proc();
    for (;;) {
        struct msg_q *q = msg_by_id(id);
        if (!q) return -ABI_EIDRM;
        /* Select per Linux: 0 = first; >0 = first of that type; <0 =
         * lowest type <= |msgtyp|. */
        struct msg_msg **pp = 0;
        if (msgtyp == 0) {
            if (q->head) pp = &q->head;
        } else if (msgtyp > 0) {
            for (struct msg_msg **it = &q->head; *it; it = &(*it)->next)
                if ((*it)->mtype == msgtyp) { pp = it; break; }
        } else {
            long best = 0;
            for (struct msg_msg **it = &q->head; *it; it = &(*it)->next)
                if ((*it)->mtype <= -msgtyp &&
                    (!pp || (*it)->mtype < best)) {
                    pp = it;
                    best = (*it)->mtype;
                }
        }
        if (pp) {
            struct msg_msg *m = *pp;
            if (m->len > msgsz && !(flags & LX_MSG_NOERROR))
                return -ABI_E2BIG;
            size_t give = m->len < msgsz ? m->len : msgsz;
            if (copy_to_user((void *)(uintptr_t)umsgp, &m->mtype, 8) != 0)
                return -ABI_EFAULT;
            if (give &&
                copy_to_user((void *)(uintptr_t)(umsgp + 8), m + 1,
                             give) != 0)
                return -ABI_EFAULT;
            *pp = m->next;
            if (q->tail == m) {
                q->tail = 0;
                for (struct msg_msg *it = q->head; it; it = it->next)
                    q->tail = it;
            }
            q->bytes -= m->len;
            kfree(m);
            return (long)give;
        }
        if (flags & LX_IPC_NOWAIT) return -ABI_ENOMSG;
        if (p && p->pending_signals) return -ABI_EINTR;
        sched_yield();
    }
}

long sysv_msgctl(int id, int cmd, uint64_t arg) {
    struct msg_q *q = msg_by_id(id);
    if (!q) return -ABI_EIDRM;
    switch (cmd) {
    case LX_IPC_RMID:
        msg_free_all(q);
        q->used = false;
        return 0;
    case LX_IPC_STAT: {
        /* struct msqid_ds: zero except msg_qnum (+80) and msg_qbytes (+88)
         * -- the two fields ipcs and health checks actually read. */
        uint8_t ds[120];
        memset(ds, 0, sizeof ds);
        uint64_t qnum = 0;
        for (struct msg_msg *m = q->head; m; m = m->next) qnum++;
        uint64_t qbytes = MSG_MAX_BYTES;
        memcpy(ds + 80, &qnum, 8);
        memcpy(ds + 88, &qbytes, 8);
        if (copy_to_user((void *)(uintptr_t)arg, ds, sizeof ds) != 0)
            return -ABI_EFAULT;
        return 0;
    }
    case LX_IPC_SET:
        return 0;
    }
    return -ABI_EINVAL;
}

/* ---- exit hook ------------------------------------------------------- */

/* Apply this process's SEM_UNDO adjustments and drop its undo records.
 * Called once per dying PROCESS (tgid) from the exit path -- the reason
 * SEM_UNDO exists: a crashed holder must release what it held or every
 * sibling deadlocks on a semaphore nobody owns. */
void sysv_release_proc(int tgid) {
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        struct sem_undo *u = &g_undo[i];
        if (!u->used || u->tgid != tgid) continue;
        struct sem_set *s = &g_sems[u->slot];
        if (s->used && s->seq == u->seq) {
            for (int j = 0; j < s->nsems; j++) {
                int32_t v = (int32_t)s->vals[j] + u->adj[j];
                if (v < 0) v = 0;
                if (v > 32767) v = 32767;
                s->vals[j] = (uint16_t)v;
            }
        }
        u->used = false;
    }
}
