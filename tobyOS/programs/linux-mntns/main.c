/* linux-mntns -- Phase 3 slice 9 acceptance test: MOUNT namespaces.
 *
 * ===========================================================================
 * EVERY MOUNT CHECK HERE IS FUNCTIONAL, NOT TABLE-INSPECTION
 *
 * The lazy way to test a mount is to read /proc/mounts and look for a line.
 * That passes for a mount table entry that resolves to nothing -- and this
 * kernel's mount(2) is exactly a table insertion, so a broken bind would still
 * produce the line. So every check asks the question that matters: **does a
 * marker file created through one path become visible through the other?**
 *
 * And, as in slices 8 and 10, each isolation claim is asserted three ways:
 *   positive : the namespace that mounted sees the mount
 *   negative : the namespace that did not does NOT see it
 *   control  : the same mount WITHOUT unshare *is* visible to the parent
 *
 * The control is what makes the result attributable to CLONE_NEWNS rather than
 * to fork, or to the mount having silently failed in both places.
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  MS_SLAVE, both directions: a slave RECEIVES its master's mounts and
 *         does NOT send its own back. (The baseline inum check moved to bit7 to
 *         make room -- slave is the propagation type that would otherwise ship
 *         implemented but unexercised.)
 *   bit1  mount isolation: a bind in a new ns is invisible outside it
 *         (+ control: without unshare it IS visible)
 *   bit2  umount isolation: umount /data in a new ns does not unmount it
 *         for anyone else
 *   bit3  PROPAGATION: with / shared, a mount made in the child APPEARS in the
 *         parent -- the half that a stored-and-ignored MS_SHARED cannot fake
 *   bit4  PROPAGATION CONTROL: with / private, the same mount does NOT appear
 *   bit5  MS_UNBINDABLE is enforced (bind refused), and works again once the
 *         mount is made private
 *   bit6  pivot_root: the new root takes effect AND the old root MOVES (it is
 *         reachable at put_old and no longer at /) -- the property chroot lacks
 *   bit7  honest refusals + a namespaced child's mnt inum differs + the
 *         parent's mount table is restored to its baseline
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
#include <sys/stat.h>
#include <sys/wait.h>

#define NS_NEWNS   0x00020000

#define MS_BIND        0x1000u
#define MS_UNBINDABLE  (1u << 17)
#define MS_PRIVATE     (1u << 18)
#define MS_SLAVE       (1u << 19)
#define MS_SHARED      (1u << 20)

/* vfs.c stamps the initial mount namespace with this (Linux has no fixed
 * constant for it -- see nsproxy.h). */
#define INIT_MNT_INUM 4026531840ull

#define PROBE "mntns-probe"          /* marker file created in /data */

static int k_mount(const char *src, const char *tgt, const char *fs,
                   unsigned long flags) {
    return (int)syscall(SYS_mount, src, tgt, fs, flags, (void *)0);
}
static int k_umount(const char *tgt) {
    return (int)syscall(SYS_umount2, tgt, 0);
}
static int k_unshare(int flags)  { return (int)syscall(SYS_unshare, flags); }
static int k_pivot(const char *nr, const char *po) {
    return (int)syscall(SYS_pivot_root, nr, po);
}

static unsigned long long ns_inum(const char *kind) {
    char path[128], link[128], want[32];
    snprintf(path, sizeof path, "/proc/self/ns/%s", kind);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) { printf("mntns: readlink(%s) errno=%d\n", path, errno); return 0; }
    link[n] = '\0';
    snprintf(want, sizeof want, "%s:[", kind);
    size_t wl = strlen(want);
    if (strncmp(link, want, wl) != 0) {
        printf("mntns: %s malformed '%s'\n", path, link); return 0;
    }
    char *e = strchr(link + wl, ']');
    if (!e) return 0;
    *e = '\0';
    return strtoull(link + wl, NULL, 10);
}

/* THE functional test: is the marker visible through `dir`? */
static int probe_visible(const char *dir) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, PROBE);
    return access(p, F_OK) == 0;
}

static int count_mounts(void) {
    int fd = open("/proc/mounts", O_RDONLY);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    int lines = 0;
    for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    return lines;
}

static int reap(pid_t p) {
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);   /* children _exit(); see slice 8 */

    unsigned long long my_inum = ns_inum("mnt");
    int base_mounts = count_mounts();
    printf("mntns: start mnt:[%llu] mounts=%d\n", my_inum, base_mounts);

    /* Create the marker inside /data (the writable tobyfs volume). Everything
     * below asks whether THIS file is reachable through some other path. */
    {
        char p[256];
        snprintf(p, sizeof p, "/data/%s", PROBE);
        int fd = open(p, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) { printf("mntns: cannot create %s errno=%d\n", p, errno); return 0; }
        (void)!write(fd, "x", 1);
        close(fd);
        if (!probe_visible("/data")) { printf("mntns: marker unusable\n"); return 0; }
    }

    /* ---- bit0: MS_SLAVE, BOTH directions ----
     * A slave receives events from its master and sends none back. Testing only
     * one direction would pass for a "slave" that is really just private (no
     * receive) or really just shared (no containment), so both are asserted.
     *
     * Needs two LIVE namespaces at once, so it uses a pipe handshake rather
     * than sleeps -- deterministic under TCG, same reason as linux-pidns bit4. */
    {
        int up[2], down[2];
        if (k_mount(NULL, "/", NULL, MS_SHARED) != 0) {
            printf("mntns: bit0 make-shared / failed errno=%d\n", errno);
        } else if (pipe(up) != 0 || pipe(down) != 0) {
            printf("mntns: bit0 pipe failed errno=%d\n", errno);
        } else {
            pid_t c = fork();
            if (c == 0) {
                close(up[0]); close(down[1]);
                char b;
                if (k_unshare(NS_NEWNS) != 0) _exit(1);
                /* Peer of the parent's / at this point; now demote to slave. */
                if (k_mount(NULL, "/", NULL, MS_SLAVE) != 0) _exit(2);
                if (write(up[1], "R", 1) != 1) _exit(3);
                if (read(down[0], &b, 1) != 1) _exit(4);
                /* RECEIVE half: the master mounted while we waited. */
                if (!probe_visible("/slv_in")) _exit(5);
                /* SEND half: our own mount must not travel back up. */
                if (k_mount("/data", "/slv_out", NULL, MS_BIND) != 0) _exit(6);
                if (write(up[1], "M", 1) != 1) _exit(7);
                if (read(down[0], &b, 1) != 1) _exit(8);
                _exit(0);
            }
            close(up[1]); close(down[0]);
            char b;
            int ok = (read(up[0], &b, 1) == 1);
            /* Mount in the MASTER; it must reach the slave. */
            if (ok && k_mount("/data", "/slv_in", NULL, MS_BIND) != 0) {
                printf("mntns: bit0 master mount failed errno=%d\n", errno);
                ok = 0;
            }
            (void)!write(down[1], "G", 1);
            if (ok && read(up[0], &b, 1) != 1) ok = 0;
            /* The slave's own mount must NOT have arrived here. */
            int leaked_up = probe_visible("/slv_out");
            (void)!write(down[1], "G", 1);
            int rc = reap(c);
            close(up[0]); close(down[1]);

            if (rc == 0 && ok && !leaked_up) {
                bits |= 1;
                printf("mntns: MS_SLAVE -- received the master's mount, did not "
                       "send its own back\n");
            } else {
                printf("mntns: bit0 FAILED rc=%d ok=%d leaked_up=%d\n",
                       rc, ok, leaked_up);
            }
            (void)k_umount("/slv_in");
            if (leaked_up) (void)k_umount("/slv_out");
            (void)k_mount(NULL, "/", NULL, MS_PRIVATE);   /* restore */
        }
    }

    /* ---- bit1: mount isolation, with a control ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNS) != 0) _exit(1);
            if (ns_inum("mnt") == my_inum) _exit(2);       /* must be a new ns */
            if (k_mount("/data", "/nsmnt", NULL, MS_BIND) != 0) _exit(3);
            /* POSITIVE, and functional: the bind must really resolve. */
            if (!probe_visible("/nsmnt")) _exit(4);
            _exit(0);
        }
        int rc = reap(c);
        /* NEGATIVE: the parent must not see it. */
        int leaked = probe_visible("/nsmnt");

        /* CONTROL: the same bind without unshare must reach the parent. */
        pid_t d = fork();
        if (d == 0) {
            if (k_mount("/data", "/nsctl", NULL, MS_BIND) != 0) _exit(1);
            _exit(probe_visible("/nsctl") ? 0 : 2);
        }
        int drc = reap(d);
        int shared_ok = probe_visible("/nsctl");

        if (rc == 0 && !leaked && drc == 0 && shared_ok) {
            bits |= 2;
            printf("mntns: bind in new ns invisible outside; same bind without "
                   "unshare IS visible -> isolation is due to CLONE_NEWNS\n");
        } else {
            printf("mntns: bit1 FAILED rc=%d leaked=%d ctl=%d shared=%d\n",
                   rc, leaked, drc, shared_ok);
        }
        (void)k_umount("/nsctl");            /* hygiene */
    }

    /* ---- bit2: umount isolation ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNS) != 0) _exit(1);
            if (k_umount("/data") != 0) _exit(2);
            /* /data is gone HERE... */
            if (probe_visible("/data")) _exit(3);
            _exit(0);
        }
        int rc = reap(c);
        /* ...but must still be mounted for everyone else. */
        int still = probe_visible("/data");
        if (rc == 0 && still) {
            bits |= 4;
            printf("mntns: umount /data inside a ns did not unmount it outside\n");
        } else {
            printf("mntns: bit2 FAILED rc=%d parent_still_has_data=%d\n", rc, still);
        }
    }

    /* ---- bit3: PROPAGATION (shared) ----
     * The check MS_SHARED cannot fake: a mount performed in the CHILD's
     * namespace has to appear in the PARENT's, because their roots are peers. */
    {
        if (k_mount(NULL, "/", NULL, MS_SHARED) != 0) {
            printf("mntns: make-shared / failed errno=%d\n", errno);
        } else {
            pid_t c = fork();
            if (c == 0) {
                if (k_unshare(NS_NEWNS) != 0) _exit(1);
                if (k_mount("/data", "/nsprop", NULL, MS_BIND) != 0) _exit(2);
                _exit(probe_visible("/nsprop") ? 0 : 3);
            }
            int rc = reap(c);
            int propagated = probe_visible("/nsprop");
            if (rc == 0 && propagated) {
                bits |= 8;
                printf("mntns: PROPAGATION -- child's mount appeared in the "
                       "parent (peer roots)\n");
            } else {
                printf("mntns: bit3 FAILED rc=%d propagated=%d\n", rc, propagated);
            }
            (void)k_umount("/nsprop");
        }
    }

    /* ---- bit4: PROPAGATION CONTROL (private) ----
     * Same operation with / private must NOT reach the parent. Without this,
     * bit3 could be leakage rather than propagation. */
    {
        if (k_mount(NULL, "/", NULL, MS_PRIVATE) != 0) {
            printf("mntns: make-private / failed errno=%d\n", errno);
        } else {
            pid_t c = fork();
            if (c == 0) {
                if (k_unshare(NS_NEWNS) != 0) _exit(1);
                if (k_mount("/data", "/nspriv", NULL, MS_BIND) != 0) _exit(2);
                _exit(probe_visible("/nspriv") ? 0 : 3);
            }
            int rc = reap(c);
            int leaked = probe_visible("/nspriv");
            if (rc == 0 && !leaked) {
                bits |= 16;
                printf("mntns: private / did NOT propagate -- bit3 was real "
                       "propagation, not leakage\n");
            } else {
                printf("mntns: bit4 FAILED rc=%d leaked=%d\n", rc, leaked);
            }
            if (leaked) (void)k_umount("/nspriv");
        }
    }

    /* ---- bit5: MS_UNBINDABLE is enforced, both ways ---- */
    {
        int ok = 1;
        if (k_mount(NULL, "/data", NULL, MS_UNBINDABLE) != 0) {
            printf("mntns: make-unbindable /data failed errno=%d\n", errno); ok = 0;
        }
        errno = 0;
        if (k_mount("/data", "/nsub", NULL, MS_BIND) != -1 || errno != EINVAL) {
            printf("mntns: bind of an UNBINDABLE mount should be EINVAL "
                   "(errno=%d)\n", errno);
            ok = 0;
            (void)k_umount("/nsub");
        }
        /* And it must work again once the mount is private -- otherwise "bind
         * refused" could just mean bind is broken. */
        if (k_mount(NULL, "/data", NULL, MS_PRIVATE) != 0) ok = 0;
        if (k_mount("/data", "/nsub", NULL, MS_BIND) != 0 ||
            !probe_visible("/nsub")) {
            printf("mntns: bind should work again after make-private\n"); ok = 0;
        }
        (void)k_umount("/nsub");
        if (ok) {
            bits |= 32;
            printf("mntns: MS_UNBINDABLE refuses binds; private allows them\n");
        }
    }

    /* ---- bit6: pivot_root really MOVES the root ---- */
    {
        pid_t c = fork();
        if (c == 0) {
            if (k_unshare(NS_NEWNS) != 0) _exit(1);
            if (k_mount("/data", "/newroot", NULL, MS_BIND) != 0) _exit(2);
            /* put_old must live under new_root, as Linux requires. */
            if (k_pivot("/newroot", "/newroot/oldroot") != 0) {
                printf("mntns: pivot_root errno=%d\n", errno);
                _exit(3);
            }
            /* The new root is in effect: the marker was /data/PROBE and /data
             * is now /. */
            if (!probe_visible("")) _exit(4);
            /* The old root MOVED rather than vanished: it is at put_old... */
            if (access("/oldroot/bin/busybox", F_OK) != 0) _exit(5);
            /* ...and is NO LONGER at /. This is the property chroot cannot
             * give: the old tree is not merely hidden, it is somewhere else. */
            if (access("/bin/busybox", F_OK) == 0) _exit(6);
            _exit(0);
        }
        int rc = reap(c);
        /* The parent must be entirely unaffected. */
        int parent_intact = (access("/bin/busybox", F_OK) == 0) &&
                            probe_visible("/data");
        if (rc == 0 && parent_intact) {
            bits |= 64;
            printf("mntns: pivot_root -- new root live, old root at /oldroot and "
                   "GONE from /, parent unaffected\n");
        } else {
            printf("mntns: bit6 FAILED rc=%d parent_intact=%d\n", rc, parent_intact);
        }
    }

    /* ---- bit7: refusals, inum, and cleanliness ---- */
    {
        int ok = 1;

        /* Baseline inum (moved here from bit0 to make room for MS_SLAVE). */
        if (my_inum != INIT_MNT_INUM) {
            printf("mntns: initial mnt inum %llu, want %llu\n",
                   my_inum, INIT_MNT_INUM);
            ok = 0;
        }
        if (base_mounts <= 0) { printf("mntns: no baseline mounts\n"); ok = 0; }

        /* pivot_root from the INITIAL mount namespace is refused. Rewriting the
         * running system's root with no way back is not something to offer. */
        errno = 0;
        if (k_pivot("/data", "/data/oldroot") != -1) {
            printf("mntns: pivot_root in the initial ns should be refused\n");
            ok = 0;
        }
        /* unshare(CLONE_NEWNS) must now SUCCEED -- slice 9 implements it, so
         * assert that positively (the mirror of the refusal checks in
         * linux-ns, and it will catch a regression to EINVAL). */
        {
            pid_t k = fork();
            if (k == 0) _exit(k_unshare(NS_NEWNS) == 0 ? 0 : 1);
            if (reap(k) != 0) {
                printf("mntns: unshare(NEWNS) should succeed (slice 9)\n");
                ok = 0;
            }
        }
        /* A namespaced child's inum must differ from ours. */
        {
            pid_t k = fork();
            if (k == 0) {
                if (k_unshare(NS_NEWNS) != 0) _exit(1);
                _exit(ns_inum("mnt") != my_inum ? 0 : 2);
            }
            if (reap(k) != 0) { printf("mntns: child mnt inum did not differ\n"); ok = 0; }
        }
        /* Our own namespace must be untouched by all of the above. */
        if (ns_inum("mnt") != my_inum) {
            printf("mntns: OUR mnt namespace changed -- it should not have\n");
            ok = 0;
        }
        /* Hygiene: the parent's mount table must be back to baseline, or this
         * test has left state that poisons whatever the harness runs next. */
        int now = count_mounts();
        if (now != base_mounts) {
            printf("mntns: mount count %d != baseline %d -- leftover mounts\n",
                   now, base_mounts);
            ok = 0;
        }
        if (ok) {
            bits |= 128;
            printf("mntns: refusals honest, inum per-ns, mount table restored "
                   "(%d)\n", now);
        }
    }

    { char p[256]; snprintf(p, sizeof p, "/data/%s", PROBE); (void)unlink(p); }
    printf("mntns: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
