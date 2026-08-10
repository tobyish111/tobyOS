/* linux-pidns -- Phase 3 slice 10 acceptance test: PID namespaces.
 *
 * A separate binary from linux-ns (slice 8) on purpose: that test is a passing
 * regression gate and there is no reason to risk it, and a pid namespace needs
 * a full byte of result bits of its own.
 *
 * ===========================================================================
 * THE SHAPE OF EVERY CHECK HERE
 *
 * Same rule as slice 8, and it bites harder for pids: **"the child says its pid
 * is 1" is not evidence of a pid namespace.** A getpid() that returned a
 * constant would satisfy it. So each claim is asserted three ways:
 *
 *   positive : the process in the new namespace sees the new numbering
 *   negative : that numbering is NOT visible outside it
 *   control  : the identical operation WITHOUT CLONE_NEWPID does not renumber
 *
 * The sharpest check in the file is bit4, which has ONE process report TWO
 * different pids to two different readers at the same instant -- 1 to itself,
 * its host pid to its parent. Nothing but a real translation layer produces
 * that, and no constant-returning stub can fake it.
 *
 * Every pid is read with syscall(SYS_getpid) rather than getpid(3), so no libc
 * caching can stand between the assertion and the kernel.
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  baseline: /proc/self/ns/pid is the Linux init constant, and we are
 *         not pid 1
 *   bit1  clone(CLONE_NEWPID): child is pid 1 (positive), parent unchanged
 *         (negative), plain-fork control child is NOT pid 1 (control)
 *   bit2  the child's pid-ns inum differs from the parent's (second witness,
 *         independent of getpid entirely)
 *   bit3  getppid() is 0 inside the namespace (parent is outside it, so it has
 *         no number there) while the control child sees the real parent
 *   bit4  ONE process, TWO numbers: the child reads Pid:1 from its own
 *         /proc/self/status while the parent reads its host pid from
 *         /proc/<hostpid>/status -- simultaneously, via a pipe handshake
 *   bit5  unshare(CLONE_NEWPID) does NOT move the caller; it moves the caller's
 *         next CHILD. Caller's pid and ns inum unchanged; child is pid 1.
 *   bit6  process-table isolation: the child's /proc lists exactly itself
 *         while the parent's lists many
 *   bit7  kill(2) isolation: inside the namespace kill(1,0) reaches the child
 *         itself, and the parent's host pid -- which certainly exists -- is
 *         ESRCH, i.e. the container cannot signal the host
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define NS_NEWPID  0x20000000
#define NS_NEWUTS  0x04000000

/* PROC_PID_INIT_INO from a real Linux kernel's include/linux/proc_ns.h. */
#define INIT_PID_INUM 4026531836ull

/* ---- straight to the kernel, never through a libc cache ---- */
static int k_getpid(void)  { return (int)syscall(SYS_getpid);  }
static int k_getppid(void) { return (int)syscall(SYS_getppid); }
static int k_kill(int pid, int sig) { return (int)syscall(SYS_kill, pid, sig); }
static int k_unshare(int flags) { return (int)syscall(SYS_unshare, flags); }

static unsigned long long ns_inum(const char *whose, const char *kind) {
    char path[128], link[128], want[32];
    snprintf(path, sizeof path, "/proc/%s/ns/%s", whose, kind);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) { printf("pidns: readlink(%s) errno=%d\n", path, errno); return 0; }
    link[n] = '\0';
    snprintf(want, sizeof want, "%s:[", kind);
    size_t wl = strlen(want);
    if (strncmp(link, want, wl) != 0) {
        printf("pidns: %s malformed '%s'\n", path, link); return 0;
    }
    char *end = strchr(link + wl, ']');
    if (!end) { printf("pidns: %s unterminated '%s'\n", path, link); return 0; }
    *end = '\0';
    return strtoull(link + wl, NULL, 10);
}

/* Read the "Pid:" line out of a /proc/<who>/status. -1 on any failure, so a
 * missing file can never be mistaken for a matching value. */
static int status_pid(const char *who) {
    char path[128], buf[2048];
    snprintf(path, sizeof path, "/proc/%s/status", who);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("pidns: open(%s) errno=%d\n", path, errno); return -1; }
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *l = strstr(buf, "Pid:");
    if (!l) return -1;
    return (int)strtol(l + 4, NULL, 10);
}

/* Count numeric directories in /proc -- i.e. how many processes the CALLER's
 * pid namespace can see. Returns -1 on failure. */
static int count_proc_pids(int *lone_pid) {
    DIR *d = opendir("/proc");
    if (!d) { printf("pidns: opendir(/proc) errno=%d\n", errno); return -1; }
    int n = 0, last = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *s = e->d_name;
        if (!*s) continue;
        int all_digit = 1;
        for (const char *q = s; *q; q++)
            if (*q < '0' || *q > '9') { all_digit = 0; break; }
        if (!all_digit) continue;
        n++;
        last = atoi(s);
    }
    closedir(d);
    if (lone_pid) *lone_pid = last;
    return n;
}

/* ------------------------------------------------------------------ */

struct shared {
    int  parent_hostpid;      /* the parent's pid in the INITIAL namespace */
    int  parent_inum_lo;      /* parent's pid-ns inum, for the != assertion */
    int  pipe_to_parent[2];
    int  pipe_to_child[2];
};

/* Spawn via raw clone so CLONE_NEWPID is exercised (a NULL stack makes this
 * behave as a fork, which is what the kernel maps a non-CLONE_THREAD clone
 * onto). Returns the child's host pid, or -1. */
static long spawn_clone(int extra_flags) {
    return syscall(SYS_clone, (long)(extra_flags | SIGCHLD),
                   (long)0, (long)0, (long)0, (long)0);
}

static int reap(long pid) {
    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) != (pid_t)pid) return -1;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

int main(void) {
    int bits = 0;
    /* Unbuffered: the diagnostics that say WHICH sub-check failed come from
     * forked children leaving via _exit(), and glibc's full buffering on a
     * non-tty would discard exactly those. (Learned in slice 8.) */
    setvbuf(stdout, NULL, _IONBF, 0);

    int my_pid = k_getpid();
    unsigned long long my_inum = ns_inum("self", "pid");
    printf("pidns: start pid=%d pid:[%llu]\n", my_pid, my_inum);

    /* ---- bit0: baseline ---- */
    if (my_inum == INIT_PID_INUM && my_pid > 1) {
        bits |= 1;
    } else {
        printf("pidns: baseline FAILED (want inum=%llu, pid>1)\n", INIT_PID_INUM);
    }

    /* ---- bits1-3: clone(CLONE_NEWPID), with a control ---- */
    {
        static struct shared sh;
        sh.parent_hostpid = my_pid;

        /* --- the namespaced child --- */
        long c = spawn_clone(NS_NEWPID);
        if (c < 0) {
            printf("pidns: clone(NEWPID) failed errno=%d\n", errno);
        } else if (c == 0) {
            int rc = 0;
            int p = k_getpid();
            if (p != 1) rc = 2;                            /* positive half */
            else if (k_getppid() != 0) rc = 3;             /* bit3 */
            else {
                unsigned long long in = ns_inum("self", "pid");
                if (in == 0) rc = 4;
                else if (in == INIT_PID_INUM) rc = 5;      /* bit2: must differ */
            }
            _exit(rc);
        } else {
            int rc = reap(c);
            int parent_ok = (k_getpid() == my_pid) &&
                            (ns_inum("self", "pid") == INIT_PID_INUM);
            /* --- the CONTROL: same spawn, no CLONE_NEWPID --- */
            long ctl = spawn_clone(0);
            int ctl_rc = -1;
            if (ctl == 0) {
                int p = k_getpid();
                /* Must NOT have been renumbered, and must see the real parent. */
                _exit((p != 1 && k_getppid() == sh.parent_hostpid) ? 0 : 1);
            } else if (ctl > 0) {
                ctl_rc = reap(ctl);
            }

            if (rc == 0 && parent_ok && ctl_rc == 0) {
                bits |= 2 | 4 | 8;
                printf("pidns: clone(NEWPID) child is pid 1, ppid 0, new inum; "
                       "parent unchanged; control child NOT renumbered\n");
            } else {
                printf("pidns: clone(NEWPID) FAILED child_rc=%d parent_ok=%d "
                       "control_rc=%d\n", rc, parent_ok, ctl_rc);
            }
        }
    }

    /* ---- bit4: ONE process, TWO numbers, at the same instant ----
     * The pipe handshake makes this deterministic -- no sleeps, so it cannot
     * become flaky under slow TCG. */
    {
        int up[2], down[2];
        if (pipe(up) != 0 || pipe(down) != 0) {
            printf("pidns: pipe failed errno=%d\n", errno);
        } else {
            long c = spawn_clone(NS_NEWPID);
            if (c == 0) {
                close(up[0]); close(down[1]);
                int self_view = status_pid("self");
                /* Tell the parent we are up, and what WE think our pid is. */
                if (write(up[1], &self_view, sizeof self_view) != sizeof self_view)
                    _exit(1);
                char ack;
                if (read(down[0], &ack, 1) != 1) _exit(2);   /* stay alive */
                _exit(self_view == 1 ? 0 : 3);
            } else if (c > 0) {
                close(up[1]); close(down[0]);
                int child_view = -1;
                int got = (read(up[0], &child_view, sizeof child_view)
                           == sizeof child_view);
                /* The child is still alive and blocked. Read the SAME process
                 * through the parent's namespace. */
                char who[24];
                snprintf(who, sizeof who, "%ld", c);
                int parent_view = status_pid(who);
                char ack = 'x';
                (void)!write(down[1], &ack, 1);
                int rc = reap(c);
                close(up[0]); close(down[1]);

                printf("pidns: same process -- it reads Pid:%d, parent reads "
                       "Pid:%d (host pid %ld)\n", child_view, parent_view, c);
                if (got && rc == 0 && child_view == 1 &&
                    parent_view == (int)c && (int)c != 1) {
                    bits |= 16;
                } else {
                    printf("pidns: two-views FAILED got=%d rc=%d child=%d "
                           "parent=%d\n", got, rc, child_view, parent_view);
                }
            }
        }
    }

    /* ---- bit5: unshare(CLONE_NEWPID) moves CHILDREN, not the caller ----
     * The distinctive Linux behaviour, and the one most likely to be
     * implemented as "move the caller" by mistake. Asserted from both ends:
     * the caller is provably unchanged, the child is provably renumbered. */
    {
        long c = spawn_clone(0);       /* do the unshare in a throwaway child */
        if (c == 0) {
            int before      = k_getpid();
            unsigned long long ib = ns_inum("self", "pid");
            if (k_unshare(NS_NEWPID) != 0) _exit(1);
            int after       = k_getpid();
            unsigned long long ia = ns_inum("self", "pid");
            /* The caller must be untouched. */
            if (after != before || ia != ib) _exit(2);
            /* ...but its next child must be init of the new namespace. */
            long g = spawn_clone(0);
            if (g < 0) _exit(3);
            if (g == 0) {
                unsigned long long gi = ns_inum("self", "pid");
                _exit((k_getpid() == 1 && gi != ib && gi != 0) ? 0 : 4);
            }
            int grc = reap(g);
            /* And the caller STILL must not have moved. */
            if (k_getpid() != before) _exit(5);
            _exit(grc == 0 ? 0 : 6);
        } else if (c > 0) {
            int rc = reap(c);
            if (rc == 0) {
                bits |= 32;
                printf("pidns: unshare(NEWPID) left the caller in place and put "
                       "its child at pid 1\n");
            } else {
                printf("pidns: unshare(NEWPID) semantics FAILED rc=%d\n", rc);
            }
        }
    }

    /* ---- bit6: process-table isolation, two-sided ---- */
    {
        int parent_n = count_proc_pids(NULL);
        long c = spawn_clone(NS_NEWPID);
        if (c == 0) {
            int lone = 0;
            int n = count_proc_pids(&lone);
            /* Exactly itself, listed as 1. */
            _exit((n == 1 && lone == 1) ? 0 : (n < 0 ? 1 : 2));
        } else if (c > 0) {
            int rc = reap(c);
            if (rc == 0 && parent_n > 1) {
                bits |= 64;
                printf("pidns: /proc isolation -- child sees 1 process, parent "
                       "sees %d\n", parent_n);
            } else {
                printf("pidns: /proc isolation FAILED rc=%d parent_n=%d\n",
                       rc, parent_n);
            }
        }
    }

    /* ---- bit7: kill(2) cannot cross the boundary ---- */
    {
        static int host_pid_for_child;
        host_pid_for_child = my_pid;
        long c = spawn_clone(NS_NEWPID);
        if (c == 0) {
            /* Positive: pid 1 in here is us, and signal 0 is the existence
             * probe, so this must succeed. */
            if (k_kill(1, 0) != 0) _exit(1);
            /* Negative: the parent certainly exists on the host. From inside
             * the namespace it must be unreachable -- and specifically ESRCH,
             * not merely "some error", or we would also accept a kill(2) that
             * is broken for an unrelated reason. */
            errno = 0;
            if (k_kill(host_pid_for_child, 0) != -1) _exit(2);
            if (errno != ESRCH) _exit(3);
            _exit(0);
        } else if (c > 0) {
            int rc = reap(c);
            /* Control: from out here, that same host pid IS signalable. */
            int ctl = (k_kill(my_pid, 0) == 0);
            if (rc == 0 && ctl) {
                bits |= 128;
                printf("pidns: kill isolation -- in-ns kill(1,0) ok, host pid "
                       "%d is ESRCH from inside, reachable from outside\n",
                       my_pid);
            } else {
                printf("pidns: kill isolation FAILED rc=%d control=%d\n",
                       rc, ctl);
            }
        }
    }

    printf("pidns: end pid=%d pid:[%llu]\n", k_getpid(),
           ns_inum("self", "pid"));
    printf("pidns: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
