/* chmod -- change file mode bits, the way GNU coreutils' does.
 *
 * Until 2026-09-01 this accepted three octal digits and nothing else, so
 * `chmod +x script` -- the single most common invocation there is --
 * printed "invalid mode" and every script fixture in the distro gate
 * stayed 0644. Symbolic clauses are POSIX:
 *
 *     [ugoa]*([-+=]([rwxXst]*|[ugo]))+       comma-separated
 *
 * A clause with no who is masked by the umask for + and =, as the standard
 * says; X is execute-if-directory-or-already-executable; s and t are the
 * set-id and sticky bits. -R recurses, -v reports every file, -c only the
 * changed ones, -f suppresses errors, --reference=RFILE copies a mode.
 * Messages and exit status match GNU's, because scripts parse them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *prog = "chmod";
static int opt_R, opt_v, opt_c, opt_f;
static int status;
static mode_t g_umask;

/* "rwxr-xr-x", with s/S/t/T where the set-id and sticky bits land. */
static void rwx_string(mode_t m, char *out) {
    const char *bits = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[i] = (m & (0400u >> i)) ? bits[i] : '-';
    if (m & 04000) out[2] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[5] = (m & 0010) ? 's' : 'S';
    if (m & 01000) out[8] = (m & 0001) ? 't' : 'T';
    out[9] = 0;
}

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

/* Applies a symbolic spec to `cur`. Returns -1 if the spec is malformed. */
static int apply_symbolic(const char *spec, mode_t cur, int is_dir, mode_t *out) {
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
                /* copy another class's rwx into every class named by who */
                mode_t src = (*p == 'u') ? (mode >> 6) & 7
                           : (*p == 'g') ? (mode >> 3) & 7
                           :               mode & 7;
                perm = src | (src << 3) | (src << 6);
                p++;
            } else {
                for (;; p++) {
                    if      (*p == 'r') perm |= 0444;
                    else if (*p == 'w') perm |= 0222;
                    else if (*p == 'x') perm |= 0111;
                    else if (*p == 'X') { if (is_dir || (mode & 0111)) perm |= 0111; }
                    else if (*p == 's') perm |= 06000;
                    else if (*p == 't') perm |= 01000;
                    else break;
                }
            }
            mode_t bits = perm & who;
            /* No who: the umask says which permission bits may be granted. */
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

struct modespec { int numeric; long num; const char *sym; int has_ref; mode_t ref; };

static void report(const char *path, mode_t from, mode_t to, int changed) {
    char a[10], b[10];
    rwx_string(from, a);
    rwx_string(to, b);
    if (changed)
        printf("mode of '%s' changed from %04o (%s) to %04o (%s)\n",
               path, (unsigned)from, a, (unsigned)to, b);
    else
        printf("mode of '%s' retained as %04o (%s)\n", path, (unsigned)from, a);
}

static void do_path(const char *path, const struct modespec *ms) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (!opt_f)
            fprintf(stderr, "%s: cannot access '%s': %s\n", prog, path, strerror(errno));
        status = 1;
        return;
    }
    mode_t cur = st.st_mode & 07777;
    int is_dir = S_ISDIR(st.st_mode);
    mode_t want;
    if (ms->has_ref)      want = ms->ref;
    else if (ms->numeric) want = (mode_t)ms->num;
    else if (apply_symbolic(ms->sym, cur, is_dir, &want) < 0) {
        fprintf(stderr, "%s: invalid mode: '%s'\n", prog, ms->sym);
        exit(1);
    }
    if (want != cur) {
        if (chmod(path, want) < 0) {
            if (!opt_f)
                fprintf(stderr, "%s: changing permissions of '%s': %s\n",
                        prog, path, strerror(errno));
            status = 1;
        } else if (opt_v || opt_c) {
            report(path, cur, want, 1);
        }
    } else if (opt_v) {
        report(path, cur, want, 0);
    }

    if (opt_R && is_dir) {
        DIR *d = opendir(path);
        if (!d) {
            if (!opt_f)
                fprintf(stderr, "%s: cannot read directory '%s': %s\n",
                        prog, path, strerror(errno));
            status = 1;
            return;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t pl = strlen(path), nl = strlen(e->d_name);
            char *sub = malloc(pl + 1 + nl + 1);
            if (!sub) break;
            memcpy(sub, path, pl);
            int slash = (pl > 0 && path[pl - 1] != '/');
            if (slash) sub[pl] = '/';
            memcpy(sub + pl + slash, e->d_name, nl + 1);
            do_path(sub, ms);
            free(sub);
        }
        closedir(d);
    }
}

static void usage(int rc) {
    FILE *o = rc ? stderr : stdout;
    fprintf(o, "Usage: %s [OPTION]... MODE[,MODE]... FILE...\n"
               "  or:  %s [OPTION]... OCTAL-MODE FILE...\n"
               "  or:  %s [OPTION]... --reference=RFILE FILE...\n"
               "Change the mode of each FILE to MODE.\n"
               "  -c, --changes     like verbose but report only when a change is made\n"
               "  -f, --silent      suppress most error messages\n"
               "  -v, --verbose     output a diagnostic for every file processed\n"
               "  -R, --recursive   change files and directories recursively\n"
               "      --reference=RFILE  use RFILE's mode instead of MODE values\n",
            prog, prog, prog);
    exit(rc);
}

/* `-w`, `-rx`, `-st`: an argument that starts with '-' but is made of mode
 * letters is a MODE, not an option -- GNU's rule, so `chmod -w f` works. */
static int looks_like_mode(const char *a) {
    if (a[0] != '-' || !a[1]) return 0;
    for (const char *p = a + 1; *p; p++)
        if (!strchr("rwxXstugoa+-=,", *p)) return 0;
    return 1;
}

int main(int argc, char **argv) {
    struct modespec ms = {0};
    int have_mode = 0;
    int first_file = -1;
    g_umask = umask(0);
    umask(g_umask);

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (a[0] != '-' || !a[1] || looks_like_mode(a)) break;
        if (!strncmp(a, "--reference=", 12)) {
            struct stat st;
            if (stat(a + 12, &st) < 0) {
                fprintf(stderr, "%s: failed to get attributes of '%s': %s\n",
                        prog, a + 12, strerror(errno));
                return 1;
            }
            ms.has_ref = 1;
            ms.ref = st.st_mode & 07777;
            have_mode = 1;
            continue;
        }
        if (!strcmp(a, "--recursive")) { opt_R = 1; continue; }
        if (!strcmp(a, "--verbose"))   { opt_v = 1; continue; }
        if (!strcmp(a, "--changes"))   { opt_c = 1; continue; }
        if (!strcmp(a, "--silent") || !strcmp(a, "--quiet")) { opt_f = 1; continue; }
        if (!strcmp(a, "--help"))      usage(0);
        if (!strcmp(a, "--version"))   { printf("chmod (tobyOS) 1.0\n"); return 0; }
        if (a[1] == '-') {
            fprintf(stderr, "%s: unrecognized option '%s'\n", prog, a);
            usage(1);
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'R': opt_R = 1; break;
            case 'v': opt_v = 1; break;
            case 'c': opt_c = 1; break;
            case 'f': opt_f = 1; break;
            default:
                fprintf(stderr, "%s: invalid option -- '%c'\n", prog, *p);
                usage(1);
            }
        }
    }
    if (!have_mode) {
        if (i >= argc) {
            fprintf(stderr, "%s: missing operand\n", prog);
            usage(1);
        }
        const char *m = argv[i++];
        long num = parse_octal(m);
        if (num >= 0) { ms.numeric = 1; ms.num = num; }
        else {
            /* validate once, against a plain mode, so a bad spec is refused
             * before any file is touched */
            mode_t probe;
            if (apply_symbolic(m, 0, 0, &probe) < 0) {
                fprintf(stderr, "%s: invalid mode: '%s'\n", prog, m);
                return 1;
            }
            ms.sym = m;
        }
        have_mode = 1;
    }
    first_file = i;
    if (first_file >= argc) {
        fprintf(stderr, "%s: missing operand after '%s'\n", prog, argv[argc - 1]);
        usage(1);
    }
    for (i = first_file; i < argc; i++) do_path(argv[i], &ms);
    return status;
}
