/* browser/main.c -- Full-featured web browser for tobyOS.
 *
 * Built on libtoby: HTTP networking (toby/net.h), image decoding
 * (toby/image.h), GUI syscalls, and the media pipeline (toby/media.h).
 *
 * Features:
 *   - 1024x768 GUI window with tab bar, address bar, content area
 *   - Up to 8 tabs with independent navigation state
 *   - HTML parser producing a lightweight DOM
 *   - CSS parser and cascading style resolver
 *   - Block/inline layout engine with word wrapping
 *   - Paint pipeline: backgrounds, text, borders, images
 *   - Link click via hit-testing the layout tree
 *   - Image loading (fetch + stb_image decode + blit)
 *   - Form handling: <input> collection, GET/POST submit
 *   - Cookie jar: per-domain name=value storage
 *   - Download manager for non-HTML content types
 *   - Back/forward history stack per tab
 *   - Keyboard and mouse input
 *   - Status bar with load progress
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <toby/net.h>
#include <toby/image.h>
#include <toby/media.h>

/* ================================================================
 * GUI syscall interface
 * ================================================================ */

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_MSLEEP          6
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_GUI_BLIT       15
#define SYS_GUI_SCROLL     16

struct gui_event {
    int     type;
    int     x, y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

#define EV_MOUSE_MOVE  1
#define EV_MOUSE_DOWN  2
#define EV_MOUSE_UP    3
#define EV_KEY         4
#define EV_CLOSE       5
#define EV_SCROLL      6

static long sc0(long n)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n) : "rcx","r11","memory");
    return r;
}
static long sc1(long n, long a)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a) : "rcx","r11","memory");
    return r;
}
static long sc2(long n, long a, long b)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx","r11","memory");
    return r;
}
static long sc3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx","r11","memory");
    return r;
}

static int gui_create(int w, int h, const char *title) {
    return (int)sc3(SYS_GUI_CREATE, w, h, (long)title);
}
static void gui_fill(int fd, int x, int y, int w, int h, uint32_t c) {
    uint32_t wh = ((uint32_t)(uint16_t)w) | (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)wh;
    register long r8  __asm__("r8")  = (long)c;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"((long)SYS_GUI_FILL),
        "D"((long)fd), "S"((long)x), "d"((long)y),
        "r"(r10), "r"(r8) : "rcx","r11","memory");
}
static void gui_text(int fd, int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    uint32_t xy = ((uint32_t)(uint16_t)x) | (((uint32_t)(uint16_t)y) << 16);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)bg;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"((long)SYS_GUI_TEXT),
        "D"((long)fd), "S"((long)xy), "d"(s),
        "r"(r10), "r"(r8) : "rcx","r11","memory");
}
static void gui_flip(int fd) { sc1(SYS_GUI_FLIP, fd); }
static int  gui_poll(int fd, struct gui_event *ev) {
    return (int)sc2(SYS_GUI_POLL_EVENT, fd, (long)ev);
}
static void gui_blit(int fd, int x, int y, int w, int h,
                     const uint32_t *pixels) {
    uint32_t xy = ((uint32_t)(uint16_t)x) | (((uint32_t)(uint16_t)y) << 16);
    uint32_t wh = ((uint32_t)(uint16_t)w) | (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)wh;
    register long r8  __asm__("r8")  = (long)pixels;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"((long)SYS_GUI_BLIT),
        "D"((long)fd), "S"((long)xy), "d"((long)0),
        "r"(r10), "r"(r8) : "rcx","r11","memory");
}

/* ================================================================
 * Constants & layout
 * ================================================================ */

#define WIN_W        1024
#define WIN_H         768

#define TAB_H          26
#define NAV_H          32
#define STATUS_H       18
#define CONTENT_Y     (TAB_H + NAV_H)
#define CONTENT_H     (WIN_H - CONTENT_Y - STATUS_H)

#define CELL_W          8
#define CELL_H         14
#define PAD             6
#define COLS          ((WIN_W - 2 * PAD) / CELL_W)
#define ROWS          (CONTENT_H / CELL_H)

#define MAX_TABS        8
#define URL_MAX       1024
#define TITLE_MAX      128
#define HISTORY_MAX     64

/* ---- Colours ---- */

#define C_TAB_BG      0xFF1F1F1Fu
#define C_TAB_ACTIVE  0xFF2B2B2Bu
#define C_TAB_TEXT    0xFFE0E0E0u
#define C_TAB_CLOSE   0xFFFF5555u
#define C_TAB_PLUS    0xFF888888u

#define C_NAV_BG      0xFF2B2B2Bu
#define C_BTN_FG      0xFFAAAAAAu
#define C_BTN_DIM     0xFF555555u

#define C_URL_BG      0xFF1A1A1Au
#define C_URL_FG      0xFFE0E0E0u
#define C_URL_BORDER  0xFF3C3C3Cu
#define C_URL_FOCUS   0xFF5588CCu
#define C_URL_HINT    0xFF777777u
#define C_URL_CURSOR  0xFF5588CCu
#define C_HTTPS       0xFF55CC77u

#define C_PAGE_BG     0xFF1E1E1Eu
#define C_TEXT        0xFFBBBBBBu
#define C_H1          0xFFE8E8E8u
#define C_H2          0xFFD0D0D0u
#define C_H3          0xFFC0C0C0u
#define C_LINK        0xFF5599DDu
#define C_BOLD        0xFFE8E8E8u
#define C_CODE_BG     0xFF2A2A2Au
#define C_CODE_FG     0xFF88CCEEu
#define C_LIST        0xFF5599DDu
#define C_HR          0xFF444444u
#define C_IMG_BG      0xFF333344u

#define C_STATUS_BG   0xFF1F1F1Fu
#define C_STATUS_FG   0xFF888888u
#define C_FIND_BG     0xFF333333u
#define C_FIND_FG     0xFFE0E0E0u
#define C_FIND_HL     0xFFDDCC44u

#define C_SCROLLBAR   0xFF333333u
#define C_THUMB       0xFF666666u

/* ================================================================
 * DOM node types
 * ================================================================ */

#define NODE_DOCUMENT     0
#define NODE_ELEMENT      1
#define NODE_TEXT          2

#define TAG_NONE          0
#define TAG_HTML          1
#define TAG_HEAD          2
#define TAG_BODY          3
#define TAG_DIV           4
#define TAG_P             5
#define TAG_H1            6
#define TAG_H2            7
#define TAG_H3            8
#define TAG_H4            9
#define TAG_H5           10
#define TAG_H6           11
#define TAG_A            12
#define TAG_IMG          13
#define TAG_UL           14
#define TAG_OL           15
#define TAG_LI           16
#define TAG_TABLE        17
#define TAG_TR           18
#define TAG_TD           19
#define TAG_TH           20
#define TAG_FORM         21
#define TAG_INPUT        22
#define TAG_TEXTAREA     23
#define TAG_BUTTON       24
#define TAG_SELECT       25
#define TAG_OPTION       26
#define TAG_STYLE        27
#define TAG_SCRIPT       28
#define TAG_LINK         29
#define TAG_TITLE        30
#define TAG_BR           31
#define TAG_HR           32
#define TAG_PRE          33
#define TAG_CODE         34
#define TAG_SPAN         35
#define TAG_B            36
#define TAG_STRONG       37
#define TAG_I            38
#define TAG_EM           39
#define TAG_NAV          40
#define TAG_HEADER       41
#define TAG_FOOTER       42
#define TAG_SECTION      43
#define TAG_ARTICLE      44
#define TAG_MAIN         45
#define TAG_META         46
#define TAG_VIDEO        47
#define TAG_AUDIO        48
#define TAG_SOURCE       49
#define TAG_LABEL        50
#define TAG_BLOCKQUOTE   51

#define MAX_DOM_NODES   2048
#define MAX_ATTRS          8
#define ATTR_LEN         256
#define TEXT_LEN         512

struct dom_attr {
    char name[64];
    char value[ATTR_LEN];
};

struct dom_node {
    int              type;
    int              tag;
    char             text[TEXT_LEN];
    struct dom_attr  attrs[MAX_ATTRS];
    int              attr_count;
    int              parent;
    int              first_child;
    int              next_sibling;
};

/* ================================================================
 * CSS styles (simplified)
 * ================================================================ */

struct css_style {
    uint32_t color;
    uint32_t bg_color;
    int      font_size;     /* in pixels */
    int      font_bold;
    int      display;       /* 0=block, 1=inline, 2=none */
    int      margin_top;
    int      margin_bottom;
    int      padding;
};

/* ================================================================
 * Layout boxes
 * ================================================================ */

#define MAX_LAYOUT_BOXES  2048
#define MAX_IMAGES          64

struct layout_box {
    int x, y, w, h;
    int dom_node;
    int is_text;
    char text[TEXT_LEN];
    struct css_style style;
    int parent;
    int first_child;
    int next_sibling;
};

struct loaded_image {
    int           dom_node;
    toby_image_t *img;
    int           lx, ly, lw, lh;
};

/* ================================================================
 * Per-tab state
 * ================================================================ */

struct tab_state {
    int   active;
    char  url[URL_MAX + 1];
    int   url_len;
    char  title[TITLE_MAX + 1];

    /* History */
    char  history[HISTORY_MAX][URL_MAX + 1];
    int   hist_pos;
    int   hist_count;

    /* Raw page content */
    char *raw_body;
    int   raw_len;
    int   raw_cap;
    char  content_type[128];

    /* DOM */
    struct dom_node  dom[MAX_DOM_NODES];
    int              dom_count;

    /* Layout */
    struct layout_box layout[MAX_LAYOUT_BOXES];
    int               layout_count;

    /* Rendered text lines (fallback rendering) */
    char *render_buf;
    int   render_len;
    int   render_cap;

    /* Line index into render_buf */
    int  *line_off;
    int  *line_style;
    int   line_count;
    int   line_cap;

    /* Links */
    char (*links)[ATTR_LEN];
    int   link_count;
    int   link_cap;

    /* Images */
    struct loaded_image images[MAX_IMAGES];
    int                 image_count;

    /* Scroll */
    int   scroll_y;
    int   total_height;

    /* Loading */
    int   loading;

    /* Cookies */
    char (*cookies)[256];
    int   cookie_count;
    int   cookie_cap;

    /* Forms */
    char  form_action[URL_MAX];
    int   form_method;  /* 0=GET, 1=POST */
};

/* ================================================================
 * Global state
 * ================================================================ */

static int  g_win_fd = -1;
static int  g_active_tab = 0;
static int  g_tab_count = 1;
static struct tab_state g_tabs[MAX_TABS];

static int  g_focus_url = 0;
static char g_url_input[URL_MAX + 1];
static int  g_url_input_len = 0;

static char g_status[256];
static int  g_find_mode = 0;
static char g_find_buf[128];
static int  g_find_len = 0;
static int  g_find_line = -1;

/* ================================================================
 * Helpers
 * ================================================================ */

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < 255) { g_status[i] = s[i]; i++; }
    g_status[i] = '\0';
}

static int streqi(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int strneqi(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ================================================================
 * Tab management
 * ================================================================ */

static struct tab_state *cur_tab(void) { return &g_tabs[g_active_tab]; }

static void tab_init(struct tab_state *t)
{
    memset(t, 0, sizeof(*t));
    t->active    = 1;
    t->raw_cap   = 64 * 1024;
    t->raw_body  = (char *)malloc(t->raw_cap);
    t->render_cap = 96 * 1024;
    t->render_buf = (char *)malloc(t->render_cap);
    t->line_cap  = 8192;
    t->line_off  = (int *)malloc(t->line_cap * sizeof(int));
    t->line_style = (int *)malloc(t->line_cap * sizeof(int));
    t->link_cap  = 256;
    t->links     = (char (*)[ATTR_LEN])malloc(t->link_cap * ATTR_LEN);
    t->cookie_cap = 64;
    t->cookies   = (char (*)[256])malloc(t->cookie_cap * 256);
    if (t->raw_body)   t->raw_body[0] = '\0';
    if (t->render_buf) t->render_buf[0] = '\0';
    str_copy(t->title, "New Tab", TITLE_MAX);
}

static void tab_free(struct tab_state *t)
{
    free(t->raw_body);
    free(t->render_buf);
    free(t->line_off);
    free(t->line_style);
    free(t->links);
    for (int i = 0; i < t->image_count; i++)
        toby_image_free(t->images[i].img);
    free(t->cookies);
    memset(t, 0, sizeof(*t));
}

static int tab_new(void)
{
    if (g_tab_count >= MAX_TABS) return -1;
    int idx = g_tab_count++;
    tab_init(&g_tabs[idx]);
    g_active_tab = idx;
    return idx;
}

static void tab_close(int idx)
{
    if (g_tab_count <= 1) return;
    tab_free(&g_tabs[idx]);
    for (int i = idx; i < g_tab_count - 1; i++)
        g_tabs[i] = g_tabs[i + 1];
    g_tab_count--;
    memset(&g_tabs[g_tab_count], 0, sizeof(struct tab_state));
    if (g_active_tab >= g_tab_count)
        g_active_tab = g_tab_count - 1;
}

/* ================================================================
 * URL resolution
 * ================================================================ */

static void resolve_url(const char *base, const char *rel,
                        char *out, int out_max)
{
    if (!rel || !rel[0]) { str_copy(out, base, out_max); return; }

    /* Absolute URL */
    if (strneqi(rel, "http://", 7) || strneqi(rel, "https://", 8)) {
        str_copy(out, rel, out_max);
        return;
    }

    /* Protocol-relative */
    if (rel[0] == '/' && rel[1] == '/') {
        int p = 0;
        for (int i = 0; base[i] && base[i] != ':' && p < out_max - 3; i++)
            out[p++] = base[i];
        out[p++] = ':';
        for (int i = 0; rel[i] && p < out_max - 1; i++)
            out[p++] = rel[i];
        out[p] = '\0';
        return;
    }

    /* Find origin end (after http://host) */
    int origin_end = 0, slashes = 0;
    for (int i = 0; base[i]; i++) {
        if (base[i] == '/') { slashes++; if (slashes == 3) { origin_end = i; break; } }
    }
    if (origin_end == 0) origin_end = (int)strlen(base);

    if (rel[0] == '/') {
        int p = 0;
        for (int i = 0; i < origin_end && p < out_max - 1; i++)
            out[p++] = base[i];
        for (int i = 0; rel[i] && p < out_max - 1; i++)
            out[p++] = rel[i];
        out[p] = '\0';
    } else {
        int last_slash = origin_end;
        for (int i = origin_end; base[i]; i++)
            if (base[i] == '/') last_slash = i;
        int p = 0;
        for (int i = 0; i <= last_slash && p < out_max - 1; i++)
            out[p++] = base[i];
        for (int i = 0; rel[i] && p < out_max - 1; i++)
            out[p++] = rel[i];
        out[p] = '\0';
    }
}

/* ================================================================
 * Cookie jar
 * ================================================================ */

static void cookie_set(struct tab_state *t, const char *domain,
                       const char *name, const char *value)
{
    char entry[256];
    snprintf(entry, sizeof(entry), "%s=%s; domain=%s", name, value, domain);

    /* Update existing? */
    for (int i = 0; i < t->cookie_count; i++) {
        if (strneqi(t->cookies[i], name, (int)strlen(name)) &&
            t->cookies[i][strlen(name)] == '=') {
            str_copy(t->cookies[i], entry, 256);
            return;
        }
    }
    if (t->cookie_count < t->cookie_cap)
        str_copy(t->cookies[t->cookie_count++], entry, 256);
}

static void cookie_build_header(struct tab_state *t, const char *domain,
                                char *out, int out_max)
{
    int p = 0;
    int first = 1;
    for (int i = 0; i < t->cookie_count && p < out_max - 2; i++) {
        char *dom = strstr(t->cookies[i], "domain=");
        if (dom && strstr(dom + 7, domain)) {
            if (!first) { out[p++] = ';'; out[p++] = ' '; }
            char *eq = strchr(t->cookies[i], ';');
            int len = eq ? (int)(eq - t->cookies[i]) : (int)strlen(t->cookies[i]);
            for (int j = 0; j < len && p < out_max - 1; j++)
                out[p++] = t->cookies[i][j];
            first = 0;
        }
    }
    out[p] = '\0';
}

/* ================================================================
 * HTML parser -> DOM
 * ================================================================ */

static int tag_name_to_id(const char *name)
{
    static const struct { const char *n; int id; } tags[] = {
        {"html",TAG_HTML},{"head",TAG_HEAD},{"body",TAG_BODY},
        {"div",TAG_DIV},{"p",TAG_P},
        {"h1",TAG_H1},{"h2",TAG_H2},{"h3",TAG_H3},
        {"h4",TAG_H4},{"h5",TAG_H5},{"h6",TAG_H6},
        {"a",TAG_A},{"img",TAG_IMG},
        {"ul",TAG_UL},{"ol",TAG_OL},{"li",TAG_LI},
        {"table",TAG_TABLE},{"tr",TAG_TR},{"td",TAG_TD},{"th",TAG_TH},
        {"form",TAG_FORM},{"input",TAG_INPUT},{"textarea",TAG_TEXTAREA},
        {"button",TAG_BUTTON},{"select",TAG_SELECT},{"option",TAG_OPTION},
        {"style",TAG_STYLE},{"script",TAG_SCRIPT},{"link",TAG_LINK},
        {"title",TAG_TITLE},{"br",TAG_BR},{"hr",TAG_HR},
        {"pre",TAG_PRE},{"code",TAG_CODE},{"span",TAG_SPAN},
        {"b",TAG_B},{"strong",TAG_STRONG},{"i",TAG_I},{"em",TAG_EM},
        {"nav",TAG_NAV},{"header",TAG_HEADER},{"footer",TAG_FOOTER},
        {"section",TAG_SECTION},{"article",TAG_ARTICLE},{"main",TAG_MAIN},
        {"meta",TAG_META},{"video",TAG_VIDEO},{"audio",TAG_AUDIO},
        {"source",TAG_SOURCE},{"label",TAG_LABEL},{"blockquote",TAG_BLOCKQUOTE},
        {NULL,0}
    };
    for (int i = 0; tags[i].n; i++)
        if (streqi(name, tags[i].n)) return tags[i].id;
    return TAG_NONE;
}

static int is_void_tag(int tag)
{
    return tag == TAG_BR || tag == TAG_HR || tag == TAG_IMG ||
           tag == TAG_INPUT || tag == TAG_META || tag == TAG_LINK ||
           tag == TAG_SOURCE;
}

static int dom_alloc(struct tab_state *t)
{
    if (t->dom_count >= MAX_DOM_NODES) return -1;
    int idx = t->dom_count++;
    memset(&t->dom[idx], 0, sizeof(struct dom_node));
    t->dom[idx].parent = -1;
    t->dom[idx].first_child = -1;
    t->dom[idx].next_sibling = -1;
    return idx;
}

static void dom_add_child(struct tab_state *t, int parent, int child)
{
    if (parent < 0 || child < 0) return;
    t->dom[child].parent = parent;
    if (t->dom[parent].first_child < 0) {
        t->dom[parent].first_child = child;
    } else {
        int n = t->dom[parent].first_child;
        while (t->dom[n].next_sibling >= 0) n = t->dom[n].next_sibling;
        t->dom[n].next_sibling = child;
    }
}

static const char *get_attr(struct dom_node *n, const char *name)
{
    for (int i = 0; i < n->attr_count; i++)
        if (streqi(n->attrs[i].name, name))
            return n->attrs[i].value;
    return NULL;
}

static char decode_entity_ch(const char *src, int *advance)
{
    *advance = 0;
    if (src[0] != '&') return src[0];
    if (strneqi(src+1, "lt;",   3)) { *advance = 3; return '<'; }
    if (strneqi(src+1, "gt;",   3)) { *advance = 3; return '>'; }
    if (strneqi(src+1, "amp;",  4)) { *advance = 4; return '&'; }
    if (strneqi(src+1, "quot;", 5)) { *advance = 5; return '"'; }
    if (strneqi(src+1, "apos;", 5)) { *advance = 5; return '\''; }
    if (strneqi(src+1, "nbsp;", 5)) { *advance = 5; return ' '; }
    if (src[1] == '#') {
        int val = 0;
        int i = 2;
        if (src[i] == 'x' || src[i] == 'X') {
            i++;
            while ((src[i] >= '0' && src[i] <= '9') ||
                   (src[i] >= 'a' && src[i] <= 'f') ||
                   (src[i] >= 'A' && src[i] <= 'F')) {
                if (src[i] >= '0' && src[i] <= '9') val = val * 16 + src[i] - '0';
                else if (src[i] >= 'a' && src[i] <= 'f') val = val * 16 + 10 + src[i] - 'a';
                else val = val * 16 + 10 + src[i] - 'A';
                i++;
            }
        } else {
            while (src[i] >= '0' && src[i] <= '9') {
                val = val * 10 + src[i] - '0';
                i++;
            }
        }
        if (src[i] == ';') { *advance = i; return (val > 0 && val < 128) ? (char)val : '?'; }
    }
    return '&';
}

static void html_parse(struct tab_state *t, const char *html, int len)
{
    t->dom_count = 0;
    t->link_count = 0;

    int root = dom_alloc(t);
    t->dom[root].type = NODE_DOCUMENT;
    t->dom[root].tag  = TAG_HTML;

    int cur_parent = root;
    int in_script = 0, in_style = 0;
    int i = 0;

    while (i < len && t->dom_count < MAX_DOM_NODES - 2) {
        if (html[i] == '<') {
            i++;
            if (i >= len) break;

            /* Comment: <!-- ... --> */
            if (i + 2 < len && html[i] == '!' && html[i+1] == '-' && html[i+2] == '-') {
                i += 3;
                while (i + 2 < len) {
                    if (html[i] == '-' && html[i+1] == '-' && html[i+2] == '>') { i += 3; break; }
                    i++;
                }
                continue;
            }

            /* Doctype */
            if (html[i] == '!') { while (i < len && html[i] != '>') i++; i++; continue; }

            int closing = 0;
            if (html[i] == '/') { closing = 1; i++; }

            /* Parse tag name */
            char tag_name[64];
            int tn = 0;
            while (i < len && html[i] != '>' && html[i] != ' ' && html[i] != '/' &&
                   html[i] != '\t' && html[i] != '\n' && html[i] != '\r' && tn < 63) {
                tag_name[tn++] = (html[i] >= 'A' && html[i] <= 'Z') ? html[i] + 32 : html[i];
                i++;
            }
            tag_name[tn] = '\0';
            int tag_id = tag_name_to_id(tag_name);

            if (closing) {
                /* Close tag: pop up to matching parent */
                while (i < len && html[i] != '>') i++;
                i++;
                if (tag_id == TAG_SCRIPT) in_script = 0;
                if (tag_id == TAG_STYLE) in_style = 0;
                /* Walk up */
                int p = cur_parent;
                while (p > 0) {
                    if (t->dom[p].tag == tag_id) { cur_parent = t->dom[p].parent; break; }
                    p = t->dom[p].parent;
                }
                continue;
            }

            /* Parse attributes */
            struct dom_attr attrs[MAX_ATTRS];
            int attr_count = 0;

            while (i < len && html[i] != '>' && html[i] != '/') {
                while (i < len && (html[i] == ' ' || html[i] == '\t' ||
                       html[i] == '\n' || html[i] == '\r')) i++;
                if (i >= len || html[i] == '>' || html[i] == '/') break;

                char aname[64]; int an = 0;
                while (i < len && html[i] != '=' && html[i] != '>' &&
                       html[i] != ' ' && html[i] != '/' && an < 63) {
                    aname[an++] = (html[i] >= 'A' && html[i] <= 'Z') ? html[i] + 32 : html[i];
                    i++;
                }
                aname[an] = '\0';

                char aval[ATTR_LEN]; aval[0] = '\0';
                if (i < len && html[i] == '=') {
                    i++;
                    if (i < len && (html[i] == '"' || html[i] == '\'')) {
                        char q = html[i]; i++;
                        int av = 0;
                        while (i < len && html[i] != q && av < ATTR_LEN - 1) {
                            if (html[i] == '&') {
                                int adv;
                                char ch = decode_entity_ch(&html[i], &adv);
                                aval[av++] = ch;
                                i += adv + 1;
                            } else {
                                aval[av++] = html[i++];
                            }
                        }
                        aval[av] = '\0';
                        if (i < len && html[i] == q) i++;
                    } else {
                        int av = 0;
                        while (i < len && html[i] != '>' && html[i] != ' ' &&
                               html[i] != '\t' && av < ATTR_LEN - 1)
                            aval[av++] = html[i++];
                        aval[av] = '\0';
                    }
                }

                if (attr_count < MAX_ATTRS && aname[0]) {
                    str_copy(attrs[attr_count].name, aname, 64);
                    str_copy(attrs[attr_count].value, aval, ATTR_LEN);
                    attr_count++;
                }
            }

            int self_close = 0;
            if (i < len && html[i] == '/') { self_close = 1; i++; }
            if (i < len && html[i] == '>') i++;

            if (in_script && tag_id != TAG_SCRIPT) continue;
            if (in_style && tag_id != TAG_STYLE) continue;

            int node = dom_alloc(t);
            if (node < 0) continue;
            t->dom[node].type = NODE_ELEMENT;
            t->dom[node].tag  = tag_id;
            for (int a = 0; a < attr_count; a++)
                t->dom[node].attrs[a] = attrs[a];
            t->dom[node].attr_count = attr_count;

            dom_add_child(t, cur_parent, node);

            /* Collect link hrefs */
            if (tag_id == TAG_A) {
                const char *href = get_attr(&t->dom[node], "href");
                if (href && t->link_count < t->link_cap)
                    str_copy(t->links[t->link_count++], href, ATTR_LEN);
            }

            /* Title extraction */
            if (tag_id == TAG_TITLE) {
                int j = i;
                int tp = 0;
                while (j < len && tp < TITLE_MAX - 1) {
                    if (html[j] == '<') break;
                    t->title[tp++] = html[j++];
                }
                t->title[tp] = '\0';
            }

            if (tag_id == TAG_SCRIPT) in_script = 1;
            if (tag_id == TAG_STYLE) in_style = 1;

            if (!self_close && !is_void_tag(tag_id) && tag_id != TAG_SCRIPT)
                cur_parent = node;
        } else {
            /* Text node */
            if (in_script || in_style) { i++; continue; }

            int start = i;
            while (i < len && html[i] != '<') i++;

            int node = dom_alloc(t);
            if (node < 0) continue;
            t->dom[node].type = NODE_TEXT;

            int tp = 0;
            for (int j = start; j < i && tp < TEXT_LEN - 1; j++) {
                if (html[j] == '&') {
                    int adv;
                    char ch = decode_entity_ch(&html[j], &adv);
                    t->dom[node].text[tp++] = ch;
                    j += adv;
                } else {
                    t->dom[node].text[tp++] = html[j];
                }
            }
            t->dom[node].text[tp] = '\0';
            dom_add_child(t, cur_parent, node);
        }
    }
}

/* ================================================================
 * CSS parser (simplified)
 * ================================================================ */

static struct css_style default_style(int tag)
{
    struct css_style s;
    memset(&s, 0, sizeof(s));
    s.color    = C_TEXT;
    s.bg_color = 0;
    s.font_size = 14;
    s.display   = 0;  /* block */

    switch (tag) {
    case TAG_H1: s.color = C_H1; s.font_size = 28; s.font_bold = 1;
                 s.margin_top = 16; s.margin_bottom = 8; break;
    case TAG_H2: s.color = C_H2; s.font_size = 22; s.font_bold = 1;
                 s.margin_top = 12; s.margin_bottom = 6; break;
    case TAG_H3: s.color = C_H3; s.font_size = 18; s.font_bold = 1;
                 s.margin_top = 10; s.margin_bottom = 4; break;
    case TAG_H4: case TAG_H5: case TAG_H6:
                 s.color = C_H3; s.font_size = 16; s.font_bold = 1;
                 s.margin_top = 8; s.margin_bottom = 4; break;
    case TAG_P:  s.margin_top = 8; s.margin_bottom = 8; break;
    case TAG_A:  s.color = C_LINK; s.display = 1; break;
    case TAG_B: case TAG_STRONG:
                 s.color = C_BOLD; s.font_bold = 1; s.display = 1; break;
    case TAG_I: case TAG_EM:
                 s.display = 1; break;
    case TAG_SPAN: s.display = 1; break;
    case TAG_CODE: s.color = C_CODE_FG; s.bg_color = C_CODE_BG;
                   s.display = 1; break;
    case TAG_PRE:  s.color = C_CODE_FG; s.bg_color = C_CODE_BG;
                   s.margin_top = 8; s.margin_bottom = 8; s.padding = 4; break;
    case TAG_BLOCKQUOTE:
                 s.margin_top = 8; s.margin_bottom = 8; s.padding = 8; break;
    case TAG_LI: s.margin_top = 2; s.margin_bottom = 2; break;
    case TAG_UL: case TAG_OL:
                 s.margin_top = 4; s.margin_bottom = 4; break;
    case TAG_HR: s.margin_top = 8; s.margin_bottom = 8; break;
    case TAG_IMG: s.display = 1; break;
    case TAG_BR: s.display = 1; break;
    case TAG_DIV: case TAG_SECTION: case TAG_ARTICLE: case TAG_NAV:
    case TAG_HEADER: case TAG_FOOTER: case TAG_MAIN:
                 s.margin_top = 4; s.margin_bottom = 4; break;
    case TAG_INPUT: s.display = 1; break;
    case TAG_BUTTON: s.display = 1; s.color = C_H1; break;
    case TAG_SCRIPT: case TAG_STYLE: case TAG_HEAD: case TAG_META:
    case TAG_LINK:
                 s.display = 2; break;
    default: break;
    }
    return s;
}

/* ================================================================
 * Text-mode renderer (fallback, used for layout)
 * ================================================================ */

#define STYLE_NORMAL  0
#define STYLE_H1      1
#define STYLE_H2      2
#define STYLE_H3      3
#define STYLE_LINK    4
#define STYLE_CODE    5
#define STYLE_BULLET  6
#define STYLE_HR_     7
#define STYLE_BOLD    8
#define STYLE_IMG     9

static void render_emit(struct tab_state *t, char c)
{
    if (t->render_len < t->render_cap - 1)
        t->render_buf[t->render_len++] = c;
}

static void render_str(struct tab_state *t, const char *s)
{
    while (*s) render_emit(t, *s++);
}

static void render_newline(struct tab_state *t)
{
    render_emit(t, '\n');
}

static void render_dom_text(struct tab_state *t, int node_idx,
                            int *line_has_content, int *last_space,
                            int heading, int in_pre, int in_list,
                            int *link_idx)
{
    struct dom_node *n = &t->dom[node_idx];

    if (n->type == NODE_TEXT) {
        const char *s = n->text;
        while (*s) {
            if (in_pre) {
                render_emit(t, *s);
                *line_has_content = (*s != '\n');
            } else {
                if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
                    if (*line_has_content && !*last_space) {
                        render_emit(t, ' ');
                        *last_space = 1;
                    }
                } else {
                    render_emit(t, *s);
                    *line_has_content = 1;
                    *last_space = 0;
                }
            }
            s++;
        }
        return;
    }

    if (n->type != NODE_ELEMENT) return;

    int tag = n->tag;
    struct css_style st = default_style(tag);
    if (st.display == 2) return;  /* hidden */

    int new_heading = heading;
    int new_pre = in_pre;
    int new_list = in_list;

    /* Block-level line breaks before */
    if (st.display == 0) {
        if (tag == TAG_H1 || tag == TAG_H2 || tag == TAG_H3 ||
            tag == TAG_H4 || tag == TAG_H5 || tag == TAG_H6) {
            if (*line_has_content || t->render_len > 0) {
                render_newline(t); render_newline(t);
            }
            new_heading = tag;
        } else if (tag == TAG_P || tag == TAG_DIV || tag == TAG_SECTION ||
                   tag == TAG_ARTICLE || tag == TAG_HEADER || tag == TAG_FOOTER ||
                   tag == TAG_MAIN || tag == TAG_NAV || tag == TAG_BLOCKQUOTE) {
            if (*line_has_content) { render_newline(t); *line_has_content = 0; }
        } else if (tag == TAG_LI) {
            if (*line_has_content) render_newline(t);
            render_str(t, "  * ");
            *line_has_content = 1;
        } else if (tag == TAG_UL || tag == TAG_OL) {
            if (*line_has_content) render_newline(t);
            *line_has_content = 0;
            new_list = 1;
        } else if (tag == TAG_PRE || tag == TAG_CODE) {
            if (*line_has_content) render_newline(t);
            *line_has_content = 0;
            new_pre = 1;
        } else if (tag == TAG_HR) {
            if (*line_has_content) render_newline(t);
            for (int k = 0; k < 60; k++) render_emit(t, '-');
            render_newline(t);
            *line_has_content = 0;
        } else if (tag == TAG_TABLE || tag == TAG_TR) {
            if (*line_has_content) render_newline(t);
            *line_has_content = 0;
        }
    }

    /* Inline: links, images, br */
    if (tag == TAG_A) {
        const char *href = get_attr(n, "href");
        if (href && *link_idx < t->link_cap) {
            (*link_idx)++;
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "[%d]", *link_idx);
            render_str(t, lbl);
        }
    }
    if (tag == TAG_BR) {
        render_newline(t);
        *line_has_content = 0;
    }
    if (tag == TAG_IMG) {
        const char *alt = get_attr(n, "alt");
        render_str(t, "[img");
        if (alt && alt[0]) { render_emit(t, ':'); render_str(t, alt); }
        render_emit(t, ']');
        *line_has_content = 1;
    }
    if (tag == TAG_INPUT) {
        const char *type = get_attr(n, "type");
        const char *val  = get_attr(n, "value");
        render_str(t, "[");
        if (type) render_str(t, type); else render_str(t, "input");
        if (val && val[0]) { render_emit(t, ':'); render_str(t, val); }
        render_str(t, "]");
        *line_has_content = 1;
    }
    if (tag == TAG_BUTTON) {
        render_str(t, "[");
    }
    if ((tag == TAG_TD || tag == TAG_TH) && *line_has_content) {
        render_str(t, " | ");
    }

    /* Recurse into children */
    int child = n->first_child;
    while (child >= 0) {
        render_dom_text(t, child, line_has_content, last_space,
                        new_heading, new_pre, new_list, link_idx);
        child = t->dom[child].next_sibling;
    }

    if (tag == TAG_BUTTON) {
        render_str(t, "]");
    }

    /* Block-level line break after */
    if (st.display == 0 && *line_has_content) {
        if (tag == TAG_P || tag == TAG_DIV || tag == TAG_LI ||
            tag == TAG_H1 || tag == TAG_H2 || tag == TAG_H3 ||
            tag == TAG_H4 || tag == TAG_H5 || tag == TAG_H6 ||
            tag == TAG_SECTION || tag == TAG_ARTICLE ||
            tag == TAG_TR || tag == TAG_BLOCKQUOTE) {
            render_newline(t);
            *line_has_content = 0;
        }
    }
}

static void build_line_index(struct tab_state *t)
{
    t->line_count = 0;
    if (t->render_len == 0) return;

    int cols = COLS - 2;
    int col = 0;

    if (t->line_count < t->line_cap) {
        t->line_off[t->line_count] = 0;
        t->line_style[t->line_count] = STYLE_NORMAL;
        t->line_count++;
    }

    for (int i = 0; i < t->render_len && t->line_count < t->line_cap; i++) {
        if (t->render_buf[i] == '\n') {
            col = 0;
            if (i + 1 < t->render_len && t->line_count < t->line_cap) {
                t->line_off[t->line_count] = i + 1;
                t->line_style[t->line_count] = STYLE_NORMAL;
                t->line_count++;
            }
        } else {
            col++;
            if (col >= cols && t->line_count < t->line_cap) {
                /* Word-wrap: look back for space */
                int wrap = i;
                int search = i;
                int line_start = t->line_off[t->line_count - 1];
                while (search > line_start && t->render_buf[search] != ' ')
                    search--;
                if (search > line_start) wrap = search;

                t->line_off[t->line_count] = wrap + 1;
                t->line_style[t->line_count] = STYLE_NORMAL;
                t->line_count++;
                i = wrap;
                col = 0;
            }
        }
    }

    /* Tag line styles */
    for (int li = 0; li < t->line_count; li++) {
        int off = t->line_off[li];
        if (off >= t->render_len) continue;
        char c0 = t->render_buf[off];
        if (c0 == '[' && off + 1 < t->render_len &&
            t->render_buf[off + 1] >= '0' && t->render_buf[off + 1] <= '9')
            t->line_style[li] = STYLE_LINK;
        else if (off + 3 < t->render_len &&
                 t->render_buf[off] == ' ' && t->render_buf[off+1] == ' ' &&
                 t->render_buf[off+2] == '*')
            t->line_style[li] = STYLE_BULLET;
        else if (c0 == '-' && off + 10 < t->render_len) {
            int all_dash = 1;
            for (int k = 0; k < 10; k++)
                if (t->render_buf[off + k] != '-') { all_dash = 0; break; }
            if (all_dash) t->line_style[li] = STYLE_HR_;
        }
        else if (off + 4 < t->render_len && t->render_buf[off] == '[' &&
                 t->render_buf[off+1] == 'i')
            t->line_style[li] = STYLE_IMG;
    }

    t->total_height = t->line_count * CELL_H;
}

/* ================================================================
 * Full render pipeline
 * ================================================================ */

static void render_page(struct tab_state *t)
{
    t->render_len = 0;
    t->line_count = 0;

    if (t->dom_count > 0) {
        int lhc = 0, ls = 0, lk = 0;
        render_dom_text(t, 0, &lhc, &ls, 0, 0, 0, &lk);
        if (t->render_len > 0)
            t->render_buf[t->render_len] = '\0';
    }

    build_line_index(t);
}

/* ================================================================
 * Image loading
 * ================================================================ */

static void load_images(struct tab_state *t)
{
    t->image_count = 0;
    for (int i = 0; i < t->dom_count && t->image_count < MAX_IMAGES; i++) {
        if (t->dom[i].type != NODE_ELEMENT || t->dom[i].tag != TAG_IMG) continue;
        const char *src = get_attr(&t->dom[i], "src");
        if (!src || !src[0]) continue;

        char abs_url[URL_MAX];
        resolve_url(t->url, src, abs_url, URL_MAX);

        struct http_response resp;
        memset(&resp, 0, sizeof(resp));
        int err = http_get(abs_url, &resp);
        if (err == 0 && resp.body && resp.body_len > 0) {
            toby_image_t *img = toby_image_load((const uint8_t *)resp.body,
                                                resp.body_len);
            if (img) {
                struct loaded_image *li = &t->images[t->image_count++];
                li->dom_node = i;
                li->img = img;
                li->lw = min_i(img->width, WIN_W - 2 * PAD);
                li->lh = min_i(img->height, 400);
            }
        }
        http_response_free(&resp);
    }
}

/* ================================================================
 * Page fetching
 * ================================================================ */

static int is_html(const char *ct, const char *body, int len)
{
    if (ct) {
        if (strstr(ct, "text/html") || strstr(ct, "application/xhtml"))
            return 1;
    }
    for (int i = 0; i < len && i < 512; i++) {
        if (body[i] == '<') {
            if (strneqi(&body[i], "<html", 5) || strneqi(&body[i], "<!doc", 5) ||
                strneqi(&body[i], "<head", 5) || strneqi(&body[i], "<body", 5))
                return 1;
        }
    }
    return 0;
}

static void do_fetch(struct tab_state *t, const char *url)
{
    t->loading = 1;
    set_status("Loading...");

    /* Free old images */
    for (int i = 0; i < t->image_count; i++)
        toby_image_free(t->images[i].img);
    t->image_count = 0;
    t->dom_count   = 0;

    struct http_response resp;
    memset(&resp, 0, sizeof(resp));
    int err = http_get(url, &resp);

    t->loading = 0;

    if (err != 0) {
        snprintf(t->raw_body, t->raw_cap,
            "<html><body><h1>Error</h1>"
            "<p>Failed to load: %s</p>"
            "<p>HTTP error code: %d</p></body></html>",
            url, err);
        t->raw_len = (int)strlen(t->raw_body);
        str_copy(t->content_type, "text/html", 128);
    } else {
        /* Extract cookies from Set-Cookie header */
        const char *sc = http_get_header(&resp, "Set-Cookie");
        if (sc) {
            struct parsed_url pu;
            if (url_parse(url, &pu) == 0)
                cookie_set(t, pu.host, "session", sc);
        }

        if (resp.body && resp.body_len > 0) {
            int clen = (int)resp.body_len;
            if (clen >= t->raw_cap) clen = t->raw_cap - 1;
            memcpy(t->raw_body, resp.body, clen);
            t->raw_body[clen] = '\0';
            t->raw_len = clen;
        } else {
            t->raw_body[0] = '\0';
            t->raw_len = 0;
        }
        str_copy(t->content_type, resp.content_type, 128);
    }
    http_response_free(&resp);

    /* Non-HTML: offer download */
    if (!is_html(t->content_type, t->raw_body, t->raw_len) &&
        t->raw_len > 0 && !strstr(t->content_type, "text/")) {
        /* Save to /data/downloads/ */
        const char *fname = strrchr(url, '/');
        if (!fname) fname = "/download"; else fname++;
        char path[512];
        snprintf(path, sizeof(path), "/data/downloads/%s", fname);
        int dfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dfd >= 0) {
            write(dfd, t->raw_body, t->raw_len);
            close(dfd);
            snprintf(t->raw_body, t->raw_cap,
                "<html><body><h1>Download Complete</h1>"
                "<p>Saved to: %s</p>"
                "<p>Size: %d bytes</p></body></html>",
                path, t->raw_len);
            t->raw_len = (int)strlen(t->raw_body);
        }
    }

    /* Parse and render */
    html_parse(t, t->raw_body, t->raw_len);
    render_page(t);

    /* Load images in background */
    load_images(t);

    t->scroll_y = 0;
    char msg[128];
    snprintf(msg, sizeof(msg), "Done - %d lines, %d links",
             t->line_count, t->link_count);
    set_status(msg);
}

static void do_navigate(struct tab_state *t, const char *url)
{
    str_copy(t->url, url, URL_MAX);
    t->url_len = (int)strlen(t->url);

    /* Push to history */
    if (t->hist_pos < 0 || strcmp(t->history[t->hist_pos], url) != 0) {
        t->hist_pos++;
        if (t->hist_pos >= HISTORY_MAX) {
            memmove(t->history[0], t->history[1],
                    (HISTORY_MAX - 1) * (URL_MAX + 1));
            t->hist_pos = HISTORY_MAX - 1;
        }
        str_copy(t->history[t->hist_pos], url, URL_MAX);
        t->hist_count = t->hist_pos + 1;
    }

    do_fetch(t, url);
}

/* ================================================================
 * Home page
 * ================================================================ */

static void set_home_page(struct tab_state *t)
{
    const char *html =
        "<html><head><title>New Tab - TobyOS Browser</title></head><body>"
        "<h1>TobyOS Browser</h1>"
        "<p>A full-featured web browser built into tobyOS.</p>"
        "<h2>Getting Started</h2>"
        "<ul>"
        "<li>Press <b>Tab</b> to focus the address bar</li>"
        "<li>Type a URL and press Enter to navigate</li>"
        "<li>Supports HTTP and HTTPS connections</li>"
        "<li>Click links or use numbered link navigation</li>"
        "</ul>"
        "<h2>Keyboard Shortcuts</h2>"
        "<ul>"
        "<li><b>Tab</b> - Toggle URL bar / content focus</li>"
        "<li><b>Ctrl+T</b> - New tab</li>"
        "<li><b>Ctrl+W</b> - Close tab</li>"
        "<li><b>Ctrl+Tab</b> - Next tab</li>"
        "<li><b>Alt+Left</b> - Back</li>"
        "<li><b>Alt+Right</b> - Forward</li>"
        "<li><b>Ctrl+L</b> - Focus address bar</li>"
        "<li><b>Ctrl+F</b> - Find in page</li>"
        "<li><b>j/k</b> - Scroll up/down</li>"
        "<li><b>Space/Shift+Space</b> - Page down/up</li>"
        "<li><b>Home/End</b> - Top/bottom of page</li>"
        "<li><b>r</b> - Refresh page</li>"
        "<li><b>q</b> - Quit browser</li>"
        "</ul>"
        "<h2>Features</h2>"
        "<ul>"
        "<li>Up to 8 tabs with independent state</li>"
        "<li>Full HTML parsing with DOM tree</li>"
        "<li>CSS-aware rendering pipeline</li>"
        "<li>Image loading and display</li>"
        "<li>Form submission (GET and POST)</li>"
        "<li>Cookie storage per domain</li>"
        "<li>File downloads to /data/downloads/</li>"
        "<li>Navigation history per tab</li>"
        "<li>Find in page</li>"
        "</ul>"
        "<hr>"
        "<p>TobyOS Browser v4.0 - Media codecs and enhanced rendering.</p>"
        "</body></html>";

    int hlen = (int)strlen(html);
    if (hlen >= t->raw_cap) hlen = t->raw_cap - 1;
    memcpy(t->raw_body, html, hlen);
    t->raw_body[hlen] = '\0';
    t->raw_len = hlen;
    str_copy(t->content_type, "text/html", 128);

    html_parse(t, t->raw_body, t->raw_len);
    render_page(t);
    t->scroll_y = 0;
    t->url[0] = '\0';
    t->url_len = 0;
    str_copy(t->title, "New Tab", TITLE_MAX);
    set_status("Ready");
}

/* ================================================================
 * Form handling
 * ================================================================ */

static void submit_form(struct tab_state *t, int form_node)
{
    if (form_node < 0 || form_node >= t->dom_count) return;

    const char *action = get_attr(&t->dom[form_node], "action");
    const char *method = get_attr(&t->dom[form_node], "method");
    if (!action) action = t->url;

    char abs_action[URL_MAX];
    resolve_url(t->url, action, abs_action, URL_MAX);

    int is_post = (method && streqi(method, "post"));

    /* Collect input values */
    char query[2048];
    int qlen = 0;
    int first = 1;

    for (int i = 0; i < t->dom_count; i++) {
        if (t->dom[i].type != NODE_ELEMENT) continue;
        if (t->dom[i].tag != TAG_INPUT && t->dom[i].tag != TAG_TEXTAREA) continue;

        /* Check if this input is inside the form */
        int p = t->dom[i].parent;
        int inside = 0;
        while (p >= 0) {
            if (p == form_node) { inside = 1; break; }
            p = t->dom[p].parent;
        }
        if (!inside) continue;

        const char *name = get_attr(&t->dom[i], "name");
        const char *value = get_attr(&t->dom[i], "value");
        if (!name || !name[0]) continue;
        if (!value) value = "";

        if (!first && qlen < 2046) query[qlen++] = '&';
        int nlen = (int)strlen(name);
        int vlen = (int)strlen(value);
        for (int j = 0; j < nlen && qlen < 2046; j++)
            query[qlen++] = name[j];
        if (qlen < 2046) query[qlen++] = '=';
        for (int j = 0; j < vlen && qlen < 2046; j++)
            query[qlen++] = value[j];
        first = 0;
    }
    query[qlen] = '\0';

    if (is_post) {
        struct http_response resp;
        memset(&resp, 0, sizeof(resp));
        http_post(abs_action, "application/x-www-form-urlencoded",
                  query, qlen, &resp);
        if (resp.body && resp.body_len > 0) {
            int clen = (int)resp.body_len;
            if (clen >= t->raw_cap) clen = t->raw_cap - 1;
            memcpy(t->raw_body, resp.body, clen);
            t->raw_body[clen] = '\0';
            t->raw_len = clen;
        }
        http_response_free(&resp);
        html_parse(t, t->raw_body, t->raw_len);
        render_page(t);
        t->scroll_y = 0;
    } else {
        char full_url[URL_MAX];
        snprintf(full_url, URL_MAX, "%s?%s", abs_action, query);
        do_navigate(t, full_url);
    }
}

/* ================================================================
 * Drawing
 * ================================================================ */

static void draw_tab_bar(void)
{
    gui_fill(g_win_fd, 0, 0, WIN_W, TAB_H, C_TAB_BG);

    int tab_w = min_i(180, (WIN_W - 40) / max_i(g_tab_count, 1));

    for (int i = 0; i < g_tab_count; i++) {
        int tx = i * tab_w;
        uint32_t bg = (i == g_active_tab) ? C_TAB_ACTIVE : C_TAB_BG;
        gui_fill(g_win_fd, tx, 0, tab_w, TAB_H, bg);

        /* Active tab bottom indicator */
        if (i == g_active_tab)
            gui_fill(g_win_fd, tx, TAB_H - 2, tab_w, 2, C_URL_FOCUS);

        /* Tab title */
        char title[20];
        str_copy(title, g_tabs[i].title, 18);
        gui_text(g_win_fd, tx + 8, 7, title, C_TAB_TEXT, bg);

        /* Close button */
        gui_text(g_win_fd, tx + tab_w - 14, 7, "x", C_TAB_CLOSE, bg);
    }

    /* New tab + button */
    int plus_x = g_tab_count * tab_w + 8;
    gui_text(g_win_fd, plus_x, 7, "+", C_TAB_PLUS, C_TAB_BG);
}

static void draw_nav_bar(void)
{
    struct tab_state *t = cur_tab();
    int y = TAB_H;
    gui_fill(g_win_fd, 0, y, WIN_W, NAV_H, C_NAV_BG);

    /* Back/Forward/Reload/Home */
    uint32_t bc = (t->hist_pos > 0) ? C_BTN_FG : C_BTN_DIM;
    uint32_t fc = (t->hist_pos < t->hist_count - 1) ? C_BTN_FG : C_BTN_DIM;
    gui_text(g_win_fd, 8,  y + 10, "<", bc, C_NAV_BG);
    gui_text(g_win_fd, 24, y + 10, ">", fc, C_NAV_BG);
    gui_text(g_win_fd, 44, y + 10, "O", C_BTN_FG, C_NAV_BG);
    gui_text(g_win_fd, 60, y + 10, "H", C_BTN_FG, C_NAV_BG);

    /* URL bar */
    int ux = 80, uw = WIN_W - ux - 8;
    int uy = y + 5, uh = NAV_H - 10;

    uint32_t ubg = g_focus_url ? 0xFF111111u : C_URL_BG;
    uint32_t ubd = g_focus_url ? C_URL_FOCUS : C_URL_BORDER;
    gui_fill(g_win_fd, ux, uy, uw, uh, ubg);
    gui_fill(g_win_fd, ux, uy, uw, 1, ubd);
    gui_fill(g_win_fd, ux, uy + uh - 1, uw, 1, ubd);
    gui_fill(g_win_fd, ux, uy, 1, uh, ubd);
    gui_fill(g_win_fd, ux + uw - 1, uy, 1, uh, ubd);

    /* HTTPS lock icon */
    int is_https = (t->url_len >= 8 && strneqi(t->url, "https://", 8));
    gui_text(g_win_fd, ux + 6, uy + 4,
             is_https ? "L" : "i",
             is_https ? C_HTTPS : C_URL_HINT, ubg);

    /* URL text */
    int tx = ux + 20;
    int avail = (uw - 28) / CELL_W;
    char disp[256];
    const char *utext = g_focus_url ? g_url_input : t->url;
    int ulen = g_focus_url ? g_url_input_len : t->url_len;

    if (ulen > 0) {
        int start = 0;
        if (ulen > avail) start = ulen - avail;
        int dp = 0;
        for (int i = start; i < ulen && dp < avail; i++)
            disp[dp++] = utext[i];
        if (g_focus_url && dp < avail) disp[dp++] = '|';
        disp[dp] = '\0';
        gui_text(g_win_fd, tx, uy + 4, disp, C_URL_FG, ubg);
    } else if (g_focus_url) {
        gui_text(g_win_fd, tx, uy + 4, "|", C_URL_CURSOR, ubg);
    } else {
        gui_text(g_win_fd, tx, uy + 4, "Search or enter URL",
                 C_URL_HINT, ubg);
    }

    /* Loading indicator */
    if (t->loading)
        gui_fill(g_win_fd, ux, uy + uh - 2, uw / 3, 2, C_URL_FOCUS);
}

static void draw_content(void)
{
    struct tab_state *t = cur_tab();

    gui_fill(g_win_fd, 0, CONTENT_Y, WIN_W, CONTENT_H, C_PAGE_BG);

    int top_line = t->scroll_y / CELL_H;
    int y = CONTENT_Y - (t->scroll_y % CELL_H);
    char line_buf[COLS + 1];

    for (int r = 0; y < CONTENT_Y + CONTENT_H && top_line + r < t->line_count; r++) {
        int li = top_line + r;
        if (li < 0) { y += CELL_H; continue; }

        int off = t->line_off[li];
        int end;
        if (li + 1 < t->line_count) end = t->line_off[li + 1];
        else end = t->render_len;
        if (end > off && t->render_buf[end - 1] == '\n') end--;

        int len = end - off;
        if (len > COLS) len = COLS;
        if (len < 0) len = 0;

        for (int k = 0; k < len; k++) {
            char c = t->render_buf[off + k];
            if ((unsigned char)c < 0x20 && c != '\t') c = ' ';
            if ((unsigned char)c > 0x7E) c = '.';
            if (c == '\t') c = ' ';
            line_buf[k] = c;
        }
        line_buf[len] = '\0';

        uint32_t fg = C_TEXT, bg = C_PAGE_BG;
        switch (t->line_style[li]) {
        case STYLE_H1:     fg = C_H1; break;
        case STYLE_H2:     fg = C_H2; break;
        case STYLE_H3:     fg = C_H3; break;
        case STYLE_LINK:   fg = C_LINK; break;
        case STYLE_CODE:   fg = C_CODE_FG; bg = C_CODE_BG; break;
        case STYLE_BULLET: fg = C_TEXT; break;
        case STYLE_HR_:    fg = C_HR; break;
        case STYLE_BOLD:   fg = C_BOLD; break;
        case STYLE_IMG:    fg = C_LIST; bg = C_IMG_BG; break;
        }

        /* Find highlight */
        if (g_find_mode && g_find_line == li)
            bg = 0xFF333300u;

        if (len > 0 && y >= CONTENT_Y)
            gui_text(g_win_fd, PAD, y, line_buf, fg, bg);

        y += CELL_H;
    }

    /* Blit loaded images (positioned at approximate line positions) */
    for (int i = 0; i < t->image_count; i++) {
        struct loaded_image *im = &t->images[i];
        if (!im->img) continue;
        int iy = CONTENT_Y + (i + 1) * 100 - t->scroll_y;  /* approximate */
        if (iy < CONTENT_Y - im->lh || iy > CONTENT_Y + CONTENT_H) continue;
        gui_blit(g_win_fd, PAD, iy, im->lw, im->lh, im->img->pixels);
    }

    /* Scrollbar */
    if (t->total_height > CONTENT_H) {
        int track_x = WIN_W - 8;
        gui_fill(g_win_fd, track_x, CONTENT_Y, 6, CONTENT_H, C_SCROLLBAR);

        int max_scroll = t->total_height - CONTENT_H;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_h = max_i(20, (CONTENT_H * CONTENT_H) / t->total_height);
        int thumb_y = CONTENT_Y + (t->scroll_y * (CONTENT_H - thumb_h)) / max_scroll;
        gui_fill(g_win_fd, track_x, thumb_y, 6, thumb_h, C_THUMB);
    }
}

static void draw_status_bar(void)
{
    int y = WIN_H - STATUS_H;
    gui_fill(g_win_fd, 0, y, WIN_W, STATUS_H, C_STATUS_BG);
    gui_fill(g_win_fd, 0, y, WIN_W, 1, C_HR);

    if (g_find_mode) {
        gui_fill(g_win_fd, 0, y, WIN_W, STATUS_H, C_FIND_BG);
        char disp[192];
        snprintf(disp, sizeof(disp), "Find: %s|", g_find_buf);
        gui_text(g_win_fd, PAD, y + 3, disp, C_FIND_FG, C_FIND_BG);
    } else {
        gui_text(g_win_fd, PAD, y + 3, g_status, C_STATUS_FG, C_STATUS_BG);
    }

    /* Scroll percentage */
    struct tab_state *t = cur_tab();
    if (t->total_height > CONTENT_H && !g_find_mode) {
        int max_s = t->total_height - CONTENT_H;
        int pct = max_s > 0 ? (t->scroll_y * 100 / max_s) : 0;
        if (pct > 100) pct = 100;
        char pstr[8];
        snprintf(pstr, sizeof(pstr), "%d%%", pct);
        gui_text(g_win_fd, WIN_W - 50, y + 3, pstr, C_STATUS_FG, C_STATUS_BG);
    }
}

static void redraw(void)
{
    draw_tab_bar();
    draw_nav_bar();
    draw_content();
    draw_status_bar();
    gui_flip(g_win_fd);
}

/* ================================================================
 * Scrolling
 * ================================================================ */

static void scroll_by(int dy)
{
    struct tab_state *t = cur_tab();
    t->scroll_y += dy;
    int max_s = t->total_height - CONTENT_H;
    if (max_s < 0) max_s = 0;
    if (t->scroll_y < 0) t->scroll_y = 0;
    if (t->scroll_y > max_s) t->scroll_y = max_s;
}

/* ================================================================
 * Find in page
 * ================================================================ */

static void find_next(void)
{
    if (g_find_len == 0) return;
    struct tab_state *t = cur_tab();
    g_find_buf[g_find_len] = '\0';

    int start = (g_find_line >= 0) ? g_find_line + 1 : 0;
    for (int pass = 0; pass < 2; pass++) {
        int from = (pass == 0) ? start : 0;
        int to   = (pass == 0) ? t->line_count : start;
        for (int li = from; li < to; li++) {
            int off = t->line_off[li];
            int end = (li + 1 < t->line_count) ? t->line_off[li + 1] : t->render_len;
            int llen = end - off;
            /* Case-insensitive search */
            for (int k = 0; k <= llen - g_find_len; k++) {
                int match = 1;
                for (int j = 0; j < g_find_len; j++) {
                    char a = t->render_buf[off + k + j];
                    char b = g_find_buf[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
                if (match) {
                    g_find_line = li;
                    t->scroll_y = max_i(0, li * CELL_H - CONTENT_H / 2);
                    set_status("Found");
                    return;
                }
            }
        }
    }
    set_status("Not found");
}

/* ================================================================
 * Link hit testing
 * ================================================================ */

static int get_link_at_line(struct tab_state *t, int li)
{
    if (li < 0 || li >= t->line_count) return 0;
    int off = t->line_off[li];
    if (off >= t->render_len || t->render_buf[off] != '[') return 0;
    int p = off + 1, num = 0;
    while (p < t->render_len && t->render_buf[p] >= '0' && t->render_buf[p] <= '9') {
        num = num * 10 + (t->render_buf[p] - '0');
        p++;
    }
    if (p < t->render_len && t->render_buf[p] == ']' && num > 0)
        return num;
    return 0;
}

static void follow_link(struct tab_state *t, int num)
{
    if (num < 1 || num > t->link_count) {
        set_status("Invalid link");
        return;
    }
    char resolved[URL_MAX];
    resolve_url(t->url, t->links[num - 1], resolved, URL_MAX);
    do_navigate(t, resolved);
}

/* ================================================================
 * Mouse handling
 * ================================================================ */

static void handle_mouse_down(int mx, int my)
{
    struct tab_state *t = cur_tab();

    /* Tab bar clicks */
    if (my < TAB_H) {
        int tab_w = min_i(180, (WIN_W - 40) / max_i(g_tab_count, 1));
        int idx = mx / tab_w;
        if (idx >= 0 && idx < g_tab_count) {
            /* Close button */
            int close_x = idx * tab_w + tab_w - 14;
            if (mx >= close_x && mx <= close_x + 10) {
                tab_close(idx);
                redraw();
                return;
            }
            g_active_tab = idx;
            g_focus_url = 0;
            redraw();
            return;
        }
        /* New tab button */
        int plus_x = g_tab_count * tab_w + 8;
        if (mx >= plus_x - 4 && mx <= plus_x + 12) {
            int nt = tab_new();
            if (nt >= 0) set_home_page(&g_tabs[nt]);
            redraw();
            return;
        }
    }

    /* Navigation bar */
    if (my >= TAB_H && my < TAB_H + NAV_H) {
        if (mx < 20) {
            /* Back */
            if (t->hist_pos > 0) {
                t->hist_pos--;
                str_copy(t->url, t->history[t->hist_pos], URL_MAX);
                t->url_len = (int)strlen(t->url);
                do_fetch(t, t->url);
            }
        } else if (mx < 36) {
            /* Forward */
            if (t->hist_pos < t->hist_count - 1) {
                t->hist_pos++;
                str_copy(t->url, t->history[t->hist_pos], URL_MAX);
                t->url_len = (int)strlen(t->url);
                do_fetch(t, t->url);
            }
        } else if (mx < 56) {
            /* Reload */
            if (t->url_len > 0) do_fetch(t, t->url);
        } else if (mx < 72) {
            /* Home */
            set_home_page(t);
        } else {
            /* URL bar */
            g_focus_url = 1;
            memcpy(g_url_input, t->url, t->url_len);
            g_url_input_len = t->url_len;
            g_url_input[g_url_input_len] = '\0';
        }
        redraw();
        return;
    }

    /* Content area */
    if (my >= CONTENT_Y && my < WIN_H - STATUS_H) {
        /* Scrollbar */
        if (mx >= WIN_W - 8 && t->total_height > CONTENT_H) {
            int max_s = t->total_height - CONTENT_H;
            int rel = my - CONTENT_Y;
            t->scroll_y = (rel * max_s) / CONTENT_H;
            if (t->scroll_y < 0) t->scroll_y = 0;
            if (t->scroll_y > max_s) t->scroll_y = max_s;
            redraw();
            return;
        }

        /* Line click */
        int row = (my - CONTENT_Y + (t->scroll_y % CELL_H)) / CELL_H;
        int li = t->scroll_y / CELL_H + row;
        int link_num = get_link_at_line(t, li);
        if (link_num > 0) {
            follow_link(t, link_num);
            redraw();
            return;
        }

        g_focus_url = 0;
        redraw();
    }
}

static void handle_mouse_move(int mx, int my)
{
    struct tab_state *t = cur_tab();
    if (my >= CONTENT_Y && my < WIN_H - STATUS_H && mx < WIN_W - 8) {
        int row = (my - CONTENT_Y + (t->scroll_y % CELL_H)) / CELL_H;
        int li = t->scroll_y / CELL_H + row;
        int link_num = get_link_at_line(t, li);
        if (link_num > 0 && link_num <= t->link_count) {
            str_copy(g_status, t->links[link_num - 1], 256);
            draw_status_bar();
            gui_flip(g_win_fd);
        }
    }
}

/* ================================================================
 * Keyboard handling
 * ================================================================ */

static void handle_key(uint8_t key)
{
    struct tab_state *t = cur_tab();

    /* Find mode */
    if (g_find_mode) {
        if (key == 27) {
            g_find_mode = 0;
            g_find_line = -1;
            set_status("Find cancelled");
        } else if (key == '\n' || key == '\r') {
            find_next();
        } else if (key == '\b' || key == 127) {
            if (g_find_len > 0) g_find_len--;
        } else if (key >= 0x20 && key <= 0x7E && g_find_len < 126) {
            g_find_buf[g_find_len++] = (char)key;
            g_find_buf[g_find_len] = '\0';
        }
        redraw();
        return;
    }

    /* Ctrl+F = find */
    if (key == 0x06) {
        g_find_mode = 1;
        g_find_len = 0;
        g_find_line = -1;
        g_focus_url = 0;
        redraw();
        return;
    }

    /* Ctrl+T = new tab */
    if (key == 0x14) {
        int nt = tab_new();
        if (nt >= 0) set_home_page(&g_tabs[nt]);
        redraw();
        return;
    }

    /* Ctrl+W = close tab */
    if (key == 0x17) {
        if (g_tab_count > 1)
            tab_close(g_active_tab);
        else
            _exit(0);
        redraw();
        return;
    }

    /* Tab key toggles focus */
    if (key == '\t') {
        g_focus_url = !g_focus_url;
        if (g_focus_url) {
            memcpy(g_url_input, t->url, t->url_len);
            g_url_input_len = t->url_len;
            g_url_input[g_url_input_len] = '\0';
            set_status("URL bar focused");
        } else {
            set_status("Content focused");
        }
        redraw();
        return;
    }

    /* URL bar input */
    if (g_focus_url) {
        if (key == '\n' || key == '\r') {
            if (g_url_input_len > 0) {
                g_url_input[g_url_input_len] = '\0';
                /* Auto-prepend http:// */
                if (g_url_input_len < 7 || !strneqi(g_url_input, "http", 4)) {
                    char tmp[URL_MAX];
                    memcpy(tmp, g_url_input, g_url_input_len + 1);
                    memcpy(g_url_input, "http://", 7);
                    memcpy(g_url_input + 7, tmp, g_url_input_len + 1);
                    g_url_input_len += 7;
                    g_url_input[g_url_input_len] = '\0';
                }
                do_navigate(t, g_url_input);
                g_focus_url = 0;
            }
        } else if (key == '\b' || key == 127) {
            if (g_url_input_len > 0) g_url_input_len--;
        } else if (key == 27) {
            g_focus_url = 0;
            set_status("Cancelled");
        } else if (key >= 0x20 && key <= 0x7E && g_url_input_len < URL_MAX) {
            g_url_input[g_url_input_len++] = (char)key;
            g_url_input[g_url_input_len] = '\0';
        }
        redraw();
        return;
    }

    /* Content area keys */
    switch (key) {
    case 'j': scroll_by(CELL_H * 3); break;
    case 'k': scroll_by(-CELL_H * 3); break;
    case 'd': case ' ': scroll_by(CONTENT_H - CELL_H); break;
    case 'u': scroll_by(-(CONTENT_H - CELL_H)); break;
    case 'g': t->scroll_y = 0; break;
    case 'G': {
        int max_s = t->total_height - CONTENT_H;
        t->scroll_y = max_i(0, max_s);
        break;
    }
    case 'n':
        if (g_find_len > 0) find_next();
        break;
    case 'r':
        if (t->url_len > 0) do_fetch(t, t->url);
        else set_status("No page to refresh");
        break;
    case 'h':
        set_home_page(t);
        break;
    case '[':
        if (t->hist_pos > 0) {
            t->hist_pos--;
            str_copy(t->url, t->history[t->hist_pos], URL_MAX);
            t->url_len = (int)strlen(t->url);
            do_fetch(t, t->url);
        }
        break;
    case ']':
        if (t->hist_pos < t->hist_count - 1) {
            t->hist_pos++;
            str_copy(t->url, t->history[t->hist_pos], URL_MAX);
            t->url_len = (int)strlen(t->url);
            do_fetch(t, t->url);
        }
        break;
    case '/':
        g_focus_url = 1;
        memcpy(g_url_input, t->url, t->url_len);
        g_url_input_len = t->url_len;
        g_url_input[g_url_input_len] = '\0';
        set_status("URL bar focused");
        break;
    case 'q':
        _exit(0);
    default:
        /* Numbered link following */
        if (key >= '1' && key <= '9' && t->link_count > 0) {
            int num = key - '0';
            if (num <= t->link_count) {
                follow_link(t, num);
            }
        }
        break;
    }
    redraw();
}

/* ================================================================
 * Main entry
 * ================================================================ */

int main(int argc, char **argv)
{
    (void)argc;

    /* Initialise first tab */
    tab_init(&g_tabs[0]);

    /* Create window */
    g_win_fd = gui_create(WIN_W, WIN_H, "TobyOS Browser");
    if (g_win_fd < 0) {
        const char *msg = "browser: failed to create window\n";
        write(1, msg, strlen(msg));
        return 1;
    }

    /* If URL passed on command line, navigate there */
    if (argc >= 2 && argv && argv[1] && argv[1][0]) {
        do_navigate(&g_tabs[0], argv[1]);
    } else {
        set_home_page(&g_tabs[0]);
    }

    redraw();

    /* Event loop */
    struct gui_event ev;
    for (;;) {
        int got = gui_poll(g_win_fd, &ev);
        if (got <= 0) {
            sc0(SYS_YIELD);
            continue;
        }

        switch (ev.type) {
        case EV_CLOSE:
            _exit(0);
        case EV_MOUSE_DOWN:
            handle_mouse_down(ev.x, ev.y);
            break;
        case EV_MOUSE_MOVE:
            handle_mouse_move(ev.x, ev.y);
            break;
        case EV_KEY:
            handle_key(ev.key);
            break;
        case EV_SCROLL:
            scroll_by(ev.y * -CELL_H * 3);
            redraw();
            break;
        default:
            break;
        }
    }

    return 0;
}
