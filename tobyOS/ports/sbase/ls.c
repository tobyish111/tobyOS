/* ls -- list directory contents.
 *
 * Modeled on sbase's ls.c (ISC license). Subset:
 *
 *     ls [-aFl] [file ...]
 *
 *   -a   include entries beginning with '.'
 *   -F   append /, *, etc. to indicate file type
 *   -l   long listing: type + mode + size + name
 *
 * No -R (recursive), no -t (time-sorted), no -S (size-sorted), no
 * -h (human-readable). Sort is alphabetical by ASCII byte order.
 *
 * With no arguments lists the current directory. With one or more
 * file arguments, files are listed first (one per line) followed by
 * each directory's contents under a "name:" header (only when more
 * than one operand).
 */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

static int aflag, Fflag, lflag;

struct entry {
    char       *name;
    struct stat st;
};

static int entry_cmp(const void *a, const void *b) {
    const struct entry *ea = a, *eb = b;
    return strcmp(ea->name, eb->name);
}

static char type_char(mode_t m) {
    if (S_ISDIR(m)) return 'd';
    if (S_ISREG(m)) return '-';
    return '?';
}

static char *mode_str(mode_t m, char out[10]) {
    out[0] = (m & S_IRUSR) ? 'r' : '-';
    out[1] = (m & S_IWUSR) ? 'w' : '-';
    out[2] = (m & S_IXUSR) ? 'x' : '-';
    out[3] = (m & S_IRGRP) ? 'r' : '-';
    out[4] = (m & S_IWGRP) ? 'w' : '-';
    out[5] = (m & S_IXGRP) ? 'x' : '-';
    out[6] = (m & S_IROTH) ? 'r' : '-';
    out[7] = (m & S_IWOTH) ? 'w' : '-';
    out[8] = (m & S_IXOTH) ? 'x' : '-';
    out[9] = '\0';
    return out;
}

static const char *suffix(mode_t m) {
    if (!Fflag) return "";
    if (S_ISDIR(m)) return "/";
    return "";
}

static void print_entry(const struct entry *e) {
    if (lflag) {
        char perms[10];
        printf("%c%s %8ld %s%s\n",
               type_char(e->st.st_mode), mode_str(e->st.st_mode, perms),
               (long)e->st.st_size, e->name, suffix(e->st.st_mode));
    } else {
        printf("%s%s\n", e->name, suffix(e->st.st_mode));
    }
}

static char *path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    int    add_slash = (dl > 0 && dir[dl - 1] != '/');
    char  *p = emalloc(dl + (size_t)add_slash + nl + 1);
    memcpy(p, dir, dl);
    if (add_slash) p[dl] = '/';
    memcpy(p + dl + (size_t)add_slash, name, nl + 1);
    return p;
}

static int list_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) { weprintf("opendir %s:", path); return -1; }

    size_t cap = 16, n = 0;
    struct entry *items = emalloc(cap * sizeof(*items));

    struct dirent *de;
    while ((de = readdir(d)) != 0) {
        if (!aflag && de->d_name[0] == '.') continue;
        if (n == cap) {
            cap *= 2;
            items = erealloc(items, cap * sizeof(*items));
        }
        items[n].name = estrdup(de->d_name);
        char *full = path_join(path, de->d_name);
        if (stat(full, &items[n].st) < 0) {
            /* Best-effort: zero the stat so we still print the name. */
            memset(&items[n].st, 0, sizeof(items[n].st));
        }
        free(full);
        n++;
    }
    closedir(d);

    /* sbase sorts; do the same so output is stable. */
    if (n > 1) {
        /* Simple insertion sort; n stays tiny on tobyOS. */
        for (size_t i = 1; i < n; i++) {
            struct entry x = items[i];
            size_t j = i;
            while (j > 0 && entry_cmp(&items[j - 1], &x) > 0) {
                items[j] = items[j - 1];
                j--;
            }
            items[j] = x;
        }
    }

    for (size_t i = 0; i < n; i++) {
        print_entry(&items[i]);
        free(items[i].name);
    }
    free(items);
    return 0;
}

static void usage(void) { eprintf("usage: %s [-aFl] [file ...]", argv0); }

int main(int argc, char *argv[]) {
    util_argv0_set(argv[0]);

    int opt;
    while ((opt = getopt(argc, argv, "aFl")) != -1) {
        switch (opt) {
        case 'a': aflag = 1; break;
        case 'F': Fflag = 1; break;
        case 'l': lflag = 1; break;
        default:  usage();
        }
    }

    int   rc       = 0;
    int   nargs    = argc - optind;
    int   show_hd  = nargs > 1;

    if (nargs == 0) {
        return list_dir(".");
    }

    /* sbase prints non-dir operands first (each on its own line),
     * then dirs with their contents underneath. */
    for (int pass = 0; pass < 2; pass++) {
        int first_in_pass = 1;
        for (int i = optind; i < argc; i++) {
            struct stat st;
            if (stat(argv[i], &st) < 0) {
                if (pass == 0) { weprintf("stat %s:", argv[i]); rc = 1; }
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (pass == 0) continue;
                if (show_hd) {
                    printf("%s%s:\n", first_in_pass ? "" : "\n", argv[i]);
                    first_in_pass = 0;
                }
                if (list_dir(argv[i]) < 0) rc = 1;
            } else {
                if (pass == 1) continue;
                struct entry e = { .name = argv[i], .st = st };
                print_entry(&e);
            }
        }
    }
    return rc;
}
