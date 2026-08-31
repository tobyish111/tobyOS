/* user_utillinux -- the util-linux tools tobyOS was missing.
 *
 *   column colrm col rename whereis namei uuidgen setterm
 *   lsblk findmnt lsns wdctl
 *   prlimit lslocks lsipc zramctl
 *
 * One binary dispatching on argv[0], the same shape as user_coreutil,
 * user_admin and user_ssh.
 *
 * The first twelve do their real job against real data: lsblk reads the
 * kernel's block registry through ABI_SYS_BLK_LIST, findmnt reads
 * /proc/mounts, lsns reads /proc/<pid>/ns, wdctl asks the watchdog it
 * actually has.
 *
 * The last four have NO data source on this system, and each says exactly
 * which one is missing instead of printing an empty table that reads as
 * "nothing to report":
 *
 *   prlimit   no getrlimit/setrlimit syscall exists
 *   lslocks   no /proc/locks
 *   lsipc     the kernel has SysV IPC but exposes no listing interface
 *   zramctl   zram exists but nothing reports its devices (and
 *             /proc/meminfo hardcodes SwapTotal to 0)
 *
 * An empty table is the failure mode worth avoiding here: `lslocks` printing
 * a header and no rows means "no locks are held", which is a claim, and a
 * false one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include <tobyos/abi/abi.h>
#include <tobyos_wdog.h>

#define LINE_CAP 4096
#define MAX_ROWS 512

static const char *g_prog = "util";

static int errf(const char *msg, const char *a) {
    if (a) fprintf(stderr, "%s: %s: %s\n", g_prog, a, msg);
    else   fprintf(stderr, "%s: %s\n", g_prog, msg);
    return 1;
}

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}

static int readline_into(FILE *f, char *buf, size_t cap) {
    if (!fgets(buf, (int)cap, f)) return 0;
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    return 1;
}

/* ------------------------------------------------------------------ column
 * -t builds a table: split each line into fields and pad every column to the
 * widest cell in it. Without -t, fill columns across the width. */
static int cmd_column(int argc, char **argv) {
    int table = 0;
    char sep = 0;                 /* 0 = runs of whitespace */
    int width = 80, argi = 1;
    for (; argi < argc; argi++) {
        const char *a = argv[argi];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "-t")) { table = 1; continue; }
        if (!strcmp(a, "-s") && argi + 1 < argc) { sep = argv[++argi][0]; continue; }
        if (!strcmp(a, "-c") && argi + 1 < argc) { width = atoi(argv[++argi]); continue; }
        if (!strcmp(a, "-x")) continue;
        return errf("invalid option", a);
    }
    FILE *f = (argi < argc) ? fopen(argv[argi], "r") : stdin;
    if (!f) return errf(strerror(errno), argv[argi]);

    static char rows[MAX_ROWS][LINE_CAP];
    int n = 0;
    while (n < MAX_ROWS && readline_into(f, rows[n], LINE_CAP)) n++;
    if (f != stdin) fclose(f);

    if (!table) {
        /* Fill down the columns like `column` with no -t: widest entry sets
         * the column width, then as many columns as fit. */
        int wmax = 1;
        for (int i = 0; i < n; i++) {
            int l = (int)strlen(rows[i]);
            if (l > wmax) wmax = l;
        }
        int cols = width / (wmax + 2);
        if (cols < 1) cols = 1;
        int lines = (n + cols - 1) / cols;
        for (int r = 0; r < lines; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = c * lines + r;
                if (idx >= n) continue;
                printf("%-*s", (c == cols - 1) ? 0 : wmax + 2, rows[idx]);
            }
            printf("\n");
        }
        return 0;
    }

    #define MAXCOL 32
    static char cell[MAX_ROWS][MAXCOL][128];
    static int  ncell[MAX_ROWS];
    int wid[MAXCOL];
    memset(wid, 0, sizeof wid);
    for (int i = 0; i < n; i++) {
        const char *p = rows[i];
        int c = 0;
        while (*p && c < MAXCOL) {
            if (sep) { while (*p == sep) p++; }
            else     { while (*p == ' ' || *p == '\t') p++; }
            if (!*p) break;
            size_t k = 0;
            while (*p && (sep ? *p != sep : (*p != ' ' && *p != '\t'))) {
                if (k + 1 < sizeof cell[0][0]) cell[i][c][k++] = *p;
                p++;
            }
            cell[i][c][k] = 0;
            if ((int)k > wid[c]) wid[c] = (int)k;
            c++;
        }
        ncell[i] = c;
    }
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < ncell[i]; c++) {
            if (c + 1 == ncell[i]) printf("%s", cell[i][c]);
            else                   printf("%-*s  ", wid[c], cell[i][c]);
        }
        printf("\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ colrm */
static int cmd_colrm(int argc, char **argv) {
    long start = 0, stop = 0;
    if (argc > 1) start = atol(argv[1]);
    if (argc > 2) stop  = atol(argv[2]);
    char line[LINE_CAP];
    while (readline_into(stdin, line, sizeof line)) {
        long col = 1;
        for (const char *p = line; *p; p++, col++) {
            /* Columns are 1-based and inclusive; with no stop, everything
             * from `start` on goes. */
            if (start && col >= start && (!stop || col <= stop)) continue;
            putchar(*p);
        }
        putchar('\n');
    }
    return 0;
}

/* --------------------------------------------------------------------- col
 * Removes backspace overstrike (the "X\bX" bolding roff emits) and turns a
 * lone CR into a line break. Reverse line feeds are not produced by anything
 * on this system, so they are not synthesised. */
static int cmd_col(int argc, char **argv) {
    (void)argc; (void)argv;
    int c, prev = -1;
    char out[LINE_CAP];
    size_t n = 0;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\b') { if (n) n--; prev = c; continue; }
        if (c == '\r') { c = '\n'; }
        if (c == '\n') {
            out[n] = 0;
            printf("%s\n", out);
            n = 0; prev = c;
            continue;
        }
        if (n + 1 < sizeof out) out[n++] = (char)c;
        prev = c;
    }
    (void)prev;
    if (n) { out[n] = 0; printf("%s\n", out); }
    return 0;
}

/* ------------------------------------------------------------------ rename
 * util-linux form: rename FROM TO FILE...  -- replaces the FIRST occurrence
 * of FROM in each name. (That is not the Perl `rename` with a regex; the
 * util-linux one is what ships in a base install.) */
static int cmd_rename(int argc, char **argv) {
    int verbose = 0, argi = 1, rc = 0;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-v")) { verbose = 1; continue; }
        if (!strcmp(argv[argi], "-n")) { continue; }
        break;
    }
    if (argi + 2 >= argc) {
        fprintf(stderr, "usage: rename [-v] FROM TO FILE...\n");
        return 1;
    }
    const char *from = argv[argi], *to = argv[argi + 1];
    size_t fl = strlen(from);
    for (int i = argi + 2; i < argc; i++) {
        const char *hit = strstr(argv[i], from);
        if (!hit) continue;
        char nn[512];
        size_t pre = (size_t)(hit - argv[i]);
        if (pre >= sizeof nn) continue;
        memcpy(nn, argv[i], pre);
        nn[pre] = 0;
        snprintf(nn + pre, sizeof nn - pre, "%s%s", to, hit + fl);
        if (rename(argv[i], nn) != 0) {
            fprintf(stderr, "rename: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        } else if (verbose) {
            printf("`%s' -> `%s'\n", argv[i], nn);
        }
    }
    return rc;
}

/* ----------------------------------------------------------------- whereis */
static int cmd_whereis(int argc, char **argv) {
    static const char *bindirs[] = { "/bin", "/usr/bin", "/sbin", "/usr/sbin", 0 };
    static const char *mandirs[] = { "/usr/share/man/man1", "/usr/man/man1", 0 };
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        printf("%s:", argv[i]);
        for (int d = 0; bindirs[d]; d++) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s", bindirs[d], argv[i]);
            struct stat st;
            if (stat(p, &st) == 0) printf(" %s", p);
        }
        for (int d = 0; mandirs[d]; d++) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s.1", mandirs[d], argv[i]);
            struct stat st;
            if (stat(p, &st) == 0) printf(" %s", p);
        }
        printf("\n");
    }
    return 0;
}

/* ------------------------------------------------------------------- namei
 * Walks a path one component at a time and says what each one IS. The point
 * is to find WHERE a lookup fails, so a missing component is reported and
 * the walk stops there rather than the whole path returning one ENOENT. */
static int cmd_namei(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        printf("f: %s\n", argv[i]);
        char acc[512];
        size_t al = 0;
        const char *p = argv[i];
        if (*p == '/') { printf(" d /\n"); acc[al++] = '/'; p++; }
        while (*p) {
            char comp[256];
            size_t k = 0;
            while (*p && *p != '/' && k + 1 < sizeof comp) comp[k++] = *p++;
            comp[k] = 0;
            while (*p == '/') p++;
            if (!k) continue;
            if (al && acc[al-1] != '/') acc[al++] = '/';
            for (size_t q = 0; q < k && al + 1 < sizeof acc; q++) acc[al++] = comp[q];
            acc[al] = 0;
            struct stat st;
            if (stat(acc, &st) != 0) {
                printf(" ? %s - %s\n", comp, strerror(errno));
                rc = 1;
                break;
            }
            char t = S_ISDIR(st.st_mode) ? 'd' : '-';
            printf(" %c %s\n", t, comp);
        }
    }
    return rc;
}

/* ---------------------------------------------------------------- uuidgen */
static int cmd_uuidgen(int argc, char **argv) {
    (void)argc; (void)argv;
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return errf("no entropy source (/dev/urandom)", 0);
    size_t got = 0;
    while (got < sizeof b) {
        long r = read(fd, b + got, sizeof b - got);
        if (r <= 0) { close(fd); return errf("short read from /dev/urandom", 0); }
        got += (size_t)r;
    }
    close(fd);
    /* RFC 4122 version 4, variant 1 -- without these two the string is just
     * random hex and other tools will reject it as a UUID. */
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
           b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
           b[10],b[11],b[12],b[13],b[14],b[15]);
    return 0;
}

/* ---------------------------------------------------------------- setterm */
static int cmd_setterm(int argc, char **argv) {
    int did = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--clear"))        { printf("\033[H\033[2J"); did = 1; continue; }
        if (!strcmp(a, "--reset"))        { printf("\033c");         did = 1; continue; }
        if (!strcmp(a, "--cursor") && i + 1 < argc) {
            printf(strcmp(argv[++i], "on") == 0 ? "\033[?25h" : "\033[?25l");
            did = 1; continue;
        }
        if (!strcmp(a, "--bold") && i + 1 < argc) {
            printf(strcmp(argv[++i], "on") == 0 ? "\033[1m" : "\033[22m");
            did = 1; continue;
        }
        if (!strcmp(a, "--inversescreen") && i + 1 < argc) {
            printf(strcmp(argv[++i], "on") == 0 ? "\033[?5h" : "\033[?5l");
            did = 1; continue;
        }
        fprintf(stderr, "setterm: unsupported option %s\n", a);
        return 1;
    }
    if (!did) fprintf(stderr, "usage: setterm [--clear] [--reset] "
                              "[--cursor on|off] [--bold on|off]\n");
    fflush(stdout);
    return 0;
}

/* ------------------------------------------------------------------ lsblk */
static void human(uint64_t bytes, char *out, size_t cap) {
    static const char *u[] = { "B", "K", "M", "G", "T" };
    int i = 0;
    uint64_t v = bytes, rem = 0;
    while (v >= 1024 && i < 4) { rem = v % 1024; v /= 1024; i++; }
    if (i && v < 10) snprintf(out, cap, "%llu.%llu%s",
                              (unsigned long long)v,
                              (unsigned long long)(rem * 10 / 1024), u[i]);
    else             snprintf(out, cap, "%llu%s", (unsigned long long)v, u[i]);
}

static int cmd_lsblk(int argc, char **argv) {
    (void)argc; (void)argv;
    static struct abi_blk_info dev[ABI_BLK_MAX_DEVICES];
    long n = sc2(ABI_SYS_BLK_LIST, (long)(uintptr_t)dev, (long)ABI_BLK_MAX_DEVICES);
    if (n < 0) return errf("cannot list block devices", 0);
    printf("%-16s %-6s %-10s %-8s %s\n", "NAME", "TYPE", "SIZE", "FSTYPE", "LABEL");
    for (long i = 0; i < n; i++) {
        char sz[32];
        human(dev[i].sector_count * 512ULL, sz, sizeof sz);
        const char *type = dev[i].class == 1 ? "disk"
                         : dev[i].class == 2 ? "part" : "misc";
        printf("%-16s %-6s %-10s %-8s %s\n",
               dev[i].name, type, sz,
               dev[i].fs[0] ? dev[i].fs : "-",
               dev[i].label[0] ? dev[i].label : "");
    }
    return 0;
}

/* ---------------------------------------------------------------- findmnt */
static int cmd_findmnt(int argc, char **argv) {
    const char *want = 0;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') { want = argv[i]; break; }
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return errf("cannot read /proc/mounts", 0);
    printf("%-20s %-16s %-10s %s\n", "TARGET", "SOURCE", "FSTYPE", "OPTIONS");
    char line[LINE_CAP];
    int found = 0;
    while (readline_into(f, line, sizeof line)) {
        char src[128] = "", tgt[128] = "", typ[64] = "", opt[128] = "";
        /* "<dev> <point> <type> <opts> 0 0" */
        if (sscanf(line, "%127s %127s %63s %127s", src, tgt, typ, opt) < 3) continue;
        if (want && strcmp(tgt, want) && strcmp(src, want)) continue;
        printf("%-20s %-16s %-10s %s\n", tgt, src, typ, opt);
        found = 1;
    }
    fclose(f);
    return want && !found ? 1 : 0;
}

/* ------------------------------------------------------------------- lsns */
static int cmd_lsns(int argc, char **argv) {
    const char *pid = "self";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) { pid = argv[++i]; continue; }
    }
    char dir[128];
    snprintf(dir, sizeof dir, "/proc/%s/ns", pid);
    DIR *d = opendir(dir);
    if (!d) return errf("cannot read the namespace directory", dir);
    printf("%-12s %s\n", "TYPE", "PATH");
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        printf("%-12s %s/%s\n", e->d_name, dir, e->d_name);
        n++;
    }
    closedir(d);
    if (!n) fprintf(stderr, "lsns: no namespaces are exposed for %s\n", pid);
    return 0;
}

/* ------------------------------------------------------------------ wdctl */
static int cmd_wdctl(int argc, char **argv) {
    (void)argc; (void)argv;
    struct abi_wdog_status st;
    if (tobywdog_status(&st) != 0) return errf("cannot read the watchdog", 0);
    printf("Device:        software watchdog (tobyOS)\n");
    printf("Enabled:       %s\n", st.enabled ? "yes" : "no");
    printf("Timeout:       %u ms\n", (unsigned)st.timeout_ms);
    printf("Kernel kicks:  %llu (last %llu ms ago)\n",
           (unsigned long long)st.kernel_heartbeats,
           (unsigned long long)st.ms_since_kernel_kick);
    printf("Sched kicks:   %llu (last %llu ms ago)\n",
           (unsigned long long)st.sched_heartbeats,
           (unsigned long long)st.ms_since_sched_kick);
    printf("Syscall kicks: %llu\n", (unsigned long long)st.syscall_heartbeats);
    printf("Events:        %llu\n", (unsigned long long)st.event_count);
    if (st.event_count)
        printf("Last event:    kind %s at %llu ms\n",
               tobywdog_kind_str(st.last_event_kind),
               (unsigned long long)st.last_event_ms);
    return 0;
}

/* ---- the four with nothing to read ------------------------------------- */
static int no_source(const char *what, const char *why) {
    fprintf(stderr, "%s: %s\n", g_prog, why);
    (void)what;
    return 1;
}
static int cmd_prlimit(int argc, char **argv) {
    (void)argc; (void)argv;
    return no_source("prlimit",
        "resource limits are not available: this kernel has no "
        "getrlimit/setrlimit syscall, so there is nothing to read or set");
}
static int cmd_lslocks(int argc, char **argv) {
    (void)argc; (void)argv;
    return no_source("lslocks",
        "file locks cannot be listed: there is no /proc/locks on this system. "
        "An empty table would claim no locks are held, which is not known");
}
static int cmd_lsipc(int argc, char **argv) {
    (void)argc; (void)argv;
    return no_source("lsipc",
        "the kernel implements SysV IPC but exposes no listing interface "
        "(no /proc/sysvipc and no syscall), so there is nothing to enumerate");
}
static int cmd_zramctl(int argc, char **argv) {
    (void)argc; (void)argv;
    return no_source("zramctl",
        "zram is compiled in but reports no devices to userland, and "
        "/proc/meminfo reports SwapTotal as 0 regardless -- there is no "
        "honest number to print");
}

/* ---- dispatch ---------------------------------------------------------- */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "column",  cmd_column },
    { "colrm",   cmd_colrm },
    { "col",     cmd_col },
    { "rename",  cmd_rename },
    { "whereis", cmd_whereis },
    { "namei",   cmd_namei },
    { "uuidgen", cmd_uuidgen },
    { "setterm", cmd_setterm },
    { "lsblk",   cmd_lsblk },
    { "findmnt", cmd_findmnt },
    { "lsns",    cmd_lsns },
    { "wdctl",   cmd_wdctl },
    { "prlimit", cmd_prlimit },
    { "lslocks", cmd_lslocks },
    { "lsipc",   cmd_lsipc },
    { "zramctl", cmd_zramctl },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "util";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    g_prog = base;
    for (int i = 0; g_applets[i].name; i++)
        if (!strcmp(base, g_applets[i].name))
            return g_applets[i].fn(argc, argv);
    if (argc > 1) {
        for (int i = 0; g_applets[i].name; i++) {
            if (strcmp(argv[1], g_applets[i].name)) continue;
            g_prog = g_applets[i].name;
            return g_applets[i].fn(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "util: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
