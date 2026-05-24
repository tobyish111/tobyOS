/* user_gui_calc/main.c -- Calculator for tobyOS.
 *
 * 280x380 window, 6-row x 4-col button grid. Fixed-point arithmetic
 * (4 decimal places, SCALE=10000) for freestanding environment.
 * Supports +, -, *, /, %, sqrt, negate, decimal, and memory.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

#define GUI_EV_MOUSE_DOWN 2
#define GUI_EV_KEY        4
#define GUI_EV_CLOSE      5

/* ── syscall wrappers ─────────────────────────────────────────────── */

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}

static inline void sys_yield(void) {
    long _dummy;
    __asm__ volatile("syscall"
        : "=a"(_dummy) : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
}

static inline int sys_gui_create(uint32_t w, uint32_t h, const char *title) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(title)
        : "rcx", "r11", "memory");
    return (int)r;
}

static inline int sys_gui_fill(int fd, int x, int y, int w, int h,
                               uint32_t color) {
    long r;
    uint32_t whlen = ((uint32_t)(uint16_t)w) |
                     (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)whlen;
    register long r8  __asm__("r8")  = (long)color;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL), "D"((long)fd),
          "S"((long)x), "d"((long)y),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}

static inline int sys_gui_text(int fd, int x, int y, const char *s,
                               uint32_t fg, uint32_t bg) {
    long r;
    uint32_t xy = ((uint32_t)(uint16_t)x) | (((uint32_t)(uint16_t)y) << 16);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)bg;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd),
          "S"((long)xy), "d"(s),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}

static inline int sys_gui_flip(int fd) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FLIP), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}

static inline int sys_gui_poll_event(int fd, struct gui_event *ev) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_POLL_EVENT), "D"((long)fd), "S"(ev)
        : "rcx", "r11", "memory");
    return (int)r;
}

/* ── layout constants ─────────────────────────────────────────────── */

#define WIN_W 280
#define WIN_H 380

#define COL_BG      0x001E1E2Eu
#define COL_BTN     0x00313244u
#define COL_DISPLAY 0x00181825u
#define COL_TEXT    0x00CDD6F4u
#define COL_OP      0x00F38BA8u
#define COL_EQ      0x00A6E3A1u
#define COL_MEM     0x00585B70u
#define COL_DIM     0x00585B70u
#define COL_BORDER  0x0045475Au

#define MARGIN   8
#define GAP      4
#define DISP_H   70
#define GRID_TOP (MARGIN + DISP_H + GAP)
#define ROWS     6
#define COLS     4

#define BTN_W ((WIN_W - 2 * MARGIN - (COLS - 1) * GAP) / COLS)
#define BTN_H ((WIN_H - GRID_TOP - MARGIN - (ROWS - 1) * GAP) / ROWS)

#define SCALE 10000LL

/* ── button identifiers (row-major order) ─────────────────────────── */

enum {
    B_MC, B_MR, B_MADD, B_MSUB,
    B_C,  B_NEG, B_PCT, B_DIV,
    B_7,  B_8,   B_9,   B_MUL,
    B_4,  B_5,   B_6,   B_SUB,
    B_1,  B_2,   B_3,   B_ADD,
    B_0,  B_DOT, B_SQRT, B_EQ,
    B_COUNT
};

static const char *btn_labels[B_COUNT] = {
    "MC", "MR", "M+", "M-",
    "C",  "+/-", "%",  "/",
    "7",  "8",   "9",  "*",
    "4",  "5",   "6",  "-",
    "1",  "2",   "3",  "+",
    "0",  ".",   "sq", "=",
};

/* ── utility ──────────────────────────────────────────────────────── */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static void fixed_to_str(char *out, int cap, int64_t val) {
    if (cap <= 0) return;
    int pos = 0;

    if (val < 0) {
        if (pos < cap - 1) out[pos++] = '-';
        val = -val;
    }

    int64_t ipart = val / SCALE;
    int64_t frac  = val % SCALE;

    char tmp[20];
    int n = 0;
    if (ipart == 0)
        tmp[n++] = '0';
    else
        while (ipart > 0) { tmp[n++] = '0' + (int)(ipart % 10); ipart /= 10; }
    while (n-- > 0 && pos < cap - 1)
        out[pos++] = tmp[n];

    if (frac > 0 && pos < cap - 6) {
        out[pos++] = '.';
        char fd[4];
        fd[0] = '0' + (int)((frac / 1000) % 10);
        fd[1] = '0' + (int)((frac / 100) % 10);
        fd[2] = '0' + (int)((frac / 10) % 10);
        fd[3] = '0' + (int)(frac % 10);
        int last = 3;
        while (last > 0 && fd[last] == '0') last--;
        for (int i = 0; i <= last && pos < cap - 1; i++)
            out[pos++] = fd[i];
    }

    out[pos] = '\0';
}

static int64_t isqrt64(int64_t n) {
    if (n <= 0) return 0;
    int64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

/* ── calculator state ─────────────────────────────────────────────── */

static int64_t current;
static int64_t accum;
static int64_t memory;
static char    pending_op;
static int     new_entry;
static int     has_accum;
static int     decimal_mode;
static int     decimal_places;
static char    display[32];
static char    expr_line[48];

static void update_display(void) {
    fixed_to_str(display, (int)sizeof(display), current);
    if (decimal_mode && decimal_places == 0) {
        int len = (int)my_strlen(display);
        if (len < (int)sizeof(display) - 2) {
            display[len]     = '.';
            display[len + 1] = '\0';
        }
    }
}

static void reset_all(void) {
    current = 0;  accum = 0;
    pending_op = 0;  new_entry = 1;  has_accum = 0;
    decimal_mode = 0;  decimal_places = 0;
    display[0] = '0';  display[1] = '\0';
    expr_line[0] = '\0';
}

/* fixed-point ops: a and b are scaled by SCALE */
static int64_t apply_op(int64_t a, int64_t b, char op) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return (a / SCALE) * b + ((a % SCALE) * b) / SCALE;
    case '/': return b != 0
                  ? (a / b) * SCALE + ((a % b) * SCALE) / b
                  : 0;
    default:  return b;
    }
}

/* ── input actions ────────────────────────────────────────────────── */

static void push_digit(int d) {
    if (new_entry) {
        current = (int64_t)d * SCALE;
        new_entry = 0;
        decimal_mode = 0;
        decimal_places = 0;
    } else if (decimal_mode) {
        if (decimal_places >= 4) return;
        decimal_places++;
        int64_t mult = SCALE;
        for (int i = 0; i < decimal_places; i++) mult /= 10;
        if (current < 0)
            current -= (int64_t)d * mult;
        else
            current += (int64_t)d * mult;
    } else {
        int64_t ip = current / SCALE;
        int64_t next = ip * 10 + (ip >= 0 ? d : -d);
        if (next > 999999999LL || next < -999999999LL) return;
        current = next * SCALE;
    }
    update_display();
}

static void push_dot(void) {
    if (decimal_mode) return;
    if (new_entry) {
        current = 0;
        new_entry = 0;
    }
    decimal_mode = 1;
    decimal_places = 0;
    update_display();
}

static void build_expr(int64_t val, char op) {
    fixed_to_str(expr_line, (int)sizeof(expr_line) - 3, val);
    int len = (int)my_strlen(expr_line);
    expr_line[len++] = ' ';
    expr_line[len++] = op;
    expr_line[len]   = '\0';
}

static void push_op(char op) {
    if (has_accum && !new_entry) {
        accum = apply_op(accum, current, pending_op);
        current = accum;
        update_display();
    } else {
        accum = current;
        has_accum = 1;
    }
    pending_op = op;
    new_entry = 1;
    decimal_mode = 0;
    decimal_places = 0;
    build_expr(accum, op);
}

static void push_equals(void) {
    if (has_accum) {
        accum = apply_op(accum, current, pending_op);
        current = accum;
        update_display();
    }
    pending_op = 0;
    has_accum = 0;
    new_entry = 1;
    decimal_mode = 0;
    decimal_places = 0;
    expr_line[0] = '\0';
}

static void push_negate(void) {
    current = -current;
    update_display();
}

static void push_percent(void) {
    current = current / 100;
    new_entry = 1;
    decimal_mode = 0;
    decimal_places = 0;
    update_display();
}

static void push_sqrt_fn(void) {
    if (current < 0) return;
    current = isqrt64(current * SCALE);
    new_entry = 1;
    decimal_mode = 0;
    decimal_places = 0;
    update_display();
}

/* ── grid helpers ─────────────────────────────────────────────────── */

static int btn_x(int col) { return MARGIN + col * (BTN_W + GAP); }
static int btn_y(int row) { return GRID_TOP + row * (BTN_H + GAP); }

static int hit_test(int mx, int my) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int bx = btn_x(c), by = btn_y(r);
            if (mx >= bx && mx < bx + BTN_W &&
                my >= by && my < by + BTN_H)
                return r * COLS + c;
        }
    return -1;
}

static void btn_colors(int id, uint32_t *bg, uint32_t *fg) {
    *bg = COL_BTN;
    *fg = COL_TEXT;

    if (id <= B_MSUB)
        { *bg = COL_MEM; }
    else if (id == B_DIV || id == B_MUL || id == B_SUB || id == B_ADD)
        { *bg = COL_OP; *fg = COL_BG; }
    else if (id == B_EQ)
        { *bg = COL_EQ; *fg = COL_BG; }
    else if (id == B_C)
        { *fg = COL_OP; }
}

/* ── drawing ──────────────────────────────────────────────────────── */

static void draw(int fd) {
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);

    int dx = MARGIN, dy = MARGIN, dw = WIN_W - 2 * MARGIN;
    sys_gui_fill(fd, dx, dy, dw, DISP_H, COL_DISPLAY);
    sys_gui_fill(fd, dx, dy, dw, 1, COL_BORDER);
    sys_gui_fill(fd, dx, dy + DISP_H - 1, dw, 1, COL_BORDER);

    if (expr_line[0])
        sys_gui_text(fd, dx + 8, dy + 6, expr_line, COL_DIM, COL_DISPLAY);

    if (memory != 0)
        sys_gui_text(fd, dx + 8, dy + 22, "M", COL_DIM, COL_DISPLAY);

    int dlen = (int)my_strlen(display);
    int tx = dx + dw - 8 - dlen * 8;
    if (tx < dx + 8) tx = dx + 8;
    sys_gui_text(fd, tx, dy + 42, display, COL_TEXT, COL_DISPLAY);

    for (int i = 0; i < B_COUNT; i++) {
        int r = i / COLS, c = i % COLS;
        int bx = btn_x(c), by = btn_y(r);
        uint32_t bg, fg;
        btn_colors(i, &bg, &fg);
        sys_gui_fill(fd, bx, by, BTN_W, BTN_H, bg);

        const char *lbl = btn_labels[i];
        int llen = (int)my_strlen(lbl);
        int ltx = bx + BTN_W / 2 - llen * 4;
        int lty = by + BTN_H / 2 - 6;
        sys_gui_text(fd, ltx, lty, lbl, fg, bg);
    }

    sys_gui_flip(fd);
}

/* ── input dispatch ───────────────────────────────────────────────── */

static int digit_for_btn(int id) {
    if (id == B_0) return 0;
    if (id >= B_7 && id <= B_9) return 7 + (id - B_7);
    if (id >= B_4 && id <= B_6) return 4 + (id - B_4);
    if (id >= B_1 && id <= B_3) return 1 + (id - B_1);
    return -1;
}

static void handle_btn(int id) {
    int d = digit_for_btn(id);
    if (d >= 0) { push_digit(d); return; }

    switch (id) {
    case B_DOT:  push_dot();       break;
    case B_ADD:  push_op('+');     break;
    case B_SUB:  push_op('-');     break;
    case B_MUL:  push_op('*');     break;
    case B_DIV:  push_op('/');     break;
    case B_EQ:   push_equals();    break;
    case B_C:    reset_all();      break;
    case B_NEG:  push_negate();    break;
    case B_PCT:  push_percent();   break;
    case B_SQRT: push_sqrt_fn();   break;
    case B_MC:   memory = 0;       break;
    case B_MR:
        current = memory;
        new_entry = 1;
        decimal_mode = 0;
        decimal_places = 0;
        update_display();
        break;
    case B_MADD: memory += current; break;
    case B_MSUB: memory -= current; break;
    }
}

static void handle_key(uint8_t key) {
    if (key >= '0' && key <= '9')                    push_digit(key - '0');
    else if (key == '.')                             push_dot();
    else if (key == '+')                             push_op('+');
    else if (key == '-')                             push_op('-');
    else if (key == '*')                             push_op('*');
    else if (key == '/')                             push_op('/');
    else if (key == '%')                             push_percent();
    else if (key == '=' || key == '\n' || key == '\r') push_equals();
    else if (key == 'c' || key == 'C')               reset_all();
    else if (key == 8 || key == 127) {
        if (!new_entry) {
            if (decimal_mode && decimal_places > 0) {
                int64_t mult = SCALE;
                for (int i = 0; i < decimal_places; i++) mult /= 10;
                int64_t last_digit;
                if (current >= 0)
                    last_digit = (current / mult) % 10;
                else
                    last_digit = ((-current) / mult) % 10;
                if (current >= 0)
                    current -= last_digit * mult;
                else
                    current += last_digit * mult;
                decimal_places--;
                if (decimal_places == 0 && (current % SCALE) == 0)
                    decimal_mode = 0;
            } else if (!decimal_mode) {
                int64_t ip = current / SCALE;
                ip /= 10;
                current = ip * SCALE;
                if (ip == 0) new_entry = 1;
            }
            update_display();
        }
    }
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = sys_gui_create(WIN_W, WIN_H, "Calculator");
    if (fd < 0) {
        const char *msg = "gui_calc: sys_gui_create failed\n";
        sys_write(1, msg, my_strlen(msg));
        return 1;
    }

    reset_all();
    draw(fd);

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got <= 0) { sys_yield(); continue; }

        if (ev.type == GUI_EV_CLOSE)
            return 0;

        int dirty = 0;

        if (ev.type == GUI_EV_KEY) {
            if (ev.key == 'q' || ev.key == 27) return 0;
            handle_key(ev.key);
            dirty = 1;
        }

        if (ev.type == GUI_EV_MOUSE_DOWN) {
            int id = hit_test(ev.x, ev.y);
            if (id >= 0) { handle_btn(id); dirty = 1; }
        }

        if (dirty) draw(fd);
    }
}
