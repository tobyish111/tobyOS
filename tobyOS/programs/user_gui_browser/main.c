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
#define SYS_HTTP_FETCH    171

/* MUST mirror struct abi_http_fetch in include/tobyos/abi/abi.h (user
 * programs build against libtoby headers only, so the layout is
 * duplicated here; the struct is reserved-padded and ABI-frozen). */
struct http_fetch {
    unsigned long url;              /* in: const char * */
    unsigned long buf;              /* in: body destination */
    uint32_t      buf_sz;
    uint32_t      flags;            /* must be 0 */
    int32_t       status;           /* out: HTTP status of final response */
    uint32_t      body_total;       /* out: full body length pre-truncation */
    char          final_url[512];   /* out: post-redirect URL */
    char          content_type[64];
    uint8_t       reserved[64];
};

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
#include <toby/image.h>     /* stb_image-backed ARGB decoder (libtoby) */
/* QuickJS (third_party/quickjs, linked by the Makefile). Included this
 * early so its headers see none of the short macros defined below
 * (E, cur, g_raw, ...). Its inline helpers trip -Wextra; scoped off. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#include "quickjs.h"
#pragma clang diagnostic pop
static struct tk_window win;

/* libtoby heap (stdlib.c over sbrk); the image path allocates fetch +
 * decode buffers. Declared here to avoid pulling <stdlib.h>. */
extern void *malloc(unsigned long);
extern void  free(void *);

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
static inline long sys_http_fetch(struct http_fetch *req) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_FETCH), "D"(req)
        : "rcx", "r11", "memory");
    return r;
}
#define SYS_GUI_SET_TITLE  76
static inline void sys_gui_set_title(int fd, const char *t) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_SET_TITLE), "D"((long)fd), "S"(t)
        : "rcx", "r11", "memory");
    (void)r;
}
#define SYS_NANOSLEEP  47
static inline void sys_sleep_ms(int ms) {
    long r, ns = (long)ms * 1000000L;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"((long)SYS_NANOSLEEP), "D"(ns)
        : "rcx", "r11", "memory");
    (void)r;
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

static int atoi_simple(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
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

#define WIN_W        720          /* initial window size only */
#define WIN_H        500

#define TAB_BAR_H     22
#define NAV_BAR_H     28
#define TOOLBAR_H     (TAB_BAR_H + NAV_BAR_H)
#define STATUS_H      16
#define CONTENT_TOP   TOOLBAR_H
#define PAD            6
#define CELL_W         8
#define CELL_H        12

/* Live layout metrics, refreshed from the toolkit window each paint /
 * mouse event (sync_geometry) so a WM resize reflows everything. The
 * chrome bands (tab/nav/status) keep fixed heights; the page area and
 * the text grid are derived from the live client size. */
static int g_win_w     = WIN_W;
static int g_win_h     = WIN_H;
static int g_content_h = WIN_H - TOOLBAR_H - STATUS_H;

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

/* ---- Content buffer -------------------------------------------- */

/* 512 KiB of page HTML: enough for every full Wikipedia article body
 * (kernel SYS_HTTP_FETCH truncates past this). raw[] lives inline in
 * struct tab -> TAB_MAX * 512K = 3 MiB BSS; the eng pools below scale
 * with it (heap, per open tab). */
#define RAW_CAP       (512 * 1024)

/* Visible-text pool: must exceed RAW_CAP so view-source of a max-size
 * page keeps every character (source view flows raw through a pre). */
#define RENDER_CAP    (576 * 1024)

/* Body font size (px); code/pre use the fixed 8x16 mono font. */
#define PX_BODY   15
#define MONO_W     8
#define MONO_H    16

/* ---- Forms (GET submission) -------------------------------------- */

#define FT_TEXT    0
#define FT_HIDDEN  1
#define FT_SUBMIT  2

struct form {
    int16_t  first, nf;          /* g_fields slice */
    uint8_t  post;               /* method=post (submit refused for now) */
    char     action[512];
};
#define FORM_MAX 16

struct field {
    int16_t  form;
    uint8_t  type;               /* FT_* */
    int16_t  size;               /* <input size=N> or 0 */
    int16_t  node;               /* DOM node (JS 'input' events + .value) */
    char     name[64];
    char     value[192];         /* editable in place for FT_TEXT */
};
#define FIELD_MAX 64


/* ---- Images ------------------------------------------------------ */

struct img {
    char      src[512];          /* resolved absolute URL */
    int16_t   attr_w, attr_h;    /* width/height attributes, 0 if absent */
    int16_t   w, h;              /* decoded dimensions (0 until loaded) */
    uint32_t *pixels;            /* decoded ARGB8888, NULL until loaded */
    int8_t    state;             /* 0 pending, 1 loaded, -1 failed */
};
#define IMG_MAX      48          /* image records tracked per page */
#define IMG_FETCH_N  16          /* how many we actually fetch+decode */
#define IMG_FETCH_CAP (1u << 20) /* per-image download cap (1 MiB) */
#define IMG_MAX_DIM  1600        /* skip absurd decoded dimensions */

/* Advance-width cache for the proportional TTF faces, keyed by
 * (px, bold) since CSS font-size is continuous. Filled lazily one char
 * at a time via tk_text_width (the kernel rasterizer is advance-based,
 * so per-char sums equal string widths); afterwards all wrapping math
 * is userspace. Round-robin eviction; a page uses a handful of sizes. */
#define ADV_SLOTS 14
struct advtab { short adv[95]; short px; short bold; short used; };
static struct advtab g_advc[ADV_SLOTS];
static int g_advc_rr = 0;

/* =================================================================
 *  ENGINE: DOM tree + CSSOM + computed styles + box layout
 *
 *  The retained-tree pipeline that replaces the flat span/block/run
 *  model:  HTML -> DOM tree; CSS (<style>, style=, <link rel=
 *  stylesheet>) -> rules; cascade -> computed style per node; layout
 *  (block flow + inline formatting contexts) -> display-list items
 *  painted with the kernel TTF/blit primitives. All storage lives in
 *  one heap-allocated `struct eng` per tab; nodes link by index so
 *  the tab-close struct-copy shift stays safe.
 * ================================================================= */

/* ---- Tag ids ----------------------------------------------------- */
enum {
    T_TEXT = 0, T_UNK,
    T_HTML, T_HEAD, T_BODY, T_TITLE, T_STYLE, T_SCRIPT, T_LINKE, T_META,
    T_NOSCRIPT, T_TEMPLATE,
    T_DIV, T_P, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6,
    T_UL, T_OL, T_LI, T_DL, T_DT, T_DD,
    T_TABLE, T_THEAD, T_TBODY, T_TFOOT, T_TR, T_TD, T_TH, T_CAPTION,
    T_COL, T_COLGROUP,
    T_PRE, T_BLOCKQUOTE, T_HR, T_BR,
    T_A, T_SPAN, T_B, T_STRONG, T_I, T_EM, T_U, T_S, T_CODE, T_TT,
    T_KBD, T_SAMP, T_VAR, T_MARK, T_SMALL, T_BIG, T_SUB, T_SUP, T_FONT,
    T_CENTER, T_ABBR, T_CITE, T_Q, T_LABEL, T_TIME,
    T_IMG, T_FORM, T_INPUT, T_BUTTON, T_SELECT, T_OPTION, T_TEXTAREA,
    T_FIELDSET, T_LEGEND,
    T_HEADER, T_FOOTER, T_NAV, T_MAIN, T_SECTION, T_ARTICLE, T_ASIDE,
    T_FIGURE, T_FIGCAPTION, T_ADDRESS, T_DETAILS, T_SUMMARY,
    T_IFRAME, T_OBJECT, T_EMBED, T_VIDEO, T_AUDIO, T_CANVAS, T_SVG,
    T_AREA, T_BASE, T_SOURCE, T_TRACK, T_WBR, T_PARAM, T_MAPE, T_PICTURE,
    T_NTAGS
};

/* ---- Computed style ---------------------------------------------- */

#define D_INLINE    0
#define D_BLOCK     1
#define D_NONE      2
#define D_LISTITEM  3
#define D_INLBLOCK  4
#define D_TABLE     5            /* table wrapper box (grid layout)     */
#define D_TSEC      6            /* thead/tbody/tfoot row group         */
#define D_TROW      7            /* table row                           */
#define D_TCELL     8            /* td/th (block container in the grid) */
#define D_CAPTION   9            /* caption: block above the grid       */

/* style flag bits */
#define SF_BOLD     0x01
#define SF_UNDER    0x02
#define SF_MONO     0x04
#define SF_PRE      0x08
#define SF_WPCT     0x10         /* width is a percentage */
#define SF_NOBULLET 0x20         /* list-style: none */
#define SF_BCOLLAPSE 0x40        /* border-collapse: collapse (tables) */

#define M_AUTO  (-32768)         /* margin: auto sentinel */

struct cstyle {
    uint32_t color;
    uint32_t bg;                 /* 0 alpha = transparent/none */
    uint32_t border_col;
    int32_t  width, height;      /* px (-1 auto; width may be % per SF_WPCT) */
    int32_t  max_w;              /* px, -1 none */
    int16_t  m[4], p[4];         /* margin/padding: T R B L */
    uint8_t  bw[4];              /* border widths:  T R B L */
    uint8_t  disp;               /* D_* */
    uint8_t  px;                 /* font size */
    uint8_t  fl;                 /* SF_* */
    uint8_t  talign;             /* 0 left, 1 center, 2 right */
    int16_t  line_h;             /* >0 px; <0 scale*-100; 0 auto */
    uint8_t  flt;                /* float: 0 none, 1 left, 2 right */
    uint8_t  clr;                /* clear mask: 1 left, 2 right, 3 both */
    uint8_t  valign;             /* cells: 0 top/baseline, 1 middle, 2 bottom */
    uint8_t  _pad0[3];
};

/* ---- DOM ---------------------------------------------------------- */

#define DNF_STYLE_DONE 1     /* <style> content already parsed into rules */

struct dnode {
    int32_t parent, first, last, next;   /* tree links (node idx, -1) */
    int32_t toff, tlen;          /* T_TEXT: tpool slice (decoded) */
    int32_t attr0;               /* first attr in attr pool */
    int16_t nattr;
    int16_t tag;                 /* T_* */
    int16_t link, field, img;    /* interaction indices or -1 */
    int16_t flags;               /* DNF_* */
    struct cstyle st;            /* computed by the style pass */
};
#define NODE_MAX  32768

struct dattr {                   /* name/value slices in tpool */
    int32_t noff, voff, vlen;
    int16_t nlen;
    int16_t _pad;
};
#define ATTR_MAX  32768
#define TPOOL_CAP (768 * 1024)

/* ---- CSSOM -------------------------------------------------------- */

struct cpart {                   /* one compound selector part */
    int16_t tag;                 /* T_* or -1 = any */
    uint8_t comb;                /* combinator to the LEFT: 0 none/first,
                                    1 descendant, 2 child */
    uint8_t nclass;
    uint8_t pseudo_link;         /* :link / :visited present */
    uint8_t has_attr;
    int32_t id_off;   int16_t id_len;
    int32_t cls_off[2]; int16_t cls_len[2];
    int32_t an_off;   int16_t an_len;    /* [name] / [name=value] */
    int32_t av_off;   int16_t av_len;    /* 0 len = presence test */
};
#define PART_MAX  24576

struct cdecl {
    uint8_t prop;                /* CP_* */
    uint8_t imp;                 /* !important */
    int16_t vlen;
    int32_t voff;                /* raw value text in csspool */
};
#define DECL_MAX  32768

struct crule {
    int32_t part0; int16_t nparts;
    int16_t origin;              /* 0 UA, 1 author */
    int32_t decl0; int16_t ndecl;
    uint16_t spec;               /* specificity: 100*id + 10*(cls/attr) + type */
    int32_t order;               /* source order for cascade ties */
};
#define RULE_MAX    8192
#define CSSPOOL_CAP (320 * 1024)

/* property ids */
enum {
    CP_DISPLAY, CP_COLOR, CP_BGCOLOR, CP_BG,
    CP_FONT_SIZE, CP_FONT_WEIGHT, CP_FONT_FAMILY, CP_FONT_STYLE, CP_FONT,
    CP_TALIGN, CP_TDECOR, CP_LINEH, CP_WSPACE,
    CP_LSTYLE, CP_LSTYLE_TYPE,
    CP_MARGIN, CP_MT, CP_MR, CP_MB, CP_ML,
    CP_PADDING, CP_PT, CP_PR, CP_PB, CP_PL,
    CP_BORDER, CP_BTOP, CP_BRIGHT, CP_BBOTTOM, CP_BLEFT,
    CP_BWIDTH, CP_BCOLOR,
    CP_WIDTH, CP_HEIGHT, CP_MAXW, CP_VISIBILITY,
    CP_FLOAT, CP_CLEAR,
    CP_VALIGN, CP_BCOLLAPSE,
    CP__N
};

/* ---- Display list -------------------------------------------------- */

#define DI_RECT   1              /* backgrounds, borders, hr */
#define DI_TEXT   2
#define DI_IMG    3
#define DI_FIELD  4
#define DI_BULLET 5

/* item flag bits */
#define IF_BOLD    0x01
#define IF_UNDER   0x02
#define IF_MONO    0x04
#define IF_INPUT   0x08          /* field: text input box */
#define IF_SUBMITB 0x10          /* field: submit button */

struct ditem {
    int32_t x, y, w, h;          /* doc coords */
    uint32_t fg, bg;             /* text fg / rect color; text bg (0=none) */
    int32_t off;                 /* render-pool slice (DI_TEXT) */
    int32_t node;                /* source DOM node (JS event target), -1 */
    int16_t len;
    int16_t link, field, img;    /* interaction indices or -1 */
    uint8_t kind, px, fl, _pad2;
};
#define ITEM_MAX  49152

/* ---- The per-tab engine (heap-allocated, ~8 MiB) ------------------- */

struct eng {
    /* DOM */
    struct dnode nodes[NODE_MAX];   int nnodes;
    struct dattr attrs[ATTR_MAX];   int nattrs;
    char   tpool[TPOOL_CAP];        int tpool_len;
    int    body;                    /* <body> node idx or -1 */
    /* CSSOM (UA rules first, then author) */
    struct cpart parts[PART_MAX];   int nparts;
    struct cdecl decls[DECL_MAX];   int ndecls;
    struct crule rules[RULE_MAX];   int nrules;
    char   csspool[CSSPOOL_CAP];    int csspool_len;
    int    css_order;               /* running decl-order counter */
    int    nsheets;                 /* fetched <link> sheets so far */
    /* display list + collapsed visible text (find-in-page) */
    struct ditem items[ITEM_MAX];   int nitems;
    char   render[RENDER_CAP + 1];  int render_len;
    uint32_t page_bg;               /* body/html background */
};

#define LINK_MAX       128
#define LINK_URL_MAX   256

#define URL_MAX        1024

#define TITLE_MAX      64

#define HISTORY_MAX    32

static char g_status_text[128];

/* Find-in-page state */
#define FIND_MAX 64

/* Source view toggle */

/* What produced g_render -- picks the line-index style + wrap rules,
 * and lets a resize re-wrap without re-fetching or re-parsing. */
#define VIEW_HTML    0
#define VIEW_PLAIN   1
#define VIEW_SOURCE  2

/* HTTP status of the last successful transport fetch (0 = none). */

/* ---- Tabs: per-page state bundle -------------------------------- *
 * Every field that makes up "the current page" lives here; the browser
 * keeps an array of tabs and an active index. A `#define g_foo
 * (cur->foo)` shim below lets the ~500 existing g_* accesses address the
 * active tab unchanged, so switching tabs is just moving g_active +
 * re-layout + repaint (no re-fetch, no re-render). Bundle is ~1 MiB;
 * TAB_MAX inline tabs => a few MiB of BSS (the ELF loader maps+zeroes it
 * eagerly, trivially affordable). Window geometry, the font advance
 * tables, the status bar and parser scratch stay app-global (below). */
/* Phase 10: one-shot/interval timers owned by a tab's JS runtime. */
#define JS_TIMER_MAX 32
struct jstimer {
    int      id;
    long     due_ms;
    long     interval_ms;        /* 0 = one-shot */
    JSValue  fn;
    uint8_t  used;
};

struct tab {
    char   raw[RAW_CAP + 1];   long raw_len;
    struct eng *eng;           /* DOM/CSS/layout engine (heap, ~8 MiB) */
    /* phase 10: the page's JS world persists for the page lifetime so
     * listeners/timers/promises can fire after load */
    JSRuntime *js_rt;
    JSContext *js_cx;
    JSValue    js_dispatch;    /* prelude event dispatcher */
    int        js_has_dispatch;
    struct jstimer js_timers[JS_TIMER_MAX];
    int        js_timer_seq;
    char       js_nav[URL_MAX + 1];   /* deferred location.href target */
    struct form forms[FORM_MAX];    int nforms;
    struct field fields[FIELD_MAX]; int nfields; int focus_field;
    struct img  images[IMG_MAX];    int nimages;
    char   links[LINK_MAX][LINK_URL_MAX]; int link_count;
    char   url[URL_MAX + 1];   int url_len;
    char   title[TITLE_MAX + 1];
    char   history[HISTORY_MAX][URL_MAX + 1]; int hist_pos, hist_count;
    int    doc_h, scroll_y, layout_w, find_run;
    int    focus_url, loading;
    int    find_mode; char find_buf[FIND_MAX + 1]; int find_len;
    int    source_view, view_mode, last_status;
    int    used;               /* slot occupied */
};
#define TAB_MAX 6
static struct tab g_tabs[TAB_MAX];
static int g_ntabs  = 0;
static int g_active = 0;
#define cur (&g_tabs[g_active])
#define E   (cur->eng)

#define g_raw         (cur->raw)
#define g_raw_len     (cur->raw_len)
#define g_render      (E->render)
#define g_render_len  (E->render_len)
#define g_items       (E->items)
#define g_nitems      (E->nitems)
#define g_forms       (cur->forms)
#define g_nforms      (cur->nforms)
#define g_fields      (cur->fields)
#define g_nfields     (cur->nfields)
#define g_focus_field (cur->focus_field)
#define g_images      (cur->images)
#define g_nimages     (cur->nimages)
#define g_links       (cur->links)
#define g_link_count  (cur->link_count)
#define g_url         (cur->url)
#define g_url_len     (cur->url_len)
#define g_title       (cur->title)
#define g_history     (cur->history)
#define g_hist_pos    (cur->hist_pos)
#define g_hist_count  (cur->hist_count)
#define g_doc_h       (cur->doc_h)
#define g_scroll_y    (cur->scroll_y)
#define g_layout_w    (cur->layout_w)
#define g_find_run    (cur->find_run)
#define g_focus_url   (cur->focus_url)
#define g_loading     (cur->loading)
#define g_find_mode   (cur->find_mode)
#define g_find_buf    (cur->find_buf)
#define g_find_len    (cur->find_len)
#define g_source_view (cur->source_view)
#define g_view_mode   (cur->view_mode)
#define g_last_status (cur->last_status)

/* ---- Parsing helpers -------------------------------------------- */

static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int is_alpha(char c) { c = (char)lc(c); return c >= 'a' && c <= 'z'; }

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

/* ---- Text measurement (dynamic advance cache) --------------------- */

static const short *adv_for(int px, int bold) {
    if (px < 8) px = 8;
    if (px > 40) px = 40;
    bold = bold ? 1 : 0;
    for (int i = 0; i < ADV_SLOTS; i++)
        if (g_advc[i].used && g_advc[i].px == px && g_advc[i].bold == bold)
            return g_advc[i].adv;
    struct advtab *t = &g_advc[g_advc_rr];
    g_advc_rr = (g_advc_rr + 1) % ADV_SLOTS;
    char one[2] = { 0, 0 };
    for (int c = 0; c < 95; c++) {
        one[0] = (char)(32 + c);
        int w = tk_text_width(one, px, bold);
        t->adv[c] = (short)(w > 0 ? w : px / 2);
    }
    t->px = (short)px; t->bold = (short)bold; t->used = 1;
    return t->adv;
}

static int text_px_w(const char *s, int len, int px, int bold, int mono) {
    if (mono) return len * MONO_W;
    const short *adv = adv_for(px, bold);
    int w = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        w += adv[(c >= 32 && c <= 126) ? c - 32 : '?' - 32];
    }
    return w;
}

/* Resolve a computed line-height against the node's font. */
static int st_line_h(const struct cstyle *st) {
    if (st->fl & SF_MONO) return MONO_H + 2;
    if (st->line_h > 0) return st->line_h;
    if (st->line_h < 0) {
        int lh = (-st->line_h * st->px) / 100;
        return lh < st->px ? st->px : lh;
    }
    return st->px + st->px / 3;
}

/* ---- Text pool: entity decoding + UTF-8 -> ASCII ------------------ */

static void tp_putc(char c) {
    if (E->tpool_len < TPOOL_CAP - 1) E->tpool[E->tpool_len++] = c;
}
static void tp_puts(const char *s) { while (*s) tp_putc(*s++); }

/* Transliterate a Unicode codepoint to ASCII into the tpool (the
 * kernel TTF paint path is ASCII-only). */
static void tp_put_cp(unsigned int cp) {
    if (cp == 0xA0) { tp_putc(' '); return; }
    if (cp == '\t') { tp_putc(' '); tp_putc(' '); return; }
    if (cp < 0x20) { if (cp == '\n') tp_putc('\n'); return; }
    if (cp < 0x7F) { tp_putc((char)cp); return; }
    if (cp >= 0xC0 && cp <= 0xFF) {          /* Latin-1 letters */
        static const char l1[65] =
            "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
        tp_putc(l1[cp - 0xC0]);
        return;
    }
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: tp_putc('\''); return;
    case 0x201C: case 0x201D: case 0x201E: tp_putc('"');  return;
    case 0x2013: case 0x2014: case 0x2212: tp_putc('-');  return;
    case 0x2026: tp_puts("...");  return;
    case 0x2022: case 0xB7: tp_putc('*'); return;
    case 0xA9:   tp_puts("(c)");  return;
    case 0xAE:   tp_puts("(R)");  return;
    case 0x2122: tp_puts("(TM)"); return;
    case 0xD7:   tp_putc('x');  return;
    case 0xAB:   tp_puts("<<"); return;
    case 0xBB:   tp_puts(">>"); return;
    case 0x2192: tp_puts("->"); return;
    case 0x2190: tp_puts("<-"); return;
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: return;
    default:     tp_putc('?');  return;
    }
}

/* Decode the HTML entity at src[i]=='&'. Returns total chars consumed
 * (including the '&') and sets *cp, or 0 if unrecognized. */
static int entity_cp(const char *src, long i, long len, unsigned int *cp) {
    long start = i + 1;
    if (start >= len) return 0;
    if (src[start] == '#') {
        long p = start + 1;
        unsigned int val = 0;
        int digits = 0;
        if (p < len && (src[p] == 'x' || src[p] == 'X')) {
            p++;
            while (p < len && hex_digit(src[p]) >= 0 && digits < 6) {
                val = val * 16 + (unsigned)hex_digit(src[p]);
                p++; digits++;
            }
        } else {
            while (p < len && src[p] >= '0' && src[p] <= '9' && digits < 7) {
                val = val * 10 + (unsigned)(src[p] - '0');
                p++; digits++;
            }
        }
        if (!digits || p >= len || src[p] != ';') return 0;
        *cp = val;
        return (int)(p - i + 1);
    }
    {
        static const struct { const char *n; unsigned short c; } ents[] = {
            {"lt",'<'},{"gt",'>'},{"amp",'&'},{"quot",'"'},{"apos",'\''},
            {"nbsp",0xA0},{"copy",0xA9},{"reg",0xAE},{"trade",0x2122},
            {"mdash",0x2014},{"ndash",0x2013},{"hellip",0x2026},
            {"laquo",0xAB},{"raquo",0xBB},{"lsquo",0x2018},{"rsquo",0x2019},
            {"ldquo",0x201C},{"rdquo",0x201D},{"bull",0x2022},
            {"middot",0xB7},{"times",0xD7},{"shy",0xAD},
        };
        for (unsigned k = 0; k < sizeof(ents) / sizeof(ents[0]); k++)
            if (entity_match(src, start, len, ents[k].n)) {
                *cp = ents[k].c;
                return (int)str_len(ents[k].n) + 2;
            }
    }
    return 0;
}

/* Append raw HTML text [s, s+n) to the tpool: decodes entities,
 * transliterates UTF-8, drops CR. Returns bytes appended. */
static int tp_put_html(const char *s, long n) {
    int start = E->tpool_len;
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') continue;
        if (c == '&') {
            unsigned int cp;
            int adv = entity_cp(s, i, n, &cp);
            if (adv > 0) { tp_put_cp(cp); i += adv - 1; continue; }
            tp_putc('&');
            continue;
        }
        if (c < 0x80) { tp_put_cp(c); continue; }
        /* UTF-8 sequence -> codepoint */
        unsigned int cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else continue;                    /* stray continuation byte */
        if (i + extra >= n) break;
        int ok = 1;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) { tp_put_cp(cp); i += extra; }
    }
    return E->tpool_len - start;
}

/* Append UTF-8 text [s, s+n) to the tpool WITHOUT entity decoding
 * (JS textContent strings are literal). */
static int tp_put_utf8(const char *s, long n) {
    int start = E->tpool_len;
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') continue;
        if (c < 0x80) { tp_put_cp(c); continue; }
        unsigned int cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else continue;
        if (i + extra >= n) break;
        int ok = 1;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) { tp_put_cp(cp); i += extra; }
    }
    return E->tpool_len - start;
}

/* ---- Tag table ---------------------------------------------------- */

static const char * const g_tag_names[T_NTAGS] = {
    "#text","#unk",
    "html","head","body","title","style","script","link","meta",
    "noscript","template",
    "div","p","h1","h2","h3","h4","h5","h6",
    "ul","ol","li","dl","dt","dd",
    "table","thead","tbody","tfoot","tr","td","th","caption",
    "col","colgroup",
    "pre","blockquote","hr","br",
    "a","span","b","strong","i","em","u","s","code","tt",
    "kbd","samp","var","mark","small","big","sub","sup","font",
    "center","abbr","cite","q","label","time",
    "img","form","input","button","select","option","textarea",
    "fieldset","legend",
    "header","footer","nav","main","section","article","aside",
    "figure","figcaption","address","details","summary",
    "iframe","object","embed","video","audio","canvas","svg",
    "area","base","source","track","wbr","param","map","picture",
};

static int tag_lookup(const char *s, int len) {
    if (len <= 0 || len > 12) return T_UNK;
    for (int t = T_HTML; t < T_NTAGS; t++) {
        const char *n = g_tag_names[t];
        int i = 0;
        while (i < len && n[i] && lc(s[i]) == n[i]) i++;
        if (i == len && !n[i]) return t;
    }
    return T_UNK;
}

static int tag_is_void(int t) {
    return t == T_BR || t == T_HR || t == T_IMG || t == T_INPUT ||
           t == T_LINKE || t == T_META || t == T_AREA || t == T_BASE ||
           t == T_COL || t == T_EMBED || t == T_SOURCE || t == T_TRACK ||
           t == T_WBR || t == T_PARAM;
}

/* HTML5-ish implied end tags: opening `nt` auto-closes an open `ot`. */
static int tag_autocloses(int ot, int nt) {
    if (ot == T_P) {
        switch (nt) {
        case T_DIV: case T_P: case T_H1: case T_H2: case T_H3: case T_H4:
        case T_H5: case T_H6: case T_UL: case T_OL: case T_LI: case T_DL:
        case T_DT: case T_DD: case T_TABLE: case T_PRE: case T_BLOCKQUOTE:
        case T_HR: case T_FORM: case T_HEADER: case T_FOOTER: case T_NAV:
        case T_MAIN: case T_SECTION: case T_ARTICLE: case T_ASIDE:
        case T_FIGURE: case T_FIGCAPTION: case T_ADDRESS: case T_FIELDSET:
        case T_DETAILS:
            return 1;
        }
        return 0;
    }
    if (ot == T_LI && nt == T_LI) return 1;
    if ((ot == T_DT || ot == T_DD) && (nt == T_DT || nt == T_DD)) return 1;
    if ((ot == T_TD || ot == T_TH) &&
        (nt == T_TD || nt == T_TH || nt == T_TR)) return 1;
    if (ot == T_TR && nt == T_TR) return 1;
    if (ot == T_OPTION && nt == T_OPTION) return 1;
    return 0;
}

/* ---- DOM construction ---------------------------------------------- */

static int dom_new(int tag, int parent) {
    if (E->nnodes >= NODE_MAX) return -1;
    int ni = E->nnodes++;
    struct dnode *n = &E->nodes[ni];
    mem_zero(n, sizeof(*n));
    n->parent = parent;
    n->first = n->last = n->next = -1;
    n->tag = (int16_t)tag;
    n->link = n->field = n->img = -1;
    if (parent >= 0) {
        struct dnode *p = &E->nodes[parent];
        if (p->last >= 0) E->nodes[p->last].next = ni;
        else p->first = ni;
        p->last = ni;
    }
    return ni;
}

/* Attribute lookup: value slice in tpool, or NULL. */
static const char *node_attr(const struct dnode *n, const char *name, int *out_len) {
    int nlen = (int)str_len(name);
    for (int i = 0; i < n->nattr; i++) {
        const struct dattr *a = &E->attrs[n->attr0 + i];
        if (a->nlen != nlen) continue;
        int k = 0;
        while (k < nlen && lc(E->tpool[a->noff + k]) == lc(name[k])) k++;
        if (k == nlen) {
            if (out_len) *out_len = a->vlen;
            return &E->tpool[a->voff];
        }
    }
    if (out_len) *out_len = 0;
    return NULL;
}

static int node_attr_str(const struct dnode *n, const char *name, char *out, int cap) {
    int vlen;
    const char *v = node_attr(n, name, &vlen);
    if (!v) { out[0] = 0; return 0; }
    int o = 0;
    while (o < vlen && o < cap - 1) { out[o] = v[o]; o++; }
    out[o] = 0;
    return 1;
}

/* Parse the attributes of a start tag (cursor just past the name);
 * returns the index one past '>'. Names lowercased, values entity-
 * decoded, both stored in the tpool. */
static long parse_attrs(const char *s, long i, long n,
                        int *attr0, int *nattr, int *selfclose) {
    *attr0 = E->nattrs;
    *nattr = 0;
    *selfclose = 0;
    while (i < n && s[i] != '>') {
        if (is_whitespace(s[i])) { i++; continue; }
        if (s[i] == '/') { *selfclose = 1; i++; continue; }
        long k0 = i;
        while (i < n && !is_whitespace(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/')
            i++;
        long klen = i - k0;
        while (i < n && is_whitespace(s[i])) i++;
        int noff = E->tpool_len;
        for (long k = 0; k < klen && k < 48; k++) tp_putc((char)lc(s[k0 + k]));
        int nlen = E->tpool_len - noff;
        int voff = E->tpool_len, vlen = 0;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && is_whitespace(s[i])) i++;
            char q = 0;
            if (i < n && (s[i] == '"' || s[i] == '\'')) { q = s[i]; i++; }
            long v0 = i;
            while (i < n && (q ? s[i] != q
                               : (!is_whitespace(s[i]) && s[i] != '>')))
                i++;
            vlen = tp_put_html(s + v0, i - v0);
            if (q && i < n && s[i] == q) i++;
        }
        if (klen > 0 && nlen > 0 && E->nattrs < ATTR_MAX) {
            struct dattr *a = &E->attrs[E->nattrs++];
            a->noff = noff; a->nlen = (int16_t)nlen;
            a->voff = voff; a->vlen = vlen;
            (*nattr)++;
        }
    }
    if (i < n) i++;                       /* consume '>' */
    return i;
}

#define DOM_STACK_MAX 96

/* Parse HTML [s, s+n) appending under `root` (the tree-constructor
 * core; also reused for JS innerHTML fragments). */
static void dom_parse(const char *s, long n, int root) {
    int stack[DOM_STACK_MAX];
    int sp = 0;
    stack[0] = root;
    long i = 0;
    while (i < n && E->nnodes < NODE_MAX - 2) {
        if (s[i] == '<' && i + 1 < n &&
            (is_alpha(s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?')) {
            if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                i += 4;                   /* comment */
                while (i + 2 < n && !(s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>'))
                    i++;
                i = (i + 3 <= n) ? i + 3 : n;
                continue;
            }
            if (s[i + 1] == '!' || s[i + 1] == '?') {   /* doctype / PI */
                while (i < n && s[i] != '>') i++;
                if (i < n) i++;
                continue;
            }
            if (s[i + 1] == '/') {        /* end tag */
                long k0 = i + 2, k = k0;
                while (k < n && s[k] != '>' && !is_whitespace(s[k])) k++;
                int t = tag_lookup(s + k0, (int)(k - k0));
                while (k < n && s[k] != '>') k++;
                i = (k < n) ? k + 1 : n;
                for (int d = sp; d >= 1; d--)
                    if (E->nodes[stack[d]].tag == t) { sp = d - 1; break; }
                continue;
            }
            /* start tag */
            long k0 = i + 1, k = k0;
            while (k < n && !is_whitespace(s[k]) && s[k] != '>' && s[k] != '/')
                k++;
            int t = tag_lookup(s + k0, (int)(k - k0));
            int attr0, nattr, selfclose;
            i = parse_attrs(s, k, n, &attr0, &nattr, &selfclose);
            while (sp > 0 && tag_autocloses(E->nodes[stack[sp]].tag, t))
                sp--;
            if (t == T_HTML || t == T_HEAD || t == T_BODY) {
                int seen = 0;
                for (int q = 1; q < E->nnodes; q++)
                    if (E->nodes[q].tag == t) { seen = 1; break; }
                if (seen) continue;       /* one each; re-opens ignored */
            }
            int ni = dom_new(t, stack[sp]);
            if (ni < 0) break;
            E->nodes[ni].attr0 = attr0;
            E->nodes[ni].nattr = (int16_t)nattr;
            if (t == T_BODY && E->body < 0) E->body = ni;
            if (t == T_SCRIPT || t == T_STYLE || t == T_TITLE || t == T_TEXTAREA) {
                /* raw-text element: capture verbatim to the end tag */
                const char *close = g_tag_names[t];
                int clen = (int)str_len(close);
                long j = i;
                while (j < n) {
                    if (s[j] == '<' && j + 1 < n && s[j + 1] == '/' &&
                        j + 2 + clen <= n &&
                        str_ncasecmp(s + j + 2, close, clen) == 0)
                        break;
                    j++;
                }
                if (j > i) {
                    int toff = E->tpool_len, tl;
                    if (t == T_STYLE || t == T_SCRIPT) {   /* raw: no entities */
                        for (long q2 = i; q2 < j && E->tpool_len < TPOOL_CAP - 1; q2++)
                            if (s[q2] != '\r') tp_putc(s[q2]);
                        tl = E->tpool_len - toff;
                    } else {
                        tl = tp_put_html(s + i, j - i);
                    }
                    if (tl > 0) {
                        int tn = dom_new(T_TEXT, ni);
                        if (tn >= 0) {
                            E->nodes[tn].toff = toff;
                            E->nodes[tn].tlen = tl;
                        }
                    }
                }
                while (j < n && s[j] != '>') j++;
                i = (j < n) ? j + 1 : n;
                continue;
            }
            if (!tag_is_void(t) && !selfclose && sp < DOM_STACK_MAX - 1)
                stack[++sp] = ni;
            continue;
        }
        /* text run up to the next real tag opener */
        long t0 = i;
        while (i < n) {
            if (s[i] == '<' && i + 1 < n &&
                (is_alpha(s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?'))
                break;
            i++;
        }
        int toff = E->tpool_len;
        int tl = tp_put_html(s + t0, i - t0);
        if (tl > 0) {
            int parent = stack[sp];
            struct dnode *p = &E->nodes[parent];
            if (p->last >= 0 && E->nodes[p->last].tag == T_TEXT &&
                E->nodes[p->last].toff + E->nodes[p->last].tlen == toff) {
                E->nodes[p->last].tlen += tl;   /* coalesce */
            } else {
                int tn = dom_new(T_TEXT, parent);
                if (tn >= 0) {
                    E->nodes[tn].toff = toff;
                    E->nodes[tn].tlen = tl;
                }
            }
        }
    }
}

/* g_raw -> DOM tree in E. */
static void dom_build(void) {
    int root = dom_new(T_UNK, -1);        /* document node */
    dom_parse(g_raw, g_raw_len, root);
}

/* =================== CSS parser =================== */

static int cssp_putc(char c) {
    if (E->csspool_len < CSSPOOL_CAP - 1) {
        E->csspool[E->csspool_len++] = c;
        return 1;
    }
    return 0;
}

struct ccur { const char *s; long i, n; };

static void css_ws(struct ccur *c) {
    for (;;) {
        while (c->i < c->n && is_whitespace(c->s[c->i])) c->i++;
        if (c->i + 1 < c->n && c->s[c->i] == '/' && c->s[c->i + 1] == '*') {
            c->i += 2;
            while (c->i + 1 < c->n && !(c->s[c->i] == '*' && c->s[c->i + 1] == '/'))
                c->i++;
            c->i = (c->i + 2 <= c->n) ? c->i + 2 : c->n;
            continue;
        }
        break;
    }
}

/* Skip an unsupported at-rule: to ';' or over a balanced {...} block. */
static void css_skip_block_or_semi(struct ccur *c) {
    while (c->i < c->n) {
        char ch = c->s[c->i];
        if (ch == ';') { c->i++; return; }
        if (ch == '{') {
            int depth = 0;
            while (c->i < c->n) {
                if (c->s[c->i] == '{') depth++;
                else if (c->s[c->i] == '}') {
                    depth--;
                    if (!depth) { c->i++; return; }
                }
                c->i++;
            }
            return;
        }
        c->i++;
    }
}

/* Evaluate a media query list against the viewport ("screen and
 * (min-width: 600px)", comma = OR). Unknown features fail their query. */
static int media_matches(const char *s, int len) {
    int allws = 1;
    for (int k = 0; k < len; k++)
        if (!is_whitespace(s[k])) { allws = 0; break; }
    if (allws) return 1;
    int i = 0;
    while (i < len) {
        int ok = 1, neg = 0;
        while (i < len && s[i] != ',') {
            if (is_whitespace(s[i])) { i++; continue; }
            if (s[i] == '(') {
                int f0 = ++i;
                while (i < len && s[i] != ':' && s[i] != ')') i++;
                int flen = i - f0;
                while (flen > 0 && is_whitespace(s[f0 + flen - 1])) flen--;
                int vv0 = 0, vvn = 0;
                if (i < len && s[i] == ':') {
                    i++;
                    while (i < len && is_whitespace(s[i])) i++;
                    vv0 = i;
                    while (i < len && s[i] != ')') i++;
                    vvn = i - vv0;
                }
                if (i < len) i++;         /* ')' */
                int px = 0;
                for (int k = vv0; k < vv0 + vvn && s[k] >= '0' && s[k] <= '9'; k++)
                    px = px * 10 + (s[k] - '0');
                int val;
                if (flen == 9 && str_ncasecmp(s + f0, "min-width", 9) == 0)
                    val = g_win_w >= px;
                else if (flen == 9 && str_ncasecmp(s + f0, "max-width", 9) == 0)
                    val = g_win_w <= px;
                else if (flen == 5 && str_ncasecmp(s + f0, "width", 5) == 0)
                    val = g_win_w == px;
                else if (flen == 11 && str_ncasecmp(s + f0, "orientation", 11) == 0)
                    val = (vvn > 0 && lc(s[vv0]) == 'l') ? (g_win_w >= g_win_h)
                                                         : (g_win_w < g_win_h);
                else if (flen == 20 && str_ncasecmp(s + f0, "prefers-color-scheme", 20) == 0)
                    val = vvn > 0 && lc(s[vv0]) == 'l';
                else
                    val = 0;
                if (!val) ok = 0;
                continue;
            }
            int w0 = i;
            while (i < len && !is_whitespace(s[i]) && s[i] != ',' && s[i] != '(')
                i++;
            int wl = i - w0;
            if (wl == 3 && str_ncasecmp(s + w0, "not", 3) == 0) neg = 1;
            else if (wl == 4 && str_ncasecmp(s + w0, "only", 4) == 0) { }
            else if (wl == 3 && str_ncasecmp(s + w0, "and", 3) == 0) { }
            else if (wl == 6 && str_ncasecmp(s + w0, "screen", 6) == 0) { }
            else if (wl == 3 && str_ncasecmp(s + w0, "all", 3) == 0) { }
            else if (wl > 0) ok = 0;      /* print, speech, unknown */
        }
        if (neg) ok = !ok;
        if (ok) return 1;
        if (i < len && s[i] == ',') i++;
    }
    return 0;
}

/* CSS identifier -> lowercased copy in csspool. */
static int css_ident(struct ccur *c, int *off, int *outlen) {
    int o = E->csspool_len, l = 0;
    while (c->i < c->n) {
        char ch = c->s[c->i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            (unsigned char)ch >= 0x80) {
            cssp_putc((char)lc(ch));
            l++;
            c->i++;
        } else break;
    }
    *off = o;
    *outlen = l;
    return l > 0;
}

/* ---- Value tokens -------------------------------------------------- */

struct vtok { const char *s; int len; };

static int vtok_next(const char *v, int n, int *pos, struct vtok *t) {
    int i = *pos;
    while (i < n && (is_whitespace(v[i]) || v[i] == ',' || v[i] == '/')) i++;
    if (i >= n) return 0;
    int s0 = i, paren = 0;
    while (i < n) {
        char c = v[i];
        if (paren) {
            if (c == '(') paren++;
            else if (c == ')') { paren--; i++; if (!paren) break; continue; }
            i++;
            continue;
        }
        if (c == '(') { paren = 1; i++; continue; }
        if (is_whitespace(c) || c == ',' || c == '/') break;
        i++;
    }
    t->s = v + s0;
    t->len = i - s0;
    *pos = i;
    return 1;
}

static int tok_is(const struct vtok *t, const char *kw) {
    int l = (int)str_len(kw);
    if (t->len != l) return 0;
    for (int i = 0; i < l; i++)
        if (lc(t->s[i]) != lc(kw[i])) return 0;
    return 1;
}

static int hexv(char c) { int d = hex_digit(c); return d < 0 ? 0 : d; }

/* One color token -> ARGB. */
static int css_color_tok(const char *s, int len, uint32_t *out) {
    if (len >= 4 && s[0] == '#') {
        if (len == 4) {
            int r = hexv(s[1]), g = hexv(s[2]), b = hexv(s[3]);
            *out = 0xFF000000u | (uint32_t)(r * 17) << 16 |
                   (uint32_t)(g * 17) << 8 | (uint32_t)(b * 17);
            return 1;
        }
        if (len == 7 || len == 9) {
            uint32_t v = 0;
            for (int i = 1; i < 7; i++) v = v * 16 + (uint32_t)hexv(s[i]);
            uint32_t a = 255;
            if (len == 9) a = (uint32_t)(hexv(s[7]) * 16 + hexv(s[8]));
            *out = (a << 24) | v;
            return 1;
        }
        return 0;
    }
    if (len > 4 && str_ncasecmp(s, "rgb", 3) == 0) {
        int vals[4] = { 0, 0, 0, 255 };
        int nv = 0, i = 0;
        while (i < len && s[i] != '(') i++;
        i++;
        while (i < len && nv < 4) {
            while (i < len && !((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '-'))
                i++;
            if (i >= len || s[i] == ')') break;
            int v = 0, neg = 0;
            if (s[i] == '-') { neg = 1; i++; }
            while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
            if (i < len && s[i] == '.') {
                i++;
                int f = 0, fd = 0;
                while (i < len && s[i] >= '0' && s[i] <= '9') {
                    if (fd < 2) { f = f * 10 + (s[i] - '0'); fd++; }
                    i++;
                }
                if (nv == 3) v = v * 255 + (fd == 1 ? f * 255 / 10 : f * 255 / 100);
            }
            if (i < len && s[i] == '%') { v = v * 255 / 100; i++; }
            if (neg) v = 0;
            if (v > 255) v = 255;
            vals[nv++] = v;
        }
        *out = ((uint32_t)vals[3] << 24) | ((uint32_t)vals[0] << 16) |
               ((uint32_t)vals[1] << 8) | (uint32_t)vals[2];
        return 1;
    }
    {
        static const struct { const char *n; uint32_t c; } NC[] = {
            {"black",0xFF000000},{"white",0xFFFFFFFF},{"red",0xFFFF0000},
            {"green",0xFF008000},{"blue",0xFF0000FF},{"yellow",0xFFFFFF00},
            {"orange",0xFFFFA500},{"purple",0xFF800080},{"gray",0xFF808080},
            {"grey",0xFF808080},{"silver",0xFFC0C0C0},{"maroon",0xFF800000},
            {"navy",0xFF000080},{"teal",0xFF008080},{"aqua",0xFF00FFFF},
            {"cyan",0xFF00FFFF},{"fuchsia",0xFFFF00FF},{"magenta",0xFFFF00FF},
            {"lime",0xFF00FF00},{"olive",0xFF808000},{"brown",0xFFA52A2A},
            {"pink",0xFFFFC0CB},{"gold",0xFFFFD700},{"tan",0xFFD2B48C},
            {"beige",0xFFF5F5DC},{"ivory",0xFFFFFFF0},{"snow",0xFFFFFAFA},
            {"whitesmoke",0xFFF5F5F5},{"lightgray",0xFFD3D3D3},
            {"lightgrey",0xFFD3D3D3},{"darkgray",0xFFA9A9A9},
            {"darkgrey",0xFFA9A9A9},{"dimgray",0xFF696969},
            {"lightblue",0xFFADD8E6},{"lightgreen",0xFF90EE90},
            {"lightyellow",0xFFFFFFE0},{"darkred",0xFF8B0000},
            {"darkblue",0xFF00008B},{"darkgreen",0xFF006400},
            {"steelblue",0xFF4682B4},{"royalblue",0xFF4169E1},
            {"slategray",0xFF708090},{"tomato",0xFFFF6347},
            {"coral",0xFFFF7F50},{"salmon",0xFFFA8072},{"khaki",0xFFF0E68C},
            {"indigo",0xFF4B0082},{"violet",0xFFEE82EE},{"plum",0xFFDDA0DD},
            {"orchid",0xFFDA70D6},{"turquoise",0xFF40E0D0},
            {"crimson",0xFFDC143C},{"chocolate",0xFFD2691E},
            {"rebeccapurple",0xFF663399},{"transparent",0x00000000},
        };
        for (unsigned k = 0; k < sizeof(NC) / sizeof(NC[0]); k++) {
            const char *n = NC[k].n;
            int i = 0;
            while (i < len && n[i] && lc(s[i]) == n[i]) i++;
            if (i == len && !n[i]) { *out = NC[k].c; return 1; }
        }
    }
    return 0;
}

/* Length token -> px. */
#define LK_PX   0
#define LK_PCT  1
#define LK_AUTO 2
#define LK_BAD  3

static int css_len_tok(const char *s, int len, int own_px, int *out) {
    if (len == 4 && str_ncasecmp(s, "auto", 4) == 0) return LK_AUTO;
    int i = 0, neg = 0;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); i++; }
    long whole = 0;
    int digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        whole = whole * 10 + (s[i] - '0');
        i++; digits++;
    }
    long frac = 0, fdiv = 1;
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (fdiv < 1000) { frac = frac * 10 + (s[i] - '0'); fdiv *= 10; }
            i++; digits++;
        }
    }
    if (!digits) return LK_BAD;
    long v1000 = whole * 1000 + (fdiv > 1 ? frac * 1000 / fdiv : 0);
    if (neg) v1000 = -v1000;
    int px;
    if (i >= len) px = (int)(v1000 / 1000);
    else if (s[i] == '%') { *out = (int)(v1000 / 1000); return LK_PCT; }
    else if (len - i == 2 && lc(s[i]) == 'p' && lc(s[i + 1]) == 'x')
        px = (int)(v1000 / 1000);
    else if (len - i == 2 && lc(s[i]) == 'e' && lc(s[i + 1]) == 'm')
        px = (int)(v1000 * own_px / 1000);
    else if (len - i == 3 && str_ncasecmp(s + i, "rem", 3) == 0)
        px = (int)(v1000 * PX_BODY / 1000);
    else if (len - i == 2 && lc(s[i]) == 'p' && lc(s[i + 1]) == 't')
        px = (int)(v1000 * 4 / 3000);
    else if (len - i == 2 && lc(s[i]) == 'c' && lc(s[i + 1]) == 'h')
        px = (int)(v1000 * (own_px / 2) / 1000);
    else if (len - i == 2 && lc(s[i]) == 'v' && lc(s[i + 1]) == 'w')
        px = (int)(v1000 * g_win_w / 100000);
    else if (len - i == 2 && lc(s[i]) == 'v' && lc(s[i + 1]) == 'h')
        px = (int)(v1000 * g_win_h / 100000);
    else return LK_BAD;
    *out = px;
    return LK_PX;
}

static int prop_lookup(const char *s, int len) {
    static const struct { const char *n; uint8_t id; } P[] = {
        {"display",CP_DISPLAY},{"color",CP_COLOR},
        {"background-color",CP_BGCOLOR},{"background",CP_BG},
        {"font-size",CP_FONT_SIZE},{"font-weight",CP_FONT_WEIGHT},
        {"font-family",CP_FONT_FAMILY},{"font-style",CP_FONT_STYLE},
        {"font",CP_FONT},
        {"text-align",CP_TALIGN},{"text-decoration",CP_TDECOR},
        {"text-decoration-line",CP_TDECOR},
        {"line-height",CP_LINEH},{"white-space",CP_WSPACE},
        {"list-style",CP_LSTYLE},{"list-style-type",CP_LSTYLE_TYPE},
        {"margin",CP_MARGIN},{"margin-top",CP_MT},{"margin-right",CP_MR},
        {"margin-bottom",CP_MB},{"margin-left",CP_ML},
        {"padding",CP_PADDING},{"padding-top",CP_PT},{"padding-right",CP_PR},
        {"padding-bottom",CP_PB},{"padding-left",CP_PL},
        {"border",CP_BORDER},{"border-top",CP_BTOP},
        {"border-right",CP_BRIGHT},{"border-bottom",CP_BBOTTOM},
        {"border-left",CP_BLEFT},{"border-width",CP_BWIDTH},
        {"border-color",CP_BCOLOR},
        {"width",CP_WIDTH},{"height",CP_HEIGHT},{"max-width",CP_MAXW},
        {"visibility",CP_VISIBILITY},
        {"float",CP_FLOAT},{"clear",CP_CLEAR},
        {"vertical-align",CP_VALIGN},{"border-collapse",CP_BCOLLAPSE},
    };
    for (unsigned k = 0; k < sizeof(P) / sizeof(P[0]); k++) {
        const char *n = P[k].n;
        int i = 0;
        while (i < len && n[i] && s[i] == n[i]) i++;
        if (i == len && !n[i]) return P[k].id;
    }
    return -1;
}

/* Parse declarations "a: b; c: d" until '}' (consumed) or EOF. */
static void css_parse_decls(struct ccur *c) {
    for (;;) {
        css_ws(c);
        if (c->i >= c->n) break;
        if (c->s[c->i] == '}') { c->i++; break; }
        if (c->s[c->i] == ';') { c->i++; continue; }
        int po, pl;
        if (!css_ident(c, &po, &pl)) {
            while (c->i < c->n && c->s[c->i] != ';' && c->s[c->i] != '}') c->i++;
            continue;
        }
        E->csspool_len = po;              /* name is transient */
        int prop = prop_lookup(&E->csspool[po], pl);
        css_ws(c);
        if (c->i >= c->n || c->s[c->i] != ':') {
            while (c->i < c->n && c->s[c->i] != ';' && c->s[c->i] != '}') c->i++;
            continue;
        }
        c->i++;
        css_ws(c);
        int vo = E->csspool_len, vl = 0, paren = 0;
        while (c->i < c->n) {
            char ch2 = c->s[c->i];
            if (!paren && (ch2 == ';' || ch2 == '}')) break;
            if (ch2 == '(') paren++;
            if (ch2 == ')' && paren) paren--;
            if (ch2 == '/' && c->i + 1 < c->n && c->s[c->i + 1] == '*') {
                c->i += 2;
                while (c->i + 1 < c->n && !(c->s[c->i] == '*' && c->s[c->i + 1] == '/'))
                    c->i++;
                c->i = (c->i + 2 <= c->n) ? c->i + 2 : c->n;
                continue;
            }
            if (cssp_putc(ch2)) vl++;
            c->i++;
        }
        int imp = 0;
        while (vl > 0 && is_whitespace(E->csspool[vo + vl - 1])) vl--;
        for (int k = vl - 1; k >= 0; k--) {
            if (E->csspool[vo + k] == '!') {
                int q = k + 1;
                while (q < vl && is_whitespace(E->csspool[vo + q])) q++;
                if (vl - q == 9 &&
                    str_ncasecmp(&E->csspool[vo + q], "important", 9) == 0) {
                    imp = 1;
                    vl = k;
                    while (vl > 0 && is_whitespace(E->csspool[vo + vl - 1])) vl--;
                }
                break;
            }
        }
        E->csspool_len = vo + vl;
        if (prop >= 0 && vl > 0 && E->ndecls < DECL_MAX) {
            struct cdecl *d = &E->decls[E->ndecls++];
            d->prop = (uint8_t)prop;
            d->imp = (uint8_t)imp;
            d->voff = vo;
            d->vlen = (int16_t)(vl > 32767 ? 32767 : vl);
        }
        if (c->i < c->n && c->s[c->i] == ';') c->i++;
    }
}

/* One compound selector part. Returns 1 ok, 0 unsupported/invalid. */
static int css_parse_part(struct ccur *c, struct cpart *p,
                          int *spec_id, int *spec_cls, int *spec_type) {
    p->tag = -1;
    p->nclass = 0;
    p->pseudo_link = 0;
    p->has_attr = 0;
    p->id_len = 0;
    p->an_len = 0;
    p->av_len = 0;
    int got = 0;
    for (;;) {
        if (c->i >= c->n) break;
        char ch = c->s[c->i];
        if (ch == '*') { c->i++; got = 1; continue; }
        if (ch == '.') {
            c->i++;
            int off, l;
            if (!css_ident(c, &off, &l)) return 0;
            if (p->nclass < 2) {
                p->cls_off[p->nclass] = off;
                p->cls_len[p->nclass] = (int16_t)l;
                p->nclass++;
            }
            (*spec_cls)++;
            got = 1;
            continue;
        }
        if (ch == '#') {
            c->i++;
            int off, l;
            if (!css_ident(c, &off, &l)) return 0;
            p->id_off = off;
            p->id_len = (int16_t)l;
            (*spec_id)++;
            got = 1;
            continue;
        }
        if (ch == '[') {
            c->i++;
            css_ws(c);
            int off, l;
            if (!css_ident(c, &off, &l)) return 0;
            p->an_off = off;
            p->an_len = (int16_t)l;
            css_ws(c);
            if (c->i < c->n && c->s[c->i] == '=') {
                c->i++;
                css_ws(c);
                char q = 0;
                if (c->i < c->n && (c->s[c->i] == '"' || c->s[c->i] == '\'')) {
                    q = c->s[c->i];
                    c->i++;
                }
                int vo = E->csspool_len, vl2 = 0;
                while (c->i < c->n &&
                       (q ? c->s[c->i] != q
                          : (c->s[c->i] != ']' && !is_whitespace(c->s[c->i])))) {
                    cssp_putc((char)lc(c->s[c->i]));
                    vl2++;
                    c->i++;
                }
                if (q && c->i < c->n) c->i++;
                p->av_off = vo;
                p->av_len = (int16_t)vl2;
            } else if (c->i < c->n &&
                       (c->s[c->i] == '~' || c->s[c->i] == '|' ||
                        c->s[c->i] == '^' || c->s[c->i] == '$' ||
                        c->s[c->i] == '*')) {
                return 0;                 /* fancy attr matchers unsupported */
            }
            css_ws(c);
            if (c->i >= c->n || c->s[c->i] != ']') return 0;
            c->i++;
            p->has_attr = 1;
            (*spec_cls)++;
            got = 1;
            continue;
        }
        if (ch == ':') {
            c->i++;
            if (c->i < c->n && c->s[c->i] == ':') return 0;   /* ::pseudo-elem */
            int off, l;
            if (!css_ident(c, &off, &l)) return 0;
            E->csspool_len = off;         /* transient */
            const char *pp = &E->csspool[off];
            if ((l == 4 && str_ncasecmp(pp, "link", 4) == 0) ||
                (l == 7 && str_ncasecmp(pp, "visited", 7) == 0)) {
                p->pseudo_link = 1;
                (*spec_cls)++;
                got = 1;
                continue;
            }
            return 0;                     /* :hover, :not(...), etc. */
        }
        if (is_alpha(ch)) {
            long t0 = c->i;
            while (c->i < c->n) {
                char cc = c->s[c->i];
                if (is_alpha(cc) || (cc >= '0' && cc <= '9') || cc == '-' || cc == '_')
                    c->i++;
                else break;
            }
            p->tag = (int16_t)tag_lookup(c->s + t0, (int)(c->i - t0));
            if (p->tag == T_UNK) return 0;    /* unknown element: no match */
            (*spec_type)++;
            got = 1;
            continue;
        }
        break;
    }
    return got;
}

/* Parse "sel1, sel2 { decls }". Decls are shared by every valid
 * selector. SEL_GROUP_MAX must fit real-world grouped selectors: the
 * UA sheet's block-display group alone has 34 members, and at the old
 * cap of 16 everything past the 16th (blockquote, form, header, nav,
 * center...) silently rendered INLINE -- on HN, whose whole page sits
 * inside <center>, that degraded the table grid to inline flow. */
#define SEL_GROUP_MAX 64
static void css_parse_ruleset(struct ccur *c, int origin) {
    int sel_part0[SEL_GROUP_MAX], sel_nparts[SEL_GROUP_MAX];
    uint16_t sel_spec[SEL_GROUP_MAX];
    int nsel = 0;
    int cur_valid = 1;
    int part0 = E->nparts;
    int spec_id = 0, spec_cls = 0, spec_type = 0;
    int pending_comb = 0;
    for (;;) {
        css_ws(c);
        if (c->i >= c->n) return;
        char ch = c->s[c->i];
        if (ch == '{' || ch == ',') {
            if (cur_valid && E->nparts > part0 &&
                E->nparts - part0 <= 12 && nsel < SEL_GROUP_MAX) {
                int spec = spec_id * 100 + spec_cls * 10 + spec_type;
                if (spec > 0xFFFF) spec = 0xFFFF;
                sel_part0[nsel] = part0;
                sel_nparts[nsel] = E->nparts - part0;
                sel_spec[nsel] = (uint16_t)spec;
                nsel++;
            } else {
                E->nparts = part0;        /* roll back an invalid selector */
            }
            part0 = E->nparts;
            spec_id = spec_cls = spec_type = 0;
            cur_valid = 1;
            pending_comb = 0;
            c->i++;
            if (ch == '{') break;
            continue;
        }
        if (ch == '>') { pending_comb = 2; c->i++; continue; }
        if (ch == '+' || ch == '~') { cur_valid = 0; c->i++; continue; }
        if (!cur_valid) { c->i++; continue; }
        struct cpart tmp;
        long before = c->i;
        int ok = css_parse_part(c, &tmp, &spec_id, &spec_cls, &spec_type);
        if (!ok || c->i == before) {
            cur_valid = 0;
            if (c->i == before) c->i++;
            continue;
        }
        tmp.comb = (uint8_t)((E->nparts == part0) ? 0
                             : (pending_comb ? pending_comb : 1));
        pending_comb = 0;
        if (E->nparts < PART_MAX) E->parts[E->nparts++] = tmp;
        else cur_valid = 0;
    }
    int decl0 = E->ndecls;
    css_parse_decls(c);
    int ndecl = E->ndecls - decl0;
    for (int k = 0; k < nsel && ndecl > 0; k++) {
        if (E->nrules >= RULE_MAX) break;
        struct crule *r = &E->rules[E->nrules++];
        r->part0 = sel_part0[k];
        r->nparts = (int16_t)sel_nparts[k];
        r->decl0 = decl0;
        r->ndecl = (int16_t)ndecl;
        r->spec = sel_spec[k];
        r->origin = (int16_t)origin;
        r->order = E->css_order++;
    }
}

/* Parse a whole stylesheet into E's rule pools. */
static void css_parse_sheet(const char *s, long n, int origin) {
    struct ccur c = { s, 0, n };
    while (c.i < c.n) {
        css_ws(&c);
        if (c.i >= c.n) break;
        if (c.s[c.i] == '@') {
            c.i++;
            int o, l;
            css_ident(&c, &o, &l);
            E->csspool_len = o;
            if (l == 5 && str_ncasecmp(&E->csspool[o], "media", 5) == 0) {
                long m0 = c.i;
                while (c.i < c.n && c.s[c.i] != '{' && c.s[c.i] != ';') c.i++;
                if (c.i >= c.n || c.s[c.i] == ';') {
                    if (c.i < c.n) c.i++;
                    continue;
                }
                int cond = media_matches(c.s + m0, (int)(c.i - m0));
                c.i++;
                long b0 = c.i;
                int depth = 1;
                while (c.i < c.n && depth) {
                    if (c.s[c.i] == '{') depth++;
                    else if (c.s[c.i] == '}') depth--;
                    c.i++;
                }
                if (cond) {
                    long b1 = depth ? c.i : c.i - 1;
                    css_parse_sheet(c.s + b0, b1 - b0, origin);
                }
                continue;
            }
            css_skip_block_or_semi(&c);
            continue;
        }
        if (c.s[c.i] == '}') { c.i++; continue; }
        css_parse_ruleset(&c, origin);
    }
}

/* =================== Selector matching + cascade =================== */

static int class_attr_contains(const struct dnode *nd, const char *cls, int clen) {
    int vlen;
    const char *v = node_attr(nd, "class", &vlen);
    if (!v) return 0;
    int i = 0;
    while (i < vlen) {
        while (i < vlen && is_whitespace(v[i])) i++;
        int w0 = i;
        while (i < vlen && !is_whitespace(v[i])) i++;
        if (i - w0 == clen) {
            int k = 0;
            while (k < clen && lc(v[w0 + k]) == cls[k]) k++;
            if (k == clen) return 1;
        }
    }
    return 0;
}

static int part_match(const struct cpart *p, int ni) {
    const struct dnode *nd = &E->nodes[ni];
    if (nd->tag == T_TEXT) return 0;
    if (p->tag >= 0 && p->tag != nd->tag) return 0;
    if (p->pseudo_link && nd->link < 0) return 0;
    if (p->id_len > 0) {
        int vlen;
        const char *v = node_attr(nd, "id", &vlen);
        if (!v || vlen != p->id_len) return 0;
        for (int k = 0; k < vlen; k++)
            if (lc(v[k]) != E->csspool[p->id_off + k]) return 0;
    }
    for (int c = 0; c < p->nclass; c++)
        if (!class_attr_contains(nd, &E->csspool[p->cls_off[c]], p->cls_len[c]))
            return 0;
    if (p->has_attr) {
        char an[64];
        int l = p->an_len < 63 ? p->an_len : 63;
        for (int k = 0; k < l; k++) an[k] = E->csspool[p->an_off + k];
        an[l] = 0;
        int vlen;
        const char *v = node_attr(nd, an, &vlen);
        if (!v) return 0;
        if (p->av_len > 0) {
            if (vlen != p->av_len) return 0;
            for (int k = 0; k < vlen; k++)
                if (lc(v[k]) != E->csspool[p->av_off + k]) return 0;
        }
    }
    return 1;
}

/* Right-to-left matching with ancestor backtracking. */
static int match_upward(const struct cpart *parts, int pi, int ni) {
    if (!part_match(&parts[pi], ni)) return 0;
    if (pi == 0) return 1;
    int comb = parts[pi].comb;
    int a = E->nodes[ni].parent;
    if (comb == 2)
        return a >= 0 && match_upward(parts, pi - 1, a);
    while (a >= 0) {
        if (match_upward(parts, pi - 1, a)) return 1;
        a = E->nodes[a].parent;
    }
    return 0;
}

static int sel_match_rule(const struct crule *ru, int ni) {
    if (ru->nparts <= 0) return 0;
    const struct cpart *parts = &E->parts[ru->part0];
    const struct cpart *last = &parts[ru->nparts - 1];
    if (last->tag >= 0 && last->tag != E->nodes[ni].tag) return 0;
    return match_upward(parts, ru->nparts - 1, ni);
}

/* ---- Computed-style defaults + inheritance ------------------------ */

static void st_init(struct cstyle *st, const struct cstyle *pst) {
    if (pst) {
        st->color = pst->color;
        st->px = pst->px;
        st->talign = pst->talign;
        st->line_h = pst->line_h;
        st->fl = pst->fl & (SF_BOLD | SF_MONO | SF_PRE | SF_UNDER | SF_NOBULLET);
    } else {
        st->color = 0xFF1B1B1B;
        st->px = PX_BODY;
        st->talign = 0;
        st->line_h = 0;
        st->fl = 0;
    }
    st->bg = 0;
    st->border_col = 0xFF9A9A9E;
    st->width = -1;
    st->height = -1;
    st->max_w = -1;
    for (int i = 0; i < 4; i++) { st->m[i] = 0; st->p[i] = 0; st->bw[i] = 0; }
    st->disp = D_INLINE;
    st->flt = 0;
    st->clr = 0;
    st->valign = 0;
}

static int clamp_px(int v) {
    if (v < 8) return 8;
    if (v > 40) return 40;
    return v;
}
static int16_t clamp_m(int v) {
    if (v < -2000) v = -2000;
    if (v > 2000) v = 2000;
    return (int16_t)v;
}

/* margin/padding shorthand expansion into T R B L. */
static void expand_sides(int16_t out[4], const int16_t *vals, int nv) {
    if (nv == 1) { out[0] = out[1] = out[2] = out[3] = vals[0]; }
    else if (nv == 2) { out[0] = out[2] = vals[0]; out[1] = out[3] = vals[1]; }
    else if (nv == 3) { out[0] = vals[0]; out[1] = out[3] = vals[1]; out[2] = vals[2]; }
    else if (nv >= 4) { out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[3]; }
}

/* border[-side] shorthand: <width> <style> <color> in any order. */
static void apply_border_shorthand(struct cstyle *st, const char *v, int n,
                                   int side /* -1 = all */) {
    int pos = 0, wpx = -1, saw_style = 0, none = 0;
    struct vtok t;
    uint32_t col;
    while (vtok_next(v, n, &pos, &t)) {
        int px;
        if (tok_is(&t, "none") || tok_is(&t, "hidden")) { none = 1; continue; }
        if (tok_is(&t, "solid") || tok_is(&t, "dashed") || tok_is(&t, "dotted") ||
            tok_is(&t, "double") || tok_is(&t, "groove") || tok_is(&t, "ridge") ||
            tok_is(&t, "inset") || tok_is(&t, "outset")) { saw_style = 1; continue; }
        if (tok_is(&t, "thin")) { wpx = 1; continue; }
        if (tok_is(&t, "medium")) { wpx = 3; continue; }
        if (tok_is(&t, "thick")) { wpx = 5; continue; }
        if (css_color_tok(t.s, t.len, &col)) {
            if (col >> 24) st->border_col = col;
            continue;
        }
        if (css_len_tok(t.s, t.len, st->px, &px) == LK_PX)
            wpx = px < 0 ? 0 : (px > 16 ? 16 : px);
    }
    int w = none ? 0 : (wpx >= 0 ? wpx : (saw_style ? 1 : -1));
    if (w < 0) return;
    if (side < 0)
        for (int i = 0; i < 4; i++) st->bw[i] = (uint8_t)w;
    else
        st->bw[side] = (uint8_t)w;
}

/* Apply one declaration to a computed style. Pass 0 applies only
 * font-size (so em elsewhere resolves against the final font); pass 1
 * applies everything else. */
static void st_apply(struct cstyle *st, const struct cstyle *pst,
                     const struct cdecl *d, int pass) {
    const char *v = &E->csspool[d->voff];
    int n = d->vlen;
    struct vtok t;
    int pos = 0;

    int is_fs = (d->prop == CP_FONT_SIZE);
    if (pass == 0 && !is_fs && d->prop != CP_FONT) return;
    if (pass == 1 && is_fs) return;

    switch (d->prop) {
    case CP_FONT_SIZE:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            if (tok_is(&t, "xx-small")) st->px = 9;
            else if (tok_is(&t, "x-small")) st->px = 10;
            else if (tok_is(&t, "small")) st->px = 13;
            else if (tok_is(&t, "medium")) st->px = 15;
            else if (tok_is(&t, "large")) st->px = 18;
            else if (tok_is(&t, "x-large")) st->px = 24;
            else if (tok_is(&t, "xx-large")) st->px = 32;
            else if (tok_is(&t, "smaller")) st->px = (uint8_t)clamp_px(pst->px * 5 / 6);
            else if (tok_is(&t, "larger")) st->px = (uint8_t)clamp_px(pst->px * 6 / 5);
            else {
                int k = css_len_tok(t.s, t.len, pst->px, &px);
                if (k == LK_PX && px > 0) st->px = (uint8_t)clamp_px(px);
                else if (k == LK_PCT && px > 0)
                    st->px = (uint8_t)clamp_px(pst->px * px / 100);
            }
        }
        break;
    case CP_FONT:
        /* shorthand: pass 0 takes the size token, pass 1 bold + family */
        while (vtok_next(v, n, &pos, &t)) {
            if (pass == 0) {
                int px;
                if (css_len_tok(t.s, t.len, pst->px, &px) == LK_PX && px >= 6 &&
                    !tok_is(&t, "bold") && t.len > 2)
                    st->px = (uint8_t)clamp_px(px);
            } else {
                if (tok_is(&t, "bold")) st->fl |= SF_BOLD;
                if (str_contains(t.s, t.len, "mono", 4) >= 0 ||
                    str_contains(t.s, t.len, "courier", 7) >= 0)
                    st->fl |= SF_MONO;
            }
        }
        break;
    case CP_COLOR: {
        uint32_t c;
        if (vtok_next(v, n, &pos, &t) && css_color_tok(t.s, t.len, &c) &&
            (c >> 24) >= 128)
            st->color = c | 0xFF000000u;
        break;
    }
    case CP_BGCOLOR:
    case CP_BG: {
        uint32_t c;
        while (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "none")) { st->bg = 0; break; }
            if (css_color_tok(t.s, t.len, &c)) {
                st->bg = ((c >> 24) >= 128) ? (c | 0xFF000000u) : 0;
                break;
            }
        }
        break;
    }
    case CP_FONT_WEIGHT:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "bold") || tok_is(&t, "bolder")) st->fl |= SF_BOLD;
            else if (tok_is(&t, "normal") || tok_is(&t, "lighter"))
                st->fl &= (uint8_t)~SF_BOLD;
            else {
                int w = atoi_simple(t.s);
                if (w >= 600) st->fl |= SF_BOLD;
                else if (w > 0) st->fl &= (uint8_t)~SF_BOLD;
            }
        }
        break;
    case CP_FONT_FAMILY:
        if (str_contains(v, n, "mono", 4) >= 0 ||
            str_contains(v, n, "courier", 7) >= 0 ||
            str_contains(v, n, "consol", 6) >= 0)
            st->fl |= SF_MONO;
        else
            st->fl &= (uint8_t)~SF_MONO;
        break;
    case CP_FONT_STYLE:
        break;                            /* no italic face yet */
    case CP_TALIGN:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "center")) st->talign = 1;
            else if (tok_is(&t, "right")) st->talign = 2;
            else st->talign = 0;
        }
        break;
    case CP_TDECOR:
        if (str_contains(v, n, "underline", 9) >= 0) st->fl |= SF_UNDER;
        else if (str_contains(v, n, "none", 4) >= 0)
            st->fl &= (uint8_t)~SF_UNDER;
        break;
    case CP_LINEH:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "normal")) { st->line_h = 0; break; }
            int has_alpha = 0;
            for (int k = 0; k < t.len; k++)
                if (is_alpha(t.s[k])) { has_alpha = 1; break; }
            int px;
            int k2 = css_len_tok(t.s, t.len, st->px, &px);
            if (k2 == LK_PCT) st->line_h = (int16_t)-(px > 300 ? 300 : px);
            else if (k2 == LK_PX && !has_alpha) {
                /* bare number: scale factor (x100) */
                long v1000 = 0;
                int i2 = 0, dg = 0;
                long fr = 0, fdv = 1;
                while (i2 < t.len && t.s[i2] >= '0' && t.s[i2] <= '9') {
                    v1000 = v1000 * 10 + (t.s[i2] - '0');
                    i2++; dg++;
                }
                if (i2 < t.len && t.s[i2] == '.') {
                    i2++;
                    while (i2 < t.len && t.s[i2] >= '0' && t.s[i2] <= '9') {
                        if (fdv < 100) { fr = fr * 10 + (t.s[i2] - '0'); fdv *= 10; }
                        i2++;
                    }
                }
                if (dg || fdv > 1) {
                    int scale = (int)(v1000 * 100 + (fdv > 1 ? fr * 100 / fdv : 0));
                    if (scale > 300) scale = 300;
                    st->line_h = (int16_t)-scale;
                }
            } else if (k2 == LK_PX) {
                if (px > 80) px = 80;
                st->line_h = (int16_t)(px > 0 ? px : 0);
            }
        }
        break;
    case CP_WSPACE:
        if (str_contains(v, n, "pre", 3) >= 0) st->fl |= SF_PRE;
        else st->fl &= (uint8_t)~SF_PRE;
        break;
    case CP_LSTYLE:
    case CP_LSTYLE_TYPE:
        if (str_contains(v, n, "none", 4) >= 0) st->fl |= SF_NOBULLET;
        break;
    case CP_MARGIN: case CP_PADDING: {
        int16_t vals[4];
        int nv = 0;
        while (nv < 4 && vtok_next(v, n, &pos, &t)) {
            int px;
            int k = css_len_tok(t.s, t.len, st->px, &px);
            if (k == LK_AUTO) vals[nv] = (d->prop == CP_MARGIN) ? M_AUTO : 0;
            else if (k == LK_PX) vals[nv] = clamp_m(px);
            else if (k == LK_PCT) vals[nv] = 0;
            else break;
            nv++;
        }
        if (nv > 0)
            expand_sides(d->prop == CP_MARGIN ? st->m : st->p, vals, nv);
        if (d->prop == CP_PADDING)
            for (int k = 0; k < 4; k++)
                if (st->p[k] < 0 || st->p[k] == M_AUTO) st->p[k] = 0;
        break;
    }
    case CP_MT: case CP_MR: case CP_MB: case CP_ML:
    case CP_PT: case CP_PR: case CP_PB: case CP_PL: {
        int is_margin = (d->prop <= CP_ML);
        int side = is_margin ? d->prop - CP_MT : d->prop - CP_PT;
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            int k = css_len_tok(t.s, t.len, st->px, &px);
            int16_t val = 0;
            if (k == LK_AUTO) val = is_margin ? M_AUTO : 0;
            else if (k == LK_PX) val = clamp_m(px);
            else break;
            if (is_margin) st->m[side] = val;
            else st->p[side] = (val < 0 || val == M_AUTO) ? 0 : val;
        }
        break;
    }
    case CP_BORDER:  apply_border_shorthand(st, v, n, -1); break;
    case CP_BTOP:    apply_border_shorthand(st, v, n, 0);  break;
    case CP_BRIGHT:  apply_border_shorthand(st, v, n, 1);  break;
    case CP_BBOTTOM: apply_border_shorthand(st, v, n, 2);  break;
    case CP_BLEFT:   apply_border_shorthand(st, v, n, 3);  break;
    case CP_BWIDTH: {
        int16_t vals[4];
        int nv = 0;
        while (nv < 4 && vtok_next(v, n, &pos, &t)) {
            int px;
            if (tok_is(&t, "thin")) px = 1;
            else if (tok_is(&t, "medium")) px = 3;
            else if (tok_is(&t, "thick")) px = 5;
            else if (css_len_tok(t.s, t.len, st->px, &px) != LK_PX) break;
            vals[nv++] = (int16_t)(px < 0 ? 0 : (px > 16 ? 16 : px));
        }
        if (nv > 0) {
            int16_t out[4];
            expand_sides(out, vals, nv);
            for (int k = 0; k < 4; k++) st->bw[k] = (uint8_t)out[k];
        }
        break;
    }
    case CP_BCOLOR: {
        uint32_t c;
        if (vtok_next(v, n, &pos, &t) && css_color_tok(t.s, t.len, &c) &&
            (c >> 24))
            st->border_col = c | 0xFF000000u;
        break;
    }
    case CP_WIDTH:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            int k = css_len_tok(t.s, t.len, st->px, &px);
            if (k == LK_AUTO) { st->width = -1; st->fl &= (uint8_t)~SF_WPCT; }
            else if (k == LK_PX && px >= 0) { st->width = px; st->fl &= (uint8_t)~SF_WPCT; }
            else if (k == LK_PCT && px > 0) {
                st->width = px > 100 ? 100 : px;
                st->fl |= SF_WPCT;
            }
        }
        break;
    case CP_HEIGHT:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            if (css_len_tok(t.s, t.len, st->px, &px) == LK_PX && px >= 0)
                st->height = px > 4000 ? 4000 : px;
            else st->height = -1;
        }
        break;
    case CP_MAXW:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            int k = css_len_tok(t.s, t.len, st->px, &px);
            if (k == LK_PX && px > 0) st->max_w = px;
            else if (tok_is(&t, "none")) st->max_w = -1;
        }
        break;
    case CP_DISPLAY:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "none")) st->disp = D_NONE;
            else if (tok_is(&t, "inline")) st->disp = D_INLINE;
            else if (tok_is(&t, "inline-block") || tok_is(&t, "inline-flex"))
                st->disp = D_INLBLOCK;
            else if (tok_is(&t, "list-item")) st->disp = D_LISTITEM;
            else if (tok_is(&t, "table") || tok_is(&t, "inline-table"))
                st->disp = D_TABLE;
            else if (tok_is(&t, "table-row-group") ||
                     tok_is(&t, "table-header-group") ||
                     tok_is(&t, "table-footer-group"))
                st->disp = D_TSEC;
            else if (tok_is(&t, "table-row")) st->disp = D_TROW;
            else if (tok_is(&t, "table-cell")) st->disp = D_TCELL;
            else if (tok_is(&t, "table-caption")) st->disp = D_CAPTION;
            else if (tok_is(&t, "contents")) st->disp = D_INLINE;
            else st->disp = D_BLOCK;      /* block, flex, grid... */
        }
        break;
    case CP_VISIBILITY:
        if (str_contains(v, n, "hidden", 6) >= 0 ||
            str_contains(v, n, "collapse", 8) >= 0)
            st->disp = D_NONE;
        break;
    case CP_FLOAT:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "left")) st->flt = 1;
            else if (tok_is(&t, "right")) st->flt = 2;
            else st->flt = 0;
        }
        break;
    case CP_CLEAR:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "left")) st->clr = 1;
            else if (tok_is(&t, "right")) st->clr = 2;
            else if (tok_is(&t, "both")) st->clr = 3;
            else st->clr = 0;
        }
        break;
    case CP_VALIGN:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "middle")) st->valign = 1;
            else if (tok_is(&t, "bottom")) st->valign = 2;
            else st->valign = 0;          /* top / baseline / other */
        }
        break;
    case CP_BCOLLAPSE:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "collapse")) st->fl |= SF_BCOLLAPSE;
            else st->fl &= (uint8_t)~SF_BCOLLAPSE;
        }
        break;
    default:
        break;
    }
}

/* ---- The style pass ------------------------------------------------ */

#define MATCH_MAX 96

struct smatch { uint32_t key; int32_t order; int32_t decl0; int16_t ndecl; };

static void style_node(int ni, const struct cstyle *pst) {
    struct dnode *nd = &E->nodes[ni];
    if (nd->tag == T_TEXT) {
        nd->st = *pst;                    /* text renders in parent style */
        return;
    }
    struct cstyle st;
    st_init(&st, pst);
    /* Tables reset inherited text-align: <center> (and align=center
     * ancestors) center the table BOX in real browsers, not the text
     * inside its cells (HN's whole page sits in <center>). Explicit
     * CSS/attrs on the table or its cells still apply below. */
    if (nd->tag == T_TABLE) st.talign = 0;
    /* presentational hint: legacy align= on images/tables floats them
     * (lowest priority; any CSS float overrides) */
    if (nd->tag == T_IMG || nd->tag == T_TABLE) {
        char al[12];
        if (node_attr_str(nd, "align", al, sizeof(al))) {
            if (lc(al[0]) == 'l') st.flt = 1;
            else if (lc(al[0]) == 'r') st.flt = 2;
        }
    }
    /* legacy table presentational attributes (pre-cascade: CSS wins) */
    int is_cell = (nd->tag == T_TD || nd->tag == T_TH);
    if (is_cell || nd->tag == T_TR || nd->tag == T_TABLE) {
        char a[24];
        uint32_t col;
        if (node_attr_str(nd, "bgcolor", a, sizeof(a)) &&
            css_color_tok(a, (int)str_len(a), &col))
            st.bg = col;
        if ((is_cell || nd->tag == T_TABLE) &&
            node_attr_str(nd, "width", a, sizeof(a))) {
            int v = atoi_simple(a);
            if (v > 0) {
                int l = (int)str_len(a);
                if (a[l - 1] == '%') {
                    st.width = v > 100 ? 100 : v;
                    st.fl |= SF_WPCT;
                } else {
                    st.width = v > 4000 ? 4000 : v;
                }
            }
        }
        if (is_cell && node_attr_str(nd, "height", a, sizeof(a))) {
            int v = atoi_simple(a);
            if (v > 0) st.height = v > 4000 ? 4000 : v;
        }
        if (is_cell && node_attr_str(nd, "align", a, sizeof(a))) {
            if (lc(a[0]) == 'c') st.talign = 1;
            else if (lc(a[0]) == 'r') st.talign = 2;
            else if (lc(a[0]) == 'l') st.talign = 0;
        }
        if ((is_cell || nd->tag == T_TR) &&
            node_attr_str(nd, "valign", a, sizeof(a))) {
            if (lc(a[0]) == 'm' || lc(a[0]) == 'c') st.valign = 1;
            else if (lc(a[0]) == 'b') st.valign = 2;
            else st.valign = 0;
        }
        /* table border= draws 1px borders on every cell */
        if (is_cell) {
            int t2 = nd->parent;
            for (int hop = 0; hop < 6 && t2 >= 0 &&
                              E->nodes[t2].tag != T_TABLE; hop++)
                t2 = E->nodes[t2].parent;
            if (t2 >= 0 && E->nodes[t2].tag == T_TABLE &&
                node_attr_str(&E->nodes[t2], "border", a, sizeof(a)) &&
                atoi_simple(a) > 0)
                for (int s2 = 0; s2 < 4; s2++) st.bw[s2] = 1;
        }
    }
    struct smatch M[MATCH_MAX];
    int nm = 0;
    for (int r = 0; r < E->nrules; r++) {
        struct crule *ru = &E->rules[r];
        if (!sel_match_rule(ru, ni)) continue;
        if (nm >= MATCH_MAX) break;
        uint32_t key = ((uint32_t)ru->origin << 16) | ru->spec;
        int k = nm++;
        while (k > 0 && (M[k - 1].key > key ||
               (M[k - 1].key == key && M[k - 1].order > ru->order))) {
            M[k] = M[k - 1];
            k--;
        }
        M[k].key = key;
        M[k].order = ru->order;
        M[k].decl0 = ru->decl0;
        M[k].ndecl = ru->ndecl;
    }
    /* inline style="" -> transient decls (rolled back afterwards) */
    int inl0 = E->ndecls, cp0 = E->csspool_len;
    int vlen;
    const char *sv = node_attr(nd, "style", &vlen);
    if (sv && vlen > 0) {
        struct ccur c = { sv, 0, vlen };
        css_parse_decls(&c);
    }
    int inlN = E->ndecls - inl0;
    for (int pass = 0; pass < 2; pass++) {
        for (int m = 0; m < nm; m++)
            for (int d2 = 0; d2 < M[m].ndecl; d2++)
                if (!E->decls[M[m].decl0 + d2].imp)
                    st_apply(&st, pst, &E->decls[M[m].decl0 + d2], pass);
        for (int d2 = 0; d2 < inlN; d2++)
            if (!E->decls[inl0 + d2].imp)
                st_apply(&st, pst, &E->decls[inl0 + d2], pass);
        for (int m = 0; m < nm; m++)
            for (int d2 = 0; d2 < M[m].ndecl; d2++)
                if (E->decls[M[m].decl0 + d2].imp)
                    st_apply(&st, pst, &E->decls[M[m].decl0 + d2], pass);
        for (int d2 = 0; d2 < inlN; d2++)
            if (E->decls[inl0 + d2].imp)
                st_apply(&st, pst, &E->decls[inl0 + d2], pass);
    }
    E->ndecls = inl0;
    E->csspool_len = cp0;
    /* cellpadding= hint: applies only when the cascade left the UA
     * default td,th padding (1px 8px) untouched -- author CSS wins */
    if (is_cell && st.p[0] == 1 && st.p[1] == 8 &&
        st.p[2] == 1 && st.p[3] == 8) {
        int t2 = nd->parent;
        for (int hop = 0; hop < 6 && t2 >= 0 &&
                          E->nodes[t2].tag != T_TABLE; hop++)
            t2 = E->nodes[t2].parent;
        char a[12];
        if (t2 >= 0 && E->nodes[t2].tag == T_TABLE &&
            node_attr_str(&E->nodes[t2], "cellpadding", a, sizeof(a))) {
            int v = atoi_simple(a);
            if (v >= 0 && v <= 200)
                for (int s2 = 0; s2 < 4; s2++) st.p[s2] = (int16_t)v;
        }
    }
    nd->st = st;
    for (int c = nd->first; c >= 0; c = E->nodes[c].next)
        style_node(c, &nd->st);
}

/* ---- The user-agent stylesheet ------------------------------------- */

static const char UA_SHEET[] =
"html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,dl,dt,dd,"
"pre,blockquote,form,header,footer,nav,main,section,"
"article,aside,figure,figcaption,address,details,summary,fieldset,"
"legend,center,hr{display:block}\n"
"table{display:table}\n"
"thead,tbody,tfoot{display:table-row-group}\n"
"tr{display:table-row}\n"
"td,th{display:table-cell}\n"
"caption{display:table-caption}\n"
"li{display:list-item}\n"
"head,script,style,title,meta,link,noscript,template,option,base,param,"
"track,source,area,col,colgroup,select,textarea,iframe,object,embed,"
"video,audio,canvas,svg,map,picture{display:none}\n"
"input[type=hidden]{display:none}\n"
"body{margin:8px;color:#1b1b1b;background-color:#ffffff;font-size:15px;"
"line-height:1.3}\n"
"h1{font-size:26px;font-weight:bold;margin:16px 0 8px}\n"
"h2{font-size:21px;font-weight:bold;margin:14px 0 7px}\n"
"h3{font-size:17px;font-weight:bold;margin:12px 0 6px}\n"
"h4,h5,h6{font-size:15px;font-weight:bold;margin:10px 0 5px}\n"
"p{margin:8px 0}\n"
"ul,ol{margin:8px 0;padding-left:26px}\n"
"li{margin:2px 0}\n"
"dt{font-weight:bold;margin-top:6px}\n"
"dd{margin-left:24px}\n"
"a{color:#1558d6;text-decoration:underline}\n"
"b,strong{font-weight:bold}\n"
"u{text-decoration:underline}\n"
"code,tt,kbd,samp{font-family:monospace;background-color:#f1f1f3}\n"
"pre{font-family:monospace;white-space:pre;background-color:#f6f6f8;"
"margin:8px 0;padding:6px;border:1px solid #e3e3e6}\n"
"blockquote{margin:8px 0 8px 26px;color:#484848}\n"
"hr{border-top:1px solid #c8c8cc;margin:10px 0}\n"
"center{text-align:center}\n"
"table{margin:6px 0}\n"
"td,th{padding:1px 8px}\n"
"th{font-weight:bold}\n"
"caption{font-weight:bold;text-align:center}\n"
"small{font-size:12px}\n"
"big{font-size:18px}\n"
"sub,sup{font-size:11px}\n"
"mark{background-color:#ffef9c}\n"
"figure{margin:8px 0 8px 24px}\n"
"button{display:inline-block;background-color:#f1f3f4;border:1px solid #babcbe;padding:2px 10px}\n";

/* =================== Layout: DOM + styles -> display list =========== */

/* Display size of an image scaled to fit `avail` px wide (aspect
 * preserved). Loaded images use decoded dims; pending/failed fall back
 * to width/height attributes or a default placeholder box. */
static void img_disp_dims(const struct img *im, int avail, int *dw, int *dh) {
    int w, h;
    if (im->state == 1 && im->w > 0 && im->h > 0) {
        w = im->w; h = im->h;
    } else if (im->attr_w > 0 && im->attr_h > 0) {
        w = im->attr_w; h = im->attr_h;
    } else if (im->attr_w > 0) {
        w = im->attr_w; h = im->attr_w * 3 / 4;
    } else {
        w = 180; h = 120;                 /* placeholder box */
    }
    if (avail < 16) avail = 16;
    if (w > avail) { h = (int)((long)h * avail / w); w = avail; }
    if (h < 1) h = 1;
    *dw = w; *dh = h;
}

/* On-screen width of a form control. */
static int field_w(const struct field *f, uint8_t fl) {
    if (fl & IF_INPUT)
        return f->size > 0 ? f->size * 8 + 18 : 190;
    /* submit button: label width + padding */
    int lw = text_px_w(f->value, (int)str_len(f->value), PX_BODY, 0, 0);
    return lw + 28;
}
#define FIELD_H (PX_BODY + 12)

static struct ditem *item_new(int kind) {
    if (E->nitems >= ITEM_MAX) return NULL;
    struct ditem *it = &E->items[E->nitems++];
    mem_zero(it, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->link = it->field = it->img = -1;
    it->node = -1;
    return it;
}

static int emit_rect(int x, int y, int w, int h, uint32_t col) {
    struct ditem *it = item_new(DI_RECT);
    if (!it) return -1;
    it->x = x; it->y = y; it->w = w; it->h = h;
    it->fg = col;
    return (int)(it - E->items);
}

/* ---- Floats --------------------------------------------------------- *
 * Out-of-flow boxes in doc coordinates, reset per layout() pass. Only
 * LINE boxes avoid them (block boxes extend under floats, per CSS);
 * parents don't grow around them (the classic un-clearfixed behavior). */

#define FLOAT_MAX 64
struct fltbox { int32_t x0, x1, y0, y1; uint8_t side; };
static struct fltbox g_flts[FLOAT_MAX];
static int g_nflts;
static int g_flt_bottom;
static int g_flt_freeze;   /* shrink-to-fit measure pass: floats are
                              neither consulted nor registered */

/* Line-box bounds at band [y, y+h) within [cx, cx+cw). */
static void float_bounds(int y, int h, int cx, int cw, int *lx0, int *lx1) {
    int x0 = cx, x1 = cx + cw;
    if (g_flt_freeze) { *lx0 = x0; *lx1 = x1; return; }
    for (int i = 0; i < g_nflts; i++) {
        struct fltbox *f = &g_flts[i];
        if (y >= f->y1 || y + h <= f->y0) continue;
        if (f->side == 1) { if (f->x1 > x0) x0 = f->x1; }
        else { if (f->x0 < x1) x1 = f->x0; }
    }
    if (x1 < x0 + 8) x1 = x0 + 8;
    *lx0 = x0;
    *lx1 = x1;
}

static int float_clear_y(int y, int mask) {
    if (g_flt_freeze) return y;
    for (int i = 0; i < g_nflts; i++)
        if ((g_flts[i].side & mask) && g_flts[i].y1 > y)
            y = g_flts[i].y1;
    return y;
}

/* First y below the shallowest float constraining the band at y. */
static int float_next_y(int y, int h) {
    int ny = -1;
    if (g_flt_freeze) return y;
    for (int i = 0; i < g_nflts; i++) {
        struct fltbox *f = &g_flts[i];
        if (y < f->y1 && y + h > f->y0)
            if (ny < 0 || f->y1 < ny) ny = f->y1;
    }
    return ny < 0 ? y : ny;
}

/* ---- Inline formatting context ------------------------------------- */

struct istyle {
    uint32_t fg, bg;
    int px, fl;                  /* IF_* text flags */
    int lh;                      /* resolved line height for text items */
    int link;
    int pre;
    int node;                    /* nearest element node (event target) */
};

struct ictx {
    int cx, cw;                  /* content box left + width */
    int x, y;                    /* cursor (y = top of current line) */
    int lx0, lx1;                /* current line-box bounds (floats) */
    int def_lh;                  /* representative line height for bounds */
    int line_h;
    int line_i0;                 /* first item of the current line */
    int talign;
    int pend_sp;
    int citem;                   /* open coalescing text item or -1 */
};

/* Recompute the line bounds at the current y (line start only). */
static void ic_rebound(struct ictx *ic) {
    float_bounds(ic->y, ic->def_lh, ic->cx, ic->cw, &ic->lx0, &ic->lx1);
    ic->x = ic->lx0;
}

/* Finish the current line: bottom-align items, apply text-align.
 * In a frozen measure pass (shrink-to-fit / table column sizing) the
 * alignment shift is suppressed: centering/right-aligning against the
 * huge provisional width would inflate the measured item extents. */
static void ic_break(struct ictx *ic, int min_h) {
    int lh = ic->line_h > 0 ? ic->line_h : min_h;
    int used = ic->x - ic->lx0;
    int talign = g_flt_freeze ? 0 : ic->talign;
    int shift = 0;
    if (talign == 1) shift = ((ic->lx1 - ic->lx0) - used) / 2;
    else if (talign == 2) shift = ic->lx1 - ic->x;
    if (shift < 0) shift = 0;
    for (int i = ic->line_i0; i < E->nitems; i++) {
        struct ditem *it = &E->items[i];
        it->y = ic->y + (lh - it->h);
        if (shift) it->x += shift;
    }
    ic->y += lh;
    ic->line_h = 0;
    ic->line_i0 = E->nitems;
    ic->citem = -1;
    ic->pend_sp = 0;
    ic_rebound(ic);
}

/* Append one word to the line, wrapping as needed; coalesces into the
 * open text item when style + render-pool position allow. */
static void ic_word(struct ictx *ic, const char *w, int wl, const struct istyle *is) {
    int mono = is->fl & IF_MONO;
    int bold = is->fl & IF_BOLD;
    int ww = text_px_w(w, wl, is->px, bold, mono);
    int sw = ic->pend_sp ? text_px_w(" ", 1, is->px, bold, mono) : 0;
    for (int guard = 0; guard < 64; guard++) {
        if (ic->x > ic->lx0 && ic->x + sw + ww > ic->lx1) {
            ic_break(ic, is->lh);      /* wrap within the line box */
            sw = 0;
            continue;
        }
        if (ic->x == ic->lx0 && ic->x + ww > ic->lx1 &&
            (ic->lx0 > ic->cx || ic->lx1 < ic->cx + ic->cw)) {
            /* empty float-shortened line can't fit the word: drop below */
            int ny = float_next_y(ic->y, is->lh);
            if (ny > ic->y) {
                ic->y = ny;
                ic->line_i0 = E->nitems;
                ic->citem = -1;
                ic_rebound(ic);
                continue;
            }
        }
        break;
    }
    int sp = (ic->x > ic->lx0) ? ic->pend_sp : 0;
    ic->pend_sp = 0;
    sw = sp ? text_px_w(" ", 1, is->px, bold, mono) : 0;
    struct ditem *cu = (ic->citem >= 0) ? &E->items[ic->citem] : NULL;
    int same = cu && cu->kind == DI_TEXT && cu->px == is->px &&
               cu->fl == (uint8_t)is->fl && cu->fg == is->fg &&
               cu->bg == is->bg && cu->link == is->link &&
               cu->off + cu->len == E->render_len;
    if (!same) {
        if (sp) { ic->x += sw; sp = 0; sw = 0; }
        struct ditem *it = item_new(DI_TEXT);
        if (!it) return;
        it->x = ic->x;
        it->y = ic->y;
        it->w = 0;
        it->h = is->lh;
        it->fg = is->fg;
        it->bg = is->bg;
        it->off = E->render_len;
        it->len = 0;
        it->px = (uint8_t)is->px;
        it->fl = (uint8_t)is->fl;
        it->link = (int16_t)is->link;
        it->node = is->node;
        ic->citem = (int)(it - E->items);
        cu = it;
    }
    if (sp && E->render_len < RENDER_CAP) {
        E->render[E->render_len++] = ' ';
        cu->len++;
        cu->w += sw;
        ic->x += sw;
    }
    for (int k = 0; k < wl && E->render_len < RENDER_CAP; k++) {
        char c2 = w[k];
        E->render[E->render_len++] = ((unsigned char)c2 < 32) ? ' ' : c2;
        cu->len++;
    }
    cu->w += ww;
    ic->x += ww;
    if (cu->h > ic->line_h) ic->line_h = cu->h;
}

/* Atomic inline box (image / form control). */
static void ic_atomic(struct ictx *ic, int kind, int w, int h,
                      int link, int field, int img, int fl, int node) {
    for (int guard = 0; guard < 64; guard++) {
        if (ic->x > ic->lx0 && ic->x + w > ic->lx1) {
            ic_break(ic, h + 2);
            continue;
        }
        if (ic->x == ic->lx0 && ic->x + w > ic->lx1 &&
            (ic->lx0 > ic->cx || ic->lx1 < ic->cx + ic->cw)) {
            int ny = float_next_y(ic->y, h);
            if (ny > ic->y) {
                ic->y = ny;
                ic->line_i0 = E->nitems;
                ic->citem = -1;
                ic_rebound(ic);
                continue;
            }
        }
        break;
    }
    struct ditem *it = item_new(kind);
    if (!it) return;
    it->x = ic->x;
    it->y = ic->y;
    it->w = w;
    it->h = h;
    it->link = (int16_t)link;
    it->field = (int16_t)field;
    it->img = (int16_t)img;
    it->fl = (uint8_t)fl;
    it->px = PX_BODY;
    it->node = node;
    ic->x += w + 4;
    if (h + 2 > ic->line_h) ic->line_h = h + 2;
    ic->citem = -1;
    ic->pend_sp = 0;
}

static void inl_text_norm(struct ictx *ic, const char *t, int tl, const struct istyle *is) {
    int i = 0;
    while (i < tl) {
        if (is_whitespace(t[i])) { ic->pend_sp = 1; i++; continue; }
        int w0 = i;
        while (i < tl && !is_whitespace(t[i])) i++;
        ic_word(ic, t + w0, i - w0, is);
    }
}

static void inl_text_pre(struct ictx *ic, const char *t, int tl, const struct istyle *is) {
    int i = 0;
    while (i < tl) {
        if (t[i] == '\n') {
            ic_break(ic, is->lh);
            i++;
            continue;
        }
        int budget = ic->lx1 - ic->x;
        int w = 0, j = i;
        while (j < tl && t[j] != '\n') {
            int cw2 = text_px_w(&t[j], 1, is->px, is->fl & IF_BOLD, is->fl & IF_MONO);
            if (w + cw2 > budget && j > i) break;
            w += cw2;
            j++;
        }
        if (j == i) {                     /* nothing fits */
            if (ic->x > ic->lx0) { ic_break(ic, is->lh); continue; }
            j = i + 1;
        }
        ic->pend_sp = 0;
        ic_word(ic, t + i, j - i, is);
        i = j;
    }
}

static void inl_walk(struct ictx *ic, int ni, const struct istyle *is) {
    struct dnode *nd = &E->nodes[ni];
    if (nd->tag == T_TEXT) {
        if (is->pre) inl_text_pre(ic, &E->tpool[nd->toff], nd->tlen, is);
        else inl_text_norm(ic, &E->tpool[nd->toff], nd->tlen, is);
        return;
    }
    const struct cstyle *st = &nd->st;
    if (st->disp == D_NONE) return;
    if (nd->tag == T_BR) { ic_break(ic, is->lh); return; }

    struct istyle cs;
    cs.fg = st->color;
    cs.bg = (st->bg >> 24) ? st->bg : is->bg;
    cs.px = st->px;
    cs.fl = 0;
    if (st->fl & SF_BOLD)  cs.fl |= IF_BOLD;
    if (st->fl & SF_UNDER) cs.fl |= IF_UNDER;
    if (st->fl & SF_MONO)  cs.fl |= IF_MONO;
    cs.lh = st_line_h(st);
    cs.pre = (st->fl & SF_PRE) ? 1 : 0;
    cs.link = nd->link >= 0 ? nd->link : is->link;
    cs.node = ni;

    if (nd->img >= 0) {
        int dw, dh;
        img_disp_dims(&g_images[nd->img], ic->cw, &dw, &dh);
        if (st->width > 0 && !(st->fl & SF_WPCT)) {
            int nw = st->width > ic->cw ? ic->cw : st->width;
            dh = (int)((long)dh * nw / (dw > 0 ? dw : 1));
            dw = nw;
        }
        if (st->height > 0) dh = st->height;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        ic_atomic(ic, DI_IMG, dw, dh, cs.link, -1, nd->img, 0, ni);
        return;
    }
    if (nd->field >= 0) {
        struct field *f = &g_fields[nd->field];
        int fl2 = (f->type == FT_TEXT) ? IF_INPUT : IF_SUBMITB;
        ic_atomic(ic, DI_FIELD, field_w(f, (uint8_t)fl2), FIELD_H,
                  -1, nd->field, -1, fl2, ni);
        return;
    }
    /* inline container (block-in-inline degrades to inline flow) */
    for (int c = nd->first; c >= 0; c = E->nodes[c].next)
        inl_walk(ic, c, &cs);
}

static int is_inline_level(int ci) {
    struct dnode *c = &E->nodes[ci];
    if (c->tag == T_TEXT) return 1;
    if (c->st.disp == D_NONE) return 1;   /* skipped inside either path */
    if (c->st.flt) return 0;              /* floats leave the flow */
    int d = c->st.disp;
    return d == D_INLINE || d == D_INLBLOCK;
}

/* Lay an anonymous run of inline-level siblings [first, stop).
 * `inbg` is the effective background the text sits on (nearest painted
 * ancestor box); items record it only when it differs from the page bg
 * so the mono painter can fill matching cells. */
static int flush_inline(int first_child, int stop_child, int cx, int cw,
                        int y, const struct cstyle *bst, int link,
                        uint32_t inbg, int bnode) {
    struct ictx ic;
    ic.cx = cx;
    ic.cw = cw > 8 ? cw : 8;
    ic.y = y;
    ic.def_lh = st_line_h(bst);
    ic.line_h = 0;
    ic.line_i0 = E->nitems;
    ic.talign = bst->talign;
    ic.pend_sp = 0;
    ic.citem = -1;
    ic_rebound(&ic);                      /* sets x/lx0/lx1 vs floats */
    struct istyle is;
    is.fg = bst->color;
    is.bg = ((inbg >> 24) &&
             (inbg & 0xFFFFFF) != (E->page_bg & 0xFFFFFF)) ? inbg : 0;
    is.px = bst->px;
    is.fl = 0;
    if (bst->fl & SF_BOLD)  is.fl |= IF_BOLD;
    if (bst->fl & SF_UNDER) is.fl |= IF_UNDER;
    if (bst->fl & SF_MONO)  is.fl |= IF_MONO;
    is.lh = st_line_h(bst);
    is.pre = (bst->fl & SF_PRE) ? 1 : 0;
    is.link = link;
    is.node = bnode;
    int n0 = E->nitems;
    for (int c = first_child; c >= 0 && c != stop_child; c = E->nodes[c].next)
        inl_walk(&ic, c, &is);
    if (E->nitems > n0 || ic.x > ic.lx0)
        ic_break(&ic, is.lh);
    return ic.y;
}

/* ---- Block flow ----------------------------------------------------- */

static int lay_block(int ni, int x, int cw, int y, int link, uint32_t inbg);

/* Widest non-rect item extent since i0 (bg/border rects excluded: an
 * auto-width block's bg spans the whole avail width). */
static int items_extent(int i0) {
    int e = 0;
    for (int i = i0; i < E->nitems; i++) {
        struct ditem *it = &E->items[i];
        if (it->kind == DI_RECT) continue;
        if (it->x + it->w > e) e = it->x + it->w;
    }
    return e;
}

/* Take a floated box out of flow: lay it (shrink-to-fit when width is
 * auto), slide it against the left/right line edge (dropping below
 * other floats when it doesn't fit), shift its items to the placed
 * origin, and register its margin box so line boxes wrap around it. */
static void lay_float(int ni, int cx, int cw, int cy, int link, uint32_t inbg) {
    struct dnode *nd = &E->nodes[ni];
    struct cstyle *st = &nd->st;
    if (st->clr) cy = float_clear_y(cy, st->clr);

    int ml = st->m[3] == M_AUTO ? 0 : st->m[3];
    int mr = st->m[1] == M_AUTO ? 0 : st->m[1];
    int mt = st->m[0] == M_AUTO ? 0 : st->m[0];
    int mb = st->m[2] == M_AUTO ? 0 : st->m[2];

    int i0 = E->nitems, f1;
    int by = cy + mt;                      /* real flow position (search start) */
    int laytop = by;                       /* where the content was laid */
    int bw2, bbh;                          /* border-box width incl. ml, height */

    if (nd->img >= 0) {
        int dw, dh;
        img_disp_dims(&g_images[nd->img], cw > 48 ? cw / 2 : cw, &dw, &dh);
        if (st->width > 0 && !(st->fl & SF_WPCT)) {
            int nw = st->width > cw ? cw : st->width;
            dh = (int)((long)dh * nw / (dw > 0 ? dw : 1));
            dw = nw;
        }
        if (st->height > 0) dh = st->height;
        f1 = g_nflts;
        struct ditem *it = item_new(DI_IMG);
        if (!it) return;
        it->x = ml;
        it->y = by;
        it->w = dw;
        it->h = dh;
        it->img = nd->img;
        it->link = (int16_t)(nd->link >= 0 ? nd->link : link);
        it->node = ni;
        bw2 = ml + dw;
        bbh = dh;
    } else {
        /* Lay the float's content in a provisional space far below any
         * real content (y + 1M): its lines must not consult the real
         * float list (coordinates are pre-shift), while its own inner
         * floats still work locally. Items shift into place below. */
        int prov = (1 << 20) + by;
        int lay_cw = cw;
        if (st->width < 0) {
            /* shrink-to-fit: measure the preferred (unwrapped) width
             * with floats frozen (no consulting, no registering) */
            int mi = E->nitems, mrr = E->render_len, mf = g_nflts;
            g_flt_freeze++;
            lay_block(ni, 0, 100000, prov, link, inbg);
            g_flt_freeze--;
            int pref = items_extent(mi) + st->p[1] + st->bw[1];
            E->nitems = mi;
            E->render_len = mrr;
            g_nflts = mf;
            lay_cw = pref + mr + 2;
            if (lay_cw > cw) lay_cw = cw;
            if (lay_cw < 24) lay_cw = 24;
        }
        f1 = g_nflts;
        int bot = lay_block(ni, 0, lay_cw, prov, link, inbg);
        bbh = bot - prov;
        laytop = prov;
        if (st->width >= 0) {
            int avail = lay_cw - ml - mr - st->bw[3] - st->bw[1] - st->p[3] - st->p[1];
            if (avail < 8) avail = 8;
            int cwid = (st->fl & SF_WPCT) ? (int)((long)avail * st->width / 100)
                                          : st->width;
            if (cwid > avail) cwid = avail;
            if (cwid < 8) cwid = 8;
            if (st->max_w >= 0 && cwid > st->max_w) cwid = st->max_w;
            bw2 = ml + st->bw[3] + st->p[3] + cwid + st->p[1] + st->bw[1];
        } else {
            bw2 = items_extent(i0) + st->p[1] + st->bw[1];
        }
    }

    /* place: walk down until the margin box fits between other floats */
    int mbw = bw2 + mr;
    int fy = by, lx0, lx1;
    for (int guard = 0; guard < 64; guard++) {
        float_bounds(fy, bbh > 0 ? bbh : 1, cx, cw, &lx0, &lx1);
        if (lx1 - lx0 >= mbw) break;
        if (lx0 == cx && lx1 == cx + cw) break;   /* nothing to dodge */
        int ny = float_next_y(fy, bbh > 0 ? bbh : 1);
        if (ny <= fy) break;
        fy = ny;
    }
    float_bounds(fy, bbh > 0 ? bbh : 1, cx, cw, &lx0, &lx1);
    int dx = (st->flt == 1) ? lx0 : lx1 - mbw;
    if (dx < cx) dx = cx;
    int dy = fy - laytop;
    for (int i = i0; i < E->nitems; i++) {
        E->items[i].x += dx;
        E->items[i].y += dy;
    }
    for (int i = f1; i < g_nflts; i++) {   /* floats nested in this float */
        g_flts[i].x0 += dx; g_flts[i].x1 += dx;
        g_flts[i].y0 += dy; g_flts[i].y1 += dy;
    }
    if (!g_flt_freeze && g_nflts < FLOAT_MAX) {
        struct fltbox *f = &g_flts[g_nflts++];
        f->x0 = dx;
        f->x1 = dx + mbw;
        f->y0 = fy - mt;
        f->y1 = fy + bbh + mb;
        f->side = st->flt;
    }
}

/* ---- Block flow child loop (shared by blocks and table cells) ------- *
 * Lays ni's children into the content box (cx, cy, content_w): floats
 * leave the flow, inline-level runs go through the inline formatting
 * context, block-level children recurse. Returns the flow bottom. */
static int lay_flow(int ni, int cx, int content_w, int cy, int link,
                    uint32_t curbg) {
    struct dnode *nd = &E->nodes[ni];
    const struct cstyle *st = &nd->st;
    int c = nd->first;
    int prev_mb = 0;
    while (c >= 0) {
        struct dnode *cn = &E->nodes[c];
        if (cn->tag != T_TEXT && cn->st.disp != D_NONE && cn->st.flt) {
            lay_float(c, cx, content_w, cy, link, curbg);
            c = cn->next;
            continue;
        }
        if (is_inline_level(c)) {
            int start = c;
            while (c >= 0 && is_inline_level(c))
                c = E->nodes[c].next;
            int ny = flush_inline(start, c, cx, content_w, cy, st, link,
                                  curbg, ni);
            if (ny != cy) prev_mb = 0;
            cy = ny;
        } else {
            if (cn->st.clr) cy = float_clear_y(cy, cn->st.clr);
            int mt = cn->st.m[0] == M_AUTO ? 0 : cn->st.m[0];
            int mb = cn->st.m[2] == M_AUTO ? 0 : cn->st.m[2];
            cy += mt > prev_mb ? mt : prev_mb;     /* sibling collapse */
            cy = lay_block(c, cx, content_w, cy, link, curbg);
            prev_mb = mb;
            c = cn->next;
        }
    }
    return cy + prev_mb;
}

/* ---- Table layout (CSS tables v1) ----------------------------------- *
 * Auto + fixed column sizing from per-cell min/preferred content
 * measures (the frozen provisional-layout trick from floats), colspan,
 * legacy table attributes (border/cellspacing/cellpadding/bgcolor/
 * align/valign/width), border-collapse as spacing 0, valign by item
 * shift, caption above the grid. Rowspan degrades to 1; cells are
 * block containers laid by lay_flow. All grid state lives on the
 * stack so nested tables recurse safely. */

#define TBL_COL_MAX 24

/* Grid width used by the most recent lay_table_grid; consumed
 * immediately by its caller lay_block (set-then-read is adjacent even
 * under nested-table recursion). */
static int g_tbl_used_w;

static int node_attr_int(const struct dnode *n, const char *name, int dflt) {
    char a[12];
    if (!node_attr_str(n, name, a, sizeof(a))) return dflt;
    return atoi_simple(a);
}

/* Min-content / preferred border-box widths of a cell, via two frozen
 * provisional layouts (unwrapped, then narrowest) rolled back after. */
static void tbl_measure_cell(int ci, int link, uint32_t inbg,
                             int *out_min, int *out_pref) {
    struct dnode *nd = &E->nodes[ci];
    struct cstyle *st = &nd->st;
    int ins = st->bw[3] + st->bw[1] + st->p[3] + st->p[1];
    if (st->width >= 0 && !(st->fl & SF_WPCT)) {
        *out_min = *out_pref = st->width + ins;
        return;
    }
    int mi = E->nitems, mrr = E->render_len, mf = g_nflts;
    int prov = 1 << 20;
    g_flt_freeze++;
    lay_flow(ci, 0, 100000, prov, link, inbg);
    int pref = items_extent(mi);
    E->nitems = mi; E->render_len = mrr; g_nflts = mf;
    lay_flow(ci, 0, 8, prov, link, inbg);
    int minw = items_extent(mi);
    E->nitems = mi; E->render_len = mrr; g_nflts = mf;
    g_flt_freeze--;
    if (minw > pref) pref = minw;
    *out_min = minw + ins;
    *out_pref = pref + ins;
}

/* Fold one row's cells into the column min/pref/percent tables. */
static void tbl_measure_row(int ri, int col_min[], int col_pref[],
                            int col_pct[], int *ncols, int link,
                            uint32_t inbg) {
    int cc = 0;
    for (int c = E->nodes[ri].first; c >= 0; c = E->nodes[c].next) {
        struct dnode *cn = &E->nodes[c];
        if (cn->tag == T_TEXT || cn->st.disp != D_TCELL) continue;
        if (cc >= TBL_COL_MAX) break;
        int span = node_attr_int(cn, "colspan", 1);
        if (span < 1) span = 1;
        if (span > TBL_COL_MAX - cc) span = TBL_COL_MAX - cc;
        int mn, pf;
        tbl_measure_cell(c, link, inbg, &mn, &pf);
        if ((cn->st.fl & SF_WPCT) && cn->st.width > 0 && span == 1 &&
            cn->st.width > col_pct[cc])
            col_pct[cc] = cn->st.width;
        if (span == 1) {
            if (mn > col_min[cc]) col_min[cc] = mn;
            if (pf > col_pref[cc]) col_pref[cc] = pf;
        } else {
            int mn_e = mn / span, pf_e = pf / span;
            for (int k = 0; k < span; k++) {
                if (mn_e > col_min[cc + k]) col_min[cc + k] = mn_e;
                if (pf_e > col_pref[cc + k]) col_pref[cc + k] = pf_e;
            }
        }
        cc += span;
        if (cc > *ncols) *ncols = cc;
    }
}

struct tcell_info { int i0, i1, bg_i, x, w, h, node; };

/* Lay one row at y with resolved column widths. Returns the row
 * bottom. Cell backgrounds stretch to the row height, valign shifts
 * the content items, borders are drawn at the final box. Floats
 * cannot escape their row. */
static int tbl_lay_row(int ri, int cx, int y, const int colw[], int ncols,
                       int spacing, int grid_w, int link, uint32_t inbg) {
    struct dnode *rn = &E->nodes[ri];
    uint32_t rowbg = (rn->st.bg >> 24) ? rn->st.bg : inbg;
    int row_bg_i = -1;
    if (rn->st.bg >> 24) {
        row_bg_i = emit_rect(cx, y, grid_w, 0, rn->st.bg);
        if (row_bg_i >= 0) E->items[row_bg_i].node = ri;
    }
    struct tcell_info cells[TBL_COL_MAX];
    int nc = 0, cc = 0;
    int xcur = cx + spacing;
    int flt0 = g_nflts;
    int row_h = rn->st.height > 0 ? rn->st.height : 0;
    for (int c = rn->first; c >= 0; c = E->nodes[c].next) {
        struct dnode *cn = &E->nodes[c];
        if (cn->tag == T_TEXT || cn->st.disp != D_TCELL) continue;
        if (cc >= ncols || nc >= TBL_COL_MAX) break;
        int span = node_attr_int(cn, "colspan", 1);
        if (span < 1) span = 1;
        if (span > ncols - cc) span = ncols - cc;
        int w = spacing * (span - 1);
        for (int k = 0; k < span; k++) w += colw[cc + k];
        struct cstyle *st = &cn->st;
        struct tcell_info *tc = &cells[nc++];
        tc->node = c;
        tc->x = xcur;
        tc->w = w;
        tc->bg_i = -1;
        uint32_t cbg = (st->bg >> 24) ? st->bg : rowbg;
        if (st->bg >> 24) {
            tc->bg_i = emit_rect(xcur, y, w, 0, st->bg);
            if (tc->bg_i >= 0) E->items[tc->bg_i].node = c;
        }
        tc->i0 = E->nitems;
        int ccx = xcur + st->bw[3] + st->p[3];
        int ccy = y + st->bw[0] + st->p[0];
        int ccw = w - st->bw[3] - st->bw[1] - st->p[3] - st->p[1];
        if (ccw < 8) ccw = 8;
        int cl = cn->link >= 0 ? cn->link : link;
        int bot = lay_flow(c, ccx, ccw, ccy, cl, cbg);
        int content_h = bot - ccy;
        if (content_h < 0) content_h = 0;
        if (st->height > content_h) content_h = st->height;
        int bbh = st->bw[0] + st->p[0] + content_h + st->p[2] + st->bw[2];
        tc->i1 = E->nitems;
        tc->h = bbh;
        if (bbh > row_h) row_h = bbh;
        xcur += w + spacing;
        cc += span;
    }
    if (nc == 0) {
        if (row_bg_i >= 0) E->items[row_bg_i].h = row_h;
        return y + row_h;
    }
    if (row_bg_i >= 0) E->items[row_bg_i].h = row_h;
    for (int k = 0; k < nc; k++) {
        struct tcell_info *tc = &cells[k];
        struct cstyle *st = &E->nodes[tc->node].st;
        if (tc->bg_i >= 0) E->items[tc->bg_i].h = row_h;
        uint8_t va = st->valign ? st->valign : rn->st.valign;
        if (va && row_h > tc->h) {
            int off = (va == 1) ? (row_h - tc->h) / 2 : row_h - tc->h;
            for (int i2 = tc->i0; i2 < tc->i1; i2++)
                E->items[i2].y += off;
        }
        if (st->bw[0]) emit_rect(tc->x, y, tc->w, st->bw[0], st->border_col);
        if (st->bw[2]) emit_rect(tc->x, y + row_h - st->bw[2], tc->w,
                                 st->bw[2], st->border_col);
        if (st->bw[3]) emit_rect(tc->x, y, st->bw[3], row_h, st->border_col);
        if (st->bw[1]) emit_rect(tc->x + tc->w - st->bw[1], y, st->bw[1],
                                 row_h, st->border_col);
    }
    g_nflts = flt0;                /* floats do not escape their row */
    return y + row_h;
}

/* The grid: captions, two passes over the rows (measure, lay). */
static int lay_table_grid(int ti, int cx, int avail_w, int y, int link,
                          uint32_t inbg) {
    struct dnode *tn = &E->nodes[ti];
    struct cstyle *tst = &tn->st;

    int spacing = 2;
    int cs = node_attr_int(tn, "cellspacing", -1);
    if (cs >= 0 && cs <= 100) spacing = cs;
    if (tst->fl & SF_BCOLLAPSE) spacing = 0;

    /* caption(s) above the grid (caption-side: top only) */
    for (int c = tn->first; c >= 0; c = E->nodes[c].next)
        if (E->nodes[c].tag != T_TEXT && E->nodes[c].st.disp == D_CAPTION)
            y = lay_block(c, cx, avail_w, y, link, inbg);

    /* pass 1: column min/pref widths over all rows */
    int col_min[TBL_COL_MAX], col_pref[TBL_COL_MAX], col_pct[TBL_COL_MAX];
    for (int i2 = 0; i2 < TBL_COL_MAX; i2++)
        col_min[i2] = col_pref[i2] = col_pct[i2] = 0;
    int ncols = 0;
    for (int g = tn->first; g >= 0; g = E->nodes[g].next) {
        struct dnode *gn = &E->nodes[g];
        if (gn->tag == T_TEXT || gn->st.disp == D_NONE) continue;
        if (gn->st.disp == D_TROW)
            tbl_measure_row(g, col_min, col_pref, col_pct, &ncols, link, inbg);
        else if (gn->st.disp == D_TSEC)
            for (int r = gn->first; r >= 0; r = E->nodes[r].next)
                if (E->nodes[r].tag != T_TEXT &&
                    E->nodes[r].st.disp == D_TROW)
                    tbl_measure_row(r, col_min, col_pref, col_pct, &ncols,
                                    link, inbg);
    }
    if (ncols == 0) { g_tbl_used_w = 0; return y; }

    /* distribute the available width over the columns */
    int sum_min = 0, sum_pref = 0;
    for (int i2 = 0; i2 < ncols; i2++) {
        sum_min += col_min[i2];
        sum_pref += col_pref[i2];
    }
    int chrome = spacing * (ncols + 1);
    int W;
    if (tst->width >= 0) W = avail_w;   /* resolved by lay_block */
    else {
        W = sum_pref + chrome;          /* shrink-to-fit */
        if (W > avail_w) W = avail_w;
    }
    int Wc = W - chrome;
    if (Wc < ncols * 8) { Wc = ncols * 8; W = Wc + chrome; }

    /* percent columns claim their share first */
    for (int i2 = 0; i2 < ncols; i2++)
        if (col_pct[i2] > 0) {
            int want = (int)((long)Wc * col_pct[i2] / 100);
            if (want > col_pref[i2]) col_pref[i2] = want;
            if (col_pref[i2] < col_min[i2]) col_pref[i2] = col_min[i2];
        }
    sum_pref = 0;
    for (int i2 = 0; i2 < ncols; i2++) sum_pref += col_pref[i2];

    int colw[TBL_COL_MAX];
    if (sum_pref <= Wc) {
        int extra = Wc - sum_pref;
        for (int i2 = 0; i2 < ncols; i2++) {
            colw[i2] = col_pref[i2];
            if (tst->width >= 0 && extra > 0)
                colw[i2] += (int)((long)extra *
                                  (col_pref[i2] ? col_pref[i2] : 1) /
                                  (sum_pref ? sum_pref : ncols));
        }
        if (tst->width < 0) { Wc = sum_pref; W = Wc + chrome; }
    } else if (sum_min < Wc) {
        /* between min and pref: interpolate proportionally */
        long num = Wc - sum_min, den = sum_pref - sum_min;
        for (int i2 = 0; i2 < ncols; i2++)
            colw[i2] = col_min[i2] +
                (int)((long)(col_pref[i2] - col_min[i2]) * num /
                      (den > 0 ? den : 1));
    } else {
        /* doesn't fit even at min: scale the mins down (no h-scroll) */
        for (int i2 = 0; i2 < ncols; i2++)
            colw[i2] = (int)((long)col_min[i2] * Wc /
                             (sum_min > 0 ? sum_min : 1));
    }

    /* pass 2: lay the rows */
    for (int g = tn->first; g >= 0; g = E->nodes[g].next) {
        struct dnode *gn = &E->nodes[g];
        if (gn->tag == T_TEXT || gn->st.disp == D_NONE) continue;
        if (gn->st.disp == D_TROW) {
            y += spacing;
            y = tbl_lay_row(g, cx, y, colw, ncols, spacing, W, link, inbg);
        } else if (gn->st.disp == D_TSEC) {
            for (int r = gn->first; r >= 0; r = E->nodes[r].next)
                if (E->nodes[r].tag != T_TEXT &&
                    E->nodes[r].st.disp == D_TROW) {
                    y += spacing;
                    y = tbl_lay_row(r, cx, y, colw, ncols, spacing, W,
                                    link, inbg);
                }
        }
    }
    y += spacing;
    g_tbl_used_w = W;
    return y;
}

static int lay_block(int ni, int x, int cw, int y, int link, uint32_t inbg) {
    struct dnode *nd = &E->nodes[ni];
    struct cstyle *st = &nd->st;
    if (st->disp == D_NONE) return y;
    if (nd->link >= 0) link = nd->link;
    uint32_t curbg = (st->bg >> 24) ? st->bg : inbg;

    int ml = st->m[3] == M_AUTO ? 0 : st->m[3];
    int mr = st->m[1] == M_AUTO ? 0 : st->m[1];
    int bwt = st->bw[0], bwr = st->bw[1], bwb = st->bw[2], bwl = st->bw[3];
    int pt = st->p[0], pr = st->p[1], pb = st->p[2], pl = st->p[3];

    int avail = cw - ml - mr - bwl - bwr - pl - pr;
    if (avail < 8) avail = 8;
    int content_w = avail;
    if (st->width >= 0) {
        content_w = (st->fl & SF_WPCT) ? (int)((long)avail * st->width / 100)
                                       : st->width;
        if (content_w > avail) content_w = avail;
        if (content_w < 8) content_w = 8;
    }
    if (st->max_w >= 0 && content_w > st->max_w) content_w = st->max_w;
    if (g_flt_freeze) {
        /* measure pass: auto-margin centering against the provisional
         * width would inflate the measured extents */
    } else if (content_w < avail && st->m[3] == M_AUTO && st->m[1] == M_AUTO)
        ml += (avail - content_w) / 2;    /* margin: 0 auto centering */
    else if (content_w < avail && st->m[3] == M_AUTO)
        ml += avail - content_w;

    int bx = x + ml;                      /* border-box left */
    int by = y;
    int cx = bx + bwl + pl;
    int cy = by + bwt + pt;
    int bbw = bwl + pl + content_w + pr + bwr;

    int bg_i = -1;
    if (st->bg >> 24) {
        bg_i = emit_rect(bx, by, bbw, 0, st->bg);   /* height patched below */
        if (bg_i >= 0) E->items[bg_i].node = ni;    /* clicks on the box */
    }

    if (st->disp == D_LISTITEM && !(st->fl & SF_NOBULLET)) {
        struct ditem *bu = item_new(DI_BULLET);
        if (bu) {
            bu->x = cx - 14;
            bu->y = cy + st_line_h(st) / 2 - 4;
            bu->w = 5;
            bu->h = 5;
            bu->fg = st->color;
        }
    }

    /* children: table grid, or block flow with anonymous inline runs */
    if (st->disp == D_TABLE)
        cy = lay_table_grid(ni, cx, content_w, cy, link, curbg);
    else
        cy = lay_flow(ni, cx, content_w, cy, link, curbg);

    /* auto-width tables shrink-to-fit their grid: pull the border box
     * (and its already-emitted background) in to the used grid width */
    if (st->disp == D_TABLE && st->width < 0) {
        int used = g_tbl_used_w;
        if (used < 8) used = 8;
        if (used < content_w) {
            content_w = used;
            bbw = bwl + pl + content_w + pr + bwr;
            if (bg_i >= 0) E->items[bg_i].w = bbw;
        }
    }

    int content_h = cy - (by + bwt + pt);
    if (content_h < 0) content_h = 0;
    if (st->height >= 0 && st->height > content_h) content_h = st->height;
    int bbh = bwt + pt + content_h + pb + bwb;

    if (bg_i >= 0) E->items[bg_i].h = bbh;
    if (bwt) emit_rect(bx, by, bbw, bwt, st->border_col);
    if (bwb) emit_rect(bx, by + bbh - bwb, bbw, bwb, st->border_col);
    if (bwl) emit_rect(bx, by, bwl, bbh, st->border_col);
    if (bwr) emit_rect(bx + bbw - bwr, by, bwr, bbh, st->border_col);

    return by + bbh;
}

/* Rebuild the display list from the styled DOM at `width`. */
static void layout(int width) {
    if (!E) return;
    E->nitems = 0;
    E->render_len = 0;
    g_find_run = -1;
    g_layout_w = width;
    if (E->nnodes == 0) { g_doc_h = 0; E->render[0] = 0; return; }

    /* page background: body bg, else html bg, else white */
    uint32_t pbg = 0xFFFFFFFFu;
    if (E->body >= 0 && (E->nodes[E->body].st.bg >> 24)) {
        pbg = E->nodes[E->body].st.bg;
    } else {
        for (int i = 0; i < E->nnodes; i++)
            if (E->nodes[i].tag == T_HTML) {
                if (E->nodes[i].st.bg >> 24) pbg = E->nodes[i].st.bg;
                break;
            }
    }
    E->page_bg = pbg;

    int root = E->body >= 0 ? E->body : 0;
    int cw = width - 12;                  /* room for the scrollbar */
    if (cw < 60) cw = 60;
    g_nflts = 0;
    g_flt_bottom = 0;
    struct cstyle *rst = &E->nodes[root].st;
    int y = rst->m[0] == M_AUTO ? 0 : rst->m[0];
    int end = lay_block(root, 0, cw, y, -1, 0);
    g_doc_h = end + (rst->m[2] == M_AUTO ? 0 : rst->m[2]) + 10;
    g_flt_bottom = 0;
    for (int i = 0; i < g_nflts; i++)
        if (g_flts[i].y1 > g_flt_bottom) g_flt_bottom = g_flts[i].y1;
    if (g_flt_bottom + 10 > g_doc_h)
        g_doc_h = g_flt_bottom + 10;      /* floats may outgrow the flow */
    E->render[E->render_len] = 0;
}

/* ---- Engine / page reset ------------------------------------------- */

static void eng_reset(void) {
    E->nnodes = 0;
    E->nattrs = 0;
    E->tpool_len = 0;
    E->body = -1;
    E->nparts = 0;
    E->ndecls = 0;
    E->nrules = 0;
    E->csspool_len = 0;
    E->css_order = 0;
    E->nsheets = 0;
    E->nitems = 0;
    E->render_len = 0;
    E->render[0] = 0;
    E->page_bg = 0xFFFFFFFFu;
}

static void page_reset(void) {
    eng_reset();
    g_link_count = 0;
    g_title[0] = '\0';
    g_scroll_y = 0;
    g_doc_h = 0;
    g_layout_w = 0;
    g_find_run = -1;
    g_nforms = 0;
    g_nfields = 0;
    g_focus_field = -1;
    /* NOTE: caller frees decoded pixels via images_free() before reset. */
    g_nimages = 0;
}

static void resolve_relative_url(const char *base, const char *rel, char *out, int out_max);
static void images_free(void);
static void tab_images_free(struct tab *t);
static void layout(int width);
static void clamp_scroll(void);
static int  has_scheme(const char *s);
static void js_teardown(struct tab *t);

/* ---- Collect pass: interaction objects + stylesheets ---------------- *
 * Walks the DOM in document order registering links/images/forms/title
 * into the tab arrays (so all the existing interaction code works
 * unchanged), parsing <style> blocks, and fetching + parsing
 * <link rel=stylesheet> sheets. */

#define SHEET_MAX       6
#define SHEET_FETCH_CAP (256 * 1024)

static int g_form_open;          /* collect-walk state: innermost form */
static int g_js_dirty;           /* a JS primitive mutated the DOM */
static int g_js_in_load;         /* inside render_html: defer rerender */
static int g_collect_light;      /* collect pass: skip styles/forms,
                                    register only unregistered nodes */

static void collect_node(int ni) {
    struct dnode *nd = &E->nodes[ni];
    int save_form = g_form_open;
    switch (nd->tag) {
    case T_TITLE: {
        int c = nd->first;
        if (c >= 0 && E->nodes[c].tag == T_TEXT && !g_title[0]) {
            int l = E->nodes[c].tlen;
            if (l > TITLE_MAX) l = TITLE_MAX;
            int w = 0;
            for (int k = 0; k < l; k++) {
                char ch = E->tpool[E->nodes[c].toff + k];
                g_title[w++] = ((unsigned char)ch < 32) ? ' ' : ch;
            }
            g_title[w] = '\0';
        }
        break;
    }
    case T_A: {
        char href[LINK_URL_MAX];
        if (nd->link < 0 &&
            node_attr_str(nd, "href", href, sizeof(href)) && href[0] &&
            g_link_count < LINK_MAX) {
            str_copy(g_links[g_link_count], href, LINK_URL_MAX);
            nd->link = (int16_t)g_link_count++;
        }
        break;
    }
    case T_IMG: {
        char src[512];
        if (nd->img < 0 && g_nimages < IMG_MAX &&
            node_attr_str(nd, "src", src, sizeof(src)) && src[0] &&
            /* skip data: URIs cheaply */
            !(src[0] == 'd' && src[1] == 'a' && src[2] == 't' &&
              src[3] == 'a' && src[4] == ':')) {
            struct img *im = &g_images[g_nimages];
            mem_zero(im, sizeof(*im));
            resolve_relative_url(g_url, src, im->src, sizeof(im->src));
            char d[8];
            im->attr_w = node_attr_str(nd, "width", d, sizeof(d))
                             ? (int16_t)atoi_simple(d) : 0;
            im->attr_h = node_attr_str(nd, "height", d, sizeof(d))
                             ? (int16_t)atoi_simple(d) : 0;
            im->state = 0;
            nd->img = (int16_t)g_nimages++;
        }
        break;
    }
    case T_FORM:
        if (!g_collect_light && g_nforms < FORM_MAX) {
            struct form *f = &g_forms[g_nforms];
            f->first = (int16_t)g_nfields;
            f->nf = 0;
            node_attr_str(nd, "action", f->action, sizeof(f->action));
            char meth[12];
            f->post = node_attr_str(nd, "method", meth, sizeof(meth)) &&
                      (meth[0] == 'p' || meth[0] == 'P');
            g_form_open = g_nforms++;
        }
        break;
    case T_INPUT:
        if (nd->field < 0 && g_nfields < FIELD_MAX) {
            char ty[20];
            if (!node_attr_str(nd, "type", ty, sizeof(ty))) ty[0] = 0;
            int ftype = -1;
            if (!ty[0] || str_ncasecmp(ty, "text", 5) == 0 ||
                str_ncasecmp(ty, "search", 7) == 0)
                ftype = FT_TEXT;
            else if (str_ncasecmp(ty, "hidden", 7) == 0)
                ftype = FT_HIDDEN;
            else if (str_ncasecmp(ty, "submit", 7) == 0)
                ftype = FT_SUBMIT;
            if (g_collect_light && ftype != FT_TEXT)
                ftype = -1;               /* light: only new text inputs */
            if (ftype >= 0) {
                struct field *fd = &g_fields[g_nfields];
                mem_zero(fd, sizeof(*fd));
                fd->form = (int16_t)g_form_open;
                fd->type = (uint8_t)ftype;
                fd->node = (int16_t)ni;
                node_attr_str(nd, "name", fd->name, sizeof(fd->name));
                node_attr_str(nd, "value", fd->value, sizeof(fd->value));
                char szs[8];
                if (node_attr_str(nd, "size", szs, sizeof(szs)))
                    fd->size = (int16_t)atoi_simple(szs);
                if (ftype == FT_SUBMIT && !fd->value[0])
                    str_copy(fd->value, "Submit", sizeof(fd->value));
                if (ftype != FT_HIDDEN)
                    nd->field = (int16_t)g_nfields;
                if (g_form_open >= 0)     /* form-less inputs are legal */
                    g_forms[g_form_open].nf++;
                g_nfields++;
            }
        }
        break;
    case T_STYLE: {
        if (nd->flags & DNF_STYLE_DONE) break;
        nd->flags |= DNF_STYLE_DONE;      /* CSS-in-JS: light mode parses
                                             script-added styles too */
        char med[128];
        int has_med = node_attr_str(nd, "media", med, sizeof(med));
        if (!has_med || media_matches(med, (int)str_len(med))) {
            int c = nd->first;
            if (c >= 0 && E->nodes[c].tag == T_TEXT)
                css_parse_sheet(&E->tpool[E->nodes[c].toff],
                                E->nodes[c].tlen, 1);
        }
        break;
    }
    case T_LINKE: {
        if (g_collect_light) break;
        char rel[64], href[512], med[128];
        if (node_attr_str(nd, "rel", rel, sizeof(rel)) &&
            str_contains(rel, (int)str_len(rel), "stylesheet", 10) >= 0 &&
            node_attr_str(nd, "href", href, sizeof(href)) && href[0] &&
            E->nsheets < SHEET_MAX) {
            int has_med = node_attr_str(nd, "media", med, sizeof(med));
            if (!has_med || media_matches(med, (int)str_len(med))) {
                char url[URL_MAX + 1];
                resolve_relative_url(g_url, href, url, URL_MAX);
                if (has_scheme(url)) {
                    char *buf = (char *)malloc(SHEET_FETCH_CAP);
                    if (buf) {
                        struct http_fetch req;
                        mem_zero(&req, sizeof(req));
                        req.url = (unsigned long)url;
                        req.buf = (unsigned long)buf;
                        req.buf_sz = SHEET_FETCH_CAP;
                        long r = sys_http_fetch(&req);
                        if (r > 0 && req.status > 0 && req.status < 400) {
                            E->nsheets++;
                            css_parse_sheet(buf, r, 1);
                        }
                        free(buf);
                    }
                }
            }
        }
        break;
    }
    case T_SCRIPT:
        g_form_open = save_form;
        return;
    default:
        break;
    }
    for (int c = nd->first; c >= 0; c = E->nodes[c].next)
        collect_node(c);
    g_form_open = save_form;
}

/* =================== JavaScript: QuickJS + DOM bindings ===============
 * Phase 9 of the engine roadmap. Scripts run ONCE at page load (after
 * the DOM is built, before styles/layout); mutations are picked up by
 * the normal collect -> cascade -> layout pipeline. No event loop yet
 * (that is phase 10): the runtime is created per load and freed after
 * the scripts finish. C exposes small __dom primitives on ints (node
 * indices); a JS prelude wraps them in document/Element objects. */

static void set_status(const char *s);
static void update_title(void);
static int  msg_append(char *dst, int pos, int max, const char *s);
static void collect_node(int ni);
static int  js_dispatch_event(int node, const char *type);
static int  js_dispatch_key(int node, const char *type, const char *key);
static void history_push(const char *url);
static long do_navigate(const char *url);

/* ---- DOM mutation helpers ------------------------------------------ */

static void dom_unlink(int ni) {
    if (ni <= 0 || ni >= E->nnodes) return;
    int p = E->nodes[ni].parent;
    if (p < 0) return;
    int prev = -1, c = E->nodes[p].first;
    while (c >= 0 && c != ni) { prev = c; c = E->nodes[c].next; }
    if (c != ni) return;
    if (prev < 0) E->nodes[p].first = E->nodes[ni].next;
    else E->nodes[prev].next = E->nodes[ni].next;
    if (E->nodes[p].last == ni) E->nodes[p].last = prev;
    E->nodes[ni].parent = -1;
    E->nodes[ni].next = -1;
}

static void dom_attach(int parent, int child) {
    if (parent < 0 || child <= 0 || parent == child) return;
    if (parent >= E->nnodes || child >= E->nnodes) return;
    for (int a = parent; a >= 0; a = E->nodes[a].parent)
        if (a == child) return;           /* would create a cycle */
    dom_unlink(child);
    struct dnode *p = &E->nodes[parent];
    E->nodes[child].parent = parent;
    E->nodes[child].next = -1;
    if (p->last >= 0) E->nodes[p->last].next = child;
    else p->first = child;
    p->last = child;
}

static int dom_find_by_id(const char *id) {
    int idl = (int)str_len(id);
    if (!idl) return -1;
    for (int i = 1; i < E->nnodes; i++) {
        if (E->nodes[i].tag == T_TEXT) continue;
        int vlen;
        const char *v = node_attr(&E->nodes[i], "id", &vlen);
        if (v && vlen == idl) {
            int k = 0;
            while (k < idl && v[k] == id[k]) k++;
            if (k == idl) return i;
        }
    }
    return -1;
}

/* querySelector: parse one selector into scratch parts, scan the tree. */
static int dom_query_first(const char *sel, long n) {
    int p0 = E->nparts, c0 = E->csspool_len;
    struct ccur c = { sel, 0, n };
    int pending = 0, valid = 1;
    int sid = 0, scls = 0, sty = 0;
    for (;;) {
        css_ws(&c);
        if (c.i >= c.n) break;
        char ch = c.s[c.i];
        if (ch == '>') { pending = 2; c.i++; continue; }
        if (ch == '+' || ch == '~' || ch == ',') { valid = 0; break; }
        struct cpart tmp;
        long before = c.i;
        if (!css_parse_part(&c, &tmp, &sid, &scls, &sty) || c.i == before) {
            valid = 0;
            break;
        }
        tmp.comb = (uint8_t)((E->nparts == p0) ? 0 : (pending ? pending : 1));
        pending = 0;
        if (E->nparts < PART_MAX) E->parts[E->nparts++] = tmp;
        else { valid = 0; break; }
    }
    int found = -1;
    if (valid && E->nparts > p0) {
        const struct cpart *parts = &E->parts[p0];
        int np = E->nparts - p0;
        for (int i = 1; i < E->nnodes && found < 0; i++) {
            if (E->nodes[i].tag == T_TEXT) continue;
            if (match_upward(parts, np - 1, i)) found = i;
        }
    }
    E->nparts = p0;
    E->csspool_len = c0;
    return found;
}

static int dom_text_collect(int ni, char *buf, int pos, int cap) {
    struct dnode *n = &E->nodes[ni];
    if (n->tag == T_TEXT) {
        for (int k = 0; k < n->tlen && pos < cap; k++)
            buf[pos++] = E->tpool[n->toff + k];
        return pos;
    }
    for (int c = n->first; c >= 0; c = E->nodes[c].next)
        pos = dom_text_collect(c, buf, pos, cap);
    return pos;
}

/* Set/replace one attribute: the node's records are rebuilt as a fresh
 * contiguous block (old slices are shared; the arena resets per page). */
static void dom_set_attr(int ni, const char *name, const char *val) {
    if (ni <= 0 || ni >= E->nnodes) return;
    struct dnode *n = &E->nodes[ni];
    int nlen = (int)str_len(name);
    if (nlen > 48) nlen = 48;
    if (!nlen) return;
    int new0 = E->nattrs;
    int replaced = 0;
    for (int i = 0; i < n->nattr && E->nattrs < ATTR_MAX; i++) {
        struct dattr *a = &E->attrs[n->attr0 + i];
        int match = (a->nlen == nlen);
        if (match)
            for (int k = 0; k < nlen; k++)
                if (lc(E->tpool[a->noff + k]) != lc(name[k])) { match = 0; break; }
        struct dattr *d = &E->attrs[E->nattrs++];
        if (match && !replaced) {
            int noff = E->tpool_len;
            for (int k = 0; k < nlen; k++) tp_putc((char)lc(name[k]));
            int voff = E->tpool_len;
            int vlen = tp_put_utf8(val, (long)str_len(val));
            d->noff = noff; d->nlen = (int16_t)nlen;
            d->voff = voff; d->vlen = vlen;
            replaced = 1;
        } else {
            *d = *a;
        }
    }
    if (!replaced && E->nattrs < ATTR_MAX) {
        int noff = E->tpool_len;
        for (int k = 0; k < nlen; k++) tp_putc((char)lc(name[k]));
        int voff = E->tpool_len;
        int vlen = tp_put_utf8(val, (long)str_len(val));
        struct dattr *d = &E->attrs[E->nattrs++];
        d->noff = noff; d->nlen = (int16_t)nlen;
        d->voff = voff; d->vlen = vlen;
    }
    n->attr0 = new0;
    n->nattr = (int16_t)(E->nattrs - new0);
}

/* ---- __dom primitives (C side) -------------------------------------- */

static int jsi(JSContext *cx, JSValueConst v) {
    int32_t i;
    if (JS_ToInt32(cx, &i, v)) return -1;
    if (i < 0 || i >= E->nnodes) return -1;
    return i;
}

static JSValue js_console_log(JSContext *cx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    char line[512];
    int p = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(cx, argv[i]);
        if (!s) continue;
        if (i && p < 510) line[p++] = ' ';
        for (const char *q = s; *q && p < 510; q++) line[p++] = *q;
        JS_FreeCString(cx, s);
    }
    line[p++] = '\n';
    sys_write(1, "[js] ", 5);
    sys_write(1, line, (size_t)p);
    return JS_UNDEFINED;
}

static JSValue js_dom_byid(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NewInt32(cx, -1);
    const char *s = JS_ToCString(cx, argv[0]);
    int r = s ? dom_find_by_id(s) : -1;
    if (s) JS_FreeCString(cx, s);
    return JS_NewInt32(cx, r);
}

static JSValue js_dom_query(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NewInt32(cx, -1);
    const char *s = JS_ToCString(cx, argv[0]);
    int r = s ? dom_query_first(s, (long)str_len(s)) : -1;
    if (s) JS_FreeCString(cx, s);
    return JS_NewInt32(cx, r);
}

static JSValue js_dom_create(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NewInt32(cx, -1);
    const char *s = JS_ToCString(cx, argv[0]);
    int r = -1;
    if (s) {
        int tag = tag_lookup(s, (int)str_len(s));
        r = dom_new(tag, -1);
        JS_FreeCString(cx, s);
    }
    return JS_NewInt32(cx, r);
}

static JSValue js_dom_text(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NewInt32(cx, -1);
    const char *s = JS_ToCString(cx, argv[0]);
    int r = -1;
    if (s) {
        r = dom_new(T_TEXT, -1);
        if (r >= 0) {
            E->nodes[r].toff = E->tpool_len;
            E->nodes[r].tlen = tp_put_utf8(s, (long)str_len(s));
        }
        JS_FreeCString(cx, s);
    }
    return JS_NewInt32(cx, r);
}

static JSValue js_dom_append(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 2) { dom_attach(jsi(cx, argv[0]), jsi(cx, argv[1])); g_js_dirty = 1; }
    return JS_UNDEFINED;
}

static JSValue js_dom_remove(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) { dom_unlink(jsi(cx, argv[0])); g_js_dirty = 1; }
    return JS_UNDEFINED;
}

static JSValue js_dom_settext(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int ni = jsi(cx, argv[0]);
    const char *s = JS_ToCString(cx, argv[1]);
    if (ni > 0 && s) {
        g_js_dirty = 1;
        if (E->nodes[ni].tag == T_TEXT) {
            /* text node: rewrite its slice (Preact sets .data/.nodeValue) */
            E->nodes[ni].toff = E->tpool_len;
            E->nodes[ni].tlen = tp_put_utf8(s, (long)str_len(s));
        } else {
            E->nodes[ni].first = E->nodes[ni].last = -1;
            int tn = dom_new(T_TEXT, ni);
            if (tn >= 0) {
                E->nodes[tn].toff = E->tpool_len;
                E->nodes[tn].tlen = tp_put_utf8(s, (long)str_len(s));
            }
        }
    }
    if (s) JS_FreeCString(cx, s);
    return JS_UNDEFINED;
}

/* insertBefore(parent, child, ref): attach child before ref (append
 * when ref < 0). The singly-linked child list makes this a prev-scan. */
static JSValue js_dom_insbefore(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return JS_UNDEFINED;
    int pa = jsi(cx, argv[0]);
    int ch = jsi(cx, argv[1]);
    int ref = jsi(cx, argv[2]);
    if (pa < 0 || ch <= 0 || pa == ch) return JS_UNDEFINED;
    if (ref < 0 || E->nodes[ref].parent != pa) {
        dom_attach(pa, ch);
        g_js_dirty = 1;
        return JS_UNDEFINED;
    }
    for (int a = pa; a >= 0; a = E->nodes[a].parent)
        if (a == ch) return JS_UNDEFINED;  /* cycle */
    dom_unlink(ch);
    struct dnode *p = &E->nodes[pa];
    int prev = -1, c = p->first;
    while (c >= 0 && c != ref) { prev = c; c = E->nodes[c].next; }
    E->nodes[ch].parent = pa;
    E->nodes[ch].next = ref;
    if (prev < 0) p->first = ch;
    else E->nodes[prev].next = ch;
    g_js_dirty = 1;
    return JS_UNDEFINED;
}

static JSValue js_dom_removeattr(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int ni = jsi(cx, argv[0]);
    const char *name = JS_ToCString(cx, argv[1]);
    if (ni > 0 && name) {
        struct dnode *n = &E->nodes[ni];
        int nlen = (int)str_len(name);
        int new0 = E->nattrs;
        for (int i = 0; i < n->nattr && E->nattrs < ATTR_MAX; i++) {
            struct dattr *a = &E->attrs[n->attr0 + i];
            int match = (a->nlen == nlen);
            if (match)
                for (int k = 0; k < nlen; k++)
                    if (lc(E->tpool[a->noff + k]) != lc(name[k])) { match = 0; break; }
            if (!match)
                E->attrs[E->nattrs++] = *a;
        }
        n->attr0 = new0;
        n->nattr = (int16_t)(E->nattrs - new0);
        g_js_dirty = 1;
    }
    if (name) JS_FreeCString(cx, name);
    return JS_UNDEFINED;
}

/* childNodes: ALL children incl. text nodes (Preact walks these). */
static JSValue js_dom_childnodes(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    JSValue arr = JS_NewArray(cx);
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    if (ni >= 0) {
        uint32_t k = 0;
        for (int c = E->nodes[ni].first; c >= 0; c = E->nodes[c].next)
            JS_SetPropertyUint32(cx, arr, k++, JS_NewInt32(cx, c));
    }
    return arr;
}

static JSValue js_dom_nextsib(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    return JS_NewInt32(cx, ni >= 0 ? E->nodes[ni].next : -1);
}

/* pushState(url): update the address bar + history, no navigation. */
static JSValue js_dom_pushstate(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) {
        const char *u = JS_ToCString(cx, argv[0]);
        if (u && u[0]) {
            char url[URL_MAX + 1];
            resolve_relative_url(g_url, u, url, URL_MAX);
            str_copy(g_url, url, URL_MAX);
            g_url_len = (int)str_len(g_url);
            history_push(g_url);
        }
        if (u) JS_FreeCString(cx, u);
    }
    return JS_UNDEFINED;
}

/* navigate(url): DEFERRED -- performed after JS unwinds (navigation
 * tears down the running runtime). */
static JSValue js_dom_navigate(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) {
        const char *u = JS_ToCString(cx, argv[0]);
        if (u && u[0]) {
            char url[URL_MAX + 1];
            resolve_relative_url(g_url, u, url, URL_MAX);
            str_copy(cur->js_nav, url, URL_MAX);
        }
        if (u) JS_FreeCString(cx, u);
    }
    return JS_UNDEFINED;
}

static JSValue js_dom_href(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewString(cx, g_url);
}

static JSValue js_dom_gettext(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    static char buf[4096];
    if (argc < 1) return JS_NewString(cx, "");
    int ni = jsi(cx, argv[0]);
    int len = (ni >= 0) ? dom_text_collect(ni, buf, 0, (int)sizeof(buf)) : 0;
    return JS_NewStringLen(cx, buf, (size_t)len);
}

static JSValue js_dom_sethtml(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int ni = jsi(cx, argv[0]);
    const char *s = JS_ToCString(cx, argv[1]);
    if (ni > 0 && s) {
        g_js_dirty = 1;
        E->nodes[ni].first = E->nodes[ni].last = -1;
        dom_parse(s, (long)str_len(s), ni);
    }
    if (s) JS_FreeCString(cx, s);
    return JS_UNDEFINED;
}

static JSValue js_dom_getattr(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NULL;
    int ni = jsi(cx, argv[0]);
    const char *name = JS_ToCString(cx, argv[1]);
    JSValue r = JS_NULL;
    if (ni > 0 && name) {
        int vlen;
        const char *v = node_attr(&E->nodes[ni], name, &vlen);
        if (v) r = JS_NewStringLen(cx, v, (size_t)vlen);
    }
    if (name) JS_FreeCString(cx, name);
    return r;
}

static JSValue js_dom_setattr(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return JS_UNDEFINED;
    int ni = jsi(cx, argv[0]);
    const char *name = JS_ToCString(cx, argv[1]);
    const char *val = JS_ToCString(cx, argv[2]);
    if (ni > 0 && name && val) { dom_set_attr(ni, name, val); g_js_dirty = 1; }
    if (name) JS_FreeCString(cx, name);
    if (val) JS_FreeCString(cx, val);
    return JS_UNDEFINED;
}

static JSValue js_dom_tag(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    return JS_NewString(cx, ni >= 0 ? g_tag_names[E->nodes[ni].tag] : "");
}

static JSValue js_dom_parent(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    return JS_NewInt32(cx, ni >= 0 ? E->nodes[ni].parent : -1);
}

static JSValue js_dom_children(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    JSValue arr = JS_NewArray(cx);
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    if (ni >= 0) {
        uint32_t k = 0;
        for (int c = E->nodes[ni].first; c >= 0; c = E->nodes[c].next)
            if (E->nodes[c].tag != T_TEXT)
                JS_SetPropertyUint32(cx, arr, k++, JS_NewInt32(cx, c));
    }
    return arr;
}

static JSValue js_dom_body(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(cx, E->body >= 0 ? E->body : 0);
}

static JSValue js_dom_title(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) {
        const char *s = JS_ToCString(cx, argv[0]);
        if (s) {
            str_copy(g_title, s, TITLE_MAX);
            update_title();
            JS_FreeCString(cx, s);
        }
    }
    return JS_UNDEFINED;
}

/* ---- phase 10 primitives: timers, fetch, field values, dispatch ---- */

struct sys_timeval { long tv_sec; long tv_usec; };
extern int gettimeofday(struct sys_timeval *tv, void *tz);

static long js_now_ms(void) {
    struct sys_timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static JSValue js_dom_timer(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2 || !JS_IsFunction(cx, argv[0])) return JS_NewInt32(cx, 0);
    int32_t ms = 0, rep = 0;
    JS_ToInt32(cx, &ms, argv[1]);
    if (argc >= 3) JS_ToInt32(cx, &rep, argv[2]);
    if (ms < 0) ms = 0;
    for (int k = 0; k < JS_TIMER_MAX; k++) {
        struct jstimer *tm = &cur->js_timers[k];
        if (tm->used) continue;
        tm->used = 1;
        tm->id = cur->js_timer_seq++;
        tm->due_ms = js_now_ms() + ms;
        tm->interval_ms = rep ? (ms > 0 ? ms : 10) : 0;
        tm->fn = JS_DupValue(cx, argv[0]);
        return JS_NewInt32(cx, tm->id);
    }
    return JS_NewInt32(cx, 0);
}

static JSValue js_dom_untimer(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = 0;
    if (argc >= 1) JS_ToInt32(cx, &id, argv[0]);
    for (int k = 0; k < JS_TIMER_MAX; k++) {
        struct jstimer *tm = &cur->js_timers[k];
        if (tm->used && tm->id == id) {
            JS_FreeValue(cx, tm->fn);
            tm->used = 0;
            break;
        }
    }
    return JS_UNDEFINED;
}

#define JS_FETCH_CAP (256 * 1024)

/* Synchronous fetch over the kernel HTTP/TLS stack (SPAs assume async;
 * the Promise-shaped wrapper lives in the prelude -- honest blocking
 * until the kernel grows an async HTTP ABI). */
static JSValue js_dom_fetchsync(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    JSValue obj = JS_NewObject(cx);
    int status = 0;
    int have_body = 0;
    if (argc >= 1) {
        const char *u = JS_ToCString(cx, argv[0]);
        if (u) {
            char url[URL_MAX + 1];
            resolve_relative_url(g_url, u, url, URL_MAX);
            if (has_scheme(url)) {
                char *buf = (char *)malloc(JS_FETCH_CAP + 1);
                if (buf) {
                    struct http_fetch req;
                    mem_zero(&req, sizeof(req));
                    req.url = (unsigned long)url;
                    req.buf = (unsigned long)buf;
                    req.buf_sz = JS_FETCH_CAP;
                    long r = sys_http_fetch(&req);
                    if (r >= 0) {
                        status = req.status;
                        JS_SetPropertyStr(cx, obj, "body",
                            JS_NewStringLen(cx, buf, (size_t)r));
                        JS_SetPropertyStr(cx, obj, "url",
                            JS_NewString(cx, req.final_url[0] ? req.final_url : url));
                        have_body = 1;
                    }
                    free(buf);
                }
            }
            JS_FreeCString(cx, u);
        }
    }
    if (!have_body) {
        JS_SetPropertyStr(cx, obj, "body", JS_NewString(cx, ""));
        JS_SetPropertyStr(cx, obj, "url", JS_NewString(cx, ""));
    }
    JS_SetPropertyStr(cx, obj, "status", JS_NewInt32(cx, status));
    return obj;
}

static JSValue js_dom_getvalue(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    if (ni > 0 && E->nodes[ni].field >= 0)
        return JS_NewString(cx, g_fields[E->nodes[ni].field].value);
    return JS_NewString(cx, "");
}

static JSValue js_dom_setvalue(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int ni = jsi(cx, argv[0]);
    const char *s = JS_ToCString(cx, argv[1]);
    if (ni > 0 && s && E->nodes[ni].field >= 0) {
        struct field *f = &g_fields[E->nodes[ni].field];
        str_copy(f->value, s, sizeof(f->value));
        g_js_dirty = 1;
    }
    if (s) JS_FreeCString(cx, s);
    return JS_UNDEFINED;
}

static JSValue js_dom_setdispatcher(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1 && JS_IsFunction(cx, argv[0])) {
        if (cur->js_has_dispatch) JS_FreeValue(cx, cur->js_dispatch);
        cur->js_dispatch = JS_DupValue(cx, argv[0]);
        cur->js_has_dispatch = 1;
    }
    return JS_UNDEFINED;
}

/* Free a tab's JS world (navigation away / tab close). */
static void js_teardown(struct tab *t) {
    if (!t->js_cx) { t->js_rt = NULL; return; }
    JSContext *cx = t->js_cx;
    for (int k = 0; k < JS_TIMER_MAX; k++)
        if (t->js_timers[k].used) {
            JS_FreeValue(cx, t->js_timers[k].fn);
            t->js_timers[k].used = 0;
        }
    if (t->js_has_dispatch) JS_FreeValue(cx, t->js_dispatch);
    t->js_has_dispatch = 0;
    JS_FreeContext(cx);
    JS_FreeRuntime(t->js_rt);
    t->js_cx = NULL;
    t->js_rt = NULL;
}

static void js_drain_jobs(struct tab *t);

/* ---- prelude: document/Element wrappers over __dom ------------------ */

static const char JS_PRELUDE[] =
"(function(g){\n"
"  var D = g.__dom;\n"
"  var wrap = new Map();\n"
"  function Element(i){\n"
"    this.__i = i;\n"
"    var self = this;\n"
"    this.style = new Proxy({}, {\n"
"      get: function(tg, k){\n"
"        if (k === 'setProperty')\n"
"          return function(n, v){ self.style[String(n).replace(/-([a-z])/g,\n"
"            function(m, c){ return c.toUpperCase(); })] = v; };\n"
"        if (k === 'removeProperty') return function(n){};\n"
"        if (k === 'cssText') return '';\n"
"        return tg[k];\n"
"      },\n"
"      set: function(tg, k, v){\n"
"        if (k === 'cssText'){\n"
"          D.setAttr(self.__i, 'style', String(v));\n"
"          return true;\n"
"        }\n"
"        tg[k] = v;\n"
"        var s = '';\n"
"        for (var kk in tg){\n"
"          var name = '';\n"
"          for (var ci = 0; ci < kk.length; ci++){\n"
"            var ch = kk[ci];\n"
"            name += (ch >= 'A' && ch <= 'Z') ? '-' + ch.toLowerCase() : ch;\n"
"          }\n"
"          s += name + ':' + tg[kk] + ';';\n"
"        }\n"
"        D.setAttr(self.__i, 'style', s);\n"
"        return true;\n"
"      }\n"
"    });\n"
"  }\n"
"  function el(i){\n"
"    if (i === null || i === undefined || i < 0) return null;\n"
"    var w = wrap.get(i);\n"
"    if (!w){ w = new Element(i); wrap.set(i, w); }\n"
"    return w;\n"
"  }\n"
"  Object.defineProperties(Element.prototype, {\n"
"    textContent: {\n"
"      get: function(){ return D.getText(this.__i); },\n"
"      set: function(v){ D.setText(this.__i, String(v)); }\n"
"    },\n"
"    innerText: {\n"
"      get: function(){ return D.getText(this.__i); },\n"
"      set: function(v){ D.setText(this.__i, String(v)); }\n"
"    },\n"
"    innerHTML: {\n"
"      get: function(){ return ''; },\n"
"      set: function(v){ D.setHTML(this.__i, String(v)); }\n"
"    },\n"
"    id: {\n"
"      get: function(){ return D.getAttr(this.__i, 'id') || ''; },\n"
"      set: function(v){ D.setAttr(this.__i, 'id', String(v)); }\n"
"    },\n"
"    className: {\n"
"      get: function(){ return D.getAttr(this.__i, 'class') || ''; },\n"
"      set: function(v){ D.setAttr(this.__i, 'class', String(v)); }\n"
"    },\n"
"    tagName: { get: function(){ return D.tag(this.__i).toUpperCase(); } },\n"
"    nodeName: { get: function(){ return D.tag(this.__i).toUpperCase(); } },\n"
"    localName: { get: function(){ return D.tag(this.__i); } },\n"
"    nodeType: { get: function(){ return D.tag(this.__i) === '#text' ? 3 : 1; } },\n"
"    parentNode: { get: function(){ return el(D.parent(this.__i)); } },\n"
"    children: { get: function(){ return D.children(this.__i).map(el); } },\n"
"    childNodes: { get: function(){ return D.childNodes(this.__i).map(el); } },\n"
"    firstChild: { get: function(){\n"
"      var c = D.childNodes(this.__i);\n"
"      return c.length ? el(c[0]) : null;\n"
"    } },\n"
"    lastChild: { get: function(){\n"
"      var c = D.childNodes(this.__i);\n"
"      return c.length ? el(c[c.length - 1]) : null;\n"
"    } },\n"
"    nextSibling: { get: function(){ return el(D.nextSib(this.__i)); } },\n"
"    nodeValue: {\n"
"      get: function(){ return D.getText(this.__i); },\n"
"      set: function(v){ D.setText(this.__i, String(v)); }\n"
"    },\n"
"    data: {\n"
"      get: function(){ return D.getText(this.__i); },\n"
"      set: function(v){ D.setText(this.__i, String(v)); }\n"
"    },\n"
"    classList: { get: function(){\n"
"      var self = this;\n"
"      function toks(){\n"
"        return (D.getAttr(self.__i, 'class') || '').split(/\\s+/).filter(Boolean);\n"
"      }\n"
"      return {\n"
"        add: function(c){ var t = toks(); if (t.indexOf(c) < 0){ t.push(c); D.setAttr(self.__i, 'class', t.join(' ')); } },\n"
"        remove: function(c){ D.setAttr(self.__i, 'class', toks().filter(function(x){ return x !== c; }).join(' ')); },\n"
"        toggle: function(c){ var t = toks(); var ix = t.indexOf(c); if (ix < 0) t.push(c); else t.splice(ix, 1); D.setAttr(self.__i, 'class', t.join(' ')); },\n"
"        contains: function(c){ return toks().indexOf(c) >= 0; }\n"
"      };\n"
"    } },\n"
"    ownerDocument: { get: function(){ return g.document; } }\n"
"  });\n"
"  Element.prototype.appendChild = function(c){ D.append(this.__i, c.__i); return c; };\n"
"  Element.prototype.removeChild = function(c){ D.remove(c.__i); return c; };\n"
"  Element.prototype.remove = function(){ D.remove(this.__i); };\n"
"  Element.prototype.setAttribute = function(n, v){ D.setAttr(this.__i, String(n), String(v)); };\n"
"  Element.prototype.getAttribute = function(n){ return D.getAttr(this.__i, String(n)); };\n"
"  Element.prototype.querySelector = function(s){ return el(D.query(String(s))); };\n"
"  Element.prototype.insertBefore = function(c, ref){\n"
"    D.insBefore(this.__i, c.__i, ref ? ref.__i : -1);\n"
"    return c;\n"
"  };\n"
"  Element.prototype.replaceChild = function(nc, oc){\n"
"    D.insBefore(this.__i, nc.__i, oc.__i);\n"
"    D.remove(oc.__i);\n"
"    return oc;\n"
"  };\n"
"  Element.prototype.removeAttribute = function(n){ D.removeAttr(this.__i, String(n)); };\n"
"  Element.prototype.hasAttribute = function(n){ return D.getAttr(this.__i, String(n)) !== null; };\n"
"  Element.prototype.contains = function(o){\n"
"    for (var n = o; n; n = n.parentNode) if (n.__i === this.__i) return true;\n"
"    return false;\n"
"  };\n"
"  Element.prototype.addEventListener = function(t, f){\n"
"    t = String(t).toLowerCase();\n"
"    if (!this.__lst) this.__lst = {};\n"
"    (this.__lst[t] = this.__lst[t] || []).push(f);\n"
"  };\n"
"  Element.prototype.removeEventListener = function(t, f){\n"
"    t = String(t).toLowerCase();\n"
"    var a = this.__lst && this.__lst[t];\n"
"    if (a){ var ix = a.indexOf(f); if (ix >= 0) a.splice(ix, 1); }\n"
"  };\n"
"  ['click','dblclick','input','change','keydown','keyup','keypress',\n"
"   'submit','focus','blur','focusin','focusout','mousedown','mouseup',\n"
"   'mousemove','mouseover','mouseout','mouseenter','mouseleave','load',\n"
"   'error','scroll','wheel','contextmenu','pointerdown','pointerup',\n"
"   'pointermove','touchstart','touchend','touchmove','drag','drop',\n"
"   'animationend','transitionend'].forEach(function(t){\n"
"    Element.prototype['on' + t] = null;\n"
"  });\n"
"  Element.prototype.click = function(){ dispatch(this.__i, 'click'); };\n"
"  Element.prototype.focus = function(){};\n"
"  Object.defineProperty(Element.prototype, 'value', {\n"
"    get: function(){ return D.getValue(this.__i); },\n"
"    set: function(v){ D.setValue(this.__i, String(v)); }\n"
"  });\n"
"  var docListeners = {};\n"
"  function dispatch(i, type, key){\n"
"    var pd = false, stop = false;\n"
"    var evt = {\n"
"      type: type,\n"
"      key: key || '',\n"
"      keyCode: (key && key.length === 1) ? key.charCodeAt(0)\n"
"               : (key === 'Enter' ? 13 : key === 'Backspace' ? 8\n"
"               : key === 'Escape' ? 27 : key === 'Tab' ? 9 : 0),\n"
"      target: el(i >= 0 ? i : D.body()),\n"
"      currentTarget: null,\n"
"      bubbles: true,\n"
"      preventDefault: function(){ pd = true; },\n"
"      stopPropagation: function(){ stop = true; },\n"
"      stopImmediatePropagation: function(){ stop = true; }\n"
"    };\n"
"    if (i < 0){\n"
"      var ls = docListeners[type] || [];\n"
"      for (var k = 0; k < ls.length; k++){\n"
"        try { ls[k].call(g.document, evt); }\n"
"        catch(e){ g.console.log('[evt-err]', e); }\n"
"      }\n"
"      return pd;\n"
"    }\n"
"    var n = i;\n"
"    while (n >= 0 && !stop){\n"
"      var w = el(n);\n"
"      evt.currentTarget = w;\n"
"      var ls = (w.__lst && w.__lst[type]) || [];\n"
"      for (var k = 0; k < ls.length && !stop; k++){\n"
"        try { ls[k].call(w, evt); }\n"
"        catch(e){ g.console.log('[evt-err]', e); }\n"
"      }\n"
"      var oc = D.getAttr(n, 'on' + type);\n"
"      if (oc){\n"
"        try { (new Function('event', oc)).call(w, evt); }\n"
"        catch(e){ g.console.log('[evt-err]', e); }\n"
"      }\n"
"      n = D.parent(n);\n"
"    }\n"
"    return pd;\n"
"  }\n"
"  D.setDispatcher(dispatch);\n"
"  g.document = {\n"
"    getElementById: function(id){ return el(D.byId(String(id))); },\n"
"    querySelector: function(s){ return el(D.query(String(s))); },\n"
"    createElement: function(t){ return el(D.create(String(t))); },\n"
"    createTextNode: function(t){ return el(D.text(String(t))); },\n"
"    addEventListener: function(t, f){\n"
"      (docListeners[t] = docListeners[t] || []).push(f);\n"
"    },\n"
"    removeEventListener: function(){},\n"
"    createElementNS: function(ns, t){ return el(D.create(String(t))); },\n"
"    createDocumentFragment: function(){ return el(D.create('div')); },\n"
"    get documentElement(){ return el(D.body()); },\n"
"    get body(){ return el(D.body()); },\n"
"    set title(v){ D.title(String(v)); },\n"
"    get title(){ return ''; }\n"
"  };\n"
"  g.window = g;\n"
"  g.window.addEventListener = g.document.addEventListener;\n"
"  g.alert = function(m){ g.console.log('[alert]', m); };\n"
"  g.setTimeout = function(f, ms){\n"
"    return (typeof f === 'function') ? D.timer(f, ms|0, 0) : 0;\n"
"  };\n"
"  g.setInterval = function(f, ms){\n"
"    return (typeof f === 'function') ? D.timer(f, (ms|0) || 10, 1) : 0;\n"
"  };\n"
"  g.clearTimeout = g.clearInterval = function(id){ D.untimer(id|0); };\n"
"  g.queueMicrotask = function(f){ Promise.resolve().then(f); };\n"
"  g.requestAnimationFrame = function(f){\n"
"    return g.setTimeout(function(){ f(0); }, 16);\n"
"  };\n"
"  g.fetch = function(u){\n"
"    return new Promise(function(res, rej){\n"
"      var r = D.fetchSync(String(u));\n"
"      if (!r || r.status <= 0){ rej(new Error('fetch failed')); return; }\n"
"      res({\n"
"        ok: r.status >= 200 && r.status < 300,\n"
"        status: r.status,\n"
"        url: r.url,\n"
"        text: function(){ return Promise.resolve(r.body); },\n"
"        json: function(){ return Promise.resolve(JSON.parse(r.body)); }\n"
"      });\n"
"    });\n"
"  };\n"
"  g.XMLHttpRequest = function(){\n"
"    this.readyState = 0; this.status = 0; this.responseText = '';\n"
"  };\n"
"  g.XMLHttpRequest.prototype.open = function(m, u){ this.__u = u; this.readyState = 1; };\n"
"  g.XMLHttpRequest.prototype.setRequestHeader = function(){};\n"
"  g.XMLHttpRequest.prototype.send = function(){\n"
"    var r = D.fetchSync(String(this.__u));\n"
"    this.status = r ? r.status : 0;\n"
"    this.responseText = r ? r.body : '';\n"
"    this.readyState = 4;\n"
"    var sf = this;\n"
"    g.setTimeout(function(){\n"
"      if (sf.onreadystatechange) sf.onreadystatechange();\n"
"      if (sf.onload) sf.onload();\n"
"    }, 0);\n"
"  };\n"
"  g.navigator = { userAgent: 'TobyOS/4.0 (tobyOS x86_64) QuickJS' };\n"
"  function parseUrl(h){\n"
"    var m = /^(https?:)\\/\\/([^\\/?#]*)([^?#]*)(\\??[^#]*)(#?.*)$/.exec(h) || [];\n"
"    return { protocol: m[1] || '', host: m[2] || '', pathname: m[3] || '/',\n"
"             search: m[4] || '', hash: m[5] || '' };\n"
"  }\n"
"  g.location = {\n"
"    get href(){ return D.href(); },\n"
"    set href(v){ D.navigate(String(v)); },\n"
"    get protocol(){ return parseUrl(D.href()).protocol; },\n"
"    get host(){ return parseUrl(D.href()).host; },\n"
"    get hostname(){ return parseUrl(D.href()).host.split(':')[0]; },\n"
"    get pathname(){ return parseUrl(D.href()).pathname; },\n"
"    get search(){ return parseUrl(D.href()).search; },\n"
"    get hash(){ return parseUrl(D.href()).hash; },\n"
"    get origin(){ var u = parseUrl(D.href()); return u.protocol + '//' + u.host; },\n"
"    assign: function(u){ D.navigate(String(u)); },\n"
"    replace: function(u){ D.navigate(String(u)); },\n"
"    reload: function(){ D.navigate(D.href()); },\n"
"    toString: function(){ return D.href(); }\n"
"  };\n"
"  g.history = {\n"
"    pushState: function(st, ti, u){ if (u) D.pushState(String(u)); },\n"
"    replaceState: function(st, ti, u){ if (u) D.pushState(String(u)); },\n"
"    back: function(){},\n"
"    forward: function(){},\n"
"    state: null\n"
"  };\n"
"  function mkStorage(){\n"
"    var st = {};\n"
"    var o = {\n"
"      getItem: function(k){ return (k in st) ? st[k] : null; },\n"
"      setItem: function(k, v){ st[String(k)] = String(v); },\n"
"      removeItem: function(k){ delete st[k]; },\n"
"      clear: function(){ st = {}; },\n"
"      key: function(i){ return Object.keys(st)[i] || null; }\n"
"    };\n"
"    Object.defineProperty(o, 'length',\n"
"      { get: function(){ return Object.keys(st).length; } });\n"
"    return o;\n"
"  }\n"
"  g.localStorage = mkStorage();\n"
"  g.sessionStorage = mkStorage();\n"
"})(globalThis);\n";

/* ---- run every <script> at load -------------------------------------- */

#define JS_SRC_CAP (384 * 1024)

static void js_dump_error(JSContext *cx) {
    JSValue e = JS_GetException(cx);
    const char *s = JS_ToCString(cx, e);
    if (s) {
        char m[120];
        int p = 0;
        p = msg_append(m, p, sizeof(m), "JS error: ");
        p = msg_append(m, p, sizeof(m), s);
        set_status(m);
        sys_write(1, "[js] ERROR: ", 12);
        sys_write(1, s, str_len(s));
        sys_write(1, "\n", 1);
        JS_FreeCString(cx, s);
    }
    JS_FreeValue(cx, e);
}

/* Create the tab's persistent JS world (bindings + prelude). */
static int js_ensure(void) {
    if (cur->js_cx) return 1;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return 0;
    JS_SetMemoryLimit(rt, 32u * 1024 * 1024);
    JS_SetMaxStackSize(rt, 192 * 1024);
    JSContext *cx = JS_NewContext(rt);
    if (!cx) { JS_FreeRuntime(rt); return 0; }
    cur->js_rt = rt;
    cur->js_cx = cx;
    cur->js_timer_seq = 1;

    {
        JSValue g = JS_GetGlobalObject(cx);
        JSValue dom = JS_NewObject(cx);
        static const struct { const char *n; JSCFunction *f; int na; } B[] = {
            {"byId", js_dom_byid, 1},     {"query", js_dom_query, 1},
            {"create", js_dom_create, 1}, {"text", js_dom_text, 1},
            {"append", js_dom_append, 2}, {"remove", js_dom_remove, 1},
            {"setText", js_dom_settext, 2},{"getText", js_dom_gettext, 1},
            {"setHTML", js_dom_sethtml, 2},{"getAttr", js_dom_getattr, 2},
            {"setAttr", js_dom_setattr, 3},{"tag", js_dom_tag, 1},
            {"parent", js_dom_parent, 1}, {"children", js_dom_children, 1},
            {"body", js_dom_body, 0},     {"title", js_dom_title, 1},
            {"timer", js_dom_timer, 3},   {"untimer", js_dom_untimer, 1},
            {"fetchSync", js_dom_fetchsync, 1},
            {"getValue", js_dom_getvalue, 1},
            {"setValue", js_dom_setvalue, 2},
            {"setDispatcher", js_dom_setdispatcher, 1},
            {"insBefore", js_dom_insbefore, 3},
            {"removeAttr", js_dom_removeattr, 2},
            {"childNodes", js_dom_childnodes, 1},
            {"nextSib", js_dom_nextsib, 1},
            {"pushState", js_dom_pushstate, 1},
            {"navigate", js_dom_navigate, 1},
            {"href", js_dom_href, 0},
        };
        for (unsigned k = 0; k < sizeof(B) / sizeof(B[0]); k++)
            JS_SetPropertyStr(cx, dom, B[k].n,
                              JS_NewCFunction(cx, B[k].f, B[k].n, B[k].na));
        JS_SetPropertyStr(cx, g, "__dom", dom);
        JSValue cons = JS_NewObject(cx);
        JS_SetPropertyStr(cx, cons, "log",
                          JS_NewCFunction(cx, js_console_log, "log", 1));
        JS_SetPropertyStr(cx, cons, "warn",
                          JS_NewCFunction(cx, js_console_log, "warn", 1));
        JS_SetPropertyStr(cx, cons, "error",
                          JS_NewCFunction(cx, js_console_log, "error", 1));
        JS_SetPropertyStr(cx, g, "console", cons);
        JS_FreeValue(cx, g);
    }
    {
        JSValue r = JS_Eval(cx, JS_PRELUDE, sizeof(JS_PRELUDE) - 1,
                            "<prelude>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) js_dump_error(cx);
        JS_FreeValue(cx, r);
    }
    return 1;
}

static void run_scripts(void) {
    int scripts[64];
    int ns = 0;
    for (int i = 1; i < E->nnodes && ns < 64; i++)
        if (E->nodes[i].tag == T_SCRIPT) scripts[ns++] = i;
    if (!ns) return;
    if (!js_ensure()) return;
    JSContext *cx = cur->js_cx;

    for (int k = 0; k < ns; k++) {
        struct dnode *nd = &E->nodes[scripts[k]];
        char ty[40];
        if (node_attr_str(nd, "type", ty, sizeof(ty)) && ty[0] &&
            str_contains(ty, (int)str_len(ty), "javascript", 10) < 0 &&
            str_contains(ty, (int)str_len(ty), "module", 6) < 0)
            continue;                     /* JSON/templates/etc. */
        /* JS_Eval requires a NUL-terminated buffer (the lexer relies on
         * the sentinel), so scripts always run from an owned copy. */
        char *fbuf = NULL;
        long slen = 0;
        char surl[URL_MAX + 1];
        if (node_attr_str(nd, "src", surl, sizeof(surl)) && surl[0]) {
            char url[URL_MAX + 1];
            resolve_relative_url(g_url, surl, url, URL_MAX);
            if (has_scheme(url)) {
                fbuf = (char *)malloc(JS_SRC_CAP + 1);
                if (fbuf) {
                    struct http_fetch req;
                    mem_zero(&req, sizeof(req));
                    req.url = (unsigned long)url;
                    req.buf = (unsigned long)fbuf;
                    req.buf_sz = JS_SRC_CAP;
                    long r = sys_http_fetch(&req);
                    if (r > 0 && req.status > 0 && req.status < 400)
                        slen = r;
                }
            }
        } else if (nd->first >= 0 && E->nodes[nd->first].tag == T_TEXT) {
            long tl = E->nodes[nd->first].tlen;
            fbuf = (char *)malloc((unsigned long)tl + 1);
            if (fbuf) {
                for (long q = 0; q < tl; q++)
                    fbuf[q] = E->tpool[E->nodes[nd->first].toff + q];
                slen = tl;
            }
        }
        if (fbuf && slen > 0) {
            fbuf[slen] = '\0';
            JSValue r = JS_Eval(cx, fbuf, (size_t)slen, "script",
                                JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(r)) js_dump_error(cx);
            JS_FreeValue(cx, r);
        }
        if (fbuf) free(fbuf);
    }

    /* microtasks queued during load, then the document lifecycle
     * events (the runtime stays alive: phase 10) */
    js_drain_jobs(cur);
    js_dispatch_event(-1, "DOMContentLoaded");
    js_dispatch_event(-1, "load");
}

/* ---- Phase 10: the event loop half ---------------------------------- */

static void js_drain_jobs(struct tab *t) {
    if (!t->js_rt) return;
    for (int i = 0; i < 256; i++) {
        JSContext *cx1;
        int r = JS_ExecutePendingJob(t->js_rt, &cx1);
        if (r <= 0) {
            if (r < 0) js_dump_error(cx1);
            break;
        }
    }
}

/* JS mutated the DOM: register new links/images (light collect),
 * re-cascade (class/style attribute changes), re-layout. The CSSOM is
 * NOT rebuilt: stylesheets added by scripts after load are ignored. */
static void js_rerender(void) {
    g_collect_light = 1;
    g_form_open = -1;
    collect_node(0);
    g_collect_light = 0;
    struct cstyle base;
    st_init(&base, NULL);
    base.disp = D_BLOCK;
    style_node(0, &base);
    layout(g_win_w);
    clamp_scroll();
}

/* Call the prelude dispatcher: bubble `type` from `node` (or document
 * level when node < 0), with a key payload for keyboard events.
 * Returns 1 when preventDefault() was called. */
static int js_dispatch_key(int node, const char *type, const char *key) {
    if (!cur->js_cx || !cur->js_has_dispatch) return 0;
    JSContext *cx = cur->js_cx;
    JSValue args[3] = { JS_NewInt32(cx, node), JS_NewString(cx, type),
                        JS_NewString(cx, key ? key : "") };
    JSValue r = JS_Call(cx, cur->js_dispatch, JS_UNDEFINED, 3, args);
    int prevented = 0;
    if (JS_IsException(r)) js_dump_error(cx);
    else prevented = JS_ToBool(cx, r);
    JS_FreeValue(cx, r);
    JS_FreeValue(cx, args[0]);
    JS_FreeValue(cx, args[1]);
    JS_FreeValue(cx, args[2]);
    js_drain_jobs(cur);
    if (g_js_dirty && !g_js_in_load) {
        g_js_dirty = 0;
        js_rerender();
    }
    return prevented;
}

static int js_dispatch_event(int node, const char *type) {
    return js_dispatch_key(node, type, "");
}

/* Key byte -> DOM KeyboardEvent.key name ("" = do not dispatch). */
static const char *js_key_name(uint8_t key, char one[2]) {
    switch (key) {
    case 0x0A: case 0x0D: return "Enter";
    case 0x08: case 0x7F: return "Backspace";
    case 0x1B: return "Escape";
    case 0x09: return "Tab";
    case 0x80: return "ArrowUp";
    case 0x81: return "ArrowDown";
    case 0x82: return "ArrowLeft";
    case 0x83: return "ArrowRight";
    case 0x84: return "Home";
    case 0x85: return "End";
    case 0x86: return "Delete";
    }
    if (key >= 0x20 && key <= 0x7E) {
        one[0] = (char)key;
        one[1] = 0;
        return one;
    }
    return "";
}

/* Execute a location.href navigation once JS has fully unwound. */
static void js_do_pending_nav(void) {
    if (!cur->js_nav[0]) return;
    char url[URL_MAX + 1];
    str_copy(url, cur->js_nav, URL_MAX);
    cur->js_nav[0] = 0;
    do_navigate(url);
}

/* Main-loop tick: run due timers + pending jobs for every tab with a
 * live JS world (g_active flips like the image loader so the __dom
 * primitives address the right engine). Returns 1 if work was done. */
static int js_pump_all(void) {
    int work = 0;
    for (int ti = 0; ti < g_ntabs; ti++) {
        struct tab *t = &g_tabs[ti];
        if (!t->js_cx) continue;
        int save = g_active;
        g_active = ti;
        long now = js_now_ms();
        for (int k = 0; k < JS_TIMER_MAX; k++) {
            struct jstimer *tm = &t->js_timers[k];
            if (!tm->used || now < tm->due_ms) continue;
            JSValue fn = JS_DupValue(t->js_cx, tm->fn);
            if (tm->interval_ms > 0) {
                tm->due_ms = now + tm->interval_ms;
            } else {
                JS_FreeValue(t->js_cx, tm->fn);
                tm->used = 0;
            }
            JSValue r = JS_Call(t->js_cx, fn, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(r)) js_dump_error(t->js_cx);
            JS_FreeValue(t->js_cx, r);
            JS_FreeValue(t->js_cx, fn);
            work = 1;
        }
        js_drain_jobs(t);
        if (g_js_dirty) {
            g_js_dirty = 0;
            js_rerender();
            tk_redraw(&win);
            work = 1;
        }
        if (cur->js_nav[0]) {             /* location.href assignment */
            js_do_pending_nav();
            tk_redraw(&win);
            work = 1;
        }
        g_active = save;
    }
    return work;
}

/* ---- The full pipeline: raw -> DOM -> CSSOM -> style -> layout ------ */

static void render_html(void) {
    if (!E) return;
    js_teardown(cur);                     /* the old page's JS world dies */
    images_free();
    page_reset();

    dom_build();
    g_js_in_load = 1;                     /* pipeline restyles anyway */
    run_scripts();                        /* phase 9/10: mutate, then style */
    g_js_in_load = 0;
    g_js_dirty = 0;
    css_parse_sheet(UA_SHEET, (long)sizeof(UA_SHEET) - 1, 0);
    g_form_open = -1;
    collect_node(0);

    struct cstyle base;
    st_init(&base, NULL);
    base.disp = D_BLOCK;
    style_node(0, &base);

    g_view_mode = VIEW_HTML;
    layout(g_win_w);
}

/* Whole-buffer monospace document (plain text + source view):
 * synthesize doc > body > pre > #text and run the normal pipeline. */
static void render_mono_doc(void) {
    if (!E) return;
    js_teardown(cur);
    images_free();
    page_reset();

    int root = dom_new(T_UNK, -1);
    int body = dom_new(T_BODY, root);
    int pre = dom_new(T_PRE, body);
    E->body = body;
    int toff = E->tpool_len;
    for (long i = 0; i < g_raw_len && E->tpool_len < TPOOL_CAP - 2; i++) {
        char c = g_raw[i];
        if (c == '\n') tp_putc('\n');
        else if (c == '\t') { tp_putc(' '); tp_putc(' '); }
        else if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E)
            tp_putc(c);
        else if ((unsigned char)c > 0x7E)
            tp_putc('.');
    }
    int tn = dom_new(T_TEXT, pre);
    if (tn >= 0) {
        E->nodes[tn].toff = toff;
        E->nodes[tn].tlen = E->tpool_len - toff;
    }
    css_parse_sheet(UA_SHEET, (long)sizeof(UA_SHEET) - 1, 0);
    struct cstyle base;
    st_init(&base, NULL);
    base.disp = D_BLOCK;
    style_node(0, &base);
    layout(g_win_w);
}

static void render_plain_text(void) {
    render_mono_doc();
    str_copy(g_title, "Plain Text", TITLE_MAX);
    g_view_mode = VIEW_PLAIN;
}

/* Source view: show raw HTML */
static void render_source_view(void) {
    render_mono_doc();
    str_copy(g_title, "Source View", TITLE_MAX);
    g_view_mode = VIEW_SOURCE;
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

/* Reflect the page title in the WM title bar (Chrome-style). Safe to
 * call before the window exists (the kernel ignores a bad fd). */
static void update_title(void) {
    char t[TITLE_MAX + 24];
    const char *name = g_title[0] ? g_title : "New Tab";
    int p = 0;
    while (name[p] && p < TITLE_MAX) { t[p] = name[p]; p++; }
    const char *suf = " - TobyOS Browser";
    for (int i = 0; suf[i] && p < (int)sizeof(t) - 1; i++) t[p++] = suf[i];
    t[p] = '\0';
    sys_gui_set_title(win.fd, t);
}

static long do_navigate(const char *url);

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
        out[pos] = 0;
    } else {
        int last_slash = origin_end;
        for (int i = origin_end; base[i]; i++)
            if (base[i] == '/') last_slash = i;

        int pos = 0;
        for (int i = 0; i <= last_slash && pos < out_max - 1; i++)
            out[pos++] = base[i];
        for (int i = 0; rel[i] && pos < out_max - 1; i++)
            out[pos++] = rel[i];
        out[pos] = 0;
    }
}
/* Append `s` to out[*pos] percent-encoded (form/query component). */
static void url_encode_append(const char *s, char *out, int *pos, int out_max) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; s[i] && *pos < out_max - 4; i++) {
        char c = s[i];
        if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) ||
            (c >= 48 && c <= 57) ||
            c == 45 || c == 95 || c == 46 || c == 126) {
            out[(*pos)++] = c;
        } else if (c == 32) {
            out[(*pos)++] = 43;              /* '+' */
        } else {
            out[(*pos)++] = 37;              /* '%' */
            out[(*pos)++] = hex[((unsigned char)c >> 4) & 0xF];
            out[(*pos)++] = hex[(unsigned char)c & 0xF];
        }
    }
    out[*pos] = 0;
}

static void build_search_url(const char *base, const char *q,
                             char *out, int out_max) {
    int pos = 0;
    while (base[pos] && pos < out_max - 4) { out[pos] = base[pos]; pos++; }
    out[pos] = 0;
    url_encode_append(q, out, &pos, out_max);
}

static void set_home_page(void) {
    const char *html =
        "<html><head><title>New Tab</title>"
        "<style>"
        "body{background-color:#ffffff;color:#202124}"
        ".hero{background-color:#f8f9fa;border:1px solid #dadce0;"
        "padding:14px;margin:4px 0 12px;text-align:center}"
        ".hero h1{color:#1a73e8;margin:4px 0}"
        ".hero p{color:#5f6368;margin:4px 0}"
        ".card{border:1px solid #dadce0;background-color:#fbfbfc;"
        "padding:4px 12px 8px;margin:10px 0}"
        ".card h2{font-size:17px;color:#1a73e8;margin:8px 0 4px}"
        "kbd{font-family:monospace;background-color:#eef1f5;"
        "border:1px solid #d3d8de;padding:0 3px}"
        ".foot{color:#80868b;font-size:12px;text-align:center}"
        "</style></head><body>"
        "<div class=hero>"
        "<h1>TobyOS Browser</h1>"
        "<p>Now with a real engine: DOM tree + CSS cascade + box layout.</p>"
        "</div>"
        "<div class=card>"
        "<h2>Quick Start</h2>"
        "<ul>"
        "<li>Press <b>Tab</b> to focus the address bar</li>"
        "<li>Type a URL (https tried first) or search terms and press Enter</li>"
        "<li>Supports HTTP and HTTPS, follows redirects</li>"
        "<li>Renders images (PNG/JPEG/GIF/BMP), forms, and links</li>"
        "<li>Tabs: click + or Ctrl+T to open, Ctrl+W close, Ctrl+N/P switch</li>"
        "<li>Use <b>j/k</b> to scroll, <b>d/u</b> for page scroll</li>"
        "<li>Press <b>[</b> to go back, <b>]</b> to go forward</li>"
        "<li>Click links or type link number + Enter</li>"
        "<li>Press <b>r</b> to refresh, <b>h</b> for home, <b>e</b> to edit a form field</li>"
        "</ul>"
        "</div><div class=card>"
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
        "<li><b>M</b> - Maximize window</li>"
        "<li><b>S</b> - Toggle page source view</li>"
        "<li><b>q</b> - Quit browser</li>"
        "</ul>"
        "</div><div class=card>"
        "<h2>Mouse Support</h2>"
        "<ul>"
        "<li>Click navigation buttons (Back, Forward, Refresh, Home)</li>"
        "<li>Click URL bar to focus it</li>"
        "<li>Click on numbered links to follow them</li>"
        "<li>Click scrollbar to jump to position</li>"
        "</ul>"
        "</div><div class=card>"
        "<h2>Engine Features</h2>"
        "<ul>"
        "<li>DOM tree + CSS cascade: <code>&lt;style&gt;</code>, "
        "<code>style=</code>, linked stylesheets</li>"
        "<li>Selectors (type/class/id/descendant), specificity, inheritance</li>"
        "<li>Box model: margins, padding, borders, backgrounds, text-align</li>"
        "<li>HTTP and HTTPS (kernel TLS 1.3), cookies, gzip</li>"
        "<li>Images, forms, tabs, find in page, source view</li>"
        "</ul>"
        "</div>"
        "<hr>"
        "<p class=foot>TobyOS Browser v4.0 - DOM + CSS engine on the "
        "tobyOS kernel HTTP/HTTPS stack.</p>"
        "</body></html>";

    g_raw_len = 0;
    while (html[g_raw_len]) { g_raw[g_raw_len] = html[g_raw_len]; g_raw_len++; }
    g_raw[g_raw_len] = '\0';

    render_html();
    g_source_view = 0;
    update_title();
    set_status("Ready");
}

/* ---- Tab management --------------------------------------------- */

static void tab_reset(struct tab *t) {
    tab_images_free(t);
    js_teardown(t);
    struct eng *e = t->eng;    /* the heap engine survives tab resets */
    mem_zero(t, sizeof(*t));
    if (!e) e = (struct eng *)malloc(sizeof(struct eng));
    t->eng = e;
    if (e) {                   /* safe defaults until the first render */
        e->nnodes = 0;
        e->nitems = 0;
        e->render_len = 0;
        e->render[0] = 0;
        e->page_bg = 0xFFFFFFFFu;
    }
    t->hist_pos     = -1;
    t->focus_url    = 1;
    t->view_mode    = VIEW_HTML;
    t->find_run     = -1;
    t->focus_field  = -1;
    t->used         = 1;
}

/* Open a fresh tab on the home page and make it active. No-op (just
 * switches to the last tab) when the tab array is full. */
static void tab_open(void) {
    if (g_ntabs >= TAB_MAX) { set_status("Tab limit reached"); return; }
    int idx = g_ntabs++;
    g_active = idx;
    tab_reset(&g_tabs[idx]);
    set_home_page();
    update_title();
}

/* Close tab `idx`; the last tab closing quits the app. Neighbours shift
 * down so g_tabs[0..g_ntabs) stays dense. */
static void tab_close(int idx) {
    if (idx < 0 || idx >= g_ntabs) return;
    if (g_ntabs == 1) sys_exit(0);
    tab_images_free(&g_tabs[idx]);
    js_teardown(&g_tabs[idx]);
    if (g_tabs[idx].eng) { free(g_tabs[idx].eng); g_tabs[idx].eng = NULL; }
    for (int i = idx; i < g_ntabs - 1; i++)
        g_tabs[i] = g_tabs[i + 1];        /* struct copy shifts the bundle */
    g_ntabs--;
    /* The shift duplicated the last tab's image + engine pointers into
     * the now-vacated slot; clear it WITHOUT freeing (the live shifted-
     * down tab owns them now) so a future tab_reset can't double-free. */
    mem_zero(&g_tabs[g_ntabs], sizeof(struct tab));
    if (g_active >= g_ntabs) g_active = g_ntabs - 1;
    else if (g_active > idx) g_active--;
    update_title();
}

static void tab_switch(int idx) {
    if (idx < 0 || idx >= g_ntabs || idx == g_active) return;
    g_active = idx;
    update_title();
}

/* ---- Fetching + Chrome-style navigation ------------------------- */

static const char *http_err_str(int err) {
    static const char *errs[] = {
        "OK", "Bad URL", "DNS lookup failed", "Connection failed",
        "Protocol error", "Chunked encoding (unsupported)",
        "Response too large", "Timed out", "Out of memory",
        "Connection reset"
    };
    int idx = -err;
    return (idx >= 1 && idx <= 9) ? errs[idx] : "Unknown error";
}
#define HTTPE_DNS (-2)

static int msg_append(char *dst, int pos, int max, const char *s) {
    while (*s && pos < max - 1) dst[pos++] = *s++;
    dst[pos] = '\0';
    return pos;
}
static int msg_append_int(char *dst, int pos, int max, int v) {
    char rev[12];
    int t = 0;
    if (v <= 0) rev[t++] = '0';
    while (v > 0 && t < 11) { rev[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0 && pos < max - 1) dst[pos++] = rev[--t];
    dst[pos] = '\0';
    return pos;
}

static void raw_append(const char *s) {
    while (*s && g_raw_len < RAW_CAP) g_raw[g_raw_len++] = *s++;
}

/* In-page error report: names the URL tried and the failure, so a dead
 * fetch on real HW is diagnosable from the screen alone. */
static void show_error_page(const char *url, int err) {
    const char *emsg = http_err_str(err);
    set_status(emsg);

    g_raw_len = 0;
    raw_append("<html><head><title>Problem loading page</title>"
               "<style>"
               "body{background-color:#ffffff;color:#202124}"
               ".box{max-width:520px;margin:24px auto;padding:8px 18px;"
               "border:1px solid #dadce0;background-color:#fbfbfc}"
               "h1{font-size:21px;color:#c5221f}"
               ".url{font-family:monospace;color:#5f6368}"
               "</style></head><body><div class=box>"
               "<h1>This site can't be reached</h1><p class=url>");
    raw_append(url);
    raw_append("</p><p>Error: <b>");
    raw_append(emsg);
    raw_append("</b></p><hr><ul>"
               "<li>Check the address for typos</li>"
               "<li>DNS failed? The gateway/DNS server may be unreachable</li>"
               "<li>Connection failed? The host may be down, or HTTPS-only</li>"
               "</ul><p>Press <b>r</b> to retry, <b>[</b> to go back.</p>"
               "</div></body></html>");
    g_raw[g_raw_len] = '\0';
    render_html();
    update_title();
}

/* Release decoded image pixels from the previous page. */
/* Free decoded pixels held by a specific tab. */
static void tab_images_free(struct tab *t) {
    for (int i = 0; i < t->nimages; i++)
        if (t->images[i].pixels) { free(t->images[i].pixels); t->images[i].pixels = NULL; }
}

static void images_free(void) { tab_images_free(cur); }

/* Cooperative (incremental) image loading. Images parse as state=0
 * "pending"; the main idle loop calls load_one_pending_image() which
 * fetches+decodes exactly ONE pending image per call -- across ALL tabs,
 * active first -- so the UI stays responsive between images and
 * background tabs fill in too. (A single image's fetch still blocks
 * briefly since the kernel HTTP is synchronous; that's bounded to one
 * image, vs the old freeze-for-all-N.) Returns 1 if it did work. */
static uint8_t *g_img_fetch_buf = NULL;

static int load_one_pending_image(void) {
    int ti_sel = -1, ii_sel = -1;
    for (int pass = 0; pass < 2 && ti_sel < 0; pass++) {
        for (int ti = 0; ti < g_ntabs; ti++) {
            if ((pass == 0) != (ti == g_active)) continue;   /* active first */
            struct tab *t = &g_tabs[ti];
            int budget = t->nimages < IMG_FETCH_N ? t->nimages : IMG_FETCH_N;
            for (int ii = 0; ii < budget; ii++)
                if (t->images[ii].state == 0 && t->images[ii].src[0]) {
                    ti_sel = ti; ii_sel = ii; break;
                }
            if (ti_sel >= 0) break;
        }
    }
    if (ti_sel < 0) return 0;

    if (!g_img_fetch_buf) {
        g_img_fetch_buf = (uint8_t *)malloc(IMG_FETCH_CAP);
        if (!g_img_fetch_buf) return 0;
    }
    struct img *im = &g_tabs[ti_sel].images[ii_sel];

    struct http_fetch req;
    mem_zero(&req, sizeof(req));
    req.url    = (unsigned long)im->src;
    req.buf    = (unsigned long)g_img_fetch_buf;
    req.buf_sz = IMG_FETCH_CAP;
    long n = sys_http_fetch(&req);          /* blocks (this image only) */
    if (n <= 0) { im->state = -1; return 1; }

    toby_image_t *dec = toby_image_load(g_img_fetch_buf, (size_t)n);
    if (!dec || dec->width <= 0 || dec->height <= 0 ||
        dec->width > IMG_MAX_DIM || dec->height > IMG_MAX_DIM) {
        if (dec) toby_image_free(dec);
        im->state = -1;
        return 1;
    }
    im->pixels = dec->pixels;               /* steal the decoded buffer */
    im->w = (int16_t)dec->width;
    im->h = (int16_t)dec->height;
    im->state = 1;
    dec->pixels = NULL;
    toby_image_free(dec);

    /* Re-layout the affected tab. The layout shim addresses `cur`, so
     * temporarily point it at the affected tab (safe: single-threaded,
     * no events run here), then restore + request a repaint. */
    int save = g_active;
    g_active = ti_sel;
    layout(g_win_w);
    if (ti_sel == save) clamp_scroll();
    g_active = save;
    tk_redraw(&win);
    return 1;
}

/* Transport-only fetch of `url` into g_raw via SYS_HTTP_FETCH (kernel
 * follows redirects). On success the address bar is updated to the
 * URL that actually served the page. Returns bytes (>= 0) or HTTP_ERR. */
static long fetch_page(const char *url) {
    g_loading = 1;
    set_status("Loading...");

    struct http_fetch req;
    mem_zero(&req, sizeof(req));
    req.url    = (unsigned long)url;
    req.buf    = (unsigned long)g_raw;
    req.buf_sz = RAW_CAP;
    long n = sys_http_fetch(&req);
    g_loading = 0;
    if (n < 0) return n;

    g_last_status = req.status;
    g_raw_len = n;
    g_raw[g_raw_len] = '\0';
    if (req.final_url[0]) {
        str_copy(g_url, req.final_url, URL_MAX);
        g_url_len = (int)str_len(g_url);
    }
    return n;
}

static void render_fetched(void) {
    g_source_view = 0;
    if (is_html_content())
        render_html();
    else
        render_plain_text();
    update_title();

    char m[96];
    int p = 0;
    if (g_last_status >= 400) {
        p = msg_append(m, p, sizeof(m), "HTTP ");
        p = msg_append_int(m, p, sizeof(m), g_last_status);
        p = msg_append(m, p, sizeof(m), " - ");
    } else {
        p = msg_append(m, p, sizeof(m), "Done - ");
    }
    p = msg_append_int(m, p, sizeof(m), g_link_count);
    p = msg_append(m, p, sizeof(m), " links");
    set_status(m);
    /* Images (state=0 pending) stream in from the main idle loop. */
}

/* Fetch + render, error page on failure. Back/Forward/Refresh path. */
static long do_fetch_url(const char *url) {
    long n = fetch_page(url);
    if (n < 0) show_error_page(url, (int)n);
    else render_fetched();
    return n;
}

/* Navigate to a fully-formed URL (links, search, explicit schemes).
 * Returns the fetch result so callers can chain fallbacks. */
static long do_navigate(const char *url) {
    char target[URL_MAX + 1];
    str_copy(target, url, URL_MAX);

    str_copy(g_url, target, URL_MAX);
    g_url_len = (int)str_len(g_url);

    long n = fetch_page(target);
    if (n < 0) {
        show_error_page(target, (int)n);
        history_push(target);
    } else {
        render_fetched();
        history_push(g_url);      /* the post-redirect URL */
    }
    g_focus_url = 0;
    return n;
}

/* GET-submit a form: resolve its action against the current page,
 * append ?name=value pairs for every named text/hidden field, and
 * navigate. POST forms are refused (no request-body support yet). */
static void submit_form(int fi) {
    if (fi < 0 || fi >= g_nforms) return;
    struct form *f = &g_forms[fi];
    if (f->post) { set_status("POST forms not supported yet"); return; }

    char base[URL_MAX + 1];
    if (f->action[0]) {
        resolve_relative_url(g_url, f->action, base, URL_MAX);
    } else {
        str_copy(base, g_url, URL_MAX);      /* action="" = same URL */
    }
    /* a GET submission replaces any existing query */
    for (int i = 0; base[i]; i++)
        if (base[i] == 63) { base[i] = 0; break; }   /* '?' */

    char out[URL_MAX + 1];
    int pos = 0;
    for (int i = 0; base[i] && pos < URL_MAX - 2; i++) out[pos++] = base[i];
    int first = 1;
    for (int k = 0; k < f->nf; k++) {
        struct field *fd2 = &g_fields[f->first + k];
        if (!fd2->name[0] || fd2->type == FT_SUBMIT) continue;
        if (pos >= URL_MAX - 4) break;
        out[pos++] = first ? 63 : 38;        /* '?' then '&' */
        first = 0;
        url_encode_append(fd2->name, out, &pos, URL_MAX);
        if (pos < URL_MAX - 2) out[pos++] = 61;   /* '=' */
        url_encode_append(fd2->value, out, &pos, URL_MAX);
    }
    out[pos] = 0;
    do_navigate(out);
}

/* Run a web search: DuckDuckGo's HTML endpoint first; its 202
 * bot-challenge page (or a transport failure) falls back to Mojeek,
 * which serves plain HTML without a challenge wall. */
static void navigate_search(const char *query) {
    char surl[URL_MAX + 1];
    build_search_url("https://html.duckduckgo.com/html/?q=", query,
                     surl, URL_MAX);
    long rc = do_navigate(surl);
    if (rc < 0 || g_last_status == 202) {
        set_status("Search challenged - trying Mojeek...");
        build_search_url("https://www.mojeek.com/search?q=", query,
                         surl, URL_MAX);
        do_navigate(surl);
    }
}

/* ---- Omnibox input resolution (Chrome-style) --------------------- */

static int has_scheme(const char *s) {
    int i = 0;
    const char *p = "http";
    for (; i < 4; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != p[i]) return 0;
    }
    if (s[i] == 's' || s[i] == 'S') i++;
    return s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/';
}

/* Search query vs. navigable host: spaces always mean search; a single
 * token with no dot, no colon and no slash (and not "localhost") does
 * too. Everything else is treated as a URL. */
static int input_is_query(const char *s) {
    int urlish = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == ' ') return 1;
        if (s[i] == '.' || s[i] == ':' || s[i] == '/') urlish = 1;
    }
    if (urlish) return 0;
    if (str_eq(s, "localhost")) return 0;
    return 1;
}


static void str_concat2(char *out, int max, const char *a, const char *b) {
    int pos = 0;
    for (int i = 0; a[i] && pos < max - 1; i++) out[pos++] = a[i];
    for (int i = 0; b[i] && pos < max - 1; i++) out[pos++] = b[i];
    out[pos] = '\0';
}

static int nav_success(long rc) {
    if (rc < 0) return 0;
    render_fetched();
    history_push(g_url);
    g_focus_url = 0;
    return 1;
}

/* The omnibox: what runs when Enter is pressed in the address bar.
 *   - explicit scheme        -> use as-is
 *   - non-URL text           -> DuckDuckGo HTML search
 *   - bare host[/path]       -> https:// first, http:// fallback,
 *                               www. retry when DNS says no such host */
static void navigate_input(const char *input_raw) {
    char input[URL_MAX + 1];
    int s = 0;
    while (input_raw[s] == ' ' || input_raw[s] == '\t') s++;
    int pos = 0;
    for (int i = s; input_raw[i] && pos < URL_MAX; i++) input[pos++] = input_raw[i];
    while (pos > 0 && (input[pos - 1] == ' ' || input[pos - 1] == '\t')) pos--;
    input[pos] = '\0';
    if (!input[0]) return;

    if (has_scheme(input)) { do_navigate(input); return; }

    if (input_is_query(input)) {
        navigate_search(input);
        return;
    }

    char cand[URL_MAX + 1];
    str_concat2(cand, URL_MAX, "https://", input);
    str_copy(g_url, cand, URL_MAX);
    g_url_len = (int)str_len(g_url);
    long rc = fetch_page(cand);
    if (nav_success(rc)) return;

    if (rc == HTTPE_DNS) {
        /* Host didn't resolve: retry with a www. prefix (https, then
         * http only if www. actually resolved). */
        if (!(input[0] == 'w' && input[1] == 'w' && input[2] == 'w' &&
              input[3] == '.')) {
            char www[URL_MAX + 1];
            str_concat2(www, URL_MAX, "https://www.", input);
            rc = fetch_page(www);
            if (nav_success(rc)) return;
            if (rc != HTTPE_DNS) {
                str_concat2(www, URL_MAX, "http://www.", input);
                rc = fetch_page(www);
                if (nav_success(rc)) return;
            }
            str_copy(cand, www, URL_MAX);
        }
    } else {
        /* Resolved but HTTPS didn't answer: fall back to plain HTTP. */
        str_concat2(cand, URL_MAX, "http://", input);
        rc = fetch_page(cand);
        if (nav_success(rc)) return;
    }

    str_copy(g_url, cand, URL_MAX);
    g_url_len = (int)str_len(g_url);
    show_error_page(cand, (int)rc);
    history_push(cand);
    g_focus_url = 0;
}

/* ---- Scrolling (pixels) ----------------------------------------- */

static void clamp_scroll(void) {
    int maxs = g_doc_h - g_content_h;
    if (maxs < 0) maxs = 0;
    if (g_scroll_y > maxs) g_scroll_y = maxs;
    if (g_scroll_y < 0) g_scroll_y = 0;
}
static void scroll_up(int px)   { g_scroll_y -= px; clamp_scroll(); }
static void scroll_down(int px) { g_scroll_y += px; clamp_scroll(); }

/* Refresh the layout metrics from the live toolkit window size (the
 * toolkit updates win.w/h on TK_EV_RESIZE) and re-run layout() when
 * the width changed. Called at the top of every paint and mouse
 * handler, so the browser self-corrects on any repaint. Scroll
 * position is kept proportionally across reflows. */
static void sync_geometry(void) {
    if (win.w > 0)  g_win_w = win.w;
    if (win.h > 0)  g_win_h = win.h;

    g_content_h = g_win_h - TOOLBAR_H - STATUS_H;
    if (g_content_h < MONO_H) g_content_h = MONO_H;

    if (g_layout_w != g_win_w && E && E->nnodes > 0) {
        int old_doc = g_doc_h > 0 ? g_doc_h : 1;
        long keep = g_scroll_y;
        layout(g_win_w);
        g_scroll_y = (int)(keep * (long)g_doc_h / old_doc);
    }
    clamp_scroll();
}

/* ---- Find in page ---------------------------------------------- */

static void find_next_match(void) {
    if (g_find_len == 0 || !E) return;
    g_find_buf[g_find_len] = '\0';

    /* continue after the current match, wrapping once */
    long start = 0;
    if (g_find_run >= 0 && g_find_run < g_nitems)
        start = g_items[g_find_run].off + 1;

    long hit = -1;
    int idx = str_contains(&g_render[start], (int)(g_render_len - start),
                           g_find_buf, g_find_len);
    if (idx >= 0) hit = start + idx;
    else {
        idx = str_contains(g_render, (int)g_render_len, g_find_buf, g_find_len);
        if (idx >= 0) hit = idx;
    }
    if (hit < 0) { g_find_run = -1; set_status("Not found"); return; }

    /* find the text item containing the hit offset */
    for (int ri = 0; ri < g_nitems; ri++) {
        struct ditem *r = &g_items[ri];
        if (r->kind == DI_TEXT && r->len > 0 &&
            hit >= r->off && hit < r->off + r->len) {
            g_find_run = ri;
            g_scroll_y = r->y - g_content_h / 3;
            clamp_scroll();
            set_status("Found");
            return;
        }
    }
    g_find_run = -1;
    set_status("Not found");
}

/* ---- Drawing --------------------------------------------------- */

static void draw_hline(int fd, int x, int y, int w, uint32_t color) {
    sys_gui_fill(fd, x, y, w, 1, color);
}

/* paint_all() draws the whole window (called from the canvas on_paint);
 * redraw() just requests a repaint and is what the event handlers call. */
/* Paint one image run at screen-y `sy`. Loaded images nearest-neighbor
 * scale-blit row-by-row (alpha-composited) into the run rect; pending
 * and failed images draw a placeholder box with a label. Clipped
 * vertically to the content viewport. */
#define IMG_ROW_MAX 2048
static uint32_t g_img_row[IMG_ROW_MAX];

static void paint_image(const struct ditem *r, int sy) {
    struct img *im = &g_images[r->img];
    int dw = r->w, dh = r->h;

    if (im->state == 1 && im->pixels && im->w > 0 && im->h > 0) {
        int cw = dw < IMG_ROW_MAX ? dw : IMG_ROW_MAX;
        int vis0 = CONTENT_TOP, vis1 = CONTENT_TOP + g_content_h;
        for (int dy = 0; dy < dh; dy++) {
            int ry = sy + dy;
            if (ry < vis0 || ry >= vis1) continue;      /* vertical clip */
            int syx = (int)((long)dy * im->h / dh);
            if (syx >= im->h) syx = im->h - 1;
            const uint32_t *srow = im->pixels + (long)syx * im->w;
            for (int dx = 0; dx < cw; dx++) {
                int sxx = (int)((long)dx * im->w / dw);
                if (sxx >= im->w) sxx = im->w - 1;
                g_img_row[dx] = srow[sxx];
            }
            tk_draw_blit_blend(&win, r->x, ry, cw, 1, g_img_row, cw);
        }
        return;
    }

    /* placeholder / failed box (light page theme) */
    uint32_t bg = 0x00F1F3F4u, bd = 0x00DADCE0u;
    sys_gui_fill(0, r->x, sy, dw, dh, bg);
    sys_gui_fill(0, r->x, sy, dw, 1, bd);
    sys_gui_fill(0, r->x, sy + dh - 1, dw, 1, bd);
    sys_gui_fill(0, r->x, sy, 1, dh, bd);
    sys_gui_fill(0, r->x + dw - 1, sy, 1, dh, bd);
    const char *lbl = (im->state < 0) ? "[image failed]" : "[loading image...]";
    if (dw > 40 && dh > 16)
        tk_draw_text(&win, r->x + 6, sy + dh / 2 - 7, lbl, 0x00757A80u, PX_BODY, 0);
}

/* ---- Tab strip geometry (shared by paint + hit-testing) --------- */
#define TAB_PLUS_W  26           /* the "+" new-tab button at the strip end */
#define TAB_W_MAX  190
#define TAB_W_MIN   46

static int tab_cell_w(void) {
    int avail = g_win_w - TAB_PLUS_W;
    int n = g_ntabs > 0 ? g_ntabs : 1;
    int w = avail / n;
    if (w > TAB_W_MAX) w = TAB_W_MAX;
    if (w < TAB_W_MIN) w = TAB_W_MIN;
    return w;
}
static int tab_x(int i) { return i * tab_cell_w(); }

static void redraw(int fd) { (void)fd; tk_redraw(&win); }
static void paint_all(void) {
    int fd = 0; (void)fd;
    sync_geometry();             /* track live window size; reflow if needed */
    /* Tab Bar: one cell per tab (active highlighted) + a "+" button. */
    sys_gui_fill(fd, 0, 0, g_win_w, TAB_BAR_H, COL_TAB_BG);
    {
        int cw = tab_cell_w();
        for (int i = 0; i < g_ntabs; i++) {
            int tx = tab_x(i);
            int active = (i == g_active);
            uint32_t cbg = active ? COL_TAB_ACTIVE : COL_TAB_BG;
            sys_gui_fill(fd, tx, 0, cw - 1, TAB_BAR_H, cbg);
            if (active) draw_hline(fd, tx, TAB_BAR_H - 1, cw - 1, COL_NAV_BG);
            else sys_gui_fill(fd, tx + cw - 1, 4, 1, TAB_BAR_H - 8, COL_URL_BORDER);

            /* title, truncated to the cell (leave room for the close x) */
            const char *ts = g_tabs[i].title[0] ? g_tabs[i].title : "New Tab";
            int maxc = (cw - 24) / CELL_W;
            if (maxc < 1) maxc = 1;
            if (maxc > 30) maxc = 30;
            char tb[32];
            int tl = 0;
            while (ts[tl] && tl < maxc) { tb[tl] = ts[tl]; tl++; }
            if (ts[tl] && tl >= 3) { tb[tl-1] = '.'; tb[tl-2] = '.'; }
            tb[tl] = '\0';
            sys_gui_text(fd, tx + 6, 5, tb,
                         active ? COL_TAB_TEXT : COL_NAV_BTN, cbg);
            /* per-tab close x near the right edge */
            sys_gui_text(fd, tx + cw - 14, 5, "x", COL_TAB_CLOSE, cbg);
        }
        /* new-tab button */
        int px = g_ntabs * cw + 6;
        if (px < g_win_w - 10)
            sys_gui_text(fd, px, 5, "+", COL_NAV_BTN, COL_TAB_BG);
    }

    /* Navigation Bar */
    int nav_y = TAB_BAR_H;
    sys_gui_fill(fd, 0, nav_y, g_win_w, NAV_BAR_H, COL_NAV_BG);

    uint32_t back_col = can_go_back() ? COL_NAV_BTN : COL_NAV_BTN_DIM;
    sys_gui_text(fd, 8, nav_y + 8, "<", back_col, COL_NAV_BG);

    uint32_t fwd_col = can_go_forward() ? COL_NAV_BTN : COL_NAV_BTN_DIM;
    sys_gui_text(fd, 24, nav_y + 8, ">", fwd_col, COL_NAV_BG);

    sys_gui_text(fd, 40, nav_y + 8, "O", COL_NAV_BTN, COL_NAV_BG);
    sys_gui_text(fd, 56, nav_y + 8, "H", COL_NAV_BTN, COL_NAV_BG);

    /* URL bar */
    int url_x = 72;
    int url_w = g_win_w - url_x - 8;
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

    /* Content Area: paint the display-list items that intersect the
     * viewport. Items are painted in emission order (containing boxes
     * before their content), so no z-sorting is needed. */
    uint32_t page_bg = E ? (E->page_bg & 0xFFFFFF) : 0x00FFFFFFu;
    sys_gui_fill(fd, 0, CONTENT_TOP, g_win_w, g_content_h, page_bg);

    if (E) {
        static char tb[512];
        int vtop = g_scroll_y;
        int vbot = g_scroll_y + g_content_h;
        int vy0 = CONTENT_TOP, vy1 = CONTENT_TOP + g_content_h;

        for (int ri = 0; ri < g_nitems; ri++) {
            struct ditem *r = &g_items[ri];
            if (r->y + r->h <= vtop || r->y >= vbot) continue;
            int sy = CONTENT_TOP + (r->y - vtop);

            if (r->kind == DI_RECT) {
                int y0 = sy, h = r->h;             /* clip to the viewport */
                if (y0 < vy0) { h -= vy0 - y0; y0 = vy0; }
                if (y0 + h > vy1) h = vy1 - y0;
                if (h > 0 && r->w > 0)
                    sys_gui_fill(fd, r->x, y0, r->w, h, r->fg & 0xFFFFFF);
                continue;
            }
            if (r->kind == DI_BULLET) {
                sys_gui_fill(fd, r->x, sy, r->w, r->h, r->fg & 0xFFFFFF);
                continue;
            }
            if (r->kind == DI_IMG) {
                paint_image(r, sy);
                continue;
            }
            if (r->kind == DI_FIELD) {
                struct field *ff = &g_fields[r->field];
                if (r->fl & IF_INPUT) {
                    uint32_t bd = (r->field == g_focus_field)
                                      ? 0x001A73E8u : 0x009AA0A6u;
                    sys_gui_fill(fd, r->x, sy, r->w, FIELD_H, 0x00FFFFFFu);
                    sys_gui_fill(fd, r->x, sy, r->w, 1, bd);
                    sys_gui_fill(fd, r->x, sy + FIELD_H - 1, r->w, 1, bd);
                    sys_gui_fill(fd, r->x, sy, 1, FIELD_H, bd);
                    sys_gui_fill(fd, r->x + r->w - 1, sy, 1, FIELD_H, bd);
                    /* show the tail of the value if it overflows */
                    const char *v = ff->value;
                    int vl = (int)str_len(v);
                    int avail = r->w - 14;
                    while (vl > 0) {
                        if (text_px_w(v, vl, PX_BODY, 0, 0) <= avail) break;
                        v++; vl--;
                    }
                    if (vl > 0)
                        tk_draw_text(&win, r->x + 6, sy + 5, v, 0x00202124u,
                                     PX_BODY, 0);
                    if (r->field == g_focus_field) {
                        int cx = r->x + 6 + text_px_w(v, vl, PX_BODY, 0, 0);
                        if (cx > r->x + r->w - 4) cx = r->x + r->w - 4;
                        sys_gui_fill(fd, cx, sy + 3, 2, FIELD_H - 6, 0x001A73E8u);
                    }
                } else {
                    /* submit button */
                    sys_gui_fill(fd, r->x, sy, r->w, FIELD_H, 0x00F1F3F4u);
                    sys_gui_fill(fd, r->x, sy, r->w, 1, 0x00DADCE0u);
                    sys_gui_fill(fd, r->x, sy + FIELD_H - 1, r->w, 1, 0x00DADCE0u);
                    sys_gui_fill(fd, r->x, sy, 1, FIELD_H, 0x00DADCE0u);
                    sys_gui_fill(fd, r->x + r->w - 1, sy, 1, FIELD_H, 0x00DADCE0u);
                    tk_draw_text(&win, r->x + 14, sy + 5, ff->value,
                                 0x00202124u, PX_BODY, 0);
                }
                continue;
            }
            /* DI_TEXT */
            if (r->len <= 0) continue;

            int n = r->len < (int)sizeof(tb) - 1 ? r->len : (int)sizeof(tb) - 1;
            for (int k = 0; k < n; k++) {
                char ch = g_render[r->off + k];
                tb[k] = ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E)
                            ? ' ' : ch;
            }
            tb[n] = '\0';

            if (ri == g_find_run)
                sys_gui_fill(fd, r->x - 2, sy, r->w + 4, r->h, 0x00FFE49Cu);

            uint32_t fg = r->fg & 0xFFFFFF;
            if (r->fl & IF_MONO) {
                uint32_t mbg = (r->bg >> 24) ? (r->bg & 0xFFFFFF) : page_bg;
                int ty = sy + (r->h - MONO_H) / 2;
                if ((r->bg >> 24) && ri != g_find_run)
                    sys_gui_fill(fd, r->x - 2, sy, r->w + 4, r->h, mbg);
                sys_gui_text(fd, r->x, ty < sy ? sy : ty, tb, fg,
                             (ri == g_find_run) ? 0x00FFE49Cu : mbg);
                continue;
            }
            if ((r->bg >> 24) && ri != g_find_run)
                sys_gui_fill(fd, r->x - 1, sy, r->w + 2, r->h, r->bg & 0xFFFFFF);
            int nat = r->px + r->px / 3;
            int ty = sy + (r->h - nat) / 2;
            if (ty < sy) ty = sy;
            tk_draw_text(&win, r->x, ty, tb, fg, r->px,
                         (r->fl & IF_BOLD) ? 1 : 0);
            if (r->fl & IF_UNDER)
                sys_gui_fill(fd, r->x, ty + r->px + 2, r->w, 1, fg);
        }
    }

    /* Scrollbar (pixel-proportional) */
    if (g_doc_h > g_content_h) {
        int track_x = g_win_w - 6;
        int track_h = g_content_h;
        sys_gui_fill(fd, track_x, CONTENT_TOP, 4, track_h, 0x002D2E31u);

        int thumb_h = (int)((long)track_h * g_content_h / g_doc_h);
        if (thumb_h < 20) thumb_h = 20;
        int maxs = g_doc_h - g_content_h;
        int thumb_y = CONTENT_TOP +
                      (int)((long)g_scroll_y * (track_h - thumb_h) / (maxs > 0 ? maxs : 1));
        sys_gui_fill(fd, track_x, thumb_y, 4, thumb_h, 0x005F6368u);
    }

    /* Status Bar (or Find Bar if in find mode) */
    int status_y = g_win_h - STATUS_H;
    sys_gui_fill(fd, 0, status_y, g_win_w, STATUS_H, COL_STATUS_BG);
    draw_hline(fd, 0, status_y, g_win_w, 0x003C4043u);

    if (g_find_mode) {
        sys_gui_fill(fd, 0, status_y, g_win_w, STATUS_H, COL_FIND_BG);
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
    if (g_doc_h > g_content_h && !g_find_mode) {
        int maxs = g_doc_h - g_content_h;
        int pct = (int)((long)g_scroll_y * 100 / (maxs > 0 ? maxs : 1));
        if (pct > 100) pct = 100;
        char pct_str[8];
        int pp = 0;
        if (pct >= 100) pct_str[pp++] = '1';
        if (pct >= 10)  pct_str[pp++] = '0' + (pct / 10) % 10;
        pct_str[pp++] = '0' + pct % 10;
        pct_str[pp++] = 37;   /* percent sign */
        pct_str[pp] = 0;
        sys_gui_text(fd, g_win_w - (pp * CELL_W) - PAD, status_y + 3,
                     pct_str, COL_STATUS_FG, COL_STATUS_BG);
    }

    /* Source view indicator */
    if (g_source_view) {
        sys_gui_text(fd, g_win_w - 80, status_y + 3, "[SOURCE]", COL_FIND_HL, COL_STATUS_BG);
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

/* Doc-space hit-test: interactive item (link or form control) under
 * client (mx,my), or NULL. */
static struct ditem *run_at(int mx, int my) {
    if (!E) return NULL;
    int dy = my - CONTENT_TOP + g_scroll_y;
    for (int ri = 0; ri < g_nitems; ri++) {
        struct ditem *r = &g_items[ri];
        if (r->link < 0 && r->field < 0) continue;
        if (r->kind != DI_TEXT && r->kind != DI_IMG && r->kind != DI_FIELD)
            continue;
        if (dy >= r->y && dy < r->y + r->h &&
            mx >= r->x && mx < r->x + r->w)
            return r;
    }
    return NULL;
}

static int run_link_at(int mx, int my) {
    struct ditem *r = run_at(mx, my);
    return (r && r->link >= 0) ? r->link : -1;
}

/* ---- Mouse event handling -------------------------------------- */

static void handle_mouse_down(int fd, int mx, int my) {
    sync_geometry();
    int nav_y = TAB_BAR_H;

    /* Tab strip: switch tab, close its x, or open a new tab. */
    if (my < TAB_BAR_H) {
        int cw = tab_cell_w();
        for (int i = 0; i < g_ntabs; i++) {
            int tx = tab_x(i);
            if (mx >= tx && mx < tx + cw) {
                if (mx >= tx + cw - 18) tab_close(i);   /* the x hit-box */
                else tab_switch(i);
                redraw(fd);
                return;
            }
        }
        /* past the last tab -> the + button */
        if (mx >= g_ntabs * cw) { tab_open(); redraw(fd); }
        return;
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
    if (my >= CONTENT_TOP && my < g_win_h - STATUS_H) {
        /* Scrollbar area (right 6px): jump-scroll to position */
        if (mx >= g_win_w - 8 && g_doc_h > g_content_h) {
            int rel_y = my - CONTENT_TOP;
            int maxs = g_doc_h - g_content_h;
            g_scroll_y = (int)((long)rel_y * maxs / g_content_h);
            clamp_scroll();
            redraw(fd);
            return;
        }

        /* JS click dispatch first: bubble from the topmost item under
         * the cursor; a preventDefault() suppresses the default
         * link/submit action below. */
        if (cur->js_cx && cur->js_has_dispatch && E) {
            int dy2 = my - CONTENT_TOP + g_scroll_y;
            int evn = -1;
            for (int ri = 0; ri < g_nitems; ri++) {
                struct ditem *r2 = &g_items[ri];
                if (r2->node < 0) continue;
                if (dy2 >= r2->y && dy2 < r2->y + r2->h &&
                    mx >= r2->x && mx < r2->x + r2->w)
                    evn = r2->node;        /* last match = topmost */
            }
            if (evn >= 0) {
                int prevented = js_dispatch_event(evn, "click");
                js_do_pending_nav();
                if (prevented) {
                    redraw(fd);
                    return;
                }
            }
        }

        /* Inline link / form-control click: hit-test the items */
        struct ditem *hit = run_at(mx, my);
        if (hit && hit->field >= 0) {
            g_focus_url = 0;
            if (hit->fl & IF_INPUT) {
                g_focus_field = hit->field;
                set_status("Type into the field; Enter submits");
            } else {
                g_focus_field = -1;
                submit_form(g_fields[hit->field].form);
            }
            redraw(fd);
            return;
        }
        if (hit && hit->link >= 0 && hit->link < g_link_count) {
            follow_link(hit->link + 1);
            redraw(fd);
            return;
        }
        /* Click in empty content unfocuses URL bar + fields */
        g_focus_url = 0;
        g_focus_field = -1;
        redraw(fd);
        return;
    }

    redraw(fd);
}

/* Show the link target in the status bar on hover (Chrome-style). */
static void handle_mouse_move(int fd, int mx, int my) {
    if (my >= CONTENT_TOP && my < g_win_h - STATUS_H && mx < g_win_w - 8) {
        int lnk = run_link_at(mx, my);
        if (lnk >= 0 && lnk < g_link_count) {
            if (!str_eq(g_status_text, g_links[lnk])) {
                str_copy(g_status_text, g_links[lnk], sizeof(g_status_text));
                redraw(fd);
            }
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

    /* Global tab accelerators (control codes; work in any mode -- these
     * are never printable text). Ctrl+Tab is unusable (indistinguishable
     * from Tab), so tobyOS uses Ctrl+N / Ctrl+P to cycle. */
    if (key == 0x14) { tab_open();  redraw(0); return; }              /* Ctrl+T */
    if (key == 0x17) { tab_close(g_active); redraw(0); return; }      /* Ctrl+W */
    if (key == 0x0E) { tab_switch((g_active + 1) % (g_ntabs>0?g_ntabs:1));
                       redraw(0); return; }                          /* Ctrl+N */
    if (key == 0x10) { tab_switch((g_active + g_ntabs - 1) % (g_ntabs>0?g_ntabs:1));
                       redraw(0); return; }                          /* Ctrl+P */

    /* A focused form field owns the keyboard (Tab hands off, Esc
     * unfocuses, Enter submits the field's form). */
    if (g_focus_field >= 0 && !g_focus_url && !g_find_mode) {
        struct field *f = &g_fields[g_focus_field];
        if (cur->js_cx && f->node >= 0) {
            char one[2];
            const char *kn = js_key_name(key, one);
            if (kn[0] && js_dispatch_key(f->node, "keydown", kn)) {
                js_do_pending_nav();
                redraw(0);
                return;
            }
        }
        if (key == 27) {
            g_focus_field = -1;
            set_status("Field unfocused");
            redraw(0);
            return;
        }
        if (key == 9) {
            g_focus_field = -1;              /* fall through to Tab */
        } else if (key == 10 || key == 13) {
            submit_form(f->form);
            redraw(0);
            return;
        } else if (key == 8 || key == 127) {
            int l = (int)str_len(f->value);
            if (l > 0) f->value[l - 1] = 0;
            if (cur->js_cx && f->node >= 0)
                js_dispatch_event(f->node, "input");
            redraw(0);
            return;
        } else if (key >= 0x20 && key <= 0x7E) {
            int l = (int)str_len(f->value);
            if (l < (int)sizeof(f->value) - 1) {
                f->value[l] = (char)key;
                f->value[l + 1] = 0;
            }
            if (cur->js_cx && f->node >= 0)
                js_dispatch_event(f->node, "input");
            redraw(0);
            return;
        } else {
            return;
        }
    }

        /* Find mode input */
        if (g_find_mode) {
            if (key == 27) {
                g_find_mode = 0;
                g_find_run = -1;
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
            g_find_run = -1;
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
                    /* Omnibox: navigate_input() rewrites g_url as it
                     * resolves (scheme inference / search / redirects),
                     * so hand it a stable copy of what was typed. */
                    char typed[URL_MAX + 1];
                    str_copy(typed, g_url, URL_MAX);
                    navigate_input(typed);
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

            if (cur->js_cx) {
                char one[2];
                const char *kn = js_key_name(key, one);
                if (kn[0] && js_dispatch_key(-1, "keydown", kn)) {
                    js_do_pending_nav();
                    redraw(0);
                    return;
                }
                js_do_pending_nav();
            }

            switch (key) {
            case 'j':  scroll_down(48);   break;
            case 'k':  scroll_up(48);     break;
            case 'd':  scroll_down(g_content_h - 24); break;
            case 'u':  scroll_up(g_content_h - 24);   break;
            case 'g':  g_scroll_y = 0;    break;
            case 'G':
                g_scroll_y = g_doc_h;
                clamp_scroll();
                break;
            case 'n':
                if (g_find_len > 0) find_next_match();
                break;
            case 'M':
                /* Maximize; the WM answers with TK_EV_RESIZE and the next
                 * paint reflows from the live geometry. */
                tk_maximize(&win);
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
                    g_scroll_y = 0;
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
            case 'e': {
                /* Focus the next text field on the page and scroll to
                 * it (keyboard access to form inputs). */
                int start = g_focus_field + 1;
                int found = -1;
                for (int k = 0; k < g_nfields; k++) {
                    int idx = (start + k) % (g_nfields > 0 ? g_nfields : 1);
                    if (g_nfields > 0 && g_fields[idx].type == FT_TEXT) {
                        found = idx; break;
                    }
                }
                if (found < 0) { set_status("No text fields on this page"); break; }
                g_focus_field = found;
                g_focus_url = 0;
                for (int ri = 0; ri < g_nitems; ri++) {
                    if (g_items[ri].field == found) {
                        g_scroll_y = g_items[ri].y - g_content_h / 3;
                        clamp_scroll();
                        break;
                    }
                }
                set_status("Field focused - type, Enter submits, Esc cancels");
                break;
            }
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

    mem_zero(g_status_text, sizeof(g_status_text));
    /* One tab to start (its bundle non-zero defaults set by tab_reset). */
    g_ntabs = 1; g_active = 0;
    tab_reset(&g_tabs[0]);
    if (!g_tabs[0].eng) {
        sys_write(1, "gui_browser: engine alloc failed\n", 33);
        return 1;
    }

    if (tk_window_open(&win, WIN_W, WIN_H, "TobyOS Browser") != 0) {
        sys_write(1, "gui_browser: window failed\n", 27);
        return 1;
    }
    tk_on_key(&win, on_key);
    struct tk_widget *root = tk_root(&win); tk_pad(root, 0);
    struct tk_widget *cv = tk_canvas(&win, root, on_paint);
    tk_grow(cv, 1); tk_on_event(cv, on_event);

    /* Build the home page after the window exists: layout measures
     * glyph advances through the TTF-width syscall and update_title
     * needs the window fd. */
    set_home_page();
    set_status("Ready - Press Tab to focus address bar");

    /* Cooperative loop: pump events + repaint, then fetch ONE pending
     * image per idle pass (keeps the UI live during image loading and
     * lets background tabs load). Idle-sleep only when there is no image
     * work, so a page with images fills in as fast as the network + a
     * repaint between each allows. */
    tk_redraw(&win);
    for (;;) {
        if (tk_pump(&win)) break;          /* events + repaint; 1 = quit */
        int work = load_one_pending_image();
        work |= js_pump_all();             /* timers + promise jobs */
        if (!work)
            sys_sleep_ms(15);              /* nothing pending -> idle */
    }
    return 0;
}
