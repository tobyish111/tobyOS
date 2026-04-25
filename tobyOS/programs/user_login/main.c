/* user_login/main.c -- /bin/login, the milestone-14 GUI login screen.
 *
 * Layout (380 x 200 client area):
 *
 *   +-----------------------------------------------+
 *   |  Sign in to tobyOS                            |
 *   |                                               |
 *   |  Welcome to the desktop. Type a username to   |
 *   |  start a session. (No password required.)     |
 *   |                                               |
 *   |  username:                                    |
 *   |  +-----------------------------------------+  |  <- text input,
 *   |  | toby_                                   |  |     pre-filled with
 *   |  +-----------------------------------------+  |     settings.user.last
 *   |                                               |
 *   |  +---------+   status: (waiting...)          |
 *   |  |  Login  |                                  |
 *   |  +---------+                                  |
 *   +-----------------------------------------------+
 *
 * Behaviour:
 *   - on launch, reads settings["user.last"] via SYS_SETTING_GET and
 *     pre-fills the text input.
 *   - clicking Login (or pressing Enter while the input is focused, via
 *     a dedicated focus on the button + Enter activation) calls
 *     SYS_LOGIN(name). On success the program exits cleanly; the kernel
 *     service manager sees /bin/login is dead and -- because the
 *     session is now active -- does NOT restart it. The user is now
 *     looking at the empty desktop with the taskbar; clicking [Apps]
 *     opens the launcher and any spawned app belongs to this session.
 *
 * The whole window stays modal-feeling because the desktop has no other
 * apps yet -- it's just wallpaper + taskbar behind us.
 */

#include "../common/toby_gui.h"

/* ---- syscall numbers (must match include/tobyos/syscall.h) ------- */

#define SYS_SETTING_GET   21
#define SYS_LOGIN         23

/* ---- syscall stubs ---------------------------------------------- */

static inline long sys_setting_get(const char *key, char *out, tg_size_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SETTING_GET), "D"(key), "S"(out), "d"(cap)
        : "rcx", "r11", "memory");
    return r;
}

static inline long sys_login(const char *username) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_LOGIN), "D"(username)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- app state -------------------------------------------------- */

#define WIN_W 380
#define WIN_H 220

static struct tg_widget *g_input;
static struct tg_widget *g_status;
static struct tg_app    *g_app;

/* ---- callbacks -------------------------------------------------- */

static void on_login_click(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    const char *name = tg_get_text(g_input);
    if (!name || !name[0]) {
        tg_set_text(app, g_status, "status: please enter a username.");
        return;
    }
    long rc = sys_login(name);
    if (rc != 0) {
        tg_set_text(app, g_status, "status: login refused (kernel said no).");
        return;
    }
    /* Show one last frame so the operator sees we accepted, then quit;
     * the trampoline calls SYS_EXIT and the kernel reaps us. The login
     * service won't be restarted because the session is now active. */
    tg_set_text(app, g_status, "status: signed in -- launching desktop...");
    tg_app_quit(app);
}

/* ---- main ------------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, WIN_W, WIN_H, "tobyOS login") != 0) {
        tg_puts("login: tg_app_init failed\n");
        return 1;
    }
    g_app = &app;

    tg_label(&app, 12,   8, 360, 16, "Sign in to tobyOS (milestone 15)");
    tg_label(&app, 12,  34, 360, 14, "Type a username from /data/users.");
    tg_label(&app, 12,  50, 360, 14, "Defaults: root (uid 0), toby (uid 1000), guest (1001).");

    tg_label(&app, 12,  80, 200, 14, "username:");
    g_input = tg_textinput(&app, 12, 98, 356, 24);

    /* Pre-fill from /data/settings.conf so repeat logins are one click. */
    char last[64];
    long n = sys_setting_get("user.last", last, sizeof(last));
    if (n > 0 && last[0]) {
        tg_set_text(&app, g_input, last);
    } else {
        tg_set_text(&app, g_input, "toby");
    }

    tg_button(&app, 12, 138, 100, 28, "Login", on_login_click);
    g_status = tg_label(&app, 124, 144, 244, 16, "status: ready.");

    tg_label(&app, 12, 184, 360, 14,
             "Click the input, type a name, then press Login.");

    return tg_run(&app);
}
