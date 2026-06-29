/* user_gui_demo/main.c -- TobyTK widget gallery (/bin/gui_demo).
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). Exercises every widget the
 * toolkit provides -- the basic set (label/button/field/checkbox/listbox/
 * slider/progress/separator) plus the additions made for the unify-on-TobyTK
 * effort: TK_TABLE, TK_TEXTAREA and the TK_CANVAS custom-draw surface. It
 * doubles as the toolkit's live regression demo: if a widget renders or
 * behaves wrong, it shows here first.
 */

#include <toby/tk.h>

static struct tk_window win;
static struct tk_widget *w_status;
static struct tk_widget *w_progress;

/* ---- listbox data ---------------------------------------------------- */
static const char *list_items[] = {
    "Documents", "Downloads", "Pictures", "Music", "Videos", "Projects",
};

/* ---- table data (cell accessor; no per-row widgets) ------------------ */
static const char *const table_headers[] = { "Name", "Type", "Size" };
static const int    table_widths[]  = { 120, 90, 70 };
static const char  *table_cells[][3] = {
    { "kernel.c",   "C source", "210 KB" },
    { "tk.c",       "C source", "36 KB"  },
    { "Lato.ttf",   "Font",     "75 KB"  },
    { "boot.log",   "Log",      "20 KB"  },
    { "disk.img",   "Image",    "16 MB"  },
    { "readme.txt", "Text",     "2 KB"   },
};
static const char *table_cell(void *user, int row, int col) {
    (void)user;
    if (row < 0 || row >= (int)(sizeof(table_cells) / sizeof(table_cells[0]))) return "";
    if (col < 0 || col > 2) return "";
    return table_cells[row][col];
}

static const char *area_text =
    "TobyTK textarea\n"
    "----------------\n"
    "Read-only, scrollable,\n"
    "antialiased TrueType.\n"
    "Use Up / Down when focused.\n"
    "\n"
    "Editors layer their own\n"
    "cursor logic over a canvas.\n"
    "Line 9\n"
    "Line 10\n"
    "Line 11 (scroll to see me)\n";

/* ---- callbacks ------------------------------------------------------- */
static void on_ok(struct tk_window *w, struct tk_widget *b) {
    (void)b; tk_set_text(w, w_status, "OK clicked");
}
static void on_cancel(struct tk_window *w, struct tk_widget *b) {
    (void)b; tk_set_text(w, w_status, "Cancel clicked");
}
static void on_slide(struct tk_window *w, struct tk_widget *s) {
    tk_set_value(w, w_progress, s->value);
    char msg[40];
    int v = s->value, p = 0;
    const char *pre = "Slider = ";
    for (int i = 0; pre[i]; i++) msg[p++] = pre[i];
    if (v >= 100) { msg[p++] = '1'; msg[p++] = '0'; msg[p++] = '0'; }
    else { if (v >= 10) msg[p++] = '0' + v / 10; msg[p++] = '0' + v % 10; }
    msg[p] = '\0';
    tk_set_text(w, w_status, msg);
}
static void on_list(struct tk_window *w, struct tk_widget *l) {
    int i = tk_selected(l);
    if (i >= 0 && i < (int)(sizeof(list_items) / sizeof(list_items[0])))
        tk_set_text(w, w_status, list_items[i]);
}

/* ---- canvas: prove the tk_draw_* primitive API ----------------------- */
static void paint_canvas(struct tk_window *w, struct tk_widget *c) {
    int x, y, cw, ch;
    tk_geom(c, &x, &y, &cw, &ch);
    tk_draw_rrect(w, x, y, cw, ch, 8, 0x00141A24);
    uint32_t sw[5] = { 0x00E06C75, 0x0098C379, 0x00E5C07B, 0x0061AFEF, 0x00C678DD };
    for (int i = 0; i < 5; i++)
        tk_draw_fill(w, x + 10 + i * 26, y + 10, 22, 22, sw[i]);
    int cx = x + cw - 40, cy = y + 30;
    tk_draw_circle(w, cx, cy, 18, 0x0061AFEF);
    tk_draw_circle_outline(w, cx, cy, 18, 0x00FFFFFF);
    tk_draw_line(w, x + 10, y + 50, x + cw - 10, y + 50, 0x00343A45);
    tk_draw_text(w, x + 10, y + ch - 22, "tk_draw_* canvas", 0x00C8D0DA, 14, 0);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 780, 560, "TobyTK Widget Gallery") != 0)
        return 1;

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 12);
    root->gap = 8;

    tk_bold(tk_font(tk_label(&win, root, "TobyTK Widget Gallery"), 20));
    tk_separator(&win, root);

    struct tk_widget *cols = tk_hbox(&win, root, 16);
    tk_grow(cols, 1);

    /* ---- left column: basic widgets ---- */
    struct tk_widget *left = tk_vbox(&win, cols, 8);
    tk_grow(left, 1);
    tk_label(&win, left, "Buttons");
    struct tk_widget *brow = tk_hbox(&win, left, 8);
    tk_button(&win, brow, "OK", on_ok);
    tk_button(&win, brow, "Cancel", on_cancel);
    tk_label(&win, left, "Text field");
    tk_field(&win, left, "edit me");
    tk_checkbox(&win, left, "Enable feature", 1);
    tk_label(&win, left, "Slider + progress");
    {
        struct tk_widget *s = tk_slider(&win, left, 0, 100, 60);
        s->on_change = on_slide;
    }
    w_progress = tk_progress(&win, left, 60, 100);
    tk_label(&win, left, "Listbox");
    {
        struct tk_widget *lb = tk_listbox(&win, left,
            list_items, (int)(sizeof(list_items) / sizeof(list_items[0])));
        lb->on_change = on_list;
        tk_grow(lb, 1);
    }

    /* ---- right column: new widgets ---- */
    struct tk_widget *right = tk_vbox(&win, cols, 8);
    tk_grow(right, 1);
    tk_label(&win, right, "Table");
    {
        struct tk_widget *tbl = tk_table(&win, right, table_headers, table_widths, 3);
        tk_table_rows(&win, tbl,
            (int)(sizeof(table_cells) / sizeof(table_cells[0])), table_cell, 0);
        tk_grow(tbl, 1);
    }
    tk_label(&win, right, "Textarea");
    tk_grow(tk_textarea(&win, right, area_text), 1);
    tk_label(&win, right, "Canvas");
    tk_size(tk_canvas(&win, right, paint_canvas), 0, 96);

    tk_separator(&win, root);
    w_status = tk_colors(tk_label(&win, root, "Ready"), 0, 0x0080C0FF);

    return tk_run(&win);
}
