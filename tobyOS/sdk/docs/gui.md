# GUI toolkit reference (TobyTK)

The SDK GUI toolkit is **TobyTK** (`<toby/tk.h>`). It ships *inside* `libtoby.a`,
so a GUI app links exactly the same libs as a CLI app (`crt0.o` + `libtoby.a`)
and the linker drops the widget code from apps that don't call it. There is no
separate GUI archive.

TobyTK is a small **retained-mode** toolkit: you build a tree of widgets once,
then run an event loop that hit-tests the tree, fires callbacks, and repaints
only when something changes. Layout is automatic (vbox/hbox + flex), so you set
sizes/relationships rather than absolute pixel rectangles.

## Programming model

```c
#include <toby/tk.h>

static struct tk_window win;   /* ~150 KB: keep it static, never on the stack */

static void on_quit(struct tk_window *w, struct tk_widget *btn) {
    (void)btn;
    tk_quit(w);
}

int main(void) {
    if (tk_window_open(&win, 320, 160, "My App") != 0) return 1;

    struct tk_widget *root = tk_root(&win);   /* the root is a vbox */
    tk_pad(root, 16);
    root->gap = 8;

    tk_label(&win, root, "Hello!");
    struct tk_widget *row = tk_hbox(&win, root, 8);
    tk_grow(tk_label(&win, row, ""), 1);      /* spacer pushes the button right */
    tk_button(&win, row, "Quit", on_quit);

    return tk_run(&win);                       /* blocks until tk_quit() */
}
```

`tk_run` polls events, dispatches them to the focused / captured widget,
re-paints when dirty, and **self-paces** (sleeps when idle — never a busy
loop). Every callback runs on the main thread between poll cycles; there is no
async model. A callback has the signature `void cb(struct tk_window *w,
struct tk_widget *self)`.

## Layout

Containers lay their children out along one axis:

- `tk_vbox(win, parent, gap)` / `tk_hbox(win, parent, gap)` — stack children
  vertically / horizontally. The root window is a vbox.
- `tk_pad(w, px)` — inner padding; `tk_size(w, fw, fh)` — fixed size (0 = auto);
  `tk_grow(w, flex)` — flex-grow weight along the parent's main axis (use a
  grown empty label as a spacer); `tk_align(w, TK_ALIGN_*)`, `tk_font(w, px)`,
  `tk_bold(w)`, `tk_colors(w, bg, fg)` (0 = theme default).

`parent == NULL` attaches to the root.

## Widgets

| Constructor | Notes |
|---|---|
| `tk_label`     | static text; `tk_set_text` updates it |
| `tk_button`    | fires `on_click(win, self)` |
| `tk_field`     | single-line editable text; `tk_get_text` / `tk_set_text`; Enter fires `on_click` |
| `tk_checkbox`  | `tk_checked` / `tk_set_checked` |
| `tk_listbox`   | scrollable single-select; `on_change`; `tk_selected`. **The items array you pass must outlive the widget — use a static/global array, not a stack local.** |
| `tk_slider` / `tk_progress` | `value`/min/max; slider fires `on_change` |
| `tk_separator` | thin rule |
| `tk_table`     | columns + scrollable rows pulled lazily via an app cell-accessor (no slot per row); `tk_table_rows`, `tk_table_selected`. Same array-lifetime rule for headers/widths. |
| `tk_textarea`  | scrollable read-only multiline |
| `tk_canvas`    | custom-draw escape hatch: register `on_paint`/`on_event` and draw with `tk_draw_fill/rect/rrect/line/circle/gradient/text/blit` + `tk_draw_text_mono` (column-aligned 8×16 bitmap text for terminals/editors) |

Window-level keyboard hook: `tk_on_key(&win, cb)` — receives every key (for
accelerators / app-driven keyboards), replacing the default focus routing.

## Updating the UI

Use the runtime accessors (`tk_set_text`, `tk_set_value`, …) for in-place
changes. To swap a whole page of widgets (e.g. a settings tab), use the
dynamic-rebuild trio:

```c
int base = tk_checkpoint(&win);     /* after building the static chrome */
/* ... */
tk_clear_children(&win, content);   /* detach the old page */
tk_rewind(&win, base);              /* free its pool slots */
/* ... rebuild into `content` ... */
tk_redraw(&win);
```

## Building & linking

```
clang ... -isystem $(TOBYOS_SDK)/include -c src/main.c -o build/main.o
ld.lld ... -T $(TOBYOS_SDK)/lib/program.ld -o app.elf \
    $(TOBYOS_SDK)/lib/crt0.o build/main.o $(TOBYOS_SDK)/lib/libtoby.a
```

See `samples/hello_gui/` (minimal) and `samples/notes_app/` (a real app with a
text field + file I/O) for working examples.
