/* tk.c -- TobyTK widget toolkit implementation (see toby/tk.h).
 *
 * Drawing goes through the native gui_* syscalls; text uses the kernel
 * TrueType rasterizer via GUI_TEXT_TTF. The arg packing for each gui_* op is
 * NOT uniform (some pack x|y<<16 as int16, rounded_rect/gradient pack x in the
 * low 32 bits and y in the high 32 bits, fill passes x/y as separate args) --
 * the wrappers below were written against the actual kernel handlers in
 * src/syscall.c, so callers never see the packing.
 */

#include <toby/tk.h>
#include <tobyos/abi/abi.h>     /* ABI_SYS_* (libtoby build adds -I include) */

/* ---- raw syscalls --------------------------------------------------- */

static inline long sc1(long n, long a1) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1)
        : "rcx", "r11", "memory"); return r;
}
static inline long sc2(long n, long a1, long a2) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"); return r;
}
static inline long sc3(long n, long a1, long a2, long a3) {
    long r; register long r10 __asm__("r10") = 0;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory"); return r;
}
static inline long sc5(long n, long a1, long a2, long a3, long a4, long a5) {
    long r; register long r10 __asm__("r10") = a4; register long r8 __asm__("r8") = a5;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"); return r;
}

/* ---- gui_* wrappers (packing matches src/syscall.c handlers) -------- */

static int  g_create(int w, int h, const char *t) {
    return (int)sc3(ABI_SYS_GUI_CREATE, w, h, (long)t);
}
static void g_flip(int fd)             { sc1(ABI_SYS_GUI_FLIP, fd); }
/* Flip only a client-relative sub-rect (per-widget dirty tracking). */
static void g_flip_rect(int fd, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) { g_flip(fd); return; }
    long a2 = ((long)(uint16_t)x) | ((long)(uint16_t)y << 16);
    long a3 = ((long)(uint16_t)w) | ((long)(uint16_t)h << 16);
    sc3(ABI_SYS_GUI_FLIP_RECT, fd, a2, a3);
}

/* ---- userspace draw clip (per-widget partial repaint) --------------- *
 * When active, g_fill trims to it and paint_widget skips widgets fully
 * outside it, so a repaint scoped to a dirty widget's rect only touches
 * that region of the window backbuffer. File-static because the g_*
 * wrappers take an fd, not the window, and repaint is synchronous per
 * window on a single-threaded app. */
static int gclip_on, gclip_x, gclip_y, gclip_w, gclip_h;
static void clip_set(int x, int y, int w, int h) {
    gclip_x = x; gclip_y = y; gclip_w = w; gclip_h = h; gclip_on = 1;
}
static void clip_off(void) { gclip_on = 0; }
/* true => rect (x,y,w,h) is entirely outside the active clip (skip it). */
static int clip_reject(int x, int y, int w, int h) {
    if (!gclip_on) return 0;
    return (x >= gclip_x + gclip_w) || (y >= gclip_y + gclip_h) ||
           (x + w <= gclip_x)       || (y + h <= gclip_y);
}

/* Force a whole-window repaint next pass. Chained assignment dodges any
 * mechanical want_redraw rewrite and marks both flags. */
static void tk_mark_full(struct tk_window *win) {
    if (win) win->want_redraw = win->dirty_full = 1;
}
/* Mark one widget dirty (live-update setters). The next repaint touches
 * only the listed widgets' rects. Overflow / a listed-full window falls
 * back to a whole-window repaint. */
static void tk_mark_dirty(struct tk_window *win, struct tk_widget *w) {
    if (!win || !w) return;
    win->want_redraw = 1;
    if (win->dirty_full) return;
    for (int i = 0; i < win->n_dirty; i++)
        if (win->dirty[i] == w) return;              /* already listed */
    if (win->n_dirty >= TK_MAX_DIRTY) { win->dirty_full = 1; return; }
    win->dirty[win->n_dirty++] = w;
}
static int  g_poll(int fd, struct tk_event *e) {
    return (int)sc2(ABI_SYS_GUI_POLL_EVENT, fd, (long)e);
}
static void g_state(int fd, int s)     { sc2(ABI_SYS_GUI_SET_STATE, fd, s); }
static void g_sleep_ms(int ms)         { sc1(ABI_SYS_NANOSLEEP, (long)ms * 1000000L); }

/* FILL: a1=fd, a2=x, a3=y, a4=(w|h<<16), a5=color. Trims to the active
 * userspace clip so the whole-window / container bg fills only touch the
 * dirty region during a partial repaint. */
static void g_fill(int fd, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (gclip_on) {
        int x1 = x + w, y1 = y + h;
        if (x  < gclip_x)             x  = gclip_x;
        if (y  < gclip_y)             y  = gclip_y;
        if (x1 > gclip_x + gclip_w)   x1 = gclip_x + gclip_w;
        if (y1 > gclip_y + gclip_h)   y1 = gclip_y + gclip_h;
        w = x1 - x; h = y1 - y;
        if (w <= 0 || h <= 0) return;
    }
    long wh = ((long)(uint16_t)w) | ((long)(uint16_t)h << 16);
    sc5(ABI_SYS_GUI_FILL, fd, x, y, wh, (long)c);
}
/* ROUNDED_RECT: a2 = x | (y<<32), a3 = (w&0xFFFF)|((h&0xFFFF)<<16),
 *               a5 = (radius<<24)|(color&0x00FFFFFF) */
static void g_rrect(int fd, int x, int y, int w, int h, int rad, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    long a2 = (long)((uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32));
    long a3 = ((long)(uint16_t)w) | ((long)(uint16_t)h << 16);
    long a5 = ((long)(rad & 0xFF) << 24) | (long)(c & 0x00FFFFFFu);
    sc5(ABI_SYS_GUI_ROUNDED_RECT, fd, a2, a3, 0, a5);
}
/* GRADIENT: a2 = x | (y<<32), a3 = (w&0xFFFF)|((h&0xFFFF)<<16), a4=top, a5=bot.
 * Part of the gui_* wrapper set; retained (with the verified packing) for
 * widgets/apps that want it even though the core widget set draws flat. */
__attribute__((unused))
static void g_gradient(int fd, int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    if (w <= 0 || h <= 0) return;
    long a2 = (long)((uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32));
    long a3 = ((long)(uint16_t)w) | ((long)(uint16_t)h << 16);
    sc5(ABI_SYS_GUI_GRADIENT, fd, a2, a3, (long)top, (long)bot);
}
/* LINE: a2=(x0&0xFFFF)|(y0<<16), a3=(x1&0xFFFF)|(y1<<16), a4=color (int16 coords).
 * Retained wrapper (verified packing) for apps/widgets that draw lines. */
__attribute__((unused))
static void g_line(int fd, int x0, int y0, int x1, int y1, uint32_t c) {
    long a2 = ((long)(uint16_t)x0) | ((long)(uint16_t)y0 << 16);
    long a3 = ((long)(uint16_t)x1) | ((long)(uint16_t)y1 << 16);
    sc5(ABI_SYS_GUI_LINE, fd, a2, a3, (long)c, 0);
}
/* TEXT_TTF: a2=(x&0xFFFF)|(y<<16), a3=str, a4=fg, a5=(px&0xFFFF)|(face<<16) */
static void g_text(int fd, int x, int y, const char *s, uint32_t fg, int px, int face) {
    long a2 = ((long)(uint16_t)x) | ((long)(uint16_t)y << 16);
    long a5 = ((long)(px & 0xFFFF)) | ((long)(face & 0x3) << 16);
    sc5(ABI_SYS_GUI_TEXT_TTF, fd, a2, (long)s, (long)fg, a5);
}
static int g_text_w(const char *s, int px, int face) {
    long a2 = ((long)(px & 0xFFFF)) | ((long)(face & 0x3) << 16);
    long r = sc2(ABI_SYS_GUI_TEXT_TTF_WIDTH, (long)s, a2);
    return r < 0 ? 0 : (int)r;
}
/* MONO bitmap text (8x16 fixed cell, fg + opaque bg): a2=(x|y<<16), a3=str,
 * a4=fg, a5=bg. The fixed-advance VGA font terminals/char-grids need -- one
 * syscall per run, perfectly column-aligned (TTF is proportional). */
static void g_text_mono(int fd, int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    long a2 = ((long)(uint16_t)x) | ((long)(uint16_t)y << 16);
    sc5(ABI_SYS_GUI_TEXT, fd, a2, (long)s, (long)fg, (long)bg);
}
/* RECT (outline): a2=x, a3=y, a4=(w&0xFFFF)|(h<<16), a5=color */
static void g_rect(int fd, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    long wh = ((long)(uint16_t)w) | ((long)(uint16_t)h << 16);
    sc5(ABI_SYS_GUI_RECT, fd, x, y, wh, (long)c);
}
/* CIRCLE (filled): a2=(cx&0xFFFF)|(cy<<16) int16, a3=radius, a4=color */
static void g_circle(int fd, int cx, int cy, int r, uint32_t c) {
    if (r <= 0) return;
    long a2 = ((long)(uint16_t)cx) | ((long)(uint16_t)cy << 16);
    sc5(ABI_SYS_GUI_CIRCLE, fd, a2, r, (long)c, 0);
}
/* CIRCLE_OUTLINE: same packing as CIRCLE */
static void g_circle_outline(int fd, int cx, int cy, int r, uint32_t c) {
    if (r <= 0) return;
    long a2 = ((long)(uint16_t)cx) | ((long)(uint16_t)cy << 16);
    sc5(ABI_SYS_GUI_CIRCLE_OUTLINE, fd, a2, r, (long)c, 0);
}
/* BLIT (opaque): a2=(dx&0xFFFF)|(dy<<16) int16, a3=src(uint32_t*),
 *                a4=(w&0xFFFF)|(h<<16) int16. The kernel treats src as a
 * tightly-packed w*h ARGB buffer, so a strided source is blitted row-wise. */
static void g_blit(int fd, int dx, int dy, int w, int h,
                   const uint32_t *src, int pitch, int op) {
    if (w <= 0 || h <= 0 || !src) return;
    if (pitch <= 0) pitch = w;
    if (pitch == w) {
        long a2 = ((long)(uint16_t)dx) | ((long)(uint16_t)dy << 16);
        long a4 = ((long)(uint16_t)w) | ((long)(uint16_t)h << 16);
        sc5(op, fd, a2, (long)src, a4, 0);
        return;
    }
    for (int r = 0; r < h; r++) {
        long a2 = ((long)(uint16_t)dx) | ((long)(uint16_t)(dy + r) << 16);
        long a4 = ((long)(uint16_t)w) | ((long)(uint16_t)1 << 16);
        sc5(op, fd, a2, (long)(src + (long)r * pitch), a4, 0);
    }
}

/* ---- tiny local helpers (avoid libc header coupling) ---------------- */

static int t_strlen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void t_strcpy_cap(char *d, const char *s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    if (s) for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}
static void t_zero(void *p, int n) { char *c = (char *)p; for (int i = 0; i < n; i++) c[i] = 0; }

/* Line iteration over NUL-terminated, '\n'-separated text (TK_TEXTAREA). */
static int t_line_count(const char *s) {
    if (!s || !*s) return 0;
    int n = 1;
    for (const char *p = s; *p; p++) if (*p == '\n') n++;
    return n;
}
/* Pointer to start of line `idx` (0-based); sets *len to its length sans NL. */
static const char *t_line_at(const char *s, int idx, int *len) {
    if (len) *len = 0;
    if (!s) return 0;
    const char *p = s;
    for (int line = 0; line < idx; line++) {
        while (*p && *p != '\n') p++;
        if (!*p) return 0;
        p++;
    }
    const char *e = p;
    while (*e && *e != '\n') e++;
    if (len) *len = (int)(e - p);
    return p;
}

/* ---- theme ---------------------------------------------------------- */

void tk_theme_default(struct tk_theme *t) {
    t->window_bg     = 0x001B1E24;
    t->panel_bg      = 0x00232831;
    t->text          = 0x00E8ECF2;
    t->text_dim      = 0x008A93A0;
    t->accent        = 0x003DAEE9;   /* Breeze blue */
    t->border        = 0x00343A45;
    t->btn_bg        = 0x00313845;
    t->btn_bg_hover  = 0x003B4453;
    t->btn_bg_active = 0x00283D52;
    t->btn_text      = 0x00EAF0F7;
    t->field_bg      = 0x00171A1F;
    t->field_border  = 0x003A4250;
    t->field_text    = 0x00E8ECF2;
    t->focus_ring    = 0x003DAEE9;
    t->list_bg       = 0x00171A1F;
    t->list_sel      = 0x00204763;
    t->list_text     = 0x00DCE2EA;
    t->check_mark    = 0x003DAEE9;
    t->track         = 0x00171A1F;
    t->track_fill    = 0x003DAEE9;
}

/* ---- widget pool ---------------------------------------------------- */

static struct tk_widget *alloc_widget(struct tk_window *win, int kind) {
    if (win->n >= TK_MAX_WIDGETS) return 0;
    struct tk_widget *w = &win->pool[win->n++];
    t_zero(w, (int)sizeof(*w));
    w->kind = kind;
    w->visible = 1;
    w->enabled = 1;
    w->align = TK_ALIGN_LEFT;
    return w;
}

static void attach(struct tk_window *win, struct tk_widget *parent,
                   struct tk_widget *child) {
    if (!parent) parent = win->root;
    if (!parent || !child || parent == child) return;
    if (parent->n_child >= TK_MAX_CHILDREN) return;
    child->parent = parent;
    parent->child[parent->n_child++] = child;
}

/* ---- construction --------------------------------------------------- */

struct tk_widget *tk_panel(struct tk_window *win, struct tk_widget *p) {
    struct tk_widget *w = alloc_widget(win, TK_PANEL);
    if (w) { w->layout = TK_LAYOUT_NONE; attach(win, p, w); }
    return w;
}
struct tk_widget *tk_vbox(struct tk_window *win, struct tk_widget *p, int gap) {
    struct tk_widget *w = alloc_widget(win, TK_PANEL);
    if (w) { w->layout = TK_LAYOUT_VBOX; w->gap = gap; attach(win, p, w); }
    return w;
}
struct tk_widget *tk_hbox(struct tk_window *win, struct tk_widget *p, int gap) {
    struct tk_widget *w = alloc_widget(win, TK_PANEL);
    if (w) { w->layout = TK_LAYOUT_HBOX; w->gap = gap; attach(win, p, w); }
    return w;
}
struct tk_widget *tk_label(struct tk_window *win, struct tk_widget *p, const char *text) {
    struct tk_widget *w = alloc_widget(win, TK_LABEL);
    if (w) { t_strcpy_cap(w->text, text, TK_TEXT_MAX); attach(win, p, w); }
    return w;
}
struct tk_widget *tk_button(struct tk_window *win, struct tk_widget *p,
                            const char *text, tk_cb on_click) {
    struct tk_widget *w = alloc_widget(win, TK_BUTTON);
    if (w) {
        t_strcpy_cap(w->text, text, TK_TEXT_MAX);
        w->on_click = on_click;
        w->focusable = 1;
        w->align = TK_ALIGN_CENTER;
        attach(win, p, w);
    }
    return w;
}
struct tk_widget *tk_field(struct tk_window *win, struct tk_widget *p, const char *initial) {
    struct tk_widget *w = alloc_widget(win, TK_FIELD);
    if (w) { t_strcpy_cap(w->text, initial, TK_TEXT_MAX); w->focusable = 1; attach(win, p, w); }
    return w;
}
struct tk_widget *tk_checkbox(struct tk_window *win, struct tk_widget *p,
                              const char *label, int checked) {
    struct tk_widget *w = alloc_widget(win, TK_CHECKBOX);
    if (w) {
        t_strcpy_cap(w->text, label, TK_TEXT_MAX);
        w->checked = checked ? 1 : 0;
        w->focusable = 1;
        attach(win, p, w);
    }
    return w;
}
struct tk_widget *tk_listbox(struct tk_window *win, struct tk_widget *p,
                             const char **items, int n) {
    struct tk_widget *w = alloc_widget(win, TK_LISTBOX);
    if (w) {
        w->items = items; w->n_items = n; w->sel = 0; w->scroll = 0;
        w->focusable = 1;
        attach(win, p, w);
    }
    return w;
}
struct tk_widget *tk_slider(struct tk_window *win, struct tk_widget *p, int mn, int mx, int v) {
    struct tk_widget *w = alloc_widget(win, TK_SLIDER);
    if (w) {
        w->value_min = mn; w->value_max = mx; w->value = v;
        w->focusable = 1;
        attach(win, p, w);
    }
    return w;
}
struct tk_widget *tk_progress(struct tk_window *win, struct tk_widget *p, int v, int mx) {
    struct tk_widget *w = alloc_widget(win, TK_PROGRESS);
    if (w) { w->value = v; w->value_max = mx > 0 ? mx : 100; attach(win, p, w); }
    return w;
}
struct tk_widget *tk_separator(struct tk_window *win, struct tk_widget *p) {
    struct tk_widget *w = alloc_widget(win, TK_SEPARATOR);
    if (w) attach(win, p, w);
    return w;
}
struct tk_widget *tk_canvas(struct tk_window *win, struct tk_widget *p, tk_cb on_paint) {
    struct tk_widget *w = alloc_widget(win, TK_CANVAS);
    if (w) {
        w->on_paint = on_paint;
        w->focusable = 1;        /* so it can take keyboard focus on click */
        attach(win, p, w);
    }
    return w;
}
struct tk_widget *tk_on_event(struct tk_widget *w, tk_event_cb on_event) {
    if (w) w->on_event = on_event;
    return w;
}
struct tk_widget *tk_table(struct tk_window *win, struct tk_widget *p,
                           const char *const *headers, const int *col_w, int ncols) {
    struct tk_widget *w = alloc_widget(win, TK_TABLE);
    if (w) {
        w->th = headers; w->col_w = col_w; w->ncols = ncols;
        w->nrows = 0; w->sel = -1; w->scroll = 0;
        w->focusable = 1;
        attach(win, p, w);
    }
    return w;
}
void tk_table_rows(struct tk_window *win, struct tk_widget *w, int nrows,
                   const char *(*cell)(void *user, int row, int col), void *user) {
    if (!w) return;
    w->nrows = nrows < 0 ? 0 : nrows;
    w->cell = cell; w->cell_user = user;
    if (w->sel >= w->nrows) w->sel = w->nrows - 1;
    tk_mark_dirty(win, w);
}
int tk_table_selected(struct tk_widget *w) { return w ? w->sel : -1; }
struct tk_widget *tk_textarea(struct tk_window *win, struct tk_widget *p, const char *text) {
    struct tk_widget *w = alloc_widget(win, TK_TEXTAREA);
    if (w) {
        w->ta_text = text ? text : "";
        w->scroll = 0;
        w->focusable = 1;
        attach(win, p, w);
    }
    return w;
}
void tk_textarea_set(struct tk_window *win, struct tk_widget *w, const char *text) {
    if (!w) return;
    w->ta_text = text ? text : "";
    w->scroll = 0;
    tk_mark_dirty(win, w);
}

/* ---- configuration -------------------------------------------------- */

struct tk_widget *tk_size(struct tk_widget *w, int wd, int ht) {
    if (w) { w->fixed_w = wd; w->fixed_h = ht; } return w;
}
struct tk_widget *tk_grow(struct tk_widget *w, int flex) { if (w) w->flex = flex; return w; }
struct tk_widget *tk_pad(struct tk_widget *w, int pad)   { if (w) w->pad = pad;   return w; }
struct tk_widget *tk_align(struct tk_widget *w, int a)   { if (w) w->align = a;   return w; }
struct tk_widget *tk_colors(struct tk_widget *w, uint32_t bg, uint32_t fg) {
    if (w) { w->bg = bg; w->fg = fg; } return w;
}
struct tk_widget *tk_font(struct tk_widget *w, int px) { if (w) w->font_px = px; return w; }
struct tk_widget *tk_bold(struct tk_widget *w)         { if (w) w->bold = 1;     return w; }

/* ---- canvas draw API (public; wraps the verified g_* primitives) ---- */

void tk_geom(struct tk_widget *w, int *x, int *y, int *ww, int *hh) {
    if (x)  *x  = w ? w->x : 0;
    if (y)  *y  = w ? w->y : 0;
    if (ww) *ww = w ? w->w : 0;
    if (hh) *hh = w ? w->h : 0;
}
void tk_draw_fill(struct tk_window *win, int x, int y, int w, int h, uint32_t c) {
    if (win) g_fill(win->fd, x, y, w, h, c);
}
void tk_draw_rect(struct tk_window *win, int x, int y, int w, int h, uint32_t c) {
    if (win) g_rect(win->fd, x, y, w, h, c);
}
void tk_draw_rrect(struct tk_window *win, int x, int y, int w, int h, int rad, uint32_t c) {
    if (win) g_rrect(win->fd, x, y, w, h, rad, c);
}
void tk_draw_line(struct tk_window *win, int x0, int y0, int x1, int y1, uint32_t c) {
    if (win) g_line(win->fd, x0, y0, x1, y1, c);
}
void tk_draw_circle(struct tk_window *win, int cx, int cy, int r, uint32_t c) {
    if (win) g_circle(win->fd, cx, cy, r, c);
}
void tk_draw_circle_outline(struct tk_window *win, int cx, int cy, int r, uint32_t c) {
    if (win) g_circle_outline(win->fd, cx, cy, r, c);
}
void tk_draw_gradient(struct tk_window *win, int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    if (win) g_gradient(win->fd, x, y, w, h, top, bot);
}
void tk_draw_text(struct tk_window *win, int x, int y, const char *s, uint32_t fg, int px, int bold) {
    if (win && s) g_text(win->fd, x, y, s, fg, px < 8 ? 8 : px, bold ? 1 : 0);
}
void tk_draw_text_mono(struct tk_window *win, int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    if (win && s) g_text_mono(win->fd, x, y, s, fg, bg);
}
int tk_text_width(const char *s, int px, int bold) {
    return g_text_w(s ? s : "", px < 8 ? 8 : px, bold ? 1 : 0);
}
void tk_draw_blit(struct tk_window *win, int x, int y, int w, int h,
                  const uint32_t *argb, int pitch) {
    if (win) g_blit(win->fd, x, y, w, h, argb, pitch, ABI_SYS_GUI_BLIT);
}
void tk_draw_blit_blend(struct tk_window *win, int x, int y, int w, int h,
                        const uint32_t *argb, int pitch) {
    if (win) g_blit(win->fd, x, y, w, h, argb, pitch, ABI_SYS_GUI_BLIT_BLEND);
}

/* ---- accessors ------------------------------------------------------ */

void tk_set_text(struct tk_window *win, struct tk_widget *w, const char *text) {
    if (!w) return;
    t_strcpy_cap(w->text, text, TK_TEXT_MAX);
    tk_mark_dirty(win, w);
}
const char *tk_get_text(struct tk_widget *w) { return w ? w->text : ""; }
int  tk_checked(struct tk_widget *w) { return w ? w->checked : 0; }
void tk_set_checked(struct tk_window *win, struct tk_widget *w, int on) {
    if (!w) return; w->checked = on ? 1 : 0; tk_mark_dirty(win, w);
}
int  tk_selected(struct tk_widget *w) { return w ? w->sel : -1; }
void tk_set_value(struct tk_window *win, struct tk_widget *w, int v) {
    if (!w) return;
    if (v < w->value_min) v = w->value_min;
    if (w->value_max > w->value_min && v > w->value_max) v = w->value_max;
    w->value = v; tk_mark_dirty(win, w);
}

/* Explicit "repaint everything" -- the escape hatch for structural changes
 * (page swaps, rebuilds). Forces a whole-window repaint. */
void tk_redraw(struct tk_window *win) { tk_mark_full(win); }
/* Public per-widget dirty mark (e.g. a canvas whose content changed). */
void tk_dirty(struct tk_window *win, struct tk_widget *w) { tk_mark_dirty(win, w); }
void tk_quit(struct tk_window *win)   { if (win) win->want_quit = 1; }

int tk_checkpoint(struct tk_window *win) { return win ? win->n : 0; }
void tk_clear_children(struct tk_window *win, struct tk_widget *w) {
    if (w) w->n_child = 0;
    tk_mark_full(win);       /* structural change -> whole-window repaint */
}
void tk_rewind(struct tk_window *win, int cp) {
    if (!win || cp < 0 || cp > win->n) return;
    /* Drop dangling focus/capture/hover before freeing the slots. */
    if (win->focus   && (int)(win->focus   - win->pool) >= cp) win->focus = 0;
    if (win->capture && (int)(win->capture - win->pool) >= cp) win->capture = 0;
    win->n = cp;
    tk_mark_full(win);       /* structural change -> whole-window repaint */
}
struct tk_widget *tk_root(struct tk_window *win) { return win ? win->root : 0; }
void tk_maximize(struct tk_window *win) { if (win) g_state(win->fd, TK_WIN_MAXIMIZED); }
void tk_on_key(struct tk_window *win, tk_key_cb on_key) { if (win) win->on_key = on_key; }

/* ---- font helpers --------------------------------------------------- */

static int wid_px(struct tk_window *win, struct tk_widget *w) {
    int px = w->font_px > 0 ? w->font_px : win->default_font_px;
    if (px < 8) px = 8;
    return px;
}
#define LIST_ITEM_H 22
#define TABLE_HEAD_H 24

/* ---- measure (natural size) ----------------------------------------- */

static int measure_w(struct tk_window *win, struct tk_widget *w);
static int measure_h(struct tk_window *win, struct tk_widget *w);

static int leaf_w(struct tk_window *win, struct tk_widget *w) {
    int px = wid_px(win, w);
    int face = w->bold ? 1 : 0;
    switch (w->kind) {
    case TK_LABEL:    return g_text_w(w->text, px, face) + 2;
    case TK_BUTTON:   return g_text_w(w->text, px, face) + 28;
    case TK_FIELD:    return 180;
    case TK_CHECKBOX: return 20 + 8 + g_text_w(w->text, px, face);
    case TK_SEPARATOR:return 8;
    case TK_PROGRESS: return 140;
    case TK_SLIDER:   return 140;
    case TK_LISTBOX:  return 180;
    case TK_TABLE: {
        int tw = 0;
        for (int i = 0; i < w->ncols && w->col_w; i++) tw += w->col_w[i];
        return tw > 0 ? tw + 2 : 240;
    }
    default:          return 0;
    }
}
static int leaf_h(struct tk_window *win, struct tk_widget *w) {
    int px = wid_px(win, w);
    switch (w->kind) {
    case TK_LABEL:    return px + 8;
    case TK_BUTTON:   return px + 16;
    case TK_FIELD:    return px + 14;
    case TK_CHECKBOX: return px + 10;
    case TK_SEPARATOR:return 9;
    case TK_PROGRESS: return 18;
    case TK_SLIDER:   return 24;
    case TK_LISTBOX:  return LIST_ITEM_H * (w->n_items > 0 ? w->n_items : 1) + 4;
    case TK_TABLE: {
        int rows = w->nrows > 0 ? (w->nrows < 8 ? w->nrows : 8) : 3;
        return (w->th ? TABLE_HEAD_H : 0) + rows * LIST_ITEM_H + 4;
    }
    default:          return 0;
    }
}

static int measure_w(struct tk_window *win, struct tk_widget *w) {
    if (!w || !w->visible) return 0;
    if (w->fixed_w > 0) return w->fixed_w;
    if (w->kind != TK_PANEL) return leaf_w(win, w);
    int inner = 0, nvis = 0;
    for (int i = 0; i < w->n_child; i++) {
        struct tk_widget *c = w->child[i];
        if (!c->visible) continue;
        int cw = measure_w(win, c);
        if (w->layout == TK_LAYOUT_HBOX) { inner += cw; nvis++; }
        else if (cw > inner) inner = cw;          /* vbox/none: max child width */
    }
    if (w->layout == TK_LAYOUT_HBOX && nvis > 1) inner += w->gap * (nvis - 1);
    return inner + 2 * w->pad;
}
static int measure_h(struct tk_window *win, struct tk_widget *w) {
    if (!w || !w->visible) return 0;
    if (w->fixed_h > 0) return w->fixed_h;
    if (w->kind != TK_PANEL) return leaf_h(win, w);
    int inner = 0, nvis = 0;
    for (int i = 0; i < w->n_child; i++) {
        struct tk_widget *c = w->child[i];
        if (!c->visible) continue;
        int ch = measure_h(win, c);
        if (w->layout == TK_LAYOUT_VBOX) { inner += ch; nvis++; }
        else if (ch > inner) inner = ch;          /* hbox/none: max child height */
    }
    if (w->layout == TK_LAYOUT_VBOX && nvis > 1) inner += w->gap * (nvis - 1);
    return inner + 2 * w->pad;
}

/* ---- layout (assigns absolute x/y/w/h) ------------------------------ */

static void layout_node(struct tk_window *win, struct tk_widget *w);

static void layout_children(struct tk_window *win, struct tk_widget *w) {
    int ix = w->x + w->pad, iy = w->y + w->pad;
    int iw = w->w - 2 * w->pad, ih = w->h - 2 * w->pad;
    if (iw < 0) iw = 0;
    if (ih < 0) ih = 0;

    if (w->layout == TK_LAYOUT_NONE) {
        /* Children keep their own x/y/w/h relative to this node's inner
         * origin (absolute positioning). */
        for (int i = 0; i < w->n_child; i++) {
            struct tk_widget *c = w->child[i];
            if (!c->visible) continue;
            c->x = ix + c->x;
            c->y = iy + c->y;
            if (c->w == 0) c->w = c->fixed_w > 0 ? c->fixed_w : measure_w(win, c);
            if (c->h == 0) c->h = c->fixed_h > 0 ? c->fixed_h : measure_h(win, c);
            layout_node(win, c);
        }
        return;
    }

    int vbox = (w->layout == TK_LAYOUT_VBOX);
    int main_avail = vbox ? ih : iw;
    int fixed = 0, total_flex = 0, nvis = 0;

    for (int i = 0; i < w->n_child; i++) {
        struct tk_widget *c = w->child[i];
        if (!c->visible) continue;
        nvis++;
        if (c->flex > 0) { total_flex += c->flex; }
        else fixed += vbox ? measure_h(win, c) : measure_w(win, c);
    }
    if (nvis > 1) fixed += w->gap * (nvis - 1);
    int flex_space = main_avail - fixed;
    if (flex_space < 0) flex_space = 0;

    int pos = vbox ? iy : ix;
    for (int i = 0; i < w->n_child; i++) {
        struct tk_widget *c = w->child[i];
        if (!c->visible) continue;

        int main_sz;
        if (c->flex > 0 && total_flex > 0) main_sz = flex_space * c->flex / total_flex;
        else main_sz = vbox ? measure_h(win, c) : measure_w(win, c);

        if (vbox) {
            c->y = pos;
            c->h = main_sz;
            c->x = ix;
            c->w = (c->fixed_w > 0 && c->fixed_w < iw) ? c->fixed_w : iw;  /* stretch cross-axis */
        } else {
            c->x = pos;
            c->w = main_sz;
            c->y = iy;
            c->h = (c->fixed_h > 0 && c->fixed_h < ih) ? c->fixed_h : ih;
        }
        pos += main_sz + w->gap;
        layout_node(win, c);
    }
}

static void layout_node(struct tk_window *win, struct tk_widget *w) {
    if (!w || !w->visible) return;
    if (w->kind == TK_PANEL && w->n_child > 0) layout_children(win, w);
}

static void do_layout(struct tk_window *win) {
    struct tk_widget *r = win->root;
    r->x = 0; r->y = 0; r->w = win->w; r->h = win->h;
    layout_node(win, r);
}

/* ---- painting ------------------------------------------------------- */

static void draw_text_aligned(struct tk_window *win, struct tk_widget *w,
                              int x, int y, int box_w, const char *s, uint32_t fg) {
    int px = wid_px(win, w);
    int face = w->bold ? 1 : 0;
    int tw = g_text_w(s, px, face);
    int tx = x;
    if (w->align == TK_ALIGN_CENTER) tx = x + (box_w - tw) / 2;
    else if (w->align == TK_ALIGN_RIGHT) tx = x + (box_w - tw);
    if (tx < x) tx = x;
    g_text(win->fd, tx, y, s, fg, px, face);
}

static void border(struct tk_window *win, int x, int y, int w, int h, uint32_t c) {
    g_fill(win->fd, x, y, w, 1, c);
    g_fill(win->fd, x, y + h - 1, w, 1, c);
    g_fill(win->fd, x, y, 1, h, c);
    g_fill(win->fd, x + w - 1, y, 1, h, c);
}

static void paint_widget(struct tk_window *win, struct tk_widget *w) {
    if (!w || !w->visible) return;
    /* Partial repaint: skip widgets fully outside the active clip. A
     * container that merely contains the clip still intersects, so it
     * paints (bg trimmed by g_fill) and recurses into its children. */
    if (clip_reject(w->x, w->y, w->w, w->h)) return;
    struct tk_theme *t = &win->theme;
    int px = wid_px(win, w);
    int ty = w->y + (w->h - px) / 2;     /* vertical text centring */

    switch (w->kind) {
    case TK_PANEL:
        if (w->bg) g_fill(win->fd, w->x, w->y, w->w, w->h, w->bg);
        break;
    case TK_LABEL: {
        uint32_t fg = w->fg ? w->fg : t->text;
        draw_text_aligned(win, w, w->x, ty, w->w, w->text, fg);
        break;
    }
    case TK_SEPARATOR:
        g_fill(win->fd, w->x, w->y + w->h / 2, w->w, 1, t->border);
        break;
    case TK_BUTTON: {
        uint32_t bg = w->pressed ? t->btn_bg_active
                    : w->hovered ? t->btn_bg_hover : (w->bg ? w->bg : t->btn_bg);
        g_rrect(win->fd, w->x, w->y, w->w, w->h, 6, bg);
        if (w->focused) {
            /* focus ring just inside the rounded edge */
            border(win, w->x + 1, w->y + 1, w->w - 2, w->h - 2, t->focus_ring);
        }
        uint32_t fg = w->fg ? w->fg : t->btn_text;
        draw_text_aligned(win, w, w->x, ty, w->w, w->text, fg);
        break;
    }
    case TK_FIELD: {
        g_fill(win->fd, w->x, w->y, w->w, w->h, t->field_bg);
        border(win, w->x, w->y, w->w, w->h, w->focused ? t->focus_ring : t->field_border);
        int face = 0;
        int pad = 8;
        const char *s = w->text;
        /* scroll text so the tail (caret) stays visible */
        int avail = w->w - 2 * pad;
        int tw = g_text_w(s, px, face);
        int start = 0;
        while (tw > avail && s[start]) {
            start++;
            tw = g_text_w(s + start, px, face);
        }
        g_text(win->fd, w->x + pad, ty, s + start, t->field_text, px, face);
        if (w->focused) {
            int caret = w->x + pad + g_text_w(s + start, px, face);
            if (caret > w->x + w->w - 3) caret = w->x + w->w - 3;
            g_fill(win->fd, caret, w->y + 4, 2, w->h - 8, t->accent);
        }
        break;
    }
    case TK_CHECKBOX: {
        int box = 18;
        int by = w->y + (w->h - box) / 2;
        g_fill(win->fd, w->x, by, box, box, t->field_bg);
        border(win, w->x, by, box, box, w->focused ? t->focus_ring : t->field_border);
        if (w->checked)
            g_fill(win->fd, w->x + 4, by + 4, box - 8, box - 8, t->check_mark);
        uint32_t fg = w->fg ? w->fg : t->text;
        g_text(win->fd, w->x + box + 8, ty, w->text, fg, px, 0);
        break;
    }
    case TK_LISTBOX: {
        g_fill(win->fd, w->x, w->y, w->w, w->h, t->list_bg);
        border(win, w->x, w->y, w->w, w->h, w->focused ? t->focus_ring : t->field_border);
        int vis = (w->h - 4) / LIST_ITEM_H;
        if (vis < 1) vis = 1;
        if (w->scroll > w->sel) w->scroll = w->sel;
        if (w->scroll + vis <= w->sel) w->scroll = w->sel - vis + 1;
        if (w->scroll < 0) w->scroll = 0;
        for (int i = 0; i < vis; i++) {
            int idx = w->scroll + i;
            if (idx >= w->n_items) break;
            int iy = w->y + 2 + i * LIST_ITEM_H;
            if (idx == w->sel)
                g_fill(win->fd, w->x + 1, iy, w->w - 2, LIST_ITEM_H, t->list_sel);
            const char *it = w->items ? w->items[idx] : "";
            int tpy = iy + (LIST_ITEM_H - px) / 2;
            if (tpy < iy) tpy = iy;
            g_text(win->fd, w->x + 8, tpy, it, t->list_text, px, 0);
        }
        /* scrollbar */
        if (w->n_items > vis) {
            int track_h = w->h - 4;
            int thumb = track_h * vis / w->n_items;
            if (thumb < 8) thumb = 8;
            int ty2 = w->y + 2 + (track_h - thumb) * w->scroll / (w->n_items - vis);
            g_fill(win->fd, w->x + w->w - 5, ty2, 3, thumb, t->border);
        }
        break;
    }
    case TK_SLIDER: {
        int track_h = 4;
        int tyy = w->y + (w->h - track_h) / 2;
        g_fill(win->fd, w->x, tyy, w->w, track_h, t->track);
        int range = w->value_max - w->value_min;
        int fillw = range > 0 ? (w->value - w->value_min) * w->w / range : 0;
        g_fill(win->fd, w->x, tyy, fillw, track_h, t->track_fill);
        int thumb_x = w->x + fillw - 6;
        if (thumb_x < w->x) thumb_x = w->x;
        g_rrect(win->fd, thumb_x, w->y + (w->h - 14) / 2, 12, 14, 3, t->accent);
        break;
    }
    case TK_PROGRESS: {
        g_fill(win->fd, w->x, w->y, w->w, w->h, t->track);
        border(win, w->x, w->y, w->w, w->h, t->border);
        int fillw = w->value_max > 0 ? w->value * (w->w - 2) / w->value_max : 0;
        if (fillw > 0) g_fill(win->fd, w->x + 1, w->y + 1, fillw, w->h - 2, t->track_fill);
        break;
    }
    case TK_CANVAS:
        if (w->bg) g_fill(win->fd, w->x, w->y, w->w, w->h, w->bg);
        if (w->on_paint) w->on_paint(win, w);
        break;
    case TK_TABLE: {
        g_fill(win->fd, w->x, w->y, w->w, w->h, t->list_bg);
        border(win, w->x, w->y, w->w, w->h, w->focused ? t->focus_ring : t->field_border);
        int head_h = w->th ? TABLE_HEAD_H : 0;
        if (w->th) {
            g_fill(win->fd, w->x + 1, w->y + 1, w->w - 2, head_h - 1, t->panel_bg);
            g_fill(win->fd, w->x + 1, w->y + head_h, w->w - 2, 1, t->border);
            int cx = w->x + 8;
            int hpy = w->y + (head_h - px) / 2; if (hpy < w->y + 1) hpy = w->y + 1;
            for (int c = 0; c < w->ncols; c++) {
                if (w->th[c]) g_text(win->fd, cx, hpy, w->th[c], t->text_dim, px, 1);
                cx += w->col_w ? w->col_w[c] : 80;
            }
        }
        int area_y = w->y + head_h + 1;
        int area_h = w->h - head_h - 2;
        int vis = area_h / LIST_ITEM_H; if (vis < 1) vis = 1;
        if (w->sel >= 0) {
            if (w->scroll > w->sel) w->scroll = w->sel;
            if (w->scroll + vis <= w->sel) w->scroll = w->sel - vis + 1;
        }
        if (w->scroll < 0) w->scroll = 0;
        for (int i = 0; i < vis; i++) {
            int row = w->scroll + i;
            if (row >= w->nrows) break;
            int ry = area_y + i * LIST_ITEM_H;
            if (row == w->sel) g_fill(win->fd, w->x + 1, ry, w->w - 2, LIST_ITEM_H, t->list_sel);
            int cx = w->x + 8;
            int tpy = ry + (LIST_ITEM_H - px) / 2; if (tpy < ry) tpy = ry;
            for (int c = 0; c < w->ncols; c++) {
                const char *s = w->cell ? w->cell(w->cell_user, row, c) : "";
                if (s) g_text(win->fd, cx, tpy, s, t->list_text, px, 0);
                cx += w->col_w ? w->col_w[c] : 80;
            }
        }
        if (w->nrows > vis) {
            int thumb = area_h * vis / w->nrows; if (thumb < 8) thumb = 8;
            int sy = area_y + (area_h - thumb) * w->scroll / (w->nrows - vis);
            g_fill(win->fd, w->x + w->w - 5, sy, 3, thumb, t->border);
        }
        break;
    }
    case TK_TEXTAREA: {
        g_fill(win->fd, w->x, w->y, w->w, w->h, t->field_bg);
        border(win, w->x, w->y, w->w, w->h, w->focused ? t->focus_ring : t->field_border);
        int pad = 6;
        int line_h = px + 4;
        int vis = (w->h - 2 * pad) / line_h; if (vis < 1) vis = 1;
        int total = t_line_count(w->ta_text);
        int maxscroll = total - vis; if (maxscroll < 0) maxscroll = 0;
        if (w->scroll > maxscroll) w->scroll = maxscroll;
        if (w->scroll < 0) w->scroll = 0;
        int avail = w->w - 2 * pad;
        for (int i = 0; i < vis; i++) {
            int ll;
            const char *lp = t_line_at(w->ta_text, w->scroll + i, &ll);
            if (!lp) break;
            char line[256];
            int n = ll < (int)sizeof(line) - 1 ? ll : (int)sizeof(line) - 1;
            for (int k = 0; k < n; k++) line[k] = lp[k];
            line[n] = '\0';
            while (n > 0 && g_text_w(line, px, 0) > avail) line[--n] = '\0';
            if (n > 0)
                g_text(win->fd, w->x + pad, w->y + pad + i * line_h, line, t->field_text, px, 0);
        }
        if (total > vis) {
            int track_h = w->h - 4;
            int thumb = track_h * vis / total; if (thumb < 8) thumb = 8;
            int sy = w->y + 2 + (track_h - thumb) * w->scroll / (total - vis);
            g_fill(win->fd, w->x + w->w - 5, sy, 3, thumb, t->border);
        }
        break;
    }
    default: break;
    }

    for (int i = 0; i < w->n_child; i++) paint_widget(win, w->child[i]);
}

/* Whole-window repaint: fill the bg + walk the tree + flip everything. */
static void repaint_full(struct tk_window *win) {
    clip_off();
    do_layout(win);
    g_fill(win->fd, 0, 0, win->w, win->h, win->theme.window_bg);
    paint_widget(win, win->root);
    g_flip(win->fd);
}

static void repaint(struct tk_window *win) {
    if (win->dirty_full || win->n_dirty == 0) {
        repaint_full(win);
    } else {
        /* Partial repaint: only the dirty widgets' rects changed. Snapshot
         * their geometry, re-layout, and if anything moved/resized (e.g. a
         * shrink-to-fit label grew) fall back to a full repaint -- else the
         * old rect would leave stale pixels. Otherwise repaint each dirty
         * widget's exact rect (window bg + ancestor bgs + the widget's
         * subtree, all fresh within that rect) and flip just that rect. */
        int ox[TK_MAX_DIRTY], oy[TK_MAX_DIRTY], ow[TK_MAX_DIRTY], oh[TK_MAX_DIRTY];
        int nd = win->n_dirty;
        for (int i = 0; i < nd; i++) {
            struct tk_widget *d = win->dirty[i];
            ox[i] = d->x; oy[i] = d->y; ow[i] = d->w; oh[i] = d->h;
        }
        do_layout(win);
        int moved = 0;
        for (int i = 0; i < nd; i++) {
            struct tk_widget *d = win->dirty[i];
            if (d->x != ox[i] || d->y != oy[i] || d->w != ow[i] || d->h != oh[i]) { moved = 1; break; }
        }
        if (moved) {
            repaint_full(win);
        } else {
            for (int i = 0; i < nd; i++) {
                struct tk_widget *d = win->dirty[i];
                if (d->w <= 0 || d->h <= 0) continue;
                clip_set(d->x, d->y, d->w, d->h);
                g_fill(win->fd, d->x, d->y, d->w, d->h, win->theme.window_bg);
                paint_widget(win, win->root);
                clip_off();
                g_flip_rect(win->fd, d->x, d->y, d->w, d->h);
            }
        }
    }
    win->want_redraw = 0;
    win->dirty_full  = 0;
    win->n_dirty     = 0;
    clip_off();
}

/* ---- hit-testing + event dispatch ----------------------------------- */

static int point_in(struct tk_widget *w, int x, int y) {
    return x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h;
}

static struct tk_widget *hit_test(struct tk_widget *w, int x, int y) {
    if (!w || !w->visible) return 0;
    if (!point_in(w, x, y)) return 0;
    for (int i = w->n_child - 1; i >= 0; i--) {
        struct tk_widget *h = hit_test(w->child[i], x, y);
        if (h) return h;
    }
    return w;
}

static void set_focus(struct tk_window *win, struct tk_widget *w) {
    if (win->focus == w) return;
    if (win->focus) win->focus->focused = 0;
    win->focus = w;
    if (w) w->focused = 1;
    win->want_redraw = 1;
}

static void field_key(struct tk_widget *w, uint8_t k) {
    int n = t_strlen(w->text);
    if (k == TK_KEY_BACKSPACE) { if (n > 0) w->text[n - 1] = '\0'; return; }
    if (k < 0x20 || k > 0x7E) return;
    if (n + 1 >= TK_TEXT_MAX) return;
    w->text[n] = (char)k; w->text[n + 1] = '\0';
}

static void slider_set_from_x(struct tk_window *win, struct tk_widget *w, int mx) {
    int range = w->value_max - w->value_min;
    if (range <= 0 || w->w <= 0) return;
    int rel = mx - w->x;
    if (rel < 0) rel = 0;
    if (rel > w->w) rel = w->w;
    int v = w->value_min + rel * range / w->w;
    if (v != w->value) {
        w->value = v;
        if (w->on_change) w->on_change(win, w);
        win->want_redraw = 1;
    }
}

static void dispatch(struct tk_window *win, struct tk_event *ev) {
    switch (ev->type) {
    case TK_EV_CLOSE:
        win->want_quit = 1;
        return;
    case TK_EV_RESIZE:
        win->w = ev->x; win->h = ev->y;
        win->want_redraw = 1;
        return;
    case TK_EV_MOUSE_DOWN: {
        struct tk_widget *h = hit_test(win->root, ev->x, ev->y);
        set_focus(win, (h && h->focusable) ? h : 0);
        if (!h) return;
        if (h->kind == TK_BUTTON) {
            h->pressed = 1; win->capture = h; win->want_redraw = 1;
        } else if (h->kind == TK_CHECKBOX) {
            h->checked = !h->checked;
            if (h->on_click) h->on_click(win, h);
            win->want_redraw = 1;
        } else if (h->kind == TK_LISTBOX) {
            int rel = ev->y - (h->y + 2);
            if (rel >= 0) {
                int idx = h->scroll + rel / LIST_ITEM_H;
                if (idx < h->n_items) {
                    h->sel = idx;
                    if (h->on_click) h->on_click(win, h);
                    if (h->on_change) h->on_change(win, h);
                    win->want_redraw = 1;
                }
            }
        } else if (h->kind == TK_SLIDER) {
            win->capture = h;
            slider_set_from_x(win, h, ev->x);
        } else if (h->kind == TK_CANVAS) {
            win->capture = h;
            if (h->on_event) h->on_event(win, h, ev);
        } else if (h->kind == TK_TABLE) {
            int head_h = h->th ? TABLE_HEAD_H : 0;
            int rel = ev->y - (h->y + head_h + 1);
            if (rel >= 0) {
                int row = h->scroll + rel / LIST_ITEM_H;
                if (row >= 0 && row < h->nrows) {
                    h->sel = row;
                    if (h->on_click) h->on_click(win, h);
                    if (h->on_change) h->on_change(win, h);
                    win->want_redraw = 1;
                }
            }
        }
        return;
    }
    case TK_EV_MOUSE_UP: {
        struct tk_widget *cap = win->capture;
        win->capture = 0;
        if (cap && cap->kind == TK_BUTTON) {
            int was = cap->pressed;
            cap->pressed = 0;
            win->want_redraw = 1;
            if (was && point_in(cap, ev->x, ev->y) && cap->on_click)
                cap->on_click(win, cap);
        } else if (cap && cap->kind == TK_CANVAS) {
            if (cap->on_event) cap->on_event(win, cap, ev);
        }
        return;
    }
    case TK_EV_MOUSE_MOVE: {
        struct tk_widget *cap = win->capture;
        if (cap && cap->kind == TK_BUTTON) {
            int now = point_in(cap, ev->x, ev->y);
            if (now != cap->pressed) { cap->pressed = now; win->want_redraw = 1; }
        } else if (cap && cap->kind == TK_SLIDER) {
            slider_set_from_x(win, cap, ev->x);
        } else if (cap && cap->kind == TK_CANVAS) {
            if (cap->on_event) cap->on_event(win, cap, ev);
        } else {
            /* hover tracking */
            struct tk_widget *h = hit_test(win->root, ev->x, ev->y);
            if (h && h->kind != TK_BUTTON) h = 0;   /* only buttons show hover */
            if (h != 0) {
                if (!h->hovered) { h->hovered = 1; win->want_redraw = 1; }
            }
            for (int i = 0; i < win->n; i++) {
                struct tk_widget *p = &win->pool[i];
                if (p->kind == TK_BUTTON && p != h && p->hovered) {
                    p->hovered = 0; win->want_redraw = 1;
                }
            }
        }
        return;
    }
    case TK_EV_KEY: {
        if (win->on_key) { win->on_key(win, ev); return; }
        struct tk_widget *f = win->focus;
        if (!f) return;
        if (f->kind == TK_FIELD) {
            if (ev->key == TK_KEY_ENTER) { if (f->on_click) f->on_click(win, f); }
            else { field_key(f, ev->key); win->want_redraw = 1; }
        } else if (f->kind == TK_BUTTON) {
            if (ev->key == TK_KEY_ENTER || ev->key == ' ')
                if (f->on_click) f->on_click(win, f);
        } else if (f->kind == TK_CHECKBOX) {
            if (ev->key == TK_KEY_ENTER || ev->key == ' ') {
                f->checked = !f->checked;
                if (f->on_click) f->on_click(win, f);
                win->want_redraw = 1;
            }
        } else if (f->kind == TK_LISTBOX) {
            if (ev->key == TK_KEY_DOWN && f->sel < f->n_items - 1) {
                f->sel++; if (f->on_change) f->on_change(win, f); win->want_redraw = 1;
            } else if (ev->key == TK_KEY_UP && f->sel > 0) {
                f->sel--; if (f->on_change) f->on_change(win, f); win->want_redraw = 1;
            }
        } else if (f->kind == TK_SLIDER) {
            if (ev->key == TK_KEY_DOWN && f->value > f->value_min) {
                f->value--; if (f->on_change) f->on_change(win, f); win->want_redraw = 1;
            } else if (ev->key == TK_KEY_UP && f->value < f->value_max) {
                f->value++; if (f->on_change) f->on_change(win, f); win->want_redraw = 1;
            }
        } else if (f->kind == TK_CANVAS) {
            if (f->on_event) f->on_event(win, f, ev);
        } else if (f->kind == TK_TABLE) {
            if (ev->key == TK_KEY_DOWN && f->sel < f->nrows - 1) {
                f->sel++; if (f->on_change) f->on_change(win, f); win->want_redraw = 1;
            } else if (ev->key == TK_KEY_UP && f->sel > 0) {
                f->sel--; if (f->on_change) f->on_change(win, f); win->want_redraw = 1;
            }
        } else if (f->kind == TK_TEXTAREA) {
            if (ev->key == TK_KEY_DOWN) { f->scroll++; win->want_redraw = 1; }
            else if (ev->key == TK_KEY_UP && f->scroll > 0) { f->scroll--; win->want_redraw = 1; }
        }
        return;
    }
    default: return;
    }
}

/* ---- lifecycle + loop ----------------------------------------------- */

int tk_window_open(struct tk_window *win, int w, int h, const char *title) {
    t_zero(win, (int)sizeof(*win));
    win->w = w; win->h = h;
    win->default_font_px = 15;
    tk_theme_default(&win->theme);
    int fd = g_create(w, h, title ? title : "");
    if (fd < 0) return -1;
    win->fd = fd;
    /* root vbox fills the window */
    struct tk_widget *root = alloc_widget(win, TK_PANEL);
    root->layout = TK_LAYOUT_VBOX;
    win->root = root;
    tk_mark_full(win);       /* first paint is a whole-window repaint */
    return 0;
}

int tk_pump(struct tk_window *win) {
    struct tk_event ev;
    int any = 0;
    while (g_poll(win->fd, &ev) > 0) {
        dispatch(win, &ev);
        any = 1;
        if (win->want_quit) return 1;
    }
    if (win->want_redraw) {
        /* Event handlers set want_redraw without per-widget marking, so an
         * event-driven repaint must be full. A repaint with no events is
         * pure setter marks (e.g. a live metrics refresh) -> partial. */
        if (any) win->dirty_full = 1;
        repaint(win);
    }
    return win->want_quit;
}

int tk_run(struct tk_window *win) {
    repaint(win);
    for (;;) {
        if (win->want_quit) return 0;
        struct tk_event ev;
        int got = g_poll(win->fd, &ev);
        if (got < 0) return -1;
        if (got == 0) {
            if (win->want_redraw) repaint(win);
            /* Self-pace: sleep instead of busy-yield. A tight yield loop with
             * the whole-iteration BKL hold in pid 0's idle_loop can livelock
             * the desktop under SMP (see user_login.c). ~15ms is imperceptible. */
            g_sleep_ms(15);
            continue;
        }
        dispatch(win, &ev);
        if (win->want_redraw) { win->dirty_full = 1; repaint(win); }
    }
}
