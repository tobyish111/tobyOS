/* /bin/cp -- copy files, close enough to coreutils to not surprise anyone.
 *
 * What this was: `cp [-v] src dst`, exactly two arguments, no flags. It could
 * not do `cp file dir/`, which is most of how anyone actually uses cp, and it
 * could not copy a directory at all. Under tsh that made ordinary shell
 * muscle memory fail in ways that looked like the shell was broken.
 *
 * What it is now: -r/-R, -n, -f, -i, -p, -v, multiple sources into a
 * directory, and -U.
 *
 * -U (--unique) is the one thing here that coreutils does not have: when the
 * destination exists, pick the next free name instead of overwriting --
 * report.txt -> report_1.txt. The naming lives in <tobyos/unique_name.h> so
 * File Explorer's Duplicate produces byte-identical names.
 *
 * Overwriting stays the DEFAULT, because `cp a b` clobbering b is what every
 * script ever written expects, and this shell is held to bash parity. -U is
 * opt-in for exactly that reason.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <tobyos/unique_name.h>

#define NAME_CAP 256
#define PATH_CAP 1024

static int f_recursive, f_noclobber, f_force, f_interactive;
static int f_preserve, f_verbose, f_unique;
static int had_error;

static void usage(void) {
    fprintf(stderr,
        "usage: cp [-rRnfipvU] source... destination\n"
        "  -r, -R  copy directories recursively\n"
        "  -n      never overwrite an existing file\n"
        "  -f      overwrite without asking (default)\n"
        "  -i      ask before overwriting\n"
        "  -p      preserve the source's permission bits\n"
        "  -v      say what is being copied\n"
        "  -U      on a clash, pick a free name (report.txt -> report_1.txt)\n");
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *c = p; *c; c++) if (*c == '/') b = c + 1;
    return b;
}

/* join, but never emit "//" and never overflow */
static int join(char *out, size_t cap, const char *dir, const char *name) {
    size_t n = 0;
    if (!dir || !*dir) dir = ".";
    while (dir[n] && n + 1 < cap) { out[n] = dir[n]; n++; }
    if (n && out[n - 1] != '/' && n + 1 < cap) out[n++] = '/';
    for (size_t i = 0; name[i] && n + 1 < cap; i++) out[n++] = name[i];
    if (n + 1 > cap) return 0;
    out[n] = '\0';
    return 1;
}

/* Ask on stdin, like coreutils. A non-tty or EOF answers "no": an
 * unattended script must not be able to overwrite by accident just because
 * nobody was there to say no. */
static int say_yes(const char *path) {
    fprintf(stderr, "cp: overwrite '%s'? ", path);
    fflush(stderr);
    char line[8];
    if (!fgets(line, sizeof line, stdin)) { fprintf(stderr, "\n"); return 0; }
    return line[0] == 'y' || line[0] == 'Y';
}

/* Resolve the destination this source should land on, applying -n/-i/-U.
 * Returns 1 to proceed with `out`, 0 to skip this source. */
static int settle_dest(char *out, size_t cap, const char *dst_arg,
                       const char *src, int dst_is_dir) {
    if (dst_is_dir) {
        if (!join(out, cap, dst_arg, base_name(src))) {
            fprintf(stderr, "cp: path too long under '%s'\n", dst_arg);
            had_error = 1; return 0;
        }
    } else {
        size_t i = 0;
        while (dst_arg[i] && i + 1 < cap) { out[i] = dst_arg[i]; i++; }
        out[i] = '\0';
    }

    if (!exists(out)) return 1;

    if (f_unique) {
        /* Rename the LEAF only; the directory part must stay put. */
        char dir[PATH_CAP], cand[NAME_CAP], full[PATH_CAP];
        const char *leaf = base_name(out);
        size_t dlen = (size_t)(leaf - out);
        if (dlen >= sizeof dir) { had_error = 1; return 0; }
        for (size_t i = 0; i < dlen; i++) dir[i] = out[i];
        dir[dlen] = '\0';
        for (unsigned nth = 1; nth <= 9999; nth++) {
            if (!unique_name_nth(cand, sizeof cand, leaf, nth)) break;
            if (!join(full, sizeof full, dlen ? dir : ".", cand)) break;
            if (!exists(full)) {
                size_t i = 0;
                while (full[i] && i + 1 < cap) { out[i] = full[i]; i++; }
                out[i] = '\0';
                return 1;
            }
        }
        fprintf(stderr, "cp: no free name for '%s'\n", out);
        had_error = 1; return 0;
    }
    if (f_noclobber) { if (f_verbose) fprintf(stderr, "cp: not overwriting '%s'\n", out); return 0; }
    if (f_interactive && !say_yes(out)) return 0;
    return 1;
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: cannot open '%s': %s\n", src, strerror(errno));
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "cp: cannot create '%s': %s\n", dst, strerror(errno));
        close(in);
        return -1;
    }
    char buf[4096];
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        long w = 0;
        while (w < n) {
            long k = write(out, buf + w, (size_t)(n - w));
            if (k <= 0) {
                fprintf(stderr, "cp: write error on '%s': %s\n", dst, strerror(errno));
                rc = -1; goto done;
            }
            w += k;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: read error on '%s': %s\n", src, strerror(errno));
        rc = -1;
    }
done:
    close(in);
    close(out);
    if (rc == 0 && f_preserve) {
        struct stat st;
        if (stat(src, &st) == 0) (void)chmod(dst, st.st_mode & 07777);
    }
    if (rc == 0 && f_verbose) printf("'%s' -> '%s'\n", src, dst);
    return rc;
}

static int copy_tree(const char *src, const char *dst);

/* One source onto one already-settled destination path. */
static int copy_one(const char *src, const char *dst) {
    if (is_dir(src)) {
        if (!f_recursive) {
            fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src);
            return -1;
        }
        return copy_tree(src, dst);
    }
    return copy_file(src, dst);
}

static int copy_tree(const char *src, const char *dst) {
    if (mkdir(dst, 0755) != 0 && !is_dir(dst)) {
        fprintf(stderr, "cp: cannot create directory '%s': %s\n", dst, strerror(errno));
        return -1;
    }
    if (f_verbose) printf("'%s' -> '%s'\n", src, dst);

    DIR *d = opendir(src);
    if (!d) {
        fprintf(stderr, "cp: cannot read directory '%s': %s\n", src, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[PATH_CAP], t[PATH_CAP];
        if (!join(s, sizeof s, src, e->d_name) ||
            !join(t, sizeof t, dst, e->d_name)) {
            fprintf(stderr, "cp: path too long under '%s'\n", src);
            rc = -1; continue;
        }
        /* -n/-i/-U apply inside the tree too, or a recursive copy would
         * quietly overwrite what a top-level copy just refused to. */
        if (exists(t)) {
            char settled[PATH_CAP];
            if (!settle_dest(settled, sizeof settled, t, s, 0)) continue;
            if (copy_one(s, settled) != 0) rc = -1;
            continue;
        }
        if (copy_one(s, t) != 0) rc = -1;
    }
    closedir(d);
    return rc;
}

int main(int argc, char **argv) {
    int i = 1;
    /* Hand-rolled rather than getopt(): -rRnfipvU must clump ("cp -rv"),
     * and "--unique" has to be recognised too. */
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--unique"))    { f_unique = 1; continue; }
        if (!strcmp(a, "--recursive")) { f_recursive = 1; continue; }
        if (!strcmp(a, "--verbose"))   { f_verbose = 1; continue; }
        if (!strcmp(a, "--help"))      { usage(); return 0; }
        if (a[1] == '-') { fprintf(stderr, "cp: unknown option '%s'\n", a); usage(); return 1; }
        for (const char *c = a + 1; *c; c++) {
            switch (*c) {
            case 'r': case 'R': f_recursive = 1; break;
            case 'n': f_noclobber = 1; f_interactive = 0; break;
            case 'f': f_force = 1; f_noclobber = 0; f_interactive = 0; break;
            case 'i': f_interactive = 1; f_noclobber = 0; break;
            case 'p': f_preserve = 1; break;
            case 'v': f_verbose = 1; break;
            case 'U': f_unique = 1; break;
            case 'a': f_recursive = 1; f_preserve = 1; break;
            default:
                fprintf(stderr, "cp: unknown option '-%c'\n", *c);
                usage();
                return 1;
            }
        }
    }
    (void)f_force;

    int nargs = argc - i;
    if (nargs < 2) { usage(); return 1; }

    const char *dst_arg = argv[argc - 1];
    int nsrc = nargs - 1;
    int dst_is_dir = is_dir(dst_arg);

    /* Several sources can only go INTO a directory. coreutils refuses this
     * rather than copying each over the last one in turn. */
    if (nsrc > 1 && !dst_is_dir) {
        fprintf(stderr, "cp: target '%s' is not a directory\n", dst_arg);
        return 1;
    }

    for (int s = 0; s < nsrc; s++) {
        const char *src = argv[i + s];
        char dst[PATH_CAP];

        /* A recursive copy names the new directory after the source when the
         * target is an existing directory: cp -r a b/ makes b/a. */
        if (!settle_dest(dst, sizeof dst, dst_arg, src, dst_is_dir)) continue;
        if (copy_one(src, dst) != 0) had_error = 1;
    }
    return had_error ? 1 : 0;
}
