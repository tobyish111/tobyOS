/* user_moreutils -- the coreutils that were busybox-only, native
 * (2026-09-04, native-parity batch). One binary dispatched on argv[0]:
 *
 *   arch chgrp factor false link logname nproc printf pwd shred sum test [
 *   true unlink uptime users who hostid init passwd
 *
 * Behaviour follows GNU coreutils (procps for uptime, shadow-utils for
 * passwd). printf/test/true/false/pwd exist as tsh builtins; these are the
 * /bin binaries bash also ships, for `env printf`, `xargs test`, #! lines
 * and everything else that execs by name. No floating point (native
 * programs are -mno-sse): printf's %f/%e/%g are done on decimal strings. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <termios.h>
#include <sys/stat.h>
#include <tobyos/abi/abi.h>

/* libtoby's sys/stat.h defines only S_ISDIR/S_ISREG and no inode/device
 * fields; the file-type macros are the POSIX values. */
#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & 0xF000) == 0xA000)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m)  (((m) & 0xF000) == 0x6000)
#define S_ISCHR(m)  (((m) & 0xF000) == 0x2000)
#define S_ISFIFO(m) (((m) & 0xF000) == 0x1000)
#define S_ISSOCK(m) (((m) & 0xF000) == 0xC000)
#endif
#ifndef S_ISUID
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#endif

static const char *g_prog = "moreutils";

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

/* ---------------------------------------------------------- passwd/group */
static int pw_name_of(int uid, char *name, size_t cap) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *fld[8]; int k = 0; fld[k++] = line;
        for (char *p = line; *p && k < 8; p++) if (*p == ':') { *p = '\0'; fld[k++] = p + 1; }
        if (k < 4) continue;
        if (atoi(fld[2]) == uid) { snprintf(name, cap, "%s", fld[0]); fclose(f); return 0; }
    }
    fclose(f);
    return -1;
}

static int gr_lookup(const char *want, int *gid) {
    if (isdigit((unsigned char)want[0])) { *gid = atoi(want); return 0; }
    FILE *f = fopen("/etc/group", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *fld[5]; int k = 0; fld[k++] = line;
        for (char *p = line; *p && k < 5; p++) if (*p == ':') { *p = '\0'; fld[k++] = p + 1; }
        if (k < 3) continue;
        if (!strcmp(fld[0], want)) { *gid = atoi(fld[2]); fclose(f); return 0; }
    }
    fclose(f);
    return -1;
}

static int gr_name_of(int gid, char *name, size_t cap) {
    FILE *f = fopen("/etc/group", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *fld[5]; int k = 0; fld[k++] = line;
        for (char *p = line; *p && k < 5; p++) if (*p == ':') { *p = '\0'; fld[k++] = p + 1; }
        if (k < 3) continue;
        if (atoi(fld[2]) == gid) { snprintf(name, cap, "%s", fld[0]); fclose(f); return 0; }
    }
    fclose(f);
    return -1;
}

/* ---------------------------------------------------------- session */
struct sess { int id, active; char user[64]; long since; };

static void session_read(struct sess *s) {
    char buf[256];
    memset(s, 0, sizeof *s);
    long n = sc2(ABI_SYS_SESSION_INFO, (long)(uintptr_t)buf, sizeof buf);
    if (n <= 0) return;
    buf[n < (long)sizeof buf ? n : (long)sizeof buf - 1] = '\0';
    char *p;
    if ((p = strstr(buf, "id="))) s->id = atoi(p + 3);
    if ((p = strstr(buf, "active="))) s->active = atoi(p + 7);
    if ((p = strstr(buf, "user="))) { sscanf(p + 5, "%63s", s->user); }
    if ((p = strstr(buf, "since="))) s->since = atol(p + 6);
}

static long boot_time(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[256]; long bt = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "btime ", 6)) { bt = atol(line + 6); break; }
    fclose(f);
    return bt;
}

/* ================================================================= arch */
static int cmd_arch(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("arch (tobyOS coreutils) 9.4\n"); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--help")) { printf("Usage: arch [OPTION]...\nPrint machine architecture.\n"); return 0; }
    if (argc > 1) { fprintf(stderr, "arch: extra operand '%s'\n", argv[1]); return 1; }
    puts("x86_64");
    return 0;
}

/* ================================================================ chgrp */
static int chgrp_one(const char *path, int gid, int recursive, int verbose, int changes, int quiet) {
    struct stat st;
    if (stat(path, &st) != 0) { if (!quiet) fprintf(stderr, "chgrp: cannot access '%s': %s\n", path, strerror(errno)); return 1; }
    int rc = 0;
    if ((int)st.st_gid != gid) {
        if (chown(path, (int)st.st_uid, gid) != 0) { if (!quiet) fprintf(stderr, "chgrp: changing group of '%s': %s\n", path, strerror(errno)); rc = 1; }
        else if (verbose || changes) {
            char a[64], b[64];
            if (gr_name_of((int)st.st_gid, a, sizeof a)) snprintf(a, sizeof a, "%d", (int)st.st_gid);
            if (gr_name_of(gid, b, sizeof b)) snprintf(b, sizeof b, "%d", gid);
            printf("changed group of '%s' from %s to %s\n", path, a, b);
        }
    } else if (verbose) {
        char b[64]; if (gr_name_of(gid, b, sizeof b)) snprintf(b, sizeof b, "%d", gid);
        printf("group of '%s' retained as %s\n", path, b);
    }
    if (recursive && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char sub[1024]; snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                rc |= chgrp_one(sub, gid, 1, verbose, changes, quiet);
            }
            closedir(d);
        }
    }
    return rc;
}

static int cmd_chgrp(int argc, char **argv) {
    int recursive = 0, verbose = 0, changes = 0, quiet = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--version")) { printf("chgrp (tobyOS coreutils) 9.4\n"); return 0; }
        if (!strcmp(a, "--help")) { printf("Usage: chgrp [OPTION]... GROUP FILE...\nChange the group of each FILE to GROUP.\n\n  -c, --changes   like verbose but report only when a change is made\n  -f, --silent    suppress most error messages\n  -v, --verbose   output a diagnostic for every file processed\n  -R, --recursive operate on files and directories recursively\n"); return 0; }
        if (!strcmp(a, "--recursive")) { recursive = 1; continue; }
        if (!strcmp(a, "--verbose")) { verbose = 1; continue; }
        if (!strcmp(a, "--changes")) { changes = 1; continue; }
        if (!strcmp(a, "--silent") || !strcmp(a, "--quiet")) { quiet = 1; continue; }
        if (!strncmp(a, "--reference=", 12)) { struct stat st; if (stat(a + 12, &st)) { fprintf(stderr, "chgrp: failed to get attributes of '%s'\n", a + 12); return 1; } static char gbuf[16]; snprintf(gbuf, sizeof gbuf, "%d", (int)st.st_gid); argv[--i] = gbuf; i++; break; }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'R': recursive = 1; break;
            case 'v': verbose = 1; break;
            case 'c': changes = 1; break;
            case 'f': quiet = 1; break;
            case 'h': case 'H': case 'L': case 'P': break;
            default: fprintf(stderr, "chgrp: invalid option -- '%c'\n", *p); return 1;
            }
        }
    }
    if (argc - i < 2) { fprintf(stderr, argc - i < 1 ? "chgrp: missing operand\n" : "chgrp: missing operand after '%s'\n", argv[argc - 1]); fprintf(stderr, "Try 'chgrp --help' for more information.\n"); return 1; }
    int gid;
    if (gr_lookup(argv[i], &gid)) { fprintf(stderr, "chgrp: invalid group: '%s'\n", argv[i]); return 1; }
    int rc = 0;
    for (i++; i < argc; i++) rc |= chgrp_one(argv[i], gid, recursive, verbose, changes, quiet);
    return rc;
}

/* =============================================================== factor */
static int factor_one(const char *s) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) return 0;
    for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) { fprintf(stderr, "factor: '%s' is not a valid positive integer\n", s); return 1; }
    unsigned long long n = strtoull(p, NULL, 10);
    printf("%llu:", n);
    if (n < 2) { putchar('\n'); return 0; }
    while (n % 2 == 0) { printf(" 2"); n /= 2; }
    for (unsigned long long d = 3; d * d <= n; d += 2)
        while (n % d == 0) { printf(" %llu", d); n /= d; }
    if (n > 1) printf(" %llu", n);
    putchar('\n');
    return 0;
}

static int cmd_factor(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("factor (tobyOS coreutils) 9.4\n"); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--help")) { printf("Usage: factor [NUMBER]...\nPrint the prime factors of each specified integer NUMBER.\n"); return 0; }
    int rc = 0;
    if (argc > 1) { for (int i = 1; i < argc; i++) rc |= factor_one(argv[i]); return rc; }
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        char *tok = strtok(line, " \t\n");
        while (tok) { rc |= factor_one(tok); tok = strtok(NULL, " \t\n"); }
    }
    return rc;
}

/* ============================================================ true/false */
static int cmd_true(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) printf("%s (tobyOS coreutils) 9.4\n", g_prog);
    else if (argc == 2 && !strcmp(argv[1], "--help")) printf("Usage: %s [ignored command line arguments]\n  or:  %s OPTION\nExit with a status code indicating %s.\n", g_prog, g_prog, !strcmp(g_prog, "false") ? "failure" : "success");
    return !strcmp(g_prog, "false") ? 1 : 0;
}

/* ========================================================== link/unlink */
static int cmd_link(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("link (tobyOS coreutils) 9.4\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { printf("Usage: link FILE1 FILE2\nCall the link function to create a link named FILE2 to an existing FILE1.\n"); return 0; }
    if (argc < 3) { fprintf(stderr, argc < 2 ? "link: missing operand\n" : "link: missing operand after '%s'\n", argv[1]); fprintf(stderr, "Try 'link --help' for more information.\n"); return 1; }
    if (argc > 3) { fprintf(stderr, "link: extra operand '%s'\n", argv[3]); return 1; }
    if (link(argv[1], argv[2]) != 0) { fprintf(stderr, "link: cannot create link '%s' to '%s': %s\n", argv[2], argv[1], strerror(errno)); return 1; }
    return 0;
}

static int cmd_unlink(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("unlink (tobyOS coreutils) 9.4\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { printf("Usage: unlink FILE\nCall the unlink function to remove the specified FILE.\n"); return 0; }
    if (argc < 2) { fprintf(stderr, "unlink: missing operand\nTry 'unlink --help' for more information.\n"); return 1; }
    if (argc > 2) { fprintf(stderr, "unlink: extra operand '%s'\n", argv[2]); return 1; }
    if (unlink(argv[1]) != 0) { fprintf(stderr, "unlink: cannot unlink '%s': %s\n", argv[1], strerror(errno)); return 1; }
    return 0;
}

/* ============================================================== logname */
static int cmd_logname(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("logname (tobyOS coreutils) 9.4\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { printf("Usage: logname [OPTION]\nPrint the user's login name.\n"); return 0; }
    if (argc > 1) { fprintf(stderr, "logname: extra operand '%s'\n", argv[1]); return 1; }
    char name[64];
    if (pw_name_of((int)getuid(), name, sizeof name)) { fprintf(stderr, "logname: no login name\n"); return 1; }
    puts(name);
    return 0;
}

/* ================================================================ nproc */
static int cmd_nproc(int argc, char **argv) {
    int ignore = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) { printf("nproc (tobyOS coreutils) 9.4\n"); return 0; }
        if (!strcmp(argv[i], "--help")) { printf("Usage: nproc [OPTION]...\nPrint the number of processing units available to the current process.\n\n      --all      print the number of installed processors\n      --ignore=N if possible, exclude N processing units\n"); return 0; }
        if (!strcmp(argv[i], "--all")) continue;
        if (!strncmp(argv[i], "--ignore=", 9)) { ignore = atoi(argv[i] + 9); continue; }
        fprintf(stderr, "nproc: unrecognized option '%s'\n", argv[i]); return 1;
    }
    FILE *f = fopen("/proc/cpuinfo", "r");
    int n = 0; char line[256];
    if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "processor", 9)) n++; fclose(f); }
    if (n < 1) n = 1;
    n -= ignore; if (n < 1) n = 1;
    printf("%d\n", n);
    return 0;
}

/* ================================================================== pwd */
static int cmd_pwd(int argc, char **argv) {
    int logical = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) { printf("pwd (tobyOS coreutils) 9.4\n"); return 0; }
        if (!strcmp(argv[i], "--help")) { printf("Usage: pwd [OPTION]...\nPrint the full filename of the current working directory.\n\n  -L, --logical   use PWD from environment, even if it contains symlinks\n  -P, --physical  avoid all symlinks\n"); return 0; }
        if (!strcmp(argv[i], "-L") || !strcmp(argv[i], "--logical")) logical = 1;
        else if (!strcmp(argv[i], "-P") || !strcmp(argv[i], "--physical")) logical = 0;
        else { fprintf(stderr, "pwd: invalid option -- '%s'\n", argv[i] + 1); return 1; }
    }
    char buf[1024];
    if (logical) {
        const char *pwd = getenv("PWD");
        struct stat a, b;
        /* no inode numbers in libtoby's stat: $PWD is trusted when it is
         * absolute, dot-free, a directory, and names the physical cwd */
        char phys[1024];
        if (pwd && pwd[0] == '/' && !strstr(pwd, "/.") && stat(pwd, &a) == 0 && S_ISDIR(a.st_mode) &&
            stat(".", &b) == 0 && getcwd(phys, sizeof phys) && !strcmp(phys, pwd)) { puts(pwd); return 0; }
    }
    if (!getcwd(buf, sizeof buf)) { fprintf(stderr, "pwd: cannot get current directory: %s\n", strerror(errno)); return 1; }
    puts(buf);
    return 0;
}

/* ================================================================ shred */
static unsigned long long g_rng;
static void rng_fill(unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
        unsigned long long v = g_rng;
        for (int k = 0; k < 8 && i + k < n; k++) { b[i + k] = (unsigned char)v; v >>= 8; }
    }
}

static int shred_one(const char *path, int passes, int zero, int remove, int force, int verbose, long size_override) {
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "shred: %s: failed to open for writing: %s\n", path, strerror(errno)); return 1; }
    if (force) chmod(path, 0600);
    int fd = open(path, O_WRONLY);
    if (fd < 0) { fprintf(stderr, "shred: %s: failed to open for writing: %s\n", path, strerror(errno)); return 1; }
    long size = size_override >= 0 ? size_override : (long)st.st_size;
    static unsigned char buf[65536];
    int total = passes + (zero ? 1 : 0);
    for (int p = 0; p < total; p++) {
        int zpass = zero && p == passes;
        if (verbose) fprintf(stderr, "shred: %s: pass %d/%d (%s)...\n", path, p + 1, total, zpass ? "000000" : "random");
        if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return 1; }
        long left = size;
        while (left > 0) {
            size_t n = left > (long)sizeof buf ? sizeof buf : (size_t)left;
            if (zpass) memset(buf, 0, n); else rng_fill(buf, n);
            ssize_t w = write(fd, buf, n);
            if (w <= 0) { fprintf(stderr, "shred: %s: error writing at offset %ld: %s\n", path, size - left, strerror(errno)); close(fd); return 1; }
            left -= w;
        }
    }
    close(fd);
    if (remove) {
        if (verbose) fprintf(stderr, "shred: %s: removing\n", path);
        if (unlink(path) != 0) { fprintf(stderr, "shred: %s: failed to remove: %s\n", path, strerror(errno)); return 1; }
        if (verbose) fprintf(stderr, "shred: %s: removed\n", path);
    }
    return 0;
}

static int cmd_shred(int argc, char **argv) {
    int passes = 3, zero = 0, remove = 0, force = 0, verbose = 0, i = 1;
    long size = -1;
    g_rng = (unsigned long long)time(NULL) * 6364136223846793005ull ^ (unsigned long long)getpid() * 1442695040888963407ull;
    if (!g_rng) g_rng = 88172645463325252ull;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--version")) { printf("shred (tobyOS coreutils) 9.4\n"); return 0; }
        if (!strcmp(a, "--help")) { printf("Usage: shred [OPTION]... FILE...\nOverwrite the specified FILE(s) repeatedly, in order to make it harder\nfor even very expensive hardware probing to recover the data.\n\n  -f, --force        change permissions to allow writing if necessary\n  -n, --iterations=N overwrite N times instead of the default (3)\n  -s, --size=N       shred this many bytes (suffixes like K, M, G accepted)\n  -u                 deallocate and remove file after overwriting\n  -v, --verbose      show progress\n  -z, --zero         add a final overwrite with zeros to hide shredding\n"); return 0; }
        if (!strcmp(a, "--zero")) { zero = 1; continue; }
        if (!strcmp(a, "--remove") || !strncmp(a, "--remove=", 9)) { remove = 1; continue; }
        if (!strcmp(a, "--force")) { force = 1; continue; }
        if (!strcmp(a, "--verbose")) { verbose = 1; continue; }
        if (!strncmp(a, "--iterations=", 13)) { passes = atoi(a + 13); continue; }
        if (!strncmp(a, "--size=", 7)) { char *e; size = strtol(a + 7, &e, 10); if (*e == 'K') size *= 1024; else if (*e == 'M') size *= 1048576; else if (*e == 'G') size *= 1073741824L; continue; }
        if (!strcmp(a, "--exact") || !strcmp(a, "--random-source")) continue;
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'z': zero = 1; break;
            case 'u': remove = 1; break;
            case 'f': force = 1; break;
            case 'v': verbose = 1; break;
            case 'x': break;
            case 'n': { const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : "3"); passes = atoi(v); p += strlen(p) - 1; break; }
            case 's': { const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : "0"); char *e; size = strtol(v, &e, 10); if (*e == 'K') size *= 1024; else if (*e == 'M') size *= 1048576; else if (*e == 'G') size *= 1073741824L; p += strlen(p) - 1; break; }
            default: fprintf(stderr, "shred: invalid option -- '%c'\n", *p); return 1;
            }
        }
    }
    if (i >= argc) { fprintf(stderr, "shred: missing file operand\nTry 'shred --help' for more information.\n"); return 1; }
    if (passes < 0) passes = 0;
    int rc = 0;
    for (; i < argc; i++) rc |= shred_one(argv[i], passes, zero, remove, force, verbose, size);
    return rc;
}

/* ================================================================== sum */
static int sum_one(const char *path, int sysv, int print_name) {
    FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!f) { fprintf(stderr, "sum: %s: No such file or directory\n", path); return 1; }
    unsigned char buf[8192]; size_t n; unsigned long long total = 0;
    unsigned s = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        total += n;
        if (sysv) for (size_t i = 0; i < n; i++) s += buf[i];
        else for (size_t i = 0; i < n; i++) { s = (s >> 1) + ((s & 1) << 15); s += buf[i]; s &= 0xFFFF; }
    }
    if (f != stdin) fclose(f);
    if (sysv) {
        unsigned r = (s & 0xFFFF) + ((s & 0xFFFFFFFF) >> 16);
        unsigned ck = (r & 0xFFFF) + (r >> 16);
        printf("%u %llu", ck, (total + 511) / 512);
        if (print_name) printf(" %s", path);
    } else {
        printf("%05u %5llu", s, (total + 1023) / 1024);
        if (print_name > 1) printf(" %s", path);
    }
    putchar('\n');
    return 0;
}

static int cmd_sum(int argc, char **argv) {
    int sysv = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "--version")) { printf("sum (tobyOS coreutils) 9.4\n"); return 0; }
        if (!strcmp(argv[i], "--help")) { printf("Usage: sum [OPTION]... [FILE]...\nPrint checksum and block counts for each FILE.\n\n  -r   use BSD sum algorithm (the default), use 1K blocks\n  -s, --sysv   use System V sum algorithm, use 512 bytes blocks\n"); return 0; }
        if (!strcmp(argv[i], "-r")) sysv = 0;
        else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--sysv")) sysv = 1;
        else { fprintf(stderr, "sum: invalid option -- '%c'\n", argv[i][1]); return 1; }
    }
    int nfiles = argc - i;
    if (nfiles == 0) return sum_one("-", sysv, 0);
    int rc = 0;
    for (; i < argc; i++) rc |= sum_one(argv[i], sysv, nfiles);
    return rc;
}

/* ================================================================= test */
static int t_argc; static char **t_argv; static int t_pos;
static int t_err;

static const char *t_peek(void) { return t_pos < t_argc ? t_argv[t_pos] : NULL; }
static const char *t_next(void) { return t_pos < t_argc ? t_argv[t_pos++] : NULL; }

static int t_is_unary(const char *s) { return s && s[0] == '-' && s[1] && !s[2] && strchr("bcdefgGhkLnNOprsStuwxz", s[1]); }
static int t_is_binary(const char *s) {
    if (!s) return 0;
    static const char *ops[] = { "=", "==", "!=", "-eq", "-ne", "-gt", "-ge", "-lt", "-le", "-nt", "-ot", "-ef", "<", ">", 0 };
    for (int i = 0; ops[i]; i++) if (!strcmp(s, ops[i])) return 1;
    return 0;
}

static int t_int(const char *s, long *out) {
    if (!s) return 0;
    const char *p = s; while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    while (isspace((unsigned char)*p)) p++;
    if (*p) return 0;
    *out = strtol(s, NULL, 10);
    return 1;
}

static int t_unary(const char *op, const char *arg) {
    struct stat st;
    switch (op[1]) {
    case 'n': return arg[0] != '\0';
    case 'z': return arg[0] == '\0';
    case 't': { long fd; return t_int(arg, &fd) && isatty((int)fd); }
    case 'e': return stat(arg, &st) == 0;
    case 'f': return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
    case 'd': return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
    case 'b': return stat(arg, &st) == 0 && S_ISBLK(st.st_mode);
    case 'c': return stat(arg, &st) == 0 && S_ISCHR(st.st_mode);
    case 'p': return stat(arg, &st) == 0 && S_ISFIFO(st.st_mode);
    case 'S': return stat(arg, &st) == 0 && S_ISSOCK(st.st_mode);
    case 'h': case 'L': { char b[8]; return readlink(arg, b, sizeof b) >= 0; }
    case 's': return stat(arg, &st) == 0 && st.st_size > 0;
    case 'r': return access(arg, R_OK) == 0;
    case 'w': return access(arg, W_OK) == 0;
    case 'x': return access(arg, X_OK) == 0;
    case 'g': return stat(arg, &st) == 0 && (st.st_mode & S_ISGID);
    case 'u': return stat(arg, &st) == 0 && (st.st_mode & S_ISUID);
    case 'k': return stat(arg, &st) == 0 && (st.st_mode & S_ISVTX);
    case 'O': return stat(arg, &st) == 0 && st.st_uid == geteuid();
    case 'G': return stat(arg, &st) == 0 && st.st_gid == getegid();
    case 'N': return stat(arg, &st) == 0 && st.st_mtime > st.st_atime;
    }
    return 0;
}

static int t_binary(const char *a, const char *op, const char *b) {
    if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
    if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
    if (!strcmp(op, "<")) return strcmp(a, b) < 0;
    if (!strcmp(op, ">")) return strcmp(a, b) > 0;
    if (!strcmp(op, "-nt") || !strcmp(op, "-ot") || !strcmp(op, "-ef")) {
        struct stat sa, sb; int ea = stat(a, &sa) == 0, eb = stat(b, &sb) == 0;
        /* -ef: libtoby's stat carries no inode/device, so "same file" is
         * "same path" (hard links to one inode compare unequal here) */
        if (!strcmp(op, "-ef")) return ea && eb && !strcmp(a, b);
        if (!strcmp(op, "-nt")) return ea && (!eb || sa.st_mtime > sb.st_mtime);
        return eb && (!ea || sa.st_mtime < sb.st_mtime);
    }
    long x, y;
    if (!t_int(a, &x)) { fprintf(stderr, "%s: %s: integer expression expected\n", g_prog, a); t_err = 1; return 0; }
    if (!t_int(b, &y)) { fprintf(stderr, "%s: %s: integer expression expected\n", g_prog, b); t_err = 1; return 0; }
    if (!strcmp(op, "-eq")) return x == y;
    if (!strcmp(op, "-ne")) return x != y;
    if (!strcmp(op, "-gt")) return x > y;
    if (!strcmp(op, "-ge")) return x >= y;
    if (!strcmp(op, "-lt")) return x < y;
    return x <= y;
}

static int t_expr(void);

static int t_primary(void) {
    const char *s = t_peek();
    if (!s) { fprintf(stderr, "%s: argument expected\n", g_prog); t_err = 1; return 0; }
    if (!strcmp(s, "!")) { t_next(); return !t_primary(); }
    if (!strcmp(s, "(")) {
        t_next();
        int v = t_expr();
        const char *c = t_next();
        if (!c || strcmp(c, ")")) { fprintf(stderr, "%s: ')' expected\n", g_prog); t_err = 1; }
        return v;
    }
    /* unary op followed by an argument (and not a binary op after it) */
    if (t_is_unary(s) && t_pos + 1 < t_argc) {
        const char *after = t_pos + 2 < t_argc ? t_argv[t_pos + 2] : NULL;
        if (!(t_is_binary(t_argv[t_pos + 1]) && after)) { t_next(); const char *a = t_next(); return t_unary(s, a); }
    }
    t_next();
    if (t_is_binary(t_peek()) && t_pos + 1 < t_argc) {
        const char *op = t_next(); const char *b = t_next();
        return t_binary(s, op, b);
    }
    return s[0] != '\0';
}

static int t_and(void) {
    int v = t_primary();
    while (t_peek() && !strcmp(t_peek(), "-a")) { t_next(); int w = t_primary(); v = v && w; }
    return v;
}

static int t_expr(void) {
    int v = t_and();
    while (t_peek() && !strcmp(t_peek(), "-o")) { t_next(); int w = t_and(); v = v || w; }
    return v;
}

static int test_eval(int n, char **a) {
    /* the POSIX special cases for 0..4 arguments */
    if (n == 0) return 0;
    if (n == 1) return a[0][0] != '\0';
    if (n == 2) {
        if (!strcmp(a[0], "!")) return !test_eval(1, a + 1);
        if (t_is_unary(a[0])) return t_unary(a[0], a[1]);
    }
    if (n == 3) {
        if (t_is_binary(a[1])) return t_binary(a[0], a[1], a[2]);
        if (!strcmp(a[0], "!")) return !test_eval(2, a + 1);
        if (!strcmp(a[0], "(") && !strcmp(a[2], ")")) return test_eval(1, a + 1);
    }
    if (n == 4) {
        if (!strcmp(a[0], "!")) return !test_eval(3, a + 1);
        if (!strcmp(a[0], "(") && !strcmp(a[3], ")")) return test_eval(2, a + 1);
    }
    t_argc = n; t_argv = a; t_pos = 0;
    int v = t_expr();
    if (t_pos < t_argc && !t_err) { fprintf(stderr, "%s: extra argument '%s'\n", g_prog, t_argv[t_pos]); t_err = 1; }
    return v;
}

static int cmd_test(int argc, char **argv) {
    int n = argc - 1; char **a = argv + 1;
    if (!strcmp(g_prog, "[")) {
        if (n == 1 && !strcmp(a[0], "--help")) { printf("Usage: test EXPRESSION\n  or:  test\n  or:  [ EXPRESSION ]\n  or:  [ ]\n  or:  [ OPTION\nExit with the status determined by EXPRESSION.\n"); return 0; }
        if (n == 1 && !strcmp(a[0], "--version")) { printf("test (tobyOS coreutils) 9.4\n"); return 0; }
        if (n < 1 || strcmp(a[n - 1], "]")) { fprintf(stderr, "[: missing ']'\n"); return 2; }
        n--;
    }
    t_err = 0;
    int v = test_eval(n, a);
    if (t_err) return 2;
    return v ? 0 : 1;
}

/* =============================================================== printf */
/* Decimal-string arithmetic for %f/%e/%g without floating point. */
struct dec { int neg; char digits[400]; int ndig; int exp; };   /* value = 0.digits * 10^exp */

static int dec_parse(const char *s, struct dec *d) {
    memset(d, 0, sizeof *d);
    const char *p = s; while (isspace((unsigned char)*p)) p++;
    if (*p == '-') { d->neg = 1; p++; } else if (*p == '+') p++;
    if (!strncasecmp(p, "inf", 3) || !strncasecmp(p, "nan", 3)) return -2;
    int seen = 0, intdigits = 0, started = 0;
    for (; isdigit((unsigned char)*p); p++) { seen = 1; if (*p != '0' || started) { if (d->ndig < 380) d->digits[d->ndig++] = *p; started = 1; intdigits++; } }
    if (*p == '.') { p++; for (; isdigit((unsigned char)*p); p++) { seen = 1; if (*p != '0' || started) { if (d->ndig < 380) d->digits[d->ndig++] = *p; started = 1; } else if (!started) intdigits--; } }
    if (!seen) return -1;
    d->exp = started ? intdigits : 0;
    if (*p == 'e' || *p == 'E') { char *e; long ex = strtol(p + 1, &e, 10); if (e != p + 1) { d->exp += (int)ex; p = e; } }
    while (isspace((unsigned char)*p)) p++;
    if (!d->ndig) d->exp = 0;
    return *p ? 1 : 0;   /* 1 = trailing garbage */
}

/* Round the digit string to keep `keep` digits (may carry). */
static void dec_round(struct dec *d, int keep) {
    if (keep < 0) keep = 0;
    if (d->ndig <= keep) return;
    int up = d->digits[keep] >= '5';
    d->ndig = keep;
    if (up) {
        int i = keep - 1;
        while (i >= 0) { if (d->digits[i] == '9') { d->digits[i] = '0'; i--; } else { d->digits[i]++; break; } }
        if (i < 0) { memmove(d->digits + 1, d->digits, (size_t)d->ndig); d->digits[0] = '1'; d->ndig++; d->exp++; }
    }
    while (d->ndig && d->digits[d->ndig - 1] == '0') d->ndig--;
}

static void dec_to_f(struct dec *d, int prec, int alt, char *out, size_t cap) {
    struct dec t = *d;
    dec_round(&t, t.exp + prec);
    size_t o = 0;
    if (t.neg) out[o++] = '-';
    if (t.exp <= 0) out[o++] = '0';
    else for (int i = 0; i < t.exp && o + 1 < cap; i++) out[o++] = i < t.ndig ? t.digits[i] : '0';
    if (prec > 0 || alt) out[o++] = '.';
    for (int i = 0; i < prec && o + 1 < cap; i++) { int idx = t.exp + i; out[o++] = (idx >= 0 && idx < t.ndig) ? t.digits[idx] : '0'; }
    out[o] = '\0';
}

static void dec_to_e(struct dec *d, int prec, int alt, int upper, char *out, size_t cap) {
    struct dec t = *d;
    dec_round(&t, prec + 1);
    int e = t.ndig ? t.exp - 1 : 0;
    size_t o = 0;
    if (t.neg) out[o++] = '-';
    out[o++] = t.ndig ? t.digits[0] : '0';
    if (prec > 0 || alt) out[o++] = '.';
    for (int i = 1; i <= prec && o + 1 < cap; i++) out[o++] = i < t.ndig ? t.digits[i] : '0';
    snprintf(out + o, cap - o, "%c%c%02d", upper ? 'E' : 'e', e < 0 ? '-' : '+', e < 0 ? -e : e);
}

static void strip_zeros(char *s) {
    char *dot = strchr(s, '.'); if (!dot) return;
    char *e = dot; while (*e && *e != 'e' && *e != 'E') e++;
    char *q = e; while (q > dot + 1 && q[-1] == '0') q--;
    if (q == dot + 1) q = dot;
    memmove(q, e, strlen(e) + 1);
}

static void dec_to_g(struct dec *d, int prec, int alt, int upper, char *out, size_t cap) {
    if (prec == 0) prec = 1;
    struct dec t = *d; dec_round(&t, prec);
    int e = t.ndig ? t.exp - 1 : 0;
    if (e < -4 || e >= prec) dec_to_e(&t, prec - 1, alt, upper, out, cap);
    else dec_to_f(&t, prec - 1 - e, alt, out, cap);
    if (!alt) strip_zeros(out);
}

static int pf_rc;

static long pf_long(const char *s, int *ok) {
    *ok = 1;
    if (!s || !*s) return 0;
    if (s[0] == '\'' || s[0] == '"') return (unsigned char)s[1];
    char *e; errno = 0;
    long v = strtol(s, &e, 0);
    while (isspace((unsigned char)*e)) e++;
    if (e == s) { fprintf(stderr, "printf: '%s': expected a numeric value\n", s); pf_rc = 1; *ok = 0; return 0; }
    if (*e) { fprintf(stderr, "printf: '%s': value not completely converted\n", s); pf_rc = 1; }
    return v;
}

static unsigned long pf_ulong(const char *s, int *ok) {
    *ok = 1;
    if (!s || !*s) return 0;
    if (s[0] == '\'' || s[0] == '"') return (unsigned char)s[1];
    char *e; errno = 0;
    unsigned long v = strtoul(s, &e, 0);
    while (isspace((unsigned char)*e)) e++;
    if (e == s) { fprintf(stderr, "printf: '%s': expected a numeric value\n", s); pf_rc = 1; *ok = 0; return 0; }
    if (*e) { fprintf(stderr, "printf: '%s': value not completely converted\n", s); pf_rc = 1; }
    return v;
}

/* Expand backslash escapes; returns bytes written; sets *stop on \c. */
static size_t pf_unescape(const char *s, int in_b, char *out, size_t cap, int *stop) {
    size_t o = 0;
    for (const char *p = s; *p && o + 4 < cap; p++) {
        if (*p != '\\') { out[o++] = *p; continue; }
        p++;
        switch (*p) {
        case '\0': out[o++] = '\\'; p--; break;
        case '\\': out[o++] = '\\'; break;
        case 'a': out[o++] = 7; break;
        case 'b': out[o++] = 8; break;
        case 'c': *stop = 1; out[o] = '\0'; return o;
        case 'e': case 'E': out[o++] = 27; break;
        case 'f': out[o++] = 12; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'v': out[o++] = 11; break;
        case '"': out[o++] = '"'; break;
        case '\'': out[o++] = '\''; break;
        case 'x': { int v = 0, k = 0; while (k < 2 && isxdigit((unsigned char)p[1])) { p++; v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); k++; } if (!k) { out[o++] = '\\'; out[o++] = 'x'; } else out[o++] = (char)v; break; }
        case 'u': case 'U': { int nd = *p == 'u' ? 4 : 8; unsigned v = 0; int k = 0; while (k < nd && isxdigit((unsigned char)p[1])) { p++; v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); k++; }
            if (v < 0x80) out[o++] = (char)v; else if (v < 0x800) { out[o++] = (char)(0xC0 | v >> 6); out[o++] = (char)(0x80 | (v & 0x3F)); }
            else if (v < 0x10000) { out[o++] = (char)(0xE0 | v >> 12); out[o++] = (char)(0x80 | ((v >> 6) & 0x3F)); out[o++] = (char)(0x80 | (v & 0x3F)); }
            else { out[o++] = (char)(0xF0 | v >> 18); out[o++] = (char)(0x80 | ((v >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((v >> 6) & 0x3F)); out[o++] = (char)(0x80 | (v & 0x3F)); } break; }
        default:
            if (*p >= '0' && *p <= '7') {
                int v = 0, k = 0, maxd = 3;
                if (in_b && *p == '0') { p++; maxd = 3; if (!(*p >= '0' && *p <= '7')) { out[o++] = 0; p--; break; } }
                while (k < maxd && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; }
                p--; out[o++] = (char)v;
            } else { out[o++] = '\\'; out[o++] = *p; }
        }
    }
    out[o] = '\0';
    return o;
}

static void pf_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }

static int cmd_printf(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "printf: missing operand\nTry 'printf --help' for more information.\n"); return 1; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { printf("Usage: printf FORMAT [ARGUMENT]...\n  or:  printf OPTION\nPrint ARGUMENT(s) according to FORMAT, or execute according to OPTION:\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("printf (tobyOS coreutils) 9.4\n"); return 0; }
    const char *fmt = argv[1];
    if (!strcmp(fmt, "--") && argc > 2) { fmt = argv[2]; argv++; argc--; }
    char **args = argv + 2; int nargs = argc - 2, ai = 0;
    pf_rc = 0;
    int stop = 0;
    do {
        int consumed = 0;
        for (const char *p = fmt; *p && !stop; p++) {
            if (*p == '\\') {
                char tmp[8]; char esc[3] = { '\\', p[1], 0 };
                const char *q = p;
                /* re-use pf_unescape on the shortest prefix: copy up to 10 chars */
                char seg[12]; size_t sl = 0; seg[sl++] = *q++;
                while (*q && sl < 11 && (sl < 2 || (isxdigit((unsigned char)*q) && (p[1] == 'x' || p[1] == 'u' || p[1] == 'U')) || (sl < 4 && *q >= '0' && *q <= '7' && p[1] >= '0' && p[1] <= '7'))) seg[sl++] = *q++;
                seg[sl] = '\0';
                (void)esc;
                size_t n = pf_unescape(seg, 0, tmp, sizeof tmp, &stop);
                pf_write(tmp, n);
                p = q - 1;
                continue;
            }
            if (*p != '%') { putchar(*p); continue; }
            p++;
            if (*p == '%') { putchar('%'); continue; }
            char flags[8] = ""; int nf = 0;
            while (*p && strchr("-+ #0'", *p)) { if (nf < 7 && *p != '\'') flags[nf++] = *p; p++; }
            flags[nf] = '\0';
            int width = -1, prec = -1;
            if (*p == '*') { int ok; width = (int)pf_long(ai < nargs ? args[ai] : "0", &ok); if (ai < nargs) { ai++; consumed = 1; } p++; }
            else if (isdigit((unsigned char)*p)) width = (int)strtol(p, (char **)&p, 10);
            if (*p == '.') { p++; if (*p == '*') { int ok; prec = (int)pf_long(ai < nargs ? args[ai] : "0", &ok); if (ai < nargs) { ai++; consumed = 1; } p++; } else prec = isdigit((unsigned char)*p) ? (int)strtol(p, (char **)&p, 10) : 0; }
            while (*p && strchr("hlLjzt", *p)) p++;   /* not q: %q is a conversion */
            char conv = *p;
            if (!conv) { fprintf(stderr, "printf: %%: missing conversion\n"); return 1; }
            const char *arg = ai < nargs ? args[ai] : NULL;
            char spec[32], outb[1024];
            switch (conv) {
            case 'd': case 'i': { int ok; long v = arg ? pf_long(arg, &ok) : 0; if (arg) { ai++; consumed = 1; }
                snprintf(spec, sizeof spec, "%%%s%s%s%ld", flags, width >= 0 ? "*" : "", prec >= 0 ? ".*" : "", 0L); /* placeholder */
                /* build the spec manually */
                { char sp[32]; int k = 0; sp[k++] = '%'; for (const char *f = flags; *f; f++) sp[k++] = *f; if (width >= 0) k += sprintf(sp + k, "%d", width); if (prec >= 0) k += sprintf(sp + k, ".%d", prec); sp[k++] = 'l'; sp[k++] = 'd'; sp[k] = '\0'; snprintf(outb, sizeof outb, sp, v); }
                fputs(outb, stdout); break; }
            case 'u': case 'o': case 'x': case 'X': { int ok; unsigned long v = arg ? pf_ulong(arg, &ok) : 0; if (arg) { ai++; consumed = 1; }
                char sp[32]; int k = 0; sp[k++] = '%'; for (const char *f = flags; *f; f++) sp[k++] = *f; if (width >= 0) k += sprintf(sp + k, "%d", width); if (prec >= 0) k += sprintf(sp + k, ".%d", prec); sp[k++] = 'l'; sp[k++] = conv; sp[k] = '\0';
                snprintf(outb, sizeof outb, sp, v); fputs(outb, stdout); break; }
            case 'c': { char c = arg && arg[0] ? arg[0] : 0; if (arg) { ai++; consumed = 1; }
                char sp[32]; int k = 0; sp[k++] = '%'; for (const char *f = flags; *f; f++) sp[k++] = *f; if (width >= 0) k += sprintf(sp + k, "%d", width); sp[k++] = 'c'; sp[k] = '\0';
                if (c) { snprintf(outb, sizeof outb, sp, c); fputs(outb, stdout); } else if (width > 0) { for (int w = 0; w < width; w++) putchar(' '); } break; }
            case 's': case 'b': case 'q': {
                const char *s = arg ? arg : ""; if (arg) { ai++; consumed = 1; }
                char buf[4096];
                if (conv == 'b') { int st2 = 0; pf_unescape(s, 1, buf, sizeof buf, &st2); s = buf; if (st2) stop = 1; }
                else if (conv == 'q') {
                    int safe = s[0] != '\0';
                    for (const char *c = s; *c; c++) if (!(isalnum((unsigned char)*c) || strchr("_-./:=@%^+,", *c))) { safe = 0; break; }
                    if (safe) snprintf(buf, sizeof buf, "%s", s);
                    else { size_t o = 0; buf[o++] = '\''; for (const char *c = s; *c && o + 5 < sizeof buf; c++) { if (*c == '\'') { memcpy(buf + o, "'\\''", 4); o += 4; } else buf[o++] = *c; } buf[o++] = '\''; buf[o] = '\0'; }
                    s = buf;
                }
                char sp[32]; int k = 0; sp[k++] = '%'; for (const char *f = flags; *f; f++) if (*f == '-') sp[k++] = *f; if (width >= 0) k += sprintf(sp + k, "%d", width); if (prec >= 0 && conv != 'q') k += sprintf(sp + k, ".%d", prec); sp[k++] = 's'; sp[k] = '\0';
                if (width < 0 && prec < 0) fputs(s, stdout); else { snprintf(outb, sizeof outb, sp, s); fputs(outb, stdout); }
                break; }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                struct dec d; int r = arg ? dec_parse(arg, &d) : (memset(&d, 0, sizeof d), 0);
                if (arg) { ai++; consumed = 1; }
                if (r == -1) { fprintf(stderr, "printf: '%s': expected a numeric value\n", arg); pf_rc = 1; }
                else if (r == 1) { fprintf(stderr, "printf: '%s': value not completely converted\n", arg); pf_rc = 1; }
                int pr = prec >= 0 ? prec : 6, alt = strchr(flags, '#') != NULL;
                char num[512];
                if (r == -2) snprintf(num, sizeof num, "%s%s", d.neg ? "-" : "", strncasecmp(arg + (arg[0] == '-' || arg[0] == '+'), "inf", 3) == 0 ? "inf" : "nan");
                else if (conv == 'f' || conv == 'F' || conv == 'a' || conv == 'A') dec_to_f(&d, pr, alt, num, sizeof num);
                else if (conv == 'e' || conv == 'E') dec_to_e(&d, pr, alt, conv == 'E', num, sizeof num);
                else dec_to_g(&d, pr, alt, conv == 'G', num, sizeof num);
                if (strchr(flags, '+') && num[0] != '-') { memmove(num + 1, num, strlen(num) + 1); num[0] = '+'; }
                else if (strchr(flags, ' ') && num[0] != '-') { memmove(num + 1, num, strlen(num) + 1); num[0] = ' '; }
                size_t l = strlen(num);
                if (width > (int)l) {
                    int pad = width - (int)l;
                    if (strchr(flags, '-')) { fputs(num, stdout); for (int w = 0; w < pad; w++) putchar(' '); }
                    else if (strchr(flags, '0') && r != -2) { size_t sgn = (num[0] == '-' || num[0] == '+' || num[0] == ' ') ? 1 : 0; fwrite(num, 1, sgn, stdout); for (int w = 0; w < pad; w++) putchar('0'); fputs(num + sgn, stdout); }
                    else { for (int w = 0; w < pad; w++) putchar(' '); fputs(num, stdout); }
                } else fputs(num, stdout);
                break; }
            default:
                fprintf(stderr, "printf: %%%c: invalid conversion specification\n", conv);
                return 1;
            }
        }
        if (!consumed) break;
    } while (ai < nargs && !stop);
    fflush(stdout);
    return pf_rc;
}

/* =============================================================== uptime */
static void fmt_uptime(long secs, int pretty, char *out, size_t cap) {
    long days = secs / 86400, hours = (secs / 3600) % 24, mins = (secs / 60) % 60;
    size_t o = 0;
    if (pretty) {
        o += (size_t)snprintf(out + o, cap - o, "up");
        if (days) o += (size_t)snprintf(out + o, cap - o, " %ld day%s,", days, days == 1 ? "" : "s");
        if (hours) o += (size_t)snprintf(out + o, cap - o, " %ld hour%s,", hours, hours == 1 ? "" : "s");
        o += (size_t)snprintf(out + o, cap - o, " %ld minute%s", mins, mins == 1 ? "" : "s");
        return;
    }
    if (days) o += (size_t)snprintf(out + o, cap - o, "%ld day%s, ", days, days == 1 ? "" : "s");
    if (hours) snprintf(out + o, cap - o, "%2ld:%02ld", hours, mins);
    else snprintf(out + o, cap - o, "%ld min", mins);
}

static int cmd_uptime(int argc, char **argv) {
    int pretty = 0, since = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) { printf("uptime from tobyOS procps 4.0\n"); return 0; }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { printf("\nUsage:\n uptime [options]\n\nOptions:\n -p, --pretty   show uptime in pretty format\n -h, --help     display this help and exit\n -s, --since    system up since\n -V, --version  output version information and exit\n"); return 0; }
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--pretty")) pretty = 1;
        else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--since")) since = 1;
        else { fprintf(stderr, "uptime: invalid option -- '%s'\n", argv[i] + 1); return 1; }
    }
    long up = 0; char la[3][16] = { "0.00", "0.00", "0.00" };
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { char line[64]; if (fgets(line, sizeof line, f)) up = atol(line); fclose(f); }
    f = fopen("/proc/loadavg", "r");
    if (f) { char line[128]; if (fgets(line, sizeof line, f)) sscanf(line, "%15s %15s %15s", la[0], la[1], la[2]); fclose(f); }
    time_t now = time(NULL);
    if (since) { time_t b = now - up; struct tm *tm = localtime(&b); char buf[64]; strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", tm); puts(buf); return 0; }
    char ups[64]; fmt_uptime(up, pretty, ups, sizeof ups);
    if (pretty) { puts(ups); return 0; }
    struct sess s; session_read(&s);
    int users = s.active ? 1 : 0;
    struct tm *tm = localtime(&now);
    printf(" %02d:%02d:%02d up %s,  %d user%s,  load average: %s, %s, %s\n",
           tm->tm_hour, tm->tm_min, tm->tm_sec, ups, users, users == 1 ? "" : "s", la[0], la[1], la[2]);
    return 0;
}

/* ============================================================ users/who */
static int cmd_users(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("users (tobyOS coreutils) 9.4\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { printf("Usage: users [OPTION]... [FILE]\nOutput who is currently logged in according to FILE.\n"); return 0; }
    struct sess s; session_read(&s);
    if (s.active && s.user[0]) puts(s.user); else putchar('\n');
    return 0;
}

static void fmt_when(long t, char *out, size_t cap) {
    time_t tt = (time_t)t; struct tm *tm = localtime(&tt);
    if (tm) strftime(out, cap, "%Y-%m-%d %H:%M", tm); else snprintf(out, cap, "?");
}

static int cmd_who(int argc, char **argv) {
    int header = 0, boot = 0, count = 0, me = 0, all = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--version")) { printf("who (tobyOS coreutils) 9.4\n"); return 0; }
        if (!strcmp(a, "--help")) { printf("Usage: who [OPTION]... [ FILE | ARG1 ARG2 ]\nPrint information about users who are currently logged in.\n\n  -a, --all         same as -b -d --login -p -r -t -T -u\n  -b, --boot        time of last system boot\n  -H, --heading     print line of column headings\n  -m                only hostname and user associated with stdin\n  -q, --count       all login names and number of users logged on\n"); return 0; }
        if (!strcmp(a, "am") || !strcmp(a, "i") || !strcmp(a, "I") || !strcmp(a, "mom") || !strcmp(a, "likes")) { me = 1; continue; }
        if (!strcmp(a, "--heading")) { header = 1; continue; }
        if (!strcmp(a, "--boot")) { boot = 1; continue; }
        if (!strcmp(a, "--count")) { count = 1; continue; }
        if (!strcmp(a, "--all")) { all = 1; boot = 1; continue; }
        if (a[0] == '-' && a[1] != '-') { for (const char *p = a + 1; *p; p++) { switch (*p) { case 'H': header = 1; break; case 'b': boot = 1; break; case 'q': count = 1; break; case 'm': me = 1; break; case 'a': all = 1; boot = 1; break; case 'u': case 's': case 'T': case 'w': case 'd': case 'l': case 'p': case 'r': case 't': break; default: fprintf(stderr, "who: invalid option -- '%c'\n", *p); return 1; } } continue; }
        /* a utmp FILE operand: none exists here; ignore */
    }
    struct sess s; session_read(&s);
    char when[32];
    if (count) { if (s.active && s.user[0]) printf("%s\n", s.user); printf("# users=%d\n", s.active ? 1 : 0); return 0; }
    if (header) printf("%-8s %-12s %s\n", "NAME", "LINE", "TIME");
    if (boot) { fmt_when(boot_time(), when, sizeof when); printf("%-8s %-12s %s\n", "", "system boot", when); }
    if (me) {
        char name[64];
        if (pw_name_of((int)getuid(), name, sizeof name) == 0) { fmt_when(s.since ? s.since : boot_time(), when, sizeof when); printf("%-8s %-12s %s\n", name, "console", when); }
        return 0;
    }
    if (s.active && s.user[0]) { fmt_when(s.since ? s.since : boot_time(), when, sizeof when); printf("%-8s %-12s %s\n", s.user, "console", when); }
    (void)all;
    return 0;
}

/* =============================================================== hostid */
static int cmd_hostid(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { printf("hostid (tobyOS coreutils) 9.4\n"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { printf("Usage: hostid [OPTION]\nPrint the numeric identifier (in hexadecimal) for the current host.\n"); return 0; }
    if (argc > 1) { fprintf(stderr, "hostid: extra operand '%s'\n", argv[1]); return 1; }
    unsigned long id = 0;
    FILE *f = fopen("/etc/hostid", "rb");
    if (f) { unsigned char b[4]; if (fread(b, 1, 4, f) == 4) id = (unsigned long)b[0] | (unsigned long)b[1] << 8 | (unsigned long)b[2] << 16 | (unsigned long)b[3] << 24; fclose(f); }
    if (!id) {
        /* glibc without /etc/hostid: the primary IPv4 address with its halves
         * swapped -- in_addr_t is the address bytes read as a host word. */
        struct abi_system_metrics m;
        if (sc2(ABI_SYS_SYSTEM_METRICS, (long)(uintptr_t)&m, sizeof m) >= 0 && m.net_ip_be) {
            unsigned long a = m.net_ip_be;
            id = ((a << 16) | (a >> 16)) & 0xFFFFFFFFul;
        }
    }
    char hx[9];
    for (int i = 7; i >= 0; i--) { hx[i] = "0123456789abcdef"[id & 0xF]; id >>= 4; }
    hx[8] = '\0';
    printf("%s\n", hx);
    return 0;
}

/* ================================================================= init */
static int cmd_init(int argc, char **argv) {
    (void)argc; (void)argv;
    if (getpid() != 1) { fprintf(stderr, "init: must be run as PID 1\n"); return 1; }
    /* tobyOS boots from an in-kernel init; a userspace PID 1 never happens.
     * If it ever did, there is nothing sensible to run: say so and idle. */
    fprintf(stderr, "init: tobyOS's init is in-kernel; nothing to do\n");
    for (;;) sleep(3600);
}

/* =============================================================== passwd */
static int read_pw(const char *prompt, char *out, size_t cap) {
    struct termios saved, raw; int tty = isatty(0);
    if (tty && tcgetattr(0, &saved) == 0) { raw = saved; raw.c_lflag &= (unsigned)~ECHO; tcsetattr(0, TCSANOW, &raw); }
    fputs(prompt, stderr);
    int ok = fgets(out, (int)cap, stdin) != NULL;
    if (tty) { tcsetattr(0, TCSANOW, &saved); }
    fputc('\n', stderr);
    if (!ok) return 0;
    out[strcspn(out, "\r\n")] = '\0';
    return 1;
}

static int cmd_passwd(int argc, char **argv) {
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { printf("Usage: passwd [options] [LOGIN]\n\nOptions:\n  -h, --help   display this help message and exit\n"); return 0; }
        if (argv[i][0] == '-') { fprintf(stderr, "passwd: unrecognized option '%s'\n", argv[i]); return 1; }
        target = argv[i];
    }
    int uid = (int)getuid();
    char me[64];
    if (pw_name_of(uid, me, sizeof me)) snprintf(me, sizeof me, "uid%d", uid);
    if (!target) target = me;
    if (uid != 0 && strcmp(target, me)) { fprintf(stderr, "passwd: You may not view or modify password information for %s.\n", target); return 1; }
    fprintf(stderr, "Changing password for %s.\n", target);
    char old[128] = "", n1[128], n2[128];
    if (uid != 0) { if (!read_pw("Current password: ", old, sizeof old)) return 1; }
    if (!read_pw("New password: ", n1, sizeof n1)) return 1;
    if (!read_pw("Retype new password: ", n2, sizeof n2)) return 1;
    if (strcmp(n1, n2)) { fprintf(stderr, "Sorry, passwords do not match.\npasswd: Authentication token manipulation error\npasswd: password unchanged\n"); return 1; }
    if (!n1[0]) { fprintf(stderr, "No password has been supplied.\npasswd: password unchanged\n"); return 1; }
    long rc = sc3(ABI_SYS_SETPASSWD, (long)(uintptr_t)target, (long)(uintptr_t)n1, (long)(uintptr_t)old);
    memset(old, 0, sizeof old); memset(n1, 0, sizeof n1); memset(n2, 0, sizeof n2);
    if (rc == -ABI_EACCES) { fprintf(stderr, "passwd: Authentication failure\npasswd: password unchanged\n"); return 1; }
    if (rc == -ABI_ENOENT) { fprintf(stderr, "passwd: user '%s' does not exist\n", target); return 1; }
    if (rc == -ABI_EPERM) { fprintf(stderr, "passwd: You may not view or modify password information for %s.\n", target); return 1; }
    if (rc != 0) { fprintf(stderr, "passwd: Authentication token manipulation error\npasswd: password unchanged\n"); return 1; }
    fprintf(stderr, "passwd: password updated successfully\n");
    return 0;
}

/* ============================================================== dispatch */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "arch",    cmd_arch },
    { "chgrp",   cmd_chgrp },
    { "factor",  cmd_factor },
    { "false",   cmd_true },
    { "link",    cmd_link },
    { "logname", cmd_logname },
    { "nproc",   cmd_nproc },
    { "printf",  cmd_printf },
    { "pwd",     cmd_pwd },
    { "shred",   cmd_shred },
    { "sum",     cmd_sum },
    { "test",    cmd_test },
    { "[",       cmd_test },
    { "true",    cmd_true },
    { "unlink",  cmd_unlink },
    { "uptime",  cmd_uptime },
    { "users",   cmd_users },
    { "who",     cmd_who },
    { "hostid",  cmd_hostid },
    { "init",    cmd_init },
    { "passwd",  cmd_passwd },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "moreutils";
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
    fprintf(stderr, "moreutils: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
