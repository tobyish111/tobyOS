/* linux-cgroup2 -- Phase 3 slice 16: the cgroup v2 MEMORY and IO controllers.
 *
 * A second program rather than more bits in linux-cgroup because the exit status
 * carries the bitmask and slice 15 already uses all eight bits (0xff).
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS HAS TO PROVE, AND WHY EACH CHECK IS SHAPED THE WAY IT IS
 *
 * `memory.current` is the easiest number in an operating system to fake. It is a
 * decimal string in a file. A kernel that printed the machine's total RSS, or a
 * constant, or the value last written to memory.max, would pass any test that
 * merely reads it and checks it is a number. So:
 *
 *   bit0  the files exist with the v2 defaults, and io.max does NOT exist --
 *         because this kernel does not throttle and must not pretend to
 *   bit1  memory.current TRACKS a known allocation: touch 8 MiB, and the number
 *         must rise by at least 8 MiB and not wildly more
 *   bit2  ...and FALLS when the memory is returned. Charge-only accounting is the
 *         classic half-implementation: it looks perfect until something frees
 *   bit3  memory.max is ENFORCED -- allocation actually fails at the limit, and
 *         memory.events `max` counts the refusal. Enforcement is the difference
 *         between a controller and a text file
 *   bit4  the limit is HIERARCHICAL: a child cgroup cannot escape an ancestor's
 *         cap by raising its own (the whole point of a container limit)
 *   bit5  io.stat counts REAL disk bytes, verified by doing real disk I/O
 *   bit6  THE INDEPENDENT CROSS-CHECK: memory.current must agree with the RSS
 *         that /proc/<pid>/stat reports for the same processes. Two separate code
 *         paths reading the same underlying counter -- if the cgroup number were
 *         invented, or drifted, this is what catches it
 *   bit7  the accounting survives a fork/exit cycle without drifting -- a leaked
 *         charge is invisible until it has accumulated enough to break a limit
 *
 * Exit status is the bitmask: 255 means all eight.
 * ---------------------------------------------------------------------------
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

#define CG "/sys/fs/cgroup"

/* Read a whole small file, NUL-terminate, strip the trailing newline. */
static int rd(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, n - 1);
    close(fd);
    if (k < 0) return -1;
    buf[k] = '\0';
    while (k > 0 && (buf[k - 1] == '\n' || buf[k - 1] == ' ')) buf[--k] = '\0';
    return (int)k;
}

static int wr(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t k = write(fd, val, strlen(val));
    close(fd);
    return k == (ssize_t)strlen(val) ? 0 : -1;
}

static long long rd_ll(const char *path) {
    char b[64];
    if (rd(path, b, sizeof b) <= 0) return -1;
    if (strcmp(b, "max") == 0) return -2;
    return atoll(b);
}

/* One field out of a "key value\nkey value\n" file (memory.events, io.stat). */
static long long rd_field(const char *path, const char *key) {
    char b[256];
    if (rd(path, b, sizeof b) <= 0) return -1;
    char *p = strstr(b, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ') p++;
    return atoll(p);
}

/* RSS in pages for one pid, from /proc/<pid>/stat field 24.
 *
 * Deliberately parsed the same way busybox `ps` does -- by counting fields --
 * because that is the consumer whose breakage taught this arc that a truncated
 * /proc file yields a wild pointer rather than an error. */
static long long proc_rss_pages(int pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t k = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (k <= 0) return -1;
    buf[k] = '\0';
    /* Field 2 is "(name)" and may contain spaces -- skip past the LAST ')'. */
    char *s = strrchr(buf, ')');
    if (!s) return -1;
    s++;
    int field = 2;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        field++;
        if (field == 24) return atoll(s);
        while (*s && *s != ' ') s++;
    }
    return -1;
}

/* Touch `n` bytes of freshly-mapped anonymous memory so the pages are really
 * resident. mmap alone proves nothing here: this kernel demand-faults, so an
 * untouched mapping costs zero pages and a correct memory.current would not
 * move -- a test that mmap'd without touching would "fail" a working kernel. */
static void *touch_alloc(size_t n) {
    void *p = mmap(0, n, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    for (size_t i = 0; i < n; i += 4096) ((volatile char *)p)[i] = 1;
    return p;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("cg2: start pid=%d\n", (int)getpid());

    /* ---- bit0: the files exist, defaults are v2, and io.max is ABSENT ---- */
    {
        int ok = 1;
        char b[128];
        if (rd(CG "/cgroup.controllers", b, sizeof b) <= 0 ||
            !strstr(b, "memory") || !strstr(b, "io")) {
            printf("cg2: controllers='%s' -- want memory and io listed\n", b);
            ok = 0;
        }
        if (ok && mkdir(CG "/m1", 0755) != 0) {
            printf("cg2: mkdir errno=%d\n", errno); ok = 0;
        }
        if (ok && (rd(CG "/m1/memory.max", b, sizeof b) <= 0 ||
                   strcmp(b, "max") != 0)) {
            printf("cg2: fresh memory.max='%s', want 'max'\n", b); ok = 0;
        }
        if (ok && rd_ll(CG "/m1/memory.current") != 0) {
            printf("cg2: fresh memory.current=%lld, want 0\n",
                   rd_ll(CG "/m1/memory.current"));
            ok = 0;
        }
        if (ok && rd(CG "/m1/io.stat", b, sizeof b) <= 0) {
            printf("cg2: io.stat unreadable\n"); ok = 0;
        }
        /* io.max must NOT exist. A kernel that cannot throttle and offers the
         * knob anyway is the failure this arc caught twice before. */
        if (ok) {
            int fd = open(CG "/m1/io.max", O_RDONLY);
            if (fd >= 0) {
                printf("cg2: io.max EXISTS -- it would throttle nothing\n");
                close(fd); ok = 0;
            }
        }
        if (ok) { bits |= 1; printf("cg2: memory+io files present, v2 defaults, no io.max\n"); }
    }

    /* ---- bit1: memory.current tracks a known allocation ---- */
    const size_t MB8 = 8u << 20;
    void *blk = 0;
    {
        int ok = 1;
        char pidbuf[16];
        snprintf(pidbuf, sizeof pidbuf, "%d", (int)getpid());
        if (wr(CG "/m1/cgroup.procs", pidbuf) != 0) {
            printf("cg2: could not join m1, errno=%d\n", errno); ok = 0;
        }
        long long before = ok ? rd_ll(CG "/m1/memory.current") : -1;
        if (ok && before < 0) { printf("cg2: memory.current unreadable\n"); ok = 0; }

        if (ok) {
            blk = touch_alloc(MB8);
            if (!blk) { printf("cg2: mmap of 8 MiB failed\n"); ok = 0; }
        }
        long long after = ok ? rd_ll(CG "/m1/memory.current") : -1;
        if (ok) {
            long long delta = after - before;
            /* Lower bound is the assertion that matters: the pages we touched
             * MUST be counted. The upper bound catches a kernel reporting
             * something global (total RSS, machine usage) instead of ours. */
            if (delta < (long long)MB8) {
                printf("cg2: bit1 FAILED -- touched 8 MiB, memory.current rose "
                       "only %lld bytes (%lld -> %lld)\n", delta, before, after);
                ok = 0;
            } else if (delta > (long long)MB8 * 4) {
                printf("cg2: bit1 FAILED -- rose %lld bytes for an 8 MiB touch; "
                       "that is not this process's accounting\n", delta);
                ok = 0;
            } else {
                printf("cg2: memory.current %lld -> %lld (+%lld for 8 MiB "
                       "touched)\n", before, after, delta);
            }
        }
        if (ok) bits |= 2;
    }

    /* ---- bit2: ...and it falls again when the memory is returned ---- */
    {
        int ok = 1;
        long long before = rd_ll(CG "/m1/memory.current");
        if (blk && munmap(blk, MB8) != 0) {
            printf("cg2: munmap errno=%d\n", errno); ok = 0;
        }
        long long after = ok ? rd_ll(CG "/m1/memory.current") : -1;
        if (ok) {
            long long freed = before - after;
            if (freed < (long long)MB8) {
                printf("cg2: bit2 FAILED -- freed 8 MiB but memory.current fell "
                       "only %lld bytes (%lld -> %lld). Charge-without-uncharge "
                       "makes every limit a one-way ratchet.\n",
                       freed, before, after);
                ok = 0;
            } else {
                printf("cg2: memory.current %lld -> %lld (-%lld on munmap)\n",
                       before, after, freed);
            }
        }
        if (ok) bits |= 4;
    }

    /* ---- bit3: memory.max is ENFORCED, and the refusal is counted ----
     *
     * Enforced in a CHILD, in its own cgroup, for two reasons: this process must
     * survive to report, and the limit has to be seen to apply to whoever is in
     * the cgroup rather than to whoever wrote the file.
     *
     * The child grows the heap with brk-backed mallocs, which this kernel commits
     * EAGERLY -- so the refusal comes back as a NULL from malloc, in-process and
     * unambiguous, instead of as a fault-time kill that would be hard to tell
     * apart from an ordinary crash. */
    {
        int ok = 1;
        if (mkdir(CG "/m2", 0755) != 0 && errno != EEXIST) {
            printf("cg2: mkdir m2 errno=%d\n", errno); ok = 0;
        }
        /* 6 MiB: comfortably above a static-glibc process's footprint, far below
         * the 64 MiB the child will try to take. */
        if (ok && wr(CG "/m2/memory.max", "6291456") != 0) {
            printf("cg2: writing memory.max errno=%d\n", errno); ok = 0;
        }
        if (ok && rd_ll(CG "/m2/memory.max") != 6291456) {
            printf("cg2: memory.max did not round-trip (%lld)\n",
                   rd_ll(CG "/m2/memory.max"));
            ok = 0;
        }
        if (ok) {
            pid_t c = fork();
            if (c == 0) {
                setvbuf(stdout, NULL, _IONBF, 0);
                char pb[16]; snprintf(pb, sizeof pb, "%d", (int)getpid());
                if (wr(CG "/m2/cgroup.procs", pb) != 0) _exit(21);
                size_t got = 0;
                char *first = 0, *last = 0;
                /* 64 MiB in 64 KiB steps. A kernel enforcing nothing reaches
                 * the end; an enforcing one hands back NULL long before. */
                for (int i = 0; i < 1024; i++) {
                    char *q = malloc(64u << 10);
                    if (!q) break;
                    memset(q, 1, 64u << 10);
                    if (!first) first = q;
                    last = q;
                    got += 64u << 10;
                }
                /* Where the memory came from: the user heap starts at
                 * 0x10000000 (brk), mmap regions land far higher. This says
                 * which kernel path served it. */
                printf("cg2:   [child] first=%p last=%p span=%lldKiB\n",
                       (void *)first, (void *)last,
                       (long long)((last - first) >> 10));
                /* Diagnostic: p->user_pages is maintained ONLY by the charge
                 * funnel now, and /proc RSS reports it -- so this line
                 * distinguishes "the funnel never ran for me" (RSS stays tiny)
                 * from "it ran but charged the wrong cgroup" (RSS tracks got). */
                printf("cg2:   [child pid=%d] got=%lluKiB own /proc RSS=%lld "
                       "pages, m2.current=%lld\n",
                       (int)getpid(), (unsigned long long)(got >> 10),
                       proc_rss_pages((int)getpid()),
                       rd_ll(CG "/m2/memory.current"));
                _exit(got >= (48u << 20) ? 22 : 0);   /* 22 == limit ignored */
            }
            int st = 0;
            waitpid(c, &st, 0);
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            long long refusals = rd_field(CG "/m2/memory.events", "max");
            if (code == 21) { printf("cg2: child could not join m2\n"); ok = 0; }
            else if (code == 22) {
                /* Print m2's own numbers: they separate "the charges never
                 * reached this cgroup" from "they did and the check is wrong". */
                printf("cg2: bit3 FAILED -- child allocated 48+ MiB inside a "
                       "6 MiB memory.max. m2 current=%lld peak=%lld max=%lld "
                       "refusals=%lld\n",
                       rd_ll(CG "/m2/memory.current"),
                       rd_ll(CG "/m2/memory.peak"),
                       rd_ll(CG "/m2/memory.max"), refusals);
                ok = 0;
            } else if (code != 0) {
                printf("cg2: bit3 child exited %d (signalled=%d)\n",
                       code, WIFSIGNALED(st));
                ok = 0;
            } else if (refusals <= 0) {
                printf("cg2: bit3 FAILED -- allocation stopped but "
                       "memory.events max=%lld. Without a counted refusal this "
                       "is indistinguishable from the machine being full.\n",
                       refusals);
                ok = 0;
            } else {
                printf("cg2: memory.max ENFORCED -- child stopped short, "
                       "memory.events max=%lld\n", refusals);
            }
        }
        if (ok) bits |= 8;
    }

    /* ---- bit4: the limit is HIERARCHICAL ----
     *
     * m2 keeps its 6 MiB cap; the nested cgroup sets its own to "max". If the cap
     * were checked only on the process's own cgroup, the child would then be
     * unlimited -- which is exactly how a container would escape its pod limit. */
    {
        int ok = 1;
        if (mkdir(CG "/m2/inner", 0755) != 0 && errno != EEXIST) {
            printf("cg2: mkdir m2/inner errno=%d\n", errno); ok = 0;
        }
        if (ok && wr(CG "/m2/inner/memory.max", "max") != 0) {
            printf("cg2: inner memory.max write errno=%d\n", errno); ok = 0;
        }
        if (ok) {
            pid_t c = fork();
            if (c == 0) {
                char pb[16]; snprintf(pb, sizeof pb, "%d", (int)getpid());
                if (wr(CG "/m2/inner/cgroup.procs", pb) != 0) _exit(21);
                size_t got = 0;
                char *ifirst = 0, *ilast = 0;
                for (int i = 0; i < 1024; i++) {
                    char *q = malloc(64u << 10);
                    if (!q) break;
                    memset(q, 1, 64u << 10);
                    if (!ifirst) ifirst = q;
                    ilast = q;
                    got += 64u << 10;
                }
                printf("cg2:   [inner child] first=%p last=%p\n",
                       (void *)ifirst, (void *)ilast);
                printf("cg2:   [inner child pid=%d] got=%lluKiB RSS=%lld pages | "
                       "inner.current=%lld inner.max=%lld | m2.current=%lld "
                       "m2.max=%lld\n",
                       (int)getpid(), (unsigned long long)(got >> 10),
                       proc_rss_pages((int)getpid()),
                       rd_ll(CG "/m2/inner/memory.current"),
                       rd_ll(CG "/m2/inner/memory.max"),
                       rd_ll(CG "/m2/memory.current"),
                       rd_ll(CG "/m2/memory.max"));
                _exit(got >= (48u << 20) ? 22 : 0);
            }
            int st = 0; waitpid(c, &st, 0);
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (code == 22) {
                printf("cg2: bit4 FAILED -- a nested cgroup set memory.max=max "
                       "and escaped its parent's 6 MiB cap\n");
                ok = 0;
            } else if (code != 0) {
                printf("cg2: bit4 child exited %d\n", code); ok = 0;
            } else {
                printf("cg2: the memory limit is HIERARCHICAL (inner max=max "
                       "still bounded by the parent)\n");
            }
        }
        if (ok) bits |= 16;
    }

    /* ---- bit5: io.stat counts REAL disk bytes ----
     *
     * Written to a disk-backed filesystem on purpose: a tmpfs/ramfs write touches
     * no block device, so it would (correctly) charge nothing, and a test that
     * used /tmp would report failure against a working kernel. */
    {
        int ok = 1, skipped = 0;
        const char *cand[] = { "/data/cg2io.tmp", "/cg2io.tmp", 0 };
        const char *path = 0;
        for (int i = 0; cand[i]; i++) {
            int fd = open(cand[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { close(fd); path = cand[i]; break; }
        }
        if (!path) { printf("cg2: bit5 SKIP -- no writable disk-backed path\n"); skipped = 1; }

        if (!skipped) {
            long long wb0 = rd_field(CG "/m1/io.stat", "wbytes");
            long long wi0 = rd_field(CG "/m1/io.stat", "wios");
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            char *buf = malloc(256u << 10);
            if (fd < 0 || !buf) { printf("cg2: bit5 setup failed\n"); ok = 0; }
            else {
                memset(buf, 0xA5, 256u << 10);
                for (int i = 0; i < 4; i++) (void)!write(fd, buf, 256u << 10);
                fsync(fd);
                close(fd);
            }
            free(buf);
            long long wb1 = ok ? rd_field(CG "/m1/io.stat", "wbytes") : -1;
            long long wi1 = ok ? rd_field(CG "/m1/io.stat", "wios") : -1;
            if (ok) {
                if (wb1 <= wb0 || wi1 <= wi0) {
                    printf("cg2: bit5 FAILED -- wrote 1 MiB to disk, io.stat "
                           "wbytes %lld -> %lld, wios %lld -> %lld\n",
                           wb0, wb1, wi0, wi1);
                    ok = 0;
                } else {
                    printf("cg2: io.stat counts real I/O -- wbytes +%lld over "
                           "%lld requests\n", wb1 - wb0, wi1 - wi0);
                }
            }
            unlink(path);
        }
        /* A SKIP must NOT set the bit.
         *
         * It did, with the comment "honest skip; the reason is printed" -- but the
         * exit status IS the machine-readable result, so anything reading only the
         * code saw a pass. That is the harness-lie pattern this arc keeps catching,
         * committed in the very test written to catch it. /data is always mounted in
         * this harness, so if this ever skips the gate SHOULD go red and someone
         * should look. Loud beats silent. */
        if (ok && !skipped) bits |= 32;
    }

    /* ---- bit6: THE INDEPENDENT CROSS-CHECK against /proc RSS ----
     *
     * memory.current is a counter the kernel maintains; /proc/<pid>/stat's RSS is
     * read through a completely different code path. If the cgroup number were
     * fabricated, or had drifted from reality by a missed uncharge, the two would
     * disagree. This is the check that makes the other bits mean something.
     *
     * (This also retro-actively tests a real defect: before this slice, RSS in
     * /proc was a spawn-time constant -- user_stack_pages + 1 -- so it could not
     * have agreed with any honest accounting.) */
    {
        int ok = 1;
        void *b2 = touch_alloc(4u << 20);
        if (!b2) { printf("cg2: bit6 alloc failed\n"); ok = 0; }
        long long cur_pages = ok ? rd_ll(CG "/m1/memory.current") / 4096 : -1;
        long long rss = ok ? proc_rss_pages((int)getpid()) : -1;
        if (ok && (cur_pages < 0 || rss < 0)) {
            printf("cg2: bit6 could not read both counters (cur=%lld rss=%lld)\n",
                   cur_pages, rss);
            ok = 0;
        }
        if (ok) {
            /* This process is m1's only member, so the two must agree exactly.
             * Any slack here would hide precisely the drift being looked for. */
            if (cur_pages != rss) {
                printf("cg2: bit6 FAILED -- memory.current=%lld pages but "
                       "/proc RSS=%lld pages for the same single-member cgroup. "
                       "One of the two is invented.\n", cur_pages, rss);
                ok = 0;
            } else {
                printf("cg2: memory.current agrees EXACTLY with /proc RSS "
                       "(%lld pages) -- two independent paths, one truth\n",
                       cur_pages);
            }
        }
        if (b2) munmap(b2, 4u << 20);
        if (ok) bits |= 64;
    }

    /* ---- bit7: the accounting does not drift across fork/exit ----
     *
     * A leaked charge is invisible until it has accumulated enough to break a
     * limit, at which point the cause is long gone. So: record, spawn children
     * that allocate and exit, and require the number to come back to where it
     * started. */
    {
        int ok = 1;
        long long base = rd_ll(CG "/m1/memory.current");
        for (int r = 0; r < 4; r++) {
            pid_t c = fork();
            if (c == 0) {
                void *q = touch_alloc(2u << 20);
                _exit(q ? 0 : 3);
            }
            int st = 0; waitpid(c, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                printf("cg2: bit7 child failed (st=0x%x)\n", st); ok = 0; break;
            }
        }
        long long end = ok ? rd_ll(CG "/m1/memory.current") : -1;
        if (ok) {
            long long drift = end - base;
            if (drift != 0) {
                printf("cg2: bit7 FAILED -- memory.current drifted by %lld bytes "
                       "(%lld -> %lld) across 4 fork/allocate/exit cycles\n",
                       drift, base, end);
                ok = 0;
            } else {
                printf("cg2: no drift across 4 fork/allocate/exit cycles "
                       "(memory.current back to %lld)\n", end);
            }
        }
        if (ok) bits |= 128;
    }

    /* Leave the hierarchy as we found it. rmdir of a populated cgroup fails, so
     * step out of m1 first -- and that ordering is itself the v2 rule. */
    {
        char pb[16]; snprintf(pb, sizeof pb, "%d", (int)getpid());
        (void)wr(CG "/cgroup.procs", pb);
        (void)rmdir(CG "/m2/inner");
        (void)rmdir(CG "/m2");
        (void)rmdir(CG "/m1");
    }

    printf("cg2: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
