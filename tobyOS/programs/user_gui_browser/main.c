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

/* Local mirror of the kernel struct gui_event -- keep in step with
 * include/tobyos/gui.h AND toby/tk.h (the wheel field replaced one of
 * the two old pad bytes; size is unchanged). */
struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    signed char wheel;   /* int8_t is not in this program's type set */
    uint8_t _pad[1];
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
#include <toby/video_decode.h>  /* real openh264 H.264 decode (stage 13 media) */
/* QuickJS (third_party/quickjs, linked by the Makefile). Included this
 * early so its headers see none of the short macros defined below
 * (E, cur, g_raw, ...). Its inline helpers trip -Wextra; scoped off. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#include "quickjs.h"
#pragma clang diagnostic pop

/* wasm3 WebAssembly engine (third_party/wasm3, linked via libtoby). The
 * public API only; import metadata comes through toby/wasm_bridge.h so
 * wasm3's private u8/u32 typedefs never enter this TU. */
#include "wasm3.h"
#include "toby/wasm_bridge.h"

/* Color emoji (COLR/CPAL via libtoby). */
#include "toby/emoji.h"

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
/* 2026-08-24: this file draws a CHARACTER GRID at CELL_W=8 columns, and
 * tk_draw_text_mono() finally delivers one -- it used to route to the
 * proportional TrueType renderer, so the columns never lined up and the
 * opaque background was dropped. The fixed cell is 8x16, taller than this
 * file's 12px row pitch, so go through tk_draw_text_cell(): the glyph is
 * centred in the pitch and clipped, and the background covers exactly that
 * many rows instead of bleeding into the line below.
 *
 * The pitch is spelled here rather than as CELL_H because the drawing
 * forwarders below sit ~400 lines ABOVE that #define; the static assert
 * keeps the two from drifting apart. */
#define MONO_PITCH 12
#define TK_MONO(w,x,y,s,fg,bg) tk_draw_text_cell((w),(x),(y),(s),(fg),(bg),MONO_PITCH)

/* Drawing now forwards to TobyTK. The content grid is monospace (CELL_W=8), so
 * text uses tk_draw_text_mono (column-aligned, opaque bg). `fd` is ignored. */
static inline int sys_gui_fill(int fd, int x, int y, int w, int h, uint32_t color) {
    (void)fd; tk_draw_fill(&win, x, y, w, h, color); return 0;
}
static inline int sys_gui_text(int fd, int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    (void)fd; TK_MONO(&win, x, y, s, fg, bg); return 0;
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

/* fetch()/XHR: async request with method/headers/body (ABI 183/184). */
#define SYS_HTTP_START2 183
#define SYS_HTTP_HDRS   184
#define HTTP_F_NO_COOKIES 0x10u
struct http_start2 {
    unsigned long url, method, headers, body;
    uint32_t body_len, max_body, flags, reserved;
};
static inline long sys_http_start2(struct http_start2 *rq) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_START2), "D"(rq)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_http_hdrs(long h, void *buf, uint32_t off, uint32_t len) {
    unsigned long off_len = ((unsigned long)off << 32) | (unsigned long)len;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_HTTP_HDRS), "D"(h), "S"(buf), "d"(off_len)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- TTF -> coverage raster (css-anim slice 3 compositing) -------- *
 * The kernel rasterizes a text run as 0..255 coverage bytes into a
 * user buffer (same glyph cache as on-window TTF text, so layer text
 * is pixel-identical). Struct mirrors abi_ttf_raster (ABI-frozen). */
#define SYS_GUI_TEXT_TTF_RASTER 182
struct ttf_raster_rq {
    unsigned long cov;
    int32_t w, h;
    unsigned long s;
    int32_t len, x, y, px, face;
};
static inline long sys_ttf_raster(uint8_t *cov, int w, int h, int x, int y,
                                  const char *s, int len, int px, int face) {
    struct ttf_raster_rq rq;
    rq.cov = (unsigned long)cov; rq.w = w; rq.h = h;
    rq.s = (unsigned long)s; rq.len = len; rq.x = x; rq.y = y;
    rq.px = px; rq.face = face;
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT_TTF_RASTER), "D"(&rq)
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

/* 1.5 MiB of page HTML: covers heavy modern SPAs (e.g. youtube.com is
 * ~777 KiB of mostly-inline JS) without truncating a <script> mid-string
 * (which yields spurious SyntaxErrors). raw[] lives inline in struct tab
 * -> TAB_MAX * 1.5M = 9 MiB BSS; the eng pools below scale with it. */
#define RAW_CAP       (1536 * 1024)

/* Visible-text pool: must exceed RAW_CAP so view-source of a max-size
 * page keeps every character (source view flows raw through a pre). */
#define RENDER_CAP    (1600 * 1024)

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
    T_SLOT,
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
    int32_t  min_w, min_h;       /* px, -1 none (min-width/min-height) */
    int16_t  m[4], p[4];         /* margin/padding: T R B L */
    uint8_t  bw[4];              /* border widths:  T R B L */
    uint8_t  disp;               /* D_* */
    uint8_t  px;                 /* font size */
    uint8_t  fl;                 /* SF_* */
    uint8_t  radius;             /* border-radius px (0 = square corners) */
    int16_t  deco;               /* index into g_deco (gradient/shadow), -1 */
    int16_t  mask;               /* index into g_masks (mask-image), -1 none */
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
    /* transform scale/rotate + opacity (slice 3): painted through a
     * per-element compositing layer, not baked at layout. */
    uint16_t scx, scy;           /* scale, per-mille (1000 = 1x) */
    int16_t  rot;                /* rotate, degrees */
    uint8_t  opa;                /* opacity 0..255 (255 = opaque) */
    uint8_t  po_pct;             /* top/right/bottom/left are % (bits T R B L) */
    int8_t   webface;            /* @font-face kernel face id, -1 = none */
    /* CSS animations: @keyframes-driven overrides (not inherited). */
    int16_t  anim_kf;            /* @keyframes index, -1 = none */
    uint16_t anim_dur;           /* animation-duration ms (0 = none) */
    uint16_t anim_delay;         /* animation-delay ms */
    uint8_t  anim_iter;          /* iteration count, 0 = infinite */
    uint8_t  anim_flags;         /* bit0 alternate, bit1 ease-in-out */
    /* CSS transitions: interpolate on computed-value change (not inherited). */
    uint8_t  trans_mask;         /* bit0 color, bit1 bg, bit2 transform */
    uint8_t  trans_flags;        /* bit1 ease-in-out */
    uint16_t trans_dur;          /* transition-duration ms (0 = none) */
    uint16_t trans_delay;        /* transition-delay ms */
};

/* ---- DOM ---------------------------------------------------------- */

#define DNF_STYLE_DONE 1     /* <style> content already parsed into rules */
#define DNF_LAID_FIXED 2     /* laid rect is viewport coords (pos: fixed) */
#define DNF_LAID_BLOCK 4     /* laid rect came from a block stamp (exact
                                border box), not an item union */

struct dnode {
    int32_t parent, first, last, next;   /* tree links (node idx, -1) */
    int32_t shadow;              /* shadow root child idx, -1 (slice 2) */
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
    /* bloom of this node's + all ancestors' keys (tag/id/class);
     * set top-down in style_node, read by sel_match_rule's fast
     * reject to skip hopeless ancestor walks. */
    uint64_t bloom;
    /* cached class-attribute span in tpool: style_node fills it so the
     * matcher stops re-finding "class" (a linear node_attr scan) on
     * every candidate. cgen == g_cls_gen means it is valid this pass. */
    int32_t  cls_voff; int16_t cls_vlen; int32_t cgen;
    struct cstyle st;            /* computed by the style pass */
};
#define NODE_MAX  32768

struct dattr {                   /* name/value slices in tpool */
    int32_t noff, voff, vlen;
    int16_t nlen;
    int16_t _pad;
};
#define ATTR_MAX  32768
#define TPOOL_CAP (2048 * 1024)   /* text nodes incl. large inline <script> bodies */

/* ---- CSSOM -------------------------------------------------------- */

/* Classes stored per compound. Codex/BEM state selectors chain up to 5
 * (.cdx-button.cdx-button--fake-button--enabled.cdx-button--weight-primary
 *  .cdx-button--action-destructive.cdx-button--is-active); a compound
 * with MORE than the cap makes the whole selector invalid (drop the
 * rule) -- silently keeping a prefix over-matched: every state-variant
 * rule applied to every button (wikipedia's salmon boxes). */
#define PART_CLS_MAX 6
/* :not(<simple>) negations per compound. Real sheets chain them
 * (wikipedia: table:not(.infobox):not(.navbox-inner):not(.navbox));
 * github hides its whole mobile menu behind
 * `.header-logged-out:not(.open) .HeaderMenu`. Arguments beyond one
 * simple selector (combinators, commas, nested :not) fail closed. */
#define PART_NOT_MAX 4
#define NOT_CLASS 0
#define NOT_ID    1
#define NOT_TAG   2
#define NOT_ATTR  3              /* presence only: :not([attr]) */
struct cpart {                   /* one compound selector part */
    int16_t tag;                 /* T_* or -1 = any */
    uint8_t comb;                /* combinator to the LEFT: 0 none/first,
                                    1 descendant, 2 child */
    uint8_t nclass;
    uint8_t pseudo_link;         /* :link / :visited present */
    uint8_t has_attr;
    uint8_t nnot;
    uint8_t not_kind[PART_NOT_MAX];
    int32_t id_off;   int16_t id_len;
    int32_t cls_off[PART_CLS_MAX]; int16_t cls_len[PART_CLS_MAX];
    int32_t an_off;   int16_t an_len;    /* [name] / [name=value] */
    int32_t av_off;   int16_t av_len;    /* 0 len = presence test */
    int32_t not_off[PART_NOT_MAX];       /* csspool text (tag: T_* id) */
    int16_t not_len[PART_NOT_MAX];
};
/* Sized for real modern stylesheets: youtube.com ships a single 3.4 MiB
 * bundle with ~21k selectors / ~66k declarations / ~18.5k rule blocks.
 * These pools live in the heap-allocated per-tab struct eng. */
#define PART_MAX  65536

struct cdecl {
    uint8_t prop;                /* CP_* (CP_VAR for --custom-property) */
    uint8_t imp;                 /* !important */
    int16_t vlen;
    int32_t voff;                /* raw value text in csspool */
    int32_t noff;                /* CP_VAR: --name text in csspool */
    int16_t nlen;                /* CP_VAR: name length */
    int16_t _pad;
};
#define DECL_MAX  131072

struct crule {
    int32_t part0; int16_t nparts;
    int16_t origin;              /* 0 UA, 1 author */
    int32_t decl0; int16_t ndecl;
    uint16_t spec;               /* specificity: 100*id + 10*(cls/attr) + type */
    int32_t order;               /* source order for cascade ties */
    int32_t scope;               /* shadow root node the sheet lives in,
                                    -1 = document (shadow-dom arc) */
    /* bloom bits every NON-rightmost part demands: if a node's
     * parent bloom lacks any, no ancestor can match. 0 = no usable
     * key (e.g. '*'), so no reject is possible. */
    uint64_t anc_bloom;
};
#define RULE_MAX    32768
#define CSSPOOL_CAP (4096 * 1024)
#define RIDX_BUCKETS 1024            /* cascade rule index; power of two */
#define RIDX_MIN_RULES 256           /* below this the linear scan is faster */

/* Shadow-DOM style scoping: the shadow-root node whose subtree we are
 * currently collecting (sheet parse) / styling (rule match); -1 =
 * document scope. Maintained as a stack via save/restore around the
 * tree recursions. */
static int g_css_scope = -1;      /* collect-time: stamps crule.scope */
static int g_style_scope = -1;    /* style-time: gates rule matching  */

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
    CP_WIDTH, CP_HEIGHT, CP_MAXW, CP_MINW, CP_MINH, CP_VISIBILITY,
    CP_FLOAT, CP_CLEAR,
    CP_VALIGN, CP_BCOLLAPSE,
    CP_FLEXDIR, CP_FLEXWRAP, CP_FLEXFLOW, CP_JUSTC, CP_ALITEMS,
    CP_FLEX, CP_FGROW, CP_FSHRINK, CP_FBASIS, CP_GAP,
    CP_POSITION, CP_TOP, CP_RIGHT, CP_BOTTOM, CP_LEFT,
    CP_VAR,                      /* --custom-property (resolved via var()) */
    CP_GRID_TC, CP_GRID_TR, CP_GRID_COL, CP_GRID_ROW,
    CP_OVERFLOW, CP_OVERFLOWX, CP_OVERFLOWY, CP_TRANSFORM,
    CP_ANIMATION, CP_ANIM_NAME, CP_ANIM_DUR, CP_ANIM_ITER,
    CP_ANIM_DELAY, CP_ANIM_DIR, CP_ANIM_TIMING,
    CP_TRANSITION, CP_TRANS_PROP, CP_TRANS_DUR, CP_TRANS_DELAY, CP_TRANS_TIMING,
    CP_OPACITY,
    CP_BORDER_RADIUS, CP_BOX_SHADOW,
    CP_MASK,                     /* mask-image / -webkit-mask-image url() */
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
    uint8_t radius;              /* rect corner radius px (decoration) */
    int16_t deco;                /* g_deco index (gradient/shadow), -1 */
    int16_t mask;                /* g_masks index (mask-image), -1 none */
};
#define DI_SHADOW 6              /* box-shadow (soft rounded rect behind) */
#define DI_EMOJI  7              /* color emoji glyph (off = codepoint) */

/* Per-node CSS decoration (border-radius already lives in cstyle/ditem;
 * this holds the bulkier gradient + box-shadow params, referenced by an
 * index so cstyle stays lean). Pool reset each style pass. */
#define GRAD_STOPS 8
struct deco {
    uint8_t  has_grad;
    uint8_t  grad_dir;           /* 0 to-bottom,1 to-right,2 to-top,3 to-left,
                                    4 radial, 5 conic */
    uint8_t  grad_n;             /* number of stops (2..GRAD_STOPS) */
    uint32_t grad_col[GRAD_STOPS];
    uint8_t  grad_pos[GRAD_STOPS];  /* 0..100 percent along the axis / turn */
    uint8_t  has_shadow;
    int16_t  sh_dx, sh_dy, sh_spread;
    uint8_t  sh_blur;
    uint32_t sh_col;             /* ARGB (alpha = shadow strength) */
    uint8_t  has_r4;             /* per-corner border-radius present */
    uint8_t  r4[4];              /* radius px: TL, TR, BR, BL (255 = 50%) */
};
#define DECO_MAX 512
static struct deco g_deco[DECO_MAX];
static int g_ndeco;

/* mask-image registry (CSS masks): interned by URL and shared across
 * every element that references the same icon, so each unique mask
 * fetches+decodes once. The decoded alpha is the stencil; the element's
 * own color fills it. Reset per page. */
#define MASK_MAX 128
struct maskimg {
    char     url[512];
    uint8_t *alpha;              /* w*h coverage, 0..255; NULL until loaded */
    int16_t  w, h;
    int8_t   state;              /* 0 pending, 1 loaded, -1 failed, 2 in flight */
};
static struct maskimg g_masks[MASK_MAX];
static int g_nmasks;
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
    /* cascade rule index: rule chains bucketed by the rightmost
     * compound selector's key (see rule_index_build). idx_nrules is the
     * rule count the index was built for; -1 forces a rebuild. */
    int32_t idx_id[RIDX_BUCKETS];
    int32_t idx_cls[RIDX_BUCKETS];
    int32_t idx_tag[T_NTAGS];
    int32_t idx_uni;
    int32_t idx_next[RULE_MAX];
    int     idx_nrules;
    int    css_order;               /* running decl-order counter */
    int    nsheets;                 /* fetched <link> sheets so far */
    /* display list + collapsed visible text (find-in-page) */
    struct ditem items[ITEM_MAX];   int nitems;
    char   render[RENDER_CAP + 1];  int render_len;
    uint32_t page_bg;               /* body/html background */
    int    lay_gen;                 /* bumped per layout(); validates
                                       the nodes' laid rects (13F) */
};

/* Real pages blow well past 128 (wikipedia saturated the old cap dead-on
 * "Done - 128 links"; hn fronts ~200): links past the cap were both
 * unstyled (a:link never matched) and UNCLICKABLE. 512x256B x tabs is
 * cheap. */
#define LINK_MAX       512
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
#define MEDIA_PCM_CARRY  (2048 * 2)   /* int16s: one MP3/AAC frame, stereo
                                       * (AAC-LC 1024, SBR/HE-AAC 2048) */

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
    /* video: AVI frame index (MJPEG or H.264 Annex-B chunks) */
    long     frame_off[MEDIA_FRAMES_MAX];
    int      frame_len[MEDIA_FRAMES_MAX];
    int      frame_pts[MEDIA_FRAMES_MAX];  /* presentation time (ms) from stts */
    int      nframes, cur_frame;
    long     frame_ms, next_ms;
    long     play_start;                   /* wall clock (ms) at play start;
                                            * video presents against it as
                                            * the master (audio) clock */
    int      have_pts;                     /* frame_pts populated (MP4) */
    void    *vdec;                /* video_decoder* when H.264 (else MJPEG) */
    int      vdec_errs;           /* consecutive chunk errors (leniency) */
    int      is_mp4;              /* MP4 container (samples are H.264) */
    uint8_t  vcfg[512];           /* SPS/PPS Annex-B from avcC (MP4) */
    int      vcfg_len;
    /* audio: MP3 (minimp3) -> kernel PCM16 stream */
    void    *mp3;                 /* toby_mp3_decoder_t* */
    int      audio_fd;
    long     apos;                /* read offset into data */
    int16_t  carry[MEDIA_PCM_CARRY];  /* decoded-but-unwritten PCM */
    int      carry_off, carry_len;    /* bytes */
    /* audio: AAC-LC from an MP4 mp4a track (raw AUs, not ADTS). The AU
     * table indexes into `data`; the decoder is configured from the esds
     * AudioSpecificConfig. */
    void    *aac;                 /* toby_aac_decoder_t* */
    long    *aud_off;             /* per-AU file offset (heap) */
    int     *aud_len;             /* per-AU byte length (heap) */
    int      naud, aud_cur;
    int      aac_rate, aac_ch;
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

/* Per-codepoint advance cache for non-ASCII text (utf8-text arc). Keyed
 * exactly by (cp, clamped px, face mod 32); one kernel width query per
 * unique key, then all wrapping math stays in userspace like ASCII. */
#define CPW_SLOTS 1024
static struct { uint32_t key; short adv; } g_cpw[CPW_SLOTS];

static int cp_adv(unsigned cp, int px, int face) {
    if (px < 8) px = 8;
    if (px > 40) px = 40;
    uint32_t key = (cp & 0x1FFFFF) | ((uint32_t)(px & 0x3F) << 21) |
                   ((uint32_t)(face & 0x1F) << 27);
    if (!key) key = 1;
    int h = (int)((key * 2654435761u) & (CPW_SLOTS - 1));
    int idx = -1;
    for (int probe = 0; probe < 8; probe++) {
        int k = (h + probe) & (CPW_SLOTS - 1);
        if (g_cpw[k].key == key) return g_cpw[k].adv;
        if (g_cpw[k].key == 0 && idx < 0) idx = k;
    }
    char u8[5];
    int n = 0;
    if (cp < 0x800) {
        u8[n++] = (char)(0xC0 | (cp >> 6));
        u8[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        u8[n++] = (char)(0xE0 | (cp >> 12));
        u8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u8[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        u8[n++] = (char)(0xF0 | (cp >> 18));
        u8[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u8[n++] = (char)(0x80 | (cp & 0x3F));
    }
    u8[n] = 0;
    int w = (face >= KFONT_WEB_BASE) ? sys_text_ttf_width(u8, px, face)
                                     : tk_text_width(u8, px, face ? 1 : 0);
    if (w <= 0) w = px * 3 / 5 + 3;              /* tofu-box advance */
    if (idx >= 0) { g_cpw[idx].key = key; g_cpw[idx].adv = (short)w; }
    return w;
}

static int text_px_w(const char *s, int len, int px, int face, int mono) {
    if (mono) return len * MONO_W;   /* mono stays byte-based ('?' cells) */
    const short *adv = adv_for(px, face);
    int w = 0;
    for (int i = 0; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            w += adv[(c >= 32 && c <= 126) ? c - 32 : '?' - 32];
            i++;
            continue;
        }
        unsigned cp;
        int extra;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { w += adv['?' - 32]; i++; continue; }
        if (i + extra >= len) { w += adv['?' - 32]; i++; continue; }
        int ok = 1;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { w += adv['?' - 32]; i++; continue; }
        w += cp_adv(cp, px, face);
        i += extra + 1;
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

/* Append a Unicode codepoint to the tpool as UTF-8 (utf8-text arc: the
 * whole pipeline -- measurement, wrapping, paint, the kernel TTF
 * rasterizer -- is codepoint-aware now; missing glyphs draw as tofu
 * boxes in the kernel). Zero-width characters are dropped; NBSP becomes
 * a normal space (the wrapper has no no-break support yet). */
static void tp_put_cp(unsigned int cp) {
    if (cp == 0xA0) { tp_putc(' '); return; }
    if (cp == '\t') { tp_putc(' '); tp_putc(' '); return; }
    if (cp < 0x20) { if (cp == '\n') tp_putc('\n'); return; }
    if (cp < 0x80) { tp_putc((char)cp); return; }
    if (cp == 0x200B || cp == 0x200C ||
        cp == 0xFEFF || cp == 0xAD) return;         /* zero-width */
    /* U+200D ZWJ is kept: it glues emoji ZWJ sequences (the inline layout
     * consumes it into a ligature, or drops a standalone one). */
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x800) {
        tp_putc((char)(0xC0 | (cp >> 6)));
        tp_putc((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        tp_putc((char)(0xE0 | (cp >> 12)));
        tp_putc((char)(0x80 | ((cp >> 6) & 0x3F)));
        tp_putc((char)(0x80 | (cp & 0x3F)));
    } else {
        tp_putc((char)(0xF0 | (cp >> 18)));
        tp_putc((char)(0x80 | ((cp >> 12) & 0x3F)));
        tp_putc((char)(0x80 | ((cp >> 6) & 0x3F)));
        tp_putc((char)(0x80 | (cp & 0x3F)));
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

/* ---- Color emoji (COLR/CPAL via libtoby) -------------------------- *
 * The emoji font (/etc/emoji.ttf, Twemoji Mozilla) is loaded lazily; a
 * small LRU caches rendered ARGB glyphs by (codepoint, px). Inline text
 * splits out emoji codepoints as atomic DI_EMOJI items painted here. */

static toby_emoji_t *g_emoji;
static int g_emoji_tried;
static uint8_t *g_emoji_ttf;

static toby_emoji_t *emoji_font(void) {
    if (g_emoji_tried) return g_emoji;
    g_emoji_tried = 1;
    long fd = sys_open("/etc/emoji.ttf", O_RDONLY, 0);
    if (fd < 0) return NULL;
    /* read the whole font into a growable buffer */
    unsigned long cap = 2u << 20, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { sys_close((int)fd); return NULL; }
    for (;;) {
        if (len + 65536 > cap) {
            unsigned long ncap = cap * 2;
            uint8_t *nb = (uint8_t *)malloc(ncap);
            if (!nb) break;
            memcpy(nb, buf, len); free(buf); buf = nb; cap = ncap;
        }
        long r = sys_read((int)fd, buf + len, 65536);
        if (r <= 0) break;
        len += (unsigned long)r;
    }
    sys_close((int)fd);
    if (len < 16) { free(buf); return NULL; }
    g_emoji_ttf = buf;                       /* must outlive the handle */
    g_emoji = toby_emoji_load(buf, len);
    return g_emoji;
}

static int emoji_is(unsigned int cp) {
    if (cp < 0x2000) return 0;               /* ASCII/Latin: never emoji */
    toby_emoji_t *e = emoji_font();
    return e && toby_emoji_has(e, (int)cp);
}

/* Decode one UTF-8 codepoint from [s, s+n); *adv = bytes consumed. */
static unsigned int emoji_next_cp(const char *s, int n, int *adv) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *adv = 1; return c; }
    unsigned int cp; int extra;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { *adv = 1; return c; }
    if (extra >= n) { *adv = 1; return c; }
    for (int k = 1; k <= extra; k++) {
        unsigned char cc = (unsigned char)s[k];
        if ((cc & 0xC0) != 0x80) { *adv = 1; return (unsigned char)s[0]; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *adv = extra + 1;
    return cp;
}

#define EMOJI_CACHE 24
#define EMOJI_SEQ_CPS 8
static struct emoji_glyph {
    int cps[EMOJI_SEQ_CPS], n, px, w, h, adv, lru;
    uint32_t *rgba;
} g_emoji_cache[EMOJI_CACHE];
static int g_emoji_lru;

static int emoji_seq_eq(const struct emoji_glyph *g, const int *cps, int n, int px) {
    if (g->px != px || g->n != n) return 0;
    for (int i = 0; i < n; i++) if (g->cps[i] != cps[i]) return 0;
    return 1;
}

/* Render+cache the color glyph for a codepoint sequence (n==1 = a plain
 * emoji; n>1 = a ZWJ ligature). */
static struct emoji_glyph *emoji_get(const int *cps, int n, int px) {
    if (px < 6) px = 6; if (px > 128) px = 128;
    if (n > EMOJI_SEQ_CPS) n = EMOJI_SEQ_CPS;
    for (int i = 0; i < EMOJI_CACHE; i++)
        if (g_emoji_cache[i].rgba && emoji_seq_eq(&g_emoji_cache[i], cps, n, px)) {
            g_emoji_cache[i].lru = ++g_emoji_lru;
            return &g_emoji_cache[i];
        }
    toby_emoji_t *e = emoji_font();
    if (!e) return NULL;
    int w = 0, h = 0, adv = 0;
    uint32_t *rgba = toby_emoji_render_seq(e, cps, n, px, &w, &h, &adv);
    if (!rgba) return NULL;
    int slot = 0, best = g_emoji_cache[0].lru;
    for (int i = 1; i < EMOJI_CACHE; i++)
        if (!g_emoji_cache[i].rgba) { slot = i; break; }
        else if (g_emoji_cache[i].lru < best) { best = g_emoji_cache[i].lru; slot = i; }
    if (g_emoji_cache[slot].rgba) free(g_emoji_cache[slot].rgba);
    for (int i = 0; i < n; i++) g_emoji_cache[slot].cps[i] = cps[i];
    g_emoji_cache[slot].n = n; g_emoji_cache[slot].px = px;
    g_emoji_cache[slot].w = w; g_emoji_cache[slot].h = h;
    g_emoji_cache[slot].adv = adv; g_emoji_cache[slot].rgba = rgba;
    g_emoji_cache[slot].lru = ++g_emoji_lru;
    return &g_emoji_cache[slot];
}

/* Per-layout pool of emoji codepoint sequences (a DI_EMOJI item's `off`
 * indexes it). Reset each layout pass. */
#define EMOJI_SEQ_MAX 512
static int g_eseq[EMOJI_SEQ_MAX][EMOJI_SEQ_CPS];
static int g_eseq_n[EMOJI_SEQ_MAX];
static int g_neseq;

static int emoji_seq_add(const int *cps, int n) {
    if (g_neseq >= EMOJI_SEQ_MAX) return -1;
    if (n > EMOJI_SEQ_CPS) n = EMOJI_SEQ_CPS;
    int idx = g_neseq++;
    for (int i = 0; i < n; i++) g_eseq[idx][i] = cps[i];
    g_eseq_n[idx] = n;
    return idx;
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
    "slot",
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
    n->shadow = -1;
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
static void dom_set_attr(int ni, const char *name, const char *val);

/* Custom elements (shadow-dom arc): dnode tags are a fixed enum, so a
 * source tag like <x-counter> maps to T_UNK and its NAME would be lost.
 * Stash the lowercased name as a synthetic "__tag" attribute when it
 * looks like a custom element (contains '-'); D.tag()/customElements
 * read it back. */
static void dom_stash_custom_tag(int ni, const char *s, long len) {
    if (ni < 0 || len <= 0 || len > 48) return;
    char nm[49];
    int dash = 0;
    for (long q = 0; q < len; q++) {
        nm[q] = (char)lc(s[q]);
        if (nm[q] == '-') dash = 1;
    }
    nm[len] = 0;
    if (dash) dom_set_attr(ni, "__tag", nm);
}

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
            if (t == T_UNK) dom_stash_custom_tag(ni, s + k0, k - k0);
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

/* ---- Media query evaluation ----------------------------------------- *
 * Real mobile-first sheets gate their layouts on forms the old integer
 * parser could not read -- em/rem lengths, calc() arithmetic (wikipedia:
 * max-width:calc(640px - 1px)), and level-4 range syntax (mdn:
 * (width < calc(1rem*2 + 15rem + 2rem + 31rem))). Failing those queries
 * silently selected the WRONG layout (mdn rendered its desktop grid at
 * a 719px viewport and stacked it). Unknown features still fail. */

/* calc()-capable length -> px (float). expr := term {(+|-) term};
 * term := factor {(*|/) factor}; factor := number[unit] | (expr).
 * Returns 0 on parse failure (*ok cleared). */
static float med_expr(const char *s, int n, int *pos, int *ok);

static float med_factor(const char *s, int n, int *pos, int *ok) {
    int i = *pos;
    while (i < n && is_whitespace(s[i])) i++;
    if (i < n && s[i] == '(') {
        i++;
        float v = med_expr(s, n, &i, ok);
        while (i < n && is_whitespace(s[i])) i++;
        if (i < n && s[i] == ')') i++;
        else *ok = 0;
        *pos = i;
        return v;
    }
    if (i + 5 <= n && str_ncasecmp(s + i, "calc(", 5) == 0) {
        i += 5;
        float v = med_expr(s, n, &i, ok);
        while (i < n && is_whitespace(s[i])) i++;
        if (i < n && s[i] == ')') i++;
        else *ok = 0;
        *pos = i;
        return v;
    }
    int neg = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); i++; }
    if (i >= n || !((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
        *ok = 0;
        *pos = i;
        return 0;
    }
    float v = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (i < n && s[i] == '.') {
        i++;
        float f = 0.1f;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v += (s[i] - '0') * f; f *= 0.1f; i++; }
    }
    if (neg) v = -v;
    /* unit (default px; 1em/1rem = 16px in media queries) */
    if (i + 2 <= n && (str_ncasecmp(s + i, "em", 2) == 0)) { v *= 16; i += 2; }
    else if (i + 3 <= n && str_ncasecmp(s + i, "rem", 3) == 0) { v *= 16; i += 3; }
    else if (i + 2 <= n && str_ncasecmp(s + i, "px", 2) == 0) i += 2;
    else if (i + 2 <= n && str_ncasecmp(s + i, "vw", 2) == 0) { v = v * g_win_w / 100; i += 2; }
    else if (i + 2 <= n && str_ncasecmp(s + i, "vh", 2) == 0) { v = v * g_win_h / 100; i += 2; }
    *pos = i;
    return v;
}

static float med_expr(const char *s, int n, int *pos, int *ok) {
    float v = med_factor(s, n, pos, ok);
    for (;;) {
        int i = *pos;
        while (i < n && is_whitespace(s[i])) i++;
        if (i < n && (s[i] == '*' || s[i] == '/')) {
            char op = s[i];
            i++;
            *pos = i;
            float r = med_factor(s, n, pos, ok);
            if (op == '*') v *= r;
            else v = (r != 0) ? v / r : 0;
            continue;
        }
        if (i < n && (s[i] == '+' || s[i] == '-') &&
            i + 1 < n && (is_whitespace(s[i + 1]) || s[i + 1] == '(' ||
                          (s[i + 1] >= '0' && s[i + 1] <= '9') || s[i + 1] == '.')) {
            char op = s[i];
            i++;
            *pos = i;
            float r = med_factor(s, n, pos, ok);
            if (op == '+') v += r;
            else v -= r;
            continue;
        }
        break;
    }
    return v;
}

static int med_len_px(const char *s, int n, float *out) {
    int pos = 0, ok = 1;
    float v = med_expr(s, n, &pos, &ok);
    while (pos < n && is_whitespace(s[pos])) pos++;
    if (!ok || pos < n) return 0;         /* trailing junk = unparseable */
    *out = v;
    return 1;
}

/* One (feature) -- text between the feature's own parens, which may
 * contain nested parens (calc). name:value, bare presence, or level-4
 * range comparisons with width/height on either side. */
static int media_feature(const char *s, int n) {
    while (n > 0 && is_whitespace(s[0])) { s++; n--; }
    while (n > 0 && is_whitespace(s[n - 1])) n--;
    if (n <= 0) return 0;
    /* find a top-level ':' or comparison op */
    int par = 0, colon = -1, cmp0 = -1;
    char c1 = 0, c2 = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '(') par++;
        else if (s[i] == ')') par--;
        else if (par == 0 && s[i] == ':' && colon < 0) colon = i;
        else if (par == 0 && cmp0 < 0 && (s[i] == '<' || s[i] == '>')) {
            cmp0 = i;
            c1 = s[i];
            c2 = (i + 1 < n && s[i + 1] == '=') ? '=' : 0;
        }
    }
    if (cmp0 >= 0) {                       /* range syntax */
        int a0 = 0, a1 = cmp0;
        int b0 = cmp0 + (c2 ? 2 : 1), b1 = n;
        while (a1 > a0 && is_whitespace(s[a1 - 1])) a1--;
        while (b0 < b1 && is_whitespace(s[b0])) b0++;
        /* which side is the feature keyword? */
        int fa = (a1 - a0 == 5 && str_ncasecmp(s + a0, "width", 5) == 0) ? 1
               : (a1 - a0 == 6 && str_ncasecmp(s + a0, "height", 6) == 0) ? 2 : 0;
        int fb = (b1 - b0 == 5 && str_ncasecmp(s + b0, "width", 5) == 0) ? 1
               : (b1 - b0 == 6 && str_ncasecmp(s + b0, "height", 6) == 0) ? 2 : 0;
        float lim;
        if (fa && !fb) {                   /* width < expr */
            if (!med_len_px(s + b0, b1 - b0, &lim)) return 0;
            float w = (fa == 1) ? (float)g_win_w : (float)g_win_h;
            if (c1 == '<') return c2 ? w <= lim : w < lim;
            return c2 ? w >= lim : w > lim;
        }
        if (fb && !fa) {                   /* expr < width */
            if (!med_len_px(s + a0, a1 - a0, &lim)) return 0;
            float w = (fb == 1) ? (float)g_win_w : (float)g_win_h;
            if (c1 == '<') return c2 ? lim <= w : lim < w;
            return c2 ? lim >= w : lim > w;
        }
        return 0;                          /* A < width < B etc: not yet */
    }
    int flen = (colon >= 0) ? colon : n;
    while (flen > 0 && is_whitespace(s[flen - 1])) flen--;
    const char *vv = (colon >= 0) ? s + colon + 1 : NULL;
    int vn = (colon >= 0) ? n - colon - 1 : 0;
    while (vn > 0 && is_whitespace(vv[0])) { vv++; vn--; }
    float px;
    if (flen == 9 && str_ncasecmp(s, "min-width", 9) == 0)
        return vv && med_len_px(vv, vn, &px) && g_win_w >= px;
    if (flen == 9 && str_ncasecmp(s, "max-width", 9) == 0)
        return vv && med_len_px(vv, vn, &px) && g_win_w <= px;
    if (flen == 5 && str_ncasecmp(s, "width", 5) == 0)
        return vv && med_len_px(vv, vn, &px) && g_win_w == (int)px;
    if (flen == 10 && str_ncasecmp(s, "min-height", 10) == 0)
        return vv && med_len_px(vv, vn, &px) && g_win_h >= px;
    if (flen == 10 && str_ncasecmp(s, "max-height", 10) == 0)
        return vv && med_len_px(vv, vn, &px) && g_win_h <= px;
    if (flen == 6 && str_ncasecmp(s, "height", 6) == 0)
        return vv && med_len_px(vv, vn, &px) && g_win_h == (int)px;
    if (flen == 11 && str_ncasecmp(s, "orientation", 11) == 0)
        return (vn > 0 && lc(vv[0]) == 'l') ? (g_win_w >= g_win_h)
                                            : (g_win_w < g_win_h);
    if (flen == 20 && str_ncasecmp(s, "prefers-color-scheme", 20) == 0)
        return vn > 0 && lc(vv[0]) == 'l';
    return 0;                              /* unknown feature fails */
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
                /* capture to the MATCHING ')' -- calc() nests parens */
                int f0 = ++i, par = 0;
                while (i < len && !(par == 0 && s[i] == ')')) {
                    if (s[i] == '(') par++;
                    else if (s[i] == ')') par--;
                    i++;
                }
                int fend = i;
                if (i < len) i++;         /* ')' */
                if (!media_feature(s + f0, fend - f0)) ok = 0;
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
        {"min-width",CP_MINW},{"min-height",CP_MINH},
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
        {"animation",CP_ANIMATION},{"animation-name",CP_ANIM_NAME},
        {"animation-duration",CP_ANIM_DUR},{"animation-iteration-count",CP_ANIM_ITER},
        {"animation-delay",CP_ANIM_DELAY},{"animation-direction",CP_ANIM_DIR},
        {"animation-timing-function",CP_ANIM_TIMING},
        {"transition",CP_TRANSITION},{"transition-property",CP_TRANS_PROP},
        {"transition-duration",CP_TRANS_DUR},{"transition-delay",CP_TRANS_DELAY},
        {"transition-timing-function",CP_TRANS_TIMING},
        {"opacity",CP_OPACITY},
        {"border-radius",CP_BORDER_RADIUS},{"box-shadow",CP_BOX_SHADOW},
        {"mask-image",CP_MASK},{"-webkit-mask-image",CP_MASK},
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
    p->nnot = 0;
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
            if (p->nclass >= PART_CLS_MAX) return 0;   /* fail closed: a
                 * kept prefix would over-match (apply state-variant rules
                 * to every element carrying the base classes) */
            p->cls_off[p->nclass] = off;
            p->cls_len[p->nclass] = (int16_t)l;
            p->nclass++;
            (*spec_cls)++;
            got = 1;
            continue;
        }
        if (ch == '#') {
            c->i++;
            int off, l;
            if (!css_ident(c, &off, &l)) return 0;
            if (p->id_len) return 0;      /* fail closed, as for classes */
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
            if (p->has_attr) return 0;    /* one attr matcher per compound */
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
            if (l == 3 && str_ncasecmp(pp, "not", 3) == 0 &&
                c->i < c->n && c->s[c->i] == '(') {
                c->i++;
                css_ws(c);
                if (p->nnot >= PART_NOT_MAX) return 0;   /* fail closed */
                char ch2 = (c->i < c->n) ? c->s[c->i] : 0;
                int koff, kl, kind;
                if (ch2 == '.') {
                    c->i++;
                    if (!css_ident(c, &koff, &kl)) return 0;
                    kind = NOT_CLASS;
                    (*spec_cls)++;
                } else if (ch2 == '#') {
                    c->i++;
                    if (!css_ident(c, &koff, &kl)) return 0;
                    kind = NOT_ID;
                    (*spec_id)++;
                } else if (ch2 == '[') {
                    c->i++;
                    css_ws(c);
                    if (!css_ident(c, &koff, &kl)) return 0;
                    css_ws(c);
                    if (c->i >= c->n || c->s[c->i] != ']') return 0;
                    c->i++;               /* presence only; [a=v] unsupported */
                    kind = NOT_ATTR;
                    (*spec_cls)++;
                } else if (is_alpha(ch2)) {
                    long t0 = c->i;
                    while (c->i < c->n) {
                        char cc = c->s[c->i];
                        if (is_alpha(cc) || (cc >= '0' && cc <= '9') ||
                            cc == '-' || cc == '_')
                            c->i++;
                        else break;
                    }
                    int tg = tag_lookup(c->s + t0, (int)(c->i - t0));
                    if (tg == T_UNK) return 0;   /* unknown tag: can't test */
                    koff = tg;            /* kind TAG: off = tag id, len 0 */
                    kl = 0;
                    kind = NOT_TAG;
                    (*spec_type)++;
                } else {
                    return 0;             /* complex :not() arg: fail closed */
                }
                css_ws(c);
                if (c->i >= c->n || c->s[c->i] != ')') return 0;
                c->i++;
                p->not_kind[p->nnot] = (uint8_t)kind;
                p->not_off[p->nnot] = koff;
                p->not_len[p->nnot] = (int16_t)kl;
                p->nnot++;
                got = 1;
                continue;
            }
            return 0;                     /* :hover, :is(...), etc. */
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
        if (ch == '+') { pending_comb = 3; c->i++; continue; }
        if (ch == '~') { pending_comb = 4; c->i++; continue; }
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
        r->scope = g_css_scope;      /* -1 unless inside a shadow tree */
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
/* ---- CSS @keyframes animations (v1: translate + color + background) --- *
 * A small registry of named keyframe sets; each stop carries the animated
 * subset (translate px, color, background-color). The animation clock
 * (anim_apply) interpolates between stops and overrides computed style
 * before layout, driving the existing reflow loop. */
#define KF_MAX   24
#define KF_STOPS 8
/* has bits: 1 translate, 2 color, 4 bg, 8 scale, 16 rotate, 32 opacity */
struct kf_stop { uint8_t pct, has; int16_t txpx, typx; uint32_t color, bg;
                 uint16_t scx, scy; int16_t rot; uint8_t opa; };
struct kf_anim { char name[28]; int nstops; struct kf_stop stops[KF_STOPS]; };
static struct kf_anim g_kf[KF_MAX];
static int      g_nkf;
static int      g_anim_active;          /* >=1 while any element animates */
static int32_t  g_anim_t0[NODE_MAX];    /* per-node animation start ms, -1 */

/* CSS transitions: per-node runtime for the transitionable properties.
 * from/to/cur are the interpolation endpoints + current displayed value;
 * a transition (re)starts when the newly computed target differs. */
struct trans_state {
    uint8_t  seen, active;              /* bit0 color, bit1 bg, bit2 transform,
                                           bit3 opacity */
    uint32_t from_color, to_color, cur_color;
    uint32_t from_bg, to_bg, cur_bg;
    int16_t  from_tx, to_tx, cur_tx, from_ty, to_ty, cur_ty;
    /* transform scale/rotate ride the same bit-2 channel/t0 as translate
     * (CSS transitions animate the whole `transform` value as one). */
    uint16_t from_sx, to_sx, cur_sx, from_sy, to_sy, cur_sy;
    int16_t  from_rot, to_rot, cur_rot;
    uint8_t  from_opa, to_opa, cur_opa;
    int32_t  t0_color, t0_bg, t0_tf, t0_opa;
};
static struct trans_state g_trans[NODE_MAX];

/* ---- Compositing layers (css-anim slice 3) ------------------------- *
 * An element with transform: scale/rotate or opacity < 1 paints through
 * a layer: its display items [i0, i1) -- contiguous, because lay_block
 * emits a node's subtree in one run -- are rendered into an offscreen
 * ARGB buffer, then affine-blitted (scale+rotate about the layer's
 * center, alpha scaled by opacity) via tk_draw_blit_blend. Registered
 * at layout (skipping measure passes), reset per layout(); the bbox is
 * computed at PAINT time from the live item rects so later ancestor
 * shifts (relative/translate/abs placement) need no bookkeeping. */
struct clayer {
    int32_t  node, i0, i1;
    uint16_t scx, scy;           /* per-mille */
    int16_t  rot;                /* degrees */
    uint8_t  opa;                /* 0..255 */
};
#define LAYER_MAX 32
static struct clayer g_layer_tab[LAYER_MAX];
static int g_nlayer;

/* position: sticky (scroll-pinned). Recorded at layout; the paint loop
 * pins each group's items to the viewport top once the page scrolls
 * past its threshold, within its containing block. */
struct sticky { int32_t i0, i1, top, bot, top_off, cb_bot; };
#define STICKY_MAX 16
static struct sticky g_sticky[STICKY_MAX];
static int g_nsticky;

/* Parse a CSS <number> or <percentage> to per-mille: "1.5" -> 1500,
 * "0.35" -> 350, "50%" -> 500. Clamps to +-32000. */
static int css_frac_1000(const char *s, int n) {
    int i = 0, neg = 0;
    long whole = 0, frac = 0, fdiv = 1;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && s[i] == '-') { neg = 1; i++; }
    while (i < n && s[i] >= '0' && s[i] <= '9') { whole = whole * 10 + (s[i] - '0'); i++; }
    if (i < n && s[i] == '.') { i++;
        while (i < n && s[i] >= '0' && s[i] <= '9' && fdiv < 1000000) {
            frac = frac * 10 + (s[i] - '0'); fdiv *= 10; i++; } }
    long v = whole * 1000 + frac * 1000 / fdiv;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && s[i] == '%') v /= 100;
    if (neg) v = -v;
    /* wide clamp: angles reach 360000 ("360deg"); scale/opacity apply
     * their own tighter clamps at the call sites */
    if (v > 2000000) v = 2000000;
    if (v < -2000000) v = -2000000;
    return (int)v;
}

/* Parse a CSS <angle> ("45deg", "-30deg", "0.25turn") to whole degrees. */
static int css_rot_deg(const char *s, int n) {
    int v1000 = css_frac_1000(s, n);
    if (str_contains(s, n, "turn", 4) >= 0)
        return (int)((long)v1000 * 360 / 1000);
    return v1000 / 1000;
}

/* Scan one transform function list: translate()/translateX/Y (px/%),
 * scale()/scaleX/scaleY (number), rotate (deg/turn). Shared by the
 * cascade (CP_TRANSFORM) and @keyframes stop parsing. Out-params may
 * be NULL when a caller only wants a subset. */
static void css_parse_transform(const char *v, int n, int fontpx,
                                uint8_t *has_tf,
                                int16_t *txpx, int16_t *typx,
                                int8_t *txpct, int8_t *typct,
                                uint16_t *scx, uint16_t *scy, int16_t *rot,
                                uint8_t *has_sc, uint8_t *has_rot) {
    int q = 0;
    while (q < n) {
        while (q < n && !((v[q] >= 'a' && v[q] <= 'z') ||
                          (v[q] >= 'A' && v[q] <= 'Z'))) q++;
        int f0 = q;
        while (q < n && ((v[q] >= 'a' && v[q] <= 'z') ||
                         (v[q] >= 'A' && v[q] <= 'Z'))) q++;
        int fl2 = q - f0;
        while (q < n && v[q] != '(') q++;
        if (q >= n) break;
        int a0 = ++q;
        while (q < n && v[q] != ')') q++;
        int an = q - a0;
        if (q < n) q++;
        const char *fn = v + f0, *ar = v + a0;
        if (fl2 >= 9 && str_ncasecmp(fn, "translate", 9) == 0 && txpx) {
            int isx = (fl2 == 10 && lc(fn[9]) == 'x');
            int isy = (fl2 == 10 && lc(fn[9]) == 'y');
            int comp = 0, cs = 0;
            for (int p2 = 0; p2 <= an && comp < 2; p2++) {
                if (p2 == an || ar[p2] == ',') {
                    int cb = cs, cl = p2 - cs;
                    while (cl > 0 && is_whitespace(ar[cb])) { cb++; cl--; }
                    while (cl > 0 && is_whitespace(ar[cb + cl - 1])) cl--;
                    if (cl > 0) {
                        int pct = (ar[cb + cl - 1] == '%');
                        int px = 0;
                        if (pct) {
                            int num = 0, neg = 0, k = cb;
                            if (ar[k] == '-') { neg = 1; k++; }
                            for (; k < cb + cl - 1 && ar[k] >= '0' && ar[k] <= '9'; k++)
                                num = num * 10 + (ar[k] - '0');
                            px = neg ? -num : num;
                        } else {
                            css_len_tok(ar + cb, cl, fontpx, &px);
                        }
                        int axis = isy ? 1 : (isx ? 0 : comp);
                        if (axis == 0) {
                            if (pct) { if (txpct) *txpct = (int8_t)px; }
                            else *txpx = (int16_t)px;
                        } else {
                            if (pct) { if (typct) *typct = (int8_t)px; }
                            else *typx = (int16_t)px;
                        }
                        if (has_tf) *has_tf = 1;
                    }
                    cs = p2 + 1;
                    comp++;
                }
            }
        } else if (fl2 >= 5 && str_ncasecmp(fn, "scale", 5) == 0 && scx) {
            int isx = (fl2 == 6 && lc(fn[5]) == 'x');
            int isy = (fl2 == 6 && lc(fn[5]) == 'y');
            int ci = 0; while (ci < an && ar[ci] != ',') ci++;
            int sa = css_frac_1000(ar, ci);
            if (sa < 0) sa = 0;
            if (sa > 16000) sa = 16000;
            if (isx)      *scx = (uint16_t)sa;
            else if (isy) { if (scy) *scy = (uint16_t)sa; }
            else {
                *scx = (uint16_t)sa;
                int sb = sa;
                if (ci < an) {
                    sb = css_frac_1000(ar + ci + 1, an - ci - 1);
                    if (sb < 0) sb = 0;
                    if (sb > 16000) sb = 16000;
                }
                if (scy) *scy = (uint16_t)sb;
            }
            if (has_sc) *has_sc = 1;
        } else if (fl2 == 6 && str_ncasecmp(fn, "rotate", 6) == 0 && rot) {
            /* keep the raw degree count (e.g. "360deg" endpoints in a
             * spinner keyframe must NOT collapse to 0); the painter's
             * sin table normalizes. int16 covers +-32767 degrees. */
            int d = css_rot_deg(ar, an);
            if (d > 32000) d = 32000;
            if (d < -32000) d = -32000;
            *rot = (int16_t)d;
            if (has_rot) *has_rot = 1;
        }
    }
}

/* Extract the animatable declarations from one keyframe stop block. */
static void kf_parse_stop_decls(const char *s, int n, struct kf_stop *st) {
    int i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == ';' || s[i] == '\n' ||
                         s[i] == '\t' || s[i] == '\r')) i++;
        int p0 = i;
        while (i < n && s[i] != ':' && s[i] != '}') i++;
        if (i >= n || s[i] != ':') break;
        int pn = i - p0; i++;
        int v0 = i;
        while (i < n && s[i] != ';' && s[i] != '}') i++;
        int vn = i - v0;
        while (pn > 0 && s[p0 + pn - 1] == ' ') pn--;
        const char *vv = s + v0;
        while (vn > 0 && (*vv == ' ' || *vv == '\t')) { vv++; vn--; }
        while (vn > 0 && (vv[vn - 1] == ' ' || vv[vn - 1] == ';')) vn--;
        if (pn == 9 && str_ncasecmp(s + p0, "transform", 9) == 0) {
            uint8_t htf = 0, hsc = 0, hrot = 0;
            int16_t tx = 0, ty = 0, rot = 0;
            uint16_t sx = 1000, sy = 1000;
            css_parse_transform(vv, vn, 16, &htf, &tx, &ty, NULL, NULL,
                                &sx, &sy, &rot, &hsc, &hrot);
            if (htf)  { st->has |= 1;  st->txpx = tx; st->typx = ty; }
            if (hsc)  { st->has |= 8;  st->scx = sx; st->scy = sy; }
            if (hrot) { st->has |= 16; st->rot = rot; }
        } else if (pn == 7 && str_ncasecmp(s + p0, "opacity", 7) == 0) {
            int m = css_frac_1000(vv, vn);
            if (m < 0) m = 0;
            if (m > 1000) m = 1000;
            st->opa = (uint8_t)(m * 255 / 1000);
            st->has |= 32;
        } else if (pn == 5 && str_ncasecmp(s + p0, "color", 5) == 0) {
            uint32_t c; if (css_color_tok(vv, vn, &c)) { st->color = c; st->has |= 2; }
        } else if ((pn == 16 && str_ncasecmp(s + p0, "background-color", 16) == 0) ||
                   (pn == 10 && str_ncasecmp(s + p0, "background", 10) == 0)) {
            uint32_t c; if (css_color_tok(vv, vn, &c)) { st->bg = c; st->has |= 4; }
        }
    }
}

/* Parse one @keyframes block (cursor is just past the "keyframes" ident). */
static void kf_parse(struct ccur *c) {
    css_ws(c);
    int no, nl; css_ident(c, &no, &nl);
    char name[28];
    int nn = nl < (int)sizeof(name) - 1 ? nl : (int)sizeof(name) - 1;
    for (int k = 0; k < nn; k++) name[k] = lc(E->csspool[no + k]);
    name[nn] = 0;
    css_ws(c);
    if (c->i >= c->n || c->s[c->i] != '{') return;
    c->i++;
    struct kf_anim *a = (g_nkf < KF_MAX) ? &g_kf[g_nkf] : NULL;
    if (a) { str_copy(a->name, name, sizeof(a->name)); a->nstops = 0; }
    while (c->i < c->n && c->s[c->i] != '}') {
        css_ws(c);
        if (c->i >= c->n || c->s[c->i] == '}') break;
        int sel0 = c->i;
        while (c->i < c->n && c->s[c->i] != '{' && c->s[c->i] != '}') c->i++;
        if (c->i >= c->n || c->s[c->i] != '{') break;
        int seln = c->i - sel0; c->i++;
        int b0 = c->i;
        while (c->i < c->n && c->s[c->i] != '}') c->i++;
        int bn = c->i - b0;
        if (c->i < c->n) c->i++;
        if (!a) continue;
        int p = sel0, end = sel0 + seln;
        while (p < end && a->nstops < KF_STOPS) {
            while (p < end && (c->s[p] == ' ' || c->s[p] == ',' || c->s[p] == '\n' ||
                               c->s[p] == '\t' || c->s[p] == '\r')) p++;
            int t0 = p;
            while (p < end && c->s[p] != ',') p++;
            int tn = p - t0;
            while (tn > 0 && c->s[t0 + tn - 1] == ' ') tn--;
            int pct = -1;
            if (tn >= 4 && str_ncasecmp(c->s + t0, "from", 4) == 0) pct = 0;
            else if (tn >= 2 && str_ncasecmp(c->s + t0, "to", 2) == 0) pct = 100;
            else if (tn > 0) { int v = 0;
                for (int k = 0; k < tn && c->s[t0 + k] >= '0' && c->s[t0 + k] <= '9'; k++)
                    v = v * 10 + (c->s[t0 + k] - '0');
                pct = v; }
            if (pct >= 0 && pct <= 100) {
                struct kf_stop *s = &a->stops[a->nstops++];
                s->pct = (uint8_t)pct; s->has = 0; s->txpx = s->typx = 0;
                s->color = 0; s->bg = 0;
                s->scx = s->scy = 1000; s->rot = 0; s->opa = 255;
                kf_parse_stop_decls(c->s + b0, bn, s);
            }
            if (p < end && c->s[p] == ',') p++;
        }
    }
    if (c->i < c->n && c->s[c->i] == '}') c->i++;
    if (a) {
        for (int i = 1; i < a->nstops; i++) {   /* stable sort by pct */
            struct kf_stop t = a->stops[i]; int j = i - 1;
            while (j >= 0 && a->stops[j].pct > t.pct) { a->stops[j + 1] = a->stops[j]; j--; }
            a->stops[j + 1] = t;
        }
        g_nkf++;
    }
}

/* Find a registered @keyframes set by (trimmed) name; -1 if none. */
static int kf_find(const char *s, int n) {
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    for (int i = 0; i < g_nkf; i++) {
        int L = (int)str_len(g_kf[i].name);
        if (L == n && str_ncasecmp(s, g_kf[i].name, n) == 0) return i;
    }
    return -1;
}

/* Parse a CSS <time> ("1s", "1.5s", "300ms") to milliseconds. */
static int css_time_ms(const char *s, int n) {
    int i = 0, whole = 0, frac = 0, fdiv = 1;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') { whole = whole * 10 + (s[i] - '0'); i++; }
    if (i < n && s[i] == '.') { i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            frac = frac * 10 + (s[i] - '0'); fdiv *= 10; i++; } }
    if (i + 1 <= n && i < n && lc(s[i]) == 'm')     /* "ms" */
        return whole + frac / fdiv;
    return whole * 1000 + frac * 1000 / fdiv;       /* seconds (default) */
}

/* Map a transition-property name to a transitionable-property bit
 * (bit0 color, bit1 background-color, bit2 transform, bit3 opacity;
 * "all" = all). */
static uint8_t trans_prop_bit(const char *s, int n) {
    if (n == 3 && str_ncasecmp(s, "all", 3) == 0) return 15;
    if (n == 5 && str_ncasecmp(s, "color", 5) == 0) return 1;
    if ((n == 16 && str_ncasecmp(s, "background-color", 16) == 0) ||
        (n == 10 && str_ncasecmp(s, "background", 10) == 0)) return 2;
    if (n == 9 && str_ncasecmp(s, "transform", 9) == 0) return 4;
    if (n == 7 && str_ncasecmp(s, "opacity", 7) == 0) return 8;
    return 0;
}

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
            if ((l == 9 && str_ncasecmp(&E->csspool[o], "keyframes", 9) == 0) ||
                (l == 17 && str_ncasecmp(&E->csspool[o], "-webkit-keyframes", 17) == 0)) {
                kf_parse(&c);
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

/* -DSTYLE_PROF: where the style pass actually spends its time. rdtsc
 * (~20 cycles) rather than gettimeofday, which would dominate at
 * per-node granularity. */
#ifdef STYLE_PROF
static inline unsigned long long sp_tsc(void) {
    unsigned int a, d;
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    return ((unsigned long long)d << 32) | a;
}
static unsigned long long g_sp_pres, g_sp_match, g_sp_inline,
                          g_sp_var, g_sp_apply;
static long g_sp_nodes, g_sp_matches, g_sp_decls;
static long g_sp_cand;          /* rules actually tested (bucket walk) */
static long g_sp_uni;           /* rules stuck in the universal bucket */
static long g_sp_partm;         /* part_match calls (ancestor hops) */
static long g_sp_clook;         /* class_attr_contains calls */
#define SP_T0(v)      unsigned long long v = sp_tsc()
#define SP_T1(acc, v) do { (acc) += sp_tsc() - (v); } while (0)
#define SP_N(c, n)    do { (c) += (n); } while (0)
#else
#define SP_T0(v)      do {} while (0)
#define SP_T1(acc, v) do {} while (0)
#define SP_N(c, n)    do {} while (0)
#endif

static uint32_t ridx_hash32(const char *s, int len) {
    /* FNV-1a over lowercased text: part_match compares id/class
     * case-insensitively (csspool text is already lowercased), so the
     * index has to agree or lookups miss. */
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)lc(s[i]);
        h *= 16777619u;
    }
    return h;
}

static uint32_t ridx_hash(const char *s, int len) {
    return ridx_hash32(s, len) & (RIDX_BUCKETS - 1);
}

/* ---- Ancestor bloom (Blink's "fast reject") ------------------------
 * Matching a descendant selector means walking every ancestor and
 * re-testing -- measured on wikipedia: 10 part_match calls and ~7 class
 * re-parses per candidate, which was 85% of the whole style pass.
 *
 * Each node carries a 64-bit bloom of the keys (tag / id / class) of
 * ITSELF AND ALL ITS ANCESTORS, and each rule precomputes the bits its
 * NON-rightmost parts demand. If the node's parent bloom lacks any of
 * them, no ancestor can satisfy the selector and the walk is skipped.
 *
 * Correctness: a bloom only ever yields false POSITIVES (we then walk
 * and match properly), never false negatives -- a real ancestor's key is
 * always OR'd in. So the match set is unchanged; -DCSS_VERIFY proves it. */
static inline uint64_t bloom_bit(uint32_t h) {
    return (1ull << (h & 63)) | (1ull << ((h >> 8) & 63));
}
static inline uint32_t bloom_tag_hash(int tag) {
    return 0x9E3779B9u ^ (uint32_t)(tag * 2654435761u);
}

/* The single key this compound demands, as bloom bits (0 = none). */
static uint64_t part_key_bits(const struct cpart *p) {
    if (p->id_len > 0)
        return bloom_bit(ridx_hash32(&E->csspool[p->id_off], p->id_len));
    if (p->nclass > 0)
        return bloom_bit(ridx_hash32(&E->csspool[p->cls_off[0]], p->cls_len[0]));
    if (p->tag >= 0)
        return bloom_bit(bloom_tag_hash(p->tag));
    return 0;
}

/* Keys of a node itself: tag + id + every class. */
static uint64_t node_self_bits(int ni) {
    const struct dnode *nd = &E->nodes[ni];
    uint64_t b = 0;
    if (nd->tag >= 0) b |= bloom_bit(bloom_tag_hash(nd->tag));
    int vlen;
    const char *v = node_attr(nd, "id", &vlen);
    if (v && vlen > 0) b |= bloom_bit(ridx_hash32(v, vlen));
    v = node_attr(nd, "class", &vlen);
    if (v && vlen > 0) {
        int i = 0;
        while (i < vlen) {
            while (i < vlen && is_whitespace(v[i])) i++;
            int w0 = i;
            while (i < vlen && !is_whitespace(v[i])) i++;
            if (i > w0) b |= bloom_bit(ridx_hash32(&v[w0], i - w0));
        }
    }
    return b;
}

static int32_t g_cls_gen;   /* bumped once per style pass (rule_index_ensure) */

static int class_attr_contains(const struct dnode *nd, const char *cls, int clen) {
    SP_N(g_sp_clook, 1);
    int vlen;
    const char *v;
    if (nd->cgen == g_cls_gen) {           /* cached span, valid this pass */
        if (nd->cls_vlen <= 0) return 0;
        v = &E->tpool[nd->cls_voff];
        vlen = nd->cls_vlen;
    } else {
        v = node_attr(nd, "class", &vlen);
        if (!v) return 0;
    }
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
    SP_N(g_sp_partm, 1);
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
    for (int q = 0; q < p->nnot; q++) {    /* :not(<simple>) negations */
        int hit = 0;
        switch (p->not_kind[q]) {
        case NOT_CLASS:
            hit = class_attr_contains(nd, &E->csspool[p->not_off[q]],
                                      p->not_len[q]);
            break;
        case NOT_ID: {
            int vlen;
            const char *v = node_attr(nd, "id", &vlen);
            if (v && vlen == p->not_len[q]) {
                int k = 0;
                while (k < vlen && lc(v[k]) == E->csspool[p->not_off[q] + k])
                    k++;
                hit = (k == vlen);
            }
            break;
        }
        case NOT_TAG:
            hit = (nd->tag == p->not_off[q]);
            break;
        case NOT_ATTR: {
            char an[64];
            int l = p->not_len[q] < 63 ? p->not_len[q] : 63;
            for (int k = 0; k < l; k++) an[k] = E->csspool[p->not_off[q] + k];
            an[l] = 0;
            int vlen;
            hit = node_attr(nd, an, &vlen) != NULL;
            break;
        }
        }
        if (hit) return 0;
    }
    return 1;
}

/* Previous ELEMENT sibling of ni (text nodes skipped), -1 if none. */
static int prev_elem_sibling(int ni) {
    int parent = E->nodes[ni].parent;
    if (parent < 0) return -1;
    int prev = -1;
    for (int c = E->nodes[parent].first; c >= 0; c = E->nodes[c].next) {
        if (c == ni) return prev;
        if (E->nodes[c].tag != T_TEXT) prev = c;
    }
    return -1;
}

/* Right-to-left matching with ancestor backtracking. comb: 1 descendant,
 * 2 child, 3 adjacent sibling (+), 4 general sibling (~). */
static int match_upward(const struct cpart *parts, int pi, int ni) {
    if (!part_match(&parts[pi], ni)) return 0;
    if (pi == 0) return 1;
    int comb = parts[pi].comb;
    if (comb == 3) {
        int s = prev_elem_sibling(ni);
        return s >= 0 && match_upward(parts, pi - 1, s);
    }
    if (comb == 4) {
        for (int s = prev_elem_sibling(ni); s >= 0; s = prev_elem_sibling(s))
            if (match_upward(parts, pi - 1, s)) return 1;
        return 0;
    }
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
    /* Fast reject: every non-rightmost part must match SOME ancestor, so
     * if the ancestors' bloom is missing any of their keys the whole
     * upward walk is hopeless. This is what makes descendant selectors
     * affordable -- without it each candidate cost ~10 part_match calls
     * and ~7 class re-parses. */
    if (ru->anc_bloom) {
        int par = E->nodes[ni].parent;
        if (par < 0) return 0;                 /* needs an ancestor, has none */
        if ((E->nodes[par].bloom & ru->anc_bloom) != ru->anc_bloom) return 0;
    }
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
    st->min_w = -1;
    st->min_h = -1;
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
    st->radius = 0;                          /* decoration does not inherit */
    st->deco = -1;
    st->mask = -1;               /* mask-image is not inherited */
    st->has_tf = 0;
    st->txpx = st->typx = 0;
    st->txpct = st->typct = 0;
    st->scx = st->scy = 1000;                /* transform does not inherit */
    st->rot = 0;
    st->opa = 255;                           /* opacity does not inherit */
    st->po_pct = 0;
    st->webface = pst ? pst->webface : -1;   /* font-family inherits */
    st->anim_kf = -1;                        /* animation does not inherit */
    st->anim_dur = 0; st->anim_delay = 0;
    st->anim_iter = 0; st->anim_flags = 0;
    st->trans_mask = 0; st->trans_flags = 0; /* transition does not inherit */
    st->trans_dur = 0; st->trans_delay = 0;
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

/* Sized for design-token sheets: wikipedia's Codex peaks at 381 live
 * entries / 14 KiB (measured with -DVAR_PROF), youtube ships ~13k
 * custom-property declarations. Overflow drops definitions silently ->
 * every var() referencing them takes its fallback. */
#define VARSCOPE_MAX 4096
#define VARPOOL_CAP  (512 * 1024)
struct varent { int noff, nlen, voff, vlen; };    /* offsets into g_varpool */
static struct varent g_varscope[VARSCOPE_MAX];
static int  g_nvarscope;
static char g_varpool[VARPOOL_CAP];
static int  g_varpool_len;

#ifdef VAR_PROF
/* Per-style-pass custom-property accounting: drops (definitions lost to
 * a full scope table / pool), lookup hit/miss, high-water marks, and the
 * first few distinct names that missed (i.e. fell back). */
static int g_vp_drop_scope, g_vp_drop_pool, g_vp_hit, g_vp_miss;
static int g_vp_hwm_scope, g_vp_hwm_pool;
#define VP_MISSNAMES 12
static char g_vp_missname[VP_MISSNAMES][64];
static int  g_vp_nmissname;
static void vp_record_miss(const char *name, int nlen) {
    if (nlen > 63) nlen = 63;
    for (int i = 0; i < g_vp_nmissname; i++) {
        int k = 0;
        while (k < nlen && g_vp_missname[i][k] == name[k]) k++;
        if (k == nlen && g_vp_missname[i][k] == 0) return;
    }
    if (g_vp_nmissname >= VP_MISSNAMES) return;
    for (int i = 0; i < nlen; i++) g_vp_missname[g_vp_nmissname][i] = name[i];
    g_vp_missname[g_vp_nmissname][nlen] = 0;
    g_vp_nmissname++;
}
#endif

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
    if (g_nvarscope >= VARSCOPE_MAX) {
#ifdef VAR_PROF
        g_vp_drop_scope++;
#endif
        return;
    }
    if (nlen < 0) nlen = 0;
    if (vlen < 0) vlen = 0;
    /* `--x: initial` = guaranteed-invalid (css-variables-1): the
     * property counts as NOT set, and it SHADOWS outer definitions --
     * var(--x, fb) must take the fallback. The csstools light-dark()
     * polyfill (mdn et al) leans on exactly this to switch themes;
     * substituting the literal text "initial" leaked the dark branch
     * of every token. Stored as a vlen<0 sentinel. */
    const char *tv = val;
    int tn = vlen;
    while (tn > 0 && is_whitespace(tv[0])) { tv++; tn--; }
    while (tn > 0 && is_whitespace(tv[tn - 1])) tn--;
    int ginvalid = (tn == 7 && str_ncasecmp(tv, "initial", 7) == 0);
    if (ginvalid) vlen = 0;
    if (g_varpool_len + nlen + vlen > VARPOOL_CAP) {
#ifdef VAR_PROF
        g_vp_drop_pool++;
#endif
        return;
    }
#ifdef VAR_PROF
    if (g_nvarscope + 1 > g_vp_hwm_scope) g_vp_hwm_scope = g_nvarscope + 1;
    if (g_varpool_len + nlen + vlen > g_vp_hwm_pool)
        g_vp_hwm_pool = g_varpool_len + nlen + vlen;
#endif
    struct varent *e = &g_varscope[g_nvarscope++];
    e->noff = g_varpool_len;
    e->nlen = nlen;
    for (int i = 0; i < nlen; i++) g_varpool[g_varpool_len++] = name[i];
    e->voff = g_varpool_len;
    e->vlen = ginvalid ? -1 : vlen;
    for (int i = 0; i < vlen; i++) g_varpool[g_varpool_len++] = val[i];
}

/* Most-recent (highest-priority / nearest-scope) definition wins. */
static int var_lookup(const char *name, int nlen, const char **out, int *outlen) {
    for (int i = g_nvarscope - 1; i >= 0; i--) {
        struct varent *e = &g_varscope[i];
        if (e->nlen != nlen) continue;
        int k = 0;
        while (k < nlen && g_varpool[e->noff + k] == name[k]) k++;
        if (k == nlen) {
            if (e->vlen < 0) break;    /* guaranteed-invalid: counts as
                                          unset AND shadows outer defs */
#ifdef VAR_PROF
            g_vp_hit++;
#endif
            *out = &g_varpool[e->voff]; *outlen = e->vlen; return 1;
        }
    }
#ifdef VAR_PROF
    g_vp_miss++;
    vp_record_miss(name, nlen);
#endif
    return 0;
}

static int var_subst(const char *v, int n, char *out, int cap, int depth,
                     int *poison);

/* Expand every var(--name[, fallback]) in [v,v+n) into `out`. Returns
 * the written length. Recurses (depth-limited) so a var value may
 * itself use var().
 *
 * Invalid-at-computed-value-time (css-variables-1 §3): var() of an
 * UNSET (or guaranteed-invalid) property with NO fallback poisons the
 * whole containing value -- it does NOT expand to empty. A referenced
 * property whose own value poisons counts as invalid too, so the
 * REFERRER's fallback is taken. The csstools light-dark() polyfill is
 * built on exactly this: `--toggle: var(--scheme-light) <darkval>`
 * must collapse to invalid in light mode (--scheme-light: initial) so
 * that `var(--toggle, <lightval>)` yields the light value; expanding
 * to empty instead leaked <darkval> = mdn's black page. */
static int var_subst(const char *v, int n, char *out, int cap, int depth,
                     int *poison) {
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
            int sub = 0;
            if (depth < 16 && var_lookup(v + ns, ne - ns, &rv, &rl)) {
                int wrote = var_subst(rv, rl, out + o, cap - o, depth + 1,
                                      &sub);
                if (!sub) {
                    o += wrote;               /* clean expansion */
                    continue;
                }
                /* referenced value invalid: fall through to fallback
                 * (the partial bytes at out+o get overwritten) */
            }
            if (fb_s >= 0 && depth < 16) {
                sub = 0;
                o += var_subst(v + fb_s, fb_e - fb_s, out + o, cap - o,
                               depth + 1, &sub);
                if (sub) *poison = 1;         /* fallback itself invalid */
            } else {
                *poison = 1;                  /* unset + no fallback */
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

/* CSS light-dark(A, B): this engine renders the light scheme only, so
 * every occurrence collapses to A. Runs on the declaration value AFTER
 * var() substitution (either arm usually comes from custom props).
 * Paren-aware, so nested functions survive. */
static int val_has_lightdark(const char *v, int n) {
    return str_contains(v, n, "light-dark(", 11) >= 0;
}

static int lightdark_subst(const char *v, int n, char *out, int cap) {
    int o = 0, i = 0;
    while (i < n && o < cap - 1) {
        if (n - i >= 11 && str_ncasecmp(v + i, "light-dark(", 11) == 0) {
            i += 11;
            int par = 0;
            while (i < n && !(par == 0 && (v[i] == ',' || v[i] == ')'))) {
                if (v[i] == '(') par++;
                else if (v[i] == ')') par--;
                if (o < cap - 1) out[o++] = v[i];
                i++;
            }
            if (i < n && v[i] == ',') {          /* skip the dark arm */
                i++;
                par = 0;
                while (i < n && !(par == 0 && v[i] == ')')) {
                    if (v[i] == '(') par++;
                    else if (v[i] == ')') par--;
                    i++;
                }
            }
            if (i < n && v[i] == ')') i++;
            continue;
        }
        out[o++] = v[i++];
    }
    out[o] = 0;
    return o;
}

/* Apply one declaration to a computed style. Pass 0 applies only
 * font-size (so em elsewhere resolves against the final font); pass 1
 * applies everything else. */
/* Find-or-allocate this style's decoration slot (gradient/shadow). All
 * of a node's decls apply to the same `st`, so lazy alloc keyed off
 * st->deco groups them. Returns NULL when the pool is full. */
static struct deco *deco_of(struct cstyle *st) {
    if (st->deco >= 0 && st->deco < g_ndeco) return &g_deco[st->deco];
    if (g_ndeco >= DECO_MAX) return NULL;
    struct deco *d = &g_deco[g_ndeco];
    mem_zero(d, sizeof(*d));
    st->deco = (int16_t)g_ndeco++;
    return d;
}

/* Intern a mask-image URL (raw CSS value; resolved at fetch time).
 * Returns its g_masks index, deduped by exact URL so shared icons load
 * once. -1 if unstorable/full. */
static int mask_intern(const char *url, int len) {
    if (len <= 0 || len >= (int)sizeof(g_masks[0].url)) return -1;
    for (int i = 0; i < g_nmasks; i++) {
        if ((int)str_len(g_masks[i].url) != len) continue;
        int k = 0; while (k < len && g_masks[i].url[k] == url[k]) k++;
        if (k == len) return i;
    }
    if (g_nmasks >= MASK_MAX) return -1;
    struct maskimg *m = &g_masks[g_nmasks];
    for (int k = 0; k < len; k++) m->url[k] = url[k];
    m->url[len] = 0;
    m->alpha = NULL; m->w = m->h = 0; m->state = 0;
    return g_nmasks++;
}

/* Parse "linear-gradient([to <dir> | <deg>,] c0 [p0], c1 [p1], ...)"
 * into `d`. Returns 1 on success. Angles snap to the nearest axis
 * (v1: axis-aligned gradients). */
static int parse_linear_gradient(const char *v, int n, struct deco *d) {
    int radial = 0, conic = 0, hdr = 15;
    int lg = str_contains(v, n, "linear-gradient", 15);
    if (lg < 0) {
        lg = str_contains(v, n, "radial-gradient", 15);
        if (lg >= 0) radial = 1;
        else { lg = str_contains(v, n, "conic-gradient", 14);
               if (lg < 0) return 0; conic = 1; hdr = 14; }
    }
    int i = lg + hdr;
    while (i < n && v[i] != '(') i++;
    if (i >= n) return 0;
    i++;
    int e = i;
    int depth = 1;
    while (e < n && depth) { if (v[e] == '(') depth++; else if (v[e] == ')') depth--; if (depth) e++; }
    d->grad_dir = radial ? 4 : conic ? 5 : 0;   /* 4 radial, 5 conic */
    d->grad_n = 0;
    int any_pos = 0;
    /* split on top-level commas */
    int seg0 = i;
    int di = i;
    int first = 1;
    for (int p = i; p <= e && d->grad_n < GRAD_STOPS; p++) {
        if (p < e && v[p] == '(') { int dp = 1; p++; while (p < e && dp) { if (v[p]=='(') dp++; else if (v[p]==')') dp--; p++; } p--; continue; }
        if (p == e || v[p] == ',') {
            const char *s = v + seg0; int sl = p - seg0;
            while (sl > 0 && (*s == ' ' || *s == '\t')) { s++; sl--; }
            while (sl > 0 && (s[sl-1] == ' ' || s[sl-1] == '\t')) sl--;
            if (first) {
                first = 0;
                if (radial) {
                    /* skip a shape/size/position prefix (circle, ellipse,
                     * "at center", closest/farthest-corner/side) -- it's
                     * not a color stop. */
                    if (str_contains(s, sl, "circle", 6) >= 0 ||
                        str_contains(s, sl, "ellipse", 7) >= 0 ||
                        (sl >= 2 && str_ncasecmp(s, "at", 2) == 0) ||
                        str_contains(s, sl, "closest", 7) >= 0 ||
                        str_contains(s, sl, "farthest", 8) >= 0) {
                        seg0 = p + 1;
                        continue;
                    }
                    /* else it's the first color stop: fall through */
                }
                if (conic) {
                    /* skip a "from <angle>" / "at <position>" prefix. */
                    if ((sl >= 4 && str_ncasecmp(s, "from", 4) == 0) ||
                        (sl >= 2 && str_ncasecmp(s, "at", 2) == 0)) {
                        seg0 = p + 1;
                        continue;
                    }
                    /* else the first color stop: fall through */
                }
                if (sl >= 2 && str_ncasecmp(s, "to", 2) == 0) {
                    if (str_contains(s, sl, "right", 5) >= 0) d->grad_dir = 1;
                    else if (str_contains(s, sl, "top", 3) >= 0) d->grad_dir = 2;
                    else if (str_contains(s, sl, "left", 4) >= 0) d->grad_dir = 3;
                    else d->grad_dir = 0;
                    seg0 = p + 1;
                    continue;                /* direction consumed */
                }
                if (sl > 0 && (s[sl-1] == 'g' /* deg */ )) {
                    int deg = atoi_simple(s);
                    deg = ((deg % 360) + 360) % 360;
                    d->grad_dir = (deg >= 315 || deg < 45) ? 2 :   /* up */
                                  (deg < 135) ? 1 : (deg < 225) ? 0 : 3;
                    seg0 = p + 1;
                    continue;
                }
                /* no direction: this seg is the first color, fall through */
            }
            /* color [position]  (position: <pct>% or, for conic, <deg>deg) */
            uint32_t c;
            int cl = sl;
            int pct = -1;
            int sp = sl;
            while (sp > 0 && s[sp-1] != ' ') sp--;
            if (sp > 0 && sl - sp > 0) {
                if (s[sl-1] == '%') { pct = atoi_simple(s + sp); }
                else if (sl - sp >= 4 && str_ncasecmp(s + sl - 3, "deg", 3) == 0) {
                    int deg = atoi_simple(s + sp);
                    deg = ((deg % 360) + 360) % 360;
                    pct = deg * 100 / 360;      /* angle -> fraction of turn */
                }
                if (pct >= 0) { cl = sp; while (cl > 0 && s[cl-1] == ' ') cl--; }
            }
            if (css_color_tok(s, cl, &c)) {
                int k = d->grad_n++;
                d->grad_col[k] = c | 0xFF000000u;
                if (pct >= 0 && pct <= 100) { d->grad_pos[k] = (uint8_t)pct; any_pos = 1; }
                else d->grad_pos[k] = (uint8_t)(d->grad_n == 1 ? 0 : 100);
            }
            seg0 = p + 1;
        }
    }
    /* No explicit positions: distribute stops evenly (0..100). */
    if (!any_pos && d->grad_n >= 2)
        for (int k = 0; k < d->grad_n; k++)
            d->grad_pos[k] = (uint8_t)(k * 100 / (d->grad_n - 1));
    if (d->grad_n >= 2) { d->has_grad = 1; return 1; }
    return 0;
}

static void st_apply(struct cstyle *st, const struct cstyle *pst,
                     const struct cdecl *d, int pass) {
    if (d->prop == CP_VAR) return;        /* collected separately */
    const char *v = &E->csspool[d->voff];
    int n = d->vlen;
    char vbuf[1024];
    if (val_has_var(v, n)) {              /* resolve var() before parsing */
        int poison = 0;
        n = var_subst(v, n, vbuf, sizeof(vbuf), 0, &poison);
        if (poison) return;   /* invalid at computed-value time: the
                                 declaration does not apply at all */
        v = vbuf;
    }
    char ldbuf[1024];
    if (val_has_lightdark(v, n)) {        /* light-dark(A,B) -> A */
        n = lightdark_subst(v, n, ldbuf, sizeof(ldbuf));
        v = ldbuf;
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
        if (str_contains(v, n, "linear-gradient", 15) >= 0 ||
            str_contains(v, n, "radial-gradient", 15) >= 0 ||
            str_contains(v, n, "conic-gradient", 14) >= 0) {
            struct deco *dc = deco_of(st);
            if (dc && parse_linear_gradient(v, n, dc)) {
                /* fallback solid = first stop (also the color used when
                 * the gradient can't paint) */
                st->bg = dc->grad_col[0] | 0xFF000000u;
                break;
            }
        }
        uint32_t c;
        while (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "none")) { st->bg = 0; break; }
            /* currentColor: the element's own text color (Codex icons do
             * `background-color: currentColor` so the icon takes the text
             * color). Resolve against st->color, already applied for this
             * node in the first cascade pass. */
            if (tok_is(&t, "currentcolor")) {
                st->bg = st->color | 0xFF000000u;
                if (st->deco >= 0) g_deco[st->deco].has_grad = 0;
                break;
            }
            if (css_color_tok(t.s, t.len, &c)) {
                st->bg = ((c >> 24) >= 128) ? (c | 0xFF000000u) : 0;
                /* a solid color clears any prior gradient on this node */
                if (st->deco >= 0) g_deco[st->deco].has_grad = 0;
                break;
            }
        }
        break;
    }
    case CP_BORDER_RADIUS: {
        /* 1-4 space-separated values: TL [TR [BR [BL]]] (CSS fill order).
         * LK_PX == 0, so test the return code explicitly. 255 = sentinel
         * "half the shorter side" so border-radius:50% renders circles. */
        uint8_t rv[4]; int rn = 0;
        struct vtok rt; int rp = 0;
        while (rn < 4 && vtok_next(v, n, &rp, &rt)) {
            int r = 0;
            int k = css_len_tok(rt.s, rt.len, st->px, &r);
            if (k == LK_PCT) rv[rn++] = 255;
            else if (k == LK_PX) { if (r < 0) r = 0; if (r > 250) r = 250; rv[rn++] = (uint8_t)r; }
            else break;
        }
        if (rn == 0) break;
        /* CSS fill: 1->all, 2->(TL/BR, TR/BL), 3->(TL, TR/BL, BR). */
        uint8_t tl=rv[0], tr=rv[0], br=rv[0], bl=rv[0];
        if (rn == 2) { tr = bl = rv[1]; }
        else if (rn == 3) { tr = bl = rv[1]; br = rv[2]; }
        else if (rn == 4) { tr = rv[1]; br = rv[2]; bl = rv[3]; }
        st->radius = tl;                          /* uniform-path fallback */
        if (rn >= 2 && !(tl == tr && tr == br && br == bl)) {
            struct deco *dc = deco_of(st);
            if (dc) { dc->has_r4 = 1; dc->r4[0]=tl; dc->r4[1]=tr; dc->r4[2]=br; dc->r4[3]=bl; }
        }
        break;
    }
    case CP_BOX_SHADOW: {
        if (str_contains(v, n, "none", 4) >= 0 && n <= 6) break;
        struct deco *dc = deco_of(st);
        if (!dc) break;
        /* "[inset] <dx> <dy> [blur] [spread] <color>" -- inset ignored */
        int nums[4], nn = 0;
        int p2 = 0;
        uint32_t col = 0x40000000u;           /* default ~25% black */
        int have_col = 0;
        struct vtok tk;
        while (vtok_next(v, n, &p2, &tk) && nn < 4) {
            if (tok_is(&tk, "inset")) continue;
            uint32_t c;
            int px2, k;
            if (css_color_tok(tk.s, tk.len, &c)) { col = c; have_col = 1; }
            else if ((k = css_len_tok(tk.s, tk.len, st->px, &px2)) == LK_PX)
                nums[nn++] = px2;         /* LK_PX == 0: test explicitly */
        }
        /* a trailing color after numbers */
        if (!have_col) {
            int p3 = 0; struct vtok t3; uint32_t c;
            while (vtok_next(v, n, &p3, &t3))
                if (css_color_tok(t3.s, t3.len, &c)) { col = c; break; }
        }
        if (nn >= 2) {
            dc->has_shadow = 1;
            dc->sh_dx = (int16_t)nums[0];
            dc->sh_dy = (int16_t)nums[1];
            dc->sh_blur = (uint8_t)(nn >= 3 ? (nums[2] < 0 ? 0 : nums[2] > 80 ? 80 : nums[2]) : 0);
            dc->sh_spread = (int16_t)(nn >= 4 ? nums[3] : 0);
            /* ensure a visible alpha even if the color was opaque-looking */
            if ((col >> 24) == 0) col |= 0x40000000u;
            dc->sh_col = col;
        }
        break;
    }
    case CP_MASK: {
        /* mask-image / -webkit-mask-image: url(<icon.svg>) | none.
         * Codex/Primer render UI icons as an SVG mask over a colored box;
         * we intern the URL and stencil-fill at paint time. Only url() is
         * handled (gradient masks / mask-mode:luminance are out of
         * scope). "none" clears any inherited mask. */
        if (str_contains(v, n, "none", 4) >= 0 && n <= 6) { st->mask = -1; break; }
        int u = str_contains(v, n, "url(", 4);
        if (u < 0) break;
        int a = u + 4;
        while (a < n && v[a] == ' ') a++;
        /* If the url() content is quoted, read to the MATCHING quote --
         * a data: URI legitimately contains internal quotes (SVG attrs),
         * so we must not stop at those. Unquoted: read to ')'. */
        int b;
        if (a < n && (v[a] == '"' || v[a] == '\'')) {
            char q = v[a++];
            b = a;
            while (b < n && v[b] != q) b++;
        } else {
            b = a;
            while (b < n && v[b] != ')' && v[b] != ' ') b++;
        }
        if (b > a) {
            int mi = mask_intern(v + a, b - a);
            if (mi >= 0) st->mask = (int16_t)mi;
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
    case CP_MINW:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            int k = css_len_tok(t.s, t.len, st->px, &px);
            if (k == LK_PX && px >= 0) st->min_w = px;
            else if (tok_is(&t, "auto") || tok_is(&t, "none")) st->min_w = -1;
        }
        break;
    case CP_MINH:
        if (vtok_next(v, n, &pos, &t)) {
            int px;
            int k = css_len_tok(t.s, t.len, st->px, &px);
            if (k == LK_PX && px >= 0) st->min_h = px;
            else if (tok_is(&t, "auto") || tok_is(&t, "none")) st->min_h = -1;
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
        /* translate()/translateX/Y (px or %), scale()/scaleX/scaleY,
         * rotate(deg|turn) -- every function in the list is scanned.
         * Translate bakes at layout; scale/rotate (+ opacity) paint
         * through a compositing layer (slice 3). matrix()/skew() are
         * ignored (no wrong result, just no-op). */
        st->has_tf = 0; st->txpx = 0; st->typx = 0; st->txpct = 0; st->typct = 0;
        st->scx = st->scy = 1000; st->rot = 0;
        css_parse_transform(v, n, st->px, &st->has_tf,
                            &st->txpx, &st->typx, &st->txpct, &st->typct,
                            &st->scx, &st->scy, &st->rot, NULL, NULL);
        break;
    }
    case CP_OPACITY: {
        int m = css_frac_1000(v, n);
        if (m < 0) m = 0;
        if (m > 1000) m = 1000;
        st->opa = (uint8_t)(m * 255 / 1000);
        break;
    }
    case CP_ANIMATION: {
        /* animation: <name> <duration> [timing] [delay] [iter] [direction] */
        st->anim_kf = -1; st->anim_dur = 0; st->anim_delay = 0;
        st->anim_iter = 1; st->anim_flags = 0;
        int timeseen = 0, i = 0;
        while (i < n) {
            while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
            int t0 = i;
            while (i < n && v[i] != ' ' && v[i] != '\t' && v[i] != ',') i++;
            int tl = i - t0;
            if (tl <= 0) break;
            const char *tk = v + t0;
            if (tk[0] >= '0' && tk[0] <= '9') {
                if (tl >= 2 && lc(tk[tl - 1]) == 's') {           /* <time> */
                    int ms = css_time_ms(tk, tl);
                    if (!timeseen) { st->anim_dur = (uint16_t)ms; timeseen = 1; }
                    else st->anim_delay = (uint16_t)ms;
                } else {                                          /* iteration */
                    int x = 0;
                    for (int k = 0; k < tl && tk[k] >= '0' && tk[k] <= '9'; k++)
                        x = x * 10 + (tk[k] - '0');
                    st->anim_iter = (uint8_t)x;
                }
            } else if (tl == 8 && str_ncasecmp(tk, "infinite", 8) == 0) {
                st->anim_iter = 0;
            } else if (tl == 9 && str_ncasecmp(tk, "alternate", 9) == 0) {
                st->anim_flags |= 1;
            } else if (tl >= 4 && str_ncasecmp(tk, "ease", 4) == 0) {
                st->anim_flags |= 2;
            } else if ((tl == 6 && str_ncasecmp(tk, "linear", 6) == 0) ||
                       (tl == 6 && str_ncasecmp(tk, "normal", 6) == 0)) {
                /* linear timing / normal direction: nothing to set */
            } else {
                int kf = kf_find(tk, tl);
                if (kf >= 0) st->anim_kf = (int16_t)kf;
            }
        }
        break;
    }
    case CP_ANIM_NAME:   st->anim_kf = (int16_t)kf_find(v, n); break;
    case CP_ANIM_DUR:    st->anim_dur = (uint16_t)css_time_ms(v, n); break;
    case CP_ANIM_DELAY:  st->anim_delay = (uint16_t)css_time_ms(v, n); break;
    case CP_ANIM_ITER:
        if (str_contains(v, n, "infinite", 8) >= 0) st->anim_iter = 0;
        else { int x = 0, k = 0; while (k < n && (v[k] < '0' || v[k] > '9')) k++;
               for (; k < n && v[k] >= '0' && v[k] <= '9'; k++) x = x * 10 + (v[k] - '0');
               st->anim_iter = (uint8_t)x; }
        break;
    case CP_ANIM_DIR:
        if (str_contains(v, n, "alternate", 9) >= 0) st->anim_flags |= 1;
        break;
    case CP_ANIM_TIMING:
        if (str_contains(v, n, "ease", 4) >= 0) st->anim_flags |= 2;
        break;
    case CP_TRANSITION: {
        /* transition: <property> <duration> [timing] [delay] (comma lists;
         * v1 folds all entries into one mask + one dur/delay/timing). */
        st->trans_mask = 0; st->trans_dur = 0; st->trans_delay = 0; st->trans_flags = 0;
        int timeseen = 0, i = 0;
        while (i < n) {
            while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
            int t0 = i;
            while (i < n && v[i] != ' ' && v[i] != '\t' && v[i] != ',') i++;
            int tl = i - t0;
            if (tl <= 0) break;
            const char *tk = v + t0;
            if (tk[0] >= '0' && tk[0] <= '9' && tl >= 2 && lc(tk[tl - 1]) == 's') {
                int ms = css_time_ms(tk, tl);
                if (!timeseen) { st->trans_dur = (uint16_t)ms; timeseen = 1; }
                else st->trans_delay = (uint16_t)ms;
            } else if (tl >= 4 && str_ncasecmp(tk, "ease", 4) == 0) {
                st->trans_flags |= 2;
            } else if ((tl == 6 && str_ncasecmp(tk, "linear", 6) == 0) ||
                       (tl == 4 && str_ncasecmp(tk, "none", 4) == 0)) {
                /* linear timing / no property */
            } else {
                st->trans_mask |= trans_prop_bit(tk, tl);
            }
        }
        if (st->trans_mask == 0 && st->trans_dur > 0) st->trans_mask = 15; /* bare time -> all */
        break;
    }
    case CP_TRANS_PROP: {
        st->trans_mask = 0;
        int i = 0;
        while (i < n) {
            while (i < n && (v[i] == ' ' || v[i] == ',')) i++;
            int t0 = i;
            while (i < n && v[i] != ' ' && v[i] != ',') i++;
            if (i > t0) st->trans_mask |= trans_prop_bit(v + t0, i - t0);
        }
        break;
    }
    case CP_TRANS_DUR:
        st->trans_dur = (uint16_t)css_time_ms(v, n);
        if (!st->trans_mask) st->trans_mask = 15;
        break;
    case CP_TRANS_DELAY:
        st->trans_delay = (uint16_t)css_time_ms(v, n);
        break;
    case CP_TRANS_TIMING:
        if (str_contains(v, n, "ease", 4) >= 0) st->trans_flags |= 2;
        break;
    case CP_POSITION:
        if (vtok_next(v, n, &pos, &t)) {
            if (tok_is(&t, "relative")) st->pos = 1;
            else if (tok_is(&t, "absolute")) st->pos = 2;
            else if (tok_is(&t, "fixed")) st->pos = 3;
            else if (tok_is(&t, "sticky")) st->pos = 4;   /* scroll-pinned */
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



/* ---- Cascade rule index --------------------------------------------
 * The cascade used to test EVERY rule against EVERY node: O(nodes x
 * rules). That is fine for a hand-written page and hopeless for a real
 * one -- youtube.com ships a 3.4 MiB sheet (~10k rules), so one style
 * pass ran millions of selector matches (750 ms measured, every pass).
 *
 * A rule can only match a node if its RIGHTMOST compound selector
 * matches it, and that compound nearly always demands a specific id,
 * class or tag. So bucket each rule under that key; a node then tests
 * only the rules that could possibly match it (what Blink does).
 * Selectors with no usable key ("*", "[attr]") land in a small
 * universal bucket that every node walks.
 *
 * This only ever skips rules that CANNOT match -- sel_match_rule still
 * runs on every candidate, and M[] is sorted by (key, order) which is
 * unique per rule, so the resulting match set is identical to the
 * linear scan's. -DCSS_VERIFY cross-checks that on every node. */
static void rule_index_build(void) {
    for (int i = 0; i < RIDX_BUCKETS; i++) {
        E->idx_id[i] = -1;
        E->idx_cls[i] = -1;
    }
    for (int i = 0; i < T_NTAGS; i++) E->idx_tag[i] = -1;
    E->idx_uni = -1;
    /* walk backwards + prepend so each chain comes out in ascending
     * rule order (matches the old scan's iteration order) */
    for (int r = E->nrules - 1; r >= 0; r--) {
        struct crule *ru = &E->rules[r];
        /* bits the ancestors must carry (all parts but the rightmost).
         * A part joined through a SIBLING combinator (+/~) is NOT an
         * ancestor -- once one appears (right-to-left), stop demanding
         * bits: the bloom must stay false-positive-only. */
        ru->anc_bloom = 0;
        int pure_anc = 1;
        for (int k = ru->nparts - 1; k >= 1; k--) {
            if (E->parts[ru->part0 + k].comb >= 3) pure_anc = 0;
            if (pure_anc)
                ru->anc_bloom |= part_key_bits(&E->parts[ru->part0 + k - 1]);
        }
        int32_t *head = &E->idx_uni;
        if (ru->nparts > 0) {
            const struct cpart *last = &E->parts[ru->part0 + ru->nparts - 1];
            if (last->id_len > 0)
                head = &E->idx_id[ridx_hash(&E->csspool[last->id_off],
                                            last->id_len)];
            else if (last->nclass > 0)
                head = &E->idx_cls[ridx_hash(&E->csspool[last->cls_off[0]],
                                             last->cls_len[0])];
            else if (last->tag >= 0 && last->tag < T_NTAGS)
                head = &E->idx_tag[last->tag];
        }
        E->idx_next[r] = *head;
        *head = (int32_t)r;
#ifdef STYLE_PROF
        if (head == &E->idx_uni) g_sp_uni++;   /* every node walks these */
#endif
    }
    E->idx_nrules = E->nrules;
}

static void rule_index_ensure(void) {
    if (E->idx_nrules != E->nrules) rule_index_build();
    /* New style pass: invalidate every node's cached class span. A bump
     * (not per-node clears) so it is O(1); a stale cgen simply misses. */
    g_cls_gen++;
}

/* Test one candidate and keep M[] sorted by (origin, specificity,
 * source order). Extracted so the index and the -DCSS_VERIFY linear
 * scan share identical semantics, including the MATCH_MAX cutoff. */
static void style_try_rule(int r, int ni, struct smatch *M, int *nm) {
    SP_N(g_sp_cand, 1);
    const struct crule *ru = &E->rules[r];
    /* shadow scoping: author rules only match inside the tree that
     * declared them (document rules stop at shadow boundaries, a
     * shadow tree's rules stay inside it). UA rules apply anywhere. */
    if (ru->origin && ru->scope != g_style_scope) return;
    if (!sel_match_rule(ru, ni)) return;
    if (*nm >= MATCH_MAX) return;
    uint32_t key = ((uint32_t)ru->origin << 16) | ru->spec;
    int k = (*nm)++;
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

/* The pre-index behaviour: walk every rule in source order. */
static int style_collect_linear(int ni, struct smatch *M) {
    int nm = 0;
    for (int r = 0; r < E->nrules; r++) style_try_rule(r, ni, M, &nm);
    return nm;
}

/* Gather every rule that could match `ni`: universal + its tag bucket +
 * its id bucket + one bucket per class. */
static int style_collect(int ni, struct smatch *M) {
    /* Below a few hundred rules the linear scan actually wins: rules[]
     * is a tight sequential array that sits in cache, while the index
     * hashes the node's id/class and then chases pointers through a
     * 128 KiB next[]. Measured on the built-in home page (118 rules):
     * 43 ms linear vs 79 ms indexed. The index is for real sheets --
     * youtube.com (10.7k rules): 750 ms linear vs 31 ms indexed. */
    if (E->nrules < RIDX_MIN_RULES) return style_collect_linear(ni, M);

    int nm = 0;
    const struct dnode *nd = &E->nodes[ni];
    for (int r = E->idx_uni; r >= 0; r = E->idx_next[r])
        style_try_rule(r, ni, M, &nm);
    if (nd->tag >= 0 && nd->tag < T_NTAGS)
        for (int r = E->idx_tag[nd->tag]; r >= 0; r = E->idx_next[r])
            style_try_rule(r, ni, M, &nm);
    int vlen;
    const char *v = node_attr(nd, "id", &vlen);
    if (v && vlen > 0)
        for (int r = E->idx_id[ridx_hash(v, vlen)]; r >= 0; r = E->idx_next[r])
            style_try_rule(r, ni, M, &nm);
    v = node_attr(nd, "class", &vlen);
    if (v && vlen > 0) {
        /* Two classes can hash to the SAME bucket; walking it twice
         * would add its rules twice, so remember which we've done. */
        uint16_t seen[32];
        int nseen = 0, i = 0;
        while (i < vlen) {
            while (i < vlen && is_whitespace(v[i])) i++;
            int w0 = i;
            while (i < vlen && !is_whitespace(v[i])) i++;
            if (i == w0) break;
            uint32_t b = ridx_hash(&v[w0], i - w0);
            int dup = 0;
            for (int k = 0; k < nseen; k++)
                if (seen[k] == (uint16_t)b) { dup = 1; break; }
            if (dup) continue;
            if (nseen < 32) seen[nseen++] = (uint16_t)b;
            for (int r = E->idx_cls[b]; r >= 0; r = E->idx_next[r])
                style_try_rule(r, ni, M, &nm);
        }
    }
    return nm;
}

#ifdef CSS_VERIFY
static int msg_append(char *dst, int pos, int max, const char *s);
static int msg_append_int(char *dst, int pos, int max, int v);

static void style_verify(int ni, const struct smatch *M, int nm) {
    struct smatch L[MATCH_MAX];
    int nl = style_collect_linear(ni, L);
    int bad = (nl != nm);
    for (int k = 0; !bad && k < nl; k++)
        if (L[k].order != M[k].order || L[k].key != M[k].key) bad = 1;
    if (bad) {
        char m[128]; int p = 0;
        p = msg_append(m, p, sizeof(m), "[cssverify] MISMATCH node=");
        p = msg_append_int(m, p, sizeof(m), ni);
        p = msg_append(m, p, sizeof(m), " idx=");
        p = msg_append_int(m, p, sizeof(m), nm);
        p = msg_append(m, p, sizeof(m), " linear=");
        p = msg_append_int(m, p, sizeof(m), nl);
        p = msg_append(m, p, sizeof(m), "\n");
        sys_write(1, m, p);
    }
}
#endif

static void style_node(int ni, const struct cstyle *pst) {
    struct dnode *nd = &E->nodes[ni];
    if (nd->tag == T_TEXT) {
        nd->st = *pst;                    /* text renders in parent style */
        /* light-DOM text of a shadow host is not in the composed tree */
        if (nd->parent >= 0 && E->nodes[nd->parent].shadow >= 0)
            nd->st.disp = D_NONE;
        return;
    }
    /* entering a shadow root: its subtree styles in its own scope */
    int save_scope = g_style_scope;
    if (nd->parent >= 0 && E->nodes[nd->parent].shadow == ni)
        g_style_scope = ni;
    SP_T0(t_pres);
    /* self+ancestor bloom for the fast reject. Top-down, so the
     * parent's is already final. Also cache the class span here so the
     * matcher stops re-finding it on every candidate. */
    {
        int cvl; const char *cv = node_attr(nd, "class", &cvl);
        nd->cls_voff = cv ? (int32_t)(cv - E->tpool) : 0;
        nd->cls_vlen = cv ? (int16_t)cvl : 0;
        nd->cgen = g_cls_gen;
    }
    nd->bloom = (nd->parent >= 0 ? E->nodes[nd->parent].bloom : 0) |
                node_self_bits(ni);
    struct cstyle st;
    st_init(&st, pst);
    /* Tables reset inherited text-align: <center> (and align=center
     * ancestors) center the table BOX in real browsers, not the text
     * inside its cells (HN's whole page sits in <center>). Explicit
     * CSS/attrs on the table or its cells still apply below. */
    if (nd->tag == T_TABLE) st.talign = 0;
    /* Custom elements (T_UNK with a stashed name) default to
     * display:block. Spec says inline, but this engine has no
     * block-in-inline splitting and virtually every real component's
     * first style line is :host{display:block} anyway. Pre-cascade:
     * author CSS still overrides. */
    if (nd->tag == T_UNK) {
        char cet[49];
        if (node_attr_str(nd, "__tag", cet, sizeof(cet))) st.disp = D_BLOCK;
    }
    /* <slot> hosts projected block content; same deviation. */
    if (nd->tag == T_SLOT) st.disp = D_BLOCK;
    /* Global `hidden` attribute = UA display:none (github alone ships
     * 25 of them -- session flashes, template stashes). Pre-cascade, so
     * an author display: rule still overrides, matching real UAs. */
    {
        int hl;
        if (node_attr(nd, "hidden", &hl)) st.disp = D_NONE;
    }
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
    SP_T1(g_sp_pres, t_pres);
    SP_T0(t_match);
    int nm = style_collect(ni, M);
    SP_T1(g_sp_match, t_match);
    SP_N(g_sp_nodes, 1); SP_N(g_sp_matches, nm);
#ifdef CSS_VERIFY
    style_verify(ni, M, nm);
#endif
    /* inline style="" -> transient decls (rolled back afterwards) */
    SP_T0(t_inline);
    int inl0 = E->ndecls, cp0 = E->csspool_len;
    int vlen;
    const char *sv = node_attr(nd, "style", &vlen);
    if (sv && vlen > 0) {
        struct ccur c = { sv, 0, vlen };
        css_parse_decls(&c);
    }
    int inlN = E->ndecls - inl0;
    SP_T1(g_sp_inline, t_inline);
    SP_T0(t_var);
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
    SP_T1(g_sp_var, t_var);
    SP_T0(t_apply);
    for (int m = 0; m < nm; m++) SP_N(g_sp_decls, M[m].ndecl);
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
    SP_T1(g_sp_apply, t_apply);
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
    /* Web components (shadow-dom arc): <template> content is inert --
     * never rendered until cloned out. display:none makes layout skip
     * the whole subtree. */
    if (nd->tag == T_TEMPLATE) st.disp = D_NONE;
    /* Shadow host's light children are not in the composed tree: only
     * the shadow root renders (slots project them back in slice 3). */
    if (nd->parent >= 0 && E->nodes[nd->parent].shadow >= 0 &&
        E->nodes[nd->parent].shadow != ni)
        st.disp = D_NONE;
    nd->st = st;
    if (nd->tag == T_TEMPLATE) {          /* inert: children keep no style */
        g_nvarscope = var_mark;
        g_varpool_len = varpool_mark;
        g_style_scope = save_scope;
        return;
    }
    for (int c = nd->first; c >= 0; c = E->nodes[c].next)
        style_node(c, &nd->st);
    /* pop this node's custom properties as we leave its subtree */
    g_nvarscope = var_mark;
    g_varpool_len = varpool_mark;
    g_style_scope = save_scope;
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

/* Provisionally-laid subtrees (inline-blocks, floats) shift their items
 * into place afterwards -- clips created inside MUST shift with them or
 * they stay at the provisional origin (y ~ 1<<20) and cull everything
 * they clip (wikipedia's masked icons: .cdx-button{overflow:hidden} is
 * an inline-flex whose interior clip never moved). The interior is laid
 * with g_cur_clip = 0, so after the shift each new clip re-intersects
 * the outer clip here, in REAL coordinates. */
static void clips_shift(int c0, int dx, int dy, int outer) {
    for (int c = c0; c < g_nclips; c++) {
        struct cliprect *cl = &g_clips[c];
        cl->x += dx;
        cl->y += dy;
        if (outer > 0) {
            const struct cliprect *oc = &g_clips[outer];
            if (oc->x > cl->x) { cl->w -= oc->x - cl->x; cl->x = oc->x; }
            if (cl->x + cl->w > oc->x + oc->w) cl->w = oc->x + oc->w - cl->x;
            if (oc->y > cl->y) { cl->h -= oc->y - cl->y; cl->y = oc->y; }
            if (cl->y + cl->h > oc->y + oc->h) cl->h = oc->y + oc->h - cl->y;
            if (cl->w < 0) cl->w = 0;
            if (cl->h < 0) cl->h = 0;
        }
    }
}

static struct ditem *item_new(int kind) {
    if (E->nitems >= ITEM_MAX) return NULL;
    struct ditem *it = &E->items[E->nitems++];
    mem_zero(it, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->link = it->field = it->img = -1;
    it->node = -1;
    it->deco = -1;                         /* no gradient/shadow by default */
    it->mask = -1;
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
/* Place one color emoji (a codepoint sequence -- a single emoji or a ZWJ
 * ligature) as an atomic box on the inline line. Its DI_EMOJI item's `off`
 * indexes the per-layout sequence pool. Sized ~1em, advance from the
 * rendered glyph. */
static void ic_emoji(struct ictx *ic, const int *cps, int n, const struct istyle *is) {
    int px = is->px;
    struct emoji_glyph *g = emoji_get(cps, n, px);
    int h = px, w = px;
    if (g) { h = g->h; w = g->adv; }
    for (int guard = 0; guard < 64; guard++) {
        if (ic->x > ic->lx0 && ic->x + w > ic->lx1) { ic_break(ic, is->lh); continue; }
        break;
    }
    ic->pend_sp = 0;
    int si = emoji_seq_add(cps, n);
    if (si < 0) return;
    struct ditem *it = item_new(DI_EMOJI);
    if (!it) return;
    it->x = ic->x;
    it->y = ic->y;
    it->w = w;
    it->h = h;
    it->off = si;                     /* index into the emoji sequence pool */
    it->px = (uint8_t)px;
    it->link = (int16_t)is->link;
    it->node = is->node;
    it->face = 0;
    ic->x += w;
    if (h + 2 > ic->line_h) ic->line_h = h + 2;
    ic->citem = -1;
}

static void ic_word(struct ictx *ic, const char *w, int wl, const struct istyle *is) {
    /* Split out color-emoji codepoints as atomic glyphs (the surrounding
     * text lays out normally). Cheap for ASCII-heavy text: emoji_is gates
     * on cp >= 0x2000 before touching the font. */
    for (int k = 0; k < wl; ) {
        int adv = 1;
        unsigned int cp = emoji_next_cp(w + k, wl - k, &adv);
        if (cp >= 0x2000 && emoji_is(cp)) {
            if (k > 0) ic_word(ic, w, k, is);          /* text before */
            /* Gather the run of codepoints from here (emoji + ZWJ + VS)
             * and let the font's GSUB tell us the longest ZWJ ligature. */
            int cps[EMOJI_SEQ_CPS]; int ncp = 0, blen[EMOJI_SEQ_CPS];
            int p = k;
            while (p < wl && ncp < EMOJI_SEQ_CPS) {
                int a2 = 1;
                unsigned int c2 = emoji_next_cp(w + p, wl - p, &a2);
                /* keep emoji, ZWJ, and variation selectors in the run */
                if (!(emoji_is(c2) || c2 == 0x200D || c2 == 0xFE0F || c2 == 0xFE0E))
                    break;
                cps[ncp] = (int)c2; blen[ncp] = a2; ncp++;
                p += a2;
            }
            toby_emoji_t *ef = emoji_font();
            int seqn = ef ? toby_emoji_seq_len(ef, cps, ncp) : 0;
            int after;
            if (seqn >= 2) {                            /* a ZWJ ligature */
                ic_emoji(ic, cps, seqn, is);
                after = k; for (int i = 0; i < seqn; i++) after += blen[i];
            } else {                                    /* a single emoji */
                int one[1] = { (int)cp };
                ic_emoji(ic, one, 1, is);
                after = k + adv;
                /* swallow a trailing variation selector */
                while (after < wl) {
                    int a3 = 1;
                    unsigned int c3 = emoji_next_cp(w + after, wl - after, &a3);
                    if (c3 == 0xFE0F || c3 == 0xFE0E) after += a3; else break;
                }
            }
            if (after < wl) ic_word(ic, w + after, wl - after, is);
            return;
        }
        if (cp == 0x200D || cp == 0xFE0F || cp == 0xFE0E) {
            /* standalone joiner/variation selector: zero-width, drop it */
            if (k > 0) ic_word(ic, w, k, is);
            if (k + adv < wl) ic_word(ic, w + k + adv, wl - k - adv, is);
            return;
        }
        k += adv;
    }
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
static void ic_inline_block(struct ictx *ic, int ni, const struct istyle *is);

static void inl_walk(struct ictx *ic, int ni, const struct istyle *is) {
    struct dnode *nd = &E->nodes[ni];
    if (nd->tag == T_TEXT) {
        if (is->pre) inl_text_pre(ic, &E->tpool[nd->toff], nd->tlen, is);
        else inl_text_norm(ic, &E->tpool[nd->toff], nd->tlen, is);
        return;
    }
    const struct cstyle *st = &nd->st;
    if (st->disp == D_NONE) return;
    if (st->pos == 2 || st->pos == 3) {     /* absolute/fixed leaves flow
                                               (sticky=4 stays in flow) */
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
    if (st->disp == D_INLBLOCK) {          /* atomic block box on the line */
        ic_inline_block(ic, ni, &cs);
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
    int fc0 = g_nclips, fsave_cc = g_cur_clip;
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
        fc0 = g_nclips;                    /* interior clips: no outer in
                                              force, re-intersect post-shift */
        fsave_cc = g_cur_clip;
        g_cur_clip = 0;
        int bot = lay_block(ni, 0, lay_cw, prov, link, inbg);
        g_cur_clip = fsave_cc;
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
            if (st->min_w >= 0 && cwid < st->min_w) cwid = st->min_w;
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
    /* Measure passes place right floats at the LEFT: intrinsic width is
     * direction-agnostic (a float contributes its width, not its
     * position), and lx1-mbw against the provisional 100000px width put
     * items at x~100000 -> items_extent exploded -> any container with
     * a float:right child measured "infinitely" wide and wrapped its
     * whole flex line (wikipedia's header stacked on .search-toggle). */
    int dx = (st->flt == 1 || g_flt_freeze) ? lx0 : lx1 - mbw;
    if (dx < cx) dx = cx;
    int dy = fy - laytop;
    for (int i = i0; i < E->nitems; i++) {
        E->items[i].x += dx;
        E->items[i].y += dy;
        if (fsave_cc && E->items[i].clip == 0)  /* re-attach the outer clip */
            E->items[i].clip = (uint8_t)fsave_cc;
    }
    nstamp_shift(s0, g_nnst, dx, dy);
    clips_shift(fc0, dx, dy, fsave_cc);
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

/* display:inline-block -- an atomic block box that flows on the inline
 * line. Shrink-to-fit width when `width` is auto (measured with floats
 * frozen, like lay_float); otherwise the explicit width. Lay the box at a
 * provisional origin far below real content (so its interior line layout
 * never consults real floats), then shift the item range onto the line at
 * the inline cursor. Top-aligned (no baseline shift). Establishes its own
 * BFC: interior floats are contained (not registered with the parent). */
static void ic_inline_block(struct ictx *ic, int ni, const struct istyle *is) {
    struct dnode *nd = &E->nodes[ni];
    struct cstyle *st = &nd->st;
    int ml = st->m[3] == M_AUTO ? 0 : st->m[3];
    int mr = st->m[1] == M_AUTO ? 0 : st->m[1];
    int mt = st->m[0] == M_AUTO ? 0 : st->m[0];
    int mb = st->m[2] == M_AUTO ? 0 : st->m[2];
    int bwl = st->bw[3], bwr = st->bw[1];
    int pl = st->p[3], pr = st->p[1];
    int maxcw = ic->cw;
    int avail = maxcw - ml - mr - bwl - bwr - pl - pr;
    if (avail < 8) avail = 8;

    int i0 = E->nitems, s0 = g_nnst, f0 = g_nflts;
    int prov = (1 << 20) + ic->y;

    int content_w;
    if (st->width >= 0) {
        content_w = (st->fl & SF_WPCT) ? (int)((long)avail * st->width / 100)
                                       : st->width;
        if (st->fl & SF_WPCT) content_w += st->woff;
        if (content_w > avail) content_w = avail;
        if (content_w < 8) content_w = 8;
        if (st->max_w >= 0 && content_w > st->max_w) content_w = st->max_w;
        if (st->min_w >= 0 && content_w < st->min_w) content_w = st->min_w;
    } else {
        /* shrink-to-fit: measure the preferred (unwrapped) content width */
        int mi = E->nitems, mrr = E->render_len, mf = g_nflts;
        g_flt_freeze++;
        lay_block(ni, 0, 100000, prov, is->link, is->bg);
        g_flt_freeze--;
        content_w = items_extent(mi) + pr + bwr;
        E->nitems = mi; E->render_len = mrr; g_nflts = mf;
        if (content_w > avail) content_w = avail;
        if (content_w < 8) content_w = 8;
        if (st->max_w >= 0 && content_w > st->max_w) content_w = st->max_w;
        if (st->min_w >= 0 && content_w < st->min_w) content_w = st->min_w;
    }

    /* Lay the real box: pass a container width that reproduces content_w.
     * Interior clips are created in provisional coordinates, so lay with
     * no outer clip in force and re-intersect after the shift. */
    int lay_cw = ml + mr + bwl + bwr + pl + pr + content_w;
    int c0 = g_nclips, save_cc = g_cur_clip;
    g_cur_clip = 0;
    int bot = lay_block(ni, 0, lay_cw, prov, is->link, is->bg);
    g_cur_clip = save_cc;
    int bbh = bot - prov;                       /* border-box height */
    int bw  = ml + bwl + pl + content_w + pr + bwr;   /* incl. left margin */
    int adv = bw + mr;

    /* Line-break if the margin box doesn't fit the remaining line. */
    if (ic->x > ic->lx0 && ic->x + adv > ic->lx1)
        ic_break(ic, ic->line_h > 0 ? ic->line_h : is->lh);

    /* At prov the border-box left = ml, top = prov. Move it so the left
     * margin edge sits at ic->x and the top margin edge at ic->y. */
    int dx = ic->x;
    int dy = (ic->y + mt) - prov;
    for (int i = i0; i < E->nitems; i++) {
        E->items[i].x += dx;
        E->items[i].y += dy;
        if (save_cc && E->items[i].clip == 0)   /* re-attach the outer clip */
            E->items[i].clip = (uint8_t)save_cc;
    }
    nstamp_shift(s0, g_nnst, dx, dy);
    clips_shift(c0, dx, dy, save_cc);
    g_nflts = f0;                               /* contain interior floats */

    ic->x += adv;
    int line_need = mt + bbh + mb + 2;
    if (line_need > ic->line_h) ic->line_h = line_need;
    ic->citem = -1;
    ic->pend_sp = 0;
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
            (cn->st.pos == 2 || cn->st.pos == 3)) {  /* abs/fixed out of flow
                                                        (sticky stays in flow) */
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

#ifdef FLEXDBG
static int msg_append(char *dst, int pos, int max, const char *s);
static int msg_append_int(char *dst, int pos, int max, int v);
#endif

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

#ifdef FLEXDBG
    if (!g_flt_freeze) {
        char m[400]; int p = 0;
        p = msg_append(m, p, sizeof(m), "[flex] node=");
        p = msg_append_int(m, p, sizeof(m), ni);
        p = msg_append(m, p, sizeof(m), " cw=");
        p = msg_append_int(m, p, sizeof(m), content_w);
        p = msg_append(m, p, sizeof(m), " wrap=");
        p = msg_append_int(m, p, sizeof(m), st->fwrap);
        {
            int cl; const char *cv = node_attr(nd, "class", &cl);
            if (cv) {
                p = msg_append(m, p, sizeof(m), " class=\"");
                for (int q = 0; q < cl && q < 40 && p < (int)sizeof(m) - 2; q++)
                    m[p++] = cv[q];
                p = msg_append(m, p, sizeof(m), "\"");
            }
        }
        p = msg_append(m, p, sizeof(m), " items:");
        for (int k = 0; k < n && p < (int)sizeof(m) - 24; k++) {
            p = msg_append(m, p, sizeof(m), " ");
            p = msg_append_int(m, p, sizeof(m), items[k]);
            p = msg_append(m, p, sizeof(m), "=h");
            p = msg_append_int(m, p, sizeof(m), hyp[k]);
            p = msg_append(m, p, sizeof(m), "/m");
            p = msg_append_int(m, p, sizeof(m), mnw[k]);
        }
        p = msg_append(m, p, sizeof(m), "\n");
        sys_write(1, m, p);
    }
#endif

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
    if (st->min_w >= 0 && content_w < st->min_w) content_w = st->min_w;
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

    /* box-shadow paints BEHIND the box, so emit it first (h patched
     * below alongside the bg). */
    int sh_i = -1;
    if (st->deco >= 0 && g_deco[st->deco].has_shadow) {
        struct ditem *sh = item_new(DI_SHADOW);
        if (sh) { sh->x = bx; sh->y = by; sh->w = bbw; sh->h = 0;
                  sh->radius = st->radius; sh->deco = st->deco;
                  sh_i = (int)(sh - E->items); }
    }
    int bg_i = -1;
    int has_deco_paint = (st->deco >= 0 &&
                          (g_deco[st->deco].has_grad || g_deco[st->deco].has_r4));
    if ((st->bg >> 24) || st->radius || has_deco_paint || st->mask >= 0) {
        bg_i = emit_rect(bx, by, bbw, 0, st->bg);   /* height patched below */
        if (bg_i >= 0) {
            E->items[bg_i].node = ni;               /* clicks on the box */
            E->items[bg_i].radius = st->radius;
            E->items[bg_i].deco = has_deco_paint ? st->deco : -1;
            E->items[bg_i].mask = st->mask;         /* mask-image stencil */
        }
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
    if (st->min_h >= 0 && content_h < st->min_h) content_h = st->min_h;
    /* overflow-clip with an explicit height: the box is EXACTLY that
     * height (content past it is clipped), not grown to fit. */
    if (st->ovf && st->height >= 0) content_h = st->height;
    int bbh = bwt + pt + content_h + pb + bwb;

    if (bg_i >= 0) E->items[bg_i].h = bbh;
    if (sh_i >= 0) E->items[sh_i].h = bbh;
    nstamp_add(ni, bx, by, bbw, bbh);
    if (bwt) emit_rect(bx, by, bbw, bwt, st->border_col);
    if (bwb) emit_rect(bx, by + bbh - bwb, bbw, bwb, st->border_col);
    if (bwl) emit_rect(bx, by, bwl, bbh, st->border_col);
    if (bwr) emit_rect(bx + bbw - bwr, by, bwr, bbh, st->border_col);

    g_cb_x = save_cbx; g_cb_y = save_cby; g_cb_w = save_cbw; g_cb_h = save_cbh;

    /* Compositing layer (slice 3): scale/rotate/opacity paint the whole
     * subtree [items0, nitems) through an offscreen buffer at paint
     * time. Registered BEFORE the relative/translate shifts below so
     * the paint-time item-union bbox self-tracks every later shift.
     * Measure passes (g_flt_freeze) discard their items, so skip. */
    if (!g_flt_freeze && E->nitems > items0 && g_nlayer < LAYER_MAX &&
        (st->scx != 1000 || st->scy != 1000 || st->rot != 0 || st->opa != 255)) {
        struct clayer *L = &g_layer_tab[g_nlayer++];
        L->node = ni; L->i0 = items0; L->i1 = E->nitems;
        L->scx = st->scx; L->scy = st->scy;
        L->rot = st->rot; L->opa = st->opa;
    }

    /* position: sticky -- record the item range + geometry; the paint
     * loop pins it once scroll passes the threshold (no relayout on
     * scroll). Containing block bottom is approximated by the document
     * height (clamped at paint), which is right for page-level sticky
     * headers/nav. */
    if (st->pos == 4 && !g_flt_freeze && E->nitems > items0 &&
        g_nsticky < STICKY_MAX) {
        struct sticky *sk = &g_sticky[g_nsticky++];
        sk->i0 = items0; sk->i1 = E->nitems;
        sk->top = by; sk->bot = by + bbh;
        sk->top_off = (st->po[0] != M_AUTO) ? st->po[0] : 0;
        sk->cb_bot = 1 << 30;                /* clamped to doc height at paint */
    }

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
    g_nlayer = 0;                /* compositing layers rebuild per layout */
    g_nsticky = 0;               /* sticky groups rebuild per layout */
    g_neseq = 0;                 /* emoji sequence pool rebuilds per layout */
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
    E->idx_nrules = -1;      /* invalidate the cascade rule index */
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
    g_nkf = 0;
    g_anim_active = 0;
    for (int i = 0; i < NODE_MAX; i++) g_anim_t0[i] = -1;
    mem_zero(g_trans, sizeof g_trans);
}

static void resolve_relative_url(const char *base, const char *rel, char *out, int out_max);
static void images_free(void);
static void tab_images_free(struct tab *t);
static void masks_free(void);
static void media_free(void);
static uint32_t *data_uri_decode_image(const char *s, int n, int *ow, int *oh);
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

/* Real sites split CSS into many small module chunks: github.com ships
 * 40 <link rel=stylesheet> on one page, so a cap of 16 silently dropped
 * more than half its styling. */
#ifdef CSS_PERF
static int msg_append(char *dst, int pos, int max, const char *s);
static int msg_append_int(char *dst, int pos, int max, int v);
static long js_now_ms(void);
#define SHEET_T(v) long v = js_now_ms()
static void sheet_report(long t0, long t1, long t2, long t3, long t4, long r) {
    char m[176]; int p = 0;
    p = msg_append(m, p, sizeof(m), "[cssperf] sheet malloc=");
    p = msg_append_int(m, p, sizeof(m), (int)(t1 - t0));
    p = msg_append(m, p, sizeof(m), " fetch=");
    p = msg_append_int(m, p, sizeof(m), (int)(t2 - t1));
    p = msg_append(m, p, sizeof(m), " parse=");
    p = msg_append_int(m, p, sizeof(m), (int)(t3 - t2));
    p = msg_append(m, p, sizeof(m), " free=");
    p = msg_append_int(m, p, sizeof(m), (int)(t4 - t3));
    p = msg_append(m, p, sizeof(m), " ms bytes=");
    p = msg_append_int(m, p, sizeof(m), (int)r);
    p = msg_append(m, p, sizeof(m), "\n");
    sys_write(1, m, p);
}
#define SHEET_REPORT(a,b,c,e,f,g) sheet_report(a,b,c,e,f,g)
#else
#define SHEET_T(v)                do {} while (0)
#define SHEET_REPORT(a,b,c,e,f,g) do {} while (0)
#endif

#define SHEET_MAX       64
#define SHEET_FETCH_CAP (4096 * 1024)

static int g_form_open;          /* collect-walk state: innermost form */
static int g_js_dirty;           /* a JS primitive mutated the DOM */
static int g_js_in_load;         /* inside render_html: defer rerender */
static int g_collect_light;      /* collect pass: skip styles/forms,
                                    register only unregistered nodes */

static void collect_node(int ni);

static void collect_node_inner(int ni) {
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
            node_attr_str(nd, "src", src, sizeof(src)) && src[0]) {
            struct img *im = &g_images[g_nimages];
            mem_zero(im, sizeof(*im));
            char d[8];
            im->attr_w = node_attr_str(nd, "width", d, sizeof(d))
                             ? (int16_t)atoi_simple(d) : 0;
            im->attr_h = node_attr_str(nd, "height", d, sizeof(d))
                             ? (int16_t)atoi_simple(d) : 0;
            if (str_ncasecmp(src, "data:", 5) == 0) {
                /* data: URI: decode inline, nothing to fetch. The full
                 * URI usually exceeds src[512] (the whole image is in
                 * the attribute), so read it from the attr pool. */
                int slen;
                const char *sv = node_attr(nd, "src", &slen);
                str_copy(im->src, "data:", sizeof(im->src)); /* marker;
                                     never fetched (state never stays 0) */
                int iw = 0, ih = 0;
                uint32_t *pix = sv ? data_uri_decode_image(sv, slen, &iw, &ih)
                                   : NULL;
                if (pix && iw > 0 && ih > 0) {
                    im->pixels = pix;
                    im->w = (int16_t)iw;
                    im->h = (int16_t)ih;
                    im->state = 1;
                } else {
                    if (pix) free(pix);
                    im->state = -1;
                }
            } else {
                resolve_relative_url(g_url, src, im->src, sizeof(im->src));
                im->state = 0;
            }
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
                    SHEET_T(t0);
                    char *buf = (char *)malloc(SHEET_FETCH_CAP);
                    SHEET_T(t1);
                    if (buf) {
                        struct http_fetch req;
                        mem_zero(&req, sizeof(req));
                        req.url = (unsigned long)url;
                        req.buf = (unsigned long)buf;
                        req.buf_sz = SHEET_FETCH_CAP;
                        long r = sys_http_fetch(&req);
                        SHEET_T(t2);
                        if (r > 0 && req.status > 0 && req.status < 400) {
                            E->nsheets++;
                            css_parse_sheet(buf, r, 1);
                        }
                        SHEET_T(t3);
                        free(buf);
                        SHEET_T(t4);
                        SHEET_REPORT(t0, t1, t2, t3, t4, r);
                    }
                }
            }
        }
        break;
    }
    case T_SCRIPT:
        g_form_open = save_form;
        return;
    case T_TEMPLATE:
        /* Web components: template content is inert -- no link/media/
         * canvas/style collection until it is cloned into the live tree. */
        g_form_open = save_form;
        return;
    default:
        break;
    }
    for (int c = nd->first; c >= 0; c = E->nodes[c].next)
        collect_node(c);
    g_form_open = save_form;
}

/* Scope-tracking wrapper: entering a shadow root switches the CSS
 * collect scope so a shadow tree's <style> rules stamp crule.scope =
 * that root (shadow-dom arc). */
static void collect_node(int ni) {
    struct dnode *nd = &E->nodes[ni];
    int save_scope = g_css_scope;
    if (nd->parent >= 0 && E->nodes[nd->parent].shadow == ni)
        g_css_scope = ni;
    collect_node_inner(ni);
    g_css_scope = save_scope;
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

/* ---- <slot> distribution (shadow-dom arc, slice 3) ------------------ *
 * Physical projection: each shadow host's light children move under the
 * matching <slot> in its shadow tree (name attr vs slot attr; unnamed
 * slot takes unslotted children). Runs before every style pass and is
 * idempotent (moved children are no longer light children). Unassigned
 * light DOM stays hidden; a slot with no assignment keeps its fallback
 * children. v1 deviations (documented): distributed nodes' parentNode
 * becomes the slot, they style in the shadow scope, and there is no
 * un-distribution when slots change. */
static int slot_find_rec(int ni, const char *name) {
    struct dnode *nd = &E->nodes[ni];
    if (nd->tag == T_SLOT) {
        char sn[49];
        int has = node_attr_str(nd, "name", sn, sizeof(sn));
        if ((!has && !name[0]) || (has && str_ncasecmp(sn, name, 49) == 0))
            return ni;
    }
    for (int c = nd->first; c >= 0; c = E->nodes[c].next) {
        int r = slot_find_rec(c, name);
        if (r >= 0) return r;
    }
    return -1;
}

static void distribute_slots(void) {
    for (int host = 1; host < E->nnodes; host++) {
        int sr = E->nodes[host].shadow;
        if (sr < 0) continue;
        int kids[32], nkids = 0;
        for (int c = E->nodes[host].first; c >= 0 && nkids < 32;
             c = E->nodes[c].next)
            if (c != sr) kids[nkids++] = c;
        for (int k = 0; k < nkids; k++) {
            struct dnode *cn = &E->nodes[kids[k]];
            char sl[49];
            sl[0] = 0;
            if (cn->tag != T_TEXT)
                node_attr_str(cn, "slot", sl, sizeof(sl));
            int slot = slot_find_rec(sr, sl);
            if (slot >= 0) {
                /* first assignment ever: drop the slot's fallback
                 * children (v1 has no un-distribution anyway) */
                char f[4];
                if (!node_attr_str(&E->nodes[slot], "__filled", f, sizeof(f))) {
                    while (E->nodes[slot].first >= 0)
                        dom_unlink(E->nodes[slot].first);
                    dom_set_attr(slot, "__filled", "1");
                }
                dom_attach(slot, kids[k]);
            }
        }
    }
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
        if (tag == T_UNK) dom_stash_custom_tag(r, s, (long)str_len(s));
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
    if (ni >= 0 && E->nodes[ni].tag == T_UNK) {
        /* custom element: report the stashed source name */
        char nm[49];
        if (node_attr_str(&E->nodes[ni], "__tag", nm, sizeof(nm)) && nm[0])
            return JS_NewString(cx, nm);
    }
    return JS_NewString(cx, ni >= 0 ? g_tag_names[E->nodes[ni].tag] : "");
}

/* Deep-clone a subtree (template stamping). Text/attr slices point into
 * the immutable tpool, so clones share them; a later dom_set_attr on the
 * clone rewrites its own attr block (copy-on-write). Returns the new
 * root (parent -1). */
static int dom_clone_rec(int src, int parent) {
    struct dnode *sn = &E->nodes[src];
    int ni = dom_new(sn->tag, parent);
    if (ni < 0) return -1;
    struct dnode *dn = &E->nodes[ni];
    dn->toff = sn->toff; dn->tlen = sn->tlen;
    dn->attr0 = sn->attr0; dn->nattr = sn->nattr;
    for (int c = E->nodes[src].first; c >= 0; c = E->nodes[c].next)
        if (dom_clone_rec(c, ni) < 0) break;
    return ni;
}

static JSValue js_dom_clone(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    if (ni < 0) return JS_NewInt32(cx, -1);
    return JS_NewInt32(cx, dom_clone_rec(ni, -1));
}

/* Attach (or return the existing) shadow root of a node: a fresh block
 * container child recorded in dnode.shadow. The style pass renders it
 * INSTEAD of the host's light children (composed tree); its <style>
 * rules scope to the shadow tree. */
static JSValue js_dom_attachshadow(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int ni = (argc >= 1) ? jsi(cx, argv[0]) : -1;
    if (ni < 0) return JS_NewInt32(cx, -1);
    if (E->nodes[ni].shadow >= 0)
        return JS_NewInt32(cx, E->nodes[ni].shadow);
    int sr = dom_new(T_DIV, ni);
    if (sr < 0) return JS_NewInt32(cx, -1);
    E->nodes[ni].shadow = sr;
    mut_note(ni, 0);
    g_js_dirty = 1;
    return JS_NewInt32(cx, sr);
}

/* Next node index >= from whose tag name equals `name` (custom "__tag"
 * names included); -1 when exhausted. Drives customElements upgrades. */
static JSValue js_dom_bytagfrom(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NewInt32(cx, -1);
    const char *s = JS_ToCString(cx, argv[0]);
    int32_t from = 0;
    JS_ToInt32(cx, &from, argv[1]);
    if (!s) return JS_NewInt32(cx, -1);
    int want = tag_lookup(s, (int)str_len(s));
    int found = -1;
    for (int ni = from < 1 ? 1 : from; ni < E->nnodes; ni++) {
        struct dnode *nd = &E->nodes[ni];
        if (nd->tag != want) continue;
        if (want == T_UNK) {
            char nm[49];
            if (!node_attr_str(nd, "__tag", nm, sizeof(nm)) ||
                str_ncasecmp(nm, s, (int)str_len(s) + 1) != 0)
                continue;
        }
        found = ni;
        break;
    }
    JS_FreeCString(cx, s);
    return JS_NewInt32(cx, found);
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

#define JS_FETCH_CAP (4 * 1024 * 1024)  /* XHR/fetch() bodies (SPA data payloads can be large) */

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
                        JS_SetPropertyStr(cx, obj, "bodyBytes",
                            JS_NewArrayBufferCopy(cx, (const uint8_t *)buf, (size_t)r));
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

/* fetchStart2(url, cb, method, headers, body, noCookies): the extended
 * fetch primitive (method/headers/body + credentials-less cross-origin
 * mode). Same async cb({status,url,body,headers}) contract; cb(null)
 * on failure. The prelude fetch() wrapper computes CORS + credentials
 * and calls this. */
static JSValue js_dom_fetchstart2(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2 || !JS_IsFunction(cx, argv[1])) return JS_UNDEFINED;
    char url[URL_MAX + 1];
    url[0] = 0;
    const char *u = JS_ToCString(cx, argv[0]);
    if (u) { resolve_relative_url(g_url, u, url, URL_MAX); JS_FreeCString(cx, u); }

    const char *method = (argc > 2 && JS_IsString(argv[2])) ? JS_ToCString(cx, argv[2]) : NULL;
    const char *headers = (argc > 3 && JS_IsString(argv[3])) ? JS_ToCString(cx, argv[3]) : NULL;
    size_t blen = 0;
    const char *body = (argc > 4 && JS_IsString(argv[4])) ? JS_ToCStringLen(cx, &blen, argv[4]) : NULL;
    int no_cookies = (argc > 5) && JS_ToBool(cx, argv[5]);

    long h = -1;
    if (url[0] && has_scheme(url)) {
        struct http_start2 rq;
        mem_zero(&rq, sizeof(rq));
        rq.url = (unsigned long)url;
        rq.method = (unsigned long)method;
        rq.headers = (unsigned long)headers;
        rq.body = (unsigned long)body;
        rq.body_len = (uint32_t)blen;
        rq.max_body = JS_FETCH_CAP;
        rq.flags = no_cookies ? HTTP_F_NO_COOKIES : 0;
        h = sys_http_start2(&rq);
    }
    if (method) JS_FreeCString(cx, method);
    if (headers) JS_FreeCString(cx, headers);
    if (body) JS_FreeCString(cx, body);

    int slot = -1;
    if (h >= 0)
        for (int k = 0; k < JS_FETCH_MAX; k++)
            if (!cur->js_fetches[k].used) { slot = k; break; }
    if (h < 0 || slot < 0) {
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

/* ---- Web Workers (chrome-parity) ------------------------------------- *
 * A Worker is a second JSContext on its own JSRuntime, pumped
 * cooperatively from the main loop (the engine is single-threaded, so
 * this is concurrency by interleaving, not parallelism -- correct
 * semantics without a JIT/threads). Messages cross contexts as JSON
 * strings (structured-clone approximation); all serialize/parse stays
 * in JS via each side's __deliver(json) method, so C only shuttles
 * opaque strings. Workers have no DOM access. */
#define WORKER_MAX 4
#define WMSG_MAX   64
struct jsworker {
    int        used, terminated;
    JSRuntime *rt;
    JSContext *cx;             /* the worker's context */
    JSContext *main_cx;        /* owning tab's context (delivery target) */
    JSValue    main_obj;       /* main-side Worker object (dup'd) */
    char      *to_worker[WMSG_MAX]; int twh, twt;
    char      *to_main[WMSG_MAX];   int tmh, tmt;
    struct jstimer timers[JS_TIMER_MAX];
    int        timer_seq;
};
static struct jsworker g_workers[WORKER_MAX];
static int g_cur_worker = -1;             /* index while running worker code */

static char *wstrdup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (p) { for (size_t i = 0; i < n; i++) p[i] = s[i]; p[n] = 0; }
    return p;
}
static int wq_push(char **q, int *tail, int head, char *s) {
    int nx = (*tail + 1) % WMSG_MAX;
    if (nx == head) { free(s); return 0; }   /* full: drop */
    q[*tail] = s; *tail = nx; return 1;
}

static const char WORKER_PRELUDE[];       /* defined below JS_PRELUDE */

/* Fetch a script synchronously (worker load + importScripts). Returns a
 * malloc'd NUL-terminated body or NULL. */
static char *worker_fetch_script(const char *rel) {
    char url[URL_MAX + 1];
    resolve_relative_url(g_url, rel, url, URL_MAX);
    if (!has_scheme(url)) return NULL;
    char *buf = (char *)malloc(JS_FETCH_CAP + 1);
    if (!buf) return NULL;
    struct http_fetch req;
    mem_zero(&req, sizeof(req));
    req.url = (unsigned long)url;
    req.buf = (unsigned long)buf;
    req.buf_sz = JS_FETCH_CAP;
    long r = sys_http_fetch(&req);
    if (r < 0 || req.status < 200 || req.status >= 400) { free(buf); return NULL; }
    buf[r] = 0;
    return buf;
}

static void js_dump_error(JSContext *cx);

/* __w.postMain(json): worker -> main queue (runs in worker context). */
static JSValue js_w_postmain(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (g_cur_worker < 0 || argc < 1) return JS_UNDEFINED;
    struct jsworker *w = &g_workers[g_cur_worker];
    size_t n = 0;
    const char *s = JS_ToCStringLen(cx, &n, argv[0]);
    if (s) { char *d = wstrdup(s, n);
             if (d) wq_push(w->to_main, &w->tmt, w->tmh, d);
             JS_FreeCString(cx, s); }
    return JS_UNDEFINED;
}

/* __w.timer(fn, ms, rep) / untimer(id): worker-local timers. */
static JSValue js_w_timer(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (g_cur_worker < 0 || argc < 2 || !JS_IsFunction(cx, argv[0])) return JS_NewInt32(cx, 0);
    struct jsworker *w = &g_workers[g_cur_worker];
    int32_t ms = 0, rep = 0;
    JS_ToInt32(cx, &ms, argv[1]);
    if (argc >= 3) JS_ToInt32(cx, &rep, argv[2]);
    if (ms < 0) ms = 0;
    for (int k = 0; k < JS_TIMER_MAX; k++) {
        struct jstimer *tm = &w->timers[k];
        if (tm->used) continue;
        tm->used = 1; tm->id = w->timer_seq++;
        tm->due_ms = js_now_ms() + ms;
        tm->interval_ms = rep ? (ms > 0 ? ms : 10) : 0;
        tm->fn = JS_DupValue(cx, argv[0]);
        return JS_NewInt32(cx, tm->id);
    }
    return JS_NewInt32(cx, 0);
}
static JSValue js_w_untimer(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (g_cur_worker < 0 || argc < 1) return JS_UNDEFINED;
    struct jsworker *w = &g_workers[g_cur_worker];
    int32_t id = 0; JS_ToInt32(cx, &id, argv[0]);
    for (int k = 0; k < JS_TIMER_MAX; k++)
        if (w->timers[k].used && w->timers[k].id == id) {
            JS_FreeValue(cx, w->timers[k].fn); w->timers[k].used = 0; break;
        }
    return JS_UNDEFINED;
}

/* importScripts(url...): synchronous fetch + eval in the worker. */
static JSValue js_w_import(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    for (int i = 0; i < argc; i++) {
        const char *u = JS_ToCString(cx, argv[i]);
        if (!u) continue;
        char *src = worker_fetch_script(u);
        JS_FreeCString(cx, u);
        if (!src) continue;
        JSValue r = JS_Eval(cx, src, str_len(src), "importScripts", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) js_dump_error(cx);
        JS_FreeValue(cx, r);
        free(src);
    }
    return JS_UNDEFINED;
}

/* Install the worker global bindings + prelude, eval the script. */
static int worker_boot(struct jsworker *w, const char *script) {
    JSValue g = JS_GetGlobalObject(w->cx);
    JSValue wd = JS_NewObject(w->cx);
    JS_SetPropertyStr(w->cx, wd, "postMain", JS_NewCFunction(w->cx, js_w_postmain, "postMain", 1));
    JS_SetPropertyStr(w->cx, wd, "timer",    JS_NewCFunction(w->cx, js_w_timer, "timer", 3));
    JS_SetPropertyStr(w->cx, wd, "untimer",  JS_NewCFunction(w->cx, js_w_untimer, "untimer", 1));
    JS_SetPropertyStr(w->cx, wd, "import",    JS_NewCFunction(w->cx, js_w_import, "import", 1));
    JS_SetPropertyStr(w->cx, g, "__w", wd);
    JSValue cons = JS_NewObject(w->cx);
    JS_SetPropertyStr(w->cx, cons, "log",   JS_NewCFunction(w->cx, js_console_log, "log", 1));
    JS_SetPropertyStr(w->cx, cons, "warn",  JS_NewCFunction(w->cx, js_console_log, "warn", 1));
    JS_SetPropertyStr(w->cx, cons, "error", JS_NewCFunction(w->cx, js_console_log, "error", 1));
    JS_SetPropertyStr(w->cx, g, "console", cons);
    JS_FreeValue(w->cx, g);
    JSValue pr = JS_Eval(w->cx, WORKER_PRELUDE, str_len(WORKER_PRELUDE),
                         "<worker-prelude>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(pr)) js_dump_error(w->cx);
    JS_FreeValue(w->cx, pr);
    JSValue r = JS_Eval(w->cx, script, str_len(script), "worker", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(r);
    if (!ok) js_dump_error(w->cx);
    JS_FreeValue(w->cx, r);
    return ok;
}

static void worker_free(struct jsworker *w) {
    if (!w->used) return;
    for (int k = 0; k < JS_TIMER_MAX; k++)
        if (w->timers[k].used) { JS_FreeValue(w->cx, w->timers[k].fn); w->timers[k].used = 0; }
    while (w->twh != w->twt) { free(w->to_worker[w->twh]); w->twh = (w->twh + 1) % WMSG_MAX; }
    while (w->tmh != w->tmt) { free(w->to_main[w->tmh]); w->tmh = (w->tmh + 1) % WMSG_MAX; }
    if (w->main_cx && !JS_IsUndefined(w->main_obj)) JS_FreeValue(w->main_cx, w->main_obj);
    if (w->cx) JS_FreeContext(w->cx);
    if (w->rt) JS_FreeRuntime(w->rt);
    mem_zero(w, sizeof(*w));
}

/* new Worker(url, obj): main-context primitive. Returns handle or -1. */
static JSValue js_worker_new(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NewInt32(cx, -1);
    int slot = -1;
    for (int i = 0; i < WORKER_MAX; i++) if (!g_workers[i].used) { slot = i; break; }
    if (slot < 0) return JS_NewInt32(cx, -1);
    const char *u = JS_ToCString(cx, argv[0]);
    char *src = u ? worker_fetch_script(u) : NULL;
    if (u) JS_FreeCString(cx, u);
    if (!src) return JS_NewInt32(cx, -1);
    struct jsworker *w = &g_workers[slot];
    mem_zero(w, sizeof(*w));
    w->rt = JS_NewRuntime();
    if (w->rt) { JS_SetMemoryLimit(w->rt, 24u * 1024 * 1024);
                 JS_SetMaxStackSize(w->rt, 192 * 1024);
                 w->cx = JS_NewContext(w->rt); }
    if (!w->cx) { if (w->rt) JS_FreeRuntime(w->rt); free(src); return JS_NewInt32(cx, -1); }
    w->used = 1;
    w->main_cx = cx;
    w->main_obj = JS_DupValue(cx, argv[1]);
    w->timer_seq = 1;
    g_cur_worker = slot;
    int ok = worker_boot(w, src);
    g_cur_worker = -1;
    free(src);
    (void)ok;
    return JS_NewInt32(cx, slot);
}

/* workerPost(handle, json): main -> worker queue. */
static JSValue js_worker_post(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int32_t h = -1; JS_ToInt32(cx, &h, argv[0]);
    if (h < 0 || h >= WORKER_MAX || !g_workers[h].used) return JS_UNDEFINED;
    size_t n = 0;
    const char *s = JS_ToCStringLen(cx, &n, argv[1]);
    if (s) { char *d = wstrdup(s, n);
             if (d) wq_push(g_workers[h].to_worker, &g_workers[h].twt, g_workers[h].twh, d);
             JS_FreeCString(cx, s); }
    return JS_UNDEFINED;
}
static JSValue js_worker_terminate(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t h = -1; if (argc >= 1) JS_ToInt32(cx, &h, argv[0]);
    if (h >= 0 && h < WORKER_MAX && g_workers[h].used) worker_free(&g_workers[h]);
    return JS_UNDEFINED;
}

/* Deliver `json` to a context's global __deliver(json) (worker side) or
 * an object's __deliver (main side). */
static void worker_deliver(JSContext *cx, JSValueConst target, const char *json) {
    JSValue fn = JS_GetPropertyStr(cx, target, "__deliver");
    if (JS_IsFunction(cx, fn)) {
        JSValue arg = JS_NewString(cx, json);
        JSValue r = JS_Call(cx, fn, target, 1, &arg);
        if (JS_IsException(r)) js_dump_error(cx);
        JS_FreeValue(cx, r);
        JS_FreeValue(cx, arg);
    }
    JS_FreeValue(cx, fn);
}

/* Pump all workers once: deliver queued messages both directions, run
 * worker timers + pending jobs. Returns 1 if any work happened. */
static int worker_pump(void) {
    int work = 0;
    for (int i = 0; i < WORKER_MAX; i++) {
        struct jsworker *w = &g_workers[i];
        if (!w->used) continue;
        g_cur_worker = i;
        /* main -> worker */
        while (w->twh != w->twt) {
            char *json = w->to_worker[w->twh]; w->twh = (w->twh + 1) % WMSG_MAX;
            JSValue g = JS_GetGlobalObject(w->cx);
            worker_deliver(w->cx, g, json);
            JS_FreeValue(w->cx, g);
            free(json); work = 1;
        }
        /* worker timers */
        long now = js_now_ms();
        for (int k = 0; k < JS_TIMER_MAX; k++) {
            struct jstimer *tm = &w->timers[k];
            if (!tm->used || now < tm->due_ms) continue;
            JSValue fn = JS_DupValue(w->cx, tm->fn);
            if (tm->interval_ms > 0) tm->due_ms = now + tm->interval_ms;
            else { JS_FreeValue(w->cx, tm->fn); tm->used = 0; }
            JSValue r = JS_Call(w->cx, fn, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(r)) js_dump_error(w->cx);
            JS_FreeValue(w->cx, r); JS_FreeValue(w->cx, fn); work = 1;
        }
        /* worker pending jobs (promises) */
        JSContext *pc;
        while (JS_ExecutePendingJob(w->rt, &pc) > 0) work = 1;
        g_cur_worker = -1;
        /* worker -> main */
        while (w->tmh != w->tmt) {
            char *json = w->to_main[w->tmh]; w->tmh = (w->tmh + 1) % WMSG_MAX;
            worker_deliver(w->main_cx, w->main_obj, json);
            free(json); work = 1;
        }
    }
    return work;
}

/* Free all workers owned by a tab teardown (main_cx match). */
static void workers_free_for(JSContext *main_cx) {
    for (int i = 0; i < WORKER_MAX; i++)
        if (g_workers[i].used && g_workers[i].main_cx == main_cx)
            worker_free(&g_workers[i]);
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
                /* anchor the master clock so frame_pts[cur] is relative to
                 * now (rewind the anchor by the current frame's PTS). */
                m->play_start = js_now_ms() -
                    (m->have_pts && m->cur_frame < m->nframes
                         ? m->frame_pts[m->cur_frame]
                         : (long)m->cur_frame * m->frame_ms);
                if (m->is_video && m->cur_frame >= m->nframes)
                    { m->cur_frame = 0; m->play_start = js_now_ms(); }
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

/* ---- WebAssembly (wasm3) ------------------------------------------------
 *
 * The native half of the browser's WebAssembly JS API. wasm3 runs the
 * module; the prelude (WASM_PRELUDE) wraps these primitives into the
 * WebAssembly.{Module,Instance,Memory,instantiate,compile} surface.
 *
 * A module's imported functions are resolved by calling a JS resolver
 * (module,field)->fn during instantiate; each is linked to a single C
 * trampoline (wasm_import_tramp) that marshals wasm values <-> JSValues
 * and calls back into JS. Exports are called via wasmCall; exported
 * linear memory is exposed as a live ArrayBuffer aliasing wasm memory. */

#define WASM_MAX_INSTANCES 8
#define WASM_MAX_IMPORTS   96
#define WASM_MAX_ARGS      16
/* value types mirror M3ValueType: 1 i32, 2 i64, 3 f32, 4 f64 */

struct wasm_import {
    JSContext *cx;
    JSValue    fn;             /* JS function to invoke (may be undefined) */
    int        has_fn;
    uint16_t   nargs, nrets;
    uint8_t    argt[WASM_MAX_ARGS];
    uint8_t    rett;
};

struct wasm_inst {
    int             used;
    JSContext      *cx;        /* owning tab context (for teardown) */
    IM3Environment  env;
    IM3Runtime      rt;
    IM3Module       mod;
    unsigned char  *bytes;     /* owned copy; m3 references it for life */
    unsigned long   nbytes;
    int             nimports;
    struct wasm_import imports[WASM_MAX_IMPORTS];
    /* the last aliasing ArrayBuffer handed to JS + the memory base it was
     * issued over; when the base moves (memory.grow reallocs), the old
     * buffer is detached so stale JS views throw instead of reading freed
     * memory -- matching the WebAssembly.Memory grow semantics. */
    JSValue         mem_buf;
    int             has_mem_buf;
    uint8_t        *mem_base;
    int             mem_imported;      /* memory came from an import */
};

static struct wasm_inst g_wasm[WASM_MAX_INSTANCES];

/* Single trampoline linked for every JS-resolved function import. sp holds
 * result slots first (nrets), then argument slots (nargs); each is 64-bit. */
static const void *wasm_import_tramp(IM3Runtime rt, IM3ImportContext ctx,
                                     uint64_t *sp, void *mem) {
    (void)rt; (void)mem;
    struct wasm_import *wi = (struct wasm_import *)ctx->userdata;
    if (!wi || !wi->has_fn || !JS_IsFunction(wi->cx, wi->fn))
        return "unresolved wasm import";
    JSContext *cx = wi->cx;

    JSValue av[WASM_MAX_ARGS];
    int n = wi->nargs; if (n > WASM_MAX_ARGS) n = WASM_MAX_ARGS;
    for (int k = 0; k < n; k++) {
        uint64_t raw = sp[wi->nrets + k];
        switch (wi->argt[k]) {
        case 1: av[k] = JS_NewInt32(cx, (int32_t)(uint32_t)raw); break;
        case 2: av[k] = JS_NewBigInt64(cx, (int64_t)raw); break;  /* i64 -> BigInt */
        case 3: { float f; __builtin_memcpy(&f, &raw, 4);
                  av[k] = JS_NewFloat64(cx, (double)f); break; }
        case 4: { double d; __builtin_memcpy(&d, &raw, 8);
                  av[k] = JS_NewFloat64(cx, d); break; }
        default: av[k] = JS_UNDEFINED; break;
        }
    }
    JSValue r = JS_Call(cx, wi->fn, JS_UNDEFINED, n, av);
    for (int k = 0; k < n; k++) JS_FreeValue(cx, av[k]);
    if (JS_IsException(r)) { JS_FreeValue(cx, r); return "wasm import threw"; }

    if (wi->nrets >= 1) {
        switch (wi->rett) {
        case 1: { int32_t v = 0; JS_ToInt32(cx, &v, r);
                  sp[0] = (uint64_t)(uint32_t)v; break; }
        case 2: { int64_t v = 0;               /* accept BigInt or Number */
                  if (JS_IsBigInt(cx, r)) JS_ToBigInt64(cx, &v, r);
                  else JS_ToInt64(cx, &v, r);
                  sp[0] = (uint64_t)v; break; }
        case 3: { double d = 0; JS_ToFloat64(cx, &d, r); float f = (float)d;
                  uint32_t u; __builtin_memcpy(&u, &f, 4); sp[0] = u; break; }
        case 4: { double d = 0; JS_ToFloat64(cx, &d, r);
                  __builtin_memcpy(&sp[0], &d, 8); break; }
        default: sp[0] = 0; break;
        }
    }
    JS_FreeValue(cx, r);
    return m3Err_none;      /* NULL: success */
}

static void wasm_type_char(int t, char *out) {
    switch (t) { case 1: *out='i'; break; case 2: *out='I'; break;
                 case 3: *out='f'; break; case 4: *out='F'; break;
                 default: *out='v'; break; }
}

/* Drop the cached aliasing ArrayBuffer (detaching it in JS if requested,
 * so any live JS view over the old memory base throws on access). */
static void wasm_drop_membuf(struct wasm_inst *w, int detach) {
    if (!w->has_mem_buf) return;
    if (detach) JS_DetachArrayBuffer(w->cx, w->mem_buf);
    JS_FreeValue(w->cx, w->mem_buf);
    w->has_mem_buf = 0;
    w->mem_base = NULL;
}

static void wasm_sync_membuf(struct wasm_inst *w);   /* defined below */

static void wasm_inst_free(struct wasm_inst *w) {
    if (!w->used) return;
    wasm_drop_membuf(w, 0);
    for (int i = 0; i < w->nimports; i++)
        if (w->imports[i].has_fn) JS_FreeValue(w->imports[i].cx, w->imports[i].fn);
    if (w->rt)  m3_FreeRuntime(w->rt);     /* also frees the loaded module */
    if (w->env) m3_FreeEnvironment(w->env);
    if (w->bytes) free(w->bytes);
    __builtin_memset(w, 0, sizeof(*w));
}

/* Free every instance owned by a tab context (called at JS teardown). */
static void wasm_free_for_context(JSContext *cx) {
    for (int i = 0; i < WASM_MAX_INSTANCES; i++)
        if (g_wasm[i].used && g_wasm[i].cx == cx) wasm_inst_free(&g_wasm[i]);
}

/* wasmInstantiate(bytesArrayBuffer, resolverFn) -> instanceId | throws.
 * resolverFn(moduleName, fieldName) returns the JS function for a function
 * import (or undefined). */
static JSValue js_wasm_instantiate(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_ThrowTypeError(cx, "wasmInstantiate: bytes required");
    size_t blen = 0;
    uint8_t *bsrc = JS_GetArrayBuffer(cx, &blen, argv[0]);
    if (!bsrc || blen < 8)
        return JS_ThrowTypeError(cx, "wasmInstantiate: expected wasm ArrayBuffer");

    int id = -1;
    for (int i = 0; i < WASM_MAX_INSTANCES; i++)
        if (!g_wasm[i].used) { id = i; break; }
    if (id < 0) return JS_ThrowInternalError(cx, "wasm: too many instances");
    struct wasm_inst *w = &g_wasm[id];
    __builtin_memset(w, 0, sizeof(*w));

    /* Own a copy: wasm3 references the bytes for the module's lifetime. */
    w->bytes = (unsigned char *)malloc(blen);
    if (!w->bytes) return JS_ThrowOutOfMemory(cx);
    __builtin_memcpy(w->bytes, bsrc, blen);
    w->nbytes = blen;

    w->env = m3_NewEnvironment();
    w->rt  = w->env ? m3_NewRuntime(w->env, 256 * 1024, NULL) : NULL;
    if (!w->env || !w->rt) { wasm_inst_free(w);
        return JS_ThrowOutOfMemory(cx); }

    M3Result r = m3_ParseModule(w->env, &w->mod, w->bytes, (uint32_t)blen);
    if (r) { wasm_inst_free(w);
        return JS_ThrowTypeError(cx, "wasm parse: %s", r); }

    /* Imported memory: wasm3 leaves the runtime's linear memory
     * unallocated for a module that imports it, and LoadModule then trips
     * on the unallocated memory -- so set it up here, between parse and
     * load. Size = the JS Memory's requested pages (argv[2]) if given,
     * else the module's declared minimum; capped at the declared max. */
    if (toby_wasm_memory_imported(w->mod)) {
        int want = (argc >= 3) ? jsi(cx, argv[2]) : 0;
        int minp = toby_wasm_memory_init_pages(w->mod);
        if (want < minp) want = minp;
        if (toby_wasm_mem_setup(w->rt, want, toby_wasm_memory_max_pages(w->mod)) != 0) {
            wasm_inst_free(w);
            return JS_ThrowInternalError(cx, "wasm: imported memory setup failed");
        }
        w->mem_imported = 1;
    }

    r = m3_LoadModule(w->rt, w->mod);      /* runtime now owns the module */
    if (r) { wasm_inst_free(w);
        return JS_ThrowTypeError(cx, "wasm load: %s", r); }

    /* Resolve + link function imports. */
    int nimp = toby_wasm_num_func_imports(w->mod);
    if (nimp > WASM_MAX_IMPORTS) nimp = WASM_MAX_IMPORTS;
    w->cx = cx;
    w->nimports = nimp;
    int have_resolver = (argc >= 2 && JS_IsFunction(cx, argv[1]));
    for (int i = 0; i < nimp; i++) {
        const char *mod = toby_wasm_import_module(w->mod, i);
        const char *fld = toby_wasm_import_field(w->mod, i);
        struct wasm_import *wi = &w->imports[i];
        wi->cx = cx;
        wi->nargs = (uint16_t)toby_wasm_import_num_args(w->mod, i);
        wi->nrets = (uint16_t)toby_wasm_import_num_rets(w->mod, i);
        if (wi->nargs > WASM_MAX_ARGS) wi->nargs = WASM_MAX_ARGS;
        for (int a = 0; a < wi->nargs; a++)
            wi->argt[a] = (uint8_t)toby_wasm_import_arg_type(w->mod, i, a);
        wi->rett = (uint8_t)(wi->nrets ? toby_wasm_import_ret_type(w->mod, i, 0) : 0);

        if (have_resolver && mod && fld) {
            JSValue ma = JS_NewString(cx, mod), fa = JS_NewString(cx, fld);
            JSValue ar[2] = { ma, fa };
            JSValue fn = JS_Call(cx, argv[1], JS_UNDEFINED, 2, ar);
            JS_FreeValue(cx, ma); JS_FreeValue(cx, fa);
            if (JS_IsFunction(cx, fn)) { wi->fn = fn; wi->has_fn = 1; }
            else JS_FreeValue(cx, fn);
        }

        /* signature: <ret>(<args>) */
        char sig[WASM_MAX_ARGS + 4]; int sp = 0;
        wasm_type_char(wi->nrets ? wi->rett : 0, &sig[sp]); sp++;
        sig[sp++] = '(';
        for (int a = 0; a < wi->nargs; a++) wasm_type_char(wi->argt[a], &sig[sp++]);
        sig[sp++] = ')'; sig[sp] = 0;

        if (mod && fld)
            m3_LinkRawFunctionEx(w->mod, mod, fld, sig, &wasm_import_tramp, wi);
    }

    w->used = 1;
    return JS_NewInt32(cx, id);
}

static struct wasm_inst *wasm_get(int id) {
    if (id < 0 || id >= WASM_MAX_INSTANCES || !g_wasm[id].used) return NULL;
    return &g_wasm[id];
}

/* wasmExportNames(id) -> [names...] of exported functions. */
static JSValue js_wasm_exports(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    JSValue a = JS_NewArray(cx);
    if (!w) return a;
    int nf = toby_wasm_num_functions(w->mod), n = 0;
    for (int i = 0; i < nf; i++) {
        const char *nm = toby_wasm_func_export_name(w->mod, i);
        if (nm && nm[0])
            JS_SetPropertyUint32(cx, a, n++, JS_NewString(cx, nm));
    }
    return a;
}

/* wasmMemName(id) -> exported-memory name or "" (empty => no exported mem) */
static JSValue js_wasm_memname(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    const char *nm = w ? toby_wasm_memory_export_name(w->mod) : NULL;
    return JS_NewString(cx, nm ? nm : "");
}

/* Marshal a JS args array into f's signature, call it, and return the
 * result as a JSValue (or throws). args may be JS_UNDEFINED for none. */
static JSValue wasm_invoke(JSContext *cx, struct wasm_inst *w,
                           IM3Function f, JSValueConst args) {
    uint32_t na = m3_GetArgCount(f);
    if (na > WASM_MAX_ARGS) na = WASM_MAX_ARGS;
    uint64_t slots[WASM_MAX_ARGS];
    const void *aptr[WASM_MAX_ARGS];
    for (uint32_t k = 0; k < na; k++) {
        JSValue e = JS_IsUndefined(args) ? JS_UNDEFINED
                                         : JS_GetPropertyUint32(cx, args, k);
        slots[k] = 0;
        switch (m3_GetArgType(f, k)) {
        case c_m3Type_i32: { int32_t v = 0; JS_ToInt32(cx, &v, e);
                             slots[k] = (uint64_t)(uint32_t)v; break; }
        case c_m3Type_i64: { int64_t v = 0;   /* accept BigInt or Number */
                             if (JS_IsBigInt(cx, e)) JS_ToBigInt64(cx, &v, e);
                             else JS_ToInt64(cx, &v, e);
                             slots[k] = (uint64_t)v; break; }
        case c_m3Type_f32: { double d = 0; JS_ToFloat64(cx, &d, e); float ff = (float)d;
                             uint32_t u; __builtin_memcpy(&u, &ff, 4); slots[k] = u; break; }
        case c_m3Type_f64: { double d = 0; JS_ToFloat64(cx, &d, e);
                             __builtin_memcpy(&slots[k], &d, 8); break; }
        default: break;
        }
        JS_FreeValue(cx, e);
        aptr[k] = &slots[k];
    }

    M3Result r = m3_Call(f, na, aptr);
    if (r) return JS_ThrowInternalError(cx, "wasm trap: %s", r);
    /* the call may have grown memory internally (memory.grow), moving the
     * base -- detach the stale cached buffer so a re-read issues a fresh one. */
    wasm_sync_membuf(w);

    if (m3_GetRetCount(f) < 1) return JS_UNDEFINED;
    uint64_t ret = 0; const void *rptr[1] = { &ret };
    if (m3_GetResults(f, 1, rptr)) return JS_UNDEFINED;
    switch (m3_GetRetType(f, 0)) {
    case c_m3Type_i32: return JS_NewInt32(cx, (int32_t)(uint32_t)ret);
    case c_m3Type_i64: return JS_NewBigInt64(cx, (int64_t)ret);  /* i64 -> BigInt */
    case c_m3Type_f32: { float ff; __builtin_memcpy(&ff, &ret, 4);
                         return JS_NewFloat64(cx, (double)ff); }
    case c_m3Type_f64: { double d; __builtin_memcpy(&d, &ret, 8);
                         return JS_NewFloat64(cx, d); }
    default: return JS_UNDEFINED;
    }
}

/* wasmCall(id, name, argsArray) -> result | undefined | throws. */
static JSValue js_wasm_call(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    if (!w) return JS_ThrowTypeError(cx, "wasmCall: bad instance");
    const char *name = JS_ToCString(cx, argv[1]);
    if (!name) return JS_ThrowTypeError(cx, "wasmCall: bad name");
    IM3Function f = NULL;
    M3Result r = m3_FindFunction(&f, w->rt, name);
    JS_FreeCString(cx, name);
    if (r || !f) return JS_ThrowTypeError(cx, "wasm export not found");
    return wasm_invoke(cx, w, f, (argc >= 3) ? argv[2] : JS_UNDEFINED);
}

/* wasmTableLen(id) -> number of entries in the exported table. */
static JSValue js_wasm_tablelen(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    return JS_NewInt32(cx, w ? toby_wasm_table_size(w->mod) : 0);
}

/* wasmTableName(id) -> exported-table name or "". */
static JSValue js_wasm_tablename(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    const char *nm = w ? toby_wasm_table_export_name(w->mod) : NULL;
    return JS_NewString(cx, nm ? nm : "");
}

/* wasmTableCall(id, index, argsArray) -> result | undefined | throws.
 * call_indirect from JS: resolve the function at a table slot and call it. */
static JSValue js_wasm_tablecall(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    if (!w) return JS_ThrowTypeError(cx, "wasmTableCall: bad instance");
    uint32_t idx = (argc >= 2) ? (uint32_t)jsi(cx, argv[1]) : 0;
    IM3Function f = NULL;
    M3Result r = m3_GetTableFunction(&f, w->mod, idx);
    if (r || !f) return JS_ThrowTypeError(cx, "wasm table entry not callable");
    return wasm_invoke(cx, w, f, (argc >= 3) ? argv[2] : JS_UNDEFINED);
}

/* wasmGlobalNames(id) -> [names...] of exported globals. */
static JSValue js_wasm_globalnames(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    JSValue a = JS_NewArray(cx);
    if (!w) return a;
    int ng = toby_wasm_num_globals(w->mod), n = 0;
    for (int i = 0; i < ng; i++) {
        const char *nm = toby_wasm_global_export_name(w->mod, i);
        if (nm && nm[0]) JS_SetPropertyUint32(cx, a, n++, JS_NewString(cx, nm));
    }
    return a;
}

/* wasmGlobalGet(id, name) -> the global's current value (by type). */
static JSValue js_wasm_globalget(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    if (!w) return JS_UNDEFINED;
    const char *name = JS_ToCString(cx, argv[1]);
    if (!name) return JS_UNDEFINED;
    IM3Global g = m3_FindGlobal(w->mod, name);
    JS_FreeCString(cx, name);
    if (!g) return JS_UNDEFINED;
    M3TaggedValue tv;
    if (m3_GetGlobal(g, &tv)) return JS_UNDEFINED;
    switch (tv.type) {
    case c_m3Type_i32: return JS_NewInt32(cx, (int32_t)tv.value.i32);
    case c_m3Type_i64: return JS_NewBigInt64(cx, (int64_t)tv.value.i64);
    case c_m3Type_f32: return JS_NewFloat64(cx, (double)tv.value.f32);
    case c_m3Type_f64: return JS_NewFloat64(cx, tv.value.f64);
    default: return JS_UNDEFINED;
    }
}

/* wasmGlobalSet(id, name, value): set a mutable global (throws if not). */
static JSValue js_wasm_globalset(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    if (!w) return JS_ThrowTypeError(cx, "wasmGlobalSet: bad instance");
    const char *name = JS_ToCString(cx, argv[1]);
    if (!name) return JS_ThrowTypeError(cx, "wasmGlobalSet: bad name");
    IM3Global g = m3_FindGlobal(w->mod, name);
    if (!g) { JS_FreeCString(cx, name); return JS_ThrowTypeError(cx, "no such global"); }
    JS_FreeCString(cx, name);
    JSValueConst v = (argc >= 3) ? argv[2] : JS_UNDEFINED;
    M3TaggedValue tv; tv.type = m3_GetGlobalType(g);
    switch (tv.type) {
    case c_m3Type_i32: { int32_t x = 0; JS_ToInt32(cx, &x, v); tv.value.i32 = (uint32_t)x; break; }
    case c_m3Type_i64: { int64_t x = 0; if (JS_IsBigInt(cx, v)) JS_ToBigInt64(cx, &x, v);
                         else JS_ToInt64(cx, &x, v); tv.value.i64 = (uint64_t)x; break; }
    case c_m3Type_f32: { double d = 0; JS_ToFloat64(cx, &d, v); tv.value.f32 = (float)d; break; }
    case c_m3Type_f64: { double d = 0; JS_ToFloat64(cx, &d, v); tv.value.f64 = d; break; }
    default: break;
    }
    M3Result r = m3_SetGlobal(g, &tv);
    if (r) return JS_ThrowTypeError(cx, "wasm global set: %s", r);
    return JS_UNDEFINED;
}

/* If the memory base has moved since the cached buffer was issued (a grow
 * reallocated it), detach the stale buffer so JS views over it fault. */
static void wasm_sync_membuf(struct wasm_inst *w) {
    if (!w || !w->has_mem_buf) return;
    uint32_t sz = 0;
    uint8_t *mp = m3_GetMemory(w->rt, &sz, 0);
    if (mp != w->mem_base) wasm_drop_membuf(w, 1);
}

/* wasmMemBuffer(id) -> ArrayBuffer aliasing the module's linear memory
 * (writes reflect into wasm). Cached so its identity is stable between
 * grows; a grow detaches it and the next call issues a fresh one over the
 * new base. */
static JSValue js_wasm_membuffer(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    if (!w) return JS_NULL;
    uint32_t sz = 0;
    uint8_t *mp = m3_GetMemory(w->rt, &sz, 0);
    if (!mp || !sz) return JS_NULL;
    if (w->has_mem_buf) {
        if (w->mem_base == mp) return JS_DupValue(cx, w->mem_buf);
        wasm_drop_membuf(w, 1);            /* memory moved: detach the old */
    }
    /* free_func NULL: the buffer aliases wasm-owned memory; QuickJS must
     * not free it. */
    JSValue buf = JS_NewArrayBuffer(cx, mp, sz, NULL, NULL, 0);
    w->mem_buf = JS_DupValue(cx, buf);
    w->has_mem_buf = 1;
    w->mem_base = mp;
    return buf;
}

/* wasmMemPages(id) -> current linear-memory size in 64 KiB pages. */
static JSValue js_wasm_mempages(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    return JS_NewInt32(cx, w ? toby_wasm_mem_pages(w->rt) : 0);
}

/* wasmMemGrow(id, deltaPages) -> previous page count, or -1 if it can't
 * grow. Detaches the cached ArrayBuffer (the realloc moves the base). */
static JSValue js_wasm_memgrow(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    if (!w) return JS_NewInt32(cx, -1);
    int delta = (argc >= 2) ? jsi(cx, argv[1]) : 0;
    int old = toby_wasm_mem_grow(w->rt, delta);
    if (old >= 0) wasm_drop_membuf(w, 1);
    return JS_NewInt32(cx, old);
}

/* wasmFree(id): release an instance (prelude calls this if a page drops it). */
static JSValue js_wasm_free(JSContext *cx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)cx; (void)t;
    struct wasm_inst *w = wasm_get(argc >= 1 ? jsi(cx, argv[0]) : -1);
    if (w) wasm_inst_free(w);
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
static int worker_pump(void);
static void workers_free_for(JSContext *main_cx);

static void js_teardown(struct tab *t) {
    if (!t->js_cx) { t->js_rt = NULL; return; }
    JSContext *cx = t->js_cx;
    workers_free_for(cx);          /* reap this page's Web Workers */
    wasm_free_for_context(cx);     /* reap this page's WebAssembly instances */
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
"  var __upg = [];\n"          /* custom-element upgrade stack */
"  function Element(i){\n"
"    if (i === undefined && __upg.length) i = __upg.pop();\n"
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
"      set: function(v){ D.setHTML(this.__i, String(v)); __upgradeUnder(this.__i); }\n"
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
"    ownerDocument: { get: function(){ return g.document; } },\n"
"    content: { get: function(){\n"          /* <template>.content */
"      if (D.tag(this.__i) !== 'template') return null;\n"
"      var ti = this.__i;\n"
"      return { __tplFrag: ti,\n"
"               cloneNode: function(){ return { __tplFrag: ti }; } };\n"
"    } }\n"
"  });\n"
"  Element.prototype.cloneNode = function(deep){ return el(D.clone(this.__i)); };\n"
"  Element.prototype.attachShadow = function(o){\n"
"    var sr = el(D.attachShadow(this.__i));\n"
"    if (sr){\n"
"      var self = this;\n"
"      try { Object.defineProperty(sr, 'host', { get: function(){ return self; } }); } catch(e){}\n"
"    }\n"
"    this.__sr = sr;\n"
"    return sr;\n"
"  };\n"
"  Object.defineProperty(Element.prototype, 'shadowRoot', {\n"
"    get: function(){ return this.__sr || null; }\n"
"  });\n"
"  /* ---- Web components: custom elements registry + upgrades ---- */\n"
"  g.HTMLElement = Element;\n"
"  var __ce = {};\n"
"  function __isConn(i){\n"
"    var b = D.body();\n"
"    for (var n2 = i; n2 >= 0; n2 = D.parent(n2)) if (n2 === b) return true;\n"
"    return false;\n"
"  }\n"
"  function __mkCustom(name, i){\n"
"    var C = __ce[name];\n"
"    __upg.push(i);\n"
"    var inst;\n"
"    try { inst = new C(); } catch(e){ g.console.log('[ce-err]', e); inst = null; }\n"
"    if (!inst || inst.__i !== i){ __upg.length = 0; inst = inst || new Element(i); inst.__i = i; }\n"
"    wrap.set(i, inst);\n"
"    return inst;\n"
"  }\n"
"  function __fireConn(inst){\n"
"    if (inst.__ceConn) return;\n"
"    inst.__ceConn = true;\n"
"    if (typeof inst.connectedCallback === 'function'){\n"
"      try { inst.connectedCallback(); } catch(e){ g.console.log('[ce-err]', e); }\n"
"    }\n"
"  }\n"
"  function __upgradeUnder(root){\n"
"    for (var name in __ce){\n"
"      var i = 0;\n"
"      while ((i = D.byTagFrom(name, i + 1)) >= 0){\n"
"        if (root >= 0){\n"
"          var ok = false;\n"
"          for (var n2 = i; n2 >= 0; n2 = D.parent(n2)) if (n2 === root){ ok = true; break; }\n"
"          if (!ok) continue;\n"
"        }\n"
"        var w = wrap.get(i);\n"
"        var inst = (w && w instanceof __ce[name]) ? w : __mkCustom(name, i);\n"
"        if (__isConn(i)) __fireConn(inst);\n"
"      }\n"
"    }\n"
"  }\n"
"  g.customElements = {\n"
"    define: function(name, C){\n"
"      name = String(name).toLowerCase();\n"
"      __ce[name] = C;\n"
"      __upgradeUnder(-1);\n"
"    },\n"
"    get: function(name){ return __ce[String(name).toLowerCase()]; },\n"
"    whenDefined: function(){ return Promise.resolve(); }\n"
"  };\n"
"  Element.prototype.appendChild = function(c){\n"
"    if (c && c.__tplFrag !== undefined){\n"
"      var kids = D.childNodes(c.__tplFrag);\n"
"      for (var k = 0; k < kids.length; k++)\n"
"        D.append(this.__i, D.clone(kids[k]));\n"
"      __upgradeUnder(this.__i);\n"
"      return c;\n"
"    }\n"
"    D.append(this.__i, c.__i);\n"
"    __upgradeUnder(c.__i);\n"
"    return c;\n"
"  };\n"
"  Element.prototype.removeChild = function(c){\n"
"    D.remove(c.__i);\n"
"    var w = wrap.get(c.__i);\n"
"    if (w && w.__ceConn){\n"
"      w.__ceConn = false;\n"
"      if (typeof w.disconnectedCallback === 'function'){\n"
"        try { w.disconnectedCallback(); } catch(e){}\n"
"      }\n"
"    }\n"
"    return c;\n"
"  };\n"
"  Element.prototype.remove = function(){ D.remove(this.__i); };\n"
"  Element.prototype.setAttribute = function(n, v){\n"
"    n = String(n); v = String(v);\n"
"    var old = D.getAttr(this.__i, n);\n"
"    D.setAttr(this.__i, n, v);\n"
"    var C2 = this.constructor;\n"
"    if (C2 && C2.observedAttributes && C2.observedAttributes.indexOf(n) >= 0 &&\n"
"        typeof this.attributeChangedCallback === 'function'){\n"
"      try { this.attributeChangedCallback(n, old, v); }\n"
"      catch(e){ g.console.log('[ce-err]', e); }\n"
"    }\n"
"  };\n"
"  Element.prototype.getAttribute = function(n){ return D.getAttr(this.__i, String(n)); };\n"
"  Element.prototype.querySelector = function(s){ return el(D.query(String(s))); };\n"
"  Element.prototype.insertBefore = function(c, ref){\n"
"    D.insBefore(this.__i, c.__i, ref ? ref.__i : -1);\n"
"    __upgradeUnder(c.__i);\n"
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
"    createElement: function(t){\n"
"      t = String(t).toLowerCase();\n"
"      var i = D.create(t);\n"
"      if (__ce[t]) return __mkCustom(t, i);\n"
"      return el(i);\n"
"    },\n"
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
/* self/globalThis are the global object on the main page too, not just
 * in workers -- real scripts (e.g. bbc.com) read `self` and threw
 * ReferenceError without this. */
"  g.self = g; g.globalThis = g;\n"
"  g.frames = g; g.parent = g; g.top = g;\n"
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
"  /* ---- fetch()/XHR: real methods+headers+body + CORS (parity) ---- */\n"
"  function __origin(u){\n"                /* scheme://host[:port] of a URL */
"    var m = /^([a-z]+:\\/\\/[^\\/?#]*)/i.exec(String(u));\n"
"    return m ? m[1].toLowerCase() : '';\n"
"  }\n"
"  function __pageOrigin(){ return __origin(g.location ? g.location.href : ''); }\n"
"  function __parseHeaders(raw){\n"        /* raw block -> lowercased map */
"    var h = {};\n"
"    if (!raw) return h;\n"
"    var lines = String(raw).split(/\\r?\\n/);\n"
"    for (var i = 1; i < lines.length; i++){\n"  /* skip status line */
"      var c = lines[i].indexOf(':');\n"
"      if (c < 0) continue;\n"
"      var k = lines[i].slice(0, c).trim().toLowerCase();\n"
"      var v = lines[i].slice(c + 1).trim();\n"
"      if (k) h[k] = h[k] ? (h[k] + ', ' + v) : v;\n"
"    }\n"
"    return h;\n"
"  }\n"
"  function Headers(map){ this.__m = map || {}; }\n"
"  Headers.prototype.get = function(k){\n"
"    var v = this.__m[String(k).toLowerCase()];\n"
"    return (v === undefined) ? null : v;\n"
"  };\n"
"  Headers.prototype.has = function(k){ return this.__m[String(k).toLowerCase()] !== undefined; };\n"
"  Headers.prototype.forEach = function(cb){\n"
"    for (var k in this.__m) cb(this.__m[k], k, this);\n"
"  };\n"
"  var __simpleReqH = {'accept':1,'accept-language':1,'content-language':1,'content-type':1};\n"
"  function __corsAllows(reqOrigin, respMap, method, reqHeaderNames){\n"
"    var ao = respMap['access-control-allow-origin'];\n"
"    if (ao !== '*' && ao !== reqOrigin) return false;\n"
"    return true;\n"           /* v1: origin check (preflight is separate) */
"  }\n"
"  g.Headers = Headers;\n"
"  g.fetch = function(u, opt){\n"
"    opt = opt || {};\n"
"    var url = String(u);\n"
"    var abs = /^[a-z]+:\\/\\//i.test(url) ? url\n"
"            : (/^\\//.test(url) ? __pageOrigin() + url : url);\n"
"    var target = __origin(abs);\n"
"    var pageO = __pageOrigin();\n"
"    var cross = target && pageO && target !== pageO;\n"
"    var method = (opt.method ? String(opt.method) : 'GET').toUpperCase();\n"
"    var hdrArr = [], hdrNames = [];\n"
"    if (opt.headers){\n"
"      var hh = opt.headers;\n"
"      if (hh instanceof Headers) hh = hh.__m;\n"
"      for (var k in hh){ hdrArr.push(k + ': ' + hh[k]); hdrNames.push(String(k).toLowerCase()); }\n"
"    }\n"
"    var body = (opt.body !== undefined && opt.body !== null) ? String(opt.body) : null;\n"
"    if (body && hdrNames.indexOf('content-type') < 0){\n"
"      hdrArr.push('Content-Type: text/plain;charset=UTF-8');\n"
"    }\n"
"    /* credentials: same-origin default -> cookies only same-origin. */\n"
"    var creds = opt.credentials || 'same-origin';\n"
"    var noCookies = cross ? (creds !== 'include') : (creds === 'omit');\n"
"    return new Promise(function(res, rej){\n"
"      D.fetchStart2(abs, function(r){\n"
"        if (!r || r.status <= 0){ rej(new Error('fetch failed')); return; }\n"
"        var map = __parseHeaders(r.headers);\n"
"        if (cross && !__corsAllows(pageO, map, method, hdrNames)){\n"
"          rej(new TypeError('Failed to fetch: CORS blocked (' + target + ')'));\n"
"          return;\n"
"        }\n"
"        var used = false;\n"
"        function once(){ if (used) throw new TypeError('body used'); used = true; }\n"
"        res({\n"
"          ok: r.status >= 200 && r.status < 300,\n"
"          status: r.status, statusText: '', redirected: r.url !== abs,\n"
"          url: r.url, type: cross ? 'cors' : 'basic',\n"
"          headers: new Headers(map),\n"
"          text: function(){ once(); return Promise.resolve(r.body); },\n"
"          json: function(){ once(); return Promise.resolve(JSON.parse(r.body)); },\n"
"          arrayBuffer: function(){ once();\n"
"            return Promise.resolve(r.bodyBytes ? r.bodyBytes.slice(0) : new ArrayBuffer(0)); },\n"
"          blob: function(){ once();\n"
"            return Promise.resolve(new Blob([r.bodyBytes || new ArrayBuffer(0)])); },\n"
"          get body(){ return __mkBodyStream(r.bodyBytes); },\n"
"          clone: function(){ return this; }\n"
"        });\n"
"      }, method, hdrArr.join('\\r\\n'), body, noCookies);\n"
"    });\n"
"  };\n"
"  g.XMLHttpRequest = function(){\n"
"    this.readyState = 0; this.status = 0; this.statusText = '';\n"
"    this.responseText = ''; this.response = ''; this.responseType = '';\n"
"    this.__hdr = []; this.__hn = []; this.__rh = {};\n"
"    this.onreadystatechange = null; this.onload = null; this.onerror = null;\n"
"  };\n"
"  g.XMLHttpRequest.prototype.open = function(m, u){\n"
"    this.__m = String(m || 'GET').toUpperCase(); this.__u = String(u);\n"
"    this.readyState = 1;\n"
"    if (this.onreadystatechange) this.onreadystatechange();\n"
"  };\n"
"  g.XMLHttpRequest.prototype.setRequestHeader = function(n, v){\n"
"    this.__hdr.push(n + ': ' + v); this.__hn.push(String(n).toLowerCase());\n"
"  };\n"
"  g.XMLHttpRequest.prototype.getResponseHeader = function(n){\n"
"    var v = this.__rh[String(n).toLowerCase()]; return v === undefined ? null : v;\n"
"  };\n"
"  g.XMLHttpRequest.prototype.getAllResponseHeaders = function(){\n"
"    var s = ''; for (var k in this.__rh) s += k + ': ' + this.__rh[k] + '\\r\\n'; return s;\n"
"  };\n"
"  g.XMLHttpRequest.prototype.send = function(bodyArg){\n"
"    var self = this;\n"
"    var abs = /^[a-z]+:\\/\\//i.test(this.__u) ? this.__u\n"
"            : (/^\\//.test(this.__u) ? __pageOrigin() + this.__u : this.__u);\n"
"    var cross = __origin(abs) && __pageOrigin() && __origin(abs) !== __pageOrigin();\n"
"    var body = (bodyArg !== undefined && bodyArg !== null) ? String(bodyArg) : null;\n"
"    D.fetchStart2(abs, function(r){\n"
"      if (!r || r.status <= 0){\n"
"        self.status = 0; self.readyState = 4;\n"
"        if (self.onerror) self.onerror();\n"
"        if (self.onreadystatechange) self.onreadystatechange();\n"
"        return;\n"
"      }\n"
"      self.__rh = __parseHeaders(r.headers);\n"
"      if (cross){\n"
"        var ao = self.__rh['access-control-allow-origin'];\n"
"        if (ao !== '*' && ao !== __pageOrigin()){\n"
"          self.status = 0; self.readyState = 4;\n"
"          if (self.onerror) self.onerror();\n"
"          if (self.onreadystatechange) self.onreadystatechange();\n"
"          return;\n"
"        }\n"
"      }\n"
"      self.status = r.status; self.statusText = '';\n"
"      var rt = self.responseType || '';\n"
"      self.responseText = (rt === '' || rt === 'text') ? r.body : '';\n"
"      if (rt === 'arraybuffer') self.response = r.bodyBytes ? r.bodyBytes.slice(0) : new ArrayBuffer(0);\n"
"      else if (rt === 'blob') self.response = new Blob([r.bodyBytes || new ArrayBuffer(0)]);\n"
"      else if (rt === 'json') { try { self.response = JSON.parse(r.body); } catch(e){ self.response = null; } }\n"
"      else self.response = r.body;\n"
"      self.readyState = 4;\n"
"      if (self.onreadystatechange) self.onreadystatechange();\n"
"      if (self.onload) self.onload();\n"
"    }, this.__m, this.__hdr.join('\\r\\n'), body,\n"
"       cross ? !this.withCredentials : false);\n"
"  };\n"
/* ---- Web Workers (chrome-parity) --------------------------------- */
"  function Worker(url){\n"
"    this.onmessage = null; this.onerror = null;\n"
"    this.__h = D.workerNew(String(url), this);\n"
"    if (this.__h < 0) throw new Error('Worker: failed to load ' + url);\n"
"  }\n"
"  Worker.prototype.postMessage = function(msg){\n"
"    if (this.__h >= 0) D.workerPost(this.__h, JSON.stringify(msg === undefined ? null : msg));\n"
"  };\n"
"  Worker.prototype.terminate = function(){\n"
"    if (this.__h >= 0){ D.workerTerminate(this.__h); this.__h = -1; }\n"
"  };\n"
"  Worker.prototype.addEventListener = function(t, f){\n"
"    if (String(t) === 'message') this.onmessage = f;\n"
"    else if (String(t) === 'error') this.onerror = f;\n"
"  };\n"
"  Worker.prototype.removeEventListener = function(){};\n"
"  Worker.prototype.__deliver = function(json){\n"    /* C calls this */
"    if (this.onmessage){\n"
"      try { this.onmessage({ data: JSON.parse(json) }); }\n"
"      catch(e){ g.console.log('[worker-msg-err]', e); }\n"
"    }\n"
"  };\n"
"  g.Worker = Worker;\n"
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
/* ---- performance: real sites instrument themselves constantly
 *      (github.com threw 63 ReferenceErrors without this). now() is
 *      ms since page start; the mark/measure entry log is a stub. --- */
"  var __perfT0 = Date.now();\n"
"  g.performance = {\n"
"    timeOrigin: __perfT0,\n"
"    now: function(){ return Date.now() - __perfT0; },\n"
"    mark: function(){}, measure: function(){},\n"
"    clearMarks: function(){}, clearMeasures: function(){},\n"
"    getEntries: function(){ return []; },\n"
"    getEntriesByName: function(){ return []; },\n"
"    getEntriesByType: function(){ return []; },\n"
"    setResourceTimingBufferSize: function(){},\n"
"    toJSON: function(){ return {}; }\n"
"  };\n"
/* ---- Image(): preloaders, tracking pixels and lazy-loaders build
 *      detached <img> via `new Image()`. Fire load synchronously on
 *      src-set so waiters progress instead of hanging. ------------- */
"  g.Image = function(w,h){\n"
"    var self = { tagName:'IMG', nodeName:'IMG', width:w||0, height:h||0,\n"
"      naturalWidth:0, naturalHeight:0, complete:false, __ls:{}, style:{},\n"
"      addEventListener:function(t,f){ (this.__ls[t]=this.__ls[t]||[]).push(f); },\n"
"      removeEventListener:function(t,f){ var l=this.__ls[t]; if(l){var i=l.indexOf(f); if(i>=0)l.splice(i,1);} },\n"
"      setAttribute:function(k,v){ this[k]=v; if(k==='src') this.src=v; },\n"
"      getAttribute:function(k){ return this[k]; } };\n"
"    Object.defineProperty(self,'src',{ configurable:true,\n"
"      get:function(){ return this.__src||''; },\n"
"      set:function(v){ this.__src=v; this.complete=true;\n"
"        var s=this, ev={type:'load',target:s};\n"
"        if(typeof s.onload==='function'){ try{ s.onload(ev); }catch(e){} }\n"
"        var l=s.__ls['load']; if(l) for(var i=0;i<l.length;i++){ try{ l[i](ev); }catch(e){} } } });\n"
"    return self;\n"
"  };\n"
"  function parseUrl(h){\n"
"    var m = /^(https?:)\\/\\/([^\\/?#]*)([^?#]*)(\\??[^#]*)(#?.*)$/.exec(h) || [];\n"
"    return { protocol: m[1] || '', host: m[2] || '', pathname: m[3] || '/',\n"
"             search: m[4] || '', hash: m[5] || '' };\n"
"  }\n"
/* ---- URLSearchParams + URL (WHATWG). Real scripts (mdn, bbc, ...)
 *      construct `new URL(...)`/`new URLSearchParams(...)` constantly and
 *      threw ReferenceError without these. ------------------------- */
"  function USP(init){\n"
"    this.__p = [];\n"
"    var self = this;\n"
"    function add(s){ if(s.charAt(0)==='?') s=s.slice(1); if(!s) return;\n"
"      var a=s.split('&'); for(var i=0;i<a.length;i++){ if(!a[i]) continue;\n"
"        var e=a[i].indexOf('='); var k=e<0?a[i]:a[i].slice(0,e); var v=e<0?'':a[i].slice(e+1);\n"
"        try{ k=decodeURIComponent(k.replace(/\\+/g,' ')); v=decodeURIComponent(v.replace(/\\+/g,' ')); }catch(x){}\n"
"        self.__p.push([k,v]); } }\n"
"    if(typeof init==='string') add(init);\n"
"    else if(init && init.__p){ for(var i=0;i<init.__p.length;i++) this.__p.push([init.__p[i][0],init.__p[i][1]]); }\n"
"    else if(init && init.length){ for(var i=0;i<init.length;i++) this.__p.push([String(init[i][0]),String(init[i][1])]); }\n"
"    else if(init){ for(var k in init) if(Object.prototype.hasOwnProperty.call(init,k)) this.__p.push([k,String(init[k])]); }\n"
"  }\n"
"  USP.prototype.get=function(k){ for(var i=0;i<this.__p.length;i++) if(this.__p[i][0]===k) return this.__p[i][1]; return null; };\n"
"  USP.prototype.getAll=function(k){ var r=[]; for(var i=0;i<this.__p.length;i++) if(this.__p[i][0]===k) r.push(this.__p[i][1]); return r; };\n"
"  USP.prototype.has=function(k){ return this.get(k)!==null; };\n"
"  USP.prototype.append=function(k,v){ this.__p.push([String(k),String(v)]); };\n"
"  USP.prototype.set=function(k,v){ var done=false; for(var i=this.__p.length-1;i>=0;i--){ if(this.__p[i][0]===k){ if(done) this.__p.splice(i,1); else { this.__p[i][1]=String(v); done=true; } } } if(!done) this.append(k,v); };\n"
"  USP.prototype['delete']=function(k){ for(var i=this.__p.length-1;i>=0;i--) if(this.__p[i][0]===k) this.__p.splice(i,1); };\n"
"  USP.prototype.forEach=function(f,t){ for(var i=0;i<this.__p.length;i++) f.call(t,this.__p[i][1],this.__p[i][0],this); };\n"
"  USP.prototype.keys=function(){ return this.__p.map(function(x){return x[0];}); };\n"
"  USP.prototype.values=function(){ return this.__p.map(function(x){return x[1];}); };\n"
"  USP.prototype.entries=function(){ return this.__p.map(function(x){return [x[0],x[1]];}); };\n"
"  USP.prototype.toString=function(){ return this.__p.map(function(x){ return encodeURIComponent(x[0])+'='+encodeURIComponent(x[1]); }).join('&'); };\n"
"  g.URLSearchParams = USP;\n"
"  function resolveRel(url, base){\n"
"    if(/^[a-zA-Z][a-zA-Z0-9+.\\-]*:/.test(url)) return url;\n"        /* already absolute */
"    var bm = /^([a-zA-Z][a-zA-Z0-9+.\\-]*:)\\/\\/([^\\/?#]*)([^?#]*)(\\?[^#]*)?(#.*)?$/.exec(base||'');\n"
"    if(!bm) return url;\n"
"    var scheme=bm[1], host=bm[2], bpath=bm[3]||'/';\n"
"    if(url.slice(0,2)==='//') return scheme+url;\n"
"    var path;\n"
"    if(url.charAt(0)==='/') path=url;\n"
"    else if(url.charAt(0)==='?'||url.charAt(0)==='#'){ return scheme+'//'+host+bpath+url; }\n"
"    else { var i=bpath.lastIndexOf('/'); path=(i<0?'/':bpath.slice(0,i+1))+url; }\n"
"    var segs=path.split('/'), out=[];\n"
"    for(var s=0;s<segs.length;s++){ if(segs[s]==='.') continue; if(segs[s]==='..'){ if(out.length>1) out.pop(); } else out.push(segs[s]); }\n"
"    return scheme+'//'+host+out.join('/');\n"
"  }\n"
"  function URLc(url, base){\n"
"    url=String(url);\n"
"    if(base!==undefined && base!==null) url=resolveRel(url, base.href!==undefined?base.href:String(base));\n"
"    var m=/^([a-zA-Z][a-zA-Z0-9+.\\-]*:)\\/\\/(([^:@\\/?#]*)(?::([^@\\/?#]*))?@)?([^:\\/?#]*)(?::(\\d+))?([^?#]*)(\\?[^#]*)?(#.*)?$/.exec(url);\n"
"    if(!m) throw new TypeError('Invalid URL: '+url);\n"
"    this.protocol=m[1]||''; this.username=m[3]||''; this.password=m[4]||'';\n"
"    this.hostname=m[5]||''; this.port=m[6]||''; this.pathname=m[7]||'/';\n"
"    this.search=m[8]||''; this.hash=m[9]||'';\n"
"    this.host=this.hostname+(this.port?(':'+this.port):'');\n"
"    this.origin=this.protocol+'//'+this.host;\n"
"    this.searchParams=new USP(this.search);\n"
"  }\n"
"  URLc.prototype.toString=function(){ var s=this.searchParams.toString();\n"
"    return this.origin+this.pathname+(s?('?'+s):'')+(this.hash||''); };\n"
"  Object.defineProperty(URLc.prototype,'href',{ get:function(){ return this.toString(); },\n"
"    set:function(v){ URLc.call(this, v); } });\n"
"  g.URL = URLc;\n"
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
/* ---- Encoding + Streams (fetch bodies, binary) -------------------- */
"  if (!g.TextEncoder){\n"
"    g.TextEncoder = function(){ this.encoding='utf-8'; };\n"
"    g.TextEncoder.prototype.encode = function(str){\n"
"      str = String(str === undefined ? '' : str); var out = [];\n"
"      for (var i=0;i<str.length;i++){ var c = str.charCodeAt(i);\n"
"        if (c < 0x80) out.push(c);\n"
"        else if (c < 0x800) out.push(0xC0|(c>>6), 0x80|(c&0x3F));\n"
"        else if (c >= 0xD800 && c <= 0xDBFF){ var c2 = str.charCodeAt(++i);\n"
"          var cp = 0x10000 + ((c&0x3FF)<<10) + (c2&0x3FF);\n"
"          out.push(0xF0|(cp>>18),0x80|((cp>>12)&0x3F),0x80|((cp>>6)&0x3F),0x80|(cp&0x3F)); }\n"
"        else out.push(0xE0|(c>>12),0x80|((c>>6)&0x3F),0x80|(c&0x3F)); }\n"
"      return new Uint8Array(out); };\n"
"  }\n"
"  if (!g.TextDecoder){\n"
"    g.TextDecoder = function(){ this.encoding='utf-8'; };\n"
"    g.TextDecoder.prototype.decode = function(buf){\n"
"      if (!buf) return '';\n"
"      var u = (buf instanceof Uint8Array) ? buf\n"
"            : (buf.buffer ? new Uint8Array(buf.buffer, buf.byteOffset||0, buf.byteLength)\n"
"                          : new Uint8Array(buf));\n"
"      var s = '', i = 0;\n"
"      while (i < u.length){ var c = u[i++];\n"
"        if (c < 0x80) s += String.fromCharCode(c);\n"
"        else if (c >= 0xF0){ var cp = ((c&0x07)<<18)|((u[i++]&0x3F)<<12)|((u[i++]&0x3F)<<6)|(u[i++]&0x3F);\n"
"          cp -= 0x10000; s += String.fromCharCode(0xD800+(cp>>10),0xDC00+(cp&0x3FF)); }\n"
"        else if (c >= 0xE0) s += String.fromCharCode(((c&0x0F)<<12)|((u[i++]&0x3F)<<6)|(u[i++]&0x3F));\n"
"        else s += String.fromCharCode(((c&0x1F)<<6)|(u[i++]&0x3F)); }\n"
"      return s; };\n"
"  }\n"
/* ReadableStream: an async queue -- read() returns a pending promise when
 * no chunk is buffered yet, resolved by a later enqueue/close, so async
 * producers (tee, transform) work, not just fully-buffered body streams. */
"  function ReadableStream(src){\n"
"    var self = this; this._q = []; this._closed = false; this._err = null;\n"
"    this._src = src || {}; this._started = false; this._wait = [];\n"
"    this._ctl = {\n"
"      enqueue: function(c){ if(self._wait.length) self._wait.shift()({value:c,done:false}); else self._q.push(c); },\n"
"      close: function(){ self._closed = true; while(self._wait.length) self._wait.shift()({value:undefined,done:true}); },\n"
"      error: function(e){ self._err=e; self._closed=true; var w=self._wait; self._wait=[]; for(var i=0;i<w.length;i++) w[i]({value:undefined,done:true}); } };\n"
"  }\n"
"  ReadableStream.prototype._start = function(){\n"
"    if (this._started) return; this._started = true;\n"
"    try { if (typeof this._src.start === 'function') this._src.start(this._ctl); }\n"
"    catch(e){ this._err = e; this._closed = true; }\n"
"  };\n"
"  ReadableStream.prototype.getReader = function(){\n"
"    var self = this;\n"
"    return { read: function(){ self._start();\n"
"        if (!self._q.length && !self._closed && typeof self._src.pull === 'function'){\n"
"          try { self._src.pull(self._ctl); } catch(e){ self._err=e; self._closed=true; } }\n"
"        if (self._q.length) return Promise.resolve({ value: self._q.shift(), done: false });\n"
"        if (self._err) return Promise.reject(self._err);\n"
"        if (self._closed) return Promise.resolve({ value: undefined, done: true });\n"
"        return new Promise(function(res){ self._wait.push(res); }); },\n"
"      releaseLock: function(){},\n"
"      cancel: function(){ self._closed = true; return Promise.resolve(); } };\n"
"  };\n"
"  ReadableStream.prototype.cancel = function(){ this._closed = true; return Promise.resolve(); };\n"
"  ReadableStream.prototype.pipeTo = function(writable){\n"
"    var reader = this.getReader(), writer = writable.getWriter();\n"
"    return new Promise(function(res, rej){\n"
"      function step(){ reader.read().then(function(r){\n"
"        if (r.done){ writer.close().then(res, rej); return; }\n"
"        Promise.resolve(writer.write(r.value)).then(step, rej); }, rej); }\n"
"      step(); });\n"
"  };\n"
"  ReadableStream.prototype.pipeThrough = function(t){ this.pipeTo(t.writable); return t.readable; };\n"
"  ReadableStream.prototype.tee = function(){\n"
"    var reader = this.getReader(), chunks = [];\n"
"    var drain = new Promise(function(res){ function s(){ reader.read().then(function(r){\n"
"      if(r.done){ res(); return; } chunks.push(r.value); s(); }); } s(); });\n"
"    function mk(){ return new ReadableStream({ start:function(c){\n"
"      drain.then(function(){ for(var i=0;i<chunks.length;i++) c.enqueue(chunks[i]); c.close(); }); } }); }\n"
"    return [mk(), mk()];\n"
"  };\n"
"  g.ReadableStream = ReadableStream;\n"
/* WritableStream with backpressure: chunks queue up to highWaterMark and
 * are handed to the sink one at a time (awaiting each sink.write); the
 * writer's `ready` promise gates producers while the queue is full, and
 * `desiredSize` reflects the remaining room. */
"  function WritableStream(sink, strategy){ this._sink = sink || {};\n"
"    this._hwm = (strategy && strategy.highWaterMark != null) ? strategy.highWaterMark : 1;\n"
"    this._q = []; this._writing = false; this._started = false; this._closeReq = null;\n"
"    this._readyRes = null; this._ready = Promise.resolve(); }\n"
"  WritableStream.prototype._start = function(){ if(this._started) return; this._started=true;\n"
"    try { if(typeof this._sink.start==='function') this._sink.start(); } catch(e){} };\n"
"  WritableStream.prototype._desired = function(){ return this._hwm - this._q.length - (this._writing?1:0); };\n"
"  WritableStream.prototype._wake = function(){ if(this._desired() > 0 && this._readyRes){\n"
"    this._readyRes(); this._readyRes = null; this._ready = Promise.resolve(); } };\n"
"  WritableStream.prototype._pump = function(){ var self = this;\n"
"    if (self._writing) return;\n"
"    if (!self._q.length){\n"
"      if (self._closeReq){ var c = self._closeReq; self._closeReq = null;\n"
"        try { Promise.resolve(self._sink.close ? self._sink.close() : undefined).then(c.res, c.rej); }\n"
"        catch(e){ c.rej(e); } }\n"
"      return; }\n"
"    self._writing = true; var it = self._q.shift();\n"
"    Promise.resolve().then(function(){ return self._sink.write ? self._sink.write(it.chunk) : undefined; })\n"
"      .then(function(){ it.res(); self._writing = false; self._wake(); self._pump(); },\n"
"            function(err){ it.rej(err); self._writing = false; self._pump(); }); };\n"
"  WritableStream.prototype.getWriter = function(){ var self = this;\n"
"    return {\n"
"      get desiredSize(){ return self._desired(); },\n"
"      get ready(){ if (self._desired() > 0) return Promise.resolve();\n"
"        if (!self._readyRes) self._ready = new Promise(function(r){ self._readyRes = r; });\n"
"        return self._ready; },\n"
"      write: function(chunk){ self._start();\n"
"        var p = new Promise(function(res, rej){ self._q.push({ chunk:chunk, res:res, rej:rej }); });\n"
"        self._pump(); return p; },\n"
"      close: function(){ self._start();\n"
"        var p = new Promise(function(res, rej){ self._closeReq = { res:res, rej:rej }; });\n"
"        self._pump(); return p; },\n"
"      abort: function(reason){ self._q = [];\n"
"        try { return Promise.resolve(self._sink.abort ? self._sink.abort(reason) : undefined); }\n"
"        catch(e){ return Promise.reject(e); } },\n"
"      releaseLock: function(){}, closed: Promise.resolve() }; };\n"
"  g.WritableStream = WritableStream;\n"
/* TransformStream: a writable that feeds a transformer, and a readable of
 * the transformed chunks. */
"  function TransformStream(tr){ tr = tr || {}; var q = [], rc = null, ended = false;\n"
"    var tc = { enqueue: function(c){ if(rc) rc.enqueue(c); else q.push(c); },\n"
"      terminate: function(){ if(rc) rc.close(); else ended = true; } };\n"
"    this.readable = new ReadableStream({ start: function(c){ rc = c;\n"
"      for(var i=0;i<q.length;i++) c.enqueue(q[i]); q = []; if(ended) c.close(); } });\n"
"    this.writable = new WritableStream({\n"
"      write: function(chunk){ if(tr.transform) return Promise.resolve(tr.transform(chunk, tc)); tc.enqueue(chunk); },\n"
"      close: function(){ if(tr.flush) tr.flush(tc); tc.terminate(); } }); }\n"
"  g.TransformStream = TransformStream;\n"
"  function __mkBodyStream(ab){\n"
"    var bytes = ab ? new Uint8Array(ab.slice(0)) : new Uint8Array(0);\n"
"    return new ReadableStream({ start: function(c){\n"
"      if (bytes.length) c.enqueue(bytes); c.close(); } });\n"
"  }\n"
"  if (!g.Blob){\n"
"    g.Blob = function(parts, opts){ this._parts = parts || [];\n"
"      this.type = (opts && opts.type) || ''; var sz = 0;\n"
"      for (var i=0;i<this._parts.length;i++){ var p=this._parts[i];\n"
"        sz += (p && p.byteLength!==undefined) ? p.byteLength : String(p).length; }\n"
"      this.size = sz; };\n"
"    g.Blob.prototype.arrayBuffer = function(){\n"
"      var u = new Uint8Array(this.size), o = 0;\n"
"      for (var i=0;i<this._parts.length;i++){ var p=this._parts[i];\n"
"        if (p instanceof ArrayBuffer){ u.set(new Uint8Array(p), o); o+=p.byteLength; }\n"
"        else if (p && p.buffer){ u.set(new Uint8Array(p.buffer, p.byteOffset||0, p.byteLength), o); o+=p.byteLength; }\n"
"        else { var s=String(p); for (var j=0;j<s.length;j++) u[o++]=s.charCodeAt(j)&0xFF; } }\n"
"      return Promise.resolve(u.buffer); };\n"
"    g.Blob.prototype.text = function(){ return this.arrayBuffer().then(function(ab){\n"
"      return new TextDecoder().decode(ab); }); };\n"
"  }\n"
/* ---- Response constructor (fetch + service-worker synthesis) ------ */
"  if (!g.Response){\n"
"    g.Response = function(body, init){ init = init || {};\n"
"      this.status = (init.status !== undefined) ? init.status : 200;\n"
"      this.statusText = init.statusText || ''; this.ok = this.status>=200 && this.status<300;\n"
"      this.headers = (init.headers instanceof Headers) ? init.headers : new Headers(init.headers||{});\n"
"      this.url = ''; this.__used = false; var ab;\n"
"      if (body===undefined||body===null) ab = new ArrayBuffer(0);\n"
"      else if (body instanceof ArrayBuffer) ab = body;\n"
"      else if (body && body.buffer instanceof ArrayBuffer)\n"
"        ab = body.buffer.slice(body.byteOffset||0,(body.byteOffset||0)+body.byteLength);\n"
"      else ab = new TextEncoder().encode(String(body)).buffer;\n"
"      this.__ab = ab; };\n"
"    function __ro(r){ if(r.__used) throw new TypeError('body used'); r.__used=true; }\n"
"    g.Response.prototype.text = function(){ __ro(this); return Promise.resolve(new TextDecoder().decode(this.__ab)); };\n"
"    g.Response.prototype.json = function(){ __ro(this); return Promise.resolve(JSON.parse(new TextDecoder().decode(this.__ab))); };\n"
"    g.Response.prototype.arrayBuffer = function(){ __ro(this); return Promise.resolve(this.__ab.slice(0)); };\n"
"    g.Response.prototype.blob = function(){ __ro(this); return Promise.resolve(new Blob([this.__ab])); };\n"
"    Object.defineProperty(g.Response.prototype,'body',{ get: function(){ return __mkBodyStream(this.__ab); } });\n"
"    g.Response.prototype.clone = function(){ var r=Object.create(g.Response.prototype);\n"
"      r.status=this.status;r.statusText=this.statusText;r.ok=this.ok;r.headers=this.headers;\n"
"      r.url=this.url;r.__ab=this.__ab;r.__used=false; return r; };\n"
"  }\n"
/* ---- Cache API (window + service worker) -------------------------- */
"  var __cs = {};\n"
"  function __reqUrl(req){ return (typeof req==='string') ? req : (req&&req.url)?req.url:String(req); }\n"
"  function __respBytes(resp){\n"
"    if (resp.arrayBuffer) return resp.arrayBuffer();\n"
"    if (resp.__ab) return Promise.resolve(resp.__ab);\n"
"    return Promise.resolve(new ArrayBuffer(0)); }\n"
"  function __cachedResponse(v, url){ var r = new Response(v.ab, { status:v.status, statusText:v.st, headers:v.hdrs }); r.url = url||''; return r; }\n"
/* Cache persistence: bytes serialize as a Latin-1 string (round-trips
 * losslessly through idbSave's UTF-8 file I/O). Saved to /data/browser on
 * every mutation; restored at startup so caches survive reboots. */
"  function __ab2s(ab){ var u=new Uint8Array(ab),s=''; for(var i=0;i<u.length;i++) s+=String.fromCharCode(u[i]); return s; }\n"
"  function __s2ab(s){ var u=new Uint8Array(s.length); for(var i=0;i<s.length;i++) u[i]=s.charCodeAt(i)&0xFF; return u.buffer; }\n"
"  function __cacheSave(){ try {\n"
"    var o = {}; for(var n in __cs){ o[n] = {}; var m = __cs[n];\n"
"      for(var u in m){ var e = m[u]; o[n][u] = { s:e.status, t:e.st, h:e.hdrs, b:__ab2s(e.ab) }; } }\n"
"    D.idbSave('__httpcache', JSON.stringify(o)); } catch(e){} }\n"
"  (function __cacheLoad(){ try { var raw = D.idbLoad('__httpcache'); if(!raw) return;\n"
"    var o = JSON.parse(raw); for(var n in o){ __cs[n] = {}; var m = o[n];\n"
"      for(var u in m){ var e = m[u]; __cs[n][u] = { status:e.s, st:e.t, hdrs:e.h||{}, ab:__s2ab(e.b||'') }; } }\n"
"  } catch(e){} })();\n"
"  function __openCache(name){ if(!__cs[name]) __cs[name]={}; var map=__cs[name];\n"
"    var cache={\n"
"      put: function(req,resp){ var u=__reqUrl(req);\n"
"        var hd={}; if(resp.headers&&resp.headers.forEach) resp.headers.forEach(function(v,k){hd[k]=v;});\n"
"        return __respBytes(resp).then(function(ab){ map[u]={status:resp.status||200,st:resp.statusText||'',ab:ab,hdrs:hd}; __cacheSave(); }); },\n"
"      match: function(req){ var u=__reqUrl(req); return Promise.resolve(map[u]?__cachedResponse(map[u],u):undefined); },\n"
"      add: function(req){ var u=__reqUrl(req); return fetch(u).then(function(r){ return cache.put(u,r); }); },\n"
"      addAll: function(reqs){ return Promise.all((reqs||[]).map(function(r){ return cache.add(r); })); },\n"
"      'delete': function(req){ var u=__reqUrl(req); var had=!!map[u]; delete map[u]; __cacheSave(); return Promise.resolve(had); },\n"
"      keys: function(){ return Promise.resolve(Object.keys(map)); } };\n"
"    return cache; }\n"
"  g.caches = {\n"
"    open: function(name){ return Promise.resolve(__openCache(name)); },\n"
"    match: function(req){ var u=__reqUrl(req);\n"
"      for(var n in __cs) if(__cs[n][u]) return Promise.resolve(__cachedResponse(__cs[n][u],u));\n"
"      return Promise.resolve(undefined); },\n"
"    has: function(name){ return Promise.resolve(!!__cs[name]); },\n"
"    'delete': function(name){ var had=!!__cs[name]; delete __cs[name]; __cacheSave(); return Promise.resolve(had); },\n"
"    keys: function(){ return Promise.resolve(Object.keys(__cs)); } };\n"
/* ---- Service worker (register + lifecycle + fetch interception) --- */
"  var __swScope=null, __swReg=null, __realFetch=g.fetch, __readyRes=null;\n"
"  function __swNew(){ var Lst={};\n"
"    var scope={ caches:g.caches, location:g.location, registration:null,\n"
"      addEventListener: function(t,fn){ (Lst[t]=Lst[t]||[]).push(fn); },\n"
"      removeEventListener: function(t,fn){ var a=Lst[t]; if(a){var i=a.indexOf(fn); if(i>=0)a.splice(i,1);} },\n"
"      skipWaiting: function(){ return Promise.resolve(); },\n"
"      clients: { claim:function(){return Promise.resolve();}, matchAll:function(){return Promise.resolve([]);} },\n"
"      __has: function(t){ return !!(Lst[t]&&Lst[t].length); },\n"
"      __fire: function(t,ev){ var a=Lst[t]||[]; for(var i=0;i<a.length;i++){ try{a[i].call(scope,ev);}catch(e){g.console.log('[sw-err]',e);} } return ev; } };\n"
"    scope.self=scope; return scope; }\n"
"  function __extEv(){ var p=[]; return { waitUntil:function(pr){ p.push(Promise.resolve(pr)); }, __p:p }; }\n"
"  if(!g.navigator) g.navigator={};\n"
"  g.navigator.serviceWorker = {\n"
"    controller: null,\n"
"    register: function(url, opts){\n"
"      return __realFetch(url).then(function(r){ return r.text(); }).then(function(src){\n"
"        var scope=__swNew();\n"
"        scope.registration={ scope:(opts&&opts.scope)||'/', active:{}, installing:null, waiting:null,\n"
"          addEventListener:function(){}, update:function(){return Promise.resolve();},\n"
"          unregister:function(){ __swScope=null; __swReg=null; return Promise.resolve(true); } };\n"
"        var swFetch=function(u,o){ return __realFetch(u,o); };\n"
"        (new Function('self','addEventListener','caches','fetch','location','clients','skipWaiting','registration', src))\n"
"          (scope, scope.addEventListener.bind(scope), g.caches, swFetch, g.location, scope.clients,\n"
"           scope.skipWaiting, scope.registration);\n"
"        var iev=__extEv(); scope.__fire('install', iev);\n"
"        return Promise.all(iev.__p).then(function(){ var aev=__extEv(); scope.__fire('activate', aev);\n"
"          return Promise.all(aev.__p); }).then(function(){\n"
"          __swScope=scope; __swReg=scope.registration;\n"
"          g.navigator.serviceWorker.controller={ postMessage:function(){}, scriptURL:url, state:'activated' };\n"
"          if(__readyRes){ __readyRes(__swReg); __readyRes=null; }\n"
"          return __swReg; }); }); },\n"
"    getRegistration: function(){ return Promise.resolve(__swReg); },\n"
"    getRegistrations: function(){ return Promise.resolve(__swReg?[__swReg]:[]); },\n"
"    addEventListener: function(){}, removeEventListener: function(){} };\n"
"  g.navigator.serviceWorker.ready = new Promise(function(res){ if(__swReg) res(__swReg); else __readyRes=res; });\n"
/* route page fetch through the SW's fetch handler (respondWith or fall through) */
"  g.fetch = function(input, opt){\n"
"    if (__swScope && __swScope.__has('fetch')){\n"
"      var req = (input && typeof input==='object' && typeof input.url==='string')\n"
"              ? input : { url:String(input), method:(opt&&opt.method)||'GET' };\n"
"      var answered=null;\n"
"      var ev={ request:req, respondWith:function(pr){ answered=Promise.resolve(pr); }, waitUntil:function(){} };\n"
"      __swScope.__fire('fetch', ev);\n"
"      if (answered) return answered;\n"
"    }\n"
"    return __realFetch(input, opt); };\n"
/* ---- WebAssembly JS API (wasm3 backend) --------------------------- */
"  (function(){\n"
"    function toAB(src){\n"
"      if (src instanceof ArrayBuffer) return src;\n"
"      if (src && src.buffer instanceof ArrayBuffer)\n"
"        return src.buffer.slice(src.byteOffset||0, (src.byteOffset||0)+(src.byteLength||0));\n"
"      var u = new Uint8Array((src && src.length)||0);\n"
"      for (var i=0;i<u.length;i++) u[i]=src[i]&255;\n"
"      return u.buffer;\n"
"    }\n"
"    function mkResolver(imp){\n"
"      return function(mod, field){\n"
"        var m = imp && imp[mod];\n"
"        if (m && typeof m[field] === 'function') return m[field];\n"
"        return undefined;\n"
"      };\n"
"    }\n"
/* A WebAssembly.Memory bound to instance id: buffer aliases wasm linear
 * memory (getter, so a grow's new base is picked up on re-read); grow(n)
 * resizes and returns the previous size in pages. */
"    function mkMemory(id){\n"
"      var m = {\n"
"        grow: function(delta){\n"
"          var old = D.wasmMemGrow(id, delta|0);\n"
"          if (old < 0) throw new RangeError('wasm memory grow failed');\n"
"          return old;\n"
"        }\n"
"      };\n"
"      Object.defineProperty(m, 'buffer',\n"
"        { get: function(){ return D.wasmMemBuffer(id) || new ArrayBuffer(0); } });\n"
"      return m;\n"
"    }\n"
"    function buildInstance(id){\n"
"      var ex = {};\n"
"      var names = D.wasmExportNames(id);\n"
"      for (var i=0;i<names.length;i++){\n"
"        (function(nm){ ex[nm] = function(){\n"
"          return D.wasmCall(id, nm, Array.prototype.slice.call(arguments)); }; })(names[i]);\n"
"      }\n"
"      var mn = D.wasmMemName(id);\n"
"      if (mn) ex[mn] = mkMemory(id);\n"
"      var gns = D.wasmGlobalNames(id);\n"
"      for (var gi=0;gi<gns.length;gi++) ex[gns[gi]] = mkGlobal(id, gns[gi]);\n"
"      var tn = D.wasmTableName(id);\n"
"      if (tn) ex[tn] = mkTable(id);\n"
"      return { exports: ex, __id: id };\n"
"    }\n"
/* WebAssembly.Global bound to an exported global: .value reads/writes the
 * live global (throws on an immutable set); valueOf for coercion. */
"    function mkGlobal(id, name){\n"
"      var o = { valueOf: function(){ return D.wasmGlobalGet(id, name); } };\n"
"      Object.defineProperty(o, 'value', {\n"
"        get: function(){ return D.wasmGlobalGet(id, name); },\n"
"        set: function(v){ D.wasmGlobalSet(id, name, v); } });\n"
"      return o;\n"
"    }\n"
/* WebAssembly.Table bound to an exported table: length + get(i) returns a
 * callable that does a JS-driven call_indirect through slot i. */
"    function mkTable(id){\n"
"      var o = { get: function(i){\n"
"        return function(){ return D.wasmTableCall(id, i|0, Array.prototype.slice.call(arguments)); }; } };\n"
"      Object.defineProperty(o, 'length', { get: function(){ return D.wasmTableLen(id); } });\n"
"      return o;\n"
"    }\n"
"    function Module(bytes){ this.__bytes = toAB(bytes); }\n"
"    function Instance(mod, imp){\n"
"      var bi = instantiateBytes(mod.__bytes, imp);\n"
"      this.exports = bi.exports; this.__id = bi.__id;\n"
"    }\n"
/* Standalone WebAssembly.Memory (not yet bound to an instance; imported
 * memory linking is a follow-up). Backed by a plain ArrayBuffer that grow
 * rebuilds + copies, so the JS-facing API works even unlinked. */
"    function Memory(desc){\n"
"      this._pages = (desc && desc.initial) ? (desc.initial|0) : 0;\n"
"      this._max = (desc && desc.maximum) ? (desc.maximum|0) : 65536;\n"
"      this.buffer = new ArrayBuffer(this._pages*65536);\n"
"      this.grow = function(delta){\n"
"        var old = this._pages;\n"
"        if (old + (delta|0) > this._max) throw new RangeError('wasm memory grow failed');\n"
"        this._pages += (delta|0);\n"
"        var nb = new ArrayBuffer(this._pages*65536);\n"
"        new Uint8Array(nb).set(new Uint8Array(this.buffer));\n"
"        this.buffer = nb;\n"
"        return old;\n"
"      };\n"
"    }\n"
"    function Table(desc){ this.length = (desc && desc.initial)||0; }\n"
/* Standalone WebAssembly.Global (not linked to an instance; holds a value
 * with a .value getter/setter honoring mutability). */
"    function Global(desc, init){\n"
"      this._val = (init !== undefined) ? init : 0;\n"
"      this._mutable = !!(desc && desc.mutable);\n"
"    }\n"
"    Object.defineProperty(Global.prototype, 'value', {\n"
"      get: function(){ return this._val; },\n"
"      set: function(v){ if(!this._mutable) throw new TypeError('immutable global'); this._val = v; } });\n"
"    Global.prototype.valueOf = function(){ return this._val; };\n"
/* find the WebAssembly.Memory an importObject supplies (any namespace) */
"    function findImportMem(imp){\n"
"      if (!imp) return null;\n"
"      for (var k in imp){ var ns = imp[k];\n"
"        if (ns && typeof ns === 'object') for (var f in ns)\n"
"          if (ns[f] instanceof Memory) return ns[f];\n"
"      }\n"
"      return null;\n"
"    }\n"
/* bind a standalone Memory to an instance: copy any pre-instantiate bytes
 * into the instance's linear memory, then re-point .buffer/.grow at it so
 * the same object now aliases wasm memory (shared both ways). */
"    function bindImportedMemory(mem, id){\n"
"      try {\n"
"        var dab = D.wasmMemBuffer(id);\n"
"        if (dab){ var src = new Uint8Array(mem.buffer), dst = new Uint8Array(dab);\n"
"          var n = Math.min(src.length, dst.length); if (n) dst.set(src.subarray(0,n)); }\n"
"      } catch(e){}\n"
"      Object.defineProperty(mem, 'buffer', { configurable:true,\n"
"        get: function(){ return D.wasmMemBuffer(id) || new ArrayBuffer(0); } });\n"
"      mem.grow = function(delta){ var old = D.wasmMemGrow(id, delta|0);\n"
"        if (old < 0) throw new RangeError('wasm memory grow failed'); return old; };\n"
"      mem.__id = id;\n"
"    }\n"
"    function instantiateBytes(ab, imp){\n"
"      var im = findImportMem(imp);\n"
"      var id = D.wasmInstantiate(ab, mkResolver(imp), im ? (im._pages||0) : 0);\n"
"      if (im) bindImportedMemory(im, id);\n"
"      return buildInstance(id);\n"
"    }\n"
"    function instantiate(src, imp){\n"
"      try {\n"
"        if (src instanceof Module)\n"
"          return Promise.resolve(instantiateBytes(src.__bytes, imp));\n"
"        var ab = toAB(src);\n"
"        return Promise.resolve({ module: new Module(ab), instance: instantiateBytes(ab, imp) });\n"
"      } catch(e){ return Promise.reject(e); }\n"
"    }\n"
"    function compile(bytes){ try { return Promise.resolve(new Module(bytes)); }\n"
"      catch(e){ return Promise.reject(e); } }\n"
"    function instantiateStreaming(resp, imp){\n"
"      return Promise.resolve(resp).then(function(r){\n"
"        var src = (r && r.arrayBuffer) ? r.arrayBuffer() : r;\n"
"        return Promise.resolve(src).then(function(ab){ return instantiate(ab, imp); });\n"
"      });\n"
"    }\n"
"    function compileStreaming(resp){\n"
"      return Promise.resolve(resp).then(function(r){\n"
"        var src = (r && r.arrayBuffer) ? r.arrayBuffer() : r;\n"
"        return Promise.resolve(src).then(function(ab){ return compile(ab); });\n"
"      });\n"
"    }\n"
"    function CompileError(m){ this.message=m; this.name='CompileError'; }\n"
"    function RuntimeError(m){ this.message=m; this.name='RuntimeError'; }\n"
"    function LinkError(m){ this.message=m; this.name='LinkError'; }\n"
"    g.WebAssembly = { Module: Module, Instance: Instance, Memory: Memory,\n"
"      Table: Table, Global: Global, instantiate: instantiate, compile: compile,\n"
"      instantiateStreaming: instantiateStreaming, compileStreaming: compileStreaming,\n"
"      validate: function(b){ try { new Module(b); return true; } catch(e){ return false; } },\n"
"      CompileError: CompileError, RuntimeError: RuntimeError, LinkError: LinkError };\n"
"  })();\n"
"})(globalThis);\n";

/* Worker global scope (runs in the worker's own context). `self`/
 * `globalThis` are the worker global; postMessage/onmessage cross to
 * the main thread; timers + importScripts + a Promise microtask hook
 * mirror the page environment (minus DOM). __deliver(json) is invoked
 * by the C pump to deliver a message. */
static const char WORKER_PRELUDE[] =
"(function(g){\n"
"  var W = g.__w;\n"
"  g.self = g; g.globalThis = g;\n"
"  g.onmessage = null; g.onerror = null;\n"
"  g.postMessage = function(m){ W.postMain(JSON.stringify(m === undefined ? null : m)); };\n"
"  g.__deliver = function(json){\n"
"    if (g.onmessage){\n"
"      try { g.onmessage({ data: JSON.parse(json) }); }\n"
"      catch(e){ g.console.log('[worker-onmsg-err]', e); }\n"
"    }\n"
"  };\n"
"  g.addEventListener = function(t, f){ if (String(t) === 'message') g.onmessage = f; };\n"
"  g.removeEventListener = function(){};\n"
"  g.close = function(){};\n"     /* pump reaps on terminate() from main */
"  g.importScripts = function(){ W['import'].apply(null, arguments); };\n"
"  g.setTimeout = function(f, ms){ return (typeof f==='function') ? W.timer(f, ms|0, 0) : 0; };\n"
"  g.setInterval = function(f, ms){ return (typeof f==='function') ? W.timer(f, (ms|0)||10, 1) : 0; };\n"
"  g.clearTimeout = g.clearInterval = function(id){ W.untimer(id|0); };\n"
"  g.queueMicrotask = function(f){ Promise.resolve().then(f); };\n"
"})(globalThis);\n";

/* ---- run every <script> at load -------------------------------------- */

#define JS_SRC_CAP (4 * 1024 * 1024)  /* external framework bundles (YT base.js ~2-3 MiB) */

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
static char *js_module_normalize(JSContext *cx, const char *base,
                                 const char *name, void *opaque);
static JSModuleDef *js_module_loader(JSContext *cx, const char *name,
                                     void *opaque);

/* ---- JS watchdog ---------------------------------------------------- *
 * Page scripts run synchronously on the UI thread; a heavy framework
 * bundle (bbc's React boot grinds for minutes on the interpreter) must
 * not hold the tab hostage. QuickJS polls the interrupt handler from
 * the interpreter loop; past the deadline the current eval aborts with
 * an exception and the page degrades to its server-rendered content --
 * like browsing with JS off, not a hung "Loading...". 0 = unlimited.
 * Entry points save/restore the deadline, so nesting is safe. */
static long g_js_deadline;
static int  g_js_interrupted;
#define JS_LOAD_BUDGET_MS 20000        /* all page-load scripts, total */
#define JS_TICK_BUDGET_MS 3000         /* one timer/job/event entry */

static int js_interrupt_cb(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    if (g_js_deadline && js_now_ms() > g_js_deadline) {
        g_js_interrupted = 1;
        return 1;
    }
    return 0;
}

static int js_ensure(void) {
    if (cur->js_cx) return 1;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return 0;
    JS_SetMemoryLimit(rt, 32u * 1024 * 1024);
    JS_SetMaxStackSize(rt, 192 * 1024);
    JS_SetInterruptHandler(rt, js_interrupt_cb, NULL);
    JSContext *cx = JS_NewContext(rt);
    if (!cx) { JS_FreeRuntime(rt); return 0; }
    cur->js_rt = rt;
    cur->js_cx = cx;
    cur->js_timer_seq = 1;
    JS_SetModuleLoaderFunc(rt, js_module_normalize, js_module_loader, NULL);

    {
        JSValue g = JS_GetGlobalObject(cx);
        JSValue dom = JS_NewObject(cx);
        static const struct { const char *n; JSCFunction *f; int na; } B[] = {
            {"byId", js_dom_byid, 1},     {"query", js_dom_query, 1},
            {"create", js_dom_create, 1}, {"text", js_dom_text, 1},
            {"clone", js_dom_clone, 1},   {"byTagFrom", js_dom_bytagfrom, 2},
            {"attachShadow", js_dom_attachshadow, 1},
            {"append", js_dom_append, 2}, {"remove", js_dom_remove, 1},
            {"setText", js_dom_settext, 2},{"getText", js_dom_gettext, 1},
            {"setHTML", js_dom_sethtml, 2},{"getAttr", js_dom_getattr, 2},
            {"setAttr", js_dom_setattr, 3},{"tag", js_dom_tag, 1},
            {"parent", js_dom_parent, 1}, {"children", js_dom_children, 1},
            {"body", js_dom_body, 0},     {"title", js_dom_title, 1},
            {"timer", js_dom_timer, 3},   {"untimer", js_dom_untimer, 1},
            {"fetchSync", js_dom_fetchsync, 1},
            {"fetchStart", js_dom_fetchstart, 2},
            {"fetchStart2", js_dom_fetchstart2, 6},
            {"workerNew", js_worker_new, 2},
            {"workerPost", js_worker_post, 2},
            {"workerTerminate", js_worker_terminate, 1},
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
            {"wasmInstantiate", js_wasm_instantiate, 3},
            {"wasmExportNames", js_wasm_exports, 1},
            {"wasmMemName", js_wasm_memname, 1},
            {"wasmCall", js_wasm_call, 3},
            {"wasmMemBuffer", js_wasm_membuffer, 1},
            {"wasmMemPages", js_wasm_mempages, 1},
            {"wasmMemGrow", js_wasm_memgrow, 2},
            {"wasmGlobalNames", js_wasm_globalnames, 1},
            {"wasmGlobalGet", js_wasm_globalget, 2},
            {"wasmGlobalSet", js_wasm_globalset, 3},
            {"wasmTableLen", js_wasm_tablelen, 1},
            {"wasmTableName", js_wasm_tablename, 1},
            {"wasmTableCall", js_wasm_tablecall, 3},
            {"wasmFree", js_wasm_free, 1},
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

/* ---- ES modules ----------------------------------------------------
 * <script type="module">, static import/export and dynamic import().
 * QuickJS walks a module graph through two hooks: `normalize` turns a
 * specifier plus the importer's name into an absolute name, and `load`
 * turns that name into a compiled module. We name modules by absolute
 * URL and fetch them synchronously (mirroring the classic external-
 * script path); QuickJS caches by name, so each URL loads at most once
 * and import cycles terminate. ------------------------------------- */

/* Report an error value that never became a live exception (a rejected
 * module promise), through the same channel as js_dump_error. */
static void js_report_val(JSContext *cx, JSValueConst v) {
    const char *s = JS_ToCString(cx, v);
    if (!s) return;
    char m[120];
    int p = msg_append(m, 0, sizeof(m), "JS error: ");
    p = msg_append(m, p, sizeof(m), s);
    set_status(m);
    sys_write(1, "[js] ERROR: ", 12);
    sys_write(1, s, str_len(s));
    sys_write(1, "\n", 1);
    JS_FreeCString(cx, s);
}

/* import.meta.url: modules resolve their own asset/dynamic-import URLs
 * from it, so it must be the module's absolute URL. */
static void js_module_set_meta(JSContext *cx, JSValueConst fv) {
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(fv);
    JSAtom na = JS_GetModuleName(cx, m);
    const char *nm = JS_AtomToCString(cx, na);
    JS_FreeAtom(cx, na);
    JSValue meta = JS_GetImportMeta(cx, m);
    if (!JS_IsException(meta)) {
        JS_DefinePropertyValueStr(cx, meta, "url",
                                  JS_NewString(cx, nm ? nm : ""),
                                  JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(cx, meta, "main", JS_FALSE, JS_PROP_C_W_E);
        JS_FreeValue(cx, meta);
    }
    if (nm) JS_FreeCString(cx, nm);
}

static char *js_module_normalize(JSContext *cx, const char *base,
                                 const char *name, void *opaque) {
    (void)opaque;
    char abs[URL_MAX + 1];
    /* Bare specifiers ("react") are meaningless without an import map;
     * resolving them as a relative path yields an honest module-load
     * error rather than a silent hang. */
    resolve_relative_url(base, name, abs, URL_MAX);
    int n = (int)str_len(abs);
    char *out = (char *)js_malloc(cx, (size_t)n + 1);
    if (!out) return NULL;
    for (int i = 0; i <= n; i++) out[i] = abs[i];
    return out;
}

static JSModuleDef *js_module_loader(JSContext *cx, const char *name,
                                     void *opaque) {
    (void)opaque;
    if (!has_scheme(name)) {
        JS_ThrowReferenceError(cx, "module '%s' is not a resolvable URL", name);
        return NULL;
    }
    char *buf = (char *)malloc(JS_SRC_CAP + 1);
    if (!buf) { JS_ThrowOutOfMemory(cx); return NULL; }
    struct http_fetch req;
    mem_zero(&req, sizeof(req));
    req.url = (unsigned long)name;
    req.buf = (unsigned long)buf;
    req.buf_sz = JS_SRC_CAP;
    long r = sys_http_fetch(&req);
    if (r <= 0 || req.status <= 0 || req.status >= 400) {
        free(buf);
        JS_ThrowReferenceError(cx, "failed to load module '%s'", name);
        return NULL;
    }
    buf[r] = '\0';
    JSValue fv = JS_Eval(cx, buf, (size_t)r, name,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(buf);
    if (JS_IsException(fv)) return NULL;
    js_module_set_meta(cx, fv);
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(fv);
    JS_FreeValue(cx, fv);
    return m;
}

/* Compile+evaluate one module script. Module bodies may top-level-await,
 * so evaluation yields a promise: it can still be pending here and reject
 * later, which is why run_scripts re-checks after draining jobs. Returns
 * that promise (JS_UNDEFINED if it never got that far). */
static JSValue js_eval_module(JSContext *cx, const char *src, long len,
                              const char *name) {
    JSValue fv = JS_Eval(cx, src, (size_t)len, name,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fv)) { js_dump_error(cx); return JS_UNDEFINED; }
    js_module_set_meta(cx, fv);
    JSValue r = JS_EvalFunction(cx, fv);        /* consumes fv */
    if (JS_IsException(r)) {
        js_dump_error(cx);
        JS_FreeValue(cx, r);
        return JS_UNDEFINED;
    }
    return r;
}

static void run_scripts(void) {
    int scripts[64];
    int ns = 0;
    for (int i = 1; i < E->nnodes && ns < 64; i++)
        if (E->nodes[i].tag == T_SCRIPT) scripts[ns++] = i;
    if (!ns) return;
    if (!js_ensure()) return;
    JSContext *cx = cur->js_cx;
    JSValue modp[64];          /* module eval promises (top-level await) */
    int nmodp = 0;
    long save_dl = g_js_deadline;
    g_js_interrupted = 0;
    g_js_deadline = js_now_ms() + JS_LOAD_BUDGET_MS;

    for (int k = 0; k < ns; k++) {
        struct dnode *nd = &E->nodes[scripts[k]];
        char ty[40];
        int is_mod = 0;
        if (node_attr_str(nd, "type", ty, sizeof(ty)) && ty[0]) {
            is_mod = str_contains(ty, (int)str_len(ty), "module", 6) >= 0;
            if (!is_mod &&
                str_contains(ty, (int)str_len(ty), "javascript", 10) < 0)
                continue;                 /* JSON/templates/etc. */
        }
        /* JS_Eval requires a NUL-terminated buffer (the lexer relies on
         * the sentinel), so scripts always run from an owned copy. */
        char *fbuf = NULL;
        long slen = 0;
        char surl[URL_MAX + 1];
        /* A module's name is its own absolute URL, so its relative
         * imports resolve against it; an inline module resolves against
         * the document (its src-less name is the page URL). */
        char mname[URL_MAX + 1];
        str_copy(mname, g_url, URL_MAX);
        if (node_attr_str(nd, "src", surl, sizeof(surl)) && surl[0]) {
            char url[URL_MAX + 1];
            resolve_relative_url(g_url, surl, url, URL_MAX);
            str_copy(mname, url, URL_MAX);
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
            if (is_mod) {
                JSValue p = js_eval_module(cx, fbuf, slen, mname);
                if (JS_IsUndefined(p) || nmodp >= 64) JS_FreeValue(cx, p);
                else modp[nmodp++] = p;
            } else {
                JSValue r = JS_Eval(cx, fbuf, (size_t)slen, "script",
                                    JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(r)) js_dump_error(cx);
                JS_FreeValue(cx, r);
            }
        }
        if (fbuf) free(fbuf);
    }

    /* microtasks queued during load, then the document lifecycle
     * events (the runtime stays alive: phase 10) */
    js_drain_jobs(cur);

    /* A module body that top-level-awaited settles only once the jobs
     * above run; surface a rejection instead of losing it. */
    for (int k = 0; k < nmodp; k++) {
        if (JS_PromiseState(cx, modp[k]) == JS_PROMISE_REJECTED) {
            JSValue e = JS_PromiseResult(cx, modp[k]);
            js_report_val(cx, e);
            JS_FreeValue(cx, e);
        }
        JS_FreeValue(cx, modp[k]);
    }
    if (g_js_interrupted) {
        char m[96]; int p = 0;
        p = msg_append(m, p, sizeof(m),
                       "[js] page-script budget exhausted after ");
        p = msg_append_int(m, p, sizeof(m), JS_LOAD_BUDGET_MS);
        p = msg_append(m, p, sizeof(m), " ms; page continues without JS\n");
        sys_write(1, m, p);
    }
    g_js_deadline = save_dl;
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
static int css_lerp_i(int a, int b, int num, int den) {
    return a + (b - a) * num / den;
}
static uint32_t css_col_lerp(uint32_t a, uint32_t b, int num, int den) {
    return ((uint32_t)css_lerp_i((a >> 24) & 255, (b >> 24) & 255, num, den) << 24) |
           ((uint32_t)css_lerp_i((a >> 16) & 255, (b >> 16) & 255, num, den) << 16) |
           ((uint32_t)css_lerp_i((a >> 8) & 255, (b >> 8) & 255, num, den) << 8) |
           (uint32_t)css_lerp_i(a & 255, b & 255, num, den);
}

/* CSS animation clock: interpolate @keyframes overrides onto computed
 * styles for the active engine's nodes, just before layout. Sets
 * g_anim_active so the main loop keeps ticking while anything runs. */
static void anim_apply(void) {
    long now = js_now_ms();
    int active = 0;
    for (int ni = 0; ni < E->nnodes; ni++) {
        struct cstyle *st = &E->nodes[ni].st;
        if (st->anim_kf < 0 || st->anim_kf >= g_nkf || st->anim_dur == 0) continue;
        struct kf_anim *kf = &g_kf[st->anim_kf];
        if (kf->nstops < 1) continue;
        if (g_anim_t0[ni] < 0) g_anim_t0[ni] = (int32_t)now;
        long el = now - g_anim_t0[ni] - (long)st->anim_delay;
        if (el < 0) el = 0;
        long dur = st->anim_dur;
        long cyc = el / dur;
        int done = (st->anim_iter && cyc >= st->anim_iter);
        long inms = done ? dur : (el % dur);
        int fr = (int)(inms * 1000 / dur);                 /* 0..1000 (t*1000) */
        if ((st->anim_flags & 1) && (cyc & 1)) fr = 1000 - fr;   /* alternate */
        if (st->anim_flags & 2) {                            /* ease-in-out */
            long f = fr;
            fr = (int)(3 * f * f / 1000 - 2 * f * f * f / 1000000);  /* smoothstep */
            if (fr < 0) fr = 0;
            if (fr > 1000) fr = 1000;
        }
        int pct = fr / 10;
        struct kf_stop *lo = &kf->stops[0], *hi = &kf->stops[kf->nstops - 1];
        for (int s = 0; s < kf->nstops; s++)
            if (kf->stops[s].pct <= pct) lo = &kf->stops[s];
        for (int s = kf->nstops - 1; s >= 0; s--)
            if (kf->stops[s].pct >= pct) hi = &kf->stops[s];
        int span = hi->pct - lo->pct;
        int num = span ? (pct - lo->pct) : 0, den = span ? span : 1;
        if (lo->has & 1) {
            st->has_tf = 1; st->txpct = st->typct = 0;
            st->txpx = (hi->has & 1) ? (int16_t)css_lerp_i(lo->txpx, hi->txpx, num, den) : lo->txpx;
            st->typx = (hi->has & 1) ? (int16_t)css_lerp_i(lo->typx, hi->typx, num, den) : lo->typx;
        }
        if (lo->has & 2)
            st->color = (hi->has & 2) ? css_col_lerp(lo->color, hi->color, num, den) : lo->color;
        if (lo->has & 4)
            st->bg = (hi->has & 4) ? css_col_lerp(lo->bg, hi->bg, num, den) : lo->bg;
        if (lo->has & 8) {
            st->scx = (uint16_t)css_lerp_i(lo->scx,
                          (hi->has & 8) ? hi->scx : lo->scx, num, den);
            st->scy = (uint16_t)css_lerp_i(lo->scy,
                          (hi->has & 8) ? hi->scy : lo->scy, num, den);
        }
        if (lo->has & 16)
            st->rot = (int16_t)css_lerp_i(lo->rot,
                          (hi->has & 16) ? hi->rot : lo->rot, num, den);
        if (lo->has & 32)
            st->opa = (uint8_t)css_lerp_i(lo->opa,
                          (hi->has & 32) ? hi->opa : lo->opa, num, den);
        if (!done) active = 1;
    }
    g_anim_active = active;
}

/* Easing (0..1000 -> 0..1000): smoothstep for ease*, linear otherwise. */
static int css_ease(int fr, uint8_t flags) {
    if (flags & 2) {
        long f = fr;
        fr = (int)(3 * f * f / 1000 - 2 * f * f * f / 1000000);
        if (fr < 0) fr = 0;
        if (fr > 1000) fr = 1000;
    }
    return fr;
}

/* CSS transitions: when a transitioned property's freshly-cascaded value
 * differs from its target, ease from the current displayed value to the
 * new one over the duration. Runs after anim_apply (animation wins) and
 * before layout. Overrides st with the interpolated value. */
static void trans_apply(void) {
    long now = js_now_ms();
    for (int ni = 0; ni < E->nnodes; ni++) {
        struct cstyle *st = &E->nodes[ni].st;
        struct trans_state *ts = &g_trans[ni];
        if (st->anim_kf >= 0) { ts->seen = 0; ts->active = 0; continue; }  /* anim wins */
        uint8_t mask = st->trans_mask;
        if (!mask && !ts->seen) continue;
        long dur = st->trans_dur, delay = st->trans_delay;
        if (mask & 1) {                                     /* color */
            uint32_t target = st->color;
            if (!(ts->seen & 1)) { ts->cur_color = ts->to_color = target; ts->seen |= 1; }
            else if (target != ts->to_color) {
                ts->from_color = ts->cur_color; ts->to_color = target;
                ts->t0_color = (int32_t)now; ts->active |= 1;
            }
            if (ts->active & 1) {
                long el = now - ts->t0_color - delay;
                if (dur <= 0 || el >= dur) { ts->cur_color = ts->to_color; ts->active &= ~1; }
                else if (el < 0) ts->cur_color = ts->from_color;
                else ts->cur_color = css_col_lerp(ts->from_color, ts->to_color,
                                                  css_ease((int)(el * 1000 / dur), st->trans_flags), 1000);
            } else ts->cur_color = target;
            st->color = ts->cur_color;
        } else ts->seen &= ~1;
        if (mask & 2) {                                     /* background-color */
            uint32_t target = st->bg;
            if (!(ts->seen & 2)) { ts->cur_bg = ts->to_bg = target; ts->seen |= 2; }
            else if (target != ts->to_bg) {
                ts->from_bg = ts->cur_bg; ts->to_bg = target;
                ts->t0_bg = (int32_t)now; ts->active |= 2;
            }
            if (ts->active & 2) {
                long el = now - ts->t0_bg - delay;
                if (dur <= 0 || el >= dur) { ts->cur_bg = ts->to_bg; ts->active &= ~2; }
                else if (el < 0) ts->cur_bg = ts->from_bg;
                else ts->cur_bg = css_col_lerp(ts->from_bg, ts->to_bg,
                                               css_ease((int)(el * 1000 / dur), st->trans_flags), 1000);
            } else ts->cur_bg = target;
            st->bg = ts->cur_bg;
        } else ts->seen &= ~2;
        if (mask & 4) {              /* transform: translate + scale + rotate */
            int16_t  tx = st->has_tf ? st->txpx : 0, ty = st->has_tf ? st->typx : 0;
            uint16_t sx = st->scx, sy = st->scy;
            int16_t  ro = st->rot;
            if (!(ts->seen & 4)) {
                ts->cur_tx = ts->to_tx = tx; ts->cur_ty = ts->to_ty = ty;
                ts->cur_sx = ts->to_sx = sx; ts->cur_sy = ts->to_sy = sy;
                ts->cur_rot = ts->to_rot = ro;
                ts->seen |= 4;
            } else if (tx != ts->to_tx || ty != ts->to_ty ||
                       sx != ts->to_sx || sy != ts->to_sy || ro != ts->to_rot) {
                ts->from_tx = ts->cur_tx;  ts->from_ty = ts->cur_ty;
                ts->from_sx = ts->cur_sx;  ts->from_sy = ts->cur_sy;
                ts->from_rot = ts->cur_rot;
                ts->to_tx = tx; ts->to_ty = ty;
                ts->to_sx = sx; ts->to_sy = sy; ts->to_rot = ro;
                ts->t0_tf = (int32_t)now; ts->active |= 4;
            }
            if (ts->active & 4) {
                long el = now - ts->t0_tf - delay;
                if (dur <= 0 || el >= dur) {
                    ts->cur_tx = ts->to_tx; ts->cur_ty = ts->to_ty;
                    ts->cur_sx = ts->to_sx; ts->cur_sy = ts->to_sy;
                    ts->cur_rot = ts->to_rot;
                    ts->active &= ~4;
                } else if (el < 0) {
                    ts->cur_tx = ts->from_tx; ts->cur_ty = ts->from_ty;
                    ts->cur_sx = ts->from_sx; ts->cur_sy = ts->from_sy;
                    ts->cur_rot = ts->from_rot;
                } else { int fr = css_ease((int)(el * 1000 / dur), st->trans_flags);
                    ts->cur_tx = (int16_t)css_lerp_i(ts->from_tx, ts->to_tx, fr, 1000);
                    ts->cur_ty = (int16_t)css_lerp_i(ts->from_ty, ts->to_ty, fr, 1000);
                    ts->cur_sx = (uint16_t)css_lerp_i(ts->from_sx, ts->to_sx, fr, 1000);
                    ts->cur_sy = (uint16_t)css_lerp_i(ts->from_sy, ts->to_sy, fr, 1000);
                    ts->cur_rot = (int16_t)css_lerp_i(ts->from_rot, ts->to_rot, fr, 1000); }
            } else {
                ts->cur_tx = tx; ts->cur_ty = ty;
                ts->cur_sx = sx; ts->cur_sy = sy; ts->cur_rot = ro;
            }
            st->has_tf = 1; st->txpct = st->typct = 0;
            st->txpx = ts->cur_tx; st->typx = ts->cur_ty;
            st->scx = ts->cur_sx; st->scy = ts->cur_sy; st->rot = ts->cur_rot;
        } else ts->seen &= ~4;
        if (mask & 8) {                                     /* opacity */
            uint8_t target = st->opa;
            if (!(ts->seen & 8)) { ts->cur_opa = ts->to_opa = target; ts->seen |= 8; }
            else if (target != ts->to_opa) {
                ts->from_opa = ts->cur_opa; ts->to_opa = target;
                ts->t0_opa = (int32_t)now; ts->active |= 8;
            }
            if (ts->active & 8) {
                long el = now - ts->t0_opa - delay;
                if (dur <= 0 || el >= dur) { ts->cur_opa = ts->to_opa; ts->active &= ~8; }
                else if (el < 0) ts->cur_opa = ts->from_opa;
                else ts->cur_opa = (uint8_t)css_lerp_i(ts->from_opa, ts->to_opa,
                                       css_ease((int)(el * 1000 / dur), st->trans_flags), 1000);
            } else ts->cur_opa = target;
            st->opa = ts->cur_opa;
        } else ts->seen &= ~8;
        if (ts->active) g_anim_active = 1;
    }
}

/* -DCSS_PERF: time the style pass. The cascade tests every rule against
 * every node, so this is the number that decides whether a site's
 * stylesheet is merely large or actually unusable. */
#ifdef CSS_PERF
static long g_style_t0;
#define STYLE_PERF_BEGIN() (g_style_t0 = js_now_ms())
static void style_perf_end(void) {
    char m[128]; int p = 0;
    p = msg_append(m, p, sizeof(m), "[cssperf] style pass ");
    p = msg_append_int(m, p, sizeof(m), (int)(js_now_ms() - g_style_t0));
    p = msg_append(m, p, sizeof(m), " ms, rules=");
    p = msg_append_int(m, p, sizeof(m), E->nrules);
    p = msg_append(m, p, sizeof(m), " nodes=");
    p = msg_append_int(m, p, sizeof(m), E->nnodes);
    p = msg_append(m, p, sizeof(m), "\n");
    sys_write(1, m, p);
}
#define STYLE_PERF_END() do { style_perf_end(); STYLE_PROF_DUMP(); } while (0)

/* Per-phase render timing: which part of a page load actually costs. */
static long g_phase_t0;
#define PHASE_PERF_BEGIN() (g_phase_t0 = js_now_ms())
static void phase_perf(const char *name) {
    char m[128]; int p = 0;
    p = msg_append(m, p, sizeof(m), "[cssperf] phase ");
    p = msg_append(m, p, sizeof(m), name);
    p = msg_append(m, p, sizeof(m), " ");
    p = msg_append_int(m, p, sizeof(m), (int)(js_now_ms() - g_phase_t0));
    p = msg_append(m, p, sizeof(m), " ms\n");
    sys_write(1, m, p);
    g_phase_t0 = js_now_ms();
}
#define PHASE_PERF(n) phase_perf(n)

#ifdef STYLE_PROF
/* Sub-phase breakdown of the style pass, in millions of TSC cycles. */
static void style_prof_dump(void) {
    char m[224]; int p = 0;
    p = msg_append(m, p, sizeof(m), "[styleprof] Mtsc pres=");
    p = msg_append_int(m, p, sizeof(m), (int)(g_sp_pres / 1000000ull));
    p = msg_append(m, p, sizeof(m), " match=");
    p = msg_append_int(m, p, sizeof(m), (int)(g_sp_match / 1000000ull));
    p = msg_append(m, p, sizeof(m), " inline=");
    p = msg_append_int(m, p, sizeof(m), (int)(g_sp_inline / 1000000ull));
    p = msg_append(m, p, sizeof(m), " var=");
    p = msg_append_int(m, p, sizeof(m), (int)(g_sp_var / 1000000ull));
    p = msg_append(m, p, sizeof(m), " apply=");
    p = msg_append_int(m, p, sizeof(m), (int)(g_sp_apply / 1000000ull));
    p = msg_append(m, p, sizeof(m), " | nodes=");
    p = msg_append_int(m, p, sizeof(m), (int)g_sp_nodes);
    p = msg_append(m, p, sizeof(m), " matches=");
    p = msg_append_int(m, p, sizeof(m), (int)g_sp_matches);
    p = msg_append(m, p, sizeof(m), " decls=");
    p = msg_append_int(m, p, sizeof(m), (int)g_sp_decls);
    p = msg_append(m, p, sizeof(m), " cand=");
    p = msg_append_int(m, p, sizeof(m), (int)g_sp_cand);
    p = msg_append(m, p, sizeof(m), " partm=");
    p = msg_append_int(m, p, sizeof(m), (int)g_sp_partm);
    p = msg_append(m, p, sizeof(m), " clook=");
    p = msg_append_int(m, p, sizeof(m), (int)g_sp_clook);
    p = msg_append(m, p, sizeof(m), " uni_bucket=");
    p = msg_append_int(m, p, sizeof(m), (int)g_sp_uni);
    p = msg_append(m, p, sizeof(m), "\n");
    sys_write(1, m, p);
    g_sp_pres = g_sp_match = g_sp_inline = g_sp_var = g_sp_apply = 0;
    g_sp_nodes = g_sp_matches = g_sp_decls = g_sp_cand = 0;
    g_sp_partm = g_sp_clook = 0;
}
#define STYLE_PROF_DUMP() style_prof_dump()
#else
#define STYLE_PROF_DUMP() ((void)0)
#endif

#else
#define STYLE_PERF_BEGIN() ((void)0)
#define STYLE_PERF_END()   ((void)0)
#define PHASE_PERF_BEGIN() ((void)0)
#define PHASE_PERF(n)      ((void)0)
#define STYLE_PROF_DUMP()  ((void)0)
#endif

#ifdef VAR_PROF
/* Custom-property accounting for the style pass that just finished. */
static void var_prof_dump(void) {
    char m[512]; int p = 0;
    p = msg_append(m, p, sizeof(m), "[varprof] hwm_scope=");
    p = msg_append_int(m, p, sizeof(m), g_vp_hwm_scope);
    p = msg_append(m, p, sizeof(m), "/");
    p = msg_append_int(m, p, sizeof(m), VARSCOPE_MAX);
    p = msg_append(m, p, sizeof(m), " hwm_pool=");
    p = msg_append_int(m, p, sizeof(m), g_vp_hwm_pool);
    p = msg_append(m, p, sizeof(m), "/");
    p = msg_append_int(m, p, sizeof(m), VARPOOL_CAP);
    p = msg_append(m, p, sizeof(m), " drop_scope=");
    p = msg_append_int(m, p, sizeof(m), g_vp_drop_scope);
    p = msg_append(m, p, sizeof(m), " drop_pool=");
    p = msg_append_int(m, p, sizeof(m), g_vp_drop_pool);
    p = msg_append(m, p, sizeof(m), " hit=");
    p = msg_append_int(m, p, sizeof(m), g_vp_hit);
    p = msg_append(m, p, sizeof(m), " miss=");
    p = msg_append_int(m, p, sizeof(m), g_vp_miss);
    p = msg_append(m, p, sizeof(m), "\n");
    sys_write(1, m, p);
    for (int i = 0; i < g_vp_nmissname; i++) {
        p = 0;
        p = msg_append(m, p, sizeof(m), "[varprof] missed: ");
        p = msg_append(m, p, sizeof(m), g_vp_missname[i]);
        p = msg_append(m, p, sizeof(m), "\n");
        sys_write(1, m, p);
    }
    g_vp_drop_scope = g_vp_drop_pool = g_vp_hit = g_vp_miss = 0;
    g_vp_hwm_scope = g_vp_hwm_pool = 0;
    g_vp_nmissname = 0;
}
#define VAR_PROF_DUMP() var_prof_dump()
#else
#define VAR_PROF_DUMP() ((void)0)
#endif

static void js_rerender(void) {
    g_collect_light = 1;
    g_form_open = -1;
    collect_node(0);
    g_collect_light = 0;
    distribute_slots();          /* project light DOM into shadow slots */
    struct cstyle base;
    st_init(&base, NULL);
    base.disp = D_BLOCK;
    var_scope_reset();
    g_ndeco = 0;                 /* decoration pool: per style pass */
    rule_index_ensure();
    STYLE_PERF_BEGIN();
    style_node(0, &base);
    STYLE_PERF_END();
    VAR_PROF_DUMP();
    anim_apply();                /* overlay CSS animation keyframes */
    trans_apply();               /* overlay CSS transitions */
    layout(g_win_w);
    clamp_scroll();
}

/* Main-loop tick: advance CSS @keyframes animations on the active tab,
 * throttled to ~30 fps. A full restyle+relayout+repaint per frame (the
 * same reflow path JS uses), since transforms/colors bake at layout. */
static int anim_pump(void) {
    if (!g_anim_active) return 0;
    static long last_ms = 0;
    long now = js_now_ms();
    if (now - last_ms < 32) return 0;      /* throttle; loop idles briefly */
    last_ms = now;
    js_rerender();
    tk_redraw(&win);
    return 1;
}

/* Call the prelude dispatcher: bubble `type` from `node` (or document
 * level when node < 0), with a key payload for keyboard events.
 * Returns 1 when preventDefault() was called. */
static int js_dispatch_key(int node, const char *type, const char *key) {
    if (!cur->js_cx || !cur->js_has_dispatch) return 0;
    JSContext *cx = cur->js_cx;
    long save_dl = g_js_deadline;
    g_js_deadline = js_now_ms() + JS_TICK_BUDGET_MS;
    JSValue args[3] = { JS_NewInt32(cx, node), JS_NewString(cx, type),
                        JS_NewString(cx, key ? key : "") };
    JSValue r = JS_Call(cx, cur->js_dispatch, JS_UNDEFINED, 3, args);
    int prevented = 0;
    if (JS_IsException(r)) js_dump_error(cx);
    else prevented = JS_ToBool(cx, r);
    JS_FreeValue(cx, args[0]);
    JS_FreeValue(cx, args[1]);
    JS_FreeValue(cx, args[2]);
    js_drain_jobs(cur);
    g_js_deadline = save_dl;
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
    long save_dl = g_js_deadline;
    g_js_deadline = js_now_ms() + JS_TICK_BUDGET_MS;
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
                    /* binary-safe view of the same bytes: text/json read
                     * `body` (UTF-8), arrayBuffer()/streams read this. */
                    JS_SetPropertyStr(t->js_cx, obj, "bodyBytes",
                        JS_NewArrayBufferCopy(t->js_cx,
                            (const uint8_t *)(buf ? buf : ""), (size_t)n2));
                    /* raw response headers (fetch()/XHR read them; the
                     * prelude parses them + enforces CORS). Empty on the
                     * plain-GET path (no capture). */
                    char hb[4096];
                    long hn = sys_http_hdrs(jf->handle, hb, 0, sizeof(hb) - 1);
                    JS_SetPropertyStr(t->js_cx, obj, "headers",
                        JS_NewStringLen(t->js_cx, hb, hn > 0 ? (size_t)hn : 0));
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
    g_js_deadline = save_dl;
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
    masks_free();
    media_free();
    page_reset();

    PHASE_PERF_BEGIN();
    dom_build();
    PHASE_PERF("dom_build");
    css_parse_sheet(UA_SHEET, (long)sizeof(UA_SHEET) - 1, 0);
    PHASE_PERF("ua_sheet");
    g_form_open = -1;
    collect_node(0);                      /* also fetches <link> sheets */
    PHASE_PERF("collect+sheets");
    load_webfonts();                      /* @font-face -> download + register */
    PHASE_PERF("webfonts");

    distribute_slots();                   /* project light DOM into slots */
    struct cstyle base;
    st_init(&base, NULL);
    base.disp = D_BLOCK;
    var_scope_reset();
    g_ndeco = 0;                 /* decoration pool: per style pass */
    rule_index_ensure();
    STYLE_PERF_BEGIN();
    style_node(0, &base);
    STYLE_PERF_END();
    VAR_PROF_DUMP();
    anim_apply();                         /* start CSS animations at t0 */
    trans_apply();                        /* seed transition current values */

    g_view_mode = VIEW_HTML;
    PHASE_PERF_BEGIN();
    layout(g_win_w);
#ifdef DL_TRACE
    /* Dump header-region display items: what IS each painted box? */
    for (int di = 0; di < g_nitems; di++) {
        struct ditem *it = &g_items[di];
        if (it->y > 200 || it->w < 8 || it->h < 8) continue;
        if (it->kind != DI_RECT && it->kind != DI_IMG) continue;
        char m[400]; int p = 0;
        p = msg_append(m, p, sizeof(m), "[dl] ");
        p = msg_append(m, p, sizeof(m), it->kind == DI_RECT ? "RECT" : "IMG");
        p = msg_append(m, p, sizeof(m), " x=");
        p = msg_append_int(m, p, sizeof(m), it->x);
        p = msg_append(m, p, sizeof(m), " y=");
        p = msg_append_int(m, p, sizeof(m), it->y);
        p = msg_append(m, p, sizeof(m), " w=");
        p = msg_append_int(m, p, sizeof(m), it->w);
        p = msg_append(m, p, sizeof(m), " h=");
        p = msg_append_int(m, p, sizeof(m), it->h);
        p = msg_append(m, p, sizeof(m), " fg=");
        p = msg_append_int(m, p, sizeof(m), (int)(it->fg & 0xFFFFFF));
        p = msg_append(m, p, sizeof(m), " img=");
        p = msg_append_int(m, p, sizeof(m), it->img);
        if (it->node >= 0 && it->node < E->nnodes) {
            int cl; const char *cv = node_attr(&E->nodes[it->node], "class", &cl);
            if (cv) {
                p = msg_append(m, p, sizeof(m), " class=\"");
                for (int q = 0; q < cl && q < 60 && p < (int)sizeof(m) - 2; q++)
                    m[p++] = cv[q];
                p = msg_append(m, p, sizeof(m), "\"");
            }
        }
        p = msg_append(m, p, sizeof(m), "\n");
        sys_write(1, m, p);
    }
#endif
    PHASE_PERF("layout");                      /* initial style+layout BEFORE
                                             scripts so synchronous
                                             getComputedStyle / rect reads
                                             see real computed values (13F) */

    /* FIRST PAINT: the server-rendered page is fully styled and laid
     * out -- show it before scripts run. A heavy framework bundle can
     * grind for its whole watchdog budget; Edge paints server HTML
     * immediately and so should we. Scripts that mutate the DOM set
     * g_js_dirty -> the re-render below repaints. */
    update_title();
    tk_redraw(&win);

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
    masks_free();
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
    g_ndeco = 0;                 /* decoration pool: per style pass */
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

/* Collapse "." and ".." path segments in place (RFC 3986
 * remove_dot_segments). Relative ES-module specifiers are nearly always
 * "./x.js" or "../y.js", so resolution MUST fold them or every import
 * resolves to a 404 path. Query/fragment ride along untouched. */
static void url_norm_dots(char *url, int out_max) {
    int origin_end = 0, slashes = 0;
    for (int i = 0; url[i]; i++)
        if (url[i] == '/' && ++slashes == 3) { origin_end = i; break; }
    if (!origin_end) return;                  /* no path to normalize */

    int n = (int)str_len(url), plen = n;
    for (int i = origin_end; i < n; i++)
        if (url[i] == '?' || url[i] == '#') { plen = i; break; }
    if (origin_end >= plen) return;

    char out[URL_MAX + 2];
    int w = 0;
    for (int i = 0; i < origin_end; i++) out[w++] = url[i];

    int i = origin_end, trail = 0;
    while (i < plen) {
        if (url[i] == '/') { i++; continue; }
        int seg0 = i;
        while (i < plen && url[i] != '/') i++;
        int slen = i - seg0;
        if (slen == 1 && url[seg0] == '.') {
            trail = 1;                        /* "." -> drop */
        } else if (slen == 2 && url[seg0] == '.' && url[seg0 + 1] == '.') {
            while (w > origin_end && out[w - 1] != '/') w--;   /* pop seg */
            if (w > origin_end) w--;                           /* and its '/' */
            trail = 1;
        } else {
            if (w < URL_MAX) out[w++] = '/';
            for (int q = seg0; q < i && w < URL_MAX; q++) out[w++] = url[q];
            trail = (i < plen);               /* a '/' followed this segment */
        }
    }
    if ((w == origin_end || trail) && w < URL_MAX && out[w - 1] != '/')
        out[w++] = '/';
    for (int q = plen; q < n && w < URL_MAX; q++) out[w++] = url[q];
    out[w] = 0;
    /* out_max is the CALLER's capacity: several callers hand us 512-byte
     * src[] fields, so copying URL_MAX+1 here smashes their buffer. */
    str_copy(url, out, (size_t)out_max);
}

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

    /* protocol-relative "//host/path": inherit the base's scheme. Real
     * sites emit these constantly; treating them as root-relative built
     * a bogus "https://host//other/path". */
    if (rel[0] == '/' && rel[1] == '/') {
        int scheme_end = 0;
        while (base[scheme_end] && base[scheme_end] != ':') scheme_end++;
        int pos = 0;
        for (int i = 0; i < scheme_end && pos < out_max - 1; i++)
            out[pos++] = base[i];
        if (pos < out_max - 1) out[pos++] = ':';
        for (int i = 0; rel[i] && pos < out_max - 1; i++)
            out[pos++] = rel[i];
        out[pos] = 0;
        url_norm_dots(out, out_max);
        return;
    }

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

        /* Pathless base ("https://host", no slash after the origin):
         * last_slash == origin_end == strlen(base), and the inclusive
         * copy below used to embed base's NUL -- every relative URL
         * resolved to the bare origin (hn's news.css fetch returned the
         * HTML page again; logo/links broke the same way on any
         * bare-origin navigation). Copy what exists, then guarantee the
         * root slash. */
        int pos = 0;
        for (int i = 0; i <= last_slash && base[i] && pos < out_max - 1; i++)
            out[pos++] = base[i];
        if (pos > 0 && out[pos - 1] != '/' && pos < out_max - 1)
            out[pos++] = '/';
        for (int i = 0; rel[i] && pos < out_max - 1; i++)
            out[pos++] = rel[i];
        out[pos] = 0;
    }
    url_norm_dots(out, out_max);
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

#ifdef WASM_BROWSER_TEST
    /* On-screen WebAssembly test home page: instantiate an inline wasm
     * module (the /bin/wasmtest module) via the WebAssembly JS API with a
     * JS host import, call every export, and read exported linear memory.
     * console.log lines (marked [wasmjs]) reach the serial log. */
    html =
        "<html><head><title>WASM test</title>"
        "<style>body{background:#fff;color:#202124;font-size:15px}"
        "#big{font-size:30px;color:#1a73e8;margin:12px 0}"
        "#out{font-family:monospace;font-size:15px;color:#111}</style>"
        "</head><body>"
        "<h1 id=big>WASM: running...</h1>"
        "<div id=out></div>"
        "<script>"
        "var B=[0,97,115,109,1,0,0,0,1,13,2,96,2,127,127,1,127,96,2,125,125,1,125,"
        "2,16,1,3,101,110,118,8,104,111,115,116,95,109,117,108,0,0,3,5,4,0,1,0,0,"
        "5,3,1,0,1,7,42,5,3,97,100,100,0,1,4,102,109,117,108,0,2,7,109,101,109,95,"
        "115,117,109,0,3,9,99,97,108,108,95,104,111,115,116,0,4,3,109,101,109,2,0,"
        "10,57,4,7,0,32,0,32,1,106,11,7,0,32,0,32,1,148,11,27,0,65,0,32,0,54,2,0,"
        "65,4,32,1,54,2,0,65,0,40,2,0,65,4,40,2,0,106,11,11,0,32,0,32,1,16,0,65,2,108,11];"
        "function L(s){console.log('[wasmjs] '+s);"
        "var d=document.getElementById('out');if(d)d.innerHTML+=s+'<br>';}"
        "var imp={env:{host_mul:function(a,b){return (a*b)|0;}}};"
        "try{WebAssembly.instantiate(new Uint8Array(B),imp).then(function(res){"
        "var e=res.instance.exports,ok=true;"
        "var a=e.add(7,35);L('add(7,35)='+a);if(a!==42)ok=false;"
        "var m=e.mem_sum(20,22);L('mem_sum(20,22)='+m);if(m!==42)ok=false;"
        "var c=e.call_host(6,7);L('call_host(6,7)='+c);if(c!==84)ok=false;"
        "var f=e.fmul(1.5,2.0);L('fmul(1.5,2.0)='+f);if(Math.abs(f-3.0)>0.01)ok=false;"
        "var pre=e.mem.buffer;L('pre-grow buflen='+pre.byteLength);"
        "if(new Uint8Array(pre)[0]!==20)ok=false;"
        "var old=e.mem.grow(2);L('mem.grow(2) old pages='+old);if(old!==1)ok=false;"
        "L('old buffer detached? byteLength='+pre.byteLength);if(pre.byteLength!==0)ok=false;"
        "var post=e.mem.buffer;L('post-grow buflen='+post.byteLength);if(post.byteLength!==196608)ok=false;"
        "var u=new Uint8Array(post);L('mem[0] survived grow='+u[0]);if(u[0]!==20)ok=false;"
        "u[100000]=77;L('grown region [100000]='+new Uint8Array(e.mem.buffer)[100000]);"
        "if(new Uint8Array(e.mem.buffer)[100000]!==77)ok=false;"
        "function finish(){L(ok?'RESULT: ALL PASS':'RESULT: FAIL');"
        "var h=document.getElementById('big');if(h)h.textContent=ok?'WASM: ALL PASS':'WASM: FAIL';"
        "L('ALL DONE');}"
        /* second module: i64 ops marshalled as BigInt (exact past 2^53) */
        "var B2=[0,97,115,109,1,0,0,0,1,7,1,96,2,126,126,1,126,3,3,2,0,0,7,21,2,"
        "7,105,54,52,95,115,104,108,0,0,7,105,54,52,95,97,100,100,0,1,10,17,2,7,0,"
        "32,0,32,1,134,11,7,0,32,0,32,1,124,11];"
        "WebAssembly.instantiate(new Uint8Array(B2),{}).then(function(r2){"
        "var e2=r2.instance.exports;"
        "var s=e2.i64_shl(1n,62n);L('i64_shl(1,62)='+s+' ('+(typeof s)+')');"
        "if(typeof s!=='bigint'||s!==4611686018427387904n)ok=false;"
        "var ad=e2.i64_add(9007199254740993n,5n);L('i64_add(2^53+1,5)='+ad);"
        "if(ad!==9007199254740998n)ok=false;"
        /* third module: imports env.memory from a JS-created WebAssembly.Memory */
        "var mem=new WebAssembly.Memory({initial:1,maximum:10});"
        "new Uint8Array(mem.buffer)[16]=55;"
        "var B3=[0,97,115,109,1,0,0,0,1,15,3,96,2,127,127,0,96,1,127,1,127,96,0,1,"
        "127,2,16,1,3,101,110,118,6,109,101,109,111,114,121,2,1,1,10,3,4,3,0,1,2,7,"
        "22,3,4,112,111,107,101,0,0,4,112,101,101,107,0,1,4,115,105,122,101,0,2,10,"
        "24,3,9,0,32,0,32,1,54,2,0,11,7,0,32,0,40,2,0,11,4,0,63,0,11];"
        "WebAssembly.instantiate(new Uint8Array(B3),{env:{memory:mem}}).then(function(r3){"
        "var e3=r3.instance.exports;"
        "L('imp pre-init byte='+new Uint8Array(mem.buffer)[16]);if(new Uint8Array(mem.buffer)[16]!==55)ok=false;"
        "e3.poke(8,4242);var u=new Uint8Array(mem.buffer);"
        "var wv=u[8]|(u[9]<<8)|(u[10]<<16)|(u[11]<<24);"
        "L('imp wasm->JS [8]='+wv);if(wv!==4242)ok=false;"
        "u[12]=0x61;u[13]=0x1e;u[14]=0;u[15]=0;"
        "L('imp JS->wasm peek(12)='+e3.peek(12));if(e3.peek(12)!==7777)ok=false;"
        "var op=mem.grow(1);L('imp mem.grow(1) old='+op+' size='+e3.size());if(e3.size()!==2)ok=false;"
        /* fourth module: exported global + table (call_indirect from JS) */
        "var B4=[0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,3,3,2,0,0,4,4,1,112,0,2,6,7,"
        "1,127,1,65,228,0,11,7,21,4,2,102,49,0,0,2,102,50,0,1,1,103,3,0,3,116,98,108,"
        "1,0,9,8,1,0,65,0,11,2,0,1,10,13,2,5,0,65,239,0,11,5,0,65,222,1,11];"
        "WebAssembly.instantiate(new Uint8Array(B4),{}).then(function(r4){"
        "var e4=r4.instance.exports;"
        "L('global g.value='+e4.g.value);if(e4.g.value!==100)ok=false;"
        "e4.g.value=250;L('after set g.value='+e4.g.value);if(e4.g.value!==250)ok=false;"
        "L('table length='+e4.tbl.length);if(e4.tbl.length!==2)ok=false;"
        "var t0=e4.tbl.get(0)(),t1=e4.tbl.get(1)();"
        "L('tbl[0]()='+t0+' tbl[1]()='+t1);if(t0!==111||t1!==222)ok=false;"
        /* fifth module: WASM SIMD (v128) -- splat/add/extract + f32x4.mul */
        "var B5=[0,97,115,109,1,0,0,0,1,17,3,96,2,127,127,1,127,96,0,1,127,96,2,125,"
        "125,1,125,3,4,3,0,1,2,7,21,3,4,105,97,100,100,0,0,3,99,115,116,0,1,4,102,"
        "109,117,108,0,2,10,59,3,16,0,32,0,253,17,32,1,253,17,253,174,1,253,27,0,11,"
        "23,0,253,12,10,0,0,0,20,0,0,0,30,0,0,0,40,0,0,0,253,27,2,11,16,0,32,0,253,"
        "19,32,1,253,19,253,230,1,253,31,0,11];"
        "WebAssembly.instantiate(new Uint8Array(B5),{}).then(function(r5){"
        "var e5=r5.instance.exports;"
        "var sa=e5.iadd(7,35);L('simd i32x4 splat+add+extract='+sa);if(sa!==42)ok=false;"
        "var sc=e5.cst();L('simd v128.const extract[2]='+sc);if(sc!==30)ok=false;"
        "var sf=e5.fmul(1.5,2.0);L('simd f32x4.mul='+sf);if(Math.abs(sf-3.0)>0.01)ok=false;"
        "finish();"
        "},function(er){L('simd inst rejected: '+er);ok=false;finish();});"
        "},function(er){L('gt inst rejected: '+er);ok=false;finish();});"
        "},function(er){L('imp inst rejected: '+er);ok=false;finish();});"
        "},function(er){L('i64 inst rejected: '+er);ok=false;finish();});"
        "},function(err){L('instantiate rejected: '+err);L('ALL DONE');});"
        "}catch(ex){L('threw: '+ex);L('ALL DONE');}"
        "</script>"
        "</body></html>";
#endif
#ifdef MP4AUDIO_TEST
    /* On-screen MP4 audio test: an autoplay <audio> pointing at an AAC
     * M4A served from the host (10.0.2.2:8099). The browser fetches it,
     * demuxes the mp4a track, decodes the AAC AUs (Helix), and opens a
     * kernel audio stream -- watch serial for "[med] mp4/aac ..." lines. */
    html =
        "<html><head><title>MP4 audio</title></head><body>"
        "<h1 style='color:#1a73e8;font-size:28px'>MP4 audio test</h1>"
        "<p>Autoplay AAC-in-MP4 &mdash; check serial for [med] mp4/aac.</p>"
        "<audio src='http://10.0.2.2:8099/a.m4a' autoplay loop></audio>"
        "</body></html>";
#endif
#ifdef AVSYNC_TEST
    /* MP4 video+audio A/V-sync test: autoplay <video> with an H.264 +
     * AAC MP4 from the host. Watch serial for "[med] mp4/h264 ...",
     * "mp4 pts[0..1] ..." (0 ~40), and "mp4/aac ..." -- both tracks
     * demux, video presents against the audio master clock. */
    html =
        "<html><head><title>A/V sync</title></head><body>"
        "<h1 style='color:#1a73e8;font-size:24px'>A/V sync test</h1>"
        "<p>H.264 + AAC MP4 &mdash; check serial for [med] pts + aac.</p>"
        "<video src='http://10.0.2.2:8099/av.mp4' autoplay loop "
        "width='160' height='120'></video>"
        "</body></html>";
#endif
#ifdef MASKTEST
    /* Controlled mask-image test: a data: SVG circle used as a mask over
     * solid-color boxes -- validates the mask pipeline (intern/decode/
     * currentColor/stencil-fill) without any site's layout quirks. Two
     * boxes should show a red circle and a blue circle. */
    html =
        "<html><head><title>mask-image</title>"
        "<style>body{background:#fff}"
        ".ico{display:inline-block;width:48px;height:48px;"
        "-webkit-mask-image:url(data:image/svg+xml,"
        "%3Csvg%20viewBox='0%200%2024%2024'%3E"
        "%3Crect%20x='3'%20y='3'%20width='18'%20height='18'%20fill='%23000'/%3E%3C/svg%3E);"
        "mask-image:url(data:image/svg+xml,"
        "%3Csvg%20viewBox='0%200%2024%2024'%3E"
        "%3Crect%20x='3'%20y='3'%20width='18'%20height='18'%20fill='%23000'/%3E%3C/svg%3E)}"
        ".r{background:#cc0000}.b{background:#1a73e8}"
        ".cir{display:inline-block;width:48px;height:48px;background:#0a0;"
        "-webkit-mask-image:url(data:image/svg+xml,"
        "%3Csvg%20viewBox='0%200%2024%2024'%3E"
        "%3Ccircle%20cx='12'%20cy='12'%20r='10'%20fill='%23000'/%3E%3C/svg%3E);"
        "mask-image:url(data:image/svg+xml,"
        "%3Csvg%20viewBox='0%200%2024%2024'%3E"
        "%3Ccircle%20cx='12'%20cy='12'%20r='10'%20fill='%23000'/%3E%3C/svg%3E)}"
        "</style></head><body>"
        "<h1 style='color:#111'>mask-image test</h1>"
        "<span class='ico r'></span><span class='ico b'></span>"
        "<span class='cir'></span>"
        "</body></html>";
#endif
#ifdef ESM_TEST
    /* On-screen ES-module test: an inline <script type="module"> imports
     * a module over HTTP which itself uses './' and '../' specifiers,
     * plus named/default/namespace imports, import.meta.url, live
     * bindings and dynamic import(). Serves esm/*.mjs from the host
     * (10.0.2.2:8099). [esm] lines on serial. */
    html =
        "<html><head><title>ES Modules</title>"
        "<style>body{background:#fff;color:#111;font-family:monospace}"
        "#big{font-size:28px;color:#1a73e8}</style></head><body>"
        "<h1 id=big>ESM: running...</h1><div id=out></div>"
        "<script type='module'>"
        "import { run, MAIN } from 'http://10.0.2.2:8099/esm/main.mjs';\n"
        "function L(s){console.log('[esm] '+s);"
        "var d=document.getElementById('out');if(d)d.innerHTML+=s+'<br>';}\n"
        "function fin(ok){var h=document.getElementById('big');"
        "if(h)h.textContent=ok?'ESM: ALL PASS':'ESM: FAIL';"
        "L(ok?'RESULT: ALL PASS':'RESULT: FAIL');L('ALL DONE');}\n"
        "var ok = run(L);\n"
        "L('re-export MAIN: '+(MAIN==='main'?'ok':'FAIL'));"
        "if(MAIN!=='main')ok=false;\n"
        "import('http://10.0.2.2:8099/esm/dyn.mjs').then(function(m){"
        "var d=(m.dyn==='DYN_OK');L('dynamic import(): '+(d?'ok':'FAIL'));"
        "fin(ok&&d);}).catch(function(e){L('dynamic import(): FAIL '+e);"
        "fin(false);});\n"
        "</script>"
        "</body></html>";
#endif
#ifdef STREAM_TEST
    /* On-screen Streams/binary test: binary-safe arrayBuffer(), a
     * ReadableStream reader loop over a fetched .wasm, TextEncoder/
     * Decoder round-trip, and WebAssembly.instantiateStreaming. Serves
     * add.wasm from the host (10.0.2.2:8099). [strm] lines on serial. */
    html =
        "<html><head><title>Streams</title>"
        "<style>body{background:#fff;color:#111;font-family:monospace}"
        "#big{font-size:28px;color:#1a73e8}</style></head><body>"
        "<h1 id=big>Streams: running...</h1><div id=out></div>"
        "<script>"
        "function L(s){console.log('[strm] '+s);var d=document.getElementById('out');"
        "if(d)d.innerHTML+=s+'<br>';}"
        "var ok=true;var imp={env:{host_mul:function(a,b){return (a*b)|0;}}};"
        "function fin(){var h=document.getElementById('big');"
        "if(h)h.textContent=ok?'Streams: ALL PASS':'Streams: FAIL';"
        "L(ok?'RESULT: ALL PASS':'RESULT: FAIL');L('ALL DONE');}"
        "var U='http://10.0.2.2:8099/add.wasm';"
        "fetch(U).then(function(r){return r.arrayBuffer();}).then(function(ab){"
        "L('arrayBuffer bytes='+ab.byteLength);if(ab.byteLength!==156)ok=false;"
        "return WebAssembly.instantiate(ab,imp);}).then(function(res){"
        "var a=res.instance.exports.add(7,35);L('fetched-wasm add(7,35)='+a);if(a!==42)ok=false;"
        "return fetch(U);}).then(function(r){"
        "var rd=r.body.getReader();var tot=0;"
        "function pump(){return rd.read().then(function(x){"
        "if(x.done){L('reader total bytes='+tot);if(tot!==156)ok=false;return;}"
        "tot+=x.value.length;return pump();});}return pump();}).then(function(){"
        "var s='h\\u00e9llo \\u20ac';var e=new TextEncoder().encode(s);"
        "var d=new TextDecoder().decode(e);L('textcodec len='+e.length+' roundtrip='+(d===s));"
        "if(d!==s)ok=false;"
        "return WebAssembly.instantiateStreaming(fetch(U),imp);}).then(function(res){"
        "var a=res.instance.exports.add(10,20);L('instantiateStreaming add(10,20)='+a);if(a!==30)ok=false;"
        /* WritableStream + pipeTo, with an async-producing source */
        "var got=[];var ws=new WritableStream({write:function(c){got.push(c);}});"
        "var rs=new ReadableStream({start:function(c){Promise.resolve().then(function(){"
        "c.enqueue('a');c.enqueue('b');c.enqueue('c');c.close();});}});"
        "return rs.pipeTo(ws).then(function(){L('pipeTo='+got.join(''));if(got.join('')!=='abc')ok=false;"
        /* TransformStream + pipeThrough */
        "var ts=new TransformStream({transform:function(ch,ctl){ctl.enqueue(ch.toUpperCase());}});"
        "var src=new ReadableStream({start:function(c){c.enqueue('x');c.enqueue('y');c.close();}});"
        "var rd=src.pipeThrough(ts).getReader();var acc='';"
        "function p1(){return rd.read().then(function(r){if(r.done)return;acc+=r.value;return p1();});}"
        "return p1().then(function(){L('transform='+acc);if(acc!=='XY')ok=false;"
        /* tee */
        "var tb=new ReadableStream({start:function(c){c.enqueue('1');c.enqueue('2');c.close();}}).tee();"
        "var a1=tb[0].getReader(),a2=tb[1].getReader(),s1='',s2='';"
        "function d1(){return a1.read().then(function(r){if(r.done)return;s1+=r.value;return d1();});}"
        "function d2(){return a2.read().then(function(r){if(r.done)return;s2+=r.value;return d2();});}"
        "return Promise.all([d1(),d2()]).then(function(){L('tee='+s1+'/'+s2);"
        "if(s1!=='12'||s2!=='12')ok=false;"
        /* XHR responseType='arraybuffer' */
        "var xhr=new XMLHttpRequest();xhr.open('GET',U);xhr.responseType='arraybuffer';"
        "xhr.onload=function(){var ab=xhr.response;"
        "L('xhr arraybuffer bytes='+(ab?ab.byteLength:-1));if(!ab||ab.byteLength!==156)ok=false;"
        /* WritableStream backpressure: slow sink, hwm=2 */
        "var wr=[];var ss=new WritableStream({write:function(c){return new Promise(function(res){"
        "setTimeout(function(){wr.push(c);res();},8);});}},{highWaterMark:2});"
        "var w=ss.getWriter();var d0=w.desiredSize;"
        "w.write('a');w.write('b');w.write('c');var bp=(w.desiredSize<=0);"
        "w.ready.then(function(){return w.close();}).then(function(){"
        "L('backpressure d0='+d0+' bp='+bp+' writes='+wr.join(''));"
        "if(d0!==2||!bp||wr.join('')!=='abc')ok=false;fin();});};"
        "xhr.send();"
        "});});});"
        "}).catch(function(e){L('stream error: '+e);ok=false;fin();});"
        "</script></body></html>";
#endif
#ifdef DATAURI_TEST
    /* On-screen data: URI test: a base64 PNG (4 colored quadrants), a
     * percent-encoded SVG (purple square, yellow dot), and a malformed
     * URI that must fail cleanly (broken image, no crash). */
    html =
        "<html><head><title>data: URIs</title>"
        "<style>body{background:#fff;color:#111;font-family:monospace}"
        "img{border:1px solid #888;margin:8px}</style></head><body>"
        "<h1 style='color:#1a73e8'>data: URI images</h1>"
        "<p>1. base64 PNG (red/green/blue/yellow quadrants):</p>"
        "<img src=\"data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAAAf0lEQVR4nN3QQQ3DMADF"
        "UM8ahIAImEArnIIYiIHpvedItfoAWF//85+THdYxtnQkRmIkRmIkRmIkRmIkRmIkRmIk"
        "RmIkRmIkRmIkRmIkRmIk5rvmuSX0G+udD0mMxEiMxEiMxEiMxEiMxEiMxEiMxEiMxEiM"
        "xEiMxEiMTw+4uwATYwTKV/3g+wAAAABJRU5ErkJggg==\">"
        "<p>2. percent-encoded SVG (purple square, yellow dot):</p>"
        "<img src=\"data:image/svg+xml,%3Csvg%20xmlns%3D%22http%3A//www.w3.org"
        "/2000/svg%22%20width%3D%2248%22%20height%3D%2248%22%3E%3Crect%20width"
        "%3D%2248%22%20height%3D%2248%22%20fill%3D%22%237b2ff7%22/%3E%3Ccircle"
        "%20cx%3D%2224%22%20cy%3D%2224%22%20r%3D%2214%22%20fill%3D%22%23ffd400"
        "%22/%3E%3C/svg%3E\">"
        "<p>3. malformed (no comma; must show broken, not crash):</p>"
        "<img src=\"data:image/png;base64\" width=32 height=32>"
        "</body></html>";
#endif
#ifdef SW_TEST
    /* On-screen service-worker test: register a SW from the host, which on
     * install populates a cache and on fetch serves cached + synthesized
     * responses (respondWith) -- plus a page-side Cache API check.
     * [sw] lines on serial. */
    html =
        "<html><head><title>Service Worker</title>"
        "<style>body{background:#fff;color:#111;font-family:monospace}"
        "#big{font-size:28px;color:#1a73e8}</style></head><body>"
        "<h1 id=big>SW: running...</h1><div id=out></div>"
        "<script>"
        "function L(s){console.log('[sw] '+s);var d=document.getElementById('out');"
        "if(d)d.innerHTML+=s+'<br>';}"
        "var ok=true;"
        "function fin(){var h=document.getElementById('big');"
        "if(h)h.textContent=ok?'SW: ALL PASS':'SW: FAIL';"
        "L(ok?'RESULT: ALL PASS':'RESULT: FAIL');L('ALL DONE');}"
        "navigator.serviceWorker.register('http://10.0.2.2:8099/sw.js').then(function(reg){"
        "L('registered scope='+reg.scope+' controller='+(!!navigator.serviceWorker.controller));"
        "return fetch('http://10.0.2.2:8099/cached');}).then(function(r){return r.text();}).then(function(t){"
        "L('fetch /cached (from SW cache)='+t);if(t!=='CACHED_BODY')ok=false;"
        "return fetch('http://10.0.2.2:8099/synth').then(function(r){return r.text();});}).then(function(t){"
        "L('fetch /synth (SW-synthesized)='+t);if(t.indexOf('SYNTH_')!==0)ok=false;"
        "return caches.open('page').then(function(c){"
        "return c.put('k1',new Response('PAGE_CACHED')).then(function(){return c.match('k1');});});}).then(function(r){"
        "return r.text();}).then(function(t){"
        "L('page-side caches match='+t);if(t!=='PAGE_CACHED')ok=false;"
        "fin();}).catch(function(e){L('sw error: '+e);ok=false;fin();});"
        "</script></body></html>";
#endif
#ifdef CACHE_TEST
    /* Persistent Cache test: on first load no marker exists, so cache one
     * and reload; the reloaded page (fresh JS context) restores the cache
     * from disk and finds the marker. [cache] lines on serial. */
    html =
        "<html><head><title>Cache persist</title>"
        "<style>#big{font-size:26px;color:#1a73e8;font-family:sans-serif}</style>"
        "</head><body><h1 id=big>Cache: running...</h1><div id=out></div>"
        "<script>"
        "function L(s){console.log('[cache] '+s);var d=document.getElementById('out');"
        "if(d)d.innerHTML+=s+'<br>';}"
        "var ok=true;"
        "function fin(){var h=document.getElementById('big');"
        "if(h)h.textContent=ok?'Cache: ALL PASS':'Cache: FAIL';"
        "L(ok?'RESULT: ALL PASS':'RESULT: FAIL');L('ALL DONE');}"
        /* the serialization the disk-persist path uses: arbitrary bytes ->
         * Latin-1 string -> JSON -> back, must be lossless */
        "var orig=new Uint8Array([0,1,2,255,254,128,127,65,10,13,34,92]);"
        "var s='';for(var i=0;i<orig.length;i++)s+=String.fromCharCode(orig[i]);"
        "var round=JSON.parse(JSON.stringify({b:s})).b;"
        "var bk=new Uint8Array(round.length);for(var i=0;i<round.length;i++)bk[i]=round.charCodeAt(i)&0xFF;"
        "var eq=(bk.length===orig.length);for(var i=0;i<orig.length;i++)if(bk[i]!==orig[i])eq=false;"
        "L('serialize binary roundtrip='+eq);if(!eq)ok=false;"
        /* cache stores + replays a binary body intact */
        "caches.open('v1').then(function(c){"
        "return c.put('http://t/bin', new Response(orig.buffer)).then(function(){return c.match('http://t/bin');});"
        "}).then(function(r){return r.arrayBuffer();}).then(function(ab){"
        "var u=new Uint8Array(ab);var ceq=(u.length===orig.length);"
        "for(var i=0;i<orig.length;i++)if(u[i]!==orig[i])ceq=false;"
        "L('cache binary body preserved='+ceq+' ('+u.length+' bytes)');if(!ceq)ok=false;"
        "L('(disk persist uses the same idbSave path as IndexedDB/localStorage)');"
        "fin();}).catch(function(e){L('cache error: '+e);ok=false;fin();});"
        "</script></body></html>";
#endif
#ifdef EMOJI_TEST
    /* Color emoji test: emoji codepoints inline with text should render as
     * color glyphs (Twemoji via COLR/CPAL), not monochrome tofu. */
    html =
        "<html><head><title>Emoji</title>"
        "<style>body{background:#fff;color:#111;font-family:sans-serif;margin:16px}"
        "h1{color:#1a73e8}p{font-size:22px}.big{font-size:40px}</style></head><body>"
        "<h1>Color emoji \xF0\x9F\x8E\x89</h1>"
        "<p>Hello \xF0\x9F\x98\x80 world \xF0\x9F\x91\x8D! I \xE2\x9D\xA4\xEF\xB8\x8F tobyOS "
        "\xF0\x9F\x9A\x80 \xF0\x9F\x8C\x88</p>"
        "<p class=big>\xF0\x9F\x98\x80 \xF0\x9F\x98\x8E \xF0\x9F\x98\x82 \xF0\x9F\x91\x8D "
        "\xE2\x9D\xA4\xEF\xB8\x8F \xF0\x9F\x8E\x89 \xF0\x9F\x9A\x80 \xF0\x9F\x8C\x88 "
        "\xF0\x9F\x94\xA5 \xF0\x9F\x8C\x9F</p>"
        "<h1>ZWJ sequences</h1>"
        "<p class=big>"
        "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88 "        /* rainbow flag */
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 " /* family MWG */
        "\xF0\x9F\x91\xA9\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x91\xA8" /* couple WHM */
        "</p></body></html>";
#endif
#ifdef CSSVIS_TEST
    /* Visual CSS test: inline-block flow, conic gradients, per-corner
     * border-radius. Verified by screenshot. */
    html =
        "<html><head><title>CSS visual</title><style>"
        "body{background:#fff;color:#111;font-family:sans-serif;margin:10px}"
        "h2{color:#1a73e8;font-size:18px;margin:12px 0 6px}"
        ".ib{display:inline-block;width:90px;height:60px;margin:4px;"
        "padding:6px;color:#fff;text-align:center;vertical-align:top}"
        ".a{background:#e5484d}.b{background:#2a7e3b}.c{background:#1a73e8}"
        ".auto{display:inline-block;background:#7c3aed;color:#fff;padding:8px 14px;margin:4px}"
        ".conic{width:120px;height:120px;margin:6px;display:inline-block;"
        "background:conic-gradient(#e5484d,#f5a623,#2a7e3b,#1a73e8,#7c3aed,#e5484d);"
        "border-radius:50%}"
        ".corners{width:140px;height:80px;margin:6px;display:inline-block;"
        "background:#0f766e;color:#fff;text-align:center;line-height:80px;"
        "border-radius:4px 24px 4px 24px}"
        "</style></head><body>"
        "<h2>inline-block (three boxes should sit side by side)</h2>"
        "<div class='ib a'>A</div><div class='ib b'>B</div><div class='ib c'>C</div>"
        "<div class='auto'>auto-width</div><div class='auto'>shrinks to fit</div>"
        "<h2>conic-gradient + per-corner radius</h2>"
        "<div class='conic'></div><div class='corners'>4/24/4/24</div>"
        "</body></html>";
#endif

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

/* Free the (page-global) mask registry: called on each page load. Masks
 * survive across style passes (async-fetched, referenced by re-renders),
 * so they reset per document, not per style pass. */
static void masks_free(void) {
    for (int i = 0; i < g_nmasks; i++)
        if (g_masks[i].alpha) { free(g_masks[i].alpha); g_masks[i].alpha = NULL; }
    g_nmasks = 0;
}

/* Free a tab's media elements: file data, MP3 decoder, audio stream.
 * (The video backing store is an img record -- tab_images_free owns
 * that.) Safe to call repeatedly. */
static void tab_media_free(struct tab *t) {
    for (int k = 0; k < t->nmedia; k++) {
        struct media_el *m = &t->media[k];
        if (!m->used) continue;
        if (m->data) { free(m->data); m->data = NULL; }
        if (m->mp3)  { toby_mp3_decode_free((toby_mp3_decoder_t *)m->mp3); m->mp3 = NULL; }
        if (m->aac)  { toby_aac_decode_free((toby_aac_decoder_t *)m->aac); m->aac = NULL; }
        if (m->aud_off) { free(m->aud_off); m->aud_off = NULL; }
        if (m->aud_len) { free(m->aud_len); m->aud_len = NULL; }
        m->naud = 0; m->aud_cur = 0;
        if (m->vdec) { video_decoder_destroy((struct video_decoder *)m->vdec); m->vdec = NULL; }
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

/* Real logos are heavy: enwiki-25.svg's longest single <path d="..">
 * is 19,230 chars and flattens to thousands of points. svgpoly is
 * heap-allocated, so these caps cost one ~70 KiB transient. */
#define SVG_MAX_PTS   8192
#define SVG_MAX_SUBS  256
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

/* ---- Gradient paint servers (v1: average stop color) --------------- *
 * <defs> gradients are parsed in a pre-pass into a small id table; a
 * fill="url(#id)" then paints the AVERAGE of the gradient's stop colors
 * as a solid -- shapes get the right hue and geometry without a
 * per-pixel gradient rasterizer. href/xlink:href stop inheritance
 * (Inkscape-style shared defs) is resolved after the pass. */
#define SVG_GRAD_MAX 128
struct svggrad {
    char     id[48];
    char     ref[48];            /* href="#other" stop inheritance */
    uint32_t sr, sg, sb;         /* opacity-weighted channel sums */
    uint32_t swt;                /* total weight (0..255 per stop) */
    uint32_t avg;                /* resolved average (ARGB) */
    uint8_t  resolved;
};
static struct svggrad g_svg_grads[SVG_GRAD_MAX];
static int g_svg_ngrads;

static struct svggrad *svg_grad_find(const char *id, int len) {
    if (len <= 0) return NULL;
    for (int k = 0; k < g_svg_ngrads; k++) {
        const char *gid = g_svg_grads[k].id;
        int i = 0;
        while (i < len && gid[i] == id[i]) i++;
        if (i == len && gid[i] == 0) return &g_svg_grads[k];
    }
    return NULL;
}

/* Accumulate one <stop> tag's color into the open gradient entry. */
static void svg_grad_stop(struct svggrad *g, const char *s, long i, long end) {
    char a[128];
    uint32_t col = 0;
    int have = 0;
    if (svg_attr(s, i, end, "stop-color", a, sizeof(a)) &&
        css_color_tok(a, (int)str_len(a), &col))
        have = 1;
    int wt = 255;
    if (svg_attr(s, i, end, "stop-opacity", a, sizeof(a))) {
        int p = 0;
        float o = svg_num(a, (int)str_len(a), &p);
        if (o < 0) o = 0;
        if (o > 1) o = 1;
        wt = (int)(o * 255.0f + 0.5f);
    }
    if (svg_attr(s, i, end, "style", a, sizeof(a))) {
        int al = (int)str_len(a);
        int fo = str_contains(a, al, "stop-color:", 11);
        if (fo >= 0) {
            int v0 = fo + 11;
            while (v0 < al && a[v0] == ' ') v0++;
            int v1 = v0;
            while (v1 < al && a[v1] != ';') v1++;
            if (css_color_tok(a + v0, v1 - v0, &col)) have = 1;
        }
    }
    if (!have || wt == 0) return;
    g->sr += ((col >> 16) & 0xFF) * (uint32_t)wt;
    g->sg += ((col >> 8) & 0xFF) * (uint32_t)wt;
    g->sb += (col & 0xFF) * (uint32_t)wt;
    g->swt += (uint32_t)wt;
}

/* Pre-pass over the whole document: every <linearGradient>/
 * <radialGradient> id -> averaged stop color. */
static void svg_collect_gradients(const char *s, long n) {
    g_svg_ngrads = 0;
    struct svggrad *open_g = NULL;
    long i = 0;
    while (i < n) {
        while (i < n && s[i] != '<') i++;
        if (i + 1 >= n) break;
        if (s[i + 1] == '!' || s[i + 1] == '?') {
            i = svg_tag_close(s, n, i) + 1;
            continue;
        }
        if (s[i + 1] == '/') {                        /* closing tag */
            char nm[20];
            long k = i + 2;
            int o = 0;
            while (k < n && s[k] != '>' && s[k] != ' ' && o < 19)
                nm[o++] = (char)lc(s[k++]);
            nm[o] = 0;
            if (str_eq(nm, "lineargradient") || str_eq(nm, "radialgradient"))
                open_g = NULL;
            i = svg_tag_close(s, n, i) + 1;
            continue;
        }
        char nm[20];
        long k = i + 1;
        int o = 0;
        while (k < n && (is_alpha(s[k]) || (s[k] >= '0' && s[k] <= '9')) &&
               o < 19)
            nm[o++] = (char)lc(s[k++]);
        nm[o] = 0;
        long tend = svg_tag_close(s, n, k);
        int selfclose = (tend > i && s[tend - 1] == '/');
        if (str_eq(nm, "lineargradient") || str_eq(nm, "radialgradient")) {
            open_g = NULL;
            char a[96];
            if (svg_attr(s, k, tend, "id", a, sizeof(a)) &&
                g_svg_ngrads < SVG_GRAD_MAX) {
                struct svggrad *g = &g_svg_grads[g_svg_ngrads++];
                mem_zero(g, sizeof(*g));
                str_copy(g->id, a, sizeof(g->id));
                /* svg_attr requires whitespace before the name, so a
                 * plain "href" scan cannot false-match "xlink:href" */
                if (svg_attr(s, k, tend, "href", a, sizeof(a)) ||
                    svg_attr(s, k, tend, "xlink:href", a, sizeof(a))) {
                    const char *r = a[0] == '#' ? a + 1 : a;
                    str_copy(g->ref, r, sizeof(g->ref));
                }
                if (!selfclose) open_g = g;
            }
            i = tend + 1;
            continue;
        }
        if (open_g && str_eq(nm, "stop"))
            svg_grad_stop(open_g, s, k, tend);
        i = tend + 1;
    }
    /* Resolve averages; stop-less gradients inherit through href chains. */
    for (int k2 = 0; k2 < g_svg_ngrads; k2++) {
        struct svggrad *g = &g_svg_grads[k2];
        struct svggrad *src = g;
        for (int hop = 0; hop < 4 && src && src->swt == 0 && src->ref[0]; hop++)
            src = svg_grad_find(src->ref, (int)str_len(src->ref));
        if (src && src->swt > 0) {
            g->avg = 0xFF000000u |
                     ((src->sr / src->swt) << 16) |
                     ((src->sg / src->swt) << 8) |
                     (src->sb / src->swt);
            g->resolved = 1;
        }
    }
}

/* fill="url(#id) [fallback]" -> gradient average, else the spec'd
 * fallback color, else none (per SVG, a broken paint-server reference
 * paints NOTHING -- the old behavior fell through to opaque black).
 * Returns 1 when the value was a url() form (out/none are set). */
static int svg_url_fill(const char *v, int vl, uint32_t *out, int *none) {
    int u = str_contains(v, vl, "url(", 4);
    if (u < 0) return 0;
    int p = u + 4;
    while (p < vl && (v[p] == ' ' || v[p] == '"' || v[p] == '\'')) p++;
    if (p < vl && v[p] == '#') {
        p++;
        int e = p;
        while (e < vl && v[e] != ')' && v[e] != '"' && v[e] != '\'' &&
               v[e] != ' ')
            e++;
        struct svggrad *g = svg_grad_find(v + p, e - p);
        if (g && g->resolved) {
            *out = g->avg;
            *none = 0;
            return 1;
        }
    }
    /* unresolved reference: honor "url(#x) <color>" fallback */
    int q = u;
    while (q < vl && v[q] != ')') q++;
    if (q < vl) q++;
    while (q < vl && v[q] == ' ') q++;
    uint32_t col;
    if (q < vl && css_color_tok(v + q, vl - q, &col)) {
        *out = col | 0xFF000000u;
        *none = 0;
        return 1;
    }
    *none = 1;
    return 1;
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
        if (svg_url_fill(a, (int)str_len(a), out, none)) return;
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
            if (svg_url_fill(a + v0, v1 - v0, out, none)) return;
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

    svg_collect_gradients(s, n);   /* defs can precede OR follow shapes */

    /* Path/points data buffer: sized from the document (a single attr
     * value cannot exceed it) -- the old fixed 8 KiB truncated real
     * logos' 19 KiB+ path data mid-path. */
    long dcap = n + 1;
    if (dcap < 8192) dcap = 8192;
    if (dcap > 262144) dcap = 262144;
    uint32_t *canvas = (uint32_t *)malloc((size_t)W * H * 4);
    struct svgpoly *poly = (struct svgpoly *)malloc(sizeof(struct svgpoly));
    char *dbuf = (char *)malloc((size_t)dcap);
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
                if (svg_attr(s, k, tend, "points", dbuf, (int)dcap)) {
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
                if (svg_attr(s, k, tend, "d", dbuf, (int)dcap)) {
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

/* Decode raw image bytes (any raster format via libtoby, else SVG) to
 * a fresh ARGB canvas. Shared by the network fetch path and data: URIs. */
static uint32_t *img_decode_bytes(const uint8_t *buf, long n, int *ow, int *oh) {
    uint32_t *pix = NULL;
    *ow = *oh = 0;
    toby_image_t *dec = toby_image_load(buf, (size_t)n);
    if (dec && dec->width > 0 && dec->height > 0 &&
        dec->width <= IMG_MAX_DIM && dec->height <= IMG_MAX_DIM) {
        pix = dec->pixels;                  /* steal the decoded buffer */
        *ow = dec->width;
        *oh = dec->height;
        dec->pixels = NULL;
    }
    if (dec) toby_image_free(dec);
    if (!pix && svg_sniff(buf, n))          /* stage 12E: icons */
        pix = svg_render(buf, n, ow, oh);
    return pix;
}

/* ---- data: image URIs ---------------------------------------------- *
 * "data:[<mediatype>][;base64],<payload>" decodes straight into the
 * raster path above -- no fetch. The payload is base64 or percent-
 * encoded text (e.g. data:image/svg+xml,%3Csvg...). */

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;                              /* ws / '=' padding / junk */
}

static long b64_decode_buf(const char *s, long n, uint8_t *out) {
    long o = 0;
    int acc = 0, nbits = 0;
    for (long i = 0; i < n; i++) {
        int v = b64_val(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) { nbits -= 8; out[o++] = (uint8_t)(acc >> nbits); }
    }
    return o;
}

static long pct_decode_buf(const char *s, long n, uint8_t *out) {
    long o = 0;
    for (long i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int h = hex_digit(s[i + 1]), l = hex_digit(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out[o++] = (uint8_t)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        out[o++] = (uint8_t)s[i];
    }
    return o;
}

static uint32_t *data_uri_decode_image(const char *s, int n, int *ow, int *oh) {
    *ow = *oh = 0;
    if (n < 7 || str_ncasecmp(s, "data:", 5) != 0) return NULL;
    long comma = -1;
    for (long i = 5; i < n; i++)
        if (s[i] == ',') { comma = i; break; }
    if (comma < 0) return NULL;
    int is_b64 = ci_find(s + 5, (int)(comma - 5), ";base64") >= 0;
    const char *pl = s + comma + 1;
    long pn = n - comma - 1;
    if (pn <= 0) return NULL;
    long cap = is_b64 ? (pn / 4 + 1) * 3 : pn;
    if (cap > (long)IMG_FETCH_CAP) return NULL; /* mirror the fetch cap */
    uint8_t *buf = (uint8_t *)malloc((size_t)cap + 1);
    if (!buf) return NULL;
    long bn = is_b64 ? b64_decode_buf(pl, pn, buf)
                     : pct_decode_buf(pl, pn, buf);
    uint32_t *pix = bn > 0 ? img_decode_bytes(buf, bn, ow, oh) : NULL;
    free(buf);
    return pix;
}

static uint8_t *g_img_fetch_buf = NULL;

/* Stage 12D: one image transfer in flight at a time, fully async --
 * start it on one idle pass, poll it on the next ones, decode when
 * DONE. Nothing blocks; image state 2 = "in flight". */
static int g_imgf_handle = -1;
static int g_imgf_ti = -1, g_imgf_ii = -1;
/* mask-image fetch: one in flight at a time, indexed into g_masks. */
static int g_maskf_handle = -1;
static int g_maskf_idx = -1;

/* Cancel the in-flight image fetch and clear every in-flight marker
 * (called before tab indices shift, and when a tab's images go away). */
static void img_fetch_cancel(void) {
    if (g_imgf_handle >= 0) {
        sys_http_finish(g_imgf_handle);
        g_imgf_handle = -1;
    }
    if (g_maskf_handle >= 0) {
        sys_http_finish(g_maskf_handle);
        g_maskf_handle = -1; g_maskf_idx = -1;
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

        int iw = 0, ih = 0;
        uint32_t *pix = img_decode_bytes(g_img_fetch_buf, n, &iw, &ih);
#ifdef IMG_PROF
        {   /* decode outcome + opacity profile (black-blob hunting) */
            long opq = 0;
            unsigned long long sr = 0, sg = 0, sb = 0;
            if (pix)
                for (long q2 = 0; q2 < (long)iw * ih; q2++)
                    if ((pix[q2] >> 24) >= 128) {
                        opq++;
                        sr += (pix[q2] >> 16) & 0xFF;
                        sg += (pix[q2] >> 8) & 0xFF;
                        sb += pix[q2] & 0xFF;
                    }
            char m[320]; int p2 = 0;
            p2 = msg_append(m, p2, sizeof(m), "[imgprof] ");
            p2 = msg_append(m, p2, sizeof(m), pix ? "ok " : "FAIL ");
            p2 = msg_append_int(m, p2, sizeof(m), iw);
            p2 = msg_append(m, p2, sizeof(m), "x");
            p2 = msg_append_int(m, p2, sizeof(m), ih);
            p2 = msg_append(m, p2, sizeof(m), " bytes=");
            p2 = msg_append_int(m, p2, sizeof(m), (int)n);
            p2 = msg_append(m, p2, sizeof(m), " opq%=");
            p2 = msg_append_int(m, p2, sizeof(m),
                (pix && iw > 0 && ih > 0)
                    ? (int)(opq * 100 / ((long)iw * ih)) : 0);
            p2 = msg_append(m, p2, sizeof(m), " avg=");
            p2 = msg_append_int(m, p2, sizeof(m), opq ? (int)(sr / opq) : 0);
            p2 = msg_append(m, p2, sizeof(m), ",");
            p2 = msg_append_int(m, p2, sizeof(m), opq ? (int)(sg / opq) : 0);
            p2 = msg_append(m, p2, sizeof(m), ",");
            p2 = msg_append_int(m, p2, sizeof(m), opq ? (int)(sb / opq) : 0);
            p2 = msg_append(m, p2, sizeof(m), " grads=");
            p2 = msg_append_int(m, p2, sizeof(m), g_svg_ngrads);
            p2 = msg_append(m, p2, sizeof(m), " src=");
            p2 = msg_append(m, p2, sizeof(m), im->src);
            p2 = msg_append(m, p2, sizeof(m), "\n");
            sys_write(1, m, p2);
        }
#endif
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

/* Turn a decoded ARGB image into a mask's alpha stencil. For the solid
 * icon SVGs used by design systems the shape is opaque and the ground
 * transparent, so the alpha channel is exactly the coverage we want. */
static void mask_store_alpha(int idx, uint32_t *pix, int w, int h) {
    if (!pix || w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
        g_masks[idx].state = -1; return;
    }
    uint8_t *a = (uint8_t *)malloc((long)w * h);
    if (!a) { g_masks[idx].state = -1; return; }
    for (long i = 0; i < (long)w * h; i++) a[i] = (uint8_t)(pix[i] >> 24);
    g_masks[idx].alpha = a;
    g_masks[idx].w = (int16_t)w;
    g_masks[idx].h = (int16_t)h;
    g_masks[idx].state = 1;
}

/* Fetch+decode one pending mask-image, async, mirroring the image
 * loader. Masks are page-global (shared by URL), so no per-tab index.
 * A finished mask only repaints -- it never changes layout (the icon box
 * has its CSS size regardless), so no re-layout is needed. */
static int load_one_pending_mask(void) {
    if (g_maskf_handle >= 0) {
        struct http_poll pi;
        long pr = sys_http_poll(g_maskf_handle, &pi);
        if (pr == 0 && (pi.state == HTTPA_QUEUED || pi.state == HTTPA_RUNNING))
            return 0;
        int idx = g_maskf_idx;
        long n = -1;
        if (pr == 0 && pi.state == HTTPA_DONE) {
            if (!g_img_fetch_buf)
                g_img_fetch_buf = (uint8_t *)malloc(IMG_FETCH_CAP);
            if (g_img_fetch_buf && pi.body_len > 0)
                n = sys_http_read(g_maskf_handle, g_img_fetch_buf, 0, IMG_FETCH_CAP);
        }
        sys_http_finish(g_maskf_handle);
        g_maskf_handle = -1; g_maskf_idx = -1;
        if (idx < 0 || idx >= g_nmasks || g_masks[idx].state != 2)
            return 1;                         /* navigated away meanwhile */
        if (n <= 0) { g_masks[idx].state = -1; return 1; }
        int w = 0, ht = 0;
        uint32_t *pix = img_decode_bytes(g_img_fetch_buf, n, &w, &ht);
        mask_store_alpha(idx, pix, w, ht);
        if (pix) free(pix);
        tk_redraw(&win);
        return 1;
    }

    for (int i = 0; i < g_nmasks; i++) {
        if (g_masks[i].state != 0 || !g_masks[i].url[0]) continue;
        /* data: mask -> decode inline, nothing to fetch. */
        if (str_ncasecmp(g_masks[i].url, "data:", 5) == 0) {
            int w = 0, ht = 0;
            uint32_t *pix = data_uri_decode_image(g_masks[i].url,
                                (int)str_len(g_masks[i].url), &w, &ht);
            mask_store_alpha(i, pix, w, ht);
            if (pix) free(pix);
            tk_redraw(&win);
            return 1;
        }
        char url[URL_MAX + 1];
        resolve_relative_url(g_url, g_masks[i].url, url, URL_MAX);
        if (!has_scheme(url)) { g_masks[i].state = -1; continue; }
        long h2 = sys_http_start(url, IMG_FETCH_CAP);
        if (h2 < 0) return 0;                 /* slot table busy: retry */
        g_masks[i].state = 2;
        g_maskf_handle = (int)h2;
        g_maskf_idx = i;
        return 0;
    }
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

/* ---- MP4 (ISO base media file format) demux -> H.264 --------------- *
 * Walk the box tree to the video track's sample table, pull the
 * SPS/PPS out of avcC, reconstruct each sample's file offset+size from
 * stsc/stco/stsz, and convert each sample's length-prefixed NALs to
 * Annex-B in place (a 4-byte AVCC length overwrites cleanly with a
 * 00000001 start code). The frame table + the existing H.264 pump path
 * then play it exactly like H.264-in-AVI. */

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Find a child box `type` directly inside [p, p+len). Returns its
 * payload pointer + length via *out_len, or NULL. */
static const uint8_t *mp4_find(const uint8_t *p, long len,
                               const char *type, long *out_len) {
    long o = 0;
    while (o + 8 <= len) {
        uint32_t bsz = rd32be(p + o);
        long hdr = 8;
        long real;
        if (bsz == 1) {                    /* 64-bit largesize */
            if (o + 16 > len) break;
            real = (long)rd32be(p + o + 12);  /* low 32 bits (enough here) */
            hdr = 16;
        } else {
            real = (long)bsz;
        }
        if (bsz == 0) real = len - o;      /* to end */
        if (real < hdr || o + real > len) break;
        if (m4(p + o + 4, type)) {
            *out_len = real - hdr;
            return p + o + hdr;
        }
        o += real;
    }
    return NULL;
}

/* Emit an Annex-B NAL (00000001 + data) into buf, bounded. */
static int mp4_emit_nal(uint8_t *buf, int cap, int pos,
                        const uint8_t *nal, int nlen) {
    if (pos + 4 + nlen > cap) return pos;
    buf[pos] = 0; buf[pos+1] = 0; buf[pos+2] = 0; buf[pos+3] = 1;
    memcpy(buf + pos + 4, nal, nlen);
    return pos + 4 + nlen;
}

static int media_demux_mp4(struct media_el *m) {
    const uint8_t *d = m->data;
    long n = m->dlen;
    if (n < 16 || !m4(d + 4, "ftyp")) return -1;

    long moov_len = 0;
    const uint8_t *moov = mp4_find(d, n, "moov", &moov_len);
    if (!moov) return -1;

    /* Find the video trak: trak whose mdia/hdlr handler == 'vide'. */
    const uint8_t *stbl = NULL; long stbl_len = 0;
    uint32_t timescale = 0;
    long o = 0;
    while (o + 8 <= moov_len) {
        long tl;
        const uint8_t *trak = mp4_find(moov + o, moov_len - o, "trak", &tl);
        if (!trak) break;
        long ml;
        const uint8_t *mdia = mp4_find(trak, tl, "mdia", &ml);
        if (mdia) {
            long hl, mdhl;
            const uint8_t *hdlr = mp4_find(mdia, ml, "hdlr", &hl);
            const uint8_t *mdhd = mp4_find(mdia, ml, "mdhd", &mdhl);
            if (hdlr && hl >= 12 && m4(hdlr + 8, "vide")) {
                if (mdhd && mdhl >= 20) timescale = rd32be(mdhd + 12);
                long minfl;
                const uint8_t *minf = mp4_find(mdia, ml, "minf", &minfl);
                if (minf) stbl = mp4_find(minf, minfl, "stbl", &stbl_len);
                if (stbl) break;
            }
        }
        o = (trak - moov) + tl;            /* advance past this trak */
    }
    if (!stbl) return -1;

    /* avcC (in stsd -> avc1) -> SPS/PPS as Annex-B into m->vcfg. */
    long stsdl;
    const uint8_t *stsd = mp4_find(stbl, stbl_len, "stsd", &stsdl);
    if (!stsd || stsdl < 8) return -1;
    /* stsd: version(4) + entry_count(4) + entries; first entry = sample
     * entry box. Search it (and its children) for avcC. */
    long avccl;
    const uint8_t *entries = stsd + 8;
    const uint8_t *avcc = mp4_find(entries, stsdl - 8, "avcC", &avccl);
    if (!avcc) {
        /* avc1 sample entry: 78-byte VisualSampleEntry header, then avcC. */
        long avc1l;
        const uint8_t *avc1 = mp4_find(entries, stsdl - 8, "avc1", &avc1l);
        if (!avc1) avc1 = mp4_find(entries, stsdl - 8, "avc3", &avc1l);
        if (avc1 && avc1l > 78)
            avcc = mp4_find(avc1 + 78, avc1l - 78, "avcC", &avccl);
    }
    if (!avcc || avccl < 7) return -1;
    /* AVCDecoderConfigurationRecord: [0]=1 [1..3]=profile/compat/level
     * [4]=lengthSizeMinusOne (low 2 bits) [5]=numSPS(low 5 bits), then
     * SPS entries (u16 len + data), then numPPS + PPS entries. */
    if ((avcc[4] & 0x03) != 3) return -1;  /* only 4-byte NAL lengths (v1) */
    m->vcfg_len = 0;
    long ap = 5;
    int nsps = avcc[ap++] & 0x1f;
    for (int i = 0; i < nsps && ap + 2 <= avccl; i++) {
        int sl = (avcc[ap] << 8) | avcc[ap + 1]; ap += 2;
        if (ap + sl > avccl) break;
        m->vcfg_len = mp4_emit_nal(m->vcfg, sizeof m->vcfg, m->vcfg_len,
                                   avcc + ap, sl);
        ap += sl;
    }
    if (ap >= avccl) return -1;
    int npps = avcc[ap++];
    for (int i = 0; i < npps && ap + 2 <= avccl; i++) {
        int pl = (avcc[ap] << 8) | avcc[ap + 1]; ap += 2;
        if (ap + pl > avccl) break;
        m->vcfg_len = mp4_emit_nal(m->vcfg, sizeof m->vcfg, m->vcfg_len,
                                   avcc + ap, pl);
        ap += pl;
    }

    /* Sample sizes (stsz), chunk offsets (stco/co64), sample-to-chunk
     * (stsc), and timing (stts). */
    long stszl, stscl, stcol, sttsl;
    const uint8_t *stsz = mp4_find(stbl, stbl_len, "stsz", &stszl);
    const uint8_t *stsc = mp4_find(stbl, stbl_len, "stsc", &stscl);
    const uint8_t *stco = mp4_find(stbl, stbl_len, "stco", &stcol);
    int co64 = 0;
    if (!stco) { stco = mp4_find(stbl, stbl_len, "co64", &stcol); co64 = 1; }
    const uint8_t *stts = mp4_find(stbl, stbl_len, "stts", &sttsl);
    if (!stsz || !stsc || !stco || stszl < 12 || stscl < 8 || stcol < 8)
        return -1;

    uint32_t samp_size0 = rd32be(stsz + 4);   /* nonzero = all same size */
    uint32_t nsamp = rd32be(stsz + 8);
    uint32_t nchunks = rd32be(stco + 4);
    uint32_t nsc = rd32be(stsc + 4);

    /* Frame period from the first stts entry (constant-fps assumption). */
    m->frame_ms = 40;
    if (stts && sttsl >= 16 && timescale > 0) {
        uint32_t delta = rd32be(stts + 12);
        if (delta > 0) {
            long ms = 1000L * (long)delta / (long)timescale;
            if (ms >= 10 && ms <= 2000) m->frame_ms = ms;
        }
    }

    /* Walk samples via stsc (which chunk each sample lands in) + stco
     * (chunk file offset) + stsz (per-sample size), converting each to
     * Annex-B in place. */
    m->nframes = 0;
    uint32_t sc_idx = 0;                    /* current stsc run */
    uint32_t chunk = 1;
    uint32_t samp_in_chunk = 0;
    uint32_t spc = (nsc > 0) ? rd32be(stsc + 8 + 4) : 1;  /* samples/chunk */
    long sample_off = 0;
    uint32_t stsz_p = 12;
    /* stts cursor for per-frame PTS (cumulative decode time -> ms). */
    uint32_t stts_ent = (stts && sttsl >= 8) ? rd32be(stts + 4) : 0;
    uint32_t stts_i = 0, stts_rem = 0, stts_delta = 0;
    long pts_ticks = 0;
    m->have_pts = (stts_ent > 0 && timescale > 0);
    for (uint32_t s = 0; s < nsamp && m->nframes < MEDIA_FRAMES_MAX; s++) {
        /* Advance stsc run when we pass its first_chunk boundary. */
        while (sc_idx + 1 < nsc) {
            uint32_t next_first = rd32be(stsc + 8 + (sc_idx + 1) * 12);
            if (chunk >= next_first) {
                sc_idx++;
                spc = rd32be(stsc + 8 + sc_idx * 12 + 4);
            } else break;
        }
        if (samp_in_chunk == 0) {          /* first sample of a chunk */
            if (chunk > nchunks) break;
            if (co64)
                sample_off = (long)rd32be(stco + 8 + (chunk - 1) * 8 + 4);
            else
                sample_off = (long)rd32be(stco + 8 + (chunk - 1) * 4);
        }
        uint32_t ssz = samp_size0 ? samp_size0 : rd32be(stsz + stsz_p);
        stsz_p += samp_size0 ? 0 : 4;
        if (sample_off < 0 || sample_off + (long)ssz > n) break;

        /* Convert this sample's 4-byte-length NALs to Annex-B in place. */
        long q = sample_off, sample_end = sample_off + (long)ssz;
        while (q + 4 <= sample_end) {
            uint32_t nlen = rd32be(d + q);
            if (q + 4 + (long)nlen > sample_end) break;
            uint8_t *w = m->data + q;      /* overwrite length with start code */
            w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 1;
            q += 4 + (long)nlen;
        }
        m->frame_off[m->nframes] = sample_off;
        m->frame_len[m->nframes] = (int)ssz;
        if (m->have_pts) {
            m->frame_pts[m->nframes] =
                (int)(pts_ticks * 1000 / (long)timescale);
            /* advance to the next sample's cumulative decode time */
            if (stts_rem == 0 && stts_i < stts_ent) {
                stts_delta = rd32be(stts + 8 + stts_i * 8 + 4);
                stts_rem   = rd32be(stts + 8 + stts_i * 8);
                stts_i++;
            }
            if (stts_rem > 0) { pts_ticks += stts_delta; stts_rem--; }
        }
        m->nframes++;

        sample_off += (long)ssz;
        samp_in_chunk++;
        if (samp_in_chunk >= spc) { samp_in_chunk = 0; chunk++; }
    }
    m->is_mp4 = 1;
    return m->nframes > 0 ? 0 : -1;
}

/* ---- MP4 audio (mp4a / AAC-LC) demux ------------------------------- *
 * Walk the box tree to the 'soun' track's sample table, pull the
 * AudioSpecificConfig out of stsd->mp4a->esds (audioObjectType +
 * samplingFrequencyIndex + channelConfiguration), and build a table of
 * raw AAC access units (file offset + length) from stsc/stco/stsz. The
 * AAC pump feeds each AU to the vendored Helix decoder (raw mode). */

static const uint32_t g_aac_freq[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,  7350,     0,     0,     0
};

/* Find the DecoderSpecificInfo (tag 0x05) payload inside an esds box.
 * esds = version/flags(4) then a descriptor tree: ES_Descriptor(0x03) ->
 * DecoderConfigDescriptor(0x04) -> DecSpecificInfo(0x05). Each descriptor
 * is tag(1) + length(1-4 bytes, 7 bits each, high bit = continue). */
static const uint8_t *esds_find_dsi(const uint8_t *p, long len, long *out_len) {
    long o = 4;                                /* skip version + flags */
    while (o < len) {
        uint8_t tag = p[o++];
        long sz = 0; int b, cnt = 0;
        do { if (o >= len) return NULL; b = p[o++]; sz = (sz << 7) | (b & 0x7f); }
        while ((b & 0x80) && ++cnt < 4);
        if (tag == 0x05) {                     /* DecSpecificInfo = the ASC */
            if (o + sz > len) sz = len - o;
            *out_len = sz;
            return p + o;
        }
        if (tag == 0x03) o += 3;               /* ES_Descriptor: ES_ID(2)+flags(1) */
        else if (tag == 0x04) o += 13;         /* DecoderConfig: object/stream/bufs */
        /* else: descend by falling through to the next tag */
    }
    return NULL;
}

static int media_demux_mp4_audio(struct media_el *m) {
    const uint8_t *d = m->data;
    long n = m->dlen;
    if (n < 16 || !m4(d + 4, "ftyp")) return -1;
    long moov_len = 0;
    const uint8_t *moov = mp4_find(d, n, "moov", &moov_len);
    if (!moov) return -1;

    /* Find the audio trak (mdia/hdlr handler == 'soun'). */
    const uint8_t *stbl = NULL; long stbl_len = 0;
    long o = 0;
    while (o + 8 <= moov_len) {
        long tl;
        const uint8_t *trak = mp4_find(moov + o, moov_len - o, "trak", &tl);
        if (!trak) break;
        long ml;
        const uint8_t *mdia = mp4_find(trak, tl, "mdia", &ml);
        if (mdia) {
            long hl;
            const uint8_t *hdlr = mp4_find(mdia, ml, "hdlr", &hl);
            if (hdlr && hl >= 12 && m4(hdlr + 8, "soun")) {
                long minfl;
                const uint8_t *minf = mp4_find(mdia, ml, "minf", &minfl);
                if (minf) stbl = mp4_find(minf, minfl, "stbl", &stbl_len);
                if (stbl) break;
            }
        }
        o = (trak - moov) + tl;
    }
    if (!stbl) return -1;

    /* stsd -> mp4a sample entry -> esds -> AudioSpecificConfig. */
    long stsdl;
    const uint8_t *stsd = mp4_find(stbl, stbl_len, "stsd", &stsdl);
    if (!stsd || stsdl < 8) return -1;
    const uint8_t *entries = stsd + 8;
    long mp4al;
    const uint8_t *mp4a = mp4_find(entries, stsdl - 8, "mp4a", &mp4al);
    if (!mp4a || mp4al < 28) return -1;
    /* AudioSampleEntry: 28-byte header (incl. channelcount/samplerate),
     * then child boxes -- esds among them. */
    long esdsl;
    const uint8_t *esds = mp4_find(mp4a + 28, mp4al - 28, "esds", &esdsl);
    if (!esds || esdsl < 5) return -1;
    long ascl;
    const uint8_t *asc = esds_find_dsi(esds, esdsl, &ascl);
    if (!asc || ascl < 2) return -1;
    /* ASC: aot(5) freqIdx(4) chanCfg(4). */
    int aot     = (asc[0] >> 3) & 0x1f;
    int freqIdx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);
    int chan    = (asc[1] >> 3) & 0x0f;
    if (aot != 2) return -1;                   /* AAC-LC only */
    m->aac_rate = (freqIdx < 16) ? (int)g_aac_freq[freqIdx] : 0;
    m->aac_ch   = chan;
    if (m->aac_rate <= 0 || m->aac_ch <= 0 || m->aac_ch > 2) return -1;

    /* Build the AU table from stsc/stco/stsz (same walk as the video
     * demux, minus the NAL rewriting -- audio samples are raw AUs). */
    long stszl, stscl, stcol;
    const uint8_t *stsz = mp4_find(stbl, stbl_len, "stsz", &stszl);
    const uint8_t *stsc = mp4_find(stbl, stbl_len, "stsc", &stscl);
    const uint8_t *stco = mp4_find(stbl, stbl_len, "stco", &stcol);
    int co64 = 0;
    if (!stco) { stco = mp4_find(stbl, stbl_len, "co64", &stcol); co64 = 1; }
    if (!stsz || !stsc || !stco || stszl < 12 || stscl < 8 || stcol < 8)
        return -1;

    uint32_t samp_size0 = rd32be(stsz + 4);
    uint32_t nsamp = rd32be(stsz + 8);
    uint32_t nchunks = rd32be(stco + 4);
    uint32_t nsc = rd32be(stsc + 4);
    if (nsamp == 0 || nsamp > 200000) return -1;

    m->aud_off = (long *)malloc((unsigned long)nsamp * sizeof(long));
    m->aud_len = (int  *)malloc((unsigned long)nsamp * sizeof(int));
    if (!m->aud_off || !m->aud_len) return -1;
    m->naud = 0; m->aud_cur = 0;

    uint32_t sc_idx = 0, chunk = 1, samp_in_chunk = 0;
    uint32_t spc = (nsc > 0) ? rd32be(stsc + 8 + 4) : 1;
    long sample_off = 0;
    uint32_t stsz_p = 12;
    for (uint32_t s = 0; s < nsamp; s++) {
        while (sc_idx + 1 < nsc) {
            uint32_t next_first = rd32be(stsc + 8 + (sc_idx + 1) * 12);
            if (chunk >= next_first) { sc_idx++; spc = rd32be(stsc + 8 + sc_idx * 12 + 4); }
            else break;
        }
        if (samp_in_chunk == 0) {
            if (chunk > nchunks) break;
            sample_off = co64 ? (long)rd32be(stco + 8 + (chunk - 1) * 8 + 4)
                              : (long)rd32be(stco + 8 + (chunk - 1) * 4);
        }
        uint32_t ssz = samp_size0 ? samp_size0 : rd32be(stsz + stsz_p);
        stsz_p += samp_size0 ? 0 : 4;
        if (sample_off < 0 || sample_off + (long)ssz > n) break;
        m->aud_off[m->naud] = sample_off;
        m->aud_len[m->naud] = (int)ssz;
        m->naud++;
        sample_off += (long)ssz;
        samp_in_chunk++;
        if (samp_in_chunk >= spc) { samp_in_chunk = 0; chunk++; }
    }
    return m->naud > 0 ? 0 : -1;
}

/* MP3 sniff: ID3v2 tag or an MPEG audio sync word. */
static int media_sniff_mp3(const uint8_t *d, long n) {
    if (n < 4) return 0;
    if (d[0] == 'I' && d[1] == 'D' && d[2] == '3') return 1;
    return d[0] == 0xFF && (d[1] & 0xE0) == 0xE0;
}

/* Nearest-neighbour blit of a decoded frame into the backing store. */
static void media_blit_raw(struct img *dst, const uint32_t *src,
                           int sw, int sh) {
    if (!dst->pixels || !src || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dst->h; y++) {
        int sy = (int)((long)y * sh / dst->h);
        uint32_t *drow = &dst->pixels[(long)y * dst->w];
        const uint32_t *srow = &src[(long)sy * sw];
        for (int x = 0; x < dst->w; x++)
            drow[x] = srow[(long)x * sw / dst->w] | 0xFF000000u;
    }
}

static void media_blit(struct img *dst, toby_image_t *src) {
    media_blit_raw(dst, src->pixels, src->width, src->height);
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

/* Init the AAC decoder for an already-demuxed MP4 audio track (raw AUs). */
static int media_init_mp4_audio_dec(struct media_el *m) {
    m->aac = toby_aac_decode_init();
    if (!m->aac) return -1;
    /* profile 2 = AAC-LC (verified by the demux); raw block params. */
    if (toby_aac_decode_set_raw((toby_aac_decoder_t *)m->aac, 2,
                                m->aac_rate, m->aac_ch) != 0) {
        toby_aac_decode_free((toby_aac_decoder_t *)m->aac);
        m->aac = NULL;
        return -1;
    }
    return 0;
}

/* Best-effort: attach the MP4's AAC audio track alongside its video. A
 * missing/unsupported audio track just leaves the video silent. */
static void media_init_mp4_audio(struct media_el *m) {
    if (media_demux_mp4_audio(m) != 0) return;
    if (media_init_mp4_audio_dec(m) != 0) {
        if (m->aud_off) { free(m->aud_off); m->aud_off = NULL; }
        if (m->aud_len) { free(m->aud_len); m->aud_len = NULL; }
        m->naud = 0;
        return;
    }
    med_log("mp4/aac track AUs rate", m->naud, m->aac_rate);
}

/* Fetch completion: sniff the container and ready the element. */
static void media_ready(struct media_el *m) {
    /* MP4 (H.264) first: ftyp box at offset 4. */
    if (m->is_video && m->dlen > 12 && m4(m->data + 4, "ftyp") &&
        media_demux_mp4(m) == 0) {
        m->vdec = video_decoder_create(VIDEO_CODEC_H264);
        if (!m->vdec) { m->state = -1; return; }
        /* Prime the decoder with the avcC SPS/PPS (no picture yet). */
        if (m->vcfg_len > 0) {
            struct video_frame vf;
            video_decoder_decode((struct video_decoder *)m->vdec,
                                 m->vcfg, (size_t)m->vcfg_len, &vf);
        }
        med_log("mp4/h264 frames/period", m->nframes, (int)m->frame_ms);
        med_log("mp4 pts[0..1] (have_pts=1)",
                m->nframes > 0 ? m->frame_pts[0] : -1,
                m->nframes > 1 ? m->frame_pts[1] : -1);
        media_init_mp4_audio(m);       /* + AAC track if the MP4 has one */
        m->state = 1;
    } else if (!m->is_video && m->dlen > 12 && m4(m->data + 4, "ftyp") &&
               media_demux_mp4_audio(m) == 0) {
        /* Audio-only MP4/M4A: AAC track, no video. */
        if (media_init_mp4_audio_dec(m) != 0) { m->state = -1; return; }
        med_log("mp4/aac AUs rate", m->naud, m->aac_rate);
        m->state = 1;
    } else if (m->is_video && media_demux_avi(m) == 0) {
        /* Codec sniff on the first frame chunk: an Annex-B start code
         * (00 00 [00] 01) means H.264 (real openh264, all profiles);
         * FF D8 means MJPEG (stb JPEG per frame). */
        const uint8_t *f0 = m->data + m->frame_off[0];
        int l0 = m->frame_len[0];
        if (l0 >= 4 && f0[0] == 0 && f0[1] == 0 &&
            (f0[2] == 1 || (f0[2] == 0 && f0[3] == 1))) {
            m->vdec = video_decoder_create(VIDEO_CODEC_H264);
            if (!m->vdec) { m->state = -1; return; }
            med_log("h264 frames/period", m->nframes, (int)m->frame_ms);
        } else {
            med_log("video frames/period", m->nframes, (int)m->frame_ms);
        }
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
        m->play_start = js_now_ms();     /* master-clock anchor (PTS 0 = now) */
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

/* AAC (MP4 mp4a) analogue of media_audio_pump: write leftover PCM, then
 * decode raw AUs from the sample table until the ring pushes back. For a
 * <video> this runs alongside the H.264 pump so audio + video play
 * together; the ring's own pacing keeps them roughly in sync. */
static int media_aac_pump(struct media_el *m) {
    if (!m->aac || !m->aud_off) return 0;
    int work = 0;
    for (int guard = 0; guard < 24; guard++) {
        if (m->carry_len > 0) {
            if (m->audio_fd < 0) break;
            long w = sys_audio_write(m->audio_fd,
                                     (const uint8_t *)m->carry + m->carry_off,
                                     (unsigned long)m->carry_len);
            if (w <= 0) break;                 /* ring full: next pass */
            m->carry_off += (int)w;
            m->carry_len -= (int)w;
            work = 1;
            if (m->carry_len > 0) break;
            continue;
        }
        if (m->aud_cur >= m->naud) break;
        int samples = 0, ch = 0, rate = 0;
        toby_aac_decode_frame((toby_aac_decoder_t *)m->aac,
                              m->data + m->aud_off[m->aud_cur],
                              (size_t)m->aud_len[m->aud_cur],
                              m->carry, &samples, &ch, &rate);
        m->aud_cur++;                          /* one AU per table entry */
        if (samples > 0 && ch > 0 && ch <= 2) {
            if (m->audio_fd < 0) {
                if (rate <= 0) rate = m->aac_rate;
                m->audio_fd = (int)sys_audio_open((uint32_t)rate, ch,
                                                  AUDIO_FMT_PCM16);
                med_log("mp4/aac stream fd/rate", m->audio_fd, rate);
                if (m->audio_fd < 0) { m->playing = 0; return work; }
            }
            m->carry_off = 0;
            m->carry_len = samples * ch * 2;
            work = 1;
        }
    }
    if (m->aud_cur >= m->naud && m->carry_len == 0) {
        if (m->loop) m->aud_cur = 0;
        /* else: let the video pump decide when playback ends. */
        else if (!m->is_video) m->playing = 0;
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
            /* Feed audio every tick (independent of the video frame
             * clock), else it would starve between frames. */
            if (m->aac) work |= media_aac_pump(m);
            if (m->nframes <= 0) continue;
            /* Present against the master clock (wall time since play
             * start; audio self-paces to real time via its ring, so this
             * keeps video locked to audio). Each frame shows when its PTS
             * is due; under slow decode video lags but audio never
             * stutters. */
            long master = now - m->play_start;
            long due_pts = (m->have_pts && m->cur_frame < m->nframes)
                         ? (long)m->frame_pts[m->cur_frame]
                         : (long)m->cur_frame * m->frame_ms;
            if (m->cur_frame < m->nframes && master < due_pts) continue;
            struct img *im = (m->img >= 0 && m->img < g_nimages)
                                 ? &g_images[m->img] : NULL;
            if (!im || !im->pixels) {
                m->cur_frame++;
                if (m->cur_frame >= m->nframes) {
                    if (m->loop) { m->cur_frame = 0; m->play_start = now; }
                    else m->playing = 0;
                }
            } else if (m->vdec) {
                /* H.264 via openh264. Feed samples in stream order until
                 * a picture lands (parameter-set / reorder-held AUs yield
                 * none). openh264 has a ~1-frame output delay, so once
                 * the samples run out we FLUSH (nal_len == 0) to drain
                 * the held picture(s) -- the last frame shows and the
                 * loop boundary stays clean -- and only then wrap or
                 * stop. Lenient on per-NAL errors (SEI variants etc.). */
                struct video_frame vf;
                for (int fed = 0; fed < 8; fed++) {
                    int rc;
                    if (m->cur_frame < m->nframes) {
                        rc = video_decoder_decode(
                            (struct video_decoder *)m->vdec,
                            m->data + m->frame_off[m->cur_frame],
                            (size_t)m->frame_len[m->cur_frame], &vf);
                        m->cur_frame++;
                    } else {
                        rc = video_decoder_decode(
                            (struct video_decoder *)m->vdec, NULL, 0, &vf);
                        if (rc <= 0) {          /* decoder fully drained */
                            if (m->loop) { m->cur_frame = 0; m->play_start = now; }
                            else m->playing = 0;
                            break;
                        }
                    }
                    if (rc > 0) {
                        m->vdec_errs = 0;
                        media_blit_raw(im, (const uint32_t *)vf.data,
                                       vf.width, vf.height);
                        repaint = 1;
                        break;
                    }
                    if (rc < 0 && ++m->vdec_errs > 60) {
                        med_log("h264 giving up after errs",
                                m->vdec_errs, m->cur_frame);
                        m->playing = 0;
                        break;
                    }
                }
            } else {
                toby_image_t *f = toby_image_load(
                    m->data + m->frame_off[m->cur_frame],
                    (size_t)m->frame_len[m->cur_frame]);
                if (f) {
                    media_blit(im, f);
                    toby_image_free(f);
                    repaint = 1;
                }
                m->cur_frame++;
                if (m->cur_frame >= m->nframes) {
                    if (m->loop) m->cur_frame = 0;
                    else m->playing = 0;
                }
            }
            work = 1;
        } else if (m->aac) {
            work |= media_aac_pump(m);         /* audio-only MP4/M4A */
        } else {
            work |= media_audio_pump(m);       /* MP3 */
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

/* ---- Compositing-layer painter (css-anim slice 3) ------------------ *
 * transform: scale/rotate and opacity cannot bake at layout like
 * translate/color do -- they need pixels. A registered layer's items
 * are rendered into an offscreen ARGB buffer (userland replicas of the
 * item painters; text via the kernel TTF->coverage syscall, so glyphs
 * are pixel-identical), then the buffer is affine-blitted to the
 * window row by row: inverse-map each destination pixel through
 * S(1/s)*R(-a) about the layer center, nearest-sample, scale alpha by
 * opacity, tk_draw_blit_blend. All integer math (sin table * 10000,
 * doubled coords for exact half-pixel centers). */

static const int16_t g_sin10k_tab[91] = {
        0,  175,  349,  523,  698,  872, 1045, 1219, 1392, 1564,
     1736, 1908, 2079, 2250, 2419, 2588, 2756, 2924, 3090, 3256,
     3420, 3584, 3746, 3907, 4067, 4226, 4384, 4540, 4695, 4848,
     5000, 5150, 5299, 5446, 5592, 5736, 5878, 6018, 6157, 6293,
     6428, 6561, 6691, 6820, 6947, 7071, 7193, 7314, 7431, 7547,
     7660, 7771, 7880, 7986, 8090, 8192, 8290, 8387, 8480, 8572,
     8660, 8746, 8829, 8910, 8988, 9063, 9135, 9205, 9272, 9336,
     9397, 9455, 9511, 9563, 9613, 9659, 9703, 9744, 9781, 9816,
     9848, 9877, 9903, 9925, 9945, 9962, 9976, 9986, 9994, 9998,
    10000
};
static int sin10k(int deg) {
    deg %= 360; if (deg < 0) deg += 360;
    int s = 1;
    if (deg >= 180) { deg -= 180; s = -1; }
    if (deg > 90) deg = 180 - deg;
    return s * g_sin10k_tab[deg];
}
static int cos10k(int deg) { return sin10k(deg + 90); }

#define LAYER_BUF_W_MAX 1024
#define LAYER_BUF_PX (512 * 512)
static uint32_t g_layer_buf[LAYER_BUF_PX];       /* 1 MiB offscreen ARGB */
#define LAYER_COV_CAP (96 * 1024)
static uint8_t  g_layer_cov[LAYER_COV_CAP];      /* per-text-run coverage */
static uint32_t g_layer_row[2048];               /* one transformed row */

/* Straight-alpha source-over of src onto dst. */
static uint32_t argb_over(uint32_t dst, uint32_t src) {
    uint32_t sa = src >> 24;
    if (sa == 0) return dst;
    if (sa == 255) return src;
    uint32_t da = dst >> 24;
    uint32_t rest = da * (255 - sa) / 255;
    uint32_t oa = sa + rest;
    if (oa == 0) return 0;
    uint32_t r = (((src >> 16) & 255) * sa + ((dst >> 16) & 255) * rest) / oa;
    uint32_t g = (((src >>  8) & 255) * sa + ((dst >>  8) & 255) * rest) / oa;
    uint32_t b = ((src & 255) * sa + (dst & 255) * rest) / oa;
    return (oa << 24) | (r << 16) | (g << 8) | b;
}

static void layer_fill(uint32_t *buf, int lw, int lh,
                       int x, int y, int w, int h, uint32_t rgb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > lw) w = lw - x;
    if (y + h > lh) h = lh - y;
    uint32_t px = 0xFF000000u | (rgb & 0xFFFFFF);
    for (int r = 0; r < h; r++) {
        uint32_t *row = buf + (long)(y + r) * lw + x;
        for (int c = 0; c < w; c++) row[c] = px;
    }
}

/* TTF run into the layer: kernel rasters coverage, we blend fg. The
 * coverage box is padded left/top so glyph overhang (negative xoff)
 * isn't clipped. */
static void layer_text_ttf(uint32_t *buf, int lw, int lh, int x, int y,
                           const char *s, int len, uint32_t fg,
                           int px, int face, int runw, int runh) {
    int pad = px / 4 + 2;
    int cw = runw + pad + px;
    int ch = runh + pad;
    if (cw > 2048) cw = 2048;
    if ((long)cw * ch > LAYER_COV_CAP) return;
    for (long i = 0; i < (long)cw * ch; i++) g_layer_cov[i] = 0;
    if (sys_ttf_raster(g_layer_cov, cw, ch, pad, 0, s, len, px, face) < 0)
        return;
    uint32_t rgb = fg & 0xFFFFFF;
    for (int r = 0; r < ch; r++) {
        int dy = y + r;
        if (dy < 0 || dy >= lh) continue;
        const uint8_t *cov = g_layer_cov + (long)r * cw;
        uint32_t *dst = buf + (long)dy * lw;
        for (int c = 0; c < cw; c++) {
            int dx = x - pad + c;
            if (dx < 0 || dx >= lw || !cov[c]) continue;
            dst[dx] = argb_over(dst[dx], ((uint32_t)cov[c] << 24) | rgb);
        }
    }
}

/* 8x8 monospace run into the layer (mono <pre>/<code> inside layers;
 * canvas bitmap font, close to the kernel console face). */
static void layer_text_mono(uint32_t *buf, int lw, int lh, int x, int y,
                            const char *s, int len, uint32_t fg) {
    uint32_t px = 0xFF000000u | (fg & 0xFFFFFF);
    for (int i = 0; i < len; i++) {
        unsigned ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7E) ch = ' ';
        const unsigned char *gl = g_font8x8[ch - 0x20];
        for (int r = 0; r < 8; r++) {
            int dy = y + r;
            if (dy < 0 || dy >= lh) continue;
            uint32_t *dst = buf + (long)dy * lw;
            for (int c = 0; c < 8; c++) {
                if (!(gl[r] & (1 << c))) continue;
                int dx = x + i * CELL_W + c;
                if (dx >= 0 && dx < lw) dst[dx] = px;
            }
        }
    }
}

/* Render layer L's items into g_layer_buf. Returns 1 and the union
 * bbox (doc coords) on success; 0 when the layer is empty or exceeds
 * the buffer budget (caller falls back to untransformed painting). */
static int layer_render(const struct clayer *L,
                        int *out_x0, int *out_y0, int *out_w, int *out_h) {
    static char tb[512];
    int x0 = 1 << 28, y0 = 1 << 28, x1 = -(1 << 28), y1 = -(1 << 28);
    for (int i = L->i0; i < L->i1 && i < g_nitems; i++) {
        struct ditem *r = &g_items[i];
        if (r->x < x0) x0 = r->x;
        if (r->y < y0) y0 = r->y;
        if (r->x + r->w > x1) x1 = r->x + r->w;
        if (r->y + r->h > y1) y1 = r->y + r->h;
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    x0 -= 2; y0 -= 2; x1 += 2; y1 += 2;   /* glyph-overhang slop */
    int lw = x1 - x0, lh = y1 - y0;
    if (lw > LAYER_BUF_W_MAX || (long)lw * lh > LAYER_BUF_PX) return 0;
    for (long i = 0; i < (long)lw * lh; i++) g_layer_buf[i] = 0;

    for (int ri = L->i0; ri < L->i1 && ri < g_nitems; ri++) {
        struct ditem *r = &g_items[ri];
        int rx = r->x - x0, ry = r->y - y0;
        if (r->kind == DI_RECT || r->kind == DI_BULLET) {
            layer_fill(g_layer_buf, lw, lh, rx, ry, r->w, r->h, r->fg);
            continue;
        }
        if (r->kind == DI_IMG) {
            struct img *im = &g_images[r->img];
            if (im->state == 1 && im->pixels && im->w > 0 && im->h > 0) {
                for (int dy = 0; dy < r->h; dy++) {
                    int by2 = ry + dy;
                    if (by2 < 0 || by2 >= lh) continue;
                    int syy = (int)((long)dy * im->h / r->h);
                    if (syy >= im->h) syy = im->h - 1;
                    const uint32_t *srow = im->pixels + (long)syy * im->w;
                    uint32_t *drow = g_layer_buf + (long)by2 * lw;
                    for (int dx = 0; dx < r->w; dx++) {
                        int bx2 = rx + dx;
                        if (bx2 < 0 || bx2 >= lw) continue;
                        int sxx = (int)((long)dx * im->w / r->w);
                        if (sxx >= im->w) sxx = im->w - 1;
                        drow[bx2] = argb_over(drow[bx2], srow[sxx]);
                    }
                }
            } else {
                layer_fill(g_layer_buf, lw, lh, rx, ry, r->w, r->h, 0x00F1F3F4u);
            }
            continue;
        }
        if (r->kind == DI_FIELD) {
            /* v1: box + border only (form text inside layers is rare) */
            layer_fill(g_layer_buf, lw, lh, rx, ry, r->w, FIELD_H, 0x00FFFFFFu);
            layer_fill(g_layer_buf, lw, lh, rx, ry, r->w, 1, 0x009AA0A6u);
            layer_fill(g_layer_buf, lw, lh, rx, ry + FIELD_H - 1, r->w, 1, 0x009AA0A6u);
            layer_fill(g_layer_buf, lw, lh, rx, ry, 1, FIELD_H, 0x009AA0A6u);
            layer_fill(g_layer_buf, lw, lh, rx + r->w - 1, ry, 1, FIELD_H, 0x009AA0A6u);
            continue;
        }
        /* DI_TEXT */
        if (r->len <= 0) continue;
        int n = r->len < (int)sizeof(tb) - 1 ? r->len : (int)sizeof(tb) - 1;
        {   /* same UTF-8 pass-through as the window paint path */
            int mono_run = (r->fl & IF_MONO) != 0;
            for (int k = 0; k < n; k++) {
                unsigned char ch = (unsigned char)g_render[r->off + k];
                tb[k] = (ch < 0x20) ? ' '
                      : (mono_run && ch > 0x7E) ? '?' : (char)ch;
            }
        }
        tb[n] = '\0';
        uint32_t fg = r->fg & 0xFFFFFF;
        if (r->fl & IF_MONO) {
            if (r->bg >> 24)
                layer_fill(g_layer_buf, lw, lh, rx - 2, ry, r->w + 4, r->h,
                           r->bg & 0xFFFFFF);
            int ty = ry + (r->h - 8) / 2;
            layer_text_mono(g_layer_buf, lw, lh, rx, ty, tb, n, fg);
        } else {
            if (r->bg >> 24)
                layer_fill(g_layer_buf, lw, lh, rx - 1, ry, r->w + 2, r->h,
                           r->bg & 0xFFFFFF);
            int nat = r->px + r->px / 3;
            int ty = ry + (r->h - nat) / 2;
            if (ty < ry) ty = ry;
            layer_text_ttf(g_layer_buf, lw, lh, rx, ty, tb, n, fg,
                           r->px, r->face, r->w, r->h);
            if (r->fl & IF_UNDER)
                layer_fill(g_layer_buf, lw, lh, rx, ty + r->px + 2, r->w, 1, fg);
        }
    }
    *out_x0 = x0; *out_y0 = y0; *out_w = lw; *out_h = lh;
    return 1;
}

/* Paint layer li: offscreen render + affine blit. Returns 1 when the
 * layer was handled (painted or invisible), 0 = fall back to normal
 * untransformed item painting. */
static int paint_layer(int li, int vtop) {
    struct clayer *L = &g_layer_tab[li];
    int x0, y0, lw, lh;
    if (!layer_render(L, &x0, &y0, &lw, &lh)) return 0;
    if (L->opa == 0) return 1;                  /* invisible, but handled */
    int fixed = (g_items[L->i0].fl & IF_FIXED) != 0;

    /* doubled coords keep exact half-pixel centers in integer math */
    long cx2 = 2L * x0 + lw, cy2 = 2L * y0 + lh;
    int c10 = cos10k(L->rot), s10 = sin10k(L->rot);
    long sx = L->scx ? L->scx : 1, sy = L->scy ? L->scy : 1;

    /* destination bbox: forward-transform the 4 corners (scale, rotate) */
    long dx0 = 1 << 28, dy0 = 1 << 28, dx1 = -(1 << 28), dy1 = -(1 << 28);
    for (int k = 0; k < 4; k++) {
        long rx2 = ((k & 1) ? (long)lw : -(long)lw);   /* corner rel *2 */
        long ry2 = ((k & 2) ? (long)lh : -(long)lh);
        long xs = rx2 * sx / 1000, ys = ry2 * sy / 1000;
        long xr = (xs * c10 - ys * s10) / 10000;
        long yr = (xs * s10 + ys * c10) / 10000;
        long px2 = cx2 + xr, py2 = cy2 + yr;
        if (px2 < dx0) dx0 = px2;
        if (px2 > dx1) dx1 = px2;
        if (py2 < dy0) dy0 = py2;
        if (py2 > dy1) dy1 = py2;
    }
    int bx0 = (int)(dx0 / 2) - 1, bx1 = (int)(dx1 / 2) + 2;
    int by0 = (int)(dy0 / 2) - 1, by1 = (int)(dy1 / 2) + 2;
    if (bx0 < 0) bx0 = 0;
    if (bx1 > g_win_w) bx1 = g_win_w;
    if (bx1 <= bx0) return 1;

    int vy0 = CONTENT_TOP, vy1 = CONTENT_TOP + g_content_h;
    for (int yD = by0; yD < by1; yD++) {
        int ys2 = fixed ? (CONTENT_TOP + yD) : (CONTENT_TOP + (yD - vtop));
        if (ys2 < vy0 || ys2 >= vy1) continue;
        long ry2 = 2L * yD + 1 - cy2;
        int cnt = bx1 - bx0;
        for (int i = 0; i < cnt; i++) {
            long rx2 = 2L * (bx0 + i) + 1 - cx2;
            /* inverse: rotate by -a, then unscale */
            long ur = ( (long)c10 * rx2 + (long)s10 * ry2) / 10000;
            long vr = (-(long)s10 * rx2 + (long)c10 * ry2) / 10000;
            long us2 = ur * 1000 / sx, vs2 = vr * 1000 / sy;
            long u = (us2 + cx2) / 2 - x0, v = (vs2 + cy2) / 2 - y0;
            uint32_t px = 0;
            if (u >= 0 && u < lw && v >= 0 && v < lh) {
                px = g_layer_buf[v * lw + u];
                if (L->opa != 255)
                    px = ((((px >> 24) * L->opa) / 255) << 24) | (px & 0xFFFFFF);
            }
            g_layer_row[i] = px;
        }
        tk_draw_blit_blend(&win, bx0, ys2, cnt, 1, g_layer_row, cnt);
    }
    return 1;
}

/* paint_all() draws the whole window (called from the canvas on_paint);
 * redraw() just requests a repaint and is what the event handlers call. */
/* ---- CSS decoration paint: rounded corners + gradients + shadow ----- *
 * All three composite over what's already painted (containers paint
 * before content), so rounded corners and soft shadows blend correctly
 * against the real backdrop. Rows are built in g_layer_row and pushed
 * with tk_draw_blit_blend, like the compositing layer. */

static int isqrt32(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (int)r;
}

/* Signed distance into an axis-aligned rounded rect (w x h, corner r) at
 * local point (px,py): <0 inside, 0 on the boundary, >0 outside. */
static int rbox_sdf(int px, int py, int w, int h, int r) {
    int ax = (2 * px - w); ax = (ax < 0 ? -ax : ax) / 2;    /* |px - w/2| */
    int ay = (2 * py - h); ay = (ay < 0 ? -ay : ay) / 2;
    int qx = ax - (w / 2 - r);
    int qy = ay - (h / 2 - r);
    int mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    int outside = isqrt32((uint32_t)(mx * mx + my * my));
    int inside = (qx > qy ? qx : qy); if (inside > 0) inside = 0;
    return outside + inside - r;
}

/* Per-corner rounded-rect SDF: pick the radius of the corner quadrant the
 * pixel falls in (r4 = TL, TR, BR, BL). */
static int rbox_sdf4(int px, int py, int w, int h, const int *r4) {
    int left = (px < w / 2), top = (py < h / 2);
    int r = top ? (left ? r4[0] : r4[1]) : (left ? r4[3] : r4[2]);
    return rbox_sdf(px, py, w, h, r);
}

/* Integer atan2 -> degrees [0,360). Linear per-octant approximation
 * (a few degrees of error, plenty for a gradient). */
static int iatan2_deg(long y, long x) {
    if (x == 0 && y == 0) return 0;
    long ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int a = (ax >= ay) ? (int)(45 * ay / ax) : 90 - (int)(45 * ax / ay);
    if (x >= 0 && y >= 0) return a;             /* Q1 */
    if (x <  0 && y >= 0) return 180 - a;       /* Q2 */
    if (x <  0 && y <  0) return 180 + a;       /* Q3 */
    return 360 - a;                             /* Q4 */
}

/* Gradient color at local (lx,ly) within a w x h box (ARGB, opaque). */
static uint32_t grad_color(const struct deco *dc, int lx, int ly, int w, int h) {
    int t;
    switch (dc->grad_dir) {
    case 1: t = w > 1 ? lx * 100 / (w - 1) : 0; break;             /* to right */
    case 3: t = w > 1 ? (w - 1 - lx) * 100 / (w - 1) : 0; break;   /* to left */
    case 2: t = h > 1 ? (h - 1 - ly) * 100 / (h - 1) : 0; break;   /* to top */
    case 4: {                                            /* radial (center out) */
        long dx = 2L * lx - w, dy = 2L * ly - h;         /* 2*(offset from center) */
        long d2 = dx * dx + dy * dy;                     /* (2*dist)^2 */
        long r2 = (long)w * w + (long)h * h;             /* (2*corner_dist)^2 */
        t = r2 ? isqrt32((uint32_t)(d2 * 10000 / r2)) : 0;  /* sqrt(d2/r2)*100 */
        if (t > 100) t = 100;
        break;
    }
    case 5: {                                            /* conic (from top, CW) */
        long dx = 2L * lx - w, dy = 2L * ly - h;
        int deg = iatan2_deg(dx, -dy);                   /* 0 at top, CW */
        t = deg * 100 / 360;
        break;
    }
    default: t = h > 1 ? ly * 100 / (h - 1) : 0; break;           /* to bottom */
    }
    if (t < 0) t = 0; if (t > 100) t = 100;
    int i = 0;
    while (i + 1 < dc->grad_n && dc->grad_pos[i + 1] < t) i++;
    int j = (i + 1 < dc->grad_n) ? i + 1 : i;
    int span = dc->grad_pos[j] - dc->grad_pos[i];
    int num = span ? (t - dc->grad_pos[i]) : 0, den = span ? span : 1;
    return css_col_lerp(dc->grad_col[i], dc->grad_col[j], num, den) | 0xFF000000u;
}

/* Paint a (possibly rounded, possibly gradient) rect at screen (sx,sy),
 * clipped to [cx0,cx1)x[cyy0,cyy1). solid used when no gradient. */
static void paint_deco_rect(int sx, int sy, int w, int h, int radius, int deco,
                            uint32_t solid, int cx0, int cx1, int cyy0, int cyy1) {
    if (w <= 0 || h <= 0) return;
    const struct deco *dc = (deco >= 0) ? &g_deco[deco] : 0;
    int r = (radius == 255) ? (w < h ? w : h) / 2 : radius;  /* 255 = 50% */
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    /* per-corner radii (resolve 255=50%, clamp to half-extent) */
    int r4[4]; int use_r4 = (dc && dc->has_r4);
    if (use_r4) {
        int hw = w / 2, hh = h / 2;
        for (int k = 0; k < 4; k++) {
            int v = (dc->r4[k] == 255) ? (w < h ? w : h) / 2 : dc->r4[k];
            if (v > hw) v = hw; if (v > hh) v = hh; if (v < 0) v = 0;
            r4[k] = v;
        }
    }
    /* Honor the solid's alpha: a radius/deco rect is emitted even for a
     * TRANSPARENT background (the item carries the gradient/shadow), and
     * forcing alpha here painted transparent rounded buttons as opaque
     * black boxes (wikipedia header). bg is opaque-or-zero by st_apply. */
    uint32_t sc = solid;
    if ((!dc || !dc->has_grad) && (solid >> 24) == 0)
        return;                            /* nothing to fill */
    for (int yy = 0; yy < h; yy++) {
        int py = sy + yy;
        if (py < cyy0 || py >= cyy1) continue;
        int x0 = sx, x1 = sx + w;
        if (x0 < cx0) x0 = cx0;
        if (x1 > cx1) x1 = cx1;
        int cnt = x1 - x0;
        if (cnt <= 0 || cnt > 2048) { if (cnt > 2048) cnt = 2048; else continue; }
        for (int i = 0; i < cnt; i++) {
            int px = x0 + i;
            uint32_t col = (dc && dc->has_grad) ? grad_color(dc, px - sx, yy, w, h) : sc;
            int cov = 256;
            if (use_r4) { int s = rbox_sdf4(px - sx, yy, w, h, r4);
                          cov = 128 - s * 256; if (cov < 0) cov = 0; if (cov > 256) cov = 256; }
            else if (r > 0) { int s = rbox_sdf(px - sx, yy, w, h, r);
                         cov = 128 - s * 256; if (cov < 0) cov = 0; if (cov > 256) cov = 256; }
            uint32_t a = ((col >> 24) * (uint32_t)cov) / 256;
            g_layer_row[i] = (a << 24) | (col & 0xFFFFFF);
        }
        tk_draw_blit_blend(&win, x0, py, cnt, 1, g_layer_row, cnt);
    }
}

/* Paint a box-shadow (soft rounded rect) for a box at screen (sx,sy). */
static void paint_deco_shadow(int sx, int sy, int w, int h, int radius, int deco,
                              int cx0, int cx1, int cyy0, int cyy1) {
    if (deco < 0) return;
    const struct deco *dc = &g_deco[deco];
    int blur = dc->sh_blur; if (blur < 1) blur = 1;
    int spread = dc->sh_spread;
    int iw = w + 2 * spread, ih = h + 2 * spread;      /* pre-blur shape */
    if (iw <= 0 || ih <= 0) return;
    int rad = (radius == 255) ? (w < h ? w : h) / 2 : radius;   /* 50% */
    int r = rad + spread; if (r < 0) r = 0;
    int ox = sx + dc->sh_dx - spread - blur;
    int oy = sy + dc->sh_dy - spread - blur;
    int ow = iw + 2 * blur, oh = ih + 2 * blur;
    uint32_t scol = dc->sh_col & 0xFFFFFF;
    int salpha = (int)(dc->sh_col >> 24);
    for (int yy = 0; yy < oh; yy++) {
        int py = oy + yy;
        if (py < cyy0 || py >= cyy1) continue;
        int x0 = ox, x1 = ox + ow;
        if (x0 < cx0) x0 = cx0;
        if (x1 > cx1) x1 = cx1;
        int cnt = x1 - x0;
        if (cnt <= 0) continue;
        if (cnt > 2048) cnt = 2048;
        for (int i = 0; i < cnt; i++) {
            int px = x0 + i;
            int lx = px - ox - blur, ly = yy - blur;
            int s = rbox_sdf(lx, ly, iw, ih, r);
            int cov = s <= 0 ? 256 : (s >= blur ? 0 : 256 - s * 256 / blur);
            uint32_t a = (uint32_t)(salpha * cov) / 256;
            g_layer_row[i] = (a << 24) | scol;
        }
        tk_draw_blit_blend(&win, x0, py, cnt, 1, g_layer_row, cnt);
    }
}

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

/* Paint one content display item at scroll offset, with an optional
 * sticky pin shift (sksh, doc-space). Factored out so sticky groups can
 * be repainted on top in a final pass (correct z-order). */
static void paint_content_item(int ri, int sksh, int vtop, int vbot,
                               int vy0, int vy1, uint32_t page_bg) {
    int fd = 0;
    static char tb[512];
    struct ditem *r = &g_items[ri];
#ifdef MASKDBG
    if (r->mask >= 0) {
        struct maskimg *mk = (r->mask < g_nmasks) ? &g_masks[r->mask] : NULL;
        int ry2 = (r->fl & IF_FIXED) ? r->y : r->y + sksh;
        int culled = (r->fl & IF_FIXED)
            ? (r->y + r->h <= 0 || r->y >= g_content_h)
            : (ry2 + r->h <= vtop || ry2 >= vbot);
        char m[240]; int p = 0;
        p = msg_append(m, p, sizeof(m), "[maskdbg] ri=");
        p = msg_append_int(m, p, sizeof(m), ri);
        p = msg_append(m, p, sizeof(m), " xy=");
        p = msg_append_int(m, p, sizeof(m), r->x);
        p = msg_append(m, p, sizeof(m), ",");
        p = msg_append_int(m, p, sizeof(m), r->y);
        p = msg_append(m, p, sizeof(m), " wh=");
        p = msg_append_int(m, p, sizeof(m), r->w);
        p = msg_append(m, p, sizeof(m), "x");
        p = msg_append_int(m, p, sizeof(m), r->h);
        p = msg_append(m, p, sizeof(m), " sksh=");
        p = msg_append_int(m, p, sizeof(m), sksh);
        p = msg_append(m, p, sizeof(m), " clip=");
        p = msg_append_int(m, p, sizeof(m), r->clip);
        if (r->clip) {
            struct cliprect *cl2 = &g_clips[r->clip];
            p = msg_append(m, p, sizeof(m), "[");
            p = msg_append_int(m, p, sizeof(m), cl2->x);
            p = msg_append(m, p, sizeof(m), ",");
            p = msg_append_int(m, p, sizeof(m), cl2->y);
            p = msg_append(m, p, sizeof(m), " ");
            p = msg_append_int(m, p, sizeof(m), cl2->w);
            p = msg_append(m, p, sizeof(m), "x");
            p = msg_append_int(m, p, sizeof(m), cl2->h);
            p = msg_append(m, p, sizeof(m), "]");
        }
        p = msg_append(m, p, sizeof(m), " mstate=");
        p = msg_append_int(m, p, sizeof(m), mk ? mk->state : -9);
        p = msg_append(m, p, sizeof(m), culled ? " CULL-VIEW" : " paint");
        p = msg_append(m, p, sizeof(m), "\n");
        sys_write(1, m, p);
    }
#endif
    int sy;
    if (r->fl & IF_FIXED) {             /* viewport coords, no scroll */
        if (r->y + r->h <= 0 || r->y >= g_content_h) return;
        sy = CONTENT_TOP + r->y;
    } else {
        int ry = r->y + sksh;
        if (ry + r->h <= vtop || ry >= vbot) return;
        sy = CONTENT_TOP + (ry - vtop);
    }
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
    if (r->x >= cx1 || r->x + r->w <= cx0 || sy >= cyy1 || sy + r->h <= cyy0)
        return;

    if (r->kind == DI_SHADOW) {
        paint_deco_shadow(r->x, sy, r->w, r->h, r->radius, r->deco,
                          cx0, cx1, cyy0, cyy1);
        return;
    }
    if (r->kind == DI_RECT) {
        /* mask-image: fill the box with its color (r->fg) shaped by the
         * mask's alpha (mask-size: contain, centered). Pending/failed
         * masks paint NOTHING -- a bare colored square would look worse
         * than a momentarily-absent icon. */
        if (r->mask >= 0) {
            struct maskimg *mk = (r->mask < g_nmasks) ? &g_masks[r->mask] : NULL;
            if (mk && mk->state == 1 && mk->alpha) {
                int rw = r->w, rh = r->h, mw = mk->w, mh = mk->h;
                if (rw > 0 && rh > 0 && mw > 0 && mh > 0) {
                    int sw, sh;
                    if ((long)mw * rh <= (long)mh * rw) {
                        sh = rh; sw = (int)((long)mw * rh / mh);
                    } else { sw = rw; sh = (int)((long)mh * rw / mw); }
                    if (sw < 1) sw = 1;
                    if (sh < 1) sh = 1;
                    int ox = r->x + (rw - sw) / 2, oy = sy + (rh - sh) / 2;
                    uint32_t fg = r->fg;
                    int fa = (fg >> 24) ? (int)(fg >> 24) : 255;
                    uint32_t rgb = fg & 0xFFFFFFu;
                    for (int dy = 0; dy < sh; dy++) {
                        int py = oy + dy;
                        if (py < cyy0 || py >= cyy1) continue;
                        int my = (int)((long)dy * mh / sh);
                        if (my >= mh) my = mh - 1;
                        const uint8_t *mrow = mk->alpha + (long)my * mw;
                        int x0 = ox, x1 = ox + sw;
                        if (x0 < cx0) x0 = cx0;
                        if (x1 > cx1) x1 = cx1;
                        if (x1 > x0 + IMG_ROW_MAX) x1 = x0 + IMG_ROW_MAX;
                        int cnt = x1 - x0;
                        if (cnt <= 0) continue;
                        for (int k = 0; k < cnt; k++) {
                            int dx = x0 + k - ox;
                            int mx = (int)((long)dx * mw / sw);
                            if (mx < 0) mx = 0;
                            if (mx >= mw) mx = mw - 1;
                            uint32_t a = (uint32_t)(fa * mrow[mx]) / 255u;
                            g_img_row[k] = (a << 24) | rgb;
                        }
                        tk_draw_blit_blend(&win, x0, py, cnt, 1, g_img_row, cnt);
                    }
                }
            }
            return;
        }
        if (r->radius > 0 || r->deco >= 0) {
            paint_deco_rect(r->x, sy, r->w, r->h, r->radius, r->deco,
                            r->fg, cx0, cx1, cyy0, cyy1);
            return;
        }
        int x0 = r->x, w = r->w, y0 = sy, h = r->h;
        if (x0 < cx0) { w -= cx0 - x0; x0 = cx0; }
        if (x0 + w > cx1) w = cx1 - x0;
        if (y0 < cyy0) { h -= cyy0 - y0; y0 = cyy0; }
        if (y0 + h > cyy1) h = cyy1 - y0;
        if (h > 0 && w > 0)
            sys_gui_fill(fd, x0, y0, w, h, r->fg & 0xFFFFFF);
        return;
    }
    if (r->kind == DI_BULLET) {
        sys_gui_fill(fd, r->x, sy, r->w, r->h, r->fg & 0xFFFFFF);
        return;
    }
    if (r->kind == DI_IMG) { paint_image(r, sy); return; }
    if (r->kind == DI_EMOJI) {
        int si = r->off;
        struct emoji_glyph *g = (si >= 0 && si < g_neseq)
            ? emoji_get(g_eseq[si], g_eseq_n[si], r->px) : NULL;
        if (g && g->rgba) {
            for (int yy = 0; yy < g->h; yy++) {
                int py = sy + yy;
                if (py < cyy0 || py >= cyy1) continue;
                int x0 = r->x, x1 = r->x + g->w;
                if (x0 < cx0) x0 = cx0;
                if (x1 > cx1) x1 = cx1;
                if (x1 <= x0) continue;
                const uint32_t *row = &g->rgba[(long)yy * g->w + (x0 - r->x)];
                tk_draw_blit_blend(&win, x0, py, x1 - x0, 1, row, x1 - x0);
            }
        }
        return;
    }
    if (r->kind == DI_FIELD) {
        struct field *ff = &g_fields[r->field];
        if (r->fl & IF_INPUT) {
            uint32_t bd = (r->field == g_focus_field) ? 0x001A73E8u : 0x009AA0A6u;
            sys_gui_fill(fd, r->x, sy, r->w, FIELD_H, 0x00FFFFFFu);
            sys_gui_fill(fd, r->x, sy, r->w, 1, bd);
            sys_gui_fill(fd, r->x, sy + FIELD_H - 1, r->w, 1, bd);
            sys_gui_fill(fd, r->x, sy, 1, FIELD_H, bd);
            sys_gui_fill(fd, r->x + r->w - 1, sy, 1, FIELD_H, bd);
            const char *v = ff->value;
            int vl = (int)str_len(v);
            int avail = r->w - 14;
            while (vl > 0) { if (text_px_w(v, vl, PX_BODY, 0, 0) <= avail) break; v++; vl--; }
            if (vl > 0)
                tk_draw_text(&win, r->x + 6, sy + 5, v, 0x00202124u, PX_BODY, 0);
            if (r->field == g_focus_field) {
                int cx = r->x + 6 + text_px_w(v, vl, PX_BODY, 0, 0);
                if (cx > r->x + r->w - 4) cx = r->x + r->w - 4;
                sys_gui_fill(fd, cx, sy + 3, 2, FIELD_H - 6, 0x001A73E8u);
            }
        } else {
            sys_gui_fill(fd, r->x, sy, r->w, FIELD_H, 0x00F1F3F4u);
            sys_gui_fill(fd, r->x, sy, r->w, 1, 0x00DADCE0u);
            sys_gui_fill(fd, r->x, sy + FIELD_H - 1, r->w, 1, 0x00DADCE0u);
            sys_gui_fill(fd, r->x, sy, 1, FIELD_H, 0x00DADCE0u);
            sys_gui_fill(fd, r->x + r->w - 1, sy, 1, FIELD_H, 0x00DADCE0u);
            tk_draw_text(&win, r->x + 14, sy + 5, ff->value, 0x00202124u, PX_BODY, 0);
        }
        return;
    }
    /* DI_TEXT */
    if (r->len <= 0) return;
    int n = r->len < (int)sizeof(tb) - 1 ? r->len : (int)sizeof(tb) - 1;
    {
        int mono_run = (r->fl & IF_MONO) != 0;
        for (int k = 0; k < n; k++) {
            unsigned char ch = (unsigned char)g_render[r->off + k];
            tb[k] = (ch < 0x20) ? ' ' : (mono_run && ch > 0x7E) ? '?' : (char)ch;
        }
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
        return;
    }
    if ((r->bg >> 24) && ri != g_find_run)
        sys_gui_fill(fd, r->x - 1, sy, r->w + 2, r->h, r->bg & 0xFFFFFF);
    int nat = r->px + r->px / 3;
    int ty = sy + (r->h - nat) / 2;
    if (ty < sy) ty = sy;
    sys_text_ttf(win.fd, r->x, ty, tb, fg & 0xFFFFFF, r->px, r->face);
    if (r->fl & IF_UNDER)
        sys_gui_fill(fd, r->x, ty + r->px + 2, r->w, 1, fg);
}

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

        /* position: sticky -- per-group downward pin shift for this
         * scroll offset. shift > 0 once the page scrolls past the
         * threshold, clamped so the element stays inside its container. */
        int sk_shift[STICKY_MAX];
        for (int k = 0; k < g_nsticky; k++) {
            struct sticky *sk = &g_sticky[k];
            int cb_bot = sk->cb_bot < g_doc_h ? sk->cb_bot : g_doc_h;
            int shift = g_scroll_y + sk->top_off - sk->top;
            if (shift < 0) shift = 0;
            int maxs = cb_bot - sk->bot;         /* don't leave the container */
            if (maxs < 0) maxs = 0;
            if (shift > maxs) shift = maxs;
            sk_shift[k] = shift;
        }

        for (int ri = 0; ri < g_nitems; ri++) {
            /* compositing layer starting here? (outermost = widest range
             * wins; nested transforms flatten into it, v1) */
            if (g_nlayer) {
                int li = -1;
                for (int k = 0; k < g_nlayer; k++)
                    if (g_layer_tab[k].i0 == ri &&
                        (li < 0 || g_layer_tab[k].i1 > g_layer_tab[li].i1))
                        li = k;
                if (li >= 0 && paint_layer(li, vtop)) {
                    ri = g_layer_tab[li].i1 - 1;
                    continue;
                }
            }
            /* sticky items are deferred to a final on-top pass (correct
             * z-order: they overlap content that scrolls under them). */
            int is_sticky = 0;
            for (int k = 0; k < g_nsticky; k++)
                if (ri >= g_sticky[k].i0 && ri < g_sticky[k].i1) { is_sticky = 1; break; }
            if (is_sticky) continue;
            paint_content_item(ri, 0, vtop, vbot, vy0, vy1, page_bg);
        }
        /* final pass: sticky groups pinned, painted on top. */
        for (int k = 0; k < g_nsticky; k++)
            for (int ri = g_sticky[k].i0; ri < g_sticky[k].i1 && ri < g_nitems; ri++)
                paint_content_item(ri, sk_shift[k], vtop, vbot, vy0, vy1, page_bg);

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
    else if (ev->type == TK_EV_WHEEL && ev->wheel) {
        /* Same travel per notch as j/k, times the standard 3 lines --
         * and through the same clamp, so the document end still stops
         * dead rather than scrolling into blank space. */
        int px = ev->wheel * TK_WHEEL_LINES * TK_WHEEL_STEP_PX;
        if (px > 0) scroll_up(px); else scroll_down(-px);
        redraw(0);
    }
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
#ifdef YT_TEST
    do_navigate("https://www.youtube.com/");
#endif
/* -DNAV_URL="https://example.com/": auto-navigate at startup. Handy for
 * driving a real site headlessly (pair with -DCSS_PERF). */
#ifdef NAV_URL
    do_navigate(NAV_URL);
#endif

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
        work |= load_one_pending_mask();   /* CSS mask-image icons */
        work |= load_one_pending_media();  /* <video>/<audio> files */
        work |= media_pump();              /* frame + PCM advance */
        work |= js_pump_all();             /* timers + jobs + fetches */
        work |= worker_pump();             /* Web Worker messages + timers */
        work |= anim_pump();               /* CSS @keyframes animations */
        if (!work)
            sys_sleep_ms(15);              /* nothing pending -> idle */
    }
    return 0;
}
