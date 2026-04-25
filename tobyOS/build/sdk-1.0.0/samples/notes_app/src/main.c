/* notes_app/src/main.c -- a small but real GUI sample.
 *
 * A one-line "sticky note" with Save / Load / Clear buttons that
 * persists to /data/apps/notes_app/note.txt. Demonstrates, in 130
 * lines:
 *
 *   - tg_app_init / tg_label / tg_button / tg_textinput
 *   - file I/O via libtoby (open/read/write/close) into /data/
 *   - structured logging via tobylog_write
 *   - desktop notifications via toby_notify_post (M31)
 *
 * The persistence path lives under the package's install prefix so
 * `pkg remove notes_app` cleans it up automatically.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include <toby/gui.h>
#include <tobyos_notify.h>
#include <tobyos_slog.h>

#define NOTE_PATH  "/data/apps/notes_app/note.txt"
#define NOTE_MAX   (TG_TEXT_MAX - 1)   /* matches textinput buffer cap */

static struct tg_widget *g_input;
static struct tg_widget *g_status;

/* ---- helpers ---- */

static void set_status(struct tg_app *app, const char *fmt, ...) {
    char buf[TG_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tg_set_text(app, g_status, buf);
}

/* Best-effort mkdir; the package's install prefix is /data/apps/notes_app
 * so the directory should already exist after `pkg install`, but the
 * sample stays usable when run from a hand-built ELF too. */
static void ensure_dir(const char *path) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return;
    *slash = '\0';
    /* mkdir is idempotent in libtoby's wrapper -- failing because the
     * directory exists is silently swallowed. */
    (void)mkdir(buf, 0755);
}

/* ---- button callbacks ---- */

static void on_save_clicked(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    const char *txt = tg_get_text(g_input);
    size_t      n   = strlen(txt);

    ensure_dir(NOTE_PATH);
    int fd = open(NOTE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        set_status(app, "Save failed: %s", strerror(errno));
        tobylog_write(2 /* WARN */, "notes", "save failed");
        return;
    }
    ssize_t w = write(fd, txt, n);
    close(fd);
    if (w < 0 || (size_t)w != n) {
        set_status(app, "Short write to note.txt");
        return;
    }
    set_status(app, "Saved %zu bytes", n);
    tobylog_write(0 /* INFO */, "notes", "note saved");
    /* Friendly toast -- the kernel will pop it the next gui_tick. */
    (void)toby_notify_post(TOBY_NOTIFY_INFO, "Note saved",
                           "notes_app wrote /data/apps/notes_app/note.txt");
}

static void on_load_clicked(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    int fd = open(NOTE_PATH, O_RDONLY);
    if (fd < 0) {
        set_status(app, "No saved note (%s)", strerror(errno));
        return;
    }
    char buf[NOTE_MAX + 1];
    ssize_t r = read(fd, buf, NOTE_MAX);
    close(fd);
    if (r < 0) {
        set_status(app, "Read failed: %s", strerror(errno));
        return;
    }
    buf[r] = '\0';
    tg_set_text(app, g_input, buf);
    set_status(app, "Loaded %zd bytes", r);
}

static void on_clear_clicked(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    tg_set_text(app, g_input, "");
    set_status(app, "Cleared (use Save to persist empty)");
}

static void on_quit_clicked(struct tg_widget *btn, struct tg_app *app) {
    (void)btn;
    tg_app_quit(app);
}

/* ---- main ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct tg_app app;
    if (tg_app_init(&app, 360, 200, "Notes") != 0) {
        fprintf(stderr, "[notes_app] tg_app_init failed\n");
        return 1;
    }

    tg_label    (&app,  16,  16, 328, 18, "Type a note:");
    g_input   = tg_textinput(&app, 16,  40, 328, 28);

    tg_button(&app,  16,  84,  72, 28, "Save",  on_save_clicked);
    tg_button(&app,  96,  84,  72, 28, "Load",  on_load_clicked);
    tg_button(&app, 176,  84,  72, 28, "Clear", on_clear_clicked);
    tg_button(&app, 272,  84,  72, 28, "Quit",  on_quit_clicked);

    g_status = tg_label(&app, 16, 130, 328, 18,
                        "Ready. Notes persist in /data/apps/notes_app/.");

    return tg_run(&app);
}
