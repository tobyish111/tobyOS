/* toby/gui.hpp -- OPTIONAL header-only C++ RAII wrapper around the
 * tobyOS GUI toolkit. Demonstrates that the SDK's C surface is C++-
 * friendly without forcing anyone through C++ to use it; nothing in
 * the SDK build chain requires this header.
 *
 * Tradeoffs vs. the raw C API:
 *   - Window lifetime is tied to scope (toby::Window destructor calls
 *     tg_app_quit; widgets are stored as pointers into the underlying
 *     tg_app's static widget table, so they don't need destruction).
 *   - Buttons can take a std::function-equivalent via a small thunk
 *     trampoline; if you need <functional> you're free to add it
 *     yourself, but this header stays freestanding-clean.
 *
 * This is intentionally small (~120 lines). It exists so you can
 * write an SDK app in C++ if you want to, not so you have to. */

#ifndef TOBY_GUI_HPP
#define TOBY_GUI_HPP

#include <toby/gui.h>

namespace toby {

class Window;

/* Lightweight handle around a single tg_widget *. Pointers are stable
 * for the lifetime of the owning Window (the underlying widget table
 * is fixed-size inside tg_app, never reallocated). */
class Widget {
public:
    Widget() : w_(nullptr) {}
    explicit Widget(struct tg_widget *w) : w_(w) {}

    explicit operator bool() const { return w_ != nullptr; }
    struct tg_widget *handle() const { return w_; }

    const char *text() const { return w_ ? ::tg_get_text(w_) : ""; }

private:
    struct tg_widget *w_;
};

/* RAII Window. Declared on the stack inside main(); the destructor
 * calls tg_app_quit so the toolkit cleans up the next time tg_run
 * inspects want_quit. The kernel teardown of the underlying window fd
 * happens automatically when the process exits, so this destructor
 * has nothing else to do. */
class Window {
public:
    Window(int w, int h, const char *title) : ok_(false) {
        ok_ = (::tg_app_init(&app_, w, h, title) == 0);
    }

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    ~Window() {
        if (ok_) ::tg_app_quit(&app_);
    }

    explicit operator bool() const { return ok_; }

    Widget label(int x, int y, int w, int h, const char *text) {
        return Widget(::tg_label(&app_, x, y, w, h, text));
    }

    Widget button(int x, int y, int w, int h, const char *text,
                  tg_button_cb cb) {
        return Widget(::tg_button(&app_, x, y, w, h, text, cb));
    }

    Widget textinput(int x, int y, int w, int h) {
        return Widget(::tg_textinput(&app_, x, y, w, h));
    }

    void set_text(Widget w, const char *t) {
        ::tg_set_text(&app_, w.handle(), t);
    }

    void request_redraw()        { ::tg_request_redraw(&app_); }
    void quit()                  { ::tg_app_quit(&app_); }
    int  run()                   { return ::tg_run(&app_); }

    /* Escape hatch: hand the raw struct out for callers who need to
     * mix C-style code with the wrapper (most commonly: a button
     * callback that wants to update widgets via the C API). */
    struct tg_app *raw() { return &app_; }

private:
    struct tg_app app_;
    bool          ok_;
};

}  /* namespace toby */

#endif /* TOBY_GUI_HPP */
