/* mkdir -- make directories, the way GNU coreutils' does.
 *
 * A new directory is mkdir(2)'s 0777 less the umask (the kernel takes the
 * umask off). It asked for 0755, so under `umask 000` a directory was 0755
 * where GNU's is 0777 (logs/b14 r2). -m MODE gives the LAST directory of
 * each operand exactly MODE, umask or not: octal, or symbolic as chmod
 * reads it starting from a=rwx -- `-m go-w` is 0755, `-m +t` 1777, `-m =rx`
 * 0555 under umask 022 (measured on GNU coreutils 9.4). -p makes missing
 * parents, each 0777 less the umask with u+wx kept so the walk can go on,
 * and is quiet about a directory that exists. -v names each directory it
 * makes. tobyOS's mkdir knew only -p.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static mode_t g_umask;

/* Every octal digit, up to four of them; -1 if anything else is present. */
static long parse_octal(const char *s) {
    long m = 0;
    int n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '7' || n >= 4) return -1;
        m = (m << 3) | (*s - '0');
    }
    return n ? m : -1;
}

/* chmod's symbolic clauses applied to CUR (programs/user_chmod, the same
 * rules): [ugoa]*([-+=]([rwxXst]*|[ugo]))+, comma-separated; a clause with
 * no who is masked by the umask for + and =. -1 when malformed. */
static int apply_symbolic(const char *spec, mode_t cur, mode_t *out) {
    mode_t mode = cur & 07777;
    const char *p = spec;
    if (!*p) return -1;
    while (*p) {
        mode_t who = 0;
        int explicit_who = 0;
        for (;; p++) {
            if      (*p == 'u') who |= 04700;
            else if (*p == 'g') who |= 02070;
            else if (*p == 'o') who |= 01007;
            else if (*p == 'a') who |= 07777;
            else break;
            explicit_who = 1;
        }
        if (!explicit_who) who = 07777;
        if (*p != '+' && *p != '-' && *p != '=') return -1;
        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p++;
            mode_t perm = 0;
            if (*p == 'u' || *p == 'g' || *p == 'o') {
                mode_t src = (*p == 'u') ? (mode >> 6) & 7
                           : (*p == 'g') ? (mode >> 3) & 7
                           :               mode & 7;
                perm = src | (src << 3) | (src << 6);
                p++;
            } else {
                for (;; p++) {
                    if      (*p == 'r') perm |= 0444;
                    else if (*p == 'w') perm |= 0222;
                    else if (*p == 'x' || *p == 'X') perm |= 0111;  /* a directory */
                    else if (*p == 's') perm |= 06000;
                    else if (*p == 't') perm |= 01000;
                    else break;
                }
            }
            mode_t bits = perm & who;
            if (!explicit_who && op != '-') bits &= ~(g_umask & 0777);
            if      (op == '+') mode |= bits;
            else if (op == '-') mode &= ~bits;
            else {
                mode &= ~(who & (explicit_who ? 07777u : 0777u));
                mode |= bits;
            }
        }
        if (*p == ',') { p++; if (!*p) return -1; continue; }
        if (*p) return -1;
    }
    *out = mode;
    return 0;
}

static int opt_p, opt_v;

static void created(const char *path) {
    if (opt_v) printf("mkdir: created directory '%s'\n", path);
}

/* The missing parents of PATH, each 0777 less the umask with u+wx. */
static void make_parents(const char *path) {
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof tmp) return;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *q = tmp + 1; *q; q++) {
        if (*q != '/') continue;
        *q = '\0';
        struct stat st;
        if (stat(tmp, &st) != 0) {
            if (mkdir(tmp, 0777) == 0) {
                mode_t m = 0777 & ~g_umask;
                if ((m & 0300) != 0300) chmod(tmp, m | 0300);
                created(tmp);
            }
        }
        *q = '/';
        while (q[1] == '/') q++;
    }
}

static void usage(void) {
    fprintf(stderr, "Usage: mkdir [OPTION]... DIRECTORY...\n");
}

int main(int argc, char **argv) {
    const char *mode_arg = 0;
    int i = 1;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || !a[1]) break;
        if (strcmp(a, "--parents") == 0) { opt_p = 1; continue; }
        if (strcmp(a, "--verbose") == 0) { opt_v = 1; continue; }
        if (strncmp(a, "--mode=", 7) == 0) { mode_arg = a + 7; continue; }
        if (strcmp(a, "--mode") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "mkdir: option '--mode' requires an argument\n"); usage(); return 1; }
            mode_arg = argv[++i];
            continue;
        }
        if (a[1] == '-') {
            fprintf(stderr, "mkdir: unrecognized option '%s'\n", a);
            usage();
            return 1;
        }
        for (char *c = a + 1; *c; c++) {
            if (*c == 'p') opt_p = 1;
            else if (*c == 'v') opt_v = 1;
            else if (*c == 'm') {
                if (c[1]) mode_arg = c + 1;
                else if (i + 1 < argc) mode_arg = argv[++i];
                else { fprintf(stderr, "mkdir: option requires an argument -- 'm'\n"); usage(); return 1; }
                break;
            } else {
                fprintf(stderr, "mkdir: invalid option -- '%c'\n", *c);
                usage();
                return 1;
            }
        }
    }
    if (i >= argc) {
        fprintf(stderr, "mkdir: missing operand\n");
        usage();
        return 1;
    }

    g_umask = umask(0);
    umask(g_umask);

    int have_mode = 0;
    mode_t want = 0;
    if (mode_arg) {
        long o = parse_octal(mode_arg);
        if (o >= 0) want = (mode_t)o;
        else if (apply_symbolic(mode_arg, 0777, &want) < 0) {
            fprintf(stderr, "mkdir: invalid mode '%s'\n", mode_arg);
            return 1;
        }
        have_mode = 1;
    }

    int ret = 0;
    for (; i < argc; i++) {
        const char *dir = argv[i];
        if (opt_p) make_parents(dir);
        if (mkdir(dir, have_mode ? (want & 0777) : 0777) != 0) {
            /* -p: "no error shall be reported if the directory named by the
             * operand already exists" (POSIX XCU) */
            if (opt_p && errno == EEXIST) {
                struct stat st;
                if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) continue;
            }
            fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", dir, strerror(errno));
            ret = 1;
            continue;
        }
        /* -m MODE is the mode, whatever the umask took off */
        if (have_mode && (want & ~(0777 & ~g_umask))) chmod(dir, want);
        created(dir);
    }
    return ret;
}
