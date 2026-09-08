/* ls -- list directory contents.
 *
 *     ls [-1aAdFgGhlnorRSt] [-i] [file ...]
 *
 * `ls -l` prints GNU's columns -- mode, link count, OWNER and GROUP by
 * name, size, date, name -- since 2026-09-03. It printed numeric ids and no
 * link count before, so `ls -l | awk '{print $5}'` (the size, everywhere
 * else) gave the date here, and a bare number where a name belonged.
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
static int n_numeric, g_noowner, o_nogroup;   /* -n, -g, -o/-G */
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

/* GNU's date column: a file modified in the last six months (and not more
 * than an hour in the future) shows its time of day, anything older shows
 * its year instead. The six months are GNU's own 15778476 seconds. */
static void fmt_time(time_t t, time_t now, char *out, size_t cap) {
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) { snprintf(out, cap, "%12s", "?"); return; }
    if (t <= now + 3600 && t + 15778476L >= now)
        strftime(out, cap, "%b %e %H:%M", &tmv);
    else
        strftime(out, cap, "%b %e  %Y", &tmv);
}

/* Owner and group NAMES, from the files everything else reads them from.
 * ls printed bare numbers here, which is not what `ls -l` looks like
 * anywhere else; -n asks for the numbers back. */
static const char *id_name(const char *file, unsigned id, char *out, size_t cap) {
    snprintf(out, cap, "%u", id);
    if (n_numeric) return out;
    FILE *f = fopen(file, "r");
    if (!f) return out;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':'); if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':'); if (!c2) continue;
        if ((unsigned)atoi(c2 + 1) == id) {
            *c1 = '\0';
            snprintf(out, cap, "%s", line);
            break;
        }
    }
    fclose(f);
    return out;
}

static const char *user_name(unsigned uid) {
    static char cache[8][40]; static unsigned ids[8]; static int n;
    for (int i = 0; i < n; i++) if (ids[i] == uid) return cache[i];
    int slot = n < 8 ? n++ : 7;
    ids[slot] = uid;
    return id_name("/etc/passwd", uid, cache[slot], sizeof cache[slot]);
}

static const char *group_name(unsigned gid) {
    static char cache[8][40]; static unsigned ids[8]; static int n;
    for (int i = 0; i < n; i++) if (ids[i] == gid) return cache[i];
    int slot = n < 8 ? n++ : 7;
    ids[slot] = gid;
    return id_name("/etc/group", gid, cache[slot], sizeof cache[slot]);
}

/* The REAL hard-link count (2026-09-08). This column used to be
 * synthesised -- 1 for a file, 2 for a directory -- because struct stat
 * carried no st_nlink, so a file with three names still read as 1 and the
 * column was decoration rather than information. The kernel substitutes
 * the same 1/2 defaults when a filesystem genuinely has no answer, so an
 * fs without link counts looks exactly as it did. */
static int nlink_of(const struct ent *e) {
    if (!e->have_stat) return 1;
    if (e->st.st_nlink) return (int)e->st.st_nlink;
    return S_ISDIR(e->st.st_mode) ? 2 : 1;
}

static void size_of(const struct ent *e, char *out, size_t cap) {
    if (!e->have_stat) { snprintf(out, cap, "?"); return; }
    if (h_human) human((long)e->st.st_size, out, cap);
    else         snprintf(out, cap, "%ld", (long)e->st.st_size);
}

/* Column widths are the widest entry in the listing, as in GNU ls: nlink
 * and size right-aligned, owner and group left-aligned. */
struct lwidths { int nlink, user, group, size; };

static void long_widths(const struct ent *v, size_t n, struct lwidths *w) {
    char buf[64];
    w->nlink = w->user = w->group = w->size = 1;
    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%d", nlink_of(&v[i]));
        if ((int)strlen(buf) > w->nlink) w->nlink = (int)strlen(buf);
        size_of(&v[i], buf, sizeof buf);
        if ((int)strlen(buf) > w->size) w->size = (int)strlen(buf);
        if (v[i].have_stat) {
            int ul = (int)strlen(user_name((unsigned)v[i].st.st_uid));
            int gl = (int)strlen(group_name((unsigned)v[i].st.st_gid));
            if (ul > w->user) w->user = ul;
            if (gl > w->group) w->group = gl;
        }
    }
}

static void pad_to(int have, int want) { while (have++ < want) putchar(' '); }

static void print_long(const struct ent *e, const struct lwidths *w, time_t now) {
    char m[16] = "?---------";
    char sz[32], tm[32], nl[16];
    if (e->have_stat) {
        mode_str(e->st.st_mode, m);
        fmt_time(e->st.st_mtime, now, tm, sizeof tm);
    } else {
        snprintf(tm, sizeof tm, "?");
    }
    size_of(e, sz, sizeof sz);
    snprintf(nl, sizeof nl, "%d", nlink_of(e));
    if (i_inode) printf("%8d ", 0);   /* no st_ino in this struct stat */
    fputs(m, stdout);
    putchar(' ');
    pad_to((int)strlen(nl), w->nlink); fputs(nl, stdout);
    if (!g_noowner) {
        const char *u = e->have_stat ? user_name((unsigned)e->st.st_uid) : "?";
        putchar(' '); fputs(u, stdout); pad_to((int)strlen(u), w->user);
    }
    if (!o_nogroup) {
        const char *g = e->have_stat ? group_name((unsigned)e->st.st_gid) : "?";
        putchar(' '); fputs(g, stdout); pad_to((int)strlen(g), w->group);
    }
    putchar(' ');
    pad_to((int)strlen(sz), w->size); fputs(sz, stdout);
    printf(" %s %s%s\n", tm, e->name, classify(e));
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
        struct lwidths w;
        long_widths(v, n, &w);
        time_t now = time(0);
        for (size_t i = 0; i < n; i++) print_long(&v[i], &w, now);
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
                printf("usage: ls [-1aAdFgGhilnorRSt] [file ...]\n");
                return 0;
            }
            if (!strcmp(a, "--color") || !strncmp(a, "--color=", 8)) continue;
            if (!strcmp(a, "--no-group")) { o_nogroup = 1; continue; }
            if (!strcmp(a, "--numeric-uid-gid")) { n_numeric = 1; l_long = 1; one_per_line = 0; continue; }
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
                case 'n': n_numeric = 1; l_long = 1; one_per_line = 0; break;
                case 'g': g_noowner = 1; l_long = 1; one_per_line = 0; break;
                case 'o': o_nogroup = 1; l_long = 1; one_per_line = 0; break;
                case 'G': o_nogroup = 1; break;
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
