# GUI toolkit reference

The SDK GUI toolkit (`<toby/gui.h>`, link `libtoby_gui.a`) is a small
immediate-style framework for building windowed applications on top
of the kernel's GUI syscalls.

## Programming model

```c
struct tg_app app;
tg_app_init(&app, w, h, title);   /* open a window */

tg_label  (&app, x, y, w, h, "...");                /* add widgets */
tg_button (&app, x, y, w, h, "Quit", on_quit);
tg_textinput(&app, x, y, w, h);

return tg_run(&app);              /* main loop -- never returns
                                     until on_quit calls tg_app_quit */
```

`tg_run` polls events, dispatches them to the focused / captured
widget, and re-paints the window when `want_redraw` is set. There is
**no async model** -- every callback runs on the main thread, between
poll cycles.

## Widgets

| Function | Visual | Focusable | Notes |
|---|---|---|---|
| `tg_label`     | static text | no  | `tg_set_text` updates it |
| `tg_button`    | clickable label | yes | callback fires on release-inside |
| `tg_textinput` | one-line text field | yes | edits in place, `tg_get_text` to read |

Geometry is **absolute pixels** in the client area (i.e. inside the
title-bar). There is no layout engine. For most useful apps, hand-
positioned widgets are fine -- the toolkit is intentionally small.

## Events

```c
struct tg_event {
    int        type;     /* TG_EV_NONE/MOUSE_MOVE/MOUSE_DOWN/MOUSE_UP/KEY */
    int        x, y;
    uint8_t    button;   /* mouse button on MOUSE_* events */
    uint8_t    key;      /* ASCII for printable, special codes for others */
};
```

Most apps don't need to look at events directly -- you wire a button
callback (`tg_button_cb`) and let the toolkit call you back. If you
want raw event access, `tg_app::want_redraw`/`want_quit` flags let
you replace `tg_run` with a hand-rolled loop calling
`sys_gui_poll_event` directly.

## Theming

Window chrome (title-bar, border, taskbar) is painted by the kernel
compositor using the active `theme_palette` (see M31 docs). The
toolkit's widget colours track the same palette, so a switch between
the `cyber` and `basic` themes is automatically reflected in widget
backgrounds.

You cannot override colours from a widget today. Adding per-widget
styling is on the M33+ roadmap.

## C++ wrapper

`<toby/gui.hpp>` ships an optional, header-only RAII wrapper:

```cpp
#include <toby/gui.hpp>

void on_quit(struct tg_widget *, struct tg_app *app) {
    tg_app_quit(app);
}

int main() {
    toby::Window win(320, 140, "Hello");
    if (!win) return 1;
    win.label (16, 20, 288, 24, "Hello, C++!");
    win.button(232, 90,  72, 28, "Quit", on_quit);
    return win.run();
}
```

The wrapper is freestanding-clean: no exceptions, no RTTI, no STL.
Compile your `.cpp` with `clang++ -std=c++17 -fno-exceptions -fno-rtti`
plus the same flags the C templates use.

## Examples

See `samples/hello_gui/src/main.c` and `samples/notes_app/src/main.c`
for working starting points.
