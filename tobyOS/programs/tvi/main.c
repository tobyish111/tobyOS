/* tvi -- a native VT100 visual editor for tobyOS, POSIX vi(1)-shaped.
 *
 * WHY THIS EXISTS, honestly stated: unlike lspci/lsusb/dmidecode, this
 * one is NOT unlocking a kernel data source -- nano and busybox vi both
 * already run here. What it does have that those did not is a SPEC:
 * vi is a POSIX XCU utility, so "native" means "conforms", and the
 * behaviour below is checkable rather than a matter of taste. That is
 * also what makes it gateable (logs/tvi.sh drives keystrokes and asserts
 * the resulting FILE BYTES, not that the program exited 0).
 *
 * Deliberately NOT started from programs/user_gui_edit: that is a GUI
 * editor built on TobyTK widgets and shares nothing with a terminal
 * program that must own raw mode, cursor addressing and a modal parser.
 *
 * Structure:
 *   - buffer  : an array of lines, each a grown char array. Line-oriented
 *               because every vi operator is either line-wise or
 *               within-line, and a gap buffer would buy nothing at the
 *               file sizes this OS edits.
 *   - undo    : whole-buffer snapshots in a bounded ring. Chosen over
 *               per-operation diffs because correctness matters more than
 *               memory here and a diff-based undo is where editors get
 *               subtly wrong; the cost is stated in UNDO_MAX below.
 *   - regex   : libtoby's real POSIX engine, so / ? and :s take genuine
 *               BREs including backrefs -- not a substring search
 *               wearing a regex's name.
 *
 * Terminal contract: raw mode via termios, size via TIOCGWINSZ (falling
 * back to 80x24 rather than guessing something exotic), output batched
 * into one write() per refresh so the screen never tears over a slow
 * serial console.
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
#include <errno.h>

#define TIOCGWINSZ 0x5413u
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define MAXLINES   200000
#define UNDO_MAX   64        /* snapshots; see the note above */
#define STATUS_MAX 512

/* ---------------- buffer ---------------- */

struct line { char *s; int len, cap; };

static struct line *B;
static int   B_n;              /* lines in use            */
static int   B_cap;
static char  g_file[512];
static int   g_dirty;
static int   g_readonly;
static int   g_nofile;         /* started with no filename */

/* view + cursor */
static int   cx, cy;           /* buffer coords: cy = line, cx = column */
static int   top;              /* first displayed line                  */
static int   want_col;         /* sticky column for j/k                 */
static int   rows = 24, cols = 80;

/* options */
static int   opt_number;
static int   opt_ignorecase;
static int   opt_autoindent;
static int   opt_tabstop = 8;

/* messages */
static char  g_msg[STATUS_MAX];
static int   g_msg_is_err;

/* registers: one unnamed register is what vi guarantees for the
 * operators implemented here. `linewise` decides whether p pastes above
 * or after the cursor, which is the difference between dd/p round-tripping
 * and quietly mangling the file. */
static char *R_text;
static int   R_len;
static int   R_linewise;

/* last search + last f/t, for n/N and ; / , */
static char  g_lastpat[256];
static int   g_lastdir = 1;
static char  g_lastft;
static char  g_lastftchar;

static struct termios g_saved_tio;
static int   g_raw_active;

/* ---------------- small helpers ---------------- */

static void die(const char *m) {
    write(1, "\033[2J\033[H", 7);
    fprintf(stderr, "tvi: %s\n", m);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static void msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_msg_is_err = 0;
}

static void errmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_msg_is_err = 1;
}

/* ---------------- line primitives ---------------- */

static void line_reserve(struct line *l, int n) {
    if (l->cap >= n) return;
    int c = l->cap ? l->cap : 32;
    while (c < n) c *= 2;
    l->s = xrealloc(l->s, (size_t)c);
    l->cap = c;
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
    memcpy(B[i].s, s, (size_t)len);
    B[i].s[len] = '\0';
    B[i].len = len;
}

static void line_insert(int at, const char *s, int len) {
    if (B_n + 1 > MAXLINES) { errmsg("file too large"); return; }
    buf_reserve(B_n + 1);
    memmove(B + at + 1, B + at, (size_t)(B_n - at) * sizeof *B);
    memset(B + at, 0, sizeof *B);
    B_n++;
    line_set(at, s, len);
}

static void line_delete(int at) {
    free(B[at].s);
    memmove(B + at, B + at + 1, (size_t)(B_n - at - 1) * sizeof *B);
    B_n--;
    memset(B + B_n, 0, sizeof *B);
    if (B_n == 0) { buf_reserve(1); B_n = 1; line_set(0, "", 0); }
}

/* ---------------- undo (snapshot ring) ---------------- */

struct snap { struct line *l; int n, cx, cy; };
static struct snap U[UNDO_MAX];
static int U_head, U_count, U_redo;

static void snap_free(struct snap *s) {
    if (!s->l) return;
    for (int i = 0; i < s->n; i++) free(s->l[i].s);
    free(s->l);
    s->l = NULL; s->n = 0;
}

static void snap_take(struct snap *s) {
    snap_free(s);
    s->l = xrealloc(NULL, (size_t)(B_n ? B_n : 1) * sizeof *s->l);
    memset(s->l, 0, (size_t)(B_n ? B_n : 1) * sizeof *s->l);
    for (int i = 0; i < B_n; i++) {
        s->l[i].len = B[i].len;
        s->l[i].cap = B[i].len + 1;
        s->l[i].s   = xrealloc(NULL, (size_t)s->l[i].cap);
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
    B_n = s->n;
    if (B_n == 0) { B_n = 1; line_set(0, "", 0); }
    cx = s->cx; cy = s->cy;
    if (cy >= B_n) cy = B_n - 1;
    if (cy < 0) cy = 0;
    if (cx > B[cy].len) cx = B[cy].len;
}

/* Called BEFORE any mutation. Pushing the pre-state is what makes one
 * `u` undo one command rather than half of one. */
static void undo_push(void) {
    snap_take(&U[U_head]);
    U_head = (U_head + 1) % UNDO_MAX;
    if (U_count < UNDO_MAX) U_count++;
    U_redo = 0;
}

static struct snap g_redo;
static int g_have_redo;

static void do_undo(void) {
    if (U_count == 0) { errmsg("Already at oldest change"); return; }
    snap_take(&g_redo); g_have_redo = 1;
    U_head = (U_head - 1 + UNDO_MAX) % UNDO_MAX;
    U_count--;
    snap_restore(&U[U_head]);
    g_dirty = 1;
    msg("1 change; before");
}

static void do_redo(void) {
    if (!g_have_redo) { errmsg("Already at newest change"); return; }
    struct snap cur; memset(&cur, 0, sizeof cur);
    snap_take(&cur);
    snap_restore(&g_redo);
    snap_free(&g_redo);
    g_redo = cur; /* toggling redo/undo stays symmetric */
    g_dirty = 1;
    msg("redo");
}

/* ---------------- file I/O ---------------- */

static void buf_empty(void) {
    for (int i = 0; i < B_n; i++) { free(B[i].s); }
    B_n = 0;
    buf_reserve(1);
    memset(B, 0, sizeof *B);
    B_n = 1; line_set(0, "", 0);
}

/* Returns: 0 ok, 1 new file, -1 error. A missing file is NOT an error --
 * vi opens it as a new buffer and says so. */
static int file_load(const char *path) {
    buf_empty();
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    B_n = 0;
    int c, cur_cap = 0, cur_len = 0; char *cur = NULL;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            buf_reserve(B_n + 1);
            memset(B + B_n, 0, sizeof *B);
            B_n++;
            line_set(B_n - 1, cur ? cur : "", cur_len);
            cur_len = 0;
        } else {
            if (cur_len + 1 >= cur_cap) {
                cur_cap = cur_cap ? cur_cap * 2 : 128;
                cur = xrealloc(cur, (size_t)cur_cap);
            }
            cur[cur_len++] = (char)c;
            cur[cur_len] = '\0';
        }
    }
    /* A final line with no trailing newline is still a line. */
    if (cur_len > 0) {
        buf_reserve(B_n + 1);
        memset(B + B_n, 0, sizeof *B);
        B_n++;
        line_set(B_n - 1, cur, cur_len);
    }
    free(cur);
    fclose(f);
    if (B_n == 0) { buf_reserve(1); memset(B, 0, sizeof *B); B_n = 1; line_set(0, "", 0); }
    return 0;
}

static long file_bytes(void) {
    long t = 0;
    for (int i = 0; i < B_n; i++) t += B[i].len + 1;
    return t;
}

static int file_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (int i = 0; i < B_n; i++) {
        if (B[i].len && fwrite(B[i].s, 1, (size_t)B[i].len, f) != (size_t)B[i].len) {
            fclose(f); return -1;
        }
        if (fputc('\n', f) == EOF) { fclose(f); return -1; }
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

/* ---------------- terminal ---------------- */

static void raw_off(void) {
    if (!g_raw_active) return;
    tcsetattr(0, TCSANOW, &g_saved_tio);
    write(1, "\033[2J\033[H", 7);
    g_raw_active = 0;
}

static void raw_on(void) {
    if (tcgetattr(0, &g_saved_tio) != 0) {
        /* Not a terminal. Refuse rather than scribble escapes into a pipe
         * and pretend to be an editor. */
        die("standard input is not a terminal");
    }
    struct termios t = g_saved_tio;
    t.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(unsigned)(ICRNL | IXON | INLCR | IGNCR);
    t.c_oflag &= ~(unsigned)(OPOST);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) die("cannot set raw mode");
    g_raw_active = 1;
}

static void term_size(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 10) {
        rows = ws.ws_row; cols = ws.ws_col;
    } else {
        rows = 24; cols = 80;   /* the honest default, not a guess */
    }
    if (rows > 200) rows = 200;
    if (cols > 512) cols = 512;
}

static int getkey(void) {
    unsigned char c;
    long n = read(0, &c, 1);
    if (n <= 0) return -1;
    return c;
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
    memcpy(OB + OB_len, s, (size_t)n);
    OB_len += n;
}
static void ob_str(const char *s) { ob_put(s, (int)strlen(s)); }
static void ob_fmt(const char *fmt, ...) {
    char t[1024]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (n > 0) ob_put(t, n < (int)sizeof t ? n : (int)sizeof t - 1);
}

static int numwidth(void) {
    if (!opt_number) return 0;
    int w = 1, n = B_n;
    while (n >= 10) { n /= 10; w++; }
    if (w < 3) w = 3;
    return w + 1;
}

/* Screen column of buffer column `bc`, honouring tabs. */
static int screen_col(int line, int bc) {
    int sc = 0;
    for (int i = 0; i < bc && i < B[line].len; i++) {
        if (B[line].s[i] == '\t') sc += opt_tabstop - (sc % opt_tabstop);
        else sc++;
    }
    return sc;
}

static void scroll_fix(void) {
    int textrows = rows - 1;
    if (cy < top) top = cy;
    if (cy >= top + textrows) top = cy - textrows + 1;
    if (top < 0) top = 0;
    if (top > B_n - 1) top = B_n - 1;
}

static void draw(int mode_insert, const char *cmdline) {
    int textrows = rows - 1;
    int nw = numwidth();
    scroll_fix();
    ob_reset();
    ob_str("\033[H");                    /* home; no full clear = no flicker */

    for (int r = 0; r < textrows; r++) {
        int ln = top + r;
        ob_str("\033[K");
        if (ln < B_n) {
            if (opt_number) ob_fmt("%*d ", nw - 1, ln + 1);
            /* Expand tabs; clip to the window. */
            int sc = 0;
            for (int i = 0; i < B[ln].len && sc < cols - nw; i++) {
                char ch = B[ln].s[i];
                if (ch == '\t') {
                    int adv = opt_tabstop - (sc % opt_tabstop);
                    for (int k = 0; k < adv && sc < cols - nw; k++) { ob_str(" "); sc++; }
                } else if ((unsigned char)ch < 32) {
                    ob_fmt("^%c", ch + 64); sc += 2;
                } else { ob_put(&ch, 1); sc++; }
            }
        } else {
            ob_str("~");                 /* past end of buffer, as vi does */
        }
        ob_str("\r\n");
    }

    /* status line */
    ob_str("\033[K\033[7m");
    if (cmdline) {
        ob_fmt("%.*s", cols - 1, cmdline);
    } else if (g_msg[0]) {
        ob_fmt("%.*s", cols - 1, g_msg);
    } else {
        char left[256];
        snprintf(left, sizeof left, "\"%s\"%s%s %d line%s",
                 g_nofile ? "[No Name]" : g_file,
                 g_readonly ? " [readonly]" : "",
                 g_dirty ? " [+]" : "",
                 B_n, B_n == 1 ? "" : "s");
        char right[64];
        snprintf(right, sizeof right, "%d,%d%s", cy + 1,
                 B[cy].len ? cx + 1 : 0, mode_insert ? "  -- INSERT --" : "");
        int pad = cols - 1 - (int)strlen(left) - (int)strlen(right);
        ob_fmt("%s", left);
        for (int i = 0; i < pad; i++) ob_str(" ");
        ob_fmt("%s", right);
    }
    ob_str("\033[m");

    /* cursor */
    if (cmdline) {
        ob_fmt("\033[%d;%dH", rows, (int)strlen(cmdline) + 1);
    } else {
        int sc = screen_col(cy, cx) + nw;
        if (sc >= cols) sc = cols - 1;
        ob_fmt("\033[%d;%dH", cy - top + 1, sc + 1);
    }
    write(1, OB, (size_t)OB_len);
}

/* ---------------- cursor clamping ---------------- */

/* In command mode the cursor sits ON a character, so the last legal
 * column is len-1; in insert mode it may sit one past the end. Getting
 * this wrong is how editors let you type past the end of a line. */
static void clamp(int insert) {
    if (B_n == 0) { cy = cx = 0; return; }
    if (cy < 0) cy = 0;
    if (cy >= B_n) cy = B_n - 1;
    int max = B[cy].len - (insert ? 0 : 1);
    if (max < 0) max = 0;
    if (cx > max) cx = max;
    if (cx < 0) cx = 0;
}

/* ---------------- character classes for word motions ---------------- */

static int cls(char c) {
    if (c == ' ' || c == '\t') return 0;
    if (isalnum((unsigned char)c) || c == '_') return 1;
    return 2;
}

static void mot_w(int big) {
    int start = cls(cx < B[cy].len ? B[cy].s[cx] : ' ');
    if (cx >= B[cy].len) { if (cy < B_n - 1) { cy++; cx = 0; } return; }
    if (big) {
        while (cx < B[cy].len && cls(B[cy].s[cx]) != 0) cx++;
    } else {
        while (cx < B[cy].len && cls(B[cy].s[cx]) == start && start != 0) cx++;
    }
    while (cx < B[cy].len && cls(B[cy].s[cx]) == 0) cx++;
    if (cx >= B[cy].len && cy < B_n - 1) { cy++; cx = 0;
        while (cx < B[cy].len && cls(B[cy].s[cx]) == 0) cx++; }
}

static void mot_b(int big) {
    if (cx == 0) {
        if (cy == 0) return;
        cy--; cx = B[cy].len;
    }
    while (cx > 0 && cls(B[cy].s[cx - 1]) == 0) cx--;
    if (cx == 0) return;
    int c0 = cls(B[cy].s[cx - 1]);
    while (cx > 0 && cls(B[cy].s[cx - 1]) != 0 &&
           (big || cls(B[cy].s[cx - 1]) == c0)) cx--;
}

static void mot_e(int big) {
    if (cx < B[cy].len) cx++;
    while (cx < B[cy].len && cls(B[cy].s[cx]) == 0) cx++;
    if (cx >= B[cy].len) {
        if (cy < B_n - 1) { cy++; cx = 0;
            while (cx < B[cy].len && cls(B[cy].s[cx]) == 0) cx++; }
        else { cx = B[cy].len ? B[cy].len - 1 : 0; return; }
    }
    int c0 = cls(B[cy].s[cx]);
    while (cx + 1 < B[cy].len && cls(B[cy].s[cx + 1]) != 0 &&
           (big || cls(B[cy].s[cx + 1]) == c0)) cx++;
}

static int mot_find(int ch, int dir, int till) {
    if (dir > 0) {
        for (int i = cx + 1; i < B[cy].len; i++)
            if (B[cy].s[i] == ch) { cx = till ? i - 1 : i; return 1; }
    } else {
        for (int i = cx - 1; i >= 0; i--)
            if (B[cy].s[i] == ch) { cx = till ? i + 1 : i; return 1; }
    }
    return 0;
}

static void mot_matchpair(void) {
    static const char *open = "([{", *close = ")]}";
    if (cx >= B[cy].len) return;
    char c = B[cy].s[cx];
    const char *o = strchr(open, c), *k = strchr(close, c);
    int dir, want, have;
    if (o) { dir = 1; have = c; want = close[o - open]; }
    else if (k) { dir = -1; have = c; want = open[k - close]; }
    else return;
    int depth = 0, y = cy, x = cx;
    for (;;) {
        if (x < 0) { if (--y < 0) return; x = B[y].len - 1; if (x < 0) continue; }
        else if (x >= B[y].len) { if (++y >= B_n) return; x = 0; if (B[y].len == 0) continue; }
        char ch = B[y].s[x];
        if (ch == have) depth++;
        else if (ch == want) { if (--depth == 0) { cy = y; cx = x; return; } }
        x += dir;
    }
}

/* ---------------- register + edits ---------------- */

static void reg_set(const char *s, int len, int linewise) {
    free(R_text);
    R_text = xrealloc(NULL, (size_t)len + 1);
    memcpy(R_text, s, (size_t)len);
    R_text[len] = '\0';
    R_len = len; R_linewise = linewise;
}

static void reg_set_lines(int from, int to) {
    int total = 0;
    for (int i = from; i <= to && i < B_n; i++) total += B[i].len + 1;
    char *t = xrealloc(NULL, (size_t)total + 1);
    int o = 0;
    for (int i = from; i <= to && i < B_n; i++) {
        memcpy(t + o, B[i].s, (size_t)B[i].len); o += B[i].len;
        t[o++] = '\n';
    }
    t[o] = '\0';
    free(R_text); R_text = t; R_len = o; R_linewise = 1;
}

static void del_lines(int from, int count) {
    if (from >= B_n) return;
    if (from + count > B_n) count = B_n - from;
    reg_set_lines(from, from + count - 1);
    for (int i = 0; i < count; i++) line_delete(from);
    g_dirty = 1;
    cy = from; if (cy >= B_n) cy = B_n - 1;
    cx = 0;
}

static void del_range_inline(int y, int from, int to) {
    if (from > to) { int t = from; from = to; to = t; }
    if (from < 0) from = 0;
    if (to > B[y].len) to = B[y].len;
    if (from >= to) return;
    reg_set(B[y].s + from, to - from, 0);
    memmove(B[y].s + from, B[y].s + to, (size_t)(B[y].len - to + 1));
    B[y].len -= (to - from);
    g_dirty = 1;
}

static void ins_char(int ch) {
    struct line *l = &B[cy];
    line_reserve(l, l->len + 2);
    memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx + 1));
    l->s[cx] = (char)ch;
    l->len++;
    cx++;
    g_dirty = 1;
}

static void split_line(void) {
    struct line *l = &B[cy];
    int tail = l->len - cx;
    char *t = xrealloc(NULL, (size_t)tail + 1);
    memcpy(t, l->s + cx, (size_t)tail); t[tail] = '\0';
    l->len = cx; l->s[cx] = '\0';
    int indent = 0;
    if (opt_autoindent) {
        while (indent < l->len && (l->s[indent] == ' ' || l->s[indent] == '\t')) indent++;
    }
    line_insert(cy + 1, "", 0);
    if (indent > 0) {
        line_reserve(&B[cy + 1], indent + tail + 1);
        memcpy(B[cy + 1].s, B[cy].s, (size_t)indent);
        memcpy(B[cy + 1].s + indent, t, (size_t)tail);
        B[cy + 1].len = indent + tail;
        B[cy + 1].s[B[cy + 1].len] = '\0';
    } else {
        line_set(cy + 1, t, tail);
    }
    free(t);
    cy++; cx = indent;
    g_dirty = 1;
}

static void join_lines(int count) {
    for (int k = 0; k < count; k++) {
        if (cy + 1 >= B_n) break;
        struct line *a = &B[cy], *b = &B[cy + 1];
        int bs = 0;
        while (bs < b->len && (b->s[bs] == ' ' || b->s[bs] == '\t')) bs++;
        int need_space = (a->len > 0 && b->len - bs > 0);
        line_reserve(a, a->len + (b->len - bs) + 2);
        int at = a->len;
        if (need_space) a->s[at++] = ' ';
        cx = at;
        memcpy(a->s + at, b->s + bs, (size_t)(b->len - bs));
        a->len = at + (b->len - bs);
        a->s[a->len] = '\0';
        line_delete(cy + 1);
        g_dirty = 1;
    }
}

static void do_paste(int after) {
    if (!R_text || R_len == 0) { errmsg("nothing in register"); return; }
    undo_push();
    if (R_linewise) {
        int at = cy + (after ? 1 : 0);
        int start = 0;
        int inserted = 0;
        for (int i = 0; i < R_len; i++) {
            if (R_text[i] == '\n') {
                line_insert(at + inserted, R_text + start, i - start);
                inserted++; start = i + 1;
            }
        }
        cy = at; cx = 0;
    } else {
        int at = cx + (after && B[cy].len ? 1 : 0);
        struct line *l = &B[cy];
        line_reserve(l, l->len + R_len + 1);
        memmove(l->s + at + R_len, l->s + at, (size_t)(l->len - at + 1));
        memcpy(l->s + at, R_text, (size_t)R_len);
        l->len += R_len;
        cx = at + R_len - 1;
    }
    g_dirty = 1;
    clamp(0);
}

/* ---------------- search ---------------- */

/* POSIX vi searches with BREs, and libtoby has a real engine, so this is
 * a genuine regex search rather than a substring scan wearing the name.
 * A bad pattern is reported, not silently treated as a literal. */
static int search(const char *pat, int dir, int from_y, int from_x, int *fy, int *fx) {
    regex_t re;
    int flags = opt_ignorecase ? REG_ICASE : 0;
    if (regcomp(&re, pat, flags) != 0) { errmsg("bad regex: %s", pat); return 0; }
    int y = from_y;
    for (int step = 0; step <= B_n; step++) {
        regmatch_t m;
        const char *s = B[y].s;
        int startx = 0;
        if (step == 0 && dir > 0) startx = from_x + 1;
        if (startx > B[y].len) { goto next; }
        if (dir > 0) {
            if (regexec(&re, s + startx, 1, &m, startx ? REG_NOTBOL : 0) == 0) {
                *fy = y; *fx = startx + (int)m.rm_so; regfree(&re); return 1;
            }
        } else {
            int best = -1, off = 0;
            while (off <= B[y].len) {
                if (regexec(&re, s + off, 1, &m, off ? REG_NOTBOL : 0) != 0) break;
                int abs = off + (int)m.rm_so;
                if (step == 0 && abs >= from_x) break;
                best = abs;
                off = abs + ((m.rm_eo > m.rm_so) ? (int)(m.rm_eo - m.rm_so) : 1);
            }
            if (best >= 0) { *fy = y; *fx = best; regfree(&re); return 1; }
        }
    next:
        y += dir;
        if (y >= B_n) y = 0;
        if (y < 0) y = B_n - 1;
    }
    regfree(&re);
    return 0;
}

static void search_next(int dir) {
    if (!g_lastpat[0]) { errmsg("no previous search"); return; }
    int fy, fx;
    if (search(g_lastpat, dir, cy, cx, &fy, &fx)) {
        cy = fy; cx = fx; clamp(0);
        msg("/%s", g_lastpat);
    } else {
        errmsg("Pattern not found: %s", g_lastpat);
    }
}

/* ---------------- :s substitution ---------------- */

static int subst_line(int y, regex_t *re, const char *rep, int global, int *changed) {
    char out[8192];
    int o = 0, pos = 0, n = 0;
    regmatch_t m[10];
    const char *s = B[y].s;
    while (pos <= B[y].len) {
        if (regexec(re, s + pos, 10, m, pos ? REG_NOTBOL : 0) != 0) break;
        int so = pos + (int)m[0].rm_so, eo = pos + (int)m[0].rm_eo;
        if (o + (so - pos) >= (int)sizeof out - 1) return -1;
        memcpy(out + o, s + pos, (size_t)(so - pos)); o += so - pos;
        /* replacement: & = whole match, \1..\9 = groups, \& = literal & */
        for (const char *r = rep; *r; r++) {
            int gi = -1;
            if (*r == '&') gi = 0;
            else if (*r == '\\' && r[1] >= '0' && r[1] <= '9') { gi = r[1] - '0'; r++; }
            else if (*r == '\\' && r[1]) { r++; if (o < (int)sizeof out - 1) out[o++] = *r; continue; }
            if (gi >= 0) {
                if (m[gi].rm_so < 0) continue;
                int gs = pos + (int)m[gi].rm_so, ge = pos + (int)m[gi].rm_eo;
                if (o + (ge - gs) >= (int)sizeof out - 1) return -1;
                memcpy(out + o, s + gs, (size_t)(ge - gs)); o += ge - gs;
            } else if (o < (int)sizeof out - 1) out[o++] = *r;
        }
        n++;
        if (eo == so) { if (so < B[y].len && o < (int)sizeof out - 1) out[o++] = s[so]; pos = so + 1; }
        else pos = eo;
        if (!global) break;
    }
    if (n == 0) return 0;
    if (pos < B[y].len) {
        if (o + (B[y].len - pos) >= (int)sizeof out - 1) return -1;
        memcpy(out + o, s + pos, (size_t)(B[y].len - pos)); o += B[y].len - pos;
    }
    out[o] = '\0';
    line_set(y, out, o);
    *changed += n;
    return n;
}

/* ---------------- ex command line ---------------- */

static int g_quit;

static void ex_write(const char *arg, int force, int *ok) {
    const char *path = (arg && *arg) ? arg : g_file;
    if (!path || !*path) { errmsg("No file name"); *ok = 0; return; }
    if (g_readonly && !force && path == g_file) {
        errmsg("'readonly' option is set (add ! to override)"); *ok = 0; return;
    }
    if (file_save(path) != 0) { errmsg("cannot write %s", path); *ok = 0; return; }
    if (!arg || !*arg || strcmp(path, g_file) == 0) { g_dirty = 0; g_nofile = 0; }
    msg("\"%s\" %d lines, %ld bytes written", path, B_n, file_bytes());
    *ok = 1;
}

/* Parse a line address: N, ., $, or empty. Returns -1 for "absent". */
static int ex_addr(const char **p) {
    const char *s = *p;
    while (*s == ' ') s++;
    int v = -1;
    if (*s == '.') { v = cy; s++; }
    else if (*s == '$') { v = B_n - 1; s++; }
    else if (isdigit((unsigned char)*s)) {
        v = 0;
        while (isdigit((unsigned char)*s)) v = v * 10 + (*s++ - '0');
        v -= 1;
    }
    if (v >= 0) {
        while (*s == '+' || *s == '-') {
            int sign = (*s == '+') ? 1 : -1; s++;
            int d = 0; while (isdigit((unsigned char)*s)) d = d * 10 + (*s++ - '0');
            if (d == 0) d = 1;
            v += sign * d;
        }
    }
    *p = s;
    return v;
}

static void ex_exec(const char *cmdbuf) {
    const char *p = cmdbuf;
    while (*p == ' ') p++;

    int a1 = -1, a2 = -1;
    if (*p == '%') { a1 = 0; a2 = B_n - 1; p++; }
    else {
        a1 = ex_addr(&p);
        if (*p == ',') { p++; a2 = ex_addr(&p); }
    }
    while (*p == ' ') p++;

    /* Bare address = goto line, which is what :5 and :$ mean. */
    if (*p == '\0') {
        if (a1 >= 0) {
            cy = a1 < 0 ? 0 : (a1 >= B_n ? B_n - 1 : a1);
            cx = 0; clamp(0);
        }
        return;
    }

    char cmd[64]; int ci = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '!' &&
           ci < (int)sizeof cmd - 1 &&
           (isalpha((unsigned char)*p) || (ci == 0 && (*p == 's' || *p == '&')))) {
        cmd[ci++] = *p++;
    }
    cmd[ci] = '\0';
    int force = 0;
    if (*p == '!') { force = 1; p++; }
    while (*p == ' ') p++;
    const char *arg = p;

    if (strcmp(cmd, "w") == 0 || strcmp(cmd, "write") == 0) {
        int ok; ex_write(arg, force, &ok);
    } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0 ||
               strcmp(cmd, "xit") == 0) {
        int ok; ex_write(arg, force, &ok);
        if (ok) g_quit = 1;
    } else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
        if (g_dirty && !force) {
            errmsg("No write since last change (add ! to override)");
        } else g_quit = 1;
    } else if (strcmp(cmd, "qa") == 0 || strcmp(cmd, "qall") == 0) {
        if (g_dirty && !force) errmsg("No write since last change (add ! to override)");
        else g_quit = 1;
    } else if (strcmp(cmd, "s") == 0 || strcmp(cmd, "substitute") == 0) {
        char delim = *arg;
        if (!delim || isalnum((unsigned char)delim)) { errmsg("bad :s"); return; }
        const char *q = arg + 1;
        char pat[256], rep[256]; int i = 0;
        while (*q && *q != delim && i < (int)sizeof pat - 1) {
            if (*q == '\\' && q[1] == delim) { pat[i++] = delim; q += 2; continue; }
            pat[i++] = *q++;
        }
        pat[i] = '\0';
        if (*q == delim) q++;
        i = 0;
        while (*q && *q != delim && i < (int)sizeof rep - 1) {
            if (*q == '\\' && q[1] == delim) { rep[i++] = delim; q += 2; continue; }
            rep[i++] = *q++;
        }
        rep[i] = '\0';
        if (*q == delim) q++;
        int global = 0;
        int icase = opt_ignorecase;
        for (; *q; q++) { if (*q == 'g') global = 1; else if (*q == 'i') icase = 1; }
        if (!pat[0]) { if (!g_lastpat[0]) { errmsg("no previous pattern"); return; }
                       snprintf(pat, sizeof pat, "%s", g_lastpat); }
        snprintf(g_lastpat, sizeof g_lastpat, "%s", pat);
        regex_t re;
        if (regcomp(&re, pat, icase ? REG_ICASE : 0) != 0) {
            errmsg("bad regex: %s", pat); return;
        }
        int from = (a1 >= 0) ? a1 : cy, to = (a2 >= 0) ? a2 : from;
        if (from < 0) from = 0;
        if (to >= B_n) to = B_n - 1;
        undo_push();
        int changed = 0, lines = 0, lastline = -1;
        for (int y = from; y <= to; y++) {
            int before = changed;
            if (subst_line(y, &re, rep, global, &changed) < 0) {
                errmsg("line too long after substitution"); regfree(&re); return;
            }
            if (changed > before) { lines++; lastline = y; }
        }
        regfree(&re);
        if (changed == 0) { errmsg("Pattern not found: %s", pat); return; }
        g_dirty = 1;
        if (lastline >= 0) { cy = lastline; cx = 0; }
        msg("%d substitution%s on %d line%s", changed, changed == 1 ? "" : "s",
            lines, lines == 1 ? "" : "s");
    } else if (strcmp(cmd, "d") == 0 || strcmp(cmd, "delete") == 0) {
        int from = (a1 >= 0) ? a1 : cy, to = (a2 >= 0) ? a2 : from;
        if (to >= B_n) to = B_n - 1;
        undo_push();
        del_lines(from, to - from + 1);
        msg("%d fewer lines", to - from + 1);
    } else if (strcmp(cmd, "set") == 0) {
        if (strcmp(arg, "number") == 0 || strcmp(arg, "nu") == 0) opt_number = 1;
        else if (strcmp(arg, "nonumber") == 0 || strcmp(arg, "nonu") == 0) opt_number = 0;
        else if (strcmp(arg, "ignorecase") == 0 || strcmp(arg, "ic") == 0) opt_ignorecase = 1;
        else if (strcmp(arg, "noignorecase") == 0 || strcmp(arg, "noic") == 0) opt_ignorecase = 0;
        else if (strcmp(arg, "autoindent") == 0 || strcmp(arg, "ai") == 0) opt_autoindent = 1;
        else if (strcmp(arg, "noautoindent") == 0 || strcmp(arg, "noai") == 0) opt_autoindent = 0;
        else if (strncmp(arg, "tabstop=", 8) == 0) {
            int t = atoi(arg + 8); if (t > 0 && t <= 16) opt_tabstop = t;
        } else errmsg("unknown option: %s", arg);
    } else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "read") == 0) {
        FILE *f = fopen(arg, "rb");
        if (!f) { errmsg("cannot open %s", arg); return; }
        undo_push();
        int at = ((a1 >= 0) ? a1 : cy) + 1;
        char lb[4096]; int li = 0, c, added = 0;
        while ((c = fgetc(f)) != EOF) {
            if (c == '\n') { lb[li] = '\0'; line_insert(at + added, lb, li); added++; li = 0; }
            else if (li < (int)sizeof lb - 1) lb[li++] = (char)c;
        }
        if (li) { lb[li] = '\0'; line_insert(at + added, lb, li); added++; }
        fclose(f);
        g_dirty = 1;
        msg("\"%s\" %d lines", arg, added);
    } else if (strcmp(cmd, "e") == 0 || strcmp(cmd, "edit") == 0) {
        if (g_dirty && !force) { errmsg("No write since last change (add ! to override)"); return; }
        const char *path = (*arg) ? arg : g_file;
        int rc = file_load(path);
        snprintf(g_file, sizeof g_file, "%s", path);
        g_nofile = 0; g_dirty = 0; cx = cy = top = 0;
        U_count = 0; U_head = 0; g_have_redo = 0;
        if (rc == 1) msg("\"%s\" [New File]", path);
        else msg("\"%s\" %d lines, %ld bytes", path, B_n, file_bytes());
    } else {
        errmsg("Not an editor command: %s", cmd);
    }
}

/* Read a : / ? line at the bottom of the screen. Returns 0 if cancelled. */
static int read_cmdline(char prefix, char *out, int outsz) {
    int n = 0;
    out[0] = '\0';
    for (;;) {
        char shown[STATUS_MAX];
        snprintf(shown, sizeof shown, "%c%s", prefix, out);
        draw(0, shown);
        int c = getkey();
        if (c < 0) return 0;
        if (c == 27) return 0;                       /* ESC cancels */
        if (c == '\r' || c == '\n') return 1;
        if (c == 127 || c == 8) {
            if (n == 0) return 0;                    /* backspace off the end cancels */
            out[--n] = '\0';
            continue;
        }
        if (c < 32) continue;
        if (n < outsz - 1) { out[n++] = (char)c; out[n] = '\0'; }
    }
}

/* ---------------- insert mode ---------------- */

static void insert_mode(int replace) {
    for (;;) {
        draw(1, NULL);
        int c = getkey();
        if (c < 0 || c == 27) break;
        if (c == '\r' || c == '\n') { split_line(); continue; }
        if (c == 127 || c == 8) {
            if (cx > 0) {
                struct line *l = &B[cy];
                memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx + 1));
                l->len--; cx--; g_dirty = 1;
            } else if (cy > 0) {
                int prev = B[cy - 1].len;
                struct line *a = &B[cy - 1], *b = &B[cy];
                line_reserve(a, a->len + b->len + 1);
                memcpy(a->s + a->len, b->s, (size_t)b->len + 1);
                a->len += b->len;
                line_delete(cy);
                cy--; cx = prev; g_dirty = 1;
            }
            continue;
        }
        if (c == '\t') { ins_char('\t'); continue; }
        if (c < 32) continue;
        if (replace && cx < B[cy].len) { B[cy].s[cx] = (char)c; cx++; g_dirty = 1; }
        else ins_char(c);
    }
    clamp(0);
}

/* ---------------- operators ---------------- */

/* Apply operator `op` over a motion. Returns 1 if it consumed the motion. */
static int apply_operator(int op, int count) {
    int c = getkey();
    if (c < 0) return 0;
    int cnt2 = 0;
    while (c >= '1' && c <= '9') {
        cnt2 = cnt2 * 10 + (c - '0');
        c = getkey();
        if (c < 0) return 0;
        while (c >= '0' && c <= '9') { cnt2 = cnt2 * 10 + (c - '0'); c = getkey(); }
    }
    if (cnt2 > 0) count *= cnt2;

    int sy = cy, sx = cx;

    /* doubled operator = line-wise over `count` lines (dd, cc, yy) */
    if (c == op) {
        if (op == 'y') { reg_set_lines(cy, cy + count - 1 < B_n ? cy + count - 1 : B_n - 1);
                         msg("%d lines yanked", count); return 1; }
        undo_push();
        if (op == 'c') {
            int keep = cy;
            del_lines(cy, count);
            line_insert(keep, "", 0);
            cy = keep; cx = 0;
            insert_mode(0);
        } else {
            del_lines(cy, count);
            clamp(0);
        }
        return 1;
    }

    switch (c) {
        /* POSIX vi's one famous irregularity: `cw` behaves like `ce`, so
         * it changes the WORD and leaves the whitespace after it, while
         * `dw` takes that whitespace with it. Treating them alike is what
         * turned "foo bar" into "bazbar" instead of "baz bar". */
        case 'w':
            if (op == 'c' && cx < B[cy].len && cls(B[cy].s[cx]) != 0) {
                for (int i = 0; i < count; i++) {
                    if (i) mot_w(0);
                    mot_e(0);
                }
                if (cx < B[cy].len) cx++;
            } else {
                for (int i = 0; i < count; i++) mot_w(0);
            }
            break;
        case 'W':
            if (op == 'c' && cx < B[cy].len && cls(B[cy].s[cx]) != 0) {
                for (int i = 0; i < count; i++) { if (i) mot_w(1); mot_e(1); }
                if (cx < B[cy].len) cx++;
            } else {
                for (int i = 0; i < count; i++) mot_w(1);
            }
            break;
        case 'b': for (int i = 0; i < count; i++) mot_b(0); break;
        case 'B': for (int i = 0; i < count; i++) mot_b(1); break;
        case 'e': for (int i = 0; i < count; i++) mot_e(0); if (cx < B[cy].len) cx++; break;
        case '$': cx = B[cy].len; break;
        case '0': cx = 0; break;
        case '^': { int i = 0; while (i < B[cy].len && (B[cy].s[i]==' '||B[cy].s[i]=='\t')) i++;
                    cx = i; break; }
        case 'f': case 'F': case 't': case 'T': {
            int ch = getkey(); if (ch < 0) return 0;
            int dir = (c == 'f' || c == 't') ? 1 : -1;
            if (!mot_find(ch, dir, (c == 't' || c == 'T'))) return 1;
            if (dir > 0) cx++;
            break;
        }
        case 'G': {
            int target = (cnt2 > 0) ? cnt2 - 1 : B_n - 1;
            if (target < 0) target = 0;
            if (target >= B_n) target = B_n - 1;
            int from = sy < target ? sy : target, to = sy < target ? target : sy;
            undo_push();
            if (op == 'y') { reg_set_lines(from, to); return 1; }
            del_lines(from, to - from + 1);
            if (op == 'c') { line_insert(from, "", 0); cy = from; cx = 0; insert_mode(0); }
            clamp(0);
            return 1;
        }
        default:
            return 1;   /* unknown motion: operator aborts, as vi does */
    }

    /* character-wise operator between (sy,sx) and (cy,cx) */
    if (cy != sy) {
        int from = sy < cy ? sy : cy, to = sy < cy ? cy : sy;
        undo_push();
        if (op == 'y') { reg_set_lines(from, to); cy = from; cx = sx; return 1; }
        del_lines(from, to - from + 1);
        if (op == 'c') { line_insert(from, "", 0); cy = from; cx = 0; insert_mode(0); }
        clamp(0);
        return 1;
    }
    int from = sx < cx ? sx : cx, to = sx < cx ? cx : sx;
    if (op == 'y') {
        reg_set(B[sy].s + from, to - from, 0);
        cy = sy; cx = from;
        return 1;
    }
    undo_push();
    cy = sy;
    del_range_inline(sy, from, to);
    cx = from;
    if (op == 'c') { clamp(1); insert_mode(0); }
    else clamp(0);
    return 1;
}

/* ---------------- main loop ---------------- */

static void usage(void) {
    printf("usage: tvi [-R] [+N] [file]\n");
    printf("  -R   read-only\n");
    printf("  +N   start on line N\n");
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int startline = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-R") == 0) g_readonly = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(); return 0;
        } else if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1])) {
            startline = atoi(argv[i] + 1) - 1;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "tvi: unknown option %s\n", argv[i]);
            return 2;
        } else path = argv[i];
    }

    buf_reserve(256);
    B_n = 1; line_set(0, "", 0);

    if (path) {
        snprintf(g_file, sizeof g_file, "%s", path);
        int rc = file_load(path);
        if (rc == 1) msg("\"%s\" [New File]", path);
        else msg("\"%s\" %d lines, %ld bytes", path, B_n, file_bytes());
    } else {
        g_nofile = 1;
        msg("tvi -- native vi for tobyOS. :q to quit, :help is not a thing.");
    }
    if (startline > 0) { cy = startline < B_n ? startline : B_n - 1; }

    raw_on();
    term_size();

    int count = 0;
    while (!g_quit) {
        clamp(0);
        draw(0, NULL);
        int c = getkey();
        if (c < 0) break;
        g_msg[0] = '\0';

        /* counts */
        if (c >= '1' && c <= '9') {
            count = c - '0';
            for (;;) {
                c = getkey();
                if (c < 0) { g_quit = 1; break; }
                if (c >= '0' && c <= '9') count = count * 10 + (c - '0');
                else break;
            }
            if (g_quit) break;
        }
        int n = count > 0 ? count : 1;

        switch (c) {
        /* ---- motion ---- */
        case 'h': case 8:   cx -= n; clamp(0); want_col = cx; break;
        case 'l': case ' ': cx += n; clamp(0); want_col = cx; break;
        case 'j': cy += n; clamp(0); cx = want_col; clamp(0); break;
        case 'k': cy -= n; clamp(0); cx = want_col; clamp(0); break;
        case '0': cx = 0; want_col = cx; break;
        case '$': cx = B[cy].len ? B[cy].len - 1 : 0; want_col = 1 << 30; break;
        case '^': { int i = 0; while (i < B[cy].len && (B[cy].s[i]==' '||B[cy].s[i]=='\t')) i++;
                    cx = i; want_col = cx; break; }
        case 'w': for (int i = 0; i < n; i++) mot_w(0); want_col = cx; break;
        case 'W': for (int i = 0; i < n; i++) mot_w(1); want_col = cx; break;
        case 'b': for (int i = 0; i < n; i++) mot_b(0); want_col = cx; break;
        case 'B': for (int i = 0; i < n; i++) mot_b(1); want_col = cx; break;
        case 'e': for (int i = 0; i < n; i++) mot_e(0); want_col = cx; break;
        case 'E': for (int i = 0; i < n; i++) mot_e(1); want_col = cx; break;
        case 'G': cy = (count > 0) ? count - 1 : B_n - 1; cx = 0; clamp(0); break;
        case 'g': { int c2 = getkey(); if (c2 == 'g') { cy = (count>0)?count-1:0; cx = 0; } break; }
        case 'H': cy = top; clamp(0); break;
        case 'M': cy = top + (rows - 1) / 2; clamp(0); break;
        case 'L': cy = top + rows - 2; clamp(0); break;
        case '%': mot_matchpair(); break;
        case 6:  cy += rows - 2; top += rows - 2; clamp(0); break;   /* ^F */
        case 2:  cy -= rows - 2; top -= rows - 2; clamp(0); break;   /* ^B */
        case 4:  cy += (rows - 1) / 2; clamp(0); break;              /* ^D */
        case 21: cy -= (rows - 1) / 2; clamp(0); break;              /* ^U */
        case 'f': case 'F': case 't': case 'T': {
            int ch = getkey(); if (ch < 0) break;
            g_lastft = (char)c; g_lastftchar = (char)ch;
            for (int i = 0; i < n; i++)
                mot_find(ch, (c=='f'||c=='t') ? 1 : -1, (c=='t'||c=='T'));
            break;
        }
        case ';': if (g_lastft) for (int i = 0; i < n; i++)
                      mot_find(g_lastftchar, (g_lastft=='f'||g_lastft=='t')?1:-1,
                               (g_lastft=='t'||g_lastft=='T'));
                  break;
        case ',': if (g_lastft) for (int i = 0; i < n; i++)
                      mot_find(g_lastftchar, (g_lastft=='f'||g_lastft=='t')?-1:1,
                               (g_lastft=='t'||g_lastft=='T'));
                  break;

        /* ---- insert entry ---- */
        case 'i': undo_push(); insert_mode(0); break;
        case 'I': undo_push(); { int i=0; while (i<B[cy].len && (B[cy].s[i]==' '||B[cy].s[i]=='\t')) i++;
                    cx = i; } insert_mode(0); break;
        case 'a': undo_push(); if (B[cy].len) cx++; insert_mode(0); break;
        case 'A': undo_push(); cx = B[cy].len; insert_mode(0); break;
        case 'o': undo_push(); line_insert(cy + 1, "", 0); cy++; cx = 0;
                  g_dirty = 1; insert_mode(0); break;
        case 'O': undo_push(); line_insert(cy, "", 0); cx = 0;
                  g_dirty = 1; insert_mode(0); break;
        case 'R': undo_push(); insert_mode(1); break;

        /* ---- edits ---- */
        case 'x': if (B[cy].len) { undo_push();
                      int to = cx + n; if (to > B[cy].len) to = B[cy].len;
                      del_range_inline(cy, cx, to); clamp(0); } break;
        case 'X': if (cx > 0) { undo_push();
                      int from = cx - n; if (from < 0) from = 0;
                      del_range_inline(cy, from, cx); cx = from; clamp(0); } break;
        case 's': undo_push(); { int to = cx + n; if (to > B[cy].len) to = B[cy].len;
                    del_range_inline(cy, cx, to); } clamp(1); insert_mode(0); break;
        case 'S': undo_push(); { int keep = cy; del_lines(cy, n);
                    line_insert(keep, "", 0); cy = keep; cx = 0; } insert_mode(0); break;
        case 'D': undo_push(); del_range_inline(cy, cx, B[cy].len); clamp(0); break;
        case 'C': undo_push(); del_range_inline(cy, cx, B[cy].len); clamp(1);
                  insert_mode(0); break;
        case 'Y': reg_set_lines(cy, cy + n - 1 < B_n ? cy + n - 1 : B_n - 1);
                  msg("%d lines yanked", n); break;
        case 'J': undo_push(); join_lines(n > 1 ? n - 1 : 1); break;
        case 'r': { int ch = getkey(); if (ch < 0) break;
                    if (cx + n <= B[cy].len) { undo_push();
                        for (int i = 0; i < n; i++) B[cy].s[cx + i] = (char)ch;
                        cx += n - 1; g_dirty = 1; } break; }
        case '~': if (cx < B[cy].len) { undo_push();
                      for (int i = 0; i < n && cx < B[cy].len; i++, cx++) {
                          char ch = B[cy].s[cx];
                          B[cy].s[cx] = isupper((unsigned char)ch) ? (char)tolower((unsigned char)ch)
                                      : (char)toupper((unsigned char)ch);
                      }
                      g_dirty = 1; clamp(0); } break;
        case 'd': case 'c': case 'y': apply_operator(c, n); break;
        case 'p': do_paste(1); break;
        case 'P': do_paste(0); break;
        case 'u': do_undo(); break;
        case 18: do_redo(); break;      /* ^R */

        /* ---- search ---- */
        case '/': case '?': {
            char pat[256];
            if (read_cmdline((char)c, pat, sizeof pat) && pat[0]) {
                snprintf(g_lastpat, sizeof g_lastpat, "%s", pat);
                g_lastdir = (c == '/') ? 1 : -1;
                search_next(g_lastdir);
            }
            break;
        }
        case 'n': search_next(g_lastdir); break;
        case 'N': search_next(-g_lastdir); break;

        /* ---- ex ---- */
        case ':': {
            char cmdbuf[STATUS_MAX];
            if (read_cmdline(':', cmdbuf, sizeof cmdbuf) && cmdbuf[0])
                ex_exec(cmdbuf);
            break;
        }

        case 'Z': { int c2 = getkey();
                    if (c2 == 'Z') { int ok; ex_write(NULL, 0, &ok); if (ok) g_quit = 1; }
                    else if (c2 == 'Q') g_quit = 1;
                    break; }
        case 12: term_size(); break;    /* ^L: re-read size and repaint */
        case 7:  msg("\"%s\" %d lines --%d%%--", g_nofile ? "[No Name]" : g_file,
                     B_n, B_n ? (cy + 1) * 100 / B_n : 0); break;   /* ^G */
        case 27: break;
        default: break;
        }
        count = 0;
    }

    raw_off();
    return 0;
}
