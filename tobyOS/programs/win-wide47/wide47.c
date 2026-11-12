/* win-wide47 -- Track C, W7p: the C runtime's wide text modes. A descriptor
 * or stream in _O_U16TEXT or _O_WTEXT writes what the wide functions give as
 * UTF-16LE, in _O_U8TEXT as UTF-8, each L'\n' as "\r\n"; it reads those back
 * as UTF-16 code units with "\r\n" as L'\n' and a Ctrl-Z as the end; the
 * narrow functions refuse it (EINVAL, through the invalid-parameter handler)
 * but fwrite and _write, which give their bytes as UTF-16 code units. A text
 * stream in the "C" locale writes a wide character as its byte when it has
 * one; a binary stream writes the code units as they are. fopen's "ccs="
 * (UTF-8, UTF-16LE, UNICODE) and _open's wide flags write a byte order mark
 * on an empty file and are decided by the one a file has. Each check is what
 * Windows 11 does (measured first: this program exits 49 there); the program
 * runs itself as children over pipes to see the bytes a child writes and to
 * give one the bytes it reads. Exit 49 iff all pass, else 100 + the first
 * check that failed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

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
static void rawwrite(const char *path, const void *b, int n) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    if (n) fwrite(b, 1, (size_t)n, f);
    fclose(f);
}

/* What a check saw, as text: compared with what Windows gave */
static char g_s[4096];
static int g_k;
static void s_reset(void) { g_k = 0; g_s[0] = 0; }
static void s_add(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_s + g_k, sizeof g_s - (size_t)g_k, fmt, ap);
    va_end(ap);
    if (n > 0 && g_k + n < (int)sizeof g_s) g_k += n;
}
static void s_hex(const unsigned char *b, int n) {
    s_add(" (%d):", n);
    for (int i = 0; i < n; i++) s_add(" %02X", b[i]);
}
static void s_file(const char *p) { unsigned char b[512]; int n = rawread(p, b, sizeof b); s_hex(b, n); }
static void s_units(FILE *f) {
    for (int i = 0; i < 16; i++) {
        errno = 0;
        wint_t c = fgetwc(f);
        if (c == WEOF) { s_add(" WEOF(%d,%d,%d)", errno, feof(f) != 0, ferror(f) != 0); return; }
        s_add(" %04X", (unsigned)c);
    }
}
static int modeof(int fd) { int m = _setmode(fd, _O_TEXT); _setmode(fd, m); return m; }
static int seen(int n, const char *want) {
    printf("     %d saw:%s\n", n, g_s);
    if (strcmp(g_s, want) != 0) { printf("     %d want:%s\n", n, want); return 0; }
    return 1;
}

/* Run this program as a child with `what` as its argument, its stdin, stdout
 * and stderr pipes of ours: give it `in`, collect what it writes. */
static int run(const char *what, const void *in, int inlen, unsigned char *out, int *outn,
               unsigned char *err, int *errn) {
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
    snprintf(cmd, sizeof cmd, "\"%s\" %s", g_self, what);
    if (!CreateProcessA(g_self, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) return -2;
    CloseHandle(ir); CloseHandle(ow); CloseHandle(ew);
    DWORD w;
    if (inlen) WriteFile(iw, in, (DWORD)inlen, &w, 0);
    CloseHandle(iw);
    int n = 0, en = 0; DWORD got;
    while (n < 1000 && ReadFile(orr, out + n, (DWORD)(1000 - n), &got, 0) && got) n += (int)got;
    while (en < 400 && ReadFile(er, err + en, (DWORD)(400 - en), &got, 0) && got) en += (int)got;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(orr); CloseHandle(er);
    *outn = n; *errn = en;
    return (int)code;
}

/* ---- the children ---- */
static int child(const char *m) {
    _set_invalid_parameter_handler(iph);
    if (!strcmp(m, "u16")) {                       /* stdout in _O_U16TEXT */
        printf("pre\n"); fflush(stdout);
        int prev = _setmode(1, _O_U16TEXT);
        int r1 = wprintf(L"h%lc\n", (wint_t)0xE9);
        int r2 = _putws(L"z");
        wint_t r3 = putwchar(L'q');
        int r4 = fputws(L"\x4e2d\n", stdout);
        errno = 0; int r5 = printf("narrow\n"); int e5 = errno;
        errno = 0; int r6 = puts("n2"); int e6 = errno;
        fflush(stdout);
        _setmode(1, _O_TEXT);
        printf("[%x %d %d %x %d %d/%d %d/%d iph %d]\n", prev, r1, r2, (unsigned)r3, r4, r5, e5, r6, e6, g_iph);
        return 7;
    }
    if (!strcmp(m, "u8")) {                        /* stdout and stderr in _O_U8TEXT */
        int prev = _setmode(1, _O_U8TEXT);
        int r1 = wprintf(L"h%lc%lc\n", (wint_t)0xE9, (wint_t)0x4e2d);
        int r2 = fwprintf(stderr, L"e\n");
        _setmode(2, _O_U8TEXT);
        int r3 = fwprintf(stderr, L"e%lc\n", (wint_t)0xE9);
        fflush(stdout);
        _setmode(1, _O_TEXT);
        printf("[%x %d %d %d iph %d]\n", prev, r1, r2, r3, g_iph);
        return 7;
    }
    if (!strcmp(m, "in")) {                        /* stdin in _O_U16TEXT */
        int prev = _setmode(0, _O_U16TEXT);
        wchar_t buf[32]; memset(buf, 0, sizeof buf);
        wchar_t *r = fgetws(buf, 32, stdin);
        printf("[%x %d:", prev, r != 0);
        for (int i = 0; buf[i]; i++) printf(" %04X", buf[i]);
        printf("]\n");
        return 7;
    }
    return 9;
}

/* 1-3: the wide functions' output in one mode, to a file */
static void wout(int mode, const char *name) {
    FILE *f = fopen(at(name), "wb");
    int prev = _setmode(_fileno(f), mode);
    errno = 0; int r1 = fputws(L"a\x00e9\n", f); int e1 = errno;
    errno = 0; wint_t r2 = fputwc(0x4e2d, f); int e2 = errno;
    errno = 0; wint_t r3 = fputwc(L'\n', f); int e3 = errno;
    errno = 0; int r4 = fwprintf(f, L"%d%ls%lc|%hs|\n", 7, L"\x00e9", (wint_t)0x4e2d, "n\xe9"); int e4 = errno;
    errno = 0; int r5 = fwprintf(f, L"%ls\n", L"\x4e2d" L"z"); int e5 = errno;
    fclose(f);
    s_add(" %d/%d %X/%d %X/%d %d/%d %d/%d prev %x", r1, e1, (unsigned)r2, e2, (unsigned)r3, e3, r4, e4, r5, e5, prev);
    s_file(at(name));
}

/* 5: fgetwc over a file of raw bytes in one mode */
static void win(const char *name, int mode) {
    FILE *f = fopen(at(name), "rb");
    _setmode(_fileno(f), mode);
    s_add(" |");
    s_units(f);
    fclose(f);
}
/* 6: a file of UTF-8 through _O_U8TEXT: fgetwc's units */
static void dec(const char *bytes, int n) {
    rawwrite(at("d.txt"), bytes, n);
    FILE *f = fopen(at("d.txt"), "rb");
    _setmode(_fileno(f), _O_U8TEXT);
    s_add(" |");
    s_units(f);
    fclose(f);
}
/* 9: one _read of up to 64 bytes and the descriptor's mode */
static void rd(int fd) {
    unsigned char b[64]; memset(b, 0xCC, sizeof b);
    errno = 0; int n = _read(fd, b, 64); int e = errno;
    s_add(" | %d/%d %x", n, e, modeof(fd));
    s_hex(b, n > 0 ? n : 0);
}

int main(int argc, char **argv) {
    GetModuleFileNameA(0, g_self, sizeof g_self);
    if (argc > 1) return child(argv[1]);
    _set_invalid_parameter_handler(iph);
    GetTempPathA(sizeof g_dir, g_dir);
    if (g_dir[0] && g_dir[strlen(g_dir) - 1] != '\\') strcat(g_dir, "\\");
    strcat(g_dir, "w7p");
    CreateDirectoryA(g_dir, NULL);
    strcat(g_dir, "\\");

    static const unsigned char u16[] = { 0xFF,0xFE, 'a',0, '\r',0, '\n',0, 0xE9,0, 0x2D,0x4E, '\r',0, 'b',0, 0x1A,0, 'c',0 };
    static const unsigned char u8[] = { 0xEF,0xBB,0xBF, 'a', '\r','\n', 0xC3,0xA9, 0xE4,0xB8,0xAD, '\r', 'b', 0x1A, 'c' };
    static const unsigned char an[] = { 'a', '\r','\n', 0xE9, 'b', 0x1A, 'c' };
    rawwrite(at("u16.txt"), u16, sizeof u16);
    rawwrite(at("u8.txt"), u8, sizeof u8);
    rawwrite(at("an.txt"), an, sizeof an);

    /* 1: _O_U16TEXT and _O_WTEXT */
    s_reset();
    wout(_O_U16TEXT, "c1a.txt");
    wout(_O_WTEXT, "c1b.txt");
    check(1, seen(1,
        " 0/0 4E2D/0 A/0 8/0 3/0 prev 8000 (40): 61 00 E9 00 0D 00 0A 00 2D 4E 0D 00 0A 00 37 00 E9 00 2D 4E 7C 00 6E 00 E9 00 7C 00 0D 00 0A 00 2D 4E 7A 00 0D 00 0A 00"
        " 0/0 4E2D/0 A/0 8/0 3/0 prev 8000 (40): 61 00 E9 00 0D 00 0A 00 2D 4E 0D 00 0A 00 37 00 E9 00 2D 4E 7C 00 6E 00 E9 00 7C 00 0D 00 0A 00 2D 4E 7A 00 0D 00 0A 00"),
        "_O_U16TEXT and _O_WTEXT: fputws, fputwc and fwprintf (%ls %lc %hs) write UTF-16LE, L'\\n' as \"\\r\\n\"; "
        "fputws answers 0, fputwc the character, fwprintf the characters");

    /* 2: _O_U8TEXT, and its surrogate pairs */
    s_reset();
    wout(_O_U8TEXT, "c2.txt");
    {
        FILE *f = fopen(at("c2s.txt"), "wb");
        _setmode(_fileno(f), _O_U8TEXT);
        fputws(L"\xD83D\xDE00|", f);
        fputws(L"\xD800x|", f);
        fputwc(0xD83D, f); fputwc(0xDE00, f);
        int r = fwprintf(f, L"|%lc%lc|", (wint_t)0xD83D, (wint_t)0xDE00);
        fputws(L"x\xD83D", f); fflush(f); fputws(L"\xDE00y", f);
        fclose(f);
        s_add(" | %d", r);
        s_file(at("c2s.txt"));
    }
    check(2, seen(2,
        " 0/0 4E2D/0 A/0 8/0 3/0 prev 8000 (29): 61 C3 A9 0D 0A E4 B8 AD 0D 0A 37 C3 A9 E4 B8 AD 7C 6E C3 A9 7C 0D 0A E4 B8 AD 7A 0D 0A"
        " | 4 (28): F0 9F 98 80 7C EF BF BD 78 7C F0 9F 98 80 7C F0 9F 98 80 7C 78 EF BF BD EF BF BD 79"),
        "_O_U8TEXT writes UTF-8, L'\\n' as \"\\r\\n\"; a surrogate pair is one character even over two calls "
        "while the stream holds it; a lone surrogate, and one a flush split, U+FFFD");

    /* 3: a text stream in the "C" locale, and a binary one */
    s_reset();
    wout(_O_TEXT, "c3a.txt");
    wout(_O_BINARY, "c3b.txt");
    {
        FILE *f = fopen(at("c3c.txt"), "w");
        errno = 0; wint_t a = fputwc(0xE9, f); int ea = errno;
        errno = 0; wint_t b = fputwc(0x100, f); int eb = errno;
        errno = 0; int c = fputws(L"x\x0100y", f); int ec = errno;
        errno = 0; int d = fwprintf(f, L"p%lcq", (wint_t)0x100); int ed = errno;
        int fe = ferror(f);
        fclose(f);
        s_add(" | %X/%d %X/%d %d/%d %d/%d %d", (unsigned)a, ea, (unsigned)b, eb, c, ec, d, ed, fe);
        s_file(at("c3c.txt"));
    }
    check(3, seen(3,
        " 0/0 FFFF/42 A/0 8/0 3/0 prev 8000 (19): 61 E9 0D 0A 0D 0A 37 E9 3F 7C 6E E9 7C 0D 0A 3F 7A 0D 0A"
        " 0/0 4E2D/0 A/0 8/0 3/0 prev 8000 (32): 61 00 E9 00 0A 00 2D 4E 0A 00 37 00 E9 00 2D 4E 7C 00 6E 00 E9 00 7C 00 0A 00 2D 4E 7A 00 0A 00"
        " | E9/0 FFFF/42 -1/42 3/0 0 (5): E9 78 70 3F 71"),
        "a text stream in the \"C\" locale: a character to 0xFF its byte, past it fputwc and fputws fail (EILSEQ) "
        "and fwprintf writes '?'; a binary stream: the code units as they are");

    /* 4: the narrow functions on a wide stream */
    s_reset();
    {
        const int wm[2] = { _O_U16TEXT, _O_U8TEXT };
        for (int i = 0; i < 2; i++) {
            FILE *f = fopen(at("c4.txt"), "wb");
            int fd = _fileno(f);
            _setmode(fd, wm[i]);
            g_iph = 0;
            errno = 0; int a = fprintf(f, "x\n"); int ea = errno;
            errno = 0; int b = fputs("y\n", f); int eb = errno;
            errno = 0; int c = fputc('z', f); int ec = errno;
            errno = 0; size_t d = fwrite("w\n", 1, 2, f); int ed = errno;
            fflush(f);
            errno = 0; int e = _write(fd, "q\n", 2); int ee = errno;
            errno = 0; int g = _write(fd, "odd", 3); int eg = errno;
            errno = 0; int h = _write(fd, L"r\x00e9\n", 6); int eh = errno;
            fclose(f);
            s_add(" | %d/%d %d/%d %d/%d %d/%d %d/%d %d/%d %d/%d iph %d", a, ea, b, eb, c, ec, (int)d, ed, e, ee, g, eg, h, eh, g_iph);
            s_file(at("c4.txt"));
        }
    }
    check(4, seen(4,
        " | -1/22 -1/22 -1/22 2/0 2/0 -1/22 6/0 iph 4 (12): 77 0A 71 0A 72 00 E9 00 0D 00 0A 00"
        " | -1/22 -1/22 -1/22 2/0 2/0 -1/22 6/0 iph 4 (11): E0 A9 B7 E0 A9 B1 72 C3 A9 0D 0A"),
        "fprintf, fputs and fputc on a wide stream: -1, EINVAL, the handler called; fwrite and _write give their bytes "
        "as UTF-16 code units (an odd count EINVAL)");

    /* 5: reading: fgetwc in each mode, fgetws, ungetwc */
    s_reset();
    win("u16.txt", _O_U16TEXT);
    win("u8.txt", _O_U8TEXT);
    win("an.txt", _O_TEXT);
    win("u16.txt", _O_BINARY);
    win("an.txt", _O_U8TEXT);
    win("u8.txt", _O_U16TEXT);
    {
        FILE *f = fopen(at("u16.txt"), "rb");
        _setmode(_fileno(f), _O_U16TEXT);
        wchar_t b[16];
        for (int k = 0; k < 2; k++) {
            memset(b, 0, sizeof b);
            wchar_t *r = fgetws(b, 16, f);
            s_add(" | %d:", r != 0);
            for (int i = 0; b[i]; i++) s_add(" %04X", b[i]);
        }
        fclose(f);
        f = fopen(at("u16.txt"), "rb");
        _setmode(_fileno(f), _O_U16TEXT);
        wint_t x = fgetwc(f), y = ungetwc(0x4e2d, f), z = fgetwc(f), w = fgetwc(f);
        s_add(" | %X %X %X %X", (unsigned)x, (unsigned)y, (unsigned)z, (unsigned)w);
        fclose(f);
    }
    check(5, seen(5,
        " | FEFF 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0)"
        " | FEFF 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0)"
        " | 0061 000A 00E9 0062 WEOF(0,1,0)"
        " | FEFF 0061 000D 000A 00E9 4E2D 000D 0062 001A 0063 WEOF(0,1,0)"
        " | 0061 000A FFFD 0062 WEOF(0,1,0)"
        " | BBEF 61BF 0A0D A9C3 B8E4 0DAD 1A62 WEOF(0,1,0)"
        " | 1: FEFF 0061 000A | 1: 00E9 4E2D 000D 0062 | FEFF 4E2D 4E2D 61"),
        "fgetwc: UTF-16LE and UTF-8 read as code units, \"\\r\\n\" as L'\\n', a Ctrl-Z the end, the byte order mark "
        "kept; a text stream's bytes in the \"C\" locale; a binary stream's units as they are; fgetws to L'\\n'; ungetwc");

    /* 6: UTF-8 read: what is not UTF-8 is U+FFFD, as Windows replaces it */
    s_reset();
#define DEC(s) dec(s, (int)sizeof(s) - 1)
    DEC("\xF0\x9F\x98\x80" "a");
    DEC("\xED\xA0\x80" "a");
    DEC("\xE0\x80\x80" "a");
    DEC("\xF5\x80\x80\x80" "a");
    DEC("\xE4\xB8" "A");
    DEC("\xC0\xAF" "a");
    DEC("\x80\xBF" "a");
    DEC("\xF0\x9F" "A");
    DEC("\xC3\r\n");
    DEC("a\r\x1A" "b");
    DEC("a\xE4\xB8");
    {
        static const unsigned char s8[] = { 0xF0,0x9F,0x98,0x80, 'a', 0xC0,0xAF, 'b', 0xE4,0xB8, 'c', 0xED,0xA0,0x80, 'd', 0xC3 };
        rawwrite(at("s8.txt"), s8, sizeof s8);
        int fd = _open(at("s8.txt"), _O_RDONLY | _O_BINARY);
        _setmode(fd, _O_U8TEXT);
        rd(fd);
        errno = 0; unsigned char b[8]; int r2 = _read(fd, b, 8);
        s_add(" | %d/%d", r2, errno);
        _close(fd);
    }
    check(6, seen(6,
        " | D83D DE00 0061 WEOF(0,1,0) | FFFD FFFD 0061 WEOF(0,1,0) | FFFD FFFD 0061 WEOF(0,1,0)"
        " | FFFD FFFD FFFD FFFD 0061 WEOF(0,1,0) | FFFD 0041 WEOF(0,1,0) | FFFD FFFD 0061 WEOF(0,1,0)"
        " | FFFD FFFD 0061 WEOF(0,1,0) | FFFD 0041 WEOF(0,1,0) | FFFD 000A WEOF(0,1,0) | 0061 000D WEOF(0,1,0)"
        " | 0061 WEOF(22,0,1)"
        " | 22/0 40000 (22): 3D D8 00 DE 61 00 FD FF FD FF 62 00 FD FF 63 00 FD FF FD FF 64 00 | -1/22"),
        "_O_U8TEXT reads a 4-byte character as a surrogate pair and what is not UTF-8 as U+FFFD, as Windows counts them; "
        "a character the file cuts short ends with EINVAL");

    /* 7: fopen's ccs= when writing: the byte order mark */
    s_reset();
    {
        const char *wm[] = { "w, ccs=UTF-8", "w, ccs=UTF-16LE", "w, ccs=UNICODE", "w+, ccs=UTF-8", "wb, ccs=UTF-8" };
        for (int i = 0; i < 5; i++) {
            FILE *f = fopen(at("c7.txt"), wm[i]);
            if (!f) { s_add(" | NULL"); continue; }
            int m = modeof(_fileno(f));
            fputws(L"\x00e9\n", f);
            fclose(f);
            s_add(" | %x", m);
            s_file(at("c7.txt"));
        }
        rawwrite(at("c7a.txt"), "x", 1);
        FILE *f = fopen(at("c7a.txt"), "a, ccs=UTF-8"); fputws(L"\x00e9\n", f); fclose(f);
        s_add(" |"); s_file(at("c7a.txt"));
        rawwrite(at("c7a.txt"), "", 0);
        f = fopen(at("c7a.txt"), "a, ccs=UTF-8"); fputws(L"\x00e9\n", f); fclose(f);
        s_add(" |"); s_file(at("c7a.txt"));
        rawwrite(at("c7a.txt"), "\xEF\xBB\xBF" "x", 4);
        f = fopen(at("c7a.txt"), "a, ccs=UTF-16LE"); int m = modeof(_fileno(f)); fputws(L"\x00e9\n", f); fclose(f);
        s_add(" | %x", m); s_file(at("c7a.txt"));
        wchar_t wp[MAX_PATH];
        swprintf(wp, MAX_PATH, L"%hs%hs", g_dir, "c7w.txt");
        f = _wfopen(wp, L"w, ccs=UTF-8");
        if (f) { fputws(L"\x4e2d", f); fclose(f); }
        s_add(" |"); s_file(at("c7w.txt"));
    }
    check(7, seen(7,
        " | 40000 (7): EF BB BF C3 A9 0D 0A | 10000 (8): FF FE E9 00 0D 00 0A 00 | 10000 (8): FF FE E9 00 0D 00 0A 00"
        " | 40000 (7): EF BB BF C3 A9 0D 0A | 8000 (4): E9 00 0A 00"
        " | (5): 78 C3 A9 0D 0A | (7): EF BB BF C3 A9 0D 0A | 40000 (8): EF BB BF 78 C3 A9 0D 0A"
        " | (6): EF BB BF E4 B8 AD"),
        "fopen's ccs=: a new file's byte order mark (UTF-8 _O_U8TEXT, UTF-16LE and UNICODE _O_WTEXT); 'b' ignores it; "
        "appending to a file that has text, none, and the encoding is its mark's; _wfopen the same");

    /* 8: fopen's ccs= when reading: the mark decides */
    s_reset();
    {
        const char *rm[] = { "r, ccs=UNICODE", "r, ccs=UTF-8", "r, ccs=UTF-16LE", "r, ccs=utf-8", "r,ccs=UTF-8", "r+, ccs=UTF-8" };
        const char *fl[] = { "u16.txt", "u8.txt", "an.txt" };
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 3; j++) {
                if (i >= 3 && j < 2) continue;
                FILE *f = fopen(at(fl[j]), rm[i]);
                if (!f) { s_add(" | NULL"); continue; }
                s_add(" | %x %ld:", modeof(_fileno(f)), ftell(f));
                s_units(f);
                fclose(f);
            }
        g_iph = 0; errno = 0;
        FILE *f = fopen(at("an.txt"), "r, ccs=UTF-16BE");
        s_add(" | %d %d %d", f != 0, errno, g_iph);
        if (f) fclose(f);
    }
    check(8, seen(8,
        " | 10000 2: 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0) | 40000 3: 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0)"
        " | 4000 0: 0061 000A 00E9 0062 WEOF(0,1,0)"
        " | 10000 2: 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0) | 40000 3: 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0)"
        " | 40000 0: 0061 000A FFFD 0062 WEOF(0,1,0)"
        " | 10000 2: 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0) | 40000 3: 0061 000A 00E9 4E2D 000D 0062 WEOF(0,1,0)"
        " | 10000 0: 0D61 E90A 1A62 WEOF(0,1,0)"
        " | 40000 0: 0061 000A FFFD 0062 WEOF(0,1,0) | 40000 0: 0061 000A FFFD 0062 WEOF(0,1,0)"
        " | 40000 0: 0061 000A FFFD 0062 WEOF(0,1,0)"
        " | 0 22 1"),
        "fopen's ccs= when reading: the file's byte order mark decides and is skipped; with none UNICODE is the "
        "\"C\" locale's text, UTF-8 and UTF-16LE their own; the name in any case; UTF-16BE: NULL, EINVAL, the handler");

    /* 9: _setmode's answers; _read in the wide modes; _open's wide flags */
    s_reset();
    {
        FILE *f = fopen(at("c9.txt"), "wb"); int fd = _fileno(f);
        int a = _setmode(fd, _O_U8TEXT), b = _setmode(fd, _O_U16TEXT), c = _setmode(fd, _O_WTEXT), d = _setmode(fd, _O_TEXT);
        g_iph = 0; errno = 0; int e = _setmode(fd, _O_U8TEXT | _O_U16TEXT); int ee = errno;
        s_add(" %x %x %x %x %d/%d/%d", a, b, c, d, e, ee, g_iph);
        fclose(f);

        fd = _open(at("u16.txt"), _O_RDONLY | _O_BINARY);
        _setmode(fd, _O_U16TEXT);
        unsigned char k[64];
        errno = 0; int n = _read(fd, k, 8); s_add(" | %d/%d", n, errno); s_hex(k, n > 0 ? n : 0);
        g_iph = 0; errno = 0; n = _read(fd, k, 3); s_add(" | %d/%d/%d", n, errno, g_iph);
        rd(fd);
        _close(fd);
        fd = _open(at("u8.txt"), _O_RDONLY | _O_BINARY);
        _setmode(fd, _O_U8TEXT);
        rd(fd);
        _close(fd);
        fd = _open(at("u8.txt"), _O_RDONLY | _O_U8TEXT); rd(fd); _close(fd);
        fd = _open(at("u16.txt"), _O_RDONLY | _O_U8TEXT); rd(fd); _close(fd);
        fd = _open(at("u16.txt"), _O_RDONLY | _O_WTEXT); rd(fd); _close(fd);
        fd = _open(at("an.txt"), _O_RDONLY | _O_WTEXT); rd(fd); _close(fd);
        fd = _open(at("an.txt"), _O_RDONLY | _O_U16TEXT); rd(fd); _close(fd);
        const int cm[3] = { _O_U8TEXT, _O_U16TEXT, _O_WTEXT };
        for (int i = 0; i < 3; i++) {
            fd = _open(at("c9w.txt"), _O_WRONLY | _O_CREAT | _O_TRUNC | cm[i], _S_IREAD | _S_IWRITE);
            n = _write(fd, L"\x00e9\n", 4);
            _close(fd);
            s_add(" | %d", n); s_file(at("c9w.txt"));
        }
        rawwrite(at("c9w.txt"), "\xFF\xFE" "x\0", 4);
        fd = _open(at("c9w.txt"), _O_WRONLY | _O_APPEND | _O_U8TEXT);
        s_add(" | %x %d", modeof(fd), _write(fd, L"\x00e9", 2)); _close(fd); s_file(at("c9w.txt"));
        rawwrite(at("c9w.txt"), "", 0);
        fd = _open(at("c9w.txt"), _O_WRONLY | _O_U16TEXT);
        s_add(" | %d", _write(fd, L"\x00e9", 2)); _close(fd); s_file(at("c9w.txt"));
        rawwrite(at("c9w.txt"), "xyz", 3);
        fd = _open(at("c9w.txt"), _O_WRONLY | _O_U8TEXT);
        s_add(" | %d", _write(fd, L"\x00e9", 2)); _close(fd); s_file(at("c9w.txt"));
    }
    check(9, seen(9,
        " 8000 40000 10000 10000 -1/22/1"
        " | 6/0 (6): FF FE 61 00 0A 00 | -1/22/1 | 8/0 10000 (8): E9 00 2D 4E 0D 00 62 00"
        " | 14/0 40000 (14): FF FE 61 00 0A 00 E9 00 2D 4E 0D 00 62 00"
        " | 12/0 40000 (12): 61 00 0A 00 E9 00 2D 4E 0D 00 62 00"
        " | 12/0 10000 (12): 61 00 0A 00 E9 00 2D 4E 0D 00 62 00"
        " | 12/0 10000 (12): 61 00 0A 00 E9 00 2D 4E 0D 00 62 00"
        " | 4/0 4000 (4): 61 0A E9 62"
        " | 6/0 10000 (6): 61 0D 0A E9 62 1A"
        " | 4 (7): EF BB BF C3 A9 0D 0A | 4 (8): FF FE E9 00 0D 00 0A 00 | 4 (8): FF FE E9 00 0D 00 0A 00"
        " | 10000 2 (6): FF FE 78 00 E9 00 | 2 (4): FF FE E9 00 | 2 (3): C3 A9 7A"),
        "_setmode answers _O_U16TEXT as _O_WTEXT; _read in a wide mode: \"\\r\\n\" as L'\\n', UTF-8 as UTF-16LE, an odd "
        "count EINVAL; _open's wide flags: the mark decides and is skipped, with none _O_WTEXT is text; a mark written "
        "on an empty file, none on one with text");

    /* 10: a child's stdout, stderr and stdin in the wide modes, pipes */
    s_reset();
    {
        unsigned char o[1000], e[400]; int on, en;
        int rc = run("u16", 0, 0, o, &on, e, &en);
        s_add(" %d", rc); s_hex(o, on); s_hex(e, en);
        rc = run("u8", 0, 0, o, &on, e, &en);
        s_add(" | %d", rc); s_hex(o, on); s_hex(e, en);
        static const unsigned char in[] = { 'a',0, 0xE9,0, '\r',0, '\n',0, 'b',0 };
        rc = run("in", in, sizeof in, o, &on, e, &en);
        s_add(" | %d %.*s", rc, on, (const char *)o);
    }
    check(10, seen(10,
        " 7 (62): 70 72 65 0D 0A 68 00 E9 00 0D 00 0A 00 7A 00 0D 00 0A 00 71 00 2D 4E 0D 00 0A 00 5B 34 30 30 30 20 33 20 30 20 37 31 20 30 20 2D 31 2F 32 32 20 2D 31 2F 32 32 20 69 70 68 20 32 5D 0D 0A (0):"
        " | 7 (28): 68 C3 A9 E4 B8 AD 0D 0A 5B 34 30 30 30 20 34 20 32 20 33 20 69 70 68 20 30 5D 0D 0A (8): 65 0D 0A 65 C3 A9 0D 0A"
        " | 7 [4000 1: 0061 00E9 000A]\r\n"),
        "a child's stdout in _O_U16TEXT (wprintf, _putws, putwchar, fputws; printf and puts refused) and in "
        "_O_U8TEXT, its stderr in text then _O_U8TEXT, its stdin in _O_U16TEXT (fgetws), over pipes");

    /* 11: the narrow reads on a wide stream; fread */
    s_reset();
    {
        FILE *f = fopen(at("u16.txt"), "rb"); _setmode(_fileno(f), _O_U16TEXT);
        g_iph = 0;
        char buf[16];
        errno = 0; int a = fgetc(f); int ea = errno;
        errno = 0; char *b = fgets(buf, 16, f); int eb = errno;
        int x = 0; errno = 0; int d = fscanf(f, "%d", &x); int ed = errno;
        errno = 0; int g = getc(f); int eg = errno;
        errno = 0; int u = ungetc('z', f); int eu = errno;
        s_add(" %d/%d %d/%d %d/%d %d/%d %d/%d iph %d", a, ea, b != 0, eb, d, ed, g, eg, u, eu, g_iph);
        errno = 0; size_t c = fread(buf, 1, 3, f);
        s_add(" | %d/%d", (int)c, errno); s_hex((unsigned char *)buf, (int)c);
        c = fread(buf, 1, 16, f);
        s_add(" | %d", (int)c); s_hex((unsigned char *)buf, (int)c);
        fclose(f);
        f = fopen(at("u8.txt"), "rb"); _setmode(_fileno(f), _O_U8TEXT);
        c = fread(buf, 1, 16, f);
        s_add(" | %d", (int)c); s_hex((unsigned char *)buf, (int)c);
        fclose(f);
    }
    check(11, seen(11,
        " -1/22 0/22 -1/22 -1/22 -1/22 iph 5"
        " | 3/0 (3): FF FE 61 | 11 (11): 00 0A 00 E9 00 2D 4E 0D 00 62 00"
        " | 14 (14): FF FE 61 00 0A 00 E9 00 2D 4E 0D 00 62 00"),
        "fgetc, fgets, fscanf, getc and ungetc on a wide stream: EOF or NULL, EINVAL, the handler called; fread reads "
        "the code units _read gives, an odd count keeping the byte after for the next");

    /* 12: swprintf and snprintf past ASCII */
    s_reset();
    {
        wchar_t w[32]; memset(w, 0, sizeof w);
        int r = swprintf(w, 32, L"%ls|%lc|%hs|\x4e2d", L"\x00e9\x4e2d", (wint_t)0x4e2d, "\xe9");
        s_add(" %d:", r);
        for (int i = 0; w[i]; i++) s_add(" %04X", w[i]);
        char nb[32]; memset(nb, 0, sizeof nb);
        r = snprintf(nb, sizeof nb, "%ls|%lc|", L"\x00e9z", (wint_t)0xe9);
        s_add(" | %d", r); s_hex((unsigned char *)nb, r > 0 ? r : 0);
        errno = 0;
        r = snprintf(nb, sizeof nb, "%ls|", L"a\x4e2d" L"b");
        s_add(" | %d %d", r, errno);
    }
    check(12, seen(12,
        " 8: 00E9 4E2D 007C 4E2D 007C 00E9 007C 4E2D | 5 (5): E9 7A 7C E9 7C | -1 42"),
        "swprintf writes each character (a narrow %hs in the \"C\" locale); snprintf's %ls and %lc a character's "
        "byte to 0xFF, and past it -1, EILSEQ");

    if (first_fail) { printf("FAILED (first: %d)\n", first_fail); return 100 + first_fail; }
    printf("ALL PASS\n");
    return 49;
}
