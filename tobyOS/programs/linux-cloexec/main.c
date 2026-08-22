/* linux-cloexec -- close-on-exec acceptance test (2026-08-22).
 *
 * Before this slice the kernel had NO close-on-exec tracking at all:
 * O_CLOEXEC/SOCK_CLOEXEC/EFD_CLOEXEC were parsed and dropped by every
 * creator, fcntl(F_GETFD/F_SETFD) was a blanket `return 0`, and every
 * descriptor leaked into every exec'd image. A test that only read the
 * flag back would pass on a kernel that stores the bit and never acts on
 * it -- so bit3 execs a real child and asserts which descriptors SURVIVED,
 * which is the only observable that matters.
 *
 * Each check sets a bit; all pass => exit 63.
 *
 *   bit0  O_CLOEXEC at open() reads back via F_GETFD; a plain open reads 0
 *   bit1  F_SETFD sets and clears for real (round-trip both directions)
 *   bit2  the dup family: pipe2(O_CLOEXEC) marks both ends; dup2 CLEARS on
 *         the new fd; dup3(O_CLOEXEC) and F_DUPFD_CLOEXEC set; F_DUPFD clears
 *   bit3  exec: a marked fd is CLOSED in the exec'd image, an unmarked one
 *         SURVIVES -- asserted from inside the child, not inferred
 *   bit4  close_range(2): plain form closes the range; CLOSE_RANGE_CLOEXEC
 *         marks instead of closing
 *   bit5  socketpair(SOCK_CLOEXEC|SOCK_NONBLOCK): both ends marked AND the
 *         empty read is EAGAIN (both type bits used to be parsed and DROPPED
 *         here, unlike socket()/accept4() which honoured them)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1u << 2)
#endif

/* Child mode: argv[1] == "--cloexec-child", argv[2] = fd that was marked
 * cloexec (must be CLOSED here), argv[3] = fd left unmarked (must be OPEN).
 * Exit 42 iff both hold. Asserting the ERRNO, not just failure: a closed fd
 * answers EBADF specifically. */
static int child_main(int argc, char **argv) {
    if (argc < 4) return 40;
    int marked = atoi(argv[2]);
    int plain  = atoi(argv[3]);
    errno = 0;
    int rm = fcntl(marked, F_GETFD);
    int marked_closed = (rm == -1 && errno == EBADF);
    int rp = fcntl(plain, F_GETFD);
    int plain_open = (rp >= 0);
    /* Report through the surviving fd so the parent's log shows WHICH half
     * failed even though our stdout may be the harness console. */
    fprintf(stderr, "cloexec-child: marked fd %d %s (rc=%d errno=%d), "
                    "plain fd %d %s\n",
            marked, marked_closed ? "CLOSED" : "STILL OPEN", rm, errno,
            plain, plain_open ? "open" : "MISSING");
    if (!marked_closed) return 41;
    if (!plain_open)    return 40;
    return 42;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--cloexec-child") == 0)
        return child_main(argc, argv);

    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: O_CLOEXEC at open, and its absence, both read back ---- */
    {
        int a = open("/dev/null", O_RDONLY | O_CLOEXEC);
        int b = open("/dev/null", O_RDONLY);
        int fa = (a >= 0) ? fcntl(a, F_GETFD) : -1;
        int fb = (b >= 0) ? fcntl(b, F_GETFD) : -1;
        printf("cloexec: open(O_CLOEXEC) F_GETFD=%d, plain open F_GETFD=%d\n",
               fa, fb);
        if (fa == FD_CLOEXEC && fb == 0) bits |= 1;
        if (a >= 0) close(a);
        if (b >= 0) close(b);
    }

    /* ---- bit1: F_SETFD round-trips in both directions ---- */
    {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0 &&
            fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
            fcntl(fd, F_GETFD) == FD_CLOEXEC &&
            fcntl(fd, F_SETFD, 0) == 0 &&
            fcntl(fd, F_GETFD) == 0) {
            bits |= 2;
            printf("cloexec: F_SETFD set->read->clear->read ok\n");
        } else {
            printf("cloexec: F_SETFD round-trip FAILED\n");
        }
        if (fd >= 0) close(fd);
    }

    /* ---- bit2: the dup family's flag rules ---- */
    {
        int pf[2] = { -1, -1 };
        int ok = 0;
        if (pipe2(pf, O_CLOEXEC) == 0 &&
            fcntl(pf[0], F_GETFD) == FD_CLOEXEC &&
            fcntl(pf[1], F_GETFD) == FD_CLOEXEC) {
            int d2 = dup2(pf[0], 60);               /* dup2: flag CLEARS */
            int d3 = dup3(pf[0], 61, O_CLOEXEC);    /* dup3(O_CLOEXEC): sets */
            int dc = fcntl(pf[0], F_DUPFD_CLOEXEC, 62);
            int dp = fcntl(pf[0], F_DUPFD, 62);     /* lowest free >= 62: 63 */
            ok = (d2 == 60 && fcntl(60, F_GETFD) == 0) &&
                 (d3 == 61 && fcntl(61, F_GETFD) == FD_CLOEXEC) &&
                 (dc >= 62 && fcntl(dc, F_GETFD) == FD_CLOEXEC) &&
                 (dp >= 62 && fcntl(dp, F_GETFD) == 0);
            printf("cloexec: dup2->%d(fd flags %d) dup3->%d(%d) "
                   "F_DUPFD_CLOEXEC->%d(%d) F_DUPFD->%d(%d)\n",
                   d2, fcntl(60, F_GETFD), d3, fcntl(61, F_GETFD),
                   dc, dc >= 0 ? fcntl(dc, F_GETFD) : -1,
                   dp, dp >= 0 ? fcntl(dp, F_GETFD) : -1);
            if (d2 >= 0) close(d2);
            if (d3 >= 0) close(d3);
            if (dc >= 0) close(dc);
            if (dp >= 0) close(dp);
        }
        if (pf[0] >= 0) close(pf[0]);
        if (pf[1] >= 0) close(pf[1]);
        if (ok) bits |= 4;
    }

    /* ---- bit3: exec actually closes marked fds and keeps unmarked ---- */
    {
        int marked = open("/dev/null", O_RDONLY | O_CLOEXEC);
        int plain  = open("/dev/null", O_RDONLY);
        if (marked >= 0 && plain >= 0) {
            pid_t pid = fork();
            if (pid == 0) {
                char m[16], p[16];
                snprintf(m, sizeof m, "%d", marked);
                snprintf(p, sizeof p, "%d", plain);
                char *cargv[] = { "/proc/self/exe", "--cloexec-child",
                                  m, p, NULL };
                execv("/proc/self/exe", cargv);
                _exit(39);                  /* exec itself failed */
            }
            if (pid > 0) {
                int st = 0;
                if (waitpid(pid, &st, 0) == pid && WIFEXITED(st)) {
                    printf("cloexec: exec child exit=%d (want 42)\n",
                           WEXITSTATUS(st));
                    if (WEXITSTATUS(st) == 42) bits |= 8;
                }
            }
        }
        if (marked >= 0) close(marked);
        if (plain >= 0) close(plain);
    }

    /* ---- bit4: close_range, both forms ---- */
    {
        int a = open("/dev/null", O_RDONLY);
        int b = open("/dev/null", O_RDONLY);
        int c = open("/dev/null", O_RDONLY);
        int ok = 0;
        if (a >= 0 && b >= 0 && c >= 0) {
            /* Mark [a..b] cloexec without closing: still open, flag set. */
            long r1 = syscall(SYS_close_range, (unsigned)a, (unsigned)b,
                              CLOSE_RANGE_CLOEXEC);
            int fa = fcntl(a, F_GETFD), fb = fcntl(b, F_GETFD);
            /* Then really close [a..c]: all three EBADF afterwards. */
            long r2 = syscall(SYS_close_range, (unsigned)a, (unsigned)c, 0);
            errno = 0;
            int ga = fcntl(a, F_GETFD);
            int ea = errno;
            ok = (r1 == 0 && fa == FD_CLOEXEC && fb == FD_CLOEXEC &&
                  r2 == 0 && ga == -1 && ea == EBADF &&
                  fcntl(b, F_GETFD) == -1 && fcntl(c, F_GETFD) == -1);
            printf("cloexec: close_range mark rc=%ld flags=%d,%d; "
                   "close rc=%ld post-errno=%d\n", r1, fa, fb, r2, ea);
        }
        if (ok) bits |= 16;
        else { if (a >= 0) close(a); if (b >= 0) close(b); if (c >= 0) close(c); }
    }

    /* ---- bit5: socketpair honours BOTH type bits it used to drop ---- */
    {
        int sv[2] = { -1, -1 };
        int ok = 0;
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                       0, sv) == 0) {
            char buf[8];
            errno = 0;
            ssize_t rr = read(sv[0], buf, sizeof buf);   /* nothing sent */
            int nb_ok = (rr == -1 && errno == EAGAIN);
            ok = (fcntl(sv[0], F_GETFD) == FD_CLOEXEC &&
                  fcntl(sv[1], F_GETFD) == FD_CLOEXEC && nb_ok);
            printf("cloexec: socketpair flags=%d,%d empty-read rc=%zd "
                   "errno=%d (want EAGAIN=%d)\n",
                   fcntl(sv[0], F_GETFD), fcntl(sv[1], F_GETFD),
                   rr, errno, EAGAIN);
        }
        if (sv[0] >= 0) close(sv[0]);
        if (sv[1] >= 0) close(sv[1]);
        if (ok) bits |= 32;
    }

    printf("LXCLOEXEC: VERDICT bits=%d (63=all)\n", bits);
    fflush(stdout);
    return bits;
}
