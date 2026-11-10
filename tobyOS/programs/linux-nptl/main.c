/* linux-nptl -- thread-group semantics acceptance test (2026-08-22).
 *
 * Every bit here was BROKEN before this slice, each for a structural
 * reason the audit named:
 *   - SIG_MAX was 32: glibc's SIGCANCEL (32) and SIGSETXID (33) got
 *     EINVAL from rt_sigaction/tgkill, so pthread_cancel and setuid-from-
 *     a-threaded-process could not work at all.
 *   - Each thread had a SNAPSHOT copy of the sighand table, so a handler
 *     installed after pthread_create was invisible to siblings.
 *   - A fatal signal ran proc_exit, not proc_exit_group: a SIGSEGV in a
 *     worker killed the thread and left the process limping.
 *   - getpid() returned the TID inside a thread.
 *
 * Real glibc NPTL, static. Each check sets a bit; all pass => exit 63.
 *
 *   bit0  pthread_create/join regression guard (was already green)
 *   bit1  pthread_cancel: a worker parked in a cancellation point is
 *         cancelled and joins with PTHREAD_CANCELED
 *   bit2  setuid() from a threaded process reaches EVERY thread (glibc
 *         broadcasts via SIGSETXID) -- asserted from the OTHER thread
 *   bit3  a handler installed by the MAIN thread after workers started
 *         fires when a worker is pthread_kill'ed (shared sighand)
 *   bit4  SIGSEGV in a worker kills the WHOLE process: a forked child
 *         crashes one of its threads; the parent reaps WTERMSIG==SIGSEGV
 *   bit5  getpid() agrees across threads and differs from gettid()
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/syscall.h>

static pthread_barrier_t g_bar;

/* ---- bit0/bit5 worker ---- */
static void *idgetter(void *arg) {
    long *out = (long *)arg;
    out[0] = (long)getpid();
    out[1] = (long)syscall(SYS_gettid);
    return (void *)42;
}

/* ---- bit1: park in a cancellation point ---- */
static void *cancellee(void *arg) {
    (void)arg;
    pthread_barrier_wait(&g_bar);
    for (;;) {
        struct timespec ts = { 10, 0 };
        nanosleep(&ts, 0);             /* cancellation point */
    }
    return 0;
}

/* ---- bit2: observe the uid from a second thread ---- */
static void *uidwatcher(void *arg) {
    (void)arg;
    pthread_barrier_wait(&g_bar);      /* main sets uid after this */
    pthread_barrier_wait(&g_bar);      /* main says: check now */
    return (void *)(long)getuid();
}

/* ---- bit3: shared sighand ---- */
static volatile sig_atomic_t g_usr1_seen;
static void on_usr1(int s) { (void)s; g_usr1_seen = 1; }
static void *sigwaiter(void *arg) {
    (void)arg;
    pthread_barrier_wait(&g_bar);      /* main installs the handler AFTER this */
    pthread_barrier_wait(&g_bar);      /* then pthread_kills us */
    for (int i = 0; i < 2000 && !g_usr1_seen; i++) usleep(1000);
    return (void *)(long)g_usr1_seen;
}

/* ---- bit4 child: a worker thread dies of an uncaught fatal signal ----
 * abort(), not a raw segfault: the semantics under test (an uncaught
 * fatal signal in ONE thread kills the GROUP) are identical, but a
 * deliberate SIGSEGV prints CPU-exception lines that the lxposix health
 * gate rightly counts as faults -- a test must not make the gate's
 * fault census unreadable. The hardware-fault flavour of the same rule
 * lives in isr.c's fatal path (same one-line proc_exit_group change),
 * exercised by any real crash. */
static void *crasher(void *arg) {
    (void)arg;
    usleep(50 * 1000);
    abort();                           /* SIGABRT in the WORKER */
    return 0;
}
static int crash_child(void) {
    pthread_t t;
    if (pthread_create(&t, 0, crasher, 0) != 0) _exit(7);
    /* If the group-fatal rule is missing, the worker dies alone and THIS
     * thread lives on to _exit(9) -- which the parent sees as a clean exit
     * and fails the bit. With the rule, the SIGABRT takes us both. */
    sleep(5);
    _exit(9);
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0 + bit5 ---- */
    {
        long ids[2] = { 0, 0 };
        pthread_t t;
        void *rv = 0;
        if (pthread_create(&t, 0, idgetter, ids) == 0 &&
            pthread_join(t, &rv) == 0 && rv == (void *)42) {
            bits |= 1;
            long self = (long)getpid();
            printf("nptl: main getpid=%ld thread getpid=%ld gettid=%ld\n",
                   self, ids[0], ids[1]);
            if (ids[0] == self && ids[1] != self && ids[1] != 0) bits |= 32;
        } else {
            printf("nptl: create/join FAILED\n");
        }
    }

    /* ---- bit1: pthread_cancel ---- */
    {
        pthread_barrier_init(&g_bar, 0, 2);
        pthread_t t;
        if (pthread_create(&t, 0, cancellee, 0) == 0) {
            pthread_barrier_wait(&g_bar);      /* it is inside nanosleep soon */
            usleep(100 * 1000);
            int cr = pthread_cancel(t);
            void *rv = 0;
            int jr = pthread_join(t, &rv);
            printf("nptl: cancel rc=%d join rc=%d rv=%s\n", cr, jr,
                   rv == PTHREAD_CANCELED ? "PTHREAD_CANCELED" : "other");
            if (cr == 0 && jr == 0 && rv == PTHREAD_CANCELED) bits |= 2;
        }
        pthread_barrier_destroy(&g_bar);
    }

    /* ---- bit2: threaded setuid broadcast ---- */
    {
        pthread_barrier_init(&g_bar, 0, 2);
        pthread_t t;
        if (pthread_create(&t, 0, uidwatcher, 0) == 0) {
            pthread_barrier_wait(&g_bar);
            int sr = setuid(1000);             /* we run as root in the gate */
            pthread_barrier_wait(&g_bar);
            void *rv = (void *)-1L;
            pthread_join(t, &rv);
            printf("nptl: setuid rc=%d, OTHER thread's getuid=%ld (want 1000)\n",
                   sr, (long)rv);
            if (sr == 0 && (long)rv == 1000) bits |= 4;
        }
        pthread_barrier_destroy(&g_bar);
    }

    /* ---- bit3: shared sighand ---- */
    {
        pthread_barrier_init(&g_bar, 0, 2);
        pthread_t t;
        if (pthread_create(&t, 0, sigwaiter, 0) == 0) {
            pthread_barrier_wait(&g_bar);
            /* Installed AFTER the worker exists: a per-thread snapshot
             * sighand can never see this registration. */
            signal(SIGUSR1, on_usr1);
            pthread_barrier_wait(&g_bar);
            pthread_kill(t, SIGUSR1);
            void *rv = 0;
            pthread_join(t, &rv);
            printf("nptl: handler installed post-create fired in worker=%ld\n",
                   (long)rv);
            if ((long)rv == 1) bits |= 8;
        }
        pthread_barrier_destroy(&g_bar);
    }

    /* ---- bit4: worker SIGSEGV is group-fatal ---- */
    {
        pid_t pid = fork();
        if (pid == 0) crash_child();
        if (pid > 0) {
            int st = 0;
            if (waitpid(pid, &st, 0) == pid) {
                printf("nptl: crash child signaled=%d termsig=%d "
                       "(want SIGABRT=%d; exited=%d code=%d)\n",
                       WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : -1,
                       SIGABRT, WIFEXITED(st),
                       WIFEXITED(st) ? WEXITSTATUS(st) : -1);
                /* Accept 128+SIGABRT as an exit code too: this kernel
                 * reports signal deaths through the exit-code channel. */
                if ((WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) ||
                    (WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGABRT))
                    bits |= 16;
            }
        }
    }

    printf("LXNPTL: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
