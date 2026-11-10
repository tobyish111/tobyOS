/* vmstat / tload / ss / skill / snice / slabtop / bridge / tracepath
 *
 * One multi-call binary, dispatching on argv[0]. Four of these read real
 * kernel state; three cannot, and say exactly which piece of it is missing
 * rather than printing a plausible zero. That distinction is the whole
 * point -- a monitoring tool that invents its numbers is worse than one
 * that admits it has none.
 *
 *   vmstat     memory and process counts are REAL, from /proc/meminfo and
 *              /proc/stat. The cpu, io and system columns are NOT: the
 *              kernel's /proc/stat emits literal zeros for user/sys/idle,
 *              intr and ctxt because nothing accumulates them. Those
 *              columns print "-" and the reason is stated once on stderr.
 *   tload      REAL, from /proc/loadavg.
 *   ss         REAL, from /proc/net/tcp and /proc/net/udp.
 *   skill      REAL: the process list behind ps, plus kill(2).
 *   snice      same list, but renice is not exposed to userland -- refuses.
 *   slabtop    /proc/slabinfo does not exist. Refuses and says so.
 *   bridge     the kernel HAS a bridge (src/bridge.c) but nothing in the
 *              ABI publishes its port or fdb tables. Refuses and says so.
 *   tracepath  needs a per-packet TTL, and libtoby has no setsockopt at
 *              all, so there is no IP_TTL to set. Refuses and says so.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>

static const char *prog = "vmstat";

/* ------------------------------------------------------------ /proc bits */

static int slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* Pull "Key:  1234 kB" out of a /proc/meminfo-shaped buffer. Returns -1
 * when the key is absent, which is different from it being zero. */
static long meminfo_kb(const char *buf, const char *key) {
    size_t klen = strlen(key);
    for (const char *p = buf; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > klen && !strncmp(p, key, klen) && p[klen] == ':') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t') q++;
            long v = 0;
            int got = 0;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q++ - '0'); got = 1; }
            return got ? v : -1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

static long stat_field(const char *buf, const char *key) {
    size_t klen = strlen(key);
    for (const char *p = buf; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > klen && !strncmp(p, key, klen) && p[klen] == ' ') {
            const char *q = p + klen + 1;
            while (*q == ' ') q++;
            long v = 0;
            int got = 0;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q++ - '0'); got = 1; }
            return got ? v : -1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

/* The "cpu  ..." line and the two counters vmstat's system/cpu columns are
 * made of. Nothing here is a placeholder any more: the kernel accumulates
 * all of them (2026-09-01). */
struct cpustat { long user, nice, sys, idle, iowait, irq, softirq, steal, intr, ctxt; };

static int read_cpustat(struct cpustat *c) {
    char st[4096];
    if (slurp("/proc/stat", st, sizeof st) < 0) return -1;
    memset(c, 0, sizeof *c);
    const char *p = st;
    if (strncmp(p, "cpu ", 4)) return -1;
    p += 4;
    long *f[8] = { &c->user, &c->nice, &c->sys, &c->idle,
                   &c->iowait, &c->irq, &c->softirq, &c->steal };
    for (int i = 0; i < 8; i++) {
        while (*p == ' ') p++;
        long v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        *f[i] = v;
    }
    c->intr = stat_field(st, "intr");
    c->ctxt = stat_field(st, "ctxt");
    if (c->intr < 0) c->intr = 0;
    if (c->ctxt < 0) c->ctxt = 0;
    return 0;
}

/* /proc/uptime's first field, in whole seconds (at least 1). */
static long read_uptime_s(void) {
    char up[64];
    if (slurp("/proc/uptime", up, sizeof up) < 0) return 1;
    long v = 0;
    for (const char *p = up; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    return v > 0 ? v : 1;
}

/* One vmstat row's system and cpu columns from a pair of samples: the
 * first row is the average since boot (divided by uptime), every later one
 * the delta over the interval, which is what procps prints. */
static void cpu_columns(const struct cpustat *a, const struct cpustat *b,
                        long secs, long *in, long *cs,
                        long *us, long *sy, long *id, long *wa, long *st) {
    long du = (b->user - a->user) + (b->nice - a->nice);
    long ds = (b->sys - a->sys) + (b->irq - a->irq) + (b->softirq - a->softirq);
    long di = b->idle - a->idle;
    long dw = b->iowait - a->iowait;
    long dt = b->steal - a->steal;
    long tot = du + ds + di + dw + dt;
    if (secs < 1) secs = 1;
    *in = (b->intr - a->intr) / secs;
    *cs = (b->ctxt - a->ctxt) / secs;
    if (tot <= 0) { *us = *sy = *wa = *st = 0; *id = 100; return; }
    *us = du * 100 / tot;
    *sy = ds * 100 / tot;
    *wa = dw * 100 / tot;
    *st = dt * 100 / tot;
    *id = 100 - *us - *sy - *wa - *st;
}

/* ---------------------------------------------------------------- vmstat */

static int do_vmstat(int argc, char **argv) {
    int    delay = 0, count = 1;
    int    unit_kb = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-S")) { if (i + 1 < argc) i++; continue; }
        if (!strcmp(a, "-s")) { /* summary */ }
        if (a[0] >= '0' && a[0] <= '9') {
            if (!delay) { delay = atoi(a); count = 0; }
            else        { count = atoi(a); }
            continue;
        }
    }
    (void)unit_kb;

    char mem[4096], st[4096];
    if (slurp("/proc/meminfo", mem, sizeof mem) < 0) {
        fprintf(stderr, "%s: cannot read /proc/meminfo\n", prog);
        return 1;
    }
    if (slurp("/proc/stat", st, sizeof st) < 0) st[0] = 0;

    long free_kb  = meminfo_kb(mem, "MemFree");
    long buff_kb  = meminfo_kb(mem, "Buffers");
    long cache_kb = meminfo_kb(mem, "Cached");
    long swtot    = meminfo_kb(mem, "SwapTotal");
    long swfree   = meminfo_kb(mem, "SwapFree");
    long running  = stat_field(st, "procs_running");
    long blocked  = stat_field(st, "procs_blocked");

    /* Said ONCE, to stderr, so the table stays parseable and the reader is
     * not left to assume an idle disk. The cpu and system columns are real
     * now; the io ones need page-in/page-out counters this kernel does not
     * publish (there is no /proc/vmstat). */
    fprintf(stderr,
            "%s: io columns (si so bi bo) are unavailable: there is no "
            "/proc/vmstat to read page traffic from.\n", prog);
    struct cpustat zero, prev, cur;
    memset(&zero, 0, sizeof zero);
    int have_prev = read_cpustat(&prev) == 0;
    long boot_secs = read_uptime_s();

    printf("procs -----------memory---------- ---swap-- -----io---- "
           "-system-- ------cpu-----\n");
    printf(" r  b   swpd   free   buff  cache   si   so    bi    bo   "
           "in   cs us sy id wa st\n");

    int iter = 0;
    for (;;) {
        if (slurp("/proc/meminfo", mem, sizeof mem) >= 0) {
            free_kb  = meminfo_kb(mem, "MemFree");
            buff_kb  = meminfo_kb(mem, "Buffers");
            cache_kb = meminfo_kb(mem, "Cached");
            swtot    = meminfo_kb(mem, "SwapTotal");
            swfree   = meminfo_kb(mem, "SwapFree");
        }
        if (slurp("/proc/stat", st, sizeof st) >= 0) {
            running = stat_field(st, "procs_running");
            blocked = stat_field(st, "procs_blocked");
        }
        long swused = (swtot >= 0 && swfree >= 0) ? swtot - swfree : 0;

        long in = 0, cs = 0, us = 0, sy = 0, id = 100, wa = 0, stl = 0;
        if (have_prev) {
            if (iter == 0)
                cpu_columns(&zero, &prev, boot_secs, &in, &cs, &us, &sy, &id, &wa, &stl);
            else if (read_cpustat(&cur) == 0) {
                cpu_columns(&prev, &cur, delay, &in, &cs, &us, &sy, &id, &wa, &stl);
                prev = cur;
            }
        }
        printf("%2ld %2ld %6ld %6ld %6ld %6ld %4s %4s %5s %5s %4ld %4ld "
               "%2ld %2ld %2ld %2ld %2ld\n",
               running < 0 ? 0 : running,
               blocked < 0 ? 0 : blocked,
               swused,
               free_kb  < 0 ? 0 : free_kb,
               buff_kb  < 0 ? 0 : buff_kb,
               cache_kb < 0 ? 0 : cache_kb,
               "-", "-", "-", "-", in, cs, us, sy, id, wa, stl);
        iter++;
        if (count && iter >= count) break;
        if (delay <= 0) break;
        sleep((unsigned)delay);
    }
    return 0;
}

/* ----------------------------------------------------------------- tload */

static int do_tload(int argc, char **argv) {
    int delay = 0, scale = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { delay = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-s") && i + 1 < argc) { scale = atoi(argv[++i]); continue; }
    }
    if (scale < 1) scale = 1;

    /* Real tload loops until killed. It still does when stdout is a
     * terminal; into a pipe or a file it draws once and exits, so it can
     * be used from a script (and tested) without hanging. */
    int interactive = isatty(1);

    for (;;) {
        char buf[256];
        if (slurp("/proc/loadavg", buf, sizeof buf) < 0) {
            fprintf(stderr, "tload: cannot read /proc/loadavg\n");
            return 1;
        }
        /* Carried as HUNDREDTHS in a long, not a double: user programs in
         * this tree are built -mno-sse, so floating point here would be
         * either a soft-float libcall or an x87 surprise. */
        long l1x100 = 0;
        {
            const char *p = buf;
            long ip = 0, fp = 0, digits = 0;
            while (*p >= '0' && *p <= '9') ip = ip * 10 + (*p++ - '0');
            if (*p == '.') {
                p++;
                while (*p >= '0' && *p <= '9' && digits < 2) { fp = fp * 10 + (*p++ - '0'); digits++; }
                while (digits < 2) { fp *= 10; digits++; }
            }
            l1x100 = ip * 100 + fp;
        }
        int bars = (int)(l1x100 / (10L * (long)scale));
        if (bars > 60) bars = 60;
        if (bars < 0)  bars = 0;
        printf("%ld.%02ld ", l1x100 / 100, l1x100 % 100);
        for (int i = 0; i < bars; i++) putchar('*');
        putchar('\n');
        fflush(stdout);
        if (!interactive) break;
        sleep(delay > 0 ? (unsigned)delay : 5u);
    }
    return 0;
}

/* -------------------------------------------------------------------- ss */

/* /proc/net/tcp gives hex addresses in the kernel's byte order, exactly as
 * Linux does; ss is the tool that turns them back into something readable. */
static void fmt_addr(const char *hex, char *out, size_t cap) {
    unsigned a = 0, port = 0;
    unsigned v[4] = {0,0,0,0};
    /* "0100007F:1F90" */
    for (int i = 0; i < 8; i++) {
        char c = hex[i];
        unsigned d = (c >= '0' && c <= '9') ? (unsigned)(c - '0')
                   : (unsigned)(tolower((unsigned char)c) - 'a' + 10);
        a = (a << 4) | d;
    }
    for (const char *p = hex + 9; *p && *p != ' ' && *p != '\n'; p++) {
        unsigned d = (*p >= '0' && *p <= '9') ? (unsigned)(*p - '0')
                   : (unsigned)(tolower((unsigned char)*p) - 'a' + 10);
        port = (port << 4) | d;
    }
    v[0] = a & 0xFF; v[1] = (a >> 8) & 0xFF; v[2] = (a >> 16) & 0xFF; v[3] = (a >> 24) & 0xFF;
    snprintf(out, cap, "%u.%u.%u.%u:%u", v[0], v[1], v[2], v[3], port);
}

static const char *tcp_state(unsigned st) {
    switch (st) {
    case 1:  return "ESTAB";
    case 2:  return "SYN-SENT";
    case 3:  return "SYN-RECV";
    case 4:  return "FIN-WAIT-1";
    case 5:  return "FIN-WAIT-2";
    case 6:  return "TIME-WAIT";
    case 7:  return "CLOSE";
    case 8:  return "CLOSE-WAIT";
    case 9:  return "LAST-ACK";
    case 10: return "LISTEN";
    case 11: return "CLOSING";
    default: return "UNKNOWN";
    }
}

static int ss_dump(const char *path, const char *proto, int listening_only,
                   int *printed_header)
{
    char buf[16384];
    if (slurp(path, buf, sizeof buf) < 0) return 0;
    int rows = 0;
    char *save = buf;
    int first = 1;
    for (char *line = save; line && *line; ) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        if (first) { first = 0; goto next; }
        {
            /* sl local rem st ... */
            char *p = line;
            while (*p == ' ') p++;
            while (*p && *p != ':') p++;          /* skip "  0:" */
            if (!*p) goto next;
            p++;
            while (*p == ' ') p++;
            char lhex[32], rhex[32];
            int i = 0;
            while (*p && *p != ' ' && i < 31) lhex[i++] = *p++;
            lhex[i] = 0;
            while (*p == ' ') p++;
            i = 0;
            while (*p && *p != ' ' && i < 31) rhex[i++] = *p++;
            rhex[i] = 0;
            while (*p == ' ') p++;
            unsigned st = 0;
            for (; *p && *p != ' '; p++) {
                unsigned d = (*p >= '0' && *p <= '9') ? (unsigned)(*p - '0')
                           : (unsigned)(tolower((unsigned char)*p) - 'a' + 10);
                st = st * 16 + d;
            }
            if (strlen(lhex) < 9 || strlen(rhex) < 9) goto next;
            int is_listen = (st == 10);
            if (listening_only && !is_listen) goto next;
            char la[64], ra[64];
            fmt_addr(lhex, la, sizeof la);
            fmt_addr(rhex, ra, sizeof ra);
            if (!*printed_header) {
                printf("%-6s %-12s %-22s %-22s\n",
                       "Netid", "State", "Local Address:Port", "Peer Address:Port");
                *printed_header = 1;
            }
            printf("%-6s %-12s %-22s %-22s\n", proto,
                   strcmp(proto, "udp") == 0 ? "UNCONN" : tcp_state(st), la, ra);
            rows++;
        }
    next:
        if (!eol) break;
        line = eol + 1;
    }
    return rows;
}

static int do_ss(int argc, char **argv) {
    int listening = 0, want_tcp = 0, want_udp = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) continue;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'l': listening = 1; break;
            case 't': want_tcp = 1; break;
            case 'u': want_udp = 1; break;
            case 'a': break;
            case 'n': break;   /* numeric: the only mode there is here */
            case 'p': break;
            default: break;
            }
        }
    }
    if (!want_tcp && !want_udp) { want_tcp = 1; want_udp = 1; }

    int header = 0;
    if (want_tcp) (void)ss_dump("/proc/net/tcp", "tcp", listening, &header);
    if (want_udp) (void)ss_dump("/proc/net/udp", "udp", listening, &header);
    if (!header)
        printf("%-6s %-12s %-22s %-22s\n",
               "Netid", "State", "Local Address:Port", "Peer Address:Port");
    return 0;
}

/* ---------------------------------------------------------- skill / snice */

/* The process list comes from /proc, not from the service list behind ps:
 * syscall 65 enumerates SERVICES, so a plain `sleep` never appears in it and
 * the call fails outright for an unprivileged caller. /proc lists a
 * directory per live pid, and each has a status file whose first field is
 * the name. */
static int proc_name_of(const char *pid, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int got = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Name:", 5)) continue;
        const char *q = line + 5;
        while (*q == ' ' || *q == 9) q++;   /* space or tab */
        size_t n = 0;
        while (q[n] && q[n] != 10 && q[n] != 13 && n + 1 < cap) { out[n] = q[n]; n++; }
        out[n] = 0;
        got = 0;
        break;
    }
    fclose(f);
    return got;
}

static int signal_by_name(const char *s) {
    if (*s == '-') s++;
    if (*s >= '0' && *s <= '9') return atoi(s);
    if (!strncmp(s, "SIG", 3)) s += 3;
    if (!strcmp(s, "HUP"))  return SIGHUP;
    if (!strcmp(s, "INT"))  return SIGINT;
    if (!strcmp(s, "QUIT")) return SIGQUIT;
    if (!strcmp(s, "KILL")) return SIGKILL;
    if (!strcmp(s, "TERM")) return SIGTERM;
    if (!strcmp(s, "STOP")) return SIGSTOP;
    if (!strcmp(s, "CONT")) return SIGCONT;
    if (!strcmp(s, "USR1")) return SIGUSR1;
    if (!strcmp(s, "USR2")) return SIGUSR2;
    return -1;
}

static int do_skill(int argc, char **argv, int is_snice) {
    if (is_snice) {
        fprintf(stderr, "snice: cannot change priorities: no setpriority "
                        "syscall is exposed to userland, so there is nothing "
                        "to renice through. Use skill to signal instead.\n");
        return 1;
    }

    int sig = SIGTERM;
    const char *names[64];
    int nnames = 0;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "-v")) { verbose = 1; continue; }
            if (!strcmp(a, "-n")) { continue; }
            int s = signal_by_name(a);
            if (s > 0) { sig = s; continue; }
            continue;
        }
        if (nnames < 64) names[nnames++] = a;
    }
    if (nnames == 0) {
        fprintf(stderr, "usage: skill [-SIGNAL] name ...\n");
        return 1;
    }

    DIR *d = opendir("/proc");
    if (!d) {
        fprintf(stderr, "skill: cannot open /proc\n");
        return 1;
    }
    int hit = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 1) continue;                 /* never the init/idle task */
        if (pid == (int)getpid()) continue;     /* nor ourselves */
        char nm[64];
        if (proc_name_of(e->d_name, nm, sizeof nm) != 0) continue;
        for (int k = 0; k < nnames; k++) {
            if (strcmp(nm, names[k])) continue;
            if (kill(pid, sig) == 0) {
                hit++;
                if (verbose) printf("%s (%d) signalled %d\n", nm, pid, sig);
            }
        }
    }
    closedir(d);
    if (!hit) return 1;
    return 0;
}

/* -------------------------------- the three with no data source to read */

static int refuse(const char *name, const char *why) {
    fprintf(stderr, "%s: %s\n", name, why);
    return 1;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "vmstat";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;

    if (!strcmp(base, "vmstat")) return do_vmstat(argc, argv);
    if (!strcmp(base, "tload"))  return do_tload(argc, argv);
    if (!strcmp(base, "ss"))     return do_ss(argc, argv);
    if (!strcmp(base, "skill"))  return do_skill(argc, argv, 0);
    if (!strcmp(base, "snice"))  return do_skill(argc, argv, 1);

    if (!strcmp(base, "slabtop"))
        return refuse("slabtop",
            "/proc/slabinfo does not exist: this kernel's allocator keeps no "
            "per-cache statistics, so there is nothing to report. Printing a "
            "table of zeros would be worse than saying so.");

    if (!strcmp(base, "bridge"))
        return refuse("bridge",
            "the kernel has a bridge (src/bridge.c) but no syscall or /proc "
            "file publishes its port list or forwarding database, so there "
            "is no way to read it from userland yet.");

    if (!strcmp(base, "tracepath"))
        return refuse("tracepath",
            "needs a per-packet TTL and there is no setsockopt in libtoby, "
            "so IP_TTL cannot be set; nor is there a raw socket to read the "
            "ICMP time-exceeded replies from.");

    fprintf(stderr, "%s: unknown applet\n", base);
    return 1;
}
