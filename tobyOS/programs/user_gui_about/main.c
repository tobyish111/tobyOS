/* user_gui_about/main.c -- /bin/gui_about, the second milestone-12 GUI app.
 *
 * A small "About system" window. Two purposes:
 *
 *   1. Give the desktop launcher a SECOND program to spawn so we can
 *      demonstrate multiple windows + the taskbar tab switcher.
 *   2. Show that the toby_gui toolkit composes nicely for read-only
 *      content too -- this app has no text input, just labels and one
 *      button that increments a counter (so you can see the redraw
 *      pipeline reacting to clicks).
 *
 * Layout (320 x 200 client area):
 *
 *   +------------------------------------------+
 *   |  About tobyOS                            |
 *   |                                          |
 *   |  milestone : 12 (desktop environment)    |
 *   |  features  : taskbar, launcher, drag,    |
 *   |              focus, close button         |
 *   |  toolkit   : toby_gui (label/button/text)|
 *   |                                          |
 *   |  +-----------+   counter: 0              |
 *   |  | Click me  |                           |
 *   |  +-----------+                           |
 *   |                                          |
 *   |  Drag by title bar. Click X to close.    |
 *   +------------------------------------------+
 */

#include "../common/toby_gui.h"

#define WIN_W 360
#define WIN_H 220

static struct tg_widget *g_counter_label;
static int               g_count;

/* utoa for small unsigned ints (no libc available). Writes into out
 * up to cap-1 chars + NUL. Returns the number of chars written. */
static unsigned u_to_dec(char *out, unsigned cap, unsigned v) {
    char tmp[16];
    unsigned n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    unsigned i = 0;
    while (n && i + 1 < cap) { out[i++] = tmp[--n]; }
    out[i] = '\0';
    return i;
}

static void on_click_me(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    g_count++;
    char buf[32];
    char num[16];
    u_to_dec(num, sizeof(num), (unsigned)g_count);
    /* Build "counter: N" without snprintf. */
    const char *prefix = "counter: ";
    unsigned i = 0;
    while (prefix[i] && i + 1 < sizeof(buf)) { buf[i] = prefix[i]; i++; }
    unsigned j = 0;
    while (num[j] && i + 1 < sizeof(buf))   { buf[i++] = num[j++]; }
    buf[i] = '\0';
    tg_set_text(app, g_counter_label, buf);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, WIN_W, WIN_H, "about tobyOS") != 0) {
        tg_puts("gui_about: tg_app_init failed\n");
        return 1;
    }

    tg_label(&app, 12,   8, 336, 16, "About tobyOS");
    tg_label(&app, 12,  32, 336, 14, "milestone : 12 (desktop environment)");
    tg_label(&app, 12,  50, 336, 14, "features  : taskbar, launcher, drag, focus, close X");
    tg_label(&app, 12,  68, 336, 14, "toolkit   : toby_gui (label / button / textinput)");
    tg_label(&app, 12,  86, 336, 14, "kernel    : ring-3 syscalls + per-proc PML4 + SMP");

    tg_button(&app, 12, 116, 100, 28, "Click me", on_click_me);
    g_counter_label = tg_label(&app, 124, 122, 200, 16, "counter: 0");

    tg_label(&app, 12, 168, 336, 14,
             "Drag by the title bar. Click X (top-right) to close.");
    tg_label(&app, 12, 186, 336, 14,
             "Open the Apps menu in the taskbar to launch more windows.");

    return tg_run(&app);
}
