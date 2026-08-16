/* linux-ns -- Phase 3 slice 8 acceptance test: UTS + IPC namespaces.
 *
 * Runs as root and exercises the whole namespace mechanism: unshare(2),
 * clone(CLONE_NEW*), setns(2), and the /proc/PID/ns tree.
 *
 * ===========================================================================
 * THE METHOD RULE THIS TEST IS BUILT AROUND
 *
 * "The parent's hostname did not change" is NOT evidence of isolation. It is
 * equally satisfied by a sethostname(2) that does nothing at all -- and a
 * no-op syscall returning success is a failure mode this project has shipped
 * before (setitimer, utimensat, VFS_MNT_RDONLY). So EVERY isolation claim here
 * is asserted from BOTH sides:
 *
 *   positive : the process that unshared sees its OWN change take effect
 *   negative : the process that did not unshare does NOT see that change
 *   control  : the same operation WITHOUT unshare *does* propagate
 *
 * The control is what makes the result attributable. Without it, a fork that
 * accidentally deep-copied the namespace would pass the first two checks and
 * look exactly like working isolation.
 *
 * Namespace identity is also cross-checked independently, through
 * /proc/PID/ns/<kind> inode numbers -- a second witness that does not go
 * through the payload at all. If sethostname and the inum ever disagree, one
 * of them is lying and the test fails rather than picking the convenient one.
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  baseline: uname() nodename + the initial UTS inum is the Linux
 *         constant 4026531838
 *   bit1  sethostname(2) actually works in the initial namespace (the check
 *         that makes bit2's negative half meaningful)
 *   bit2  unshare(CLONE_NEWUTS) ISOLATES -- child sees its change, different
 *         inum; parent's hostname untouched
 *   bit3  CONTROL: a plain fork SHARES the UTS namespace, so the same
 *         sethostname *does* reach the parent
 *   bit4  clone(CLONE_NEWUTS) isolates too (a different code path from
 *         unshare: the flags are applied inside fork, before the child can run)
 *   bit5  setns(2) round-trip: a child in a new namespace re-enters the
 *         parent's and its inum matches the parent's again
 *   bit6  IPC: an unshared child cannot see a SysV segment the parent created,
 *         while a non-unshared control child can
 *   bit7  honest refusals, the /proc/PID/ns listing, and the machine hostname
 *         restored to what it was on entry
 *
 * That last part is not cosmetic. bit1 and bit3 deliberately change the REAL
 * hostname of the initial namespace -- that is the whole point of the control
 * -- so a run that exits without putting it back leaves global state altered
 * for whatever the boot harness runs next. Restoring it is what keeps this
 * test re-runnable and keeps its bit0 assertion honest.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>

/* Raw CLONE_NEW* values, spelled out rather than trusted from headers so the
 * test asserts the ABI numbers the kernel is supposed to implement. */
#define NS_NEWNS      0x00020000
#define NS_NEWCGROUP  0x02000000
#define NS_NEWUTS     0x04000000
#define NS_NEWIPC     0x08000000
#define NS_NEWUSER    0x10000000
#define NS_NEWPID     0x20000000
#define NS_NEWNET     0x40000000

#define IPC_CREAT     01000
#define SHM_KEY       0x10510008
#define SHM_SIZE      4096

/* The initial UTS namespace's inode number in a real Linux kernel
 * (PROC_UTS_INIT_INO). Asserted, not merely printed. */
#define INIT_UTS_INUM 4026531838ull

/* Linux __NEW_UTS_LEN + 1. */
#define UTS_LEN 65

/* ---- thin syscall wrappers: always go to the KERNEL, never a libc cache --- */

static int ns_uname(struct utsname *u) {
    return (int)syscall(SYS_uname, u);
}
static int ns_sethostname(const char *s) {
    return (int)syscall(SYS_sethostname, s, strlen(s));
}
static int ns_setdomainname(const char *s) {
    return (int)syscall(SYS_setdomainname, s, strlen(s));
}
static int ns_unshare(int flags) {
    return (int)syscall(SYS_unshare, flags);
}
static int ns_setns(int fd, int nstype) {
    return (int)syscall(SYS_setns, fd, nstype);
}
static long ns_shmget(int key, size_t sz, int flg) {
    return syscall(SYS_shmget, key, sz, flg);
}

/* The current hostname, straight out of uname(2). */
static int hostname_is(const char *want) {
    struct utsname u;
    memset(&u, 0, sizeof u);
    if (ns_uname(&u) != 0) return 0;
    return strcmp(u.nodename, want) == 0;
}

static int domainname_is(const char *want) {
    struct utsname u;
    memset(&u, 0, sizeof u);
    if (ns_uname(&u) != 0) return 0;
#ifdef _GNU_SOURCE
    return strcmp(u.domainname, want) == 0;
#else
    return strcmp(u.__domainname, want) == 0;
#endif
}

static void hostname_show(const char *tag) {
    struct utsname u;
    memset(&u, 0, sizeof u);
    if (ns_uname(&u) == 0) printf("ns: %s nodename='%s'\n", tag, u.nodename);
    else                   printf("ns: %s uname failed errno=%d\n", tag, errno);
}

/* Read /proc/<pid>/ns/<kind> and return the inode number out of
 * "<kind>:[<inum>]". 0 means "could not read it", which every caller treats as
 * a failure rather than as a match. */
static unsigned long long ns_inum(const char *pid, const char *kind) {
    char path[128], link[128];
    snprintf(path, sizeof path, "/proc/%s/ns/%s", pid, kind);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) { printf("ns: readlink(%s) failed errno=%d\n", path, errno); return 0; }
    link[n] = '\0';
    /* Require the exact "<kind>:[" prefix: a target that does not have the
     * Linux shape is a failure, not something to parse loosely out of. */
    char want[32];
    snprintf(want, sizeof want, "%s:[", kind);
    size_t wl = strlen(want);
    if (strncmp(link, want, wl) != 0) {
        printf("ns: %s has unexpected form '%s'\n", path, link);
        return 0;
    }
    char *end = strchr(link + wl, ']');
    if (!end || end[1] != '\0') {
        printf("ns: %s unterminated '%s'\n", path, link);
        return 0;
    }
    *end = '\0';
    return strtoull(link + wl, NULL, 10);
}

/* Run `fn` in a forked child and return its exit status (or -1). The child's
 * status is the ONLY channel: each child returns 0 for success and a distinct
 * small number naming which of its own sub-checks failed, so a failure points
 * at a line rather than just at a bit. */
static int in_child(int (*fn)(void *), void *arg) {
    pid_t pid = fork();
    if (pid < 0) { printf("ns: fork failed errno=%d\n", errno); return -1; }
    if (pid == 0) _exit(fn(arg));
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

/* ---- bit2: unshare(CLONE_NEWUTS) isolates -------------------------------- */

struct uts_case { unsigned long long parent_inum; const char *parent_host; };

static int child_unshare_uts(void *arg) {
    struct uts_case *c = arg;
    if (ns_unshare(NS_NEWUTS) != 0) { printf("ns: child unshare failed errno=%d\n", errno); return 1; }
    /* POSITIVE half: the new namespace inherited the hostname (Linux copies
     * it) and then took our change. */
    if (!hostname_is(c->parent_host))  return 2;   /* not inherited */
    if (ns_sethostname("child-host") != 0) return 3;
    if (!hostname_is("child-host"))    return 4;   /* our own write not visible */
    /* SECOND WITNESS: a different namespace must report a different inum. */
    unsigned long long mine = ns_inum("self", "uts");
    if (mine == 0)                   return 5;
    if (mine == c->parent_inum)       return 6;   /* same ns => not unshared */
    printf("ns: child in uts:[%llu] (parent uts:[%llu]) host='child-host'\n",
           mine, c->parent_inum);
    return 0;
}

/* ---- bit3: the CONTROL -- a plain fork SHARES the namespace -------------- */

static int child_share_uts(void *arg) {
    (void)arg;
    if (ns_sethostname("sibling-host") != 0) return 1;
    if (!hostname_is("sibling-host")) return 2;
    return 0;
}

/* ---- bit4: clone(CLONE_NEWUTS) ------------------------------------------- */

static int clone_newuts_child(struct uts_case *c) {
    /* Raw clone with a NULL stack behaves like fork here (the kernel maps a
     * non-CLONE_THREAD clone onto its fork path), which is what we want: this
     * exercises the flags-applied-inside-fork route rather than unshare. */
    long pid = syscall(SYS_clone, (long)(NS_NEWUTS | SIGCHLD),
                       (long)0, (long)0, (long)0, (long)0);
    if (pid < 0) { printf("ns: clone(NEWUTS) failed errno=%d\n", errno); return -1; }
    if (pid == 0) {
        int rc = 0;
        if (!hostname_is(c->parent_host))          rc = 2;
        else if (ns_sethostname("clone-host") != 0) rc = 3;
        else if (!hostname_is("clone-host"))       rc = 4;
        else {
            unsigned long long mine = ns_inum("self", "uts");
            if (mine == 0)                  rc = 5;
            else if (mine == c->parent_inum) rc = 6;
        }
        _exit(rc);
    }
    int st = 0;
    if (waitpid((pid_t)pid, &st, 0) != (pid_t)pid) return -1;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

/* ---- bit5: setns round-trip ---------------------------------------------- */

struct setns_case { int fd; unsigned long long parent_inum; const char *parent_host; };

static int child_setns(void *arg) {
    struct setns_case *c = arg;
    if (ns_unshare(NS_NEWUTS) != 0) return 1;
    if (ns_sethostname("temp-host") != 0) return 2;
    if (!hostname_is("temp-host")) return 3;
    unsigned long long tmp = ns_inum("self", "uts");
    if (tmp == 0 || tmp == c->parent_inum) return 4;
    /* Now go back in. This is the check that cannot be faked by a no-op: the
     * hostname has to REVERT to the parent's, which nothing in this process
     * ever wrote. */
    if (ns_setns(c->fd, NS_NEWUTS) != 0) {
        printf("ns: setns failed errno=%d\n", errno);
        return 5;
    }
    if (!hostname_is(c->parent_host)) return 6;
    if (ns_inum("self", "uts") != c->parent_inum) return 7;
    printf("ns: setns round-trip OK (uts:[%llu] -> uts:[%llu], host='%s')\n",
           tmp, c->parent_inum, c->parent_host);
    return 0;
}

/* ---- bit6: IPC ----------------------------------------------------------- */

static int child_ipc_unshared(void *arg) {
    (void)arg;
    if (ns_unshare(NS_NEWIPC) != 0) { printf("ns: unshare(NEWIPC) errno=%d\n", errno); return 1; }
    /* No IPC_CREAT: a lookup. In a fresh IPC namespace the parent's segment
     * must not exist. Note that "the call failed" is not enough -- it has to
     * fail with ENOENT specifically, or we would also accept a broken shmget
     * that fails for some unrelated reason. */
    errno = 0;
    long id = ns_shmget(SHM_KEY, 0, 0);
    if (id >= 0) { printf("ns: LEAK -- unshared child SAW shmid=%ld\n", id); return 2; }
    if (errno != ENOENT) { printf("ns: unshared child got errno=%d, want ENOENT\n", errno); return 3; }
    unsigned long long mine = ns_inum("self", "ipc");
    if (mine == 0) return 4;
    printf("ns: unshared child ipc:[%llu] cannot see key 0x%x (ENOENT)\n",
           mine, SHM_KEY);
    return 0;
}

static int child_ipc_control(void *arg) {
    long want = *(long *)arg;
    errno = 0;
    long id = ns_shmget(SHM_KEY, 0, 0);
    if (id < 0) { printf("ns: CONTROL child could not see the segment errno=%d\n", errno); return 1; }
    if (id != want) { printf("ns: CONTROL child saw shmid=%ld want %ld\n", id, want); return 2; }
    printf("ns: control child (no unshare) sees shmid=%ld -- sharing works\n", id);
    return 0;
}

/* ------------------------------------------------------------------------- */

int main(void) {
    int bits = 0;

    /* Unbuffered, because this test's diagnostics come from FORKED CHILDREN
     * that leave via _exit(). glibc fully buffers stdout when it is not a tty,
     * so a child's printf would sit in its private copy of the buffer and be
     * discarded -- losing exactly the output that says WHICH sub-check failed.
     * Also keeps the ordering sane against the kernel's own serial writes. */
    setvbuf(stdout, NULL, _IONBF, 0);

    hostname_show("start");

    /* Remember the hostname we were handed so bit7 can put it back. Captured
     * from the kernel, not assumed to be "tobyos", so the restore is correct
     * even if something earlier in the boot changed it. */
    char start_host[UTS_LEN];
    {
        struct utsname u;
        memset(&u, 0, sizeof u);
        if (ns_uname(&u) != 0) { printf("ns: cannot read uname; aborting\n"); return 0; }
        snprintf(start_host, sizeof start_host, "%s", u.nodename);
    }

    /* ---- bit0: baseline ---- */
    {
        unsigned long long u = ns_inum("self", "uts");
        unsigned long long i = ns_inum("self", "ipc");
        printf("ns: initial uts:[%llu] ipc:[%llu]\n", u, i);
        if (hostname_is("tobyos") && u == INIT_UTS_INUM && i != 0)
            bits |= 1;
        else
            printf("ns: baseline FAILED (want nodename=tobyos uts=%llu)\n",
                   INIT_UTS_INUM);
    }

    /* ---- bit1: sethostname is REAL in the initial namespace ----
     * Deliberately first: if this fails, every "the parent did not change"
     * assertion below would pass for the wrong reason, so the whole test would
     * be meaningless. Checking it up front is what stops that. */
    if (ns_sethostname("lxns-host") == 0 && hostname_is("lxns-host")) {
        bits |= 2;
        printf("ns: sethostname in initial ns OK\n");
    } else {
        printf("ns: sethostname in initial ns FAILED errno=%d\n", errno);
    }

    unsigned long long parent_uts = ns_inum("self", "uts");

    /* ---- bit2: unshare isolates (positive + negative) ---- */
    {
        struct uts_case c = { parent_uts, "lxns-host" };
        int rc = in_child(child_unshare_uts, &c);
        int parent_untouched = hostname_is("lxns-host");
        if (rc == 0 && parent_untouched) {
            bits |= 4;
            printf("ns: unshare(NEWUTS) isolates; parent still 'lxns-host'\n");
        } else {
            printf("ns: unshare isolation FAILED child_rc=%d parent_untouched=%d\n",
                   rc, parent_untouched);
        }
    }

    /* ---- bit3: CONTROL -- without unshare the change DOES propagate ----
     * This is the check that makes bit2 attributable to unshare rather than to
     * fork. It intentionally clobbers the parent's hostname, then restores it. */
    {
        int rc = in_child(child_share_uts, NULL);
        int propagated = hostname_is("sibling-host");
        if (rc == 0 && propagated) {
            bits |= 8;
            printf("ns: control OK -- shared ns propagated 'sibling-host'\n");
        } else {
            printf("ns: control FAILED rc=%d propagated=%d (isolation in bit2 "
                   "may be an artifact of fork, not of unshare)\n", rc, propagated);
        }
        if (ns_sethostname("lxns-host") != 0 || !hostname_is("lxns-host"))
            printf("ns: WARNING could not restore hostname\n");
    }

    /* ---- bit4: clone(CLONE_NEWUTS) ---- */
    {
        struct uts_case c = { parent_uts, "lxns-host" };
        int rc = clone_newuts_child(&c);
        int parent_untouched = hostname_is("lxns-host");
        if (rc == 0 && parent_untouched) {
            bits |= 16;
            printf("ns: clone(NEWUTS) isolates; parent still 'lxns-host'\n");
        } else {
            printf("ns: clone(NEWUTS) FAILED rc=%d parent_untouched=%d\n",
                   rc, parent_untouched);
        }
    }

    /* ---- bit5: setns round-trip ----
     * Deliberately opened by NUMERIC PID rather than "self". /proc/<pid>/ns
     * and /proc/self/ns are separate branches in the kernel's open path, and
     * the numeric one is what `nsenter -t PID` and every container runtime
     * actually uses -- yet every other check here goes through "self", so
     * without this the numeric branch would ship untested. The readlink
     * comparison below asserts the two agree. */
    {
        char selfpath[64];
        snprintf(selfpath, sizeof selfpath, "/proc/%d/ns/uts", (int)getpid());

        char by_num[16];
        snprintf(by_num, sizeof by_num, "%d", (int)getpid());
        unsigned long long num_inum = ns_inum(by_num, "uts");
        if (num_inum != parent_uts) {
            printf("ns: /proc/%s/ns/uts says %llu but /proc/self says %llu\n",
                   by_num, num_inum, parent_uts);
        }

        int fd = open(selfpath, O_RDONLY);
        if (fd < 0) {
            printf("ns: open(%s) failed errno=%d\n", selfpath, errno);
        } else if (num_inum != parent_uts) {
            close(fd);   /* the by-number path disagreed; do not claim bit5 */
        } else {
            printf("ns: opened %s by NUMERIC pid (inum matches self)\n", selfpath);
            struct setns_case c = { fd, parent_uts, "lxns-host" };
            int rc = in_child(child_setns, &c);
            if (rc == 0 && hostname_is("lxns-host")) bits |= 32;
            else printf("ns: setns round-trip FAILED rc=%d\n", rc);
            close(fd);
        }
    }

    /* ---- bit6: IPC isolation + control ---- */
    {
        errno = 0;
        long id = ns_shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0600);
        if (id < 0) {
            printf("ns: shmget(create) failed errno=%d\n", errno);
        } else {
            printf("ns: parent created shmid=%ld key=0x%x\n", id, SHM_KEY);
            int iso  = in_child(child_ipc_unshared, NULL);
            int ctrl = in_child(child_ipc_control, &id);
            if (iso == 0 && ctrl == 0) {
                bits |= 64;
                printf("ns: IPC namespace isolates (and sharing still works)\n");
            } else {
                printf("ns: IPC FAILED isolated_rc=%d control_rc=%d\n", iso, ctrl);
            }
        }
    }

    /* ---- bit7: honest refusals + the /proc/PID/ns listing ----
     * A kernel that ACCEPTED CLONE_NEWNET and did nothing would look better
     * than one that refuses, right up until something relied on the network
     * being isolated. So the refusal is a required behaviour, asserted here. */
    {
        int ok = 1;

        /* Only namespaces this kernel genuinely does not implement belong here.
         * NS_NEWPID used to be in this list and slice 10 implemented it, so the
         * assertion started failing -- correctly: it had encoded "we do not
         * support pid namespaces" as a REQUIREMENT. That is the gate working,
         * not the gate being wrong, and the fix is to move the flag out rather
         * than to weaken the check. Whatever lands next (9 = NEWNS,
         * 11 = NEWUSER, 12 = NEWNET) must do the same. */
        /* CLONE_NEWCGROUP is the ONLY namespace flag still unimplemented, so it
         * is the only honest refusal left to assert. NEWNET moved out of this
         * list when slice 12 landed -- the FOURTH time this list went stale, and
         * the one I had explicitly predicted in slice 11's notes and still
         * shipped. If a future slice implements cgroup namespaces, this whole
         * block goes away and the positive list below grows instead. */
        errno = 0;
        if (ns_unshare(NS_NEWCGROUP) != -1 || errno != EINVAL) {
            printf("ns: unshare(NEWCGROUP) should be EINVAL, got errno=%d\n", errno);
            ok = 0;
        }
        /* NS_NEWUSER used to be asserted as EINVAL here and slice 11 implemented
         * it. Note what that cost: the call SUCCEEDED, so besides reporting the
         * wrong verdict it left THIS process inside a user namespace, which then
         * made the hostname restore below fail too. A stale refusal assertion is
         * not merely a wrong answer -- it can have side effects that corrupt
         * later checks. Any refusal test that runs in the parent should be
         * treated as suspect for exactly this reason; the positive assertions
         * below all run in children. */
        /* CLONE_NEWPID (slice 10) and CLONE_NEWNS (slice 9) are now SUPPORTED --
         * assert that POSITIVELY, so this test notices a regression to a
         * refusal just as it noticed the transition away from one. Both run in a
         * child: unshare(CLONE_NEWPID) permanently changes where this process's
         * children go, and unshare(CLONE_NEWNS) would give it a private mount
         * table for the rest of the test. */
        {
            static const struct { int flag; const char *name; } sup[] = {
                { NS_NEWPID, "NEWPID" },   /* slice 10 */
                { NS_NEWNS,  "NEWNS"  },   /* slice 9  */
                { NS_NEWUSER,"NEWUSER"},   /* slice 11 */
                { NS_NEWNET, "NEWNET" },   /* slice 12 */
            };
            for (int i = 0; i < 4; i++) {
                pid_t k = fork();
                if (k == 0) _exit(ns_unshare(sup[i].flag) == 0 ? 0 : 1);
                int st = 0;
                if (k < 0 || waitpid(k, &st, 0) != k ||
                    !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                    printf("ns: unshare(%s) should now SUCCEED\n", sup[i].name);
                    ok = 0;
                }
            }
        }
        /* setns on a descriptor that is not a namespace: EINVAL, not EBADF --
         * the fd is fine, it just is not a namespace. */
        errno = 0;
        if (ns_setns(1, 0) != -1 || errno != EINVAL) {
            printf("ns: setns(stdout) should be EINVAL, errno=%d\n", errno);
            ok = 0;
        }
        /* unshare(0) is a legal no-op on Linux. */
        if (ns_unshare(0) != 0) { printf("ns: unshare(0) should succeed\n"); ok = 0; }

        /* setdomainname(2) is the UTS namespace's OTHER field and a syscall
         * arm added by this slice, so it gets asserted rather than assumed
         * correct on the grounds that it shares a helper with sethostname.
         * "(none)" is what uname has always reported, and what it must go back
         * to -- an untested write path is how a no-op stub survives. */
        if (!domainname_is("(none)")) {
            printf("ns: initial domainname is not '(none)'\n"); ok = 0;
        } else if (ns_setdomainname("nsdom") != 0 || !domainname_is("nsdom")) {
            printf("ns: setdomainname failed errno=%d\n", errno); ok = 0;
        } else if (ns_setdomainname("(none)") != 0 || !domainname_is("(none)")) {
            printf("ns: could not restore domainname\n"); ok = 0;
        } else {
            printf("ns: setdomainname round-trip OK\n");
        }

        /* Every kind must be listed and every link must parse. */
        static const char *kinds[] = { "ipc", "mnt", "net", "pid", "user", "uts" };
        for (int k = 0; k < 6; k++) {
            if (ns_inum("self", kinds[k]) == 0) {
                printf("ns: /proc/self/ns/%s missing or malformed\n", kinds[k]);
                ok = 0;
            }
        }
        /* Put the machine hostname back. This test really did change it (bit1
         * and bit3 depend on doing so), so leaving it changed would corrupt
         * state for whatever runs next in the same boot. */
        if (ns_sethostname(start_host) != 0 || !hostname_is(start_host)) {
            printf("ns: FAILED to restore hostname to '%s' errno=%d\n",
                   start_host, errno);
            ok = 0;
        }

        if (ok) {
            bits |= 128;
            printf("ns: refusals honest (EINVAL), 6 ns links present, "
                   "hostname restored to '%s'\n", start_host);
        }
    }

    hostname_show("end");
    printf("ns: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
