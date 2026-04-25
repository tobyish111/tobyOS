/* hello_gui/src/main.c -- minimal GUI sample for the tobyOS SDK 1.0.
 *
 * Opens a 320x140 window, drops a label and a Quit button, runs the
 * toolkit event loop. Pops a notification toast on first paint so
 * the user can see the M31 notify pipeline triggered from a regular
 * SDK app (not the kernel).
 *
 * Build (out-of-tree):
 *     export TOBYOS_SDK=/path/to/sdk
 *     make
 */

#include <stdio.h>
#include <toby/gui.h>
#include <tobyos_notify.h>

static void on_quit_clicked(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    tg_puts("[hello_gui] Quit clicked\n");
    tg_app_quit(app);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, 320, 140, "Hello SDK") != 0) {
        fprintf(stderr, "[hello_gui] tg_app_init failed -- no GUI?\n");
        return 1;
    }

    tg_label (&app,  16,  20, 288, 24, "Hello from the tobyOS SDK!");
    tg_label (&app,  16,  48, 288, 18, "Built out-of-tree against SDK 1.0.");
    tg_button(&app, 232,  90,  72, 28, "Quit", on_quit_clicked);

    /* Friendly heads-up -- the toast will appear in the bottom-right
     * corner above the taskbar. Failure is non-fatal: an SDK app
     * might run on a kernel without M31 wired in. */
    if (toby_notify_post(TOBY_NOTIFY_INFO,
                         "hello_gui",
                         "SDK demo running") < 0) {
        fprintf(stderr, "[hello_gui] notify_post: not available, continuing\n");
    }

    return tg_run(&app);
}
