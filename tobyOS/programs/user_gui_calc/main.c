/* user_gui_calc/main.c -- Calculator for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h): a vbox display panel above a
 * 6x4 grid of buttons laid out by the toolkit's flex engine. The
 * fixed-point arithmetic + calculator state machine below is unchanged from
 * the original raw-gui_* version -- only the rendering/input layer moved to
 * TobyTK so the calculator shares the system theme and widget behaviour.
 *
 * Fixed-point (4 decimal places, SCALE=10000). Supports + - * / %, sqrt,
 * negate, decimal, and memory. Keyboard via the window-level key hook.
 */

#include <toby/tk.h>

typedef unsigned long long uint64_t_;
typedef long long          int64_t_;

/* ── utility ──────────────────────────────────────────────────────── */

static unsigned long my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

#define SCALE 10000LL

static void fixed_to_str(char *out, int cap, long long val) {
    if (cap <= 0) return;
    int pos = 0;

    if (val < 0) {
        if (pos < cap - 1) out[pos++] = '-';
        val = -val;
    }

    long long ipart = val / SCALE;
    long long frac  = val % SCALE;

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

static long long isqrt64(long long n) {
    if (n <= 0) return 0;
    long long x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

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

/* Calculator palette (kept from the original for visual identity). */
#define COL_DISPLAY 0x00181825u
#define COL_TEXT    0x00CDD6F4u
#define COL_OP      0x00F38BA8u
#define COL_EQ      0x00A6E3A1u
#define COL_MEM     0x00585B70u
#define COL_DIM     0x00A6ADC8u
#define COL_DARK    0x001E1E2Eu

/* ── calculator state ─────────────────────────────────────────────── */

static long long current;
static long long accum;
static long long memory;
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
static long long apply_op(long long a, long long b, char op) {
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
        current = (long long)d * SCALE;
        new_entry = 0;
        decimal_mode = 0;
        decimal_places = 0;
    } else if (decimal_mode) {
        if (decimal_places >= 4) return;
        decimal_places++;
        long long mult = SCALE;
        for (int i = 0; i < decimal_places; i++) mult /= 10;
        if (current < 0)
            current -= (long long)d * mult;
        else
            current += (long long)d * mult;
    } else {
        long long ip = current / SCALE;
        long long next = ip * 10 + (ip >= 0 ? d : -d);
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

static void build_expr(long long val, char op) {
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

static void handle_key(unsigned char key) {
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
                long long mult = SCALE;
                for (int i = 0; i < decimal_places; i++) mult /= 10;
                long long last_digit;
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
                long long ip = current / SCALE;
                ip /= 10;
                current = ip * SCALE;
                if (ip == 0) new_entry = 1;
            }
            update_display();
        }
    }
}

/* ── TobyTK UI ────────────────────────────────────────────────────── */

static struct tk_window win;
static struct tk_widget *w_expr;     /* small dim expression line   */
static struct tk_widget *w_disp;     /* large current-value display */

static void refresh(struct tk_window *w) {
    char ex[56];
    int p = 0;
    if (memory != 0) { ex[p++] = 'M'; ex[p++] = ' '; }
    for (int i = 0; expr_line[i] && p < (int)sizeof(ex) - 1; i++) ex[p++] = expr_line[i];
    ex[p] = '\0';
    tk_set_text(w, w_expr, ex);
    tk_set_text(w, w_disp, display);
}

static void on_btn(struct tk_window *w, struct tk_widget *b) {
    handle_btn((int)(long)b->user);
    refresh(w);
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (ev->key == 27 || ev->key == 'q') { tk_quit(w); return; }
    handle_key(ev->key);
    refresh(w);
}

static void make_button(struct tk_widget *row, int id) {
    struct tk_widget *b = tk_button(&win, row, btn_labels[id], on_btn);
    if (!b) return;
    b->user = (void *)(long)id;
    tk_grow(b, 1);
    tk_font(b, 17);
    /* per-type colours (kept from the original identity) */
    if (id <= B_MSUB)
        tk_colors(b, COL_MEM, COL_TEXT);
    else if (id == B_DIV || id == B_MUL || id == B_SUB || id == B_ADD)
        tk_colors(b, COL_OP, COL_DARK);
    else if (id == B_EQ)
        tk_colors(b, COL_EQ, COL_DARK);
    else if (id == B_C)
        tk_colors(b, 0, COL_OP);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 280, 380, "Calculator") != 0)
        return 1;

    reset_all();
    tk_on_key(&win, on_key);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 8);
    root->gap = 8;

    /* display panel */
    struct tk_widget *disp = tk_vbox(&win, root, 2);
    tk_size(disp, 0, 74);
    tk_pad(disp, 8);
    tk_colors(disp, COL_DISPLAY, 0);
    w_expr = tk_align(tk_font(tk_colors(tk_label(&win, disp, ""), 0, COL_DIM), 13), TK_ALIGN_RIGHT);
    w_disp = tk_align(tk_grow(tk_font(tk_colors(tk_label(&win, disp, "0"), 0, COL_TEXT), 30), 1), TK_ALIGN_RIGHT);

    /* 6x4 button grid */
    struct tk_widget *grid = tk_vbox(&win, root, 6);
    tk_grow(grid, 1);
    for (int r = 0; r < 6; r++) {
        struct tk_widget *row = tk_hbox(&win, grid, 6);
        tk_grow(row, 1);
        for (int c = 0; c < 4; c++)
            make_button(row, r * 4 + c);
    }

    refresh(&win);
    return tk_run(&win);
}
