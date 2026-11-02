/* lineedit.c -- the readline subset for /bin/tsh. See lineedit.h.
 *
 * NO FIXED SIZES (batch 13). The line was 4096 bytes, the cut buffer and the
 * parked history line the same, a completion list 256 names of 128 bytes,
 * the word being completed 255: past any of them the editor silently
 * dropped input or candidates. Every one of them grows now.
 *
 * AND CHARACTERS, NOT BYTES. In a UTF-8 locale (the shell's, as bash's is --
 * shell_utf8_hosted) a typed multibyte character is read whole and inserted
 * whole -- every byte past 0x7f used to be thrown away, so nothing outside
 * ASCII could be typed at all -- the cursor moves, deletes and transposes a
 * character at a time, and the redraw counts COLUMNS by musl's wcwidth, so
 * a CJK character takes two and a combining one none.
 *
 * A LINE WIDER THAN THE TERMINAL WRAPS, and the redraw follows it across
 * rows (refresh). It was drawn as one row, so past the right margin the
 * cursor arithmetic was wrong and every repaint left a trail.
 *
 * AND BOTH OF READLINE'S MODES. `set -o vi` chose nothing here: the editor
 * was emacs-only. vi insert and command modes now follow readline's vi
 * keymaps (lib/readline/vi_keymap.c): counts, motions, the d/c/y operators,
 * x X r R s S ~ p P u U . _ #, history by j k G / ? n N, marks. Emacs mode
 * gained what it lacked of readline's common commands: undo, revert, the
 * kill ring, yank-last-arg, case and transpose of words, character search,
 * quoted insert, insert-comment, operate-and-get-next.
 *
 * Everything drawn goes to standard error, as readline's output does in
 * bash (bashline.c). */
#include "lineedit.h"
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <tobyos/wctab.h>
#ifdef __linux__
#include <sys/ioctl.h>                  /* the host test harness's build */
#endif

/* Provided by src/shell.c: bash's programmable completion for the line
 * being edited (shell_complete.inc), and the builtin names hosted tsh will
 * run. */
#include <stdbool.h>
bool tsh_progcomp(const char *line, size_t point, const char *word,
                  char ***out, int *nout, unsigned long *opts);
void tsh_progcomp_free(char **v, int n);
const char *tsh_builtin_name_at(int i);

extern const char *shell_get_var_hosted(const char *name);
extern int         shell_history_count_hosted(void);
extern const char *shell_history_get_hosted(int i);
extern bool        shell_utf8_hosted(void);
extern bool        shell_opt_vi_hosted(void);
extern void        shell_set_editing_mode_hosted(bool vi);
extern unsigned long shell_history_base_hosted(void);

struct le_snap { char *buf; size_t pos; };

struct le {
    char        *buf;            /* *bufp: grows */
    size_t       cap;
    char       **bufp;
    size_t      *capp;
    size_t       len, pos;
    const char  *prompt;
    size_t       plen;
    int          hist;           /* == count while on the live line */
    int          painted;        /* has anything been drawn for this line yet */
    char        *saved;          /* live line, parked while browsing history */
    bool         u8;             /* the shell's locale is UTF-8 */
    int          crow;           /* the cursor's row, from the prompt's first */
    struct le_snap *undo;        /* readline's undo list, as whole-line states */
    int          nundo, capundo;
    char        *orig;           /* the line as it was fetched: revert-line */
    int          prev;           /* the command before this one (kills join) */
    bool         vionce;         /* vi command mode entered for this line */
    size_t       mark;           /* emacs: set-mark */
    int          last;           /* the last command, for kill/yank chaining */
    bool         vi;             /* editing-mode vi */
    bool         vcmd;           /* vi: command (movement) mode */
    bool         vrepl;          /* vi: R, overwrite */
};

enum { LC_OTHER, LC_INSERT, LC_KILL_F, LC_KILL_B, LC_YANK };

/* ---- output and input ------------------------------------------------- */

/* EVERYTHING THE EDITOR DRAWS GOES TO STANDARD ERROR -- bash sets readline's
 * output stream to stderr (bashline.c), so `tsh -i >file` still shows the
 * prompt and the line being typed. */
/* ...and over a pipe it draws nothing as it goes: an interactive shell
 * reading a pipe edits its lines as bash's readline does there (^R, ^O,
 * ^P), and what shows is the prompt and the line it accepted, once, the
 * echo the plain reader gave (tsh_readline_pipe). */
static bool g_le_pipe;
static void out(const char *s, size_t n) { if (!g_le_pipe) (void)!write(2, s, n); }
static void outs(const char *s) { out(s, strlen(s)); }
static void bell(void) { out("\a", 1); }

/* vi's `.` replays the keys of the last CHANGE. While RECORDING, every key
 * read is kept in g_rec, the command being read; a change that completes
 * -- at once, or at the ESC that ends its insertion -- becomes g_chg
 * (vi_commit). While REPLAYING the keys come from g_rep. */
static char  *g_rec, *g_rep, *g_chg;
static size_t g_recn, g_recc, g_repn, g_repi, g_chgn;
static bool   g_recording, g_replaying;

static void rec_key(char c) {
    if (!g_recording || g_replaying) return;
    if (g_recn + 1 >= g_recc) {
        size_t nc = g_recc ? g_recc * 2 : 64;
        char *n = (char *)realloc(g_rec, nc);
        if (!n) return;
        g_rec = n;
        g_recc = nc;
    }
    g_rec[g_recn++] = c;
}

static void vi_commit(void) {
    if (!g_recording || g_replaying) { g_recording = false; return; }
    g_recording = false;
    char *n = (char *)malloc(g_recn ? g_recn : 1);
    if (!n) return;
    memcpy(n, g_rec, g_recn);
    free(g_chg);
    g_chg = n;
    g_chgn = g_recn;
}

/* A key read too soon -- the one after an ESC -- given back. */
static int g_pushback = -1;

/* One key: -1 at end of input. */
extern volatile int g_tsh_async_ok, g_tsh_le_async;

static int read_key(void) {
    if (g_pushback >= 0) {
        int k = g_pushback;
        g_pushback = -1;
        rec_key((char)k);
        return k;
    }
    if (g_replaying) {
        if (g_repi < g_repn) return (unsigned char)g_rep[g_repi++];
        g_replaying = false;
        return -1;
    }
    char c;
    /* `set -b` may report a job while this read waits (host.c) */
    g_tsh_async_ok = g_tsh_le_async;
    long r = read(0, &c, 1);
    g_tsh_async_ok = 0;
    if (r <= 0) return -1;
    rec_key(c);
    return (unsigned char)c;
}

/* Bytes already waiting on the terminal: what tells ESC from the start of
 * an arrow key's ESC [ A, which arrives in one write. */
static int keys_pending(void) {
    if (g_replaying) return g_repi < g_repn;
    int n = 0;
    for (int spin = 0; spin < 2000; spin++) {
        if (ioctl(0, 0x541BUL /* FIONREAD */, &n) != 0) return 0;
        if (n > 0) return n;
    }
    return 0;
}

/* ---- characters --------------------------------------------------------- */

/* The bytes of the character at P (AVAIL bytes there) and its code point --
 * a valid UTF-8 sequence when U8, else one byte. */
static size_t le_dec(const char *p, size_t avail, bool u8, uint32_t *cp) {
    const unsigned char *s = (const unsigned char *)p;
    if (!avail) { *cp = 0; return 0; }
    unsigned c = s[0];
    *cp = c;
    if (!u8 || c < 0x80) return 1;
    size_t n;
    uint32_t v;
    unsigned lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; v = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) {
        n = 3; v = c & 0x0F;
        if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4; v = c & 0x07;
        if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F;
    } else return 1;
    if (avail < n) return 1;
    for (size_t k = 1; k < n; k++) {
        unsigned b = s[k];
        if (b < lo || b > hi) return 1;
        lo = 0x80; hi = 0xBF;
        v = (v << 6) | (b & 0x3F);
    }
    *cp = v;
    return n;
}

static size_t le_next(const struct le *s, size_t at) {
    uint32_t cp;
    return at < s->len ? at + le_dec(s->buf + at, s->len - at, s->u8, &cp) : at;
}

/* The start of the character before AT. */
static size_t le_prev(const struct le *s, size_t at) {
    if (at == 0) return 0;
    if (!s->u8) return at - 1;
    size_t q = at - 1;
    while (q > 0 && at - q < 4 && ((unsigned char)s->buf[q] & 0xC0) == 0x80) q--;
    uint32_t cp;
    if (q + le_dec(s->buf + q, s->len - q, true, &cp) == at) return q;
    return at - 1;
}

static uint32_t le_cp(const struct le *s, size_t at) {
    uint32_t cp = 0;
    if (at < s->len) (void)le_dec(s->buf + at, s->len - at, s->u8, &cp);
    return cp;
}

/* Display columns of the bytes [p, p+n). */
static size_t le_cols(const char *p, size_t n, bool u8) {
    size_t cols = 0, i = 0;
    while (i < n) {
        uint32_t cp;
        size_t l = le_dec(p + i, n - i, u8, &cp);
        if (cp >= 0x80 && u8 && l > 1) {
            int w = wc_width(cp);
            cols += w < 0 ? 1 : (size_t)w;
        } else {
            cols++;
        }
        i += l;
    }
    return cols;
}

/* A prompt's columns: an escape sequence -- a colour, a title -- takes none. */
static size_t le_prompt_cols(const char *p, size_t n, bool u8) {
    size_t cols = 0, i = 0;
    while (i < n) {
        if (p[i] == 27) {
            i++;
            if (i < n && p[i] == '[') {
                i++;
                while (i < n && !((unsigned char)p[i] >= 0x40 && (unsigned char)p[i] <= 0x7e)) i++;
                if (i < n) i++;
            } else if (i < n && p[i] == ']') {
                while (i < n && p[i] != 7 && !(p[i] == 27 && i + 1 < n && p[i + 1] == '\\')) i++;
                if (i < n) i += p[i] == 7 ? 1 : 2;
            } else if (i < n) {
                i++;
            }
            continue;
        }
        if (p[i] == 1 || p[i] == 2) { i++; continue; }
        uint32_t cp;
        size_t l = le_dec(p + i, n - i, u8, &cp);
        cols += le_cols(p + i, l, u8);
        i += l;
    }
    return cols;
}

/* Character classes: readline's alphabetic (alnum) for the emacs word
 * commands, vi's word (alnum and _) and blank. */
static bool le_alnum(uint32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return wc_class(WC_ALNUM, c) != 0;
}
static bool le_blank(uint32_t c) { return c == ' ' || c == '\t'; }
static bool vi_word(uint32_t c) { return le_alnum(c) || c == '_'; }

/* ---- the buffer ----------------------------------------------------------- */

static int le_room(struct le *s, size_t need) {
    if (need + 1 <= s->cap) return 1;
    size_t nc = s->cap ? s->cap : 256;
    while (nc < need + 1) nc *= 2;
    char *nb = (char *)realloc(s->buf, nc);
    if (!nb) return 0;
    s->buf = nb;
    s->cap = nc;
    *s->bufp = nb;
    *s->capp = nc;
    return 1;
}

/* ---- display ----------------------------------------------------------------- */

static size_t le_term_cols(void) {
    struct { unsigned short row, col, x, y; } ws;
    if (ioctl(2, 0x5413UL /* TIOCGWINSZ */, &ws) == 0 && ws.col > 0) return ws.col;
    if (ioctl(0, 0x5413UL, &ws) == 0 && ws.col > 0) return ws.col;
    const char *c = shell_get_var_hosted("COLUMNS");
    if (c && atoi(c) > 0) return (size_t)atoi(c);
    return 80;
}

static size_t emit_num(char *o, char final, size_t n) {
    return (size_t)snprintf(o, 24, "%c[%u%c", 27, (unsigned)n, final);
}

/* Where the end of the text leaves the cursor, from the prompt's first row,
 * and whether it sits exactly on the right margin (TC a multiple of W). */
static size_t le_end_row(size_t tc, size_t w) {
    if (tc == 0) return 0;
    return tc % w == 0 ? tc / w - 1 : tc / w;
}

/* Repaint: up to the prompt's first row, CR, clear to the end of the screen,
 * prompt and buffer, then the cursor to its row and column. ONE write per
 * refresh -- on a 38400-baud console a per-character redraw is visibly slow
 * and interleaves badly with anything else printing.
 *
 * FIRST paint of a line: the prompt and buffer plainly, with no CR and no
 * erase -- nothing is drawn yet, as readline does too -- because the
 * interactive parity gate frames its protocol on PROMPT SENTINELS: wrapping
 * a fresh prompt as "\r" + prompt + ESC[J makes those bytes stop matching,
 * the harness waits for a prompt it never recognises, and SIGKILLs tsh
 * (ttyparity 11/11 to 0/11 BROKEN once). */
static void refresh(struct le *s) {
    size_t w = le_term_cols();
    size_t pc = le_prompt_cols(s->prompt, s->plen, s->u8);
    size_t tc = pc + le_cols(s->buf, s->len, s->u8);
    size_t cc = pc + le_cols(s->buf, s->pos, s->u8);
    size_t need = s->plen + s->len + 96;
    char *o = (char *)malloc(need);
    if (!o) return;
    size_t k = 0;
    if (s->painted) {
        if (s->crow > 0) k += emit_num(o + k, 'A', (size_t)s->crow);
        o[k++] = '\r';
        o[k++] = 27; o[k++] = '['; o[k++] = 'J';
    }
    s->painted = 1;
    memcpy(o + k, s->prompt, s->plen); k += s->plen;
    memcpy(o + k, s->buf, s->len); k += s->len;
    size_t endrow = le_end_row(tc, w);
    /* at the right margin with the cursor at the end: move it onto the next
     * row now, where the next character would go */
    if (tc && tc % w == 0 && s->pos == s->len) {
        o[k++] = '\r'; o[k++] = '\n';
        endrow = tc / w;
    }
    size_t crow = cc / w, ccol = cc % w;
    /* with the cursor at the end it is already there: an untouched
     * prompt's bytes stay prompt and text alone */
    if (s->pos < s->len) {
        if (endrow > crow) k += emit_num(o + k, 'A', endrow - crow);
        o[k++] = '\r';
        if (ccol) k += emit_num(o + k, 'C', ccol);
    }
    out(o, k);
    free(o);
    s->crow = (int)crow;
}

/* The cursor onto the row after the text, column 0: before the line is
 * accepted, or a list is printed under it. */
static void le_below(struct le *s) {
    size_t w = le_term_cols();
    size_t pc = le_prompt_cols(s->prompt, s->plen, s->u8);
    size_t tc = pc + le_cols(s->buf, s->len, s->u8);
    size_t endrow = le_end_row(tc, w);
    if (tc && tc % w == 0 && s->pos == s->len) endrow = tc / w;
    char o[32];
    size_t k = 0;
    if (endrow > (size_t)s->crow) k += emit_num(o, 'B', endrow - (size_t)s->crow);
    if (k) out(o, k);
    if (tc && tc % w == 0 && s->pos == s->len) outs("\r");
    else outs("\r\n");
    s->crow = 0;
}

/* ---- editing primitives ---------------------------------------------------- */

static void set_line(struct le *s, const char *t) {
    size_t n = t ? strlen(t) : 0;
    if (!le_room(s, n)) return;
    if (n) memcpy(s->buf, t, n);
    s->buf[n] = 0;
    s->len = s->pos = n;
}

static void insert_n(struct le *s, const char *c, size_t n) {
    if (!n || !le_room(s, s->len + n)) return;
    memmove(s->buf + s->pos + n, s->buf + s->pos, s->len - s->pos);
    memcpy(s->buf + s->pos, c, n);
    s->pos += n;
    s->len += n;
    s->buf[s->len] = 0;
}

static void insert(struct le *s, char c) { insert_n(s, &c, 1); }

/* The kill ring: ten kills, the newest first; consecutive kills grow one
 * entry, in the direction they cut (readline). */
static char *g_kill[10];
static size_t g_killn;          /* entries in use */
static size_t g_killi;          /* yank-pop's position */
static size_t g_yank_from, g_yank_to;

static void kill_push(const char *t, size_t n, int dir, int last, bool vi) {
    bool join = (last == LC_KILL_F || last == LC_KILL_B) && g_killn > 0;
    if (join && vi) {
        /* vi mode: a kill right after a kill takes the same slot over */
        char *e = (char *)malloc(n + 1);
        if (!e) return;
        memcpy(e, t, n);
        e[n] = 0;
        free(g_kill[0]);
        g_kill[0] = e;
        return;
    }
    if (join) {
        size_t ol = strlen(g_kill[0]);
        char *j = (char *)malloc(ol + n + 1);
        if (!j) return;
        if (dir > 0) { memcpy(j, g_kill[0], ol); memcpy(j + ol, t, n); }
        else { memcpy(j, t, n); memcpy(j + n, g_kill[0], ol); }
        j[ol + n] = 0;
        free(g_kill[0]);
        g_kill[0] = j;
        return;
    }
    char *e = (char *)malloc(n + 1);
    if (!e) return;
    memcpy(e, t, n);
    e[n] = 0;
    free(g_kill[9]);
    memmove(g_kill + 1, g_kill, 9 * sizeof g_kill[0]);
    g_kill[0] = e;
    if (g_killn < 10) g_killn++;
}

/* Delete [from, to); KILL >0 cuts it forward onto the ring, <0 backward. */
static void del_range(struct le *s, size_t from, size_t to, int kill) {
    if (to > s->len) to = s->len;
    if (to <= from) return;
    if (kill) {
        kill_push(s->buf + from, to - from, kill, s->prev, s->vi);
        s->last = kill > 0 ? LC_KILL_F : LC_KILL_B;
    }
    memmove(s->buf + from, s->buf + to, s->len - to);
    s->len -= (to - from);
    s->buf[s->len] = 0;
    if (s->pos >= to)       s->pos -= (to - from);
    else if (s->pos > from) s->pos = from;
    if (s->pos > s->len) s->pos = s->len;
}

/* ---- undo ------------------------------------------------------------------ */

static void le_snap(struct le *s) {
    if (s->nundo > 0) {
        struct le_snap *t = &s->undo[s->nundo - 1];
        if (t->pos == s->pos && strlen(t->buf) == s->len &&
            memcmp(t->buf, s->buf, s->len) == 0)
            return;
    }
    if (s->nundo == s->capundo) {
        int nc = s->capundo ? s->capundo * 2 : 16;
        struct le_snap *n = (struct le_snap *)realloc(s->undo, (size_t)nc * sizeof *n);
        if (!n) return;
        s->undo = n;
        s->capundo = nc;
    }
    char *b = (char *)malloc(s->len + 1);
    if (!b) return;
    memcpy(b, s->buf, s->len);
    b[s->len] = 0;
    s->undo[s->nundo].buf = b;
    s->undo[s->nundo].pos = s->pos;
    s->nundo++;
}

static void le_undo_clear(struct le *s) {
    for (int i = 0; i < s->nundo; i++) free(s->undo[i].buf);
    s->nundo = 0;
}

static bool le_undo(struct le *s) {
    if (s->nundo == 0) return false;
    struct le_snap *t = &s->undo[--s->nundo];
    set_line(s, t->buf);
    s->pos = t->pos <= s->len ? t->pos : s->len;
    free(t->buf);
    return true;
}

static void le_set_orig(struct le *s) {
    free(s->orig);
    s->orig = strdup(s->buf);
    le_undo_clear(s);
}

/* ---- words --------------------------------------------------------------- */

/* unix-word-rubout's word: blank-delimited */
static size_t word_left_blank(struct le *s) {
    size_t p = s->pos;
    while (p > 0 && le_blank((unsigned char)s->buf[p - 1])) p--;
    while (p > 0 && !le_blank((unsigned char)s->buf[p - 1])) p--;
    return p;
}

/* readline's forward-word/backward-word: alphanumeric runs */
static size_t word_right(struct le *s, size_t p) {
    while (p < s->len && !le_alnum(le_cp(s, p))) p = le_next(s, p);
    while (p < s->len && le_alnum(le_cp(s, p))) p = le_next(s, p);
    return p;
}

static size_t word_left(struct le *s, size_t p) {
    while (p > 0 && !le_alnum(le_cp(s, le_prev(s, p)))) p = le_prev(s, p);
    while (p > 0 && le_alnum(le_cp(s, le_prev(s, p)))) p = le_prev(s, p);
    return p;
}

/* ---- history ------------------------------------------------------------- */

/* The half-typed line is parked in s->saved so walking up and back down
 * returns it intact -- losing it is the single most irritating way to get
 * history wrong. */
static void history_go(struct le *s, int want) {
    int count = shell_history_count_hosted();
    if (count <= 0) return;
    if (s->hist >= count) {
        char *sv = (char *)malloc(s->len + 1);
        if (!sv) return;
        memcpy(sv, s->buf, s->len);
        sv[s->len] = 0;
        free(s->saved);
        s->saved = sv;
    }
    if (want < 0) want = 0;
    if (want > count) want = count;
    if (want == s->hist) return;
    s->hist = want;
    set_line(s, want == count ? (s->saved ? s->saved : "")
                              : shell_history_get_hosted(want));
    le_set_orig(s);
}

static void history_move(struct le *s, int dir) {
    int count = shell_history_count_hosted();
    if (count <= 0) { bell(); return; }
    int want = s->hist + dir;
    if (want < 0 || want > count) { bell(); return; }
    history_go(s, want);
}

/* ^R and ^S, incremental search. 1 = accept and run, 0 = accept and keep
 * editing, -1 = interrupted. DIR -1 searches older lines. */
static int isearch(struct le *s, int dir) {
    char *q = (char *)calloc(1, 1);
    size_t qn = 0;
    if (!q) return 0;
    int found = -1;
    int rc;
    for (;;) {
        size_t dl = qn + 40;
        char *disp = (char *)malloc(dl);
        if (!disp) { rc = 0; break; }
        snprintf(disp, dl, "(%si-search)`%s': ", dir < 0 ? "reverse-" : "", q);
        /* the search shows on a scratch copy of the editor */
        struct le tmp = *s;
        char *tb = 0;
        size_t tc = 0;
        tmp.buf = 0; tmp.cap = 0; tmp.bufp = &tb; tmp.capp = &tc;
        tmp.len = tmp.pos = 0;
        tmp.prompt = disp;
        tmp.plen = strlen(disp);
        set_line(&tmp, found >= 0 ? shell_history_get_hosted(found) : s->buf);
        refresh(&tmp);
        s->crow = tmp.crow;
        free(tmp.buf);
        free(disp);

        int c = read_key();
        if (c < 0) { rc = 0; break; }
        if (c == 3 || c == 7) { rc = c == 3 ? -1 : 0; break; }
        /* ESC and ^J end the search on the line found, to be edited; Return
         * runs it -- readline's isearch-terminators: `^R one` then a
         * newline leaves `echo one` there, and the ^P after it walks back
         * from that entry (logs/b43 hst4 k4) */
        if (c == 27 || c == 13 || c == 10) {
            if (found >= 0) {
                set_line(s, shell_history_get_hosted(found));
                s->hist = found;
                le_set_orig(s);
            }
            rc = c == 13 ? 1 : 0;
            break;
        }
        int again = 0;
        if (c == 127 || c == 8) {
            if (qn) {
                size_t k = qn - 1;
                if (s->u8)
                    while (k > 0 && ((unsigned char)q[k] & 0xC0) == 0x80) k--;
                qn = k;
                q[qn] = 0;
            }
        } else if (c == 18 || c == 19) {
            dir = c == 18 ? -1 : 1;
            again = 1;
        } else if (c >= 32 && c != 127) {
            char *nq = (char *)realloc(q, qn + 2);
            if (!nq) { rc = 0; break; }
            q = nq;
            q[qn++] = (char)c;
            q[qn] = 0;
        } else {
            /* ANY OTHER CONTROL KEY ENDS THE SEARCH AND IS THEN ITSELF, on
             * the line found, as readline's: `^R left ^O` runs the line
             * with "left" in it and fetches the next -- tsh dropped the ^O
             * (bash's history4.sub) -- finding 599 */
            if (found >= 0) {
                set_line(s, shell_history_get_hosted(found));
                s->hist = found;
                le_set_orig(s);
            }
            g_pushback = c;
            rc = 0;
            break;
        }

        int count = shell_history_count_hosted();
        int start;
        if (dir < 0) start = (again && found > 0) ? found - 1 : (found >= 0 ? found : count - 1);
        else start = (again && found >= 0) ? found + 1 : (found >= 0 ? found : 0);
        int hit = -1;
        for (int i = start; i >= 0 && i < count; i += dir) {
            const char *h = shell_history_get_hosted(i);
            if (!h) continue;
            if (qn == 0 || strstr(h, q)) { hit = i; break; }
        }
        if (hit >= 0) found = hit;
        else if (qn) bell();
    }
    free(q);
    return rc;
}

int tsh_lineedit_available(void) {
    struct termios t;
    return isatty(0) && tcgetattr(0, &t) == 0;
}


/* ---- Tab completion ----------------------------------------------------
 *
 * There was none. Tab inserted a literal tab, which in a shell prompt is
 * never what anyone means -- and it left `complete`/`compopt` with nothing to
 * feed, so implementing those first would have produced a registry no code
 * consumed.
 *
 * The rules follow bash closely enough to be unsurprising:
 *   - the FIRST word of a command completes against PATH (and builtins);
 *     every later word completes against the filesystem. Completing a
 *     filename where a command belongs is the mistake that makes a completer
 *     feel broken.
 *   - one match completes in place, adding '/' for a directory and a space
 *     otherwise, so the next word can be typed straight away.
 *   - several matches extend the word by the LONGEST COMMON PREFIX and, only
 *     if that added nothing, print the candidates. Redrawing the list on
 *     every Tab when there is still an unambiguous prefix to insert is what
 *     makes completion feel chatty.
 *   - a word registered with `complete -W` completes from that list instead.
 */

/* The word under the cursor: from the last unquoted separator to the caret. */
static size_t le_word_start(const struct le *s) {
    size_t i = s->pos;
    while (i > 0) {
        char c = s->buf[i - 1];
        if (c == ' ' || c == '\t' || c == '|' || c == '&' ||
            c == ';' || c == '(' || c == ')' || c == '<' || c == '>') break;
        i--;
    }
    return i;
}

/* Is the word at `start` the first of its command? Anything before it must be
 * blank or a command separator. */
static int le_is_command_slot(const struct le *s, size_t start) {
    size_t i = start;
    while (i > 0) {
        char c = s->buf[i - 1];
        if (c == ' ' || c == '\t') { i--; continue; }
        return (c == '|' || c == '&' || c == ';' ||
                c == '(' || c == '\n');
    }
    return 1;
}

struct le_matches {
    char **v;
    int    n, cap;
};

static void le_add(struct le_matches *m, const char *name, int is_dir) {
    if (m->n + 1 >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 64;
        char **nv = (char **)realloc(m->v, (size_t)nc * sizeof *nv);
        if (!nv) return;
        m->v = nv;
        m->cap = nc;
    }
    size_t l = strlen(name);
    char *e = (char *)malloc(l + 2);
    if (!e) return;
    memcpy(e, name, l);
    if (is_dir) e[l++] = '/';
    e[l] = 0;
    m->v[m->n++] = e;
}

static int le_has(const struct le_matches *m, const char *name) {
    for (int k = 0; k < m->n; k++)
        if (!strcmp(m->v[k], name)) return 1;
    return 0;
}

static void le_matches_free(struct le_matches *m) {
    for (int k = 0; k < m->n; k++) free(m->v[k]);
    free(m->v);
    m->v = 0;
    m->n = m->cap = 0;
}

/* Split "dir/partial" into the directory to scan and the prefix to match
 * (both malloc'd). */
static void le_split(const char *word, char **dir, char **pre) {
    const char *slash = strrchr(word, '/');
    if (!slash) {
        *dir = strdup(".");
        *pre = strdup(word);
        return;
    }
    size_t dl = (size_t)(slash - word) + 1;          /* keep the slash */
    *dir = (char *)malloc(dl + 1);
    if (*dir) { memcpy(*dir, word, dl); (*dir)[dl] = 0; }
    *pre = strdup(slash + 1);
}

static void le_scan_dir(struct le_matches *m, const char *dir, const char *pre) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t pl = strlen(pre), dl = strlen(dir);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        /* A dotfile only shows when the user has typed the dot, as in bash. */
        if (e->d_name[0] == '.' && (pl == 0 || pre[0] != '.')) continue;
        if (pl && strncmp(e->d_name, pre, pl) != 0) continue;
        size_t fl = dl + 1 + strlen(e->d_name) + 1;
        char *full = (char *)malloc(fl);
        if (!full) continue;
        snprintf(full, fl, "%s%s%s", dir,
                 (dl && dir[dl - 1] == '/') ? "" : "/", e->d_name);
        struct stat st;
        int isdir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        free(full);
        le_add(m, e->d_name, isdir);
    }
    closedir(d);
}

/* Commands: every executable name on PATH, plus the shell's own builtins. */
static void le_scan_commands(struct le_matches *m, const char *pre) {
    /* the shell's PATH, which need not be exported (bash's completion
     * searches the variable; `env -i tsh` has PATH unexported) */
    const char *path = shell_get_var_hosted("PATH");
    if (!path || !*path) path = "/bin";
    size_t pl = strlen(pre);
    const char *p = path;
    while (*p) {
        const char *c = strchr(p, ':');
        size_t dl = c ? (size_t)(c - p) : strlen(p);
        char *dir = (char *)malloc(dl + 1);
        if (!dir) return;
        memcpy(dir, p, dl);
        dir[dl] = 0;
        p += dl;
        if (*p == ':') p++;
        if (!dl) { free(dir); continue; }
        DIR *d = opendir(dir);
        free(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != 0) {
            if (e->d_name[0] == '.') continue;
            if (pl && strncmp(e->d_name, pre, pl) != 0) continue;
            if (!le_has(m, e->d_name)) le_add(m, e->d_name, 0);
        }
        closedir(d);
    }
    for (int i = 0; ; i++) {
        const char *bn = tsh_builtin_name_at(i);
        if (!bn) break;
        if (pl && strncmp(bn, pre, pl) != 0) continue;
        if (!le_has(m, bn)) le_add(m, bn, 0);
    }
}

static int le_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The candidates for the word before the cursor, sorted; how much of the
 * word they replace in *TYPED, and whether a compspec made them. */
static void le_candidates(struct le *s, struct le_matches *m, size_t *typed,
                          int *spec) {
    size_t start = le_word_start(s);
    size_t wl = s->pos - start;
    *typed = 0;
    *spec = 0;
    char *word = (char *)malloc(wl + 1);
    if (!word) return;
    memcpy(word, s->buf + start, wl);
    word[wl] = 0;

    /* A COMPSPEC FOR THE COMMAND WINS over the editor's own completion:
     * bash runs the spec -- its actions, -W words, -F function, -C command,
     * -X filter -- and only falls back when no `complete` applies. */
    char **pm = 0;
    int pn = 0;
    unsigned long popts = 0;
    int wl_spec = tsh_progcomp(s->buf, s->pos, word, &pm, &pn, &popts);
    if (wl_spec) {
        for (int i = 0; i < pn; i++) le_add(m, pm[i], 0);
        tsh_progcomp_free(pm, pn);
    } else if (le_is_command_slot(s, start) && !strchr(word, '/')) {
        le_scan_commands(m, word);
    } else {
        char *dir = 0, *pre = 0;
        le_split(word, &dir, &pre);
        if (dir && pre) le_scan_dir(m, dir, pre);
        free(dir);
        free(pre);
    }
    if (m->n) qsort(m->v, (size_t)m->n, sizeof m->v[0], le_cmp);

    /* How much of the word is already typed -- only the part AFTER the last
     * slash was matched against, so only that part gets replaced. */
    *typed = strlen(word);
    const char *lastslash = strrchr(word, '/');
    if (lastslash && !wl_spec) *typed = strlen(lastslash + 1);
    *spec = wl_spec;
    free(word);
}

/* possible-completions: the list under the line, then the line again */
static void le_list(struct le *s, struct le_matches *m) {
    le_below(s);
    for (int i = 0; i < m->n; i++) {
        outs(m->v[i]);
        outs((i + 1) % 4 == 0 ? "\r\n" : "  ");
    }
    if (m->n % 4) outs("\r\n");
    s->painted = 0;      /* the prompt is drawn fresh under the list */
    refresh(s);
}

static void do_complete(struct le *s) {
    struct le_matches m = { 0, 0, 0 };
    size_t typed = 0;
    int spec = 0;
    le_candidates(s, &m, &typed, &spec);
    (void)spec;
    if (m.n == 0) { bell(); return; }

    if (m.n == 1) {
        const char *v = m.v[0];
        size_t vl = strlen(v);
        le_snap(s);
        if (vl > typed) insert_n(s, v + typed, vl - typed);
        if (vl && v[vl - 1] != '/') insert(s, ' ');
        refresh(s);
        le_matches_free(&m);
        return;
    }

    /* Longest common prefix across the candidates -- whole characters. */
    size_t lcp = strlen(m.v[0]);
    for (int i = 1; i < m.n; i++) {
        size_t k = 0;
        while (k < lcp && m.v[i][k] && m.v[0][k] == m.v[i][k]) k++;
        lcp = k;
    }
    if (s->u8)
        while (lcp > 0 && ((unsigned char)m.v[0][lcp] & 0xC0) == 0x80) lcp--;
    if (lcp > typed) {
        le_snap(s);
        insert_n(s, m.v[0] + typed, lcp - typed);
        refresh(s);
        le_matches_free(&m);
        return;
    }

    /* Nothing left to insert -- now it is worth showing the list. */
    le_list(s, &m);
    le_matches_free(&m);
}

/* M-? and vi `=`: the list, inserting nothing */
static void list_completions(struct le *s) {
    struct le_matches m = { 0, 0, 0 };
    size_t typed = 0;
    int spec = 0;
    le_candidates(s, &m, &typed, &spec);
    if (m.n) le_list(s, &m);
    else bell();
    le_matches_free(&m);
}

/* M-* and vi `*`: every candidate, in place of the word */
static void insert_completions(struct le *s) {
    struct le_matches m = { 0, 0, 0 };
    size_t typed = 0;
    int spec = 0;
    le_candidates(s, &m, &typed, &spec);
    if (m.n == 0) { bell(); le_matches_free(&m); return; }
    le_snap(s);
    size_t start = le_word_start(s);
    size_t prefix_keep = (s->pos - start) - typed;   /* a directory part */
    char *dirpart = (char *)malloc(prefix_keep + 1);
    if (dirpart) { memcpy(dirpart, s->buf + start, prefix_keep); dirpart[prefix_keep] = 0; }
    del_range(s, start, s->pos, 0);
    for (int i = 0; i < m.n; i++) {
        if (dirpart) insert_n(s, dirpart, prefix_keep);
        insert_n(s, m.v[i], strlen(m.v[i]));
        insert(s, ' ');
    }
    free(dirpart);
    le_matches_free(&m);
    refresh(s);
}

/* ---- reading a character ---------------------------------------------- */

/* The rest of a multibyte character whose first byte C was just read: read
 * as many continuation bytes as its lead byte announces (UTF-8 only). */
static size_t read_char_rest(int c, char *b) {
    b[0] = (char)c;
    unsigned u = (unsigned char)c;
    size_t want = u >= 0xC2 && u <= 0xDF ? 2 : u >= 0xE0 && u <= 0xEF ? 3 :
                  u >= 0xF0 && u <= 0xF4 ? 4 : 1;
    size_t n = 1;
    while (n < want) {
        int k = read_key();
        if (k < 0) break;
        b[n] = (char)k;
        if ((k & 0xC0) != 0x80) { n++; break; }
        n++;
    }
    return n;
}

/* A character from the keyboard, whole: into B, its length returned (0 at
 * end of input). */
static size_t read_char(struct le *s, char *b) {
    int c = read_key();
    if (c < 0) return 0;
    if (c >= 0x80 && s->u8) return read_char_rest(c, b);
    b[0] = (char)c;
    return 1;
}

/* An escape sequence's tail after ESC: arrows, Home, End, Delete. Returns
 * 1 when it was one and has been handled. */
static int le_escape_seq(struct le *s, int a) {
    if (a != '[' && a != 'O') return 0;
    int b = read_key();
    if (b < 0) return 1;
    if (b == 'A') { history_move(s, -1); if (s->vi && s->vcmd) s->pos = 0; refresh(s); return 1; }
    if (b == 'B') { history_move(s, +1); if (s->vi && s->vcmd) s->pos = 0; refresh(s); return 1; }
    if (b == 'C') { s->pos = le_next(s, s->pos); refresh(s); return 1; }
    if (b == 'D') { s->pos = le_prev(s, s->pos); refresh(s); return 1; }
    if (b == 'H') { s->pos = 0;     refresh(s); return 1; }
    if (b == 'F') { s->pos = s->len; refresh(s); return 1; }
    if (b >= '0' && b <= '9') {                 /* ESC [ n ~ */
        int t2 = read_key();
        while (t2 >= '0' && t2 <= '9') t2 = read_key();
        if (t2 != '~') return 1;
        if (b == '1' || b == '7')      { s->pos = 0;     refresh(s); }
        else if (b == '4' || b == '8') { s->pos = s->len; refresh(s); }
        else if (b == '3' && s->pos < s->len) {
            le_snap(s);
            del_range(s, s->pos, le_next(s, s->pos), 0);
            refresh(s);
        }
    }
    return 1;
}

/* ---- shared commands ---------------------------------------------------- */

/* The words of a history line as the history library cuts them
 * (history_tokenize): blanks separate; quotes, backslashes and $( ) keep a
 * word together; ( ) and the operators ; & | < > are words of their own; a
 * word starting with `#` ends the line -- a comment has no words. WORD N
 * (0 the first, -1 the last) into [*B, *E); false when there is none. */
static bool le_hist_word(const char *h, int nth, size_t *b, size_t *e) {
    size_t i = 0, wb = 0, we = 0;
    int n = 0;
    bool any = false;
    while (h[i]) {
        while (h[i] == ' ' || h[i] == '\t' || h[i] == '\n') i++;
        if (!h[i] || h[i] == '#') break;
        size_t st = i;
        if (strchr("()", h[i])) {
            i++;
        } else if (strchr("<>;&|", h[i])) {
            char c = h[i++];
            if (h[i] == c || (c != ';' && h[i] == '&') || (c == '>' && h[i] == '|') ||
                (c == '&' && h[i] == '>'))
                i++;
        } else {
            char q = 0;
            int depth = 0;
            while (h[i]) {
                char c = h[i];
                if (q) {
                    if (c == '\\' && q != '\'' && h[i + 1]) { i += 2; continue; }
                    if (c == q) q = 0;
                    i++;
                    continue;
                }
                if (c == '\\' && h[i + 1]) { i += 2; continue; }
                if (c == '\'' || c == '"' || c == '`') { q = c; i++; continue; }
                if (c == '$' && h[i + 1] == '(') { depth++; i += 2; continue; }
                if (c == ')' && depth > 0) { depth--; i++; continue; }
                if (depth == 0 && (c == ' ' || c == '\t' || c == '\n' ||
                                   strchr("()<>;&|", c)))
                    break;
                i++;
            }
        }
        if (nth < 0 || n == nth) { wb = st; we = i; any = true; }
        if (nth >= 0 && n == nth) break;
        n++;
    }
    if (!any) return false;
    *b = wb;
    *e = we;
    return true;
}

/* yank-last-arg (M-. M-_): the last word of the line before; a repeat
 * goes one line further back and puts its word in place of what the last
 * one put in. A miss rings the bell and inserts nothing -- and still
 * counts as the command a repeat follows, as readline's does. */
static int  g_yla_back;
static bool g_yla_ok;
static void yank_last_arg(struct le *s, bool repeat) {
    if (!repeat) {
        g_yla_back = 0;
    } else {
        if (g_yla_ok && g_yank_to > g_yank_from && g_yank_to <= s->len) {
            del_range(s, g_yank_from, g_yank_to, 0);
            s->pos = g_yank_from;
        }
        g_yla_back++;
    }
    g_yla_ok = false;
    int idx = shell_history_count_hosted() - 1 - g_yla_back;
    const char *h = idx >= 0 ? shell_history_get_hosted(idx) : 0;
    size_t b = 0, e = 0;
    if (!h || !le_hist_word(h, -1, &b, &e)) { bell(); return; }
    g_yank_from = s->pos;
    insert_n(s, h + b, e - b);
    g_yank_to = s->pos;
    g_yla_ok = true;
}

/* vi `_`: a blank and then the last word of the line before -- word N
 * with a count, vi's words counting from 1 -- after the cursor, and
 * insert mode; nothing at all when the word is not there (readline's
 * rl_vi_yank_arg). */
static bool vi_yank_arg(struct le *s, int nth) {
    int count = shell_history_count_hosted();
    const char *h = count > 0 ? shell_history_get_hosted(count - 1) : 0;
    size_t b = 0, e = 0;
    if (!h || !le_hist_word(h, nth > 0 ? nth - 1 : -1, &b, &e)) { bell(); return false; }
    if (s->len) s->pos = le_next(s, s->pos);
    insert(s, ' ');
    insert_n(s, h + b, e - b);
    return true;
}

/* insert-comment (M-# and vi `#`): comment the line out -- or, with a
 * count, uncomment it if it is commented -- and accept it */
static void insert_comment(struct le *s, bool counted) {
    /* readline's comment-begin, "#" */
    if (counted && s->len && s->buf[0] == '#') {
        size_t p = s->pos;
        s->pos = 0;
        del_range(s, 0, 1, 0);
        s->pos = p ? p - 1 : 0;
    } else {
        size_t p = s->pos;
        s->pos = 0;
        insert(s, '#');
        s->pos = p + 1;
    }
}

/* the case of a word: 'u' 'l' or 'c' (M-u M-l M-c) */
static void case_word(struct le *s, char how) {
    size_t e = word_right(s, s->pos);
    bool first = true;
    for (size_t p = s->pos; p < e; ) {
        size_t n = le_next(s, p) - p;
        if (n == 1) {
            char c = s->buf[p];
            bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (how == 'u' || (how == 'c' && first && alpha)) {
                if (c >= 'a' && c <= 'z') s->buf[p] = (char)(c - 32);
            } else if (how == 'l' || how == 'c') {
                if (c >= 'A' && c <= 'Z') s->buf[p] = (char)(c + 32);
            }
            if (alpha || (c >= '0' && c <= '9')) first = false;
        } else {
            first = false;
        }
        p += n;
    }
    s->pos = e;
}

/* transpose-words (M-t) */
static void transpose_words(struct le *s) {
    size_t e2 = word_right(s, s->pos);
    size_t b2 = word_left(s, e2);
    size_t b1 = word_left(s, b2);
    size_t e1 = word_right(s, b1);
    if (b1 >= b2 || e1 > b2 || b2 >= e2) { bell(); return; }
    size_t l1 = e1 - b1, l2 = e2 - b2, lm = b2 - e1;
    char *t = (char *)malloc(e2 - b1);
    if (!t) return;
    memcpy(t, s->buf + b2, l2);
    memcpy(t + l2, s->buf + e1, lm);
    memcpy(t + l2 + lm, s->buf + b1, l1);
    memcpy(s->buf + b1, t, e2 - b1);
    free(t);
    s->pos = e2;
}

/* character search (^] and M-^]): to the next / previous C */
static void char_search(struct le *s, int dir) {
    char cb[4];
    size_t n = read_char(s, cb);
    if (!n) return;
    size_t p = s->pos;
    for (;;) {
        p = dir > 0 ? le_next(s, p) : (p ? le_prev(s, p) : (size_t)-1);
        if (p == (size_t)-1 || p >= s->len) { bell(); return; }
        if (p + n <= s->len && memcmp(s->buf + p, cb, n) == 0) { s->pos = p; return; }
        if (p == 0 && dir < 0) { bell(); return; }
    }
}

/* operate-and-get-next (^O): the line runs, and the next prompt starts on
 * the history line after it */
static long g_next_hist = -1;

/* ---- vi ------------------------------------------------------------------ */

/* In command mode the cursor sits ON a character, never past the last. */
static void vi_clamp(struct le *s) {
    if (s->len && s->pos >= s->len) s->pos = le_prev(s, s->len);
    if (!s->len) s->pos = 0;
}

static size_t vi_fword(struct le *s, size_t p, bool big) {
    if (p >= s->len) return p;
    uint32_t c = le_cp(s, p);
    if (big) {
        while (p < s->len && !le_blank(le_cp(s, p))) p = le_next(s, p);
    } else if (vi_word(c)) {
        while (p < s->len && vi_word(le_cp(s, p))) p = le_next(s, p);
    } else if (!le_blank(c)) {
        while (p < s->len && !vi_word(le_cp(s, p)) && !le_blank(le_cp(s, p)))
            p = le_next(s, p);
    }
    while (p < s->len && le_blank(le_cp(s, p))) p = le_next(s, p);
    return p;
}

static size_t vi_bword(struct le *s, size_t p, bool big) {
    while (p > 0 && le_blank(le_cp(s, le_prev(s, p)))) p = le_prev(s, p);
    if (p == 0) return 0;
    uint32_t c = le_cp(s, le_prev(s, p));
    if (big) {
        while (p > 0 && !le_blank(le_cp(s, le_prev(s, p)))) p = le_prev(s, p);
    } else if (vi_word(c)) {
        while (p > 0 && vi_word(le_cp(s, le_prev(s, p)))) p = le_prev(s, p);
    } else {
        while (p > 0 && !vi_word(le_cp(s, le_prev(s, p))) &&
               !le_blank(le_cp(s, le_prev(s, p))))
            p = le_prev(s, p);
    }
    return p;
}

/* the last character of the word at or after p+1 */
static size_t vi_eword(struct le *s, size_t p, bool big) {
    if (p >= s->len) return p;
    size_t q = le_next(s, p);
    while (q < s->len && le_blank(le_cp(s, q))) q = le_next(s, q);
    if (q >= s->len) return s->len ? le_prev(s, s->len) : 0;
    uint32_t c = le_cp(s, q);
    size_t last = q;
    if (big) {
        while (q < s->len && !le_blank(le_cp(s, q))) { last = q; q = le_next(s, q); }
    } else if (vi_word(c)) {
        while (q < s->len && vi_word(le_cp(s, q))) { last = q; q = le_next(s, q); }
    } else {
        while (q < s->len && !vi_word(le_cp(s, q)) && !le_blank(le_cp(s, q))) {
            last = q;
            q = le_next(s, q);
        }
    }
    return last;
}

static char g_vi_cs_cmd;        /* the last f F t T, for ; and , */
static char g_vi_cs_char[4];
static size_t g_vi_cs_n;

/* f F t T to the CHAR at count N: the new position, or (size_t)-1 */
static size_t vi_csearch(struct le *s, char cmd, const char *ch, size_t chn, int n) {
    size_t p = s->pos;
    bool fwd = cmd == 'f' || cmd == 't';
    for (int k = 0; k < n; k++) {
        size_t q = p;
        bool hit = false;
        for (;;) {
            if (fwd) {
                q = le_next(s, q);
                if (q >= s->len) break;
            } else {
                if (q == 0) break;
                q = le_prev(s, q);
            }
            if (q + chn <= s->len && memcmp(s->buf + q, ch, chn) == 0) {
                hit = true;
                break;
            }
        }
        if (!hit) return (size_t)-1;
        p = q;
    }
    if (cmd == 't') p = le_prev(s, p);
    else if (cmd == 'T') p = le_next(s, p);
    return p;
}

static size_t vi_match(struct le *s) {
    size_t p = s->pos;
    while (p < s->len && !strchr("()[]{}", s->buf[p])) p++;
    if (p >= s->len) return (size_t)-1;
    char c = s->buf[p];
    const char *pairs = "()[]{}";
    const char *at = strchr(pairs, c);
    int idx = (int)(at - pairs);
    char open = pairs[idx & ~1], close = pairs[idx | 1];
    int depth = 0;
    if (idx % 2 == 0) {
        for (size_t q = p; q < s->len; q++) {
            if (s->buf[q] == open) depth++;
            else if (s->buf[q] == close && --depth == 0) return q;
        }
    } else {
        for (size_t q = p + 1; q-- > 0; ) {
            if (s->buf[q] == close) depth++;
            else if (s->buf[q] == open && --depth == 0) return q;
        }
    }
    return (size_t)-1;
}

static size_t g_vi_marks[26];
static bool   g_vi_markset[26];
static char  *g_vi_search;      /* the last / or ? pattern */
static int    g_vi_sdir;        /* its direction: -1 older (/), +1 newer (?) */

/* A MOTION from the cursor: the target in *NP, and whether the range it
 * makes for an operator includes the target character. 0: not a motion. */
static int vi_motion(struct le *s, int c, int n, size_t *np, bool *incl, bool op) {
    size_t p = s->pos;
    *incl = false;
    switch (c) {
    case 'h': case 8: case 127:
        for (int k = 0; k < n && p > 0; k++) p = le_prev(s, p);
        break;
    case 'l': case ' ':
        for (int k = 0; k < n && p < s->len; k++) p = le_next(s, p);
        if (!op && p >= s->len) p = s->len ? le_prev(s, s->len) : 0;
        break;
    case '0':
        p = 0;
        break;
    case '^':
        p = 0;
        while (p < s->len && le_blank(le_cp(s, p))) p = le_next(s, p);
        break;
    case '$':
        p = s->len;
        if (!op && p) p = le_prev(s, p);
        break;
    case '|':
        p = 0;
        for (int k = 1; k < n && p < s->len; k++) p = le_next(s, p);
        break;
    case 'w': case 'W':
        for (int k = 0; k < n; k++) p = vi_fword(s, p, c == 'W');
        break;
    case 'b': case 'B':
        for (int k = 0; k < n; k++) p = vi_bword(s, p, c == 'B');
        break;
    case 'e': case 'E':
        for (int k = 0; k < n; k++) p = vi_eword(s, p, c == 'E');
        *incl = true;
        break;
    case 'f': case 'F': case 't': case 'T': {
        char cb[4];
        size_t cn = read_char(s, cb);
        if (!cn) return 0;
        g_vi_cs_cmd = (char)c;
        memcpy(g_vi_cs_char, cb, cn);
        g_vi_cs_n = cn;
        size_t q = vi_csearch(s, (char)c, cb, cn, n);
        if (q == (size_t)-1) return -1;
        p = q;
        *incl = c == 'f' || c == 't';
        break;
    }
    case ';': case ',': {
        if (!g_vi_cs_cmd) return -1;
        char cmd = g_vi_cs_cmd;
        if (c == ',') cmd = cmd == 'f' ? 'F' : cmd == 'F' ? 'f' : cmd == 't' ? 'T' : 't';
        size_t q = vi_csearch(s, cmd, g_vi_cs_char, g_vi_cs_n, n);
        if (q == (size_t)-1) return -1;
        p = q;
        *incl = cmd == 'f' || cmd == 't';
        break;
    }
    case '%': {
        size_t q = vi_match(s);
        if (q == (size_t)-1) return -1;
        p = q;
        *incl = true;
        break;
    }
    case '`': {
        int m = read_key();
        if (m < 'a' || m > 'z' || !g_vi_markset[m - 'a']) return -1;
        p = g_vi_marks[m - 'a'];
        if (p > s->len) p = s->len;
        break;
    }
    default:
        return 0;
    }
    *np = p;
    return 1;
}

static void vi_yank(const char *t, size_t n) {
    char *e = (char *)malloc(n + 1);
    if (!e) return;
    memcpy(e, t, n);
    e[n] = 0;
    free(g_kill[9]);
    memmove(g_kill + 1, g_kill, 9 * sizeof g_kill[0]);
    g_kill[0] = e;
    if (g_killn < 10) g_killn++;
}

/* vi history search: the newest (DIR -1) or oldest (+1) line from the
 * current one containing PAT -- `^` anchors it at the start */
static bool vi_hsearch(struct le *s, const char *pat, int dir) {
    int count = shell_history_count_hosted();
    bool anch = pat[0] == '^';
    const char *pp = anch ? pat + 1 : pat;
    for (int i = s->hist + dir; i >= 0 && i < count; i += dir) {
        const char *h = shell_history_get_hosted(i);
        if (!h) continue;
        if (anch ? strncmp(h, pp, strlen(pp)) == 0 : strstr(h, pp) != 0) {
            history_go(s, i);
            s->pos = 0;
            return true;
        }
    }
    bell();
    return false;
}

/* The / or ? line: read on the bottom of the line itself, as readline
 * does, until Enter. 0 when it was abandoned. */
static char *vi_read_pattern(struct le *s, char lead) {
    struct le tmp;
    memset(&tmp, 0, sizeof tmp);
    char *tb = 0;
    size_t tc = 0;
    tmp.bufp = &tb; tmp.capp = &tc;
    tmp.u8 = s->u8;
    char pr[2] = { lead, 0 };
    tmp.prompt = pr;
    tmp.plen = 1;
    tmp.painted = 1;
    tmp.crow = s->crow;
    if (!le_room(&tmp, 0)) return 0;
    tmp.buf[0] = 0;
    refresh(&tmp);
    for (;;) {
        int c = read_key();
        if (c < 0 || c == 3 || c == 27) { free(tmp.buf); s->crow = tmp.crow; return 0; }
        if (c == 13 || c == 10) break;
        if (c == 127 || c == 8) {
            if (tmp.len == 0) { free(tmp.buf); s->crow = tmp.crow; return 0; }
            del_range(&tmp, le_prev(&tmp, tmp.pos), tmp.pos, 0);
        } else if (c >= 32) {
            char cb[4];
            size_t n = (c >= 0x80 && s->u8) ? read_char_rest(c, cb) : (cb[0] = (char)c, 1);
            insert_n(&tmp, cb, n);
        }
        refresh(&tmp);
    }
    s->crow = tmp.crow;
    char *r = strdup(tmp.buf);
    free(tmp.buf);
    return r;
}

enum { VI_GO_ON, VI_ACCEPT, VI_EOF, VI_INTR };

/* Enter insert mode -- the whole insertion is one undo step */
static void vi_insert_mode(struct le *s) {
    s->vcmd = false;
    s->vrepl = false;
}

/* ONE COMMAND-MODE COMMAND, its first key C already read. */
static int vi_command(struct le *s, int c) {
    int count = 0;
    /* every command records its keys -- from the count on -- and a change
     * keeps them for `.` (vi_commit) */
    g_recn = 0;
    bool counted = false;
    if (!g_replaying) { g_recording = true; rec_key((char)c); }
    while ((c >= '1' && c <= '9') || (c == '0' && counted)) {
        count = count * 10 + (c - '0');
        counted = true;
        c = read_key();
        if (c < 0) { g_recording = false; return VI_EOF; }
    }
    int n = counted ? count : 1;
    switch (c) {
    case 13: case 10:
        g_recording = false;
        return VI_ACCEPT;
    case 4:                                     /* ^D: EOF on an empty line */
        g_recording = false;
        return s->len == 0 ? VI_EOF : VI_ACCEPT;
    case 3:
        g_recording = false;
        return VI_INTR;
    case 27:
        g_recording = false;
        if (keys_pending()) {
            int a = read_key();
            if (a == '[' || a == 'O') {
                (void)le_escape_seq(s, a);
                vi_clamp(s);
                refresh(s);
                return VI_GO_ON;
            }
            /* ESC then a key that starts no sequence: the key is a
             * command of its own (readline's dispatch) */
            return a < 0 ? VI_EOF : vi_command(s, a);
        }
        bell();
        return VI_GO_ON;
    case 12: {
        g_recording = false;
        char cl[12];
        int k = snprintf(cl, sizeof cl, "%c[H%c[2J", 27, 27);
        out(cl, (size_t)k);
        s->painted = 0;
        s->crow = 0;
        refresh(s);
        return VI_GO_ON;
    }
    case 18: case 19: {                         /* ^R ^S */
        g_recording = false;
        int r = isearch(s, c == 18 ? -1 : 1);
        if (r < 0) return VI_INTR;
        vi_clamp(s);
        refresh(s);
        return r == 1 ? VI_ACCEPT : VI_GO_ON;
    }
    case 5:                                     /* ^E: emacs mode */
        g_recording = false;
        s->vi = false;
        shell_set_editing_mode_hosted(false);
        return VI_GO_ON;
    case 'j': case '+': case 14:
        g_recording = false;
        for (int k = 0; k < n; k++) history_move(s, +1);
        s->pos = 0;
        refresh(s);
        return VI_GO_ON;
    case 'k': case '-': case 16:
        g_recording = false;
        for (int k = 0; k < n; k++) history_move(s, -1);
        s->pos = 0;
        refresh(s);
        return VI_GO_ON;
    case 'G': {
        g_recording = false;
        int count_h = shell_history_count_hosted();
        if (counted) {
            long want = (long)count - (long)shell_history_base_hosted() - 1;
            if (want < 0 || want >= count_h) { bell(); return VI_GO_ON; }
            history_go(s, (int)want);
        } else {
            history_go(s, 0);
        }
        s->pos = 0;
        refresh(s);
        return VI_GO_ON;
    }
    case '/': case '?': {
        g_recording = false;
        char *pat = vi_read_pattern(s, (char)c);
        if (pat && *pat) {
            free(g_vi_search);
            g_vi_search = pat;
            g_vi_sdir = c == '/' ? -1 : 1;
            (void)vi_hsearch(s, g_vi_search, g_vi_sdir);
        } else {
            if (pat && g_vi_search) (void)vi_hsearch(s, g_vi_search, c == '/' ? -1 : 1);
            free(pat);
        }
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    }
    case 'n': case 'N':
        g_recording = false;
        if (!g_vi_search) { bell(); return VI_GO_ON; }
        (void)vi_hsearch(s, g_vi_search, c == 'n' ? g_vi_sdir : -g_vi_sdir);
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    case 'm': {
        g_recording = false;
        int m = read_key();
        if (m >= 'a' && m <= 'z') { g_vi_marks[m - 'a'] = s->pos; g_vi_markset[m - 'a'] = true; }
        else bell();
        return VI_GO_ON;
    }
    case '#':
        g_recording = false;
        le_snap(s);
        insert_comment(s, counted);
        refresh(s);
        return VI_ACCEPT;
    case '\\':
        g_recording = false;
        do_complete(s);
        vi_insert_mode(s);
        return VI_GO_ON;
    case '=':
        g_recording = false;
        list_completions(s);
        return VI_GO_ON;
    case '*':
        g_recording = false;
        insert_completions(s);
        vi_insert_mode(s);
        return VI_GO_ON;
    case 'u': case 31:                          /* u, ^_ */
        g_recording = false;
        if (!le_undo(s)) bell();
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    case 'U':
        g_recording = false;
        le_snap(s);
        set_line(s, s->orig ? s->orig : "");
        s->pos = 0;
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    case '.': {
        g_recording = false;
        if (!g_chgn) { bell(); return VI_GO_ON; }
        /* replay the last change -- a count given here replaces its own */
        size_t skip = 0;
        while (skip < g_chgn && g_chg[skip] >= '0' && g_chg[skip] <= '9' &&
               !(skip == 0 && g_chg[skip] == '0'))
            skip++;
        char *buf = (char *)malloc(g_chgn + 24);
        if (!buf) return VI_GO_ON;
        size_t bl = 0;
        if (counted) bl += (size_t)snprintf(buf, 24, "%d", count);
        else { memcpy(buf, g_chg, skip); bl = skip; }
        memcpy(buf + bl, g_chg + skip, g_chgn - skip);
        bl += g_chgn - skip;
        free(g_rep);
        g_rep = buf;
        g_repn = bl;
        g_repi = 0;
        g_replaying = true;
        int r = VI_GO_ON;
        while (g_replaying && g_repi < g_repn) {
            int k = read_key();
            if (k < 0) break;
            if (s->vcmd) r = vi_command(s, k);
            else {
                /* the insertion part of the change */
                if (k == 27) { s->vcmd = true; if (s->pos > 0) s->pos = le_prev(s, s->pos); continue; }
                if (k == 127 || k == 8) { if (s->pos > 0) del_range(s, le_prev(s, s->pos), s->pos, 0); continue; }
                char cb[4];
                size_t cn = (k >= 0x80 && s->u8) ? read_char_rest(k, cb) : (cb[0] = (char)k, 1);
                if (s->vrepl && s->pos < s->len) del_range(s, s->pos, le_next(s, s->pos), 0);
                insert_n(s, cb, cn);
            }
            if (r != VI_GO_ON) break;
        }
        g_replaying = false;
        g_recording = false;
        if (!s->vcmd) { s->vcmd = true; if (s->pos > 0) s->pos = le_prev(s, s->pos); }
        vi_clamp(s);
        refresh(s);
        return r;
    }
    case 'i': case 'a': case 'I': case 'A':
        le_snap(s);
        if (c == 'a' && s->len) s->pos = le_next(s, s->pos);
        if (c == 'I') s->pos = 0;
        if (c == 'A') s->pos = s->len;
        vi_insert_mode(s);
        refresh(s);
        return VI_GO_ON;
    case 'R':
        le_snap(s);
        vi_insert_mode(s);
        s->vrepl = true;
        return VI_GO_ON;
    case '_':
        le_snap(s);
        if (vi_yank_arg(s, counted ? count : -1)) vi_insert_mode(s);
        else g_recording = false;
        refresh(s);
        return VI_GO_ON;
    case 'x': case 'X': {
        if (!s->len || (c == 'X' && s->pos == 0)) { bell(); g_recording = false; return VI_GO_ON; }
        le_snap(s);
        size_t a = s->pos, b = s->pos;
        if (c == 'x') for (int k = 0; k < n && b < s->len; k++) b = le_next(s, b);
        else for (int k = 0; k < n && a > 0; k++) a = le_prev(s, a);
        vi_yank(s->buf + a, b - a);
        del_range(s, a, b, 0);
        s->pos = a;
        vi_clamp(s);
        refresh(s);
        vi_commit();
        return VI_GO_ON;
    }
    case 'r': {
        char cb[4];
        size_t cn = read_char(s, cb);
        if (!cn || cb[0] == 27) { g_recording = false; return VI_GO_ON; }
        size_t b = s->pos;
        int k = 0;
        for (; k < n && b < s->len; k++) b = le_next(s, b);
        if (k < n) { bell(); g_recording = false; return VI_GO_ON; }
        le_snap(s);
        del_range(s, s->pos, b, 0);
        size_t at = s->pos;
        for (k = 0; k < n; k++) insert_n(s, cb, cn);
        s->pos = at;
        for (k = 1; k < n; k++) s->pos = le_next(s, s->pos);
        vi_clamp(s);
        refresh(s);
        vi_commit();
        return VI_GO_ON;
    }
    case '~': {
        if (!s->len) { bell(); g_recording = false; return VI_GO_ON; }
        le_snap(s);
        for (int k = 0; k < n && s->pos < s->len; k++) {
            size_t nx = le_next(s, s->pos);
            if (nx - s->pos == 1) {
                char ch = s->buf[s->pos];
                if (ch >= 'a' && ch <= 'z') s->buf[s->pos] = (char)(ch - 32);
                else if (ch >= 'A' && ch <= 'Z') s->buf[s->pos] = (char)(ch + 32);
            } else if (s->u8) {
                uint32_t cp = le_cp(s, s->pos), nc = wc_toupper(cp);
                if (nc == cp) nc = wc_tolower(cp);
                if (nc != cp) {
                    char eb[4];
                    size_t el;
                    if (nc < 0x800) { eb[0] = (char)(0xC0 | (nc >> 6)); eb[1] = (char)(0x80 | (nc & 0x3F)); el = 2; }
                    else if (nc < 0x10000) { eb[0] = (char)(0xE0 | (nc >> 12)); eb[1] = (char)(0x80 | ((nc >> 6) & 0x3F)); eb[2] = (char)(0x80 | (nc & 0x3F)); el = 3; }
                    else { eb[0] = (char)(0xF0 | (nc >> 18)); eb[1] = (char)(0x80 | ((nc >> 12) & 0x3F)); eb[2] = (char)(0x80 | ((nc >> 6) & 0x3F)); eb[3] = (char)(0x80 | (nc & 0x3F)); el = 4; }
                    size_t at = s->pos;
                    del_range(s, at, nx, 0);
                    s->pos = at;
                    insert_n(s, eb, el);
                    nx = s->pos;
                }
            }
            s->pos = nx;
        }
        vi_clamp(s);
        refresh(s);
        vi_commit();
        return VI_GO_ON;
    }
    case 'p': case 'P': {
        if (!g_killn) { bell(); g_recording = false; return VI_GO_ON; }
        le_snap(s);
        const char *y = g_kill[0];
        size_t yl = strlen(y);
        if (c == 'p' && s->len) s->pos = le_next(s, s->pos);
        for (int k = 0; k < n; k++) insert_n(s, y, yl);
        if (s->pos > 0) s->pos = le_prev(s, s->pos);
        vi_clamp(s);
        refresh(s);
        vi_commit();
        return VI_GO_ON;
    }
    case 's': case 'S': case 'C': case 'D': case 'Y':
    case 'c': case 'd': case 'y': {
        int op = c;
        int mc = 0, mcount = 0;
        bool mcounted = false;
        size_t a = s->pos, b = s->pos;
        bool whole = false;
        if (op == 's') { op = 'c'; mc = 'l'; }
        else if (op == 'S') { op = 'c'; whole = true; }
        else if (op == 'C') { op = 'c'; mc = '$'; }
        else if (op == 'D') { op = 'd'; mc = '$'; }
        else if (op == 'Y') { op = 'y'; mc = '$'; }
        else {
            mc = read_key();
            while ((mc >= '1' && mc <= '9') || (mc == '0' && mcounted)) {
                mcount = mcount * 10 + (mc - '0');
                mcounted = true;
                mc = read_key();
            }
            if (mc < 0) { g_recording = false; return VI_EOF; }
            if (mc == op) whole = true;
        }
        int mn = n * (mcounted ? mcount : 1);
        if (whole) {
            a = 0;
            b = s->len;
        } else {
            /* `cw` on a word changes to its END, as vi's does (ce) */
            if (op == 'c' && (mc == 'w' || mc == 'W') && s->pos < s->len &&
                !le_blank(le_cp(s, s->pos)))
                mc = mc == 'w' ? 'e' : 'E';
            size_t np = 0;
            bool incl = false;
            int r = vi_motion(s, mc, mn, &np, &incl, true);
            if (r <= 0) { bell(); g_recording = false; return VI_GO_ON; }
            if (np >= s->pos) {
                a = s->pos;
                b = incl ? le_next(s, np) : np;
            } else {
                a = np;
                b = s->pos;
                if (incl) b = le_next(s, b);
            }
            if (b > s->len) b = s->len;
        }
        if (op == 'y') {
            vi_yank(s->buf + a, b - a);
            if (a < s->pos) s->pos = a;
            vi_clamp(s);
            refresh(s);
            g_recording = false;
            return VI_GO_ON;
        }
        le_snap(s);
        vi_yank(s->buf + a, b - a);
        del_range(s, a, b, 0);
        s->pos = a;
        if (op == 'c') {
            vi_insert_mode(s);          /* recording goes on to its ESC */
        } else {
            vi_clamp(s);
            vi_commit();
        }
        refresh(s);
        return VI_GO_ON;
    }
    case 21:                                    /* ^U */
        g_recording = false;
        le_snap(s);
        del_range(s, 0, s->pos, -1);
        refresh(s);
        return VI_GO_ON;
    case 11:                                    /* ^K */
        g_recording = false;
        le_snap(s);
        del_range(s, s->pos, s->len, 1);
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    case 25:                                    /* ^Y */
        g_recording = false;
        if (g_killn) { le_snap(s); insert_n(s, g_kill[0], strlen(g_kill[0])); }
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    case 20:                                    /* ^T */
        g_recording = false;
        if (s->len >= 2 && s->pos > 0) {
            le_snap(s);
            size_t b2 = s->pos >= s->len ? le_prev(s, s->len) : s->pos;
            size_t a2 = le_prev(s, b2), e2 = le_next(s, b2);
            if (a2 < b2 && b2 < e2) {
                size_t la = b2 - a2, lb = e2 - b2;
                char t[8];
                memcpy(t, s->buf + a2, la);
                memmove(s->buf + a2, s->buf + b2, lb);
                memcpy(s->buf + a2 + lb, t, la);
                s->pos = e2;
            }
        }
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    default: {
        size_t np = 0;
        bool incl = false;
        g_recording = false;
        int r = vi_motion(s, c, n, &np, &incl, false);
        if (r <= 0) { bell(); return VI_GO_ON; }
        s->pos = np;
        vi_clamp(s);
        refresh(s);
        return VI_GO_ON;
    }
    }
}

/* ---- the line ------------------------------------------------------------ */

/* ^C AT THE PROMPT echoes "^C" as readline's does: only when the terminal
 * echoes control characters (ECHOCTL in the termios the line was read
 * under -- readline's _rl_echoctl), then the line ends and the caller
 * starts afresh. This printed "^C" whatever the terminal; on this system's
 * ptys, which have no ECHOCTL, the guest's bash prints none (logs/b22
 * rawintr). */
static bool g_le_echoctl;
static void le_echo_intr(void) {
    if (g_le_echoctl) outs("^C");
}

static long le_readline(const char *prompt, char **bufp, size_t *capp, bool tty);

long tsh_readline(const char *prompt, char **bufp, size_t *capp) {
    return le_readline(prompt, bufp, capp, true);
}

/* The editor over a pipe (see g_le_pipe): no terminal to set, nothing drawn
 * while the keys are read, and the prompt and the line accepted shown once
 * at the end -- the bytes the plain reader's echo gave. */
long tsh_readline_pipe(const char *prompt, char **bufp, size_t *capp) {
    g_le_pipe = true;
    long r = le_readline(prompt, bufp, capp, false);
    g_le_pipe = false;
    const char *p = prompt ? prompt : "";
    (void)!write(2, p, strlen(p));          /* at the end of input too */
    if (r >= 0) {
        (void)!write(2, *bufp, (size_t)r);
        (void)!write(2, "\n", 1);
    }
    return r;
}

static long le_readline(const char *prompt, char **bufp, size_t *capp, bool tty) {
    struct termios orig, raw;
    if (!bufp || !capp) return TSH_LE_EOF;
    if (tty) {
        if (tcgetattr(0, &orig) != 0) return TSH_LE_EOF;
        g_le_echoctl = (orig.c_lflag & ECHOCTL) != 0;
        raw = orig;
        /* ICANON off so every keystroke arrives; ECHO off because we paint
         * the line ourselves; ISIG off because ^C belongs to the editor while
         * a line is being typed. All restored before returning, so a COMMAND
         * runs under a normal tty and job control still routes ^C/^Z to the
         * foreground job. */
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
        raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSANOW, &raw) != 0) return TSH_LE_EOF;
    }

    struct le s;
    memset(&s, 0, sizeof s);
    s.bufp = bufp;
    s.capp = capp;
    s.buf = *bufp;
    s.cap = *capp;
    s.u8 = shell_utf8_hosted();
    if (!le_room(&s, 0)) { if (tty) (void)tcsetattr(0, TCSANOW, &orig); return TSH_LE_EOF; }
    s.prompt = prompt ? prompt : "";
    s.plen = strlen(s.prompt);
    s.hist = shell_history_count_hosted();
    s.buf[0] = 0;
    s.vi = shell_opt_vi_hosted();
    s.vcmd = false;
    g_pushback = -1;
    /* ^O left the history line after the one it ran for this prompt */
    /* (a history NUMBER: the line ^O ran may have pushed the oldest entry
     * out under HISTSIZE, and the index would then be one past it) */
    long nh = g_next_hist >= 0 ? g_next_hist - (long)shell_history_base_hosted() : -1;
    if (nh >= 0 && nh < s.hist) {
        s.hist = (int)nh;
        set_line(&s, shell_history_get_hosted((int)nh));
    }
    g_next_hist = -1;
    le_set_orig(&s);
    refresh(&s);

    long ret = 0;
    for (;;) {
        int c = read_key();
        if (c < 0) {
            ret = (s.len == 0) ? TSH_LE_EOF : (long)s.len;
            break;
        }
        int last = s.last;
        s.prev = last;
        s.last = LC_OTHER;

        if (s.vi && s.vcmd) {
            int r = vi_command(&s, c);
            if (r == VI_ACCEPT) { le_below(&s); ret = (long)s.len; break; }
            if (r == VI_EOF) { le_below(&s); ret = s.len ? (long)s.len : TSH_LE_EOF; break; }
            if (r == VI_INTR) { le_echo_intr(); le_below(&s); ret = TSH_LE_INTR; break; }
            if (!s.vcmd) refresh(&s);
            continue;
        }

        /* keys both modes' insertion share */
        if (c == 13 || c == 10) {
            if (s.vi) g_recording = false;
            le_below(&s);
            ret = (long)s.len;
            break;
        }
        if (c == 3) { le_echo_intr(); le_below(&s); ret = TSH_LE_INTR; break; }
        if (c == 4) {                                   /* ^D */
            if (s.len == 0) { le_below(&s); ret = TSH_LE_EOF; break; }
            if (s.vi) { le_below(&s); ret = (long)s.len; break; }
            if (s.pos < s.len) { le_snap(&s); del_range(&s, s.pos, le_next(&s, s.pos), 0); refresh(&s); }
            continue;
        }
        if (c == 127 || c == 8) {                       /* backspace */
            if (s.pos > 0) {
                if (!s.vi) le_snap(&s);
                if (s.vi && s.vrepl) s.pos = le_prev(&s, s.pos);
                else del_range(&s, le_prev(&s, s.pos), s.pos, 0);
                refresh(&s);
            } else {
                bell();
            }
            continue;
        }
        if (c == 9)  { do_complete(&s); continue; }     /* Tab */
        if (c == 18 || c == 19) {                       /* ^R ^S */
            int rs = isearch(&s, c == 18 ? -1 : 1);
            if (rs < 0) { le_echo_intr(); le_below(&s); ret = TSH_LE_INTR; break; }
            refresh(&s);
            if (rs == 1) { le_below(&s); ret = (long)s.len; break; }
            continue;
        }
        if (c == 20) {                                  /* ^T transpose */
            /* the characters before and at the cursor -- at the end of the
             * line, the last two -- trade places, whole */
            if (s.len >= 2 && s.pos > 0) {
                size_t b = s.pos >= s.len ? le_prev(&s, s.len) : s.pos;
                size_t a = le_prev(&s, b);
                size_t e = le_next(&s, b);
                if (a < b && b < e) {
                    le_snap(&s);
                    size_t la = b - a, lb = e - b;
                    char t[8];
                    memcpy(t, s.buf + a, la);
                    memmove(s.buf + a, s.buf + b, lb);
                    memcpy(s.buf + a + lb, t, la);
                    s.pos = e;
                    refresh(&s);
                }
            }
            continue;
        }
        if (c == 21) { le_snap(&s); del_range(&s, 0, s.pos, -1); refresh(&s); continue; }
        if (c == 23) {                                  /* ^W */
            le_snap(&s);
            size_t from = s.vi ? vi_bword(&s, s.pos, false) : word_left_blank(&s);
            del_range(&s, from, s.pos, -1);
            refresh(&s);
            continue;
        }
        if (c == 25) {                                  /* ^Y */
            if (g_killn) {
                le_snap(&s);
                g_killi = 0;
                g_yank_from = s.pos;
                insert_n(&s, g_kill[0], strlen(g_kill[0]));
                g_yank_to = s.pos;
                s.last = LC_YANK;
            }
            refresh(&s);
            continue;
        }
        if (c == 22 || (c == 17 && !s.vi)) {            /* ^V (^Q): quoted insert */
            char cb[4];
            size_t n = read_char(&s, cb);
            if (n) { le_snap(&s); insert_n(&s, cb, n); refresh(&s); }
            continue;
        }
        if (c == 31) {                                  /* ^_ undo */
            if (!le_undo(&s)) bell();
            refresh(&s);
            continue;
        }

        if (s.vi) {
            /* vi INSERT mode: ESC to command mode; the other controls are
             * self-inserting in readline's vi-insert keymap */
            if (c == 27) {
                if (keys_pending()) {
                    int a = read_key();
                    if (a == '[' || a == 'O') { (void)le_escape_seq(&s, a); continue; }
                    /* ESC then a key that starts no sequence: command
                     * mode, and the key is its first command */
                    if (a >= 0) {
                        if (g_recording && g_recn > 0) g_recn--;
                        g_pushback = a;
                    }
                }
                vi_commit();            /* an insertion's change ends here */
                s.vcmd = true;
                s.vrepl = false;
                if (s.pos > 0) s.pos = le_prev(&s, s.pos);
                /* U goes back to the line as command mode first found it
                 * -- what was typed before is not undone (readline frees
                 * its undo list there, as POSIX describes U) */
                if (!s.vionce) {
                    s.vionce = true;
                    le_set_orig(&s);
                }
                refresh(&s);
                continue;
            }
            if (c == 14 || c == 16) { bell(); continue; }   /* menu-complete */
            char cb[4];
            size_t n = (c >= 0x80 && s.u8) ? read_char_rest(c, cb) : (cb[0] = (char)c, 1);
            if (s.vrepl && s.pos < s.len) del_range(&s, s.pos, le_next(&s, s.pos), 0);
            insert_n(&s, cb, n);
            refresh(&s);
            continue;
        }

        /* ---- emacs ---- */
        if (c == 1)  { s.pos = 0;                  refresh(&s); continue; }
        if (c == 5)  { s.pos = s.len;              refresh(&s); continue; }
        if (c == 2)  { s.pos = le_prev(&s, s.pos); refresh(&s); continue; }
        if (c == 6)  { s.pos = le_next(&s, s.pos); refresh(&s); continue; }
        if (c == 16) { history_move(&s, -1);       refresh(&s); continue; }
        if (c == 14) { history_move(&s, +1);       refresh(&s); continue; }
        if (c == 11) { le_snap(&s); del_range(&s, s.pos, s.len, 1); refresh(&s); continue; }
        if (c == 7)  { bell(); continue; }               /* ^G */
        if (c == 15) {                                  /* ^O */
            int count = shell_history_count_hosted();
            g_next_hist = s.hist < count
                              ? (long)shell_history_base_hosted() + s.hist + 1 : -1;
            le_below(&s);
            ret = (long)s.len;
            break;
        }
        if (c == 29) { char_search(&s, +1); refresh(&s); continue; }   /* ^] */
        if (c == 0)  { s.mark = s.pos; continue; }       /* ^@ */
        if (c == 24) {                                  /* ^X ... */
            int a = read_key();
            if (a == 21 || a == 31) { if (!le_undo(&s)) bell(); refresh(&s); }   /* ^X^U */
            else if (a == 24) {                         /* ^X^X */
                size_t m = s.mark <= s.len ? s.mark : s.len;
                s.mark = s.pos;
                s.pos = m;
                refresh(&s);
            } else {
                bell();
            }
            continue;
        }
        if (c == 12) {
            char cl[12];
            int n = snprintf(cl, sizeof cl, "%c[H%c[2J", 27, 27);
            out(cl, (size_t)n);
            s.painted = 0;
            s.crow = 0;
            refresh(&s);
            continue;
        }
        if (c == 27) {                                  /* ESC: Meta */
            int a = read_key();
            if (a < 0) continue;
            if (le_escape_seq(&s, a)) continue;
            switch (a) {
            case 'b': s.pos = word_left(&s, s.pos);  refresh(&s); break;
            case 'f': s.pos = word_right(&s, s.pos); refresh(&s); break;
            case 'd': le_snap(&s); del_range(&s, s.pos, word_right(&s, s.pos), 1); s.last = LC_KILL_F; refresh(&s); break;
            case 127: case 8:
                le_snap(&s); del_range(&s, word_left(&s, s.pos), s.pos, -1); s.last = LC_KILL_B; refresh(&s); break;
            case 'u': case 'l': case 'c': le_snap(&s); case_word(&s, (char)a); refresh(&s); break;
            case 't': le_snap(&s); transpose_words(&s); refresh(&s); break;
            case 'r':
                le_snap(&s);
                set_line(&s, s.orig ? s.orig : "");
                refresh(&s);
                break;
            case '.': case '_':
                le_snap(&s);
                yank_last_arg(&s, last == LC_YANK + 1);
                s.last = LC_YANK + 1;
                refresh(&s);
                break;
            case 25: {                                  /* M-^Y: yank-nth-arg */
                int idx = shell_history_count_hosted() - 1;
                const char *h = idx >= 0 ? shell_history_get_hosted(idx) : 0;
                size_t b = 0, e = 0;
                if (!h || !le_hist_word(h, 1, &b, &e)) { bell(); break; }
                le_snap(&s);
                insert_n(&s, h + b, e - b);
                refresh(&s);
                break;
            }
            case 'y':                                   /* yank-pop */
                if (last == LC_YANK && g_killn > 1 && g_yank_to <= s.len &&
                    g_yank_from < g_yank_to) {
                    del_range(&s, g_yank_from, g_yank_to, 0);
                    s.pos = g_yank_from;
                    g_killi = (g_killi + 1) % g_killn;
                    insert_n(&s, g_kill[g_killi], strlen(g_kill[g_killi]));
                    g_yank_to = s.pos;
                    s.last = LC_YANK;
                    refresh(&s);
                } else {
                    bell();
                }
                break;
            case '#':
                le_snap(&s);
                insert_comment(&s, false);
                refresh(&s);
                le_below(&s);
                ret = (long)s.len;
                goto done;
            case '<': history_go(&s, 0); refresh(&s); break;
            case '>': history_go(&s, shell_history_count_hosted()); refresh(&s); break;
            case '\\': {                                /* delete-horizontal-space */
                size_t a2 = s.pos, b2 = s.pos;
                while (a2 > 0 && le_blank((unsigned char)s.buf[a2 - 1])) a2--;
                while (b2 < s.len && le_blank((unsigned char)s.buf[b2])) b2++;
                if (b2 > a2) { le_snap(&s); del_range(&s, a2, b2, 0); s.pos = a2; refresh(&s); }
                break;
            }
            case '?': case '=': list_completions(&s); break;
            case '*': insert_completions(&s); break;
            case 29: char_search(&s, -1); refresh(&s); break;   /* M-^] */
            case 10: case 13:                           /* M-^J: vi mode */
                s.vi = true;
                s.vcmd = false;
                shell_set_editing_mode_hosted(true);
                break;
            default:
                bell();
                break;
            }
            continue;
        }
        if (c >= 32 && c != 127) {
            if (last != LC_INSERT) le_snap(&s);
            if (c >= 0x80 && s.u8) {
                char mb[4];
                size_t n = read_char_rest(c, mb);
                insert_n(&s, mb, n);
            } else {
                insert(&s, (char)c);
            }
            s.last = LC_INSERT;
            refresh(&s);
        }
    }
done:
    free(s.saved);
    free(s.orig);
    le_undo_clear(&s);
    free(s.undo);
    g_recording = false;
    if (tty) (void)tcsetattr(0, TCSANOW, &orig);
    return ret;
}
