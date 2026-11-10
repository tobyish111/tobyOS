/* linux-thread2 -- exec de_thread, PROCESS_SHARED futexes, robust mutexes
 * (Phase F, 2026-08-22).
 *
 *   bit0  execve from a leader with live threads: the exec'd image sees
 *         Threads: 1 (siblings really die at the point of no return)
 *   bit1  a PTHREAD_PROCESS_SHARED mutex in MAP_SHARED memory serializes
 *         parent and child -- cross-process futex WAIT/WAKE actually meet
 *         (pre-fix this HANGS: the word is shared but the futex keys were
 *         (cr3,vaddr), so the child slept on a key nobody ever woke)
 *   bit2  robust+pshared: parent already BLOCKED in lock when the owner
 *         child dies -> woken with EOWNERDEAD; consistent+unlock recovers
 *   bit3  robust private: a sibling thread dies holding one -> EOWNERDEAD
 *   bit4  get_robust_list returns the head glibc registered (len 24)
 *   bit5  lock-after-death: owner died with NO waiter; the exit-time
 *         FUTEX_OWNER_DIED stamp alone must deliver EOWNERDEAD later
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>

static int wait_exit(pid_t k, int ms) {
    int st = 0;
    for (int i = 0; i < ms / 10; i++) {
        pid_t r = waitpid(k, &st, WNOHANG);
        if (r == k) return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
        usleep(10000);
    }
    kill(k, SIGKILL);
    waitpid(k, &st, 0);
    return -1;                       /* timed out: the pre-fix hang shape */
}

static void *spin_forever(void *a) {
    (void)a;
    for (;;) usleep(10000);
    return 0;
}

static int count_threads(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[128];
    int n = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "Threads: %d", &n) == 1) break;
    fclose(f);
    return n;
}

static void *hold_and_die(void *m) {
    pthread_mutex_lock((pthread_mutex_t *)m);
    return 0;                        /* thread exits still holding it */
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "execkid") == 0)
        return count_threads() == 1 ? 42 : 7;

    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: exec kills sibling threads ---- */
    {
        pid_t k = fork();
        if (k == 0) {
            pthread_t t;
            pthread_create(&t, 0, spin_forever, 0);
            usleep(50000);           /* let it reach its loop */
            char *kargv[] = { (char *)"/bin/linux-thread2",
                              (char *)"execkid", NULL };
            execv("/bin/linux-thread2", kargv);
            _exit(9);
        }
        int e = wait_exit(k, 3000);
        printf("t2: exec-de-thread=%d (42=ok)\n", e);
        if (e == 42) bits |= 1;
    }

    /* ---- shared page: mutexes + markers ---- */
    unsigned char *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) { printf("t2: mmap shared failed\n"); return bits; }
    pthread_mutex_t *m1 = (pthread_mutex_t *)(shm + 0);      /* pshared */
    pthread_mutex_t *m2 = (pthread_mutex_t *)(shm + 128);    /* robust  */
    pthread_mutex_t *m3 = (pthread_mutex_t *)(shm + 256);    /* robust  */
    volatile int *marker  = (volatile int *)(shm + 512);
    volatile int *marker2 = (volatile int *)(shm + 516);
    volatile int *marker3 = (volatile int *)(shm + 520);

    /* Diagnostic: is the anon-MAP_SHARED page itself fork-coherent? A
     * child write must be visible to the parent after waitpid. */
    {
        volatile int *probe = (volatile int *)(shm + 1024);
        *probe = 7;
        pid_t k = fork();
        if (k == 0) { *probe = 9; _exit(0); }
        wait_exit(k, 2000);
        printf("t2: shm-probe=%d (9=fork-coherent, 7=DIVERGED)\n", *probe);
    }

    pthread_mutexattr_t ap, ar;
    pthread_mutexattr_init(&ap);
    pthread_mutexattr_setpshared(&ap, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_init(&ar);
    pthread_mutexattr_setpshared(&ar, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ar, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(m1, &ap);
    pthread_mutex_init(m2, &ar);
    pthread_mutex_init(m3, &ar);

    /* ---- bit1: pshared mutex serializes across fork ---- */
    {
        pthread_mutex_lock(m1);
        pid_t k = fork();
        if (k == 0) {
            pthread_mutex_lock(m1);              /* must block on parent */
            int ok = (*marker == 1);
            pthread_mutex_unlock(m1);
            _exit(ok ? 42 : 7);
        }
        usleep(150000);
        *marker = 1;
        pthread_mutex_unlock(m1);
        int e = wait_exit(k, 3000);
        printf("t2: pshared-serialize=%d (42=ok, -1=HANG)\n", e);
        if (e == 42) bits |= 2;
    }

    /* ---- bit2: robust EOWNERDEAD with the survivor already blocked ---- */
    {
        pid_t k = fork();
        if (k == 0) {
            pthread_mutex_lock(m2);
            *marker2 = 1;
            usleep(250000);          /* parent is blocked in lock by now */
            _exit(0);                /* die holding it: kernel must stamp+wake */
        }
        for (int i = 0; i < 300 && !*marker2; i++) usleep(10000);
        int lr = pthread_mutex_lock(m2);
        int cr = -1, ur = -1, rr = -1;
        if (lr == EOWNERDEAD) {
            cr = pthread_mutex_consistent(m2);
            ur = pthread_mutex_unlock(m2);
            rr = pthread_mutex_lock(m2);
            if (rr == 0) pthread_mutex_unlock(m2);
        }
        wait_exit(k, 2000);
        printf("t2: robust-blocked lr=%d(EOWNERDEAD=%d) cons=%d unl=%d "
               "relock=%d\n", lr, EOWNERDEAD, cr, ur, rr);
        if (lr == EOWNERDEAD && cr == 0 && ur == 0 && rr == 0) bits |= 4;
    }

    /* ---- bit3: robust PRIVATE mutex, owner is a dying sibling thread ---- */
    {
        static pthread_mutex_t mr;
        pthread_mutexattr_t arp;
        pthread_mutexattr_init(&arp);
        pthread_mutexattr_setrobust(&arp, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(&mr, &arp);
        pthread_t t;
        pthread_create(&t, 0, hold_and_die, &mr);
        pthread_join(t, 0);
        int lr = pthread_mutex_lock(&mr);
        if (lr == EOWNERDEAD) {
            pthread_mutex_consistent(&mr);
            pthread_mutex_unlock(&mr);
        }
        printf("t2: robust-thread lr=%d (EOWNERDEAD=%d)\n", lr, EOWNERDEAD);
        if (lr == EOWNERDEAD) bits |= 8;
    }

    /* ---- bit4: get_robust_list reads back glibc's registration ---- */
    {
        void *head = 0;
        size_t len = 0;
        long r = syscall(274 /* get_robust_list */, 0, &head, &len);
        printf("t2: get_robust_list r=%ld head=%p len=%zu\n", r, head, len);
        if (r == 0 && head != 0 && len == 24) bits |= 16;
    }

    /* ---- bit5: lock-after-death (stamp alone, no waiter to wake) ---- */
    {
        pid_t k = fork();
        if (k == 0) {
            pthread_mutex_lock(m3);
            *marker3 = 1;
            _exit(0);
        }
        int e = wait_exit(k, 2000);      /* owner fully dead first */
        int lr = (*marker3 == 1 && e == 0) ? pthread_mutex_lock(m3) : -1;
        if (lr == EOWNERDEAD) {
            pthread_mutex_consistent(m3);
            pthread_mutex_unlock(m3);
        }
        printf("t2: robust-postmortem lr=%d (EOWNERDEAD=%d)\n",
               lr, EOWNERDEAD);
        if (lr == EOWNERDEAD) bits |= 32;
    }

    printf("LXTHREAD2: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
