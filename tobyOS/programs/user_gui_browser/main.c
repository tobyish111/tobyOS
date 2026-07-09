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
#include <toby/audio_decode.h>  /* real minimp3 MP3 decode (stage 13 media) */
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

/* ---- Async HTTP (stage 12D): start/poll/read/finish -------------- *
 * The kernel drives the transfer on a worker; the browser polls the
 * handle from its main loop and NEVER blocks on the network. Structs
 * mirror abi_http_start / abi_http_poll (ABI-frozen). */
#define SYS_HTTP_START  172
#define SYS_HTTP_POLL   173
#define SYS_HTTP_READ   174
#define SYS_HTTP_FINISH 175

struct http_start {
    unsigned long url;
    uint32_t      max_body;
    uint32_t      flags;
};
#define HTTPA_QUEUED  0
#define HTTPA_RUNNING 1
#define HTTPA_DONE    2
#define HTTPA_ERROR   3
struct http_poll {
    int32_t  state;             /* HTTPA_* */
    int32_t  err;               /* HTTP_ERR_* when state == ERROR */
    int32_t  status;
    uint32_t body_len;
    uint32_t body_total;
    uint8_t  content_encoding;
    uint8_t  reserved0[3];
    char     final_url[512];
    char     content_type[64];
};

static inline long sys_http_start(const char *url, uint32_t max_body) {
    struct http_start rq;
    rq.url = (unsigned long)url;
    rq.max_body = max_body;
    rq.flags = 0;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_START), "D"(&rq)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_http_poll(long h, struct http_poll *out) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_POLL), "D"(h), "S"(out)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_http_read(long h, void *buf, uint32_t off, uint32_t len) {
    unsigned long off_len = ((unsigned long)off << 32) | (unsigned long)len;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_READ), "D"(h), "S"(buf), "d"(off_len)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_http_finish(long h) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_FINISH), "D"(h)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- WebSocket (stage 13): open/poll/recv/send/close -------------- *
 * Kernel-side RFC 6455 client on the async worker; the browser polls
 * handles from the pump and never blocks. Structs mirror abi_ws_open /
 * abi_ws_poll (ABI-frozen). */
#define SYS_WS_OPEN   177
#define SYS_WS_POLL   178
#define SYS_WS_RECV   179
#define SYS_WS_SEND   180
#define SYS_WS_CLOSE  181

#define WS_ST_CONNECTING 0
#define WS_ST_OPEN       1
#define WS_ST_CLOSING    2
#define WS_ST_CLOSED     3

struct ws_open_rq {
    unsigned long url;
    uint32_t      flags;
    uint32_t      reserved;
};
struct ws_poll_info {
    int32_t  state;             /* WS_ST_* */
    int32_t  err;               /* 0 = clean */
    uint32_t msg_avail;
    uint32_t msg_len;
    uint32_t msg_type;          /* 1 text, 2 binary */
    uint32_t queued;
    uint16_t close_code;
    uint16_t reserved0;
    uint32_t reserved1;
};

static inline long sys_ws_open(const char *url) {
    struct ws_open_rq rq;
    rq.url = (unsigned long)url;
    rq.flags = 0;
    rq.reserved = 0;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WS_OPEN), "D"(&rq)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_ws_poll(long h, struct ws_poll_info *out) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WS_POLL), "D"(h), "S"(out)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_ws_recv(long h, void *buf, uint32_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WS_RECV), "D"(h), "S"(buf), "d"((unsigned long)cap)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_ws_send(long h, const void *buf, uint32_t len, int op) {
    unsigned long op_len = ((unsigned long)(unsigned)op << 32) | (unsigned long)len;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WS_SEND), "D"(h), "S"(buf), "d"(op_len)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_ws_close(long h, int code) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WS_CLOSE), "D"(h), "S"((long)code)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- Audio engine (stage 13 media): open/write/close --------------- *
 * PCM16 stream into the kernel mixer (audio_engine.c). WRITE is a
 * non-blocking ring write returning bytes accepted -- the media pump
 * throttles on that. */
#define SYS_AUDIO_OPEN  143
#define SYS_AUDIO_WRITE 144
#define SYS_AUDIO_CLOSE 145
#define AUDIO_FMT_PCM16 1

static inline long sys_audio_open(uint32_t rate, int channels, int fmt) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_AUDIO_OPEN), "D"((long)rate), "S"((long)channels),
          "d"((long)fmt)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_audio_write(int fd, const void *samples, unsigned long bytes) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_AUDIO_WRITE), "D"((long)fd), "S"(samples), "d"(bytes)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_audio_close(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_AUDIO_CLOSE), "D"((long)fd)
        : "rcx", "r11", "memory");
    return r;
}

/* File syscalls (stage 12E: persistent localStorage). */
#define SYS_READ   2
#define SYS_CLOSE  4
#define SYS_OPEN  35
#define SYS_MKDIR 42
#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_CREAT   0x040
#define O_TRUNC   0x200

static inline long sys_open(const char *path, int flags, int mode) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_OPEN), "D"(path), "S"((long)flags), "d"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_read(int fd, void *buf, unsigned long len) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_READ), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_close(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOSE), "D"((long)fd)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_mkdir(const char *path, int mode) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_MKDIR), "D"(path), "S"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- Web fonts (stage 13E): register + face-aware text ------------- */
#define SYS_GUI_TEXT_TTF        94
#define SYS_GUI_TEXT_TTF_WIDTH  95
#define SYS_GUI_FONT_REGISTER  176

/* Register a downloaded TTF/OTF blob; returns a kernel face id (>=4) or <0. */
static inline long sys_gui_font_register(const void *buf, unsigned long len) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FONT_REGISTER), "D"(buf), "S"(len)
        : "rcx", "r11", "memory");
    return r;
}
/* Draw TTF text with an explicit face (bundled 0-3 or a web face). */
static inline int sys_text_ttf(int fd, int x, int y, const char *s,
                               uint32_t fg, int px, int face) {
    unsigned long xy = ((unsigned long)(x & 0xFFFF)) |
                       ((unsigned long)(y & 0xFFFF) << 16);
    unsigned long pf = ((unsigned long)(px & 0xFFFF)) |
                       ((unsigned long)(face & 0xFF) << 16);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)pf;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT_TTF), "D"((long)fd), "S"(xy),
          "d"(s), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_text_ttf_width(const char *s, int px, int face) {
    unsigned long pf = ((unsigned long)(px & 0xFFFF)) |
                       ((unsigned long)(face & 0xFF) << 16);
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT_TTF_WIDTH), "D"(s), "S"(pf)
        : "rcx", "r11", "memory");
    return (int)r;
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
#define ADV_SLOTS 16
struct advtab { short adv[95]; short px; short face; short used; };
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
#define D_FLEX      10           /* flex container (row/column layout)  */
#define D_GRID      11           /* grid container (track-based layout)  */

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
    uint8_t  pos;                /* 0 static, 1 relative, 2 absolute, 3 fixed */
    int16_t  po[4];              /* top/right/bottom/left offsets; M_AUTO */
    /* flexbox (containers: fdir/fwrap/fjust/falign/gap; items: f*) */
    uint8_t  fdir;               /* 0 row, 1 column */
    uint8_t  fwrap;              /* flex-wrap: wrap */
    uint8_t  fjust;              /* 0 start, 1 end, 2 center, 3 between */
    uint8_t  falign;             /* 0 stretch/top, 1 start, 2 center, 3 end */
    uint8_t  fgrow;              /* flex-grow (integer part) */
    uint8_t  fshrink;            /* flex-shrink (integer part, default 1) */
    int16_t  gap;                /* gap between items/lines, px */
    int32_t  fbasis;             /* flex-basis px (content box), -1 auto */
    int16_t  woff;               /* calc() px offset added after width% resolves */
    /* grid: container templates (raw text in g_gridpool) + item spans */
    int32_t  gtc_off;            /* grid-template-columns raw text offset */
    int32_t  gtr_off;            /* grid-template-rows raw text offset */
    int16_t  gtc_len, gtr_len;   /* 0 = none */
    uint8_t  gcol_span, grow_span; /* item column/row span (default 1) */
    uint8_t  ovf;                /* overflow: 1 = clip (hidden/scroll/auto) */
    uint8_t  has_tf;             /* transform: translate present */
    int16_t  txpx, typx;         /* transform translate px */
    int8_t   txpct, typct;       /* transform translate % (of own size) */
    uint8_t  po_pct;             /* top/right/bottom/left are % (bits T R B L) */
    int8_t   webface;            /* @font-face kernel face id, -1 = none */
};

/* ---- DOM ---------------------------------------------------------- */

#define DNF_STYLE_DONE 1     /* <style> content already parsed into rules */
#define DNF_LAID_FIXED 2     /* laid rect is viewport coords (pos: fixed) */
#define DNF_LAID_BLOCK 4     /* laid rect came from a block stamp (exact
                                border box), not an item union */

struct dnode {
    int32_t parent, first, last, next;   /* tree links (node idx, -1) */
    int32_t toff, tlen;          /* T_TEXT: tpool slice (decoded) */
    int32_t attr0;               /* first attr in attr pool */
    int16_t nattr;
    int16_t tag;                 /* T_* */
    int16_t link, field, img;    /* interaction indices or -1 */
    int16_t flags;               /* DNF_* */
    /* stage 13F: laid border box (doc coords; viewport coords when
     * DNF_LAID_FIXED). Valid only while lgen == eng->lay_gen. */
    int32_t lx, ly, lw, lh;
    int32_t lgen;
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
    uint8_t prop;                /* CP_* (CP_VAR for --custom-property) */
    uint8_t imp;                 /* !important */
    int16_t vlen;
    int32_t voff;                /* raw value text in csspool */
    int32_t noff;                /* CP_VAR: --name text in csspool */
    int16_t nlen;                /* CP_VAR: name length */
    int16_t _pad;
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
    CP_FLEXDIR, CP_FLEXWRAP, CP_FLEXFLOW, CP_JUSTC, CP_ALITEMS,
    CP_FLEX, CP_FGROW, CP_FSHRINK, CP_FBASIS, CP_GAP,
    CP_POSITION, CP_TOP, CP_RIGHT, CP_BOTTOM, CP_LEFT,
    CP_VAR,                      /* --custom-property (resolved via var()) */
    CP_GRID_TC, CP_GRID_TR, CP_GRID_COL, CP_GRID_ROW,
    CP_OVERFLOW, CP_OVERFLOWX, CP_OVERFLOWY, CP_TRANSFORM,
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
#define IF_FIXED   0x20          /* position:fixed -- viewport coords,
                                    painted/hit-tested without scroll */

struct ditem {
    int32_t x, y, w, h;          /* doc coords */
    uint32_t fg, bg;             /* text fg / rect color; text bg (0=none) */
    int32_t off;                 /* render-pool slice (DI_TEXT) */
    int32_t node;                /* source DOM node (JS event target), -1 */
    int16_t len;
    int16_t link, field, img;    /* interaction indices or -1 */
    uint8_t kind, px, fl;
    uint8_t clip;                /* index into g_clips (0 = no clip) */
    uint8_t face;                /* kernel text face (web font / bold) */
    uint8_t _pad3[3];
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
    int    lay_gen;                 /* bumped per layout(); validates
                                       the nodes' laid rects (13F) */
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

/* Stage 12D: a pending JS fetch() -- the async-HTTP handle plus the
 * JS callback that settles the Promise, called from the pump. */
#define JS_FETCH_MAX 8
struct jsfetch {
    int      used;
    int      handle;
    JSValue  cb;
};

/* Stage 13: a live JS WebSocket -- the kernel handle plus the JS
 * object whose __fire(type, ...) the pump calls for open / message /
 * close / error events. */
#define JS_WS_MAX 4
struct jsws {
    int      used;
    int      handle;
    int      last_state;         /* WS_ST_* seen by the pump */
    JSValue  obj;
};

/* Stage 13: <video>/<audio> media elements. A <video> owns an ARGB
 * backing store (the canvas/img pattern) that MJPEG-in-AVI frames
 * decode into on a wall-clock schedule; an <audio> decodes MP3
 * (real minimp3 via libtoby) into a kernel audio-engine stream.
 * The whole file is fetched async (one in flight, like images). */
#define MEDIA_MAX        2            /* media elements per tab */
#define MEDIA_FETCH_CAP  (4u << 20)   /* per-file download cap */
#define MEDIA_FRAMES_MAX 512
#define MEDIA_PCM_CARRY  (1152 * 2)   /* int16s: one MP3 frame, stereo */

struct media_el {
    int      used;
    int      node;                /* dnode index */
    int      img;                 /* backing-store img index (video) */
    int      is_video;
    int      autoplay, loop;
    int      state;               /* 0 pending, 2 in-flight, 1 ready, -1 failed */
    int      playing;
    char     src[512];
    uint8_t *data;                /* the whole media file */
    long     dlen;
    /* video: MJPEG-in-AVI frame index */
    long     frame_off[MEDIA_FRAMES_MAX];
    int      frame_len[MEDIA_FRAMES_MAX];
    int      nframes, cur_frame;
    long     frame_ms, next_ms;
    /* audio: MP3 (minimp3) -> kernel PCM16 stream */
    void    *mp3;                 /* toby_mp3_decoder_t* */
    int      audio_fd;
    long     apos;                /* read offset into data */
    int16_t  carry[MEDIA_PCM_CARRY];  /* decoded-but-unwritten PCM */
    int      carry_off, carry_len;    /* bytes */
};

/* Stage 13F: DOM mutation log for MutationObserver delivery. Each JS
 * mutation primitive records (node, kind); the pump hands the batch to
 * the prelude's observer check. Kinds: 0 childList, 1 attributes,
 * 2 characterData. */
#define MUT_MAX 64

/* Stage 12D: async navigation continuations (what the pump does when
 * the page fetch completes / fails). Mirrors the old synchronous
 * chains: do_navigate, do_fetch_url, the DDG->Mojeek search fallback,
 * and navigate_input's https/http/www bare-host ladder. */
#define NAVK_NORMAL 0            /* render + history_push; error page + push */
#define NAVK_PLAIN  1            /* back/forward/refresh: render; no push */
#define NAVK_DDG    2            /* search leg 1: 202/error -> Mojeek */
#define NAVK_HOST0  3            /* https://input */
#define NAVK_HOST1  4            /* https://www.input (DNS retry) */
#define NAVK_HOST2  5            /* http://www.input */
#define NAVK_HOST3  6            /* http://input (https answered-but-failed) */

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
    struct jsfetch js_fetches[JS_FETCH_MAX];  /* pending fetch() calls */
    struct jsws js_ws[JS_WS_MAX];             /* live WebSocket objects */
    struct media_el media[MEDIA_MAX]; int nmedia;  /* <video>/<audio> */
    /* stage 13F: observers (Mutation/Intersection/Resize + scroll) */
    JSValue    js_obs_check;   /* prelude observer-check callback */
    int        js_has_obs_check;
    int32_t    mut_node[MUT_MAX];
    uint8_t    mut_kind[MUT_MAX];
    int        mut_n;
    uint8_t    obs_kick;       /* a JS observe() wants a check pass */
    int        obs_gen;        /* eng->lay_gen at the last check */
    int        obs_scroll;     /* scroll_y at the last check */
    /* stage 12D: async page navigation (poll-driven from the loop) */
    int    nav_handle;                /* async-HTTP handle or -1 */
    int    nav_kind;                  /* NAVK_* continuation */
    char   nav_url[URL_MAX + 1];      /* current leg's target URL */
    char   nav_input[256];            /* bare-host input / search query */
    struct form forms[FORM_MAX];    int nforms;
    struct field fields[FIELD_MAX]; int nfields; int focus_field;
    struct img  images[IMG_MAX];    int nimages;
    char   links[LINK_MAX][LINK_URL_MAX]; int link_count;
    char   url[URL_MAX + 1];   int url_len;
    char   title[TITLE_MAX + 1];
    char   history[HISTORY_MAX][URL_MAX + 1]; int hist_pos, hist_count;
    /* stage 12E: same-document history (pushState/popstate). doc_gen
     * bumps on every real render; entries carry the generation they
     * belong to, so back/forward can tell "fire popstate" from
     * "refetch". */
    int    doc_gen;
    int    hist_gen[HISTORY_MAX];
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

#define KFONT_WEB_BASE 4       /* face ids >= this are registered web fonts */

static const short *adv_for(int px, int face) {
    if (px < 8) px = 8;
    if (px > 40) px = 40;
    for (int i = 0; i < ADV_SLOTS; i++)
        if (g_advc[i].used && g_advc[i].px == px && g_advc[i].face == face)
            return g_advc[i].adv;
    struct advtab *t = &g_advc[g_advc_rr];
    g_advc_rr = (g_advc_rr + 1) % ADV_SLOTS;
    char one[2] = { 0, 0 };
    for (int c = 0; c < 95; c++) {
        one[0] = (char)(32 + c);
        int w = (face >= KFONT_WEB_BASE) ? sys_text_ttf_width(one, px, face)
                                         : tk_text_width(one, px, face ? 1 : 0);
        t->adv[c] = (short)(w > 0 ? w : px / 2);
    }
    t->px = (short)px; t->face = (short)face; t->used = 1;
    return t->adv;
}

/* Resolved kernel face for a run: a web font wins; else bold/regular. */
static int run_face(int webface, int bold) {
    if (webface >= 0) return webface;
    return bold ? 1 : 0;
}

static int text_px_w(const char *s, int len, int px, int face, int mono) {
    if (mono) return len * MONO_W;
    const short *adv = adv_for(px, face);
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

/* ---- calc() evaluator (stage 13B) ---------------------------------- *
 * Evaluates a calc(...) expression into an absolute px part + a
 * percentage part, so a mixed `calc(100% - 250px)` is representable.
 * Recursive descent: +,- (lowest) then *,/ then factor (number+unit or
 * a parenthesized subexpression). One operand of * or / must be
 * unitless. Units: px em rem pt ch vw vh %, and plain numbers. */

struct calcval { double px, pct; int is_num; };
struct calccur { const char *s; int i, n, own_px; };

static void calc_ws(struct calccur *c) {
    while (c->i < c->n && is_whitespace(c->s[c->i])) c->i++;
}

static int calc_expr(struct calccur *c, struct calcval *out);

static int calc_factor(struct calccur *c, struct calcval *out) {
    calc_ws(c);
    if (c->i < c->n && c->s[c->i] == '(') {
        c->i++;
        if (!calc_expr(c, out)) return 0;
        calc_ws(c);
        if (c->i < c->n && c->s[c->i] == ')') c->i++;
        return 1;
    }
    int neg = 0;
    if (c->i < c->n && (c->s[c->i] == '-' || c->s[c->i] == '+')) {
        neg = (c->s[c->i] == '-'); c->i++;
    }
    calc_ws(c);
    int digits = 0;
    double val = 0;
    while (c->i < c->n && c->s[c->i] >= '0' && c->s[c->i] <= '9') {
        val = val * 10 + (c->s[c->i] - '0'); c->i++; digits++;
    }
    if (c->i < c->n && c->s[c->i] == '.') {
        c->i++;
        double f = 0.1;
        while (c->i < c->n && c->s[c->i] >= '0' && c->s[c->i] <= '9') {
            val += (c->s[c->i] - '0') * f; f *= 0.1; c->i++; digits++;
        }
    }
    if (!digits) return 0;
    if (neg) val = -val;
    out->px = 0; out->pct = 0; out->is_num = 0;
    if (c->i < c->n && c->s[c->i] == '%') { out->pct = val; c->i++; return 1; }
    int u = c->i, ue = u;
    while (ue < c->n && lc(c->s[ue]) >= 'a' && lc(c->s[ue]) <= 'z') ue++;
    int ul = ue - u;
    const char *un = c->s + u;
    if (ul == 0) { out->is_num = 1; out->px = val; return 1; }
    if (ul == 2 && lc(un[0]) == 'p' && lc(un[1]) == 'x') out->px = val;
    else if (ul == 2 && lc(un[0]) == 'e' && lc(un[1]) == 'm') out->px = val * c->own_px;
    else if (ul == 3 && str_ncasecmp(un, "rem", 3) == 0) out->px = val * PX_BODY;
    else if (ul == 2 && lc(un[0]) == 'p' && lc(un[1]) == 't') out->px = val * 4.0 / 3.0;
    else if (ul == 2 && lc(un[0]) == 'c' && lc(un[1]) == 'h') out->px = val * (c->own_px / 2.0);
    else if (ul == 2 && lc(un[0]) == 'v' && lc(un[1]) == 'w') out->px = val * g_win_w / 100.0;
    else if (ul == 2 && lc(un[0]) == 'v' && lc(un[1]) == 'h') out->px = val * g_win_h / 100.0;
    else return 0;
    c->i = ue;
    return 1;
}

static int calc_term(struct calccur *c, struct calcval *out) {
    if (!calc_factor(c, out)) return 0;
    for (;;) {
        calc_ws(c);
        if (c->i >= c->n) break;
        char op = c->s[c->i];
        if (op != '*' && op != '/') break;
        c->i++;
        struct calcval r;
        if (!calc_factor(c, &r)) return 0;
        if (op == '*') {
            if (out->is_num) {
                double k = out->px;
                out->px = r.px * k; out->pct = r.pct * k; out->is_num = r.is_num;
            } else if (r.is_num) {
                out->px *= r.px; out->pct *= r.px;
            } else return 0;              /* length * length is invalid */
        } else {
            if (!r.is_num || r.px == 0) return 0;
            out->px /= r.px; out->pct /= r.px;
        }
    }
    return 1;
}

static int calc_expr(struct calccur *c, struct calcval *out) {
    if (!calc_term(c, out)) return 0;
    for (;;) {
        calc_ws(c);
        if (c->i >= c->n) break;
        char op = c->s[c->i];
        if (op != '+' && op != '-') break;
        c->i++;
        struct calcval r;
        if (!calc_term(c, &r)) return 0;
        if (op == '+') { out->px += r.px; out->pct += r.pct; }
        else           { out->px -= r.px; out->pct -= r.pct; }
        out->is_num = out->is_num && r.is_num;
    }
    return 1;
}

/* If [s,len) is a calc(...) token, evaluate it. Returns 1 and fills the
 * px + pct parts (rounded), else 0. */
static int css_calc(const char *s, int len, int own_px, int *out_px, int *out_pct) {
    if (len < 6 || str_ncasecmp(s, "calc(", 5) != 0) return 0;
    struct calccur c = { s + 5, 0, len - 5, own_px };
    struct calcval v;
    if (!calc_expr(&c, &v)) return 0;
    *out_px  = (int)(v.px  + (v.px  >= 0 ? 0.5 : -0.5));
    *out_pct = (int)(v.pct + (v.pct >= 0 ? 0.5 : -0.5));
    return 1;
}

/* px offset from the last mixed calc() parsed by css_len_tok; the
 * width handler folds it into cstyle.woff (applied after % resolves). */
static int g_calc_off_px;

static int css_len_tok(const char *s, int len, int own_px, int *out) {
    if (len == 4 && str_ncasecmp(s, "auto", 4) == 0) return LK_AUTO;
    g_calc_off_px = 0;
    if (len >= 6 && (s[0] == 'c' || s[0] == 'C') &&
        str_ncasecmp(s, "calc(", 5) == 0) {
        int cpx, cpct;
        if (css_calc(s, len, own_px, &cpx, &cpct)) {
            if (cpct != 0) { *out = cpct; g_calc_off_px = cpx; return LK_PCT; }
            *out = cpx; return LK_PX;
        }
        return LK_BAD;
    }
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
        {"flex-direction",CP_FLEXDIR},{"flex-wrap",CP_FLEXWRAP},
        {"flex-flow",CP_FLEXFLOW},{"justify-content",CP_JUSTC},
        {"align-items",CP_ALITEMS},{"flex",CP_FLEX},
        {"flex-grow",CP_FGROW},{"flex-shrink",CP_FSHRINK},
        {"flex-basis",CP_FBASIS},{"gap",CP_GAP},{"column-gap",CP_GAP},
        {"row-gap",CP_GAP},{"grid-gap",CP_GAP},{"grid-column-gap",CP_GAP},
        {"grid-row-gap",CP_GAP},
        {"position",CP_POSITION},{"top",CP_TOP},{"right",CP_RIGHT},
        {"bottom",CP_BOTTOM},{"left",CP_LEFT},
        {"grid-template-columns",CP_GRID_TC},{"grid-template-rows",CP_GRID_TR},
        {"grid-column",CP_GRID_COL},{"grid-row",CP_GRID_ROW},
        {"overflow",CP_OVERFLOW},{"overflow-x",CP_OVERFLOWX},
        {"overflow-y",CP_OVERFLOWY},{"transform",CP_TRANSFORM},
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
        /* --custom-property: keep the name (needed at var() resolve),
         * everything else drops the name (transient, pool reused). */
        int is_var = (pl >= 2 && E->csspool[po] == '-' && E->csspool[po + 1] == '-');
        int var_noff = po, var_nlen = pl;
        int prop;
        if (is_var) {
            prop = CP_VAR;                /* keep name bytes in the pool */
        } else {
            E->csspool_len = po;          /* name is transient */
            prop = prop_lookup(&E->csspool[po], pl);
        }
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
            d->noff = is_var ? var_noff : 0;
            d->nlen = is_var ? (int16_t)var_nlen : 0;
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
            if (l == 4 && str_ncasecmp(pp, "root", 4) == 0) {
                p->tag = T_HTML;          /* :root == the html element */
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

/* ---- Web fonts (@font-face, stage 13E) ----------------------------- *
 * A session cache: each entry is a font family declared by @font-face
 * with a downloadable src url and (once loaded) a kernel face id. The
 * table persists across page loads (keyed by url) so re-visits reuse
 * the registered kernel face instead of consuming a new slot. v1
 * downloads raw TTF/OTF sources; WOFF/WOFF2 sources are skipped (they
 * need decompression) and such a family falls back to the default. */
#define WEBFONT_MAX 12
struct webfont {
    char    family[64];
    char    url[URL_MAX + 1];
    int     face;                /* kernel face id, -1 until registered */
    uint8_t tried;               /* download attempted (success or fail) */
    uint8_t used;
};
static struct webfont g_webfonts[WEBFONT_MAX];
static int g_nwebfonts;

/* Case-insensitive substring index or -1. */
static int ci_find(const char *hay, int hl, const char *needle) {
    int nl = (int)str_len(needle);
    for (int i = 0; i + nl <= hl; i++) {
        int k = 0;
        while (k < nl && lc(hay[i + k]) == lc(needle[k])) k++;
        if (k == nl) return i;
    }
    return -1;
}

/* Extract the first raw-TTF/OTF url from a `src:` value. Prefers a url
 * whose format() is truetype/opentype or whose path ends .ttf/.otf;
 * returns 0 if only compressed (woff/woff2) sources are present. */
static int webfont_pick_url(const char *s, int n, char *out, int cap) {
    int best = 0, i = 0;
    while (i + 4 <= n) {
        int u = ci_find(s + i, n - i, "url(");
        if (u < 0) break;
        int p = i + u + 4;
        while (p < n && (s[p] == ' ' || s[p] == '"' || s[p] == '\'')) p++;
        int e = p;
        while (e < n && s[e] != ')' && s[e] != '"' && s[e] != '\'') e++;
        int ulen = e - p;
        /* look at the trailing format(...) or extension */
        int after = e;
        while (after < n && s[after] != ',' && s[after] != ';') after++;
        int seg_len = after - e;
        int is_ttf = 0;
        if (ci_find(s + e, seg_len, "truetype") >= 0 ||
            ci_find(s + e, seg_len, "opentype") >= 0) is_ttf = 1;
        if (ulen >= 4 && (ci_find(s + p + ulen - 4, 4, ".ttf") >= 0 ||
                          ci_find(s + p + ulen - 4, 4, ".otf") >= 0)) is_ttf = 1;
        /* WOFF2 is now decodable (kernel woff2.c); WOFF1 still isn't. */
        int is_woff2 = (ci_find(s + e, seg_len, "woff2") >= 0) ||
                       (ulen >= 6 && ci_find(s + p + ulen - 6, 6, ".woff2") >= 0);
        int is_woff1 = !is_woff2 &&
                       ((ci_find(s + e, seg_len, "woff") >= 0) ||
                        (ulen >= 5 && ci_find(s + p + ulen - 5, 5, ".woff") >= 0));
        if (is_ttf && ulen > 0 && ulen < cap) {   /* raw sfnt: best, use now */
            memcpy(out, s + p, ulen); out[ulen] = 0;
            return 1;
        }
        if (is_woff2 && ulen > 0 && ulen < cap) {  /* decodable: strong pick */
            memcpy(out, s + p, ulen); out[ulen] = 0;
            best = 1;
        } else if (!is_woff1 && !best && ulen > 0 && ulen < cap) {
            memcpy(out, s + p, ulen); out[ulen] = 0;
            best = 1;                       /* unknown format: tentative */
        }
        i = after + 1;
    }
    return best;
}

/* Parse one @font-face block body: capture family + a usable src url. */
static void webfont_parse_block(const char *s, int n) {
    if (g_nwebfonts >= WEBFONT_MAX) return;
    char family[64] = {0};
    char url[URL_MAX + 1] = {0};
    /* font-family: "Name"; */
    int ff = ci_find(s, n, "font-family");
    if (ff >= 0) {
        int p = ff + 11;
        while (p < n && (s[p] == ' ' || s[p] == ':')) p++;
        while (p < n && (s[p] == '"' || s[p] == '\'')) p++;
        int e = p, o = 0;
        while (e < n && s[e] != ';' && s[e] != '"' && s[e] != '\'' &&
               o < (int)sizeof(family) - 1)
            family[o++] = (char)lc(s[e++]);
        while (o > 0 && family[o - 1] == ' ') o--;
        family[o] = 0;
    }
    /* src: ... */
    int sr = ci_find(s, n, "src");
    if (sr >= 0) {
        int p = sr + 3;
        int e = p;
        while (e < n && s[e] != ';') e++;
        webfont_pick_url(s + p, e - p, url, sizeof(url));
    }
    if (!family[0] || !url[0]) return;
    for (int k = 0; k < g_nwebfonts; k++)   /* dedup by url */
        if (str_eq(g_webfonts[k].url, url)) return;
    struct webfont *w = &g_webfonts[g_nwebfonts++];
    str_copy(w->family, family, sizeof(w->family));
    str_copy(w->url, url, sizeof(w->url));
    w->face = -1; w->tried = 0; w->used = 1;
}

/* Face id registered for a font-family, or -1. */
static int webfont_face_for(const char *family, int flen) {
    for (int k = 0; k < g_nwebfonts; k++) {
        if (g_webfonts[k].face < 0) continue;
        int fl = (int)str_len(g_webfonts[k].family);
        if (fl == flen && ci_find(family, flen, g_webfonts[k].family) == 0)
            return g_webfonts[k].face;
    }
    return -1;
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
            if (l == 9 && str_ncasecmp(&E->csspool[o], "font-face", 9) == 0) {
                while (c.i < c.n && c.s[c.i] != '{' && c.s[c.i] != ';') c.i++;
                if (c.i >= c.n || c.s[c.i] == ';') {
                    if (c.i < c.n) c.i++;
                    continue;
                }
                c.i++;
                long b0 = c.i;
                int depth = 1;
                while (c.i < c.n && depth) {
                    if (c.s[c.i] == '{') depth++;
                    else if (c.s[c.i] == '}') depth--;
                    c.i++;
                }
                long b1 = depth ? c.i : c.i - 1;
                webfont_parse_block(c.s + b0, (int)(b1 - b0));
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
    st->pos = 0;
    for (int i = 0; i < 4; i++) st->po[i] = M_AUTO;
    st->fdir = 0;
    st->fwrap = 0;
    st->fjust = 0;
    st->falign = 0;
    st->fgrow = 0;
    st->fshrink = 1;
    st->gap = 0;
    st->fbasis = -1;
    st->woff = 0;
    st->gtc_len = 0;
    st->gtr_len = 0;
    st->gcol_span = 1;
    st->grow_span = 1;
    /* grid template offsets are only read when gtc_len/gtr_len > 0 */
    st->ovf = 0;
    st->has_tf = 0;
    st->txpx = st->typx = 0;
    st->txpct = st->typct = 0;
    st->po_pct = 0;
    st->webface = pst ? pst->webface : -1;   /* font-family inherits */
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

/* ---- CSS custom properties + var() (stage 13) ---------------------- *
 * Custom properties (--name) cascade + inherit like normal properties.
 * They are collected into a scoped stack during style_node (an entry
 * per definition, name+value copied into a stable pool so they survive
 * the per-node csspool rollback), and var(--name[, fallback]) uses are
 * substituted on the raw value string just before a normal declaration
 * is parsed. Scoping is honored: a node pushes its vars, styles itself
 * and its subtree, then pops -- so a var set on one subtree doesn't
 * leak to siblings, and descendants inherit ancestor vars. */

#define VARSCOPE_MAX 512
#define VARPOOL_CAP  (64 * 1024)
struct varent { int noff, nlen, voff, vlen; };    /* offsets into g_varpool */
static struct varent g_varscope[VARSCOPE_MAX];
static int  g_nvarscope;
static char g_varpool[VARPOOL_CAP];
static int  g_varpool_len;

/* Grid templates are variable-length text; kept in a stable pool per
 * style pass (cstyle stores offset+len) so lay_grid can parse them at
 * layout time, after the per-node csspool rollback. */
#define GRIDPOOL_CAP (16 * 1024)
static char g_gridpool[GRIDPOOL_CAP];
static int  g_gridpool_len;

static int gridpool_put(const char *s, int n) {
    if (n < 0) n = 0;
    if (g_gridpool_len + n + 1 > GRIDPOOL_CAP) return -1;
    int off = g_gridpool_len;
    for (int i = 0; i < n; i++) g_gridpool[g_gridpool_len++] = s[i];
    g_gridpool[g_gridpool_len++] = 0;
    return off;
}

static void var_scope_reset(void) {
    g_nvarscope = 0; g_varpool_len = 0; g_gridpool_len = 0;
}

static void var_push(const char *name, int nlen, const char *val, int vlen) {
    if (g_nvarscope >= VARSCOPE_MAX) return;
    if (nlen < 0) nlen = 0;
    if (vlen < 0) vlen = 0;
    if (g_varpool_len + nlen + vlen > VARPOOL_CAP) return;
    struct varent *e = &g_varscope[g_nvarscope++];
    e->noff = g_varpool_len;
    e->nlen = nlen;
    for (int i = 0; i < nlen; i++) g_varpool[g_varpool_len++] = name[i];
    e->voff = g_varpool_len;
    e->vlen = vlen;
    for (int i = 0; i < vlen; i++) g_varpool[g_varpool_len++] = val[i];
}

/* Most-recent (highest-priority / nearest-scope) definition wins. */
static int var_lookup(const char *name, int nlen, const char **out, int *outlen) {
    for (int i = g_nvarscope - 1; i >= 0; i--) {
        struct varent *e = &g_varscope[i];
        if (e->nlen != nlen) continue;
        int k = 0;
        while (k < nlen && g_varpool[e->noff + k] == name[k]) k++;
        if (k == nlen) { *out = &g_varpool[e->voff]; *outlen = e->vlen; return 1; }
    }
    return 0;
}

static int var_subst(const char *v, int n, char *out, int cap, int depth);

/* Expand every var(--name[, fallback]) in [v,v+n) into `out`. Returns
 * the written length. Unknown vars with no fallback expand to empty.
 * Recurses (depth-limited) so a var value may itself use var(). */
static int var_subst(const char *v, int n, char *out, int cap, int depth) {
    int o = 0, i = 0;
    while (i < n && o < cap - 1) {
        if (i + 4 <= n && v[i] == 'v' && v[i+1] == 'a' && v[i+2] == 'r' &&
            v[i+3] == '(') {
            i += 4;
            while (i < n && is_whitespace(v[i])) i++;
            int ns = i;
            while (i < n && v[i] != ',' && v[i] != ')') i++;
            int ne = i;
            while (ne > ns && is_whitespace(v[ne - 1])) ne--;
            int fb_s = -1, fb_e = -1;
            if (i < n && v[i] == ',') {          /* fallback present */
                i++;
                while (i < n && is_whitespace(v[i])) i++;
                fb_s = i;
                int par = 0;
                while (i < n && !(v[i] == ')' && par == 0)) {
                    if (v[i] == '(') par++;
                    else if (v[i] == ')') par--;
                    i++;
                }
                fb_e = i;
            }
            if (i < n && v[i] == ')') i++;       /* consume ')' */
            const char *rv; int rl;
            if (depth < 16 && var_lookup(v + ns, ne - ns, &rv, &rl)) {
                o += var_subst(rv, rl, out + o, cap - o, depth + 1);
            } else if (fb_s >= 0 && depth < 16) {
                o += var_subst(v + fb_s, fb_e - fb_s, out + o, cap - o, depth + 1);
            }
            continue;
        }
        out[o++] = v[i++];
    }
    out[o] = 0;
    return o;
}

static int val_has_var(const char *v, int n) {
    for (int i = 0; i + 4 <= n; i++)
        if (v[i] == 'v' && v[i+1] == 'a' && v[i+2] == 'r' && v[i+3] == '(')
            return 1;
    return 0;
}

/* Apply one declaration to a computed style. Pass 0 applies only
 * font-size (so em elsewhere resolves against the final font); pass 1
 * applies everything else. */
static void st_apply(struct cstyle *st, const struct cstyle *pst,
                     const struct cdecl *d, int pass) {
    if (d->prop == CP_VAR) return;        /* collected separately */
    const char *v = &E->csspool[d->voff];
    int n = d->vlen;
    char vbuf[1024];
    if (val_has_var(v, n)) {              /* resolve var() before parsing */
        n = var_subst(v, n, vbuf, sizeof(vbuf), 0);
        v = vbuf;
    }
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
        /* @font-face: first listed family that names a registered web
         * font wins; else inherit/none. */
        st->webface = -1;
        {
            int p2 = 0;
            struct vtok ft;
            while (vtok_next(v, n, &p2, &ft)) {
                const char *fs = ft.s; int fl2 = ft.len;
                while (fl2 > 0 && (*fs == '"' || *fs == '\'' || *fs == ' ')) { fs++; fl2--; }
                while (fl2 > 0 && (fs[fl2-1] == '"' || fs[fl2-1] == '\'' ||
                                   fs[fl2-1] == ' ' || fs[fl2-1] == ',')) fl2--;
                int face = webfont_face_for(fs, fl2);
                if (face >= 0) { st->webface = (int8_t)face; break; }
            }
        }
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
            if (k == LK_AUTO) { st->width = -1; st->fl &= (uint8_t)~SF_WPCT; st->woff = 0; }
            else if (k == LK_PX && px >= 0) {
                st->width = px; st->fl &= (uint8_t)~SF_WPCT; st->woff = 0;
            } else if (k == LK_PCT && px > 0) {
                st->width = px > 100 ? 100 : px;
                st->fl |= SF_WPCT;
                st->woff = (int16_t)g_calc_off_px;   /* calc(% - Npx) */
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
            else if (tok_is(&t, "flex")) st->disp = D_FLEX;
            else if (tok_is(&t, "grid") || tok_is(&t, "inline-grid"))
                st->disp = D_GRID;
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
    case CP_FLEXDIR:
        if (vtok_next(v, n, &pos, &t))
            st->fdir = (t.len >= 3 && lc(t.s[0]) == 'c') ? 1 : 0;
        break;
    case CP_FLEXWRAP:
        if (vtok_next(v, n, &pos, &t))
            st->fwrap = tok_is(&t, "nowrap") ? 0 : 1;
        break;
    case CP_FLEXFLOW:
        while (vtok_next(v, n, &pos, &t)) {
            if (lc(t.s[0]) == 'c' && t.len >= 3) st->fdir = 1;
            else if (tok_is(&t, "row")) st->fdir = 0;
            else if (tok_is(&t, "wrap") || tok_is(&t, "wrap-reverse"))
                st->fwrap = 1;
            else if (tok_is(&t, "nowrap")) st->fwrap = 0;
        }
        break;
    case CP_JUSTC:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "flex-end") || tok_is(&t, "end") ||
                tok_is(&t, "right")) st->fjust = 1;
            else if (tok_is(&t, "center")) st->fjust = 2;
            else if (tok_is(&t, "space-between") ||
                     tok_is(&t, "space-around") ||
                     tok_is(&t, "space-evenly")) st->fjust = 3;
            else st->fjust = 0;
        }
        break;
    case CP_ALITEMS:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "center")) st->falign = 2;
            else if (tok_is(&t, "flex-end") || tok_is(&t, "end"))
                st->falign = 3;
            else if (tok_is(&t, "flex-start") || tok_is(&t, "start") ||
                     tok_is(&t, "baseline")) st->falign = 1;
            else st->falign = 0;          /* stretch */
        }
        break;
    case CP_FGROW:
        if (vtok_next(v, n, &pos, &t) && t.s[0] >= '0' && t.s[0] <= '9')
            st->fgrow = (uint8_t)(t.s[0] - '0');
        break;
    case CP_FSHRINK:
        if (vtok_next(v, n, &pos, &t) && t.s[0] >= '0' && t.s[0] <= '9')
            st->fshrink = (uint8_t)(t.s[0] - '0');
        break;
    case CP_FBASIS:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            if (tok_is(&t, "auto") || tok_is(&t, "content"))
                st->fbasis = -1;
            else if (css_len_tok(t.s, t.len, st->px, &px) == LK_PX && px >= 0)
                st->fbasis = px > 4000 ? 4000 : px;
        }
        break;
    case CP_FLEX: {
        /* shorthand: none | auto | <grow> [<shrink>] [<basis>] */
        int nnum = 0;
        while (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "none")) {
                st->fgrow = 0; st->fshrink = 0; st->fbasis = -1;
            } else if (tok_is(&t, "auto")) {
                st->fgrow = 1; st->fshrink = 1; st->fbasis = -1;
            } else if (t.s[0] >= '0' && t.s[0] <= '9') {
                /* bare digit(s): grow then shrink; "0%"/"10px" = basis */
                int is_len = 0;
                for (int q = 1; q < t.len; q++)
                    if (!(t.s[q] >= '0' && t.s[q] <= '9') && t.s[q] != '.')
                        { is_len = 1; break; }
                int px;
                if (is_len) {
                    if (css_len_tok(t.s, t.len, st->px, &px) == LK_PX && px >= 0)
                        st->fbasis = px > 4000 ? 4000 : px;
                    else st->fbasis = 0;  /* 0% and friends */
                } else if (nnum == 0) {
                    st->fgrow = (uint8_t)(t.s[0] - '0');
                    st->fshrink = 1;
                    st->fbasis = 0;       /* flex:N => basis 0 */
                    nnum++;
                } else {
                    st->fshrink = (uint8_t)(t.s[0] - '0');
                    nnum++;
                }
            }
        }
        break;
    }
    case CP_GAP:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            if (css_len_tok(t.s, t.len, st->px, &px) == LK_PX && px >= 0)
                st->gap = (int16_t)(px > 500 ? 500 : px);
        }
        break;
    case CP_GRID_TC:
        if (pass == 1) {                     /* raw text -> stable pool */
            int off = gridpool_put(v, n);
            if (off >= 0) { st->gtc_off = off; st->gtc_len = (int16_t)n; }
        }
        break;
    case CP_GRID_TR:
        if (pass == 1) {
            int off = gridpool_put(v, n);
            if (off >= 0) { st->gtr_off = off; st->gtr_len = (int16_t)n; }
        }
        break;
    case CP_GRID_COL: case CP_GRID_ROW: {
        /* v1: only the span count. "span N", or "A / B" -> B-A. */
        int span = 1;
        int a = -1, b = -1, saw_span = 0;
        while (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "span")) { saw_span = 1; continue; }
            if (t.len == 1 && t.s[0] == '/') continue;
            if (t.s[0] >= '0' && t.s[0] <= '9') {
                int num = 0;
                for (int q = 0; q < t.len && t.s[q] >= '0' && t.s[q] <= '9'; q++)
                    num = num * 10 + (t.s[q] - '0');
                if (saw_span) { span = num; break; }
                if (a < 0) a = num; else b = num;
            }
        }
        if (!saw_span && a >= 0 && b >= 0 && b > a) span = b - a;
        if (span < 1) span = 1;
        if (span > 32) span = 32;
        if (d->prop == CP_GRID_COL) st->gcol_span = (uint8_t)span;
        else st->grow_span = (uint8_t)span;
        break;
    }
    case CP_OVERFLOW: case CP_OVERFLOWX: case CP_OVERFLOWY:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "hidden") || tok_is(&t, "scroll") ||
                tok_is(&t, "auto") || tok_is(&t, "clip"))
                st->ovf = 1;
            else st->ovf = 0;              /* visible */
        }
        break;
    case CP_TRANSFORM: {
        /* v1: translate()/translateX()/translateY(); px or % of own size.
         * scale/rotate/matrix are ignored (no wrong result, just no-op). */
        st->has_tf = 0; st->txpx = 0; st->typx = 0; st->txpct = 0; st->typct = 0;
        int fo = str_contains(v, n, "translate", 9);
        if (fo >= 0) {
            int isx = (fo + 9 < n && lc(v[fo + 9]) == 'x');
            int isy = (fo + 9 < n && lc(v[fo + 9]) == 'y');
            int p2 = fo + 9;
            while (p2 < n && v[p2] != '(') p2++;
            if (p2 < n) p2++;
            int e2 = p2;
            while (e2 < n && v[e2] != ')') e2++;
            /* parse up to two comma-separated components */
            int comp = 0;
            int cs = p2;
            for (int q = p2; q <= e2 && comp < 2; q++) {
                if (q == e2 || v[q] == ',') {
                    int cl = q - cs;
                    while (cl > 0 && is_whitespace(v[cs])) { cs++; cl--; }
                    int ce = cs + cl;
                    while (ce > cs && is_whitespace(v[ce - 1])) ce--;
                    cl = ce - cs;
                    if (cl > 0) {
                        int pct = (v[cs + cl - 1] == '%');
                        int px = 0;
                        if (pct) {
                            int num = 0, neg = 0, k = cs;
                            if (v[k] == '-') { neg = 1; k++; }
                            for (; k < cs + cl - 1 && v[k] >= '0' && v[k] <= '9'; k++)
                                num = num * 10 + (v[k] - '0');
                            px = neg ? -num : num;
                        } else {
                            css_len_tok(v + cs, cl, st->px, &px);
                        }
                        int axis = isy ? 1 : (isx ? 0 : comp);
                        if (axis == 0) {
                            if (pct) st->txpct = (int8_t)px; else st->txpx = (int16_t)px;
                        } else {
                            if (pct) st->typct = (int8_t)px; else st->typx = (int16_t)px;
                        }
                        st->has_tf = 1;
                    }
                    cs = q + 1;
                    comp++;
                }
            }
        }
        break;
    }
    case CP_POSITION:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "relative") || tok_is(&t, "sticky"))
                st->pos = 1;                  /* sticky degrades */
            else if (tok_is(&t, "absolute")) st->pos = 2;
            else if (tok_is(&t, "fixed")) st->pos = 3;
            else st->pos = 0;                 /* static */
        }
        break;
    case CP_TOP: case CP_RIGHT: case CP_BOTTOM: case CP_LEFT: {
        int side = d->prop - CP_TOP;          /* T R B L, contiguous enum */
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            int k = css_len_tok(t.s, t.len, st->px, &px);
            if (tok_is(&t, "auto")) { st->po[side] = M_AUTO; st->po_pct &= (uint8_t)~(1 << side); }
            else if (k == LK_PX) { st->po[side] = clamp_m(px); st->po_pct &= (uint8_t)~(1 << side); }
            else if (k == LK_PCT) { st->po[side] = (int16_t)px; st->po_pct |= (uint8_t)(1 << side); }
        }
        break;
    }
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
    /* Custom properties first, in cascade order (lowest priority pushed
     * first so the last-pushed wins on lookup): matched non-important,
     * inline non-important, matched important, inline important. Copied
     * into the stable var pool so they outlive the csspool rollback and
     * this node's whole subtree can resolve them. */
    int var_mark = g_nvarscope, varpool_mark = g_varpool_len;
    for (int m = 0; m < nm; m++)
        for (int d2 = 0; d2 < M[m].ndecl; d2++) {
            struct cdecl *dv = &E->decls[M[m].decl0 + d2];
            if (dv->prop == CP_VAR && !dv->imp)
                var_push(&E->csspool[dv->noff], dv->nlen,
                         &E->csspool[dv->voff], dv->vlen);
        }
    for (int d2 = 0; d2 < inlN; d2++) {
        struct cdecl *dv = &E->decls[inl0 + d2];
        if (dv->prop == CP_VAR && !dv->imp)
            var_push(&E->csspool[dv->noff], dv->nlen,
                     &E->csspool[dv->voff], dv->vlen);
    }
    for (int m = 0; m < nm; m++)
        for (int d2 = 0; d2 < M[m].ndecl; d2++) {
            struct cdecl *dv = &E->decls[M[m].decl0 + d2];
            if (dv->prop == CP_VAR && dv->imp)
                var_push(&E->csspool[dv->noff], dv->nlen,
                         &E->csspool[dv->voff], dv->vlen);
        }
    for (int d2 = 0; d2 < inlN; d2++) {
        struct cdecl *dv = &E->decls[inl0 + d2];
        if (dv->prop == CP_VAR && dv->imp)
            var_push(&E->csspool[dv->noff], dv->nlen,
                     &E->csspool[dv->voff], dv->vlen);
    }
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
    /* pop this node's custom properties as we leave its subtree */
    g_nvarscope = var_mark;
    g_varpool_len = varpool_mark;
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
"video,audio,svg,map,picture{display:none}\n"
"canvas{display:inline-block}\n"      /* stage 13I: replaced element */
"video{display:inline-block}\n"       /* stage 13: media elements */
"audio{display:none}\n"
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

/* overflow clip rects (stage 13D). Each display item carries a clip
 * index; the painter intersects the item with g_clips[idx] (doc coords,
 * already the intersection with any ancestor clip). Index 0 = no clip.
 * g_cur_clip is the clip in force while laying the current subtree. */
#define CLIP_MAX 128
struct cliprect { int32_t x, y, w, h; };
static struct cliprect g_clips[CLIP_MAX];
static int g_nclips;       /* index 0 reserved as "no clip" */
static int g_cur_clip;

static struct ditem *item_new(int kind) {
    if (E->nitems >= ITEM_MAX) return NULL;
    struct ditem *it = &E->items[E->nitems++];
    mem_zero(it, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->link = it->field = it->img = -1;
    it->node = -1;
    it->clip = (uint8_t)g_cur_clip;        /* overflow clip in force */
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
    int face;                    /* resolved kernel face (web font / bold) */
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
    int face = is->face;
    int ww = text_px_w(w, wl, is->px, face, mono);
    int sw = ic->pend_sp ? text_px_w(" ", 1, is->px, face, mono) : 0;
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
    sw = sp ? text_px_w(" ", 1, is->px, face, mono) : 0;
    struct ditem *cu = (ic->citem >= 0) ? &E->items[ic->citem] : NULL;
    int same = cu && cu->kind == DI_TEXT && cu->px == is->px &&
               cu->fl == (uint8_t)is->fl && cu->fg == is->fg &&
               cu->bg == is->bg && cu->link == is->link &&
               cu->face == (uint8_t)is->face &&
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
        it->face = (uint8_t)is->face;
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
            int cw2 = text_px_w(&t[j], 1, is->px, is->face, is->fl & IF_MONO);
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

static void absq_add(int ni, int sx, int sy);

static void inl_walk(struct ictx *ic, int ni, const struct istyle *is) {
    struct dnode *nd = &E->nodes[ni];
    if (nd->tag == T_TEXT) {
        if (is->pre) inl_text_pre(ic, &E->tpool[nd->toff], nd->tlen, is);
        else inl_text_norm(ic, &E->tpool[nd->toff], nd->tlen, is);
        return;
    }
    const struct cstyle *st = &nd->st;
    if (st->disp == D_NONE) return;
    if (st->pos >= 2) {                     /* absolute/fixed leaves flow */
        absq_add(ni, ic->x, ic->y);
        return;
    }
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
    cs.face = run_face(st->webface, st->fl & SF_BOLD);

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
    is.face = run_face(bst->webface, bst->fl & SF_BOLD);
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

/* ---- Laid-rect stamp journal (stage 13F) ---------------------------- *
 * getComputedStyle / getBoundingClientRect / Intersection- and Resize-
 * Observer need each element's final laid box. Blocks stamp their
 * border box into this journal as they lay; every post-lay item shift
 * (floats, valign, flex align, grid rows, relative, transform,
 * absolute placement) mirrors onto the journal range it shifts. At
 * the end of layout() the journal commits into the nodes (last write
 * wins, so provisional passes are harmless), then inline elements
 * pick up a union of their painted items. */
#define NST_MAX 16384
struct nstamp { int32_t node, x, y, w, h; uint8_t fixed; };
static struct nstamp g_nst[NST_MAX];
static int g_nnst;

static void nstamp_add(int ni, int x, int y, int w, int h) {
    if (g_flt_freeze || g_nnst >= NST_MAX) return;   /* measure: ignore */
    struct nstamp *s = &g_nst[g_nnst++];
    s->node = ni;
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->fixed = 0;
}

static void nstamp_shift(int s0, int s1, int dx, int dy) {
    for (int k = s0; k < s1; k++) {
        g_nst[k].x += dx;
        g_nst[k].y += dy;
    }
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
    int s0 = g_nnst;
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
            if (st->fl & SF_WPCT) cwid += st->woff;   /* calc(% +/- Npx) */
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
    nstamp_shift(s0, g_nnst, dx, dy);
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

/* ---- position: absolute/fixed (stage 12E) --------------------------- *
 * Out-of-flow boxes are QUEUED during normal flow (with a snapshot of
 * their containing block -- the nearest positioned ancestor's border
 * box, or the viewport for fixed) and laid AFTER the whole in-flow
 * pass, so they paint on top and never disturb flow. Fixed items get
 * IF_FIXED: the painter and hit-test use viewport coords for them.
 * v1 limits: `bottom` anchoring only for fixed (viewport height is
 * known; a positioned ancestor's height is not), static-position
 * fallback is the flow cursor, z-index ignored. */

#define ABS_MAX 64
struct absq {
    int     node;
    int     cb_x, cb_y, cb_w, cb_h;  /* containing block (border box) */
    int     sx, sy;              /* static-position fallback */
    uint8_t fixed;
};
static struct absq g_absq[ABS_MAX];
static int g_nabsq;

/* Current positioned containing block (set by positioned ancestors in
 * lay_block; initialized to the document by layout()). cb_h is -1 when
 * the positioned ancestor's height is auto (unknown at queue time). */
static int g_cb_x, g_cb_y, g_cb_w, g_cb_h;
static int g_vp_w, g_vp_h;       /* viewport content size at layout() */

static void absq_add(int ni, int sx, int sy) {
    if (g_flt_freeze || g_nabsq >= ABS_MAX) return;   /* measure: ignore */
    struct absq *q = &g_absq[g_nabsq++];
    q->node = ni;
    q->fixed = (E->nodes[ni].st.pos == 3);
    if (q->fixed) {
        q->cb_x = 0; q->cb_y = 0; q->cb_w = g_vp_w; q->cb_h = g_vp_h;
        q->sx = sx; q->sy = 0;
    } else {
        q->cb_x = g_cb_x; q->cb_y = g_cb_y; q->cb_w = g_cb_w; q->cb_h = g_cb_h;
        q->sx = sx; q->sy = sy;
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
        if (cn->tag != T_TEXT && cn->st.disp != D_NONE &&
            cn->st.pos >= 2) {               /* absolute/fixed: out of flow */
            absq_add(c, cx, cy);
            c = cn->next;
            continue;
        }
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

struct tcell_info { int i0, i1, s0, s1, bg_i, x, w, h, node; };

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
        tc->s0 = g_nnst;
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
        tc->s1 = g_nnst;
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
            nstamp_shift(tc->s0, tc->s1, 0, off);
        }
        nstamp_add(tc->node, tc->x, y, tc->w, row_h);
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

/* ---- Flexbox (v1) ---------------------------------------------------- *
 * display:flex row/column, flex-direction, justify-content (start/end/
 * center/space-between), align-items (start/center/end; stretch lays
 * top-aligned), flex-grow/shrink/basis, flex-wrap, gap. Items are
 * block containers: hypothetical main sizes come from basis/width/the
 * frozen measure pass, free space distributes by grow (or shrinks
 * proportionally, floored at min-content), then each item is laid by
 * lay_block at its resolved width (width imposed via a save/restore of
 * the computed style). Grow and justify are suppressed in measure
 * passes so a flex container's preferred width is the sum of its
 * items. Reverse directions, order:, align-self, auto-margin space
 * absorption and %-basis are out of scope for v1. */

#define FLEX_MAX 48

static int lay_flex(int ni, int cx, int content_w, int y, int link,
                    uint32_t inbg) {
    struct dnode *nd = &E->nodes[ni];
    struct cstyle *st = &nd->st;
    int gap = st->gap > 0 ? st->gap : 0;

    int items[FLEX_MAX];
    int n = 0;
    for (int c = nd->first; c >= 0 && n < FLEX_MAX; c = E->nodes[c].next) {
        struct dnode *cn = &E->nodes[c];
        if (cn->tag == T_TEXT) continue;      /* bare text skipped (v1) */
        if (cn->st.disp == D_NONE) continue;
        items[n++] = c;
    }
    if (n == 0) return y;

    if (st->fdir == 1) {
        /* column: a block stack with gap; align-items shifts narrower
         * items horizontally (stretch/start = full-width/left) */
        int cy = y;
        for (int k = 0; k < n; k++) {
            struct dnode *cn = &E->nodes[items[k]];
            if (k) cy += gap;
            int mt = cn->st.m[0] == M_AUTO ? 0 : cn->st.m[0];
            int mb = cn->st.m[2] == M_AUTO ? 0 : cn->st.m[2];
            cy += mt;
            int i0 = E->nitems;
            int ny = lay_block(items[k], cx, content_w, cy, link, inbg);
            if (st->falign >= 2 && !g_flt_freeze) {
                int ext = cx;
                for (int q = i0; q < E->nitems; q++) {
                    int e = E->items[q].x + E->items[q].w;
                    if (e > ext) ext = e;
                }
                int iw = ext - cx;
                if (iw > 0 && iw < content_w) {
                    int off = (st->falign == 2) ? (content_w - iw) / 2
                                                : content_w - iw;
                    for (int q = i0; q < E->nitems; q++)
                        E->items[q].x += off;
                }
            }
            cy = ny + mb;
        }
        return cy;
    }

    /* row: hypothetical outer (margin-box) main sizes + min floors */
    int hyp[FLEX_MAX], mnw[FLEX_MAX];
    for (int k = 0; k < n; k++) {
        struct cstyle *cs = &E->nodes[items[k]].st;
        int ml = cs->m[3] == M_AUTO ? 0 : cs->m[3];
        int mr = cs->m[1] == M_AUTO ? 0 : cs->m[1];
        int cmin, cpref;
        tbl_measure_cell(items[k], link, inbg, &cmin, &cpref);
        int basis;
        if (cs->fbasis >= 0)
            basis = cs->fbasis + cs->bw[3] + cs->bw[1] + cs->p[3] + cs->p[1];
        else if (cs->width >= 0 && (cs->fl & SF_WPCT))
            basis = (int)((long)content_w * cs->width / 100);
        else
            basis = cpref;    /* explicit px width already folded in */
        hyp[k] = basis + ml + mr;
        mnw[k] = cmin + ml + mr;
        if (hyp[k] < 8) hyp[k] = 8;
    }

    int cy = y;
    int k0 = 0;
    while (k0 < n) {
        int k1 = k0;
        long used = 0;
        while (k1 < n) {
            long add = hyp[k1] + (k1 > k0 ? gap : 0);
            if (st->fwrap && k1 > k0 && used + add > content_w) break;
            used += add;
            k1++;
        }
        int cnt = k1 - k0;
        long freew = (long)content_w - used;
        int w[FLEX_MAX];
        long tg = 0;
        for (int k = k0; k < k1; k++) tg += E->nodes[items[k]].st.fgrow;
        if (freew > 0 && tg > 0 && !g_flt_freeze) {
            for (int k = k0; k < k1; k++)
                w[k - k0] = hyp[k] +
                    (int)(freew * E->nodes[items[k]].st.fgrow / tg);
        } else if (freew < 0) {
            long tsw = 0;
            for (int k = k0; k < k1; k++)
                tsw += (long)E->nodes[items[k]].st.fshrink * hyp[k];
            for (int k = k0; k < k1; k++) {
                long red = tsw > 0
                    ? (-freew) * E->nodes[items[k]].st.fshrink * hyp[k] / tsw
                    : 0;
                int ww = hyp[k] - (int)red;
                if (ww < mnw[k]) ww = mnw[k];
                if (ww < 8) ww = 8;
                w[k - k0] = ww;
            }
        } else {
            for (int k = k0; k < k1; k++) w[k - k0] = hyp[k];
        }

        /* justify-content: shift the whole line / spread the gaps */
        long used2 = (long)gap * (cnt - 1);
        for (int k = 0; k < cnt; k++) used2 += w[k];
        long slack = content_w - used2;
        if (slack < 0) slack = 0;
        int x = cx, extra_gap = 0;
        if (!g_flt_freeze) {
            if (st->fjust == 1) x += (int)slack;
            else if (st->fjust == 2) x += (int)(slack / 2);
            else if (st->fjust == 3 && cnt > 1)
                extra_gap = (int)(slack / (cnt - 1));
        }

        struct { int i0, i1, s0, s1, h; } L[FLEX_MAX];
        int line_h = 0;
        for (int k = k0; k < k1; k++) {
            struct dnode *cn = &E->nodes[items[k]];
            struct cstyle *cs = &cn->st;
            int li = k - k0;
            int ml = cs->m[3] == M_AUTO ? 0 : cs->m[3];
            int mr = cs->m[1] == M_AUTO ? 0 : cs->m[1];
            int mt = cs->m[0] == M_AUTO ? 0 : cs->m[0];
            int mb = cs->m[2] == M_AUTO ? 0 : cs->m[2];
            int32_t save_w = cs->width;
            uint8_t save_fl = cs->fl;
            cs->width = w[li] - ml - mr - cs->bw[3] - cs->bw[1] -
                        cs->p[3] - cs->p[1];
            if (cs->width < 8) cs->width = 8;
            cs->fl &= (uint8_t)~SF_WPCT;
            L[li].i0 = E->nitems;
            L[li].s0 = g_nnst;
            int ny = lay_block(items[k], x, w[li], cy + mt, link, inbg);
            L[li].i1 = E->nitems;
            L[li].s1 = g_nnst;
            L[li].h = (ny - cy) + mb;
            cs->width = save_w;
            cs->fl = save_fl;
            if (L[li].h > line_h) line_h = L[li].h;
            x += w[li] + gap + extra_gap;
        }
        /* align-items center/end: shift the shorter items down */
        if (st->falign >= 2 && !g_flt_freeze) {
            for (int li = 0; li < cnt; li++) {
                int off = line_h - L[li].h;
                if (off <= 0) continue;
                if (st->falign == 2) off /= 2;
                for (int q = L[li].i0; q < L[li].i1; q++)
                    E->items[q].y += off;
                nstamp_shift(L[li].s0, L[li].s1, 0, off);
            }
        }
        cy += line_h;
        k0 = k1;
        if (k0 < n) cy += gap;
    }
    return cy;
}

/* ---- CSS Grid (v1) -------------------------------------------------- *
 * display:grid with grid-template-columns (px / % / fr / auto /
 * repeat(N,...) / repeat(auto-fill|auto-fit, minmax(min,max))), gap,
 * auto-placement (row-major), and grid-column/grid-row span. Rows are
 * content-sized (auto), each row as tall as its tallest cell. Items are
 * block containers laid at their cell with the spanned width imposed.
 * v1 skips: named lines/areas, grid-template-areas, explicit line
 * placement start numbers, dense packing, per-axis alignment beyond
 * stretch, and fr in rows (rows are always content-sized). */

#define GRID_MAX_TRACKS 32
#define GRID_MAX_ITEMS  256

/* One simple track token (no repeat) -> type (0 px,1 pct,2 fr,3 auto)
 * + value. minmax(a,b) collapses to its max track. Returns 1 on ok. */
static int grid_one_track(const char *tok, int tl, int own_px,
                          int *type, int *val) {
    while (tl > 0 && is_whitespace(*tok)) { tok++; tl--; }
    while (tl > 0 && is_whitespace(tok[tl - 1])) tl--;
    if (tl <= 0) return 0;
    if (tl > 7 && str_ncasecmp(tok, "minmax(", 7) == 0) {
        int par = 0, comma = -1;
        for (int k = 7; k < tl; k++) {
            if (tok[k] == '(') par++;
            else if (tok[k] == ')') { if (par) par--; }
            else if (tok[k] == ',' && par == 0) { comma = k; break; }
        }
        int m0 = (comma >= 0) ? comma + 1 : 7;
        int m1 = tl - (tok[tl - 1] == ')' ? 1 : 0);
        return grid_one_track(tok + m0, m1 - m0, own_px, type, val);
    }
    if (tl == 4 && str_ncasecmp(tok, "auto", 4) == 0) { *type = 3; *val = 0; return 1; }
    if (tl == 7 && str_ncasecmp(tok, "min-con", 7) == 0) { *type = 3; *val = 0; return 1; }
    /* number + unit */
    int i = 0;
    double num = 0;
    int digits = 0;
    while (i < tl && tok[i] >= '0' && tok[i] <= '9') { num = num * 10 + (tok[i] - '0'); i++; digits++; }
    if (i < tl && tok[i] == '.') {
        i++;
        double f = 0.1;
        while (i < tl && tok[i] >= '0' && tok[i] <= '9') { num += (tok[i] - '0') * f; f *= 0.1; i++; digits++; }
    }
    if (!digits) return 0;
    if (tl - i == 2 && lc(tok[i]) == 'f' && lc(tok[i + 1]) == 'r') {
        *type = 2; *val = (int)(num + 0.5); if (*val < 1) *val = 1; return 1;
    }
    if (tl - i == 1 && tok[i] == '%') { *type = 1; *val = (int)(num + 0.5); return 1; }
    int px;
    if (css_len_tok(tok, tl, own_px, &px) == LK_PX) { *type = 0; *val = px; return 1; }
    return 0;
}

/* Parse grid-template-columns into pixel widths. Returns ncols. */
static int grid_resolve_cols(const char *s, int n, int W, int gap, int own_px,
                             int colw[], int maxcols) {
    int ty[GRID_MAX_TRACKS], vl[GRID_MAX_TRACKS], nt = 0;
    int i = 0;
    while (i < n && nt < maxcols) {
        while (i < n && (is_whitespace(s[i]) || s[i] == ',')) i++;
        if (i >= n) break;
        int t0 = i, par = 0;
        while (i < n) {
            char c = s[i];
            if (c == '(') par++;
            else if (c == ')') { if (par) par--; }
            else if (is_whitespace(c) && par == 0) break;
            i++;
        }
        const char *tok = s + t0;
        int tl = i - t0;
        if (tl > 7 && str_ncasecmp(tok, "repeat(", 7) == 0) {
            int in1 = tl - (tok[tl - 1] == ')' ? 1 : 0);
            int j = 7;
            while (j < in1 && is_whitespace(tok[j])) j++;
            int is_autofill = (str_ncasecmp(tok + j, "auto-fill", 9) == 0 ||
                               str_ncasecmp(tok + j, "auto-fit", 8) == 0);
            int cnt = 0;
            while (j < in1 && tok[j] >= '0' && tok[j] <= '9') { cnt = cnt * 10 + (tok[j] - '0'); j++; }
            int par2 = 0;
            while (j < in1 && !(tok[j] == ',' && par2 == 0)) {
                if (tok[j] == '(') par2++; else if (tok[j] == ')') { if (par2) par2--; }
                j++;
            }
            if (j < in1) j++;                    /* past comma */
            int subt, subv;
            /* the subtrack (single pattern in v1); for auto-fill get its
             * min from the minmax to compute how many columns fit */
            int sub_ok = grid_one_track(tok + j, in1 - j, own_px, &subt, &subv);
            if (!sub_ok) continue;
            if (is_autofill) {
                int minpx = 0;                   /* min track width for the fit */
                /* re-read the minmax min (first arg) */
                const char *st2 = tok + j;
                int sl = in1 - j;
                if (sl > 7 && str_ncasecmp(st2, "minmax(", 7) == 0) {
                    int mt, mv;
                    int k2 = 7;
                    while (k2 < sl && is_whitespace(st2[k2])) k2++;
                    int me = k2;
                    while (me < sl && st2[me] != ',' && st2[me] != ')') me++;
                    if (grid_one_track(st2 + k2, me - k2, own_px, &mt, &mv) && mt == 0)
                        minpx = mv;
                }
                if (minpx <= 0) minpx = (subt == 0 ? subv : 100);
                if (minpx < 1) minpx = 1;
                int fit = (W + gap) / (minpx + gap);
                if (fit < 1) fit = 1;
                for (int k = 0; k < fit && nt < maxcols; k++) {
                    ty[nt] = 2; vl[nt] = 1; nt++;   /* each fills as 1fr */
                }
            } else {
                if (cnt < 1) cnt = 1;
                for (int k = 0; k < cnt && nt < maxcols; k++) {
                    ty[nt] = subt; vl[nt] = subv; nt++;
                }
            }
            continue;
        }
        int tt, tv;
        if (grid_one_track(tok, tl, own_px, &tt, &tv)) {
            ty[nt] = tt; vl[nt] = tv; nt++;
        }
    }
    if (nt == 0) return 0;

    /* resolve: fixed px/pct claim first, fr split the remainder,
     * auto ~= 1fr (v1) */
    long fixed = 0, frsum = 0;
    for (int k = 0; k < nt; k++) {
        if (ty[k] == 0) fixed += vl[k];
        else if (ty[k] == 1) fixed += (long)W * vl[k] / 100;
        else if (ty[k] == 2) frsum += vl[k];
        else { ty[k] = 2; vl[k] = 1; frsum += 1; }   /* auto -> 1fr */
    }
    long avail = W - (long)gap * (nt - 1) - fixed;
    if (avail < 0) avail = 0;
    for (int k = 0; k < nt; k++) {
        if (ty[k] == 0) colw[k] = vl[k];
        else if (ty[k] == 1) colw[k] = (int)((long)W * vl[k] / 100);
        else colw[k] = frsum > 0 ? (int)(avail * vl[k] / frsum) : 0;
        if (colw[k] < 1) colw[k] = 1;
    }
    return nt;
}

static int lay_grid(int ni, int cx, int content_w, int y, int link,
                    uint32_t inbg) {
    struct dnode *nd = &E->nodes[ni];
    struct cstyle *st = &nd->st;
    int gap = st->gap > 0 ? st->gap : 0;

    int colw[GRID_MAX_TRACKS];
    int ncols = 0;
    if (st->gtc_len > 0)
        ncols = grid_resolve_cols(&g_gridpool[st->gtc_off], st->gtc_len,
                                  content_w, gap, st->px, colw, GRID_MAX_TRACKS);
    if (ncols <= 0) {                        /* no template -> single column */
        ncols = 1; colw[0] = content_w;
    }
    /* column x offsets */
    int colx[GRID_MAX_TRACKS + 1];
    colx[0] = cx;
    for (int k = 0; k < ncols; k++) colx[k + 1] = colx[k] + colw[k] + gap;

    int items[GRID_MAX_ITEMS], nitem = 0;
    for (int c = nd->first; c >= 0 && nitem < GRID_MAX_ITEMS; c = E->nodes[c].next) {
        struct dnode *cn = &E->nodes[c];
        if (cn->tag == T_TEXT || cn->st.disp == D_NONE) continue;
        items[nitem++] = c;
    }
    if (nitem == 0) return y;

    /* auto-placement (row-major), honoring column span; compute each
     * item's (row, col, colspan) and the number of rows */
    int it_row[GRID_MAX_ITEMS], it_col[GRID_MAX_ITEMS], it_span[GRID_MAX_ITEMS];
    int row = 0, col = 0, nrows = 0;
    for (int k = 0; k < nitem; k++) {
        int span = E->nodes[items[k]].st.gcol_span;
        if (span < 1) span = 1;
        if (span > ncols) span = ncols;
        if (col + span > ncols) { row++; col = 0; }
        it_row[k] = row; it_col[k] = col; it_span[k] = span;
        col += span;
        if (col >= ncols) { row++; col = 0; }
        if (it_row[k] + 1 > nrows) nrows = it_row[k] + 1;
    }

    /* lay each item at its cell x with the spanned width imposed; track
     * per-row max height */
    int rowh[GRID_MAX_TRACKS];
    for (int r = 0; r < GRID_MAX_TRACKS && r < nrows; r++) rowh[r] = 0;
    struct { int i0, i1, s0, s1, r, h; } placed[GRID_MAX_ITEMS];
    int rowy[GRID_MAX_TRACKS + 1];

    /* first pass: lay items in provisional y to measure row heights */
    for (int k = 0; k < nitem; k++) {
        int c = items[k];
        struct cstyle *cs = &E->nodes[c].st;
        int cwid = 0;
        for (int s = 0; s < it_span[k]; s++) cwid += colw[it_col[k] + s];
        cwid += gap * (it_span[k] - 1);
        int ix = colx[it_col[k]];
        int32_t save_w = cs->width;
        uint8_t save_fl = cs->fl;
        cs->width = cwid - cs->m[3] - cs->m[1] - cs->bw[3] - cs->bw[1]
                        - cs->p[3] - cs->p[1];
        if (cs->width < 8) cs->width = 8;
        cs->fl &= (uint8_t)~SF_WPCT;
        placed[k].i0 = E->nitems;
        placed[k].s0 = g_nnst;
        int bot = lay_block(c, ix, cwid, 0, link, inbg);   /* prov y=0 */
        placed[k].i1 = E->nitems;
        placed[k].s1 = g_nnst;
        placed[k].r = it_row[k];
        placed[k].h = bot;                   /* height (laid from y=0) */
        cs->width = save_w;
        cs->fl = save_fl;
        if (it_row[k] < GRID_MAX_TRACKS && placed[k].h > rowh[it_row[k]])
            rowh[it_row[k]] = placed[k].h;
    }
    /* row y offsets */
    rowy[0] = y;
    for (int r = 0; r < nrows && r < GRID_MAX_TRACKS; r++)
        rowy[r + 1] = rowy[r] + rowh[r] + gap;

    /* second pass: shift each item's display items down to its row y */
    for (int k = 0; k < nitem; k++) {
        int r = placed[k].r;
        int dy = (r < GRID_MAX_TRACKS) ? rowy[r] : y;
        for (int q = placed[k].i0; q < placed[k].i1; q++)
            E->items[q].y += dy;
        nstamp_shift(placed[k].s0, placed[k].s1, 0, dy);
    }
    int end = (nrows > 0 && nrows <= GRID_MAX_TRACKS) ? rowy[nrows] - gap : y;
    return end;
}

static int lay_block(int ni, int x, int cw, int y, int link, uint32_t inbg) {
    struct dnode *nd = &E->nodes[ni];
    struct cstyle *st = &nd->st;
    if (st->disp == D_NONE) return y;
    int items0 = E->nitems;                /* for the relative shift */
    int nst0 = g_nnst;
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
        if (st->fl & SF_WPCT) content_w += st->woff;   /* calc(% +/- Npx) */
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

    /* A positioned element is the containing block for its absolute
     * descendants (queued with a snapshot of these coords). cb_h is
     * known only when this element has an explicit height. */
    int save_cbx = g_cb_x, save_cby = g_cb_y, save_cbw = g_cb_w, save_cbh = g_cb_h;
    if (st->pos != 0 && !g_flt_freeze) {
        g_cb_x = bx;
        g_cb_y = by;
        g_cb_w = bbw;
        g_cb_h = (st->height >= 0) ? bwt + pt + st->height + pb + bwb : -1;
    }

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

    /* overflow: clip descendants to this element's padding box. The
     * box height is known upfront only when height is explicit (which
     * is exactly when vertical clipping matters -- an auto-height box
     * grows to fit and never clips vertically). Width always clips. */
    int save_clip = g_cur_clip;
    if (st->ovf && !g_flt_freeze && g_nclips < CLIP_MAX) {
        int clx = bx + bwl, cly = by + bwt;
        int clw = pl + content_w + pr;
        int clh = (st->height >= 0) ? pt + st->height + pb : (1 << 24);
        struct cliprect *pc = &g_clips[g_cur_clip];   /* intersect ancestor */
        if (g_cur_clip) {
            if (pc->x > clx) { clw -= pc->x - clx; clx = pc->x; }
            if (clx + clw > pc->x + pc->w) clw = pc->x + pc->w - clx;
            if (pc->y > cly) { clh -= pc->y - cly; cly = pc->y; }
            if (cly + clh > pc->y + pc->h) clh = pc->y + pc->h - cly;
        }
        if (clw < 0) clw = 0;
        if (clh < 0) clh = 0;
        g_clips[g_nclips].x = clx; g_clips[g_nclips].y = cly;
        g_clips[g_nclips].w = clw; g_clips[g_nclips].h = clh;
        g_cur_clip = g_nclips++;
    }

    /* children: table grid, flex line(s), CSS grid, or normal block flow */
    if (st->disp == D_TABLE)
        cy = lay_table_grid(ni, cx, content_w, cy, link, curbg);
    else if (st->disp == D_FLEX)
        cy = lay_flex(ni, cx, content_w, cy, link, curbg);
    else if (st->disp == D_GRID)
        cy = lay_grid(ni, cx, content_w, cy, link, curbg);
    else
        cy = lay_flow(ni, cx, content_w, cy, link, curbg);

    g_cur_clip = save_clip;                /* pop the overflow clip */

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
    /* overflow-clip with an explicit height: the box is EXACTLY that
     * height (content past it is clipped), not grown to fit. */
    if (st->ovf && st->height >= 0) content_h = st->height;
    int bbh = bwt + pt + content_h + pb + bwb;

    if (bg_i >= 0) E->items[bg_i].h = bbh;
    nstamp_add(ni, bx, by, bbw, bbh);
    if (bwt) emit_rect(bx, by, bbw, bwt, st->border_col);
    if (bwb) emit_rect(bx, by + bbh - bwb, bbw, bwb, st->border_col);
    if (bwl) emit_rect(bx, by, bwl, bbh, st->border_col);
    if (bwr) emit_rect(bx + bbw - bwr, by, bwr, bbh, st->border_col);

    g_cb_x = save_cbx; g_cb_y = save_cby; g_cb_w = save_cbw; g_cb_h = save_cbh;

    /* position: relative -- shift the whole element (its flow slot is
     * untouched: the parent keeps advancing as if unshifted). */
    if (st->pos == 1 && !g_flt_freeze) {
        int dx = st->po[3] != M_AUTO ? st->po[3]
               : (st->po[1] != M_AUTO ? -st->po[1] : 0);
        int dyy = st->po[0] != M_AUTO ? st->po[0]
                : (st->po[2] != M_AUTO ? -st->po[2] : 0);
        if (dx || dyy) {
            for (int i2 = items0; i2 < E->nitems; i2++) {
                E->items[i2].x += dx;
                E->items[i2].y += dyy;
            }
            nstamp_shift(nst0, g_nnst, dx, dyy);
        }
    }

    /* transform: translate() -- shift the element's painted items after
     * layout (px, or % of the element's own border-box size). Paint-only,
     * like relative: the flow slot is unchanged. */
    if (st->has_tf && !g_flt_freeze) {
        int dx = st->txpx + (st->txpct ? st->txpct * bbw / 100 : 0);
        int dyy = st->typx + (st->typct ? st->typct * bbh / 100 : 0);
        if (dx || dyy) {
            for (int i2 = items0; i2 < E->nitems; i2++) {
                E->items[i2].x += dx;
                E->items[i2].y += dyy;
            }
            nstamp_shift(nst0, g_nnst, dx, dyy);
        }
    }

    return by + bbh;
}

/* Lay one queued absolute/fixed box: lay it in provisional space
 * (shrink-to-fit when width is auto), resolve the position from the
 * containing-block snapshot + top/right/bottom/left, shift the items
 * into place. Entries queued DURING this lay (nested absolutes) are
 * shifted along and inherit fixed-ness. Returns the box bottom in doc
 * coords (0 for fixed). */
static int lay_abs(int qi) {
    struct absq *q = &g_absq[qi];
    struct dnode *nd = &E->nodes[q->node];
    struct cstyle *st = &nd->st;
    int prov = 3 << 20;                     /* own provisional band */
    int i0 = E->nitems, f0 = g_nflts, q0 = g_nabsq, s0 = g_nnst;

    int lay_cw;
    if (st->width >= 0) {
        lay_cw = q->cb_w;                   /* lay_block resolves width */
    } else {
        int mi = E->nitems, mrr = E->render_len, mf = g_nflts;
        g_flt_freeze++;
        lay_block(q->node, 0, 100000, prov, -1, 0);
        g_flt_freeze--;
        int pref = items_extent(mi) + st->p[1] + st->bw[1];
        E->nitems = mi; E->render_len = mrr; g_nflts = mf;
        lay_cw = pref + 2;
        if (lay_cw > q->cb_w) lay_cw = q->cb_w;
        if (lay_cw < 24) lay_cw = 24;
    }

    int bot = lay_block(q->node, 0, lay_cw, prov, -1, 0);
    int bbh = bot - prov;

    int w_used;
    if (st->width >= 0 && !(st->fl & SF_WPCT))
        w_used = st->width + st->bw[3] + st->bw[1] + st->p[3] + st->p[1];
    else if (st->width >= 0)
        w_used = (int)((long)q->cb_w * st->width / 100);
    else
        w_used = lay_cw;

    /* offsets: % of the containing block (width for L/R, height for
     * T/B); an unknown-height CB (auto) falls back to 0 for % top/bot */
    int cbh = (q->cb_h >= 0) ? q->cb_h : 0;
    int poL = (st->po_pct & (1 << 3)) ? (int)((long)q->cb_w * st->po[3] / 100) : st->po[3];
    int poR = (st->po_pct & (1 << 1)) ? (int)((long)q->cb_w * st->po[1] / 100) : st->po[1];
    int poT = (st->po_pct & (1 << 0)) ? (int)((long)cbh * st->po[0] / 100) : st->po[0];
    int poB = (st->po_pct & (1 << 2)) ? (int)((long)cbh * st->po[2] / 100) : st->po[2];
    int x;
    if (st->po[3] != M_AUTO)      x = q->cb_x + poL;
    else if (st->po[1] != M_AUTO) x = q->cb_x + q->cb_w - poR - w_used;
    else                          x = q->sx;
    int y;
    if (st->po[0] != M_AUTO)      y = q->cb_y + poT;
    else if (st->po[2] != M_AUTO && q->fixed)
                                  y = g_content_h - poB - bbh;
    else if (st->po[2] != M_AUTO && q->cb_h >= 0)
                                  y = q->cb_y + cbh - poB - bbh;
    else                          y = q->sy;

    int dx = x - 0, dyy = y - prov;
    for (int i2 = i0; i2 < E->nitems; i2++) {
        E->items[i2].x += dx;
        E->items[i2].y += dyy;
        if (q->fixed) E->items[i2].fl |= IF_FIXED;
    }
    for (int k = s0; k < g_nnst; k++) {
        g_nst[k].x += dx;
        g_nst[k].y += dyy;
        if (q->fixed) g_nst[k].fixed = 1;
    }
    for (int i2 = f0; i2 < g_nflts; i2++) {
        g_flts[i2].x0 += dx; g_flts[i2].x1 += dx;
        g_flts[i2].y0 += dyy; g_flts[i2].y1 += dyy;
    }
    for (int k = q0; k < g_nabsq; k++) {    /* nested out-of-flow boxes */
        g_absq[k].sx += dx;
        g_absq[k].sy += dyy;
        if (!g_absq[k].fixed) {
            g_absq[k].cb_x += dx;
            g_absq[k].cb_y += dyy;
        }
        if (q->fixed) g_absq[k].fixed = 1;
    }
    return q->fixed ? 0 : y + bbh;
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
    g_nabsq = 0;
    g_nnst = 0;                           /* laid-rect stamp journal */
    g_nclips = 1;                         /* index 0 = no clip */
    g_cur_clip = 0;
    g_vp_w = cw;
    g_vp_h = g_content_h;
    g_cb_x = 0; g_cb_y = 0; g_cb_w = cw;  /* initial containing block */
    g_cb_h = g_content_h;
    struct cstyle *rst = &E->nodes[root].st;
    int y = rst->m[0] == M_AUTO ? 0 : rst->m[0];
    int end = lay_block(root, 0, cw, y, -1, 0);
    g_doc_h = end + (rst->m[2] == M_AUTO ? 0 : rst->m[2]) + 10;
    g_flt_bottom = 0;
    for (int i = 0; i < g_nflts; i++)
        if (g_flts[i].y1 > g_flt_bottom) g_flt_bottom = g_flts[i].y1;
    if (g_flt_bottom + 10 > g_doc_h)
        g_doc_h = g_flt_bottom + 10;      /* floats may outgrow the flow */
    /* out-of-flow boxes: laid after the whole flow so they paint on
     * top; the queue grows as nested absolutes are discovered */
    for (int qi = 0; qi < g_nabsq; qi++) {
        int b = lay_abs(qi);
        if (b + 10 > g_doc_h) g_doc_h = b + 10;
    }
    /* Commit laid rects to the nodes (13F): journal stamps in order
     * (last write wins, so any provisional lay is superseded), then
     * inline elements take the union of their painted items. */
    E->lay_gen++;
    for (int k = 0; k < g_nnst; k++) {
        struct nstamp *s = &g_nst[k];
        struct dnode *n = &E->nodes[s->node];
        n->lx = s->x; n->ly = s->y; n->lw = s->w; n->lh = s->h;
        n->lgen = E->lay_gen;
        n->flags = (int16_t)((n->flags & ~DNF_LAID_FIXED) | DNF_LAID_BLOCK |
                             (s->fixed ? DNF_LAID_FIXED : 0));
    }
    for (int i = 0; i < E->nitems; i++) {
        struct ditem *it = &E->items[i];
        if (it->node < 0 || it->node >= E->nnodes) continue;
        struct dnode *n = &E->nodes[it->node];
        if (n->lgen == E->lay_gen && (n->flags & DNF_LAID_BLOCK)) continue;
        if (n->lgen != E->lay_gen) {
            n->lx = it->x; n->ly = it->y; n->lw = it->w; n->lh = it->h;
            n->lgen = E->lay_gen;
            n->flags = (int16_t)((n->flags & ~(DNF_LAID_BLOCK | DNF_LAID_FIXED))
                                 | ((it->fl & IF_FIXED) ? DNF_LAID_FIXED : 0));
        } else {
            if (it->x < n->lx) { n->lw += n->lx - it->x; n->lx = it->x; }
            if (it->y < n->ly) { n->lh += n->ly - it->y; n->ly = it->y; }
            if (it->x + it->w > n->lx + n->lw) n->lw = it->x + it->w - n->lx;
            if (it->y + it->h > n->ly + n->lh) n->lh = it->y + it->h - n->ly;
        }
    }
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
    E->lay_gen = 0;
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
    cur->nmedia = 0;              /* caller ran media_free() first */
}

static void resolve_relative_url(const char *base, const char *rel, char *out, int out_max);
static void images_free(void);
static void tab_images_free(struct tab *t);
static void media_free(void);
static void tab_media_free(struct tab *t);
static void media_fetch_cancel(void);
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
    case T_CANVAS: {
        /* stage 13I: a <canvas> gets an ARGB backing store reusing the
         * img record (so it lays out + paints as an image); JS draws into
         * im->pixels. The store survives light re-collects (nd->img set)
         * and is freed with the page's images. */
        if (nd->img < 0 && g_nimages < IMG_MAX) {
            char d[12];
            int cw = node_attr_str(nd, "width",  d, sizeof d) ? atoi_simple(d) : 300;
            int ch = node_attr_str(nd, "height", d, sizeof d) ? atoi_simple(d) : 150;
            if (cw < 1)    cw = 300;
            if (ch < 1)    ch = 150;
            if (cw > 1600) cw = 1600;
            if (ch > 1600) ch = 1600;
            uint32_t *px = (uint32_t *)malloc((size_t)cw * (size_t)ch * 4);
            if (px) {
                mem_zero(px, (size_t)cw * (size_t)ch * 4);   /* transparent */
                struct img *im = &g_images[g_nimages];
                mem_zero(im, sizeof(*im));
                im->w = (int16_t)cw;      im->h = (int16_t)ch;
                im->attr_w = (int16_t)cw; im->attr_h = (int16_t)ch;
                im->pixels = px;
                im->state = 1;            /* "loaded" -> paints backing store */
                nd->img = (int16_t)g_nimages++;
            }
        }
        break;
    }
    case T_VIDEO:
    case T_AUDIO: {
        /* stage 13: media elements. Register a media_el keyed by the
         * node (stable across light re-collects); <video> also gets an
         * ARGB backing store (canvas pattern) frames decode into. */
        int found = -1;
        for (int k = 0; k < cur->nmedia; k++)
            if (cur->media[k].used && cur->media[k].node == ni) { found = k; break; }
        if (found >= 0) break;
        char src[512];
        if (!(node_attr_str(nd, "src", src, sizeof(src)) && src[0])) break;
        if (cur->nmedia >= MEDIA_MAX) break;
        struct media_el *m = &cur->media[cur->nmedia];
        mem_zero(m, sizeof(*m));
        m->used = 1;
        m->node = ni;
        m->img = -1;
        m->audio_fd = -1;
        m->is_video = (nd->tag == T_VIDEO);
        resolve_relative_url(g_url, src, m->src, sizeof(m->src));
        char d[12];
        m->autoplay = node_attr_str(nd, "autoplay", d, sizeof d) ? 1 : 0;
        m->loop     = node_attr_str(nd, "loop", d, sizeof d) ? 1 : 0;
        if (m->is_video && nd->img < 0 && g_nimages < IMG_MAX) {
            int cw = node_attr_str(nd, "width",  d, sizeof d) ? atoi_simple(d) : 320;
            int ch = node_attr_str(nd, "height", d, sizeof d) ? atoi_simple(d) : 240;
            if (cw < 16)   cw = 320;
            if (ch < 16)   ch = 240;
            if (cw > 1280) cw = 1280;
            if (ch > 960)  ch = 960;
            uint32_t *px = (uint32_t *)malloc((size_t)cw * (size_t)ch * 4);
            if (px) {
                for (long i = 0; i < (long)cw * ch; i++)
                    px[i] = 0xFF000000u;      /* opaque black until frames */
                struct img *im = &g_images[g_nimages];
                mem_zero(im, sizeof(*im));
                im->w = (int16_t)cw;      im->h = (int16_t)ch;
                im->attr_w = (int16_t)cw; im->attr_h = (int16_t)ch;
                im->pixels = px;
                im->state = 1;
                m->img = g_nimages;
                nd->img = (int16_t)g_nimages++;
            }
        }
        cur->nmedia++;
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
static int  msg_append_int(char *dst, int pos, int max, int v);
static void collect_node(int ni);
static int  js_dispatch_event(int node, const char *type);
static int  js_dispatch_key(int node, const char *type, const char *key);
static void history_push(const char *url);
static long do_navigate(const char *url);
static void history_go(int delta);

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

/* stage 13F: record a DOM mutation for MutationObserver delivery. On
 * overflow the tail is dropped -- the batch is already non-empty, so
 * observers still fire (records are best-effort v1 anyway). */
static void mut_note(int ni, int kind) {
    if (ni < 0 || cur->mut_n >= MUT_MAX) return;
    cur->mut_node[cur->mut_n] = ni;
    cur->mut_kind[cur->mut_n] = (uint8_t)kind;
    cur->mut_n++;
}

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
    if (argc >= 2) {
        int pa = jsi(cx, argv[0]);
        dom_attach(pa, jsi(cx, argv[1]));
        mut_note(pa, 0);
        g_js_dirty = 1;
    }
    return JS_UNDEFINED;
}

static JSValue js_dom_remove(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) {
        int ch = jsi(cx, argv[0]);
        int pa = (ch > 0) ? E->nodes[ch].parent : -1;
        dom_unlink(ch);
        mut_note(pa, 0);
        g_js_dirty = 1;
    }
    return JS_UNDEFINED;
}

static JSValue js_dom_settext(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int ni = jsi(cx, argv[0]);
    const char *s = JS_ToCString(cx, argv[1]);
    if (ni > 0 && s) {
        g_js_dirty = 1;
        mut_note(ni, 2);
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
        mut_note(pa, 0);
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
    mut_note(pa, 0);
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
        mut_note(ni, 1);
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

/* history.back/forward/go: same-document traversal fires popstate
 * synchronously (safe re-entrancy); cross-document refetches via the
 * async nav path, so nothing tears down under the running script. */
static JSValue js_dom_histgo(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int d = (argc >= 1) ? jsi(cx, argv[0]) : 0;
    if (d) history_go(d);
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
        mut_note(ni, 0);
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
    if (ni > 0 && name && val) {
        dom_set_attr(ni, name, val);
        mut_note(ni, 1);
        g_js_dirty = 1;
    }
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

static void js_dump_error(JSContext *cx);

/* Stage 12D: truly async fetch. fetchStart(url, cb) kicks off a
 * kernel-side transfer and returns immediately; the pump calls
 * cb({status, url, body}) -- or cb(null) on failure -- when it lands.
 * Timers keep ticking and the UI stays live during the transfer. */
static JSValue js_dom_fetchstart(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2 || !JS_IsFunction(cx, argv[1])) return JS_UNDEFINED;
    char url[URL_MAX + 1];
    url[0] = 0;
    const char *u = JS_ToCString(cx, argv[0]);
    if (u) {
        resolve_relative_url(g_url, u, url, URL_MAX);
        JS_FreeCString(cx, u);
    }
    long h = -1;
    if (url[0] && has_scheme(url))
        h = sys_http_start(url, JS_FETCH_CAP);
    int slot = -1;
    if (h >= 0)
        for (int k = 0; k < JS_FETCH_MAX; k++)
            if (!cur->js_fetches[k].used) { slot = k; break; }
    if (h < 0 || slot < 0) {               /* fail the promise right away */
        if (h >= 0) sys_http_finish(h);
        JSValue nul = JS_NULL;
        JSValue r = JS_Call(cx, argv[1], JS_UNDEFINED, 1, &nul);
        if (JS_IsException(r)) js_dump_error(cx);
        JS_FreeValue(cx, r);
        return JS_UNDEFINED;
    }
    cur->js_fetches[slot].used = 1;
    cur->js_fetches[slot].handle = (int)h;
    cur->js_fetches[slot].cb = JS_DupValue(cx, argv[1]);
    return JS_UNDEFINED;
}

/* ---- WebSocket bindings (stage 13) ----------------------------------- *
 * wsOpen(url, obj) starts a kernel-side RFC 6455 connection and pins
 * the JS WebSocket object; the pump polls the handle and calls the
 * object's __fire(type, a, b) as events land. wsSend/wsClose forward
 * to the kernel; wsClose also unpins the object. */

static JSValue js_ws_openprim(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2 || !JS_IsObject(argv[1])) return JS_NewInt32(cx, -1);
    long h = -1;
    const char *u = JS_ToCString(cx, argv[0]);
    if (u) {
        h = sys_ws_open(u);
        JS_FreeCString(cx, u);
    }
    int slot = -1;
    if (h >= 0)
        for (int k = 0; k < JS_WS_MAX; k++)
            if (!cur->js_ws[k].used) { slot = k; break; }
    if (h < 0 || slot < 0) {
        if (h >= 0) sys_ws_close(h, 0);
        return JS_NewInt32(cx, -1);
    }
    cur->js_ws[slot].used = 1;
    cur->js_ws[slot].handle = (int)h;
    cur->js_ws[slot].last_state = WS_ST_CONNECTING;
    cur->js_ws[slot].obj = JS_DupValue(cx, argv[1]);
    return JS_NewInt32(cx, (int)h);
}

static JSValue js_ws_sendprim(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int32_t h = -1;
    JS_ToInt32(cx, &h, argv[0]);
    size_t len = 0;
    const char *s = JS_ToCStringLen(cx, &len, argv[1]);
    if (h >= 0 && s)
        sys_ws_send(h, s, (uint32_t)len, 1 /* text */);
    if (s) JS_FreeCString(cx, s);
    return JS_UNDEFINED;
}

static JSValue js_ws_closeprim(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int32_t h = -1, code = 1000;
    JS_ToInt32(cx, &h, argv[0]);
    if (argc >= 2) JS_ToInt32(cx, &code, argv[1]);
    if (h < 0) return JS_UNDEFINED;
    for (int k = 0; k < JS_WS_MAX; k++)
        if (cur->js_ws[k].used && cur->js_ws[k].handle == (int)h) {
            JS_FreeValue(cx, cur->js_ws[k].obj);
            cur->js_ws[k].used = 0;
        }
    sys_ws_close(h, code);
    return JS_UNDEFINED;
}

/* __dom.mediaCtl(node, play): play/pause the node's media element
 * (stage 13). Returns true if the node had one. */
static JSValue js_media_ctl(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_FALSE;
    int ni = jsi(cx, argv[0]);
    int32_t play = 0;
    JS_ToInt32(cx, &play, argv[1]);
    for (int k = 0; k < cur->nmedia; k++) {
        struct media_el *m = &cur->media[k];
        if (!m->used || m->node != ni) continue;
        if (m->state == 1) {
            m->playing = play ? 1 : 0;
            if (m->playing) {
                m->next_ms = js_now_ms();
                if (m->is_video && m->cur_frame >= m->nframes)
                    m->cur_frame = 0;      /* replay from the top */
            }
        } else if (m->state == 0 || m->state == 2) {
            m->autoplay = play ? 1 : 0;    /* start once the fetch lands */
        }
        return JS_TRUE;
    }
    return JS_FALSE;
}

/* Call obj.__fire(type, a, b) from the pump. Consumes a and b. */
static void js_ws_fire(JSContext *cx, JSValue obj, const char *type,
                       JSValue a, JSValue b) {
    JSValue fn = JS_GetPropertyStr(cx, obj, "__fire");
    if (JS_IsFunction(cx, fn)) {
        JSValue args[3] = { JS_NewString(cx, type), a, b };
        JSValue r = JS_Call(cx, fn, obj, 3, args);
        if (JS_IsException(r)) js_dump_error(cx);
        JS_FreeValue(cx, r);
        JS_FreeValue(cx, args[0]);
    }
    JS_FreeValue(cx, fn);
    JS_FreeValue(cx, a);
    JS_FreeValue(cx, b);
}

/* ---- Persistent localStorage (stage 12E) ---------------------------- *
 * One small file per origin host: /data/browser/<host>.ls, written
 * whole on every mutation (storage blobs are tiny) and loaded when a
 * page's JS world spins up. The blob is the prelude's serialization
 * (0x1E between records, 0x1F between key and value). */

#define STORE_CAP (16 * 1024)

static void store_path_of(char *out, int cap) {
    int p = 0;
    const char *pre = "/data/browser/";
    for (int i = 0; pre[i] && p < cap - 1; i++) out[p++] = pre[i];
    const char *u = g_url;
    int i = 0;
    if (has_scheme(u)) {
        while (u[i] && u[i] != ':') i++;
        i += 3;                            /* past "://" */
    }
    int wrote = 0;
    for (; u[i] && u[i] != '/' && u[i] != ':' && u[i] != '?' && p < cap - 4;
         i++) {
        char c = (char)lc(u[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-'))
            c = '_';
        out[p++] = c;
        wrote = 1;
    }
    if (!wrote) out[p++] = '_';
    out[p++] = '.'; out[p++] = 'l'; out[p++] = 's';
    out[p] = 0;
}

static JSValue js_dom_storeload(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char path[600];
    store_path_of(path, sizeof(path));
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return JS_NewString(cx, "");
    char *buf = (char *)malloc(STORE_CAP + 1);
    long n = buf ? sys_read((int)fd, buf, STORE_CAP) : -1;
    sys_close((int)fd);
    JSValue r = (n > 0) ? JS_NewStringLen(cx, buf, (size_t)n)
                        : JS_NewString(cx, "");
    if (buf) free(buf);
    return r;
}

static JSValue js_dom_storesave(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(cx, &len, argv[0]);
    if (!s) return JS_UNDEFINED;
    if (len > STORE_CAP) len = STORE_CAP;
    sys_mkdir("/data/browser", 0755);      /* idempotent */
    char path[600];
    store_path_of(path, sizeof(path));
    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        sys_write((int)fd, s, len);
        sys_close((int)fd);
    }
    JS_FreeCString(cx, s);
    return JS_UNDEFINED;
}

/* ---- IndexedDB persistence (stage 13) ------------------------------- *
 * One JSON blob per (origin, database): /data/browser/<host>.i_<db>.idb.
 * The whole IndexedDB machinery lives in the JS prelude; these two
 * primitives just load/save its serialization, exactly like the
 * localStorage pair above (whole-file writes; databases are small). */

#define IDB_CAP (64 * 1024)

static void idb_path_of(char *out, int cap, const char *db) {
    store_path_of(out, cap);               /* /data/browser/<host>.ls */
    int p = (int)str_len(out) - 3;         /* strip the ".ls" suffix */
    if (p < 0) p = 0;
    if (p < cap - 4) {
        out[p++] = '.'; out[p++] = 'i'; out[p++] = '_';
        for (int i = 0; db[i] && p < cap - 5; i++) {
            char c = (char)lc(db[i]);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-'))
                c = '_';
            out[p++] = c;
        }
        out[p++] = '.'; out[p++] = 'i'; out[p++] = 'd'; out[p++] = 'b';
    }
    out[p] = 0;
}

static JSValue js_dom_idbload(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NewString(cx, "");
    const char *db = JS_ToCString(cx, argv[0]);
    if (!db) return JS_NewString(cx, "");
    char path[600];
    idb_path_of(path, sizeof(path), db);
    JS_FreeCString(cx, db);
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return JS_NewString(cx, "");
    char *buf = (char *)malloc(IDB_CAP + 1);
    long n = buf ? sys_read((int)fd, buf, IDB_CAP) : -1;
    sys_close((int)fd);
    JSValue r = (n > 0) ? JS_NewStringLen(cx, buf, (size_t)n)
                        : JS_NewString(cx, "");
    if (buf) free(buf);
    return r;
}

static JSValue js_dom_idbsave(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    const char *db = JS_ToCString(cx, argv[0]);
    size_t len = 0;
    const char *s = db ? JS_ToCStringLen(cx, &len, argv[1]) : NULL;
    if (db && s) {
        if (len > IDB_CAP) len = IDB_CAP;
        sys_mkdir("/data/browser", 0755);  /* idempotent */
        char path[600];
        idb_path_of(path, sizeof(path), db);
        long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            if (len) sys_write((int)fd, s, len);
            sys_close((int)fd);
        }
    }
    if (s) JS_FreeCString(cx, s);
    if (db) JS_FreeCString(cx, db);
    return JS_UNDEFINED;
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

/* ---- stage 13I: Canvas 2D --------------------------------------------
 * A <canvas> owns an ARGB backing store (the img record created in
 * collect_node). These primitives draw straight into im->pixels in
 * userspace (no kernel calls); the display list paints the store as an
 * image. The prelude wraps them in a CanvasRenderingContext2D. */
#include "canvas_font.h"                 /* g_font8x8[95][8] */

static int g_canvas_dirty;               /* a canvas op needs a repaint */

static struct img *canvas_img(int ni) {
    if (ni <= 0 || ni >= E->nnodes) return NULL;
    int ii = E->nodes[ni].img;
    if (ii < 0 || ii >= g_nimages) return NULL;
    struct img *im = &g_images[ii];
    if (im->state != 1 || !im->pixels || im->w <= 0 || im->h <= 0) return NULL;
    return im;
}

/* source-over composite one pixel. */
static void cv_px(struct img *im, int x, int y, uint32_t rgba) {
    if ((unsigned)x >= (unsigned)im->w || (unsigned)y >= (unsigned)im->h) return;
    unsigned sa = (rgba >> 24) & 0xFF;
    if (sa == 0) return;
    uint32_t *d = &im->pixels[(long)y * im->w + x];
    if (sa == 255) { *d = rgba; return; }
    unsigned sr = (rgba >> 16) & 0xFF, sg = (rgba >> 8) & 0xFF, sb = rgba & 0xFF;
    uint32_t dv = *d;
    unsigned da = (dv >> 24) & 0xFF, dr = (dv >> 16) & 0xFF,
             dg = (dv >> 8) & 0xFF, db = dv & 0xFF;
    unsigned na = sa + da * (255 - sa) / 255;
    unsigned nr, ng, nb;
    if (na == 0) { nr = ng = nb = 0; }
    else {
        nr = (sr * sa + dr * da * (255 - sa) / 255) / na;
        ng = (sg * sa + dg * da * (255 - sa) / 255) / na;
        nb = (sb * sa + db * da * (255 - sa) / 255) / na;
    }
    *d = (na << 24) | (nr << 16) | (ng << 8) | nb;
}

static void cv_fill_rect(struct img *im, int x, int y, int w, int h, uint32_t rgba) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            cv_px(im, x + i, y + j, rgba);
}

static void cv_clear_rect(struct img *im, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int yy = y + j; if ((unsigned)yy >= (unsigned)im->h) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i; if ((unsigned)xx >= (unsigned)im->w) continue;
            im->pixels[(long)yy * im->w + xx] = 0;
        }
    }
}

static void cv_line(struct img *im, int x0, int y0, int x1, int y1,
                    uint32_t rgba, int width) {
    if (width < 1) width = 1;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady, hw = (width - 1) / 2;
    for (;;) {
        for (int j = -hw; j <= hw; j++)
            for (int i = -hw; i <= hw; i++) cv_px(im, x0 + i, y0 + j, rgba);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
}

/* even-odd scanline polygon fill (path fill + filled arcs). */
static void cv_fill_poly(struct img *im, const int *xs, const int *ys, int n,
                         uint32_t rgba) {
    if (n < 3) return;
    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) { if (ys[i] < ymin) ymin = ys[i];
                                  if (ys[i] > ymax) ymax = ys[i]; }
    if (ymin < 0) ymin = 0;
    if (ymax >= im->h) ymax = im->h - 1;
    for (int y = ymin; y <= ymax; y++) {
        int xi[128], m = 0;
        for (int i = 0; i < n && m < 128; i++) {
            int j = (i + 1) % n;
            int y0 = ys[i], y1 = ys[j], x0 = xs[i], x1 = xs[j];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y))
                xi[m++] = x0 + (int)((long)(y - y0) * (x1 - x0) / (y1 - y0));
        }
        for (int a = 0; a < m; a++)
            for (int b = a + 1; b < m; b++)
                if (xi[b] < xi[a]) { int t = xi[a]; xi[a] = xi[b]; xi[b] = t; }
        for (int a = 0; a + 1 < m; a += 2) {
            int xa = xi[a], xb = xi[a + 1];
            if (xa < 0) xa = 0;
            if (xb >= im->w) xb = im->w - 1;
            for (int x = xa; x <= xb; x++) cv_px(im, x, y, rgba);
        }
    }
}

static void cv_text(struct img *im, int x, int y, const char *s,
                    uint32_t rgba, int px) {
    if (px < 6) px = 6;
    int scale = px / 8; if (scale < 1) scale = 1;
    int top = y - 8 * scale, adv = 8 * scale, cx = x;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c <= 0x7E) {
            const unsigned char *g = g_font8x8[c - 0x20];
            for (int ry = 0; ry < 8; ry++) {
                unsigned char row = g[ry];
                for (int rx = 0; rx < 8; rx++)
                    if (row & (1u << rx))
                        for (int j = 0; j < scale; j++)
                            for (int i = 0; i < scale; i++)
                                cv_px(im, cx + rx * scale + i, top + ry * scale + j, rgba);
            }
        }
        cx += adv;
    }
}

static uint32_t js_u32(JSContext *cx, JSValueConst v) {
    uint32_t r = 0; JS_ToUint32(cx, &r, v); return r;
}
static int js_i32(JSContext *cx, JSValueConst v) {
    int32_t r = 0; JS_ToInt32(cx, &r, v); return r;
}

static JSValue js_cv_fillrect(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 6) return JS_UNDEFINED;
    struct img *im = canvas_img(jsi(cx, argv[0]));
    if (im) { cv_fill_rect(im, js_i32(cx, argv[1]), js_i32(cx, argv[2]),
                           js_i32(cx, argv[3]), js_i32(cx, argv[4]),
                           js_u32(cx, argv[5])); g_canvas_dirty = 1; }
    return JS_UNDEFINED;
}
static JSValue js_cv_clearrect(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 5) return JS_UNDEFINED;
    struct img *im = canvas_img(jsi(cx, argv[0]));
    if (im) { cv_clear_rect(im, js_i32(cx, argv[1]), js_i32(cx, argv[2]),
                            js_i32(cx, argv[3]), js_i32(cx, argv[4]));
              g_canvas_dirty = 1; }
    return JS_UNDEFINED;
}
static JSValue js_cv_line(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 7) return JS_UNDEFINED;
    struct img *im = canvas_img(jsi(cx, argv[0]));
    if (im) { cv_line(im, js_i32(cx, argv[1]), js_i32(cx, argv[2]),
                      js_i32(cx, argv[3]), js_i32(cx, argv[4]),
                      js_u32(cx, argv[5]), js_i32(cx, argv[6])); g_canvas_dirty = 1; }
    return JS_UNDEFINED;
}
static JSValue js_cv_fillpoly(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    struct img *im = canvas_img(jsi(cx, argv[0]));
    if (!im) return JS_UNDEFINED;
    JSValue arr = argv[1];
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(cx, arr, "length");
    JS_ToUint32(cx, &len, lv); JS_FreeValue(cx, lv);
    int npt = (int)(len / 2); if (npt > 256) npt = 256;
    if (npt >= 3) {
        static int xs[256], ys[256];
        for (int i = 0; i < npt; i++) {
            JSValue vx = JS_GetPropertyUint32(cx, arr, (uint32_t)(2 * i));
            JSValue vy = JS_GetPropertyUint32(cx, arr, (uint32_t)(2 * i + 1));
            xs[i] = js_i32(cx, vx); ys[i] = js_i32(cx, vy);
            JS_FreeValue(cx, vx); JS_FreeValue(cx, vy);
        }
        cv_fill_poly(im, xs, ys, npt, js_u32(cx, argv[2]));
        g_canvas_dirty = 1;
    }
    return JS_UNDEFINED;
}
static JSValue js_cv_text(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 6) return JS_UNDEFINED;
    struct img *im = canvas_img(jsi(cx, argv[0]));
    const char *s = JS_ToCString(cx, argv[3]);
    if (im && s) { cv_text(im, js_i32(cx, argv[1]), js_i32(cx, argv[2]), s,
                           js_u32(cx, argv[4]), js_i32(cx, argv[5])); g_canvas_dirty = 1; }
    if (s) JS_FreeCString(cx, s);
    return JS_UNDEFINED;
}
static JSValue js_cv_drawimg(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 6) return JS_UNDEFINED;
    struct img *im = canvas_img(jsi(cx, argv[0]));
    int sni = jsi(cx, argv[1]);
    if (im && sni > 0 && sni < E->nnodes) {
        int sii = E->nodes[sni].img;
        if (sii >= 0 && sii < g_nimages) {
            struct img *s = &g_images[sii];
            if (s->state == 1 && s->pixels && s->w > 0 && s->h > 0) {
                int dx = js_i32(cx, argv[2]), dy = js_i32(cx, argv[3]);
                int dw = js_i32(cx, argv[4]), dh = js_i32(cx, argv[5]);
                if (dw <= 0) dw = s->w; if (dh <= 0) dh = s->h;
                for (int j = 0; j < dh; j++) {
                    int sy = (int)((long)j * s->h / dh); if (sy >= s->h) sy = s->h - 1;
                    for (int i = 0; i < dw; i++) {
                        int sx = (int)((long)i * s->w / dw); if (sx >= s->w) sx = s->w - 1;
                        cv_px(im, dx + i, dy + j, s->pixels[(long)sy * s->w + sx]);
                    }
                }
                g_canvas_dirty = 1;
            }
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_cv_size(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct img *im = (argc >= 1) ? canvas_img(jsi(cx, argv[0])) : NULL;
    JSValue a = JS_NewArray(cx);
    JS_SetPropertyUint32(cx, a, 0, JS_NewInt32(cx, im ? im->w : 0));
    JS_SetPropertyUint32(cx, a, 1, JS_NewInt32(cx, im ? im->h : 0));
    return a;
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

/* ---- stage 13F: observers + getComputedStyle ----------------------- */

/* Format an ARGB color the way getComputedStyle reports it: "rgb(r, g, b)"
 * for opaque, "rgba(0, 0, 0, 0)" for the transparent sentinel. */
static void css_color_str(uint32_t c, char *out) {
    if ((c >> 24) == 0) {
        const char *z = "rgba(0, 0, 0, 0)";
        int p = 0; while (z[p]) { out[p] = z[p]; p++; } out[p] = 0;
        return;
    }
    int p = 0;
    out[p++]='r'; out[p++]='g'; out[p++]='b'; out[p++]='(';
    unsigned ch[3] = { (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF };
    for (int k = 0; k < 3; k++) {
        p = msg_append_int(out, p, 64, (int)ch[k]);
        if (k < 2) { out[p++] = ','; out[p++] = ' '; }
    }
    out[p++] = ')'; out[p] = 0;
}

/* "Npx" from an int. */
static void css_px_str(int v, char *out) {
    int p = msg_append_int(out, 0, 32, v < 0 ? 0 : v);
    out[p++] = 'p'; out[p++] = 'x'; out[p] = 0;
}

static void js_set_prop_str(JSContext *cx, JSValue o, const char *k, const char *v) {
    JS_SetPropertyStr(cx, o, k, JS_NewString(cx, v));
}

/* __dom.computed(node): the node's computed style as a plain object keyed
 * by kebab-case CSS property. Values are the used ones where a layout has
 * run (width/height come from the laid content box); otherwise specified.
 * The prelude adds camelCase aliases + getPropertyValue(). */
static JSValue js_dom_computed(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    JSValue o = JS_NewObject(cx);
    if (ni < 0) return o;
    struct dnode *n = &E->nodes[ni];
    struct cstyle *st = &n->st;
    char buf[64];

    static const char *disp_names[] = {
        "inline", "block", "none", "list-item", "inline-block",
        "table", "table-row-group", "table-row", "table-cell",
        "table-caption", "flex", "grid"
    };
    js_set_prop_str(cx, o, "display",
        st->disp < (int)(sizeof(disp_names)/sizeof(disp_names[0]))
            ? disp_names[st->disp] : "block");

    css_color_str(st->color, buf);           js_set_prop_str(cx, o, "color", buf);
    css_color_str(st->bg, buf);              js_set_prop_str(cx, o, "background-color", buf);
    css_px_str(st->px, buf);                 js_set_prop_str(cx, o, "font-size", buf);
    js_set_prop_str(cx, o, "font-weight", (st->fl & SF_BOLD) ? "700" : "400");
    js_set_prop_str(cx, o, "text-align",
        st->talign == 1 ? "center" : st->talign == 2 ? "right" : "left");
    js_set_prop_str(cx, o, "position",
        st->pos == 1 ? "relative" : st->pos == 2 ? "absolute" :
        st->pos == 3 ? "fixed" : "static");

    /* used content-box width/height from the laid border box when fresh */
    int cw = -1, chh = -1;
    if (n->lgen == E->lay_gen) {
        cw = n->lw - st->bw[3] - st->bw[1] - st->p[3] - st->p[1];
        chh = n->lh - st->bw[0] - st->bw[2] - st->p[0] - st->p[2];
        if (cw < 0) cw = 0;
        if (chh < 0) chh = 0;
    } else {
        if (st->width >= 0 && !(st->fl & SF_WPCT)) cw = st->width;
        if (st->height >= 0) chh = st->height;
    }
    if (cw >= 0) { css_px_str(cw, buf); js_set_prop_str(cx, o, "width", buf); }
    else js_set_prop_str(cx, o, "width", "auto");
    if (chh >= 0) { css_px_str(chh, buf); js_set_prop_str(cx, o, "height", buf); }
    else js_set_prop_str(cx, o, "height", "auto");

    static const char *mnames[4] = { "margin-top","margin-right","margin-bottom","margin-left" };
    static const char *pnames[4] = { "padding-top","padding-right","padding-bottom","padding-left" };
    for (int k = 0; k < 4; k++) {
        css_px_str(st->m[k] == M_AUTO ? 0 : st->m[k], buf);
        js_set_prop_str(cx, o, mnames[k], buf);
        css_px_str(st->p[k], buf);
        js_set_prop_str(cx, o, pnames[k], buf);
    }
    return o;
}

/* __dom.rect(node): laid border box [x, y, w, h, fixed] in DOC coords
 * (viewport coords when fixed), or null when the node was not laid in
 * the current generation (display:none / detached / pre-layout). */
static JSValue js_dom_rect(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    if (ni < 0 || E->nodes[ni].lgen != E->lay_gen) return JS_NULL;
    struct dnode *n = &E->nodes[ni];
    JSValue a = JS_NewArray(cx);
    JS_SetPropertyUint32(cx, a, 0, JS_NewInt32(cx, n->lx));
    JS_SetPropertyUint32(cx, a, 1, JS_NewInt32(cx, n->ly));
    JS_SetPropertyUint32(cx, a, 2, JS_NewInt32(cx, n->lw));
    JS_SetPropertyUint32(cx, a, 3, JS_NewInt32(cx, n->lh));
    JS_SetPropertyUint32(cx, a, 4,
        JS_NewInt32(cx, (n->flags & DNF_LAID_FIXED) ? 1 : 0));
    return a;
}

/* __dom.viewport(): [scrollY, contentHeight, contentWidth]. The content
 * band is what IntersectionObserver treats as the root. */
static JSValue js_dom_viewport(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue a = JS_NewArray(cx);
    JS_SetPropertyUint32(cx, a, 0, JS_NewInt32(cx, g_scroll_y));
    JS_SetPropertyUint32(cx, a, 1, JS_NewInt32(cx, g_content_h));
    int vw = g_layout_w > 12 ? g_layout_w - 12 : g_layout_w;
    JS_SetPropertyUint32(cx, a, 2, JS_NewInt32(cx, vw));
    return a;
}

/* __dom.takeMutations(): the pending mutation batch as [{node, kind}],
 * clearing it. kind: 0 childList, 1 attributes, 2 characterData. */
static JSValue js_dom_take_mut(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue a = JS_NewArray(cx);
    for (int k = 0; k < cur->mut_n; k++) {
        JSValue o = JS_NewObject(cx);
        JS_SetPropertyStr(cx, o, "node", JS_NewInt32(cx, cur->mut_node[k]));
        JS_SetPropertyStr(cx, o, "kind", JS_NewInt32(cx, cur->mut_kind[k]));
        JS_SetPropertyUint32(cx, a, (uint32_t)k, o);
    }
    cur->mut_n = 0;
    return a;
}

/* __dom.setObsCheck(fn): register the prelude's observer-delivery pass;
 * the pump calls it when the DOM mutates, the layout changes, the page
 * scrolls, or observe() kicks it. */
static JSValue js_dom_set_obscheck(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1 && JS_IsFunction(cx, argv[0])) {
        if (cur->js_has_obs_check) JS_FreeValue(cx, cur->js_obs_check);
        cur->js_obs_check = JS_DupValue(cx, argv[0]);
        cur->js_has_obs_check = 1;
    }
    return JS_UNDEFINED;
}

/* __dom.obsKick(): force one observer-check pass (a fresh observe()
 * must deliver the current state even without a mutation). */
static JSValue js_dom_obskick(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    cur->obs_kick = 1;
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
    for (int k = 0; k < JS_FETCH_MAX; k++)
        if (t->js_fetches[k].used) {
            sys_http_finish(t->js_fetches[k].handle);
            JS_FreeValue(cx, t->js_fetches[k].cb);
            t->js_fetches[k].used = 0;
        }
    for (int k = 0; k < JS_WS_MAX; k++)
        if (t->js_ws[k].used) {
            sys_ws_close(t->js_ws[k].handle, 1001 /* going away */);
            JS_FreeValue(cx, t->js_ws[k].obj);
            t->js_ws[k].used = 0;
        }
    if (t->js_has_dispatch) JS_FreeValue(cx, t->js_dispatch);
    t->js_has_dispatch = 0;
    if (t->js_has_obs_check) JS_FreeValue(cx, t->js_obs_check);
    t->js_has_obs_check = 0;
    t->mut_n = 0;
    t->obs_kick = 0;
    t->obs_gen = 0;
    t->obs_scroll = 0;
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
"      D.fetchStart(String(u), function(r){\n"
"        if (!r || r.status <= 0){ rej(new Error('fetch failed')); return; }\n"
"        res({\n"
"          ok: r.status >= 200 && r.status < 300,\n"
"          status: r.status,\n"
"          url: r.url,\n"
"          text: function(){ return Promise.resolve(r.body); },\n"
"          json: function(){ return Promise.resolve(JSON.parse(r.body)); }\n"
"        });\n"
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
/* ---- stage 13: WebSocket (RFC 6455) ------------------------------- */
"  function WebSocket(url){\n"
"    var self = this;\n"
"    this.url = String(url);\n"
"    this.readyState = 0;\n"
"    this.bufferedAmount = 0; this.protocol = ''; this.extensions = '';\n"
"    this.binaryType = 'blob';\n"
"    this.onopen = null; this.onmessage = null;\n"
"    this.onclose = null; this.onerror = null;\n"
"    this.__ls = {};\n"
"    this.__fire = function(type, a, b){\n"
"      var e;\n"
"      if (type === 'open'){ self.readyState = 1; e = { type: 'open' }; }\n"
"      else if (type === 'message')\n"
"        e = { type: 'message', data: a, origin: '', lastEventId: '' };\n"
"      else if (type === 'close'){ self.readyState = 3;\n"
"        e = { type: 'close', code: a|0, reason: '', wasClean: !!b }; }\n"
"      else e = { type: 'error' };\n"
"      e.target = self; e.currentTarget = self;\n"
"      var h = self['on' + type];\n"
"      if (typeof h === 'function'){\n"
"        try { h.call(self, e); } catch(ex){ g.console.log('[ws-err]', ex); }\n"
"      }\n"
"      var ls = self.__ls[type] || [];\n"
"      for (var k = 0; k < ls.length; k++){\n"
"        try { ls[k].call(self, e); } catch(ex){ g.console.log('[ws-err]', ex); }\n"
"      }\n"
"    };\n"
"    this.__h = D.wsOpen(this.url, this);\n"
"    if (this.__h < 0) g.setTimeout(function(){\n"
"      self.readyState = 3;\n"
"      self.__fire('error'); self.__fire('close', 1006, false);\n"
"    }, 0);\n"
"  }\n"
"  WebSocket.CONNECTING = WebSocket.prototype.CONNECTING = 0;\n"
"  WebSocket.OPEN = WebSocket.prototype.OPEN = 1;\n"
"  WebSocket.CLOSING = WebSocket.prototype.CLOSING = 2;\n"
"  WebSocket.CLOSED = WebSocket.prototype.CLOSED = 3;\n"
"  WebSocket.prototype.send = function(d){\n"
"    if (this.readyState !== 1 || this.__h < 0)\n"
"      throw new Error('InvalidStateError: WebSocket is not open');\n"
"    D.wsSend(this.__h, String(d));\n"
"  };\n"
"  WebSocket.prototype.close = function(code){\n"
"    if (this.readyState >= 2) return;\n"
"    this.readyState = 2;\n"
"    var self = this, h = this.__h, c = (code === undefined) ? 1000 : code|0;\n"
"    this.__h = -1;\n"
"    if (h >= 0) D.wsClose(h, c);\n"
"    g.setTimeout(function(){ self.__fire('close', c, true); }, 0);\n"
"  };\n"
"  WebSocket.prototype.addEventListener = function(t, f){\n"
"    t = String(t).toLowerCase();\n"
"    (this.__ls[t] = this.__ls[t] || []).push(f);\n"
"  };\n"
"  WebSocket.prototype.removeEventListener = function(t, f){\n"
"    var l = this.__ls[String(t).toLowerCase()];\n"
"    if (!l) return;\n"
"    var ix = l.indexOf(f); if (ix >= 0) l.splice(ix, 1);\n"
"  };\n"
"  g.WebSocket = WebSocket;\n"
/* ---- stage 13: media element control ------------------------------ */
"  Element.prototype.play = function(){\n"
"    D.mediaCtl(this.__i, 1);\n"
"    return Promise.resolve();\n"
"  };\n"
"  Element.prototype.pause = function(){ D.mediaCtl(this.__i, 0); };\n"
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
"    back: function(){ D.histGo(-1); },\n"
"    forward: function(){ D.histGo(1); },\n"
"    go: function(n){ if (n|0) D.histGo(n|0); },\n"
"    state: null\n"
"  };\n"
"  function mkStorage(persist){\n"
"    var st = {};\n"
"    if (persist){\n"
"      var blob = D.storeLoad();\n"
"      if (blob) blob.split('\\x1e').forEach(function(rec){\n"
"        var i = rec.indexOf('\\x1f');\n"
"        if (i > 0) st[rec.slice(0, i)] = rec.slice(i + 1);\n"
"      });\n"
"    }\n"
"    function save(){\n"
"      if (!persist) return;\n"
"      D.storeSave(Object.keys(st).map(function(k){\n"
"        return k + '\\x1f' + st[k];\n"
"      }).join('\\x1e'));\n"
"    }\n"
"    var o = {\n"
"      getItem: function(k){ return (k in st) ? st[k] : null; },\n"
"      setItem: function(k, v){ st[String(k)] = String(v); save(); },\n"
"      removeItem: function(k){ delete st[k]; save(); },\n"
"      clear: function(){ st = {}; save(); },\n"
"      key: function(i){ return Object.keys(st)[i] || null; }\n"
"    };\n"
"    Object.defineProperty(o, 'length',\n"
"      { get: function(){ return Object.keys(st).length; } });\n"
"    return o;\n"
"  }\n"
"  g.localStorage = mkStorage(true);\n"
"  g.sessionStorage = mkStorage(false);\n"
/* ---- stage 13: IndexedDB (v1) ------------------------------------- */
"  (function(){\n"
"    var dbs = {};\n"
"    function later(f){ g.setTimeout(f, 0); }\n"
"    function mkReq(){\n"
"      return { result: undefined, error: null, readyState: 'pending',\n"
"               onsuccess: null, onerror: null, onupgradeneeded: null,\n"
"               addEventListener: function(t, f){ this['on' + t] = f; },\n"
"               removeEventListener: function(){} };\n"
"    }\n"
"    function fire(r, ok){\n"
"      r.readyState = 'done';\n"
"      var e = { target: r, type: ok ? 'success' : 'error' };\n"
"      var h = ok ? r.onsuccess : r.onerror;\n"
"      if (typeof h === 'function'){\n"
"        try { h.call(r, e); } catch(ex){ g.console.log('[idb-err]', ex); }\n"
"      }\n"
"    }\n"
"    function load(name){\n"
"      if (dbs[name]) return dbs[name];\n"
"      var raw = D.idbLoad(String(name)), d = null;\n"
"      if (raw){ try { d = JSON.parse(raw); } catch(e){} }\n"
"      if (!d || typeof d !== 'object' || !d.stores)\n"
"        d = { version: 0, stores: {} };\n"
"      dbs[name] = d;\n"
"      return d;\n"
"    }\n"
"    function save(name){ D.idbSave(String(name), JSON.stringify(dbs[name])); }\n"
"    function ks(k){ return (typeof k === 'number') ? 'n:' + k : 's:' + String(k); }\n"
"    function sortedKeys(st){\n"
"      var ns = [], ss = [];\n"
"      for (var k in st.recs){\n"
"        if (k.charAt(0) === 'n') ns.push({ raw: k, key: parseFloat(k.slice(2)) });\n"
"        else ss.push({ raw: k, key: k.slice(2) });\n"
"      }\n"
"      ns.sort(function(a, b){ return a.key - b.key; });\n"
"      ss.sort(function(a, b){ return a.key < b.key ? -1 : a.key > b.key ? 1 : 0; });\n"
"      return ns.concat(ss);\n"
"    }\n"
"    function clone(v){ return (v === undefined) ? undefined : JSON.parse(JSON.stringify(v)); }\n"
"    function txSettle(tx){\n"
"      if (!tx) return;\n"
"      tx.__pend--;\n"
"      if (tx.__pend === 0) later(function(){\n"
"        if (tx.__pend !== 0 || tx.__done) return;\n"
"        tx.__done = true;\n"
"        if (typeof tx.oncomplete === 'function'){\n"
"          try { tx.oncomplete({ target: tx }); }\n"
"          catch(ex){ g.console.log('[idb-err]', ex); }\n"
"        }\n"
"      });\n"
"    }\n"
"    function txReq(tx, work){\n"
"      var r = mkReq();\n"
"      if (tx) tx.__pend++;\n"
"      later(function(){\n"
"        var ok = true;\n"
"        try { r.result = work(r); } catch(ex){ ok = false; r.error = ex; }\n"
"        fire(r, ok);\n"
"        txSettle(tx);\n"
"      });\n"
"      return r;\n"
"    }\n"
"    function mkStore(dbw, tx, sname){\n"
"      var st = dbw.__d.stores[sname];\n"
"      if (!st) throw new Error('NotFoundError: no object store ' + sname);\n"
"      function keyFor(v, k, adding){\n"
"        if (k === undefined && st.keyPath && v && typeof v === 'object'){\n"
"          k = v[st.keyPath];\n"
"          if (k === undefined && st.autoIncrement){\n"
"            k = ++st.seq; v[st.keyPath] = k;\n"
"          }\n"
"        }\n"
"        if (k === undefined && st.autoIncrement) k = ++st.seq;\n"
"        if (k === undefined) throw new Error('DataError: no key');\n"
"        if (adding && (ks(k) in st.recs)) throw new Error('ConstraintError');\n"
"        if (typeof k === 'number' && k > st.seq) st.seq = Math.floor(k);\n"
"        return k;\n"
"      }\n"
"      return {\n"
"        name: sname, keyPath: st.keyPath || null,\n"
"        autoIncrement: !!st.autoIncrement, transaction: tx,\n"
"        put: function(v, k){ return txReq(tx, function(){\n"
"          v = clone(v); var kk = keyFor(v, k, false);\n"
"          st.recs[ks(kk)] = v; save(dbw.__name); return kk; }); },\n"
"        add: function(v, k){ return txReq(tx, function(){\n"
"          v = clone(v); var kk = keyFor(v, k, true);\n"
"          st.recs[ks(kk)] = v; save(dbw.__name); return kk; }); },\n"
"        get: function(k){ return txReq(tx, function(){\n"
"          return clone(st.recs[ks(k)]); }); },\n"
"        getAll: function(){ return txReq(tx, function(){\n"
"          return sortedKeys(st).map(function(e){ return clone(st.recs[e.raw]); }); }); },\n"
"        getAllKeys: function(){ return txReq(tx, function(){\n"
"          return sortedKeys(st).map(function(e){ return e.key; }); }); },\n"
"        count: function(){ return txReq(tx, function(){\n"
"          return sortedKeys(st).length; }); },\n"
"        'delete': function(k){ return txReq(tx, function(){\n"
"          delete st.recs[ks(k)]; save(dbw.__name); return undefined; }); },\n"
"        clear: function(){ return txReq(tx, function(){\n"
"          st.recs = {}; save(dbw.__name); return undefined; }); },\n"
"        openCursor: function(){\n"
"          var keys = null, i = 0;\n"
"          var r = mkReq();\n"
"          function sched(){\n"
"            if (tx) tx.__pend++;\n"
"            later(function(){\n"
"              if (!keys) keys = sortedKeys(st);\n"
"              if (i < keys.length){\n"
"                var e = keys[i++];\n"
"                r.result = { key: e.key, primaryKey: e.key,\n"
"                             value: clone(st.recs[e.raw]),\n"
"                             continue: sched };\n"
"              } else r.result = null;\n"
"              fire(r, true);\n"
"              txSettle(tx);\n"
"            });\n"
"          }\n"
"          sched();\n"
"          return r;\n"
"        },\n"
"        createIndex: function(){ return { get: function(){\n"
"          return txReq(tx, function(){ return undefined; }); } }; },\n"
"        index: function(){ throw new Error('NotFoundError: indexes not supported (v1)'); }\n"
"      };\n"
"    }\n"
"    function mkTx(dbw, mode){\n"
"      var tx = { mode: mode || 'readonly', db: dbw, error: null,\n"
"                 oncomplete: null, onerror: null, onabort: null,\n"
"                 __pend: 0, __done: false,\n"
"                 abort: function(){},\n"
"                 addEventListener: function(t, f){ this['on' + t] = f; } };\n"
"      tx.objectStore = function(n){ return mkStore(dbw, tx, n); };\n"
"      later(function(){ later(function(){\n"
"        if (tx.__pend !== 0 || tx.__done) return;\n"
"        tx.__done = true;\n"
"        if (typeof tx.oncomplete === 'function'){\n"
"          try { tx.oncomplete({ target: tx }); }\n"
"          catch(ex){ g.console.log('[idb-err]', ex); }\n"
"        }\n"
"      }); });\n"
"      return tx;\n"
"    }\n"
"    function mkDb(name, d){\n"
"      var dbw = {\n"
"        name: name, __d: d, __name: name,\n"
"        objectStoreNames: {\n"
"          contains: function(n){ return !!d.stores[n]; },\n"
"          item: function(i){ return Object.keys(d.stores)[i] || null; }\n"
"        },\n"
"        createObjectStore: function(n, opts){\n"
"          opts = opts || {};\n"
"          d.stores[n] = { keyPath: opts.keyPath || null,\n"
"                          autoIncrement: !!opts.autoIncrement,\n"
"                          seq: 0, recs: {} };\n"
"          save(name);\n"
"          return mkStore(dbw, null, n);\n"
"        },\n"
"        deleteObjectStore: function(n){ delete d.stores[n]; save(name); },\n"
"        transaction: function(names, mode){ return mkTx(dbw, mode); },\n"
"        close: function(){},\n"
"        onversionchange: null\n"
"      };\n"
"      Object.defineProperty(dbw, 'version',\n"
"        { get: function(){ return d.version; } });\n"
"      Object.defineProperty(dbw.objectStoreNames, 'length',\n"
"        { get: function(){ return Object.keys(d.stores).length; } });\n"
"      return dbw;\n"
"    }\n"
"    g.indexedDB = {\n"
"      open: function(name, version){\n"
"        var r = mkReq();\n"
"        later(function(){\n"
"          var d = load(name);\n"
"          var dbw = mkDb(name, d);\n"
"          var want = (version === undefined) ? (d.version || 1) : (version | 0);\n"
"          if (want > d.version){\n"
"            var oldv = d.version;\n"
"            d.version = want;\n"
"            r.result = dbw;\n"
"            r.transaction = mkTx(dbw, 'versionchange');\n"
"            if (typeof r.onupgradeneeded === 'function'){\n"
"              try { r.onupgradeneeded({ target: r, oldVersion: oldv,\n"
"                                        newVersion: want }); }\n"
"              catch(ex){ g.console.log('[idb-err]', ex); }\n"
"            }\n"
"            save(name);\n"
"          }\n"
"          r.result = dbw;\n"
"          fire(r, true);\n"
"        });\n"
"        return r;\n"
"      },\n"
"      deleteDatabase: function(name){\n"
"        var r = mkReq();\n"
"        later(function(){\n"
"          delete dbs[name];\n"
"          D.idbSave(String(name), '');\n"
"          fire(r, true);\n"
"        });\n"
"        return r;\n"
"      },\n"
"      cmp: function(a, b){ return a < b ? -1 : a > b ? 1 : 0; }\n"
"    };\n"
"  })();\n"
/* ---- stage 13F: getComputedStyle + laid-rect geometry ------------- */
"  g.getComputedStyle = function(elm){\n"
"    var o = (elm && elm.__i >= 0) ? D.computed(elm.__i) : {};\n"
"    o.getPropertyValue = function(p){ return (p in this) ? String(this[p]) : ''; };\n"
"    ['color','background-color','font-size','font-weight','text-align',\n"
"     'display','position','width','height',\n"
"     'margin-top','margin-right','margin-bottom','margin-left',\n"
"     'padding-top','padding-right','padding-bottom','padding-left'\n"
"    ].forEach(function(k){\n"
"      var cc = k.replace(/-([a-z])/g, function(m, c){ return c.toUpperCase(); });\n"
"      if (cc !== k && (k in o)) o[cc] = o[k];\n"
"    });\n"
"    return o;\n"
"  };\n"
"  function rectVP(i){\n"
"    var r = D.rect(i);\n"
"    if (!r) return null;\n"
"    var vp = D.viewport();\n"
"    var sy = r[4] ? 0 : vp[0];\n"          /* fixed: ignore scroll */
"    var top = r[1] - sy, left = r[0];\n"
"    return { x: left, y: top, left: left, top: top, width: r[2], height: r[3],\n"
"             right: left + r[2], bottom: top + r[3] };\n"
"  }\n"
"  Element.prototype.getBoundingClientRect = function(){\n"
"    return rectVP(this.__i) ||\n"
"      { x:0, y:0, top:0, left:0, right:0, bottom:0, width:0, height:0 };\n"
"  };\n"
"  Element.prototype.getClientRects = function(){\n"
"    var r = this.getBoundingClientRect(); return [r];\n"
"  };\n"
"  Object.defineProperties(Element.prototype, {\n"
"    offsetWidth:  { get: function(){ var r = D.rect(this.__i); return r ? r[2] : 0; } },\n"
"    offsetHeight: { get: function(){ var r = D.rect(this.__i); return r ? r[3] : 0; } },\n"
"    offsetLeft:   { get: function(){ var r = D.rect(this.__i); return r ? r[0] : 0; } },\n"
"    offsetTop:    { get: function(){ var r = D.rect(this.__i); return r ? r[1] : 0; } },\n"
"    offsetParent: { get: function(){ return g.document.body; } },\n"
"    clientWidth:  { get: function(){ var r = D.rect(this.__i); return r ? r[2] : 0; } },\n"
"    clientHeight: { get: function(){ var r = D.rect(this.__i); return r ? r[3] : 0; } },\n"
"    scrollWidth:  { get: function(){ var r = D.rect(this.__i); return r ? r[2] : 0; } },\n"
"    scrollHeight: { get: function(){ var r = D.rect(this.__i); return r ? r[3] : 0; } },\n"
"    scrollTop:    { get: function(){ return 0; }, set: function(v){} },\n"
"    scrollLeft:   { get: function(){ return 0; }, set: function(v){} }\n"
"  });\n"
"  Object.defineProperties(g, {\n"
"    scrollY:     { get: function(){ return D.viewport()[0]; } },\n"
"    pageYOffset: { get: function(){ return D.viewport()[0]; } },\n"
"    scrollX:     { get: function(){ return 0; } },\n"
"    pageXOffset: { get: function(){ return 0; } },\n"
"    innerHeight: { get: function(){ return D.viewport()[1]; } },\n"
"    innerWidth:  { get: function(){ return D.viewport()[2]; } }\n"
"  });\n"
"  g.scrollTo = function(){}; g.scrollBy = function(){};\n"
/* ---- stage 13I: Canvas 2D ---------------------------------------- */
"  function cvColor(s){\n"
"    if (typeof s !== 'string') return 0xFF000000;\n"
"    s = s.trim().toLowerCase();\n"
"    var named = { black:'#000', white:'#fff', red:'#ff0000', green:'#008000',\n"
"      blue:'#0000ff', gray:'#808080', grey:'#808080', orange:'#ffa500',\n"
"      yellow:'#ffff00', purple:'#800080', lime:'#00ff00', navy:'#000080',\n"
"      teal:'#008080', silver:'#c0c0c0', maroon:'#800000', cyan:'#00ffff',\n"
"      magenta:'#ff00ff', transparent:'rgba(0,0,0,0)' };\n"
"    if (named[s]) s = named[s];\n"
"    if (s.charAt(0) === '#'){\n"
"      var h = s.slice(1);\n"
"      if (h.length === 3) h = h.charAt(0)+h.charAt(0)+h.charAt(1)+h.charAt(1)+h.charAt(2)+h.charAt(2);\n"
"      if (h.length >= 6){\n"
"        var r=parseInt(h.slice(0,2),16), gg=parseInt(h.slice(2,4),16), b=parseInt(h.slice(4,6),16);\n"
"        return (0xFF000000 | (r<<16) | (gg<<8) | b) >>> 0;\n"
"      }\n"
"    }\n"
"    if (s.indexOf('rgb') === 0){\n"
"      var lp=s.indexOf('('), rp=s.indexOf(')');\n"
"      var p=s.slice(lp+1, rp).split(',');\n"
"      var cr=parseInt(p[0])|0, cg=parseInt(p[1])|0, cb=parseInt(p[2])|0;\n"
"      var a=p.length>3 ? Math.round(parseFloat(p[3])*255) : 255;\n"
"      return ((a<<24)|(cr<<16)|(cg<<8)|cb) >>> 0;\n"
"    }\n"
"    return 0xFF000000;\n"
"  }\n"
"  function makeCtx(node){\n"
"    var i = node.__i;\n"
"    var ctx = {\n"
"      canvas: node, fillStyle:'#000000', strokeStyle:'#000000',\n"
"      lineWidth:1, font:'10px sans-serif', textAlign:'left', globalAlpha:1,\n"
"      __p: [],\n"
"      fillRect: function(x,y,w,h){ D.cvFillRect(i,x|0,y|0,w|0,h|0,cvColor(this.fillStyle)); },\n"
"      clearRect: function(x,y,w,h){ D.cvClearRect(i,x|0,y|0,w|0,h|0); },\n"
"      strokeRect: function(x,y,w,h){\n"
"        var c=cvColor(this.strokeStyle), lw=(this.lineWidth|0)||1;\n"
"        D.cvLine(i,x|0,y|0,(x+w)|0,y|0,c,lw); D.cvLine(i,(x+w)|0,y|0,(x+w)|0,(y+h)|0,c,lw);\n"
"        D.cvLine(i,(x+w)|0,(y+h)|0,x|0,(y+h)|0,c,lw); D.cvLine(i,x|0,(y+h)|0,x|0,y|0,c,lw);\n"
"      },\n"
"      beginPath: function(){ this.__p = []; },\n"
"      moveTo: function(x,y){ this.__p.push([0,x,y]); },\n"
"      lineTo: function(x,y){ this.__p.push([1,x,y]); },\n"
"      closePath: function(){ this.__p.push([2]); },\n"
"      rect: function(x,y,w,h){ this.moveTo(x,y); this.lineTo(x+w,y);\n"
"        this.lineTo(x+w,y+h); this.lineTo(x,y+h); this.closePath(); },\n"
"      arc: function(cx,cy,r,a0,a1){\n"
"        var steps=Math.max(10, Math.floor(r));\n"
"        for (var k=0;k<=steps;k++){ var a=a0+(a1-a0)*k/steps;\n"
"          var x=cx+r*Math.cos(a), y=cy+r*Math.sin(a);\n"
"          this.__p.push([k===0?0:1, x, y]); }\n"
"      },\n"
"      stroke: function(){\n"
"        var c=cvColor(this.strokeStyle), lw=(this.lineWidth|0)||1;\n"
"        var px=0, py=0, fx=0, fy=0, have=false;\n"
"        for (var k=0;k<this.__p.length;k++){ var s=this.__p[k];\n"
"          if (s[0]===0){ px=s[1]; py=s[2]; fx=px; fy=py; have=true; }\n"
"          else if (s[0]===1){ D.cvLine(i,px|0,py|0,s[1]|0,s[2]|0,c,lw); px=s[1]; py=s[2]; }\n"
"          else if (s[0]===2 && have){ D.cvLine(i,px|0,py|0,fx|0,fy|0,c,lw); px=fx; py=fy; } }\n"
"      },\n"
"      fill: function(){\n"
"        var pts=[];\n"
"        for (var k=0;k<this.__p.length;k++){ var s=this.__p[k];\n"
"          if (s[0]===0||s[0]===1){ pts.push(s[1]|0); pts.push(s[2]|0); } }\n"
"        if (pts.length>=6) D.cvFillPoly(i, pts, cvColor(this.fillStyle));\n"
"      },\n"
"      fillText: function(s,x,y){ var px=parseInt(this.font)||12;\n"
"        D.cvText(i,x|0,y|0,String(s),cvColor(this.fillStyle),px); },\n"
"      strokeText: function(s,x,y){ this.fillText(s,x,y); },\n"
"      measureText: function(s){ var px=parseInt(this.font)||12;\n"
"        return { width: String(s).length * (px|0) }; },\n"
"      drawImage: function(img,dx,dy,dw,dh){\n"
"        if (img && img.__i !== undefined)\n"
"          D.cvDrawImg(i, img.__i, dx|0, dy|0, (dw||0)|0, (dh||0)|0); },\n"
"      createLinearGradient: function(){ var self=this;\n"
"        return { __c:'#888', addColorStop:function(o,c){ this.__c=c; } }; },\n"
"      save: function(){}, restore: function(){}, translate: function(){},\n"
"      scale: function(){}, rotate: function(){}, setTransform: function(){},\n"
"      setLineDash: function(){}, clip: function(){}\n"
"    };\n"
"    return ctx;\n"
"  }\n"
"  Element.prototype.getContext = function(type){\n"
"    if (String(type) !== '2d') return null;\n"
"    if (!this.__ctx) this.__ctx = makeCtx(this);\n"
"    return this.__ctx;\n"
"  };\n"
"  Object.defineProperties(Element.prototype, {\n"
"    width: { get: function(){ var s=D.cvSize(this.__i);\n"
"        if (s[0]) return s[0];\n"
"        var a=D.getAttr(this.__i,'width'); return a?(parseInt(a)||0):0; },\n"
"      set: function(v){ D.setAttr(this.__i,'width',String(v|0)); } },\n"
"    height: { get: function(){ var s=D.cvSize(this.__i);\n"
"        if (s[1]) return s[1];\n"
"        var a=D.getAttr(this.__i,'height'); return a?(parseInt(a)||0):0; },\n"
"      set: function(v){ D.setAttr(this.__i,'height',String(v|0)); } }\n"
"  });\n"
/* ---- stage 13F: Mutation / Intersection / Resize observers -------- */
"  var mutObservers = [], interObservers = [], resizeObservers = [];\n"
"  function isDescendant(node, anc){\n"
"    var n = D.parent(node);\n"
"    while (n >= 0){ if (n === anc) return true; n = D.parent(n); }\n"
"    return false;\n"
"  }\n"
"  function MutationObserver(cb){ this.__cb = cb; this.__targets = []; }\n"
"  MutationObserver.prototype.observe = function(elm, opts){\n"
"    if (!elm) return;\n"
"    this.__targets.push({ node: elm.__i, opts: opts || {} });\n"
"    if (mutObservers.indexOf(this) < 0) mutObservers.push(this);\n"
"  };\n"
"  MutationObserver.prototype.disconnect = function(){\n"
"    this.__targets = [];\n"
"    var ix = mutObservers.indexOf(this); if (ix >= 0) mutObservers.splice(ix, 1);\n"
"  };\n"
"  MutationObserver.prototype.takeRecords = function(){ return []; };\n"
"  function IntersectionObserver(cb, options){\n"
"    this.__cb = cb; this.__targets = new Map();\n"
"    this.root = (options && options.root) || null;\n"
"    this.rootMargin = (options && options.rootMargin) || '0px';\n"
"    var th = options && options.threshold;\n"
"    this.thresholds = (th == null) ? [0] : (th.length ? th.slice() : [th]);\n"
"  }\n"
"  IntersectionObserver.prototype.observe = function(elm){\n"
"    if (!elm) return;\n"
"    this.__targets.set(elm.__i, { last: null });\n"
"    if (interObservers.indexOf(this) < 0) interObservers.push(this);\n"
"    D.obsKick();\n"
"  };\n"
"  IntersectionObserver.prototype.unobserve = function(elm){\n"
"    if (elm) this.__targets.delete(elm.__i);\n"
"  };\n"
"  IntersectionObserver.prototype.disconnect = function(){\n"
"    this.__targets = new Map();\n"
"    var ix = interObservers.indexOf(this); if (ix >= 0) interObservers.splice(ix, 1);\n"
"  };\n"
"  IntersectionObserver.prototype.takeRecords = function(){ return []; };\n"
"  function ResizeObserver(cb){ this.__cb = cb; this.__targets = new Map(); }\n"
"  ResizeObserver.prototype.observe = function(elm){\n"
"    if (!elm) return;\n"
"    this.__targets.set(elm.__i, { w: -1, h: -1 });\n"
"    if (resizeObservers.indexOf(this) < 0) resizeObservers.push(this);\n"
"    D.obsKick();\n"
"  };\n"
"  ResizeObserver.prototype.unobserve = function(elm){\n"
"    if (elm) this.__targets.delete(elm.__i);\n"
"  };\n"
"  ResizeObserver.prototype.disconnect = function(){\n"
"    this.__targets = new Map();\n"
"    var ix = resizeObservers.indexOf(this); if (ix >= 0) resizeObservers.splice(ix, 1);\n"
"  };\n"
"  g.MutationObserver = MutationObserver;\n"
"  g.WebKitMutationObserver = MutationObserver;\n"
"  g.IntersectionObserver = IntersectionObserver;\n"
"  g.ResizeObserver = ResizeObserver;\n"
"  function obsCheck(){\n"
"    var muts = D.takeMutations();\n"
"    if (muts.length && mutObservers.length){\n"
"      for (var mo = 0; mo < mutObservers.length; mo++){\n"
"        var obs = mutObservers[mo], records = [];\n"
"        for (var ti = 0; ti < obs.__targets.length; ti++){\n"
"          var tg = obs.__targets[ti];\n"
"          for (var mi = 0; mi < muts.length; mi++){\n"
"            var m = muts[mi], matched = (m.node === tg.node);\n"
"            if (!matched && tg.opts.subtree) matched = isDescendant(m.node, tg.node);\n"
"            if (!matched) continue;\n"
"            var typ = m.kind === 1 ? 'attributes'\n"
"                    : m.kind === 2 ? 'characterData' : 'childList';\n"
"            if (typ === 'attributes' && tg.opts.attributes === false) continue;\n"
"            if (typ === 'characterData' && tg.opts.characterData === false) continue;\n"
"            if (typ === 'childList' && !tg.opts.childList && !tg.opts.subtree) continue;\n"
"            records.push({ type: typ, target: el(m.node), addedNodes: [],\n"
"                           removedNodes: [], attributeName: null,\n"
"                           previousSibling: null, nextSibling: null,\n"
"                           oldValue: null });\n"
"          }\n"
"        }\n"
"        if (records.length){\n"
"          try { obs.__cb.call(obs, records, obs); }\n"
"          catch(e){ g.console.log('[obs-err]', e); }\n"
"        }\n"
"      }\n"
"    }\n"
"    for (var io = 0; io < interObservers.length; io++){\n"
"      var iobs = interObservers[io], vp = D.viewport(), entries = [];\n"
"      iobs.__targets.forEach(function(state, node){\n"
"        var r = rectVP(node);\n"
"        var vis = !!(r && r.bottom > 0 && r.top < vp[1] &&\n"
"                     r.right > 0 && r.left < vp[2]);\n"
"        if (state.last === vis) return;\n"
"        state.last = vis;\n"
"        var ratio = 0, ir = null;\n"
"        if (r){\n"
"          var iy0 = Math.max(0, r.top), iy1 = Math.min(vp[1], r.bottom);\n"
"          var ih = Math.max(0, iy1 - iy0);\n"
"          ratio = (r.height > 0) ? ih / r.height : (vis ? 1 : 0);\n"
"          ir = { top: iy0, bottom: iy1, left: r.left, right: r.right,\n"
"                 width: r.width, height: ih, x: r.left, y: iy0 };\n"
"        }\n"
"        entries.push({ target: el(node), isIntersecting: vis,\n"
"          intersectionRatio: vis ? ratio : 0, boundingClientRect: r,\n"
"          intersectionRect: ir, time: Date.now(),\n"
"          rootBounds: { top: 0, left: 0, bottom: vp[1], right: vp[2],\n"
"                        width: vp[2], height: vp[1] } });\n"
"      });\n"
"      if (entries.length){\n"
"        try { iobs.__cb.call(iobs, entries, iobs); }\n"
"        catch(e){ g.console.log('[obs-err]', e); }\n"
"      }\n"
"    }\n"
"    for (var ro = 0; ro < resizeObservers.length; ro++){\n"
"      var robs = resizeObservers[ro], rentries = [];\n"
"      robs.__targets.forEach(function(state, node){\n"
"        var r = D.rect(node);\n"
"        if (!r) return;\n"
"        var w = r[2], h = r[3];\n"
"        if (state.w === w && state.h === h) return;\n"
"        state.w = w; state.h = h;\n"
"        rentries.push({ target: el(node),\n"
"          contentRect: { x: 0, y: 0, width: w, height: h,\n"
"                         top: 0, left: 0, right: w, bottom: h },\n"
"          borderBoxSize: [{ inlineSize: w, blockSize: h }],\n"
"          contentBoxSize: [{ inlineSize: w, blockSize: h }] });\n"
"      });\n"
"      if (rentries.length){\n"
"        try { robs.__cb.call(robs, rentries, robs); }\n"
"        catch(e){ g.console.log('[obs-err]', e); }\n"
"      }\n"
"    }\n"
"  }\n"
"  D.setObsCheck(obsCheck);\n"
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
            {"fetchStart", js_dom_fetchstart, 2},
            {"wsOpen", js_ws_openprim, 2},
            {"wsSend", js_ws_sendprim, 2},
            {"wsClose", js_ws_closeprim, 2},
            {"mediaCtl", js_media_ctl, 2},
            {"storeLoad", js_dom_storeload, 0},
            {"storeSave", js_dom_storesave, 1},
            {"idbLoad", js_dom_idbload, 1},
            {"idbSave", js_dom_idbsave, 2},
            {"getValue", js_dom_getvalue, 1},
            {"setValue", js_dom_setvalue, 2},
            {"setDispatcher", js_dom_setdispatcher, 1},
            {"insBefore", js_dom_insbefore, 3},
            {"removeAttr", js_dom_removeattr, 2},
            {"childNodes", js_dom_childnodes, 1},
            {"nextSib", js_dom_nextsib, 1},
            {"pushState", js_dom_pushstate, 1},
            {"histGo", js_dom_histgo, 1},
            {"navigate", js_dom_navigate, 1},
            {"href", js_dom_href, 0},
            {"computed", js_dom_computed, 1},
            {"rect", js_dom_rect, 1},
            {"viewport", js_dom_viewport, 0},
            {"takeMutations", js_dom_take_mut, 0},
            {"setObsCheck", js_dom_set_obscheck, 1},
            {"obsKick", js_dom_obskick, 0},
            {"cvFillRect", js_cv_fillrect, 6},
            {"cvClearRect", js_cv_clearrect, 5},
            {"cvLine", js_cv_line, 7},
            {"cvFillPoly", js_cv_fillpoly, 3},
            {"cvText", js_cv_text, 6},
            {"cvDrawImg", js_cv_drawimg, 6},
            {"cvSize", js_cv_size, 1},
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
    var_scope_reset();
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
    if (g_canvas_dirty && !g_js_in_load) {
        g_canvas_dirty = 0;
        tk_redraw(&win);                  /* canvas draw from an event handler */
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
        /* Stage 12D: settle completed fetch() transfers. The slot is
         * freed BEFORE the callback runs so the handler can fetch
         * again; js_drain_jobs below runs the promise chain. */
        for (int k = 0; k < JS_FETCH_MAX; k++) {
            struct jsfetch *jf = &t->js_fetches[k];
            if (!jf->used) continue;
            struct http_poll pi;
            long pr = sys_http_poll(jf->handle, &pi);
            if (pr == 0 &&
                (pi.state == HTTPA_QUEUED || pi.state == HTTPA_RUNNING))
                continue;
            JSValue arg = JS_NULL;
            if (pr == 0 && pi.state == HTTPA_DONE) {
                long n2 = 0;
                char *buf = NULL;
                if (pi.body_len > 0) {
                    buf = (char *)malloc(pi.body_len + 1);
                    n2 = buf ? sys_http_read(jf->handle, buf, 0,
                                             pi.body_len) : -1;
                }
                if (n2 >= 0) {
                    JSValue obj = JS_NewObject(t->js_cx);
                    JS_SetPropertyStr(t->js_cx, obj, "status",
                                      JS_NewInt32(t->js_cx, pi.status));
                    JS_SetPropertyStr(t->js_cx, obj, "url",
                                      JS_NewString(t->js_cx, pi.final_url));
                    JS_SetPropertyStr(t->js_cx, obj, "body",
                        JS_NewStringLen(t->js_cx, buf ? buf : "",
                                        (size_t)n2));
                    arg = obj;
                }
                if (buf) free(buf);
            }
            sys_http_finish(jf->handle);
            JSValue cb = jf->cb;
            jf->used = 0;
            JSValue r = JS_Call(t->js_cx, cb, JS_UNDEFINED, 1, &arg);
            if (JS_IsException(r)) js_dump_error(t->js_cx);
            JS_FreeValue(t->js_cx, r);
            JS_FreeValue(t->js_cx, cb);
            JS_FreeValue(t->js_cx, arg);
            work = 1;
        }
        /* Stage 13: WebSocket events. Every __fire may run user JS
         * that closes the socket (freeing the slot via wsClose), so
         * re-check w->used after each callback. Terminal events
         * detach the slot BEFORE firing. */
        for (int k = 0; k < JS_WS_MAX; k++) {
            struct jsws *w = &t->js_ws[k];
            if (!w->used) continue;
            struct ws_poll_info pi;
            if (sys_ws_poll(w->handle, &pi) != 0) {
                /* kernel forgot the handle: surface an abnormal close */
                JSValue obj = w->obj;
                w->used = 0;
                js_ws_fire(t->js_cx, obj, "error", JS_UNDEFINED, JS_UNDEFINED);
                js_ws_fire(t->js_cx, obj, "close",
                           JS_NewInt32(t->js_cx, 1006), JS_FALSE);
                JS_FreeValue(t->js_cx, obj);
                work = 1;
                continue;
            }
            if (pi.state == WS_ST_OPEN && w->last_state == WS_ST_CONNECTING) {
                w->last_state = WS_ST_OPEN;
                JSValue obj = JS_DupValue(t->js_cx, w->obj);
                js_ws_fire(t->js_cx, obj, "open", JS_UNDEFINED, JS_UNDEFINED);
                JS_FreeValue(t->js_cx, obj);
                work = 1;
                if (!w->used) continue;
            }
            int guard = 16;                /* bound per-tick delivery */
            while (w->used && pi.msg_avail && guard-- > 0) {
                char *buf = (char *)malloc(pi.msg_len + 1);
                if (!buf) break;
                long n = sys_ws_recv(w->handle, buf,
                                     pi.msg_len ? pi.msg_len : 1);
                if (n < 0) { free(buf); break; }
                JSValue data = JS_NewStringLen(t->js_cx, buf, (size_t)n);
                free(buf);
                JSValue obj = JS_DupValue(t->js_cx, w->obj);
                js_ws_fire(t->js_cx, obj, "message", data, JS_UNDEFINED);
                JS_FreeValue(t->js_cx, obj);
                work = 1;
                if (!w->used || sys_ws_poll(w->handle, &pi) != 0) break;
            }
            if (!w->used) continue;
            if (pi.state == WS_ST_CLOSED && !pi.msg_avail) {
                JSValue obj = w->obj;
                int code = pi.close_code ? pi.close_code
                                         : (pi.err ? 1006 : 1000);
                sys_ws_close(w->handle, 0);      /* free the kernel slot */
                w->used = 0;
                if (pi.err)
                    js_ws_fire(t->js_cx, obj, "error",
                               JS_UNDEFINED, JS_UNDEFINED);
                js_ws_fire(t->js_cx, obj, "close",
                           JS_NewInt32(t->js_cx, code),
                           pi.err ? JS_FALSE : JS_TRUE);
                JS_FreeValue(t->js_cx, obj);
                work = 1;
            }
        }
        js_drain_jobs(t);
        if (g_js_dirty) {
            g_js_dirty = 0;
            js_rerender();
            tk_redraw(&win);
            work = 1;
        }
        /* stage 13I: canvas draws don't touch the DOM/layout -- just
         * repaint so the updated backing store shows (timers / rAF). */
        if (g_canvas_dirty) {
            g_canvas_dirty = 0;
            tk_redraw(&win);
            work = 1;
        }
        /* stage 13F: deliver observer callbacks. Runs when the DOM
         * mutated (MutationObserver), the layout changed or the page
         * scrolled (Intersection/Resize), or a fresh observe() kicked
         * it. Rects are fresh here (any g_js_dirty relayout just ran).
         * Callbacks may mutate the DOM -> one more relayout below. */
        if (t->js_has_obs_check &&
            (t->obs_kick || t->mut_n > 0 ||
             t->obs_gen != E->lay_gen || t->obs_scroll != g_scroll_y)) {
            t->obs_kick = 0;
            t->obs_gen = E->lay_gen;
            t->obs_scroll = g_scroll_y;
            JSValue r = JS_Call(t->js_cx, t->js_obs_check, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(r)) js_dump_error(t->js_cx);
            JS_FreeValue(t->js_cx, r);
            js_drain_jobs(t);
            if (g_js_dirty) {
                g_js_dirty = 0;
                js_rerender();
                tk_redraw(&win);
                t->obs_gen = E->lay_gen;   /* our own relayout, not a change */
            }
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

/* Download each not-yet-loaded @font-face src and register it with the
 * kernel rasterizer (stage 13E). Synchronous, once per font, at load
 * time (like linked stylesheets). URLs resolve against the page. */
#define WEBFONT_CAP (1u << 20)
static void load_webfonts(void) {
    for (int k = 0; k < g_nwebfonts; k++) {
        struct webfont *w = &g_webfonts[k];
        if (w->face >= 0 || w->tried) continue;
        w->tried = 1;
        char url[URL_MAX + 1];
        resolve_relative_url(g_url, w->url, url, URL_MAX);
        if (!has_scheme(url)) continue;
        uint8_t *buf = (uint8_t *)malloc(WEBFONT_CAP);
        if (!buf) continue;
        struct http_fetch req;
        mem_zero(&req, sizeof(req));
        req.url = (unsigned long)url;
        req.buf = (unsigned long)buf;
        req.buf_sz = WEBFONT_CAP;
        long nf = sys_http_fetch(&req);
        if (nf > 12 && req.status >= 200 && req.status < 400) {
            long face = sys_gui_font_register(buf, (unsigned long)nf);
            if (face >= 0)
                w->face = (int)face;
        }
        free(buf);
    }
}

static void render_html(void) {
    if (!E) return;
    cur->doc_gen++;                       /* new document (popstate gen) */
    js_teardown(cur);                     /* the old page's JS world dies */
    images_free();
    media_free();
    page_reset();

    dom_build();
    css_parse_sheet(UA_SHEET, (long)sizeof(UA_SHEET) - 1, 0);
    g_form_open = -1;
    collect_node(0);
    load_webfonts();                      /* @font-face -> download + register */

    struct cstyle base;
    st_init(&base, NULL);
    base.disp = D_BLOCK;
    var_scope_reset();
    style_node(0, &base);

    g_view_mode = VIEW_HTML;
    layout(g_win_w);                      /* initial style+layout BEFORE
                                             scripts so synchronous
                                             getComputedStyle / rect reads
                                             see real computed values (13F) */

    /* phase 9/10: run scripts now that the DOM is styled + laid. Browsers
     * force a style/layout flush for synchronous measurement; here it is
     * already done. Scripts that mutate the DOM set g_js_dirty -> one
     * re-collect/style/layout folds the changes in. */
    g_js_in_load = 1;
    run_scripts();
    g_js_in_load = 0;
    if (g_js_dirty) js_rerender();
    g_js_dirty = 0;
}

/* Whole-buffer monospace document (plain text + source view):
 * synthesize doc > body > pre > #text and run the normal pipeline. */
static void render_mono_doc(void) {
    if (!E) return;
    cur->doc_gen++;                       /* new document (popstate gen) */
    js_teardown(cur);
    images_free();
    media_free();
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
    var_scope_reset();
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
        for (int i = 0; i < HISTORY_MAX - 1; i++) {
            str_copy(g_history[i], g_history[i + 1], URL_MAX);
            cur->hist_gen[i] = cur->hist_gen[i + 1];
        }
        g_hist_pos = HISTORY_MAX - 1;
    }
    str_copy(g_history[g_hist_pos], url, URL_MAX);
    cur->hist_gen[g_hist_pos] = cur->doc_gen;
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
        e->lay_gen = 0;
    }
    t->hist_pos     = -1;
    t->focus_url    = 1;
    t->view_mode    = VIEW_HTML;
    t->find_run     = -1;
    t->focus_field  = -1;
    t->nav_handle   = -1;
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
static void img_fetch_cancel(void);

static void tab_close(int idx) {
    if (idx < 0 || idx >= g_ntabs) return;
    if (g_ntabs == 1) sys_exit(0);
    if (g_tabs[idx].nav_handle >= 0) {       /* cancel an in-flight nav */
        sys_http_finish(g_tabs[idx].nav_handle);
        g_tabs[idx].nav_handle = -1;
    }
    img_fetch_cancel();                      /* tab indices shift below */
    media_fetch_cancel();
    tab_images_free(&g_tabs[idx]);
    tab_media_free(&g_tabs[idx]);
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
        "Connection reset", "Certificate not trusted"     /* -10: 13H */
    };
    int idx = -err;
    return (idx >= 1 && idx <= 10) ? errs[idx] : "Unknown error";
}
#define HTTPE_CERT (-10)
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

    if (err == HTTPE_CERT) {
        /* stage 13H: a TLS certificate failure is a security condition,
         * not a plain connectivity error -- present it as such and offer
         * no one-click bypass (fail closed). */
        g_raw_len = 0;
        raw_append("<html><head><title>Your connection is not private</title>"
                   "<style>body{background-color:#ffffff;color:#202124}"
                   ".box{max-width:540px;margin:24px auto;padding:8px 18px;"
                   "border:1px solid #dadce0;background-color:#fef7f6}"
                   "h1{font-size:21px;color:#c5221f}"
                   ".url{font-family:monospace;color:#5f6368}</style>"
                   "</head><body><div class=box>"
                   "<h1>&#9888; Your connection is not private</h1>"
                   "<p class=url>");
        raw_append(url);
        raw_append("</p><p>tobyOS could not verify this site's TLS "
                   "certificate against its trusted root store. The "
                   "certificate may be <b>expired</b>, <b>self-signed</b>, "
                   "issued for a <b>different hostname</b>, or signed by an "
                   "authority tobyOS does not trust.</p>"
                   "<p>The connection was refused to protect you from a "
                   "possible impostor. Error: <b>");
        raw_append(emsg);
        raw_append("</b></p><hr><p>Press <b>[</b> to go back.</p>"
                   "</div></body></html>");
        g_raw[g_raw_len] = '\0';
        render_html();
        update_title();
        return;
    }

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

/* Free a tab's media elements: file data, MP3 decoder, audio stream.
 * (The video backing store is an img record -- tab_images_free owns
 * that.) Safe to call repeatedly. */
static void tab_media_free(struct tab *t) {
    for (int k = 0; k < t->nmedia; k++) {
        struct media_el *m = &t->media[k];
        if (!m->used) continue;
        if (m->data) { free(m->data); m->data = NULL; }
        if (m->mp3)  { toby_mp3_decode_free((toby_mp3_decoder_t *)m->mp3); m->mp3 = NULL; }
        if (m->audio_fd >= 0) { sys_audio_close(m->audio_fd); m->audio_fd = -1; }
        m->used = 0;
        m->playing = 0;
    }
}

static void media_free(void) { media_fetch_cancel(); tab_media_free(cur); }

/* Cooperative (incremental) image loading. Images parse as state=0
 * "pending"; the main idle loop calls load_one_pending_image() which
 * fetches+decodes exactly ONE pending image per call -- across ALL tabs,
 * active first -- so the UI stays responsive between images and
 * background tabs fill in too. (A single image's fetch still blocks
 * briefly since the kernel HTTP is synchronous; that's bounded to one
 * image, vs the old freeze-for-all-N.) Returns 1 if it did work. */
/* ===================== Minimal SVG renderer (stage 12E) ============== *
 * Enough for icons (Wikipedia chrome, favicons-as-svg): <rect>
 * <circle> <ellipse> <polygon> <polyline> <path> with solid fills
 * (fill= attribute or style="fill:.."), <g> fill inheritance,
 * viewBox scaling, even-odd scanline filling of flattened outlines.
 * No strokes, gradients, transforms, text, masks or clipping (defs/
 * mask/gradient subtrees are skipped). Renders into a malloc'd
 * ARGB8888 canvas with a transparent background; small icons render
 * at 2x for a crisper nearest-neighbor blit. */

extern double sin(double);
extern double cos(double);

#define SVG_MAX_PTS   1536
#define SVG_MAX_SUBS  48
#define SVG_MAX_DIM   512

struct svgpoly {
    float px[SVG_MAX_PTS], py[SVG_MAX_PTS];
    int   sub0[SVG_MAX_SUBS];
    int   nsubs, npts;
};

static void svgp_moveto(struct svgpoly *p, float x, float y) {
    if (p->nsubs >= SVG_MAX_SUBS || p->npts >= SVG_MAX_PTS) return;
    p->sub0[p->nsubs++] = p->npts;
    p->px[p->npts] = x; p->py[p->npts] = y; p->npts++;
}
static void svgp_lineto(struct svgpoly *p, float x, float y) {
    if (p->nsubs == 0) { svgp_moveto(p, x, y); return; }
    if (p->npts >= SVG_MAX_PTS) return;
    p->px[p->npts] = x; p->py[p->npts] = y; p->npts++;
}

/* Even-odd scanline fill across every (implicitly closed) subpath. */
static void svg_fill_poly(uint32_t *canvas, int W, int H,
                          const struct svgpoly *p, uint32_t argb) {
    if (p->npts < 3) return;
    float xs[64];
    for (int y = 0; y < H; y++) {
        float fy = (float)y + 0.5f;
        int nx = 0;
        for (int s = 0; s < p->nsubs; s++) {
            int i0 = p->sub0[s];
            int i1 = (s + 1 < p->nsubs) ? p->sub0[s + 1] : p->npts;
            int cnt = i1 - i0;
            if (cnt < 2) continue;
            for (int i = 0; i < cnt; i++) {
                float y1 = p->py[i0 + i];
                float y2 = p->py[i0 + (i + 1) % cnt];
                if ((y1 <= fy && y2 > fy) || (y2 <= fy && y1 > fy)) {
                    float x1 = p->px[i0 + i];
                    float x2 = p->px[i0 + (i + 1) % cnt];
                    float t = (fy - y1) / (y2 - y1);
                    if (nx < 64) xs[nx++] = x1 + t * (x2 - x1);
                }
            }
        }
        for (int i = 1; i < nx; i++) {          /* insertion sort */
            float v = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        for (int i = 0; i + 1 < nx; i += 2) {
            int xa = (int)(xs[i] + 0.5f), xb = (int)(xs[i + 1] + 0.5f);
            if (xa < 0) xa = 0;
            if (xb > W) xb = W;
            for (int x = xa; x < xb; x++) canvas[(long)y * W + x] = argb;
        }
    }
}

/* Number scanner: [ws,][-+]digits[.digits][eN]. */
static float svg_num(const char *s, int n, int *pos) {
    int i = *pos;
    while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' ||
                     s[i] == '\n' || s[i] == '\r')) i++;
    float sign = 1.0f, v = 0.0f;
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        if (s[i] == '-') sign = -1.0f;
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (i < n && s[i] == '.') {
        i++;
        float f = 0.1f;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v += (s[i] - '0') * f; f *= 0.1f; i++; }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        int es = 1, ev = 0;
        if (i < n && (s[i] == '-' || s[i] == '+')) { if (s[i] == '-') es = -1; i++; }
        while (i < n && s[i] >= '0' && s[i] <= '9') { ev = ev * 10 + (s[i] - '0'); i++; }
        while (ev-- > 0) v = es > 0 ? v * 10.0f : v * 0.1f;
    }
    *pos = i;
    return sign * v;
}

static long svg_tag_close(const char *s, long n, long i) {
    while (i < n && s[i] != '>') i++;
    return i;
}

/* attr="value" scan inside one tag's [i, end). */
static int svg_attr(const char *s, long i, long end, const char *name,
                    char *out, int cap) {
    int nl = (int)str_len(name);
    for (long k = i; k + nl + 1 < end; k++) {
        if (str_ncasecmp(s + k, name, nl) != 0) continue;
        if (k > i && s[k - 1] != ' ' && s[k - 1] != '\t' &&
            s[k - 1] != '\n' && s[k - 1] != '\r') continue;
        long q = k + nl;
        while (q < end && (s[q] == ' ' || s[q] == '\t')) q++;
        if (q >= end || s[q] != '=') continue;
        q++;
        while (q < end && (s[q] == ' ' || s[q] == '\t')) q++;
        char quote = (q < end && (s[q] == '"' || s[q] == '\'')) ? s[q] : 0;
        if (quote) q++;
        int o = 0;
        while (q < end && o < cap - 1 &&
               (quote ? s[q] != quote : (s[q] != ' ' && s[q] != '>')))
            out[o++] = s[q++];
        out[o] = 0;
        return 1;
    }
    return 0;
}

/* Effective fill for a shape tag: fill= attr, style="fill:..", else
 * the inherited <g> fill. Sets *none for fill:none. */
static void svg_fill_of(const char *s, long i, long end,
                        uint32_t inherit, int inherit_none,
                        uint32_t *out, int *none) {
    char a[128];
    *out = inherit;
    *none = inherit_none;
    if (svg_attr(s, i, end, "fill", a, sizeof(a))) {
        if (str_eq(a, "none")) { *none = 1; return; }
        uint32_t col;
        if (css_color_tok(a, (int)str_len(a), &col)) {
            *out = col | 0xFF000000u;
            *none = 0;
            return;
        }
    }
    if (svg_attr(s, i, end, "style", a, sizeof(a))) {
        int al = (int)str_len(a);
        int fo = str_contains(a, al, "fill:", 5);
        if (fo >= 0) {
            int v0 = fo + 5;
            while (v0 < al && a[v0] == ' ') v0++;
            int v1 = v0;
            while (v1 < al && a[v1] != ';') v1++;
            if (v1 - v0 == 4 && str_ncasecmp(a + v0, "none", 4) == 0) {
                *none = 1;
                return;
            }
            uint32_t col;
            if (css_color_tok(a + v0, v1 - v0, &col)) {
                *out = col | 0xFF000000u;
                *none = 0;
            }
        }
    }
}

static void svg_flat_cubic(struct svgpoly *p, float x0, float y0,
                           float x1, float y1, float x2, float y2,
                           float x3, float y3) {
    for (int k = 1; k <= 12; k++) {
        float t = (float)k / 12.0f, u = 1.0f - t;
        float x = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
        float y = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
        svgp_lineto(p, x, y);
    }
}
static void svg_flat_quad(struct svgpoly *p, float x0, float y0,
                          float x1, float y1, float x2, float y2) {
    for (int k = 1; k <= 8; k++) {
        float t = (float)k / 8.0f, u = 1.0f - t;
        float x = u*u*x0 + 2*u*t*x1 + t*t*x2;
        float y = u*u*y0 + 2*u*t*y1 + t*t*y2;
        svgp_lineto(p, x, y);
    }
}

/* Path data -> flattened subpaths, mapped from user units into canvas
 * pixels via (v - o) * s. Unsupported S/T degrade to lines toward the
 * endpoint's control shape; A (arc) degrades to a line. */
static void svg_path_d(const char *d, int n, struct svgpoly *p,
                       float sxx, float syy, float ox, float oy) {
    int i = 0;
    float cx = 0, cy = 0, sx0 = 0, sy0 = 0;
    char cmd = 0;
    while (i < n) {
        int before = i;
        while (i < n && (d[i] == ' ' || d[i] == ',' || d[i] == '\n' ||
                         d[i] == '\r' || d[i] == '\t')) i++;
        if (i >= n) break;
        char c = d[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cmd = c; i++; }
        if (!cmd) break;
        int rel = (cmd >= 'a');
        char C = rel ? (char)(cmd - 32) : cmd;
        switch (C) {
        case 'M': {
            float x = svg_num(d, n, &i), y = svg_num(d, n, &i);
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y; sx0 = x; sy0 = y;
            svgp_moveto(p, (x - ox) * sxx, (y - oy) * syy);
            cmd = rel ? 'l' : 'L';
            break;
        }
        case 'L': {
            float x = svg_num(d, n, &i), y = svg_num(d, n, &i);
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            svgp_lineto(p, (x - ox) * sxx, (y - oy) * syy);
            break;
        }
        case 'H': {
            float x = svg_num(d, n, &i);
            if (rel) x += cx;
            cx = x;
            svgp_lineto(p, (x - ox) * sxx, (cy - oy) * syy);
            break;
        }
        case 'V': {
            float y = svg_num(d, n, &i);
            if (rel) y += cy;
            cy = y;
            svgp_lineto(p, (cx - ox) * sxx, (y - oy) * syy);
            break;
        }
        case 'C': case 'S': {
            float x1, y1, x2, y2, x, y;
            if (C == 'C') { x1 = svg_num(d, n, &i); y1 = svg_num(d, n, &i); }
            else { x1 = cx; y1 = cy; }
            x2 = svg_num(d, n, &i); y2 = svg_num(d, n, &i);
            x  = svg_num(d, n, &i); y  = svg_num(d, n, &i);
            if (rel) {
                if (C == 'C') { x1 += cx; y1 += cy; }
                x2 += cx; y2 += cy; x += cx; y += cy;
            }
            svg_flat_cubic(p, (cx-ox)*sxx, (cy-oy)*syy, (x1-ox)*sxx, (y1-oy)*syy,
                           (x2-ox)*sxx, (y2-oy)*syy, (x-ox)*sxx, (y-oy)*syy);
            cx = x; cy = y;
            break;
        }
        case 'Q': {
            float x1 = svg_num(d, n, &i), y1 = svg_num(d, n, &i);
            float x  = svg_num(d, n, &i), y  = svg_num(d, n, &i);
            if (rel) { x1 += cx; y1 += cy; x += cx; y += cy; }
            svg_flat_quad(p, (cx-ox)*sxx, (cy-oy)*syy, (x1-ox)*sxx, (y1-oy)*syy,
                          (x-ox)*sxx, (y-oy)*syy);
            cx = x; cy = y;
            break;
        }
        case 'T': {
            float x = svg_num(d, n, &i), y = svg_num(d, n, &i);
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            svgp_lineto(p, (x - ox) * sxx, (y - oy) * syy);
            break;
        }
        case 'A': {
            (void)svg_num(d, n, &i); (void)svg_num(d, n, &i);
            (void)svg_num(d, n, &i); (void)svg_num(d, n, &i);
            (void)svg_num(d, n, &i);
            float x = svg_num(d, n, &i), y = svg_num(d, n, &i);
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y;
            svgp_lineto(p, (x - ox) * sxx, (y - oy) * syy);
            break;
        }
        case 'Z':
            cx = sx0; cy = sy0;            /* filler closes implicitly */
            break;
        default:
            i++;
            break;
        }
        if (i == before) i++;              /* safety: always advance */
    }
}

static int svg_sniff(const uint8_t *b, long n) {
    for (long i = 0; i + 4 < n && i < 512; i++)
        if (b[i] == '<' && lc(b[i+1]) == 's' && lc(b[i+2]) == 'v' &&
            lc(b[i+3]) == 'g')
            return 1;
    return 0;
}

/* Render an SVG buffer to a fresh ARGB canvas; NULL on anything we
 * can't make sense of. */
static uint32_t *svg_render(const uint8_t *bufu, long n, int *out_w, int *out_h) {
    const char *s = (const char *)bufu;
    long i = 0;
    while (i + 4 < n && !(s[i] == '<' && lc(s[i+1]) == 's' &&
                          lc(s[i+2]) == 'v' && lc(s[i+3]) == 'g')) i++;
    if (i + 4 >= n) return NULL;
    long at = i + 4;
    long end = svg_tag_close(s, n, at);
    char a[256];
    float vbx = 0, vby = 0, vbw = 0, vbh = 0, aw = 0, ah = 0;
    if (svg_attr(s, at, end, "viewBox", a, sizeof(a))) {
        int p2 = 0, al = (int)str_len(a);
        vbx = svg_num(a, al, &p2); vby = svg_num(a, al, &p2);
        vbw = svg_num(a, al, &p2); vbh = svg_num(a, al, &p2);
    }
    if (svg_attr(s, at, end, "width", a, sizeof(a))) {
        int p2 = 0;
        aw = svg_num(a, (int)str_len(a), &p2);
        if (str_contains(a, (int)str_len(a), "em", 2) >= 0) aw = 0;
    }
    if (svg_attr(s, at, end, "height", a, sizeof(a))) {
        int p2 = 0;
        ah = svg_num(a, (int)str_len(a), &p2);
        if (str_contains(a, (int)str_len(a), "em", 2) >= 0) ah = 0;
    }
    if (vbw <= 0 || vbh <= 0) {
        vbx = vby = 0;
        vbw = aw > 0 ? aw : 24;
        vbh = ah > 0 ? ah : vbw;
    }
    if (aw <= 0 && ah > 0) aw = ah * vbw / vbh;
    if (ah <= 0 && aw > 0) ah = aw * vbh / vbw;
    if (aw <= 0) { aw = vbw; ah = vbh; }
    int W = (int)(aw + 0.5f), H = (int)(ah + 0.5f);
    if (W < 1 || H < 1) return NULL;
    if (W <= 64 && H <= 64) { W *= 2; H *= 2; }   /* crisper small icons */
    if (W > SVG_MAX_DIM) { H = H * SVG_MAX_DIM / W; W = SVG_MAX_DIM; }
    if (H > SVG_MAX_DIM) { W = W * SVG_MAX_DIM / H; H = SVG_MAX_DIM; }
    if (W < 1 || H < 1) return NULL;
    float sxx = (float)W / vbw, syy = (float)H / vbh;

    uint32_t *canvas = (uint32_t *)malloc((size_t)W * H * 4);
    struct svgpoly *poly = (struct svgpoly *)malloc(sizeof(struct svgpoly));
    char *dbuf = (char *)malloc(8192);
    if (!canvas || !poly || !dbuf) {
        if (canvas) free(canvas);
        if (poly) free(poly);
        if (dbuf) free(dbuf);
        return NULL;
    }
    for (long k = 0; k < (long)W * H; k++) canvas[k] = 0;

    uint32_t gfill[8];
    int gnone[8], gdepth = 0, skip_depth = 0;
    gfill[0] = 0xFF000000u;
    gnone[0] = 0;

    i = end + 1;
    while (i < n) {
        while (i < n && s[i] != '<') i++;
        if (i + 1 >= n) break;
        if (s[i + 1] == '!' || s[i + 1] == '?') {         /* comment/PI */
            i = svg_tag_close(s, n, i) + 1;
            continue;
        }
        if (s[i + 1] == '/') {                            /* closing tag */
            char nm[16];
            long k = i + 2;
            int o = 0;
            while (k < n && s[k] != '>' && s[k] != ' ' && o < 15)
                nm[o++] = (char)lc(s[k++]);
            nm[o] = 0;
            if (str_eq(nm, "svg")) break;
            if (str_eq(nm, "g")) { if (gdepth > 0) gdepth--; }
            else if (str_eq(nm, "defs") || str_eq(nm, "mask") ||
                     str_eq(nm, "clippath") || str_eq(nm, "symbol") ||
                     str_eq(nm, "pattern") || str_eq(nm, "lineargradient") ||
                     str_eq(nm, "radialgradient")) {
                if (skip_depth > 0) skip_depth--;
            }
            i = svg_tag_close(s, n, i) + 1;
            continue;
        }
        char nm[16];
        long k = i + 1;
        int o = 0;
        while (k < n && (is_alpha(s[k]) || (s[k] >= '0' && s[k] <= '9')) &&
               o < 15)
            nm[o++] = (char)lc(s[k++]);
        nm[o] = 0;
        long tend = svg_tag_close(s, n, k);
        int selfclose = (tend > i && s[tend - 1] == '/');

        if (str_eq(nm, "defs") || str_eq(nm, "mask") ||
            str_eq(nm, "clippath") || str_eq(nm, "symbol") ||
            str_eq(nm, "pattern") || str_eq(nm, "lineargradient") ||
            str_eq(nm, "radialgradient")) {
            if (!selfclose) skip_depth++;
            i = tend + 1;
            continue;
        }
        if (str_eq(nm, "g")) {
            if (!selfclose && gdepth < 7) {
                uint32_t f = gfill[gdepth];
                int no = gnone[gdepth];
                svg_fill_of(s, k, tend, f, no, &f, &no);
                gdepth++;
                gfill[gdepth] = f;
                gnone[gdepth] = no;
            }
            i = tend + 1;
            continue;
        }
        if (skip_depth == 0) {
            uint32_t fill;
            int none;
            svg_fill_of(s, k, tend, gfill[gdepth], gnone[gdepth],
                        &fill, &none);
            poly->nsubs = 0;
            poly->npts = 0;
            if (!none && str_eq(nm, "rect")) {
                float x = 0, y = 0, w = 0, h = 0;
                int p2;
                if (svg_attr(s, k, tend, "x", a, sizeof(a))) { p2 = 0; x = svg_num(a, (int)str_len(a), &p2); }
                if (svg_attr(s, k, tend, "y", a, sizeof(a))) { p2 = 0; y = svg_num(a, (int)str_len(a), &p2); }
                if (svg_attr(s, k, tend, "width", a, sizeof(a))) { p2 = 0; w = svg_num(a, (int)str_len(a), &p2); }
                if (svg_attr(s, k, tend, "height", a, sizeof(a))) { p2 = 0; h = svg_num(a, (int)str_len(a), &p2); }
                if (w > 0 && h > 0) {
                    svgp_moveto(poly, (x-vbx)*sxx, (y-vby)*syy);
                    svgp_lineto(poly, (x+w-vbx)*sxx, (y-vby)*syy);
                    svgp_lineto(poly, (x+w-vbx)*sxx, (y+h-vby)*syy);
                    svgp_lineto(poly, (x-vbx)*sxx, (y+h-vby)*syy);
                    svg_fill_poly(canvas, W, H, poly, fill);
                }
            } else if (!none && (str_eq(nm, "circle") || str_eq(nm, "ellipse"))) {
                float cx = 0, cy = 0, rx = 0, ry = 0;
                int p2;
                if (svg_attr(s, k, tend, "cx", a, sizeof(a))) { p2 = 0; cx = svg_num(a, (int)str_len(a), &p2); }
                if (svg_attr(s, k, tend, "cy", a, sizeof(a))) { p2 = 0; cy = svg_num(a, (int)str_len(a), &p2); }
                if (svg_attr(s, k, tend, "r", a, sizeof(a))) { p2 = 0; rx = ry = svg_num(a, (int)str_len(a), &p2); }
                if (svg_attr(s, k, tend, "rx", a, sizeof(a))) { p2 = 0; rx = svg_num(a, (int)str_len(a), &p2); }
                if (svg_attr(s, k, tend, "ry", a, sizeof(a))) { p2 = 0; ry = svg_num(a, (int)str_len(a), &p2); }
                if (rx > 0 && ry > 0) {
                    for (int seg = 0; seg < 40; seg++) {
                        double th = 6.28318530718 * seg / 40.0;
                        float x = cx + rx * (float)cos(th);
                        float y = cy + ry * (float)sin(th);
                        if (seg == 0) svgp_moveto(poly, (x-vbx)*sxx, (y-vby)*syy);
                        else svgp_lineto(poly, (x-vbx)*sxx, (y-vby)*syy);
                    }
                    svg_fill_poly(canvas, W, H, poly, fill);
                }
            } else if (!none && (str_eq(nm, "polygon") || str_eq(nm, "polyline"))) {
                if (svg_attr(s, k, tend, "points", dbuf, 8192)) {
                    int p2 = 0, dn = (int)str_len(dbuf), first = 1;
                    while (p2 < dn) {
                        int save = p2;
                        float x = svg_num(dbuf, dn, &p2);
                        float y = svg_num(dbuf, dn, &p2);
                        if (p2 == save) break;
                        if (first) { svgp_moveto(poly, (x-vbx)*sxx, (y-vby)*syy); first = 0; }
                        else svgp_lineto(poly, (x-vbx)*sxx, (y-vby)*syy);
                    }
                    svg_fill_poly(canvas, W, H, poly, fill);
                }
            } else if (!none && str_eq(nm, "path")) {
                if (svg_attr(s, k, tend, "d", dbuf, 8192)) {
                    svg_path_d(dbuf, (int)str_len(dbuf), poly, sxx, syy, vbx, vby);
                    svg_fill_poly(canvas, W, H, poly, fill);
                }
            }
        }
        i = tend + 1;
    }

    free(poly);
    free(dbuf);
    *out_w = W;
    *out_h = H;
    return canvas;
}

static uint8_t *g_img_fetch_buf = NULL;

/* Stage 12D: one image transfer in flight at a time, fully async --
 * start it on one idle pass, poll it on the next ones, decode when
 * DONE. Nothing blocks; image state 2 = "in flight". */
static int g_imgf_handle = -1;
static int g_imgf_ti = -1, g_imgf_ii = -1;

/* Cancel the in-flight image fetch and clear every in-flight marker
 * (called before tab indices shift, and when a tab's images go away). */
static void img_fetch_cancel(void) {
    if (g_imgf_handle >= 0) {
        sys_http_finish(g_imgf_handle);
        g_imgf_handle = -1;
    }
    for (int ti = 0; ti < g_ntabs; ti++)
        for (int ii = 0; ii < g_tabs[ti].nimages; ii++)
            if (g_tabs[ti].images[ii].state == 2)
                g_tabs[ti].images[ii].state = 0;
    g_imgf_ti = g_imgf_ii = -1;
}

static int load_one_pending_image(void) {
    /* An image is in flight: poll it. */
    if (g_imgf_handle >= 0) {
        struct http_poll pi;
        long pr = sys_http_poll(g_imgf_handle, &pi);
        if (pr == 0 &&
            (pi.state == HTTPA_QUEUED || pi.state == HTTPA_RUNNING))
            return 0;                       /* still transferring */

        int ti = g_imgf_ti, ii = g_imgf_ii;
        long n = -1;
        if (pr == 0 && pi.state == HTTPA_DONE) {
            if (!g_img_fetch_buf)
                g_img_fetch_buf = (uint8_t *)malloc(IMG_FETCH_CAP);
            if (g_img_fetch_buf && pi.body_len > 0)
                n = sys_http_read(g_imgf_handle, g_img_fetch_buf, 0,
                                  IMG_FETCH_CAP);
        }
        sys_http_finish(g_imgf_handle);
        g_imgf_handle = -1;
        g_imgf_ti = g_imgf_ii = -1;

        /* The tab may have navigated away or closed since the start. */
        if (ti < 0 || ti >= g_ntabs || ii >= g_tabs[ti].nimages ||
            g_tabs[ti].images[ii].state != 2)
            return 1;
        struct img *im = &g_tabs[ti].images[ii];
        if (n <= 0) { im->state = -1; return 1; }

        uint32_t *pix = NULL;
        int iw = 0, ih = 0;
        toby_image_t *dec = toby_image_load(g_img_fetch_buf, (size_t)n);
        if (dec && dec->width > 0 && dec->height > 0 &&
            dec->width <= IMG_MAX_DIM && dec->height <= IMG_MAX_DIM) {
            pix = dec->pixels;              /* steal the decoded buffer */
            iw = dec->width;
            ih = dec->height;
            dec->pixels = NULL;
        }
        if (dec) toby_image_free(dec);
        if (!pix && svg_sniff(g_img_fetch_buf, n))   /* stage 12E: icons */
            pix = svg_render(g_img_fetch_buf, n, &iw, &ih);
        if (!pix) {
            im->state = -1;
            return 1;
        }
        im->pixels = pix;
        im->w = (int16_t)iw;
        im->h = (int16_t)ih;
        im->state = 1;

        /* Re-layout the affected tab. The layout shim addresses `cur`,
         * so temporarily point it at the affected tab (safe: single-
         * threaded, no events run here), restore + repaint. */
        int save = g_active;
        g_active = ti;
        layout(g_win_w);
        if (ti == save) clamp_scroll();
        g_active = save;
        tk_redraw(&win);
        return 1;
    }

    /* Nothing in flight: start the next pending image (active tab
     * first). Starting is instant -- the kernel worker transfers it. */
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

    struct img *im = &g_tabs[ti_sel].images[ii_sel];
    long h = sys_http_start(im->src, IMG_FETCH_CAP);
    if (h < 0) return 0;                    /* slot table busy: retry later */
    im->state = 2;
    g_imgf_handle = (int)h;
    g_imgf_ti = ti_sel;
    g_imgf_ii = ii_sel;
    return 0;
}

/* ===================== Media playback (stage 13) ====================== *
 * <video> = MJPEG-in-AVI: demux the RIFF container once, then decode
 * one JPEG frame per due tick (stb JPEG via toby_image_load) into the
 * element's ARGB backing store and repaint -- no relayout. <audio> =
 * MP3 via the real minimp3 (libtoby), decoded incrementally into the
 * kernel audio engine's PCM16 stream, throttled by the ring. The file
 * downloads async exactly like images (one transfer in flight). */

static int g_medf_handle = -1;
static int g_medf_ti = -1, g_medf_ii = -1;

static void media_fetch_cancel(void) {
    if (g_medf_handle >= 0) {
        sys_http_finish(g_medf_handle);
        g_medf_handle = -1;
    }
    for (int ti = 0; ti < g_ntabs; ti++)
        for (int k = 0; k < g_tabs[ti].nmedia; k++)
            if (g_tabs[ti].media[k].used && g_tabs[ti].media[k].state == 2)
                g_tabs[ti].media[k].state = 0;
    g_medf_ti = g_medf_ii = -1;
}

static int m4(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Index the '##dc'/'##db' video chunks of an MJPEG AVI and read the
 * frame period from avih. Returns 0 with >= 1 frame, else -1. */
static int media_demux_avi(struct media_el *m) {
    const uint8_t *d = m->data;
    long n = m->dlen;
    if (n < 12 || !m4(d, "RIFF") || !m4(d + 8, "AVI ")) return -1;
    m->frame_ms = 100;
    m->nframes = 0;
    long off = 12;
    while (off + 8 <= n) {
        const uint8_t *c = d + off;
        uint32_t sz = rd32le(c + 4);
        if ((long)sz > n - off - 8) sz = (uint32_t)(n - off - 8);
        if (m4(c, "LIST") && off + 12 <= n) {
            const uint8_t *lt = c + 8;
            if (m4(lt, "hdrl")) {
                const uint8_t *a = c + 12;   /* avih is the first subchunk */
                if (a + 16 <= d + n && m4(a, "avih")) {
                    uint32_t uspf = rd32le(a + 8);
                    if (uspf >= 1000 && uspf <= 10000000)
                        m->frame_ms = (long)(uspf / 1000);
                }
            } else if (m4(lt, "movi")) {
                long p = off + 12, end = off + 8 + (long)sz;
                if (end > n) end = n;
                while (p + 8 <= end && m->nframes < MEDIA_FRAMES_MAX) {
                    const uint8_t *k = d + p;
                    uint32_t ksz = rd32le(k + 4);
                    if ((long)ksz > end - p - 8) break;
                    if (k[2] == 'd' && (k[3] == 'c' || k[3] == 'b') && ksz > 0) {
                        m->frame_off[m->nframes] = p + 8;
                        m->frame_len[m->nframes] = (int)ksz;
                        m->nframes++;
                    }
                    p += 8 + (long)ksz + (ksz & 1);
                }
            }
        }
        off += 8 + (long)sz + (sz & 1);
    }
    return m->nframes > 0 ? 0 : -1;
}

/* MP3 sniff: ID3v2 tag or an MPEG audio sync word. */
static int media_sniff_mp3(const uint8_t *d, long n) {
    if (n < 4) return 0;
    if (d[0] == 'I' && d[1] == 'D' && d[2] == '3') return 1;
    return d[0] == 0xFF && (d[1] & 0xE0) == 0xE0;
}

/* Nearest-neighbour blit of a decoded frame into the backing store. */
static void media_blit(struct img *dst, toby_image_t *src) {
    if (!dst->pixels || !src->pixels || src->width <= 0 || src->height <= 0)
        return;
    for (int y = 0; y < dst->h; y++) {
        int sy = (int)((long)y * src->height / dst->h);
        uint32_t *drow = &dst->pixels[(long)y * dst->w];
        const uint32_t *srow = &src->pixels[(long)sy * src->width];
        for (int x = 0; x < dst->w; x++)
            drow[x] = srow[(long)x * src->width / dst->w] | 0xFF000000u;
    }
}

static void med_log(const char *what, int a, int b) {
    char m[96];
    int p = msg_append(m, 0, sizeof(m), "[med] ");
    p = msg_append(m, p, sizeof(m), what);
    p = msg_append(m, p, sizeof(m), " ");
    p = msg_append_int(m, p, sizeof(m), a);
    p = msg_append(m, p, sizeof(m), " ");
    p = msg_append_int(m, p, sizeof(m), b);
    p = msg_append(m, p, sizeof(m), "\n");
    sys_write(1, m, (unsigned long)p);
}

/* Fetch completion: sniff the container and ready the element. */
static void media_ready(struct media_el *m) {
    if (m->is_video && media_demux_avi(m) == 0) {
        med_log("video frames/period", m->nframes, (int)m->frame_ms);
        m->state = 1;
    } else if (!m->is_video && media_sniff_mp3(m->data, m->dlen)) {
        m->mp3 = toby_mp3_decode_init();
        if (!m->mp3) { m->state = -1; return; }
        med_log("audio bytes", (int)m->dlen, 0);
        m->apos = 0;
        m->state = 1;
    } else {
        med_log("unsupported container", (int)m->dlen, m->is_video);
        m->state = -1;
        return;
    }
    if (m->autoplay) {
        m->playing = 1;
        m->next_ms = js_now_ms();
    }
}

/* One async media transfer at a time (the image-loader pattern). */
static int load_one_pending_media(void) {
    if (g_medf_handle >= 0) {
        struct http_poll pi;
        long pr = sys_http_poll(g_medf_handle, &pi);
        if (pr == 0 &&
            (pi.state == HTTPA_QUEUED || pi.state == HTTPA_RUNNING))
            return 0;
        int ti = g_medf_ti, ii = g_medf_ii;
        uint8_t *buf = NULL;
        long n = -1;
        if (pr == 0 && pi.state == HTTPA_DONE && pi.body_len > 0) {
            buf = (uint8_t *)malloc(pi.body_len);
            if (buf)
                n = sys_http_read(g_medf_handle, buf, 0, pi.body_len);
        }
        sys_http_finish(g_medf_handle);
        g_medf_handle = -1;
        g_medf_ti = g_medf_ii = -1;

        if (ti < 0 || ti >= g_ntabs || ii >= g_tabs[ti].nmedia ||
            g_tabs[ti].media[ii].state != 2) {
            if (buf) free(buf);
            return 1;
        }
        struct media_el *m = &g_tabs[ti].media[ii];
        if (n <= 0) {
            if (buf) free(buf);
            m->state = -1;
            return 1;
        }
        m->data = buf;
        m->dlen = n;
        media_ready(m);
        return 1;
    }

    int ti_sel = -1, ii_sel = -1;
    for (int pass = 0; pass < 2 && ti_sel < 0; pass++) {
        for (int ti = 0; ti < g_ntabs; ti++) {
            if ((pass == 0) != (ti == g_active)) continue;   /* active first */
            struct tab *t = &g_tabs[ti];
            for (int k = 0; k < t->nmedia; k++)
                if (t->media[k].used && t->media[k].state == 0 &&
                    t->media[k].src[0]) {
                    ti_sel = ti; ii_sel = k; break;
                }
            if (ti_sel >= 0) break;
        }
    }
    if (ti_sel < 0) return 0;

    struct media_el *m = &g_tabs[ti_sel].media[ii_sel];
    long h = sys_http_start(m->src, MEDIA_FETCH_CAP);
    if (h < 0) return 0;                    /* slot table busy: retry later */
    m->state = 2;
    g_medf_handle = (int)h;
    g_medf_ti = ti_sel;
    g_medf_ii = ii_sel;
    return 0;
}

/* Keep the audio-engine ring fed: write leftover PCM first, then
 * decode more MP3 frames until the ring pushes back. */
static int media_audio_pump(struct media_el *m) {
    if (!m->mp3) return 0;
    int work = 0;
    for (int guard = 0; guard < 24; guard++) {
        if (m->carry_len > 0) {
            if (m->audio_fd < 0) break;
            long w = sys_audio_write(m->audio_fd,
                                     (const uint8_t *)m->carry + m->carry_off,
                                     (unsigned long)m->carry_len);
            if (w <= 0) break;              /* ring full: next pass */
            m->carry_off += (int)w;
            m->carry_len -= (int)w;
            work = 1;
            if (m->carry_len > 0) break;
            continue;
        }
        if (m->apos >= m->dlen) break;
        int samples = 0, ch = 0, rate = 0;
        int fb = toby_mp3_decode_frame((toby_mp3_decoder_t *)m->mp3,
                                       m->data + m->apos,
                                       (size_t)(m->dlen - m->apos),
                                       m->carry, &samples, &ch, &rate);
        if (fb <= 0) { m->apos = m->dlen; break; }
        m->apos += fb;
        if (samples > 0 && ch > 0 && ch <= 2) {
            if (m->audio_fd < 0) {
                m->audio_fd = (int)sys_audio_open((uint32_t)rate, ch,
                                                  AUDIO_FMT_PCM16);
                med_log("audio stream fd/rate",
                        m->audio_fd, rate);
                if (m->audio_fd < 0) { m->playing = 0; return work; }
            }
            m->carry_off = 0;
            m->carry_len = samples * ch * 2;
            work = 1;
        }
    }
    if (m->apos >= m->dlen && m->carry_len == 0) {
        if (m->loop) m->apos = 0;
        else m->playing = 0;
    }
    return work;
}

/* Main-loop tick: advance the ACTIVE tab's playing media. */
static int media_pump(void) {
    int work = 0;
    int repaint = 0;
    long now = js_now_ms();
    for (int k = 0; k < cur->nmedia; k++) {
        struct media_el *m = &cur->media[k];
        if (!m->used || m->state != 1 || !m->playing) continue;
        if (m->is_video) {
            if (m->nframes <= 0 || now < m->next_ms) continue;
            struct img *im = (m->img >= 0 && m->img < g_nimages)
                                 ? &g_images[m->img] : NULL;
            if (im && im->pixels) {
                toby_image_t *f = toby_image_load(
                    m->data + m->frame_off[m->cur_frame],
                    (size_t)m->frame_len[m->cur_frame]);
                if (f) {
                    media_blit(im, f);
                    toby_image_free(f);
                    repaint = 1;
                }
            }
            m->cur_frame++;
            if (m->cur_frame >= m->nframes) {
                if (m->loop) m->cur_frame = 0;
                else m->playing = 0;
            }
            long due = m->next_ms + m->frame_ms;
            m->next_ms = (due > now - 4 * m->frame_ms) ? due
                                                       : now + m->frame_ms;
            work = 1;
        } else {
            work |= media_audio_pump(m);
        }
    }
    if (repaint) tk_redraw(&win);
    return work;
}

/* ---- Stage 12D: async page navigation ----------------------------- *
 * nav_begin starts a kernel-side transfer for the CURRENT tab (cur)
 * and returns immediately; nav_pump (main loop) polls every tab's
 * handle and runs the completion continuation (NAVK_*). The UI keeps
 * pumping events, timers and paints the whole time. */

/* Terminal failure: error page (+ optional history entry). */
static void nav_finish_error(const char *url, int err, int push) {
    str_copy(g_url, url, URL_MAX);
    g_url_len = (int)str_len(g_url);
    show_error_page(url, err);
    if (push) history_push(url);
    g_focus_url = 0;
}

static void nav_begin(int kind, const char *url) {
    if (cur->nav_handle >= 0) {              /* replaces an in-flight nav */
        sys_http_finish(cur->nav_handle);
        cur->nav_handle = -1;
    }
    str_copy(cur->nav_url, url, URL_MAX);
    cur->nav_kind = kind;
    long h = sys_http_start(cur->nav_url, RAW_CAP);
    if (h < 0) {                             /* handle table exhausted */
        g_loading = 0;
        nav_finish_error(cur->nav_url, -8 /* out of memory */,
                         kind != NAVK_PLAIN);
        return;
    }
    cur->nav_handle = (int)h;
    g_loading = 1;
    set_status("Loading...");
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

/* Fetch + render, error page on failure. Back/Forward/Refresh path.
 * Async: kicks off the transfer; nav_pump finishes the job. */
static long do_fetch_url(const char *url) {
    nav_begin(NAVK_PLAIN, url);
    return 0;
}

/* Navigate to a fully-formed URL (links, search, explicit schemes).
 * Async: the address bar shows the target immediately, the page swaps
 * in when the transfer completes. */
static long do_navigate(const char *url) {
    char target[URL_MAX + 1];
    str_copy(target, url, URL_MAX);

    str_copy(g_url, target, URL_MAX);
    g_url_len = (int)str_len(g_url);

    nav_begin(NAVK_NORMAL, target);
    return 0;
}

/* Focus handoff between form fields, with JS focus/blur events on the
 * fields' DOM nodes (stage 12E). */
static void set_focus_field(int fi) {
    if (fi == g_focus_field) return;
    int old = g_focus_field;
    g_focus_field = fi;
    if (!cur->js_cx) return;
    if (old >= 0 && old < g_nfields && g_fields[old].node >= 0)
        js_dispatch_event(g_fields[old].node, "blur");
    if (fi >= 0 && fi < g_nfields && g_fields[fi].node >= 0)
        js_dispatch_event(g_fields[fi].node, "focus");
}

/* Traverse history by delta. Entries created by pushState belong to
 * the LIVE document: moving between same-generation entries fires a
 * popstate event and updates the bar -- no refetch (SPA routers).
 * Anything else refetches through the async nav path. */
static void history_go(int delta) {
    int npos = g_hist_pos + delta;
    if (npos < 0 || npos >= g_hist_count) return;
    g_hist_pos = npos;
    str_copy(g_url, g_history[npos], URL_MAX);
    g_url_len = (int)str_len(g_url);
    if (cur->hist_gen[npos] == cur->doc_gen && cur->js_cx) {
        js_dispatch_event(-1, "popstate");
        update_title();
        tk_redraw(&win);
        return;
    }
    do_fetch_url(g_url);
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
 * which serves plain HTML without a challenge wall. Async: the
 * fallback chains from the pump (NAVK_DDG). */
static void navigate_search(const char *query) {
    char surl[URL_MAX + 1];
    str_copy(cur->nav_input, query, sizeof(cur->nav_input));
    build_search_url("https://html.duckduckgo.com/html/?q=", query,
                     surl, URL_MAX);
    str_copy(g_url, surl, URL_MAX);
    g_url_len = (int)str_len(g_url);
    nav_begin(NAVK_DDG, surl);
}

/* Search fallback leg: Mojeek serves challenge-free plain HTML. */
static void nav_search_mojeek(void) {
    char surl[URL_MAX + 1];
    set_status("Search challenged - trying Mojeek...");
    build_search_url("https://www.mojeek.com/search?q=", cur->nav_input,
                     surl, URL_MAX);
    str_copy(g_url, surl, URL_MAX);
    g_url_len = (int)str_len(g_url);
    nav_begin(NAVK_NORMAL, surl);
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

/* The omnibox: what runs when Enter is pressed in the address bar.
 *   - explicit scheme        -> use as-is
 *   - non-URL text           -> DuckDuckGo HTML search
 *   - bare host[/path]       -> https:// first, http:// fallback,
 *                               www. retry when DNS says no such host
 * Async: the fallback ladder chains from the pump (NAVK_HOST*). */
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
    str_copy(cur->nav_input, input, sizeof(cur->nav_input));
    str_concat2(cand, URL_MAX, "https://", input);
    str_copy(g_url, cand, URL_MAX);
    g_url_len = (int)str_len(g_url);
    nav_begin(NAVK_HOST0, cand);
}

/* ---- Stage 12D: nav completion + the poll pump -------------------- */

/* The transfer for the CURRENT tab (cur) finished cleanly: pull the
 * body into g_raw, then run the continuation for the nav kind. */
static void nav_complete_done(const struct http_poll *pi) {
    long n = 0;
    if (pi->body_len > 0)
        n = sys_http_read(cur->nav_handle, g_raw, 0, RAW_CAP);
    sys_http_finish(cur->nav_handle);
    cur->nav_handle = -1;
    g_loading = 0;
    if (n < 0) n = 0;

    g_last_status = pi->status;
    g_raw_len = n;
    g_raw[g_raw_len] = '\0';
    if (pi->final_url[0]) {
        str_copy(g_url, pi->final_url, URL_MAX);
        g_url_len = (int)str_len(g_url);
    }

    int kind = cur->nav_kind;
    if (kind == NAVK_DDG && g_last_status == 202) {
        nav_search_mojeek();               /* bot wall: chain the fallback */
        return;
    }
    render_fetched();
    if (kind != NAVK_PLAIN) {
        history_push(g_url);               /* the post-redirect URL */
        g_focus_url = 0;
    }
}

/* The transfer for the CURRENT tab failed: run the retry ladder the
 * old synchronous chains used, or land on the error page. */
static void nav_complete_error(int err) {
    sys_http_finish(cur->nav_handle);
    cur->nav_handle = -1;
    g_loading = 0;

    char inp[256];
    str_copy(inp, cur->nav_input, sizeof(inp));
    char urlbuf[URL_MAX + 1];
    str_copy(urlbuf, cur->nav_url, URL_MAX);
    char next[URL_MAX + 1];

    /* stage 13H: a TLS certificate failure is definitive -- the server is
     * there and speaking TLS, its cert is just untrusted. Never downgrade
     * to plain http or walk the www ladder; show the security page now. */
    if (err == HTTPE_CERT) {
        nav_finish_error(urlbuf, err, 1);
        return;
    }

    switch (cur->nav_kind) {
    case NAVK_DDG:
        nav_search_mojeek();
        return;
    case NAVK_HOST0:
        if (err == HTTPE_DNS) {
            /* Host didn't resolve: retry with a www. prefix. */
            if (!(inp[0] == 'w' && inp[1] == 'w' && inp[2] == 'w' &&
                  inp[3] == '.')) {
                str_concat2(next, URL_MAX, "https://www.", inp);
                str_copy(g_url, next, URL_MAX);
                g_url_len = (int)str_len(g_url);
                nav_begin(NAVK_HOST1, next);
                return;
            }
        } else {
            /* Resolved but HTTPS didn't answer: plain HTTP. */
            str_concat2(next, URL_MAX, "http://", inp);
            nav_begin(NAVK_HOST3, next);
            return;
        }
        break;
    case NAVK_HOST1:
        if (err != HTTPE_DNS) {            /* www. resolved: try http */
            str_concat2(next, URL_MAX, "http://www.", inp);
            nav_begin(NAVK_HOST2, next);
            return;
        }
        break;
    case NAVK_PLAIN:
        show_error_page(urlbuf, err);      /* no history entry */
        return;
    default:
        break;
    }
    nav_finish_error(urlbuf, err, 1);
}

/* Poll every tab's in-flight navigation; complete the finished ones.
 * Runs with cur temporarily pointed at each loading tab (the whole
 * g_* shim addresses the active tab). */
static int nav_pump(void) {
    int work = 0;
    for (int ti = 0; ti < g_ntabs; ti++) {
        if (g_tabs[ti].nav_handle < 0) continue;
        int save = g_active;
        g_active = ti;
        struct http_poll pi;
        long pr = sys_http_poll(cur->nav_handle, &pi);
        if (pr < 0) {                      /* lost handle: connection err */
            cur->nav_handle = -1;
            g_loading = 0;
            show_error_page(cur->nav_url, -9);
            work = 1;
        } else if (pi.state == HTTPA_DONE) {
            nav_complete_done(&pi);
            work = 1;
        } else if (pi.state == HTTPA_ERROR) {
            nav_complete_error(pi.err);
            work = 1;
        }
        g_active = save;
        if (work) {
            update_title();
            tk_redraw(&win);
        }
    }
    return work;
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
            int sy;
            if (r->fl & IF_FIXED) {         /* viewport coords, no scroll */
                if (r->y + r->h <= 0 || r->y >= g_content_h) continue;
                sy = CONTENT_TOP + r->y;
            } else {
                if (r->y + r->h <= vtop || r->y >= vbot) continue;
                sy = CONTENT_TOP + (r->y - vtop);
            }

            /* effective clip = viewport intersected with any overflow
             * clip ancestor (stage 13D), translated to screen coords. */
            int cx0 = 0, cx1 = g_win_w, cyy0 = vy0, cyy1 = vy1;
            if (r->clip) {
                struct cliprect *cl = &g_clips[r->clip];
                int csy = (r->fl & IF_FIXED) ? (CONTENT_TOP + cl->y)
                                             : (CONTENT_TOP + cl->y - vtop);
                if (cl->x > cx0) cx0 = cl->x;
                if (cl->x + cl->w < cx1) cx1 = cl->x + cl->w;
                if (csy > cyy0) cyy0 = csy;
                if (csy + cl->h < cyy1) cyy1 = csy + cl->h;
            }
            /* cull anything fully outside the effective clip */
            if (r->x >= cx1 || r->x + r->w <= cx0 ||
                sy >= cyy1 || sy + r->h <= cyy0) continue;

            if (r->kind == DI_RECT) {
                int x0 = r->x, w = r->w, y0 = sy, h = r->h;
                if (x0 < cx0) { w -= cx0 - x0; x0 = cx0; }
                if (x0 + w > cx1) w = cx1 - x0;
                if (y0 < cyy0) { h -= cyy0 - y0; y0 = cyy0; }
                if (y0 + h > cyy1) h = cyy1 - y0;
                if (h > 0 && w > 0)
                    sys_gui_fill(fd, x0, y0, w, h, r->fg & 0xFFFFFF);
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
            /* draw with the run's resolved face (web font or bold);
             * win.fd, not the paint fd (0) the *_fill stubs ignore */
            sys_text_ttf(win.fd, r->x, ty, tb, fg & 0xFFFFFF, r->px, r->face);
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
    int vy = my - CONTENT_TOP;             /* fixed items: viewport coords */
    for (int ri = 0; ri < g_nitems; ri++) {
        struct ditem *r = &g_items[ri];
        if (r->link < 0 && r->field < 0) continue;
        if (r->kind != DI_TEXT && r->kind != DI_IMG && r->kind != DI_FIELD)
            continue;
        int iy = (r->fl & IF_FIXED) ? vy : dy;
        if (iy >= r->y && iy < r->y + r->h &&
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
                history_go(-1);
                set_status("Back");
            }
        } else if (mx < 36) {
            /* Forward */
            if (can_go_forward()) {
                history_go(1);
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
            int vy2 = my - CONTENT_TOP;    /* fixed items: viewport coords */
            int evn = -1;
            for (int ri = 0; ri < g_nitems; ri++) {
                struct ditem *r2 = &g_items[ri];
                if (r2->node < 0) continue;
                int iy2 = (r2->fl & IF_FIXED) ? vy2 : dy2;
                if (iy2 >= r2->y && iy2 < r2->y + r2->h &&
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
                set_focus_field(hit->field);
                set_status("Type into the field; Enter submits");
            } else {
                set_focus_field(-1);
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
        set_focus_field(-1);
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
            int prevented = kn[0] ? js_dispatch_key(f->node, "keydown", kn) : 0;
            /* keyup is synthesized right after keydown: the key ABI
             * has no break codes (stage 12E) */
            if (kn[0]) js_dispatch_key(f->node, "keyup", kn);
            if (prevented) {
                js_do_pending_nav();
                redraw(0);
                return;
            }
        }
        if (key == 27) {
            set_focus_field(-1);
            set_status("Field unfocused");
            redraw(0);
            return;
        }
        if (key == 9) {
            set_focus_field(-1);             /* fall through to Tab */
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
                int prevented = kn[0] ? js_dispatch_key(-1, "keydown", kn) : 0;
                if (kn[0]) js_dispatch_key(-1, "keyup", kn);   /* 12E */
                if (prevented) {
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
                    history_go(-1);
                    set_status("Back");
                } else {
                    set_status("No previous page");
                }
                break;
            case ']':
                if (can_go_forward()) {
                    history_go(1);
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
                set_focus_field(found);
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
        int work = nav_pump();             /* async page loads (12D) */
        work |= load_one_pending_image();
        work |= load_one_pending_media();  /* <video>/<audio> files */
        work |= media_pump();              /* frame + PCM advance */
        work |= js_pump_all();             /* timers + jobs + fetches */
        if (!work)
            sys_sleep_ms(15);              /* nothing pending -> idle */
    }
    return 0;
}
