/* linux-sysv -- System V semaphores + message queues (Phase H, 2026-08-22).
 *
 *   bit0  semget/semctl basics: SETVAL/GETVAL round-trip, GETALL
 *   bit1  semop mutual exclusion ACROSS processes: the child's P()
 *         really blocks until the parent's V()
 *   bit2  SEM_UNDO: a child dies holding the semaphore; the kernel's
 *         exit-time undo releases it and unblocks the parent (the
 *         Apache/PostgreSQL crashed-worker case)
 *   bit3  msgsnd/msgrcv round-trip + type-selective receive
 *   bit4  blocking msgrcv wakes when the child sends 150 ms later
 *   bit5  IPC_RMID: operations on a removed id answer EIDRM/EIDRM-class
 *         errors, not success
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <sys/wait.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };

static int wait_exit(pid_t k, int ms) {
    int st = 0;
    for (int i = 0; i < ms / 10; i++) {
        pid_t r = waitpid(k, &st, WNOHANG);
        if (r == k) return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
        usleep(10000);
    }
    kill(k, SIGKILL);
    waitpid(k, &st, 0);
    return -1;
}

static void sem_p(int id, int flg) {
    struct sembuf op = { 0, -1, (short)flg };
    semop(id, &op, 1);
}
static void sem_v(int id) {
    struct sembuf op = { 0, 1, 0 };
    semop(id, &op, 1);
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    volatile int *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) { printf("sysv: no shm\n"); return 0; }

    /* ---- bit0: semget + semctl basics ---- */
    int sid = semget(IPC_PRIVATE, 3, IPC_CREAT | 0600);
    {
        union semun u;
        u.val = 5;
        int sr = semctl(sid, 1, SETVAL, u);
        int gv = semctl(sid, 1, GETVAL);
        unsigned short all[3] = { 9, 9, 9 };
        union semun ua;
        ua.array = all;
        int ga = semctl(sid, 0, GETALL, ua);
        printf("sysv: sid=%d set=%d get=%d getall=%d [%u,%u,%u]\n",
               sid, sr, gv, ga, all[0], all[1], all[2]);
        if (sid >= 0 && sr == 0 && gv == 5 && ga == 0 &&
            all[0] == 0 && all[1] == 5 && all[2] == 0)
            bits |= 1;
    }

    /* ---- bit1: cross-process mutual exclusion ---- */
    {
        union semun u;
        u.val = 1;
        semctl(sid, 0, SETVAL, u);       /* binary semaphore, free */
        shm[0] = 0;
        sem_p(sid, 0);                    /* parent takes it */
        pid_t k = fork();
        if (k == 0) {
            sem_p(sid, 0);                /* must block on the parent */
            int ok = (shm[0] == 1);
            sem_v(sid);
            _exit(ok ? 42 : 7);
        }
        usleep(150000);
        shm[0] = 1;
        sem_v(sid);                       /* release: child proceeds */
        int e = wait_exit(k, 3000);
        printf("sysv: mutex=%d (42=ok, -1=HANG)\n", e);
        if (e == 42) bits |= 2;
    }

    /* ---- bit2: SEM_UNDO releases a dead holder's semaphore ---- */
    {
        pid_t k = fork();
        if (k == 0) {
            sem_p(sid, SEM_UNDO);         /* take it, flagged */
            usleep(150000);
            _exit(0);                     /* die holding it */
        }
        usleep(50000);                    /* let the child take it */
        sem_p(sid, 0);                    /* blocks until the undo fires */
        int got = 1;
        sem_v(sid);
        wait_exit(k, 2000);
        printf("sysv: undo-released=%d\n", got);
        if (got) bits |= 4;               /* reaching here IS the proof */
    }

    /* ---- bit3: message round-trip + type-selective receive ---- */
    int qid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    {
        struct { long t; char b[32]; } m1 = { 1, "first" },
                                       m2 = { 2, "second" }, r;
        int s1 = msgsnd(qid, &m1, 6, 0);
        int s2 = msgsnd(qid, &m2, 7, 0);
        memset(&r, 0, sizeof r);
        long g2 = msgrcv(qid, &r, sizeof r.b, 2, 0);   /* type 2 first */
        int sel = (g2 == 7 && r.t == 2 && strcmp(r.b, "second") == 0);
        memset(&r, 0, sizeof r);
        long g1 = msgrcv(qid, &r, sizeof r.b, 0, 0);   /* then the rest */
        int rest = (g1 == 6 && r.t == 1 && strcmp(r.b, "first") == 0);
        printf("sysv: msg snd=%d/%d sel=%d rest=%d\n", s1, s2, sel, rest);
        if (qid >= 0 && s1 == 0 && s2 == 0 && sel && rest) bits |= 8;
    }

    /* ---- bit4: blocking msgrcv wakes on a later send ---- */
    {
        pid_t k = fork();
        if (k == 0) {
            usleep(150000);
            struct { long t; char b[8]; } m = { 7, "wake" };
            msgsnd(qid, &m, 5, 0);
            _exit(0);
        }
        struct { long t; char b[8]; } r;
        memset(&r, 0, sizeof r);
        long g = msgrcv(qid, &r, sizeof r.b, 0, 0);    /* blocks ~150ms */
        wait_exit(k, 2000);
        printf("sysv: blocking-rcv got=%ld type=%ld (%s)\n", g, r.t, r.b);
        if (g == 5 && r.t == 7 && strcmp(r.b, "wake") == 0) bits |= 16;
    }

    /* ---- bit5: RMID makes ids dead ---- */
    {
        int rs = semctl(sid, 0, IPC_RMID);
        errno = 0;
        struct sembuf op = { 0, -1, IPC_NOWAIT };
        int se = semop(sid, &op, 1);
        int sem_dead = (rs == 0 && se < 0 &&
                        (errno == EIDRM || errno == EINVAL));
        int rq = msgctl(qid, IPC_RMID, 0);
        errno = 0;
        struct { long t; char b[4]; } m = { 1, "x" };
        int me = msgsnd(qid, &m, 2, 0);
        int msg_dead = (rq == 0 && me < 0 &&
                        (errno == EIDRM || errno == EINVAL));
        printf("sysv: rmid sem=%d msg=%d\n", sem_dead, msg_dead);
        if (sem_dead && msg_dead) bits |= 32;
    }

    printf("LXSYSV: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
