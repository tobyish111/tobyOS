/* user_gui_helloapp/main.c -- demo GUI app shipped via the package manager.
 *
 * Milestone 16's "how do I know installation works?" sample. This program is
 * NOT bundled into /bin/ like the other GUI apps -- it ships inside
 * initrd/repo/helloapp.tpkg and only lands on the system after:
 *
 *     pkg install helloapp
 *
 * Migrated to the TobyTK toolkit (toby/tk.h) as part of the unify-on-TobyTK
 * effort -- it no longer uses the old programs/common/toby_gui.c toolkit.
 */

#include <toby/tk.h>

static struct tk_window win;
static struct tk_widget *g_click_label;
static int g_clicks;

static unsigned u_to_dec(char *out, unsigned cap, unsigned v) {
    char tmp[16];
    unsigned n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    unsigned i = 0;
    while (n && i + 1 < cap) out[i++] = tmp[--n];
    out[i] = '\0';
    return i;
}

static void on_click(struct tk_window *w, struct tk_widget *btn) {
    (void)btn;
    g_clicks++;
    char buf[32]; char num[12];
    u_to_dec(num, sizeof(num), (unsigned)g_clicks);
    const char *prefix = "clicks: ";
    unsigned i = 0;
    while (prefix[i] && i + 1 < sizeof(buf)) { buf[i] = prefix[i]; i++; }
    unsigned j = 0;
    while (num[j] && i + 1 < sizeof(buf)) buf[i++] = num[j++];
    buf[i] = '\0';
    tk_set_text(w, g_click_label, buf);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 340, 200, "Hello Pkg") != 0)
        return 1;

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 14);
    root->gap = 6;

    tk_bold(tk_label(&win, root, "Hello from a packaged app!"));
    tk_colors(tk_label(&win, root, "Installed via `pkg install helloapp`."), 0, 0x008A93A0);
    tk_colors(tk_label(&win, root, "Lives in /data/apps/helloapp.elf on disk."), 0, 0x008A93A0);
    tk_separator(&win, root);

    struct tk_widget *row = tk_hbox(&win, root, 10);
    tk_button(&win, row, "Click me", on_click);
    g_click_label = tk_label(&win, row, "clicks: 0");

    tk_grow(tk_panel(&win, root), 1);   /* spacer */
    tk_colors(tk_label(&win, root, "Remove with `pkg remove helloapp`."), 0, 0x008A93A0);

    return tk_run(&win);
}
