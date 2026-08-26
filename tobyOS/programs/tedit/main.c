/* tedit -- a native modeless editor for tobyOS: nano's shape, with the
 * things nano makes you live without.
 *
 * WHY A SECOND NATIVE EDITOR: /bin/tvi covers the vi idiom (modal, POSIX
 * vi(1) conformant). This covers the other one -- modeless, discoverable,
 * shortcut bar at the bottom -- because that is the idiom most people
 * actually reach for, and "native userland" should mean the everyday tool
 * is ours too, not only the standards-mandated one.
 *
 * NANO-FAITHFUL where it costs nothing: the bindings below are nano's, so
 * muscle memory transfers. ^X exit, ^O write out, ^W where-is, ^\ replace,
 * ^K cut, ^U uncut, ^R read file, ^G help, ^C position, ^_ go-to-line,
 * ^A/^E line ends, ^Y/^V paging, ^6 mark, M-U/M-E undo/redo.
 *
 * WHERE IT IMPROVES ON NANO, deliberately and each for a reason:
 *
 *   1. MULTI-LEVEL UNDO WITH TYPING COALESCING. A run of typed characters
 *      collapses into ONE undo step, so ^Z-equivalent does what a human
 *      means by "undo that word" rather than removing one letter.
 *   2. REAL REGEX, INCLUDING BACKREFERENCES IN THE REPLACEMENT. \1..\9
 *      and & work in Replace. nano cannot do this without regex mode and
 *      still has no backrefs in the replacement.
 *   3. INCREMENTAL SEARCH WITH A LIVE MATCH COUNT -- "match 3 of 7"
 *      updates as you type. nano tells you nothing until you commit.
 *   4. REGION COPY (M-6), not just cut. nano's mark can only cut-then-paste
 *      to duplicate, which is destructive if you get it wrong.
 *   5. GO-TO LINE:COLUMN, and a bracket-match jump (M-]).
 *   6. A LINE-NUMBER GUTTER you can toggle (M-#).
 *
 * The gate (logs/edit.sh) drives it over a real pty and compares the
 * RESULTING FILE BYTES, exactly as it does for tvi.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <regex.h>
#include <ctype.h>
#include <time.h>

#define TIOCGWINSZ 0x5413u
#define FIONREAD   0x541BUL
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define UNDO_MAX   96
#define PROMPT_MAX 512

/* ---------------- buffer ---------------- */

struct line { char *s; int len, cap; };
static struct line *B;
static int  B_n, B_cap;
static char g_file[512];
static int  g_dirty, g_nofile;

static int  cx, cy, top, want_col;
static int  rows = 24, cols = 80;

static int  opt_number, opt_autoindent = 1, opt_tabstop = 8, opt_regex = 1;
static int  opt_icase;

static char g_msg[PROMPT_MAX];
static struct termios g_saved_tio;
static int  g_raw;

/* mark / selection */
static int  mark_set, mark_y, mark_x;

/* cut buffer; consecutive ^K accumulate into it, as nano does */
static char *CUT; static int CUT_len, CUT_linewise, g_last_was_cut;

/* search state */
static char g_pat[256], g_rep[256];
static int  g_search_back;

/* ---------------- helpers ---------------- */

static void die(const char *m) {
    write(1, "\033[2J\033[H", 7);
    fprintf(stderr, "tedit: %s\n", m);
    exit(1);
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}
static void msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap); va_end(ap);
}

static void line_reserve(struct line *l, int n) {
    if (l->cap >= n) return;
    int c = l->cap ? l->cap : 32;
    while (c < n) c *= 2;
    l->s = xrealloc(l->s, (size_t)c); l->cap = c;
}
static void buf_reserve(int n) {
    if (B_cap >= n) return;
    int c = B_cap ? B_cap : 256;
    while (c < n) c *= 2;
    B = xrealloc(B, (size_t)c * sizeof *B);
    memset(B + B_cap, 0, (size_t)(c - B_cap) * sizeof *B);
    B_cap = c;
}
static void line_set(int i, const char *s, int len) {
    line_reserve(&B[i], len + 1);
    memcpy(B[i].s, s, (size_t)len); B[i].s[len] = '\0'; B[i].len = len;
}
static void line_insert(int at, const char *s, int len) {
    buf_reserve(B_n + 1);
    memmove(B + at + 1, B + at, (size_t)(B_n - at) * sizeof *B);
    memset(B + at, 0, sizeof *B);
    B_n++; line_set(at, s, len);
}
static void line_delete(int at) {
    free(B[at].s);
    memmove(B + at, B + at + 1, (size_t)(B_n - at - 1) * sizeof *B);
    B_n--; memset(B + B_n, 0, sizeof *B);
    if (B_n == 0) { buf_reserve(1); B_n = 1; line_set(0, "", 0); }
}

/* ---------------- undo, with typing coalescing ---------------- */

struct snap { struct line *l; int n, cx, cy; };
static struct snap U[UNDO_MAX], R[UNDO_MAX];
static int U_head, U_count, R_head, R_count;
/* An open "typing run": while the next inserted character lands exactly
 * where the last one left off, it belongs to the SAME undo step. This is
 * what makes undo remove a word instead of a letter. */
static int run_open, run_y, run_x;

static void snap_free(struct snap *s) {
    if (!s->l) return;
    for (int i = 0; i < s->n; i++) free(s->l[i].s);
    free(s->l); s->l = NULL; s->n = 0;
}
static void snap_take(struct snap *s) {
    snap_free(s);
    int n = B_n ? B_n : 1;
    s->l = xrealloc(NULL, (size_t)n * sizeof *s->l);
    memset(s->l, 0, (size_t)n * sizeof *s->l);
    for (int i = 0; i < B_n; i++) {
        s->l[i].len = B[i].len; s->l[i].cap = B[i].len + 1;
        s->l[i].s = xrealloc(NULL, (size_t)s->l[i].cap);
        memcpy(s->l[i].s, B[i].s, (size_t)B[i].len + 1);
    }
    s->n = B_n; s->cx = cx; s->cy = cy;
}
static void snap_restore(struct snap *s) {
    for (int i = 0; i < B_n; i++) { free(B[i].s); B[i].s = NULL; B[i].cap = 0; }
    buf_reserve(s->n ? s->n : 1);
    for (int i = 0; i < s->n; i++) {
        B[i].s = NULL; B[i].cap = 0; B[i].len = 0;
        line_set(i, s->l[i].s, s->l[i].len);
    }
    B_n = s->n ? s->n : 1;
    if (!s->n) line_set(0, "", 0);
    cy = s->cy; cx = s->cx;
    if (cy >= B_n) cy = B_n - 1;
    if (cy < 0) cy = 0;
    if (cx > B[cy].len) cx = B[cy].len;
}

static void redo_clear(void) {
    while (R_count) { R_head = (R_head - 1 + UNDO_MAX) % UNDO_MAX;
                      snap_free(&R[R_head]); R_count--; }
}
static void undo_push(void) {
    snap_take(&U[U_head]);
    U_head = (U_head + 1) % UNDO_MAX;
    if (U_count < UNDO_MAX) U_count++;
    redo_clear();
}
/* Any mutation that is NOT a continuation of a typing run. */
static void edit_other(void) { undo_push(); run_open = 0; }
/* Call BEFORE inserting one typed character. */
static void edit_typed(void) {
    if (!(run_open && cy == run_y && cx == run_x)) { undo_push(); run_open = 1; }
}
static void break_run(void) { run_open = 0; }

static void do_undo(void) {
    if (!U_count) { msg("Nothing to undo"); return; }
    snap_take(&R[R_head]); R_head = (R_head + 1) % UNDO_MAX;
    if (R_count < UNDO_MAX) R_count++;
    U_head = (U_head - 1 + UNDO_MAX) % UNDO_MAX; U_count--;
    snap_restore(&U[U_head]);
    g_dirty = 1; run_open = 0;
    msg("Undid an action");
}
static void do_redo(void) {
    if (!R_count) { msg("Nothing to redo"); return; }
    snap_take(&U[U_head]); U_head = (U_head + 1) % UNDO_MAX;
    if (U_count < UNDO_MAX) U_count++;
    R_head = (R_head - 1 + UNDO_MAX) % UNDO_MAX; R_count--;
    snap_restore(&R[R_head]);
    g_dirty = 1; run_open = 0;
    msg("Redid an action");
}

/* ---------------- file ---------------- */

static void buf_empty(void) {
    for (int i = 0; i < B_n; i++) free(B[i].s);
    B_n = 0; buf_reserve(1); memset(B, 0, sizeof *B);
    B_n = 1; line_set(0, "", 0);
}
static int file_load(const char *path) {
    buf_empty();
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    B_n = 0;
    int c, cap = 0, len = 0; char *cur = NULL;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            buf_reserve(B_n + 1); memset(B + B_n, 0, sizeof *B); B_n++;
            line_set(B_n - 1, cur ? cur : "", len); len = 0;
        } else {
            if (len + 1 >= cap) { cap = cap ? cap * 2 : 128; cur = xrealloc(cur, (size_t)cap); }
            cur[len++] = (char)c; cur[len] = '\0';
        }
    }
    if (len > 0) { buf_reserve(B_n + 1); memset(B + B_n, 0, sizeof *B); B_n++;
                   line_set(B_n - 1, cur, len); }
    free(cur); fclose(f);
    if (B_n == 0) { buf_reserve(1); memset(B, 0, sizeof *B); B_n = 1; line_set(0, "", 0); }
    return 0;
}
static long file_bytes(void) {
    long t = 0; for (int i = 0; i < B_n; i++) t += B[i].len + 1; return t;
}
static int file_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (int i = 0; i < B_n; i++) {
        if (B[i].len && fwrite(B[i].s, 1, (size_t)B[i].len, f) != (size_t)B[i].len) {
            fclose(f); return -1; }
        if (fputc('\n', f) == EOF) { fclose(f); return -1; }
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* ---------------- terminal ---------------- */

static void raw_off(void) {
    if (!g_raw) return;
    tcsetattr(0, TCSANOW, &g_saved_tio);
    write(1, "\033[2J\033[H", 7);
    g_raw = 0;
}
static void raw_on(void) {
    if (tcgetattr(0, &g_saved_tio) != 0) die("standard input is not a terminal");
    struct termios t = g_saved_tio;
    t.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(unsigned)(ICRNL | IXON | INLCR | IGNCR);
    t.c_oflag &= ~(unsigned)(OPOST);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) die("cannot set raw mode");
    g_raw = 1;
}
static void term_size(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 6 && ws.ws_col > 20) {
        rows = ws.ws_row; cols = ws.ws_col;
    } else { rows = 24; cols = 80; }
    if (rows > 200) rows = 200;
    if (cols > 512) cols = 512;
}

/* ---------------- key decoding ---------------- */

enum {
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL,
    K_META = 0x200      /* | ascii */
};

static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(0, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000 + (long)(ts.tv_nsec / 1000000);
}
/* Is another byte already waiting? Used to tell ESC-as-Alt and a CSI
 * sequence apart from a bare ESC keypress. FIONREAD, not poll(): libtoby's
 * poll is on record as answering "ready" when it does not know. */
static int pending(int ms) {
    long deadline = now_ms() + ms;
    for (;;) {
        int avail = 0;
        if (ioctl(0, FIONREAD, &avail) == 0 && avail > 0) return 1;
        if (now_ms() >= deadline) return 0;
        for (volatile int s = 0; s < 20000; s++) { }
    }
}
/* Optional keystroke trace to a FILE (not the terminal, which is busy
 * being the editor). Set TEDIT_TRACE=<path>. Reading the source gave
 * three confident wrong answers about where this editor was blocking;
 * printing what it was ACTUALLY GIVEN answered it in one run. */
static FILE *g_trace;
static void trace_init(void) {
    const char *p = getenv("TEDIT_TRACE");
    if (p && *p) g_trace = fopen(p, "wb");
}
static void trace(const char *fmt, ...) {
    if (!g_trace) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_trace, fmt, ap); va_end(ap);
    fflush(g_trace);
}

static int rawkey(void) {
    unsigned char c;
    long n = read(0, &c, 1);
    if (n <= 0) { trace("read->EOF(%ld)\n", n); return -1; }
    return (int)c;
}
static int getkey(void) {
    int c = rawkey();
    if (c != 27) {
        /* Some console paths deliver arrows as 0x80..0x83 rather than as
         * escape sequences; accept both rather than assume one. */
        if (c == 0x80) return K_UP;
        if (c == 0x81) return K_DOWN;
        if (c == 0x82) return K_LEFT;
        if (c == 0x83) return K_RIGHT;
        return c;
    }
    if (!pending(40)) return 27;            /* a real, bare ESC */
    int c2 = rawkey();
    if (c2 == '[' || c2 == 'O') {
        int c3 = rawkey();
        switch (c3) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            default: break;
        }
        if (c3 >= '0' && c3 <= '9') {       /* ESC [ n ~ */
            int n = c3 - '0', c4;
            while ((c4 = rawkey()) >= '0' && c4 <= '9') n = n * 10 + (c4 - '0');
            switch (n) {
                case 1: case 7: return K_HOME;
                case 3: return K_DEL;
                case 4: case 8: return K_END;
                case 5: return K_PGUP;
                case 6: return K_PGDN;
                default: return -2;
            }
        }
        return -2;
    }
    return K_META | (c2 & 0xff);
}

/* ---------------- screen ---------------- */

static char *OB; static int OB_len, OB_cap;
static void ob_reset(void) { OB_len = 0; }
static void ob_put(const char *s, int n) {
    if (OB_len + n + 1 > OB_cap) {
        OB_cap = OB_cap ? OB_cap : 8192;
        while (OB_cap < OB_len + n + 1) OB_cap *= 2;
        OB = xrealloc(OB, (size_t)OB_cap);
    }
    memcpy(OB + OB_len, s, (size_t)n); OB_len += n;
}
static void ob_str(const char *s) { ob_put(s, (int)strlen(s)); }
static void ob_fmt(const char *fmt, ...) {
    char t[1024]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (n > 0) ob_put(t, n < (int)sizeof t ? n : (int)sizeof t - 1);
}

static int numwidth(void) {
    if (!opt_number) return 0;
    int w = 1, n = B_n; while (n >= 10) { n /= 10; w++; }
    return (w < 3 ? 3 : w) + 1;
}
static int screen_col(int y, int bc) {
    int sc = 0;
    for (int i = 0; i < bc && i < B[y].len; i++)
        sc += (B[y].s[i] == '\t') ? opt_tabstop - (sc % opt_tabstop) : 1;
    return sc;
}
static int textrows(void) { return rows - 4; }   /* title + status + 2 keybar */

static void scroll_fix(void) {
    int tr = textrows();
    if (cy < top) top = cy;
    if (cy >= top + tr) top = cy - tr + 1;
    if (top < 0) top = 0;
}

/* Is (y,x) inside the marked region? */
static int in_region(int y, int x) {
    if (!mark_set) return 0;
    int sy = mark_y, sx = mark_x, ey = cy, ex = cx;
    if (sy > ey || (sy == ey && sx > ex)) { int t;
        t = sy; sy = ey; ey = t; t = sx; sx = ex; ex = t; }
    if (y < sy || y > ey) return 0;
    if (y == sy && x < sx) return 0;
    if (y == ey && x >= ex) return 0;
    return 1;
}

static void draw(const char *prompt, const char *answer) {
    scroll_fix();
    int nw = numwidth();
    ob_reset();
    ob_str("\033[H");

    /* title bar */
    ob_str("\033[K\033[7m");
    {
        char t[256];
        snprintf(t, sizeof t, "  tedit %s%s", g_nofile ? "New Buffer" : g_file,
                 g_dirty ? "  (modified)" : "");
        ob_fmt("%-*.*s", cols, cols, t);
    }
    ob_str("\033[m\r\n");

    /* text */
    for (int r = 0; r < textrows(); r++) {
        int y = top + r;
        ob_str("\033[K");
        if (y < B_n) {
            if (opt_number) ob_fmt("\033[2m%*d \033[m", nw - 1, y + 1);
            int sc = 0, inmark = 0;
            for (int i = 0; i < B[y].len && sc < cols - nw; i++) {
                int want = in_region(y, i);
                if (want != inmark) { ob_str(want ? "\033[7m" : "\033[m"); inmark = want; }
                char ch = B[y].s[i];
                if (ch == '\t') {
                    int adv = opt_tabstop - (sc % opt_tabstop);
                    for (int k = 0; k < adv && sc < cols - nw; k++) { ob_str(" "); sc++; }
                } else if ((unsigned char)ch < 32) { ob_fmt("^%c", ch + 64); sc += 2; }
                else { ob_put(&ch, 1); sc++; }
            }
            if (inmark) ob_str("\033[m");
        }
        ob_str("\r\n");
    }

    /* status / prompt line */
    ob_str("\033[K");
    if (prompt) {
        ob_str("\033[7m"); ob_fmt("%s", prompt); ob_str("\033[m");
        ob_fmt("%s", answer ? answer : "");
    } else if (g_msg[0]) {
        ob_str("\033[7m"); ob_fmt("[ %.*s ]", cols - 5, g_msg); ob_str("\033[m");
    }
    ob_str("\r\n");

    /* two-line shortcut bar, nano's shape */
    static const char *bar[2][6] = {
        { "^G Help", "^O Write Out", "^W Where Is", "^K Cut",   "^_ Go To Line", "M-U Undo" },
        { "^X Exit", "^R Read File", "^\\ Replace", "^U Uncut", "^6 Mark",       "M-E Redo" },
    };
    for (int r = 0; r < 2; r++) {
        ob_str("\033[K");
        int w = cols / 6;
        for (int i = 0; i < 6; i++) {
            char cell[64];
            const char *s = bar[r][i];
            int klen = (int)(strchr(s, ' ') - s);
            snprintf(cell, sizeof cell, "%.*s", klen, s);
            ob_str("\033[7m"); ob_fmt("%s", cell); ob_str("\033[m");
            ob_fmt("%-*.*s", w - klen, w - klen - 1, s + klen);
        }
        if (r == 0) ob_str("\r\n");
    }

    /* cursor */
    if (prompt) {
        ob_fmt("\033[%d;%dH", rows - 2,
               (int)strlen(prompt) + (int)(answer ? strlen(answer) : 0) + 1);
    } else {
        int sc = screen_col(cy, cx) + nw;
        if (sc >= cols) sc = cols - 1;
        ob_fmt("\033[%d;%dH", cy - top + 2, sc + 1);
    }
    trace("draw write %d bytes\n", OB_len);
    long wrote = write(1, OB, (size_t)OB_len);
    trace("draw wrote %ld\n", wrote);
}

static void clamp(void) {
    if (cy < 0) cy = 0;
    if (cy >= B_n) cy = B_n - 1;
    if (cx < 0) cx = 0;
    if (cx > B[cy].len) cx = B[cy].len;
}

/* ---------------- prompts ---------------- */

/* Returns 1 = accepted, 0 = cancelled (^C or ESC).
 * `oninput` is called after every keystroke when non-NULL -- that is what
 * makes Where Is incremental. */
static int prompt_line(const char *p, char *out, int cap,
                       void (*oninput)(const char *)) {
    int n = (int)strlen(out);
    trace("prompt enter '%s' pre='%s'\n", p, out);
    for (;;) {
        draw(p, out);
        int c = getkey();
        trace("prompt key=%d\n", c);
        if (c == -1 || c == 3 || c == 27) { trace("prompt cancel\n"); return 0; }
        if (c == '\r' || c == '\n') { trace("prompt accept '%s'\n", out); return 1; }
        if (c == 127 || c == 8) { if (n > 0) out[--n] = '\0'; }
        else if (c == 21) { n = 0; out[0] = '\0'; }      /* ^U clears */
        else if (c < 32 || c > 126) continue;
        else if (n < cap - 1) { out[n++] = (char)c; out[n] = '\0'; }
        if (oninput) oninput(out);
    }
}

static int prompt_yesno(const char *p) {
    for (;;) {
        draw(p, "");
        int c = getkey();
        if (c == 'y' || c == 'Y') return 1;
        if (c == 'n' || c == 'N') return 0;
        if (c == 3 || c == 27) return -1;
    }
}

/* ---------------- search ---------------- */

static int find_from(const char *pat, int sy, int sx, int back, int *fy, int *fx) {
    regex_t re;
    int flags = opt_icase ? REG_ICASE : 0;
    if (opt_regex) { if (regcomp(&re, pat, flags) != 0) return -1; }
    int plen = (int)strlen(pat);
    int y = sy;
    for (int step = 0; step <= B_n; step++) {
        const char *s = B[y].s;
        if (!back) {
            int from = (step == 0) ? sx : 0;
            if (from <= B[y].len) {
                if (opt_regex) {
                    regmatch_t m;
                    if (regexec(&re, s + from, 1, &m, from ? REG_NOTBOL : 0) == 0) {
                        *fy = y; *fx = from + (int)m.rm_so; regfree(&re); return 1;
                    }
                } else {
                    const char *h = strstr(s + from, pat);
                    if (h) { *fy = y; *fx = (int)(h - s); return 1; }
                }
            }
        } else {
            int limit = (step == 0) ? sx : B[y].len;
            int best = -1;
            for (int off = 0; off <= B[y].len; off++) {
                int hit = -1;
                if (opt_regex) {
                    regmatch_t m;
                    if (regexec(&re, s + off, 1, &m, off ? REG_NOTBOL : 0) == 0)
                        hit = off + (int)m.rm_so;
                } else if (plen && strncmp(s + off, pat, (size_t)plen) == 0) hit = off;
                if (hit < 0) { if (opt_regex) break; else continue; }
                if (hit >= limit) break;
                best = hit; off = hit;
            }
            if (best >= 0) { *fy = y; *fx = best; if (opt_regex) regfree(&re); return 1; }
        }
        y += back ? -1 : 1;
        if (y >= B_n) y = 0;
        if (y < 0) y = B_n - 1;
    }
    if (opt_regex) regfree(&re);
    return 0;
}

/* Total matches, and which one the cursor is at or before. An improvement
 * over nano, which tells you nothing until you commit the search. */
static void match_stats(const char *pat, int *total, int *idx) {
    *total = 0; *idx = 0;
    if (!pat || !*pat) return;
    regex_t re; int use_re = opt_regex;
    if (use_re && regcomp(&re, pat, opt_icase ? REG_ICASE : 0) != 0) return;
    int plen = (int)strlen(pat);
    for (int y = 0; y < B_n; y++) {
        int off = 0;
        while (off <= B[y].len) {
            int so = -1, eo = -1;
            if (use_re) {
                regmatch_t m;
                if (regexec(&re, B[y].s + off, 1, &m, off ? REG_NOTBOL : 0) != 0) break;
                so = off + (int)m.rm_so; eo = off + (int)m.rm_eo;
            } else {
                if (!plen) break;
                const char *h = strstr(B[y].s + off, pat);
                if (!h) break;
                so = (int)(h - B[y].s); eo = so + plen;
            }
            (*total)++;
            if (y < cy || (y == cy && so <= cx)) *idx = *total;
            off = (eo > so) ? eo : so + 1;
        }
    }
    if (use_re) regfree(&re);
}

static void search_preview(const char *pat) {
    if (!pat || !*pat) { msg(""); return; }
    int total, idx;
    match_stats(pat, &total, &idx);
    if (total == 0) msg("no matches");
    else msg("match %d of %d", idx ? idx : 1, total);
}

static void do_search(int again) {
    if (!again) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", g_pat);
        char p[128];
        snprintf(p, sizeof p, "Search%s%s: ",
                 opt_regex ? " [regex]" : "", g_search_back ? " [back]" : "");
        if (!prompt_line(p, buf, sizeof buf, search_preview)) { msg("Cancelled"); return; }
        if (buf[0]) snprintf(g_pat, sizeof g_pat, "%s", buf);
    }
    if (!g_pat[0]) { msg("No search string"); return; }
    int fy, fx;
    int rc = find_from(g_pat, cy, cx + (g_search_back ? 0 : 1), g_search_back, &fy, &fx);
    if (rc < 0) { msg("Invalid regex: %s", g_pat); return; }
    if (!rc) { msg("\"%s\" not found", g_pat); return; }
    if (fy < cy || (fy == cy && fx <= cx)) msg("Search Wrapped");
    else { int t, i; match_stats(g_pat, &t, &i); msg("match %d of %d", i + 1 > t ? t : i + 1, t); }
    cy = fy; cx = fx; break_run(); clamp();
}

/* ---------------- replace ---------------- */

/* Expand & and \1..\9 against the match at (y, so..eo). */
static int expand_rep(const char *rep, int y, regmatch_t *m, int base,
                      char *out, int cap) {
    int o = 0;
    for (const char *r = rep; *r && o < cap - 1; r++) {
        int gi = -1;
        if (*r == '&') gi = 0;
        else if (*r == '\\' && r[1] >= '0' && r[1] <= '9') { gi = r[1] - '0'; r++; }
        else if (*r == '\\' && r[1]) { r++; out[o++] = *r; continue; }
        if (gi >= 0) {
            if (!m || m[gi].rm_so < 0) continue;
            int gs = base + (int)m[gi].rm_so, ge = base + (int)m[gi].rm_eo;
            for (int i = gs; i < ge && o < cap - 1; i++) out[o++] = B[y].s[i];
        } else out[o++] = *r;
    }
    out[o] = '\0';
    return o;
}

static void do_replace(void) {
    char pat[256]; snprintf(pat, sizeof pat, "%s", g_pat);
    char p[128];
    snprintf(p, sizeof p, "Search%s (to replace): ", opt_regex ? " [regex]" : "");
    if (!prompt_line(p, pat, sizeof pat, search_preview)) { msg("Cancelled"); return; }
    if (!pat[0]) { msg("No search string"); return; }
    snprintf(g_pat, sizeof g_pat, "%s", pat);
    char rep[256]; snprintf(rep, sizeof rep, "%s", g_rep);
    if (!prompt_line("Replace with: ", rep, sizeof rep, NULL)) { msg("Cancelled"); return; }
    snprintf(g_rep, sizeof g_rep, "%s", rep);

    regex_t re;
    if (opt_regex && regcomp(&re, pat, opt_icase ? REG_ICASE : 0) != 0) {
        msg("Invalid regex: %s", pat); return;
    }
    int plen = (int)strlen(pat);
    int all = 0, n = 0, pushed = 0;
    int y = cy, x = cx;

    while (y < B_n) {
        int so = -1, eo = -1;
        regmatch_t m[10];
        if (opt_regex) {
            if (regexec(&re, B[y].s + x, 10, m, x ? REG_NOTBOL : 0) != 0) {
                y++; x = 0; continue;
            }
            so = x + (int)m[0].rm_so; eo = x + (int)m[0].rm_eo;
        } else {
            if (!plen) break;
            const char *h = strstr(B[y].s + x, pat);
            if (!h) { y++; x = 0; continue; }
            so = (int)(h - B[y].s); eo = so + plen;
            for (int i = 0; i < 10; i++) { m[i].rm_so = -1; m[i].rm_eo = -1; }
            m[0].rm_so = so - x; m[0].rm_eo = eo - x;
        }
        cy = y; cx = so; clamp();
        int doit = all;
        if (!all) {
            draw("Replace this instance? (Y/N/A/^C) ", "");
            int c = getkey();
            if (c == 3 || c == 27) break;
            if (c == 'a' || c == 'A') { all = 1; doit = 1; }
            else if (c == 'y' || c == 'Y' || c == '\r' || c == '\n') doit = 1;
            else if (c == 'n' || c == 'N') doit = 0;
            else continue;
        }
        if (doit) {
            if (!pushed) { edit_other(); pushed = 1; }
            char rbuf[1024];
            int rl = expand_rep(rep, y, opt_regex ? m : NULL, x, rbuf, sizeof rbuf);
            struct line *l = &B[y];
            int tail = l->len - eo;
            line_reserve(l, so + rl + tail + 1);
            memmove(l->s + so + rl, l->s + eo, (size_t)tail + 1);
            memcpy(l->s + so, rbuf, (size_t)rl);
            l->len = so + rl + tail; l->s[l->len] = '\0';
            g_dirty = 1; n++;
            x = so + rl;
            if (rl == 0 && eo == so) x++;
        } else {
            x = (eo > so) ? eo : so + 1;
        }
        if (x > B[y].len) { y++; x = 0; }
    }
    if (opt_regex) regfree(&re);
    msg(n ? "Replaced %d occurrence%s" : "Nothing replaced", n, n == 1 ? "" : "s");
    clamp();
}

/* ---------------- editing ---------------- */

static void ins_char(int ch) {
    edit_typed();
    struct line *l = &B[cy];
    line_reserve(l, l->len + 2);
    memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx + 1));
    l->s[cx] = (char)ch; l->len++; cx++;
    run_y = cy; run_x = cx;
    g_dirty = 1;
}

static void do_enter(void) {
    edit_other();
    struct line *l = &B[cy];
    int tail = l->len - cx;
    char *t = xrealloc(NULL, (size_t)tail + 1);
    memcpy(t, l->s + cx, (size_t)tail); t[tail] = '\0';
    l->len = cx; l->s[cx] = '\0';
    int ind = 0;
    if (opt_autoindent)
        while (ind < l->len && (l->s[ind] == ' ' || l->s[ind] == '\t')) ind++;
    line_insert(cy + 1, "", 0);
    line_reserve(&B[cy + 1], ind + tail + 1);
    memcpy(B[cy + 1].s, B[cy].s, (size_t)ind);
    memcpy(B[cy + 1].s + ind, t, (size_t)tail);
    B[cy + 1].len = ind + tail; B[cy + 1].s[B[cy + 1].len] = '\0';
    free(t);
    cy++; cx = ind; g_dirty = 1;
}

static void do_backspace(void) {
    if (cx > 0) {
        edit_other();
        struct line *l = &B[cy];
        memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx + 1));
        l->len--; cx--; g_dirty = 1;
    } else if (cy > 0) {
        edit_other();
        int prev = B[cy - 1].len;
        struct line *a = &B[cy - 1], *b = &B[cy];
        line_reserve(a, a->len + b->len + 1);
        memcpy(a->s + a->len, b->s, (size_t)b->len + 1);
        a->len += b->len;
        line_delete(cy);
        cy--; cx = prev; g_dirty = 1;
    }
}

static void do_delete(void) {
    if (cx < B[cy].len) {
        edit_other();
        struct line *l = &B[cy];
        memmove(l->s + cx, l->s + cx + 1, (size_t)(l->len - cx));
        l->len--; g_dirty = 1;
    } else if (cy + 1 < B_n) {
        edit_other();
        struct line *a = &B[cy], *b = &B[cy + 1];
        line_reserve(a, a->len + b->len + 1);
        memcpy(a->s + a->len, b->s, (size_t)b->len + 1);
        a->len += b->len;
        line_delete(cy + 1); g_dirty = 1;
    }
}

static void cut_append(const char *s, int len, int linewise) {
    if (!g_last_was_cut) { free(CUT); CUT = NULL; CUT_len = 0; CUT_linewise = 0; }
    CUT = xrealloc(CUT, (size_t)(CUT_len + len + 1));
    memcpy(CUT + CUT_len, s, (size_t)len);
    CUT_len += len; CUT[CUT_len] = '\0';
    CUT_linewise = linewise || CUT_linewise;
}

/* Normalised region bounds; returns 0 if there is no region. */
static int region(int *sy, int *sx, int *ey, int *ex) {
    if (!mark_set) return 0;
    *sy = mark_y; *sx = mark_x; *ey = cy; *ex = cx;
    if (*sy > *ey || (*sy == *ey && *sx > *ex)) {
        int t; t = *sy; *sy = *ey; *ey = t; t = *sx; *sx = *ex; *ex = t;
    }
    return 1;
}

static void region_text(int sy, int sx, int ey, int ex, char **out, int *len) {
    int total = 0;
    for (int y = sy; y <= ey; y++) {
        int a = (y == sy) ? sx : 0, b = (y == ey) ? ex : B[y].len;
        total += (b - a) + 1;
    }
    char *t = xrealloc(NULL, (size_t)total + 1);
    int o = 0;
    for (int y = sy; y <= ey; y++) {
        int a = (y == sy) ? sx : 0, b = (y == ey) ? ex : B[y].len;
        memcpy(t + o, B[y].s + a, (size_t)(b - a)); o += b - a;
        if (y != ey) t[o++] = '\n';
    }
    t[o] = '\0'; *out = t; *len = o;
}

static void region_delete(int sy, int sx, int ey, int ex) {
    if (sy == ey) {
        struct line *l = &B[sy];
        memmove(l->s + sx, l->s + ex, (size_t)(l->len - ex + 1));
        l->len -= (ex - sx);
    } else {
        struct line *a = &B[sy];
        int taillen = B[ey].len - ex;
        line_reserve(a, sx + taillen + 1);
        memcpy(a->s + sx, B[ey].s + ex, (size_t)taillen);
        a->len = sx + taillen; a->s[a->len] = '\0';
        for (int i = 0; i < ey - sy; i++) line_delete(sy + 1);
    }
    cy = sy; cx = sx; g_dirty = 1;
}

static void do_cut(void) {
    int sy, sx, ey, ex;
    if (region(&sy, &sx, &ey, &ex)) {
        edit_other();
        char *t; int n; region_text(sy, sx, ey, ex, &t, &n);
        cut_append(t, n, 0); free(t);
        region_delete(sy, sx, ey, ex);
        mark_set = 0;
        msg("Cut selection");
    } else {
        edit_other();
        char *t = xrealloc(NULL, (size_t)B[cy].len + 2);
        memcpy(t, B[cy].s, (size_t)B[cy].len); t[B[cy].len] = '\n'; t[B[cy].len + 1] = '\0';
        cut_append(t, B[cy].len + 1, 1); free(t);
        line_delete(cy);
        cx = 0; g_dirty = 1;
    }
    clamp();
}

static void do_copy(void) {
    int sy, sx, ey, ex;
    if (!region(&sy, &sx, &ey, &ex)) { msg("No region marked"); return; }
    char *t; int n; region_text(sy, sx, ey, ex, &t, &n);
    g_last_was_cut = 0; cut_append(t, n, 0); free(t);
    mark_set = 0;
    msg("Copied %d bytes", n);
}

static void do_uncut(void) {
    if (!CUT || !CUT_len) { msg("Cutbuffer is empty"); return; }
    edit_other();
    /* Split the cutbuffer on newlines and splice it in at the cursor. */
    int start = 0;
    for (int i = 0; i <= CUT_len; i++) {
        if (i == CUT_len || CUT[i] == '\n') {
            int seglen = i - start;
            struct line *l = &B[cy];
            line_reserve(l, l->len + seglen + 1);
            memmove(l->s + cx + seglen, l->s + cx, (size_t)(l->len - cx + 1));
            memcpy(l->s + cx, CUT + start, (size_t)seglen);
            l->len += seglen; cx += seglen;
            if (i < CUT_len) {          /* the newline splits the line */
                struct line *cur = &B[cy];
                int tail = cur->len - cx;
                char *t = xrealloc(NULL, (size_t)tail + 1);
                memcpy(t, cur->s + cx, (size_t)tail); t[tail] = '\0';
                cur->len = cx; cur->s[cx] = '\0';
                line_insert(cy + 1, t, tail);
                free(t);
                cy++; cx = 0;
            }
            start = i + 1;
        }
    }
    g_dirty = 1; clamp();
    msg("Uncut %d bytes", CUT_len);
}

static void bracket_match(void) {
    static const char *op = "([{", *cl = ")]}";
    if (cx >= B[cy].len) { msg("Not on a bracket"); return; }
    char c = B[cy].s[cx];
    const char *o = strchr(op, c), *k = strchr(cl, c);
    int dir, want, have;
    if (o) { dir = 1; have = c; want = cl[o - op]; }
    else if (k) { dir = -1; have = c; want = op[k - cl]; }
    else { msg("Not on a bracket"); return; }
    int depth = 0, y = cy, x = cx;
    for (;;) {
        if (x < 0) { if (--y < 0) break; x = B[y].len - 1; if (x < 0) continue; }
        else if (x >= B[y].len) { if (++y >= B_n) break; x = 0; if (!B[y].len) continue; }
        char ch = B[y].s[x];
        if (ch == have) depth++;
        else if (ch == want && --depth == 0) { cy = y; cx = x; break_run(); clamp();
                                               msg("Matched"); return; }
        x += dir;
    }
    msg("No matching bracket");
}

/* ---------------- write out / exit ---------------- */

/* ALWAYS prompts, with the current name pre-filled -- which is what nano
 * does, and therefore what "^O then Enter" means to anyone's fingers.
 * Skipping the prompt for an already-named buffer looked like a kindness
 * and was not: it turned the Enter people reflexively type after ^O into
 * a literal newline inserted in their file. */
static int write_out(int unused) {
    (void)unused;
    char name[512];
    snprintf(name, sizeof name, "%s", g_nofile ? "" : g_file);
    {
        if (!prompt_line("File Name to Write: ", name, sizeof name, NULL)) {
            msg("Cancelled"); return 0;
        }
        if (!name[0]) { msg("Cancelled"); return 0; }
    }
    trace("saving '%s'\n", name);
    if (file_save(name) != 0) { trace("save FAILED\n"); msg("Error writing %s", name); return 0; }
    trace("saved ok\n");
    snprintf(g_file, sizeof g_file, "%s", name);
    g_nofile = 0; g_dirty = 0;
    msg("Wrote %d line%s, %ld bytes", B_n, B_n == 1 ? "" : "s", file_bytes());
    return 1;
}

static void show_help(void) {
    ob_reset();
    ob_str("\033[2J\033[H");
    ob_str("tedit -- native modeless editor for tobyOS\r\n\r\n");
    ob_str("  ^G help          ^X exit           ^O write out     ^R read file\r\n");
    ob_str("  ^W where is      M-W find next     ^\\ replace       ^_ go to line[,col]\r\n");
    ob_str("  ^K cut line/sel  M-6 copy region   ^U uncut/paste   ^6 set mark\r\n");
    ob_str("  ^A line start    ^E line end       ^Y page up       ^V page down\r\n");
    ob_str("  ^P/^N up/down    ^B/^F left/right  ^D delete        ^C cursor position\r\n");
    ob_str("  M-U undo         M-E redo          M-\\ first line   M-/ last line\r\n");
    ob_str("  M-] match bracket  M-# line numbers  M-I autoindent  M-R regex  M-C case\r\n");
    ob_str("\r\n  Improvements over nano:\r\n");
    ob_str("   - undo/redo collapses a typing run into ONE step\r\n");
    ob_str("   - real POSIX regex, with & and \\1..\\9 in the replacement\r\n");
    ob_str("   - Where Is shows a live \"match N of M\" as you type\r\n");
    ob_str("   - M-6 copies a region without cutting it first\r\n");
    ob_str("   - ^_ takes line,column; M-] jumps to the matching bracket\r\n");
    ob_str("\r\n  Press any key to continue.\r\n");
    write(1, OB, (size_t)OB_len);
    getkey();
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    const char *path = NULL;
    int startline = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: tedit [+N] [file]\n"); return 0;
        }
        if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1]))
            startline = atoi(argv[i] + 1) - 1;
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "tedit: unknown option %s\n", argv[i]); return 2;
        } else path = argv[i];
    }

    buf_reserve(256); B_n = 1; line_set(0, "", 0);
    if (path) {
        snprintf(g_file, sizeof g_file, "%s", path);
        if (file_load(path) == 1) msg("New File");
        else msg("Read %d line%s", B_n, B_n == 1 ? "" : "s");
    } else { g_nofile = 1; msg("New Buffer"); }
    if (startline > 0) cy = startline < B_n ? startline : B_n - 1;

    trace_init();
    trace("start file=%s lines=%d\n", g_file, B_n);
    raw_on();
    term_size();
    trace("raw on, rows=%d cols=%d\n", rows, cols);

    int quit = 0;
    while (!quit) {
        clamp();
        draw(NULL, NULL);
        int c = getkey();
        trace("key=%d\n", c);
        if (c == -1) break;
        if (c != 11) g_last_was_cut = 0;     /* only ^K runs accumulate */
        int keep_msg = 0;

        switch (c) {
        /* ---- motion ---- */
        case K_LEFT: case 2:  if (cx > 0) cx--; else if (cy > 0) { cy--; cx = B[cy].len; }
                              break_run(); want_col = cx; break;
        case K_RIGHT: case 6: if (cx < B[cy].len) cx++; else if (cy + 1 < B_n) { cy++; cx = 0; }
                              break_run(); want_col = cx; break;
        case K_UP: case 16:   if (cy > 0) { cy--; cx = want_col; } break_run(); break;
        case K_DOWN: case 14: if (cy + 1 < B_n) { cy++; cx = want_col; } break_run(); break;
        case K_HOME: case 1:  cx = 0; want_col = 0; break_run(); break;
        case K_END:  case 5:  cx = B[cy].len; want_col = cx; break_run(); break;
        case K_PGUP: case 25: cy -= textrows(); top -= textrows();
                              if (top < 0) top = 0; break_run(); break;
        case K_PGDN: case 22: cy += textrows(); top += textrows();
                              if (top > B_n - 1) top = B_n - 1; break_run(); break;

        /* ---- editing ---- */
        case '\r': case '\n': do_enter(); break;
        case 127: case 8:     do_backspace(); break;
        case K_DEL: case 4:   do_delete(); break;
        case '\t':            ins_char('\t'); break;

        /* ---- file ---- */
        case 15: write_out(0); keep_msg = 1; break;                 /* ^O */
        case 24: {                                                  /* ^X */
            if (g_dirty) {
                int a = prompt_yesno("Save modified buffer? (Y/N/^C) ");
                if (a < 0) { msg("Cancelled"); break; }
                if (a == 1 && !write_out(0)) break;
            }
            quit = 1; break;
        }
        case 18: {                                                  /* ^R */
            char name[512] = "";
            if (!prompt_line("File to insert: ", name, sizeof name, NULL)) { msg("Cancelled"); break; }
            FILE *f = fopen(name, "rb");
            if (!f) { msg("Error reading %s", name); break; }
            edit_other();
            char lb[4096]; int li = 0, ch, added = 0;
            while ((ch = fgetc(f)) != EOF) {
                if (ch == '\n') { lb[li] = '\0'; line_insert(++cy, lb, li); li = 0; added++; }
                else if (li < (int)sizeof lb - 1) lb[li++] = (char)ch;
            }
            if (li) { lb[li] = '\0'; line_insert(++cy, lb, li); added++; }
            fclose(f); g_dirty = 1; keep_msg = 1;
            msg("Inserted %d line%s", added, added == 1 ? "" : "s");
            break;
        }

        /* ---- search / replace ---- */
        case 23: do_search(0); keep_msg = 1; break;                 /* ^W */
        case 28: do_replace(); keep_msg = 1; break;                 /* ^\ */

        /* ---- cut / paste / mark ---- */
        case 11: do_cut(); g_last_was_cut = 1; break;               /* ^K */
        case 21: do_uncut(); keep_msg = 1; break;                   /* ^U */
        case 30: mark_set = !mark_set; mark_y = cy; mark_x = cx;    /* ^6 */
                 msg(mark_set ? "Mark set" : "Mark unset"); keep_msg = 1; break;

        /* ---- info / misc ---- */
        case 7: show_help(); break;                                  /* ^G */
        case 3: msg("line %d/%d, col %d, char %ld/%ld", cy + 1, B_n, cx + 1,
                    (long)0, file_bytes()); keep_msg = 1; break;     /* ^C */
        case 31: {                                                   /* ^_ */
            char buf[64] = "";
            if (!prompt_line("Go to line[,column]: ", buf, sizeof buf, NULL)) { msg("Cancelled"); break; }
            int l = 0, co = 1; const char *q = buf;
            while (isdigit((unsigned char)*q)) l = l * 10 + (*q++ - '0');
            if (*q == ',' || *q == ':') { q++; co = 0;
                while (isdigit((unsigned char)*q)) co = co * 10 + (*q++ - '0'); }
            if (l > 0) { cy = l - 1 < B_n ? l - 1 : B_n - 1; cx = co > 0 ? co - 1 : 0; }
            break_run(); clamp(); break;
        }
        case 12: term_size(); break;                                 /* ^L */

        default:
            if (c & K_META) {
                int mk = c & 0xff;
                switch (mk) {
                case 'u': case 'U': do_undo(); keep_msg = 1; break;
                case 'e': case 'E': do_redo(); keep_msg = 1; break;
                case 'w': case 'W': do_search(1); keep_msg = 1; break;
                case '6': do_copy(); keep_msg = 1; break;
                case ']': bracket_match(); keep_msg = 1; break;
                case '#': opt_number = !opt_number;
                          msg("Line numbers %s", opt_number ? "on" : "off"); keep_msg = 1; break;
                case 'i': case 'I': opt_autoindent = !opt_autoindent;
                          msg("Auto-indent %s", opt_autoindent ? "on" : "off"); keep_msg = 1; break;
                case 'r': case 'R': opt_regex = !opt_regex;
                          msg("Regex search %s", opt_regex ? "on" : "off"); keep_msg = 1; break;
                case 'c': case 'C': opt_icase = !opt_icase;
                          msg("Case-insensitive %s", opt_icase ? "on" : "off"); keep_msg = 1; break;
                case 'b': case 'B': g_search_back = !g_search_back;
                          msg("Search direction %s", g_search_back ? "backward" : "forward");
                          keep_msg = 1; break;
                case '\\': cy = 0; cx = 0; break_run(); break;
                case '/':  cy = B_n - 1; cx = 0; break_run(); break;
                default: break;
                }
            } else if (c >= 32 && c < 127) {
                ins_char(c);
            }
            break;
        }
        if (!keep_msg && c != 3) g_msg[0] = '\0';
    }

    trace("exit dirty=%d\n", g_dirty);
    raw_off();
    return 0;
}
