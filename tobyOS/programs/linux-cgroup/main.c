/* linux-cgroup -- Phase 3 slice 15 acceptance test: cgroup v2 (pids + cpu).
 *
 * ===========================================================================
 * A CGROUP INTERFACE IS THE EASIEST THING IN THIS ARC TO FAKE
 *
 * The control files are just text. `pids.max` can accept a write and read back
 * perfectly while limiting nothing; `cpu.weight` can round-trip while changing
 * no scheduling decision. Both would satisfy a test that only checked the files.
 *
 * So neither is tested through its file:
 *   - pids.max is tested by whether fork() ACTUALLY FAILS at the limit, with
 *     EAGAIN, and succeeds again when the limit is raised;
 *   - cpu.weight is tested by MEASURING CPU time out of /proc/<pid>/stat.
 *
 * The plan's gate is exactly this: "pids.max enforced; cpu.weight produces
 * measured CPU skew".
 * ===========================================================================
 *
 * Each check sets a bit; all pass => exit 255.
 *
 *   bit0  cgroupfs is mounted and the root advertises both controllers
 *   bit1  mkdir creates a cgroup; the control files appear with v2 defaults
 *         ("max" / 100); rmdir removes it
 *   bit2  cgroup.procs moves a process, and pids.current reflects it
 *   bit3  pids.max ENFORCED: fork fails with EAGAIN at the limit...
 *   bit4  ...and succeeds again once the limit is raised (the control -- without
 *         it, "fork failed" could mean the table was simply full)
 *   bit5  the limit is HIERARCHICAL: a child cgroup cannot escape its parent's
 *         cap by raising its own pids.max
 *   bit6  cpu.weight produces MEASURED CPU skew between two contended cgroups
 *   bit7  honest refusals + hygiene: a populated cgroup will not rmdir, a
 *         control file will not unlink, out-of-range weights are refused, and
 *         the test leaves the hierarchy as it found it
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>

#define CG "/sys/fs/cgroup"

static int wr(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    int e = errno;
    close(fd);
    if (n < 0) { errno = e; return -1; }
    return 0;
}

static int rd(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    /* strip a trailing newline so comparisons are simple */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
    return (int)n;
}

static long rd_num(const char *path) {
    char b[64];
    if (rd(path, b, sizeof b) <= 0) return -1;
    if (strncmp(b, "max", 3) == 0) return -1;
    return strtol(b, NULL, 10);
}

/* field 14 (utime) of /proc/<pid>/stat, in scheduler ticks-as-ms. */
static long proc_utime(int pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (rd(path, buf, sizeof buf) <= 0) return -1;
    /* Skip past "comm" -- it is parenthesised and may contain spaces. */
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    p++;
    /* Now at field 3 (state). utime is field 14, so skip 11 more. */
    int skip = 11;
    while (skip--) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    return strtol(p, NULL, 10);
}

static int reap(pid_t p) {
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

/* Burn CPU for `ms` of wall time. */
static void burn(long ms) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile unsigned long x = 0;
    for (;;) {
        for (int i = 0; i < 200000; i++) x += i;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long d = (t1.tv_sec - t0.tv_sec) * 1000 +
                 (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (d >= ms) break;
    }
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("cgroup: start pid=%d\n", (int)getpid());

    /* ---- bit0: mounted, controllers advertised ---- */
    {
        char b[128];
        if (rd(CG "/cgroup.controllers", b, sizeof b) > 0 &&
            strstr(b, "pids") && strstr(b, "cpu")) {
            bits |= 1;
            printf("cgroup: cgroup2 mounted, controllers = '%s'\n", b);
        } else printf("cgroup: bit0 FAILED (controllers unreadable)\n");
    }

    /* ---- bit1: mkdir/rmdir and v2 defaults ---- */
    {
        int ok = 1;
        if (mkdir(CG "/t1", 0755) != 0) { printf("cgroup: mkdir errno=%d\n", errno); ok = 0; }
        char b[64];
        if (ok && (rd(CG "/t1/pids.max", b, sizeof b) <= 0 || strcmp(b, "max") != 0)) {
            printf("cgroup: fresh pids.max = '%s', want 'max'\n", b); ok = 0;
        }
        if (ok && (rd(CG "/t1/cpu.weight", b, sizeof b) <= 0 || strcmp(b, "100") != 0)) {
            printf("cgroup: fresh cpu.weight = '%s', want 100\n", b); ok = 0;
        }
        /* A cgroup name may not contain '.', so it cannot shadow a control file. */
        if (ok && mkdir(CG "/bad.name", 0755) == 0) {
            printf("cgroup: a name with '.' was accepted\n"); ok = 0;
        }
        if (ok && rmdir(CG "/t1") != 0) { printf("cgroup: rmdir errno=%d\n", errno); ok = 0; }
        if (ok) { bits |= 2; printf("cgroup: mkdir/rmdir work; v2 defaults are max/100\n"); }
    }

    /* ---- bit2: membership ---- */
    {
        int ok = 1;
        if (mkdir(CG "/t2", 0755) != 0) ok = 0;
        pid_t c = ok ? fork() : -1;
        if (c == 0) { burn(400); _exit(0); }        /* stays alive to be moved */
        if (c > 0) {
            char pidstr[16];
            snprintf(pidstr, sizeof pidstr, "%d", (int)c);
            if (wr(CG "/t2/cgroup.procs", pidstr) != 0) {
                printf("cgroup: attach errno=%d\n", errno); ok = 0;
            }
            if (ok && rd_num(CG "/t2/pids.current") < 1) {
                printf("cgroup: pids.current did not reflect the move\n"); ok = 0;
            }
            char b[256];
            if (ok && (rd(CG "/t2/cgroup.procs", b, sizeof b) <= 0 ||
                       !strstr(b, pidstr))) {
                printf("cgroup: cgroup.procs does not list the pid\n"); ok = 0;
            }
            reap(c);
            rmdir(CG "/t2");
        } else ok = 0;
        if (ok) { bits |= 4; printf("cgroup: cgroup.procs moves a process; pids.current tracks it\n"); }
    }

    /* ---- bits3+4: pids.max ENFORCED, with the raise-the-limit control ---- */
    {
        int ok3 = 0, ok4 = 0;
        if (mkdir(CG "/t3", 0755) == 0) {
            pid_t c = fork();
            if (c == 0) {
                /* Join the cgroup, then set the limit to exactly "me". */
                char me[16]; snprintf(me, sizeof me, "%d", (int)getpid());
                if (wr(CG "/t3/cgroup.procs", me) != 0) _exit(1);
                if (wr(CG "/t3/pids.max", "1") != 0) _exit(2);
                /* At the limit: fork MUST fail, and specifically with EAGAIN --
                 * any error would also be produced by an exhausted proc table. */
                errno = 0;
                pid_t g = fork();
                if (g == 0) _exit(0);                 /* must never run */
                if (g >= 0) _exit(3);                 /* fork SUCCEEDED: not enforced */
                if (errno != EAGAIN) _exit(4);
                /* Raise the limit: fork must work again. Without this, exit 0
                 * could just mean "forking is broken in here". */
                if (wr(CG "/t3/pids.max", "8") != 0) _exit(5);
                pid_t g2 = fork();
                if (g2 == 0) _exit(0);
                if (g2 < 0) _exit(6);
                int st = 0;
                if (waitpid(g2, &st, 0) != g2) _exit(7);
                _exit(0);
            }
            int rc = reap(c);
            if (rc == 0) { ok3 = ok4 = 1; }
            else printf("cgroup: pids.max child rc=%d\n", rc);
            rmdir(CG "/t3");
        }
        if (ok3) { bits |= 8;  printf("cgroup: pids.max ENFORCED -- fork returns EAGAIN at the limit\n"); }
        if (ok4) { bits |= 16; printf("cgroup: ...and forks again once the limit is raised (control)\n"); }
    }

    /* ---- bit5: the limit is HIERARCHICAL ----
     * A child cgroup raising its own pids.max must not escape the parent's cap.
     * If it could, the control would be worthless for containers. */
    {
        int ok = 0;
        if (mkdir(CG "/t5", 0755) == 0 && mkdir(CG "/t5/inner", 0755) == 0) {
            pid_t c = fork();
            if (c == 0) {
                char me[16]; snprintf(me, sizeof me, "%d", (int)getpid());
                if (wr(CG "/t5/inner/cgroup.procs", me) != 0) _exit(1);
                /* Parent caps at 1; child sets its own to a big number. */
                if (wr(CG "/t5/pids.max", "1") != 0) _exit(2);
                if (wr(CG "/t5/inner/pids.max", "100") != 0) _exit(3);
                errno = 0;
                pid_t g = fork();
                if (g == 0) _exit(0);
                if (g >= 0) _exit(4);            /* escaped the parent's cap */
                if (errno != EAGAIN) _exit(5);
                _exit(0);
            }
            int rc = reap(c);
            if (rc == 0) ok = 1; else printf("cgroup: hierarchy child rc=%d\n", rc);
            rmdir(CG "/t5/inner");
            rmdir(CG "/t5");
        }
        if (ok) { bits |= 32; printf("cgroup: pids.max is hierarchical -- a child cannot raise its way out\n"); }
    }

    /* ---- bit6: cpu.weight produces MEASURED skew ----
     * Two cgroups, weight 10 (PRIO_LOW band) vs 1000 (PRIO_HIGH band), with MORE
     * spinners than CPUs so there is real contention -- with 4 CPUs and only 2
     * spinners each would simply get a core and the weights would be invisible.
     * Measured from /proc/<pid>/stat, not from cpu.weight. */
    {
        const int N = 3;                       /* per side; 6 spinners on 4 cpus */
        int ok = 1;
        if (mkdir(CG "/lo", 0755) != 0 || mkdir(CG "/hi", 0755) != 0) ok = 0;
        if (ok && (wr(CG "/lo/cpu.weight", "10") != 0 ||
                   wr(CG "/hi/cpu.weight", "1000") != 0)) {
            printf("cgroup: cpu.weight write errno=%d\n", errno); ok = 0;
        }
        pid_t lo[8], hi[8];
        if (ok) {
            for (int i = 0; i < N; i++) {
                lo[i] = fork();
                if (lo[i] == 0) {
                    char me[16]; snprintf(me, sizeof me, "%d", (int)getpid());
                    (void)wr(CG "/lo/cgroup.procs", me);
                    burn(1500);
                    _exit(0);
                }
                hi[i] = fork();
                if (hi[i] == 0) {
                    char me[16]; snprintf(me, sizeof me, "%d", (int)getpid());
                    (void)wr(CG "/hi/cgroup.procs", me);
                    burn(1500);
                    _exit(0);
                }
            }
            /* Sample while they are still running -- /proc entries vanish on reap. */
            burn(1200);
            long lo_t = 0, hi_t = 0;
            for (int i = 0; i < N; i++) {
                long a = proc_utime(lo[i]), b = proc_utime(hi[i]);
                if (a > 0) lo_t += a;
                if (b > 0) hi_t += b;
            }
            for (int i = 0; i < N; i++) { reap(lo[i]); reap(hi[i]); }
            printf("cgroup: measured CPU -- weight10 total=%ld, weight1000 total=%ld\n",
                   lo_t, hi_t);
            /* Require a real difference, not a coin flip. The scheduler ages
             * starved processes (SCHED_AGE_MAX_BOOST), so the skew is bounded and
             * a proportional-share ratio is NOT expected -- 25% is a signal that
             * the weight reached the scheduler at all, which is the claim. */
            if (hi_t > 0 && lo_t >= 0 && hi_t >= lo_t + (lo_t / 4) + 1) {
                bits |= 64;
                printf("cgroup: cpu.weight produced measured skew (high >= low + 25%%)\n");
            } else {
                printf("cgroup: bit6 FAILED -- no measurable skew from cpu.weight\n");
                ok = 0;
            }
        }
        rmdir(CG "/lo"); rmdir(CG "/hi");
    }

    /* ---- bit7: honest refusals + hygiene ---- */
    {
        int ok = 1;
        if (mkdir(CG "/t7", 0755) != 0) ok = 0;
        /* A control file is not removable. */
        if (ok && unlink(CG "/t7/pids.max") == 0) {
            printf("cgroup: a control file was unlinked\n"); ok = 0;
        }
        /* Out-of-range weights are refused (v2 range is 1..10000). */
        if (ok && wr(CG "/t7/cpu.weight", "0") == 0) {
            printf("cgroup: cpu.weight 0 accepted\n"); ok = 0;
        }
        if (ok && wr(CG "/t7/cpu.weight", "99999") == 0) {
            printf("cgroup: cpu.weight 99999 accepted\n"); ok = 0;
        }
        /* A POPULATED cgroup must not rmdir -- otherwise its members would be
         * left pointing at a freed cgroup. */
        pid_t c = ok ? fork() : -1;
        if (c == 0) { burn(500); _exit(0); }
        if (c > 0) {
            char me[16]; snprintf(me, sizeof me, "%d", (int)c);
            (void)wr(CG "/t7/cgroup.procs", me);
            if (rmdir(CG "/t7") == 0) {
                printf("cgroup: a POPULATED cgroup was removed\n"); ok = 0;
            }
            reap(c);
        } else if (ok) ok = 0;
        if (ok && rmdir(CG "/t7") != 0) {
            printf("cgroup: rmdir after the member exited failed errno=%d\n", errno);
            ok = 0;
        }
        /* Hygiene: nothing of ours may remain. */
        struct stat st;
        if (ok && stat(CG "/t7", &st) == 0) { printf("cgroup: /t7 survived\n"); ok = 0; }
        if (ok) { bits |= 128; printf("cgroup: refusals honest; hierarchy left clean\n"); }
    }

    printf("cgroup: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
