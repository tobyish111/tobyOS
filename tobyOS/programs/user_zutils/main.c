/* zgrep zegrep zfgrep zdiff zcmp zless zmore gzexe lesspipe
 * bzgrep bzegrep bzfgrep bzdiff bzcmp bzless bzmore
 *
 * The compressed-file wrappers. In a distro these are shell scripts that
 * pipe `gzip -cdfq` into grep/diff/less; here they are one multi-call
 * binary, for one reason: popen() and system() both run /bin/sh, and
 * /bin/sh in this tree is the old minimal shell (the Makefile notes it
 * scored 0/14 against bash). Building a command string for it would make
 * these tools depend on the weakest shell in the image. fork + execvp +
 * waitpid needs no shell at all and no quoting rules, so that is what
 * these use.
 *
 * The decompression itself is never reimplemented -- each wrapper shells
 * out to /bin/gunzip, exactly as the real scripts do.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

static const char *prog = "zgrep";
static int status;

/* ------------------------------------------------------------ processes */

/* Run argv to completion. `fd_out`, when >= 0, becomes the child's stdout.
 * Returns the child's exit status, or -1 if it could not be started. */
static int run(char *const argv[], int fd_out) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (fd_out >= 0 && fd_out != 1) { dup2(fd_out, 1); close(fd_out); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (fd_out >= 0 && fd_out != 1) close(fd_out);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* ------------------------------------------------------------ temp files */

static char  tmpnames[8][128];
static int   ntmp;

static const char *make_temp(const char *tag) {
    if (ntmp >= 8) return NULL;
    snprintf(tmpnames[ntmp], sizeof tmpnames[0], "/tmp/.z%s.%d.%d",
             tag, (int)getpid(), ntmp);
    return tmpnames[ntmp++];
}

static void cleanup(void) {
    for (int i = 0; i < ntmp; i++) unlink(tmpnames[i]);
    ntmp = 0;
}

enum { COMP_NONE = 0, COMP_GZIP, COMP_BZIP2 };

/* Which compression, by MAGIC rather than by suffix or by which name we
 * were invoked as -- so bzgrep on a .gz still works, and a .bz2 that has
 * been renamed still decompresses. */
static int compression_of(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return COMP_NONE;
    unsigned char m[3] = {0, 0, 0};
    ssize_t n = read(fd, m, 3);
    close(fd);
    if (n >= 2 && m[0] == 0x1F && m[1] == 0x8B) return COMP_GZIP;
    if (n >= 3 && m[0] == 'B' && m[1] == 'Z' && m[2] == 'h') return COMP_BZIP2;
    return COMP_NONE;
}


/* Give back a path holding the PLAIN text of `path`: the file itself when
 * it is not compressed, or a temp file when it is. Returns NULL on error. */
static const char *plain_copy(const char *path, const char *tag) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "%s: %s: No such file or directory\n", prog, path);
        status = 2;
        return NULL;
    }
    /* Which decompressor is chosen by the file's MAGIC, not by the name we
     * were invoked as: bzgrep on a .gz works, and so does zgrep on a .bz2. */
    int comp = compression_of(path);
    if (comp == COMP_NONE) return path;

    const char *tmp = make_temp(tag);
    if (!tmp) return NULL;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { fprintf(stderr, "%s: cannot create %s\n", prog, tmp); status = 2; return NULL; }
    char *av[4];
    av[0] = (char *)(comp == COMP_BZIP2 ? "bunzip2" : "gunzip");
    av[1] = (char *)"-c"; av[2] = (char *)path; av[3] = NULL;
    int rc = run(av, fd);
    if (rc != 0) {
        fprintf(stderr, "%s: %s: decompression failed\n", prog, path);
        status = 2;
        return NULL;
    }
    return tmp;
}

/* ---------------------------------------------------------------- zgrep */

/* Reads the child's stdout so each line can be labelled with the ORIGINAL
 * filename -- grep only ever sees the temp file, and would name that. */
static int grep_capture(char *const argv[], const char *label) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fds[1]);
    /* libtoby has no fdopen(), so split the lines by hand. A line longer
     * than the buffer is emitted in pieces, and only the piece that starts
     * a line gets a prefix -- a long line stays one line. */
    char rbuf[4096];
    char line[8192];
    size_t ll = 0;
    int at_line_start = 1;
    ssize_t n;
    while ((n = read(fds[0], rbuf, sizeof rbuf)) > 0) {
        for (ssize_t k = 0; k < n; k++) {
            line[ll++] = rbuf[k];
            if (rbuf[k] == 10 || ll == sizeof line - 1) {
                if (at_line_start && label) printf("%s:", label);
                fwrite(line, 1, ll, stdout);
                at_line_start = (rbuf[k] == 10);
                ll = 0;
            }
        }
    }
    if (ll) {
        if (at_line_start && label) printf("%s:", label);
        fwrite(line, 1, ll, stdout);
    }
    close(fds[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static int do_zgrep(int argc, char **argv, int which) {
    char *gopts[32]; int ngopts = 0;
    char *files[128]; int nfiles = 0;
    const char *pattern = NULL;

    /* egrep/fgrep flavours become the corresponding grep flag. */
    if (which == 1) gopts[ngopts++] = (char *)"-E";
    if (which == 2) gopts[ngopts++] = (char *)"-F";

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '-' && a[1] && !pattern) {
            if (!strcmp(a, "-e")) {
                if (i + 1 < argc) pattern = argv[++i];
                continue;
            }
            if (ngopts < 30) gopts[ngopts++] = a;
            continue;
        }
        if (!pattern) { pattern = a; continue; }
        if (nfiles < 128) files[nfiles++] = a;
    }
    if (!pattern) {
        fprintf(stderr, "usage: %s [grep options] pattern [file ...]\n", prog);
        return 2;
    }

    /* With no files, decompress stdin. gunzip with no argument already
     * filters stdin, so hand grep the result through a temp file. */
    if (nfiles == 0) {
        const char *tmp = make_temp("in");
        if (!tmp) return 2;
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return 2;
        char *av[3]; av[0] = (char *)"gunzip"; av[1] = (char *)"-c"; av[2] = NULL;
        run(av, fd);
        char *gv[40]; int n = 0;
        gv[n++] = (char *)"grep";
        for (int i = 0; i < ngopts; i++) gv[n++] = gopts[i];
        gv[n++] = (char *)"--";
        gv[n++] = (char *)pattern;
        gv[n++] = (char *)tmp;
        gv[n] = NULL;
        int rc = grep_capture(gv, NULL);
        cleanup();
        return rc < 0 ? 2 : rc;
    }

    int any = 0, trouble = 0;
    for (int f = 0; f < nfiles; f++) {
        const char *src = plain_copy(files[f], "grep");
        if (!src) { trouble = 1; continue; }
        char *gv[40]; int n = 0;
        gv[n++] = (char *)"grep";
        for (int i = 0; i < ngopts; i++) gv[n++] = gopts[i];
        gv[n++] = (char *)"--";
        gv[n++] = (char *)pattern;
        gv[n++] = (char *)src;
        gv[n] = NULL;
        int rc = grep_capture(gv, nfiles > 1 ? files[f] : NULL);
        if (rc == 0) any = 1;
        else if (rc > 1) trouble = 1;
        cleanup();
    }
    if (trouble) return 2;
    return any ? 0 : 1;
}

/* ------------------------------------------------------ zdiff / zcmp */

static int do_zdiff(int argc, char **argv, const char *tool) {
    char *opts[32]; int nopts = 0;
    char *files[4];  int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { if (nopts < 30) opts[nopts++] = argv[i]; }
        else if (nfiles < 4) files[nfiles++] = argv[i];
    }
    if (nfiles < 2) {
        fprintf(stderr, "usage: %s [options] file1 file2\n", prog);
        return 2;
    }
    const char *a = plain_copy(files[0], "d1");
    if (!a) { cleanup(); return 2; }
    const char *b = plain_copy(files[1], "d2");
    if (!b) { cleanup(); return 2; }

    char *av[40]; int n = 0;
    av[n++] = (char *)tool;
    for (int i = 0; i < nopts; i++) av[n++] = opts[i];
    av[n++] = (char *)a;
    av[n++] = (char *)b;
    av[n] = NULL;
    int rc = run(av, -1);
    cleanup();
    return rc < 0 ? 2 : rc;
}

/* ------------------------------------------------------ zless / zmore */

static int do_zless(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file ...\n", prog);
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        const char *src = plain_copy(argv[i], "less");
        if (!src) { rc = 2; continue; }
        char *av[3];
        av[0] = (char *)"less"; av[1] = (char *)src; av[2] = NULL;
        int r = run(av, -1);
        if (r < 0) rc = 2;
        cleanup();
    }
    return rc;
}

/* --------------------------------------------------------- lesspipe */

/* less's input preprocessor: write the READABLE form of one file to
 * stdout. less itself does not invoke it here (nothing sets LESSOPEN by
 * default), but it is a useful command in its own right and scripts call
 * it directly. */
static int do_lesspipe(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: lesspipe file\n");
        return 1;
    }
    const char *path = argv[1];
    int comp = compression_of(path);
    if (comp != COMP_NONE) {
        char *av[4];
        av[0] = (char *)(comp == COMP_BZIP2 ? "bunzip2" : "gunzip");
        av[1] = (char *)"-c"; av[2] = (char *)path; av[3] = NULL;
        return run(av, -1);
    }
    char *av[3];
    av[0] = (char *)"cat"; av[1] = (char *)path; av[2] = NULL;
    return run(av, -1);
}

/* ------------------------------------------------------------- gzexe */

/* Turn an executable into a self-extracting shell script: the script
 * unpacks the compressed payload appended to itself, runs it, and cleans
 * up. The original is kept alongside as NAME~, exactly as gzexe does. */
static int do_gzexe(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: gzexe file\n"); return 1; }
    int rc = 0;
    for (int f = 1; f < argc; f++) {
        const char *path = argv[f];
        struct stat st;
        if (stat(path, &st) != 0) {
            fprintf(stderr, "gzexe: %s: No such file or directory\n", path);
            rc = 1; continue;
        }
        char gz[1024], bak[1024];
        snprintf(gz,  sizeof gz,  "%s.gzexe.tmp", path);
        snprintf(bak, sizeof bak, "%s~", path);

        int fd = open(gz, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) { fprintf(stderr, "gzexe: cannot create %s\n", gz); rc = 1; continue; }
        char *av[5];
        av[0] = (char *)"gzip"; av[1] = (char *)"-9"; av[2] = (char *)"-c";
        av[3] = (char *)path;   av[4] = NULL;
        if (run(av, fd) != 0) {
            fprintf(stderr, "gzexe: %s: compression failed\n", path);
            unlink(gz); rc = 1; continue;
        }
        /* Keep the original, then write the wrapper over the name. */
        if (rename(path, bak) != 0) {
            fprintf(stderr, "gzexe: cannot keep %s as %s\n", path, bak);
            unlink(gz); rc = 1; continue;
        }
        FILE *out = fopen(path, "wb");
        if (!out) { fprintf(stderr, "gzexe: cannot write %s\n", path); rc = 1; continue; }
        fprintf(out, "#!/bin/tsh\n");
        fprintf(out, "# Self-extracting archive made by gzexe.\n");
        /* The wrapper is exactly 10 lines, so the appended payload begins
         * on line 11. Off by even one and the tail of the script itself
         * gets fed into gunzip. */
        fprintf(out, "skip=%d\n", 11);
        fprintf(out, "tmp=/tmp/.gzexe.$$\n");
        fprintf(out, "tail -n +$skip \"$0\" | gunzip -c > $tmp || exit 1\n");
        fprintf(out, "chmod 755 $tmp 2>/dev/null\n");
        fprintf(out, "$tmp \"$@\"\n");
        fprintf(out, "rc=$?\n");
        fprintf(out, "rm -f $tmp\n");
        fprintf(out, "exit $rc\n");
        /* The payload follows the script text. */
        FILE *in = fopen(gz, "rb");
        if (in) {
            int c;
            while ((c = fgetc(in)) != EOF) fputc(c, out);
            fclose(in);
        }
        fclose(out);
        unlink(gz);
        printf("%s\n", path);
    }
    return rc;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "zgrep";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;

    int rc;
    /* The bz* names behave identically -- the decompressor is chosen from
     * the file's magic, not from the name. */
    if (!strcmp(base, "zgrep")  || !strcmp(base, "bzgrep"))  rc = do_zgrep(argc, argv, 0);
    else if (!strcmp(base, "zegrep") || !strcmp(base, "bzegrep")) rc = do_zgrep(argc, argv, 1);
    else if (!strcmp(base, "zfgrep") || !strcmp(base, "bzfgrep")) rc = do_zgrep(argc, argv, 2);
    else if (!strcmp(base, "zdiff")  || !strcmp(base, "bzdiff"))  rc = do_zdiff(argc, argv, "diff");
    else if (!strcmp(base, "zcmp")   || !strcmp(base, "bzcmp"))   rc = do_zdiff(argc, argv, "cmp");
    else if (!strcmp(base, "zless")  || !strcmp(base, "bzless"))  rc = do_zless(argc, argv);
    else if (!strcmp(base, "zmore")  || !strcmp(base, "bzmore"))  rc = do_zless(argc, argv);
    else if (!strcmp(base, "lesspipe")) rc = do_lesspipe(argc, argv);
    else if (!strcmp(base, "gzexe"))   rc = do_gzexe(argc, argv);
    else {
        fprintf(stderr, "%s: unknown applet\n", base);
        rc = 1;
    }
    cleanup();
    return status ? status : rc;
}
