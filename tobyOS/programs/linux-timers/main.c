/* linux-timers -- Linux slice 3 acceptance test.
 *
 * timerfd/signalfd only earn their keep if an event loop can WAIT on them
 * through poll(2) alongside everything else, so the tests here go through
 * poll rather than just reading the fds directly. A read()-only test would
 * pass even if the poll readiness arms were missing entirely -- which is
 * exactly the failure the slice plan warned about.
 *
 * Each check sets a bit; all pass => exit 63.
 *
 *   bit0  timerfd_create + settime + gettime report a sane remaining time
 *   bit1  poll() BLOCKS on the timerfd and wakes when it expires
 *   bit2  periodic timerfd accumulates expiries across an interval
 *   bit3  signalfd becomes readable when the watched signal is raised,
 *         and read() yields the right ssi_signo
 *   bit4  rt_sigpending reports a blocked-and-raised signal
 *   bit5  alarm(1) + pause() -- the process actually sleeps and SIGALRM wakes it
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static volatile int g_alarmed = 0;
static void on_alarm(int s) { (void)s; g_alarmed = 1; }

int main(void) {
    int bits = 0;
    /* Unbuffered: stdout here is a pipe/console, so glibc block-buffers it and
     * a mid-test death would discard every diagnostic printed so far -- which
     * is exactly what happened on the first run (exit 138, zero output). */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: create + arm + read back the remaining time ---- */
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { printf("timers: timerfd_create errno=%d\n", errno); }
    else {
        struct itimerspec its;
        memset(&its, 0, sizeof its);
        its.it_value.tv_sec  = 0;
        its.it_value.tv_nsec = 200 * 1000000L;      /* 200 ms one-shot */
        if (timerfd_settime(tfd, 0, &its, NULL) == 0) {
            struct itimerspec cur;
            memset(&cur, 0, sizeof cur);
            if (timerfd_gettime(tfd, &cur) == 0) {
                long rem_ms = cur.it_value.tv_sec * 1000L
                            + cur.it_value.tv_nsec / 1000000L;
                printf("timers: armed 200ms, gettime says %ldms left\n", rem_ms);
                if (rem_ms > 0 && rem_ms <= 200) bits |= 1;
            }
        }

        /* ---- bit1: poll() must BLOCK, then report POLLIN at expiry ---- */
        struct pollfd pfd = { .fd = tfd, .events = POLLIN };
        long t0 = now_ms();
        int pr = poll(&pfd, 1, 5000);
        long waited = now_ms() - t0;
        printf("timers: poll returned %d after %ldms revents=0x%x\n",
               pr, waited, pfd.revents);
        if (pr == 1 && (pfd.revents & POLLIN)) {
            uint64_t exp = 0;
            if (read(tfd, &exp, sizeof exp) == (ssize_t)sizeof exp && exp >= 1) {
                /* It must have actually WAITED -- an immediate return would
                 * mean poll reported ready before the timer fired. */
                if (waited >= 100) bits |= 2;
                printf("timers: expirations=%llu\n", (unsigned long long)exp);
            }
        }

        /* ---- bit2: periodic timer accumulates ---- */
        memset(&its, 0, sizeof its);
        its.it_value.tv_nsec    = 50 * 1000000L;    /* first at 50ms  */
        its.it_interval.tv_nsec = 50 * 1000000L;    /* then every 50ms */
        if (timerfd_settime(tfd, 0, &its, NULL) == 0) {
            usleep(260 * 1000);                      /* ~5 intervals    */
            uint64_t exp = 0;
            if (read(tfd, &exp, sizeof exp) == (ssize_t)sizeof exp) {
                printf("timers: periodic accumulated %llu expirations\n",
                       (unsigned long long)exp);
                if (exp >= 3) bits |= 4;             /* allow scheduling slop */
            }
        }
        close(tfd);
    }

    /* ---- bit3: signalfd readable when the signal is raised ---- */
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        /* Block it first, or the default action kills us before signalfd
         * ever sees it -- this ordering IS the signalfd contract. */
        sigprocmask(SIG_BLOCK, &mask, NULL);
        int sfd = signalfd(-1, &mask, 0);
        if (sfd < 0) { printf("timers: signalfd errno=%d\n", errno); }
        else {
            raise(SIGUSR1);
            struct pollfd sp = { .fd = sfd, .events = POLLIN };
            int pr = poll(&sp, 1, 2000);
            if (pr == 1 && (sp.revents & POLLIN)) {
                struct signalfd_siginfo si;
                memset(&si, 0, sizeof si);
                if (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                    printf("timers: signalfd got signo=%u\n", si.ssi_signo);
                    if (si.ssi_signo == SIGUSR1) bits |= 8;
                }
            } else {
                printf("timers: signalfd poll rc=%d revents=0x%x\n",
                       pr, sp.revents);
            }
            close(sfd);
        }
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
    }

    /* ---- bit4: rt_sigpending sees a blocked, raised signal ---- */
    {
        sigset_t mask, pend;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR2);
        sigprocmask(SIG_BLOCK, &mask, NULL);
        raise(SIGUSR2);
        sigemptyset(&pend);
        if (sigpending(&pend) == 0) {
            int is = sigismember(&pend, SIGUSR2);
            printf("timers: sigpending reports SIGUSR2=%d\n", is);
            if (is == 1) bits |= 16;
        }
        /* Drain it so it cannot kill us on unblock. */
        signal(SIGUSR2, SIG_IGN);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
    }

    /* ---- bit5: alarm(1) + pause() actually sleeps and is woken ---- */
    {
        signal(SIGALRM, on_alarm);
        printf("timers: handler installed\n");
        long t0 = now_ms();
        unsigned prev = alarm(1);
        printf("timers: alarm(1) armed (prev=%u), calling pause()\n", prev);
        pause();                       /* must block ~1s, then return EINTR */
        long waited = now_ms() - t0;
        printf("timers: pause woke after %ldms (alarmed=%d)\n",
               waited, g_alarmed);
        /* Must have genuinely slept: an immediate return would mean pause()
         * is a no-op, which is the failure mode worth catching. */
        if (g_alarmed && waited >= 500) bits |= 32;
    }

    printf("LXTIMERS: VERDICT bits=%d (63=all)\n", bits);
    fflush(stdout);
    return bits;
}
