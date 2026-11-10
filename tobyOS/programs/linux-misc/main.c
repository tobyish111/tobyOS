/* linux-misc -- the 2026-08-22 syscall-tail batch, asserted by value.
 *
 *   bit0  auxv carries REAL credentials: after setuid(1000) + re-exec,
 *         getauxval(AT_UID/EUID/GID/EGID) agree with getuid()&c and are
 *         the NON-TRIVIAL values (they were hardcoded 0, which as root
 *         agrees by accident -- hence the drop-then-reexec shape)
 *   bit1  AT_SECURE is 0 for this ordinary exec (glibc trusts LD_* env;
 *         a wrong 1 here would silently break every LD_LIBRARY_PATH
 *         launch in the tree)
 *   bit2  sendmmsg/recvmmsg: two datagrams cross a socketpair in ONE
 *         call each, with per-message msg_len filled in
 *   bit3  io_uring_setup and bpf answer ENOSYS authoritatively (the
 *         census contract claimed these arms existed; now they do --
 *         and the gate's enosys_gaps=0 double-checks from the outside)
 *   bit4  sigqueue(3) (rt_sigqueueinfo) delivers to an SA_SIGINFO
 *         handler with the right signo and a sane si_pid
 *   bit5  restart_syscall answers EINTR, not unknown-syscall
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static volatile sig_atomic_t g_signo, g_sipid;
static void on_info(int s, siginfo_t *si, void *u) {
    (void)u;
    g_signo = s;
    g_sipid = si ? si->si_pid : -1;
}

static int child_main(void) {
    int bits = 0;
    /* bit0: we were re-exec'd after setuid(1000): every id is 1000 and the
     * auxv must SAY so. */
    unsigned long au = getauxval(AT_UID),  ae = getauxval(AT_EUID);
    unsigned long ag = getauxval(AT_GID),  aeg = getauxval(AT_EGID);
    printf("misc-child: AT_UID=%lu AT_EUID=%lu AT_GID=%lu AT_EGID=%lu "
           "uid=%u euid=%u\n", au, ae, ag, aeg, getuid(), geteuid());
    if (au == getuid() && ae == geteuid() && ag == getgid() &&
        aeg == getegid() && au == 1000) bits |= 1;
    if (getauxval(AT_SECURE) == 0) bits |= 2;
    return bits;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--misc-child") == 0)
        return child_main();

    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit2: sendmmsg / recvmmsg over a socketpair ---- */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            char a[] = "first", b[] = "second!";
            struct iovec iva = { a, 5 }, ivb = { b, 7 };
            struct mmsghdr tx[2];
            memset(tx, 0, sizeof tx);
            tx[0].msg_hdr.msg_iov = &iva; tx[0].msg_hdr.msg_iovlen = 1;
            tx[1].msg_hdr.msg_iov = &ivb; tx[1].msg_hdr.msg_iovlen = 1;
            int sn = sendmmsg(sv[0], tx, 2, 0);
            char ra[16] = {0}, rb[16] = {0};
            struct iovec riva = { ra, sizeof ra }, rivb = { rb, sizeof rb };
            struct mmsghdr rx[2];
            memset(rx, 0, sizeof rx);
            rx[0].msg_hdr.msg_iov = &riva; rx[0].msg_hdr.msg_iovlen = 1;
            rx[1].msg_hdr.msg_iov = &rivb; rx[1].msg_hdr.msg_iovlen = 1;
            int rn = recvmmsg(sv[1], rx, 2, 0, 0);
            printf("misc: sendmmsg=%d recvmmsg=%d lens=%u,%u data='%s','%s'\n",
                   sn, rn, rx[0].msg_len, rx[1].msg_len, ra, rb);
            if (sn == 2 && rn == 2 && tx[0].msg_len == 5 && tx[1].msg_len == 7 &&
                rx[0].msg_len == 5 && rx[1].msg_len == 7 &&
                memcmp(ra, "first", 5) == 0 && memcmp(rb, "second!", 7) == 0)
                bits |= 4;
            close(sv[0]); close(sv[1]);
        }
    }

    /* ---- bit3: authoritative ENOSYS for io_uring + bpf ---- */
    {
        errno = 0;
        long r1 = syscall(425 /* io_uring_setup */, 8, 0);
        int e1 = errno;
        errno = 0;
        long r2 = syscall(321 /* bpf */, 0, 0, 0);
        int e2 = errno;
        printf("misc: io_uring_setup rc=%ld errno=%d, bpf rc=%ld errno=%d "
               "(want ENOSYS=%d)\n", r1, e1, r2, e2, ENOSYS);
        if (r1 == -1 && e1 == ENOSYS && r2 == -1 && e2 == ENOSYS) bits |= 8;
    }

    /* ---- bit4: sigqueue via rt_sigqueueinfo ---- */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_info;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGUSR1, &sa, 0);
        union sigval v; v.sival_int = 7;
        int qr = sigqueue(getpid(), SIGUSR1, v);
        for (int i = 0; i < 2000 && !g_signo; i++) usleep(1000);
        printf("misc: sigqueue rc=%d handler signo=%d si_pid=%d (self=%d)\n",
               qr, (int)g_signo, (int)g_sipid, (int)getpid());
        if (qr == 0 && g_signo == SIGUSR1 && g_sipid == getpid()) bits |= 16;
        signal(SIGUSR1, SIG_DFL);
    }

    /* ---- bit5: restart_syscall is EINTR, not unknown ---- */
    {
        errno = 0;
        long r = syscall(219 /* restart_syscall */);
        printf("misc: restart_syscall rc=%ld errno=%d (want EINTR=%d)\n",
               r, errno, EINTR);
        if (r == -1 && errno == EINTR) bits |= 32;
    }

    /* ---- bits 0+1: drop to 1000 and re-exec; child reports via exit ---- */
    {
        if (setuid(1000) == 0) {
            /* Replace ourselves: the CHILD-mode run returns bits 0-1 and we
             * are done -- exec never returns on success, so fold the two
             * runs with fork instead. */
            pid_t pid = fork();
            if (pid == 0) {
                char *cargv[] = { "/proc/self/exe", "--misc-child", NULL };
                execv("/proc/self/exe", cargv);
                _exit(0);
            }
            if (pid > 0) {
                int st = 0;
                if (waitpid(pid, &st, 0) == pid && WIFEXITED(st))
                    bits |= WEXITSTATUS(st) & 3;   /* child bits 0-1 */
            }
        } else {
            printf("misc: setuid(1000) failed -- not root?\n");
        }
    }

    printf("LXMISC: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
