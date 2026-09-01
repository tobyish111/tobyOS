/* ls -- list directory contents.
 *
 *     ls [-1aAdFhlrRSt] [-i] [file ...]
 *
 * `ls` was the single most-used command in the image that had no native
 * implementation. ports/sbase has one, but it accepts only -aFl and is
 * staged as `p_ls`; the name `ls` itself resolved to a busybox applet, and
 * busybox here is a gitignored, opt-in artifact -- so on a clean checkout
 * there was no `ls` at all.
 *
 * Columns: with a terminal, names are laid out down-then-across in columns
 * sized to the widest entry, which is what makes `ls` readable. Into a
 * pipe it is ONE PER LINE, because that is what every script that reads
 * ls's output expects, and getting that wrong silently breaks them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static int a_all, A_almost, d_dir, F_class, h_human, l_long;
static int r_rev, R_recurse, S_size, t_time, one_per_line, i_inode;
static int status;
static int term_cols = 80;

struct ent {
    char  *name;
    struct stat st;
    int    have_stat;
};

static void human(long bytes, char *out, size_t cap) {
    /* Integer only: user programs here are built -mno-sse, so no floats.
     * GNU prints one decimal; this rounds up to the unit, which is the
     * same information at the same glance. */
    static const char *u = "BKMGT";
    long v = bytes;
    int i = 0;
    while (v >= 1024 && i < 4) { v = (v + 1023) / 1024; i++; }
    if (i == 0) snprintf(out, cap, "%ld", v);
    else        snprintf(out, cap, "%ld%c", v, u[i]);
}

static char type_char(mode_t m) {
    if (S_ISDIR(m)) return 'd';
    if (S_ISREG(m)) return '-';
    return '?';
}

static void mode_str(mode_t m, char *out) {
    out[0] = type_char(m);
    static const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    /* setuid, setgid and sticky REPLACE the execute character of their
     * field -- lower case when execute is also set, upper case when it is
     * not. Without this a setuid binary is indistinguishable from an
     * ordinary one in `ls -l`, which is exactly the thing anyone looking at
     * /bin with `ls -l` is checking for. (Found by staging /bin/sudo mode
     * 4755: the tar recorded it, ramfs preserved it, and ls hid it.) */
    if (m & 04000) out[3] = (out[3] == 'x') ? 's' : 'S';
    if (m & 02000) out[6] = (out[6] == 'x') ? 's' : 'S';
    if (m & 01000) out[9] = (out[9] == 'x') ? 't' : 'T';
    out[10] = 0;
}

/* A suffix that says what the entry IS, for -F. */
static const char *classify(const struct ent *e) {
    if (!F_class || !e->have_stat) return "";
    if (S_ISDIR(e->st.st_mode)) return "/";
    if (e->st.st_mode & 0111)   return "*";
    return "";
}

static int cmp_ent(const void *pa, const void *pb) {
    const struct ent *a = (const struct ent *)pa;
    const struct ent *b = (const struct ent *)pb;
    int c;
    if (S_size && a->have_stat && b->have_stat) {
        if (a->st.st_size != b->st.st_size)
            c = (a->st.st_size < b->st.st_size) ? 1 : -1;   /* largest first */
        else c = strcmp(a->name, b->name);
    } else if (t_time && a->have_stat && b->have_stat) {
        if (a->st.st_mtime != b->st.st_mtime)
            c = (a->st.st_mtime < b->st.st_mtime) ? 1 : -1; /* newest first */
        else c = strcmp(a->name, b->name);
    } else {
        c = strcmp(a->name, b->name);
    }
    return r_rev ? -c : c;
}

static void fmt_time(time_t t, char *out, size_t cap) {
    struct tm *tm = localtime(&t);
    if (!tm) { snprintf(out, cap, "%12s", "?"); return; }
    static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    snprintf(out, cap, "%s %2d %02d:%02d",
             mon[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min);
}

static void print_long(const struct ent *e) {
    char m[16] = "?---------";
    char sz[32], tm[32];
    if (e->have_stat) {
        mode_str(e->st.st_mode, m);
        if (h_human) human((long)e->st.st_size, sz, sizeof sz);
        else         snprintf(sz, sizeof sz, "%ld", (long)e->st.st_size);
        fmt_time(e->st.st_mtime, tm, sizeof tm);
    } else {
        snprintf(sz, sizeof sz, "?");
        snprintf(tm, sizeof tm, "?");
    }
    if (i_inode) printf("%8d ", 0);   /* no st_ino in this struct stat */
    printf("%s %4u %4u %8s %s %s%s\n", m,
           e->have_stat ? (unsigned)e->st.st_uid : 0,
           e->have_stat ? (unsigned)e->st.st_gid : 0,
           sz, tm, e->name, classify(e));
}

/* Down-then-across, the layout ls has always used: the eye reads a column
 * top to bottom, so the names stay in order within a column. */
static void print_columns(struct ent *v, size_t n) {
    size_t widest = 0;
    for (size_t i = 0; i < n; i++) {
        size_t w = strlen(v[i].name) + strlen(classify(&v[i]));
        if (w > widest) widest = w;
    }
    size_t colw = widest + 2;
    size_t cols = colw ? (size_t)term_cols / colw : 1;
    if (cols == 0) cols = 1;
    size_t rows = (n + cols - 1) / cols;
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t idx = c * rows + r;
            if (idx >= n) continue;
            size_t w = strlen(v[idx].name) + strlen(classify(&v[idx]));
            printf("%s%s", v[idx].name, classify(&v[idx]));
            if (c + 1 < cols && idx + rows < n)
                for (size_t k = w; k < colw; k++) putchar(' ');
        }
        putchar('\n');
    }
}

static void list_dir(const char *path, int show_header);

static void emit(struct ent *v, size_t n, const char *dirpath) {
    qsort(v, n, sizeof *v, cmp_ent);
    if (l_long) {
        long total = 0;
        for (size_t i = 0; i < n; i++)
            if (v[i].have_stat) total += (long)((v[i].st.st_size + 1023) / 1024);
        if (dirpath) printf("total %ld\n", total);
        for (size_t i = 0; i < n; i++) print_long(&v[i]);
    } else if (one_per_line) {
        for (size_t i = 0; i < n; i++)
            printf("%s%s\n", v[i].name, classify(&v[i]));
    } else {
        print_columns(v, n);
    }
    if (R_recurse && dirpath) {
        for (size_t i = 0; i < n; i++) {
            if (!v[i].have_stat || !S_ISDIR(v[i].st.st_mode)) continue;
            if (!strcmp(v[i].name, ".") || !strcmp(v[i].name, "..")) continue;
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", dirpath, v[i].name);
            putchar('\n');
            list_dir(sub, 1);
        }
    }
}

static void list_dir(const char *path, int show_header) {
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: cannot open directory '%s': %s\n",
                path, strerror(errno));
        status = 2;
        return;
    }
    if (show_header) printf("%s:\n", path);
    struct ent *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int dot = e->d_name[0] == '.';
        if (dot && !a_all && !A_almost) continue;
        if (A_almost && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64;
                        v = (struct ent *)realloc(v, cap * sizeof *v); }
        v[n].name = strdup(e->d_name);
        char full[1024];
        if (!strcmp(path, "/")) snprintf(full, sizeof full, "/%s", e->d_name);
        else                    snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        v[n].have_stat = (stat(full, &v[n].st) == 0);
        n++;
    }
    closedir(d);
    emit(v, n, path);
    for (size_t i = 0; i < n; i++) free(v[i].name);
    free(v);
}

int main(int argc, char **argv) {
    /* Into a pipe, one name per line. Every script that parses ls assumes
     * it, and columns would silently break them. */
    if (!isatty(1)) one_per_line = 1;
    else {
        const char *c = getenv("COLUMNS");
        if (c && atoi(c) > 0) term_cols = atoi(c);
    }

    const char *files[256];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "--help")) {
                printf("usage: ls [-1aAdFhilrRSt] [file ...]\n");
                return 0;
            }
            if (!strcmp(a, "--color") || !strncmp(a, "--color=", 8)) continue;
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case '1': one_per_line = 1; l_long = 0; break;
                case 'a': a_all = 1; break;
                case 'A': A_almost = 1; break;
                case 'd': d_dir = 1; break;
                case 'F': F_class = 1; break;
                case 'h': h_human = 1; break;
                case 'i': i_inode = 1; break;
                case 'l': l_long = 1; one_per_line = 0; break;
                case 'r': r_rev = 1; break;
                case 'R': R_recurse = 1; break;
                case 'S': S_size = 1; break;
                case 't': t_time = 1; break;
                case 'C': one_per_line = 0; break;
                case 'c': case 'u': break;
                default:
                    fprintf(stderr, "ls: invalid option -- '%c'\n", *p);
                    return 2;
                }
            }
            continue;
        }
        if (nf < 256) files[nf++] = a;
    }

    if (nf == 0) { list_dir(".", 0); return status; }

    /* Files first, then directories -- and with more than one directory
     * each gets a header, which is what makes `ls a b` readable. */
    struct ent *plain = (struct ent *)calloc((size_t)nf, sizeof *plain);
    size_t np = 0;
    int ndirs = 0;
    for (int i = 0; i < nf; i++) {
        struct stat st;
        int ok = stat(files[i], &st) == 0;
        if (!ok) {
            fprintf(stderr, "ls: cannot access '%s': %s\n",
                    files[i], strerror(errno));
            status = 2;
            continue;
        }
        if (S_ISDIR(st.st_mode) && !d_dir) { ndirs++; continue; }
        plain[np].name = strdup(files[i]);
        plain[np].st = st;
        plain[np].have_stat = 1;
        np++;
    }
    if (np) emit(plain, np, NULL);
    for (size_t i = 0; i < np; i++) free(plain[i].name);
    free(plain);

    int printed = np > 0;
    for (int i = 0; i < nf; i++) {
        struct stat st;
        if (stat(files[i], &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) || d_dir) continue;
        if (printed) putchar('\n');
        list_dir(files[i], ndirs > 1 || np > 0);
        printed = 1;
    }
    return status;
}
