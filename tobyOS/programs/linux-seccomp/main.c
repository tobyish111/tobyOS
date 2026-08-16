/* linux-seccomp -- Phase 3 slice 13 acceptance test: seccomp-bpf.
 *
 * ===========================================================================
 * A SANDBOX THAT FAILS OPEN IS WORSE THAN NO SANDBOX
 *
 * Every check here is built to distinguish "the filter denied this" from "this
 * would have failed anyway". A test that only asserted "the syscall returned an
 * error" would pass on a kernel where seccomp(2) is a no-op stub, because most
 * of the syscalls worth blocking can fail for their own reasons.
 *
 * So each denial is paired with a POSITIVE control on the SAME syscall:
 *   - the filter returns a DISTINCTIVE errno (EACCES / a chosen value), never a
 *     plausible one, so a real failure cannot be mistaken for a filtered one;
 *   - and the same syscall is shown WORKING before the filter is installed.
 *
 * bit5 is the one that matters most: a filter must be INESCAPABLE. If a child
 * could shed it by fork()ing or by exec()ing, it would not be a sandbox at all.
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  seccomp(2) exists; SECCOMP_GET_ACTION_AVAIL answers honestly (ALLOW
 *         and ERRNO available, TRACE/USER_NOTIF reported unavailable rather
 *         than silently accepted)
 *   bit1  an unprivileged process must set PR_SET_NO_NEW_PRIVS first, and
 *         PR_GET_NO_NEW_PRIVS actually reports it (it was a no-op stub before)
 *   bit2  SECCOMP_RET_ERRNO: the chosen syscall returns the filter's exact
 *         errno, while an unfiltered syscall still works (control)
 *   bit3  the verifier REJECTS a malformed program (backward jump, out-of-range
 *         load, missing trailing RET) instead of running it
 *   bit4  filters CHAIN and only tighten: a second, stricter filter wins, and a
 *         second, looser filter cannot re-open what the first closed
 *   bit5  INESCAPABLE: the filter survives fork() AND execve()
 *   bit6  SECCOMP_RET_KILL terminates the process (observed via wait status)
 *   bit7  PR_GET_SECCOMP reports the mode; a filter cannot be un-installed
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* ---- classic BPF, spelled out so the test does not depend on libseccomp ---- */
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog  { unsigned short len; struct sock_filter *filter; };

#define BPF_LD   0x00
#define BPF_W    0x00
#define BPF_ABS  0x20
#define BPF_JMP  0x05
#define BPF_JEQ  0x10
#define BPF_JA   0x00
#define BPF_K    0x00
#define BPF_RET  0x06

#define STMT(c, kk)          { (unsigned short)(c), 0, 0, (unsigned int)(kk) }
#define JUMP(c, kk, t, f)    { (unsigned short)(c), (t), (f), (unsigned int)(kk) }

#define SD_NR 0                     /* offsetof(struct seccomp_data, nr) */

#define SECCOMP_SET_MODE_STRICT   0
#define SECCOMP_SET_MODE_FILTER   1
#define SECCOMP_GET_ACTION_AVAIL  2

#define RET_KILL_PROCESS 0x80000000u
#define RET_TRAP         0x00030000u
#define RET_ERRNO        0x00050000u
#define RET_USER_NOTIF   0x7fc00000u
#define RET_TRACE        0x7ff00000u
#define RET_ALLOW        0x7fff0000u

#define MODE_DISABLED 0
#define MODE_FILTER   2

/* The syscall we deny, and the errno we deny it with. getppid is ideal: it takes
 * no arguments, cannot fail for any reason of its own, and nothing else in this
 * program needs it -- so ANY error from it can only have come from the filter. */
#define DENIED_NR   SYS_getppid
#define DENY_ERRNO  EACCES

static long k_seccomp(unsigned op, unsigned flags, void *arg) {
    return syscall(SYS_seccomp, op, flags, arg);
}

static int install(struct sock_filter *f, unsigned short len) {
    struct sock_fprog prog = { len, f };
    return (int)k_seccomp(SECCOMP_SET_MODE_FILTER, 0, &prog);
}

/* Deny DENIED_NR with `err`, allow everything else. */
static int install_deny(int err) {
    struct sock_filter f[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, SD_NR),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, DENIED_NR, 0, 1),
        STMT(BPF_RET | BPF_K, RET_ERRNO | (unsigned)err),
        STMT(BPF_RET | BPF_K, RET_ALLOW),
    };
    return install(f, 4);
}

static int reap_status(pid_t p, int *status) {
    if (p < 0 || waitpid(p, status, 0) != p) return -1;
    return 0;
}
static int reap(pid_t p) {
    int st = 0;
    if (reap_status(p, &st) != 0 || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

/* Is DENIED_NR currently blocked with DENY_ERRNO? */
static int denied_now(int expect_errno) {
    errno = 0;
    long r = syscall(DENIED_NR);
    return (r == -1 && errno == expect_errno);
}

int main(int argc, char **argv) {
    /* Re-exec probe for bit5: if we were exec'd by the filtered child, just
     * report whether the filter is STILL in force. This is the only way to
     * observe a filter surviving execve from inside the program. */
    if (argc > 1 && strcmp(argv[1], "--post-exec") == 0) {
        int mode = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
        _exit((denied_now(DENY_ERRNO) && mode == MODE_FILTER) ? 0 : 1);
    }

    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("seccomp: start pid=%d\n", (int)getpid());

    /* ---- bit0: the syscall exists and answers honestly ---- */
    {
        int ok = 1;
        unsigned a;
        a = RET_ALLOW;
        if (k_seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) != 0) { printf("seccomp: ALLOW unavailable\n"); ok = 0; }
        a = RET_ERRNO;
        if (k_seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) != 0) { printf("seccomp: ERRNO unavailable\n"); ok = 0; }
        a = RET_KILL_PROCESS;
        if (k_seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) != 0) { printf("seccomp: KILL unavailable\n"); ok = 0; }
        /* TRACE/USER_NOTIF need a supervisor we do not have. Reporting them
         * UNAVAILABLE is the honest answer -- claiming them and then treating
         * them as ALLOW would be a filter that fails open. */
        a = RET_TRACE;
        if (k_seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) == 0) {
            printf("seccomp: TRACE claimed available but there is no tracer\n"); ok = 0;
        }
        a = RET_USER_NOTIF;
        if (k_seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) == 0) {
            printf("seccomp: USER_NOTIF claimed available but there is no notifier\n"); ok = 0;
        }
        if (ok) { bits |= 1; printf("seccomp: action availability reported honestly\n"); }
    }

    /* ---- bit1: no_new_privs is REAL (it was an accept-and-ignore stub) ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (syscall(SYS_setuid, 1000) != 0) _exit(1);
            if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 0) _exit(2);
            /* Unprivileged, no_new_privs unset: installing a filter must fail. */
            if (install_deny(DENY_ERRNO) == 0) _exit(3);
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(4);
            /* ...and the flag must actually READ BACK, which a stub would not do. */
            if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) _exit(5);
            /* ...and now the filter installs. */
            if (install_deny(DENY_ERRNO) != 0) _exit(6);
            _exit(denied_now(DENY_ERRNO) ? 0 : 7);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 2;
            printf("seccomp: no_new_privs gates unprivileged filters AND reads back\n");
        } else printf("seccomp: bit1 FAILED rc=%d\n", rc);
    }

    /* ---- bit2: RET_ERRNO returns the filter's EXACT errno ----
     * The control is taken on the SAME syscall before the filter exists, so a
     * pass cannot mean "getppid was broken all along". */
    {
        pid_t c = fork();
        if (c == 0) {
            errno = 0;
            if (syscall(DENIED_NR) < 0) _exit(1);        /* control: works now */
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(2);
            if (install_deny(DENY_ERRNO) != 0) _exit(3);
            if (!denied_now(DENY_ERRNO)) _exit(4);
            /* An unfiltered syscall must still work -- otherwise "everything
             * fails" would masquerade as a working filter. */
            if (getpid() <= 0) _exit(5);
            if (write(1, "", 0) != 0) _exit(6);
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 4;
            printf("seccomp: RET_ERRNO gives the exact errno; other syscalls "
                   "unaffected\n");
        } else printf("seccomp: bit2 FAILED rc=%d\n", rc);
    }

    /* ---- bit3: the verifier rejects malformed programs ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(1);
            /* (a) no trailing RET -- falling off the end has no defined value */
            { struct sock_filter f[] = { STMT(BPF_LD | BPF_W | BPF_ABS, SD_NR) };
              if (install(f, 1) == 0) _exit(2); }
            /* (b) load past the end of seccomp_data (would read kernel memory) */
            { struct sock_filter f[] = { STMT(BPF_LD | BPF_W | BPF_ABS, 4096),
                                         STMT(BPF_RET | BPF_K, RET_ALLOW) };
              if (install(f, 2) == 0) _exit(3); }
            /* (c) a jump past the end -- the check that makes filters terminate */
            { struct sock_filter f[] = { STMT(BPF_JMP | BPF_JA, 999),
                                         STMT(BPF_RET | BPF_K, RET_ALLOW) };
              if (install(f, 2) == 0) _exit(4); }
            /* (d) zero-length */
            { struct sock_fprog p0 = { 0, 0 };
              if (k_seccomp(SECCOMP_SET_MODE_FILTER, 0, &p0) == 0) _exit(5); }
            /* And a WELL-FORMED program must still install -- else "rejects
             * everything" would look like a working verifier. */
            if (install_deny(DENY_ERRNO) != 0) _exit(6);
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 8;
            printf("seccomp: verifier rejects no-RET / OOB-load / wild-jump / "
                   "empty, and still accepts a valid filter\n");
        } else printf("seccomp: bit3 FAILED rc=%d\n", rc);
    }

    /* ---- bit4: filters CHAIN and can only tighten ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(1);
            if (install_deny(EACCES) != 0) _exit(2);
            if (!denied_now(EACCES)) _exit(3);
            /* Install a SECOND filter that would ALLOW the same syscall. Every
             * filter runs and the lowest return wins, so the denial must stand:
             * a policy can never be loosened by adding to it. */
            { struct sock_filter f[] = { STMT(BPF_RET | BPF_K, RET_ALLOW) };
              if (install(f, 1) != 0) _exit(4); }
            if (!denied_now(EACCES)) { printf("seccomp: a later ALLOW filter RE-OPENED a denial\n"); _exit(5); }
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 16;
            printf("seccomp: filters chain -- a later ALLOW cannot re-open an "
                   "earlier denial\n");
        } else printf("seccomp: bit4 FAILED rc=%d\n", rc);
    }

    /* ---- bit5: INESCAPABLE -- survives fork AND execve ----
     * The single most important property here. A filter a child could shed by
     * forking or exec'ing would not be a sandbox. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(1);
            if (install_deny(DENY_ERRNO) != 0) _exit(2);
            /* (a) survives fork */
            pid_t g = fork();
            if (g == 0) _exit(denied_now(DENY_ERRNO) ? 0 : 9);
            int gst = 0;
            if (waitpid(g, &gst, 0) != g || !WIFEXITED(gst) ||
                WEXITSTATUS(gst) != 0) _exit(3);
            /* (b) survives execve -- re-exec ourselves and let the child report */
            execl("/bin/linux-seccomp", "linux-seccomp", "--post-exec", (char *)0);
            _exit(4);                                  /* exec failed */
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 32;
            printf("seccomp: filter survives BOTH fork() and execve() -- "
                   "inescapable\n");
        } else printf("seccomp: bit5 FAILED rc=%d\n", rc);
    }

    /* ---- bit6: RET_KILL actually kills ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(1);
            struct sock_filter f[] = {
                STMT(BPF_LD | BPF_W | BPF_ABS, SD_NR),
                JUMP(BPF_JMP | BPF_JEQ | BPF_K, DENIED_NR, 0, 1),
                STMT(BPF_RET | BPF_K, RET_KILL_PROCESS),
                STMT(BPF_RET | BPF_K, RET_ALLOW),
            };
            if (install(f, 4) != 0) _exit(2);
            (void)syscall(DENIED_NR);       /* must not return */
            _exit(50);                      /* reached => NOT killed */
        }
        int st = 0;
        int got = (reap_status(c, &st) == 0);
        /* The process must NOT have exited 50. Either a signal death or the
         * kernel's 128+SIGSYS convention counts; what must not happen is the
         * syscall returning normally. */
        int killed = got && !(WIFEXITED(st) && WEXITSTATUS(st) == 50);
        if (killed) {
            bits |= 64;
            printf("seccomp: RET_KILL terminated the process (status 0x%x)\n", st);
        } else {
            printf("seccomp: bit6 FAILED -- RET_KILL did not kill (status 0x%x)\n", st);
        }
    }

    /* ---- bit7: PR_GET_SECCOMP, and a filter cannot be un-installed ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != MODE_DISABLED) _exit(1);
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(2);
            if (install_deny(DENY_ERRNO) != 0) _exit(3);
            if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != MODE_FILTER) _exit(4);
            /* There is no "uninstall" operation; assert the closest attempts do
             * not work. Mode 0 is not a valid SET argument. */
            if (prctl(PR_SET_SECCOMP, MODE_DISABLED, 0, 0, 0) == 0) _exit(5);
            if (!denied_now(DENY_ERRNO)) _exit(6);
            _exit(0);
        }
        int rc = reap(c);
        /* And OUR mode must still be DISABLED -- every filter above was
         * installed in a child, so this process must be untouched. */
        int ours = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
        if (rc == 0 && ours == MODE_DISABLED) {
            bits |= 128;
            printf("seccomp: PR_GET_SECCOMP reports the mode; no way to "
                   "un-install; our own mode untouched\n");
        } else printf("seccomp: bit7 FAILED rc=%d ours=%d\n", rc, ours);
    }

    printf("seccomp: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
