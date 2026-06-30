/* GUI app template -- single window with a label + Quit button, on TobyTK.
 * Replace the widgets / callbacks with your own.
 */

#include <stdio.h>
#include <toby/tk.h>

/* tk_window embeds the widget pool (~150 KB) -- keep it static, not on the stack. */
static struct tk_window win;

static void on_quit(struct tk_window *w, struct tk_widget *btn) {
    (void)btn;
    tk_quit(w);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 320, 160, "My App") != 0) return 1;

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 16);
    root->gap = 8;

    tk_label(&win, root, "Edit me!");
    struct tk_widget *row = tk_hbox(&win, root, 8);
    tk_grow(tk_label(&win, row, ""), 1);
    tk_button(&win, row, "Quit", on_quit);

    return tk_run(&win);
}
