/* user_gui_browser/main.c -- /bin/gui_browser, Chrome-like web browser.
 *
 * Features:
 *   - Chrome-style tab bar with page title
 *   - Navigation: Back, Forward, Refresh, Home buttons (keyboard + mouse)
 *   - Styled address bar with focus highlighting
 *   - Basic HTML rendering (strips tags, renders headings/links/lists)
 *   - Page history with back/forward navigation
 *   - Link extraction and numbered link navigation
 *   - Loading status indicator
 *   - Scroll with j/k/d/u, arrow keys, or mouse scrollbar
 *   - HTTP and HTTPS support (kernel TLS 1.3)
 *   - Mouse click navigation and link following
 *   - Find in page (Ctrl+F)
 *   - Page source view (Shift+S)
 *   - Link hover preview in status bar
 *   - Better HTML entity decoding (numeric, named)
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define NULL ((void *)0)

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_HTTP_GET       79

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};
#define GUI_EV_MOUSE_MOVE 1
#define GUI_EV_MOUSE_DOWN 2
#define GUI_EV_KEY 4
#define GUI_EV_CLOSE 5

/* Migrated to the TobyTK toolkit (toby/tk.h): the whole browser chrome + the
 * monospace content grid render through a full-window TK_CANVAS, and the
 * drawing helpers below now forward to tk_draw_*. All HTML rendering, history,
 * link and find logic is unchanged. */
#include <toby/tk.h>
static struct tk_window win;

/* ---- Syscall stubs --------------------------------------------- */

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
__attribute__((noreturn))
static inline void sys_exit(int code) {
    __asm__ volatile ("syscall"
        : : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory");
    for (;;) { }
}
/* Drawing now forwards to TobyTK. The content grid is monospace (CELL_W=8), so
 * text uses tk_draw_text_mono (column-aligned, opaque bg). `fd` is ignored. */
static inline int sys_gui_fill(int fd, int x, int y, int w, int h, uint32_t color) {
    (void)fd; tk_draw_fill(&win, x, y, w, h, color); return 0;
}
static inline int sys_gui_text(int fd, int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    (void)fd; tk_draw_text_mono(&win, x, y, s, fg, bg); return 0;
}
static inline long sys_http_get(const char *url, void *buf, uint32_t buf_sz) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_GET), "D"(url), "S"(buf), "d"((long)buf_sz)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- Utility functions ----------------------------------------- */

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_ncasecmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 1;
        if (!ca) return 0;
    }
    return 0;
}

static void mem_zero(void *dst, size_t n) {
    char *d = (char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0;
}

static int str_contains(const char *haystack, int hlen, const char *needle, int nlen) {
    if (nlen == 0) return -1;
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match) return i;
    }
    return -1;
}

/* ---- Window Layout --------------------------------------------- */

#define WIN_W        720
#define WIN_H        500

#define TAB_BAR_H     22
#define NAV_BAR_H     28
#define TOOLBAR_H     (TAB_BAR_H + NAV_BAR_H)
#define STATUS_H      16
#define CONTENT_TOP   TOOLBAR_H
#define CONTENT_H     (WIN_H - TOOLBAR_H - STATUS_H)
#define PAD            6
#define CELL_W         8
#define CELL_H        12
#define COLS         ((WIN_W - 2 * PAD) / CELL_W)
#define ROWS         (CONTENT_H / CELL_H)

/* ---- Colors (Chrome dark theme inspired) ----------------------- */

#define COL_TAB_BG       0x00202124u
#define COL_TAB_ACTIVE   0x00292A2Du
#define COL_TAB_TEXT     0x00E8EAEDu
#define COL_TAB_CLOSE    0x00FF6060u

#define COL_NAV_BG       0x00292A2Du
#define COL_NAV_BTN      0x009AA0A6u
#define COL_NAV_BTN_DIM  0x005F6368u

#define COL_URL_BG       0x00202124u
#define COL_URL_FOCUS_BG 0x00171819u
#define COL_URL_BORDER   0x003C4043u
#define COL_URL_FOCUS_BD 0x008AB4F8u
#define COL_URL_TEXT     0x00E8EAEDu
#define COL_URL_HINT     0x009AA0A6u
#define COL_URL_SCHEME   0x009AA0A6u
#define COL_URL_CURSOR   0x008AB4F8u

#define COL_PAGE_BG      0x00202124u
#define COL_TEXT_FG      0x00BDC1C6u
#define COL_H1_FG        0x00E8EAEDu
#define COL_H2_FG        0x00D2D4D7u
#define COL_H3_FG        0x00C4C7CBu
#define COL_LINK_FG      0x008AB4F8u
#define COL_BOLD_FG      0x00E8EAEDu
#define COL_CODE_BG      0x002D2E31u
#define COL_CODE_FG      0x0087CEEBu
#define COL_LIST_FG      0x008AB4F8u
#define COL_HR_FG        0x003C4043u

#define COL_STATUS_BG    0x00202124u
#define COL_STATUS_FG    0x009AA0A6u
#define COL_STATUS_LINK  0x008AB4F8u

#define COL_FIND_BG      0x00353639u
#define COL_FIND_FG      0x00E8EAEDu
#define COL_FIND_HL      0x00F1C232u

#define COL_HTTPS_FG     0x0081C995u

/* Heading sentinel bytes: placed at start of heading lines during render,
 * detected during line-index build to tag styles, then removed for display */
#define SENTINEL_H1  0x01
#define SENTINEL_H2  0x02
#define SENTINEL_H3  0x03

/* ---- Content buffer & HTML renderer ---------------------------- */

#define RAW_CAP       (32 * 1024)
static char g_raw[RAW_CAP + 1];
static long g_raw_len = 0;

#define RENDER_CAP    (48 * 1024)
static char g_render[RENDER_CAP + 1];
static long g_render_len = 0;

#define LINES_MAX     4096
static long g_line_off[LINES_MAX];
static int  g_line_count = 0;
static int  g_top_line   = 0;

#define STYLE_NORMAL   0
#define STYLE_H1       1
#define STYLE_H2       2
#define STYLE_H3       3
#define STYLE_LINK     4
#define STYLE_CODE     5
#define STYLE_BULLET   6
#define STYLE_HR       7
#define STYLE_BOLD     8

static uint8_t g_line_style[LINES_MAX];

#define LINK_MAX       128
#define LINK_URL_MAX   256
static char g_links[LINK_MAX][LINK_URL_MAX];
static int  g_link_count = 0;

#define URL_MAX        1024
static char g_url[URL_MAX + 1];
static int  g_url_len = 0;

#define TITLE_MAX      64
static char g_title[TITLE_MAX + 1];

#define HISTORY_MAX    32
static char g_history[HISTORY_MAX][URL_MAX + 1];
static int  g_hist_pos  = -1;
static int  g_hist_count = 0;

static int  g_focus_url = 1;
static char g_status_text[128];
static int  g_loading = 0;

/* Find-in-page state */
#define FIND_MAX 64
static int  g_find_mode = 0;
static char g_find_buf[FIND_MAX + 1];
static int  g_find_len = 0;
static int  g_find_line = -1;

/* Source view toggle */
static int  g_source_view = 0;

/* ---- HTML Parser/Renderer -------------------------------------- */

static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int tag_match(const char *src, const char *tag) {
    while (*tag) {
        char cs = *src, ct = *tag;
        if (cs >= 'A' && cs <= 'Z') cs += 32;
        if (ct >= 'A' && ct <= 'Z') ct += 32;
        if (cs != ct) return 0;
        src++; tag++;
    }
    return (*src == '>' || *src == ' ' || *src == '/' || *src == '\0');
}

static void emit_char(char c) {
    if (g_render_len < RENDER_CAP)
        g_render[g_render_len++] = c;
}

static void emit_str(const char *s) {
    while (*s && g_render_len < RENDER_CAP)
        g_render[g_render_len++] = *s++;
}

static void emit_newline(void) {
    emit_char('\n');
}

/* Entity matching helper: returns 1 if src[pos..] starts with `name;` */
static int entity_match(const char *src, long pos, long len, const char *name) {
    int i = 0;
    while (name[i]) {
        if (pos + i >= len) return 0;
        if (src[pos + i] != name[i]) return 0;
        i++;
    }
    if (pos + i >= len) return 0;
    return src[pos + i] == ';';
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Decode HTML entity at src[i] (which is '&'). Returns chars consumed after '&',
 * writes decoded char to *out. Returns 0 if not recognized. */
static int decode_entity(const char *src, long i, long len, char *out) {
    long start = i + 1;

    if (start >= len) return 0;

    /* Numeric: &#NNN; or &#xHH; */
    if (src[start] == '#') {
        long p = start + 1;
        int val = 0;
        if (p < len && (src[p] == 'x' || src[p] == 'X')) {
            p++;
            int digits = 0;
            while (p < len && hex_digit(src[p]) >= 0 && digits < 6) {
                val = val * 16 + hex_digit(src[p]);
                p++; digits++;
            }
            if (digits == 0) return 0;
        } else {
            int digits = 0;
            while (p < len && src[p] >= '0' && src[p] <= '9' && digits < 7) {
                val = val * 10 + (src[p] - '0');
                p++; digits++;
            }
            if (digits == 0) return 0;
        }
        if (p < len && src[p] == ';') {
            if (val > 0 && val < 127) *out = (char)val;
            else *out = '?';
            return (int)(p - i);
        }
        return 0;
    }

    /* Named entities */
    if (entity_match(src, start, len, "lt"))    { *out = '<'; return 3; }
    if (entity_match(src, start, len, "gt"))    { *out = '>'; return 3; }
    if (entity_match(src, start, len, "amp"))   { *out = '&'; return 4; }
    if (entity_match(src, start, len, "nbsp"))  { *out = ' '; return 5; }
    if (entity_match(src, start, len, "quot"))  { *out = '"'; return 5; }
    if (entity_match(src, start, len, "apos"))  { *out = '\''; return 5; }

    /* Multi-char entities: emit string directly */
    if (entity_match(src, start, len, "mdash"))  { *out = '-'; return 6; }
    if (entity_match(src, start, len, "ndash"))  { *out = '-'; return 6; }
    if (entity_match(src, start, len, "copy"))   { *out = 0; return 5; }
    if (entity_match(src, start, len, "reg"))    { *out = 0; return 4; }
    if (entity_match(src, start, len, "hellip")) { *out = 0; return 7; }
    if (entity_match(src, start, len, "laquo"))  { *out = 0; return 6; }
    if (entity_match(src, start, len, "raquo"))  { *out = 0; return 6; }

    return 0;
}

/* Handle multi-char entity expansions that decode_entity signals with out=0 */
static int emit_multi_entity(const char *src, long start, long len) {
    if (entity_match(src, start + 1, len, "mdash"))  { emit_char('-'); emit_char('-'); return 6; }
    if (entity_match(src, start + 1, len, "ndash"))  { emit_char('-'); return 6; }
    if (entity_match(src, start + 1, len, "copy"))   { emit_char('('); emit_char('c'); emit_char(')'); return 5; }
    if (entity_match(src, start + 1, len, "reg"))    { emit_char('('); emit_char('R'); emit_char(')'); return 4; }
    if (entity_match(src, start + 1, len, "hellip")) { emit_char('.'); emit_char('.'); emit_char('.'); return 7; }
    if (entity_match(src, start + 1, len, "laquo"))  { emit_char('<'); emit_char('<'); return 6; }
    if (entity_match(src, start + 1, len, "raquo"))  { emit_char('>'); emit_char('>'); return 6; }
    return 0;
}

static void render_html(void) {
    g_render_len = 0;
    g_link_count = 0;
    g_title[0] = '\0';

    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int in_title = 0;
    int in_pre = 0;
    int line_has_content = 0;
    int title_pos = 0;
    int last_was_space = 0;
    int heading_level = 0;

    char tag_buf[64];
    int tag_buf_len = 0;

    char href_buf[LINK_URL_MAX];
    int href_len = 0;

    const char *src = g_raw;
    long len = g_raw_len;

    for (long i = 0; i < len; i++) {
        char c = src[i];

        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                tag_buf[tag_buf_len] = '\0';

                const char *t = tag_buf;
                int closing = 0;
                if (*t == '/') { closing = 1; t++; }

                if (tag_match(t, "script")) {
                    in_script = !closing;
                } else if (tag_match(t, "style")) {
                    in_style = !closing;
                } else if (tag_match(t, "title")) {
                    if (!closing) { in_title = 1; title_pos = 0; }
                    else { in_title = 0; g_title[title_pos] = '\0'; }
                } else if (tag_match(t, "pre") || tag_match(t, "code")) {
                    if (!closing) {
                        in_pre = 1;
                        if (line_has_content) { emit_newline(); line_has_content = 0; }
                    } else {
                        in_pre = 0;
                        if (line_has_content) { emit_newline(); line_has_content = 0; }
                    }
                } else if (tag_match(t, "h1") || tag_match(t, "h2") ||
                           tag_match(t, "h3") || tag_match(t, "h4") ||
                           tag_match(t, "h5") || tag_match(t, "h6")) {
                    if (!closing) {
                        if (line_has_content || g_render_len > 0) {
                            emit_newline(); emit_newline();
                        }
                        line_has_content = 0;
                        if (t[1] == '1') heading_level = 1;
                        else if (t[1] == '2') heading_level = 2;
                        else heading_level = 3;
                        /* Emit sentinel at start of heading line */
                        emit_char((char)heading_level);
                    } else {
                        heading_level = 0;
                        emit_newline();
                        line_has_content = 0;
                    }
                } else if (tag_match(t, "p") || tag_match(t, "div") ||
                           tag_match(t, "section") || tag_match(t, "article") ||
                           tag_match(t, "header") || tag_match(t, "footer") ||
                           tag_match(t, "main") || tag_match(t, "nav")) {
                    if (line_has_content) {
                        emit_newline();
                        if (!closing) emit_newline();
                        line_has_content = 0;
                    }
                    heading_level = 0;
                } else if (tag_match(t, "br")) {
                    emit_newline();
                    if (heading_level) emit_char((char)heading_level);
                    line_has_content = 0;
                } else if (tag_match(t, "hr")) {
                    if (line_has_content) emit_newline();
                    emit_str("────────────────────────────────────────");
                    emit_newline();
                    line_has_content = 0;
                } else if (tag_match(t, "li")) {
                    if (!closing) {
                        if (line_has_content) emit_newline();
                        emit_str("  * ");
                        line_has_content = 1;
                    } else {
                        if (line_has_content) emit_newline();
                        line_has_content = 0;
                    }
                } else if (tag_match(t, "ul") || tag_match(t, "ol")) {
                    if (line_has_content) emit_newline();
                    line_has_content = 0;
                } else if (tag_match(t, "a")) {
                    if (!closing) {
                        href_len = 0;
                        href_buf[0] = '\0';
                        const char *p = t;
                        while (*p && !is_whitespace(*p)) p++;
                        while (*p) {
                            if (str_ncasecmp(p, "href", 4) == 0) {
                                p += 4;
                                while (*p == ' ') p++;
                                if (*p == '=') {
                                    p++;
                                    while (*p == ' ') p++;
                                    char q = 0;
                                    if (*p == '"' || *p == '\'') { q = *p; p++; }
                                    while (*p && (q ? *p != q : !is_whitespace(*p) && *p != '>') &&
                                           href_len < LINK_URL_MAX - 1) {
                                        href_buf[href_len++] = *p++;
                                    }
                                    href_buf[href_len] = '\0';
                                }
                                break;
                            }
                            p++;
                        }
                        if (href_len > 0 && g_link_count < LINK_MAX) {
                            str_copy(g_links[g_link_count], href_buf, LINK_URL_MAX);
                            emit_char('[');
                            int ln = g_link_count + 1;
                            if (ln >= 100) emit_char('0' + (ln / 100) % 10);
                            if (ln >= 10) emit_char('0' + (ln / 10) % 10);
                            emit_char('0' + ln % 10);
                            emit_char(']');
                            g_link_count++;
                        }
                    }
                } else if (tag_match(t, "b") || tag_match(t, "strong")) {
                    /* inline bold */
                } else if (tag_match(t, "i") || tag_match(t, "em")) {
                    /* inline italic */
                } else if (tag_match(t, "img")) {
                    emit_str("[img]");
                    line_has_content = 1;
                } else if (tag_match(t, "table")) {
                    if (!closing && line_has_content) { emit_newline(); line_has_content = 0; }
                } else if (tag_match(t, "tr")) {
                    if (closing && line_has_content) { emit_newline(); line_has_content = 0; }
                } else if (tag_match(t, "td") || tag_match(t, "th")) {
                    if (!closing && line_has_content) emit_str(" | ");
                }

                last_was_space = 0;
            } else {
                if (tag_buf_len < 63)
                    tag_buf[tag_buf_len++] = c;
            }
            continue;
        }

        if (c == '<') {
            in_tag = 1;
            tag_buf_len = 0;
            continue;
        }

        if (in_script || in_style) continue;

        /* HTML entity decoding */
        if (c == '&') {
            /* Try multi-char entities first */
            int skip = emit_multi_entity(src, i, len);
            if (skip > 0) {
                i += skip;
                last_was_space = 0;
                line_has_content = 1;
                continue;
            }
            char decoded;
            skip = decode_entity(src, i, len, &decoded);
            if (skip > 0) {
                c = decoded;
                i += skip;
            } else {
                /* Unknown entity: skip to semicolon */
                long j = i + 1;
                while (j < len && j < i + 10 && src[j] != ';' && src[j] != '<') j++;
                if (j < len && src[j] == ';') { i = j; continue; }
            }
        }

        if (in_title) {
            if (title_pos < TITLE_MAX) g_title[title_pos++] = c;
            continue;
        }

        if (in_pre) {
            emit_char(c);
            line_has_content = (c != '\n');
            continue;
        }

        if (is_whitespace(c)) {
            if (line_has_content && !last_was_space) {
                emit_char(' ');
                last_was_space = 1;
            }
            continue;
        }

        last_was_space = 0;
        line_has_content = 1;
        emit_char(c);
    }

    g_render[g_render_len] = '\0';

    /* Build line index: detect sentinels for heading style tagging */
    g_line_count = 0;
    g_line_off[0] = 0;
    g_line_style[0] = STYLE_NORMAL;

    /* Check first char for sentinel */
    if (g_render_len > 0 && (unsigned char)g_render[0] >= SENTINEL_H1 &&
        (unsigned char)g_render[0] <= SENTINEL_H3) {
        int s = (unsigned char)g_render[0];
        g_line_style[0] = (s == SENTINEL_H1) ? STYLE_H1 :
                          (s == SENTINEL_H2) ? STYLE_H2 : STYLE_H3;
        g_line_off[0] = 1;
    }
    g_line_count = 1;

    int word_wrap_col = COLS - 2;
    int col = 0;

    for (long i = g_line_off[0]; i < g_render_len && g_line_count < LINES_MAX; i++) {
        if (g_render[i] == '\n') {
            col = 0;
            if (g_line_count < LINES_MAX && i + 1 < g_render_len) {
                long next_start = i + 1;
                uint8_t style = STYLE_NORMAL;
                /* Check for heading sentinel at line start */
                if (next_start < g_render_len &&
                    (unsigned char)g_render[next_start] >= SENTINEL_H1 &&
                    (unsigned char)g_render[next_start] <= SENTINEL_H3) {
                    int s = (unsigned char)g_render[next_start];
                    style = (s == SENTINEL_H1) ? STYLE_H1 :
                            (s == SENTINEL_H2) ? STYLE_H2 : STYLE_H3;
                    next_start++;
                }
                g_line_off[g_line_count] = next_start;
                g_line_style[g_line_count] = style;
                g_line_count++;
            }
        } else {
            col++;
            if (col >= word_wrap_col) {
                long wrap_at = i;
                long search = i;
                while (search > g_line_off[g_line_count - 1] && g_render[search] != ' ')
                    search--;
                if (search > g_line_off[g_line_count - 1])
                    wrap_at = search;

                if (g_line_count < LINES_MAX) {
                    g_line_off[g_line_count] = wrap_at + 1;
                    g_line_style[g_line_count] = g_line_style[g_line_count - 1];
                    g_line_count++;
                    i = wrap_at;
                    col = 0;
                }
            }
        }
    }

    /* Tag remaining line styles based on content patterns (bullets, links, hr) */
    for (int li = 0; li < g_line_count; li++) {
        if (g_line_style[li] != STYLE_NORMAL) continue;
        long start = g_line_off[li];
        if (g_render_len > start + 3 &&
            g_render[start] == (char)0xE2 && g_render[start+1] == (char)0x94 &&
            g_render[start+2] == (char)0x80) {
            g_line_style[li] = STYLE_HR;
        } else if (g_render_len > start + 2 &&
                   g_render[start] == ' ' && g_render[start+1] == ' ' &&
                   g_render[start+2] == '*') {
            g_line_style[li] = STYLE_BULLET;
        } else if (g_render_len > start + 1 && g_render[start] == '[' &&
                   g_render[start+1] >= '0' && g_render[start+1] <= '9') {
            g_line_style[li] = STYLE_LINK;
        }
    }
}

static void render_plain_text(void) {
    g_render_len = 0;
    g_link_count = 0;
    g_title[0] = '\0';
    str_copy(g_title, "Plain Text", TITLE_MAX);

    for (long i = 0; i < g_raw_len && g_render_len < RENDER_CAP; i++)
        g_render[g_render_len++] = g_raw[i];
    g_render[g_render_len] = '\0';

    g_line_count = 0;
    g_line_off[0] = 0;
    g_line_style[0] = STYLE_NORMAL;
    g_line_count = 1;

    int col = 0;
    int word_wrap_col = COLS - 2;
    for (long i = 0; i < g_render_len && g_line_count < LINES_MAX; i++) {
        if (g_render[i] == '\n') {
            col = 0;
            if (i + 1 < g_render_len && g_line_count < LINES_MAX) {
                g_line_off[g_line_count] = i + 1;
                g_line_style[g_line_count] = STYLE_NORMAL;
                g_line_count++;
            }
        } else {
            col++;
            if (col >= word_wrap_col && g_line_count < LINES_MAX) {
                g_line_off[g_line_count] = i + 1;
                g_line_style[g_line_count] = STYLE_NORMAL;
                g_line_count++;
                col = 0;
            }
        }
    }
}

/* Source view: show raw HTML with line wrapping */
static void render_source_view(void) {
    g_render_len = 0;
    g_link_count = 0;
    str_copy(g_title, "Source View", TITLE_MAX);

    for (long i = 0; i < g_raw_len && g_render_len < RENDER_CAP; i++)
        g_render[g_render_len++] = g_raw[i];
    g_render[g_render_len] = '\0';

    g_line_count = 0;
    g_line_off[0] = 0;
    g_line_style[0] = STYLE_CODE;
    g_line_count = 1;

    int col = 0;
    int word_wrap_col = COLS - 2;
    for (long i = 0; i < g_render_len && g_line_count < LINES_MAX; i++) {
        if (g_render[i] == '\n') {
            col = 0;
            if (i + 1 < g_render_len && g_line_count < LINES_MAX) {
                g_line_off[g_line_count] = i + 1;
                g_line_style[g_line_count] = STYLE_CODE;
                g_line_count++;
            }
        } else {
            col++;
            if (col >= word_wrap_col && g_line_count < LINES_MAX) {
                g_line_off[g_line_count] = i + 1;
                g_line_style[g_line_count] = STYLE_CODE;
                g_line_count++;
                col = 0;
            }
        }
    }
}

static int is_html_content(void) {
    for (long i = 0; i < g_raw_len && i < 512; i++) {
        if (g_raw[i] == '<') {
            if (str_ncasecmp(&g_raw[i], "<html", 5) == 0 ||
                str_ncasecmp(&g_raw[i], "<!doc", 5) == 0 ||
                str_ncasecmp(&g_raw[i], "<head", 5) == 0 ||
                str_ncasecmp(&g_raw[i], "<body", 5) == 0 ||
                str_ncasecmp(&g_raw[i], "<div", 4) == 0 ||
                str_ncasecmp(&g_raw[i], "<p>", 3) == 0 ||
                str_ncasecmp(&g_raw[i], "<h1", 3) == 0)
                return 1;
        }
    }
    return 0;
}

/* ---- Navigation history ---------------------------------------- */

static void history_push(const char *url) {
    if (g_hist_pos >= 0 && str_eq(g_history[g_hist_pos], url))
        return;
    g_hist_pos++;
    if (g_hist_pos >= HISTORY_MAX) {
        for (int i = 0; i < HISTORY_MAX - 1; i++)
            str_copy(g_history[i], g_history[i + 1], URL_MAX);
        g_hist_pos = HISTORY_MAX - 1;
    }
    str_copy(g_history[g_hist_pos], url, URL_MAX);
    g_hist_count = g_hist_pos + 1;
}

static int can_go_back(void) { return g_hist_pos > 0; }
static int can_go_forward(void) { return g_hist_pos < g_hist_count - 1; }

/* ---- Page fetching --------------------------------------------- */

static void set_status(const char *s) {
    str_copy(g_status_text, s, sizeof(g_status_text));
}

static void do_navigate(const char *url);

static void resolve_relative_url(const char *base, const char *rel, char *out, int out_max) {
    if (rel[0] == 'h' && rel[1] == 't' && rel[2] == 't' && rel[3] == 'p') {
        str_copy(out, rel, out_max);
        return;
    }

    int origin_end = 0;
    int slash_count = 0;
    for (int i = 0; base[i]; i++) {
        if (base[i] == '/') {
            slash_count++;
            if (slash_count == 3) { origin_end = i; break; }
        }
    }
    if (origin_end == 0) origin_end = (int)str_len(base);

    if (rel[0] == '/') {
        int pos = 0;
        for (int i = 0; i < origin_end && pos < out_max - 1; i++)
            out[pos++] = base[i];
        for (int i = 0; rel[i] && pos < out_max - 1; i++)
            out[pos++] = rel[i];
        out[pos] = '\0';
    } else {
        int last_slash = origin_end;
        for (int i = origin_end; base[i]; i++)
            if (base[i] == '/') last_slash = i;

        int pos = 0;
        for (int i = 0; i <= last_slash && pos < out_max - 1; i++)
            out[pos++] = base[i];
        for (int i = 0; rel[i] && pos < out_max - 1; i++)
            out[pos++] = rel[i];
        out[pos] = '\0';
    }
}

static void set_home_page(void) {
    const char *html =
        "<html><head><title>New Tab</title></head><body>"
        "<h1>TobyOS Browser</h1>"
        "<p>Welcome to the TobyOS web browser. This is a Chrome-inspired "
        "browser built into the operating system.</p>"
        "<h2>Quick Start</h2>"
        "<ul>"
        "<li>Press <b>Tab</b> to focus the address bar</li>"
        "<li>Type a URL and press Enter</li>"
        "<li>Supports HTTP and HTTPS</li>"
        "<li>Use <b>j/k</b> to scroll, <b>d/u</b> for page scroll</li>"
        "<li>Press <b>[</b> to go back, <b>]</b> to go forward</li>"
        "<li>Click links or type link number + Enter</li>"
        "<li>Press <b>r</b> to refresh, <b>h</b> for home</li>"
        "</ul>"
        "<h2>Keyboard Shortcuts</h2>"
        "<ul>"
        "<li><b>Tab</b> - Toggle URL bar / content focus</li>"
        "<li><b>/</b> - Focus address bar</li>"
        "<li><b>[</b> - Navigate back</li>"
        "<li><b>]</b> - Navigate forward</li>"
        "<li><b>r</b> - Refresh page</li>"
        "<li><b>h</b> - Home page</li>"
        "<li><b>f</b> - Follow link (type number then Enter)</li>"
        "<li><b>Ctrl+F</b> - Find in page</li>"
        "<li><b>n</b> - Find next</li>"
        "<li><b>S</b> - Toggle page source view</li>"
        "<li><b>q</b> - Quit browser</li>"
        "</ul>"
        "<h2>Mouse Support</h2>"
        "<ul>"
        "<li>Click navigation buttons (Back, Forward, Refresh, Home)</li>"
        "<li>Click URL bar to focus it</li>"
        "<li>Click on numbered links to follow them</li>"
        "<li>Click scrollbar to jump to position</li>"
        "</ul>"
        "<h2>Features</h2>"
        "<ul>"
        "<li>HTTP and HTTPS support (kernel TLS 1.3)</li>"
        "<li>HTML rendering (headings, paragraphs, links, lists)</li>"
        "<li>Navigation history with back/forward</li>"
        "<li>Numbered link following (keyboard and mouse)</li>"
        "<li>Find in page with highlighting</li>"
        "<li>Page source view</li>"
        "<li>Word wrapping</li>"
        "</ul>"
        "<hr>"
        "<p>TobyOS Browser v3.0 - Built with the tobyOS kernel HTTP/HTTPS stack.</p>"
        "</body></html>";

    g_raw_len = 0;
    while (html[g_raw_len]) { g_raw[g_raw_len] = html[g_raw_len]; g_raw_len++; }
    g_raw[g_raw_len] = '\0';

    render_html();
    g_top_line = 0;
    g_source_view = 0;
    set_status("Ready");
}

static void do_fetch_url(const char *url) {
    g_loading = 1;
    set_status("Loading...");

    long n = sys_http_get(url, g_raw, RAW_CAP);
    g_loading = 0;

    if (n < 0) {
        const char *errs[] = {
            "OK", "Bad URL", "DNS failed", "Connect failed",
            "Protocol error", "Chunked (unsupported)", "Response too large",
            "Timeout", "Out of memory", "Connection reset"
        };
        int idx = (int)(-n);
        const char *emsg = (idx >= 1 && idx <= 9) ? errs[idx] : "Unknown error";
        set_status(emsg);

        g_raw_len = 0;
        const char *pre = "<html><body><h1>Page Load Error</h1><p>Could not load: ";
        while (*pre && g_raw_len < RAW_CAP) g_raw[g_raw_len++] = *pre++;
        for (int i = 0; url[i] && g_raw_len < RAW_CAP; i++) g_raw[g_raw_len++] = url[i];
        const char *mid = "</p><p>Error: <b>";
        while (*mid && g_raw_len < RAW_CAP) g_raw[g_raw_len++] = *mid++;
        while (*emsg && g_raw_len < RAW_CAP) g_raw[g_raw_len++] = *emsg++;
        const char *suf = "</b></p><p>Check the URL and try again.</p></body></html>";
        while (*suf && g_raw_len < RAW_CAP) g_raw[g_raw_len++] = *suf++;
        g_raw[g_raw_len] = '\0';
        render_html();
        g_top_line = 0;
        return;
    }

    g_raw_len = n;
    g_raw[g_raw_len] = '\0';

    g_source_view = 0;
    if (is_html_content())
        render_html();
    else
        render_plain_text();

    g_top_line = 0;

    char done_msg[64];
    str_copy(done_msg, "Done - ", 8);
    int pos = 7;
    int lc = g_line_count;
    if (lc >= 10000) done_msg[pos++] = '0' + (lc / 10000) % 10;
    if (lc >= 1000)  done_msg[pos++] = '0' + (lc / 1000) % 10;
    if (lc >= 100)   done_msg[pos++] = '0' + (lc / 100) % 10;
    if (lc >= 10)    done_msg[pos++] = '0' + (lc / 10) % 10;
    done_msg[pos++] = '0' + lc % 10;
    done_msg[pos++] = ' ';
    done_msg[pos++] = 'l';
    done_msg[pos++] = 'i';
    done_msg[pos++] = 'n';
    done_msg[pos++] = 'e';
    done_msg[pos++] = 's';
    if (g_link_count > 0) {
        done_msg[pos++] = ',';
        done_msg[pos++] = ' ';
        int nlk = g_link_count;
        if (nlk >= 100) done_msg[pos++] = '0' + (nlk / 100) % 10;
        if (nlk >= 10)  done_msg[pos++] = '0' + (nlk / 10) % 10;
        done_msg[pos++] = '0' + nlk % 10;
        done_msg[pos++] = ' ';
        done_msg[pos++] = 'l';
        done_msg[pos++] = 'i';
        done_msg[pos++] = 'n';
        done_msg[pos++] = 'k';
        done_msg[pos++] = 's';
    }
    done_msg[pos] = '\0';
    set_status(done_msg);
}

static void do_navigate(const char *url) {
    g_url_len = 0;
    while (url[g_url_len] && g_url_len < URL_MAX) {
        g_url[g_url_len] = url[g_url_len];
        g_url_len++;
    }
    g_url[g_url_len] = '\0';

    history_push(url);
    do_fetch_url(url);
    g_focus_url = 0;
}

/* ---- Scrolling ------------------------------------------------- */

static void scroll_up(int n) {
    g_top_line -= n;
    if (g_top_line < 0) g_top_line = 0;
}
static void scroll_down(int n) {
    g_top_line += n;
    int max_top = g_line_count - ROWS;
    if (max_top < 0) max_top = 0;
    if (g_top_line > max_top) g_top_line = max_top;
}

/* ---- Find in page ---------------------------------------------- */

static void find_next_match(void) {
    if (g_find_len == 0) return;
    g_find_buf[g_find_len] = '\0';

    int start_line = (g_find_line >= 0) ? g_find_line + 1 : 0;
    for (int pass = 0; pass < 2; pass++) {
        int from = (pass == 0) ? start_line : 0;
        int to   = (pass == 0) ? g_line_count : start_line;
        for (int li = from; li < to; li++) {
            long lstart = g_line_off[li];
            long lend;
            if (li + 1 < g_line_count) lend = g_line_off[li + 1];
            else lend = g_render_len;
            int llen = (int)(lend - lstart);
            if (str_contains(&g_render[lstart], llen, g_find_buf, g_find_len) >= 0) {
                g_find_line = li;
                g_top_line = li - ROWS / 2;
                if (g_top_line < 0) g_top_line = 0;
                int max_top = g_line_count - ROWS;
                if (max_top < 0) max_top = 0;
                if (g_top_line > max_top) g_top_line = max_top;
                set_status("Found");
                return;
            }
        }
    }
    set_status("Not found");
}

/* ---- Link detection for mouse click ----------------------------- */

static int get_link_number_at_line(int li) {
    if (li < 0 || li >= g_line_count) return 0;
    long start = g_line_off[li];
    if (start >= g_render_len) return 0;
    if (g_render[start] != '[') return 0;
    long p = start + 1;
    int num = 0;
    while (p < g_render_len && g_render[p] >= '0' && g_render[p] <= '9') {
        num = num * 10 + (g_render[p] - '0');
        p++;
    }
    if (p < g_render_len && g_render[p] == ']' && num > 0)
        return num;
    return 0;
}

/* ---- Drawing --------------------------------------------------- */

static void draw_hline(int fd, int x, int y, int w, uint32_t color) {
    sys_gui_fill(fd, x, y, w, 1, color);
}

/* paint_all() draws the whole window (called from the canvas on_paint);
 * redraw() just requests a repaint and is what the event handlers call. */
static void redraw(int fd) { (void)fd; tk_redraw(&win); }
static void paint_all(void) {
    int fd = 0; (void)fd;
    /* Tab Bar */
    sys_gui_fill(fd, 0, 0, WIN_W, TAB_BAR_H, COL_TAB_BG);
    sys_gui_fill(fd, 0, 0, 220, TAB_BAR_H, COL_TAB_ACTIVE);
    draw_hline(fd, 0, TAB_BAR_H - 1, 220, COL_NAV_BG);

    sys_gui_text(fd, 8, 5, "@", COL_URL_CURSOR, COL_TAB_ACTIVE);

    char tab_title[28];
    int tlen = 0;
    const char *title_src = g_title[0] ? g_title : "New Tab";
    while (title_src[tlen] && tlen < 24) { tab_title[tlen] = title_src[tlen]; tlen++; }
    if (tlen >= 24) { tab_title[21] = '.'; tab_title[22] = '.'; tab_title[23] = '.'; }
    tab_title[tlen] = '\0';
    sys_gui_text(fd, 22, 5, tab_title, COL_TAB_TEXT, COL_TAB_ACTIVE);

    sys_gui_text(fd, 200, 5, "x", COL_TAB_CLOSE, COL_TAB_ACTIVE);
    sys_gui_text(fd, 230, 5, "+", COL_NAV_BTN, COL_TAB_BG);

    /* Navigation Bar */
    int nav_y = TAB_BAR_H;
    sys_gui_fill(fd, 0, nav_y, WIN_W, NAV_BAR_H, COL_NAV_BG);

    uint32_t back_col = can_go_back() ? COL_NAV_BTN : COL_NAV_BTN_DIM;
    sys_gui_text(fd, 8, nav_y + 8, "<", back_col, COL_NAV_BG);

    uint32_t fwd_col = can_go_forward() ? COL_NAV_BTN : COL_NAV_BTN_DIM;
    sys_gui_text(fd, 24, nav_y + 8, ">", fwd_col, COL_NAV_BG);

    sys_gui_text(fd, 40, nav_y + 8, "O", COL_NAV_BTN, COL_NAV_BG);
    sys_gui_text(fd, 56, nav_y + 8, "H", COL_NAV_BTN, COL_NAV_BG);

    /* URL bar */
    int url_x = 72;
    int url_w = WIN_W - url_x - 8;
    int url_y = nav_y + 4;
    int url_h = NAV_BAR_H - 8;

    uint32_t url_bg = g_focus_url ? COL_URL_FOCUS_BG : COL_URL_BG;
    uint32_t url_bd = g_focus_url ? COL_URL_FOCUS_BD : COL_URL_BORDER;

    sys_gui_fill(fd, url_x, url_y, url_w, url_h, url_bg);
    draw_hline(fd, url_x, url_y, url_w, url_bd);
    draw_hline(fd, url_x, url_y + url_h - 1, url_w, url_bd);
    sys_gui_fill(fd, url_x, url_y, 1, url_h, url_bd);
    sys_gui_fill(fd, url_x + url_w - 1, url_y, 1, url_h, url_bd);

    /* Lock icon for https, info icon otherwise */
    int is_https = (g_url_len >= 8 && g_url[0]=='h' && g_url[1]=='t' &&
                    g_url[2]=='t' && g_url[3]=='p' && g_url[4]=='s');
    uint32_t icon_col = is_https ? COL_HTTPS_FG : COL_URL_HINT;
    sys_gui_text(fd, url_x + 6, url_y + 4, is_https ? "L" : "i", icon_col, url_bg);

    int url_text_x = url_x + 18;
    int url_avail = (url_w - 24) / CELL_W;
    char url_disp[128];

    if (g_url_len > 0) {
        int start = 0;
        if (g_url_len > url_avail) start = g_url_len - url_avail;
        int dlen = 0;
        for (int i = start; i < g_url_len && dlen < url_avail; i++)
            url_disp[dlen++] = g_url[i];
        if (g_focus_url && dlen < url_avail) url_disp[dlen++] = '|';
        url_disp[dlen] = '\0';
        sys_gui_text(fd, url_text_x, url_y + 4, url_disp, COL_URL_TEXT, url_bg);
    } else if (g_focus_url) {
        sys_gui_text(fd, url_text_x, url_y + 4, "|", COL_URL_CURSOR, url_bg);
    } else {
        sys_gui_text(fd, url_text_x, url_y + 4, "Search or type a URL",
                     COL_URL_HINT, url_bg);
    }

    if (g_loading) {
        sys_gui_fill(fd, url_x, url_y + url_h - 2, url_w / 3, 2, COL_URL_FOCUS_BD);
    }

    /* Content Area */
    sys_gui_fill(fd, 0, CONTENT_TOP, WIN_W, CONTENT_H, COL_PAGE_BG);

    int content_y = CONTENT_TOP;
    char line[COLS + 1];

    for (int r = 0; r < ROWS; r++) {
        int li = g_top_line + r;
        if (li >= g_line_count) break;

        long start = g_line_off[li];
        long end;
        if (li + 1 < g_line_count)
            end = g_line_off[li + 1];
        else
            end = g_render_len;
        if (end > start && g_render[end - 1] == '\n') end--;

        long len = end - start;
        if (len > COLS) len = COLS;

        for (long k = 0; k < len; k++) {
            char c = g_render[start + k];
            if ((unsigned char)c < 0x20 && c != '\t') c = ' ';
            if ((unsigned char)c > 0x7E) c = '.';
            if (c == '\t') c = ' ';
            line[k] = c;
        }
        line[len] = '\0';

        uint32_t fg = COL_TEXT_FG;
        uint32_t bg = COL_PAGE_BG;

        switch (g_line_style[li]) {
        case STYLE_H1:   fg = COL_H1_FG; break;
        case STYLE_H2:   fg = COL_H2_FG; break;
        case STYLE_H3:   fg = COL_H3_FG; break;
        case STYLE_LINK: fg = COL_LINK_FG; break;
        case STYLE_CODE: fg = COL_CODE_FG; bg = COL_CODE_BG; break;
        case STYLE_BULLET: fg = COL_TEXT_FG; break;
        case STYLE_HR:   fg = COL_HR_FG; break;
        case STYLE_BOLD: fg = COL_BOLD_FG; break;
        default: break;
        }

        /* Highlight find match line */
        if (g_find_mode && g_find_line == li) {
            bg = 0x003A3A00u;
        }

        if (len > 0)
            sys_gui_text(fd, PAD, content_y, line, fg, bg);
        content_y += CELL_H;
    }

    /* Scrollbar */
    if (g_line_count > ROWS) {
        int track_x = WIN_W - 6;
        int track_h = CONTENT_H;
        sys_gui_fill(fd, track_x, CONTENT_TOP, 4, track_h, 0x002D2E31u);

        int thumb_h = (ROWS * track_h) / g_line_count;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = CONTENT_TOP + (g_top_line * (track_h - thumb_h)) /
                      (g_line_count - ROWS > 0 ? g_line_count - ROWS : 1);
        sys_gui_fill(fd, track_x, thumb_y, 4, thumb_h, 0x005F6368u);
    }

    /* Status Bar (or Find Bar if in find mode) */
    int status_y = WIN_H - STATUS_H;
    sys_gui_fill(fd, 0, status_y, WIN_W, STATUS_H, COL_STATUS_BG);
    draw_hline(fd, 0, status_y, WIN_W, 0x003C4043u);

    if (g_find_mode) {
        sys_gui_fill(fd, 0, status_y, WIN_W, STATUS_H, COL_FIND_BG);
        char find_disp[FIND_MAX + 8];
        find_disp[0] = 'F'; find_disp[1] = 'i'; find_disp[2] = 'n';
        find_disp[3] = 'd'; find_disp[4] = ':'; find_disp[5] = ' ';
        int fp = 6;
        for (int i = 0; i < g_find_len && fp < FIND_MAX + 6; i++)
            find_disp[fp++] = g_find_buf[i];
        find_disp[fp++] = '|';
        find_disp[fp] = '\0';
        sys_gui_text(fd, PAD, status_y + 3, find_disp, COL_FIND_FG, COL_FIND_BG);
    } else {
        if (g_status_text[0])
            sys_gui_text(fd, PAD, status_y + 3, g_status_text, COL_STATUS_FG, COL_STATUS_BG);
    }

    /* Scroll position indicator */
    if (g_line_count > ROWS && !g_find_mode) {
        int pct = (g_top_line * 100) / (g_line_count - ROWS > 0 ? g_line_count - ROWS : 1);
        if (pct > 100) pct = 100;
        char pct_str[8];
        int pp = 0;
        if (pct >= 100) pct_str[pp++] = '1';
        if (pct >= 10)  pct_str[pp++] = '0' + (pct / 10) % 10;
        pct_str[pp++] = '0' + pct % 10;
        pct_str[pp++] = '%';
        pct_str[pp] = '\0';
        sys_gui_text(fd, WIN_W - (pp * CELL_W) - PAD, status_y + 3,
                     pct_str, COL_STATUS_FG, COL_STATUS_BG);
    }

    /* Source view indicator */
    if (g_source_view) {
        sys_gui_text(fd, WIN_W - 80, status_y + 3, "[SOURCE]", COL_FIND_HL, COL_STATUS_BG);
    }
}

/* ---- Link following -------------------------------------------- */

static int g_link_input_mode = 0;
static int g_link_num = 0;

static void follow_link(int num) {
    if (num < 1 || num > g_link_count) {
        set_status("Invalid link number");
        return;
    }

    char resolved[URL_MAX + 1];
    const char *href = g_links[num - 1];

    if (g_url_len > 0)
        resolve_relative_url(g_url, href, resolved, URL_MAX);
    else
        str_copy(resolved, href, URL_MAX);

    do_navigate(resolved);
}

/* ---- Mouse event handling -------------------------------------- */

static void handle_mouse_down(int fd, int mx, int my) {
    int nav_y = TAB_BAR_H;

    /* Tab close button area */
    if (my < TAB_BAR_H && mx >= 195 && mx <= 215) {
        sys_exit(0);
    }

    /* Navigation bar buttons */
    if (my >= nav_y && my < nav_y + NAV_BAR_H) {
        if (mx < 20) {
            /* Back */
            if (can_go_back()) {
                g_hist_pos--;
                g_url_len = 0;
                while (g_history[g_hist_pos][g_url_len]) {
                    g_url[g_url_len] = g_history[g_hist_pos][g_url_len];
                    g_url_len++;
                }
                g_url[g_url_len] = '\0';
                do_fetch_url(g_url);
                set_status("Back");
            }
        } else if (mx < 36) {
            /* Forward */
            if (can_go_forward()) {
                g_hist_pos++;
                g_url_len = 0;
                while (g_history[g_hist_pos][g_url_len]) {
                    g_url[g_url_len] = g_history[g_hist_pos][g_url_len];
                    g_url_len++;
                }
                g_url[g_url_len] = '\0';
                do_fetch_url(g_url);
                set_status("Forward");
            }
        } else if (mx < 52) {
            /* Refresh */
            if (g_url_len > 0) {
                g_url[g_url_len] = '\0';
                do_fetch_url(g_url);
            }
        } else if (mx < 68) {
            /* Home */
            set_home_page();
            g_url_len = 0;
            set_status("Home");
        } else if (mx >= 72) {
            /* URL bar clicked */
            g_focus_url = 1;
            set_status("Address bar focused");
        }
        redraw(fd);
        return;
    }

    /* Content area */
    if (my >= CONTENT_TOP && my < WIN_H - STATUS_H) {
        /* Scrollbar area (right 6px) */
        if (mx >= WIN_W - 6 && g_line_count > ROWS) {
            int track_h = CONTENT_H;
            int rel_y = my - CONTENT_TOP;
            int max_top = g_line_count - ROWS;
            if (max_top < 0) max_top = 0;
            g_top_line = (rel_y * max_top) / track_h;
            if (g_top_line < 0) g_top_line = 0;
            if (g_top_line > max_top) g_top_line = max_top;
            redraw(fd);
            return;
        }

        /* Content click: determine which line */
        int row = (my - CONTENT_TOP) / CELL_H;
        int li = g_top_line + row;
        if (li >= 0 && li < g_line_count) {
            int link_num = get_link_number_at_line(li);
            if (link_num > 0 && link_num <= g_link_count) {
                follow_link(link_num);
                redraw(fd);
                return;
            }
        }
        /* Click in content area unfocuses URL bar */
        g_focus_url = 0;
        redraw(fd);
        return;
    }

    redraw(fd);
}

/* Show link URL in status bar on mouse move over content */
static void handle_mouse_move(int fd, int mx, int my) {
    if (my >= CONTENT_TOP && my < WIN_H - STATUS_H && mx < WIN_W - 6) {
        int row = (my - CONTENT_TOP) / CELL_H;
        int li = g_top_line + row;
        int link_num = get_link_number_at_line(li);
        if (link_num > 0 && link_num <= g_link_count) {
            str_copy(g_status_text, g_links[link_num - 1], sizeof(g_status_text));
            redraw(fd);
            return;
        }
    }
    (void)mx;
}

/* ---- Main ------------------------------------------------------ */

/* ---- TobyTK callbacks ------------------------------------------ */
static void on_paint(struct tk_window *w, struct tk_widget *c) { (void)w; (void)c; paint_all(); }
static void on_event(struct tk_window *w, struct tk_widget *c, struct tk_event *ev) {
    (void)w; (void)c;
    if (ev->type == TK_EV_MOUSE_DOWN) handle_mouse_down(0, ev->x, ev->y);
    else if (ev->type == TK_EV_MOUSE_MOVE) handle_mouse_move(0, ev->x, ev->y);
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    (void)w;
    uint8_t key = ev->key;

        /* Find mode input */
        if (g_find_mode) {
            if (key == 27) {
                g_find_mode = 0;
                g_find_line = -1;
                set_status("Find cancelled");
                redraw(0);
                return;
            } else if (key == '\n' || key == '\r') {
                find_next_match();
                redraw(0);
                return;
            } else if (key == '\b' || key == 127) {
                if (g_find_len > 0) g_find_len--;
                redraw(0);
                return;
            } else if (key >= 0x20 && key <= 0x7E && g_find_len < FIND_MAX) {
                g_find_buf[g_find_len++] = (char)key;
                redraw(0);
                return;
            }
            return;
        }

        /* Ctrl+F (key code 0x06) enters find mode */
        if (key == 0x06) {
            g_find_mode = 1;
            g_find_len = 0;
            g_find_line = -1;
            g_focus_url = 0;
            redraw(0);
            return;
        }

        /* Tab switches focus */
        if (key == '\t') {
            g_focus_url = !g_focus_url;
            if (g_focus_url)
                set_status("Address bar focused - type URL and press Enter");
            else
                set_status("Content focused - j/k scroll, [ back, ] fwd");
            g_link_input_mode = 0;
            redraw(0);
            return;
        }

        if (g_focus_url) {
            if (key == '\n' || key == '\r') {
                if (g_url_len > 0) {
                    g_url[g_url_len] = '\0';
                    /* Auto-prepend http:// if no scheme present.
                     * Accept both http:// and https:// typed by user. */
                    if (g_url_len < 7 ||
                        !(g_url[0]=='h' && g_url[1]=='t' && g_url[2]=='t' && g_url[3]=='p')) {
                        char tmp[URL_MAX + 1];
                        str_copy(tmp, g_url, URL_MAX);
                        g_url[0] = 'h'; g_url[1] = 't'; g_url[2] = 't'; g_url[3] = 'p';
                        g_url[4] = ':'; g_url[5] = '/'; g_url[6] = '/';
                        int tl = (int)str_len(tmp);
                        for (int i = 0; i < tl && i + 7 < URL_MAX; i++)
                            g_url[7 + i] = tmp[i];
                        g_url_len = 7 + tl;
                        g_url[g_url_len] = '\0';
                    }
                    do_navigate(g_url);
                }
                redraw(0);
            } else if (key == '\b' || key == 127) {
                if (g_url_len > 0) g_url_len--;
                redraw(0);
            } else if (key == 27) {
                g_url_len = 0;
                g_focus_url = 0;
                set_status("Cancelled");
                redraw(0);
            } else if (key >= 0x20 && key <= 0x7E && g_url_len < URL_MAX) {
                g_url[g_url_len++] = (char)key;
                redraw(0);
            }
        } else {
            /* Content area input */
            if (g_link_input_mode) {
                if (key >= '0' && key <= '9') {
                    g_link_num = g_link_num * 10 + (key - '0');
                    char msg[32] = "Follow link: ";
                    int p = 13;
                    if (g_link_num >= 100) msg[p++] = '0' + (g_link_num/100)%10;
                    if (g_link_num >= 10) msg[p++] = '0' + (g_link_num/10)%10;
                    msg[p++] = '0' + g_link_num % 10;
                    msg[p] = '\0';
                    set_status(msg);
                    redraw(0);
                    return;
                } else if (key == '\n' || key == '\r') {
                    g_link_input_mode = 0;
                    follow_link(g_link_num);
                    redraw(0);
                    return;
                } else {
                    g_link_input_mode = 0;
                    set_status("Link follow cancelled");
                    redraw(0);
                    return;
                }
            }

            switch (key) {
            case 'j':  scroll_down(1);    break;
            case 'k':  scroll_up(1);      break;
            case 'd':  scroll_down(ROWS); break;
            case 'u':  scroll_up(ROWS);   break;
            case 'g':  g_top_line = 0;    break;
            case 'G': {
                int max_top = g_line_count - ROWS;
                if (max_top < 0) max_top = 0;
                g_top_line = max_top;
                break;
            }
            case 'n':
                if (g_find_len > 0) find_next_match();
                break;
            case 'S':
                if (g_raw_len > 0) {
                    g_source_view = !g_source_view;
                    if (g_source_view) {
                        render_source_view();
                    } else {
                        if (is_html_content())
                            render_html();
                        else
                            render_plain_text();
                    }
                    g_top_line = 0;
                    set_status(g_source_view ? "Source view" : "Rendered view");
                }
                break;
            case '[':
                if (can_go_back()) {
                    g_hist_pos--;
                    g_url_len = 0;
                    while (g_history[g_hist_pos][g_url_len]) {
                        g_url[g_url_len] = g_history[g_hist_pos][g_url_len];
                        g_url_len++;
                    }
                    g_url[g_url_len] = '\0';
                    do_fetch_url(g_url);
                    set_status("Back");
                } else {
                    set_status("No previous page");
                }
                break;
            case ']':
                if (can_go_forward()) {
                    g_hist_pos++;
                    g_url_len = 0;
                    while (g_history[g_hist_pos][g_url_len]) {
                        g_url[g_url_len] = g_history[g_hist_pos][g_url_len];
                        g_url_len++;
                    }
                    g_url[g_url_len] = '\0';
                    do_fetch_url(g_url);
                    set_status("Forward");
                } else {
                    set_status("No next page");
                }
                break;
            case 'r':
                if (g_url_len > 0) {
                    g_url[g_url_len] = '\0';
                    do_fetch_url(g_url);
                } else {
                    set_status("No page to refresh");
                }
                break;
            case 'h':
                set_home_page();
                g_url_len = 0;
                set_status("Home");
                break;
            case 'f':
                if (g_link_count > 0) {
                    g_link_input_mode = 1;
                    g_link_num = 0;
                    set_status("Type link number, then Enter");
                } else {
                    set_status("No links on this page");
                }
                break;
            case '/':
                g_focus_url = 1;
                set_status("Address bar focused");
                break;
            case 'q':
                sys_exit(0);
            default:
                if (key >= '1' && key <= '9') {
                    g_link_input_mode = 1;
                    g_link_num = key - '0';
                    char msg[32] = "Follow link: ";
                    int p = 13;
                    msg[p++] = (char)key;
                    msg[p] = '\0';
                    set_status(msg);
                }
                break;
            }
            redraw(0);
        }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    mem_zero(g_url, sizeof(g_url));
    mem_zero(g_title, sizeof(g_title));
    mem_zero(g_status_text, sizeof(g_status_text));
    mem_zero(g_find_buf, sizeof(g_find_buf));

    set_home_page();
    set_status("Ready - Press Tab to focus address bar");

    if (tk_window_open(&win, WIN_W, WIN_H, "TobyOS Browser") != 0) {
        sys_write(1, "gui_browser: window failed\n", 27);
        return 1;
    }
    tk_on_key(&win, on_key);
    struct tk_widget *root = tk_root(&win); tk_pad(root, 0);
    struct tk_widget *cv = tk_canvas(&win, root, on_paint);
    tk_grow(cv, 1); tk_on_event(cv, on_event);
    return tk_run(&win);
}
