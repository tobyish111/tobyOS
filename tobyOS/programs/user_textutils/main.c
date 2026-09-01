/* comm expand unexpand fold nl od hexdump paste rev shuf split
 * readlink realpath mktemp groups hostname chown killall mount umount
 *
 * One multi-call binary, dispatching on argv[0]. The last block of names
 * in the essential set that existed only as busybox applets -- and
 * busybox here is a gitignored, opt-in artifact, so on a clean checkout
 * none of them existed at all.
 *
 * Three of these cannot do everything their name promises, and say which
 * piece is missing rather than pretending: `hostname` can read the name
 * but not set it, and `mount`/`umount` can report the table but not change
 * it, because no native syscall reaches the VFS mount path.
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
#include <time.h>
#include <sys/stat.h>

static const char *prog = "textutils";

static int slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* ------------------------------------------------------------ line I/O */

struct lines { char **v; size_t n, cap; };

static void lines_push(struct lines *L, const char *s) {
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 256;
        L->v = (char **)realloc(L->v, L->cap * sizeof(char *));
    }
    L->v[L->n++] = strdup(s);
}

static int read_lines(const char *path, struct lines *L) {
    memset(L, 0, sizeof *L);
    FILE *f = (!path || !strcmp(path, "-")) ? stdin : fopen(path, "r");
    if (!f) return -1;
    char b[8192];
    while (fgets(b, sizeof b, f)) {
        size_t l = strlen(b);
        while (l && (b[l-1] == '\n' || b[l-1] == '\r')) b[--l] = 0;
        lines_push(L, b);
    }
    if (f != stdin) fclose(f);
    return 0;
}

static void lines_free(struct lines *L) {
    for (size_t i = 0; i < L->n; i++) free(L->v[i]);
    free(L->v);
}

/* ---------------------------------------------------------------- comm */

static int do_comm(int argc, char **argv) {
    int s1 = 0, s2 = 0, s3 = 0;
    const char *f[2] = { NULL, NULL };
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *p = argv[i] + 1; *p; p++) {
                if (*p == '1') s1 = 1;
                else if (*p == '2') s2 = 1;
                else if (*p == '3') s3 = 1;
            }
            continue;
        }
        if (nf < 2) f[nf++] = argv[i];
    }
    if (nf < 2) { fprintf(stderr, "usage: comm [-123] file1 file2\n"); return 1; }
    struct lines A, B;
    if (read_lines(f[0], &A) != 0) { fprintf(stderr, "comm: %s: %s\n", f[0], strerror(errno)); return 1; }
    if (read_lines(f[1], &B) != 0) { fprintf(stderr, "comm: %s: %s\n", f[1], strerror(errno)); return 1; }
    size_t i = 0, j = 0;
    /* comm requires SORTED input and does a single merge pass -- it does
     * not sort for you, which is why unsorted input gives odd answers. */
    while (i < A.n || j < B.n) {
        int c;
        if (i >= A.n) c = 1;
        else if (j >= B.n) c = -1;
        else c = strcmp(A.v[i], B.v[j]);
        if (c < 0)      { if (!s1) printf("%s\n", A.v[i]); i++; }
        else if (c > 0) { if (!s2) printf("%s%s\n", s1 ? "" : "\t", B.v[j]); j++; }
        else {
            if (!s3) printf("%s%s%s\n", s1 ? "" : "\t", s2 ? "" : "\t", A.v[i]);
            i++; j++;
        }
    }
    lines_free(&A); lines_free(&B);
    return 0;
}

/* ------------------------------------------------- expand / unexpand */

static int do_expand(int argc, char **argv, int un) {
    int tabsz = 8;
    const char *files[64];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) { tabsz = atoi(argv[++i]); continue; }
        if (!strncmp(argv[i], "-t", 2) && argv[i][2]) { tabsz = atoi(argv[i] + 2); continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (nf < 64) files[nf++] = argv[i];
    }
    if (tabsz < 1) tabsz = 8;
    int pass = 0;
    do {
        FILE *f = nf ? fopen(files[pass], "r") : stdin;
        if (!f) { fprintf(stderr, "%s: %s: %s\n", prog, files[pass], strerror(errno)); return 1; }
        int c, col = 0, run = 0;
        while ((c = fgetc(f)) != EOF) {
            if (!un) {
                if (c == '\t') {
                    int next = ((col / tabsz) + 1) * tabsz;
                    while (col < next) { putchar(' '); col++; }
                    continue;
                }
                putchar(c);
                col = (c == '\n') ? 0 : col + 1;
                continue;
            }
            /* unexpand only collapses LEADING blanks, which is what makes
             * it safe to run over source code. */
            if (c == ' ' && col == run) { run++; col++; continue; }
            if (run) {
                int emitted = 0;
                while ((emitted + tabsz) <= run) { putchar('\t'); emitted += tabsz; }
                while (emitted < run) { putchar(' '); emitted++; }
                run = 0;
            }
            putchar(c);
            col = (c == '\n') ? 0 : col + 1;
            if (c == '\n') run = 0;
        }
        if (un && run) { int e = 0; while ((e + tabsz) <= run) { putchar('\t'); e += tabsz; }
                         while (e < run) { putchar(' '); e++; } }
        if (f != stdin) fclose(f);
        pass++;
    } while (pass < nf);
    return 0;
}

/* ---------------------------------------------------------------- fold */

static int do_fold(int argc, char **argv) {
    int width = 80, bytes = 0, spaces = 0;
    const char *files[64];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) { width = atoi(argv[++i]); continue; }
        if (!strncmp(argv[i], "-w", 2) && argv[i][2]) { width = atoi(argv[i] + 2); continue; }
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') { width = atoi(argv[i] + 1); continue; }
        if (!strcmp(argv[i], "-b")) { bytes = 1; continue; }
        if (!strcmp(argv[i], "-s")) { spaces = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (nf < 64) files[nf++] = argv[i];
    }
    (void)bytes;
    if (width < 1) width = 80;
    int pass = 0;
    do {
        FILE *f = nf ? fopen(files[pass], "r") : stdin;
        if (!f) { fprintf(stderr, "fold: %s: %s\n", files[pass], strerror(errno)); return 1; }
        char line[65536];
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
            size_t start = 0;
            while (l - start > (size_t)width) {
                size_t cut = start + (size_t)width;
                if (spaces) {
                    size_t k = cut;
                    while (k > start && line[k-1] != ' ') k--;
                    if (k > start) cut = k;
                }
                fwrite(line + start, 1, cut - start, stdout);
                putchar('\n');
                start = cut;
            }
            fwrite(line + start, 1, l - start, stdout);
            putchar('\n');
        }
        if (f != stdin) fclose(f);
        pass++;
    } while (pass < nf);
    return 0;
}

/* ------------------------------------------------------------------ nl */

static int do_nl(int argc, char **argv) {
    long n = 1;
    const char *files[64];
    int nf = 0;
    int all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-ba")) { all = 1; continue; }
        if (!strcmp(argv[i], "-bt")) { all = 0; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (nf < 64) files[nf++] = argv[i];
    }
    int pass = 0;
    do {
        FILE *f = nf ? fopen(files[pass], "r") : stdin;
        if (!f) { fprintf(stderr, "nl: %s: %s\n", files[pass], strerror(errno)); return 1; }
        char line[65536];
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
            /* nl numbers NON-EMPTY lines by default -- an empty line gets
             * blank padding and no number, which is why line numbers in nl
             * output do not match a file's line count. */
            if (l == 0 && !all) printf("       %s\n", line);
            else                printf("%6ld\t%s\n", n++, line);
        }
        if (f != stdin) fclose(f);
        pass++;
    } while (pass < nf);
    return 0;
}

/* ---------------------------------------------------------- od/hexdump */

/* GNU od with no options prints OCTAL 16-BIT WORDS, not bytes -- "AB"
 * comes out as 041101 because it is one little-endian word, where a byte
 * dump would say 101 102. -c gives characters; hexdump -C gives the
 * canonical hex-plus-ASCII view. Getting the default wrong produces
 * output that looks reasonable and matches nothing. */
static void dump_octal_words(FILE *f) {
    unsigned char b[16];
    size_t off = 0, n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) {
        printf("%07lo", (unsigned long)off);
        for (size_t i = 0; i + 1 < n; i += 2)
            printf(" %06o", (unsigned)(b[i] | (b[i+1] << 8)));
        if (n & 1) printf(" %06o", (unsigned)b[n-1]);
        putchar(10);
        off += n;
    }
    printf("%07lo\n", (unsigned long)off);
}

static void dump_chars(FILE *f) {
    unsigned char b[16];
    size_t off = 0, n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) {
        printf("%07lo", (unsigned long)off);
        for (size_t i = 0; i < n; i++) {
            unsigned char c = b[i];
            if (c == 10)      printf("  \\n");
            else if (c == 9)  printf("  \\t");
            else if (c == 0)  printf("  \\0");
            else if (c >= 32 && c < 127) printf("   %c", c);
            else              printf(" %03o", c);
        }
        putchar(10);
        off += n;
    }
    printf("%07lo\n", (unsigned long)off);
}

static void dump_canonical(FILE *f) {
    unsigned char b[16];
    size_t off = 0, n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) {
        printf("%08lx  ", (unsigned long)off);
        for (size_t i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", b[i]); else printf("   ");
            if (i == 7) putchar(32);
        }
        printf(" |");
        for (size_t i = 0; i < n; i++)
            putchar((b[i] >= 32 && b[i] < 127) ? (char)b[i] : 46);
        printf("|\n");
        off += n;
    }
    printf("%08lx\n", (unsigned long)off);
}

static int do_od(int argc, char **argv, int hexdump_style) {
    const char *path = NULL;
    int chars = 0, canonical = 0;
    (void)hexdump_style;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == 45 && argv[i][1]) {
            if (!strcmp(argv[i], "-C")) { canonical = 1; continue; }
            if (!strcmp(argv[i], "-c")) { chars = 1; continue; }
            continue;
        }
        path = argv[i];
    }
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) { fprintf(stderr, "%s: %s: %s\n", prog, path ? path : "-", strerror(errno)); return 1; }
    if (canonical)   dump_canonical(f);
    else if (chars)  dump_chars(f);
    else             dump_octal_words(f);
    if (f != stdin) fclose(f);
    return 0;
}

/* --------------------------------------------------------------- paste */

static int do_paste(int argc, char **argv) {
    const char *delim = "\t";
    const char *files[16];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { delim = argv[++i]; continue; }
        if (!strncmp(argv[i], "-d", 2) && argv[i][2]) { delim = argv[i] + 2; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (nf < 16) files[nf++] = argv[i];
    }
    if (nf == 0) { fprintf(stderr, "usage: paste [-d list] file...\n"); return 1; }
    struct lines L[16];
    size_t most = 0;
    for (int i = 0; i < nf; i++) {
        if (read_lines(files[i], &L[i]) != 0) {
            fprintf(stderr, "paste: %s: %s\n", files[i], strerror(errno));
            return 1;
        }
        if (L[i].n > most) most = L[i].n;
    }
    for (size_t r = 0; r < most; r++) {
        for (int i = 0; i < nf; i++) {
            if (i) fputc(delim[(i - 1) % (int)strlen(delim)], stdout);
            if (r < L[i].n) fputs(L[i].v[r], stdout);
        }
        putchar('\n');
    }
    for (int i = 0; i < nf; i++) lines_free(&L[i]);
    return 0;
}

/* ----------------------------------------------------------------- rev */

static int do_rev(int argc, char **argv) {
    const char *files[64];
    int nf = 0;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-' || !argv[i][1]) { if (nf < 64) files[nf++] = argv[i]; }
    int pass = 0;
    do {
        FILE *f = nf ? fopen(files[pass], "r") : stdin;
        if (!f) { fprintf(stderr, "rev: %s: %s\n", files[pass], strerror(errno)); return 1; }
        char line[65536];
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
            for (size_t i = l; i > 0; i--) putchar(line[i-1]);
            putchar('\n');
        }
        if (f != stdin) fclose(f);
        pass++;
    } while (pass < nf);
    return 0;
}

/* ---------------------------------------------------------------- shuf */

static int do_shuf(int argc, char **argv) {
    long count = -1;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { count = atol(argv[++i]); continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        path = argv[i];
    }
    struct lines L;
    if (read_lines(path, &L) != 0) { fprintf(stderr, "shuf: %s: %s\n", path, strerror(errno)); return 1; }

    /* Seeded from /dev/urandom, not from the clock: two shufs started in
     * the same second must not produce the same order. */
    unsigned seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, &seed, sizeof seed) != (ssize_t)sizeof seed) seed = 0; close(fd); }
    if (!seed) seed = (unsigned)time(NULL);
    srand(seed);

    for (size_t i = L.n; i > 1; i--) {          /* Fisher-Yates */
        size_t j = (size_t)(rand() % (int)i);
        char *t = L.v[i-1]; L.v[i-1] = L.v[j]; L.v[j] = t;
    }
    size_t lim = (count >= 0 && (size_t)count < L.n) ? (size_t)count : L.n;
    for (size_t i = 0; i < lim; i++) printf("%s\n", L.v[i]);
    lines_free(&L);
    return 0;
}

/* --------------------------------------------------------------- split */

static int do_split(int argc, char **argv) {
    long nlines = 1000;
    const char *path = NULL, *pfx = "x";
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) { nlines = atol(argv[++i]); continue; }
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') { nlines = atol(argv[i] + 1); continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (npos == 0) { path = argv[i]; npos++; }
        else if (npos == 1) { pfx = argv[i]; npos++; }
    }
    if (nlines < 1) nlines = 1000;
    FILE *f = (!path || !strcmp(path, "-")) ? stdin : fopen(path, "r");
    if (!f) { fprintf(stderr, "split: %s: %s\n", path, strerror(errno)); return 1; }
    char line[65536];
    long in_this = 0;
    int idx = 0;
    FILE *out = NULL;
    while (fgets(line, sizeof line, f)) {
        if (!out || in_this >= nlines) {
            if (out) fclose(out);
            char name[1024];
            /* aa, ab, ac ... the suffix split has always used. */
            snprintf(name, sizeof name, "%s%c%c", pfx,
                     'a' + (idx / 26) % 26, 'a' + idx % 26);
            out = fopen(name, "w");
            if (!out) { fprintf(stderr, "split: cannot create %s\n", name); if (f != stdin) fclose(f); return 1; }
            idx++;
            in_this = 0;
        }
        fputs(line, out);
        in_this++;
    }
    if (out) fclose(out);
    if (f != stdin) fclose(f);
    return 0;
}

/* --------------------------------------------- readlink / realpath */

static int do_readlink(int argc, char **argv, int canonical) {
    int rc = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "-e")) { canonical = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        char buf[1024];
        ssize_t n = readlink(argv[i], buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; printf("%s\n", buf); rc = 0; continue; }
        if (canonical) {
            /* Not a link: realpath still answers, with the path made
             * absolute against the cwd. */
            if (argv[i][0] == '/') { printf("%s\n", argv[i]); rc = 0; }
            else {
                char cwd[1024];
                if (getcwd(cwd, sizeof cwd)) {
                    printf("%s%s%s\n", cwd, strcmp(cwd, "/") ? "/" : "", argv[i]);
                    rc = 0;
                }
            }
        }
    }
    return rc;
}

/* -------------------------------------------------------------- mktemp */

static int do_mktemp(int argc, char **argv) {
    int dir = 0;
    const char *tmpl = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) { dir = 1; continue; }
        if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "-q")) continue;
        if (argv[i][0] == '-' && argv[i][1]) continue;
        tmpl = argv[i];
    }
    char name[1024];
    snprintf(name, sizeof name, "%s", tmpl ? tmpl : "/tmp/tmp.XXXXXX");
    /* Replace the trailing Xs. Seeded from the pid and the clock, and the
     * result is CREATED before the name is printed, so a caller cannot be
     * handed a name that something else then takes. */
    size_t l = strlen(name);
    size_t xs = 0;
    while (xs < l && name[l - 1 - xs] == 'X') xs++;
    if (xs == 0) { fprintf(stderr, "mktemp: template must end in XXXXXX\n"); return 1; }
    unsigned seed = (unsigned)getpid() ^ (unsigned)time(NULL);
    for (int attempt = 0; attempt < 128; attempt++) {
        for (size_t i = 0; i < xs; i++) {
            seed = seed * 1103515245u + 12345u;
            name[l - xs + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[(seed >> 16) % 36];
        }
        if (dir) {
            if (mkdir(name, 0700) == 0) { printf("%s\n", name); return 0; }
        } else {
            int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { close(fd); printf("%s\n", name); return 0; }
        }
    }
    fprintf(stderr, "mktemp: could not create a unique name\n");
    return 1;
}

/* -------------------------------------------------------------- groups */

static int do_groups(int argc, char **argv) {
    char buf[8192];
    const char *who = (argc > 1) ? argv[1] : NULL;
    char self[64];
    if (!who) {
        /* Our own name, from the identity database. */
        uid_t u = getuid();
        char pw[8192];
        snprintf(self, sizeof self, "%u", (unsigned)u);
        if (slurp("/etc/passwd", pw, sizeof pw) > 0) {
            for (char *p = pw; p && *p; ) {
                char *eol = strchr(p, '\n');
                if (eol) *eol = 0;
                char *c1 = strchr(p, ':');
                if (c1) {
                    char *c2 = strchr(c1 + 1, ':');
                    if (c2 && atol(c2 + 1) == (long)u) {
                        *c1 = 0;
                        snprintf(self, sizeof self, "%s", p);
                        break;
                    }
                }
                if (!eol) break;
                p = eol + 1;
            }
        }
        who = self;
    }
    if (slurp("/etc/group", buf, sizeof buf) < 0) {
        fprintf(stderr, "groups: cannot read /etc/group\n");
        return 1;
    }
    int first = 1;
    for (char *p = buf; p && *p; ) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        /* name:passwd:gid:members */
        char *c1 = strchr(p, ':');
        if (c1) {
            char *c2 = strchr(c1 + 1, ':');
            char *c3 = c2 ? strchr(c2 + 1, ':') : NULL;
            if (c3) {
                const char *members = c3 + 1;
                if (*members && strstr(members, who)) {
                    *c1 = 0;
                    if (!first) putchar(' ');
                    fputs(p, stdout);
                    first = 0;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    putchar('\n');
    return 0;
}

/* ------------------------------------------------------------ hostname */

static int do_hostname(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        fprintf(stderr, "hostname: cannot set the name: no sethostname is "
                        "exposed to userland; the UTS namespace is only "
                        "readable from here\n");
        return 1;
    }
    char buf[256];
    if (slurp("/proc/sys/kernel/hostname", buf, sizeof buf) < 0) {
        fprintf(stderr, "hostname: cannot read /proc/sys/kernel/hostname\n");
        return 1;
    }
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    printf("%s\n", buf);
    return 0;
}

/* -------------------------------------------------------------- getent
 *
 * The databases this system actually HAS: passwd and group are real files,
 * hosts is /etc/hosts if it exists plus the loopback name every resolver
 * knows, and ahosts/ahostsv4 answer from the same place. A database this
 * system does not keep (shadow, netgroup, rpc) is reported as unknown
 * rather than answered as empty -- `getent shadow` printing nothing and
 * exiting 0 would read as "no users have passwords".
 */

static int getent_file(const char *path, const char *key, int keyfield,
                       int numeric_field)
{
    FILE *f = fopen(path, "r");
    if (!f) return 2;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        if (!key) { printf("%s\n", line); found = 1; continue; }

        /* split on ':' just far enough to reach the two fields a key can
         * match: the name, and the numeric id */
        char copy[512];
        snprintf(copy, sizeof copy, "%s", line);
        char *fields[8];
        int nf = 0;
        char *p = copy;
        while (nf < 8) {
            fields[nf++] = p;
            char *c = strchr(p, ':');
            if (!c) break;
            *c = 0;
            p = c + 1;
        }
        if (keyfield < nf && !strcmp(fields[keyfield], key)) { printf("%s\n", line); found = 1; }
        else if (numeric_field >= 0 && numeric_field < nf &&
                 !strcmp(fields[numeric_field], key)) { printf("%s\n", line); found = 1; }
    }
    fclose(f);
    return found ? 0 : 2;
}

static int getent_hosts(const char *key) {
    int found = 0;
    FILE *f = fopen("/etc/hosts", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            if (!line[0]) continue;
            if (!key) { printf("%s\n", line); found = 1; continue; }
            /* the key may be the address or any of the names on the line */
            char copy[512];
            snprintf(copy, sizeof copy, "%s", line);
            char *tok = copy;
            while (*tok) {
                while (*tok == ' ' || *tok == '\t') tok++;
                if (!*tok) break;
                char *end = tok;
                while (*end && *end != ' ' && *end != '\t') end++;
                char save = *end;
                *end = 0;
                if (!strcmp(tok, key)) { printf("%s\n", line); found = 1; break; }
                *end = save;
                tok = end;
            }
        }
        fclose(f);
    }
    /* Loopback is not in a file here, and every getent answers it. */
    if (!found && (!key || !strcmp(key, "localhost") || !strcmp(key, "127.0.0.1"))) {
        printf("127.0.0.1       localhost\n");
        found = 1;
    }
    return found ? 0 : 2;
}

static int do_getent(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: getent DATABASE [KEY ...]\n");
        fprintf(stderr, "databases: passwd group hosts ahosts ahostsv4 services\n");
        return 1;
    }
    const char *db = argv[1];
    int rc = 2;

    if (!strcmp(db, "passwd")) {
        if (argc == 2) return getent_file("/etc/passwd", NULL, 0, -1);
        for (int i = 2; i < argc; i++) {
            int r = getent_file("/etc/passwd", argv[i], 0, 2);
            if (r == 0) rc = 0;
        }
        return rc;
    }
    if (!strcmp(db, "group")) {
        if (argc == 2) return getent_file("/etc/group", NULL, 0, -1);
        for (int i = 2; i < argc; i++) {
            int r = getent_file("/etc/group", argv[i], 0, 2);
            if (r == 0) rc = 0;
        }
        return rc;
    }
    if (!strcmp(db, "hosts") || !strcmp(db, "ahosts") || !strcmp(db, "ahostsv4")) {
        if (argc == 2) return getent_hosts(NULL);
        for (int i = 2; i < argc; i++) if (getent_hosts(argv[i]) == 0) rc = 0;
        return rc;
    }
    if (!strcmp(db, "services")) {
        if (argc == 2) return getent_file("/etc/services", NULL, 0, -1);
        for (int i = 2; i < argc; i++) {
            int r = getent_file("/etc/services", argv[i], 0, -1);
            if (r == 0) rc = 0;
        }
        return rc;
    }

    fprintf(stderr, "getent: unknown database '%s' -- this system keeps "
                    "passwd, group, hosts and services\n", db);
    return 2;
}

/* ---------------------------------------------------------- domainname */

static int do_domainname(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        fprintf(stderr, "domainname: cannot set the name: no setdomainname is "
                        "exposed to userland; the UTS namespace is only "
                        "readable from here\n");
        return 1;
    }
    char buf[256];
    if (slurp("/proc/sys/kernel/domainname", buf, sizeof buf) < 0) {
        fprintf(stderr, "domainname: cannot read /proc/sys/kernel/domainname\n");
        return 1;
    }
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    printf("%s\n", buf);
    return 0;
}

/* --------------------------------------------------------------- chown */

static int do_chown(int argc, char **argv) {
    const char *spec = NULL;
    int rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (!spec) { spec = argv[i]; continue; }
        any = 1;
        long uid = -1, gid = -1;
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%s", spec);
        char *colon = strchr(tmp, ':');
        if (colon) { *colon = 0; if (colon[1]) gid = atol(colon + 1); }
        if (tmp[0]) uid = atol(tmp);
        if (chown(argv[i], (uid_t)uid, (gid_t)gid) != 0) {
            fprintf(stderr, "chown: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    if (!spec || !any) { fprintf(stderr, "usage: chown UID[:GID] file...\n"); return 1; }
    return rc;
}

/* ------------------------------------------------------------- killall */

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

static int do_killall(int argc, char **argv) {
    int sig = SIGTERM;
    const char *names[32];
    int nn = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            const char *s = a + 1;
            if (*s >= '0' && *s <= '9') { sig = atoi(s); continue; }
            if (!strncmp(s, "SIG", 3)) s += 3;
            if (!strcmp(s, "KILL")) { sig = SIGKILL; continue; }
            if (!strcmp(s, "TERM")) { sig = SIGTERM; continue; }
            if (!strcmp(s, "HUP"))  { sig = SIGHUP;  continue; }
            if (!strcmp(s, "INT"))  { sig = SIGINT;  continue; }
            continue;
        }
        if (nn < 32) names[nn++] = a;
    }
    if (!nn) { fprintf(stderr, "usage: killall [-SIGNAL] name...\n"); return 1; }
    DIR *d = opendir("/proc");
    if (!d) { fprintf(stderr, "killall: cannot open /proc\n"); return 1; }
    int hits = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 1 || pid == (int)getpid()) continue;
        char nm[64];
        if (name_of_pid(e->d_name, nm, sizeof nm) != 0) continue;
        for (int k = 0; k < nn; k++) {
            /* killall matches the name EXACTLY, unlike pkill's substring --
             * that difference is the whole reason both exist. */
            if (strcmp(nm, names[k])) continue;
            if (kill(pid, sig) == 0) hits++;
        }
    }
    closedir(d);
    if (!hits) {
        for (int k = 0; k < nn; k++)
            fprintf(stderr, "killall: %s: no process found\n", names[k]);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------- mount / umount */

static int do_mount(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1]) continue;
            fprintf(stderr, "mount: cannot mount: no native syscall reaches "
                            "the VFS mount path from userland. The table "
                            "below is what is mounted now.\n");
            break;
        }
    }
    char buf[16384];
    if (slurp("/proc/mounts", buf, sizeof buf) < 0) {
        fprintf(stderr, "mount: cannot read /proc/mounts\n");
        return 1;
    }
    fputs(buf, stdout);
    return 0;
}

static int do_umount(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "umount: cannot unmount: no native syscall reaches the "
                    "VFS mount path from userland\n");
    return 1;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;

    if (!strcmp(base, "comm"))     return do_comm(argc, argv);
    if (!strcmp(base, "expand"))   return do_expand(argc, argv, 0);
    if (!strcmp(base, "unexpand")) return do_expand(argc, argv, 1);
    if (!strcmp(base, "fold"))     return do_fold(argc, argv);
    if (!strcmp(base, "nl"))       return do_nl(argc, argv);
    if (!strcmp(base, "od"))       return do_od(argc, argv, 0);
    if (!strcmp(base, "hexdump"))  return do_od(argc, argv, 1);
    if (!strcmp(base, "paste"))    return do_paste(argc, argv);
    if (!strcmp(base, "rev"))      return do_rev(argc, argv);
    if (!strcmp(base, "shuf"))     return do_shuf(argc, argv);
    if (!strcmp(base, "split"))    return do_split(argc, argv);
    if (!strcmp(base, "readlink")) return do_readlink(argc, argv, 0);
    if (!strcmp(base, "realpath")) return do_readlink(argc, argv, 1);
    if (!strcmp(base, "mktemp"))   return do_mktemp(argc, argv);
    if (!strcmp(base, "groups"))   return do_groups(argc, argv);
    if (!strcmp(base, "hostname")) return do_hostname(argc, argv);
    if (!strcmp(base, "getent"))   return do_getent(argc, argv);
    if (!strcmp(base, "domainname")) return do_domainname(argc, argv);
    if (!strcmp(base, "chown"))    return do_chown(argc, argv);
    if (!strcmp(base, "killall"))  return do_killall(argc, argv);
    if (!strcmp(base, "mount"))    return do_mount(argc, argv);
    if (!strcmp(base, "umount"))   return do_umount(argc, argv);

    fprintf(stderr, "%s: unknown applet\n", base);
    return 1;
}
