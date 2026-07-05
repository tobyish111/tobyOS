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

/* Heading sentinel bytes: placed at start of heading lines during render,
 * detected during line-index build to tag styles, then removed for display */
#define SENTINEL_H1  0x01
#define SENTINEL_H2  0x02
#define SENTINEL_H3  0x03

/* ---- Content buffer & HTML renderer ---------------------------- */

#define RAW_CAP       (96 * 1024)

#define RENDER_CAP    (128 * 1024)

/* ---- Layout-engine document model ------------------------------- *
 * Parse once into styled SPANS grouped into BLOCKS; lay out per
 * viewport width into positioned RUNS drawn with the kernel TTF
 * rasterizer. A resize re-runs layout() only; a fetch re-parses. */

#define FL_BOLD   0x01
#define FL_LINK   0x02
#define FL_CODE   0x04           /* monospace (pre/code) */
#define FL_BR     0x08           /* forced line break before this span */
#define FL_DIM    0x10           /* de-emphasized ([img] placeholders) */

/* Block types */
#define BT_P      0
#define BT_H1     1
#define BT_H2     2
#define BT_H3     3
#define BT_LI     4
#define BT_HR     5
#define BT_PRE    6

/* Font sizes (px) per role; code uses the fixed 8x16 mono font. */
#define PX_BODY   15
#define PX_H1     26
#define PX_H2     21
#define PX_H3     17
#define MONO_W     8
#define MONO_H    16

struct span {                    /* style-consistent slice of g_render */
    int32_t  off;
    int32_t  len;
    int16_t  link;               /* g_links index or -1 */
    int16_t  field;              /* g_fields index or -1 (FL_INPUT/SUBMIT) */
    int16_t  img;                /* g_images index or -1 */
    uint8_t  px;
    uint8_t  fl;
};
#define SPAN_MAX  16384

struct blk {
    int32_t  s0;                 /* first span */
    int32_t  ns;
    uint8_t  type;
};
#define BLK_MAX   4096

struct run {                     /* positioned draw command (doc coords) */
    int32_t  off;                /* text slice (unused for HR/bullet) */
    int32_t  y;
    int16_t  x, w, len;
    int16_t  h;                  /* explicit height (images); 0 = derive */
    int16_t  link;
    int16_t  field;              /* g_fields index or -1 */
    int16_t  img;                /* g_images index or -1 */
    uint8_t  px;
    uint8_t  fl;
};
#define RUN_MAX   16384


/* ---- Forms (GET submission) -------------------------------------- */

#define FT_TEXT    0
#define FT_HIDDEN  1
#define FT_SUBMIT  2

#define FL_INPUT   0x80          /* span/run is a text-input box */
/* FL_BULLET doubles as the submit-button flag on field runs (a field
 * run is never a list bullet, so the bit is unambiguous there). */
#define FL_SUBMITB FL_BULLET

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

/* Advance-width tables for the proportional faces we use, indexed by
 * (style, ch - 32). Filled lazily via SYS_GUI_TEXT_TTF_WIDTH one char
 * at a time (the kernel rasterizer is advance-based, so per-char sums
 * equal string widths); afterwards all wrapping math is userspace. */
#define FACE_BODY   0            /* PX_BODY regular */
#define FACE_BOLD   1            /* PX_BODY bold */
#define FACE_H1     2            /* PX_H1 bold */
#define FACE_H2     3            /* PX_H2 bold */
#define FACE_H3     4            /* PX_H3 bold */
#define NFACES      5
static short g_adv[NFACES][95];
static int   g_adv_ready = 0;

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
struct tab {
    char   raw[RAW_CAP + 1];   long raw_len;
    char   render[RENDER_CAP + 1]; long render_len;
    struct span spans[SPAN_MAX];   int nspans;
    struct blk  blks[BLK_MAX];      int nblks;
    struct run  runs[RUN_MAX];      int nruns;
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

#define g_raw         (cur->raw)
#define g_raw_len     (cur->raw_len)
#define g_render      (cur->render)
#define g_render_len  (cur->render_len)
#define g_spans       (cur->spans)
#define g_nspans      (cur->nspans)
#define g_blks        (cur->blks)
#define g_nblks       (cur->nblks)
#define g_runs        (cur->runs)
#define g_nruns       (cur->nruns)
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

/* emit_char / emit_str are defined with the layout engine below (they
 * append normalized text AND maintain the styled span stream). */
static void emit_char(char c);
static void emit_str(const char *s);

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

/* ---- Measurement ------------------------------------------------- */

#define FL_BULLET 0x20           /* run-only: list bullet marker */
#define FL_HRULE  0x40           /* run-only: horizontal rule */

static int face_for(uint8_t px, uint8_t fl) {
    if (px == PX_H1) return FACE_H1;
    if (px == PX_H2) return FACE_H2;
    if (px == PX_H3) return FACE_H3;
    return (fl & FL_BOLD) ? FACE_BOLD : FACE_BODY;
}

static void adv_init(void) {
    static const uint8_t face_px[NFACES]   = { PX_BODY, PX_BODY, PX_H1, PX_H2, PX_H3 };
    static const uint8_t face_bold[NFACES] = { 0, 1, 1, 1, 1 };
    char one[2] = { 0, 0 };
    for (int f = 0; f < NFACES; f++) {
        for (int c = 0; c < 95; c++) {
            one[0] = (char)(32 + c);
            int w = tk_text_width(one, face_px[f], face_bold[f]);
            g_adv[f][c] = (short)(w > 0 ? w : face_px[f] / 2);
        }
    }
    g_adv_ready = 1;
}

/* Width of a g_render slice in the given style (advance-sum). */
static int text_w(long off, int len, uint8_t px, uint8_t fl) {
    if (fl & FL_CODE) return len * MONO_W;
    const short *adv = g_adv[face_for(px, fl)];
    int w = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)g_render[off + i];
        w += adv[(c >= 32 && c <= 126) ? c - 32 : '?' - 32];
    }
    return w;
}

static int line_h_for(uint8_t px, uint8_t fl) {
    if (fl & FL_CODE) return MONO_H + 2;
    return px + px / 3;
}

/* ---- Parser emit API (styled spans grouped into blocks) ---------- */

static uint8_t g_cur_px, g_cur_fl;
static short   g_cur_link;
static int     g_blk_openp;      /* a block is being filled */
static int     g_pending_br;     /* <br>: mark next span FL_BR */

static void span_break(void) {   /* close the span under construction */
    if (g_nspans > 0) {
        struct span *s = &g_spans[g_nspans - 1];
        if (s->len == 0 && !(s->fl & (FL_BR | FL_INPUT | FL_SUBMITB)) &&
            s->field < 0 && s->img < 0)
            g_nspans--;                                    /* drop empties */
    }
}

static void blk_close(void) {
    if (!g_blk_openp) return;
    span_break();
    struct blk *b = &g_blks[g_nblks];
    b->ns = g_nspans - b->s0;
    if (b->ns > 0 || b->type == BT_HR) g_nblks++;
    g_blk_openp = 0;
}

static void blk_open(uint8_t type) {
    blk_close();
    if (g_nblks >= BLK_MAX) return;
    g_blks[g_nblks].s0 = g_nspans;
    g_blks[g_nblks].ns = 0;
    g_blks[g_nblks].type = type;
    g_blk_openp = 1;
    g_pending_br = 0;
    switch (type) {
    case BT_H1: g_cur_px = PX_H1; break;
    case BT_H2: g_cur_px = PX_H2; break;
    case BT_H3: g_cur_px = PX_H3; break;
    default:    g_cur_px = PX_BODY; break;
    }
}

/* Append one normalized character in the current style. Opens spans /
 * blocks as needed; splits the span when the style changed. */
static void emit_char(char c) {
    if (g_render_len >= RENDER_CAP) return;
    if (!g_blk_openp) blk_open(BT_P);
    uint8_t fl = g_cur_fl | (g_pending_br ? FL_BR : 0);
    struct span *s = (g_nspans > 0) ? &g_spans[g_nspans - 1] : NULL;
    int fresh = (!s || g_nspans <= g_blks[g_nblks].s0 ||
                 s->off + s->len != g_render_len ||
                 s->px != g_cur_px || s->fl != fl || s->link != g_cur_link ||
                 s->field >= 0 || s->img >= 0);
    if (fresh) {
        if (g_nspans >= SPAN_MAX) return;
        s = &g_spans[g_nspans++];
        s->off = (int32_t)g_render_len;
        s->len = 0;
        s->px = g_cur_px;
        s->fl = fl;
        s->link = g_cur_link;
        s->field = -1;
        s->img = -1;
    }
    g_pending_br = 0;
    g_render[g_render_len++] = c;
    s->len++;
}

static void emit_str(const char *str) {
    while (*str) emit_char(*str++);
}

/* Place a form control (text box / submit button) into the flow. */
static void emit_field(short fi, uint8_t fl) {
    if (!g_blk_openp) blk_open(BT_P);
    span_break();
    if (g_nspans >= SPAN_MAX) return;
    struct span *s = &g_spans[g_nspans++];
    s->off = (int32_t)g_render_len;
    s->len = 0;
    s->px = PX_BODY;
    s->fl = fl | (g_pending_br ? FL_BR : 0);
    s->link = -1;
    s->field = fi;
    s->img = -1;
    g_pending_br = 0;
}

/* Place an <img> into the flow (its own span carrying the image idx). */
static void emit_image(short ii) {
    if (!g_blk_openp) blk_open(BT_P);
    span_break();
    if (g_nspans >= SPAN_MAX) return;
    struct span *s = &g_spans[g_nspans++];
    s->off = (int32_t)g_render_len;
    s->len = 0;
    s->px = PX_BODY;
    s->fl = (g_pending_br ? FL_BR : 0);
    s->link = g_cur_link;        /* an <a><img></a> stays clickable */
    s->field = -1;
    s->img = ii;
    g_pending_br = 0;
}

/* ---- Layout: blocks + spans -> positioned runs ------------------- */

#define MARGIN_L   10
#define MARGIN_R   16            /* leaves room for the scrollbar */
#define LI_INDENT  20

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
    if (fl & FL_INPUT)
        return f->size > 0 ? f->size * 8 + 18 : 190;
    /* submit button: label width + padding */
    int lw = 0;
    for (int i = 0; f->value[i]; i++) {
        unsigned char ch = (unsigned char)f->value[i];
        lw += g_adv[FACE_BODY][(ch >= 32 && ch <= 126) ? ch - 32 : 31];
    }
    return lw + 28;
}
#define FIELD_H (PX_BODY + 12)

static void run_flush(long off, int len, int x, int y, int w,
                      uint8_t px, uint8_t fl, short link) {
    if (len <= 0 || g_nruns >= RUN_MAX) return;
    struct run *r = &g_runs[g_nruns++];
    r->off = (int32_t)off;
    r->len = (int16_t)len;
    r->x = (int16_t)x;
    r->y = (int32_t)y;
    r->w = (int16_t)w;
    r->px = px;
    r->fl = fl;
    r->link = link;
    r->field = -1;
    r->img = -1;
    r->h = 0;
}

static void layout(int width) {
    if (!g_adv_ready) adv_init();
    g_nruns = 0;
    g_find_run = -1;
    int y = 8;
    int usable = width - MARGIN_R;

    for (int bi = 0; bi < g_nblks; bi++) {
        struct blk *b = &g_blks[bi];
        int indent = MARGIN_L + (b->type == BT_LI ? LI_INDENT : 0);

        /* top margins */
        switch (b->type) {
        case BT_H1: y += 14; break;
        case BT_H2: y += 12; break;
        case BT_H3: y += 10; break;
        case BT_HR: y += 8;  break;
        case BT_LI: y += 2;  break;
        case BT_PRE: y += 1; break;
        default:    y += 8;  break;
        }

        if (b->type == BT_HR) {
            if (g_nruns < RUN_MAX) {
                struct run *r = &g_runs[g_nruns++];
                r->off = 0; r->len = 0;
                r->x = (int16_t)MARGIN_L;
                r->y = (int32_t)y;
                r->w = (int16_t)(usable - MARGIN_L);
                r->px = PX_BODY; r->fl = FL_HRULE; r->link = -1; r->field = -1; r->img = -1; r->h = 0;
            }
            y += 6;
            continue;
        }

        /* line metrics from the tallest span in the block */
        int lh = 0;
        for (int si = 0; si < b->ns; si++) {
            struct span *s = &g_spans[b->s0 + si];
            int h = (s->field >= 0) ? FIELD_H + 4 : line_h_for(s->px, s->fl);
            if (h > lh) lh = h;
        }
        if (lh == 0) lh = line_h_for(PX_BODY, 0);

        if (b->type == BT_LI && g_nruns < RUN_MAX) {
            struct run *r = &g_runs[g_nruns++];
            r->off = 0; r->len = 0;
            r->x = (int16_t)(MARGIN_L + 6);
            r->y = (int32_t)y;
            r->w = 5;
            r->px = PX_BODY; r->fl = FL_BULLET; r->link = -1; r->field = -1; r->img = -1; r->h = 0;
        }

        int x = indent;
        for (int si = 0; si < b->ns; si++) {
            struct span *s = &g_spans[b->s0 + si];
            if (s->fl & FL_BR) { y += lh; x = indent; }

            if (s->img >= 0) {                   /* image: own line block */
                if (x > indent) { y += lh; x = indent; }
                int dw, dh;
                img_disp_dims(&g_images[s->img], usable - indent, &dw, &dh);
                if (g_nruns < RUN_MAX) {
                    struct run *r = &g_runs[g_nruns++];
                    r->off = 0; r->len = 0;
                    r->x = (int16_t)indent;
                    r->y = (int32_t)y;
                    r->w = (int16_t)dw;
                    r->h = (int16_t)dh;
                    r->px = PX_BODY;
                    r->fl = s->fl;
                    r->link = s->link;           /* clickable if in an <a> */
                    r->field = -1;
                    r->img = s->img;
                }
                y += dh + 4;
                x = indent;
                continue;
            }

            if (s->field >= 0) {                 /* inline form control */
                int fw = field_w(&g_fields[s->field], s->fl);
                if (x + fw > usable && x > indent) { y += lh; x = indent; }
                if (g_nruns < RUN_MAX) {
                    struct run *r = &g_runs[g_nruns++];
                    r->off = 0; r->len = 0;
                    r->x = (int16_t)x;
                    r->y = (int32_t)y;
                    r->w = (int16_t)fw;
                    r->px = PX_BODY;
                    r->fl = s->fl;
                    r->link = -1;
                    r->field = s->field;
                    r->img = -1;
                    r->h = 0;
                }
                x += fw + 6;
                continue;
            }

            /* walk the span word-by-word, coalescing words that stay on
             * the same line into one run */
            long roff = s->off; int rlen = 0, rx = x, rw = 0;
            long i = s->off, end = s->off + s->len;
            while (i < end) {
                /* next word (including one leading space if present) */
                long wstart = i;
                int  sp = 0;
                if (g_render[i] == ' ') { sp = 1; i++; }
                while (i < end && g_render[i] != ' ') i++;
                int wlen = (int)(i - wstart);
                int ww = text_w(wstart, wlen, s->px, s->fl);

                if (x + ww > usable && x > indent) {
                    /* wrap: flush current run, drop the leading space */
                    run_flush(roff, rlen, rx, y, rw, s->px, s->fl, s->link);
                    y += lh; x = indent;
                    roff = wstart + sp; rlen = 0; rx = x; rw = 0;
                    ww = text_w(wstart + sp, wlen - sp, s->px, s->fl);
                    wlen -= sp;
                    wstart += sp;
                }
                if (rlen == 0) { roff = wstart; rx = x; }
                rlen = (int)(wstart + wlen - roff);
                rw += ww;
                x += ww;
            }
            run_flush(roff, rlen, rx, y, rw, s->px, s->fl, s->link);
        }
        y += lh;

        /* bottom margins */
        switch (b->type) {
        case BT_H1: y += 6; break;
        case BT_H2: y += 5; break;
        case BT_H3: y += 4; break;
        case BT_LI: y += 0; break;
        case BT_PRE: y += 0; break;
        default:    y += 2; break;
        }
    }

    g_doc_h = y + 12;
    g_layout_w = width;
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

/* Reset all per-page document state (text, spans, blocks, runs). */
static void doc_reset(void) {
    g_render_len = 0;
    g_nspans = 0;
    g_nblks = 0;
    g_nruns = 0;
    g_blk_openp = 0;
    g_pending_br = 0;
    g_cur_px = PX_BODY;
    g_cur_fl = 0;
    g_cur_link = -1;
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

static int blk_is_empty(void) {
    return !g_blk_openp || g_nspans == g_blks[g_nblks].s0;
}

/* Extract attribute `name` from a tag body ("input type=text ...").
 * Case-insensitive, handles quoted and bare values. Returns 1 if the
 * attribute exists (out may be empty for a valueless attribute). */
static int tag_attr(const char *t, const char *name, char *out, int cap) {
    int nlen = (int)str_len(name);
    out[0] = 0;
    const char *p = t;
    while (*p && !is_whitespace(*p)) p++;          /* skip the tag name */
    while (*p) {
        while (is_whitespace(*p)) p++;
        if (!*p) break;
        const char *k = p;
        while (*p && !is_whitespace(*p) && *p != '=' && *p != '>') p++;
        int klen = (int)(p - k);
        int match = (klen == nlen && str_ncasecmp(k, name, nlen) == 0);
        while (is_whitespace(*p)) p++;
        if (*p != '=') {                            /* valueless attr */
            if (match) return 1;
            continue;
        }
        p++;
        while (is_whitespace(*p)) p++;
        char q = 0;
        if (*p == 34 || *p == 39) { q = *p; p++; }
        int o = 0;
        while (*p && (q ? *p != q : !is_whitespace(*p) && *p != '>')) {
            if (match && o < cap - 1) out[o++] = *p;
            p++;
        }
        if (q && *p == q) p++;
        if (match) { out[o] = 0; return 1; }
    }
    return 0;
}

static void resolve_relative_url(const char *base, const char *rel, char *out, int out_max);
static void images_free(void);
static void tab_images_free(struct tab *t);
static void layout(int width);
static void clamp_scroll(void);

static void render_html(void) {
    images_free();
    doc_reset();

    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int in_title = 0;
    int in_pre = 0;
    int title_pos = 0;
    int last_was_space = 1;
    int bold_depth = 0;

    char tag_buf[512];
    int tag_buf_len = 0;
    int cur_form = -1;

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
                } else if (tag_match(t, "pre")) {
                    if (!closing) {
                        in_pre = 1;
                        g_cur_fl |= FL_CODE;
                        blk_open(BT_PRE);
                    } else {
                        in_pre = 0;
                        g_cur_fl &= (uint8_t)~FL_CODE;
                        blk_close();
                    }
                    last_was_space = 1;
                } else if (tag_match(t, "code")) {
                    if (!closing) g_cur_fl |= FL_CODE;
                    else g_cur_fl &= (uint8_t)~FL_CODE;
                } else if (tag_match(t, "h1") || tag_match(t, "h2") ||
                           tag_match(t, "h3") || tag_match(t, "h4") ||
                           tag_match(t, "h5") || tag_match(t, "h6")) {
                    if (!closing) {
                        blk_open(t[1] == '1' ? BT_H1 :
                                 t[1] == '2' ? BT_H2 : BT_H3);
                    } else {
                        blk_close();
                    }
                    last_was_space = 1;
                } else if (tag_match(t, "p") || tag_match(t, "div") ||
                           tag_match(t, "section") || tag_match(t, "article") ||
                           tag_match(t, "header") || tag_match(t, "footer") ||
                           tag_match(t, "main") || tag_match(t, "nav") ||
                           tag_match(t, "ul") || tag_match(t, "ol") ||
                           tag_match(t, "table") || tag_match(t, "tr")) {
                    blk_close();
                    last_was_space = 1;
                } else if (tag_match(t, "br")) {
                    g_pending_br = 1;
                    last_was_space = 1;
                } else if (tag_match(t, "hr")) {
                    blk_open(BT_HR);
                    blk_close();
                    last_was_space = 1;
                } else if (tag_match(t, "li")) {
                    if (!closing) blk_open(BT_LI);
                    else blk_close();
                    last_was_space = 1;
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
                            g_cur_link = (short)g_link_count;
                            g_link_count++;
                            g_cur_fl |= FL_LINK;
                        }
                    } else {
                        g_cur_link = -1;
                        g_cur_fl &= (uint8_t)~FL_LINK;
                    }
                } else if (tag_match(t, "b") || tag_match(t, "strong")) {
                    if (!closing) { bold_depth++; g_cur_fl |= FL_BOLD; }
                    else {
                        if (bold_depth > 0) bold_depth--;
                        if (bold_depth == 0) g_cur_fl &= (uint8_t)~FL_BOLD;
                    }
                } else if (tag_match(t, "i") || tag_match(t, "em")) {
                    /* no italic face yet */
                } else if (tag_match(t, "img")) {
                    char isrc[512];
                    if (g_nimages < IMG_MAX &&
                        tag_attr(t, "src", isrc, sizeof(isrc)) && isrc[0] &&
                        /* skip data: URIs and 1px tracker gifs cheaply */
                        !(isrc[0] == 'd' && isrc[1] == 'a' && isrc[2] == 't' &&
                          isrc[3] == 'a' && isrc[4] == ':')) {
                        struct img *im = &g_images[g_nimages];
                        mem_zero(im, sizeof(*im));
                        resolve_relative_url(g_url, isrc, im->src, sizeof(im->src));
                        char dbuf[8];
                        im->attr_w = tag_attr(t, "width", dbuf, sizeof(dbuf))
                                         ? (int16_t)atoi_simple(dbuf) : 0;
                        im->attr_h = tag_attr(t, "height", dbuf, sizeof(dbuf))
                                         ? (int16_t)atoi_simple(dbuf) : 0;
                        im->state = 0;
                        emit_image((short)g_nimages);
                        g_nimages++;
                        last_was_space = 0;
                    } else {
                        uint8_t sv = g_cur_fl;
                        g_cur_fl |= FL_DIM;
                        emit_str("[img]");
                        g_cur_fl = sv;
                        last_was_space = 0;
                    }
                } else if (tag_match(t, "form")) {
                    if (!closing && g_nforms < FORM_MAX) {
                        struct form *f = &g_forms[g_nforms];
                        f->first = (int16_t)g_nfields;
                        f->nf = 0;
                        tag_attr(t, "action", f->action, sizeof(f->action));
                        char meth[12];
                        f->post = tag_attr(t, "method", meth, sizeof(meth)) &&
                                  (meth[0] == 'p' || meth[0] == 'P');
                        cur_form = g_nforms++;
                    } else if (closing) {
                        cur_form = -1;
                    }
                    blk_close();
                    last_was_space = 1;
                } else if (tag_match(t, "input")) {
                    if (cur_form >= 0 && g_nfields < FIELD_MAX) {
                        char ty[20];
                        if (!tag_attr(t, "type", ty, sizeof(ty))) ty[0] = 0;
                        int ftype = -1;
                        if (!ty[0] || str_ncasecmp(ty, "text", 5) == 0 ||
                            str_ncasecmp(ty, "search", 7) == 0)
                            ftype = FT_TEXT;
                        else if (str_ncasecmp(ty, "hidden", 7) == 0)
                            ftype = FT_HIDDEN;
                        else if (str_ncasecmp(ty, "submit", 7) == 0)
                            ftype = FT_SUBMIT;
                        if (ftype >= 0) {
                            struct field *fl2 = &g_fields[g_nfields];
                            fl2->form = (int16_t)cur_form;
                            fl2->type = (uint8_t)ftype;
                            tag_attr(t, "name", fl2->name, sizeof(fl2->name));
                            tag_attr(t, "value", fl2->value, sizeof(fl2->value));
                            char szs[8];
                            fl2->size = 0;
                            if (tag_attr(t, "size", szs, sizeof(szs))) {
                                int v = 0;
                                for (int k = 0; szs[k] >= 48 && szs[k] <= 57; k++)
                                    v = v * 10 + (szs[k] - 48);
                                fl2->size = (int16_t)v;
                            }
                            if (ftype == FT_SUBMIT && !fl2->value[0])
                                str_copy(fl2->value, "Submit", sizeof(fl2->value));
                            if (ftype == FT_TEXT)
                                emit_field((short)g_nfields, FL_INPUT);
                            else if (ftype == FT_SUBMIT)
                                emit_field((short)g_nfields, FL_SUBMITB);
                            g_forms[cur_form].nf++;
                            g_nfields++;
                            if (ftype != FT_HIDDEN) last_was_space = 1;
                        }
                    }
                } else if (tag_match(t, "td") || tag_match(t, "th")) {
                    if (!closing && !blk_is_empty()) {
                        emit_char(' ');
                        emit_char(' ');
                        last_was_space = 1;
                    }
                }
                continue;
            }
            if (tag_buf_len < (int)sizeof(tag_buf) - 1)
                tag_buf[tag_buf_len++] = c;
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
            int skip = emit_multi_entity(src, i, len);
            if (skip > 0) {
                i += skip;
                last_was_space = 0;
                continue;
            }
            char decoded;
            skip = decode_entity(src, i, len, &decoded);
            if (skip > 0) {
                c = decoded;
                i += skip;
            } else {
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
            if (c == '\n') {
                if (blk_is_empty()) emit_char(' ');   /* keep blank lines */
                blk_open(BT_PRE);
            } else if (c != '\r') {
                emit_char(c == '\t' ? ' ' : c);
            }
            continue;
        }

        if (is_whitespace(c)) {
            if (!last_was_space && !blk_is_empty()) {
                emit_char(' ');
                last_was_space = 1;
            }
            continue;
        }

        last_was_space = 0;
        emit_char(c);
    }

    blk_close();
    g_render[g_render_len] = '\0';

    g_view_mode = VIEW_HTML;
    layout(g_win_w);
}

/* Whole-buffer monospace document (plain text + source view). */
static void render_mono_doc(void) {
    images_free();
    doc_reset();
    g_cur_fl = FL_CODE;
    blk_open(BT_PRE);
    for (long i = 0; i < g_raw_len && g_render_len < RENDER_CAP - 1; i++) {
        char c = g_raw[i];
        if (c == '\n') {
            if (blk_is_empty()) emit_char(' ');
            blk_open(BT_PRE);
        } else if (c == '\t') {
            emit_char(' '); emit_char(' ');
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E) {
            emit_char(c);
        } else if ((unsigned char)c > 0x7E) {
            emit_char('.');
        }
    }
    blk_close();
    g_cur_fl = 0;
    g_render[g_render_len] = '\0';
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
        "<html><head><title>New Tab</title></head><body>"
        "<h1>TobyOS Browser</h1>"
        "<p>Welcome to the TobyOS web browser. This is a Chrome-inspired "
        "browser built into the operating system.</p>"
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
    g_source_view = 0;
    update_title();
    set_status("Ready");
}

/* ---- Tab management --------------------------------------------- */

static void tab_reset(struct tab *t) {
    tab_images_free(t);
    mem_zero(t, sizeof(*t));
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
    for (int i = idx; i < g_ntabs - 1; i++)
        g_tabs[i] = g_tabs[i + 1];        /* struct copy shifts the bundle */
    g_ntabs--;
    /* The shift duplicated the last tab's image pointers into the now-
     * vacated slot; clear it WITHOUT freeing (the live shifted-down tab
     * owns those pixels now) so a future tab_reset can't double-free. */
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
    raw_append("<html><head><title>Problem loading page</title></head><body>"
               "<h1>This site can't be reached</h1><p>");
    raw_append(url);
    raw_append("</p><p>Error: <b>");
    raw_append(emsg);
    raw_append("</b></p><hr><ul>"
               "<li>Check the address for typos</li>"
               "<li>DNS failed? The gateway/DNS server may be unreachable</li>"
               "<li>Connection failed? The host may be down, or HTTPS-only</li>"
               "</ul><p>Press <b>r</b> to retry, <b>[</b> to go back.</p>"
               "</body></html>");
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

    if (g_layout_w != g_win_w && g_nblks > 0) {
        int old_doc = g_doc_h > 0 ? g_doc_h : 1;
        long keep = g_scroll_y;
        layout(g_win_w);
        g_scroll_y = (int)(keep * (long)g_doc_h / old_doc);
    }
    clamp_scroll();
}

/* Approximate line height of a run (hit-testing + visibility). */
static int run_h(const struct run *r) {
    if (r->img >= 0 && r->h > 0) return r->h;
    if (r->field >= 0) return FIELD_H;
    if (r->fl & FL_CODE) return MONO_H + 2;
    return r->px + r->px / 3;
}

/* ---- Find in page ---------------------------------------------- */

static void find_next_match(void) {
    if (g_find_len == 0) return;
    g_find_buf[g_find_len] = '\0';

    /* continue after the current match, wrapping once */
    long start = 0;
    if (g_find_run >= 0 && g_find_run < g_nruns)
        start = g_runs[g_find_run].off + 1;

    long hit = -1;
    int idx = str_contains(&g_render[start], (int)(g_render_len - start),
                           g_find_buf, g_find_len);
    if (idx >= 0) hit = start + idx;
    else {
        idx = str_contains(g_render, (int)g_render_len, g_find_buf, g_find_len);
        if (idx >= 0) hit = idx;
    }
    if (hit < 0) { g_find_run = -1; set_status("Not found"); return; }

    /* find the run containing the hit offset */
    for (int ri = 0; ri < g_nruns; ri++) {
        struct run *r = &g_runs[ri];
        if (r->len > 0 && hit >= r->off && hit < r->off + r->len) {
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

static void paint_image(const struct run *r, int sy) {
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

    /* placeholder / failed box */
    uint32_t bg = 0x0026282Cu, bd = COL_URL_BORDER;
    sys_gui_fill(0, r->x, sy, dw, dh, bg);
    sys_gui_fill(0, r->x, sy, dw, 1, bd);
    sys_gui_fill(0, r->x, sy + dh - 1, dw, 1, bd);
    sys_gui_fill(0, r->x, sy, 1, dh, bd);
    sys_gui_fill(0, r->x + dw - 1, sy, 1, dh, bd);
    const char *lbl = (im->state < 0) ? "[image failed]" : "[loading image...]";
    if (dw > 40 && dh > 16)
        tk_draw_text(&win, r->x + 6, sy + dh / 2 - 7, lbl, COL_URL_HINT, PX_BODY, 0);
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

    /* Content Area: draw the laid-out runs that intersect the viewport */
    sys_gui_fill(fd, 0, CONTENT_TOP, g_win_w, g_content_h, COL_PAGE_BG);

    {
        static char tb[512];
        int vtop = g_scroll_y;
        int vbot = g_scroll_y + g_content_h;

        for (int ri = 0; ri < g_nruns; ri++) {
            struct run *r = &g_runs[ri];
            int rh = run_h(r);
            if (r->y + rh <= vtop) continue;
            if (r->y >= vbot) break;              /* runs are y-sorted */
            int sy = CONTENT_TOP + (r->y - vtop);

            if (r->fl & FL_HRULE) {
                sys_gui_fill(fd, r->x, sy + 2, r->w, 1, COL_HR_FG);
                continue;
            }
            if (r->img >= 0) {
                paint_image(r, sy);
                continue;
            }
            if (r->field >= 0) {
                struct field *ff = &g_fields[r->field];
                if (r->fl & FL_INPUT) {
                    uint32_t bd = (r->field == g_focus_field)
                                      ? COL_URL_FOCUS_BD : COL_URL_BORDER;
                    sys_gui_fill(fd, r->x, sy, r->w, FIELD_H, COL_URL_FOCUS_BG);
                    sys_gui_fill(fd, r->x, sy, r->w, 1, bd);
                    sys_gui_fill(fd, r->x, sy + FIELD_H - 1, r->w, 1, bd);
                    sys_gui_fill(fd, r->x, sy, 1, FIELD_H, bd);
                    sys_gui_fill(fd, r->x + r->w - 1, sy, 1, FIELD_H, bd);
                    /* show the tail of the value if it overflows */
                    const char *v = ff->value;
                    int vl = (int)str_len(v);
                    int avail = r->w - 14;
                    while (vl > 0) {
                        int wpx = 0;
                        for (int k2 = 0; v[k2]; k2++) {
                            unsigned char c2 = (unsigned char)v[k2];
                            wpx += g_adv[FACE_BODY][(c2 >= 32 && c2 <= 126) ? c2 - 32 : 31];
                        }
                        if (wpx <= avail) break;
                        v++; vl--;
                    }
                    if (vl > 0)
                        tk_draw_text(&win, r->x + 6, sy + 5, v, COL_URL_TEXT,
                                     PX_BODY, 0);
                    if (r->field == g_focus_field) {
                        int cx = r->x + 6;
                        for (int k2 = 0; v[k2]; k2++) {
                            unsigned char c2 = (unsigned char)v[k2];
                            cx += g_adv[FACE_BODY][(c2 >= 32 && c2 <= 126) ? c2 - 32 : 31];
                        }
                        if (cx > r->x + r->w - 4) cx = r->x + r->w - 4;
                        sys_gui_fill(fd, cx, sy + 3, 2, FIELD_H - 6, COL_URL_FOCUS_BD);
                    }
                } else {
                    /* submit button */
                    sys_gui_fill(fd, r->x, sy, r->w, FIELD_H, 0x00313845u);
                    sys_gui_fill(fd, r->x, sy, r->w, 1, COL_URL_BORDER);
                    sys_gui_fill(fd, r->x, sy + FIELD_H - 1, r->w, 1, COL_URL_BORDER);
                    sys_gui_fill(fd, r->x, sy, 1, FIELD_H, COL_URL_BORDER);
                    sys_gui_fill(fd, r->x + r->w - 1, sy, 1, FIELD_H, COL_URL_BORDER);
                    tk_draw_text(&win, r->x + 14, sy + 5, ff->value,
                                 0x00EAF0F7u, PX_BODY, 0);
                }
                continue;
            }
            if (r->fl & FL_BULLET) {
                sys_gui_fill(fd, r->x, sy + (rh / 2) - 2, 5, 5, COL_LIST_FG);
                continue;
            }
            if (r->len <= 0) continue;

            int n = r->len < (int)sizeof(tb) - 1 ? r->len : (int)sizeof(tb) - 1;
            for (int k = 0; k < n; k++) {
                char ch = g_render[r->off + k];
                tb[k] = ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E)
                            ? ' ' : ch;
            }
            tb[n] = '\0';

            if (ri == g_find_run)
                sys_gui_fill(fd, r->x - 2, sy - 1, r->w + 4, rh, 0x00554400u);

            if (r->fl & FL_CODE) {
                sys_gui_fill(fd, r->x - 2, sy, r->w + 4, MONO_H, COL_CODE_BG);
                sys_gui_text(fd, r->x, sy, tb, COL_CODE_FG, COL_CODE_BG);
                continue;
            }

            uint32_t fg = COL_TEXT_FG;
            if (r->px == PX_H1)      fg = COL_H1_FG;
            else if (r->px == PX_H2) fg = COL_H2_FG;
            else if (r->px == PX_H3) fg = COL_H3_FG;
            else if (r->fl & FL_LINK) fg = COL_LINK_FG;
            else if (r->fl & FL_BOLD) fg = COL_BOLD_FG;
            else if (r->fl & FL_DIM)  fg = COL_URL_HINT;

            tk_draw_text(&win, r->x, sy, tb, fg, r->px,
                         (r->fl & FL_BOLD) || r->px != PX_BODY);
            if (r->fl & FL_LINK)
                sys_gui_fill(fd, r->x, sy + r->px + 2, r->w, 1, COL_LINK_FG);
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

/* Doc-space hit-test: interactive run (link or form control) under
 * client (mx,my), or NULL. */
static struct run *run_at(int mx, int my) {
    int dy = my - CONTENT_TOP + g_scroll_y;
    for (int ri = 0; ri < g_nruns; ri++) {
        struct run *r = &g_runs[ri];
        if (r->y > dy) break;
        if (r->link < 0 && r->field < 0) continue;
        int rh = (r->field >= 0) ? FIELD_H : run_h(r);
        if (dy >= r->y && dy < r->y + rh &&
            mx >= r->x && mx < r->x + r->w)
            return r;
    }
    return NULL;
}

static int run_link_at(int mx, int my) {
    struct run *r = run_at(mx, my);
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

        /* Inline link / form-control click: hit-test the runs */
        struct run *hit = run_at(mx, my);
        if (hit && hit->field >= 0) {
            g_focus_url = 0;
            if (hit->fl & FL_INPUT) {
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
            redraw(0);
            return;
        } else if (key >= 0x20 && key <= 0x7E) {
            int l = (int)str_len(f->value);
            if (l < (int)sizeof(f->value) - 1) {
                f->value[l] = (char)key;
                f->value[l + 1] = 0;
            }
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
                for (int ri = 0; ri < g_nruns; ri++) {
                    if (g_runs[ri].field == found) {
                        g_scroll_y = g_runs[ri].y - g_content_h / 3;
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
        if (!load_one_pending_image())
            sys_sleep_ms(15);              /* nothing pending -> idle */
    }
    return 0;
}
