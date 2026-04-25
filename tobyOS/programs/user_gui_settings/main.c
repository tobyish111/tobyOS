/* user_gui_settings/main.c -- /bin/gui_settings, the milestone-14
 * persistent settings editor.
 *
 * Lets the user view and modify a small set of system settings backed
 * by /data/settings.conf:
 *
 *   desktop.bg       0xAARRGGBB wallpaper colour. Live: the next time
 *                    the compositor repaints the desktop (every
 *                    gui_tick), it pulls this through settings_get_u32.
 *   desktop.title    short string painted by the compositor.
 *   user.greeting    string the login screen / shell uses for "hello".
 *
 * Every change is written through SYS_SETTING_SET, which the kernel
 * persists immediately via settings_save(). So a reboot keeps the
 * change.
 *
 * The window also has a "Logout" button that calls SYS_LOGOUT -- handy
 * for demoing the session lifecycle without diving into the launcher.
 *
 * Layout (420 x 280):
 *
 *   +------------------------------------------+
 *   |  System Settings                         |
 *   |  (changes are saved to /data/settings.conf)
 *   |                                          |
 *   |  desktop.bg (0xAARRGGBB):                |
 *   |  [ 0x00204060                          ] |
 *   |                                          |
 *   |  desktop.title:                          |
 *   |  [ tobyOS                              ] |
 *   |                                          |
 *   |  user.greeting:                          |
 *   |  [ Hello, friend!                      ] |
 *   |                                          |
 *   |  [ Apply ]  [ Reset ]  [ Logout ]        |
 *   |                                          |
 *   |  status: ready.                          |
 *   +------------------------------------------+
 */

#include "../common/toby_gui.h"

/* ---- syscall numbers (must match include/tobyos/syscall.h) ------- */

#define SYS_SETTING_GET   21
#define SYS_SETTING_SET   22
#define SYS_LOGOUT        24

/* ---- syscall stubs ---------------------------------------------- */

static inline long sys_setting_get(const char *key, char *out, tg_size_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SETTING_GET), "D"(key), "S"(out), "d"(cap)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_setting_set(const char *key, const char *val) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SETTING_SET), "D"(key), "S"(val)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_logout(void) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_LOGOUT)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- baked-in defaults (mirror src/settings.c) ------------------ */

#define DEF_BG       "0x00204060"
#define DEF_TITLE    "tobyOS"
#define DEF_GREETING "Hello, friend!"

/* ---- widget handles --------------------------------------------- */

#define WIN_W 440
#define WIN_H 300

static struct tg_widget *g_in_bg;
static struct tg_widget *g_in_title;
static struct tg_widget *g_in_greet;
static struct tg_widget *g_status;

/* ---- helpers ---------------------------------------------------- */

static void load_into(struct tg_app *app, struct tg_widget *w,
                      const char *key, const char *def) {
    char buf[64];
    long n = sys_setting_get(key, buf, sizeof(buf));
    if (n <= 0 || !buf[0]) {
        tg_set_text(app, w, def);
    } else {
        tg_set_text(app, w, buf);
    }
}

/* ---- callbacks -------------------------------------------------- */

static void on_apply(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    long e1 = sys_setting_set("desktop.bg",     tg_get_text(g_in_bg));
    long e2 = sys_setting_set("desktop.title",  tg_get_text(g_in_title));
    long e3 = sys_setting_set("user.greeting",  tg_get_text(g_in_greet));
    if (e1 || e2 || e3) {
        tg_set_text(app, g_status,
                    "status: kernel rejected one or more values.");
    } else {
        tg_set_text(app, g_status,
                    "status: applied + saved to /data/settings.conf.");
    }
}

static void on_reset(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    tg_set_text(app, g_in_bg,    DEF_BG);
    tg_set_text(app, g_in_title, DEF_TITLE);
    tg_set_text(app, g_in_greet, DEF_GREETING);
    tg_set_text(app, g_status,
                "status: reset to defaults (click Apply to persist).");
}

static void on_logout(struct tg_widget *btn, struct tg_app *app) {
    (void)btn; (void)app;
    /* SYS_LOGOUT SIGTERMs every process tagged with the active session,
     * including this one. We won't get a chance to draw a final frame --
     * the compositor will be back to "no apps" + login window in a few
     * ticks. */
    sys_logout();
}

/* ---- main ------------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, WIN_W, WIN_H, "tobyOS settings") != 0) {
        tg_puts("gui_settings: tg_app_init failed\n");
        return 1;
    }

    tg_label(&app, 12,   8, 416, 16, "System Settings (milestone 14)");
    tg_label(&app, 12,  26, 416, 14,
             "Changes are persisted to /data/settings.conf.");

    /* desktop.bg */
    tg_label(&app, 12,  56, 200, 14, "desktop.bg (0xAARRGGBB):");
    g_in_bg = tg_textinput(&app, 12, 74, 416, 22);

    /* desktop.title */
    tg_label(&app, 12, 110, 200, 14, "desktop.title:");
    g_in_title = tg_textinput(&app, 12, 128, 416, 22);

    /* user.greeting */
    tg_label(&app, 12, 164, 200, 14, "user.greeting:");
    g_in_greet = tg_textinput(&app, 12, 182, 416, 22);

    /* Buttons */
    tg_button(&app,  12, 220,  90, 28, "Apply",  on_apply);
    tg_button(&app, 110, 220,  90, 28, "Reset",  on_reset);
    tg_button(&app, 208, 220,  90, 28, "Logout", on_logout);

    /* Status */
    g_status = tg_label(&app, 12, 260, 416, 16, "status: ready.");

    /* Pre-populate from the kernel cache. */
    load_into(&app, g_in_bg,    "desktop.bg",    DEF_BG);
    load_into(&app, g_in_title, "desktop.title", DEF_TITLE);
    load_into(&app, g_in_greet, "user.greeting", DEF_GREETING);

    return tg_run(&app);
}
