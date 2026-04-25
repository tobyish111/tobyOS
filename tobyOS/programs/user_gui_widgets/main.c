/* user_gui_widgets/main.c -- /bin/gui_widgets, the milestone-11 demo.
 *
 * Builds a single window containing:
 *
 *   +-----------------------------------------------+
 *   |  tobyOS widget demo                           |
 *   |                                               |
 *   |  Type your name:                              |
 *   |  +-----------------------------------------+  |  <- text input
 *   |  | bob_                                    |  |
 *   |  +-----------------------------------------+  |
 *   |                                               |
 *   |  +---------+   +---------+                    |
 *   |  | Greet   |   | Clear   |                    |  <- buttons
 *   |  +---------+   +---------+                    |
 *   |                                               |
 *   |  hello, bob!                                  |  <- label updated by Greet
 *   |                                               |
 *   +-----------------------------------------------+
 *
 * Behaviour:
 *   - clicking the text input gives it keyboard focus (yellow border +
 *     blinking caret while focused)
 *   - typing fills the input; backspace deletes the last char
 *   - clicking [Greet] reads the input and updates the bottom label
 *   - clicking [Clear] empties both the input and the label
 *
 * Ctrl+C from the shell tears the process down; the kernel auto-closes
 * the window via the FILE_KIND_WINDOW close path -- no exit handling
 * required from the app.
 */

#include "../common/toby_gui.h"

#define WIN_W 360
#define WIN_H 220

/* Forward decls because the callbacks need the widget pointers and
 * the widget pointers are produced after the callbacks are defined.
 * Stored as file-static handles for simplicity. */
static struct tg_widget *g_input;
static struct tg_widget *g_status;

/* ---- tiny formatting helpers (no libc available) ----------------- */

static unsigned tg_strlen_u(const char *s) {
    const char *p = s; while (*p) p++; return (unsigned)(p - s);
}

/* Concatenate up to three NUL-terminated strings into `out`, capped to
 * `cap` (writes a trailing NUL and stops short if needed). */
static void str3(char *out, unsigned cap,
                 const char *a, const char *b, const char *c) {
    unsigned i = 0;
    const char *parts[3] = { a, b, c };
    for (int p = 0; p < 3; p++) {
        const char *s = parts[p];
        if (!s) continue;
        while (*s && i + 1 < cap) out[i++] = *s++;
    }
    out[i] = '\0';
}

/* ---- button callbacks ------------------------------------------- */

static void on_greet_clicked(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    const char *name = tg_get_text(g_input);
    char msg[TG_TEXT_MAX];
    if (name[0]) {
        str3(msg, sizeof(msg), "hello, ", name, "!");
    } else {
        str3(msg, sizeof(msg), "hello, stranger!", 0, 0);
    }
    tg_set_text(app, g_status, msg);
}

static void on_clear_clicked(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    tg_set_text(app, g_input,  "");
    tg_set_text(app, g_status, "(cleared -- click Greet to try again)");
}

/* ---- main -------------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, WIN_W, WIN_H, "widgets demo") != 0) {
        tg_puts("gui_widgets: tg_app_init failed (no GUI? out of slots?)\n");
        return 1;
    }

    /* Title + prompt labels. */
    tg_label(&app,  12,   8, 320, 16, "tobyOS widget demo (milestone 11)");
    tg_label(&app,  12,  44, 200, 14, "Type your name:");

    /* Text input -- click it to focus, then type. */
    g_input = tg_textinput(&app, 12, 62, 336, 24);

    /* Two buttons. */
    tg_button(&app, 12,  98,  80, 28, "Greet", on_greet_clicked);
    tg_button(&app, 100, 98,  80, 28, "Clear", on_clear_clicked);

    /* Status label that the buttons update. */
    g_status = tg_label(&app, 12, 144, 336, 16, "(click Greet or type a name)");

    /* Footer hint. */
    tg_label(&app, 12, 184, 336, 14,
             "Click input -> type. Click Greet. Ctrl+C in shell quits.");

    return tg_run(&app);
}
