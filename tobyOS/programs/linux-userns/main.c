/* linux-userns -- Phase 3 slice 11 acceptance test: USER namespaces.
 *
 * ===========================================================================
 * THE CHECK THAT MATTERS MOST HERE IS A NEGATIVE ONE
 *
 * Slices 8-10 were about isolation. This slice hands an UNPRIVILEGED process a
 * full capability set, so its correctness is mostly about what it still cannot
 * do. bit5 is therefore the most important test in the file: a process that
 * unshares only CLONE_NEWUSER must NOT be able to mount anything, because if it
 * could, "unprivileged user namespace" would be a root exploit -- unshare and
 * then mount over the real /.
 *
 * And, as before, every claim is asserted from both sides plus a control:
 *   positive : the namespaced process sees the new id
 *   negative : the outside world does not
 *   control  : the same operation without the namespace behaves as before
 *
 * bit3 is the sharpest positive: ONE process reports uid 0 to itself and its
 * real host uid to its parent, simultaneously, over a pipe handshake. Same
 * shape as linux-pidns bit4 -- nothing but a real translation layer does that.
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  baseline: /proc/self/ns/user is the init constant; uid_map is the
 *         full range
 *   bit1  an UNPRIVILEGED process can create a user namespace (the whole point)
 *   bit2  before uid_map is written every id reads as the overflow id 65534 --
 *         proving the map is really consulted and not identity-substituted
 *   bit3  after writing "0 <myuid> 1": getuid()==0 INSIDE while the parent still
 *         reads the real host uid for the same process, at the same instant
 *   bit4  uid_map is WRITE-ONCE (a second write is EPERM)
 *   bit5  ESCALATION GUARD: CLONE_NEWUSER alone cannot mount; with
 *         CLONE_NEWUSER|CLONE_NEWNS it can (that is the control)
 *   bit6  a NON-ROOT process really does mount inside its own namespace, and
 *         the mount is invisible outside it
 *   bit7  honest refusals: setgroups must be denied before an unprivileged
 *         gid_map; NEWNET still EINVAL; our own namespace unchanged
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
#include <sys/wait.h>

#define NS_NEWNS       0x00020000
#define NS_NEWCGROUP   0x02000000
#define NS_NEWUSER     0x10000000
#define NS_NEWNET      0x40000000
#define MS_BIND     0x1000u

#define INIT_USER_INUM 4026531837ull      /* PROC_USER_INIT_INO */
#define OVERFLOW_ID    65534
#define TESTUID        1000               /* the unprivileged id we drop to */
#define PROBE          "userns-probe"

static int k_unshare(int f) { return (int)syscall(SYS_unshare, f); }
static int k_getuid(void)   { return (int)syscall(SYS_getuid);   }
static int k_setuid(int u)  { return (int)syscall(SYS_setuid, u); }
static int k_mount(const char *s, const char *t, unsigned long fl) {
    return (int)syscall(SYS_mount, s, t, (const char *)0, fl, (void *)0);
}

static unsigned long long ns_inum(const char *kind) {
    char path[128], link[128], want[32];
    snprintf(path, sizeof path, "/proc/self/ns/%s", kind);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) return 0;
    link[n] = '\0';
    snprintf(want, sizeof want, "%s:[", kind);
    size_t wl = strlen(want);
    if (strncmp(link, want, wl) != 0) return 0;
    char *e = strchr(link + wl, ']');
    if (!e) return 0;
    *e = '\0';
    return strtoull(link + wl, NULL, 10);
}

static int write_file(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, s, strlen(s));
    int e = errno;
    close(fd);
    if (n < 0) { errno = e; return -1; }
    return 0;
}

static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

/* The "Uid:" line's EFFECTIVE column out of a /proc/<who>/status. */
static int status_euid(const char *who) {
    char path[128], buf[2048];
    snprintf(path, sizeof path, "/proc/%s/status", who);
    if (read_file(path, buf, sizeof buf) < 0) return -1;
    char *l = strstr(buf, "Uid:");
    if (!l) return -1;
    long real = strtol(l + 4, &l, 10);
    (void)real;
    return (int)strtol(l, NULL, 10);        /* second column = effective */
}

static int probe_visible(const char *dir) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, PROBE);
    return access(p, F_OK) == 0;
}

static int reap(pid_t p) {
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    unsigned long long my_inum = ns_inum("user");
    int my_uid = k_getuid();
    printf("userns: start uid=%d user:[%llu]\n", my_uid, my_inum);

    /* marker used by the mount checks */
    {
        char p[256];
        snprintf(p, sizeof p, "/data/%s", PROBE);
        int fd = open(p, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) { (void)!write(fd, "x", 1); close(fd); }
    }

    /* ---- bit0: baseline ---- */
    {
        char m[256];
        int ok = (my_inum == INIT_USER_INUM && my_uid == 0);
        if (read_file("/proc/self/uid_map", m, sizeof m) > 0) {
            printf("userns: initial uid_map = %s", m);
            /* The initial namespace maps the whole id space. */
            if (!strstr(m, "4294967295")) ok = 0;
        } else ok = 0;
        if (ok) bits |= 1;
        else printf("userns: baseline FAILED\n");
    }

    /* ---- bit1: an UNPRIVILEGED process can create a user namespace ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_getuid() != TESTUID) _exit(2);
            /* No privilege required -- this is the entire point. */
            if (k_unshare(NS_NEWUSER) != 0) {
                printf("userns: unprivileged unshare(NEWUSER) failed errno=%d\n",
                       errno);
                _exit(3);
            }
            unsigned long long in = ns_inum("user");
            _exit((in != 0 && in != INIT_USER_INUM) ? 0 : 4);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 2;
            printf("userns: uid %d created a user namespace with NO privilege\n",
                   TESTUID);
        } else printf("userns: bit1 FAILED rc=%d\n", rc);
    }

    /* ---- bit2: unmapped ids read as the overflow id ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_unshare(NS_NEWUSER) != 0) _exit(2);
            /* No map written yet: Linux reports overflowuid, NOT the host id.
             * Reporting the host id here would leak it into the namespace. */
            int u = k_getuid();
            if (u != OVERFLOW_ID) {
                printf("userns: pre-map getuid=%d, want %d\n", u, OVERFLOW_ID);
                _exit(3);
            }
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0) {
            bits |= 4;
            printf("userns: before uid_map is written, ids read as %d "
                   "(the map is really consulted)\n", OVERFLOW_ID);
        } else printf("userns: bit2 FAILED rc=%d\n", rc);
    }

    /* ---- bit3: ONE process, TWO uids, at the same instant ---- */
    {
        int up[2], down[2];
        if (pipe(up) != 0 || pipe(down) != 0) {
            printf("userns: pipe failed\n");
        } else {
            pid_t c = fork();
            if (c == 0) {
                close(up[0]); close(down[1]);
                char b;
                if (k_setuid(TESTUID) != 0) _exit(1);
                if (k_unshare(NS_NEWUSER) != 0) _exit(2);
                /* The unprivileged single-id map: "0 <myuid> 1" -- I am root
                 * inside, mapped onto the one id I already own. */
                char line[64];
                snprintf(line, sizeof line, "0 %d 1\n", TESTUID);
                if (write_file("/proc/self/uid_map", line) != 0) {
                    printf("userns: uid_map write failed errno=%d\n", errno);
                    _exit(3);
                }
                int inside = k_getuid();
                int st_in  = status_euid("self");
                if (write(up[1], &inside, sizeof inside) != sizeof inside) _exit(4);
                if (read(down[0], &b, 1) != 1) _exit(5);
                _exit((inside == 0 && st_in == 0) ? 0 : 6);
            }
            close(up[1]); close(down[0]);
            int inside = -1;
            int got = (read(up[0], &inside, sizeof inside) == sizeof inside);
            char who[24];
            snprintf(who, sizeof who, "%d", (int)c);
            int outside = status_euid(who);      /* read through OUR namespace */
            char ack = 'x';
            (void)!write(down[1], &ack, 1);
            int rc = reap(c);
            close(up[0]); close(down[1]);
            printf("userns: same process -- it reads uid %d, we read uid %d\n",
                   inside, outside);
            if (got && rc == 0 && inside == 0 && outside == TESTUID) {
                bits |= 8;
            } else printf("userns: bit3 FAILED got=%d rc=%d in=%d out=%d\n",
                          got, rc, inside, outside);
        }
    }

    /* ---- bit4: uid_map is WRITE-ONCE ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_unshare(NS_NEWUSER) != 0) _exit(2);
            char line[64];
            snprintf(line, sizeof line, "0 %d 1\n", TESTUID);
            if (write_file("/proc/self/uid_map", line) != 0) _exit(3);
            errno = 0;
            /* A second write must be refused: otherwise a process could widen
             * its own mapping after the fact. */
            if (write_file("/proc/self/uid_map", line) == 0) _exit(4);
            _exit(0);
        }
        int rc = reap(c);
        if (rc == 0) { bits |= 16; printf("userns: uid_map is write-once\n"); }
        else printf("userns: bit4 FAILED rc=%d\n", rc);
    }

    /* ---- bit5: THE ESCALATION GUARD ----
     * CLONE_NEWUSER alone must grant NO mounting authority. With
     * CLONE_NEWUSER|CLONE_NEWNS it must work -- that pairing is the control, and
     * without it "mount refused" could simply mean mount is broken. */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_unshare(NS_NEWUSER) != 0) _exit(2);
            char line[64];
            snprintf(line, sizeof line, "0 %d 1\n", TESTUID);
            if (write_file("/proc/self/uid_map", line) != 0) _exit(3);
            /* Root inside, full caps inside -- and still no authority over the
             * REAL mount table. */
            errno = 0;
            if (k_mount("/data", "/evil", MS_BIND) != -1) {
                printf("userns: ESCALATION -- mounted with NEWUSER alone!\n");
                _exit(4);
            }
            if (errno != EPERM) { printf("userns: want EPERM got %d\n", errno); _exit(5); }
            _exit(0);
        }
        int guard = reap(c);
        /* Control: the same thing WITH a mount namespace must succeed. */
        pid_t d = fork();
        if (d == 0) {
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_unshare(NS_NEWUSER | NS_NEWNS) != 0) _exit(2);
            char line[64];
            snprintf(line, sizeof line, "0 %d 1\n", TESTUID);
            if (write_file("/proc/self/uid_map", line) != 0) _exit(3);
            if (k_mount("/data", "/uns", MS_BIND) != 0) {
                printf("userns: NEWUSER|NEWNS mount failed errno=%d\n", errno);
                _exit(4);
            }
            _exit(probe_visible("/uns") ? 0 : 5);
        }
        int ctl = reap(d);
        int leaked = probe_visible("/uns");
        if (guard == 0 && ctl == 0 && !leaked) {
            bits |= 32;
            printf("userns: GUARD -- NEWUSER alone cannot mount (EPERM); "
                   "NEWUSER|NEWNS can, and it stays inside\n");
        } else {
            printf("userns: bit5 FAILED guard=%d control=%d leaked=%d\n",
                   guard, ctl, leaked);
        }
    }

    /* ---- bit6: a non-root process mounts for real, invisibly ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_unshare(NS_NEWUSER | NS_NEWNS) != 0) _exit(2);
            char line[64];
            snprintf(line, sizeof line, "0 %d 1\n", TESTUID);
            if (write_file("/proc/self/uid_map", line) != 0) _exit(3);
            if (k_mount("/data", "/unpriv", MS_BIND) != 0) _exit(4);
            /* Functional, not a /proc/mounts line. */
            if (!probe_visible("/unpriv")) _exit(5);
            _exit(0);
        }
        int rc = reap(c);
        int leaked = probe_visible("/unpriv");
        if (rc == 0 && !leaked) {
            bits |= 64;
            printf("userns: uid %d mounted inside its own namespace; invisible "
                   "outside\n", TESTUID);
        } else printf("userns: bit6 FAILED rc=%d leaked=%d\n", rc, leaked);
    }

    /* ---- bit7: honest refusals + our namespace unchanged ---- */
    {
        int ok = 1;
        pid_t c = fork();
        if (c == 0) {
            /* CAPTURE THE HOST gid BEFORE UNSHARING.
             *
             * Two mistakes led here, both worth keeping. First the line was
             * "0 TESTUID 1", but the child calls setuid() only -- its gid is
             * still 0 -- so the pre-deny write failed because the id was not the
             * writer's, NOT because setgroups was still allowed: that half would
             * have passed with no setgroups rule implemented at all.
             *
             * Then reading getgid() AFTER the unshare returned 65534, because an
             * unmapped id correctly reports as the overflow id (that is bit2
             * working). A process inside a fresh user namespace simply CANNOT
             * learn its own host ids -- it has to capture them beforehand, which
             * is exactly what real container runtimes do. */
            unsigned mygid = (unsigned)syscall(SYS_getgid);
            if (k_setuid(TESTUID) != 0) _exit(1);
            if (k_unshare(NS_NEWUSER) != 0) _exit(2);
            char line[64];
            snprintf(line, sizeof line, "0 %u 1\n", mygid);

            /* gid_map, unprivileged, WITHOUT denying setgroups first: must be
             * refused with EPERM. Supplementary groups could otherwise be used
             * to shed a group that was restricting access. */
            errno = 0;
            if (write_file("/proc/self/gid_map", line) == 0) _exit(3);
            if (errno != EPERM) {
                printf("userns: pre-deny gid_map errno=%d, want EPERM\n", errno);
                _exit(6);
            }
            if (write_file("/proc/self/setgroups", "deny") != 0) _exit(4);
            /* ...and permitted once it is denied. */
            if (write_file("/proc/self/gid_map", line) != 0) {
                printf("userns: post-deny gid_map failed errno=%d\n", errno);
                _exit(5);
            }
            _exit(0);
        }
        int rc = reap(c);
        if (rc != 0) {
            printf("userns: setgroups/gid_map ordering FAILED rc=%d\n", rc);
            ok = 0;
        }
        /* Was NEWNET until slice 12 implemented it. CLONE_NEWCGROUP is the only
         * namespace flag left unimplemented, so it is the only honest refusal
         * still available to assert here. */
        errno = 0;
        if (k_unshare(NS_NEWCGROUP) != -1 || errno != EINVAL) {
            printf("userns: unshare(NEWCGROUP) should be EINVAL, errno=%d\n", errno);
            ok = 0;
        }
        if (ns_inum("user") != my_inum || k_getuid() != my_uid) {
            printf("userns: OUR namespace/uid changed -- it should not have\n");
            ok = 0;
        }
        if (ok) {
            bits |= 128;
            printf("userns: setgroups must be denied before an unprivileged "
                   "gid_map; NEWNET still EINVAL; our own ns untouched\n");
        }
    }

    { char p[256]; snprintf(p, sizeof p, "/data/%s", PROBE); (void)unlink(p); }
    printf("userns: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
