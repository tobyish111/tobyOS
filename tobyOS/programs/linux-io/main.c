/* linux-io -- POSIX timers + the kernel copy family (2026-08-22).
 *
 *   bit0  timer_create + timer_settime(100 ms one-shot) delivers the
 *         chosen signal (librt had no syscalls to land on before)
 *   bit1  a 50 ms periodic timer fires repeatedly; timer_delete stops it
 *   bit2  timer_gettime reports a sane, nonzero remaining right after arm
 *   bit3  copy_file_range moves exact bytes with exact content
 *         (coreutils 9 cp and Go call it unconditionally)
 *   bit4  sendfile file->file is byte-exact (it was an honest ENOSYS)
 *   bit5  splice file->pipe->file round-trips content exactly
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
#include <time.h>
#include <sys/sendfile.h>

static volatile sig_atomic_t g_fires;
static void on_tick(int s) { (void)s; g_fires++; }

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGUSR1, on_tick);

    /* ---- bit0 + bit2: one-shot fires; gettime sane ---- */
    {
        timer_t t;
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo  = SIGUSR1;
        int cr = timer_create(CLOCK_MONOTONIC, &sev, &t);
        struct itimerspec its;
        memset(&its, 0, sizeof its);
        its.it_value.tv_nsec = 100 * 1000000L;      /* 100 ms one-shot */
        int sr = cr == 0 ? timer_settime(t, 0, &its, 0) : -1;
        struct itimerspec cur;
        memset(&cur, 0, sizeof cur);
        int gr = sr == 0 ? timer_gettime(t, &cur) : -1;
        long rem_ms = cur.it_value.tv_sec * 1000 + cur.it_value.tv_nsec / 1000000;
        g_fires = 0;
        for (int i = 0; i < 3000 && !g_fires; i++) usleep(1000);
        printf("io: one-shot cr=%d sr=%d fires=%d gettime_rem=%ldms\n",
               cr, sr, (int)g_fires, rem_ms);
        if (cr == 0 && sr == 0 && g_fires >= 1) bits |= 1;
        if (gr == 0 && rem_ms > 0 && rem_ms <= 100) bits |= 4;
        if (cr == 0) timer_delete(t);
    }

    /* ---- bit1: periodic fires repeatedly, delete stops it ---- */
    {
        timer_t t;
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo  = SIGUSR1;
        int ok = 0;
        if (timer_create(CLOCK_MONOTONIC, &sev, &t) == 0) {
            struct itimerspec its;
            memset(&its, 0, sizeof its);
            its.it_value.tv_nsec    = 50 * 1000000L;
            its.it_interval.tv_nsec = 50 * 1000000L;
            if (timer_settime(t, 0, &its, 0) == 0) {
                g_fires = 0;
                /* EINTR-proof wait: a single usleep(400ms) returns EARLY on
                 * the first delivery (v1 of this test read fires=1 at 50 ms
                 * and blamed the kernel). Sleep in slices against the clock. */
                struct timespec t0, tn;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                do {
                    usleep(10 * 1000);
                    clock_gettime(CLOCK_MONOTONIC, &tn);
                } while ((tn.tv_sec - t0.tv_sec) * 1000 +
                         (tn.tv_nsec - t0.tv_nsec) / 1000000 < 400);
                int during = g_fires;
                timer_delete(t);
                g_fires = 0;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                do {
                    usleep(10 * 1000);
                    clock_gettime(CLOCK_MONOTONIC, &tn);
                } while ((tn.tv_sec - t0.tv_sec) * 1000 +
                         (tn.tv_nsec - t0.tv_nsec) / 1000000 < 200);
                int after = g_fires;
                printf("io: periodic fires=%d (want >=3), post-delete=%d "
                       "(want 0)\n", during, after);
                ok = (during >= 3 && after == 0);
            }
        }
        if (ok) bits |= 2;
    }

    /* ---- copy-family setup: a source file with known content ---- */
    static char pat[65536];
    for (size_t i = 0; i < sizeof pat; i++) pat[i] = (char)(i * 131 + 7);
    int sfd = open("/data/io-src", O_CREAT | O_RDWR | O_TRUNC, 0644);
    (void)!write(sfd, pat, sizeof pat);

    /* ---- bit3: copy_file_range ---- */
    {
        int dfd = open("/data/io-cfr", O_CREAT | O_RDWR | O_TRUNC, 0644);
        off_t oin = 0, oout = 0;
        ssize_t n = copy_file_range(sfd, &oin, dfd, &oout, sizeof pat, 0);
        static char back[65536];
        lseek(dfd, 0, SEEK_SET);
        ssize_t r = read(dfd, back, sizeof back);
        int ok = (n == (ssize_t)sizeof pat && r == n &&
                  memcmp(back, pat, sizeof pat) == 0 &&
                  oin == (off_t)sizeof pat);
        printf("io: copy_file_range n=%zd read-back=%zd exact=%d oin=%lld\n",
               n, r, ok, (long long)oin);
        if (dfd >= 0) close(dfd);
        unlink("/data/io-cfr");
        if (ok) bits |= 8;
    }

    /* ---- bit4: sendfile ---- */
    {
        int dfd = open("/data/io-sf", O_CREAT | O_RDWR | O_TRUNC, 0644);
        off_t off = 0;
        ssize_t n = sendfile(dfd, sfd, &off, sizeof pat);
        static char back[65536];
        lseek(dfd, 0, SEEK_SET);
        ssize_t r = read(dfd, back, sizeof back);
        int ok = (n == (ssize_t)sizeof pat && r == n &&
                  memcmp(back, pat, sizeof pat) == 0);
        printf("io: sendfile n=%zd read-back=%zd exact=%d\n", n, r, ok);
        if (dfd >= 0) close(dfd);
        unlink("/data/io-sf");
        if (ok) bits |= 16;
    }

    /* ---- bit5: splice file->pipe->file (16K through a 4K-ish pipe:
     * loop in slices so the pipe never has to hold it all) ---- */
    {
        int pfd[2];
        int dfd = open("/data/io-sp", O_CREAT | O_RDWR | O_TRUNC, 0644);
        int ok = 0;
        if (pipe(pfd) == 0 && dfd >= 0) {
            off_t oin = 0, oout = 0;
            size_t total = 16384, moved = 0;
            int stuck = 0;
            while (moved < total && !stuck) {
                ssize_t a = splice(sfd, &oin, pfd[1], 0, 2048, 0);
                if (a <= 0) { stuck = 1; break; }
                ssize_t drained = 0;
                while (drained < a) {
                    ssize_t b = splice(pfd[0], 0, dfd, &oout,
                                       (size_t)(a - drained), 0);
                    if (b <= 0) { stuck = 1; break; }
                    drained += b;
                }
                moved += (size_t)drained;
            }
            static char back[16384];
            lseek(dfd, 0, SEEK_SET);
            ssize_t r = read(dfd, back, sizeof back);
            ok = (!stuck && moved == total && r == (ssize_t)total &&
                  memcmp(back, pat, total) == 0);
            printf("io: splice moved=%zu read-back=%zd exact=%d\n",
                   moved, r, ok);
        }
        if (pfd[0] >= 0) close(pfd[0]);
        if (pfd[1] >= 0) close(pfd[1]);
        if (dfd >= 0) close(dfd);
        unlink("/data/io-sp");
        if (ok) bits |= 32;
    }

    if (sfd >= 0) close(sfd);
    unlink("/data/io-src");

    printf("LXIO: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
