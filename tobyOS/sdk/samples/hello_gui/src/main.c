/* hello_gui/src/main.c -- minimal GUI sample for the tobyOS SDK.
 *
 * Built on TobyTK (<toby/tk.h>), the canonical tobyOS widget toolkit. Opens a
 * small window with two labels and a Quit button laid out by the toolkit's
 * vbox/hbox engine, then runs the retained-mode event loop. Pops a notification
 * toast so the user can see the notify pipeline triggered from a regular SDK
 * app (not the kernel).
 *
 * Build (out-of-tree):
 *     export TOBYOS_SDK=/path/to/sdk
 *     make
 */

#include <stdio.h>
#include <toby/tk.h>
#include <tobyos_notify.h>

/* tk_window embeds the whole widget pool (~150 KB) -- declare it static/global,
 * never on the stack. */
static struct tk_window win;

static void on_quit(struct tk_window *w, struct tk_widget *btn) {
    (void)btn;
    printf("[hello_gui] Quit clicked\n");
    tk_quit(w);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 320, 160, "Hello SDK") != 0) {
        fprintf(stderr, "[hello_gui] tk_window_open failed -- no GUI?\n");
        return 1;
    }

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 16);
    root->gap = 8;
    tk_label(&win, root, "Hello from the tobyOS SDK!");
    tk_label(&win, root, "Built out-of-tree on TobyTK.");

    struct tk_widget *row = tk_hbox(&win, root, 8);
    tk_grow(tk_label(&win, row, ""), 1);           /* spacer pushes the button right */
    tk_button(&win, row, "Quit", on_quit);

    /* Friendly heads-up -- toast appears bottom-right above the taskbar.
     * Failure is non-fatal (an SDK app might run on a kernel without it). */
    if (toby_notify_post(TOBY_NOTIFY_INFO, "hello_gui", "SDK demo running") < 0)
        fprintf(stderr, "[hello_gui] notify_post: not available, continuing\n");

    return tk_run(&win);
}
