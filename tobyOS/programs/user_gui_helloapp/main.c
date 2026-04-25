/* user_gui_helloapp/main.c -- demo GUI app shipped via the package manager.
 *
 * Milestone 16's "how do I know installation works?" sample. This
 * program is NOT bundled into /bin/ like every other GUI app -- it
 * ships inside initrd/repo/helloapp.tpkg and only ends up on the
 * system after the user types:
 *
 *     pkg install helloapp
 *
 * Which extracts the ELF to /data/apps/helloapp.elf, writes a .app
 * descriptor, and registers a launcher entry. The app is otherwise a
 * perfectly ordinary toby_gui-based window, reusing the same toolkit
 * as gui_about / gui_widgets. */

#include "../common/toby_gui.h"

#define WIN_W 320
#define WIN_H 160

static struct tg_widget *g_click_label;
static int               g_clicks;

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

static void on_click(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    g_clicks++;
    char buf[32];
    char num[12];
    u_to_dec(num, sizeof(num), (unsigned)g_clicks);
    const char *prefix = "clicks: ";
    unsigned i = 0;
    while (prefix[i] && i + 1 < sizeof(buf)) { buf[i] = prefix[i]; i++; }
    unsigned j = 0;
    while (num[j] && i + 1 < sizeof(buf))   { buf[i++] = num[j++]; }
    buf[i] = '\0';
    tg_set_text(app, g_click_label, buf);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, WIN_W, WIN_H, "Hello Pkg") != 0) {
        tg_puts("helloapp: tg_app_init failed\n");
        return 1;
    }

    tg_label(&app, 12,  10, 296, 16, "Hello from a packaged app!");
    tg_label(&app, 12,  32, 296, 14,
             "Installed via `pkg install helloapp`.");
    tg_label(&app, 12,  50, 296, 14,
             "Lives in /data/apps/helloapp.elf on disk.");

    tg_button(&app, 12, 86, 100, 28, "Click me", on_click);
    g_click_label = tg_label(&app, 124, 92, 180, 16, "clicks: 0");

    tg_label(&app, 12, 132, 296, 14,
             "Remove with `pkg remove helloapp`.");

    return tg_run(&app);
}
