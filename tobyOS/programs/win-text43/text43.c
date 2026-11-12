/* win-text43 -- Track C, W7l: the C runtime's text mode. On Windows a CRT
 * stream or descriptor is in text mode unless opened binary: what the program
 * writes has each '\n' written as "\r\n", and what it reads has each "\r\n"
 * read as '\n' and ends at a Ctrl-Z. _setmode switches a descriptor and
 * answers the mode it had; _fmode (_set_fmode/_get_fmode) is the default for
 * fopen and _open when the mode says neither 'b' nor 't'. Each check is what
 * Windows 11 does (measured first: this program exits 45 there); the program
 * runs itself as children over pipes to see the bytes a child writes and to
 * give one the bytes it reads. Exit 45 iff all pass, else 100 + the first
 * check that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
/* the bytes, made visible */
static void show(const char *tag, const unsigned char *b, int n) {
    char o[700]; int k = 0;
    for (int i = 0; i < n && k < (int)sizeof o - 8; i++) {
        unsigned c = b[i];
        if (c == '\r') { o[k++] = '\\'; o[k++] = 'r'; }
        else if (c == '\n') { o[k++] = '\\'; o[k++] = 'n'; }
        else if (c < 32 || c > 126) k += sprintf(o + k, "\\x%02X", c);
        else o[k++] = (char)c;
    }
    o[k] = 0;
    printf("     %s (%d): \"%s\"\n", tag, n, o);
}
static int same(const unsigned char *b, int n, const char *want, int wn) {
    return n == wn && memcmp(b, want, (size_t)wn) == 0;
}
#define SAME(b, n, lit) same((const unsigned char *)(b), (n), (lit), (int)sizeof(lit) - 1)

static int g_iph;
static void iph(const wchar_t *a, const wchar_t *b, const wchar_t *c, unsigned d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    g_iph++;
}

static char g_self[MAX_PATH], g_dir[MAX_PATH];
static const char *at(const char *name) {
    static char p[4][MAX_PATH]; static int k;
    char *r = p[k++ & 3];
    strcpy(r, g_dir); strcat(r, name);
    return r;
}
static int rawread(const char *path, unsigned char *b, int cap) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    int n = (int)fread(b, 1, (size_t)cap, f); fclose(f); return n;
}
static void rawwrite(const char *path, const char *b, int n) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite(b, 1, (size_t)n, f); fclose(f);
}

/* Run this program as a child with `mode` as its argument, its stdin, stdout
 * and stderr pipes of ours: give it `input` (in two writes, 150 ms apart, if
 * `split`), collect what it writes. Answers the child's exit code. */
static int run(const char *mode, const char *input, int inlen, int split,
               unsigned char *out, int *outn, int cap, unsigned char *err, int *errn) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE ir, iw, orr, ow, er, ew;
    if (!CreatePipe(&ir, &iw, &sa, 0) || !CreatePipe(&orr, &ow, &sa, 0) || !CreatePipe(&er, &ew, &sa, 0))
        return -1;
    SetHandleInformation(iw, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(orr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(er, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = ir; si.hStdOutput = ow; si.hStdError = ew;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 40];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", g_self, mode);
    if (!CreateProcessA(g_self, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) return -2;
    CloseHandle(ir); CloseHandle(ow); CloseHandle(ew);
    DWORD w;
    if (input) {
        if (split) {
            WriteFile(iw, input, (DWORD)split, &w, 0);
            Sleep(150);
            WriteFile(iw, input + split, (DWORD)(inlen - split), &w, 0);
        } else WriteFile(iw, input, (DWORD)inlen, &w, 0);
    }
    CloseHandle(iw);
    int n = 0; DWORD got;
    while (n < cap && ReadFile(orr, out + n, (DWORD)(cap - n), &got, 0) && got) n += (int)got;
    int en = 0;
    if (err) while (en < 400 && ReadFile(er, err + en, (DWORD)(400 - en), &got, 0) && got) en += (int)got;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(orr); CloseHandle(er);
    *outn = n;
    if (errn) *errn = en;
    return (int)code;
}

/* ---- the children ---- */
static int child(const char *m) {
    _set_invalid_parameter_handler(iph);
    if (!strcmp(m, "out")) {                      /* 1-3: what it writes */
        int r1 = printf("p1\n");
        int r2 = puts("p2");
        int r3 = fputs("p3\n", stdout);
        int r4 = (int)fwrite("p4\n", 1, 3, stdout);
        fputc('5', stdout); fputc('\n', stdout); putchar('\n');
        int r6 = fprintf(stdout, "p6\r\n");
        fflush(stdout);
        int r7 = _write(1, "w7\n", 3);
        int prev = _setmode(1, _O_BINARY);
        printf("b8\n"); fflush(stdout);
        int prev2 = _setmode(1, _O_TEXT);
        printf("t9\n"); fflush(stdout);
        DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "k10\n", 4, &w, 0);
        int prev3 = _setmode(0, _O_TEXT);
        errno = 0; int bad = _setmode(99, _O_BINARY); int e1 = errno; int h1 = g_iph;
        errno = 0; int bad2 = _setmode(1, 0x1234); int e2 = errno; int h2 = g_iph;
        int pb = _setmode(1, _O_BINARY);
        printf("[%d %d %d %d %d %d|%x %x %x|%d %d %d|%d %d %d|%x]",
               r1, r2, r3, r4, r6, r7, prev, prev2, prev3, bad, e1, h1, bad2, e2, h2, pb);
        fflush(stdout);
        fputc('\n', stderr); fprintf(stderr, "e1\n"); fputs("e2\n", stderr);
        return 7;
    }
    if (!strcmp(m, "in")) {                       /* 9: fgets on stdin */
        char line[64]; unsigned char out[512]; int on = 0;
        while (fgets(line, sizeof line, stdin)) {
            int l = (int)strlen(line);
            out[on++] = '['; memcpy(out + on, line, (size_t)l); on += l; out[on++] = ']';
        }
        out[on++] = feof(stdin) ? 'E' : 'e';
        _setmode(1, _O_BINARY);
        fwrite(out, 1, (size_t)on, stdout);
        return 8;
    }
    if (!strcmp(m, "inraw")) {                    /* 9: _read on stdin */
        unsigned char b[200];
        int n = _read(0, b, 200);
        int n2 = _read(0, b + (n > 0 ? n : 0), 50);
        _setmode(1, _O_BINARY);
        printf("%d %d:", n, n2);
        fwrite(b, 1, (size_t)(n > 0 ? n : 0), stdout);
        return 9;
    }
    if (!strcmp(m, "inbin")) {                    /* 9: binary stdin */
        int prev = _setmode(0, _O_BINARY);
        unsigned char b[200]; int n = (int)fread(b, 1, 200, stdin);
        _setmode(1, _O_BINARY);
        printf("%x:", prev);
        fwrite(b, 1, (size_t)n, stdout);
        return 10;
    }
    return 1;
}

int main(int argc, char **argv) {
    GetModuleFileNameA(0, g_self, sizeof g_self);
    if (argc > 1) return child(argv[1]);
    _set_invalid_parameter_handler(iph);
    GetTempPathA(sizeof g_dir, g_dir);
    if (g_dir[0] && g_dir[strlen(g_dir) - 1] != '\\') strcat(g_dir, "\\");
    strcat(g_dir, "w7l");
    CreateDirectoryA(g_dir, NULL);
    strcat(g_dir, "\\");

    unsigned char b[2000], e[400]; int n, en, rc;

    /* 1-3: a child's stdout and stderr, pipes, in text mode */
    rc = run("out", 0, 0, 0, b, &n, sizeof b, e, &en);
    show("stdout", b, n); show("stderr", e, en);
    {
        static const char want[] = "p1\r\np2\r\np3\r\np4\r\n5\r\n\r\np6\r\r\nw7\r\nb8\nt9\r\nk10\n";
        int wn = (int)sizeof want - 1;
        check(1, rc == 7 && n > wn && memcmp(b, want, (size_t)wn) == 0,
              "stdout: printf, puts, fputs, fwrite, fputc, putchar, fprintf and _write write '\\n' as \"\\r\\n\" "
              "(a \"\\r\\n\" as \"\\r\\r\\n\"); binary after _setmode(_O_BINARY); WriteFile as it is");
        char tail[200] = "";
        if (n > wn) { memcpy(tail, b + wn, (size_t)(n - wn < 199 ? n - wn : 199)); tail[n - wn < 199 ? n - wn : 199] = 0; }
        check(2, !strcmp(tail, "[3 0 0 3 4 3|4000 8000 4000|-1 9 1|-1 22 2|4000]"),
              "the answers: printf's count before the '\\r's, puts and fputs 0, _write 3 of 3; _setmode the mode it had "
              "(_O_TEXT 0x4000, _O_BINARY 0x8000), -1 with EBADF / EINVAL and the invalid-parameter handler called");
        check(3, SAME(e, en, "\r\ne1\r\ne2\r\n"), "stderr is in text mode too");
    }

    /* 4: files written: "w" text, "wb" binary, "wt" text; ftell the bytes */
    {
        FILE *f = fopen(at("f1.txt"), "w");
        long t[4];
        fputs("a\nb\n", f); t[0] = ftell(f);
        fprintf(f, "%d\n", 5); t[1] = ftell(f);
        size_t fw = fwrite("c\n", 1, 2, f); t[2] = ftell(f);
        fputc('\n', f); t[3] = ftell(f);
        fclose(f);
        printf("     ftell %ld %ld %ld %ld, fwrite %d\n", t[0], t[1], t[2], t[3], (int)fw);
        int n1 = rawread(at("f1.txt"), b, sizeof b);
        show("\"w\"", b, n1);
        int ok = SAME(b, n1, "a\r\nb\r\n5\r\nc\r\n\r\n") && t[0] == 6 && t[1] == 9 && t[2] == 12 && t[3] == 14 && fw == 2;
        f = fopen(at("f2.txt"), "wb"); fputs("a\nb\n", f); fclose(f);
        int n2 = rawread(at("f2.txt"), b, sizeof b); show("\"wb\"", b, n2);
        ok = ok && SAME(b, n2, "a\nb\n");
        f = fopen(at("f3.txt"), "wt"); fputs("a\nb\n", f); fclose(f);
        int n3 = rawread(at("f3.txt"), b, sizeof b); show("\"wt\"", b, n3);
        ok = ok && SAME(b, n3, "a\r\nb\r\n");
        check(4, ok, "fopen \"w\" and \"wt\" write text (ftell counts the '\\r's), \"wb\" binary");
    }

    /* 5: "a" appends (in text mode), "w+" reads back what it wrote */
    {
        FILE *f = fopen(at("f1.txt"), "a"); fputs("d\n", f); fclose(f);
        int n1 = rawread(at("f1.txt"), b, sizeof b); show("\"a\"", b, n1);
        f = fopen(at("f7.txt"), "w+"); fputs("x\ny\n", f); rewind(f);
        char ln[32] = ""; fgets(ln, 32, f); long p7 = ftell(f); fclose(f);
        printf("     \"w+\": \"%s\" then ftell %ld\n", ln[0] == 'x' ? "x\\n" : ln, p7);
        check(5, SAME(b, n1, "a\r\nb\r\n5\r\nc\r\n\r\nd\r\n") && !strcmp(ln, "x\n") && p7 == 3,
              "fopen \"a\" appends, \"w+\" reads its \"\\r\\n\" back as '\\n'");
    }

    /* 6: _fmode: 0 at first; _set_fmode refuses a mode that is not one;
     *    _O_BINARY makes "w" and _open binary, 't' still text */
    {
        int fm = -1; _get_fmode(&fm);
        int h0 = g_iph;
        errno = 0; int sf = _set_fmode(0x1234); int e3 = errno; int h3 = g_iph - h0;
        int fm2 = -1; _get_fmode(&fm2);
        _set_fmode(_O_BINARY);
        int fm3 = -1; _get_fmode(&fm3);
        FILE *f = fopen(at("f4.txt"), "w"); fputs("a\nb\n", f); fclose(f);
        int n4 = rawread(at("f4.txt"), b, sizeof b); show("_fmode binary, \"w\"", b, n4);
        int ok = SAME(b, n4, "a\nb\n");
        f = fopen(at("f5.txt"), "wt"); fputs("a\nb\n", f); fclose(f);
        int n5 = rawread(at("f5.txt"), b, sizeof b); show("_fmode binary, \"wt\"", b, n5);
        ok = ok && SAME(b, n5, "a\r\nb\r\n");
        int fd = _open(at("f8.txt"), _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE);
        int w8 = _write(fd, "o\n", 2); _close(fd);
        int n8 = rawread(at("f8.txt"), b, sizeof b); show("_fmode binary, _open", b, n8);
        ok = ok && w8 == 2 && SAME(b, n8, "o\n");
        _set_fmode(_O_TEXT);
        fd = _open(at("f9.txt"), _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE);
        int w9 = _write(fd, "o\n\n", 3); _close(fd);
        int n9 = rawread(at("f9.txt"), b, sizeof b); show("_open", b, n9);
        ok = ok && w9 == 3 && SAME(b, n9, "o\r\n\r\n");
        fd = _open(at("f9.txt"), _O_WRONLY | _O_TRUNC | _O_BINARY);
        int w10 = _write(fd, "o\n", 2); _close(fd);
        int n10 = rawread(at("f9.txt"), b, sizeof b);
        ok = ok && w10 == 2 && SAME(b, n10, "o\n");
        printf("     _get_fmode %x, _set_fmode(0x1234) %d errno %d handler %d, then %x, %x\n", fm, sf, e3, h3, fm2, fm3);
        check(6, ok && fm == 0 && sf == EINVAL && e3 == EINVAL && h3 == 1 && fm2 == 0 && fm3 == _O_BINARY,
              "_fmode: 0 at first, _set_fmode refuses 0x1234; _O_BINARY makes \"w\" and _open binary, \"wt\" text");
    }

    /* 7: files read in text mode: "\r\n" -> '\n', a lone '\r' kept, Ctrl-Z
     *    ends the input (and a seek starts it again) */
    {
        static const char r1[] = "l1\r\nl2\nl3\r\r\nl4\rX\x1Ayy\r\n";
        rawwrite(at("r1.txt"), r1, (int)sizeof r1 - 1);
        FILE *f = fopen(at("r1.txt"), "r");
        char line[64], all[200] = "";
        int lines = 0;
        while (fgets(line, sizeof line, f)) { strcat(all, "["); strcat(all, line); strcat(all, "]"); lines++; }
        int eof1 = feof(f) != 0;
        fseek(f, 0, SEEK_SET);
        int eof2 = feof(f) != 0;
        char again[64] = ""; fgets(again, sizeof again, f);
        fclose(f);
        show("fgets", (unsigned char *)all, (int)strlen(all));
        int ok = !strcmp(all, "[l1\n][l2\n][l3\r\n][l4\rX]") && eof1 && !eof2 && !strcmp(again, "l1\n");
        f = fopen(at("r1.txt"), "r"); n = (int)fread(b, 1, sizeof b, f); int eof3 = feof(f) != 0; fclose(f);
        show("fread", b, n);
        ok = ok && SAME(b, n, "l1\nl2\nl3\r\nl4\rX") && eof3;
        f = fopen(at("r1.txt"), "r");
        int c, k = 0, cs[32];
        while ((c = fgetc(f)) != EOF && k < 32) cs[k++] = c;
        int eof4 = feof(f) != 0;
        fclose(f);
        static const int want[] = { 108, 49, 10, 108, 50, 10, 108, 51, 13, 10, 108, 52, 13, 88 };
        int gok = k == 14 && eof4;
        for (int i = 0; gok && i < 14; i++) gok = cs[i] == want[i];
        printf("     fgetc %s\n", gok ? "108 49 10 108 50 10 108 51 13 10 108 52 13 88, EOF" : "differs");
        f = fopen(at("r1.txt"), "rb"); n = (int)fread(b, 1, sizeof b, f); fclose(f);
        check(7, ok && gok && n == (int)sizeof r1 - 1,
              "fgets, fread and fgetc read \"\\r\\n\" as '\\n', keep a lone '\\r', stop at Ctrl-Z (feof); fseek reads again; \"rb\" all of it");
    }

    /* 8: _read: a chunk's bytes read, then translated (so fewer); Ctrl-Z
     *    stays the end until a seek; a "\r" at a chunk's end looks ahead */
    {
        int fd = _open(at("r1.txt"), _O_RDONLY | _O_TEXT);
        int a1 = _read(fd, b, 5); int a2 = _read(fd, b + (a1 > 0 ? a1 : 0), 5);
        show("_read(5), _read(5)", b, (a1 > 0 ? a1 : 0) + (a2 > 0 ? a2 : 0));
        int ok = a1 == 4 && a2 == 5 && !memcmp(b, "l1\nl2\nl3\r", 9);
        _lseek(fd, 0, SEEK_SET);
        int a3 = _read(fd, b, 500); long p = _lseek(fd, 0, SEEK_CUR); int a4 = _read(fd, b + 600, 10); int eo = _eof(fd);
        printf("     _read %d, at %ld, then %d, _eof %d\n", a3, p, a4, eo);
        ok = ok && a3 == 14 && !memcmp(b, "l1\nl2\nl3\r\nl4\rX", 14) && p == 21 && a4 == 0 && eo == 1;
        _lseek(fd, 15, SEEK_SET);
        int a5 = _read(fd, b, 10); int x5 = a5 > 0 ? b[0] : -1;
        _close(fd);
        fd = _open(at("r1.txt"), _O_RDONLY | _O_BINARY);
        int a6 = _read(fd, b, 5); _close(fd);
        printf("     from 15: %d ('%c'); _O_BINARY: %d\n", a5, x5 > 32 ? x5 : '?', a6);
        ok = ok && a5 == 1 && x5 == 'X' && a6 == 5 && !memcmp(b, "l1\r\nl", 5);
        check(8, ok, "_read in text mode: 5 bytes read give 4 or 5; Ctrl-Z ends it (0 after, _eof), _lseek reads on; _O_BINARY as they are");
    }

    /* 9: stdin, a pipe, in text mode */
    {
        static const char in1[] = "s1\r\ns2\n\rs3\r\r\nlast\x1Atail\n";
        rc = run("in", in1, (int)sizeof in1 - 1, 0, b, &n, sizeof b, 0, 0); show("fgets", b, n);
        int ok = rc == 8 && SAME(b, n, "[s1\n][s2\n][\rs3\r\n][last]E");
        static const char in2[] = "a\r\nb";
        rc = run("in", in2, (int)sizeof in2 - 1, 2, b, &n, sizeof b, 0, 0); show("fgets, the '\\n' 150 ms after the '\\r'", b, n);
        ok = ok && rc == 8 && SAME(b, n, "[a\n][b]E");
        rc = run("inraw", in1, (int)sizeof in1 - 1, 0, b, &n, sizeof b, 0, 0); show("_read", b, n);
        ok = ok && rc == 9 && SAME(b, n, "15 0:s1\ns2\n\rs3\r\nlast");
        rc = run("inbin", in1, (int)sizeof in1 - 1, 0, b, &n, sizeof b, 0, 0); show("binary", b, n);
        ok = ok && rc == 10 && SAME(b, n, "4000:s1\r\ns2\n\rs3\r\r\nlast\x1Atail\n");
        check(9, ok, "stdin from a pipe: fgets and _read in text mode (a \"\\r\" and its '\\n' in two writes too), Ctrl-Z its end; binary after _setmode");
    }

    /* 10: what else reads: ungetc, fscanf's %c, ftell on a "\r\n" file */
    {
        FILE *f = fopen(at("r1.txt"), "r");
        int c = fgetc(f); int u = ungetc('Q', f); int c2 = fgetc(f); int c3 = fgetc(f);
        fclose(f);
        static const char r4[] = "1\r\n2\r\n";
        rawwrite(at("r4.txt"), r4, (int)sizeof r4 - 1);
        f = fopen(at("r4.txt"), "r"); int i1 = 0, i2 = 0; char ch = 0;
        int sc = fscanf(f, "%d%c%d", &i1, &ch, &i2);
        fclose(f);
        static const char r5[] = "ab\r\ncd\r\nef\r\n";
        rawwrite(at("r5.txt"), r5, (int)sizeof r5 - 1);
        f = fopen(at("r5.txt"), "r"); char ln[16];
        fgets(ln, sizeof ln, f); long q1 = ftell(f); fgets(ln, sizeof ln, f); long q2 = ftell(f);
        fseek(f, q1, SEEK_SET); fgets(ln, sizeof ln, f);
        fclose(f);
        static const char r6[] = "abc\r";
        rawwrite(at("r6.txt"), r6, 4);
        f = fopen(at("r6.txt"), "r"); int n6 = (int)fread(b, 1, sizeof b, f); fclose(f);
        static const char r7[] = "\x1A" "after";
        rawwrite(at("r7.txt"), r7, 6);
        f = fopen(at("r7.txt"), "r"); int n7 = (int)fread(b + 100, 1, 100, f); int eof7 = feof(f) != 0; fclose(f);
        printf("     ungetc %d %d %d %d; fscanf %d %d %d %d; ftell %ld %ld then \"%.2s\"; \"abc\\r\" %d; Ctrl-Z first %d %d\n",
               c, u, c2, c3, sc, i1, (int)ch, i2, q1, q2, ln, n6, n7, eof7);
        check(10, c == 'l' && u == 'Q' && c2 == 'Q' && c3 == '1' && sc == 3 && i1 == 1 && ch == '\n' && i2 == 2 &&
                  q1 == 4 && q2 == 8 && !strcmp(ln, "cd\n") && n6 == 4 && !memcmp(b, "abc\r", 4) && n7 == 0 && eof7,
              "ungetc, fscanf's %c ('\\n' of a \"\\r\\n\"), ftell on a \"\\r\\n\" file (fseek back to it), a last '\\r', Ctrl-Z first");
    }

    printf(first_fail ? "SOME FAILED (first %d)\n" : "ALL PASS\n", first_fail);
    return first_fail ? 100 + first_fail : 45;
}
