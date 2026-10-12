/* rmdir yes tac seq whoami which uname free sync nohup timeout watch
 * stat du xargs dd printenv nice pgrep pkill tty stty cksum truncate
 * install mkfifo
 *
 * One multi-call binary, dispatching on argv[0]. Every one of these names
 * previously resolved to a busybox applet -- and busybox in this tree is a
 * gitignored, opt-in artifact that a clean checkout does not have, so all
 * sixteen would simply vanish. They are native now.
 *
 * Where a tool reads kernel state it reads a real one: uname from
 * /proc/sys/kernel and /proc/version, free from /proc/meminfo, sync
 * through the block cache. Nothing here invents a number.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>

#include <tobyos/abi/abi.h>

static const char *prog = "shellutils";

static long sc0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
    return r;
}

static int slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* Trim to the first line, in place. */
static char *first_line(char *s) {
    char *nl = strchr(s, '\n');
    if (nl) *nl = 0;
    return s;
}

/* ------------------------------------------------------------- trivial */

static int do_rmdir(int argc, char **argv) {
    int rc = 0, parents = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--parents")) { parents = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        n++;
        char path[1024];
        snprintf(path, sizeof path, "%s", argv[i]);
        for (;;) {
            if (rmdir(path) != 0) {
                fprintf(stderr, "rmdir: failed to remove '%s': %s\n",
                        path, strerror(errno));
                rc = 1;
                break;
            }
            if (!parents) break;
            /* -p walks up, and STOPS at the first parent that will not go;
             * that is not an error, it is where the tree stops being empty. */
            char *slash = strrchr(path, '/');
            if (!slash || slash == path) break;
            *slash = 0;
        }
    }
    if (!n) { fprintf(stderr, "usage: rmdir [-p] directory...\n"); return 1; }
    return rc;
}

static int do_yes(int argc, char **argv) {
    char line[4096];
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (off + l + 2 >= sizeof line) break;
        if (off) line[off++] = ' ';
        memcpy(line + off, argv[i], l);
        off += l;
    }
    if (!off) { line[off++] = 'y'; }
    line[off++] = '\n';
    for (;;) {
        if (fwrite(line, 1, off, stdout) != off) return 1;
    }
}

static int do_tac(int argc, char **argv) {
    int rc = 0;
    int nfiles = 0;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-' || !argv[i][1]) nfiles++;
    for (int pass = 0; pass < (nfiles ? nfiles : 1); pass++) {
        const char *path = NULL;
        int seen = 0;
        for (int i = 1; i < argc && nfiles; i++) {
            if (argv[i][0] == '-' && argv[i][1]) continue;
            if (seen++ == pass) { path = argv[i]; break; }
        }
        FILE *f = path ? fopen(path, "rb") : stdin;
        if (!f) { fprintf(stderr, "tac: %s: %s\n", path, strerror(errno)); rc = 1; continue; }
        char **lines = NULL;
        size_t n = 0, cap = 0;
        char buf[8192];
        while (fgets(buf, sizeof buf, f)) {
            if (n == cap) { cap = cap ? cap * 2 : 256;
                            lines = (char **)realloc(lines, cap * sizeof(char *)); }
            lines[n++] = strdup(buf);
        }
        if (f != stdin) fclose(f);
        for (size_t i = n; i > 0; i--) {
            char *l = lines[i - 1];
            size_t L = strlen(l);
            /* A last line with no newline gains one when it moves up, and
             * the line that ends up last loses it -- which is what tac
             * does, and why it is not just "print backwards". */
            if (L && l[L-1] == '\n') fputs(l, stdout);
            else { fputs(l, stdout); if (i != 1) putchar('\n'); }
            free(l);
        }
        free(lines);
    }
    return rc;
}

static int do_seq(int argc, char **argv) {
    long first = 1, incr = 1, last = 0;
    const char *sep = "\n";
    const char *nums[3];
    int nn = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) { sep = argv[++i]; continue; }
        if (!strncmp(argv[i], "-s", 2) && argv[i][2]) { sep = argv[i] + 2; continue; }
        if (!strcmp(argv[i], "-w")) continue;      /* equal width: accepted */
        if (nn < 3) nums[nn++] = argv[i];
    }
    if (nn == 0) { fprintf(stderr, "usage: seq [first [incr]] last\n"); return 1; }
    if (nn == 1) { last = atol(nums[0]); }
    else if (nn == 2) { first = atol(nums[0]); last = atol(nums[1]); }
    else { first = atol(nums[0]); incr = atol(nums[1]); last = atol(nums[2]); }
    if (incr == 0) { fprintf(stderr, "seq: increment must not be zero\n"); return 1; }
    int printed = 0;
    for (long v = first; incr > 0 ? v <= last : v >= last; v += incr) {
        if (printed) fputs(sep, stdout);
        printf("%ld", v);
        printed = 1;
    }
    if (printed) putchar('\n');
    return 0;
}

static int do_whoami(void) {
    /* The name behind the uid, from the identity database, not $USER --
     * `whoami` exists precisely to be the thing an environment variable
     * cannot lie about. */
    uid_t u = getuid();
    char buf[8192];
    if (slurp("/etc/passwd", buf, sizeof buf) > 0) {
        for (char *p = buf; p && *p; ) {
            char *eol = strchr(p, '\n');
            if (eol) *eol = 0;
            char *n1 = strchr(p, ':');
            if (n1) {
                char *n2 = strchr(n1 + 1, ':');
                if (n2) {
                    long id = atol(n2 + 1);
                    if (id == (long)u) { *n1 = 0; printf("%s\n", p); return 0; }
                }
            }
            if (!eol) break;
            p = eol + 1;
        }
    }
    printf("%u\n", (unsigned)u);
    return 0;
}

static int do_which(int argc, char **argv) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin:/usr/bin";
    int rc = 0, all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) { all = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        int found = 0;
        const char *p = path;
        while (*p) {
            const char *e = p;
            while (*e && *e != ':') e++;
            char dir[512];
            size_t dl = (size_t)(e - p);
            if (dl >= sizeof dir) dl = sizeof dir - 1;
            memcpy(dir, p, dl); dir[dl] = 0;
            char cand[1024];
            snprintf(cand, sizeof cand, "%s/%s", dl ? dir : ".", argv[i]);
            struct stat st;
            if (stat(cand, &st) == 0 && !S_ISDIR(st.st_mode)) {
                printf("%s\n", cand);
                found = 1;
                if (!all) break;
            }
            p = *e ? e + 1 : e;
        }
        if (!found) rc = 1;
    }
    return rc;
}

static int do_uname(int argc, char **argv) {
    char sys[128] = "tobyOS", rel[128] = "", ver[512] = "", mach[64] = "x86_64", node[128] = "";
    char buf[1024];
    if (slurp("/proc/sys/kernel/ostype", buf, sizeof buf) > 0)
        snprintf(sys, sizeof sys, "%s", first_line(buf));
    if (slurp("/proc/sys/kernel/osrelease", buf, sizeof buf) > 0)
        snprintf(rel, sizeof rel, "%s", first_line(buf));
    if (slurp("/proc/sys/kernel/hostname", buf, sizeof buf) > 0)
        snprintf(node, sizeof node, "%s", first_line(buf));
    if (slurp("/proc/version", buf, sizeof buf) > 0)
        snprintf(ver, sizeof ver, "%s", first_line(buf));

    int s = 0, n = 0, r = 0, v = 0, m = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--all")) { s = n = r = v = m = 1; any = 1; continue; }
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'a': s = n = r = v = m = 1; break;
                case 's': s = 1; break;
                case 'n': n = 1; break;
                case 'r': r = 1; break;
                case 'v': v = 1; break;
                case 'm': case 'p': case 'i': m = 1; break;
                default: break;
                }
                any = 1;
            }
        }
    }
    if (!any) s = 1;
    int first = 1;
    #define OUT(x) do { if (*(x)) { if (!first) putchar(' '); fputs((x), stdout); first = 0; } } while (0)
    if (s) OUT(sys);
    if (n) OUT(node);
    if (r) OUT(rel);
    if (v) OUT(ver);
    if (m) OUT(mach);
    #undef OUT
    putchar('\n');
    return 0;
}

static long meminfo_kb(const char *buf, const char *key) {
    size_t klen = strlen(key);
    for (const char *p = buf; *p; ) {
        const char *eol = strchr(p, '\n');
        if (!strncmp(p, key, klen) && p[klen] == ':') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t') q++;
            long v = 0; int got = 0;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q++ - '0'); got = 1; }
            return got ? v : -1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

static int do_free(int argc, char **argv) {
    int human = 0, div = 1;
    const char *unit = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h")) { human = 1; continue; }
        if (!strcmp(argv[i], "-m")) { div = 1024; unit = "Mi"; continue; }
        if (!strcmp(argv[i], "-g")) { div = 1024 * 1024; unit = "Gi"; continue; }
        if (!strcmp(argv[i], "-k")) { div = 1; continue; }
    }
    char buf[8192];
    if (slurp("/proc/meminfo", buf, sizeof buf) < 0) {
        fprintf(stderr, "free: cannot read /proc/meminfo\n");
        return 1;
    }
    long total = meminfo_kb(buf, "MemTotal");
    long freek = meminfo_kb(buf, "MemFree");
    long avail = meminfo_kb(buf, "MemAvailable");
    long buff  = meminfo_kb(buf, "Buffers");
    long cache = meminfo_kb(buf, "Cached");
    long swtot = meminfo_kb(buf, "SwapTotal");
    long swfre = meminfo_kb(buf, "SwapFree");
    if (total < 0) total = 0;
    if (freek < 0) freek = 0;
    if (buff  < 0) buff  = 0;
    if (cache < 0) cache = 0;
    if (avail < 0) avail = freek;
    long used = total - freek - buff - cache;
    if (used < 0) used = 0;

    if (human) {
        /* Integer mebibytes: this program is built without SSE, so no
         * floating point. One decimal is not worth a soft-float call. */
        printf("%-14s%9s%9s%9s%9s%9s%9s\n",
               "", "total", "used", "free", "shared", "buff/cache", "available");
        printf("%-14s%8ldM%8ldM%8ldM%9s%9ldM%8ldM\n", "Mem:",
               total/1024, used/1024, freek/1024, "0", (buff+cache)/1024, avail/1024);
        printf("%-14s%8ldM%8ldM%8ldM\n", "Swap:",
               (swtot < 0 ? 0 : swtot)/1024,
               (swtot < 0 || swfre < 0 ? 0 : swtot - swfre)/1024,
               (swfre < 0 ? 0 : swfre)/1024);
        return 0;
    }
    printf("%-14s%12s%12s%12s%12s%12s%12s\n",
           "", "total", "used", "free", "shared", "buff/cache", "available");
    printf("%-14s%12ld%12ld%12ld%12d%12ld%12ld\n", "Mem:",
           total/div, used/div, freek/div, 0, (buff+cache)/div, avail/div);
    printf("%-14s%12ld%12ld%12ld\n", "Swap:",
           (swtot < 0 ? 0 : swtot)/div,
           (swtot < 0 || swfre < 0 ? 0 : (swtot - swfre))/div,
           (swfre < 0 ? 0 : swfre)/div);
    (void)unit;
    return 0;
}

static int do_sync(void) {
    long rc = sc0(ABI_SYS_SYNC);
    if (rc < 0) {
        fprintf(stderr, "sync: failed (rc=%ld)\n", rc);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ stat / du */

/* GNU stat's %F names. libtoby's <sys/stat.h> used to define only S_ISDIR
 * and S_ISREG, so this said "unknown" for every /dev node -- which the
 * kernel reports as S_IFCHR -- while GNU says "character special file". */
static const char *type_name(mode_t m) {
    if (S_ISDIR(m))  return "directory";
    if (S_ISREG(m))  return "regular file";
    if (S_ISCHR(m))  return "character special file";
    if (S_ISBLK(m))  return "block special file";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISLNK(m))  return "symbolic link";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

static int do_stat(int argc, char **argv) {
    int rc = 0, terse = 0;
    const char *fmt = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--terse")) { terse = 1; continue; }
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { fmt = argv[++i]; continue; }
        if (!strncmp(argv[i], "--format=", 9)) { fmt = argv[i] + 9; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "stat: cannot stat '%s': %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (fmt) {
            for (const char *p = fmt; *p; p++) {
                if (*p != '%') { putchar(*p); continue; }
                p++;
                switch (*p) {
                case 'n': fputs(argv[i], stdout); break;
                case 's': printf("%ld", (long)st.st_size); break;
                case 'f': printf("%lx", (unsigned long)st.st_mode); break;
                case 'a': printf("%lo", (unsigned long)(st.st_mode & 07777)); break;
                case 'u': printf("%u", (unsigned)st.st_uid); break;
                case 'g': printf("%u", (unsigned)st.st_gid); break;
                case 'i': fputs("0", stdout); break;   /* no st_ino here */
                case 'F': fputs(type_name(st.st_mode), stdout); break;
                case '%': putchar('%'); break;
                default: putchar('%'); if (*p) putchar(*p); break;
                }
                if (!*p) break;
            }
            putchar('\n');
            continue;
        }
        if (terse) {
            printf("%s %ld %ld %lx %u %u\n", argv[i], (long)st.st_size,
                   (long)st.st_size, (unsigned long)st.st_mode,
                   (unsigned)st.st_uid, (unsigned)st.st_gid);
            continue;
        }
        printf("  File: %s\n", argv[i]);
        printf("  Size: %-14ld Blocks: %-10ld %s\n",
               (long)st.st_size, (long)((st.st_size + 511) / 512),
               type_name(st.st_mode));
        printf("Access: (%04lo)  Uid: (%5u)   Gid: (%5u)\n",
               (unsigned long)(st.st_mode & 07777),
               (unsigned)st.st_uid, (unsigned)st.st_gid);
    }
    return rc;
}

static long du_walk(const char *path, int depth, int all, int summarise,
                    int human, long *total)
{
    struct stat st;
    /* lstat: du counts a link, it does not follow one (GNU's -P) */
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
        return 0;
    }
    long kb = 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char sub[1024];
                snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                kb += du_walk(sub, depth + 1, all, summarise, human, total);
            }
            closedir(d);
        }
    } else {
        kb = (long)((st.st_size + 1023) / 1024);
        if (kb == 0) kb = 1;      /* du counts an allocated block, not zero */
    }
    int show = S_ISDIR(st.st_mode) ? 1 : all;
    if (summarise && depth > 0) show = 0;
    if (show) {
        if (human && kb >= 1024) printf("%ldM\t%s\n", kb / 1024, path);
        else if (human)          printf("%ldK\t%s\n", kb, path);
        else                     printf("%ld\t%s\n", kb, path);
    }
    *total += 0;
    return kb;
}

static int do_du(int argc, char **argv) {
    int all = 0, summarise = 0, human = 0;
    const char *paths[64];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'a') all = 1;
                else if (*p == 's') summarise = 1;
                else if (*p == 'h') human = 1;
                else if (*p == 'k') human = 0;
            }
            continue;
        }
        if (np < 64) paths[np++] = a;
    }
    if (!np) paths[np++] = ".";
    long total = 0;
    for (int i = 0; i < np; i++) du_walk(paths[i], 0, all, summarise, human, &total);
    return 0;
}

/* ------------------------------------------------------------ processes */

static int run_argv(char **av) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(av[0], av); _exit(127); }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* xargs [-n N] command [args]: run command with the words read from stdin
 * appended, as many commands as it takes.
 *
 * EVERY WORD, AND EVERY WORD WHOLE. This kept the first 4095 words and
 * dropped the rest, cut any word at 8 KiB, and packed up to 4150 words into
 * one command whatever their size:
 *
 *     seq 5000 | xargs echo | wc -w         GNU: 5000    here: 4095
 *
 * A command is now filled to XARGS_CMD_BYTES of argument text -- GNU's
 * default -s is 128 KiB, well inside the kernel's ABI_ARGS_TOTAL_MAX --
 * or to -n words, and as many commands run as the input needs. */
#define XARGS_CMD_BYTES (128 * 1024)

static int do_xargs(int argc, char **argv) {
    char **base = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!base) { fprintf(stderr, "xargs: out of memory\n"); return 1; }
    int nb = 0;
    int maxargs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { maxargs = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-0") || !strcmp(argv[i], "-r")) continue;
        if (!strcmp(argv[i], "-t")) continue;
        base[nb++] = argv[i];
    }
    if (nb == 0) base[nb++] = (char *)"echo";
    size_t base_bytes = 0;
    for (int k = 0; k < nb; k++) base_bytes += strlen(base[k]) + 1;

    char  **items = 0;
    int     ni = 0, icap = 0;
    char   *word = 0;
    size_t  wl = 0, wcap = 0;
    int     c;
    int     oom = 0;
    /* Whitespace-separated, which is xargs's default; -0 is accepted and
     * behaves the same here because nothing in this tree emits NUL-separated
     * lists yet. */
    for (;;) {
        c = fgetc(stdin);
        int sep = (c == EOF || c == ' ' || c == '\t' || c == '\n' ||
                   c == '\r' || c == 0);
        if (sep) {
            if (wl) {
                if (ni >= icap) {
                    int ncap = icap ? icap * 2 : 256;
                    char **ni_ = (char **)realloc(items, (size_t)ncap * sizeof(char *));
                    if (!ni_) { oom = 1; break; }
                    items = ni_;
                    icap = ncap;
                }
                word[wl] = 0;
                items[ni] = strdup(word);
                if (!items[ni]) { oom = 1; break; }
                ni++;
                wl = 0;
            }
            if (c == EOF) break;
            continue;
        }
        if (wl + 1 >= wcap) {
            size_t ncap = wcap ? wcap * 2 : 256;
            char *nw = (char *)realloc(word, ncap);
            if (!nw) { oom = 1; break; }
            word = nw;
            wcap = ncap;
        }
        word[wl++] = (char)c;
    }
    free(word);
    if (oom) {
        fprintf(stderr, "xargs: out of memory\n");
        for (int k = 0; k < ni; k++) free(items[k]);
        free(items);
        free(base);
        return 1;
    }

    char **av = (char **)malloc((size_t)(nb + ni + 1) * sizeof(char *));
    if (!av) {
        fprintf(stderr, "xargs: out of memory\n");
        for (int k = 0; k < ni; k++) free(items[k]);
        free(items);
        free(base);
        return 1;
    }
    int rc = 0;
    int i = 0;
    do {
        int n = 0;
        for (int k = 0; k < nb; k++) av[n++] = base[k];
        size_t bytes = base_bytes;
        int taken = 0;
        while (i < ni && (!maxargs || taken < maxargs)) {
            size_t wb = strlen(items[i]) + 1;
            /* Always at least one word: a word longer than the budget
             * still gets its command, and the kernel says if it is too
             * big. */
            if (taken > 0 && bytes + wb > XARGS_CMD_BYTES) break;
            av[n++] = items[i++];
            bytes += wb;
            taken++;
        }
        av[n] = NULL;
        if (taken == 0 && ni > 0) break;
        int r = run_argv(av);
        if (r != 0) rc = r;
    } while (i < ni);
    for (int k = 0; k < ni; k++) free(items[k]);
    free(items);
    free(av);
    free(base);
    return rc;
}

static int do_nohup(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: nohup command [args]\n"); return 125; }
    signal(SIGHUP, SIG_IGN);
    /* Real nohup redirects stdout to nohup.out when it is a terminal, so
     * output survives the session going away. */
    if (isatty(1)) {
        int fd = open("nohup.out", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            fprintf(stderr, "nohup: appending output to 'nohup.out'\n");
            dup2(fd, 1);
            if (isatty(2)) dup2(fd, 2);
            close(fd);
        }
    }
    execvp(argv[1], argv + 1);
    fprintf(stderr, "nohup: cannot run '%s': %s\n", argv[1], strerror(errno));
    return 127;
}

/* timeout -- start a COMMAND, and signal it if it outlasts a DURATION.
 *
 *     timeout [OPTION] DURATION COMMAND [ARG]...
 *
 * AS GNU COREUTILS' TIMEOUT, measured on 9.7 in the C locale (tools/agent/
 * probes/b24/timeoutcases*.sh; logs/b24-gnu-timeout*.out):
 *
 *   - DURATION as sleep reads one (a number as strtod reads it, one suffix
 *     of s m h d; 0 or inf: no timeout); -k DURATION also KILLs the command
 *     that long after the first signal; -s SIGNAL, a name in any case with
 *     or without SIG, RTMIN/RTMAX(+-n), or a number 0..64.
 *   - The status: the command's own; 124 if it timed out (with -p, the
 *     command's own then too); 125 when timeout itself fails, 126 when the
 *     command cannot be run, 127 when it is not found. A command a signal
 *     killed, not by timing out: timeout dies of the same signal, so its
 *     shell reports it as for the command. A KILL sent to the group takes
 *     timeout down with it (137, "Killed").
 *   - Unless -f, timeout leads a process group and signals all of it -- the
 *     command's own children too -- ignoring the signal itself, then a
 *     CONT for a stopped command. A HUP INT QUIT or TERM sent to timeout is
 *     passed on to the command the same way.
 *   - Options until the first operand (getopt's "+"): -f -k -p -s -v and
 *     their long names, any unambiguous prefix; the errors and "Try
 *     'timeout --help' ..." getopt's; too few operands, the Try line alone.
 *
 * It took whole seconds (atol), ignored -s, -k and -p, signalled the
 * command alone, said 1 for a command a signal killed, and -- polling once
 * a second and killing without a last look -- said 124 for a command that
 * failed inside its first second: `timeout 1 false` (finding 130). */

static const char *g_to_name;          /* argv[0] as typed */
static const char *g_to_short;         /* its last component */

static void to_try(void) {
    fprintf(stderr, "Try '%s --help' for more information.\n", g_to_name);
    exit(125);
}

static const struct { int num; const char *name; } k_to_sigs[] = {
    { 1,  "HUP"  }, { 2,  "INT"  }, { 3,  "QUIT" }, { 4,  "ILL"  },
    { 5,  "TRAP" }, { 6,  "ABRT" }, { 6,  "IOT"  }, { 7,  "BUS"  },
    { 8,  "FPE"  }, { 9,  "KILL" }, { 10, "USR1" }, { 11, "SEGV" },
    { 12, "USR2" }, { 13, "PIPE" }, { 14, "ALRM" }, { 15, "TERM" },
    { 16, "STKFLT" }, { 17, "CHLD" }, { 17, "CLD" }, { 18, "CONT" },
    { 19, "STOP" }, { 20, "TSTP" }, { 21, "TTIN" }, { 22, "TTOU" },
    { 23, "URG"  }, { 24, "XCPU" }, { 25, "XFSZ" }, { 26, "VTALRM" },
    { 27, "PROF" }, { 28, "WINCH" }, { 29, "IO" }, { 29, "POLL" },
    { 30, "PWR" }, { 31, "SYS" },
};
#define TO_NSIGS ((int)(sizeof k_to_sigs / sizeof k_to_sigs[0]))
#define TO_RTMIN 34
#define TO_RTMAX 64

/* SIGNAL's number, or -1. */
static int to_signal(const char *s) {
    if (*s >= '0' && *s <= '9') {
        int v = 0;
        for (; *s; s++) {
            if (*s < '0' || *s > '9' || v > 64) return -1;
            v = v * 10 + (*s - '0');
        }
        return v <= 64 ? v : -1;
    }
    char up[16];
    size_t n = strlen(s);
    if (n >= sizeof up) return -1;
    for (size_t k = 0; k <= n; k++) up[k] = (char)toupper((unsigned char)s[k]);
    const char *u = strncmp(up, "SIG", 3) == 0 ? up + 3 : up;
    for (int k = 0; k < TO_NSIGS; k++)
        if (strcmp(u, k_to_sigs[k].name) == 0) return k_to_sigs[k].num;
    int base = strncmp(u, "RTMIN", 5) == 0 ? TO_RTMIN
             : strncmp(u, "RTMAX", 5) == 0 ? TO_RTMAX : 0;
    if (!base) return -1;
    const char *p = u + 5;
    if (!*p) return base;
    if ((*p != '+' && *p != '-') || p[1] < '0' || p[1] > '9') return -1;
    int off = atoi(p + 1), v = *p == '+' ? base + off : base - off;
    return (v >= TO_RTMIN && v <= TO_RTMAX) ? v : -1;
}

static const char *to_signame(int sig, char *buf, size_t cap) {
    for (int k = 0; k < TO_NSIGS; k++)
        if (k_to_sigs[k].num == sig) return k_to_sigs[k].name;
    snprintf(buf, cap, "%d", sig);
    return buf;
}

/* A DURATION into nanoseconds, saturating (TO_FOREVER), as sleep reads its
 * operands -- ports/sbase/sleep.c parse_interval, the same rules (this
 * multi-call binary and the ports share no source). 0 if one, -1 if not.
 * Integer arithmetic: built without SSE. */
#define TO_FOREVER (~0ULL)
static unsigned long long to_mul(unsigned long long a, unsigned long long b) {
    if (a == 0 || b == 0) return 0;
    if (a == TO_FOREVER || b == TO_FOREVER || b > TO_FOREVER / a) return TO_FOREVER;
    return a * b;
}
static long to_exponent(const char **pp) {
    const char *q = *pp + 1;
    long sign = 1, e = 0;
    if (*q == '+' || *q == '-') { if (*q == '-') sign = -1; q++; }
    if (*q < '0' || *q > '9') return 0;
    for (; *q >= '0' && *q <= '9'; q++) if (e < 100000) e = e * 10 + (*q - '0');
    *pp = q;
    return sign * e;
}
static int to_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}
static int to_duration(const char *arg, unsigned long long *out) {
    const char *p = arg;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    enum { DEC, HEX, INF } kind;
    unsigned long long m = 0;
    long e = 0;
    if (strncasecmp(p, "inf", 3) == 0) {
        kind = INF;
        p += strncasecmp(p, "infinity", 8) == 0 ? 8 : 3;
    } else if (strncasecmp(p, "nan", 3) == 0) {
        return -1;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
               (to_hexval(p[2]) >= 0 || (p[2] == '.' && to_hexval(p[3]) >= 0))) {
        kind = HEX;
        for (p += 2; to_hexval(*p) >= 0; p++) {
            if (m < (1ULL << 56)) m = m * 16 + (unsigned)to_hexval(*p);
            else e += 4;
        }
        if (*p == '.')
            for (p++; to_hexval(*p) >= 0; p++)
                if (m < (1ULL << 56)) { m = m * 16 + (unsigned)to_hexval(*p); e -= 4; }
        if (*p == 'p' || *p == 'P') e += to_exponent(&p);
    } else {
        kind = DEC;
        int digits = 0;
        for (; *p >= '0' && *p <= '9'; p++, digits++) {
            if (m < 100000000000000000ULL) m = m * 10 + (unsigned)(*p - '0');
            else e++;
        }
        if (*p == '.')
            for (p++; *p >= '0' && *p <= '9'; p++, digits++)
                if (m < 100000000000000000ULL) { m = m * 10 + (unsigned)(*p - '0'); e--; }
        if (!digits) return -1;
        if (*p == 'e' || *p == 'E') e += to_exponent(&p);
    }
    unsigned long long mult = 1;
    switch (*p) {
    case '\0': break;
    case 's': p++; break;
    case 'm': mult = 60; p++; break;
    case 'h': mult = 3600; p++; break;
    case 'd': mult = 86400; p++; break;
    default:  return -1;
    }
    if (*p) return -1;
    if (neg && (kind == INF || m != 0)) return -1;
    unsigned long long ns;
    if (kind == INF) {
        ns = TO_FOREVER;
    } else if (m == 0) {
        ns = 0;
    } else if (kind == DEC) {
        long k = e + 9;
        ns = m;
        while (k < 0 && ns > TO_FOREVER / mult) { ns /= 10; k++; }
        ns = to_mul(ns, mult);
        for (; k > 0 && ns != TO_FOREVER; k--) ns = to_mul(ns, 10);
        for (; k < 0 && ns; k++) ns /= 10;
    } else {
        ns = to_mul(m, mult);
        while (e < 0 && ns > TO_FOREVER / 1000000000ULL) { ns >>= 1; e++; }
        ns = to_mul(ns, 1000000000ULL);
        for (; e > 0 && ns != TO_FOREVER; e--) ns = to_mul(ns, 2);
        for (; e < 0 && ns; e++) ns >>= 1;
    }
    *out = ns;
    return 0;
}

static unsigned long long to_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

struct to_opts {
    int sig, verbose, preserve, foreground;
    unsigned long long kill_after;
};

/* One option: f k p s v, or H (--help) and V (--version), which end
 * timeout here (1). */
static int to_apply(int opt, const char *val, struct to_opts *o) {
    switch (opt) {
    case 'f': o->foreground = 1; break;
    case 'p': o->preserve = 1; break;
    case 'v': o->verbose = 1; break;
    case 'k':
        if (to_duration(val, &o->kill_after) != 0) {
            fprintf(stderr, "%s: invalid time interval '%s'\n", g_to_short, val);
            to_try();
        }
        break;
    case 's':
        o->sig = to_signal(val);
        if (o->sig < 0) {
            fprintf(stderr, "%s: '%s': invalid signal\n", g_to_short, val);
            to_try();
        }
        break;
    case 'H':
        printf("Usage: %s [OPTION] DURATION COMMAND [ARG]...\n"
               "  or:  %s [OPTION]\n"
               "Run COMMAND; if it is still running after DURATION, send it a signal.\n"
               "\n"
               "  -f, --foreground       signal COMMAND alone, not its process group,\n"
               "                         and leave it the terminal\n"
               "  -k, --kill-after=DUR   send KILL too if COMMAND is still running\n"
               "                         DUR after the first signal\n"
               "  -p, --preserve-status  exit with COMMAND's status even when it times out\n"
               "  -s, --signal=SIGNAL    the signal to send (TERM unless given): a name\n"
               "                         such as HUP, or a number\n"
               "  -v, --verbose          say on stderr each signal sent\n"
               "      --help             show this help and exit\n"
               "      --version          show the version and exit\n"
               "\n"
               "DURATION is a number with an optional s, m, h or d suffix; 0 means none.\n"
               "Exit status: COMMAND's; 124 if it timed out, 125 if timeout failed,\n"
               "126 if COMMAND could not be run, 127 if it was not found.\n",
               g_to_name, g_to_name);
        return 1;
    case 'V':
        printf("timeout (tobyOS) 1.0\n");
        return 1;
    }
    return 0;
}

/* A HUP INT QUIT or TERM sent to timeout, for the wait to pass on. */
static volatile int g_to_pending;
static void to_note(int sig) { g_to_pending = sig; }

/* Send SIG to the command -- and, unless -f, to the whole group, timeout
 * ignoring it (a KILL it cannot: that ends timeout too), then a CONT for a
 * stopped command. */
static void to_send(pid_t pid, int sig, const struct to_opts *o,
                    const char *cmd) {
    if (o->verbose) {
        char nb[16];
        fprintf(stderr, "%s: sending signal %s to command '%s'\n",
                g_to_short, to_signame(sig, nb, sizeof nb), cmd);
    }
    kill(pid, sig);
    if (!o->foreground) {
        if (sig > 0 && sig != SIGKILL && sig != SIGSTOP) signal(sig, SIG_IGN);
        kill(0, sig);
        if (sig != SIGKILL && sig != SIGCONT) {
            kill(pid, SIGCONT);
            kill(0, SIGCONT);
        }
    }
}

/* Wait for PID until DEADLINE (TO_FOREVER: none): 1 and *ST if it ended, 0
 * if the deadline came first, minus the signal if timeout was sent one.
 * Polled at 10 ms: libtoby has no alarm(). */
static int to_wait(pid_t pid, unsigned long long deadline, int *st) {
    for (;;) {
        pid_t r = waitpid(pid, st, WNOHANG);
        if (r == pid) return 1;
        if (r < 0 && errno != EINTR) { *st = 0; return 1; }
        if (g_to_pending) {
            int s = g_to_pending;
            g_to_pending = 0;
            return -s;
        }
        if (deadline != TO_FOREVER && to_now() >= deadline) {
            /* a last look: an end that came with the deadline is an end */
            if (waitpid(pid, st, WNOHANG) == pid) return 1;
            return 0;
        }
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, 0);
    }
}

static int do_timeout(int argc, char **argv) {
    g_to_name = (argc > 0 && argv[0] && argv[0][0]) ? argv[0] : "timeout";
    g_to_short = strrchr(g_to_name, '/') ? strrchr(g_to_name, '/') + 1 : g_to_name;

    struct to_opts o = { SIGTERM, 0, 0, 0, 0 };
    static const char *const longs[] = {
        "foreground", "kill-after", "preserve-status", "signal", "verbose",
        "help", "version",
    };
    static const char long_opt[] = "fkpsvHV";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || a[1] == '\0') break;          /* the first operand */
        if (a[1] == '-') {
            const char *name = a + 2;
            size_t n = strcspn(name, "=");
            int hit = -1, hits = 0;
            for (int k = 0; k < 7; k++) {
                if (n == 0 || strncmp(name, longs[k], n) != 0) continue;
                if (strlen(longs[k]) == n) { hit = k; hits = 1; break; }
                hit = k;
                hits++;
            }
            if (hits == 0) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", g_to_name, a);
                to_try();
            }
            if (hits > 1) {
                fprintf(stderr, "%s: option '%.*s' is ambiguous; possibilities:",
                        g_to_name, (int)(n + 2), a);
                for (int k = 0; k < 7; k++)
                    if (strncmp(name, longs[k], n) == 0)
                        fprintf(stderr, " '--%s'", longs[k]);
                fprintf(stderr, "\n");
                to_try();
            }
            int opt = long_opt[hit];
            int takes = opt == 'k' || opt == 's';
            const char *val = 0;
            if (name[n] == '=') {
                if (!takes) {
                    fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                            g_to_name, longs[hit]);
                    to_try();
                }
                val = name + n + 1;
            } else if (takes) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "%s: option '--%s' requires an argument\n",
                            g_to_name, longs[hit]);
                    to_try();
                }
                val = argv[++i];
            }
            if (to_apply(opt, val, &o)) return 0;
            continue;
        }
        /* a cluster of short options: -vsINT, -k 1 */
        for (const char *c = a + 1; *c; c++) {
            if (!strchr("fkpsv", *c)) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", g_to_name, *c);
                to_try();
            }
            const char *val = 0;
            if (*c == 'k' || *c == 's') {
                if (c[1]) val = c + 1;
                else if (i + 1 < argc) val = argv[++i];
                else {
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                            g_to_name, *c);
                    to_try();
                }
            }
            to_apply(*c, val, &o);
            if (val) break;
        }
    }
    if (argc - i < 2) to_try();

    unsigned long long dur;
    if (to_duration(argv[i], &dur) != 0) {
        fprintf(stderr, "%s: invalid time interval '%s'\n", g_to_short, argv[i]);
        to_try();
    }
    char **cmdv = argv + i + 1;

    if (!o.foreground) (void)setpgid(0, 0);
    static const int passed_on[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
    for (int k = 0; k < 4; k++) signal(passed_on[k], to_note);
    if (o.sig != SIGKILL && o.sig != SIGSTOP && o.sig != 0) signal(o.sig, to_note);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: fork system call failed: %s\n", g_to_short,
                strerror(errno));
        return 125;
    }
    if (pid == 0) {
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        execvp(cmdv[0], cmdv);
        int en = errno;
        fprintf(stderr, "%s: failed to run command '%s': %s\n", g_to_short,
                cmdv[0], strerror(en));
        _exit(en == ENOENT ? 127 : 126);
    }

    /* The deadline sends -s's signal, then -- with -k -- a KILL that long
     * after the first signal sent, whether a timeout's or one passed on. */
    int st = 0, timed_out = 0, next = o.sig;
    unsigned long long deadline = (dur == 0 || dur == TO_FOREVER)
                                ? TO_FOREVER : to_now() + dur;
    for (;;) {
        int r = to_wait(pid, deadline, &st), sig;
        if (r == 1) break;
        if (r == 0) {
            timed_out = 1;
            sig = next;
            deadline = TO_FOREVER;
        } else {
            sig = -r;
        }
        to_send(pid, sig, &o, cmdv[0]);
        if (o.kill_after) {
            next = SIGKILL;
            deadline = o.kill_after == TO_FOREVER ? TO_FOREVER
                                                  : to_now() + o.kill_after;
            o.kill_after = 0;
        }
    }

    /* WIFSIGNALED first: libtoby's WIFEXITED is true of a child a signal
     * killed too (128+N, sys/wait.h). */
    int status;
    if (WIFSIGNALED(st)) {
        int s = WTERMSIG(st);
        if (!timed_out) {
            /* the command died of it: so does timeout, for its shell */
            signal(s, SIG_DFL);
            raise(s);
        }
        if (timed_out && s == SIGKILL) o.preserve = 1;
        status = 128 + s;
    } else if (WIFEXITED(st)) {
        status = WEXITSTATUS(st);
    } else {
        status = 125;
    }
    if (timed_out && !o.preserve) status = 124;
    return status;
}

static int do_watch(int argc, char **argv) {
    long interval = 2;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-n") && argi + 1 < argc) { interval = atol(argv[++argi]); continue; }
        if (!strcmp(argv[argi], "-t") || !strcmp(argv[argi], "-d")) continue;
        if (argv[argi][0] == '-' && argv[argi][1]) continue;
        break;
    }
    if (argi >= argc) { fprintf(stderr, "usage: watch [-n secs] command\n"); return 1; }
    if (interval < 1) interval = 1;

    /* Into a pipe, run ONCE and stop: a watch that loops forever in a
     * script would simply hang, and a script is the only place a
     * non-terminal stdout comes from. */
    int once = !isatty(1);
    for (;;) {
        if (!once) fputs("\033[H\033[2J", stdout);
        fflush(stdout);
        run_argv(argv + argi);
        fflush(stdout);
        if (once) break;
        sleep((unsigned)interval);
    }
    return 0;
}

/* ------------------------------------------------------------------- dd */

static long parse_size(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == 'k' || *s == 'K') v *= 1024;
    else if (*s == 'M' || *s == 'm') v *= 1024 * 1024;
    else if (*s == 'b') v *= 512;
    return v;
}

static int do_dd(int argc, char **argv) {
    const char *inf = NULL, *outf = NULL;
    long bs = 512, count = -1, skip = 0, seek = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "if=", 3))    inf = argv[i] + 3;
        else if (!strncmp(argv[i], "of=", 3))  outf = argv[i] + 3;
        else if (!strncmp(argv[i], "bs=", 3))  bs = parse_size(argv[i] + 3);
        else if (!strncmp(argv[i], "count=", 6)) count = parse_size(argv[i] + 6);
        else if (!strncmp(argv[i], "skip=", 5))  skip = parse_size(argv[i] + 5);
        else if (!strncmp(argv[i], "seek=", 5))  seek = parse_size(argv[i] + 5);
        else if (!strncmp(argv[i], "status=", 7)) continue;
        else if (!strncmp(argv[i], "conv=", 5))  continue;
    }
    if (bs <= 0) bs = 512;

    int ifd = inf ? open(inf, O_RDONLY) : 0;
    if (ifd < 0) { fprintf(stderr, "dd: %s: %s\n", inf, strerror(errno)); return 1; }
    int ofd = outf ? open(outf, O_WRONLY | O_CREAT | O_TRUNC, 0666) : 1;
    if (ofd < 0) { fprintf(stderr, "dd: %s: %s\n", outf, strerror(errno)); return 1; }

    char *buf = (char *)malloc((size_t)bs);
    if (!buf) return 1;
    if (skip > 0) lseek(ifd, skip * bs, SEEK_SET);
    if (seek > 0) lseek(ofd, seek * bs, SEEK_SET);

    long full = 0, partial = 0, bytes = 0;
    for (long n = 0; count < 0 || n < count; n++) {
        ssize_t r = read(ifd, buf, (size_t)bs);
        if (r < 0) { fprintf(stderr, "dd: read error: %s\n", strerror(errno)); break; }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(ofd, buf + off, (size_t)(r - off));
            if (w <= 0) { fprintf(stderr, "dd: write error\n"); goto done; }
            off += w;
        }
        bytes += r;
        if (r == bs) full++; else partial++;
    }
done:
    free(buf);
    if (ifd > 0) close(ifd);
    if (ofd > 1) close(ofd);
    /* dd reports on stderr, which is why `dd ... 2>/dev/null` is idiomatic. */
    fprintf(stderr, "%ld+%ld records in\n%ld+%ld records out\n%ld bytes copied\n",
            full, partial, full, partial, bytes);
    return 0;
}

/* ------------------------------- the second batch of busybox-only names */

extern char **environ;

static int do_printenv(int argc, char **argv) {
    if (argc > 1) {
        int rc = 1;
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1]) continue;
            const char *v = getenv(argv[i]);
            if (v) { printf("%s\n", v); rc = 0; }
        }
        return rc;
    }
    for (char **e = environ; e && *e; e++) printf("%s\n", *e);
    return 0;
}

static int do_nice(int argc, char **argv) {
    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-n") && argi + 1 < argc) { argi++; continue; }
        if (argv[argi][0] == '-' && argv[argi][1]) continue;
        break;
    }
    if (argi >= argc) {
        fprintf(stderr, "nice: no setpriority syscall is exposed, so there "
                        "is no niceness to report\n");
        return 1;
    }
    /* The command still RUNS; only the priority change is impossible, and
     * that is said once rather than silently dropped. */
    fprintf(stderr, "nice: running without a priority change: no setpriority "
                    "syscall is exposed to userland\n");
    execvp(argv[argi], argv + argi);
    fprintf(stderr, "nice: cannot run %s: %s\n", argv[argi], strerror(errno));
    return 127;
}

/* ---- pgrep / pkill: the same /proc walk skill uses ---- */

static int name_of_pid(const char *pid, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int got = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Name:", 5)) continue;
        const char *q = line + 5;
        while (*q == ' ' || *q == 9) q++;
        size_t n = 0;
        while (q[n] && q[n] != 10 && q[n] != 13 && n + 1 < cap) { out[n] = q[n]; n++; }
        out[n] = 0;
        got = 0;
        break;
    }
    fclose(f);
    return got;
}

static int sig_from_name(const char *s) {
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

static int do_pgrep(int argc, char **argv, int do_kill) {
    int sig = SIGTERM;
    const char *pat = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (do_kill) {
                int s = sig_from_name(a);
                if (s > 0) { sig = s; continue; }
            }
            continue;                 /* -f, -l and friends: accepted */
        }
        if (!pat) pat = a;
    }
    if (!pat) {
        fprintf(stderr, "%s: no pattern given\n", do_kill ? "pkill" : "pgrep");
        return 2;
    }
    DIR *d = opendir("/proc");
    if (!d) { fprintf(stderr, "%s: cannot open /proc\n", prog); return 2; }
    int hits = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 1 || pid == (int)getpid()) continue;
        char nm[64];
        if (name_of_pid(e->d_name, nm, sizeof nm) != 0) continue;
        if (!strstr(nm, pat)) continue;      /* pgrep matches a substring */
        hits++;
        if (do_kill) kill(pid, sig);
        else         printf("%d\n", pid);
    }
    closedir(d);
    return hits ? 0 : 1;
}

static int do_tty(void) {
    if (!isatty(0)) { printf("not a tty\n"); return 1; }
    /* ttyname(0): what /proc/self/fd/0 names -- /dev/pts/N for a pty (an
     * SSH session, a terminal window). It said /dev/console for every
     * terminal, from before procfs had those links; the console is still
     * the answer where there is no link. */
    char buf[256];
    long n = readlink("/proc/self/fd/0", buf, sizeof buf - 1);
    if (n > 5 && strncmp(buf, "/dev/", 5) == 0) {
        buf[n] = '\0';
        printf("%s\n", buf);
        return 0;
    }
    printf("/dev/console\n");
    return 0;
}

static int do_stty(int argc, char **argv) {
    struct termios t;
    if (tcgetattr(0, &t) != 0) {
        fprintf(stderr, "stty: standard input is not a terminal\n");
        return 1;
    }
    int changed = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int off = 0;
        if (a[0] == '-' && a[1] && strcmp(a, "-a") && strcmp(a, "-g")) { off = 1; a++; }
        if (!strcmp(a, "a") || !strcmp(a, "g")) continue;
        if (!strcmp(a, "echo"))   { if (off) t.c_lflag &= ~ECHO;   else t.c_lflag |= ECHO;   changed = 1; continue; }
        if (!strcmp(a, "icanon")) { if (off) t.c_lflag &= ~ICANON; else t.c_lflag |= ICANON; changed = 1; continue; }
        if (!strcmp(a, "raw"))    { t.c_lflag &= (tcflag_t)~(ICANON | ECHO); changed = 1; continue; }
        if (!strcmp(a, "cooked") || !strcmp(a, "sane")) {
            t.c_lflag |= (ICANON | ECHO);
            changed = 1;
            continue;
        }
    }
    if (changed) {
        if (tcsetattr(0, 0, &t) != 0) {
            fprintf(stderr, "stty: cannot set terminal attributes\n");
            return 1;
        }
        return 0;
    }
    printf("speed 38400 baud; line = 0;\n");
    printf("%sicanon %secho\n",
           (t.c_lflag & ICANON) ? "" : "-", (t.c_lflag & ECHO) ? "" : "-");
    return 0;
}

/* POSIX cksum is a CRC-32 with a DIFFERENT convention from gzip's: the
 * polynomial runs MSB-first and the LENGTH is fed in after the data, which
 * is why the two never agree on the same file. */
static unsigned long cksum_tab[256];
static int cksum_ready;

static unsigned long posix_cksum(const unsigned char *p, size_t n) {
    if (!cksum_ready) {
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i << 24;
            for (int k = 0; k < 8; k++)
                c = (c & 0x80000000UL) ? ((c << 1) ^ 0x04C11DB7UL) : (c << 1);
            cksum_tab[i] = c & 0xFFFFFFFFUL;
        }
        cksum_ready = 1;
    }
    unsigned long c = 0;
    for (size_t i = 0; i < n; i++)
        c = ((c << 8) ^ cksum_tab[((c >> 24) ^ p[i]) & 0xFF]) & 0xFFFFFFFFUL;
    for (size_t len = n; len; len >>= 8)
        c = ((c << 8) ^ cksum_tab[((c >> 24) ^ (len & 0xFF)) & 0xFF]) & 0xFFFFFFFFUL;
    return (~c) & 0xFFFFFFFFUL;
}

static unsigned char *slurp_stream(FILE *f, size_t *outlen) {
    unsigned char *b = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            b = (unsigned char *)realloc(b, cap);
            if (!b) { *outlen = 0; return NULL; }
        }
        size_t got = fread(b + len, 1, cap - len, f);
        if (!got) break;
        len += got;
    }
    *outlen = len;
    return b;
}

static int do_cksum(int argc, char **argv) {
    int rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        any = 1;
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "cksum: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        size_t len = 0;
        unsigned char *b = slurp_stream(f, &len);
        fclose(f);
        printf("%lu %zu %s\n", posix_cksum(b, len), len, argv[i]);
        free(b);
    }
    if (!any) {
        size_t len = 0;
        unsigned char *b = slurp_stream(stdin, &len);
        printf("%lu %zu\n", posix_cksum(b, len), len);
        free(b);
    }
    return rc;
}

static long size_arg(const char *s) {
    long v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == 'K' || *p == 'k') v *= 1024L;
    else if (*p == 'M' || *p == 'm') v *= 1024L * 1024L;
    else if (*p == 'G' || *p == 'g') v *= 1024L * 1024L * 1024L;
    return v;
}

static int do_truncate(int argc, char **argv) {
    long size = -1;
    int rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) { size = size_arg(argv[++i]); continue; }
        if (!strncmp(argv[i], "-s", 2) && argv[i][2]) { size = size_arg(argv[i] + 2); continue; }
        if (!strncmp(argv[i], "--size=", 7)) { size = size_arg(argv[i] + 7); continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        any = 1;
        if (size < 0) { fprintf(stderr, "truncate: -s is required\n"); return 1; }
        /* Created if absent, as truncate does. */
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) close(fd);
        if (truncate(argv[i], (off_t)size) != 0) {
            fprintf(stderr, "truncate: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    if (!any) { fprintf(stderr, "usage: truncate -s SIZE file...\n"); return 1; }
    return rc;
}

static int copy_one(const char *src, const char *dst, int mode) {
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "install: %s: %s\n", src, strerror(errno)); return 1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); fprintf(stderr, "install: %s: %s\n", dst, strerror(errno)); return 1; }
    char b[8192];
    size_t n;
    while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out);
    fclose(in);
    fclose(out);
    if (mode >= 0) chmod(dst, (mode_t)mode);
    return 0;
}

/* mkfifo [-m MODE] NAME... -- GNU coreutils': each NAME a FIFO, 0666
 * less the umask; with -m, MODE exactly (octal, or symbolic from a=rw).
 * It refused: there were no FIFOs (finding 96). */
static int fifo_mode(const char *s, mode_t *out) {
    if (*s >= '0' && *s <= '7') {
        char *e;
        long v = strtol(s, &e, 8);
        if (*e || v < 0 || v > 07777) return -1;
        *out = (mode_t)v;
        return 0;
    }
    mode_t m = 0666;
    while (*s) {
        mode_t who = 0;
        for (; *s == 'u' || *s == 'g' || *s == 'o' || *s == 'a'; s++)
            who |= *s == 'u' ? 04700 : *s == 'g' ? 02070 : *s == 'o' ? 01007 : 07777;
        if (!who) who = 07777;
        char op = *s++;
        if (op != '+' && op != '-' && op != '=') return -1;
        mode_t bits = 0;
        for (; *s == 'r' || *s == 'w' || *s == 'x'; s++)
            bits |= *s == 'r' ? 0444 : *s == 'w' ? 0222 : 0111;
        bits &= who;
        if (op == '+') m |= bits;
        else if (op == '-') m &= ~bits;
        else m = (m & ~(who & 0777)) | bits;
        if (*s == ',') { s++; continue; }
        if (*s) return -1;
    }
    *out = m;
    return 0;
}

static int do_mkfifo(int argc, char **argv) {
    const char *mstr = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strncmp(a, "--mode=", 7)) { mstr = a + 7; continue; }
        if (a[0] == '-' && a[1] == 'm') {
            mstr = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
            if (!mstr) {
                fprintf(stderr, "mkfifo: option requires an argument -- 'm'\n"
                                "Try 'mkfifo --help' for more information.\n");
                return 1;
            }
            continue;
        }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "mkfifo: invalid option -- '%c'\n"
                            "Try 'mkfifo --help' for more information.\n", a[1]);
            return 1;
        }
        break;
    }
    if (i >= argc) {
        fprintf(stderr, "mkfifo: missing operand\n"
                        "Try 'mkfifo --help' for more information.\n");
        return 1;
    }
    mode_t mode = 0666;
    if (mstr && fifo_mode(mstr, &mode) < 0) {
        fprintf(stderr, "mkfifo: invalid mode '%s'\n", mstr);
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        if (mkfifo(argv[i], mode) != 0) {
            fprintf(stderr, "mkfifo: cannot create fifo '%s': %s\n",
                    argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (mstr) (void)chmod(argv[i], mode);   /* exactly MODE, not less the umask */
    }
    return rc;
}

static int do_install(int argc, char **argv) {
    int mode = 0755, dirs = 0;
    const char *args[64];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            mode = (int)strtol(argv[++i], NULL, 8);
            continue;
        }
        if (!strcmp(argv[i], "-d")) { dirs = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (n < 64) args[n++] = argv[i];
    }
    if (n == 0) { fprintf(stderr, "usage: install [-m mode] src dst | -d dir...\n"); return 1; }
    if (dirs) {
        /* `install -d a/b/c` creates EVERY component, as GNU does -- that
         * is the whole reason a Makefile reaches for it over mkdir. */
        int rc = 0;
        for (int i = 0; i < n; i++) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", args[i]);
            for (char *q = tmp + 1; *q; q++) {
                if (*q != '/') continue;
                *q = 0;
                mkdir(tmp, (mode_t)mode);
                *q = '/';
            }
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) {
                fprintf(stderr, "install: %s: %s\n", args[i], strerror(errno));
                rc = 1;
            }
        }
        return rc;
    }
    if (n < 2) { fprintf(stderr, "install: need a source and a destination\n"); return 1; }
    struct stat st;
    if (stat(args[n-1], &st) == 0 && S_ISDIR(st.st_mode)) {
        int rc = 0;
        for (int i = 0; i < n - 1; i++) {
            const char *b = args[i];
            for (const char *q = args[i]; *q; q++) if (*q == '/') b = q + 1;
            char dst[1024];
            snprintf(dst, sizeof dst, "%s/%s", args[n-1], b);
            if (copy_one(args[i], dst, mode)) rc = 1;
        }
        return rc;
    }
    return copy_one(args[0], args[n-1], mode);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;

    if (!strcmp(base, "rmdir"))   return do_rmdir(argc, argv);
    if (!strcmp(base, "yes"))     return do_yes(argc, argv);
    if (!strcmp(base, "tac"))     return do_tac(argc, argv);
    if (!strcmp(base, "seq"))     return do_seq(argc, argv);
    if (!strcmp(base, "whoami"))  return do_whoami();
    if (!strcmp(base, "which"))   return do_which(argc, argv);
    if (!strcmp(base, "uname"))   return do_uname(argc, argv);
    if (!strcmp(base, "free"))    return do_free(argc, argv);
    if (!strcmp(base, "sync"))    return do_sync();
    if (!strcmp(base, "stat"))    return do_stat(argc, argv);
    if (!strcmp(base, "du"))      return do_du(argc, argv);
    if (!strcmp(base, "xargs"))   return do_xargs(argc, argv);
    if (!strcmp(base, "nohup"))   return do_nohup(argc, argv);
    if (!strcmp(base, "timeout")) return do_timeout(argc, argv);
    if (!strcmp(base, "watch"))   return do_watch(argc, argv);
    if (!strcmp(base, "dd"))      return do_dd(argc, argv);
    if (!strcmp(base, "printenv")) return do_printenv(argc, argv);
    if (!strcmp(base, "nice"))     return do_nice(argc, argv);
    if (!strcmp(base, "pgrep"))    return do_pgrep(argc, argv, 0);
    if (!strcmp(base, "pkill"))    return do_pgrep(argc, argv, 1);
    if (!strcmp(base, "tty"))      return do_tty();
    if (!strcmp(base, "stty"))     return do_stty(argc, argv);
    if (!strcmp(base, "cksum"))    return do_cksum(argc, argv);
    if (!strcmp(base, "truncate")) return do_truncate(argc, argv);
    if (!strcmp(base, "install"))  return do_install(argc, argv);
    if (!strcmp(base, "mkfifo"))   return do_mkfifo(argc, argv);

    fprintf(stderr, "%s: unknown applet\n", base);
    return 1;
}
