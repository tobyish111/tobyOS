/* lineedit.c -- the readline subset for /bin/tsh. See lineedit.h. */
#include "lineedit.h"
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

/* Provided by src/shell.c: the completion specs registered by
 * `complete -W`, and the builtin names hosted tsh will run. */
const char *tsh_complete_wordlist(const char *cmdname);
const char *tsh_builtin_name_at(int i);

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

#define LE_MATCH_MAX 256

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
    char  v[LE_MATCH_MAX][128];
    int   n;
};

static void le_add(struct le_matches *m, const char *name, int is_dir) {
    if (m->n >= LE_MATCH_MAX) return;
    size_t i = 0;
    for (; name[i] && i < sizeof(m->v[0]) - 2; i++) m->v[m->n][i] = name[i];
    if (is_dir && i < sizeof(m->v[0]) - 2) m->v[m->n][i++] = '/';
    m->v[m->n][i] = 0;
    m->n++;
}

/* Split "dir/partial" into the directory to scan and the prefix to match. */
static void le_split(const char *word, char *dir, size_t dcap,
                     char *pre, size_t pcap) {
    const char *slash = 0;
    for (const char *p = word; *p; p++) if (*p == '/') slash = p;
    if (!slash) {
        dir[0] = '.'; dir[1] = 0;
        size_t i = 0;
        for (; word[i] && i + 1 < pcap; i++) pre[i] = word[i];
        pre[i] = 0;
        return;
    }
    size_t dl = (size_t)(slash - word) + 1;          /* keep the slash */
    if (dl >= dcap) dl = dcap - 1;
    for (size_t i = 0; i < dl; i++) dir[i] = word[i];
    dir[dl] = 0;
    size_t i = 0;
    for (const char *p = slash + 1; *p && i + 1 < pcap; p++) pre[i++] = *p;
    pre[i] = 0;
}

static void le_scan_dir(struct le_matches *m, const char *dir, const char *pre) {
    DIR *d = opendir(dir);
    if (!d) return;
    size_t pl = strlen(pre);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        /* A dotfile only shows when the user has typed the dot, as in bash. */
        if (e->d_name[0] == '.' && (pl == 0 || pre[0] != '.')) continue;
        if (pl && strncmp(e->d_name, pre, pl) != 0) continue;
        char full[512];
        snprintf(full, sizeof full, "%s%s%s", dir,
                 (dir[strlen(dir) - 1] == '/') ? "" : "/", e->d_name);
        struct stat st;
        int isdir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        le_add(m, e->d_name, isdir);
    }
    closedir(d);
}

/* Commands: every executable name on PATH, plus the shell's own builtins. */
static void le_scan_commands(struct le_matches *m, const char *pre) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    size_t pl = strlen(pre);
    char dir[256];
    const char *p = path;
    while (*p) {
        size_t i = 0;
        while (*p && *p != ':' && i + 1 < sizeof dir) dir[i++] = *p++;
        dir[i] = 0;
        if (*p == ':') p++;
        if (!i) continue;
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != 0) {
            if (e->d_name[0] == '.') continue;
            if (pl && strncmp(e->d_name, pre, pl) != 0) continue;
            int dup = 0;
            for (int k = 0; k < m->n; k++)
                if (!strcmp(m->v[k], e->d_name)) { dup = 1; break; }
            if (!dup) le_add(m, e->d_name, 0);
        }
        closedir(d);
    }
    for (int i = 0; ; i++) {
        const char *bn = tsh_builtin_name_at(i);
        if (!bn) break;
        if (pl && strncmp(bn, pre, pl) != 0) continue;
        int dup = 0;
        for (int k = 0; k < m->n; k++)
            if (!strcmp(m->v[k], bn)) { dup = 1; break; }
        if (!dup) le_add(m, bn, 0);
    }
}

static int le_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void do_complete(struct le *s) {
    size_t start = le_word_start(s);
    char word[256];
    size_t wl = s->pos - start;
    if (wl >= sizeof word) return;
    memcpy(word, s->buf + start, wl);
    word[wl] = 0;

    struct le_matches m;
    m.n = 0;

    /* A `complete -W` spec for the COMMAND wins over the filesystem: that is
     * the whole point of registering one. */
    char cmdname[128];
    cmdname[0] = 0;
    if (!le_is_command_slot(s, start)) {
        size_t i = 0;
        while (i < s->len && (s->buf[i] == ' ' || s->buf[i] == '\t')) i++;
        size_t k = 0;
        while (i < s->len && s->buf[i] != ' ' && s->buf[i] != '\t' &&
               k + 1 < sizeof cmdname) cmdname[k++] = s->buf[i++];
        cmdname[k] = 0;
    }
    const char *wl_spec = cmdname[0] ? tsh_complete_wordlist(cmdname) : 0;
    if (wl_spec) {
        size_t pl = strlen(word);
        const char *p = wl_spec;
        while (*p) {
            while (*p == ' ') p++;
            char tok[128];
            size_t k = 0;
            while (*p && *p != ' ' && k + 1 < sizeof tok) tok[k++] = *p++;
            tok[k] = 0;
            if (k && (!pl || strncmp(tok, word, pl) == 0)) le_add(&m, tok, 0);
        }
    } else if (le_is_command_slot(s, start) && !strchr(word, '/')) {
        le_scan_commands(&m, word);
    } else {
        char dir[256], pre[256];
        le_split(word, dir, sizeof dir, pre, sizeof pre);
        le_scan_dir(&m, dir, pre);
    }

    if (m.n == 0) return;
    qsort(m.v, (size_t)m.n, sizeof m.v[0], le_cmp);

    /* How much of the word is already typed -- only the part AFTER the last
     * slash was matched against, so only that part gets replaced. */
    size_t typed = strlen(word);
    const char *lastslash = strrchr(word, '/');
    if (lastslash && !wl_spec) typed = strlen(lastslash + 1);

    if (m.n == 1) {
        const char *v = m.v[0];
        size_t vl = strlen(v);
        for (size_t i = typed; i < vl; i++) insert(s, v[i]);
        if (vl && v[vl - 1] != '/') insert(s, ' ');
        refresh(s);
        return;
    }

    /* Longest common prefix across the candidates. */
    size_t lcp = strlen(m.v[0]);
    for (int i = 1; i < m.n; i++) {
        size_t k = 0;
        while (k < lcp && m.v[i][k] && m.v[0][k] == m.v[i][k]) k++;
        lcp = k;
    }
    if (lcp > typed) {
        for (size_t i = typed; i < lcp; i++) insert(s, m.v[0][i]);
        refresh(s);
        return;
    }

    /* Nothing left to insert -- now it is worth showing the list. */
    outs("\r\n");
    for (int i = 0; i < m.n; i++) {
        outs(m.v[i]);
        outs((i + 1) % 4 == 0 ? "\r\n" : "  ");
    }
    if (m.n % 4) outs("\r\n");
    s->painted = 0;      /* the prompt must be drawn fresh under the list */
    refresh(s);
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
        if (c == 9)  { do_complete(&s);            continue; }   /* Tab */
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
