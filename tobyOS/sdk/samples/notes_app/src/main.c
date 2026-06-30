/* notes_app/src/main.c -- a small but real GUI sample.
 *
 * A one-line "sticky note" with Save / Load / Clear buttons that persists to
 * /data/apps/notes_app/note.txt. Demonstrates, on the TobyTK toolkit:
 *
 *   - tk_window_open / tk_label / tk_button / tk_field (the widget toolkit)
 *   - file I/O via libtoby (open/read/write/close) into /data/
 *   - structured logging via tobylog_write
 *   - desktop notifications via toby_notify_post
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

#include <toby/tk.h>
#include <tobyos_notify.h>
#include <tobyos_slog.h>

#define NOTE_PATH  "/data/apps/notes_app/note.txt"
#define NOTE_MAX   (TK_TEXT_MAX - 1)   /* matches the field's buffer cap */

static struct tk_window  win;          /* ~150 KB: must be static, not on the stack */
static struct tk_widget *g_input;
static struct tk_widget *g_status;

/* ---- helpers ---- */

static void set_status(const char *fmt, ...) {
    char buf[TK_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tk_set_text(&win, g_status, buf);
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
    (void)mkdir(buf, 0755);
}

/* ---- button callbacks (TobyTK: cb(win, widget)) ---- */

static void on_save(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    const char *txt = tk_get_text(g_input);
    size_t      n   = strlen(txt);

    ensure_dir(NOTE_PATH);
    int fd = open(NOTE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        set_status("Save failed: %s", strerror(errno));
        tobylog_write(2 /* WARN */, "notes", "save failed");
        return;
    }
    ssize_t wr = write(fd, txt, n);
    close(fd);
    if (wr < 0 || (size_t)wr != n) {
        set_status("Short write to note.txt");
        return;
    }
    set_status("Saved %zu bytes", n);
    tobylog_write(0 /* INFO */, "notes", "note saved");
    (void)toby_notify_post(TOBY_NOTIFY_INFO, "Note saved",
                           "notes_app wrote /data/apps/notes_app/note.txt");
}

static void on_load(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    int fd = open(NOTE_PATH, O_RDONLY);
    if (fd < 0) {
        set_status("No saved note (%s)", strerror(errno));
        return;
    }
    char buf[NOTE_MAX + 1];
    ssize_t r = read(fd, buf, NOTE_MAX);
    close(fd);
    if (r < 0) {
        set_status("Read failed: %s", strerror(errno));
        return;
    }
    buf[r] = '\0';
    tk_set_text(&win, g_input, buf);
    set_status("Loaded %zd bytes", r);
}

static void on_clear(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    tk_set_text(&win, g_input, "");
    set_status("Cleared (use Save to persist empty)");
}

static void on_quit(struct tk_window *w, struct tk_widget *b) {
    (void)b;
    tk_quit(w);
}

/* ---- main ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 380, 200, "Notes") != 0) {
        fprintf(stderr, "[notes_app] tk_window_open failed\n");
        return 1;
    }

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 16);
    root->gap = 8;

    tk_label(&win, root, "Type a note:");
    g_input = tk_field(&win, root, "");

    struct tk_widget *row = tk_hbox(&win, root, 8);
    tk_button(&win, row, "Save",  on_save);
    tk_button(&win, row, "Load",  on_load);
    tk_button(&win, row, "Clear", on_clear);
    tk_grow(tk_label(&win, row, ""), 1);
    tk_button(&win, row, "Quit",  on_quit);

    g_status = tk_label(&win, root, "Ready. Notes persist in /data/apps/notes_app/.");

    return tk_run(&win);
}
