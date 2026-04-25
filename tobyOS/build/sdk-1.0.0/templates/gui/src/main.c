/* GUI app template -- single window with a label + Quit button.
 * Replace the widgets / callbacks with your own.
 */

#include <stdio.h>
#include <toby/gui.h>

static void on_quit(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    tg_app_quit(app);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, 320, 160, "My App") != 0) return 1;

    tg_label (&app,  16,  20, 288, 24, "Edit me!");
    tg_button(&app, 232, 110,  72, 28, "Quit", on_quit);

    return tg_run(&app);
}
