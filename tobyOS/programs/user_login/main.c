/* user_login/main.c -- Full-screen GUI login screen for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The session model is unchanged
 * from the original raw-gui_* version: a centred sign-in panel over a dark
 * background, the username prefilled from the "user.last" setting, ABI login on
 * Enter / Sign In, and a smooth opacity fade-out on success that reveals the
 * desktop beneath. As the session gateway the window must NOT be dismissable,
 * so it drives its own loop (tk_pump) and swallows the close request rather
 * than letting the toolkit quit; the only exit is a successful sign-in.
 */

#include <toby/tk.h>
#include <unistd.h>

#define SYS_SETTING_GET     21
#define SYS_LOGIN           23
#define SYS_GUI_SET_OPACITY 91
#define SYS_GUI_FLIP        13

static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static long sys_setting_get(const char *k, char *o, long cap){ return sc3(SYS_SETTING_GET,(long)k,(long)o,cap); }
static long sys_login(const char *u){ return sc2(SYS_LOGIN,(long)u,0); }

#define COL_PANEL  0x00162030u
#define COL_ACCENT 0x0066BBDDu
#define COL_DIM    0x00708090u
#define COL_ERROR  0x00EF5350u
#define COL_SUCCESS 0x0066BB6Au
#define COL_BTN    0x00225588u

static struct tk_window win;
static struct tk_widget *w_user;     /* username field   */
static struct tk_widget *w_status;   /* status / error   */
static int g_done;                   /* set on successful sign-in */

static void do_login(void) {
    const char *u = tk_get_text(w_user);
    if (!u || !u[0]) {
        tk_colors(w_status, 0, COL_ERROR);
        tk_set_text(&win, w_status, "Please enter a username");
        return;
    }
    if (sys_login(u) != 0) {
        tk_colors(w_status, 0, COL_ERROR);
        tk_set_text(&win, w_status, "Login failed - invalid username");
        return;
    }
    tk_colors(w_status, 0, COL_SUCCESS);
    tk_set_text(&win, w_status, "Welcome! Loading desktop...");
    g_done = 1;
}

static void on_signin(struct tk_window *w, struct tk_widget *b) { (void)w; (void)b; do_login(); }

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 1024, 724, "") != 0)
        return 1;
    tk_maximize(&win);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 0);
    root->gap = 0;
    tk_colors(root, 0x000A0E14, 0);

    /* vertically centred row holding the fixed-width sign-in panel */
    tk_grow(tk_label(&win, root, ""), 1);
    struct tk_widget *row = tk_hbox(&win, root, 0);
    tk_grow(tk_label(&win, row, ""), 1);

    struct tk_widget *panel = tk_vbox(&win, row, 8);
    tk_size(panel, 380, 0);
    tk_pad(panel, 30);
    tk_colors(panel, COL_PANEL, 0);

    tk_align(tk_bold(tk_font(tk_colors(tk_label(&win, panel, "TobyOS"), 0, COL_ACCENT), 30)),
             TK_ALIGN_CENTER);
    tk_align(tk_colors(tk_label(&win, panel, "Sign in to continue"), 0, COL_DIM), TK_ALIGN_CENTER);
    tk_label(&win, panel, "");
    tk_colors(tk_label(&win, panel, "Username"), 0, COL_DIM);

    /* prefill from the last-user setting, else "toby" */
    char last[64];
    long n = sys_setting_get("user.last", last, sizeof(last));
    w_user = tk_field(&win, panel, (n > 0 && last[0]) ? last : "toby");
    w_user->on_click = on_signin;   /* Enter signs in */

    tk_colors(tk_button(&win, panel, "Sign In", on_signin), COL_BTN, 0x00E0E8F0);
    w_status = tk_align(tk_label(&win, panel, ""), TK_ALIGN_CENTER);

    tk_grow(tk_label(&win, row, ""), 1);
    tk_grow(tk_label(&win, root, ""), 1);
    tk_align(tk_colors(tk_label(&win, root, "Type username and press Enter or click Sign In"),
                       0, COL_DIM), TK_ALIGN_CENTER);

    /* focus the username field so typing works immediately */
    win.focus = w_user;
    w_user->focused = 1;

    /* session-gateway loop: never quit on CLOSE; only a successful login exits */
    for (;;) {
        int q = tk_pump(&win);
        if (g_done) break;
        if (q) win.want_quit = 0;
        usleep(15000);
    }

    /* smooth fade-out: 30 steps over ~600ms, revealing the desktop */
    for (int step = 0; step < 30; step++) {
        int alpha = 255 - (step * 255 / 30);
        if (alpha < 0) alpha = 0;
        sc2(SYS_GUI_SET_OPACITY, win.fd, alpha);
        sc2(SYS_GUI_FLIP, win.fd, 0);
        usleep(20000);
    }
    sc2(SYS_GUI_SET_OPACITY, win.fd, 0);
    sc2(SYS_GUI_FLIP, win.fd, 0);
    return 0;
}
