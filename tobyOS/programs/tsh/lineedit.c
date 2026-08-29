/* lineedit.c -- the readline subset for /bin/tsh. See lineedit.h. */
#include "lineedit.h"
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

extern int         shell_history_count_hosted(void);
extern const char *shell_history_get_hosted(int i);

#define LE_MAX 4096

struct le {
    char        *buf;
    size_t       cap;
    size_t       len, pos;
    const char  *prompt;
    size_t       plen;
    int          hist;           /* == count while on the live line */
    int          painted;        /* has anything been drawn for this line yet */
    char         saved[LE_MAX];  /* live line, parked while browsing history */
};

static char g_yank[LE_MAX];      /* ^K/^U/^W cut text, pasted by ^Y */

static void out(const char *s, size_t n) { (void)!write(1, s, n); }
static void outs(const char *s) { out(s, strlen(s)); }

/* Repaint: CR, prompt, buffer, erase-to-EOL, then walk the cursor back.
 * ONE write per refresh -- on a 38400-baud console a per-character redraw is
 * visibly slow and interleaves badly with anything else printing.
 *
 * Deliberately single-line: a buffer wider than the terminal wraps and the
 * cursor arithmetic stops being right. Fixing that needs the window width and
 * a multi-line redraw, which is more than this earns today. */
static void refresh(struct le *s) {
    char o[LE_MAX + 128];
    size_t k = 0;

    /* FIRST paint of a line: emit the prompt and buffer plainly, with no CR
     * and no erase. Two reasons, and the second is not cosmetic.
     *
     * Nothing has been drawn yet, so there is nothing to erase or rewind --
     * this is what readline does too.
     *
     * And the interactive parity gate frames its whole protocol on PROMPT
     * SENTINELS (see the note in host.c). Wrapping a fresh prompt as
     * "\r" + prompt + ESC[K makes those bytes stop matching, so the harness
     * waits for a prompt it will never recognise, times out, and SIGKILLs
     * tsh -- ttyparity went from 11/11 to 0/11 BROKEN, with the shell simply
     * blocked in read(). A line editor may repaint itself freely, but the
     * bytes of an untouched prompt have to stay exactly what they were. */
    if (!s->painted) {
        s->painted = 1;
        for (size_t i = 0; i < s->plen && k < sizeof o - 8; i++) o[k++] = s->prompt[i];
        for (size_t i = 0; i < s->len && k < sizeof o - 8; i++) o[k++] = s->buf[i];
        if (k) out(o, k);
        return;
    }

    o[k++] = '\r';
    for (size_t i = 0; i < s->plen && k < sizeof o - 32; i++) o[k++] = s->prompt[i];
    for (size_t i = 0; i < s->len && k < sizeof o - 32; i++) o[k++] = s->buf[i];
    o[k++] = 27; o[k++] = '['; o[k++] = 'K';
    size_t back = s->len - s->pos;
    if (back > 0) {
        char n[16];
        int nl = snprintf(n, sizeof n, "%c[%uD", 27, (unsigned)back);
        for (int i = 0; i < nl && k < sizeof o; i++) o[k++] = n[i];
    }
    out(o, k);
}

static void set_line(struct le *s, const char *t) {
    size_t n = t ? strlen(t) : 0;
    if (n >= s->cap) n = s->cap - 1;
    if (n) memcpy(s->buf, t, n);
    s->buf[n] = 0;
    s->len = s->pos = n;
}

static void insert(struct le *s, char c) {
    if (s->len + 1 >= s->cap) return;
    memmove(s->buf + s->pos + 1, s->buf + s->pos, s->len - s->pos);
    s->buf[s->pos++] = c;
    s->len++;
    s->buf[s->len] = 0;
}

static void del_range(struct le *s, size_t from, size_t to, int yank) {
    if (to > s->len) to = s->len;
    if (to <= from) return;
    if (yank) {
        size_t n = to - from;
        if (n >= sizeof g_yank) n = sizeof g_yank - 1;
        memcpy(g_yank, s->buf + from, n);
        g_yank[n] = 0;
    }
    memmove(s->buf + from, s->buf + to, s->len - to);
    s->len -= (to - from);
    s->buf[s->len] = 0;
    if (s->pos >= to)       s->pos -= (to - from);
    else if (s->pos > from) s->pos = from;
    if (s->pos > s->len) s->pos = s->len;
}

/* Word motion the way bash does it: runs of non-space split by spaces. */
static size_t word_left(struct le *s) {
    size_t p = s->pos;
    while (p > 0 && s->buf[p - 1] == ' ') p--;
    while (p > 0 && s->buf[p - 1] != ' ') p--;
    return p;
}

static size_t word_right(struct le *s) {
    size_t p = s->pos;
    while (p < s->len && s->buf[p] == ' ') p++;
    while (p < s->len && s->buf[p] != ' ') p++;
    return p;
}

/* dir -1 = older, +1 = newer. The half-typed line is parked in s->saved so
 * walking up and back down returns it intact -- losing it is the single most
 * irritating way to get history wrong. */
static void history_move(struct le *s, int dir) {
    int count = shell_history_count_hosted();
    if (count <= 0) return;
    if (s->hist >= count) {
        size_t n = s->len < sizeof s->saved - 1 ? s->len : sizeof s->saved - 1;
        memcpy(s->saved, s->buf, n);
        s->saved[n] = 0;
    }
    int want = s->hist + dir;
    if (want < 0) want = 0;
    if (want > count) want = count;
    if (want == s->hist) return;
    s->hist = want;
    set_line(s, want == count ? s->saved : shell_history_get_hosted(want));
}

/* ^R incremental reverse search. 1 = accept and run, 0 = accept and keep
 * editing, -1 = interrupted. */
static int reverse_search(struct le *s) {
    char q[128];
    size_t qn = 0;
    q[0] = 0;
    int found = -1;
    for (;;) {
        char disp[224];
        snprintf(disp, sizeof disp, "(reverse-i-search)%s: ", q);
        struct le tmp = *s;
        tmp.prompt = disp;
        tmp.plen = strlen(disp);
        tmp.painted = 1;      /* the search prompt always REPLACES a line */
        if (found >= 0) set_line(&tmp, shell_history_get_hosted(found));
        refresh(&tmp);

        char c;
        if (read(0, &c, 1) <= 0) return 0;
        if (c == 3) return -1;
        if (c == 27 || c == 13 || c == 10) {
            if (found >= 0) set_line(s, shell_history_get_hosted(found));
            return (c == 13 || c == 10) ? 1 : 0;
        }
        int again = 0;
        if (c == 127 || c == 8) {
            if (qn) q[--qn] = 0;
        } else if (c == 18) {
            again = 1;
        } else if ((unsigned char)c >= 32 && qn + 1 < sizeof q) {
            q[qn++] = c;
            q[qn] = 0;
        } else {
            continue;
        }

        int count = shell_history_count_hosted();
        int start = (again && found > 0) ? found - 1 : count - 1;
        found = -1;
        for (int i = start; i >= 0; i--) {
            const char *h = shell_history_get_hosted(i);
            if (!h) continue;
            if (qn == 0 || strstr(h, q)) { found = i; break; }
        }
    }
}

int tsh_lineedit_available(void) {
    struct termios t;
    return isatty(0) && isatty(1) && tcgetattr(0, &t) == 0;
}

long tsh_readline(const char *prompt, char *buf, size_t cap) {
    struct termios orig, raw;
    if (!buf || cap < 2) return TSH_LE_EOF;
    if (tcgetattr(0, &orig) != 0) return TSH_LE_EOF;
    raw = orig;
    /* ICANON off so every keystroke arrives; ECHO off because we paint the
     * line ourselves; ISIG off because ^C belongs to the editor while a line
     * is being typed. All restored before returning, so a COMMAND runs under
     * a normal tty and job control still routes ^C/^Z to the foreground job. */
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) != 0) return TSH_LE_EOF;

    struct le s;
    memset(&s, 0, sizeof s);
    s.buf = buf;
    s.cap = cap > LE_MAX ? LE_MAX : cap;
    s.prompt = prompt ? prompt : "";
    s.plen = strlen(s.prompt);
    s.hist = shell_history_count_hosted();
    buf[0] = 0;
    refresh(&s);

    long ret = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) <= 0) {
            ret = (s.len == 0) ? TSH_LE_EOF : (long)s.len;
            break;
        }

        if (c == 13 || c == 10) { outs("\r\n"); ret = (long)s.len; break; }
        if (c == 3) { outs("^C\r\n"); ret = TSH_LE_INTR; break; }
        if (c == 4) {                                   /* ^D */
            if (s.len == 0) { outs("\r\n"); ret = TSH_LE_EOF; break; }
            if (s.pos < s.len) { del_range(&s, s.pos, s.pos + 1, 0); refresh(&s); }
            continue;
        }
        if (c == 127 || c == 8) {                       /* backspace */
            if (s.pos > 0) { del_range(&s, s.pos - 1, s.pos, 0); refresh(&s); }
            continue;
        }
        if (c == 1)  { s.pos = 0;                  refresh(&s); continue; }
        if (c == 5)  { s.pos = s.len;              refresh(&s); continue; }
        if (c == 2)  { if (s.pos) s.pos--;         refresh(&s); continue; }
        if (c == 6)  { if (s.pos < s.len) s.pos++; refresh(&s); continue; }
        if (c == 16) { history_move(&s, -1);       refresh(&s); continue; }
        if (c == 14) { history_move(&s, +1);       refresh(&s); continue; }
        if (c == 11) { del_range(&s, s.pos, s.len, 1); refresh(&s); continue; }
        if (c == 21) { del_range(&s, 0, s.pos, 1);     refresh(&s); continue; }
        if (c == 23) { del_range(&s, word_left(&s), s.pos, 1); refresh(&s); continue; }
        if (c == 25) {
            for (const char *y = g_yank; *y; y++) insert(&s, *y);
            refresh(&s);
            continue;
        }
        if (c == 12) {
            char cl[12];
            int n = snprintf(cl, sizeof cl, "%c[H%c[2J", 27, 27);
            out(cl, (size_t)n);
            refresh(&s);
            continue;
        }
        if (c == 20) {                                  /* ^T transpose */
            if (s.len >= 2) {
                size_t i = (s.pos == 0) ? 0
                         : (s.pos >= s.len ? s.len - 2 : s.pos - 1);
                char t = s.buf[i];
                s.buf[i] = s.buf[i + 1];
                s.buf[i + 1] = t;
                if (s.pos < s.len) s.pos++;
                refresh(&s);
            }
            continue;
        }
        if (c == 18) {                                  /* ^R */
            int rs = reverse_search(&s);
            if (rs < 0) { outs("^C\r\n"); ret = TSH_LE_INTR; break; }
            refresh(&s);
            if (rs == 1) { outs("\r\n"); ret = (long)s.len; break; }
            continue;
        }
        if (c == 27) {                                  /* ESC ... */
            char a, b;
            if (read(0, &a, 1) <= 0) continue;
            if (a == 'b') { s.pos = word_left(&s);  refresh(&s); continue; }
            if (a == 'f') { s.pos = word_right(&s); refresh(&s); continue; }
            if (a == 'd') { del_range(&s, s.pos, word_right(&s), 1); refresh(&s); continue; }
            if (a == 127) { del_range(&s, word_left(&s), s.pos, 1);  refresh(&s); continue; }
            if (a != '[' && a != 'O') continue;
            if (read(0, &b, 1) <= 0) continue;
            if (b == 'A') { history_move(&s, -1); refresh(&s); continue; }
            if (b == 'B') { history_move(&s, +1); refresh(&s); continue; }
            if (b == 'C') { if (s.pos < s.len) s.pos++; refresh(&s); continue; }
            if (b == 'D') { if (s.pos) s.pos--;         refresh(&s); continue; }
            if (b == 'H') { s.pos = 0;     refresh(&s); continue; }
            if (b == 'F') { s.pos = s.len; refresh(&s); continue; }
            if (b >= '0' && b <= '9') {                 /* ESC [ n ~ */
                char t2;
                if (read(0, &t2, 1) <= 0) continue;
                if (t2 != '~') continue;
                if (b == '1' || b == '7')      { s.pos = 0;     refresh(&s); }
                else if (b == '4' || b == '8') { s.pos = s.len; refresh(&s); }
                else if (b == '3' && s.pos < s.len) {
                    del_range(&s, s.pos, s.pos + 1, 0);
                    refresh(&s);
                }
            }
            continue;
        }
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
            insert(&s, c);
            refresh(&s);
        }
    }

    (void)tcsetattr(0, TCSANOW, &orig);
    return ret;
}
